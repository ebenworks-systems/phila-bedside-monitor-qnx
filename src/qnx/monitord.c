/*
 * monitord.c - the bedside monitor.
 *
 *   intake      receives sensor messages (MsgReceive on the name_attach
 *               channel), scores each bed with NEWS2 and decides alarms;
 *               runs at the sending sensor's priority
 *   alarm       woken by a pulse; puts each alarm on the board the nurse
 *               display reads, logs it to slog2 and timestamps it
 *   supervisor  a 250 ms timer pulse; finds silent sensors and measures each
 *               thread's CPU time
 *   recorder    writes the append-only, hash-chained record under a
 *               sporadic-scheduling budget
 *   main        the resource manager behind the files in /dev/phila/
 *
 * Alarm latency is measured for every alarm onset, escalation and technical
 * alarm, from the deadline of the sensor reading that caused it to the
 * moment it is on the board. The samples go to a CSV file at the end.
 *
 *   phila-monitord [--scenario ward|probe] [--seed N] [--naive] [--duration S]
 *                  [--record FILE] [--latency-csv FILE] [--obs-every MS]
 *
 * Prototype for a hackathon, not a medical device, not for clinical use.
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/mman.h>
#include <sys/neutrino.h>
#include <sys/slog2.h>
#include <time.h>
#include <unistd.h>

#include "phila/beds.h"
#include "phila/record.h"
#include "phila/scenario.h"
#include "protocol.h"
#include "resmgr.h"
#include "rt.h"

#define EVQ_MAX 512
#define RECQ_MAX 16384
#define LAT_MAX 262144
#define HISTORY_MAX 24
#define THREADS_MAX 8

typedef struct {
    int64_t sched, sample, decided, raised;
    uint8_t bed, kind;
} lat_t;

typedef struct {
    int64_t t_ns;
    int bed;
    bed_event_kind_t kind;
    news2_risk_t level;
    char reason[BED_REASON_MAX];
    uint32_t latency_us;
} hist_t;

typedef struct {
    news2_risk_t level; /* NEWS2_RISK_LOW: no clinical alarm */
    char clinical[BED_REASON_MAX];
    int64_t since_ns;
    uint32_t latency_us;
    char tech[BED_REASON_MAX]; /* "" when none */
    int64_t tech_since_ns;
} board_t;

typedef struct {
    const char *name;
    pthread_t tid;
    int policy, prio;
    int64_t cpu_prev_ns, cpu_ns;
    double cpu_pct;
} thread_info_t;

static struct {
    /* options */
    scenario_t sc;
    int naive;
    double duration_s;
    const char *record_path;
    const char *latency_path;
    int64_t obs_every_ns;
    int64_t start_ns;

    /* ward state, alarm queue, board and history: under lock */
    pthread_mutex_t lock;
    ward_t ward;
    bed_event_t evq[EVQ_MAX];
    unsigned evq_head, evq_tail;
    uint32_t evq_dropped;
    board_t board[SCN_BEDS];
    hist_t hist[HISTORY_MAX];
    unsigned hist_n;
    int64_t last_obs_ns[SCN_BEDS];
    uint64_t readings;
    uint64_t missed;
    int intake_max_prio;

    /* record queue: under rec_lock */
    pthread_mutex_t rec_lock;
    pthread_cond_t rec_cv;
    rec_entry_t *recq;
    unsigned rec_head, rec_tail, rec_high;
    uint32_t rec_dropped;
    uint64_t rec_written;
    int rec_stop;
    pthread_t rec_tid;

    /* latency samples: appended by the alarm thread only */
    lat_t *lat;
    unsigned lat_n;

    /* threads: written by the supervisor */
    thread_info_t th[THREADS_MAX];
    int th_n;
    int recorder_sporadic;

    int alarm_chid, alarm_coid, alarm_prio;
    slog2_buffer_t slog;
    volatile sig_atomic_t stop_requested;
} M;

/* ------------------------------------------------------------------ */

static void mlog(int severity, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (M.slog != NULL) vslog2f(M.slog, 0, (uint8_t)severity, fmt, ap);
    va_end(ap);
}

static void lock_init(pthread_mutex_t *m)
{
    /* Priority inheritance: a low-priority thread that holds the lock is
     * raised to the priority of the highest thread waiting for it. */
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_setprotocol(&a, PTHREAD_PRIO_INHERIT);
    pthread_mutex_init(m, &a);
    pthread_mutexattr_destroy(&a);
}

static void evq_push(const bed_event_t *e)
{
    unsigned next = (M.evq_tail + 1) % EVQ_MAX;
    if (next == M.evq_head) {
        M.evq_dropped++;
        return;
    }
    M.evq[M.evq_tail] = *e;
    M.evq_tail = next;
}

static void rec_push(const rec_entry_t *e)
{
    pthread_mutex_lock(&M.rec_lock);
    unsigned next = (M.rec_tail + 1) % RECQ_MAX;
    if (next == M.rec_head) {
        M.rec_dropped++;
    } else {
        M.recq[M.rec_tail] = *e;
        M.rec_tail = next;
        unsigned used = (M.rec_tail + RECQ_MAX - M.rec_head) % RECQ_MAX;
        if (used > M.rec_high) M.rec_high = used;
        pthread_cond_signal(&M.rec_cv);
    }
    pthread_mutex_unlock(&M.rec_lock);
}

static void rec_event(const char *bed, const char *fmt, ...)
{
    rec_entry_t e;
    memset(&e, 0, sizeof e);
    e.kind = REC_EVENT;
    e.mono_ns = rt_now_ns();
    e.wall_ms = rt_wall_ms();
    snprintf(e.bed, sizeof e.bed, "%s", bed);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e.text, sizeof e.text, fmt, ap);
    va_end(ap);
    for (char *p = e.text; *p; p++) /* keep the record's text rule: printable, no quotes */
        if (*p < 0x20 || *p > 0x7e || *p == '"' || *p == '\\') *p = '_';
    rec_push(&e);
}

static void register_thread(const char *name, pthread_t tid, int policy, int prio)
{
    if (M.th_n >= THREADS_MAX) return;
    thread_info_t *t = &M.th[M.th_n++];
    t->name = name;
    t->tid = tid;
    t->policy = policy;
    t->prio = prio;
}

/* ------------------------------------------------------------------ */
/* intake                                                              */

typedef union {
    uint16_t type;
    struct _pulse pulse;
    phila_reading_msg_t reading;
    char pad[512];
} intake_msg_t;

static void *intake_main(void *arg)
{
    name_attach_t *att = arg;
    intake_msg_t msg;
    bed_event_t ev[16];
    rec_entry_t obs[SCN_BEDS];

    for (;;) {
        int rcvid = MsgReceive(att->chid, &msg, sizeof msg, NULL);
        if (rcvid == -1) continue;
        if (rcvid == 0) {
            if (msg.pulse.code == _PULSE_CODE_DISCONNECT) ConnectDetach(msg.pulse.scoid);
            continue;
        }
        if (msg.type == _IO_CONNECT) { /* from name_open() */
            MsgReply(rcvid, EOK, NULL, 0);
            continue;
        }
        if (msg.type != PHILA_MSG_READING || msg.reading.sensor >= SENSOR_COUNT) {
            MsgError(rcvid, ENOSYS);
            continue;
        }
        /* While it serves this message the thread runs at the sender's
         * priority, not its own base priority of 10. */
        int prio = rt_current_priority();
        const phila_reading_msg_t *r = &msg.reading;
        sensor_t s = (sensor_t)r->sensor;
        int raised = 0, nobs = 0;

        pthread_mutex_lock(&M.lock);
        if (prio > M.intake_max_prio) M.intake_max_prio = prio;
        M.readings++;
        M.missed += r->missed;
        for (int b = 0; b < SCN_BEDS; b++) {
            int n = ward_reading(&M.ward, b, s, r->v[b], r->t_sched_ns, r->t_sample_ns, 0, ev, 16);
            int64_t decided = rt_now_ns();
            for (int i = 0; i < n && i < 16; i++) {
                ev[i].t_decided_ns = decided;
                evq_push(&ev[i]);
                raised++;
            }
            const bed_t *bed = &M.ward.bed[b];
            if (s == SENSOR_PULSE_OX && bed->complete && decided - M.last_obs_ns[b] >= M.obs_every_ns) {
                M.last_obs_ns[b] = decided;
                rec_entry_t *e = &obs[nobs++];
                memset(e, 0, sizeof *e);
                e->kind = REC_OBS;
                e->mono_ns = r->t_sample_ns;
                e->wall_ms = rt_wall_ms();
                snprintf(e->bed, sizeof e->bed, "%s", scn_bed_id(b));
                ward_obs(&M.ward, b, &e->obs);
                e->res = bed->res;
            }
        }
        pthread_mutex_unlock(&M.lock);

        if (raised > 0) MsgSendPulse(M.alarm_coid, M.alarm_prio, PULSE_ALARM, raised);
        MsgReply(rcvid, EOK, NULL, 0);
        for (int i = 0; i < nobs; i++) rec_push(&obs[i]);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* alarm                                                               */

/* Put one event on the board. Called with M.lock held. */
static void board_apply(const bed_event_t *e, int64_t now)
{
    board_t *bd = &M.board[e->bed];
    switch (e->kind) {
    case BEV_ALARM_ON:
    case BEV_ALARM_UP:
        bd->level = e->level;
        snprintf(bd->clinical, sizeof bd->clinical, "%s", e->reason);
        if (e->kind == BEV_ALARM_ON) bd->since_ns = now;
        bd->latency_us = (uint32_t)((now - e->t_sched_ns) / 1000);
        break;
    case BEV_ALARM_CLEAR:
        bd->level = NEWS2_RISK_LOW;
        bd->clinical[0] = '\0';
        break;
    case BEV_TECH_ON:
        snprintf(bd->tech, sizeof bd->tech, "%s", e->reason);
        bd->tech_since_ns = now;
        break;
    case BEV_TECH_CLEAR:
        bd->tech[0] = '\0';
        break;
    }
    hist_t *h = &M.hist[M.hist_n++ % HISTORY_MAX];
    h->t_ns = now;
    h->bed = e->bed;
    h->kind = e->kind;
    h->level = e->level;
    snprintf(h->reason, sizeof h->reason, "%s", e->reason);
    h->latency_us = (uint32_t)((now - e->t_sched_ns) / 1000);
}

static int slog_severity(const bed_event_t *e)
{
    if (e->kind == BEV_ALARM_CLEAR || e->kind == BEV_TECH_CLEAR) return SLOG2_INFO;
    if (e->kind == BEV_TECH_ON) return SLOG2_WARNING;
    return e->level == NEWS2_RISK_HIGH ? SLOG2_CRITICAL : SLOG2_ERROR;
}

static void *alarm_main(void *arg)
{
    (void)arg;
    bed_event_t ev[64];
    for (;;) {
        struct _pulse p;
        if (MsgReceivePulse(M.alarm_chid, &p, sizeof p, NULL) == -1 || p.code != PULSE_ALARM) continue;
        for (;;) {
            int n = 0;
            pthread_mutex_lock(&M.lock);
            while (n < 64 && M.evq_head != M.evq_tail) {
                ev[n++] = M.evq[M.evq_head];
                M.evq_head = (M.evq_head + 1) % EVQ_MAX;
            }
            int64_t now = rt_now_ns();
            for (int i = 0; i < n; i++) board_apply(&ev[i], now);
            int64_t raised = rt_now_ns(); /* the alarms are on the board */
            pthread_mutex_unlock(&M.lock);
            if (n == 0) break;

            for (int i = 0; i < n; i++) {
                const bed_event_t *e = &ev[i];
                uint32_t lat_us = (uint32_t)((raised - e->t_sched_ns) / 1000);
                bool measured = e->kind == BEV_ALARM_ON || e->kind == BEV_ALARM_UP || e->kind == BEV_TECH_ON;
                if (measured && M.lat_n < LAT_MAX) {
                    lat_t *l = &M.lat[M.lat_n];
                    l->sched = e->t_sched_ns;
                    l->sample = e->t_sample_ns;
                    l->decided = e->t_decided_ns;
                    l->raised = raised;
                    l->bed = (uint8_t)e->bed;
                    l->kind = (uint8_t)e->kind;
                    __atomic_store_n(&M.lat_n, M.lat_n + 1, __ATOMIC_RELEASE);
                }
                mlog(slog_severity(e), "%s %s %s %s latency_us=%u", scn_bed_id(e->bed), bed_event_name(e->kind),
                     news2_risk_name(e->level), e->reason, lat_us);

                rec_entry_t r;
                memset(&r, 0, sizeof r);
                r.mono_ns = raised;
                r.wall_ms = rt_wall_ms();
                snprintf(r.bed, sizeof r.bed, "%s", scn_bed_id(e->bed));
                if (measured) {
                    r.kind = REC_ALARM;
                    r.level = e->level;
                    r.technical = e->kind == BEV_TECH_ON;
                    snprintf(r.reason, sizeof r.reason, "%s %s", bed_event_name(e->kind), e->reason);
                    r.latency_us = lat_us;
                } else {
                    r.kind = REC_EVENT;
                    snprintf(r.text, sizeof r.text, "%s %s", bed_event_name(e->kind), e->reason);
                }
                rec_push(&r);
            }
        }
    }
    return NULL;
}

static void wake_alarm(int n)
{
    if (n > 0) MsgSendPulse(M.alarm_coid, M.alarm_prio, PULSE_ALARM, n);
}

/* ------------------------------------------------------------------ */
/* recorder                                                            */

static void *recorder_main(void *arg)
{
    int fd = *(int *)arg;
    rec_chain_t chain;
    rec_chain_init(&chain);
    static rec_entry_t batch[64];
    char line[REC_LINE_MAX];
    int64_t last_sync = rt_now_ns();

    for (;;) {
        int n = 0, stop = 0;
        pthread_mutex_lock(&M.rec_lock);
        if (M.rec_head == M.rec_tail && !M.rec_stop) { /* wait for work, at most a second */
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            ts.tv_sec += 1;
            pthread_cond_timedwait(&M.rec_cv, &M.rec_lock, &ts);
        }
        while (n < 64 && M.rec_head != M.rec_tail) {
            batch[n++] = M.recq[M.rec_head];
            M.rec_head = (M.rec_head + 1) % RECQ_MAX;
        }
        stop = M.rec_stop && M.rec_head == M.rec_tail;
        pthread_mutex_unlock(&M.rec_lock);

        int alarm = 0;
        for (int i = 0; i < n; i++) {
            int len = rec_format(&chain, &batch[i], line, sizeof line);
            if (len < 0) {
                mlog(SLOG2_ERROR, "record: entry for %s refused", batch[i].bed);
                continue;
            }
            if (write(fd, line, (size_t)len) != len) mlog(SLOG2_ERROR, "record: write failed: %s", strerror(errno));
            M.rec_written++;
            if (batch[i].kind == REC_ALARM) alarm = 1;
        }
        int64_t now = rt_now_ns();
        if ((alarm || now - last_sync >= 1000000000LL) && n > 0) {
            fsync(fd); /* an alarm is on disk before the next batch */
            last_sync = now;
        }
        if (stop) break;
    }
    fsync(fd);
    close(fd);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* supervisor                                                          */

static int cmp_i64(const void *a, const void *b)
{
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

static void write_latency_csv(void)
{
    if (M.latency_path == NULL) return;
    FILE *f = fopen(M.latency_path, "w");
    if (f == NULL) {
        fprintf(stderr, "phila-monitord: %s: %s\n", M.latency_path, strerror(errno));
        return;
    }
    fprintf(f, "kind,bed,sched_ns,sample_ns,decided_ns,raised_ns,total_us\n");
    unsigned n = __atomic_load_n(&M.lat_n, __ATOMIC_ACQUIRE);
    for (unsigned i = 0; i < n; i++) {
        const lat_t *l = &M.lat[i];
        fprintf(f, "%s,%s,%lld,%lld,%lld,%lld,%.3f\n", bed_event_name((bed_event_kind_t)l->kind), scn_bed_id(l->bed),
                (long long)l->sched, (long long)l->sample, (long long)l->decided, (long long)l->raised,
                (double)(l->raised - l->sched) / 1000.0);
    }
    fclose(f);
}

/* Which latency samples and which span of each. */
typedef enum { LAT_CLINICAL, LAT_TECHNICAL } lat_class_t;
typedef enum { SPAN_TOTAL, SPAN_WAKE, SPAN_PIPELINE } lat_span_t;

/* Fills min, median, p99, p99.9 and max in microseconds; returns the count.
 * TOTAL is deadline to alarm on the board; WAKE is deadline to the sensor
 * thread running; PIPELINE is the sensor thread running to the board. */
static unsigned latency_summary(lat_class_t cls, lat_span_t span, double out[5])
{
    unsigned n = __atomic_load_n(&M.lat_n, __ATOMIC_ACQUIRE), k = 0;
    memset(out, 0, 5 * sizeof out[0]);
    if (n == 0) return 0;
    int64_t *v = malloc(n * sizeof *v);
    if (v == NULL) return 0;
    for (unsigned i = 0; i < n; i++) {
        const lat_t *l = &M.lat[i];
        bool tech = l->kind == BEV_TECH_ON;
        if (tech != (cls == LAT_TECHNICAL)) continue;
        v[k++] = span == SPAN_WAKE ? l->sample - l->sched : span == SPAN_PIPELINE ? l->raised - l->sample : l->raised - l->sched;
    }
    if (k > 0) {
        qsort(v, k, sizeof *v, cmp_i64);
        out[0] = (double)v[0] / 1000.0;
        out[1] = (double)v[(k - 1) / 2] / 1000.0;
        out[2] = (double)v[(unsigned)((k - 1) * 0.99)] / 1000.0;
        out[3] = (double)v[(unsigned)((k - 1) * 0.999)] / 1000.0;
        out[4] = (double)v[k - 1] / 1000.0;
    }
    free(v);
    return k;
}

static void finish(void)
{
    rec_event("WARD", "monitor stopped after %.1f s", (double)(rt_now_ns() - M.start_ns) / 1e9);
    pthread_mutex_lock(&M.rec_lock);
    M.rec_stop = 1;
    pthread_cond_signal(&M.rec_cv);
    pthread_mutex_unlock(&M.rec_lock);
    pthread_join(M.rec_tid, NULL);
    write_latency_csv();

    double s[5], w[5], p[5], t[5];
    unsigned n = latency_summary(LAT_CLINICAL, SPAN_TOTAL, s);
    latency_summary(LAT_CLINICAL, SPAN_WAKE, w);
    latency_summary(LAT_CLINICAL, SPAN_PIPELINE, p);
    unsigned nt = latency_summary(LAT_TECHNICAL, SPAN_TOTAL, t);
    printf("phila-monitord: %llu readings, %llu missed sensor deadlines, %llu record lines\n",
           (unsigned long long)M.readings, (unsigned long long)M.missed, (unsigned long long)M.rec_written);
    printf("phila-monitord: clinical alarm latency over %u alarms (us): min %.1f, median %.1f, p99 %.1f, p99.9 %.1f, "
           "max %.1f\n", n, s[0], s[1], s[2], s[3], s[4]);
    printf("phila-monitord:   of which timer wake: median %.1f, max %.1f; alarm pipeline: median %.1f, max %.1f\n", w[1],
           w[4], p[1], p[4]);
    printf("phila-monitord: technical alarms %u, max %.1f us\n", nt, t[4]);
    printf("phila-monitord: intake ran at priority %d at most; dropped alarm events %u, dropped record lines %u\n",
           M.intake_max_prio, M.evq_dropped, M.rec_dropped);
    mlog(SLOG2_INFO, "stopped: %u clinical alarms, max latency %.1f us", n, s[4]);
    fflush(stdout);
    _exit(0);
}

static void sample_cpu(void)
{
    static int64_t prev_wall = 0;
    int64_t wall = rt_now_ns();
    for (int i = 0; i < M.th_n; i++) {
        clockid_t cid;
        struct timespec ts;
        if (pthread_getcpuclockid(M.th[i].tid, &cid) != 0 || clock_gettime(cid, &ts) != 0) continue;
        int64_t cpu = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
        if (prev_wall != 0 && wall > prev_wall)
            M.th[i].cpu_pct = 100.0 * (double)(cpu - M.th[i].cpu_prev_ns) / (double)(wall - prev_wall);
        M.th[i].cpu_prev_ns = cpu;
        M.th[i].cpu_ns = cpu;
    }
    prev_wall = wall;
}

static void *supervisor_main(void *arg)
{
    (void)arg;
    int prio = M.naive ? PRIO_NAIVE : PRIO_SUPERVISOR;
    int chid = ChannelCreate(_NTO_CHF_PRIVATE);
    int self = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    struct sigevent sev;
    SIGEV_PULSE_INIT(&sev, self, prio, PULSE_SUPERVISE, 0);
    timer_t timer;
    struct itimerspec its;
    if (chid == -1 || self == -1 || timer_create(CLOCK_MONOTONIC, &sev, &timer) == -1) {
        perror("phila-monitord: supervisor timer");
        return NULL;
    }
    rt_ts(&its.it_value, 250000000LL);
    rt_ts(&its.it_interval, 250000000LL);
    timer_settime(timer, 0, &its, NULL);

    bed_event_t ev[32];
    for (unsigned tick = 1;; tick++) {
        struct _pulse p;
        if (MsgReceivePulse(chid, &p, sizeof p, NULL) == -1 || p.code != PULSE_SUPERVISE) continue;
        int64_t now = rt_now_ns();
        pthread_mutex_lock(&M.lock);
        int n = ward_check_silence(&M.ward, now, ev, 32);
        for (int i = 0; i < n && i < 32; i++) {
            ev[i].t_decided_ns = rt_now_ns();
            evq_push(&ev[i]);
        }
        pthread_mutex_unlock(&M.lock);
        wake_alarm(n);
        if (tick % 4 == 0) sample_cpu();
        if (M.stop_requested || (M.duration_s > 0 && now - M.start_ns >= (int64_t)(M.duration_s * 1e9))) finish();
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* /dev/phila                                                          */

typedef struct {
    char *buf;
    size_t len, cap;
} text_t;

static void put(text_t *t, const char *fmt, ...)
{
    for (;;) {
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(t->buf ? t->buf + t->len : NULL, t->buf ? t->cap - t->len : 0, fmt, ap);
        va_end(ap);
        if (n < 0) return;
        if (t->buf != NULL && t->len + (size_t)n < t->cap) {
            t->len += (size_t)n;
            return;
        }
        size_t cap = t->cap ? t->cap * 2 : 4096;
        while (cap < t->len + (size_t)n + 1) cap *= 2;
        char *nb = realloc(t->buf, cap);
        if (nb == NULL) return;
        t->buf = nb;
        t->cap = cap;
    }
}

static void put_val(text_t *t, const char *key, const bed_t *bed, sensor_t s, int ch)
{
    if (bed->have[s])
        put(t, " %s=%d", key, bed->val[s][ch]);
    else
        put(t, " %s=-", key);
}

static char *render(pfile_t which, size_t *len)
{
    text_t t = {NULL, 0, 0};
    int64_t now = rt_now_ns();
    double up = (double)(now - M.start_ns) / 1e9;
    put(&t, "%s", ""); /* allocate even when empty */

    if (which == PFILE_STATUS) {
        pthread_mutex_lock(&M.lock);
        put(&t, "monitor scenario=%s mode=%s uptime_s=%.1f readings=%llu missed=%llu\n", scn_name(M.sc.id),
            M.naive ? "naive" : "designed", up, (unsigned long long)M.readings, (unsigned long long)M.missed);
        for (int b = 0; b < SCN_BEDS; b++) {
            const bed_t *bed = &M.ward.bed[b];
            const board_t *bd = &M.board[b];
            put(&t, "bed=%s", scn_bed_id(b));
            put_val(&t, "hr", bed, SENSOR_PULSE_OX, 1);
            put_val(&t, "spo2", bed, SENSOR_PULSE_OX, 0);
            put_val(&t, "rr", bed, SENSOR_RESP, 0);
            put_val(&t, "sbp", bed, SENSOR_BP, 0);
            put_val(&t, "temp", bed, SENSOR_TEMP, 0);
            put(&t, " acvpu=%s o2=%d scale=%d news2=%u risk=%s complete=%d alarm=%s acked=%d tech=%s\n",
                news2_acvpu_letter(bed->acvpu), bed->on_oxygen ? 1 : 0, (int)bed->scale, (unsigned)bed->res.total,
                news2_risk_name(bed->res.risk), bed->complete ? 1 : 0,
                bd->level == NEWS2_RISK_LOW ? "none" : news2_risk_name(bd->level), bed->acked ? 1 : 0,
                bd->tech[0] ? bd->tech : "none");
        }
        pthread_mutex_unlock(&M.lock);
    } else if (which == PFILE_ALARMS) {
        pthread_mutex_lock(&M.lock);
        put(&t, "board uptime_s=%.1f\n", up);
        for (int b = 0; b < SCN_BEDS; b++) {
            const board_t *bd = &M.board[b];
            if (bd->level != NEWS2_RISK_LOW)
                put(&t, "active bed=%s level=%s for_s=%.1f latency_us=%u acked=%d reason=%s\n", scn_bed_id(b),
                    news2_risk_name(bd->level), (double)(now - bd->since_ns) / 1e9, bd->latency_us,
                    M.ward.bed[b].acked ? 1 : 0, bd->clinical);
            if (bd->tech[0])
                put(&t, "active bed=%s level=technical for_s=%.1f reason=%s\n", scn_bed_id(b),
                    (double)(now - bd->tech_since_ns) / 1e9, bd->tech);
        }
        unsigned n = M.hist_n < HISTORY_MAX ? M.hist_n : HISTORY_MAX;
        for (unsigned i = 0; i < n; i++) {
            const hist_t *h = &M.hist[(M.hist_n - 1 - i) % HISTORY_MAX];
            put(&t, "event t_s=%.1f bed=%s kind=%s level=%s latency_us=%u reason=%s\n",
                (double)(h->t_ns - M.start_ns) / 1e9, scn_bed_id(h->bed), bed_event_name(h->kind),
                news2_risk_name(h->level), h->latency_us, h->reason);
        }
        pthread_mutex_unlock(&M.lock);
    } else if (which == PFILE_LATENCY) {
        static const char *const SPANS[3] = {"total", "timer_wake", "pipeline"};
        put(&t, "alarm latency in microseconds; total = deadline of the sensor reading to the alarm on the board\n");
        for (int span = 0; span < 3; span++) {
            double s[5];
            unsigned n = latency_summary(LAT_CLINICAL, (lat_span_t)span, s);
            put(&t, "clinical %-10s alarms=%u min=%.1f median=%.1f p99=%.1f p99.9=%.1f max=%.1f\n", SPANS[span], n,
                s[0], s[1], s[2], s[3], s[4]);
        }
        double s[5];
        unsigned n = latency_summary(LAT_TECHNICAL, SPAN_TOTAL, s);
        put(&t, "technical total      alarms=%u min=%.1f median=%.1f max=%.1f\n", n, s[0], s[1], s[4]);
        put(&t, "intake_max_priority=%d missed_deadlines=%llu\n", M.intake_max_prio, (unsigned long long)M.missed);
        pthread_mutex_lock(&M.rec_lock);
        put(&t, "record_lines=%llu record_queue_high_water=%u record_dropped=%u alarm_events_dropped=%u\n",
            (unsigned long long)M.rec_written, M.rec_high, M.rec_dropped, M.evq_dropped);
        pthread_mutex_unlock(&M.rec_lock);
    } else if (which == PFILE_THREADS) {
        put(&t, "%-12s %-9s %4s %7s %9s\n", "thread", "policy", "prio", "cpu_%", "cpu_ms");
        for (int i = 0; i < M.th_n; i++) {
            const thread_info_t *th = &M.th[i];
            put(&t, "%-12s %-9s %4d %7.2f %9.1f\n", th->name, rt_policy_name(th->policy), th->prio, th->cpu_pct,
                (double)th->cpu_ns / 1e6);
        }
    }
    *len = t.len;
    return t.buf;
}

static int control(const char *line)
{
    char cmd[16], bedid[REC_BED_MAX], arg[16] = "";
    if (sscanf(line, "%15s %15s %15s", cmd, bedid, arg) < 2) return EINVAL;
    int b = ward_bed_index(bedid);
    if (b < 0) return ENOENT;
    bed_event_t ev[8];
    int n = 0;
    int64_t now = rt_now_ns();

    pthread_mutex_lock(&M.lock);
    if (strcmp(cmd, "ack") == 0) {
        n = ward_ack(&M.ward, b) ? 0 : -1;
    } else if (strcmp(cmd, "acvpu") == 0) {
        static const char letters[] = "ACVPU";
        const char *p = arg[0] && !arg[1] ? strchr(letters, arg[0]) : NULL;
        n = p ? ward_set_acvpu(&M.ward, b, (acvpu_t)(p - letters), now, ev, 8) : -1;
    } else if (strcmp(cmd, "oxygen") == 0) {
        bool on = strcmp(arg, "on") == 0;
        n = (on || strcmp(arg, "off") == 0) ? ward_set_oxygen(&M.ward, b, on, now, ev, 8) : -1;
    } else if (strcmp(cmd, "scale") == 0) {
        n = (strcmp(arg, "1") == 0 || strcmp(arg, "2") == 0)
                ? ward_set_scale(&M.ward, b, arg[0] == '2' ? SPO2_SCALE_2 : SPO2_SCALE_1, now, ev, 8)
                : -1;
    } else {
        n = -1;
    }
    for (int i = 0; i < n && i < 8; i++) {
        ev[i].t_decided_ns = rt_now_ns();
        evq_push(&ev[i]);
    }
    pthread_mutex_unlock(&M.lock);
    if (n < 0) return EINVAL;
    wake_alarm(n);
    rec_event(scn_bed_id(b), "nurse: %s", line);
    mlog(SLOG2_INFO, "control: %s", line);
    return EOK;
}

/* ------------------------------------------------------------------ */

static void on_signal(int sig)
{
    (void)sig;
    M.stop_requested = 1;
}

static void usage(void)
{
    fprintf(stderr, "usage: phila-monitord [--scenario ward|probe] [--seed N] [--naive] [--duration S]\n"
                    "                      [--record FILE] [--latency-csv FILE] [--obs-every MS]\n");
    exit(2);
}

static void die(const char *what)
{
    fprintf(stderr, "phila-monitord: %s: %s\n", what, strerror(errno));
    exit(1);
}

int main(int argc, char **argv)
{
    M.sc.id = SCN_WARD;
    M.sc.seed = 2026;
    M.record_path = "/data/phila/record.jsonl";
    M.obs_every_ns = 5000000000LL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--scenario") == 0 && i + 1 < argc) {
            if (!scn_parse(argv[++i], &M.sc.id)) usage();
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            M.sc.seed = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--naive") == 0) {
            M.naive = 1;
        } else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            M.duration_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "--record") == 0 && i + 1 < argc) {
            M.record_path = argv[++i];
        } else if (strcmp(argv[i], "--latency-csv") == 0 && i + 1 < argc) {
            M.latency_path = argv[++i];
        } else if (strcmp(argv[i], "--obs-every") == 0 && i + 1 < argc) {
            M.obs_every_ns = (int64_t)atoi(argv[++i]) * 1000000LL;
        } else {
            usage();
        }
    }

    /* Keep every page in memory so no page fault lands on the alarm path. */
    if (mlockall(MCL_CURRENT | MCL_FUTURE) == -1) perror("phila-monitord: mlockall (continuing)");

    M.recq = calloc(RECQ_MAX, sizeof *M.recq);
    M.lat = calloc(LAT_MAX, sizeof *M.lat);
    if (M.recq == NULL || M.lat == NULL) die("memory");
    lock_init(&M.lock);
    lock_init(&M.rec_lock);
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&M.rec_cv, &ca);

    /* The record is append-only: a run never reopens or truncates an old file. */
    int fd = open(M.record_path, O_WRONLY | O_CREAT | O_EXCL | O_APPEND, 0644);
    if (fd == -1) die(M.record_path);

    slog2_buffer_set_config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.buffer_set_name = "phila-monitor";
    cfg.num_buffers = 1;
    cfg.verbosity_level = SLOG2_INFO;
    cfg.buffer_config[0].buffer_name = "alarms";
    cfg.buffer_config[0].num_pages = 8;
    if (slog2_register(&cfg, &M.slog, 0) == -1) {
        perror("phila-monitord: slog2_register (continuing without slog2)");
        M.slog = NULL;
    }

    M.start_ns = rt_now_ns();
    ward_init(&M.ward, &M.sc, M.start_ns);
    M.alarm_prio = M.naive ? PRIO_NAIVE : PRIO_ALARM;
    if ((M.alarm_chid = ChannelCreate(_NTO_CHF_PRIVATE)) == -1) die("alarm channel");
    if ((M.alarm_coid = ConnectAttach(0, 0, M.alarm_chid, _NTO_SIDE_CHANNEL, 0)) == -1) die("alarm connection");

    name_attach_t *att = name_attach(NULL, PHILA_MON_NAME, 0);
    if (att == NULL) die("name_attach");
    if (phila_resmgr_attach(render, control) == -1) die("resmgr_attach /dev/phila");

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    const int rr = SCHED_RR, naive = M.naive;
    pthread_t tid;
    int rc;
    if ((rc = rt_thread(&tid, "alarm", alarm_main, NULL, naive ? rr : SCHED_FIFO, M.alarm_prio, NULL)) != 0)
        errno = rc, die("alarm thread");
    register_thread("alarm", tid, naive ? rr : SCHED_FIFO, M.alarm_prio);

    if ((rc = rt_thread(&tid, "supervisor", supervisor_main, NULL, naive ? rr : SCHED_FIFO,
                        naive ? PRIO_NAIVE : PRIO_SUPERVISOR, NULL)) != 0)
        errno = rc, die("supervisor thread");
    register_thread("supervisor", tid, naive ? rr : SCHED_FIFO, naive ? PRIO_NAIVE : PRIO_SUPERVISOR);

    static int rec_fd;
    rec_fd = fd;
    if (naive) {
        rc = rt_thread(&M.rec_tid, "recorder", recorder_main, &rec_fd, rr, PRIO_NAIVE, NULL);
        register_thread("recorder", M.rec_tid, rr, PRIO_NAIVE);
    } else {
        /* 10 ms of CPU in every 100 ms at priority 30; past that, priority 5. */
        rt_sporadic_t ss = {PRIO_RECORDER_LOW, 8, 100000000LL, 10000000LL};
        rc = rt_thread(&M.rec_tid, "recorder", recorder_main, &rec_fd, SCHED_SPORADIC, PRIO_RECORDER, &ss);
        if (rc == 0) {
            M.recorder_sporadic = 1;
            register_thread("recorder", M.rec_tid, SCHED_SPORADIC, PRIO_RECORDER);
        } else {
            fprintf(stderr, "phila-monitord: sporadic scheduling refused (%s); recorder runs FIFO %d\n",
                    strerror(rc), PRIO_RECORDER);
            rc = rt_thread(&M.rec_tid, "recorder", recorder_main, &rec_fd, SCHED_FIFO, PRIO_RECORDER, NULL);
            register_thread("recorder", M.rec_tid, SCHED_FIFO, PRIO_RECORDER);
        }
    }
    if (rc != 0) errno = rc, die("recorder thread");

    if ((rc = rt_thread(&tid, "intake", intake_main, att, rr, PRIO_NAIVE, NULL)) != 0) errno = rc, die("intake thread");
    register_thread("intake", tid, rr, PRIO_NAIVE);

    rt_self(rr, PRIO_NAIVE);
    pthread_setname_np(pthread_self(), "resmgr");
    register_thread("resmgr", pthread_self(), rr, PRIO_NAIVE);

    rec_event("WARD", "monitor started scenario=%s seed=%u mode=%s", scn_name(M.sc.id), (unsigned)M.sc.seed,
              naive ? "naive" : "designed");
    mlog(SLOG2_INFO, "started: scenario %s, %s priorities, recorder %s", scn_name(M.sc.id),
         naive ? "naive" : "designed", M.recorder_sporadic ? "sporadic" : "not sporadic");
    printf("phila-monitord: running; scenario %s, %s priorities; /dev/phila/{status,alarms,latency,threads,control}\n",
           scn_name(M.sc.id), naive ? "naive" : "designed");
    fflush(stdout);

    phila_resmgr_run();
    return 0;
}

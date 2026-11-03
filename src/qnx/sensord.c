/*
 * sensord.c - simulated bedside sensors.
 *
 * One thread per sensor channel (pulse oximeter, respiration, blood
 * pressure, temperature). Each thread owns a private channel and a one-shot
 * timer, armed at one deadline at a time on the grid start + k * period,
 * that delivers a pulse to it; on every pulse the thread reads its channel
 * for all four beds from the scenario, sends one message to the monitor
 * with MsgSend() and arms the timer at the next deadline still ahead. The
 * message carries two times: when the reading was due (the timer deadline)
 * and when the thread actually ran, so the monitor can measure the whole
 * path from deadline to alarm.
 *
 *   phila-sensord [--scenario ward|probe] [--seed N] [--duration SECONDS] [--naive] [--tick-timers]
 *
 * --naive runs at the default priority with default (tick-aligned) timers;
 * --tick-timers keeps the designed priorities but uses tick-aligned timers.
 *
 * Prototype for a hackathon, not a medical device, not for clinical use.
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <time.h>
#include <unistd.h>

#include "phila/deadline.h"
#include "phila/scenario.h"
#include "protocol.h"
#include "rt.h"

static scenario_t g_sc = {SCN_WARD, 2026};
static int g_naive = 0;
static int64_t g_start_ns;   /* deadline of reading 0, the same for every sensor */
static int64_t g_end_ns;     /* 0 = run until stopped */
static int g_coid = -1;      /* connection to the monitor */
static int g_hires = 1;      /* high-resolution timers (off with --naive or --tick-timers) */

typedef struct {
    sensor_t sensor;
    uint32_t sent, missed, failed;
    int hires;
    int64_t readings; /* readings taken, whether or not the send worked */
} sensor_ctx_t;

/* Arm the one-shot timer at an absolute time; it_interval stays zero. */
static int arm(timer_t timer, int64_t at_ns)
{
    struct itimerspec its;
    memset(&its, 0, sizeof its);
    rt_ts(&its.it_value, at_ns);
    return timer_settime(timer, TIMER_ABSTIME, &its, NULL);
}

static void *sensor_main(void *arg)
{
    sensor_ctx_t *ctx = arg;
    const int prio = g_naive ? PRIO_NAIVE : PRIO_SENSOR;
    const int64_t period = (int64_t)scn_period_ms(&g_sc, ctx->sensor) * 1000000LL;

    int chid = ChannelCreate(_NTO_CHF_PRIVATE);
    int self = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    if (chid == -1 || self == -1) {
        perror("phila-sensord: channel");
        return NULL;
    }

    /* A one-shot timer whose expiry is a pulse at the sensor's priority. */
    struct sigevent ev;
    SIGEV_PULSE_INIT(&ev, self, prio, PULSE_TICK, ctx->sensor);
    timer_t timer;
    if (timer_create(CLOCK_MONOTONIC, &ev, &timer) == -1) {
        perror("phila-sensord: timer_create");
        return NULL;
    }
    /* QNX 8.0 is tickless. By default a timer still expires on the 1 ms
     * system tick; a tolerance below the tick asks for a high-resolution
     * timer that fires at its deadline (needs the HIGH_RESOLUTION_TIMER
     * ability, which root has). */
    if (g_hires) {
        struct itimerspec tol;
        memset(&tol, 0, sizeof tol);
        tol.it_value.tv_nsec = 1;
        if (timer_settime(timer, TIMER_TOLERANCE, &tol, NULL) == -1) {
            fprintf(stderr, "phila-sensord: high-resolution timer refused (%s); using the tick\n", strerror(errno));
            ctx->hires = 0;
        } else {
            ctx->hires = 1;
        }
    }
    /* One deadline at a time. A periodic timer (it_interval set) kept the
     * late phase of a host stall of hundreds of milliseconds for the rest of
     * the run (seen on 4 Nov 2026); arming each deadline anew at its own
     * absolute time puts the next expiry back on the grid whatever a stall
     * did. No early allowance: the kernel expires a timer only once
     * CLOCK_MONOTONIC, the clock rt_now_ns() reads, has reached it, and a
     * tolerance only lets it fire later; in the clean runs of 4 Nov no
     * reading came before its deadline (earliest 6.1 us after it with
     * high-resolution timers, 570.1 us with tick timers). A pulse before its
     * deadline would be no reading, and the timer is armed for it again. */
    deadline_t dl;
    dl_init(&dl, g_start_ns, period, 0);
    if (arm(timer, dl_at(&dl, dl.armed)) == -1) {
        perror("phila-sensord: timer_settime");
        return NULL;
    }

    phila_reading_msg_t msg;
    memset(&msg, 0, sizeof msg);
    msg.type = PHILA_MSG_READING;
    msg.sensor = (uint16_t)ctx->sensor;
    uint32_t carry = 0; /* missed deadlines of a reading whose send failed */

    for (;;) {
        struct _pulse p;
        if (MsgReceivePulse(chid, &p, sizeof p, NULL) == -1) continue;
        if (p.code != PULSE_TICK) continue;
        int64_t now = rt_now_ns();
        if (g_end_ns != 0 && now >= g_end_ns) break;

        /* The pulse is for the armed deadline. If the thread was held up
         * past further deadlines, one reading stands for the latest one
         * passed, its delay still counts from the armed deadline, and the
         * deadlines between are missed. */
        deadline_reading_t r;
        if (!dl_pulse(&dl, now, &r)) {
            if (arm(timer, dl_at(&dl, dl.armed)) == -1) {
                perror("phila-sensord: timer_settime");
                break;
            }
            continue;
        }
        int64_t k = r.latest;
        msg.missed = r.missed + carry;
        ctx->missed += r.missed;
        carry = 0;

        msg.t_sched_ns = dl_at(&dl, r.served);
        msg.t_sample_ns = now;
        /* The ward follows the clock. The probe alternates on every reading
         * actually taken, so a reading after any number of missed deadlines
         * is still a fresh alarm onset and its delay is measured. */
        int64_t step = g_sc.id == SCN_PROBE ? ctx->readings : k;
        ctx->readings++;
        int64_t t_ms = step * (period / 1000000LL);
        for (int b = 0; b < SCN_BEDS; b++) {
            scn_value_t v = scn_read(&g_sc, b, ctx->sensor, t_ms);
            msg.v[b][0] = v.v[0];
            msg.v[b][1] = v.v[1];
        }
        msg.seq = (uint32_t)k;
        if (MsgSend(g_coid, &msg, sizeof msg, NULL, 0) == -1) {
            ctx->failed++;
            carry += msg.missed; /* the monitor did not hear of them */
            if (errno == EBADF || errno == ESRCH) {
                fprintf(stderr, "phila-sensord: the monitor has gone (%s)\n", strerror(errno));
                break;
            }
        } else {
            ctx->sent++;
        }

        /* Armed after the send, so the reading's own path is the same as
         * before. A next deadline that passed during the send fires at once
         * and is served late, with its delay measured. */
        dl_advance(&dl, &r);
        if (arm(timer, dl_at(&dl, dl.armed)) == -1) {
            perror("phila-sensord: timer_settime");
            break;
        }
    }
    timer_delete(timer);
    return NULL;
}

static void usage(void)
{
    fprintf(stderr, "usage: phila-sensord [--scenario ward|probe] [--seed N] [--duration SECONDS] [--naive] "
                    "[--tick-timers]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    double duration = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--scenario") == 0 && i + 1 < argc) {
            if (!scn_parse(argv[++i], &g_sc.id)) usage();
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            g_sc.seed = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            duration = atof(argv[++i]);
        } else if (strcmp(argv[i], "--naive") == 0) {
            g_naive = 1;
            g_hires = 0;
        } else if (strcmp(argv[i], "--tick-timers") == 0) {
            g_hires = 0;
        } else {
            usage();
        }
    }

    /* Wait up to ten seconds for the monitor to register its name. */
    for (int tries = 0; tries < 100 && (g_coid = name_open(PHILA_MON_NAME, 0)) == -1; tries++) usleep(100000);
    if (g_coid == -1) {
        fprintf(stderr, "phila-sensord: cannot find %s: %s\n", PHILA_MON_NAME, strerror(errno));
        return 1;
    }

    /* Start on a whole 100 ms boundary half a second from now, so every
     * sensor's deadlines line up. */
    int64_t now = rt_now_ns();
    g_start_ns = ((now + 500000000LL) / 100000000LL) * 100000000LL;
    g_end_ns = duration > 0 ? g_start_ns + (int64_t)(duration * 1e9) : 0;

    sensor_ctx_t ctx[SENSOR_COUNT];
    pthread_t tid[SENSOR_COUNT];
    for (int s = 0; s < SENSOR_COUNT; s++) {
        memset(&ctx[s], 0, sizeof ctx[s]);
        ctx[s].sensor = (sensor_t)s;
        char name[24];
        snprintf(name, sizeof name, "sensor-%s", scn_sensor_name((sensor_t)s));
        int rc = rt_thread(&tid[s], name, sensor_main, &ctx[s], g_naive ? SCHED_RR : SCHED_FIFO,
                           g_naive ? PRIO_NAIVE : PRIO_SENSOR, NULL);
        if (rc != 0) {
            fprintf(stderr, "phila-sensord: thread for %s: %s\n", scn_sensor_name((sensor_t)s), strerror(rc));
            return 1;
        }
    }
    printf("phila-sensord: scenario %s, seed %u, %s priorities, %s timers, 4 beds\n", scn_name(g_sc.id),
           (unsigned)g_sc.seed, g_naive ? "naive" : "designed", g_hires ? "high-resolution" : "tick-aligned");
    fflush(stdout);
    for (int s = 0; s < SENSOR_COUNT; s++) pthread_join(tid[s], NULL);
    for (int s = 0; s < SENSOR_COUNT; s++)
        printf("phila-sensord: %-8s sent %u, missed deadlines %u, failed sends %u, %s timer\n",
               scn_sensor_name((sensor_t)s), ctx[s].sent, ctx[s].missed, ctx[s].failed,
               ctx[s].hires ? "high-resolution" : "tick-aligned");
    name_close(g_coid);
    return 0;
}

/*
 * beds.c - the state of each bed and the alarm decision. See beds.h.
 * Prototype for a hackathon, not a medical device, not for clinical use.
 * SPDX-License-Identifier: MIT
 */
#include "phila/beds.h"

#include <stdio.h>
#include <string.h>

#define SILENT_PERIODS 3

void ward_init(ward_t *w, const scenario_t *sc, int64_t now_ns)
{
    memset(w, 0, sizeof *w);
    for (int s = 0; s < SENSOR_COUNT; s++) w->period_ns[s] = (int64_t)scn_period_ms(sc, (sensor_t)s) * 1000000;
    for (int b = 0; b < SCN_BEDS; b++) {
        bed_t *bed = &w->bed[b];
        scn_bed_defaults(sc, b, &bed->scale, &bed->on_oxygen, &bed->acvpu);
        for (int s = 0; s < SENSOR_COUNT; s++) bed->last_ns[s] = now_ns;
        bed->alarm_level = NEWS2_RISK_LOW;
    }
}

static bool in_range(sensor_t s, const int16_t v[2])
{
    switch (s) {
    case SENSOR_PULSE_OX: return v[0] >= 0 && v[0] <= 100 && v[1] >= 0 && v[1] <= 300;
    case SENSOR_RESP: return v[0] >= 0 && v[0] <= 80;
    case SENSOR_BP: return v[0] >= 0 && v[0] <= 300;
    case SENSOR_TEMP: return v[0] >= 250 && v[0] <= 450;
    default: return false;
    }
}

void ward_obs(const ward_t *w, int b, news2_obs_t *o)
{
    const bed_t *bed = &w->bed[b];
    o->spo2_scale = bed->scale;
    o->on_oxygen = bed->on_oxygen;
    o->consciousness = bed->acvpu;
    /* Neutral values score 0 on every scale. */
    o->spo2 = bed->scale == SPO2_SCALE_2 ? 90 : 97;
    o->pulse = 70;
    o->resp_rate = 16;
    o->systolic = 120;
    o->temp_dc = 370;
    if (bed->have[SENSOR_PULSE_OX]) {
        o->spo2 = bed->val[SENSOR_PULSE_OX][0];
        o->pulse = bed->val[SENSOR_PULSE_OX][1];
    }
    if (bed->have[SENSOR_RESP]) o->resp_rate = bed->val[SENSOR_RESP][0];
    if (bed->have[SENSOR_BP]) o->systolic = bed->val[SENSOR_BP][0];
    if (bed->have[SENSOR_TEMP]) o->temp_dc = bed->val[SENSOR_TEMP][0];
}

static void add_red(char *buf, size_t cap, bool *first, const char *name)
{
    size_t n = strlen(buf);
    snprintf(buf + n, cap - n, "%s%s", *first ? " red=" : "+", name);
    *first = false;
}

static void reason_for(const bed_t *bed, char *buf, size_t cap)
{
    const news2_result_t *r = &bed->res;
    snprintf(buf, cap, "news2=%u", (unsigned)r->total);
    bool first = true;
    if (r->resp_rate == 3) add_red(buf, cap, &first, "rr");
    if (r->spo2 == 3) add_red(buf, cap, &first, "spo2");
    if (r->systolic == 3) add_red(buf, cap, &first, "sbp");
    if (r->pulse == 3) add_red(buf, cap, &first, "pulse");
    if (r->consciousness == 3) add_red(buf, cap, &first, "acvpu");
    if (r->temperature == 3) add_red(buf, cap, &first, "temp");
    if (!bed->complete) {
        size_t n = strlen(buf);
        snprintf(buf + n, cap - n, " incomplete");
    }
}

static int push(bed_event_t *ev, int cap, int n, const bed_event_t *e)
{
    if (n < cap) ev[n] = *e;
    return n + 1;
}

/* Score the bed from what it holds and compare with its alarm level. */
static int rescore(ward_t *w, int b, int64_t t_sched, int64_t t_sample, int64_t now, bed_event_t *ev, int cap, int n)
{
    bed_t *bed = &w->bed[b];
    news2_obs_t o;
    ward_obs(w, b, &o);
    if (news2_assess(&o, &bed->res) != NEWS2_OK) return n; /* cannot happen: inputs are range-checked */
    bed->complete = true;
    for (int s = 0; s < SENSOR_COUNT; s++)
        if (!bed->have[s]) bed->complete = false;

    news2_risk_t level = bed->res.risk;
    bed_event_t e;
    memset(&e, 0, sizeof e);
    e.bed = b;
    e.level = level;
    e.t_sched_ns = t_sched;
    e.t_sample_ns = t_sample;
    e.t_decided_ns = now;
    if (level > bed->alarm_level) {
        e.kind = bed->alarm_level == NEWS2_RISK_LOW ? BEV_ALARM_ON : BEV_ALARM_UP;
        reason_for(bed, e.reason, sizeof e.reason);
        bed->alarm_level = level;
        bed->acked = false;
        n = push(ev, cap, n, &e);
    } else if (level == NEWS2_RISK_LOW && bed->alarm_level != NEWS2_RISK_LOW) {
        e.kind = BEV_ALARM_CLEAR;
        reason_for(bed, e.reason, sizeof e.reason);
        bed->alarm_level = NEWS2_RISK_LOW;
        bed->acked = false;
        n = push(ev, cap, n, &e);
    } else if (level < bed->alarm_level) {
        /* Still above low but lower than before: keep alarming at the new
         * band, so a later rise counts as a fresh escalation. */
        bed->alarm_level = level;
    }
    return n;
}

static int tech(ward_t *w, int b, sensor_t s, bool on, const char *why, int64_t t_sched, int64_t t_sample,
                int64_t now, bed_event_t *ev, int cap, int n)
{
    bed_t *bed = &w->bed[b];
    bed_event_t e;
    memset(&e, 0, sizeof e);
    e.bed = b;
    e.kind = on ? BEV_TECH_ON : BEV_TECH_CLEAR;
    e.sensor = s;
    e.level = bed->res.risk;
    e.t_sched_ns = t_sched;
    e.t_sample_ns = t_sample;
    e.t_decided_ns = now;
    snprintf(e.reason, sizeof e.reason, "%s:%s", why, scn_sensor_name(s));
    bed->lost[s] = on;
    return push(ev, cap, n, &e);
}

int ward_reading(ward_t *w, int b, sensor_t s, const int16_t v[2], int64_t t_sched, int64_t t_sample, int64_t now,
                 bed_event_t *ev, int cap)
{
    if (w == NULL || b < 0 || b >= SCN_BEDS || s < 0 || s >= SENSOR_COUNT || v == NULL) return 0;
    bed_t *bed = &w->bed[b];
    int n = 0;
    bed->last_ns[s] = t_sample;
    bed->readings++;
    if (!in_range(s, v)) {
        bed->have[s] = false;
        if (!bed->lost[s]) n = tech(w, b, s, true, v[0] < 0 ? "no-reading" : "fault", t_sched, t_sample, now, ev, cap, n);
        return rescore(w, b, t_sched, t_sample, now, ev, cap, n);
    }
    bed->val[s][0] = v[0];
    bed->val[s][1] = s == SENSOR_PULSE_OX ? v[1] : 0;
    bed->have[s] = true;
    if (bed->lost[s]) n = tech(w, b, s, false, "reading", t_sched, t_sample, now, ev, cap, n);
    return rescore(w, b, t_sched, t_sample, now, ev, cap, n);
}

int ward_check_silence(ward_t *w, int64_t now, bed_event_t *ev, int cap)
{
    int n = 0;
    for (int b = 0; b < SCN_BEDS; b++) {
        bed_t *bed = &w->bed[b];
        for (int s = 0; s < SENSOR_COUNT; s++) {
            int64_t due = bed->last_ns[s] + SILENT_PERIODS * w->period_ns[s];
            if (bed->lost[s] || now < due) continue;
            bed->have[s] = false;
            n = tech(w, b, (sensor_t)s, true, "silent", due, now, now, ev, cap, n);
            n = rescore(w, b, due, now, now, ev, cap, n);
        }
    }
    return n;
}

int ward_set_acvpu(ward_t *w, int b, acvpu_t level, int64_t now, bed_event_t *ev, int cap)
{
    if (b < 0 || b >= SCN_BEDS || level < ACVPU_ALERT || level > ACVPU_UNRESPONSIVE) return -1;
    w->bed[b].acvpu = level;
    return rescore(w, b, now, now, now, ev, cap, 0);
}

int ward_set_oxygen(ward_t *w, int b, bool on_oxygen, int64_t now, bed_event_t *ev, int cap)
{
    if (b < 0 || b >= SCN_BEDS) return -1;
    w->bed[b].on_oxygen = on_oxygen;
    return rescore(w, b, now, now, now, ev, cap, 0);
}

int ward_set_scale(ward_t *w, int b, spo2_scale_t scale, int64_t now, bed_event_t *ev, int cap)
{
    if (b < 0 || b >= SCN_BEDS || (scale != SPO2_SCALE_1 && scale != SPO2_SCALE_2)) return -1;
    w->bed[b].scale = scale;
    return rescore(w, b, now, now, now, ev, cap, 0);
}

bool ward_ack(ward_t *w, int b)
{
    if (b < 0 || b >= SCN_BEDS || w->bed[b].alarm_level == NEWS2_RISK_LOW) return false;
    w->bed[b].acked = true;
    return true;
}

int ward_bed_index(const char *id)
{
    if (id == NULL) return -1;
    for (int b = 0; b < SCN_BEDS; b++)
        if (strcmp(id, scn_bed_id(b)) == 0) return b;
    return -1;
}

const char *bed_event_name(bed_event_kind_t k)
{
    switch (k) {
    case BEV_ALARM_ON: return "alarm";
    case BEV_ALARM_UP: return "escalate";
    case BEV_ALARM_CLEAR: return "clear";
    case BEV_TECH_ON: return "technical";
    case BEV_TECH_CLEAR: return "technical-clear";
    }
    return "?";
}

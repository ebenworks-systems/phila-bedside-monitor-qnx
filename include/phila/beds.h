/*
 * beds.h - the state of each bed and the alarm decision.
 *
 * Plain C with no operating-system calls, so the same decision runs in the
 * unit tests on a laptop and inside the QNX monitor. The caller holds the
 * lock and supplies every timestamp.
 *
 * Alarm rules (NEWS2 clinical response bands, Royal College of Physicians,
 * 2017):
 *   - a clinical alarm starts when the bed's band rises from low to
 *     low-medium (a single red score), medium (5 or 6) or high (7 or more),
 *     and is raised again whenever the band goes above the highest band
 *     this alarm has reached;
 *   - it holds that band until the bed falls back to low, then clears, so a
 *     score wobbling across a band edge does not repeat the alarm;
 *   - a sensor that stops reading cannot clear the alarm: while one is
 *     missing, the alarm clears only if the bed would still be low with that
 *     sensor's last good reading; if the alarm is held on such last
 *     readings alone and the readings present raise the score again, that
 *     is a new onset the nurse has not acknowledged;
 *   - a technical alarm starts when a sensor reports "no reading" or has
 *     been silent for three of its sampling periods, and clears with the
 *     next good reading.
 * A parameter with no current reading is left out of the score and the bed
 * is marked incomplete; a red score in the parameters present still alarms.
 *
 * Prototype for a hackathon, not a medical device, not for clinical use.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PHILA_BEDS_H
#define PHILA_BEDS_H

#include <stdbool.h>
#include <stdint.h>

#include "phila/news2.h"
#include "phila/scenario.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BED_REASON_MAX 64

typedef enum {
    BEV_ALARM_ON = 0, /* a clinical alarm starts */
    BEV_ALARM_UP,     /* an active clinical alarm moves to a higher band */
    BEV_ALARM_CLEAR,  /* back to low */
    BEV_TECH_ON,      /* a sensor has no reading */
    BEV_TECH_CLEAR    /* the sensor reads again */
} bed_event_kind_t;

typedef struct {
    int bed;
    bed_event_kind_t kind;
    news2_risk_t level;        /* band for clinical events */
    sensor_t sensor;           /* sensor for technical events */
    int64_t t_sched_ns;        /* when the reading behind the event was due */
    int64_t t_sample_ns;       /* when it was taken */
    int64_t t_decided_ns;      /* when this module decided (caller's clock) */
    char reason[BED_REASON_MAX];
} bed_event_t;

typedef struct {
    /* latest readings */
    int16_t val[SENSOR_COUNT][2]; /* the last good reading, kept while the sensor is missing */
    bool have[SENSOR_COUNT];      /* a good reading is held */
    bool seen[SENSOR_COUNT];      /* the sensor has read well at least once */
    bool lost[SENSOR_COUNT];      /* technical alarm active */
    int64_t last_ns[SENSOR_COUNT];/* time of the last reading, good or "none" */
    /* set by a clinician or nurse */
    spo2_scale_t scale;
    bool on_oxygen;
    acvpu_t acvpu;
    /* the score from the readings held */
    news2_result_t res;
    bool complete;                /* every parameter had a current reading */
    news2_risk_t alarm_level;     /* highest band of the active alarm; NEWS2_RISK_LOW when none */
    bool acked;                   /* a nurse acknowledged the current alarm */
    bool held_stale;              /* the alarm is open only on the last readings of missing sensors */
    uint32_t readings;
} bed_t;

typedef struct {
    bed_t bed[SCN_BEDS];
    int64_t period_ns[SENSOR_COUNT];
} ward_t;

/* Start every bed with no readings; sensors silent from now_ns on raise a
 * technical alarm after three periods. */
void ward_init(ward_t *w, const scenario_t *sc, int64_t now_ns);

/* Apply one sensor reading to one bed. Writes up to `cap` events and
 * returns how many. v[0] < 0 means the sensor reports no reading. */
int ward_reading(ward_t *w, int bed, sensor_t s, const int16_t v[2], int64_t t_sched_ns, int64_t t_sample_ns,
                 int64_t now_ns, bed_event_t *ev, int cap);

/* Raise technical alarms for sensors silent for three periods. */
int ward_check_silence(ward_t *w, int64_t now_ns, bed_event_t *ev, int cap);

/* Nurse and clinician inputs. Each rescoring can raise or clear an alarm. */
int ward_set_acvpu(ward_t *w, int bed, acvpu_t level, int64_t now_ns, bed_event_t *ev, int cap);
int ward_set_oxygen(ward_t *w, int bed, bool on_oxygen, int64_t now_ns, bed_event_t *ev, int cap);
int ward_set_scale(ward_t *w, int bed, spo2_scale_t scale, int64_t now_ns, bed_event_t *ev, int cap);
bool ward_ack(ward_t *w, int bed);

/* The observation set the score was taken from (missing parameters filled
 * with neutral values that score 0). */
void ward_obs(const ward_t *w, int bed, news2_obs_t *out);

int ward_bed_index(const char *id); /* "BED-01" -> 0, or -1 */
const char *bed_event_name(bed_event_kind_t k);

#ifdef __cplusplus
}
#endif
#endif /* PHILA_BEDS_H */

/*
 * scenario.h - fictional patients for the simulated sensors.
 *
 * Every bed follows a scripted course in time, with a little deterministic
 * noise, so a run is repeatable: the same scenario and seed give the same
 * readings on a laptop and on QNX. All patients are invented.
 *
 * Prototype for a hackathon, not a medical device, not for clinical use.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PHILA_SCENARIO_H
#define PHILA_SCENARIO_H

#include <stdbool.h>
#include <stdint.h>

#include "phila/news2.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SCN_BEDS 4

/* The five sensor channels. Consciousness (ACVPU) is not a sensor: a nurse
 * enters it through /dev/phila/control. */
typedef enum {
    SENSOR_PULSE_OX = 0, /* SpO2 and pulse rate */
    SENSOR_RESP,         /* respiration rate */
    SENSOR_BP,           /* non-invasive blood pressure, systolic */
    SENSOR_TEMP,         /* temperature */
    SENSOR_COUNT
} sensor_t;

typedef enum {
    SCN_WARD = 0, /* four beds with the stories in scenario.c; about 4 minutes, then holds */
    SCN_PROBE     /* latency probe: beds 1 and 2's SpO2 alternate normal and red on every
                     reading, in opposite phase, so every reading starts an alarm */
} scenario_id_t;

/* One sensor's reading for one bed. A value of -1 means "no reading": the
 * sensor is unplugged or has failed. */
typedef struct {
    int16_t v[2]; /* PULSE_OX: {spo2, pulse}; RESP: {rr, -}; BP: {systolic, -}; TEMP: {temp_dc, -} */
} scn_value_t;

typedef struct {
    scenario_id_t id;
    uint32_t seed;
} scenario_t;

/* Reading for bed b (0-based) from sensor s at t_ms milliseconds after start. */
scn_value_t scn_read(const scenario_t *sc, int bed, sensor_t s, int64_t t_ms);

/* The fixed parts of each bed's observation set: SpO2 scale and oxygen, which
 * a clinician sets, and the starting level of consciousness. */
void scn_bed_defaults(const scenario_t *sc, int bed, spo2_scale_t *scale, bool *on_oxygen, acvpu_t *acvpu);

/* Sampling period of each sensor, in milliseconds, for the scenario. */
int32_t scn_period_ms(const scenario_t *sc, sensor_t s);

const char *scn_sensor_name(sensor_t s); /* "pulse-ox", "resp", "bp", "temp" */
const char *scn_bed_id(int bed);         /* "BED-01" ... */
bool scn_parse(const char *name, scenario_id_t *out);
const char *scn_name(scenario_id_t id);

#ifdef __cplusplus
}
#endif
#endif /* PHILA_SCENARIO_H */

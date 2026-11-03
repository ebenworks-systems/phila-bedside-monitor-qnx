/*
 * scenario.c - fictional patients for the simulated sensors.
 *
 * The ward scenario tells four short stories over about four minutes:
 *
 *   BED-01  stable after an operation: NEWS2 stays at 0.
 *   BED-02  a slow deterioration from 30 s to 210 s: breathing, pulse and
 *           temperature rise while blood pressure and SpO2 fall, so the
 *           score climbs through medium to high.
 *   BED-03  a sudden drop in SpO2 from 60 s to 100 s (a red score), then
 *           recovery.
 *   BED-04  a patient on oxygen scored on SpO2 Scale 2; the blood-pressure
 *           cuff is unplugged from 120 s to 170 s, which must raise a
 *           technical alarm rather than be scored.
 *
 * After 240 s every bed holds its last state. All patients are invented.
 * Prototype for a hackathon, not a medical device, not for clinical use.
 *
 * SPDX-License-Identifier: MIT
 */
#include "phila/scenario.h"

#include <string.h>

static const char *const BED_IDS[SCN_BEDS] = {"BED-01", "BED-02", "BED-03", "BED-04"};

/* A small integer hash, so noise depends only on (seed, bed, sensor, step). */
static uint32_t mix(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

/* Noise in [-amp, +amp], fixed for each sampling step. */
static int noise(const scenario_t *sc, int bed, sensor_t s, int ch, int64_t step, int amp)
{
    if (amp <= 0) return 0;
    uint32_t h = mix(sc->seed ^ mix((uint32_t)bed * 0x9e3779b9U ^ (uint32_t)s * 0x85ebca6bU ^
                                    (uint32_t)ch * 0xc2b2ae35U ^ (uint32_t)step));
    return (int)(h % (uint32_t)(2 * amp + 1)) - amp;
}

/* Linear ramp from a to b between t0 and t1 (milliseconds), clamped. */
static int ramp(int64_t t, int64_t t0, int64_t t1, int a, int b)
{
    if (t <= t0) return a;
    if (t >= t1) return b;
    return a + (int)(((int64_t)(b - a) * (t - t0)) / (t1 - t0));
}

int32_t scn_period_ms(const scenario_t *sc, sensor_t s)
{
    if (sc->id == SCN_PROBE) return s == SENSOR_PULSE_OX ? 100 : 1000;
    switch (s) {
    case SENSOR_PULSE_OX: return 1000;
    case SENSOR_RESP: return 1000;
    case SENSOR_BP: return 10000;
    case SENSOR_TEMP: return 5000;
    default: return 1000;
    }
}

void scn_bed_defaults(const scenario_t *sc, int bed, spo2_scale_t *scale, bool *on_oxygen, acvpu_t *acvpu)
{
    *scale = SPO2_SCALE_1;
    *on_oxygen = false;
    *acvpu = ACVPU_ALERT;
    if (sc->id == SCN_WARD && bed == 3) {
        *scale = SPO2_SCALE_2;
        *on_oxygen = true;
    }
}

static scn_value_t ward(const scenario_t *sc, int bed, sensor_t s, int64_t t)
{
    scn_value_t r = {{-1, -1}};
    int64_t step = t / scn_period_ms(sc, s);
    switch (bed) {
    case 0: /* stable */
        switch (s) {
        case SENSOR_PULSE_OX:
            r.v[0] = (int16_t)(97 + noise(sc, bed, s, 0, step, 1));
            r.v[1] = (int16_t)(72 + noise(sc, bed, s, 1, step, 3));
            break;
        case SENSOR_RESP: r.v[0] = (int16_t)(14 + noise(sc, bed, s, 0, step, 1)); break;
        case SENSOR_BP: r.v[0] = (int16_t)(124 + noise(sc, bed, s, 0, step, 4)); break;
        case SENSOR_TEMP: r.v[0] = (int16_t)(368 + noise(sc, bed, s, 0, step, 1)); break;
        default: break;
        }
        break;
    case 1: /* deterioration from 30 s to 210 s */
        switch (s) {
        case SENSOR_PULSE_OX:
            r.v[0] = (int16_t)(ramp(t, 30000, 210000, 96, 92) + noise(sc, bed, s, 0, step, 0));
            r.v[1] = (int16_t)(ramp(t, 30000, 210000, 84, 128) + noise(sc, bed, s, 1, step, 2));
            break;
        case SENSOR_RESP: r.v[0] = (int16_t)ramp(t, 30000, 210000, 16, 27); break;
        case SENSOR_BP: r.v[0] = (int16_t)(ramp(t, 30000, 210000, 128, 92) + noise(sc, bed, s, 0, step, 2)); break;
        case SENSOR_TEMP: r.v[0] = (int16_t)ramp(t, 30000, 210000, 372, 392); break;
        default: break;
        }
        break;
    case 2: { /* desaturation from 60 s to 100 s */
        bool event = t >= 60000 && t < 100000;
        switch (s) {
        case SENSOR_PULSE_OX:
            r.v[0] = (int16_t)(event ? 86 + noise(sc, bed, s, 0, step, 1) : 97 + noise(sc, bed, s, 0, step, 1));
            r.v[1] = (int16_t)(event ? 104 + noise(sc, bed, s, 1, step, 3) : 78 + noise(sc, bed, s, 1, step, 3));
            break;
        case SENSOR_RESP: r.v[0] = (int16_t)((event ? 19 : 15) + noise(sc, bed, s, 0, step, 1)); break;
        case SENSOR_BP: r.v[0] = (int16_t)(118 + noise(sc, bed, s, 0, step, 4)); break;
        case SENSOR_TEMP: r.v[0] = (int16_t)(370 + noise(sc, bed, s, 0, step, 1)); break;
        default: break;
        }
        break;
    }
    case 3: /* on oxygen, Scale 2; BP cuff unplugged from 120 s to 170 s */
        switch (s) {
        case SENSOR_PULSE_OX:
            r.v[0] = (int16_t)(90 + noise(sc, bed, s, 0, step, 1));
            r.v[1] = (int16_t)(86 + noise(sc, bed, s, 1, step, 3));
            break;
        case SENSOR_RESP: r.v[0] = (int16_t)(18 + noise(sc, bed, s, 0, step, 1)); break;
        case SENSOR_BP:
            if (t >= 120000 && t < 170000) break; /* no reading */
            r.v[0] = (int16_t)(132 + noise(sc, bed, s, 0, step, 4));
            break;
        case SENSOR_TEMP: r.v[0] = (int16_t)(370 + noise(sc, bed, s, 0, step, 1)); break;
        default: break;
        }
        break;
    default: break;
    }
    return r;
}

static scn_value_t probe(const scenario_t *sc, int bed, sensor_t s, int64_t t)
{
    if (bed == 0 && s == SENSOR_PULSE_OX) {
        /* Alternate a normal and a red SpO2 on every reading: each red one is
         * a fresh alarm onset whose latency the monitor measures. */
        int64_t step = t / scn_period_ms(sc, s);
        scn_value_t r = {{(int16_t)((step % 2) ? 89 : 97), 76}};
        return r;
    }
    scenario_t calm = {SCN_WARD, sc->seed};
    return ward(&calm, 0, s, t); /* every other bed reads like the stable one */
}

scn_value_t scn_read(const scenario_t *sc, int bed, sensor_t s, int64_t t_ms)
{
    scn_value_t none = {{-1, -1}};
    if (sc == NULL || bed < 0 || bed >= SCN_BEDS || s < 0 || s >= SENSOR_COUNT || t_ms < 0) return none;
    if (t_ms > 240000 && sc->id == SCN_WARD) t_ms = 240000; /* hold the last state */
    return sc->id == SCN_PROBE ? probe(sc, bed, s, t_ms) : ward(sc, bed, s, t_ms);
}

const char *scn_sensor_name(sensor_t s)
{
    switch (s) {
    case SENSOR_PULSE_OX: return "pulse-ox";
    case SENSOR_RESP: return "resp";
    case SENSOR_BP: return "bp";
    case SENSOR_TEMP: return "temp";
    default: return "?";
    }
}

const char *scn_bed_id(int bed)
{
    return (bed >= 0 && bed < SCN_BEDS) ? BED_IDS[bed] : "?";
}

bool scn_parse(const char *name, scenario_id_t *out)
{
    if (name == NULL) return false;
    if (strcmp(name, "ward") == 0) {
        *out = SCN_WARD;
        return true;
    }
    if (strcmp(name, "probe") == 0) {
        *out = SCN_PROBE;
        return true;
    }
    return false;
}

const char *scn_name(scenario_id_t id)
{
    return id == SCN_PROBE ? "probe" : "ward";
}

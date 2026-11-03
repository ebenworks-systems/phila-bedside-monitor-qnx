/*
 * news2.h - National Early Warning Score 2 (NEWS2).
 *
 * Scoring follows the chart in: Royal College of Physicians. National Early
 * Warning Score (NEWS) 2: Standardising the assessment of acute-illness
 * severity in the NHS. Updated report of a working party. London: RCP, 2017.
 *
 * Prototype for a hackathon, not a medical device, not for clinical use.
 *
 * The functions here are pure: no allocation, no locks, no I/O. They run in
 * well under a microsecond, so the monitor scores every reading on the alarm
 * path without adding to its worst-case latency.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PHILA_NEWS2_H
#define PHILA_NEWS2_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Level of consciousness on the ACVPU scale. Anything but Alert scores 3. */
typedef enum {
    ACVPU_ALERT = 0,
    ACVPU_CONFUSION, /* new confusion */
    ACVPU_VOICE,
    ACVPU_PAIN,
    ACVPU_UNRESPONSIVE
} acvpu_t;

/* SpO2 Scale 1 is the default. Scale 2 is for a patient with a prescribed
 * target range of 88-92% (for example hypercapnic respiratory failure); a
 * clinician chooses it, the monitor never does. */
typedef enum { SPO2_SCALE_1 = 1, SPO2_SCALE_2 = 2 } spo2_scale_t;

typedef struct {
    int resp_rate;           /* breaths per minute */
    int spo2;                /* oxygen saturation, percent */
    spo2_scale_t spo2_scale; /* 1 or 2 */
    bool on_oxygen;          /* supplemental oxygen (true) or air (false) */
    int systolic;            /* systolic blood pressure, mmHg */
    int pulse;               /* beats per minute */
    acvpu_t consciousness;
    int temp_dc;             /* temperature in tenths of a degree Celsius: 368 = 36.8 C */
} news2_obs_t;

typedef enum {
    NEWS2_RISK_LOW = 0,
    NEWS2_RISK_LOW_MEDIUM, /* one parameter scored 3 and the aggregate is below 5 */
    NEWS2_RISK_MEDIUM,     /* aggregate 5 or 6 */
    NEWS2_RISK_HIGH        /* aggregate 7 or more */
} news2_risk_t;

typedef struct {
    uint8_t resp_rate;
    uint8_t spo2;
    uint8_t air_or_oxygen;
    uint8_t systolic;
    uint8_t pulse;
    uint8_t consciousness;
    uint8_t temperature;
    uint8_t total;  /* 0 to 20 */
    bool any_red;   /* at least one parameter scored 3 */
    news2_risk_t risk;
} news2_result_t;

typedef enum {
    NEWS2_OK = 0,
    NEWS2_E_NULL,
    NEWS2_E_RESP_RATE,     /* outside 0..80 */
    NEWS2_E_SPO2,          /* outside 0..100 */
    NEWS2_E_SCALE,
    NEWS2_E_SYSTOLIC,      /* outside 0..300 */
    NEWS2_E_PULSE,         /* outside 0..300 */
    NEWS2_E_CONSCIOUSNESS,
    NEWS2_E_TEMPERATURE    /* outside 25.0..45.0 C */
} news2_status_t;

/* Score one parameter each. Inputs are assumed range-checked already. */
uint8_t news2_score_resp_rate(int breaths_per_min);
uint8_t news2_score_spo2(int spo2, spo2_scale_t scale, bool on_oxygen);
uint8_t news2_score_air_or_oxygen(bool on_oxygen);
uint8_t news2_score_systolic(int mmhg);
uint8_t news2_score_pulse(int bpm);
uint8_t news2_score_consciousness(acvpu_t level);
uint8_t news2_score_temperature(int temp_dc);

/* Check ranges, score every parameter and work out the risk band.
 * Returns NEWS2_OK and fills *out, or an error and leaves *out zeroed. An
 * out-of-range reading is treated as a sensor fault, never scored. */
news2_status_t news2_assess(const news2_obs_t *obs, news2_result_t *out);

const char *news2_risk_name(news2_risk_t risk);  /* "low", "low-medium", "medium", "high" */
const char *news2_status_name(news2_status_t st);
const char *news2_acvpu_letter(acvpu_t level);    /* "A", "C", "V", "P", "U" */

#ifdef __cplusplus
}
#endif
#endif /* PHILA_NEWS2_H */

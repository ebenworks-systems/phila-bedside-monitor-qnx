/*
 * news2.c - NEWS2 scoring (Royal College of Physicians, 2017).
 * Prototype for a hackathon, not a medical device, not for clinical use.
 * SPDX-License-Identifier: MIT
 */
#include "phila/news2.h"

#include <string.h>

/* Respiration rate: <=8 3 | 9-11 1 | 12-20 0 | 21-24 2 | >=25 3 */
uint8_t news2_score_resp_rate(int rr)
{
    if (rr <= 8) return 3;
    if (rr <= 11) return 1;
    if (rr <= 20) return 0;
    if (rr <= 24) return 2;
    return 3;
}

/* SpO2 Scale 1: <=91 3 | 92-93 2 | 94-95 1 | >=96 0
 * SpO2 Scale 2: <=83 3 | 84-85 2 | 86-87 1 | 88-92 0 | >=93 on air 0
 *               93-94 on oxygen 1 | 95-96 on oxygen 2 | >=97 on oxygen 3 */
uint8_t news2_score_spo2(int spo2, spo2_scale_t scale, bool on_oxygen)
{
    if (scale == SPO2_SCALE_2) {
        if (spo2 <= 83) return 3;
        if (spo2 <= 85) return 2;
        if (spo2 <= 87) return 1;
        if (spo2 <= 92) return 0;
        if (!on_oxygen) return 0;
        if (spo2 <= 94) return 1;
        if (spo2 <= 96) return 2;
        return 3;
    }
    if (spo2 <= 91) return 3;
    if (spo2 <= 93) return 2;
    if (spo2 <= 95) return 1;
    return 0;
}

/* Air 0 | Oxygen 2 */
uint8_t news2_score_air_or_oxygen(bool on_oxygen)
{
    return on_oxygen ? 2 : 0;
}

/* Systolic BP: <=90 3 | 91-100 2 | 101-110 1 | 111-219 0 | >=220 3 */
uint8_t news2_score_systolic(int sbp)
{
    if (sbp <= 90) return 3;
    if (sbp <= 100) return 2;
    if (sbp <= 110) return 1;
    if (sbp <= 219) return 0;
    return 3;
}

/* Pulse: <=40 3 | 41-50 1 | 51-90 0 | 91-110 1 | 111-130 2 | >=131 3 */
uint8_t news2_score_pulse(int hr)
{
    if (hr <= 40) return 3;
    if (hr <= 50) return 1;
    if (hr <= 90) return 0;
    if (hr <= 110) return 1;
    if (hr <= 130) return 2;
    return 3;
}

/* Consciousness: Alert 0 | new Confusion, Voice, Pain or Unresponsive 3 */
uint8_t news2_score_consciousness(acvpu_t level)
{
    return level == ACVPU_ALERT ? 0 : 3;
}

/* Temperature (tenths of a degree C):
 * <=35.0 3 | 35.1-36.0 1 | 36.1-38.0 0 | 38.1-39.0 1 | >=39.1 2 */
uint8_t news2_score_temperature(int t)
{
    if (t <= 350) return 3;
    if (t <= 360) return 1;
    if (t <= 380) return 0;
    if (t <= 390) return 1;
    return 2;
}

news2_status_t news2_assess(const news2_obs_t *o, news2_result_t *out)
{
    if (out != NULL) memset(out, 0, sizeof *out);
    if (o == NULL || out == NULL) return NEWS2_E_NULL;
    if (o->resp_rate < 0 || o->resp_rate > 80) return NEWS2_E_RESP_RATE;
    if (o->spo2 < 0 || o->spo2 > 100) return NEWS2_E_SPO2;
    if (o->spo2_scale != SPO2_SCALE_1 && o->spo2_scale != SPO2_SCALE_2) return NEWS2_E_SCALE;
    if (o->systolic < 0 || o->systolic > 300) return NEWS2_E_SYSTOLIC;
    if (o->pulse < 0 || o->pulse > 300) return NEWS2_E_PULSE;
    if (o->consciousness < ACVPU_ALERT || o->consciousness > ACVPU_UNRESPONSIVE) return NEWS2_E_CONSCIOUSNESS;
    if (o->temp_dc < 250 || o->temp_dc > 450) return NEWS2_E_TEMPERATURE;

    news2_result_t r;
    r.resp_rate = news2_score_resp_rate(o->resp_rate);
    r.spo2 = news2_score_spo2(o->spo2, o->spo2_scale, o->on_oxygen);
    r.air_or_oxygen = news2_score_air_or_oxygen(o->on_oxygen);
    r.systolic = news2_score_systolic(o->systolic);
    r.pulse = news2_score_pulse(o->pulse);
    r.consciousness = news2_score_consciousness(o->consciousness);
    r.temperature = news2_score_temperature(o->temp_dc);
    r.total = (uint8_t)(r.resp_rate + r.spo2 + r.air_or_oxygen + r.systolic + r.pulse +
                        r.consciousness + r.temperature);
    /* Air or oxygen scores at most 2, so it never counts as a red score. */
    r.any_red = r.resp_rate == 3 || r.spo2 == 3 || r.systolic == 3 || r.pulse == 3 ||
                r.consciousness == 3 || r.temperature == 3;
    if (r.total >= 7)
        r.risk = NEWS2_RISK_HIGH;
    else if (r.total >= 5)
        r.risk = NEWS2_RISK_MEDIUM;
    else if (r.any_red)
        r.risk = NEWS2_RISK_LOW_MEDIUM;
    else
        r.risk = NEWS2_RISK_LOW;
    *out = r;
    return NEWS2_OK;
}

const char *news2_risk_name(news2_risk_t risk)
{
    switch (risk) {
    case NEWS2_RISK_LOW: return "low";
    case NEWS2_RISK_LOW_MEDIUM: return "low-medium";
    case NEWS2_RISK_MEDIUM: return "medium";
    case NEWS2_RISK_HIGH: return "high";
    }
    return "unknown";
}

const char *news2_status_name(news2_status_t st)
{
    switch (st) {
    case NEWS2_OK: return "ok";
    case NEWS2_E_NULL: return "missing input";
    case NEWS2_E_RESP_RATE: return "respiration rate out of range";
    case NEWS2_E_SPO2: return "SpO2 out of range";
    case NEWS2_E_SCALE: return "unknown SpO2 scale";
    case NEWS2_E_SYSTOLIC: return "systolic pressure out of range";
    case NEWS2_E_PULSE: return "pulse out of range";
    case NEWS2_E_CONSCIOUSNESS: return "unknown consciousness level";
    case NEWS2_E_TEMPERATURE: return "temperature out of range";
    }
    return "unknown";
}

const char *news2_acvpu_letter(acvpu_t level)
{
    static const char *const letters[] = {"A", "C", "V", "P", "U"};
    if (level < ACVPU_ALERT || level > ACVPU_UNRESPONSIVE) return "?";
    return letters[level];
}

/*
 * test_news2.c - every band edge of the NEWS2 chart (Royal College of
 * Physicians, 2017), the aggregate risk bands and the input range checks.
 * SPDX-License-Identifier: MIT
 */
#include "check.h"
#include "phila/news2.h"

struct edge {
    int value;
    int score;
};

static void edges(uint8_t (*fn)(int), const struct edge *e, size_t n, const char *name)
{
    for (size_t i = 0; i < n; i++) {
        int got = fn(e[i].value);
        check_count++;
        if (got != e[i].score) {
            check_failures++;
            fprintf(stderr, "%s(%d) = %d, expected %d\n", name, e[i].value, got, e[i].score);
        }
    }
}

static news2_obs_t normal(void)
{
    news2_obs_t o = {16, 97, SPO2_SCALE_1, false, 120, 70, ACVPU_ALERT, 370};
    return o;
}

int main(void)
{
    /* Respiration rate: <=8 3 | 9-11 1 | 12-20 0 | 21-24 2 | >=25 3 */
    static const struct edge rr[] = {{0, 3}, {8, 3}, {9, 1}, {11, 1}, {12, 0}, {20, 0},
                                     {21, 2}, {24, 2}, {25, 3}, {80, 3}};
    edges(news2_score_resp_rate, rr, sizeof rr / sizeof rr[0], "resp_rate");

    /* Systolic: <=90 3 | 91-100 2 | 101-110 1 | 111-219 0 | >=220 3 */
    static const struct edge sbp[] = {{0, 3},   {90, 3},  {91, 2},  {100, 2}, {101, 1},
                                      {110, 1}, {111, 0}, {219, 0}, {220, 3}, {300, 3}};
    edges(news2_score_systolic, sbp, sizeof sbp / sizeof sbp[0], "systolic");

    /* Pulse: <=40 3 | 41-50 1 | 51-90 0 | 91-110 1 | 111-130 2 | >=131 3 */
    static const struct edge hr[] = {{0, 3},  {40, 3},  {41, 1},  {50, 1},  {51, 0},  {90, 0},
                                     {91, 1}, {110, 1}, {111, 2}, {130, 2}, {131, 3}, {300, 3}};
    edges(news2_score_pulse, hr, sizeof hr / sizeof hr[0], "pulse");

    /* Temperature in tenths: <=35.0 3 | 35.1-36.0 1 | 36.1-38.0 0 | 38.1-39.0 1 | >=39.1 2 */
    static const struct edge temp[] = {{250, 3}, {350, 3}, {351, 1}, {360, 1}, {361, 0},
                                       {380, 0}, {381, 1}, {390, 1}, {391, 2}, {450, 2}};
    edges(news2_score_temperature, temp, sizeof temp / sizeof temp[0], "temperature");

    /* SpO2 Scale 1: <=91 3 | 92-93 2 | 94-95 1 | >=96 0, the same on air or oxygen */
    static const int s1[][2] = {{0, 3}, {91, 3}, {92, 2}, {93, 2}, {94, 1}, {95, 1}, {96, 0}, {100, 0}};
    for (size_t i = 0; i < sizeof s1 / sizeof s1[0]; i++) {
        CHECK_EQ_INT(news2_score_spo2(s1[i][0], SPO2_SCALE_1, false), s1[i][1]);
        CHECK_EQ_INT(news2_score_spo2(s1[i][0], SPO2_SCALE_1, true), s1[i][1]);
    }

    /* SpO2 Scale 2: <=83 3 | 84-85 2 | 86-87 1 | 88-92 0 | >=93 on air 0
     * | 93-94 on oxygen 1 | 95-96 on oxygen 2 | >=97 on oxygen 3 */
    static const int s2[][3] = {/* spo2, on air, on oxygen */
                                {0, 3, 3},  {83, 3, 3}, {84, 2, 2}, {85, 2, 2},  {86, 1, 1}, {87, 1, 1},
                                {88, 0, 0}, {92, 0, 0}, {93, 0, 1}, {94, 0, 1},  {95, 0, 2}, {96, 0, 2},
                                {97, 0, 3}, {100, 0, 3}};
    for (size_t i = 0; i < sizeof s2 / sizeof s2[0]; i++) {
        CHECK_EQ_INT(news2_score_spo2(s2[i][0], SPO2_SCALE_2, false), s2[i][1]);
        CHECK_EQ_INT(news2_score_spo2(s2[i][0], SPO2_SCALE_2, true), s2[i][2]);
    }

    CHECK_EQ_INT(news2_score_air_or_oxygen(false), 0);
    CHECK_EQ_INT(news2_score_air_or_oxygen(true), 2);
    CHECK_EQ_INT(news2_score_consciousness(ACVPU_ALERT), 0);
    CHECK_EQ_INT(news2_score_consciousness(ACVPU_CONFUSION), 3);
    CHECK_EQ_INT(news2_score_consciousness(ACVPU_VOICE), 3);
    CHECK_EQ_INT(news2_score_consciousness(ACVPU_PAIN), 3);
    CHECK_EQ_INT(news2_score_consciousness(ACVPU_UNRESPONSIVE), 3);

    /* Aggregate bands */
    news2_result_t r;
    news2_obs_t o = normal();
    CHECK_EQ_INT(news2_assess(&o, &r), NEWS2_OK);
    CHECK_EQ_INT(r.total, 0);
    CHECK_EQ_INT(r.risk, NEWS2_RISK_LOW);
    CHECK(!r.any_red);

    o = normal();
    o.on_oxygen = true; /* 2 */
    o.pulse = 95;       /* 1 */
    o.temp_dc = 385;    /* 1 */
    CHECK_EQ_INT(news2_assess(&o, &r), NEWS2_OK);
    CHECK_EQ_INT(r.total, 4);
    CHECK_EQ_INT(r.risk, NEWS2_RISK_LOW);

    o = normal();
    o.spo2 = 91; /* a single red score, aggregate 3 */
    CHECK_EQ_INT(news2_assess(&o, &r), NEWS2_OK);
    CHECK_EQ_INT(r.total, 3);
    CHECK(r.any_red);
    CHECK_EQ_INT(r.risk, NEWS2_RISK_LOW_MEDIUM);

    o = normal();
    o.consciousness = ACVPU_CONFUSION; /* new confusion alone is a red score */
    CHECK_EQ_INT(news2_assess(&o, &r), NEWS2_OK);
    CHECK_EQ_INT(r.risk, NEWS2_RISK_LOW_MEDIUM);

    o = normal();
    o.resp_rate = 22; /* 2 */
    o.pulse = 115;    /* 2 */
    o.systolic = 105; /* 1 */
    CHECK_EQ_INT(news2_assess(&o, &r), NEWS2_OK);
    CHECK_EQ_INT(r.total, 5);
    CHECK_EQ_INT(r.risk, NEWS2_RISK_MEDIUM);

    o.temp_dc = 386; /* +1 = 6 */
    CHECK_EQ_INT(news2_assess(&o, &r), NEWS2_OK);
    CHECK_EQ_INT(r.total, 6);
    CHECK_EQ_INT(r.risk, NEWS2_RISK_MEDIUM);

    o.spo2 = 95; /* +1 = 7 */
    CHECK_EQ_INT(news2_assess(&o, &r), NEWS2_OK);
    CHECK_EQ_INT(r.total, 7);
    CHECK_EQ_INT(r.risk, NEWS2_RISK_HIGH);

    /* Highest possible: 3+3+2+3+3+3+2 = 19 on Scale 1 */
    news2_obs_t worst = {5, 80, SPO2_SCALE_1, true, 80, 150, ACVPU_UNRESPONSIVE, 400};
    CHECK_EQ_INT(news2_assess(&worst, &r), NEWS2_OK);
    CHECK_EQ_INT(r.total, 19);
    CHECK_EQ_INT(r.risk, NEWS2_RISK_HIGH);

    /* Out-of-range readings are sensor faults, never scored */
    o = normal();
    o.resp_rate = 81;
    CHECK_EQ_INT(news2_assess(&o, &r), NEWS2_E_RESP_RATE);
    CHECK_EQ_INT(r.total, 0);
    o = normal();
    o.spo2 = 101;
    CHECK_EQ_INT(news2_assess(&o, &r), NEWS2_E_SPO2);
    o = normal();
    o.spo2_scale = (spo2_scale_t)3;
    CHECK_EQ_INT(news2_assess(&o, &r), NEWS2_E_SCALE);
    o = normal();
    o.systolic = -1;
    CHECK_EQ_INT(news2_assess(&o, &r), NEWS2_E_SYSTOLIC);
    o = normal();
    o.pulse = 301;
    CHECK_EQ_INT(news2_assess(&o, &r), NEWS2_E_PULSE);
    o = normal();
    o.consciousness = (acvpu_t)9;
    CHECK_EQ_INT(news2_assess(&o, &r), NEWS2_E_CONSCIOUSNESS);
    o = normal();
    o.temp_dc = 249;
    CHECK_EQ_INT(news2_assess(&o, &r), NEWS2_E_TEMPERATURE);
    o.temp_dc = 451;
    CHECK_EQ_INT(news2_assess(&o, &r), NEWS2_E_TEMPERATURE);
    CHECK_EQ_INT(news2_assess(NULL, &r), NEWS2_E_NULL);

    CHECK_EQ_STR(news2_risk_name(NEWS2_RISK_LOW_MEDIUM), "low-medium");
    CHECK_EQ_STR(news2_acvpu_letter(ACVPU_VOICE), "V");
    return check_report("news2");
}

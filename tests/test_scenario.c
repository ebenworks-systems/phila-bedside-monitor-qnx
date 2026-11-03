/*
 * test_scenario.c - runs the ward scenario through the alarm decision in
 * simulated time and checks each bed's story ends as scripted.
 * SPDX-License-Identifier: MIT
 */
#include "check.h"
#include "phila/beds.h"
#include "phila/scenario.h"

#define MS 1000000LL

typedef struct {
    int on, up, clear, tech_on, tech_clear;
    int64_t first_on_ms, last_clear_ms, first_tech_ms, last_tech_clear_ms, high_ms;
} tally_t;

static void run(const scenario_t *sc, int64_t end_ms, tally_t t[SCN_BEDS])
{
    static ward_t w;
    bed_event_t ev[16];
    memset(t, 0, sizeof(tally_t) * SCN_BEDS);
    for (int b = 0; b < SCN_BEDS; b++) t[b].first_on_ms = t[b].first_tech_ms = t[b].high_ms = -1;
    ward_init(&w, sc, 0);
    for (int64_t ms = 0; ms <= end_ms; ms += 50) {
        for (int s = 0; s < SENSOR_COUNT; s++) {
            if (ms % scn_period_ms(sc, (sensor_t)s) != 0) continue;
            for (int b = 0; b < SCN_BEDS; b++) {
                scn_value_t v = scn_read(sc, b, (sensor_t)s, ms);
                int n = ward_reading(&w, b, (sensor_t)s, v.v, ms * MS, ms * MS, ms * MS, ev, 16);
                for (int i = 0; i < n && i < 16; i++) {
                    tally_t *x = &t[ev[i].bed];
                    switch (ev[i].kind) {
                    case BEV_ALARM_ON: x->on++; if (x->first_on_ms < 0) x->first_on_ms = ms; break;
                    case BEV_ALARM_UP: x->up++; break;
                    case BEV_ALARM_CLEAR: x->clear++; x->last_clear_ms = ms; break;
                    case BEV_TECH_ON: x->tech_on++; if (x->first_tech_ms < 0) x->first_tech_ms = ms; break;
                    case BEV_TECH_CLEAR: x->tech_clear++; x->last_tech_clear_ms = ms; break;
                    }
                    if ((ev[i].kind == BEV_ALARM_ON || ev[i].kind == BEV_ALARM_UP) && ev[i].level == NEWS2_RISK_HIGH &&
                        x->high_ms < 0)
                        x->high_ms = ms;
                }
            }
        }
        int n = ward_check_silence(&w, ms * MS, ev, 16);
        for (int i = 0; i < n && i < 16; i++) t[ev[i].bed].tech_on += 1000; /* must never happen */
    }
}

int main(void)
{
    scenario_t sc = {SCN_WARD, 2026};
    tally_t t[SCN_BEDS];
    run(&sc, 250000, t);

    /* BED-01: stable, never alarms. */
    CHECK_EQ_INT(t[0].on + t[0].up + t[0].clear + t[0].tech_on, 0);

    /* BED-02: deteriorates after 30 s and reaches high before the ramp ends at 210 s. */
    CHECK(t[1].first_on_ms > 30000);
    CHECK(t[1].high_ms > t[1].first_on_ms);
    CHECK(t[1].high_ms <= 210000);
    CHECK(t[1].up <= 2); /* medium then high, with no repeats while the score wobbles */
    CHECK_EQ_INT(t[1].on, 1);
    CHECK_EQ_INT(t[1].tech_on, 0);
    printf("BED-02: first alarm at %lld ms, high at %lld ms, %d escalations\n", (long long)t[1].first_on_ms,
           (long long)t[1].high_ms, t[1].up);

    /* BED-03: one alarm at exactly 60 s, cleared at exactly 100 s. */
    CHECK_EQ_INT(t[2].on, 1);
    CHECK_EQ_INT(t[2].first_on_ms, 60000);
    CHECK_EQ_INT(t[2].clear, 1);
    CHECK_EQ_INT(t[2].last_clear_ms, 100000);

    /* BED-04: no clinical alarm; the cuff's technical alarm from 120 s to 170 s. */
    CHECK_EQ_INT(t[3].on, 0);
    CHECK_EQ_INT(t[3].tech_on, 1);
    CHECK_EQ_INT(t[3].first_tech_ms, 120000);
    CHECK_EQ_INT(t[3].tech_clear, 1);
    CHECK_EQ_INT(t[3].last_tech_clear_ms, 170000);

    /* Repeatable: the same seed gives the same readings; another seed differs. */
    scenario_t other = {SCN_WARD, 7};
    int same = 1, differ = 0;
    for (int64_t ms = 0; ms < 60000; ms += 1000) {
        scn_value_t a = scn_read(&sc, 0, SENSOR_PULSE_OX, ms), b = scn_read(&sc, 0, SENSOR_PULSE_OX, ms);
        scn_value_t c = scn_read(&other, 0, SENSOR_PULSE_OX, ms);
        if (a.v[0] != b.v[0] || a.v[1] != b.v[1]) same = 0;
        if (a.v[1] != c.v[1]) differ = 1;
    }
    CHECK(same);
    CHECK(differ);

    /* Probe: beds 1 and 2's SpO2 alternate every 100 ms in opposite phase,
     * so every reading starts an alarm on one of them. */
    scenario_t probe = {SCN_PROBE, 1};
    CHECK_EQ_INT(scn_period_ms(&probe, SENSOR_PULSE_OX), 100);
    run(&probe, 10000, t);
    CHECK_EQ_INT(t[0].on, 50);
    CHECK_EQ_INT(t[0].clear, 50);
    CHECK_EQ_INT(t[1].on, 51);
    CHECK_EQ_INT(t[1].clear, 50);
    CHECK_EQ_INT(t[2].on + t[3].on, 0);
    for (int64_t step = 0; step < 200; step++) { /* exactly one of the two is red on each reading */
        int red0 = scn_read(&probe, 0, SENSOR_PULSE_OX, step * 100).v[0] == 89;
        int red1 = scn_read(&probe, 1, SENSOR_PULSE_OX, step * 100).v[0] == 89;
        CHECK(red0 + red1 == 1);
    }

    scenario_id_t id;
    CHECK(scn_parse("probe", &id) && id == SCN_PROBE);
    CHECK(!scn_parse("icu", &id));
    return check_report("scenario");
}

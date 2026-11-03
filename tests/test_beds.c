/*
 * test_beds.c - the alarm decision: onset, escalation, clearing, technical
 * alarms for a missing or silent sensor, and nurse inputs.
 * SPDX-License-Identifier: MIT
 */
#include "check.h"
#include "phila/beds.h"

#define MS 1000000LL

static ward_t w;
static bed_event_t ev[8];

static int feed(int bed, sensor_t s, int a, int b, int64_t t)
{
    int16_t v[2] = {(int16_t)a, (int16_t)b};
    return ward_reading(&w, bed, s, v, t, t + 5 * MS / 1000, t + 7 * MS / 1000, ev, 8);
}

/* Normal readings; bed 4 is on SpO2 Scale 2 with oxygen, where 90 is normal. */
static void all_normal(int bed, int64_t t)
{
    feed(bed, SENSOR_PULSE_OX, bed == 3 ? 90 : 97, 70, t);
    feed(bed, SENSOR_RESP, 16, 0, t);
    feed(bed, SENSOR_BP, 120, 0, t);
    feed(bed, SENSOR_TEMP, 370, 0, t);
}

int main(void)
{
    scenario_t sc = {SCN_WARD, 1};
    ward_init(&w, &sc, 0);
    int n;

    /* A full set of normal readings: no events, complete, NEWS2 0. */
    CHECK_EQ_INT(feed(0, SENSOR_PULSE_OX, 97, 70, 0), 0);
    CHECK(!w.bed[0].complete);
    CHECK_EQ_INT(feed(0, SENSOR_RESP, 16, 0, 0), 0);
    CHECK_EQ_INT(feed(0, SENSOR_BP, 120, 0, 0), 0);
    CHECK_EQ_INT(feed(0, SENSOR_TEMP, 370, 0, 0), 0);
    CHECK(w.bed[0].complete);
    CHECK_EQ_INT(w.bed[0].res.total, 0);

    /* One red score starts a low-medium alarm and carries the reading's times. */
    n = feed(0, SENSOR_PULSE_OX, 91, 70, 1000 * MS);
    CHECK_EQ_INT(n, 1);
    CHECK_EQ_INT(ev[0].kind, BEV_ALARM_ON);
    CHECK_EQ_INT(ev[0].level, NEWS2_RISK_LOW_MEDIUM);
    CHECK_EQ_STR(ev[0].reason, "news2=3 red=spo2");
    CHECK_EQ_INT(ev[0].t_sched_ns, 1000 * MS);
    CHECK_EQ_INT(ev[0].t_decided_ns, 1000 * MS + 7000);

    /* Rising scores escalate once per band. */
    n = feed(0, SENSOR_RESP, 22, 0, 2000 * MS); /* 3 + 2 = 5 */
    CHECK_EQ_INT(n, 1);
    CHECK_EQ_INT(ev[0].kind, BEV_ALARM_UP);
    CHECK_EQ_INT(ev[0].level, NEWS2_RISK_MEDIUM);
    n = feed(0, SENSOR_PULSE_OX, 91, 115, 3000 * MS); /* 3 + 2 + 2 = 7 */
    CHECK_EQ_INT(n, 1);
    CHECK_EQ_INT(ev[0].kind, BEV_ALARM_UP);
    CHECK_EQ_INT(ev[0].level, NEWS2_RISK_HIGH);
    CHECK_EQ_STR(ev[0].reason, "news2=7 red=spo2");
    n = feed(0, SENSOR_BP, 105, 0, 4000 * MS); /* 8, still high */
    CHECK_EQ_INT(n, 0);

    /* The nurse acknowledges; the alarm stays until the scores fall. */
    CHECK(ward_ack(&w, 0));
    CHECK(w.bed[0].acked);
    CHECK(!ward_ack(&w, 1)); /* nothing to acknowledge on bed 2 */

    /* Dropping straight from high to low (aggregate 3, no red) clears it. */
    n = feed(0, SENSOR_PULSE_OX, 97, 70, 5000 * MS);
    CHECK_EQ_INT(n, 1);
    CHECK_EQ_INT(ev[0].kind, BEV_ALARM_CLEAR);
    CHECK_EQ_INT(w.bed[0].alarm_level, NEWS2_RISK_LOW);
    CHECK(!w.bed[0].acked);
    all_normal(0, 6000 * MS);
    CHECK_EQ_INT(w.bed[0].res.total, 0);

    /* Start again: rise to high, then wobble. The alarm holds high until the
     * bed is back to low, so the wobble raises nothing. */
    ward_init(&w, &sc, 0);
    all_normal(1, 0);
    feed(1, SENSOR_RESP, 26, 0, 10 * MS);           /* 3, red: low-medium */
    feed(1, SENSOR_PULSE_OX, 97, 120, 20 * MS);     /* 5: medium */
    n = feed(1, SENSOR_BP, 95, 0, 30 * MS);         /* 7: high */
    CHECK_EQ_INT(n, 1);
    CHECK_EQ_INT(ev[0].level, NEWS2_RISK_HIGH);
    n = feed(1, SENSOR_BP, 120, 0, 40 * MS);        /* 5: medium now */
    CHECK_EQ_INT(n, 0);
    CHECK_EQ_INT(w.bed[1].res.risk, NEWS2_RISK_MEDIUM);
    CHECK_EQ_INT(w.bed[1].alarm_level, NEWS2_RISK_HIGH);
    n = feed(1, SENSOR_BP, 95, 0, 50 * MS);         /* 7 again: no repeat */
    CHECK_EQ_INT(n, 0);
    feed(1, SENSOR_BP, 120, 0, 60 * MS);
    n = feed(1, SENSOR_PULSE_OX, 97, 70, 70 * MS);  /* 3, red rr: low-medium */
    CHECK_EQ_INT(n, 0);
    CHECK_EQ_INT(w.bed[1].alarm_level, NEWS2_RISK_HIGH);
    n = feed(1, SENSOR_RESP, 16, 0, 80 * MS);       /* 0 */
    CHECK_EQ_INT(n, 1);
    CHECK_EQ_INT(ev[0].kind, BEV_ALARM_CLEAR);
    /* After clearing, a new rise is a new alarm. */
    n = feed(1, SENSOR_RESP, 26, 0, 85 * MS);
    CHECK_EQ_INT(n, 1);
    CHECK_EQ_INT(ev[0].kind, BEV_ALARM_ON);
    feed(1, SENSOR_RESP, 16, 0, 86 * MS);

    /* A sensor that reports no reading raises a technical alarm once. */
    n = feed(1, SENSOR_BP, -1, -1, 90 * MS);
    CHECK_EQ_INT(n, 1);
    CHECK_EQ_INT(ev[0].kind, BEV_TECH_ON);
    CHECK_EQ_INT(ev[0].sensor, SENSOR_BP);
    CHECK_EQ_STR(ev[0].reason, "no-reading:bp");
    CHECK(!w.bed[1].complete);
    CHECK_EQ_INT(feed(1, SENSOR_BP, -1, -1, 100 * MS), 0);
    n = feed(1, SENSOR_BP, 118, 0, 110 * MS);
    CHECK_EQ_INT(n, 1);
    CHECK_EQ_INT(ev[0].kind, BEV_TECH_CLEAR);
    CHECK(w.bed[1].complete);

    /* An impossible value is a fault, not a score. */
    n = feed(1, SENSOR_PULSE_OX, 101, 70, 120 * MS);
    CHECK_EQ_INT(n, 1);
    CHECK_EQ_STR(ev[0].reason, "fault:pulse-ox");

    /* A red score in the parameters still present alarms while incomplete. */
    n = feed(1, SENSOR_RESP, 7, 0, 130 * MS);
    CHECK_EQ_INT(n, 1);
    CHECK_EQ_INT(ev[0].kind, BEV_ALARM_ON);
    CHECK_EQ_STR(ev[0].reason, "news2=3 red=rr incomplete");

    /* Silence: three periods without a reading raise a technical alarm. */
    ward_init(&w, &sc, 0);
    all_normal(2, 0);
    int64_t temp_period = (int64_t)scn_period_ms(&sc, SENSOR_TEMP) * MS;
    all_normal(0, 0); all_normal(1, 0); all_normal(3, 0);
    for (int b = 0; b < SCN_BEDS; b++)
        for (int s = 0; s < SENSOR_COUNT; s++)
            if (s != SENSOR_TEMP || b != 2) w.bed[b].last_ns[s] = 3 * temp_period; /* others fresh */
    /* bed 3's thermometer last read at 5 us (see feed); due three periods later */
    int64_t due = 5000 + 3 * temp_period;
    CHECK_EQ_INT(ward_check_silence(&w, due - 1, ev, 8), 0);
    n = ward_check_silence(&w, due, ev, 8);
    CHECK_EQ_INT(n, 1);
    CHECK_EQ_INT(ev[0].kind, BEV_TECH_ON);
    CHECK_EQ_INT(ev[0].bed, 2);
    CHECK_EQ_STR(ev[0].reason, "silent:temp");
    CHECK_EQ_INT(ev[0].t_sched_ns, due);
    CHECK_EQ_INT(ward_check_silence(&w, due + 1000 * MS, ev, 8), 0); /* raised once */

    /* Nurse input: new confusion is a red score on its own. Bed 4 starts on
     * Scale 2 with oxygen (2 points), so confusion takes it to 5, medium. */
    ward_init(&w, &sc, 0);
    all_normal(3, 0);
    CHECK_EQ_INT(w.bed[3].scale, SPO2_SCALE_2);
    CHECK(w.bed[3].on_oxygen);
    CHECK_EQ_INT(w.bed[3].res.total, 2);
    CHECK_EQ_INT(w.bed[3].alarm_level, NEWS2_RISK_LOW);
    n = ward_set_acvpu(&w, 3, ACVPU_CONFUSION, 5 * MS, ev, 8);
    CHECK_EQ_INT(n, 1);
    CHECK_EQ_INT(ev[0].kind, BEV_ALARM_ON);
    CHECK_EQ_INT(ev[0].level, NEWS2_RISK_MEDIUM);
    CHECK_EQ_STR(ev[0].reason, "news2=5 red=acvpu");
    CHECK_EQ_INT(ward_set_acvpu(&w, 3, (acvpu_t)7, 6 * MS, ev, 8), -1);
    n = ward_set_acvpu(&w, 3, ACVPU_ALERT, 7 * MS, ev, 8);
    CHECK_EQ_INT(n, 1);
    CHECK_EQ_INT(ev[0].kind, BEV_ALARM_CLEAR);
    /* Taking the oxygen away on Scale 2 removes its 2 points. */
    CHECK_EQ_INT(ward_set_oxygen(&w, 3, false, 8 * MS, ev, 8), 0);
    CHECK_EQ_INT(w.bed[3].res.total, 0);

    /* A sensor that stops reading cannot clear the alarm it caused. The
     * missing SpO2 scores 0, so the score falls to low, but the alarm holds
     * until a complete set of readings is back to low. */
    ward_init(&w, &sc, 0);
    all_normal(0, 0);
    n = feed(0, SENSOR_PULSE_OX, 89, 70, 10 * MS); /* SpO2 89 on Scale 1: red */
    CHECK_EQ_INT(n, 1);
    CHECK_EQ_INT(ev[0].kind, BEV_ALARM_ON);
    n = feed(0, SENSOR_PULSE_OX, -1, -1, 20 * MS); /* the probe comes off */
    CHECK_EQ_INT(n, 1);
    CHECK_EQ_INT(ev[0].kind, BEV_TECH_ON);
    CHECK_EQ_INT(w.bed[0].res.risk, NEWS2_RISK_LOW);
    CHECK_EQ_INT(w.bed[0].alarm_level, NEWS2_RISK_LOW_MEDIUM);
    n = feed(0, SENSOR_RESP, 16, 0, 30 * MS); /* other sensors still read */
    CHECK_EQ_INT(n, 0);
    n = feed(0, SENSOR_PULSE_OX, 97, 70, 40 * MS); /* back, and normal */
    CHECK_EQ_INT(n, 2);
    CHECK_EQ_INT(ev[0].kind, BEV_TECH_CLEAR);
    CHECK_EQ_INT(ev[1].kind, BEV_ALARM_CLEAR);
    CHECK_EQ_INT(w.bed[0].alarm_level, NEWS2_RISK_LOW);
    /* The same when the probe goes silent instead of reporting no reading. */
    n = feed(0, SENSOR_PULSE_OX, 89, 70, 50 * MS);
    CHECK_EQ_INT(n, 1);
    CHECK_EQ_INT(ev[0].kind, BEV_ALARM_ON);
    int64_t ox_period = (int64_t)scn_period_ms(&sc, SENSOR_PULSE_OX) * MS;
    int64_t later = 50 * MS + 5000 + 3 * ox_period;
    for (int b = 0; b < SCN_BEDS; b++)
        for (int s = 0; s < SENSOR_COUNT; s++)
            if (s != SENSOR_PULSE_OX || b != 0) w.bed[b].last_ns[s] = later; /* others fresh */
    n = ward_check_silence(&w, later, ev, 8);
    CHECK_EQ_INT(n, 1);
    CHECK_EQ_INT(ev[0].kind, BEV_TECH_ON);
    CHECK_EQ_STR(ev[0].reason, "silent:pulse-ox");
    CHECK_EQ_INT(w.bed[0].alarm_level, NEWS2_RISK_LOW_MEDIUM);
    CHECK(w.bed[0].held_stale);

    /* A missing sensor whose last reading was normal does not hold an
     * alarm: the cuff is off, SpO2 alarms, is acknowledged and recovers, so
     * the alarm clears; the next drop is a new alarm the nurse has not seen. */
    ward_init(&w, &sc, 0);
    all_normal(0, 0);
    n = feed(0, SENSOR_BP, -1, -1, 10 * MS); /* the cuff comes off; it last read 120 */
    CHECK_EQ_INT(n, 1);
    CHECK_EQ_INT(ev[0].kind, BEV_TECH_ON);
    n = feed(0, SENSOR_PULSE_OX, 89, 70, 20 * MS);
    CHECK_EQ_INT(n, 1);
    CHECK_EQ_INT(ev[0].kind, BEV_ALARM_ON);
    CHECK(ward_ack(&w, 0));
    n = feed(0, SENSOR_PULSE_OX, 97, 70, 30 * MS);
    CHECK_EQ_INT(n, 1);
    CHECK_EQ_INT(ev[0].kind, BEV_ALARM_CLEAR);
    n = feed(0, SENSOR_PULSE_OX, 89, 70, 40 * MS);
    CHECK_EQ_INT(n, 1);
    CHECK_EQ_INT(ev[0].kind, BEV_ALARM_ON);
    CHECK(!w.bed[0].acked);

    /* An alarm held only on a missing sensor's last reading: a new red score
     * from the readings present is a new onset, not hidden behind the
     * acknowledged one. */
    ward_init(&w, &sc, 0);
    all_normal(0, 0);
    n = feed(0, SENSOR_PULSE_OX, 89, 70, 10 * MS);
    CHECK_EQ_INT(n, 1);
    CHECK(ward_ack(&w, 0));
    n = feed(0, SENSOR_PULSE_OX, -1, -1, 20 * MS); /* held on the last SpO2, 89 */
    CHECK_EQ_INT(n, 1);
    CHECK_EQ_INT(ev[0].kind, BEV_TECH_ON);
    CHECK(w.bed[0].held_stale);
    CHECK(w.bed[0].acked);
    n = feed(0, SENSOR_RESP, 25, 0, 30 * MS); /* breathing rate 25: red */
    CHECK_EQ_INT(n, 1);
    CHECK_EQ_INT(ev[0].kind, BEV_ALARM_ON);
    CHECK_EQ_INT(ev[0].level, NEWS2_RISK_LOW_MEDIUM);
    CHECK_EQ_STR(ev[0].reason, "news2=3 red=rr incomplete");
    CHECK(!w.bed[0].acked);
    CHECK(!w.bed[0].held_stale);
    /* The same band again from the readings present is a wobble, not a new alarm. */
    CHECK_EQ_INT(feed(0, SENSOR_RESP, 26, 0, 40 * MS), 0);

    /* The missing sensor coming back with the abnormal reading it had is the
     * alarm the nurse already acknowledged, not a new one. */
    ward_init(&w, &sc, 0);
    all_normal(0, 0);
    n = feed(0, SENSOR_PULSE_OX, 89, 70, 10 * MS);
    CHECK_EQ_INT(n, 1);
    CHECK(ward_ack(&w, 0));
    n = feed(0, SENSOR_PULSE_OX, -1, -1, 20 * MS); /* one "no reading" sample */
    CHECK_EQ_INT(n, 1);
    CHECK_EQ_INT(ev[0].kind, BEV_TECH_ON);
    CHECK(w.bed[0].held_stale);
    n = feed(0, SENSOR_PULSE_OX, 89, 70, 30 * MS); /* back, still 89 */
    CHECK_EQ_INT(n, 1);
    CHECK_EQ_INT(ev[0].kind, BEV_TECH_CLEAR);
    CHECK(w.bed[0].acked);
    CHECK(!w.bed[0].held_stale);
    CHECK_EQ_INT(w.bed[0].alarm_level, NEWS2_RISK_LOW_MEDIUM);
    /* The same after a silence of three periods, as under a CPU hog. */
    int64_t back = 30 * MS + 5000 + 3 * ox_period;
    for (int b = 0; b < SCN_BEDS; b++)
        for (int s = 0; s < SENSOR_COUNT; s++)
            if (s != SENSOR_PULSE_OX || b != 0) w.bed[b].last_ns[s] = back; /* others fresh */
    n = ward_check_silence(&w, back, ev, 8);
    CHECK_EQ_INT(n, 1);
    CHECK_EQ_INT(ev[0].kind, BEV_TECH_ON);
    n = feed(0, SENSOR_PULSE_OX, 89, 70, back + 10 * MS);
    CHECK_EQ_INT(n, 1);
    CHECK_EQ_INT(ev[0].kind, BEV_TECH_CLEAR);
    CHECK(w.bed[0].acked);
    /* Back and normal: the alarm clears as before. */
    n = feed(0, SENSOR_PULSE_OX, 97, 70, back + 20 * MS);
    CHECK_EQ_INT(n, 1);
    CHECK_EQ_INT(ev[0].kind, BEV_ALARM_CLEAR);

    /* Events beyond the caller's array are counted, not written. */
    ward_init(&w, &sc, 0);
    n = ward_check_silence(&w, 60000 * MS, ev, 2);
    CHECK(n > 2);

    CHECK_EQ_INT(ward_bed_index("BED-03"), 2);
    CHECK_EQ_INT(ward_bed_index("BED-9"), -1);
    return check_report("beds");
}

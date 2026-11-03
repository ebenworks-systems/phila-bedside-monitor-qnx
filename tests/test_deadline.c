/*
 * test_deadline.c - a sensor's deadline grid with a one-shot timer: a late
 * pulse serves its own deadline and counts the ones it passed as missed, an
 * early pulse takes no reading, a deadline that passed during the send is
 * served late and measured, and a stall never moves the deadlines after it.
 * SPDX-License-Identifier: MIT
 */
#include "check.h"
#include "phila/deadline.h"

#define US 1000LL
#define MS 1000000LL

/* A timer that never fires before its deadline and fires at once when it is
 * armed for a deadline already passed; it wakes the thread `wake` later,
 * except during a stall, when nothing runs until the stall ends. The thread
 * then needs `work` to send the reading before it arms the next deadline. */
typedef struct {
    int64_t stall_at, stall_len; /* one stall of the whole machine */
    int64_t wake, work;          /* timer wake and send time */
    int64_t settle;              /* readings this long after the stall's end count as settled */
} sim_t;

typedef struct {
    int64_t readings, missed, last_latest, armed;
    int64_t worst_settled_wake, earliest_wake;
} sim_out_t;

static int64_t after_stall(const sim_t *s, int64_t t)
{
    return t >= s->stall_at && t < s->stall_at + s->stall_len ? s->stall_at + s->stall_len : t;
}

static sim_out_t simulate(const sim_t *s, int64_t start, int64_t period, int64_t until)
{
    deadline_t d;
    dl_init(&d, start, period, 0);
    sim_out_t o = {0, 0, -1, 0, 0, INT64_MAX};
    int64_t armed_at = 0;
    for (;;) {
        int64_t due = dl_at(&d, d.armed);
        int64_t fires = due > armed_at ? due : armed_at;
        int64_t now = after_stall(s, fires + s->wake);
        if (now >= until) break;
        deadline_reading_t r;
        if (!dl_pulse(&d, now, &r)) break; /* cannot happen: this timer is never early */
        int64_t w = now - dl_at(&d, r.served);
        if (w < o.earliest_wake) o.earliest_wake = w;
        if (now >= s->stall_at + s->stall_len + s->settle && w > o.worst_settled_wake) o.worst_settled_wake = w;
        o.readings++;
        o.missed += r.missed;
        o.last_latest = r.latest;
        dl_advance(&d, &r);
        armed_at = after_stall(s, now + s->work);
    }
    o.armed = d.armed;
    return o;
}

int main(void)
{
    deadline_t d;
    deadline_reading_t r;

    /* On time: each pulse serves the armed deadline and arms the next. */
    dl_init(&d, 1000 * MS, 100 * MS, 0);
    CHECK_EQ_INT(dl_at(&d, 0), 1000 * MS);
    CHECK_EQ_INT(dl_at(&d, 3), 1300 * MS);
    CHECK(dl_pulse(&d, 1000 * MS + 40 * US, &r));
    CHECK_EQ_INT(r.served, 0);
    CHECK_EQ_INT(r.latest, 0);
    CHECK_EQ_INT(r.missed, 0);
    dl_advance(&d, &r);
    CHECK_EQ_INT(d.armed, 1);

    /* A pulse before the armed deadline takes no reading: with no allowance
     * not even 1 ns early, and the timer is armed for the same deadline. */
    CHECK(!dl_pulse(&d, 1100 * MS - 1, &r));
    CHECK_EQ_INT(d.armed, 1);
    CHECK(dl_pulse(&d, 1100 * MS, &r));
    CHECK_EQ_INT(r.served, 1);
    CHECK_EQ_INT(r.latest, 1);

    /* An allowance lets a pulse that early count for the armed deadline,
     * never for the one before it, and no earlier pulse counts. */
    dl_init(&d, 0, 100 * MS, 50 * US);
    d.armed = 4;
    CHECK(!dl_pulse(&d, 400 * MS - 51 * US, &r));
    CHECK(dl_pulse(&d, 400 * MS - 50 * US, &r));
    CHECK_EQ_INT(r.served, 4);
    CHECK_EQ_INT(r.latest, 4);
    CHECK_EQ_INT(r.missed, 0);
    dl_advance(&d, &r);
    CHECK_EQ_INT(d.armed, 5);

    /* A pulse held up past further deadlines serves the armed one, late,
     * counts the ones passed as missed, and the timer goes back on the grid. */
    dl_init(&d, 1000 * MS, 100 * MS, 0);
    d.armed = 2;
    CHECK(dl_pulse(&d, 1530 * MS, &r));
    CHECK_EQ_INT(r.served, 2);
    CHECK_EQ_INT(r.latest, 5);
    CHECK_EQ_INT(r.missed, 3);
    dl_advance(&d, &r);
    CHECK_EQ_INT(d.armed, 6);
    CHECK_EQ_INT(dl_at(&d, d.armed), 1600 * MS);

    /* A send that outlasts the next deadline: the timer is still armed for
     * it, fires at once, and the reading for it is late from its own
     * deadline, with the deadlines after it that also passed missed. */
    CHECK(dl_pulse(&d, 1600 * MS + 20 * US, &r));
    CHECK_EQ_INT(r.latest, 6);
    dl_advance(&d, &r);
    CHECK_EQ_INT(d.armed, 7);
    CHECK(dl_pulse(&d, 1850 * MS, &r)); /* fired at once when armed at 1850 ms */
    CHECK_EQ_INT(r.served, 7);
    CHECK_EQ_INT(r.latest, 8);
    CHECK_EQ_INT(r.missed, 1);
    CHECK_EQ_INT(1850 * MS - dl_at(&d, r.served), 150 * MS); /* the delay that is measured */

    /* Before deadline 0 nothing is taken. */
    dl_init(&d, 1000 * MS, 100 * MS, 0);
    CHECK(!dl_pulse(&d, 999 * MS, &r));
    /* A period of zero cannot divide by zero. */
    dl_init(&d, 0, 0, -5);
    CHECK_EQ_INT(d.period_ns, 1);
    CHECK_EQ_INT(d.early_ns, 0);

    /* Simulated stalls of 330 ms and 170 ms on a 100 ms sensor whose grid
     * starts at 500 ms: after the stall every reading is back on its own
     * deadline (wake back to the timer's 30 us), no reading comes before its
     * deadline, and every deadline before the armed one is a reading or
     * missed. The 330 ms stall from 4037 ms holds up the deadlines at 4100,
     * 4200 and 4300 ms: one late reading and two missed; the 170 ms one from
     * 8061 ms those at 8100 and 8200 ms: one late reading and one missed. */
    const int64_t stalls[][3] = {{4000 * MS + 37 * MS, 330 * MS, 2}, {8000 * MS + 61 * MS, 170 * MS, 1}};
    for (int i = 0; i < 2; i++) {
        sim_t s = {stalls[i][0], stalls[i][1], 30 * US, 200 * US, 100 * MS};
        sim_out_t o = simulate(&s, 500 * MS, 100 * MS, 12000 * MS);
        CHECK_EQ_INT(o.worst_settled_wake, 30 * US);
        CHECK(o.earliest_wake >= 0);
        CHECK_EQ_INT(o.missed, stalls[i][2]);
        CHECK_EQ_INT(o.readings + o.missed, o.armed);
        CHECK_EQ_INT(o.armed, o.last_latest + 1);
    }

    /* A send that takes 250 ms on a 100 ms sensor: the sensor falls behind
     * and the delays show it. Deadline 0 is read at 30 us; deadline 1 fires
     * when it is armed at 250.03 ms and is read 150.06 ms late, standing for
     * deadline 2 as well; then deadline 3 at 500.06 ms, 200.09 ms late,
     * standing for 3 to 5; and so on, every deadline a reading or missed. */
    {
        sim_t s = {0, 0, 30 * US, 250 * MS, 0};
        sim_out_t o = simulate(&s, 0, 100 * MS, 10000 * MS);
        CHECK(o.worst_settled_wake > 100 * MS);
        CHECK(o.earliest_wake >= 0);
        CHECK(o.missed > 0);
        CHECK_EQ_INT(o.readings + o.missed, o.armed);
        CHECK_EQ_INT(o.armed, o.last_latest + 1);
    }

    return check_report("deadline");
}

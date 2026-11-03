/*
 * deadline.h - which deadline a sensor reading serves, and which deadline its
 * one-shot timer is armed for next.
 *
 * Deadline k of a sensor is start_ns + k * period_ns, the same grid for the
 * whole run. The sensor arms a one-shot timer at one deadline at a time, so a
 * stall of the host or of the sensor thread can make one reading late but
 * cannot move the deadlines after it: the next timer is armed on the grid
 * again. Plain C with no operating-system calls, so the same logic runs in
 * the unit tests on a laptop and inside the QNX sensor program; the caller
 * supplies every timestamp.
 *
 * Prototype for a hackathon, not a medical device, not for clinical use.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PHILA_DEADLINE_H
#define PHILA_DEADLINE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int64_t start_ns;  /* deadline 0 */
    int64_t period_ns; /* > 0 */
    int64_t early_ns;  /* a pulse up to this long before its deadline counts as on time (>= 0) */
    int64_t armed;     /* the deadline the timer is armed for */
} deadline_t;

/* One reading: the deadline it is late from and the latest deadline passed. */
typedef struct {
    int64_t served; /* the armed deadline; the reading's delay counts from it */
    int64_t latest; /* the latest deadline passed at the pulse, >= served */
    uint32_t missed; /* deadlines passed with no reading of their own: latest - served */
} deadline_reading_t;

/* Deadline 0 at start_ns, armed first. */
void dl_init(deadline_t *d, int64_t start_ns, int64_t period_ns, int64_t early_ns);

/* The time of deadline k. */
int64_t dl_at(const deadline_t *d, int64_t k);

/* The timer's pulse arrived at now. Returns false when it came more than
 * early_ns before the armed deadline: no reading, and the caller arms the
 * same deadline again. Otherwise fills *r and returns true. */
bool dl_pulse(const deadline_t *d, int64_t now, deadline_reading_t *r);

/* After the reading for r->latest, at now: arm the first deadline after
 * r->latest that is still ahead of now. Returns how many deadlines were
 * skipped because they had already passed (counted as missed; normally 0). */
uint32_t dl_advance(deadline_t *d, const deadline_reading_t *r, int64_t now);

#endif

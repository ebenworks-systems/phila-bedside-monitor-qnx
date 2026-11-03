/*
 * deadline.c - a sensor's deadline grid for a one-shot timer (see deadline.h).
 *
 * Prototype for a hackathon, not a medical device, not for clinical use.
 * SPDX-License-Identifier: MIT
 */
#include "phila/deadline.h"

void dl_init(deadline_t *d, int64_t start_ns, int64_t period_ns, int64_t early_ns)
{
    d->start_ns = start_ns;
    d->period_ns = period_ns > 0 ? period_ns : 1;
    d->early_ns = early_ns > 0 ? early_ns : 0;
    d->armed = 0;
}

int64_t dl_at(const deadline_t *d, int64_t k)
{
    return d->start_ns + k * d->period_ns;
}

/* The latest deadline at or before t, or -1 before deadline 0. */
static int64_t latest_at(const deadline_t *d, int64_t t)
{
    if (t < d->start_ns) return -1;
    return (t - d->start_ns) / d->period_ns;
}

bool dl_pulse(const deadline_t *d, int64_t now, deadline_reading_t *r)
{
    if (now < dl_at(d, d->armed) - d->early_ns) return false;
    int64_t latest = latest_at(d, now);
    if (latest < d->armed) latest = d->armed; /* on time but up to early_ns before the deadline */
    r->served = d->armed;
    r->latest = latest;
    r->missed = (uint32_t)(latest - d->armed);
    return true;
}

void dl_advance(deadline_t *d, const deadline_reading_t *r)
{
    d->armed = r->latest + 1;
}

/*
 * rt.h - small helpers for real-time threads on QNX.
 * SPDX-License-Identifier: MIT
 */
#ifndef PHILA_RT_H
#define PHILA_RT_H

#include <pthread.h>
#include <stdint.h>
#include <time.h>

typedef struct {
    int low_priority;  /* priority while the budget is used up */
    int max_repl;      /* pending replenishments allowed */
    int64_t period_ns; /* replenishment period */
    int64_t budget_ns; /* execution budget in each period */
} rt_sporadic_t;

/* CLOCK_MONOTONIC in nanoseconds. */
int64_t rt_now_ns(void);
/* CLOCK_REALTIME in milliseconds since 1970. */
int64_t rt_wall_ms(void);
void rt_ts(struct timespec *ts, int64_t ns);

/* Start a thread with an explicit policy and priority (and sporadic
 * parameters when policy is SCHED_SPORADIC). Returns 0 or an errno value. */
int rt_thread(pthread_t *t, const char *name, void *(*fn)(void *), void *arg, int policy, int prio,
              const rt_sporadic_t *ss);

/* Set the calling thread's policy and priority. Returns 0 or an errno value. */
int rt_self(int policy, int prio);

/* The calling thread's current (effective) priority, which can be higher
 * than its base priority while it serves a message or holds a mutex. */
int rt_current_priority(void);

const char *rt_policy_name(int policy);

#endif

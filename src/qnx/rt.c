/*
 * rt.c - small helpers for real-time threads on QNX.
 * SPDX-License-Identifier: MIT
 */
#include "rt.h"

#include <errno.h>
#include <sched.h>
#include <string.h>

int64_t rt_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

int64_t rt_wall_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

void rt_ts(struct timespec *ts, int64_t ns)
{
    ts->tv_sec = (time_t)(ns / 1000000000LL);
    ts->tv_nsec = (long)(ns % 1000000000LL);
}

static void fill(struct sched_param *sp, int policy, int prio, const rt_sporadic_t *ss)
{
    memset(sp, 0, sizeof *sp);
    sp->sched_priority = prio;
    if (policy == SCHED_SPORADIC && ss != NULL) {
        sp->sched_ss_low_priority = ss->low_priority;
        sp->sched_ss_max_repl = ss->max_repl;
        rt_ts(&sp->sched_ss_repl_period, ss->period_ns);
        rt_ts(&sp->sched_ss_init_budget, ss->budget_ns);
    }
}

int rt_thread(pthread_t *t, const char *name, void *(*fn)(void *), void *arg, int policy, int prio,
              const rt_sporadic_t *ss)
{
    pthread_attr_t a;
    struct sched_param sp;
    int rc = pthread_attr_init(&a);
    if (rc != 0) return rc;
    fill(&sp, policy, prio, ss);
    if ((rc = pthread_attr_setinheritsched(&a, PTHREAD_EXPLICIT_SCHED)) == 0 &&
        (rc = pthread_attr_setschedpolicy(&a, policy)) == 0 && (rc = pthread_attr_setschedparam(&a, &sp)) == 0)
        rc = pthread_create(t, &a, fn, arg);
    pthread_attr_destroy(&a);
    if (rc == 0 && name != NULL) pthread_setname_np(*t, name);
    return rc;
}

int rt_self(int policy, int prio)
{
    struct sched_param sp;
    fill(&sp, policy, prio, NULL);
    return pthread_setschedparam(pthread_self(), policy, &sp);
}

int rt_current_priority(void)
{
    struct sched_param sp;
    int policy;
    if (pthread_getschedparam(pthread_self(), &policy, &sp) != 0) return -1;
    return sp.sched_curpriority;
}

const char *rt_policy_name(int policy)
{
    switch (policy) {
    case SCHED_FIFO: return "fifo";
    case SCHED_RR: return "rr";
    case SCHED_SPORADIC: return "sporadic";
    case SCHED_OTHER: return "other";
    default: return "?";
    }
}

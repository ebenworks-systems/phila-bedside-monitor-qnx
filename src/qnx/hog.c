/*
 * hog.c - a CPU hog for the latency tests: N threads that spin at a chosen
 * priority and policy for a set time.
 *
 *   phila-hog [--threads N] [--prio P] [--policy fifo|rr] [--seconds S]
 *
 * With FIFO at a priority above the system's own services, the hog stops
 * everything below it on the CPUs it holds, which is the point: only work
 * designed to sit above it keeps running.
 *
 * SPDX-License-Identifier: MIT
 */
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rt.h"

static int64_t g_end_ns;
static volatile unsigned long long g_sink;

static void *spin(void *arg)
{
    (void)arg;
    unsigned long long x = 0;
    while (rt_now_ns() < g_end_ns)
        for (int i = 0; i < 100000; i++) x += (unsigned long long)i * 2654435761u;
    g_sink = x;
    return NULL;
}

int main(int argc, char **argv)
{
    int threads = 2, prio = 35, policy = SCHED_FIFO;
    double seconds = 30;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) threads = atoi(argv[++i]);
        else if (strcmp(argv[i], "--prio") == 0 && i + 1 < argc) prio = atoi(argv[++i]);
        else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) seconds = atof(argv[++i]);
        else if (strcmp(argv[i], "--policy") == 0 && i + 1 < argc) {
            const char *p = argv[++i];
            if (strcmp(p, "fifo") == 0) policy = SCHED_FIFO;
            else if (strcmp(p, "rr") == 0) policy = SCHED_RR;
            else goto bad;
        } else {
        bad:
            fprintf(stderr, "usage: phila-hog [--threads N] [--prio P] [--policy fifo|rr] [--seconds S]\n");
            return 2;
        }
    }
    if (threads < 1 || threads > 64 || prio < 1 || prio > 254 || seconds <= 0) {
        fprintf(stderr, "phila-hog: out of range\n");
        return 2;
    }
    printf("phila-hog: %d threads, %s priority %d, %.0f s\n", threads, rt_policy_name(policy), prio, seconds);
    fflush(stdout);
    /* The main thread waits above the spinners so it can always finish. */
    rt_self(SCHED_FIFO, prio < 254 ? prio + 1 : prio);
    g_end_ns = rt_now_ns() + (int64_t)(seconds * 1e9);
    pthread_t t[64];
    for (int i = 0; i < threads; i++) {
        int rc = rt_thread(&t[i], "hog", spin, NULL, policy, prio, NULL);
        if (rc != 0) {
            fprintf(stderr, "phila-hog: thread: %s\n", strerror(rc));
            return 1;
        }
    }
    for (int i = 0; i < threads; i++) pthread_join(t[i], NULL);
    printf("phila-hog: done\n");
    return 0;
}

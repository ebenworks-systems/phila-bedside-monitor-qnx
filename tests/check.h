/*
 * check.h - a very small test harness: no dependencies, runs the same on a
 * laptop and on QNX.
 * SPDX-License-Identifier: MIT
 */
#ifndef PHILA_CHECK_H
#define PHILA_CHECK_H

#include <stdio.h>
#include <string.h>

static int check_failures = 0;
static int check_count = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        check_count++;                                                                 \
        if (!(cond)) {                                                                 \
            check_failures++;                                                          \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond);   \
        }                                                                              \
    } while (0)

#define CHECK_EQ_INT(a, b)                                                                              \
    do {                                                                                                \
        long long check_a_ = (long long)(a), check_b_ = (long long)(b);                                 \
        check_count++;                                                                                  \
        if (check_a_ != check_b_) {                                                                     \
            check_failures++;                                                                           \
            fprintf(stderr, "%s:%d: %s == %lld, expected %lld\n", __FILE__, __LINE__, #a, check_a_, check_b_); \
        }                                                                                               \
    } while (0)

#define CHECK_EQ_STR(a, b)                                                                              \
    do {                                                                                                \
        const char *check_a_ = (a), *check_b_ = (b);                                                    \
        check_count++;                                                                                  \
        if (strcmp(check_a_, check_b_) != 0) {                                                          \
            check_failures++;                                                                           \
            fprintf(stderr, "%s:%d: %s == \"%s\", expected \"%s\"\n", __FILE__, __LINE__, #a, check_a_, check_b_); \
        }                                                                                               \
    } while (0)

static int check_report(const char *name)
{
    printf("%s: %d checks, %d failed\n", name, check_count, check_failures);
    return check_failures == 0 ? 0 : 1;
}

#endif

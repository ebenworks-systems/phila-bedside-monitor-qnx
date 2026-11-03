/*
 * test_record.c - the append-only record: every line verifies, and any
 * change, removal or reordering is found at the first broken line.
 * SPDX-License-Identifier: MIT
 */
#include "check.h"
#include "phila/record.h"

#define LINES 6

static char lines[LINES][REC_LINE_MAX];
static size_t lens[LINES];

static void build(void)
{
    rec_chain_t chain;
    rec_chain_init(&chain);
    for (int i = 0; i < LINES; i++) {
        rec_entry_t e;
        memset(&e, 0, sizeof e);
        e.mono_ns = 1000000000LL * (i + 1);
        e.wall_ms = 1793000000000LL + 1000 * i; /* a fixed, fictional time */
        snprintf(e.bed, sizeof e.bed, "BED-0%d", 1 + i % 4);
        if (i % 3 == 0) {
            e.kind = REC_OBS;
            news2_obs_t o = {22, 93, SPO2_SCALE_1, false, 105, 115, ACVPU_ALERT, 386};
            e.obs = o;
            news2_assess(&e.obs, &e.res);
        } else if (i % 3 == 1) {
            e.kind = REC_ALARM;
            e.level = NEWS2_RISK_MEDIUM;
            e.technical = i == 4;
            snprintf(e.reason, sizeof e.reason, "news2=6");
            e.latency_us = 120 + (uint32_t)i;
        } else {
            e.kind = REC_EVENT;
            snprintf(e.text, sizeof e.text, "nurse acknowledged the alarm");
        }
        int n = rec_format(&chain, &e, lines[i], sizeof lines[i]);
        CHECK(n > 0);
        CHECK(lines[i][n - 1] == '\n');
        lens[i] = (size_t)n - 1; /* without the newline */
    }
}

/* Verify a sequence of line indexes; returns the index (into order) of the
 * first failure and its verdict, or -1. */
static int verify(const int *order, int n, rec_verify_t *why)
{
    rec_chain_t chain;
    rec_chain_init(&chain);
    for (int i = 0; i < n; i++) {
        rec_verify_t v = rec_verify_line(&chain, lines[order[i]], lens[order[i]]);
        if (v != REC_V_OK) {
            *why = v;
            return i;
        }
    }
    return -1;
}

int main(void)
{
    build();
    rec_verify_t why = REC_V_OK;
    int all[LINES] = {0, 1, 2, 3, 4, 5};
    CHECK_EQ_INT(verify(all, LINES, &why), -1);

    /* The first line is exactly the documented shape. */
    {
        static const char head[] = "{\"seq\":1,\"mono_ns\":1000000000,\"time\":\"2026-10-26T07:33:20.000Z\",\"bed\":\"BED-01\"";
        CHECK(strncmp(lines[0], head, sizeof head - 1) == 0);
    }
    CHECK(strstr(lines[0], "\"kind\":\"obs\",\"rr\":22,\"spo2\":93,\"spo2_scale\":1,\"o2\":0,\"sbp\":105,\"hr\":115,"
                           "\"acvpu\":\"A\",\"temp_dc\":386,\"news2\":8,\"red\":0,\"risk\":\"high\"") != NULL);
    CHECK(strstr(lines[0], "\"prev\":\"0000000000000000000000000000000000000000000000000000000000000000\"") != NULL);
    CHECK(strstr(lines[1], "\"kind\":\"alarm\",\"level\":\"medium\",\"reason\":\"news2=6\",\"latency_us\":121") != NULL);
    CHECK(strstr(lines[4], "\"kind\":\"alarm\",\"level\":\"technical\",") != NULL);

    /* A changed digit breaks that line's hash. */
    char saved[REC_LINE_MAX];
    memcpy(saved, lines[3], sizeof saved);
    char *p = strstr(lines[3], "\"rr\":22");
    CHECK(p != NULL);
    p[6] = '1'; /* rr 22 -> 21 */
    CHECK_EQ_INT(verify(all, LINES, &why), 3);
    CHECK_EQ_INT(why, REC_V_HASH);
    memcpy(lines[3], saved, sizeof saved);

    /* A changed hash breaks the next line's "prev". */
    p = strstr(lines[2], "\"hash\":\"");
    CHECK(p != NULL);
    p[8] = p[8] == 'a' ? 'b' : 'a';
    CHECK_EQ_INT(verify(all, LINES, &why), 2);
    CHECK_EQ_INT(why, REC_V_HASH);
    build(); /* clean lines again */

    /* A removed line breaks the sequence. */
    int removed[LINES - 1] = {0, 1, 3, 4, 5};
    CHECK_EQ_INT(verify(removed, LINES - 1, &why), 2);
    CHECK_EQ_INT(why, REC_V_SEQ);

    /* Swapped lines break the sequence. */
    int swapped[LINES] = {0, 2, 1, 3, 4, 5};
    CHECK_EQ_INT(verify(swapped, LINES, &why), 1);
    CHECK_EQ_INT(why, REC_V_SEQ);

    /* A whole record rebuilt from line 4 on, with honest sequence numbers but
     * a fresh chain, fails at its first line on "prev". */
    {
        rec_chain_t chain;
        rec_chain_init(&chain);
        chain.next_seq = 4;
        rec_entry_t e;
        memset(&e, 0, sizeof e);
        e.kind = REC_EVENT;
        e.wall_ms = 1793000000000LL;
        snprintf(e.bed, sizeof e.bed, "BED-01");
        snprintf(e.text, sizeof e.text, "forged");
        char forged[REC_LINE_MAX];
        int n = rec_format(&chain, &e, forged, sizeof forged);
        CHECK(n > 0);
        rec_chain_t v;
        rec_chain_init(&v);
        for (int i = 0; i < 3; i++) CHECK_EQ_INT(rec_verify_line(&v, lines[i], lens[i]), REC_V_OK);
        CHECK_EQ_INT(rec_verify_line(&v, forged, (size_t)n - 1), REC_V_PREV);
    }

    /* Not a record line at all. */
    {
        rec_chain_t v;
        rec_chain_init(&v);
        CHECK_EQ_INT(rec_verify_line(&v, "hello", 5), REC_V_SHAPE);
        CHECK_EQ_INT(rec_verify_line(&v, "{}", 2), REC_V_SHAPE);
    }

    /* Bad input is refused rather than written. */
    {
        rec_chain_t chain;
        rec_chain_init(&chain);
        rec_entry_t e;
        memset(&e, 0, sizeof e);
        e.kind = REC_EVENT;
        char buf[REC_LINE_MAX];
        snprintf(e.bed, sizeof e.bed, "BED 1"); /* space */
        snprintf(e.text, sizeof e.text, "ok");
        CHECK_EQ_INT(rec_format(&chain, &e, buf, sizeof buf), -1);
        snprintf(e.bed, sizeof e.bed, "BED-01");
        snprintf(e.text, sizeof e.text, "a \"quoted\" word");
        CHECK_EQ_INT(rec_format(&chain, &e, buf, sizeof buf), -1);
        snprintf(e.text, sizeof e.text, "line\nbreak");
        CHECK_EQ_INT(rec_format(&chain, &e, buf, sizeof buf), -1);
        CHECK_EQ_INT(chain.next_seq, 1); /* nothing was consumed */
        CHECK(!rec_bed_ok(""));
        CHECK(!rec_bed_ok("A-VERY-LONG-BED-NAME"));
        CHECK(rec_bed_ok("WARD3_BED-12"));
    }

    char when[32];
    rec_iso8601(0, when);
    CHECK_EQ_STR(when, "1970-01-01T00:00:00.000Z");
    rec_iso8601(1793000000123LL, when);
    CHECK_EQ_STR(when, "2026-10-26T07:33:20.123Z");
    return check_report("record");
}

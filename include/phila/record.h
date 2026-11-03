/*
 * record.h - the append-only record.
 *
 * Every observation, alarm and event becomes one JSON line. Each line carries
 * the SHA-256 of the line before it ("prev") and its own hash ("hash"), so the
 * file forms a chain: changing, removing or reordering any line breaks every
 * hash after it, and rec_verify_line() finds the first broken one.
 *
 * Keys always appear in the same order and values are integers or plain
 * strings, so the hash is reproducible on any machine without a JSON library.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PHILA_RECORD_H
#define PHILA_RECORD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "phila/news2.h"
#include "phila/sha256.h"

#ifdef __cplusplus
extern "C" {
#endif

#define REC_BED_MAX 16     /* "BED-01", letters, digits, '-' and '_' only */
#define REC_TEXT_MAX 96
#define REC_LINE_MAX 640

typedef enum { REC_OBS = 0, REC_ALARM, REC_EVENT } rec_kind_t;

typedef struct {
    rec_kind_t kind;
    int64_t mono_ns;          /* CLOCK_MONOTONIC when the reading was taken */
    int64_t wall_ms;          /* CLOCK_REALTIME, milliseconds since 1970 */
    char bed[REC_BED_MAX];
    /* REC_OBS */
    news2_obs_t obs;
    news2_result_t res;
    /* REC_ALARM */
    news2_risk_t level;
    char reason[REC_TEXT_MAX]; /* "red:spo2", "aggregate>=7", "sensor-lost:bp" ... */
    uint32_t latency_us;       /* reading taken -> alarm raised */
    /* REC_EVENT */
    char text[REC_TEXT_MAX];
} rec_entry_t;

typedef struct {
    uint64_t next_seq;
    char prev_hex[SHA256_HEX_LEN + 1];
} rec_chain_t;

/* Start a new chain: seq 1, prev = 64 zeros. */
void rec_chain_init(rec_chain_t *chain);

/* True if s is a usable bed id (1 to 15 characters of [A-Za-z0-9_-]). */
bool rec_bed_ok(const char *s);

/* Format e as one line ending in '\n', chained to *chain, and advance the
 * chain. Returns the line length, or -1 if it does not fit or e is invalid. */
int rec_format(rec_chain_t *chain, const rec_entry_t *e, char *buf, size_t cap);

typedef enum {
    REC_V_OK = 0,
    REC_V_SHAPE,    /* not one of our lines */
    REC_V_SEQ,      /* sequence number is not the next one */
    REC_V_PREV,     /* "prev" does not match the hash of the line before */
    REC_V_HASH      /* the line's own hash does not match its content */
} rec_verify_t;

/* Check one line (without its '\n') against the chain and advance the chain
 * when it is good. */
rec_verify_t rec_verify_line(rec_chain_t *chain, const char *line, size_t len);
const char *rec_verify_name(rec_verify_t v);

/* Write the ISO 8601 UTC form of wall_ms ("2026-11-03T08:41:07.123Z"). */
void rec_iso8601(int64_t wall_ms, char out[32]);

#ifdef __cplusplus
}
#endif
#endif /* PHILA_RECORD_H */

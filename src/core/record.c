/*
 * record.c - append-only record lines with a SHA-256 hash chain.
 * SPDX-License-Identifier: MIT
 */
#include "phila/record.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char HASH_KEY[] = ",\"hash\":\"";

void rec_chain_init(rec_chain_t *chain)
{
    chain->next_seq = 1;
    memset(chain->prev_hex, '0', SHA256_HEX_LEN);
    chain->prev_hex[SHA256_HEX_LEN] = '\0';
}

bool rec_bed_ok(const char *s)
{
    size_t n = 0;
    if (s == NULL) return false;
    for (; s[n] != '\0'; n++) {
        char c = s[n];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                  c == '_';
        if (!ok || n >= REC_BED_MAX - 1) return false;
    }
    return n > 0;
}

/* Text fields hold printable ASCII only; quotes and backslashes are refused
 * rather than escaped, which keeps every line trivially canonical. */
static bool text_ok(const char *s)
{
    for (size_t i = 0; s[i] != '\0'; i++) {
        unsigned char c = (unsigned char)s[i];
        if (i >= REC_TEXT_MAX - 1 || c < 0x20 || c > 0x7e || c == '"' || c == '\\') return false;
    }
    return true;
}

void rec_iso8601(int64_t wall_ms, char out[32])
{
    time_t secs = (time_t)(wall_ms / 1000);
    int ms = (int)(wall_ms % 1000);
    if (ms < 0) {
        ms += 1000;
        secs -= 1;
    }
    struct tm tm;
    if (gmtime_r(&secs, &tm) == NULL) {
        snprintf(out, 32, "invalid");
        return;
    }
    /* Each field is reduced to its width, so the text is always 24 characters. */
    snprintf(out, 32, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", (unsigned)(tm.tm_year + 1900) % 10000u,
             (unsigned)(tm.tm_mon + 1) % 100u, (unsigned)tm.tm_mday % 100u, (unsigned)tm.tm_hour % 100u,
             (unsigned)tm.tm_min % 100u, (unsigned)tm.tm_sec % 100u, (unsigned)ms % 1000u);
}

static const char *kind_name(rec_kind_t k)
{
    switch (k) {
    case REC_OBS: return "obs";
    case REC_ALARM: return "alarm";
    case REC_EVENT: return "event";
    }
    return "?";
}

int rec_format(rec_chain_t *chain, const rec_entry_t *e, char *buf, size_t cap)
{
    if (chain == NULL || e == NULL || buf == NULL || cap < 2 * SHA256_HEX_LEN) return -1;
    if (!rec_bed_ok(e->bed)) return -1;

    char when[32];
    rec_iso8601(e->wall_ms, when);
    int n = snprintf(buf, cap, "{\"seq\":%" PRIu64 ",\"mono_ns\":%" PRId64 ",\"time\":\"%s\",\"bed\":\"%s\",\"kind\":\"%s\"",
                     chain->next_seq, e->mono_ns, when, e->bed, kind_name(e->kind));
    if (n < 0 || (size_t)n >= cap) return -1;
    size_t len = (size_t)n;

    switch (e->kind) {
    case REC_OBS: {
        const news2_obs_t *o = &e->obs;
        const news2_result_t *r = &e->res;
        n = snprintf(buf + len, cap - len,
                     ",\"rr\":%d,\"spo2\":%d,\"spo2_scale\":%d,\"o2\":%d,\"sbp\":%d,\"hr\":%d,\"acvpu\":\"%s\","
                     "\"temp_dc\":%d,\"news2\":%u,\"red\":%d,\"risk\":\"%s\"",
                     o->resp_rate, o->spo2, (int)o->spo2_scale, o->on_oxygen ? 1 : 0, o->systolic, o->pulse,
                     news2_acvpu_letter(o->consciousness), o->temp_dc, (unsigned)r->total, r->any_red ? 1 : 0,
                     news2_risk_name(r->risk));
        break;
    }
    case REC_ALARM:
        if (!text_ok(e->reason)) return -1;
        n = snprintf(buf + len, cap - len, ",\"level\":\"%s\",\"reason\":\"%s\",\"latency_us\":%" PRIu32,
                     news2_risk_name(e->level), e->reason, e->latency_us);
        break;
    case REC_EVENT:
        if (!text_ok(e->text)) return -1;
        n = snprintf(buf + len, cap - len, ",\"text\":\"%s\"", e->text);
        break;
    default:
        return -1;
    }
    if (n < 0 || (size_t)n >= cap - len) return -1;
    len += (size_t)n;

    n = snprintf(buf + len, cap - len, ",\"prev\":\"%s\"}", chain->prev_hex);
    if (n < 0 || (size_t)n >= cap - len) return -1;
    len += (size_t)n;

    /* The hash covers the line as it stands now: every field and "prev". */
    uint8_t d[SHA256_DIGEST_LEN];
    char hex[SHA256_HEX_LEN + 1];
    sha256(buf, len, d);
    sha256_hex(d, hex);

    /* Replace the closing brace with ,"hash":"<hex>"} and a newline. */
    size_t need = (len - 1) + (sizeof HASH_KEY - 1) + SHA256_HEX_LEN + 2 + 1;
    if (need >= cap) return -1;
    len -= 1;
    memcpy(buf + len, HASH_KEY, sizeof HASH_KEY - 1);
    len += sizeof HASH_KEY - 1;
    memcpy(buf + len, hex, SHA256_HEX_LEN);
    len += SHA256_HEX_LEN;
    buf[len++] = '"';
    buf[len++] = '}';
    buf[len++] = '\n';
    buf[len] = '\0';

    memcpy(chain->prev_hex, hex, sizeof hex);
    chain->next_seq++;
    return (int)len;
}

rec_verify_t rec_verify_line(rec_chain_t *chain, const char *line, size_t len)
{
    /* Shape: {"seq":N, ... ,"prev":"<64>","hash":"<64>"} */
    const size_t tail = (sizeof HASH_KEY - 1) + SHA256_HEX_LEN + 2;
    if (len < 16 + tail || len >= REC_LINE_MAX || line[0] != '{' || line[len - 1] != '}' ||
        line[len - 2] != '"')
        return REC_V_SHAPE;
    size_t hash_at = len - tail;
    if (memcmp(line + hash_at, HASH_KEY, sizeof HASH_KEY - 1) != 0) return REC_V_SHAPE;
    const char *hash_hex = line + hash_at + sizeof HASH_KEY - 1;

    /* seq */
    static const char SEQ_KEY[] = "{\"seq\":";
    if (strncmp(line, SEQ_KEY, sizeof SEQ_KEY - 1) != 0) return REC_V_SHAPE;
    uint64_t seq = 0;
    size_t i = sizeof SEQ_KEY - 1;
    if (i >= len || line[i] < '0' || line[i] > '9') return REC_V_SHAPE;
    while (i < len && line[i] >= '0' && line[i] <= '9') seq = seq * 10u + (uint64_t)(line[i++] - '0');
    if (seq != chain->next_seq) return REC_V_SEQ;

    /* prev: the 64 hex characters just before ","hash" */
    static const char PREV_KEY[] = ",\"prev\":\"";
    size_t prev_key_at = hash_at - (sizeof PREV_KEY - 1) - SHA256_HEX_LEN - 1;
    if (hash_at < (sizeof PREV_KEY - 1) + SHA256_HEX_LEN + 1 ||
        memcmp(line + prev_key_at, PREV_KEY, sizeof PREV_KEY - 1) != 0)
        return REC_V_SHAPE;
    if (memcmp(line + prev_key_at + sizeof PREV_KEY - 1, chain->prev_hex, SHA256_HEX_LEN) != 0) return REC_V_PREV;

    /* hash over the body: the line up to the hash key, closed with '}' */
    char body[REC_LINE_MAX];
    memcpy(body, line, hash_at);
    body[hash_at] = '}';
    uint8_t d[SHA256_DIGEST_LEN];
    char hex[SHA256_HEX_LEN + 1];
    sha256(body, hash_at + 1, d);
    sha256_hex(d, hex);
    if (memcmp(hex, hash_hex, SHA256_HEX_LEN) != 0) return REC_V_HASH;

    memcpy(chain->prev_hex, hex, sizeof hex);
    chain->next_seq++;
    return REC_V_OK;
}

const char *rec_verify_name(rec_verify_t v)
{
    switch (v) {
    case REC_V_OK: return "ok";
    case REC_V_SHAPE: return "not a record line";
    case REC_V_SEQ: return "sequence number out of order";
    case REC_V_PREV: return "previous hash does not match";
    case REC_V_HASH: return "line hash does not match its content";
    }
    return "unknown";
}

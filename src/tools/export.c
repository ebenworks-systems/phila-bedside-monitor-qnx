/*
 * export.c - check a record's hash chain and write it out as one plain JSON
 * document that another system can read.
 *
 *   phila-export RECORD.jsonl [OUT.json]     (OUT defaults to standard output)
 *
 * Exit status: 0 exported; 3 the chain is broken (nothing is written, and
 * the first broken line is named); 1 any other error.
 *
 * Builds unchanged on QNX and on a laptop: it uses only the C library.
 * Prototype for a hackathon, not a medical device, not for clinical use.
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "phila/record.h"

#define MAX_PAIRS 32

typedef struct {
    char key[24];
    char val[128];
    int is_str;
} pair_t;

/* Our lines hold only integers and strings without escapes, in a fixed
 * order, so a small reader is enough. Returns the number of pairs or -1. */
static int parse(const char *s, pair_t *p, int cap)
{
    int n = 0;
    if (*s++ != '{') return -1;
    while (*s && *s != '}') {
        if (n >= cap || *s++ != '"') return -1;
        size_t k = strcspn(s, "\"");
        if (k >= sizeof p[n].key || s[k] != '"' || s[k + 1] != ':') return -1;
        memcpy(p[n].key, s, k);
        p[n].key[k] = '\0';
        s += k + 2;
        size_t v;
        if (*s == '"') {
            s++;
            v = strcspn(s, "\"");
            if (s[v] != '"') return -1;
            p[n].is_str = 1;
        } else {
            v = strspn(s, "-0123456789");
            if (v == 0) return -1;
            p[n].is_str = 0;
        }
        if (v >= sizeof p[n].val) return -1;
        memcpy(p[n].val, s, v);
        p[n].val[v] = '\0';
        s += v + (p[n].is_str ? 1 : 0);
        n++;
        if (*s == ',') s++;
    }
    return *s == '}' ? n : -1;
}

static const char *val(const pair_t *p, int n, const char *key)
{
    for (int i = 0; i < n; i++)
        if (strcmp(p[i].key, key) == 0) return p[i].val;
    return "";
}

static void emit(FILE *out, const pair_t *p, int n, const char *const *keys)
{
    int first = 1;
    fputs("    {", out);
    for (const char *const *k = keys; *k; k++) {
        for (int i = 0; i < n; i++) {
            if (strcmp(p[i].key, *k) != 0) continue;
            if (strcmp(*k, "temp_dc") == 0) {
                long dc = strtol(p[i].val, NULL, 10);
                fprintf(out, "%s\"temp_c\": %ld.%ld", first ? "" : ", ", dc / 10, labs(dc % 10));
            } else if (p[i].is_str) {
                fprintf(out, "%s\"%s\": \"%s\"", first ? "" : ", ", p[i].key, p[i].val);
            } else {
                fprintf(out, "%s\"%s\": %s", first ? "" : ", ", p[i].key, p[i].val);
            }
            first = 0;
        }
    }
    fputs("}", out);
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: phila-export RECORD.jsonl [OUT.json]\n");
        return 1;
    }
    FILE *in = fopen(argv[1], "r");
    if (in == NULL) {
        perror(argv[1]);
        return 1;
    }

    /* Pass 1: verify the whole chain before writing anything. */
    rec_chain_t chain;
    rec_chain_init(&chain);
    char line[REC_LINE_MAX + 2];
    unsigned long lines = 0, obs = 0, alarms = 0, events = 0;
    char first_hash[SHA256_HEX_LEN + 1] = "";
    while (fgets(line, sizeof line, in) != NULL) {
        size_t len = strcspn(line, "\n");
        lines++;
        if (line[len] != '\n') {
            fprintf(stderr, "phila-export: line %lu: too long or not terminated\n", lines);
            return 3;
        }
        rec_verify_t v = rec_verify_line(&chain, line, len);
        if (v != REC_V_OK) {
            fprintf(stderr, "phila-export: line %lu: %s; nothing exported\n", lines, rec_verify_name(v));
            return 3;
        }
        if (lines == 1) memcpy(first_hash, chain.prev_hex, sizeof first_hash);
        if (strstr(line, "\"kind\":\"obs\"")) obs++;
        else if (strstr(line, "\"kind\":\"alarm\"")) alarms++;
        else events++;
    }
    if (lines == 0) {
        fprintf(stderr, "phila-export: %s is empty\n", argv[1]);
        return 3;
    }

    /* A file is written under a temporary name and renamed once it is complete, so a failure
     * never leaves a half document under the real name. */
    char tmp_path[4096] = "";
    if (argc == 3 && snprintf(tmp_path, sizeof tmp_path, "%s.tmp", argv[2]) >= (int)sizeof tmp_path) {
        fprintf(stderr, "phila-export: output path too long\n");
        return 1;
    }
    FILE *out = argc == 3 ? fopen(tmp_path, "w") : stdout;
    if (out == NULL) {
        perror(argv[2]);
        return 1;
    }
    fprintf(out, "{\n  \"format\": \"phila-bedside-export/1\",\n");
    fprintf(out, "  \"notice\": \"Prototype for a hackathon, not a medical device, not for clinical use.\",\n");
    fprintf(out, "  \"score\": \"NEWS2, Royal College of Physicians, 2017\",\n");
    fprintf(out, "  \"record\": {\"lines\": %lu, \"verified\": true, \"first_hash\": \"%s\", \"last_hash\": \"%s\"},\n",
                 lines, first_hash, chain.prev_hex);

    static const char *const OBS_KEYS[] = {"seq", "time", "bed", "rr", "spo2", "spo2_scale", "o2", "sbp", "hr",
                                           "acvpu", "temp_dc", "news2", "risk", NULL};
    static const char *const ALARM_KEYS[] = {"seq", "time", "bed", "level", "reason", "latency_us", NULL};
    static const char *const EVENT_KEYS[] = {"seq", "time", "bed", "text", NULL};
    const char *const sections[3] = {"obs", "alarm", "event"};
    const char *const names[3] = {"observations", "alarms", "events"};
    const char *const *keys[3] = {OBS_KEYS, ALARM_KEYS, EVENT_KEYS};
    const unsigned long counts[3] = {obs, alarms, events};

    pair_t p[MAX_PAIRS];
    for (int sec = 0; sec < 3; sec++) {
        fprintf(out, "  \"%s\": [", names[sec]);
        rewind(in);
        unsigned long written = 0, seen = 0;
        /* Only the lines pass 1 verified: the monitor may still be appending to the file. */
        while (seen < lines && fgets(line, sizeof line, in) != NULL) {
            seen++;
            line[strcspn(line, "\n")] = '\0';
            int n = parse(line, p, MAX_PAIRS);
            if (n < 0) {
                fprintf(stderr, "phila-export: cannot read a verified line; stopping\n");
                if (out != stdout) {
                    fclose(out);
                    remove(tmp_path);
                }
                return 1;
            }
            if (strcmp(val(p, n, "kind"), sections[sec]) != 0) continue;
            fputs(written++ ? ",\n" : "\n", out);
            emit(out, p, n, keys[sec]);
        }
        fprintf(out, "%s]%s\n", counts[sec] ? "\n  " : "", sec < 2 ? "," : "");
    }
    fprintf(out, "}\n");
    if (out != stdout && (fclose(out) != 0 || rename(tmp_path, argv[2]) != 0)) {
        perror(argv[2]);
        remove(tmp_path);
        return 1;
    }
    fprintf(stderr, "phila-export: %lu lines verified; %lu observations, %lu alarms, %lu events\n", lines, obs, alarms,
            events);
    return 0;
}

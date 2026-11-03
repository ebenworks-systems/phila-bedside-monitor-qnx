/*
 * display.c - a nurse's view of the ward in a terminal. Reads
 * /dev/phila/status and /dev/phila/alarms once a second; it holds no state
 * of its own and could be stopped or restarted at any time without touching
 * the monitor.
 *
 *   phila-display [--once] [--interval MS] [--no-color]
 *
 * Colour is text colour only, so the view reads on a light terminal.
 *
 * Prototype for a hackathon, not a medical device, not for clinical use.
 * SPDX-License-Identifier: MIT
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

static int g_color = 1;

static const char *c(const char *code)
{
    return g_color ? code : "";
}
#define RESET c("\033[0m")
#define BOLD c("\033[1m")
#define DIM c("\033[2m")
#define RED c("\033[1;31m")
#define AMBER c("\033[1;33m")
#define ORANGE c("\033[35m")
#define BLUE c("\033[1;34m")
#define GREEN c("\033[32m")

static size_t slurp(const char *path, char *buf, size_t cap)
{
    int fd = open(path, O_RDONLY);
    if (fd == -1) return 0;
    size_t len = 0;
    ssize_t n;
    while (len + 1 < cap && (n = read(fd, buf + len, cap - 1 - len)) > 0) len += (size_t)n;
    close(fd);
    buf[len] = '\0';
    return len;
}

/* Copy the value of key=... from a line of space-separated pairs. The last
 * key on a line ("reason=", "tech=") takes the rest of the line. */
static const char *get(const char *line, const char *key, char *out, size_t cap, int rest)
{
    size_t klen = strlen(key);
    const char *p = line;
    out[0] = '\0';
    while ((p = strstr(p, key)) != NULL) {
        if ((p == line || p[-1] == ' ') && p[klen] == '=') {
            p += klen + 1;
            size_t n = rest ? strcspn(p, "\n") : strcspn(p, " \n");
            if (n >= cap) n = cap - 1;
            memcpy(out, p, n);
            out[n] = '\0';
            return out;
        }
        p += klen;
    }
    return out;
}

static const char *level_color(const char *level)
{
    if (strcmp(level, "high") == 0) return RED;
    if (strcmp(level, "medium") == 0) return AMBER;
    if (strcmp(level, "low-medium") == 0) return ORANGE;
    if (strcmp(level, "technical") == 0) return BLUE;
    return GREEN;
}

static void show(void)
{
    static char status[8192], alarms[16384];
    if (slurp("/dev/phila/status", status, sizeof status) == 0) {
        printf("phila-display: /dev/phila/status is not there; is phila-monitord running?\n");
        return;
    }
    slurp("/dev/phila/alarms", alarms, sizeof alarms);

    struct utsname u;
    char osname[96] = "QNX";
    if (uname(&u) == 0) snprintf(osname, sizeof osname, "%.40s %.40s", u.sysname, u.release);
    char v[96], up[32], mode[32];
    const char *first = status;
    get(first, "uptime_s", up, sizeof up, 0);
    get(first, "mode", mode, sizeof mode, 0);

    printf("%sPhila Health Ecosystem bedside monitor prototype%s   %s%s on %s, %s priorities, up %s s%s\n", BOLD, RESET,
           DIM, "running", osname, mode, up, RESET);
    printf("%sPrototype for a hackathon, not a medical device, not for clinical use. Fictional patients.%s\n\n", DIM,
           RESET);
    printf("%s%-8s %5s %5s %4s %5s %6s %5s %4s %6s  %-11s %s%s\n", BOLD, "Bed", "Pulse", "SpO2", "RR", "SBP", "Temp",
           "ACVPU", "O2", "NEWS2", "Alarm", "Why", RESET);

    char line[512];
    const char *p = strchr(status, '\n');
    while (p != NULL && p[1] != '\0') {
        const char *start = p + 1;
        p = strchr(start, '\n');
        size_t n = p ? (size_t)(p - start) : strlen(start);
        if (n >= sizeof line) n = sizeof line - 1;
        memcpy(line, start, n);
        line[n] = '\0';
        if (strncmp(line, "bed=", 4) != 0) continue;

        char bed[16], hr[8], spo2[8], rr[8], sbp[8], temp[8], acvpu[4], o2[4], news2[8], alarm[16], tech[64];
        get(line, "bed", bed, sizeof bed, 0);
        get(line, "hr", hr, sizeof hr, 0);
        get(line, "spo2", spo2, sizeof spo2, 0);
        get(line, "rr", rr, sizeof rr, 0);
        get(line, "sbp", sbp, sizeof sbp, 0);
        get(line, "temp", temp, sizeof temp, 0);
        get(line, "acvpu", acvpu, sizeof acvpu, 0);
        get(line, "o2", o2, sizeof o2, 0);
        get(line, "news2", news2, sizeof news2, 0);
        get(line, "alarm", alarm, sizeof alarm, 0);
        get(line, "tech", tech, sizeof tech, 1);

        char tbuf[12] = "-";
        if (strcmp(temp, "-") != 0) snprintf(tbuf, sizeof tbuf, "%.1f", atoi(temp) / 10.0);

        /* The reason for this bed's clinical alarm, from the board. */
        char why[96] = "";
        char key[64];
        snprintf(key, sizeof key, "active bed=%.15s level=", bed);
        const char *a = strstr(alarms, key);
        if (a != NULL && strncmp(a + strlen(key), "technical", 9) != 0) get(a, "reason", why, sizeof why, 1);

        const char *alarm_text = strcmp(alarm, "none") == 0 ? "-" : alarm;
        printf("%-8s %5s %5s %4s %5s %6s %5s %4s %6s  %s%-11s%s %s%s%s", bed, hr, spo2, rr, sbp, tbuf, acvpu,
               strcmp(o2, "1") == 0 ? "yes" : "air", news2, level_color(alarm), alarm_text, RESET, DIM, why, RESET);
        if (strcmp(tech, "none") != 0) printf("  %ssensor: %s%s", BLUE, tech, RESET);
        printf("\n");
    }

    printf("\n%sLatest alarm events%s %s(time since start, bed, event, band, deadline-to-alarm time)%s\n", BOLD, RESET,
           DIM, RESET);
    int shown = 0;
    p = alarms;
    while ((p = strstr(p, "event t_s=")) != NULL && shown < 6) {
        size_t n = strcspn(p, "\n");
        if (n >= sizeof line) n = sizeof line - 1;
        memcpy(line, p, n);
        line[n] = '\0';
        p += n;
        char t[16], bed[16], kind[24], level[16], lat[16], reason[64];
        get(line, "t_s", t, sizeof t, 0);
        get(line, "bed", bed, sizeof bed, 0);
        get(line, "kind", kind, sizeof kind, 0);
        get(line, "level", level, sizeof level, 0);
        get(line, "latency_us", lat, sizeof lat, 0);
        get(line, "reason", reason, sizeof reason, 1);
        const char *lc = strncmp(kind, "technical", 9) == 0 ? BLUE : (strcmp(kind, "clear") == 0 ? GREEN : level_color(level));
        printf("  %7s s  %-7s %s%-15s%s %-11s %-34s %s%s us%s\n", t, bed, lc, kind, RESET,
               strncmp(kind, "technical", 9) == 0 ? "" : level, reason, DIM, lat, RESET);
        shown++;
    }
    if (shown == 0) printf("  %snone yet%s\n", DIM, RESET);
    (void)v;
}

int main(int argc, char **argv)
{
    int once = 0, interval_ms = 1000;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--once") == 0) once = 1;
        else if (strcmp(argv[i], "--no-color") == 0) g_color = 0;
        else if (strcmp(argv[i], "--interval") == 0 && i + 1 < argc) interval_ms = atoi(argv[++i]);
        else {
            fprintf(stderr, "usage: phila-display [--once] [--interval MS] [--no-color]\n");
            return 2;
        }
    }
    if (once) {
        show();
        return 0;
    }
    for (;;) {
        if (g_color) printf("\033[H\033[2J");
        show();
        fflush(stdout);
        usleep((useconds_t)interval_ms * 1000);
    }
}

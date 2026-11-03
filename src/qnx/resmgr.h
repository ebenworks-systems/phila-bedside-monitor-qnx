/*
 * resmgr.h - the monitor's files under /dev/phila, served by a QNX
 * resource manager.
 *
 *   /dev/phila/status    one line per bed: readings, NEWS2, alarm state
 *   /dev/phila/alarms    the alarm board and the latest alarm events
 *   /dev/phila/latency   the measured alarm latency so far
 *   /dev/phila/threads   each monitor thread's policy, priority and CPU use
 *   /dev/phila/control   write-only: "ack BED-02", "acvpu BED-02 C",
 *                        "oxygen BED-04 on|off", "scale BED-04 1|2"
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PHILA_RESMGR_H
#define PHILA_RESMGR_H

#include <stddef.h>

typedef enum { PFILE_STATUS = 0, PFILE_ALARMS, PFILE_LATENCY, PFILE_THREADS, PFILE_CONTROL, PFILE_COUNT } pfile_t;

/* Returns a malloc'd text snapshot of a readable file and its length. */
typedef char *(*pfile_render_fn)(pfile_t which, size_t *len);
/* Handles one control line; returns 0 (EOK) or an errno value. */
typedef int (*pfile_control_fn)(const char *line);

/* Attach the five names. Returns 0, or -1 with errno set. */
int phila_resmgr_attach(pfile_render_fn render, pfile_control_fn control);
/* Serve requests on the calling thread; does not return. */
void phila_resmgr_run(void);

#endif

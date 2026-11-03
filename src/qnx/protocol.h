/*
 * protocol.h - the message the sensor process sends to the monitor, and the
 * priorities that make up the design.
 *
 * Prototype for a hackathon, not a medical device, not for clinical use.
 * SPDX-License-Identifier: MIT
 */
#ifndef PHILA_PROTOCOL_H
#define PHILA_PROTOCOL_H

#include <stdint.h>
#include <sys/iomsg.h>

#include "phila/scenario.h"

/* The monitor registers this name with name_attach(); the sensors find it
 * with name_open(). It appears as /dev/name/local/phila-monitor. */
#define PHILA_MON_NAME "phila-monitor"

/* Above the range the resource-manager layer uses for its own messages. */
#define PHILA_MSG_READING (_IO_MAX + 0x40)

typedef struct {
    uint16_t type;       /* PHILA_MSG_READING */
    uint16_t sensor;     /* sensor_t */
    uint32_t seq;        /* per sensor, from 0 */
    uint32_t missed;     /* deadlines this sensor missed before this reading */
    uint32_t reserved;
    int64_t t_sched_ns;  /* CLOCK_MONOTONIC: the oldest deadline this reading serves */
    int64_t t_sample_ns; /* CLOCK_MONOTONIC: when the sensor thread ran */
    int16_t v[SCN_BEDS][2];
} phila_reading_msg_t;

/* Pulse codes (private to each process). */
#define PULSE_TICK (_PULSE_CODE_MINAVAIL + 0)
#define PULSE_ALARM (_PULSE_CODE_MINAVAIL + 1)
#define PULSE_SUPERVISE (_PULSE_CODE_MINAVAIL + 2)

/*
 * The priority design (QNX priorities: a higher number runs first).
 *
 *   60  alarm thread        FIFO; woken by a pulse sent at 60
 *   55  supervisor          FIFO; 250 ms timer pulse, finds silent sensors
 *   50  sensor threads      FIFO; their timer pulses carry 50
 *   50  intake thread       base 10, runs at 50 while it serves a sensor
 *                           message (QNX message passing passes the
 *                           sender's priority to the receiver)
 *   30  recorder            SCHED_SPORADIC: 30 for a 10 ms budget in every
 *                           100 ms, then drops to 5
 *   10  resource manager    serves /dev/phila/ files at its client's priority
 *
 * "--naive" runs every thread at 10 round-robin and sends every pulse at 10,
 * which is what a program gets when nobody designs its priorities.
 */
#define PRIO_ALARM 60
#define PRIO_SUPERVISOR 55
#define PRIO_SENSOR 50
#define PRIO_RECORDER 30
#define PRIO_RECORDER_LOW 5
#define PRIO_NAIVE 10

#endif

# Latency tables of the bedside monitor on QNX 8.0 in QEMU

**Prototype for a hackathon, not a medical device.**

These are the tables that `tools/latency_report.py` printed for the runs in `results/latency/` and `results/stall-test/`, copied here unchanged. [`LATENCY-REPORT.md`](LATENCY-REPORT.md) explains them. To print them again from the same files:

```sh
python3 tools/latency_report.py results/latency
python3 tools/latency_report.py results/stall-test
```

## The six cases, 4 November 2026, fixed build

**Clinical alarms, total** (Deadline of the reading to the alarm on the board)

| Case | Alarms | Min | Median | 99th percentile | 99.9th percentile | Max |
|:---|---:|---:|---:|---:|---:|---:|
| Designed, no load | 2986 | 26.7 µs | 93.9 µs | 1,157.5 µs | 3,682.3 µs | 62.8 ms |
| Designed, tick timers, no load | 2986 | 679.9 µs | 766.3 µs | 2,611.1 µs | 17.2 ms | 34.0 ms |
| Designed, hog FIFO 35 | 2984 | 11.5 µs | 55.9 µs | 540.7 µs | 18.4 ms | 247.7 ms |
| Designed, hog RR 10 | 2985 | 13.9 µs | 42.5 µs | 188.0 µs | 16.3 ms | 53.2 ms |
| Naive, hog RR 10 | 2986 | 853.1 µs | 3,850.4 µs | 23.9 ms | 27.9 ms | 28.9 ms |
| Naive, hog FIFO 35 | 187 | 912.9 µs | 982.5 µs | 1,784.4 µs | 1,909.2 µs | 279,952.2 ms |

**Clinical alarms, timer wake** (Deadline to the sensor thread running)

| Case | Alarms | Min | Median | 99th percentile | 99.9th percentile | Max |
|:---|---:|---:|---:|---:|---:|---:|
| Designed, no load | 2986 | 10.4 µs | 65.6 µs | 902.5 µs | 3,653.6 µs | 62.8 ms |
| Designed, tick timers, no load | 2986 | 658.6 µs | 734.7 µs | 2,566.0 µs | 17.2 ms | 34.0 ms |
| Designed, hog FIFO 35 | 2984 | 6.1 µs | 32.0 µs | 471.6 µs | 18.4 ms | 247.7 ms |
| Designed, hog RR 10 | 2985 | 6.8 µs | 19.1 µs | 134.6 µs | 16.3 ms | 53.1 ms |
| Naive, hog RR 10 | 2986 | 825.3 µs | 1,853.0 µs | 4,866.3 µs | 4,879.0 µs | 4,881.7 µs |
| Naive, hog FIFO 35 | 187 | 894.1 µs | 950.9 µs | 1,748.5 µs | 1,873.0 µs | 279,951.2 ms |

**Clinical alarms, alarm pipeline** (Sensor thread running to the alarm on the board)

| Case | Alarms | Min | Median | 99th percentile | 99.9th percentile | Max |
|:---|---:|---:|---:|---:|---:|---:|
| Designed, no load | 2986 | 8.1 µs | 27.9 µs | 83.3 µs | 446.8 µs | 1,626.6 µs |
| Designed, tick timers, no load | 2986 | 8.1 µs | 30.9 µs | 97.1 µs | 257.9 µs | 2,370.6 µs |
| Designed, hog FIFO 35 | 2984 | 3.6 µs | 22.3 µs | 75.9 µs | 133.6 µs | 499.3 µs |
| Designed, hog RR 10 | 2985 | 4.3 µs | 21.6 µs | 59.4 µs | 115.0 µs | 198.7 µs |
| Naive, hog RR 10 | 2986 | 14.2 µs | 1,999.3 µs | 20.0 ms | 24.0 ms | 24.0 ms |
| Naive, hog FIFO 35 | 187 | 12.0 µs | 24.4 µs | 73.9 µs | 76.9 µs | 998.5 µs |

**Each run**

| Case | Technical alarms | Missed sensor deadlines | Hog | Started |
|:---|---:|---:|:---|:---|
| Designed, no load | 4 | 0 | none | 2026-11-04 07:12:57 KST |
| Designed, tick timers, no load | 4 | 0 | none | 2026-11-04 07:18:08 KST |
| Designed, hog FIFO 35 | 8 | 2 | 2 threads, fifo priority 35, 280 s | 2026-11-04 07:23:20 KST |
| Designed, hog RR 10 | 4 | 0 | 2 threads, rr priority 10, 280 s | 2026-11-04 07:29:00 KST |
| Naive, hog RR 10 | 4 | 0 | 2 threads, rr priority 10, 280 s | 2026-11-04 07:34:10 KST |
| Naive, hog FIFO 35 | 16 | 3636 | 2 threads, fifo priority 35, 280 s | 2026-11-04 07:39:21 KST |

**Timer phase** (the median timer wake of each run of 100 clinical alarms, about ten seconds; a lasting shift means the sensor timer kept a late phase)

| Case | Lowest median | Highest median | Medians over 1 ms | Readings taken before their deadline | Earliest |
|:---|---:|---:|---:|---:|---:|
| Designed, no load | 55.0 µs | 89.9 µs | 0 of 29 | 0 | - |
| Designed, tick timers, no load | 700.5 µs | 779.9 µs | 0 of 29 | 0 | - |
| Designed, hog FIFO 35 | 15.4 µs | 58.7 µs | 0 of 29 | 0 | - |
| Designed, hog RR 10 | 12.2 µs | 68.9 µs | 0 of 29 | 0 | - |
| Naive, hog RR 10 | 874.0 µs | 1,875.9 µs | 28 of 29 | 0 | - |
| Naive, hog FIFO 35 | 932.8 µs | 932.8 µs | 0 of 1 | 0 | - |

**Alarms counted** (the monitor's own summary in monitor.log against the rows of latency.csv)

| Case | Clinical, summary | Clinical, latency.csv | Technical, summary | Technical, latency.csv | Same |
|:---|---:|---:|---:|---:|:---|
| Designed, no load | 2,986 | 2,986 | 4 | 4 | yes |
| Designed, tick timers, no load | 2,986 | 2,986 | 4 | 4 | yes |
| Designed, hog FIFO 35 | 2,984 | 2,984 | 8 | 8 | yes |
| Designed, hog RR 10 | 2,985 | 2,985 | 4 | 4 | yes |
| Naive, hog RR 10 | 2,986 | 2,986 | 4 | 4 | yes |
| Naive, hog FIFO 35 | 187 | 187 | 16 | 16 | yes |

**The host during each case** (its one-minute, five-minute and fifteen-minute load averages before and after the case, and the run-queue wait of the VM's virtual CPU threads during it)

| Case | Host load before | Host load after | Host kept the virtual CPUs waiting | Times put on a processor | Mean wait each time |
|:---|:---|:---|---:|---:|---:|
| Designed, no load | 4.54 69.60 141.47 | 8.47 30.32 103.83 | 2,322.8 ms over 308 s, 2 virtual CPUs | 106,918 | 21.7 µs |
| Designed, tick timers, no load | 8.47 30.32 103.83 | 8.39 17.43 77.45 | 4,053.3 ms over 308 s, 2 virtual CPUs | 112,674 | 36.0 µs |
| Designed, hog FIFO 35 | 8.39 17.43 77.45 | 11.55 14.94 59.30 | 8,433.6 ms over 308 s, 2 virtual CPUs | 69,610 | 121.2 µs |
| Designed, hog RR 10 | 9.23 13.96 57.33 | 6.64 10.56 43.83 | 4,246.4 ms over 308 s, 2 virtual CPUs | 32,625 | 130.2 µs |
| Naive, hog RR 10 | 6.83 10.53 43.64 | 3.59 6.64 32.72 | 165.3 ms over 308 s, 2 virtual CPUs | 14,092 | 11.7 µs |
| Naive, hog FIFO 35 | 4.50 6.78 32.62 | 4.27 6.01 25.12 | 413.7 ms over 309 s, 2 virtual CPUs | 14,560 | 28.4 µs |

Guest clock (pidin syspage=qtime, every case): tick 1 ms (nsec_inc 1000000 ns); ClockCycles() at 2,112,140,611 per second, 0.47 ns a cycle.

## The stall test, 4 November 2026, before and after the fix

**Clinical alarms, total** (Deadline of the reading to the alarm on the board)

| Case | Alarms | Min | Median | 99th percentile | 99.9th percentile | Max |
|:---|---:|---:|---:|---:|---:|---:|
| Before the fix: periodic timer, two host stalls | 1182 | 33.6 µs | 57.0 ms | 62.1 ms | 158.8 ms | 257.0 ms |
| After the fix: one-shot timer, two host stalls | 1184 | 21.5 µs | 84.2 µs | 238.7 µs | 1,360.9 µs | 291.9 ms |

**Clinical alarms, timer wake** (Deadline to the sensor thread running)

| Case | Alarms | Min | Median | 99th percentile | 99.9th percentile | Max |
|:---|---:|---:|---:|---:|---:|---:|
| Before the fix: periodic timer, two host stalls | 1182 | 13.3 µs | 57.0 ms | 62.1 ms | 158.7 ms | 256.9 ms |
| After the fix: one-shot timer, two host stalls | 1184 | 9.8 µs | 60.7 µs | 196.9 µs | 1,299.2 µs | 291.9 ms |

**Clinical alarms, alarm pipeline** (Sensor thread running to the alarm on the board)

| Case | Alarms | Min | Median | 99th percentile | 99.9th percentile | Max |
|:---|---:|---:|---:|---:|---:|---:|
| Before the fix: periodic timer, two host stalls | 1182 | 7.8 µs | 29.1 µs | 94.9 µs | 136.4 µs | 195.3 µs |
| After the fix: one-shot timer, two host stalls | 1184 | 8.0 µs | 23.2 µs | 77.4 µs | 112.1 µs | 191.7 µs |

**Each run**

| Case | Technical alarms | Missed sensor deadlines | Hog | Started |
|:---|---:|---:|:---|:---|
| Before the fix: periodic timer, two host stalls | 8 | 5 | none | 2026-11-04 06:22:45 KST |
| After the fix: one-shot timer, two host stalls | 8 | 3 | none | 2026-11-04 07:10:45 KST |

**Timer phase** (the median timer wake of each run of 100 clinical alarms, about ten seconds; a lasting shift means the sensor timer kept a late phase)

| Case | Lowest median | Highest median | Medians over 1 ms | Readings taken before their deadline | Earliest |
|:---|---:|---:|---:|---:|---:|
| Before the fix: periodic timer, two host stalls | 59.9 µs | 61.3 ms | 7 of 11 | 0 | - |
| After the fix: one-shot timer, two host stalls | 53.2 µs | 102.0 µs | 0 of 11 | 0 | - |

**Alarms counted** (the monitor's own summary in monitor.log against the rows of latency.csv)

| Case | Clinical, summary | Clinical, latency.csv | Technical, summary | Technical, latency.csv | Same |
|:---|---:|---:|---:|---:|:---|
| Before the fix: periodic timer, two host stalls | 1,182 | 1,182 | 8 | 8 | yes |
| After the fix: one-shot timer, two host stalls | 1,184 | 1,184 | 8 | 8 | yes |

**Timer wake every ten seconds, Before the fix: periodic timer, two host stalls** (the clinical alarms whose reading was due in each ten seconds, counted from the first alarm's deadline, which comes about 1.5 s after the start the stall times count from)

- Host stall: 40.037 s after the start, 332.7 ms (at 06:23:25.374)
- Host stall: 80.003 s after the start, 176.0 ms (at 06:24:05.341)

| Seconds | Alarms | Median wake | Max wake | Readings taken before their deadline |
|:---|---:|---:|---:|---:|
| 0 to 10 | 100 | 71.0 µs | 1,575.4 µs | 0 |
| 10 to 20 | 100 | 62.6 µs | 679.0 µs | 0 |
| 20 to 30 | 100 | 59.9 µs | 253.6 µs | 0 |
| 30 to 40 | 98 | 64.5 µs | 256.9 ms | 0 |
| 40 to 50 | 100 | 57.0 ms | 57.4 ms | 0 |
| 50 to 60 | 100 | 57.0 ms | 77.2 ms | 0 |
| 60 to 70 | 100 | 57.0 ms | 58.8 ms | 0 |
| 70 to 80 | 98 | 57.0 ms | 161.3 ms | 0 |
| 80 to 90 | 100 | 61.3 ms | 62.3 ms | 0 |
| 90 to 100 | 100 | 61.3 ms | 66.9 ms | 0 |
| 100 to 110 | 100 | 61.3 ms | 80.3 ms | 0 |
| 110 to 120 | 86 | 61.3 ms | 103.8 ms | 0 |

**Timer wake every ten seconds, After the fix: one-shot timer, two host stalls** (the clinical alarms whose reading was due in each ten seconds, counted from the first alarm's deadline, which comes about 1.5 s after the start the stall times count from)

- Host stall: 40.020 s after the start, 332.1 ms (at 07:11:25.287)
- Host stall: 80.036 s after the start, 172.2 ms (at 07:12:05.303)

| Seconds | Alarms | Median wake | Max wake | Readings taken before their deadline |
|:---|---:|---:|---:|---:|
| 0 to 10 | 100 | 84.7 µs | 271.0 µs | 0 |
| 10 to 20 | 100 | 102.0 µs | 1,299.2 µs | 0 |
| 20 to 30 | 100 | 71.2 µs | 343.8 µs | 0 |
| 30 to 40 | 98 | 58.3 µs | 291.9 ms | 0 |
| 40 to 50 | 100 | 55.9 µs | 146.8 µs | 0 |
| 50 to 60 | 100 | 52.3 µs | 126.6 µs | 0 |
| 60 to 70 | 100 | 53.5 µs | 133.8 µs | 0 |
| 70 to 80 | 99 | 55.4 µs | 144.5 ms | 0 |
| 80 to 90 | 100 | 54.5 µs | 126.9 µs | 0 |
| 90 to 100 | 100 | 55.9 µs | 224.4 µs | 0 |
| 100 to 110 | 100 | 57.8 µs | 216.3 µs | 0 |
| 110 to 120 | 87 | 55.0 µs | 105.8 µs | 0 |

**The host during each case** (its one-minute, five-minute and fifteen-minute load averages before and after the case, and the run-queue wait of the VM's virtual CPU threads during it)

| Case | Host load before | Host load after | Host kept the virtual CPUs waiting | Times put on a processor | Mean wait each time |
|:---|:---|:---|---:|---:|---:|
| Before the fix: periodic timer, two host stalls | 8.52 22.63 90.32 | 11.14 18.81 80.38 | 1,293.5 ms over 128 s, 2 virtual CPUs | 44,348 | 29.2 µs |
| After the fix: one-shot timer, two host stalls | 7.98 105.28 162.11 | 4.76 70.74 142.23 | 250.3 ms over 128 s, 2 virtual CPUs | 42,371 | 5.9 µs |

Guest clock (pidin syspage=qtime, every case): tick 1 ms (nsec_inc 1000000 ns); ClockCycles() at 2,112,140,611 per second, 0.47 ns a cycle.

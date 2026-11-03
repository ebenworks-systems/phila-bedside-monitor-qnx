# Phila Health Ecosystem bedside monitor prototype for QNX 8.0

This is a bedside vital-signs monitor written for the QNX 8.0 real-time operating system and run in QEMU. Simulated sensors measure four fictional patients' pulse, oxygen saturation, breathing rate, blood pressure and temperature at fixed rates. The monitor scores every patient with the National Early Warning Score 2 (NEWS2) after each reading and puts an alarm on the nurse's board within a time it measures, including while other programs hold every processor. A snapshot of each bed every five seconds, every alarm and every nurse action go into an append-only record. Its lines are chained by SHA-256 hashes, and once a second the monitor writes the chain's latest hash to the system log, so a check against that logged hash catches a line edited, removed or reordered before it, even when every later hash was rewritten to match. The record exports as one plain JSON document.

**This is a prototype for a hackathon. It is not a medical device and is not for clinical use.** The patients and readings are invented. Ebenworks built it for the QNX Global Hackathon 2026 as new work, separate from the products of Phila Health Ecosystem, which keeps clinics and health teams working without a signal. It shares no code with them.

## How it is built

Five small programs run on QNX:

| Program | What it does |
|:---|:---|
| `phila-sensord` | One thread per sensor. A periodic high-resolution timer sends each thread a pulse at its deadline; the thread reads its sensor for all four beds and sends the readings to the monitor with `MsgSend()`. |
| `phila-monitord` | Receives the readings, scores NEWS2, decides alarms, raises them, keeps the record and serves the files under `/dev/phila/`. |
| `phila-display` | The nurse's view in a terminal, read from `/dev/phila/status` and `/dev/phila/alarms` once a second. |
| `phila-hog` | Spinning threads at a chosen priority, used to load the processors during the latency tests. |
| `phila-export` | Checks the record's hash chain and writes the export. It also builds on Linux and macOS. |

The monitor's threads and their priorities are the design. On QNX a higher number runs first.

| Priority | Thread | How it gets there |
|---:|:---|:---|
| 60 | alarm | `SCHED_FIFO`; woken by a pulse that is itself sent at priority 60 |
| 55 | supervisor | `SCHED_FIFO`; a 250 ms timer pulse; finds sensors that went silent and reads each thread's CPU time |
| 50 | sensors | `SCHED_FIFO`; their timer pulses carry priority 50 |
| 50 | intake | base priority 10, but QNX message passing runs a receiving thread at its sender's priority, so it handles each reading at 50 |
| 30, then 5 | recorder | `SCHED_SPORADIC`: priority 30 for 10 ms of CPU in every 100 ms, then 5 until the budget refills |
| client's | resource manager | serves `/dev/phila/*` at the priority of the program that asked |

The alarm path never waits on the disk. The recorder takes entries from a queue and writes them when it can, and the queue's high-water mark is reported, so a slow disk shows up as a number rather than a late alarm. Both locks use priority inheritance, so a low-priority thread holding a lock that the alarm thread needs is raised until it lets go.

The plan for this entry named QNX adaptive partitioning to cap the recorder's share of the CPU. QNX's own documentation for the 8.0 IDE says "QNX OS 8.0 does not support adaptive partitioning" ([APS View](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.ide.userguide/topic/APS_view.html)), so the recorder runs under sporadic scheduling instead, and the supervisor measures every thread's CPU time with the per-thread CPU clocks (`pthread_getcpuclockid()`), which `/dev/phila/threads` shows.

Running the monitor with `--naive` puts every thread at priority 10 round-robin and uses default tick-aligned timers. That is the comparison case in the latency runs: the same code with no priority design.

The QNX features in use are message passing with priority inheritance (`name_attach()`, `name_open()`, `MsgSend()`, `MsgReceive()`, `MsgReply()`), pulses (`MsgSendPulse()`, `MsgReceivePulse()`), periodic and high-resolution timers (`timer_create()` with `SIGEV_PULSE_INIT`, `timer_settime()` with `TIMER_TOLERANCE`), a resource manager (`resmgr_attach()`, the `iofunc` layer and a per-open snapshot), `SCHED_SPORADIC`, mutexes with `PTHREAD_PRIO_INHERIT`, per-thread CPU-time clocks, named threads and `slog2` logging.

## The early warning score

Scoring follows the NEWS2 chart in: Royal College of Physicians. *National Early Warning Score (NEWS) 2: Standardising the assessment of acute-illness severity in the NHS.* Updated report of a working party. London: RCP, 2017.

Each of seven parameters scores 0 to 3 (breathing rate, oxygen saturation on Scale 1 or Scale 2, air or oxygen, systolic pressure, pulse, consciousness on the ACVPU scale and temperature). An alarm starts when a bed enters one of the report's clinical response bands: a single score of 3 (low-medium), an aggregate of 5 or 6 (medium), or 7 or more (high). It is raised again when the band goes above the highest band the alarm has reached, and it clears when the bed returns to low, so a score wobbling across a band edge does not repeat the alarm. A sensor that reports no reading, or that has been silent for three of its periods, raises a technical alarm and is left out of the score until it reads again. SpO2 Scale 2 and supplemental oxygen are set by a clinician through `/dev/phila/control`, and so is consciousness, because no sensor measures it.

`tests/test_news2.c` checks every band edge of the chart on both SpO2 scales.

## Build and run

You need a Linux host on x86_64 with QEMU (KVM makes it fast), `make`, a C compiler and Python 3, and the QNX Software Development Platform 8.0 with its QEMU target, which comes with the free QNX Everywhere licence from [myQNX](https://www.qnx.com/products/everywhere/). These steps were run on Ubuntu with SDP 8.0 installed by QNX Software Center 2.0.4.

| | |
|:---|:---|
| Operating system | QNX OS 8.0 (`procnto-smp-instr`, build 8.0.0 2026/06/05) |
| Target | x86_64 QEMU virtual machine made by `mkqnximage`, 2 CPUs, 1 GB |
| Toolchain | `qcc -Vgcc_ntox86_64` (GCC 12.2.0) from SDP 8.0 |

```sh
make test                                  # unit tests on the host
source ~/qnx800/qnxsdp-env.sh              # wherever your SDP 8.0 lives
make qnx                                   # cross-compile everything into build/qnx-x86_64/
scripts/make-image.sh ~/qnx-vm/phila       # build the QNX 8.0 QEMU image
scripts/qemu-vm.sh start ~/qnx-vm/phila    # boot it in the background
scripts/qemu-vm.sh wait ~/qnx-vm/phila     # until SSH answers on 127.0.0.1:2222
```

Copy the programs in and start the monitor, the sensors and the display:

```sh
scripts/qemu-vm.sh ssh ~/qnx-vm/phila 'mkdir -p /data/phila/bin /data/phila/runs'
for f in build/qnx-x86_64/*; do scripts/qemu-vm.sh put ~/qnx-vm/phila "$f" /data/phila/bin/; done
scripts/qemu-vm.sh ssh ~/qnx-vm/phila
# then, inside QNX:
cd /data/phila
bin/phila-monitord --scenario ward --record runs/ward.jsonl &
bin/phila-sensord --scenario ward &
bin/phila-display
```

The ward scenario runs for about four minutes. Bed 1 stays stable. Bed 2 deteriorates slowly from 30 seconds and reaches the high band. Bed 3's oxygen saturation drops from 60 to 100 seconds. Bed 4 is on oxygen and scored on SpO2 Scale 2, and its blood-pressure cuff comes off from 120 to 170 seconds. Try the nurse controls while it runs:

```sh
echo "acvpu BED-01 C" > /dev/phila/control   # new confusion: a score of 3
echo "ack BED-01" > /dev/phila/control
echo "acvpu BED-01 A" > /dev/phila/control
cat /dev/phila/latency /dev/phila/threads
slog2info | grep phila
```

Stop the monitor with `kill` (it finishes the record and prints its latency summary and the record's head), then check and export the record:

```sh
bin/phila-export runs/ward.jsonl runs/ward-export.json     # on QNX
python3 tools/verify_record.py ward.jsonl                   # on the host, after scripts/qemu-vm.sh get
python3 tools/verify_record.py --head SEQ HASH ward.jsonl   # SEQ and HASH from "record head" in slog2info
```

The chain alone shows a changed line unless every hash after it was rewritten too. The head check closes that gap for every line written before the head was logged: a rewritten or shortened file no longer has that hash at that line.

The QEMU image that `mkqnximage` makes does not include OpenSSH's `sshd-auth` helper, so sshd refuses every login. `scripts/make-image.sh` adds it through the two files in `qemu/snippets/`. `scripts/qemu-vm.sh` boots the image with QEMU's user-mode network, so it needs no bridge or root, and forwards SSH to 127.0.0.1 only.

## The files under /dev/phila

| File | Contents |
|:---|:---|
| `status` | One line per bed: the latest readings, NEWS2, the band and the alarm state |
| `alarms` | The alarm board and the latest alarm events, each with its measured latency |
| `latency` | Alarm latency so far: deadline to alarm, split into the timer wake and the alarm pipeline |
| `threads` | Each monitor thread's policy, priority and CPU use |
| `control` | Write-only: `ack BED-02`, `acvpu BED-02 C`, `oxygen BED-04 on`, `scale BED-04 2` |

## Measuring the alarm path

`scripts/latency-runs.sh` runs the probe scenario, in which the oxygen saturation of beds 1 and 2 alternates between normal and a red score on every 100 ms reading, in opposite phase, so every reading starts a new alarm on one of the two beds, ten a second, and the first reading after any stall is measured too. For every alarm the monitor stores three spans: from the reading's deadline to the sensor thread running (the timer wake), from there to the alarm on the board (the alarm pipeline), and the total. The cases are no load, two spinning threads at priority 35 (above the system's own services and below the alarm path), two at priority 10, and the same loads against the `--naive` build. `tools/latency_report.py` turns the fetched runs into tables and a chart.

## Repository layout

| Path | |
|:---|:---|
| `include/phila/`, `src/core/` | Portable C: NEWS2, SHA-256, the record, the scenarios and the alarm decision |
| `src/qnx/` | The QNX programs |
| `src/tools/export.c` | The export, portable |
| `tests/` | Unit tests that run on the host and on QNX |
| `tools/verify_record.py` | An independent check of the record's hash chain |
| `scripts/` | Image build, VM control and the latency runs |
| `qemu/snippets/` | The `mkqnximage` additions |
| `docs/` | The codelab |

## Licence

MIT, in `LICENSE`. QNX is a trademark of BlackBerry Limited. NEWS2 is published by the Royal College of Physicians; this code implements its scoring chart and is not endorsed by the College.

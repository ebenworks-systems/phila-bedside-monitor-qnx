# Codelab: a bedside monitor that keeps its alarm deadline on QNX 8.0

In this codelab you build and run a vital-signs monitor on QNX 8.0 in QEMU, then load the processors until they have no spare time and watch the alarm path keep its deadline. On the way you use the parts of QNX that make that possible: message passing that hands the sender's priority to the receiver, pulses, high-resolution timers, a resource manager, sporadic scheduling and priority-inheritance mutexes.

The monitor is a prototype for a hackathon. It is not a medical device and is not for clinical use, and every patient in it is invented.

**What you need:** a Linux PC on x86_64 (these steps were run on Ubuntu 24.04) with QEMU, `make`, `gcc`, `socat`, `ssh` and Python 3; a free myQNX account; about 5 GB of disk; and about 90 minutes, most of it waiting for the SDP download.

**What you will have at the end:** QNX 8.0 running in a virtual machine, the monitor scoring four patients there, an alarm latency measurement of your own, and a hash-chained record you can check from the host.

## 1. Get QNX SDP 8.0

QNX gives a free non-commercial licence, QNX Everywhere, which covers learning, research and prototypes. Sign in at [myQNX](https://www.qnx.com/), request the QNX Everywhere licence, and accept and deploy it in the myQNX License Manager. Then install QNX Software Center for Linux from your myQNX downloads.

Software Center has a window, and the same steps run from its command-line tool, which is easier to repeat. Your licence key is on your myQNX dashboard.

```sh
cd ~/qnx/qnxsoftwarecenter
./qnxsoftwarecenter_clt -addLicenseKey XXXX-XXXX-XXXX-XXXX-XXXX
./qnxsoftwarecenter_clt -myqnx.user YOU@EXAMPLE.COM -myqnx.password '...' -activateAll
./qnxsoftwarecenter_clt -myqnx.user YOU@EXAMPLE.COM -myqnx.password '...' -mirrorBaseline qnx800
./qnxsoftwarecenter_clt -myqnx.user YOU@EXAMPLE.COM -myqnx.password '...' -installBaseline com.qnx.qnx800 -destination ~/qnx800
```

Two things that cost us time: `-mirrorBaseline` wants the short name `qnx800`, while `-installBaseline` wants the full `com.qnx.qnx800`, and the install refuses to start until the mirror has finished. The SDP itself takes about 3.5 GB, and Software Center keeps about another gigabyte of downloads and tools.

Every terminal you build in needs the SDP's environment:

```sh
source ~/qnx800/qnxsdp-env.sh
qcc -V          # lists 12.2.0,gcc_ntox86_64 and 12.2.0,gcc_ntoaarch64le
```

## 2. Build the code and run the tests on your PC

```sh
git clone https://github.com/ebenworks-systems/phila-bedside-monitor-qnx.git
cd phila-bedside-monitor-qnx
make test
```

You should see five test programs report no failures and the record checker's self-test pass:

```
news2: 130 checks, 0 failed
sha256: 5 checks, 0 failed
record: 56 checks, 0 failed
beds: 78 checks, 0 failed
scenario: 24 checks, 0 failed
verify_record.py: self-test passed
```

The scoring, the hash chain and the alarm decision are plain C in `src/core/` with no operating-system calls. That is deliberate: the part that decides whether a patient needs help can be tested anywhere, and the QNX code around it only decides *when* it runs.

Now cross-compile everything for QNX:

```sh
make qnx
ls build/qnx-x86_64/
```

`make qnx` calls `qcc -Vgcc_ntox86_64` and links the monitor with `-lslog2`. Set `QNX_ARCH=aarch64le` to build for a Raspberry Pi 4 instead.

## 3. Make the QEMU image and boot it

`mkqnximage` comes with the SDP and builds a bootable QNX 8.0 image for QEMU. The repository wraps it:

```sh
scripts/make-image.sh ~/qnx-vm/phila
scripts/qemu-vm.sh start ~/qnx-vm/phila
scripts/qemu-vm.sh wait ~/qnx-vm/phila
scripts/qemu-vm.sh ssh ~/qnx-vm/phila uname -a
```

The last line should print `QNX phila-monitor 8.0.0 ... x86pc x86_64`.

Two fixes are in those scripts, and both are worth knowing if you build your own image.

First, the default image's OpenSSH server looks for a helper program, `/usr/libexec/sshd-auth`, that the image leaves out, so it refuses every login. `mkqnximage` reads extra build lines from `local/snippets/` in the image folder, and the two files in `qemu/snippets/` add the helper:

```
# ifs_files.custom
[type=link] /usr/libexec/sshd-auth=/system/bin/sshd-auth
# system_files.custom
bin/sshd-auth=usr/libexec/sshd-auth
```

Second, `mkqnximage --run` joins the guest to a host network bridge, which needs root to set up. `scripts/qemu-vm.sh` boots the same disk with QEMU's user-mode network instead and forwards SSH to 127.0.0.1:2222 only, so nothing outside your PC can reach the guest. If SSH reports "Too many authentication failures", your SSH agent is offering other keys first; the script passes `-o IdentitiesOnly=yes` for that reason.

Copy the programs in and run the tests again, this time on QNX:

```sh
scripts/qemu-vm.sh ssh ~/qnx-vm/phila 'mkdir -p /data/phila/bin /data/phila/runs'
for f in build/qnx-x86_64/*; do scripts/qemu-vm.sh put ~/qnx-vm/phila "$f" /data/phila/bin/; done
scripts/qemu-vm.sh ssh ~/qnx-vm/phila 'cd /data/phila/bin && for t in news2 sha256 record beds scenario; do ./test_$t; done'
```

The same 293 checks pass on QNX 8.0.

## 4. Sensors that keep time: pulses and high-resolution timers

Open `src/qnx/sensord.c`. Each sensor is a thread with a private channel and a timer whose expiry is a *pulse*, a tiny message that needs no reply, delivered to that channel:

```c
int chid = ChannelCreate(_NTO_CHF_PRIVATE);
int self = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
struct sigevent ev;
SIGEV_PULSE_INIT(&ev, self, prio, PULSE_TICK, ctx->sensor);
timer_create(CLOCK_MONOTONIC, &ev, &timer);
```

The pulse carries a priority (`prio`, 50 here), and the thread that receives it runs at that priority. So the timer itself decides how urgently the sensor runs.

QNX 8.0 is tickless, but by default a timer still expires on the 1 ms system tick. A tolerance below the tick asks for a high-resolution timer that fires at its deadline:

```c
struct itimerspec tol = {0};
tol.it_value.tv_nsec = 1;
timer_settime(timer, TIMER_TOLERANCE, &tol, NULL);   /* then arm it as usual */
```

Every reading the sensor sends carries two times: the deadline it was due and the moment the thread actually ran. A high-resolution timer can fire a few microseconds before its deadline, so the sensor counts a pulse within half a period of the next deadline as on time, and if the thread was held up past several deadlines it counts the delay from the oldest one it missed. That way a late sensor can never hide its lateness.

## 5. Message passing that carries priority

The monitor registers a name; the sensors find it and send readings with `MsgSend()`:

```c
/* monitor */
name_attach_t *att = name_attach(NULL, "phila-monitor", 0);
/* sensor */
int coid = name_open("phila-monitor", 0);
MsgSend(coid, &msg, sizeof msg, NULL, 0);
```

The monitor's intake thread is started at priority 10. On QNX, a thread that receives a message runs at the sender's priority until it replies. The sensors send at 50, so intake handles every reading at 50, and a lower-priority program cannot delay it. The monitor records the highest priority intake ran at, and you can see it:

```sh
cat /dev/phila/latency        # ... intake_max_priority=50 ...
pidin -p phila-monitord -F "%a %b %N %p %J"
```

```
     pid tid name               prio STATE
 2236438   1 phila-monitord      10r RECEIVE
 2236438   2 phila-monitord      60f RECEIVE
 2236438   3 phila-monitord      55f RECEIVE
 2236438   4 phila-monitord      30s CONDVAR
 2236438   5 phila-monitord      50r RECEIVE
```

Thread 5 is intake, still at the 50 it inherited from the last reading. Thread 4 is the recorder; the `s` means sporadic scheduling, which step 7 covers.

Intake scores NEWS2 for each bed, and when an alarm starts it does not raise it itself. It queues the event and sends a pulse at priority 60 to the alarm thread, which puts the alarm on the board the nurse's display reads and timestamps it:

```c
MsgSendPulse(M.alarm_coid, M.alarm_prio, PULSE_ALARM, raised);
MsgReply(rcvid, EOK, NULL, 0);
```

The pulse goes out before the reply, so the alarm thread is already running on a free CPU, or has taken this one, while the sensor gets its answer.

## 6. Files for the nurse: a resource manager

`src/qnx/resmgr.c` makes the monitor answer `open()`, `read()` and `write()` on five names under `/dev/phila/`. Any program can read the ward's state with the ordinary C library or the shell, without linking to the monitor:

```sh
cat /dev/phila/status
echo "acvpu BED-01 C" > /dev/phila/control    # the nurse records new confusion
```

Each `open()` gets its own snapshot, built on the first `read()` and kept in an extended OCB (the per-open structure of the `iofunc` layer), so a reader that reads in small pieces still sees one consistent picture. The thread that serves these requests runs at the priority of whoever asked, which is the default for a QNX resource manager: the display at priority 10 never borrows the alarm path's priority.

Start the ward scenario and the display:

```sh
cd /data/phila
bin/phila-monitord --scenario ward --record runs/ward.jsonl &
bin/phila-sensord --scenario ward &
bin/phila-display
```

Over about four minutes, bed 2 deteriorates into the high band, bed 3's oxygen saturation drops for 40 seconds, and bed 4's blood-pressure cuff comes off and raises a technical alarm. Try `echo "ack BED-02" > /dev/phila/control` while bed 2 is alarming, and `slog2info | grep phila` to see every alarm in the system log.

## 7. Keep the record without risking the alarm

A snapshot of each bed every five seconds while every sensor on that bed is reading, every alarm and every nurse action are written to an append-only record. Each line holds the SHA-256 hash of the line before it and its own hash, so editing, removing or reordering a line breaks the chain from that point, unless whoever changed it also rewrote every hash after it. To catch that too, the recorder writes the chain's head, the latest line's number and hash, to the system log once a second, and the monitor prints it when it stops. The recorder thread writes the file and calls `fsync()` after every batch that holds an alarm.

Writing to a disk can take a long time, so the recorder must never hold up the alarm path, and must not take more CPU than it should when it has a backlog. Older QNX releases offered adaptive partitioning for that. QNX's documentation for the 8.0 IDE says "QNX OS 8.0 does not support adaptive partitioning", so this monitor uses POSIX sporadic scheduling, which QNX 8.0 does support:

```c
rt_sporadic_t ss = {PRIO_RECORDER_LOW /* 5 */, 8, 100000000LL /* 100 ms */, 10000000LL /* 10 ms */};
rt_thread(&M.rec_tid, "recorder", recorder_main, &rec_fd, SCHED_SPORADIC, PRIO_RECORDER /* 30 */, &ss);
```

The recorder runs at priority 30 for 10 ms of CPU in every 100 ms, then at 5 until its budget refills. `/dev/phila/threads` shows each thread's CPU use, read from the per-thread CPU clocks:

```
thread       policy    prio   cpu_%    cpu_ms
alarm        fifo        60    0.00       0.0
supervisor   fifo        55    0.01       0.7
recorder     sporadic    30    0.00       0.4
intake       rr          10    0.00       0.2
resmgr       rr          10    0.00       0.7
```

Intake shows its base priority, 10; it runs at 50 only while it serves a sensor.

The alarm thread and the recorder share a queue, and its lock is a mutex with `PTHREAD_PRIO_INHERIT`. If the recorder (priority 30) holds the lock when the alarm thread (60) wants it, QNX raises the recorder to 60 until it lets go, so a program at priority 35 cannot stretch that wait.

Stop the display with Ctrl-C and the monitor with `slay`, which sends it SIGTERM. `slay` returns at once, and the monitor takes a moment to finish the record and print its head, so wait until it has gone before reading the file. Then check the record on QNX and again, independently, on your PC:

```sh
slay phila-monitord              # the monitor prints "record head seq=N hash=..." as it stops
while pidin -p phila-monitord -f a | grep -q '[0-9]'; do sleep 1; done
bin/phila-export runs/ward.jsonl runs/ward-export.json
# on the host
scripts/qemu-vm.sh get ~/qnx-vm/phila /data/phila/runs/ward.jsonl ward.jsonl
python3 tools/verify_record.py ward.jsonl
python3 tools/verify_record.py --head N HASH ward.jsonl
```

Copy `ward.jsonl`, change one digit in the copy and run the first check on it: it names the line you changed. The head check is for a copy rewritten with every hash after the change recomputed, which still passes the chain but no longer has the logged hash at line N, and for a copy with lines cut off the end. While the monitor runs, `slog2info | grep "record head"` lists the heads logged so far; check a file only against a head logged before you copied it.

## 8. Load the CPUs and measure

`phila-hog` starts spinning threads at a priority you choose. With two threads at priority 35 on the two virtual CPUs, everything below 35 stops while the hog runs, including the network stack (io-sock, priority 21) and SSH (priority 10). That is why the latency cases run inside the guest from `scripts/run-case.sh`, and the host only starts them and collects the results:

```sh
scripts/latency-runs.sh ~/qnx-vm/phila 300
```

The probe scenario makes the oxygen saturation of beds 1 and 2 alternate between normal and a red score on every 100 ms reading, in opposite phase, so every reading starts an alarm on one of the two beds, ten a second. The first reading after a stall is an alarm onset too, so a stall's full delay always shows in the figures. For each alarm the monitor stores the reading's deadline, when the sensor thread ran, when intake decided and when the alarm was on the board, and writes them to `latency.csv` at the end. The cases are:

| Case | Monitor | Load |
|:---|:---|:---|
| `designed-idle` | designed priorities, high-resolution timers | none |
| `designed-tick-idle` | designed priorities, 1 ms tick timers | none |
| `designed-hog35` | designed | two threads, FIFO priority 35 |
| `designed-hog10` | designed | two threads, round-robin priority 10 |
| `naive-hog10` | `--naive`: everything at 10, tick timers | two threads, round-robin priority 10 |
| `naive-hog35` | `--naive` | two threads, FIFO priority 35 |

Under the designed priorities, the hog at 35 sits below every thread on the alarm path, so the alarm path should run as if the hog were not there. Under `--naive`, the same hog sits above the whole monitor, which cannot run at all until the hog stops.

A virtual machine measures its own timing only as well as the host lets it run. On a busy PC the guest's virtual CPUs wait for the host scheduler, and that wait lands in the timer wake figure. Measure with the host as quiet as you can make it, and compare the cases with each other rather than with figures from real hardware. `scripts/latency-runs.sh` records the host's load before and after each case, and `python3 tools/latency_report.py results/latency --chart latency.png` prints the tables and draws the chart.

## 9. Clean up

```sh
scripts/qemu-vm.sh stop ~/qnx-vm/phila
```

## What you used

| QNX feature | Where |
|:---|:---|
| `name_attach()`, `name_open()`, `MsgSend()`, `MsgReceive()`, `MsgReply()` and priority inheritance through messages | `sensord.c`, `monitord.c` |
| Pulses with priorities: `SIGEV_PULSE_INIT`, `MsgSendPulse()`, `MsgReceivePulse()` | `sensord.c`, `monitord.c` |
| High-resolution timers: `TIMER_TOLERANCE` | `sensord.c` |
| Resource manager: `resmgr_attach()`, `iofunc`, an extended OCB | `resmgr.c` |
| `SCHED_SPORADIC` and `SCHED_FIFO` | `monitord.c`, `rt.c` |
| `PTHREAD_PRIO_INHERIT` mutexes | `monitord.c` |
| Per-thread CPU-time clocks | `monitord.c` |
| `slog2` | `monitord.c` |
| `mkqnximage` and its snippets | `scripts/make-image.sh`, `qemu/snippets/` |

NEWS2 scoring follows Royal College of Physicians, *National Early Warning Score (NEWS) 2*, London: RCP, 2017. QNX is a trademark of BlackBerry Limited.

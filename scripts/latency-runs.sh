#!/usr/bin/env bash
# latency-runs.sh - copy the QNX build into the running VM, run the latency
# cases one after another and fetch the results to results/latency/.
#
#   scripts/latency-runs.sh VMDIR SECONDS [CASE...]
#
# Cases: designed-idle designed-tick-idle designed-hog35 designed-hog10 naive-hog10 naive-hog35
# Each hog uses two threads (one per virtual CPU) and starts ten seconds in.
#
# A VM measures its own timing only as well as the host lets it run. Set
# QUIET_LOAD (for example 2.5) to wait before each case until the host's
# one-minute load average is below it, for at most QUIET_WAIT seconds
# (default 1800); the load is written beside each case's results either way,
# with how long the host kept the VM's virtual CPU threads waiting for a
# processor during the case (their run-queue wait, from /proc/PID/task/TID/schedstat).
# SPDX-License-Identifier: MIT
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
vm="$1"; secs="$2"; shift 2
cases=("$@")
[ ${#cases[@]} -gt 0 ] || cases=(designed-idle designed-tick-idle designed-hog35 designed-hog10 naive-hog10 naive-hog35)
q="$here/scripts/qemu-vm.sh"
build="$here/build/qnx-x86_64"
out="$here/results/latency"
mkdir -p "$out"

"$q" ssh "$vm" 'mkdir -p /data/phila/bin /data/phila/runs'
for f in "$build"/phila-*; do "$q" put "$vm" "$f" "/data/phila/bin/$(basename "$f")"; done
"$q" put "$vm" "$here/scripts/run-case.sh" /data/phila/run-case.sh

hog_for() { # prio policy delay seconds threads
    local busy=$((secs - 20)); [ "$busy" -gt 5 ] || busy=5
    case "$1" in
    designed-hog35|naive-hog35) echo "35 fifo 10 $busy 2" ;;
    designed-hog10|naive-hog10) echo "10 rr 10 $busy 2" ;;
    *) echo "" ;;
    esac
}
mode_for() {
    case "$1" in
    designed-tick-*) echo designed-tick ;;
    naive-*) echo naive ;;
    *) echo designed ;;
    esac
}

wait_quiet() {
    [ -n "${QUIET_LOAD:-}" ] || return 0
    local waited=0
    while awk -v max="$QUIET_LOAD" '{exit !($1 >= max)}' /proc/loadavg; do
        [ "$waited" -ge "${QUIET_WAIT:-1800}" ] && { echo "  host still busy after ${waited} s; running anyway"; return 0; }
        sleep 30; waited=$((waited + 30))
    done
}

vcpu_stat() { # run ns, run-queue wait ns and times run, summed over the VM's virtual CPU threads; then the thread count
    local pid t r w s run=0 wt=0 n=0 k=0
    pid="$(cat "$vm/qemu.pid" 2>/dev/null)" || { echo "0 0 0 0"; return 0; }
    for t in /proc/"$pid"/task/*; do
        grep -qE '^CPU [0-9]+/(KVM|TCG)$' "$t/comm" 2>/dev/null || continue
        read -r r w s < "$t/schedstat" || continue
        run=$((run + r)); wt=$((wt + w)); n=$((n + s)); k=$((k + 1))
    done
    echo "$run $wt $n $k"
}

for c in "${cases[@]}"; do
    wait_quiet
    load_before="$(cut -d' ' -f1-3 /proc/loadavg)"
    read -r run0 wait0 n0 _ <<< "$(vcpu_stat)"; t0="$(date +%s.%N)"
    hog="$(hog_for "$c")"
    echo "$(date '+%H:%M:%S') case $c: mode $(mode_for "$c"), ${secs} s, hog: ${hog:-none}; host load $(cut -d' ' -f1-3 /proc/loadavg)"
    "$q" ssh "$vm" "nohup sh /data/phila/run-case.sh $c $(mode_for "$c") $secs $hog > /dev/null 2>&1 &"
    sleep $((secs + 8))
    for _ in $(seq 1 60); do
        if "$q" ssh "$vm" "test -f /data/phila/runs/$c/done" 2>/dev/null; then break; fi
        sleep 5
    done
    read -r run1 wait1 n1 k <<< "$(vcpu_stat)"; t1="$(date +%s.%N)"
    load_after="$(cut -d' ' -f1-3 /proc/loadavg)"
    mkdir -p "$out/$c"
    for f in monitor.log sensor.log hog.log latency.csv record.jsonl uname.txt pidin-info.txt qtime.txt threads-under-load.txt \
             pidin-under-load.txt latency-mid.txt done; do
        "$q" get "$vm" "/data/phila/runs/$c/$f" "$out/$c/$f" 2>/dev/null || true
    done
    {
        printf 'host load before: %s\nhost load after: %s\n' "$load_before" "$load_after"
        printf 'started: %s\n' "$(date -d "@${t0%.*}" '+%Y-%m-%d %H:%M:%S %Z')"
        awk -v t0="$t0" -v t1="$t1" -v r0="$run0" -v r1="$run1" -v w0="$wait0" -v w1="$wait1" -v n0="$n0" -v n1="$n1" -v k="$k" 'BEGIN {
            printf "case seconds: %.1f\nvcpu threads: %d\nvcpu run ms: %.1f\nvcpu wait ms: %.1f\nvcpu times run: %d\n",
                t1 - t0, k, (r1 - r0) / 1e6, (w1 - w0) / 1e6, n1 - n0 }'
    } > "$out/$c/host.txt"
    sed -n '2,6p' "$out/$c/monitor.log" 2>/dev/null || echo "  (no monitor log)"
done

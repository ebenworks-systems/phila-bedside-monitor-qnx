#!/usr/bin/env bash
# latency-runs.sh - copy the QNX build into the running VM, run the latency
# cases one after another and fetch the results to results/latency/.
#
#   scripts/latency-runs.sh VMDIR SECONDS [CASE...]
#
# Cases: designed-idle designed-tick-idle designed-hog35 designed-hog10 naive-hog10 naive-hog35
# Each hog uses two threads (one per virtual CPU) and starts ten seconds in.
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

for c in "${cases[@]}"; do
    hog="$(hog_for "$c")"
    echo "$(date '+%H:%M:%S') case $c: mode $(mode_for "$c"), ${secs} s, hog: ${hog:-none}; host load $(cut -d' ' -f1-3 /proc/loadavg)"
    "$q" ssh "$vm" "nohup sh /data/phila/run-case.sh $c $(mode_for "$c") $secs $hog > /dev/null 2>&1 &"
    sleep $((secs + 8))
    for _ in $(seq 1 60); do
        if "$q" ssh "$vm" "test -f /data/phila/runs/$c/done" 2>/dev/null; then break; fi
        sleep 5
    done
    mkdir -p "$out/$c"
    for f in monitor.log sensor.log hog.log latency.csv record.jsonl uname.txt pidin-info.txt threads-under-load.txt \
             pidin-under-load.txt latency-mid.txt done; do
        "$q" get "$vm" "/data/phila/runs/$c/$f" "$out/$c/$f" 2>/dev/null || true
    done
    echo "host load after: $(cut -d' ' -f1-3 /proc/loadavg)" > "$out/$c/host.txt"
    sed -n '2,6p' "$out/$c/monitor.log" 2>/dev/null || echo "  (no monitor log)"
done

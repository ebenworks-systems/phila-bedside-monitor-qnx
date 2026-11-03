#!/usr/bin/env bash
# stall-test.sh - freeze the whole virtual machine from the host twice during
# a designed probe with no load, to show whether each sensor's timer goes back
# to its own deadlines after a stall.
#
#   scripts/stall-test.sh VMDIR [NAME]
#
# Copies the QNX build into the running VM, starts `run-case.sh stall-test
# designed 120` in the guest, stops the whole QEMU process with SIGSTOP for
# 330 ms at 40 s and for 170 ms at 80 s (SIGCONT ends each stall), and fetches
# the case as latency-runs.sh does into results/stall-test/NAME/ (default NAME
# stall-test; STALL_OUT replaces results/stall-test). host.txt holds the host's
# load, how long the host kept the VM's virtual CPU threads waiting for a
# processor, and each stall's start (seconds after the case started, and the
# clock time) and its measured length. The start of the case is when the ssh
# command that starts it returned; the first reading's deadline comes about
# 1.5 s later (run-case.sh starts the sensors a second after the monitor, and
# they begin on a 100 ms boundary at least half a second on). A stopped
# process is not waiting for a processor, so the stalls themselves are not in
# that wait.
#
# QNX_BUILD names another build folder to copy (default build/qnx-x86_64), so
# an older build can be tested with this script.
# SPDX-License-Identifier: MIT
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
vm="$1"; name="${2:-stall-test}"
q="$here/scripts/qemu-vm.sh"
build="${QNX_BUILD:-$here/build/qnx-x86_64}"
out="${STALL_OUT:-$here/results/stall-test}"
case "$out" in /*) ;; *) out="$here/$out" ;; esac
secs=120
stalls=("40 0.330" "80 0.170")   # seconds after the case starts, stall length in seconds

vcpu_stat() { # run ns, run-queue wait ns and times run, summed over the VM's virtual CPU threads; their count; QEMU's pid
    local pid t r w s run=0 wt=0 n=0 k=0
    pid="$(cat "$vm/qemu.pid" 2>/dev/null)" || pid=""
    if [ -n "$pid" ] && [ -d "/proc/$pid" ]; then
        for t in /proc/"$pid"/task/*; do
            grep -qE '^CPU [0-9]+/(KVM|TCG)$' "$t/comm" 2>/dev/null || continue
            read -r r w s < "$t/schedstat" || continue
            run=$((run + r)); wt=$((wt + w)); n=$((n + s)); k=$((k + 1))
        done
    fi
    echo "$run $wt $n $k ${pid:-none}"
}
read -r _ _ _ k pid <<< "$(vcpu_stat)"
if [ "$k" -eq 0 ]; then
    echo "no running QEMU with CPU n/KVM threads for $vm; start it with scripts/qemu-vm.sh start" >&2
    exit 1
fi
[ -x "$build/phila-sensord" ] && [ -x "$build/phila-monitord" ] || { echo "no QNX build in $build; run make qnx" >&2; exit 1; }

# A case still running in the guest (an interrupted run) would share the monitor's name, folder and
# programs, so nothing is copied while one runs.
running="$("$q" ssh "$vm" '{ pidin -p phila-monitord -f a; pidin -p phila-sensord -f a; true; }' 2>/dev/null || true)"
if grep -q '[0-9]' <<< "$running"; then
    echo "a phila-monitord or phila-sensord is still running in the guest; wait for it or slay it first" >&2
    exit 1
fi

"$q" ssh "$vm" 'mkdir -p /data/phila/bin /data/phila/runs'
for f in "$build"/phila-*; do "$q" put "$vm" "$f" "/data/phila/bin/$(basename "$f")"; done
"$q" put "$vm" "$here/scripts/run-case.sh" /data/phila/run-case.sh

load_before="$(cut -d' ' -f1-3 /proc/loadavg)"
read -r run0 wait0 n0 k0 pid0 <<< "$(vcpu_stat)"
"$q" ssh "$vm" "nohup sh /data/phila/run-case.sh stall-test designed $secs > /dev/null 2>&1 &"
t0="$(date +%s.%N)"
echo "$(date '+%H:%M:%S') stall test $name: designed, ${secs} s, no load; host load $load_before"

stall_lines=()
trap 'kill -CONT "$pid0" 2>/dev/null' EXIT   # never leave the VM frozen
trap 'exit 130' INT
trap 'exit 143' TERM
for s in "${stalls[@]}"; do
    read -r at len <<< "$s"
    while awk -v t0="$t0" -v at="$at" -v now="$(date +%s.%N)" 'BEGIN {exit !(now - t0 < at)}'; do sleep 0.05; done
    a="$(date +%s.%N)"
    kill -STOP "$pid0"
    sleep "$len"
    kill -CONT "$pid0"
    b="$(date +%s.%N)"
    stall_lines+=("$(awk -v t0="$t0" -v a="$a" -v b="$b" 'BEGIN {printf "%.3f s after the start, %.1f ms", a - t0, (b - a) * 1000}') (at $(date -d "@$a" '+%H:%M:%S.%3N'))")
    echo "  stall: ${stall_lines[-1]}"
done

sleep $((secs + 8 - ${stalls[-1]%% *}))
for _ in $(seq 1 60); do
    if "$q" ssh "$vm" "test -f /data/phila/runs/stall-test/done" 2>/dev/null; then break; fi
    sleep 5
done
read -r run1 wait1 n1 k pid1 <<< "$(vcpu_stat)"; t1="$(date +%s.%N)"
if [ "$k" -eq 0 ] || [ "$k" != "$k0" ] || [ "$pid1" != "$pid0" ]; then
    echo "  the VM's virtual CPU threads changed during the case; no host wait recorded" >&2
    k=0
fi
load_after="$(cut -d' ' -f1-3 /proc/loadavg)"
mkdir -p "$out"
rm -rf -- "${out:?}/$name" && mkdir -p "$out/$name"
for f in monitor.log sensor.log latency.csv record.jsonl uname.txt pidin-info.txt qtime.txt done; do
    "$q" get "$vm" "/data/phila/runs/stall-test/$f" "$out/$name/$f" 2>/dev/null || true
done
{
    printf 'host load before: %s\nhost load after: %s\n' "$load_before" "$load_after"
    printf 'started: %s\n' "$(date -d "@${t0%.*}" '+%Y-%m-%d %H:%M:%S %Z')"
    awk -v t0="$t0" -v t1="$t1" -v r0="$run0" -v r1="$run1" -v w0="$wait0" -v w1="$wait1" -v n0="$n0" -v n1="$n1" -v k="$k" 'BEGIN {
        printf "case seconds: %.1f\nvcpu threads: %d\n", t1 - t0, k
        if (k > 0) printf "vcpu run ms: %.1f\nvcpu wait ms: %.1f\nvcpu times run: %d\n", (r1 - r0) / 1e6, (w1 - w0) / 1e6, n1 - n0
        else printf "vcpu run ms: -\nvcpu wait ms: -\nvcpu times run: -\n" }'
    for l in "${stall_lines[@]}"; do printf 'stall: %s\n' "$l"; done
} > "$out/$name/host.txt"
sed -n '2,6p' "$out/$name/monitor.log" 2>/dev/null || echo "  (no monitor log)"

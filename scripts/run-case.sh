#!/bin/sh
# run-case.sh - one latency case, run inside the QNX target so that nothing
# depends on the network while a CPU hog holds the processors.
#
#   sh run-case.sh LABEL MODE SECONDS [HOG_PRIO HOG_POLICY HOG_DELAY HOG_SECONDS HOG_THREADS]
#
# MODE: designed | designed-tick | naive. Results go to /data/phila/runs/LABEL/.
# SPDX-License-Identifier: MIT
L=$1 MODE=$2 DUR=$3
cd /data/phila || exit 1
OUT=runs/$L
rm -rf "$OUT" && mkdir -p "$OUT" || exit 1
MON="" SEN=""
case "$MODE" in
designed) ;;
designed-tick) SEN=--tick-timers ;;
naive) MON=--naive SEN=--naive ;;
*) echo "unknown mode $MODE" >&2; exit 2 ;;
esac
uname -a > "$OUT/uname.txt"
pidin info > "$OUT/pidin-info.txt" 2>&1
bin/phila-monitord --scenario probe $MON --duration "$DUR" --record "$OUT/record.jsonl" \
    --latency-csv "$OUT/latency.csv" > "$OUT/monitor.log" 2>&1 &
sleep 1
bin/phila-sensord --scenario probe $SEN --duration $((DUR + 5)) > "$OUT/sensor.log" 2>&1 &
if [ -n "$4" ]; then
    # Snapshots taken at priority 45 so they still run while a hog holds the CPUs.
    on -p 45 sh -c "sleep $(($6 + 5)); cat /dev/phila/threads > $OUT/threads-under-load.txt; \
        pidin -F '%a %b %N %p %J' > $OUT/pidin-under-load.txt; cat /dev/phila/latency > $OUT/latency-mid.txt" &
    sleep "$6"
    bin/phila-hog --prio "$4" --policy "$5" --seconds "$7" --threads "$8" > "$OUT/hog.log" 2>&1 &
fi
wait
date -u > "$OUT/done"

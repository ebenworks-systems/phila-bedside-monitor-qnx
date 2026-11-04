#!/bin/sh
# board-under-load.sh - watch the nurse's board while a CPU hog holds the
# processors, run inside the QNX target.
#
#   sh board-under-load.sh LABEL MODE SECONDS HOG_PRIO HOG_POLICY HOG_DELAY HOG_SECONDS HOG_THREADS
#
# The same probe run as run-case.sh, plus phila-display started with
# `on -p 40`, above the hog, writing one frame a second to /dev/shmem (RAM, so
# no disk driver below the hog can hold it up). A hog at priority 35 starves
# io-sock and sshd, so nothing can be watched over SSH while it runs; fetch
# runs/LABEL/board.txt afterwards and replay it one frame a second.
#
# Use it to see the board, not to measure: a second reader at priority 40
# changes what the latency cases measure, so run-case.sh never starts one.
# MODE: designed | naive. Results go to /data/phila/runs/LABEL/.
# SPDX-License-Identifier: MIT
L=$1 MODE=$2 DUR=$3
[ -n "$8" ] || { sed -n '5p' "$0" >&2; exit 2; }
cd /data/phila || exit 1
OUT=runs/$L
rm -rf "$OUT" && mkdir -p "$OUT" || exit 1
MON="" SEN=""
case "$MODE" in
designed) ;;
naive) MON=--naive SEN=--naive ;;
*) echo "unknown mode $MODE" >&2; exit 2 ;;
esac
BOARD=/dev/shmem/phila-board-$L.txt
PIDIN=/dev/shmem/phila-pidin-$L.txt
rm -f "$BOARD" "$PIDIN"
/data/phila/bin/phila-monitord --scenario probe $MON --duration "$DUR" --record "$OUT/record.jsonl" \
    --latency-csv "$OUT/latency.csv" > "$OUT/monitor.log" 2>&1 &
sleep 1
/data/phila/bin/phila-sensord --scenario probe $SEN --duration $((DUR + 5)) > "$OUT/sensor.log" 2>&1 &
on -p 40 /data/phila/bin/phila-display > "$BOARD" 2>&1 &
DISP=$!
on -p 45 sh -c "sleep $(($6 + 5)); pidin -F '%a %b %N %p %J' > $PIDIN" &
sleep "$6"
/data/phila/bin/phila-hog --prio "$4" --policy "$5" --seconds "$7" --threads "$8" > "$OUT/hog.log" 2>&1 &
sleep $((DUR - $6 + 2))
kill "$DISP" 2>/dev/null
wait
cp "$BOARD" "$OUT/board.txt" && rm -f "$BOARD"
cp "$PIDIN" "$OUT/pidin-under-load.txt" && rm -f "$PIDIN"
date -u > "$OUT/done"

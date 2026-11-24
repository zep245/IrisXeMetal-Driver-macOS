#!/bin/bash
# IrisXe bigger-GPU-jobs test (v0.68.33+). Changes nothing permanently (a restart resets everything anyway).
# Tries, one at a time for 8 seconds each, while you drag the map in Maps:
#   1. smart cache flushing (with the normal 4 draws per GPU job)
#   2. 5 draws per GPU job, 3. 6 draws, 4. 7 draws, 5. 8 draws
# It stops at the FIRST GPU hang and puts everything back to normal, so the screen can freeze for 2-3 seconds
# at most once. Records what hung (the draw, its shaders, the GPU state) for the fix.
#     sudo bash irisxe-batchtest.sh
# Results: a zip in the same folder as this script.
cd "$(dirname "$0")" || exit 1
if [ "$(id -u)" != 0 ]; then echo "Run with sudo: sudo bash irisxe-batchtest.sh"; exit 1; fi
T=$(date +%Y%m%d-%H%M%S)
OUT="irisxe-batch-$T"
mkdir -p "$OUT"
resets() { ioreg -l -w0 | grep '"IrisXe-Resets"' | head -1 | sed -E 's/.*= "([0-9]+) resets.*/\1/'; }
props() { ioreg -l -w0 | grep -E '"IrisXe-(Resets|HungDraws|GPU|Metal|FrameTiming|KernelTiming|AsyncState|AccelBatch|BatchCfg|Batch|LastDraw|DrawPerf|Flush)[A-Za-z]*"' | sed -E 's/^[ |]*//'; }
OLD_BATCH=$(sysctl -n debug.irisxe_batch 2>/dev/null || echo 0)
normal() { sysctl debug.irisxe_flush=0 > /dev/null 2>&1; sysctl debug.irisxe_batch=$OLD_BATCH > /dev/null 2>&1; }
trap normal EXIT
R0=$(resets); [ -z "$R0" ] && R0=0
echo "resets before: $R0" > "$OUT/batch.txt"
props > "$OUT/before.txt"
echo "IrisXe bigger-jobs test: up to 5 rounds of 8 seconds. Open Maps now."
read -r -p "Press Enter, then keep dragging the map until it says DONE..." _
HUNG=""
for step in "flush 1 0" "batch5 0 5" "batch6 0 6" "batch7 0 7" "batch8 0 8"; do
  set -- $step
  sysctl debug.irisxe_flush=$2 > /dev/null 2>&1
  sysctl debug.irisxe_batch=$3 > /dev/null 2>&1
  sysctl debug.irisxe_timing=1 > /dev/null 2>&1
  S=$(date +%s)
  echo "  testing $1 (8 s) - keep dragging..."
  sleep 8
  normal
  R=$(resets); [ -z "$R" ] && R=$R0
  { echo "=== $1: resets now $R"; props | grep -E "FrameTiming|KernelTiming"; } >> "$OUT/batch.txt"
  if [ "$R" != "$R0" ]; then
    HUNG=$1
    echo "  -> $1 hung the GPU. Stopped; everything is back to normal."
    sleep 2
    log show --start "$(date -r $S '+%Y-%m-%d %H:%M:%S')" --style compact \
      --predicate 'process == "kernel" AND (eventMessage CONTAINS "IrisXe")' 2>/dev/null | grep -v "display pipe:" > "$OUT/hang-kernel-log.txt"
    log show --start "$(date -r $S '+%Y-%m-%d %H:%M:%S')" --style compact \
      --predicate 'eventMessage CONTAINS "IrisXeMTL" AND NOT eventMessage CONTAINS "newBuffer"' 2>/dev/null | tail -4000 > "$OUT/hang-metal-log.txt"
    props > "$OUT/after-hang.txt"
    ioreg -l -w0 | grep '"IrisXe-' | sed -E 's/^[ |]*//' > "$OUT/all-properties.txt"
    break
  fi
  echo "  $1 ok"
done
echo "first hang: ${HUNG:-none}" >> "$OUT/batch.txt"
sw_vers >> "$OUT/batch.txt"
zip -qr "$OUT.zip" "$OUT"
chown "$(stat -f %Su /dev/console)" "$OUT.zip" 2>/dev/null
echo "DONE. First hang: ${HUNG:-none}. Send this file: $(pwd)/$OUT.zip"

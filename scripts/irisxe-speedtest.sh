#!/bin/bash
# IrisXe speed test (v0.68.43+). Changes nothing permanently: the switches go back to normal at the end
# (and a restart resets them anyway).
# Tries the driver's speed settings one at a time, 20 seconds each, while you do the SAME thing each time (drag
# the map around in Maps, or scroll a long page in Safari). Records frames per second, stutters and how long the
# GPU work takes, for each setting:
#   A = no GPU worker, full flushing between draws
#   B = no GPU worker, smart flushing
#   C = GPU worker + smart flushing (the v0.68.43 default)
#   D = GPU worker, full flushing
# (background completion and 6 draws per GPU job stay on in all four)
cd "$(dirname "$0")" || exit 1
if [ "$(id -u)" != 0 ]; then echo "Run with sudo: sudo bash irisxe-speedtest.sh"; exit 1; fi
T=$(date +%Y%m%d-%H%M%S)
OUT="irisxe-speed-$T"
mkdir -p "$OUT"
props() { ioreg -l -w0 | grep -E '"IrisXe-(FrameTiming|KernelTiming|AsyncState|AccelBatch|HungDraws|DrawPhases|Worker)"' | sed -E 's/^[ |]*//'; }
phase() {   # name worker flush
  sysctl debug.irisxe_worker=$2 > /dev/null 2>&1
  sysctl debug.irisxe_flush=$3 > /dev/null 2>&1
  echo
  echo "=== Setting $1 ==="
  echo "Get ready: in 5 seconds start dragging the map in Maps (or scrolling in Safari) and keep doing it for 20 seconds."
  sleep 5
  sysctl debug.irisxe_timing=1 > /dev/null 2>&1
  S=$(date +%s)
  echo "GO - keep dragging / scrolling ..."
  for i in 20 15 10 5; do echo "  $i seconds left"; sleep 5; done
  E=$(date +%s)
  echo "STOP. (Setting $1 done)"
  { echo "=== Setting $1: worker $2, flush $3 ==="; props
    log show --start "$(date -r $S '+%Y-%m-%d %H:%M:%S')" --end "$(date -r $E '+%Y-%m-%d %H:%M:%S')" --style compact \
      --predicate 'process == "kernel" AND eventMessage CONTAINS "IrisXeAccel"' 2>/dev/null | grep -E "display pipe|failed" | sed -n '1p;$p'
    echo; } >> "$OUT/speed.txt"
}
OLD_WORKER=$(sysctl -n debug.irisxe_worker 2>/dev/null || echo 1)
OLD_FLUSH=$(sysctl -n debug.irisxe_flush 2>/dev/null || echo 1)
echo "IrisXe speed test: 4 rounds of 20 seconds. Open Maps (or Safari with a long page) now and put it on the screen."
read -r -p "Press Enter when ready..." _
phase A 0 0
phase B 0 1
phase C 1 1
phase D 1 0
sysctl debug.irisxe_worker=$OLD_WORKER > /dev/null 2>&1; sysctl debug.irisxe_flush=$OLD_FLUSH > /dev/null 2>&1
echo
echo "Which setting felt smoothest? Type A, B, C or D (or 'same') and press Enter:"
read -r FEEL
echo "felt smoothest: $FEEL" >> "$OUT/speed.txt"
sw_vers >> "$OUT/speed.txt"
zip -qr "$OUT.zip" "$OUT"
chown "$(stat -f %Su /dev/console)" "$OUT.zip" 2>/dev/null
echo "Done - everything is back to normal. Send this file: $(pwd)/$OUT.zip"

#!/bin/bash
# IrisXe CPU profile (v0.68.44+). Changes nothing. While you drag the map in Maps it records, for 8 seconds, where
# Maps and WindowServer spend their CPU time (macOS's own "sample" tool, call stacks) and how busy each process is.
# Shows whether the limit is our Metal driver code, Maps itself, WindowServer or the kernel driver.
#     sudo bash irisxe-profile.sh
cd "$(dirname "$0")" || exit 1
if [ "$(id -u)" != 0 ]; then echo "Run with sudo: sudo bash irisxe-profile.sh"; exit 1; fi
OUT="irisxe-profile-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT"
MAPS=$(pgrep -x Maps | head -1)
WS=$(pgrep -x WindowServer | head -1)
if [ -z "$MAPS" ]; then echo "Open Maps first, then run this again."; exit 1; fi
echo "In 5 seconds start dragging the map and keep going until it says STOP (about 12 seconds)."
sleep 5
echo "GO - keep dragging..."
sysctl debug.irisxe_timing=1 > /dev/null 2>&1
top -l 3 -s 3 -n 15 -o cpu -stats pid,command,cpu,threads > "$OUT/top.txt" &
TP=$!
sample "$MAPS" 8 1 -mayDie -file "$OUT/maps-sample.txt" > /dev/null 2>&1 &
SP1=$!
sample "$WS" 8 1 -mayDie -file "$OUT/windowserver-sample.txt" > /dev/null 2>&1 &
SP2=$!
wait $SP1 $SP2 $TP
echo "STOP."
{ echo "async $(sysctl -n debug.irisxe_async 2>/dev/null), flush $(sysctl -n debug.irisxe_flush 2>/dev/null), batch $(sysctl -n debug.irisxe_batch 2>/dev/null), worker $(sysctl -n debug.irisxe_worker 2>/dev/null)"
  ioreg -l -w0 | grep -E '"IrisXe-(FrameTiming|KernelTiming|DrawPhases|DrawPerf|GPUFreq|AccelMaps)"' | sed -E 's/^[ |]*//'
  sw_vers; } > "$OUT/driver.txt"
zip -qr "$OUT.zip" "$OUT"
chown "$(stat -f %Su /dev/console)" "$OUT.zip" 2>/dev/null
echo "Send this file: $(pwd)/$OUT.zip"

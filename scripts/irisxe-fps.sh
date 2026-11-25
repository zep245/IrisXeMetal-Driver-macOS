#!/bin/bash
# IrisXe quick speed check (v0.68.37+). Changes nothing: measures 20 seconds with the current settings while you
# drag the map in Maps (or scroll in Safari), then prints frames per second and where the time went.
#     sudo bash irisxe-fps.sh
cd "$(dirname "$0")" || exit 1
if [ "$(id -u)" != 0 ]; then echo "Run with sudo: sudo bash irisxe-fps.sh"; exit 1; fi
echo "Open Maps. In 5 seconds start dragging the map and keep going for 20 seconds."
sleep 5
sysctl debug.irisxe_timing=1 > /dev/null 2>&1
echo "GO - keep dragging..."
for i in 20 15 10 5; do echo "  $i seconds left"; sleep 5; done
echo "STOP."
OUT="irisxe-fps-$(date +%Y%m%d-%H%M%S).txt"
{ echo "async $(sysctl -n debug.irisxe_async 2>/dev/null), flush $(sysctl -n debug.irisxe_flush 2>/dev/null), batch $(sysctl -n debug.irisxe_batch 2>/dev/null), worker $(sysctl -n debug.irisxe_worker 2>/dev/null)"
  ioreg -l -w0 | grep -E '"IrisXe-(FrameTiming|KernelTiming|DrawPhases|AccelMaps|Resets|HungDraws|AsyncState|Worker)"' | sed -E 's/^[ |]*//'
  sw_vers; } > "$OUT"
chown "$(stat -f %Su /dev/console)" "$OUT" 2>/dev/null
grep -o 'moving: [^|]*' "$OUT"
echo "Send this file: $(pwd)/$OUT"

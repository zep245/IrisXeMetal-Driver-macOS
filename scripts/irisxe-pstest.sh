#!/bin/bash
# IrisXe v0.63.3 fill-rate test: why does plain drawing write only ~12 bytes per GPU clock?
# Runs irisxe-metaltest --pixelrate five times (about 40 seconds each, nothing else changes, no screen reset):
#   0 = our compiler's pixel shaders (as always)
#   1 = IrisXeCore's own constant-colour pixel shader, SIMD8 (8 pixels per thread, like our compiler's)
#   2 = the same, SIMD16 (16 pixels per thread = a whole 64-byte cache line of a tiled 4-byte image per write)
#   3 = our shaders, render targets with the GPU's last-level cache (LLC) bypassed (cache lines are not read first)
#   4 = our shaders, render targets with the GPU's L3 cache bypassed
# If 2 is much faster than 1, our shaders should run 16 pixels per thread. "picture ok" lines are expected to say
# WRONG in runs 1 and 2 (the test shader draws its own colour). Run from the release folder:
#     sudo bash irisxe-pstest.sh
# Results: ~/Downloads/irisxe-pstest.zip
U="${SUDO_USER:-prasad}"
OUT="/Users/$U/Downloads/irisxe-pstest"
HERE="$(cd "$(dirname "$0")" && pwd)"
if [ "$(id -u)" != "0" ]; then echo "Run it with sudo: sudo bash irisxe-pstest.sh"; exit 1; fi
if [ ! -x "$HERE/irisxe-metaltest" ]; then echo "Run it from the unzipped release folder (irisxe-metaltest must be next to it)."; exit 1; fi
if ! sysctl -n debug.irisxe_pstest > /dev/null 2>&1; then echo "IrisXeAccel is older than v0.63.2: install install-accel.sh and install-core.sh from this release (v0.63.3 or newer), restart, run this again."; exit 1; fi
rm -rf "$OUT" "/Users/$U/Downloads/irisxe-pstest.zip"; mkdir -p "$OUT"
{ sw_vers; kextstat 2>/dev/null | grep -i irisxe; ioreg -l -w0 | grep -E '"IrisXe-(GPU|GPUFreq|MOCS|PAT|Balance|L3)"' | sed 's/^[ |]*//'; } > "$OUT/0-status.txt" 2>&1
for m in 0 1 2 3 4; do
    echo "$(date +%H:%M:%S) run $m of 4 ..."
    sysctl debug.irisxe_pstest=$m > /dev/null
    "$HERE/irisxe-metaltest" --pixelrate > "$OUT/pixelrate-$m.txt" 2>&1
    ioreg -l -w0 | grep -E '"IrisXe-DrawPerf"' | sed 's/^[ |]*//' >> "$OUT/pixelrate-$m.txt"
done
sysctl debug.irisxe_pstest=0 > /dev/null
echo "BGRA8 (4 bytes), tiled render target:"
for m in 0 1 2 3 4; do
    printf "  run %s: %s\n" "$m" "$(grep -m1 'BGRA8 (4 bytes)' "$OUT/pixelrate-$m.txt" | sed 's/ *picture.*//')"
done
chown -R "$U" "$OUT"
cd "/Users/$U/Downloads" && zip -qr irisxe-pstest.zip irisxe-pstest && chown "$U" irisxe-pstest.zip
echo "Done: send ~/Downloads/irisxe-pstest.zip"

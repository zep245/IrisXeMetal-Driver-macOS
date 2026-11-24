#!/bin/bash
# Round 24 (v0.54.6): round 23 found CoreDisplay::Display::GetNextSurface gives the screen GPU images (DisplaySurfacePool,
# IOSurfaces) only when CoreDisplay::UseIOPresentment() is true AND the flag displaySurfaceUsesIOSurface is 1; otherwise
# it hands back the old framebuffer (CGXAcquireDisplayDeviceSurface). UseIOPresentment() needs useIOP = 1 and every GPU
# vendor in GetSystemGPUVendors() to be enabled (useIOP_onIG = 2, useIOP_onAMD = 4, useIOP_onNVIDIA = 8,
# useIOP_onAPV = 0x10); no vendor found at all = off. This round reads where those flags are set (preference keys?)
# and how GetSystemGPUVendors classifies a GPU. Text output only. Round 23 is kept as run23.sh.
set -x
OUT="$PWD/out"; mkdir -p "$OUT"
sw_vers > "$OUT/00-versions.txt" 2>&1
pip3 install --break-system-packages capstone > "$OUT/01-capstone.txt" 2>&1 || pip3 install capstone >> "$OUT/01-capstone.txt" 2>&1
xcrun clang -fno-objc-arc -O1 -framework Foundation -framework Metal -framework IOKit udis_server.m -o udis_server > "$OUT/40-build.txt" 2>&1
SL=/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight
CD=/System/Library/Frameworks/CoreDisplay.framework/CoreDisplay
IOP=/System/Library/PrivateFrameworks/IOPresentment.framework/IOPresentment
python3 udis.py "$OUT/f0-flags.txt" "load:$CD" "load:$IOP" "load:$SL" \
  'r:CoreDisplay:_ZN11CoreDisplayL6useIOPE' 'r:CoreDisplay:_ZN11CoreDisplayL11useIOP_onIGE' \
  'r:CoreDisplay:_ZN11CoreDisplayL27displaySurfaceUsesIOSurfaceE' > "$OUT/f0-log.txt" 2>&1
python3 udis.py "$OUT/f1-vendors.txt" "load:$CD" "load:$IOP" "load:$SL" \
  'y:CoreDisplay:(GetSystemGPUVendors|GPUVendor|gpuVendors)' 'r:CoreDisplay:_ZZ19GetSystemGPUVendorsE10gpuVendors' \
  --follow 'Vendor|GPU|Wrangler|IOService|Registry|CFProperty' > "$OUT/f1-log.txt" 2>&1
python3 udis.py "$OUT/f2-strings.txt" "load:$CD" "load:$IOP" "load:$SL" \
  'x:CoreDisplay:useIOP' 'x:CoreDisplay:IOPresentment' 'x:CoreDisplay:IOSurface' > "$OUT/f2-log.txt" 2>&1
strings -a "$CD" 2>/dev/null | grep -iE "iop|iosurface|presentment" | sort -u > "$OUT/f3-coredisplay-strings.txt"
ls -la "$OUT"
exit 0

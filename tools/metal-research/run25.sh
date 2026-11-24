#!/bin/bash
# Round 25 (v0.54.6): round 24 found GetSystemGPUVendors() classifies each IOFramebuffer by its CLASS NAME
# (GetGPUVendorForFramebufferService: four case-insensitive substrings -> IG 2, AMD 4, APV 0x10, NVIDIA 8, else 1) and
# drops the "unknown" bit. With only IrisXeFB, the vendor set is 0 and UseIOPresentment() turns itself off, so the screen
# never gets GPU images. This round reads the four substrings and the start values of the useIOP flags.
# Text output only. Round 24 is kept as run24.sh.
set -x
OUT="$PWD/out"; mkdir -p "$OUT"
sw_vers > "$OUT/00-versions.txt" 2>&1
pip3 install --break-system-packages capstone > "$OUT/01-capstone.txt" 2>&1 || pip3 install capstone >> "$OUT/01-capstone.txt" 2>&1
xcrun clang -fno-objc-arc -O1 -framework Foundation -framework Metal -framework IOKit udis_server.m -o udis_server > "$OUT/40-build.txt" 2>&1
SL=/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight
CD=/System/Library/Frameworks/CoreDisplay.framework/CoreDisplay
IOP=/System/Library/PrivateFrameworks/IOPresentment.framework/IOPresentment
python3 udis.py "$OUT/g0-vendor-names.txt" "load:$CD" "load:$IOP" "load:$SL" \
  'cfs:CoreDisplay:GetGPUVendorForFramebufferService' \
  'v:CoreDisplay:_ZN11CoreDisplayL6useIOPE:1' 'v:CoreDisplay:_ZN11CoreDisplayL11useIOP_onIGE:1' \
  'v:CoreDisplay:_ZN11CoreDisplayL12useIOP_onAMDE:1' 'v:CoreDisplay:_ZN11CoreDisplayL15useIOP_onNVIDIAE:1' \
  'v:CoreDisplay:_ZN11CoreDisplayL12useIOP_onAPVE:1' 'v:CoreDisplay:_ZN11CoreDisplayL27displaySurfaceUsesIOSurfaceE:1' \
  'v:CoreDisplay:_ZZ19GetSystemGPUVendorsE10gpuVendors:4' > "$OUT/g0-log.txt" 2>&1
ioreg -l -w0 -c IOFramebuffer | grep -E "class|IOClass" | head -20 > "$OUT/g1-runner-framebuffers.txt" 2>&1
ls -la "$OUT"
exit 0

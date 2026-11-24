#!/bin/bash
# Round 19 (v0.54.1): with IrisXeAGDC 0.54 every IOPresentment / CoreDisplay error is gone ("Valid ADGC node",
# 0x921 and 0x711 answered, no "Unable to find display pipe"), WindowServer says "Metal compositor activated",
# yet the screen is still composited by CompositorSW (CPU) into the old framebuffer. This round reads how SkyLight
# and CoreDisplay choose the compositor and the display destination (framebuffer vs IOPresentment surfaces).
# Text output only. Round 18 is kept as run18.sh.
set -x
OUT="$PWD/out"; mkdir -p "$OUT"
sw_vers > "$OUT/00-versions.txt" 2>&1
pip3 install --break-system-packages capstone > "$OUT/01-capstone.txt" 2>&1 || pip3 install capstone >> "$OUT/01-capstone.txt" 2>&1
xcrun clang -fno-objc-arc -O1 -framework Foundation -framework Metal -framework IOKit udis_server.m -o udis_server > "$OUT/40-build.txt" 2>&1
SL=/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight
CD=/System/Library/Frameworks/CoreDisplay.framework/CoreDisplay
IOP=/System/Library/PrivateFrameworks/IOPresentment.framework/IOPresentment
python3 udis.py "$OUT/a0-skylight-compositor.txt" "load:$CD" "load:$IOP" "load:$SL" \
  'x:SkyLight:Software compositor activated' 'x:SkyLight:Metal compositor activated' \
  'y:SkyLight:(CompositorSW|CompositorMetal|Compositor)::(Create|create|New|Make|Select|Choose|Init|init)' \
  'y:SkyLight:(CreateCompositeDestination|CompositeDestination.*Create|CreateDestination|WSCompositeDestinationCreate)' \
  'y:SkyLight:(UsesIOPresentment|IOPresentment|DisplayPipe|SupportsDirect|UseMetal|ShouldUseMetal|MetalCapable|SoftwareCompositor)' \
  --follow 'Compositor|Destination|Presentment|CoreDisplay_|Metal' --depth 1 > "$OUT/a0-log.txt" 2>&1
python3 udis.py "$OUT/a1-coredisplay-surface.txt" "load:$CD" "load:$IOP" \
  'y:CoreDisplay:(Display::GetNextSurface|Display::.*Surface.*|UsesIOPresentment|IsIOPresentment|Presentment.*Enabled|SupportsIOPresentment|CoreDisplay_Display_.*Presentment|HasDisplayPipe|SupportsMetal)' \
  --follow 'Presentment|Surface|DisplayPipe' --depth 1 > "$OUT/a1-log.txt" 2>&1
python3 udis.py "$OUT/a2-names.txt" "load:$CD" "load:$IOP" "load:$SL" \
  'y:SkyLight:(Compositor|Destination|Presentment)' > "$OUT/a2-log.txt" 2>&1
ls -la "$OUT"
exit 0

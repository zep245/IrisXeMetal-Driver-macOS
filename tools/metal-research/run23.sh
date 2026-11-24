#!/bin/bash
# Round 23 (v0.54.6): round 22 showed SkyLight's CDSurface::prepare_for_update asks CoreDisplay for the next display
# surface (CoreDisplay_Display_GetNextSurface[WithProtection]); if that surface has an IOSurface the screen is drawn by
# the GPU, if not WindowServer falls back to the old framebuffer + CPU compositor. Samples 3 and 4 (display pipe on)
# called CoreDisplay::Display::GetNextSurface and still got the old framebuffer. This round reads GetNextSurface and
# everything that decides whether a CoreDisplay display gets a DisplaySurfacePool (IOSurfaces).
# Text output only. Round 22 is kept as run22.sh.
set -x
OUT="$PWD/out"; mkdir -p "$OUT"
sw_vers > "$OUT/00-versions.txt" 2>&1
pip3 install --break-system-packages capstone > "$OUT/01-capstone.txt" 2>&1 || pip3 install capstone >> "$OUT/01-capstone.txt" 2>&1
xcrun clang -fno-objc-arc -O1 -framework Foundation -framework Metal -framework IOKit udis_server.m -o udis_server > "$OUT/40-build.txt" 2>&1
SL=/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight
CD=/System/Library/Frameworks/CoreDisplay.framework/CoreDisplay
IOP=/System/Library/PrivateFrameworks/IOPresentment.framework/IOPresentment
python3 udis.py "$OUT/e0-getnextsurface.txt" "load:$CD" "load:$IOP" "load:$SL" \
  'y:CoreDisplay:(Display14GetNextSurface|Display_GetNextSurface|Display.*(SurfacePool|[Ff]lip|UseIOSurface|UsesIOSurface|DirectDisplay|Composit))' \
  --follow 'Surface|Pool|Flip|Presentment|Pipe|Capab' > "$OUT/e0-log.txt" 2>&1
python3 udis.py "$OUT/e1-pool-makers.txt" "load:$CD" "load:$IOP" "load:$SL" \
  'k:CoreDisplay:_ZN11CoreDisplay18DisplaySurfacePoolC1EN3gsl8not_nullIPKNS_15AbstractDisplayEEEib' \
  'k:CoreDisplay:_ZN11CoreDisplay18DisplaySurfacePool14GetNextSurfaceEiiP15CGSRegionObjectS2_S2_ddjP12CGColorSpaceby' \
  --follow 'Surface|Pool|Flip|Presentment|Pipe|Capab' > "$OUT/e1-log.txt" 2>&1
ls -la "$OUT"
exit 0

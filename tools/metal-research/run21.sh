#!/bin/bash
# Round 21 (v0.54.6): in WindowServer, new_compositor(83235) finds our device (MATCH), so a CompositorMetal for our GPU
# exists - yet every screen update (CDDisplay::render_update) still goes to CompositorSW. This round finds where a
# display's Compositor* is chosen: every caller of WSGetCompositorForAccelerator / WSGetCompositorMetal, every user of
# the CompositorSW singleton, and CDDisplay's compositor / accelerator functions.
# Text output only. Round 20 is kept as run20.sh.
set -x
OUT="$PWD/out"; mkdir -p "$OUT"
sw_vers > "$OUT/00-versions.txt" 2>&1
pip3 install --break-system-packages capstone > "$OUT/01-capstone.txt" 2>&1 || pip3 install capstone >> "$OUT/01-capstone.txt" 2>&1
xcrun clang -fno-objc-arc -O1 -framework Foundation -framework Metal -framework IOKit udis_server.m -o udis_server > "$OUT/40-build.txt" 2>&1
SL=/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight
CD=/System/Library/Frameworks/CoreDisplay.framework/CoreDisplay
IOP=/System/Library/PrivateFrameworks/IOPresentment.framework/IOPresentment
python3 udis.py "$OUT/c0-callers.txt" "load:$CD" "load:$IOP" "load:$SL" \
  'k:SkyLight:WSGetCompositorForAccelerator' 'k:SkyLight:WSGetCompositorMetal' \
  --follow 'Compositor|Accelerator|accelerator' > "$OUT/c0-log.txt" 2>&1
python3 udis.py "$OUT/c1-compositor-sw.txt" "load:$CD" "load:$IOP" "load:$SL" \
  'r:SkyLight:_ZL13compositor_sw' 'r:SkyLight:_ZL17active_compositor' > "$OUT/c1-log.txt" 2>&1
python3 udis.py "$OUT/c2-cddisplay.txt" "load:$CD" "load:$IOP" "load:$SL" \
  'y:SkyLight:(CDDisplay|Displays.*Display).*([Cc]ompositor|[Aa]ccelerator|[Mm]etal|IOSurface|[Ff]ramebuffer)' \
  'y:SkyLight:(CDDisplay).*(create|init|C1|C2|setup|configure)' > "$OUT/c2-log.txt" 2>&1
ls -la "$OUT"
exit 0

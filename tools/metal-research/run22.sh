#!/bin/bash
# Round 22 (v0.54.6): round 21 showed CDDisplay::render_update draws into the old framebuffer (CGXAccessDisplayDeviceSurface)
# when the update's surface object has byte +0x40 = 1, and into a CoreDisplay DisplaySurface (Metal texture / IOSurface,
# i.e. the GPU path) otherwise. Every sample so far took the old-framebuffer path, even with the display pipe made.
# This round reads where that surface is chosen: SkyLight's display-surface functions and CoreDisplay's DisplaySurface
# creation, and what CoreDisplay checks before it makes one.
# Text output only. Round 21 is kept as run21.sh.
set -x
OUT="$PWD/out"; mkdir -p "$OUT"
sw_vers > "$OUT/00-versions.txt" 2>&1
pip3 install --break-system-packages capstone > "$OUT/01-capstone.txt" 2>&1 || pip3 install capstone >> "$OUT/01-capstone.txt" 2>&1
xcrun clang -fno-objc-arc -O1 -framework Foundation -framework Metal -framework IOKit udis_server.m -o udis_server > "$OUT/40-build.txt" 2>&1
SL=/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight
CD=/System/Library/Frameworks/CoreDisplay.framework/CoreDisplay
IOP=/System/Library/PrivateFrameworks/IOPresentment.framework/IOPresentment
python3 udis.py "$OUT/d0-skylight-surface.txt" "load:$CD" "load:$IOP" "load:$SL" \
  'y:SkyLight:([Dd]isplay_?[Ss]urface|DeviceSurface|[Ss]urface_?[Ff]or_?[Dd]isplay|update_surface|CreateUpdate|begin_update|BeginUpdate)' \
  'k:SkyLight:CGXAccessDisplayDeviceSurface' \
  --follow 'Surface|surface' > "$OUT/d0-log.txt" 2>&1
python3 udis.py "$OUT/d1-coredisplay-surface.txt" "load:$CD" "load:$IOP" "load:$SL" \
  'y:CoreDisplay:(DisplaySurface.*(Create|Init|C1|C2|Get|Prepare|Supports|Enable)|CreateDisplaySurface|[Ss]urface[Ss]upport|UsesDisplaySurface|SupportsMetal|IsMetal)' \
  --follow 'Surface|Presentment|Pipe|Metal' > "$OUT/d1-log.txt" 2>&1
python3 udis.py "$OUT/d2-coredisplay-strings.txt" "load:$CD" "load:$IOP" "load:$SL" \
  'x:CoreDisplay:DisplaySurface' 'x:CoreDisplay:display surface' 'x:CoreDisplay:Unable to find display pipe' > "$OUT/d2-log.txt" 2>&1
ls -la "$OUT"
exit 0

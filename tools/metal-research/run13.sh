#!/bin/bash
# Round 13 (display pipe, user space): the kernel now has a display pipe for IrisXeFB (v0.51.42) and the accelerator
# declares IOAccelDisplayPipeCapabilities (v0.51.43), yet WindowServer still logs "Unable to find display pipe for
# IOFB service". Which checks in CoreDisplay / SkyLight lead to that message and to the CPU compositor fallback
# ("Unsupported sw composite destination")? Text output only (annotated disassembly of the functions that use those
# strings, plus their direct calls). Round 12 is kept as run12.sh.
set -x
OUT="$PWD/out"; mkdir -p "$OUT"
sw_vers > "$OUT/00-versions.txt" 2>&1
pip3 install --break-system-packages capstone > "$OUT/01-capstone.txt" 2>&1 || pip3 install capstone >> "$OUT/01-capstone.txt" 2>&1
xcrun clang -fno-objc-arc -O1 -framework Foundation -framework Metal -framework IOKit udis_server.m -o udis_server > "$OUT/40-build.txt" 2>&1
CD=/System/Library/Frameworks/CoreDisplay.framework/CoreDisplay
SL=/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight
ls -la /System/Library/Frameworks/CoreDisplay.framework /System/Library/PrivateFrameworks/SkyLight.framework > "$OUT/41-paths.txt" 2>&1
F='^\?$|IOService|IORegistry|IOConnect|IOAccel|IOIterator|IOObject|CFDictionary|CFBoolean|DisplayPipe|Pipe'
python3 udis.py "$OUT/50-coredisplay-pipe.txt" "load:$CD" "load:$SL" \
  'x:CoreDisplay:Unable to find display pipe' \
  'x:CoreDisplay:IOAccelDisplayPipeCapabilities' \
  'x:CoreDisplay:DisplayPipeSupported' \
  'x:CoreDisplay:TransactionsSupported' \
  'x:CoreDisplay:IOAccelDisplayPipe' \
  --follow "$F" --depth 2 > "$OUT/50-log.txt" 2>&1
python3 udis.py "$OUT/51-skylight-sw.txt" "load:$CD" "load:$SL" \
  'x:SkyLight:Unsupported sw composite destination' \
  'x:SkyLight:Metal compositor activated' \
  'x:SkyLight:Software compositor activated' \
  'x:SkyLight:DisplayPipeSupported' \
  --follow '^\?$|Display|Pipe|Compositor|CoreDisplay' --depth 1 > "$OUT/51-log.txt" 2>&1
ls -la "$OUT"
exit 0

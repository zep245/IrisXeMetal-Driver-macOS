#!/bin/bash
# Round 15 (display pipe, user space 3): round 14 listed CoreDisplay's display pipe code (CoreDisplay::DisplayPipe
# works through IOPresentment: IOPAttachment, IOPTransaction) and found 9 functions that use the display pipe
# table gDisplayPipes, but the disassembler stopped after the first one (fixed: reads past an image's end).
# Now: the functions that fill gDisplayPipes, and IOPresentment's functions that find a display / framebuffer.
# Text output only. Round 14 is kept as run14.sh.
set -x
OUT="$PWD/out"; mkdir -p "$OUT"
sw_vers > "$OUT/00-versions.txt" 2>&1
pip3 install --break-system-packages capstone > "$OUT/01-capstone.txt" 2>&1 || pip3 install capstone >> "$OUT/01-capstone.txt" 2>&1
xcrun clang -fno-objc-arc -O1 -framework Foundation -framework Metal -framework IOKit udis_server.m -o udis_server > "$OUT/40-build.txt" 2>&1
CD=/System/Library/Frameworks/CoreDisplay.framework/CoreDisplay
SL=/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight
IOP=/System/Library/PrivateFrameworks/IOPresentment.framework/IOPresentment
ls -la /System/Library/PrivateFrameworks/IOPresentment.framework > "$OUT/41-paths.txt" 2>&1
F='^\?$|IOP|Presentment|IOService|IORegistry|IOConnect|IOAccel|DisplayPipe|emplace'
python3 udis.py "$OUT/54-coredisplay-pipe-table.txt" "load:$CD" "load:$SL" "load:$IOP" \
  'r:CoreDisplay:_ZN11CoreDisplay13gDisplayPipesE' \
  --follow "$F" --depth 1 > "$OUT/54-log.txt" 2>&1
# IOPresentment: names first (everything), then the functions that look up displays / framebuffers / accelerators
python3 udis.py "$OUT/55-iopresentment-names.txt" "load:$CD" "load:$IOP" 'y:IOPresentment:.' > "$OUT/55-log.txt" 2>&1
grep '^#   ' "$OUT/55-iopresentment-names.txt" > "$OUT/55-names-only.txt"
rm -f "$OUT/55-iopresentment-names.txt"
python3 udis.py "$OUT/56-iopresentment-find.txt" "load:$CD" "load:$IOP" \
  'y:IOPresentment:(Create|Find|Match|Lookup|Copy|Get)[A-Za-z]*(Display|Framebuffer|FB|Service|Accel|Pipe|Attachment)' \
  'x:IOPresentment:IOAccelDisplayPipe' \
  'x:IOPresentment:IOFramebuffer' \
  'x:IOPresentment:DisplayPipeSupported' \
  --follow '^\?$|IOService|IORegistry|IOConnect|IOAccel|IOP' --depth 1 > "$OUT/56-log.txt" 2>&1
ls -la "$OUT"
exit 0

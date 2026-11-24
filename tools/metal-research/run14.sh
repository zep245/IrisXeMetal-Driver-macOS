#!/bin/bash
# Round 14 (display pipe, user space 2): round 13 showed that CoreDisplay::FindDisplayPipeForService looks the
# framebuffer's registry entry ID up in a table of display pipes (CoreDisplay::gDisplayPipes). Who fills that
# table, and from what (which IOKit services, properties, user-client calls)? Text output only.
# Round 13 is kept as run13.sh.
set -x
OUT="$PWD/out"; mkdir -p "$OUT"
sw_vers > "$OUT/00-versions.txt" 2>&1
pip3 install --break-system-packages capstone > "$OUT/01-capstone.txt" 2>&1 || pip3 install capstone >> "$OUT/01-capstone.txt" 2>&1
xcrun clang -fno-objc-arc -O1 -framework Foundation -framework Metal -framework IOKit udis_server.m -o udis_server > "$OUT/40-build.txt" 2>&1
CD=/System/Library/Frameworks/CoreDisplay.framework/CoreDisplay
SL=/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight
F='^\?$|IOService|IORegistry|IOConnect|IOAccel|IOIterator|IOObject|IOKit|DisplayPipe|Pipe|CFDictionary|CFBoolean|CFNumber'
# every CoreDisplay symbol about display pipes (names only first, so nothing is missed)
python3 udis.py "$OUT/52-coredisplay-pipe-names.txt" "load:$CD" "load:$SL" \
  'y:CoreDisplay:[Dd]isplayPipe|gDisplayPipes|Accel' > "$OUT/52-log.txt" 2>&1
grep '^#   ' "$OUT/52-coredisplay-pipe-names.txt" > "$OUT/52-names-only.txt"
# functions that use the display pipe table, and the display pipe constructors / setup
python3 udis.py "$OUT/53-coredisplay-pipe-table.txt" "load:$CD" "load:$SL" \
  'r:CoreDisplay:_ZN11CoreDisplay13gDisplayPipesE' \
  'y:CoreDisplay:_ZN11CoreDisplay11DisplayPipeC[12]|_ZN11CoreDisplay11DisplayPipe(4Init|5Setup|4Open|6Create)|AddDisplayPipe|InitDisplayPipes|InitializeDisplayPipes|CreateDisplayPipes|UpdateDisplayPipes' \
  --follow "$F" --depth 2 > "$OUT/53-log.txt" 2>&1
ls -la "$OUT"
exit 0

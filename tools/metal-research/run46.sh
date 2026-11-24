#!/bin/bash
# Round 45 + v0.68.7..v0.68.11 results: Apple's VA driver opens the decoder and submits jobs, but its sideband holds
# only the family's Start + BindDataBuffer (+ End) tokens and its command buffer is empty: its user-space side builds
# no work at all, silently. Round 46 (hardware video decoding, part 14): the WHOLE VA driver, disassembled by otool
# with its stubs and C strings annotated, plus its symbols, load commands and Objective-C metadata - so the token
# writers (0x80xx / 0x81xx / 0x82xx) and the checks before them can be searched offline. Round 45 = run45.sh.
set -x
OUT="$PWD/out"; mkdir -p "$OUT"
sw_vers > "$OUT/00-versions.txt" 2>&1
VA=/System/Library/Extensions/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/AppleIntelICLGraphicsVADriver
ls -la "$VA" > "$OUT/01-va.txt" 2>&1
otool -l "$VA" > "$OUT/02-va-loadcmds.txt" 2>&1
nm -m "$VA" > "$OUT/03-va-nm.txt" 2>&1
otool -tV "$VA" > "$OUT/10-va-text.txt" 2>&1
otool -s __TEXT __cstring -v "$VA" > "$OUT/11-va-cstrings.txt" 2>&1
otool -s __TEXT __const "$VA" > "$OUT/12-va-const.txt" 2>&1
otool -s __DATA_CONST __const "$VA" > "$OUT/13-va-dataconst.txt" 2>&1
otool -s __DATA __data "$VA" > "$OUT/14-va-data.txt" 2>&1
# the VA bundle's own plist (codec support tables)
plutil -convert xml1 -o - "$(dirname "$(dirname "$VA")")/Info.plist" > "$OUT/04-va-info.txt" 2>&1
gzip -9 "$OUT/10-va-text.txt"
ls -la "$OUT"
exit 0

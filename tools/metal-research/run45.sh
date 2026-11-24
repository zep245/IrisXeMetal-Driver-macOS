#!/bin/bash
# Round 44 results: the RO private part is sized by IOAccelConfig +0x58 (Intel: 0x28). v0.68.5 test boot: the
# hardware decoder session OPENED, 30 of 30 frames came back (black: nothing ran), yet our video context recorded NO
# Intel token at all: the VA driver called context selectors 2 and 5 many times; mediaanalysisd logged "AVD error:
# Gen75IntelGVAScheduleAVDCommands: Command[0] err = 11".
# Round 45 (hardware video decoding, part 13): the submit path - what IGAccelVideoContext overrides beyond ours
# (populateContextConfig, contextStart, add/removeDataBufferToChannel, beginCommandStream, endCommandStream,
# processSidebandToken, attach), the family's context selectors table and IOAccelContext2's submit / data buffer /
# sideband code; and in the VA driver the function that logs "Command[%d] err".
set -x
OUT="$PWD/out"; mkdir -p "$OUT"
sw_vers > "$OUT/00-versions.txt" 2>&1
python3 -c "import capstone" 2>/dev/null || pip3 install --user capstone > "$OUT/01-capstone.txt" 2>&1 || pip3 install --break-system-packages capstone >> "$OUT/01-capstone.txt" 2>&1
KC=/System/Library/KernelCollections
A="$KC/BootKernelExtensions.kc $KC/SystemKernelExtensions.kc"
F=com.apple.iokit.IOAcceleratorFamily2
I=com.apple.driver.AppleIntelICLGraphics
python3 kcdump.py "$OUT" $A -- $F $I > "$OUT/03-kcdump-stdout.txt" 2>&1
for f in "$OUT"/12-*-vtables.txt "$OUT"/10-*-symbols.txt; do [ -f "$f" ] && c++filt < "$f" > "$f.tmp" && mv "$f.tmp" "$f"; done
rm -f "$OUT"/11-*-segments.txt "$OUT"/02-*.txt
VT="--vt IOAccelContext2 --vt IOAccelVideoContext2 --vt IGAccelVideoContext --vt IOGraphicsAccelerator2 --vt IntelAccelerator --vt IOAccelResource2"
python3 kcdis.py "$OUT/a0-igvideo.txt" "$OUT" $A -- \
  "$I:^IGAccelVideoContext::(populateContextConfig|contextStart|contextStop|addDataBufferToChannel|removeDataBufferFromChannel|removeCurrentResourceFromChannel|beginCommandStream|endCommandStream|processSidebandToken|discardSidebandToken|attach|set_current_resource|getDataBufferLimits)" $VT > "$OUT/a0-log.txt" 2>&1
python3 kcdis.py "$OUT/a1-family.txt" "$OUT" $A -- \
  "$F:^IOAccelContext2::(getTargetAndMethodForIndex|externalMethod|s_submit|submit|processDataBuffers|processSidebandBuffer|processSidebandToken|beginCommandStream|endCommandStream|populateContextConfig|contextStart|addDataBufferToChannel|s_finish|finish|s_get_data_buffer|getDataBuffer\(|requestDataBuffer|s_set|validateDataBuffer)" \
  "$F:^IOAccelVideoContext2::" $VT > "$OUT/a1-log.txt" 2>&1
grep -n "IOAccelContext2::sContextMethods\|IOAccelContext2::s[A-Z]" "$OUT"/10-$F-symbols.txt > "$OUT/a2-tables.txt" 2>&1
xcrun clang -fno-objc-arc -O1 -framework Foundation -framework Metal -framework IOKit udis_server.m -o udis_server > "$OUT/40-build.txt" 2>&1
VA=/System/Library/Extensions/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/AppleIntelICLGraphicsVADriver
python3 udis.py "$OUT/50-va-schedule.txt" "load:$VA" \
  'x:AppleIntelICLGraphicsVADriver:ScheduleAVDCommands' \
  'x:AppleIntelICLGraphicsVADriver:err = ' \
  --follow '.' --depth 1 > "$OUT/50-log.txt" 2>&1
python3 udis.py "$OUT/51-ioaccel-video.txt" \
  'load:/System/Library/PrivateFrameworks/IOAccelerator.framework/IOAccelerator' \
  's:IOAccelVideoContextSubmitDataBuffers' 's:IOAccelVideoContextRequestDataBuffer' 's:IOAccelVideoContextFinishFenceEvent' \
  --follow '(?i)(IOConnect|video|context)' --depth 1 > "$OUT/51-log.txt" 2>&1
rm -f "$OUT"/13-*-strings.txt "$OUT"/12-*-vtables.txt
ls -la "$OUT"
exit 0

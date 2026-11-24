#!/bin/bash
# Round 46 results: the whole VA driver (otool). Its submit code (0x9dc1) re-acquires its data buffers per class after
# each IOAccelVideoContextSubmitDataBuffers and writes the Start token itself; the End token carries the command
# offset - our streams show offset 0. Apple's IntelAccelerator::newContext makes THREE classes, not one:
# IGAccelVideoContextMain / Media / VEBox, each overriding processDataBuffers, processSidebandToken, begin/end
# CommandStream, getTargetAndMethodForIndex and contextStart (own token tables). Round 47 (hardware video decoding,
# part 15): those subclass methods in full, with their token tables. Round 46 is kept as run46.sh.
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
VT="--vt IOAccelContext2 --vt IOAccelVideoContext2 --vt IGAccelVideoContext --vt IGAccelVideoContextMain --vt IGAccelVideoContextMedia --vt IGAccelVideoContextVEBox --vt IOGraphicsAccelerator2 --vt IntelAccelerator --vt IOAccelResource2"
python3 kcdis.py "$OUT/a0-subclasses.txt" "$OUT" $A -- \
  "$I:^IGAccelVideoContext(Main|Media|VEBox)::(contextStart|processDataBuffers|processSidebandToken|discardSidebandToken|beginCommandStream|endCommandStream|endScalableCommandStreamFE_BE|getTargetAndMethodForIndex)" \
  "$I:^IGAccelVideoContext(Main|Media|VEBox)::contextStart\(\)::token_" \
  "$I:^IGAccelVideoContextMedia::process_token_AVCDecode" \
  "$I:^IGAccelVideoContext::(process_token_SetFence|process_token_BatchBufferStart|set_current_resource|get_resource_gpu_address|patch_codechal_command_buffer)" $VT > "$OUT/a0-log.txt" 2>&1
python3 kcdis.py "$OUT/a1-family-bind.txt" "$OUT" $A -- \
  "$F:^IOAccelContext2::(process_token_BindDataBuffer|process_token_Start|process_token_End|validateTokenSize|initSidebandBufferHeader|getDataBufferLimits)" $VT > "$OUT/a1-log.txt" 2>&1
rm -f "$OUT"/13-*-strings.txt "$OUT"/12-*-vtables.txt
ls -la "$OUT"
exit 0

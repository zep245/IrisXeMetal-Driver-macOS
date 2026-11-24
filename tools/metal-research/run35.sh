#!/bin/bash
# Round 35 (v0.67.0): hardware video decoding, part 3 - the exact interface. Round 34: the ICL VA driver talks to the
# kext only through IOAccelerator's video context (IOAccelVideoContextRequestDataBuffer / SubmitDataBuffers /
# sideband / fence buffers); the kext's IGAccelVideoContext(-Media) subclasses the family's IOAccelVideoContext2 and
# handles a few command-stream tokens. This round reads, as text:
#   1. IOAcceleratorFamily2's IOAccelVideoContext2 (the base class our video context will subclass): every function,
#      and how IOGraphicsAccelerator2 creates it (newUserClient -> newVideoContext)
#   2. the ICL kext's tables, now decoded as data: method_descs (the video context's extra methods) and the token
#      handler tables (token number -> handler), and its token handlers / address patching with the base vtable
#   3. IOAccelerator.framework's IOAccelVideoContext* functions (user space): selectors and argument sizes
# Text output only. Round 34 is kept as run34.sh.
set -x
OUT="$PWD/out"; mkdir -p "$OUT"
sw_vers > "$OUT/00-versions.txt" 2>&1
pip3 install --break-system-packages capstone > "$OUT/01-capstone.txt" 2>&1 || pip3 install capstone >> "$OUT/01-capstone.txt" 2>&1
KC=/System/Library/KernelCollections
A="$KC/BootKernelExtensions.kc $KC/SystemKernelExtensions.kc"
I=com.apple.driver.AppleIntelICLGraphics
F=com.apple.iokit.IOAcceleratorFamily2
python3 kcdump.py "$OUT" $A -- $I $F > "$OUT/03-kcdump-stdout.txt" 2>&1
for f in "$OUT"/12-*-vtables.txt "$OUT"/10-*-symbols.txt; do [ -f "$f" ] && c++filt < "$f" > "$f.tmp" && mv "$f.tmp" "$f"; done
rm -f "$OUT"/11-*-segments.txt "$OUT"/13-*-strings.txt
VT="--vt IOAccelVideoContext2 --vt IOAccelContext2 --vt IGAccelVideoContext --vt IGAccelVideoContextMedia --vt IntelAccelerator --vt IOGraphicsAccelerator2 --vt IOAccelResource2 --vt IOAccelShared2 --vt IOAccelSubmitter2"
# 1. the family's video context
python3 kcdis.py "$OUT/x0-family-videocontext.txt" "$OUT" $A -- "$F:^IOAccelVideoContext2::" $VT > "$OUT/x0-log.txt" 2>&1
python3 kcdis.py "$OUT/x1-family-context-stream.txt" "$OUT" $A -- \
  "$F:^IOAccelContext2::(processSidebandBuffer|processDataBuffers|submit_data_buffers|s_submit_data_buffers|submitDataBuffers|beginCommandStream|endCommandStream|getDataBufferLimits|externalMethod|getTargetAndMethodForIndex|requestDataBuffer|s_get_data_buffer|getDataBuffer|addDataBufferToChannel|finish_fence_event|s_finish_fence_event|get_fence|s_set_)" \
  "$F:^IOGraphicsAccelerator2::(newVideoContext|newUserClient)" $VT > "$OUT/x1-log.txt" 2>&1
grep -a "^vtable for IOAccelVideoContext2 " -A380 "$OUT"/12-$F-vtables.txt > "$OUT/x2-family-videocontext-vtable.txt" 2>&1
grep -a "^vtable for IGAccelVideoContextMedia " -A380 "$OUT"/12-$I-vtables.txt > "$OUT/x3-icl-media-vtable.txt" 2>&1
# 2. the ICL kext: tables decoded, token handlers
python3 kcdis.py "$OUT/x4-icl-tables.txt" "$OUT" $A -- "$I:(method_descs|token_process_methods|token_discard_methods)$" $VT > "$OUT/x4-log.txt" 2>&1
python3 kcdis.py "$OUT/x5-icl-tokens.txt" "$OUT" $A -- \
  "$I:^IGAccelVideoContext::(contextStart|getTargetAndMethodForIndex|beginCommandStream|endCommandStream|processSidebandToken|process_token_|patch_codechal|bind_resource|set_current_resource|getDataBufferLimits|set_context_priority|update_perf_capability|get_iosurface_accel_id)" \
  "$I:^IGAccelVideoContextMedia::(contextStart|getTargetAndMethodForIndex|processDataBuffers|beginCommandStream|endCommandStream|processSidebandToken|process_token_AVCDecode|process_token_CODECDecode|patch_codechal_command_buffer|patch_AVC_WA|endScalableCommandStreamFE_BE)" \
  "$I:^IntelAccelerator::newVideoContext" "$I:^IGHardwareRingBuffer::submitBatchBuffer" $VT > "$OUT/x5-log.txt" 2>&1
grep -a -E "_CODECHAL_TOKEN|sIntelVideo|IOAccelCommandStreamInfo" "$OUT"/10-$I-symbols.txt | sort -u > "$OUT/x6-types.txt" 2>&1
grep -a "IOAccelVideoContext2" "$OUT"/10-$F-symbols.txt | sort -u > "$OUT/x7-family-videocontext-symbols.txt" 2>&1
rm -f "$OUT"/10-$F-symbols.txt
# 3. user space: IOAccelerator.framework's video context functions
xcrun clang -fno-objc-arc -O1 -framework Foundation -framework Metal -framework IOKit udis_server.m -o udis_server > "$OUT/x9-build.txt" 2>&1
IOA=/System/Library/PrivateFrameworks/IOAccelerator.framework/IOAccelerator
python3 udis.py "$OUT/x8-ioaccel-videocontext.txt" "load:$IOA" 'y:IOAccelerator:IOAccelVideoContext' \
  'y:IOAccelerator:^_?IOAccelContextCreate$' --follow 'IOAccel|IOConnect' > "$OUT/x8-log.txt" 2>&1
ls -la "$OUT"
exit 0

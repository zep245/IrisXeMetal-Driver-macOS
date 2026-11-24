#!/bin/bash
# Round 36 (v0.67.1): hardware video decoding, part 4 - what our video context must do, exactly.
# Round 35: IntelAccelerator::newVideoContext() returns 0 on Ice Lake; the video contexts (IGAccelVideoContextMain /
# -Media / -VEBox) are made by IntelAccelerator::newContext(type) (slot 333). Their token tables: 0x80xx..0x82xx =
# SetFence / BatchBufferStart / CodecHAL (IGAccelVideoContext), the codec tokens in IGAccelVideoContextMedia's table.
# This round reads, as text:
#   1. IntelAccelerator::newContext(unsigned int): which type makes which context (and the IGAccelVideoContext*
#      constructors / init / start / free / attach / getTargetAndMethodForIndex / externalMethod)
#   2. every remaining IGAccelVideoContext(-Media) function: patch_codechal_command_buffer__set_base_addresses,
#      bind_resource, set_current_resource, process_token_BatchBufferStart / SetFence / AVCDecode / CODECDecode,
#      beginCommandStream / endCommandStream, processDataBuffers, add/removeDataBufferFromChannel, getChannel,
#      populateContextConfig, map_memory, prepare_to_decode, endScalableCommandStreamFE_BE, IGHardwareRingBuffer submit
#   3. the family's IOAccelContext2 command-stream machinery those call (processSidebandBuffer / processSidebandToken
#      default / submit_data_buffers / data buffer pool / IOAccelCommandStreamInfo use / fences)
#   4. the full vtables of IGAccelVideoContext and IGAccelVideoContextMedia (which slots Apple overrides)
# Text output only. Round 35 is kept as run35.sh.
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
VT="--vt IGAccelVideoContext --vt IGAccelVideoContextMedia --vt IOAccelVideoContext2 --vt IOAccelContext2 --vt IntelAccelerator --vt IOGraphicsAccelerator2 --vt IOAccelResource2 --vt IGAccelResource --vt IOAccelShared2 --vt IGHardwareRingBuffer --vt IGHardwareContext --vt IOAccelMemory"
python3 kcdis.py "$OUT/y0-newcontext.txt" "$OUT" $A -- \
  "$I:^IntelAccelerator::(newContext|newVideoContext|newGLContext|new2DContext|newCLContext)" \
  "$I:^IGAccelVideoContext(Media|Main|VEBox)?::(IGAccelVideoContext|init|start|stop|free|attach|detach|getTargetAndMethodForIndex|externalMethod|clientClose|contextStart|contextStop|getMetaClass)" $VT > "$OUT/y0-log.txt" 2>&1
python3 kcdis.py "$OUT/y1-videocontext-all.txt" "$OUT" $A -- \
  "$I:^IGAccelVideoContext::" "$I:^IGAccelVideoContextMedia::(process_token_AVCDecode|process_token_CODECDecode|process_token_JPEGDecode|patch_codechal|patch_AVC_WA|beginCommandStream|endCommandStream|endScalableCommandStreamFE_BE|processDataBuffers|processSidebandToken|releaseScalableCommandBufferPointers)" \
  "$I:^IGHardwareRingBuffer::(submitBatchBuffer|submitCommands|submitStamp|submitToRing)" "$I:^IGHardwareRingBufferMedia::init" "$I:^IGAccelResource::(getGPUVirtualAddress|gpuAddress|getGPUAddress|map|getMemory)" $VT > "$OUT/y1-log.txt" 2>&1
python3 kcdis.py "$OUT/y2-family-stream.txt" "$OUT" $A -- \
  "$F:^IOAccelContext2::(processSidebandBuffer|processSidebandToken|discardSidebandToken|submit_data_buffers|s_submit_data_buffers|processDataBuffers|beginCommandStream|endCommandStream|initSidebandBufferHeader|getDataBuffer|getDataBufferPrivate|validateDataBuffer|retireDataBuffers|retireCommandBuffer|contextStart|contextStop|init|start|free|attach|getChannel|postTokenSanityCheck|getFrameDelimiter|invalidate|setFence|s_finish_fence_event|finish_fence_event|addDataBufferToChannel|removeDataBufferFromChannel|populateContextConfig|getDataBufferLimits|prepareResources|getOwningTask|getGPUTask)" \
  "$F:^IOAccelVideoContext2::" $VT > "$OUT/y2-log.txt" 2>&1
for c in IGAccelVideoContext IGAccelVideoContextMedia IntelAccelerator; do
    grep -a "^vtable for $c " -A400 "$OUT"/12-$I-vtables.txt | sed '/^vtable for .*MetaClass/,$d' > "$OUT/y3-vtable-$c.txt" 2>&1
done
grep -a "^vtable for IOAccelContext2 " -A380 "$OUT"/12-$F-vtables.txt | sed '/^vtable for .*MetaClass/,$d' > "$OUT/y3-vtable-IOAccelContext2.txt" 2>&1
grep -a -E "IOAccelCommandStreamInfo|IOAccelSidebandBuffer|sIntelVideo|_CODECHAL" "$OUT"/10-*-symbols.txt | sort -u > "$OUT/y4-types.txt" 2>&1
rm -f "$OUT"/10-$F-symbols.txt
ls -la "$OUT"
exit 0

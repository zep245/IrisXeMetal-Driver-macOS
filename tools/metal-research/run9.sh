#!/bin/bash
# Round 9 (milestone 2c): segments - how an encoder's kernel commands end up in a
# segment of a Metal command buffer (MTLIOAccelCommandBufferStorage*), with
# selector names now resolved and tail calls followed (2 levels).
set -x
OUT="$PWD/out"; mkdir -p "$OUT"
pip3 install --break-system-packages capstone > "$OUT/01-capstone.txt" 2>&1 || pip3 install capstone >> "$OUT/01-capstone.txt" 2>&1
xcrun clang -fno-objc-arc -O1 -framework Foundation -framework Metal -framework IOKit udis_server.m -o udis_server > "$OUT/40-build.txt" 2>&1
ICL=/System/Library/Extensions/AppleIntelICLGraphicsMTLDriver.bundle/Contents/MacOS/AppleIntelICLGraphicsMTLDriver
python3 udis.py "$OUT/60-user-segments.txt" \
  'm:MTLIOAccelCommandBuffer:beginSegment:' \
  'm:MTLIOAccelCommandBuffer:endCurrentSegment' \
  'm:MTLIOAccelCommandBuffer:_reserveKernelCommandBufferSpace:' \
  'm:MTLIOAccelCommandBuffer:commit' \
  'm:MTLIOAccelCommandBuffer:initWithQueue:retainedReferences:synchronousDebugMode:' \
  'm:MTLIOAccelCommandBuffer:setCurrentSegmentListPointer:' \
  'm:MTLIOAccelCommandBuffer:getSegmentListHeader' \
  'm:MTLIOAccelBlitCommandEncoder:fillBuffer:range:pattern4:' \
  --follow 'Segment|StorageGrow|StorageCreate|StorageReset|ResourceList' --depth 2 > "$OUT/60-log.txt" 2>&1
python3 udis.py "$OUT/61-user-icl.txt" "load:$ICL" \
  'm:MTLIGAccelCommandBuffer:blitCommandEncoder' \
  'm:MTLIGAccelBlitCommandEncoder:initWithCommandBuffer:stateDescriptor:' \
  'm:MTLIGAccelBlitCommandEncoder:endEncoding' \
  'm:MTLIGAccelBlitCommandEncoder:fillBuffer:range:value:' \
  --follow 'Segment|IGAccelBlitCommandEncoder|beginSegment|KernelCommand|reserve|IOAccel' --depth 2 > "$OUT/61-log.txt" 2>&1
ls -la "$OUT"
exit 0

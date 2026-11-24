#!/bin/bash
# Round 8 (milestone 2c, first GPU work from Metal): how a command encoder puts
# a driver ("kernel") command into a Metal command buffer, and how resources
# are referenced. Apple's generic MTLIOAccel* code plus how Apple's ICL Metal
# driver uses that API for fillBuffer (API usage only).
set -x
OUT="$PWD/out"; mkdir -p "$OUT"
pip3 install --break-system-packages capstone > "$OUT/01-capstone.txt" 2>&1 || pip3 install capstone >> "$OUT/01-capstone.txt" 2>&1
xcrun clang -fno-objc-arc -O1 -framework Foundation -framework Metal -framework IOKit udis_server.m -o udis_server > "$OUT/40-build.txt" 2>&1
ICL=/System/Library/Extensions/AppleIntelICLGraphicsMTLDriver.bundle/Contents/MacOS/AppleIntelICLGraphicsMTLDriver
python3 udis.py "$OUT/50-user-encoder.txt" \
  'm:MTLIOAccelCommandEncoder:initWithCommandBuffer:' \
  'm:MTLIOAccelCommandEncoder:endEncoding' \
  'm:MTLIOAccelBlitCommandEncoder:fillBuffer:range:pattern4:' \
  'm:MTLIOAccelCommandBuffer:_reserveKernelCommandBufferSpace:' \
  'm:MTLIOAccelCommandBuffer:beginSegment:' \
  'm:MTLIOAccelCommandBuffer:endCurrentSegment' \
  'm:MTLIOAccelCommandBuffer:setCurrentCommandEncoder:' \
  'm:MTLIOAccelCommandBuffer:commitEncoder' \
  'm:MTLIOAccelCommandBuffer:getCurrentKernelCommandBufferPointer:end:' \
  'm:MTLIOAccelCommandBuffer:getCurrentKernelCommandBufferStart:current:end:' \
  'm:MTLIOAccelCommandBuffer:setCurrentKernelCommandBufferPointer:' \
  'm:MTLIOAccelCommandBuffer:growKernelCommandBuffer:' \
  'm:MTLIOAccelCommandBuffer:getSegmentListPointerStart:current:end:' \
  'm:MTLIOAccelCommandBuffer:ioAccelResourceList' \
  'm:MTLIOAccelCommandBuffer:akResourceList' \
  'm:MTLIOAccelCommandBuffer:commit' \
  's:IOAccelResourceListAddResource' 's:IOAccelResourceListInit' \
  'ivars:MTLIOAccelCommandBuffer' 'ivars:MTLIOAccelCommandEncoder' 'ivars:MTLIOAccelResource' \
  --follow 'MTLIOAccel|IOAccel|Segment|KernelCommand' > "$OUT/50-log.txt" 2>&1
python3 udis.py "$OUT/51-user-icl-blit.txt" "load:$ICL" \
  'm:MTLIGAccelCommandBuffer:blitCommandEncoder' \
  'm:MTLIGAccelBlitCommandEncoder:fillBuffer:range:value:' \
  'm:MTLIGAccelBlitCommandEncoder:endEncoding' \
  'm:MTLIGAccelCommandBuffer:commit' \
  'ivars:MTLIGAccelBlitCommandEncoder' \
  --follow 'MTLIOAccel|IOAccel|Segment|KernelCommand|Resource' > "$OUT/51-log.txt" 2>&1
ls -la "$OUT"
exit 0

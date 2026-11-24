#!/bin/bash
# Round 7 (milestone 2b, command queue + command buffers):
#  user space : AppleParavirt command queue / command buffer creation and commit
#               on top of MTLIOAccelCommandQueue / MTLIOAccelCommandBuffer
#  kernel     : IOAccelCommandQueue (receives command buffers), IOAccelFIFOChannel2
#               (ring buffer to the GPU), and Apple's paravirt versions of both
set -x
OUT="$PWD/out"; mkdir -p "$OUT"
pip3 install --break-system-packages capstone > "$OUT/01-capstone.txt" 2>&1 || pip3 install capstone >> "$OUT/01-capstone.txt" 2>&1
KC=/System/Library/KernelCollections
A="$KC/BootKernelExtensions.kc $KC/SystemKernelExtensions.kc"

python3 kcdump.py "$OUT" $A -- com.apple.iokit.IOAcceleratorFamily2 com.apple.driver.AppleParavirtGPU > "$OUT/03-kcdump-stdout.txt" 2>&1
for f in "$OUT"/12-*-vtables.txt; do c++filt < "$f" > "$f.tmp" && mv "$f.tmp" "$f"; done
rm -f "$OUT"/13-*-strings.txt "$OUT"/11-*-segments.txt
VT="--vt IOGraphicsAccelerator2 --vt AppleParavirtAccelerator --vt IOAccelCommandQueue --vt AppleParavirtCommandQueue --vt IOAccelFIFOChannel2 --vt IOAccelChannel2 --vt AppleParavirtChannel --vt AppleParavirtRootChannel --vt AppleParavirtVirtualChannel --vt IOAccelEventMachine2 --vt IOAccelEventMachineFast2 --vt AppleParavirtEventMachine --vt IOAccelSegmentResourceList --vt IOAccelTask --vt IOAccelResource2 --vt IOAccelMemoryMap"
python3 kcdis.py "$OUT/30-kernel-paravirt-queue.txt" "$OUT" $A -- \
  'com.apple.driver.AppleParavirtGPU:^AppleParavirt(CommandQueue|Channel|RootChannel|VirtualChannel|SegmentResourceList)::' \
  'com.apple.driver.AppleParavirtGPU:^AppleParavirtAccelerator::(newCommandQueue|setupChannels|getChannel|setupFIFO|setupRoot|submit|interrupt|handleInterrupt)' \
  'com.apple.driver.AppleParavirtGPU:^AppleParavirtEventMachine::' $VT > "$OUT/30-log.txt" 2>&1
python3 kcdis.py "$OUT/31-kernel-family-queue.txt" "$OUT" $A -- \
  'com.apple.iokit.IOAcceleratorFamily2:^IOAccelCommandQueue::' \
  'com.apple.iokit.IOAcceleratorFamily2:^IOAccelFIFOChannel2::' \
  'com.apple.iokit.IOAcceleratorFamily2:^IOAccelChannel2::(init|submit|setStampIndex|incrementStamp|setEventStamp)' \
  'com.apple.iokit.IOAcceleratorFamily2:^IOGraphicsAccelerator2::(newCommandQueue|createCommandQueue|newUserClient|getChannel|createChannel)' \
  'com.apple.iokit.IOAcceleratorFamily2:^IOAccelEventMachineFast2::(writeStamp|incrementStamp|writeStampCommand|getStamp|testStamp|finishStamp|stampIsIncremented|checkGPUProgress|init)' $VT > "$OUT/31-log.txt" 2>&1

xcrun clang -fno-objc-arc -O1 -framework Foundation -framework Metal -framework IOKit udis_server.m -o udis_server > "$OUT/40-build.txt" 2>&1
PV=/System/Library/Extensions/AppleParavirtGPUMetal.bundle/Contents/MacOS/AppleParavirtGPUMetal
python3 udis.py "$OUT/41-user-queue.txt" "load:$PV" \
  'm:AppleParavirtDevice:newCommandQueueWithDescriptor:' \
  'm:AppleParavirtCommandQueue:initWithDevice:descriptor:' \
  'm:AppleParavirtCommandQueue:commandBuffer' \
  'm:AppleParavirtCommandQueue:commandBufferWithUnretainedReferences' \
  'm:AppleParavirtCommandBuffer:initWithQueue:retainedReferences:' \
  'm:AppleParavirtCommandBuffer:endEncoding' \
  'm:AppleParavirtCommandBuffer:dealloc' \
  'm:MTLIOAccelCommandQueue:initWithDevice:descriptor:' \
  'm:MTLIOAccelCommandQueue:submitCommandBuffers:count:' \
  'm:MTLIOAccelCommandBuffer:initWithQueue:retainedReferences:' \
  'm:MTLIOAccelCommandBuffer:initWithQueue:retainedReferences:synchronousDebugMode:' \
  'm:MTLIOAccelCommandBuffer:commit' \
  'm:MTLIOAccelCommandBuffer:commitAndReset' \
  'm:MTLIOAccelCommandBuffer:beginSegment:' \
  'm:MTLIOAccelCommandBuffer:endCurrentSegment' \
  'm:MTLIOAccelDevice:newCommandQueueWithDescriptor:' \
  'm:MTLIOAccelDevice:newCommandQueue' \
  'm:_MTLDevice:newCommandQueue' \
  'm:_MTLDevice:newCommandQueueWithMaxCommandBufferCount:' \
  's:IOAccelCommandQueueCreate' 's:IOAccelCommandQueueCreateWithQoS' \
  's:IOAccelCommandQueueSubmitCommandBuffers' 's:IOAccelCommandQueueSubmitCommandBuffer' \
  'ivars:MTLIOAccelCommandQueue' 'ivars:MTLIOAccelCommandBuffer' 'ivars:AppleParavirtCommandQueue' \
  --follow 'IOAccel|MTLIOAccel|AppleParavirt|CommandQueue|CommandBuffer|Segment' > "$OUT/41-log.txt" 2>&1
ls -la "$OUT"
exit 0

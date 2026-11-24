#!/bin/bash
# Round 6 (milestone 2a, buffers): how a Metal buffer is created.
#  user space : AppleParavirtDevice newBuffer* -> MTLIOAccelBuffer/Resource init
#               -> IOAccelResourceCreate (IOAccelerator.framework) -> kernel call
#  kernel     : IOAccelSharedUserClient2::new_resource -> IOAccelResource2 /
#               IOAccelSysMemory / memory map, and the paravirt Resource class
set -x
OUT="$PWD/out"; mkdir -p "$OUT"
pip3 install --break-system-packages capstone > "$OUT/01-capstone.txt" 2>&1 || pip3 install capstone >> "$OUT/01-capstone.txt" 2>&1
KC=/System/Library/KernelCollections
A="$KC/BootKernelExtensions.kc $KC/SystemKernelExtensions.kc"

# ---- kernel (same tools as round 5)
python3 kcdump.py "$OUT" $A -- com.apple.iokit.IOAcceleratorFamily2 com.apple.driver.AppleParavirtGPU > "$OUT/03-kcdump-stdout.txt" 2>&1
for f in "$OUT"/12-*-vtables.txt; do c++filt < "$f" > "$f.tmp" && mv "$f.tmp" "$f"; done
rm -f "$OUT"/13-*-strings.txt "$OUT"/10-*-symbols.txt "$OUT"/11-*-segments.txt
VT="--vt IOGraphicsAccelerator2 --vt AppleParavirtAccelerator --vt IOAccelResource2 --vt AppleParavirtResource --vt IOAccelSysMemory --vt IOAccelMemory --vt IOAccelMemoryMap --vt AppleParavirtMemoryMap --vt IOAccelShared2 --vt IOAccelTask"
python3 kcdis.py "$OUT/30-kernel-paravirt-resource.txt" "$OUT" $A -- \
  'com.apple.driver.AppleParavirtGPU:^AppleParavirt(Resource|MemoryMap|SysMemory|PageTable)::' $VT > "$OUT/30-log.txt" 2>&1
python3 kcdis.py "$OUT/31-kernel-family-resource.txt" "$OUT" $A -- \
  'com.apple.iokit.IOAcceleratorFamily2:^(IOAccelSharedUserClient2::(s_new_resource|new_resource)|IOAccelResource2::(newResourceWithOptions|init|initWith|prepare|addToAperture|commit|setup|create)|IOAccelSysMemory::(init|initWith|create|wire|getPhysicalSegment)|IOAccelMemory::(init|initWith)|IOAccelMemoryMap::(init|commit|initWith)|IOGraphicsAccelerator2::(createVidMemory|createSysMemory|createMemoryMap|newResource))' $VT > "$OUT/31-log.txt" 2>&1

# ---- user space
xcrun clang -fno-objc-arc -O1 -framework Foundation -framework Metal -framework IOKit udis_server.m -o udis_server > "$OUT/40-build.txt" 2>&1
PV=/System/Library/Extensions/AppleParavirtGPUMetal.bundle/Contents/MacOS/AppleParavirtGPUMetal
python3 udis.py "$OUT/41-user-buffer.txt" "load:$PV" \
  'm:AppleParavirtDevice:newBufferWithLength:options:' \
  'm:AppleParavirtDevice:newBufferWithBytes:length:options:' \
  'm:AppleParavirtDevice:newBufferWithBytesNoCopy:length:options:deallocator:' \
  'm:AppleParavirtBuffer:initWithDevice:withLength:withOptions:withPointer:withDeallocator:' \
  'm:MTLIOAccelBuffer:initWithDevice:pointer:length:options:sysMemSize:vidMemSize:args:argsSize:deallocator:' \
  'm:MTLIOAccelBuffer:initWithDevice:pointer:length:options:sysMemSize:vidMemSize:gpuAddress:args:argsSize:deallocator:' \
  'm:MTLIOAccelResource:initWithDevice:options:args:argsSize:' \
  's:IOAccelResourceCreate' \
  'ivars:MTLIOAccelResource' 'ivars:MTLIOAccelBuffer' 'ivars:AppleParavirtBuffer' \
  --follow 'IOAccel|MTLIOAccel|AppleParavirt|Resource' > "$OUT/41-log.txt" 2>&1
python3 udis.py "$OUT/42-user-device.txt" "load:$PV" \
  'm:AppleParavirtDevice:initWithAcceleratorPort:' \
  'm:AppleParavirtDevice:setupResourcePools' 'm:AppleParavirtDevice:setupDeviceInfo' \
  'm:AppleParavirtDevice:featureProfile' \
  'm:MTLIOAccelDevice:initWithAcceleratorPort:' \
  'm:AppleParavirtDevice:newCommandQueueWithDescriptor:' \
  'm:MTLIOAccelDevice:newCommandQueueWithDescriptor:' \
  'ivars:AppleParavirtDevice' > "$OUT/42-log.txt" 2>&1
ls -la "$OUT"
exit 0

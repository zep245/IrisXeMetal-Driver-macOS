#!/bin/bash
# Round 31 (v0.56.4): in the v0.56.3 test the display pipe showed WindowServer's frames, but every transaction was
# answered 0xe00002be. The kernel log says why: IOAccelResource2::prepare() "failed to allocate GPU virtual address
# for mapping. resType=0xc0" after IOAccelTask::freeToAllocGPUAddress "failed to alloc GART space at segment 0.
# Needed 0 bytes, used 16384000, size 536866816. vram=1". This round reads how the GPU address of a resource's
# memory is sized and allocated (and where vram=1 and the 0-byte size come from), plus Apple's paravirtual GPU,
# which shows IOSurfaces through a display pipe too. Text output only. Round 30 is kept as run30.sh.
set -x
OUT="$PWD/out"; mkdir -p "$OUT"
sw_vers > "$OUT/00-versions.txt" 2>&1
pip3 install --break-system-packages capstone > "$OUT/01-capstone.txt" 2>&1 || pip3 install capstone >> "$OUT/01-capstone.txt" 2>&1
KC=/System/Library/KernelCollections
A="$KC/BootKernelExtensions.kc $KC/SystemKernelExtensions.kc"
IDS="com.apple.iokit.IOAcceleratorFamily2 com.apple.driver.AppleParavirtGPU"
python3 kcdump.py "$OUT" $A -- $IDS > "$OUT/03-kcdump-stdout.txt" 2>&1
for f in "$OUT"/12-*-vtables.txt "$OUT"/10-*-symbols.txt; do c++filt < "$f" > "$f.tmp" && mv "$f.tmp" "$f"; done
rm -f "$OUT"/11-*-segments.txt
VT="--vt IOAccelTask --vt IOAccelResource2 --vt IOAccelMemory --vt IOAccelVidMemory --vt IOAccelSysMemory --vt IOAccelMemoryMap --vt IOGraphicsAccelerator2 --vt AppleParavirtAccelerator"
python3 kcdis.py "$OUT/m0-gpuaddress.txt" "$OUT" $A -- \
  'com.apple.iokit.IOAcceleratorFamily2:^IOAccelTask::(freeToAllocGPUAddress|allocGPUAddress|alloc_gpu_address|freeGPUAddress|init|initGART|getGART|segment)' \
  'com.apple.iokit.IOAcceleratorFamily2:^IOAccelResource2::(map|prepare|getStorageResource|initialize|newResourceWithOptions|initSurfaceBuffer|initWithIOSurface|setIOSurface|getVRAM|isVRAM)' \
  'com.apple.iokit.IOAcceleratorFamily2:^IOAccelMemoryMap::(init|getGPUVirtualAddress|getLength|commit|commit_pte|allocate|prepare)' \
  'com.apple.iokit.IOAcceleratorFamily2:^IOAccelVidMemory::(init|prepare|wire|getLength|getSize|getResidentSize|complete)' \
  'com.apple.iokit.IOAcceleratorFamily2:^IOAccelMemory::(init|getLength|getSize|prepare)' \
  'com.apple.iokit.IOAcceleratorFamily2:^IOGraphicsAccelerator2::(freeToPrepareMapping|createVidMemory|createSysMemory|createMemoryMap|vramSize|getVRAM|tmpTotalVRAM|initGART|gart|setup)' \
  'com.apple.driver.AppleParavirtGPU:^AppleParavirtAccelerator::(newVidMemory|tmpTotalVRAM|totalTextureMemory|start|initGART|newSysMemory|getGPUVA|gartSize)' $VT > "$OUT/m0-log.txt" 2>&1
# the strings of the family (the log formats name the fields)
grep -a -i "GART\|vram\|GPU virtual address" "$OUT"/13-com.apple.iokit.IOAcceleratorFamily2-strings.txt > "$OUT/m1-strings.txt" 2>&1
rm -f "$OUT"/13-*-strings.txt
ls -la "$OUT"
exit 0

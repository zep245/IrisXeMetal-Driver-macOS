#!/bin/bash
# Round 5: GPU task / page table / event machine / display machine setup,
# plus C strings referenced by code (text only).
set -x
OUT="$PWD/out"; mkdir -p "$OUT"
KC=/System/Library/KernelCollections
python3 kcdump.py "$OUT" $KC/BootKernelExtensions.kc $KC/SystemKernelExtensions.kc -- \
  com.apple.iokit.IOAcceleratorFamily2 com.apple.driver.AppleParavirtGPU com.apple.iokit.IOPCIFamily > "$OUT/03-kcdump-stdout.txt" 2>&1
for f in "$OUT"/12-*-vtables.txt; do c++filt < "$f" > "$f.tmp" && mv "$f.tmp" "$f"; done
rm -f "$OUT"/13-*-strings.txt
pip3 install --break-system-packages capstone > "$OUT/01-capstone.txt" 2>&1 || pip3 install capstone >> "$OUT/01-capstone.txt" 2>&1
VT="--vt IOGraphicsAccelerator2 --vt AppleParavirtAccelerator --vt IOAccelEventMachineFast2 --vt AppleParavirtEventMachine --vt IOAccelDisplayMachine --vt AppleParavirtDisplayMachine --vt IOAccelTask --vt AppleParavirtTask --vt IOPCIDevice --vt IOAccelSysMemory --vt IOAccelMemoryMap"
A="$KC/BootKernelExtensions.kc $KC/SystemKernelExtensions.kc"
python3 kcdis.py "$OUT/20-paravirt-all.txt" "$OUT" $A -- \
  'com.apple.driver.AppleParavirtGPU:^AppleParavirt(Accelerator|EventMachine|DisplayMachine|Task|KernelTask|PageTable|SysMemory|VidMemory|MemoryMap|Shared|SharedUserClient|GPUControl)::' $VT > "$OUT/20-log.txt" 2>&1
python3 kcdis.py "$OUT/21-family-setup.txt" "$OUT" $A -- \
  'com.apple.iokit.IOAcceleratorFamily2:^(IOAccelTask::(init|reserve|allocate)|IOAccelEventMachine(2|Fast2)::(init|start|setup)|IOAccelDisplayMachine::(init|start|found_framebuffer|attach)|IOGraphicsAccelerator2::(createEventMachine|validateConfigStructure|initializeConfigStructure|calcMaxGPUPhysicalMemoryBytes|oneTimeSetup|newShared|newSharedUserClient|newCommandQueue|newDisplayPipe|newDevice|newUserClient|newStatistics))' $VT > "$OUT/21-log.txt" 2>&1
ls -la "$OUT"
exit 0

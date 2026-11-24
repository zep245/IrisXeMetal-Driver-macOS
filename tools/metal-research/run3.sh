#!/bin/bash
# Round 3: kernel side. Vendor GPU kexts subclass the C++ classes of
# IOAcceleratorFamily2. Their binaries live inside the kernel collections;
# kcdump.py reads them directly and writes TEXT only (symbols, vtables with
# resolved slots, strings). No binaries are uploaded.
set -x
OUT="$PWD/out"; mkdir -p "$OUT"
sw_vers > "$OUT/00-system.txt"
ls -la /System/Library/KernelCollections >> "$OUT/00-system.txt" 2>&1
KC=/System/Library/KernelCollections
python3 kcdump.py "$OUT" $KC/BootKernelExtensions.kc $KC/SystemKernelExtensions.kc -- \
  com.apple.iokit.IOAcceleratorFamily2 com.apple.iokit.IOGPUFamily \
  com.apple.driver.AppleParavirtGPU com.apple.driver.AppleParavirtGPUIOGPUFamily \
  com.apple.driver.AppleIntelICLGraphics com.apple.iokit.IOSurface \
  com.apple.iokit.IOGraphicsFamily com.apple.AppleGraphicsDeviceControl com.apple.kernel \
  > "$OUT/03-kcdump-stdout.txt" 2>&1
# compiler's vtable layout for IOService / IOUserClient (kernel slots)
SDK=$(xcrun --sdk macosx --show-sdk-path)
KH="$SDK/System/Library/Frameworks/Kernel.framework/Headers"
clang++ -x c++ -std=gnu++17 -S -o /dev/null -mkernel -nostdinc -fapple-kext -isystem "$KH" -I"$KH" \
  -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
  -Xclang -fdump-vtable-layouts vtlayout.cpp > "$OUT/14-kernel-vtable-layouts.txt" 2>&1
# demangle C++ names in place
for f in "$OUT"/10-*-symbols.txt "$OUT"/12-*-vtables.txt; do
  [ -f "$f" ] && c++filt < "$f" > "$f.tmp" && mv "$f.tmp" "$f"
done
ls -la "$OUT"
exit 0

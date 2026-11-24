#!/bin/bash
# Round 2: Apple's paravirtual GPU driver (the GPU of these runner VMs) is a
# small, complete Metal driver stack on Intel macOS: an IOAcceleratorFamily2
# kext + a Metal bundle. It is the model for our own Iris Xe stack.
set -x
OUT="$PWD/out"; mkdir -p "$OUT"
sw_vers > "$OUT/00-system.txt"; xcodebuild -version >> "$OUT/00-system.txt" 2>&1
system_profiler SPDisplaysDataType > "$OUT/01-displays.txt" 2>&1
SLE=/System/Library/Extensions

# 1. kext + bundle plists and files
for k in AppleParavirtGPU AppleParavirtGPUIOGPUFamily IOAcceleratorFamily2 IOGPUFamily; do
  plutil -p $SLE/$k.kext/Contents/Info.plist > "$OUT/02-$k-Info.txt" 2>&1
  ls -laR $SLE/$k.kext > "$OUT/02-$k-files.txt" 2>&1
done
for b in AppleParavirtGPUMetal AppleParavirtGPUMetalIOGPUFamily; do
  plutil -p $SLE/$b.bundle/Contents/Info.plist > "$OUT/03-$b-Info.txt" 2>&1
  ls -laR $SLE/$b.bundle > "$OUT/03-$b-files.txt" 2>&1
done

# 2. live registry: the running paravirt accelerator and its user clients
ioreg -l -w0 -r -c IOAccelerator > "$OUT/04-ioreg-accelerator.txt" 2>&1
ioreg -l -w0 -r -c IOAccelSharedUserClient2 > "$OUT/04-ioreg-shared-uc.txt" 2>&1
ioreg -w0 -t -c IOUserClient | grep -i -E "accel|paravirt|surface" > "$OUT/04-ioreg-userclients.txt" 2>&1

# 3. kernel C++ interfaces: symbols of the kext binaries (still on disk)
for k in IOAcceleratorFamily2 AppleParavirtGPU IOGPUFamily AppleParavirtGPUIOGPUFamily; do
  bin=$(ls $SLE/$k.kext/Contents/MacOS/* 2>/dev/null | head -1)
  if [ -n "$bin" ]; then
    nm -m "$bin" | c++filt > "$OUT/10-$k-symbols.txt" 2>&1
    otool -L "$bin" > "$OUT/11-$k-libs.txt" 2>&1
  else
    echo "no binary" > "$OUT/10-$k-symbols.txt"
  fi
done

# 4. Objective-C side: Metal base classes, IOAccelerator/IOGPU frameworks, paravirt bundles
clang -fobjc-arc -framework Foundation -o objc_dump objc_dump.m
./objc_dump /System/Library/Frameworks/Metal.framework/Metal Metal.framework --protocols MTL > "$OUT/20-Metal-objc.txt" 2>&1
./objc_dump /System/Library/PrivateFrameworks/IOAccelerator.framework/IOAccelerator IOAccelerator.framework > "$OUT/21-IOAccelerator-objc.txt" 2>&1
./objc_dump /System/Library/PrivateFrameworks/IOGPU.framework/IOGPU IOGPU.framework > "$OUT/22-IOGPU-objc.txt" 2>&1
for b in AppleParavirtGPUMetal AppleParavirtGPUMetalIOGPUFamily; do
  exe=$(plutil -extract CFBundleExecutable raw $SLE/$b.bundle/Contents/Info.plist 2>/dev/null)
  ./objc_dump $SLE/$b.bundle/Contents/MacOS/$exe $b.bundle > "$OUT/23-$b-objc.txt" 2>&1
  dyld_info -exports $SLE/$b.bundle/Contents/MacOS/$exe > "$OUT/24-$b-exports.txt" 2>&1
  dyld_info -imports $SLE/$b.bundle/Contents/MacOS/$exe > "$OUT/25-$b-imports.txt" 2>&1
  dyld_info -linked_dylibs $SLE/$b.bundle/Contents/MacOS/$exe > "$OUT/26-$b-linked.txt" 2>&1
done
for p in /System/Library/PrivateFrameworks/IOAccelerator.framework/IOAccelerator \
         /System/Library/PrivateFrameworks/IOGPU.framework/IOGPU; do
  n=$(basename $p)
  dyld_info -exports $p > "$OUT/27-$n-exports.txt" 2>&1
done
ls -la "$OUT"
exit 0

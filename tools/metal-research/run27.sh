#!/bin/bash
# Round 27 (v0.55.1): before writing our own display pipe (IrisXeDisplayPipe: take each transaction's screen image and
# put it on the panel), list what a kext can call: the kernel IOSurface class (memory descriptor, size, row bytes,
# pixel format), IOAccelDisplayPipe / IOAccelDisplayPipeTransaction2 / IOAccelResource2 exports, and Apple's Intel
# IGAccelDisplayPipe::performTransaction + generateFlip (how Apple turns a transaction into a flip).
# Text output only. Round 26 is kept as run26.sh.
set -x
OUT="$PWD/out"; mkdir -p "$OUT"
sw_vers > "$OUT/00-versions.txt" 2>&1
pip3 install --break-system-packages capstone > "$OUT/01-capstone.txt" 2>&1 || pip3 install capstone >> "$OUT/01-capstone.txt" 2>&1
KC=/System/Library/KernelCollections
A="$KC/BootKernelExtensions.kc $KC/SystemKernelExtensions.kc"
IDS="com.apple.iokit.IOSurface com.apple.iokit.IOAcceleratorFamily2 com.apple.driver.AppleIntelICLGraphics"
python3 kcdump.py "$OUT" $A -- $IDS > "$OUT/03-kcdump-stdout.txt" 2>&1
for f in "$OUT"/12-*-vtables.txt "$OUT"/10-*-symbols.txt; do c++filt < "$f" > "$f.tmp" && mv "$f.tmp" "$f"; done
rm -f "$OUT"/11-*-segments.txt "$OUT"/13-*-strings.txt
grep -E "IOSurface::" "$OUT/10-com.apple.iokit.IOSurface-symbols.txt" > "$OUT/i0-iosurface-methods.txt" 2>&1
VT="--vt IOSurface --vt IOAccelDisplayPipe --vt IGAccelDisplayPipe --vt IOAccelDisplayPipeTransaction2 --vt IOAccelResource2 --vt IGAccelResource"
python3 kcdis.py "$OUT/i1-intel-perform.txt" "$OUT" $A -- \
  'com.apple.driver.AppleIntelICLGraphics:^IGAccelDisplayPipe::(performTransaction|generateFlip|beginTransaction|signalTransactionComplete|getScanoutOffset|submitCommands)\(' \
  'com.apple.iokit.IOSurface:^IOSurface::(getMemoryDescriptor|getWidth|getHeight|getBytesPerRow|getPixelFormat|getAllocSize|getPlane|getBaseAddress|prepare|complete)' \
  'com.apple.iokit.IOAcceleratorFamily2:^IOAccelResource2::(getIOSurface|getMemory|getVidMemory|getSysMemory|getResourceSize|getBytesPerRow|getWidth|getHeight)' $VT > "$OUT/i1-log.txt" 2>&1
ls -la "$OUT"
exit 0

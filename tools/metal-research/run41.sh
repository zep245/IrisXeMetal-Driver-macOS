#!/bin/bash
# Round 40 results (v0.68.2): Apple's VA driver initialises fine on our accelerator - it opens its 3 video contexts,
# gets the hw caps (its device-id check accepts 0x8A528086), the accelerator id, then builds its engine/codec tables
# (AVD_CreateAVDAccelerator returns success). Yet VideoToolbox says VTIsHardwareDecodeSupported(H.264) = 0 without
# loading it again: the decision is made ABOVE the VA driver, in VideoToolbox / AppleGVA.
# Round 41 (hardware video decoding, part 9): HOW VideoToolbox and AppleGVA decide "hardware decode supported" -
# VTIsHardwareDecodeSupported followed 3 levels, AppleGVA's functions that read board-id / model / IOGVA* properties
# / its preferences, and AppleGVA's own strings and plists. Round 40 is kept as run40.sh.
set -x
OUT="$PWD/out"; mkdir -p "$OUT"
sw_vers > "$OUT/00-versions.txt" 2>&1
python3 -c "import capstone" 2>/dev/null || pip3 install --user capstone > "$OUT/01-capstone.txt" 2>&1 || pip3 install --break-system-packages capstone >> "$OUT/01-capstone.txt" 2>&1
xcrun clang -fno-objc-arc -O1 -framework Foundation -framework Metal -framework IOKit udis_server.m -o udis_server > "$OUT/40-build.txt" 2>&1
G=/System/Library/PrivateFrameworks/AppleGVA.framework
ls -laR "$G" > "$OUT/41-gva-files.txt" 2>&1
ls -la /System/Library/PrivateFrameworks/AppleGVACore.framework /System/Library/Extensions | grep -iE "gva|VADriver|AppleGVA|total" >> "$OUT/41-gva-files.txt" 2>&1
for p in $(find "$G" /System/Library/PrivateFrameworks/AppleGVACore.framework -name "*.plist" 2>/dev/null); do echo "=== $p"; plutil -convert xml1 -o - "$p" 2>&1 | head -400; done > "$OUT/42-gva-plists.txt" 2>&1
python3 udis.py "$OUT/50-vt-hwdec.txt" \
  'load:/System/Library/Frameworks/VideoToolbox.framework/VideoToolbox' \
  'load:/System/Library/PrivateFrameworks/AppleGVA.framework/AppleGVA' \
  's:VTIsHardwareDecodeSupported' \
  --follow '.' --depth 3 > "$OUT/50-log.txt" 2>&1
python3 udis.py "$OUT/51-gva-checks.txt" \
  'load:/System/Library/PrivateFrameworks/AppleGVA.framework/AppleGVA' \
  'x:AppleGVA:board-id' \
  'x:AppleGVA:hwdec' \
  'x:AppleGVA:IOGVA' \
  'x:AppleGVA:IODVDBundleName' \
  'x:AppleGVA:IOVARendererID' \
  'x:AppleGVA:forceOffline' \
  'x:AppleGVA:Mac-' \
  'x:AppleGVA:model' \
  'y:AppleGVA:(?i)(support|capab|hwdec|hardware|boardid|board_id|platform|isavailable|decoderavailable)' \
  --follow '(?i)(support|capab|board|platform|gpu|accel|renderer|device|property|hardware)' --depth 2 > "$OUT/51-log.txt" 2>&1
ls -la "$OUT"
exit 0

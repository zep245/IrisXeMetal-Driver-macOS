#!/bin/bash
# Round 41 results (v0.68.2): VTIsHardwareDecodeSupported just tries to create a decoder with
# RequireHardwareAcceleratedVideoDecoder; our test got -12906 (kVTCouldNotFindVideoDecoderErr) with no AppleGVA log
# line at all: VideoToolbox's decoder registry filtered the hardware decoder out BEFORE loading AppleGVA. The board
# id of MacBookPro16,2 (Mac-5F9802EFE386AA28) is in AppleGVA's list (pp 3), so the Mac model is fine. AppleGVA finds
# the GPU by walking the IOService plane for a node named IGPU / GFX0 and searching a property below it.
# Round 42 (hardware video decoding, part 10): the decoder registry - every Video plug-in / codec bundle and its
# Info.plist (the matching rules), VideoToolbox's own built-in decoder list and the IORegistry keys it tests.
set -x
OUT="$PWD/out"; mkdir -p "$OUT"
sw_vers > "$OUT/00-versions.txt" 2>&1
python3 -c "import capstone" 2>/dev/null || pip3 install --user capstone > "$OUT/01-capstone.txt" 2>&1 || pip3 install --break-system-packages capstone >> "$OUT/01-capstone.txt" 2>&1
xcrun clang -fno-objc-arc -O1 -framework Foundation -framework Metal -framework IOKit udis_server.m -o udis_server > "$OUT/40-build.txt" 2>&1
for d in /System/Library/Video/Plug-Ins /Library/Video/Plug-Ins /System/Library/Components /System/Library/VideoDecoders /System/Library/PrivateFrameworks/AppleGVA.framework /System/Library/Frameworks/VideoToolbox.framework; do
  echo "=== $d"; ls -la "$d" 2>&1
done > "$OUT/41-plugin-dirs.txt" 2>&1
find /System/Library/Video /System/Library/Frameworks/VideoToolbox.framework /System/Library/PrivateFrameworks/AppleGVA.framework /System/Library/PrivateFrameworks/AppleGVACore.framework -name "Info.plist" 2>/dev/null | while read -r p; do
  echo "=================== $p"; plutil -convert xml1 -o - "$p" 2>&1 | grep -v "<string>[0-9A-F]\{40\}</string>" | head -600
done > "$OUT/42-plugin-plists.txt" 2>&1
grep -rl "VideoDecoder\|videodecoder" /System/Library/Video 2>/dev/null | head -50 > "$OUT/43-decoder-files.txt"
python3 udis.py "$OUT/50-vt-registry.txt" \
  'load:/System/Library/Frameworks/VideoToolbox.framework/VideoToolbox' \
  'x:VideoToolbox:IOGVA' \
  'x:VideoToolbox:IORegistry' \
  'x:VideoToolbox:IOAVD' \
  'x:VideoToolbox:IOVARenderer' \
  'x:VideoToolbox:HardwareAcceleratedVideoDecoder' \
  'x:VideoToolbox:avc1' \
  'k:VideoToolbox:IORegistryEntrySearchCFProperty' \
  'k:VideoToolbox:IORegistryEntryCreateCFProperty' \
  'k:VideoToolbox:IOServiceGetMatchingServices' \
  > "$OUT/50-log.txt" 2>&1
ls -la "$OUT"
exit 0

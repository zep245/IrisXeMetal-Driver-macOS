#!/bin/bash
# Round 42 results: hardware decoders are plug-ins with CMDependencies / IORegistryRequiredKey (GVA VP9: IOGVAXDecode,
# GVA HEVC: IOGVAHEVCDecode). The v0.68.3 test boot: VTCopyVideoDecoderList shows NO GVA decoder at all - not even
# the VP9 one, although our accelerator has IOGVAXDecode = "2" - so VideoToolbox's IORegistryRequiredKey check does
# not see our accelerator. Round 43 (hardware video decoding, part 11): that check - VideoToolbox's
# VTDoesAnyDeviceSupportIORegistryKey and every function around the decoder registry / CMDependencies, with the
# CFStrings they use decoded (which services it looks at: class, plane, name, GPU registry id).
set -x
OUT="$PWD/out"; mkdir -p "$OUT"
sw_vers > "$OUT/00-versions.txt" 2>&1
python3 -c "import capstone" 2>/dev/null || pip3 install --user capstone > "$OUT/01-capstone.txt" 2>&1 || pip3 install --break-system-packages capstone >> "$OUT/01-capstone.txt" 2>&1
xcrun clang -fno-objc-arc -O1 -framework Foundation -framework Metal -framework IOKit udis_server.m -o udis_server > "$OUT/40-build.txt" 2>&1
VT=/System/Library/Frameworks/VideoToolbox.framework/VideoToolbox
python3 udis.py "$OUT/50-vt-iokey.txt" "load:$VT" \
  'y:VideoToolbox:(DoesAnyDevice|IORegistry|Dependenc|GPURegistry|DeviceSupport|CopyGPU|GPUDevice|IOKitDevice|AcceleratorDevice)' \
  --follow '(?i)(IOService|IORegistry|IOIterator|IOObject|Matching|Property|Device|GPU|Accel|stub)' --depth 2 > "$OUT/50-log.txt" 2>&1
python3 udis.py "$OUT/51-vt-cfs.txt" "load:$VT" \
  'cfs:VideoToolbox:VTDoesAnyDeviceSupportIORegistryKey' \
  > "$OUT/51-log.txt" 2>&1
# the FigRegistry side (CoreMedia / MediaToolbox): how CMDependencies are checked
python3 udis.py "$OUT/52-fig-deps.txt" \
  'load:/System/Library/Frameworks/CoreMedia.framework/CoreMedia' \
  'load:/System/Library/PrivateFrameworks/MediaToolbox.framework/MediaToolbox' \
  'y:CoreMedia:(?i)(dependenc|ioregistry|requiredkey)' \
  'y:MediaToolbox:(?i)(dependenc|ioregistry|requiredkey)' \
  --follow '(?i)(IOService|IORegistry|IOIterator|Matching|Property|dependenc)' --depth 2 > "$OUT/52-log.txt" 2>&1
ls -la "$OUT"
exit 0

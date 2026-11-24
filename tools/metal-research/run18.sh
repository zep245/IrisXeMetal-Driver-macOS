#!/bin/bash
# Round 18 (v0.52.2): round 17 + the WindowServer crash report showed:
#  - IOPresentment asks AGDC 0x921 from IOPresentmentCapabilities::__GatherAGDCLogicalDeviceCapabilities (0xB0 bytes in/out);
#    Apple's Intel driver answers 0x921 with AppleIntelFramebufferController::GetLinkConfig, 0x941 with GetPortCapability,
#    0x2001 through processCmd.
#  - WindowServer then aborts on an assert in CoreDisplay_CreateDisplayForCGXDisplayDevice (+3349).
# This round reads those Intel functions, the CoreDisplay function with its assert, and IOPresentmentCapabilities.
# Text output only. Round 17 is kept as run17.sh.
set -x
OUT="$PWD/out"; mkdir -p "$OUT"
sw_vers > "$OUT/00-versions.txt" 2>&1
pip3 install --break-system-packages capstone > "$OUT/01-capstone.txt" 2>&1 || pip3 install capstone >> "$OUT/01-capstone.txt" 2>&1
KC=/System/Library/KernelCollections
A="$KC/BootKernelExtensions.kc $KC/SystemKernelExtensions.kc"
IDS="com.apple.AppleGraphicsDeviceControl com.apple.driver.AppleIntelICLLPGraphicsFramebuffer"
python3 kcdump.py "$OUT" $A -- $IDS > "$OUT/03-kcdump-stdout.txt" 2>&1
for f in "$OUT"/12-*-vtables.txt "$OUT"/10-*-symbols.txt; do c++filt < "$f" > "$f.tmp" && mv "$f.tmp" "$f"; done
rm -f "$OUT"/11-*-segments.txt "$OUT"/13-*-strings.txt
VT="--vt AppleGraphicsDeviceControl --vt IntelFBClientControl --vt AppleIntelFramebufferController --vt AppleIntelFramebuffer --vt IOFramebuffer"
python3 kcdis.py "$OUT/80-intel-link.txt" "$OUT" $A -- \
  'com.apple.driver.AppleIntelICLLPGraphicsFramebuffer:^AppleIntelFramebufferController::(GetLinkConfig|GetPortCapability|processCmd|GetMultiLinkConfig|getFramebufferByPort|GetStreamConfig)\(' \
  'com.apple.driver.AppleIntelICLLPGraphicsFramebuffer:^AppleIntelFramebuffer::(getLinkConfig|GetLinkConfig|fillLinkConfig|getPortCapability)' $VT > "$OUT/80-log.txt" 2>&1
xcrun clang -fno-objc-arc -O1 -framework Foundation -framework Metal -framework IOKit udis_server.m -o udis_server > "$OUT/40-build.txt" 2>&1
CD=/System/Library/Frameworks/CoreDisplay.framework/CoreDisplay
IOP=/System/Library/PrivateFrameworks/IOPresentment.framework/IOPresentment
python3 udis.py "$OUT/90-coredisplay-assert.txt" "load:$CD" "load:$IOP" \
  'y:CoreDisplay:CoreDisplay_CreateDisplayForCGXDisplayDevice$' \
  'x:CoreDisplay:CoreDisplay_CreateDisplayForCGXDisplayDevice' > "$OUT/90-log.txt" 2>&1
python3 udis.py "$OUT/91-iop-capabilities.txt" "load:$CD" "load:$IOP" \
  'y:IOPresentment:IOPresentmentCapabilities' \
  --follow 'IOPresentmentCapabilities' --depth 1 > "$OUT/91-log.txt" 2>&1
ls -la "$OUT"
exit 0

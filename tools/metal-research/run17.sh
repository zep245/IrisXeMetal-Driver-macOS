#!/bin/bash
# Round 17 (v0.52.0 results): with IrisXeAGDC + a display pipe with transactions, WindowServer gets past
# "Valid ADGC node found", asks AGDC attribute 0x921 (we answer unsupported) and then crashes in IOPresentment
# every 10 s. Without transactions it stops at "Interface creation error: 0x2005". This round reads:
#  A. Apple's Intel AGDC answers for 0x921 (display pipe capability = AppleIntelFramebuffer's transaction caps,
#     0x1D8 bytes at +0x83F8), 0x2001, 0x941, 0x3: IntelFBClientControl::doAttribute with its jump tables,
#     AppleIntelFramebuffer::initTransactionCaps / setTransactionCaps.
#  B. The AGDC family filter for those attributes (filtered_doDeviceAttribute jump tables) and the user client check.
#  C. IOPresentment: every function using 0x921, 0x2001, 0x941, 0x2002, 0x2005 and the "kAGDC..." / pipe /
#     interface error strings, so we know which fields of the 0x921 answer it reads.
# Text output only. Round 16 is kept as run16.sh.
set -x
OUT="$PWD/out"; mkdir -p "$OUT"
sw_vers > "$OUT/00-versions.txt" 2>&1
pip3 install --break-system-packages capstone > "$OUT/01-capstone.txt" 2>&1 || pip3 install capstone >> "$OUT/01-capstone.txt" 2>&1
KC=/System/Library/KernelCollections
A="$KC/BootKernelExtensions.kc $KC/SystemKernelExtensions.kc"
IDS="com.apple.AppleGraphicsDeviceControl com.apple.driver.AppleIntelICLLPGraphicsFramebuffer com.apple.iokit.IOAcceleratorFamily2"
python3 kcdump.py "$OUT" $A -- $IDS > "$OUT/03-kcdump-stdout.txt" 2>&1
for f in "$OUT"/12-*-vtables.txt "$OUT"/10-*-symbols.txt; do c++filt < "$f" > "$f.tmp" && mv "$f.tmp" "$f"; done
rm -f "$OUT"/11-*-segments.txt "$OUT"/13-*-strings.txt
VT="--vt AppleGraphicsDeviceControl --vt IntelFBClientControl --vt AppleIntelFramebufferController --vt AppleIntelFramebuffer --vt IOAccelDisplayPipe --vt IOFramebuffer"
python3 kcdis.py "$OUT/60-intel-attr.txt" "$OUT" $A -- \
  'com.apple.driver.AppleIntelICLLPGraphicsFramebuffer:^IntelFBClientControl::(doAttribute|GetAttributeType)\(' \
  'com.apple.driver.AppleIntelICLLPGraphicsFramebuffer:^AppleIntelFramebuffer::(initTransactionCaps|setTransactionCaps|canSupportFrameScaler|getDisplayPipeCapability|GetDisplayCapability)' \
  'com.apple.driver.AppleIntelICLLPGraphicsFramebuffer:^AppleIntelFramebufferController::(getDisplayPipeCapability|RegisterAGDCCallback|GetPortStatus|AGDCEnableController|SetFbStatusOnNextProbe|getFBIndex|getVendorInfo)' $VT > "$OUT/60-log.txt" 2>&1
python3 kcdis.py "$OUT/61-agdc-filter.txt" "$OUT" $A -- \
  'com.apple.AppleGraphicsDeviceControl:^AppleGraphicsDeviceControl::(filtered_doDeviceAttribute|start)\(' \
  'com.apple.AppleGraphicsDeviceControl:^AppleGraphicsDeviceControlClient::' $VT > "$OUT/61-log.txt" 2>&1
xcrun clang -fno-objc-arc -O1 -framework Foundation -framework Metal -framework IOKit udis_server.m -o udis_server > "$OUT/40-build.txt" 2>&1
CD=/System/Library/Frameworks/CoreDisplay.framework/CoreDisplay
IOP=/System/Library/PrivateFrameworks/IOPresentment.framework/IOPresentment
python3 udis.py "$OUT/70-iopresentment-pipe.txt" "load:$CD" "load:$IOP" \
  'i:IOPresentment:0x921' 'i:IOPresentment:0x2001' 'i:IOPresentment:0x941' \
  'i:IOPresentment:0x2002' 'i:IOPresentment:0x2005' \
  'x:IOPresentment:kAGDCFB' 'x:IOPresentment:Pipe creation failed' 'x:IOPresentment:Interface creation error' \
  'x:IOPresentment:Unable to create IOAccelDisplayPipe' 'x:IOPresentment:Valid ADGC node' \
  --follow '^\?$|ioPresentment|IOPresentment|IOConnect|AGDC' --depth 1 > "$OUT/70-log.txt" 2>&1
python3 udis.py "$OUT/71-iopresentment-symbols.txt" "load:$CD" "load:$IOP" \
  'y:IOPresentment:(Capab|DisplayPipe|PipeCreate|CreateInterface|Transaction)' > "$OUT/71-log.txt" 2>&1
ls -la "$OUT"
exit 0

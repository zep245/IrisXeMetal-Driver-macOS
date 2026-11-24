#!/bin/bash
# Round 16 (display chain, complete map): WindowServer logs "(IOPresentment) Error: Unable to get AGDC information"
# and the GPU wrangler shows "agdc 0x0" for our GPU, so IOPresentment never builds a device map and CoreDisplay never
# lists a display pipe for IrisXeFB. This round maps everything still unknown, before any more driver code:
#  A. AGDC: AppleGraphicsDeviceControl (the kext) and Apple's Intel display driver's own AGDC nub
#     (IntelFBClientControl::vendor_doDeviceAttribute, AppleIntelFramebufferController::createAppleGraphicsDeviceControlnub,
#     GetGPUCapability, getPlatformID ...): what is registered, which attributes are asked, what is answered.
#  B. IOPresentment (user space): every step of its device map (AGDC match, framebuffer entries, platform ID check,
#     accelerator and IOPresentmentClient lookups).
#  C. Showing GPU frames: Apple's Intel display driver's transaction / flip functions and the family display pipe
#     transaction code that calls them.
# Text output only. Round 15 is kept as run15.sh.
set -x
OUT="$PWD/out"; mkdir -p "$OUT"
sw_vers > "$OUT/00-versions.txt" 2>&1
pip3 install --break-system-packages capstone > "$OUT/01-capstone.txt" 2>&1 || pip3 install capstone >> "$OUT/01-capstone.txt" 2>&1
KC=/System/Library/KernelCollections
A="$KC/BootKernelExtensions.kc $KC/SystemKernelExtensions.kc"
IDS="com.apple.AppleGraphicsDeviceControl com.apple.driver.AppleGraphicsControl com.apple.AGDCPluginDisplayMetrics com.apple.driver.AppleIntelICLLPGraphicsFramebuffer com.apple.driver.AppleIntelICLGraphics com.apple.iokit.IOAcceleratorFamily2 com.apple.iokit.IOGraphicsFamily"
python3 kcdump.py "$OUT" $A -- $IDS > "$OUT/03-kcdump-stdout.txt" 2>&1
for f in "$OUT"/12-*-vtables.txt "$OUT"/10-*-symbols.txt; do c++filt < "$f" > "$f.tmp" && mv "$f.tmp" "$f"; done
for f in "$OUT"/13-*-strings.txt; do
  grep -i -E "agdc|vendor|capab|platform|pipe|framebuffer|IOFB|flip|transaction|wrangler|gpu|presentment" "$f" > "$f.tmp"; mv "$f.tmp" "$f"
done
rm -f "$OUT"/11-*-segments.txt
# the IrisXe kexts will need these classes' vtables / sizes to subclass them
VT="--vt AppleGraphicsDeviceControl --vt IntelFBClientControl --vt AppleIntelFramebufferController --vt AppleIntelFramebuffer --vt IOAccelDisplayPipe --vt IOAccelDisplayPipeTransaction2 --vt IOFramebuffer --vt IGAccelDisplayPipe"
python3 kcdis.py "$OUT/30-agdc.txt" "$OUT" $A -- \
  'com.apple.AppleGraphicsDeviceControl:^AppleGraphicsDeviceControl::' \
  'com.apple.AppleGraphicsDeviceControl:^AppleGraphicsDeviceControlUserClient::' \
  'com.apple.AppleGraphicsDeviceControl:^AGDC' $VT > "$OUT/30-log.txt" 2>&1
python3 kcdis.py "$OUT/31-intel-agdc.txt" "$OUT" $A -- \
  'com.apple.driver.AppleIntelICLLPGraphicsFramebuffer:^IntelFBClientControl::' \
  'com.apple.driver.AppleIntelICLLPGraphicsFramebuffer:^AppleIntelFramebufferController::(createAppleGraphicsDeviceControlnub|GetGPUCapability|getPlatformID|AGDCEnableController|getDisplayPipeCapability|RegisterAGDCCallback|GetPortStatus|SetFbStatusOnNextProbe|callPlatformFunction)' $VT > "$OUT/31-log.txt" 2>&1
python3 kcdis.py "$OUT/32-intel-flip.txt" "$OUT" $A -- \
  'com.apple.driver.AppleIntelICLLPGraphicsFramebuffer:^AppleIntelFramebuffer::(ValidateTransaction|PerformFlipTransaction|IsTransactionComplete|PrepareForTransaction|setTransactionCaps|SetTransactionInterrupts|setupInitialTransactionState|PreProcessTransaction|PostProcessTransaction|ValidateTransactionParams)' \
  'com.apple.driver.AppleIntelICLLPGraphicsFramebuffer:^AppleIntelPlane::configurePlane\(' \
  'com.apple.driver.AppleIntelICLGraphics:^IGAccelDisplayPipe::' $VT > "$OUT/32-log.txt" 2>&1
python3 kcdis.py "$OUT/33-family-transaction.txt" "$OUT" $A -- \
  'com.apple.iokit.IOAcceleratorFamily2:^IOAccelDisplayPipe::(.*[Tt]ransaction|.*[Ff]lip|.*vbl|.*VBL|.*[Ss]canout|display_change_handler|init_framebuffer_resource)' \
  'com.apple.iokit.IOAcceleratorFamily2:^IOAccelDisplayPipeTransaction2::(init|set_transaction_args|free)' $VT > "$OUT/33-log.txt" 2>&1
# user space: IOPresentment's device map
xcrun clang -fno-objc-arc -O1 -framework Foundation -framework Metal -framework IOKit udis_server.m -o udis_server > "$OUT/40-build.txt" 2>&1
CD=/System/Library/Frameworks/CoreDisplay.framework/CoreDisplay
IOP=/System/Library/PrivateFrameworks/IOPresentment.framework/IOPresentment
python3 udis.py "$OUT/50-iopresentment-devicemap.txt" "load:$CD" "load:$IOP" \
  'y:IOPresentment:ioPresentment(FindAGDCMatch|PopulateIOFramebufferEntries|ValidatePlatformID|FindGPUWrangler|OpenServiceForID|FindIOServiceMatch|GenerateDeviceMap|ExtractIDs|CreateAssociationMap|GetNodeMap)' \
  'x:IOPresentment:Unable to get AGDC information' \
  'x:IOPresentment:IOPresentmentClient' \
  --follow '^\?$|ioPresentment|IOService|IORegistry|IOConnect|AGDC' --depth 1 > "$OUT/50-log.txt" 2>&1
ls -la "$OUT"
exit 0

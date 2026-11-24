#!/bin/bash
# Round 34 (v0.66.1): hardware video decoding, part 2. Round 33 found the path on Intel Macs: AppleGVA loads the VA
# driver named by the accelerator's IODVDBundleName (AppleIntelICLGraphicsVADriver: Intel's CodecHAL media driver);
# the VA driver builds the video engine's command buffers in user space and hands them to the kext's video context
# (IGAccelVideoContext / -Media), which patches their addresses (patch_codechal_command_buffer) and runs them on the
# media ring (IGHardwareRingBufferMedia). This round reads, as text:
#   1. the kext's video context: method table (getTargetAndMethodForIndex / attach), command-stream token handling
#      (beginCommandStream, process_token_*, patch_codechal_*, bind_resource, endCommandStream) and the media ring
#   2. how IntelAccelerator creates the video user clients (newUserClient types)
#   3. the ICL VA driver (on disk, not in the shared cache): strings, imports / exports, linked libraries, and the
#      functions that open the kext and submit (IOServiceOpen / IOConnectCall* / IOAccel* callers)
#   4. AppleGVA: how it loads the VA driver by IODVDBundleName / IOGVACodec
# Text output only. Round 33 is kept as run33.sh.
set -x
OUT="$PWD/out"; mkdir -p "$OUT"
sw_vers > "$OUT/00-versions.txt" 2>&1
pip3 install --break-system-packages capstone > "$OUT/01-capstone.txt" 2>&1 || pip3 install capstone >> "$OUT/01-capstone.txt" 2>&1
KC=/System/Library/KernelCollections
A="$KC/BootKernelExtensions.kc $KC/SystemKernelExtensions.kc"
I=com.apple.driver.AppleIntelICLGraphics
python3 kcdump.py "$OUT" $A -- $I com.apple.iokit.IOAcceleratorFamily2 > "$OUT/03-kcdump-stdout.txt" 2>&1
for f in "$OUT"/12-*-vtables.txt "$OUT"/10-*-symbols.txt; do [ -f "$f" ] && c++filt < "$f" > "$f.tmp" && mv "$f.tmp" "$f"; done
rm -f "$OUT"/11-*-segments.txt
VT="--vt IGAccelVideoContext --vt IGAccelVideoContextMedia --vt IGAccelVideoContextMain --vt IOAccelContext2 --vt IntelAccelerator --vt IGHardwareRingBufferMedia --vt IGHardwareRingBuffer --vt IOGraphicsAccelerator2"
# 1. the video context
python3 kcdis.py "$OUT/w0-videocontext.txt" "$OUT" $A -- \
  "$I:^IGAccelVideoContext::(getTargetAndMethodForIndex|attach|contextStart|contextStop|beginCommandStream|endCommandStream|process_token_|patch_codechal|bind_resource|prepare_to_decode|processSidebandToken|discardSidebandToken|addDataBufferToChannel|removeDataBufferFromChannel|set_current_resource|getChannel|getDataBufferLimits|get_iosurface_accel_id|map_memory|populateContextConfig)" \
  "$I:^IGAccelVideoContextMedia::" $VT > "$OUT/w0-log.txt" 2>&1
python3 kcdis.py "$OUT/w1-mediaring.txt" "$OUT" $A -- "$I:^IGHardwareRingBufferMedia::" "$I:^IGHardwareRingBuffer::(init|start|submit|setRingBufferRegisters|getRingRegister|kick|initRing|enable)" $VT > "$OUT/w1-log.txt" 2>&1
# 2. user client types
python3 kcdis.py "$OUT/w2-newuserclient.txt" "$OUT" $A -- "$I:^IntelAccelerator::(newUserClient|createUserClient|newVideoContext|getVideoContext)" \
  "com.apple.iokit.IOAcceleratorFamily2:^IOGraphicsAccelerator2::newUserClient" $VT > "$OUT/w2-log.txt" 2>&1
grep -a -E "IGAccelVideo|VideoContext|sIntelVideo|CODECHAL|codechal|_TOKEN|Media|MFX|HCP|VDBox|VDBOX|VCS" "$OUT"/10-$I-symbols.txt > "$OUT/w3-video-symbols.txt" 2>&1
grep -a -iE "video|codechal|token|media|mfx|hcp|vdbox|vcs|huc|avc|hevc|jpeg|mpeg|decode|encode|patch" "$OUT"/13-$I-strings.txt > "$OUT/w4-video-strings.txt" 2>&1
rm -f "$OUT"/13-*-strings.txt "$OUT"/10-com.apple.iokit.IOAcceleratorFamily2-symbols.txt
# 3. the ICL VA driver (on disk)
VA=/System/Library/Extensions/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/AppleIntelICLGraphicsVADriver
plutil -p /System/Library/Extensions/AppleIntelICLGraphicsVADriver.bundle/Contents/Info.plist > "$OUT/w10-va-plist.txt" 2>&1
otool -L "$VA" > "$OUT/w11-va-links.txt" 2>&1
otool -l "$VA" | grep -A4 -E "LC_ID|LC_LOAD|LC_SEGMENT_64" | head -200 >> "$OUT/w11-va-links.txt" 2>&1
nm -m "$VA" | grep -iE "undefined" > "$OUT/w12-va-imports.txt" 2>&1
nm -gm "$VA" | grep -v undefined > "$OUT/w13-va-exports.txt" 2>&1
nm "$VA" | c++filt | grep -iE " t | T " | grep -iE "Ioctl|ioctl|Submit|submit|Exec|Batch|IOConnect|IOAccel|GVA|Create|Open|Device|Context|Token|token|Patch|Codechal|CodecHal|Mos_Specific|MosInterface|Gen11|Gen12|G11|G12|Icl|Tgl|Sku|WaTable|DeviceId|PlatformInfo" | head -4000 > "$OUT/w14-va-functions.txt" 2>&1
strings -a "$VA" | grep -iE "IOAccel|IOConnect|IGAccel|IntelAccel|Video|token|Token|ioctl|GEN11|GEN12|ICL|TGL|Tiger|Ice Lake|Device|PCI|SKU|0x8a|0x9a|HuC|GuC|MOCS|failed|error|Error" | sort -u | head -6000 > "$OUT/w15-va-strings.txt" 2>&1
xcrun clang -fno-objc-arc -O1 -framework Foundation -framework Metal -framework IOKit udis_server.m -o udis_server > "$OUT/w19-build.txt" 2>&1
python3 udis.py "$OUT/w16-va-kernel-calls.txt" "load:$VA" \
  'k:AppleIntelICLGraphicsVADriver:IOServiceOpen' 'k:AppleIntelICLGraphicsVADriver:IOConnectCallMethod' \
  'k:AppleIntelICLGraphicsVADriver:IOConnectCallStructMethod' 'k:AppleIntelICLGraphicsVADriver:IOConnectCallScalarMethod' \
  'k:AppleIntelICLGraphicsVADriver:IOConnectMapMemory64' 'k:AppleIntelICLGraphicsVADriver:IOAccelContextSubmitDataBuffersExt2' \
  'k:AppleIntelICLGraphicsVADriver:IOAccelContextSubmitDataBuffers' 'k:AppleIntelICLGraphicsVADriver:IOAccelDeviceCreateWithAPIProperty' \
  'k:AppleIntelICLGraphicsVADriver:IOAccelContextCreate' 'x:AppleIntelICLGraphicsVADriver:IGAccelVideo' \
  'y:AppleIntelICLGraphicsVADriver:(MosOsSpecific|Mos_Specific_Submit|SubmitCommandBuffer|GetPlatform|DeviceId|deviceId|InitPlatform|InitSku|InitWa)' \
  --follow 'Submit|submit|IOConnect|IOAccel|Platform|Sku|Wa' > "$OUT/w16-log.txt" 2>&1
# 4. AppleGVA loading the VA driver
GVA=/System/Library/PrivateFrameworks/AppleGVA.framework/AppleGVA
python3 udis.py "$OUT/w20-gva-load.txt" "load:$GVA" 'x:AppleGVA:IODVDBundleName' 'x:AppleGVA:IOGVACodec' \
  'x:AppleGVA:.bundle/Contents/MacOS/' 'x:AppleGVA:AppleIntelICLGraphicsVADriver' 'x:AppleGVA:IOGVAHEVCDecode' > "$OUT/w20-log.txt" 2>&1
ls -la "$OUT"
exit 0

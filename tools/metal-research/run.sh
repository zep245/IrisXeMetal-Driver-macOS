#!/bin/bash
# Round 50 (hardware video decoding, R8): where an IOSurface resource keeps its IOSurface, memory and GPU address. Text only.
# Round 49 is kept as run49.sh.
# Round 48 (R4, R6, R7) is kept as run48.sh.
#   R4: what happens to a frame after its End token in Apple's Intel kernel driver: SetFence / SetBlockFence,
#       BatchBufferStart, submission to the media ring, the stamp / event that tells the VA driver "done",
#       the video engine's interrupt and hang handling.
#   R6: HEVC / VP9: the codec tokens, HuC (firmware microcontroller) use, short / long format - kernel and VA driver.
#   R7: the same on macOS 26 Tahoe (the workflow runs this script on macos-15-intel AND macos-26-intel; the two
#       outputs are compared offline: vtable slots, struct offsets in the disassembly, token tables).
# Round 47 is kept as run47.sh.
set -x
OUT="$PWD/out"; mkdir -p "$OUT"
sw_vers > "$OUT/00-versions.txt" 2>&1
uname -a >> "$OUT/00-versions.txt" 2>&1
kmutil showloaded --list-only 2>/dev/null | grep -i -E "Accelerator|IntelICL|IntelKBL|GVA" >> "$OUT/00-versions.txt" 2>&1
python3 -c "import capstone" 2>/dev/null || pip3 install --user capstone > "$OUT/01-capstone.txt" 2>&1 || pip3 install --break-system-packages capstone >> "$OUT/01-capstone.txt" 2>&1
KC=/System/Library/KernelCollections
A="$KC/BootKernelExtensions.kc $KC/SystemKernelExtensions.kc"
F=com.apple.iokit.IOAcceleratorFamily2
I=com.apple.driver.AppleIntelICLGraphics
python3 kcdump.py "$OUT" $A -- $F $I > "$OUT/03-kcdump-stdout.txt" 2>&1
for f in "$OUT"/12-*-vtables.txt "$OUT"/10-*-symbols.txt; do [ -f "$f" ] && c++filt < "$f" > "$f.tmp" && mv "$f.tmp" "$f"; done
rm -f "$OUT"/11-*-segments.txt "$OUT"/02-*.txt

VT="--vt IOAccelResource2 --vt IGAccelResource --vt IOAccelVideoContext2 --vt IGAccelVideoContext --vt IGAccelVideoContextMedia --vt IOGraphicsAccelerator2 --vt IntelAccelerator --vt IOAccelMemory --vt IOAccelSharedUserClient2 --vt IOAccelShared2"
S=com.apple.iokit.IOSurface
# --- R8: where an IOSurface resource keeps its IOSurface / memory / GPU address (v0.69.0 and v0.69.1 guessed wrong:
# resource +0xe0 and (+0xe0)+0x10 are not the IOSurface for the VA driver's picture buffers -> kernel panics)
python3 kcdis.py "$OUT/70-r8-family-resource.txt" "$OUT" $A -- \
  "$F:^IOAccelResource2::(newResourceWithIOSurface|newResourceWithIOSurfaceDeviceCache|initialize|init|initWith.*|free|map|prepare|getPhysicalOffset|getResourceInfo|update_iosurface_valid_bits|update_iosurface_textures_stamp|testIOSurfacePurgeable|setPurgeableIOSurface|getResidentSize|getStorageResource|getGPUVirtualAddressForUserProcess|getMemoryAllocParameter|.*[Ss]urface.*)" \
  "$F:^IOAccelSharedUserClient2::(new_resource|.*[Ss]urface.*)" \
  "$F:^IOAccelShared2::(new_resource|lookupResource|.*[Ss]urface.*)" $VT > "$OUT/70-log.txt" 2>&1
python3 kcdis.py "$OUT/71-r8-intel-resource.txt" "$OUT" $A -- \
  "$I:^IGAccelResource::.*" \
  "$I:^IGAccelVideoContext::(bind_resource|set_current_resource|getResource.*|patch_codechal_command_buffer|process_token_CodecHAL)" \
  "$S:^IOSurface::(getPlaneOffset|getMemoryDescriptor|getAllocSize|getBytesPerRow|getPlaneCount|getPlaneBytesPerRow|getSurfaceID|getMetaClass)" $VT > "$OUT/71-log.txt" 2>&1
gzip -9 "$OUT"/7*.txt 2>/dev/null
ls -la "$OUT"
exit 0

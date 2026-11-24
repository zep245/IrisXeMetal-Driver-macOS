#!/bin/bash
# Round 43 results (v0.68.4): VideoToolbox finds a hardware decoder only through a Metal device of the process. With
# our Metal device visible in the video processes, VTIsHardwareDecodeSupported(H.264) = 1 and the Intel GVA decoders
# are listed (h264.gva, vpx.gva). Opening the decoder then crashed VTDecoderXPCService in Apple's VA driver: it makes
# an IOSurface-backed resource (IOAccelResourceCreate, type 0x82) and reads IOAccelResourceGetClientSharedPrivate(res,
# 0)->+8 - NULL on our accelerator (our resources have no client-shared private part).
# Round 44 (hardware video decoding, part 12): the resource client-shared private part - which IOAccelConfig field
# sizes it (IOAccelClientSharedMachine, IOAccelShared2, IOAccelResource2::initClientShared / getClientShared*Private),
# what IOAccelerator.framework's IOAccelResourceGetClientSharedPrivate returns, and what Apple's Intel resource writes
# there (IGAccelResource*, AppleParavirtResource*). Round 43 is kept as run43.sh.
set -x
OUT="$PWD/out"; mkdir -p "$OUT"
sw_vers > "$OUT/00-versions.txt" 2>&1
python3 -c "import capstone" 2>/dev/null || pip3 install --user capstone > "$OUT/01-capstone.txt" 2>&1 || pip3 install --break-system-packages capstone >> "$OUT/01-capstone.txt" 2>&1
KC=/System/Library/KernelCollections
A="$KC/BootKernelExtensions.kc $KC/SystemKernelExtensions.kc"
F=com.apple.iokit.IOAcceleratorFamily2
I=com.apple.driver.AppleIntelICLGraphics
P=com.apple.driver.AppleParavirtGPU
python3 kcdump.py "$OUT" $A -- $F $I $P > "$OUT/03-kcdump-stdout.txt" 2>&1
for f in "$OUT"/12-*-vtables.txt "$OUT"/10-*-symbols.txt; do [ -f "$f" ] && c++filt < "$f" > "$f.tmp" && mv "$f.tmp" "$f"; done
rm -f "$OUT"/11-*-segments.txt "$OUT"/02-*.txt
VT="--vt IOAccelResource2 --vt IOGraphicsAccelerator2 --vt IOAccelShared2 --vt IntelAccelerator --vt IGAccelResource"
python3 kcdis.py "$OUT/a0-family.txt" "$OUT" $A -- \
  "$F:^IOAccelClientSharedMachine::" \
  "$F:^IOAccelResource2::(initClientShared|getClientShared|init\(|initialize|newResourceWithIOSurface|copyResourcePrivate|getResourceInfo)" \
  "$F:^IOAccelShared2::(init|new_resource|s_new_resource|newResource|createResource|initClientShared|getClientShared)" \
  "$F:^IOGraphicsAccelerator2::(initializeConfigStructure|getAccelConfig|createClientShared|newClientShared)" $VT > "$OUT/a0-log.txt" 2>&1
python3 kcdis.py "$OUT/a1-intel.txt" "$OUT" $A -- \
  "$I:^IGAccelResource[A-Za-z0-9]*::" \
  "$I:(ClientShared|SharedRO|SharedRW|ROPrivate|RWPrivate)" $VT > "$OUT/a1-log.txt" 2>&1
python3 kcdis.py "$OUT/a2-paravirt.txt" "$OUT" $A -- \
  "$P:^AppleParavirtResource::" "$P:(ClientShared|ROPrivate|RWPrivate)" $VT > "$OUT/a2-log.txt" 2>&1
# user space: IOAccelerator.framework's resource calls
xcrun clang -fno-objc-arc -O1 -framework Foundation -framework Metal -framework IOKit udis_server.m -o udis_server > "$OUT/40-build.txt" 2>&1
python3 udis.py "$OUT/50-ioaccel-user.txt" \
  'load:/System/Library/PrivateFrameworks/IOAccelerator.framework/IOAccelerator' \
  's:IOAccelResourceGetClientSharedPrivate' 's:IOAccelResourceGetClientShared' 's:IOAccelResourceCreate' \
  's:IOAccelResourceGetClientSharedRO' 's:IOAccelResourceGetClientSharedRW' \
  --follow '(?i)(resource|shared|private|IOConnect)' --depth 2 > "$OUT/50-log.txt" 2>&1
rm -f "$OUT"/13-*-strings.txt "$OUT"/12-*-vtables.txt
ls -la "$OUT"
exit 0

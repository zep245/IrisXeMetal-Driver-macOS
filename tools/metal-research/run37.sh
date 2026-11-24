#!/bin/bash
# Round 37 (v0.68.1): hardware video decoding, part 5 - the device's hardware-info methods. The v0.68.1 test boot:
# AppleGVA made all three video contexts, called their selector 2, and the ICL VA driver then logged "Failed to get hw
# caps from kernel" (no command streams). Apple's Intel device user client (IGAccelDevice, made by
# IntelAccelerator::newDevice) has get_hw_caps / get_hw_info / get_wa_table / get_vram_size ...: the Intel media
# driver's platform, SKU and workaround tables. This round reads, as text:
#   1. IGAccelDevice: getTargetAndMethodForIndex, deviceStart's methodDescs table (selector -> method, sizes), and
#      get_hw_caps / get_hw_info / get_wa_table / get_vram_size (what they fill in, from where)
#   2. where IntelAccelerator keeps that data (functions that fill the hw info / SKU / WA tables at start)
#   3. the family's IOAccelDevice2 (base class): vtable, deviceStart, getTargetAndMethodForIndex, externalMethod,
#      and IOAccelContext2's selector table (what selector 2 of a context is)
#   4. IGAccelSharedUserClient's methodDescs (the shared user client's Intel methods)
# Text output only. Round 36 is kept as run36.sh.
set -x
OUT="$PWD/out"; mkdir -p "$OUT"
sw_vers > "$OUT/00-versions.txt" 2>&1
pip3 install --break-system-packages capstone > "$OUT/01-capstone.txt" 2>&1 || pip3 install capstone >> "$OUT/01-capstone.txt" 2>&1
KC=/System/Library/KernelCollections
A="$KC/BootKernelExtensions.kc $KC/SystemKernelExtensions.kc"
I=com.apple.driver.AppleIntelICLGraphics
F=com.apple.iokit.IOAcceleratorFamily2
python3 kcdump.py "$OUT" $A -- $I $F > "$OUT/03-kcdump-stdout.txt" 2>&1
for f in "$OUT"/12-*-vtables.txt "$OUT"/10-*-symbols.txt; do [ -f "$f" ] && c++filt < "$f" > "$f.tmp" && mv "$f.tmp" "$f"; done
rm -f "$OUT"/11-*-segments.txt
VT="--vt IGAccelDevice --vt IOAccelDevice2 --vt IntelAccelerator --vt IOGraphicsAccelerator2 --vt IGAccelSharedUserClient --vt IOAccelSharedUserClient2 --vt IOAccelContext2 --vt IGHardwareContext"
python3 kcdis.py "$OUT/z0-igdevice.txt" "$OUT" $A -- "$I:^IGAccelDevice::" "$I:^IntelAccelerator::newDevice" $VT > "$OUT/z0-log.txt" 2>&1
python3 kcdis.py "$OUT/z1-tables.txt" "$OUT" $A -- "$I:(deviceStart\(\)::methodDescs|sharedStart\(\)::methodDescs)$" "$F:(method|Method)[A-Za-z]*s$" $VT > "$OUT/z1-log.txt" 2>&1
python3 kcdis.py "$OUT/z2-hwinfo-fill.txt" "$OUT" $A -- \
  "$I:^IntelAccelerator::(.*[Hh]wInfo|.*HwCaps|.*[Ss]ku|.*WaTable|.*Workaround|getGTSystemInfo|initPlatform|.*SystemInfo|.*PlatformInfo|.*FtrTable|getDeviceInfo|start\(IOService)" \
  "$I:^IGHardwareContext::(.*[Hh]wInfo|.*Sku|.*Wa)" $VT > "$OUT/z2-log.txt" 2>&1
python3 kcdis.py "$OUT/z3-family-device.txt" "$OUT" $A -- \
  "$F:^IOAccelDevice2::(getTargetAndMethodForIndex|externalMethod|deviceStart|deviceStop|init|start|s_)" \
  "$F:^IOAccelContext2::(getTargetAndMethodForIndex|externalMethod|s_)" $VT > "$OUT/z3-log.txt" 2>&1
grep -a "^vtable for IOAccelDevice2 " -A320 "$OUT"/12-$F-vtables.txt | sed '/^vtable for .*MetaClass/,$d' > "$OUT/z4-vtable-IOAccelDevice2.txt" 2>&1
grep -a "^vtable for IGAccelDevice " -A320 "$OUT"/12-$I-vtables.txt | sed '/^vtable for .*MetaClass/,$d' > "$OUT/z4-vtable-IGAccelDevice.txt" 2>&1
grep -a -E "IntelHwCapsInfo|IntelHwInfoRec|_WA_TABLE|_SKU_FEATURE|GT_SYSTEM_INFO|IntelSkuFeature|PLATFORM" "$OUT"/10-$I-symbols.txt | sort -u > "$OUT/z5-types.txt" 2>&1
grep -a -iE "hw caps|hwcaps|hw info|wa table|sku|gt system|platform" "$OUT"/13-$I-strings.txt > "$OUT/z6-strings.txt" 2>&1
rm -f "$OUT"/13-*-strings.txt "$OUT"/10-$F-symbols.txt
# the VA driver's side: strings around the hardware info
VA=/System/Library/Extensions/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/AppleIntelICLGraphicsVADriver
strings -a "$VA" | grep -iE "hw cap|hwcap|hw info|hwinfo|wa table|sku|gt system|platform|device id|deviceid|kernel" | sort -u > "$OUT/z7-va-strings.txt" 2>&1
ls -la "$OUT"
exit 0

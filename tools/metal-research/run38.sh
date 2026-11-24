#!/bin/bash
# Round 38 (v0.68.1): hardware video decoding, part 6 - the hardware-info data. Round 37: IGAccelDevice's Intel
# methods are selectors 10..24 (deviceStart's methodDescs, 15 entries of 0x30 bytes); get_hw_caps copies 0x80 bytes
# from IntelAccelerator+0x1140 (initHardwareCaps fills them), get_wa_table 0xda bytes from +0xf36, get_hw_info 0x288
# bytes (+0x1140 0x80, +0xf36 0xda, +0x1010 0x6e, +0x1080 0xc0). This round reads, as text:
#   1. the method tables decoded as data (round 37's dump missed them: kcdis.py now also dumps *Descs / *Methods)
#   2. who fills IntelAccelerator+0x1140 / +0xf36 / +0x1010 / +0x1080: initHardwareCaps, InitIclLpWaTable and the
#      other Init*WaTable / *SkuTable / GT system info functions, and IntelAccelerator::start
# Only Python + capstone: this round also runs on a Mac with just the Command Line Tools (no Xcode):
#     cd tools/metal-research && bash run.sh      (results in tools/metal-research/out)
# Text output only. Round 37 is kept as run37.sh.
set -x
OUT="$PWD/out"; mkdir -p "$OUT"
sw_vers > "$OUT/00-versions.txt" 2>&1
python3 -c "import capstone" 2>/dev/null || pip3 install --user capstone > "$OUT/01-capstone.txt" 2>&1 || pip3 install --break-system-packages capstone >> "$OUT/01-capstone.txt" 2>&1
KC=/System/Library/KernelCollections
A="$KC/BootKernelExtensions.kc $KC/SystemKernelExtensions.kc"
I=com.apple.driver.AppleIntelICLGraphics
F=com.apple.iokit.IOAcceleratorFamily2
python3 kcdump.py "$OUT" $A -- $I $F > "$OUT/03-kcdump-stdout.txt" 2>&1
for f in "$OUT"/12-*-vtables.txt "$OUT"/10-*-symbols.txt; do [ -f "$f" ] && c++filt < "$f" > "$f.tmp" && mv "$f.tmp" "$f"; done
rm -f "$OUT"/11-*-segments.txt
VT="--vt IGAccelDevice --vt IOAccelDevice2 --vt IntelAccelerator --vt IOGraphicsAccelerator2 --vt IGHardwareContext"
python3 kcdis.py "$OUT/v0-tables.txt" "$OUT" $A -- "$I:(deviceStart\(\)::methodDescs|sharedStart\(\)::methodDescs|attach\(IOService\*\)::method_descs)$" \
  "$F:^IOAccelDevice2::sDeviceMethods$" "$F:^IOAccelContext2::sContextMethods$" $VT > "$OUT/v0-log.txt" 2>&1
python3 kcdis.py "$OUT/v1-hwcaps.txt" "$OUT" $A -- \
  "$I:^IntelAccelerator::(initHardwareCaps|initHardwareWorkarounds|initSku|initPlatform|initGTSystemInfo|.*SystemInfo|.*Sku.*|.*Platform.*|getDeviceID|getRevisionID)" \
  "$I:^(Init.*WaTable|Init.*SkuTable|Init.*Sku|Init.*GtSysInfo|Init.*SystemInfo|.*GT_SYSTEM_INFO.*)" $VT > "$OUT/v1-log.txt" 2>&1
python3 kcdis.py "$OUT/v2-start.txt" "$OUT" $A -- "$I:^IntelAccelerator::start\(IOService\*\)$" $VT > "$OUT/v2-log.txt" 2>&1
grep -a -E "WaTable|SkuTable|SystemInfo|HardwareCaps|Platform|FtrTable|GtSysInfo" "$OUT"/10-$I-symbols.txt | sort -u > "$OUT/v3-symbols.txt" 2>&1
grep -a -iE "sku|wa table|platform|gt system|hardware caps|hwcaps|eu count|subslice|slice" "$OUT"/13-$I-strings.txt > "$OUT/v4-strings.txt" 2>&1
rm -f "$OUT"/13-*-strings.txt "$OUT"/10-$F-symbols.txt "$OUT"/02-*.txt
ls -la "$OUT"
exit 0

#!/bin/bash
# Round 38 results (v0.68.1): IGAccelDevice selectors 10..24 = get_wa_table, get_hw_caps, debug_control, 5 telemetry,
# logStatus, 4 aub capture, get_vram_size (23), get_hw_info (24). The data lives in IntelAccelerator: hw caps at
# +0x1140 (0x80 bytes: +0x1140 u16 device id?, +0x1144 u16 revision?, +0x1148 ..., +0x1150 GT type, +0x1188 ...),
# WA table +0xf36 (0xda), SKU table +0x1010 (0x6e), GT system info +0x1080 (0xc0); initHardwareCaps and
# InitIclLpWaTable(WA, SKU, [+0x1148], [+0x1140], [+0x1144]) fill parts of it, IntelAccelerator::start does not fill
# the rest. Round 39 (hardware video decoding, part 7): WHO writes +0xf36 .. +0x11c0 - every IntelAccelerator /
# IGHardware* / global init function is disassembled and only the functions touching that range are kept, plus
# InitIclLpWaTable in full and the hardware-info reads (PCI config, fuse registers).
# Python + capstone only (also runs with just the Command Line Tools). Text output only. Round 38 is kept as run38.sh.
set -x
OUT="$PWD/out"; mkdir -p "$OUT"
sw_vers > "$OUT/00-versions.txt" 2>&1
python3 -c "import capstone" 2>/dev/null || pip3 install --user capstone > "$OUT/01-capstone.txt" 2>&1 || pip3 install --break-system-packages capstone >> "$OUT/01-capstone.txt" 2>&1
KC=/System/Library/KernelCollections
A="$KC/BootKernelExtensions.kc $KC/SystemKernelExtensions.kc"
I=com.apple.driver.AppleIntelICLGraphics
python3 kcdump.py "$OUT" $A -- $I > "$OUT/03-kcdump-stdout.txt" 2>&1
for f in "$OUT"/12-*-vtables.txt "$OUT"/10-*-symbols.txt; do [ -f "$f" ] && c++filt < "$f" > "$f.tmp" && mv "$f.tmp" "$f"; done
rm -f "$OUT"/11-*-segments.txt "$OUT"/02-*.txt
VT="--vt IntelAccelerator --vt IOGraphicsAccelerator2 --vt IGHardwareContext --vt IOPCIDevice"
python3 kcdis.py "$OUT/all-intelaccel.txt" "$OUT" $A -- "$I:^IntelAccelerator::" "$I:^(Init|init|Get|get|Set|set)[A-Za-z]*(Sku|Wa|Platform|SysInfo|SystemInfo|GtInfo|GTInfo|Hw|HW)" $VT > "$OUT/u0-log.txt" 2>&1
python3 - "$OUT/all-intelaccel.txt" > "$OUT/u1-writers.txt" <<'PY'
import re, sys
txt = open(sys.argv[1], errors='replace').read().split('\n### ')
for blk in txt:
    lines = blk.split('\n'); name = lines[0]
    hits = []
    for l in lines[1:]:
        for m in re.finditer(r'\[r[a-z0-9]+ \+ 0x([0-9a-f]+)\]', l):
            off = int(m.group(1), 16)
            if 0xf30 <= off <= 0x11d0 and l.split(':', 1)[-1].strip().startswith(('mov', 'or ', 'and', 'lea', 'movzx', 'add', 'inc', 'bts', 'btr', 'rep')):
                hits.append(l.strip()); break
    if hits:
        print('### ' + name)
        print('\n'.join(hits[:400]))
        print()
PY
python3 - "$OUT/all-intelaccel.txt" "$OUT/u2-functions.txt" <<'PY'
import re, sys
txt = open(sys.argv[1], errors='replace').read().split('\n### ')
keep = []
for blk in txt:
    name = blk.split('\n', 1)[0]
    if re.search(r'Init.*WaTable|initHardware|Sku|Platform|SysInfo|SystemInfo|GtInfo|configureDevice|getDeviceID|readFuse|Fuse|EUCount|Slice|probe|initPCI|PCI', name) or \
       re.search(r'\[r[a-z0-9]+ \+ 0x(10[1-9a-f][0-9a-f]|11[0-9a-c][0-9a-f])\]', blk):
        keep.append('### ' + blk)
open(sys.argv[2], 'w').write('\n'.join(keep))
PY
rm -f "$OUT/all-intelaccel.txt"
grep -a -iE "sku|wa table|platform|gt system|hardware caps|eu count|subslice|slice|device id|stepping|revision" "$OUT"/13-$I-strings.txt > "$OUT/u3-strings.txt" 2>&1
rm -f "$OUT"/13-*-strings.txt
ls -la "$OUT"
exit 0

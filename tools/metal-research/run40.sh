#!/bin/bash
# Round 39 results (v0.68.2): IntelAccelerator's hardware caps are filled by probe (PCI ids, revision), getGPUInfo
# (fuse registers: slices, subslices, EUs, GT type) and initHardwareCaps; v0.68.2's own device connection answers
# get_hw_caps with Ice Lake GT2 values. The v0.68.2 test boot: Apple's VA driver now gets past "Failed to get hw
# caps" (selector 11 answered, 0x80 bytes), then asks the accelerator id and sets perf / priority on its contexts -
# and then stops (no hw_info, no WA table, no command streams; VTIsHardwareDecodeSupported = 0).
# Round 40 (hardware video decoding, part 8): WHAT the VA driver checks after the hw caps - its user-space code is
# disassembled: every function that loads its kernel error strings, every function calling the IOKit connection
# calls, and its symbols for caps / SKU / WA / GT info / platform / device checks (2 levels followed).
# Python + capstone + a small Objective-C helper. Text output only. Round 39 is kept as run39.sh.
set -x
OUT="$PWD/out"; mkdir -p "$OUT"
sw_vers > "$OUT/00-versions.txt" 2>&1
python3 -c "import capstone" 2>/dev/null || pip3 install --user capstone > "$OUT/01-capstone.txt" 2>&1 || pip3 install --break-system-packages capstone >> "$OUT/01-capstone.txt" 2>&1
xcrun clang -fno-objc-arc -O1 -framework Foundation -framework Metal -framework IOKit udis_server.m -o udis_server > "$OUT/40-build.txt" 2>&1
VA=/System/Library/Extensions/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/AppleIntelICLGraphicsVADriver
ls -la "$(dirname "$VA")" > "$OUT/41-va-files.txt" 2>&1
otool -L "$VA" >> "$OUT/41-va-files.txt" 2>&1
strings -a "$VA" | sort -u > "$OUT/42-va-strings-all.txt" 2>&1
# every exported / local symbol name of the VA driver (names only)
nm -m "$VA" > "$OUT/43-va-nm.txt" 2>&1 || true
python3 udis.py "$OUT/50-va-errors.txt" "load:$VA" \
  'x:AppleIntelICLGraphicsVADriver:hw caps' \
  'x:AppleIntelICLGraphicsVADriver:accelerator id' \
  'x:AppleIntelICLGraphicsVADriver:Not supported' \
  'x:AppleIntelICLGraphicsVADriver:not supported' \
  'x:AppleIntelICLGraphicsVADriver:Unsupported' \
  'x:AppleIntelICLGraphicsVADriver:Failed' \
  --follow '.' --depth 1 > "$OUT/50-log.txt" 2>&1
python3 udis.py "$OUT/51-va-iokit.txt" "load:$VA" \
  'k:AppleIntelICLGraphicsVADriver:IOConnectCallMethod' \
  'k:AppleIntelICLGraphicsVADriver:IOConnectCallStructMethod' \
  'k:AppleIntelICLGraphicsVADriver:IOConnectCallScalarMethod' \
  'k:AppleIntelICLGraphicsVADriver:IOServiceOpen' \
  'k:AppleIntelICLGraphicsVADriver:IORegistryEntryCreateCFProperty' \
  'k:AppleIntelICLGraphicsVADriver:IORegistryEntrySearchCFProperty' \
  > "$OUT/51-log.txt" 2>&1
python3 udis.py "$OUT/52-va-caps.txt" "load:$VA" \
  'y:AppleIntelICLGraphicsVADriver:(?i)(hwcaps|hw_caps|hwinfo|hw_info|skutable|sku_|watable|wa_table|gtsystem|gt_sys|sysinfo|platform|deviceid|device_id|getdevice|isdecode|supportsdecode|capabilit|initializ|probe|create.*device|codechal.*init|mos.*init)' \
  --follow '(?i)(caps|info|sku|wa|platform|device|init|support)' --depth 2 > "$OUT/52-log.txt" 2>&1
ls -la "$OUT"
exit 0

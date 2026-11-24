#!/bin/bash
# IrisXe hang report (v0.68.34+). Changes nothing: it only records.
# Run it right after a black flash / freeze / white bar: it saves what the driver kept about the last GPU hangs
# (the hung job's commands, the GPU state, which program, which shaders) and which programs use the GPU.
#     sudo bash irisxe-hangdump.sh
cd "$(dirname "$0")" || exit 1
if [ "$(id -u)" != 0 ]; then echo "Run with sudo: sudo bash irisxe-hangdump.sh"; exit 1; fi
T=$(date +%Y%m%d-%H%M%S)
OUT="irisxe-hang-$T"
mkdir -p "$OUT"
ioreg -l -w0 | grep '"IrisXe-' | sed -E 's/^[ |]*//' > "$OUT/properties.txt"
log show --last 30m --style compact --predicate 'process == "kernel" AND eventMessage CONTAINS "IrisXe" AND NOT eventMessage CONTAINS "display pipe:"' > "$OUT/kernel-log.txt" 2>/dev/null
log show --last 30m --style compact --predicate 'eventMessage CONTAINS "IrisXeMTL: enabled in" OR eventMessage CONTAINS "IrisXeMTL: render pipeline ok" OR eventMessage CONTAINS "IrisXeMTL: render pipeline failed"' 2>/dev/null | tail -3000 > "$OUT/programs-and-shaders.txt"
nvram irisxe-apps > "$OUT/nvram.txt" 2>&1; nvram irisxe-noapps >> "$OUT/nvram.txt" 2>&1
ps axo pid,comm | grep -i -E "helper|claude|github" > "$OUT/processes.txt"
sw_vers > "$OUT/system.txt"
zip -qr "$OUT.zip" "$OUT"
chown "$(stat -f %Su /dev/console)" "$OUT.zip" 2>/dev/null
echo "Send this file: $(pwd)/$OUT.zip"

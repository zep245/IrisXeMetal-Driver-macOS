#!/bin/bash
# IrisXe Brave shader collector (v0.68.32+).
# Brave draws with Skia Graphite -> Dawn -> Metal; every shader our compiler cannot build yet is saved as AIR.
# This collects all of them in one zip, so they can be compiled and fixed offline many at a time.
#   sudo bash irisxe-bravetest.sh clean    delete the old saved shaders (run this first)
#   sudo bash irisxe-bravetest.sh          after using Brave for a minute: collect the shaders + logs into a zip
cd "$(dirname "$0")" || exit 1
if [ "$(id -u)" != 0 ]; then echo "Run with sudo: sudo bash irisxe-bravetest.sh"; exit 1; fi
DIRS=$( { echo /var/tmp/irisxe-fail; find /private/var/folders -maxdepth 4 -type d -name irisxe-fail 2>/dev/null
          find /Users/*/Library/Containers -maxdepth 4 -type d -name irisxe-fail 2>/dev/null; } | sort -u)
if [ "$1" = "clean" ]; then
  for d in $DIRS; do rm -rf "$d"; done
  echo "Old saved shaders deleted. Now quit Brave fully (Cmd+Q), open it, browse a few sites for a minute,"
  echo "then run: sudo bash irisxe-bravetest.sh"
  exit 0
fi
T=$(date +%Y%m%d-%H%M%S)
OUT="irisxe-brave-$T"
mkdir -p "$OUT/shaders"
n=0
for d in $DIRS; do
  [ -d "$d" ] || continue
  for f in "$d"/*.bc; do [ -f "$f" ] && cp "$f" "$OUT/shaders/" && n=$((n+1)); done
done
log show --last 20m --style compact --predicate 'eventMessage CONTAINS "IrisXeMTL"' > "$OUT/metal-log.txt" 2>/dev/null
sw_vers > "$OUT/system.txt"
# which programs used the driver, and whether Brave's GPU process ran at all
grep -E "IrisXeMTL: (enabled|not enabled|disabled|off) " "$OUT/metal-log.txt" | sed -E 's/^.{24}//' | sort -u > "$OUT/programs.txt"
ps axo pid,comm | grep -i -E "brave|helper" > "$OUT/processes.txt"
log show --last 20m --style compact --predicate 'process BEGINSWITH "Brave"' 2>/dev/null | grep -E "Brave Browser Helper \([A-Za-z]+\)\[" | sed -E 's/.*(Brave Browser Helper \([A-Za-z]+\)).*/\1/' | sort | uniq -c >> "$OUT/processes.txt"
nvram irisxe-apps > "$OUT/nvram.txt" 2>&1; nvram irisxe-noapps >> "$OUT/nvram.txt" 2>&1
# a freeze: GPU timeouts / hangs (kernel side), Brave's own GPU messages, and crash reports
ioreg -l -w0 | grep -E '"IrisXe-(AsyncState|GPUDesktop|NoCore|HungDraws)"' > "$OUT/gpu-state.txt" 2>/dev/null
log show --last 20m --style compact --predicate 'process == "kernel" AND eventMessage CONTAINS[c] "irisxe"' > "$OUT/kernel-log.txt" 2>/dev/null
log show --last 20m --style compact --predicate 'process BEGINSWITH "Brave"' 2>/dev/null | grep -i -E "gpu|metal|dawn|graphite|crash|lost|hang|timeout|IrisXe" | tail -3000 > "$OUT/brave-log.txt"
U=$(stat -f %Su /dev/console)
find "/Users/$U/Library/Logs/DiagnosticReports" /Library/Logs/DiagnosticReports -maxdepth 1 -type f -mmin -30 \
     \( -iname "*Brave*" -o -iname "WindowServer*" -o -iname "*.panic" -o -iname "*hang*" \) -exec cp {} "$OUT/" \; 2>/dev/null
echo "$n shader files" >> "$OUT/system.txt"
zip -qr "$OUT.zip" "$OUT"
chown "$(stat -f %Su /dev/console)" "$OUT.zip" 2>/dev/null
echo "Collected $n shader files."
echo "Send this file: $(pwd)/$OUT.zip"

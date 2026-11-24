#!/bin/bash
# IrisXe shader collector (v0.68.34+): saves the Metal shaders (AIR) of the apps you open next, so a shader that
# hangs the GPU can be compiled and fixed offline. Two steps:
#   sudo bash irisxe-airdump.sh start     then quit Terminal (Cmd+Q) and Claude, and open them again
#   sudo bash irisxe-airdump.sh           (in the new Terminal) collects the shaders into a zip and switches it off
cd "$(dirname "$0")" || exit 1
if [ "$(id -u)" != 0 ]; then echo "Run with sudo: sudo bash irisxe-airdump.sh"; exit 1; fi
U=$(stat -f %Su /dev/console)
UID_=$(id -u "$U")
if [ "$1" = "start" ]; then
  launchctl asuser "$UID_" launchctl setenv IRISXE_DUMP_AIR 1
  find /private/var/folders -maxdepth 4 -type d -name irisxe-air -exec rm -rf {} + 2>/dev/null
  find /Users/*/Library/Containers -maxdepth 4 -type d -name irisxe-air -exec rm -rf {} + 2>/dev/null
  echo "Shader saving is ON for apps opened from now on."
  echo "Now: quit Claude (Cmd+Q) and open it again, use it for 20 seconds."
  echo "Then quit Terminal (Cmd+Q), open it again, and run: sudo bash $(pwd)/irisxe-airdump.sh"
  exit 0
fi
launchctl asuser "$UID_" launchctl unsetenv IRISXE_DUMP_AIR
T=$(date +%Y%m%d-%H%M%S)
OUT="irisxe-air-$T"
mkdir -p "$OUT"
n=0
for d in $( { find /private/var/folders -maxdepth 4 -type d -name irisxe-air 2>/dev/null; find /Users/*/Library/Containers -maxdepth 4 -type d -name irisxe-air 2>/dev/null; } ); do
  owner=$(echo "$d" | md5 | cut -c1-6)
  mkdir -p "$OUT/$owner"
  cp "$d"/*.bc "$OUT/$owner/" 2>/dev/null && n=$((n+$(ls "$d"/*.bc 2>/dev/null | wc -l)))
  echo "$owner $d" >> "$OUT/folders.txt"
done
log show --last 15m --style compact --predicate 'eventMessage CONTAINS "IrisXeMTL: AIR saved" OR eventMessage CONTAINS "render pipeline ok" OR eventMessage CONTAINS "failed (-9)"' 2>/dev/null > "$OUT/log.txt"
ioreg -l -w0 | grep -E '"IrisXe-(Hang|HangHistory|HungDraws|Resets)"' | sed -E 's/^[ |]*//' > "$OUT/hangs.txt"
zip -qr "$OUT.zip" "$OUT"
chown "$U" "$OUT.zip" 2>/dev/null
echo "Shader saving is OFF again. Collected $n shader files."
echo "Send this file: $(pwd)/$OUT.zip"

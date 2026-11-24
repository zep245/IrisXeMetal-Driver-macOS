#!/bin/bash
# IrisXe boot log. Changes nothing: it only records.
# Run it on the boot AFTER a bad boot (black screen, freeze, restart): it saves what our driver, WindowServer and the
# kernel logged during the last hours (the earlier boots included), crash reports, and the driver's state now.
#   sudo bash irisxe-bootlog.sh
# Results: a folder irisxe-bootlog-<time> and a zip of it in the same folder as this script.
cd "$(dirname "$0")" || exit 1
if [ "$(id -u)" != 0 ]; then echo "Run with sudo: sudo bash irisxe-bootlog.sh"; exit 1; fi
OUT="irisxe-bootlog-$(date +%H%M%S)"
mkdir -p "$OUT"
H=4h
echo "== saving the logs of the last 4 hours (takes up to a minute)"
{
    echo "-- this boot"; sysctl -n kern.boottime; sw_vers
    echo "-- boot-args"; nvram boot-args 2>&1
    echo "-- our NVRAM settings"; nvram -p 2>/dev/null | grep -i irisxe
    echo "-- driver versions"; kextstat 2>/dev/null | grep -i irisxe
    echo "-- driver state"
    ioreg -l -w0 | grep -E '"IrisXe-(Version|GPUDesktop|GPUSafety|ProbeKeys|Video"|DisplayPower|BrightnessRestored)' | sed 's/^[ |]*//' | sort -u
    echo "-- lid"; pmset -g | grep -iE "lidwake|sleep|standby"; ioreg -l -w0 -r -c IOPMrootDomain -d 1 | grep -i clamshell | sed 's/^[ |]*//'
    echo "-- reboots"; last reboot 2>/dev/null | head -8; last shutdown 2>/dev/null | head -8
} > "$OUT/state.txt" 2>&1
log show --last $H --predicate 'sender CONTAINS "IrisXe" OR (process == "kernel" AND (eventMessage CONTAINS "IrisXe" OR eventMessage CONTAINS[c] "IOAccel" OR eventMessage CONTAINS[c] "GPU" OR eventMessage CONTAINS[c] "panic" OR eventMessage CONTAINS "BSD root" OR eventMessage CONTAINS "Darwin Kernel Version"))' 2>/dev/null \
    | tail -40000 > "$OUT/kernel.txt"
log show --last $H --predicate 'process == "WindowServer" AND (messageType == error OR messageType == fault OR eventMessage CONTAINS[c] "metal" OR eventMessage CONTAINS[c] "display" OR eventMessage CONTAINS[c] "timeout" OR eventMessage CONTAINS[c] "hang")' 2>/dev/null \
    | tail -15000 > "$OUT/windowserver.txt"
log show --last $H --predicate 'process == "loginwindow" OR process == "launchd" AND eventMessage CONTAINS[c] "WindowServer"' 2>/dev/null \
    | tail -3000 > "$OUT/login.txt"
log show --last $H --predicate 'eventMessage CONTAINS "IrisXeMTL" OR eventMessage CONTAINS "IrisXe Metal" OR subsystem CONTAINS "IrisXe"' 2>/dev/null \
    | tail -8000 > "$OUT/metal.txt"
pmset -g log 2>/dev/null | grep -E "Entering|Wake  |DarkWake|Clamshell|Display is turned|Start  |Shutdown" | tail -80 > "$OUT/power-log.txt"
mkdir -p "$OUT/reports"
find /Library/Logs/DiagnosticReports /Library/Logs/DiagnosticReports/Retired /var/root/Library/Logs/DiagnosticReports \
     -maxdepth 1 -type f -mtime -1 \( -name "*.panic" -o -name "*panic*" -o -name "WindowServer*" -o -name "*.hang" -o -name "*GPU*" \) \
     -exec cp {} "$OUT/reports/" \; 2>/dev/null
ls -la "$OUT/reports" > "$OUT/reports-list.txt"
zip -qr "$OUT.zip" "$OUT"
U=$(stat -f %Su /dev/console); chown -R "$U" "$OUT" "$OUT.zip" 2>/dev/null
echo
echo "== done: send me $(pwd)/$OUT.zip"

#!/bin/bash
# IrisXe lid test. Changes nothing: it only records.
# Why does opening the lid not wake the laptop? Saves the laptop's ACPI tables (as macOS loaded them, including the
# ones OpenCore adds), the lid device and macOS's lid/wake settings, so an OpenCore ACPI patch can be written.
#   sudo bash irisxe-lidtest.sh
# Results: a folder irisxe-lid-<time> and a zip of it in the same folder as this script.
cd "$(dirname "$0")" || exit 1
if [ "$(id -u)" != 0 ]; then echo "Run with sudo: sudo bash irisxe-lidtest.sh"; exit 1; fi
OUT="irisxe-lid-$(date +%H%M%S)"
mkdir -p "$OUT"
echo "== saving the ACPI tables"
ioreg -a -l -w0 -r -c AppleACPIPlatformExpert -d 1 > "$OUT/acpi-tables.plist" 2>&1
echo "== lid device and wake settings"
{
    echo "-- lid devices (PNP0C0D) in the device tree"
    ioreg -l -w0 -p IODeviceTree | grep -B2 -A25 'PNP0C0D'
    echo "-- clamshell / lid state"
    ioreg -l -w0 -r -c IOPMrootDomain -d 1 | grep -iE 'clamshell|lid|wake|sleep' | sed 's/^[ |]*//'
    echo "-- ACPI buttons / lid drivers"
    ioreg -l -w0 | grep -iE 'class (AppleACPIButton|AppleACPILid|AppleACPI.*Lid|AppleACPIEC)' -A12
} > "$OUT/lid-device.txt" 2>&1
{
    echo "-- pmset -g";      pmset -g
    echo "-- pmset -g cap";  pmset -g cap
    echo "-- pmset -g custom"; pmset -g custom
} > "$OUT/power-settings.txt" 2>&1
pmset -g log 2>/dev/null | grep -E "Entering (Sleep|DarkWake)|Wake  |DarkWake to|Wake Reason|Display is turned" | tail -80 > "$OUT/sleep-log.txt"
log show --last 1d --predicate 'process == "kernel" AND (eventMessage CONTAINS[c] "lid" OR eventMessage CONTAINS[c] "clamshell" OR eventMessage CONTAINS[c] "GPE" OR eventMessage CONTAINS[c] "wake reason")' 2>/dev/null \
    | tail -300 > "$OUT/kernel-lid.txt"
sw_vers > "$OUT/macos.txt"
ioreg -l -w0 -p IODeviceTree -d 2 | grep -E '"(model|board-id|OEMVendor|OEMBoard)"' >> "$OUT/macos.txt"
zip -qr "$OUT.zip" "$OUT"
U=$(stat -f %Su /dev/console); chown -R "$U" "$OUT" "$OUT.zip" 2>/dev/null
echo
echo "== done: send me $(pwd)/$OUT.zip"

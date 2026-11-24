#!/bin/bash
# IrisXe apps-on-the-GPU test (v0.68.30+).
# Sandboxed apps cannot read /Library/Preferences/irisxe-apps, so the lists go into NVRAM; the driver publishes them
# at boot (only when IrisXeCore runs) where every app can read them.
#   sudo bash irisxe-apptest.sh all        EVERY app on the GPU (except the "keep on CPU" list), then restart
#   sudo bash irisxe-apptest.sh prepare    only the first five test apps on the GPU, then restart
#   sudo bash irisxe-apptest.sh            after the restart: opens each test app, asks how it looks, records logs
#   sudo bash irisxe-apptest.sh clear      back to normal (only the wallpapers on the GPU), then restart
# Names with a space are written with "_" in the lists (System_Settings).
# Results: a folder irisxe-apps-<time> and a zip of it in the same folder as this script.
cd "$(dirname "$0")" || exit 1
if [ "$(id -u)" != 0 ]; then echo "Run with sudo: sudo bash irisxe-apptest.sh"; exit 1; fi
U=$(stat -f %Su /dev/console)
NOAPPS="AvatarPickerMemojiPicker"   # Preview and Photo Booth are kept on the CPU by the driver itself (Core Image)
TESTAPPS="Calculator
TextEdit
Maps
Photos
System Settings
Notes
Music
App Store
Mail
Calendar
Messages
Freeform
Books
Photo Booth
QuickTime Player
Chess
Activity Monitor"

case "$1" in
    all)
        nvram irisxe-apps="*" && nvram irisxe-noapps="$NOAPPS" || exit 1
        echo "== every app on the GPU, except: $NOAPPS (and Preview, Photo Booth: built in)"
        echo "   Restart, wait a minute after logging in, then run: sudo bash irisxe-apptest.sh"
        echo "   If anything goes wrong: sudo bash irisxe-apptest.sh clear   (or in Recovery: nvram -d irisxe-apps)"
        exit 0 ;;
    prepare)
        nvram irisxe-apps="Calculator TextEdit Maps Photos" && nvram irisxe-noapps="$NOAPPS" || exit 1
        echo "== test apps on the GPU: Calculator TextEdit Maps Photos. Restart, then run: sudo bash irisxe-apptest.sh"
        exit 0 ;;
    clear)
        nvram -d irisxe-apps; nvram -d irisxe-noapps
        echo "== back to normal (only the wallpapers on the GPU). Restart to use it."
        exit 0 ;;
esac

OUT="irisxe-apps-$(date +%H%M%S)"
mkdir -p "$OUT"
{
    echo "-- NVRAM"; nvram irisxe-apps 2>&1; nvram irisxe-noapps 2>&1
    echo "-- published by the driver"
    ioreg -l -w0 -r -c IrisXeAccelerator -d 1 | grep -E '"IrisXe-(Apps|NoApps|Apps-Off|Version|GPUDesktop)"' | sed 's/^[ |]*//'
} > "$OUT/setup.txt"
cat "$OUT/setup.txt"
if ! ioreg -l -w0 -r -c IrisXeAccelerator -d 1 | grep -q '"IrisXe-Apps"'; then
    echo
    echo "The driver did not publish an app list. Run 'sudo bash irisxe-apptest.sh all', restart, and run this again."
    echo "(Needs driver v0.68.30 or newer.)"
    exit 1
fi

answers="$OUT/answers.txt"
IFS=$'\n'
for app in $TESTAPPS; do
    if [ -n "$1" ] && ! printf '%s\n' "$@" | grep -qx "$app"; then continue; fi
    echo
    echo "== $app"
    sudo -u "$U" osascript -e "tell application \"$app\" to quit" >/dev/null 2>&1
    sleep 2; pkill -x "$app" 2>/dev/null; sleep 1
    T0=$(date "+%Y-%m-%d %H:%M:%S")
    sudo -u "$U" open -a "$app" 2>/dev/null || { echo "   not installed, skipped"; echo "$app: not installed" >> "$answers"; continue; }
    echo "   waiting 15 seconds while it starts and draws..."
    sleep 15
    running=no; pgrep -x "$app" >/dev/null && running=yes
    echo "   running: $running"
    echo "   Use it a little (scroll, click around, open a window), then answer."
    read -r -p "   Did $app look and work normally? (yes / no / describe what was wrong) " a
    gpu=no
    log show --start "$T0" --predicate "process == \"$app\" AND eventMessage CONTAINS \"IrisXeMTL: enabled in\"" 2>/dev/null | grep -q "enabled in" && gpu=yes
    echo "$app: running=$running gpu=$gpu answer=$a" >> "$answers"
    {
        echo "== $app since $T0"
        echo "-- our Metal driver in $app"
        log show --start "$T0" --predicate "process == \"$app\" AND (eventMessage CONTAINS \"IrisXeMTL\" OR messageType == fault)" 2>/dev/null \
            | grep -vE "newBuffer length|texture [0-9]+x" | tail -400
        echo "-- kernel: GPU problems"
        log show --start "$T0" --predicate 'sender CONTAINS "IrisXeAccel" AND NOT eventMessage CONTAINS "display pipe" AND (eventMessage CONTAINS[c] "skipped" OR eventMessage CONTAINS[c] "timeout" OR eventMessage CONTAINS[c] "timed out" OR eventMessage CONTAINS[c] "reset" OR eventMessage CONTAINS[c] "hang" OR eventMessage CONTAINS[c] "fault")' 2>/dev/null | tail -200
    } > "$OUT/$app-log.txt"
    find /Library/Logs/DiagnosticReports "/Users/$U/Library/Logs/DiagnosticReports" -maxdepth 1 -type f -newermt "$T0" \
         -exec cp {} "$OUT/" \; 2>/dev/null
    sudo -u "$U" osascript -e "tell application \"$app\" to quit" >/dev/null 2>&1
    sleep 2; pkill -x "$app" 2>/dev/null
done
unset IFS

# crashes anywhere since this boot (other apps and background processes)
mkdir -p "$OUT/all-crashes-this-boot"
BOOT=$(sysctl -n kern.boottime | sed -E 's/.*sec = ([0-9]+).*/\1/')
BT=$(date -r "$BOOT" "+%Y-%m-%d %H:%M:%S")
find /Library/Logs/DiagnosticReports "/Users/$U/Library/Logs/DiagnosticReports" -maxdepth 1 -type f -newermt "$BT" \
     -exec cp {} "$OUT/all-crashes-this-boot/" \; 2>/dev/null
ls "$OUT/all-crashes-this-boot" > "$OUT/crash-list.txt"
log show --start "$BT" --predicate 'eventMessage CONTAINS "IrisXeMTL: enabled in"' 2>/dev/null \
    | sed -E 's/.*enabled in ([^(]*) \(.*/\1/' | sort | uniq -c | sort -rn > "$OUT/programs-on-gpu.txt"
sw_vers > "$OUT/macos.txt"; kextstat 2>/dev/null | grep -i irisxe >> "$OUT/macos.txt"
zip -qr "$OUT.zip" "$OUT"
chown -R "$U" "$OUT" "$OUT.zip" 2>/dev/null
echo
echo "== done: send me $(pwd)/$OUT.zip"
echo "   To go back to normal: sudo bash irisxe-apptest.sh clear"

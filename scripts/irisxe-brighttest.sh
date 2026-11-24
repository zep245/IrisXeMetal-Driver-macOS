#!/bin/bash
# IrisXe v0.68.37 brightness + sleep test. Changes nothing: it only records.
# Finds out why the brightness slider always shows full, whether the brightness is remembered, and whether sleep and
# wake (including "dark wake": awake with the screen off) work.
#   sudo bash irisxe-brighttest.sh          the main test (slider, then sleep and wake), about 3 minutes
#   sudo bash irisxe-brighttest.sh boot     run right after a restart: is the brightness restored at boot?
#   sudo bash irisxe-brighttest.sh mode 2   brightness "probe" mode for the next restarts (0 = off, as v0.68.17;
#                                           1 = as v0.68.18; 2 = both probe keys; 0 is the default)
# Results: a folder irisxe-bright-<time> and a zip of it in the same folder as this script.
cd "$(dirname "$0")" || exit 1
if [ "$(id -u)" != 0 ]; then echo "Run with sudo: sudo bash irisxe-brighttest.sh"; exit 1; fi
if [ "$1" = "mode" ]; then
    case "$2" in
        0) nvram -d irisxe-probekeys; echo "== probe mode 0 (the default). Restart to use it." ;;
        1|2) nvram irisxe-probekeys="$2"; echo "== probe mode $2 saved. Restart to use it." ;;
        *) echo "Use: sudo bash irisxe-brighttest.sh mode 0   (or 1 or 2)" ;;
    esac
    exit 0
fi
OUT="irisxe-bright-$(date +%H%M%S)"
mkdir -p "$OUT"
NOTES="$OUT/answers.txt"
START=$(date "+%Y-%m-%d %H:%M:%S")

# one snapshot: our driver's brightness notes, what macOS has stored for the display, the saved value, power state
snap() {
    local f="$OUT/snap-$1.txt"
    {
        echo "== $1 at $(date "+%H:%M:%S")"
        echo "-- IrisXeFB brightness properties"
        ioreg -l -w0 -r -c IrisXeFB | grep -E '"IrisXe-(Version|Bright|Backlight|LastBrightness|LastProbe|ProbeKeys|WakeBacklight|WakeCheck|DisplayPower|Restore|LinkTrain)' \
            | sed 's/^[ |]*//'
        echo "-- display parameters macOS reads (brightness / linear-brightness / dsyp)"
        ioreg -l -w0 -r -c IODisplayConnect | grep -E '"IODisplayParameters"|"IODisplayGUID"' | sed 's/^[ |]*//'
        ioreg -l -w0 -r -c AppleBacklightDisplay | grep -E 'IODisplayParameters|brightness|Brightness' | sed 's/^[ |]*//' | head -20
        echo "-- saved brightness (NVRAM irisxe-brightness)"
        nvram irisxe-brightness 2>&1
        echo "-- probe mode (NVRAM irisxe-probekeys; none = 0)"
        nvram irisxe-probekeys 2>&1
        echo "-- CoreBrightness / display settings"
        defaults read /var/root/Library/Preferences/com.apple.CoreBrightness.plist 2>/dev/null | head -60
        echo "-- power state"
        pmset -g | head -40
    } > "$f" 2>&1
}
ask() { local a; read -r -p "$1 " a; echo "$1 -> $a" >> "$NOTES"; }

if [ "$1" = "boot" ]; then
    echo "== boot check (run this within a few minutes after logging in)"
    snap boot
    log show --last boot --predicate 'sender CONTAINS "IrisXe" AND eventMessage CONTAINS[c] "bright"' > "$OUT/kernel-boot.txt" 2>&1
    log show --last boot --predicate 'process == "WindowServer" OR subsystem CONTAINS "CoreBrightness" OR subsystem CONTAINS "CoreDisplay"' 2>/dev/null \
        | grep -iE "bright|backlight|nits|lux" | head -400 > "$OUT/brightness-boot.txt"
    echo
    ask "Before the restart, was the screen dimmer than full? (yes/no)"
    ask "After the restart, is the screen brightness the same as before the restart? (yes/no)"
    ask "Open Control Centre (top right): where is the Display slider? (full / about half / other)"
else
    echo "== step 1 of 3: now"
    snap 1-start
    echo
    echo "Press the brightness-down key (or move the slider in Control Centre) until the screen is about HALF bright."
    ask "Done? Then open Control Centre (top right): where is the Display slider? (full / about half / other)"
    snap 2-half
    echo
    echo "== step 2 of 3: sleep"
    echo "Close the lid (or Apple menu > Sleep). Wait 1 minute. Open the lid, log in, then come back here."
    read -r -p "Press Return to start; then put the laptop to sleep... " _
    SLEPT=$(date "+%Y-%m-%d %H:%M:%S")
    read -r -p "Back from sleep? Press Return... " _
    echo
    echo "== step 3 of 3: after wake"
    ask "Did the screen come back on by itself after opening the lid? (yes/no)"
    ask "Is the brightness the same as before sleep (about half)? (yes/no)"
    ask "Where is the Display slider now? (full / about half / other)"
    ask "Did anything look wrong (black screen, flash, wrong colours, slow)? (no / describe)"
    snap 3-after-wake
    echo "== collecting"
    pmset -g log 2>/dev/null | grep -E "Sleep  |Wake  |DarkWake|Wake Requests|Display is turned" | tail -60 > "$OUT/sleep-log.txt"
    pmset -g log 2>/dev/null | awk -v s="$SLEPT" '$0 >= s' | head -300 > "$OUT/sleep-log-since-test.txt"
    pmset -g assertions > "$OUT/assertions.txt" 2>&1
    log show --start "$START" --predicate 'sender CONTAINS "IrisXe"' > "$OUT/kernel-irisxe.txt" 2>&1
    log show --start "$START" --predicate 'subsystem CONTAINS "CoreBrightness" OR subsystem CONTAINS "CoreDisplay" OR process == "WindowServer" OR process == "ControlCenter"' 2>/dev/null \
        | grep -iE "bright|backlight|nits|slider|wake|sleep|display" | head -3000 > "$OUT/brightness-log.txt"
fi

sw_vers > "$OUT/macos.txt"
kextstat 2>/dev/null | grep -i irisxe >> "$OUT/macos.txt"
{
    echo "== slider log from our driver (b=brightness l=linear-brightness, value@displaypower; 2=on 1=dim 0=off)"
    grep -h '"IrisXe-BrightLog"' "$OUT"/snap-*.txt | tail -1
    echo "== answers"; cat "$NOTES" 2>/dev/null
    echo "== saved brightness per snapshot"; grep -h -A1 "NVRAM" "$OUT"/snap-*.txt | grep irisxe
    echo "== sleeps (incl. dark wake) / wakes since the test started"; grep -cE "Entering (Sleep|DarkWake)" "$OUT/sleep-log-since-test.txt" 2>/dev/null; grep -cE "^20.*Wake  *(from|DarkWake)" "$OUT/sleep-log-since-test.txt" 2>/dev/null
} > "$OUT/summary.txt"
zip -qr "$OUT.zip" "$OUT"
U=$(stat -f %Su /dev/console); chown -R "$U" "$OUT" "$OUT.zip" 2>/dev/null
echo
echo "== done: send me $(pwd)/$OUT.zip"

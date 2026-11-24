#!/bin/bash
# IrisXe v0.62.2 white-line test: which shader compiler speed-up draws the white lines around windows?
# Run it from the unzipped release folder, with the GPU-drawn desktop on (after irisxe-fulltest.sh):
#     sudo bash irisxe-linetest.sh 1      only speed-up 1 on, the others off; WindowServer restarts
#     sudo bash irisxe-linetest.sh 2      ... up to 6
#     sudo bash irisxe-linetest.sh 0      all speed-ups off (the white lines should be gone)
#     sudo bash irisxe-linetest.sh all    all speed-ups on again (removes the switch files)
# After each step: wait until the desktop is back, open a few windows (Finder, Safari, Settings), look at their
# corners and edges, and write down whether the white lines are there.
# The switches are words in /Library/Preferences/irisxe-compiler (see IrisXeMTLDriver.m, xeCompilerSwitches).
STEP="$1"
if [ "$(id -u)" != "0" ]; then echo "Run it with sudo: sudo bash irisxe-linetest.sh 1"; exit 1; fi
ALL="nosb nogroup nohoist nopack nounroll noblock"
NAMES=("" "reads overlapped (nosb)" "samples grouped (nogroup)" "constants read once at the start (nohoist)" \
       "half vectors in one read (nopack)" "short loops unrolled (nounroll)" "constants read in blocks (noblock)")
rm -f /Library/Preferences/irisxe-overlap
case "$STEP" in
    all)
        rm -f /Library/Preferences/irisxe-compiler
        echo "All speed-ups on again." ;;
    0)
        echo "$ALL" > /Library/Preferences/irisxe-compiler
        echo "Step 0: all speed-ups off. The white lines should be gone." ;;
    [1-6])
        W=($ALL)
        KEEP="${W[$((STEP - 1))]}"
        echo "$ALL" | tr ' ' '\n' | grep -vx "$KEEP" | tr '\n' ' ' > /Library/Preferences/irisxe-compiler
        echo "Step $STEP: only this speed-up is on: ${NAMES[$STEP]}" ;;
    *)
        echo "Usage: sudo bash irisxe-linetest.sh 1   (steps 1 to 6, then 0, then all)"; exit 1 ;;
esac
echo "Switch file now: $(cat /Library/Preferences/irisxe-compiler 2>/dev/null || echo '(none)')"
if ! ioreg -l -w0 | grep '"IrisXe-DisplayPipe"' | head -1 | grep -q "showing images"; then
    echo "NOTE: the GPU-drawn desktop is not on. Run sudo bash irisxe-fulltest.sh first, then this again."
    exit 1
fi
echo "The screen resets in 5 seconds (WindowServer restarts; the GPU-drawn desktop stays on)."
echo "Then open a few windows and look at their corners and edges for white lines."
sleep 5
killall -9 WindowServer

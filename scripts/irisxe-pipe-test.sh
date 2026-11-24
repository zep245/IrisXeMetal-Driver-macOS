#!/bin/bash
# IrisXe v0.57.0 display pipe test, for a boot with the framebuffer named IrisXeIntelFB
# (sudo nvram irisxe-intelfb=1 + restart). Run: sudo bash irisxe-pipe-test.sh
# v0.56.7: sudo bash irisxe-pipe-test.sh batch  - WindowServer's GPU draws go up to 16 per GPU job during the test
#          (faster; it hung the GPU in v0.51.37, so the GPU hang details are saved too)
# v0.56.9: sudo bash irisxe-pipe-test.sh batchtest  - batched as above, in two parts (irisxe-mtkdemo --batch N, from
#          the same folder as this script): 5-30 s one draw per GPU job but in the batch "slot" pages, 30-55 s two draws
#          per job. The hangs of each part tell whether the slot pages or two draws in one job cause them.
# v0.56.10: sudo bash irisxe-pipe-test.sh batchtest A B  - part 1 with up to A draws per job, part 2 with up to B
#          (1..8; default 1 and 2). v0.56.9 test: 1 and 2 never hung, 8 hangs about once a second.
# v0.56.10 tests: 4 per job never hung, 6 hung within seconds. So "batch" now means up to 4 draws per job
#          (sudo bash irisxe-pipe-test.sh batch N for another number).
# v0.57.0: WindowServer's draws are batched by default on the test boot (IrisXeCore: up to 4 per GPU job);
#          sudo bash irisxe-pipe-test.sh nobatch = one draw per GPU job as before
#
# What it does:
#   1. makes the AGDC and the display pipe (IrisXeDisplayPipe in v0.56.0) and makes the pipe "active"
#      (v0.56.3). If the pipe is not ready, it undoes this at once and stops: no WindowServer restart, no lag.
#   2. restarts WindowServer, so CoreDisplay starts again WITH IOPresentment and the display pipe
#   3. after 40 s saves the pipe status and a 5 s WindowServer sample
#   4. after 60 s: if ~/Downloads/pipe-test/keep does NOT exist, removes the display pipe and the AGDC again and
#      restarts WindowServer (back to how it was). If the screen works and you want to keep it, create that file
#      before then:  touch ~/Downloads/pipe-test/keep
# Results go to ~/Downloads/pipe-test/. The logs are read afterwards by time (bash ~/Downloads/pipe-test/9-read-logs.txt): a script left
# running after WindowServer restarts cannot reach the log service.

U="${SUDO_USER:-prasad}"
OUT="/Users/$U/Downloads/pipe-test"
if [ "$(id -u)" != "0" ]; then echo "Run it with sudo: sudo bash irisxe-pipe-test.sh"; exit 1; fi
if ! ioreg -c IrisXeIntelFB | grep -q "<class IrisXeIntelFB"; then
    echo "This is not the test boot (no IrisXeIntelFB). Run: sudo nvram irisxe-intelfb=1 , restart, then run this again."
    exit 1
fi
rm -rf "$OUT"; mkdir -p "$OUT"; chown "$U" "$OUT"
rm -f /Library/Preferences/irisxe-ws-mode
if [ "$1" = "batch" ] || [ "$1" = "batchtest" ]; then echo "batch" > /Library/Preferences/irisxe-ws-mode; echo "Test mode: batched GPU draws"; fi
if [ "$1" = "nobatch" ]; then echo "nobatch" > /Library/Preferences/irisxe-ws-mode; echo "Test mode: one GPU job per draw"; fi
MTK="$(cd "$(dirname "$0")" && pwd)/irisxe-mtkdemo"
PHASES=0
if [ "$1" = "batch" ]; then
    if [ ! -x "$MTK" ]; then echo "batch needs irisxe-mtkdemo next to this script ($MTK). Run: cp irisxe-mtkdemo ~/Downloads/"; rm -f /Library/Preferences/irisxe-ws-mode; exit 1; fi
    "$MTK" --batch "${2:-4}" > "$OUT/0-batch-phases.txt" 2>&1
    echo "$(date "+%H:%M:%S") batch: up to ${2:-4} draws per job" >> "$OUT/0-batch-phases.txt"
fi
if [ "$1" = "batchtest" ]; then
    if [ ! -x "$MTK" ]; then echo "batchtest needs irisxe-mtkdemo next to this script ($MTK). Run: cp irisxe-mtkdemo ~/Downloads/"; rm -f /Library/Preferences/irisxe-ws-mode; exit 1; fi
    PHASES=1
    P1="${2:-1}"; P2="${3:-2}"
    "$MTK" --batch "$P1" > "$OUT/0-batch-phases.txt" 2>&1
    echo "$(date "+%H:%M:%S") part 1: up to $P1 draws per job" >> "$OUT/0-batch-phases.txt"
fi
START=$(date -v-5S "+%Y-%m-%d %H:%M:%S")   # v0.56.1: before the setup, so the logs include the pipe being made
# v0.56.2: CoreDisplay reports why a transaction fails only at Info/Debug level, which macOS does not keep by default.
# Keep those levels for CoreDisplay and IOPresentment during the test (put back to default at the end).
log config --subsystem com.apple.CoreDisplay --mode "level:debug,persist:debug" 2>/dev/null
log config --subsystem com.apple.IOPresentment --mode "level:debug,persist:debug" 2>/dev/null
{
    echo "== $(date)"
    kmutil load -b com.irisxe.IrisXeAGDC 2>&1
    sysctl debug.irisxe_agdc=1 2>&1
    sleep 2
    echo "agdc status: $(sysctl -n debug.irisxe_agdc_status 2>&1)"
    sysctl debug.irisxe_dpipe=2 2>&1
    sleep 2
    ioreg -l -w0 | grep -E '"IrisXe-DisplayPipe"|"IrisXe-IntelName"|"IrisXe-AccelVersion"' 2>&1
} > "$OUT/1-setup.txt" 2>&1
cat "$OUT/1-setup.txt"
# v0.56.3: WindowServer only gets the pipe's transactions accepted when all three checks pass
if ! grep -q "ready: active 1, accel-on 1, not-busy 1" "$OUT/1-setup.txt"; then
    sysctl debug.irisxe_dpipe=4 >> "$OUT/1-setup.txt" 2>&1
    sysctl debug.irisxe_agdc=0 >> "$OUT/1-setup.txt" 2>&1
    rm -f /Library/Preferences/irisxe-ws-mode
    [ "$PHASES" = "1" ] && "$MTK" --batch 8 >> "$OUT/0-batch-phases.txt" 2>&1
    log config --subsystem com.apple.CoreDisplay --reset 2>/dev/null
    log config --subsystem com.apple.IOPresentment --reset 2>/dev/null
    chown -R "$U" "$OUT"
    echo
    echo "The display pipe is NOT ready (see the ready: numbers above, 0 = check failed). Nothing was restarted."
    echo "Send ~/Downloads/pipe-test/1-setup.txt"
    exit 0
fi
echo
echo "The display pipe is ready (all three checks pass)."
cat > "$OUT/9-read-logs.txt" <<EOF
# After the test run: bash ~/Downloads/pipe-test/9-read-logs.txt  - then send the four pipe12-*.txt files in Downloads
log show --start "$START" --end "$(date -v+3M "+%Y-%m-%d %H:%M:%S")" --info --debug --predicate 'process == "WindowServer" AND NOT (subsystem == "com.irisxe.IrisXeMTLDriver")' | grep -v "Unsupported sw composite" > ~/Downloads/pipe12-ws-log.txt
log show --start "$START" --end "$(date -v+3M "+%Y-%m-%d %H:%M:%S")" --predicate 'process == "WindowServer" AND subsystem == "com.irisxe.IrisXeMTLDriver"' | grep -vE "newBuffer|texture [0-9]|IOSurface [0-9]" > ~/Downloads/pipe12-ws-metal.txt
log show --start "$START" --end "$(date -v+3M "+%Y-%m-%d %H:%M:%S")" --predicate 'process == "kernel" AND (sender == "IrisXeAccel" OR sender == "IrisXeFB" OR sender == "IrisXeAGDC")' | grep -v "mapped GPU" > ~/Downloads/pipe12-kernel.txt
log show --start "$START" --end "$(date -v+3M "+%Y-%m-%d %H:%M:%S")" --predicate 'process == "kernel" AND (sender == "IOAcceleratorFamily2" OR sender == "IOSurface")' | grep -v "display_change_handler" > ~/Downloads/pipe12-family.txt
EOF
chown "$U" "$OUT"/*
echo
echo "WindowServer restarts in 5 seconds. After 60 s it is put back as it was, unless you run:"
echo "    touch ~/Downloads/pipe-test/keep"
echo "If the screen freezes or goes black, wait: it comes back by itself after about a minute."

nohup bash -c '
OUT="'"$OUT"'"; U="'"$U"'"; MTK="'"$MTK"'"; PHASES="'"$PHASES"'"; P2="'"$P2"'"
sleep 5
date > "$OUT/2-restart-time.txt"
killall -9 WindowServer
sleep 20
ioreg -l -w0 | grep -E "\"IrisXe-(DisplayPipe|DrawPerf|DrawSlowest|AccelBatch|GPU|GPUFreq|Hang|HangHistory|HangMaps|BatchTrim)\"" > "$OUT/3-pipe-status.txt" 2>&1
# v0.56.11: which processes use the CPU while you open Launchpad (it is drawn by the Dock process) - 20-32 s
top -l 4 -s 3 -o cpu -n 15 -stats pid,command,cpu,time > "$OUT/6-top.txt" 2>&1 &
sample Dock 5 -file "$OUT/4-dock-sample.txt" > /dev/null 2>&1 &
sample WindowServer 5 -file "$OUT/4-ws-sample.txt" > /dev/null 2>&1   # v0.56.7: 20-25 s, while you move things
if [ "$PHASES" = "1" ]; then                                              # v0.56.9: part 2 from 30 s
    "$MTK" --batch "$P2" >> "$OUT/0-batch-phases.txt" 2>&1
    echo "$(date "+%H:%M:%S") part 2: up to $P2 draws per job" >> "$OUT/0-batch-phases.txt"
fi
sleep 15
ioreg -l -w0 | grep -E "\"IrisXe-(DisplayPipe|DrawPerf|DrawSlowest|AccelBatch|GPU|GPUFreq|Hang|HangHistory|HangMaps|BatchTrim)\"" >> "$OUT/3-pipe-status.txt" 2>&1
ioreg -l -w0 | grep -E "\"IrisXe-HangJob\"" > "$OUT/3-hang-job.txt" 2>&1
sleep 10
if [ -e "$OUT/keep" ]; then
    echo "kept (keep file found)" > "$OUT/5-undo.txt"
else
    sysctl debug.irisxe_dpipe=4 > "$OUT/5-undo.txt" 2>&1
    sysctl debug.irisxe_agdc=0 >> "$OUT/5-undo.txt" 2>&1
    killall -9 WindowServer
fi
rm -f /Library/Preferences/irisxe-ws-mode
if [ "$PHASES" = "1" ]; then "$MTK" --batch 8 >> "$OUT/0-batch-phases.txt" 2>&1; echo "$(date "+%H:%M:%S") end: back to 8" >> "$OUT/0-batch-phases.txt"; fi
log config --subsystem com.apple.CoreDisplay --reset 2>/dev/null
log config --subsystem com.apple.IOPresentment --reset 2>/dev/null
chown -R "$U" "$OUT"
' > /dev/null 2>&1 &
exit 0

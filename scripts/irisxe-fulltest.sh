#!/bin/bash
# IrisXe v0.63.2 full test: one command, one folder of results.
# Run it from the unzipped release folder, after the restart that follows installing:
#     sudo bash irisxe-fulltest.sh            the whole test (about 6 minutes)
#     sudo bash irisxe-fulltest.sh manual     the whole test, but you use the desktop during the recording (v0.63.1)
#     sudo bash irisxe-fulltest.sh record     only the 3-minute desktop recording: no speed tests, no screen reset
# The whole test:
#   1. checks that the Mac is calm (top) and records versions / driver status
#   2. the speed test (--fillrate), the copy test (--blittest), the pixel test (--pixelrate) and, v0.58.7, the
#      big memory test (--bigalloc: 1.25 GB of GPU buffers in one program; the old limit was 512 MB)
#   3. on the test boot only (sudo nvram irisxe-intelfb=1 + restart): turns the GPU-drawn desktop on and keeps it
#      (irisxe-pipe-test.sh + the keep file) - v0.58.3: skipped when it is already on (no screen reset then)
#   4. records 3 minutes, once a second: frames shown, GPU busy, GPU clock, WindowServer CPU, load; and IrisXeCore's
#      GPU time by kind of drawing (IrisXe-DrawMix, started from zero) - while you use the desktop
#      v0.59.1: in three 60 s phases - v0.63.2: as before, smart flushing, async - each with the
#      stutter meter (IrisXe-FrameTiming) from zero: 4-timing-1-before.txt, 4-timing-2-flush.txt, 4-timing-3-all.txt.
#      Before the tests it waits (up to 5 minutes) until the Mac is calm.
#      v0.58.3: the recording runs under launchd, so a WindowServer restart no longer stops it
#   5. then run collect.sh (bash ~/Downloads/irisxe-results/collect.sh): it waits until the recording has finished,
#      reads the logs and packs ~/Downloads/irisxe-results.zip
MODE="${1:-full}"
# v0.63.1: the test opens and closes Mission Control and Launchpad itself (the same in every phase); "manual" = you do it
AUTO=1; if [ "$MODE" = "manual" ]; then AUTO=0; MODE=full; fi
U="${SUDO_USER:-prasad}"
OUT="/Users/$U/Downloads/irisxe-results"
HERE="$(cd "$(dirname "$0")" && pwd)"
if [ "$(id -u)" != "0" ]; then echo "Run it with sudo: sudo bash irisxe-fulltest.sh"; exit 1; fi
if [ ! -x "$HERE/irisxe-metaltest" ]; then echo "Run it from the unzipped release folder (irisxe-metaltest must be next to it)."; exit 1; fi
launchctl bootout system/com.irisxe.timeline 2>/dev/null
rm -rf "$OUT" "/Users/$U/Downloads/irisxe-results.zip"; mkdir -p "$OUT"; chown "$U" "$OUT"
START=$(date -v-5S "+%Y-%m-%d %H:%M:%S")
echo "$START" > "$OUT/0-start.txt"
echo "$MODE" > "$OUT/0-mode.txt"

say() { echo "$(date "+%H:%M:%S") $*"; echo "$(date "+%H:%M:%S") $*" >> "$OUT/0-steps.txt"; }

say "1/4 is the Mac calm? (top, versions, driver status)"
top -l 2 -s 2 -o cpu -n 12 -stats pid,command,cpu | tail -14 > "$OUT/1-top-before.txt" 2>&1
sysctl -n vm.loadavg > "$OUT/1-load.txt"
{
    sw_vers; echo
    kextstat 2>/dev/null | grep -i irisxe
    echo; nvram irisxe-intelfb 2>&1
    echo; ioreg -l -w0 | grep -E '"IrisXe-(AccelVersion|Version|GPU|GPUFreq|Hash|PAT|MOCS|DrawPerf|DisplayPipe|IntelName|GPUSpace|GPUVA|Scanout|FrameTiming|KernelTiming|DrawPhases)"'
    echo; ls -la /Library/Preferences/irisxe-* 2>/dev/null
    echo; echo "WindowServer started: $(ps -o lstart= -p "$(pgrep -x WindowServer)")"
} > "$OUT/1-status.txt" 2>&1
LOAD=$(sysctl -n vm.loadavg | awk '{print int($2)}')
[ "$LOAD" -gt 8 ] && say "   NOTE: the Mac is busy (load $LOAD, see 1-top-before.txt) - results will be slower than they should be"
if ! kextstat 2>/dev/null | grep -q com.irisxe.IrisXeCore; then
    say "   ERROR: IrisXeCore is not loaded - the GPU can't be tested. Check the EFI copy (install-core.sh) and restart."
fi

# v0.59.1: wait until macOS has finished its start-up work (indexing etc.): a busy Mac lags in any mode
W=0
while [ "$(sysctl -n vm.loadavg | awk '{print int($2)}')" -ge 6 ] && [ $W -lt 300 ]; do
    [ $((W % 30)) -eq 0 ] && say "   waiting for the Mac to calm down (load $(sysctl -n vm.loadavg | awk '{print $2}'), up to 5 minutes) - don't use it now"
    sleep 5; W=$((W + 5))
done
[ $W -gt 0 ] && say "   load now $(sysctl -n vm.loadavg | awk '{print $2}') after $W s"

if [ "$MODE" != "record" ]; then
    say "2/4 speed test, copy test, pixel test (about 2 minutes)"
    "$HERE/irisxe-metaltest" --fillrate > "$OUT/2-fillrate.txt" 2>&1
    "$HERE/irisxe-metaltest" --blittest > "$OUT/2-blittest.txt" 2>&1
    "$HERE/irisxe-metaltest" --pixelrate > "$OUT/2-pixelrate.txt" 2>&1
    "$HERE/irisxe-metaltest" --bigalloc > "$OUT/2-bigalloc.txt" 2>&1
    # v0.62.0: the shader compiler changed again (short loops unrolled, constants read in blocks) - these tests check it on the GPU
    for m in --shaders --textures --texmore --metaldraw --types --compute --mpsblur; do
        echo "=== $m" >> "$OUT/2-shadertests.txt"
        "$HERE/irisxe-metaltest" $m >> "$OUT/2-shadertests.txt" 2>&1
    done
    grep -c -E "WRONG|FAIL" "$OUT/2-shadertests.txt" | sed 's/^/   shader test lines WRONG or FAILED: /' | tee -a "$OUT/0-steps.txt"
    grep -c "WRONG" "$OUT/2-blittest.txt" | sed 's/^/   copy test lines WRONG: /' | tee -a "$OUT/0-steps.txt"
    grep "bigalloc test:" "$OUT/2-bigalloc.txt" | sed 's/^ */   /' | tee -a "$OUT/0-steps.txt"
else
    say "2/4 skipped (record mode)"
fi

# collect.sh: run by you after the recording (it waits for it)
cat > "$OUT/collect.sh" <<EOF
#!/bin/bash
# reads the logs from the test and packs ~/Downloads/irisxe-results.zip
O="$OUT"
if [ -e "\$O/4-recording" ] && [ ! -e "\$O/4-done" ]; then
    echo "The recording is still running (it takes 3 minutes; a notification says when it is finished). Run this again then."
    exit 1
fi
S="\$(cat "\$O/0-start.txt")"
E="\$(date "+%Y-%m-%d %H:%M:%S")"
log show --start "\$S" --end "\$E" --predicate 'process == "WindowServer" AND subsystem == "com.irisxe.IrisXeMTLDriver"' | grep -vE "newBuffer|texture [0-9]|IOSurface [0-9]" > "\$O/5-ws-metal.txt" 2>&1
log show --start "\$S" --end "\$E" --predicate 'process == "kernel" AND (sender == "IrisXeAccel" OR sender == "IrisXeFB" OR sender == "IrisXeAGDC" OR sender == "IrisXeCore")' | grep -vE "mapped GPU|skipped: not in|queue (processCommandBuffer|parseSegmentList|processSegment|segmentKernelCommand)" > "\$O/5-kernel.txt" 2>&1
log show --start "\$S" --end "\$E" --predicate 'process == "WindowServer" AND messageType == error' | tail -400 > "\$O/5-ws-errors.txt" 2>&1
echo "WindowServer started: \$(ps -o lstart= -p "\$(pgrep -x WindowServer)")" >> "\$O/5-ws-errors.txt"
# v0.60.2: the AIR of WindowServer's big shaders (for compiler work) and of shaders our compiler refused
mkdir -p "\$O/shaders"
cp /var/tmp/irisxe-air-big/*.bc "\$O/shaders/" 2>/dev/null
cp /var/tmp/irisxe-fail/*.bc "\$O/shaders/" 2>/dev/null
ls "\$O/shaders" | wc -l | sed 's/^ */shaders saved: /' >> "\$O/5-ws-errors.txt"
# v0.58.7: WindowServer crash reports from today (a screen reset = WindowServer crashed and restarted)
mkdir -p "\$O/crashes"
find /Library/Logs/DiagnosticReports "\$HOME/Library/Logs/DiagnosticReports" -maxdepth 1 -name 'WindowServer*' -mtime -1 -exec cp {} "\$O/crashes/" \; 2>/dev/null
ls "\$O/crashes" | sed 's/^/crash report: /' >> "\$O/5-ws-errors.txt"
[ -d "\$HOME/Downloads/pipe-test" ] && cp -R "\$HOME/Downloads/pipe-test" "\$O/" 2>/dev/null
cd "\$HOME/Downloads" && rm -f irisxe-results.zip && zip -qr irisxe-results.zip irisxe-results
echo "Done: send ~/Downloads/irisxe-results.zip"
EOF
chmod +x "$OUT/collect.sh"; chown -R "$U" "$OUT"

DP=$(ioreg -l -w0 | grep '"IrisXe-DisplayPipe"' | head -1)
GD=$(ioreg -l -w0 | grep -o '"IrisXe-GPUDesktop" = "[^"]*"' | head -1)    # v0.64.0: the boot hand-over's report
echo "$GD" > "$OUT/3-gpu-desktop-boot.txt"
WAIT=3
if echo "$DP" | grep -q "showing images" || echo "$GD" | grep -q "step 4 done"; then
    say "3/4 the GPU-drawn desktop is already on: no screen reset"
elif [ "$MODE" = "record" ]; then
    say "3/4 NOTE: the GPU-drawn desktop is not on - this records the normal desktop"
elif ! ioreg -c IrisXeIntelFB | grep -q "<class IrisXeIntelFB"; then
    say "3/4 skipped: no IrisXeIntelFB this boot ($GD) - the GPU desktop starts at the next boot"
else
    say "3/4 GPU-drawn desktop: turning it on and keeping it. The screen resets once (it may freeze for a few seconds)."
    mkdir -p "/Users/$U/Downloads/pipe-test"
    bash "$HERE/irisxe-pipe-test.sh" > "$OUT/3-pipe-test-start.txt" 2>&1
    touch "/Users/$U/Downloads/pipe-test/keep"
    chown -R "$U" "/Users/$U/Downloads/pipe-test"
    WAIT=30                                          # WindowServer restarts after 5 s; let it come back first
fi

# the recording: a launchd job (survives WindowServer restarting and the Terminal session ending)
"$HERE/irisxe-metaltest" --mixreset > "$OUT/4-mixreset.txt" 2>&1
cat > "$OUT/timeline.sh" <<EOF
#!/bin/bash
OUT="$OUT"; U="$U"
touch "\$OUT/4-recording"
sleep $WAIT
[ -d "/Users/\$U/Downloads/pipe-test" ] && touch "/Users/\$U/Downloads/pipe-test/keep"
"$HERE/irisxe-metaltest" --mixreset >> "\$OUT/4-mixreset.txt" 2>&1
T="\$OUT/4-timeline.txt"
echo "time      frames  fps  GPU-RC6%  GPU-MHz  WS-CPU%  load   draw-perf" > "\$T"
note() { launchctl asuser "\$(id -u "\$U")" osascript -e "display notification \"\$1\" with title \"IrisXe test\"" 2>/dev/null; }
# v0.59.1: three phases of 60 s, the stutter meter from zero in each
phase() {   # number, scanout mode, name, async command buffers (v0.63.0), smart flushing, draws per job (v0.63.1)
    sysctl debug.irisxe_scanout=\$2 > /dev/null 2>&1
    sysctl debug.irisxe_async=\$4 > /dev/null 2>&1
    sysctl debug.irisxe_flush=\$5 > /dev/null 2>&1
    sysctl debug.irisxe_batch=\$6 > /dev/null 2>&1
    sysctl debug.irisxe_timing=1 > /dev/null 2>&1
    echo "--- phase \$1: \$3 (debug.irisxe_scanout=\$2, async=\$4, flush=\$5, batch=\$6) ---" >> "\$T"
    if [ $AUTO -eq 1 ]; then note "Phase \$1 of 3 (\$3): don't touch the Mac"; else note "Phase \$1 of 3 (\$3): keep going - Launchpad, Mission Control, drag a window"; fi
}
endphase() {
    ioreg -l -w0 | grep -E '"IrisXe-(FrameTiming|KernelTiming|DrawPhases|AccelMaps|DrawPerf|Async|AsyncState|Hang|HangHistory|GPU)"' | sed 's/^[ |]*//' > "\$OUT/4-timing-\$1.txt"
}
phase 1 1 "as before" 0 0 0
P=1
prev=""; t0=\$(date +%s)
# v0.63.1: a fixed workload, the same in every phase: Mission Control in and out, Launchpad in and out, every 2 s
if [ $AUTO -eq 1 ]; then
    ( ui() { launchctl asuser "\$(id -u "\$U")" sudo -u "\$U" open -a "\$1" > /dev/null 2>&1; }
      while [ -e "\$OUT/4-recording" ] && [ ! -e "\$OUT/4-done" ]; do
          ui "Mission Control"; sleep 2; ui "Mission Control"; sleep 2; ui Launchpad; sleep 2; ui Launchpad; sleep 2
      done ) &
    WORK=\$!
fi
while [ \$((\$(date +%s) - t0)) -lt 180 ]; do
    el=\$((\$(date +%s) - t0))
    if [ \$P -eq 1 ] && [ \$el -ge 60 ]; then endphase 1-before; phase 2 1 "flushing between draws only when needed (new)" 0 1 0; P=2; fi
    if [ \$P -eq 2 ] && [ \$el -ge 120 ]; then endphase 2-flush; phase 3 1 "asynchronous command buffers" 1 0 0; P=3; fi
    # v0.59.3: where WindowServer's (and the Dock's: Launchpad) CPU time goes while the screen moves - 5 s each
    if [ \$el -ge 20 ] && [ -z "\$S1" ]; then S1=1; sample WindowServer 5 1 -mayDie -file "\$OUT/4-sample-ws-direct.txt" > /dev/null 2>&1 & fi
    if [ \$el -ge 40 ] && [ -z "\$S2" ]; then S2=1; sample Dock 5 1 -mayDie -file "\$OUT/4-sample-dock.txt" > /dev/null 2>&1 & fi
    if [ \$el -ge 80 ] && [ -z "\$S3" ]; then S3=1; sample WindowServer 5 1 -mayDie -file "\$OUT/4-sample-ws-flush.txt" > /dev/null 2>&1 & fi
    st=\$(ioreg -l -w0 | grep -E '"IrisXe-(DisplayPipe|GPU|GPUFreq|DrawPerf)"')
    fr=\$(echo "\$st" | grep "IrisXe-DisplayPipe" | sed -nE 's/.*frames ([0-9]+),.*/\1/p' | head -1)
    rc=\$(echo "\$st" | grep '"IrisXe-GPU"' | sed -nE 's/.*RC6 on ([0-9]+)%.*/\1/p' | head -1)
    mhz=\$(echo "\$st" | grep "IrisXe-GPUFreq" | sed -nE 's/.*now ([0-9]+) MHz.*/\1/p' | head -1)
    dp=\$(echo "\$st" | grep "IrisXe-DrawPerf" | sed -nE 's/.*: (.*)"\$/\1/p' | head -1 | cut -c1-90)
    ws=\$(ps -A -o %cpu=,comm= | awk '/WindowServer\$/{print \$1; exit}')
    ld=\$(sysctl -n vm.loadavg | awk '{print \$2}')
    fps=""; [ -n "\$prev" ] && [ -n "\$fr" ] && fps=\$((fr - prev)); prev="\$fr"
    printf "%s %7s %4s %9s %8s %8s %6s  %s\n" "\$(date +%H:%M:%S)" "\$fr" "\$fps" "\$rc" "\$mhz" "\$ws" "\$ld" "\$dp" >> "\$T"
    s=\$((\$(date +%s) - t0))
    if [ \$((s % 30)) -lt 2 ]; then
        { date +%H:%M:%S; top -l 2 -s 1 -o cpu -n 8 -stats pid,command,cpu | tail -8; } >> "\$OUT/4-top-during.txt"   # v0.59.3: 2nd sample (the 1st has no CPU %)
        ioreg -l -w0 | grep '"IrisXe-DrawMix"' >> "\$OUT/4-drawmix.txt"
    fi
    sleep 1
done
endphase 3-all
[ -n "\$WORK" ] && kill \$WORK 2> /dev/null
sysctl debug.irisxe_scanout=1 > /dev/null 2>&1
sysctl debug.irisxe_async=0 > /dev/null 2>&1
sysctl debug.irisxe_flush=0 > /dev/null 2>&1
sysctl debug.irisxe_batch=0 > /dev/null 2>&1
ioreg -l -w0 | grep -E '"IrisXe-(DisplayPipe|DrawPerf|DrawSlowest|DrawMix|AccelBatch|GPU|GPUFreq|Hang|HangHistory|HangJob|GPUSpace|GPUVA|AccelMaps|Scanout|FrameTiming|KernelTiming|DrawPhases|Async|AsyncState)"' > "\$OUT/4-status-after.txt" 2>&1
echo "WindowServer started: \$(ps -o lstart= -p "\$(pgrep -x WindowServer)")" >> "\$OUT/4-status-after.txt"
echo "\$(date "+%H:%M:%S") 4/4 recording finished. Now run:  bash ~/Downloads/irisxe-results/collect.sh" >> "\$OUT/0-steps.txt"
touch "\$OUT/4-done"
chown -R "\$U" "\$OUT"
launchctl asuser "\$(id -u "\$U")" osascript -e 'display notification "Run: bash ~/Downloads/irisxe-results/collect.sh" with title "IrisXe test finished"' 2>/dev/null
launchctl bootout system/com.irisxe.timeline 2>/dev/null
EOF
chmod +x "$OUT/timeline.sh"
PL=/tmp/com.irisxe.timeline.plist
cat > "$PL" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key><string>com.irisxe.timeline</string>
  <key>ProgramArguments</key><array><string>/bin/bash</string><string>$OUT/timeline.sh</string></array>
  <key>RunAtLoad</key><true/>
  <key>StandardOutPath</key><string>$OUT/4-timeline-job.log</string>
  <key>StandardErrorPath</key><string>$OUT/4-timeline-job.log</string>
</dict></plist>
EOF
chown root:wheel "$PL"; chmod 644 "$PL"
if launchctl bootstrap system "$PL" 2>> "$OUT/0-steps.txt"; then
    say "4/4 recording 3 minutes in 3 phases (starts in $WAIT s; a notification marks each phase)."
    if [ $AUTO -eq 1 ]; then
        say "    The test opens and closes Mission Control and Launchpad by itself. Don't touch the Mac for 3 minutes."
    else
        say "    For the whole 3 minutes, repeat: open and close Launchpad, open and close Mission Control, drag a window around."
        say "    Do the same in every phase - the test compares them."
    fi
    say "    A notification says when it is finished. Then run:  bash ~/Downloads/irisxe-results/collect.sh"
else
    say "4/4 ERROR: could not start the recording job (see 0-steps.txt)"
fi
exit 0

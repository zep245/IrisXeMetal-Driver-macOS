#!/bin/bash
# IrisXe v0.69.4 hardware video decoding test (H.264 on Tiger Lake's video engine VCS0).
# Apple's Ice Lake video driver is allowed to use our decoder for ONE boot (the switch erases itself at the start of
# that boot, so the boot after it is normal again). On that boot this script goes through the stages live - no more
# restarts:
#   stage A  sudo sysctl debug.irisxe_video=1   video engine self-test: 100 jobs + a forced hang that must recover
#   stage B  sudo sysctl debug.irisxe_video=2   real decoding on VCS0, the first jobs recorded
#   stage C  sudo sysctl debug.irisxe_video=3   real decoding, no recording (speed)
# Any GPU timeout resets the video engine only (the desktop keeps running); after 3 timeouts decoding switches itself
# off for the rest of the boot and apps decode in software again.
#   sudo bash irisxe-videodecode.sh prepare   switch the test on for the next boot (then restart)
#   sudo bash irisxe-videodecode.sh           after that restart: run the stages, collect everything in a zip
cd "$(dirname "$0")" || exit 1
if [ "$(id -u)" != 0 ]; then echo "Run with sudo: sudo bash irisxe-videodecode.sh $1"; exit 1; fi
if [ "$1" = prepare ]; then
    nvram irisxe-videotest=1 && echo "== the video test is on for the next boot. Restart now, log in, wait 2 minutes, then run: sudo bash irisxe-videodecode.sh"
    exit 0
fi
if ! sysctl -n debug.irisxe_video > /dev/null 2>&1; then
    echo "IrisXeAccel is older than v0.69.4: run bash install-all.sh from this release, allow it in Privacy & Security, restart, run this again."
    exit 1
fi
U=$(stat -f %Su /dev/console)
OUT="irisxe-videodecode-$(date +%H%M%S)"
mkdir -p "$OUT"
props() { ioreg -l -w0 | grep -oE '"(IrisXe-Video|IrisXe-VideoDecode|IrisXe-VideoEngine|IrisXe-VideoSelfTest|IrisXe-VideoSummary|IrisXe-VideoCounts|IODVDBundleName)" = "?[^"]*"?' ; }
ST=$(ioreg -l -w0 | grep -o '"IrisXe-Video" = "[^"]*"' | head -1)
echo "$ST"
START=$(date "+%Y-%m-%d %H:%M:%S")
chmod +x ./irisxe-vtdecode; xattr -c ./irisxe-vtdecode 2>/dev/null
{
    echo "== versions"; kextstat 2>/dev/null | grep -i irisxe | awk '{print $6, $7}'
    echo "== stage A: video engine self-test"
    sysctl debug.irisxe_video=1
    ioreg -l -w0 | grep -o '"IrisXe-VideoSelfTest" = "[^"]*"'
} 2>&1 | tee "$OUT/stage-a.txt"
SELF=$(ioreg -l -w0 | grep -o '"IrisXe-VideoSelfTest" = "[^"]*"')
case "$SELF" in
    *"100/100 ok"*"job after the reset: ok"*) A_OK=1 ;;
    *) A_OK=0 ;;
esac
case "$ST" in *"video test boot"*) TESTBOOT=1 ;; *) TESTBOOT=0 ;; esac
if [ "$A_OK" = 1 ] && [ "$TESTBOOT" = 1 ]; then
    {
        echo "== stage B: decoding on the video engine, first jobs recorded (10 s clip)"
        sysctl debug.irisxe_video=2
        sudo -u "$U" ./irisxe-vtdecode 10 2>&1
        sleep 1
        props
        echo "== stage C: decoding, no recording (10 s clip)"
        sysctl debug.irisxe_video=3
        sudo -u "$U" ./irisxe-vtdecode 10 2>&1
        sleep 1
        props
    } 2>&1 | tee "$OUT/stages-bc.txt"
    echo
    echo "Stage C stays on for this boot: play an H.264 video in QuickTime Player or Safari now if you like."
    echo "(To stop it: sudo sysctl debug.irisxe_video=0 - or just restart.)"
elif [ "$A_OK" != 1 ]; then
    echo "== stage A did not pass: stages B and C are skipped (nothing ran on the video engine for real)." | tee -a "$OUT/stage-a.txt"
else
    echo "== stage A passed. This is not the video test boot: for stages B and C run 'sudo bash irisxe-videodecode.sh prepare', restart, run this again." | tee -a "$OUT/stage-a.txt"
fi
props > "$OUT/accel-properties.txt"
ioreg -l -w0 | grep -oE '"IrisXe-VideoRec[0-9]+" = "[^"]*"' | sed -E 's/^"IrisXe-VideoRec([0-9]+)"/\1 &/' | sort -n | cut -d' ' -f2- > "$OUT/record.txt"
ioreg -l -w0 -r -c IrisXeProbe -d 1 | grep -E "IrisXe-(Video|VCS|Resets)" > "$OUT/core-properties.txt" 2>&1
log show --start "$START" --predicate 'process CONTAINS "VTDecoder" OR process CONTAINS "irisxe-vtdecode" OR sender CONTAINS "AppleGVA" OR sender CONTAINS "IrisXe" OR sender CONTAINS "VADriver" OR eventMessage CONTAINS "GVA"' \
    2>&1 | grep -iE "GVA|AVD|error|fail|video|VCS|hang|reset|decoder" | grep -v "IrisXeMTL: \(newBuffer\|texture\|IOSurface\)" | tail -600 > "$OUT/log.txt"
log show --last boot --predicate 'sender == "IOAcceleratorFamily2" OR eventMessage CONTAINS "IOAccelContext2"' 2>&1 | tail -200 > "$OUT/kernel-family.txt"
find /Library/Logs/DiagnosticReports "/Users/$U/Library/Logs/DiagnosticReports" -newermt "$START" \( -name "VTDecoder*" -o -name "irisxe-vtdecode*" -o -name "*panic*" \) -exec cp {} "$OUT/" \; 2>/dev/null
cat "$OUT/accel-properties.txt"
chown -R "$U" "$OUT"; zip -qr "$OUT.zip" "$OUT" && chown "$U" "$OUT.zip"
echo
echo "== done: send $OUT.zip (in $(pwd))"

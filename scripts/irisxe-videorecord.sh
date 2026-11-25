#!/bin/bash
# IrisXe v0.68.43 video record test (hardware video decoding, step V2b). Apple's Ice Lake video driver is allowed to
# talk to our accelerator for ONE boot; our video context accepts and RECORDS its command streams (nothing runs on
# the GPU). The record shows exactly what Apple's driver sends - the base for running it on the video engine.
#   sudo bash irisxe-videorecord.sh prepare   switch the test on for the next boot (then restart)
#   sudo bash irisxe-videorecord.sh           after that restart: decode a short H.264 clip, collect the record
# The switch is erased at the very start of the test boot, so the boot after it is normal again by itself.
# During the test boot, hardware video decoding is announced but does not work yet: Safari / QuickTime videos may
# not play on that boot. Restart normally afterwards.
cd "$(dirname "$0")" || exit 1
if [ "$(id -u)" != 0 ]; then echo "Run with sudo: sudo bash irisxe-videorecord.sh $1"; exit 1; fi
if [ "$1" = prepare ]; then
    nvram irisxe-videotest=1 && echo "== the video record test is on for the next boot. Restart now, log in, then run: sudo bash irisxe-videorecord.sh"
    exit 0
fi
U=$(stat -f %Su /dev/console)
OUT="irisxe-videorecord-$(date +%H%M%S)"
mkdir -p "$OUT"
ST=$(ioreg -l -w0 | grep -o '"IrisXe-Video" = "[^"]*"' | head -1)
echo "$ST"
case "$ST" in *RECORD-ONLY*) ;; *) echo "The test is not on this boot. First: sudo bash irisxe-videorecord.sh prepare, restart, then run this again."; exit 1 ;; esac
START=$(date "+%Y-%m-%d %H:%M:%S")
echo "== decoding a short H.264 clip with the hardware decoder (at most 25 s)"
chmod +x ./irisxe-vtdecode; xattr -c ./irisxe-vtdecode 2>/dev/null
sudo -u "$U" ./irisxe-vtdecode 20 2>&1 | tee "$OUT/vtdecode.txt"
sleep 2
ioreg -l -w0 | grep -oE '"(IrisXe-Video|IrisXe-VideoSummary|IrisXe-ROPrivate|IODVDBundleName|IOGVACodec|IOVARendererID)" = [^|]{0,200}' > "$OUT/accel-properties.txt"
ioreg -l -w0 | grep -oE '"IrisXe-VideoRec[0-9]+" = "[^"]*"' | sed -E 's/^"IrisXe-VideoRec([0-9]+)"/\1 &/' | sort -n | cut -d' ' -f2- > "$OUT/record.txt"   # v0.68.10: in parts
ioreg -l -w0 | grep -oE '"(IrisXe-VideoSummary|IrisXe-VideoCounts)" = "[^"]*"' >> "$OUT/record.txt"   # v0.68.6
ioreg -l -w0 -r -c IrisXeAccelerator -d 1 > "$OUT/accel-ioreg.txt" 2>&1   # v0.68.10: everything, as a fallback
log show --start "$START" --predicate 'process CONTAINS "VTDecoder" OR process CONTAINS "irisxe-vtdecode" OR sender CONTAINS "AppleGVA" OR sender CONTAINS "IrisXe" OR sender CONTAINS "VADriver" OR eventMessage CONTAINS "GVA" OR eventMessage CONTAINS "AVD"' \
    > "$OUT/log.txt" 2>&1
# v0.68.3: Apple's video framework sets up during the boot, before this script runs - its lines from the whole boot
log show --last boot --predicate 'eventMessage CONTAINS "GVA" OR eventMessage CONTAINS "hw caps" OR eventMessage CONTAINS "accelerator id" OR process CONTAINS "VTDecoder" OR eventMessage CONTAINS "video test boot"' \
    2>&1 | grep -v "IrisXeMTL: \(newBuffer\|texture\|IOSurface\)" | tail -600 > "$OUT/boot-gva.txt"
# v0.68.12: the GPU family's own kernel messages (token / data buffer errors) from the whole boot
log show --last boot --predicate 'sender == "IOAcceleratorFamily2" OR eventMessage CONTAINS "IOAccelContext2"' 2>&1 | tail -300 > "$OUT/kernel-family.txt"
ioreg -l -w0 | grep -oE '"(IOGVA[A-Za-z0-9]*|IODVDBundleName|IOVARendererID)" = [^|]{0,120}' | sort -u > "$OUT/gva-keys.txt"
grep -iE "GVA|AVD|error|fail|IrisXe|VADriver|decoder" "$OUT/log.txt" | grep -v "display pipe: IrisXeDisplayPipe" | tail -400 > "$OUT/log-important.txt"
find /Library/Logs/DiagnosticReports "/Users/$U/Library/Logs/DiagnosticReports" -newermt "$START" \( -name "VTDecoder*" -o -name "irisxe-vtdecode*" -o -name "*panic*" \) -exec cp {} "$OUT/" \; 2>/dev/null
cat "$OUT/accel-properties.txt"
chown -R "$U" "$OUT"; zip -qr "$OUT.zip" "$OUT" && chown "$U" "$OUT.zip"
echo
echo "== done: send $OUT.zip (in $(pwd)). Then restart normally (video decoding is back to the CPU by itself)."

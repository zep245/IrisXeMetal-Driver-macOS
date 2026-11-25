#!/bin/bash
# Round 48 (hardware video decoding, research for docs/VIDEO-DECODER.md section 7: R4, R6, R7). Text only.
#   R4: what happens to a frame after its End token in Apple's Intel kernel driver: SetFence / SetBlockFence,
#       BatchBufferStart, submission to the media ring, the stamp / event that tells the VA driver "done",
#       the video engine's interrupt and hang handling.
#   R6: HEVC / VP9: the codec tokens, HuC (firmware microcontroller) use, short / long format - kernel and VA driver.
#   R7: the same on macOS 26 Tahoe (the workflow runs this script on macos-15-intel AND macos-26-intel; the two
#       outputs are compared offline: vtable slots, struct offsets in the disassembly, token tables).
# Round 47 is kept as run47.sh.
set -x
OUT="$PWD/out"; mkdir -p "$OUT"
sw_vers > "$OUT/00-versions.txt" 2>&1
uname -a >> "$OUT/00-versions.txt" 2>&1
kmutil showloaded --list-only 2>/dev/null | grep -i -E "Accelerator|IntelICL|IntelKBL|GVA" >> "$OUT/00-versions.txt" 2>&1
python3 -c "import capstone" 2>/dev/null || pip3 install --user capstone > "$OUT/01-capstone.txt" 2>&1 || pip3 install --break-system-packages capstone >> "$OUT/01-capstone.txt" 2>&1
KC=/System/Library/KernelCollections
A="$KC/BootKernelExtensions.kc $KC/SystemKernelExtensions.kc"
F=com.apple.iokit.IOAcceleratorFamily2
I=com.apple.driver.AppleIntelICLGraphics
python3 kcdump.py "$OUT" $A -- $F $I > "$OUT/03-kcdump-stdout.txt" 2>&1
for f in "$OUT"/12-*-vtables.txt "$OUT"/10-*-symbols.txt; do [ -f "$f" ] && c++filt < "$f" > "$f.tmp" && mv "$f.tmp" "$f"; done
rm -f "$OUT"/11-*-segments.txt "$OUT"/02-*.txt

# --- symbol lists, filtered (small): everything about video, media ring, fences, stamps, HuC, codecs
cat "$OUT"/10-*-symbols.txt 2>/dev/null | grep -E "Video|Media|VEBox|Fence|fence|Stamp|stamp|HuC|Huc|huc|HEVC|Hevc|VP9|Vp9|HCP|Hcp|MFX|Mfx|VCS|Vcs|Bsd|BSD|Vdbox|VDBox|Codec|codec|Watchdog|Hang|hang|Reset" > "$OUT/20-symbols-video.txt" 2>&1
# --- vtables of the classes we subclass / call (R7 compares slot numbers between Sequoia and Tahoe)
for c in IOAccelContext2 IOAccelVideoContext2 IGAccelVideoContext IGAccelVideoContextMain IGAccelVideoContextMedia IGAccelVideoContextVEBox IOAccelDevice2 IGAccelDevice IOAccelEventMachine2 IOAccelResource2; do
  awk -v C="vtable for $c @" 'index($0,C)==1{p=1;print;next} /^vtable for /{p=0} p' "$OUT"/12-*-vtables.txt >> "$OUT/21-vtables-video.txt" 2>/dev/null
done

VT="--vt IOAccelContext2 --vt IOAccelVideoContext2 --vt IGAccelVideoContext --vt IGAccelVideoContextMain --vt IGAccelVideoContextMedia --vt IGAccelVideoContextVEBox --vt IOGraphicsAccelerator2 --vt IntelAccelerator --vt IOAccelResource2 --vt IOAccelEventMachine2"

# --- R4: from the End token to "frame done"
python3 kcdis.py "$OUT/30-r4-kext-video.txt" "$OUT" $A -- \
  "$I:^IGAccelVideoContext::(process_token_SetFence|process_token_BatchBufferStart|patch_codechal_command_buffer|.*[Ss]ubmit.*|.*[Ff]ence.*|.*[Ss]tamp.*|.*[Ff]lush.*|.*[Ww]ait.*|.*[Cc]omplete.*)" \
  "$I:^IGAccelVideoContext(Main|Media|VEBox)::(endCommandStream|endScalableCommandStreamFE_BE|processDataBuffers|contextStart)" $VT > "$OUT/30-log.txt" 2>&1
python3 kcdis.py "$OUT/31-r4-kext-ring.txt" "$OUT" $A -- \
  "$I:^IGHardware\w*::.*" \
  "$I:^IntelAccelerator::.*([Mm]edia|[Vv]ideo|VCS|Vcs|BSD|Bsd|Vdbox|VDBox|[Ii]nterrupt|[Hh]ang|[Rr]eset|[Ww]atchdog|[Ss]tamp).*" $VT > "$OUT/31-log.txt" 2>&1
python3 kcdis.py "$OUT/32-r4-family.txt" "$OUT" $A -- \
  "$F:^IOAccelContext2::(process_token_SetBlockFence|process_token_End|process_token_Start|endCommandStream|beginCommandStream|.*[Ss]ubmit.*|.*[Ff]ence.*|.*[Ss]tamp.*|processSidebandBuffer|processDataBuffers)" \
  "$F:^IOAccelVideoContext2::.*" \
  "$F:^IOAccelEventMachine2::(signalStamp|incrementStamp|setEventStamp|getStamp|finishEventRetire|.*[Ww]ait.*)" $VT > "$OUT/32-log.txt" 2>&1

# --- R6: HEVC / VP9 / HuC in the kernel driver
python3 kcdis.py "$OUT/40-r6-kext-codecs.txt" "$OUT" $A -- \
  "$I:^IGAccelVideoContext(Media|Main)::process_token_.*" \
  "$I:.*([Hh]u[Cc]|HEVC|Hevc|VP9|Vp9|HCP|Hcp).*" $VT > "$OUT/40-log.txt" 2>&1

# --- R6: the VA driver (user space): codec support, HuC / short-format strings and symbols (no binary uploaded)
VA=/System/Library/Extensions/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/AppleIntelICLGraphicsVADriver
ls -la "$VA" > "$OUT/50-va.txt" 2>&1
nm -m "$VA" 2>/dev/null | c++filt | grep -i -E "huc|hevc|vp9|hcp|s2l|shortformat|longformat|codechal_decode|Decode(Hevc|Vp9|Avc)" > "$OUT/51-va-symbols-codecs.txt" 2>&1
otool -s __TEXT __cstring -v "$VA" 2>/dev/null | grep -i -E "huc|hevc|vp9|hcp|s2l|short|long format|firmware|guc|dmem|imem" > "$OUT/52-va-strings-codecs.txt" 2>&1
plutil -convert xml1 -o - "$(dirname "$(dirname "$VA")")/Info.plist" > "$OUT/53-va-info.txt" 2>&1
# HuC / GuC firmware files shipped with macOS (names only)
ls -la /usr/share/firmware 2>/dev/null > "$OUT/54-firmware.txt"; find /System/Library/Extensions/AppleIntelICLGraphics*.kext /System/Library/Extensions/AppleIntelICLGraphicsFramebuffer.kext -iname "*huc*" -o -iname "*guc*" -o -iname "*.bin" 2>/dev/null >> "$OUT/54-firmware.txt"
strings -a "$KC/SystemKernelExtensions.kc" 2>/dev/null | grep -i -E "huc|guc" | sort -u | head -300 > "$OUT/55-kc-huc-guc-strings.txt"

rm -f "$OUT"/13-*-strings.txt "$OUT"/12-*-vtables.txt
gzip -9 "$OUT"/3*.txt "$OUT"/4*.txt 2>/dev/null
ls -la "$OUT"
exit 0

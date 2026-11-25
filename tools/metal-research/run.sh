#!/bin/bash
# Round 49 (hardware video decoding, R4 rest): the video context end of stream, fences, FIFO channel. Text only.
# Round 48 (R4, R6, R7) is kept as run48.sh.
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

VT="--vt IOAccelContext2 --vt IOAccelVideoContext2 --vt IGAccelVideoContext --vt IGAccelVideoContextMain --vt IGAccelVideoContextMedia --vt IGAccelVideoContextVEBox --vt IOGraphicsAccelerator2 --vt IntelAccelerator --vt IOAccelResource2 --vt IOAccelEventMachine2"
# --- R4 (rest): how the frame's fence slot (SetFence index, context +0x1300) gets its stamp, and how the job is queued
python3 kcdis.py "$OUT/60-r4-endstream.txt" "$OUT" $A -- \
  "$I:^IGAccelVideoContext::(endCommandStream|beginCommandStream|prepare_to_decode|bind_resource|set_current_resource|addDataBufferToChannel|removeDataBufferFromChannel|removeCurrentResourceFromChannel|getChannel|process_token_CodecHAL|contextStart|contextStop|getDataBufferLimits)" \
  "$I:^IGAccelFIFOChannel::.*" \
  "$I:^IGAccelVideoContextMedia::process_token_(AVCDecode|CODECDecode)" $VT > "$OUT/60-log.txt" 2>&1
python3 kcdis.py "$OUT/61-family-fences.txt" "$OUT" $A -- \
  "$F:^IOAccelContext2::(enable_block_fences|finish_fence_event|s_finish_fence_event|.*[Ff]ence.*|process_token_End|submit_data_buffers)" \
  "$F:^IOAccelEventMachine2::(setEventStamp|initEvent|mergeEvent|finishEvent|testEvent|waitForEvent|.*[Ee]vent.*)" \
  "$F:^IOAccelChannel2::.*" $VT > "$OUT/61-log.txt" 2>&1
rm -f "$OUT"/13-*-strings.txt "$OUT"/12-*-vtables.txt
gzip -9 "$OUT"/6*.txt 2>/dev/null
ls -la "$OUT"
exit 0

#!/bin/bash
# Round 26 (v0.55.1): on the IrisXeIntelFB boot with the display pipe made, CoreDisplay now hands the screen GPU images
# (DisplaySurface 2560x1600 BGRA) and sends them to the display pipe as IOPresentment transactions - and logs
# "Fallback GPU Display Pipe transaction submission failed"; the picture freezes. This round reads the transaction path:
#  kernel : IOAccelDisplayPipeUserClient2 transaction calls -> IOAccelDisplayPipe -> IOAccelDisplayPipeTransaction2
#           (what the family asks the GPU driver for, what makes a transaction fail), and Apple's Intel
#           IGAccelDisplayPipe / IGAccelDisplayPipeTransaction for comparison
#  user   : CoreDisplay's RunGPUDisplayPipe (the "Fallback GPU Display Pipe") and IOPresentment's transaction submit
# Text output only. Round 25 is kept as run25.sh.
set -x
OUT="$PWD/out"; mkdir -p "$OUT"
sw_vers > "$OUT/00-versions.txt" 2>&1
pip3 install --break-system-packages capstone > "$OUT/01-capstone.txt" 2>&1 || pip3 install capstone >> "$OUT/01-capstone.txt" 2>&1
KC=/System/Library/KernelCollections
A="$KC/BootKernelExtensions.kc $KC/SystemKernelExtensions.kc"
IDS="com.apple.iokit.IOAcceleratorFamily2 com.apple.driver.AppleIntelICLGraphics"
python3 kcdump.py "$OUT" $A -- $IDS > "$OUT/03-kcdump-stdout.txt" 2>&1
for f in "$OUT"/12-*-vtables.txt "$OUT"/10-*-symbols.txt; do c++filt < "$f" > "$f.tmp" && mv "$f.tmp" "$f"; done
for f in "$OUT"/13-*-strings.txt; do
  grep -i -E "transaction|pipe|flip|swap|plane|surface|gamma" "$f" > "$f.tmp"; mv "$f.tmp" "$f"
done
rm -f "$OUT"/11-*-segments.txt
VT="--vt IOAccelDisplayPipe --vt IOAccelDisplayPipeUserClient2 --vt IOAccelDisplayPipeTransaction2 --vt IOAccelDisplayMachine --vt IOGraphicsAccelerator2 --vt IGAccelDisplayPipe --vt IGAccelDisplayPipeTransaction --vt IGAccelDisplayMachine --vt IntelAccelerator"
python3 kcdis.py "$OUT/h0-family-transaction.txt" "$OUT" $A -- \
  'com.apple.iokit.IOAcceleratorFamily2:^IOAccelDisplayPipeUserClient2::(s_transaction|transaction_|s_set_pipe_index|set_pipe_index|externalMethod)' \
  'com.apple.iokit.IOAcceleratorFamily2:^IOAccelDisplayPipe::.*([Tt]ransaction|[Ff]lip|[Ss]ubmit|[Cc]ommit|[Ss]wap|vbl|VBL)' \
  'com.apple.iokit.IOAcceleratorFamily2:^IOAccelDisplayPipeTransaction2?::' \
  'com.apple.iokit.IOAcceleratorFamily2:^IOAccelDisplayMachine2?::.*([Tt]ransaction|[Ff]lip|[Ss]ubmit|[Ss]wap)' $VT > "$OUT/h0-log.txt" 2>&1
python3 kcdis.py "$OUT/h1-intel-transaction.txt" "$OUT" $A -- \
  'com.apple.driver.AppleIntelICLGraphics:^IGAccelDisplayPipe::' \
  'com.apple.driver.AppleIntelICLGraphics:^IGAccelDisplayPipeTransaction::' \
  'com.apple.driver.AppleIntelICLGraphics:^IntelAccelerator::(newDisplayPipeTransaction|.*[Tt]ransaction|.*[Ff]lip)' $VT > "$OUT/h1-log.txt" 2>&1
xcrun clang -fno-objc-arc -O1 -framework Foundation -framework Metal -framework IOKit udis_server.m -o udis_server > "$OUT/40-build.txt" 2>&1
SL=/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight
CD=/System/Library/Frameworks/CoreDisplay.framework/CoreDisplay
IOP=/System/Library/PrivateFrameworks/IOPresentment.framework/IOPresentment
python3 udis.py "$OUT/h2-coredisplay-fallback.txt" "load:$CD" "load:$IOP" "load:$SL" \
  'x:CoreDisplay:Fallback GPU Display Pipe transaction submission failed' \
  "x:CoreDisplay:Can't run fallback without surface" \
  'y:CoreDisplay:DisplayPipe17RunGPUDisplayPipe' \
  --follow 'Transaction|Presentment|IOP|Submit|Commit' > "$OUT/h2-log.txt" 2>&1
python3 udis.py "$OUT/h3-iop-transaction.txt" "load:$CD" "load:$IOP" "load:$SL" \
  'y:IOPresentment:([Tt]ransaction.*([Ss]ubmit|[Cc]ommit|[Ee]nd|[Bb]egin)|[Ss]ubmit[Tt]ransaction|SwapSubmit)' \
  --follow 'IOConnect|Transaction' > "$OUT/h3-log.txt" 2>&1
ls -la "$OUT"
exit 0

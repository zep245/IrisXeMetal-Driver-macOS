#!/bin/bash
# Round 29 (v0.56.1): with IrisXeDisplayPipe, CoreDisplay still logs "Fallback GPU Display Pipe transaction submission
# failed": CoreDisplay::DisplayPipe::Commit -> IOPTransaction::Submit() returns false. This round reads that path down
# to the kernel call: CoreDisplay IOPTransaction::Submit, IOPresentment's transaction functions, and the
# IOAccelerator.framework display-pipe calls (IOAccelDisplayPipeTransaction*) that talk to IOAccelDisplayPipeUserClient2.
# Text output only. Round 28 is kept as run28.sh.
set -x
OUT="$PWD/out"; mkdir -p "$OUT"
sw_vers > "$OUT/00-versions.txt" 2>&1
pip3 install --break-system-packages capstone > "$OUT/01-capstone.txt" 2>&1 || pip3 install capstone >> "$OUT/01-capstone.txt" 2>&1
xcrun clang -fno-objc-arc -O1 -framework Foundation -framework Metal -framework IOKit udis_server.m -o udis_server > "$OUT/40-build.txt" 2>&1
SL=/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight
CD=/System/Library/Frameworks/CoreDisplay.framework/CoreDisplay
IOP=/System/Library/PrivateFrameworks/IOPresentment.framework/IOPresentment
IOA=/System/Library/PrivateFrameworks/IOAccelerator.framework/IOAccelerator
python3 udis.py "$OUT/k0-iop-submit.txt" "load:$IOA" "load:$CD" "load:$IOP" "load:$SL" \
  'y:CoreDisplay:IOPTransaction6Submit' \
  --follow 'IOPresentment|IOAccel|Submit|Commit|Transaction' --depth 2 > "$OUT/k0-log.txt" 2>&1
python3 udis.py "$OUT/k1-ioaccel-pipe.txt" "load:$IOA" "load:$CD" "load:$IOP" "load:$SL" \
  'y:IOAccelerator:IOAccelDisplayPipe' > "$OUT/k1-log.txt" 2>&1
python3 udis.py "$OUT/k2-iop-transaction.txt" "load:$IOA" "load:$CD" "load:$IOP" "load:$SL" \
  'y:IOPresentment:(IOPresentmentTransactionDisplayPipe|IOPresentmentDisplayPipe).*(Submit|Commit|End|Begin|Wait|Validate|Prepare)' \
  --follow 'IOAccel|IOConnect' > "$OUT/k2-log.txt" 2>&1
ls -la "$OUT"
exit 0

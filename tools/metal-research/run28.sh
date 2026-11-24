#!/bin/bash
# Round 28 (v0.55.1, plan for v0.56.0 approved): the last two facts before IrisXeDisplayPipe is written -
#  1. how a queued transaction becomes "live" and is completed: IOAccelDisplayPipe::event_interrupt_gated (not read
#     yet), signal_event_interrupt, enable/disable_event_interrupt, getCurrentPlane*, copySurface, isActive
#  2. the simplest real implementation Apple ships: AppleParavirtDisplayPipe (the virtual-machine GPU), every method
# Text output only. Round 27 is kept as run27.sh.
set -x
OUT="$PWD/out"; mkdir -p "$OUT"
sw_vers > "$OUT/00-versions.txt" 2>&1
pip3 install --break-system-packages capstone > "$OUT/01-capstone.txt" 2>&1 || pip3 install capstone >> "$OUT/01-capstone.txt" 2>&1
KC=/System/Library/KernelCollections
A="$KC/BootKernelExtensions.kc $KC/SystemKernelExtensions.kc"
IDS="com.apple.iokit.IOAcceleratorFamily2 com.apple.driver.AppleParavirtGPU"
python3 kcdump.py "$OUT" $A -- $IDS > "$OUT/03-kcdump-stdout.txt" 2>&1
for f in "$OUT"/12-*-vtables.txt "$OUT"/10-*-symbols.txt; do c++filt < "$f" > "$f.tmp" && mv "$f.tmp" "$f"; done
rm -f "$OUT"/11-*-segments.txt "$OUT"/13-*-strings.txt
VT="--vt IOAccelDisplayPipe --vt AppleParavirtDisplayPipe --vt IOAccelDisplayPipeTransaction2 --vt IOAccelEventMachine2 --vt IOAccelEventMachineFast2 --vt IOAccelResource2"
python3 kcdis.py "$OUT/j0-family-live.txt" "$OUT" $A -- \
  'com.apple.iokit.IOAcceleratorFamily2:^IOAccelDisplayPipe::(event_interrupt_gated|signal_event_interrupt|enable_event_interrupt|disable_event_interrupt|getCurrentPlane|copySurface|isActive|setup_workloop|transaction_wait_gated|finishTransactionQueue|releaseLiveTransaction|init\(|getWidth|getHeight)' \
  'com.apple.iokit.IOAcceleratorFamily2:^IOAccelDisplayPipeTransaction2::(prepare|build_barrier_event|make_flip_transaction|getPlaneDirtyRect|getPlaneSrcRect|getPlaneDstRect|getPlaneIOSurface)' $VT > "$OUT/j0-log.txt" 2>&1
python3 kcdis.py "$OUT/j1-paravirt-pipe.txt" "$OUT" $A -- \
  'com.apple.driver.AppleParavirtGPU:^AppleParavirtDisplayPipe::' \
  'com.apple.driver.AppleParavirtGPU:^AppleParavirtDisplayMachine::' \
  'com.apple.driver.AppleParavirtGPU:^AppleParavirtAccelerator::.*([Dd]isplay|[Vv]bl|VBL|[Ii]nterrupt|[Ff]lip|[Ss]canout)' $VT > "$OUT/j1-log.txt" 2>&1
grep -E "AppleParavirtDisplayPipe::" "$OUT/10-com.apple.driver.AppleParavirtGPU-symbols.txt" > "$OUT/j2-paravirt-pipe-symbols.txt" 2>&1
ls -la "$OUT"
exit 0

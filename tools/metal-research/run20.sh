#!/bin/bash
# Round 20 (v0.54.2): round 19 showed SkyLight picks the compositor per display GPU: WSGetCompositorForAccelerator(id)
# -> WSGetCompositorMetal(id) -> CompositorMetal::new_compositor(id); when that returns nothing the display gets
# CompositorSW (CPU) although "Metal compositor activated" was logged. This round reads those functions and where the
# display's accelerator id (a 32-bit number) comes from in SkyLight and CoreDisplay.
# Text output only. Round 19 is kept as run19.sh.
set -x
OUT="$PWD/out"; mkdir -p "$OUT"
sw_vers > "$OUT/00-versions.txt" 2>&1
pip3 install --break-system-packages capstone > "$OUT/01-capstone.txt" 2>&1 || pip3 install capstone >> "$OUT/01-capstone.txt" 2>&1
xcrun clang -fno-objc-arc -O1 -framework Foundation -framework Metal -framework IOKit udis_server.m -o udis_server > "$OUT/40-build.txt" 2>&1
SL=/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight
CD=/System/Library/Frameworks/CoreDisplay.framework/CoreDisplay
IOP=/System/Library/PrivateFrameworks/IOPresentment.framework/IOPresentment
python3 udis.py "$OUT/b0-new-compositor.txt" "load:$CD" "load:$IOP" "load:$SL" \
  'y:SkyLight:(new_compositor|WSGetCompositorForAccelerator|HaveMetalCompositorForAccelerator|WSSystemCanCompositeWithMetal|CompositorMetal::CompositorMetal|CompositorMetal13GPURegistryID)' \
  --follow 'Compositor|Metal|MTL|Accelerator|registry|Registry' --depth 2 > "$OUT/b0-log.txt" 2>&1
python3 udis.py "$OUT/b1-accel-id.txt" "load:$CD" "load:$IOP" "load:$SL" \
  'y:SkyLight:(CDDisplay.*[Aa]ccel|[Aa]ccelerator[A-Z]|GetAccelerator|AcceleratorID|acceleratorID|accelerator_id|IOAccelIndex)' \
  'y:CoreDisplay:([Aa]ccelerator|IOAccelIndex|GPURegistry|AccelID)' \
  'x:SkyLight:IOAccelIndex' 'x:CoreDisplay:IOAccelIndex' > "$OUT/b1-log.txt" 2>&1
ls -la "$OUT"
exit 0

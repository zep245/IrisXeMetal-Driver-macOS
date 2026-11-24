#!/bin/bash
# Round 12 (display pipe): why WindowServer says "Unable to find display pipe for IOFB service" and paints the
# desktop on the CPU, and what a framebuffer must provide before Apple's display machine builds a display pipe.
#  family : IOAccelDisplayMachine (start / found_framebuffer / framebuffer resources), IOAccelDisplayPipe
#           (creation, flips), the display-pipe user client WindowServer opens
#  Intel  : Apple's Ice Lake accelerator (IGAccelDisplayMachine / IGAccelDisplayPipe) and the calls it makes
#           into its framebuffer driver (AppleIntelICLLPGraphicsFramebuffer)
# Text output only (annotated disassembly of selected functions, names, strings about display pipes).
# Round 11 is kept as run11.sh.
set -x
OUT="$PWD/out"; mkdir -p "$OUT"
sw_vers > "$OUT/00-versions.txt" 2>&1
pip3 install --break-system-packages capstone > "$OUT/01-capstone.txt" 2>&1 || pip3 install capstone >> "$OUT/01-capstone.txt" 2>&1
KC=/System/Library/KernelCollections
A="$KC/BootKernelExtensions.kc $KC/SystemKernelExtensions.kc"
IDS="com.apple.iokit.IOAcceleratorFamily2 com.apple.driver.AppleIntelICLGraphics com.apple.driver.AppleIntelICLLPGraphicsFramebuffer com.apple.iokit.IOGraphicsFamily"
python3 kcdump.py "$OUT" $A -- $IDS > "$OUT/03-kcdump-stdout.txt" 2>&1
for f in "$OUT"/12-*-vtables.txt "$OUT"/10-*-symbols.txt; do c++filt < "$f" > "$f.tmp" && mv "$f.tmp" "$f"; done
# strings: keep only the ones about display pipes / framebuffers / flips
for f in "$OUT"/13-*-strings.txt; do
  grep -i -E "pipe|framebuffer|IOFB|flip|scanout|swap|vbl|plane|display" "$f" > "$f.tmp"; mv "$f.tmp" "$f"
done
rm -f "$OUT"/11-*-segments.txt
VT="--vt IOGraphicsAccelerator2 --vt IOAccelDisplayMachine --vt IOAccelDisplayPipe --vt IOAccelDisplayPipeUserClient2 --vt IOFramebuffer --vt IGAccelDisplayMachine --vt IGAccelDisplayPipe --vt IntelAccelerator --vt AppleIntelFramebuffer --vt AppleIntelFramebufferController --vt IOAccelResource2 --vt IOAccelDisplayPipeTransaction2"
python3 kcdis.py "$OUT/30-family-displaymachine.txt" "$OUT" $A -- \
  'com.apple.iokit.IOAcceleratorFamily2:^IOAccelDisplayMachine::' \
  'com.apple.iokit.IOAcceleratorFamily2:^IOGraphicsAccelerator2::.*([Dd]isplay|[Ff]ramebuffer|newUserClient)' $VT > "$OUT/30-log.txt" 2>&1
python3 kcdis.py "$OUT/31-family-displaypipe.txt" "$OUT" $A -- \
  'com.apple.iokit.IOAcceleratorFamily2:^IOAccelDisplayPipe::(init|start|stop|free|probe|attach|createWorkLoop|copyCapabilities|framebuffer|getFramebuffer|setFramebuffer|newDisplayPipeTransaction|submitFlipBufferTransaction|enableVBLInterrupt|getDisplayModePipeScalerSetup|didPowerOn)' \
  'com.apple.iokit.IOAcceleratorFamily2:^IOAccelDisplayPipeUserClient2::(init|initWithTask|start|stop|externalMethod|s_|getDisplayPipe|clientClose)' $VT > "$OUT/31-log.txt" 2>&1
python3 kcdis.py "$OUT/32-intel-displaymachine.txt" "$OUT" $A -- \
  'com.apple.driver.AppleIntelICLGraphics:^IGAccelDisplayMachine::(init|start|stop|probeDisplayPipes|getPlaneAtIndex|getHardwareContext)' \
  'com.apple.driver.AppleIntelICLGraphics:^IGAccelDisplayPipe::(init|start|probeDisplayPipe|copyCapabilities|getScanoutOffset|submitFlipBufferTransaction|newDisplayPipeTransaction)' \
  'com.apple.driver.AppleIntelICLGraphics:^IntelAccelerator::(newDisplayMachine|getFramebuffer|.*[Dd]isplay)' $VT > "$OUT/32-log.txt" 2>&1
# which functions of the Intel framebuffer driver the accelerator / display pipe can call (names only)
grep -E "AppleIntelFramebuffer|FBCLASS|DisplayPipe|setTransaction|[Ff]lip" "$OUT"/10-com.apple.driver.AppleIntelICLLPGraphicsFramebuffer-symbols.txt > "$OUT/33-intel-fb-names.txt" 2>&1
grep -E "fSymFBCLASS|AppleIntelFramebuffer" "$OUT"/10-com.apple.driver.AppleIntelICLGraphics-symbols.txt > "$OUT/34-intel-accel-fb-imports.txt" 2>&1
ls -la "$OUT"
exit 0

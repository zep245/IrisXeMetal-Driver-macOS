#!/bin/bash
# Round 32 (v0.62.2): asynchronous command buffers. Today IrisXeAccel runs a command buffer's GPU work inside
# processCommandBuffer and returns only when the GPU has finished, so WindowServer's main thread waits ~5 ms per
# command buffer (about 60% of its display update time) and cannot prepare the next frame meanwhile. Our event
# machine's writeStamp does nothing: Apple's queue must complete the command buffer itself after we return. To
# return early and complete it later (from the GPU's interrupt), this round reads how IOAccelCommandQueue submits and
# retires a command buffer and how the event machine's stamps decide that it is complete - and how Apple's
# paravirtual GPU (asynchronous: the host runs the work) writes and signals its stamps. Text output only.
# Round 31 is kept as run31.sh.
set -x
OUT="$PWD/out"; mkdir -p "$OUT"
sw_vers > "$OUT/00-versions.txt" 2>&1
pip3 install --break-system-packages capstone > "$OUT/01-capstone.txt" 2>&1 || pip3 install capstone >> "$OUT/01-capstone.txt" 2>&1
KC=/System/Library/KernelCollections
A="$KC/BootKernelExtensions.kc $KC/SystemKernelExtensions.kc"
IDS="com.apple.iokit.IOAcceleratorFamily2 com.apple.driver.AppleParavirtGPU"
python3 kcdump.py "$OUT" $A -- $IDS > "$OUT/03-kcdump-stdout.txt" 2>&1
for f in "$OUT"/12-*-vtables.txt "$OUT"/10-*-symbols.txt; do c++filt < "$f" > "$f.tmp" && mv "$f.tmp" "$f"; done
rm -f "$OUT"/11-*-segments.txt
VT="--vt IOAccelCommandQueue --vt IOAccelSubmitter2 --vt IOAccelEventMachine2 --vt IOAccelEventMachineFast2 --vt IOGraphicsAccelerator2 --vt AppleParavirtAccelerator --vt AppleParavirtEventMachine --vt IOAccelContext2"
F=com.apple.iokit.IOAcceleratorFamily2
P=com.apple.driver.AppleParavirtGPU
# 1. the command queue: submit, process, retire
python3 kcdis.py "$OUT/a0-queue.txt" "$OUT" $A -- \
  "$F:^IOAccelCommandQueue::(processCommandBuffer|retireCommandBuffer|submitCommandBuffer|s_submit_command_buffer|_submit|submit|commandQueueStart|commandQueueStop|canSubmitCommandBuffer|pauseSubmitCommandBuffer|processKernelCommands|processSegmentKernelCommands|beginCoalescedSegment|endCoalescedSegment|setPriority)" \
  "$F:^IOAccelSubmitter2::(retireCommandBuffer|setSubmissionError|submit|isOpportunisticWorkload)" $VT > "$OUT/a0-log.txt" 2>&1
# 2. the event machine: stamps, fences, completion
python3 kcdis.py "$OUT/a1-events.txt" "$OUT" $A -- \
  "$F:^IOAccelEventMachineFast2::(finishStamp|finishAllStamps|testStamp|testAllStamps|testAllStampsUnlocked|incrementStamp|getStamp|setEventStamp|writeStampCommand|writeEventStampCommand|stampIsIncremented|checkGPUProgress|checkChannelProgress|eventTimeout|init|enableEventStampInterrupts|disableEventStampInterrupts|testEvent|finishEvent)" \
  "$F:^IOAccelEventMachine2::(signalStamp|stampInterrupt|eventStampInterrupt|processStamps|setStampBaseAddress|getStampBaseAddress|collectAllLiveEventIfNeeded|retire|notify|wakeup|signal)" \
  "$F:^IOAccelEventFence::" "$F:^IOAccelChannel2::(incrementStamp|getStamp|init)" $VT > "$OUT/a1-log.txt" 2>&1
# 3. the accelerator: who is told when the GPU finished (interrupt -> stamps -> waiting threads)
python3 kcdis.py "$OUT/a2-accel.txt" "$OUT" $A -- \
  "$F:^IOGraphicsAccelerator2::(signalStamp|stampInterrupt|eventInterrupt|handleInterrupt|interruptOccurred|getStampMemory|blockFenceEvent|notifyStamp|processStampNotification|stampUpdate|checkStamps|tickle)" $VT > "$OUT/a2-log.txt" 2>&1
# 4. Apple's paravirtual GPU: its event machine, queue and interrupt (the asynchronous example)
python3 kcdis.py "$OUT/a3-paravirt.txt" "$OUT" $A -- \
  "$P:^AppleParavirtEventMachine::" \
  "$P:^AppleParavirtCommandQueue::" \
  "$P:^AppleParavirtAccelerator::(interruptOccurred|handleInterrupt|interrupt|getStampMemory|newEventMachine|newCommandQueue|stamp|processCompletion|submit)" $VT > "$OUT/a3-log.txt" 2>&1
# the class names of both, and the family's log strings about stamps / retiring
grep -a "^vtable for" "$OUT"/12-*-vtables.txt | sed 's/ @.*//' > "$OUT/a4-classes.txt" 2>&1
grep -a -i "stamp\|retire\|fence\|submitCommandBuffer\|processCommandBuffer\|completion" "$OUT"/13-*-strings.txt > "$OUT/a5-strings.txt" 2>&1
grep -a -i "stamp\|retire\|fence\|Completion\|CommandQueue\|submit" "$OUT"/10-*-symbols.txt > "$OUT/a6-symbols.txt" 2>&1
rm -f "$OUT"/13-*-strings.txt
ls -la "$OUT"
exit 0

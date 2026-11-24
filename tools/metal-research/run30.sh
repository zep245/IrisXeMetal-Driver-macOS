#!/bin/bash
# Round 30 (v0.56.2): the v0.56.2 test kept CoreDisplay's Info messages: every transaction ends with
# "Failed to submit transaction: IOPRESENTMENT_METHOD_FAILURE: IOReturn: 0x2d8" and
# "WSDisplayPipeTransactionEnd returned error e00002d8" (kIOReturnNotReady), while the kernel never reached
# IOAccelDisplayPipe::transaction_end (our submit count 0). So IOAccelDisplayPipeUserClient2::transactionEnd itself
# answers "not ready". This round reads it and every check it makes (canUseDisplayPipe, setPipeIndex, online state).
# Text output only. Round 29 is kept as run29.sh.
set -x
OUT="$PWD/out"; mkdir -p "$OUT"
sw_vers > "$OUT/00-versions.txt" 2>&1
pip3 install --break-system-packages capstone > "$OUT/01-capstone.txt" 2>&1 || pip3 install capstone >> "$OUT/01-capstone.txt" 2>&1
KC=/System/Library/KernelCollections
A="$KC/BootKernelExtensions.kc $KC/SystemKernelExtensions.kc"
IDS="com.apple.iokit.IOAcceleratorFamily2"
python3 kcdump.py "$OUT" $A -- $IDS > "$OUT/03-kcdump-stdout.txt" 2>&1
for f in "$OUT"/12-*-vtables.txt "$OUT"/10-*-symbols.txt; do c++filt < "$f" > "$f.tmp" && mv "$f.tmp" "$f"; done
rm -f "$OUT"/11-*-segments.txt "$OUT"/13-*-strings.txt
VT="--vt IOAccelDisplayPipe --vt IOAccelDisplayPipeUserClient2 --vt IOAccelDisplayPipeTransaction2 --vt IOGraphicsAccelerator2"
python3 kcdis.py "$OUT/l0-userclient.txt" "$OUT" $A -- \
  'com.apple.iokit.IOAcceleratorFamily2:^IOAccelDisplayPipeUserClient2::(transactionEnd|transactionBegin|transactionWait|canUseDisplayPipe|setPipeIndex|externalMethod|start|attach|clientClose|requestNotify)' \
  'com.apple.iokit.IOAcceleratorFamily2:^IOAccelDisplayPipe::(isActive|online|offline|setOnline|setOffline|display_change|framebuffer_|isOnline|getOnline|setActive|is_ready|setPipeIndex|attach_user_client|detach_user_client|userClient)' $VT > "$OUT/l0-log.txt" 2>&1
ls -la "$OUT"
exit 0

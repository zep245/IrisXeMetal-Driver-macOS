#!/bin/bash
# Every IOAcceleratorFamily2 symbol our kext references must exist in Apple's
# kext (macOS 15). A wrong parameter type in a generated header gives a
# different mangled name -> WRONG (build fails).
# Symbols that exist but are internal (not exported) are reported as INTERNAL:
# the kext linker fills those vtable slots from the parent class's vtable
# (being verified on hardware in step 1b).
bin="$1"; d="$(dirname "$0")/data"
wrong=0; internal=0
while IFS= read -r sym; do
  if grep -qxF "$sym" "$d/IOAcceleratorFamily2.exports.txt"; then continue; fi
  if grep -qxF "$sym" "$d/IOAcceleratorFamily2.all.txt"; then echo "INTERNAL: $sym"; internal=$((internal+1));
  else echo "WRONG: $sym"; wrong=$((wrong+1)); fi
done < <(nm -u "$bin" | c++filt | grep -E "IOGraphicsAccelerator2|IOAccel" | sed 's/^ *//')
total=$(nm -u "$bin" | c++filt | grep -cE "IOGraphicsAccelerator2|IOAccel")
echo "$total family symbols used: $wrong wrong, $internal internal"
[ "$wrong" -eq 0 ]

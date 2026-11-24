#!/bin/bash
# v0.52.0: every AppleGraphicsDeviceControl symbol IrisXeAGDC references must be exported by Apple's
# kext (list from research round 16, macOS 15). A wrong parameter type in our hand-written header
# gives a different mangled name -> WRONG (build fails).
bin="$1"; d="$(dirname "$0")/data"
wrong=0
while IFS= read -r sym; do
  if grep -qxF "$sym" "$d/AppleGraphicsDeviceControl.exports.txt"; then echo "ok: $sym"; continue; fi
  echo "WRONG: $sym"; wrong=$((wrong+1))
done < <(nm -u "$bin" | c++filt | grep -E "AppleGraphicsDeviceControl|AGDC" | sed 's/^ *//')
echo "AGDC symbols used: $wrong wrong"
[ "$wrong" -eq 0 ]

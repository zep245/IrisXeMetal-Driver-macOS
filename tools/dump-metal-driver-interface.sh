#!/bin/bash
# Lists the public interface (exported symbols, Objective-C classes/methods,
# linked libraries) of Apple's Ice Lake Metal driver and kexts. It records names
# only - no Apple code is copied. Run on your Mac:
#   bash tools/dump-metal-driver-interface.sh
# Output: ~/Desktop/icl-interface.txt

OUT="$HOME/Desktop/icl-interface.txt"
EXT=/System/Library/Extensions
: > "$OUT"

echo "macOS: $(sw_vers -productVersion) ($(sw_vers -buildVersion))" >> "$OUT"

for item in "$EXT"/AppleIntelICL*; do
    [ -e "$item" ] || continue
    name=$(basename "$item")
    bin="$item/Contents/MacOS/${name%.*}"
    echo "" >> "$OUT"
    echo "================ $name" >> "$OUT"
    if [ ! -f "$bin" ]; then
        echo "(no binary on disk - probably inside the dyld shared cache)" >> "$OUT"
        continue
    fi
    echo "--- linked libraries" >> "$OUT"
    otool -L "$bin" >> "$OUT" 2>&1
    echo "--- exported symbols (demangled)" >> "$OUT"
    nm -gU "$bin" 2>/dev/null | c++filt >> "$OUT"
    echo "--- imported symbols" >> "$OUT"
    nm -u "$bin" 2>/dev/null | c++filt >> "$OUT"
    echo "--- Objective-C classes and methods" >> "$OUT"
    otool -ov "$bin" 2>/dev/null | grep -E "^\s*(name|types) " >> "$OUT"
done

echo "Done: $OUT ($(wc -l < "$OUT") lines)"

#!/bin/bash
# Round 11 (milestone 3c): can a stock LLVM read Metal's AIR? If yes, our
# shader compiler can use LLVM's bitcode reader and only needs its own Iris Xe
# backend. Uses our own test kernels only (kernels.metal).
set -x
OUT="$PWD/out"; mkdir -p "$OUT"
{ xcrun metal --version; sw_vers; } > "$OUT/80-versions.txt" 2>&1
# 1. AIR as the offline compiler makes it (text + bitcode)
xcrun -sdk macosx metal -c kernels.metal -o "$OUT/kernels.air" > "$OUT/81-metal.txt" 2>&1
xcrun -sdk macosx metal -S -emit-llvm kernels.metal -o "$OUT/kernels.apple.ll" >> "$OUT/81-metal.txt" 2>&1
# 2. AIR as Metal keeps it at run time (MTLFunction bitcodeData)
xcrun clang -fobjc-arc -O1 -framework Foundation -framework Metal air_probe.m -o air_probe > "$OUT/82-build.txt" 2>&1
./air_probe "$OUT/runtime" > "$OUT/83-air-probe.txt" 2>&1
# 3. stock LLVM (Homebrew) reading both
brew install llvm > "$OUT/84-brew.txt" 2>&1
LLVM="$(brew --prefix llvm)/bin"
"$LLVM/llvm-dis" --version > "$OUT/85-llvm-dis.txt" 2>&1
for f in "$OUT"/kernels.air "$OUT"/runtime.*.bc; do
  [ -f "$f" ] || continue
  echo "== $f" >> "$OUT/85-llvm-dis.txt"
  "$LLVM/llvm-dis" "$f" -o "$f.ll" >> "$OUT/85-llvm-dis.txt" 2>&1 && echo "OK -> $f.ll" >> "$OUT/85-llvm-dis.txt"
  "$LLVM/llvm-bcanalyzer" "$f" 2>&1 | head -40 >> "$OUT/86-bcanalyzer.txt"
done
# keep text IR only (the .air/.bc bitcode is ours too, but text is enough)
rm -f "$OUT"/*.air "$OUT"/*.bc
ls -la "$OUT"
exit 0

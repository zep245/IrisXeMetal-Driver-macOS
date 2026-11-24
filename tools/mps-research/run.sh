#!/bin/bash
# Research (v0.51.10, v0.51.11): disassemble MPSAutoCache::MPSAutoCache and MPSDevice lookups from this runner's macOS,
# to see what Metal Performance Shaders expects of a Metal driver. Writes build/irisxe-mps-research.txt.
out=${1:-build/irisxe-mps-research.txt}
d=$(mktemp -d)
{
  echo "== macOS: $(sw_vers -productVersion) ($(sw_vers -buildVersion)), $(uname -m)"
  clang -fobjc-arc -framework Foundation -framework Metal -framework MetalPerformanceShaders \
        "$(dirname "$0")/probe.m" -o "$d/probe" && "$d/probe"
  echo "== lldb"
  perl -e 'alarm 900; exec @ARGV' lldb -b -s "$(dirname "$0")/lldb-cmds.txt" "$d/probe" 2>&1
} > "$out" 2>&1
echo "wrote $out ($(wc -l < "$out") lines)"
exit 0

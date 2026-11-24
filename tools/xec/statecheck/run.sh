#!/bin/bash
# Compares IrisXeCore's packSurfaceState / packSamplerState (src/IrisXeGPU.cpp)
# with Mesa's generated genxml packers (gen120_pack.h from a Mesa build).
# Usage: run.sh <mesa source dir> <mesa build dir>
set -e
M=${1:-/home/claude/ext/mesasrc/mesa-25.2.8}; MB=${2:-$M/b}
H=$(cd "$(dirname "$0")" && pwd); R=$H/../../..; T=${TMPDIR:-/tmp}/xec_statecheck; mkdir -p $T
python3 - "$R" "$T" <<'PY'
import sys
r, t = sys.argv[1], sys.argv[2]
s = open(r + '/src/IrisXeSpaceABI.h').read()
a = s.index('struct XeTexDesc'); b = s.index('};', s.index('struct XeSampDesc')) + 2
part = s[a:b].replace('struct XeTexDesc', 'typedef struct XeTexDesc').replace('};\nstruct XeSampDesc', '} XeTexDesc;\ntypedef struct XeSampDesc')
open(t + '/abi_part.h', 'w').write(part[:-2] + '} XeSampDesc;\n')
g = open(r + '/src/IrisXeGPU.cpp').read()
a = g.index('// RENDER_SURFACE_STATE (Gen12'); b = g.index('\n}', g.index('static void packSamplerState')) + 2
open(t + '/pack_part.h', 'w').write(g[a:b].replace('const XeTexDesc &t', 'XeTexDesc t').replace('const XeSampDesc &d', 'XeSampDesc d').replace('volatile UInt32 *', 'UInt32 *'))
PY
gcc -std=gnu11 -I$T -I$MB/src/intel/genxml -I$M/src/intel/genxml -I$M/src -I$M/include -I$M/src/util $H/check_states.c -o $T/check -lm
$T/check

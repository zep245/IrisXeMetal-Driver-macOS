#!/bin/bash
# Needs clang-18, llvm-as-18 and /tmp/xec_test (tools/xec/xec_test.cpp built with LLVM 18).
# The C kernel takes (in, out, n, gid) like big_mix; see tools/xec/README.md.
# c_vs_sim.sh file.c func : native reference vs our compiler in the simulator (rolled loops, -O2)
set -e
f=$1; fn=$2; d=${TMPDIR:-/tmp}/xec_c_vs_sim; mkdir -p $d
cat > $d/main.c <<C
#include <stdio.h>
typedef unsigned int uint;
void $fn(const uint *in, uint *out, const uint *np, uint gid);
int main(void) { uint in[256], out[1]; for (int i = 0; i < 256; i++) in[i] = (uint)(i * 2654435761u + 12345u);
  uint ns[3] = {0, 1, 9}; for (int k = 0; k < 3; k++) for (int i = 0; i < 100; i++) { $fn(in, out, &ns[k], i); printf("%u\n", out[-i + i]); } }
C
sed 's/out\[gid\]/out[0]/' $f > $d/nat.c
clang-18 -O1 -DG= -DCN= -w -include /dev/null $d/nat.c $d/main.c -o $d/nat 2>&1 | head -3
sed -i 's/^#define G .*/#define G/; s/^#define CN .*/#define CN/' $d/nat.c; clang-18 -O1 -w $d/nat.c $d/main.c -o $d/nat
$d/nat > $d/ref.txt
clang-18 -O2 -fno-vectorize -fno-slp-vectorize -fno-unroll-loops -S -emit-llvm -target x86_64-apple-macos $f -o $d/t.ll
python3 - $d/t.ll $fn <<'PY'
import sys,re
p=sys.argv[1]; fn=sys.argv[2]; s=open(p).read()
s=re.sub(r'^attributes #.*$','',s,flags=re.M); s=re.sub(r' #\d+','',s)
s+='''
!air.kernel = !{!900}
!902 = !{}
!903 = !{!904, !905, !906, !907}
!904 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1}
!905 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1}
!906 = !{i32 2, !"air.buffer", !"air.location_index", i32 2, i32 1}
!907 = !{i32 3, !"air.thread_position_in_grid"}
!900 = !{ptr @%s, !902, !903}
''' % fn
open(p,'w').write(s)
PY
llvm-as-18 $d/t.ll -o $d/t.bc
/tmp/xec_test $d/t.bc $fn $d/k.bin
python3 - $d "$(dirname "$0")" <<'PY'
import sys; sys.path.insert(0, sys.argv[2])
from xesim import run_kernel, load_bin
d = sys.argv[1]; M = 0xffffffff
ref = [int(l) for l in open(d + '/ref.txt')]
inp = [(i * 2654435761 + 12345) & M for i in range(256)]
code = load_bin(d + '/k.bin'); ok = True
for k, n in enumerate((0, 1, 9)):
    mem, st = run_kernel(code, 100, 64, [(0x100000, inp), (0x200000, [0xEEEEEEEE] * 136), (0x300000, [n]), (0x400000, [0]), (0x500000, [0]), (0x1000000, [])])
    bad = [i for i in range(100) if mem[0x200000 + 4 * i] != ref[k * 100 + i]]
    print('n=%d %s' % (n, 'OK' if not bad else 'WRONG %d: lane %d got %#x want %#x' % (len(bad), bad[0], mem[0x200000 + 4 * bad[0]], ref[k * 100 + bad[0]])))
PY

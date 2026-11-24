#!/usr/bin/env python3
"""v0.29.0: the big_mix and wide_live kernels of tools/metaltest/kernels.metal
(compiled with xec_test from clang -O2 IR of the same code, or from
irisxe-kernels.ll) in the simulator, against the test tool's CPU reference.
Usage: sim_big.py <dir with big_mix.bin, wide_live.bin>"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xesim import run_kernel, load_bin
D = sys.argv[1] if len(sys.argv) > 1 else '/tmp/bg'
M = 0xffffffff
def big_mix(x, n):
    a = []
    for i in range(40):
        x = (x * 1664525 + 1013904223 + i) & M; a.append(x ^ (x >> 13))
    t = a[0]
    for i in range(300):
        b = a[(i * 7) % 40]
        if i % 3 == 0: t = (t * 3 + b) & M
        elif i % 3 == 1: t = ((t ^ b) + (t >> 5)) & M
        else: t = ((t - b) & M) ^ ((t << 3) & M)
    p, q, w = t, 5, 0
    for j in range(n):
        pa = (p + a[3] * j) & M
        qx = q ^ (a[17] >> (j & 7))
        if pa & 1: qx = (qx + a[29]) & M; w += 1
        q = qx; p = pa ^ q
    r = ((p + q) & M) ^ w
    for i in range(40): r = ((r ^ a[i]) + (r >> 7)) & M
    return r
def wide_live(x, n):
    a = []
    for i in range(64):
        x = ((x ^ ((x << 5) & M)) + 0x9e3779b9 * (i + 1)) & M; a.append(x)
    s = 0
    for j in range(n):
        for i in range(0, 64, 8): s = ((s + a[i] * (j + 1)) & M) ^ (a[i + 3] >> (j & 15))
    for i in range(64): s ^= (a[i] * (i + 1)) & M
    return s
A, B, C = 0x100000, 0x200000, 0x300000
SCRATCH = 0x1000000                    # buffer slot 5: private arrays (written before read)
inp = [(i * 2654435761 + 12345) & M for i in range(256)]
ok = True
for name, ref in (('big_mix', big_mix), ('wide_live', wide_live)):
    code = load_bin(os.path.join(D, name + '.bin'))
    for n in (0, 1, 9):
        try:
            mem, st = run_kernel(code, 100, 64, [(A, inp), (B, [0xEEEEEEEE] * 136), (C, [n]),
                                                 (0x400000, [0]), (0x500000, [0]), (SCRATCH, [])])
        except RuntimeError as e:
            print('%-10s n=%d FAILED: %s' % (name, n, e)); ok = False; continue
        bad = [i for i in range(100) if mem[B + 4 * i] != ref(inp[i], n)]
        bad += [i for i in range(100, 136) if mem[B + 4 * i] != 0xEEEEEEEE]
        print('%-10s n=%d %s (%d instructions, %d run)' % (name, n, 'OK' if not bad else 'WRONG %d, first lane %d' % (len(bad), bad[0]), len(code), st))
        ok &= not bad
print('ALL OK' if ok else 'FAILED')
sys.exit(0 if ok else 1)

#!/usr/bin/env python3
"""Runs the Apple-compiled 3e kernels (irisxe-kernels.ll -> xec_test .bin files)
in xesim with the same inputs and checks as irisxe-metaltest --types.
Usage: sim_apple.py <dir with math_f.bin, div_mod.bin, bit_ops.bin, vec_ops.bin>"""
import sys, os, struct, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xesim import run_kernel, load_bin
D = sys.argv[1] if len(sys.argv) > 1 else '/tmp/k3'
N = 256
BUF = [0x100000 * (k + 1) for k in range(4)]
def f2u(x): return struct.unpack('<I', struct.pack('<f', x))[0]
def u2f(x): return struct.unpack('<f', struct.pack('<I', x & 0xffffffff))[0]
def f32(x): return u2f(f2u(x))
def run(name, b0):
    code = load_bin(os.path.join(D, name + '.bin'))
    bufs = [(BUF[0], b0)] + [(BUF[k], [0xEEEEEEEE] * (N * 4 + 64)) for k in (1, 2, 3)]
    mem, steps = run_kernel(code, N, 64, bufs)
    return lambda k, i: mem.get(BUF[k] + 4 * i), steps
def report(name, bad, checked, steps):
    print('%-12s %s (%d checked, %d instructions run)' % (name, 'OK' if not bad else 'WRONG %d, first %s' % (len(bad), bad[0]), checked, steps))
    return not bad
ok = True
# math_f
x0 = [f32(0.25 + (i % 97) * 0.37) for i in range(N * 4)]
got, st = run('math_f', [f2u(x) for x in x0])
bad = []
for i in range(N):
    x = x0[i]
    w = [math.sqrt(x) + 1 / math.sqrt(x + 1) + x / (x + 3), math.sin(x) * math.cos(x * 0.5),
         2 ** (x * 0.1) + math.log2(x + 1) + math.exp(x * 0.05) + math.log(x + 2),
         x ** 1.5 + math.floor(x * 1.7) + math.ceil(x * 0.3) + (x * 3.3 - math.floor(x * 3.3)) + math.trunc(-x * 0.7)]
    for k in range(4):
        g = u2f(got(1, 4 * i + k) or 0)
        if not abs(g - w[k]) <= max(2e-4, 2e-4 * abs(w[k])): bad.append('[%d.%d]=%g want %g' % (i, k, g, w[k]))
ok &= report('math_f', bad, N * 4, st)
# div_mod
i0 = [((i * 7919) % 20001) - 10000 for i in range(N * 4)]
got, st = run('div_mod', [x & 0xffffffff for x in i0])
def cdiv(a, b): q = abs(a) // abs(b); return -q if (a < 0) != (b < 0) else q
def crem(a, b): return a - cdiv(a, b) * b
bad = []
for i in range(N):
    x, y = i0[i], i % 13 + 1; ux = x & 0xffffffff
    w = [cdiv(x, y), crem(x, y), ux // 7 + ux % 16, cdiv(x, 5) + crem(x, 3)]
    for k in range(4):
        if got(1, 4 * i + k) != w[k] & 0xffffffff: bad.append('[%d.%d]=%s want %d' % (i, k, got(1, 4 * i + k), w[k]))
ok &= report('div_mod', bad, N * 4, st)
# bit_ops
u0 = [0 if i % 31 == 0 else ((i * 2654435761) & 0xffffffff) >> (i % 7) for i in range(N * 4)]
got, st = run('bit_ops', u0)
bad = []
for i in range(N):
    x = u0[i]
    w = [bin(x).count('1'), 32 - x.bit_length(), (x & -x).bit_length() - 1 if x else 32, int('{:032b}'.format(x)[::-1], 2)]
    for k in range(4):
        if got(1, 4 * i + k) != w[k]: bad.append('[%d.%d] of %08X=%s want %d' % (i, k, x, got(1, 4 * i + k), w[k]))
ok &= report('bit_ops', bad, N * 4, st)
print('ALL OK' if ok else 'FAILED')
sys.exit(0 if ok else 1)

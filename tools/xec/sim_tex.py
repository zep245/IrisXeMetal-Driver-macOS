#!/usr/bin/env python3
"""Milestone 4: Apple-compiled texture kernels (kernels.metal tex_*, from
irisxe-kernels.ll, compiled with xec_test) in the simulator.
Usage: sim_tex.py <dir with tex_*.bin>"""
import sys, os, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xesim import run_kernel, load_bin, Tex
D = sys.argv[1] if len(sys.argv) > 1 else '/tmp/k4'
def code(n): return load_bin(os.path.join(D, n + '.bin'))
def f2u(x): return struct.unpack('<I', struct.pack('<f', x))[0]
def u2f(x): return struct.unpack('<f', struct.pack('<I', x & 0xffffffff))[0]
def h(x): return struct.unpack('<e', struct.pack('<e', x))[0]
ok = True
def report(name, bad, n, st):
    global ok
    print('%-16s %s (%d checked, %d instructions run)' % (name, 'OK' if not bad else 'WRONG %d, first %s' % (len(bad), bad[0]), n, st))
    ok &= not bad
B = 0x100000
W = 64
src = {(x, y, 0): (x * 0.25, y * 0.5, (x ^ y) * 0.125, 1.0) for x in range(W) for y in range(W)}
# tex_copy: dst = src * 0.5 (64 x 64, groups of 64 in x, 64 group rows)
t0, t1 = Tex(W, W, 'rgba32f', data=dict(src)), Tex(W, W, 'rgba32f')
_, st = run_kernel(code('tex_copy'), W, 64, [], tex={0: t0, 1: t1}, groups_y=W)
bad = ['(%d,%d)' % (x, y) for x in range(W) for y in range(W) if t1.data.get((x, y, 0)) != tuple(c * 0.5 for c in src[(x, y, 0)])]
report('tex_copy', bad, W * W, st)
# tex_uint_rw: r32 uint read_write, +1
t = Tex(W, W, 'r32u', data={(x, y, 0): (x * 1000 + y, 0, 0, 0) for x in range(W) for y in range(W)})
_, st = run_kernel(code('tex_uint_rw'), W, 64, [], tex={0: t}, groups_y=W)
bad = ['(%d,%d)' % (x, y) for x in range(W) for y in range(W) if t.data[(x, y, 0)][0] != x * 1000 + y + 1]
report('tex_uint_rw', bad, W * W, st)
# tex_size: width * 65536 + height
t = Tex(100, 37, 'rgba32f')
mem, st = run_kernel(code('tex_size'), 50, 32, [(B, [0] * 64)], tex={0: t})
bad = [i for i in range(50) if mem[B + 4 * i] != 100 * 65536 + 37]
report('tex_size', bad, 50, st)
# tex_sample: (gid + 0.5) / 64, linear filter: exactly the texel
t = Tex(W, W, 'rgba32f', data=dict(src))
mem, st = run_kernel(code('tex_sample'), W, 64, [(B, [0] * (W * W * 4))], tex={0: t},
                     samp={0: {'filter': 'linear', 'normalized': True}}, groups_y=W)
bad = []
for y in range(W):
    for x in range(W):
        got = tuple(u2f(mem[B + 16 * (y * W + x) + 4 * k]) for k in range(4))
        if any(abs(got[k] - src[(x, y, 0)][k]) > 1e-4 for k in range(4)): bad.append('(%d,%d) %s' % (x, y, got))
report('tex_sample', bad, W * W, st)
# tex_half_write: (x / 64, y / 64, 0.25, 1) as half
t = Tex(W, W, 'rgba16f')
_, st = run_kernel(code('tex_half_write'), W, 64, [], tex={0: t}, groups_y=W)
bad = ['(%d,%d)' % (x, y) for x in range(W) for y in range(W) if t.data.get((x, y, 0)) != (h(x / 64), h(y / 64), 0.25, 1.0)]
report('tex_half_write', bad, W * W, st)
# tex_short_coord: read with ushort2 coordinates
t = Tex(W, W, 'rgba32f', data=dict(src))
mem, st = run_kernel(code('tex_short_coord'), W, 64, [(B, [0] * (W * W * 4))], tex={0: t}, groups_y=W)
bad = ['(%d,%d)' % (x, y) for y in range(W) for x in range(W)
       if tuple(u2f(mem[B + 16 * (y * W + x) + 4 * k]) for k in range(4)) != src[(x, y, 0)]]
report('tex_short_coord', bad, W * W, st)
# tex_array_write: layer = gid.z (z = 0 here: the simulator has one group layer)
t = Tex(16, 16, 'rgba32f', layers=4)
_, st = run_kernel(code('tex_array_write'), 16, 16, [], tex={0: t}, groups_y=16)
bad = ['(%d,%d)' % (x, y) for x in range(16) for y in range(16) if t.data.get((x, y, 0)) != (float(x), float(y), 0.0, 1.0)]
report('tex_array_write', bad, 256, st)
print('ALL OK' if ok else 'FAILED')
sys.exit(0 if ok else 1)

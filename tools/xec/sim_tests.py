#!/usr/bin/env python3
"""Runs kernels compiled by our compiler (xec_test) in xesim and compares with
Python references. Usage: sim_tests.py <dir with *.bin from xec_test>"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xesim import run_kernel, load_bin
D = sys.argv[1] if len(sys.argv) > 1 else '/tmp/xt'
A, B, C = 0x1000, 0x40000, 0x80000           # buffer GPU addresses
def u(x): return x & 0xffffffff
def s32(x): x &= 0xffffffff; return x - (1 << 32) if x >> 31 else x
N = 200
def check(name, grid, tpg, bufs, want):
    code = load_bin(os.path.join(D, name + '.bin'))
    mem, steps = run_kernel(code, grid, tpg, bufs)
    bad = [(a, mem.get(a), v) for a, v in want.items() if mem.get(a) != u(v)]
    print('%-13s grid %d tpg %d: %s (%d instructions run)' % (name, grid, tpg,
          'OK' if not bad else 'WRONG %d, first at %#x: got %s want %#x' % (len(bad), bad[0][0], hex(bad[0][1]) if bad[0][1] is not None else None, u(bad[0][2])), steps))
    return not bad
ok = True
inp = [(k * 2654435761 >> 9) & 0xffff for k in range(4096)]
# add1
ok &= check('add1', N, 64, [(A, inp[:N + 8]), (B, [0xdeadbeef] * (N + 8))],
            {**{B + 4 * i: inp[i] + 1 for i in range(N)}, **{B + 4 * i: 0xdeadbeef for i in range(N, N + 8)}})
# loop_sum (n = 5)
n = 5
ok &= check('loop_sum', 100, 24, [(A, inp[:100 * n]), (B, [0] * 100), (C, [n])],
            {B + 4 * i: u(sum(inp[i * n + k] for k in range(n))) for i in range(100)})
ok &= check('loop_sum', 30, 24, [(A, inp[:100]), (B, [7] * 30), (C, [0])], {B + 4 * i: 0 for i in range(30)})
# collatz (in place)
def col(x):
    c = 0
    while x > 1: x = 3 * x + 1 if x & 1 else x // 2; c += 1
    return c
vals = [(k % 97) + 1 for k in range(64)]
ok &= check('collatz', 64, 20, [(A, vals)], {A + 4 * i: col(vals[i]) for i in range(64)})
# ifelse (cf.ll): x > 10 ? (x > 100 ? 3x + 1000 : 3x) : -x
sv = [((k * 7919) % 601) - 300 for k in range(64)]
def ie(x): return (3 * x + 1000 if x > 100 else 3 * x) if x > 10 else -x
ok &= check('ifelse', 64, 32, [(A, [u(x) for x in sv])], {A + 4 * i: u(ie(sv[i])) for i in range(64)})
# nested loops (nl.ll): s = sum_{a<n} sum_{b<=a} in[(i + 7a + b) & 1023] ^ b
n = 6
def nl(i): return u(sum(inp[(i + 7 * a + b) & 1023] ^ b for a in range(n) for b in range(a + 1)))
ok &= check('nested_loops', 40, 16, [(A, inp[:1024]), (B, [0] * 40), (C, [n])], {B + 4 * i: nl(i) for i in range(40)})
# ---- 3e ----
import struct, math
def f2u(x): return struct.unpack('<I', struct.pack('<f', x))[0]
def u2f(x): return struct.unpack('<f', struct.pack('<I', x & 0xffffffff))[0]
def close(got, want, rel=1e-5):
    g, w = u2f(got), u2f(want)
    return abs(g - w) <= rel * max(1.0, abs(w))
def checkf(name, grid, tpg, bufs, want, rel=1e-5, words=None):
    code = load_bin(os.path.join(D, name + '.bin'))
    mem, steps = run_kernel(code, grid, tpg, bufs)
    bad = [(a, mem.get(a), v) for a, v in want.items() if mem.get(a) is None or not close(mem[a], v, rel)]
    print('%-13s grid %d tpg %d: %s (%d instructions run)' % (name, grid, tpg, 'OK' if not bad else
          'WRONG %d, first at %#x: got %r want %r' % (len(bad), bad[0][0], u2f(bad[0][1] or 0), u2f(bad[0][2])), steps))
    return not bad
fv = [((k * 37) % 101) * 0.25 + 0.5 for k in range(4 * 40)]
want = {}
for i in range(40):
    v = fv[4 * i:4 * i + 4]; m = [v[0] * 2, v[1] * 3, v[2] * 4, v[3] * 5]
    sh = [m[3], m[2], v[1], m[0]]
    sh[1] = math.sqrt(sh[1])
    d = [sh[0] / 2, sh[1] / 4, sh[2] / 8, sh[3] / 16]
    for k in range(4): want[B + 16 * i + 4 * k] = f2u(d[k])
ok &= checkf('vec4', 40, 16, [(A, [f2u(x) for x in fv]), (B, [0] * 160)], want)
xs = [((k * 7919) % 2001) - 1000 for k in range(64)]
def cdiv(a, b): q = abs(a) // abs(b); return -q if (a < 0) != (b < 0) else q
def divs(i):
    x = xs[i] & 0xffffffff; sx = xs[i]; y = i + 3
    q = cdiv(sx, y); r = sx - q * y
    t = (q * 1000 + r) & 0xffffffff
    t ^= x // 7; t = (t + (x % 16)) & 0xffffffff
    t = (t + (bin(x).count('1') << 8) + ((32 - x.bit_length()) << 16)) & 0xffffffff
    return t
ok &= check('divs', 64, 24, [(A, [u(x) for x in xs]), (B, [0] * 64)], {B + 4 * i: divs(i) for i in range(64)})
# bytes: 8-bit loads/stores (4 per word)
bv = [(k * 53 + 7) & 0xff for k in range(64)]
def pack(bs): return [bs[i] | bs[i+1] << 8 | bs[i+2] << 16 | bs[i+3] << 24 for i in range(0, len(bs), 4)]
def by(x):
    sx = x - 256 if x > 127 else x
    y = (x * 3) & 0xff
    return (y + 100) & 0xff if sx < 0 else y
outb = [by(x) for x in bv]
ok &= check('bytes', 64, 16, [(A, pack(bv)), (B, [0] * 16)], {B + 4 * k: w for k, w in enumerate(pack(outb))})
# halfs
def h2u(x): return int.from_bytes(struct.pack('<e', x), 'little')
def u2h(v): return struct.unpack('<e', (v & 0xffff).to_bytes(2, 'little'))[0]
hv = [((k * 13) % 50) * 0.125 - 2.0 for k in range(64)]
def hs(x):
    y = u2h(h2u(x * u2h(0x3555))); z = u2h(h2u(y + 1.0))
    return h2u(z ** 2.0)
hpack = lambda hs_: [hs_[i] | hs_[i + 1] << 16 for i in range(0, len(hs_), 2)]
res = hpack([hs(x) for x in hv])
code = load_bin(os.path.join(D, 'halfs.bin'))
mem, steps = run_kernel(code, 64, 32, [(A, hpack([h2u(x) for x in hv])), (B, [0] * 32)])
bad = [k for k in range(32) if any(abs(u2h(mem[B + 4 * k] >> (16 * j)) - u2h(res[k] >> (16 * j))) > 2e-3 * max(1, abs(u2h(res[k] >> (16 * j)))) for j in (0, 1))]
print('%-13s grid 64 tpg 32: %s' % ('halfs', 'OK' if not bad else 'WRONG at words %s' % bad[:4]))
ok &= not bad
print('ALL OK' if ok else 'FAILED')
sys.exit(0 if ok else 1)

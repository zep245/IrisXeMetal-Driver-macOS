#!/usr/bin/env python3
"""Milestone 3f in the simulator: threadgroup memory, barriers, atomics.
Usage: sim_3f.py <dir with tg_reverse.bin, tg_sum.bin, atomic_hist.bin, tg_count.bin>
(compiled by xec_test from irisxe-kernels.ll or a hand-written AIR file)."""
import sys, os, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xesim import run_kernel, load_bin
D = sys.argv[1] if len(sys.argv) > 1 else '/tmp/f3'
A, B, C = 0x100000, 0x200000, 0x300000
def f2u(x): return struct.unpack('<I', struct.pack('<f', x))[0]
ok = True
def report(name, bad, checked, steps):
    global ok
    print('%-12s %s (%d checked, %d instructions run)' % (name, 'OK' if not bad else 'WRONG %d, first %s' % (len(bad), bad[0]), checked, steps))
    ok &= not bad
def code(n): return load_bin(os.path.join(D, n + '.bin'))
# tg_reverse: 8 groups of 64, reversed inside each group through threadgroup memory
fin = [f2u(i * 0.5 + 1) for i in range(512)]
mem, st = run_kernel(code('tg_reverse'), 512, 64, [(A, fin), (B, [0] * 512)], slm=256, barrier=True)
bad = ['[%d]' % i for i in range(512) if mem[B + 4 * i] != fin[(i // 64) * 64 + 63 - i % 64]]
report('tg_reverse', bad, 512, st)
# tg_sum: 4 groups of 256, tree reduction with a barrier in the loop
uin = [(i * 2654435761 >> 7) & 0xffff for i in range(1024)]
mem, st = run_kernel(code('tg_sum'), 1024, 256, [(A, uin), (B, [0] * 4)], slm=1024, barrier=True)
bad = ['group %d' % g for g in range(4) if mem[B + 4 * g] != sum(uin[g * 256:(g + 1) * 256]) & 0xffffffff]
report('tg_sum', bad, 4, st)
# atomic_hist: 1000 threads in groups of 64 (last group partial, no barrier)
mem, st = run_kernel(code('atomic_hist'), 1000, 64, [(A, uin[:1000]), (B, [0] * 16), (C, [0, 0, 0xffffffff])])
want = [sum(1 for v in uin[:1000] if v & 15 == b) for b in range(16)]
bad = ['bin %d' % b for b in range(16) if mem[B + 4 * b] != want[b]]
for k, w in enumerate([sum(v & 255 for v in uin[:1000]), max(uin[:1000]), min(uin[:1000])]):
    if mem[C + 4 * k] != w: bad.append('total[%d] = %d (want %d)' % (k, mem[C + 4 * k], w))
report('atomic_hist', bad, 19, st)
# tg_count: 1003 threads in groups of 128: barriers + a partial last group (107 threads)
mem, st = run_kernel(code('tg_count'), 1003, 128, [(A, uin[:1003]), (B, [0] * 8), (C, [0])], slm=4, barrier=True)
want = [sum(1 for v in uin[g * 128:min(1003, (g + 1) * 128)] if v & 1) for g in range(8)]
bad = ['group %d = %d (want %d)' % (g, mem[B + 4 * g], want[g]) for g in range(8) if mem[B + 4 * g] != want[g]]
if mem[C] != sum(want): bad.append('grand total %d (want %d)' % (mem[C], sum(want)))
report('tg_count', bad, 9, st)
# without IrisXeCore's split of the partial last group the GPU would hang:
try:
    run_kernel(code('tg_count'), 1003, 128, [(A, uin[:1003]), (B, [0] * 8), (C, [0])], slm=4, barrier=True, split=False)
    print('no-split    NOT DETECTED (expected a hang)'); ok = False
except RuntimeError as e:
    print('no-split    hang detected as expected: %s' % str(e)[:80])
print('ALL OK' if ok else 'FAILED')
sys.exit(0 if ok else 1)

#!/usr/bin/env python3
"""Runs the register-reuse stress kernels (gen_stress.py -> xec_test) in the
simulator and compares every lane with the Python reference.
Usage: sim_stress.py <dir with s_ref.py and big_chain.bin, live_loops.bin, wide_live.bin>"""
import sys, os, importlib.util
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xesim import run_kernel, load_bin
D = sys.argv[1] if len(sys.argv) > 1 else '/tmp/st'
spec = importlib.util.spec_from_file_location('sref', os.path.join(D, 's_ref.py'))
ref = importlib.util.module_from_spec(spec); spec.loader.exec_module(ref)
A, B, C = 0x100000, 0x200000, 0x300000
ok = True
inp = [(i * 2654435761 + 12345) & 0xffffffff for i in range(256)]
for name in ('big_chain', 'live_loops', 'wide_live'):
    code = load_bin(os.path.join(D, name + '.bin'))
    for N in (0, 1, 9, 17):
        try:
            mem, st = run_kernel(code, 72, 24, [(A, inp), (B, [0xEEEEEEEE] * 80), (C, [N])])
        except RuntimeError as e:
            print('%-11s n=%-2d FAILED: %s' % (name, N, e)); ok = False; continue
        bad = [i for i in range(72) if mem[B + 4 * i] != ref.run(name, inp, i, N)]
        bad += [i for i in range(72, 80) if mem[B + 4 * i] != 0xEEEEEEEE]
        print('%-11s n=%-2d %s (%d instructions run)' % (name, N, 'OK' if not bad else
              'WRONG %d, first lane %d: got %#x want %#x' % (len(bad), bad[0], mem[B + 4 * bad[0]], ref.run(name, inp, bad[0], N)), st))
        ok &= not bad
print('ALL OK' if ok else 'FAILED')
sys.exit(0 if ok else 1)

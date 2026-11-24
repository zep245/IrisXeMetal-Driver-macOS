import sys
import os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xesim import run_kernel, load_bin
code = load_bin(sys.argv[1] if len(sys.argv) > 1 else '/tmp/t2d/pos2d.bin')
A, B = 0x1000, 0x200000
ok = True
for (W, H, GW, GH) in [(64, 8, 256, 72), (8, 64, 136, 128), (5, 3, 23, 10), (16, 16, 40, 33), (128, 1, 300, 2)]:
    gx = (GW + W - 1) // W; gy = (GH + H - 1) // H
    out = [0xdeadbeef] * (gx * W * gy * H * 4 + 64)
    mem, st = run_kernel(code, GW, W, [(A, out), (B, [GW])], groups_y=gy, tpg_h=H, grid_y=GH)
    bad = 0
    for y in range(gy * H):
        for x in range(gx * W):
            base = A + 4 * ((y * GW + x) * 4)
            if x >= GW or y >= GH: continue       # outside: nothing (some of these addresses alias real rows)
            got = [mem.get(base + 4 * k) for k in range(4)]
            lx, ly = x % W, y % H
            want = [x, y, lx | ly << 8, ly * W + lx]
            if got != want:
                bad += 1
                if bad <= 3: print('   (%d,%d): got %s want %s' % (x, y, got, want))
    # nothing written past the grid's last row
    past = [a for a in range(A + 4 * (GH * GW * 4), A + 4 * len(out), 4) if mem.get(a) != 0xdeadbeef]
    print('groups of %dx%d over %dx%d: %s%s (%d instructions run)' % (W, H, GW, GH, 'OK' if not bad else 'WRONG %d' % bad,
          '' if not past else ', %d writes past the grid' % len(past), st))
    ok &= not bad and not past
print('ALL OK' if ok else 'FAILED')

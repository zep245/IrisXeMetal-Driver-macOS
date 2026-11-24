#!/usr/bin/env python3
"""xesim - a small SIMD8 simulator for the code our compiler emits (milestone
3d), so kernels can be run and checked on any machine before they reach the
GPU. It decodes the Gfx12 bit fields of src/compiler/xe_enc.h and models the
instructions we generate: ALU (mov/add/mul/shl/shr/asr/and/or/xor/not/cmp/sel),
data-port reads/writes (surface 253), sync, EOT, and the structured control
flow IF/ELSE/ENDIF/BREAK/WHILE with per-lane masks. Not a general simulator.

    xesim.py kernel.bin --grid N --tpg T --buf0 file --buf1 file ...  (see run())
"""
import struct, sys

def bits(w, hi, lo): return (w >> lo) & ((1 << (hi - lo + 1)) - 1)
def sx(v, n): return v - (1 << n) if v >> (n - 1) & 1 else v

UB, UW, UD, B, W, D, HF, F = 0, 1, 2, 4, 5, 6, 9, 10
SIZE = {UB: 1, B: 1, UW: 2, W: 2, UD: 4, D: 4, HF: 2, F: 4}
VS = [0, 1, 2, 4, 8, 16, 32]; WD = [1, 2, 4, 8, 16]; HS = [0, 1, 2, 4]

def f2u(x): return struct.unpack('<I', struct.pack('<f', x))[0]
def u2f(u): return struct.unpack('<f', struct.pack('<I', u & 0xffffffff))[0]

class Tex:
    """milestone 4: a texture for the simulator. fmt: 'rgba32f', 'rgba16f', 'rgba8' (unorm),
    'r32u', 'r32f', 'rgba32u'; data[(x, y, layer)] = 4-tuple (floats or ints)"""
    def __init__(self, w, h, fmt, layers=1, data=None):
        self.w, self.h, self.fmt, self.layers = w, h, fmt, layers
        self.data = data if data is not None else {}
    def isint(self): return self.fmt in ('r32u', 'rgba32u')
    def get(self, x, y, z=0):
        if not (0 <= x < self.w and 0 <= y < self.h and 0 <= z < self.layers):
            return (0, 0, 0, 0) if self.isint() else (0.0, 0.0, 0.0, 0.0)   # out of bounds: zero
        v = self.data.get((x, y, z), (0, 0, 0, 0))
        if self.fmt in ('r32u', 'r32f'): v = (v[0], 0, 0, 1) if self.isint() else (v[0], 0.0, 0.0, 1.0)
        return v
    def put(self, x, y, z, v):
        if not (0 <= x < self.w and 0 <= y < self.h and 0 <= z < self.layers): return   # discarded
        if self.fmt == 'rgba8': v = tuple(round(min(max(c, 0.0), 1.0) * 255) / 255 for c in v)
        elif self.fmt == 'rgba16f': v = tuple(struct.unpack('<e', struct.pack('<e', c))[0] for c in v)
        elif self.isint(): v = tuple(int(c) & 0xffffffff for c in v)
        self.data[(x, y, z)] = v

class Sim:
    def __init__(self, code, mem, slm=None, slmSize=0):
        self.code = code; self.mem = mem
        self.slm = slm if slm is not None else {}       # 3f: the thread group's shared local memory
        self.slmSize = slmSize
        self.arrived = False                            # sent its barrier message, not yet past sync.bar
        self.sr0 = 0                                    # state register: hardware thread id fields
        self.tex = {}                                   # milestone 4: binding table index -> Tex
        self.samp = {}                                  # sampler index -> dict(min, mag, addr, normalized)
        self.grf = bytearray(128 * 32)
        self.flag = [[False] * 8, [False] * 8]          # f0.0, f1.0 (we only use .0 subregs)
        self.steps = 0

    # ---- register access ----
    def rd(self, nr, sub, t):
        off = nr * 32 + sub; n = SIZE[t]
        v = int.from_bytes(self.grf[off:off + n], 'little')
        if t == F: return u2f(v)
        if t == HF: return struct.unpack('<e', v.to_bytes(2, 'little'))[0]
        if t in (B, W, D): return sx(v, n * 8)
        return v
    def wr(self, nr, sub, t, v):
        off = nr * 32 + sub; n = SIZE[t]
        if t == F: v = f2u(v)
        if t == HF:
            try: v = int.from_bytes(struct.pack('<e', v), 'little')
            except OverflowError: v = 0x7c00 if v > 0 else 0xfc00
        self.grf[off:off + n] = (int(v) & ((1 << (n * 8)) - 1)).to_bytes(n, 'little')

    def src(self, w, which, lane):
        if which == 0:
            t = bits(w, 43, 40); isimm = bits(w, 46, 46)
            if isimm:
                v = bits(w, 127, 96)
                if t == 4 and bits(w, 39, 36) in (UW, W):          # V: 8 x 4-bit signed
                    return sx((v >> (4 * lane)) & 0xf, 4)
                return self.immval(v, t)
            nr, sub = bits(w, 79, 72), bits(w, 71, 67)
            vs, wd, hs = VS[bits(w, 87, 84)], WD[bits(w, 83, 81)], HS[bits(w, 65, 64)]
            neg, ab = bits(w, 45, 45), bits(w, 44, 44)
        else:
            t = bits(w, 91, 88)
            if bits(w, 47, 47): return self.immval(bits(w, 127, 96), t)
            nr, sub = bits(w, 111, 104), bits(w, 103, 99)
            vs, wd, hs = VS[bits(w, 119, 116)], WD[bits(w, 115, 113)], HS[bits(w, 97, 96)]
            neg, ab = bits(w, 121, 121), bits(w, 120, 120)
        if not bits(w, 66 if which == 0 else 98, 66 if which == 0 else 98):
            if which == 0 and nr == 0x70 and sub == 0: return self.sr0    # state register sr0.0
            raise RuntimeError('ARF source not supported')
        e = SIZE[t]
        elem = (lane // wd) * vs + (lane % wd) * hs
        b = sub + elem * e
        v = self.rd(nr + b // 32, b % 32, t)
        if ab: v = abs(v)
        if neg: v = -v
        return v
    def immval(self, v, t):
        if t == F: return u2f(v)
        if t in (UW, W): v &= 0xffff; return sx(v, 16) if t == W else v
        if t == D: return sx(v, 32)
        return v

    def dst(self, w, lane, v):
        if not bits(w, 50, 50): return                           # null
        t = bits(w, 39, 36); nr, sub = bits(w, 63, 56), bits(w, 55, 51)
        hs = HS[bits(w, 49, 48)]
        b = sub + lane * hs * SIZE[t]
        if t in (F, HF):
            if bits(w, 34, 34): v = min(max(v, 0.0), 1.0)
            v = float(v)
            if t == F: v = u2f(f2u(v)) if abs(v) < 3.5e38 else v
        else:
            if isinstance(v, float):                             # float -> int conversion (round toward zero)
                v = int(v) if v == v else 0
                if t == D: v = max(-2**31, min(2**31 - 1, v))
                if t == UD: v = max(0, min(2**32 - 1, v))
        self.wr(nr + b // 32, b % 32, t, v)

    def run(self, payload, mask=0xff, limit=2000000):
        for _ in self.exec_gen(payload, mask, limit):
            raise RuntimeError('barrier in a thread run on its own')
        return self
    def sampler(self, desc, dstr, s0, active, pv):
        import math
        m = (desc >> 12) & 0x1f; bti = desc & 0xff; si = (desc >> 8) & 15; mlen = (desc >> 25) & 15
        if (desc >> 17) & 3 != 1 or (desc >> 19) & 1: raise RuntimeError('sampler: not SIMD8 without header')
        t = self.tex.get(bti)
        if t is None: raise RuntimeError('sampler message to binding table entry %d: no texture' % bti)
        for l in range(8):
            if not active[l] or not pv(l): continue
            if m == 10:                                          # resinfo
                v = (t.w, t.h, t.layers, 1); ty = UD
            elif m == 26:                                        # ld_lz: u, v[, r] integers
                c = [self.rd(s0 + k, l * 4, D) for k in range(mlen)] + [0]
                v = t.get(c[0], c[1], c[2] if mlen > 2 else 0); ty = UD if t.isint() else F
            elif m == 24:                                        # sample_lz: u, v[, r] floats
                sd = self.samp.get(si)
                if sd is None: raise RuntimeError('sample with sampler %d: no sampler state' % si)
                u, vv = self.rd(s0, l * 4, F), self.rd(s0 + 1, l * 4, F)
                z = int(round(self.rd(s0 + 2, l * 4, F))) if mlen > 2 else 0
                if sd.get('normalized', True): u, vv = u * t.w, vv * t.h
                def fetch(x, y):
                    x = min(max(x, 0), t.w - 1); y = min(max(y, 0), t.h - 1)    # clamp to edge only
                    return t.get(x, y, z)
                if sd.get('filter', 'nearest') == 'nearest':
                    v = fetch(math.floor(u), math.floor(vv))
                else:
                    x0, y0 = math.floor(u - 0.5), math.floor(vv - 0.5); fx, fy = u - 0.5 - x0, vv - 0.5 - y0
                    a, b, c2, d = fetch(x0, y0), fetch(x0 + 1, y0), fetch(x0, y0 + 1), fetch(x0 + 1, y0 + 1)
                    v = tuple((a[k] * (1 - fx) + b[k] * fx) * (1 - fy) + (c2[k] * (1 - fx) + d[k] * fx) * fy for k in range(4))
                ty = F
            else: raise RuntimeError('sampler message %d' % m)
            for k in range(4): self.wr(dstr + k, l * 4, ty, v[k])
    def space(self, bti, a, write):
        """memory for a surface: 253 = global (GPU VA), 254 = this group's SLM"""
        if bti == 0xfe:
            if a >= self.slmSize: raise RuntimeError('SLM access at %#x outside the %d bytes of threadgroup memory' % (a, self.slmSize))
            return self.slm
        if bti != 0xfd: raise RuntimeError('surface %d' % bti)
        return self.mem
    def exec_gen(self, payload, mask=0xff, limit=2000000):
        """payload: dict of register -> 8 dwords (g0, g1, g2); mask = lanes dispatched.
        A generator: yields at every sync.bar (the caller resumes it once the whole group arrived)."""
        for r, dws in payload.items():
            for k, x in enumerate(dws): self.wr(r, k * 4, UD, x)
        active = [bool(mask >> l & 1) for l in range(8)]
        stack = []           # ('if', saved, taken) / ('loop', start, saved)
        loopStarts = {}
        for k, w in enumerate(self.code):
            if bits(w, 6, 0) == 39: loopStarts.setdefault(k + sx(bits(w, 127, 96), 32) // 16, []).append(k)
        pc = 0
        while pc < len(self.code):
            self.steps += 1
            if self.steps > limit: raise RuntimeError('too many steps (endless loop?)')
            for _ in loopStarts.get(pc, []):
                if not (stack and stack[-1][0] == 'loop' and stack[-1][1] == pc):
                    stack.append(['loop', pc, active[:]])
            w = self.code[pc]; op = bits(w, 6, 0)
            pred, inv, fl = bits(w, 27, 24), bits(w, 28, 28), bits(w, 23, 23)
            weall = bits(w, 31, 31); ex = 1 << bits(w, 18, 16)
            def pv(l):
                return (self.flag[fl][l] != bool(inv)) if pred else True
            lanes = [l for l in range(ex) if (weall or active[l])]
            if op == 34:                                        # IF
                taken = [active[l] and pv(l) for l in range(8)]
                stack.append(['if', active[:], taken]); active = taken
                if not any(active):                             # no lane takes it: jump (JIP)
                    tgt = pc + sx(bits(w, 127, 96), 32) // 16
                    if tgt > 0 and bits(self.code[tgt - 1], 6, 0) == 36:   # just past ELSE: the else lanes run
                        fr = stack[-1]
                        active = [fr[1][l] and not fr[2][l] for l in range(8)]
                        if not any(active):                     # no lanes for the else part either: on to ENDIF
                            tgt = tgt - 1 + sx(bits(self.code[tgt - 1], 127, 96), 32) // 16
                    pc = tgt; continue
            elif op == 36:                                      # ELSE
                fr = stack[-1]; assert fr[0] == 'if'
                active = [fr[1][l] and not fr[2][l] for l in range(8)]
                if not any(active):                             # nothing for the else part: jump to ENDIF
                    pc += sx(bits(w, 127, 96), 32) // 16; continue
            elif op == 37:                                      # ENDIF
                fr = stack.pop(); assert fr[0] == 'if', 'ENDIF without IF'
                active = fr[1]
            elif op == 40:                                      # BREAK
                active = [active[l] and not pv(l) for l in range(8)]
            elif op == 39:                                      # WHILE
                fr = stack[-1]; assert fr[0] == 'loop'
                if any(active): pc = fr[1]; continue
                stack.pop(); active = fr[2]
            elif op == 1:                                       # sync
                if bits(w, 95, 92) == 0xe:                      # sync.bar
                    if not self.arrived: raise RuntimeError('sync.bar without a barrier message')
                    yield 'bar'
                    self.arrived = False
            elif op == 49:                                      # send
                if bits(w, 63, 56) == 0 and bits(w, 79, 72) == 127:   # EOT (our epilogue)
                    break
                dstr, s0, s1 = bits(w, 63, 56), bits(w, 79, 72), bits(w, 111, 104)
                desc = (bits(w, 123, 122) << 30) | (bits(w, 71, 67) << 25) | (bits(w, 55, 51) << 20) | \
                       (bits(w, 121, 113) << 11) | bits(w, 91, 81)
                sfid = bits(w, 95, 92); mtype = (desc >> 14) & 0x1f; ctrl = (desc >> 8) & 0x3f
                bti = desc & 0xff
                if sfid == 3:                                        # gateway: barrier message
                    if (desc & 7) != 4 or not weall: raise RuntimeError('gateway message %#x' % desc)
                    if self.rd(s0, 8, UD) != (self.rd(0, 8, UD) & 0x7f000000): raise RuntimeError('barrier id not from g0.2')
                    self.arrived = True
                    pc += 1; continue
                if sfid == 2:                                        # sampler (milestone 4)
                    self.sampler(desc, dstr, s0, active, pv)
                    pc += 1; continue
                if sfid == 12 and mtype in (5, 13):                  # typed surface read / write
                    mlen = (desc >> 25) & 15; t = self.tex.get(bti)
                    if t is None: raise RuntimeError('typed message to binding table entry %d: no texture' % bti)
                    if (ctrl >> 4) & 3 != 1 or ctrl & 15: raise RuntimeError('typed message control %#x' % ctrl)
                    nc = mlen - (4 if mtype == 13 else 0)
                    for l in range(8):
                        if not active[l] or not pv(l): continue
                        c = [self.rd(s0 + k, l * 4, D) for k in range(nc)] + [0, 0]
                        if mtype == 13:
                            vt = UD if t.isint() else F
                            t.put(c[0], c[1], c[2] if nc > 2 else 0, tuple(self.rd(s0 + nc + k, l * 4, vt) for k in range(4)))
                        else:
                            v = t.get(c[0], c[1], c[2] if nc > 2 else 0)
                            for k in range(4): self.wr(dstr + k, l * 4, UD if t.isint() else F, v[k])
                    pc += 1; continue
                if sfid == 10 and mtype == 7:                        # memory fence: returns a dummy register
                    if not weall: raise RuntimeError('fence without WE_all')
                    for l in range(8): self.wr(dstr, l * 4, UD, 0)
                    pc += 1; continue
                for l in range(8):
                    if not active[l] or not pv(l): continue
                    a = self.rd(s0, l * 4, UD)
                    if sfid == 12 and mtype == 2:                    # HDC1 untyped atomic (SIMD8, returns old)
                        if a & 3: raise RuntimeError('unaligned atomic %x' % a)
                        mem = self.space(bti, a, True)
                        aop = ctrl & 0xf
                        if a not in mem and aop != 4: raise RuntimeError('atomic on unwritten address %x (%s)' % (a, 'SLM' if bti == 0xfe else 'global'))
                        old = mem.get(a, 0xEEEEEEEE); v = self.rd(s1, l * 4, UD)
                        sv = lambda x: sx(x & 0xffffffff, 32)
                        new = {1: old & v, 2: old | v, 3: old ^ v, 4: v, 7: old + v, 8: old - v,
                               10: old if sv(old) >= sv(v) else v, 11: old if sv(old) <= sv(v) else v,
                               12: max(old, v), 13: min(old, v)}.get(aop)
                        if new is None: raise RuntimeError('atomic op %d' % aop)
                        mem[a] = new & 0xffffffff
                        self.wr(dstr, l * 4, UD, old)
                    elif sfid == 12:                                 # HDC1 untyped surface read/write
                        chans = [c for c in range(4) if not (ctrl >> c) & 1]
                        if a & 3: raise RuntimeError('unaligned address %x' % a)
                        mem = self.space(bti, a + 4 * (len(chans) - 1), mtype == 9)
                        for k, c in enumerate(chans):
                            if mtype == 9: mem[a + 4 * k] = self.rd(s1 + k, l * 4, UD)
                            elif mtype == 1:
                                if a + 4 * k not in mem: raise RuntimeError('read of %s address %x' % ('unwritten SLM' if bti == 0xfe else 'unmapped', a + 4 * k))
                                self.wr(dstr + k, l * 4, UD, mem[a + 4 * k])
                            else: raise RuntimeError('hdc1 message %d' % mtype)
                    elif sfid == 10:                                 # HDC0 byte scattered read/write
                        nb = [1, 2, 4][(ctrl >> 2) & 3]
                        if a % nb: raise RuntimeError('unaligned byte access %x' % a)
                        base, sh = a & ~3, (a & 3) * 8
                        mem = self.space(bti, base, mtype == 0xc)
                        if bti == 0xfe and base not in mem: mem[base] = 0xEEEEEEEE   # SLM starts as garbage
                        if base not in mem: raise RuntimeError('access to unmapped address %x' % a)
                        m = ((1 << (nb * 8)) - 1) << sh
                        if mtype == 0xc:
                            mem[base] = (mem[base] & ~m) | ((self.rd(s1, l * 4, UD) << sh) & m)
                        elif mtype == 4:
                            self.wr(dstr, l * 4, UD, (mem[base] & m) >> sh)   # upper bits zero here
                        else: raise RuntimeError('hdc0 message %d' % mtype)
                    else: raise RuntimeError('sfid %d' % sfid)
            else:
                cm = bits(w, 95, 92)
                if pred and op != 98:                           # v0.51.17: predication disables lanes (sel: selects)
                    lanes = [l for l in lanes if pv(l)]
                for l in lanes:
                    a = self.src(w, 0, l)
                    b = self.src(w, 1, l) if op not in (97, 100, 69, 70, 71, 67, 77, 74, 76, 119) and not (op == 56 and not bits(w, 98, 98)) else None
                    t = bits(w, 39, 36)
                    if op == 56:                                         # math
                        import math as m
                        fn = cm
                        if fn == 1: r = 1.0 / a if a else float('inf')
                        elif fn == 2: r = m.log2(a) if a > 0 else float('-inf')
                        elif fn == 3: r = 2.0 ** a
                        elif fn == 4: r = m.sqrt(a) if a >= 0 else float('nan')
                        elif fn == 5: r = 1.0 / m.sqrt(a) if a > 0 else float('inf')
                        elif fn == 6: r = m.sin(a)
                        elif fn == 7: r = m.cos(a)
                        elif fn == 10: raise RuntimeError('math POW: not supported on Gfx12 (hangs the GPU)')
                        elif fn in (12, 13):
                            if b == 0: r = -1
                            else:
                                q = abs(a) // abs(b)
                                if (a < 0) != (b < 0): q = -q
                                r = q if fn == 12 else a - q * b
                        else: raise RuntimeError('math function %d' % fn)
                        self.dst(w, l, r); continue
                    if op in (69, 70, 71, 67):
                        import math as m
                        r = {69: m.floor(a), 70: round(a), 71: m.trunc(a), 67: a - m.floor(a)}[op]
                        self.dst(w, l, float(r)); continue
                    if op in (77, 74, 76, 119):
                        x = int(a) & 0xffffffff
                        if op == 77: r = bin(x).count('1')
                        elif op == 74: r = 32 - x.bit_length()
                        elif op == 76: r = (x & -x).bit_length() - 1 if x else 0xffffffff
                        else: r = int('{:032b}'.format(x)[::-1], 2)
                        self.dst(w, l, r); continue
                    if op == 97: r = a                                   # mov
                    elif op == 64: r = a + b                             # add
                    elif op == 65: r = a * b                             # mul
                    elif op == 105: r = (a << (b & 31))                  # shl
                    elif op == 104: r = (a & 0xffffffff) >> (b & 31)     # shr
                    elif op == 108: r = sx(a & 0xffffffff, 32) >> (b & 31)   # asr
                    elif op == 101: r = a & b
                    elif op == 102: r = a | b
                    elif op == 103: r = a ^ b
                    elif op == 100: r = ~a
                    elif op == 98: r = a if pv(l) else b                 # sel (predicate selects)
                    elif op == 112:                                     # cmp
                        c = {1: a == b, 2: a != b, 3: a > b, 4: a >= b, 5: a < b, 6: a <= b}[cm]
                        self.flag[fl][l] = c
                        if bits(w, 50, 50):                              # dst gets 0 / all ones
                            self.wr(bits(w, 63, 56), bits(w, 55, 51) + l * 4, UD, 0xffffffff if c else 0)
                        continue
                    else: raise RuntimeError('opcode %d not simulated' % op)
                    if op == 97 and cm:                                 # mov.nz etc: flag
                        self.flag[fl][l] = {1: r == 0, 2: r != 0}[cm]
                    if t != F and isinstance(r, float): pass
                    self.dst(w, l, r)
            pc += 1
        return self


def run_kernel(code_words, grid, tpg, bufs, slm=0, barrier=False, split=True, tex=None, samp=None, groups_y=1,
               tpg_h=1, grid_y=None):
    """bufs: list of (gpu address, list of u32); returns memory dict.
    slm: threadgroup memory bytes; barrier: the kernel uses barriers. The
    hardware threads of one group run together (each until its next sync.bar);
    split: like IrisXeCore 0.28, a partial last group of a barrier kernel gets
    only the threads it needs."""
    # v0.51.17: thread groups of tpg x tpg_h threads (grid: grid x grid_y threads; IrisXeCore ABI 8
    # per-thread data g2.1..5 = W, H, grid rows, grid slices, D)
    W = tpg
    tpg = tpg * tpg_h
    if grid_y is None: grid_y = groups_y * tpg_h
    mem = {}
    for base, data in bufs:
        for k, x in enumerate(data): mem[base + 4 * k] = x & 0xffffffff
    threads = (tpg + 7) // 8
    groups = (grid + W - 1) // W
    ctd = [b for b, _ in bufs] + [0] * (6 - len(bufs)) + [grid, tpg]
    steps = 0
    for gy in range(groups_y):
     for g in range(groups):
         n = tpg
         if barrier and split and tpg_h == 1 and g == groups - 1 and grid < groups * tpg: n = grid - g * tpg
         shared = {}
         sims, gens = [], []
         for t in range((n + 7) // 8):
             s = Sim(code_words, mem, shared, slm)
             s.tex = tex or {}; s.samp = samp or {}
             # a different hardware thread for every thread of the group (Gfx12 sr0 fields:
             # thread [2:0], EU [5:4], EU row [7], subslice [8], dual subslice [10:9])
             s.sr0 = (t % 7) | ((t // 7 % 4) << 4) | ((t // 28 % 2) << 7) | ((t // 56 % 2) << 8) | 0x48   # + unused bits 3, 6
             lanes = min(8, n - t * 8)                            # the walker's right execution mask
             g0 = [0, g, 0x05000000 | (g & 0xff) << 24 & 0x7f000000, 0, 0, 0, gy, 0]
             sims.append(s)
             gens.append(s.exec_gen({0: g0, 1: ctd, 2: [t * 8, W, tpg_h, grid_y, 1, 1, 0, 0]}, (1 << lanes) - 1))
         live = list(range(len(gens)))
         while live:
             waiting, done = [], []
             for i in live:
                 try:
                     next(gens[i]); waiting.append(i)
                 except StopIteration:
                     done.append(i)
             if waiting and done:
                 raise RuntimeError('GPU HANG: in group %d threads %s ended while threads %s wait at a barrier' % (g, done, waiting))
             live = waiting
         steps += sum(s.steps for s in sims)
    return mem, steps

def load_bin(path):
    b = open(path, 'rb').read()
    return [int.from_bytes(b[i:i + 16], 'little') for i in range(0, len(b), 16)]

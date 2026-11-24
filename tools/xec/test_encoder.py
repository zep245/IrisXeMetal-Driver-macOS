#!/usr/bin/env python3
"""Checks src/compiler/xe_enc.h against Mesa's brw_asm (Gfx12): every case of
enc_cases.cpp must assemble to exactly the same 128 bits."""
import subprocess, sys, os
sys.path.insert(0, '/home/claude/xec')
from brwasm import asm, dis
HERE = os.path.dirname(os.path.abspath(__file__))
CASES = [
 'add(8) g10<1>UD g20<8,8,1>UD g30<8,8,1>UD { align1 1Q @1 };',
 'add(8) g10<1>UD g20<8,8,1>UD 0x12345678UD { align1 1Q @1 };',
 'add(8) g10<1>UD g20.3<0,1,0>UD g30<8,8,1>UD { align1 1Q @1 };',
 'add(8) g10<1>D g20<8,8,1>D -g30<8,8,1>D { align1 1Q @1 };',
 'add(8) g10<1>F g20<8,8,1>F g30<8,8,1>F { align1 1Q @1 };',
 'mul(8) g10<1>F g20<8,8,1>F g1.2<0,1,0>F { align1 1Q @1 };',
 'mov(8) g10<1>F g20<8,8,1>D { align1 1Q @1 };',
 'mov(8) g10<1>D g20<8,8,1>F { align1 1Q @1 };',
 'mov(8) g10<1>F 0x3f800000F { align1 1Q @1 };',
 'cmp.l.f0.0(8) g10<1>D g20<8,8,1>D g30<8,8,1>D { align1 1Q @1 };',
 'cmp.ge.f1.0(8) null<1>UD g20<8,8,1>UD g1.6<0,1,0>UD { align1 1Q @1 };',
 'mov.nz.f0.0(8) null<1>D g20<8,8,1>D { align1 1Q @1 };',
 '(+f0.0) sel(8) g10<1>UD g20<8,8,1>UD g30<8,8,1>UD { align1 1Q @1 };',
 '(-f0.0) sel(8) g10<1>F g20<8,8,1>F 0x40000000F { align1 1Q @1 };',
 'shl(8) g10<1>UD g20<8,8,1>UD 0x2UD { align1 1Q @1 };',
 'asr(8) g10<1>D g20<8,8,1>D g30<8,8,1>D { align1 1Q @1 };',
 'and(8) g10<1>UD g20<8,8,1>UD 0x1UD { align1 1Q @1 };',
 'mul(8) g10<1>D g20<8,8,1>D 0x000cUW { align1 1Q @1 };',
 'mul(8) g10<1>D g20<8,8,1>D g30<16,8,2>UW { align1 1Q @1 };',
 'mul(8) g11<1>D g20<8,8,1>D g30.1<16,8,2>UW { align1 1Q @1 };',
 'add(8) g10.1<2>UW g10.1<16,8,2>UW g11<16,8,2>UW { align1 1Q @1 };',
 'mul(1) g5<1>UD g0.1<0,1,0>UD g1.14<0,1,0>UW { align1 WE_all 1N @1 };',
 'mov(8) g126<1>UW 0x76543210V { align1 WE_all 1Q };',
 'mov(8) g3<1>UD g126<8,8,1>UW { align1 WE_all 1Q @1 };',
 'add(8) g10<1>F g20<8,8,1>F (abs)g20<8,8,1>F { align1 1Q @1 };',
 'mov.sat(8) g10<1>F g20<8,8,1>F { align1 1Q @1 };',
 '(+f1.0) send(8) g8UD g6UD nullUD 0x02106efd 0x00000000 hdc1 MsgDesc: (DC untyped surface read, Surface = 253, SIMD8, Mask = 0xe) mlen 1 ex_mlen 0 rlen 1 { align1 1Q @2 $0 };',
 '(+f1.0) send(8) nullUD g7UD g9UD 0x02026efd 0x00000040 hdc1 MsgDesc: (DC untyped surface write, Surface = 253, SIMD8, Mask = 0xe) mlen 1 ex_mlen 1 rlen 0 { align1 1Q @1 $1 };',
 'sync nop(8) null<0,1,0>UD { align1 WE_all 1Q $1.dst };',
 'send(8) nullUD g127UD nullUD 0x02000000 0x00000000 ts/btd MsgDesc: mlen 1 ex_mlen 0 rlen 0 { align1 WE_all 1Q @1 EOT };',
 'not(8) g10<1>UD g20<8,8,1>UD { align1 1Q @1 };',
 'xor(8) g10<1>UD g20<8,8,1>UD g30<8,8,1>UD { align1 1Q @1 };',
]
def snd(sf, d, e, dst, s1, ml, exml, rl):
    return '(+f1.0) send(8) %s g6UD %s %s %s %s MsgDesc: (x) mlen %d ex_mlen %d rlen %d { align1 1Q @1 $0 };' % (dst, s1, d, e, sf, ml, exml, rl)
for n in range(1, 5):
    mask = (~((1 << n) - 1)) & 0xf
    CASES.append(snd('hdc1', '0x%08x' % ((1 << 25) | (n << 20) | (1 << 14) | ((0x20 | mask) << 8) | 0xfd), '0x00000000', 'g8UD', 'nullUD', 1, 0, n))
for n in range(1, 5):
    mask = (~((1 << n) - 1)) & 0xf
    CASES.append(snd('hdc1', '0x%08x' % ((1 << 25) | (9 << 14) | ((0x20 | mask) << 8) | 0xfd), '0x%08x' % (n << 6), 'nullUD', 'g9UD', 1, n, 0))
for ds in (0, 1, 2):
    CASES.append(snd('hdc0', '0x%08x' % ((1 << 25) | (1 << 20) | (4 << 14) | ((ds << 2) << 8) | 0xfd), '0x00000000', 'g8UD', 'nullUD', 1, 0, 1))
for ds in (0, 1, 2):
    CASES.append(snd('hdc0', '0x%08x' % ((1 << 25) | (0xc << 14) | ((ds << 2) << 8) | 0xfd), '0x00000040', 'nullUD', 'g9UD', 1, 1, 0))
CASES += [
 'math inv(8) g10<1>F g20<8,8,1>F null<8,8,1>F { align1 1Q @1 $0 };',
 'math pow(8) g10<1>F g20<8,8,1>F g30<8,8,1>F { align1 1Q @1 $0 };',
 'math intdiv(8) g10<1>D g20<8,8,1>D g30<8,8,1>D { align1 1Q @1 $0 };',
 'math intmod(8) g10<1>UD g20<8,8,1>UD g30<8,8,1>UD { align1 1Q @1 $0 };',
 'rndd(8) g10<1>F g20<8,8,1>F { align1 1Q @1 };',
 'rnde(8) g10<1>F g20<8,8,1>F { align1 1Q @1 };',
 'rndz(8) g10<1>F g20<8,8,1>F { align1 1Q @1 };',
 'frc(8) g10<1>F g20<8,8,1>F { align1 1Q @1 };',
 'cbit(8) g10<1>UD g20<8,8,1>UD { align1 1Q @1 };',
 'lzd(8) g10<1>UD g20<8,8,1>UD { align1 1Q @1 };',
 'fbl(8) g10<1>UD g20<8,8,1>UD { align1 1Q @1 };',
 'bfrev(8) g10<1>UD g20<8,8,1>UD { align1 1Q @1 };',
 'mov(8) g10<1>HF g20<8,8,1>F { align1 1Q @1 };',
 'mov(8) g10<1>F g20<16,8,2>HF { align1 1Q @1 };',
 'mov(8) g10<2>HF g20<8,8,1>F { align1 1Q @1 };',
 'mov(8) g10<1>UD g20<32,8,4>UB { align1 1Q @1 };',
 'mov(8) g10<1>D g20<32,8,4>B { align1 1Q @1 };',
 'mov(8) g10<1>UD g20<16,8,2>UW { align1 1Q @1 };',
 'mov(8) g10<1>D g20<16,8,2>W { align1 1Q @1 };',
 # 3f
 snd('hdc1', '0x%08x' % ((1 << 25) | (2 << 20) | (1 << 14) | (0x2c << 8) | 0xfe), '0x00000000', 'g8UD', 'nullUD', 1, 0, 2),
 snd('hdc1', '0x%08x' % ((1 << 25) | (9 << 14) | (0x2e << 8) | 0xfe), '0x00000040', 'nullUD', 'g9UD', 1, 1, 0),
 snd('hdc0', '0x%08x' % ((1 << 25) | (1 << 20) | (4 << 14) | 0xfe), '0x00000000', 'g8UD', 'nullUD', 1, 0, 1),
 snd('hdc1', '0x0210b7fd', '0x00000040', 'g8UD', 'g9UD', 1, 1, 1),
 snd('hdc1', '0x0210bcfe', '0x00000040', 'g8UD', 'g9UD', 1, 1, 1),
 snd('hdc1', '0x0210befd', '0x00000080', 'g8UD', 'g9UD', 1, 2, 1),
 'send(8) g13UD g0UD nullUD 0x0219e0fe 0x00000000 hdc0 MsgDesc: (x) mlen 1 ex_mlen 0 rlen 1 { align1 WE_all 1Q @1 $0 };',
 'send(8) g13UD g0UD nullUD 0x0219e000 0x00000000 hdc0 MsgDesc: (x) mlen 1 ex_mlen 0 rlen 1 { align1 WE_all 1Q @1 $0 };',
 'send(8) nullUD g12UD nullUD 0x02000004 0x00000000 gateway MsgDesc: (barrier msg) mlen 1 ex_mlen 0 rlen 0 { align1 WE_all 1Q @1 $2 };',
 'sync bar(8) null<0,1,0>UD { align1 WE_all 1Q };',
 'mov(8) g12<1>UD 0x00000000UD { align1 WE_all 1Q @1 };',
 'and(1) g12.2<1>UD g0.2<0,1,0>UD 0x7f000000UD { align1 WE_all 1N @1 };',
 'sync nop(8) null<0,1,0>UD { align1 WE_all 1Q $2.src };',
 # milestone 4 (descriptors as Mesa brw_sampler_desc / brw_dp_typed_surface_rw_desc build them)
 '(+f1.0) send(8) g20UD g12UD nullUD 0x0443a000 0x00000000 sampler MsgDesc: (x) mlen 2 ex_mlen 0 rlen 4 { align1 1Q @1 $0 };',
 '(+f1.0) send(8) g20UD g12UD nullUD 0x04438103 0x00000000 sampler MsgDesc: (x) mlen 2 ex_mlen 0 rlen 4 { align1 1Q @1 $0 };',
 '(+f1.0) send(8) g20UD g12UD nullUD 0x0242a002 0x00000000 sampler MsgDesc: (x) mlen 1 ex_mlen 0 rlen 4 { align1 1Q @1 $0 };',
 '(+f1.0) send(8) nullUD g12UD nullUD 0x0c035001 0x00000000 hdc1 MsgDesc: (x) mlen 6 ex_mlen 0 rlen 0 { align1 1Q @1 $0 };',
 '(+f1.0) send(8) nullUD g12UD nullUD 0x0e035000 0x00000000 hdc1 MsgDesc: (x) mlen 7 ex_mlen 0 rlen 0 { align1 1Q @1 $0 };',
 '(+f1.0) send(8) g20UD g12UD nullUD 0x04415000 0x00000000 hdc1 MsgDesc: (x) mlen 2 ex_mlen 0 rlen 4 { align1 1Q @1 $0 };',
]
# control-flow cases: assembled inside small programs (labels); (program, index of the instruction)
CF = [
 (['(+f0.0) if(8) JIP: L UIP: L { align1 1Q @1 };', 'L:', 'endif(8) JIP: L2 { align1 1Q };', 'L2:', 'nop;'], 0),
 (['(+f0.0) if(8) JIP: L UIP: L { align1 1Q @1 };', 'L:', 'endif(8) JIP: L2 { align1 1Q };', 'L2:', 'nop;'], 1),
 (['(+f1.0) if(8) JIP: L UIP: L { align1 1Q };', 'L:', 'endif(8) JIP: L2 { align1 1Q };', 'L2:', 'nop;'], 0),
 (['else(8) JIP: L UIP: L { align1 1Q };', 'nop;', 'L:', 'nop;'], 0),
 (['L0:', '(-f0.0) break(8) JIP: L UIP: L { align1 1Q @1 };', 'L:', 'while(8) JIP: L0 { align1 1Q };', 'nop;'], 0),
 (['L0:', 'break(8) JIP: L UIP: L { align1 1Q };', 'L:', 'while(8) JIP: L0 { align1 1Q };', 'nop;'], 0),
 (['L0:', 'break(8) JIP: L UIP: L { align1 1Q };', 'L:', 'while(8) JIP: L0 { align1 1Q };', 'nop;'], 1),
]
def asm_prog(lines):
    import tempfile, subprocess
    from brwasm import B
    with tempfile.TemporaryDirectory() as d:
        a = os.path.join(d, 't.asm'); o = os.path.join(d, 't.bin')
        open(a, 'w').write('\n'.join(lines) + '\n')
        subprocess.run([B + '/brw_asm', '-g', 'tgl', '-t', 'bin', '-o', o, a], check=True, capture_output=True)
        b = open(o, 'rb').read()
    return [int.from_bytes(b[i:i + 16], 'little') for i in range(0, len(b), 16)]
exe = '/tmp/enc_cases'
subprocess.run(['g++', '-std=c++17', '-O1', '-o', exe, os.path.join(HERE, 'enc_cases.cpp')], check=True)
ours = [int(l, 16) for l in subprocess.run([exe], capture_output=True, text=True, check=True).stdout.split()]
assert len(ours) == len(CASES) + len(CF), (len(ours), len(CASES) + len(CF))
refs = [asm(t)[0] for t in CASES] + [asm_prog(p)[k] for p, k in CF]
names = CASES + ['%s [#%d]' % (' | '.join(p), k) for p, k in CF]
bad = 0
for i, (txt, w, ref) in enumerate(zip(names, ours, refs)):
    if ref != w:
        bad += 1
        x = ref ^ w
        print('MISMATCH %d: %s\n  ref  %032x\n  ours %032x\n  bits %s' % (i, txt, ref, w, [b for b in range(128) if x >> b & 1]))
        print('  ours disasm:', dis([w]).strip())
print('%d/%d encoder cases match brw_asm' % (len(names) - bad, len(names)))
sys.exit(1 if bad else 0)

#!/usr/bin/env python3
"""Register-reuse stress kernels for our compiler (v0.29.0): writes an AIR-style
.ll with big kernels (hundreds of values, values live across loops, loop-carried
phis, ifs inside loops, nested loops) and a Python reference for each.
Usage: gen_stress.py out.ll  -> then llvm-as, xec_test, sim_stress.py"""
import sys, random
M = 0xffffffff
def kernel(name, seed, chain, live, loops):
    rnd = random.Random(seed)
    L, ref = [], []                    # IR lines, python statements (same names)
    L.append('define void @%s(ptr addrspace(1) %%in, ptr addrspace(1) %%out, ptr addrspace(1) %%nb, i32 %%gid) {' % name)
    L.append('entry:')
    L.append('  %g = zext i32 %gid to i64')
    L.append('  %pi = getelementptr inbounds i32, ptr addrspace(1) %in, i64 %g')
    L.append('  %x = load i32, ptr addrspace(1) %pi, align 4')
    L.append('  %n = load i32, ptr addrspace(1) %nb, align 4')
    ref += ['x = inp[gid]', 'n = N']
    ops = [('add', '({a} + {b}) & M'), ('xor', '{a} ^ {b}'), ('mul', '({a} * {b}) & M'),
           ('or', '{a} | {b}'), ('and', '{a} & {b}'), ('sub', '({a} - {b}) & M')]
    # live values a0..a{live-1}, all used again at the end
    prev = '%x'
    for i in range(live):
        o, f = ops[i % len(ops)]
        k = rnd.randrange(1, 1 << 20) | 1
        L.append('  %%a%d = %s i32 %s, %d' % (i, o, prev, k))
        ref.append('a%d = %s' % (i, f.format(a=prev.strip('%'), b=k)))
        prev = '%%a%d' % i
    # a long chain of temporaries mixing in the live values
    prev = '%x'
    for i in range(chain):
        o, f = ops[rnd.randrange(len(ops))]
        b = '%%a%d' % rnd.randrange(live)
        L.append('  %%t%d = %s i32 %s, %s' % (i, o, prev, b))
        ref.append('t%d = %s' % (i, f.format(a=prev.strip('%'), b=b.strip('%'))))
        if i % 7 == 3:
            L.append('  %%s%d = lshr i32 %%t%d, %d' % (i, i, i % 13 + 1))
            L.append('  %%u%d = xor i32 %%t%d, %%s%d' % (i, i, i))
            ref.append('t%d = t%d ^ (t%d >> %d)' % (i, i, i, i % 13 + 1))
            prev = '%%u%d' % i
            ref.append('u%d = t%d' % (i, i))
        else: prev = '%%t%d' % i
    chainEnd = prev
    # loops: values defined before, used only inside; three loop-carried phis; an if inside
    inner = []                         # a-values used only in loops
    acc_ir, acc_py = [], []
    pre = 'entry'
    for li in range(loops):
        u1, u2, u3 = ('%%a%d' % rnd.randrange(live) for _ in range(3))
        inner += [u1, u2, u3]
        h, body, thn, latch, ex = ('L%d_h' % li, 'L%d_b' % li, 'L%d_t' % li, 'L%d_l' % li, 'L%d_x' % li)
        start = chainEnd if li == 0 else '%%r%d' % (li - 1)
        L.append('  br label %%%s' % h)
        L.append('%s:' % h)
        L.append('  %%j%d = phi i32 [ 0, %%%s ], [ %%jn%d, %%%s ]' % (li, pre, li, latch))
        L.append('  %%p%d = phi i32 [ %s, %%%s ], [ %%pn%d, %%%s ]' % (li, start, pre, li, latch))
        L.append('  %%q%d = phi i32 [ %d, %%%s ], [ %%qn%d, %%%s ]' % (li, li + 5, pre, li, latch))
        L.append('  %%w%d = phi i32 [ 0, %%%s ], [ %%wn%d, %%%s ]' % (li, pre, li, latch))
        L.append('  %%c%d = icmp ult i32 %%j%d, %%n' % (li, li))
        L.append('  br i1 %%c%d, label %%%s, label %%%s' % (li, body, ex))
        L.append('%s:' % body)
        L.append('  %%m%d = mul i32 %s, %%j%d' % (li, u1, li))
        L.append('  %%pa%d = add i32 %%p%d, %%m%d' % (li, li, li))
        L.append('  %%sh%d = and i32 %%j%d, 7' % (li, li))
        L.append('  %%v%d = lshr i32 %s, %%sh%d' % (li, u2, li))
        L.append('  %%qx%d = xor i32 %%q%d, %%v%d' % (li, li, li))
        L.append('  %%bit%d = and i32 %%pa%d, 1' % (li, li))
        L.append('  %%odd%d = icmp ne i32 %%bit%d, 0' % (li, li))
        L.append('  br i1 %%odd%d, label %%%s, label %%%s' % (li, thn, latch))
        L.append('%s:' % thn)
        L.append('  %%qt%d = add i32 %%qx%d, %s' % (li, li, u3))
        L.append('  %%wt%d = add i32 %%w%d, 1' % (li, li))
        L.append('  br label %%%s' % latch)
        L.append('%s:' % latch)
        L.append('  %%qn%d = phi i32 [ %%qt%d, %%%s ], [ %%qx%d, %%%s ]' % (li, li, thn, li, body))
        L.append('  %%wn%d = phi i32 [ %%wt%d, %%%s ], [ %%w%d, %%%s ]' % (li, li, thn, li, body))
        L.append('  %%pn%d = xor i32 %%pa%d, %%qn%d' % (li, li, li))
        L.append('  %%jn%d = add i32 %%j%d, 1' % (li, li))
        L.append('  br label %%%s' % h)
        L.append('%s:' % ex)
        L.append('  %%r%d = add i32 %%p%d, %%q%d' % (li, li, li))
        L.append('  %%rw%d = xor i32 %%r%d, %%w%d' % (li, li, li))
        pre = ex
        U1, U2, U3 = (u.strip('%') for u in (u1, u2, u3))
        ref += ['p = %s; q = %d; w = 0' % ('chainEnd' if li == 0 else 'r%d' % (li - 1), li + 5),
                'for j in range(n):',
                '    pa = (p + %s * j) & M' % U1,
                '    qx = q ^ (%s >> (j & 7))' % U2,
                '    if pa & 1: qx = (qx + %s) & M; w += 1' % U3,
                '    q = qx; p = pa ^ q',
                'r%d = (p + q) & M' % li, 'rw%d = r%d ^ w' % (li, li)]
        if li == 0: ref.insert(len(ref) - 8, 'chainEnd = %s' % chainEnd.strip('%'))
    # end: xor of everything still live
    acc = '%%rw%d' % (loops - 1)
    accp = 'rw%d' % (loops - 1)
    for i in range(live):
        if '%%a%d' % i in inner: continue
        L.append('  %%e%d = xor i32 %s, %%a%d' % (i, acc, i)); acc = '%%e%d' % i
        accp = '(%s ^ a%d)' % (accp, i)
    for li in range(loops - 1):
        L.append('  %%f%d = add i32 %s, %%rw%d' % (li, acc, li)); acc = '%%f%d' % li
        accp = '((%s + rw%d) & M)' % (accp, li)
    L.append('  %po = getelementptr inbounds i32, ptr addrspace(1) %out, i64 %g')
    L.append('  store i32 %s, ptr addrspace(1) %%po, align 4' % acc)
    L.append('  ret void')
    L.append('}')
    ref.append('res = %s' % accp)
    return L, ref
specs = [('big_chain', 1, 300, 40, 1), ('live_loops', 2, 120, 60, 3), ('wide_live', 3, 200, 90, 2)]
out = ['target triple = "air64-apple-macosx15.7.0"']
md = []
py = ['M = 0xffffffff', 'def run(name, inp, gid, N):']
for k, (name, seed, chain, live, loops) in enumerate(specs):
    L, ref = kernel(name, seed, chain, live, loops)
    out += L
    py.append('    if name == %r:' % name)
    py += ['        ' + r for r in ref]
    py.append('        return res')
    md.append(name)
out.append('!air.kernel = !{%s}' % ', '.join('!%d' % (10 + k) for k in range(len(md))))
out.append('!1 = !{}')
out.append('!2 = !{!3, !4, !5, !6}')
out.append('!3 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1}')
out.append('!4 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1}')
out.append('!5 = !{i32 2, !"air.buffer", !"air.location_index", i32 2, i32 1}')
out.append('!6 = !{i32 3, !"air.thread_position_in_grid"}')
for k, name in enumerate(md): out.append('!%d = !{ptr @%s, !1, !2}' % (10 + k, name))
open(sys.argv[1], 'w').write('\n'.join(out) + '\n')
open(sys.argv[1].replace('.ll', '_ref.py'), 'w').write('\n'.join(py) + '\n')
print('kernels:', ', '.join(md))

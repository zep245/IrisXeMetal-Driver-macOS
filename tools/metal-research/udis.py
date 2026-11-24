#!/usr/bin/env python3
"""udis.py - disassemble user-space functions (Objective-C methods and C
functions) of Metal, IOAccelerator and GPU driver bundles, straight from
memory (they live in the dyld shared cache), with call targets, stubs, objc
selector references and C strings named.

usage: udis.py OUT.txt SPEC... where SPEC is
    load:<path>                 dlopen first (driver bundles)
    m:<Class>:<selector>        instance method
    c:<Class>:<selector>        class method
    s:<symbol>                  C function (dlsym name, no leading _)
    all:<Class>                 every instance method of a class (from objc_dump list)
    ivars:<Class>               ivar layout
    x:<imagesub>:<text>         every function of an image that loads a string containing text
    y:<imagesub>:<regex>        every symbol of the image whose name matches regex (names listed, max 60 dumped)
    r:<imagesub>:<symbol>       every function of the image that uses the address of symbol (rip-relative)
    k:<imagesub>:<symbol>       every function of the image that calls or jumps to symbol (round 21)
    v:<imagesub>:<symbol>:<n>   hex dump of n bytes at a (data) symbol (round 25)
    cfs:<imagesub>:<symbol>     every CFString constant a function loads (lea rip+...), decoded (round 25)
Functions called directly from a dumped function are dumped too when their
symbol name matches --follow REGEX (one level).
"""
import re, subprocess, sys, os
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM, X86_OP_IMM, X86_REG_RIP

HERE = os.path.dirname(os.path.abspath(__file__))


class Srv:
    def __init__(self, exe):
        self.p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        self.cache = {}

    def q(self, cmd):
        try:
            self.p.stdin.write(cmd + '\n'); self.p.stdin.flush()
            return self.p.stdout.readline().rstrip('\n')
        except BrokenPipeError:
            raise SystemExit('udis_server stopped (last command: %s)' % cmd)

    def qlist(self, cmd):
        self.p.stdin.write(cmd + '\n'); self.p.stdin.flush()
        out = []
        while True:
            l = self.p.stdout.readline().rstrip('\n')
            if l == 'END' or l == '':
                return out
            out.append(l)

    def name(self, a):
        if a not in self.cache:
            self.cache[a] = self.q('NAME 0x%x' % a)
        return self.cache[a]

    def read(self, a, n):
        h = self.q('READ 0x%x %d' % (a, n))
        return bytes.fromhex(h) if h else b''


def short(nm):
    parts = nm.split('|')
    if len(parts) >= 3:
        img, sym, sa = parts[0], parts[1], int(parts[2], 16)
        s = sym
        if len(parts) >= 5 and parts[3].startswith('stub->'):
            s = 'stub -> ' + parts[3][6:]
        return s, img, sa
    return nm, '?', 0


def disasm(srv, addr, title, out, follow=None, seen=None):
    seen = seen if seen is not None else set()
    if addr in seen or addr == 0:
        return []
    seen.add(addr)
    sym, img, sa = short(srv.name(addr))
    code = srv.read(addr, 12000)
    md = Cs(CS_ARCH_X86, CS_MODE_64); md.detail = True
    out.write('\n### %s  @0x%x  (%s %s)\n' % (title, addr, img, sym))
    maxj, calls = addr, []
    for ins in md.disasm(code, addr):
        note = ''
        tgt = None
        if ins.mnemonic in ('call', 'jmp') or ins.mnemonic.startswith('j'):
            op = ins.operands[0] if ins.operands else None
            if op is not None and op.type == X86_OP_IMM:
                tgt = op.imm
                if ins.mnemonic.startswith('j') and ins.mnemonic != 'jmp' or (ins.mnemonic == 'jmp' and addr <= tgt < addr + 12000):
                    maxj = max(maxj, tgt)
                if ins.mnemonic == 'call' or not (addr <= tgt < addr + 12000):
                    s2, i2, _ = short(srv.name(tgt))
                    note = '%s (%s)' % (s2, i2)
                    if ins.mnemonic == 'call' or ins.mnemonic == 'jmp':   # tail calls too
                        calls.append((tgt, s2))
        for op in ins.operands:
            if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                ea = ins.address + ins.size + op.mem.disp
                n2 = srv.name(ea)
                s2, i2, _ = short(n2)
                if ins.mnemonic == 'lea':
                    if s2 == '?':
                        st = srv.q('STR 0x%x' % ea)
                        if st:
                            s2 = '"%s"' % st
                    note += ' [%s]' % s2
                else:
                    pv = srv.q('PTR 0x%x' % ea).split('|')
                    tgtname = short('|'.join(pv[1:]))[0] if len(pv) > 3 else '?'
                    if tgtname == '?' and pv and pv[0].startswith('0x'):
                        st = srv.q('SEL ' + pv[0]) or srv.q('STR ' + pv[0])   # selector reference -> name
                        if st:
                            tgtname = '"%s"' % st
                    if s2 == '?':
                        st = srv.q('STR 0x%x' % ea)
                        if st:
                            s2 = '"%s"' % st
                    note += ' [%s -> %s]' % (s2, tgtname)
        out.write('  %x: %-8s %-50s %s\n' % (ins.address, ins.mnemonic, ins.op_str, ('; ' + note) if note else ''))
        if ins.mnemonic == 'ret' and ins.address >= maxj:
            break
        if ins.mnemonic == 'jmp' and tgt is not None and not (addr <= tgt < addr + 12000) and ins.address >= maxj:
            break
        if ins.mnemonic == 'ud2' and ins.address >= maxj:
            break
    res = []
    if follow:
        for t, s2 in calls:
            if re.search(follow, s2) and t not in seen:
                res.append((t, s2))
    return res


def main():
    out_path = sys.argv[1]
    args = sys.argv[2:]
    follow = None
    depth = 1
    if '--follow' in args:
        i = args.index('--follow'); follow = args[i + 1]; del args[i:i + 2]
    if '--depth' in args:
        i = args.index('--depth'); depth = int(args[i + 1]); del args[i:i + 2]
    srv = Srv(os.path.join(HERE, 'udis_server'))
    out = open(out_path, 'w')
    seen = set()
    todo = []
    for spec in args:
        kind, _, rest = spec.partition(':')
        if kind == 'load':
            out.write('# load %s: %s\n' % (rest, srv.q('LOAD ' + rest)))
        elif kind in ('m', 'c'):
            cls, sel = rest.split(':', 1)
            a = int(srv.q('%s %s %s' % ('IMP' if kind == 'm' else 'CIMP', cls, sel)), 16)
            if not a:
                out.write('\n### %s[%s %s]  NOT FOUND\n' % ('-' if kind == 'm' else '+', cls, sel)); continue
            todo += disasm(srv, a, '%s[%s %s]' % ('-' if kind == 'm' else '+', cls, sel), out, follow, seen)
        elif kind == 's':
            a = int(srv.q('SYM ' + rest), 16)
            if not a:
                out.write('\n### %s  NOT FOUND\n' % rest); continue
            todo += disasm(srv, a, rest, out, follow, seen)
        elif kind == 'x':
            img, txt = rest.split(':', 1)
            res = srv.qlist('XREF %s %s' % (img, txt))
            out.write('\n# xref %s "%s": %d hit(s)\n' % (img, txt, len(res)))
            for l in res:
                f, h = [int(v, 16) for v in l.split()]
                out.write('#   function 0x%x loads it at 0x%x\n' % (f, h))
                todo += disasm(srv, f, 'xref "%s" (hit at 0x%x)' % (txt, h), out, follow, seen)
        elif kind == 'y':
            img, rx = rest.split(':', 1)
            lst = []
            for l in srv.qlist('SYMS %s %s' % (img, '')):
                a, _, nm = l.partition(' ')
                if re.search(rx, nm):
                    lst.append((int(a, 16), nm))
            out.write('\n# symbols of %s matching %s: %d\n' % (img, rx, len(lst)))
            for a, nm in lst:
                out.write('#   0x%x %s\n' % (a, nm))
            for a, nm in lst[:60]:
                todo += disasm(srv, a, 'symbol ' + nm, out, follow, seen)
        elif kind == 'k':
            img, sym = rest.split(':', 1)
            tgt = int(srv.q('SYM ' + sym), 16)
            if not tgt:
                for l in srv.qlist('SYMS %s %s' % (img, sym)):
                    a2, _, nm = l.partition(' ')
                    if nm in (sym, '_' + sym):
                        tgt = int(a2, 16)
            t0, n = [int(v, 0) for v in srv.q('TEXT ' + img).split()]
            out.write('\n# callers of %s (0x%x) in %s __text 0x%x+%d\n' % (sym, tgt, img, t0, n))
            funcs = []
            if tgt and t0:
                code = b''
                for o in range(0, n, 65536):
                    code += srv.read(t0 + o, min(65536, n - o))
                md = Cs(CS_ARCH_X86, CS_MODE_64); md.detail = True
                o = 0
                while o < len(code):
                    last = None
                    for ins in md.disasm(code[o:o + 4096], t0 + o):
                        if ins.mnemonic in ('call', 'jmp') and len(ins.operands) == 1 and \
                           ins.operands[0].type == X86_OP_IMM and ins.operands[0].imm == tgt:
                            f = ins.address - t0
                            while f > 0 and code[f:f + 4] != b'\x55\x48\x89\xe5':
                                f -= 1
                            if t0 + f not in funcs:
                                funcs.append(t0 + f)
                                out.write('#   function 0x%x (%s) calls it at 0x%x\n' % (t0 + f, srv.name(t0 + f), ins.address))
                        last = ins.address + ins.size - t0
                    o = last if (last is not None and last > o) else o + 1
            out.flush()
            for f in funcs[:20]:
                todo += disasm(srv, f, 'calls %s' % sym, out, follow, seen)
        elif kind == 'r':
            img, sym = rest.split(':', 1)
            tgt = int(srv.q('SYM ' + sym), 16)
            if not tgt:                                   # not exported: take it from the symbol table
                for l in srv.qlist('SYMS %s %s' % (img, sym)):
                    a2, _, nm = l.partition(' ')
                    if nm in (sym, '_' + sym):
                        tgt = int(a2, 16)
            t0, n = [int(v, 0) for v in srv.q('TEXT ' + img).split()]
            out.write('\n# uses of %s (0x%x) in %s __text 0x%x+%d\n' % (sym, tgt, img, t0, n))
            funcs = []
            if tgt and t0:
                code = b''
                for o in range(0, n, 65536):
                    code += srv.read(t0 + o, min(65536, n - o))
                md = Cs(CS_ARCH_X86, CS_MODE_64); md.detail = True
                o = 0
                while o < len(code):
                    last = None
                    for ins in md.disasm(code[o:o + 4096], t0 + o):
                        for op in ins.operands:
                            if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP and \
                               tgt <= ins.address + ins.size + op.mem.disp < tgt + 0x40:
                                f = ins.address - t0
                                while f > 0 and code[f:f + 4] != b'\x55\x48\x89\xe5':
                                    f -= 1
                                if t0 + f not in funcs:
                                    funcs.append(t0 + f)
                                    out.write('#   function 0x%x uses it at 0x%x (%s %s)\n' % (t0 + f, ins.address, ins.mnemonic, ins.op_str))
                        last = ins.address + ins.size - t0
                    o = last if (last is not None and last > o) else o + 1
            out.flush()
            for f in funcs[:20]:
                todo += disasm(srv, f, 'uses %s' % sym, out, follow, seen)
        elif kind == 'i':
            # v0.52 (round 17): every function in IMG's __text that uses the constant VALUE as an immediate
            img, val = rest.split(':', 1)
            want = int(val, 0)
            t0, n = [int(v, 0) for v in srv.q('TEXT ' + img).split()]
            out.write('\n# immediate 0x%x in %s __text 0x%x+%d\n' % (want, img, t0, n))
            funcs = []
            if t0:
                code = b''
                for o in range(0, n, 65536):
                    code += srv.read(t0 + o, min(65536, n - o))
                md = Cs(CS_ARCH_X86, CS_MODE_64); md.detail = True
                o = 0
                while o < len(code):
                    last = None
                    for ins in md.disasm(code[o:o + 4096], t0 + o):
                        for op in ins.operands:
                            if op.type == X86_OP_IMM and (op.imm & 0xffffffff) == want:
                                f = ins.address - t0
                                while f > 0 and code[f:f + 4] != b'\x55\x48\x89\xe5':
                                    f -= 1
                                if t0 + f not in funcs:
                                    funcs.append(t0 + f)
                                    out.write('#   function 0x%x (%s) uses it at 0x%x (%s %s)\n' % (t0 + f, srv.name(t0 + f), ins.address, ins.mnemonic, ins.op_str))
                        last = ins.address + ins.size - t0
                    o = last if (last is not None and last > o) else o + 1
            out.flush()
            for f in funcs[:12]:
                todo += disasm(srv, f, 'uses 0x%x' % want, out, follow, seen)
        elif kind in ('v', 'cfs'):
            parts = rest.split(':')
            img, sym = parts[0], parts[1]
            tgt = int(srv.q('SYM ' + sym), 16)
            if not tgt:
                for l in srv.qlist('SYMS %s %s' % (img, sym)):
                    a2, _, nm = l.partition(' ')
                    if nm in (sym, '_' + sym):
                        tgt = int(a2, 16)
            if kind == 'v':
                n = int(parts[2], 0) if len(parts) > 2 else 16
                b = srv.read(tgt, n) if tgt else b''
                out.write('\n# bytes of %s at 0x%x: %s\n' % (sym, tgt, b.hex(' ')))
            else:
                out.write('\n# CFStrings loaded by %s (0x%x)\n' % (sym, tgt))
                code = srv.read(tgt, 1200) if tgt else b''
                md = Cs(CS_ARCH_X86, CS_MODE_64); md.detail = True
                for ins in md.disasm(code, tgt):
                    if ins.mnemonic == 'lea' and len(ins.operands) == 2 and ins.operands[1].type == X86_OP_MEM \
                       and ins.operands[1].mem.base == X86_REG_RIP:
                        a = ins.address + ins.size + ins.operands[1].mem.disp
                        cf = srv.read(a, 32)
                        txt = '?'
                        if len(cf) == 32:
                            ptr = int.from_bytes(cf[16:24], 'little'); ln = int.from_bytes(cf[24:32], 'little')
                            if 0 < ln < 200 and ptr:
                                sb = srv.read(ptr, ln)
                                if sb:
                                    txt = repr(sb.decode('latin-1'))
                        out.write('#   0x%x lea 0x%x -> %s\n' % (ins.address, a, txt))
                    if ins.mnemonic == 'ret':
                        break
            out.flush()
        elif kind == 'ivars':
            out.write('\n### ivars of %s\n' % rest)
            for l in srv.qlist('IVARS ' + rest):
                out.write('  %s\n' % l)
        out.flush()
    for level in range(depth):
        nxt = []
        for t, s2 in todo:
            nxt += disasm(srv, t, 'followed: ' + s2, out, follow if level + 1 < depth else None, seen)
        todo = nxt
    srv.q('QUIT')
    out.close()


if __name__ == '__main__':
    main()

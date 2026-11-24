#!/usr/bin/env python3
"""kcdis - disassemble selected functions of kexts inside a kernel collection
(interoperability analysis) and annotate them:
  * direct calls/jumps and RIP-relative references -> symbol names
  * calls through a vtable ([reg+disp]) -> slot number and the method names
    at that slot in the vtables given with --vt CLASS (from kcdump output)
Writes text only.

usage: kcdis.py OUT.txt VTDIR KC [KC...] -- BUNDLE_ID:REGEX [BUNDLE_ID:REGEX ...] [--vt CLASS ...]
"""
import re, sys, os, subprocess, bisect, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from kcdump import MachO
import capstone

def demangle_all(names):
    p = subprocess.run(['c++filt'], input='\n'.join(names), capture_output=True, text=True)
    out = p.stdout.split('\n')
    return {n: (out[i] if i < len(out) else n) for i, n in enumerate(names)}

def load_vtables(vtdir, classes):
    vt = {}
    for f in os.listdir(vtdir):
        if not f.startswith('12-') or not f.endswith('vtables.txt'):
            continue
        cur = None
        for line in open(os.path.join(vtdir, f), errors='replace'):
            m = re.match(r'^vtable for (\S+) @', line)
            if m:
                cur = m.group(1) if m.group(1) in classes and m.group(1) not in vt else None
                if cur: vt[cur] = {}
                continue
            m = re.match(r'\s*\[\s*(\d+)\]\s+(.*)$', line.rstrip())
            if m and cur:
                vt[cur][int(m.group(1))] = m.group(2)
    return vt

def main():
    args = sys.argv[1:]
    vtcls = []
    while '--vt' in args:
        i = args.index('--vt'); vtcls.append(args[i + 1]); del args[i:i + 2]
    out, vtdir = args[0], args[1]
    sep = args.index('--')
    kcs, wants = args[2:sep], [w.split(':', 1) for w in args[sep + 1:]]
    vt = load_vtables(vtdir, set(vtcls))

    # chained-fixup bases chosen by kcdump (same OUT dir)
    bases = []
    try:
        for line in open(os.path.join(vtdir, '03-kcdump-log.txt')):
            if line.startswith('chosen bases:'):
                bases = [int(x.strip(), 16) for x in line.split(':', 1)[1].split(',')]
    except OSError:
        pass
    tops = []
    entries, addr2name, allsyms = {}, {}, []
    for path in kcs:
        data = open(path, 'rb').read()
        top = MachO(data, 0)
        tops.append((data, top))
        for name, vm, fo in top.fileset_entries():
            try:
                m = MachO(data, fo)
            except Exception:
                continue
            syms = m.symbols()
            entries[name] = (data, m, syms)
            for n, t, v in syms:
                if v and (t & 0x0e) == 0x0e:
                    addr2name.setdefault(v, n)
    addrs = sorted(addr2name)
    dem = demangle_all([addr2name[a] for a in addrs])

    def stub_target(a):
        """KC branch stub 'jmp qword ptr [rip+d]' -> name of the final target"""
        for data, top in tops:
            fo = top.vm_to_file(a)
            if fo is None or data[fo:fo + 2] != b'\xff\x25':
                continue
            d = struct.unpack_from('<i', data, fo + 2)[0]
            slot = a + 6 + d
            sfo = top.vm_to_file(slot)
            if sfo is None:
                return None
            raw = struct.unpack_from('<Q', data, sfo)[0]
            level = (raw >> 30) & 3
            if level < len(bases):
                vm = bases[level] + (raw & 0x3FFFFFFF)
                if vm in addr2name:
                    return 'stub -> ' + dem.get(addr2name[vm], addr2name[vm])
            return 'stub -> raw 0x%x' % raw
        return None

    def name_at(a):
        if a not in addr2name:
            st = stub_target(a)
            if st:
                return st
        i = bisect.bisect_right(addrs, a) - 1
        if i < 0: return '0x%x' % a
        base = addrs[i]
        n = dem.get(addr2name[base], addr2name[base])
        return n if a == base else '%s+0x%x' % (n, a - base)

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    o = open(out, 'w')
    for bid, rx in wants:
        if bid not in entries:
            o.write('### %s not found\n' % bid); continue
        data, m, syms = entries[bid]
        own = sorted((v, n) for n, t, v in syms if v and (t & 0x0e) == 0x0e)
        names = demangle_all([n for v, n in own])
        r = re.compile(rx)
        for idx, (v, n) in enumerate(own):
            dn = names[n]
            if not r.search(dn) or dn.startswith('vtable for') or '::MetaClass' in dn:
                continue
            end = own[idx + 1][0] if idx + 1 < len(own) else v + 0x400
            size = min(end - v, 0x2000)
            fo = m.vm_to_file(v)
            if fo is None: continue
            o.write('\n### %s  @0x%x  (%d bytes)\n' % (dn, v, size))
            # round 35: data tables (method tables, token handler tables, ...): 8-byte words, pointers decoded
            if re.search(r'(method_descs|_methods|_table|Table|_descs|Descs|Methods|DispatchDescs)$', dn):
                for q in range(0, size - 7, 8):
                    raw = struct.unpack_from('<Q', data, fo + q)[0]
                    level = (raw >> 30) & 3
                    nm = ''
                    if raw and level < len(bases):
                        if True:
                            tv = bases[level] + (raw & 0x3FFFFFFF)
                            if tv in addr2name or (addrs and addrs[0] <= tv <= addrs[-1] + 0x100000):
                                nm = name_at(tv)
                    o.write('  +%03x: %016x  %s\n' % (q, raw, nm))
                continue
            insl = list(md.disasm(data[fo:fo + size], v))
            # v0.52 (round 17): switch jump tables - 'lea r, [rip+T]' then 'movsxd x, dword ptr [r + i*4]'
            jt = {}
            for k, ins in enumerate(insl):
                if ins.mnemonic == 'lea' and 'rip' in ins.op_str:
                    tgt = ins.address + ins.size + ins.operands[1].mem.disp
                    nxt = ' '.join(i2.mnemonic + ' ' + i2.op_str for i2 in insl[k + 1:k + 3])
                    if 'movsxd' in nxt and '*4]' in nxt:
                        tf = m.vm_to_file(tgt)
                        ents = []
                        for e in range(256):
                            if tf is None: break
                            d = struct.unpack_from('<i', data, tf + 4 * e)[0]
                            a2 = tgt + d
                            if not (v <= a2 < v + size): break
                            ents.append('[%d]=%x' % (e, a2))
                        if ents: jt[ins.address] = ents
            for ins in insl:
                note = ''
                ops = ins.op_str
                if ins.mnemonic.startswith(('call', 'j')) and ins.operands and ins.operands[0].type == capstone.x86.X86_OP_IMM:
                    note = name_at(ins.operands[0].imm)
                elif 'rip' in ops:
                    for op in ins.operands:
                        if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
                            tgt = ins.address + ins.size + op.mem.disp
                            note = name_at(tgt)
                            sf = m.vm_to_file(tgt)
                            if sf is not None and ins.mnemonic == 'lea':
                                raw = data[sf:sf + 80].split(b'\0')[0]
                                if 3 <= len(raw) < 80 and all(32 <= c < 127 for c in raw):
                                    note += '  "%s"' % raw.decode()
                elif ins.mnemonic == 'call' and ins.operands and ins.operands[0].type == capstone.x86.X86_OP_MEM:
                    disp = ins.operands[0].mem.disp
                    if disp % 8 == 0 and 0 <= disp < 0x1000:
                        slot = disp // 8 + 2
                        cands = []
                        for c in vtcls:
                            s = vt.get(c, {}).get(slot)
                            if s and s not in ('0',) and s not in cands: cands.append(s)
                        note = 'vtable slot %d: %s' % (slot, ' | '.join(cands[:3]) if cands else '?')
                o.write('  %x: %-8s %-48s %s\n' % (ins.address, ins.mnemonic, ops, ('; ' + note) if note else ''))
                if ins.address in jt:
                    o.write('      ; jump table: %s\n' % ' '.join(jt[ins.address]))
                if ins.mnemonic in ('ret', 'ud2') and ins.address + ins.size >= v + size - 16:
                    break
    o.close()

if __name__ == '__main__':
    main()

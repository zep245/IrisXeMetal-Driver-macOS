#!/usr/bin/env python3
"""kcdump - read kexts out of a macOS kernel collection (Mach-O MH_FILESET)
without extra tools, and write TEXT only:

  * symbol table of each requested kext (C++ symbols demangled later)
  * every C++ vtable (__ZTV...) with its slots resolved to function names
    (x86_64 kernel-cache chained fixups decoded), i.e. the virtual method
    order a subclass has to match

Formats from Apple's open source headers: mach-o/loader.h (fileset_entry_command,
segment_command_64, symtab_command), mach-o/nlist.h, mach-o/fixup-chains.h
(dyld_chained_ptr_x86_64_kernel_cache_rebase).

usage: kcdump.py OUTDIR KC [KC ...] -- BUNDLE_ID [BUNDLE_ID ...]
"""
import struct, sys, os, bisect, re

LC_SEGMENT_64 = 0x19
LC_SYMTAB = 0x2
LC_FILESET_ENTRY = 0x80000035
MH_MAGIC_64 = 0xFEEDFACF


class MachO:
    def __init__(self, data, off):
        self.data, self.off = data, off
        magic, cpu, sub, ftype, ncmds, sizeofcmds, flags, _ = struct.unpack_from('<IiiIIIII', data, off)
        if magic != MH_MAGIC_64:
            raise ValueError('not a 64-bit Mach-O at 0x%x' % off)
        self.ftype = ftype
        self.cmds = []
        p = off + 32
        for _ in range(ncmds):
            cmd, size = struct.unpack_from('<II', data, p)
            self.cmds.append((cmd, p, size))
            p += size
        self.segments = []
        self.symtab = None
        for cmd, p, size in self.cmds:
            if cmd == LC_SEGMENT_64:
                name = data[p + 8:p + 24].split(b'\0')[0].decode()
                vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<QQQQ', data, p + 24)
                self.segments.append((name, vmaddr, vmsize, fileoff, filesize))
            elif cmd == LC_SYMTAB:
                self.symtab = struct.unpack_from('<IIII', data, p + 8)

    def fileset_entries(self):
        out = []
        for cmd, p, size in self.cmds:
            if cmd == LC_FILESET_ENTRY:
                vmaddr, fileoff, stroff = struct.unpack_from('<QQI', self.data, p + 8)
                name = self.data[p + stroff:p + size].split(b'\0')[0].decode()
                out.append((name, vmaddr, fileoff))
        return out

    def symbols(self):
        """(name, n_type, n_value) - offsets in a KC are relative to the whole file"""
        if not self.symtab:
            return []
        symoff, nsyms, stroff, strsize = self.symtab
        res = []
        for i in range(nsyms):
            strx, ntype, nsect, ndesc, value = struct.unpack_from('<IBBHQ', self.data, symoff + i * 16)
            if strx == 0 or strx >= strsize:
                continue
            end = self.data.find(b'\0', stroff + strx)
            name = self.data[stroff + strx:end].decode(errors='replace')
            res.append((name, ntype, value))
        return res

    def vm_to_file(self, vm):
        for name, vmaddr, vmsize, fileoff, filesize in self.segments:
            if vmaddr <= vm < vmaddr + filesize:
                return fileoff + (vm - vmaddr)
        return None


def main():
    if '--' not in sys.argv:
        print(__doc__); sys.exit(1)
    sep = sys.argv.index('--')
    outdir, kcs, ids = sys.argv[1], sys.argv[2:sep], sys.argv[sep + 1:]
    os.makedirs(outdir, exist_ok=True)
    log = open(os.path.join(outdir, '03-kcdump-log.txt'), 'w')

    kc_info = []          # (path, data, top MachO, base vmaddr, entries)
    for path in kcs:
        data = open(path, 'rb').read()
        top = MachO(data, 0)
        base = min(s[1] for s in top.segments if s[3] == 0) if any(s[3] == 0 for s in top.segments) \
            else min(s[1] for s in top.segments)
        entries = top.fileset_entries()
        kc_info.append((path, data, top, base, entries))
        log.write('%s: filetype %d, base 0x%x, %d entries\n' % (path, top.ftype, base, len(entries)))
        with open(os.path.join(outdir, '02-%s-entries.txt' % os.path.basename(path)), 'w') as f:
            for name, vm, fo in entries:
                f.write('%-60s vm 0x%x file 0x%x\n' % (name, vm, fo))

    # global symbol index (all entries of all KCs) for resolving vtable slots
    addr_names = {}
    per_entry = {}
    for path, data, top, base, entries in kc_info:
        for name, vm, fo in entries:
            try:
                m = MachO(data, fo)
            except Exception as e:
                log.write('  %s: %s\n' % (name, e)); continue
            syms = m.symbols()
            per_entry[name] = (path, data, m, base, syms)
            if name == 'com.apple.kernel':
                log.write('kernel: %d symbols\n' % len(syms))
            for sname, ntype, value in syms:
                if value and (ntype & 0x0e) == 0x0e:          # N_SECT
                    addr_names.setdefault(value, sname)
    addrs = sorted(addr_names)
    log.write('symbol index: %d addresses\n' % len(addrs))

    bases = [k[3] for k in kc_info]
    # candidate starts for the chained-fixup targets of each level: the KC
    # header, the first entry (kernel for level 0), or the 0xffffff8000000000
    # kernel base. Choose by counting resolved vtable slots.
    level_cands = []
    for path, data, top, base, entries in kc_info:
        c = [base]
        if entries:
            c.append(min(e[1] for e in entries))
        c += [0xffffff8000000000, 0xffffff8000100000, 0]
        level_cands.append(list(dict.fromkeys(c)))
    samples = []
    for bid in ids:
        if bid in per_entry:
            path, data, m, base, syms = per_entry[bid]
            for n, t, v in syms:
                if n.startswith('__ZTV') and v:
                    fo = m.vm_to_file(v)
                    if fo is not None:
                        for k in range(2, 40):
                            samples.append(struct.unpack_from('<Q', data, fo + 8 * k)[0])
    for lvl, cands in enumerate(level_cands):
        best, bestHits = bases[lvl], -1
        for c in cands:
            hits = sum(1 for r in samples if r and ((r >> 30) & 3) == lvl and (c + (r & 0x3FFFFFFF)) in addr_names)
            log.write('level %d base 0x%x: %d hits\n' % (lvl, c, hits))
            if hits > bestHits:
                best, bestHits = c, hits
        bases[lvl] = best
    log.write('chosen bases: %s\n' % ', '.join('0x%x' % b for b in bases))

    def resolve(raw, own_base):
        # dyld_chained_ptr_x86_64_kernel_cache_rebase:
        # target:30 cacheLevel:2 diversity:16 addrDiv:1 key:2 next:12 isAuth:1
        # target is an offset from the start of the collection named by
        # cacheLevel (0 = boot KC incl. the kernel, 1 = system KC)
        target = raw & 0x3FFFFFFF
        level = (raw >> 30) & 3
        if level >= len(bases):
            return None, raw
        vm = bases[level] + target
        if vm in addr_names:
            return addr_names[vm], vm
        tag = 'kernel' if level == 0 else 'kc%d' % level
        return None, '%s+0x%x' % (tag, target)

    for bid in ids:
        if bid not in per_entry:
            log.write('not found: %s\n' % bid); continue
        path, data, m, base, syms = per_entry[bid]
        log.write('%s: %d symbols, %d segments (in %s)\n' % (bid, len(syms), len(m.segments), path))
        with open(os.path.join(outdir, '10-%s-symbols.txt' % bid), 'w') as f:
            for sname, ntype, value in sorted(syms, key=lambda s: (s[2], s[0])):
                f.write('%016x %02x %s\n' % (value, ntype, sname))
        with open(os.path.join(outdir, '11-%s-segments.txt' % bid), 'w') as f:
            for s in m.segments:
                f.write('%-16s vm 0x%x size 0x%x file 0x%x fsize 0x%x\n' % s)
        # vtables: from __ZTV symbol to the next symbol
        own = sorted((v, n) for n, t, v in syms if v and (t & 0x0e) == 0x0e)
        with open(os.path.join(outdir, '12-%s-vtables.txt' % bid), 'w') as f:
            for idx, (v, n) in enumerate(own):
                if not n.startswith('__ZTV'):
                    continue
                end = own[idx + 1][0] if idx + 1 < len(own) else v + 8 * 400
                count = min((end - v) // 8, 600)
                fo = m.vm_to_file(v)
                if fo is None:
                    f.write('%s: not in a file-backed segment\n\n' % n); continue
                f.write('%s @0x%x (%d slots)\n' % (n, v, count))
                for k in range(count):
                    raw = struct.unpack_from('<Q', data, fo + 8 * k)[0]
                    if raw == 0:
                        f.write('  [%3d] 0\n' % k); continue
                    name, where = resolve(raw, base)
                    f.write('  [%3d] %s\n' % (k, name if name else '(%s)' % where))
                f.write('\n')
        # strings from the kext's segments
        with open(os.path.join(outdir, '13-%s-strings.txt' % bid), 'w') as f:
            seen = set()
            for sname, vmaddr, vmsize, fileoff, filesize in m.segments:
                if sname.startswith('__LINKEDIT'):
                    continue
                for s in re.findall(rb'[\x20-\x7e]{5,}', data[fileoff:fileoff + filesize]):
                    if s not in seen:
                        seen.add(s); f.write(s.decode() + '\n')
    log.close()


if __name__ == '__main__':
    main()

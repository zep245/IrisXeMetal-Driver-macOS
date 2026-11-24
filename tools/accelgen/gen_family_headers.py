#!/usr/bin/env python3
"""Generate C++ headers for Apple's IOAcceleratorFamily2 classes (no public
headers exist) from data/family.vtables.txt (vtable listings made by
tools/metal-research/kcdump.py on macOS 15).

Each class header declares only the virtual methods the class introduces, in
slot order, so a kext subclass gets Apple's exact vtable layout. Overrides of
inherited methods are not needed: the kext linker patches subclass vtables
from the parent's real vtable (verified on hardware in v0.16.0, step 1a).
Exceptions written into the header:
  * pure virtual methods of an ancestor that this class implements, so the
    class is not abstract for our subclass;
  * nothing else - methods Apple does not export are listed in the summary
    file, and our subclasses must override them.

usage: gen_family_headers.py OUTDIR
"""
import os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
VT = os.path.join(HERE, 'data', 'family.vtables.txt')
EXPORTS = os.path.join(HERE, 'data', 'IOAcceleratorFamily2.exports.txt')

# class -> (parent, first own slot when the parent is a kernel class)
CHAINS = {
    'IOGraphicsAccelerator2':  ('IOService', 268),
    'IOAccelEventMachine2':    ('OSObject', 37),
    'IOAccelEventMachineFast2': ('IOAccelEventMachine2', None),
    'IOAccelSubmitter2':       ('IOUserClient', 302),
    'IOAccelContext2':         ('IOAccelSubmitter2', None),
    'IOAccelGLContext2':       ('IOAccelContext2', None),
    'IOAccel2DContext2':       ('IOAccelContext2', None),
    'IOAccelVideoContext2':    ('IOAccelContext2', None),          # v0.67.1: hardware video decoding
    'IOAccelDevice2':          ('IOUserClient', 302),           # v0.68.2: the device connection (Intel selectors 10..24)
    'IOAccelSurface':          ('IOUserClient', 302),
    'IOAccelMemory':           ('OSObject', 37),
    'IOAccelVidMemory':        ('IOAccelMemory', None),
    'IOAccelSysMemory':        ('IOAccelMemory', None),
    'IOAccelCommandQueue':     ('IOAccelSubmitter2', None),
    'IOAccelSegmentResourceList': ('OSObject', 37),
    'IOAccelMemoryMap':        ('OSObject', 37),
    'IOAccelResource2':        ('OSObject', 37),
    'IOAccelDisplayMachine':   ('IOService', 268),
    'IOAccelDisplayPipe':      ('IOService', 268),          # v0.56.0
}
# v0.56.0: exported NON-virtual methods a subclass calls (declared inside the class so the names mangle right)
EXTRA = {
    'IOAccelDisplayPipe': [
        'virtual void free() override;      // IOAccelDisplayPipe::free() (exported) - so a subclass chains to it',
        'IOFramebuffer *getFramebuffer() const;',
        'unsigned int getFramebufferIndex() const;',
        'void signalVBLInterrupt(void *);',
        'void signalTransactionInterrupt(void *);',
    ],
}
# v0.67.1: IOAccelContext2::getDataBufferLimits() returns a 0x308-byte structure (research round 36: it bzero's
# 0x308 bytes at rdi and returns rdi). Return types are not mangled, so declaring it changes only the calling
# convention (sret), not the symbol.
RET_STRUCT = {'getDataBufferLimits': 'IOAccelDataBufferLimits'}
RET_STRUCT_DEFS = ('#ifndef IRISXE_DATA_BUFFER_LIMITS\n#define IRISXE_DATA_BUFFER_LIMITS\n'
                   'struct IOAccelDataBufferLimits { UInt8 bytes[0x308]; };   // v0.67.1: returned in memory (sret)\n'
                   '#endif\n\n')
KERNEL_INCLUDE = {'OSObject': '<libkern/c++/OSObject.h>', 'IOService': '<IOKit/IOService.h>',
                  'IOUserClient': '<IOKit/IOUserClient.h>'}
KNOWN = {'IOService', 'OSDictionary', 'OSArray', 'OSObject', 'OSSymbol', 'OSString', 'OSData',
         'OSCollection', 'OSSerialize', 'OSMetaClass', 'OSMetaClassBase', 'IOUserClient',
         'IOPCIDevice', 'IOMemoryDescriptor', 'IOWorkLoop', 'IOPowerConnection', 'OSNumber',
         'IOPMPowerState', 'IOBufferMemoryDescriptor', 'IODMACommand', 'task', 'IOMemoryMap',
         'IOExternalMethodArguments', 'IOExternalMethodDispatch', 'IOCommandGate',
         'IOInterruptEventSource', 'IOTimerEventSource', 'IOEventSource', 'IOLock', 'IORecursiveLock'}
BUILTIN = {'unsigned', 'int', 'char', 'long', 'short', 'bool', 'void', 'const', 'volatile', 'float', 'double',
           'signed', '__int128', 'wchar_t'}
INCLUDES = ['<IOKit/IOService.h>', '<IOKit/IOUserClient.h>', '<IOKit/IOMemoryDescriptor.h>',
            '<IOKit/IOBufferMemoryDescriptor.h>', '<IOKit/IODMACommand.h>', '<IOKit/IOWorkLoop.h>',
            '<IOKit/IOCommandGate.h>', '<IOKit/IOInterruptEventSource.h>', '<IOKit/IOTimerEventSource.h>',
            '<IOKit/IOLocks.h>', '<IOKit/pci/IOPCIDevice.h>']


def load():
    vt, cur = {}, None
    for line in open(VT):
        m = re.match(r'^vtable for (.+?) @0x[0-9a-f]+ \((\d+) slots\)', line)
        if m:
            cur = m.group(1)
            if cur in vt:
                cur = None          # keep the first listing only
            else:
                vt[cur] = []
            continue
        m = re.match(r'\s*\[\s*(\d+)\]\s+(.*)$', line.rstrip())
        if m and cur is not None:
            vt[cur].append(m.group(2))
    for k in vt:
        while vt[k] and vt[k][-1] == '0':
            vt[k].pop()
    return vt


def split(sig):
    m = re.match(r'^(?:([\w:]+)::)?(~?\w+)\((.*)\)(\s*const)?$', sig)
    if not m:
        return None
    return m.group(1), m.group(2), m.group(3), bool(m.group(4))


def mname(sig):
    p = split(sig)
    return (p[1], p[2], p[3]) if p else sig


PURE = '___cxa_pure_virtual'


def main():
    outdir = sys.argv[1]
    os.makedirs(outdir, exist_ok=True)
    vt = load()
    exports = set(l.rstrip('\n') for l in open(EXPORTS))

    def descendants_name(cls, k):
        """name for pure slot k of cls, taken from any class whose vtable
        agrees with cls on cls's named own slots (a subclass)"""
        base = vt[cls]
        named = [i for i, s in enumerate(base) if i >= 2 and s != PURE and s != '0']
        for other, ov in vt.items():
            if other == cls or len(ov) <= k or ov[k] in (PURE, '0'):
                continue
            ok = all(i < len(ov) and mname(ov[i]) == mname(base[i]) for i in named[-40:])
            if ok:
                return ov[k]
        return None

    order, done = [], set()

    def visit(c):
        if c in done or c not in CHAINS:
            return
        visit(CHAINS[c][0])
        done.add(c); order.append(c)

    for c in CHAINS:
        visit(c)

    first_slot, pure_after, declared = {}, {}, {}
    summary = []
    for c in order:
        parent, first = CHAINS[c]
        if c not in vt:
            raise SystemExit('no vtable data for %s' % c)
        if first is None:
            first = len(vt[parent])
        first_slot[c] = first
        v = vt[c]
        # sanity: slots before `first` must be the parent's methods (same names)
        mism = [k for k in range(2, first) if parent in vt and k < len(vt[parent])
                and v[k] != PURE and vt[parent][k] != PURE and mname(v[k]) != mname(vt[parent][k])]
        pure_in = set(pure_after.get(parent, set()))
        decl_sym = dict(declared.get(parent, {}))     # slot -> symbol our compiler will reference
        lines, fwd, own_pure, impl = [], set(), [], []

        def emit(k, sig, is_pure, override):
            owner, name, params, const = split(sig)
            for ident in re.findall(r'[A-Za-z_]\w*', params):
                if ident not in BUILTIN and ident not in KNOWN:
                    fwd.add(ident)
            note = ''
            sym = '%s::%s(%s)%s' % (c, name, params, ' const' if const else '')
            if not is_pure and sym not in exports:
                note = '  NOT EXPORTED'
            # v0.67.1: functions that really return a struct in memory (x86-64 sret: hidden pointer in rdi, this in rsi)
            ret = RET_STRUCT.get(name, 'IRISXE_SLOT_RET')
            lines.append('    virtual %s %s(%s)%s%s;   // slot %d%s' % (
                ret, name, params, ' const' if const else '',
                ' = 0' if is_pure else (' override' if override else ''), k, note))
            decl_sym[k] = None if is_pure else sym

        # ancestor pure slots this class implements
        for k in sorted(pure_in):
            if k < len(v) and v[k] != PURE:
                emit(k, v[k], False, True)
                impl.append(k)
        pure_in -= set(impl)
        # own slots
        for k in range(first, len(v)):
            sig = v[k]
            is_pure = sig == PURE
            if is_pure:
                sig = descendants_name(c, k)
                if not sig:
                    raise SystemExit('%s slot %d: pure, no subclass names it' % (c, k))
                own_pure.append(k)
            emit(k, sig, is_pure, False)
        pure_in |= set(own_pure)
        pure_after[c], declared[c] = pure_in, decl_sym

        inc = '"%s.h"' % parent if parent in CHAINS else KERNEL_INCLUDE[parent]
        with open(os.path.join(outdir, c + '.h'), 'w') as f:
            f.write('// GENERATED by tools/accelgen/gen_family_headers.py - do not edit\n')
            f.write('// %s : %s, own virtual slots %d..%d (macOS 15 kernel collection)\n' % (c, parent, first, len(v) - 1))
            f.write('// Return types are not mangled; IRISXE_SLOT_RET is a pointer-sized placeholder.\n')
            f.write('#pragma once\n')
            for i in INCLUDES:
                f.write('#include %s\n' % i)
            f.write('#include %s\n\n#ifndef IRISXE_SLOT_RET\n#define IRISXE_SLOT_RET uintptr_t\n#endif\n\n' % inc)
            if c in ('IOAccelContext2',):
                f.write(RET_STRUCT_DEFS)
            for t in sorted(fwd - set(CHAINS)):
                f.write('class %s;\n' % t)
            for t in sorted(fwd & set(CHAINS)):
                f.write('class %s;\n' % t)
            f.write('\nclass %s : public %s {\n    OSDeclareAbstractStructors(%s)\npublic:\n' % (c, parent, c))
            extra = ['    // exported non-virtual methods (EXTRA in gen_family_headers.py)'] + \
                    ['    %s' % e for e in EXTRA[c]] if c in EXTRA else []
            f.write('\n'.join(lines + extra) + '\n};\n')
        summary.append('%s : %s  own slots %d..%d  (%d own pure)  vtable %d slots  parent-name mismatches %d' % (
            c, parent, first, len(v) - 1, len(own_pure), len(v), len(mism)))
        # what a concrete subclass of c must implement
        must = []
        for k in sorted(pure_in):
            must.append('    pure  %3d %s' % (k, descendants_name(c, k) or '?'))
        for k, sym in sorted(decl_sym.items()):
            if sym and sym not in exports:
                must.append('    noexp %3d %s' % (k, sym))
        summary.append('  subclass must implement %d:' % len(must))
        summary.extend(must)
    with open(os.path.join(outdir, 'SUMMARY.txt'), 'w') as f:
        f.write('\n'.join(summary) + '\n')
    print('\n'.join(summary))


if __name__ == '__main__':
    main()

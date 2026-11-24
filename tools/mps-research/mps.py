# lldb script (research, v0.51.13): find the class MPSAutoCache::MPSAutoCache allocates (objc_alloc after a
# "lea rdi, [rip + X]"), ask the Objective-C runtime for its -initWithCommandBuffer:, print the disassembly of it
# and of the functions it calls inside MPSCore
import lldb, re

def ev(t, expr):
    v = t.EvaluateExpression(expr)
    return v

def dis(t, addr, count, title):
    print("== %s at %#x" % (title, addr))
    a = lldb.SBAddress(addr, t)
    ins = t.ReadInstructions(a, count, "intel")
    out = []
    for i in ins:
        line = "%#x: %s %s ; %s" % (i.GetAddress().GetLoadAddress(t), i.GetMnemonic(t), i.GetOperands(t), i.GetComment(t))
        out.append((i, line))
        print(line)
        if i.GetMnemonic(t) == "ret" and len(out) > 5:
            # keep going a little: functions often have code after the first ret
            pass
    return out

def dump(debugger, command, result, internal_dict):
    t = debugger.GetSelectedTarget()
    p = t.GetProcess()
    ctor = None
    for m in t.module_iter():
        if m.GetFileSpec().GetFilename() != "MPSCore":
            continue
        for s in m:
            n = s.GetName() or ""
            if n.startswith("MPSAutoCache::MPSAutoCache(") and s.GetInstructions(t).GetSize() > 20:
                ctor = s
    if not ctor:
        print("MPSAutoCache ctor not found"); return
    ins = ctor.GetInstructions(t, "intel")
    cls = 0
    for k in range(ins.GetSize() - 1):
        i, j = ins.GetInstructionAtIndex(k), ins.GetInstructionAtIndex(k + 1)
        if i.GetMnemonic(t) == "lea" and i.GetOperands(t).startswith("rdi, [rip") and "objc_alloc" in (j.GetComment(t) or ""):
            disp = int(re.search(r"0x[0-9a-f]+", i.GetOperands(t)).group(0), 16)
            ref = i.GetAddress().GetLoadAddress(t) + i.GetByteSize() + disp
            # objc_alloc gets the class itself (classref already loaded? it's a lea: the class object address)
            cls = ref
            break
    print("class object at %#x" % cls)
    nm = ev(t, "(const char *)class_getName((Class)%d)" % cls)
    print("class name: %s" % nm.GetSummary())
    for sel in ("initWithCommandBuffer:", "dealloc"):
        imp = ev(t, "(unsigned long)class_getMethodImplementation((Class)%d, (SEL)sel_registerName(\"%s\"))" % (cls, sel))
        a = imp.GetValueAsUnsigned()
        print("%s -> %#x" % (sel, a))
        if sel != "initWithCommandBuffer:" or not a:
            continue
        lines = dis(t, a, 400, "-[%s %s]" % (nm.GetSummary(), sel))
        # one level down: direct calls into MPSCore (not stubs), first 6
        seen = set()
        for i, line in lines:
            if i.GetMnemonic(t) != "call":
                continue
            m = re.match(r"0x[0-9a-f]+$", i.GetOperands(t).strip())
            if not m:
                continue
            tgt = int(m.group(0), 16)
            sa = lldb.SBAddress(tgt, t)
            sym = sa.GetSymbol()
            sn = sym.GetName() if sym else ""
            if "stub" in (i.GetComment(t) or "") or tgt in seen or not sym:
                continue
            if sa.GetModule().GetFileSpec().GetFilename() != "MPSCore":
                continue
            seen.add(tgt)
            if len(seen) > 6:
                break
            dis(t, sym.GetStartAddress().GetLoadAddress(t), min(300, sym.GetInstructions(t).GetSize()), "callee " + sn)

def __lldb_init_module(debugger, internal_dict):
    debugger.HandleCommand("command script add -f mps.dump mpsdump")

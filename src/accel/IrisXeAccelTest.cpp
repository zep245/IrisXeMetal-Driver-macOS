// IrisXeAccelTest - milestone 1 step 1a self test (boot-arg -irisxeacceltest).
// Separate file on purpose: it reads the two vtables by symbol name, so it must
// not see the C++ class definitions that own those symbols.
#include <IOKit/IOService.h>
#include <libkern/libkern.h>
#include <string.h>

class IrisXeAccelTest : public IOService {
    OSDeclareDefaultStructors(IrisXeAccelTest)
    typedef IOService super;
public:
    virtual IOService *probe(IOService *provider, SInt32 *score) override;
    virtual bool start(IOService *provider) override;
};

// created, started or registered.

OSDefineMetaClassAndStructors(IrisXeAccelTest, IOService)

#include "IrisXeVtCheck.inc"

struct VtCheck { const char *ours, *apple; void **ov, **av; UInt32 n; UInt16 over[24]; };
static const VtCheck kChecks[] = { IRISXE_VT_TABLE };

// vtables (symbols exported by IOAcceleratorFamily2 / defined by us)
extern "C" void *_ZTV22IOGraphicsAccelerator2[];   // Apple's (exported by IOAcceleratorFamily2)
extern "C" void *_ZTV17IrisXeAccelerator[];        // ours (IrisXeAccel.cpp)

#define VT_FIRST   2      // [0] offset-to-top, [1] RTTI
#define VT_SLOTS   352    // last real slot is 351
static const UInt16 kOverridden[] = { 2, 3, 9, 185, 186, 350, 270, 271, 273, 275, 309, 312, 314, 315, 316, 322,
                                      326, 327, 328, 329, 330, 331, 334, 335, 336, 339 };

IOService *IrisXeAccelTest::probe(IOService *provider, SInt32 *score) {
    if (!PE_parse_boot_argn("-irisxeacceltest", nullptr, 0)) return nullptr;
    return super::probe(provider, score);
}

bool IrisXeAccelTest::start(IOService *provider) {
    if (!super::start(provider)) return false;
    OSDictionary *d = OSDictionary::withCapacity(32);
    char buf[256];

    // 1. vtable layout: every slot we do NOT override must point to the same
    //    function as Apple's IOGraphicsAccelerator2 vtable
    UInt32 same = 0, diff = 0, ovr = 0;
    char firstDiff[160] = "";
    for (UInt32 k = VT_FIRST; k < VT_SLOTS; k++) {
        bool isOvr = false;
        for (UInt16 o : kOverridden) if (o == k) isOvr = true;
        void *a = _ZTV22IOGraphicsAccelerator2[k], *b = _ZTV17IrisXeAccelerator[k];
        if (isOvr) { ovr++; continue; }
        if (a == b) same++;
        else {
            if (!diff) snprintf(firstDiff, sizeof(firstDiff), "slot %u: apple %p ours %p", k, a, b);
            diff++;
        }
    }
    snprintf(buf, sizeof(buf), "%s: %u slots identical, %u overridden, %u DIFFERENT %s",
             diff ? "MISMATCH" : "OK", same, ovr, diff, firstDiff);
    if (d) d->setObject("VtableCheck", OSString::withCString(buf));

    // 1b. the eight helper subclasses (step 1b): same check per class, and
    //     our allocation size vs Apple's real class size
    UInt32 allDiff = 0;
    for (const VtCheck &c : kChecks) {
        UInt32 s2 = 0, d2 = 0, o2 = 0, fd = 0;
        for (UInt32 k = VT_FIRST; k < c.n; k++) {
            bool isOvr = false;
            for (UInt16 o : c.over) if (o && o == k) isOvr = true;
            if (isOvr) { o2++; continue; }
            if (c.ov[k] == c.av[k]) s2++; else { if (!d2) fd = k; d2++; }
        }
        allDiff += d2;
        const OSMetaClass *om = OSMetaClass::getMetaClassWithName(OSSymbol::withCStringNoCopy(c.ours));
        const OSMetaClass *am = OSMetaClass::getMetaClassWithName(OSSymbol::withCStringNoCopy(c.apple));
        UInt32 os = om ? om->getClassSize() : 0, as = am ? am->getClassSize() : 0;
        char e[200];
        snprintf(e, sizeof(e), "%s: %u same, %u ours, %u DIFFERENT%s%u | size %u vs Apple %u %s",
                 d2 ? "MISMATCH" : "OK", s2, o2, d2, d2 ? " first slot " : "", d2 ? fd : 0,
                 os, as, os >= as ? "ok" : "TOO SMALL");
        if (d) d->setObject(c.ours, OSString::withCString(e));
    }
    if (d) d->setObject("Step1b", OSString::withCString(allDiff ? "MISMATCH in helper classes" : "all helper vtables OK"));

    // 2. real instance sizes of the Apple classes we will subclass vs our padding
    static const char *classes[] = {
        "IOGraphicsAccelerator2", "IOAccelEventMachineFast2", "IOAccelEventMachine2",
        "IOAccelSharedUserClient2", "IOAccelShared2", "IOAccelCommandQueue",
        "IOAccelSysMemory", "IOAccelVidMemory", "IOAccelMemoryMap", "IOAccelResource2",
        "IOAccelDisplayMachine", "IOAccelDisplayPipe", "IOAccel2DContext2", "IOAccelGLContext2",
        "IOAccelCLContext2", "IOAccelSurface", "IOAccelTask", "IOAccelDevice2", "IOAccelContext2",
        "IOService" };
    char sizes[700] = "";
    for (const char *c : classes) {
        const OSMetaClass *mc = OSMetaClass::getMetaClassWithName(OSSymbol::withCStringNoCopy(c));
        char e[64];
        snprintf(e, sizeof(e), "%s=%u ", c, mc ? mc->getClassSize() : 0);
        strlcat(sizes, e, sizeof(sizes));
    }
    if (d) d->setObject("ClassSizes", OSString::withCString(sizes));
    const OSMetaClass *ours = OSMetaClass::getMetaClassWithName(OSSymbol::withCStringNoCopy("IrisXeAccelerator"));
    snprintf(buf, sizeof(buf), "IrisXeAccelerator=%u (must be >= IOGraphicsAccelerator2)",
             ours ? ours->getClassSize() : 0);
    if (d) d->setObject("OurSize", OSString::withCString(buf));

    if (d) { setProperty("IrisXe-AccelTest", d); d->release(); }
    registerService();
    return true;
}

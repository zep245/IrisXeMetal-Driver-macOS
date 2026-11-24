// IrisXeAccelMachines - hand-written parts of our event machine and display
// machine (milestone 1 step 1c). The rest of both classes is generated
// (IrisXeAccelClasses.cpp).
//
// Qualified calls like IOAccelEventMachineFast2::init(...) are compiled by
// -fapple-kext as a load from Apple's exported vtable of that class, so they
// reach Apple's real implementation even though our headers don't declare it.
#include "IrisXeAccel.hpp"
#include "IrisXeDisplayPipe.hpp"   // v0.56.3: activate()
#include "../IrisXeSpaceABI.h"
#include "../IrisXeDrawABI.h"
#include <libkern/libkern.h>
#include <kern/thread.h>              // v0.63.0: completion thread
#include <sys/proc.h>                  // v0.68.33: proc_selfname (hang reports)
#include <IOKit/IODeviceTreeSupport.h> // v0.64.3: NVRAM safety marker

// ---- event machine: GPU progress "stamps" (Apple's paravirt init, ported) ----
IRISXE_SLOT_RET IrisXeEventMachine::init(IOGraphicsAccelerator2 *accel, unsigned int timeoutMs, int numStamps) {
    IrisXeAccelerator *ours = OSDynamicCast(IrisXeAccelerator, accel);
    // Apple returns bool: only the low byte is meaningful
    bool ok = (IOAccelEventMachineFast2::init(accel, timeoutMs, numStamps) & 0xff) != 0;
    if (ours) ours->note("eventMachineInit", ok);
    if (!ok || !ours) return 0;
    volatile UInt32 *stamps = ours->stampBase();
    if (!stamps) { ours->note("stampBase", false); return 0; }
    setStampBaseAddress((unsigned int volatile *)stamps);
    ours->note("stampBase", true);
    return 1;
}

// ---- display machine ----
static IrisXeAccelerator *sDMAccel;               // v0.51.39: for the IrisXe-DisplayPipe report
IRISXE_SLOT_RET IrisXeDisplayMachine::init(IOGraphicsAccelerator2 *accel) {
    IOAccelDisplayMachine::init(accel);          // Apple: stores accel, makes its lock
    IrisXeAccelerator *ours = OSDynamicCast(IrisXeAccelerator, accel);
    sDMAccel = ours;
    if (ours) ours->note("displayMachineInit", true);
    return 1;
}

// Apple's version walks the framebuffers on the PCI device and builds a
// "display pipe" for each (our IrisXeFB). WindowServer's Metal compositor needs one to show its frames
// ("Unable to find display pipe for IOFB service" without it, then everything is painted by the CPU).
// Research round 12 (Apple's code): start() only walks the PCI device's clients and calls found_framebuffer for
// each IOFramebuffer; found_framebuffer -> accelerator createDisplayPipe -> newDisplayPipe + IOAccelDisplayPipe::init
// (framebuffer notification on the IOFramebuffer, one resource, IOAccelTypes/IOAccelIndex on the framebuffer).
// IrisXeFB starts after the accelerator, so at boot the walk finds nothing.
// v0.51.40 handed IrisXeFB over from inside its publish notification: the boot froze (most likely a lock taken
// by IOFramebuffer while it starts). v0.51.42: no boot-time switch any more. The hand-over is done on request
// after login, from a normal thread: sudo sysctl debug.irisxe_dpipe=1. Every step is reported in
// IrisXe-DisplayPipe, so a step that never returns can be read from another Terminal window.
static IrisXeDisplayMachine *sDM;
static unsigned sFound, sFoundOurs;
static char sPipeMsg[700];
void IrisXeFrameTimingPublish(const char *t) {          // v0.59.1
    if (sDMAccel) sDMAccel->setProperty("IrisXe-FrameTiming", t);
}
void IrisXeDisplayPipeStep(const char *step) {
    strlcpy(sPipeMsg, step, sizeof(sPipeMsg));
    if (sDMAccel) sDMAccel->setProperty("IrisXe-DisplayPipe", sPipeMsg);
    IOLog("IrisXeAccel: display pipe: %s\n", sPipeMsg);
}

IRISXE_SLOT_RET IrisXeDisplayMachine::start(IOPCIDevice *pci) {
    sDM = this;
    if (PE_parse_boot_argn("-irisxeaccelfb", nullptr, 0)) {          // Apple's own walk at boot (finds nothing yet)
        bool ok = (IOAccelDisplayMachine::start(pci) & 0xff) != 0;
        IrisXeDisplayPipeStep(ok ? "boot-arg -irisxeaccelfb: Apple's display machine start ok" : "boot-arg -irisxeaccelfb: Apple's start FAILED");
    } else if (!sFound) {
        IrisXeDisplayPipeStep("off (after login: sudo sysctl debug.irisxe_dpipe=1 hands IrisXeFB to Apple's display machine)");
    }
    return 1;
}

// every framebuffer Apple's display machine is told about
IRISXE_SLOT_RET IrisXeDisplayMachine::found_framebuffer(IOFramebuffer *fb) {
    IOService *s = (IOService *)fb;
    bool ours = s && s->metaCast("IrisXeFB");
    sFound++; if (ours) sFoundOurs++;
    IRISXE_SLOT_RET r = IOAccelDisplayMachine::found_framebuffer(fb);
    char m[300];
    snprintf(m, sizeof(m), "3/3 found_framebuffer returned %s (framebuffers given: %u, IrisXeFB %u)",
             (r & 0xff) ? "true: display pipe made" : "false: no pipe", sFound, sFoundOurs);
    IrisXeDisplayPipeStep(m);
    return r;
}

// v0.51.43: WindowServer (CoreDisplay) only uses a display pipe when the accelerator declares
// IOAccelDisplayPipeCapabilities (Apple's Intel and paravirtual accelerators: DisplayPipeSupported +
// TransactionsSupported, research round 15). v0.51.42 made the pipe but WindowServer still could not find it.
static void setPipeCaps(int v) {
    if (!sDMAccel) return;
    if (v == 0) { sDMAccel->removeProperty("IOAccelDisplayPipeCapabilities"); return; }
    OSDictionary *d = OSDictionary::withCapacity(2);
    if (!d) return;
    d->setObject("DisplayPipeSupported", kOSBooleanTrue);
    d->setObject("TransactionsSupported", v == 2 ? kOSBooleanTrue : kOSBooleanFalse);
    sDMAccel->setProperty("IOAccelDisplayPipeCapabilities", d);
    d->release();
}

// sudo sysctl debug.irisxe_dpipe=N (IrisXeAccel.cpp)
//   1: hand IrisXeFB to Apple's display machine (display pipe)
//   2: 1 + IOAccelDisplayPipeCapabilities {DisplayPipeSupported, TransactionsSupported} (like Apple's drivers)
//   3: 1 + IOAccelDisplayPipeCapabilities {DisplayPipeSupported only}
//   4: remove IOAccelDisplayPipeCapabilities again
// WindowServer reads them when it starts: restart it (desktop test) afterwards.
int IrisXeDisplayPipeAttach(int v) {
    if (v == 4) { setPipeCaps(0); IrisXeDisplayPipeStep("capabilities removed"); return 0; }
    if (!sDM) { IrisXeDisplayPipeStep("failed: no display machine"); return 19; }
    if (sFoundOurs) {
        if (IrisXeDisplayPipe::sPipe) IrisXeDisplayPipe::sPipe->activate();   // v0.56.3 (does nothing if active)
        if (v == 2 || v == 3) {
            setPipeCaps(v);
            IrisXeDisplayPipeStep(v == 2 ? "pipe made before; capabilities set: DisplayPipeSupported + TransactionsSupported"
                                         : "pipe made before; capabilities set: DisplayPipeSupported only");
        } else IrisXeDisplayPipeStep("already done (IrisXeFB was handed over before)");
        return 0;
    }
    OSDictionary *m = IOService::serviceMatching("IrisXeFB");
    OSIterator *it = m ? IOService::getMatchingServices(m) : nullptr;
    if (m) m->release();
    IOService *fb = it ? OSDynamicCast(IOService, it->getNextObject()) : nullptr;
    if (fb) fb->retain();
    if (it) it->release();
    if (!fb) { IrisXeDisplayPipeStep("failed: IrisXeFB not found"); return 2; }
    IrisXeDisplayPipeStep("1/3 handing IrisXeFB to Apple's display machine (found_framebuffer)");
    IRISXE_SLOT_RET r = sDM->found_framebuffer((IOFramebuffer *)fb);
    fb->release();
    // v0.56.3: make the new pipe active (framebuffer resource), otherwise every transaction is "not ready"
    if ((r & 0xff) && IrisXeDisplayPipe::sPipe) IrisXeDisplayPipe::sPipe->activate();
    if ((r & 0xff) && (v == 2 || v == 3)) {
        setPipeCaps(v);
        char m[300];
        snprintf(m, sizeof(m), "%s; capabilities set: %s", sPipeMsg,
                 v == 2 ? "DisplayPipeSupported + TransactionsSupported" : "DisplayPipeSupported only");
        IrisXeDisplayPipeStep(m);
    }
    return 0;
}

// v0.64.0 / v0.64.1: the GPU-drawn desktop from boot, done by the kexts themselves (no script, no WindowServer
// restart). WindowServer reads IOAccelDisplayPipeCapabilities when it starts, so what the after-login test did
// (sysctl debug.irisxe_agdc=1, then debug.irisxe_dpipe=2) is done before it starts.
// v0.64.0 did it on every boot and froze before the login screen (no log saved). v0.64.1: only on a one-boot test,
// one step more each time: sudo nvram irisxe-gpustep=N (read and erased by IrisXeIntelFB at the start of the boot,
// published there as IrisXe-GPUStep):
//   1: IrisXeIntelFB only                       2: 1 + display link (linkDisplays) - FROZE the boot (v0.64.2: alone)
//   3: 1 + AGDC at boot: IrisXe-AGDCRequest on IrisXeIntelFB; IrisXeAGDC.kext (loaded by IOKit when IrisXeIntelFB
//        appears) answers in IrisXe-AGDCResult        4: 3 + display pipe hand-over (IrisXeDisplayPipeAttach 2)
// Everything runs on this thread, never inside IOKit's matching (v0.51.40 froze doing that). Each step is reported
// in IrisXe-GPUDesktop (and the kernel log) before it starts. A boot that freezes keeps no log, but the step number
// that was set tells which change froze it.
extern uint64_t IrisXeDisplayPipeFrames();
extern bool IrisXeDisplayPipeReady();
static IrisXeAccelerator *sBootAccel;
static void gpuBootReport(const char *m) {
    if (sBootAccel) sBootAccel->setProperty("IrisXe-GPUDesktop", m);
    IOLog("IrisXeAccel: GPU desktop: %s\n", m);
}
// v0.64.3: remove the safety marker irisxe-gpuboot (set by IrisXeIntelFB at the start of a GPU-desktop boot)
static void gpuBootReached() {
    IORegistryEntry *opt = IORegistryEntry::fromPath("/options", gIODTPlane);
    if (!opt) return;
    if (opt->getProperty("irisxe-gpuboot")) {
        opt->removeProperty("irisxe-gpuboot");
        opt->setProperty("IONVRAM-FORCESYNCNOW-PROPERTY", "IONVRAM-FORCESYNCNOW-PROPERTY");
    }
    opt->release();
}
static IOService *copyClass(const char *cls) {
    OSDictionary *m = IOService::serviceMatching(cls);
    if (!m) return nullptr;
    IOService *s = IOService::copyMatchingService(m);
    m->release();
    return s;
}
static void gpuBootThread(void *, wait_result_t) {
    IOService *fb = nullptr;
    for (int i = 0; i < 240 && !fb; i++) { fb = copyClass("IrisXeFB"); if (!fb) IOSleep(250); }
    if (!fb) { gpuBootReport("off: no IrisXeFB after 60 s"); return; }
    if (!fb->metaCast("IrisXeIntelFB")) {
        bool safety = fb->getProperty("IrisXe-GPUSafety") != nullptr;
        bool noCore = fb->getProperty("IrisXe-NoCore") != nullptr;
        fb->release();
        gpuBootReport(noCore ? "off: IrisXeCore is not running (check EFI/OC/Kexts/IrisXeCore.kext). CPU-drawn desktop" :
                      safety ? "off this boot (safety net): the last GPU-desktop boot did not reach the desktop. "
                               "The next boot tries again"
                             : PE_parse_boot_argn("-irisxecpu", nullptr, 0) ? "off: boot-arg -irisxecpu (CPU-drawn desktop)"
                                                                            : "off: needs the boot-args -irisxefb and -irisxemetal");
        return;
    }
    int step = 0;
    if (OSNumber *n = OSDynamicCast(OSNumber, fb->getProperty("IrisXe-GPUStep"))) step = (int)n->unsigned32BitValue();
    char m[400];
    if (step < 3) fb->setProperty("IrisXe-AGDCRequest", kOSBooleanFalse);
    if (step < 2) {
        gpuBootReached();
        snprintf(m, sizeof(m), "step %d done: IrisXeIntelFB only%s", step, step ? "" : " (nvram irisxe-intelfb / -irisxeintelfb)");
        gpuBootReport(m);
        fb->release();
        return;
    }
    // v0.64.2: step 2 (display link, IOAccelTypes on the framebuffer at boot) froze the boot on 23 Nov, like v0.64.0.
    // It is not needed for the display pipe (the after-login test never used it; Apple's IOAccelDisplayPipe::init sets
    // the framebuffer's IOAccelTypes itself), so steps 3 and 4 no longer include it. Step 2 only with nvram value 2.
    if (step == 2) {
        gpuBootReport("step 2 starting: display link");
        if (sBootAccel) sBootAccel->linkDisplays("GPU test step 2");
        gpuBootReport("step 2 done: IrisXeIntelFB + display link");
        fb->release();
        return;
    }

    gpuBootReport("step 3 starting: asking IrisXeAGDC for the AGDC");
    fb->setProperty("IrisXe-AGDCRequest", kOSBooleanTrue);
    char res[220] = {};
    for (int i = 0; i < 120; i++) {                          // 30 s
        OSString *r = OSDynamicCast(OSString, fb->getProperty("IrisXe-AGDCResult"));
        if (r) { strlcpy(res, r->getCStringNoCopy(), sizeof(res)); break; }
        IOSleep(250);
    }
    fb->release();
    if (!res[0]) { gpuBootReached(); gpuBootReport("step 3 stopped: no answer from IrisXeAGDC in 30 s (IrisXeAGDC.kext installed and allowed?)"); return; }
    if (strncmp(res, "3/3", 3)) { gpuBootReached(); snprintf(m, sizeof(m), "step 3 stopped: AGDC: %s", res); gpuBootReport(m); return; }
    if (step < 4) { gpuBootReached(); gpuBootReport("step 3 done: IrisXeIntelFB + AGDC"); return; }

    gpuBootReport("step 4 starting: handing IrisXeFB to Apple's display machine");
    for (int i = 0; i < 40 && !sDM; i++) IOSleep(250);
    int r = IrisXeDisplayPipeAttach(2);
    if (r || !IrisXeDisplayPipeReady()) {
        IrisXeDisplayPipeAttach(4);                          // no capabilities: WindowServer keeps the CPU desktop
        gpuBootReached();
        snprintf(m, sizeof(m), "step 4 stopped: the display pipe is not ready (error %d; %s)", r, sPipeMsg);
        gpuBootReport(m);
        return;
    }
    gpuBootReport("step 4: display pipe ready before WindowServer, waiting for its first 300 frames");
    // v0.64.3: no time limit (the safety marker is removed only once WindowServer really shows frames)
    for (unsigned i = 0; IrisXeDisplayPipeFrames() < 300; i++) {
        IOSleep(1000);
        if (i == 600) {
            snprintf(m, sizeof(m), "step 4: only %llu frames through the display pipe after 10 minutes (0: WindowServer "
                     "started before the hand-over); still waiting", (unsigned long long)IrisXeDisplayPipeFrames());
            gpuBootReport(m);
        }
    }
    gpuBootReached();
    gpuBootReport("step 4 done: on - WindowServer draws the desktop on the GPU since boot");
}
void IrisXeGPUDesktopBoot(IrisXeAccelerator *accel) {
    sBootAccel = accel;
    if (!PE_parse_boot_argn("-irisxefb", nullptr, 0) || PE_parse_boot_argn("-irisxeoff", nullptr, 0)) {
        gpuBootReport("off: needs the boot-arg -irisxefb");
        return;
    }
    thread_t th;
    if (kernel_thread_start(&gpuBootThread, nullptr, &th) == KERN_SUCCESS) thread_deallocate(th);
    else gpuBootReport("failed: no thread");
}

// ============================================================================
// milestone 2c: GPU page tables for Metal buffers
// ============================================================================
// IOAccelMemoryMap fields (from Apple's paravirt commitIntoGPUPageTable):
//   +0x18 the IOAccelMemory being mapped; that memory's +0xd0 is its
//   IOMemoryDescriptor (the real pages). GPU address and length come from
//   Apple's own getGPUVirtualAddress() / getLength().
static IOMemoryDescriptor *mapDescriptor(IOAccelMemoryMap *map) {
    UInt8 *mem = *(UInt8 **)((UInt8 *)map + 0x18);
    // v0.56.5: only system memory has a descriptor at +0xd0. Video memory (IOSurface "device cache" memory, which
    // now has a real size, so its maps can reach here) has other fields there.
    if (!mem || !OSDynamicCast(IOAccelSysMemory, (OSObject *)mem)) return nullptr;
    return OSDynamicCast(IOMemoryDescriptor, *(OSObject **)(mem + 0xd0));
}

// write the pages of md into the GPU page tables at va (len bytes)
// write the pages of md into a GPU address space at va (len bytes)
static bool mapRange(IrisXeAccelerator *ax, UInt32 space, IOMemoryDescriptor *md, UInt64 va, UInt64 len) {
    UInt32 pages = (UInt32)((len + 4095) / 4096);
    UInt64 pa[64];
    UInt32 done = 0;
    while (done < pages) {
        UInt32 n = 0;
        while (n < 64 && done + n < pages) {
            IOByteCount seg = 0;
            addr64_t p = md->getPhysicalSegment((IOByteCount)(done + n) * 4096, &seg, kIOMemoryMapperNone);
            if (!p) break;
            pa[n++] = p & ~0xFFFULL;
        }
        if (!n || !ax->coreMapPages(space, va + (UInt64)done * 4096, pa, n)) {
            ax->countMetal(1, pages - done);
            static UInt64 maxVA;                             // v0.49.1: which addresses fail (IrisXe-AccelMapFail)
            UInt64 fva = va + (UInt64)done * 4096, top = va + len;
            if (top > maxVA) maxVA = top;
            char t[200];
            snprintf(t, sizeof(t), "last: GPU 0x%llx +%llu bytes (%s), highest end 0x%llx",
                     fva, len, !n ? "no physical page" : "core refused (address over its limit?)", maxVA);
            ax->setProperty("IrisXe-AccelMapFail", t);
            return false;
        }
        ax->countMetal(0, n);
        done += n;
    }
    return true;
}

// Apple's own path (never seen called for Metal buffers): shared space 0
IRISXE_SLOT_RET IrisXeMemoryMap::commitIntoGPUPageTable() {
    IrisXeAccelerator *ax = IrisXeAccelerator::sInstance;
    UInt64 va  = (UInt64)getGPUVirtualAddress();
    UInt64 len = (UInt64)getLength();
    IOMemoryDescriptor *md = mapDescriptor(this);
    if (!ax || !md || !va || !len) { if (ax) ax->countMetal(1, 1); return 1; }
    mapRange(ax, 0, md, va, len);
    return 1;           // Apple keeps going either way; failures are counted
}

// ---- v0.21.4: table of live memory maps; v0.23.0: one GPU space per process --
// Every memory map we create is remembered here with the process (IOAccelTask)
// it belongs to. A command that needs a GPU address finds that process's map
// covering it and writes its pages into the process's own GPU address space
// (made by IrisXeCore on first use, freed with the process's last map). The
// pages stay wired while mapped and are unmapped + unwired when the map is freed.
struct XeMapEntry { IrisXeMemoryMap *map; IOAccelTask *task; IOMemoryDescriptor *md; UInt64 va;
                    UInt32 pages, seq, space; bool wired;
                    bool mapped; };       // v0.60.0: its pages are in the GPU page tables of `space` (no re-check needed)
// v0.60.0: the last maps found for GPU commands, looked at before the full table (a draw uses a few buffers and
// textures again and again; the full search read the address and length of up to 4096 maps, for every resource)
struct XeHit { IOAccelTask *task; IrisXeMemoryMap *map; int idx; UInt64 va, len; };
#define XE_HITS 256                   // v0.60.1: 256 (32 missed 14% of lookups)
static XeHit sHits[XE_HITS];
static UInt32 sHitNext;
static UInt64 sHitFast, sHitSlow;
struct XeTaskRec  { IOAccelTask *task; UInt32 maps; int space; };   // space -1 = none yet
#define XE_MAX_MAPS  4096                    // v0.42.2: 512 was too few for Maps ("skipped: not in the page tables")
#define XE_MAX_TASKS 64
static XeMapEntry sMaps[XE_MAX_MAPS];
static XeTaskRec  sTasks[XE_MAX_TASKS];
static UInt32 sMapSeq;
static IOLock *sMapLock;
// v0.68.37: changes whenever a map's pages leave the GPU page tables (map freed / released): the draw lookup
// cache below (sOk) is only trusted while it is unchanged
static volatile UInt32 sMapGen = 1;
// live maps, table full, found, not found, wire failed, only another process's map,
// processes, spaces made, spaces freed, space failed
static UInt32 sMapStat[10];

static IOLock *mapLock() {
    if (!sMapLock) {
        IOLock *l = IOLockAlloc();
        if (l && !OSCompareAndSwapPtr(nullptr, l, &sMapLock)) IOLockFree(l);
    }
    return sMapLock;
}
static void mapStatPublish() {
    if (!IrisXeAccelerator::sInstance) return;
    char t[400];
    snprintf(t, sizeof(t), "live maps %u (table full %u) | found for commands %u, not found %u, wire failed %u, "
             "other process only %u | processes %u, GPU spaces made %u, freed %u, failed %u | quick finds %llu, full searches %llu",
             sMapStat[0], sMapStat[1], sMapStat[2], sMapStat[3], sMapStat[4], sMapStat[5],
             sMapStat[6], sMapStat[7], sMapStat[8], sMapStat[9], sHitFast, sHitSlow);
    IrisXeAccelerator::sInstance->setProperty("IrisXe-AccelMaps", t);
    // v0.58.7: IrisXe-GPUVA: GPU addresses each process holds (all its memory, mapped for the GPU or not):
    // shows how close a process is to the end of its range (WindowServer ran out at 512 MB in v0.58.6)
    static UInt32 tick;
    IOLock *l = sMapLock;
    if ((tick++ & 63) || !l) return;
    struct { IOAccelTask *task; UInt64 bytes, top; UInt32 n; } u[8] = {};
    IOLockLock(l);
    for (int i = 0; i < XE_MAX_MAPS; i++) {
        XeMapEntry &e = sMaps[i];
        if (!e.map) continue;
        UInt64 mva = (UInt64)e.map->getGPUVirtualAddress(), mlen = (UInt64)e.map->getLength();
        int k = 0;
        while (k < 8 && u[k].task && u[k].task != e.task) k++;
        if (k == 8) continue;
        u[k].task = e.task; u[k].bytes += mlen; u[k].n++;
        if (mva + mlen > u[k].top) u[k].top = mva + mlen;
    }
    IOLockUnlock(l);
    char v[700];
    int n = snprintf(v, sizeof(v), "range %s", IrisXeAccelerator::sInstance->highVA() ? "1..4 GB" : "0..512 MB");
    for (int k = 0; k < 8 && u[k].task && n > 0 && n < (int)sizeof(v) - 80; k++)
        n += snprintf(v + n, sizeof(v) - n, " | process %d: %llu MB in %u, highest 0x%llx", k,
                      u[k].bytes >> 20, u[k].n, u[k].top);
    IrisXeAccelerator::sInstance->setProperty("IrisXe-GPUVA", v);
}
static XeTaskRec *taskRec(IOAccelTask *t, bool create) {        // sMapLock held
    XeTaskRec *freeRec = nullptr;
    for (int i = 0; i < XE_MAX_TASKS; i++) {
        if (sTasks[i].task == t && sTasks[i].maps) return &sTasks[i];
        if (!sTasks[i].maps && !freeRec) freeRec = &sTasks[i];
    }
    if (!create || !freeRec) return nullptr;
    *freeRec = XeTaskRec{ t, 0, -1 };
    sMapStat[6]++;
    return freeRec;
}
// the process's GPU space, made on first use (sMapLock held). -1 = none.
static int taskSpace(IrisXeAccelerator *ax, XeTaskRec *r) {
    if (r->space < 0) {
        int s = ax->coreSpaceCreate();                // 0 = old IrisXeCore: shared space
        if (s < 0) { sMapStat[9]++; return -1; }
        r->space = s;
        if (s > 0) sMapStat[7]++;
    }
    return r->space;
}

IRISXE_SLOT_RET IrisXeMemoryMap::init(IOGraphicsAccelerator2 *a, IOAccelTask *t, IOAccelMemory *m, unsigned int o) {
    IRISXE_SLOT_RET r = IOAccelMemoryMap::init(a, t, m, o);
    IOLock *l = mapLock();
    if ((r & 0xff) && l) {
        IOLockLock(l);
        int i = 0;
        while (i < XE_MAX_MAPS && sMaps[i].map) i++;
        XeTaskRec *rec = i < XE_MAX_MAPS ? taskRec(t, true) : nullptr;
        if (rec) {
            rec->maps++;
            sMaps[i] = XeMapEntry{ this, t, nullptr, 0, 0, ++sMapSeq, 0, false };
            sMapStat[0]++;
        } else sMapStat[1]++;
        IOLockUnlock(l);
    }
    return r;
}

void IrisXeMemoryMap::free() {
    IOLock *l = sMapLock;
    IrisXeAccelerator *ax = IrisXeAccelerator::sInstance;
    if (l) {
        IOLockLock(l);
        for (int i = 0; i < XE_MAX_MAPS; i++) {
            if (sMaps[i].map != this) continue;
            for (int h = 0; h < XE_HITS; h++) if (sHits[h].map == this) sHits[h] = XeHit{};   // v0.60.0
            sMapGen++;                                                                     // v0.68.37
            XeMapEntry &e = sMaps[i];
            if (e.wired && e.md) {
                if (ax && e.pages) {
                    ax->coreUnmapPages(e.space, e.va, e.pages);
                    ax->countMetal(4, e.pages);
                }
                e.md->complete();
                e.md->release();
            }
            XeTaskRec *rec = taskRec(e.task, false);
            if (rec && rec->maps && !--rec->maps) {        // the process's last map: drop its space
                if (rec->space > 0 && ax) { ax->coreSpaceDestroy((UInt32)rec->space); sMapStat[8]++; }
                *rec = XeTaskRec{ nullptr, 0, -1 };
                if (sMapStat[6]) sMapStat[6]--;
            }
            e = XeMapEntry{ nullptr, nullptr, nullptr, 0, 0, 0, 0, false };
            if (sMapStat[0]) sMapStat[0]--;
            break;
        }
        IOLockUnlock(l);
    }
    IOAccelMemoryMap::free();
}

// Make sure [va, va+len) of process `task` is in that process's GPU space.
// Returns the space (>= 0) or -1 when no map of the process covers the range
// or its pages can't be wired/mapped.
// v0.60.0: *sure = the range is known to be in the page tables already (the caller needn't ask IrisXeCore)
static int mapForGPU(IrisXeAccelerator *ax, IOAccelTask *task, UInt64 va, UInt64 len, bool *sure) {
    IOLock *l = mapLock();
    if (sure) *sure = false;
    if (!ax || !l || !len) return -1;
    int result = -1;
    IOLockLock(l);
    XeTaskRec *rec = taskRec(task, false);
    int space = rec ? taskSpace(ax, rec) : -1;
    // v0.60.0: fast path - one of the last maps found, still the same map, already in this space's page tables
    if (space >= 0) {
        for (int j = 0; j < XE_HITS; j++) {         // v0.68.37: newest first (the same few buffers again and again)
            XeHit &x = sHits[(sHitNext - 1 - (UInt32)j) % XE_HITS];
            if (x.task != task || !x.map || va < x.va || va + len > x.va + x.len) continue;
            XeMapEntry &e = sMaps[x.idx];
            if (e.map != x.map || e.task != task || !e.wired || !e.mapped || e.space != (UInt32)space) { x = XeHit{}; continue; }
            sHitFast++;
            IOLockUnlock(l);
            if (sure) *sure = true;
            return space;
        }
    }
    sHitSlow++;
    // newest map of this process that covers the range
    int best = -1;
    bool other = false, newMap = false;
    UInt64 bva = 0, blen = 0;
    for (int i = 0; i < XE_MAX_MAPS; i++) {
        XeMapEntry &e = sMaps[i];
        if (!e.map || (best >= 0 && e.seq < sMaps[best].seq)) continue;
        UInt64 mva = (UInt64)e.map->getGPUVirtualAddress();
        UInt64 mlen = (UInt64)e.map->getLength();
        if (!mva || !mlen || va < mva || va + len > mva + mlen) continue;
        if (e.task != task) { other = true; continue; }
        best = i; bva = mva; blen = mlen;
    }
    if (best >= 0 && space >= 0) {
        XeMapEntry &e = sMaps[best];
        if (e.wired && e.mapped && e.space == (UInt32)space) { result = space; if (sure) *sure = true; }
        else if (e.wired && ax->coreIsMapped((UInt32)space, va, len)) result = space;   // already there
        else {
            IOMemoryDescriptor *md = e.wired ? e.md : mapDescriptor(e.map);
            if (md && !e.wired) {
                if (md->prepare() == kIOReturnSuccess) { md->retain(); e.md = md; e.wired = true; }
                else sMapStat[4]++;
            }
            if (e.wired) {
                e.va = bva; e.pages = (UInt32)((blen + 4095) / 4096); e.space = (UInt32)space;
                if (mapRange(ax, (UInt32)space, e.md, bva, blen)) {
                    result = space; e.mapped = true; newMap = true;
                    if (sure) *sure = true;
                }
            }
        }
        if (result >= 0 && e.mapped) {             // remember it for the next commands
            sHits[sHitNext % XE_HITS] = XeHit{ task, e.map, best, bva, blen };
            sHitNext++;
        }
    }
    if (best >= 0) sMapStat[2]++;
    else { sMapStat[3]++; if (other) sMapStat[5]++; }
    IOLockUnlock(l);
    static UInt32 pubN;                            // v0.60.0: not after every lookup
    if (newMap || (++pubN & 255) == 0) mapStatPublish();
    return result;
}
// v0.68.17: the GPU space of process `task` (made if it has none yet), -1 = unknown process / failed
int irisxeTaskSpace(IrisXeAccelerator *ax, IOAccelTask *task) {
    IOLock *l = mapLock();
    if (!ax || !l) return -1;
    IOLockLock(l);
    XeTaskRec *rec = taskRec(task, false);
    int s = rec ? taskSpace(ax, rec) : -1;
    IOLockUnlock(l);
    return s;
}
// v0.68.15: the GPU address of an IOAccelMemory in process `task` (its newest map), 0 = not mapped there
UInt64 irisxeFindMemoryVA(IOAccelTask *task, const void *mem, UInt64 *lenOut) {
    IOLock *l = mapLock();
    if (lenOut) *lenOut = 0;
    if (!l || !mem) return 0;
    UInt64 va = 0, len = 0; UInt32 seq = 0;
    IOLockLock(l);
    for (int i = 0; i < XE_MAX_MAPS; i++) {
        XeMapEntry &e = sMaps[i];
        if (!e.map || e.task != task || *(void **)((UInt8 *)e.map + 0x18) != mem || e.seq < seq) continue;
        va = (UInt64)e.map->getGPUVirtualAddress(); len = (UInt64)e.map->getLength(); seq = e.seq;
    }
    IOLockUnlock(l);
    if (lenOut) *lenOut = len;
    return va;
}
int irisxeMapForGPU(IrisXeAccelerator *ax, IOAccelTask *task, UInt64 va, UInt64 len) {
    return mapForGPU(ax, task, va, len, nullptr);
}

IRISXE_SLOT_RET IrisXeMemoryMap::releaseFromGPUPageTable() {
    IrisXeAccelerator *ax = IrisXeAccelerator::sInstance;
    UInt64 va  = (UInt64)getGPUVirtualAddress();
    UInt64 len = (UInt64)getLength();
    if (ax && va && len) {
        UInt32 pages = (UInt32)((len + 4095) / 4096);
        sMapGen++;                                     // v0.68.37
        ax->coreUnmapPages(0, va, pages);
        ax->countMetal(4, pages);
    }
    return 1;
}

// ============================================================================
// milestone 2c: our own command in Metal command buffers
// ============================================================================
// A command buffer arrives as segments of "kernel commands" {type, size, ...}.
// Apple's queue hands every one to processSegmentKernelCommand; ours is:
//   type 0x10001 "fill": u64 GPU address, u64 length, u32 32-bit pattern
// It is run on the blitter right away; the command buffer completes after.
#define XE_KCMD_FILL 0x10001u
struct XeFillCmd { UInt32 type, size; UInt64 gpuVA, length; UInt32 pattern, pad; };
//   type 0x10002 "copy" (milestone 2d): u64 source, u64 destination, u64 length
#define XE_KCMD_COPY 0x10002u
struct XeCopyCmd { UInt32 type, size; UInt64 srcVA, dstVA, length; };
//   type 0x10003 "dispatch" (milestone 3a): a SIMD8 compute kernel, followed by
//   its code (kernelDW dwords). The kernel gets g1 = { buffer GPU addresses
//   0..5 (32-bit), gridX, threads per group } (see IrisXeSpaceABI.h).
#define XE_KCMD_DISPATCH 0x10003u
struct XeDispatchCmd { UInt32 type, size; UInt32 groups[3], tpg, gridX, nbuf, kernelDW, info;   // info: v0.28.0 (SLM bytes | barrier)
                       UInt64 bufVA[6], bufLen[6];
                       UInt64 codeVA;      // v0.29.0: info & XE_INFO_CODE_IN_BUFFER: kernel at codeVA, not after the command
                       UInt32 ntex, nsamp; // v0.30.0 (milestone 4): textures and samplers (IrisXeSpaceABI.h)
                       XeTexDesc tex[kXeMaxTextures];
                       XeSampDesc samp[kXeMaxSamplers];
                       UInt32 tpgH, tpgD, gridY, gridZ; };   // v0.51.17: tpg = W x tpgH x tpgD threads; grid rows, slices
#define XE_INFO_CODE_IN_BUFFER 0x40000000u
//   type 0x10004 "draw" (milestone 5c): a Metal draw / render pass clear (IrisXeDrawABI.h)
#define XE_KCMD_DRAW 0x10004u
struct XeDrawCmd { UInt32 type, size; XeDrawDesc d; };

#define XY_COLOR_BLT_CMD    ((2u << 29) | (0x50u << 22))
#define BLT_WRITE_RGBA      ((1u << 20) | (2u << 20))
#define BLT_DEPTH_32        (3u << 24)
#define BLT_ROP_COLOR_COPY  (0xF0u << 16)

static UInt32 colorBlt(UInt32 *c, UInt64 dst, UInt32 pitch, UInt32 w, UInt32 h, UInt32 color) {
    c[0] = XY_COLOR_BLT_CMD | BLT_WRITE_RGBA | (7 - 2);
    c[1] = BLT_DEPTH_32 | BLT_ROP_COLOR_COPY | pitch;
    c[2] = 0;
    c[3] = (h << 16) | w;
    c[4] = (UInt32)dst; c[5] = (UInt32)(dst >> 32);
    c[6] = color;
    c[7] = 0;                                         // MI_NOOP (even length)
    return 8;
}

#define XY_SRC_COPY_BLT_CMD ((2u << 29) | (0x53u << 22))
#define BLT_ROP_SRC_COPY    (0xCCu << 16)
// v0.36.0: a rectangle copy (texture <-> buffer / texture): rows of widthBytes, each pitch apart
#define XE_KCMD_COPY2D 0x10005u
struct XeCopy2DCmd { UInt32 type, size; UInt64 srcVA, dstVA; UInt32 srcPitch, dstPitch, widthBytes, height; };
// XY_SRC_COPY_BLT of w x h elements of 1, 2 or 4 bytes (8 / 16 / 32 bpp), pitches < 32 KB
static UInt32 copyBlt2D(UInt32 *c, UInt64 dst, UInt32 dpitch, UInt64 src, UInt32 spitch, UInt32 w, UInt32 h, UInt32 unit) {
    UInt32 depth = unit == 4 ? BLT_DEPTH_32 : unit == 2 ? (1u << 24) : 0;   // 32 / 16 (565) / 8 bpp
    c[0] = XY_SRC_COPY_BLT_CMD | (unit == 4 ? BLT_WRITE_RGBA : 0) | (10 - 2);
    c[1] = depth | BLT_ROP_SRC_COPY | dpitch;
    c[2] = 0;
    c[3] = (h << 16) | w;
    c[4] = (UInt32)dst; c[5] = (UInt32)(dst >> 32);
    c[6] = 0;
    c[7] = spitch;
    c[8] = (UInt32)src; c[9] = (UInt32)(src >> 32);
    return 10;
}
static UInt32 copyBlt(UInt32 *c, UInt64 dst, UInt64 src, UInt32 pitch, UInt32 w, UInt32 h) {
    c[0] = XY_SRC_COPY_BLT_CMD | BLT_WRITE_RGBA | (10 - 2);
    c[1] = BLT_DEPTH_32 | BLT_ROP_SRC_COPY | pitch;
    c[2] = 0;                                         // dst x1, y1
    c[3] = (h << 16) | w;                             // dst x2, y2
    c[4] = (UInt32)dst; c[5] = (UInt32)(dst >> 32);
    c[6] = 0;                                         // src x1, y1
    c[7] = pitch;
    c[8] = (UInt32)src; c[9] = (UInt32)(src >> 32);
    return 10;
}

// ---- tracing: how far does Apple's queue get with a command buffer? -------
// Each step is counted and the first calls are logged (with the queue's error
// field at +0x610, which Apple sets to 0xa when it rejects something).
static UInt32 sTrace[6];
static const char *kTraceName[6] = { "processCommandBuffer", "parseSegmentList", "processSegment",
                                     "processKernelCommand", "segmentKernelCommand", "blit" };
static void trace(IOAccelCommandQueue *q, int what, UInt32 a, UInt32 b) {
    sTrace[what]++;
    UInt32 err = *(volatile UInt32 *)((UInt8 *)q + 0x610);
    if (sTrace[what] <= 12)
        IOLog("IrisXeAccel: queue %s #%u (0x%x, 0x%x) error field 0x%x\n", kTraceName[what], sTrace[what], a, b, err);
    if (IrisXeAccelerator::sInstance) {
        char t[220];
        snprintf(t, sizeof(t), "commandBuffers %u, segmentLists %u, segments %u, kernelCmds %u, segmentCmds %u, blits %u | last error field 0x%x",
                 sTrace[0], sTrace[1], sTrace[2], sTrace[3], sTrace[4], sTrace[5], err);
        IrisXeAccelerator::sInstance->setProperty("IrisXe-AccelQueue", t);
    }
}

// v0.60.0: kernel time meter (IrisXe-KernelTiming): per command buffer, how long the queue took and how much of it
// was looking up memory for draws and running the draws on the GPU (build + submit + wait)
static UInt64 sKtCb, sKtNs, sKtMaxNs, sKtLookNs, sKtDrawNs, sKtJobs, sKtDraws, sKtSince;
static UInt64 ktNs(uint64_t abs) { UInt64 ns = 0; absolutetime_to_nanoseconds(abs, &ns); return ns; }
void IrisXeKernelTimingReset() {
    sKtCb = sKtNs = sKtMaxNs = sKtLookNs = sKtDrawNs = sKtJobs = sKtDraws = 0;
    sKtSince = mach_absolute_time();
}
static void ktPublish() {
    if (!IrisXeAccelerator::sInstance || !sKtCb) return;
    char t[400];
    UInt64 n = sKtCb, other = sKtNs > sKtLookNs + sKtDrawNs ? sKtNs - sKtLookNs - sKtDrawNs : 0;
    snprintf(t, sizeof(t), "%llu command buffers in %llu s | each: %llu us (max %llu us) = memory lookups %llu us + "
             "draws on the GPU %llu us + other %llu us | %llu.%llu GPU jobs, %llu.%llu draws each",
             n, sKtSince ? ktNs(mach_absolute_time() - sKtSince) / 1000000000ULL : 0,
             sKtNs / n / 1000, sKtMaxNs / 1000, sKtLookNs / n / 1000, sKtDrawNs / n / 1000, other / n / 1000,
             sKtJobs / n, sKtJobs * 10 / n % 10, sKtDraws / n, sKtDraws * 10 / n % 10);
    IrisXeAccelerator::sInstance->setProperty("IrisXe-KernelTiming", t);
}


// ---- v0.63.0: asynchronous command buffers ------------------------------------------------------------------
// Apple's IOAccelCommandQueue::submit_command_buffer (macOS 15, research round 32) calls processCommandBuffer, then
// copies the queue's event (queue + 0x5c8) into the command buffer's fence; IOAccelFenceMachine retires the command
// buffer once every stamp in that event has passed. Our event machine never issued a stamp, so the fence passed at
// once and IrisXeAccel had to wait for the GPU inside processCommandBuffer. Now (debug.irisxe_async=1) the last draw
// job of a command buffer is left running and the command buffer gets a stamp, as Apple's paravirtual GPU does:
//   incrementStamp(kAsyncStamp) -> value V; setEventStamp(V) into a local event, merged into each segment's resource
//   list (+0x60: Apple keeps the resources until it passes) and the task's event (+0x270), and at the end into the
//   queue's event. A thread waits for the GPU job, writes V into the stamp's memory (our stamp page, shared with
//   Metal in user space) and calls signalStamp (wakes waiters and the display pipe's transactions).
// The offsets are checked at run time; if anything looks different, it stays synchronous (IrisXe-AsyncState says why).
extern int IrisXeAsyncMode();                     // IrisXeAccel.cpp (debug.irisxe_async)
static const int kAsyncStamp = 31;                // a stamp index nothing else uses (our event machine has 32)
static const UInt32 kQAccel = 0x5c0, kQEvent = 0x5c8, kListEvent = 0x60, kTaskEvent = 0x270, kAccelEM = 0x380, kEMPtrs = 0x28, kEMNum = 0x30;
static int sAsyncOK = -1;                         // -1 not checked, 0 refused, 1 usable
static char sAsyncWhy[160] = "not used yet";
static bool sCbAsync;                             // this command buffer left a job running
static UInt32 sCbSeq, sCbV;
static UInt64 sCbEv[8] __attribute__((aligned(16)));
static volatile UInt32 *sStampPtr;
static UInt64 sAsyncCbs, sAsyncSignals, sAsyncTimeouts, sAsyncInline;
struct AsyncDone { UInt32 v, seq; };
static AsyncDone sRing[256];
static UInt32 sRingHead, sRingTail;               // head = next to signal, tail = next free
static IOLock *sRingLock;
static bool sThreadOn;

static void asyncState(const char *why) {
    strlcpy(sAsyncWhy, why, sizeof(sAsyncWhy));
    if (!IrisXeAccelerator::sInstance) return;
    char t[300];
    snprintf(t, sizeof(t), "%s | %llu command buffers completed later, %llu signalled, %llu GPU timeouts, %llu waited in the queue (ring full)",
             sAsyncWhy, sAsyncCbs, sAsyncSignals, sAsyncTimeouts, sAsyncInline);
    IrisXeAccelerator::sInstance->setProperty("IrisXe-AsyncState", t);
}
// an IOAccelEvent: 8 slots of (stamp index, value); index -1 = empty
static bool looksLikeEvent(const void *p, int nStamps) {
    if (!p) return false;
    const UInt64 *e = (const UInt64 *)p;
    for (int i = 0; i < 8; i++) {
        SInt32 idx = (SInt32)(UInt32)e[i];
        if (idx != -1 && (idx < 0 || idx >= nStamps)) return false;
    }
    return true;
}
static IOAccelEventMachine2 *asyncEM(IrisXeAccelerator *ax) { return (IOAccelEventMachine2 *)ax->eventMachine(); }
// the layout checks (once): queue -> accelerator, accelerator -> event machine, the stamp's memory in our page
static bool asyncCheck(IOAccelCommandQueue *q, IrisXeAccelerator *ax) {
    if (sAsyncOK >= 0) return sAsyncOK == 1;
    sAsyncOK = 0;
    IOAccelEventMachine2 *em = asyncEM(ax);
    if (!em) { asyncState("off: no event machine"); return false; }
    if (*(void **)((UInt8 *)q + kQAccel) != (void *)ax) { asyncState("off: queue layout differs (+0x5c0 is not the accelerator)"); return false; }
    if (*(void **)((UInt8 *)ax + kAccelEM) != (void *)em) { asyncState("off: accelerator layout differs (+0x380 is not the event machine)"); return false; }
    int n = (int)(UInt32)em->getNumStamps();
    if (n <= kAsyncStamp || *(SInt32 *)((UInt8 *)em + kEMNum) != n) { asyncState("off: event machine has too few stamps / layout differs"); return false; }
    volatile UInt32 **ptrs = *(volatile UInt32 ***)((UInt8 *)em + kEMPtrs);
    volatile UInt32 *sp = ptrs ? ptrs[kAsyncStamp] : nullptr;
    if (!sp || sp < ax->stampBase() || sp >= ax->stampEnd()) { asyncState("off: the stamp's memory is not in our stamp page"); return false; }
    if (!looksLikeEvent((UInt8 *)q + kQEvent, n)) { asyncState("off: queue event (+0x5c8) does not look like an event"); return false; }
    sStampPtr = sp;
    *sStampPtr = (UInt32)em->getStamp(kAsyncStamp);    // nothing pending yet: memory = last issued
    sRingLock = IOLockAlloc();
    if (!sRingLock) { asyncState("off: no lock"); return false; }
    sAsyncOK = 1;
    asyncState("on");
    return true;
}
static void asyncWrite(IOAccelEventMachine2 *em, UInt32 v) {
    UInt32 cur = *sStampPtr;
    if ((SInt32)(v - cur) > 0) *sStampPtr = v;     // never backwards
    __asm__ volatile("mfence" ::: "memory");
    em->signalStamp(kAsyncStamp, 0);
    sAsyncSignals++;
}
static void asyncThread(void *, wait_result_t) {
    IrisXeAccelerator *ax = IrisXeAccelerator::sInstance;
    for (;;) {
        IOLockLock(sRingLock);
        while (sRingHead == sRingTail) IOLockSleep(sRingLock, &sRingHead, 0 /* THREAD_UNINT */);
        AsyncDone e = sRing[sRingHead & 255];
        IOLockUnlock(sRingLock);
        int r = ax ? ax->coreWaitSeq(e.seq, 3000) : 0;
        if (r == -3) sAsyncTimeouts++;
        IOAccelEventMachine2 *em = ax ? asyncEM(ax) : nullptr;
        if (em) asyncWrite(em, e.v);               // also after a timeout: never leave Metal waiting for ever
        IOLockLock(sRingLock);
        sRingHead++;
        IOLockUnlock(sRingLock);
        if ((sAsyncSignals & 255) == 1) asyncState(sAsyncWhy);
    }
}
// a draw job was left running in this command buffer: give it a stamp (once per command buffer) and protect the
// segment's resources and the task's memory with it
static void asyncNote(IOAccelCommandQueue *q, IrisXeAccelerator *ax, IOAccelSegmentResourceList *list, UInt32 seq) {
    IOAccelEventMachine2 *em = asyncEM(ax);
    int n = (int)(UInt32)em->getNumStamps();
    if (!sCbAsync) {
        em->incrementStamp(kAsyncStamp);
        sCbV = (UInt32)em->getStamp(kAsyncStamp);
        em->initEvent((IOAccelEvent *)sCbEv);
        em->setEventStamp(kAsyncStamp, (IOAccelEvent *)sCbEv);
        void *task = (void *)q->getGPUTask();
        if (task && looksLikeEvent((UInt8 *)task + kTaskEvent, n)) em->mergeEvent((IOAccelEvent *)((UInt8 *)task + kTaskEvent), (IOAccelEvent *)sCbEv);
        sCbAsync = true;
    }
    if (list && looksLikeEvent((UInt8 *)list + kListEvent, n))
        em->mergeEvent((IOAccelEvent *)((UInt8 *)list + kListEvent), (IOAccelEvent *)sCbEv);
    sCbSeq = seq;
}
// the end of a command buffer: its stamp into the queue's event (Apple copies it into the fence), then the thread
static void asyncFinishCb(IOAccelCommandQueue *q, IrisXeAccelerator *ax) {
    if (!sCbAsync) return;
    sCbAsync = false;
    IOAccelEventMachine2 *em = asyncEM(ax);
    em->setEventStamp(kAsyncStamp, (IOAccelEvent *)((UInt8 *)q + kQEvent));
    sAsyncCbs++;
    if (!sThreadOn) {
        thread_t th = nullptr;
        if (kernel_thread_start(&asyncThread, nullptr, &th) == KERN_SUCCESS) { thread_deallocate(th); sThreadOn = true; }
    }
    IOLockLock(sRingLock);
    bool full = !sThreadOn || sRingTail - sRingHead >= 255;
    if (!full) { sRing[sRingTail & 255] = AsyncDone{ sCbV, sCbSeq }; sRingTail++; IOLockWakeup(sRingLock, &sRingHead, true); }
    IOLockUnlock(sRingLock);
    if (full) {                                   // no thread / too many: wait here (as before v0.63.0)
        sAsyncInline++;
        if (ax->coreWaitSeq(sCbSeq, 3000) == -3) sAsyncTimeouts++;
        asyncWrite(em, sCbV);
    }
}

IRISXE_SLOT_RET IrisXeCommandQueue::processCommandBuffer(unsigned int a, unsigned int b) {
    trace(this, 0, a, b);
    uint64_t t0 = mach_absolute_time();
    sCbAsync = false; sCbSeq = 0;                        // v0.63.0
    IRISXE_SLOT_RET r = IOAccelCommandQueue::processCommandBuffer(a, b);
    if (sCbAsync && IrisXeAccelerator::sInstance) asyncFinishCb(this, IrisXeAccelerator::sInstance);
    UInt64 ns = ktNs(mach_absolute_time() - t0);
    if (!sKtSince) sKtSince = t0;
    sKtCb++; sKtNs += ns; if (ns > sKtMaxNs) sKtMaxNs = ns;
    if ((sKtCb & 31) == 0) ktPublish();
    trace(this, 0, (UInt32)r, 0xFFFF);
    return r;
}
// Apple's base queue doesn't build the segment's resource list (it returned
// NULL in v0.21.1, and the command buffer failed with "Invalid Resource").
// Apple's paravirt GPU creates its own subclass here; we create ours and let
// Apple's generic IOAccelSegmentResourceList code read the list.
IRISXE_SLOT_RET IrisXeCommandQueue::parseSegmentList(IOAccelShared2 *sh, IOAccelSegmentResourceListHeader const *h,
                                                       IOAccelDeviceShmem *shm) {
    IRISXE_SLOT_RET r = IOAccelCommandQueue::parseSegmentList(sh, h, shm);
    UInt32 how = r ? 1 : 0;                              // 1 = Apple made one
    if (!r) {
        IrisXeSegmentResourceList *l = new IrisXeSegmentResourceList;
        bool ok = l && (l->initWithSharedResourceList(sh, h, shm) & 0xff);
        if (ok) { r = (IRISXE_SLOT_RET)l; how = 2; }     // 2 = ours
        else { if (l) l->release(); how = 3; }           // 3 = ours, init failed
    }
    trace(this, 1, h ? ((const UInt32 *)h)[2] : 0xdead, how);
    return r;
}
// v0.49.1: the whole segment's commands, for batching: runDraw looks ahead to the segment's end
static const UInt8 *sSegStart, *sSegEnd;
static UInt64 sSegs, sSegDraws, sSegMaxDraws, sSegBytes;
static void batchStatPublish(UInt32 batchedNow);
IRISXE_SLOT_RET IrisXeCommandQueue::processSegment(IOAccelSegmentResourceList *list, IOAccelKernelCommand const *cmd,
                                                     IOAccelKernelCommand const *end) {
    trace(this, 2, (UInt32)((const UInt8 *)end - (const UInt8 *)cmd), cmd ? ((const UInt32 *)cmd)[0] : 0xdead);
    sSegStart = (const UInt8 *)cmd; sSegEnd = (const UInt8 *)end;
    {   // count the draws in this segment (statistics)
        UInt32 nd = 0;
        const UInt8 *p = sSegStart;
        while (p && p + 8 <= sSegEnd) {
            const UInt32 *w = (const UInt32 *)p;
            if (w[1] < 8 || (w[1] & 3)) break;
            if (w[0] == 0x10004u) nd++;
            p += w[1];
        }
        sSegs++; sSegDraws += nd; sSegBytes = (UInt64)(sSegEnd - sSegStart);
        if (nd > sSegMaxDraws) sSegMaxDraws = nd;
    }
    IRISXE_SLOT_RET r = IOAccelCommandQueue::processSegment(list, cmd, end);
    sSegStart = sSegEnd = nullptr;
    return r;
}
IRISXE_SLOT_RET IrisXeCommandQueue::processKernelCommand(IOAccelKernelCommand const *cmd, IOAccelKernelCommand const *end) {
    trace(this, 3, cmd ? ((const UInt32 *)cmd)[0] : 0xdead, cmd ? ((const UInt32 *)cmd)[1] : 0);
    return IOAccelCommandQueue::processKernelCommand(cmd, end);
}

// make sure a range of the queue's process is in its GPU space; returns the
// space or -1. Never send the GPU to an address without page-table entries:
// it would fault and the engine would hang (v0.21.2: first fill timed out, -3)
static int ensureMapped(IrisXeAccelerator *ax, IOAccelTask *task, UInt64 va, UInt64 len) {
    bool sure = false;
    int space = mapForGPU(ax, task, va, len, &sure);
    if (space >= 0 && (sure || ax->coreIsMapped((UInt32)space, va, len))) return space;
    ax->countMetal(6, 1);
    IOLog("IrisXeAccel: GPU 0x%llx +%llu skipped: not in the process's GPU page tables\n", va, len);
    return -1;
}

// v0.36.0: bytes a texture spans (a mip chain or an array: qpitch rows per layer)
template <class T> static UInt64 texSpan(const T &t) {
    UInt32 levels = (t.pad & 0xFF) ? (t.pad & 0xFF) : 1;
    bool tiled = (t.pad >> 16) & 1;                          // v0.37.0: Y-tiled (depth): qpitch = rows allocated
    return (UInt64)t.pitch * ((t.array || levels > 1 || tiled) ? t.qpitch : t.height) * (t.layers ? t.layers : 1);
}

static IRISXE_SLOT_RET runDispatch(IOAccelCommandQueue *q, IrisXeAccelerator *ax, IOAccelTask *task,
                                   const XeDispatchCmd *d, const UInt8 *end) {
    bool inBuf = (d->info & XE_INFO_CODE_IN_BUFFER) != 0;
    UInt32 inl = inBuf ? 0 : d->kernelDW * 4;            // code bytes after the command
    if (d->size < sizeof(XeDispatchCmd) || d->nbuf > 6 || !d->kernelDW ||
        d->kernelDW > (inBuf ? 1048576u : 992u) ||       // kXeKernelMaxDW (IrisXeSpaceABI.h)
        d->size < sizeof(XeDispatchCmd) + inl ||
        (const UInt8 *)d + sizeof(XeDispatchCmd) + inl > end ||
        (inBuf && (!d->codeVA || (d->codeVA & 63) || d->codeVA + d->kernelDW * 4ULL > 0xFFFFFFFFULL))) {
        ax->countMetal(8, 1);
        return 0;
    }
    UInt32 ctd[8] = {};
    int space = -1;
    for (UInt32 i = 0; i < d->nbuf; i++) {
        if (!d->bufVA[i]) continue;                        // slot not bound (v0.29.1: scratch in slot 5)
        if (d->bufVA[i] + d->bufLen[i] > 0xFFFFFFFFULL) { ax->countMetal(8, 1); return 0; }
        int s = ensureMapped(ax, task, d->bufVA[i], d->bufLen[i] ? d->bufLen[i] : 4);
        if (s < 0 || (space >= 0 && s != space)) return 0;
        space = s;
        ctd[i] = (UInt32)d->bufVA[i];
    }
    if (d->ntex > kXeMaxTextures || d->nsamp > kXeMaxSamplers) { ax->countMetal(8, 1); return 0; }
    for (UInt32 i = 0; i < d->ntex; i++) {                 // textures: their memory in the same space
        const XeTexDesc &t = d->tex[i];
        if (!t.va) continue;
        UInt64 len = texSpan(t);
        if (!len || t.va + len > 0xFFFFFFFFULL) { ax->countMetal(8, 1); return 0; }
        int s = ensureMapped(ax, task, t.va, len);
        if (s < 0 || (space >= 0 && s != space)) return 0;
        space = s;
    }
    if (inBuf) {                                           // the kernel's own buffer (+ prefetch padding)
        int s = ensureMapped(ax, task, d->codeVA, d->kernelDW * 4 + 256);
        if (s < 0 || (space >= 0 && s != space)) return 0;
        space = s;
    }
    if (space < 0) {                                       // no buffers: still the process's space
        space = irisxeMapForGPU(ax, task, 0, 0);
        if (space < 0) space = 0;
    }
    ctd[6] = d->gridX; ctd[7] = d->tpg;
    trace(q, 5, d->gridX, d->tpg);
    UInt32 hd[2] = { d->tpgH, d->tpgD }, yz[2] = { d->gridY, d->gridZ };
    int r = ax->coreCompute((UInt32)space, inBuf ? nullptr : (const UInt32 *)(d + 1), d->kernelDW, ctd, d->groups,
                            d->tpg, 2000, d->info & ~XE_INFO_CODE_IN_BUFFER, inBuf ? d->codeVA : 0,
                            d->ntex, d->tex, d->nsamp, d->samp, hd, yz);
    ax->countMetal(r == 0 ? 7 : 8, 1);
    if (r) IOLog("IrisXeAccel: compute dispatch %u x %u threads failed (%d)\n", d->groups[0], d->tpg, r);
    return 0;
}

// milestone 5c: every piece of memory the draw uses must be in the process's GPU space
// checks one draw command and maps its memory; *space: the space of the draws before it (-1: none yet)
static bool prepDraw(IrisXeAccelerator *ax, IOAccelTask *task, const XeDrawCmd *k, const UInt8 *end, XeDrawDesc *dp, int *spaceIO) {
    if (k->size < sizeof(XeDrawCmd) || (const UInt8 *)k + sizeof(XeDrawCmd) > end) return false;
    XeDrawDesc &d = *dp;
    memcpy(&d, &k->d, sizeof(d));
    if (d.version != XE_DRAW_VERSION || d.size != sizeof(XeDrawDesc) || d.nrt > XE_DRAW_MAX_RT ||
        d.nvb > XE_DRAW_MAX_VB || d.ntex > XE_DRAW_MAX_FTEX || d.nvtex > XE_DRAW_MAX_TEX) return false;
    int space = *spaceIO;
    // v0.68.37: ranges already found mapped for this process (until a map's pages leave the page tables: sMapGen).
    // A draw names up to ~40 buffers / textures, mostly the same as the draw before: without the lock, the hit list
    // and the core's is-mapped call (memory lookups were 0.3 - 0.6 ms of every command buffer). Boot-arg
    // -irisxenolookcache: off.
    struct XeOk { IOAccelTask *task; UInt64 va, end; int space; UInt32 gen; };
    static XeOk sOk[64];
    static UInt32 sOkNext;
    static int noCache = -1;
    static IOSimpleLock *okLock;                       // several queues (processes) may draw at the same time
    if (noCache < 0) {
        IOSimpleLock *l = IOSimpleLockAlloc();
        if (l && !OSCompareAndSwapPtr(nullptr, l, (void * volatile *)&okLock)) IOSimpleLockFree(l);
        noCache = (PE_parse_boot_argn("-irisxenolookcache", nullptr, 0) || !okLock) ? 1 : 0;
    }
    auto use = [&](UInt64 va, UInt64 len) {
        if (!va) return true;
        if (!len || va + len > 0xFFFFFFFFULL) return false;
        UInt32 gen = sMapGen;
        if (!noCache) {
            int hit = -1;
            IOSimpleLockLock(okLock);
            for (UInt32 j = 0; j < 64; j++) {
                const XeOk &o = sOk[(sOkNext - 1 - j) & 63];
                if (o.task == task && o.gen == gen && va >= o.va && va + len <= o.end) { hit = o.space; break; }
            }
            IOSimpleLockUnlock(okLock);
            if (hit >= 0) {
                if (space >= 0 && hit != space) return false;
                space = hit;
                return true;
            }
        }
        int s = ensureMapped(ax, task, va, len);
        if (s < 0 || (space >= 0 && s != space)) return false;
        space = s;
        if (!noCache && gen == sMapGen) {
            IOSimpleLockLock(okLock);
            sOk[sOkNext & 63] = XeOk{ task, va, va + len, s, gen }; sOkNext++;
            IOSimpleLockUnlock(okLock);
        }
        return true;
    };
    bool ok = true;
    bool draw = d.vertexCount && d.instanceCount;
    for (UInt32 i = 0; i < d.nrt && ok; i++) ok = use(d.rt[i].va, texSpan(d.rt[i]));
    if (draw) {
        ok = ok && use(d.vsVA, d.vsDW * 4ULL + 256) && use(d.psVA, d.psDW * 4ULL + 256);
        for (UInt32 i = 0; i < d.nvb && ok; i++) ok = use(d.vb[i].va, d.vb[i].size);
        // v0.32.5: the shaders' [[buffer(n)]] memory too (v0.32.3/4: unmapped -> zeros, page fault, hang)
        for (UInt32 i = 0; i < 16 && ok; i++) ok = use(d.vsBuf[i], d.vsLen[i] ? d.vsLen[i] : 4) && use(d.psBuf[i], d.psLen[i] ? d.psLen[i] : 4);
        if (d.ibType) ok = ok && d.ibVA && use(d.ibVA, d.ibSize);          // v0.33.0: index buffer
        for (UInt32 i = 0; i < d.ntex && ok; i++)
            ok = use(d.tex[i].va, texSpan(d.tex[i]));
        for (UInt32 i = 0; i < d.nvtex && i < XE_DRAW_MAX_TEX && ok; i++)   // v0.47.0
            ok = use(d.vtex[i].va, texSpan(d.vtex[i]));
        if (d.vsScratchVA) ok = ok && use(d.vsScratchVA, d.vsScratchLen);   // v0.48.0
        if (d.psScratchVA) ok = ok && use(d.psScratchVA, d.psScratchLen);
    }
    // v0.34.0: depth / stencil buffers (also for a clear without a draw)
    if (d.ds.depthVA) ok = ok && use(d.ds.depthVA, d.ds.depthSize);
    if (d.ds.stencilVA) ok = ok && use(d.ds.stencilVA, d.ds.stencilSize);
    if (!ok) return false;
    *spaceIO = space;
    return true;
}

// v0.49.0: batching. When the command buffer holds several draws in a row (the bundle keeps a render encoder's
// draws in one segment), they all go to the GPU as one job; the queue's calls for the draws already done are
// then skipped (sSkip). One queue command at a time (Apple's queue lock).
#define XE_ACCEL_BATCH 16
static const void *sSkip[XE_ACCEL_BATCH];
static UInt32 sSkipN, sSkipI;
static bool skipDone(const void *cmd) {
    if (sSkipI < sSkipN && cmd == sSkip[sSkipI]) { sSkipI++; return true; }
    sSkipN = sSkipI = 0;
    return false;
}
static UInt64 sBatchJobs, sBatchDraws, sLookEnd;
static void batchStatPublish(UInt32 batchedNow) {
    if (!IrisXeAccelerator::sInstance) return;
    char t[260];
    snprintf(t, sizeof(t), "segments %llu, draws in them %llu (most in one %llu), last segment %llu bytes | "
             "batched jobs %llu with %llu draws (last %u) | last look-ahead %llu bytes",
             sSegs, sSegDraws, sSegMaxDraws, sSegBytes, sBatchJobs, sBatchDraws, batchedNow, sLookEnd);
    IrisXeAccelerator::sInstance->setProperty("IrisXe-AccelBatch", t);
}
// v0.68.33: hang guard. A draw that hung the GPU (reset, -9) is remembered by its shader code (address space +
// vertex and fragment code addresses); the same draw is then skipped instead of hanging the GPU again for 2 s
// each time - Brave froze the whole desktop for a minute that way (a hang every second). Kept: the last 32.
// Published as IrisXe-HungDraws.
struct HungDraw { UInt32 space; UInt64 vs, ps; };
static HungDraw sHung[32];
static UInt32 sHungN, sHungSkips;
static bool hungKnown(UInt32 space, const XeDrawDesc &d) {
    for (UInt32 i = 0; i < 32 && i < sHungN; i++)
        if (sHung[i].space == space && sHung[i].vs == d.vsVA && sHung[i].ps == d.psVA) return true;
    return false;
}
static void hungPublish(IrisXeAccelerator *ax, const char *who, const XeDrawDesc &d) {
    char t[300];
    snprintf(t, sizeof(t), "%u draws hung the GPU (skipped later: %u) | last: %s, code vs 0x%llx ps 0x%llx, %u vertices, "
             "target %ux%u format %u, %u textures", sHungN, sHungSkips, who, d.vsVA, d.psVA, d.vertexCount,
             d.nrt ? d.rt[0].width : 0, d.nrt ? d.rt[0].height : 0, d.nrt ? d.rt[0].format : 0, d.ntex);
    ax->setProperty("IrisXe-HungDraws", t);
}
static IRISXE_SLOT_RET runDraw(IOAccelCommandQueue *q, IrisXeAccelerator *ax, IOAccelTask *task,
                               const XeDrawCmd *k, const UInt8 *end, IOAccelSegmentResourceList *list) {
    static XeDrawDesc batch[XE_ACCEL_BATCH];
    // v0.49.1: the queue may pass only this command's end: look ahead to the end of its segment
    if (sSegStart && (const UInt8 *)k >= sSegStart && (const UInt8 *)k < sSegEnd && sSegEnd > end) end = sSegEnd;
    sLookEnd = (UInt64)(end - (const UInt8 *)k);
    uint64_t tLook = mach_absolute_time();          // v0.60.0
    int space = -1;
    if (!prepDraw(ax, task, k, end, &batch[0], &space)) { ax->countMetal(10, 1); return 0; }
    UInt32 cnt = 1;
    const UInt8 *p = (const UInt8 *)k + k->size;
    while (cnt < XE_ACCEL_BATCH && p + sizeof(XeDrawCmd) <= end) {   // the draws right after it
        const XeDrawCmd *c = (const XeDrawCmd *)p;
        if (c->type != XE_KCMD_DRAW || c->size < sizeof(XeDrawCmd) || (c->size & 3)) break;
        int sp = space;
        if (!prepDraw(ax, task, c, end, &batch[cnt], &sp)) break;     // it runs on its own (and reports)
        sSkip[cnt - 1] = c;
        cnt++;
        p += c->size;
    }
    sSkipN = cnt - 1; sSkipI = 0;
    if (space < 0) { space = irisxeMapForGPU(ax, task, 0, 0); if (space < 0) space = 0; }
    if (sHungN) {                                   // v0.68.33: drop draws that hung the GPU before
        UInt32 kept = 0;
        for (UInt32 i = 0; i < cnt; i++) {
            if (batch[i].vertexCount && hungKnown((UInt32)space, batch[i])) { sHungSkips++; continue; }
            if (kept != i) batch[kept] = batch[i];
            kept++;
        }
        if (!kept) { ax->countMetal(10, cnt); return 0; }
        cnt = kept;
    }
    trace(q, 5, batch[0].vertexCount, batch[0].clearMask);
    uint64_t tDraw = mach_absolute_time();
    // v0.63.0: debug.irisxe_async - the last job is left running; the command buffer completes later
    bool wantAsync = IrisXeAsyncMode() && ax->coreABI() >= 10 && asyncCheck(q, ax);
    UInt32 aseq = 0;
    int r = ax->coreDraws((UInt32)space, batch, cnt, 2000, wantAsync ? &aseq : nullptr);
    if (aseq) asyncNote(q, ax, list, aseq);
    sKtLookNs += ktNs(tDraw - tLook); sKtDrawNs += ktNs(mach_absolute_time() - tDraw);   // v0.60.0
    sKtJobs++; sKtDraws += cnt;
    ax->countMetal(r == 0 ? 9 : 10, cnt);
    if (cnt > 1) { ax->countMetal(11, 1); sBatchJobs++; sBatchDraws += cnt; }   // v0.49.0: batched jobs
    static UInt32 pubN;
    if (cnt > 1 || (++pubN & 63) == 1) batchStatPublish(cnt);
    if (r) {
        char who[32] = "?";
        proc_selfname(who, sizeof(who));
        IOLog("IrisXeAccel: %u draw(s), first of %u vertices, failed (%d) - %s, code vs 0x%llx ps 0x%llx, target %ux%u format %u, %u textures\n",
              cnt, batch[0].vertexCount, r, who, batch[0].vsVA, batch[0].psVA, batch[0].nrt ? batch[0].rt[0].width : 0,
              batch[0].nrt ? batch[0].rt[0].height : 0, batch[0].nrt ? batch[0].rt[0].format : 0, batch[0].ntex);
        // v0.68.33: it hung - remember it. Only a single draw: in a batch the one that hung is not known, and
        // skipping a common shader (the desktop's own) would leave parts of the screen undrawn
        if (r == -9 && cnt == 1) {
            for (UInt32 i = 0; i < cnt; i++) {
                if (!batch[i].vertexCount || hungKnown((UInt32)space, batch[i])) continue;
                sHung[sHungN % 32] = { (UInt32)space, batch[i].vsVA, batch[i].psVA };
                sHungN++;
            }
            hungPublish(ax, who, batch[0]);
        }
    }
    return 0;
}

IRISXE_SLOT_RET IrisXeCommandQueue::processSegmentKernelCommand(IOAccelSegmentResourceList *list,
        IOAccelKernelCommand const *cmd, IOAccelKernelCommand const *end) {
    const UInt32 *w = (const UInt32 *)cmd;
    trace(this, 4, w[0], w[1]);
    if (w[0] != XE_KCMD_FILL && w[0] != XE_KCMD_COPY && w[0] != XE_KCMD_DISPATCH && w[0] != XE_KCMD_DRAW && w[0] != XE_KCMD_COPY2D) {
        if (IrisXeAccelerator::sInstance) IrisXeAccelerator::sInstance->countMetal(5, 1);
        return IOAccelCommandQueue::processSegmentKernelCommand(list, cmd, end);
    }
    IrisXeAccelerator *ax = IrisXeAccelerator::sInstance;
    if (w[0] == XE_KCMD_DRAW && skipDone(cmd)) return 0;   // v0.49.0: already run with the draw before it
    if (ax && w[0] == XE_KCMD_DRAW)
        return runDraw(this, ax, (IOAccelTask *)getGPUTask(), (const XeDrawCmd *)cmd, (const UInt8 *)end, list);
    if (ax && w[0] == XE_KCMD_DISPATCH)
        return runDispatch(this, ax, (IOAccelTask *)getGPUTask(), (const XeDispatchCmd *)cmd, (const UInt8 *)end);
    if (ax && w[0] == XE_KCMD_COPY2D) {                  // v0.36.0
        const XeCopy2DCmd *k = (const XeCopy2DCmd *)cmd;
        if (w[1] < sizeof(XeCopy2DCmd) || (const UInt8 *)end - (const UInt8 *)cmd < (long)sizeof(XeCopy2DCmd) ||
            !k->widthBytes || !k->height || k->widthBytes > k->srcPitch || k->widthBytes > k->dstPitch ||
            k->height > 65536 || k->srcPitch > (1u << 20) || k->dstPitch > (1u << 20)) { ax->countMetal(3, 1); return 0; }
        UInt64 sLen = (UInt64)k->srcPitch * (k->height - 1) + k->widthBytes, dLen = (UInt64)k->dstPitch * (k->height - 1) + k->widthBytes;
        if (sLen > (256ULL << 20) || dLen > (256ULL << 20) || k->srcVA + sLen > 0xFFFFFFFFULL || k->dstVA + dLen > 0xFFFFFFFFULL) {
            ax->countMetal(3, 1); return 0;
        }
        IOAccelTask *task = (IOAccelTask *)getGPUTask();
        int space = ensureMapped(ax, task, k->dstVA, dLen);
        if (space < 0 || ensureMapped(ax, task, k->srcVA, sLen) != space) return 0;
        UInt64 all = k->srcVA | k->dstVA | k->srcPitch | k->dstPitch | k->widthBytes;
        UInt32 unit = (all & 3) == 0 ? 4 : (all & 1) == 0 ? 2 : 1;
        UInt32 wEl = k->widthBytes / unit;
        // pitch field: 16 bits signed, and a multiple of 4 bytes (v0.36.0: a 13-byte pitch copied only the first
        // row right) - otherwise one blit per row
        // v0.57.10: the blitter adds the low 5 bits of the base address and of row * pitch WITHOUT the carry into
        // bit 5: when both are not multiples of 32, some rows go 32 bytes too low (irisxe-metaltest --blittest:
        // offset 3644 / pitch 1200 -> every 2nd row read 32 bytes early, offset 3644 / pitch 1204 -> 7 of 8 rows;
        // offset 0 or 64, or a pitch that is a multiple of 32, always right). Then one blit per row (its own base).
        bool carry = ((k->srcVA & 31) && (k->srcPitch & 31)) || ((k->dstVA & 31) && (k->dstPitch & 31));
        bool rows = k->srcPitch > 32767 || k->dstPitch > 32767 || ((k->srcPitch | k->dstPitch) & 3) || carry;
        static UInt32 c[960];
        UInt32 n = 0; int r = 0;
        for (UInt32 y = 0; y < k->height && !r; ) {
            UInt32 h = rows ? 1 : (k->height - y > 8192 ? 8192 : k->height - y);
            for (UInt32 x = 0; x < wEl && !r; ) {
                UInt32 cw = wEl - x > 16384 ? 16384 : wEl - x;
                n += copyBlt2D(c + n, k->dstVA + (UInt64)y * k->dstPitch + (UInt64)x * unit, rows ? 0 : k->dstPitch,
                               k->srcVA + (UInt64)y * k->srcPitch + (UInt64)x * unit, rows ? 0 : k->srcPitch, cw, h, unit);
                x += cw;
                if (n + 10 > 960) { r = ax->coreRunBlit((UInt32)space, c, n, 2000); n = 0; }
            }
            y += h;
        }
        if (n && !r) r = ax->coreRunBlit((UInt32)space, c, n, 2000);
        ax->countMetal(r == 0 ? 2 : 3, 1);
        if (r) IOLog("IrisXeAccel: GPU rectangle copy %u x %u failed (%d)\n", k->widthBytes, k->height, r);
        return 0;
    }
    bool isFill = w[0] == XE_KCMD_FILL;
    UInt32 need = isFill ? sizeof(XeFillCmd) : sizeof(XeCopyCmd);
    if (!ax || w[1] < need || (const UInt8 *)end - (const UInt8 *)cmd < (long)need) {
        if (ax) ax->countMetal(3, 1);
        return 0;
    }
    const XeFillCmd *f = (const XeFillCmd *)cmd;
    const XeCopyCmd *k = (const XeCopyCmd *)cmd;
    UInt64 dst = isFill ? f->gpuVA : k->dstVA, src = isFill ? 0 : k->srcVA;
    UInt64 len = isFill ? f->length : k->length;
    if (((dst | src | len) & 3) || !len || len > (64ULL << 20)) {   // 64 MB per command for now
        ax->countMetal(3, 1);
        return 0;
    }
    // the range as a 32 bpp surface: full 4 KB rows, then one shorter row
    UInt32 c[48]; UInt32 n = 0;
    UInt64 rows = len / 4096, rest = len % 4096, off = 0;
    if (rows) {
        UInt32 h = (UInt32)rows;                          // <= 16384 rows for 64 MB
        n += isFill ? colorBlt(c + n, dst, 4096, 1024, h, f->pattern) : copyBlt(c + n, dst, src, 4096, 1024, h);
        off = rows * 4096;
    }
    if (rest)
        n += isFill ? colorBlt(c + n, dst + off, 4096, (UInt32)(rest / 4), 1, f->pattern)
                    : copyBlt(c + n, dst + off, src + off, 4096, (UInt32)(rest / 4), 1);
    trace(this, 5, (UInt32)dst, (UInt32)len);
    IOAccelTask *task = (IOAccelTask *)getGPUTask();
    int space = ensureMapped(ax, task, dst, len);
    if (space < 0 || (!isFill && ensureMapped(ax, task, src, len) != space)) return 0;
    int r = ax->coreRunBlit((UInt32)space, c, n, 2000);
    ax->countMetal(r == 0 ? 2 : 3, 1);
    if (r) IOLog("IrisXeAccel: GPU %s 0x%llx +%llu failed (%d)\n", isFill ? "fill" : "copy", dst, len, r);
    return 0;
}

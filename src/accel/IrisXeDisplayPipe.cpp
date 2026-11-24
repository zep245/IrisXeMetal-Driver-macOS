// v0.56.0: IrisXeDisplayPipe - see IrisXeDisplayPipe.hpp for how a screen update travels.
// Stage A (v0.56): the CPU copies each finished screen image into the framebuffer the panel already shows.
// v0.59.0: direct scanout - each of WindowServer's screen images gets a GGTT window once (IrisXeCore), and a frame is
// one PLANE_SURF write: the panel shows the image itself from its next vertical blank. The transaction is complete
// when PLANE_SURFLIVE says so (WindowServer may not draw into the image on screen before). Falls back to the copy for
// anything unusual; after 2 s without frames the panel goes back to its own framebuffer (the last image copied in).
// sysctl debug.irisxe_scanout: 0 copy, 1 direct (default), 2 direct without the cache flush; boot-arg -irisxecopy = 0.
#include "IrisXeDisplayPipe.hpp"
#include "IrisXeAccel.hpp"
#include "../IrisXeSpaceABI.h"
#include <IOKit/graphics/IOFramebuffer.h>
#include <libkern/libkern.h>

OSDefineMetaClassAndStructors(IrisXeDisplayPipe, IOAccelDisplayPipe)

static const IRISXE_SLOT_RET kXeNotHandled = 0xe0014042;   // Apple's base submitTransaction: "no GPU flip"
static const uint32_t kXeBGRA = 0x42475241;                // 'BGRA': what CoreDisplay's DisplaySurface uses
static char sPipeStatus[640];
extern int IrisXeScanMode();
extern void IrisXeFrameTimingPublish(const char *t);         // IrisXeAccelMachines.cpp: IrisXe-FrameTiming
static inline uint64_t xeNs(uint64_t abs) { uint64_t ns = 0; absolutetime_to_nanoseconds(abs, &ns); return ns; }                                // IrisXeAccel.cpp: debug.irisxe_scanout

extern void IrisXeDisplayPipeStep(const char *step);        // IrisXeAccelMachines.cpp: IrisXe-DisplayPipe property

// v0.56.3 (research round 30): IOAccelDisplayPipeUserClient2::transactionEnd answers "not ready" (0xe00002d8, every
// WindowServer frame) unless all three hold:
//   1. accelerator +0xc78 bit 2: "accelerator enabled" (set by IOGraphicsAccelerator2::enableAccelerator)
//   2. pipe +0x282 == 0
//   3. pipe +0x298 != 0: IOAccelDisplayPipe::isActive(). Only init_framebuffer_resource() sets it, and only when the
//      pipe's initFramebufferResource() hands back the framebuffer's memory. Apple's base version asks the display
//      machine, whose base version returns nothing; Apple's own drivers override it in their pipe (paravirtual GPU:
//      IOAccelSysMemory::withMemoryDescriptor(accelerator, resource, framebuffer memory)). Ours did not, so the pipe
//      was never active. Apple calls init_framebuffer_resource on a display mode change; our pipe is made after
//      login (debug.irisxe_dpipe), after the last mode change, so activate() calls it once itself.
extern "C" bool _ZN18IOAccelDisplayPipe25init_framebuffer_resourceEP16IOAccelResource2(IOAccelDisplayPipe *, IOAccelResource2 *);
extern "C" OSObject *_ZN16IOAccelSysMemory20withMemoryDescriptorEP22IOGraphicsAccelerator2P16IOAccelResource2P18IOMemoryDescriptor(
    IOGraphicsAccelerator2 *, IOAccelResource2 *, IOMemoryDescriptor *);
extern "C" void _ZN22IOGraphicsAccelerator217enableAcceleratorEv(IOGraphicsAccelerator2 *self);

IrisXeDisplayPipe *IrisXeDisplayPipe::sPipe = nullptr;

// Apple's fields (offsets from IOAccelDisplayPipe::init / transactionEnd, macOS 15)
static inline UInt8 *xeBytes(const void *p) { return (UInt8 *)p; }
#define XE_ACCEL(p)   (*(IOGraphicsAccelerator2 **)(xeBytes(p) + 0x88))
#define XE_FBRES(p)   (*(IOAccelResource2 **)(xeBytes(p) + 0xe0))
#define XE_ACCEL_ON(a) ((xeBytes(a)[0xc78] & 2) != 0)

bool IrisXeDisplayPipe::sizeOK() {
    const OSMetaClass *am = OSMetaClass::getMetaClassWithName(OSSymbol::withCStringNoCopy("IOAccelDisplayPipe"));
    unsigned need = am ? am->getClassSize() : 0;
    unsigned have = (unsigned)(offsetof(IrisXeDisplayPipe, _apple) + sizeof(((IrisXeDisplayPipe *)0)->_apple));
    IOLog("IrisXeAccel: display pipe: Apple's IOAccelDisplayPipe is %u bytes, our room %u -> %s\n", need, have,
          need && need <= have ? "ours" : "Apple's base class (too small)");
    return need && need <= have;
}

void IrisXeDisplayPipe::note(const char *what) {
    IOGraphicsAccelerator2 *a = XE_ACCEL(this);
    uint64_t avg = xeCopyCount ? xeCopyNs / xeCopyCount : 0;
    // v0.56.3: the three "ready" checks of transactionEnd, read live (1 = passes)
    snprintf(sPipeStatus, sizeof(sPipeStatus),
             "IrisXeDisplayPipe: %s | ready: active %u, accel-on %u, not-busy %u | new %u, validate %u, submit %u, "
             "perform %u, complete %u, vbl %u | frames %llu, skipped %llu, fb %ux%u row %u, fbres %u/%u | "
             "copy avg %llu.%02llu ms, max %llu.%02llu ms, fast %llu, slow %llu | "
             "scanout mode %d: direct %llu, flush avg %llu us max %llu us, on screen after avg %llu us max %llu us, "
             "slot maps %llu (%llu us), timeouts %u, back to own fb %u, failed %u",
             what, xeBytes(this)[0x298] != 0, a ? XE_ACCEL_ON(a) : 0, xeBytes(this)[0x282] == 0,
             xeNewTx, xeValidate, xeSubmit, xePerform, xeComplete, xeTicks,
             xeFrames, xeSkipped, xeFBW, xeFBH, xeFBRow, xeInitRes, xeDestroyRes,
             avg / 1000000, (avg / 10000) % 100, xeCopyMaxNs / 1000000, (xeCopyMaxNs / 10000) % 100, xeFast, xeSlow,
             IrisXeScanMode(), xeDirect, xeDirect ? xeFlushNs / xeDirect / 1000 : 0, xeFlushMaxNs / 1000,
             xeFlipCount ? xeFlipNs / xeFlipCount / 1000 : 0, xeFlipMaxNs / 1000,
             xeSlotMaps, xeSlotMaps ? xeMapNs / xeSlotMaps / 1000 : 0, xeTimeouts, xeParks, xeScanFail);
    IrisXeDisplayPipeStep(sPipeStatus);
}

IRISXE_SLOT_RET IrisXeDisplayPipe::init(IOGraphicsAccelerator2 *a, IOAccelDisplayMachine *m, IOFramebuffer *fb,
                                        unsigned int idx) {
    xeLoop = nullptr; xeTimer = nullptr; xeFBMap = nullptr; xeFBMem = nullptr; xeInitRes = xeDestroyRes = 0;
    bzero(xeSrc, sizeof(xeSrc));
    xeUseClock = xeCopyNs = xeCopyMaxNs = xeCopyCount = xeFast = xeSlow = 0;
    xeFBRow = xeFBW = xeFBH = 0; xeFrames = xeSkipped = 0; xeLastFormat = 0; xeVBL = false;
    xeNewTx = xeValidate = xeSubmit = xePerform = xeComplete = xeTicks = 0;
    xeLock = IOLockAlloc();
    xeShowIdx = xePendIdx = -1; xePendAddr = 0; xePendSince = xeLastPerform = 0;
    xeShowW = xeShowH = xeShowBpr = 0;
    xeDirect = xeFlushNs = xeFlushMaxNs = xeFlipNs = xeFlipMaxNs = xeFlipCount = xeMapNs = xeSlotMaps = 0;
    xeTimeouts = xeBackoff = xeParks = xeScanFail = 0; xeVblUS = 0; xePeriodUS = 16667;
    xeSubmitT = 0;
    xeFc = xeFcLine = 0; xeFcOK = false; xeFcSeen = xeFcStuckSince = 0; xeLineSum = xeLineMax = 0;
    xeRealVbl = xeTimerVbl = 0;
    timingReset();
    IRISXE_SLOT_RET ok = IOAccelDisplayPipe::init(a, m, fb, idx);
    if (!(ok & 0xff)) { IOLog("IrisXeAccel: display pipe: Apple's init failed\n"); return ok; }
    // 60 Hz stand-in for the panel's vertical blank (own work loop: the timer never waits on anything)
    xeLoop = IOWorkLoop::workLoop();
    if (xeLoop) xeTimer = IOTimerEventSource::timerEventSource(this, &IrisXeDisplayPipe::vblTick);
    if (xeTimer && xeLoop->addEventSource(xeTimer) == kIOReturnSuccess) xeTimer->setTimeoutUS(16667);
    else IOLog("IrisXeAccel: display pipe: no VBL timer\n");
    sPipe = this;
    note(IrisXeScanMode() ? "made (direct scanout, CPU copy as fallback, 60 Hz VBL timer)" : "made (CPU copy into the framebuffer, 60 Hz VBL timer)");
    return ok;
}

// v0.56.3: the framebuffer's memory for Apple's framebuffer resource (like Apple's paravirtual GPU). Called by
// init_framebuffer_resource, which then marks the pipe active.
IRISXE_SLOT_RET IrisXeDisplayPipe::initFramebufferResource(unsigned int plane, IOAccelResource2 *res) {
    xeInitRes++;
    IOFramebuffer *fb = getFramebuffer();
    IOGraphicsAccelerator2 *a = XE_ACCEL(this);
    if (!xeFBMem && fb) xeFBMem = fb->getApertureRange(kIOFBSystemAperture);   // retained, released in free()
    if (!a || !res || !xeFBMem) {
        IOLog("IrisXeAccel: display pipe: initFramebufferResource: %s\n", !xeFBMem ? "no framebuffer memory" : "no resource");
        return 0;
    }
    OSObject *m = _ZN16IOAccelSysMemory20withMemoryDescriptorEP22IOGraphicsAccelerator2P16IOAccelResource2P18IOMemoryDescriptor(
        a, res, xeFBMem);
    IOLog("IrisXeAccel: display pipe: initFramebufferResource plane %u: framebuffer memory 0x%llx +%llu -> %s\n", plane,
          (unsigned long long)xeFBMem->getPhysicalAddress(), (unsigned long long)xeFBMem->getLength(), m ? "ok" : "FAILED");
    return (IRISXE_SLOT_RET)m;
}

IRISXE_SLOT_RET IrisXeDisplayPipe::destroyFramebufferResource(unsigned int plane, IOAccelResource2 *res) {
    xeDestroyRes++;
    return IOAccelDisplayPipe::destroyFramebufferResource(plane, res);
}

// v0.56.3: called once after Apple's display machine made the pipe (debug.irisxe_dpipe, normal thread)
void IrisXeDisplayPipe::activate() {
    IOGraphicsAccelerator2 *a = XE_ACCEL(this);
    IOAccelResource2 *res = XE_FBRES(this);
    if (!a || !res) { note("activate: no accelerator or framebuffer resource"); return; }
    if (!XE_ACCEL_ON(a)) _ZN22IOGraphicsAccelerator217enableAcceleratorEv(a);   // we enabled it at start already
    bool ok = xeBytes(this)[0x298] != 0;
    if (!ok) {
        // Apple holds the accelerator lock (accelerator +0x88) around this call
        IOLock *lk = *(IOLock **)(xeBytes(a) + 0x88);
        if (lk) IOLockLock(lk);
        ok = _ZN18IOAccelDisplayPipe25init_framebuffer_resourceEP16IOAccelResource2(this, res);
        if (lk) IOLockUnlock(lk);
    }
    note(ok ? "activated (framebuffer resource ready)" : "activate FAILED (init_framebuffer_resource said no)");
}


// v0.59.2: the display's frame counter (+1 at every real vertical blank) and the scanline shown now
bool IrisXeDisplayPipe::readFrame(uint32_t *fc, uint32_t *line) {
    IrisXeAccelerator *ax = IrisXeAccelerator::sInstance;
    if (!ax) return false;
    XeScanArgs a = {};
    a.op = kXeScanFrame;
    if (ax->coreScan(&a) != 0) return false;
    *fc = a.addr; *line = a.stride;
    return true;
}

void IrisXeDisplayPipe::vblTick(OSObject *owner, IOTimerEventSource *t) {
    IrisXeDisplayPipe *self = OSDynamicCast(IrisXeDisplayPipe, owner);
    if (!self) return;
    uint64_t now = mach_absolute_time();
    bool vbl = false;
    // v0.59.2: a real vertical blank since the last look? (frame counter changed)
    uint32_t fc = 0, line = 0;
    bool haveFc = self->readFrame(&fc, &line);
    if (haveFc) {
        if (!self->xeFcSeen) { self->xeFc = fc; self->xeFcSeen = now; self->xeFcStuckSince = now; }
        else if (fc != self->xeFc) {
            self->xeFc = fc; self->xeFcStuckSince = now; self->xeFcOK = true;
            self->xeRealVbl++; vbl = true;
            self->xeLineSum += line; if (line > self->xeLineMax) self->xeLineMax = line;
            self->xeVblUS = 0;
        } else if (xeNs(now - self->xeFcStuckSince) > 50000000ULL) self->xeFcOK = false;   // 50 ms: not moving
    } else self->xeFcOK = false;
    if (!self->xeFcOK) {
        // fallback: the 60 Hz stand-in
        self->xeVblUS += self->xePeriodUS;
        if (self->xeVblUS >= 16667) {
            self->xeVblUS -= 16667;
            if (self->xeVblUS > 16667) self->xeVblUS = 0;
            self->xeTimerVbl++; vbl = true;
        }
    }
    // while a flip waits for the panel: tell the family the moment it is on screen
    if (self->xePendIdx >= 0 && self->flipLanded()) self->signalTransactionInterrupt(nullptr);
    if (vbl) {
        // Apple's signalVBLInterrupt: wakes VBL waiters while VBL is enabled, and raises the transaction interrupt
        // while a transaction is waiting for one (both checked inside; safe from any thread, meant for interrupts)
        self->signalVBLInterrupt(nullptr);
        // v0.56.1: once a second, publish which steps were reached (IrisXe-DisplayPipe on the accelerator)
        if ((++self->xeTicks % 60) == 0) {
            self->note(self->xeFrames ? "showing images" : "waiting for images");
            self->timingPublish();                       // v0.59.1
        }
        // v0.59.0: 2 s without a new frame while the panel shows one of WindowServer's images: back to the panel's
        // own framebuffer (with that image copied in), so nothing depends on WindowServer's images when it stops
        // (WindowServer restart, the pipe test's undo, sleep). The next frame flips again.
        if (self->xeShowIdx >= 0 && self->xePendIdx < 0 && self->xeLastPerform &&
            xeNs(now - self->xeLastPerform) > 2000000000ULL && self->xeLock && IOLockTryLock(self->xeLock)) {
            if (self->xeShowIdx >= 0 && self->xePendIdx < 0) self->park(true);
            IOLockUnlock(self->xeLock);
        }
    }
    // how often to look (frame counter in use): every 1 ms while the screen moves (a frame in the last 0.5 s or a
    // flip waiting), 4 ms while WindowServer has VBL interrupts on, else 8 ms; fallback: the next 60 Hz tick
    bool busy = self->xePendIdx >= 0 || (self->xeLastPerform && xeNs(now - self->xeLastPerform) < 500000000ULL);
    if (self->xeFcOK || (haveFc && busy)) self->xePeriodUS = busy ? 1000 : self->xeVBL ? 4000 : 8000;
    else self->xePeriodUS = self->xePendIdx >= 0 ? 2000 : (16667 - self->xeVblUS);
    if (self->xePeriodUS < 1000) self->xePeriodUS = 1000;
    t->setTimeoutUS(self->xePeriodUS);
}

IRISXE_SLOT_RET IrisXeDisplayPipe::enableVBLInterrupt()  { xeVBL = true;  return 0; }   // the timer always runs
IRISXE_SLOT_RET IrisXeDisplayPipe::disableVBLInterrupt() { xeVBL = false; return 0; }

// v0.56.1: counted, so a test shows whether IOPresentment's transactions reach the kernel at all
IRISXE_SLOT_RET IrisXeDisplayPipe::newDisplayPipeTransaction() {
    if (++xeNewTx <= 3) IOLog("IrisXeAccel: display pipe: new transaction #%u\n", xeNewTx);
    return IOAccelDisplayPipe::newDisplayPipeTransaction();
}

IRISXE_SLOT_RET IrisXeDisplayPipe::validateTransaction(IOAccelDisplayPipeTransaction2 *tx) {
    if (++xeValidate <= 3) IOLog("IrisXeAccel: display pipe: validate #%u\n", xeValidate);
    return 0;   // like Apple's base class; transactions without an image (gamma only) are fine too
}

IRISXE_SLOT_RET IrisXeDisplayPipe::submitTransaction(IOAccelDisplayPipeTransaction2 *tx) {
    if (++xeSubmit <= 3) IOLog("IrisXeAccel: display pipe: submit #%u\n", xeSubmit);
    xeSubmitT = mach_absolute_time();                // v0.59.1: the family now waits for the frame's GPU work
    // "no GPU flip": the family waits for the image's GPU work (barrier event), then calls performTransaction
    return kXeNotHandled;
}

bool IrisXeDisplayPipe::mapFramebuffer() {
    if (xeFBMap) return true;
    IOFramebuffer *fb = getFramebuffer();
    if (!fb) return false;
    IODisplayModeID mode = 0; IOIndex depth = 0;
    IOPixelInformation info;
    bzero(&info, sizeof(info));
    if (fb->getCurrentDisplayMode(&mode, &depth) != kIOReturnSuccess ||
        fb->getPixelInformation(mode, depth, kIOFBSystemAperture, &info) != kIOReturnSuccess) return false;
    IODeviceMemory *mem = fb->getApertureRange(kIOFBSystemAperture);
    if (!mem) return false;
    xeFBMap = mem->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapWriteCombineCache);
    mem->release();
    if (!xeFBMap) return false;
    xeFBRow = info.bytesPerRow; xeFBW = info.activeWidth; xeFBH = info.activeHeight;
    IOLog("IrisXeAccel: display pipe: framebuffer mapped %ux%u, %u bytes per row\n", xeFBW, xeFBH, xeFBRow);
    return true;
}

// v0.56.6: the kernel address of an IOSurface's memory, mapped (and wired) once and kept for the next frames.
// WindowServer cycles through a few display surfaces; the least recently used of 4 slots is replaced.
// v0.59.0: never the slot on screen or the one waiting to go on screen (their memory must stay).
int IrisXeDisplayPipe::srcSlot(IOMemoryDescriptor *md, uint64_t need) {
    if (!md || md->getLength() < need) return -1;
    xeUseClock++;
    int victim = -1;
    for (int i = 0; i < 4; i++) {
        if (xeSrc[i].md == md && xeSrc[i].map) { xeSrc[i].lastUse = xeUseClock; return i; }
        if (i == xeShowIdx || i == xePendIdx) continue;
        if (victim < 0 || !xeSrc[i].md) { if (victim < 0 || xeSrc[victim].md) victim = i; }
        else if (xeSrc[victim].md && xeSrc[i].lastUse < xeSrc[victim].lastUse) victim = i;
    }
    if (victim < 0) return -1;
    XeSrc &v = xeSrc[victim];
    if (v.md) { OSSafeReleaseNULL(v.map); v.md->complete(); v.md->release(); v.md = nullptr; }
    v.ggtt = 0; v.gen = 0;
    if (md->prepare() != kIOReturnSuccess) return -1;
    IOMemoryMap *m = md->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapReadOnly);
    if (!m) { md->complete(); return -1; }
    md->retain();
    v.md = md; v.map = m; v.lastUse = xeUseClock;
    return victim;
}

uint8_t *IrisXeDisplayPipe::srcMapping(IOMemoryDescriptor *md, uint64_t need) {
    int i = srcSlot(md, need);
    return i < 0 ? nullptr : (uint8_t *)xeSrc[i].map->getVirtualAddress();
}

// v0.59.0: arm a flip to slot i's image (xeLock held). false: use the copy for this frame.
bool IrisXeDisplayPipe::scanShow(int i, uint64_t w, uint64_t h, uint64_t bpr) {
    IrisXeAccelerator *ax = IrisXeAccelerator::sInstance;
    XeSrc &v = xeSrc[i];
    if (!ax || !v.md || !v.map) return false;
    uint64_t need = (h - 1) * bpr + w * 4;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!v.ggtt) {                                   // first time this image is shown: its GGTT window
            uint64_t t0 = mach_absolute_time();
            uint32_t pages = (uint32_t)((need + 4095) / 4096);
            UInt64 *pa = (UInt64 *)IOMalloc(pages * sizeof(UInt64));
            if (!pa) return false;
            bool ok = true;
            for (uint32_t p = 0; p < pages && ok; p++) {
                IOByteCount len = 0;
                addr64_t ph = v.md->getPhysicalSegment((IOByteCount)p * 4096, &len, kIOMemoryMapperNone);
                ok = ph && !(ph & 0xFFF);
                pa[p] = ph;
            }
            XeScanArgs a = {};
            a.op = kXeScanMap; a.slot = (UInt32)i; a.pa = pa; a.count = pages;
            if (ok) ok = ax->coreScan(&a) == 0 && a.addr;
            IOFree(pa, pages * sizeof(UInt64));
            if (!ok) return false;
            v.ggtt = a.addr; v.gen = a.gen;
            xeSlotMaps++; xeMapNs += xeNs(mach_absolute_time() - t0);
        }
        // the GPU drew the image through the shared last-level cache; the display reads memory directly
        if (IrisXeScanMode() == 1) {
            uint64_t t0 = mach_absolute_time();
            const volatile uint8_t *p = (const volatile uint8_t *)v.map->getVirtualAddress();
            __asm__ volatile("mfence" ::: "memory");
            for (uint64_t o = 0; o < need; o += 64) __asm__ volatile("clflushopt %0" :: "m"(p[o]));
            __asm__ volatile("sfence" ::: "memory");
            uint64_t ns = xeNs(mach_absolute_time() - t0);
            xeFlushNs += ns; if (ns > xeFlushMaxNs) xeFlushMaxNs = ns;
        }
        XeScanArgs f = {};
        f.op = kXeScanFlip; f.addr = v.ggtt; f.stride = (UInt32)bpr; f.gen = v.gen;
        int r = ax->coreScan(&f);
        if (r == 0) {
            xePendIdx = i; xePendAddr = v.ggtt; xePendSince = mach_absolute_time();
            xeShowW = (uint32_t)w; xeShowH = (uint32_t)h; xeShowBpr = (uint32_t)bpr;
            return true;
        }
        if (r != -3) return false;
        for (int k = 0; k < 4; k++) xeSrc[k].ggtt = 0;    // IrisXeCore re-initialised the GPU (wake): map again
    }
    return false;
}

// v0.59.0: is the armed flip on screen? (any thread; takes no lock: only reads a register and our fields)
bool IrisXeDisplayPipe::flipLanded() {
    IrisXeAccelerator *ax = IrisXeAccelerator::sInstance;
    uint32_t want = xePendAddr;
    if (!ax || xePendIdx < 0 || !want) return false;
    XeScanArgs a = {};
    a.op = kXeScanLive;
    return ax->coreScan(&a) == 0 && a.addr == want;
}

// v0.59.0: the panel's own framebuffer again (xeLock held). copyBack: the image on screen copied into it first, so
// the switch is invisible.
void IrisXeDisplayPipe::park(bool copyBack) {
    IrisXeAccelerator *ax = IrisXeAccelerator::sInstance;
    if (xeShowIdx < 0 && xePendIdx < 0) return;
    int i = xeShowIdx >= 0 ? xeShowIdx : xePendIdx;
    if (copyBack && i >= 0 && xeSrc[i].map && mapFramebuffer() && xeShowH && xeShowBpr) {
        uint8_t *dst = (uint8_t *)xeFBMap->getVirtualAddress();
        const uint8_t *src = (const uint8_t *)xeSrc[i].map->getVirtualAddress();
        uint32_t rows = xeShowH < xeFBH ? xeShowH : xeFBH;
        uint64_t rowBytes = (uint64_t)(xeShowW < xeFBW ? xeShowW : xeFBW) * 4;
        for (uint32_t y = 0; y < rows; y++) memcpy(dst + (uint64_t)y * xeFBRow, src + (uint64_t)y * xeShowBpr, (size_t)rowBytes);
    }
    XeScanArgs a = {};
    a.op = kXeScanHome;
    if (ax) ax->coreScan(&a);
    xeShowIdx = xePendIdx = -1; xePendAddr = 0;
    xeParks++;
}

void IrisXeDisplayPipe::dropSources() {
    for (int i = 0; i < 4; i++) {
        if (!xeSrc[i].md) continue;
        OSSafeReleaseNULL(xeSrc[i].map);
        xeSrc[i].md->complete();
        xeSrc[i].md->release();
        xeSrc[i].md = nullptr;
    }
}

IRISXE_SLOT_RET IrisXeDisplayPipe::performTransaction(IOAccelDisplayPipeTransaction2 *tx) {
    if (++xePerform <= 3) IOLog("IrisXeAccel: display pipe: perform #%u\n", xePerform);
    IOSurface *s = tx ? tx->getPlaneIOSurface(0, 0) : nullptr;
    if (!s) { xeSkipped++; signalTransactionInterrupt(nullptr); return 0; }   // nothing to show (e.g. gamma only)
    uint32_t fmt = s->getPixelFormat();
    uint64_t w = s->getWidth(), h = s->getHeight(), bpr = s->getBytesPerRow();
    IOMemoryDescriptor *md = s->getMemoryDescriptor();
    if (fmt != kXeBGRA || !md || !mapFramebuffer()) {
        xeSkipped++;
        if (fmt != xeLastFormat) {
            xeLastFormat = fmt;
            IOLog("IrisXeAccel: display pipe: image %llux%llu format 0x%08x not copied (%s)\n", w, h, fmt,
                  fmt != kXeBGRA ? "format" : !md ? "no memory" : "framebuffer not mapped");
        }
        signalTransactionInterrupt(nullptr);
        return 0;
    }
    if (xeLock) IOLockLock(xeLock);
    {   // v0.59.1: stutter meter. A gap under 250 ms between two frames = the screen is moving (animation, drag);
        // longer = it was still. Buckets: on time (<= 18 ms at 60 Hz), 1, 2, 3+ refreshes missed.
        uint64_t now = mach_absolute_time();
        if (xeSubmitT) {
            uint64_t w = xeNs(now - xeSubmitT);
            if (w < 1000000000ULL) { xeWaitNs += w; xeWaitCount++; if (w > xeWaitMaxNs) xeWaitMaxNs = w; }
            xeSubmitT = 0;
        }
        if (xeLastPerform) {
            uint64_t g = xeNs(now - xeLastPerform);
            if (g < 250000000ULL) {
                xeMoveNs += g; xeMoveFrames++;
                xeGap[g <= 18000000ULL ? 0 : g <= 35000000ULL ? 1 : g <= 52000000ULL ? 2 : 3]++;
                if (g > xeGapMaxNs) xeGapMaxNs = g;
            }
        }
        xeLastPerform = now;
    }
    // v0.59.0: direct scanout when the image fits the panel exactly
    int mode = IrisXeScanMode();
    if (xeBackoff) xeBackoff--;
    if (mode && !xeBackoff && w == xeFBW && h == xeFBH && h && !(bpr & 63) && bpr >= w * 4 && bpr / 64 <= 0x7FF &&
        (h - 1) * bpr + w * 4 <= XE_SCAN_SLOT_SIZE) {
        // the previous flip may be on screen without the family having asked yet: then it is the protected one
        if (xePendIdx >= 0 && flipLanded()) { xeShowIdx = xePendIdx; xePendIdx = -1; xePendAddr = 0; }
        int i = srcSlot(md, (h - 1) * bpr + w * 4);
        if (i >= 0 && scanShow(i, w, h, bpr)) {
            xeDirect++; xeFrames++;
            if (xeFrames == 1 || (xeFrames & 1023) == 0) {
                char m[96];
                snprintf(m, sizeof(m), "showing images (direct, last %llux%llu, %llu bytes per row)", w, h, bpr);
                note(m);
            }
            if (xeLock) IOLockUnlock(xeLock);
            return 0;                       // complete once the panel shows it (vblTick / isTransactionComplete)
        }
        xeScanFail++;
        if (xeScanFail <= 3) IOLog("IrisXeAccel: display pipe: direct scanout failed for %llux%llu (bpr %llu), copying\n", w, h, bpr);
    }
    uint32_t rows = (uint32_t)(h < xeFBH ? h : xeFBH);
    uint64_t rowBytes = (w < xeFBW ? w : xeFBW) * 4;
    uint8_t *dst = (uint8_t *)xeFBMap->getVirtualAddress();
    uint64_t t0 = mach_absolute_time();
    bool copied = false;
    // v0.56.6: fast path - the surface's memory mapped once, memcpy (one call when the rows line up)
    static int slowOnly = -1;                                  // boot-arg -irisxeslowcopy: the v0.56.0 copy only
    if (slowOnly < 0) slowOnly = PE_parse_boot_argn("-irisxeslowcopy", nullptr, 0) ? 1 : 0;
    uint8_t *src = slowOnly ? nullptr : srcMapping(md, (uint64_t)(rows ? rows - 1 : 0) * bpr + rowBytes);
    if (src) {
        if (bpr == xeFBRow && rowBytes == bpr) memcpy(dst, src, (size_t)rows * (size_t)bpr);
        else for (uint32_t y = 0; y < rows; y++) memcpy(dst + (uint64_t)y * xeFBRow, src + (uint64_t)y * bpr, (size_t)rowBytes);
        xeFast++; copied = true;
    } else if (md->prepare() == kIOReturnSuccess) {          // v0.56.0 path (kept as the fallback)
        for (uint32_t y = 0; y < rows; y++)
            md->readBytes((IOByteCount)(y * bpr), dst + (uint64_t)y * xeFBRow, (IOByteCount)rowBytes);
        md->complete();
        xeSlow++; copied = true;
    }
    if (copied) {
        uint64_t ns = 0;
        absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
        xeCopyNs += ns; xeCopyCount++;
        if (ns > xeCopyMaxNs) xeCopyMaxNs = ns;
        xeFrames++;
        if (xeFrames == 1 || (xeFrames & 1023) == 0) {
            char m[96];
            snprintf(m, sizeof(m), "showing images (last %llux%llu, %llu bytes per row)", w, h, bpr);
            note(m);
        }
    } else xeSkipped++;
    // v0.59.0: the panel showed one of WindowServer's images: back to its own framebuffer, which now holds this frame
    if (copied && (xeShowIdx >= 0 || xePendIdx >= 0)) park(false);
    if (xeLock) IOLockUnlock(xeLock);
    // done: raise the transaction interrupt now (the VBL timer would do it within 16 ms anyway)
    signalTransactionInterrupt(nullptr);
    return 0;
}

IRISXE_SLOT_RET IrisXeDisplayPipe::isTransactionComplete(IOAccelDisplayPipeTransaction2 *tx) {
    xeComplete++;
    if (xePendIdx < 0) return 1;            // copied: on screen already
    // v0.59.0: a flip is complete when the panel shows it; WindowServer may then draw into the previous image
    bool landed = flipLanded();
    uint64_t ns = xeNs(mach_absolute_time() - xePendSince);
    if (!landed && ns < 100000000ULL) return 0;
    if (xeLock) IOLockLock(xeLock);
    if (xePendIdx >= 0) {
        if (landed) { xeFlipNs += ns; xeFlipCount++; if (ns > xeFlipMaxNs) xeFlipMaxNs = ns; }
        else {
            // not on screen after 100 ms (panel off?): copy for the next 300 frames, then try again
            xeTimeouts++; xeBackoff = 300;
            if (xeTimeouts <= 3) IOLog("IrisXeAccel: display pipe: flip not on screen after 100 ms, copying for a while\n");
        }
        xeShowIdx = xePendIdx; xePendIdx = -1; xePendAddr = 0;
    }
    if (xeLock) IOLockUnlock(xeLock);
    return 1;
}

void IrisXeDisplayPipe::free() {
    if (xeLock) { IOLockLock(xeLock); park(true); IOLockUnlock(xeLock); }   // v0.59.0: the panel's own framebuffer
    if (xeTimer) { xeTimer->cancelTimeout(); if (xeLoop) xeLoop->removeEventSource(xeTimer); xeTimer->release(); xeTimer = nullptr; }
    OSSafeReleaseNULL(xeLoop);
    OSSafeReleaseNULL(xeFBMap);
    OSSafeReleaseNULL(xeFBMem);
    dropSources();
    if (xeLock) { IOLockFree(xeLock); xeLock = nullptr; }
    if (sPipe == this) sPipe = nullptr;
    IOAccelDisplayPipe::free();
}

// v0.59.1: stutter meter
void IrisXeDisplayPipe::timingReset() {
    xeWaitNs = xeWaitMaxNs = xeWaitCount = 0;
    xeMoveNs = xeMoveFrames = xeGapMaxNs = 0;
    for (int i = 0; i < 4; i++) xeGap[i] = 0;
    xeTimingSince = mach_absolute_time();
    xeFlushNs = xeFlushMaxNs = xeFlipNs = xeFlipMaxNs = xeFlipCount = 0;
    xeCopyNs = xeCopyMaxNs = xeCopyCount = 0;
    xeDirect = 0;
    xeRealVbl = xeTimerVbl = xeLineSum = xeLineMax = 0;
    timingPublish();
}

void IrisXeDisplayPipe::timingPublish() {
    char t[760];
    uint64_t n = xeMoveFrames, fps10 = xeMoveNs ? n * 10000000000ULL / xeMoveNs : 0;
    uint64_t pct = n ? xeGap[0] * 100 / n : 0;
    uint64_t wavg = xeWaitCount ? xeWaitNs / xeWaitCount / 1000 : 0;
    uint64_t cavg = xeCopyCount ? xeCopyNs / xeCopyCount / 1000 : 0;
    snprintf(t, sizeof(t),
             "mode %d, %llu s | moving: %llu frames, %llu.%llu fps, on time %llu%%, missed 1 refresh %llu, 2 %llu, 3+ %llu, "
             "worst gap %llu ms | GPU work wait avg %llu us, max %llu us | direct %llu: flush avg %llu us max %llu us, "
             "on screen after avg %llu us max %llu us | copied %llu: avg %llu us max %llu us | "
             "refresh signal: real %llu (seen at scanline avg %llu, max %llu), timer %llu",
             IrisXeScanMode(), xeNs(mach_absolute_time() - xeTimingSince) / 1000000000ULL,
             n, fps10 / 10, fps10 % 10, pct, xeGap[1], xeGap[2], xeGap[3], xeGapMaxNs / 1000000,
             wavg, xeWaitMaxNs / 1000, xeDirect, xeDirect ? xeFlushNs / xeDirect / 1000 : 0, xeFlushMaxNs / 1000,
             xeFlipCount ? xeFlipNs / xeFlipCount / 1000 : 0, xeFlipMaxNs / 1000,
             xeCopyCount, cavg, xeCopyMaxNs / 1000,
             xeRealVbl, xeRealVbl ? xeLineSum / xeRealVbl : 0, xeLineMax, xeTimerVbl);
    IrisXeFrameTimingPublish(t);
}

// v0.64.0: frames shown through the pipe so far (the boot hand-over clears its safety marker after a few hundred)
uint64_t IrisXeDisplayPipeFrames() { return IrisXeDisplayPipe::sPipe ? IrisXeDisplayPipe::sPipe->xeFrames : 0; }
// v0.64.0: the three "ready" checks of transactionEnd (see note()), for the boot hand-over
bool IrisXeDisplayPipeReady() {
    IrisXeDisplayPipe *p = IrisXeDisplayPipe::sPipe;
    if (!p) return false;
    IOGraphicsAccelerator2 *a = XE_ACCEL(p);
    return xeBytes(p)[0x298] != 0 && a && XE_ACCEL_ON(a) && xeBytes(p)[0x282] == 0;
}

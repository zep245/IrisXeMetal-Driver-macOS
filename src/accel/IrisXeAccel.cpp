// IrisXeAccel - milestone 1 step 1c: IrisXeAccelerator starts on the Iris Xe.
//
// Apple's IOGraphicsAccelerator2::start() does all the generic setup and calls
// back into us in this order (found by disassembling it, research round 5):
//   configureDevice -> populateAccelConfig -> newEventMachine (+ its init)
//   -> newDisplayMachine (+ its init) -> createKernelGPUTask
// Every callback records itself in the property IrisXe-AccelStart (on us and on
// the PCI device, so it survives even if start fails), so one ioreg line tells
// how far Apple's start got.
#include "IrisXeAccel.hpp"
#include "IrisXeVideo.hpp"                    // v0.68.0
static IrisXeAccelerator *sVcsAccel;                 // v0.67.0: debug.irisxe_vcstest
#include "../IrisXeSpaceABI.h"
#include "../IrisXeDrawABI.h"         // v0.49.0: coreDraws
#include <IOKit/IORangeAllocator.h>
#include "IrisXeDisplayPipe.hpp"   // v0.56.0
#include <sys/sysctl.h>                // v0.51.42: debug.irisxe_dpipe
#include <sys/proc.h>                  // v0.63.2: proc_selfname
void IrisXeRegisterSysctl();
void IrisXeGPUDesktopBoot(IrisXeAccelerator *accel);
#include <IOKit/IODeviceTreeSupport.h>
// IOGraphicsInterfaceTypes.h (clashes with our IOAccelerator headers)
#define kIOAccelTypesKey    "IOAccelTypes"
#define kIOAccelIndexKey    "IOAccelIndex"
#define kIOAccelRevisionKey "IOAccelRevision"
#include <libkern/libkern.h>
#include <string.h>

OSDefineMetaClassAndStructors(IrisXeAccelerator, IOGraphicsAccelerator2)

IrisXeAccelerator *IrisXeAccelerator::sInstance = nullptr;

// IOAccelTask is a concrete Apple class; its init is exported, call it directly.
// Apple's "accelerator is ready for work" switch (exported). Every resource
// request (Metal buffer) first waits in acceleratorWaitEnabled() until it is
// set - in v0.19.0 we never set it, so the first buffer waited forever.
extern "C" void _ZN22IOGraphicsAccelerator217enableAcceleratorEv(IOGraphicsAccelerator2 *self);

extern "C" bool _ZN11IOAccelTask4initEP22IOGraphicsAccelerator2jPP16IORangeAllocator(
    OSObject *self, IOGraphicsAccelerator2 *accel, unsigned int count, IORangeAllocator **ranges);

static const UInt64 kGPUVASize = IRISXE_LO_END;     // v0.58.7: -irisxelowva or an older IrisXeCore; else IRISXE_HI_VA..IRISXE_HI_END    // v0.49.2: 512 MB per task = what IrisXeCore can map (was 16 GB: Maps got addresses over 256 MB -> grey tiles)
static const UInt32 kStampBytes = 0x1000;          // one page of event stamps (GPU writes them later)

// ---------------------------------------------------------------- progress

void IrisXeAccelerator::note(const char *step, bool ok) {
    // v0.38.0: a step already noted with the same result is not added again (each program opening the
    // driver made another "userTask:ok", without end)
    char item[48];
    snprintf(item, sizeof(item), "%s:%s", step, ok ? "ok" : "FAIL");
    size_t il = strlen(item);                        // (strstr / strnstr are not exported to kexts)
    for (const char *p = fLog; *p; p++) if (!strncmp(p, item, il)) return;
    size_t n = strlen(fLog);
    snprintf(fLog + n, sizeof(fLog) - n, "%s%s:%s", n ? " " : "", step, ok ? "ok" : "FAIL");
    IOLog("IrisXeAccel: %s %s\n", step, ok ? "ok" : "FAILED");
    OSString *s = OSString::withCString(fLog);
    if (s) {
        setProperty("IrisXe-AccelStart", s);
        if (fProvider) fProvider->setProperty("IrisXe-AccelStart", s);
        s->release();
    }
}

// ---------------------------------------------------------------- IOService

IOService *IrisXeAccelerator::probe(IOService *provider, SInt32 *score) {
    if (!PE_parse_boot_argn("-irisxeaccel", nullptr, 0)) return nullptr;
    if (PE_parse_boot_argn("-irisxeoff", nullptr, 0)) return nullptr;
    return IOGraphicsAccelerator2::probe(provider, score);
}

bool IrisXeAccelerator::start(IOService *provider) {
    fProvider = provider;
    fPCI = nullptr;
    fStampMem = nullptr;
    fStampVA = nullptr;
    fEM = nullptr;
    fTasks = 0;
    fLog[0] = 0;
    for (int i = 0; i < 4; i++) fMemCount[i] = 0;
    for (int i = 0; i < 12; i++) fMetal[i] = 0;
    fCore = nullptr;
    fSymMap = OSSymbol::withCString("IrisXeMapPages");
    fSymUnmap = OSSymbol::withCString("IrisXeUnmapPages");
    fSymBlit = OSSymbol::withCString("IrisXeRunBlit");
    fSymCheck = OSSymbol::withCString("IrisXeCheckMapped");
    fSymSpace = OSSymbol::withCString("IrisXeSpace");
    fSymScan = OSSymbol::withCString("IrisXeScanout");
    fSpaceABI = -1;
    fHighVA = false;
    sInstance = this;
    if (provider) provider->removeProperty("IrisXe-AccelStart");
    note("start", true);

    // Apple's generic accelerator start (calls the methods below)
    bool ok = IOGraphicsAccelerator2::start(provider);
    note("AppleStart", ok);
    if (!ok) {
        // Keep the object (unregistered, idle) instead of failing: Apple's
        // half-done setup is then never torn down, and the result stays
        // visible in ioreg on us too.
        return true;
    }

    // ready for work (resource / buffer requests wait for this)
    _ZN22IOGraphicsAccelerator217enableAcceleratorEv(this);
    note("enabled", true);

    // step 2a: tell Metal which user-space driver belongs to this GPU (only
    // with -irisxemetal; without it Metal never tries to load our bundle)
    if (PE_parse_boot_argn("-irisxemetal", nullptr, 0)) {
        setProperty("MetalPluginName", "IrisXeMTLDriver");
        setProperty("MetalPluginClassName", "IrisXeMTLDevice");
        note("metalPlugin", true);
    }

    // v0.68.22: apps on the GPU for sandboxed apps. They cannot read /Library/Preferences/irisxe-apps (the apps test:
    // TextEdit, Preview, Maps, Photos, Calculator all "device hidden"), but every Metal client can read our registry
    // entry. NVRAM irisxe-apps (program names, separated by spaces or commas; "*" = every app) -> property
    // IrisXe-Apps. v0.68.23: + NVRAM irisxe-noapps -> IrisXe-NoApps (kept on the CPU, wins over IrisXe-Apps). Only
    // published when IrisXeCore is there: without it no GPU job can run, and an app given our device would draw
    // nothing (24 Nov 2026: the black screen when IrisXeCore's binary was missing).
    if (findCore()) {
        static const char *keys[2][2] = { { "irisxe-apps", "IrisXe-Apps" }, { "irisxe-noapps", "IrisXe-NoApps" } };
        for (int k = 0; k < 2; k++) {
            IORegistryEntry *opt = IORegistryEntry::fromPath("/options", gIODTPlane);
            if (!opt) break;
            OSObject *o = opt->copyProperty(keys[k][0]);
            char buf[512] = {0};
            if (OSData *d = OSDynamicCast(OSData, o)) {
                unsigned n = d->getLength() < sizeof(buf) - 1 ? d->getLength() : sizeof(buf) - 1;
                memcpy(buf, d->getBytesNoCopy(), n);
            } else if (OSString *st = OSDynamicCast(OSString, o)) {
                strlcpy(buf, st->getCStringNoCopy(), sizeof(buf));
            }
            for (char *c = buf; *c; c++) if (*c == ',') *c = ' ';
            if (buf[0]) setProperty(keys[k][1], buf);
            OSSafeReleaseNULL(o);
            opt->release();
        }
    } else {
        setProperty("IrisXe-Apps-Off", "IrisXeCore is not running: apps stay on the CPU");
    }

    if (PE_parse_boot_argn("-irisxeaccelreg", nullptr, 0)) {
        registerService();
        note("registered", true);
    }
    // v0.40.0: tell WindowServer that the display (IrisXeFB) is driven by this accelerator, so that
    // MTLCreateSystemDefaultDevice (the main display's GPU) returns our device. Experimental: WindowServer
    // then also tries to use us. Opt-in for ONE boot: `sudo nvram irisxe-display=1` (removed again here, so
    // the next boot is normal), or every boot with the boot-arg -irisxedisplay.
    {
        bool once = false;
        IORegistryEntry *opt = IORegistryEntry::fromPath("/options", gIODTPlane);
        if (opt) {
            if (opt->getProperty("irisxe-display")) {
            once = true;
            opt->removeProperty("irisxe-display");
            // v0.55.1: write the removal to the NVRAM chip now; without this a forced power-off kept the
            // flag and every following boot was a test boot again (same request as `nvram -s`)
            opt->setProperty("IONVRAM-FORCESYNCNOW-PROPERTY", "IONVRAM-FORCESYNCNOW-PROPERTY");
        }
            opt->release();
        }
        // v0.64.1: also from GPU test step 2 (IrisXeGPUDesktopBoot, from its own thread)
        if (once || PE_parse_boot_argn("-irisxedisplay", nullptr, 0)) linkDisplays(once ? "nvram irisxe-display (this boot only)" : "boot-arg -irisxedisplay");
        else setProperty("IrisXe-DisplayLink", "off (sudo nvram irisxe-display=1 + reboot: linked for one boot)");
    }
    sVcsAccel = this;                             // v0.67.0
    IrisXeVideoTestStart(this);                   // v0.68.0: one-boot video test (nvram irisxe-videotest)
    IrisXeRegisterSysctl();                       // v0.51.42: debug.irisxe_dpipe
    setProperty("IrisXe-AccelVersion", KEXT_VERSION);
    IrisXeGPUDesktopBoot(this);                   // v0.64.0: GPU-drawn desktop from boot
    return true;
}

// v0.40.0: IOAccelFindAccelerator (CoreGraphics / WindowServer) reads the framebuffer's IOAccelTypes (the
// accelerator's registry path) and IOAccelIndex
// v0.51.40: IrisXeFB usually starts after the accelerator ("0 framebuffer(s)" in v0.51.39), so a notification
// links every IrisXeFB whenever it appears (v0.51.42: only links; the display pipe is a separate step, see below).
extern void IrisXeDisplayPipeStep(const char *step);
extern int  IrisXeDisplayPipeAttach(int v);
static bool fbAppeared(void *target, void *, IOService *fb, IONotifier *) {
    if (target && fb) ((IrisXeAccelerator *)target)->linkOne(fb);
    return true;
}
void IrisXeAccelerator::linkOne(IOService *fb) {
    fb->setProperty(kIOAccelTypesKey, fLinkPath);
    fb->setProperty(kIOAccelIndexKey, (unsigned long long)fLinked, 32);
    fb->setProperty(kIOAccelRevisionKey, 2ULL, 32);
    fLinked++;
    char msg[700];
    snprintf(msg, sizeof(msg), "%s: %u framebuffer(s) -> %s", fLinkWhy, fLinked, fLinkPath);
    setProperty("IrisXe-DisplayLink", msg);
    IOLog("IrisXeAccel: display link %s\n", msg);
}
void IrisXeAccelerator::linkDisplays(const char *why) {
    int len = sizeof(fLinkPath);
    if (!getPath(fLinkPath, &len, gIOServicePlane)) { setProperty("IrisXe-DisplayLink", "failed: no registry path"); return; }
    strlcpy(fLinkWhy, why, sizeof(fLinkWhy));
    fLinked = 0;
    setProperty(kIOAccelIndexKey, 0ULL, 32);
    setProperty(kIOAccelRevisionKey, 2ULL, 32);
    char msg[700];
    snprintf(msg, sizeof(msg), "%s: waiting for IrisXeFB -> %s", why, fLinkPath);
    setProperty("IrisXe-DisplayLink", msg);
    OSDictionary *m = IOService::serviceMatching("IrisXeFB");
    if (!m) return;
    // fires for an IrisXeFB that is already there and for one that starts later
    fFBNotifier = IOService::addMatchingNotification(gIOFirstPublishNotification, m, fbAppeared, this, nullptr, 0);
    m->release();
}

// v0.51.42: Apple's display pipe object (step 2 of the display-pipe hand-over)
// v0.56.0: our own display pipe (IrisXeDisplayPipe: shows IOPresentment's screen images on the panel). Boot-arg
// -irisxeapplepipe keeps Apple's base class (which cannot show them: "transaction submission failed").
IRISXE_SLOT_RET IrisXeAccelerator::newDisplayPipe() {
    if (!PE_parse_boot_argn("-irisxeapplepipe", nullptr, 0) && IrisXeDisplayPipe::sizeOK()) {
        IrisXeDisplayPipe *p = OSTypeAlloc(IrisXeDisplayPipe);
        if (p) {
            IrisXeDisplayPipeStep("2/3 our display pipe object made (IrisXeDisplayPipe), IOAccelDisplayPipe::init next");
            return (IRISXE_SLOT_RET)p;
        }
    }
    IrisXeDisplayPipeStep("2/3 Apple's display pipe object made, IOAccelDisplayPipe::init next (framebuffer notification, resource)");
    return IOGraphicsAccelerator2::newDisplayPipe();
}

// v0.51.42: sudo sysctl debug.irisxe_dpipe=1 - hand IrisXeFB to Apple's display machine (display pipe test)
static int sysctlDpipe SYSCTL_HANDLER_ARGS {
    int v = 0;
    int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr) return err;
    return (v >= 1 && v <= 4) ? IrisXeDisplayPipeAttach(v) : 0;
}
SYSCTL_PROC(_debug, OID_AUTO, irisxe_dpipe, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, sysctlDpipe, "I",
            "1: display pipe, 2: + capabilities with transactions, 3: + capabilities without, 4: remove capabilities");
// v0.59.0: sudo sysctl debug.irisxe_scanout=N - how the display pipe shows WindowServer's images
//   0: CPU copy into the panel's framebuffer (as v0.56-0.58), 1: direct scanout (default), 2: direct, no cache flush
// Boot-arg -irisxecopy starts at 0. Takes effect at the next frame.
int gIrisXeScanMode = -1;
int IrisXeScanMode() {
    if (gIrisXeScanMode < 0) gIrisXeScanMode = PE_parse_boot_argn("-irisxecopy", nullptr, 0) ? 0 : 1;
    return gIrisXeScanMode;
}
static int sysctlScanout SYSCTL_HANDLER_ARGS {
    int v = IrisXeScanMode();
    int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr) return err;
    if (v < 0 || v > 2) return EINVAL;
    gIrisXeScanMode = v;
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, irisxe_scanout, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, sysctlScanout, "I",
            "0: CPU copy, 1: direct scanout, 2: direct scanout without cache flush");
// v0.63.0: sudo sysctl debug.irisxe_async=1 - asynchronous command buffers: IrisXeAccel returns before the GPU has
// finished a command buffer's last draw job and completes it later through Apple's event stamps (WindowServer can
// prepare the next frame meanwhile). 0 = as before (default; boot-arg -irisxeasync starts at 1). macOS 15 only: the
// queue / event layout was read from macOS 15's IOAcceleratorFamily2. Takes effect at the next command buffer.
#include <libkern/version.h>
#include <kern/clock.h>                // v0.68.33: absolutetime_to_nanoseconds
int gIrisXeAsync = -1;
int IrisXeAsyncMode() {
    // v0.64.1: off by default again while the GPU desktop from boot is tested step by step (boot-arg -irisxeasync = 1)
    // v0.68.33: on by default again (speed test: Maps 26 -> 48 fps, frames on time 30% -> 70%, no GPU timeouts), but
    // only from 90 s after boot - the boot / login path stays exactly as tested. Boot-arg -irisxenoasync: never;
    // -irisxeasync: from the start. sysctl debug.irisxe_async still switches it live. macOS 15 only (layout check).
    if (gIrisXeAsync >= 0) return gIrisXeAsync;
    if (version_major != 24 || PE_parse_boot_argn("-irisxenoasync", nullptr, 0)) { gIrisXeAsync = 0; return 0; }
    if (PE_parse_boot_argn("-irisxeasync", nullptr, 0)) { gIrisXeAsync = 1; return 1; }
    uint64_t now = mach_absolute_time(), ns = 0;
    absolutetime_to_nanoseconds(now, &ns);
    if (ns < 90ULL * 1000000000ULL) return 0;    // still booting: synchronous, decided again later
    gIrisXeAsync = 1;
    return 1;
}
static int sysctlAsync SYSCTL_HANDLER_ARGS {
    int v = IrisXeAsyncMode();
    int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr) return err;
    if (v < 0 || v > 1) return EINVAL;
    if (v && version_major != 24) return ENOTSUP;
    gIrisXeAsync = v;
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, irisxe_async, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, sysctlAsync, "I",
            "1: command buffers complete asynchronously (macOS 15), 0: wait for the GPU in the queue");
// v0.63.1: sudo sysctl debug.irisxe_flush=1 - between the draws of a GPU job, flush the GPU's caches only when a draw
// reads what an earlier draw of the job wrote (or writes what it read, or clears); 0 = after every draw (default).
// sudo sysctl debug.irisxe_batch=N - up to N draws (1..8) per GPU job; 0 = IrisXeCore's default (4).
// Boot-args -irisxeflush / irisxebatch=N start with them.
int gIrisXeFlush = -1, gIrisXeBatch = -1;
int gIrisXePSTest = 0;
UInt32 IrisXeDrawFlags() {
    // v0.68.42: smart flushing on by default from 90 s after boot (with the GPU worker the GPU is the limit: Maps'
    // draws cost ~25% less GPU time with it, v0.68.39 test). Boot-arg -irisxenoflush: never; -irisxeflush: from boot.
    if (gIrisXeFlush < 0) {
        if (PE_parse_boot_argn("-irisxenoflush", nullptr, 0)) gIrisXeFlush = 0;
        else if (PE_parse_boot_argn("-irisxeflush", nullptr, 0)) gIrisXeFlush = 1;
        else {
            uint64_t ns = 0;
            absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
            if (ns >= 90ULL * 1000000000ULL) gIrisXeFlush = 1;
        }
    }
    // v0.68.35: 6 draws per GPU job by default (batch test after the ring fix: no hangs at 5..8, Maps 56 fps / 86%
    // on time at 6) - like background completion, only from 90 s after boot; boot-arg irisxebatch=N sets it
    UInt32 batchNow = 0;
    if (gIrisXeBatch < 0) {
        int b = 0;
        if (PE_parse_boot_argn("irisxebatch", &b, sizeof(b))) gIrisXeBatch = (b < 0 || b > 8) ? 0 : b;
        else {
            uint64_t ns = 0;
            absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
            if (ns >= 90ULL * 1000000000ULL) gIrisXeBatch = 6;
        }
    }
    batchNow = gIrisXeBatch < 0 ? 0 : (UInt32)gIrisXeBatch;
    UInt32 f = (gIrisXeFlush > 0 ? 2u : 0u) | (batchNow << 8);
    if (gIrisXePSTest > 0) {                                  // v0.63.2: only for the test program's own draws
        char nm[32] = {};
        proc_selfname(nm, sizeof(nm));
        if (!strcmp(nm, "irisxe-metaltest")) f |= (UInt32)gIrisXePSTest << 2;
    }
    return f;
}
// v0.63.2: sudo sysctl debug.irisxe_pstest=N - irisxe-metaltest's draws use IrisXeCore's own constant-colour pixel
// shader: 1 = SIMD8 (8 pixels per thread, like our compiler's), 2 = SIMD16 (16 pixels per thread); 3 / 4 = our
// shaders, render targets with the LLC / the L3 cache bypassed (MOCS 61 / 60); 0 = off
static int sysctlPSTest SYSCTL_HANDLER_ARGS {
    int v = gIrisXePSTest;
    int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr) return err;
    if (v < 0 || v > 4) return EINVAL;
    gIrisXePSTest = v;
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, irisxe_pstest, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, sysctlPSTest, "I",
            "fill-rate test for irisxe-metaltest: 1 = SIMD8 test shader, 2 = SIMD16, 3 = render targets LLC bypassed, 4 = L3 bypassed");
static int sysctlFlush SYSCTL_HANDLER_ARGS {
    IrisXeDrawFlags();
    int v = gIrisXeFlush < 0 ? 0 : gIrisXeFlush;
    int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr) return err;
    if (v < 0 || v > 1) return EINVAL;
    gIrisXeFlush = v;
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, irisxe_flush, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, sysctlFlush, "I",
            "1: flush between draws only when needed, 0: after every draw");
static int sysctlBatch SYSCTL_HANDLER_ARGS {
    IrisXeDrawFlags();
    int v = gIrisXeBatch < 0 ? 0 : gIrisXeBatch;
    int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr) return err;
    if (v < 0 || v > 8) return EINVAL;
    gIrisXeBatch = v;
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, irisxe_batch, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, sysctlBatch, "I",
            "draws per GPU job (1..8), 0: IrisXeCore's default (4); 6 by default from 90 s after boot (v0.68.35)");
// v0.68.40: sudo sysctl debug.irisxe_worker=0/1 - the GPU worker (draws run by a kernel thread, the program does not
// wait); boot-arg -irisxenoworker starts at 0
extern int gIrisXeWorker;                    // IrisXeAccelMachines.cpp
static int sysctlWorker SYSCTL_HANDLER_ARGS {
    int v = gIrisXeWorker < 0 ? (PE_parse_boot_argn("-irisxenoworker", nullptr, 0) ? 0 : 1) : gIrisXeWorker;
    int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr) return err;
    if (v < 0 || v > 1) return EINVAL;
    gIrisXeWorker = v;
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, irisxe_worker, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, sysctlWorker, "I",
            "1: a kernel thread runs the draws (programs do not wait for the GPU), 0: each program runs its own");
extern void IrisXeKernelTimingReset();       // IrisXeAccelMachines.cpp
// v0.59.1: sudo sysctl debug.irisxe_timing=1 - the stutter meter (IrisXe-FrameTiming) starts from zero
static int sysctlTiming SYSCTL_HANDLER_ARGS {
    int v = 0;
    int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr) return err;
    if (v == 1 && IrisXeDisplayPipe::sPipe) IrisXeDisplayPipe::sPipe->timingReset();
    if (v == 1) IrisXeKernelTimingReset();       // v0.60.0
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, irisxe_timing, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, sysctlTiming, "I",
            "1: reset the frame timing (IrisXe-FrameTiming)");
// v0.67.0: sudo sysctl debug.irisxe_vcstest=1 - one test job on the video engine (VCS0), report in IrisXe-VCS
// (IrisXeCore) and IrisXe-VCSTest here; reading gives the last result (0 ran, -3 no run, -4 no VDBox 0, -2 old core)
static int gIrisXeVcsResult = 1;
static int sysctlVcsTest SYSCTL_HANDLER_ARGS {
    int v = gIrisXeVcsResult;
    int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr) return err;
    if (v != 1) return EINVAL;
    if (!sVcsAccel) return ENODEV;
    static char rep[512];
    rep[0] = 0;
    gIrisXeVcsResult = sVcsAccel->coreVcsTest(rep, sizeof(rep));
    sVcsAccel->setProperty("IrisXe-VCSTest", rep[0] ? rep : "no report (IrisXeCore older than v0.67.0?)");
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, irisxe_vcstest, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, sysctlVcsTest, "I",
            "1: run one test job on the video engine VCS0 (report in IrisXe-VCSTest)");
static bool sSysctlOn;
void IrisXeRegisterSysctl() {
    if (sSysctlOn) return;
    sysctl_register_oid(&sysctl__debug_irisxe_dpipe);
    sysctl_register_oid(&sysctl__debug_irisxe_scanout);
    sysctl_register_oid(&sysctl__debug_irisxe_timing);
    sysctl_register_oid(&sysctl__debug_irisxe_async);       // v0.63.0
    sysctl_register_oid(&sysctl__debug_irisxe_flush);       // v0.63.1
    sysctl_register_oid(&sysctl__debug_irisxe_batch);
    sysctl_register_oid(&sysctl__debug_irisxe_worker);
    sysctl_register_oid(&sysctl__debug_irisxe_pstest);      // v0.63.2
    sysctl_register_oid(&sysctl__debug_irisxe_vcstest);     // v0.67.0
    sSysctlOn = true;
}

// ---------------------------------------------------------------- device

IRISXE_SLOT_RET IrisXeAccelerator::configureDevice(IOPCIDevice *pci) {
    if (!pci) { note("configureDevice", false); return 0; }
    fPCI = pci;
    fPCI->retain();
    // IrisXeCore already enabled these; make sure (Apple's drivers do the same)
    fPCI->setMemoryEnable(true);
    fPCI->setBusMasterEnable(true);

    // event stamp page: physically contiguous, shared with user space later
    fStampMem = IOBufferMemoryDescriptor::withOptions(
        kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMemoryKernelUserShared,
        kStampBytes, page_size);
    if (!fStampMem || fStampMem->prepare() != kIOReturnSuccess) {
        if (fStampMem) { fStampMem->release(); fStampMem = nullptr; }
        note("configureDevice", false);
        return 0;
    }
    fStampVA = (volatile UInt32 *)fStampMem->getBytesNoCopy();
    bzero((void *)fStampVA, kStampBytes);
    note("configureDevice", true);
    return 1;
}

IRISXE_SLOT_RET IrisXeAccelerator::teardownDevice(IOPCIDevice *) {
    if (fStampMem) { fStampMem->complete(); fStampMem->release(); fStampMem = nullptr; }
    fStampVA = nullptr;
    if (fCore) { fCore->release(); fCore = nullptr; }
    if (fPCI) { fPCI->release(); fPCI = nullptr; }
    return 0;
}

// Same values as Apple's paravirtual GPU (AppleParavirtAccelerator::
// populateAccelConfig), only the name is ours. Offsets are into Apple's
// IOAccelConfig (it lives inside the accelerator, already filled with
// Apple's defaults by initializeConfigStructure).
IRISXE_SLOT_RET IrisXeAccelerator::populateAccelConfig(IOAccelConfig *cfgp) {
    UInt8 *c = (UInt8 *)cfgp;
    if (!c) { note("populateAccelConfig", false); return 0; }
    *(const char **)(c + 0x00) = "Intel Iris Xe Graphics (IrisXe)";
    *(UInt32 *)(c + 0x08) = 0x480000;
    *(UInt64 *)(c + 0x0c) = 7;
    *(UInt64 *)(c + 0x14) = 0x2000000000ULL;     // [0x18] = 32 event stamps
    *(UInt64 *)(c + 0x20) = 0x40000000ULL;
    *(UInt64 *)(c + 0x28) = 0x100000000008ULL;
    *(UInt32 *)(c + 0x30) = 0x40004000;
    *(UInt8  *)(c + 0x47) = 1;
    *(UInt32 *)(c + 0x5c) = 4;
    *(UInt32 *)(c + 0x64) = 2;
    *(UInt64 *)(c + 0x68) = 0x10000000aULL;      // [0x68] = 10 (progress timer, s)
    *(UInt64 *)(c + 0x88) = 0;
    // v0.68.5: on the video test boot, resources get a client-shared RO private part of 0x28 bytes, as on Apple's
    // Intel accelerator (IntelAccelerator::populateAccelConfig: [0x58] = 0x28; the family sizes every resource's RO
    // block 0x108 + [0x58] and IOAccelResourceGetClientSharedPrivate returns NULL when it is 0 - research round 44)
    if (IrisXeVideoTestPending()) {
        // v0.68.12: config flag 0x40000 (byte +0xa bit 2), set on Apple's Intel accelerator (flags | 0x4f240140):
        // IOAccelContext2::process_token_BindDataBuffer only maps a bound command buffer for the kernel (stream info
        // +0x08 base, +0x10 size) when it is set - without it every video job reached us with no command buffer
        *(UInt32 *)(c + 0x08) |= 0x40000;
        *(UInt32 *)(c + 0x58) = 0x28;
        IrisXeVideoSetROPrivate(0x28);
        setProperty("IrisXe-ROPrivate", 0x28ULL, 32);
    }
    note("populateAccelConfig", true);
    return 0;
}

IRISXE_SLOT_RET IrisXeAccelerator::getStampMemory(unsigned int *idx) {
    if (idx) *idx = 0;
    return (IRISXE_SLOT_RET)fStampMem;
}

// ---------------------------------------------------------------- helpers

IRISXE_SLOT_RET IrisXeAccelerator::newEventMachine() {
    IrisXeEventMachine *em = new IrisXeEventMachine;     // Apple calls its init
    note("newEventMachine", em != nullptr);
    fEM = em;                                            // v0.63.0: asynchronous command buffers
    return (IRISXE_SLOT_RET)em;
}

IRISXE_SLOT_RET IrisXeAccelerator::newDisplayMachine() {
    IrisXeDisplayMachine *dm = new IrisXeDisplayMachine; // Apple calls its init
    note("newDisplayMachine", dm != nullptr);
    return (IRISXE_SLOT_RET)dm;
}

// A GPU task = one GPU address space. Apple's own IOAccelTask, with a range
// allocator for the 16 GB address space (page 0 kept unused).
OSObject *IrisXeAccelerator::makeTask(const char *who) {
    // v0.58.7: GPU VA 1 GB .. 4 GB - 64 KB (3 GB) when IrisXeCore can map it (ABI 9); was 4 KB .. 512 MB, and
    // WindowServer ran out of addresses: newBuffer(14.7 MB) failed, then it aborted (window capture).
    // -irisxelowva: the old range.
    bool hi = !PE_parse_boot_argn("-irisxelowva", nullptr, 0) && coreABI() >= 9;
    IORangeAllocator *ra = IORangeAllocator::withRange(hi ? IRISXE_HI_END - 1 : kGPUVASize - 1, 0x1000, 0x1000, 0);
    if (!ra) { note(who, false); return nullptr; }
    if (hi) ra->allocateRange(0, IRISXE_HI_VA);    // below 1 GB: IrisXeCore's own pages (draw state, screen)
    else    ra->allocateRange(0, 0x1000);
    if (hi) fHighVA = true;
    OSObject *t = OSMetaClass::allocClassWithName("IOAccelTask");
    bool ok = t && _ZN11IOAccelTask4initEP22IOGraphicsAccelerator2jPP16IORangeAllocator(t, this, 1, &ra);
    ra->release();                     // the task keeps its own reference
    if (!ok && t) { t->release(); t = nullptr; }
    if (ok) fTasks++;
    note(who, ok);
    return t;
}

IRISXE_SLOT_RET IrisXeAccelerator::createKernelGPUTask() { return (IRISXE_SLOT_RET)makeTask("kernelTask"); }
IRISXE_SLOT_RET IrisXeAccelerator::createUserGPUTask()   { return (IRISXE_SLOT_RET)makeTask("userTask"); }

// Not provided yet (Apple's paravirt GPU also has no GL/CL/video/surface):
// returning NULL makes Apple refuse that kind of client cleanly.
IRISXE_SLOT_RET IrisXeAccelerator::newGLContext()    { return 0; }
IRISXE_SLOT_RET IrisXeAccelerator::newCLContext()    { return 0; }
IRISXE_SLOT_RET IrisXeAccelerator::newSurface()      { return 0; }
IRISXE_SLOT_RET IrisXeAccelerator::new2DContext()    { return 0; }
IRISXE_SLOT_RET IrisXeAccelerator::newVideoContext() { return 0; }
// v0.68.0: Apple's Intel accelerators make their video contexts here (IntelAccelerator::newContext, research round 36):
// 0x100 = Main (3D engine), 0x101 = Media (video engine), 0x102 = VEBox. Only on the one-boot video test, and only
// record-only IrisXeVideoContexts (IrisXeVideo.cpp).
// v0.68.2: on the video test boot every client's device connection is an IrisXeDevice (the family's device plus
// Apple's Intel selectors 10..24, IrisXeVideo.cpp); otherwise the family's own
IRISXE_SLOT_RET IrisXeAccelerator::newDevice() {
    IRISXE_SLOT_RET d = IrisXeVideoNewDevice();
    return d ? d : IOGraphicsAccelerator2::newDevice();
}
IRISXE_SLOT_RET IrisXeAccelerator::newContext(unsigned int type) {
    if (type >= 0x100 && type <= 0x102) {
        // v0.68.1: AppleGVA asks for a Main context first (v0.68.0 refused it: no hardware decoder offered at all),
        // so on the test boot all three are record-only contexts
        IrisXeVideoRecordNote(IrisXeVideoTestOn() ? "context" : "context refused", type);
        if (!IrisXeVideoTestOn()) return 0;
        IrisXeVideoContext *c = new IrisXeVideoContext;
        if (c) c->xeType = type;
        return (IRISXE_SLOT_RET)c;
    }
    return IOGraphicsAccelerator2::newContext(type);
}

// ---- milestone 2c: the GPU core (IrisXeCore's IrisXeProbe on the same PCI
// device). No link dependency between the kexts: we find the service and call
// it through callPlatformFunction.
IOService *IrisXeAccelerator::findCore() {
    if (fCore) return fCore;
    if (!fPCI) return nullptr;
    OSIterator *it = fPCI->getChildIterator(gIOServicePlane);
    if (!it) return nullptr;
    while (OSObject *o = it->getNextObject()) {
        IOService *s = OSDynamicCast(IOService, o);
        if (s && !strcmp(s->getMetaClass()->getClassName(), "IrisXeProbe")) { fCore = s; fCore->retain(); break; }
    }
    it->release();
    return fCore;
}

static IOReturn spaceCall(IOService *c, const OSSymbol *sym, UInt32 op, XeSpaceArgs *a) {
    return c && sym ? c->callPlatformFunction(sym, false, (void *)(uintptr_t)op, a, nullptr, nullptr) : kIOReturnNotReady;
}

bool IrisXeAccelerator::coreMapPages(UInt32 space, UInt64 va, const UInt64 *pa, UInt32 count) {
    IOService *c = findCore();
    if (!c) return false;
    if (space) {
        XeSpaceArgs a = {}; a.space = space; a.va = va; a.pa = pa; a.count = count;
        return spaceCall(c, fSymSpace, kXeSpaceMap, &a) == kIOReturnSuccess;
    }
    return c->callPlatformFunction(fSymMap, false, (void *)(uintptr_t)va, (void *)pa,
                                   (void *)(uintptr_t)count, nullptr) == kIOReturnSuccess;
}
void IrisXeAccelerator::coreUnmapPages(UInt32 space, UInt64 va, UInt32 count) {
    IOService *c = findCore();
    if (!c) return;
    if (space) {
        XeSpaceArgs a = {}; a.space = space; a.va = va; a.count = count;
        spaceCall(c, fSymSpace, kXeSpaceUnmap, &a);
        return;
    }
    c->callPlatformFunction(fSymUnmap, false, (void *)(uintptr_t)va, (void *)(uintptr_t)count, nullptr, nullptr);
}
int IrisXeAccelerator::coreRunBlit(UInt32 space, const UInt32 *cmds, UInt32 count, UInt32 timeoutMS) {
    IOService *c = findCore();
    if (!c) return -10;                                   // IrisXeCore GPU core not running (-irisxegpu?)
    if (space) {
        XeSpaceArgs a = {}; a.space = space; a.cmds = cmds; a.count = count; a.timeoutMS = timeoutMS; a.result = -11;
        spaceCall(c, fSymSpace, kXeSpaceRun, &a);
        return a.result;
    }
    int r = -11;
    c->callPlatformFunction(fSymBlit, false, (void *)cmds, (void *)(uintptr_t)count, (void *)(uintptr_t)timeoutMS, &r);
    return r;
}

bool IrisXeAccelerator::coreIsMapped(UInt32 space, UInt64 va, UInt64 len) {
    IOService *c = findCore();
    if (!c) return false;
    if (space) {
        XeSpaceArgs a = {}; a.space = space; a.va = va; a.len = len;
        return spaceCall(c, fSymSpace, kXeSpaceIsMapped, &a) == kIOReturnSuccess;
    }
    return c->callPlatformFunction(fSymCheck, false, (void *)(uintptr_t)va, (void *)(uintptr_t)len,
                                   nullptr, nullptr) == kIOReturnSuccess;
}

int IrisXeAccelerator::coreCompute(UInt32 space, const UInt32 *kernel, UInt32 dwords, const UInt32 ctd[8],
                                   const UInt32 groups[3], UInt32 tpg, UInt32 timeoutMS, UInt32 info,
                                   UInt64 codeVA, UInt32 ntex, const XeTexDesc *tex,
                                   UInt32 nsamp, const XeSampDesc *samp, const UInt32 *tpgHD, const UInt32 *gridYZ) {
    IOService *c = findCore();
    if (!c) return -10;
    XeSpaceArgs a = {};
    a.space = space; a.cmds = kernel; a.count = dwords; a.timeoutMS = timeoutMS; a.tpg = tpg; a.result = -12;
    a.len = info;                                        // ABI 3: threadgroup memory bytes | barrier
    a.va = codeVA;                                       // ABI 4: kernel in the process's GPU memory
    for (int i = 0; i < 8; i++) a.ctd[i] = ctd[i];
    for (int i = 0; i < 3; i++) a.groups[i] = groups[i];
    if (fSpaceABI < 2) return -12;                       // IrisXeCore older than 0.24.0
    if (info && fSpaceABI < 3) return -13;               // threadgroup memory / barriers: IrisXeCore 0.28.0+
    if (codeVA && fSpaceABI < 4) return -14;             // big kernels: IrisXeCore 0.29.0+
    if ((ntex || nsamp) && fSpaceABI < 5) return -15;    // textures: IrisXeCore 0.30.0+
    // v0.51.17: kernels from this compiler read the group size from the per-thread data (ABI 8):
    // with an older IrisXeCore they would get 0 there
    if (fSpaceABI < 8) return -17;                       // IrisXeCore older than 0.51.17: install it too
    if (tpgHD && fSpaceABI >= 8) { a.tpgHD[0] = tpgHD[0]; a.tpgHD[1] = tpgHD[1]; }
    if (gridYZ && fSpaceABI >= 8) { a.gridYZ[0] = gridYZ[0]; a.gridYZ[1] = gridYZ[1]; }
    if (ntex > kXeMaxTextures || nsamp > kXeMaxSamplers) return -7;
    a.ntex = ntex; a.nsamp = nsamp;
    for (UInt32 i = 0; i < ntex; i++) a.tex[i] = tex[i];
    for (UInt32 i = 0; i < nsamp; i++) a.samp[i] = samp[i];
    spaceCall(c, fSymSpace, kXeSpaceCompute, &a);
    return a.result;
}

// v0.49.0: several draws in one GPU job (IrisXeCore 0.49.0+; older: one by one)
int IrisXeAccelerator::coreDraws(UInt32 space, const XeDrawDesc *d, UInt32 count, UInt32 timeoutMS, UInt32 *asyncSeq) {
    IOService *c = findCore();
    if (asyncSeq) *asyncSeq = 0;
    if (!c) return -10;
    // v0.63.1: debug.irisxe_flush / debug.irisxe_batch (IrisXeDrawFlags above)
    UInt32 fl = fSpaceABI >= 10 ? IrisXeDrawFlags() : 0;
    if ((asyncSeq || fl) && fSpaceABI >= 10 && count >= 1) {   // v0.63.0: the last job is left running
        XeSpaceArgs a = {};
        a.space = space; a.draw = d; a.count = count; a.timeoutMS = timeoutMS; a.result = -12; a.tpg = (asyncSeq ? 1 : 0) | fl;
        spaceCall(c, fSymSpace, kXeSpaceDraw, &a);
        if (asyncSeq) *asyncSeq = (UInt32)a.va;
        return a.result;
    }
    if (count <= 1 || fSpaceABI < 7) {
        int r = 0;
        for (UInt32 i = 0; i < count; i++) { int ri = coreDraw(space, &d[i], timeoutMS); if (ri && !r) r = ri; }
        return r;
    }
    XeSpaceArgs a = {};
    a.space = space; a.draw = d; a.count = count; a.timeoutMS = timeoutMS; a.result = -12;
    spaceCall(c, fSymSpace, kXeSpaceDraw, &a);
    return a.result;
}

// v0.63.0: wait (without holding the core's job lock) until a render job ended
int IrisXeAccelerator::coreVcsTest(char *rep, UInt32 len) {
    IOService *c = findCore();
    if (!c || fSpaceABI < 11) { strlcpy(rep, "IrisXeCore v0.67.0 or newer needed (install-core.sh, restart)", len); return -2; }
    XeSpaceArgs a = {};
    a.va = (UInt64)(uintptr_t)rep; a.len = len; a.result = -12;
    spaceCall(c, fSymSpace, kXeSpaceVcsTest, &a);
    return a.result;
}
int IrisXeAccelerator::coreWaitSeq(UInt32 seq, UInt32 timeoutMS) {
    IOService *c = findCore();
    if (!c || fSpaceABI < 10) return -2;
    XeSpaceArgs a = {};
    a.count = seq; a.timeoutMS = timeoutMS; a.result = -12;
    spaceCall(c, fSymSpace, kXeSpaceWaitSeq, &a);
    return a.result;
}

// milestone 5c: a Metal draw (IrisXeDrawABI.h), checked again by the core
int IrisXeAccelerator::coreDraw(UInt32 space, const XeDrawDesc *d, UInt32 timeoutMS) {
    IOService *c = findCore();
    if (!c) return -10;
    if (fSpaceABI < 6) return -16;                       // draws: IrisXeCore 0.32.0+
    XeSpaceArgs a = {};
    a.space = space; a.draw = d; a.timeoutMS = timeoutMS; a.result = -12;
    spaceCall(c, fSymSpace, kXeSpaceDraw, &a);
    return a.result;
}

// v0.59.0: direct scanout (IrisXeSpaceABI.h, kXeScan*); -2 = no IrisXeCore / too old
int IrisXeAccelerator::coreScan(XeScanArgs *a) {
    IOService *c = findCore();
    a->result = -2;
    if (!c || !fSymScan) return -2;
    IOReturn r = c->callPlatformFunction(fSymScan, false, a, nullptr, nullptr, nullptr);
    if (r == kIOReturnUnsupported && a->result == -2) return -2;   // IrisXeCore older than 0.59.0
    return a->result;
}

int IrisXeAccelerator::coreABI() {
    if (fSpaceABI >= 0) return fSpaceABI;
    IOService *c = findCore();
    XeSpaceArgs v = {};
    if (!c || spaceCall(c, fSymSpace, kXeSpaceVersion, &v) != kIOReturnSuccess) return 0;   // not known yet
    fSpaceABI = v.result;
    return fSpaceABI;
}

int IrisXeAccelerator::coreSpaceCreate() {
    IOService *c = findCore();
    if (!c) return -1;
    if (fSpaceABI < 0) {
        XeSpaceArgs v = {};
        fSpaceABI = spaceCall(c, fSymSpace, kXeSpaceVersion, &v) == kIOReturnSuccess ? v.result : 0;
    }
    if (fSpaceABI < 1) return 0;                          // IrisXeCore too old: shared space only
    XeSpaceArgs a = {};
    if (spaceCall(c, fSymSpace, kXeSpaceCreate, &a) != kIOReturnSuccess || !a.space) return -1;
    return (int)a.space;
}
void IrisXeAccelerator::coreSpaceDestroy(UInt32 space) {
    IOService *c = findCore();
    if (!c || !space) return;
    XeSpaceArgs a = {}; a.space = space;
    spaceCall(c, fSymSpace, kXeSpaceDestroy, &a);
}

void IrisXeAccelerator::countMetal(int what, UInt64 n) {
    if (what >= 0 && what < 12) fMetal[what] += n;
    char b[380];
    snprintf(b, sizeof(b), "pages mapped %llu (failed %llu, unmapped %llu) | GPU blits ok %llu, failed %llu, skipped (not mapped) %llu | "
             "compute ok %llu, failed %llu | draws ok %llu, failed %llu, batched jobs %llu | other commands %llu | core %s",
             fMetal[0], fMetal[1], fMetal[4], fMetal[2], fMetal[3], fMetal[6], fMetal[7], fMetal[8], fMetal[9], fMetal[10], fMetal[11], fMetal[5],
             fCore ? "found" : "not found");
    setProperty("IrisXe-AccelMetal", b);
}

IRISXE_SLOT_RET IrisXeAccelerator::newCommandQueue() {
    static UInt32 queues = 0;
    IrisXeCommandQueue *q = new IrisXeCommandQueue;
    setProperty("IrisXe-AccelQueuesCreated", ++queues, 32);
    return (IRISXE_SLOT_RET)q;
}

// ---- milestone 2a: memory objects behind a Metal buffer --------------------
// Apple's paravirtual GPU uses (research round 6):
//   system memory : its own subclass that adds nothing -> IrisXeSysMemory
//   resource      : 7 small required methods, all "nothing / 0"   -> IrisXeResource
//   memory map    : commit/release/update into the GPU page table -> IrisXeMemoryMap
//                   (answers "done" for now; real PPGTT entries come with the
//                   first GPU job that reads a buffer)
//   video memory  : allocPhysical "ok", no physical pages          -> IrisXeVidMemory
// Each is counted in IrisXe-AccelMem so ioreg shows what Metal created.
void IrisXeAccelerator::countMem(int kind) {
    if (kind >= 0 && kind < 4) fMemCount[kind]++;
    char b[128];
    snprintf(b, sizeof(b), "sysMemory %u, resource %u, memoryMap %u, vidMemory %u",
             fMemCount[0], fMemCount[1], fMemCount[2], fMemCount[3]);
    setProperty("IrisXe-AccelMem", b);
}

// Apple's IOAccelSysMemory can't be created directly (v0.19.1: its alloc gave
// NULL -> "allocIOAccelResource failed to alloc texture object"), so we use an
// empty subclass, exactly like Apple's paravirtual GPU does.
IRISXE_SLOT_RET IrisXeAccelerator::newSysMemory() { countMem(0); return (IRISXE_SLOT_RET)new IrisXeSysMemory; }
IRISXE_SLOT_RET IrisXeAccelerator::newResource()  { countMem(1); return (IRISXE_SLOT_RET)new IrisXeResource; }
IRISXE_SLOT_RET IrisXeAccelerator::newMemoryMap() { countMem(2); return (IRISXE_SLOT_RET)new IrisXeMemoryMap; }
IRISXE_SLOT_RET IrisXeAccelerator::newVidMemory() { countMem(3); return (IRISXE_SLOT_RET)new IrisXeVidMemory; }

// Integrated GPU: "VRAM" is shared system memory. Same figure the Intel
// drivers report for Iris Xe (1536 MB). Return type believed 64-bit size.
IRISXE_SLOT_RET IrisXeAccelerator::tmpTotalVRAM() { return 1536ULL << 20; }
IRISXE_SLOT_RET IrisXeAccelerator::totalTextureMemory() { return 1536ULL << 20; }

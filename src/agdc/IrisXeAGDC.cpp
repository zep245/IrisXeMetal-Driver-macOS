// IrisXeAGDC.kext - see IrisXeAGDC.hpp and the design document (Piece 1).
// Answers come from Apple's own code (research round 16):
//   attr 1     kAGDCVendorInfo     0x2C bytes  (IntelFBClientControl::doAttribute)
//   attr 0x980 kAGDCGPUCapability  0xDC bytes  (AppleIntelFramebufferController::GetGPUCapability)
//   attr 0x921 link configuration  0xB0 bytes  (AppleIntelFramebufferController::GetLinkConfig, v0.53.0):
//              IOPresentment's __GatherAGDCLogicalDeviceCapabilities reads the screen's timing from it
//              (without it: "Unable to acquire capabilities ... 0x80e")
// Every other attribute returns kIOReturnUnsupported and is listed in debug.irisxe_agdc_status.
#include "IrisXeAGDC.hpp"
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/c++/OSString.h>
#include <sys/sysctl.h>
#include <kern/thread.h>          // v0.64.0: boot thread
#include <IOKit/graphics/IOFramebuffer.h>

OSDefineMetaClassAndStructors(IrisXeFBController, IOService)
OSDefineMetaClassAndStructors(IrisXeAGDC, AppleGraphicsDeviceControl)

static const uint32_t kAttrVendorInfo    = 0x1;
static const uint32_t kAttrGPUCapability = 0x980;
static const uint32_t kAttrLinkConfig    = 0x921;   // v0.53.0: AppleIntelFramebufferController::GetLinkConfig
static const size_t   kLinkConfigSize    = 0xB0;
static const uint32_t kAttrPipeCapsExt   = 0x711;   // v0.54.0: getDisplayPipeCapabilityExtended
static const size_t   kPipeCapsExtSize   = 0x196C;
static const size_t   kVendorInfoSize    = 0x2C;
static const size_t   kGPUCapabilitySize = 0xDC;

// ---------------------------------------------------------------- the AGDC object

bool IrisXeAGDC::setup(IOService* framebuffer, IOService* gpu)
{
    fFB = framebuffer;  fFB->retain();
    fGPU = gpu;         fGPU->retain();
    fLock = IOLockAlloc();
    fAskedN = 0;
    setName("IrisXeAGDC");
    return fLock != nullptr;
}

void IrisXeAGDC::free()
{
    OSSafeReleaseNULL(fFB);
    OSSafeReleaseNULL(fGPU);
    if (fLock) { IOLockFree(fLock); fLock = nullptr; }
    super::free();
}

void IrisXeAGDC::noteAsked(uint32_t attr, IOReturn r)
{
    if (!fLock) return;
    bool first = false;
    IOLockLock(fLock);
    uint32_t i = 0;
    for (; i < fAskedN; i++) if (fAsked[i].attr == attr) break;
    if (i == fAskedN && fAskedN < 32) { fAsked[fAskedN++] = { attr, 0, 0 }; first = true; }
    if (i < fAskedN) { fAsked[i].count++; fAsked[i].last = r; }
    IOLockUnlock(fLock);
    if (first) IOLog("IrisXeAGDC: macOS asked attribute 0x%x -> 0x%x\n", attr, r);
}

void IrisXeAGDC::describeAsked(char* buf, size_t len)
{
    buf[0] = 0;
    if (!fLock) return;
    IOLockLock(fLock);
    for (uint32_t i = 0; i < fAskedN; i++) {
        char one[64];
        snprintf(one, sizeof(one), "%s0x%x x%u %s", i ? ", " : "", fAsked[i].attr, fAsked[i].count,
                 fAsked[i].last == kIOReturnSuccess ? "ok" :
                 fAsked[i].last == kIOReturnUnsupported ? "unsupported" : "error");
        strlcat(buf, one, len);
    }
    if (!fAskedN) strlcat(buf, "nothing yet", len);
    IOLockUnlock(fLock);
}

IOReturn IrisXeAGDC::vendor_doDeviceAttribute(unsigned int attr, unsigned long* in, unsigned long inSize,
                                              unsigned long* out, unsigned long* outSize,
                                              IOExternalMethodArguments* args)
{
    IOReturn r = kIOReturnUnsupported;
    uint8_t* o = (uint8_t*)out;

    if (attr == kAttrVendorInfo) {
        if (!o || !outSize || *outSize < kVendorInfoSize) r = kIOReturnBadArgument;
        else {
            bzero(o, kVendorInfoSize);
            *(uint32_t*)(o + 0x00) = 0x00010000;                    // version 1.0 (as Apple)
            strlcpy((char*)(o + 0x04), "IrisXeFBController", 32);   // Apple: "AppleIntelFramebufferController"
            *(uint16_t*)(o + 0x24) = 0x8086;                        // Apple writes 0x106B -> board-ID check; ours skips it
            *(uint32_t*)(o + 0x28) = 1;                             // type: IOPresentment accepts 1 or 2
            *outSize = kVendorInfoSize;
            r = kIOReturnSuccess;
        }
    } else if (attr == kAttrGPUCapability) {
        if (!o || !outSize || *outSize < kGPUCapabilitySize) r = kIOReturnBadArgument;
        else {
            bzero(o, kGPUCapabilitySize);
            *(uint64_t*)(o + 0x00) = 1ULL << 1;                     // port 0 present (Apple: 2 << port index)
            *(uint32_t*)(o + 0x28) = 2;                             // fixed values, as Apple
            *(uint32_t*)(o + 0x2C) = 3;
            *(uint32_t*)(o + 0x30) = 1;                             // one screen
            // kernel objects; Apple's filtered_doDeviceAttribute turns them into registry IDs
            *(uint64_t*)(o + 0x34) = (uint64_t)(uintptr_t)fGPU;
            *(uint64_t*)(o + 0x3C) = (uint64_t)(uintptr_t)fFB;
            *outSize = kGPUCapabilitySize;
            r = kIOReturnSuccess;
        }
    } else if (attr == kAttrLinkConfig) {
        r = answerLinkConfig(in, inSize, o, outSize);
    } else if (attr == kAttrPipeCapsExt) {
        r = answerPipeCapsExt(in, inSize, o, outSize);
    }
    noteAsked(attr, r);
    return r;
}

// v0.53.0: 0x921 = AGDCLinkConfig_t, filled exactly like Apple's GetLinkConfig (research round 18):
//   +0x00 u32 screen index (input)   +0x04 u32 pipe (Apple: stream / -1)   +0x08 u64 flags
//   +0x10..+0x97 = the current mode's IODetailedTimingInformationV2 from its +0x0C (scaled inset .. dscCompressedBpp)
//   +0x98 u16 = timing +0x94 (dscSliceWidth)   +0x9A u16 = timing +0x9A   +0x9C u64 0
//   +0xA4 u32 port number + 1   +0xA8 u32 0   +0xAC u32 1 (valid)
// flags for a built-in, online panel: 0x4 built-in | 0x10 online | 0x1 enabled = 0x15
IOReturn IrisXeAGDC::answerLinkConfig(unsigned long* in, unsigned long inSize, uint8_t* o, unsigned long* outSize)
{
    if (!o || !outSize || *outSize < kLinkConfigSize) return kIOReturnBadArgument;
    uint32_t idx = in ? *(uint32_t*)in : *(uint32_t*)o;
    if (idx != 0) return kIOReturnNoDevice;                  // Apple: index past its screens -> 0xe00002bc
    IOFramebuffer* fb = OSDynamicCast(IOFramebuffer, fFB);
    if (!fb) return kIOReturnNotReady;
    IODisplayModeID mode = 0; IOIndex depth = 0;
    IOTimingInformation ti;
    bzero(&ti, sizeof(ti));
    ti.flags = kIODetailedTimingValid;
    if (fb->getCurrentDisplayMode(&mode, &depth) != kIOReturnSuccess ||
        fb->getTimingInfoForDisplayMode(mode, &ti) != kIOReturnSuccess ||
        !(ti.flags & kIODetailedTimingValid) || !ti.detailedInfo.v2.horizontalActive)
        return kIOReturnNotReady;
    IODetailedTimingInformationV2 t = ti.detailedInfo.v2;
    if (!t.pixelEncoding)         t.pixelEncoding = kIORangePixelEncodingRGB444;
    if (!t.bitsPerColorComponent) t.bitsPerColorComponent = kIORangeBitsPerColorComponent8;
    if (!t.colorimetry)           t.colorimetry = kIORangeColorimetrysRGB;
    if (!t.numLinks)              t.numLinks = 1;
    const uint8_t* tb = (const uint8_t*)&t;
    bzero(o, kLinkConfigSize);
    *(uint32_t*)(o + 0x00) = idx;
    *(uint32_t*)(o + 0x04) = 0;                              // pipe A
    *(uint64_t*)(o + 0x08) = 0x15;                           // built-in | online | enabled
    memcpy(o + 0x10, tb + 0x0C, 0x88);                       // timing +0x0C..+0x93
    *(uint16_t*)(o + 0x98) = *(const uint16_t*)(tb + 0x94);
    *(uint16_t*)(o + 0x9A) = *(const uint16_t*)(tb + 0x9A);
    *(uint32_t*)(o + 0xA4) = 1;                              // port 0 + 1
    *(uint32_t*)(o + 0xAC) = 1;
    *outSize = kLinkConfigSize;
    if (!fLinkLogged) {
        fLinkLogged = true;
        IOLog("IrisXeAGDC: 0x921 link config: mode %d, %ux%u, pixel clock %llu Hz, blanking %u/%u\n", (int)mode,
              t.horizontalActive, t.verticalActive, t.pixelClock, t.horizontalBlanking, t.verticalBlanking);
    }
    return kIOReturnSuccess;
}

// v0.54.0: 0x711 = AGDCFBGetExtendedCapability_t (0x196C bytes in and out), like Apple's
// getDisplayPipeCapabilityExtended (research round 17). Input: +0x00 screen index, +0x04 section, +0x08 max entries (1..8).
// IOPresentment's __GatherAGDCDisplayPipelineCapabilities asks 16 sections; only section 0x10 (planes) is required -
// without it: "Unable to acquire capabilities ... 0x2006". Apple answers 0x10, 1, 2, 4, 8 and returns
// unsupported for the others; we answer 0x10 and 1 (the plane list and the pipe's colour block).
// Section 0x10, per plane (0x32C bytes from +0x0C, plane count at +0x08):
//   +0x00 u32 0xA01C4 (| 0x800 with YUV formats)   +0x04 u64 1   +0x0C u64 0   +0x14 u64 0
//   +0x1C u64 n1, +0x24 n1 x 0x20: {u32 min height, min width, max height, max width; u32 formats; u32 2; u64 0}
//   +0x1A4 u64 n2, +0x1AC n2 x 0x20: the same list again
// Apple's plane 0 (initTransactionCaps / setTransactionCaps): format mask 3 -> formats 0x30000, min 8 x 8,
// max = the plane's size limit; ours: the current mode (one full-screen plane, no scaling).
IOReturn IrisXeAGDC::answerPipeCapsExt(unsigned long* in, unsigned long inSize, uint8_t* o, unsigned long* outSize)
{
    if (!o || !outSize || *outSize < kPipeCapsExtSize) return kIOReturnBadArgument;
    const uint8_t* src = in ? (const uint8_t*)in : o;
    uint32_t idx = *(const uint32_t*)(src + 0), section = *(const uint32_t*)(src + 4), maxN = *(const uint32_t*)(src + 8);
    if (idx != 0) return kIOReturnNoDevice;
    if (maxN < 1 || maxN > 8) return kIOReturnBadArgument;
    uint32_t w = 0, h = 0;
    IOFramebuffer* fb = OSDynamicCast(IOFramebuffer, fFB);
    if (fb) {
        IODisplayModeID mode = 0; IOIndex depth = 0;
        IOTimingInformation ti; bzero(&ti, sizeof(ti)); ti.flags = kIODetailedTimingValid;
        if (fb->getCurrentDisplayMode(&mode, &depth) == kIOReturnSuccess &&
            fb->getTimingInfoForDisplayMode(mode, &ti) == kIOReturnSuccess && (ti.flags & kIODetailedTimingValid)) {
            w = ti.detailedInfo.v2.horizontalActive; h = ti.detailedInfo.v2.verticalActive;
        }
    }
    if (!w || !h) return kIOReturnNotReady;
    if (section == 0x10) {
        bzero(o, kPipeCapsExtSize);
        *(uint32_t*)(o + 0) = idx; *(uint32_t*)(o + 4) = section;
        uint8_t* e = o + 0x0C;                                   // plane 0
        *(uint32_t*)(e + 0x00) = 0xA01C4;
        *(uint64_t*)(e + 0x04) = 1;
        for (int list = 0; list < 2; list++) {
            uint8_t* cnt = e + (list ? 0x1A4 : 0x1C);
            uint8_t* d = e + (list ? 0x1AC : 0x24);
            *(uint64_t*)cnt = 1;
            *(uint32_t*)(d + 0x00) = 8;                         // min height
            *(uint32_t*)(d + 0x04) = 8;                         // min width
            *(uint32_t*)(d + 0x08) = h;                         // max height
            *(uint32_t*)(d + 0x0C) = w;                         // max width
            *(uint32_t*)(d + 0x10) = 0x30000;                   // formats (Apple's mask 3)
            *(uint32_t*)(d + 0x14) = 2;
        }
        *(uint32_t*)(o + 8) = 1;                                 // one plane
    } else if (section == 1) {
        bzero(o, kPipeCapsExtSize);
        *(uint32_t*)(o + 0) = idx; *(uint32_t*)(o + 4) = section;
        *(uint64_t*)(o + 0x08) = 0x500000001ULL;
        *(uint64_t*)(o + 0x18) = 0x800000000ULL;
        *(uint32_t*)(o + 0x2C) = 0x30700;
        *(uint64_t*)(o + 0x30) = 0x3F800000BF800000ULL;          // -1.0, 1.0
    } else {
        return kIOReturnUnsupported;                             // as Apple for 0x20 .. 0x8000; 2, 4, 8 later if needed
    }
    *outSize = kPipeCapsExtSize;
    if (!(fPipeCapsLogged & (section == 0x10 ? 1 : 2))) {
        fPipeCapsLogged |= (section == 0x10 ? 1 : 2);
        IOLog("IrisXeAGDC: 0x711 section 0x%x answered (plane max %ux%u)\n", section, w, h);
    }
    return kIOReturnSuccess;
}

// ---------------------------------------------------------------- after-login control (sysctl)

static IOLock*             sCtlLock;
static IrisXeAGDC*         sAGDC;
static IrisXeFBController* sPort;
static int                 sState;          // 0 off, 1 running
static char                sStep[200] = "off (after login: sudo sysctl debug.irisxe_agdc=1)";

static void step(const char* s)
{
    strlcpy(sStep, s, sizeof(sStep));
    IOLog("IrisXeAGDC: %s\n", s);
}

static IOService* copyService(const char* cls)
{
    OSDictionary* m = IOService::serviceMatching(cls);
    if (!m) return nullptr;
    IOService* s = IOService::copyMatchingService(m);
    m->release();
    return s;
}

static void agdcStart()
{
    IOService* fb = copyService("IrisXeFB");
    if (!fb) { step("not started: IrisXeFB is not running"); return; }
    IOService* gpu = fb->getProvider();
    if (!gpu || !OSDynamicCast(IOPCIDevice, gpu)) { fb->release(); step("not started: IrisXeFB has no PCI device"); return; }

    // Apple's AGDC::start waits with no time limit for AppleGPUWrangler: never start without it
    IOService* wr = copyService("AppleGPUWrangler");
    if (!wr) { fb->release(); step("not started: AppleGPUWrangler is missing (Apple's start would wait forever)"); return; }
    wr->release();

    // our object must have room for Apple's fields
    size_t appleSize = AppleGraphicsDeviceControl::metaClass->getClassSize();
    size_t room = sizeof(IOService) + IrisXeAGDC::kAppleRoom;
    if (appleSize > room) {
        char s[200];
        snprintf(s, sizeof(s), "not started: Apple's AGDC is %lu bytes, room is %lu", (unsigned long)appleSize, (unsigned long)room);
        fb->release(); step(s); return;
    }

    step("1/3 making IrisXeFBController under the GPU");
    IrisXeFBController* port = OSTypeAlloc(IrisXeFBController);
    if (!port || !port->init() || !port->attach(gpu) || !port->start(gpu)) {
        if (port) { port->detach(gpu); port->release(); }
        fb->release(); step("failed at 1/3: IrisXeFBController"); return;
    }
    port->setName("IrisXeFBController");
    port->registerService();

    step("2/3 starting the AGDC (Apple's start asks attributes 1 and 0x980)");
    IrisXeAGDC* agdc = OSTypeAlloc(IrisXeAGDC);
    bool ok = agdc && agdc->init() && agdc->setup(fb, gpu) && agdc->attach(port);
    if (ok && !agdc->start(port)) { agdc->detach(port); ok = false; }
    fb->release();
    if (!ok) {
        if (agdc) agdc->release();
        port->terminate(); port->release();
        step("failed at 2/3: Apple's AppleGraphicsDeviceControl::start returned false");
        return;
    }
    sAGDC = agdc;
    sPort = port;
    sState = 1;
    step("3/3 running: AGDC published (IrisXeAGDC under IrisXeFBController under the GPU)");
}

static void agdcStop()
{
    if (sAGDC) { sAGDC->terminate(); sAGDC->release(); sAGDC = nullptr; }
    if (sPort) { sPort->terminate(); sPort->release(); sPort = nullptr; }
    sState = 0;
    step("off (removed; sudo sysctl debug.irisxe_agdc=1 makes it again)");
}

static int sysctlAgdc SYSCTL_HANDLER_ARGS
{
    int v = sState;
    int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr) return err;
    IOLockLock(sCtlLock);
    if (v == 1 && !sAGDC) agdcStart();
    else if (v == 0 && sAGDC) agdcStop();
    IOLockUnlock(sCtlLock);
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, irisxe_agdc, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, sysctlAgdc, "I",
            "IrisXe AGDC: 1 = create, 0 = remove");

static int sysctlAgdcStatus SYSCTL_HANDLER_ARGS
{
    char buf[1024];
    char asked[800];
    IOLockLock(sCtlLock);
    if (sAGDC) sAGDC->describeAsked(asked, sizeof(asked));
    else strlcpy(asked, "-", sizeof(asked));
    snprintf(buf, sizeof(buf), "IrisXeAGDC %s: %s | asked: %s", KEXT_VERSION, sStep, asked);
    IOLockUnlock(sCtlLock);
    return sysctl_handle_string(oidp, buf, strlen(buf) + 1, req);
}
SYSCTL_PROC(_debug, OID_AUTO, irisxe_agdc_status, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0,
            sysctlAgdcStatus, "A", "IrisXe AGDC status");

// ---------------------------------------------------------------- at boot (v0.64.0)
// IOKit loads this kext when IrisXeIntelFB appears (personality IrisXeAGDCBoot in Info.plist). IrisXeAccel decides
// whether the GPU-drawn desktop is started this boot (safety net, -irisxecpu) and asks through the framebuffer's
// property IrisXe-AGDCRequest; the answer goes into IrisXe-AGDCResult (the status line below). Waits on its own
// thread: Apple's AGDC start must not run inside IOKit's matching (and waits forever without AppleGPUWrangler).
OSDefineMetaClassAndStructors(IrisXeAGDCBoot, IOService)

static void agdcBootThread(void* arg, wait_result_t)
{
    IOService* fb = (IOService*)arg;
    OSObject* req = nullptr;
    for (int i = 0; i < 240 && !req; i++) {          // 60 s for IrisXeAccel's decision
        req = fb->getProperty("IrisXe-AGDCRequest");
        if (!req) IOSleep(250);
    }
    if (req != kOSBooleanTrue) {
        IOLog("IrisXeAGDC: boot: %s\n", req ? "not asked this boot (IrisXeAccel: CPU-drawn desktop)" : "no request from IrisXeAccel in 60 s");
        fb->release();
        return;
    }
    IOService* wr = nullptr;
    for (int i = 0; i < 80 && !wr; i++) {            // 20 s for AppleGPUWrangler
        wr = copyService("AppleGPUWrangler");
        if (!wr) IOSleep(250);
    }
    if (wr) wr->release();
    IOLockLock(sCtlLock);
    if (!sAGDC) agdcStart();                         // reports "not started: AppleGPUWrangler is missing" itself
    char res[200];
    strlcpy(res, sStep, sizeof(res));
    IOLockUnlock(sCtlLock);
    fb->setProperty("IrisXe-AGDCResult", res);
    IOLog("IrisXeAGDC: boot: %s\n", res);
    fb->release();
}

bool IrisXeAGDCBoot::start(IOService* provider)
{
    if (!IOService::start(provider) || !sCtlLock) return false;
    provider->retain();
    thread_t th;
    if (kernel_thread_start(&agdcBootThread, provider, &th) == KERN_SUCCESS) thread_deallocate(th);
    else { provider->setProperty("IrisXe-AGDCResult", "failed: no thread"); provider->release(); }
    return true;                                     // stays (keeps this kext loaded while the AGDC runs)
}

extern "C" kern_return_t irisxe_agdc_start(kmod_info_t*, void*)
{
    sCtlLock = IOLockAlloc();
    if (!sCtlLock) return KERN_FAILURE;
    sysctl_register_oid(&sysctl__debug_irisxe_agdc);
    sysctl_register_oid(&sysctl__debug_irisxe_agdc_status);
    IOLog("IrisXeAGDC %s loaded: %s\n", KEXT_VERSION, sStep);
    return KERN_SUCCESS;
}

extern "C" kern_return_t irisxe_agdc_stop(kmod_info_t*, void*)
{
    if (sAGDC) return KERN_FAILURE;             // still running: sudo sysctl debug.irisxe_agdc=0 first
    sysctl_unregister_oid(&sysctl__debug_irisxe_agdc);
    sysctl_unregister_oid(&sysctl__debug_irisxe_agdc_status);
    IOLockFree(sCtlLock);
    sCtlLock = nullptr;
    return KERN_SUCCESS;
}

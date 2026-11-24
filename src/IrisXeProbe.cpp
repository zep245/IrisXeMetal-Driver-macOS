// IrisXeProbe - passive, read-only hardware report. Safe to run while another
// driver (e.g. NMXGPU) owns the GPU.

#include "IrisXeProbe.hpp"
#include "TGLRegs.h"
#include <libkern/libkern.h>
#include <string.h>

#define LOG(fmt, ...) IOLog("IrisXeProbe: " fmt "\n", ##__VA_ARGS__)

OSDefineMetaClassAndStructors(IrisXeProbe, IOService)

IOService *IrisXeProbe::probe(IOService *provider, SInt32 *score) {
    if (!OSDynamicCast(IOPCIDevice, provider)) return nullptr;
    if (PE_parse_boot_argn("-irisxeoff", nullptr, 0)) return nullptr;
    return super::probe(provider, score);
}

bool IrisXeProbe::start(IOService *provider) {
    if (!super::start(provider)) return false;
    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) return false;

    // macOS only uses its built-in-panel display class (AppleBacklightDisplay,
    // brightness slider + keys) if a service named "backlight" exists before
    // the first framebuffer opens. We load early from OpenCore, so publish it
    // here. Only in takeover mode, so we never change anything for other drivers.
    if (PE_parse_boot_argn("-irisxefb", nullptr, 0)) {
        IOService *nub = OSTypeAlloc(IOService);
        if (nub && nub->init()) {
            nub->setName("backlight");
            nub->setProperty("IrisXe-BacklightNub", true);
            if (nub->attach(this)) {
                nub->registerService();
                LOG("published backlight nub");
            }
        }
        OSSafeReleaseNULL(nub);
    }

    setProperty("IrisXe-VendorID", pci->configRead16(kIOPCIConfigVendorID), 16);
    setProperty("IrisXe-DeviceID", pci->configRead16(kIOPCIConfigDeviceID), 16);
    setProperty("IrisXe-Revision", pci->configRead8(kIOPCIConfigRevisionID), 8);
    LOG("device %04x:%04x rev %02x subsys %04x:%04x",
        pci->configRead16(kIOPCIConfigVendorID), pci->configRead16(kIOPCIConfigDeviceID),
        pci->configRead8(kIOPCIConfigRevisionID),
        pci->configRead16(kIOPCIConfigSubSystemVendorID),
        pci->configRead16(kIOPCIConfigSubSystemID));

    IODeviceMemory *bar0 = pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    IODeviceMemory *bar2 = pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress2);
    if (bar0) {
        setProperty("IrisXe-BAR0-Phys", (unsigned long long)bar0->getPhysicalAddress(), 64);
        setProperty("IrisXe-BAR0-Size", (unsigned long long)bar0->getLength(), 64);
    }
    if (bar2) {
        setProperty("IrisXe-BAR2-Phys", (unsigned long long)bar2->getPhysicalAddress(), 64);
        setProperty("IrisXe-BAR2-Size", (unsigned long long)bar2->getLength(), 64);
    }
    if (bar0) LOG("BAR0 (MMIO+GTT) phys=0x%llx size=0x%llx",
                  (unsigned long long)bar0->getPhysicalAddress(),
                  (unsigned long long)bar0->getLength());
    if (bar2) LOG("BAR2 (aperture) phys=0x%llx size=0x%llx",
                  (unsigned long long)bar2->getPhysicalAddress(),
                  (unsigned long long)bar2->getLength());

    // Only map if memory decoding is already on (another driver or firmware
    // enabled it). We never change PCI command bits here.
    UInt16 cmd = pci->configRead16(kIOPCIConfigCommand);
    setProperty("IrisXe-PCICommand", cmd, 16);
    if (!(cmd & kIOPCICommandMemorySpace)) {
        setProperty("IrisXe-ProbeStatus", "memory decoding off");
        LOG("memory decoding off (cmd=0x%04x); skipping register read", cmd);
        return true;
    }

    IOMemoryMap *map = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if (!map) {
        setProperty("IrisXe-ProbeStatus", "could not map BAR0");
        LOG("could not map BAR0");
        return true;
    }
    volatile UInt8 *mmio = (volatile UInt8 *)map->getVirtualAddress();
    #define RD(off) (*(volatile UInt32 *)(mmio + (off)))

    UInt32 ctl = RD(TGL_PLANE_CTL_1_A);
    UInt32 src = RD(TGL_PIPESRC_A);
    setProperty("IrisXe-PLANE_CTL_1_A",    ctl, 32);
    setProperty("IrisXe-PLANE_STRIDE_1_A", RD(TGL_PLANE_STRIDE_1_A), 32);
    setProperty("IrisXe-PLANE_SIZE_1_A",   RD(TGL_PLANE_SIZE_1_A), 32);
    setProperty("IrisXe-PLANE_SURF_1_A",   RD(TGL_PLANE_SURF_1_A), 32);
    setProperty("IrisXe-PIPESRC_A",        src, 32);
    setProperty("IrisXe-BLC_PWM_CTL1",     RD(TGL_BLC_PWM_CTL1), 32);
    setProperty("IrisXe-BLC_PWM_FREQ1",    RD(TGL_BLC_PWM_FREQ1), 32);
    setProperty("IrisXe-BLC_PWM_DUTY1",    RD(TGL_BLC_PWM_DUTY1), 32);
    LOG("plane1A ctl=0x%08x stride=0x%08x size=0x%08x surf=0x%08x",
        ctl, RD(TGL_PLANE_STRIDE_1_A), RD(TGL_PLANE_SIZE_1_A), RD(TGL_PLANE_SURF_1_A));
    LOG("pipeA %ux%u, plane enabled=%d tiling=%u",
        ((src >> 16) & 0xFFFF) + 1, (src & 0xFFFF) + 1,
        (ctl & TGL_PLANE_CTL_ENABLE) ? 1 : 0, (ctl & TGL_PLANE_CTL_TILING_MASK) >> 10);
    LOG("backlight ctl=0x%08x freq=0x%08x duty=0x%08x",
        RD(TGL_BLC_PWM_CTL1), RD(TGL_BLC_PWM_FREQ1), RD(TGL_BLC_PWM_DUTY1));
    #undef RD

    // GPU engines, step 1 (opt-in): wake the GT with forcewake and read what
    // is there. Only forcewake request registers are written; released after.
    if (PE_parse_boot_argn("-irisxegt", nullptr, 0))
        gtReport(mmio);
    // GPU engines, step 2 (opt-in): first job on the blitter engine
    // step 4 (GPU core) replaces the one-shot tests; step 3 replaces step 2
    bool gpuCore = PE_parse_boot_argn("-irisxegpu", nullptr, 0);
    if (gpuCore) {
        // started after this report, with its own BAR mapping
    } else if (PE_parse_boot_argn("-irisxeppgtt", nullptr, 0))
        gtPPGTTTest(mmio, map->getLength());
    else if (PE_parse_boot_argn("-irisxeblit", nullptr, 0))
        gtBlitTest(mmio, map->getLength());

    map->release();
    if (gpuCore) gpuStart(pci);
    setProperty("IrisXe-ProbeStatus", "done (read-only)");
    LOG("report done (read-only)");
    return true;
}

// ---------------------------------------------------------------------------
// GT (the GPU core) report. Register map: Linux i915 gt/intel_gt_regs.h,
// gt/intel_engine_regs.h, gt/uc/intel_guc_reg.h. Forcewake as in
// intel_uncore.c fw_domain_get(): masked write of FORCEWAKE_KERNEL (bit 0),
// then wait for the domain's ACK register to show it.

#define FW_KERNEL               (1u << 0)
#define FW_MASKED(bit, on)      (((bit) << 16) | ((on) ? (bit) : 0))

struct FWDomain { const char *name; UInt32 req; UInt32 ack; };
static const FWDomain kFWDomains[] = {
    { "render", 0xA278,   0x0D84   },
    { "gt",     0xA188,   0x130044 },
    { "vdbox0", 0xA540,   0x0D50   },
    { "vebox0", 0xA560,   0x0D70   },
};

static bool fwWait(volatile UInt8 *mmio, UInt32 ack, bool set, UInt32 *us) {
    for (UInt32 t = 0; t < 50000; t += 10) {        // 50 ms max
        bool on = (*(volatile UInt32 *)(mmio + ack) & FW_KERNEL) != 0;
        if (on == set) { if (us) *us = t; return true; }
        IODelay(10);
    }
    return false;
}

static UInt32 popcount32(UInt32 v) { UInt32 c = 0; while (v) { c += v & 1; v >>= 1; } return c; }

void IrisXeProbe::gtReport(volatile UInt8 *mmio) {
    #define RD(off) (*(volatile UInt32 *)(mmio + (off)))
    #define WR(off, v) (*(volatile UInt32 *)(mmio + (off)) = (v))
    OSDictionary *gt = OSDictionary::withCapacity(48);
    if (!gt) return;
    char buf[128];
    auto put = [&](const char *k, UInt32 v) {
        OSNumber *n = OSNumber::withNumber(v, 32);
        if (n) { gt->setObject(k, n); n->release(); }
    };
    auto puts = [&](const char *k, const char *v) {
        OSString *str = OSString::withCString(v);
        if (str) { gt->setObject(k, str); str->release(); }
    };

    // 1. before waking: power state as firmware left it
    put("RC_CONTROL", RD(0xA090));
    put("RC_STATE", RD(0xA094));          // bits 18:16 = RC state (0 = RC0 awake, 3 = RC6)
    put("PG_ENABLE", RD(0xA210));
    put("CORE_STATUS", RD(0x138060));

    // 2. forcewake every domain
    bool awake[4] = {};
    buf[0] = 0;
    for (UInt32 i = 0; i < 4; i++) {
        const FWDomain &d = kFWDomains[i];
        UInt32 us = 0;
        fwWait(mmio, d.ack, false, nullptr);              // previous release done
        WR(d.req, FW_MASKED(FW_KERNEL, true));
        awake[i] = fwWait(mmio, d.ack, true, &us);
        char e[32];
        if (awake[i]) snprintf(e, sizeof(e), "%s=ok(%uus) ", d.name, us);
        else          snprintf(e, sizeof(e), "%s=TIMEOUT(ack %x) ", d.name, RD(d.ack));
        strlcat(buf, e, sizeof(buf));
    }
    puts("Forcewake", buf);

    if (awake[0]) {
        // 3. what the GPU is made of (fuses)
        UInt32 dss   = RD(0x913C);      // GEN12_GT_GEOMETRY_DSS_ENABLE
        UInt32 eudis = RD(0x9134);      // GEN11_EU_DISABLE (disabled EU pairs, per DSS)
        UInt32 media = RD(0x9140);      // GEN11_GT_VEBOX_VDBOX_DISABLE
        put("DSS_ENABLE", dss);
        put("EU_DISABLE", eudis);
        put("VDBOX_VEBOX_DISABLE", media);
        UInt32 nDSS = popcount32(dss);
        UInt32 euPerDSS = (8 - popcount32(eudis & 0xFF)) * 2;
        put("DSS_count", nDSS);
        put("EU_count", nDSS * euPerDSS);

        // 4. clocks: current and the hardware limits (units of 50/3 MHz)
        UInt32 rpstat = RD(0xA01C);
        UInt32 cap    = RD(0x145998);   // GEN6_RP_STATE_CAP (MCHBAR mirror)
        put("RPSTAT1", rpstat);
        put("RP_STATE_CAP", cap);
        put("FreqNow_MHz", ((rpstat >> 23) & 0x1FF) * 50 / 3);
        put("FreqMax_MHz", (cap & 0xFF) * 50 / 3);
        put("FreqEff_MHz", ((cap >> 8) & 0xFF) * 50 / 3);
        put("FreqMin_MHz", ((cap >> 16) & 0xFF) * 50 / 3);
        put("RC6_residency", RD(0x138108));

        // 5. engines: is anything set up / running?
        struct Eng { const char *name; UInt32 base; };
        static const Eng engines[] = {
            { "RCS0", 0x02000 }, { "BCS0", 0x22000 }, { "VCS0", 0x1C0000 }, { "VECS0", 0x1C8000 },
        };
        for (const Eng &e : engines) {
            UInt32 b = e.base;
            snprintf(buf, sizeof(buf),
                     "head=%x tail=%x start=%x ctl=%x mode=%x mi=%x hws=%x el=%x esr=%x",
                     RD(b + 0x34), RD(b + 0x30), RD(b + 0x38), RD(b + 0x3C),
                     RD(b + 0x29C), RD(b + 0x9C), RD(b + 0x80), RD(b + 0x234), RD(b + 0xB8));
            puts(e.name, buf);
        }

        // 6. GuC / HuC microcontrollers (firmware loaded yet?)
        put("GUC_STATUS", RD(0xC000));
        put("GUC_WOPCM_SIZE", RD(0xC050));
        put("DMA_GUC_WOPCM_OFFSET", RD(0xC340));
        put("HUC_STATUS2", RD(0xD3B0));
        put("GT_THREAD_STATUS", RD(0x13805C));
    }

    // 7. release forcewake (GPU may go back to RC6)
    for (UInt32 i = 0; i < 4; i++)
        if (awake[i]) WR(kFWDomains[i].req, FW_MASKED(FW_KERNEL, false));

    setProperty("IrisXe-GT", gt);
    gt->release();
    #undef RD
    #undef WR
}

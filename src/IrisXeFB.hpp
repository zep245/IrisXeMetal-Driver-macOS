// IrisXeFB - framebuffer driver for Intel Tiger Lake Iris Xe (8086:9a49)
#ifndef IRISXE_FB_HPP
#define IRISXE_FB_HPP

#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOBufferMemoryDescriptor.h>

class IrisXeBacklight;

class IrisXeFB : public IOFramebuffer {
    OSDeclareDefaultStructors(IrisXeFB)
    typedef IOFramebuffer super;

private:
    IOPCIDevice   *pci      = nullptr;
    IOMemoryMap   *mmioMap  = nullptr;
    volatile UInt8 *mmio    = nullptr;

    // Current scan-out surface (inherited from UEFI GOP)
    IOPhysicalAddress fbPhys   = 0;
    IOByteCount       fbSize   = 0;
    UInt32            width    = 0;
    UInt32            height   = 0;
    UInt32            rowBytes = 0;

    // Panel identity (EDID read over DP AUX)
    UInt8             edid[256];
    UInt32            edidLen  = 0;

    // Native timing (from EDID, or synthesized) and OS-provided detailed timings
    IODetailedTimingInformationV2 nativeTiming;
    OSArray          *detailedTimings = nullptr;
    IODisplayModeID   currentMode;

    IrisXeBacklight  *backlight = nullptr;

    // Display power (DPMS): 0 off, 1 dim, 2 on
    UInt32            powerState = 2;
    UInt32            savedDuty  = 0;
    char              powerLog[96] = {0};
    UInt32            fbEventState = 2;      // last state reported via handleEvent
    char              eventLog[96] = {0};
    char              brightLog[480] = {0};
    bool              sawSystemSleep = false;
    void   setDisplayPower(UInt32 state);
    void   recordWakeCheck(const char *when);

    UInt32 readReg(UInt32 off) const;
    void   writeReg(UInt32 off, UInt32 val);
    bool   discoverFramebuffer();
    void   logPlaneState();

    int    auxTransfer(const UInt8 *tx, int txLen, UInt8 *rx, int rxMax);
    int    auxI2C(UInt8 request, const UInt8 *data, int dataLen, UInt8 *rx, int rxMax);
    bool   readEDID();
    int    auxNativeRead(UInt32 addr, UInt8 *buf, int len);
    void   snapshotDisplay(const char *regsKey = "IrisXe-Snapshot",
                           const char *phyKey = "IrisXe-SnapshotPHY",
                           bool withDPCD = true);
    int    auxNativeWrite(UInt32 addr, const UInt8 *buf, int len);
    bool   trainLink(char *report, size_t reportLen, bool finish = true);
    bool   retrainFull(char *report, size_t reportLen);

    // Stage C: rebuild the whole eDP path after system sleep from the boot snapshot
    OSDictionary *bootRegs = nullptr;
    OSDictionary *bootPHY  = nullptr;
    UInt32        colorRegs[16 + 2 + 256];   // pipe CSC block, GAMMA_MODE, PLANE_COLOR_CTL, palette
    bool          haveColor = false;
    UInt32        wmRegs[9];                 // PLANE_WM_1_A levels 0-7 + TRANS
    UInt32        bootPTE0[2] = {0, 0};      // GGTT entry 0 (scan-out page) at boot
    IOTimerEventSource *liveTimer = nullptr;
    IOTimerEventSource *blTimer = nullptr;
    UInt32        pendingDuty = 0;
    static void blTimerFired(OSObject *owner, IOTimerEventSource *sender);
    IOMemoryMap  *fbMap = nullptr;           // CPU view of the scan-out buffer
    volatile UInt32 *fbVA = nullptr;
    UInt32        screenSig[32 * 20];        // pixel grid sampled when going dark
    bool          haveScreenSig = false;
    UInt32        blWaitedMS = 0;
    void   sampleScreen(UInt32 *out) const;
    bool   screenChanged() const;
    UInt32        liveTicks = 0;
    void   liveTick();
    static void liveTimerFired(OSObject *owner, IOTimerEventSource *sender);
    void   saveColor();
    void   restoreColor();
    UInt32 snapReg(const char *key, bool phy = false) const;
    bool   waitBits(UInt32 off, UInt32 mask, UInt32 value, UInt32 timeoutMs);
    bool   restoreDisplay(char *report, size_t reportLen);
    void   buildNativeTiming();

    // Hardware cursor (cursor plane A, 256x256 ARGB, in our own GGTT pages)
    IOBufferMemoryDescriptor *curMem = nullptr;
    volatile UInt32 *curVA    = nullptr;     // 256x256 pixels, write-combined
    UInt32          *curStage = nullptr;     // convertCursorImage() output
    UInt64        curPhys     = 0;
    UInt32        curGGTT     = 0;           // GPU address of the cursor image
    bool          curReady    = false;       // memory + GGTT + DDB set up
    bool          curHaveImage = false;
    bool          curVisible  = false;
    SInt32        curX = 0, curY = 0;
    UInt32        curLastH    = 256;
    UInt32        bootPlaneBuf = 0;          // PLANE_BUF_CFG_1_A at boot
    UInt32        bootCurBuf  = 0;           // CUR_BUF_CFG_A at boot
    UInt32        curBuf      = 0;           // CUR_BUF_CFG_A we use
    UInt32        planeBuf    = 0;           // PLANE_BUF_CFG_1_A we use
    bool          wakeCursor  = false;
    bool   cursorInit();
    void   cursorHWSetup();                  // GGTT, DDB split, watermarks
    void   cursorUpdate();                   // CURCNTR / CURPOS / CURBASE
    void   cursorShutdown();
    UInt32 cursorWMFromPlane(UInt32 planeWM, bool level0) const;

    bool   timingIsUsable(const IODetailedTimingInformationV2 *t) const;
    const IODetailedTimingInformationV2 *timingForMode(IODisplayModeID mode) const;
    UInt32 refreshFixed(const IODetailedTimingInformationV2 *t) const;

public:
    // Backlight (used by IrisXeBacklight)
    bool   backlightAvailable() const;
    UInt32 backlightLevel(UInt32 maxLevel) const;
    void   setBacklightLevel(UInt32 level, UInt32 maxLevel);
    void   noteBrightness(const char *key, UInt32 value);   // diagnostics

    // IOService
    virtual IOService *probe(IOService *provider, SInt32 *score) override;
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;
    virtual void free() override;
    virtual void systemWillShutdown(IOOptionBits specifier) override;

    // IOFramebuffer
    virtual IOReturn enableController() override;
    virtual unsigned long maxCapabilityForDomainState(IOPMPowerFlags domainState) override;
    virtual unsigned long initialPowerStateForDomainState(IOPMPowerFlags domainState) override;
    virtual unsigned long powerStateForDomainState(IOPMPowerFlags domainState) override;
    virtual bool isConsoleDevice() override;
    virtual IODeviceMemory *getApertureRange(IOPixelAperture aperture) override;
    virtual IODeviceMemory *getVRAMRange() override;
    virtual const char *getPixelFormats() override;
    virtual IOItemCount getDisplayModeCount() override;
    virtual IOReturn getDisplayModes(IODisplayModeID *allDisplayModes) override;
    virtual IOReturn getInformationForDisplayMode(IODisplayModeID mode,
                                                  IODisplayModeInformation *info) override;
    virtual UInt64 getPixelFormatsForDisplayMode(IODisplayModeID mode, IOIndex depth) override;
    virtual IOReturn getPixelInformation(IODisplayModeID mode, IOIndex depth,
                                         IOPixelAperture aperture,
                                         IOPixelInformation *pixelInfo) override;
    virtual IOReturn getCurrentDisplayMode(IODisplayModeID *mode, IOIndex *depth) override;
    virtual IOReturn setDisplayMode(IODisplayModeID mode, IOIndex depth) override;
    virtual IOReturn getTimingInfoForDisplayMode(IODisplayModeID mode,
                                                 IOTimingInformation *info) override;
    virtual IOReturn validateDetailedTiming(void *description, IOByteCount descripSize) override;
    virtual IOReturn setDetailedTimings(OSArray *array) override;
    virtual IOItemCount getConnectionCount() override;
    virtual IOReturn setAttribute(IOSelect attribute, uintptr_t value) override;
    virtual IOReturn getAttribute(IOSelect attribute, uintptr_t *value) override;
    virtual IOReturn setCursorImage(void *cursorImage) override;
    virtual IOReturn setCursorState(SInt32 x, SInt32 y, bool visible) override;
    virtual IOReturn setGammaTable(UInt32 channelCount, UInt32 dataCount,
                                   UInt32 dataWidth, void *data) override;
    virtual IOReturn setGammaTable(UInt32 channelCount, UInt32 dataCount,
                                   UInt32 dataWidth, void *data, bool syncToVBL) override;
    virtual IOReturn getAttributeForConnection(IOIndex connectIndex, IOSelect attribute,
                                               uintptr_t *value) override;
    virtual IOReturn setAttributeForConnection(IOIndex connectIndex, IOSelect attribute,
                                               uintptr_t value) override;
    virtual bool hasDDCConnect(IOIndex connectIndex) override;
    virtual IOReturn getAppleSense(IOIndex connectIndex, UInt32 *senseType, UInt32 *primary,
                                   UInt32 *extended, UInt32 *displayType) override;
    virtual IOReturn getDDCBlock(IOIndex connectIndex, UInt32 blockNumber, IOSelect blockType,
                                 IOOptionBits options, UInt8 *data, IOByteCount *length) override;
};

// v0.55.0: the same framebuffer under a class name that contains "Intel". CoreDisplay tells GPU makers apart by the
// framebuffer's class name (GetGPUVendorForFramebufferService looks for "Intel", "AMD", "AppleParavirt", "NVDA") and
// switches Apple's display system (IOPresentment, GPU-drawn screen) off when no framebuffer matches - "IrisXeFB"
// matches none. Opt-in for ONE boot: `sudo nvram irisxe-intelfb=1` (removed again at boot), or boot-arg -irisxeintelfb.
// Every other boot uses IrisXeFB exactly as before.
class IrisXeIntelFB : public IrisXeFB {
    OSDeclareDefaultStructors(IrisXeIntelFB)
public:
    virtual IOService *probe(IOService *provider, SInt32 *score) override;
    virtual bool start(IOService *provider) override;
};

#endif

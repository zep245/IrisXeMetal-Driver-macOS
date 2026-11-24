// v0.56.0: our display pipe - shows IOPresentment's screen images on the panel.
//
// How a screen update travels (IOAcceleratorFamily2, research rounds 26-28):
//   WindowServer/CoreDisplay -> IOPresentment transaction -> IOAccelDisplayPipe::transaction_end
//   -> transaction_queue_gated: submitTransaction() (ours returns kXeNotHandled, like Apple's base class, which means
//      "no GPU flip: wait until the GPU has finished drawing the image, then call performTransaction")
//   -> event_interrupt_gated: once the image's GPU work is done the transaction goes live -> performTransaction()
//      (ours: copy the image into the framebuffer the panel shows)
//   -> transaction interrupt (signalTransactionInterrupt) -> isTransactionComplete() (ours: yes) ->
//      completeTransaction: WindowServer is told the image is on screen.
// The panel's vertical blank is not wired up yet, so a 60 Hz timer calls signalVBLInterrupt (which also raises the
// transaction interrupt while one is wanted).
#pragma once
#include "apple/IOAccelDisplayPipe.h"
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>

// Kernel IOSurface (com.apple.iokit.IOSurface): only the exported getters we call (round 27: width, height and
// bytes per row are 64-bit, the pixel format 32-bit).
class IOSurface : public OSObject {
public:
    IOMemoryDescriptor *getMemoryDescriptor() const;
    uint64_t getWidth() const;
    uint64_t getHeight() const;
    uint64_t getBytesPerRow() const;
    uint32_t getPixelFormat() const;
    uint64_t getAllocSize() const;
};

// IOAccelDisplayPipeTransaction2: the exported non-virtual getter we call.
class IOAccelDisplayPipeTransaction2 : public OSObject {
public:
    IOSurface *getPlaneIOSurface(unsigned int plane, unsigned int index) const;
};

uint64_t IrisXeDisplayPipeFrames();             // v0.64.0
class IrisXeDisplayPipe : public IOAccelDisplayPipe {
    OSDeclareDefaultStructors(IrisXeDisplayPipe)
public:
    virtual IRISXE_SLOT_RET init(IOGraphicsAccelerator2*, IOAccelDisplayMachine*, IOFramebuffer*, unsigned int) override;   // 268
    virtual IRISXE_SLOT_RET initFramebufferResource(unsigned int, IOAccelResource2*) override;     // 269 (v0.56.3)
    virtual IRISXE_SLOT_RET destroyFramebufferResource(unsigned int, IOAccelResource2*) override;  // 270 (v0.56.3)
    virtual IRISXE_SLOT_RET enableVBLInterrupt() override;                                   // 273
    virtual IRISXE_SLOT_RET disableVBLInterrupt() override;                                  // 274
    virtual IRISXE_SLOT_RET newDisplayPipeTransaction() override;                            // 277 (v0.56.1: counted)
    virtual IRISXE_SLOT_RET validateTransaction(IOAccelDisplayPipeTransaction2*) override;   // 278
    virtual IRISXE_SLOT_RET performTransaction(IOAccelDisplayPipeTransaction2*) override;    // 279
    virtual IRISXE_SLOT_RET isTransactionComplete(IOAccelDisplayPipeTransaction2*) override; // 280
    virtual IRISXE_SLOT_RET submitTransaction(IOAccelDisplayPipeTransaction2*) override;     // 281
    virtual void free() override;

    static bool sizeOK();                       // our object at least as big as Apple's IOAccelDisplayPipe
    static IrisXeDisplayPipe *sPipe;            // v0.56.3: the pipe Apple's display machine made for IrisXeFB
    void activate();                            // v0.56.3: make the pipe "active" (see IrisXeDisplayPipe.cpp)
    friend uint64_t IrisXeDisplayPipeFrames();  // v0.64.0
private:
    static void vblTick(OSObject *owner, IOTimerEventSource *t);
    bool mapFramebuffer();
    void note(const char *what);

    // Apple's part of the object (IOAccelDisplayPipe uses fields up to at least 0x340; checked at run time with
    // getClassSize() before we make one). Never touched by us.
    UInt8 _apple[0x500];
    // ours
    IOWorkLoop *xeLoop;
    IOTimerEventSource *xeTimer;
    IOMemoryMap *xeFBMap;
    uint32_t xeFBRow, xeFBW, xeFBH;
    uint64_t xeFrames, xeSkipped;
    uint32_t xeNewTx, xeValidate, xeSubmit, xePerform, xeComplete, xeTicks;   // v0.56.1: which steps are reached
    uint32_t xeLastFormat;
    bool xeVBL;
    IODeviceMemory *xeFBMem;                    // v0.56.3: the framebuffer memory given to Apple's resource
    // v0.56.6: WindowServer draws into a few IOSurfaces in turn; each one's memory is wired and mapped into the
    // kernel once, then every frame is one memcpy per row (was 1600 readBytes calls per frame)
    // v0.59.0: + the image's scanout slot (slot i = xeSrc[i]): GGTT address (0 = not mapped) and IrisXeCore's gen
    struct XeSrc { IOMemoryDescriptor *md; IOMemoryMap *map; uint64_t lastUse; uint32_t ggtt, gen; };
    XeSrc xeSrc[4];
    uint64_t xeUseClock, xeCopyNs, xeCopyMaxNs, xeCopyCount, xeFast, xeSlow;
    int srcSlot(IOMemoryDescriptor *md, uint64_t need);      // v0.59.0: index (the slots on screen are never replaced)
    uint8_t *srcMapping(IOMemoryDescriptor *md, uint64_t need);
    void dropSources();
    // v0.59.0: direct scanout - the panel shows WindowServer's image itself (IrisXeCore: kXeScan*)
    bool scanShow(int i, uint64_t w, uint64_t h, uint64_t bpr);   // flip armed
    void park(bool copyBack);                                      // the panel's own framebuffer again (xeLock held)
    bool flipLanded();                                             // the pending flip is on screen now
    IOLock *xeLock;
    int xeShowIdx, xePendIdx;                   // slot on screen / armed (-1 none)
    uint32_t xePendAddr;
    uint64_t xePendSince, xeLastPerform;        // mach_absolute_time
    uint32_t xeShowW, xeShowH, xeShowBpr;       // of the image on screen (for park's copy back)
    uint64_t xeDirect, xeFlushNs, xeFlushMaxNs, xeFlipNs, xeFlipMaxNs, xeFlipCount, xeMapNs, xeSlotMaps;
    uint32_t xeTimeouts, xeBackoff, xeParks, xeScanFail, xeVblUS, xePeriodUS;
    // v0.59.1: stutter meter (IrisXe-FrameTiming): gaps between frames while the screen is moving, and how long
    // WindowServer's frame waited for its GPU work (submit -> perform)
public:
    void timingReset();
    void timingPublish();
private:
    uint64_t xeSubmitT, xeWaitNs, xeWaitMaxNs, xeWaitCount;
    uint64_t xeMoveNs, xeMoveFrames, xeGap[4], xeGapMaxNs, xeTimingSince;
    // v0.59.2: the VBL signal follows the panel's real vertical blank (display frame counter, polled every 1 ms
    // while the screen moves); the 60 Hz timer is only the fallback (counter missing or not moving: panel off / PSR)
    uint32_t xeFc, xeFcLine;                    // last frame counter, scanline when we saw it change
    bool xeFcOK;
    uint64_t xeFcSeen, xeFcStuckSince, xeRealVbl, xeTimerVbl, xeLineSum, xeLineMax;
    bool readFrame(uint32_t *fc, uint32_t *line);
    uint32_t xeInitRes, xeDestroyRes;           // v0.56.3: counted
};

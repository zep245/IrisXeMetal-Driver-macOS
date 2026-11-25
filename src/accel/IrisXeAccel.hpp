// IrisXeAccel - milestone 1: our subclass of Apple's GPU accelerator base class
// (IOGraphicsAccelerator2, IOAcceleratorFamily2).
//   step 1a/1b: IrisXeAccelTest checks the vtable layout (-irisxeacceltest)
//   step 1c   : IrisXeAccelerator really starts on the Iris Xe PCI device
//               (-irisxeaccel). Metal is NOT told about it yet (no
//               MetalPluginName), and it is not registered unless
//               -irisxeaccelreg is also set.
#pragma once
#include "apple/IOGraphicsAccelerator2.h"
#include "IrisXeAccelClasses.hpp"

class IrisXeAccelerator : public IOGraphicsAccelerator2 {
    OSDeclareDefaultStructors(IrisXeAccelerator)
public:
    // IOService
    virtual IOService *probe(IOService *provider, SInt32 *score) override;   // slot 185
    virtual bool start(IOService *provider) override;                        // slot 186

    // the 17 required (pure virtual) methods
    virtual IRISXE_SLOT_RET getStampMemory(unsigned int *) override;
    virtual IRISXE_SLOT_RET newEventMachine() override;
    virtual IRISXE_SLOT_RET createUserGPUTask() override;
    virtual IRISXE_SLOT_RET populateAccelConfig(IOAccelConfig *) override;
    virtual IRISXE_SLOT_RET configureDevice(IOPCIDevice *) override;
    virtual IRISXE_SLOT_RET teardownDevice(IOPCIDevice *) override;
    virtual IRISXE_SLOT_RET createKernelGPUTask() override;
    virtual IRISXE_SLOT_RET newDisplayMachine() override;
    virtual IRISXE_SLOT_RET newGLContext() override;
    virtual IRISXE_SLOT_RET newCLContext() override;
    virtual IRISXE_SLOT_RET newDevice() override;   // slot 325 (v0.68.2: IrisXeDevice on the video test boot)
    virtual IRISXE_SLOT_RET newContext(unsigned int type) override;   // slot 333 (v0.68.0: video contexts 0x100..0x102)
    virtual IRISXE_SLOT_RET newSurface() override;
    virtual IRISXE_SLOT_RET new2DContext() override;
    virtual IRISXE_SLOT_RET newVideoContext() override;
    virtual IRISXE_SLOT_RET newSysMemory() override;
    virtual IRISXE_SLOT_RET newVidMemory() override;
    virtual IRISXE_SLOT_RET newResource() override;
    virtual IRISXE_SLOT_RET newMemoryMap() override;
    // not exported by Apple (internal), so they must be ours
    virtual IRISXE_SLOT_RET newCommandQueue() override;          // slot 350 (milestone 2c)
    virtual IRISXE_SLOT_RET newDisplayPipe() override;           // slot 331 (v0.51.42, reports the step)
    virtual IRISXE_SLOT_RET tmpTotalVRAM() override;
    virtual IRISXE_SLOT_RET totalTextureMemory() override;

    // ours (used by IrisXeEventMachine / IrisXeDisplayMachine)
    volatile UInt32 *stampBase() const { return fStampVA; }
    void linkDisplays(const char *why);           // v0.40.0 (v0.51.40: also IrisXeFBs that start later)
    void linkOne(IOService *fb);                  // v0.51.40
    void note(const char *step, bool ok);       // progress -> IrisXe-AccelStart

    // GPU core in IrisXeCore (milestone 2c), reached via callPlatformFunction
    // space 0 = the shared GPU address space; others = one per process
    // (milestone 2e, IrisXeSpaceABI.h; an older IrisXeCore only has space 0)
    bool coreMapPages(UInt32 space, UInt64 va, const UInt64 *pa, UInt32 count);
    void coreUnmapPages(UInt32 space, UInt64 va, UInt32 count);
    int  coreRunBlit(UInt32 space, const UInt32 *cmds, UInt32 count, UInt32 timeoutMS);
    bool coreIsMapped(UInt32 space, UInt64 va, UInt64 len);
    // milestone 3a: compute job (SIMD8 kernel, see IrisXeSpaceABI.h). -12 = core too old
    int  coreDraw(UInt32 space, const struct XeDrawDesc *d, UInt32 timeoutMS);   // milestone 5c
    int  coreDraws(UInt32 space, const struct XeDrawDesc *d, UInt32 count, UInt32 timeoutMS,
                   UInt32 *asyncSeq = nullptr);   // v0.49.0; v0.63.0: asyncSeq: leave the last job running, its seq
    int  coreWaitSeq(UInt32 seq, UInt32 timeoutMS);
    int  coreVcsTest(char *rep, UInt32 len);          // v0.67.0: video engine test (IrisXeCore ABI 11)   // v0.63.0: 0 ended, -3 timeout, -9 GPU reset, -2 core too old
    int  coreVideoRun(UInt32 space, UInt64 batchVA, UInt32 timeoutMS, UInt32 *usOut);   // v0.69.0 (IrisXeCore ABI 12)
    int  coreVideoSelfTest(UInt32 rounds, bool hang, char *rep, UInt32 len);
    OSObject *eventMachine() const { return fEM; }    // v0.63.0
    volatile UInt32 *stampEnd() const { return fStampVA ? fStampVA + 0x1000 / 4 : nullptr; }
    int  coreCompute(UInt32 space, const UInt32 *kernel, UInt32 dwords, const UInt32 ctd[8],
                     const UInt32 groups[3], UInt32 tpg, UInt32 timeoutMS, UInt32 info = 0,
                     UInt64 codeVA = 0, UInt32 ntex = 0, const struct XeTexDesc *tex = nullptr,
                     UInt32 nsamp = 0, const struct XeSampDesc *samp = nullptr,
                     const UInt32 *tpgHD = nullptr, const UInt32 *gridYZ = nullptr);   // v0.51.17 (ABI 8)
    int  coreScan(struct XeScanArgs *a);          // v0.59.0: result (0 ok), -2 = IrisXeCore too old
    int  coreABI();                               // v0.58.7: IrisXeCore's IRISXE_SPACE_ABI, 0 = unknown (not cached)
    bool highVA() const { return fHighVA; }       // v0.58.7: tasks get GPU VA 1..4 GB
    int  coreSpaceCreate();                       // > 0 new space, 0 = old core (shared only), < 0 failed
    void coreSpaceDestroy(UInt32 space);
    void countMetal(int what, UInt64 n);          // IrisXe-AccelMetal statistics
    static IrisXeAccelerator *sInstance;

private:
    char fLinkWhy[64]; unsigned fLinked; IONotifier *fFBNotifier;   // v0.51.40
    char fLinkPath[512];
    OSObject *makeTask(const char *who);
    IOService *findCore();
    void countMem(int kind);

    UInt8 _apple[3544];     // Apple's part is 3544 bytes (measured); padding only

    // ---- our fields (after Apple's part, Apple never touches them) ----
    IOService *fProvider;
    IOPCIDevice *fPCI;
    IOBufferMemoryDescriptor *fStampMem;
    volatile UInt32 *fStampVA;
    OSObject *fEM;          // v0.63.0: our event machine (Apple's IOAccelEventMachineFast2)
    UInt32 fTasks;
    char fLog[480];
    UInt32 fMemCount[4];
    IOService *fCore;
    const OSSymbol *fSymMap, *fSymUnmap, *fSymBlit, *fSymCheck;
    UInt64 fMetal[12];       // pages mapped, map failures, blits ok, blits failed, unmapped, other cmds,
                            // skipped (not mapped), compute ok, compute failed
    const OSSymbol *fSymSpace;
    const OSSymbol *fSymScan;          // v0.59.0
    bool fHighVA;
    int fSpaceABI;          // -1 unknown, 0 old core, else IRISXE_SPACE_ABI
};

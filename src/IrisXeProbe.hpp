// IrisXeProbe - passive side-by-side service. Loads next to any other GPU driver
// (own IOMatchCategory), only READS hardware state and logs it. Never writes
// display registers, never touches the display. With -irisxegt it also
// wakes the GPU core (forcewake) briefly and reports the engines.
#ifndef IRISXE_PROBE_HPP
#define IRISXE_PROBE_HPP

#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>

class IrisXeProbe : public IOService {
    OSDeclareDefaultStructors(IrisXeProbe)
    typedef IOService super;

public:
    virtual IOService *probe(IOService *provider, SInt32 *score) override;
    virtual bool start(IOService *provider) override;
    void gtReport(volatile UInt8 *mmio);
    void gtBlitTest(volatile UInt8 *mmio, UInt64 mmioLen);
    void gtPPGTTTest(volatile UInt8 *mmio, UInt64 mmioLen);
    IOBufferMemoryDescriptor *p3Mem = nullptr;
    IOBufferMemoryDescriptor *gtMem = nullptr;   // kept: engine points at it

    // --- GPU core (step 4, -irisxegpu): IrisXeGPU.cpp ---
    virtual IOReturn setPowerState(unsigned long state, IOService *device) override;
    bool     gpuStart(IOPCIDevice *pci);
    void     gpuBuildTables();
    void     gpuWriteGGTT();
    IOReturn gpuBringUpGated();
    IOReturn gpuSleepGated();
    bool     gpuForcewake(bool on);
    bool     gpuSubmit(const UInt32 *cmds, UInt32 count, bool render);
    void     rcsSubmitPipeControl();
    void     gpuSubmitDemo();
    void     gpuSubmitHeartbeat();
    void     gpuCheckDone(bool viaIRQ);
    void     gpuStatus(const char *err);
    static bool gpuIrqFilter(OSObject *owner, IOFilterInterruptEventSource *src);
    static void gpuIrqHandler(OSObject *owner, IOInterruptEventSource *src, int count);
    static void gpuTimerFired(OSObject *owner, IOTimerEventSource *sender);

    // v0.67.0: the video engine (VCS0, the media engine's VDBox 0): a one-shot test (kXeSpaceVcsTest)
    IOReturn vcsTest(char *rep, UInt32 repLen);
    // v0.69.0: video decode jobs on VCS0 (kXeSpaceVideoRun) and the stage A self-test (kXeSpaceVideoSelfTest)
    bool     vcsPrepare();
    bool     vcsReset();
    void     vcsPublish();
    int      vcsRunJob(UInt64 bbAddr, bool ppgtt, UInt32 timeoutMS, UInt32 *usOut);
    int      vcsSelfTest(UInt32 rounds, bool hang, char *rep, UInt32 repLen);
    IOBufferMemoryDescriptor     *vcsMem   = nullptr;
    volatile UInt32              *vcsVA    = nullptr;
    UInt64                        vcsPhys  = 0;
    UInt32                        vcsTests = 0, vcsOK = 0;
    IOMemoryMap                  *gpuMap   = nullptr;
    volatile UInt8               *gpuMMIO  = nullptr;
    IOBufferMemoryDescriptor     *gpuMem   = nullptr;
    volatile UInt32              *gpuVA    = nullptr;
    UInt64                        gpuPhys  = 0;
    IOWorkLoop                   *gpuWL    = nullptr;
    IOTimerEventSource           *gpuTimer = nullptr;
    IOFilterInterruptEventSource *gpuIRQ   = nullptr;
    int      gpuIrqIndex = -1;
    bool     gpuIrqMSI = false;
    volatile UInt32 gpuIrqTotal = 0, gpuIrqUser = 0, gpuIrqDisplay = 0;
    volatile bool   gpuIrqStorm = false;
    UInt32   fbSurf = 0, fbStride = 0, fbHeight = 0;
    bool     fbOK = false;
    UInt32   gpuRingTail = 0, gpuSeqSubmitted = 0, gpuSeqDone = 0;
    bool     gpuCtxFresh = true, gpuBusy = false, gpuUp = false, gpuWasUp = false, gpuHung = false;
    bool     gpuRC6On = false;
    UInt32   gpuDemoLeft = 0,    /* v0.68.7: colour bars only with -irisxedemo */ gpuHeartbeats = 0, gpuDoneIRQ = 0, gpuDonePoll = 0;
    UInt32   gpuLastJobUS = 0, gpuBusyMS = 0, gpuWakes = 0, gpuRC6Pct = 0;
    UInt32   gpuDisplayIntCtl = 0, gpuLastRC6 = 0;
    UInt64   gpuJobStart = 0, gpuLastRC6Time = 0;
    bool     gpuBusyRender = false;
    // render engine (step 5)
    IOBufferMemoryDescriptor *rcsMem = nullptr;
    volatile UInt32 *rcsVA = nullptr;
    UInt64   rcsPhys = 0;
    IOBufferMemoryDescriptor *drwMem = nullptr;   // milestone 5c: draw state pages
#define XE_BATCH_SLOTS 8                              // v0.49.0: draws per GPU job (state copies)
#define XE_BATCH_MAX   64                             //          draws per call
    IOBufferMemoryDescriptor *slotMem[XE_BATCH_SLOTS] = {};
    volatile UInt32 *slotVA[XE_BATCH_SLOTS] = {};
    // v0.60.1: a copy of each slot's draw state (and of the single-draw pages, index XE_BATCH_SLOTS) in normal cached
    // memory. The draw is built there, then copied to the slot in one go: the slots are write-combining memory, and
    // every read the build did there (x |= y, read-modify-write) went all the way to RAM. -irisxenoshadow: off.
    UInt32  *mtlShadow[XE_BATCH_SLOTS + 1] = {};
    bool     mtlShadowReady[XE_BATCH_SLOTS + 1] = {};
    uint64_t rcsDoneAt = 0;                                // v0.60.1: when the last render job was seen finished
    UInt64   slotPhys[XE_BATCH_SLOTS] = {};
    bool     mtlBatchOK = false;
    UInt32   mtlBatchSlots = 4;                       // v0.49.3: IrisXe-BatchCfg (v0.57.0: 4 by default; 6+ hung the GPU)
    bool     mtlBatchBarrier = true;
    volatile UInt32 *drwVA = nullptr;
    UInt64   drwPhys = 0;
    bool     rcsOn = false, rcsCtxFresh = true, rcsFirstDone = false;
    UInt32   rcsRingTail = 0, rcsSeqSubmitted = 0, rcsSeqDone = 0, rcsPCSent = 0, rcsPCOK = 0;
    UInt32   rcsLayoutEnd = 0;
    // Metal (milestone 2c, IrisXeMetalAPI.cpp): GPU page tables for Metal
    // buffers (GPU VA 4 KB .. 256 MB) and blitter jobs from IrisXeAccel, which
    // calls us through callPlatformFunction (no kext link dependency).
    virtual IOReturn callPlatformFunction(const OSSymbol *functionName, bool waitForFunction,
                                          void *p1, void *p2, void *p3, void *p4) override;
    bool     mtlMapPages(UInt64 va, const UInt64 *pa, UInt32 count);
    void     mtlUnmapPages(UInt64 va, UInt32 count);
    int      mtlRunBlit(const UInt32 *cmds, UInt32 count, UInt32 timeoutMS, UInt32 *seqOut = nullptr);
    // milestone 2e: one GPU address space per Metal process (IrisXeSpaceABI.h).
    // A space is up to 128 page tables (allocated as needed) for GPU VA
    // 0 .. 256 MB; before a job runs, PD entries 0..127 are pointed at the
    // job's space. Space 0 is the shared table above (old calls).
#define MTL_SPACE_PTS 2048                            // v0.58.7: page tables per space, by GPU VA / 2 MB: 0..255 = 0..512 MB
                                                      // (PD 0, switched per job), 512..2047 = 1..4 GB (the space's own PDs)
#define MTL_HI_PDS    3                               // v0.58.7: PDP entries 1..3 = GPU VA 1..4 GB, one PD page each
    struct MtlSpace {
        IOBufferMemoryDescriptor *ptMem[MTL_SPACE_PTS];
        volatile UInt32          *pt[MTL_SPACE_PTS];
        UInt64                    ptPhys[MTL_SPACE_PTS];
        UInt32                    mapped, tables;
        // v0.58.7: the space's own page directories for GPU VA 1..4 GB (made when first needed); before a job
        // runs, PDP entries 1..3 point at them (was: only 512 MB per process -> WindowServer's newBuffer failed)
        IOBufferMemoryDescriptor *pdMem[MTL_HI_PDS];
        volatile UInt32          *pd[MTL_HI_PDS];
        UInt64                    pdPhys[MTL_HI_PDS];
        UInt32                    hiMapped, peakMapped;
        // v0.57.6: which pages are mapped, in normal (cached) memory. The page tables themselves are uncached
        // memory (the GPU reads them directly): every CPU read of an entry went all the way to RAM, and the
        // check before each draw read every entry of every texture and render target (4096 for a 16 MB one).
        UInt64                    present[MTL_SPACE_PTS * 512 / 64];
    };
    MtlSpace *mtlSpaces[32] = {};
    UInt32   mtlCur = 0;                              // space the PD points at now
    UInt32   mtlLastSeq = 0;                          // last Metal job submitted
    bool     mtlLastDone = true;
    IOLock  *mtlJobLock = nullptr;                    // one Metal job at a time
    IOLock  *gpuDoneLock = nullptr;                   // v0.45.0: job-end wakeups
    UInt32   gpuDoneEvent = 0;
    UInt32   gpuWait(bool render, UInt32 seq, uint64_t t0);
    void     gpuWakeWaiters();
    UInt32   gpuFreqReq = 0, gpuFreqRP0 = 0, gpuFreqRPn = 0, gpuTsPerMs = 0;   // v0.45.1: GPU clock (16.67 MHz units)
    void     gpuFreqReport();
    void     gpuMocsSetup();                          // v0.49.1
    void     gpuRingFreqSetup();                      // v0.57.4: LLC/ring clock table (i915 intel_llc.c)
    char     gpuRingMsg[160] = {};
    void     gpuPatSetup();                           // v0.57.5: PPGTT PAT table (i915 tgl_setup_private_ppat)
    char     gpuPatMsg[200] = {};
    void     gpuHashSetup();                          // v0.57.8: pixel pipe balancing (Mesa genX_init_state.c, Gfx12.0)
    void     gpuHashPublish();
    bool     gpuHashOK = false, gpuHashOn = false;    // a table is needed and built / used in draw jobs
    UInt32   gpuHashDW[14] = {};                      // 3DSTATE_SUBSLICE_HASH_TABLE
    UInt32   gpuHashPipes[3] = {};                    // dual subslices per pixel pipe
    UInt32   gpuHashDSS = 0;
    bool     gpuDrawWC = false;                       // v0.57.9: ring / draw pages write-combining (-irisxeuc: uncached)
    UInt32   gpuMOCSBefore[2] = {};
    bool     gpuMOCSSet = false;
    IOReturn gpuFreqSetGated();
    UInt32   mtlSpaceStat[5] = {};                    // created, destroyed, switches, live, leaked
    IOReturn mtlSpaceOp(UInt32 op, void *args);
    int      mtlRunCompute(const struct XeSpaceArgs *a, UInt32 *seqOut);
    int      mtlRunDraw3D(const struct XeSpaceArgs *a, UInt32 *seqOut);   // milestone 5c
    int      mtlCheckDraw(const struct XeDrawDesc *d);                  // v0.49.0
    int      mtlBuildDraw(const struct XeDrawDesc *d, volatile UInt32 *base, UInt32 baseVA, UInt32 &n, UInt32 cap);
    UInt32   mtlDrawPrologue();
    UInt32   mtlDrawEpilogue(UInt32 n);
    int      mtlSubmitDrawJob(const struct XeSpaceArgs *a, UInt32 *seqOut, UInt32 n, uint64_t tIn, UInt32 nd,
                              UInt32 drawsHere, UInt32 clearsHere, const struct XeDrawDesc *d, bool async = false);
    // v0.63.0: asynchronous draw jobs. The last job of an IrisXeAccel draw call may be left running (kXeSpaceDraw
    // with tpg bit 0); its seq goes back in va. Whoever takes mtlJobLock next finishes it first (mtlFinishAsync):
    // the next job's state slots, a space switch, an unmap or a destroy never touch what the GPU still uses.
    int      mtlJobFinished(UInt32 seq, uint64_t tIn, uint64_t t0, uint64_t tSub, UInt32 n, UInt32 nd,
                            UInt32 drawsHere, UInt32 clearsHere, const struct XeDrawDesc *d, UInt32 timeoutMS, UInt32 resets0);
    int      mtlFinishAsync();                                     // mtlJobLock held
    bool     mtlAsyncPend = false;
    UInt32   mtlAsyncSeq = 0, mtlAsyncN = 0, mtlAsyncND = 0, mtlAsyncDraws = 0, mtlAsyncClears = 0, mtlAsyncTimeout = 0;
    uint64_t mtlAsyncTIn = 0, mtlAsyncT0 = 0, mtlAsyncTSub = 0;
    void    *mtlAsyncDesc = nullptr;                               // a copy of the job's first XeDrawDesc (statistics)
    UInt32   mtlAsyncJobs = 0, mtlAsyncLate = 0;
    UInt32   mtlPSTest = 0, mtlPSTestDraws = 0;                     // v0.63.2: fill-rate test shaders                    // left running / still running when the next came
    bool     mtlBatchTemplatesOK();
    UInt32   mtlDraws = 0, mtlClears = 0, mtlDrawFails = 0, mtlDbgMode2 = 0;
    bool     mtlSwitchSpace(UInt32 space, int *err);      // mtlJobLock held
    static IOReturn mtlTrySubmitRenderGated(OSObject *owner, void *cmds, void *count, void *seqOut, void *);
    UInt32   mtlComputeJobs = 0, mtlComputeFails = 0, mtlComputeBarrier = 0, mtlComputeSplit = 0, mtlComputeBig = 0, mtlComputeTex = 0;
    bool     mtlLastRender = false;
    volatile UInt32 *mtlTable(UInt32 space, UInt64 va, bool create);
    void     mtlPointPD(UInt32 space);
    bool     mtlVAOK(UInt64 va, UInt64 len) const;          // v0.58.7: inside 0..512 MB or 1..4 GB
    void     mtlSpacePublish();                            // v0.58.7: IrisXe-GPUSpace
    UInt32   mtlMapFails = 0, mtlSpacePubTick = 0;
    // v0.59.0: direct scanout (IrisXeSpaceABI.h, kXeScan*)
    IOReturn scanOp(void *args);
    void     scanPublish();
    UInt32   scanGen = 1, scanPteFlags = 1, scanHomeSurf = 0, scanHomeStride = 0;
    bool     scanHomeSaved = false;
    UInt32   scanMaps = 0, scanFlips = 0, scanHomes = 0;
    static IOReturn mtlTrySubmitGated(OSObject *owner, void *cmds, void *count, void *seqOut, void *);
    IOBufferMemoryDescriptor *mtlPTMem = nullptr;
    volatile UInt32 *mtlPT = nullptr;              // 128 page tables, 512 entries each
    UInt64   mtlPTPhys = 0;
    IOLock  *mtlLock = nullptr;
    UInt32   mtlMapped = 0, mtlJobs = 0, mtlJobFails = 0;

    // compute shader test (step 6, -irisxecompute)
    bool     cptWanted = false, cptSent = false, cptPending = false;
    UInt32   cptGood = 0;
    char     cptResult[96] = {0};

    // milestone 5a: first 3D draw (IrisXeRender.inc), started from user space
    // by setting the IrisXe-DrawTest property (irisxe-metaltest step [18])
    virtual IOReturn setProperties(OSObject *properties) override;
    int      mtlRunDraw(char *out, size_t outLen);
    int      mtlDrawJob(const UInt32 *cmds, UInt32 count, UInt32 *ms);
    int      mtlRunDrawC(const struct XeDrawTestCfg *g, char *out, size_t outLen);
    UInt32   drawRuns = 0, drawOK = 0, drawCRuns = 0;
    UInt32   jobRingStart = 0, jobCount = 0;          // v0.32.2: last job in a ring (hang report)
    UInt64   lastVS = 0, lastPS = 0;                  // v0.42.2: the last draw (hang report: which pipeline)
    struct JobRec { UInt32 usec, engine, space, dwords, first, seq; };   // v0.43.0: the last 32 jobs (hang report)
    JobRec   jobHist[32] = {};
    UInt32   jobHistN = 0, mapsDuringJob = 0, waApplied = 0;
    UInt32   gpuResets = 0, gpuResetFails = 0;                // v0.44.0
    UInt32   gpuRingTooBig = 0;                               // v0.68.33: jobs refused (more than half the ring)
    bool     gpuResetGated();
    UInt32   lastVerts = 0, lastInst = 0, lastIdx = 0, lastRT = 0, lastTex = 0, lastVB = 0;
};

#endif

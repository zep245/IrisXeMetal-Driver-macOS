// IrisXeSpaceABI.h - how IrisXeAccel asks IrisXeCore for per-process GPU
// address spaces (milestone 2e). Shared by both kexts; they talk through
// IOService::callPlatformFunction("IrisXeSpace", false, (void*)op, &args, 0, 0)
// so there is no link dependency between them.
//
// Each Metal process gets its own set of page tables for GPU VA 0 .. 256 MB.
// Space 0 is the old shared one (still used by the v0.21/0.22 calls).
#pragma once
#include <libkern/OSTypes.h>

#define IRISXE_SPACE_ABI     11     // 11 = v0.67.0: kXeSpaceVcsTest (video engine test, report: va = buffer, len = size)
                                   // 10 = v0.63.0: kXeSpaceDraw tpg bit 0 = leave the last job running (seq in va); kXeSpaceWaitSeq
// #define IRISXE_SPACE_ABI     9      // 9 = v0.58.7: a space also maps GPU VA 1 GB .. 4 GB (IRISXE_HI_VA..IRISXE_HI_END)
                                   // 8 = v0.51.17: compute thread groups of W x H x D (tpgHD, gridYZ)
                                   // 7 = v0.49.0: kXeSpaceDraw with count = several draws (an array) in one job
#define IRISXE_SPACE_ABI_6   6      // 2 = v0.24.0: + kXeSpaceCompute; 3 = v0.28.0: compute len = threadgroup memory / barrier
                                   // 4 = v0.29.0: compute va = kernel code in the process's GPU memory
                                   // 5 = v0.30.0: compute textures and samplers (milestone 4)
                                   // 6 = v0.32.0: kXeSpaceDraw (milestone 5c, IrisXeDrawABI.h)
#define IRISXE_MAX_SPACES    32
#define IRISXE_LO_END        0x20000000ULL     // GPU VA 0 .. 512 MB: every space (and the shared space 0)
#define IRISXE_HI_VA         0x40000000ULL     // ABI 9: GPU VA 1 GB .. 4 GB - 64 KB: process spaces (not space 0)
#define IRISXE_HI_END        0xFFFF0000ULL     // (the shaders use 32-bit addresses; state buffer sizes end at 4 GB - 4 KB)

enum {
    kXeSpaceCreate   = 1,   // out: space
    kXeSpaceDestroy  = 2,   // in:  space
    kXeSpaceMap      = 3,   // in:  space, va, pa[count], count
    kXeSpaceUnmap    = 4,   // in:  space, va, count
    kXeSpaceIsMapped = 5,   // in:  space, va, len          result 0 = every page mapped
    kXeSpaceRun      = 6,   // in:  space, cmds[count], count, timeoutMS (blitter job; result as mtlRunBlit)
    kXeSpaceVersion  = 7,   // out: result = IRISXE_SPACE_ABI
    kXeSpaceVcsTest  = 11,  // v0.67.0 (ABI 11): run a tiny job on the video engine VCS0; va = char *report, len = its size
    kXeSpaceWaitSeq  = 10,  // in: count = seq of a render job, timeoutMS: wait until it ended (ABI 10); result 0, -3 timeout, -9 GPU reset
    kXeSpaceDraw     = 9,   // in:  space, draw (IrisXeDrawABI.h), timeoutMS: a Metal draw (milestone 5c); result as compute,
                            //      -8 = bad description
    kXeSpaceCompute  = 8,   // in:  space, cmds = kernel (count dwords, <= 992 since v0.26.0; 512 before), ctd[8], groups[3], tpg, timeoutMS
                            //      SIMD8 GPGPU walker on the render engine (milestone 3a). result:
                            //      0 ok, -1 not ready, -2 could not submit, -3 timeout, -6 no render engine, -7 bad size
                            //      ABI 3: len = kXeComputeSLM bits (threadgroup memory bytes) | kXeComputeBarrier
                            //      ABI 4: va != 0: the kernel is at that GPU address of the space (64-byte aligned,
                            //      <= kXeKernelMaxDW dwords) instead of in cmds
};
#define kXeComputeSLM      0x00FFFFFFu      // bytes of threadgroup (shared local) memory, <= 65536
#define kXeComputeBarrier  0x80000000u      // the kernel uses threadgroup barriers
#define kXeKernelMaxDW     1048576u         // kernel in GPU memory: up to 4 MB (v0.51.9; was 256 KB)

// milestone 4 (ABI 5): textures and samplers of a compute job. The shader
// uses binding table index k for texture slot k and sampler k for sampler slot k.
#define kXeMaxTextures     8
#define kXeMaxSamplers     8
struct XeTexDesc {          // a linear (untiled) texture in the space's GPU memory; va == 0: slot unused
    UInt64 va;
    UInt32 width, height;   // texels
    UInt32 layers;          // 1, or the array length of a 2D array
    UInt32 pitch;           // bytes per row (multiple of 64)
    UInt32 qpitch;          // rows from one array layer to the next (multiple of 4)
    UInt32 format;          // Gen surface format (ISL_FORMAT_*)
    UInt32 array;           // 1 = 2D array texture
    UInt32 pad;
};
struct XeSampDesc {         // used == 0: slot unused
    UInt32 used;
    UInt8  minFilter, magFilter, mipFilter;   // 0 nearest, 1 linear (mip: 0 none, 1 nearest, 3 linear)
    UInt8  addrU, addrV, addrW;               // Gen texture coordinate mode (TCM_*)
    UInt8  normalized;                        // 0 = pixel coordinates
    UInt8  pad[5];
};

struct XeSpaceArgs {
    UInt32        space;
    UInt32        count;
    UInt64        va;
    UInt64        len;
    const UInt64 *pa;
    const UInt32 *cmds;
    UInt32        timeoutMS;
    SInt32        result;   // 0 = ok, < 0 error
    // compute (ABI 2). Kernel registers on entry: g0 = thread payload (g0.1 =
    // thread group X), g1 = ctd[0..7] (same for every thread), g2.0 = index of
    // the thread's first lane inside its thread group (8 * thread number).
    UInt32        ctd[8];
    UInt32        groups[3];
    UInt32        tpg;      // threads per thread group (x only), 1 .. 512
    // ABI 5
    UInt32        ntex, nsamp;
    XeTexDesc     tex[kXeMaxTextures];
    XeSampDesc    samp[kXeMaxSamplers];
    // ABI 6
    const struct XeDrawDesc *draw;
    // ABI 8 (v0.51.17): a thread group is W x H x D threads (tpg = W H D; 0 = H, D 1) and the grid has
    // gridYZ[0] rows, gridYZ[1] slices (0: the groups fill them). The kernel gets per-thread data
    // g2.1..g2.5 = W, H, grid rows, grid slices, D (the compiler finds x, y, z from the thread index)
    UInt32        tpgHD[2];
    UInt32        gridYZ[2];
};

// ---- v0.59.0: direct scanout - the panel shows WindowServer's image itself (no CPU copy) ----
// IrisXeAccel's display pipe calls IOService::callPlatformFunction("IrisXeScanout", false, &args, 0, 0, 0).
// Each image gets one of XE_SCAN_SLOTS fixed GGTT windows (written once); a frame is then one PLANE_SURF write,
// which the display takes at its next vertical blank (no tearing).
#define XE_SCAN_ABI        2      // 2 = v0.59.2: kXeScanFrame
#define XE_SCAN_SLOTS      4
#define XE_SCAN_GGTT       0xE0000000u      // GGTT window of slot 0 (unused by anything else)
#define XE_SCAN_SLOT_SIZE  0x02000000u      // 32 MB per slot (2560x1600x4 = 16 MB)
enum {
    kXeScanVersion = 1,   // out: result = XE_SCAN_ABI, gen
    kXeScanMap     = 2,   // in: slot, pa[count] (4 KB pages)            out: addr = the slot's GGTT address, gen
    kXeScanFlip    = 3,   // in: addr, stride (bytes, multiple of 64), gen: show that image from the next vblank
    kXeScanLive    = 4,   // out: addr = GGTT address the panel shows now (PLANE_SURFLIVE)
    kXeScanHome    = 5,   // the panel's own framebuffer again (as before the first flip)
    kXeScanFrame   = 6,   // v0.59.2: out: addr = the display's frame counter (PIPE_FRMCOUNT_A, +1 at every vertical
                          //          blank), stride = the scanline being shown now (PIPEDSL_A)
};
struct XeScanArgs {
    UInt32        op, slot;
    const UInt64 *pa;
    UInt32        count;
    UInt32        stride;
    UInt32        addr;
    UInt32        gen;       // changes when IrisXeCore re-initialises the GPU (wake): every slot must be mapped again
    SInt32        result;    // 0 ok, < 0 error (-1 bad args, -2 no GPU, -3 stale gen)
};

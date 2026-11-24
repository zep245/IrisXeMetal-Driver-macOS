// IrisXeMTLDriver.bundle - our Metal driver (user space), milestone 1 step 2a.
//
// Metal creates a GPU's MTLDevice from the driver bundle the accelerator names
// (MetalPluginName / MetalPluginClassName), with
//     [[IrisXeMTLDevice alloc] initWithAcceleratorPort:<io_service_t>]
// Our device is a subclass of Apple's MTLIOAccelDevice (Metal.framework). That
// class is private, so the subclass is created at run time when the bundle is
// loaded (constructor below), not at compile time. Built without ARC (plain
// retain/release): the init method below hands objects through unchanged.
//
// Step 2a/2b: only "the device exists": init (Apple's base class opens the
// connections to our kernel accelerator), name strings. No command queues,
// buffers or shaders yet.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <IOSurface/IOSurfaceRef.h>
#include <sys/sysctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <mach/mach.h>
#include <dlfcn.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <os/log.h>
#include <pthread.h>

#define XE_NAME   @"Intel Iris Xe Graphics (IrisXe)"

static os_log_t gLog;
static Class gSuper;     // MTLIOAccelDevice

// v0.54.4: which kernel object WindowServer's port number stands for (port numbers are per process, so they can only
// be compared by registry ID). IOKit is called through dlsym: the bundle does not link it.
static void xe_describePort(const char *what, uint32_t port) {
    typedef int (*GetIDFn)(uint32_t, uint64_t *);
    typedef int (*GetClassFn)(uint32_t, char *);
    typedef int (*GetNameFn)(uint32_t, char *);
    GetIDFn getID = (GetIDFn)dlsym(RTLD_DEFAULT, "IORegistryEntryGetRegistryEntryID");
    GetClassFn getClass = (GetClassFn)dlsym(RTLD_DEFAULT, "IOObjectGetClass");
    GetNameFn getName = (GetNameFn)dlsym(RTLD_DEFAULT, "IORegistryEntryGetName");
    uint64_t id = 0; char cls[128] = "?", name[128] = "?";
    int e1 = getID ? getID(port, &id) : -1;
    int e2 = getClass ? getClass(port, cls) : -1;
    if (getName) getName(port, name);
    mach_port_type_t type = 0;
    kern_return_t kt = mach_port_type(mach_task_self(), port, &type);
    os_log(gLog, "IrisXeMTL: %{public}s %u: class %{public}s, name %{public}s, registry ID 0x%llx (id err 0x%x, class err 0x%x, port type 0x%x/0x%x)",
           what, port, cls, name, id, e1, e2, type, kt);
}
// v0.54.6: who asks our device for its acceleratorPort inside WindowServer, and with which display accelerator.
// SkyLight's CompositorMetal::new_compositor(id) gives a display the GPU compositor only when [dev acceleratorPort]
// equals the display's accelerator id (its first argument, kept at [rbp-0x1e0] in the 15.7/15.8 build). We only READ
// the caller's stack: no SkyLight function is called, and no message is sent to that port (inside one process, the same
// kernel object always has the same port number, so comparing numbers is enough). Logged for the first 40 calls, WindowServer only.
static uint32_t xe_acceleratorPort(id self, SEL _cmd) {
    struct objc_super sup = { self, gSuper };
    uint32_t r = ((uint32_t (*)(struct objc_super *, SEL))objc_msgSendSuper)(&sup, _cmd);
    static int logged, inWS = -1;
    if (inWS < 0) inWS = getprogname() && !strcmp(getprogname(), "WindowServer");
    if (!inWS || logged >= 40) return r;
    logged++;
    void *ra = __builtin_return_address(0);
    Dl_info di; memset(&di, 0, sizeof(di));
    const char *who = dladdr(ra, &di) && di.dli_sname ? di.dli_sname : "?";
    if (!strcmp(who, "_ZN15CompositorMetal14new_compositorEj")) {
        void **fp = (void **)__builtin_frame_address(0);
        uint32_t want = fp && fp[0] ? *(uint32_t *)((char *)fp[0] - 0x1e0) : 0;
        mach_port_type_t t = 0;
        kern_return_t kt = want ? mach_port_type(mach_task_self(), want, &t) : KERN_INVALID_NAME;
        os_log(gLog, "IrisXeMTL: new_compositor asks for display accelerator %u (port type 0x%x/0x%x); our acceleratorPort %u -> %{public}s",
               want, t, kt, r, want == r ? "MATCH: GPU compositor for that display" : "different");
    } else {
        // dladdr only sees exported names, so new_compositor may show as a neighbour: log the same stack slot anyway
        // (in 15.7.9 the call returns at new_compositor+0xd9). Reading it is harmless: it is our own thread's stack.
        void **fp = (void **)__builtin_frame_address(0);
        bool sky = di.dli_fname && strstr(di.dli_fname, "SkyLight");
        uint32_t cand = sky && fp && fp[0] ? *(uint32_t *)((char *)fp[0] - 0x1e0) : 0;
        mach_port_type_t t = 0;
        kern_return_t kt = cand ? mach_port_type(mach_task_self(), cand, &t) : KERN_INVALID_NAME;
        os_log(gLog, "IrisXeMTL: acceleratorPort %u asked by %{public}s +0x%lx (%{public}s); slot [rbp-0x1e0] = %u (port type 0x%x/0x%x)",
               r, who, (unsigned long)((char *)ra - (char *)di.dli_saddr), sky ? "SkyLight" : (di.dli_fname ? di.dli_fname : "?"),
               cand, t, kt);
    }
    return r;
}

static id xe_initWithAcceleratorPort(id self, SEL _cmd, uint32_t port) {
    os_log(gLog, "IrisXeMTL: initWithAcceleratorPort:%u (super %{public}s)", port, class_getName(gSuper));
    if (getprogname() && !strcmp(getprogname(), "WindowServer")) {          // v0.54.4: diagnostic, WindowServer only
        xe_describePort("initWithAcceleratorPort", port);
        // v0.54.5: the v0.54.4 delayed SLSMainDisplayID call is gone - SkyLight's client calls assert inside WindowServer.
    }
    struct objc_super sup = { self, gSuper };
    id r = ((id (*)(struct objc_super *, SEL, uint32_t))objc_msgSendSuper)(&sup, _cmd, port);
    os_log(gLog, "IrisXeMTL: MTLIOAccelDevice init returned %p", r);
    return r;
}

static NSString *xe_name(id self, SEL _cmd)        { return XE_NAME; }
static NSString *xe_vendorName(id self, SEL _cmd)  { return @"Intel"; }
static NSString *xe_familyName(id self, SEL _cmd)  { return @"Iris Xe"; }
static NSString *xe_productName(id self, SEL _cmd) { return @"Iris Xe Graphics (IrisXe)"; }
// Metal picks the device's limits table (max colour attachments, texture
// sizes, ...) from featureProfile. Measured on this laptop with
// irisxe-metaltest --probe (v0.18.3): 1-5 and 12+ are refused, 6-7 give a
// Mac2-family GPU, 8-11 add the Apple7 (Apple Silicon) family. An Intel GPU
// is Mac2 without Apple7, so we use 7. IRISXE_FP=<n> overrides it for tests.
#define XE_FEATURE_PROFILE 7
static unsigned long long xe_featureProfile(id self, SEL _cmd) {
    const char *e = getenv("IRISXE_FP");
    return e ? strtoull(e, NULL, 0) : XE_FEATURE_PROFILE;
}

// Compute limits of Gen12 (Tiger Lake): up to 1024 threads per threadgroup,
// 64 KB of shared local memory per threadgroup.
static MTLSize xe_maxThreadsPerThreadgroup(id self, SEL _cmd) { return MTLSizeMake(1024, 1024, 1024); }   // (3a encoder: 1D, <= 512)
static NSUInteger xe_maxThreadgroupMemoryLength(id self, SEL _cmd) { return 64 * 1024; }

// ---- milestone 2a: buffers ----
// Same recipe as Apple's paravirtual GPU (research round 6): Apple's
// MTLIOAccelBuffer does the work. It fills the generic part (bytes 0x00-0x67)
// of the 0x80-byte IOAccelNewResourceArgs and asks the kernel for the
// resource; bytes 0x68-0x7f are the driver's own ("vendor") part.
static Class gBufClass;      // IrisXeMTLBuffer : MTLIOAccelBuffer
#define XE_RES_ARGS_SIZE 0x80
#define XE_RES_TAG       0x31455849u   // "IXE1"

static id makeBuffer(id dev, void *ptr, NSUInteger len, NSUInteger opts, id dealloc) {
    NSUInteger page = vm_kernel_page_size;

    if (len == 0) len = 1;
    uintptr_t off  = (uintptr_t)ptr & (page - 1);
    uintptr_t base = (uintptr_t)ptr & ~(uintptr_t)(page - 1);
    NSUInteger sys = (len + off + page - 1) & ~(page - 1);
    uint8_t args[XE_RES_ARGS_SIZE];
    memset(args, 0, sizeof(args));
    *(uint32_t *)(args + 0x68) = XE_RES_TAG;      // vendor part: tag + length
    *(uint64_t *)(args + 0x70) = len;
    SEL s = sel_registerName("initWithDevice:pointer:length:options:sysMemSize:vidMemSize:args:argsSize:deallocator:");
    id b = ((id (*)(id, SEL, id, void *, NSUInteger, NSUInteger, NSUInteger, NSUInteger, void *, uint32_t, id))objc_msgSend)(
        [gBufClass alloc], s, dev, (void *)base, len, opts, sys, 0, args, XE_RES_ARGS_SIZE, dealloc);
    os_log(gLog, "IrisXeMTL: newBuffer length %lu options 0x%lx pointer %p -> %p", (unsigned long)len,
           (unsigned long)opts, ptr, b);
    return b;
}

static id xe_newBufferWithLength(id self, SEL _cmd, NSUInteger len, NSUInteger opts) {
    return makeBuffer(self, NULL, len, opts, nil);
}
static id xe_newBufferWithBytes(id self, SEL _cmd, const void *bytes, NSUInteger len, NSUInteger opts) {
    id b = makeBuffer(self, NULL, len, opts, nil);
    void *dst = b ? ((void *(*)(id, SEL))objc_msgSend)(b, sel_registerName("contents")) : NULL;
    if (dst && bytes) memcpy(dst, bytes, len);
    return b;
}
static id xe_newBufferWithBytesNoCopy(id self, SEL _cmd, void *ptr, NSUInteger len, NSUInteger opts, id dealloc) {
    return makeBuffer(self, ptr, len, opts, dealloc);
}

// ---- milestone 2b: command queue and command buffers ----
// Apple's paravirtual GPU (research round 7) only subclasses Apple's
// MTLIOAccelCommandQueue / MTLIOAccelCommandBuffer and calls their init;
// commit and submission to the kernel are entirely Apple's code.
static Class gQueueClass;    // IrisXeMTLCommandQueue  : MTLIOAccelCommandQueue
static Class gCmdBufClass;   // IrisXeMTLCommandBuffer : MTLIOAccelCommandBuffer

static id xe_newCommandQueueWithDescriptor(id self, SEL _cmd, id desc) {
    id q = ((id (*)(id, SEL, id, id))objc_msgSend)([gQueueClass alloc], sel_registerName("initWithDevice:descriptor:"), self, desc);
    os_log(gLog, "IrisXeMTL: newCommandQueue -> %p", q);
    return q;
}
static id makeCmdBuf(id queue, BOOL retained) {
    id b = ((id (*)(id, SEL, id, BOOL))objc_msgSend)([gCmdBufClass alloc], sel_registerName("initWithQueue:retainedReferences:"), queue, retained);
    return [b autorelease];
}
static id xe_commandBuffer(id self, SEL _cmd) { return makeCmdBuf(self, YES); }
static id xe_commandBufferUnretained(id self, SEL _cmd) { return makeCmdBuf(self, NO); }

// ---- milestone 2c: blit encoder with a GPU fill ----
// Our encoder writes one driver ("kernel") command per fill into the command
// buffer, inside its own segment (research rounds 8-9):
//   [cb beginSegment:<current kernel-command pointer>]
//   p = [cb _reserveKernelCommandBufferSpace:32]   -> write the command at p
//   [cb endCurrentSegment]
// The kernel (IrisXeCommandQueue) runs it on the blitter.
#define XE_KCMD_FILL 0x10001u
typedef struct { uint32_t type, size; uint64_t gpuVA, length; uint32_t pattern, pad; } XeFillCmd;
#define XE_KCMD_COPY 0x10002u
typedef struct { uint32_t type, size; uint64_t srcVA, dstVA, length; } XeCopyCmd;
static Class gBlitClass;     // IrisXeMTLBlitCommandEncoder : MTLIOAccelBlitCommandEncoder

static id xe_blitCommandEncoder(id self, SEL _cmd) {
    id enc = ((id (*)(id, SEL, id))objc_msgSend)([gBlitClass alloc], sel_registerName("initWithCommandBuffer:"), self);
    if (enc) ((void (*)(id, SEL, id))objc_msgSend)(self, sel_registerName("setCurrentCommandEncoder:"), enc);
    return [enc autorelease];
}

// Start a segment holding one kernel command of `size` bytes that uses the
// given buffers; returns where to write the command (NULL on failure).
// Finish with endKernelCommand(cb).
// v0.49.0: newSegment false = add the command to the segment already open (a render encoder's draws share one,
// so IrisXeAccel can send them to the GPU as one job)
// v0.51.18: the command buffer keeps every resource its commands use alive until it is done (Metal's
// retained-references rule). Before, a texture whose last reference went away after encoding (Metal Performance
// Shaders' temporary images, released as soon as the next pass is encoded) was freed and unmapped before the GPU
// ran: the kernel skipped it ("not in the process's GPU page tables") and the blur wrote nothing.
static char kXeKeepRes;
static void keepForCommandBuffer(id cb, id *res, int n) {
    if (!cb || n <= 0) return;
    NSMutableSet *keep = objc_getAssociatedObject(cb, &kXeKeepRes);
    if (!keep) { keep = [NSMutableSet set]; objc_setAssociatedObject(cb, &kXeKeepRes, keep, OBJC_ASSOCIATION_RETAIN); }
    for (int i = 0; i < n; i++) if (res[i]) [keep addObject:res[i]];
}
static void *kernelCommandIn(id cb, id *buffers, int nbuf, unsigned long size, bool newSegment) {
    keepForCommandBuffer(cb, buffers, nbuf);
    if (newSegment) {
        void *cur = NULL;
        ((void (*)(id, SEL, void **, void **))objc_msgSend)(cb, sel_registerName("getCurrentKernelCommandBufferPointer:end:"), &cur, NULL);
        ((void (*)(id, SEL, void *))objc_msgSend)(cb, sel_registerName("beginSegment:"), cur);
    }
    // put the buffers in this segment's resource list (same call Apple's
    // Intel driver makes: list, &resource->_res.info, usage). The kernel maps
    // them into the GPU page tables itself (v0.21.4).
    static void (*addRes)(void *, void *, uint32_t);
    static ptrdiff_t resOff = -1;
    if (resOff < 0) {
        addRes = (void (*)(void *, void *, uint32_t))dlsym(RTLD_DEFAULT, "IOAccelResourceListAddResource");
        Ivar iv = class_getInstanceVariable(objc_getClass("MTLIOAccelResource"), "_res");
        resOff = iv ? ivar_getOffset(iv) : 0;
    }
    void *list = ((void *(*)(id, SEL))objc_msgSend)(cb, sel_registerName("ioAccelResourceList"));
    for (int i = 0; i < nbuf; i++) {
        if (!buffers[i] || (i && buffers[i] == buffers[0])) continue;   // none / same buffer twice
        if (addRes && resOff && list) addRes(list, (uint8_t *)buffers[i] + resOff + 0x20, 0x1);
        else os_log_error(gLog, "IrisXeMTL: could not add a buffer to the resource list");
    }
    return ((void *(*)(id, SEL, unsigned long))objc_msgSend)(cb, sel_registerName("_reserveKernelCommandBufferSpace:"), size);
}
static void *beginKernelCommand(id cb, id *buffers, int nbuf, unsigned long size) {
    return kernelCommandIn(cb, buffers, nbuf, size, true);
}
static void endKernelCommand(id cb) {
    ((void (*)(id, SEL))objc_msgSend)(cb, sel_registerName("endCurrentSegment"));
}
// v0.44.2: the recorder (IRISXE_RECORD=1) also keeps blits and compute dispatches, in order (IrisXeMTLRender.inc)
static void recordCmd(const char *name, const void *k, uint32_t ksize, id *res, int nres);
static bool gDesktopRecord;                             // v0.51.33: "record" in the desktop test switch
static bool recOn(void) { static int on = -1; if (on < 0) on = getenv("IRISXE_RECORD") != NULL || gDesktopRecord; return on; }

static void xe_fillBuffer(id self, SEL _cmd, id buffer, NSRange range, uint8_t value) {
    id cb = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("commandBuffer"));
    uint64_t base = ((uint64_t (*)(id, SEL))objc_msgSend)(buffer, sel_registerName("gpuAddress"));
    uint64_t va = base + range.location;
    if (!cb || !base || (va & 3) || (range.length & 3) || !range.length) {
        os_log_error(gLog, "IrisXeMTL: fillBuffer not supported yet (va 0x%llx length %lu: needs 4-byte alignment)",
                     va, (unsigned long)range.length);
        return;
    }
    XeFillCmd *k = beginKernelCommand(cb, &buffer, 1, sizeof(XeFillCmd));
    if (k) {
        k->type = XE_KCMD_FILL; k->size = sizeof(XeFillCmd);
        k->gpuVA = va; k->length = range.length;
        k->pattern = value * 0x01010101u; k->pad = 0;
        if (recOn()) recordCmd("blit: fill", k, sizeof(*k), &buffer, 1);
    }
    endKernelCommand(cb);
    os_log(gLog, "IrisXeMTL: fillBuffer GPU 0x%llx length %lu value 0x%02x -> command %p", va, (unsigned long)range.length, value, k);
}

// ---- milestone 2d: GPU copy buffer -> buffer ----
static void xe_copyBuffer(id self, SEL _cmd, id src, NSUInteger srcOff, id dst, NSUInteger dstOff, NSUInteger size) {
    id cb = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("commandBuffer"));
    uint64_t sb = ((uint64_t (*)(id, SEL))objc_msgSend)(src, sel_registerName("gpuAddress"));
    uint64_t db = ((uint64_t (*)(id, SEL))objc_msgSend)(dst, sel_registerName("gpuAddress"));
    uint64_t sva = sb + srcOff, dva = db + dstOff;
    if (!cb || !sb || !db || !size || ((sva | dva | size) & 3)) {
        os_log_error(gLog, "IrisXeMTL: copyFromBuffer not supported yet (0x%llx -> 0x%llx, %lu bytes: needs 4-byte alignment)",
                     sva, dva, (unsigned long)size);
        return;
    }
    id bufs[2] = { src, dst };
    XeCopyCmd *k = beginKernelCommand(cb, bufs, 2, sizeof(XeCopyCmd));
    if (k) {
        k->type = XE_KCMD_COPY; k->size = sizeof(XeCopyCmd);
        k->srcVA = sva; k->dstVA = dva; k->length = size;
        if (recOn()) recordCmd("blit: copy", k, sizeof(*k), bufs, 2);
    }
    endKernelCommand(cb);
    os_log(gLog, "IrisXeMTL: copyFromBuffer GPU 0x%llx -> 0x%llx, %lu bytes -> command %p", sva, dva, (unsigned long)size, k);
}

// ---- milestone 3a: compute -------------------------------------------------
// No shader compiler yet: a pipeline is made from a ready Iris Xe (Gen12)
// SIMD8 kernel with the private device method
//     -[device irisxeNewComputePipelineWithKernel:(NSData *)code]
// The encoder sends one kernel command per dispatch with the kernel code
// inside; the kernel side runs it on the render engine (GPGPU walker).
#define XE_KCMD_DISPATCH 0x10003u
typedef struct { uint32_t type, size; uint32_t groups[3], tpg, gridX, nbuf, kernelDW,
                 info;      // v0.28.0: threadgroup memory bytes | 0x80000000 = uses barriers
                 uint64_t bufVA[6], bufLen[6];
                 uint64_t codeVA;   // v0.29.0: with XE_INFO_CODE_IN_BUFFER
                 uint32_t ntex, nsamp;                  // v0.30.0: textures, samplers (IrisXeSpaceABI.h)
                 uint8_t texdesc[8 * 40];               // XeTexDescU[8] (defined in IrisXeMTLTexture.inc)
                 uint8_t sampdesc[8 * 16];
                 uint32_t tpgH, tpgD, gridY, gridZ; } XeDispatchCmd;   // v0.51.17: tpg = W x tpgH x tpgD; grid rows, slices
#define XE_INFO_CODE_IN_BUFFER 0x40000000u          // the kernel is in its own GPU buffer at codeVA
#define XE_INLINE_MAX   3968                          // bigger kernels go in a buffer (IrisXeCore 0.29+)
#define XE_KERNEL_MAX   (1048576 * 4)                 // 4 MB (v0.51.9: Core Animation's big UberShader)
#define XE_MAX_TPG 512
static Class gPipeClass, gComputeClass;
static Ivar gPipeCode, gPipeInfo, gPipeCodeBuf, gPipeScratch, gPipeCSamp, gComputeState;
#define XE_INFO_CSAMPLERS(i) (((i) >> 17) & 7u)   // v0.30.1: constexpr samplers (slots 7, 6, ..)
#define XE_INFO_BUNDLE_ONLY  0x3F0E0000u          // info bits IrisXeCore must not see
// v0.29.1: private arrays: per lane info bits 29:24 * 16 bytes, for every
// hardware thread id (12 bits from sr0) x 8 lanes, bound at buffer slot 5
#define XE_INFO_PRIVATE(i)  ((((i) >> 24) & 0x3Fu) * 16u)
#define XE_SCRATCH_THREADS  4096

static NSMutableSet *gMissing;                         // (defined with the missing-method catcher below)
// v0.39.0: every Metal method an app calls that we don't have is also written to /tmp/irisxe-missing.log
// (one line per method and program; sandboxed apps can't write there: use irisxe-metaltest --missing, which
// also reads the system log)
static void missingToFile(const char *what) {
    char line[512];
    int n = snprintf(line, sizeof(line), "%s[%d]: %s\n", getprogname(), getpid(), what);
    int fd = open("/tmp/irisxe-missing.log", O_WRONLY | O_APPEND | O_CREAT, 0666);
    if (fd >= 0) { if (n > 0) write(fd, line, (size_t)MIN(n, (int)sizeof(line) - 1)); fchmod(fd, 0666); close(fd); }
}
static bool gDesktopIOSCopy;                            // v0.51.25: set by desktopThisBoot ("copy")
static bool gDesktopNoBatch;                            // v0.51.35: "nobatch" (v0.51.37: the default)
static bool gDesktopBatch;                              // v0.51.37: "batch": batched draws again
#include "IrisXeMTLTexture.inc"                        // milestone 4: textures, samplers
typedef struct { id pipe; id buf[31]; NSUInteger off[31]; id tex[XE_MAX_TEX]; id samp[XE_MAX_SAMP]; } XeComputeState;

static id xe_newComputePipelineWithKernel(id self, SEL _cmd, NSData *code) {
    if (!code.length || (code.length & 3) || code.length > 3968) return nil;   // IrisXeCore 0.26+: 3968 bytes (older: 2048)
    id p = [gPipeClass alloc];
    *(id *)((char *)p + ivar_getOffset(gPipeCode)) = [code copy];
    return p;
}
// ---- milestone 3c: pipelines from real Metal functions (our compiler) ----
// Metal gives the function's AIR (LLVM bitcode) as -[MTLFunction bitcodeData];
// src/compiler/xe_compile.cpp turns it into Iris Xe code in this process.
// v0.25.1: the compiler (with LLVM inside) is Contents/Frameworks/
// libIrisXeCompiler.dylib, opened on first use: every process that touches
// Metal loads this bundle, and they must not all get LLVM.
typedef int (*XeCompileFn)(const void *, size_t, const char *, uint32_t *, uint32_t, uint32_t *, uint32_t *, uint64_t *,
                           char *, size_t);
static XeCompileFn compilerEntry(NSString **why) {
    static XeCompileFn fn;
    static NSString *err;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        Dl_info di = {0};
        if (!dladdr((void *)&compilerEntry, &di) || !di.dli_fname) { err = @"can't find our bundle"; return; }
        NSString *contents = [[[NSString stringWithUTF8String:di.dli_fname] stringByDeletingLastPathComponent]
                                 stringByDeletingLastPathComponent];
        NSString *path = [contents stringByAppendingPathComponent:@"Frameworks/libIrisXeCompiler.dylib"];
        void *h = dlopen(path.fileSystemRepresentation, RTLD_NOW | RTLD_LOCAL);
        if (!h) { err = [[NSString alloc] initWithFormat:@"compiler not loaded: %s", dlerror()]; return; }
        fn = (XeCompileFn)dlsym(h, "irisxe_compile_air");
        if (!fn) err = @"compiler has no irisxe_compile_air";
    });
    if (!fn && why) *why = err;
    return fn;
}
static NSString *gLastCompileLog;
static void saveFailedAIR(NSData *bc, NSString *name, NSString *kind);   // IrisXeMTLRender.inc
static bool isTestTool(void) {
    const char *p = getprogname();
    return p && (!strcmp(p, "irisxe-metaltest") || !strcmp(p, "irisxe-mtkdemo"));
}
// v0.62.1: the shader compiler's switches, read once per program: /Library/Preferences/irisxe-overlap "off" (all of
// the v0.61+ speed-ups off) and /Library/Preferences/irisxe-compiler, words that turn single speed-ups off (for finding
// which one draws something wrong): nosb (reads overlapped), nogroup (samples grouped), nohoist (constants read once at
// the start), nopack (half vectors in one read), nounroll (short loops unrolled), noblock (constants read in blocks).
// Returns a number that changes with the switches (part of the shader cache key).
static uint64_t xeCompilerSwitches(void) {
    static uint64_t sw;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        FILE *f = fopen("/Library/Preferences/irisxe-overlap", "r");
        if (f) { char b[16] = {}; if (fgets(b, sizeof(b), f) && !strncmp(b, "off", 3)) { setenv("XEC_NOOVERLAP", "1", 1); sw |= 1; } fclose(f); }
        static const char *const words[] = { "nosb", "nogroup", "nohoist", "nopack", "nounroll", "noblock", "novmask" };
        static const char *const envs[] = { "XEC_NOSB", "XEC_NOGROUP", "XEC_NOHOIST", "XEC_NOPACK", "XEC_NOUNROLL", "XEC_NOBLOCK", "XEC_NOVMASK" };
        f = fopen("/Library/Preferences/irisxe-compiler", "r");
        if (f) {
            char b[256] = {};
            size_t n = fread(b, 1, sizeof(b) - 1, f);
            fclose(f);
            b[n] = 0;
            for (unsigned k = 0; k < sizeof(words) / sizeof(words[0]); k++)
                if (strstr(b, words[k])) { setenv(envs[k], "1", 1); sw |= 2ull << k; }
        }
        os_log(gLog, "IrisXeMTL: shader compiler switches 0x%llx", (unsigned long long)sw);
    });
    return sw;
}
static id compileFunction(id dev, id fn, NSError **err) {
    NSString *name = fn ? ((NSString *(*)(id, SEL))objc_msgSend)(fn, sel_registerName("name")) : nil;
    NSString *msg = nil;
    id pipe = nil;
#ifdef IRISXE_NO_COMPILER
    msg = @"this IrisXe Metal driver was built without its shader compiler";
#else
    NSData *bc = [fn respondsToSelector:sel_registerName("bitcodeData")]
                   ? ((id (*)(id, SEL))objc_msgSend)(fn, sel_registerName("bitcodeData")) : nil;
    if (!name || ![bc isKindOfClass:[NSData class]] || !bc.length) {
        msg = @"the function has no AIR bitcode";
    } else {
        static uint32_t *code;                       // v0.29.0: up to 256 KB (kernels > 3968 bytes go in a buffer)
        if (!code) code = malloc(XE_KERNEL_MAX);
        uint32_t dw = 0, info = 0; char log[512];
        uint64_t csamp[4] = { 0 };
        static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
        pthread_mutex_lock(&lock);
        NSString *why = nil;
        XeCompileFn compile = compilerEntry(&why);
        xeCompilerSwitches();                        // v0.61.0 / v0.62.1: irisxe-overlap, irisxe-compiler
        int r = -100;
        if (!compile) snprintf(log, sizeof(log), "%s", why.UTF8String ?: "no compiler");
        else r = compile(bc.bytes, bc.length, name.UTF8String, code, XE_KERNEL_MAX / 4, &dw, &info, csamp, log, sizeof(log));
        if (r == 0 && dw * 4 <= XE_INLINE_MAX) pipe = xe_newComputePipelineWithKernel(nil, NULL, [NSData dataWithBytes:code length:dw * 4]);
        else if (r == 0) {
            // a big kernel: its own GPU buffer, sent once (the dispatch only names it);
            // 256 zero bytes after it for the GPU's instruction prefetch
            id buf = ((id (*)(id, SEL, NSUInteger, NSUInteger))objc_msgSend)(dev, sel_registerName("newBufferWithLength:options:"),
                                                                           dw * 4 + 256, MTLResourceStorageModeShared);
            void *p = buf ? ((void *(*)(id, SEL))objc_msgSend)(buf, sel_registerName("contents")) : NULL;
            if (p) {
                memcpy(p, code, dw * 4);
                memset((char *)p + dw * 4, 0, 256);
                pipe = [gPipeClass alloc];
                *(id *)((char *)pipe + ivar_getOffset(gPipeCode)) = [[NSData alloc] initWithBytes:code length:dw * 4];
                *(id *)((char *)pipe + ivar_getOffset(gPipeCodeBuf)) = buf;
            } else {
                [buf release];
                snprintf(log + strlen(log), sizeof(log) - strlen(log), " (but no GPU buffer for the %u-byte kernel)", dw * 4);
            }
        }
        if (pipe) *(uint32_t *)((char *)pipe + ivar_getOffset(gPipeInfo)) = info;   // 3f: threadgroup memory, barriers
        if (pipe) memcpy((char *)pipe + ivar_getOffset(gPipeCSamp), csamp, sizeof(csamp));
        if (pipe && XE_INFO_PRIVATE(info)) {
            NSUInteger len = (NSUInteger)XE_SCRATCH_THREADS * 8 * XE_INFO_PRIVATE(info);
            id sb = ((id (*)(id, SEL, NSUInteger, NSUInteger))objc_msgSend)(dev, sel_registerName("newBufferWithLength:options:"),
                                                                          len, MTLResourceStorageModeShared);
            if (sb) *(id *)((char *)pipe + ivar_getOffset(gPipeScratch)) = sb;
            else {
                [pipe release]; pipe = nil;
                snprintf(log + strlen(log), sizeof(log) - strlen(log), " (but no %lu-byte scratch buffer for its private arrays)", (unsigned long)len);
            }
        }
        pthread_mutex_unlock(&lock);
        msg = [NSString stringWithFormat:@"%s", log];
        // v0.51.22: a kernel our compiler can't build yet. Keep its AIR for offline work; outside our test tools give a
        // placeholder pipeline (its dispatches are skipped and logged) - Metal Performance Shaders stops the app on a nil
        // pipeline (Spotlight's Vision network did, 5 times in a row)
        if (r != 0) {
            saveFailedAIR(bc, name, @"cs");
            if (!pipe && !isTestTool()) {
                pipe = [gPipeClass alloc];
                msg = [msg stringByAppendingString:@" - placeholder pipeline: its dispatches are skipped"];
            }
        }
    }
#endif
    [gLastCompileLog release];
    gLastCompileLog = [[NSString alloc] initWithFormat:@"%@: %@", name ?: @"?", msg];
    os_log(gLog, "IrisXeMTL: compile %{public}@", gLastCompileLog);
    if (!pipe && err)
        *err = [NSError errorWithDomain:@"IrisXeMTLCompiler" code:1
                               userInfo:@{ NSLocalizedDescriptionKey : gLastCompileLog }];
    return pipe;
}
static id xe_newCPSWithFunction(id self, SEL _cmd, id fn, NSError **err) { return compileFunction(self, fn, err); }
static id xe_newCPSWithFunctionOpts(id self, SEL _cmd, id fn, NSUInteger opts, id *refl, NSError **err) {
    if (refl) *refl = nil;
    return compileFunction(self, fn, err);
}
static id xe_newCPSWithDescriptor(id self, SEL _cmd, id desc, NSUInteger opts, id *refl, NSError **err) {
    if (refl) *refl = nil;
    id fn = desc ? ((id (*)(id, SEL))objc_msgSend)(desc, sel_registerName("computeFunction")) : nil;
    return compileFunction(self, fn, err);
}
static void xe_newCPSWithFunctionHandler(id self, SEL _cmd, id fn, void (^handler)(id, NSError *)) {
    NSError *e = nil;
    id p = compileFunction(self, fn, &e);
    if (handler) handler(p, e);
    [p release];
}
static NSString *xe_lastCompileLog(id self, SEL _cmd) { return gLastCompileLog; }

static void xe_pipeDealloc(id self, SEL _cmd) {
    [*(id *)((char *)self + ivar_getOffset(gPipeCode)) release];
    [*(id *)((char *)self + ivar_getOffset(gPipeCodeBuf)) release];
    [*(id *)((char *)self + ivar_getOffset(gPipeScratch)) release];
    struct objc_super sup = { self, class_getSuperclass(gPipeClass) };
    ((void (*)(struct objc_super *, SEL))objc_msgSendSuper)(&sup, _cmd);
}
static NSUInteger xe_pipeMaxTPG(id self, SEL _cmd) { return XE_MAX_TPG; }
static uint32_t pipeInfo(id p) { return p ? *(uint32_t *)((char *)p + ivar_getOffset(gPipeInfo)) : 0; }
static NSUInteger xe_pipeTGMem(id self, SEL _cmd) { return pipeInfo(self) & 0x00FFFFFFu; }
static NSUInteger xe_pipeWidth(id self, SEL _cmd)  { return 8; }
static NSUInteger xe_zero(id self, SEL _cmd)       { return 0; }

static XeComputeState *cstate(id enc) {
    XeComputeState **slot = (XeComputeState **)((char *)enc + ivar_getOffset(gComputeState));
    if (!*slot) *slot = calloc(1, sizeof(XeComputeState));
    return *slot;
}
static id xe_computeCommandEncoder(id self, SEL _cmd) {
    id enc = ((id (*)(id, SEL, id))objc_msgSend)([gComputeClass alloc], sel_registerName("initWithCommandBuffer:"), self);
    if (enc) ((void (*)(id, SEL, id))objc_msgSend)(self, sel_registerName("setCurrentCommandEncoder:"), enc);
    return [enc autorelease];
}
static id xe_computeCommandEncoderType(id self, SEL _cmd, NSUInteger type) { return xe_computeCommandEncoder(self, _cmd); }
static void xe_computeDealloc(id self, SEL _cmd) {
    XeComputeState **slot = (XeComputeState **)((char *)self + ivar_getOffset(gComputeState));
    XeComputeState *st = *slot;
    if (st) {
        [st->pipe release];
        for (int i = 0; i < 31; i++) [st->buf[i] release];
        for (int i = 0; i < XE_MAX_TEX; i++) [st->tex[i] release];
        for (int i = 0; i < XE_MAX_SAMP; i++) [st->samp[i] release];
        free(st);
        *slot = NULL;
    }
    struct objc_super sup = { self, class_getSuperclass(gComputeClass) };
    ((void (*)(struct objc_super *, SEL))objc_msgSendSuper)(&sup, _cmd);
}
static void xe_setPipeline(id self, SEL _cmd, id pipe) {
    XeComputeState *st = cstate(self);
    if (pipe && object_getClass(pipe) != gPipeClass) {
        os_log_error(gLog, "IrisXeMTL: compute pipeline %{public}s is not ours (no shader compiler yet)", class_getName(object_getClass(pipe)));
        pipe = nil;
    }
    [pipe retain]; [st->pipe release]; st->pipe = pipe;
}
static void xe_setBuffer(id self, SEL _cmd, id buf, NSUInteger off, NSUInteger idx) {
    if (idx >= 31) return;
    XeComputeState *st = cstate(self);
    [buf retain]; [st->buf[idx] release]; st->buf[idx] = buf; st->off[idx] = off;
}
static void xe_setBufferOffset(id self, SEL _cmd, NSUInteger off, NSUInteger idx) {
    if (idx < 31) cstate(self)->off[idx] = off;
}

// v0.51.16: what Metal Performance Shaders calls on a compute encoder (its MPSComputeEncoder wrapper only
// forwards methods we respond to). setBytes: a small buffer kept alive with the command buffer.
static char kXeKeepBytes;
static void xe_cSetBytes(id self, SEL _cmd, const void *bytes, NSUInteger len, NSUInteger idx) {
    if (idx >= 31) return;
    id cb = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("commandBuffer"));
    id dev = cb ? ((id (*)(id, SEL))objc_msgSend)(cb, sel_registerName("device")) : nil;
    id b = dev ? ((id (*)(id, SEL, const void *, NSUInteger, NSUInteger))objc_msgSend)(dev, sel_registerName("newBufferWithBytes:length:options:"),
                                                                                       bytes, len ? len : 4, MTLResourceStorageModeShared) : nil;
    if (!b) { os_log_error(gLog, "IrisXeMTL: compute setBytes: no buffer (%lu bytes)", (unsigned long)len); return; }
    NSMutableArray *keep = objc_getAssociatedObject(cb, &kXeKeepBytes);
    if (!keep) { keep = [NSMutableArray array]; objc_setAssociatedObject(cb, &kXeKeepBytes, keep, OBJC_ASSOCIATION_RETAIN); }
    [keep addObject:b];
    xe_setBuffer(self, _cmd, b, 0, idx);
    [b release];
}
static void xe_cSetBuffers(id self, SEL _cmd, id const *bs, const NSUInteger *offs, NSRange r) {
    for (NSUInteger i = 0; i < r.length; i++) xe_setBuffer(self, _cmd, bs[i], offs ? offs[i] : 0, r.location + i);
}
// each dispatch finishes before the next one starts, and every resource is resident: nothing to do (yet)
static void xe_cNop1(id self, SEL _cmd, id a) {}
static void xe_cNopQ(id self, SEL _cmd, NSUInteger a) {}
static void xe_cNop1Q(id self, SEL _cmd, id a, NSUInteger u) {}
static void xe_cNopPQ(id self, SEL _cmd, const id *a, NSUInteger n) {}
static void xe_cNopPQQ(id self, SEL _cmd, const id *a, NSUInteger n, NSUInteger u) {}
static void xe_cNopQQ(id self, SEL _cmd, NSUInteger a, NSUInteger b) {}
// milestone 4
static void xe_setTexture(id self, SEL _cmd, id t, NSUInteger idx) {
    if (idx >= XE_MAX_TEX) { os_log_error(gLog, "IrisXeMTL: texture index %lu: only 0..7 yet", (unsigned long)idx); return; }
    if (t && object_getClass(t) != gTexClass) { os_log_error(gLog, "IrisXeMTL: setTexture: not our texture"); t = nil; }
    XeComputeState *st = cstate(self);
    [t retain]; [st->tex[idx] release]; st->tex[idx] = t;
}
static void xe_setTextures(id self, SEL _cmd, id const *ts, NSRange r) {
    for (NSUInteger i = 0; i < r.length; i++) xe_setTexture(self, _cmd, ts[i], r.location + i);
}
static void xe_setSampler(id self, SEL _cmd, id s, NSUInteger idx) {
    if (idx >= XE_MAX_SAMP) { os_log_error(gLog, "IrisXeMTL: sampler index %lu: only 0..7 yet", (unsigned long)idx); return; }
    if (s && object_getClass(s) != gSampClass) { os_log_error(gLog, "IrisXeMTL: setSamplerState: not our sampler"); s = nil; }
    XeComputeState *st = cstate(self);
    [s retain]; [st->samp[idx] release]; st->samp[idx] = s;
}
static void xe_setSamplers(id self, SEL _cmd, id const *ss, NSRange r) {
    for (NSUInteger i = 0; i < r.length; i++) xe_setSampler(self, _cmd, ss[i], r.location + i);
}
static void xe_setSamplerClamp(id self, SEL _cmd, id s, float lo, float hi, NSUInteger idx) { xe_setSampler(self, _cmd, s, idx); }
// 3f: kernels with [[threadgroup(n)]] buffer arguments are refused by our
// compiler for now (threadgroup arrays inside the kernel work), so the length is only logged
static void xe_setTGMemLength(id self, SEL _cmd, NSUInteger len, NSUInteger idx) {
    os_log(gLog, "IrisXeMTL: setThreadgroupMemoryLength %lu at %lu (threadgroup arguments not supported yet)",
           (unsigned long)len, (unsigned long)idx);
}
static void dispatchCommon(id self, MTLSize groups, MTLSize tpg, NSUInteger gridX, NSUInteger gridY, NSUInteger gridZ) {
    XeComputeState *st = cstate(self);
    NSData *code = st->pipe ? *(id *)((char *)st->pipe + ivar_getOffset(gPipeCode)) : nil;
    if (!code) { os_log_error(gLog, "IrisXeMTL: dispatch skipped: no compute pipeline, or its kernel did not compile (placeholder)"); return; }
    // v0.51.17: 2D / 3D thread groups (W x H x D <= 512 threads)
    if (!tpg.width || !tpg.height || !tpg.depth || tpg.width * tpg.height * tpg.depth > XE_MAX_TPG ||
        !groups.width || !groups.height || !groups.depth) {
        os_log_error(gLog, "IrisXeMTL: dispatch %lux%lux%lu groups of %lux%lux%lu not supported (<= %d threads in a group)",
                     (unsigned long)groups.width, (unsigned long)groups.height, (unsigned long)groups.depth,
                     (unsigned long)tpg.width, (unsigned long)tpg.height, (unsigned long)tpg.depth, XE_MAX_TPG);
        if (!strcmp(getprogname(), "irisxe-metaltest"))
            fprintf(stderr, "    dispatch skipped: %lux%lux%lu groups of %lux%lux%lu threads (groups of <= %d threads)\n",
                    (unsigned long)groups.width, (unsigned long)groups.height, (unsigned long)groups.depth,
                    (unsigned long)tpg.width, (unsigned long)tpg.height, (unsigned long)tpg.depth, XE_MAX_TPG);
        return;
    }
    id cb = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("commandBuffer"));
    id codeBuf = *(id *)((char *)st->pipe + ivar_getOffset(gPipeCodeBuf));
    id scratch = *(id *)((char *)st->pipe + ivar_getOffset(gPipeScratch));
    id bufs[7 + XE_MAX_TEX]; int nb = 0;
    for (int i = 0; i < 6; i++) if (st->buf[i]) nb = i + 1;
    if (scratch) nb = 6;
    for (int i = 0; i < nb; i++) bufs[i] = i == 5 && scratch ? scratch : st->buf[i];
    if (codeBuf) bufs[nb] = codeBuf;                 // the kernel's buffer is mapped like the others
    unsigned long size = sizeof(XeDispatchCmd) + (codeBuf ? 0 : code.length);
    // milestone 4: textures (their buffers go in the resource list too) and samplers
    XeTexDescU td[XE_MAX_TEX]; XeSampDescU sd[XE_MAX_SAMP];
    uint32_t ntex = 0, nsamp = 0;
    int nres = nb + (codeBuf ? 1 : 0);
    for (int i = 0; i < XE_MAX_TEX; i++) {
        id tb = nil;
        texDesc(st->tex[i], &td[i], &tb);
        if (tb) { bufs[nres++] = tb; ntex = i + 1; }
    }
    for (int i = 0; i < XE_MAX_SAMP; i++) {
        XeSamp *x = samp(st->samp[i]);
        if (x) { sd[i] = x->d; nsamp = i + 1; } else memset(&sd[i], 0, sizeof(sd[i]));
    }
    unsigned ncs = XE_INFO_CSAMPLERS(pipeInfo(st->pipe));
    const uint64_t *cs = (const uint64_t *)((char *)st->pipe + ivar_getOffset(gPipeCSamp));
    for (unsigned k = 0; k < ncs && k < 4; k++) {     // constexpr samplers: slots 7, 6, ..
        if (sd[7 - k].used) os_log_error(gLog, "IrisXeMTL: sampler slot %u is set by the app and used by a constexpr sampler", 7 - k);
        sd[7 - k] = constSampler(cs[k]);
        nsamp = XE_MAX_SAMP;
    }
    XeDispatchCmd *k = beginKernelCommand(cb, bufs, nres, size);
    if (k) {
        memset(k, 0, sizeof(*k));
        k->type = XE_KCMD_DISPATCH; k->size = (uint32_t)size;
        k->groups[0] = (uint32_t)groups.width; k->groups[1] = (uint32_t)groups.height; k->groups[2] = (uint32_t)groups.depth;
        k->tpg = (uint32_t)(tpg.width * tpg.height * tpg.depth); k->gridX = (uint32_t)gridX;
        k->tpgH = (uint32_t)tpg.height; k->tpgD = (uint32_t)tpg.depth;
        k->gridY = (uint32_t)gridY; k->gridZ = (uint32_t)gridZ;
        k->nbuf = nb; k->kernelDW = (uint32_t)(code.length / 4);
        k->info = pipeInfo(st->pipe) & ~XE_INFO_BUNDLE_ONLY;   // private size / constexpr samplers: bundle only
        if (codeBuf) {
            k->info |= XE_INFO_CODE_IN_BUFFER;
            k->codeVA = ((uint64_t (*)(id, SEL))objc_msgSend)(codeBuf, sel_registerName("gpuAddress"));
        }
        for (int i = 0; i < nb; i++) {
            if (!bufs[i]) continue;
            NSUInteger off = bufs[i] == scratch ? 0 : st->off[i];
            uint64_t va = ((uint64_t (*)(id, SEL))objc_msgSend)(bufs[i], sel_registerName("gpuAddress"));
            NSUInteger len = ((NSUInteger (*)(id, SEL))objc_msgSend)(bufs[i], sel_registerName("length"));
            k->bufVA[i] = va + off;
            k->bufLen[i] = len > off ? len - off : 0;
        }
        if (!codeBuf) memcpy(k + 1, code.bytes, code.length);
        k->ntex = ntex; k->nsamp = nsamp;
        memcpy(k->texdesc, td, sizeof(td));
        memcpy(k->sampdesc, sd, sizeof(sd));
        if (recOn()) recordCmd("compute dispatch (not replayed)", k, sizeof(*k), NULL, 0);   // v0.44.2: a marker
    }
    endKernelCommand(cb);
    os_log(gLog, "IrisXeMTL: dispatch %lu groups x %lu threads (grid %lu), %d buffers -> command %p",
           (unsigned long)groups.width, (unsigned long)tpg.width, (unsigned long)gridX, nb, k);
}
static void xe_dispatchThreadgroups(id self, SEL _cmd, MTLSize groups, MTLSize tpg) {
    dispatchCommon(self, groups, tpg, groups.width * tpg.width, groups.height * tpg.height, groups.depth * tpg.depth);
}
static void xe_dispatchThreads(id self, SEL _cmd, MTLSize grid, MTLSize tpg) {
    if (!tpg.width) return;
    MTLSize g = MTLSizeMake((grid.width + tpg.width - 1) / tpg.width,
                            tpg.height ? (grid.height + tpg.height - 1) / tpg.height : 0,
                            tpg.depth ? (grid.depth + tpg.depth - 1) / tpg.depth : 0);
    dispatchCommon(self, g, tpg, grid.width, grid.height, grid.depth);
}

static BOOL xe_no(id self, SEL _cmd)               { return NO; }
static BOOL xe_yes(id self, SEL _cmd)              { return YES; }

// ---- "missing method" catcher (step 2b) ----
// Metal calls many MTLDevice methods we don't have yet (the first one, called
// from -[_MTLDevice initLimits], crashed v0.18.1). Instead of crashing, every
// missing method that is actually CALLED is reported once (system log, and on
// screen in irisxe-metaltest) and returns 0 / nil / NO. respondsToSelector: is
// not affected, so Metal's "is this optional feature there?" checks stay honest.
static NSMutableSet *gMissing;

static const char *protoTypes(SEL sel) {
    const char *names[] = { "MTLDeviceSPI", "MTLDevice" };
    for (int i = 0; i < 2; i++) {
        Protocol *p = objc_getProtocol(names[i]);
        if (!p) continue;
        for (int req = 1; req >= 0; req--) {
            struct objc_method_description d = protocol_getMethodDescription(p, sel, req, YES);
            if (d.types) return d.types;
        }
    }
    return NULL;
}

static NSMethodSignature *xe_methodSignatureForSelector(id self, SEL _cmd, SEL sel) {
    struct objc_super sup = { self, gSuper };
    NSMethodSignature *sig = ((id (*)(struct objc_super *, SEL, SEL))objc_msgSendSuper)(&sup, _cmd, sel);
    if (sig) return sig;
    const char *t = protoTypes(sel);
    return [NSMethodSignature signatureWithObjCTypes:t ? t : "@@:"];
}

static void xe_forwardInvocation(id self, SEL _cmd, NSInvocation *inv) {
    SEL sel = inv.selector;
    NSString *name = NSStringFromSelector(sel);
    @synchronized (gMissing) {
        if (![gMissing containsObject:name]) {
            [gMissing addObject:name];
            const char *t = protoTypes(sel);
            os_log(gLog, "IrisXeMTL: MISSING -[IrisXeMTLDevice %{public}s] types %{public}s -> returns 0",
                   name.UTF8String, t ? t : "?");
            missingToFile([NSString stringWithFormat:@"MISSING -[IrisXeMTLDevice %@]", name].UTF8String);
            if (!strcmp(getprogname(), "irisxe-metaltest") && !getenv("IRISXE_QUIET"))
                fprintf(stderr, "    MISSING method: %s  (types %s) -> 0\n", name.UTF8String, t ? t : "?");
        }
    }
    NSUInteger len = inv.methodSignature.methodReturnLength;
    if (len) {
        void *zero = calloc(1, len);
        [inv setReturnValue:zero];
        free(zero);
    }
}

static void add(Class c, const char *sel, IMP imp, const char *types) {
    if (!class_addMethod(c, sel_registerName(sel), imp, types))
        os_log_error(gLog, "IrisXeMTL: could not add %{public}s", sel);
}

// ---- v0.42.0: MTLHeap. No sub-allocation yet: every resource "from the heap" is an ordinary buffer / texture of
// the device, and the heap only keeps count (Maps makes heaps; a nil heap gave nil textures -> black map tiles)
static Class gHeapClass;
static Ivar gHeapIvar;
typedef struct { id dev; NSUInteger size, used, storage, cpu, hazard, type; id label; } XeHeap;
static XeHeap *heapOf(id h) { return h ? *(XeHeap **)((char *)h + ivar_getOffset(gHeapIvar)) : NULL; }
static NSUInteger heapRound(NSUInteger n) { return (n + 4095) & ~(NSUInteger)4095; }
static id xe_newHeap(id self, SEL _cmd, id desc) {
    if (!gHeapClass || !desc) return nil;
    XeHeap *x = calloc(1, sizeof(XeHeap));
    x->dev = [self retain];
    x->size = ((NSUInteger (*)(id, SEL))objc_msgSend)(desc, sel_registerName("size"));
    x->storage = ((NSUInteger (*)(id, SEL))objc_msgSend)(desc, sel_registerName("storageMode"));
    x->cpu = ((NSUInteger (*)(id, SEL))objc_msgSend)(desc, sel_registerName("cpuCacheMode"));
    if ([desc respondsToSelector:sel_registerName("hazardTrackingMode")])
        x->hazard = ((NSUInteger (*)(id, SEL))objc_msgSend)(desc, sel_registerName("hazardTrackingMode"));
    if ([desc respondsToSelector:sel_registerName("type")])
        x->type = ((NSUInteger (*)(id, SEL))objc_msgSend)(desc, sel_registerName("type"));
    id h = [gHeapClass alloc];
    *(XeHeap **)((char *)h + ivar_getOffset(gHeapIvar)) = x;
    os_log(gLog, "IrisXeMTL: heap of %lu bytes (storage %lu)", (unsigned long)x->size, (unsigned long)x->storage);
    return h;
}
static void xe_heapDealloc(id self, SEL _cmd) {
    XeHeap *x = heapOf(self);
    if (x) { [x->dev release]; [x->label release]; free(x); }
    struct objc_super sup = { self, [NSObject class] };
    ((void (*)(struct objc_super *, SEL))objc_msgSendSuper)(&sup, _cmd);
}
static id xe_heapDevice(id s, SEL c) { return heapOf(s)->dev; }
static id xe_heapLabel(id s, SEL c) { return heapOf(s)->label; }
static void xe_heapSetLabel(id s, SEL c, id l) { XeHeap *x = heapOf(s); [x->label release]; x->label = [l copy]; }
static NSUInteger xe_heapSize(id s, SEL c) { return heapOf(s)->size; }
static NSUInteger xe_heapUsed(id s, SEL c) { return heapOf(s)->used; }
static NSUInteger xe_heapStorage(id s, SEL c) { return heapOf(s)->storage; }
static NSUInteger xe_heapCpu(id s, SEL c) { return heapOf(s)->cpu; }
static NSUInteger xe_heapHazard(id s, SEL c) { return heapOf(s)->hazard; }
static NSUInteger xe_heapType(id s, SEL c) { return heapOf(s)->type; }
static NSUInteger xe_heapOptions(id s, SEL c) { XeHeap *x = heapOf(s); return (x->storage << 4) | x->cpu | (x->hazard << 8); }
static NSUInteger xe_heapMaxAvail(id s, SEL c, NSUInteger align) {
    XeHeap *x = heapOf(s); return x->size > x->used ? x->size - x->used : 0;
}
static NSUInteger xe_heapPurgeable(id s, SEL c, NSUInteger st) { return 1; }   // MTLPurgeableStateNonVolatile
static id xe_heapNewBuffer(id s, SEL c, NSUInteger len, NSUInteger opts) {
    XeHeap *x = heapOf(s);
    id b = makeBuffer(x->dev, NULL, len, opts, nil);
    if (b) x->used += heapRound(len);
    return b;
}
static id xe_heapNewBufferOff(id s, SEL c, NSUInteger len, NSUInteger opts, NSUInteger off) { return xe_heapNewBuffer(s, c, len, opts); }
static id xe_newTexture(id self, SEL _cmd, id desc);
static id xe_heapNewTexture(id s, SEL c, id desc) {
    XeHeap *x = heapOf(s);
    id t = xe_newTexture(x->dev, sel_registerName("newTextureWithDescriptor:"), desc);
    if (t) x->used += heapRound(((NSUInteger (*)(id, SEL))objc_msgSend)(t, sel_registerName("allocatedSize")));
    return t;
}
static id xe_heapNewTextureOff(id s, SEL c, id desc, NSUInteger off) { return xe_heapNewTexture(s, c, desc); }
typedef struct { NSUInteger size, align; } XeSizeAlign;
static XeSizeAlign xe_heapTexSizeAlign(id self, SEL _cmd, id desc) {      // device: what a texture would take
    NSUInteger w = ((NSUInteger (*)(id, SEL))objc_msgSend)(desc, sel_registerName("width"));
    NSUInteger h = ((NSUInteger (*)(id, SEL))objc_msgSend)(desc, sel_registerName("height"));
    NSUInteger l = ((NSUInteger (*)(id, SEL))objc_msgSend)(desc, sel_registerName("arrayLength"));
    NSUInteger m = ((NSUInteger (*)(id, SEL))objc_msgSend)(desc, sel_registerName("mipmapLevelCount"));
    NSUInteger t = ((NSUInteger (*)(id, SEL))objc_msgSend)(desc, sel_registerName("textureType"));
    NSUInteger bytes = ((w * 16 + 63) & ~(NSUInteger)63) * (h + 32) * (l ? l : 1) * (t == 5 || t == 6 ? 6 : 1);
    if (m > 1) bytes = bytes * 3 / 2;
    XeSizeAlign r = { heapRound(bytes), 4096 };
    return r;
}
static XeSizeAlign xe_heapBufSizeAlign(id self, SEL _cmd, NSUInteger len, NSUInteger opts) {
    XeSizeAlign r = { heapRound(len), 4096 };
    return r;
}
static void registerHeapClass(Class dev) {
    gHeapClass = objc_allocateClassPair([NSObject class], "IrisXeMTLHeap", 0);
    if (!gHeapClass) return;
    Class h = gHeapClass;
    class_addIvar(h, "_xe", sizeof(void *), 3, "^v");
    add(h, "dealloc", (IMP)xe_heapDealloc, "v16@0:8");
    add(h, "device", (IMP)xe_heapDevice, "@16@0:8");
    add(h, "label", (IMP)xe_heapLabel, "@16@0:8");
    add(h, "setLabel:", (IMP)xe_heapSetLabel, "v24@0:8@16");
    add(h, "size", (IMP)xe_heapSize, "Q16@0:8");
    add(h, "usedSize", (IMP)xe_heapUsed, "Q16@0:8");
    add(h, "currentAllocatedSize", (IMP)xe_heapSize, "Q16@0:8");
    add(h, "storageMode", (IMP)xe_heapStorage, "Q16@0:8");
    add(h, "cpuCacheMode", (IMP)xe_heapCpu, "Q16@0:8");
    add(h, "hazardTrackingMode", (IMP)xe_heapHazard, "Q16@0:8");
    add(h, "resourceOptions", (IMP)xe_heapOptions, "Q16@0:8");
    add(h, "type", (IMP)xe_heapType, "q16@0:8");
    add(h, "maxAvailableSizeWithAlignment:", (IMP)xe_heapMaxAvail, "Q24@0:8Q16");
    add(h, "setPurgeableState:", (IMP)xe_heapPurgeable, "Q24@0:8Q16");
    add(h, "newBufferWithLength:options:", (IMP)xe_heapNewBuffer, "@32@0:8Q16Q24");
    add(h, "newBufferWithLength:options:offset:", (IMP)xe_heapNewBufferOff, "@40@0:8Q16Q24Q32");
    add(h, "newTextureWithDescriptor:", (IMP)xe_heapNewTexture, "@24@0:8@16");
    add(h, "newTextureWithDescriptor:offset:", (IMP)xe_heapNewTextureOff, "@32@0:8@16Q24");
    add(h, "methodSignatureForSelector:", (IMP)xe_texSig, "@24@0:8:16");
    add(h, "forwardInvocation:", (IMP)xe_texForward, "v24@0:8@16");
    Protocol *hp = objc_getProtocol("MTLHeap");
    if (hp) class_addProtocol(h, hp);
    objc_registerClassPair(h);
    gHeapIvar = class_getInstanceVariable(h, "_xe");
    add(dev, "newHeapWithDescriptor:", (IMP)xe_newHeap, "@24@0:8@16");
    add(dev, "heapTextureSizeAndAlignWithDescriptor:", (IMP)xe_heapTexSizeAlign, "{?=QQ}24@0:8@16");
    add(dev, "heapBufferSizeAndAlignWithLength:options:", (IMP)xe_heapBufSizeAlign, "{?=QQ}32@0:8Q16Q24");
}
// v0.42.0: one sample per pixel only (no MSAA yet)
// v0.51.6: WindowServer crashed in CA's blur ("Tile render pipelines are not supported on this device"): it had
// taken the tile-shader path, which only Apple-silicon GPUs have. Answer like a real Intel Mac GPU: Common 1-3 and
// Mac 1-2 (MTLGPUFamily 3001..3003, 2001, 2002), no Apple family (1001..1009), no Metal 3 (5001) / Metal 4 (5002).
// Every family asked is logged once.
static BOOL xe_supportsFamily(id self, SEL _cmd, NSInteger fam) {
    BOOL yes = fam == 2001 || fam == 2002 || fam == 3001 || fam == 3002 || fam == 3003 || fam == 4001 || fam == 4002;
    static volatile uint32_t seen[8]; static volatile int n = 0;
    int i, known = 0;
    for (i = 0; i < n && i < 8; i++) if (seen[i] == (uint32_t)fam) known = 1;
    if (!known && n < 8) { seen[n++] = (uint32_t)fam; os_log(gLog, "IrisXeMTL: supportsFamily %ld -> %d", (long)fam, yes); }
    return yes;
}
// tile shaders (newRenderPipelineStateWithTileDescriptor:): none on this GPU; fail cleanly with an error
static id xe_newTilePipeline(id self, SEL _cmd, id desc, NSUInteger opts, void *refl, NSError **err) {
    os_log_error(gLog, "IrisXeMTL: tile render pipelines are not supported");
    if (err) *err = [NSError errorWithDomain:@"IrisXeMTL" code:1 userInfo:@{ NSLocalizedDescriptionKey: @"tile render pipelines are not supported" }];
    return nil;
}
static BOOL xe_supportsSampleCount(id self, SEL _cmd, NSUInteger n) { return n == 1; }
// v0.42.0: our command buffers run in order, one job at a time: fences have nothing to wait for
static void xe_encNop1(id self, SEL _cmd, id a) { }
static void xe_encNop2(id self, SEL _cmd, id a, NSUInteger b) { }
static void xe_encAmplification(id self, SEL _cmd, NSUInteger n, const void *m) {
    if (n > 1) os_log_error(gLog, "IrisXeMTL: setVertexAmplificationCount %lu: only 1 yet", (unsigned long)n);
}
static void addFenceNops(Class c) {
    if (!c) return;
    class_addMethod(c, sel_registerName("updateFence:"), (IMP)xe_encNop1, "v24@0:8@16");
    class_addMethod(c, sel_registerName("waitForFence:"), (IMP)xe_encNop1, "v24@0:8@16");
    class_addMethod(c, sel_registerName("updateFence:afterStages:"), (IMP)xe_encNop2, "v32@0:8@16Q24");
    class_addMethod(c, sel_registerName("waitForFence:beforeStages:"), (IMP)xe_encNop2, "v32@0:8@16Q24");
}


#include "IrisXeMTLRender.inc"                         // milestone 5c: render pipelines + encoders
static bool bootArg(const char *arg) {
    char buf[1024] = ""; size_t len = sizeof(buf) - 1;
    if (sysctlbyname("kern.bootargs", buf, &len, NULL, 0) != 0) return false;
    buf[len] = 0;
    for (char *t = strtok(buf, " "); t; t = strtok(NULL, " ")) if (!strcmp(t, arg)) return true;
    return false;
}

// Our device can't draw yet. If WindowServer, the Dock or any app got it, they
// would try to use it and crash (that was the boot hang in v0.18.0). So the
// device only exists in processes that ask for it:
//   - our test tools irisxe-metaltest and irisxe-mtkdemo
//   - any program started with environment variable IRISXE_MTL=1
//   - every process, only with boot-arg -irisxemtlall (testing only!)
//   - v0.65.0: programs in the app list (appListed below)
// Everywhere else the class is not registered, so Metal skips the GPU, exactly
// like before the bundle was installed.
// v0.51.0: the desktop test, for one boot only. /Library/Preferences/irisxe-desktop holds a boot session UUID
// (sysctl kern.bootsessionuuid); while it matches this boot, every process (WindowServer too) gets our Metal
// device. After any reboot the UUID differs and everything is back to normal by itself.
// v0.51.21: Metal binary archives hold shaders precompiled for Apple's GPUs. Opening one from a file
// (RenderBox, the SwiftUI renderer of ControlCenter / NotificationCenter, does at its first draw) made Metal look for
// our GPU's slice and stop the app with an assert. We make no archives: the call fails with an error (apps handle an
// archive that can't be opened), and every pipeline is built by our compiler, as everywhere else.
static id xe_newBinaryArchive(id self, SEL _cmd, id desc, NSError **err) {
    id url = desc && [desc respondsToSelector:sel_registerName("url")] ? ((id (*)(id, SEL))objc_msgSend)(desc, sel_registerName("url")) : nil;
    os_log(gLog, "IrisXeMTL: binary archive %{public}@: not used (pipelines are compiled by our driver)",
           url && [url respondsToSelector:@selector(lastPathComponent)] ? [url lastPathComponent] : @"(new)");
    if (err) *err = [NSError errorWithDomain:@"MTLBinaryArchiveDomain" code:1
                                    userInfo:@{ NSLocalizedDescriptionKey : @"binary archives are not supported on this GPU" }];
    return nil;
}
// v0.55.0: on a boot with the framebuffer named IrisXeIntelFB (sudo nvram irisxe-intelfb=1, one boot) CoreDisplay
// switches Apple's display system on, and that needs a Metal device in WindowServer from the first second - so that
// boot counts as the desktop test "ws" by itself. IOKit through dlsym: the bundle does not link it.
static bool intelFBThisBoot(void) {
    static int r = -1;
    if (r >= 0) return r;
    r = 0;
    void *h = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_LAZY | RTLD_NOLOAD);
    if (!h) h = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_LAZY);
    if (!h) return r;
    CFMutableDictionaryRef (*match)(const char *) = (CFMutableDictionaryRef (*)(const char *))dlsym(h, "IOServiceMatching");
    uint32_t (*get)(mach_port_t, CFDictionaryRef) = (uint32_t (*)(mach_port_t, CFDictionaryRef))dlsym(h, "IOServiceGetMatchingService");
    kern_return_t (*rel)(uint32_t) = (kern_return_t (*)(uint32_t))dlsym(h, "IOObjectRelease");
    if (!match || !get || !rel) return r;
    CFMutableDictionaryRef m = match("IrisXeIntelFB");
    uint32_t svc = m ? get(0, m) : 0;          // 0 = default main port; consumes m
    if (svc) { r = 1; rel(svc); }
    return r;
}
static bool desktopThisBoot(void) {
    if (intelFBThisBoot()) {
        bool ws = getprogname() && !strcmp(getprogname(), "WindowServer");
        if (ws) os_log(gLog, "IrisXeMTL: framebuffer is IrisXeIntelFB this boot - enabled in WindowServer (desktop test ws)");
        // v0.56.7: test words for this boot's WindowServer, written by irisxe-pipe-test.sh (removed again at its end):
        // "batch" = up to 16 draws per GPU job (one job per draw is the default since v0.51.37)
        // v0.57.0: batched draws are the default here (the kernel runs up to 4 per GPU job: no hang in any test);
        // the test word "nobatch" turns them off
        if (ws) gDesktopBatch = true;
        if (ws) {
            FILE *mf = fopen("/Library/Preferences/irisxe-ws-mode", "r");
            if (mf) {
                char words[64] = {0};
                size_t got = fread(words, 1, sizeof(words) - 1, mf);
                fclose(mf);
                words[got] = 0;
                if (strstr(words, "nobatch")) { gDesktopNoBatch = true; gDesktopBatch = false; }
                else if (strstr(words, "batch")) gDesktopBatch = true;
                os_log(gLog, "IrisXeMTL: WindowServer test words: %{public}s -> draws %{public}s", words,
                       gDesktopBatch ? "batched (the kernel runs up to 4 per GPU job)" : "one per GPU job");
            }
        }
        return ws;
    }
    char cur[64] = {0}, want[64] = {0};
    size_t n = sizeof(cur) - 1;
    if (sysctlbyname("kern.bootsessionuuid", cur, &n, NULL, 0) != 0 || !cur[0]) return false;
    FILE *f = fopen("/Library/Preferences/irisxe-desktop", "r");
    if (!f) return false;
    size_t r = fread(want, 1, sizeof(want) - 1, f);
    fclose(f);
    want[r] = 0;
    // v0.51.20: "<uuid>" = every process; "<uuid> ws" = WindowServer only; "<uuid> apps" = everything but
    // WindowServer (to tell whether a missing window is drawn wrong by its app or shown wrong by WindowServer)
    char *sp = want;
    while (*sp && *sp != ' ' && *sp != '\n' && *sp != '\r' && *sp != '\t') sp++;
    char *mode = sp;
    while (*mode == ' ' || *mode == '\t') mode++;
    *sp = 0;
    if (!want[0] || strcmp(want, cur)) return false;
    // v0.51.25: the word "copy" anywhere after the UUID: IOSurface textures are copies (refreshed before every read),
    // not the surface's pages (IRISXE_IOSCOPY=1 for the desktop test)
    if (strstr(mode, "copy")) gDesktopIOSCopy = true;
    // v0.51.33: "record": every process on our device records its GPU commands to /var/tmp/irisxe-record-<name>.bin
    if (strstr(mode, "record")) gDesktopRecord = true;
    if (strstr(mode, "nobatch")) gDesktopNoBatch = true;      // v0.51.35: one GPU job per draw
    else if (strstr(mode, "batch")) gDesktopBatch = true;     // v0.51.37: batched draws (16 per GPU job) again
    bool isWS = getprogname() && !strcmp(getprogname(), "WindowServer");
    if (!strncmp(mode, "ws", 2)) return isWS;
    if (!strncmp(mode, "apps", 4)) return !isWS;
    return true;
}
static NSUInteger xe_align64(id self, SEL _cmd, NSUInteger pf) { return 64; }   // v0.51.1
// v0.65.0: apps on the GPU, one program at a time. /Library/Preferences/irisxe-apps lists program names (one per line,
// '#' starts a comment); every listed program gets our Metal device. Without the file the built-in list is used:
// WallpaperSequoiaExtension (Sequoia's live wallpapers fail with SequoiaRenderer.Error 0 without a Metal device).
// A file with no names ("# none") turns the list off. Sandboxed programs may not be allowed to read the file: then
// the built-in list applies to them.
// v0.66.1: + WallpaperMacintoshExtension (the "Macintosh" dynamic wallpaper: WallpaperDestinationError 0 without it)
static const char *kBuiltinApps[] = { "WallpaperSequoiaExtension", "WallpaperMacintoshExtension", NULL };
static int gListSource;                                  // 0 none, 1 file, 2 built-in, 3 NVRAM (for the log)
// v0.68.22: the accelerator's property IrisXe-Apps (from NVRAM irisxe-apps, space separated) - readable by sandboxed
// apps, which cannot open /Library/Preferences/irisxe-apps (apps test, 24 Nov 2026: every app "device hidden")
// v0.68.23: one of the accelerator's app lists (IrisXe-Apps / IrisXe-NoApps); "*" matches every program
static bool regListed(CFStringRef key, const char *p, bool star) {
    void *h = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_LAZY | RTLD_NOLOAD);
    if (!h) h = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_LAZY);
    if (!h || !p) return false;
    CFMutableDictionaryRef (*match)(const char *) = (CFMutableDictionaryRef (*)(const char *))dlsym(h, "IOServiceMatching");
    uint32_t (*get)(mach_port_t, CFDictionaryRef) = (uint32_t (*)(mach_port_t, CFDictionaryRef))dlsym(h, "IOServiceGetMatchingService");
    CFTypeRef (*prop)(uint32_t, CFStringRef, CFAllocatorRef, uint32_t) =
        (CFTypeRef (*)(uint32_t, CFStringRef, CFAllocatorRef, uint32_t))dlsym(h, "IORegistryEntryCreateCFProperty");
    kern_return_t (*rel)(uint32_t) = (kern_return_t (*)(uint32_t))dlsym(h, "IOObjectRelease");
    if (!match || !get || !prop || !rel) return false;
    CFMutableDictionaryRef m = match("IrisXeAccelerator");
    uint32_t svc = m ? get(0, m) : 0;
    if (!svc) return false;
    bool hit = false;
    CFTypeRef v = prop(svc, key, kCFAllocatorDefault, 0);
    if (v && CFGetTypeID(v) == CFStringGetTypeID()) {
        char list[512] = {0};
        if (CFStringGetCString((CFStringRef)v, list, sizeof(list), kCFStringEncodingUTF8)) {
            char *save = NULL;
            for (char *t = strtok_r(list, " \t", &save); t && !hit; t = strtok_r(NULL, " \t", &save))
                if ((star && !strcmp(t, "*")) || !strcmp(t, p)) hit = true;
        }
    }
    if (v) CFRelease(v);
    rel(svc);
    return hit;
}
// program names with spaces ("System Settings", "QuickTime Player") are listed with '_' for the space
static bool regListedName(CFStringRef key, const char *p, bool star) {
    if (regListed(key, p, star)) return true;
    char alt[128];
    strlcpy(alt, p ? p : "", sizeof(alt));
    bool sp = false;
    for (char *c = alt; *c; c++) if (*c == ' ') { *c = '_'; sp = true; }
    return sp && regListed(key, alt, star);
}
static bool nvramListed(const char *p) { return regListedName(CFSTR("IrisXe-Apps"), p, true); }
// v0.68.26: named one by one in irisxe-apps (not just "*"): wins over the built-in "keep on the CPU" list (tests)
static bool nvramNamed(const char *p) { return regListedName(CFSTR("IrisXe-Apps"), p, false); }
// v0.68.24: kept on the CPU by default: they crash in Core Image's old kernel language (CIKernel kernelWithString:,
// CIKernelReflection::consolidate -> NULL) - apps test, 24 Nov 2026
// v0.68.25: + iconservicesagent (draws app and file icons for Finder and the Dock: with our device they came out empty
// - Finder lists without icons, Books without a Dock icon) and every Chromium / Electron GPU helper ("... Helper (GPU)":
// GitHub Desktop's window stayed empty; Brave's own GPU helper never took our device)
static const char *kBuiltinNoApps[] = { "Preview", "Photo Booth", "iconservicesagent", NULL };
static bool nvramBlocked(const char *p) {
    if (!p) return false;
    if (regListedName(CFSTR("IrisXe-NoApps"), p, true)) return true;
    if (nvramNamed(p)) return false;
    for (int i = 0; kBuiltinNoApps[i]; i++) if (!strcmp(p, kBuiltinNoApps[i])) return true;
    if (strstr(p, " Helper (GPU)")) return true;
    // v0.68.35: any Chromium / Electron GPU process, whatever it is called ("Claude Helper", "GitHub Desktop
    // Helper", "Brave Browser Helper (GPU)", VS Code, Slack ...): started with --type=gpu-process. Their Dawn / ANGLE
    // shaders still hang the GPU (Claude: black flashes; GitHub Desktop: lag); without our device they draw with
    // their own software renderer, as before. Named explicitly in irisxe-apps: allowed (tests).
    {
        static int gpuProc = -1;
        if (gpuProc < 0) {
            gpuProc = 0;
            for (NSString *a in [[NSProcessInfo processInfo] arguments])
                if ([a isEqualToString:@"--type=gpu-process"]) { gpuProc = 1; break; }
        }
        if (gpuProc) return true;
    }
    return false;
}
static bool appListed(const char *p) {
    if (!p) return false;
    if (nvramListed(p)) { gListSource = 3; return true; }
    FILE *f = fopen("/Library/Preferences/irisxe-apps", "r");
    if (!f) {
        gListSource = 2;
        for (int i = 0; kBuiltinApps[i]; i++) if (!strcmp(p, kBuiltinApps[i])) return true;
        return false;
    }
    gListSource = 1;
    char line[256];
    bool hit = false;
    while (!hit && fgets(line, sizeof(line), f)) {
        char *h = strchr(line, '#');
        if (h) *h = 0;
        char *a = line;
        while (*a == ' ' || *a == '\t') a++;
        char *e = a + strlen(a);
        while (e > a && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        if (*a && !strcmp(a, p)) hit = true;
    }
    fclose(f);
    return hit;
}
// v0.68.4: VideoToolbox lists a hardware decoder only when a METAL DEVICE of the process has the decoder's IORegistry
// key (research round 43: VTDoesAnyDeviceSupportIORegistryKey walks VTCopyMetalDeviceArray and checks each device's
// registry entry). With our device hidden, the video processes saw no GPU and no Intel decoder at all. So on the
// video test boot (the accelerator announces IODVDBundleName) the video processes get our Metal device.
static const char *kVideoApps[] = { "irisxe-vtdecode", "VTDecoderXPCService", "VTEncoderXPCService", NULL };
static bool videoTestThisBoot(void) {
    static int r = -1;
    if (r >= 0) return r;
    r = 0;
    void *h = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_LAZY | RTLD_NOLOAD);
    if (!h) h = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_LAZY);
    if (!h) return r;
    CFMutableDictionaryRef (*match)(const char *) = (CFMutableDictionaryRef (*)(const char *))dlsym(h, "IOServiceMatching");
    uint32_t (*get)(mach_port_t, CFDictionaryRef) = (uint32_t (*)(mach_port_t, CFDictionaryRef))dlsym(h, "IOServiceGetMatchingService");
    CFTypeRef (*prop)(uint32_t, CFStringRef, CFAllocatorRef, uint32_t) =
        (CFTypeRef (*)(uint32_t, CFStringRef, CFAllocatorRef, uint32_t))dlsym(h, "IORegistryEntryCreateCFProperty");
    kern_return_t (*rel)(uint32_t) = (kern_return_t (*)(uint32_t))dlsym(h, "IOObjectRelease");
    if (!match || !get || !prop || !rel) return r;
    CFMutableDictionaryRef m = match("IrisXeAccelerator");
    uint32_t svc = m ? get(0, m) : 0;          // consumes m
    if (svc) {
        CFTypeRef v = prop(svc, CFSTR("IODVDBundleName"), kCFAllocatorDefault, 0);
        if (v) { r = 1; CFRelease(v); }
        rel(svc);
    }
    return r;
}
static bool videoAppHere(const char *p) {
    if (!p) return false;
    bool listed = false;
    for (int i = 0; kVideoApps[i]; i++) if (!strcmp(p, kVideoApps[i])) listed = true;
    return listed && videoTestThisBoot();
}
static bool allowedHere(void) {
    const char *p = getprogname();
    if (p && (!strcmp(p, "irisxe-metaltest") || !strcmp(p, "irisxe-mtkdemo"))) return true;   // v0.40.2: + the demo
    const char *e = getenv("IRISXE_MTL");
    if (e && *e == '1') return true;
    if (desktopThisBoot()) return true;                  // v0.51.0
    if (nvramBlocked(p)) return false;                   // v0.68.23: NVRAM irisxe-noapps keeps it on the CPU
    if (appListed(p)) return true;                       // v0.65.0
    if (videoAppHere(p)) return true;                    // v0.68.4: the video test boot
    return bootArg("-irisxemtlall");
}

__attribute__((constructor))
static void xe_register(void) {
    gLog = os_log_create("com.irisxe.IrisXeMTLDriver", "device");
    if (objc_getClass("IrisXeMTLDevice")) return;        // already registered
    if (!allowedHere()) {
        os_log(gLog, "IrisXeMTL: not enabled in %{public}s (device hidden)", getprogname());
        return;
    }
    os_log(gLog, "IrisXeMTL: enabled in %{public}s (pid %d)%{public}s", getprogname(), getpid(),
           desktopThisBoot() ? " - desktop test this boot"
           : videoAppHere(getprogname()) ? " - video test boot"
           : gListSource == 1 ? " - listed in /Library/Preferences/irisxe-apps"
           : gListSource == 2 ? " - built-in app list"
           : gListSource == 3 ? " - listed in NVRAM irisxe-apps" : "");   // v0.51.0, v0.65.0, v0.68.4
    gSuper = objc_getClass("MTLIOAccelDevice");
    if (!gSuper) { os_log_error(gLog, "IrisXeMTL: MTLIOAccelDevice not found (Metal not loaded?)"); return; }
    Class c = objc_allocateClassPair(gSuper, "IrisXeMTLDevice", 0);
    if (!c) { os_log_error(gLog, "IrisXeMTL: objc_allocateClassPair failed"); return; }
    add(c, "initWithAcceleratorPort:", (IMP)xe_initWithAcceleratorPort, "@20@0:8I16");
    add(c, "acceleratorPort", (IMP)xe_acceleratorPort, "I16@0:8");                        // v0.54.6: diagnostic
    add(c, "name",        (IMP)xe_name,        "@16@0:8");
    add(c, "vendorName",  (IMP)xe_vendorName,  "@16@0:8");
    add(c, "familyName",  (IMP)xe_familyName,  "@16@0:8");
    add(c, "productName", (IMP)xe_productName, "@16@0:8");
    add(c, "featureProfile", (IMP)xe_featureProfile, "Q16@0:8");
    add(c, "maxThreadsPerThreadgroup", (IMP)xe_maxThreadsPerThreadgroup, "{?=QQQ}16@0:8");
    add(c, "maxThreadgroupMemoryLength", (IMP)xe_maxThreadgroupMemoryLength, "Q16@0:8");
    add(c, "isLowPower",  (IMP)xe_no,          "c16@0:8");
    add(c, "isHeadless",  (IMP)xe_no,          "c16@0:8");
    add(c, "isRemovable", (IMP)xe_no,          "c16@0:8");
    add(c, "hasUnifiedMemory", (IMP)xe_yes,    "c16@0:8");
    add(c, "methodSignatureForSelector:", (IMP)xe_methodSignatureForSelector, "@24@0:8:16");
    add(c, "forwardInvocation:", (IMP)xe_forwardInvocation, "v24@0:8@16");
    gMissing = [[NSMutableSet alloc] init];
    add(c, "newBufferWithLength:options:", (IMP)xe_newBufferWithLength, "@32@0:8Q16Q24");
    add(c, "newBufferWithBytes:length:options:", (IMP)xe_newBufferWithBytes, "@40@0:8r^v16Q24Q32");
    add(c, "newBufferWithBytesNoCopy:length:options:deallocator:", (IMP)xe_newBufferWithBytesNoCopy, "@48@0:8^v16Q24Q32@?40");
    // milestone 4
    registerTextureClasses();
    add(c, "newTextureWithDescriptor:", (IMP)xe_newTexture, "@24@0:8@16");
    add(c, "newTextureWithDescriptor:iosurface:plane:", (IMP)xe_newTextureIOS, "@40@0:8@16^{__IOSurface=}24Q32");   // v0.38.0
    add(c, "minimumLinearTextureAlignmentForPixelFormat:", (IMP)xe_align64, "Q24@0:8Q16");   // v0.51.1: rows / offsets of
    add(c, "minimumTextureBufferAlignmentForPixelFormat:", (IMP)xe_align64, "Q24@0:8Q16");   // textures on buffers
    add(c, "newSamplerStateWithDescriptor:", (IMP)xe_newSampler, "@24@0:8@16");
    registerHeapClass(c);                                                                 // v0.42.0
    add(c, "newBinaryArchiveWithDescriptor:error:", (IMP)xe_newBinaryArchive, "@32@0:8@16^@24");
    add(c, "supportsSampleCount:", (IMP)xe_supportsSampleCount, "c24@0:8Q16");
    add(c, "supportsFamily:", (IMP)xe_supportsFamily, "c24@0:8q16");                       // v0.51.6
    add(c, "newRenderPipelineStateWithTileDescriptor:options:reflection:error:", (IMP)xe_newTilePipeline, "@48@0:8@16Q24^@32^@40");
    add(c, "supportsTileShaders", (IMP)xe_no, "c16@0:8");

    add(c, "newCommandQueueWithDescriptor:", (IMP)xe_newCommandQueueWithDescriptor, "@24@0:8@16");
    add(c, "irisxeNewComputePipelineWithKernel:", (IMP)xe_newComputePipelineWithKernel, "@24@0:8@16");
    // milestone 3c: compute pipelines from Metal functions, compiled by us
    add(c, "newComputePipelineStateWithFunction:error:", (IMP)xe_newCPSWithFunction, "@32@0:8@16^@24");
    add(c, "newComputePipelineStateWithFunction:options:reflection:error:", (IMP)xe_newCPSWithFunctionOpts, "@48@0:8@16Q24^@32^@40");
    add(c, "newComputePipelineStateWithDescriptor:options:reflection:error:", (IMP)xe_newCPSWithDescriptor, "@48@0:8@16Q24^@32^@40");
    add(c, "newComputePipelineStateWithFunction:completionHandler:", (IMP)xe_newCPSWithFunctionHandler, "v32@0:8@16@?24");
    add(c, "irisxeLastCompileLog", (IMP)xe_lastCompileLog, "@16@0:8");
    Class qsup = objc_getClass("MTLIOAccelCommandQueue"), csup = objc_getClass("MTLIOAccelCommandBuffer");
    gQueueClass = qsup ? objc_allocateClassPair(qsup, "IrisXeMTLCommandQueue", 0) : Nil;
    if (gQueueClass) {
        class_addMethod(gQueueClass, sel_registerName("commandBuffer"), (IMP)xe_commandBuffer, "@16@0:8");
        class_addMethod(gQueueClass, sel_registerName("commandBufferWithUnretainedReferences"), (IMP)xe_commandBufferUnretained, "@16@0:8");
        class_addProtocol(gQueueClass, @protocol(MTLCommandQueue));   // v0.51.15: Metal Performance Shaders checks conformsToProtocol
        objc_registerClassPair(gQueueClass);
    } else os_log_error(gLog, "IrisXeMTL: MTLIOAccelCommandQueue not found");
    gCmdBufClass = csup ? objc_allocateClassPair(csup, "IrisXeMTLCommandBuffer", 0) : Nil;
    if (gCmdBufClass) {
        class_addMethod(gCmdBufClass, sel_registerName("blitCommandEncoder"), (IMP)xe_blitCommandEncoder, "@16@0:8");
        // v0.38.0: CAMetalLayer drawables: present after the GPU is done (and the picture copied into the IOSurface)
        class_addMethod(gCmdBufClass, sel_registerName("presentDrawable:"), (IMP)xe_cbPresent, "v24@0:8@16");
        class_addMethod(gCmdBufClass, sel_registerName("presentDrawable:atTime:"), (IMP)xe_cbPresentAt, "v32@0:8@16d24");
        class_addMethod(gCmdBufClass, sel_registerName("presentDrawable:afterMinimumDuration:"), (IMP)xe_cbPresentAt, "v32@0:8@16d24");
        class_addProtocol(gCmdBufClass, @protocol(MTLCommandBuffer));   // v0.51.15: Metal Performance Shaders checks conformsToProtocol
        objc_registerClassPair(gCmdBufClass);
    } else os_log_error(gLog, "IrisXeMTL: MTLIOAccelCommandBuffer not found");
    Class esup = objc_getClass("MTLIOAccelBlitCommandEncoder");
    gBlitClass = esup ? objc_allocateClassPair(esup, "IrisXeMTLBlitCommandEncoder", 0) : Nil;
    if (gBlitClass) {
        class_addMethod(gBlitClass, sel_registerName("fillBuffer:range:value:"), (IMP)xe_fillBuffer, "v44@0:8@16{_NSRange=QQ}24C40");
        class_addMethod(gBlitClass, sel_registerName("copyFromBuffer:sourceOffset:toBuffer:destinationOffset:size:"),
                        (IMP)xe_copyBuffer, "v56@0:8@16Q24@32Q40Q48");
        // v0.36.0: copies with textures
        class_addMethod(gBlitClass, sel_registerName("copyFromBuffer:sourceOffset:sourceBytesPerRow:sourceBytesPerImage:sourceSize:toTexture:destinationSlice:destinationLevel:destinationOrigin:"),
                        (IMP)xe_blitBufToTex, "v@:@QQQ{?=QQQ}@QQ{?=QQQ}");
        class_addMethod(gBlitClass, sel_registerName("copyFromBuffer:sourceOffset:sourceBytesPerRow:sourceBytesPerImage:sourceSize:toTexture:destinationSlice:destinationLevel:destinationOrigin:options:"),
                        (IMP)xe_blitBufToTexOpt, "v@:@QQQ{?=QQQ}@QQ{?=QQQ}Q");
        class_addMethod(gBlitClass, sel_registerName("copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toBuffer:destinationOffset:destinationBytesPerRow:destinationBytesPerImage:"),
                        (IMP)xe_blitTexToBuf, "v@:@QQ{?=QQQ}{?=QQQ}@QQQ");
        class_addMethod(gBlitClass, sel_registerName("copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toBuffer:destinationOffset:destinationBytesPerRow:destinationBytesPerImage:options:"),
                        (IMP)xe_blitTexToBufOpt, "v@:@QQ{?=QQQ}{?=QQQ}@QQQQ");
        class_addMethod(gBlitClass, sel_registerName("copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toTexture:destinationSlice:destinationLevel:destinationOrigin:"),
                        (IMP)xe_blitTexToTex, "v@:@QQ{?=QQQ}{?=QQQ}@QQ{?=QQQ}");
        class_addMethod(gBlitClass, sel_registerName("copyFromTexture:toTexture:"), (IMP)xe_blitTexToTexAll, "v@:@@");
        class_addMethod(gBlitClass, sel_registerName("generateMipmapsForTexture:"), (IMP)xe_blitGenMips, "v@:@");
        class_addMethod(gBlitClass, sel_registerName("synchronizeResource:"), (IMP)xe_blitNop1, "v@:@");
        addFenceNops(gBlitClass);                                                         // v0.42.0
        class_addProtocol(gBlitClass, @protocol(MTLBlitCommandEncoder));   // v0.51.15: Metal Performance Shaders checks conformsToProtocol
        objc_registerClassPair(gBlitClass);
    } else os_log_error(gLog, "IrisXeMTL: MTLIOAccelBlitCommandEncoder not found");

    // milestone 3a: compute pipeline + encoder
    gPipeClass = objc_allocateClassPair([NSObject class], "IrisXeMTLComputePipelineState", 0);
    if (gPipeClass) {
        class_addIvar(gPipeClass, "_code", sizeof(id), 3, "@");
        class_addMethod(gPipeClass, sel_registerName("dealloc"), (IMP)xe_pipeDealloc, "v16@0:8");
        class_addMethod(gPipeClass, sel_registerName("maxTotalThreadsPerThreadgroup"), (IMP)xe_pipeMaxTPG, "Q16@0:8");
        class_addMethod(gPipeClass, sel_registerName("threadExecutionWidth"), (IMP)xe_pipeWidth, "Q16@0:8");
        class_addIvar(gPipeClass, "_info", sizeof(uint32_t), 2, "I");
        class_addIvar(gPipeClass, "_codeBuf", sizeof(id), 3, "@");
        class_addIvar(gPipeClass, "_scratch", sizeof(id), 3, "@");
        class_addIvar(gPipeClass, "_csamp", 4 * sizeof(uint64_t), 3, "[4Q]");
        class_addMethod(gPipeClass, sel_registerName("staticThreadgroupMemoryLength"), (IMP)xe_pipeTGMem, "Q16@0:8");
        class_addProtocol(gPipeClass, @protocol(MTLComputePipelineState));
        objc_registerClassPair(gPipeClass);
        gPipeCode = class_getInstanceVariable(gPipeClass, "_code");
        gPipeInfo = class_getInstanceVariable(gPipeClass, "_info");
        gPipeCodeBuf = class_getInstanceVariable(gPipeClass, "_codeBuf");
        gPipeScratch = class_getInstanceVariable(gPipeClass, "_scratch");
        gPipeCSamp = class_getInstanceVariable(gPipeClass, "_csamp");
    }
    Class gen = esup ? class_getSuperclass(esup) : Nil;   // Apple's generic MTLIOAccel encoder
    gComputeClass = gen ? objc_allocateClassPair(gen, "IrisXeMTLComputeCommandEncoder", 0) : Nil;
    if (gComputeClass) {
        class_addIvar(gComputeClass, "_xeState", sizeof(void *), 3, "^v");
        class_addMethod(gComputeClass, sel_registerName("dealloc"), (IMP)xe_computeDealloc, "v16@0:8");
        class_addMethod(gComputeClass, sel_registerName("setComputePipelineState:"), (IMP)xe_setPipeline, "v24@0:8@16");
        class_addMethod(gComputeClass, sel_registerName("setBuffer:offset:atIndex:"), (IMP)xe_setBuffer, "v40@0:8@16Q24Q32");
        class_addMethod(gComputeClass, sel_registerName("setBufferOffset:atIndex:"), (IMP)xe_setBufferOffset, "v32@0:8Q16Q24");
        class_addMethod(gComputeClass, sel_registerName("setThreadgroupMemoryLength:atIndex:"), (IMP)xe_setTGMemLength, "v32@0:8Q16Q24");
        class_addMethod(gComputeClass, sel_registerName("setTexture:atIndex:"), (IMP)xe_setTexture, "v32@0:8@16Q24");
        class_addMethod(gComputeClass, sel_registerName("setTextures:withRange:"), (IMP)xe_setTextures, "v40@0:8^@16{_NSRange=QQ}24");
        class_addMethod(gComputeClass, sel_registerName("setSamplerState:atIndex:"), (IMP)xe_setSampler, "v32@0:8@16Q24");
        class_addMethod(gComputeClass, sel_registerName("setSamplerStates:withRange:"), (IMP)xe_setSamplers, "v40@0:8^@16{_NSRange=QQ}24");
        class_addMethod(gComputeClass, sel_registerName("setSamplerState:lodMinClamp:lodMaxClamp:atIndex:"), (IMP)xe_setSamplerClamp, "v40@0:8@16f24f28Q32");
        class_addMethod(gComputeClass, sel_registerName("dispatchThreadgroups:threadsPerThreadgroup:"),
                        (IMP)xe_dispatchThreadgroups, "v64@0:8{?=QQQ}16{?=QQQ}40");
        class_addMethod(gComputeClass, sel_registerName("dispatchThreads:threadsPerThreadgroup:"),
                        (IMP)xe_dispatchThreads, "v64@0:8{?=QQQ}16{?=QQQ}40");
        addFenceNops(gComputeClass);                                                      // v0.42.0
        class_addMethod(gComputeClass, sel_registerName("setBytes:length:atIndex:"), (IMP)xe_cSetBytes, "v40@0:8r^v16Q24Q32");   // v0.51.16
        class_addMethod(gComputeClass, sel_registerName("setBuffers:offsets:withRange:"), (IMP)xe_cSetBuffers, "v48@0:8r^@16r^Q24{_NSRange=QQ}32");
        class_addMethod(gComputeClass, sel_registerName("useResource:usage:"), (IMP)xe_cNop1Q, "v32@0:8@16Q24");
        class_addMethod(gComputeClass, sel_registerName("useResources:count:usage:"), (IMP)xe_cNopPQQ, "v40@0:8r^@16Q24Q32");
        class_addMethod(gComputeClass, sel_registerName("useHeap:"), (IMP)xe_cNop1, "v24@0:8@16");
        class_addMethod(gComputeClass, sel_registerName("useHeaps:count:"), (IMP)xe_cNopPQ, "v32@0:8r^@16Q24");
        class_addMethod(gComputeClass, sel_registerName("memoryBarrierWithScope:"), (IMP)xe_cNopQ, "v24@0:8Q16");
        class_addMethod(gComputeClass, sel_registerName("memoryBarrierWithResources:count:"), (IMP)xe_cNopPQ, "v32@0:8r^@16Q24");
        class_addMethod(gComputeClass, sel_registerName("setImageblockWidth:height:"), (IMP)xe_cNopQQ, "v32@0:8Q16Q24");
        class_addProtocol(gComputeClass, @protocol(MTLComputeCommandEncoder));   // v0.51.15: Metal Performance Shaders checks conformsToProtocol
        objc_registerClassPair(gComputeClass);
        gComputeState = class_getInstanceVariable(gComputeClass, "_xeState");
        os_log(gLog, "IrisXeMTL: compute encoder based on %{public}s", class_getName(gen));
    } else os_log_error(gLog, "IrisXeMTL: no base class for the compute encoder");
    registerRenderClasses(c, gen, gCmdBufClass);          // milestone 5c
    if (gCmdBufClass && gComputeClass) {
        class_addMethod(gCmdBufClass, sel_registerName("computeCommandEncoder"), (IMP)xe_computeCommandEncoder, "@16@0:8");
        class_addMethod(gCmdBufClass, sel_registerName("computeCommandEncoderWithDispatchType:"),
                        (IMP)xe_computeCommandEncoderType, "@24@0:8Q16");
    }

    Class bsup = objc_getClass("MTLIOAccelBuffer");
    gBufClass = bsup ? objc_allocateClassPair(bsup, "IrisXeMTLBuffer", 0) : Nil;
    if (gBufClass) {
        // v0.51.1: textures on buffers; methods we don't have yet are logged and return 0 instead of crashing
        // the process (WindowServer crashed on newTextureWithDescriptor:offset:bytesPerRow:)
        class_addMethod(gBufClass, sel_registerName("newTextureWithDescriptor:offset:bytesPerRow:"), (IMP)xe_bufNewTexture, "@40@0:8@16Q24Q32");
        class_addMethod(gBufClass, sel_registerName("methodSignatureForSelector:"), (IMP)xe_texSig, "@24@0:8:16");
        class_addMethod(gBufClass, sel_registerName("forwardInvocation:"), (IMP)xe_texForward, "v24@0:8@16");
        class_addProtocol(gBufClass, @protocol(MTLBuffer));   // v0.51.15: Metal Performance Shaders checks conformsToProtocol
        objc_registerClassPair(gBufClass);
    } else os_log_error(gLog, "IrisXeMTL: MTLIOAccelBuffer not found");
    // Metal checks [device conformsToProtocol:@protocol(MTLDevice)] before it
    // adds a GPU (v0.18.0 failed exactly there). Apple's drivers declare
    // MTLDeviceSPI (which includes MTLDevice); we declare both.
    class_addProtocol(c, @protocol(MTLDevice));
    Protocol *spi = objc_getProtocol("MTLDeviceSPI");
    if (spi) class_addProtocol(c, spi);
    objc_registerClassPair(c);
    os_log(gLog, "IrisXeMTL: protocols MTLDevice=%d MTLDeviceSPI=%d", class_conformsToProtocol(c, @protocol(MTLDevice)),
           spi ? class_conformsToProtocol(c, spi) : -1);
    os_log(gLog, "IrisXeMTL: class IrisXeMTLDevice registered (instance size %zu)", class_getInstanceSize(c));
}

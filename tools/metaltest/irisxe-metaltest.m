// irisxe-metaltest - milestone 1 step 2a test tool (run on the laptop).
//
//   1. asks Metal which GPUs it knows (MTLCopyAllDevices)
//   2. finds our accelerator (IrisXeAccelerator) in the I/O registry
//   3. loads IrisXeMTLDriver.bundle by hand and creates our MTLDevice on the
//      accelerator, like Metal itself will do later. This runs in a child
//      process, so a crash only ends the child and is reported.
//
// usage: irisxe-metaltest [path/to/IrisXeMTLDriver.bundle]
//        irisxe-metaltest --probe   (try featureProfile values, step 2b)
// (the child is this same program started again with --child; fork() is not
// safe once Metal/Foundation are running)
extern char **environ;
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOKit/IOKitLib.h>
#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#include <IOSurface/IOSurfaceRef.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <sys/wait.h>
#include <spawn.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <unistd.h>
#include <math.h>
#include "xe_kernels.h"
#include <dlfcn.h>
#include "../../src/compiler/xe_compile.h"   // milestone 5b: XeShaderInfo, irisxe_compile_air_render
#include "../../src/IrisXeDrawTest.h"        // milestone 5b: draw C

static io_service_t findAccel(void) {
    return IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IrisXeAccelerator"));
}

static void listDevices(const char *title) {
    NSArray<id<MTLDevice>> *all = MTLCopyAllDevices();
    printf("%s: Metal knows %lu GPU(s)\n", title, (unsigned long)all.count);
    for (id<MTLDevice> d in all)
        printf("    - %s  (class %s, registryID 0x%llx)\n", d.name.UTF8String,
               class_getName([(id)d class]), (unsigned long long)d.registryID);
}

static int child(const char *bundlePath) {
    @autoreleasepool {
        io_service_t svc = findAccel();
        if (!svc) { printf("    accelerator not found\n"); return 2; }
        uint64_t rid = 0; IORegistryEntryGetRegistryEntryID(svc, &rid);
        printf("    accelerator service port %u, registryID 0x%llx\n", svc, (unsigned long long)rid);

        NSBundle *b = [NSBundle bundleWithPath:@(bundlePath)];
        NSError *err = nil;
        if (!b || ![b loadAndReturnError:&err]) {
            printf("    bundle load FAILED: %s\n", err ? err.description.UTF8String : "no bundle at that path");
            return 3;
        }
        printf("    bundle loaded: %s %s\n", b.bundleIdentifier.UTF8String,
               [[b objectForInfoDictionaryKey:@"CFBundleVersion"] UTF8String]);
        Class c = objc_getClass("IrisXeMTLDevice");
        if (!c) { printf("    class IrisXeMTLDevice NOT registered\n"); return 4; }
        printf("    class IrisXeMTLDevice : %s, instance size %zu, conforms to MTLDevice: %s\n",
               class_getName(class_getSuperclass(c)), class_getInstanceSize(c),
               class_conformsToProtocol(c, @protocol(MTLDevice)) ? "yes" : "NO");

        id dev = nil;
        @try {
            dev = ((id (*)(id, SEL, uint32_t))objc_msgSend)([c alloc], sel_registerName("initWithAcceleratorPort:"), svc);
        } @catch (NSException *e) {
            printf("    initWithAcceleratorPort: EXCEPTION %s: %s\n", e.name.UTF8String, e.reason.UTF8String);
            return 5;
        }
        if (!dev) { printf("    initWithAcceleratorPort: returned nil (Apple's base class refused)\n"); return 6; }
        id<MTLDevice> d = (id<MTLDevice>)dev;
        printf("    DEVICE CREATED: %s\n", d.name.UTF8String);
        @try {
            printf("    registryID 0x%llx, unified memory %d, low power %d, headless %d\n",
                   (unsigned long long)d.registryID, d.hasUnifiedMemory, d.isLowPower, d.isHeadless);
            printf("    recommendedMaxWorkingSetSize %llu MB\n",
                   (unsigned long long)(d.recommendedMaxWorkingSetSize >> 20));
        } @catch (NSException *e) {
            printf("    property query EXCEPTION %s: %s\n", e.name.UTF8String, e.reason.UTF8String);
        }
        return 0;
    }
}

// --list (child of --probe): one line about our GPU as Metal itself sees it
static int listOne(void) {
    @autoreleasepool {
        const char *fp = getenv("IRISXE_FP");
        NSArray<id<MTLDevice>> *all = MTLCopyAllDevices();
        for (id<MTLDevice> d in all) {
            if (![d.name containsString:@"IrisXe"]) continue;
            printf("    fp=%-3s OK  Mac2=%d Common3=%d Metal3=%d Apple7=%d  maxThreads=%lux%lux%lu  argBuffers=%ld\n",
                   fp, [d supportsFamily:MTLGPUFamilyMac2], [d supportsFamily:MTLGPUFamilyCommon3],
                   [d supportsFamily:MTLGPUFamilyMetal3], [d supportsFamily:MTLGPUFamilyApple7],
                   (unsigned long)d.maxThreadsPerThreadgroup.width, (unsigned long)d.maxThreadsPerThreadgroup.height,
                   (unsigned long)d.maxThreadsPerThreadgroup.depth, (long)d.argumentBuffersSupport);
            return 0;
        }
        printf("    fp=%-3s no IrisXe device (Metal knows %lu)\n", fp, (unsigned long)all.count);
        return 1;
    }
}

// --buffers (child): milestone 2a test on the GPU as Metal itself lists it
static int bufferTest(void) {
    @autoreleasepool {
        id<MTLDevice> d = nil;
        for (id<MTLDevice> x in MTLCopyAllDevices()) if ([x.name containsString:@"IrisXe"]) d = x;
        if (!d) { printf("    no IrisXe device\n"); return 2; }
        int fails = 0;
        NSUInteger sizes[] = { 16, 4096, 65536, 4 << 20 };
        for (int i = 0; i < 4; i++) {
            id<MTLBuffer> b = [d newBufferWithLength:sizes[i] options:MTLResourceStorageModeShared];
            if (!b) { printf("    newBufferWithLength %lu: nil\n", (unsigned long)sizes[i]); fails++; continue; }
            uint32_t *p = (uint32_t *)b.contents;
            NSUInteger n = b.length / 4; int bad = 0;
            if (!p) { printf("    buffer %lu: contents NULL\n", (unsigned long)sizes[i]); fails++; [b release]; continue; }
            for (NSUInteger k = 0; k < n; k++) p[k] = 0xC0DE0000u + (uint32_t)k;
            for (NSUInteger k = 0; k < n; k++) if (p[k] != 0xC0DE0000u + (uint32_t)k) bad++;
            printf("    buffer %8lu bytes: OK  length %lu  gpuAddress 0x%llx  class %s  %s\n", (unsigned long)sizes[i],
                   (unsigned long)b.length, (unsigned long long)b.gpuAddress, class_getName([(id)b class]),
                   bad ? "DATA WRONG" : "write/read ok");
            if (bad) fails++;
            [b release];
        }
        float src[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
        id<MTLBuffer> c = [d newBufferWithBytes:src length:sizeof(src) options:MTLResourceStorageModeShared];
        if (c && !memcmp(c.contents, src, sizeof(src))) printf("    newBufferWithBytes: OK (data copied)\n");
        else { printf("    newBufferWithBytes: FAILED\n"); fails++; }
        [c release];
        printf("    buffer test: %s\n", fails ? "FAILED" : "ALL OK");
        return fails ? 1 : 0;
    }
}

// --queue (child): milestone 2b - command queue, empty command buffers
static const char *cbStatus(MTLCommandBufferStatus s) {
    switch (s) {
        case MTLCommandBufferStatusNotEnqueued: return "NotEnqueued";
        case MTLCommandBufferStatusEnqueued: return "Enqueued";
        case MTLCommandBufferStatusCommitted: return "Committed";
        case MTLCommandBufferStatusScheduled: return "Scheduled";
        case MTLCommandBufferStatusCompleted: return "Completed";
        case MTLCommandBufferStatusError: return "Error";
    }
    return "?";
}

static int queueTest(void) {
    @autoreleasepool {
        id<MTLDevice> d = nil;
        for (id<MTLDevice> x in MTLCopyAllDevices()) if ([x.name containsString:@"IrisXe"]) d = x;
        if (!d) { printf("    no IrisXe device\n"); return 2; }
        id<MTLCommandQueue> q = [d newCommandQueue];
        printf("    newCommandQueue: %s\n", q ? class_getName([(id)q class]) : "nil");
        if (!q) return 3;
        int fails = 0;
        for (int i = 0; i < 3; i++) {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            if (!cb) { printf("    commandBuffer %d: nil\n", i); fails++; continue; }
            dispatch_semaphore_t sem = dispatch_semaphore_create(0);
            [cb addCompletedHandler:^(id<MTLCommandBuffer> b) { dispatch_semaphore_signal(sem); }];
            [cb commit];
            long late = dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
            printf("    empty command buffer %d (%s): %s, status %s%s%s\n", i, class_getName([(id)cb class]),
                   late ? "NOT finished after 5 s" : "completed", cbStatus(cb.status),
                   cb.error ? ", error: " : "", cb.error ? cb.error.localizedDescription.UTF8String : "");
            fflush(stdout);
            if (late || cb.status != MTLCommandBufferStatusCompleted) fails++;
        }
        printf("    queue test: %s\n", fails ? "FAILED" : "ALL OK");
        return fails ? 1 : 0;
    }
}

// --fill (child): milestone 2c - the GPU fills a Metal buffer (blit encoder)
static int fillTest(void) {
    @autoreleasepool {
        id<MTLDevice> d = nil;
        for (id<MTLDevice> x in MTLCopyAllDevices()) if ([x.name containsString:@"IrisXe"]) d = x;
        if (!d) { printf("    no IrisXe device\n"); return 2; }
        id<MTLCommandQueue> q = [d newCommandQueue];
        const NSUInteger len = 65536;
        id<MTLBuffer> b = [d newBufferWithLength:len options:MTLResourceStorageModeShared];
        if (!q || !b) { printf("    queue %p buffer %p\n", q, b); return 3; }
        uint8_t *p = (uint8_t *)b.contents;
        printf("    buffer 64 KB at GPU address 0x%llx\n", (unsigned long long)b.gpuAddress);
        struct { NSUInteger off, n; uint8_t v; } jobs[2] = { { 0, len, 0xAB }, { 4096, 8192, 0x5C } };
        int fails = 0;
        for (int j = 0; j < 2; j++) {
            if (j == 0) memset(p, 0x11, len);
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
            if (!e) { printf("    blitCommandEncoder: nil\n"); return 4; }
            if (j == 0) printf("    blit encoder: %s\n", class_getName([(id)e class]));
            [e fillBuffer:b range:NSMakeRange(jobs[j].off, jobs[j].n) value:jobs[j].v];
            [e endEncoding];
            dispatch_semaphore_t sem = dispatch_semaphore_create(0);
            [cb addCompletedHandler:^(id<MTLCommandBuffer> x) { dispatch_semaphore_signal(sem); }];
            [cb commit];
            long late = dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
            NSUInteger inside = 0, before = 0, after = 0;
            for (NSUInteger k = 0; k < len; k++) {
                bool in = k >= jobs[j].off && k < jobs[j].off + jobs[j].n;
                uint8_t want = in ? jobs[j].v : (j == 0 ? 0x11 : 0xAB);
                if (p[k] == want) { if (in) inside++; else if (k < jobs[j].off) before++; else after++; }
            }
            NSUInteger outside = len - jobs[j].n;
            bool ok = !late && inside == jobs[j].n && before + after == outside;
            if (cb.error) printf("    command buffer error: domain %s code %ld: %s\n", cb.error.domain.UTF8String,
                                 (long)cb.error.code, cb.error.localizedDescription.UTF8String);
            printf("    fill %lu bytes at +%lu with 0x%02X: %s, status %s | right inside %lu/%lu, untouched outside %lu/%lu | first bytes %02X %02X %02X %02X -> %s\n",
                   (unsigned long)jobs[j].n, (unsigned long)jobs[j].off, jobs[j].v, late ? "NOT finished after 5 s" : "completed",
                   cbStatus(cb.status), (unsigned long)inside, (unsigned long)jobs[j].n, (unsigned long)(before + after),
                   (unsigned long)outside, p[jobs[j].off], p[jobs[j].off + 1], p[jobs[j].off + 2], p[jobs[j].off + 3],
                   ok ? "GPU DID IT" : "WRONG");
            fflush(stdout);
            if (!ok) fails++;
        }
        printf("    GPU fill test: %s\n", fails ? "FAILED" : "ALL OK");
        return fails ? 1 : 0;
    }
}

// commit a command buffer and wait for it (5 s); returns 0 when completed
static int commitWait(id<MTLCommandBuffer> cb) {
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    [cb addCompletedHandler:^(id<MTLCommandBuffer> x) { dispatch_semaphore_signal(sem); }];
    [cb commit];
    long late = dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
    if (cb.error) printf("    command buffer error: domain %s code %ld: %s\n", cb.error.domain.UTF8String,
                         (long)cb.error.code, cb.error.localizedDescription.UTF8String);
    if (late) { printf("    NOT finished after 5 s\n"); return 1; }
    return cb.status == MTLCommandBufferStatusCompleted ? 0 : 2;
}
static id<MTLDevice> ourDevice(void) {
    for (id<MTLDevice> x in MTLCopyAllDevices()) if ([x.name containsString:@"IrisXe"]) return x;
    return nil;
}

// --copy (child): milestone 2d - the GPU copies one Metal buffer to another
static int copyTest(void) {
    @autoreleasepool {
        id<MTLDevice> d = ourDevice();
        if (!d) { printf("    no IrisXe device\n"); return 2; }
        id<MTLCommandQueue> q = [d newCommandQueue];
        const NSUInteger len = 1 << 20;
        id<MTLBuffer> a = [d newBufferWithLength:len options:MTLResourceStorageModeShared];
        id<MTLBuffer> b = [d newBufferWithLength:len options:MTLResourceStorageModeShared];
        if (!q || !a || !b) { printf("    queue %p buffers %p %p\n", q, a, b); return 3; }
        printf("    source 1 MB at GPU 0x%llx, destination 1 MB at GPU 0x%llx\n",
               (unsigned long long)a.gpuAddress, (unsigned long long)b.gpuAddress);
        uint8_t *pa = (uint8_t *)a.contents, *pb = (uint8_t *)b.contents;
        for (NSUInteger k = 0; k < len; k++) pa[k] = (uint8_t)(k * 7 + (k >> 12));
        memset(pb, 0, len);
        struct { NSUInteger so, dof, n; } jobs[2] = { { 0, 0, len }, { 8196, 100, 12000 } };
        int fails = 0;
        for (int j = 0; j < 2; j++) {
            if (j == 1) memset(pb, 0x33, len);
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
            [e copyFromBuffer:a sourceOffset:jobs[j].so toBuffer:b destinationOffset:jobs[j].dof size:jobs[j].n];
            [e endEncoding];
            int r = commitWait(cb);
            NSUInteger right = 0, untouched = 0;
            for (NSUInteger k = 0; k < len; k++) {
                bool in = k >= jobs[j].dof && k < jobs[j].dof + jobs[j].n;
                if (in) right += pb[k] == pa[k - jobs[j].dof + jobs[j].so];
                else untouched += pb[k] == 0x33;
            }
            NSUInteger outside = len - jobs[j].n;
            bool ok = !r && right == jobs[j].n && untouched == outside;
            printf("    copy %lu bytes (+%lu -> +%lu): status %s | copied right %lu/%lu, untouched outside %lu/%lu -> %s\n",
                   (unsigned long)jobs[j].n, (unsigned long)jobs[j].so, (unsigned long)jobs[j].dof, cbStatus(cb.status),
                   (unsigned long)right, (unsigned long)jobs[j].n, (unsigned long)untouched, (unsigned long)outside,
                   ok ? "GPU DID IT" : "WRONG");
            fflush(stdout);
            if (!ok) fails++;
        }
        printf("    GPU copy test: %s\n", fails ? "FAILED" : "ALL OK");
        return fails ? 1 : 0;
    }
}

// --stress (child): many buffers made, filled by the GPU, checked and freed.
// Freed buffers give their GPU addresses back, so this checks the kernel
// unmaps old pages and maps the new ones each time.
static int stressTest(void) {
    @autoreleasepool {
        id<MTLDevice> d = ourDevice();
        if (!d) { printf("    no IrisXe device\n"); return 2; }
        id<MTLCommandQueue> q = [d newCommandQueue];
        int fails = 0, n = 200;
        NSUInteger total = 0;
        for (int i = 0; i < n; i++) {
            @autoreleasepool {
                NSUInteger len = 4096u << (i % 7);                       // 4 KB .. 256 KB
                id<MTLBuffer> b = [d newBufferWithLength:len options:MTLResourceStorageModeShared];
                if (!b) { printf("    buffer %d: nil\n", i); fails++; break; }
                uint8_t v = (uint8_t)(i * 37 + 1), *p = (uint8_t *)b.contents;
                memset(p, (uint8_t)~v, len);
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
                [e fillBuffer:b range:NSMakeRange(0, len) value:v];
                [e endEncoding];
                int r = commitWait(cb);
                NSUInteger good = 0;
                for (NSUInteger k = 0; k < len; k++) good += p[k] == v;
                if (r || good != len) {
                    if (fails < 5) printf("    buffer %d (%lu KB at GPU 0x%llx): %s, %lu/%lu bytes right\n", i,
                                          (unsigned long)(len / 1024), (unsigned long long)b.gpuAddress,
                                          r ? "command buffer failed" : "WRONG", (unsigned long)good, (unsigned long)len);
                    fails++;
                }
                total += len;
                [b release];
            }
        }
        printf("    %d buffers (%lu KB in all) made, GPU-filled, checked and freed: %d wrong -> %s\n",
               n, (unsigned long)(total / 1024), fails, fails ? "FAILED" : "ALL OK");
        return fails ? 1 : 0;
    }
}

// --isolate V (child): milestone 2e - one of two programs using the GPU at
// the same time. Both get a buffer at the same GPU address; each fills its own
// with its own value 300 times. With one shared GPU address space the other
// program's fills would land in our buffer.
static int isolateTest(const char *arg) {
    @autoreleasepool {
        uint8_t v = (uint8_t)strtoul(arg, NULL, 0), other = 0;
        id<MTLDevice> d = ourDevice();
        if (!d) { printf("    [%02X] no IrisXe device\n", v); return 2; }
        id<MTLCommandQueue> q = [d newCommandQueue];
        const NSUInteger len = 65536;
        id<MTLBuffer> b = [d newBufferWithLength:len options:MTLResourceStorageModeShared];
        if (!q || !b) return 3;
        uint8_t *p = (uint8_t *)b.contents;
        int wrong = 0, foreign = 0, rounds = 300;
        for (int i = 0; i < rounds; i++) {
            @autoreleasepool {
                uint8_t want = (uint8_t)(v + (i & 1));              // two values so every fill changes the buffer
                memset(p, 0, len);
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
                [e fillBuffer:b range:NSMakeRange(0, len) value:want];
                [e endEncoding];
                int r = commitWait(cb);
                NSUInteger good = 0;
                for (NSUInteger k = 0; k < len; k++) {
                    if (p[k] == want) good++;
                    else if (p[k]) { foreign++; other = p[k]; }
                }
                if (r || good != len) wrong++;
            }
        }
        printf("    program %02X: buffer at GPU 0x%llx, %d fills, %d wrong, %d foreign bytes%s -> %s\n", v,
               (unsigned long long)b.gpuAddress, rounds, wrong, foreign, foreign ? " (another program's value!)" : "",
               wrong ? "FAILED" : "OK");
        (void)other;
        fflush(stdout);
        return wrong ? 1 : 0;
    }
}

// run two --isolate children at the same time
static int runIsolatePair(const char *self) {
    const char *vals[2] = { "0xA0", "0xC0" };
    pid_t pid[2] = { 0, 0 };
    fflush(stdout);
    for (int i = 0; i < 2; i++) {
        char *cargv[] = { (char *)self, (char *)"--isolate", (char *)vals[i], NULL };
        if (posix_spawn(&pid[i], self, NULL, NULL, cargv, environ) != 0) { perror("posix_spawn"); return -1; }
    }
    int fails = 0;
    for (int i = 0; i < 2; i++) {
        int st = 0, t = 0;
        for (; t < 600; t++) { if (waitpid(pid[i], &st, WNOHANG) == pid[i]) break; usleep(100000); }
        if (t >= 600) { printf("    program %d STILL RUNNING after 60 s\n", i); fails++; continue; }
        if (WIFSIGNALED(st)) { printf("    program %d CRASHED (%s)\n", i, strsignal(WTERMSIG(st))); fails++; }
        else if (WEXITSTATUS(st)) fails++;
    }
    printf("    two programs at once: %s\n", fails ? "FAILED" : "ALL OK (each kept its own GPU memory)");
    return fails;
}

// --compute (child): milestone 3a - a compute kernel through Metal's compute
// encoder. No shader compiler yet: the kernel is ready Iris Xe code (add1:
// out[i] = in[i] + 1), handed over with our private device method.
// true once IrisXeCore reports the GPU as hung (then every later GPU test is
// meaningless until a reboot)
static bool gpuHung(void);
static bool stopIfHung(const char *after) {
    for (int t = 0; t < 15 && !gpuHung(); t++) usleep(100000);   // the status is refreshed by a heartbeat
    if (!gpuHung()) return false;
    printf("    GPU HUNG after %s - stopping here (reboot before testing again)\n", after);
    fflush(stdout);
    return true;
}

static int computeTest(void) {
    @autoreleasepool {
        id<MTLDevice> d = ourDevice();
        if (!d) { printf("    no IrisXe device\n"); return 2; }
        SEL mk = sel_registerName("irisxeNewComputePipelineWithKernel:");
        if (![(id)d respondsToSelector:mk]) { printf("    bundle has no compute support (old bundle?)\n"); return 3; }
        NSData *code = [NSData dataWithBytes:kXeAdd1 length:sizeof(kXeAdd1)];
        id<MTLComputePipelineState> ps = ((id (*)(id, SEL, id))objc_msgSend)((id)d, mk, code);
        id<MTLCommandQueue> q = [d newCommandQueue];
        if (!ps || !q) { printf("    pipeline %p queue %p\n", ps, q); return 4; }
        printf("    pipeline %s, %lu threads max, SIMD width %lu\n", class_getName([(id)ps class]),
               (unsigned long)ps.maxTotalThreadsPerThreadgroup, (unsigned long)ps.threadExecutionWidth);
        struct { NSUInteger n, tpg; BOOL groups; } jobs[4] = {
            { 64, 64, NO }, { 10000, 64, NO }, { 1000, 100, NO }, { 4096, 512, YES } };
        int fails = 0;
        for (int j = 0; j < 4; j++) {
            @autoreleasepool {
                NSUInteger n = jobs[j].n, extra = 64;
                id<MTLBuffer> in = [d newBufferWithLength:(n + extra) * 4 options:MTLResourceStorageModeShared];
                id<MTLBuffer> out = [d newBufferWithLength:(n + extra) * 4 options:MTLResourceStorageModeShared];
                uint32_t *pi = (uint32_t *)in.contents, *po = (uint32_t *)out.contents;
                for (NSUInteger k = 0; k < n + extra; k++) { pi[k] = (uint32_t)(k * 3 + 7); po[k] = 0xDEADBEEF; }
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
                if (!e) { printf("    computeCommandEncoder: nil\n"); return 5; }
                if (j == 0) printf("    compute encoder: %s\n", class_getName([(id)e class]));
                [e setComputePipelineState:ps];
                [e setBuffer:in offset:0 atIndex:0];
                [e setBuffer:out offset:0 atIndex:1];
                if (jobs[j].groups)
                    [e dispatchThreadgroups:MTLSizeMake(n / jobs[j].tpg, 1, 1) threadsPerThreadgroup:MTLSizeMake(jobs[j].tpg, 1, 1)];
                else
                    [e dispatchThreads:MTLSizeMake(n, 1, 1) threadsPerThreadgroup:MTLSizeMake(jobs[j].tpg, 1, 1)];
                [e endEncoding];
                int r = commitWait(cb);
                NSUInteger right = 0, untouched = 0, firstBad = NSNotFound;
                for (NSUInteger k = 0; k < n; k++) {
                    if (po[k] == pi[k] + 1) right++; else if (firstBad == NSNotFound) firstBad = k;
                }
                for (NSUInteger k = n; k < n + extra; k++) untouched += po[k] == 0xDEADBEEF;
                bool ok = !r && right == n && untouched == extra;
                printf("    %s %lu threads, groups of %lu: status %s | right %lu/%lu, untouched after %lu/%lu",
                       jobs[j].groups ? "dispatchThreadgroups" : "dispatchThreads", (unsigned long)n,
                       (unsigned long)jobs[j].tpg, cbStatus(cb.status), (unsigned long)right, (unsigned long)n,
                       (unsigned long)untouched, (unsigned long)extra);
                if (firstBad != NSNotFound)
                    printf(" | first wrong [%lu] = %08X (want %08X)", (unsigned long)firstBad, po[firstBad], pi[firstBad] + 1);
                printf(" -> %s\n", ok ? "GPU RAN THE KERNEL" : "WRONG");
                fflush(stdout);
                if (!ok) fails++;
                [in release]; [out release];
            }
        }
        printf("    compute test: %s\n", fails ? "FAILED" : "ALL OK");
        return fails ? 1 : 0;
    }
}

// --shaders (child): milestone 3c - real Metal shaders, compiled by our
// driver's own compiler (AIR -> Iris Xe code) through the normal Metal API.
static id<MTLLibrary> testLibrary(id<MTLDevice> d) {
    char self[4096]; uint32_t sz = sizeof(self);
    if (_NSGetExecutablePath(self, &sz) != 0) return nil;
    NSString *dir = [[NSString stringWithUTF8String:self] stringByDeletingLastPathComponent];
    NSURL *url = [NSURL fileURLWithPath:[dir stringByAppendingPathComponent:@"irisxe-kernels.metallib"]];
    NSError *e = nil;
    id<MTLLibrary> lib = [d newLibraryWithURL:url error:&e];
    if (!lib) printf("    irisxe-kernels.metallib (next to this tool): %s\n", e.localizedDescription.UTF8String);
    return lib;
}
static int shaderTest(void) {
    @autoreleasepool {
        id<MTLDevice> d = ourDevice();
        if (!d) { printf("    no IrisXe device\n"); return 2; }
        id<MTLLibrary> lib = testLibrary(d);
        if (!lib) return 3;
        id<MTLCommandQueue> q = [d newCommandQueue];
        const NSUInteger n = 5000, extra = 64;
        int fails = 0;
        const char *names[] = { "add1", "saxpy", "clamp_branch", "int_ops", "to_float" };
        for (int t = 0; t < 5; t++) {
            @autoreleasepool {
                NSString *name = @(names[t]);
                id<MTLFunction> fn = [lib newFunctionWithName:name];
                NSError *e = nil;
                id<MTLComputePipelineState> ps = fn ? [d newComputePipelineStateWithFunction:fn error:&e] : nil;
                if (!ps) {
                    BOOL expected = !strcmp(names[t], "loop_sum") && e &&
                                    [e.localizedDescription containsString:@"not supported yet"];
                    printf("    %-13s compile: %s -> %s\n", names[t], e ? e.localizedDescription.UTF8String : "no function",
                           expected ? "expected for now (loops come next)" : "FAILED");
                    if (!expected) fails++;
                    continue;
                }
                NSString *log = [(id)d respondsToSelector:sel_registerName("irisxeLastCompileLog")]
                                ? ((id (*)(id, SEL))objc_msgSend)((id)d, sel_registerName("irisxeLastCompileLog")) : @"";
                id<MTLBuffer> a = [d newBufferWithLength:(n + extra) * 4 options:MTLResourceStorageModeShared];
                id<MTLBuffer> b = [d newBufferWithLength:(n + extra) * 4 options:MTLResourceStorageModeShared];
                id<MTLBuffer> k = [d newBufferWithLength:4096 options:MTLResourceStorageModeShared];
                uint32_t *pa = (uint32_t *)a.contents, *pb = (uint32_t *)b.contents;
                float *fa = (float *)a.contents, *fb = (float *)b.contents;
                int32_t *ia = (int32_t *)a.contents;
                for (NSUInteger i = 0; i < n + extra; i++) {
                    pa[i] = (uint32_t)(i * 2654435761u >> 7);
                    pb[i] = 0xDEADBEEF;
                    if (t == 1) { fa[i] = (float)i * 0.25f; fb[i] = 3.0f - (float)i; }
                    if (t == 2 || t == 4) ia[i] = (int32_t)(i % 401) - 200;
                }
                *(float *)k.contents = 1.5f;
                uint32_t *ref = malloc((n + extra) * 4);
                memcpy(ref, pb, (n + extra) * 4);
                for (NSUInteger i = 0; i < n; i++) {
                    switch (t) {
                    case 0: ref[i] = pa[i] + 1; break;
                    case 1: { float r = 1.5f * fa[i] + fb[i]; memcpy(&ref[i], &r, 4); break; }
                    case 2: break;
                    case 3: ref[i] = ((pa[i] * 7u + (pa[i] >> 3)) ^ 0x55u) * pa[i]; break;
                    case 4: { float r = (float)ia[i] * 0.5f + 1.0f; memcpy(&ref[i], &r, 4); break; }
                    }
                }
                uint32_t *check = ref;
                uint32_t *got = pb;
                if (t == 2) {                                  // works in place on buffer 0
                    memcpy(ref, pa, (n + extra) * 4);
                    for (NSUInteger i = 0; i < n; i++) { int32_t x = ia[i]; ((int32_t *)ref)[i] = x < 0 ? -x : x * 2; }
                    got = pa;
                }
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
                [enc setComputePipelineState:ps];
                [enc setBuffer:a offset:0 atIndex:0];
                if (t != 2) [enc setBuffer:b offset:0 atIndex:1];
                if (t == 1) [enc setBuffer:k offset:0 atIndex:2];
                [enc dispatchThreads:MTLSizeMake(n, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
                [enc endEncoding];
                int r = commitWait(cb);
                NSUInteger right = 0, untouched = 0, bad = NSNotFound;
                for (NSUInteger i = 0; i < n; i++) { if (got[i] == check[i]) right++; else if (bad == NSNotFound) bad = i; }
                for (NSUInteger i = n; i < n + extra; i++) untouched += got[i] == check[i];
                bool ok = !r && right == n && untouched == extra;
                printf("    %-13s [%s] status %s | right %lu/%lu, untouched after %lu/%lu", names[t], log.UTF8String,
                       cbStatus(cb.status), (unsigned long)right, (unsigned long)n, (unsigned long)untouched, (unsigned long)extra);
                if (bad != NSNotFound) printf(" | first wrong [%lu] = %08X (want %08X)", (unsigned long)bad, got[bad], check[bad]);
                printf(" -> %s\n", ok ? "OUR COMPILER WORKS" : "WRONG");
                fflush(stdout);
                if (!ok) fails++;
                free(ref);
                [a release]; [b release]; [k release];
            }
        }
        printf("    shader test: %s\n", fails ? "FAILED" : "ALL OK");
        return fails ? 1 : 0;
    }
}

// --branches (child): milestone 3d - Metal shaders with if/else and loops,
// checked against the same code run on the CPU
static uint32_t refCollatz(uint32_t x) { uint32_t n = 0; while (x > 1) { x = (x & 1) ? 3 * x + 1 : x / 2; n++; } return n; }
static int branchTest(void) {
    @autoreleasepool {
        id<MTLDevice> d = ourDevice();
        if (!d) { printf("    no IrisXe device\n"); return 2; }
        id<MTLLibrary> lib = testLibrary(d);
        if (!lib) return 3;
        id<MTLCommandQueue> q = [d newCommandQueue];
        const NSUInteger n = 4096;              // threads; buffers hold 4096 words
        const uint32_t loops = 9;               // loop count (buffer 2)
        int fails = 0;
        const char *names[] = { "loop_sum", "collatz", "if_chain", "nested_loops", "loop_if" };
        for (int t = 0; t < 5; t++) {
            @autoreleasepool {
                id<MTLFunction> fn = [lib newFunctionWithName:@(names[t])];
                NSError *e = nil;
                id<MTLComputePipelineState> ps = fn ? [d newComputePipelineStateWithFunction:fn error:&e] : nil;
                if (!ps) {
                    printf("    %-13s compile: %s -> FAILED\n", names[t], e ? e.localizedDescription.UTF8String : "no function");
                    fails++;
                    continue;
                }
                NSString *log = ((id (*)(id, SEL))objc_msgSend)((id)d, sel_registerName("irisxeLastCompileLog"));
                NSUInteger words = 4096 * (t == 0 ? loops : 1) + 64;
                id<MTLBuffer> a = [d newBufferWithLength:words * 4 options:MTLResourceStorageModeShared];
                id<MTLBuffer> b = [d newBufferWithLength:(n + 64) * 4 options:MTLResourceStorageModeShared];
                id<MTLBuffer> k = [d newBufferWithLength:4096 options:MTLResourceStorageModeShared];
                *(uint32_t *)k.contents = loops;
                uint32_t *pa = (uint32_t *)a.contents, *pb = (uint32_t *)b.contents;
                int32_t *ia = (int32_t *)a.contents;
                for (NSUInteger x = 0; x < words; x++) pa[x] = (uint32_t)(x * 2654435761u >> 9) & 0xFFFF;
                if (t == 1) for (NSUInteger x = 0; x < words; x++) pa[x] = (uint32_t)(x % 977) + 1;
                if (t == 2 || t == 4) for (NSUInteger x = 0; x < words; x++) ia[x] = (int32_t)((x * 7919) % 601) - 300;
                for (NSUInteger x = 0; x < n + 64; x++) pb[x] = 0xDEADBEEF;
                // CPU reference
                uint32_t *ra = malloc(words * 4), *rb = malloc((n + 64) * 4);
                memcpy(ra, pa, words * 4); memcpy(rb, pb, (n + 64) * 4);
                int32_t *ira = (int32_t *)ra, *irb = (int32_t *)rb;
                for (NSUInteger i = 0; i < n; i++) {
                    switch (t) {
                    case 0: { uint32_t s = 0; for (uint32_t z = 0; z < loops; z++) s += pa[i * loops + z]; rb[i] = s; break; }
                    case 1: ra[i] = refCollatz(pa[i]); break;
                    case 3: {
                        uint32_t s = 0;
                        for (uint32_t x = 0; x < loops; x++) {
                            for (uint32_t y = 0; y <= x; y++) s += pa[(i + x * 7 + y) & 1023] ^ y;
                        }
                        rb[i] = s;
                        break;
                    }
                    case 4: { int32_t acc = 0, hits = 0;
                              for (uint32_t z = 0; z < loops; z++) { int32_t x = ia[(i + z * 13) & 1023];
                                  if (x > 0) { acc += x; hits++; } else if (x < -150) acc -= 1; }
                              irb[i] = acc * 16 + hits; break; }
                    }
                }
                if (t == 2)     // if_chain: in place on buffer 0, else-side also writes buffer 1
                    for (NSUInteger i = 0; i < n; i++) {
                        int32_t x = ia[i], r;
                        if (x > 10) { r = x * 3; if (x > 100) r += 1000; else r -= x << 2; }
                        else { r = -x; irb[i] = r + 7; }
                        ira[i] = r;
                    }
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
                [enc setComputePipelineState:ps];
                [enc setBuffer:a offset:0 atIndex:0];
                [enc setBuffer:b offset:0 atIndex:1];
                [enc setBuffer:k offset:0 atIndex:2];
                [enc dispatchThreads:MTLSizeMake(n, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
                [enc endEncoding];
                int r = commitWait(cb);
                NSUInteger right = 0, checked = 0, bad = NSNotFound;
                uint32_t *gotv = NULL, *wantv = NULL;
                for (int pass = 0; pass < 2; pass++) {          // pass 0: buffer 0, pass 1: buffer 1
                    uint32_t *got = pass ? pb : pa, *want = pass ? rb : ra;
                    NSUInteger cnt = pass ? n + 64 : words;
                    for (NSUInteger i = 0; i < cnt; i++) {
                        checked++;
                        if (got[i] == want[i]) right++;
                        else if (bad == NSNotFound) { bad = i + (pass ? 1000000 : 0); gotv = &got[i]; wantv = &want[i]; }
                    }
                }
                bool ok = !r && right == checked;
                printf("    %-13s [%s] status %s | right %lu/%lu", names[t], log.UTF8String, cbStatus(cb.status),
                       (unsigned long)right, (unsigned long)checked);
                if (bad != NSNotFound)
                    printf(" | first wrong: buffer %d word %lu = %08X (want %08X)", bad >= 1000000 ? 1 : 0,
                           (unsigned long)(bad % 1000000), *gotv, *wantv);
                printf(" -> %s\n", ok ? "OUR COMPILER WORKS" : "WRONG");
                fflush(stdout);
                if (!ok) fails++;
                free(ra); free(rb);
                [a release]; [b release]; [k release];
                if (r && stopIfHung(names[t])) { fails++; break; }
            }
        }
        printf("    branch/loop test: %s\n", fails ? "FAILED" : "ALL OK");
        return fails ? 1 : 0;
    }
}

// --types (child): milestone 3e - maths, division, vectors, 8/16-bit, half, bits, 2D positions
static uint16_t f2h(float f) {                       // float -> half (round to nearest even)
    __fp16 h = (__fp16)f; uint16_t r; memcpy(&r, &h, 2); return r;
}
static float h2f(uint16_t v) { __fp16 h; memcpy(&h, &v, 2); return (float)h; }
static bool near(float got, float want, float rel, float abs_) {
    if (isnan(want)) return isnan(got);
    return fabsf(got - want) <= fmaxf(abs_, rel * fabsf(want));
}
// float -> uint the way the GPU does it: negative and NaN give 0, too big gives
// 0xFFFFFFFF (in C a negative float -> unsigned is undefined, so (uint32_t)x
// is not a valid reference)
static uint32_t f2uSat(float x) {
    if (!(x > 0.0f)) return 0;
    if (x >= 4294967296.0f) return 0xFFFFFFFFu;
    return (uint32_t)x;
}
static int typesTest(void) {
    @autoreleasepool {
        id<MTLDevice> d = ourDevice();
        if (!d) { printf("    no IrisXe device\n"); return 2; }
        id<MTLLibrary> lib = testLibrary(d);
        if (!lib) return 3;
        id<MTLCommandQueue> q = [d newCommandQueue];
        const NSUInteger n = 1024;
        int fails = 0;
        const char *names[] = { "math_f", "div_mod", "vec_ops", "small_types", "half_math", "bit_ops", "pos_2d" };
        for (int t = 0; t < 7; t++) {
            @autoreleasepool {
                id<MTLFunction> fn = [lib newFunctionWithName:@(names[t])];
                NSError *e = nil;
                id<MTLComputePipelineState> ps = fn ? [d newComputePipelineStateWithFunction:fn error:&e] : nil;
                if (!ps) {
                    printf("    %-12s compile: %s -> FAILED\n", names[t], e ? e.localizedDescription.UTF8String : "no function");
                    fails++;
                    continue;
                }
                NSString *log = ((id (*)(id, SEL))objc_msgSend)((id)d, sel_registerName("irisxeLastCompileLog"));
                id<MTLBuffer> b[4];
                for (int k = 0; k < 4; k++) {
                    b[k] = [d newBufferWithLength:n * 16 + 256 options:MTLResourceStorageModeShared];
                    memset(b[k].contents, 0xEE, n * 16 + 256);
                }
                float *f0 = (float *)b[0].contents;
                int32_t *i0 = (int32_t *)b[0].contents;
                uint32_t *u0 = (uint32_t *)b[0].contents;
                uint8_t *c0 = (uint8_t *)b[0].contents;
                uint16_t *s1 = (uint16_t *)b[1].contents, *h0 = (uint16_t *)b[0].contents;
                for (NSUInteger i = 0; i < n * 4; i++) {
                    switch (t) {
                    case 0: f0[i] = 0.25f + (float)(i % 97) * 0.37f; break;
                    case 1: i0[i] = (int32_t)((i * 7919) % 20001) - 10000; break;
                    case 2: f0[i] = (float)((i * 37) % 101) * 0.25f - 5.0f; break;
                    case 3: c0[i] = (uint8_t)(i * 53 + 7); s1[i] = (uint16_t)(i * 2654435761u >> 11); break;
                    case 4: h0[i] = f2h((float)((i * 13) % 50) * 0.125f - 2.0f); break;
                    case 5: u0[i] = (uint32_t)(i * 2654435761u) >> (i % 7); if (i % 31 == 0) u0[i] = 0; break;
                    }
                }
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
                [enc setComputePipelineState:ps];
                for (int k = 0; k < 4; k++) [enc setBuffer:b[k] offset:0 atIndex:k];
                if (t == 6) [enc dispatchThreadgroups:MTLSizeMake(1, 4, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
                else [enc dispatchThreads:MTLSizeMake(n, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
                [enc endEncoding];
                int r = commitWait(cb);
                NSUInteger right = 0, checked = 0; char bad[160] = "";
                #define CHECK(cond, ...) do { checked++; if (cond) right++; else if (!bad[0]) snprintf(bad, sizeof(bad), __VA_ARGS__); } while (0)
                float *fo = (float *)b[1].contents;
                int32_t *io = (int32_t *)b[1].contents;
                uint32_t *uo = (uint32_t *)b[1].contents, *u2 = (uint32_t *)b[2].contents;
                uint8_t *bo = (uint8_t *)b[2].contents;
                uint16_t *so = (uint16_t *)b[3].contents, *ho = (uint16_t *)b[1].contents;
                for (NSUInteger i = 0; i < n; i++) {
                    switch (t) {
                    case 0: {
                        float x = f0[i];
                        float w[4] = { sqrtf(x) + 1.0f / sqrtf(x + 1.0f) + x / (x + 3.0f), sinf(x) * cosf(x * 0.5f),
                                       exp2f(x * 0.1f) + log2f(x + 1.0f) + expf(x * 0.05f) + logf(x + 2.0f),
                                       powf(x, 1.5f) + floorf(x * 1.7f) + ceilf(x * 0.3f) + (x * 3.3f - floorf(x * 3.3f)) + truncf(-x * 0.7f) };
                        for (int k = 0; k < 4; k++)
                            CHECK(near(fo[i * 4 + k], w[k], 2e-4f, 2e-4f), "[%lu.%d] = %g (want %g)", (unsigned long)i, k, fo[i * 4 + k], w[k]);
                        break;
                    }
                    case 1: {
                        int32_t x = i0[i], y = (int32_t)(i % 13) + 1; uint32_t ux = (uint32_t)x;
                        int32_t w[4] = { x / y, x % y, (int32_t)(ux / 7u) + (int32_t)(ux % 16u), x / 5 + x % 3 };
                        for (int k = 0; k < 4; k++)
                            CHECK(io[i * 4 + k] == w[k], "[%lu.%d] = %d (want %d)", (unsigned long)i, k, io[i * 4 + k], w[k]);
                        break;
                    }
                    case 2: {
                        float *v = &f0[i * 4];
                        float w[4] = { v[3] * 1 + v[0], v[2] * 2 + v[1], v[1] * 3 + v[2], v[0] * 4 + v[3] };
                        float dd = v[0] * w[0] + v[1] * w[1] + v[2] * w[2] + v[3] * w[3];
                        float want[4] = { w[0], w[1], dd, sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) };
                        for (int k = 0; k < 4; k++)
                            CHECK(near(fo[i * 4 + k], want[k], 1e-4f, 1e-4f), "[%lu.%d] = %g (want %g)", (unsigned long)i, k, fo[i * 4 + k], want[k]);
                        uint32_t wi[2] = { f2uSat(v[0]) + (uint32_t)i, f2uSat(w[3]) + 1u };
                        for (int k = 0; k < 2; k++)
                            CHECK(u2[i * 2 + k] == wi[k], "uint2 [%lu.%d] = %u (want %u)", (unsigned long)i, k, u2[i * 2 + k], wi[k]);
                        break;
                    }
                    case 3: {
                        uint8_t x = c0[i]; int8_t sx = (int8_t)x;
                        uint8_t wb = sx < 0 ? (uint8_t)(x * 3u + 1u) : (uint8_t)(x + 77u);
                        uint16_t ws = (uint16_t)((uint16_t)(s1[i] * 5u) ^ (uint16_t)x);
                        CHECK(bo[i] == wb, "uchar [%lu] = %u (want %u)", (unsigned long)i, bo[i], wb);
                        CHECK(so[i] == ws, "ushort [%lu] = %u (want %u)", (unsigned long)i, so[i], ws);
                        break;
                    }
                    case 4: {
                        float x = h2f(h0[i]);
                        float y = h2f(f2h(h2f(f2h(x * 0.75f)) + 1.0f));
                        float w = h2f(f2h(h2f(f2h(y * y)) - h2f(f2h(0.5f * x))));
                        CHECK(near(h2f(ho[i]), w, 2e-3f, 2e-3f), "[%lu] = %g (want %g)", (unsigned long)i, h2f(ho[i]), w);
                        break;
                    }
                    case 5: {
                        uint32_t x = u0[i];
                        uint32_t rev = 0; for (int k = 0; k < 32; k++) if (x >> k & 1) rev |= 1u << (31 - k);
                        uint32_t w[4] = { (uint32_t)__builtin_popcount(x), x ? (uint32_t)__builtin_clz(x) : 32u,
                                          x ? (uint32_t)__builtin_ctz(x) : 32u, rev };
                        for (int k = 0; k < 4; k++)
                            CHECK(uo[i * 4 + k] == w[k], "[%lu.%d] of %08X = %u (want %u)", (unsigned long)i, k, x, uo[i * 4 + k], w[k]);
                        break;
                    }
                    case 6:
                        if (i < 256) CHECK(u0[i] == (uint32_t)((i % 64) * 1000 + i / 64), "[%lu] = %u (want %lu)",
                                           (unsigned long)i, u0[i], (unsigned long)((i % 64) * 1000 + i / 64));
                        break;
                    }
                }
                bool ok = !r && right == checked && checked;
                printf("    %-12s [%s] %s | right %lu/%lu%s%s -> %s\n", names[t], log.UTF8String, cbStatus(cb.status),
                       (unsigned long)right, (unsigned long)checked, bad[0] ? " | first wrong " : "", bad,
                       ok ? "OUR COMPILER WORKS" : "WRONG");
                fflush(stdout);
                if (!ok) fails++;
                for (int k = 0; k < 4; k++) [b[k] release];
                if (r && stopIfHung(names[t])) { fails++; break; }
            }
        }
        printf("    maths/types test: %s\n", fails ? "FAILED" : "ALL OK");
        return fails ? 1 : 0;
    }
}

// --sync (child): milestone 3f - threadgroup memory, barriers, atomics
static bool coreHas3f(void);
static int syncTest(void) {
    @autoreleasepool {
        id<MTLDevice> d = ourDevice();
        if (!d) { printf("    no IrisXe device\n"); return 2; }
        if (!coreHas3f()) {
            printf("    IrisXeCore is older than 0.28.0 (no barrier support): install the new IrisXeCore.kext in EFI/OC/Kexts and reboot\n");
            return 4;
        }
        id<MTLLibrary> lib = testLibrary(d);
        if (!lib) return 3;
        id<MTLCommandQueue> q = [d newCommandQueue];
        int fails = 0;
        const char *names[] = { "tg_reverse", "tg_sum", "atomic_hist", "tg_count" };
        const char *how[] = { "64 groups of 64", "16 groups of 256", "10000 threads in groups of 64",
                              "1003 threads in groups of 128 (last group 107)" };
        for (int t = 0; t < 4; t++) {
            @autoreleasepool {
                id<MTLFunction> fn = [lib newFunctionWithName:@(names[t])];
                NSError *e = nil;
                id<MTLComputePipelineState> ps = fn ? [d newComputePipelineStateWithFunction:fn error:&e] : nil;
                if (!ps) {
                    printf("    %-12s compile: %s -> FAILED\n", names[t], e ? e.localizedDescription.UTF8String : "no function");
                    fails++;
                    continue;
                }
                NSString *log = ((id (*)(id, SEL))objc_msgSend)((id)d, sel_registerName("irisxeLastCompileLog"));
                const NSUInteger N = 16384;
                id<MTLBuffer> b[3];
                for (int k = 0; k < 3; k++) {
                    b[k] = [d newBufferWithLength:N * 4 options:MTLResourceStorageModeShared];
                    memset(b[k].contents, 0xEE, N * 4);
                }
                uint32_t *in = (uint32_t *)b[0].contents, *o1 = (uint32_t *)b[1].contents, *o2 = (uint32_t *)b[2].contents;
                float *fin = (float *)b[0].contents, *fo = (float *)b[1].contents;
                for (NSUInteger i = 0; i < N; i++) in[i] = (uint32_t)(i * 2654435761u >> 7) & 0xFFFF;
                if (t == 0) for (NSUInteger i = 0; i < N; i++) fin[i] = (float)i * 0.5f + 1.0f;
                if (t == 2) { memset(o1, 0, 64); o2[0] = 0; o2[1] = 0; o2[2] = 0xFFFFFFFFu; }
                if (t == 3) o2[0] = 0;
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
                [enc setComputePipelineState:ps];
                for (int k = 0; k < 3; k++) [enc setBuffer:b[k] offset:0 atIndex:k];
                switch (t) {
                case 0: [enc dispatchThreadgroups:MTLSizeMake(64, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)]; break;
                case 1: [enc dispatchThreadgroups:MTLSizeMake(16, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)]; break;
                case 2: [enc dispatchThreads:MTLSizeMake(10000, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)]; break;
                case 3: [enc dispatchThreads:MTLSizeMake(1003, 1, 1) threadsPerThreadgroup:MTLSizeMake(128, 1, 1)]; break;
                }
                [enc endEncoding];
                int r = commitWait(cb);
                NSUInteger right = 0, checked = 0; char bad[160] = "";
                #define CHK(cond, ...) do { checked++; if (cond) right++; else if (!bad[0]) snprintf(bad, sizeof(bad), __VA_ARGS__); } while (0)
                switch (t) {
                case 0:
                    for (NSUInteger i = 0; i < 4096; i++) {
                        float w = fin[(i / 64) * 64 + 63 - i % 64];
                        CHK(fo[i] == w, "[%lu] = %g (want %g)", (unsigned long)i, fo[i], w);
                    }
                    CHK(o1[4096] == 0xEEEEEEEEu, "[4096] written (past the end)");
                    break;
                case 1:
                    for (int g = 0; g < 16; g++) {
                        uint32_t w = 0; for (int k = 0; k < 256; k++) w += in[g * 256 + k];
                        CHK(o1[g] == w, "group %d = %u (want %u)", g, o1[g], w);
                    }
                    break;
                case 2: {
                    uint32_t bins[16] = {0}, sum = 0, mx = 0, mn = 0xFFFFFFFFu;
                    for (int i = 0; i < 10000; i++) { uint32_t v = in[i]; bins[v & 15]++; sum += v & 255; if (v > mx) mx = v; if (v < mn) mn = v; }
                    for (int k = 0; k < 16; k++) CHK(o1[k] == bins[k], "bin %d = %u (want %u)", k, o1[k], bins[k]);
                    CHK(o2[0] == sum, "sum = %u (want %u)", o2[0], sum);
                    CHK(o2[1] == mx, "max = %u (want %u)", o2[1], mx);
                    CHK(o2[2] == mn, "min = %u (want %u)", o2[2], mn);
                    break;
                }
                case 3: {
                    uint32_t all = 0;
                    for (int g = 0; g < 8; g++) {
                        uint32_t w = 0; for (int i = g * 128; i < 1003 && i < (g + 1) * 128; i++) w += in[i] & 1;
                        all += w;
                        CHK(o1[g] == w, "group %d = %u (want %u)", g, o1[g], w);
                    }
                    CHK(o2[0] == all, "grand total = %u (want %u)", o2[0], all);
                    break;
                }
                }
                bool ok = !r && right == checked && checked;
                printf("    %-12s %s [%s] %s | right %lu/%lu%s%s -> %s\n", names[t], how[t], log.UTF8String, cbStatus(cb.status),
                       (unsigned long)right, (unsigned long)checked, bad[0] ? " | first wrong " : "", bad,
                       ok ? "OUR COMPILER WORKS" : "WRONG");
                fflush(stdout);
                if (!ok) fails++;
                for (int k = 0; k < 3; k++) [b[k] release];
                if (stopIfHung(names[t])) { fails++; break; }
            }
        }
        printf("    threadgroup memory / barrier / atomic test: %s\n", fails ? "FAILED" : "ALL OK");
        return fails ? 1 : 0;
    }
}

// --big (child): v0.29.0 - register reuse, kernels bigger than 3968 bytes
static uint32_t refBigMix(uint32_t x, uint32_t n) {
    uint32_t a[40];
    for (int i = 0; i < 40; i++) { x = x * 1664525u + 1013904223u + (uint32_t)i; a[i] = x ^ (x >> 13); }
    uint32_t t = a[0];
    for (int i = 0; i < 300; i++) {
        uint32_t b = a[(i * 7) % 40];
        if (i % 3 == 0) t = t * 3u + b;
        else if (i % 3 == 1) t = (t ^ b) + (t >> 5);
        else t = (t - b) ^ (t << 3);
    }
    uint32_t p = t, q = 5u, w = 0u;
    for (uint32_t j = 0; j < n; j++) {
        uint32_t pa = p + a[3] * j;
        uint32_t qx = q ^ (a[17] >> (j & 7u));
        if (pa & 1u) { qx += a[29]; w++; }
        q = qx; p = pa ^ q;
    }
    uint32_t r = (p + q) ^ w;
    for (int i = 0; i < 40; i++) r = (r ^ a[i]) + (r >> 7);
    return r;
}
static uint32_t refWideLive(uint32_t x, uint32_t n) {
    uint32_t a[64];
    for (int i = 0; i < 64; i++) { x = (x ^ (x << 5)) + 0x9e3779b9u * (uint32_t)(i + 1); a[i] = x; }
    uint32_t s = 0u;
    for (uint32_t j = 0; j < n; j++)
        for (int i = 0; i < 64; i += 8) s = (s + a[i] * (j + 1u)) ^ (a[i + 3] >> (j & 15u));
    for (int i = 0; i < 64; i++) s ^= a[i] * (uint32_t)(i + 1);
    return s;
}
static uint32_t refStraight(uint32_t x) {          // the big_straight kernel on the CPU
    uint32_t y = x ^ 0x9e3779b9u;
#include "big_straight.inc"
    return x ^ y;
}
static int bigTest(void) {
    @autoreleasepool {
        id<MTLDevice> d = ourDevice();
        if (!d) { printf("    no IrisXe device\n"); return 2; }
        id<MTLLibrary> lib = testLibrary(d);
        if (!lib) return 3;
        id<MTLCommandQueue> q = [d newCommandQueue];
        int fails = 0;
        const char *names[] = { "big_mix", "wide_live", "big_straight" };
        for (int t = 0; t < 3; t++) {
            @autoreleasepool {
                id<MTLFunction> fn = [lib newFunctionWithName:@(names[t])];
                NSError *e = nil;
                id<MTLComputePipelineState> ps = fn ? [d newComputePipelineStateWithFunction:fn error:&e] : nil;
                if (!ps) {
                    printf("    %-10s compile: %s -> FAILED\n", names[t], e ? e.localizedDescription.UTF8String : "no function");
                    fails++;
                    continue;
                }
                NSString *log = ((id (*)(id, SEL))objc_msgSend)((id)d, sel_registerName("irisxeLastCompileLog"));
                const NSUInteger N = 4096;
                const uint32_t loops[2] = { 0, 9 };
                for (int li = 0; li < (t == 2 ? 1 : 2); li++) {
                    id<MTLBuffer> in = [d newBufferWithLength:N * 4 options:MTLResourceStorageModeShared];
                    id<MTLBuffer> out = [d newBufferWithLength:(N + 64) * 4 options:MTLResourceStorageModeShared];
                    id<MTLBuffer> nb = [d newBufferWithLength:256 options:MTLResourceStorageModeShared];
                    uint32_t *pi = (uint32_t *)in.contents, *po = (uint32_t *)out.contents;
                    for (NSUInteger i = 0; i < N; i++) pi[i] = (uint32_t)(i * 2654435761u + 12345u);
                    memset(po, 0xEE, (N + 64) * 4);
                    *(uint32_t *)nb.contents = loops[li];
                    id<MTLCommandBuffer> cb = [q commandBuffer];
                    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
                    [enc setComputePipelineState:ps];
                    [enc setBuffer:in offset:0 atIndex:0];
                    [enc setBuffer:out offset:0 atIndex:1];
                    [enc setBuffer:nb offset:0 atIndex:2];
                    [enc dispatchThreads:MTLSizeMake(N, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
                    [enc endEncoding];
                    int r = commitWait(cb);
                    NSUInteger right = 0, bad = NSNotFound;
                    for (NSUInteger i = 0; i < N; i++) {
                        uint32_t w = t == 0 ? refBigMix(pi[i], loops[li]) : t == 1 ? refWideLive(pi[i], loops[li]) : refStraight(pi[i]);
                        if (po[i] == w) right++; else if (bad == NSNotFound) bad = i;
                    }
                    bool after = po[N] == 0xEEEEEEEEu;
                    bool ok = !r && right == N && after;
                    printf("    %-10s n=%u [%s] %s | right %lu/%lu%s", names[t], loops[li], log.UTF8String, cbStatus(cb.status),
                           (unsigned long)right, (unsigned long)N, after ? "" : " | wrote past the end");
                    if (bad != NSNotFound)
                        printf(" | first wrong [%lu] = %08X (want %08X)", (unsigned long)bad, po[bad],
                               t == 0 ? refBigMix(pi[bad], loops[li]) : t == 1 ? refWideLive(pi[bad], loops[li]) : refStraight(pi[bad]));
                    printf(" -> %s\n", ok ? "OUR COMPILER WORKS" : "WRONG");
                    fflush(stdout);
                    if (!ok) fails++;
                    [in release]; [out release]; [nb release];
                    if (stopIfHung(names[t])) return 1;
                }
            }
        }
        printf("    big kernel test: %s\n", fails ? "FAILED" : "ALL OK");
        return fails ? 1 : 0;
    }
}

// --textures (child): milestone 4 - textures in compute kernels
static bool coreHas4(void);
static id<MTLTexture> mkTex(id<MTLDevice> d, MTLPixelFormat f, NSUInteger w, NSUInteger h, NSUInteger layers, MTLTextureUsage u) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:w height:h mipmapped:NO];
    if (layers > 1) { td.textureType = MTLTextureType2DArray; td.arrayLength = layers; }
    td.usage = u;
    td.storageMode = MTLStorageModeShared;
    return [d newTextureWithDescriptor:td];
}
// CPU reference sampler: mode 0 clamp to edge, 1 repeat, 2 mirrored repeat, 3 clamp to zero
static int refAddr(int x, int n, int mode) {
    if (mode == 0) return x < 0 ? 0 : x >= n ? n - 1 : x;
    if (mode == 1) return ((x % n) + n) % n;
    if (mode == 2) { int m = ((x % (2 * n)) + 2 * n) % (2 * n); return m < n ? m : 2 * n - 1 - m; }
    return (x < 0 || x >= n) ? -1 : x;
}
static void refSample(const float *t, int w, int h, float u, float v, int mode, bool linear, bool normalized, float out[4]) {
    if (normalized) { u *= w; v *= h; }
    int xs[2], ys[2]; float fx = 0, fy = 0;
    if (linear) {
        float a = u - 0.5f, b = v - 0.5f;
        xs[0] = (int)floorf(a); ys[0] = (int)floorf(b); fx = a - xs[0]; fy = b - ys[0];
    } else { xs[0] = (int)floorf(u); ys[0] = (int)floorf(v); }
    xs[1] = xs[0] + 1; ys[1] = ys[0] + 1;
    float wt[2][2] = { { (1 - fx) * (1 - fy), fx * (1 - fy) }, { (1 - fx) * fy, fx * fy } };
    for (int k = 0; k < 4; k++) out[k] = 0;
    for (int j = 0; j < (linear ? 2 : 1); j++)
        for (int i = 0; i < (linear ? 2 : 1); i++) {
            int x = refAddr(xs[i], w, mode), y = refAddr(ys[j], h, mode);
            float c = linear ? wt[j][i] : 1.0f;
            for (int k = 0; k < 4; k++) out[k] += (x < 0 || y < 0) ? 0.0f : c * t[(y * w + x) * 4 + k];
        }
}
static int texturesTest(void) {
    @autoreleasepool {
        id<MTLDevice> d = ourDevice();
        if (!d) { printf("    no IrisXe device\n"); return 2; }
        if (!coreHas4()) {
            printf("    IrisXeCore is older than 0.30.0 (no texture support): install the new IrisXeCore.kext in EFI/OC/Kexts and reboot\n");
            return 4;
        }
        id<MTLLibrary> lib = testLibrary(d);
        if (!lib) return 3;
        id<MTLCommandQueue> q = [d newCommandQueue];
        const NSUInteger W = 64;
        int fails = 0;
        const char *names[] = { "tex_copy", "tex_copy", "tex_uint_rw", "tex_size", "tex_sample", "tex_half_write",
                                "tex_short_coord", "tex_array_write", "tex_sample_const", "cs_nearest_edge",
                                "cs_linear_repeat", "cs_nearest_mirror", "cs_pixel_coords", "cs_clamp_zero" };
        const char *how[] = { "RGBA32Float 64x64 -> RGBA32Float", "RGBA8Unorm 64x64 -> RGBA8Unorm", "R32Uint read_write",
                              "100x37 size", "linear sampler", "RGBA16Float write", "ushort2 coords",
                              "2D array 16x16 x4 layers", "constexpr linear, edge", "constexpr nearest, edge",
                              "constexpr linear, repeat", "constexpr nearest, mirrored", "constexpr pixel coords",
                              "constexpr linear, clamp to zero" };
        // constexpr sampler kernels (8..13): address mode, linear, normalized (as in kernels.metal)
        const int csMode[] = { 0, 0, 1, 2, 0, 3 }; const bool csLin[] = { 1, 0, 1, 0, 0, 1 }, csNorm[] = { 1, 1, 1, 1, 0, 1 };
        for (int t = 0; t < 14; t++) {
            @autoreleasepool {
                id<MTLFunction> fn = [lib newFunctionWithName:@(names[t])];
                NSError *e = nil;
                id<MTLComputePipelineState> ps = fn ? [d newComputePipelineStateWithFunction:fn error:&e] : nil;
                if (!ps) {
                    printf("    %-16s %s compile: %s -> FAILED\n", names[t], how[t], e ? e.localizedDescription.UTF8String : "no function");
                    fails++;
                    continue;
                }
                NSString *log = ((id (*)(id, SEL))objc_msgSend)((id)d, sel_registerName("irisxeLastCompileLog"));
                MTLTextureUsage rw = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
                id<MTLTexture> a = nil, b = nil;
                id<MTLBuffer> out = [d newBufferWithLength:W * W * 16 + 256 options:MTLResourceStorageModeShared];
                memset(out.contents, 0xEE, W * W * 16 + 256);
                float *fsrc = malloc(W * W * 16); uint8_t *bsrc = malloc(W * W * 4); uint32_t *usrc = malloc(W * W * 4);
                for (NSUInteger y = 0; y < W; y++)
                    for (NSUInteger x = 0; x < W; x++) {
                        float *p = &fsrc[(y * W + x) * 4];
                        p[0] = x * 0.25f; p[1] = y * 0.5f; p[2] = (x ^ y) * 0.125f; p[3] = 1.0f;
                        uint8_t *c = &bsrc[(y * W + x) * 4];
                        c[0] = (uint8_t)(x * 4); c[1] = (uint8_t)(y * 4); c[2] = (uint8_t)((x * 7 + y * 3) & 255); c[3] = 200;
                        usrc[y * W + x] = (uint32_t)(x * 1000 + y);
                    }
                MTLRegion all = MTLRegionMake2D(0, 0, W, W);
                MTLSize groups = MTLSizeMake(1, W, 1), tpg = MTLSizeMake(W, 1, 1);
                id<MTLSamplerState> smp = nil;
                switch (t) {
                case 0: a = mkTex(d, MTLPixelFormatRGBA32Float, W, W, 1, rw); b = mkTex(d, MTLPixelFormatRGBA32Float, W, W, 1, rw);
                        [a replaceRegion:all mipmapLevel:0 withBytes:fsrc bytesPerRow:W * 16]; break;
                case 1: a = mkTex(d, MTLPixelFormatRGBA8Unorm, W, W, 1, rw); b = mkTex(d, MTLPixelFormatRGBA8Unorm, W, W, 1, rw);
                        [a replaceRegion:all mipmapLevel:0 withBytes:bsrc bytesPerRow:W * 4]; break;
                case 2: a = mkTex(d, MTLPixelFormatR32Uint, W, W, 1, rw);
                        [a replaceRegion:all mipmapLevel:0 withBytes:usrc bytesPerRow:W * 4]; break;
                case 3: a = mkTex(d, MTLPixelFormatRGBA32Float, 100, 37, 1, rw); groups = MTLSizeMake(1, 1, 1); break;
                case 8: case 9: case 10: case 11: case 12: case 13:
                        a = mkTex(d, MTLPixelFormatRGBA32Float, W, W, 1, rw);
                        [a replaceRegion:all mipmapLevel:0 withBytes:fsrc bytesPerRow:W * 16];
                        if (t > 8) groups = MTLSizeMake(1, 1, 1), tpg = MTLSizeMake(128, 1, 1);   // 1D: u = v = gid / 64
                        break;
                case 4: {
                        a = mkTex(d, MTLPixelFormatRGBA32Float, W, W, 1, rw);
                        [a replaceRegion:all mipmapLevel:0 withBytes:fsrc bytesPerRow:W * 16];
                        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
                        sd.minFilter = MTLSamplerMinMagFilterLinear; sd.magFilter = MTLSamplerMinMagFilterLinear;
                        sd.sAddressMode = MTLSamplerAddressModeClampToEdge; sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
                        sd.normalizedCoordinates = YES;
                        smp = [d newSamplerStateWithDescriptor:sd];
                        [sd release];
                        break; }
                case 5: a = mkTex(d, MTLPixelFormatRGBA16Float, W, W, 1, rw); break;
                case 6: a = mkTex(d, MTLPixelFormatRGBA32Float, W, W, 1, rw);
                        [a replaceRegion:all mipmapLevel:0 withBytes:fsrc bytesPerRow:W * 16]; break;
                case 7: a = mkTex(d, MTLPixelFormatRGBA32Float, 16, 16, 4, rw); groups = MTLSizeMake(1, 16, 4); tpg = MTLSizeMake(16, 1, 1); break;
                }
                if (!a || ((t == 0 || t == 1) && !b) || (t == 4 && !smp)) {   // (8..13: samplers in the shader)
                    printf("    %-16s %s: could not make the texture / sampler -> FAILED\n", names[t], how[t]);
                    fails++;
                } else {
                    id<MTLCommandBuffer> cb = [q commandBuffer];
                    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
                    [enc setComputePipelineState:ps];
                    [enc setTexture:a atIndex:0];
                    if (b) [enc setTexture:b atIndex:1];
                    if (smp) [enc setSamplerState:smp atIndex:0];
                    [enc setBuffer:out offset:0 atIndex:0];
                    if (t == 3) [enc dispatchThreads:MTLSizeMake(50, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
                    else if (t > 8) [enc dispatchThreads:MTLSizeMake(128, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
                    else [enc dispatchThreadgroups:groups threadsPerThreadgroup:tpg];
                    [enc endEncoding];
                    int r = commitWait(cb);
                    NSUInteger right = 0, checked = 0; char bad[200] = "";
                    #define TCHK(cond, ...) do { checked++; if (cond) right++; else if (!bad[0]) snprintf(bad, sizeof(bad), __VA_ARGS__); } while (0)
                    float *fo = (float *)out.contents; uint32_t *uo = (uint32_t *)out.contents;
                    if (t == 0) {
                        float *g = malloc(W * W * 16);
                        [b getBytes:g bytesPerRow:W * 16 fromRegion:all mipmapLevel:0];
                        for (NSUInteger i = 0; i < W * W * 4; i++) TCHK(g[i] == fsrc[i] * 0.5f, "[%lu] = %g (want %g)", (unsigned long)i, g[i], fsrc[i] * 0.5f);
                        free(g);
                    } else if (t == 1) {
                        uint8_t *g = malloc(W * W * 4);
                        [b getBytes:g bytesPerRow:W * 4 fromRegion:all mipmapLevel:0];
                        for (NSUInteger i = 0; i < W * W * 4; i++) {
                            int want = (int)lrintf(bsrc[i] / 255.0f * 0.5f * 255.0f);
                            TCHK(abs((int)g[i] - want) <= 1, "byte [%lu] = %u (want %d)", (unsigned long)i, g[i], want);
                        }
                        free(g);
                    } else if (t == 2) {
                        uint32_t *g = malloc(W * W * 4);
                        [a getBytes:g bytesPerRow:W * 4 fromRegion:all mipmapLevel:0];
                        for (NSUInteger i = 0; i < W * W; i++) TCHK(g[i] == usrc[i] + 1, "[%lu] = %u (want %u)", (unsigned long)i, g[i], usrc[i] + 1);
                        free(g);
                    } else if (t == 3) {
                        for (int i = 0; i < 50; i++) TCHK(uo[i] == 100u * 65536 + 37, "[%d] = %08X (want %08X)", i, uo[i], 100u * 65536 + 37);
                    } else if (t >= 8) {
                        int k8 = t - 8;
                        NSUInteger n = t == 8 ? W * W : 128;
                        for (NSUInteger i = 0; i < n; i++) {
                            float u, v, want[4];
                            if (t == 8) { u = ((i % W) + 0.25f) / 64.0f; v = ((i / W) + 0.25f) / 64.0f; }
                            else u = v = i / 64.0f;
                            refSample(fsrc, (int)W, (int)W, u, v, csMode[k8], csLin[k8], csNorm[k8], want);
                            for (int c = 0; c < 4; c++)
                                TCHK(fabsf(fo[i * 4 + c] - want[c]) <= 2e-3f * fmaxf(1.0f, fabsf(want[c])), "[%lu].%d = %g (want %g)",
                                     (unsigned long)i, c, fo[i * 4 + c], want[c]);
                        }
                    } else if (t == 4 || t == 6) {
                        for (NSUInteger i = 0; i < W * W * 4; i++)
                            TCHK(fabsf(fo[i] - fsrc[i]) <= 1e-3f, "[%lu] = %g (want %g)", (unsigned long)i, fo[i], fsrc[i]);
                    } else if (t == 5) {
                        uint16_t *g = malloc(W * W * 8);
                        [a getBytes:g bytesPerRow:W * 8 fromRegion:all mipmapLevel:0];
                        for (NSUInteger y = 0; y < W; y++)
                            for (NSUInteger x = 0; x < W; x++) {
                                float want[4] = { x / 64.0f, y / 64.0f, 0.25f, 1.0f };
                                for (int k = 0; k < 4; k++) {
                                    float got = h2f(g[(y * W + x) * 4 + k]);
                                    TCHK(fabsf(got - want[k]) <= 1e-3f, "(%lu,%lu).%d = %g (want %g)", (unsigned long)x, (unsigned long)y, k, got, want[k]);
                                }
                            }
                        free(g);
                    } else if (t == 7) {
                        float *g = malloc(16 * 16 * 16);
                        for (NSUInteger z = 0; z < 4; z++) {
                            [a getBytes:g bytesPerRow:16 * 16 bytesPerImage:16 * 16 * 16 fromRegion:MTLRegionMake2D(0, 0, 16, 16)
                                mipmapLevel:0 slice:z];
                            for (NSUInteger y = 0; y < 16; y++)
                                for (NSUInteger x = 0; x < 16; x++) {
                                    float *p = &g[(y * 16 + x) * 4];
                                    TCHK(p[0] == x && p[1] == y && p[2] == z && p[3] == 1.0f, "layer %lu (%lu,%lu) = %g %g %g %g",
                                         (unsigned long)z, (unsigned long)x, (unsigned long)y, p[0], p[1], p[2], p[3]);
                                }
                        }
                        free(g);
                    }
                    bool ok = !r && right == checked && checked;
                    printf("    %-16s %s [%s] %s | right %lu/%lu%s%s -> %s\n", names[t], how[t], log.UTF8String, cbStatus(cb.status),
                           (unsigned long)right, (unsigned long)checked, bad[0] ? " | first wrong " : "", bad,
                           ok ? "OUR TEXTURES WORK" : "WRONG");
                    fflush(stdout);
                    if (!ok) fails++;
                }
                free(fsrc); free(bsrc); free(usrc);
                [a release]; [b release]; [smp release]; [out release];
                if (stopIfHung(names[t])) { fails++; break; }
            }
        }
        printf("    texture test: %s\n", fails ? "FAILED" : "ALL OK");
        return fails ? 1 : 0;
    }
}

// IrisXeCore's GPU core is not "registered", so matching can't find it: look
// for it next to our accelerator (same PCI device) instead.
static io_service_t findSibling(const char *cls) {
    io_service_t a = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IrisXeAccelerator"));
    io_registry_entry_t pci = 0, child = 0;
    io_iterator_t it = 0;
    io_service_t found = 0;
    if (a && IORegistryEntryGetParentEntry(a, kIOServicePlane, &pci) == KERN_SUCCESS &&
        IORegistryEntryGetChildIterator(pci, kIOServicePlane, &it) == KERN_SUCCESS) {
        while ((child = IOIteratorNext(it))) {
            io_name_t c;
            if (!found && IOObjectGetClass(child, c) == KERN_SUCCESS && !strcmp(c, cls)) { found = child; continue; }
            IOObjectRelease(child);
        }
        IOObjectRelease(it);
    }
    if (pci) IOObjectRelease(pci);
    if (a) IOObjectRelease(a);
    return found;
}

static void printProp(const char *cls, CFStringRef key, const char *title) {
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(cls));
    if (!s) s = findSibling(cls);
    if (!s) { printf("    %s: (no %s)\n", title, cls); return; }
    CFTypeRef v = IORegistryEntryCreateCFProperty(s, key, kCFAllocatorDefault, 0);
    NSString *d = !v ? nil : (CFGetTypeID(v) == CFStringGetTypeID() ? (__bridge NSString *)v : [(__bridge id)v description]);
    printf("    %s: %s\n", title, d ? d.UTF8String : "(none)");
    if (v) CFRelease(v);
    IOObjectRelease(s);
}

static bool gpuHung(void) {
    io_service_t s = findSibling("IrisXeProbe");
    if (!s) s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IrisXeProbe"));
    if (!s) return false;
    bool hung = false;
    CFTypeRef v = IORegistryEntryCreateCFProperty(s, CFSTR("IrisXe-GPU"), kCFAllocatorDefault, 0);
    if (v && CFGetTypeID(v) == CFStringGetTypeID())
        hung = [(__bridge NSString *)v hasPrefix:@"HUNG"];
    if (v) CFRelease(v);
    IOObjectRelease(s);
    return hung;
}

// a word in IrisXeCore's IrisXe-Metal status (newer cores report more counters)
static bool coreSays(NSString *word) {
    io_service_t s = findSibling("IrisXeProbe");
    if (!s) s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IrisXeProbe"));
    if (!s) return false;
    bool ok = false;
    CFTypeRef v = IORegistryEntryCreateCFProperty(s, CFSTR("IrisXe-Metal"), kCFAllocatorDefault, 0);
    if (v && CFGetTypeID(v) == CFStringGetTypeID())
        ok = [(__bridge NSString *)v rangeOfString:word].location != NSNotFound;
    if (v) CFRelease(v);
    IOObjectRelease(s);
    return ok;
}
static bool coreHas4(void) { return coreSays(@"with textures"); }   // IrisXeCore 0.30.0+
// IrisXeCore 0.28.0+ reports barrier jobs in its status (ABI 3)
static bool coreHas3f(void) {
    io_service_t s = findSibling("IrisXeProbe");
    if (!s) s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IrisXeProbe"));
    if (!s) return false;
    bool ok = false;
    CFTypeRef v = IORegistryEntryCreateCFProperty(s, CFSTR("IrisXe-Metal"), kCFAllocatorDefault, 0);
    if (v && CFGetTypeID(v) == CFStringGetTypeID())
        ok = [(__bridge NSString *)v rangeOfString:@"with barriers"].location != NSNotFound;
    if (v) CFRelease(v);
    IOObjectRelease(s);
    return ok;
}

// --draw: milestone 5a - IrisXeCore draws a rectangle and a triangle with the
// 3D pipeline (its own commands and pixel shader) and checks the pixels.
// Started by setting the IrisXe-DrawTest property on IrisXeCore's GPU core.
static int drawTest(void) {
    io_service_t s = findSibling("IrisXeProbe");
    if (!s) s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IrisXeProbe"));
    if (!s) { printf("    no IrisXeCore GPU core found\n"); return 1; }
    CFTypeRef v = IORegistryEntryCreateCFProperty(s, CFSTR("IrisXe-Draw"), kCFAllocatorDefault, 0);
    if (!v) {
        printf("    IrisXeCore is older than 0.31.0 (no draw test): install the new IrisXeCore.kext in EFI/OC/Kexts and reboot\n");
        IOObjectRelease(s);
        return 2;
    }
    CFRelease(v);
    if (gpuHung()) { printf("    SKIPPED: the GPU is hung (an earlier test hung it) - reboot, then run again\n"); IOObjectRelease(s); return 3; }
    kern_return_t kr = IORegistryEntrySetCFProperty(s, CFSTR("IrisXe-DrawTest"), kCFBooleanTrue);
    v = IORegistryEntryCreateCFProperty(s, CFSTR("IrisXe-Draw"), kCFAllocatorDefault, 0);
    NSString *r = v && CFGetTypeID(v) == CFStringGetTypeID() ? (__bridge NSString *)v : nil;
    bool okA = r && [r rangeOfString:@"A DRAWN"].location != NSNotFound;
    bool okB = r && [r rangeOfString:@"B DRAWN"].location != NSNotFound;
    bool ok = kr == KERN_SUCCESS && okA && okB;
    printf("    request: %s (0x%x)\n", kr == KERN_SUCCESS ? "accepted" : "REFUSED", kr);
    for (NSString *part in (r ? [r componentsSeparatedByString:@" | "] : @[@"(none)"])) printf("    %s\n", part.UTF8String);
    printf("    A, no vertex shader, one colour (5a):              %s\n", okA ? "OK" : "FAILED");
    printf("    B, vertex shader + clipping + blended colours (5b): %s\n", okB ? "OK" : "FAILED");
    if (v) CFRelease(v);
    IOObjectRelease(s);
    return ok ? 0 : 1;
}

// --render (child): milestone 5b - vertex and fragment functions compiled by our
// compiler (the bundle's libIrisXeCompiler.dylib), drawn by IrisXeCore (draw C:
// the triangle of draw B) and checked against a CPU reference per pixel
typedef int (*XeRenderCompileFn)(const void *, size_t, const char *, uint32_t *, uint32_t, uint32_t *,
                                 struct XeShaderInfo *, char *, size_t);
static const float kTriB[3][8] = {                      // same as tools/shadergen/render.c kVertsB
    { -0.75f, -0.75f, 0.5f, 1.0f,   1, 0, 0, 1 },
    {  0.75f, -0.75f, 0.5f, 1.0f,   0, 1, 0, 1 },
    {  0.0f,   1.5f,  1.0f, 2.0f,   0, 0, 1, 1 },
};
static int renderTest(const char *bundle) {
    @autoreleasepool {
        id<MTLDevice> d = ourDevice();
        if (!d) { printf("    no IrisXe device\n"); return 2; }
        id<MTLLibrary> lib = testLibrary(d);
        if (!lib) return 3;
        NSString *dylib = [[NSString stringWithUTF8String:bundle] stringByAppendingPathComponent:@"Contents/Frameworks/libIrisXeCompiler.dylib"];
        void *h = dlopen(dylib.fileSystemRepresentation, RTLD_NOW | RTLD_LOCAL);
        XeRenderCompileFn compile = h ? (XeRenderCompileFn)dlsym(h, "irisxe_compile_air_render") : NULL;
        if (!compile) { printf("    the bundle's compiler has no vertex/fragment support (%s): use the 0.31.3+ bundle\n", h ? "old" : dlerror()); return 4; }
        io_service_t core = findSibling("IrisXeProbe");
        if (!core) core = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IrisXeProbe"));
        CFTypeRef ready = core ? IORegistryEntryCreateCFProperty(core, CFSTR("IrisXe-DrawC"), kCFAllocatorDefault, 0) : NULL;
        if (!ready) {
            printf("    IrisXeCore is older than 0.31.2 (no draw C): install the new IrisXeCore.kext in EFI/OC/Kexts and reboot\n");
            if (core) IOObjectRelease(core);
            return 5;
        }
        CFRelease(ready);
        // screen positions and the reference image helpers
        double sx[3], sy[3], w[3];
        for (int i = 0; i < 3; i++) {
            w[i] = kTriB[i][3];
            sx[i] = (kTriB[i][0] / w[i] + 1.0) * 32.0;
            sy[i] = (1.0 - kTriB[i][1] / w[i]) * 32.0;
        }
        double area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
        struct { const char *vs, *fs, *what; } cases[] = {
            { "r5_vs",  "r5_fs",     "stage_in vertex input, colour output" },
            { "r5_vs",  "r5_fs_cut", "[[position]], discard_fragment, maths" },
            { "r5_vs2", "r5_fs2",    "flat, no_perspective and computed varyings" },
        };
        int fails = 0;
        for (int t = 0; t < 3; t++) {
            @autoreleasepool {
                static struct XeDrawTestCfg g;
                memset(&g, 0, sizeof(g));
                g.version = XE_DRAWTEST_VERSION;
                struct XeShaderInfo vi, fi;
                char vlog[512], flog[512];
                id<MTLFunction> vf = [lib newFunctionWithName:@(cases[t].vs)], ff = [lib newFunctionWithName:@(cases[t].fs)];
                NSData *vb = [(id)vf respondsToSelector:sel_registerName("bitcodeData")] ? ((id (*)(id, SEL))objc_msgSend)(vf, sel_registerName("bitcodeData")) : nil;
                NSData *fb = [(id)ff respondsToSelector:sel_registerName("bitcodeData")] ? ((id (*)(id, SEL))objc_msgSend)(ff, sel_registerName("bitcodeData")) : nil;
                if (!vb || !fb) { printf("    %-6s + %-9s: no AIR for the functions (old irisxe-kernels.metallib?) -> FAILED\n", cases[t].vs, cases[t].fs); fails++; continue; }
                int rv = compile(vb.bytes, vb.length, cases[t].vs, g.vs, 256, &g.vsDW, &vi, vlog, sizeof(vlog));
                int rf = compile(fb.bytes, fb.length, cases[t].fs, g.ps, 512, &g.psDW, &fi, flog, sizeof(flog));
                printf("    %-6s [%s]\n    %-6s [%s]\n", cases[t].vs, vlog, cases[t].fs, flog);
                if (rv || rf) { printf("      compile FAILED\n"); fails++; continue; }
                // the pipeline's part: what draw C's template needs from the shader info
                if (vi.numInputs != 2 || vi.inputLocation[0] != 0 || vi.inputLocation[1] != 1 || vi.pushRegs || fi.pushRegs || vi.vueSlots > 8) {
                    printf("      shader layout doesn't fit draw C (inputs %u, push %u/%u, VUE %u) -> FAILED\n", vi.numInputs, vi.pushRegs, fi.pushRegs, vi.vueSlots);
                    fails++; continue;
                }
                g.vsGrf = vi.grfStart; g.vsReadLen = vi.urbReadLength; g.psGrf = fi.grfStart; g.numAttr = fi.numVaryings;
                unsigned maxSrc = 0; bool linked = true;
                for (unsigned k = 0; k < fi.numVaryings; k++) {
                    unsigned j = 0;
                    while (j < vi.numVaryings && strcmp(vi.varyingName[j], fi.varyingName[k])) j++;
                    if (j == vi.numVaryings) { printf("      fragment input %s is not a vertex output -> FAILED\n", fi.varyingName[k]); linked = false; break; }
                    g.swiz[k] = j; if (j > maxSrc) maxSrc = j;
                    if (fi.varyingInterp[k] == XE_INTERP_FLAT) g.constInterp |= 1u << k;
                }
                if (!linked) { fails++; continue; }
                g.sbeReadLen = (maxSrc + 2) / 2;
                g.baryMode = ((fi.flags & XE_FS_PERSP) ? 1u : 0u) | ((fi.flags & XE_FS_LINEAR) ? 8u : 0u);
                g.nonPersp = (fi.flags & XE_FS_LINEAR) ? 1 : 0;
                g.kill = (fi.flags & XE_FS_DISCARD) ? 1 : 0;
                if (gpuHung()) { printf("      SKIPPED: the GPU is hung - reboot, then run again\n"); fails++; continue; }
                CFDataRef data = CFDataCreate(NULL, (const UInt8 *)&g, sizeof(g));
                kern_return_t kr = IORegistryEntrySetCFProperty(core, CFSTR("IrisXe-DrawShaders"), data);
                CFRelease(data);
                CFTypeRef res = IORegistryEntryCreateCFProperty(core, CFSTR("IrisXe-DrawC"), kCFAllocatorDefault, 0);
                CFTypeRef px = IORegistryEntryCreateCFProperty(core, CFSTR("IrisXe-DrawPixels"), kCFAllocatorDefault, 0);
                NSString *rs = res && CFGetTypeID(res) == CFStringGetTypeID() ? (__bridge NSString *)res : @"(no result)";
                if (kr != KERN_SUCCESS || !px || CFGetTypeID(px) != CFDataGetTypeID() || CFDataGetLength(px) != 64 * 64 * 4 ||
                    ![rs hasPrefix:@"C DRAWN"]) {
                    printf("      draw: %s (0x%x) -> FAILED\n", rs.UTF8String, kr);
                    fails++;
                } else {
                    const uint32_t *p = (const uint32_t *)CFDataGetBytePtr(px);
                    unsigned good = 0, bad = 0, want = 0, maxd = 0, bx = 0, by = 0; uint32_t bgot = 0, bwant = 0;
                    for (int y = 0; y < 64; y++)
                        for (int x = 0; x < 64; x++) {
                            double qx = x + 0.5, qy = y + 0.5, l[3], dmin = 1e9;
                            for (int i = 0; i < 3; i++) {
                                int a = (i + 1) % 3, b = (i + 2) % 3;
                                double e = (sx[b] - sx[a]) * (qy - sy[a]) - (sy[b] - sy[a]) * (qx - sx[a]);
                                l[i] = e / area;
                                double len = sqrt((sx[b] - sx[a]) * (sx[b] - sx[a]) + (sy[b] - sy[a]) * (sy[b] - sy[a]));
                                double dd = l[i] * fabs(area) / len;
                                if (dd < dmin) dmin = dd;
                            }
                            if (fabs(dmin) <= 0.05) continue;          // on an edge: the fill rule decides
                            uint32_t exp = 0;
                            bool inside = dmin > 0 && !(t == 1 && qx < 32.0);
                            if (inside) {
                                double q[3], qs = 0, pc[4] = { 0 }, lc[4] = { 0 };
                                for (int i = 0; i < 3; i++) { q[i] = l[i] / w[i]; qs += q[i]; }
                                for (int i = 0; i < 3; i++) for (int k = 0; k < 4; k++) {
                                    pc[k] += q[i] / qs * kTriB[i][4 + k];
                                    lc[k] += l[i] * kTriB[i][4 + k];
                                }
                                double c[4];
                                if (t == 0) for (int k = 0; k < 4; k++) c[k] = pc[k];
                                else if (t == 1) { c[0] = 1 - pc[0]; c[1] = 1 - pc[1]; c[2] = 1 - pc[2]; c[3] = 1; }
                                else {
                                    double kk = 0;
                                    for (int i = 0; i < 3; i++) kk += q[i] / qs * (kTriB[i][5] * 0.5 + 0.25);
                                    c[0] = lc[0]; c[1] = kk; c[2] = kTriB[0][4]; c[3] = 1;
                                }
                                for (int k = 0; k < 4; k++) {
                                    double v = c[k] < 0 ? 0 : c[k] > 1 ? 1 : c[k];
                                    exp |= (uint32_t)floor(v * 255.0 + 0.5) << (8 * k);
                                }
                                want++;
                            }
                            uint32_t got = p[y * 64 + x];
                            unsigned dmax = 0;
                            for (int k = 0; k < 4; k++) {
                                int dk = abs((int)((got >> (8 * k)) & 0xFF) - (int)((exp >> (8 * k)) & 0xFF));
                                if ((unsigned)dk > dmax) dmax = (unsigned)dk;
                            }
                            if (inside && dmax > maxd) maxd = dmax;
                            if (dmax <= 2 && (inside || got == 0)) { if (inside) good++; }
                            else { if (!bad) { bx = x; by = y; bgot = got; bwant = exp; } bad++; }
                        }
                    printf("      %s: %u/%u pixels right (max diff %u), %u wrong%s", cases[t].what, good, want, maxd, bad, bad ? "" : "");
                    if (bad) printf(" (first (%u,%u) %08x want %08x)", bx, by, bgot, bwant);
                    printf(" -> %s\n", bad || good != want ? "FAILED" : "OUR SHADERS DRAW");
                    if (bad || good != want) fails++;
                }
                if (res) CFRelease(res);
                if (px) CFRelease(px);
            }
        }
        IOObjectRelease(core);
        printf("    render shader test: %s\n", fails ? "FAILED" : "ALL OK");
        return fails ? 1 : 0;
    }
}

// --metaldraw (child): milestone 5c - drawing through Metal's own render API:
// render pipeline, render pass (clear), render encoder, drawPrimitives, into one
// of our textures; every pixel is checked against a CPU reference
static uint32_t rgba8(const double c[4]) {
    uint32_t v = 0;
    for (int k = 0; k < 4; k++) { double x = c[k] < 0 ? 0 : c[k] > 1 ? 1 : c[k]; v |= (uint32_t)floor(x * 255.0 + 0.5) << (8 * k); }
    return v;
}
static int metalDrawTest(void) {
    @autoreleasepool {
        id<MTLDevice> d = ourDevice();
        if (!d) { printf("    no IrisXe device\n"); return 2; }
        if (![(id)d respondsToSelector:@selector(newRenderPipelineStateWithDescriptor:error:)] ||
            ![(id)d respondsToSelector:sel_registerName("irisxeLastRenderLog")]) {
            printf("    the bundle has no render pipelines yet: use the 0.32.0+ bundle\n"); return 4;
        }
        id<MTLLibrary> lib = testLibrary(d);
        if (!lib) return 3;
        id<MTLCommandQueue> q = [d newCommandQueue];
        double sx[3], sy[3], w[3];
        for (int i = 0; i < 3; i++) {
            w[i] = kTriB[i][3];
            sx[i] = (kTriB[i][0] / w[i] + 1.0) * 32.0;
            sy[i] = (1.0 - kTriB[i][1] / w[i]) * 32.0;
        }
        double area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
        // 4x4 texture for case 2: texel (x, y) = (x * 64 + 32, y * 64 + 32, 255 - x * 40, 255)
        uint32_t texels[16];
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++)
            texels[y * 4 + x] = (uint32_t)(x * 64 + 32) | (uint32_t)(y * 64 + 32) << 8 | (uint32_t)(255 - x * 40) << 16 | 0xFFu << 24;
        float pos[3][4], col[3][4];
        for (int i = 0; i < 3; i++) for (int k = 0; k < 4; k++) { pos[i][k] = kTriB[i][k]; col[i][k] = kTriB[i][4 + k]; }
        // ordered from safest to riskiest: a GPU hang stops the cases after it
        // kind: 0 colour, 1 culled, 2 texture (uv = colour.rg), 3 colour * k (fragment buffer),
        //       4 vertex_id + vertex buffers (push constants), 5 both + discard
        struct { const char *vs, *fs, *what; double clear[4]; int kind; } cases[] = {
            { "r5_vs",    "r5_fs",      "vertex descriptor + setVertexBytes, clear", { 0.1, 0.2, 0.3, 1.0 }, 0 },
            { "r5_vs",    "r5_fs",      "cull back faces (this triangle is a back face)", { 0.5, 0.5, 0.5, 1.0 }, 1 },
            { "r5_vs",    "r5_fs_tex",  "fragment texture + sampler", { 1, 1, 1, 1 }, 2 },
            { "r5_vs",    "r5_fs_k",    "fragment buffer (setFragmentBytes)", { 0, 0, 0, 1 }, 3 },
            { "rv_basic", "rf_color",   "vertex_id + vertex buffers", { 0, 0, 0, 1 }, 4 },
            { "rv_basic", "rf_discard", "vertex_id + buffers + setFragmentBytes + discard", { 0, 0, 0, 0 }, 5 },
            // v0.33.0: blending, scissor, write mask, indexed draws
            { "r5_vs",    "r5_fs_k",    "alpha blending (separate alpha factors)", { 0.25, 0.5, 0.75, 1 }, 6 },
            { "r5_vs",    "r5_fs",      "blend colour + ReverseSubtract", { 1, 1, 1, 1 }, 7 },
            { "r5_vs",    "r5_fs",      "scissor rectangle (16,8) 8x32", { 0.5, 0.5, 0.5, 1 }, 8 },
            { "r5_vs",    "r5_fs",      "write mask red + alpha", { 0, 0.5, 1, 1 }, 9 },
            { "r5_vs",    "r5_fs",      "drawIndexedPrimitives uint16 + baseVertex", { 0, 0, 0, 1 }, 10 },
            { "r5_vs",    "r5_fs",      "drawIndexedPrimitives uint32 strip + restart + offset", { 0, 0, 0, 1 }, 11 },
        };
        const int ncases = sizeof(cases) / sizeof(cases[0]);
        int fails = 0;
        for (int t = 0; t < ncases; t++) {
            @autoreleasepool {
                int kind = cases[t].kind;
                if (gpuHung()) { printf("    %-9s + %-10s SKIPPED: the GPU is hung\n", cases[t].vs, cases[t].fs); fails++; continue; }
                MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor new] autorelease];
                pd.vertexFunction = [[lib newFunctionWithName:@(cases[t].vs)] autorelease];
                pd.fragmentFunction = [[lib newFunctionWithName:@(cases[t].fs)] autorelease];
                pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
                MTLRenderPipelineColorAttachmentDescriptor *ca0 = pd.colorAttachments[0];
                if (kind == 6) {          // rgb: src.a / 1 - src.a; alpha: 1 / 1 - src.a
                    ca0.blendingEnabled = YES;
                    ca0.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha; ca0.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
                    ca0.sourceAlphaBlendFactor = MTLBlendFactorOne; ca0.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
                }
                if (kind == 7) {          // rgb: dst - src * blend colour; alpha: dst * 1 - src * 0
                    ca0.blendingEnabled = YES;
                    ca0.rgbBlendOperation = ca0.alphaBlendOperation = MTLBlendOperationReverseSubtract;
                    ca0.sourceRGBBlendFactor = MTLBlendFactorBlendColor; ca0.destinationRGBBlendFactor = MTLBlendFactorOne;
                    ca0.sourceAlphaBlendFactor = MTLBlendFactorZero; ca0.destinationAlphaBlendFactor = MTLBlendFactorOne;
                }
                if (kind == 9) ca0.writeMask = MTLColorWriteMaskRed | MTLColorWriteMaskAlpha;
                if (!strcmp(cases[t].vs, "r5_vs")) {
                    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
                    vd.attributes[0].format = MTLVertexFormatFloat4; vd.attributes[0].offset = 0; vd.attributes[0].bufferIndex = 0;
                    vd.attributes[1].format = MTLVertexFormatFloat4; vd.attributes[1].offset = 16; vd.attributes[1].bufferIndex = 0;
                    vd.layouts[0].stride = 32;
                    pd.vertexDescriptor = vd;
                }
                NSError *e = nil;
                id<MTLRenderPipelineState> ps = (!pd.vertexFunction || !pd.fragmentFunction) ? nil : [d newRenderPipelineStateWithDescriptor:pd error:&e];
                NSString *log = ((id (*)(id, SEL))objc_msgSend)((id)d, sel_registerName("irisxeLastRenderLog"));
                printf("    %-9s + %-10s pipeline: %s\n", cases[t].vs, cases[t].fs, ps ? log.UTF8String : (e ? e.localizedDescription.UTF8String : "no functions"));
                if (!ps) { printf("      -> FAILED\n"); fails++; continue; }
                MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:NO];
                td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
                td.storageMode = MTLStorageModeShared;
                id<MTLTexture> rt = [[d newTextureWithDescriptor:td] autorelease];
                if (!rt) { printf("      no render target texture -> FAILED\n"); fails++; [ps release]; continue; }
                id<MTLTexture> img = nil;
                id<MTLSamplerState> smp = nil;
                if (kind == 2) {
                    MTLTextureDescriptor *id4 = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO];
                    id4.storageMode = MTLStorageModeShared;
                    img = [[d newTextureWithDescriptor:id4] autorelease];
                    [img replaceRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0 withBytes:texels bytesPerRow:16];
                    MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor new] autorelease];
                    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
                    sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
                    smp = [[d newSamplerStateWithDescriptor:sd] autorelease];
                }
                MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                rp.colorAttachments[0].texture = rt;
                rp.colorAttachments[0].loadAction = MTLLoadActionClear;
                rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                rp.colorAttachments[0].clearColor = MTLClearColorMake(cases[t].clear[0], cases[t].clear[1], cases[t].clear[2], cases[t].clear[3]);
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
                if (!enc) { printf("      no render encoder -> FAILED\n"); fails++; [ps release]; continue; }
                [enc setRenderPipelineState:ps];
                if (kind == 1) [enc setCullMode:MTLCullModeBack];
                if (kind == 7) [enc setBlendColorRed:0.5f green:0.25f blue:1.0f alpha:1.0f];
                if (kind == 8) [enc setScissorRect:(MTLScissorRect){ 16, 8, 8, 32 }];
                if (kind == 10) {             // vertices 0, 1: decoys; the triangle at 2, 3, 4 (baseVertex 2)
                    float v[5][8];
                    for (int k = 0; k < 8; k++) { v[0][k] = k == 3 ? 1.0f : 0.9f; v[1][k] = k == 3 ? 1.0f : k < 2 ? -0.9f : 0.9f; }
                    for (int i = 0; i < 3; i++) for (int k = 0; k < 8; k++) v[2 + i][k] = kTriB[i][k];
                    [enc setVertexBytes:v length:sizeof(v) atIndex:0];
                } else if (!strcmp(cases[t].vs, "r5_vs")) {
                    float v[3][8];
                    for (int i = 0; i < 3; i++) for (int k = 0; k < 8; k++) v[i][k] = kTriB[i][k];
                    [enc setVertexBytes:v length:sizeof(v) atIndex:0];
                } else {
                    [enc setVertexBytes:pos length:sizeof(pos) atIndex:0];
                    [enc setVertexBytes:col length:sizeof(col) atIndex:1];
                }
                if (kind == 5) { float cut = 0.5f; [enc setFragmentBytes:&cut length:4 atIndex:0]; }
                if (kind == 3) { float k4[4] = { 0.5f, 1.0f, 0.25f, 1.0f }; [enc setFragmentBytes:k4 length:16 atIndex:0]; }
                if (kind == 6) { float k4[4] = { 1.0f, 1.0f, 1.0f, 0.5f }; [enc setFragmentBytes:k4 length:16 atIndex:0]; }
                if (kind == 2) { [enc setFragmentTexture:img atIndex:0]; [enc setFragmentSamplerState:smp atIndex:0]; }
                id<MTLBuffer> ib = nil;
                if (kind == 10) {
                    uint16_t idx[3] = { 0, 1, 2 };
                    ib = [[d newBufferWithBytes:idx length:sizeof(idx) options:MTLResourceStorageModeShared] autorelease];
                    [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:3 indexType:MTLIndexTypeUInt16 indexBuffer:ib
                             indexBufferOffset:0 instanceCount:1 baseVertex:2 baseInstance:0];
                } else if (kind == 11) {        // two strips of the same triangle, 4 bytes into the buffer
                    uint32_t idx[8] = { 7, 0, 1, 2, 0xFFFFFFFFu, 0, 1, 2 };
                    ib = [[d newBufferWithBytes:idx length:sizeof(idx) options:MTLResourceStorageModeShared] autorelease];
                    [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangleStrip indexCount:7 indexType:MTLIndexTypeUInt32 indexBuffer:ib
                             indexBufferOffset:4];
                } else [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [enc endEncoding];
                [cb commit];
                [cb waitUntilCompleted];
                uint32_t px[64 * 64];
                [rt getBytes:px bytesPerRow:256 fromRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0];
                uint32_t clr = rgba8(cases[t].clear);
                unsigned good = 0, bad = 0, want = 0, maxd = 0, bx = 0, by = 0, cleared = 0, inside = 0; uint32_t bgot = 0, bwant = 0;
                for (int y = 0; y < 64; y++)
                    for (int x = 0; x < 64; x++) {
                        double qx = x + 0.5, qy = y + 0.5, l[3], dmin = 1e9;
                        for (int i = 0; i < 3; i++) {
                            int a = (i + 1) % 3, b = (i + 2) % 3;
                            double ee = (sx[b] - sx[a]) * (qy - sy[a]) - (sy[b] - sy[a]) * (qx - sx[a]);
                            l[i] = ee / area;
                            double len = sqrt((sx[b] - sx[a]) * (sx[b] - sx[a]) + (sy[b] - sy[a]) * (sy[b] - sy[a]));
                            double dd = l[i] * fabs(area) / len;
                            if (dd < dmin) dmin = dd;
                        }
                        uint32_t got = px[y * 64 + x];
                        if (got == clr) cleared++;
                        if (fabs(dmin) <= 0.05) continue;
                        bool in = dmin > 0 && kind != 1;
                        if (kind == 8 && (x < 16 || x >= 24 || y < 8 || y >= 40)) in = false;
                        uint32_t exp = clr;
                        bool skip = false;
                        if (in) {
                            double q[3], qs = 0, pc[4] = { 0 }, uv[2] = { 0 };
                            for (int i = 0; i < 3; i++) { q[i] = l[i] / w[i]; qs += q[i]; }
                            for (int i = 0; i < 3; i++) {
                                for (int k = 0; k < 4; k++) pc[k] += q[i] / qs * kTriB[i][4 + k];
                                for (int k = 0; k < 2; k++) uv[k] += q[i] / qs * (kTriB[i][k] * 0.5 + 0.5);
                            }
                            const double *cc = cases[t].clear;
                            if (kind == 0 || kind == 4 || kind == 8 || kind == 10 || kind == 11) exp = rgba8(pc);
                            else if (kind == 6) {           // src = (colour.rgb, 0.5)
                                double c[4];
                                for (int k = 0; k < 3; k++) c[k] = pc[k] * 0.5 + cc[k] * 0.5;
                                c[3] = 0.5 + cc[3] * 0.5;
                                exp = rgba8(c);
                            } else if (kind == 7) {
                                static const double bc[3] = { 0.5, 0.25, 1.0 };
                                double c[4];
                                for (int k = 0; k < 3; k++) { c[k] = cc[k] - pc[k] * bc[k]; if (c[k] < 0) c[k] = 0; }
                                c[3] = cc[3];
                                exp = rgba8(c);
                            } else if (kind == 9) { double c[4] = { pc[0], cc[1], cc[2], pc[3] }; exp = rgba8(c); }
                            else if (kind == 3) { double c[4] = { pc[0] * 0.5, pc[1], pc[2] * 0.25, pc[3] }; exp = rgba8(c); }
                            else if (kind == 5) {
                                if (fabs(uv[0] - 0.5) < 0.002) skip = true;
                                double c[4] = { uv[0], uv[1], 0, 1 };
                                if (uv[0] >= 0.5) exp = rgba8(c); else in = false;   // discarded: the clear colour stays
                            } else {                                                 // texture at uv = colour.rg
                                double fx = pc[0] * 4, fy = pc[1] * 4;
                                if (fabs(fx - floor(fx + 0.5)) < 0.01 || fabs(fy - floor(fy + 0.5)) < 0.01) skip = true;
                                int tx = (int)floor(fx), ty = (int)floor(fy);
                                tx = tx < 0 ? 0 : tx > 3 ? 3 : tx; ty = ty < 0 ? 0 : ty > 3 ? 3 : ty;
                                uint32_t tv = texels[ty * 4 + tx];
                                double c[4];
                                for (int k = 0; k < 4; k++) c[k] = ((tv >> (8 * k)) & 0xFF) / 255.0;
                                exp = rgba8(c);
                            }
                        }
                        if (skip) continue;
                        if (in) { want++; inside++; }
                        unsigned dmax = 0;
                        for (int k = 0; k < 4; k++) {
                            int dk = abs((int)((got >> (8 * k)) & 0xFF) - (int)((exp >> (8 * k)) & 0xFF));
                            if ((unsigned)dk > dmax) dmax = (unsigned)dk;
                        }
                        if (in && dmax > maxd) maxd = dmax;
                        if (dmax <= 2) { if (in) good++; }
                        else { if (!bad) { bx = x; by = y; bgot = got; bwant = exp; } bad++; }
                    }
                printf("      %s: %u/%u inside right (max diff %u), %u wrong, %u cleared", cases[t].what, good, want, maxd, bad, cleared);
                if (bad) printf(" (first (%u,%u) %08x want %08x)", bx, by, bgot, bwant);
                bool ok = !bad && good == want && cb.status == MTLCommandBufferStatusCompleted;
                printf(" -> %s\n", ok ? "METAL DRAWS" : "FAILED");
                if (!ok) fails++;
                [ps release];
            }
        }
        printf("    Metal render test: %s\n", fails ? "FAILED" : "ALL OK");
        return fails ? 1 : 0;
    }
}


// ---- [18d] v0.34.0: depth and stencil ----
static id<MTLRenderPipelineState> dsPipe(id<MTLDevice> d, id<MTLLibrary> lib, const char *fs, MTLPixelFormat dpf,
                                          MTLPixelFormat spf, bool noColor) {
    MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor new] autorelease];
    pd.vertexFunction = [[lib newFunctionWithName:@"r5_vs"] autorelease];
    pd.fragmentFunction = [[lib newFunctionWithName:@(fs)] autorelease];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    if (noColor) pd.colorAttachments[0].writeMask = MTLColorWriteMaskNone;
    pd.depthAttachmentPixelFormat = dpf;
    pd.stencilAttachmentPixelFormat = spf;
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    vd.attributes[0].format = MTLVertexFormatFloat4; vd.attributes[0].offset = 0; vd.attributes[0].bufferIndex = 0;
    vd.attributes[1].format = MTLVertexFormatFloat4; vd.attributes[1].offset = 16; vd.attributes[1].bufferIndex = 0;
    vd.layouts[0].stride = 32;
    pd.vertexDescriptor = vd;
    NSError *e = nil;
    id<MTLRenderPipelineState> ps = (!pd.vertexFunction || !pd.fragmentFunction) ? nil : [d newRenderPipelineStateWithDescriptor:pd error:&e];
    if (!ps) {
        NSString *log = ((id (*)(id, SEL))objc_msgSend)((id)d, sel_registerName("irisxeLastRenderLog"));
        printf("      pipeline r5_vs + %s: %s\n", fs, e ? e.localizedDescription.UTF8String : log.UTF8String);
    }
    return [ps autorelease];
}
static id<MTLDepthStencilState> dsState(id<MTLDevice> d, MTLCompareFunction f, BOOL write, int stencil, uint32_t unused) {
    MTLDepthStencilDescriptor *sd = [[MTLDepthStencilDescriptor new] autorelease];
    sd.depthCompareFunction = f; sd.depthWriteEnabled = write;
    if (stencil) {                                   // 1: always, replace (writes the reference)   2: equal, keep
        MTLStencilDescriptor *st = [[MTLStencilDescriptor new] autorelease];
        st.stencilCompareFunction = stencil == 1 ? MTLCompareFunctionAlways : MTLCompareFunctionEqual;
        st.depthStencilPassOperation = stencil == 1 ? MTLStencilOperationReplace : MTLStencilOperationKeep;
        st.readMask = st.writeMask = 0xFF;
        sd.frontFaceStencil = st; sd.backFaceStencil = st;
    }
    (void)unused;
    return [[d newDepthStencilStateWithDescriptor:sd] autorelease];
}
static void dsDraw(id<MTLRenderCommandEncoder> enc, float zScale, const float *k, bool fullScreen) {
    float v[3][8];
    static const float big[3][8] = { { -1, -1, 0.5f, 1, 0, 0, 1, 1 }, { 3, -1, 0.5f, 1, 0, 0, 1, 1 }, { -1, 3, 0.5f, 1, 0, 0, 1, 1 } };
    for (int i = 0; i < 3; i++) for (int c = 0; c < 8; c++) v[i][c] = fullScreen ? big[i][c] : kTriB[i][c];
    for (int i = 0; i < 3; i++) v[i][2] *= zScale;
    [enc setVertexBytes:v length:sizeof(v) atIndex:0];
    if (k) [enc setFragmentBytes:k length:16 atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
}
static int depthTest(void) {
    @autoreleasepool {
        id<MTLDevice> d = ourDevice();
        if (!d) { printf("    no IrisXe device\n"); return 2; }
        if (![(id)d respondsToSelector:@selector(newDepthStencilStateWithDescriptor:)]) { printf("    the bundle has no depth / stencil yet: use the 0.34.0+ bundle\n"); return 4; }
        id<MTLLibrary> lib = testLibrary(d);
        if (!lib) return 3;
        id<MTLCommandQueue> q = [[d newCommandQueue] autorelease];
        double sx[3], sy[3], w[3];
        for (int i = 0; i < 3; i++) { w[i] = kTriB[i][3]; sx[i] = (kTriB[i][0] / w[i] + 1.0) * 32.0; sy[i] = (1.0 - kTriB[i][1] / w[i]) * 32.0; }
        double area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
        // kind 0: depth test + write (draw, fail behind, pass in front)   1: depth writes off   2: stencil only (Stencil8)
        //      3: depth + stencil (Depth32Float_Stencil8)   4: [[depth]] from the fragment function   5: as 0 with Depth16Unorm
        struct { const char *what; int kind; MTLPixelFormat dpf, spf; } cases[] = {
            { "Depth32Float: clear 1, Less + write; behind fails, in front wins", 0, MTLPixelFormatDepth32Float, MTLPixelFormatInvalid },
            { "Depth32Float: clear 0.5, depth writes off", 1, MTLPixelFormatDepth32Float, MTLPixelFormatInvalid },
            { "Stencil8: write 5 (no colour), then draw where stencil == 5", 2, MTLPixelFormatInvalid, MTLPixelFormatStencil8 },
            { "Depth32Float_Stencil8: the same, with a depth buffer", 3, MTLPixelFormatDepth32Float_Stencil8, MTLPixelFormatDepth32Float_Stencil8 },
            { "[[depth]] = colour.r, clear 0.5, Less", 4, MTLPixelFormatDepth32Float, MTLPixelFormatInvalid },
            { "Depth16Unorm: as the first", 5, MTLPixelFormatDepth16Unorm, MTLPixelFormatInvalid },
        };
        int fails = 0;
        for (int t = 0; t < 6; t++) {
            @autoreleasepool {
                int kind = cases[t].kind;
                if (gpuHung()) { printf("    case %d SKIPPED: the GPU is hung\n", t + 1); fails++; continue; }
                MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:NO];
                td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; td.storageMode = MTLStorageModeShared;
                id<MTLTexture> rt = [[d newTextureWithDescriptor:td] autorelease];
                MTLPixelFormat zpf = cases[t].dpf ? cases[t].dpf : cases[t].spf;
                MTLTextureDescriptor *zd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:zpf width:64 height:64 mipmapped:NO];
                zd.usage = MTLTextureUsageRenderTarget; zd.storageMode = MTLStorageModePrivate;
                id<MTLTexture> zt = [[d newTextureWithDescriptor:zd] autorelease];
                if (!rt || !zt) { printf("    %s: no textures -> FAILED\n", cases[t].what); fails++; continue; }
                bool stencil = kind == 2 || kind == 3;
                id<MTLRenderPipelineState> pA = dsPipe(d, lib, kind == 4 ? "r5_fs_depth" : "r5_fs", cases[t].dpf, cases[t].spf, stencil);
                id<MTLRenderPipelineState> pB = dsPipe(d, lib, stencil ? "r5_fs" : "r5_fs_k", cases[t].dpf, cases[t].spf, false);
                if (!pA || !pB) { printf("    %s -> FAILED\n", cases[t].what); fails++; continue; }
                double zclear = kind == 1 || kind == 4 ? 0.5 : 1.0;
                MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                rp.colorAttachments[0].texture = rt; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
                rp.colorAttachments[0].storeAction = MTLStoreActionStore; rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
                if (cases[t].dpf) {
                    rp.depthAttachment.texture = zt; rp.depthAttachment.loadAction = MTLLoadActionClear;
                    rp.depthAttachment.storeAction = MTLStoreActionStore; rp.depthAttachment.clearDepth = zclear;
                }
                if (cases[t].spf) {
                    rp.stencilAttachment.texture = zt; rp.stencilAttachment.loadAction = MTLLoadActionClear;
                    rp.stencilAttachment.storeAction = MTLStoreActionStore; rp.stencilAttachment.clearStencil = 0;
                }
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
                static const float kBlack[4] = { 0, 0, 0, 1 }, kTint[4] = { 0.5f, 1, 0.25f, 1 };
                if (kind == 0 || kind == 5) {
                    [enc setDepthStencilState:dsState(d, MTLCompareFunctionLess, YES, 0, 0)];
                    [enc setRenderPipelineState:pA]; dsDraw(enc, 1.0f, NULL, false);      // z 0.5: passes, writes 0.5
                    [enc setRenderPipelineState:pB]; dsDraw(enc, 1.5f, kBlack, false);    // z 0.75: behind, fails
                    dsDraw(enc, 0.5f, kTint, false);                                       // z 0.25: in front, wins
                } else if (kind == 1) {
                    [enc setDepthStencilState:dsState(d, MTLCompareFunctionLess, NO, 0, 0)];
                    [enc setRenderPipelineState:pA]; dsDraw(enc, 0.5f, NULL, false);      // z 0.25 passes, depth stays 0.5
                } else if (stencil) {
                    [enc setDepthStencilState:dsState(d, MTLCompareFunctionAlways, NO, 1, 0)];
                    [enc setStencilReferenceValue:5];
                    [enc setRenderPipelineState:pA]; dsDraw(enc, 1.0f, NULL, false);      // stencil 5 under the triangle, no colour
                    [enc setDepthStencilState:dsState(d, MTLCompareFunctionAlways, NO, 2, 0)];
                    [enc setRenderPipelineState:pB]; dsDraw(enc, 1.0f, NULL, true);       // blue where stencil == 5
                } else {
                    [enc setDepthStencilState:dsState(d, MTLCompareFunctionLess, YES, 0, 0)];
                    [enc setRenderPipelineState:pA]; dsDraw(enc, 1.0f, NULL, false);      // depth = colour.r
                }
                [enc endEncoding];
                [cb commit];
                [cb waitUntilCompleted];
                uint32_t px[64 * 64]; float zf[64 * 64]; uint16_t z16[64 * 64]; uint8_t s8[64 * 64];
                [rt getBytes:px bytesPerRow:256 fromRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0];
                // our textures can be read back whatever the storage mode (the bundle untiles depth / stencil)
                if (zpf == MTLPixelFormatDepth16Unorm) [zt getBytes:z16 bytesPerRow:128 fromRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0];
                else if (zpf == MTLPixelFormatStencil8) [zt getBytes:s8 bytesPerRow:64 fromRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0];
                else [zt getBytes:zf bytesPerRow:256 fromRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0];
                unsigned good = 0, want = 0, bad = 0, zbad = 0, bx = 0, by = 0, zx = 0, zy = 0; uint32_t bgot = 0, bwant = 0; double zgot = 0, zwant = 0;
                double clr[4] = { 0, 0, 0, 1 };
                for (int y = 0; y < 64; y++)
                    for (int x = 0; x < 64; x++) {
                        double qx = x + 0.5, qy = y + 0.5, l[3], dmin = 1e9;
                        for (int i = 0; i < 3; i++) {
                            int a = (i + 1) % 3, b = (i + 2) % 3;
                            double ee = (sx[b] - sx[a]) * (qy - sy[a]) - (sy[b] - sy[a]) * (qx - sx[a]);
                            l[i] = ee / area;
                            double len = sqrt((sx[b] - sx[a]) * (sx[b] - sx[a]) + (sy[b] - sy[a]) * (sy[b] - sy[a]));
                            double dd = l[i] * fabs(area) / len;
                            if (dd < dmin) dmin = dd;
                        }
                        if (fabs(dmin) <= 0.05) continue;
                        bool in = dmin > 0;
                        double q3[3], qs = 0, pc[4] = { 0 };
                        for (int i = 0; i < 3; i++) { q3[i] = l[i] / w[i]; qs += q3[i]; }
                        for (int i = 0; i < 3; i++) for (int k = 0; k < 4; k++) pc[k] += q3[i] / qs * kTriB[i][4 + k];
                        uint32_t exp = rgba8(clr);
                        double ez = zclear; int es = 0;
                        if (in) {
                            if (kind == 0 || kind == 5) { double c[4] = { pc[0] * 0.5, pc[1], pc[2] * 0.25, pc[3] }; exp = rgba8(c); ez = 0.25; }
                            else if (kind == 1) exp = rgba8(pc);
                            else if (stencil) { double c[4] = { 0, 0, 1, 1 }; exp = rgba8(c); es = 5; }
                            else {
                                if (fabs(pc[0] - 0.5) < 0.01) continue;
                                if (pc[0] < 0.5) { exp = rgba8(pc); ez = pc[0]; }
                            }
                            want++;
                        }
                        uint32_t got = px[y * 64 + x];
                        unsigned dmax = 0;
                        for (int k = 0; k < 4; k++) {
                            int dk = abs((int)((got >> (8 * k)) & 0xFF) - (int)((exp >> (8 * k)) & 0xFF));
                            if ((unsigned)dk > dmax) dmax = (unsigned)dk;
                        }
                        if (dmax <= 2) { if (in) good++; }
                        else { if (!bad) { bx = x; by = y; bgot = got; bwant = exp; } bad++; }
                        double gz; bool zok;
                        if (zpf == MTLPixelFormatStencil8) { gz = s8[y * 64 + x]; zok = (int)gz == es; ez = es; }
                        else if (zpf == MTLPixelFormatDepth16Unorm) { gz = z16[y * 64 + x] / 65535.0; zok = fabs(gz - ez) < 2.0 / 65535; }
                        else { gz = zf[y * 64 + x]; zok = fabs(gz - ez) < 1e-4; }
                        if (kind == 3) zok = fabs(gz - ez) < 1e-4;                   // the depth plane (no depth writes: the clear)
                        if (!zok) { if (!zbad) { zx = x; zy = y; zgot = gz; zwant = ez; } zbad++; }
                    }
                bool ok = !bad && !zbad && good == want && cb.status == MTLCommandBufferStatusCompleted;
                printf("    %s:\n      colour %u/%u inside right, %u wrong; %s %u wrong", cases[t].what, good, want, bad,
                       zpf == MTLPixelFormatStencil8 ? "stencil" : "depth", zbad);
                if (bad) printf(" (colour (%u,%u) %08x want %08x)", bx, by, bgot, bwant);
                if (zbad) printf(" (%s (%u,%u) %g want %g)", zpf == MTLPixelFormatStencil8 ? "stencil" : "depth", zx, zy, zgot, zwant);
                printf(" -> %s\n", ok ? "DEPTH / STENCIL WORK" : "FAILED");
                if (!ok) fails++;
            }
        }
        printf("    depth / stencil test: %s\n", fails ? "FAILED" : "ALL OK");
        return fails ? 1 : 0;
    }
}

// ---- [18e] v0.35.0: two render targets (RGBA8 + RGBA16Float), half output, [[position]].z, front_facing ----
static int mrtTest(void) {
    @autoreleasepool {
        id<MTLDevice> d = ourDevice();
        if (!d) { printf("    no IrisXe device\n"); return 2; }
        id<MTLLibrary> lib = testLibrary(d);
        if (!lib) return 3;
        id<MTLCommandQueue> q = [[d newCommandQueue] autorelease];
        double sx[3], sy[3], w[3];
        for (int i = 0; i < 3; i++) { w[i] = kTriB[i][3]; sx[i] = (kTriB[i][0] / w[i] + 1.0) * 32.0; sy[i] = (1.0 - kTriB[i][1] / w[i]) * 32.0; }
        double area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
        float pos[3][4], col[3][4];
        for (int i = 0; i < 3; i++) for (int k = 0; k < 4; k++) { pos[i][k] = kTriB[i][k]; col[i][k] = kTriB[i][4 + k]; }
        int fails = 0;
        for (int t = 0; t < 2; t++) {
            @autoreleasepool {
                if (gpuHung()) { printf("    case %d SKIPPED: the GPU is hung\n", t + 1); fails++; continue; }
                const char *fs = t == 0 ? "rf_mrt" : "rf_half";
                MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor new] autorelease];
                pd.vertexFunction = [[lib newFunctionWithName:@"rv_basic"] autorelease];
                pd.fragmentFunction = [[lib newFunctionWithName:@(fs)] autorelease];
                if (t == 0) {
                    pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
                    pd.colorAttachments[1].pixelFormat = MTLPixelFormatRGBA16Float;
                    pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
                } else pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
                NSError *e = nil;
                id<MTLRenderPipelineState> ps = (!pd.vertexFunction || !pd.fragmentFunction) ? nil : [d newRenderPipelineStateWithDescriptor:pd error:&e];
                NSString *log = ((id (*)(id, SEL))objc_msgSend)((id)d, sel_registerName("irisxeLastRenderLog"));
                printf("    rv_basic + %-8s pipeline: %s\n", fs, ps ? log.UTF8String : (e ? e.localizedDescription.UTF8String : "no functions"));
                if (!ps) { printf("      -> FAILED\n"); fails++; continue; }
                [ps autorelease];
                MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:NO];
                td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; td.storageMode = MTLStorageModeShared;
                MTLTextureDescriptor *th = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:64 height:64 mipmapped:NO];
                th.usage = td.usage; th.storageMode = MTLStorageModeShared;
                MTLTextureDescriptor *tz = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:64 height:64 mipmapped:NO];
                tz.usage = MTLTextureUsageRenderTarget; tz.storageMode = MTLStorageModePrivate;
                id<MTLTexture> r0 = [[d newTextureWithDescriptor:td] autorelease], r1 = [[d newTextureWithDescriptor:th] autorelease];
                id<MTLTexture> zt = [[d newTextureWithDescriptor:tz] autorelease];
                MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                id<MTLTexture> hrt = t == 0 ? r1 : r1;
                if (t == 0) {
                    rp.colorAttachments[0].texture = r0; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
                    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1); rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                    rp.colorAttachments[1].texture = r1; rp.colorAttachments[1].loadAction = MTLLoadActionClear;
                    rp.colorAttachments[1].clearColor = MTLClearColorMake(0.5, 0.5, 0.5, 0.5); rp.colorAttachments[1].storeAction = MTLStoreActionStore;
                    rp.depthAttachment.texture = zt; rp.depthAttachment.loadAction = MTLLoadActionClear;
                    rp.depthAttachment.clearDepth = 1.0; rp.depthAttachment.storeAction = MTLStoreActionStore;
                } else {
                    rp.colorAttachments[0].texture = r1; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
                    rp.colorAttachments[0].clearColor = MTLClearColorMake(0.5, 0.5, 0.5, 0.5); rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                }
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
                [enc setRenderPipelineState:ps];
                if (t == 0) {
                    MTLDepthStencilDescriptor *sd = [[MTLDepthStencilDescriptor new] autorelease];
                    sd.depthCompareFunction = MTLCompareFunctionLess; sd.depthWriteEnabled = YES;
                    [enc setDepthStencilState:[[d newDepthStencilStateWithDescriptor:sd] autorelease]];
                }
                [enc setVertexBytes:pos length:sizeof(pos) atIndex:0];
                [enc setVertexBytes:col length:sizeof(col) atIndex:1];
                [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [enc endEncoding];
                [cb commit];
                [cb waitUntilCompleted];
                uint32_t p0[64 * 64]; uint16_t p1[64 * 64 * 4]; float zf[64 * 64];
                [r0 getBytes:p0 bytesPerRow:256 fromRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0];
                [hrt getBytes:p1 bytesPerRow:512 fromRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0];
                [zt getBytes:zf bytesPerRow:256 fromRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0];
                unsigned want = 0, good = 0, bad = 0, bx = 0, by = 0; char why[160] = "";
                for (int y = 0; y < 64; y++)
                    for (int x = 0; x < 64; x++) {
                        double qx = x + 0.5, qy = y + 0.5, l[3], dmin = 1e9;
                        for (int i = 0; i < 3; i++) {
                            int a = (i + 1) % 3, b = (i + 2) % 3;
                            double ee = (sx[b] - sx[a]) * (qy - sy[a]) - (sy[b] - sy[a]) * (qx - sx[a]);
                            l[i] = ee / area;
                            double len = sqrt((sx[b] - sx[a]) * (sx[b] - sx[a]) + (sy[b] - sy[a]) * (sy[b] - sy[a]));
                            double dd = l[i] * fabs(area) / len;
                            if (dd < dmin) dmin = dd;
                        }
                        if (fabs(dmin) <= 0.05) continue;
                        bool in = dmin > 0;
                        double q3[3], qs = 0, pc[4] = { 0 };
                        for (int i = 0; i < 3; i++) { q3[i] = l[i] / w[i]; qs += q3[i]; }
                        for (int i = 0; i < 3; i++) for (int k = 0; k < 4; k++) pc[k] += q3[i] / qs * kTriB[i][4 + k];
                        double h[4]; for (int k = 0; k < 4; k++) h[k] = h2f(p1[(y * 64 + x) * 4 + k]);
                        bool ok = true;
                        if (t == 0) {
                            double c0[4] = { 0, 0, 0, 1 }, e1[4] = { 0.5, 0.5, 0.5, 0.5 }, ez = 1.0;
                            if (in) {
                                for (int k = 0; k < 4; k++) c0[k] = pc[k];
                                e1[0] = qx / 256; e1[1] = qy / 256; e1[2] = 0; e1[3] = 1;      // a back face: front_facing false
                                ez = 0.25;                                                     // [[position]].z (0.5) * 0.5
                            }
                            uint32_t ex = rgba8(c0), got = p0[y * 64 + x];
                            for (int k = 0; k < 4; k++) {
                                if (abs((int)((got >> (8 * k)) & 0xFF) - (int)((ex >> (8 * k)) & 0xFF)) > 2) ok = false;
                                if (fabs(h[k] - e1[k]) > 0.002 + fabs(e1[k]) * 0.002) ok = false;
                            }
                            if (fabs(zf[y * 64 + x] - ez) > 1e-4) ok = false;
                            if (!ok && !bad) snprintf(why, sizeof(why), "rt0 %08x want %08x, rt1 %.4f %.4f %.4f %.4f want %.4f %.4f %.4f %.4f, depth %g want %g",
                                                      got, ex, h[0], h[1], h[2], h[3], e1[0], e1[1], e1[2], e1[3], zf[y * 64 + x], ez);
                        } else {
                            double e1[4] = { 0.5, 0.5, 0.5, 0.5 };
                            if (in) for (int k = 0; k < 4; k++) e1[k] = pc[k] * 0.5;
                            for (int k = 0; k < 4; k++) if (fabs(h[k] - e1[k]) > 0.002) ok = false;
                            if (!ok && !bad) snprintf(why, sizeof(why), "%.4f %.4f %.4f %.4f want %.4f %.4f %.4f %.4f", h[0], h[1], h[2], h[3], e1[0], e1[1], e1[2], e1[3]);
                        }
                        if (in) want++;
                        if (ok) { if (in) good++; } else { if (!bad) { bx = x; by = y; } bad++; }
                    }
                bool ok = !bad && good == want && cb.status == MTLCommandBufferStatusCompleted;
                printf("      %s: %u/%u inside right, %u wrong", t == 0 ? "RGBA8 + RGBA16Float + Depth32Float, front_facing, [[position]].z -> [[depth]]"
                                                                        : "half4 output into RGBA16Float", good, want, bad);
                if (bad) printf(" (first (%u,%u) %s)", bx, by, why);
                printf(" -> %s\n", ok ? "MRT WORKS" : "FAILED");
                if (!ok) fails++;
            }
        }
        printf("    render target test: %s\n", fails ? "FAILED" : "ALL OK");
        return fails ? 1 : 0;
    }
}

// ---- [18f] v0.36.0: texture blits, mipmaps, more pixel formats ----
static double srgbEnc(double c) { c = c < 0 ? 0 : c > 1 ? 1 : c; return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1 / 2.4) - 0.055; }
static int pixRun(id<MTLDevice> d, id<MTLCommandQueue> q, id<MTLRenderPipelineState> ps, id<MTLTexture> rt, NSUInteger level,
                  MTLClearColor cc, const float (*pos)[4], const float (*col)[4], id<MTLTexture> tex, id<MTLSamplerState> smp,
                  const MTLScissorRect *sc) {
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = rt; rp.colorAttachments[0].level = level;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].clearColor = cc;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    if (ps && pos) {
        [enc setRenderPipelineState:ps];
        [enc setVertexBytes:pos length:3 * 16 atIndex:0];
        [enc setVertexBytes:col length:3 * 16 atIndex:1];
        if (tex) { [enc setFragmentTexture:tex atIndex:0]; [enc setFragmentSamplerState:smp atIndex:0]; }
        if (sc) [enc setScissorRect:*sc];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    }
    [enc endEncoding];
    [cb commit]; [cb waitUntilCompleted];
    return cb.status == MTLCommandBufferStatusCompleted ? 0 : 1;
}
static id<MTLRenderPipelineState> rvPipe(id<MTLDevice> d, id<MTLLibrary> lib, const char *fs, MTLPixelFormat pf) {
    MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor new] autorelease];
    pd.vertexFunction = [[lib newFunctionWithName:@"rv_basic"] autorelease];
    pd.fragmentFunction = [[lib newFunctionWithName:@(fs)] autorelease];
    pd.colorAttachments[0].pixelFormat = pf;
    NSError *e = nil;
    id<MTLRenderPipelineState> ps = (!pd.vertexFunction || !pd.fragmentFunction) ? nil : [d newRenderPipelineStateWithDescriptor:pd error:&e];
    if (!ps) {
        NSString *log = ((id (*)(id, SEL))objc_msgSend)((id)d, sel_registerName("irisxeLastRenderLog"));
        printf("      pipeline rv_basic + %s: %s\n", fs, e ? e.localizedDescription.UTF8String : log.UTF8String);
    }
    return [ps autorelease];
}
static int texMoreTest(void) {
    @autoreleasepool {
        id<MTLDevice> d = ourDevice();
        if (!d) { printf("    no IrisXe device\n"); return 2; }
        id<MTLLibrary> lib = testLibrary(d);
        if (!lib) return 3;
        id<MTLCommandQueue> q = [[d newCommandQueue] autorelease];
        int fails = 0;
        static const float bigPos[3][4] = { { -1, -1, 0.5f, 1 }, { 3, -1, 0.5f, 1 }, { -1, 3, 0.5f, 1 } };
        float white[3][4], red[3][4];
        for (int i = 0; i < 3; i++) for (int k = 0; k < 4; k++) { white[i][k] = 1; red[i][k] = k == 0 || k == 3 ? 1 : 0; }

        // 1: blits: buffer -> level 2, level 2 (part) -> another texture, that texture -> buffer (rows 160 bytes apart)
        if (!gpuHung()) @autoreleasepool {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:YES];
            td.storageMode = MTLStorageModeShared; td.usage = MTLTextureUsageShaderRead;
            id<MTLTexture> a = [[d newTextureWithDescriptor:td] autorelease];
            MTLTextureDescriptor *tb = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:32 height:32 mipmapped:NO];
            tb.storageMode = MTLStorageModeShared;
            id<MTLTexture> b = [[d newTextureWithDescriptor:tb] autorelease];
            MTLTextureDescriptor *t8 = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm width:13 height:7 mipmapped:NO];
            t8.storageMode = MTLStorageModeShared;
            id<MTLTexture> c8 = [[d newTextureWithDescriptor:t8] autorelease];
            uint32_t pat[16 * 16]; uint8_t p8[13 * 7];
            for (int i = 0; i < 256; i++) pat[i] = 0xFF000000u | (uint32_t)(i * 2654435761u >> 8);
            for (int i = 0; i < 91; i++) p8[i] = (uint8_t)(i * 37 + 5);
            id<MTLBuffer> src = [[d newBufferWithBytes:pat length:sizeof(pat) options:MTLResourceStorageModeShared] autorelease];
            id<MTLBuffer> src8 = [[d newBufferWithBytes:p8 length:sizeof(p8) options:MTLResourceStorageModeShared] autorelease];
            id<MTLBuffer> out = [[d newBufferWithLength:160 * 32 options:MTLResourceStorageModeShared] autorelease];
            memset(out.contents, 0x55, 160 * 32);
            printf("    levels %lu (64x64 RGBA8, mipmapped)\n", (unsigned long)a.mipmapLevelCount);
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
            [be copyFromBuffer:src sourceOffset:0 sourceBytesPerRow:64 sourceBytesPerImage:0 sourceSize:MTLSizeMake(16, 16, 1)
                     toTexture:a destinationSlice:0 destinationLevel:2 destinationOrigin:MTLOriginMake(0, 0, 0)];
            [be copyFromTexture:a sourceSlice:0 sourceLevel:2 sourceOrigin:MTLOriginMake(4, 4, 0) sourceSize:MTLSizeMake(8, 8, 1)
                      toTexture:b destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(10, 12, 0)];
            [be copyFromTexture:b sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(32, 32, 1)
                       toBuffer:out destinationOffset:0 destinationBytesPerRow:160 destinationBytesPerImage:0];
            [be copyFromBuffer:src8 sourceOffset:0 sourceBytesPerRow:13 sourceBytesPerImage:0 sourceSize:MTLSizeMake(13, 7, 1)
                     toTexture:c8 destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
            [be endEncoding];
            [cb commit]; [cb waitUntilCompleted];
            uint32_t l2[16 * 16]; uint8_t g8[13 * 7];
            [a getBytes:l2 bytesPerRow:64 fromRegion:MTLRegionMake2D(0, 0, 16, 16) mipmapLevel:2];
            [c8 getBytes:g8 bytesPerRow:13 fromRegion:MTLRegionMake2D(0, 0, 13, 7) mipmapLevel:0];
            unsigned bad1 = 0, bad2 = 0, bad3 = 0, bad0 = 0;
            for (int i = 0; i < 256; i++) if (l2[i] != pat[i]) bad1++;
            const uint8_t *o = (const uint8_t *)out.contents;
            for (int y = 0; y < 32; y++)
                for (int x = 0; x < 40; x++) {
                    uint32_t v; memcpy(&v, o + y * 160 + x * 4, 4);
                    uint32_t want = x >= 32 ? 0x55555555u : (x >= 10 && x < 18 && y >= 12 && y < 20) ? pat[(y - 12 + 4) * 16 + (x - 10 + 4)] : 0;
                    if (v != want) bad2++;
                }
            for (int i = 0; i < 91; i++) if (g8[i] != p8[i]) bad3++;
            uint32_t l0[64 * 64];                                        // level 0 untouched
            [a getBytes:l0 bytesPerRow:256 fromRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0];
            for (int i = 0; i < 64 * 64; i++) if (l0[i]) bad0++;
            bool ok = !bad1 && !bad2 && !bad3 && !bad0 && cb.status == MTLCommandBufferStatusCompleted;
            printf("    blits buffer -> level 2, level 2 -> texture, texture -> buffer, 8 bpp odd width: wrong %u, %u, %u (level 0 touched %u) -> %s\n",
                   bad1, bad2, bad3, bad0, ok ? "BLITS WORK" : "FAILED");
            if (!ok) fails++;
        } else { printf("    blits SKIPPED: the GPU is hung\n"); fails++; }

        // 2: a mipmapped texture sampled in a fragment function: 256x256 drawn over 64 pixels -> level 2
        if (!gpuHung()) @autoreleasepool {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:256 height:256 mipmapped:YES];
            td.storageMode = MTLStorageModeShared; td.usage = MTLTextureUsageShaderRead;
            id<MTLTexture> t = [[d newTextureWithDescriptor:td] autorelease];
            uint32_t lc[9];
            static uint32_t buf[256 * 256];
            for (NSUInteger l = 0; l < t.mipmapLevelCount; l++) {
                lc[l] = 0xFF000000u | (uint32_t)(l * 40 + 10) | (uint32_t)(250 - l * 25) << 8 | (uint32_t)((l * 77) & 0xFF) << 16;
                NSUInteger w = 256 >> l;
                for (NSUInteger i = 0; i < w * w; i++) buf[i] = lc[l];
                [t replaceRegion:MTLRegionMake2D(0, 0, w, w) mipmapLevel:l withBytes:buf bytesPerRow:w * 4];
            }
            MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor new] autorelease];
            sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest; sd.mipFilter = MTLSamplerMipFilterNearest;
            id<MTLSamplerState> smp = [[d newSamplerStateWithDescriptor:sd] autorelease];
            id<MTLRenderPipelineState> ps = rvPipe(d, lib, "rf_texture", MTLPixelFormatRGBA8Unorm);
            MTLTextureDescriptor *rd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:NO];
            rd.storageMode = MTLStorageModeShared; rd.usage = MTLTextureUsageRenderTarget;
            id<MTLTexture> rt = [[d newTextureWithDescriptor:rd] autorelease];
            int st = ps ? pixRun(d, q, ps, rt, 0, MTLClearColorMake(0, 0, 0, 0), bigPos, white, t, smp, NULL) : 1;
            uint32_t px[64 * 64]; unsigned good = 0, wrongLevel[9] = { 0 }, other = 0;
            [rt getBytes:px bytesPerRow:256 fromRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0];
            for (int i = 0; i < 64 * 64; i++) {
                if (px[i] == lc[2]) { good++; continue; }
                int l = -1; for (int k = 0; k < 9; k++) if (px[i] == lc[k]) l = k;
                if (l >= 0) wrongLevel[l]++; else other++;
            }
            bool ok = !st && good == 64 * 64;
            printf("    mipmapped 256x256 (9 levels) drawn at 1/4 size: %u/4096 from level 2", good);
            for (int k = 0; k < 9; k++) if (wrongLevel[k]) printf(", %u from level %d", wrongLevel[k], k);
            if (other) printf(", %u other (first %08x)", other, px[0]);
            printf(" -> %s\n", ok ? "MIPMAPS WORK" : "FAILED");
            if (!ok) fails++;
        } else { printf("    mipmap sampling SKIPPED: the GPU is hung\n"); fails++; }

        // 3: drawing into mip level 1 (clear green, red left half through a scissor); levels 0 and 2 untouched
        if (!gpuHung()) @autoreleasepool {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:YES];
            td.storageMode = MTLStorageModeShared; td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            id<MTLTexture> t = [[d newTextureWithDescriptor:td] autorelease];
            id<MTLRenderPipelineState> ps = rvPipe(d, lib, "rf_color", MTLPixelFormatRGBA8Unorm);
            MTLScissorRect sc = { 0, 0, 16, 32 };
            int st = ps ? pixRun(d, q, ps, t, 1, MTLClearColorMake(0, 1, 0, 1), bigPos, red, nil, nil, &sc) : 1;
            uint32_t l1[32 * 32], l0[64 * 64], l2[16 * 16]; unsigned bad = 0, touched = 0;
            [t getBytes:l1 bytesPerRow:128 fromRegion:MTLRegionMake2D(0, 0, 32, 32) mipmapLevel:1];
            [t getBytes:l0 bytesPerRow:256 fromRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0];
            [t getBytes:l2 bytesPerRow:64 fromRegion:MTLRegionMake2D(0, 0, 16, 16) mipmapLevel:2];
            for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) if (l1[y * 32 + x] != (x < 16 ? 0xFF0000FFu : 0xFF00FF00u)) bad++;
            for (int i = 0; i < 64 * 64; i++) if (l0[i]) touched++;
            for (int i = 0; i < 16 * 16; i++) if (l2[i]) touched++;
            bool ok = !st && !bad && !touched;
            printf("    render into level 1 (32x32): %u wrong, %u touched in levels 0 / 2 (first %08x) -> %s\n", bad, touched, l1[0],
                   ok ? "MIPMAPS WORK" : "FAILED");
            if (!ok) fails++;
        } else { printf("    render into a level SKIPPED: the GPU is hung\n"); fails++; }

        // 4, 5: sRGB and 10-bit render targets (clear + triangle)
        for (int f = 0; f < 2; f++) {
            if (gpuHung()) { printf("    format case SKIPPED: the GPU is hung\n"); fails++; continue; }
            @autoreleasepool {
                MTLPixelFormat pf = f == 0 ? MTLPixelFormatBGRA8Unorm_sRGB : MTLPixelFormatRGB10A2Unorm;
                MTLTextureDescriptor *rd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:64 height:64 mipmapped:NO];
                rd.storageMode = MTLStorageModeShared; rd.usage = MTLTextureUsageRenderTarget;
                id<MTLTexture> rt = [[d newTextureWithDescriptor:rd] autorelease];
                id<MTLRenderPipelineState> ps = rvPipe(d, lib, "rf_color", pf);
                float pos[3][4], col[3][4];
                for (int i = 0; i < 3; i++) for (int k = 0; k < 4; k++) { pos[i][k] = kTriB[i][k]; col[i][k] = kTriB[i][4 + k]; }
                int st = (ps && rt) ? pixRun(d, q, ps, rt, 0, MTLClearColorMake(0.5, 0.25, 0.75, 1), pos, col, nil, nil, NULL) : 1;
                uint32_t px[64 * 64];
                [rt getBytes:px bytesPerRow:256 fromRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0];
                // check the cleared corner and the triangle's centre pixel
                double cl[4] = { 0.5, 0.25, 0.75, 1 };
                double sx[3], sy[3], w[3];
                for (int i = 0; i < 3; i++) { w[i] = kTriB[i][3]; sx[i] = (kTriB[i][0] / w[i] + 1.0) * 32.0; sy[i] = (1.0 - kTriB[i][1] / w[i]) * 32.0; }
                double area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
                unsigned bad = 0, n = 0; char why[96] = "";
                for (int y = 0; y < 64; y += 3)
                    for (int x = 0; x < 64; x += 3) {
                        double qx = x + 0.5, qy = y + 0.5, l[3], dmin = 1e9;
                        for (int i = 0; i < 3; i++) {
                            int a2 = (i + 1) % 3, b2 = (i + 2) % 3;
                            double ee = (sx[b2] - sx[a2]) * (qy - sy[a2]) - (sy[b2] - sy[a2]) * (qx - sx[a2]);
                            l[i] = ee / area;
                            double len = sqrt((sx[b2] - sx[a2]) * (sx[b2] - sx[a2]) + (sy[b2] - sy[a2]) * (sy[b2] - sy[a2]));
                            double dd = l[i] * fabs(area) / len; if (dd < dmin) dmin = dd;
                        }
                        if (fabs(dmin) <= 0.05) continue;
                        double c[4];
                        if (dmin > 0) {
                            double q3[3], qs = 0; for (int i = 0; i < 3; i++) { q3[i] = l[i] / w[i]; qs += q3[i]; }
                            for (int k = 0; k < 4; k++) { c[k] = 0; for (int i = 0; i < 3; i++) c[k] += q3[i] / qs * kTriB[i][4 + k]; }
                        } else memcpy(c, cl, sizeof(c));
                        uint32_t v = px[y * 64 + x]; bool ok1 = true;
                        if (f == 0) {                                   // B G R A bytes, colour sRGB-encoded
                            double e[4] = { srgbEnc(c[2]) * 255, srgbEnc(c[1]) * 255, srgbEnc(c[0]) * 255, c[3] * 255 };
                            for (int k = 0; k < 4; k++) if (fabs(((v >> (8 * k)) & 0xFF) - e[k]) > 2.5) ok1 = false;
                        } else {                                        // R 9:0, G 19:10, B 29:20, A 31:30
                            double e[4] = { c[0] * 1023, c[1] * 1023, c[2] * 1023, c[3] * 3 };
                            double g[4] = { (double)(v & 1023), (double)((v >> 10) & 1023), (double)((v >> 20) & 1023), (double)(v >> 30) };
                            for (int k = 0; k < 4; k++) if (fabs(g[k] - e[k]) > (k == 3 ? 0.5 : 4)) ok1 = false;
                        }
                        n++;
                        if (!ok1) { if (!bad) snprintf(why, sizeof(why), " (first (%d,%d) %08x)", x, y, v); bad++; }
                    }
                bool ok = !st && !bad;
                printf("    %s render target, clear + triangle: %u/%u right%s -> %s\n", f == 0 ? "BGRA8Unorm_sRGB" : "RGB10A2Unorm",
                       n - bad, n, why, ok ? "FORMATS WORK" : "FAILED");
                if (!ok) fails++;
            }
        }
        printf("    blit / mipmap / format test: %s\n", fails ? "FAILED" : "ALL OK");
        return fails ? 1 : 0;
    }
}

// ---- [18g] v0.37.0: generateMipmapsForTexture, depth textures in fragment functions ----
static int genMipDepthTest(void) {
    @autoreleasepool {
        id<MTLDevice> d = ourDevice();
        if (!d) { printf("    no IrisXe device\n"); return 2; }
        id<MTLLibrary> lib = testLibrary(d);
        if (!lib) return 3;
        id<MTLCommandQueue> q = [[d newCommandQueue] autorelease];
        int fails = 0;
        static const float bigPos[3][4] = { { -1, -1, 0.5f, 1 }, { 3, -1, 0.5f, 1 }, { -1, 3, 0.5f, 1 } };
        float white[3][4];
        for (int i = 0; i < 3; i++) for (int k = 0; k < 4; k++) white[i][k] = 1;

        // 1: generateMipmaps of a 64x64 RGBA8 (and the same with RGBA8 sRGB): every level against a CPU box filter
        for (int f = 0; f < 2; f++) {
            if (gpuHung()) { printf("    generateMipmaps SKIPPED: the GPU is hung\n"); fails++; continue; }
            @autoreleasepool {
                MTLPixelFormat pf = f ? MTLPixelFormatRGBA8Unorm_sRGB : MTLPixelFormatRGBA8Unorm;
                MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:64 height:64 mipmapped:YES];
                td.storageMode = MTLStorageModeShared; td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
                id<MTLTexture> t = [[d newTextureWithDescriptor:td] autorelease];
                static uint8_t lv[7][64 * 64 * 4];
                for (int y = 0; y < 64; y++)
                    for (int x = 0; x < 64; x++) {
                        uint8_t *px = &lv[0][(y * 64 + x) * 4];
                        px[0] = ((x ^ y) & 1) ? 255 : 0; px[1] = 128; px[2] = (uint8_t)(x * 4); px[3] = (uint8_t)(255 - y * 2);
                    }
                [t replaceRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0 withBytes:lv[0] bytesPerRow:256];
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
                [be generateMipmapsForTexture:t];
                [be endEncoding];
                [cb commit]; [cb waitUntilCompleted];
                // reference: each level from the previous one as stored (8 bits), sRGB averaged in linear light
                unsigned bad = 0, total = 0, worst = 0; int wl = -1, wx = 0, wy = 0;
                for (int l = 1; l < 7; l++) {
                    int w = 64 >> l, pw = w * 2;
                    static uint8_t got[64 * 64 * 4];
                    [t getBytes:got bytesPerRow:w * 4 fromRegion:MTLRegionMake2D(0, 0, w, w) mipmapLevel:l];
                    for (int y = 0; y < w; y++)
                        for (int x = 0; x < w; x++)
                            for (int c = 0; c < 4; c++) {
                                double sum = 0;
                                for (int j = 0; j < 4; j++) {
                                    double v = lv[l - 1][(((2 * y + (j >> 1)) * pw) + 2 * x + (j & 1)) * 4 + c] / 255.0;
                                    if (f && c < 3) v = v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
                                    sum += v / 4;
                                }
                                double e = (f && c < 3) ? srgbEnc(sum) : sum;
                                uint8_t eb = (uint8_t)lround(e * 255);
                                lv[l][(y * w + x) * 4 + c] = got[(y * w + x) * 4 + c];          // next level from what the GPU made
                                unsigned dd = (unsigned)abs((int)got[(y * w + x) * 4 + c] - (int)eb);
                                total++;
                                if (dd > 2) { bad++; if (dd > worst) { worst = dd; wl = l; wx = x; wy = y; } }
                            }
                }
                bool ok = !bad && cb.status == MTLCommandBufferStatusCompleted;
                printf("    generateMipmaps %s 64x64 (levels 1..6): %u/%u channels right", f ? "RGBA8Unorm_sRGB" : "RGBA8Unorm", total - bad, total);
                if (bad) printf(" (worst off by %u at level %d (%d,%d))", worst, wl, wx, wy);
                printf(" -> %s\n", ok ? "GENERATE MIPMAPS WORKS" : "FAILED");
                if (!ok) fails++;
            }
        }

        // 2, 3: a Depth32Float texture (depth = (x + 0.5) / 64) read with sample and with sample_compare (Less, 0.5)
        for (int c = 0; c < 2; c++) {
            if (gpuHung()) { printf("    depth sampling SKIPPED: the GPU is hung\n"); fails++; continue; }
            @autoreleasepool {
                MTLTextureDescriptor *zd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:64 height:64 mipmapped:NO];
                zd.storageMode = MTLStorageModeShared; zd.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
                id<MTLTexture> zt = [[d newTextureWithDescriptor:zd] autorelease];
                static float zv[64 * 64];
                for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) zv[y * 64 + x] = (x + 0.5f) / 64.0f;
                [zt replaceRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0 withBytes:zv bytesPerRow:256];
                MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor new] autorelease];
                sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
                if (c) sd.compareFunction = MTLCompareFunctionLess;
                id<MTLSamplerState> smp = [[d newSamplerStateWithDescriptor:sd] autorelease];
                id<MTLRenderPipelineState> ps = rvPipe(d, lib, c ? "rf_shadow" : "rf_depth_tex", MTLPixelFormatR32Float);
                MTLTextureDescriptor *rd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float width:64 height:64 mipmapped:NO];
                rd.storageMode = MTLStorageModeShared; rd.usage = MTLTextureUsageRenderTarget;
                id<MTLTexture> rt = [[d newTextureWithDescriptor:rd] autorelease];
                int st = ps ? pixRun(d, q, ps, rt, 0, MTLClearColorMake(-1, 0, 0, 1), bigPos, white, zt, smp, NULL) : 1;
                static float out[64 * 64];
                [rt getBytes:out bytesPerRow:256 fromRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0];
                unsigned bad = 0; int bx = 0, by = 0; float bg = 0, bw = 0;
                for (int y = 0; y < 64; y++)
                    for (int x = 0; x < 64; x++) {
                        float want = c ? (0.5f < (x + 0.5f) / 64.0f ? 1.0f : 0.0f) : (x + 0.5f) / 64.0f;
                        float g = out[y * 64 + x];
                        if (fabsf(g - want) > 1e-5f) { if (!bad) { bx = x; by = y; bg = g; bw = want; } bad++; }
                    }
                bool ok = !st && !bad;
                printf("    depth2d %s: %u/4096 right", c ? "sample_compare (Less, 0.5)" : "sample", 4096 - bad);
                if (bad) printf(" (first (%d,%d) %g want %g)", bx, by, bg, bw);
                printf(" -> %s\n", ok ? "DEPTH TEXTURES WORK" : "FAILED");
                if (!ok) fails++;
            }
        }
        // 4 (v0.41.0): a cube texture, every face one colour, sampled along the six axes
        if (gpuHung()) { printf("    cube texture SKIPPED: the GPU is hung\n"); fails++; }
        else @autoreleasepool {
            static const uint32_t faceCol[6] = { 0xff0000ffu, 0xff00ff00u, 0xffff0000u, 0xff00ffffu, 0xffff00ffu, 0xffffff00u };
            MTLTextureDescriptor *cd = [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm size:4 mipmapped:NO];
            cd.storageMode = MTLStorageModeShared; cd.usage = MTLTextureUsageShaderRead;
            id<MTLTexture> ct = [[d newTextureWithDescriptor:cd] autorelease];
            if (!ct) { printf("    cube texture: newTexture -> nil -> FAILED\n"); fails++; }
            else {
                for (int f = 0; f < 6; f++) {
                    uint32_t px[16];
                    for (int i = 0; i < 16; i++) px[i] = faceCol[f];
                    [ct replaceRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0 slice:f withBytes:px bytesPerRow:16 bytesPerImage:64];
                }
                MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor new] autorelease];
                sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
                id<MTLSamplerState> smp = [[d newSamplerStateWithDescriptor:sd] autorelease];
                id<MTLRenderPipelineState> ps = rvPipe(d, lib, "rf_cube", MTLPixelFormatRGBA8Unorm);
                MTLTextureDescriptor *rd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:NO];
                rd.storageMode = MTLStorageModeShared; rd.usage = MTLTextureUsageRenderTarget;
                id<MTLTexture> rt = [[d newTextureWithDescriptor:rd] autorelease];
                int st = ps ? pixRun(d, q, ps, rt, 0, MTLClearColorMake(0, 0, 0, 0), bigPos, white, ct, smp, NULL) : 1;
                static uint32_t out[64 * 64];
                [rt getBytes:out bytesPerRow:256 fromRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0];
                unsigned bad = 0, n = 0; int bx = 0, by = 0; uint32_t bg = 0, bw = 0;
                for (int y = 0; y < 64; y++)
                    for (int x = 0; x < 64; x++) {
                        float fx = (x + 0.5f) / 64.0f * 6.0f;
                        if (fabsf(fx - roundf(fx)) < 0.05f) continue;         // on a strip border: either face
                        int f = (int)fx; if (f > 5) f = 5;
                        n++;
                        if (out[y * 64 + x] != faceCol[f]) { if (!bad) { bx = x; by = y; bg = out[y * 64 + x]; bw = faceCol[f]; } bad++; }
                    }
                bool ok = !st && !bad;
                printf("    texturecube sample (6 faces): %u/%u right", n - bad, n);
                if (bad) printf(" (first (%d,%d) %08x want %08x)", bx, by, bg, bw);
                printf(" -> %s\n", ok ? "CUBE TEXTURES WORK" : "FAILED");
                if (!ok) fails++;
            }
        }
        // 5 (v0.42.0): dfdx / dfdy / fwidth, mix, smoothstep in a fragment function
        if (gpuHung()) { printf("    derivatives SKIPPED: the GPU is hung\n"); fails++; }
        else @autoreleasepool {
            id<MTLRenderPipelineState> ps = rvPipe(d, lib, "rf_deriv", MTLPixelFormatRGBA32Float);
            MTLTextureDescriptor *rd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:64 height:64 mipmapped:NO];
            rd.storageMode = MTLStorageModeShared; rd.usage = MTLTextureUsageRenderTarget;
            id<MTLTexture> rt = [[d newTextureWithDescriptor:rd] autorelease];
            int st = ps ? pixRun(d, q, ps, rt, 0, MTLClearColorMake(9, 9, 9, 9), bigPos, white, nil, nil, NULL) : 1;
            static float out[64 * 64 * 4];
            [rt getBytes:out bytesPerRow:64 * 16 fromRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0];
            static const float want[4] = { 1, -1, 1, 1 };
            unsigned bad = 0; int bx = 0, by = 0, bc = 0; float bg = 0;
            for (int i = 0; i < 64 * 64; i++)
                for (int c = 0; c < 4; c++)
                    if (fabsf(out[i * 4 + c] - want[c]) > 0.02f) { if (!bad) { bx = i % 64; by = i / 64; bc = c; bg = out[i * 4 + c]; } bad++; }
            bool ok = !st && !bad;
            printf("    dfdx / dfdy / fwidth / mix / smoothstep: %u/16384 right", 16384 - bad);
            if (bad) printf(" (first (%d,%d) channel %d = %g want %g)", bx, by, bc, bg, want[bc]);
            printf(" -> %s\n", ok ? "DERIVATIVES WORK" : "FAILED");
            if (!ok) fails++;
        }
        // 6 (v0.42.1): sample(..., level(2)) of a 64x64 texture whose levels are solid colours
        if (gpuHung()) { printf("    sample level SKIPPED: the GPU is hung\n"); fails++; }
        else @autoreleasepool {
            static const uint32_t lvCol[7] = { 0xff0000ffu, 0xff00ff00u, 0xffff0000u, 0xff00ffffu, 0xffff00ffu, 0xffffff00u, 0xffffffffu };
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:YES];
            td.storageMode = MTLStorageModeShared; td.usage = MTLTextureUsageShaderRead;
            id<MTLTexture> t = [[d newTextureWithDescriptor:td] autorelease];
            static uint32_t px[64 * 64];
            for (int l = 0; l < 7; l++) {
                int w = 64 >> l;
                for (int i = 0; i < w * w; i++) px[i] = lvCol[l];
                [t replaceRegion:MTLRegionMake2D(0, 0, w, w) mipmapLevel:l withBytes:px bytesPerRow:w * 4];
            }
            MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor new] autorelease];
            sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest; sd.mipFilter = MTLSamplerMipFilterNearest;
            id<MTLSamplerState> smp = [[d newSamplerStateWithDescriptor:sd] autorelease];
            id<MTLRenderPipelineState> ps = rvPipe(d, lib, "rf_level", MTLPixelFormatRGBA8Unorm);
            MTLTextureDescriptor *rd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:NO];
            rd.storageMode = MTLStorageModeShared; rd.usage = MTLTextureUsageRenderTarget;
            id<MTLTexture> rt = [[d newTextureWithDescriptor:rd] autorelease];
            int st = ps ? pixRun(d, q, ps, rt, 0, MTLClearColorMake(0, 0, 0, 0), bigPos, white, t, smp, NULL) : 1;
            static uint32_t out[64 * 64];
            [rt getBytes:out bytesPerRow:256 fromRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0];
            unsigned bad = 0; uint32_t first = 0;
            for (int i = 0; i < 64 * 64; i++) if (out[i] != lvCol[2]) { if (!bad) first = out[i]; bad++; }
            bool ok = !st && !bad;
            printf("    sample(level(2)): %u/4096 right", 4096 - bad);
            if (bad) printf(" (first wrong %08x, want %08x)", first, lvCol[2]);
            printf(" -> %s\n", ok ? "SAMPLE LEVEL WORKS" : "FAILED");
            if (!ok) fails++;
        }
        printf("    generateMipmaps / depth texture test: %s\n", fails ? "FAILED" : "ALL OK");
        return fails ? 1 : 0;
    }
}

// ---- [18h] v0.38.0: a real window drawn by our GPU: CAMetalLayer, nextDrawable, presentDrawable ----
static int windowTest(void) {
    @autoreleasepool {
        id<MTLDevice> d = ourDevice();
        if (!d) { printf("    no IrisXe device\n"); return 2; }
        id<MTLLibrary> lib = testLibrary(d);
        if (!lib) return 3;
        MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor new] autorelease];
        pd.vertexFunction = [[lib newFunctionWithName:@"r5_vs"] autorelease];
        pd.fragmentFunction = [[lib newFunctionWithName:@"r5_fs"] autorelease];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
        vd.attributes[0].format = MTLVertexFormatFloat4; vd.attributes[0].offset = 0; vd.attributes[0].bufferIndex = 0;
        vd.attributes[1].format = MTLVertexFormatFloat4; vd.attributes[1].offset = 16; vd.attributes[1].bufferIndex = 0;
        vd.layouts[0].stride = 32;
        pd.vertexDescriptor = vd;
        NSError *e = nil;
        id<MTLRenderPipelineState> ps = [[d newRenderPipelineStateWithDescriptor:pd error:&e] autorelease];
        if (!ps) { printf("    pipeline: %s -> FAILED\n", e.localizedDescription.UTF8String); return 1; }
        id<MTLCommandQueue> q = [[d newCommandQueue] autorelease];

        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        NSWindow *win = [[NSWindow alloc] initWithContentRect:NSMakeRect(200, 200, 320, 320)
                                                    styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                      backing:NSBackingStoreBuffered defer:NO];
        win.title = @"IrisXe: drawn by our own GPU driver";
        win.releasedWhenClosed = NO;
        CAMetalLayer *ml = [CAMetalLayer layer];
        ml.device = d;
        ml.pixelFormat = MTLPixelFormatBGRA8Unorm;
        ml.framebufferOnly = NO;
        ml.drawableSize = CGSizeMake(320, 320);
        NSView *v = win.contentView;
        v.layer = ml;                                     // layer-hosting view
        v.wantsLayer = YES;
        [win makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        printf("    window shown: a triangle should spin on a changing background for about 5 seconds\n");
        fflush(stdout);
        unsigned frames = 0, nils = 0, shown = 0;
        bool checked = false, ok = true;
        NSDate *start = [NSDate date];
        for (int f = 0; f < 300; f++) {
            @autoreleasepool {
                NSEvent *ev;
                while ((ev = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate dateWithTimeIntervalSinceNow:0.001]
                                                   inMode:NSDefaultRunLoopMode dequeue:YES])) [NSApp sendEvent:ev];
                id<CAMetalDrawable> dr = [ml nextDrawable];
                if (!dr) {                               // nextDrawable waits up to 1 s each time: give up soon
                    nils++;
                    if (nils >= 5) { printf("    nextDrawable keeps returning nil: stopping\n"); break; }
                    continue;
                }
                id<MTLTexture> t = dr.texture;
                if (f == 0 || !shown) {
                    printf("    drawable %s, texture %s %lux%lu format %lu, IOSurface %s\n", class_getName(object_getClass(dr)),
                           class_getName(object_getClass(t)), (unsigned long)t.width, (unsigned long)t.height,
                           (unsigned long)t.pixelFormat, t.iosurface ? "yes" : "no");
                    shown = 1;
                }
                float a = f * 0.05f, bg = 0.5f + 0.5f * sinf(f * 0.02f);
                float vtx[3][8];
                for (int i = 0; i < 3; i++) {
                    float ang = a + i * 2.0943951f;
                    vtx[i][0] = 0.8f * cosf(ang); vtx[i][1] = 0.8f * sinf(ang); vtx[i][2] = 0.5f; vtx[i][3] = 1;
                    vtx[i][4] = i == 0; vtx[i][5] = i == 1; vtx[i][6] = i == 2; vtx[i][7] = 1;
                }
                MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                rp.colorAttachments[0].texture = t;
                rp.colorAttachments[0].loadAction = MTLLoadActionClear;
                rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                rp.colorAttachments[0].clearColor = MTLClearColorMake(0.1, 0.2 * bg, 0.4 * bg + 0.1, 1);
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
                [enc setRenderPipelineState:ps];
                [enc setVertexBytes:vtx length:sizeof(vtx) atIndex:0];
                [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [enc endEncoding];
                [cb presentDrawable:dr];
                [cb commit];
                if (f == 30 && !checked) {               // the picture must be in the IOSurface after the GPU is done
                    [cb waitUntilCompleted];
                    usleep(100000);
                    checked = true;
                    IOSurfaceRef ios = t.iosurface;
                    if (!ios) { printf("    the drawable's texture has no IOSurface -> FAILED\n"); ok = false; }
                    else {
                        static uint32_t px[320 * 320];
                        [t getBytes:px bytesPerRow:320 * 4 fromRegion:MTLRegionMake2D(0, 0, 320, 320) mipmapLevel:0];
                        IOSurfaceLock(ios, kIOSurfaceLockReadOnly, NULL);
                        const uint8_t *sb = IOSurfaceGetBaseAddress(ios);
                        size_t bpr = IOSurfaceGetBytesPerRow(ios);
                        unsigned diff = 0, centre = 0;
                        for (int y = 0; y < 320 && sb; y++)
                            for (int x = 0; x < 320; x++) {
                                uint32_t sv; memcpy(&sv, sb + y * bpr + x * 4, 4);
                                if (sv != px[y * 320 + x]) diff++;
                            }
                        if (sb) memcpy(&centre, sb + 160 * bpr + 160 * 4, 4);
                        IOSurfaceUnlock(ios, kIOSurfaceLockReadOnly, NULL);
                        bool drawn = px[160 * 320 + 160] != px[2 * 320 + 2];             // triangle at the centre, background at the corner
                        printf("    frame 30: IOSurface %u (%lux%lu, %zu bytes/row): %u pixels differ from the texture; centre %08x corner %08x -> %s\n",
                               IOSurfaceGetID(ios), (unsigned long)IOSurfaceGetWidth(ios), (unsigned long)IOSurfaceGetHeight(ios), bpr,
                               diff, centre, px[2 * 320 + 2], (!diff && drawn) ? "THE PICTURE IS IN THE IOSURFACE" : "FAILED");
                        if (diff || !drawn) ok = false;
                    }
                }
                frames++;
                usleep(8000);
            }
        }
        double secs = -[start timeIntervalSinceNow];
        [win orderOut:nil];
        [win release];
        printf("    %u frames in %.1f s (%.0f per second), %u times no drawable\n", frames, secs, frames / secs, nils);
        if (!frames) ok = false;
        printf("    window test: %s (did you see the spinning triangle?)\n", ok ? "ALL OK" : "FAILED");
        return ok ? 0 : 1;
    }
}

// ---- v0.39.0: irisxe-metaltest --missing: what real apps asked our driver for and didn't get ----
static int missingReport(const char *onlyProg, int minutes) {
    if (minutes <= 0) minutes = 120;
    printf("irisxe-metaltest --missing%s%s: Metal calls our driver didn't handle (last %d minutes)\n\n",
           onlyProg ? " " : "", onlyProg ? onlyProg : "", minutes);
    printf("[a] /tmp/irisxe-missing.log (methods we don't have, per program)\n");
    FILE *f = fopen("/tmp/irisxe-missing.log", "r");
    NSMutableOrderedSet *seen = [NSMutableOrderedSet orderedSet];
    if (f) {
        char line[600];
        while (fgets(line, sizeof(line), f)) {
            char *b = strchr(line, '['), *c = strstr(line, "]: ");      // drop the pid: "prog[123]: x" -> "prog: x"
            NSString *k = (b && c && c > b) ? [NSString stringWithFormat:@"%.*s:%s", (int)(b - line), line, c + 2]
                                            : [NSString stringWithUTF8String:line];
            [seen addObject:[k stringByTrimmingCharactersInSet:[NSCharacterSet newlineCharacterSet]]];
        }
        fclose(f);
        for (NSString *k in seen) printf("    %s\n", k.UTF8String);
        if (!seen.count) printf("    (empty)\n");
    } else printf("    (no file yet: no program has called a missing method, or none could write /tmp)\n");
    printf("\n[b] system log, last 2 hours: driver errors and missing methods (all programs, also sandboxed ones)\n");
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "/usr/bin/log show --last %dm --style compact --predicate 'eventMessage BEGINSWITH \"IrisXeMTL\"' 2>/dev/null", minutes);
    FILE *p = popen(cmd, "r");
    NSMutableOrderedSet *msgs = [NSMutableOrderedSet orderedSet];
    if (p) {
        char line[2048];
        while (fgets(line, sizeof(line), p)) {
            char *m = strstr(line, "IrisXeMTL");
            if (!m) continue;
            // the program: compact style is "date time Ty process[pid:tid] message"
            char prog[128] = "?";
            char *br = strchr(line, '[');
            if (br && br < m) {
                char *sp = br; while (sp > line && sp[-1] != ' ') sp--;
                snprintf(prog, sizeof(prog), "%.*s", (int)(br - sp), sp);
            }
            if (strstr(m, "IOSurface ") && strstr(m, "texture")) continue;                 // routine
            if (onlyProg && strcmp(prog, onlyProg)) continue;                               // v0.42.1: one program
            // v0.41.0: only problems: skip the driver's routine lines
            static const char *routine[] = { "newBuffer length", ": texture ", "render pipeline ok", "not enabled in",
                "newCommandQueue", "compute encoder based", "protocols MTLDevice", "class IrisXeMTLDevice registered",
                "initWithAcceleratorPort", "MTLIOAccelDevice init returned", "compile ", "draw ", "fillBuffer GPU", "copyFromBuffer GPU",
                "dispatch ", "heap of ", "IOSurface " };
            bool skip = false;
            for (size_t r = 0; r < sizeof(routine) / sizeof(routine[0]) && !skip; r++)
                if (strstr(m, routine[r]) && !strstr(m, "fail") && !strstr(m, "FAIL") && !strstr(m, "not supported") &&
                    !strstr(m, " only ") && !strstr(m, "error") && !strstr(m, "expected")) skip = true;
            if (skip) continue;
            NSString *k = [NSString stringWithFormat:@"%s: %@", prog,
                           [[NSString stringWithUTF8String:m] stringByTrimmingCharactersInSet:[NSCharacterSet newlineCharacterSet]]];
            [msgs addObject:k];
        }
        pclose(p);
    }
    unsigned n = 0;
    for (NSString *k in msgs) { if (n++ < 300) printf("    %s\n", k.UTF8String); }
    if (!msgs.count) printf("    (nothing)\n");
    else if (n > 300) printf("    ... %u more\n", n - 300);
    printf("\nSend this whole output. To start a new list: rm /tmp/irisxe-missing.log\n");
    return 0;
}

static int runChild(const char *self, const char *mode, const char *arg) {
    char *cargv[] = { (char *)self, (char *)mode, (char *)arg, NULL };
    pid_t pid = 0; int st = 0;
    if (gpuHung()) {
        printf("    SKIPPED: the GPU is hung (an earlier test hung it) - reboot, then run again\n");
        return -3;
    }
    fflush(stdout);
    if (posix_spawn(&pid, self, NULL, NULL, cargv, environ) != 0) { perror("posix_spawn"); return -1; }
    for (int t = 0; t < 300; t++) {                       // up to 30 s
        if (waitpid(pid, &st, WNOHANG) == pid) goto done;
        usleep(100000);
    }
    printf("    STILL RUNNING after 30 s (pid %d stuck) - not waiting any longer\n", pid);
    return -2;
done:
    if (WIFSIGNALED(st)) { printf("    CRASHED with signal %d (%s) - send the crash report too\n", WTERMSIG(st), strsignal(WTERMSIG(st))); return -1; }
    return WEXITSTATUS(st);
}

// --probe: try featureProfile values, each in its own process
static int probe(const char *self) {
    printf("probing featureProfile values (each try is a separate process)\n");
    int cand[200], n = 0;
    for (int k = 1; k <= 80; k++) cand[n++] = k;
    for (int b = 100; b <= 3000; b += (b < 1000 ? 900 : 1000))
        for (int k = 0; k <= 12; k++) cand[n++] = b + k;          // 100.., 1000.., 2000.., 3000..
    for (int i = 0; i < n; i++) {
        int k = cand[i];
        char v[16]; snprintf(v, sizeof(v), "%d", k);
        setenv("IRISXE_FP", v, 1);
        setenv("IRISXE_QUIET", "1", 1);
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);   // hide Metal's assert text
        char *cargv[] = { (char *)self, (char *)"--list", NULL };
        pid_t pid = 0; int st = 0;
        fflush(stdout);
        if (posix_spawn(&pid, self, &fa, NULL, cargv, environ) != 0) { perror("posix_spawn"); return 1; }
        waitpid(pid, &st, 0);
        posix_spawn_file_actions_destroy(&fa);
        if (WIFSIGNALED(st)) printf("    fp=%-3d refused (%s)\n", k, strsignal(WTERMSIG(st)));
    }
    return 0;
}


// v0.51.14: what Metal Performance Shaders needs (WindowServer crashed in MPSAutoCache: Apple's
// -[MPSCommandBufferImageCache initWithCommandBuffer:] returned nil). Checks each condition that method
// tests, then (in a child process, so a crash only ends the child) runs an MPS Gaussian blur.
#include <dlfcn.h>
static void classChain(const char *what, id o) {
    printf("    %s: ", what);
    for (Class c = o ? object_getClass(o) : Nil; c; c = class_getSuperclass(c)) printf("%s%s", class_getName(c), class_getSuperclass(c) ? " <- " : "");
    printf("\n");
}
static int mpsBlurChild(void) {
    @autoreleasepool {
        id<MTLDevice> d = ourDevice();
        if (!d) { printf("    no IrisXe device\n"); return 2; }
        dlopen("/System/Library/Frameworks/MetalPerformanceShaders.framework/MetalPerformanceShaders", RTLD_NOW);
        Class blurC = NSClassFromString(@"MPSImageGaussianBlur");
        if (!blurC) { printf("    MPSImageGaussianBlur class not found\n"); return 3; }
        MTLTextureDescriptor *sd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:256 height:128 mipmapped:NO];
        sd.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget;
        id<MTLTexture> src = [d newTextureWithDescriptor:sd], dst = [d newTextureWithDescriptor:sd];
        printf("    textures: %s / %s\n", src ? "ok" : "nil", dst ? "ok" : "nil"); fflush(stdout);
        if (!src || !dst) return 4;
        uint32_t px[256 * 128];
        for (int i = 0; i < 256 * 128; i++) px[i] = (i % 256) < 128 ? 0xff0000ffu : 0xffff0000u;
        [src replaceRegion:MTLRegionMake2D(0, 0, 256, 128) mipmapLevel:0 withBytes:px bytesPerRow:256 * 4];
        id blur = ((id (*)(id, SEL, id, float))objc_msgSend)([blurC alloc], sel_registerName("initWithDevice:sigma:"), d, 4.0f);
        printf("    MPSImageGaussianBlur: %s\n", blur ? "created" : "nil"); fflush(stdout);
        if (!blur) return 5;
        id<MTLCommandQueue> q = [d newCommandQueue];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        printf("    encoding the blur (WindowServer crashed here)...\n"); fflush(stdout);
        ((void (*)(id, SEL, id, id, id))objc_msgSend)(blur, sel_registerName("encodeToCommandBuffer:sourceTexture:destinationTexture:"), cb, src, dst);
        printf("    encoded; committing\n"); fflush(stdout);
        [cb commit];
        [cb waitUntilCompleted];
        printf("    command buffer: status %s%s%s\n", cbStatus(cb.status), cb.error ? ", error: " : "", cb.error ? cb.error.localizedDescription.UTF8String : "");
        [dst getBytes:px bytesPerRow:256 * 4 fromRegion:MTLRegionMake2D(0, 0, 256, 128) mipmapLevel:0];
        printf("    blurred pixels at row 64, x = 100, 126, 128, 130, 156: %08x %08x %08x %08x %08x (edge should be a mix)\n",
               px[64 * 256 + 100], px[64 * 256 + 126], px[64 * 256 + 128], px[64 * 256 + 130], px[64 * 256 + 156]);
        return cb.status == MTLCommandBufferStatusCompleted ? 0 : 1;
    }
}
static int mpsTest(const char *self) {
    @autoreleasepool {
        id<MTLDevice> d = ourDevice();
        if (!d) { printf("    no IrisXe device\n"); return 2; }
        printf("    device name: \"%s\" (contains \"Intel\": %s)\n", d.name.UTF8String, [d.name containsString:@"Intel"] ? "yes" : "no");
        classChain("device class", d);
        id<MTLCommandQueue> q = [d newCommandQueue];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        classChain("command buffer class", cb);
        printf("    conformsToProtocol MTLCommandBuffer: %s\n", [(id)cb conformsToProtocol:@protocol(MTLCommandBuffer)] ? "YES" : "NO  <- MPS refuses this");
        id cbd = [cb device];
        printf("    [commandBuffer device]: %p (%s), same object as the device: %s\n", cbd, cbd ? class_getName([cbd class]) : "nil", cbd == (id)d ? "yes" : "NO");
        id ud = ((id (*)(id, SEL))objc_msgSend)(cb, sel_registerName("userDictionary"));
        printf("    userDictionary: %s\n", ud ? class_getName([ud class]) : "nil");
        printf("    retainedReferences: %d\n", (int)cb.retainedReferences);
        void *mps = dlopen("/System/Library/Frameworks/MetalPerformanceShaders.framework/MetalPerformanceShaders", RTLD_NOW);
        BOOL (*sup)(id) = mps ? (BOOL (*)(id))dlsym(mps, "MPSSupportsMTLDevice") : NULL;
        printf("    MPSSupportsMTLDevice(device): %s\n", sup ? (sup(d) ? "YES" : "NO  <- MPS has no per-GPU data") : "(not found)");
        if (cbd && cbd != (id)d && sup) printf("    MPSSupportsMTLDevice([commandBuffer device]): %s\n", sup(cbd) ? "YES" : "NO");
        Class cc = NSClassFromString(@"MPSCommandBufferImageCache");
        id cache = cc ? ((id (*)(id, SEL, id))objc_msgSend)([cc alloc], sel_registerName("initWithCommandBuffer:"), cb) : nil;
        printf("    -[MPSCommandBufferImageCache initWithCommandBuffer:]: %s\n", cc ? (cache ? "an object (OK)" : "nil  <- the WindowServer crash") : "(class not found)");
        fflush(stdout);
        printf("  MPS blur (child process):\n"); fflush(stdout);
        pid_t pid; int st = 0;
        char *cargv[] = { (char *)self, (char *)"--mpsblur", NULL };
        if (posix_spawn(&pid, self, NULL, NULL, cargv, environ) != 0) { perror("posix_spawn"); return 1; }
        waitpid(pid, &st, 0);
        if (WIFSIGNALED(st)) printf("    child CRASHED (signal %d) - like WindowServer\n", WTERMSIG(st));
        else printf("    child exit code %d %s\n", WEXITSTATUS(st), WEXITSTATUS(st) == 0 ? "(blur OK)" : "");
        return cache && WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : 1;
    }
}


// ---- v0.51.26: --surface - what WindowServer does with an app's window, in two processes ----
// A painter process (an "app") makes an IOSurface and paints it with the CPU; the reader (WindowServer) gets the
// same surface, makes a Metal texture on it and reads it with the GPU (a blit to a buffer). Then the painter paints
// it again and the reader reads again. Run twice: our usual way (the texture uses the surface's pages, "in place")
// and copy mode (IRISXE_IOSCOPY=1).
#define SURF_W 256
#define SURF_H 64
static const uint32_t kColorA = 0xff2050e0u, kColorB = 0xff10c040u;
static void surfPaint(IOSurfaceRef s, uint32_t c) {
    IOSurfaceLock(s, 0, NULL);
    uint8_t *b = IOSurfaceGetBaseAddress(s); size_t bpr = IOSurfaceGetBytesPerRow(s);
    for (int y = 0; y < SURF_H; y++) for (int x = 0; x < SURF_W; x++) ((uint32_t *)(b + y * bpr))[x] = c;
    IOSurfaceUnlock(s, 0, NULL);
}
static int surfPainter(void) {                      // --surfpaint: stdin/stdout are pipes to the reader
    @autoreleasepool {
        NSDictionary *p = @{ (id)kIOSurfaceWidth: @SURF_W, (id)kIOSurfaceHeight: @SURF_H, (id)kIOSurfaceBytesPerElement: @4,
                             (id)kIOSurfacePixelFormat: @((uint32_t)'BGRA'),
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
                             (id)kIOSurfaceIsGlobal: @YES };
#pragma clang diagnostic pop
        IOSurfaceRef s = IOSurfaceCreate((__bridge CFDictionaryRef)p);
        if (!s) { printf("ERR no surface\n"); fflush(stdout); return 2; }
        surfPaint(s, kColorA);
        printf("ID %u %zu\n", IOSurfaceGetID(s), IOSurfaceGetBytesPerRow(s)); fflush(stdout);
        char line[64];
        while (fgets(line, sizeof line, stdin)) {
            if (line[0] == 'B') { surfPaint(s, kColorB); printf("OK\n"); fflush(stdout); }
            if (line[0] == 'Q') break;
        }
        CFRelease(s);
        return 0;
    }
}
static uint32_t gpuReadPixel(id<MTLDevice> d, id<MTLCommandQueue> q, id<MTLTexture> t, int x, int y, int *err) {
    id<MTLBuffer> out = [d newBufferWithLength:SURF_W * 4 * SURF_H options:MTLResourceStorageModeShared];
    memset(out.contents, 0x5a, out.length);
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
    [e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(SURF_W, SURF_H, 1)
              toBuffer:out destinationOffset:0 destinationBytesPerRow:SURF_W * 4 destinationBytesPerImage:SURF_W * 4 * SURF_H];
    [e endEncoding];
    *err = commitWait(cb);
    uint32_t v = ((uint32_t *)out.contents)[y * SURF_W + x];
    [out release];
    return v;
}
static uint32_t cpuReadPixel(IOSurfaceRef s, int x, int y) {
    IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL);
    uint32_t v = ((uint32_t *)((uint8_t *)IOSurfaceGetBaseAddress(s) + y * IOSurfaceGetBytesPerRow(s)))[x];
    IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
    return v;
}
static const char *colorName(uint32_t v) {
    return v == kColorA ? "picture 1" : v == kColorB ? "picture 2" : v == 0 ? "ZERO (black)" : v == 0x5a5a5a5au ? "NOT WRITTEN" : "OTHER";
}
static int surfReader(const char *self) {           // --surfread: the WindowServer side
    @autoreleasepool {
        id<MTLDevice> d = ourDevice();
        if (!d) { printf("    no IrisXe device\n"); return 2; }
        id<MTLCommandQueue> q = [d newCommandQueue];
        int toC[2], fromC[2];
        if (pipe(toC) || pipe(fromC)) { perror("pipe"); return 2; }
        posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, toC[0], 0); posix_spawn_file_actions_adddup2(&fa, fromC[1], 1);
        posix_spawn_file_actions_addclose(&fa, toC[1]); posix_spawn_file_actions_addclose(&fa, fromC[0]);
        char *cargv[] = { (char *)self, (char *)"--surfpaint", NULL };
        pid_t pid = 0;
        if (posix_spawn(&pid, self, &fa, NULL, cargv, environ) != 0) { perror("posix_spawn"); return 2; }
        posix_spawn_file_actions_destroy(&fa);
        close(toC[0]); close(fromC[1]);
        FILE *in = fdopen(fromC[0], "r"), *out = fdopen(toC[1], "w");
        char line[128] = {0};
        unsigned sid = 0; size_t bpr = 0;
        if (!fgets(line, sizeof line, in) || sscanf(line, "ID %u %zu", &sid, &bpr) != 2) { printf("    painter: %s\n", line); return 2; }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        IOSurfaceRef s = IOSurfaceLookup(sid);
#pragma clang diagnostic pop
        if (!s) { printf("    IOSurfaceLookup(%u) failed\n", sid); return 2; }
        printf("    the app's surface %u: %dx%d, %zu bytes per row\n", sid, SURF_W, SURF_H, bpr);
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                     width:SURF_W height:SURF_H mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead; td.storageMode = MTLStorageModeShared;
        int fails = 0, e = 0;
        uint32_t v;
        v = cpuReadPixel(s, 10, 10);
        printf("    1. CPU reads the surface after the app painted picture 1: %08x = %s\n", v, colorName(v));
        id<MTLTexture> t = [d newTextureWithDescriptor:td iosurface:s plane:0];
        if (!t) { printf("    no texture on the surface\n"); return 2; }
        v = gpuReadPixel(d, q, t, 10, 10, &e);
        printf("    2. GPU reads it through a new texture:                  %08x = %s -> %s\n", v, colorName(v), v == kColorA && !e ? "OK" : "WRONG");
        fails += !(v == kColorA && !e);
        fprintf(out, "B\n"); fflush(out);
        if (!fgets(line, sizeof line, in)) { printf("    painter gone\n"); return 2; }
        v = cpuReadPixel(s, 10, 10);
        printf("    3. CPU reads it after the app painted picture 2:       %08x = %s\n", v, colorName(v));
        v = gpuReadPixel(d, q, t, 10, 10, &e);
        printf("    4. GPU reads it through the SAME texture:               %08x = %s -> %s\n", v, colorName(v), v == kColorB && !e ? "OK" : "WRONG");
        fails += !(v == kColorB && !e);
        id<MTLTexture> t2 = [d newTextureWithDescriptor:td iosurface:s plane:0];
        v = t2 ? gpuReadPixel(d, q, t2, 10, 10, &e) : 0;
        printf("    5. GPU reads it through a NEW texture:                  %08x = %s -> %s\n", v, colorName(v), v == kColorB && !e ? "OK" : "WRONG");
        fails += !(v == kColorB && !e);
        fprintf(out, "Q\n"); fflush(out);
        int st = 0; waitpid(pid, &st, 0);
        [t release]; [t2 release]; CFRelease(s);
        return fails ? 1 : 0;
    }
}
static int surfaceTest(const char *self) {
    printf("--surface: an app paints its window with the CPU, WindowServer reads it with our GPU driver\n\n");
    printf("[A] our usual way (the texture uses the surface's own memory)\n");
    int a = runChild(self, "--surfread", "");
    printf("    -> %s\n\n", a == 0 ? "PASS" : "FAIL");
    printf("[B] copy mode (IRISXE_IOSCOPY=1: the texture is a copy, refreshed before each read)\n");
    setenv("IRISXE_IOSCOPY", "1", 1);
    int b = runChild(self, "--surfread", "");
    unsetenv("IRISXE_IOSCOPY");
    printf("    -> %s\n\n", b == 0 ? "PASS" : "FAIL");
    printf("surface test: usual way %s, copy mode %s\n", a == 0 ? "PASS" : "FAIL", b == 0 ? "PASS" : "FAIL");
    return a || b;
}


// ---- v0.51.27: --carender - Core Animation's own renderer (the one WindowServer composites with) on our GPU ----
// A 512x256 "screen": gray background; bottom row 4 squares: a red layer, a picture (CGImage, blue), a CPU-painted
// IOSurface of this process (green), an IOSurface painted by another process (picture 1 of --surface); top row: white
// text on the gray. Each piece is checked in the read-back picture.
static const char *pixClass(uint32_t v) {                     // BGRA in memory: b = v & 255, g, r, a = v >> 24
    int b = v & 255, g = (v >> 8) & 255, r = (v >> 16) & 255;
    if (r > 200 && g > 200 && b > 200) return "white";
    if (r < 30 && g < 30 && b < 30) return "black";
    if (abs(r - g) < 25 && abs(g - b) < 25) return "gray";
    if (r > g + 60 && r > b + 60) return "red";
    if (g > r + 60 && g > b + 40) return "green";
    if (b > r + 60 && b > g + 40) return "blue";
    return "other";
}
static CGImageRef solidImage(int w, int h, CGFloat r, CGFloat g, CGFloat b) {
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef c = CGBitmapContextCreate(NULL, w, h, 8, w * 4, cs, kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGContextSetRGBFillColor(c, r, g, b, 1); CGContextFillRect(c, CGRectMake(0, 0, w, h));
    CGImageRef im = CGBitmapContextCreateImage(c);
    CGContextRelease(c); CGColorSpaceRelease(cs);
    return im;
}
static int caRenderTest(const char *self) {
    @autoreleasepool {
        id<MTLDevice> d = ourDevice();
        if (!d) { printf("    no IrisXe device\n"); return 2; }
        const int W = 512, H = 256;
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:H mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; td.storageMode = MTLStorageModeShared;
        id<MTLTexture> target = [d newTextureWithDescriptor:td];
        // the other process's surface (as in --surface)
        int toC[2], fromC[2]; pid_t pid = 0; IOSurfaceRef remote = NULL; FILE *pin = NULL, *pout = NULL;
        if (!pipe(toC) && !pipe(fromC)) {
            posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
            posix_spawn_file_actions_adddup2(&fa, toC[0], 0); posix_spawn_file_actions_adddup2(&fa, fromC[1], 1);
            char *cargv[] = { (char *)self, (char *)"--surfpaint", NULL };
            if (posix_spawn(&pid, self, &fa, NULL, cargv, environ) == 0) {
                close(toC[0]); close(fromC[1]);
                pin = fdopen(fromC[0], "r"); pout = fdopen(toC[1], "w");
                char line[128] = {0}; unsigned sid = 0; size_t bpr = 0;
                if (fgets(line, sizeof line, pin) && sscanf(line, "ID %u %zu", &sid, &bpr) == 2) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
                    remote = IOSurfaceLookup(sid);
#pragma clang diagnostic pop
                }
            }
            posix_spawn_file_actions_destroy(&fa);
        }
        // this process's surface, painted green with the CPU
        NSDictionary *sp = @{ (id)kIOSurfaceWidth: @128, (id)kIOSurfaceHeight: @128, (id)kIOSurfaceBytesPerElement: @4,
                              (id)kIOSurfacePixelFormat: @((uint32_t)'BGRA') };
        IOSurfaceRef local = IOSurfaceCreate((__bridge CFDictionaryRef)sp);
        IOSurfaceLock(local, 0, NULL);
        for (int y = 0; y < 128; y++) for (int x = 0; x < 128; x++)
            ((uint32_t *)((uint8_t *)IOSurfaceGetBaseAddress(local) + y * IOSurfaceGetBytesPerRow(local)))[x] = 0xff10c040u;
        IOSurfaceUnlock(local, 0, NULL);

        [CATransaction begin]; [CATransaction setDisableActions:YES];
        CALayer *root = [CALayer layer];
        root.bounds = CGRectMake(0, 0, W, H); root.position = CGPointMake(W / 2, H / 2); root.anchorPoint = CGPointMake(0.5, 0.5);
        CGColorRef gray = CGColorCreateGenericRGB(0.5, 0.5, 0.5, 1), red = CGColorCreateGenericRGB(1, 0, 0, 1), white = CGColorCreateGenericRGB(1, 1, 1, 1);
        root.backgroundColor = gray;
        CALayer *l[4];
        for (int i = 0; i < 4; i++) { l[i] = [CALayer layer]; l[i].frame = CGRectMake(i * 128, 0, 128, 128); [root addSublayer:l[i]]; }
        l[0].backgroundColor = red;
        CGImageRef blue = solidImage(128, 128, 0, 0.2, 1);
        l[1].contents = (__bridge id)blue;
        l[2].contents = (__bridge id)local;
        if (remote) l[3].contents = (__bridge id)remote;
        CATextLayer *tx = [CATextLayer layer];
        tx.frame = CGRectMake(16, 140, 480, 100); tx.string = @"MENU BAR"; tx.fontSize = 72; tx.foregroundColor = white;
        tx.contentsScale = 1;
        [root addSublayer:tx];
        [CATransaction commit];

        CARenderer *r = [CARenderer rendererWithMTLTexture:target options:nil];
        if (!r) { printf("    CARenderer: nil\n"); return 2; }
        // v0.51.28: the renderer only draws committed layer changes: attach, commit and flush, and let the run loop turn
        [CATransaction begin]; [CATransaction setDisableActions:YES];
        r.layer = root; r.bounds = root.bounds;
        [CATransaction commit];
        [CATransaction flush];
        int frames = 0;
        for (int f = 0; f < 4; f++) {                                 // a few frames, as WindowServer does
            [CATransaction begin]; [CATransaction setDisableActions:YES];
            root.backgroundColor = gray;                               // a change every frame
            [root setNeedsDisplay];
            [CATransaction commit];
            [CATransaction flush];
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
            [r beginFrameAtTime:CACurrentMediaTime() timeStamp:NULL];
            CGRect ub = [r updateBounds];
            [r addUpdateRect:root.bounds];
            [r render];
            [r endFrame];
            printf("    frame %d: Core Animation's own update area %.0fx%.0f at %.0f,%.0f\n", f, ub.size.width, ub.size.height,
                   ub.origin.x, ub.origin.y);
            frames++;
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);
        }
        // read the picture back with a blit on our own queue (after a pause: CARenderer uses its own queue)
        usleep(300000);
        id<MTLCommandQueue> q = [d newCommandQueue];
        id<MTLBuffer> out = [d newBufferWithLength:W * H * 4 options:MTLResourceStorageModeShared];
        memset(out.contents, 0x5a, out.length);
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
        [e copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W, H, 1)
                  toBuffer:out destinationOffset:0 destinationBytesPerRow:W * 4 destinationBytesPerImage:W * H * 4];
        [e endEncoding];
        commitWait(cb);
        const uint32_t *px = out.contents;
        // which way up? the squares are in one half, the text in the other: find the squares' row
        int sqRow = !strcmp(pixClass(px[(H - 64) * W + 64]), "red") ? H - 64 : 64, txTop = sqRow == 64 ? 128 : 0;
        const char *want[4] = { "red", "blue", "green", "blue" };
        const char *what[4] = { "colored layer (red)", "picture layer, CGImage (blue)", "window image of this process (green)",
                                "window image of ANOTHER process (blue)" };
        int fails = 0;
        printf("    %d frames rendered by Core Animation, picture is %s\n", frames, sqRow == 64 ? "top-down" : "bottom-up");
        uint32_t bg = px[(txTop + 4) * W + 4];
        printf("    background:                                %08x %-6s -> %s\n", bg, pixClass(bg), !strcmp(pixClass(bg), "gray") ? "OK" : "WRONG");
        fails += !!strcmp(pixClass(bg), "gray");
        for (int i = 0; i < 4; i++) {
            if (i == 3 && !remote) { printf("    %-42s (no surface from the other process)\n", what[i]); fails++; continue; }
            uint32_t v = px[sqRow * W + i * 128 + 64];
            bool ok = !strcmp(pixClass(v), want[i]);
            printf("    %-42s %08x %-6s -> %s\n", what[i], v, pixClass(v), ok ? "OK" : "WRONG");
            fails += !ok;
        }
        int whiteN = 0;
        for (int y = txTop; y < txTop + 128; y++) for (int x = 0; x < W; x++) whiteN += !strcmp(pixClass(px[y * W + x]), "white");
        printf("    text layer (white letters):                %d white pixels -> %s\n", whiteN, whiteN > 500 ? "OK" : "WRONG");
        fails += whiteN <= 500;
        // a small map of the whole picture: one letter per 16x16 block
        printf("    picture (16x16 blocks: R red, G green, B blue, W white, . gray, # black, ? other, ~ not drawn):\n");
        for (int y = 8; y < H; y += 16) {
            printf("      ");
            for (int x = 8; x < W; x += 16) {
                uint32_t v = px[y * W + x]; const char *c = pixClass(v);
                putchar(v == 0x5a5a5a5au ? '~' : !strcmp(c, "red") ? 'R' : !strcmp(c, "green") ? 'G' : !strcmp(c, "blue") ? 'B' :
                        !strcmp(c, "white") ? 'W' : !strcmp(c, "gray") ? '.' : !strcmp(c, "black") ? '#' : '?');
            }
            putchar('\n');
        }
        if (pout) { fprintf(pout, "Q\n"); fflush(pout); }
        if (pid) { int st = 0; waitpid(pid, &st, 0); }
        CGColorRelease(gray); CGColorRelease(red); CGColorRelease(white); CGImageRelease(blue);
        CFRelease(local); if (remote) CFRelease(remote);
        printf("    Core Animation test: %s\n", fails ? "FAILED" : "ALL OK");
        return fails ? 1 : 0;
    }
}

// ---- v0.51.29: --managed - buffers with Managed storage (Core Animation's vertex buffers): does the GPU see what
// the CPU wrote? The CPU fills the buffer (and calls didModifyRange: as Metal asks), the GPU copies it to a Shared
// buffer, the CPU checks the copy.
static int managedTest(void) {
    @autoreleasepool {
        id<MTLDevice> d = ourDevice();
        if (!d) { printf("    no IrisXe device\n"); return 2; }
        id<MTLCommandQueue> q = [d newCommandQueue];
        const NSUInteger len = 65536;
        void *mem = NULL; posix_memalign(&mem, 16384, len);
        uint8_t pattern[len];
        for (NSUInteger k = 0; k < len; k++) pattern[k] = (uint8_t)(k * 13 + 7);
        id<MTLBuffer> bufs[3];
        const char *what[3] = { "newBufferWithLength, Managed", "newBufferWithBytes, Managed", "newBufferWithBytesNoCopy, Managed" };
        bufs[0] = [d newBufferWithLength:len options:MTLResourceStorageModeManaged];
        memcpy(bufs[0].contents, pattern, len); [bufs[0] didModifyRange:NSMakeRange(0, len)];
        bufs[1] = [d newBufferWithBytes:pattern length:len options:MTLResourceStorageModeManaged];
        memcpy(mem, pattern, len);
        bufs[2] = [d newBufferWithBytesNoCopy:mem length:len options:MTLResourceStorageModeManaged deallocator:nil];
        [bufs[2] didModifyRange:NSMakeRange(0, len)];
        int fails = 0;
        for (int i = 0; i < 3; i++) {
            id<MTLBuffer> out = [d newBufferWithLength:len options:MTLResourceStorageModeShared];
            memset(out.contents, 0x5a, len);
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
            [e copyFromBuffer:bufs[i] sourceOffset:0 toBuffer:out destinationOffset:0 size:len];
            [e endEncoding];
            int r = commitWait(cb);
            NSUInteger right = 0, zero = 0;
            const uint8_t *o = out.contents;
            for (NSUInteger k = 0; k < len; k++) { right += o[k] == pattern[k]; zero += o[k] == 0; }
            bool ok = !r && right == len;
            printf("    %-36s storage %lu: GPU copied %lu/%lu right (%lu zero bytes) -> %s\n", what[i],
                   (unsigned long)bufs[i].storageMode, (unsigned long)right, (unsigned long)len, (unsigned long)zero, ok ? "OK" : "WRONG");
            fails += !ok;
            [out release];
        }
        for (int i = 0; i < 3; i++) [bufs[i] release];
        printf("    managed buffer test: %s\n", fails ? "FAILED" : "ALL OK");
        return fails ? 1 : 0;
    }
}

// ---- v0.51.30: --replaycheck FILE - replay a recording (IRISXE_RECORD=1) and say what each draw leaves in its render
// target: recorded data, our driver's draw path only (no Core Animation). Each draw runs as recorded, then without
// blending, then with fresh buffers.
typedef int (*XeReplayFn)(id, const void *, size_t, uint32_t, char *, size_t);
static unsigned xeResets(void);
static int replayCheck(const char *path) {
    @autoreleasepool {
        id<MTLDevice> d = ourDevice();
        if (!d) { printf("    no IrisXe device\n"); return 2; }
        const char *bp = getenv("IRISXE_BUNDLE");
        NSString *exe = [NSString stringWithFormat:@"%s/Contents/MacOS/IrisXeMTLDriver", bp && *bp ? bp : "/Library/GPUBundles/IrisXeMTLDriver.bundle"];
        void *h = dlopen(exe.fileSystemRepresentation, RTLD_LAZY | RTLD_NOLOAD);
        if (!h) h = dlopen(exe.fileSystemRepresentation, RTLD_LAZY | RTLD_LOCAL);
        XeReplayFn fn = h ? (XeReplayFn)dlsym(h, "IrisXeReplayDraw") : NULL;
        if (!fn) { printf("    no IrisXeReplayDraw in %s\n", exe.UTF8String); return 2; }
        NSData *all = [NSData dataWithContentsOfFile:@(path) options:NSDataReadingMappedIfSafe error:NULL];
        if (!all) { printf("    can't read %s\n", path); return 2; }
        bool verbose = getenv("IRISXE_VERBOSE") != NULL;
        const uint8_t *b = all.bytes; size_t len = all.length;
        long nrec = 0, ndraw = 0, nempty = 0, nhang = 0;
        char info[768];
        // v0.51.33: a GPU reset during a command = it hung the GPU; then each variant on its own
        static const struct { uint32_t v; const char *what; } var[] = {
            { 0, "again, the same" }, { 128, "fresh buffers" }, { 64, "no blending" }, { 1, "no clears" },
            { 4, "clears only, no draw" }, { 8, "first triangle only" } };
        for (size_t o = 0; o + 24 <= len && nhang < 4; ) {
            uint32_t magic, total; memcpy(&magic, b + o, 4); memcpy(&total, b + o + 4, 4);
            if (magic != 0x43455258u || !total || o + total > len) break;
            @autoreleasepool {
                info[0] = 0;
                unsigned r0 = xeResets();
                int rc = fn(d, b + o, total, 0, info, sizeof(info));
                bool hang = xeResets() != r0;
                if (rc == 1) { if (verbose) printf("[%ld] compute (skipped)\n", nrec); }
                else if (rc) printf("[%ld] can't replay (%d)\n", nrec, rc);
                else {
                    if (strstr(info, "RT0 ")) { ndraw++; if (strstr(info, "RT0 0 non-zero")) nempty++; }
                    if (verbose || hang) printf("[%ld] %s%s\n", nrec, info, hang ? "  <== GPU HANG" : "");
                    else if (nrec % 1000 == 0) { printf("[%ld] ...\n", nrec); fflush(stdout); }
                    if (hang) {
                        nhang++;
                        for (size_t k = 0; k < sizeof(var) / sizeof(var[0]); k++) {
                            unsigned r1 = xeResets();
                            char i2[768] = "";
                            fn(d, b + o, total, var[k].v, i2, sizeof(i2));
                            printf("      %-22s -> %s\n", var[k].what, xeResets() != r1 ? "HANG" : "ok");
                            fflush(stdout);
                        }
                        // save this one command for offline work
                        char out[512]; snprintf(out, sizeof out, "%s.hang%ld.bin", path, nrec);
                        FILE *f = fopen(out, "wb"); if (f) { fwrite(b + o, 1, total, f); fclose(f); printf("      saved as %s\n", out); }
                    }
                }
            }
            nrec++; o += total;
        }
        printf("\n%ld commands replayed, %ld draws, %ld left render target 0 all zero, %ld hung the GPU%s\n", nrec, ndraw, nempty, nhang,
               nhang >= 4 ? " (stopped after 4)" : "");
        return nhang ? 1 : 0;
    }
}

// ---- v0.51.35: --replaybatch FILE - replay a recording batched like the desktop (up to 16 draws in one GPU job, a blit
// ends the batch). A batch that resets the GPU is narrowed down: each draw alone, then the first k draws batched.
typedef int (*XeFlushFn)(id);
static int replayBatch(const char *path) {
    @autoreleasepool {
        id<MTLDevice> d = ourDevice();
        if (!d) { printf("    no IrisXe device\n"); return 2; }
        const char *bp = getenv("IRISXE_BUNDLE");
        NSString *exe = [NSString stringWithFormat:@"%s/Contents/MacOS/IrisXeMTLDriver", bp && *bp ? bp : "/Library/GPUBundles/IrisXeMTLDriver.bundle"];
        void *h = dlopen(exe.fileSystemRepresentation, RTLD_LAZY | RTLD_NOLOAD);
        if (!h) h = dlopen(exe.fileSystemRepresentation, RTLD_LAZY | RTLD_LOCAL);
        XeReplayFn fn = h ? (XeReplayFn)dlsym(h, "IrisXeReplayDraw") : NULL;
        XeFlushFn flush = h ? (XeFlushFn)dlsym(h, "IrisXeReplayFlush") : NULL;
        if (!fn || !flush) { printf("    this bundle has no batched replay (0.51.35+): %s\n", exe.UTF8String); return 2; }
        NSData *all = [NSData dataWithContentsOfFile:@(path) options:NSDataReadingMappedIfSafe error:NULL];
        if (!all) { printf("    can't read %s\n", path); return 2; }
        const uint8_t *b = all.bytes; size_t len = all.length;
        NSMutableArray *offs = [NSMutableArray array];
        for (size_t o = 0; o + 24 <= len; ) {
            uint32_t magic, total; memcpy(&magic, b + o, 4); memcpy(&total, b + o + 4, 4);
            if (magic != 0x43455258u || !total || o + total > len) break;
            [offs addObject:@(o)]; o += total;
        }
        long n = (long)offs.count, groups = 0, hangs = 0;
        printf("%ld commands in %s\n", n, path);
        #define REC(i) (b + [offs[(i)] unsignedLongValue])
        #define RECLEN(i) ({ uint32_t t_; memcpy(&t_, REC(i) + 4, 4); t_; })
        bool (^isDraw)(long) = ^bool(long i) {
            const uint8_t *r = REC(i); uint32_t nl, type; memcpy(&nl, r + 12, 4); memcpy(&type, r + 24 + nl, 4);
            return type == 0 || type == 0x10004u;
        };
        // v0.51.36: the core counts a hang only after its 1 s job timeout, and a command buffer can report "completed"
        // before that: wait for the timeout before reading the reset count (IRISXE_SETTLE_MS, default 1200)
        const char *se = getenv("IRISXE_SETTLE_MS");
        useconds_t settle = (useconds_t)((se ? atoi(se) : 1200) * 1000);
        bool (^runBatch)(long, long) = ^bool(long from, long cnt) {      // true = the GPU was reset
            unsigned r0 = xeResets();
            for (long i = from; i < from + cnt; i++) fn(d, REC(i), RECLEN(i), 0x100, NULL, 0);
            flush(d);
            usleep(settle);
            return xeResets() != r0;
        };
        printf("(waiting %u ms after each batch for the GPU's hang timeout)\n", (unsigned)(settle / 1000));
        for (long i = 0; i < n && hangs < 3; ) {
            @autoreleasepool {
                if (!isDraw(i)) {                             // v0.51.36: blits are checked too
                    unsigned r0 = xeResets(); char bi[768] = "";
                    fn(d, REC(i), RECLEN(i), 0, bi, sizeof(bi));
                    usleep(settle);
                    if (xeResets() != r0) { hangs++; printf("\n[%ld] a blit HUNG the GPU: %s\n", i, bi); fflush(stdout); }
                    i++; continue;
                }
                long cnt = 0;
                while (i + cnt < n && cnt < 16 && isDraw(i + cnt)) cnt++;
                groups++;
                if (groups % 50 == 0) { printf("[%ld] ... %ld batches\n", i, groups); fflush(stdout); }
                if (runBatch(i, cnt)) {
                    hangs++;
                    printf("\n[%ld..%ld] a batch of %ld draws HUNG the GPU\n", i, i + cnt - 1, cnt);
                    char info[768];
                    for (long k = i; k < i + cnt; k++) {
                        unsigned r1 = xeResets(); info[0] = 0;
                        fn(d, REC(k), RECLEN(k), 0, info, sizeof(info));
                        usleep(settle);
                        printf("    alone [%ld] %s -> %s\n", k, info, xeResets() != r1 ? "HANG" : "ok");
                        fflush(stdout);
                    }
                    long first = -1;
                    for (long k = 2; k <= cnt && first < 0; k++) if (runBatch(i, k)) first = k;
                    if (first > 0) {
                        printf("    batched, the first %ld draws hang (the first %ld don't)\n", first, first - 1);
                        long pairHang = -1;
                        for (long k = i; k + 1 < i + first && pairHang < 0; k++) if (runBatch(k, 2)) pairHang = k;
                        if (pairHang >= 0) printf("    smallest: the pair [%ld]+[%ld] batched hangs\n", pairHang, pairHang + 1);
                        else printf("    no single pair hangs: it needs more of the batch\n");
                    } else printf("    batched again it did NOT hang (timing / other programs?)\n");
                    char out[512]; snprintf(out, sizeof out, "%s.batch%ld.bin", path, i);
                    FILE *f = fopen(out, "wb");
                    if (f) { for (long k = i; k < i + cnt; k++) fwrite(REC(k), 1, RECLEN(k), f); fclose(f); printf("    saved as %s\n", out); }
                    fflush(stdout);
                }
                i += cnt;
            }
        }
        unsigned rEnd = xeResets(); usleep(settle);
        if (xeResets() != rEnd) printf("\n(the GPU was reset AFTER the last command: %u more)\n", xeResets() - rEnd);
        printf("\n%ld commands, %ld batches replayed: %ld hung the GPU%s\n", n, groups, hangs, hangs >= 3 ? " (stopped after 3)" : "");
        return hangs ? 1 : 0;
        #undef REC
        #undef RECLEN
    }
}
// ---- v0.51.32: --camasks - the Core Animation cases WindowServer draws with its alpha-mask image shader
// (TimgXh_Ialp: one-channel textures) - window shadows, rounded corners, masks, gray / alpha-only images, one-channel
// IOSurfaces. Each case runs in its own process; the GPU reset count before / after says whether it hung the GPU.
static unsigned xeResets(void) {
    io_service_t sv = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IrisXeProbe"));
    if (!sv) return 0;
    CFTypeRef v = IORegistryEntryCreateCFProperty(sv, CFSTR("IrisXe-Resets"), kCFAllocatorDefault, 0);
    IOObjectRelease(sv);
    unsigned n = 0;
    if (v && CFGetTypeID(v) == CFStringGetTypeID()) n = (unsigned)atoi([(__bridge NSString *)v UTF8String]);
    if (v) CFRelease(v);
    return n;
}
static CGImageRef maskImage(int w, int h, bool alphaOnly) {     // a soft round blob: gray or alpha-only, 8 bits
    CGColorSpaceRef cs = alphaOnly ? NULL : CGColorSpaceCreateDeviceGray();
    CGContextRef c = CGBitmapContextCreate(NULL, w, h, 8, w, cs, alphaOnly ? (CGBitmapInfo)kCGImageAlphaOnly : (CGBitmapInfo)kCGImageAlphaNone);
    if (!c) { if (cs) CGColorSpaceRelease(cs); return NULL; }
    uint8_t *p = CGBitmapContextGetData(c);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        float dx = x - w / 2.0f, dy = y - h / 2.0f, r = sqrtf(dx * dx + dy * dy) / (w / 2.0f);
        p[y * CGBitmapContextGetBytesPerRow(c) + x] = r < 1 ? (uint8_t)(255 * (1 - r * r)) : 0;
    }
    CGImageRef im = CGBitmapContextCreateImage(c);
    CGContextRelease(c); if (cs) CGColorSpaceRelease(cs);
    return im;
}
static const char *kMaskCase[] = {
    "color picture (control, like the passing test)",
    "gray picture (8-bit gray image)",
    "alpha-only picture (8-bit alpha image)",
    "layer with a shadow (like a window shadow)",
    "rounded corners (cornerRadius + masksToBounds)",
    "mask layer (layer.mask = alpha picture)",
    "one-channel window image (IOSurface L008)",
};
#define N_MASK_CASES 7
static int caMaskChild(int v) {
    @autoreleasepool {
        id<MTLDevice> d = ourDevice();
        if (!d) { printf("      no IrisXe device\n"); return 2; }
        const int W = 256, H = 256;
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:H mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; td.storageMode = MTLStorageModeShared;
        id<MTLTexture> target = [d newTextureWithDescriptor:td];
        CGColorRef gray = CGColorCreateGenericRGB(0.5, 0.5, 0.5, 1), red = CGColorCreateGenericRGB(1, 0, 0, 1), black = CGColorCreateGenericRGB(0, 0, 0, 1);
        CGImageRef color = solidImage(128, 128, 0, 0.2, 1), grayIm = maskImage(128, 128, false), alphaIm = maskImage(128, 128, true);
        IOSurfaceRef one = NULL;
        [CATransaction begin]; [CATransaction setDisableActions:YES];
        CALayer *root = [CALayer layer];
        root.bounds = CGRectMake(0, 0, W, H); root.position = CGPointMake(W / 2, H / 2); root.backgroundColor = gray;
        CALayer *l = [CALayer layer];
        l.frame = CGRectMake(64, 64, 128, 128);
        switch (v) {
        case 0: l.contents = (__bridge id)color; break;
        case 1: l.contents = (__bridge id)grayIm; break;
        case 2: l.contents = (__bridge id)alphaIm; l.backgroundColor = NULL; break;
        case 3: l.backgroundColor = red; l.shadowColor = black; l.shadowOpacity = 1; l.shadowRadius = 10; l.shadowOffset = CGSizeMake(0, -8); break;
        case 4: l.contents = (__bridge id)color; l.cornerRadius = 32; l.masksToBounds = YES; break;
        case 5: { l.backgroundColor = red; CALayer *m = [CALayer layer]; m.frame = l.bounds; m.contents = (__bridge id)alphaIm; l.mask = m; break; }
        case 6: {
            NSDictionary *sp = @{ (id)kIOSurfaceWidth: @128, (id)kIOSurfaceHeight: @128, (id)kIOSurfaceBytesPerElement: @1,
                                  (id)kIOSurfacePixelFormat: @((uint32_t)'L008') };
            one = IOSurfaceCreate((__bridge CFDictionaryRef)sp);
            if (one) {
                IOSurfaceLock(one, 0, NULL);
                for (int y = 0; y < 128; y++) memset((uint8_t *)IOSurfaceGetBaseAddress(one) + y * IOSurfaceGetBytesPerRow(one), 200, 128);
                IOSurfaceUnlock(one, 0, NULL);
                l.contents = (__bridge id)one;
            } else printf("      (no L008 surface)\n");
            break;
        }
        }
        [root addSublayer:l];
        [CATransaction commit];
        CARenderer *r = [CARenderer rendererWithMTLTexture:target options:nil];
        [CATransaction begin]; [CATransaction setDisableActions:YES];
        r.layer = root; r.bounds = root.bounds;
        [CATransaction commit]; [CATransaction flush];
        for (int f = 0; f < 4; f++) {
            [CATransaction begin]; [CATransaction setDisableActions:YES]; root.backgroundColor = gray; [CATransaction commit]; [CATransaction flush];
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
            [r beginFrameAtTime:CACurrentMediaTime() timeStamp:NULL];
            [r addUpdateRect:root.bounds];
            [r render];
            [r endFrame];
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);
        }
        usleep(300000);
        id<MTLCommandQueue> q = [d newCommandQueue];
        id<MTLBuffer> out = [d newBufferWithLength:W * H * 4 options:MTLResourceStorageModeShared];
        memset(out.contents, 0x5a, out.length);
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
        [e copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W, H, 1)
                  toBuffer:out destinationOffset:0 destinationBytesPerRow:W * 4 destinationBytesPerImage:W * H * 4];
        [e endEncoding];
        commitWait(cb);
        const uint32_t *px = out.contents;
        uint32_t bg = px[8 * W + 8], ctr = px[128 * W + 128];
        printf("      background %08x (%s), middle %08x (%s)\n", bg, pixClass(bg), ctr, pixClass(ctr));
        CGColorRelease(gray); CGColorRelease(red); CGColorRelease(black);
        CGImageRelease(color); CGImageRelease(grayIm); CGImageRelease(alphaIm); if (one) CFRelease(one);
        return strcmp(pixClass(bg), "gray") ? 1 : 0;
    }
}
static int caMaskTest(const char *self) {
    printf("--camasks: the Core Animation cases WindowServer draws with its alpha-mask image shader\n\n");
    int bad = 0;
    for (int v = 0; v < N_MASK_CASES; v++) {
        printf("[%d] %s\n", v, kMaskCase[v]);
        fflush(stdout);
        unsigned r0 = xeResets();
        char arg[8]; snprintf(arg, sizeof arg, "%d", v);
        int rc = runChild(self, "--camaskchild", arg);
        sleep(1);
        unsigned r1 = xeResets();
        bool hang = r1 != r0;
        printf("    -> %s%s\n\n", hang ? "GPU HANG (reset)" : rc == 0 ? "OK" : "drawn, but the picture is wrong", hang ? "" : "");
        bad += hang || rc;
    }
    printf("Core Animation mask cases: %s\n", bad ? "PROBLEMS (see above)" : "ALL OK");
    return bad ? 1 : 0;
}

// v0.51.38: --framerate: how often the screen really changes and how steady the display's refresh clock is,
// while you drag a window. Measures only (no GPU work of its own). The lag check after the CPU was ruled out:
//  - "screen updates": frames WindowServer actually put on the screen (a display stream, like a screen recorder)
//  - "refresh clock": CVDisplayLink, the 60 Hz tick apps and WindowServer time their frames by. It needs the
//    display driver's vertical-blank signal; without it the tick can be missing, slow or uneven.
#include <mach/mach_time.h>
static double frMs(uint64_t t) { static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb); return (double)t * tb.numer / tb.denom / 1e6; }
static int64_t gFrDL, gFrDLsec, gFrUp, gFrUpSec, gFrIdle, gFrStut, gFrBlank, gFrStopped;
static uint64_t gFrDLLast, gFrUpLast;
static double gFrDLMin = 1e9, gFrDLMax, gFrUpMaxSec, gFrUpMax;
static CVReturn frLinkCB(CVDisplayLinkRef l, const CVTimeStamp *now, const CVTimeStamp *out, CVOptionFlags f, CVOptionFlags *fo, void *c) {
    uint64_t t = mach_absolute_time();
    if (gFrDLLast) { double ms = frMs(t - gFrDLLast); if (ms < gFrDLMin) gFrDLMin = ms; if (ms > gFrDLMax) gFrDLMax = ms; }
    gFrDLLast = t;
    __atomic_add_fetch(&gFrDL, 1, __ATOMIC_RELAXED); __atomic_add_fetch(&gFrDLsec, 1, __ATOMIC_RELAXED);
    return kCVReturnSuccess;
}
// v0.57.6: IrisXeCore's IrisXe-DrawPerf after a case: "us each" = the driver's time for a draw (build + submit +
// wait), "GPU drawing" = the GPU's own time. The rest of the wall time above is spent before the core is called.
static void fillRatePerf(void) {
    io_service_t s = findSibling("IrisXeProbe");
    if (!s) s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IrisXeProbe"));
    if (!s) return;
    CFTypeRef v = IORegistryEntryCreateCFProperty(s, CFSTR("IrisXe-DrawPerf"), kCFAllocatorDefault, 0);
    if (v && CFGetTypeID(v) == CFStringGetTypeID()) {
        NSString *t = (__bridge NSString *)v;
        NSRange r = [t rangeOfString:@": "];
        printf("        core: %s\n", (r.location != NSNotFound ? [t substringFromIndex:r.location + 2] : t).UTF8String);
    }
    if (v) CFRelease(v);
    IOObjectRelease(s);
}
// v0.57.1: --fillrate: how fast the GPU fills a 2560x1600 render target (the panel's size): full-screen draws with a
// plain colour, one texture read, and a 9-tap blur (WindowServer's heavy effects). Times whole command buffers.
static double fillRateRun(id<MTLDevice> d, id<MTLCommandQueue> q, id<MTLLibrary> lib, const char *fsName, NSUInteger W, NSUInteger H,
                          NSUInteger draws, id<MTLTexture> src, id<MTLSamplerState> smp, id<MTLBuffer> pb, id<MTLBuffer> cbuf, double *avgOut) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:H mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> rt = [d newTextureWithDescriptor:td];
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"rv_basic"];
    pd.fragmentFunction = [lib newFunctionWithName:[NSString stringWithUTF8String:fsName]];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    NSError *e = nil;
    id<MTLRenderPipelineState> ps = rt ? [d newRenderPipelineStateWithDescriptor:pd error:&e] : nil;
    if (!ps) { printf("    %s: no pipeline / render target (%s)\n", fsName, e ? e.localizedDescription.UTF8String : "-"); return -1; }
    double best = 1e9, sum = 0; int ok = 0;
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    // v0.57.6: at least 200 draws per case, so IrisXeCore's IrisXe-DrawPerf (an average over 100 draws) covers this case only
    int runs = 1 + (int)((200 + draws - 1) / draws); if (runs < 6) runs = 6;
    for (int r = 0; r < runs; r++) {                                // the first run warms up (not counted)
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = rt;
        rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
        [en setRenderPipelineState:ps];
        [en setVertexBuffer:pb offset:0 atIndex:0];
        [en setVertexBuffer:cbuf offset:0 atIndex:1];
        [en setFragmentTexture:src atIndex:0];
        [en setFragmentSamplerState:smp atIndex:0];
        for (NSUInteger i = 0; i < draws; i++) [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [en endEncoding];
        uint64_t t0 = mach_absolute_time();
        [cb commit];
        [cb waitUntilCompleted];
        uint64_t t1 = mach_absolute_time();
        if (cb.status != MTLCommandBufferStatusCompleted) { printf("    %s: command buffer status %ld\n", fsName, (long)cb.status); return -1; }
        double ms = (double)(t1 - t0) * tb.numer / tb.denom / 1e6 / draws;
        if (r == 0) continue;
        sum += ms; ok++;
        if (ms < best) best = ms;
    }
    if (avgOut) *avgOut = sum / ok;
    return best;
}

// v0.57.1: --fillrate: how fast the GPU fills a 2560x1600 render target (the panel's size): full-screen draws with a
// plain colour, one texture read, and a 9-tap blur (WindowServer's heavy effects).
// v0.57.2: + the same draws into a small 640x400 target (fits in the GPU's caches: shows whether memory or the
// pixel shader stage limits), and a blitter fill of 16 MB (the memory write speed without the 3D pipeline).
// v0.57.8: IrisXeCore's pixel pipe balancing table on (1) or off (0) for the following draw jobs
static void setHashCfg(int on) {
    io_service_t s = findSibling("IrisXeProbe");
    if (!s) s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IrisXeProbe"));
    if (!s) { printf("    (IrisXeProbe not found)\n\n"); return; }
    long v = on;
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberLongType, &v);
    kern_return_t kr = IORegistryEntrySetCFProperty(s, CFSTR("IrisXe-HashCfg"), n);
    CFRelease(n);
    CFTypeRef t = IORegistryEntryCreateCFProperty(s, CFSTR("IrisXe-Hash"), kCFAllocatorDefault, 0);
    printf("    balancing: %s%s\n\n", t && CFGetTypeID(t) == CFStringGetTypeID() ? [(__bridge NSString *)t UTF8String] : "(no IrisXe-Hash: old IrisXeCore?)",
           kr ? " [could not switch it]" : "");
    if (t) CFRelease(t);
    IOObjectRelease(s);
}
// v0.57.9: blit copies with (tiled) render-target textures: texture -> texture, texture -> buffer, buffer -> texture,
// at odd sizes and offsets, compared with what the CPU expects
static void blitCheck(id<MTLDevice> d, id<MTLCommandQueue> q) {
    const NSUInteger W = 300, H = 200;
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:H mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> A = [d newTextureWithDescriptor:td], B = [d newTextureWithDescriptor:td], C = [d newTextureWithDescriptor:td];
    id<MTLBuffer> buf = [d newBufferWithLength:W * H * 4 options:MTLResourceStorageModeShared];
    uint32_t *px = malloc(W * H * 4), *z = calloc(W * H, 4), *back = malloc(W * H * 4);
    if (!A || !B || !C || !buf || !px) { printf("    blit check: no textures\n\n"); return; }
    for (NSUInteger i = 0; i < W * H; i++) px[i] = (uint32_t)(i * 2654435761u) | 0xFF000000u;
    [A replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:px bytesPerRow:W * 4];
    [B replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:z bytesPerRow:W * 4];
    [C replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:z bytesPerRow:W * 4];
    memset(buf.contents, 0, W * H * 4);
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
    [be copyFromTexture:A sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(17, 9, 0) sourceSize:MTLSizeMake(200, 150, 1)
              toTexture:B destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(40, 30, 0)];
    [be copyFromTexture:A sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W, H, 1)
               toBuffer:buf destinationOffset:0 destinationBytesPerRow:W * 4 destinationBytesPerImage:W * H * 4];
    [be endEncoding];
    [cb commit]; [cb waitUntilCompleted];
    long badT = 0, badB = 0, badC = 0;
    [B getBytes:back bytesPerRow:W * 4 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
    for (NSUInteger y = 0; y < H; y++)
        for (NSUInteger x = 0; x < W; x++) {
            bool in = x >= 40 && x < 240 && y >= 30 && y < 180;
            uint32_t want = in ? px[(y - 30 + 9) * W + (x - 40 + 17)] : 0;
            if (back[y * W + x] != want) badT++;
        }
    const uint32_t *bb = buf.contents;
    for (NSUInteger i = 0; i < W * H; i++) if (bb[i] != px[i]) badB++;
    // buffer (now a copy of A) -> C at (5, 7), 100 x 80 from the buffer's row 3, column 11
    cb = [q commandBuffer];
    be = [cb blitCommandEncoder];
    [be copyFromBuffer:buf sourceOffset:(3 * W + 11) * 4 sourceBytesPerRow:W * 4 sourceBytesPerImage:W * H * 4
            sourceSize:MTLSizeMake(100, 80, 1) toTexture:C destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(5, 7, 0)];
    [be endEncoding];
    [cb commit]; [cb waitUntilCompleted];
    [C getBytes:back bytesPerRow:W * 4 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
    for (NSUInteger y = 0; y < H; y++)
        for (NSUInteger x = 0; x < W; x++) {
            bool in = x >= 5 && x < 105 && y >= 7 && y < 87;
            uint32_t want = in ? px[(y - 7 + 3) * W + (x - 5 + 11)] : 0;
            if (back[y * W + x] != want) badC++;
        }
    printf("    blit check (%lux%lu, odd offsets): texture->texture %s (%ld wrong), texture->buffer %s (%ld), buffer->texture %s (%ld)\n\n",
           (unsigned long)W, (unsigned long)H, badT ? "WRONG" : "OK", badT, badB ? "WRONG" : "OK", badB, badC ? "WRONG" : "OK", badC);
    free(px); free(z); free(back);
}
// v0.57.10: --blittest: v0.57.9's blit check found copyFromBuffer:toTexture: wrong (4000 of 8000 pixels, linear and
// tiled alike). The same rectangle copy with different buffer offsets / row pitches, both directions, and for wrong
// pixels where the blitter really read them from (each source pixel's value is unique).
static int blitVariantTest(void) {
    @autoreleasepool {
        setenv("IRISXE_TILED", "0", 1);                       // the blitter path itself (no tiling)
        id<MTLDevice> d = ourDevice();
        if (!d) { printf("    no IrisXe device\n"); return 2; }
        id<MTLCommandQueue> q = [d newCommandQueue];
        const NSUInteger W = 300, H = 200, CW = 100, CH = 80, BUFLEN = 4 << 20;
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:H mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModeShared;
        id<MTLTexture> T = [d newTextureWithDescriptor:td];
        id<MTLBuffer> buf = [d newBufferWithLength:BUFLEN options:MTLResourceStorageModeShared];
        uint32_t *back = malloc(W * H * 4), *z = calloc(W * H, 4);
        if (!T || !buf || !back) { printf("    no texture / buffer\n"); return 3; }
        printf("--blittest: copy %lux%lu between a buffer and a %lux%lu texture at (5, 7)\n\n", (unsigned long)CW, (unsigned long)CH, (unsigned long)W, (unsigned long)H);
        const NSUInteger offs[5] = { 0, 4, 64, 3644, 4096 + 44 }, pitches[5] = { 1200, 1216, 1280, 1204, 400 };
        for (int dir = 0; dir < 2; dir++) {
            printf("  %s\n", dir == 0 ? "buffer -> texture (copyFromBuffer:toTexture:)" : "texture -> buffer (copyFromTexture:toBuffer:)");
            for (int oi = 0; oi < 5; oi++)
                for (int pi = 0; pi < 5; pi++) {
                    NSUInteger off = offs[oi], bpr = pitches[pi];
                    uint32_t *b = (uint32_t *)buf.contents;
                    // the buffer: pixel at byte offset o holds 0xA0000000 | o/4 (unique, says where it came from)
                    for (NSUInteger i = 0; i < BUFLEN / 4; i++) b[i] = dir == 0 ? (0xA0000000u | (uint32_t)i) : 0;
                    uint32_t *tp = malloc(W * H * 4);
                    for (NSUInteger i = 0; i < W * H; i++) tp[i] = dir == 0 ? 0 : (0xB0000000u | (uint32_t)i);
                    [T replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:tp bytesPerRow:W * 4];
                    id<MTLCommandBuffer> cb = [q commandBuffer];
                    id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
                    if (dir == 0)
                        [be copyFromBuffer:buf sourceOffset:off sourceBytesPerRow:bpr sourceBytesPerImage:bpr * CH sourceSize:MTLSizeMake(CW, CH, 1)
                                 toTexture:T destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(5, 7, 0)];
                    else
                        [be copyFromTexture:T sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(5, 7, 0) sourceSize:MTLSizeMake(CW, CH, 1)
                                   toBuffer:buf destinationOffset:off destinationBytesPerRow:bpr destinationBytesPerImage:bpr * CH];
                    [be endEncoding];
                    [cb commit]; [cb waitUntilCompleted];
                    long bad = 0, badRows = 0; NSUInteger fx = 0, fy = 0; uint32_t fgot = 0, fwant = 0;
                    if (dir == 0) [T getBytes:back bytesPerRow:W * 4 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
                    for (NSUInteger y = 0; y < CH; y++) {
                        bool rowBad = false;
                        for (NSUInteger x = 0; x < CW; x++) {
                            uint32_t want, got;
                            if (dir == 0) { want = 0xA0000000u | (uint32_t)((off + y * bpr + x * 4) / 4); got = back[(y + 7) * W + x + 5]; }
                            else { want = 0xB0000000u | (uint32_t)((y + 7) * W + x + 5); got = b[(off + y * bpr + x * 4) / 4]; }
                            if (got != want) { if (!bad) { fx = x; fy = y; fgot = got; fwant = want; } bad++; rowBad = true; }
                        }
                        if (rowBad) badRows++;
                    }
                    if (!bad) printf("    offset %4lu, %4lu bytes per row: OK\n", (unsigned long)off, (unsigned long)bpr);
                    else {
                        char where[120] = "";
                        if (dir == 0 && (fgot >> 28) == 0xA) {
                            long gotByte = (long)(fgot & 0x0FFFFFFF) * 4, wantByte = (long)(fwant & 0x0FFFFFFF) * 4;
                            snprintf(where, sizeof(where), "read from buffer byte %ld, wanted %ld (%+ld)", gotByte, wantByte, gotByte - wantByte);
                        } else if (dir == 1 && (fgot >> 28) == 0xB) {
                            long g = fgot & 0x0FFFFFFF, w = fwant & 0x0FFFFFFF;
                            snprintf(where, sizeof(where), "texel (%ld,%ld), wanted (%ld,%ld)", g % (long)W, g / (long)W, w % (long)W, w / (long)W);
                        } else snprintf(where, sizeof(where), "value 0x%08x (not copied?)", fgot);
                        printf("    offset %4lu, %4lu bytes per row: WRONG %ld of %lu (%ld rows), first at (%lu,%lu): %s\n",
                               (unsigned long)off, (unsigned long)bpr, bad, (unsigned long)(CW * CH), badRows, (unsigned long)fx, (unsigned long)fy, where);
                    }
                    free(tp);
                }
            printf("\n");
        }
        free(back); free(z);
        return 0;
    }
}
// v0.58.1: --pixelrate: what limits WindowServer's big fills? Full-screen plain-colour draws into render targets of
// 1, 4, 8 and 16 bytes per pixel, blending on / off, and with colour writes turned off (the pixel shader still runs).
// From IrisXeCore's GPU time: pixels per GPU clock and bytes per clock. Same pixels/clock for every size = the
// pixel shader stage (SIMD8 dispatch) limits; same bytes/clock = the path to memory limits.
static double perfGPUus(unsigned *mhzOut) {
    double us = -1; unsigned mhz = 0;
    io_service_t s = findSibling("IrisXeProbe");
    if (!s) s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IrisXeProbe"));
    if (!s) return -1;
    CFTypeRef v = IORegistryEntryCreateCFProperty(s, CFSTR("IrisXe-DrawPerf"), kCFAllocatorDefault, 0);
    if (v && CFGetTypeID(v) == CFStringGetTypeID()) {
        const char *c = [(__bridge NSString *)v UTF8String], *g = c ? strstr(c, "GPU drawing ") : NULL;
        if (g) us = atof(g + 12);
    }
    if (v) CFRelease(v);
    v = IORegistryEntryCreateCFProperty(s, CFSTR("IrisXe-GPUFreq"), kCFAllocatorDefault, 0);
    if (v && CFGetTypeID(v) == CFStringGetTypeID()) {
        const char *c = [(__bridge NSString *)v UTF8String], *g = c ? strstr(c, "now ") : NULL;
        if (g) mhz = (unsigned)atoi(g + 4);
    }
    if (v) CFRelease(v);
    IOObjectRelease(s);
    if (mhzOut) *mhzOut = mhz;
    return us;
}
static int pixelRateTest(void) {
    @autoreleasepool {
        id<MTLDevice> d = ourDevice();
        if (!d) { printf("    no IrisXe device\n"); return 2; }
        id<MTLCommandQueue> q = [d newCommandQueue];
        NSError *e = nil;
        id<MTLLibrary> lib = testLibrary(d);
        if (!lib) return 3;
        const NSUInteger W = 2560, H = 1600, draws = 10;
        float pos[3][4] = { { -1, -1, 0, 1 }, { 3, -1, 0, 1 }, { -1, 3, 0, 1 } };
        float col[3][4] = { { 0.2f, 0.4f, 0.6f, 0.5f }, { 0.2f, 0.4f, 0.6f, 0.5f }, { 0.2f, 0.4f, 0.6f, 0.5f } };
        id<MTLBuffer> pb = [d newBufferWithBytes:pos length:sizeof(pos) options:MTLResourceStorageModeShared];
        id<MTLBuffer> cbuf = [d newBufferWithBytes:col length:sizeof(col) options:MTLResourceStorageModeShared];
        // blend: 0 off, 1 source alpha / 1 - source alpha, 2 one / zero, 3 one / blend colour (0), 4 = 1 with alpha 1
        // noWrite: 1 colour writes off, 2 with v0.58.5's opaque blending (off by default since v0.58.6)
        struct { MTLPixelFormat pf; unsigned bpp; int blend; int noWrite; const char *name; } cs[] = {
            { MTLPixelFormatR8Unorm, 1, 0, 0, "R8 (1 byte)          " },
            { MTLPixelFormatBGRA8Unorm, 4, 0, 0, "BGRA8 (4 bytes)      " },
            { MTLPixelFormatBGRA8Unorm, 4, 0, 2, "BGRA8, opaque-blend  " },   // v0.58.5's trick (IRISXE_OPAQUEBLEND=1)
            { MTLPixelFormatRGBA16Float, 8, 0, 0, "RGBA16F (8 bytes)    " },
            { MTLPixelFormatRGBA32Float, 16, 0, 0, "RGBA32F (16 bytes)   " },
            { MTLPixelFormatBGRA8Unorm, 4, 1, 0, "BGRA8 + blending a.5 " },
            { MTLPixelFormatBGRA8Unorm, 4, 4, 0, "BGRA8 + blending a=1 " },   // v0.58.6: the same blend, opaque source
            { MTLPixelFormatBGRA8Unorm, 4, 2, 0, "BGRA8, blend 1/0     " },
            { MTLPixelFormatBGRA8Unorm, 4, 3, 0, "BGRA8, blend 1/const0" },   // v0.58.6: one / blend colour (0,0,0,0)
            { MTLPixelFormatBGRA8Unorm, 4, 0, 1, "BGRA8, writes off    " },
        };
        float col1[3][4] = { { 0.2f, 0.4f, 0.6f, 1 }, { 0.2f, 0.4f, 0.6f, 1 }, { 0.2f, 0.4f, 0.6f, 1 } };
        id<MTLBuffer> cbuf1 = [d newBufferWithBytes:col1 length:sizeof(col1) options:MTLResourceStorageModeShared];
        printf("--pixelrate: full screen %lux%lu, plain colour, %lu draws per command buffer (GPU time from IrisXeCore)\n",
               (unsigned long)W, (unsigned long)H, (unsigned long)draws);
        for (int tiled = 1; tiled >= 0; tiled--) {
            if (tiled) unsetenv("IRISXE_TILED"); else setenv("IRISXE_TILED", "0", 1);
            printf("\n  render targets %s\n", tiled ? "tiled (the default)" : "linear");
            for (unsigned c = 0; c < sizeof(cs) / sizeof(cs[0]); c++) {
                MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:cs[c].pf width:W height:H mipmapped:NO];
                td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
                td.storageMode = MTLStorageModeShared;               // v0.58.6: read back (is the picture right?)
                id<MTLTexture> rt = [d newTextureWithDescriptor:td];
                MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
                pd.vertexFunction = [lib newFunctionWithName:@"rv_basic"];
                pd.fragmentFunction = [lib newFunctionWithName:@"rf_color"];
                pd.colorAttachments[0].pixelFormat = cs[c].pf;
                if (cs[c].blend == 2 || cs[c].blend == 3) {     // src x 1 + dst x 0: the same picture as no blending
                    MTLBlendFactor z = cs[c].blend == 3 ? MTLBlendFactorBlendColor : MTLBlendFactorZero;
                    pd.colorAttachments[0].blendingEnabled = YES;
                    pd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
                    pd.colorAttachments[0].destinationRGBBlendFactor = z;
                    pd.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
                    pd.colorAttachments[0].destinationAlphaBlendFactor = z;
                } else if (cs[c].blend) {
                    pd.colorAttachments[0].blendingEnabled = YES;
                    pd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
                    pd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
                    pd.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
                    pd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
                }
                if (cs[c].noWrite == 1) pd.colorAttachments[0].writeMask = MTLColorWriteMaskNone;
                if (cs[c].noWrite == 2) setenv("IRISXE_OPAQUEBLEND", "1", 1);   // with v0.58.5's opaque blending
                e = nil;
                id<MTLRenderPipelineState> ps = rt ? [d newRenderPipelineStateWithDescriptor:pd error:&e] : nil;
                unsetenv("IRISXE_OPAQUEBLEND");
                if (!ps) { printf("    %s no pipeline / render target (%s)\n", cs[c].name, e ? e.localizedDescription.UTF8String : "-"); continue; }
                bool fail = false;
                for (int r = 0; r < 22 && !fail; r++) {                 // 220 draws: IrisXe-DrawPerf covers this case only
                    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                    rp.colorAttachments[0].texture = rt;
                    rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
                    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                    id<MTLCommandBuffer> cb = [q commandBuffer];
                    id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
                    [en setRenderPipelineState:ps];
                    [en setVertexBuffer:pb offset:0 atIndex:0];
                    [en setVertexBuffer:(cs[c].blend == 4 ? cbuf1 : cbuf) offset:0 atIndex:1];
                    if (cs[c].blend == 3) [en setBlendColorRed:0 green:0 blue:0 alpha:0];
                    for (NSUInteger i = 0; i < draws; i++) [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                    [en endEncoding];
                    [cb commit]; [cb waitUntilCompleted];
                    if (cb.status != MTLCommandBufferStatusCompleted) { printf("    %s command buffer status %ld\n", cs[c].name, (long)cb.status); fail = true; }
                }
                if (fail) continue;
                unsigned mhz = 0;
                double us = perfGPUus(&mhz);
                if (us <= 0 || !mhz) { printf("    %s (no GPU time from IrisXeCore)\n", cs[c].name); continue; }
                double px = (double)W * H, pxClk = px / (us * mhz), gpx = px / us / 1000.0;
                char chk[64] = "";
                if (cs[c].pf == MTLPixelFormatBGRA8Unorm && cs[c].noWrite != 1) {   // every pixel should now be ~ (51, 102, 153)
                    uint8_t px[4 * 5] = {0};
                    const NSUInteger xs[5] = { 0, W / 2, W - 1, W / 3, 7 }, ys[5] = { 0, H / 2, H - 1, (2 * H) / 3, H - 9 };
                    int bad = 0;
                    for (int k = 0; k < 5; k++) {
                        [rt getBytes:&px[4 * k] bytesPerRow:4 fromRegion:MTLRegionMake2D(xs[k], ys[k], 1, 1) mipmapLevel:0];
                        if (abs(px[4 * k] - 153) > 3 || abs(px[4 * k + 1] - 102) > 3 || abs(px[4 * k + 2] - 51) > 3) bad++;
                    }
                    snprintf(chk, sizeof(chk), "  picture %s", bad ? "WRONG" : "ok");
                    if (bad) snprintf(chk + strlen(chk), sizeof(chk) - strlen(chk), " (%d,%d,%d)", px[6], px[5], px[4]);
                }
                printf("    %s %7.0f us | %5.2f Gpixels/s | %5.2f pixels/clock | %6.1f bytes/clock | %5.1f GB/s  (GPU %u MHz)%s\n",
                       cs[c].name, us, gpx, pxClk, pxClk * cs[c].bpp, gpx * cs[c].bpp, mhz, chk);
            }
        }
        unsetenv("IRISXE_TILED");
        printf("\n  (Intel's own drivers fill ~16-24 pixels/clock on this chip; WindowServer's frame needs several full-screen fills)\n");
        return 0;
    }
}
// v0.58.7: --bigalloc: more GPU memory in one program than the old 512 MB limit (WindowServer crashed when its
// newBuffer failed there). 20 buffers of 64 MB (1.25 GB); every one must exist, and the GPU must fill, copy and
// run a kernel on the ones at the highest addresses, plus clear a texture made after them.
static int bigAllocTest(void) {
    @autoreleasepool {
        printf("--bigalloc: 1.25 GB of GPU buffers in one program (the old limit was 512 MB)\n\n");
        id<MTLDevice> d = ourDevice();
        if (!d) { printf("    no IrisXe device\n"); return 2; }
        id<MTLCommandQueue> q = [d newCommandQueue];
        enum { N = 20 };
        const NSUInteger len = 64u << 20;
        id<MTLBuffer> b[N] = {};
        int made = 0, above1G = 0, fails = 0;
        for (int i = 0; i < N; i++) {
            b[i] = [d newBufferWithLength:len options:MTLResourceStorageModeShared];
            if (!b[i]) { printf("    buffer %d (64 MB, %d MB so far): FAILED to make\n", i, made * 64); fails++; break; }
            made++;
            if (b[i].gpuAddress >= 0x40000000ULL) above1G++;
            printf("    buffer %2d: GPU 0x%09llx\n", i, (unsigned long long)b[i].gpuAddress);
        }
        fflush(stdout);
        printf("    made %d of %d buffers (%d MB), %d above 1 GB\n", made, N, made * 64, above1G);
        if (made < 3) { printf("    bigalloc test: FAILED\n"); return 1; }
        // the three highest buffers
        int idx[3] = { -1, -1, -1 };
        for (int k = 0; k < 3; k++)
            for (int i = 0; i < made; i++) {
                bool used = false;
                for (int j = 0; j < k; j++) used |= idx[j] == i;
                if (!used && (idx[k] < 0 || b[i].gpuAddress > b[idx[k]].gpuAddress)) idx[k] = i;
            }
        id<MTLBuffer> A = b[idx[0]], B = b[idx[1]], C = b[idx[2]];
        uint32_t *pa = (uint32_t *)A.contents, *pb = (uint32_t *)B.contents;
        const NSUInteger words = len / 4;
        // 1. blitter fill of the highest buffer (start, a middle part and its last bytes)
        memset(pa, 0x11, len);
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
        [e fillBuffer:A range:NSMakeRange(0, 1 << 20) value:0x5A];
        [e fillBuffer:A range:NSMakeRange(len - (1 << 20), 1 << 20) value:0xA5];
        [e endEncoding];
        int r = commitWait(cb);
        bool ok1 = !r;
        for (NSUInteger k = 0; ok1 && k < (1u << 18); k++) ok1 = pa[k] == 0x5A5A5A5Au && pa[words - 1 - k] == 0xA5A5A5A5u;
        for (NSUInteger k = (1u << 18); ok1 && k < words - (1u << 18); k += 4099) ok1 = pa[k] == 0x11111111u;
        printf("    fill (blitter) at GPU 0x%llx: %s\n", (unsigned long long)A.gpuAddress, ok1 ? "RIGHT" : "WRONG");
        fails += !ok1;
        // 2. blitter copy of 16 MB from the highest buffer into the second highest
        for (NSUInteger k = 0; k < words; k++) pa[k] = (uint32_t)(k * 2654435761u);
        memset(pb, 0, len);
        cb = [q commandBuffer];
        e = [cb blitCommandEncoder];
        [e copyFromBuffer:A sourceOffset:len - (16u << 20) toBuffer:B destinationOffset:4096 size:16u << 20];
        [e endEncoding];
        r = commitWait(cb);
        bool ok2 = !r;
        NSUInteger off = (len - (16u << 20)) / 4;
        for (NSUInteger k = 0; ok2 && k < (4u << 20); k++) ok2 = pb[1024 + k] == pa[off + k];
        ok2 = ok2 && pb[1023] == 0 && pb[1024 + (4u << 20)] == 0;
        printf("    copy 16 MB (blitter) 0x%llx -> 0x%llx: %s\n", (unsigned long long)A.gpuAddress + len - (16u << 20),
               (unsigned long long)B.gpuAddress + 4096, ok2 ? "RIGHT" : "WRONG");
        fails += !ok2;
        // 3. compute kernel (out = in + 1) from the second highest into the third highest buffer, near their ends
        SEL mk = sel_registerName("irisxeNewComputePipelineWithKernel:");
        if ([(id)d respondsToSelector:mk]) {
            NSData *code = [NSData dataWithBytes:kXeAdd1 length:sizeof(kXeAdd1)];
            id<MTLComputePipelineState> ps = ((id (*)(id, SEL, id))objc_msgSend)((id)d, mk, code);
            const NSUInteger n = 1u << 20, at = len - (n + 64) * 4;
            uint32_t *pi = (uint32_t *)((uint8_t *)B.contents + at), *po = (uint32_t *)((uint8_t *)C.contents + at);
            for (NSUInteger k = 0; k < n + 64; k++) { pi[k] = (uint32_t)(k * 3 + 7); po[k] = 0xDEADBEEF; }
            cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
            bool ok3 = ps && ce;
            if (ok3) {
                [ce setComputePipelineState:ps];
                [ce setBuffer:B offset:at atIndex:0];
                [ce setBuffer:C offset:at atIndex:1];
                [ce dispatchThreads:MTLSizeMake(n, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
                [ce endEncoding];
                ok3 = !commitWait(cb);
            }
            NSUInteger right = 0;
            for (NSUInteger k = 0; ok3 && k < n; k++) right += po[k] == pi[k] + 1;
            ok3 = ok3 && right == n && po[n] == 0xDEADBEEF;
            printf("    compute kernel 0x%llx -> 0x%llx (1M values): %lu right -> %s\n",
                   (unsigned long long)B.gpuAddress + at, (unsigned long long)C.gpuAddress + at, (unsigned long)right, ok3 ? "RIGHT" : "WRONG");
            fails += !ok3;
        } else printf("    compute kernel: skipped (bundle without compute)\n");
        // 4. a render pass clear of a texture made after the buffers (so also at a high address)
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                      width:512 height:512 mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModeShared;
        id<MTLTexture> t = [d newTextureWithDescriptor:td];
        bool ok4 = t != nil;
        if (t) {
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = t;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 1.0, 0.0, 1.0);
            cb = [q commandBuffer];
            id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
            [re endEncoding];
            ok4 = !commitWait(cb);
            uint32_t *pix = (uint32_t *)malloc(512 * 512 * 4);
            [t getBytes:pix bytesPerRow:512 * 4 fromRegion:MTLRegionMake2D(0, 0, 512, 512) mipmapLevel:0];
            NSUInteger good = 0;
            for (NSUInteger k = 0; k < 512 * 512; k++) good += pix[k] == 0xFF00FF00u;
            free(pix);
            ok4 = ok4 && good == 512 * 512;
            printf("    clear a 512x512 render target (render engine): %lu/%u pixels green -> %s\n",
                   (unsigned long)good, 512 * 512, ok4 ? "RIGHT" : "WRONG");
        } else printf("    texture after the buffers: FAILED to make\n");
        fails += !ok4;
        printf("\n");
        printProp("IrisXeProbe", CFSTR("IrisXe-GPUSpace"), "GPU spaces (core)");
        printProp("IrisXeAccelerator", CFSTR("IrisXe-GPUVA"), "GPU addresses (accel)");
        for (int i = 0; i < made; i++) [b[i] release];
        bool gone = gpuHung();
        if (gone) { printf("    THE GPU HUNG\n"); fails++; }
        printf("\n    bigalloc test: %s%s\n", fails ? "FAILED" : "ALL OK",
               !fails && made == N && !above1G ? " (but nothing above 1 GB: is the new IrisXeAccel installed?)" : "");
        return fails ? 1 : 0;
    }
}

// v0.58.3: --mixreset: IrisXeCore's IrisXe-DrawMix (GPU time by kind of drawing) starts from zero (needs sudo)
static int mixReset(void) {
    io_service_t s = findSibling("IrisXeProbe");          // v0.58.4: IrisXeProbe isn't a registered service (no match)
    if (!s) s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IrisXeProbe"));
    if (!s) { printf("IrisXeProbe not found\n"); return 2; }
    long v = 1;
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberLongType, &v);
    kern_return_t kr = IORegistryEntrySetCFProperty(s, CFSTR("IrisXe-DrawMixReset"), n);
    CFRelease(n);
    IOObjectRelease(s);
    printf("IrisXe-DrawMix reset: %s\n", kr ? "failed (run with sudo)" : "ok");
    return kr ? 3 : 0;
}
static volatile int gCpuSink;
// v0.57.7: a picture check for the tiled test: one full-screen draw that copies a 2560x1600 pattern 1:1 (the sampler
// at texel centres), read back and compared at 2000 points. Run linear and tiled: the two must agree.
static void fillRateCheck(id<MTLDevice> d, id<MTLCommandQueue> q, id<MTLLibrary> lib, id<MTLBuffer> pb, id<MTLBuffer> cbuf,
                          id<MTLSamplerState> smp) {
    const NSUInteger W = 2560, H = 1600;
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:H mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> src = [d newTextureWithDescriptor:td], rt = [d newTextureWithDescriptor:td];
    uint32_t *px = malloc(W * H * 4);
    if (!src || !rt || !px) { printf("    picture check: no textures\n\n"); free(px); return; }
    for (NSUInteger y = 0; y < H; y++)
        for (NSUInteger x = 0; x < W; x++) px[y * W + x] = (uint32_t)(((x ^ y) & 0xFF) | (((x >> 3) & 0xFF) << 8) | ((y & 0xFF) << 16)) | 0xFF000000u;
    [src replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:px bytesPerRow:W * 4];
    uint32_t *back = malloc(W * H * 4);
    memset(back, 0, W * H * 4);
    [src getBytes:back bytesPerRow:W * 4 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
    bool cpuOK = memcmp(back, px, W * H * 4) == 0;
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];          // nearest: exact texels
    id<MTLSamplerState> near = [d newSamplerStateWithDescriptor:sd];
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"rv_basic"];
    pd.fragmentFunction = [lib newFunctionWithName:@"rf_texture"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    NSError *e = nil;
    id<MTLRenderPipelineState> ps = [d newRenderPipelineStateWithDescriptor:pd error:&e];
    long bad = -1, badFlip = -1;
    if (ps) {
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = rt;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
        [en setRenderPipelineState:ps];
        [en setVertexBuffer:pb offset:0 atIndex:0];
        [en setVertexBuffer:cbuf offset:0 atIndex:1];
        [en setFragmentTexture:src atIndex:0];
        [en setFragmentSamplerState:near atIndex:0];
        [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [en endEncoding];
        [cb commit]; [cb waitUntilCompleted];
        if (cb.status == MTLCommandBufferStatusCompleted) {
            [rt getBytes:back bytesPerRow:W * 4 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
            bad = badFlip = 0;
            uint32_t seed = 12345;
            for (int i = 0; i < 2000; i++) {
                seed = seed * 1103515245u + 12345u; NSUInteger x = (seed >> 8) % W;
                seed = seed * 1103515245u + 12345u; NSUInteger y = (seed >> 8) % H;
                uint32_t got = back[y * W + x];
                if (got != px[y * W + x]) bad++;
                if (got != px[(H - 1 - y) * W + x]) badFlip++;
            }
        }
    }
    printf("    picture check: CPU write + read back %s | GPU copy: %ld of 2000 points wrong (%ld if the picture is upside down)%s\n\n",
           cpuOK ? "OK" : "WRONG", bad, badFlip, bad < 0 ? " - draw failed" : (bad == 0 || badFlip == 0) ? " - OK" : " - WRONG");
    free(px); free(back);
}
static int fillRateTest(void) {
    @autoreleasepool {
        id<MTLDevice> d = ourDevice();
        if (!d) { printf("    no IrisXe device\n"); return 2; }
        id<MTLLibrary> lib = testLibrary(d);
        if (!lib) return 3;
        id<MTLCommandQueue> q = [d newCommandQueue];
        // v0.57.9: two passes: tiling off (as before), then the new default (render targets tiled). Balancing on.
        const char *modeName[2] = { "LINEAR (tiling off: IRISXE_TILED=0)", "DEFAULT (v0.57.9: render targets tiled)" };
        for (int mode = 0; mode < 2; mode++) {
        if (mode == 0) setenv("IRISXE_TILED", "0", 1); else unsetenv("IRISXE_TILED");
        printf("==== %s ====\n", modeName[mode]);
        setHashCfg(1);
            const NSUInteger W = 2560, H = 1600;
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                           width:W height:H mipmapped:NO];
            td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            td.storageMode = MTLStorageModeShared;
            id<MTLTexture> src = [d newTextureWithDescriptor:td];
            if (!q || !src) { printf("    queue %p texture %p\n", q, src); return 3; }
            uint32_t *row = malloc(W * 4);
            for (NSUInteger y = 0; y < H; y++) {
                for (NSUInteger x = 0; x < W; x++) row[x] = (uint32_t)((x ^ y) & 0xFF) * 0x010101u | 0xFF000000u;
                [src replaceRegion:MTLRegionMake2D(0, y, W, 1) mipmapLevel:0 withBytes:row bytesPerRow:W * 4];
            }
            free(row);
            float pos[3][4] = { { -1, -1, 0, 1 }, { 3, -1, 0, 1 }, { -1, 3, 0, 1 } }, col[3][4] = { { 1, 1, 1, 1 }, { 1, 1, 1, 1 }, { 1, 1, 1, 1 } };
            id<MTLBuffer> pb = [d newBufferWithBytes:pos length:sizeof(pos) options:MTLResourceStorageModeShared];
            id<MTLBuffer> cbuf = [d newBufferWithBytes:col length:sizeof(col) options:MTLResourceStorageModeShared];
            MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
            sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
            id<MTLSamplerState> smp = [d newSamplerStateWithDescriptor:sd];
            const char *fs[3] = { "rf_color", "rf_texture", "rf_blur9" };
            const char *what[3] = { "plain colour  ", "1 texture read", "9-tap blur    " };
            // v0.57.3: a small 256x256 source texture (256 KB, stays in the caches) for the small and tiny targets, and a
            // tiny 8x8 target: what one draw costs the GPU when it has almost no pixels
            td.width = 256; td.height = 256;
            id<MTLTexture> srcSmall = [d newTextureWithDescriptor:td];
            {   uint32_t *px = malloc(256 * 256 * 4);
                for (int i = 0; i < 256 * 256; i++) px[i] = (uint32_t)((i ^ (i >> 8)) & 0xFF) * 0x010101u | 0xFF000000u;
                [srcSmall replaceRegion:MTLRegionMake2D(0, 0, 256, 256) mipmapLevel:0 withBytes:px bytesPerRow:256 * 4];
                free(px); }
            struct { NSUInteger w, h, draws; const char *name; bool smallSrc; } sizes[4] = {
                { 2560, 1600, 10, "full screen 2560x1600, texture 2560x1600", false },
                { 640, 400, 40, "small 640x400, texture 2560x1600", false },
                { 640, 400, 40, "small 640x400, texture 256x256 (all in the caches)", true },
                { 8, 8, 100, "tiny 8x8 (the cost of a draw itself)", true } };
            for (int z = 0; z < 4; z++) {
                printf("--fillrate: %s, %lu draws per command buffer, 5 command buffers\n", sizes[z].name, (unsigned long)sizes[z].draws);
                for (int k = 0; k < 3; k++) {
                    double avg = 0;
                    double best = fillRateRun(d, q, lib, fs[k], sizes[z].w, sizes[z].h, sizes[z].draws, sizes[z].smallSrc ? srcSmall : src, smp, pb, cbuf, &avg);
                    if (best < 0) continue;
                    double mpix = (double)sizes[z].w * sizes[z].h / (best / 1000.0) / 1e6;
                    if (z == 0) printf("    %s %6.2f ms per draw (best), %6.2f ms average | %6.0f Mpixels/s | %4.1f such draws fit in 16.7 ms\n",
                                       what[k], best, avg, mpix, 16.7 / best);
                    else printf("    %s %6.3f ms per draw (best), %6.3f ms average | %6.0f Mpixels/s\n", what[k], best, avg, mpix);
                    fillRatePerf();                                   // v0.57.6: where the time goes (driver or GPU)
                }
                printf("\n");
            }
        fillRateCheck(d, q, lib, pb, cbuf, smp);
        blitCheck(d, q);
        }
        unsetenv("IRISXE_TILED");
        setHashCfg(1);                                            // back to the default
        {   // the blitter: 16 MB fill, no 3D pipeline
            const NSUInteger len = 16 << 20;
            id<MTLBuffer> b = [d newBufferWithLength:len options:MTLResourceStorageModeShared];
            double best = 1e9;
            mach_timebase_info_data_t tb; mach_timebase_info(&tb);
            for (int r = 0; r < 6 && b; r++) {
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
                [be fillBuffer:b range:NSMakeRange(0, len) value:(uint8_t)(0x10 + r)];
                [be endEncoding];
                uint64_t t0 = mach_absolute_time();
                [cb commit];
                [cb waitUntilCompleted];
                uint64_t t1 = mach_absolute_time();
                double ms = (double)(t1 - t0) * tb.numer / tb.denom / 1e6;
                if (r && ms < best) best = ms;
            }
            if (b && best < 1e9) printf("--fillrate: blitter fill of 16 MB: %.2f ms (%.1f GB/s)\n\n", best, (double)len / (best / 1000.0) / 1e9);
        }
        {   // v0.57.5: the same memory from the CPU: is the RAM itself slow, or only the GPU's way to it?
            const size_t len = 64 << 20;
            char *a = malloc(len), *b = malloc(len);
            if (a && b) {
                memset(a, 1, len); memset(b, 2, len);
                mach_timebase_info_data_t tb; mach_timebase_info(&tb);
                double bestW = 1e9, bestC = 1e9;
                for (int r = 0; r < 5; r++) {
                    // v0.57.6: the compiler dropped the memset/memcpy (nothing read the buffer after): keep them
                    uint64_t t0 = mach_absolute_time(); memset(b, r, len);
                    __asm__ volatile("" : : "r"(b) : "memory");
                    uint64_t t1 = mach_absolute_time(); memcpy(b, a, len);
                    __asm__ volatile("" : : "r"(a), "r"(b) : "memory");
                    uint64_t t2 = mach_absolute_time();
                    gCpuSink += ((volatile char *)b)[len / 2];
                    double w = (double)(t1 - t0) * tb.numer / tb.denom / 1e6, c = (double)(t2 - t1) * tb.numer / tb.denom / 1e6;
                    if (w < bestW) bestW = w;
                    if (c < bestC) bestC = c;
                }
                printf("--fillrate: CPU, 64 MB: fill %.1f GB/s, copy %.1f GB/s (read + write)\n\n",
                       len / (bestW / 1000.0) / 1e9, 2.0 * len / (bestC / 1000.0) / 1e9);
            }
            free(a); free(b);
        }
        printf("    (smooth desktop: WindowServer's frame - several full-screen layers - must fit in 16.7 ms)\n");
        return 0;
    }
}

static int frameRateTest(void) {
    printf("--framerate: how often the screen really updates, and the display's refresh clock\n\n");
    CGDirectDisplayID d = CGMainDisplayID();
    CGDisplayModeRef m = CGDisplayCopyDisplayMode(d);
    printf("display: %zux%zu points, %zux%zu pixels, refresh rate reported by the driver: %.1f Hz%s\n",
           m ? CGDisplayModeGetWidth(m) : 0, m ? CGDisplayModeGetHeight(m) : 0,
           m ? CGDisplayModeGetPixelWidth(m) : 0, m ? CGDisplayModeGetPixelHeight(m) : 0, m ? CGDisplayModeGetRefreshRate(m) : 0,
           m && CGDisplayModeGetRefreshRate(m) == 0 ? " (0 = unknown; macOS then assumes 60)" : "");
    if (m) CGDisplayModeRelease(m);
    CVDisplayLinkRef dl = NULL;
    CVReturn cr = CVDisplayLinkCreateWithCGDisplay(d, &dl);
    if (cr != kCVReturnSuccess || !dl) printf("refresh clock: could not be created (error %d)\n", cr);
    else {
        CVTime nom = CVDisplayLinkGetNominalOutputVideoRefreshPeriod(dl);
        if (!(nom.flags & kCVTimeIsIndefinite) && nom.timeValue)
            printf("refresh clock: nominal %.2f Hz\n", (double)nom.timeScale / (double)nom.timeValue);
        else printf("refresh clock: nominal period unknown\n");
        CVDisplayLinkSetOutputCallback(dl, frLinkCB, NULL);
        cr = CVDisplayLinkStart(dl);
        if (cr != kCVReturnSuccess) printf("refresh clock: could not start (error %d)\n", cr);
    }
    // a small stream is enough: we count frames, we don't keep them
    dispatch_queue_t q = dispatch_queue_create("irisxe.framerate", DISPATCH_QUEUE_SERIAL);
    NSDictionary *props = @{ (__bridge NSString *)kCGDisplayStreamShowCursor : @NO,
                             (__bridge NSString *)kCGDisplayStreamMinimumFrameTime : @0 };
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CGDisplayStreamRef ds = CGDisplayStreamCreateWithDispatchQueue(d, 640, 400, 'BGRA', (__bridge CFDictionaryRef)props, q,
        ^(CGDisplayStreamFrameStatus st, uint64_t when, IOSurfaceRef fr, CGDisplayStreamUpdateRef up) {
            if (st == kCGDisplayStreamFrameStatusFrameIdle) { gFrIdle++; return; }
            if (st == kCGDisplayStreamFrameStatusFrameBlank) { gFrBlank++; return; }
            if (st == kCGDisplayStreamFrameStatusStopped) { gFrStopped++; return; }
            if (st != kCGDisplayStreamFrameStatusFrameComplete) return;
            uint64_t t = mach_absolute_time();
            if (gFrUpLast) {
                double ms = frMs(t - gFrUpLast);
                if (ms > gFrUpMaxSec) gFrUpMaxSec = ms;
                if (ms > gFrUpMax) gFrUpMax = ms;
                if (ms > 50 && ms < 1000) gFrStut++;
            }
            gFrUpLast = t;
            __atomic_add_fetch(&gFrUp, 1, __ATOMIC_RELAXED); __atomic_add_fetch(&gFrUpSec, 1, __ATOMIC_RELAXED);
        });
    CGError se = ds ? CGDisplayStreamStart(ds) : kCGErrorFailure;
#pragma clang diagnostic pop
    if (!ds || se != kCGErrorSuccess)
        printf("screen updates: cannot be counted (error %d). Allow Terminal in System Settings > Privacy & Security >\n"
               "    Screen & System Audio Recording, quit Terminal (Cmd+Q), open it again and rerun.\n", ds ? se : -1);
    printf("\nStart dragging a window around now, and keep moving it for the next 12 seconds...\n\n");
    sleep(2);
    __atomic_store_n(&gFrUpSec, 0, __ATOMIC_RELAXED); __atomic_store_n(&gFrDLsec, 0, __ATOMIC_RELAXED);
    gFrUp = gFrDL = gFrStut = gFrIdle = gFrBlank = gFrStopped = 0; gFrUpMax = 0; gFrDLMin = 1e9; gFrDLMax = 0;
    for (int s = 1; s <= 10; s++) {
        gFrUpMaxSec = 0;
        sleep(1);
        long up = (long)__atomic_exchange_n(&gFrUpSec, 0, __ATOMIC_RELAXED), tk = (long)__atomic_exchange_n(&gFrDLsec, 0, __ATOMIC_RELAXED);
        printf("second %2d: screen updated %3ld times (longest gap %4.0f ms) | refresh clock %3ld ticks\n", s, up, gFrUpMaxSec, tk);
        fflush(stdout);
    }
    if (dl) { CVDisplayLinkStop(dl); CVDisplayLinkRelease(dl); }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    if (ds) { CGDisplayStreamStop(ds); CFRelease(ds); }
#pragma clang diagnostic pop
    printf("\nsummary: screen %.1f updates/s (longest gap %.0f ms, %lld gaps over 50 ms) | refresh clock %.1f ticks/s",
           gFrUp / 10.0, gFrUpMax, (long long)gFrStut, gFrDL / 10.0);
    if (gFrDL) printf(" (between ticks %.1f to %.1f ms)", gFrDLMin, gFrDLMax);
    printf("\n  stream messages: %lld idle (screen unchanged), %lld blank, %lld stopped", (long long)gFrIdle, (long long)gFrBlank, (long long)gFrStopped);
    printf("\n  smooth would be: about 60 updates/s while moving, refresh clock 60 ticks/s, ticks 16.7 ms apart\n");
    return 0;
}

// v0.51.38: --latency: how long one small GPU job takes from commit until the app is told it is done. WindowServer
// waits for that every frame, so a slow "done" signal limits the frame rate even when the GPU itself is fast.
static int dblCmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static void latReport(const char *what, double *v, int n) {
    qsort(v, n, sizeof(double), dblCmp);
    double sum = 0; for (int i = 0; i < n; i++) sum += v[i];
    printf("    %-44s avg %6.2f ms  median %6.2f  fastest %6.2f  slowest %7.2f  (max %.0f per second)\n",
           what, sum / n, v[n / 2], v[0], v[n - 1], sum > 0 ? 1000.0 * n / sum : 0);
}
static int latencyTest(void) {
    printf("--latency: commit -> done, for small GPU jobs like WindowServer's (60 frames/s needs well under 16.7 ms)\n\n");
    @autoreleasepool {
        id<MTLDevice> d = ourDevice();
        if (!d) { printf("    no IrisXe device\n"); return 2; }
        id<MTLCommandQueue> q = [d newCommandQueue];
        enum { N = 200 };
        static double v[N];
        // 1. an empty command buffer
        for (int i = 0; i < N; i++) @autoreleasepool {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            uint64_t t = mach_absolute_time(); [cb commit]; [cb waitUntilCompleted]; v[i] = frMs(mach_absolute_time() - t);
        }
        latReport("empty command buffer", v, N);
        // 2. one small blit (fill 4 KB)
        id<MTLBuffer> buf = [d newBufferWithLength:4096 options:MTLResourceStorageModeShared];
        for (int i = 0; i < N; i++) @autoreleasepool {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
            [be fillBuffer:buf range:NSMakeRange(0, 4096) value:(uint8_t)i]; [be endEncoding];
            uint64_t t = mach_absolute_time(); [cb commit]; [cb waitUntilCompleted]; v[i] = frMs(mach_absolute_time() - t);
        }
        latReport("one blit (fill 4 KB)", v, N);
        // 3. one render pass that clears a 512x512 picture (a draw on the GPU)
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:512 height:512 mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        id<MTLTexture> tex = [d newTextureWithDescriptor:td];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = tex; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        int bad = 0;
        for (int i = 0; i < N; i++) @autoreleasepool {
            rp.colorAttachments[0].clearColor = MTLClearColorMake((i & 1), 0.5, 0.25, 1);
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp]; [re endEncoding];
            uint64_t t = mach_absolute_time(); [cb commit]; [cb waitUntilCompleted]; v[i] = frMs(mach_absolute_time() - t);
            if (cb.status != MTLCommandBufferStatusCompleted) bad++;
        }
        latReport("one render pass (clear 512x512)", v, N);
        // 4. the same, but told through a completion handler (how WindowServer is told)
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        for (int i = 0; i < N; i++) @autoreleasepool {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp]; [re endEncoding];
            __block uint64_t done = 0;
            [cb addCompletedHandler:^(id<MTLCommandBuffer> c) { done = mach_absolute_time(); dispatch_semaphore_signal(sem); }];
            uint64_t t = mach_absolute_time(); [cb commit];
            dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));
            v[i] = done ? frMs(done - t) : 2000;
        }
        latReport("render pass, completion handler", v, N);
        // 5. 120 frames sent back to back without waiting (the GPU can overlap them): frames per second
        uint64_t t0 = mach_absolute_time();
        id<MTLCommandBuffer> last = nil;
        for (int i = 0; i < 120; i++) @autoreleasepool {
            id<MTLCommandBuffer> cb = [[q commandBuffer] retain];
            id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp]; [re endEncoding];
            [cb commit];
            if (last) [last release];
            last = cb;
        }
        [last waitUntilCompleted]; [last release];
        double ms = frMs(mach_absolute_time() - t0);
        printf("    %-44s %6.1f ms in total = %.0f per second\n", "120 render passes back to back", ms, ms > 0 ? 120000.0 / ms : 0);
        if (bad) printf("    (%d render passes did not complete normally)\n", bad);
        printf("\n  each line: if \"median\" is far above \"fastest\", the GPU is quick but the done signal comes late\n");
    }
    return 0;
}
// v0.51.26: --bundle PATH (anywhere on the command line): load our Metal driver from PATH (e.g. the unzipped build
// in Downloads) before Metal loads the installed one, so a new build is tested without installing it. Our bundle
// registers its classes only once per process, so the first one loaded is used. Children inherit IRISXE_BUNDLE.
static void loadTestBundle(void) {
    const char *p = getenv("IRISXE_BUNDLE");
    if (!p || !*p) return;
    NSBundle *b = [NSBundle bundleWithPath:@(p)];
    NSError *err = nil;
    if (!b || ![b loadAndReturnError:&err]) { fprintf(stderr, "--bundle %s: not loaded (%s)\n", p, err ? err.localizedDescription.UTF8String : "no bundle there"); return; }
    static int once;
    if (!once++ && !getenv("IRISXE_BUNDLE_QUIET")) {
        printf("(testing the driver bundle at %s, version %s - not the installed one)\n\n", p,
               [[b objectForInfoDictionaryKey:@"CFBundleShortVersionString"] UTF8String] ?: "?");
        setenv("IRISXE_BUNDLE_QUIET", "1", 1);
    }
}

// v0.54.3: --display-accel - which GPU accelerator WindowServer gave each display. SkyLight picks the screen's
// compositor with it: accelerator 0 -> CPU compositor (CompositorSW); otherwise the Metal device whose
// acceleratorPort equals it. The numbers are WindowServer's own port names: compare them with the
// "initWithAcceleratorPort:N" line our Metal driver logs in WindowServer.
static int displayAccelTest(void) {
    void *sl = dlopen("/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight", RTLD_LAZY);
    typedef int (*AccFn)(uint32_t, uint32_t *, uint32_t *);
    AccFn acc = sl ? (AccFn)dlsym(sl, "SLSAcceleratorForDisplayNumber") : NULL;
    if (!acc) { printf("SLSAcceleratorForDisplayNumber not found\n"); return 1; }
    CGDirectDisplayID ids[16]; uint32_t n = 0;
    CGGetActiveDisplayList(16, ids, &n);
    printf("--display-accel: %u active display(s), main display 0x%x\n", n, CGMainDisplayID());
    for (uint32_t i = 0; i < n; i++) {
        uint32_t port = 0, index = 0;
        int err = acc(ids[i], &port, &index);
        printf("display 0x%x: error %d, accelerator %u (0x%x), framebuffer index %u -> %s\n", ids[i], err, port, port, index,
               port ? "WindowServer can pick a Metal compositor for it" : "0 = CPU compositor (CompositorSW) for this display");
        if (port) {   // v0.54.5: port numbers differ between processes; the registry ID is what can be compared
            uint64_t rid = 0; io_name_t cls = "?";
            kern_return_t k1 = IORegistryEntryGetRegistryEntryID(port, &rid);
            kern_return_t k2 = IOObjectGetClass(port, cls);
            printf("    that accelerator is class %s, registry ID 0x%llx (err 0x%x/0x%x)\n", cls, rid, k1, k2);
        }
    }
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    if (dev) printf("this process's Metal device: %s, registry ID 0x%llx\n", dev.name.UTF8String, (unsigned long long)dev.registryID);
    printf("same registry ID as WindowServer's line 'initWithAcceleratorPort N: class IrisXeAccelerator ... registry ID' = same GPU object\n");
    return 0;
}

int main(int argc, const char **argv) {
    {   // v0.51.26: --bundle PATH
        static const char *nv[64]; int n = 0;
        for (int i = 0; i < argc && n < 63; i++) {
            if (!strcmp(argv[i], "--bundle") && i + 1 < argc) {
                char rp[4096];
                setenv("IRISXE_BUNDLE", realpath(argv[i + 1], rp) ? rp : argv[i + 1], 1);
                i++; continue;
            }
            nv[n++] = argv[i];
        }
        nv[n] = NULL; argc = n; argv = nv;
        loadTestBundle();
    }
    if (argc > 2 && !strcmp(argv[1], "--child")) return child(argv[2]);
    if (argc > 1 && !strcmp(argv[1], "--list")) return listOne();
    if (argc > 1 && !strcmp(argv[1], "--buffers")) return bufferTest();
    if (argc > 1 && !strcmp(argv[1], "--mpsblur")) return mpsBlurChild();
    if (argc > 1 && !strcmp(argv[1], "--surfpaint")) return surfPainter();
    if (argc > 2 && !strcmp(argv[1], "--replaybatch")) {
        printf("--replaybatch: the recorded draws again, batched like the desktop (16 draws per GPU job)\n\n");
        return replayBatch(argv[2]);
    }
    if (argc > 2 && !strcmp(argv[1], "--replaycheck")) {
        printf("--replaycheck: the recorded draws again, through our driver only - what each one leaves in its picture\n\n");
        return replayCheck(argv[2]);
    }
    if (argc > 1 && !strcmp(argv[1], "--framerate")) return frameRateTest();   // v0.51.38
    if (argc > 1 && !strcmp(argv[1], "--fillrate")) return fillRateTest();     // v0.57.1
    if (argc > 1 && !strcmp(argv[1], "--blittest")) return blitVariantTest();   // v0.57.10
    if (argc > 1 && !strcmp(argv[1], "--pixelrate")) return pixelRateTest();    // v0.58.1
    if (argc > 1 && !strcmp(argv[1], "--mixreset")) return mixReset();          // v0.58.3
    if (argc > 1 && !strcmp(argv[1], "--bigalloc")) return bigAllocTest();      // v0.58.7
    if (argc > 1 && !strcmp(argv[1], "--display-accel")) return displayAccelTest();   // v0.54.3
    if (argc > 1 && !strcmp(argv[1], "--latency")) return latencyTest();       // v0.51.38
    if (argc > 1 && !strcmp(argv[1], "--managed")) {
        printf("--managed: buffers with Managed storage (Core Animation's vertex buffers)\n\n");
        return managedTest();
    }
    if (argc > 1 && (!strcmp(argv[1], "--surfread") || !strcmp(argv[1], "--surface"))) {
        char self[4096]; uint32_t sz = sizeof(self);
        if (_NSGetExecutablePath(self, &sz) != 0) strcpy(self, argv[0]);
        return !strcmp(argv[1], "--surface") ? surfaceTest(self) : surfReader(self);
    }
    if (argc > 1 && (!strcmp(argv[1], "--camasks") || !strcmp(argv[1], "--camaskchild"))) {
        char self[4096]; uint32_t sz = sizeof(self);
        if (_NSGetExecutablePath(self, &sz) != 0) strcpy(self, argv[0]);
        if (!strcmp(argv[1], "--camaskchild")) return caMaskChild(argc > 2 ? atoi(argv[2]) : 0);
        return caMaskTest(self);
    }
    if (argc > 1 && (!strcmp(argv[1], "--carender") || !strcmp(argv[1], "--carenderchild"))) {
        char self[4096]; uint32_t sz = sizeof(self);
        if (_NSGetExecutablePath(self, &sz) != 0) strcpy(self, argv[0]);
        if (!strcmp(argv[1], "--carenderchild")) return caRenderTest(self);
        printf("--carender: Core Animation's renderer (what WindowServer composites with) on our GPU\n\n");
        int r = runChild(self, "--carenderchild", "");
        if (r < 0) printf("    (the test process did not finish normally)\n");
        return r;
    }
    if (argc > 1 && !strcmp(argv[1], "--mps")) {
        char self[4096]; uint32_t sz = sizeof(self);
        if (_NSGetExecutablePath(self, &sz) != 0) strcpy(self, argv[0]);
        return mpsTest(self);
    }
    if (argc > 1 && !strcmp(argv[1], "--queue")) return queueTest();
    if (argc > 1 && !strcmp(argv[1], "--fill")) return fillTest();
    if (argc > 1 && !strcmp(argv[1], "--copy")) return copyTest();
    if (argc > 1 && !strcmp(argv[1], "--stress")) return stressTest();
    if (argc > 1 && !strcmp(argv[1], "--compute")) return computeTest();
    if (argc > 1 && !strcmp(argv[1], "--shaders")) return shaderTest();
    if (argc > 1 && !strcmp(argv[1], "--branches")) return branchTest();
    if (argc > 1 && !strcmp(argv[1], "--types")) return typesTest();
    if (argc > 1 && !strcmp(argv[1], "--sync")) return syncTest();
    if (argc > 1 && !strcmp(argv[1], "--big")) return bigTest();
    if (argc > 1 && !strcmp(argv[1], "--textures")) return texturesTest();
    if (argc > 1 && !strcmp(argv[1], "--draw")) return drawTest();
    if (argc > 2 && !strcmp(argv[1], "--render")) return renderTest(argv[2]);
    if (argc > 1 && !strcmp(argv[1], "--metaldraw")) return metalDrawTest();
    if (argc > 1 && !strcmp(argv[1], "--depth")) return depthTest();
    if (argc > 1 && !strcmp(argv[1], "--mrt")) return mrtTest();
    if (argc > 1 && !strcmp(argv[1], "--texmore")) return texMoreTest();
    if (argc > 1 && !strcmp(argv[1], "--genmips")) return genMipDepthTest();
    if (argc > 1 && !strcmp(argv[1], "--window")) return windowTest();
    if (argc > 1 && !strcmp(argv[1], "--missing"))                 // --missing [program] [minutes]
        return missingReport(argc > 2 ? argv[2] : NULL, argc > 3 ? atoi(argv[3]) : 0);
    if (argc > 2 && !strcmp(argv[1], "--isolate")) return isolateTest(argv[2]);
    if (argc > 1 && !strcmp(argv[1], "--twoproc")) {
        char self[4096]; uint32_t sz = sizeof(self);
        if (_NSGetExecutablePath(self, &sz) != 0) strcpy(self, argv[0]);
        return runIsolatePair(self);
    }
    if (argc > 1 && !strcmp(argv[1], "--probe")) {
        char self[4096]; uint32_t sz = sizeof(self);
        if (_NSGetExecutablePath(self, &sz) != 0) strcpy(self, argv[0]);
        return probe(self);
    }
    @autoreleasepool {
        const char *bundle = argc > 1 ? argv[1] : "IrisXeMTLDriver.bundle";
        printf("irisxe-metaltest (v0.42.1: sample bias / level)\n\n");
        if (gpuHung()) printf("WARNING: the GPU is already hung - reboot first, the GPU tests will be skipped\n\n");
        listDevices("[1] before");

        printf("\n[2] our accelerator in the I/O registry\n");
        io_service_t svc = findAccel();
        if (!svc) {
            printf("    NOT FOUND - boot with -irisxeaccel -irisxeaccelreg\n");
        } else {
            CFTypeRef n = IORegistryEntryCreateCFProperty(svc, CFSTR("MetalPluginName"), kCFAllocatorDefault, 0);
            CFTypeRef s = IORegistryEntryCreateCFProperty(svc, CFSTR("IrisXe-AccelStart"), kCFAllocatorDefault, 0);
            printf("    found. MetalPluginName = %s\n", n ? [(__bridge NSString *)n UTF8String] : "(not published)");
            printf("    IrisXe-AccelStart = %s\n", s ? [(__bridge NSString *)s UTF8String] : "(none)");
            if (n) CFRelease(n);
            if (s) CFRelease(s);
            CFTypeRef dl = IORegistryEntryCreateCFProperty(svc, CFSTR("IrisXe-DisplayLink"), kCFAllocatorDefault, 0);
            printf("    IrisXe-DisplayLink = %s\n", dl ? [(__bridge NSString *)dl UTF8String] : "(none: IrisXeAccel older than 0.40)");
            if (dl) CFRelease(dl);
            id<MTLDevice> def = MTLCreateSystemDefaultDevice();
            printf("    MTLCreateSystemDefaultDevice (the main display's GPU) -> %s\n", def ? class_getName(object_getClass(def)) : "nil");
            [def release];
            IOObjectRelease(svc);
        }

        printf("\n[3] create our Metal device by hand (bundle %s)\n", bundle);
        fflush(stdout);
        char self[4096]; uint32_t sz = sizeof(self);
        if (_NSGetExecutablePath(self, &sz) != 0) strcpy(self, argv[0]);
        char *cargv[] = { self, (char *)"--child", (char *)bundle, NULL };
        pid_t pid = 0; int st = 0;
        if (posix_spawn(&pid, self, NULL, NULL, cargv, environ) != 0) { perror("posix_spawn"); return 1; }
        waitpid(pid, &st, 0);
        if (WIFSIGNALED(st))
            printf("    CRASHED with signal %d (%s) - send the crash report too\n", WTERMSIG(st), strsignal(WTERMSIG(st)));
        else
            printf("    result code %d (0 = device created)\n", WEXITSTATUS(st));

        printf("\n");
        listDevices("[4] after");

        printf("\n[5] buffers (milestone 2a)\n");
        runChild(self, "--buffers", "");
        printf("\n[6] command queue and command buffers (milestone 2b)\n");
        runChild(self, "--queue", "");

        printf("\n[7] the GPU fills a Metal buffer (milestone 2c)\n");
        runChild(self, "--fill", "");
        printf("\n[8] the GPU copies a Metal buffer (milestone 2d)\n");
        runChild(self, "--copy", "");
        printf("\n[9] 200 buffers made, GPU-filled and freed (milestone 2d)\n");
        runChild(self, "--stress", "");
        printf("\n[10] two programs use the GPU at the same time (milestone 2e)\n");
        runIsolatePair(self);
        printf("\n[11] a compute kernel through Metal (milestone 3a)\n");
        printProp("IrisXeProbe", CFSTR("IrisXe-Compute"), "boot-time compute test");
        runChild(self, "--compute", "");
        printf("\n[12] real Metal shaders, compiled by our own compiler (milestone 3c)\n");
        runChild(self, "--shaders", "");
        printf("\n[13] Metal shaders with if/else and loops (milestone 3d)\n");
        runChild(self, "--branches", "");
        printf("\n[14] maths, division, vectors, 8/16-bit, half, bit tricks (milestone 3e)\n");
        runChild(self, "--types", "");
        printf("\n[15] threadgroup memory, barriers, atomics (milestone 3f)\n");
        runChild(self, "--sync", "");
        printf("\n[16] big kernels: register reuse, code in its own GPU buffer (v0.29.0)\n");
        runChild(self, "--big", "");
        printf("\n[17] textures in compute kernels (milestone 4)\n");
        runChild(self, "--textures", "");
        printf("\n[18] IrisXeCore draws with the 3D pipeline (milestones 5a, 5b)\n");
        if (drawTest() == 0 && !gpuHung()) {
            printf("    compute after the 3D job:\n");
            runChild(self, "--compute", "");
            printf("\n[18b] vertex + fragment functions compiled by our compiler, drawn by IrisXeCore (milestone 5b)\n");
            runChild(self, "--render", bundle);
            printf("\n[18c] drawing through Metal: render pipeline, render pass, drawPrimitives (milestone 5c)\n");
            runChild(self, "--metaldraw", "");
            printf("\n[18d] depth and stencil (v0.34)\n");
            runChild(self, "--depth", "");
            printf("\n[18e] two render targets, half output, [[position]].z, front_facing (v0.35)\n");
            runChild(self, "--mrt", "");
            printf("\n[18f] texture blits, mipmaps, more pixel formats (v0.36)\n");
            runChild(self, "--texmore", "");
            printf("\n[18g] generateMipmapsForTexture, depth textures in fragment functions (v0.37), cube textures (v0.41), derivatives (v0.42)\n");
            runChild(self, "--genmips", "");
            printf("\n[18h] a window drawn by our GPU: CAMetalLayer, nextDrawable, presentDrawable (v0.38)\n");
            runChild(self, "--window", "");
        }
        printf("\n[19] kernel counters\n");
        printProp("IrisXeAccelerator", CFSTR("IrisXe-AccelMetal"), "accelerator");
        printProp("IrisXeAccelerator", CFSTR("IrisXe-AccelQueue"), "kernel queue");
        printProp("IrisXeAccelerator", CFSTR("IrisXe-AccelMaps"), "memory maps");
        printProp("IrisXeProbe", CFSTR("IrisXe-GPUSpace"), "GPU spaces");
        printProp("IrisXeAccelerator", CFSTR("IrisXe-GPUVA"), "GPU addresses");
        printProp("IrisXeAccelerator", CFSTR("IrisXe-AccelQueuesCreated"), "kernel queues created");
        printProp("IrisXeProbe", CFSTR("IrisXe-Metal"), "GPU core");
        printProp("IrisXeProbe", CFSTR("IrisXe-GPU"), "GPU engines");
        if (gpuHung()) printProp("IrisXeProbe", CFSTR("IrisXe-Hang"), "GPU hang");

        io_service_t a2 = findAccel();
        if (a2) {
            CFTypeRef m = IORegistryEntryCreateCFProperty(a2, CFSTR("IrisXe-AccelMem"), kCFAllocatorDefault, 0);
            printf("    kernel side (IrisXe-AccelMem): %s\n", m ? [(__bridge NSString *)m UTF8String] : "(nothing created)");
            if (m) CFRelease(m);
            IOObjectRelease(a2);
        }
        return 0;
    }
}

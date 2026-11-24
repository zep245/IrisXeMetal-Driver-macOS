// irisxe-mtkdemo (v0.39.0; v0.40.2: prints what it got on frames 0 and 60, -nofbo): a small MetalKit app, written like the Xcode "Metal game" template: MTKView,
// MTLCreateSystemDefaultDevice, a depth buffer made by MetalKit, a vertex descriptor, indexed draws, a mipmapped
// texture, uniforms with setVertexBytes. It runs until you close the window (or for N seconds: irisxe-mtkdemo 10)
// and prints the frame rate every second. Resize the window: MetalKit then makes new drawables and depth buffers.
#import <AppKit/AppKit.h>
#import <MetalKit/MetalKit.h>
#import <objc/runtime.h>
#import <IOSurface/IOSurface.h>
#import <objc/message.h>
#include <mach-o/dyld.h>
#include <math.h>

typedef struct { float m[16]; } Mat4;                       // column-major, like Metal's float4x4
static Mat4 mul(Mat4 a, Mat4 b) {
    Mat4 r;
    for (int c = 0; c < 4; c++) for (int rr = 0; rr < 4; rr++) {
        float s = 0; for (int k = 0; k < 4; k++) s += a.m[k * 4 + rr] * b.m[c * 4 + k];
        r.m[c * 4 + rr] = s;
    }
    return r;
}
static Mat4 rotY(float a) { Mat4 r = {{ cosf(a),0,-sinf(a),0, 0,1,0,0, sinf(a),0,cosf(a),0, 0,0,0,1 }}; return r; }
static Mat4 rotX(float a) { Mat4 r = {{ 1,0,0,0, 0,cosf(a),sinf(a),0, 0,-sinf(a),cosf(a),0, 0,0,0,1 }}; return r; }
static Mat4 trans(float x, float y, float z) { Mat4 r = {{ 1,0,0,0, 0,1,0,0, 0,0,1,0, x,y,z,1 }}; return r; }
static Mat4 persp(float fovy, float aspect, float n, float f) {      // Metal clip space: z 0..1
    float ys = 1 / tanf(fovy * 0.5f), xs = ys / aspect, zs = f / (n - f);
    Mat4 r = {{ xs,0,0,0, 0,ys,0,0, 0,0,zs,-1, 0,0,zs * n,0 }};
    return r;
}
typedef struct { float pos[3], col[3], uv[2]; } Vtx;
typedef struct { Mat4 mvp; float tint[4]; } Uniforms;

static void makeCube(Vtx *v, uint16_t *idx) {
    // a cube: 6 faces x 4 vertices, a colour per face
    static const float face[6][3] = { {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1} };
    static const float fcol[6][3] = { {1,.4f,.4f}, {.4f,1,.4f}, {.4f,.4f,1}, {1,1,.4f}, {.4f,1,1}, {1,.4f,1} };
    for (int f = 0; f < 6; f++) {
        float n[3] = { face[f][0], face[f][1], face[f][2] }, u[3], w[3];
        int a = n[0] != 0 ? 0 : n[1] != 0 ? 1 : 2;
        u[0] = u[1] = u[2] = 0; w[0] = w[1] = w[2] = 0;
        u[(a + 1) % 3] = 1; w[(a + 2) % 3] = 1;
        if (n[a] < 0) { u[(a + 1) % 3] = -1; }
        for (int k = 0; k < 4; k++) {
            float su = (k & 1) ? 1 : -1, sw = (k & 2) ? 1 : -1;
            for (int c = 0; c < 3; c++) { v[f * 4 + k].pos[c] = n[c] + su * u[c] + sw * w[c]; v[f * 4 + k].col[c] = fcol[f][c]; }
            v[f * 4 + k].uv[0] = (k & 1) ? 1 : 0; v[f * 4 + k].uv[1] = (k & 2) ? 1 : 0;
        }
        uint16_t b = (uint16_t)(f * 4);
        uint16_t q[6] = { b, (uint16_t)(b + 1), (uint16_t)(b + 2), (uint16_t)(b + 2), (uint16_t)(b + 1), (uint16_t)(b + 3) };
        memcpy(idx + f * 6, q, sizeof(q));
    }
}

@interface DemoRenderer : NSObject <MTKViewDelegate>
@property (nonatomic, retain) id<MTLDevice> device;
@property (nonatomic, retain) id<MTLCommandQueue> queue;
@property (nonatomic, retain) id<MTLRenderPipelineState> pipe;
@property (nonatomic, retain) id<MTLDepthStencilState> depth;
@property (nonatomic, retain) id<MTLBuffer> vb, ib;
@property (nonatomic, retain) id<MTLTexture> tex;
@property (nonatomic, retain) id<MTLSamplerState> smp;
@property (nonatomic) unsigned frames, total;
@property (nonatomic) double t0, last;
@property (nonatomic) double runFor;
@property (nonatomic) BOOL noFBO;
@end

@implementation DemoRenderer
- (BOOL)setUp:(MTKView *)view {
    id<MTLDevice> d = view.device;
    char exe[1024]; uint32_t sz = sizeof(exe);
    _NSGetExecutablePath(exe, &sz);
    NSString *dir = [[NSString stringWithUTF8String:exe] stringByDeletingLastPathComponent];
    NSError *e = nil;
    id<MTLLibrary> lib = [d newLibraryWithURL:[NSURL fileURLWithPath:[dir stringByAppendingPathComponent:@"irisxe-mtkdemo.metallib"]] error:&e];
    if (!lib) { printf("shaders: %s\n", e.localizedDescription.UTF8String); return NO; }
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    vd.attributes[0].format = MTLVertexFormatFloat3; vd.attributes[0].offset = 0;  vd.attributes[0].bufferIndex = 0;
    vd.attributes[1].format = MTLVertexFormatFloat3; vd.attributes[1].offset = 12; vd.attributes[1].bufferIndex = 0;
    vd.attributes[2].format = MTLVertexFormatFloat2; vd.attributes[2].offset = 24; vd.attributes[2].bufferIndex = 0;
    vd.layouts[0].stride = sizeof(Vtx);
    MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor new] autorelease];
    pd.vertexFunction = [[lib newFunctionWithName:@"demo_vs"] autorelease];
    pd.fragmentFunction = [[lib newFunctionWithName:@"demo_fs"] autorelease];
    pd.vertexDescriptor = vd;
    pd.colorAttachments[0].pixelFormat = view.colorPixelFormat;
    pd.depthAttachmentPixelFormat = view.depthStencilPixelFormat;
    [lib release];
    self.pipe = [[d newRenderPipelineStateWithDescriptor:pd error:&e] autorelease];
    if (!self.pipe) {
        printf("render pipeline: %s\n", e.localizedDescription.UTF8String);
        if ([(id)d respondsToSelector:sel_registerName("irisxeLastRenderLog")]) {
            NSString *log = ((id (*)(id, SEL))objc_msgSend)((id)d, sel_registerName("irisxeLastRenderLog"));
            printf("  %s\n", log.UTF8String);
        }
        return NO;
    }
    MTLDepthStencilDescriptor *dd = [[MTLDepthStencilDescriptor new] autorelease];
    dd.depthCompareFunction = MTLCompareFunctionLess; dd.depthWriteEnabled = YES;
    self.depth = [[d newDepthStencilStateWithDescriptor:dd] autorelease];
    Vtx v[24]; uint16_t idx[36];
    makeCube(v, idx);
    self.vb = [[d newBufferWithBytes:v length:sizeof(v) options:MTLResourceStorageModeShared] autorelease];
    self.ib = [[d newBufferWithBytes:idx length:sizeof(idx) options:MTLResourceStorageModeShared] autorelease];
    // a checkerboard with mipmaps (generated on the GPU)
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:128 height:128 mipmapped:YES];
    td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    self.tex = [[d newTextureWithDescriptor:td] autorelease];
    static uint32_t px[128 * 128];
    for (int y = 0; y < 128; y++) for (int x = 0; x < 128; x++) px[y * 128 + x] = (((x >> 4) ^ (y >> 4)) & 1) ? 0xFFFFFFFFu : 0xFF606060u;
    [self.tex replaceRegion:MTLRegionMake2D(0, 0, 128, 128) mipmapLevel:0 withBytes:px bytesPerRow:512];
    MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor new] autorelease];
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear; sd.mipFilter = MTLSamplerMipFilterLinear;
    sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeRepeat;
    self.smp = [[d newSamplerStateWithDescriptor:sd] autorelease];
    self.queue = [[d newCommandQueue] autorelease];
    id<MTLCommandBuffer> cb = [self.queue commandBuffer];
    id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
    [be generateMipmapsForTexture:self.tex];
    [be endEncoding];
    [cb commit];
    self.t0 = self.last = CACurrentMediaTime();
    return YES;
}
// v0.40.2: why is the window black? Print what MetalKit gave us and whether the GPU work really happened.
static const char *cls(id o) { return o ? class_getName(object_getClass(o)) : "nil"; }
- (void)report:(MTKView *)view drawable:(id<CAMetalDrawable>)dr pass:(MTLRenderPassDescriptor *)rp
        buffer:(id<MTLCommandBuffer>)cb frame:(unsigned)n {
    id<MTLTexture> t = dr.texture, dt = rp.depthAttachment.texture;
    CAMetalLayer *ml = (CAMetalLayer *)view.layer;
    printf("[frame %u] layer %s, layer device %s, framebufferOnly %d, pixel format %lu\n", n, cls(ml),
           [ml respondsToSelector:@selector(device)] ? cls(ml.device) : "?", (int)view.framebufferOnly, (unsigned long)view.colorPixelFormat);
    printf("[frame %u] drawable %s, color texture %s %lux%lu format %lu usage 0x%lx IOSurface %s\n", n, cls(dr), cls(t),
           (unsigned long)t.width, (unsigned long)t.height, (unsigned long)t.pixelFormat, (unsigned long)t.usage,
           t.iosurface ? "yes" : "NO");
    printf("[frame %u] depth texture %s %lux%lu format %lu storage %lu; color load %lu store %lu clear %.2f %.2f %.2f\n", n, cls(dt),
           (unsigned long)dt.width, (unsigned long)dt.height, (unsigned long)dt.pixelFormat, (unsigned long)dt.storageMode,
           (unsigned long)rp.colorAttachments[0].loadAction, (unsigned long)rp.colorAttachments[0].storeAction,
           rp.colorAttachments[0].clearColor.red, rp.colorAttachments[0].clearColor.green, rp.colorAttachments[0].clearColor.blue);
    fflush(stdout);
    [cb addCompletedHandler:^(id<MTLCommandBuffer> b) {
        long st = [(id)b respondsToSelector:@selector(status)] ? (long)b.status : -1;
        NSError *err = [(id)b respondsToSelector:@selector(error)] ? b.error : nil;
        printf("[frame %u] GPU done: status %ld (4 = completed, 5 = error) %s\n", n, st, err ? err.localizedDescription.UTF8String : "");
        uint32_t px = 0xDEADBEEF, sp = 0xDEADBEEF;
        NSUInteger cx = t.width / 2, cy = t.height / 2;
        @try { [t getBytes:&px bytesPerRow:4 fromRegion:MTLRegionMake2D(cx, cy, 1, 1) mipmapLevel:0]; } @catch (id ex) {}
        uint32_t cx0 = 0xDEADBEEF;
        @try { [t getBytes:&cx0 bytesPerRow:4 fromRegion:MTLRegionMake2D(2, 2, 1, 1) mipmapLevel:0]; } @catch (id ex) {}
        IOSurfaceRef s = t.iosurface;
        if (s) {
            usleep(20000);
            IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL);
            const uint8_t *base = IOSurfaceGetBaseAddress(s);
            if (base) sp = *(const uint32_t *)(base + cy * IOSurfaceGetBytesPerRow(s) + cx * 4);
            IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
        }
        printf("[frame %u] pixels (BGRA as 0xAARRGGBB): texture corner 0x%08x, centre 0x%08x, IOSurface centre 0x%08x\n", n, cx0, px, sp);
        printf("           (the background should be about 0xff1f2433; 0x00000000 = nothing was drawn)\n");
        fflush(stdout);
    }];
}
- (void)mtkView:(MTKView *)view drawableSizeWillChange:(CGSize)size {
    printf("drawable size %.0f x %.0f\n", size.width, size.height);
    fflush(stdout);
}
// v0.50.1: where a frame's time goes (printed with the frames per second)
static double gWaitDr, gEncode, gCommit, gDone; static int gDoneN;
- (void)drawInMTKView:(MTKView *)view {
    @autoreleasepool {
        double tA = CACurrentMediaTime();
        MTLRenderPassDescriptor *rp = view.currentRenderPassDescriptor;
        id<CAMetalDrawable> dr = view.currentDrawable;
        double tB = CACurrentMediaTime();
        gWaitDr += tB - tA;
        if (!rp || !dr) return;
        double t = CACurrentMediaTime() - self.t0;
        CGSize s = view.drawableSize;
        Uniforms u;
        u.mvp = mul(persp(1.0f, s.width / (s.height > 0 ? s.height : 1), 0.1f, 100.0f),
                    mul(trans(0, 0, -5.5f), mul(rotX((float)t * 0.7f), rotY((float)t))));
        u.tint[0] = u.tint[1] = u.tint[2] = u.tint[3] = 1;
        id<MTLCommandBuffer> cb = [self.queue commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        [enc setRenderPipelineState:self.pipe];
        [enc setDepthStencilState:self.depth];
        [enc setVertexBuffer:self.vb offset:0 atIndex:0];
        [enc setVertexBytes:&u length:sizeof(u) atIndex:1];
        [enc setFragmentTexture:self.tex atIndex:0];
        [enc setFragmentSamplerState:self.smp atIndex:0];
        [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:36 indexType:MTLIndexTypeUInt16 indexBuffer:self.ib indexBufferOffset:0];
        [enc endEncoding];
        unsigned n = self.total;
        if (n == 0 || n == 60) [self report:view drawable:dr pass:rp buffer:cb frame:n];
        [cb presentDrawable:dr];
        double tC = CACurrentMediaTime();
        [cb addCompletedHandler:^(id<MTLCommandBuffer> b) { gDone += CACurrentMediaTime() - tC; gDoneN++; }];
        [cb commit];
        double tD = CACurrentMediaTime();
        gEncode += tC - tB; gCommit += tD - tC;
        self.frames++; self.total++;
        double now = CACurrentMediaTime();
        if (now - self.last >= 1.0) {
            double f = self.frames ? self.frames : 1;
            printf("%.0f s: %.0f frames per second (%.0f x %.0f) | per frame: waiting for a drawable %.1f ms, encoding %.1f ms, "
                   "commit %.1f ms, GPU done %.1f ms after commit\n", now - self.t0, self.frames / (now - self.last), s.width, s.height,
                   gWaitDr * 1000 / f, gEncode * 1000 / f, gCommit * 1000 / f, gDoneN ? gDone * 1000 / gDoneN : 0);
            fflush(stdout);
            self.frames = 0; self.last = now;
            gWaitDr = gEncode = gCommit = gDone = 0; gDoneN = 0;
        }
        if (self.runFor > 0 && now - self.t0 >= self.runFor) [NSApp terminate:nil];
    }
}
@end


// ---- v0.40.3: --steps (v0.40.4: the hang check finds IrisXeProbe like irisxe-metaltest does): the demo's GPU work one piece at a time, waiting after each, stopping at a GPU hang ----
#include <IOKit/IOKitLib.h>
static io_service_t findProbe(void) {                     // a sibling of the accelerator (not matchable by class)
    io_service_t a = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IrisXeAccelerator"));
    io_registry_entry_t pci = 0, child = 0;
    io_iterator_t it = 0;
    io_service_t found = 0;
    if (a && IORegistryEntryGetParentEntry(a, kIOServicePlane, &pci) == KERN_SUCCESS &&
        IORegistryEntryGetChildIterator(pci, kIOServicePlane, &it) == KERN_SUCCESS) {
        while ((child = IOIteratorNext(it))) {
            io_name_t c;
            if (!found && IOObjectGetClass(child, c) == KERN_SUCCESS && !strcmp(c, "IrisXeProbe")) { found = child; continue; }
            IOObjectRelease(child);
        }
        IOObjectRelease(it);
    }
    if (pci) IOObjectRelease(pci);
    if (a) IOObjectRelease(a);
    if (!found) found = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IrisXeProbe"));
    return found;
}
static NSString *probeProp(CFStringRef key) {
    io_service_t s = findProbe();
    if (!s) return @"(IrisXeProbe not found)";
    CFTypeRef v = IORegistryEntryCreateCFProperty(s, key, kCFAllocatorDefault, 0);
    IOObjectRelease(s);
    NSString *r = v && CFGetTypeID(v) == CFStringGetTypeID() ? [[(__bridge NSString *)v copy] autorelease] : nil;
    if (v) CFRelease(v);
    return r;
}
static bool hung(void) {
    for (int t = 0; t < 15; t++) {                       // the status is refreshed by a heartbeat
        if ([probeProp(CFSTR("IrisXe-GPU")) hasPrefix:@"HUNG"]) return true;
        usleep(100000);
    }
    return false;
}
static id<MTLTexture> newRT(id<MTLDevice> d, MTLPixelFormat pf, NSUInteger w, NSUInteger h, bool priv) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:w height:h mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    if (priv) td.storageMode = MTLStorageModePrivate;
    return [d newTextureWithDescriptor:td];
}
static uint32_t pixelAt(id<MTLTexture> t, NSUInteger x, NSUInteger y) {
    uint32_t p = 0xDEADBEEF;
    [t getBytes:&p bytesPerRow:4 fromRegion:MTLRegionMake2D(x, y, 1, 1) mipmapLevel:0];
    return p;
}
static int runSteps(id<MTLDevice> d, bool bigFirst) {
    char exe[1024]; uint32_t sz = sizeof(exe);
    _NSGetExecutablePath(exe, &sz);
    NSString *dir = [[NSString stringWithUTF8String:exe] stringByDeletingLastPathComponent];
    NSError *e = nil;
    if (hung()) { printf("the GPU is ALREADY hung: reboot first\n"); return 1; }
    id<MTLLibrary> lib = [d newLibraryWithURL:[NSURL fileURLWithPath:[dir stringByAppendingPathComponent:@"irisxe-mtkdemo.metallib"]] error:&e];
    if (!lib) { printf("shaders: %s\n", e.localizedDescription.UTF8String); return 1; }
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    vd.attributes[0].format = MTLVertexFormatFloat3; vd.attributes[0].offset = 0;  vd.attributes[0].bufferIndex = 0;
    vd.attributes[1].format = MTLVertexFormatFloat3; vd.attributes[1].offset = 12; vd.attributes[1].bufferIndex = 0;
    vd.attributes[2].format = MTLVertexFormatFloat2; vd.attributes[2].offset = 24; vd.attributes[2].bufferIndex = 0;
    vd.layouts[0].stride = sizeof(Vtx);
    id<MTLRenderPipelineState> pipes[3];                  // plain, plain + depth, textured + depth
    for (int i = 0; i < 3; i++) {
        MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor new] autorelease];
        pd.vertexFunction = [[lib newFunctionWithName:@"demo_vs"] autorelease];
        pd.fragmentFunction = [[lib newFunctionWithName:i == 2 ? @"demo_fs" : @"demo_fs_plain"] autorelease];
        pd.vertexDescriptor = vd;
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        if (i) pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        pipes[i] = [d newRenderPipelineStateWithDescriptor:pd error:&e];
        if (!pipes[i]) { printf("pipeline %d: %s\n", i, e.localizedDescription.UTF8String); return 1; }
    }
    MTLDepthStencilDescriptor *dd = [[MTLDepthStencilDescriptor new] autorelease];
    dd.depthCompareFunction = MTLCompareFunctionLess; dd.depthWriteEnabled = YES;
    id<MTLDepthStencilState> dss = [d newDepthStencilStateWithDescriptor:dd];
    Vtx v[24]; uint16_t idx[36];
    makeCube(v, idx);
    id<MTLBuffer> vb = [d newBufferWithBytes:v length:sizeof(v) options:MTLResourceStorageModeShared];
    id<MTLBuffer> ib = [d newBufferWithBytes:idx length:sizeof(idx) options:MTLResourceStorageModeShared];
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:128 height:128 mipmapped:YES];
    td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    id<MTLTexture> tex = [d newTextureWithDescriptor:td];
    static uint32_t px[128 * 128];
    for (int y = 0; y < 128; y++) for (int x = 0; x < 128; x++) px[y * 128 + x] = (((x >> 4) ^ (y >> 4)) & 1) ? 0xFFFFFFFFu : 0xFF606060u;
    [tex replaceRegion:MTLRegionMake2D(0, 0, 128, 128) mipmapLevel:0 withBytes:px bytesPerRow:512];
    MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor new] autorelease];
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear; sd.mipFilter = MTLSamplerMipFilterLinear;
    sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeRepeat;
    id<MTLSamplerState> smp = [d newSamplerStateWithDescriptor:sd];
    id<MTLCommandQueue> q = [d newCommandQueue];
    struct { const char *what; unsigned w, h; int pipe; bool depth, mips; } st[] = {
        { "clear only, 256 x 256",                         256,  256, -1, false, false },
        { "clear only, 1280 x 960",                       1280,  960, -1, false, false },
        { "clear colour + depth, 1280 x 960",             1280,  960, -1, true,  false },
        { "cube, plain colours, 256 x 256",                256,  256,  0, false, false },
        { "cube, plain colours, 1280 x 960",              1280,  960,  0, false, false },
        { "cube, plain colours + depth, 1280 x 960",      1280,  960,  1, true,  false },
        { "generateMipmaps of the 128 x 128 texture",      128,  128, -2, false, true  },
        { "cube, textured + depth, 256 x 256",             256,  256,  2, true,  false },
        { "cube, textured + depth, 1280 x 960 (= the demo)", 1280, 960, 2, true, false },
        { "cube, textured + depth, 1516 x 962 (an odd window size)", 1516, 962, 2, true, false },
    };
    int n = sizeof(st) / sizeof(st[0]);
    int order[16];
    for (int i = 0; i < n; i++) order[i] = i;
    if (bigFirst) { order[0] = 1; order[1] = 0; }          // v0.40.5: the 1280 x 960 clear as the process's first GPU job
    for (int oi = 0; oi < n; oi++) {
        int i = order[oi];
        @autoreleasepool {
            printf("[step %d] %s ... ", i + 1, st[i].what);
            fflush(stdout);
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLTexture> rt = nil, dt = nil;
            if (st[i].pipe == -2) {
                id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
                [be generateMipmapsForTexture:tex];
                [be endEncoding];
            } else {
                rt = [newRT(d, MTLPixelFormatBGRA8Unorm, st[i].w, st[i].h, false) autorelease];
                if (st[i].depth) dt = [newRT(d, MTLPixelFormatDepth32Float, st[i].w, st[i].h, true) autorelease];
                MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                rp.colorAttachments[0].texture = rt;
                rp.colorAttachments[0].loadAction = MTLLoadActionClear;
                rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                rp.colorAttachments[0].clearColor = MTLClearColorMake(0.12, 0.14, 0.2, 1);
                if (dt) {
                    rp.depthAttachment.texture = dt;
                    rp.depthAttachment.loadAction = MTLLoadActionClear;
                    rp.depthAttachment.storeAction = MTLStoreActionDontCare;
                    rp.depthAttachment.clearDepth = 1.0;
                }
                id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
                if (st[i].pipe >= 0) {
                    Uniforms u;
                    u.mvp = mul(persp(1.0f, (float)st[i].w / st[i].h, 0.1f, 100.0f), mul(trans(0, 0, -5.5f), mul(rotX(0.5f), rotY(0.6f))));
                    u.tint[0] = u.tint[1] = u.tint[2] = u.tint[3] = 1;
                    [enc setRenderPipelineState:pipes[st[i].pipe]];
                    if (st[i].depth) [enc setDepthStencilState:dss];
                    [enc setVertexBuffer:vb offset:0 atIndex:0];
                    [enc setVertexBytes:&u length:sizeof(u) atIndex:1];
                    [enc setFragmentTexture:tex atIndex:0];
                    [enc setFragmentSamplerState:smp atIndex:0];
                    [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:36 indexType:MTLIndexTypeUInt16 indexBuffer:ib indexBufferOffset:0];
                }
                [enc endEncoding];
            }
            [cb commit];
            [cb waitUntilCompleted];
            if (hung()) {
                printf("GPU HUNG\n    %s\n", probeProp(CFSTR("IrisXe-Hang")).UTF8String ?: "(no details)");
                printf("stopped at step %d: reboot before running anything else\n", i + 1);
                return 2;
            }
            if (rt) printf("ok, pixels: corner 0x%08x centre 0x%08x\n", pixelAt(rt, 2, 2), pixelAt(rt, st[i].w / 2, st[i].h / 2));
            else printf("ok, level 3 pixel 0x%08x\n", ({ uint32_t p = 0; [tex getBytes:&p bytesPerRow:64 fromRegion:MTLRegionMake2D(3, 3, 1, 1) mipmapLevel:3]; p; }));
            fflush(stdout);
        }
    }
    printf("all steps passed (background 0xff1f2433 expected in the corners; the centre is the cube)\n");
    return 0;
}

// v0.40.6: --steps map: which change between two jobs hangs the GPU? (v0.40.5: the first job always works, the
// second hangs at its first PIPE_CONTROL, whatever the size)
static bool clearInto(id<MTLCommandQueue> q, id<MTLTexture> t, int step, const char *what) {
    printf("[map %d] %s ... ", step, what);
    fflush(stdout);
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = t;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0.12, 0.14, 0.2 + 0.1 * step, 1);
    id<MTLCommandBuffer> cb = [q commandBuffer];
    [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    if (hung()) {
        printf("GPU HUNG\n    %s\nstopped: reboot before running anything else\n", probeProp(CFSTR("IrisXe-Hang")).UTF8String ?: "");
        return false;
    }
    printf("ok, pixel 0x%08x\n", pixelAt(t, 2, 2));
    fflush(stdout);
    return true;
}
static int runMapSteps(id<MTLDevice> d) {
    if (hung()) { printf("the GPU is ALREADY hung: reboot first\n"); return 1; }
    id<MTLCommandQueue> q = [d newCommandQueue];
    id<MTLTexture> t1 = newRT(d, MTLPixelFormatBGRA8Unorm, 256, 256, false);
    {   // step 0: three jobs straight after each other (no pause between them; every later step waits ~1.5 s first)
        printf("[map 0] clear texture A three times, no pause between the jobs ... ");
        fflush(stdout);
        id<MTLCommandBuffer> last = nil;
        for (int k = 0; k < 3; k++) {
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = t1;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(0.12, 0.14, 0.2, 1);
            id<MTLCommandBuffer> cb = [q commandBuffer];
            [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
            [cb commit];
            last = cb;
        }
        [last waitUntilCompleted];
        if (hung()) {
            printf("GPU HUNG\n    %s\nstopped: reboot before running anything else\n", probeProp(CFSTR("IrisXe-Hang")).UTF8String ?: "");
            return 2;
        }
        printf("ok, pixel 0x%08x\n", pixelAt(t1, 2, 2));
    }
    if (!clearInto(q, t1, 1, "clear texture A after a pause")) return 2;
    if (!clearInto(q, t1, 2, "clear texture A again (no new memory, nothing freed)")) return 2;
    id<MTLTexture> t2 = newRT(d, MTLPixelFormatBGRA8Unorm, 256, 256, false);
    if (!clearInto(q, t2, 3, "clear a new texture B (new memory, nothing freed)")) return 2;
    [t2 release];
    usleep(200000);
    if (!clearInto(q, t1, 4, "free B, clear A (memory freed, nothing new)")) return 2;
    id<MTLTexture> t3 = newRT(d, MTLPixelFormatBGRA8Unorm, 256, 256, false);
    if (!clearInto(q, t3, 5, "clear a new texture C (after a free: B's address used again?)")) return 2;
    printf("all map steps passed\n");
    return 0;
}

// v0.42.5: --steps ds: Maps' first map draw (hung the GPU): a 2528 x 1410 sRGB target, Depth32Float_Stencil8,
// clear colour + depth + stencil, then a draw with depth LEQUAL and stencil EQUAL 128 (mask 128). Smallest first.
static int runDSSteps(id<MTLDevice> d) {
    char exe[1024]; uint32_t sz = sizeof(exe);
    _NSGetExecutablePath(exe, &sz);
    NSString *dir = [[NSString stringWithUTF8String:exe] stringByDeletingLastPathComponent];
    NSError *e = nil;
    if (hung()) { printf("the GPU is ALREADY hung: reboot first\n"); return 1; }
    id<MTLLibrary> lib = [d newLibraryWithURL:[NSURL fileURLWithPath:[dir stringByAppendingPathComponent:@"irisxe-mtkdemo.metallib"]] error:&e];
    if (!lib) { printf("shaders: %s\n", e.localizedDescription.UTF8String); return 1; }
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    vd.attributes[0].format = MTLVertexFormatFloat3; vd.attributes[0].offset = 0;  vd.attributes[0].bufferIndex = 0;
    vd.attributes[1].format = MTLVertexFormatFloat3; vd.attributes[1].offset = 12; vd.attributes[1].bufferIndex = 0;
    vd.attributes[2].format = MTLVertexFormatFloat2; vd.attributes[2].offset = 24; vd.attributes[2].bufferIndex = 0;
    vd.layouts[0].stride = sizeof(Vtx);
    id<MTLRenderPipelineState> pipes[2];                     // 0: depth only, 1: depth + stencil
    for (int i = 0; i < 2; i++) {
        MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor new] autorelease];
        pd.vertexFunction = [[lib newFunctionWithName:@"demo_vs"] autorelease];
        pd.fragmentFunction = [[lib newFunctionWithName:@"demo_fs_plain"] autorelease];
        pd.vertexDescriptor = vd;
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm_sRGB;
        pd.depthAttachmentPixelFormat = i ? MTLPixelFormatDepth32Float_Stencil8 : MTLPixelFormatDepth32Float;
        if (i) pd.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
        pipes[i] = [d newRenderPipelineStateWithDescriptor:pd error:&e];
        if (!pipes[i]) { printf("pipeline %d: %s\n", i, e.localizedDescription.UTF8String); return 1; }
    }
    MTLDepthStencilDescriptor *dd = [[MTLDepthStencilDescriptor new] autorelease];
    dd.depthCompareFunction = MTLCompareFunctionLessEqual; dd.depthWriteEnabled = NO;
    id<MTLDepthStencilState> dssDepth = [d newDepthStencilStateWithDescriptor:dd];
    MTLStencilDescriptor *sdsc = [[MTLStencilDescriptor new] autorelease];
    sdsc.stencilCompareFunction = MTLCompareFunctionEqual; sdsc.readMask = 128; sdsc.writeMask = 0;
    dd.frontFaceStencil = sdsc; dd.backFaceStencil = sdsc;
    id<MTLDepthStencilState> dssStencil = [d newDepthStencilStateWithDescriptor:dd];
    Vtx v[24]; uint16_t idx[36];
    makeCube(v, idx);
    id<MTLBuffer> vb = [d newBufferWithBytes:v length:sizeof(v) options:MTLResourceStorageModeShared];
    id<MTLBuffer> ib = [d newBufferWithBytes:idx length:sizeof(idx) options:MTLResourceStorageModeShared];
    id<MTLCommandQueue> q = [d newCommandQueue];
    struct { const char *what; unsigned w, h; int ds; bool draw; bool stencilTest; } st[] = {
        { "256 x 256, depth + stencil, clear + draw with the stencil test",     256,  256, 2, true,  true  },
        { "1280 x 960, depth + stencil, clear + draw with the stencil test",   1280,  960, 2, true,  true  },
        { "2528 x 1410, depth only, clear + draw",                             2528, 1410, 1, true,  false },
        { "2528 x 1410, depth + stencil, clear only",                          2528, 1410, 2, false, false },
        { "2528 x 1410, depth + stencil, clear + draw, no stencil test",       2528, 1410, 2, true,  false },
        { "2528 x 1410, depth + stencil, clear + draw with the stencil test (= Maps)", 2528, 1410, 2, true, true },
    };
    for (int i = 0; i < (int)(sizeof(st) / sizeof(st[0])); i++) {
        @autoreleasepool {
            printf("[ds %d] %s ... ", i + 1, st[i].what);
            fflush(stdout);
            id<MTLTexture> rt = [newRT(d, MTLPixelFormatBGRA8Unorm_sRGB, st[i].w, st[i].h, false) autorelease];
            id<MTLTexture> dt = [newRT(d, st[i].ds == 2 ? MTLPixelFormatDepth32Float_Stencil8 : MTLPixelFormatDepth32Float,
                                       st[i].w, st[i].h, true) autorelease];
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = rt;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(0.12, 0.14, 0.2, 1);
            rp.depthAttachment.texture = dt;
            rp.depthAttachment.loadAction = MTLLoadActionClear;
            rp.depthAttachment.storeAction = MTLStoreActionDontCare;
            rp.depthAttachment.clearDepth = 1.0;
            if (st[i].ds == 2) {
                rp.stencilAttachment.texture = dt;
                rp.stencilAttachment.loadAction = MTLLoadActionClear;
                rp.stencilAttachment.storeAction = MTLStoreActionDontCare;
                rp.stencilAttachment.clearStencil = 128;
            }
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
            if (st[i].draw) {
                Uniforms u;
                u.mvp = mul(persp(1.0f, (float)st[i].w / st[i].h, 0.1f, 100.0f), mul(trans(0, 0, -5.5f), mul(rotX(0.5f), rotY(0.6f))));
                u.tint[0] = u.tint[1] = u.tint[2] = u.tint[3] = 1;
                [enc setRenderPipelineState:pipes[st[i].ds - 1]];
                [enc setDepthStencilState:st[i].stencilTest ? dssStencil : dssDepth];
                if (st[i].stencilTest) [enc setStencilReferenceValue:128];
                [enc setVertexBuffer:vb offset:0 atIndex:0];
                [enc setVertexBytes:&u length:sizeof(u) atIndex:1];
                [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:36 indexType:MTLIndexTypeUInt16 indexBuffer:ib indexBufferOffset:0];
            }
            [enc endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            if (hung()) {
                printf("GPU HUNG\n    %s\nstopped at step %d: reboot before running anything else\n",
                       probeProp(CFSTR("IrisXe-Hang")).UTF8String ?: "", i + 1);
                return 2;
            }
            printf("ok, pixels: corner 0x%08x centre 0x%08x\n", pixelAt(rt, 2, 2), pixelAt(rt, st[i].w / 2, st[i].h / 2));
            fflush(stdout);
        }
    }
    printf("all depth / stencil steps passed\n");
    return 0;
}

// v0.42.6: --steps grid: Maps' Grid shaders (rebuilt) with Maps' state: the draw that hung the GPU in Maps
static int runGridSteps(id<MTLDevice> d) {
    char exe[1024]; uint32_t sz = sizeof(exe);
    _NSGetExecutablePath(exe, &sz);
    NSString *dir = [[NSString stringWithUTF8String:exe] stringByDeletingLastPathComponent];
    NSError *e = nil;
    if (hung()) { printf("the GPU is ALREADY hung: reboot first\n"); return 1; }
    id<MTLLibrary> lib = [d newLibraryWithURL:[NSURL fileURLWithPath:[dir stringByAppendingPathComponent:@"irisxe-mtkdemo.metallib"]] error:&e];
    if (!lib) { printf("shaders: %s\n", e.localizedDescription.UTF8String); return 1; }
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    vd.attributes[0].format = MTLVertexFormatFloat4; vd.attributes[0].offset = 0; vd.attributes[0].bufferIndex = 0;
    vd.layouts[0].stride = 16;
    id<MTLRenderPipelineState> pipes[3];                     // 0: colour only, 1: + Depth32Float_Stencil8, 2: Maps' indices
    for (int i = 0; i < 3; i++) {
        MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor new] autorelease];
        pd.vertexFunction = [[lib newFunctionWithName:i == 2 ? @"grid_vs_m" : @"grid_vs"] autorelease];
        pd.fragmentFunction = [[lib newFunctionWithName:i == 2 ? @"grid_fs_m" : @"grid_fs"] autorelease];
        pd.vertexDescriptor = vd;
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm_sRGB;
        if (i) pd.depthAttachmentPixelFormat = pd.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
        pipes[i] = [d newRenderPipelineStateWithDescriptor:pd error:&e];
        if (!pipes[i]) { printf("pipeline %d: %s\n", i, e.localizedDescription.UTF8String); return 1; }
    }
    MTLDepthStencilDescriptor *dd = [[MTLDepthStencilDescriptor new] autorelease];
    dd.depthCompareFunction = MTLCompareFunctionLessEqual; dd.depthWriteEnabled = NO;
    MTLStencilDescriptor *sdsc = [[MTLStencilDescriptor new] autorelease];
    sdsc.stencilCompareFunction = MTLCompareFunctionEqual; sdsc.readMask = 128; sdsc.writeMask = 0;
    dd.frontFaceStencil = sdsc; dd.backFaceStencil = sdsc;
    id<MTLDepthStencilState> dss = [d newDepthStencilStateWithDescriptor:dd];
    static const float quad[6][4] = { {-1,-1,0.5f,1}, {1,-1,0.5f,1}, {1,1,0.5f,1}, {-1,-1,0.5f,1}, {1,1,0.5f,1}, {-1,1,0.5f,1} };
    static const uint16_t qi[6] = { 0, 1, 2, 3, 4, 5 };
    id<MTLBuffer> vb = [d newBufferWithBytes:quad length:sizeof(quad) options:MTLResourceStorageModeShared];
    id<MTLBuffer> ib = [d newBufferWithBytes:qi length:sizeof(qi) options:MTLResourceStorageModeShared];
    float view[20] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1, 0 };
    float clip[20] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1, 0.9f,0.9f,0.9f,1 };
    float style[16] = { 0.3f,0.3f,0.35f,1,  20,20,0.5f,0,  0,1,0,1,  0,0,0,0 };
    id<MTLCommandQueue> q = [d newCommandQueue];
    struct { const char *what; unsigned w, h; int ds; double clearDepth; } st[] = {
        { "Grid shaders, 256 x 256, colour only",                                256,  256, 0, 1.0 },
        { "Grid shaders, 2528 x 1410, colour only",                             2528, 1410, 0, 1.0 },
        { "Grid shaders, 2528 x 1410, depth + stencil, clear depth 1",          2528, 1410, 1, 1.0 },
        { "Grid shaders, 2528 x 1410, depth + stencil, clear depth 0",          2528, 1410, 1, 0.0 },
        { "Maps' exact vertices, buffers and data (v0.42.8)",                   2528, 1410, 2, 0.0 },
    };
    // v0.42.8: what Maps really used (IRISXE_LOG_PIPE=Grid): a quad of 4 vertices, indices 0 1 2 2 3 0, w ~ 0.00016
    static const float mQuad[6][4] = { {-1,-1,0,1}, {1,-1,0,1}, {1,1,0,1}, {-1,1,0,1}, {0,0,0,0}, {0,0,0,0} };
    static const uint16_t mIdx[6] = { 0, 1, 2, 2, 3, 0 };
    static const float mView[20] = { 0.000749691f,0,0,0, 0,0.00134413f,0,0, 0,0,-1.06977f,-1, 0,0,0.000142388f,0.00015659f, 0,0,0,0 };
    static const float mClip[20] = { 3.60158f,0,0,0, 0,3.60158f,0,0, 0,0,1,0, 0.839278f,0.0591068f,0,1, 0.0822766f,0.0822766f,0.0822766f,1 };
    static const float mStyle[16] = { 0.0395361f,0.0512551f,0.0578012f,0.4f, 2,20,0.360158f,0, 0.4f,1.4f,0.2f,1.2f, 0,0,0,0 };
    id<MTLBuffer> mvb = [d newBufferWithBytes:mQuad length:sizeof(mQuad) options:MTLResourceStorageModeShared];
    id<MTLBuffer> mib = [d newBufferWithBytes:mIdx length:sizeof(mIdx) options:MTLResourceStorageModeShared];
    for (int i = 0; i < (int)(sizeof(st) / sizeof(st[0])); i++) {
        @autoreleasepool {
            printf("[grid %d] %s ... ", i + 1, st[i].what);
            fflush(stdout);
            id<MTLTexture> rt = [newRT(d, MTLPixelFormatBGRA8Unorm_sRGB, st[i].w, st[i].h, false) autorelease];
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = rt;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(0.12, 0.14, 0.2, 1);
            if (st[i].ds) {   // (1 and 2)
                id<MTLTexture> dt = [newRT(d, MTLPixelFormatDepth32Float_Stencil8, st[i].w, st[i].h, true) autorelease];
                rp.depthAttachment.texture = dt; rp.depthAttachment.loadAction = MTLLoadActionClear;
                rp.depthAttachment.storeAction = MTLStoreActionDontCare; rp.depthAttachment.clearDepth = st[i].clearDepth;
                rp.stencilAttachment.texture = dt; rp.stencilAttachment.loadAction = MTLLoadActionClear;
                rp.stencilAttachment.storeAction = MTLStoreActionDontCare; rp.stencilAttachment.clearStencil = 128;
            }
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
            [enc setRenderPipelineState:pipes[st[i].ds]];
            if (st[i].ds) { [enc setDepthStencilState:dss]; [enc setStencilReferenceValue:128]; }
            if (st[i].ds == 2) {
                [enc setVertexBuffer:mvb offset:0 atIndex:0];
                [enc setVertexBytes:mView length:sizeof(mView) atIndex:1];
                [enc setVertexBytes:mClip length:sizeof(mClip) atIndex:3];
                [enc setVertexBytes:mStyle length:sizeof(mStyle) atIndex:4];
                [enc setFragmentBytes:mClip length:sizeof(mClip) atIndex:0];
                [enc setFragmentBytes:mStyle length:sizeof(mStyle) atIndex:1];
                [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:6 indexType:MTLIndexTypeUInt16 indexBuffer:mib indexBufferOffset:0];
                goto encoded;
            }
            [enc setVertexBuffer:vb offset:0 atIndex:0];
            [enc setVertexBytes:view length:sizeof(view) atIndex:1];
            [enc setVertexBytes:clip length:sizeof(clip) atIndex:2];
            [enc setVertexBytes:style length:sizeof(style) atIndex:3];
            [enc setFragmentBytes:clip length:sizeof(clip) atIndex:2];
            [enc setFragmentBytes:style length:sizeof(style) atIndex:3];
            [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:6 indexType:MTLIndexTypeUInt16 indexBuffer:ib indexBufferOffset:0];
        encoded:
            [enc endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            if (hung()) {
                printf("GPU HUNG\n    %s\nstopped at step %d: reboot before running anything else\n",
                       probeProp(CFSTR("IrisXe-Hang")).UTF8String ?: "", i + 1);
                return 2;
            }
            printf("ok, pixels: corner 0x%08x centre 0x%08x\n", pixelAt(rt, 2, 2), pixelAt(rt, st[i].w / 2, st[i].h / 2));
            fflush(stdout);
        }
    }
    printf("all grid steps passed\n");
    return 0;
}

// ---- v0.44.0: GPU reset test, and replaying a recording (IRISXE_RECORD=1) to find the draw that hangs ----
#include <dlfcn.h>
static unsigned resetCount(void) {
    NSString *r = probeProp(CFSTR("IrisXe-Resets"));
    return r && [r characterAtIndex:0] != '(' ? (unsigned)r.intValue : 0;
}
// v0.45.1: --freq [MHz | max | auto]: show / set the GPU clock (IrisXe-GPUFreq), e.g. --freq max
static int runFreq(const char *arg) {
    printf("GPU clock now:  %s\n", probeProp(CFSTR("IrisXe-GPUFreq")).UTF8String ?: "(no IrisXe-GPUFreq: install IrisXeCore 0.45.1)");
    if (!arg) return 0;
    long mhz = !strcmp(arg, "max") ? 99999 : !strcmp(arg, "auto") ? 0 : atol(arg);
    io_service_t s = findProbe();
    if (!s) { printf("IrisXeProbe not found\n"); return 1; }
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberLongType, &mhz);
    kern_return_t kr = IORegistryEntrySetCFProperty(s, CFSTR("IrisXe-SetFreq"), n);
    CFRelease(n); IOObjectRelease(s);
    printf("asked for %s (0x%x)\n", mhz == 99999 ? "the highest clock" : mhz ? arg : "auto", kr);
    printf("GPU clock now:  %s\n", probeProp(CFSTR("IrisXe-GPUFreq")).UTF8String ?: "?");
    return 0;
}

// v0.49.3: --batch N [nobarrier]: up to N draws per GPU job (1 = like no batching), nobarrier = no flush between
static int runBatchCfg(int argc, char **argv) {
    long v = argc > 2 ? atol(argv[2]) : 8;
    if (v < 1) v = 1;
    if (v > 8) v = 8;
    if (argc > 3 && !strcmp(argv[3], "nobarrier")) v |= 0x100;
    io_service_t s = findProbe();
    if (!s) { printf("IrisXeProbe not found\n"); return 1; }
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberLongType, &v);
    kern_return_t kr = IORegistryEntrySetCFProperty(s, CFSTR("IrisXe-BatchCfg"), n);
    CFRelease(n); IOObjectRelease(s);
    printf("set (0x%x): %s\n", kr, probeProp(CFSTR("IrisXe-BatchCfg-Now")).UTF8String ?: "? (IrisXeCore 0.49.3+)");
    return 0;
}

static int runResetTest(id<MTLDevice> d) {
    io_service_t s = findProbe();
    if (!s) { printf("IrisXeProbe not found\n"); return 1; }
    unsigned r0 = resetCount();
    printf("[reset 1] a blitter job that waits forever (the GPU hangs on purpose) ...\n");
    fflush(stdout);
    kern_return_t kr = IORegistryEntrySetCFProperty(s, CFSTR("IrisXe-HangTest"), kCFBooleanTrue);
    IOObjectRelease(s);
    printf("          %s (set: 0x%x)\n          IrisXe-Resets: %s\n", probeProp(CFSTR("IrisXe-HangTestResult")).UTF8String ?: "(no result)",
           kr, probeProp(CFSTR("IrisXe-Resets")).UTF8String ?: "(none)");
    bool reset = resetCount() > r0;
    printf("          -> %s\n", reset ? "THE GPU WAS RESET" : "no reset");
    printf("[reset 2] a draw after the reset ... ");
    fflush(stdout);
    id<MTLTexture> rt = [newRT(d, MTLPixelFormatBGRA8Unorm, 256, 256, false) autorelease];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = rt; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore; rp.colorAttachments[0].clearColor = MTLClearColorMake(0.12, 0.14, 0.2, 1);
    id<MTLCommandQueue> q = [d newCommandQueue];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
    [cb commit]; [cb waitUntilCompleted];
    uint32_t px = pixelAt(rt, 2, 2);
    bool ok = px == 0xff1f2433;
    printf("pixel 0x%08x -> %s\n", px, ok ? "THE GPU WORKS AGAIN" : "FAILED");
    printf("%s\n", reset && ok ? "GPU RESET WORKS" : "GPU reset test FAILED");
    return reset && ok ? 0 : 2;
}
typedef int (*ReplayFn)(id, const void *, size_t, uint32_t, char *, size_t);
static int runReplay(id<MTLDevice> d, const char *path, long from, long count) {
    // v0.44.1: Metal loads the bundle privately (RTLD_LOCAL): look the function up in the loaded bundle itself
    ReplayFn fn = NULL;
    void *h = dlopen("/Library/GPUBundles/IrisXeMTLDriver.bundle/Contents/MacOS/IrisXeMTLDriver", RTLD_LAZY | RTLD_NOLOAD);
    if (!h) h = dlopen("/Library/GPUBundles/IrisXeMTLDriver.bundle/Contents/MacOS/IrisXeMTLDriver", RTLD_LAZY | RTLD_LOCAL);
    if (h) fn = (ReplayFn)dlsym(h, "IrisXeReplayDraw");
    if (!fn) fn = (ReplayFn)dlsym(RTLD_DEFAULT, "IrisXeReplayDraw");
    NSDictionary *info0 = [NSDictionary dictionaryWithContentsOfFile:@"/Library/GPUBundles/IrisXeMTLDriver.bundle/Contents/Info.plist"];
    printf("installed bundle: version %s\n", [info0[@"CFBundleShortVersionString"] UTF8String] ?: "?");
    if (!fn) { printf("the bundle has no IrisXeReplayDraw (install bundle 0.44.0+)\n"); return 1; }
    NSData *all = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:path] options:NSDataReadingMappedIfSafe error:NULL];
    if (!all) { printf("can't read %s\n", path); return 1; }
    const uint8_t *b = all.bytes; size_t len = all.length;
    // index the records
    NSMutableArray *offs = [NSMutableArray array];
    for (size_t o = 0; o + 24 <= len; ) {
        uint32_t magic, total; memcpy(&magic, b + o, 4); memcpy(&total, b + o + 4, 4);
        if (magic != 0x43455258u || !total || o + total > len) break;
        [offs addObject:@(o)]; o += total;
    }
    printf("%lu GPU commands recorded in %s (draws, blits, compute markers - in the app's order)\n", (unsigned long)offs.count, path);
    if (from < 0) from = 0;
    long end = count > 0 ? from + count : (long)offs.count;
    if (end > (long)offs.count) end = (long)offs.count;
    static const struct { uint32_t v; const char *what; } variants[] = {
        { 0,   "again, the same command" },
        { 128, "again, with fresh buffers" },
        { 1,   "without its clears" },
        { 4,   "clears only, no draw" },
        { 2,   "without depth / stencil buffers" },
        { 16,  "without the stencil test" },
        { 32,  "depth test always, no depth write" },
        { 64,  "without blending" },
        { 8,   "first triangle only" },
    };
    int hangs = 0; long nDraw = 0, nBlit = 0, nSkip = 0;
    char info[512], recent[4][512] = { "", "", "", "" };
    for (long i = from; i < end && hangs < 3; i++) {
        @autoreleasepool {
            size_t o = [offs[i] unsignedLongValue];
            uint32_t total; memcpy(&total, b + o + 4, 4);
            unsigned r0 = resetCount();
            info[0] = 0;
            int rc = fn(d, b + o, total, 0, info, sizeof(info));
            if (rc == 1) { nSkip++; continue; }
            if (rc) { printf("[%ld] can't replay (%d)\n", i, rc); continue; }
            if (strstr(info, "blit:")) nBlit++; else nDraw++;
            if (i % 500 == 0) { printf("[%ld] ... %s\n", i, info); fflush(stdout); }
            if (resetCount() == r0) { strcpy(recent[i & 3], info); continue; }
            hangs++;
            printf("\n[%ld] HANG: %s\n    %s\n    the commands before it:\n", i, info, probeProp(CFSTR("IrisXe-Hang")).UTF8String ?: "");
            for (long j = i - 3; j < i; j++) if (j >= from) printf("      [%ld] %s\n", j, recent[j & 3]);
            if (i > from) {                               // the one before + this one again: does the pair hang?
                size_t po = [offs[i - 1] unsignedLongValue]; uint32_t pt; memcpy(&pt, b + po + 4, 4);
                unsigned r1 = resetCount();
                fn(d, b + po, pt, 0, NULL, 0); fn(d, b + o, total, 0, NULL, 0);
                printf("    %-36s -> %s\n", "the one before it + this one again", resetCount() == r1 ? "ok" : "HANG");
            }
            for (size_t k = 0; k < sizeof(variants) / sizeof(variants[0]); k++) {
                if (strstr(info, "blit:") && variants[k].v > 128) break;
                unsigned r1 = resetCount();
                fn(d, b + o, total, variants[k].v, NULL, 0);
                printf("    %-36s -> %s\n", variants[k].what, resetCount() == r1 ? "ok" : "HANG");
                fflush(stdout);
            }
            strcpy(recent[i & 3], info);
        }
    }
    printf("\nreplayed %ld..%ld: %ld draws, %ld blits, %ld compute skipped: %d hang(s)%s\n", from, end - 1,
           nDraw, nBlit, nSkip, hangs, hangs >= 3 ? " (stopped after 3)" : "");
    return hangs ? 2 : 0;
}

@interface DemoDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate> @end
@implementation DemoDelegate
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app { return YES; }
@end

int main(int argc, const char **argv) {
    @autoreleasepool {
        // v0.56.9: --batch only talks to IrisXeCore: no Metal device, no WindowServer (the display pipe test runs it
        // while WindowServer restarts)
        if (argc > 1 && !strcmp(argv[1], "--batch")) return runBatchCfg(argc, (char **)argv);
        // v0.40.1: Metal lists our device only if asked BEFORE the program connects to WindowServer
        // (NSApplication): after that it only lists the GPUs WindowServer knows, and WindowServer doesn't know
        // ours until the display is linked to it (sudo nvram irisxe-display=1). So: ask first.
        NSArray<id<MTLDevice>> *early = MTLCopyAllDevices();
        printf("irisxe-mtkdemo: before NSApplication, MTLCopyAllDevices -> %lu device(s)%s%s\n", (unsigned long)early.count,
               early.count ? ": " : "", early.count ? early[0].name.UTF8String : "");
        // v0.40.7: "--steps map noapp": the same jobs without ever connecting to WindowServer (like irisxe-metaltest)
        if (argc > 3 && !strcmp(argv[1], "--steps") && !strcmp(argv[2], "map") && !strcmp(argv[3], "noapp")) {
            if (!early.count) { printf("no Metal device\n"); return 1; }
            printf("(noapp: no NSApplication, no WindowServer connection)\n");
            return runMapSteps(early[0]);
        }
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        id<MTLDevice> d = MTLCreateSystemDefaultDevice();
        printf("irisxe-mtkdemo: MTLCreateSystemDefaultDevice -> %s (%s)\n", d ? d.name.UTF8String : "nil",
               d ? class_getName(object_getClass(d)) : "-");
        if (!d) {
            // the system default is "the GPU of the main display": until WindowServer knows our accelerator drives
            // the display (v0.40.0: sudo nvram irisxe-display=1), take the first device Metal lists
            NSArray<id<MTLDevice>> *all = MTLCopyAllDevices();
            printf("  after NSApplication, MTLCopyAllDevices -> %lu device(s)\n", (unsigned long)all.count);
            d = all.count ? [all[0] retain] : early.count ? [early[0] retain] : nil;
            [all release];
            printf("  (no default device: the display isn't linked to a GPU) using -> %s (%s)\n",
                   d ? d.name.UTF8String : "nil", d ? class_getName(object_getClass(d)) : "-");
        }
        if (!d) { printf("no Metal device\n"); return 1; }
        if (argc > 2 && !strcmp(argv[1], "--steps") && !strcmp(argv[2], "map")) return runMapSteps(d);
        if (argc > 2 && !strcmp(argv[1], "--steps") && !strcmp(argv[2], "ds")) return runDSSteps(d);
        if (argc > 2 && !strcmp(argv[1], "--steps") && !strcmp(argv[2], "grid")) return runGridSteps(d);
        if (argc > 1 && !strcmp(argv[1], "--resettest")) return runResetTest(d);
        if (argc > 1 && !strcmp(argv[1], "--freq")) return runFreq(argc > 2 ? argv[2] : NULL);
        if (argc > 1 && !strcmp(argv[1], "--batch")) return runBatchCfg(argc, argv);
        if (argc > 2 && !strcmp(argv[1], "--replay"))
            return runReplay(d, argv[2], argc > 3 ? atol(argv[3]) : 0, argc > 4 ? atol(argv[4]) : 0);
        if (argc > 1 && !strcmp(argv[1], "--steps")) return runSteps(d, argc > 2 && !strcmp(argv[2], "big"));
        if (!strstr(class_getName(object_getClass(d)), "IrisXe")) printf("note: this is not our driver's device\n");
        fflush(stdout);
        NSWindow *win = [[NSWindow alloc] initWithContentRect:NSMakeRect(160, 160, 640, 480)
            styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
            backing:NSBackingStoreBuffered defer:NO];
        win.title = @"IrisXe MetalKit demo";
        MTKView *view = [[MTKView alloc] initWithFrame:win.contentView.bounds device:d];
        view.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
        view.depthStencilPixelFormat = MTLPixelFormatDepth32Float;
        view.clearColor = MTLClearColorMake(0.12, 0.14, 0.2, 1);
        view.preferredFramesPerSecond = 60;
        view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        DemoRenderer *r = [DemoRenderer new];
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "-nofbo")) r.noFBO = YES;          // v0.40.2: framebufferOnly = NO (like the metaltest window)
            else r.runFor = atof(argv[i]);
        }
        if (r.noFBO) view.framebufferOnly = NO;
        if (![r setUp:view]) return 1;
        view.delegate = r;
        win.contentView = view;
        DemoDelegate *dl = [DemoDelegate new];
        NSApp.delegate = dl;
        [win makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        [NSApp run];
        printf("irisxe-mtkdemo: %u frames drawn\n", r.total);
    }
    return 0;
}

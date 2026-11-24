// IrisXeMTLRender.inc - milestone 5c: render pipelines and render command encoders.
// Included by IrisXeMTLDriver.m. A pipeline = our compiler's vertex + fragment
// code (src/compiler, irisxe_compile_air_render) in GPU buffers, plus the state
// the GPU needs to link them (varyings, vertex fetch). A draw sends one kernel
// command XE_KCMD_DRAW with a full XeDrawDesc (src/IrisXeDrawABI.h); IrisXeCore
// checks it and builds the 3D commands. A render pass clear rides on the first
// draw (or a draw-less command at endEncoding).
#include "../IrisXeDrawABI.h"
#include "../compiler/xe_compile.h"

#define XE_KCMD_DRAW 0x10004u
typedef struct { uint32_t type, size; struct XeDrawDesc d; } XeDrawCmd;

typedef int (*XeRenderCompileFn)(const void *, size_t, const char *, uint32_t *, uint32_t, uint32_t *,
                                 struct XeShaderInfo *, char *, size_t);
static XeRenderCompileFn renderCompilerEntry(NSString **why) {
    static XeRenderCompileFn fn;
    static NSString *err;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        Dl_info di = {0};
        if (!dladdr((void *)&renderCompilerEntry, &di) || !di.dli_fname) { err = @"can't find our bundle"; return; }
        NSString *contents = [[[NSString stringWithUTF8String:di.dli_fname] stringByDeletingLastPathComponent]
                                 stringByDeletingLastPathComponent];
        NSString *path = [contents stringByAppendingPathComponent:@"Frameworks/libIrisXeCompiler.dylib"];
        void *h = dlopen(path.fileSystemRepresentation, RTLD_NOW | RTLD_LOCAL);
        if (!h) { err = [[NSString alloc] initWithFormat:@"compiler not loaded: %s", dlerror()]; return; }
        fn = (XeRenderCompileFn)dlsym(h, "irisxe_compile_air_render");
        if (!fn) err = @"compiler has no irisxe_compile_air_render";
    });
    if (!fn && why) *why = err;
    return fn;
}

// ---- v0.58.5: opaque draws through the blender ----------------------------------------------------------
// v0.58.6: OFF by default - it didn't help (calm --pixelrate: tiled 1.14 vs 1.29 ms, linear the same; WindowServer's
// recording: ~930 Mpixels/s either way). On: IRISXE_OPAQUEBLEND=1, or /Library/Preferences/irisxe-opaqueblend "on".
static unsigned gOpaqueBlendPipes;
static bool opaqueBlendOn(void) {
    const char *e = getenv("IRISXE_OPAQUEBLEND");
    if (e) return e[0] == '1';
    static int filePol = -1;
    if (filePol < 0) {
        filePol = 0;
        FILE *f = fopen("/Library/Preferences/irisxe-opaqueblend", "r");
        if (f) { char b[16] = {}; if (fgets(b, sizeof(b), f) && !strncmp(b, "on", 2)) filePol = 1; fclose(f); }
    }
    return filePol;
}
static bool opaqueBlendPF(NSUInteger pf) {   // BGRA8 / RGBA8 (+ sRGB), RGB10A2 / BGR10A2
    return pf == 80 || pf == 81 || pf == 70 || pf == 71 || pf == 90 || pf == 94;
}

// ---- v0.58.0: compiled-shader cache ----------------------------------------------------------------------
// The compiler's result depends only on the AIR bitcode, the function name and the compiler itself, but every
// pipeline WindowServer makes compiled its shaders again: Core Animation's uber shader (Xghc, ~75000 instructions)
// three times in one WindowServer, ~0.3-1 s each, and under the pipeline lock, so the main thread's own pipelines
// waited. Now: once per process (memory), and kept on disk between boots in /var/tmp/irisxe-shaders-<uid> (a 0700
// folder of this user only; a file is used only if its header matches the bitcode hash, length, function name and
// compiler file). Off: IRISXE_SHADER_CACHE=0 in the environment, or /Library/Preferences/irisxe-shadercache "off".
typedef struct {
    uint32_t magic, hdrSize, dw, infoSize;
    uint64_t bcHash, bcLen, compilerId;
    char     name[128];
    char     log[512];
} XeShaderCacheHdr;
#define XE_SHADER_CACHE_MAGIC 0x58454331u                 // "XEC1"
static unsigned gShaderCacheHits[3];                      // memory, disk, compiled
static bool shaderCacheOn(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("IRISXE_SHADER_CACHE");
        on = !(e && e[0] == '0');
        FILE *f = fopen("/Library/Preferences/irisxe-shadercache", "r");
        if (f) { char b[16] = {}; if (fgets(b, sizeof(b), f) && !strncmp(b, "off", 3)) on = 0; fclose(f); }
    }
    return on;
}
static uint64_t fnv64(const void *p, size_t n, uint64_t h) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; }
    return h;
}
static uint64_t compilerId(void) {                        // the compiler file: changes with every install
    static uint64_t id;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        Dl_info di = {0};
        if (!dladdr((void *)&compilerId, &di) || !di.dli_fname) return;
        NSString *contents = [[[NSString stringWithUTF8String:di.dli_fname] stringByDeletingLastPathComponent] stringByDeletingLastPathComponent];
        struct stat st;
        if (stat([contents stringByAppendingPathComponent:@"Frameworks/libIrisXeCompiler.dylib"].fileSystemRepresentation, &st)) return;
        // v0.61.0: /Library/Preferences/irisxe-overlap "off" = the shader compiler without its v0.61.0 speed-ups
        // (overlapped reads, grouped samples, constants read once); part of the cache key
        uint64_t off = xeCompilerSwitches();               // v0.62.1: also /Library/Preferences/irisxe-compiler
        uint64_t v[5] = { (uint64_t)st.st_size, (uint64_t)st.st_mtimespec.tv_sec, (uint64_t)st.st_mtimespec.tv_nsec, (uint64_t)st.st_ino, off };
        id = fnv64(v, sizeof(v), 0xcbf29ce484222325ull) | 1;
    });
    return id;
}
static NSString *shaderCacheDir(void) {                   // nil if the folder isn't safely ours
    static NSString *dir;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        char path[64];
        // v0.58.1: the effective user (WindowServer runs as _windowserver with real user root: v0.58.0 compared the
        // folder's owner with the real user and turned the disk cache off)
        snprintf(path, sizeof(path), "/var/tmp/irisxe-shaders-%u", (unsigned)geteuid());
        mkdir(path, 0700);
        struct stat st;
        if (lstat(path, &st) || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 077)) {
            os_log_error(gLog, "IrisXeMTL: shader cache folder %{public}s not ours / not private: disk cache off", path);
            return;
        }
        dir = [[NSString alloc] initWithUTF8String:path];
    });
    return dir;
}
static int cachedCompile(XeRenderCompileFn compile, NSData *bc, const char *name, uint32_t *code, uint32_t maxDW,
                         uint32_t *dw, struct XeShaderInfo *info, char *log, size_t logLen) {
    uint64_t cid = compilerId();                          // v0.61.0: also applies /Library/Preferences/irisxe-overlap
    if (!shaderCacheOn() || !name || strlen(name) >= 128 || !cid) {
        gShaderCacheHits[2]++;
        return compile(bc.bytes, bc.length, name, code, maxDW, dw, info, log, logLen);
    }
    static NSMutableDictionary *mem;
    static pthread_mutex_t ml = PTHREAD_MUTEX_INITIALIZER;
    uint64_t h = fnv64(bc.bytes, bc.length, 0xcbf29ce484222325ull);
    h = fnv64(name, strlen(name), h);
    NSString *key = [NSString stringWithFormat:@"%016llx-%llx", (unsigned long long)h, (unsigned long long)bc.length];
    NSData *blob = nil;
    pthread_mutex_lock(&ml);
    if (!mem) mem = [NSMutableDictionary new];
    blob = [[mem objectForKey:key] retain];
    pthread_mutex_unlock(&ml);
    int from = 0;                                         // 0 memory, 1 disk
    NSString *file = shaderCacheDir() ? [shaderCacheDir() stringByAppendingPathComponent:[key stringByAppendingString:@".bin"]] : nil;
    if (!blob && file) { blob = [[NSData alloc] initWithContentsOfFile:file]; from = 1; }
    if (blob) {
        const XeShaderCacheHdr *hd = blob.bytes;
        bool ok = blob.length >= sizeof(*hd) && hd->magic == XE_SHADER_CACHE_MAGIC && hd->hdrSize == sizeof(*hd) &&
                  hd->infoSize == sizeof(*info) && hd->bcHash == h && hd->bcLen == bc.length &&
                  hd->compilerId == compilerId() && !strncmp(hd->name, name, 128) && hd->dw <= maxDW &&
                  blob.length == sizeof(*hd) + sizeof(*info) + (size_t)hd->dw * 4;
        if (ok) {
            memcpy(info, (const char *)blob.bytes + sizeof(*hd), sizeof(*info));
            memcpy(code, (const char *)blob.bytes + sizeof(*hd) + sizeof(*info), (size_t)hd->dw * 4);
            *dw = hd->dw;
            snprintf(log, logLen, "%.*s (cached: %s)", (int)sizeof(hd->log), hd->log, from ? "disk" : "memory");
            if (from) { pthread_mutex_lock(&ml); [mem setObject:blob forKey:key]; pthread_mutex_unlock(&ml); }
            gShaderCacheHits[from]++;
            [blob release];
            return 0;
        }
        [blob release];
    }
    gShaderCacheHits[2]++;
    int r = compile(bc.bytes, bc.length, name, code, maxDW, dw, info, log, logLen);
    if (r) return r;
    NSMutableData *nb = [NSMutableData dataWithLength:sizeof(XeShaderCacheHdr) + sizeof(*info) + (size_t)*dw * 4];
    XeShaderCacheHdr *hd = nb.mutableBytes;
    hd->magic = XE_SHADER_CACHE_MAGIC; hd->hdrSize = sizeof(*hd); hd->dw = *dw; hd->infoSize = sizeof(*info);
    hd->bcHash = h; hd->bcLen = bc.length; hd->compilerId = compilerId();
    strlcpy(hd->name, name, sizeof(hd->name)); strlcpy(hd->log, log, sizeof(hd->log));
    memcpy((char *)nb.mutableBytes + sizeof(*hd), info, sizeof(*info));
    memcpy((char *)nb.mutableBytes + sizeof(*hd) + sizeof(*info), code, (size_t)*dw * 4);
    pthread_mutex_lock(&ml); [mem setObject:nb forKey:key]; pthread_mutex_unlock(&ml);
    if (file) {                                           // write to a temporary name, then rename (no half files)
        NSString *tmp = [file stringByAppendingFormat:@".%d.tmp", getpid()];
        if ([nb writeToFile:tmp atomically:NO]) rename(tmp.fileSystemRepresentation, file.fileSystemRepresentation);
    }
    return 0;
}

// ---- pipeline ------------------------------------------------------------------
typedef struct {
    id dev, label, vsBuf, psBuf;
    id vsScratch, psScratch;                  // v0.48.0: private arrays
    uint32_t vsDW, psDW;
    struct XeShaderInfo vi, fi;
    uint32_t swiz[16], constInterp, sbeReadLen, baryMode;
    uint32_t nvb, vbIndex[XE_DRAW_MAX_VB], vbStride[XE_DRAW_MAX_VB], vbStep[XE_DRAW_MAX_VB], vbRate[XE_DRAW_MAX_VB];
    uint32_t nve; struct XeDrawVE ve[XE_DRAW_MAX_VE];
    uint32_t nrt, rtGen[XE_DRAW_MAX_RT]; NSUInteger rtPF[XE_DRAW_MAX_RT];
    struct XeDrawBlend blend[XE_DRAW_MAX_RT];       // v0.33.0
    NSUInteger depthPF, stencilPF;                  // v0.34.0
    char name[96];                                  // v0.42.7: "vertex + fragment" (IRISXE_LOG_PIPE)
} XeRPipe;
static bool isIntPF(NSUInteger pf) { return pf == 53 || pf == 54 || pf == 73 || pf == 123; }   // R32Uint/Sint, RGBA8Uint, RGBA32Uint
// pixel formats without an alpha channel: Metal reads destination alpha as 1
static bool pfHasAlpha(NSUInteger pf) {
    static const NSUInteger noA[] = { 10, 11, 12, 13, 14, 20, 22, 23, 24, 25, 30, 31, 32, 33, 34, 40, 53, 54, 55, 60, 62, 63, 64, 65,
                                      92, 93, 103, 104, 105 };
    for (unsigned i = 0; i < sizeof(noA) / sizeof(noA[0]); i++) if (noA[i] == pf) return false;
    return true;
}
static Class gRPipeClass, gRenderClass;
static Ivar gRPipeIvar, gRenderIvar;
static XeRPipe *rpipe(id p) { return p && object_getClass(p) == gRPipeClass ? *(XeRPipe **)((char *)p + ivar_getOffset(gRPipeIvar)) : NULL; }

// MTLVertexFormat -> Gen format and the components it has (rest: 0, 0, 1)
static bool vtxFormat(NSUInteger f, uint32_t *gen, uint32_t *comps, bool *isInt) {
    static const struct { NSUInteger f; uint32_t gen, comps, isInt; } t[] = {
        { MTLVertexFormatFloat, 216, 1, 0 }, { MTLVertexFormatFloat2, 133, 2, 0 }, { MTLVertexFormatFloat3, 64, 3, 0 },
        { MTLVertexFormatFloat4, 0, 4, 0 },
        { MTLVertexFormatInt, 214, 1, 1 }, { MTLVertexFormatInt2, 134, 2, 1 }, { MTLVertexFormatInt3, 65, 3, 1 }, { MTLVertexFormatInt4, 1, 4, 1 },
        { MTLVertexFormatUInt, 215, 1, 1 }, { MTLVertexFormatUInt2, 135, 2, 1 }, { MTLVertexFormatUInt3, 66, 3, 1 }, { MTLVertexFormatUInt4, 2, 4, 1 },
        { MTLVertexFormatHalf2, 208, 2, 0 }, { MTLVertexFormatHalf3, 411, 3, 0 }, { MTLVertexFormatHalf4, 132, 4, 0 },
        { MTLVertexFormatUChar2Normalized, 262, 2, 0 }, { MTLVertexFormatUChar3Normalized, 403, 3, 0 },
        { MTLVertexFormatUChar4Normalized, 199, 4, 0 }, { MTLVertexFormatUChar4Normalized_BGRA, 192, 4, 0 },
        { MTLVertexFormatChar2Normalized, 263, 2, 0 }, { MTLVertexFormatChar3Normalized, 404, 3, 0 }, { MTLVertexFormatChar4Normalized, 201, 4, 0 },
        { MTLVertexFormatUChar2, 265, 2, 1 }, { MTLVertexFormatUChar3, 456, 3, 1 }, { MTLVertexFormatUChar4, 203, 4, 1 },
        { MTLVertexFormatChar2, 264, 2, 1 }, { MTLVertexFormatChar3, 457, 3, 1 }, { MTLVertexFormatChar4, 202, 4, 1 },
        { MTLVertexFormatUShort2Normalized, 204, 2, 0 }, { MTLVertexFormatUShort3Normalized, 412, 3, 0 },
        { MTLVertexFormatUShort4Normalized, 128, 4, 0 }, { MTLVertexFormatShort2Normalized, 205, 2, 0 },
        { MTLVertexFormatShort3Normalized, 413, 3, 0 }, { MTLVertexFormatShort4Normalized, 129, 4, 0 },
        { MTLVertexFormatUShort2, 207, 2, 1 }, { MTLVertexFormatUShort3, 432, 3, 1 }, { MTLVertexFormatUShort4, 131, 4, 1 },
        { MTLVertexFormatShort2, 206, 2, 1 }, { MTLVertexFormatShort3, 433, 3, 1 }, { MTLVertexFormatShort4, 130, 4, 1 },
        { MTLVertexFormatUInt1010102Normalized, 194, 4, 0 },
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++)
        if (t[i].f == f) { *gen = t[i].gen; *comps = t[i].comps; *isInt = t[i].isInt; return true; }
    return false;
}

static NSString *gLastRenderLog;
static id codeBuffer(id dev, const uint32_t *code, uint32_t dw) {
    id b = ((id (*)(id, SEL, NSUInteger, NSUInteger))objc_msgSend)(dev, sel_registerName("newBufferWithLength:options:"),
                                                                  dw * 4 + 256, MTLResourceStorageModeShared);
    void *p = b ? ((void *(*)(id, SEL))objc_msgSend)(b, sel_registerName("contents")) : NULL;
    if (!p) { [b release]; return nil; }
    memcpy(p, code, dw * 4);
    memset((char *)p + dw * 4, 0, 256);
    return b;
}
static NSData *fnBitcode(id fn) {
    if (!fn || ![fn respondsToSelector:sel_registerName("bitcodeData")]) return nil;
    id bc = ((id (*)(id, SEL))objc_msgSend)(fn, sel_registerName("bitcodeData"));
    return [bc isKindOfClass:[NSData class]] && [bc length] ? bc : nil;
}
static NSString *fnName(id fn) { return fn ? ((NSString *(*)(id, SEL))objc_msgSend)(fn, sel_registerName("name")) : nil; }

static uint64_t bufVA(id b, NSUInteger off);
// v0.48.0: a shader with private arrays: make its scratch buffer and put its address into the code
static id scratchFor(id dev, uint32_t *code, uint32_t dw, uint32_t privBytes, NSString **msg) {
    if (!privBytes) return nil;
    id b = makeBuffer(dev, NULL, (NSUInteger)XE_SCRATCH_IDS * 8 * privBytes, 0, nil);
    uint64_t va = b ? bufVA(b, 0) : 0;
    if (!va || va > 0xFFFFFFFFull) { [b release]; *msg = @"no GPU memory for private arrays"; return nil; }
    int hits = 0;
    for (uint32_t i = 0; i < dw; i++) if (code[i] == XE_SCRATCH_MAGIC) { code[i] = (uint32_t)va; hits++; }
    if (hits != 1) { [b release]; *msg = [NSString stringWithFormat:@"private arrays: scratch address found %d times", hits]; return nil; }
    return b;
}
// v0.51.4: a shader our compiler refused: its AIR (as Metal gave it to us, function constants filled in) goes to
// /var/tmp/irisxe-fail/<vs|fs>-<function>.bc, so it can be compiled again elsewhere (WindowServer's shaders)
static void saveFailedAIR(NSData *bc, NSString *name, NSString *kind) {
    if (!bc) return;
    NSString *dir = @"/var/tmp/irisxe-fail";
    [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES
                                               attributes:@{ NSFilePosixPermissions: @0777 } error:nil];
    NSString *n = [[name stringByReplacingOccurrencesOfString:@":" withString:@"_"] stringByReplacingOccurrencesOfString:@"/" withString:@"_"];
    if (n.length > 120) n = [n substringToIndex:120];
    // v0.68.31: a hash of the AIR in the name - Dawn (Brave/Chrome) calls every shader "vs_main"/"fs_main",
    // so before only the first failing one was ever kept
    uint32_t h = 2166136261u;
    const uint8_t *bp = bc.bytes;
    for (NSUInteger i = 0; i < bc.length; i++) h = (h ^ bp[i]) * 16777619u;
    NSString *path = [dir stringByAppendingPathComponent:[NSString stringWithFormat:@"%@-%@-%08x-%s.bc", kind, n, h, getprogname()]];
    if ([[NSFileManager defaultManager] fileExistsAtPath:path]) return;
    BOOL ok = [bc writeToFile:path atomically:NO];
    if (!ok) {   // v0.65.1: sandboxed programs (WallpaperSequoiaExtension) may only write their own temporary folder
        NSString *d2 = [NSTemporaryDirectory() stringByAppendingPathComponent:@"irisxe-fail"];
        [[NSFileManager defaultManager] createDirectoryAtPath:d2 withIntermediateDirectories:YES attributes:nil error:nil];
        path = [d2 stringByAppendingPathComponent:[path lastPathComponent]];
        ok = [bc writeToFile:path atomically:NO];
    }
    os_log(gLog, "IrisXeMTL: the AIR of %{public}@ saved in %{public}@ (%d)", name, path, ok);
}
static id newRenderPipeline(id dev, id desc, NSError **err) {
    NSString *msg = nil;
    XeRPipe *p = calloc(1, sizeof(XeRPipe));
    // v0.58.0: the code buffer per thread, and the lock released while compiling: a long first compile (the
    // uber shader) on one thread no longer holds up pipelines made on WindowServer's main thread
    static __thread uint32_t *code;
    if (!code) code = malloc(XE_KERNEL_MAX);
    if (!code) { if (err) *err = [NSError errorWithDomain:@"IrisXeMTL" code:2 userInfo:@{ NSLocalizedDescriptionKey: @"no memory" }]; free(p); return nil; }
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    {
        id vf = ((id (*)(id, SEL))objc_msgSend)(desc, sel_registerName("vertexFunction"));
        id ff = ((id (*)(id, SEL))objc_msgSend)(desc, sel_registerName("fragmentFunction"));
        NSString *why = nil;
        XeRenderCompileFn compile = renderCompilerEntry(&why);
        char vlog[512] = "", flog[512] = "";
        NSData *vb = fnBitcode(vf), *fb = fnBitcode(ff);
        if (!compile) { msg = why ?: @"no compiler"; goto out; }
        if (!vb) { msg = @"the vertex function has no AIR bitcode"; goto out; }
        if (!fb) { msg = ff ? @"the fragment function has no AIR bitcode" : @"render pipelines without a fragment function are not supported yet"; goto out; }
        pthread_mutex_unlock(&lock);
        int cr_vb = cachedCompile(compile, vb, fnName(vf).UTF8String, code, XE_KERNEL_MAX / 4, &p->vsDW, &p->vi, vlog, sizeof(vlog));
        pthread_mutex_lock(&lock);
        if (cr_vb) {
            saveFailedAIR(vb, fnName(vf), @"vs");
            saveFailedAIR(fb, fnName(ff), @"fspair");   // v0.68.31: its fragment too, so both are fixed in one round
            msg = [NSString stringWithFormat:@"vertex %@: %s", fnName(vf), vlog]; goto out;
        }
        if (p->vi.privBytes && !(p->vsScratch = scratchFor(dev, code, p->vsDW, p->vi.privBytes, &msg))) goto out;
        p->vsBuf = codeBuffer(dev, code, p->vsDW);
        pthread_mutex_unlock(&lock);
        int cr_fb = cachedCompile(compile, fb, fnName(ff).UTF8String, code, XE_KERNEL_MAX / 4, &p->psDW, &p->fi, flog, sizeof(flog));
        pthread_mutex_lock(&lock);
        if (cr_fb) {
            saveFailedAIR(fb, fnName(ff), @"fs");
            msg = [NSString stringWithFormat:@"fragment %@: %s", fnName(ff), flog]; goto out;
        }
        if (p->fi.privBytes && !(p->psScratch = scratchFor(dev, code, p->psDW, p->fi.privBytes, &msg))) goto out;
        p->psBuf = codeBuffer(dev, code, p->psDW);
        if (!p->vsBuf || !p->psBuf) { msg = @"no GPU memory for the shader code"; goto out; }
        // v0.60.2: big fragment shaders (300+ instructions: WindowServer's blurs and colour effects, most of its GPU
        // time) - their AIR goes to /var/tmp/irisxe-air-big once per shader, so the compiler can be improved for them
        if (p->psDW >= 1200) {
            static NSMutableSet *seen;
            if (!seen) seen = [[NSMutableSet alloc] init];
            NSString *fn = [[fnName(ff) stringByReplacingOccurrencesOfString:@":" withString:@"_"] stringByReplacingOccurrencesOfString:@"/" withString:@"_"];
            if (fn.length > 100) fn = [fn substringToIndex:100];
            NSString *key = [NSString stringWithFormat:@"%@-%u", fn, p->psDW / 4];
            if (![seen containsObject:key]) {
                [seen addObject:key];
                NSString *dir = @"/var/tmp/irisxe-air-big";
                [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES
                                                           attributes:@{ NSFilePosixPermissions: @0777 } error:nil];
                NSString *fp = [dir stringByAppendingPathComponent:[NSString stringWithFormat:@"ps-%@-%ui-%s.bc", key, p->psDW / 4, getprogname()]];
                NSString *vp = [dir stringByAppendingPathComponent:[NSString stringWithFormat:@"vs-for-%@-%s.bc", key, getprogname()]];
                if (![[NSFileManager defaultManager] fileExistsAtPath:fp]) { [fb writeToFile:fp atomically:NO]; [vb writeToFile:vp atomically:NO]; }
            }
        }
        if (getenv("IRISXE_DUMP_AIR")) {           // v0.42.3: save the AIR, named by the code's GPU address (hang reports)
            NSString *dir = [NSTemporaryDirectory() stringByAppendingPathComponent:@"irisxe-air"];
            [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
            NSString *vn = [[fnName(vf) stringByReplacingOccurrencesOfString:@":" withString:@"_"] stringByReplacingOccurrencesOfString:@"/" withString:@"_"];
            NSString *fn = [[fnName(ff) stringByReplacingOccurrencesOfString:@":" withString:@"_"] stringByReplacingOccurrencesOfString:@"/" withString:@"_"];
            NSString *vp = [dir stringByAppendingPathComponent:[NSString stringWithFormat:@"vs-%llx-%@.bc", (unsigned long long)bufVA(p->vsBuf, 0), vn]];
            NSString *fp = [dir stringByAppendingPathComponent:[NSString stringWithFormat:@"ps-%llx-%@.bc", (unsigned long long)bufVA(p->psBuf, 0), fn]];
            BOOL ok1 = [vb writeToFile:vp atomically:NO], ok2 = [fb writeToFile:fp atomically:NO];
            os_log(gLog, "IrisXeMTL: AIR saved (%d %d) in %{public}@", ok1, ok2, dir);
        }
        if (p->vi.vueSlots > 20) { msg = @"too many vertex outputs"; goto out; }
        // varyings: fragment input k <- vertex output with the same AIR name
        unsigned maxSrc = 0;
        for (unsigned k = 0; k < p->fi.numVaryings; k++) {
            unsigned j = 0;
            while (j < p->vi.numVaryings && strcmp(p->vi.varyingName[j], p->fi.varyingName[k])) j++;
            if (j == p->vi.numVaryings) { msg = [NSString stringWithFormat:@"fragment input %s is not a vertex output", p->fi.varyingName[k]]; goto out; }
            p->swiz[k] = j; if (j > maxSrc) maxSrc = j;
            if (p->fi.varyingInterp[k] == XE_INTERP_FLAT) p->constInterp |= 1u << k;
        }
        p->sbeReadLen = p->fi.numVaryings ? (maxSrc + 2) / 2 : 0;
        p->baryMode = ((p->fi.flags & XE_FS_PERSP) ? 1u : 0u) | ((p->fi.flags & XE_FS_LINEAR) ? 8u : 0u);
        // vertex fetch: VS input slot s <- [[attribute(location)]] of the vertex descriptor
        id vd = ((id (*)(id, SEL))objc_msgSend)(desc, sel_registerName("vertexDescriptor"));
        for (unsigned s = 0; s < p->vi.numInputs; s++) {
            struct XeDrawVE *e = &p->ve[s];
            if (p->vi.inputLocation[s] < 0) {                        // VertexID / InstanceID slot
                e->vb = 0; e->format = 0; e->ctrl = 0x2222;          // STORE_0 x4 (the fetcher fills x / y)
                continue;
            }
            if (!vd) { msg = @"the vertex function has [[stage_in]] inputs but the pipeline has no vertex descriptor"; goto out; }
            id attr = ((id (*)(id, SEL, NSUInteger))objc_msgSend)(((id (*)(id, SEL))objc_msgSend)(vd, sel_registerName("attributes")),
                                                                    sel_registerName("objectAtIndexedSubscript:"), (NSUInteger)p->vi.inputLocation[s]);
            NSUInteger fmt = ((NSUInteger (*)(id, SEL))objc_msgSend)(attr, sel_registerName("format"));
            NSUInteger off = ((NSUInteger (*)(id, SEL))objc_msgSend)(attr, sel_registerName("offset"));
            NSUInteger bi = ((NSUInteger (*)(id, SEL))objc_msgSend)(attr, sel_registerName("bufferIndex"));
            uint32_t gen = 0, comps = 0; bool isInt = false;
            if (!vtxFormat(fmt, &gen, &comps, &isInt)) {
                msg = [NSString stringWithFormat:@"vertex attribute %d: format %lu not supported yet", p->vi.inputLocation[s], (unsigned long)fmt]; goto out;
            }
            if (bi >= 8) { msg = [NSString stringWithFormat:@"vertex buffer index %lu: only 0..7 yet", (unsigned long)bi]; goto out; }
            unsigned j = 0;
            while (j < p->nvb && p->vbIndex[j] != bi) j++;
            if (j == p->nvb) {
                if (p->nvb >= XE_DRAW_MAX_VB) { msg = @"too many vertex buffers"; goto out; }
                id lay = ((id (*)(id, SEL, NSUInteger))objc_msgSend)(((id (*)(id, SEL))objc_msgSend)(vd, sel_registerName("layouts")),
                                                                       sel_registerName("objectAtIndexedSubscript:"), bi);
                p->vbIndex[j] = (uint32_t)bi;
                p->vbStride[j] = (uint32_t)((NSUInteger (*)(id, SEL))objc_msgSend)(lay, sel_registerName("stride"));
                p->vbStep[j] = (uint32_t)((NSUInteger (*)(id, SEL))objc_msgSend)(lay, sel_registerName("stepFunction"));
                p->vbRate[j] = (uint32_t)((NSUInteger (*)(id, SEL))objc_msgSend)(lay, sel_registerName("stepRate"));
                p->nvb++;
            }
            e->vb = j; e->format = gen; e->offset = (uint32_t)off;
            for (unsigned c = 0; c < 4; c++)                        // VFCOMP: 1 source, 2 zero, 3 one (float), 4 one (int)
                e->ctrl |= (c < comps ? 1u : c < 3 ? 2u : isInt ? 4u : 3u) << (4 * c);
            e->instanced = p->vbStep[j] == MTLVertexStepFunctionPerInstance;
            e->stepRate = p->vbRate[j];
        }
        p->nve = p->vi.numInputs;
        // render targets
        id cas = ((id (*)(id, SEL))objc_msgSend)(desc, sel_registerName("colorAttachments"));
        for (unsigned k = 0; k < XE_DRAW_MAX_RT; k++) {
            id ca = ((id (*)(id, SEL, NSUInteger))objc_msgSend)(cas, sel_registerName("objectAtIndexedSubscript:"), k);
            NSUInteger pf = ca ? ((NSUInteger (*)(id, SEL))objc_msgSend)(ca, sel_registerName("pixelFormat")) : 0;
            p->rtPF[k] = pf;
            if (!pf) continue;
            NSUInteger bpp = 0;
            if (!texFormat(pf, &p->rtGen[k], &bpp)) { msg = [NSString stringWithFormat:@"color attachment %u: pixel format %lu not supported yet", k, (unsigned long)pf]; goto out; }
            struct XeDrawBlend *b = &p->blend[k];
            b->enable = ((BOOL (*)(id, SEL))objc_msgSend)(ca, sel_registerName("isBlendingEnabled")) ? 1 : 0;
#define BLENDGET(name) (uint32_t)((NSUInteger (*)(id, SEL))objc_msgSend)(ca, sel_registerName(name))
            b->src = BLENDGET("sourceRGBBlendFactor"); b->dst = BLENDGET("destinationRGBBlendFactor"); b->op = BLENDGET("rgbBlendOperation");
            b->srcA = BLENDGET("sourceAlphaBlendFactor"); b->dstA = BLENDGET("destinationAlphaBlendFactor"); b->opA = BLENDGET("alphaBlendOperation");
            b->writeMask = BLENDGET("writeMask") & 15;
#undef BLENDGET
            if (b->enable) {
                if (b->src > 14 || b->dst > 14 || b->srcA > 14 || b->dstA > 14) {
                    msg = [NSString stringWithFormat:@"color attachment %u: dual-source blending is not supported yet", k]; goto out;
                }
                if (b->op > 4 || b->opA > 4) { msg = [NSString stringWithFormat:@"color attachment %u: blend operation", k]; goto out; }
                if (isIntPF(pf)) { msg = [NSString stringWithFormat:@"color attachment %u: blending an integer format", k]; goto out; }
                if (!pfHasAlpha(pf)) {                                   // destination alpha = 1
                    uint32_t *f[4] = { &b->src, &b->dst, &b->srcA, &b->dstA };
                    for (int i = 0; i < 4; i++) { if (*f[i] == 8) *f[i] = 1; else if (*f[i] == 9) *f[i] = 0; }
                }
            } else if (b->writeMask == 15 && opaqueBlendPF(pf) && opaqueBlendOn()) {
                // v0.58.5: opaque draws with the blender on (source x 1 + destination x 0 = the same picture).
                // --pixelrate: the GPU writes plain (unblended) pixels much slower than blended ones (BGRA8 full
                // screen 2.9 ms vs 1.6 ms, tiled and linear alike), and WindowServer's recording (21 Nov) spent 63%
                // of its GPU time in opaque draws into linear surfaces. 8/10-bit unorm formats only (no NaN/Inf in
                // the destination to multiply by 0).
                b->enable = 1; b->src = b->srcA = 1; b->dst = b->dstA = 0; b->op = b->opA = 0;
                gOpaqueBlendPipes++;
            } else b->src = b->dst = b->srcA = b->dstA = b->op = b->opA = 0;
            p->nrt = k + 1;
        }
        // v0.68.30: a fragment function without color outputs (Skia Graphite's stencil passes) writes zeros to RT0
        // in our compiler; nothing of it may reach the attachments
        if (p->fi.flags & XE_FS_NOCOLOR)
            for (unsigned k = 0; k < XE_DRAW_MAX_RT; k++) { p->blend[k].writeMask = 0; }
        // v0.51.19: a fragment function may write color outputs the pipeline has no attachment for (Core Animation's
        // glyph and mask shaders do): Metal drops those writes. Our missing render targets are SURFTYPE_NULL surfaces,
        // which the hardware discards writes to, so this is fine (it used to fail the pipeline and crash the app).
        if (p->fi.rtMask & ~((1u << p->nrt) - 1))
            os_log(gLog, "IrisXeMTL: fragment %{public}@ writes color outputs 0x%x, pipeline has %u attachment(s): extra writes dropped",
                   fnName(ff), p->fi.rtMask, p->nrt);
        // v0.34.0: depth / stencil attachments
        p->depthPF = ((NSUInteger (*)(id, SEL))objc_msgSend)(desc, sel_registerName("depthAttachmentPixelFormat"));
        p->stencilPF = ((NSUInteger (*)(id, SEL))objc_msgSend)(desc, sel_registerName("stencilAttachmentPixelFormat"));
        {
            NSUInteger db = 0; bool st = false;
            if (p->depthPF && (!dsFormat(p->depthPF, &db, &st) || !db)) {
                msg = [NSString stringWithFormat:@"depth attachment pixel format %lu not supported yet", (unsigned long)p->depthPF]; goto out;
            }
            if (p->stencilPF && (!dsFormat(p->stencilPF, &db, &st) || !st)) {
                msg = [NSString stringWithFormat:@"stencil attachment pixel format %lu not supported yet", (unsigned long)p->stencilPF]; goto out;
            }
            if ((p->fi.flags & XE_FS_DEPTH) && !p->depthPF) { msg = @"the fragment function writes [[depth]] but the pipeline has no depth attachment"; goto out; }
        }
        p->dev = [dev retain];
        snprintf(p->name, sizeof(p->name), "%s + %s", fnName(vf).UTF8String ?: "?", fnName(ff).UTF8String ?: "?");
        p->label = [((id (*)(id, SEL))objc_msgSend)(desc, sel_registerName("label")) copy];
        msg = [NSString stringWithFormat:@"%@ [%s] + %@ [%s] (code: vs 0x%llx ps 0x%llx)", fnName(vf), vlog, fnName(ff), flog,
               (unsigned long long)bufVA(p->vsBuf, 0), (unsigned long long)bufVA(p->psBuf, 0)];   // v0.42.2: for hang reports
        [msg retain];
    }
    pthread_mutex_unlock(&lock);
    {
        [gLastRenderLog release];
        gLastRenderLog = [[NSString alloc] initWithFormat:@"ok: %@", msg];
        [msg release];
        os_log(gLog, "IrisXeMTL: render pipeline %{public}@", gLastRenderLog);
        id rp = [gRPipeClass alloc];
        *(XeRPipe **)((char *)rp + ivar_getOffset(gRPipeIvar)) = p;
        return rp;
    }
out:
    [msg retain];
    pthread_mutex_unlock(&lock);
    [p->vsBuf release]; [p->psBuf release]; [p->vsScratch release]; [p->psScratch release];
    free(p);
    [gLastRenderLog release];
    gLastRenderLog = [[NSString alloc] initWithFormat:@"FAILED: %@", msg];
    os_log_error(gLog, "IrisXeMTL: render pipeline %{public}@", gLastRenderLog);
    if (err) *err = [NSError errorWithDomain:@"IrisXeMTL" code:2 userInfo:@{ NSLocalizedDescriptionKey: msg }];
    [msg release];
    return nil;
}
static id xe_newRPS(id self, SEL _cmd, id desc, NSError **err) { return newRenderPipeline(self, desc, err); }
static id xe_newRPSOpts(id self, SEL _cmd, id desc, NSUInteger opts, id *refl, NSError **err) {
    if (refl) *refl = nil;
    return newRenderPipeline(self, desc, err);
}
static void xe_newRPSHandler(id self, SEL _cmd, id desc, void (^handler)(id, NSError *)) {
    NSError *e = nil;
    id p = newRenderPipeline(self, desc, &e);
    if (handler) handler(p, e);
    [p release];
}
static void xe_newRPSOptsHandler(id self, SEL _cmd, id desc, NSUInteger opts, void (^handler)(id, id, NSError *)) {
    NSError *e = nil;
    id p = newRenderPipeline(self, desc, &e);
    if (handler) handler(p, nil, e);
    [p release];
}
static NSString *xe_lastRenderLog(id self, SEL _cmd) { return gLastRenderLog; }
static void xe_rpipeDealloc(id self, SEL _cmd) {
    XeRPipe *p = rpipe(self);
    if (p) { [p->vsBuf release]; [p->psBuf release]; [p->vsScratch release]; [p->psScratch release]; [p->dev release]; [p->label release]; free(p); }
    struct objc_super sup = { self, [NSObject class] };
    ((void (*)(struct objc_super *, SEL))objc_msgSendSuper)(&sup, _cmd);
}
static id xe_rpipeDevice(id s, SEL c) { return rpipe(s)->dev; }
static id xe_rpipeLabel(id s, SEL c) { return rpipe(s)->label; }

// ---- render command encoder ----------------------------------------------------
typedef struct {
    id pipe;
    id vbuf[31], fbuf[31]; NSUInteger voff[31], foff[31];
    id ftex[XE_DRAW_MAX_FTEX], fsamp[XE_DRAW_MAX_SAMP];
    id vtex[XE_DRAW_MAX_TEX], vsamp[XE_DRAW_MAX_SAMP];   // v0.47.0
    uint8_t ftexBase[XE_DRAW_MAX_FTEX];        // v0.37.0: 0, or base level + 1 read alone (generateMipmaps)
    id rt[XE_DRAW_MAX_RT]; uint32_t clear[XE_DRAW_MAX_RT][4]; uint32_t clearPending; uint32_t rtLevel[XE_DRAW_MAX_RT];
    id segCB; uint32_t segDraws;                  // v0.49.0: the open segment of this encoder's draws (batching)
    uint32_t width, height;
    MTLViewport vp; bool haveVP;
    NSUInteger cull, winding, fill;
    float blendColor[4];                  // v0.33.0
    MTLScissorRect sc; bool haveSC;
    // v0.34.0: depth / stencil attachments and state
    id depthTex, stencilTex; bool clearDepth, clearStencil; float depthClear; uint32_t stencilClear;
    id dss; uint32_t frontRef, backRef; float bias[3]; NSUInteger clipMode;
    NSMutableArray *keep;                 // setVertexBytes / setFragmentBytes buffers
    // v0.66.0: multisample resolve (a copy: multisample textures have one sample, see xe_newTexture)
    id resolve[XE_DRAW_MAX_RT]; uint32_t resolveLevel[XE_DRAW_MAX_RT]; uint32_t resolveOn;
} XeRState;
static XeRState *rstate(id enc) {
    XeRState **slot = (XeRState **)((char *)enc + ivar_getOffset(gRenderIvar));
    if (!*slot) *slot = calloc(1, sizeof(XeRState));
    return *slot;
}

static id xe_renderCommandEncoder(id self, SEL _cmd, id pass) {
    id enc = ((id (*)(id, SEL, id))objc_msgSend)([gRenderClass alloc], sel_registerName("initWithCommandBuffer:"), self);
    if (!enc) return nil;
    ((void (*)(id, SEL, id))objc_msgSend)(self, sel_registerName("setCurrentCommandEncoder:"), enc);
    XeRState *st = rstate(enc);
    st->keep = [[NSMutableArray alloc] init];
    st->cull = MTLCullModeNone; st->winding = MTLWindingClockwise; st->fill = MTLTriangleFillModeFill;
    id cas = ((id (*)(id, SEL))objc_msgSend)(pass, sel_registerName("colorAttachments"));
    for (unsigned k = 0; k < XE_DRAW_MAX_RT; k++) {
        id ca = ((id (*)(id, SEL, NSUInteger))objc_msgSend)(cas, sel_registerName("objectAtIndexedSubscript:"), k);
        id t = ca ? ((id (*)(id, SEL))objc_msgSend)(ca, sel_registerName("texture")) : nil;
        if (!t) continue;
        if (object_getClass(t) != gTexClass) { os_log_error(gLog, "IrisXeMTL: color attachment %u: not our texture", k); continue; }
        NSUInteger level = ((NSUInteger (*)(id, SEL))objc_msgSend)(ca, sel_registerName("level"));
        NSUInteger slice = ((NSUInteger (*)(id, SEL))objc_msgSend)(ca, sel_registerName("slice"));
        XeTex *x = tex(t);
        if (slice || x->type != 2) os_log_error(gLog, "IrisXeMTL: color attachment %u: only 2D textures (slice 0) yet", k);
        if (level >= x->levels) { os_log_error(gLog, "IrisXeMTL: color attachment %u: level %lu", k, (unsigned long)level); level = 0; }
        st->rt[k] = [t retain];
        st->rtLevel[k] = (uint32_t)level;                       // v0.36.0: draw into a mip level
        if (!st->width || x->lvlW[level] < st->width) st->width = (uint32_t)x->lvlW[level];
        if (!st->height || x->lvlH[level] < st->height) st->height = (uint32_t)x->lvlH[level];
        id rv = ((id (*)(id, SEL))objc_msgSend)(ca, sel_registerName("resolveTexture"));   // v0.66.0
        if (rv && object_getClass(rv) == gTexClass) {
            NSUInteger store = ((NSUInteger (*)(id, SEL))objc_msgSend)(ca, sel_registerName("storeAction"));
            st->resolve[k] = [rv retain];
            st->resolveLevel[k] = (uint32_t)((NSUInteger (*)(id, SEL))objc_msgSend)(ca, sel_registerName("resolveLevel"));
            // MultisampleResolve 2, StoreAndMultisampleResolve 3, Unknown 4 (set later: setColorStoreAction:atIndex:)
            if (store == 2 || store == 3 || store == 4) st->resolveOn |= 1u << k;
        }
        NSUInteger load = ((NSUInteger (*)(id, SEL))objc_msgSend)(ca, sel_registerName("loadAction"));
        if (load == MTLLoadActionClear) {
            // a 32-byte struct: a plain objc_msgSend cast crashes on x86_64 (needs _stret), so call it typed
            MTLClearColor cc = [(MTLRenderPassColorAttachmentDescriptor *)ca clearColor];
            double v[4] = { cc.red, cc.green, cc.blue, cc.alpha };
            for (int c = 0; c < 4; c++) {
                if (isIntPF(x->format)) st->clear[k][c] = (uint32_t)(int32_t)v[c];
                else { float f = (float)v[c]; memcpy(&st->clear[k][c], &f, 4); }
            }
            st->clearPending |= 1u << k;
        }
    }
    NSUInteger rw = ((NSUInteger (*)(id, SEL))objc_msgSend)(pass, sel_registerName("renderTargetWidth"));
    NSUInteger rh = ((NSUInteger (*)(id, SEL))objc_msgSend)(pass, sel_registerName("renderTargetHeight"));
    if (rw && rw < st->width) st->width = (uint32_t)rw;
    if (rh && rh < st->height) st->height = (uint32_t)rh;
    // v0.34.0: depth / stencil attachments (Depth32Float_Stencil8: the same texture in both)
    for (int which = 0; which < 2; which++) {
        id a = ((id (*)(id, SEL))objc_msgSend)(pass, sel_registerName(which ? "stencilAttachment" : "depthAttachment"));
        id t = a ? ((id (*)(id, SEL))objc_msgSend)(a, sel_registerName("texture")) : nil;
        if (!t) continue;
        XeTex *x = object_getClass(t) == gTexClass ? tex(t) : NULL;
        if (!x || !(which ? x->isStencil : x->isDepth)) {
            os_log_error(gLog, "IrisXeMTL: %{public}s attachment: not one of our %{public}s textures", which ? "stencil" : "depth",
                         which ? "stencil" : "depth");
            continue;
        }
        NSUInteger level = ((NSUInteger (*)(id, SEL))objc_msgSend)(a, sel_registerName("level"));
        NSUInteger slice = ((NSUInteger (*)(id, SEL))objc_msgSend)(a, sel_registerName("slice"));
        if (level || slice) os_log_error(gLog, "IrisXeMTL: depth / stencil attachment: level / slice not supported yet (0 used)");
        if (which) st->stencilTex = [t retain]; else st->depthTex = [t retain];
        if (!st->width || x->width < st->width) st->width = (uint32_t)x->width;
        if (!st->height || x->height < st->height) st->height = (uint32_t)x->height;
        NSUInteger load = ((NSUInteger (*)(id, SEL))objc_msgSend)(a, sel_registerName("loadAction"));
        if (load == MTLLoadActionClear) {
            if (which) { st->clearStencil = true; st->stencilClear = [(MTLRenderPassStencilAttachmentDescriptor *)a clearStencil] & 0xFF; }
            else {
                double v = [(MTLRenderPassDepthAttachmentDescriptor *)a clearDepth];
                st->clearDepth = true; st->depthClear = (float)(v < 0 ? 0 : v > 1 ? 1 : v);
            }
        }
    }
    return [enc autorelease];
}
static void xe_renderDealloc(id self, SEL _cmd) {
    XeRState **slot = (XeRState **)((char *)self + ivar_getOffset(gRenderIvar));
    XeRState *st = *slot;
    if (st) {
        [st->pipe release];
        for (int i = 0; i < 31; i++) { [st->vbuf[i] release]; [st->fbuf[i] release]; }
        for (int i = 0; i < XE_DRAW_MAX_FTEX; i++) [st->ftex[i] release];
        for (int i = 0; i < XE_DRAW_MAX_SAMP; i++) [st->fsamp[i] release];
        for (int i = 0; i < XE_DRAW_MAX_TEX; i++) [st->vtex[i] release];
        for (int i = 0; i < XE_DRAW_MAX_SAMP; i++) [st->vsamp[i] release];
        for (int i = 0; i < XE_DRAW_MAX_RT; i++) { [st->rt[i] release]; [st->resolve[i] release]; }
        [st->depthTex release]; [st->stencilTex release]; [st->dss release];
        [st->keep release];
        free(st);
        *slot = NULL;
    }
    struct objc_super sup = { self, class_getSuperclass(gRenderClass) };
    ((void (*)(struct objc_super *, SEL))objc_msgSendSuper)(&sup, _cmd);
}
static void xe_rSetPipeline(id self, SEL _cmd, id pipe) {
    if (pipe && !rpipe(pipe)) { os_log_error(gLog, "IrisXeMTL: render pipeline is not ours"); pipe = nil; }
    XeRState *st = rstate(self);
    [pipe retain]; [st->pipe release]; st->pipe = pipe;
}
static void setBuf(id *slots, NSUInteger *offs, id buf, NSUInteger off, NSUInteger idx) {
    if (idx >= 31) return;
    [buf retain]; [slots[idx] release]; slots[idx] = buf; offs[idx] = off;
}
static void xe_rSetVB(id self, SEL _cmd, id b, NSUInteger off, NSUInteger idx) { XeRState *s = rstate(self); setBuf(s->vbuf, s->voff, b, off, idx); }
static void xe_rSetFB(id self, SEL _cmd, id b, NSUInteger off, NSUInteger idx) { XeRState *s = rstate(self); setBuf(s->fbuf, s->foff, b, off, idx); }
static void xe_rSetVBOff(id self, SEL _cmd, NSUInteger off, NSUInteger idx) { if (idx < 31) rstate(self)->voff[idx] = off; }
static void xe_rSetFBOff(id self, SEL _cmd, NSUInteger off, NSUInteger idx) { if (idx < 31) rstate(self)->foff[idx] = off; }
static void xe_rSetVBs(id self, SEL _cmd, id const *bs, const NSUInteger *offs, NSRange r) {
    for (NSUInteger i = 0; i < r.length; i++) xe_rSetVB(self, _cmd, bs[i], offs ? offs[i] : 0, r.location + i);
}
static void xe_rSetFBs(id self, SEL _cmd, id const *bs, const NSUInteger *offs, NSRange r) {
    for (NSUInteger i = 0; i < r.length; i++) xe_rSetFB(self, _cmd, bs[i], offs ? offs[i] : 0, r.location + i);
}
static id bytesBuffer(id enc, const void *bytes, NSUInteger len) {
    id cb = ((id (*)(id, SEL))objc_msgSend)(enc, sel_registerName("commandBuffer"));
    id dev = ((id (*)(id, SEL))objc_msgSend)(cb, sel_registerName("device"));
    id b = ((id (*)(id, SEL, const void *, NSUInteger, NSUInteger))objc_msgSend)(dev, sel_registerName("newBufferWithBytes:length:options:"),
                                                                                bytes, len ? len : 4, MTLResourceStorageModeShared);
    if (b) {
        XeRState *st = rstate(enc);
        if (!st->keep.count)                                     // lives as long as the command buffer (the GPU reads it later)
            objc_setAssociatedObject(cb, st->keep, st->keep, OBJC_ASSOCIATION_RETAIN);
        [st->keep addObject:b]; [b release];
    }
    return b;
}
static void xe_rSetVBytes(id self, SEL _cmd, const void *bytes, NSUInteger len, NSUInteger idx) {
    xe_rSetVB(self, _cmd, bytesBuffer(self, bytes, len), 0, idx);
}
static void xe_rSetFBytes(id self, SEL _cmd, const void *bytes, NSUInteger len, NSUInteger idx) {
    xe_rSetFB(self, _cmd, bytesBuffer(self, bytes, len), 0, idx);
}
static void xe_rSetFTex(id self, SEL _cmd, id t, NSUInteger idx) {
    if (idx >= XE_DRAW_MAX_FTEX) { os_log_error(gLog, "IrisXeMTL: fragment texture %lu: only 0..15 yet", (unsigned long)idx); return; }
    if (t && object_getClass(t) != gTexClass) { os_log_error(gLog, "IrisXeMTL: setFragmentTexture: not our texture"); t = nil; }
    XeRState *st = rstate(self);
    [t retain]; [st->ftex[idx] release]; st->ftex[idx] = t;
}
static void xe_rSetFTexs(id self, SEL _cmd, id const *ts, NSRange r) {
    for (NSUInteger i = 0; i < r.length; i++) xe_rSetFTex(self, _cmd, ts[i], r.location + i);
}
static void xe_rSetFSamp(id self, SEL _cmd, id s, NSUInteger idx) {
    if (idx >= XE_DRAW_MAX_SAMP) return;
    if (s && object_getClass(s) != gSampClass) { os_log_error(gLog, "IrisXeMTL: setFragmentSamplerState: not our sampler"); s = nil; }
    XeRState *st = rstate(self);
    [s retain]; [st->fsamp[idx] release]; st->fsamp[idx] = s;
}
static void xe_rSetFSamps(id self, SEL _cmd, id const *ss, NSRange r) {
    for (NSUInteger i = 0; i < r.length; i++) xe_rSetFSamp(self, _cmd, ss[i], r.location + i);
}
static void xe_rSetFSampClamp(id self, SEL _cmd, id s, float lo, float hi, NSUInteger idx) { xe_rSetFSamp(self, _cmd, s, idx); }
// v0.47.0: vertex textures / samplers
static void xe_rSetVTex(id self, SEL _cmd, id t, NSUInteger idx) {
    if (idx >= XE_DRAW_MAX_TEX) { os_log_error(gLog, "IrisXeMTL: vertex texture %lu: only 0..7 yet", (unsigned long)idx); return; }
    if (t && object_getClass(t) != gTexClass) { os_log_error(gLog, "IrisXeMTL: setVertexTexture: not our texture"); t = nil; }
    XeRState *st = rstate(self);
    [t retain]; [st->vtex[idx] release]; st->vtex[idx] = t;
}
static void xe_rSetVTexs(id self, SEL _cmd, id const *ts, NSRange r) {
    for (NSUInteger i = 0; i < r.length; i++) xe_rSetVTex(self, _cmd, ts[i], r.location + i);
}
static void xe_rSetVSamp(id self, SEL _cmd, id s, NSUInteger idx) {
    if (idx >= XE_DRAW_MAX_SAMP) return;
    if (s && object_getClass(s) != gSampClass) s = nil;
    XeRState *st = rstate(self);
    [s retain]; [st->vsamp[idx] release]; st->vsamp[idx] = s;
}
static void xe_rSetVSamps(id self, SEL _cmd, id const *ss, NSRange r) {
    for (NSUInteger i = 0; i < r.length; i++) xe_rSetVSamp(self, _cmd, ss[i], r.location + i);
}
static void xe_rSetVSampClamp(id self, SEL _cmd, id s, float lo, float hi, NSUInteger idx) { xe_rSetVSamp(self, _cmd, s, idx); }
static void xe_rSetViewport(id self, SEL _cmd, MTLViewport v) { XeRState *s = rstate(self); s->vp = v; s->haveVP = true; }
static void xe_rSetViewports(id self, SEL _cmd, const MTLViewport *v, NSUInteger n) { if (n) xe_rSetViewport(self, _cmd, v[0]); }
static void xe_rSetCull(id self, SEL _cmd, NSUInteger m) { rstate(self)->cull = m; }
static void xe_rSetWinding(id self, SEL _cmd, NSUInteger w) { rstate(self)->winding = w; }
static void xe_rSetFill(id self, SEL _cmd, NSUInteger f) { rstate(self)->fill = f; }
static void xe_rSetBlendColor(id self, SEL _cmd, float r, float g, float b, float a) {
    XeRState *s = rstate(self); s->blendColor[0] = r; s->blendColor[1] = g; s->blendColor[2] = b; s->blendColor[3] = a;
}
static void xe_rSetScissor(id self, SEL _cmd, MTLScissorRect r) { XeRState *s = rstate(self); s->sc = r; s->haveSC = true; }
static void xe_rSetScissors(id self, SEL _cmd, const MTLScissorRect *r, NSUInteger n) { if (n) xe_rSetScissor(self, _cmd, r[0]); }
// v0.34.0: depth / stencil state
typedef struct { id dev, label; uint32_t depthFunc, depthWrite; struct XeDrawStencil front, back; } XeDSS;
static Class gDSSClass; static Ivar gDSSIvar;
static XeDSS *dssOf(id s) { return s && object_getClass(s) == gDSSClass ? *(XeDSS **)((char *)s + ivar_getOffset(gDSSIvar)) : NULL; }
static void stencilFace(id sd, struct XeDrawStencil *f) {
    if (!sd) { f->func = 7; f->fail = f->depthFail = f->pass = 0; f->readMask = f->writeMask = 0xFF; return; }   // Always, Keep
#define SGET(n) (uint32_t)((NSUInteger (*)(id, SEL))objc_msgSend)(sd, sel_registerName(n))
    f->func = SGET("stencilCompareFunction") & 7; f->fail = SGET("stencilFailureOperation") & 7;
    f->depthFail = SGET("depthFailureOperation") & 7; f->pass = SGET("depthStencilPassOperation") & 7;
    f->readMask = SGET("readMask") & 0xFF; f->writeMask = SGET("writeMask") & 0xFF;
#undef SGET
}
static id xe_newDSS(id self, SEL _cmd, id desc) {
    XeDSS *x = calloc(1, sizeof(XeDSS));
    x->depthFunc = (uint32_t)((NSUInteger (*)(id, SEL))objc_msgSend)(desc, sel_registerName("depthCompareFunction")) & 7;
    x->depthWrite = ((BOOL (*)(id, SEL))objc_msgSend)(desc, sel_registerName("isDepthWriteEnabled")) ? 1 : 0;
    stencilFace(((id (*)(id, SEL))objc_msgSend)(desc, sel_registerName("frontFaceStencil")), &x->front);
    stencilFace(((id (*)(id, SEL))objc_msgSend)(desc, sel_registerName("backFaceStencil")), &x->back);
    x->dev = [self retain];
    x->label = [((id (*)(id, SEL))objc_msgSend)(desc, sel_registerName("label")) copy];
    id o = [gDSSClass alloc];
    *(XeDSS **)((char *)o + ivar_getOffset(gDSSIvar)) = x;
    return o;
}
static void xe_dssDealloc(id self, SEL _cmd) {
    XeDSS *x = dssOf(self);
    if (x) { [x->dev release]; [x->label release]; free(x); }
    struct objc_super sup = { self, [NSObject class] };
    ((void (*)(struct objc_super *, SEL))objc_msgSendSuper)(&sup, _cmd);
}
static id xe_dssDevice(id s, SEL c) { return dssOf(s)->dev; }
static id xe_dssLabel(id s, SEL c) { return dssOf(s)->label; }
static void xe_rSetDSS(id self, SEL _cmd, id dss) {
    if (dss && !dssOf(dss)) { os_log_error(gLog, "IrisXeMTL: depth stencil state is not ours"); dss = nil; }
    XeRState *s = rstate(self); [dss retain]; [s->dss release]; s->dss = dss;
}
static void xe_rSetStencilRef(id self, SEL _cmd, uint32_t r) { XeRState *s = rstate(self); s->frontRef = s->backRef = r & 0xFF; }
static void xe_rSetStencilRefs(id self, SEL _cmd, uint32_t f, uint32_t b) { XeRState *s = rstate(self); s->frontRef = f & 0xFF; s->backRef = b & 0xFF; }
static void xe_rSetDepthBias(id self, SEL _cmd, float c, float sl, float cl) { XeRState *s = rstate(self); s->bias[0] = c; s->bias[1] = sl; s->bias[2] = cl; }
static void xe_rSetDepthClip(id self, SEL _cmd, NSUInteger m) { rstate(self)->clipMode = m; }

static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static uint64_t bufVA(id b, NSUInteger off) {
    return b ? ((uint64_t (*)(id, SEL))objc_msgSend)(b, sel_registerName("gpuAddress")) + off : 0;
}
static NSUInteger bufLen(id b) { return b ? ((NSUInteger (*)(id, SEL))objc_msgSend)(b, sel_registerName("length")) : 0; }

// one draw (vertexCount 0: only the pending clear)

// ---- v0.44.0: record every draw of an app (IRISXE_RECORD=1) and replay it (irisxe-mtkdemo --replay) ----
// File: <tmp>/irisxe-record.bin, one record per draw:
//   XeRecHdr, name (nameLen bytes), XeDrawCmd, then nreg x { XeRecReg, data (hasData ? len : 0 bytes) }
// Every buffer the draw uses is a region (GPU address + length); contents are kept up to 1 MB
// (bigger ones - render targets, depth buffers - replay as zeros).
typedef struct { uint32_t magic, total, nreg, nameLen, cmdSize, index; } XeRecHdr;
typedef struct { uint64_t va, len; uint32_t hasData, pad; } XeRecReg;
#define XE_REC_MAGIC 0x43455258u                  // "XREC"
static FILE *gRecFile;
static uint32_t gRecCount;
static void recordCmd(const char *name, const void *k, uint32_t ksize, id *res, int nres) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSString *path = [NSTemporaryDirectory() stringByAppendingPathComponent:@"irisxe-record.bin"];
        if (gDesktopRecord)                                   // v0.51.33: the desktop test: one file per program, readable
            path = [NSString stringWithFormat:@"/var/tmp/irisxe-record-%s.bin", getprogname()];
        gRecFile = fopen(path.fileSystemRepresentation, "wb");
        if (gRecFile && gDesktopRecord) chmod(path.fileSystemRepresentation, 0644);
        if (!gRecFile) { path = @"/tmp/irisxe-record.bin"; gRecFile = fopen(path.fileSystemRepresentation, "wb"); }
        os_log(gLog, "IrisXeMTL: recording draws to %{public}@ (%s)", path, gRecFile ? "ok" : "can't write");
    });
    static uint64_t recBytes;                                 // v0.51.33: at most 600 MB per program (v0.51.34: 1.5 GB)
    if (!gRecFile || gRecCount >= 400000 || recBytes > (1536ull << 20)) return;
    @synchronized ([NSObject class]) {
        if (nres > 64) nres = 64;
        XeRecHdr h = { XE_REC_MAGIC, 0, (uint32_t)nres, (uint32_t)strlen(name) + 1, ksize, gRecCount++ };
        uint32_t total = (uint32_t)sizeof(h) + h.nameLen + h.cmdSize;
        XeRecReg rr[64];
        const void *data[64];
        for (int i = 0; i < nres; i++) {
            rr[i].va = bufVA(res[i], 0); rr[i].len = bufLen(res[i]); rr[i].pad = 0;
            data[i] = rr[i].len <= (1u << 20) ? ((void *(*)(id, SEL))objc_msgSend)(res[i], sel_registerName("contents")) : NULL;
            // v0.51.34: a buffer's contents only when they changed since they were last written (the replay keeps
            // buffers between records, so an unchanged one is still there); before, every draw repeated every buffer
            if (data[i]) {
                static NSMutableDictionary *seen;
                if (!seen) seen = [[NSMutableDictionary alloc] init];
                uint64_t hsh = 1469598103934665603ull;
                const uint64_t *q = data[i]; size_t nq = rr[i].len / 8;
                for (size_t k = 0; k < nq; k++) hsh = (hsh ^ q[k]) * 1099511628211ull;
                for (size_t k = nq * 8; k < rr[i].len; k++) hsh = (hsh ^ ((const uint8_t *)data[i])[k]) * 1099511628211ull;
                NSString *key = [NSString stringWithFormat:@"%llx:%llx", rr[i].va, rr[i].len];
                NSNumber *old = seen[key];
                if (old && old.unsignedLongLongValue == hsh) data[i] = NULL;
                else seen[key] = @(hsh);
            }
            rr[i].hasData = data[i] != NULL;
            total += sizeof(XeRecReg) + (rr[i].hasData ? (uint32_t)rr[i].len : 0);
        }
        h.total = total;
        recBytes += total;
        fwrite(&h, sizeof(h), 1, gRecFile);
        fwrite(name, h.nameLen, 1, gRecFile);
        fwrite(k, ksize, 1, gRecFile);
        for (int i = 0; i < nres; i++) {
            fwrite(&rr[i], sizeof(rr[i]), 1, gRecFile);
            if (rr[i].hasData) fwrite(data[i], rr[i].len, 1, gRecFile);
        }
        fflush(gRecFile);
    }
}
static uint64_t recRemap(uint64_t va, const XeRecReg *rr, const uint64_t *nva, int n) {
    if (!va) return 0;
    for (int i = 0; i < n; i++)
        if (va >= rr[i].va && va <= rr[i].va + rr[i].len) return nva[i] + (va - rr[i].va);
    return va;
}
// Replay one record (a draw or a blit) in this process (variant bits: 1 no clears, 2 no depth / stencil buffers,
// 4 no draw (clears only), 8 first triangle only, 16 no stencil test, 32 depth test always / no write,
// 64 no blending, 128 fresh buffers for this record (not the ones kept from earlier records),
// 0x80000000 forget the kept buffers and return). Returns 0 when submitted and finished, 1 = skipped (compute).
// v0.44.2: buffers are kept between records (same recorded address + length = same buffer), like in the app,
// so what a blit writes is there for the draws after it.
static NSMutableDictionary *gRepBufs;
static uint64_t gRepBytes;
static id gRepBatchCB; static unsigned gRepBatchN;       // v0.51.35: batched replay
__attribute__((visibility("default")))
int IrisXeReplayFlush(id dev) {
    if (!gRepBatchCB) return 0;
    endKernelCommand(gRepBatchCB);
    ((void (*)(id, SEL))objc_msgSend)(gRepBatchCB, sel_registerName("commit"));
    ((void (*)(id, SEL))objc_msgSend)(gRepBatchCB, sel_registerName("waitUntilCompleted"));
    long st = ((long (*)(id, SEL))objc_msgSend)(gRepBatchCB, sel_registerName("status"));
    [gRepBatchCB release]; gRepBatchCB = nil;
    int n = (int)gRepBatchN; gRepBatchN = 0;
    return st == 4 ? n : -n;
}
__attribute__((visibility("default")))
int IrisXeReplayDraw(id dev, const void *recp, size_t len, uint32_t variant, char *info, size_t infoLen) {
    if (variant & 0x80000000u) { [gRepBufs release]; gRepBufs = nil; gRepBytes = 0; return 0; }
    const uint8_t *r = (const uint8_t *)recp;
    if (len < sizeof(XeRecHdr)) return -1;
    XeRecHdr h; memcpy(&h, r, sizeof(h));
    if (h.magic != XE_REC_MAGIC || h.total > len || h.nreg > 64 || h.cmdSize < 8 || h.cmdSize > sizeof(XeDrawCmd) + 65536) return -2;
    const char *name = (const char *)(r + sizeof(h));
    const uint8_t *cmd = r + sizeof(h) + h.nameLen;
    uint32_t type; memcpy(&type, cmd, 4);
    if (type == 0 && h.cmdSize == sizeof(XeDrawCmd)) type = XE_KCMD_DRAW;   // v0.44.3: draws are recorded before their type is set
    size_t off = sizeof(h) + h.nameLen + h.cmdSize;
    if (type == XE_KCMD_DISPATCH) { if (info) snprintf(info, infoLen, "#%u %s", h.index, name); return 1; }
    if (!gRepBufs) gRepBufs = [[NSMutableDictionary alloc] init];
    if (gRepBytes > (1500ull << 20)) { [gRepBufs removeAllObjects]; gRepBytes = 0; }
    XeRecReg rr[64]; id bufs[64]; uint64_t nva[64];
    int n = 0;
    for (uint32_t i = 0; i < h.nreg; i++) {
        if (off + sizeof(XeRecReg) > len) return -3;
        memcpy(&rr[n], r + off, sizeof(XeRecReg)); off += sizeof(XeRecReg);
        NSUInteger blen = (NSUInteger)(rr[n].len ? rr[n].len : 4096);
        NSString *key = [NSString stringWithFormat:@"%llx:%llx", rr[n].va, rr[n].len];
        id b = (variant & 128) ? nil : gRepBufs[key];
        if (!b) {
            b = [makeBuffer(dev, NULL, blen, 0, nil) autorelease];
            if (!b) return -4;
            if (!(variant & 128)) { gRepBufs[key] = b; gRepBytes += blen; }
        }
        if (rr[n].hasData) {
            void *c = ((void *(*)(id, SEL))objc_msgSend)(b, sel_registerName("contents"));
            if (c) memcpy(c, r + off, rr[n].len);
            off += rr[n].len;
        }
        bufs[n] = b; nva[n] = bufVA(b, 0); n++;
    }
#define remap(v) recRemap((v), rr, nva, n)
    static id q;
    if (!q) q = ((id (*)(id, SEL))objc_msgSend)(dev, sel_registerName("newCommandQueue"));
    if (type != XE_KCMD_DRAW && gRepBatchCB) IrisXeReplayFlush(dev);   // v0.51.35: a blit ends the batch
    if (type != XE_KCMD_DRAW) {                       // v0.44.2: blits
        uint8_t kb[64]; uint32_t ks = h.cmdSize > sizeof(kb) ? sizeof(kb) : h.cmdSize;
        memcpy(kb, cmd, ks);
        if (type == XE_KCMD_FILL && ks >= sizeof(XeFillCmd)) {
            XeFillCmd *f = (XeFillCmd *)kb; f->gpuVA = remap(f->gpuVA);
            if (info) snprintf(info, infoLen, "#%u %s: %llu bytes", h.index, name, f->length);
        } else if (type == XE_KCMD_COPY && ks >= sizeof(XeCopyCmd)) {
            XeCopyCmd *c = (XeCopyCmd *)kb; c->srcVA = remap(c->srcVA); c->dstVA = remap(c->dstVA);
            if (info) snprintf(info, infoLen, "#%u %s: %llu bytes", h.index, name, c->length);
        } else if (type == XE_KCMD_COPY2D && ks >= sizeof(XeCopy2DCmd)) {
            XeCopy2DCmd *c = (XeCopy2DCmd *)kb; c->srcVA = remap(c->srcVA); c->dstVA = remap(c->dstVA);
            if (info) snprintf(info, infoLen, "#%u %s: %u bytes x %u rows", h.index, name, c->widthBytes, c->height);
        } else return -6;
        id cb = ((id (*)(id, SEL))objc_msgSend)(q, sel_registerName("commandBuffer"));
        void *out = beginKernelCommand(cb, bufs, n, ks);
        if (!out) return -5;
        memcpy(out, kb, ks);
        endKernelCommand(cb);
        ((void (*)(id, SEL))objc_msgSend)(cb, sel_registerName("commit"));
        ((void (*)(id, SEL))objc_msgSend)(cb, sel_registerName("waitUntilCompleted"));
        return 0;
    }
    if (h.cmdSize != sizeof(XeDrawCmd)) return -2;
    XeDrawCmd k; memcpy(&k, cmd, sizeof(k));
    struct XeDrawDesc *d = &k.d;
    // v0.51.30: which replay buffer holds render target 0 (to count its pixels after the draw)
    int rt0 = -1; uint64_t rt0Off = 0;
    if (d->nrt && d->rt[0].va)
        for (int i = 0; i < n; i++)
            if (d->rt[0].va >= rr[i].va && d->rt[0].va < rr[i].va + (rr[i].len ? rr[i].len : 4096)) { rt0 = i; rt0Off = d->rt[0].va - rr[i].va; break; }
    uint32_t rtW = d->rt[0].width, rtH = d->rt[0].height, rtPitch = d->rt[0].pitch;
    d->vsVA = remap(d->vsVA); d->psVA = remap(d->psVA);
    for (uint32_t i = 0; i < d->nvb && i < XE_DRAW_MAX_VB; i++) d->vb[i].va = remap(d->vb[i].va);
    for (int i = 0; i < 16; i++) { d->vsBuf[i] = (uint32_t)remap(d->vsBuf[i]); d->psBuf[i] = (uint32_t)remap(d->psBuf[i]); }
    for (uint32_t i = 0; i < d->nrt && i < XE_DRAW_MAX_RT; i++) d->rt[i].va = remap(d->rt[i].va);
    for (uint32_t i = 0; i < d->ntex && i < XE_DRAW_MAX_FTEX; i++) d->tex[i].va = remap(d->tex[i].va);
    for (uint32_t i = 0; i < d->nvtex && i < XE_DRAW_MAX_TEX; i++) d->vtex[i].va = remap(d->vtex[i].va);
    d->ibVA = remap(d->ibVA);
    d->ds.depthVA = remap(d->ds.depthVA); d->ds.stencilVA = remap(d->ds.stencilVA);
    if (variant & 1) { d->clearMask = 0; d->ds.clearDepth = d->ds.clearStencil = 0; }
    if (variant & 2) { memset(&d->ds, 0, sizeof(d->ds)); d->ds.depthFunc = 7; }      // MTLCompareFunctionAlways
    if (variant & 4) d->vertexCount = 0;
    if ((variant & 8) && d->vertexCount > 3) d->vertexCount = 3;
    if (variant & 16) { d->ds.front.func = d->ds.back.func = 7; d->ds.front.writeMask = d->ds.back.writeMask = 0; }
    if (variant & 32) { d->ds.depthFunc = 7; d->ds.depthWrite = 0; }
    if (variant & 64) for (int i = 0; i < XE_DRAW_MAX_RT; i++) d->blend[i].enable = 0;
    if (info) snprintf(info, infoLen, "#%u %s: %u vertices x %u, clear 0x%x, %ux%u, %u RT %u tex %u VB, depth %s stencil %s, %d buffers",
                       h.index, name, d->vertexCount, d->instanceCount, d->clearMask, d->width, d->height, d->nrt, d->ntex, d->nvb,
                       d->ds.depthVA ? "yes" : "no", d->ds.stencilVA ? "yes" : "no", n);

    // v0.51.35: variant 0x100: batched like a live render encoder - this draw goes into the open segment (up to 16
    // draws, one GPU job); IrisXeReplayFlush() ends the segment, commits and waits
    if (variant & 0x100) {
        const unsigned long csize = (sizeof(XeDrawCmd) + 63) & ~63ul;
        bool fresh = !gRepBatchCB || gRepBatchN >= 16;
        if (fresh && gRepBatchCB) IrisXeReplayFlush(dev);
        if (!gRepBatchCB) gRepBatchCB = [((id (*)(id, SEL))objc_msgSend)(q, sel_registerName("commandBuffer")) retain];
        XeDrawCmd *bo = kernelCommandIn(gRepBatchCB, bufs, n, csize, fresh);
        if (!bo) return -5;
        memcpy(bo, &k, sizeof(k)); bo->type = XE_KCMD_DRAW; bo->size = (uint32_t)csize;
        gRepBatchN = fresh ? 1 : gRepBatchN + 1;
        return 0;
    }
    id cb = ((id (*)(id, SEL))objc_msgSend)(q, sel_registerName("commandBuffer"));
    XeDrawCmd *out = beginKernelCommand(cb, bufs, n, sizeof(XeDrawCmd));
    if (!out) return -5;
    memcpy(out, &k, sizeof(k)); out->type = XE_KCMD_DRAW; out->size = sizeof(XeDrawCmd);
    endKernelCommand(cb);
    ((void (*)(id, SEL))objc_msgSend)(cb, sel_registerName("commit"));
    ((void (*)(id, SEL))objc_msgSend)(cb, sel_registerName("waitUntilCompleted"));
    // v0.51.30: what the draw left in render target 0: non-zero pixels, and the first one
    if (info && rt0 >= 0 && rtPitch && rtW && rtH) {
        const uint8_t *c = ((void *(*)(id, SEL))objc_msgSend)(bufs[rt0], sel_registerName("contents"));
        uint64_t blen = rr[rt0].len ? rr[rt0].len : 4096;
        unsigned long nz = 0; uint32_t first = 0; int fx = -1, fy = -1;
        if (c && rtW <= 16384 && rtH <= 16384)
            for (uint32_t y = 0; y < rtH; y++) {
                uint64_t rowOff = rt0Off + (uint64_t)y * rtPitch;
                if (rowOff + rtW * 4ull > blen) break;
                const uint32_t *row = (const uint32_t *)(c + rowOff);
                for (uint32_t x = 0; x < rtW; x++) if (row[x]) { if (!nz++) { first = row[x]; fx = x; fy = y; } }
            }
        long st = ((long (*)(id, SEL))objc_msgSend)(cb, sel_registerName("status"));
        id er = ((id (*)(id, SEL))objc_msgSend)(cb, sel_registerName("error"));
        size_t used = strlen(info);
        snprintf(info + used, infoLen - used, " -> status %ld%s, RT0 %lu non-zero pixels (first %08x at %d,%d)", st,
                 er ? " ERROR" : "", nz, first, fx, fy);
    }
    return 0;
#undef remap
}

static void drawWith(id cb, XeRState *st, NSUInteger prim, NSUInteger start, NSUInteger count, NSUInteger inst, NSUInteger baseInst,
                     id ib, NSUInteger ibOff, NSUInteger ibType, NSInteger baseVertex) {
    XeRPipe *p = rpipe(st->pipe);
    bool draw = count && inst;
    if (draw && !p) { os_log_error(gLog, "IrisXeMTL: draw without a render pipeline"); return; }
    if (!draw && !st->clearPending && !st->clearDepth && !st->clearStencil) return;
    if (!st->width || !st->height) { os_log_error(gLog, "IrisXeMTL: render pass without a color attachment"); return; }
    static const uint32_t topo[] = { 1, 2, 3, 4, 5 };            // MTLPrimitiveType Point, Line, LineStrip, Triangle, TriangleStrip
    if (prim > 4) { os_log_error(gLog, "IrisXeMTL: primitive type %lu", (unsigned long)prim); return; }
    static XeDrawCmd k0;
    XeDrawCmd *k = &k0;
    memset(k, 0, sizeof(*k));
    struct XeDrawDesc *d = &k->d;
    d->version = XE_DRAW_VERSION; d->size = sizeof(*d);
    id res[64]; int nres = 0;
    // render targets + clear
    for (unsigned i = 0; i < XE_DRAW_MAX_RT; i++) {
        if (!st->rt[i]) continue;
        id tb = nil;
        texDesc(st->rt[i], (XeTexDescU *)&d->rt[i], &tb);
        d->rt[i].pad |= st->rtLevel[i] << 8;
        if (tb) res[nres++] = tb;
        d->nrt = i + 1;
        memcpy(d->clear[i], st->clear[i], 16);
    }
    d->clearMask = st->clearPending;
    d->width = st->width; d->height = st->height;
    {   // v0.34.0: depth / stencil buffers, clears
        struct XeDrawDS *z = &d->ds;
        XeTex *dx = tex(st->depthTex), *sx = tex(st->stencilTex);
        if (dx) {
            z->depthVA = bufVA(dx->buf, 0); z->depthPitch = (uint32_t)dx->pitch; z->depthFormat = (uint32_t)dx->format;
            z->depthRows = (uint32_t)dx->dRows; z->depthSize = (uint32_t)(dx->pitch * dx->dRows);
            z->width = (uint32_t)dx->width; z->height = (uint32_t)dx->height;
            if (nres < 62) res[nres++] = dx->buf;
            if (st->clearDepth) { z->clearDepth = 1; z->depthClearValue = fbits(st->depthClear); }
        }
        if (sx) {
            z->stencilVA = bufVA(sx->buf, sx->sOff); z->stencilPitch = (uint32_t)sx->sPitch; z->stencilRows = (uint32_t)sx->sRows;
            z->stencilSize = (uint32_t)(sx->sPitch * sx->sRows);
            if (!dx) { z->width = (uint32_t)sx->width; z->height = (uint32_t)sx->height; }
            else if (sx != dx) os_log_error(gLog, "IrisXeMTL: separate depth and stencil textures: the depth size is used");
            if (nres < 62 && sx != dx) res[nres++] = sx->buf;
            if (st->clearStencil) { z->clearStencil = 1; z->stencilClearValue = st->stencilClear; }
        }
        if (draw) {
            XeDSS *s = dssOf(st->dss);
            z->depthFunc = s ? s->depthFunc : 7; z->depthWrite = s ? s->depthWrite : 0;   // default: Always, no writes
            if (s) { z->front = s->front; z->back = s->back; } else { stencilFace(nil, &z->front); stencilFace(nil, &z->back); }
            z->frontRef = st->frontRef; z->backRef = st->backRef;
            z->biasConstant = fbits(st->bias[0]); z->biasSlope = fbits(st->bias[1]); z->biasClamp = fbits(st->bias[2]);
            z->depthClamp = st->clipMode == MTLDepthClipModeClamp;
            z->psDepth = (p->fi.flags & XE_FS_DEPTH) ? 1 : 0;
            if (!dx && z->psDepth) z->psDepth = 0;
            z->psSrcDepth = ((p->fi.flags & XE_FS_SRCDEPTH) ? 1 : 0) | ((p->fi.flags & XE_FS_SRCW) ? 2 : 0);   // v0.35.0; v0.51.23: bit 1 = source W
        }
    }
    if (draw) {
        d->vsVA = bufVA(p->vsBuf, 0); d->psVA = bufVA(p->psBuf, 0);
        d->vsDW = p->vsDW; d->psDW = p->psDW;
        res[nres++] = p->vsBuf; res[nres++] = p->psBuf;
        d->vsGrf = p->vi.grfStart; d->vsReadLen = p->vi.urbReadLength; d->vsPush = p->vi.pushRegs;
        d->psGrf = p->fi.grfStart; d->psPush = p->fi.pushRegs;
        d->numAttr = p->fi.numVaryings; d->sbeReadLen = p->sbeReadLen; d->constInterp = p->constInterp;
        d->baryMode = p->baryMode; d->nonPersp = (p->fi.flags & XE_FS_LINEAR) ? 1 : 0; d->kill = ((p->fi.flags & XE_FS_DISCARD) ? 1 : 0) | (!(p->fi.flags & XE_FS_SIDEFX) && !getenv("XEC_NOVMASK") ? 2 : 0);   /* v0.62.1: bit 1 = helper pixels (Vector Mask) */
        memcpy(d->swiz, p->swiz, sizeof(d->swiz));
        for (unsigned i = 0; i < p->nvb; i++) {
            unsigned bi = p->vbIndex[i];
            id b = st->vbuf[bi];
            if (!b) { os_log_error(gLog, "IrisXeMTL: draw: vertex buffer %u is not set", bi); return; }
            d->vb[i].va = bufVA(b, st->voff[bi]);
            NSUInteger l = bufLen(b);
            d->vb[i].size = (uint32_t)(l > st->voff[bi] ? l - st->voff[bi] : 0);
            d->vb[i].pitch = p->vbStep[i] == MTLVertexStepFunctionConstant ? 0 : p->vbStride[i];
            res[nres++] = b;
        }
        d->nvb = p->nvb;
        if (p->vsScratch) { d->vsScratchVA = bufVA(p->vsScratch, 0); d->vsScratchLen = (uint32_t)bufLen(p->vsScratch); if (nres < 62) res[nres++] = p->vsScratch; }
        if (p->psScratch) { d->psScratchVA = bufVA(p->psScratch, 0); d->psScratchLen = (uint32_t)bufLen(p->psScratch); if (nres < 62) res[nres++] = p->psScratch; }
        d->nve = p->nve;
        memcpy(d->ve, p->ve, sizeof(d->ve));
        d->sysValues = p->vi.sysValues; d->sysSlot = p->vi.sysSlot;
        for (unsigned i = 0; i < 16; i++) {                        // buffers [[buffer(i)]] for the shaders
            // v0.68.26: slot i may hold a buffer 16..30 (bufSrc, set by the compiler)
            unsigned vi = p->vi.bufSrc[i] ? p->vi.bufSrc[i] - 1u : i, fi = p->fi.bufSrc[i] ? p->fi.bufSrc[i] - 1u : i;
            if (vi < 31 && st->vbuf[vi]) { d->vsBuf[i] = (uint32_t)bufVA(st->vbuf[vi], st->voff[vi]); if (nres < 60) res[nres++] = st->vbuf[vi];
                               NSUInteger l = bufLen(st->vbuf[vi]); d->vsLen[i] = (uint32_t)(l > st->voff[vi] ? l - st->voff[vi] : 0); }
            if (fi < 31 && st->fbuf[fi]) { d->psBuf[i] = (uint32_t)bufVA(st->fbuf[fi], st->foff[fi]); if (nres < 60) res[nres++] = st->fbuf[fi];
                               NSUInteger l = bufLen(st->fbuf[fi]); d->psLen[i] = (uint32_t)(l > st->foff[fi] ? l - st->foff[fi] : 0); }
        }
        for (unsigned i = 0; i < XE_DRAW_MAX_FTEX; i++) {
            id tb = nil;
            texDesc(st->ftex[i], (XeTexDescU *)&d->tex[i], &tb);
            if (st->ftexBase[i]) d->tex[i].pad |= ((uint32_t)(st->ftexBase[i] - 1) << 8) | (1u << 17);
            if (tb) { if (nres < 62) res[nres++] = tb; d->ntex = i + 1; }
        }
        for (unsigned i = 0; i < XE_DRAW_MAX_SAMP; i++) {
            XeSamp *x = samp(st->fsamp[i]);
            if (x) { memcpy(&d->samp[i], &x->d, sizeof(d->samp[i])); d->nsamp = i + 1; }
        }
        for (unsigned i = 0; i < XE_DRAW_MAX_TEX; i++) {          // v0.47.0: vertex textures
            id tb = nil;
            texDesc(st->vtex[i], (XeTexDescU *)&d->vtex[i], &tb);
            if (tb) { if (nres < 62) res[nres++] = tb; d->nvtex = i + 1; }
        }
        for (unsigned i = 0; i < XE_DRAW_MAX_SAMP; i++) {
            XeSamp *x = samp(st->vsamp[i]);
            if (x) { memcpy(&d->vsamp[i], &x->d, sizeof(d->vsamp[i])); d->nvsamp = i + 1; }
        }
        for (unsigned c = 0; c < p->vi.numConstSamplers && c < 4; c++) {
            XeSampDescU s = constSampler(p->vi.constSampler[c]);
            memcpy(&d->vsamp[7 - c], &s, sizeof(s));
            d->nvsamp = XE_DRAW_MAX_SAMP;
        }
        if (p->vi.usesTextures && !d->nvsamp) d->nvsamp = 1;       // a texture read() still needs a sampler pointer
        for (unsigned c = 0; c < p->fi.numConstSamplers && c < 4; c++) {   // constexpr samplers: slots 7, 6, ..
            XeSampDescU s = constSampler(p->fi.constSampler[c]);
            memcpy(&d->samp[7 - c], &s, sizeof(s));
            d->nsamp = XE_DRAW_MAX_SAMP;
        }
        // viewport: Metal (y down window, depth znear..zfar) -> SF_CLIP_VIEWPORT
        MTLViewport v = st->haveVP ? st->vp : (MTLViewport){ 0, 0, st->width, st->height, 0, 1 };
        float m00 = (float)(v.width / 2), m11 = (float)(-v.height / 2), m22 = (float)(v.zfar - v.znear);
        float m30 = (float)(v.originX + v.width / 2), m31 = (float)(v.originY + v.height / 2), m32 = (float)v.znear;
        // guardband: Mesa intel_calculate_guardband_size (Gfx7+: 16K), centred on the render area
        float g[4] = { 0, 0, 0, 0 };
        if (m00 != 0 && m11 != 0) {
            float rx0 = fminf(0, fminf(m30 + m00, m30 - m00)), rx1 = fmaxf(st->width - 1, fmaxf(m30 + m00, m30 - m00));
            float ry0 = fminf(0, fminf(m31 + m11, m31 - m11)), ry1 = fmaxf(st->height - 1, fmaxf(m31 + m11, m31 - m11));
            float gx0 = ((rx0 + rx1) / 2 - 16384.0f - m30) / m00, gx1 = ((rx0 + rx1) / 2 + 16384.0f - m30) / m00;
            float gy0 = ((ry0 + ry1) / 2 - 16384.0f - m31) / m11, gy1 = ((ry0 + ry1) / 2 + 16384.0f - m31) / m11;
            g[0] = fminf(gx0, gx1); g[1] = fmaxf(gx0, gx1); g[2] = fminf(gy0, gy1); g[3] = fmaxf(gy0, gy1);
        }
        float xmin = (float)fmax(v.originX, 0), ymin = (float)fmax(v.originY, 0);
        float xmax = (float)fmin(v.originX + v.width, st->width) - 1, ymax = (float)fmin(v.originY + v.height, st->height) - 1;
        uint32_t vp[14] = { fbits(m00), fbits(m11), fbits(m22), fbits(m30), fbits(m31), fbits(m32),
                            fbits(g[0]), fbits(g[1]), fbits(g[2]), fbits(g[3]),
                            fbits(xmin), fbits(xmax), fbits(ymin), fbits(ymax) };
        memcpy(d->vp, vp, sizeof(vp));
        d->cull = st->cull == MTLCullModeFront ? 2 : st->cull == MTLCullModeBack ? 3 : 1;
        // both mean the order as seen on screen (anv maps Vulkan's front face the same way)
        d->frontCCW = st->winding == MTLWindingCounterClockwise ? 1 : 0;
        d->fillLines = st->fill == MTLTriangleFillModeLines;
        d->topology = topo[prim];
        d->vertexStart = (uint32_t)start; d->vertexCount = (uint32_t)count;
        d->instanceCount = (uint32_t)inst; d->baseInstance = (uint32_t)baseInst;
        // v0.33.0: blending, blend constant, scissor, index buffer
        memcpy(d->blend, p->blend, sizeof(d->blend));
        for (int c = 0; c < 4; c++) d->blendColor[c] = fbits(st->blendColor[c]);
        if (st->haveSC) {
            NSUInteger x0 = st->sc.x, y0 = st->sc.y, x1 = st->sc.x + st->sc.width, y1 = st->sc.y + st->sc.height;
            d->scissor[0] = (uint32_t)MIN(x0, st->width); d->scissor[1] = (uint32_t)MIN(y0, st->height);
            d->scissor[2] = (uint32_t)MIN(x1, st->width); d->scissor[3] = (uint32_t)MIN(y1, st->height);
        } else { d->scissor[2] = st->width; d->scissor[3] = st->height; }
        if (ib) {
            NSUInteger isz = ibType == MTLIndexTypeUInt32 ? 4 : 2, l = bufLen(ib);
            if (ibOff % isz || ibOff + count * isz > l) {
                os_log_error(gLog, "IrisXeMTL: drawIndexedPrimitives: index buffer offset %lu / count %lu outside the buffer (%lu bytes)",
                             (unsigned long)ibOff, (unsigned long)count, (unsigned long)l);
                return;
            }
            d->ibVA = bufVA(ib, ibOff); d->ibSize = (uint32_t)(count * isz);
            d->ibType = isz == 2 ? 1 : 2;
            d->baseVertex = (int32_t)baseVertex;
            d->vertexStart = 0;
            if (nres < 63) res[nres++] = ib;
        }
    }
    // (v0.37.0: the command buffer comes from the caller: a render encoder, or generateMipmaps in a blit encoder)
    for (unsigned i = 0; i < XE_DRAW_MAX_RT; i++) if (st->rt[i]) markIOS(cb, st->rt[i]);   // v0.38.0: IOSurface targets
    // v0.42.7: IRISXE_LOG_PIPE=<part of a function name>: log the first draws of that pipeline with their data
    static const char *logPipe = (const char *)-1;
    if (logPipe == (const char *)-1) logPipe = getenv("IRISXE_LOG_PIPE");
    static int logged;
    if (logPipe && p && strstr(p->name, logPipe) && logged < 4) {
        logged++;
        os_log(gLog, "IrisXeMTL: LOGPIPE %{public}s: %u vertices x %u, index type %u, %u VBs, clear 0x%x, rt %ux%u, "
               "depth 0x%llx stencil 0x%llx", p->name, d->vertexCount, d->instanceCount, d->ibType, d->nvb, d->clearMask,
               d->rt[0].width, d->rt[0].height, (unsigned long long)d->ds.depthVA, (unsigned long long)d->ds.stencilVA);
        void (^dumpBuf)(const char *, id, NSUInteger, NSUInteger) = ^(const char *what, id b, NSUInteger off, NSUInteger n) {
            if (!b) return;
            const uint8_t *c = (const uint8_t *)((void *(*)(id, SEL))objc_msgSend)(b, sel_registerName("contents"));
            NSUInteger l = bufLen(b);
            if (!c || off >= l) return;
            if (n > l - off) n = l - off;
            if (n > 128) n = 128;
            NSMutableString *m = [NSMutableString string];
            for (NSUInteger i = 0; i + 4 <= n; i += 4) { float f; memcpy(&f, c + off + i, 4); [m appendFormat:@" %g", f]; }
            os_log(gLog, "IrisXeMTL: LOGPIPE   %{public}s (%lu bytes at +%lu, as floats):%{public}@", what, (unsigned long)l,
                   (unsigned long)off, m);
        };
        for (unsigned i = 0; i < 8; i++) {
            char w[32];
            snprintf(w, sizeof(w), "vertex buffer(%u)", i); dumpBuf(w, st->vbuf[i], st->voff[i], 128);
            snprintf(w, sizeof(w), "fragment buffer(%u)", i); dumpBuf(w, st->fbuf[i], st->foff[i], 128);
        }
        if (ib) {
            const uint8_t *c = (const uint8_t *)((void *(*)(id, SEL))objc_msgSend)(ib, sel_registerName("contents"));
            NSMutableString *m = [NSMutableString string];
            for (NSUInteger i = 0; c && i < count && i < 24; i++)
                [m appendFormat:@" %u", ibType ? ((const uint32_t *)(c + ibOff))[i] : ((const uint16_t *)(c + ibOff))[i]];
            os_log(gLog, "IrisXeMTL: LOGPIPE   indices:%{public}@ (base vertex %ld)", m, (long)baseVertex);
        }
    }
    if (recOn()) recordCmd(p ? p->name : "(clear only)", k, sizeof(*k), res, nres);   // v0.44.0
    // v0.49.0: batching: the encoder's draws go into one segment (up to 16, then a new one); IrisXeAccel runs
    // the draws of a segment as one GPU job. IRISXE_NOBATCH=1: one segment per draw, as before.
    // v0.51.37: one GPU job per draw is the default: batched draws hung the GPU on the desktop (the state change between
    // two draws of one job; 49 hangs in 5 minutes, 0 with one job per draw). IRISXE_BATCH=1 or "batch" in the
    // desktop test turns batching back on (for testing the kext fix)
    static int noBatch = -1;
    if (noBatch < 0) noBatch = !(getenv("IRISXE_BATCH") != NULL || gDesktopBatch);
    const unsigned long csize = (sizeof(XeDrawCmd) + 63) & ~63ul;      // commands 64-byte aligned, back to back
    bool fresh = noBatch || st->segCB != cb || st->segDraws >= 16;
    if (fresh && st->segCB) { endKernelCommand(st->segCB); st->segCB = nil; }
    XeDrawCmd *out = kernelCommandIn(cb, res, nres, csize, fresh);
    if (out) { memcpy(out, k, sizeof(*k)); out->type = XE_KCMD_DRAW; out->size = (uint32_t)csize; }
    if (noBatch) endKernelCommand(cb);
    else { st->segCB = cb; st->segDraws = fresh ? 1 : st->segDraws + 1; }
    st->clearPending = 0;
    st->clearDepth = st->clearStencil = false;
    os_log_debug(gLog, "IrisXeMTL: draw %lu vertices x %lu (clear 0x%x) -> command %p", (unsigned long)count, (unsigned long)inst,
           d->clearMask, out);
}
// ---- v0.37.0: generateMipmapsForTexture: level k drawn from level k - 1 (linear filter = a 2x2 box) ----
// The shaders are our own (src/metal/IrisXeBuiltins.metal, compiled with the bundle into
// Contents/Resources/irisxe-builtins.metallib), compiled by our compiler like any app's.
static id gBuiltinLib, gMipSampler;
static NSMutableDictionary *gMipPipes;                      // pixel format -> render pipeline
static id builtinLibrary(id dev) {
    if (gBuiltinLib) return gBuiltinLib;
    Dl_info di;
    if (!dladdr((const void *)builtinLibrary, &di) || !di.dli_fname) return nil;
    NSString *exe = [NSString stringWithUTF8String:di.dli_fname];   // .../IrisXeMTLDriver.bundle/Contents/MacOS/IrisXeMTLDriver
    NSString *path = [[[exe stringByDeletingLastPathComponent] stringByDeletingLastPathComponent]
                      stringByAppendingPathComponent:@"Resources/irisxe-builtins.metallib"];
    NSError *e = nil;
    gBuiltinLib = ((id (*)(id, SEL, id, NSError **))objc_msgSend)(dev, sel_registerName("newLibraryWithURL:error:"),
                                                                  [NSURL fileURLWithPath:path], &e);
    if (!gBuiltinLib) os_log_error(gLog, "IrisXeMTL: %{public}@: %{public}@", path, e.localizedDescription);
    return gBuiltinLib;
}
static void genMips(id enc, id t) {
    XeTex *x = t && object_getClass(t) == gTexClass ? tex(t) : NULL;
    if (!x) { os_log_error(gLog, "IrisXeMTL: generateMipmapsForTexture: not our texture"); return; }
    if (x->levels < 2) return;
    if (x->type != 2 || x->isDepth || x->isStencil || isIntPF(x->format)) {
        os_log_error(gLog, "IrisXeMTL: generateMipmapsForTexture: only 2D color textures with a filterable format yet");
        return;
    }
    id dev = x->dev;
    @synchronized ([NSObject class]) {
        id lib = builtinLibrary(dev);
        if (!lib) return;
        if (!gMipPipes) gMipPipes = [NSMutableDictionary new];
        id pipe = gMipPipes[@(x->format)];
        if (!pipe) {
            MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor new] autorelease];
            pd.vertexFunction = [[lib newFunctionWithName:@"irisxe_fullscreen_vs"] autorelease];
            pd.fragmentFunction = [[lib newFunctionWithName:@"irisxe_copy_fs"] autorelease];
            pd.colorAttachments[0].pixelFormat = (MTLPixelFormat)x->format;
            NSError *e = nil;
            pipe = newRenderPipeline(dev, pd, &e);
            if (!pipe) { os_log_error(gLog, "IrisXeMTL: generateMipmaps pipeline: %{public}@", e.localizedDescription); return; }
            gMipPipes[@(x->format)] = pipe;
            [pipe release];
        }
        if (!gMipSampler) {
            MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor new] autorelease];
            sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
            sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
            gMipSampler = ((id (*)(id, SEL, id))objc_msgSend)(dev, sel_registerName("newSamplerStateWithDescriptor:"), sd);
        }
        id cb = ((id (*)(id, SEL))objc_msgSend)(enc, sel_registerName("commandBuffer"));
        for (NSUInteger k = 1; k < x->levels; k++) {
            XeRState st;
            memset(&st, 0, sizeof(st));
            st.pipe = pipe;
            st.rt[0] = t; st.rtLevel[0] = (uint32_t)k;
            st.width = (uint32_t)x->lvlW[k]; st.height = (uint32_t)x->lvlH[k];
            st.ftex[0] = t; st.ftexBase[0] = (uint8_t)k;             // level k - 1 alone (+1)
            st.fsamp[0] = gMipSampler;
            drawWith(cb, &st, 3, 0, 3, 1, 0, nil, 0, 0, 0);
            if (st.segCB) { endKernelCommand(st.segCB); st.segCB = nil; }   // v0.57.9: batching left it open
        }
    }
}

// ---- v0.57.9: copies with Y-tiled textures, drawn (the blitter copies rows of linear memory) ----
// A full-target triangle, the scissor = the destination rectangle, and irisxe_copyrect_fs reading the source texel
// at (pixel + offset). One pipeline per pixel format, built on first use (gCopyReady; tiling is off for a format
// whose pipeline doesn't build).
static NSMutableDictionary *gCopyPipes;                     // pixel format -> pipeline, or NSNull = can't
static id copyPipe(id dev, NSUInteger pf) {
    @synchronized ([NSObject class]) {
        if (!gCopyPipes) gCopyPipes = [NSMutableDictionary new];
        id pipe = gCopyPipes[@(pf)];
        if (pipe) return pipe == [NSNull null] ? nil : pipe;
        id lib = builtinLibrary(dev);
        id fs = lib ? [[lib newFunctionWithName:@"irisxe_copyrect_fs"] autorelease] : nil;
        if (fs) {
            MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor new] autorelease];
            pd.vertexFunction = [[lib newFunctionWithName:@"irisxe_fullscreen_vs"] autorelease];
            pd.fragmentFunction = fs;
            pd.colorAttachments[0].pixelFormat = (MTLPixelFormat)pf;
            NSError *e = nil;
            pipe = newRenderPipeline(dev, pd, &e);
            if (!pipe) os_log_error(gLog, "IrisXeMTL: tiled copy pipeline (format %lu): %{public}@ - format stays linear",
                                    (unsigned long)pf, e.localizedDescription);
        } else os_log_error(gLog, "IrisXeMTL: irisxe_copyrect_fs not in the builtins: tiling off");
        gCopyPipes[@(pf)] = pipe ? pipe : [NSNull null];
        if (pipe) [pipe release];
        return pipe;
    }
}
static bool copyReady(id dev, NSUInteger pf) { return copyPipe(dev, pf) != nil; }
static bool copyDraw(id enc, id src, NSUInteger sLevel, MTLOrigin so, MTLSize sz, id dst, NSUInteger dLevel, MTLOrigin dorig) {
    XeTex *a = src && object_getClass(src) == gTexClass ? tex(src) : NULL, *b = dst && object_getClass(dst) == gTexClass ? tex(dst) : NULL;
    if (!a || !b || b->type != 2 || a->type != 2 || b->isDepth || b->isStencil || a->isDepth || a->isStencil) return false;
    if (sLevel >= a->levels || dLevel >= b->levels) return false;
    id pipe = copyPipe(b->dev, b->format);
    if (!pipe) return false;
    id cb = ((id (*)(id, SEL))objc_msgSend)(enc, sel_registerName("commandBuffer"));
    id pb = ((id (*)(id, SEL, NSUInteger, NSUInteger))objc_msgSend)(b->dev, sel_registerName("newBufferWithLength:options:"),
                                                                   64, MTLResourceStorageModeShared);
    if (!pb) return false;
    int32_t *o = (int32_t *)((void *(*)(id, SEL))objc_msgSend)(pb, sel_registerName("contents"));
    o[0] = (int32_t)so.x - (int32_t)dorig.x; o[1] = (int32_t)so.y - (int32_t)dorig.y; o[2] = o[3] = 0;
    ((void (*)(id, SEL, id))objc_msgSend)(cb, sel_registerName("addCompletedHandler:"), ^(id x) { [pb release]; });
    XeRState st;
    memset(&st, 0, sizeof(st));
    st.pipe = pipe;
    st.rt[0] = dst; st.rtLevel[0] = (uint32_t)dLevel;
    st.width = (uint32_t)b->lvlW[dLevel]; st.height = (uint32_t)b->lvlH[dLevel];
    st.sc = (MTLScissorRect){ dorig.x, dorig.y, sz.width, sz.height }; st.haveSC = true;
    st.ftex[0] = src; st.ftexBase[0] = (uint8_t)(sLevel + 1);   // that level alone
    st.fbuf[0] = pb;
    st.cull = 0;
    drawWith(cb, &st, 3, 0, 3, 1, 0, nil, 0, 0, 0);
    if (st.segCB) { endKernelCommand(st.segCB); st.segCB = nil; }
    return true;
}

static void drawCommon(id self, NSUInteger prim, NSUInteger start, NSUInteger count, NSUInteger inst, NSUInteger baseInst,
                       id ib, NSUInteger ibOff, NSUInteger ibType, NSInteger baseVertex) {
    drawWith(((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("commandBuffer")), rstate(self), prim, start, count, inst,
             baseInst, ib, ibOff, ibType, baseVertex);
}
static void xe_rDraw(id self, SEL _cmd, NSUInteger prim, NSUInteger start, NSUInteger count) { drawCommon(self, prim, start, count, 1, 0, nil, 0, 0, 0); }
static void xe_rDrawInst(id self, SEL _cmd, NSUInteger prim, NSUInteger start, NSUInteger count, NSUInteger inst) {
    drawCommon(self, prim, start, count, inst, 0, nil, 0, 0, 0);
}
static void xe_rDrawInstBase(id self, SEL _cmd, NSUInteger prim, NSUInteger start, NSUInteger count, NSUInteger inst, NSUInteger base) {
    drawCommon(self, prim, start, count, inst, base, nil, 0, 0, 0);
}
static bool ibOK(id ib) {
    if (ib && object_getClass(ib) != gBufClass) { os_log_error(gLog, "IrisXeMTL: drawIndexedPrimitives: not our buffer"); return false; }
    if (!ib) { os_log_error(gLog, "IrisXeMTL: drawIndexedPrimitives without an index buffer"); return false; }
    return true;
}
static void xe_rDrawIdx(id self, SEL _cmd, NSUInteger prim, NSUInteger count, NSUInteger type, id ib, NSUInteger off) {
    if (ibOK(ib)) drawCommon(self, prim, 0, count, 1, 0, ib, off, type, 0);
}
static void xe_rDrawIdxInst(id self, SEL _cmd, NSUInteger prim, NSUInteger count, NSUInteger type, id ib, NSUInteger off, NSUInteger inst) {
    if (ibOK(ib)) drawCommon(self, prim, 0, count, inst, 0, ib, off, type, 0);
}
static void xe_rDrawIdxBase(id self, SEL _cmd, NSUInteger prim, NSUInteger count, NSUInteger type, id ib, NSUInteger off, NSUInteger inst,
                            NSInteger baseVertex, NSUInteger baseInst) {
    if (ibOK(ib)) drawCommon(self, prim, 0, count, inst, baseInst, ib, off, type, baseVertex);
}
static void xe_rSetColorStore(id self, SEL _cmd, NSUInteger store, NSUInteger idx) {   // v0.66.0
    XeRState *st = rstate(self);
    if (idx >= XE_DRAW_MAX_RT) return;
    if (store == 2 || store == 3) st->resolveOn |= 1u << idx; else st->resolveOn &= ~(1u << idx);
}
static void xe_rStoreNop(id self, SEL _cmd, NSUInteger store) { }
static void xe_rEndEncoding(id self, SEL _cmd) {
    drawCommon(self, 3, 0, 0, 0, 0, nil, 0, 0, 0);             // a clear without draws
    XeRState *st = rstate(self);                               // v0.49.0: close the draws' segment
    if (st->segCB) { endKernelCommand(st->segCB); st->segCB = nil; st->segDraws = 0; }
    // v0.66.0: multisample resolve = copy the (one-sample) color attachment into the resolve texture
    for (unsigned k = 0; k < XE_DRAW_MAX_RT; k++) {
        if (!(st->resolveOn & (1u << k)) || !st->rt[k] || !st->resolve[k] || st->resolve[k] == st->rt[k]) continue;
        XeTex *a = tex(st->rt[k]), *b = tex(st->resolve[k]);
        uint32_t sl = st->rtLevel[k], dl = st->resolveLevel[k];
        if (dl >= b->levels) continue;
        NSUInteger w = MIN(a->lvlW[sl], b->lvlW[dl]), h = MIN(a->lvlH[sl], b->lvlH[dl]);
        static int warned;
        if (!copyDraw(self, st->rt[k], sl, MTLOriginMake(0, 0, 0), MTLSizeMake(w, h, 1), st->resolve[k], dl, MTLOriginMake(0, 0, 0)) && !warned) {
            warned = 1;
            os_log_error(gLog, "IrisXeMTL: multisample resolve: could not copy format %lu -> %lu", (unsigned long)a->format, (unsigned long)b->format);
        }
    }
    struct objc_super sup = { self, class_getSuperclass(gRenderClass) };
    ((void (*)(struct objc_super *, SEL))objc_msgSendSuper)(&sup, _cmd);
}

// missing methods of the pipeline / encoder: log once, return 0
static const char *renderProtoTypes(SEL sel) {
    const char *names[] = { "MTLRenderCommandEncoder", "MTLCommandEncoder", "MTLRenderPipelineState", "MTLDepthStencilState" };
    for (int i = 0; i < 4; i++) {
        Protocol *p = objc_getProtocol(names[i]);
        if (!p) continue;
        for (int req = 1; req >= 0; req--) {
            struct objc_method_description d = protocol_getMethodDescription(p, sel, req, YES);
            if (d.types) return d.types;
        }
    }
    return NULL;
}
static NSMethodSignature *xe_renderSig(id self, SEL _cmd, SEL sel) {
    struct objc_super sup = { self, class_getSuperclass(object_getClass(self)) };
    NSMethodSignature *sig = ((id (*)(struct objc_super *, SEL, SEL))objc_msgSendSuper)(&sup, _cmd, sel);
    if (sig) return sig;
    const char *t = renderProtoTypes(sel);
    return [NSMethodSignature signatureWithObjCTypes:t ? t : "@@:"];
}
static void addR(Class c, const char *sel, IMP imp, const char *fallback) {
    const char *t = renderProtoTypes(sel_registerName(sel));
    if (!class_addMethod(c, sel_registerName(sel), imp, t ? t : fallback))
        os_log_error(gLog, "IrisXeMTL: could not add %{public}s", sel);
}
static void registerRenderClasses(Class dev, Class encBase, Class cmdBuf) {
    gGenMips = genMips;                                         // v0.37.0
    gCopyReady = copyReady; gCopyDraw = copyDraw;               // v0.57.9
    gRPipeClass = objc_allocateClassPair([NSObject class], "IrisXeMTLRenderPipelineState", 0);
    if (gRPipeClass) {
        class_addIvar(gRPipeClass, "_xe", sizeof(void *), 3, "^v");
        addR(gRPipeClass, "dealloc", (IMP)xe_rpipeDealloc, "v16@0:8");
        addR(gRPipeClass, "device", (IMP)xe_rpipeDevice, "@16@0:8");
        addR(gRPipeClass, "label", (IMP)xe_rpipeLabel, "@16@0:8");
        addR(gRPipeClass, "methodSignatureForSelector:", (IMP)xe_renderSig, "@24@0:8:16");
        addR(gRPipeClass, "forwardInvocation:", (IMP)xe_texForward, "v24@0:8@16");
        class_addProtocol(gRPipeClass, @protocol(MTLRenderPipelineState));
        objc_registerClassPair(gRPipeClass);
        gRPipeIvar = class_getInstanceVariable(gRPipeClass, "_xe");
    }
    gRenderClass = encBase ? objc_allocateClassPair(encBase, "IrisXeMTLRenderCommandEncoder", 0) : Nil;
    if (gRenderClass) {
        Class r = gRenderClass;
        class_addIvar(r, "_xeRender", sizeof(void *), 3, "^v");
        addR(r, "dealloc", (IMP)xe_renderDealloc, "v16@0:8");
        addR(r, "setRenderPipelineState:", (IMP)xe_rSetPipeline, "v24@0:8@16");
        addR(r, "setVertexBuffer:offset:atIndex:", (IMP)xe_rSetVB, "v40@0:8@16Q24Q32");
        addR(r, "setFragmentBuffer:offset:atIndex:", (IMP)xe_rSetFB, "v40@0:8@16Q24Q32");
        addR(r, "setVertexBufferOffset:atIndex:", (IMP)xe_rSetVBOff, "v32@0:8Q16Q24");
        addR(r, "setFragmentBufferOffset:atIndex:", (IMP)xe_rSetFBOff, "v32@0:8Q16Q24");
        addR(r, "setVertexBuffers:offsets:withRange:", (IMP)xe_rSetVBs, "v48@0:8^@16^Q24{_NSRange=QQ}32");
        addR(r, "setFragmentBuffers:offsets:withRange:", (IMP)xe_rSetFBs, "v48@0:8^@16^Q24{_NSRange=QQ}32");
        addR(r, "setVertexBytes:length:atIndex:", (IMP)xe_rSetVBytes, "v40@0:8r^v16Q24Q32");
        addR(r, "setFragmentBytes:length:atIndex:", (IMP)xe_rSetFBytes, "v40@0:8r^v16Q24Q32");
        addR(r, "setFragmentTexture:atIndex:", (IMP)xe_rSetFTex, "v32@0:8@16Q24");
        addR(r, "setFragmentTextures:withRange:", (IMP)xe_rSetFTexs, "v40@0:8^@16{_NSRange=QQ}24");
        addR(r, "setFragmentSamplerState:atIndex:", (IMP)xe_rSetFSamp, "v32@0:8@16Q24");
        addR(r, "setFragmentSamplerStates:withRange:", (IMP)xe_rSetFSamps, "v40@0:8^@16{_NSRange=QQ}24");
        addR(r, "setFragmentSamplerState:lodMinClamp:lodMaxClamp:atIndex:", (IMP)xe_rSetFSampClamp, "v40@0:8@16f24f28Q32");
        addR(r, "setVertexTexture:atIndex:", (IMP)xe_rSetVTex, "v32@0:8@16Q24");             // v0.47.0
        addR(r, "setVertexTextures:withRange:", (IMP)xe_rSetVTexs, "v40@0:8^@16{_NSRange=QQ}24");
        addR(r, "setVertexSamplerState:atIndex:", (IMP)xe_rSetVSamp, "v32@0:8@16Q24");
        addR(r, "setVertexSamplerStates:withRange:", (IMP)xe_rSetVSamps, "v40@0:8^@16{_NSRange=QQ}24");
        addR(r, "setVertexSamplerState:lodMinClamp:lodMaxClamp:atIndex:", (IMP)xe_rSetVSampClamp, "v40@0:8@16f24f28Q32");
        addR(r, "setViewport:", (IMP)xe_rSetViewport, "v64@0:8{?=dddddd}16");
        addR(r, "setViewports:count:", (IMP)xe_rSetViewports, "v32@0:8r^{?=dddddd}16Q24");
        addR(r, "setCullMode:", (IMP)xe_rSetCull, "v24@0:8Q16");
        addR(r, "setFrontFacingWinding:", (IMP)xe_rSetWinding, "v24@0:8Q16");
        addR(r, "setTriangleFillMode:", (IMP)xe_rSetFill, "v24@0:8Q16");
        addR(r, "setDepthClipMode:", (IMP)xe_rSetDepthClip, "v24@0:8Q16");
        addR(r, "setDepthStencilState:", (IMP)xe_rSetDSS, "v24@0:8@16");
        addR(r, "setStencilReferenceValue:", (IMP)xe_rSetStencilRef, "v20@0:8I16");
        addR(r, "setStencilFrontReferenceValue:backReferenceValue:", (IMP)xe_rSetStencilRefs, "v24@0:8I16I20");
        addR(r, "setDepthBias:slopeScale:clamp:", (IMP)xe_rSetDepthBias, "v28@0:8f16f20f24");
        addR(r, "drawPrimitives:vertexStart:vertexCount:", (IMP)xe_rDraw, "v40@0:8Q16Q24Q32");
        addR(r, "drawPrimitives:vertexStart:vertexCount:instanceCount:", (IMP)xe_rDrawInst, "v48@0:8Q16Q24Q32Q40");
        addR(r, "drawPrimitives:vertexStart:vertexCount:instanceCount:baseInstance:", (IMP)xe_rDrawInstBase, "v56@0:8Q16Q24Q32Q40Q48");
        addR(r, "setBlendColorRed:green:blue:alpha:", (IMP)xe_rSetBlendColor, "v32@0:8f16f20f24f28");
        addR(r, "setScissorRect:", (IMP)xe_rSetScissor, "v48@0:8{?=QQQQ}16");
        addR(r, "setScissorRects:count:", (IMP)xe_rSetScissors, "v32@0:8r^{?=QQQQ}16Q24");
        addR(r, "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:", (IMP)xe_rDrawIdx, "v56@0:8Q16Q24Q32@40Q48");
        addR(r, "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:", (IMP)xe_rDrawIdxInst,
             "v64@0:8Q16Q24Q32@40Q48Q56");
        addR(r, "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:baseVertex:baseInstance:",
             (IMP)xe_rDrawIdxBase, "v80@0:8Q16Q24Q32@40Q48Q56q64Q72");
        addR(r, "endEncoding", (IMP)xe_rEndEncoding, "v16@0:8");
        addR(r, "setColorStoreAction:atIndex:", (IMP)xe_rSetColorStore, "v32@0:8Q16Q24");              // v0.66.0
        addR(r, "setColorStoreActionOptions:atIndex:", (IMP)xe_rStoreNop, "v32@0:8Q16Q24");
        addR(r, "setDepthStoreAction:", (IMP)xe_rStoreNop, "v24@0:8Q16");
        addR(r, "setStencilStoreAction:", (IMP)xe_rStoreNop, "v24@0:8Q16");
        addFenceNops(r);                                                                  // v0.42.0
        addR(r, "setVertexAmplificationCount:viewMappings:", (IMP)xe_encAmplification, "v32@0:8Q16r^v24");
        addR(r, "methodSignatureForSelector:", (IMP)xe_renderSig, "@24@0:8:16");
        addR(r, "forwardInvocation:", (IMP)xe_texForward, "v24@0:8@16");
        class_addProtocol(r, @protocol(MTLRenderCommandEncoder));   // v0.51.15: Metal Performance Shaders checks conformsToProtocol
        objc_registerClassPair(r);
        gRenderIvar = class_getInstanceVariable(r, "_xeRender");
    } else os_log_error(gLog, "IrisXeMTL: no base class for the render encoder");
    if (cmdBuf && gRenderClass)
        class_addMethod(cmdBuf, sel_registerName("renderCommandEncoderWithDescriptor:"), (IMP)xe_renderCommandEncoder, "@24@0:8@16");
    if (dev && gRPipeClass) {
        add(dev, "newRenderPipelineStateWithDescriptor:error:", (IMP)xe_newRPS, "@32@0:8@16^@24");
        add(dev, "newRenderPipelineStateWithDescriptor:options:reflection:error:", (IMP)xe_newRPSOpts, "@48@0:8@16Q24^@32^@40");
        add(dev, "newRenderPipelineStateWithDescriptor:completionHandler:", (IMP)xe_newRPSHandler, "v32@0:8@16@?24");
        add(dev, "newRenderPipelineStateWithDescriptor:options:completionHandler:", (IMP)xe_newRPSOptsHandler, "v40@0:8@16Q24@?32");
        add(dev, "irisxeLastRenderLog", (IMP)xe_lastRenderLog, "@16@0:8");
    }
    gDSSClass = objc_allocateClassPair([NSObject class], "IrisXeMTLDepthStencilState", 0);   // v0.34.0
    if (gDSSClass) {
        class_addIvar(gDSSClass, "_xe", sizeof(void *), 3, "^v");
        addR(gDSSClass, "dealloc", (IMP)xe_dssDealloc, "v16@0:8");
        addR(gDSSClass, "device", (IMP)xe_dssDevice, "@16@0:8");
        addR(gDSSClass, "label", (IMP)xe_dssLabel, "@16@0:8");
        addR(gDSSClass, "methodSignatureForSelector:", (IMP)xe_renderSig, "@24@0:8:16");
        addR(gDSSClass, "forwardInvocation:", (IMP)xe_texForward, "v24@0:8@16");
        class_addProtocol(gDSSClass, @protocol(MTLDepthStencilState));
        objc_registerClassPair(gDSSClass);
        gDSSIvar = class_getInstanceVariable(gDSSClass, "_xe");
        if (dev) add(dev, "newDepthStencilStateWithDescriptor:", (IMP)xe_newDSS, "@24@0:8@16");
    }
}

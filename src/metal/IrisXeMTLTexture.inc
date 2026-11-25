// IrisXeMTLTexture.inc - milestone 4: textures and samplers for compute (v0.30.0).
// Included by IrisXeMTLDriver.m.
//
// A texture is one of our own Metal buffers holding the texels in a linear
// (untiled) layout: rows of `pitch` bytes (a multiple of 64), array layers
// `qpitch` rows apart. The kernel side maps it like any buffer; IrisXeCore
// writes a RENDER_SURFACE_STATE for it at the binding table entry of its
// texture slot. Shaders read with the sampler (ld / sample at level 0),
// write with typed surface writes. Only 2D and 2D array, one mip level, one
// sample, for now.

#define XE_MAX_TEX  8
#define XE_MAX_SAMP 8
typedef struct {                // = XeTexDesc (src/IrisXeSpaceABI.h)
    uint64_t va;
    uint32_t width, height, layers, pitch, qpitch, format, array, pad;
} XeTexDescU;
typedef struct {                // = XeSampDesc
    uint32_t used;
    uint8_t minFilter, magFilter, mipFilter, addrU, addrV, addrW, normalized, pad[5];
} XeSampDescU;
_Static_assert(sizeof(XeTexDescU) == 40, "XeTexDesc layout");
_Static_assert(sizeof(XeSampDescU) == 16, "XeSampDesc layout");

typedef struct {
    id dev, buf, label;
    NSUInteger width, height, layers, type, format, usage, storage, bpp, pitch, qpitch;
    uint32_t gen;                // Gen surface format
    // v0.34.0: depth / stencil textures (the GPU's depth and stencil units need tiled memory)
    bool isDepth, isStencil;     // has a depth plane (Y-tiled, at 0) / a stencil plane (Y-tiled too on Gen12, at sOff)
    NSUInteger dRows, sOff, sPitch, sRows, size;
    // v0.36.0: mipmaps (isl's GFX4_2D layout, HALIGN / VALIGN 4): level k at lvlOff[k] bytes in a layer
    NSUInteger levels, lvlOff[16], lvlW[16], lvlH[16];
    // v0.38.0: a texture on an IOSurface (CAMetalLayer drawables): the GPU draws into our own memory and the
    // picture is copied into the IOSurface when the command buffer completes (and copied in at creation)
    IOSurfaceRef ios; NSUInteger iosPlane;
    bool iosDirect;              // v0.50.0: the texture's memory IS the IOSurface's: the GPU draws into it, no copies
    // v0.51.1: a linear texture on a buffer ([MTLBuffer newTextureWithDescriptor:offset:bytesPerRow:]): buf is
    // that buffer and the texture starts bufOff bytes into it
    NSUInteger bufOff; bool onBuffer;
    NSUInteger apiType1;         // v0.51.5: 1D (array) textures are 2D (arrays) one pixel tall; Metal's type + 1
    NSUInteger samples;          // v0.66.0: the sample count asked for (multisample textures are drawn with 1 sample)
    bool iosWritten;             // v0.51.24: this process's GPU work writes the (copied) IOSurface texture
    // v0.51.31: a texture on a buffer at an offset the GPU core can't use (not 4 KB aligned: Core Animation's images and
    // text are 32 / 256 bytes into their buffers): it has its own memory, filled from the buffer before every GPU read
    id srcBuf; NSUInteger srcOff, srcBpr; bool srcWritten;
    uint32_t srcSerial;                  // v0.68.45: the render encoder it was last refreshed for (gEncSerial)
    bool tiledColor;             // v0.57.7: a Y-tiled colour texture (test: IRISXE_TILED=1; 2D, one level, own memory)
} XeTex;
// v0.57.7: textures made inside other paths (IOSurface, buffer textures) get their memory replaced: never tiled
static __thread bool gTexNoTile;
// MTLPixelFormat of a depth / stencil texture: depth bytes per texel (0 = none), stencil plane
static bool dsFormat(NSUInteger pf, NSUInteger *dBpp, bool *stencil) {
    switch (pf) {
    case 250: *dBpp = 2; *stencil = false; return true;     // Depth16Unorm
    case 252: *dBpp = 4; *stencil = false; return true;     // Depth32Float
    case 253: *dBpp = 0; *stencil = true;  return true;     // Stencil8
    case 260: *dBpp = 4; *stencil = true;  return true;     // Depth32Float_Stencil8
    default: return false;
    }
}
// byte offset of (x bytes, y) in a Y-tiled plane: 4 KB tiles of 128 B x 32 rows (isl tiley_swiz)
static NSUInteger tileYOffset(NSUInteger pitch, NSUInteger x, NSUInteger y) {
    NSUInteger t = (y / 32) * (pitch / 128) + x / 128;
    NSUInteger u = x & 127, v = y & 31;
    return t * 4096 + ((u >> 4) << 9) + (v << 4) + (u & 15);
}
static Class gTexClass, gSampClass;
static Ivar gTexIvar, gSampIvar;

// MTLPixelFormat -> Gen surface format (ISL_FORMAT_*), bytes per texel
static bool texFormat(NSUInteger pf, uint32_t *gen, NSUInteger *bpp) {
    static const struct { NSUInteger pf; uint32_t gen; NSUInteger bpp; } t[] = {
        // v0.36.0: more formats (ISL_FORMAT_* numbers from isl.h)
        { 1, 324, 1 },     // A8Unorm
        { 12, 321, 1 },    // R8Snorm
        { 13, 323, 1 },    // R8Uint
        { 14, 322, 1 },    // R8Sint
        { 20, 266, 2 },    // R16Unorm
        { 22, 267, 2 },    // R16Snorm
        { 23, 269, 2 },    // R16Uint
        { 24, 268, 2 },    // R16Sint
        { 32, 263, 2 },    // RG8Snorm
        { 33, 265, 2 },    // RG8Uint
        { 60, 204, 4 },    // RG16Unorm
        { 62, 205, 4 },    // RG16Snorm
        { 63, 207, 4 },    // RG16Uint
        { 64, 206, 4 },    // RG16Sint
        { 71, 200, 4 },    // RGBA8Unorm_sRGB
        { 72, 201, 4 },    // RGBA8Snorm
        { 74, 202, 4 },    // RGBA8Sint
        { 81, 193, 4 },    // BGRA8Unorm_sRGB
        { 90, 194, 4 },    // RGB10A2Unorm
        { 92, 211, 4 },    // RG11B10Float
        { 93, 237, 4 },    // RGB9E5Float (sample only)
        { 94, 209, 4 },    // BGR10A2Unorm
        { 103, 135, 8 },   // RG32Uint
        { 104, 134, 8 },   // RG32Sint
        { 110, 128, 8 },   // RGBA16Unorm
        { 112, 129, 8 },   // RGBA16Snorm
        { 113, 131, 8 },   // RGBA16Uint
        { 114, 130, 8 },   // RGBA16Sint
        { 124, 1, 16 },    // RGBA32Sint
        { 10, 320, 1 },    // R8Unorm -> R8_UNORM
        { 25, 270, 2 },    // R16Float -> R16_FLOAT
        { 30, 262, 2 },    // RG8Unorm -> R8G8_UNORM
        { 53, 215, 4 },    // R32Uint
        { 54, 214, 4 },    // R32Sint
        { 55, 216, 4 },    // R32Float
        { 65, 208, 4 },    // RG16Float
        { 70, 199, 4 },    // RGBA8Unorm -> R8G8B8A8_UNORM
        { 73, 203, 4 },    // RGBA8Uint
        { 80, 192, 4 },    // BGRA8Unorm -> B8G8R8A8_UNORM
        { 105, 133, 8 },   // RG32Float
        { 115, 132, 8 },   // RGBA16Float
        { 123, 2, 16 },    // RGBA32Uint
        { 125, 0, 16 },    // RGBA32Float
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++)
        if (t[i].pf == pf) { *gen = t[i].gen; *bpp = t[i].bpp; return true; }
    return false;
}

static XeTex *tex(id t) { return t ? *(XeTex **)((char *)t + ivar_getOffset(gTexIvar)) : NULL; }

// v0.57.9: tiling policy. IRISXE_TILED=0 / 1 / rt in the environment (1 = every eligible texture, a test), otherwise
// render targets, unless /Library/Preferences/irisxe-tiling says "off".
static int tilePolicy(void) {
    const char *e = getenv("IRISXE_TILED");
    if (e) return e[0] == '0' ? 0 : e[0] == '1' ? 2 : 1;
    static int filePol = -1;
    if (filePol < 0) {
        filePol = 1;
        FILE *f = fopen("/Library/Preferences/irisxe-tiling", "r");
        if (f) { char b[16] = {}; if (fgets(b, sizeof(b), f) && !strncmp(b, "off", 3)) filePol = 0; fclose(f); }
    }
    return filePol;
}
static bool tileIntPF(NSUInteger pf) {      // integer formats stay linear (the copy shader reads floats)
    return pf == 13 || pf == 14 || pf == 23 || pf == 24 || pf == 33 || pf == 34 || pf == 53 || pf == 54 || pf == 73 ||
           pf == 74 || pf == 123 || pf == 124;
}
static bool (*gCopyReady)(id dev, NSUInteger pf);   // v0.57.9: IrisXeMTLRender.inc: the copy pipeline for this format builds
// v0.57.9: copy a region between two of our textures by drawing (dst a 2D texture we can render into, src any 2D)
static bool (*gCopyDraw)(id enc, id src, NSUInteger sLevel, MTLOrigin so, MTLSize sz, id dst, NSUInteger dLevel, MTLOrigin dorig);
static unsigned gTiledBlits[4];                     // drawn, through a temporary, failed, blitter (none tiled)
static id xe_newTexture(id self, SEL _cmd, id desc) {
    NSUInteger type = ((NSUInteger (*)(id, SEL))objc_msgSend)(desc, sel_registerName("textureType"));
    NSUInteger pf = ((NSUInteger (*)(id, SEL))objc_msgSend)(desc, sel_registerName("pixelFormat"));
    NSUInteger w = ((NSUInteger (*)(id, SEL))objc_msgSend)(desc, sel_registerName("width"));
    NSUInteger h = ((NSUInteger (*)(id, SEL))objc_msgSend)(desc, sel_registerName("height"));
    NSUInteger layers = ((NSUInteger (*)(id, SEL))objc_msgSend)(desc, sel_registerName("arrayLength"));
    NSUInteger mips = ((NSUInteger (*)(id, SEL))objc_msgSend)(desc, sel_registerName("mipmapLevelCount"));
    NSUInteger samples = ((NSUInteger (*)(id, SEL))objc_msgSend)(desc, sel_registerName("sampleCount"));
    uint32_t gen = 0; NSUInteger bpp = 0;
    const char *why = NULL;
    NSUInteger dBpp = 0; bool dsSt = false;
    bool ds = dsFormat(pf, &dBpp, &dsSt);
    NSUInteger apiType = type;
    if (type == 0 || type == 1) { type += 2; h = 1; }        // v0.51.5: 1D / 1D array -> 2D / 2D array, height 1
    // v0.66.0: no MSAA in the GPU yet. A multisample texture (2D multisample 4, 2D multisample array 8) is made as a
    // plain 2D (array) texture and drawn with one sample; the render pass's multisample resolve becomes a copy. Programs
    // that require MSAA (Sequoia's wallpaper renderer) then work, only without the extra edge smoothing.
    NSUInteger askedSamples = samples;
    if (type == 4) type = 2;
    else if (type == 8) type = 3;
    if (samples > 1) samples = 1;
    bool cube = type == 5 || type == 6;                      // v0.41.0: cube / cube array = a 2D array of 6 x n faces
    bool vol = type == 7;                                    // v0.51.8: 3D = depth slices laid out like array layers, sampled as SURFTYPE_3D
    if (vol) layers = ((NSUInteger (*)(id, SEL))objc_msgSend)(desc, sel_registerName("depth"));
    if (type != 2 && type != 3 && !cube && !vol) why = "only 2D, 2D array, cube and 3D textures";
    else if (vol && (mips > 1 || (layers > 2048))) why = "3D: one mipmap level, depth up to 2048";
    else if (cube && w != h) why = "cube: width must equal height";
    else if (ds && type != 2) why = "depth / stencil: only 2D textures yet";
    else if (!ds && !texFormat(pf, &gen, &bpp)) why = "pixel format not supported yet";
    else if (mips > 15) why = "at most 15 mipmap levels";
    else if (mips > 1 && ds) why = "depth / stencil textures: one mipmap level yet";
    else if (samples > 1) why = "no multisampling";
    else if (!w || !h || w > 16384 || h > 16384) why = "size";
    if (type == 2 || type == 5) layers = 1;
    if (!layers) layers = 1;
    if (cube) layers *= 6;
    if (why) {
        os_log_error(gLog, "IrisXeMTL: newTexture type %lu format %lu %lux%lu x%lu mips %lu: %{public}s",
                     (unsigned long)type, (unsigned long)pf, (unsigned long)w, (unsigned long)h,
                     (unsigned long)layers, (unsigned long)mips, why);
        return nil;
    }
    XeTex *x = calloc(1, sizeof(XeTex));
    x->width = w; x->height = h; x->layers = layers; x->type = type; x->format = pf; x->gen = gen; x->bpp = bpp;
    if (apiType != type) x->apiType1 = apiType + 1;
    x->samples = askedSamples ? askedSamples : 1;
    x->usage = ((NSUInteger (*)(id, SEL))objc_msgSend)(desc, sel_registerName("usage"));
    x->storage = ((NSUInteger (*)(id, SEL))objc_msgSend)(desc, sel_registerName("storageMode"));
    x->pitch = (w * bpp + 63) & ~(NSUInteger)63;
    x->qpitch = (h + 3) & ~(NSUInteger)3;
    NSUInteger len = x->pitch * (type != 2 ? x->qpitch * layers : h);
    x->levels = mips ? mips : 1;
    x->lvlW[0] = w; x->lvlH[0] = h;
    if (x->levels > 1) {                                     // isl_calc_phys_slice0_extent_sa_gfx4_2d + get_image_offset
        NSUInteger topW = 0, botW = 0, leftH = 0, rightH = 0;
        for (NSUInteger l = 0; l < x->levels; l++) {
            NSUInteger lw = MAX(w >> l, 1), lh = MAX(h >> l, 1);
            NSUInteger W = (lw + 3) & ~(NSUInteger)3, H = (lh + 3) & ~(NSUInteger)3;
            x->lvlW[l] = lw; x->lvlH[l] = lh;
            if (l == 0) { topW = W; leftH += H; rightH += H; }
            else if (l == 1) { botW = W; leftH += H; }
            else if (l == 2) { botW += W; rightH += H; }
            else rightH += H;
        }
        NSUInteger physW = MAX(topW, botW), physH = MAX(leftH, rightH);
        x->pitch = (physW * bpp + 63) & ~(NSUInteger)63;
        x->qpitch = (physH + 3) & ~(NSUInteger)3;
        for (NSUInteger l = 0; l < x->levels; l++) {         // level l: x += W1 once (at level 2), y += H of the others
            NSUInteger px = 0, py = 0;
            for (NSUInteger k = 0; k < l; k++) {
                if (k == 1) px += (MAX(w >> 1, 1) + 3) & ~(NSUInteger)3;
                else py += (MAX(h >> k, 1) + 3) & ~(NSUInteger)3;
            }
            x->lvlOff[l] = py * x->pitch + px * bpp;
        }
        len = x->pitch * x->qpitch * layers;
    }
    if (ds) {                                                // depth plane then stencil plane, both Y-tiled
        x->isDepth = dBpp != 0; x->isStencil = dsSt;
        x->bpp = dBpp ? dBpp : 1;
        x->gen = dBpp == 2 ? 266 : dBpp == 4 ? 216 : 323;   // as a texture: R16_UNORM / R32_FLOAT / R8_UINT
        len = 0;
        if (dBpp) {
            x->pitch = (w * dBpp + 127) & ~(NSUInteger)127;
            x->dRows = (h + 31) & ~(NSUInteger)31;
            len = (x->pitch * x->dRows + 4095) & ~(NSUInteger)4095;
        }
        if (dsSt) {                                          // Gen12: stencil is Y-tiled (isl_gfx6_filter_tiling), not W
            x->sOff = len;
            x->sPitch = (w + 127) & ~(NSUInteger)127;
            x->sRows = (h + 31) & ~(NSUInteger)31;
            len += (x->sPitch * x->sRows + 4095) & ~(NSUInteger)4095;
            if (!dBpp) x->pitch = x->sPitch;
        }
        x->qpitch = x->dRows;
    }
    // v0.57.7 (Step A): Y-tiled colour textures (4 KB tiles of 128 bytes x 32 rows), the layout Intel's GPUs are built
    // for. v0.57.9 (Step B): on by default for render targets (WindowServer's work surfaces): 2D, one level, a float /
    // unorm format, own memory (never IOSurface or buffer textures). CPU copies (replaceRegion / getBytes) convert;
    // blit copies with a tiled texture are drawn (copyTiled). tilePolicy(): 0 off, 1 render targets, 2 all (tests).
    if (!ds && !gTexNoTile && type == 2 && x->levels == 1 && layers == 1 && bpp <= 16 && !tileIntPF(pf)) {
        int pol = tilePolicy();
        if ((pol == 2 || (pol == 1 && (x->usage & 4))) && gCopyReady && gCopyReady(self, pf)) {
            x->tiledColor = true;
            x->pitch = (w * bpp + 127) & ~(NSUInteger)127;
            x->qpitch = (h + 31) & ~(NSUInteger)31;
            len = (x->pitch * x->qpitch + 4095) & ~(NSUInteger)4095;
        }
    }
    x->size = len;
    x->buf = ((id (*)(id, SEL, NSUInteger, NSUInteger))objc_msgSend)(self, sel_registerName("newBufferWithLength:options:"),
                                                                   len, MTLResourceStorageModeShared);
    if (!x->buf) { free(x); return nil; }
    memset(((void *(*)(id, SEL))objc_msgSend)(x->buf, sel_registerName("contents")), 0, len);
    x->dev = [self retain];
    id t = [gTexClass alloc];
    *(XeTex **)((char *)t + ivar_getOffset(gTexIvar)) = x;
    os_log(gLog, "IrisXeMTL: texture %lux%lu x%lu format %lu (gen %u) pitch %lu at GPU 0x%llx", (unsigned long)w,
           (unsigned long)h, (unsigned long)layers, (unsigned long)pf, gen, (unsigned long)x->pitch,
           ((uint64_t (*)(id, SEL))objc_msgSend)(x->buf, sel_registerName("gpuAddress")));
    return t;
}
static void xe_texDealloc(id self, SEL _cmd) {
    XeTex *x = tex(self);
    if (x) { [x->buf release]; [x->srcBuf release]; [x->dev release]; [x->label release]; if (x->ios) CFRelease(x->ios); free(x); }
    struct objc_super sup = { self, class_getSuperclass(gTexClass) };
    ((void (*)(struct objc_super *, SEL))objc_msgSendSuper)(&sup, _cmd);
}
static NSUInteger xe_texWidth(id s, SEL c)  { return tex(s)->width; }
static NSUInteger xe_texHeight(id s, SEL c) { return tex(s)->height; }
static NSUInteger xe_texOne(id s, SEL c)    { return 1; }
static NSUInteger xe_texSamples(id s, SEL c) { XeTex *x = tex(s); return x->samples ? x->samples : 1; }   // v0.66.0
static NSUInteger xe_texDepth(id s, SEL c)  { XeTex *x = tex(s); return x->type == 7 ? x->layers : 1; }
static NSUInteger xe_texZero(id s, SEL c)   { return 0; }
static NSUInteger xe_texLayers(id s, SEL c) {      // arrayLength: cubes count 6 faces as one
    XeTex *x = tex(s);
    return x->type == 5 || x->type == 7 ? 1 : x->type == 6 ? x->layers / 6 : x->layers;
}
static NSUInteger xe_texType(id s, SEL c)   { XeTex *x = tex(s); return x->apiType1 ? x->apiType1 - 1 : x->type; }
static NSUInteger xe_texFormat(id s, SEL c) { return tex(s)->format; }
static NSUInteger xe_texUsage(id s, SEL c)  { return tex(s)->usage; }
static NSUInteger xe_texStorage(id s, SEL c){ return tex(s)->storage; }
static NSUInteger xe_texSize(id s, SEL c)   { return tex(s)->size; }
static NSUInteger xe_texLevels(id s, SEL c) { return tex(s)->levels; }
static id xe_texDevice(id s, SEL c)         { return tex(s)->dev; }
static id xe_texLabel(id s, SEL c)          { return tex(s)->label; }
static void xe_texSetLabel(id s, SEL c, id l) { XeTex *x = tex(s); [l retain]; [x->label release]; x->label = l; }
static BOOL xe_texNo(id s, SEL c)           { return NO; }
static id xe_texNil(id s, SEL c)            { return nil; }

// v0.68.24: texture views (the apps test: mediaanalysisd's machine learning (MPS) and the Memoji picker (SceneKit)
// crashed on a nil view). A view shares the texture's memory and layout; for now the whole texture (all levels and
// slices) with a pixel format of the same size per texel (sRGB <-> linear, Uint <-> Unorm ...) and a type with the
// same number of layers (2D <-> 2D array of one). Anything else: nil, logged once per kind.
static id texView(id self, NSUInteger pf, NSUInteger type, NSRange lv, NSRange sl, bool full) {
    XeTex *x = tex(self);
    if (!x) return nil;
    const char *why = NULL;
    uint32_t gen = x->gen; NSUInteger bpp = x->bpp;
    NSUInteger curType = x->apiType1 ? x->apiType1 - 1 : x->type;
    if (pf != x->format) {
        if (x->isDepth || x->isStencil) why = "depth / stencil";
        else if (!texFormat(pf, &gen, &bpp)) why = "pixel format";
        else if (bpp != x->bpp) why = "texel size differs";
    }
    if (!why && full) {
        if (lv.location != 0 || lv.length != x->levels) why = "part of the mip levels";
        else if (sl.location != 0 || sl.length != (x->type == 6 ? x->layers : x->layers)) {
            if (!(sl.location == 0 && sl.length == xe_texLayers(self, NULL))) why = "part of the slices";
        }
        if (!why && type != curType) {
            bool ok = (type == 2 || type == 3) && (curType == 2 || curType == 3) && x->layers == 1;   // 2D <-> 2D array of 1
            if (!ok) why = "texture type";
        }
    }
    if (why) {
        static NSMutableSet *seen;
        @synchronized (gMissing) {
            if (!seen) seen = [NSMutableSet new];
            NSString *k = [NSString stringWithUTF8String:why];
            if (![seen containsObject:k]) {
                [seen addObject:k];
                os_log(gLog, "IrisXeMTL: texture view not made (%{public}s): format %lu -> %lu, type %lu -> %lu, levels %lu+%lu of %lu, slices %lu+%lu",
                       why, (unsigned long)x->format, (unsigned long)pf, (unsigned long)curType, (unsigned long)type,
                       (unsigned long)lv.location, (unsigned long)lv.length, (unsigned long)x->levels,
                       (unsigned long)sl.location, (unsigned long)sl.length);
            }
        }
        return nil;
    }
    XeTex *v = malloc(sizeof(XeTex));
    if (!v) return nil;
    *v = *x;
    v->format = pf; v->gen = gen;
    if (full && type != curType) { v->type = type; v->apiType1 = 0; }
    [v->buf retain]; [v->srcBuf retain]; [v->dev retain];
    v->label = nil;
    if (v->ios) CFRetain(v->ios);
    id t = [gTexClass alloc];
    *(XeTex **)((char *)t + ivar_getOffset(gTexIvar)) = v;
    return t;
}
static id xe_texViewPF(id self, SEL _cmd, NSUInteger pf) {
    return texView(self, pf, 0, NSMakeRange(0, 0), NSMakeRange(0, 0), false);
}
static id xe_texViewFull(id self, SEL _cmd, NSUInteger pf, NSUInteger type, NSRange lv, NSRange sl) {
    return texView(self, pf, type, lv, sl, true);
}
static id xe_texParent(id s, SEL c) { return nil; }

// CPU copies (shared memory, rows of `pitch`)
static void srcCopy(XeTex *x, bool toBuffer);   // v0.51.31
static void texCopy(id self, MTLRegion r, NSUInteger level, NSUInteger slice, void *mem, NSUInteger bpr,
                    NSUInteger bpi, bool toTex) {
    XeTex *x = tex(self);
    if (x->type == 7 && (r.size.depth > 1 || r.origin.z)) {   // v0.51.8: 3D: z selects the slice, one slice at a time
        for (NSUInteger k = 0; k < r.size.depth; k++) {
            MTLRegion r1 = r; r1.origin.z = 0; r1.size.depth = 1;
            texCopy(self, r1, level, r.origin.z + k, (uint8_t *)mem + k * bpi, bpr, bpi, toTex);
        }
        return;
    }
    if (level >= x->levels || slice >= x->layers || r.origin.x + r.size.width > x->lvlW[level] ||
        r.origin.y + r.size.height > x->lvlH[level] || r.origin.z + r.size.depth > 1) {
        os_log_error(gLog, "IrisXeMTL: texture %s: region outside the texture (or mip level %lu)", toTex ? "replaceRegion" : "getBytes",
                     (unsigned long)level);
        return;
    }
    if (x->srcBuf && !toTex && !x->srcWritten) srcCopy(x, false);   // v0.51.31: read what the buffer holds now
    uint8_t *base = ((uint8_t *(*)(id, SEL))objc_msgSend)(x->buf, sel_registerName("contents")) + x->bufOff;
    if (x->isDepth || x->isStencil) {                       // v0.34.0: tiled planes (depth texels, or the stencil bytes)
        for (NSUInteger y = 0; y < r.size.height; y++)
            for (NSUInteger i = 0; i < r.size.width; i++) {
                NSUInteger px = r.origin.x + i, py = r.origin.y + y;
                uint8_t *m = (uint8_t *)mem + y * bpr + i * x->bpp;
                uint8_t *t = x->isDepth ? base + tileYOffset(x->pitch, px * x->bpp, py)
                                        : base + x->sOff + tileYOffset(x->sPitch, px, py);
                if (toTex) memcpy(t, m, x->bpp); else memcpy(m, t, x->bpp);
            }
        return;
    }
    if (x->tiledColor) {                                    // v0.57.7: Y-tiled: 16-byte columns of 32 rows inside 4 KB tiles
        NSUInteger x0 = r.origin.x * x->bpp, x1 = x0 + r.size.width * x->bpp;
        for (NSUInteger y = 0; y < r.size.height; y++) {
            uint8_t *m = (uint8_t *)mem + y * bpr;
            for (NSUInteger xb = x0; xb < x1; ) {
                NSUInteger n = MIN(16 - (xb & 15), x1 - xb);
                uint8_t *t = base + tileYOffset(x->pitch, xb, r.origin.y + y);
                if (toTex) memcpy(t, m + (xb - x0), n); else memcpy(m + (xb - x0), t, n);
                xb += n;
            }
        }
        return;
    }
    base += slice * x->qpitch * x->pitch + x->lvlOff[level];
    NSUInteger rowBytes = r.size.width * x->bpp;
    for (NSUInteger y = 0; y < r.size.height; y++) {
        uint8_t *t = base + (r.origin.y + y) * x->pitch + r.origin.x * x->bpp;
        uint8_t *m = (uint8_t *)mem + y * bpr;
        if (toTex) memcpy(t, m, rowBytes); else memcpy(m, t, rowBytes);
    }
    if (toTex && x->srcBuf) srcCopy(x, true);   // v0.51.31: a shadowed buffer texture: the buffer gets it too
    (void)bpi;
}
static void xe_texReplace(id self, SEL _cmd, MTLRegion r, NSUInteger level, const void *bytes, NSUInteger bpr) {
    texCopy(self, r, level, 0, (void *)bytes, bpr, 0, true);
}
static void xe_texReplaceSlice(id self, SEL _cmd, MTLRegion r, NSUInteger level, NSUInteger slice, const void *bytes,
                               NSUInteger bpr, NSUInteger bpi) {
    texCopy(self, r, level, slice, (void *)bytes, bpr, bpi, true);
}
static void xe_texGetBytes(id self, SEL _cmd, void *bytes, NSUInteger bpr, MTLRegion r, NSUInteger level) {
    texCopy(self, r, level, 0, bytes, bpr, 0, false);
}
static void xe_texGetBytesSlice(id self, SEL _cmd, void *bytes, NSUInteger bpr, NSUInteger bpi, MTLRegion r,
                                NSUInteger level, NSUInteger slice) {
    texCopy(self, r, level, slice, bytes, bpr, bpi, false);
}
// what the dispatch command needs about a texture
static void iosCopy(XeTex *x, bool toSurface);
// v0.51.24: an IOSurface texture that is a copy (not drawn in place) and that only other processes draw into
// (WindowServer reading an app's window): copy the surface in again every time the GPU is about to read it.
// Before, it was copied in once when the texture was made - an app's window drawn later stayed black.
// v0.51.31: copy a shadowed buffer texture between its buffer and its own memory (toBuffer: back to the buffer)
static void srcCopy(XeTex *x, bool toBuffer) {
    if (!x || !x->srcBuf) return;
    uint8_t *sb = ((uint8_t *(*)(id, SEL))objc_msgSend)(x->srcBuf, sel_registerName("contents"));
    uint8_t *tb = ((uint8_t *(*)(id, SEL))objc_msgSend)(x->buf, sel_registerName("contents"));
    if (!sb || !tb) return;
    sb += x->srcOff;
    size_t rb = x->width * x->bpp;
    for (NSUInteger y = 0; y < x->height; y++) {
        if (toBuffer) memcpy(sb + y * x->srcBpr, tb + y * x->pitch, rb);
        else          memcpy(tb + y * x->pitch, sb + y * x->srcBpr, rb);
    }
}
// v0.68.45: a shadowed buffer texture is refreshed from its buffer once per render encoder, not before every draw
// that reads it (WindowServer reads the same image in many draws: a whole-image memcpy each time showed up in the
// CPU profile). The CPU cannot change the buffer for the GPU between two draws of one encoder anyway.
static volatile uint32_t gEncSerial = 1;
static void iosRefresh(XeTex *x) {
    if (x && x->ios && !x->iosDirect && !x->iosWritten) iosCopy(x, false);
    if (x && x->srcBuf && !x->srcWritten) {                        // v0.51.31
        static int all = -1;
        if (all < 0) all = getenv("IRISXE_SRCCOPYALL") ? 1 : 0;
        uint32_t ser = gEncSerial;
        if (x->srcSerial != ser || all) { srcCopy(x, false); x->srcSerial = ser; }
    }
}
static void texDesc(id t, XeTexDescU *d, id *bufOut) {
    XeTex *x = tex(t);
    memset(d, 0, sizeof(*d));
    *bufOut = nil;
    if (!x) return;
    iosRefresh(x);
    d->va = ((uint64_t (*)(id, SEL))objc_msgSend)(x->buf, sel_registerName("gpuAddress")) + x->bufOff;
    d->width = (uint32_t)x->width; d->height = (uint32_t)x->height; d->layers = (uint32_t)x->layers;
    d->pitch = (uint32_t)x->pitch; d->qpitch = (uint32_t)x->qpitch; d->format = x->gen; d->array = x->type != 2;
    d->pad = (uint32_t)x->levels;                            // v0.36.0: bits 7:0 mip levels, 15:8 the level a render target uses
    if (x->type == 5 || x->type == 6) d->pad |= 1u << 18;   // v0.41.0: sampled as a cube
    if (x->type == 7) d->pad |= 1u << 19;                    // v0.51.8: sampled as SURFTYPE_3D
    if (x->isDepth || x->tiledColor) d->pad |= 1u << 16;     // v0.37.0: the depth plane, Y-tiled (qpitch = rows allocated); v0.57.7 + tiled colour
    else if (x->isStencil) d->va = 0;                        // stencil-only: not readable in shaders yet
    *bufOut = x->buf;
}

// ---- v0.38.0: IOSurface-backed textures ----
static void iosCopy(XeTex *x, bool toSurface) {
    IOSurfaceRef s = x ? x->ios : NULL;
    if (!s || x->iosDirect) return;                          // v0.50.0: nothing to copy
    uint32_t opts = toSurface ? 0 : kIOSurfaceLockReadOnly;
    if (IOSurfaceLock(s, opts, NULL) != kIOReturnSuccess) return;
    bool planes = IOSurfaceGetPlaneCount(s) > 0;
    uint8_t *sb = planes ? IOSurfaceGetBaseAddressOfPlane(s, x->iosPlane) : IOSurfaceGetBaseAddress(s);
    size_t sbpr = planes ? IOSurfaceGetBytesPerRowOfPlane(s, x->iosPlane) : IOSurfaceGetBytesPerRow(s);
    size_t sh = planes ? IOSurfaceGetHeightOfPlane(s, x->iosPlane) : IOSurfaceGetHeight(s);
    uint8_t *tb = ((uint8_t *(*)(id, SEL))objc_msgSend)(x->buf, sel_registerName("contents")) + x->bufOff;
    if (sb && tb) {
        size_t rows = MIN(sh, x->height), rb = MIN(sbpr, x->width * x->bpp);
        for (size_t y = 0; y < rows; y++) {
            if (toSurface) memcpy(sb + y * sbpr, tb + y * x->pitch, rb);
            else memcpy(tb + y * x->pitch, sb + y * sbpr, rb);
        }
    }
    IOSurfaceUnlock(s, opts, NULL);
}
static id xe_newTexture(id self, SEL _cmd, id desc);
static id xe_newTextureIOS(id self, SEL _cmd, id desc, IOSurfaceRef s, NSUInteger plane) {
    if (!s) return nil;
    gTexNoTile = true;
    id t = xe_newTexture(self, sel_registerName("newTextureWithDescriptor:"), desc);
    gTexNoTile = false;
    XeTex *x = tex(t);
    if (!x) return t;
    if (x->isDepth || x->isStencil || x->levels > 1 || x->type != 2) {
        os_log_error(gLog, "IrisXeMTL: IOSurface textures: 2D color, one level only (got another kind; not linked)");
        return t;
    }
    x->ios = (IOSurfaceRef)CFRetain(s); x->iosPlane = plane;
    // v0.50.0: zero copy. The GPU draws straight into the IOSurface's own pages (wrapped like
    // newBufferWithBytesNoCopy) instead of into our memory with a CPU copy of every frame when the command
    // buffer completes. Needs: one plane, page-aligned memory, rows 64-byte aligned, room for every row.
    // IRISXE_IOSCOPY=1: the old way (copy).
    static int copyMode = -1;
    if (copyMode < 0) copyMode = getenv("IRISXE_IOSCOPY") != NULL || gDesktopIOSCopy;   // v0.51.25: or "copy" in the desktop test
    void *base = IOSurfaceGetBaseAddress(s);
    size_t bpr = IOSurfaceGetBytesPerRow(s), alloc = IOSurfaceGetAllocSize(s);
    size_t need = bpr * x->height;
    if (!copyMode && IOSurfaceGetPlaneCount(s) == 0 && plane == 0 && base && !((uintptr_t)base & 4095) &&
        bpr >= x->width * x->bpp && !(bpr & 63) && alloc >= need && IOSurfaceGetHeight(s) >= x->height) {
        id b = makeBuffer(self, base, (alloc + 4095) & ~(size_t)4095, MTLResourceStorageModeShared, nil);
        uint64_t va = b ? ((uint64_t (*)(id, SEL))objc_msgSend)(b, sel_registerName("gpuAddress")) : 0;
        if (b && va && !(va & 4095)) {
            [x->buf release];
            x->buf = b; x->pitch = bpr; x->size = alloc; x->iosDirect = true;
            os_log(gLog, "IrisXeMTL: IOSurface %u texture %lux%lu format %lu: drawn in place (pitch %lu, GPU 0x%llx)",
                   IOSurfaceGetID(s), (unsigned long)x->width, (unsigned long)x->height, (unsigned long)x->format,
                   (unsigned long)bpr, va);
            return t;
        }
        [b release];
    }
    iosCopy(x, false);
    // v0.51.24: say why (planes, base address, row pitch, size)
    os_log(gLog, "IrisXeMTL: IOSurface %u texture %lux%lu format %lu (copied: %{public}s; planes %zu, base %p, pitch %zu for %lu, alloc %zu of %zu, height %zu)",
           IOSurfaceGetID(s), (unsigned long)x->width, (unsigned long)x->height, (unsigned long)x->format,
           copyMode ? "IRISXE_IOSCOPY" : "not suitable for drawing in place", IOSurfaceGetPlaneCount(s), base, bpr,
           (unsigned long)(x->width * x->bpp), alloc, need, IOSurfaceGetHeight(s));
    return t;
}
// v0.51.1: [MTLBuffer newTextureWithDescriptor:offset:bytesPerRow:] (WindowServer / Core Animation upload window
// contents this way; v0.51.0 crashed WindowServer here): a 2D texture sharing the buffer's memory
static id xe_bufNewTexture(id self, SEL _cmd, id desc, NSUInteger off, NSUInteger bpr) {
    id dev = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("device"));
    gTexNoTile = true;
    id t = xe_newTexture(dev, sel_registerName("newTextureWithDescriptor:"), desc);
    gTexNoTile = false;
    XeTex *x = tex(t);
    if (!x) return t;
    NSUInteger blen = ((NSUInteger (*)(id, SEL))objc_msgSend)(self, sel_registerName("length"));
    if (x->isDepth || x->isStencil || x->levels > 1 || x->type != 2 || bpr < x->width * x->bpp ||
        off + bpr * (x->height - 1) + x->width * x->bpp > blen) {
        os_log_error(gLog, "IrisXeMTL: buffer texture %lux%lu offset %lu bytesPerRow %lu: not shared (own memory)",
                     (unsigned long)x->width, (unsigned long)x->height, (unsigned long)off, (unsigned long)bpr);
        return t;
    }
    uint64_t va = ((uint64_t (*)(id, SEL))objc_msgSend)(self, sel_registerName("gpuAddress")) + off;
    if ((va & 0xFFF) || (bpr & 63)) {          // v0.51.31: the GPU core takes 4 KB aligned textures, rows of 64 bytes
        static int said;
        if (said++ < 8)
            os_log(gLog, "IrisXeMTL: buffer texture %lux%lu at offset %lu (bytesPerRow %lu): not 4 KB aligned - own memory, refreshed from the buffer",
                   (unsigned long)x->width, (unsigned long)x->height, (unsigned long)off, (unsigned long)bpr);
        x->srcBuf = [self retain]; x->srcOff = off; x->srcBpr = bpr;
        srcCopy(x, false);
        return t;
    }
    [x->buf release];
    x->buf = [self retain]; x->bufOff = off; x->pitch = bpr; x->onBuffer = true;
    x->size = bpr * x->height;
    return t;
}
static id xe_texBuffer(id s, SEL c) { XeTex *x = tex(s); return x && x->srcBuf ? x->srcBuf : x && x->onBuffer ? x->buf : nil; }
static NSUInteger xe_texBufferOffset(id s, SEL c) { XeTex *x = tex(s); return x && x->srcBuf ? x->srcOff : x && x->onBuffer ? x->bufOff : 0; }
static NSUInteger xe_texBufferBPR(id s, SEL c) { XeTex *x = tex(s); return x && x->srcBuf ? x->srcBpr : x && x->onBuffer ? x->pitch : 0; }
static IOSurfaceRef xe_texIOS(id s, SEL c) { return tex(s)->ios; }
static NSUInteger xe_texIOSPlane(id s, SEL c) { return tex(s)->iosPlane; }
// a command buffer that draws / copies into IOSurface textures copies them out when it completes
static char gIOSKey;
static void markIOS(id cb, id t) {
    XeTex *x = t && object_getClass(t) == gTexClass ? tex(t) : NULL;
    if (x && x->srcBuf) x->srcWritten = true;   // v0.51.31: GPU-written from now on: no refresh from the buffer
    if (!x || !x->ios || x->iosDirect || !cb) return;
    x->iosWritten = true;                                           // v0.51.24: ours from now on (copied out, not in)
    NSMutableArray *a = objc_getAssociatedObject(cb, &gIOSKey);
    if (!a) {
        a = [NSMutableArray array];
        objc_setAssociatedObject(cb, &gIOSKey, a, OBJC_ASSOCIATION_RETAIN);
        ((void (*)(id, SEL, id))objc_msgSend)(cb, sel_registerName("addCompletedHandler:"), ^(id b) {
            @synchronized (a) { for (id u in a) iosCopy(tex(u), true); }
        });
    }
    @synchronized (a) { if (![a containsObject:t]) [a addObject:t]; }
}
static void presentWhenDone(id cb, id drawable, double at) {
    id d = [drawable retain];
    ((void (*)(id, SEL, id))objc_msgSend)(cb, sel_registerName("addCompletedHandler:"), ^(id b) {
        id t = ((id (*)(id, SEL))objc_msgSend)(d, sel_registerName("texture"));
        if (t && object_getClass(t) == gTexClass) iosCopy(tex(t), true);
        if (at > 0) ((void (*)(id, SEL, double))objc_msgSend)(d, sel_registerName("presentAtTime:"), at);
        else ((void (*)(id, SEL))objc_msgSend)(d, sel_registerName("present"));
        [d release];
    });
}
static void xe_cbPresent(id self, SEL _cmd, id drawable) { if (drawable) presentWhenDone(self, drawable, 0); }
static void xe_cbPresentAt(id self, SEL _cmd, id drawable, double t) { if (drawable) presentWhenDone(self, drawable, t); }

// ---- v0.36.0: blit encoder copies with textures (GPU rectangle copies on the blitter) ----
#define XE_KCMD_COPY2D 0x10005u
typedef struct { uint32_t type, size; uint64_t srcVA, dstVA; uint32_t srcPitch, dstPitch, widthBytes, height; } XeCopy2DCmd;
static void copy2D(id enc, id srcBuf, uint64_t src, uint32_t sPitch, id dstBuf, uint64_t dst, uint32_t dPitch,
                   uint32_t widthBytes, uint32_t height) {
    if (!widthBytes || !height) return;
    id cb = ((id (*)(id, SEL))objc_msgSend)(enc, sel_registerName("commandBuffer"));
    id bufs[2] = { srcBuf, dstBuf };
    XeCopy2DCmd *k = beginKernelCommand(cb, bufs, 2, sizeof(XeCopy2DCmd));
    if (k) {
        k->type = XE_KCMD_COPY2D; k->size = sizeof(XeCopy2DCmd);
        k->srcVA = src; k->dstVA = dst; k->srcPitch = sPitch; k->dstPitch = dPitch; k->widthBytes = widthBytes; k->height = height;
        if (recOn()) recordCmd("blit: copy 2D", k, sizeof(*k), bufs, 2);
    }
    endKernelCommand(cb);
}
// a region of one level / slice: its GPU address, or 0 (with a log) when it isn't one of ours or is outside
static uint64_t texRegionVA(id t, NSUInteger slice, NSUInteger level, MTLOrigin o, MTLSize sz, const char *what) {
    XeTex *x = object_getClass(t) == gTexClass ? tex(t) : NULL;
    if (!x) { os_log_error(gLog, "IrisXeMTL: %{public}s: not our texture", what); return 0; }
    iosRefresh(x);                                                  // v0.51.24
    if (x->isDepth || x->isStencil) { os_log_error(gLog, "IrisXeMTL: %{public}s: depth / stencil textures are not supported yet", what); return 0; }
    if (x->type == 7 && sz.depth <= 1) { slice += o.z; o.z = 0; }   // v0.51.8: 3D: a z slice
    if (level >= x->levels || slice >= x->layers || o.z || sz.depth > 1 || o.x + sz.width > x->lvlW[level] || o.y + sz.height > x->lvlH[level]) {
        os_log_error(gLog, "IrisXeMTL: %{public}s: region outside level %lu / slice %lu", what, (unsigned long)level, (unsigned long)slice);
        return 0;
    }
    return ((uint64_t (*)(id, SEL))objc_msgSend)(x->buf, sel_registerName("gpuAddress")) + x->bufOff + slice * x->qpitch * x->pitch +
           x->lvlOff[level] + o.y * x->pitch + o.x * x->bpp;
}
// v0.57.9: a linear texture of w x h in format pf, kept until the command buffer completes (tiled blit copies)
static id tempLinearTex(id dev, id cb, NSUInteger pf, NSUInteger w, NSUInteger h) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:(MTLPixelFormat)pf width:w height:h mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModeShared;
    gTexNoTile = true;
    id t = xe_newTexture(dev, sel_registerName("newTextureWithDescriptor:"), td);
    gTexNoTile = false;
    if (t && cb) ((void (*)(id, SEL, id))objc_msgSend)(cb, sel_registerName("addCompletedHandler:"), ^(id b) { [t release]; });
    return t;
}
static bool isTiled(id t) { XeTex *x = t && object_getClass(t) == gTexClass ? tex(t) : NULL; return x && x->tiledColor; }
static void tiledBlitLog(const char *what, bool ok) {
    if (!ok) os_log_error(gLog, "IrisXeMTL: %{public}s with a tiled texture failed (drawn %u, via temporary %u, failed %u)", what,
                          gTiledBlits[0], gTiledBlits[1], gTiledBlits[2]);
}
static void xe_blitBufToTex(id self, SEL _cmd, id buf, NSUInteger off, NSUInteger bpr, NSUInteger bpi, MTLSize sz,
                            id t, NSUInteger slice, NSUInteger level, MTLOrigin o) {
    if (isTiled(t)) {                                   // v0.57.9: buffer -> temporary linear texture (blitter) -> draw
        XeTex *x = tex(t);
        id cb = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("commandBuffer"));
        bool ok = false;
        if (texRegionVA(t, slice, level, o, sz, "copyFromBuffer:toTexture:") && buf && sz.width && sz.height) {
            id tmp = tempLinearTex(x->dev, cb, x->format, sz.width, sz.height);
            if (tmp) {
                xe_blitBufToTex(self, _cmd, buf, off, bpr, bpi, sz, tmp, 0, 0, MTLOriginMake(0, 0, 0));
                ok = gCopyDraw && gCopyDraw(self, tmp, 0, MTLOriginMake(0, 0, 0), sz, t, level, o);
            }
        }
        gTiledBlits[ok ? 1 : 2]++;
        tiledBlitLog("copyFromBuffer:toTexture:", ok);
        markIOS(cb, t);
        return;
    }
    uint64_t dst = texRegionVA(t, slice, level, o, sz, "copyFromBuffer:toTexture:");
    if (!dst || !buf) return;
    XeTex *x = tex(t);
    uint64_t src = ((uint64_t (*)(id, SEL))objc_msgSend)(buf, sel_registerName("gpuAddress")) + off;
    if (!bpr) bpr = sz.width * x->bpp;
    copy2D(self, buf, src, (uint32_t)bpr, x->buf, dst, (uint32_t)x->pitch, (uint32_t)(sz.width * x->bpp), (uint32_t)sz.height);
    markIOS(((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("commandBuffer")), t);
    (void)bpi;
}
static void xe_blitBufToTexOpt(id self, SEL _cmd, id buf, NSUInteger off, NSUInteger bpr, NSUInteger bpi, MTLSize sz,
                               id t, NSUInteger slice, NSUInteger level, MTLOrigin o, NSUInteger opts) {
    xe_blitBufToTex(self, _cmd, buf, off, bpr, bpi, sz, t, slice, level, o);
}
static void xe_blitTexToBuf(id self, SEL _cmd, id t, NSUInteger slice, NSUInteger level, MTLOrigin o, MTLSize sz,
                            id buf, NSUInteger off, NSUInteger bpr, NSUInteger bpi) {
    if (isTiled(t)) {                                   // v0.57.9: draw -> temporary linear texture -> buffer (blitter)
        XeTex *x = tex(t);
        id cb = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("commandBuffer"));
        bool ok = false;
        if (texRegionVA(t, slice, level, o, sz, "copyFromTexture:toBuffer:") && buf && sz.width && sz.height) {
            id tmp = tempLinearTex(x->dev, cb, x->format, sz.width, sz.height);
            if (tmp && gCopyDraw && gCopyDraw(self, t, level, o, sz, tmp, 0, MTLOriginMake(0, 0, 0))) {
                xe_blitTexToBuf(self, _cmd, tmp, 0, 0, MTLOriginMake(0, 0, 0), sz, buf, off, bpr, bpi);
                ok = true;
            }
        }
        gTiledBlits[ok ? 1 : 2]++;
        tiledBlitLog("copyFromTexture:toBuffer:", ok);
        return;
    }
    uint64_t src = texRegionVA(t, slice, level, o, sz, "copyFromTexture:toBuffer:");
    if (!src || !buf) return;
    XeTex *x = tex(t);
    uint64_t dst = ((uint64_t (*)(id, SEL))objc_msgSend)(buf, sel_registerName("gpuAddress")) + off;
    if (!bpr) bpr = sz.width * x->bpp;
    copy2D(self, x->buf, src, (uint32_t)x->pitch, buf, dst, (uint32_t)bpr, (uint32_t)(sz.width * x->bpp), (uint32_t)sz.height);
    (void)bpi;
}
static void xe_blitTexToBufOpt(id self, SEL _cmd, id t, NSUInteger slice, NSUInteger level, MTLOrigin o, MTLSize sz,
                               id buf, NSUInteger off, NSUInteger bpr, NSUInteger bpi, NSUInteger opts) {
    xe_blitTexToBuf(self, _cmd, t, slice, level, o, sz, buf, off, bpr, bpi);
}
static void xe_blitTexToTex(id self, SEL _cmd, id s, NSUInteger sSlice, NSUInteger sLevel, MTLOrigin so, MTLSize sz,
                            id d, NSUInteger dSlice, NSUInteger dLevel, MTLOrigin dorig) {
    if (isTiled(s) || isTiled(d)) {                     // v0.57.9: drawn (directly, or through a temporary linear texture)
        XeTex *a = object_getClass(s) == gTexClass ? tex(s) : NULL, *b = object_getClass(d) == gTexClass ? tex(d) : NULL;
        id cb = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("commandBuffer"));
        bool ok = false, direct = false;
        if (a && b && a->bpp == b->bpp && texRegionVA(s, sSlice, sLevel, so, sz, "copyFromTexture:toTexture:") &&
            texRegionVA(d, dSlice, dLevel, dorig, sz, "copyFromTexture:toTexture:") && sz.width && sz.height) {
            bool srcOK = a->type == 2 && !sSlice, dstOK = b->type == 2 && !dSlice && b->format == a->format && !tileIntPF(b->format);
            if (srcOK && dstOK) ok = direct = gCopyDraw && gCopyDraw(self, s, sLevel, so, sz, d, dLevel, dorig);
            else if (isTiled(s) && srcOK) {                 // tiled -> (array / other) texture: through a linear temporary
                id tmp = tempLinearTex(a->dev, cb, a->format, sz.width, sz.height);
                if (tmp && gCopyDraw && gCopyDraw(self, s, sLevel, so, sz, tmp, 0, MTLOriginMake(0, 0, 0))) {
                    xe_blitTexToTex(self, _cmd, tmp, 0, 0, MTLOriginMake(0, 0, 0), sz, d, dSlice, dLevel, dorig);
                    ok = true;
                }
            } else if (isTiled(d) && dstOK) {               // (array) texture -> tiled: through a linear temporary
                id tmp = tempLinearTex(b->dev, cb, b->format, sz.width, sz.height);
                if (tmp) {
                    xe_blitTexToTex(self, _cmd, s, sSlice, sLevel, so, sz, tmp, 0, 0, MTLOriginMake(0, 0, 0));
                    ok = gCopyDraw && gCopyDraw(self, tmp, 0, MTLOriginMake(0, 0, 0), sz, d, dLevel, dorig);
                }
            }
        }
        gTiledBlits[ok ? (direct ? 0 : 1) : 2]++;
        tiledBlitLog("copyFromTexture:toTexture:", ok);
        markIOS(cb, d);
        return;
    }
    uint64_t src = texRegionVA(s, sSlice, sLevel, so, sz, "copyFromTexture:toTexture:");
    uint64_t dst = texRegionVA(d, dSlice, dLevel, dorig, sz, "copyFromTexture:toTexture:");
    if (!src || !dst) return;
    XeTex *a = tex(s), *b = tex(d);
    if (a->bpp != b->bpp) { os_log_error(gLog, "IrisXeMTL: copyFromTexture:toTexture: texel sizes differ"); return; }
    copy2D(self, a->buf, src, (uint32_t)a->pitch, b->buf, dst, (uint32_t)b->pitch, (uint32_t)(sz.width * a->bpp), (uint32_t)sz.height);
    markIOS(((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("commandBuffer")), d);
}
static void xe_blitTexToTexAll(id self, SEL _cmd, id s, id d) {       // copyFromTexture:toTexture: (every level and slice)
    XeTex *a = object_getClass(s) == gTexClass ? tex(s) : NULL, *b = object_getClass(d) == gTexClass ? tex(d) : NULL;
    if (!a || !b) { os_log_error(gLog, "IrisXeMTL: copyFromTexture:toTexture: not our textures"); return; }
    for (NSUInteger l = 0; l < MIN(a->levels, b->levels); l++)
        for (NSUInteger z = 0; z < MIN(a->layers, b->layers); z++)
            xe_blitTexToTex(self, _cmd, s, z, l, MTLOriginMake(0, 0, 0), MTLSizeMake(a->lvlW[l], a->lvlH[l], 1), d, z, l, MTLOriginMake(0, 0, 0));
}
static void (*gGenMips)(id enc, id tex);                // v0.37.0: IrisXeMTLRender.inc (draws each level)
static void xe_blitGenMips(id self, SEL _cmd, id t) {
    if (gGenMips) gGenMips(self, t);
    else os_log_error(gLog, "IrisXeMTL: generateMipmapsForTexture: no render support in this build");
}
static void xe_blitNop1(id self, SEL _cmd, id r) { }                  // synchronizeResource: (memory is shared)

// ---- samplers ----
typedef struct { id dev, label; XeSampDescU d; } XeSamp;
static XeSamp *samp(id s) { return s ? *(XeSamp **)((char *)s + ivar_getOffset(gSampIvar)) : NULL; }
static uint8_t addrMode(NSUInteger m) {              // MTLSamplerAddressMode -> Gen TCM_*
    switch (m) {
    case 0: return 2;                                 // clampToEdge -> CLAMP
    case 1: return 5;                                 // mirrorClampToEdge -> MIRROR_ONCE
    case 2: return 0;                                 // repeat -> WRAP
    case 3: return 1;                                 // mirrorRepeat -> MIRROR
    default: return 4;                                // clampToZero / clampToBorderColor -> CLAMP_BORDER (black)
    }
}
static id xe_newSampler(id self, SEL _cmd, id desc) {
    XeSamp *x = calloc(1, sizeof(XeSamp));
    NSUInteger (*q)(id, SEL) = (NSUInteger (*)(id, SEL))objc_msgSend;
    x->d.used = 1;
    x->d.minFilter = q(desc, sel_registerName("minFilter")) ? 1 : 0;
    x->d.magFilter = q(desc, sel_registerName("magFilter")) ? 1 : 0;
    NSUInteger mip = q(desc, sel_registerName("mipFilter"));
    x->d.mipFilter = mip == 2 ? 3 : mip == 1 ? 1 : 0;
    x->d.addrU = addrMode(q(desc, sel_registerName("sAddressMode")));
    x->d.addrV = addrMode(q(desc, sel_registerName("tAddressMode")));
    x->d.addrW = addrMode(q(desc, sel_registerName("rAddressMode")));
    x->d.normalized = ((BOOL (*)(id, SEL))objc_msgSend)(desc, sel_registerName("normalizedCoordinates")) ? 1 : 0;
    x->d.pad[0] = (uint8_t)((q(desc, sel_registerName("compareFunction")) & 7) + 1);   // v0.37.0: sample_compare
    x->dev = [self retain];
    id s = [gSampClass alloc];
    *(XeSamp **)((char *)s + ivar_getOffset(gSampIvar)) = x;
    return s;
}
static void xe_sampDealloc(id self, SEL _cmd) {
    XeSamp *x = samp(self);
    if (x) { [x->dev release]; [x->label release]; free(x); }
    struct objc_super sup = { self, class_getSuperclass(gSampClass) };
    ((void (*)(struct objc_super *, SEL))objc_msgSendSuper)(&sup, _cmd);
}
static id xe_sampDevice(id s, SEL c) { return samp(s)->dev; }
// v0.30.1: a constexpr sampler (written in the shader), Metal's AIR encoding (first word):
// bits 2:0 / 5:3 / 8:6 s / t / r address (0 clamp to zero/border, 1 clamp to edge, 2 repeat,
// 3 mirrored repeat), 10:9 mag filter, 12:11 min filter (1 linear), 14:13 mip filter
// (1 nearest, 2 linear), 15 pixel coordinates (seen in irisxe-kernels.ll, v0.30.0)
static XeSampDescU constSampler(uint64_t w) {
    static const uint8_t addr[8] = { 4, 2, 0, 1, 5, 4, 4, 4 };    // -> Gen TCM_*
    XeSampDescU d; memset(&d, 0, sizeof(d));
    d.used = 1;
    d.addrU = addr[w & 7]; d.addrV = addr[(w >> 3) & 7]; d.addrW = addr[(w >> 6) & 7];
    d.magFilter = ((w >> 9) & 3) ? 1 : 0;
    d.minFilter = ((w >> 11) & 3) ? 1 : 0;
    unsigned mip = (w >> 13) & 3;
    d.mipFilter = mip == 2 ? 3 : mip == 1 ? 1 : 0;
    d.normalized = ((w >> 15) & 1) ? 0 : 1;
    return d;
}
static id xe_sampLabel(id s, SEL c)  { return samp(s)->label; }

// methods a texture doesn't have: log once, return 0 (like the device)
static const char *texProtoTypes(SEL sel) {
    const char *names[] = { "MTLTexture", "MTLResource", "MTLSamplerState", "MTLAllocation", "MTLBuffer" };
    for (int i = 0; i < 5; i++) {
        Protocol *p = objc_getProtocol(names[i]);
        if (!p) continue;
        for (int req = 1; req >= 0; req--) {
            struct objc_method_description d = protocol_getMethodDescription(p, sel, req, YES);
            if (d.types) return d.types;
        }
    }
    return NULL;
}
static NSMethodSignature *xe_texSig(id self, SEL _cmd, SEL sel) {
    struct objc_super sup = { self, [NSObject class] };
    NSMethodSignature *sig = ((id (*)(struct objc_super *, SEL, SEL))objc_msgSendSuper)(&sup, _cmd, sel);
    if (sig) return sig;
    const char *t = texProtoTypes(sel);
    return [NSMethodSignature signatureWithObjCTypes:t ? t : "@@:"];
}
static void xe_texForward(id self, SEL _cmd, NSInvocation *inv) {
    NSString *name = [NSString stringWithFormat:@"%s %@", class_getName(object_getClass(self)), NSStringFromSelector(inv.selector)];
    @synchronized (gMissing) {
        if (![gMissing containsObject:name]) {
            [gMissing addObject:name];
            os_log(gLog, "IrisXeMTL: MISSING -[%{public}@] -> returns 0", name);
            missingToFile([NSString stringWithFormat:@"MISSING -[%@]", name].UTF8String);
            if (!strcmp(getprogname(), "irisxe-metaltest") && !getenv("IRISXE_QUIET"))
                fprintf(stderr, "    MISSING method: %s -> 0\n", name.UTF8String);
        }
    }
    NSUInteger len = inv.methodSignature.methodReturnLength;
    if (len) { void *zero = calloc(1, len); [inv setReturnValue:zero]; free(zero); }
}
static void addT(Class c, const char *sel, IMP imp, const char *fallback) {
    const char *t = texProtoTypes(sel_registerName(sel));
    if (!class_addMethod(c, sel_registerName(sel), imp, t ? t : fallback))
        os_log_error(gLog, "IrisXeMTL: could not add %{public}s", sel);
}
static void registerTextureClasses(void) {
    gTexClass = objc_allocateClassPair([NSObject class], "IrisXeMTLTexture", 0);
    if (gTexClass) {
        class_addIvar(gTexClass, "_xe", sizeof(void *), 3, "^v");
        addT(gTexClass, "dealloc", (IMP)xe_texDealloc, "v16@0:8");
        addT(gTexClass, "width", (IMP)xe_texWidth, "Q16@0:8");
        addT(gTexClass, "height", (IMP)xe_texHeight, "Q16@0:8");
        addT(gTexClass, "depth", (IMP)xe_texDepth, "Q16@0:8");
        addT(gTexClass, "arrayLength", (IMP)xe_texLayers, "Q16@0:8");
        addT(gTexClass, "mipmapLevelCount", (IMP)xe_texLevels, "Q16@0:8");
        addT(gTexClass, "iosurface", (IMP)xe_texIOS, "^{__IOSurface=}16@0:8");           // v0.38.0
        addT(gTexClass, "iosurfacePlane", (IMP)xe_texIOSPlane, "Q16@0:8");
        addT(gTexClass, "sampleCount", (IMP)xe_texSamples, "Q16@0:8");
        addT(gTexClass, "textureType", (IMP)xe_texType, "Q16@0:8");
        addT(gTexClass, "pixelFormat", (IMP)xe_texFormat, "Q16@0:8");
        addT(gTexClass, "usage", (IMP)xe_texUsage, "Q16@0:8");
        addT(gTexClass, "storageMode", (IMP)xe_texStorage, "Q16@0:8");
        addT(gTexClass, "newTextureViewWithPixelFormat:", (IMP)xe_texViewPF, "@24@0:8Q16");                     // v0.68.24
        addT(gTexClass, "newTextureViewWithPixelFormat:textureType:levels:slices:", (IMP)xe_texViewFull,
             "@64@0:8Q16Q24{_NSRange=QQ}32{_NSRange=QQ}48");
        addT(gTexClass, "heap", (IMP)xe_texParent, "@16@0:8");
        addT(gTexClass, "cpuCacheMode", (IMP)xe_texZero, "Q16@0:8");
        addT(gTexClass, "hazardTrackingMode", (IMP)xe_texZero, "Q16@0:8");
        addT(gTexClass, "allocatedSize", (IMP)xe_texSize, "Q16@0:8");
        addT(gTexClass, "device", (IMP)xe_texDevice, "@16@0:8");
        addT(gTexClass, "label", (IMP)xe_texLabel, "@16@0:8");
        addT(gTexClass, "setLabel:", (IMP)xe_texSetLabel, "v24@0:8@16");
        addT(gTexClass, "isFramebufferOnly", (IMP)xe_texNo, "c16@0:8");
        addT(gTexClass, "isShareable", (IMP)xe_texNo, "c16@0:8");
        addT(gTexClass, "parentTexture", (IMP)xe_texNil, "@16@0:8");
        addT(gTexClass, "buffer", (IMP)xe_texBuffer, "@16@0:8");               // v0.51.1
        addT(gTexClass, "bufferOffset", (IMP)xe_texBufferOffset, "Q16@0:8");
        addT(gTexClass, "bufferBytesPerRow", (IMP)xe_texBufferBPR, "Q16@0:8");
        addT(gTexClass, "replaceRegion:mipmapLevel:withBytes:bytesPerRow:", (IMP)xe_texReplace,
             "v88@0:8{?={?=QQQ}{?=QQQ}}16Q64r^v72Q80");
        addT(gTexClass, "replaceRegion:mipmapLevel:slice:withBytes:bytesPerRow:bytesPerImage:", (IMP)xe_texReplaceSlice,
             "v104@0:8{?={?=QQQ}{?=QQQ}}16Q64Q72r^v80Q88Q96");
        addT(gTexClass, "getBytes:bytesPerRow:fromRegion:mipmapLevel:", (IMP)xe_texGetBytes,
             "v88@0:8^v16Q24{?={?=QQQ}{?=QQQ}}32Q80");
        addT(gTexClass, "getBytes:bytesPerRow:bytesPerImage:fromRegion:mipmapLevel:slice:", (IMP)xe_texGetBytesSlice,
             "v104@0:8^v16Q24Q32{?={?=QQQ}{?=QQQ}}40Q88Q96");
        addT(gTexClass, "methodSignatureForSelector:", (IMP)xe_texSig, "@24@0:8:16");
        addT(gTexClass, "forwardInvocation:", (IMP)xe_texForward, "v24@0:8@16");
        class_addProtocol(gTexClass, @protocol(MTLTexture));
        objc_registerClassPair(gTexClass);
        gTexIvar = class_getInstanceVariable(gTexClass, "_xe");
    }
    gSampClass = objc_allocateClassPair([NSObject class], "IrisXeMTLSamplerState", 0);
    if (gSampClass) {
        class_addIvar(gSampClass, "_xe", sizeof(void *), 3, "^v");
        addT(gSampClass, "dealloc", (IMP)xe_sampDealloc, "v16@0:8");
        addT(gSampClass, "device", (IMP)xe_sampDevice, "@16@0:8");
        addT(gSampClass, "label", (IMP)xe_sampLabel, "@16@0:8");
        addT(gSampClass, "methodSignatureForSelector:", (IMP)xe_texSig, "@24@0:8:16");
        addT(gSampClass, "forwardInvocation:", (IMP)xe_texForward, "v24@0:8@16");
        class_addProtocol(gSampClass, @protocol(MTLSamplerState));
        objc_registerClassPair(gSampClass);
        gSampIvar = class_getInstanceVariable(gSampClass, "_xe");
    }
}

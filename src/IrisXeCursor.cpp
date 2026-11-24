// IrisXeCursor - hardware cursor for Tiger Lake (cursor plane of pipe A)
//
// macOS hands us the cursor image through setCursorImage(); we convert it
// with IOFramebuffer::convertCursorImage() into a 256x256 ARGB image in our
// own memory, map that memory into the GPU address space (GGTT) and point the
// cursor plane at it. Moving the mouse is then just one register write
// (CURPOS) instead of WindowServer redrawing the pointer into the screen.
//
// Register sources: Linux i915 intel_cursor_regs.h / skl_watermark.c.
// Opt-in for now: boot-arg -irisxehwcursor.

#include "IrisXeFB.hpp"
#include <string.h>
#include <libkern/libkern.h>

// cursor plane A
#define CURCNTR_A        0x70080
#define CURBASE_A        0x70084
#define CURPOS_A         0x70088
#define CUR_FBC_CTL_A    0x700A0
#define CUR_WM_A_0       0x70140      // +4 per level; 0x70158/5C = SAGV on gen12
#define CUR_WM_TRANS_A   0x70168
#define CUR_BUF_CFG_A    0x7017C

// plane 1A (the desktop)
#define PLANE_WM_1_A_0   0x70240
#define PLANE_WM_TRANS_1 0x70268
#define PLANE_BUF_CFG_1  0x7027C
#define PLANE_SURF_1     0x7019C
#define PLANE_COLOR_1    0x701CC

#define MCURSOR_MODE_256_ARGB_AX  0x23
#define MCURSOR_ARB_SLOTS_1       (1u << 28)     // icl+
#define MCURSOR_PIPE_GAMMA_ENABLE (1u << 26)
#define MCURSOR_PIPE_CSC_ENABLE   (1u << 24)
#define PLANE_COLOR_PIPE_GAMMA    (1u << 30)
#define PLANE_COLOR_PIPE_CSC      (1u << 23)

#define WM_EN            (1u << 31)
#define WM_LINES(v)      (((v) >> 14) & 0x1FFF)
#define WM_BLOCKS(v)     ((v) & 0x1FFF)

#define GGTT_BASE        0x800000          // GGTT entries live at BAR0 + 8 MB
#define GFX_FLSH_CNTL    0x101008

#define CUR_DIM          256
#define CUR_BYTES        (CUR_DIM * CUR_DIM * 4)    // 256 KB = 64 pages
#define CUR_PAGES        (CUR_BYTES / 4096)
#define CUR_GGTT_ADDR    0xF0000000u                // far above the boot framebuffer
#define CUR_DDB_BLOCKS   32                         // what i915 gives a single-pipe cursor

#define kIrisXeHWCursorSupported 0x00000001         // kIOFBHWCursorSupported (IOGraphics priv.h)

bool IrisXeFB::cursorInit() {
    if (!mmio || !mmioMap || !pci) return false;

    // GGTT size from the GMCH control register (GGMS, bits 7:6): 2/4/8 MB of
    // 8-byte entries. Put the cursor near the top of that address range.
    UInt16 ggc = pci->configRead16(0x50);
    UInt32 ggms = (ggc >> 6) & 3;
    if (!ggms) { setProperty("IrisXe-HWCursor", "no GGTT"); return false; }
    UInt64 ggttSpan = (UInt64)((1u << ggms) * 1024 * 1024 / 8) * 4096;   // bytes of GPU address space
    UInt64 want = ggttSpan - 4ULL * 1024 * 1024;
    curGGTT = (UInt32)(want < CUR_GGTT_ADDR ? want : CUR_GGTT_ADDR);
    UInt32 pteOff = GGTT_BASE + (curGGTT >> 12) * 8;
    if (mmioMap->getLength() < pteOff + CUR_PAGES * 8) {
        setProperty("IrisXe-HWCursor", "BAR too small for GGTT entry");
        return false;
    }

    // physically contiguous, 4K aligned, write-combined (the display engine does not snoop the CPU cache)
    curMem = IOBufferMemoryDescriptor::withOptions(
        kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMapWriteCombineCache,
        CUR_BYTES, 4096);
    if (!curMem) { setProperty("IrisXe-HWCursor", "no memory"); return false; }
    curVA = (volatile UInt32 *)curMem->getBytesNoCopy();
    IOByteCount segLen = 0;
    curPhys = curMem->getPhysicalSegment(0, &segLen, kIOMemoryMapperNone);
    if (!curVA || !curPhys || segLen < CUR_BYTES) {
        OSSafeReleaseNULL(curMem); curVA = nullptr;
        setProperty("IrisXe-HWCursor", "memory not contiguous");
        return false;
    }
    for (UInt32 i = 0; i < CUR_DIM * CUR_DIM; i++) curVA[i] = 0;

    curStage = (UInt32 *)IOMalloc(CUR_BYTES);
    if (!curStage) {
        OSSafeReleaseNULL(curMem); curVA = nullptr;
        setProperty("IrisXe-HWCursor", "no staging memory");
        return false;
    }

    // Display buffer (DDB) for the cursor: keep the firmware's if it gave the
    // cursor one, otherwise take the last 32 blocks of plane 1's share.
    bootPlaneBuf = readReg(PLANE_BUF_CFG_1);
    bootCurBuf   = readReg(CUR_BUF_CFG_A);
    UInt32 ps = bootPlaneBuf & 0x1FFF, pe = (bootPlaneBuf >> 16) & 0x1FFF;
    UInt32 cs = bootCurBuf & 0x1FFF,   ce = (bootCurBuf >> 16) & 0x1FFF;
    bool curOK = bootCurBuf && ce > cs && (ce < ps || cs > pe);
    if (curOK) {
        curBuf = bootCurBuf; planeBuf = bootPlaneBuf;
    } else if (pe > ps + 2 * CUR_DDB_BLOCKS) {
        UInt32 newEnd = pe - CUR_DDB_BLOCKS;
        planeBuf = ps | (newEnd << 16);
        curBuf   = (newEnd + 1) | (pe << 16);
    } else {
        IOFree(curStage, CUR_BYTES); curStage = nullptr;
        OSSafeReleaseNULL(curMem); curVA = nullptr;
        setProperty("IrisXe-HWCursor", "no display buffer space");
        return false;
    }

    curReady = true;
    cursorHWSetup();

    char note[160];
    snprintf(note, sizeof(note), "ready ggms=%u ggtt=%08x phys=%llx plane=%08x>%08x cur=%08x>%08x",
             ggms, curGGTT, curPhys, bootPlaneBuf, planeBuf, bootCurBuf, curBuf);
    setProperty("IrisXe-HWCursor", note);
    return true;
}

// Cursor watermark derived from the desktop plane's watermark at the same
// level: same latency in lines, but the cursor only fetches 1 KB per line.
UInt32 IrisXeFB::cursorWMFromPlane(UInt32 planeWM, bool level0) const {
    if (!(planeWM & WM_EN)) return level0 ? (WM_EN | (2u << 14) | 8u) : 0;
    UInt32 lines  = WM_LINES(planeWM);
    UInt32 blocks = (lines + 1) * 2 + 1;               // 2 blocks per 1 KB line
    if (blocks >= CUR_DDB_BLOCKS) {
        if (!level0) return 0;                          // can't hold it: level off
        blocks = CUR_DDB_BLOCKS - 1;
    }
    return WM_EN | ((lines & 0x1FFF) << 14) | blocks;
}

// Everything that is lost after system sleep: GGTT entries, DDB split, WMs.
void IrisXeFB::cursorHWSetup() {
    if (!curReady || !mmio) return;

    // 1. GGTT: map our 64 pages at CUR_GGTT_ADDR (same flag bits as the boot page)
    UInt32 flags = (bootPTE0[0] & 0xFFF) ? (bootPTE0[0] & 0xFFF) : 0x1;
    UInt32 idx = curGGTT >> 12;
    for (UInt32 i = 0; i < CUR_PAGES; i++) {
        UInt64 pte = (curPhys + (UInt64)i * 4096) | flags;
        writeReg(GGTT_BASE + (idx + i) * 8,     (UInt32)pte);
        writeReg(GGTT_BASE + (idx + i) * 8 + 4, (UInt32)(pte >> 32));
    }
    (void)readReg(GGTT_BASE + idx * 8);
    writeReg(GFX_FLSH_CNTL, 1);                         // flush GGTT writes
    (void)readReg(GFX_FLSH_CNTL);

    // 2. display buffer: shrink plane 1, give the cursor its blocks.
    //    Plane 1 watermarks must fit the smaller share.
    UInt32 planeBlocks = ((planeBuf >> 16) & 0x1FFF) - (planeBuf & 0x1FFF) + 1;
    for (UInt32 k = 0; k < 8; k++) {
        UInt32 w = readReg(PLANE_WM_1_A_0 + k * 4);
        if ((w & WM_EN) && WM_BLOCKS(w) >= planeBlocks)
            writeReg(PLANE_WM_1_A_0 + k * 4, k == 0 ? ((w & ~0x1FFFu) | (planeBlocks - 1)) : 0);
    }
    {
        UInt32 w = readReg(PLANE_WM_TRANS_1);
        if ((w & WM_EN) && WM_BLOCKS(w) >= planeBlocks) writeReg(PLANE_WM_TRANS_1, 0);
    }
    writeReg(PLANE_BUF_CFG_1, planeBuf);
    writeReg(PLANE_SURF_1, readReg(PLANE_SURF_1));      // arms plane 1 changes

    // 3. cursor watermarks + buffer (armed by the CURBASE write in cursorUpdate)
    for (UInt32 k = 0; k < 8; k++)
        writeReg(CUR_WM_A_0 + k * 4, cursorWMFromPlane(readReg(PLANE_WM_1_A_0 + k * 4), k == 0));
    writeReg(CUR_WM_TRANS_A, cursorWMFromPlane(readReg(PLANE_WM_TRANS_1), false));
    writeReg(CUR_BUF_CFG_A, curBuf);
    writeReg(CUR_FBC_CTL_A, 0);

    cursorUpdate();
}

void IrisXeFB::cursorUpdate() {
    if (!curReady || !mmio || powerState != 2) return;
    UInt32 ctl = 0;
    if (curVisible && curHaveImage) {
        UInt32 pc = readReg(PLANE_COLOR_1);
        ctl = MCURSOR_MODE_256_ARGB_AX | MCURSOR_ARB_SLOTS_1;
        if (pc & PLANE_COLOR_PIPE_GAMMA) ctl |= MCURSOR_PIPE_GAMMA_ENABLE;   // same colour path as desktop
        if (pc & PLANE_COLOR_PIPE_CSC)   ctl |= MCURSOR_PIPE_CSC_ENABLE;
    }
    UInt32 pos = 0;
    SInt32 x = curX, y = curY;
    if (x < 0) { pos |= (1u << 15); x = -x; }
    if (y < 0) { pos |= (1u << 31); y = -y; }
    pos |= ((UInt32)x & 0x7FFF) | (((UInt32)y & 0x7FFF) << 16);

    writeReg(CURCNTR_A, ctl);
    writeReg(CURPOS_A, pos);
    writeReg(CURBASE_A, curGGTT);                       // arms CNTR/POS/WM/BUF
}

void IrisXeFB::cursorShutdown() {
    if (!curReady) return;
    curReady = false;
    if (mmio) {
        writeReg(CURCNTR_A, 0);
        writeReg(CURBASE_A, 0);
        writeReg(CUR_BUF_CFG_A, bootCurBuf);
        writeReg(PLANE_BUF_CFG_1, bootPlaneBuf);
        writeReg(PLANE_SURF_1, readReg(PLANE_SURF_1));
    }
    if (curStage) { IOFree(curStage, CUR_BYTES); curStage = nullptr; }
    OSSafeReleaseNULL(curMem);
    curVA = nullptr;
}

IOReturn IrisXeFB::getAttribute(IOSelect attribute, uintptr_t *value) {
    if (attribute == kIOHardwareCursorAttribute) {
        if (!curReady) return kIOReturnUnsupported;
        if (value) *value = kIrisXeHWCursorSupported;
        return kIOReturnSuccess;
    }
    return super::getAttribute(attribute, value);
}

IOReturn IrisXeFB::setCursorImage(void *cursorImage) {
    IOHardwareCursorDescriptor desc;
    bzero(&desc, sizeof(desc));
    desc.majorVersion = kHardwareCursorDescriptorMajorVersion;
    desc.minorVersion = kHardwareCursorDescriptorMinorVersion;
    desc.width  = CUR_DIM;
    desc.height = CUR_DIM;
    desc.bitDepth = 32;
    desc.supportedSpecialEncodings = kTransparentEncodedPixel;

    IOHardwareCursorInfo info;
    bzero(&info, sizeof(info));
    info.majorVersion = kHardwareCursorInfoMajorVersion;
    info.minorVersion = kHardwareCursorInfoMinorVersion;
    info.hardwareCursorData = (UInt8 *)curStage;

    // also used by IOFramebuffer to learn our cursor format (returns false then)
    if (!convertCursorImage(cursorImage, &desc, &info) || !curReady || !curVA) {
        if (curReady && curHaveImage) { curHaveImage = false; cursorUpdate(); }
        return kIOReturnUnsupported;
    }

    UInt32 w = info.cursorWidth, h = info.cursorHeight;
    if (w > CUR_DIM) w = CUR_DIM;
    if (h > CUR_DIM) h = CUR_DIM;
    UInt32 clearH = (curLastH > h) ? curLastH : h;
    for (UInt32 row = 0; row < clearH; row++) {
        volatile UInt32 *dst = curVA + row * CUR_DIM;
        const UInt32 *src = curStage + row * info.cursorWidth;
        UInt32 col = 0;
        if (row < h) {
            for (; col < w; col++) {
                // convertCursorImage gives straight alpha; the plane wants premultiplied
                UInt32 p = src[col], a = p >> 24;
                if (a == 0)        dst[col] = 0;
                else if (a == 255) dst[col] = p;
                else {
                    UInt32 r = (((p >> 16) & 0xFF) * a + 127) / 255;
                    UInt32 g = (((p >> 8)  & 0xFF) * a + 127) / 255;
                    UInt32 b = (( p        & 0xFF) * a + 127) / 255;
                    dst[col] = (a << 24) | (r << 16) | (g << 8) | b;
                }
            }
        }
        for (; col < CUR_DIM; col++) dst[col] = 0;
    }
    curLastH = h;
    __asm__ volatile("sfence" ::: "memory");          // flush write-combining buffers

    curHaveImage = true;
    cursorUpdate();

    static UInt32 images = 0;
    if (++images <= 40) {                               // diagnostics: first images only
        char note[48];
        snprintf(note, sizeof(note), "%ux%u hot %u,%u #%u", w, h,
                 info.cursorHotSpotX, info.cursorHotSpotY, images);
        setProperty("IrisXe-HWCursorImage", note);
    }
    return kIOReturnSuccess;
}

IOReturn IrisXeFB::setCursorState(SInt32 x, SInt32 y, bool visible) {
    if (!curReady) return kIOReturnUnsupported;
    curX = x; curY = y; curVisible = visible;
    cursorUpdate();
    return kIOReturnSuccess;
}

// IrisXeVideo.cpp - v0.68.0: hardware video decoding, step V2b: a record-only video context (see IrisXeVideo.hpp).
#include "IrisXeVideo.hpp"
#include "IrisXeAccel.hpp"
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <libkern/libkern.h>
#include <sys/errno.h>
#include "apple/IOAccelEventMachine2.h"
#include "../IrisXeSpaceABI.h"

OSDefineMetaClassAndStructors(IrisXeVideoContext, IOAccelVideoContext2)
// the family's IOAccelContext2::getTargetAndMethodForIndex (exported; our generated header lists only a class's own
// slots, and this one is an IOUserClient slot the family overrides)
extern "C" IOExternalMethod *_ZN15IOAccelContext226getTargetAndMethodForIndexEPP9IOServicej(IOAccelContext2 *, IOService **, unsigned int);

// ---------------------------------------------------------------- test switch (one boot)
static bool sVideoTest;
static void videoPublish();             // v0.69.0 (below)
static IrisXeAccelerator *sVideoAccel;
bool IrisXeVideoTestOn() { return sVideoTest; }
// v0.68.5: populateAccelConfig runs inside the family's start, before IrisXeVideoTestStart reads (and erases) the
// switch: it only peeks. On the test boot every resource gets Apple's Intel RO private part (0x28 bytes), which the VA
// driver reads (IOAccelResourceGetClientSharedPrivate) - NULL without it (VTDecoderXPCService crash, v0.68.4).
static UInt32 sROPriv;
UInt32 IrisXeROPrivateSize() { return sROPriv; }
bool IrisXeVideoTestPending() {
    bool on = PE_parse_boot_argn("-irisxevideotest", nullptr, 0);
    IORegistryEntry *opt = IORegistryEntry::fromPath("/options", gIODTPlane);
    if (opt) { if (opt->getProperty("irisxe-videotest")) on = true; opt->release(); }
    return on;
}
void IrisXeVideoSetROPrivate(UInt32 n) { sROPriv = n; }

void IrisXeVideoTestStart(IrisXeAccelerator *accel) {
    sVideoAccel = accel;
    IORegistryEntry *opt = IORegistryEntry::fromPath("/options", gIODTPlane);
    if (opt) {
        if (opt->getProperty("irisxe-videotest")) {
            sVideoTest = true;
            opt->removeProperty("irisxe-videotest");
            opt->setProperty("IONVRAM-FORCESYNCNOW-PROPERTY", "IONVRAM-FORCESYNCNOW-PROPERTY");   // erased before anything runs
        }
        opt->release();
    }
    if (PE_parse_boot_argn("-irisxevideotest", nullptr, 0)) sVideoTest = true;
    if (!sVideoTest) {
        accel->setProperty("IrisXe-Video", "off (one-boot test: sudo nvram irisxe-videotest=1, restart)");
        return;
    }
    // what Apple's Ice Lake accelerator announces (research round 33), H.264 only for now: AppleGVA loads the VA
    // driver named by IODVDBundleName. HEVC / VP9 (IOGVAHEVCDecode / IOGVAVP9Decode) need Gen12 command changes.
    accel->setProperty("IODVDBundleName", "AppleIntelICLGraphicsVADriver");
    accel->setProperty("IOGVACodec", "Gen11");
    accel->setProperty("IOGVAXDecode", "2");
    accel->setProperty("IOVARendererID", 17301632ULL, 32);
    accel->setProperty("IrisXe-Video", "video test boot: VideoToolbox uses our decoder. sudo sysctl debug.irisxe_video=N: "
                                       "0 record only (nothing on the GPU), 1 self-test, 2 decode on VCS0 + record, 3 decode "
                                       "(IrisXe-VideoDecode, IrisXe-VideoEngine)");
    videoPublish();
}

// ---------------------------------------------------------------- the record
static IOLock *sRecLock;
static char sRec[65536];
static UInt32 sRecLen, sTokens, sCtxTypes[4], sTokCount[16][8];
static UInt32 sDumps;

static void recAdd(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void recAdd(const char *fmt, ...) {
    if (sRecLen + 2 >= sizeof(sRec)) return;
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(sRec + sRecLen, sizeof(sRec) - sRecLen, fmt, ap);
    va_end(ap);
    if (n > 0) { sRecLen += (UInt32)n; if (sRecLen > sizeof(sRec) - 1) sRecLen = sizeof(sRec) - 1; }
}
static void xeCountsText(char *b, UInt32 len);   // v0.68.6
static void recLock() { if (!sRecLock) sRecLock = IOLockAlloc(); if (sRecLock) IOLockLock(sRecLock); }
static void recUnlock() { if (sRecLock) IOLockUnlock(sRecLock); }

void IrisXeVideoRecordNote(const char *what, UInt32 type) {
    recLock();
    if (type >= 0x100 && type <= 0x102) sCtxTypes[type - 0x100]++; else sCtxTypes[3]++;
    recAdd("[%s 0x%x] ", what, type);
    recUnlock();
    IrisXeVideoRecordPublish(sVideoAccel);
}
void IrisXeVideoRecordPublish(IrisXeAccelerator *accel) {
    if (!accel) return;
    char head[600];
    UInt32 o = (UInt32)snprintf(head, sizeof(head), "contexts asked: main %u, media %u, vebox %u, other %u | tokens %u | ",
                                sCtxTypes[0], sCtxTypes[1], sCtxTypes[2], sCtxTypes[3], sTokens);
    for (int g = 0; g < 16 && o + 24 < sizeof(head); g++)
        for (int k = 0; k < 8 && o + 24 < sizeof(head); k++)
            if (sTokCount[g][k]) o += (UInt32)snprintf(head + o, sizeof(head) - o, "%02x%02x x%u ", 0x80 + g, k, sTokCount[g][k]);
    char counts[900] = {0};
    xeCountsText(counts, sizeof(counts));
    OSString *s3 = OSString::withCString(counts);
    if (s3) { accel->setProperty("IrisXe-VideoCounts", s3); s3->release(); }
    // v0.68.10: the record in 2 KB parts (IrisXe-VideoRec0..7): one 16 KB string made ioreg drop the property
    static char part[2049];
    recLock();
    OSString *s1 = OSString::withCString(head);
    size_t len = strnlen(sRec, sizeof(sRec));
    for (int k = 0; k < 32; k++) {
        size_t from = (size_t)k * 2048;
        if (from >= len) break;
        size_t n = len - from < 2048 ? len - from : 2048;
        memcpy(part, sRec + from, n); part[n] = 0;
        for (size_t j = 0; j < n; j++) { if (part[j] == '"') part[j] = '\''; if (part[j] == '\n') part[j] = ' '; }   // grep reads one line
        char key[24]; snprintf(key, sizeof(key), "IrisXe-VideoRec%d", k);
        OSString *sp = OSString::withCString(part);
        if (sp) { accel->setProperty(key, sp); sp->release(); }
    }
    recUnlock();
    if (s1) { accel->setProperty("IrisXe-VideoSummary", s1); s1->release(); }
    accel->removeProperty("IrisXe-VideoRecord");
}

// IOAccelCommandStreamInfo (IOAccelContext2 + 0x650, read from IOAccelContext2::processSidebandBuffer, round 36):
//   +0x00 command pointer for this token (base + offset * 4)   +0x08 command buffer base   +0x10 its size (bytes)
//   +0x18 token pointer (sideband buffer)   +0x20 u16 id, +0x22 u16 size (dwords), +0x24 u32 command offset (dwords)
//   +0x28 the token's data (token + 8)     +0x34 command offset (dwords)   +0x3c ok flag (0 = context error)
static void recordToken(const UInt8 *info, UInt32 ctype) {
    UInt16 id = *(const UInt16 *)(info + 0x20), sz = *(const UInt16 *)(info + 0x22);
    UInt32 off = *(const UInt32 *)(info + 0x24);
    const UInt32 *tok = *(const UInt32 * const *)(info + 0x18);
    const UInt32 *cmd = *(const UInt32 * const *)(info + 0x00);
    const UInt8 *base = *(const UInt8 * const *)(info + 0x08);
    UInt64 cmdSize = *(const UInt64 *)(info + 0x10);
    recLock();
    sTokens++;
    UInt32 g = (id >> 8) - 0x80, k = id & 0xFF;
    if (g < 16 && k < 8) sTokCount[g][k]++;
    // the first 3 tokens of each kind are dumped (header, data, and the command dwords they point at)
    bool dump = g < 16 && k < 8 && sTokCount[g][k] <= 3 && sDumps < 40;
    if (dump) {
        sDumps++;
        recAdd("\n#%u ctx 0x%x token 0x%04x size %u off 0x%x cmdbase %p size 0x%llx:", sTokens, ctype, id, sz, off, base, cmdSize);
        if (tok) for (UInt32 i = 0; i < sz && i < 24; i++) recAdd(" %08x", tok[i]);
        if (cmd && base && (const UInt8 *)cmd >= base && (const UInt8 *)cmd + 4 * 32 <= base + cmdSize) {
            recAdd(" | cmd:");
            for (UInt32 i = 0; i < 32; i++) recAdd(" %08x", cmd[i]);
        }
    }
    recUnlock();
}

// v0.68.17 (defined below)
struct XeSurf;
#define XE_SURF_N     31          // v0.69.0: slot 31 (0x1F800000) = the video batch buffer
static void surfForget(IOAccelResource2 *r);
// ---------------------------------------------------------------- v0.68.15: texture ids -> resources
// RO private +8 holds each resource's texture id (1..4095; the VA driver may pick its own for its surfaces, so the
// same id can exist in several processes): up to 4 resources per id, told apart by their process.
extern "C" void *_ZNK16IOAccelResource210getGPUTaskEv(const IOAccelResource2 *);
UInt64 irisxeFindMemoryVA(IOAccelTask *task, const void *mem, UInt64 *lenOut);   // IrisXeAccelMachines.cpp
struct XeIdSlot { IOAccelResource2 *r[4]; };
static XeIdSlot sIds[4096];
static IOLock *sIdLock;
static UInt32 sIdNext[4096 / 32];
void IrisXeVideoIdSet(UInt32 id, IOAccelResource2 *r) {
    if (!sVideoTest || !id || id >= 4096 || !r) return;
    if (!sIdLock) { IOLock *l = IOLockAlloc(); if (l && !OSCompareAndSwapPtr(nullptr, l, &sIdLock)) IOLockFree(l); }
    if (!sIdLock) return;
    IOLockLock(sIdLock);
    XeIdSlot &s = sIds[id];
    int k = 0;
    while (k < 4 && s.r[k] && s.r[k] != r) k++;
    if (k == 4) { k = (int)(sIdNext[id >> 5]++ & 3); }   // full: replace one (old ones are likeliest gone)
    s.r[k] = r;
    IOLockUnlock(sIdLock);
}
void IrisXeVideoIdForget(IOAccelResource2 *r) {
    surfForget(r);                                           // v0.68.17
    if (!sIdLock || !r) return;
    IOLockLock(sIdLock);
    for (int i = 1; i < 4096; i++) for (int k = 0; k < 4; k++) if (sIds[i].r[k] == r) sIds[i].r[k] = nullptr;
    IOLockUnlock(sIdLock);
}
// the resource with this id in process `task`, and its GPU address there (0 = not mapped yet)
static IOAccelResource2 *idLookup(UInt32 id, void *task, UInt64 *va, UInt64 *len) {
    IOAccelResource2 *found = nullptr;
    if (va) *va = 0;
    if (len) *len = 0;
    if (!sIdLock || !id || id >= 4096) return nullptr;
    IOLockLock(sIdLock);
    for (int k = 0; k < 4; k++) {
        IOAccelResource2 *r = sIds[id].r[k];
        if (r && _ZNK16IOAccelResource210getGPUTaskEv(r) == task) { found = r; break; }
    }
    if (found) {
        const UInt8 *me = (const UInt8 *)found;
        const void *mem = *(void * const *)(me + 0x88);
        if (!mem) mem = *(void * const *)(me + 0x80);      // IGAccelVideoContext::patch_codechal_command_buffer
        if (va) *va = irisxeFindMemoryVA((IOAccelTask *)task, mem, len);
    }
    IOLockUnlock(sIdLock);
    return found;
}

// ---------------------------------------------------------------- v0.68.17: picture buffers in the GPU space
// The VA driver's picture buffers are IOSurface resources (type 0x82) it never maps for the GPU itself (Apple's
// kernel driver maps them when a job binds them). We map each IOSurface once into the process's GPU space, in
// 256..512 MB (unused there while processes get their addresses in 1..4 GB), 32 slots of 8 MB.
#include "IrisXeDisplayPipe.hpp"          // kernel IOSurface getters
extern "C" uint64_t _ZNK9IOSurface14getPlaneOffsetEj(const IOSurface *, unsigned int);
int irisxeTaskSpace(IrisXeAccelerator *ax, IOAccelTask *task);
#define XE_SURF_BASE  0x10000000ULL
#define XE_SURF_SLOT  0x00800000ULL
struct XeSurf { IOAccelResource2 *owner; IOSurface *surf; void *task; int space; UInt64 va; UInt32 pages; IOMemoryDescriptor *md; };
static XeSurf sSurf[XE_SURF_N];
static UInt32 sSurfStat[4];               // mapped, reused, failed, unmapped
static void surfUnmap(XeSurf &e) {
    if (!e.surf) return;
    if (sVideoAccel && e.space >= 0 && e.pages) sVideoAccel->coreUnmapPages((UInt32)e.space, e.va, e.pages);
    if (e.md) { e.md->complete(); e.md->release(); }
    e = XeSurf{};
    sSurfStat[3]++;
}
static void surfForget(IOAccelResource2 *r) {
    for (int i = 0; r && i < XE_SURF_N; i++) if (sSurf[i].owner == r) surfUnmap(sSurf[i]);
}
// GPU address of resource r (an IOSurface plane) in process `task`; *pitch = the surface's bytes per row
static UInt64 surfVA(IOAccelResource2 *r, void *task, UInt32 *pitch) {
    if (pitch) *pitch = 0;
    const UInt8 *me = (const UInt8 *)r;
    if (!r || me[0x14] != 0x82 || !sVideoAccel) return 0;
    // v0.69.2 (research round 50, IOAccelResource2::newResourceWithIOSurface / ...WithIOSurfaceDeviceCache, the same
    // on Sequoia and Tahoe): an IOSurface resource the VA driver uses is a VIEW of a storage resource:
    //   view +0xe0 = the storage resource (IOSurfaceDeviceCache +0x20), +0xf8 = plane, +0xfc = byte offset of that
    //   plane (IOSurface client-aligned offset + plane offset, computed by the family)
    //   storage: +0xe byte has 0x20 set, +0xe0 = the IOSurfaceDeviceCache, whose +0x10 = the IOSurface
    // v0.69.0 used view +0xe0 as the IOSurface, v0.69.1 (view +0xe0) +0x10: both kernel panics.
    auto kptr = [](const void *p) { return (UInt64)p >= 0xffffff8000000000ULL; };   // a kernel address at all
    const UInt8 *stor = nullptr;
    UInt32 poff = 0;
    if (me[0x0e] & 0x20) stor = me;                                  // the storage resource itself
    else {
        const UInt8 *v = *(const UInt8 * const *)(me + 0xe0);
        if (kptr(v) && (v[0x0e] & 0x20)) { stor = v; poff = *(const UInt32 *)(me + 0xfc); }
    }
    if (!stor) { sSurfStat[2]++; return 0; }
    const UInt8 *dc = *(const UInt8 * const *)(stor + 0xe0);
    IOSurface *sf = kptr(dc) ? *(IOSurface * const *)(dc + 0x10) : nullptr;
    if (!kptr(sf)) { sSurfStat[2]++; return 0; }
    if (pitch) *pitch = (UInt32)sf->getBytesPerRow();
    int freeSlot = -1;
    for (int i = 0; i < XE_SURF_N; i++) {
        if (sSurf[i].surf == sf && sSurf[i].task == task) { sSurfStat[1]++; return sSurf[i].va + poff; }
        if (!sSurf[i].surf && freeSlot < 0) freeSlot = i;
    }
    if (freeSlot < 0) { sSurfStat[2]++; return 0; }
    int space = irisxeTaskSpace(sVideoAccel, (IOAccelTask *)task);
    IOMemoryDescriptor *md = sf->getMemoryDescriptor();
    UInt64 len = md ? md->getLength() : 0;
    if (space < 0 || !md || !len || len > XE_SURF_SLOT || md->prepare() != kIOReturnSuccess) { sSurfStat[2]++; return 0; }
    md->retain();
    XeSurf &e = sSurf[freeSlot];
    e = XeSurf{ r, sf, task, space, XE_SURF_BASE + (UInt64)freeSlot * XE_SURF_SLOT, (UInt32)((len + 4095) / 4096), md };
    UInt64 pa[64];
    for (UInt32 done = 0; done < e.pages; ) {
        UInt32 n = 0;
        while (n < 64 && done + n < e.pages) {
            IOByteCount seg = 0;
            addr64_t p = md->getPhysicalSegment((IOByteCount)(done + n) * 4096, &seg, kIOMemoryMapperNone);
            if (!p) break;
            pa[n++] = p & ~0xFFFULL;
        }
        if (!n || !sVideoAccel->coreMapPages((UInt32)space, e.va + (UInt64)done * 4096, pa, n)) {
            e.pages = done; surfUnmap(e); sSurfStat[2]++; return 0;
        }
        done += n;
    }
    sSurfStat[0]++;
    return e.va + poff;
}

// v0.68.15: dry run of Apple's CodecHAL patch token (IGAccelVideoContext::patch_codechal_command_buffer, round 47):
// token data = 0x18-byte header, then 0x14-byte entries until one without bit 0:
//   dw0: bit0 valid, bits 1..3 type (0/4/5 = 64-bit address, 1 = pitch-1, 2/4/5 = second resource, 3 = skip),
//        bit 4 flag, bits 5..24 command dword index, bits 25..29 shift, bit 30 shift right, bit 31 extra offset
//   +4 u16 resource id, +6 u16 second id, +8 value added, +0xc / +0x10 extra offset (rows / planes)
// Nothing is written: the record gets each entry and what its resource resolves to.
// v0.68.17: the ids in the patch list are the family's resource ids (the VA driver reads them from each resource's
// client-shared block, +0x100), looked up like process_token_BindDataBuffer does: IOAccelShared2::lookupResource on
// the context's shared object (+0x5a0). v0.68.15 looked in our own texture-id table and found none of them.
extern "C" bool _ZN14IOAccelShared214lookupResourceEjPPv(void *shared, unsigned int id, void **out);
static IOAccelResource2 *sharedLookup(void *shared, UInt32 id, void *task, UInt64 *va, UInt64 *len) {
    if (va) *va = 0;
    if (len) *len = 0;
    void *r = nullptr;
    if (!shared || !id || !_ZN14IOAccelShared214lookupResourceEjPPv(shared, id, &r) || !r) return nullptr;
    const UInt8 *me = (const UInt8 *)r;
    const void *mem = *(void * const *)(me + 0x88);
    if (!mem) mem = *(void * const *)(me + 0x80);
    if (va) *va = irisxeFindMemoryVA((IOAccelTask *)task, mem, len);
    return (IOAccelResource2 *)r;
}
// resolve one patch-list resource id: GPU address (+ pitch for picture buffers)
static IOAccelResource2 *resolveId(void *shared, UInt32 id, void *task, UInt64 *va, UInt32 *pitch) {
    UInt64 len = 0;
    if (pitch) *pitch = 0;
    IOAccelResource2 *r = sharedLookup(shared, id, task, va, &len);
    if (r && ((const UInt8 *)r)[0x14] == 0x82) *va = surfVA(r, task, pitch);   // v0.68.17: picture buffers
    return r;
}
// v0.68.17: the patch list applied to a COPY of the job's commands (nothing the GPU sees is changed yet); the record
// gets every dword that would change (index: old -> new) and the memory-attribute dwords after each address.
static UInt32 sPatched[4096];
static void dryRunCodecHAL(const UInt32 *tok, UInt32 sizeDw, void *task, void *shared, const UInt8 *info) {
    static UInt32 runs;
    if (!tok || sizeDw < 8 || runs >= 2) return;
    const UInt32 *cmd = *(const UInt32 * const *)(info + 0x00);
    const UInt8 *base = *(const UInt8 * const *)(info + 0x08);
    UInt64 cmdSize = *(const UInt64 *)(info + 0x10);
    if (!cmd || !base || (const UInt8 *)cmd < base) return;
    UInt32 cmdDw = (UInt32)((base + cmdSize - (const UInt8 *)cmd) / 4);
    if (cmdDw > 4096) cmdDw = 4096;
    const UInt32 *e = tok + 2 + 6, *end = tok + sizeDw;
    if (!(e[0] & 1)) return;                             // an empty list (fence-only job)
    runs++;
    memcpy(sPatched, cmd, cmdDw * 4);
    UInt32 n = 0, ok = 0, bad = 0;
    recLock();
    recAdd("\n#patch job %u (%u command dwords):", runs, cmdDw);
    recUnlock();
    for (; e + 5 <= end && (e[0] & 1); e += 5, n++) {
        UInt32 dw0 = e[0], type = (dw0 >> 1) & 7, idx = (dw0 >> 5) & 0xFFFFF, sh = (dw0 >> 25) & 31;
        UInt32 id1 = e[1] & 0xFFFF, id2 = e[1] >> 16, p1 = 0, p2 = 0;
        UInt64 va1 = 0, va2 = 0, v = 0;
        resolveId(shared, id1, task, &va1, &p1);
        bool good = va1 != 0;
        switch (type) {
            case 0: v = va1; break;                                   // address
            case 1: v = p1 ? p1 - 1 : 0; good = p1 != 0; break;       // pitch - 1
            case 2: resolveId(shared, id2, task, &va2, &p2);          // rows from plane 1 to plane 2
                    v = (va2 > va1 && p1) ? (va2 - va1) / p1 : 0; good = good && va2 && p1; break;
            default: good = false; break;
        }
        if (dw0 & 0x40000000) v >>= sh; else v <<= sh;
        if ((dw0 & 0x80000000) && (e[3] | e[4])) good = false;        // extra row / plane offsets: not seen yet
        if (!good || idx + (type == 0 ? 1 : 0) >= cmdDw) { bad++; continue; }
        UInt64 full = v + e[2];
        UInt32 old0 = sPatched[idx];
        sPatched[idx] = (UInt32)(e[2] + (UInt32)v);
        recLock();
        recAdd(" %u:%x>%x", idx, old0, sPatched[idx]);
        if (type == 0) {
            UInt32 old1 = sPatched[idx + 1];
            sPatched[idx + 1] = (UInt32)(full >> 32);
            recAdd("/%x>%x", old1, sPatched[idx + 1]);
            if (idx + 2 < cmdDw) recAdd(" a%x", sPatched[idx + 2]);    // the memory attributes after it
        }
        recUnlock();
        ok++;
    }
    recLock();
    recAdd("\n#patch entries %u: applied %u, unresolved %u | surfaces mapped %u reused %u failed %u\n",
           n, ok, bad, sSurfStat[0], sSurfStat[1], sSurfStat[2]);
    recUnlock();
}

// v0.68.11: the first picture-buffer resources the VA driver makes (their RO private part as we filled it)
void IrisXeVideoRecordResource(const UInt32 *priv, UInt32 type, unsigned long long argsSize) {
    static UInt32 n;
    if (!sVideoTest || n++ >= 16) return;
    recLock();
    recAdd("\n#resource type 0x%x args 0x%llx priv: %08x %08x %08x %08x %08x %08x %08x\n", type, argsSize,
           priv[0], priv[1], priv[2], priv[3], priv[4], priv[5], priv[6]);
    recUnlock();
}

// ---------------------------------------------------------------- v0.68.6: what the family does with a submission
// counts per (kind, id, result); the first time a combination is seen it is also noted in the record
struct XeCount { const char *kind; UInt32 id, res, n; };
static XeCount sCounts[48];
static void xeCount(const char *kind, UInt32 id, UInt32 res) {
    bool first = false;
    recLock();
    int i = 0;
    for (; i < 48 && sCounts[i].kind; i++)
        if (sCounts[i].kind == kind && sCounts[i].id == id && sCounts[i].res == res) { sCounts[i].n++; break; }
    if (i < 48 && !sCounts[i].kind) { sCounts[i] = { kind, id, res, 1 }; first = true; }
    if (first) recAdd("[%s 0x%x -> 0x%x] ", kind, id, res);
    recUnlock();
}
static void xeCountsText(char *b, UInt32 len) {
    UInt32 o = 0;
    for (int i = 0; i < 48 && sCounts[i].kind && o + 40 < len; i++)
        o += (UInt32)snprintf(b + o, len - o, "%s 0x%x->0x%x x%u, ", sCounts[i].kind, sCounts[i].id, sCounts[i].res, sCounts[i].n);
}

// ---------------------------------------------------------------- v0.69.0: hardware decoding on VCS0
// debug.irisxe_video (IrisXeAccel.cpp): 0 record only, 1 self-test (stage A), 2 decode + record, 3 decode.
// Per Media-context command stream (research rounds 45-49, Apple's IGAccelVideoContext):
//   SetFence token (0x80xx): token +8 = fence slot (checked against the family's fence memory sizes), kept here
//   BatchBufferStart (0x81xx): token +8 = resource id, +0xc = offset: its GPU address into the MI_BATCH_BUFFER_START
//   CodecHAL (0x82xx): the patch list, applied in place to the command buffer (as Apple does)
//   end of stream: the commands (info +0x30 byte offset, +0x34 dwords; the last 2 dwords are room for
//   MI_BATCH_BUFFER_END) are copied into our batch buffer, mapped into the process's GPU space at 0x1F800000, and
//   run on VCS0 (IrisXeCore, synchronously). Then the frame's fence slot is set to an empty event (= passed: the
//   job has already ended) and its pending word cleared, as Apple's endCommandStream does with its stamp.
// Anything we cannot resolve (an unknown codec token, an unresolved patch address): the job is not run and the
// stream fails (ok flag 0) - the app then decodes in software. 3 GPU timeouts in one boot: decoding switches off.
static int sStage;                       // 0..3
static bool sDecodeOff;                  // after 3 timeouts
static UInt32 sVHangs, sVRun, sVFail, sVNoRun, sVBadPatch, sVUnknownTok, sVFences, sVOtherCtx, sVAttrFix;
static UInt64 sVGpuUs;
static IOLock *sVJobLock;
static IOBufferMemoryDescriptor *sVBatch;
static UInt64 sVBatchPA[130];
static const UInt64 kVBatchVA = XE_SURF_BASE + 31 * XE_SURF_SLOT;
static const UInt32 kVBatchPages = 130;  // 512 KB of commands + MI_BATCH_BUFFER_END
static char sVSelf[600] = "not run (sudo sysctl debug.irisxe_video=1)";

static void videoPublish() {
    if (!sVideoAccel) return;
    char t[600];
    snprintf(t, sizeof(t), "stage %d%s | jobs run %u (GPU %llu us average), failed %u, timeouts %u | not run: %u (unresolved "
             "patch addresses %u, unknown codec tokens %u) | fences passed %u | streams of other contexts %u | "
             "compression bits cleared %u | self-test: %s",
             sStage, sDecodeOff ? " (OFF after 3 GPU timeouts)" : "", sVRun, sVRun ? sVGpuUs / sVRun : 0ULL, sVFail, sVHangs,
             sVNoRun, sVBadPatch, sVUnknownTok, sVFences, sVOtherCtx, sVAttrFix, sVSelf);
    sVideoAccel->setProperty("IrisXe-VideoDecode", t);
}
int IrisXeVideoStage() { return sStage; }
int IrisXeVideoSetStage(int v) {
    if (v < 0 || v > 3) return EINVAL;
    if (!sVideoAccel) return ENODEV;
    if (v == 1) {                                    // stage A: the self-test, the stage stays as it was
        int r = sVideoAccel->coreVideoSelfTest(100, true, sVSelf, sizeof(sVSelf));
        if (r == -10) strlcpy(sVSelf, "IrisXeCore v0.69.0 or newer needed (install it too, restart)", sizeof(sVSelf));
        videoPublish();
        return 0;
    }
    sStage = v;
    if (v >= 2) sDecodeOff = false, sVHangs = 0;     // a new try
    videoPublish();
    return 0;
}
static bool videoBatchReady() {
    if (sVBatch) return true;
    if (!sVJobLock) sVJobLock = IOLockAlloc();
    IOBufferMemoryDescriptor *m = IOBufferMemoryDescriptor::withOptions(kIODirectionInOut, kVBatchPages * 4096, 4096);
    if (!m || m->prepare() != kIOReturnSuccess) { OSSafeReleaseNULL(m); return false; }
    for (UInt32 i = 0; i < kVBatchPages; i++) {
        IOByteCount seg = 0;
        addr64_t p = m->getPhysicalSegment((IOByteCount)i * 4096, &seg, kIOMemoryMapperNone);
        if (!p) { m->complete(); m->release(); return false; }
        sVBatchPA[i] = p & ~0xFFFULL;
    }
    sVBatch = m;
    return sVJobLock != nullptr;
}
// the CodecHAL patch list, applied to the command buffer itself. Returns the number of entries it could not apply.
// Memory-attribute dwords right after a patched address (small values, no address bits) lose the Gen12 compression
// bits 9 / 10 (Apple's Gen11 values never mean compression on Gen12 surfaces we map).
static UInt32 applyCodecHAL(const UInt32 *tok, UInt32 sizeDw, void *task, void *shared, const UInt8 *info, bool rec) {
    if (!tok || sizeDw < 8) return 0;
    UInt32 *cmd = *(UInt32 * const *)(info + 0x00);
    const UInt8 *base = *(const UInt8 * const *)(info + 0x08);
    UInt64 cmdSize = *(const UInt64 *)(info + 0x10);
    if (!cmd || !base || (const UInt8 *)cmd < base) return 1;
    UInt64 cmdDw64 = (UInt64)(base + cmdSize - (const UInt8 *)cmd) / 4;
    UInt32 cmdDw = cmdDw64 > 0xFFFFF ? 0xFFFFF : (UInt32)cmdDw64;
    const UInt32 *e = tok + 2 + 6, *end = tok + sizeDw;
    UInt32 bad = 0, ok = 0;
    if (rec) { recLock(); recAdd("\n#run patch:"); recUnlock(); }
    for (; e + 5 <= end && (e[0] & 1); e += 5) {
        UInt32 dw0 = e[0], type = (dw0 >> 1) & 7, idx = (dw0 >> 5) & 0xFFFFF, sh = (dw0 >> 25) & 31;
        if (type == 3) continue;                                        // skip entry
        UInt32 id1 = e[1] & 0xFFFF, id2 = e[1] >> 16, p1 = 0, p2 = 0;
        UInt64 va1 = 0, va2 = 0, v = 0;
        resolveId(shared, id1, task, &va1, &p1);
        bool good = va1 != 0;
        switch (type) {
            case 0: v = va1; break;
            case 1: v = p1 ? p1 - 1 : 0; good = p1 != 0; break;
            case 2: resolveId(shared, id2, task, &va2, &p2);
                    v = (va2 > va1 && p1) ? (va2 - va1) / p1 : 0; good = good && va2 && p1; break;
            default: good = false; break;
        }
        if (dw0 & 0x40000000) v >>= sh; else v <<= sh;
        if ((dw0 & 0x80000000) && (e[3] | e[4])) good = false;
        if (!good || idx + (type == 0 ? 1 : 0) >= cmdDw) { bad++; continue; }
        UInt64 full = v + e[2];
        if (type == 0) {
            cmd[idx] = (UInt32)full;
            cmd[idx + 1] = (UInt32)(full >> 32);
            if (idx + 2 < cmdDw) {
                UInt32 a = cmd[idx + 2];
                if (!(a & ~0x7FFu) && (a & 0x600u)) { cmd[idx + 2] = a & ~0x600u; sVAttrFix++; }
            }
        } else cmd[idx] = (UInt32)(e[2] + (UInt32)v);
        ok++;
        if (rec) { recLock(); recAdd(" %u:%x", idx, cmd[idx]); recUnlock(); }
    }
    if (rec) { recLock(); recAdd(" | applied %u, unresolved %u\n", ok, bad); recUnlock(); }
    return bad;
}
// the fence slot of this stream: an empty event (passed) and the pending word cleared
static void videoPassFence(IrisXeVideoContext *c, SInt32 &slot) {
    if (slot < 0) return;
    UInt8 *me = (UInt8 *)c;
    UInt8 *evMem = *(UInt8 **)(me + 0x6a8), *pend = *(UInt8 **)(me + 0x6a0);
    UInt32 evBytes = *(UInt32 *)(me + 0x6d8), pendBytes = *(UInt32 *)(me + 0x6c8);
    IOAccelEventMachine2 *em = sVideoAccel ? (IOAccelEventMachine2 *)sVideoAccel->eventMachine() : nullptr;
    if (evMem && em && (UInt32)slot * 64 + 64 <= evBytes) { em->initEvent((IOAccelEvent *)(evMem + (UInt32)slot * 64)); sVFences++; }
    if (pend && (UInt32)slot * 16 + 4 <= pendBytes) *(volatile UInt32 *)(pend + (UInt32)slot * 16) = 0;
    slot = -1;
}

// ---------------------------------------------------------------- the context
IOAccelDataBufferLimits IrisXeVideoContext::getDataBufferLimits() {
    // as Apple's Intel video context (IGAccelVideoContext::getDataBufferLimits, round 35): the family's limits, then
    // one buffer class, 16 buffers of 64 KB .. 512 KB, sideband 0x300
    IOAccelDataBufferLimits r = IOAccelContext2::getDataBufferLimits();
    *(UInt32 *)&r.bytes[0x00] = 1;
    *(UInt64 *)&r.bytes[0x08] = 0x100000010ULL;
    *(UInt64 *)&r.bytes[0x10] = 0x10000;
    *(UInt64 *)&r.bytes[0x18] = 0x80000;
    *(UInt32 *)&r.bytes[0x2c] = 0x300;
    return r;
}

IRISXE_SLOT_RET IrisXeVideoContext::processSidebandToken(IOAccelCommandStreamInfo &info) {
    const UInt8 *p = (const UInt8 *)&info;
    UInt16 id = *(const UInt16 *)(p + 0x20);
    if ((id >> 8) < 0x80) {                                                          // the family's own tokens
        // v0.68.9: the first family tokens of each context in full (header + data): v0.68.8 saw only start / end
        // tokens and empty command streams, so what they carry is the next clue
        static UInt32 fdumps[3];
        UInt32 t = xeType >= 0x100 && xeType <= 0x102 ? xeType - 0x100 : 0;
        if (fdumps[t] < 10) {
            fdumps[t]++;
            const UInt32 *tok = *(const UInt32 * const *)(p + 0x18);
            UInt16 sz = *(const UInt16 *)(p + 0x22);
            recLock();
            recAdd("\n#ftok ctx 0x%x id 0x%04x size %u off 0x%x:", xeType, id, sz, *(const UInt32 *)(p + 0x24));
            if (tok) for (UInt32 k = 0; k < sz && k < 24; k++) recAdd(" %08x", tok[k]);
            recAdd("\n");
            recUnlock();
        }
        IRISXE_SLOT_RET r = IOAccelVideoContext2::processSidebandToken(info);
        xeCount("ftok", id, (UInt32)r);
        return r;
    }
    recordToken(p, xeType);
    if (sStage >= 2 && (xeType == 0x101 || (id >> 8) == 0x80)) {   // v0.69.0: decoding on VCS0 (fences: every context)
        const UInt32 *tok = *(const UInt32 * const *)(p + 0x18);
        UInt16 sz = *(const UInt16 *)(p + 0x22);
        UInt32 g = id >> 8;
        void *task = (void *)getGPUTask(), *shared = *(void **)((UInt8 *)this + 0x5a0);
        if (g == 0x80) {                                     // SetFence
            UInt32 slot = tok && sz >= 3 ? tok[2] : 0xFFFFFFFFu;
            UInt32 pendBytes = *(UInt32 *)((UInt8 *)this + 0x6c8), evBytes = *(UInt32 *)((UInt8 *)this + 0x6d8);
            if (slot != 0xFFFFFFFFu && (UInt64)slot * 16 < pendBytes && (UInt64)slot * 64 < evBytes) xeFence = (SInt32)slot;
            else xeBad = true;
        } else if (g == 0x81) {                              // BatchBufferStart
            UInt32 *cmd = *(UInt32 * const *)(p + 0x00);
            const UInt8 *base = *(const UInt8 * const *)(p + 0x08);
            UInt64 cmdSize = *(const UInt64 *)(p + 0x10);
            UInt64 va = 0; UInt32 pitch = 0;
            if (tok && sz == 4 && cmd && base && (const UInt8 *)cmd >= base && (const UInt8 *)(cmd + 3) <= base + cmdSize &&
                resolveId(shared, tok[2], task, &va, &pitch) && va) {
                UInt64 a = va + tok[3];
                cmd[1] = (UInt32)a & ~3u;
                cmd[2] = (UInt32)(a >> 32);
            } else { xeBad = true; sVBadPatch++; }
        } else if (g == 0x82) {                              // CodecHAL patch list
            static UInt32 recs;
            bool rec = sStage == 2 && recs < 4;
            if (rec) recs++;
            UInt32 bad = applyCodecHAL(tok, sz, task, shared, p, rec);
            if (bad) { xeBad = true; sVBadPatch += bad; }
        } else { xeBad = true; sVUnknownTok++; }             // a codec token Apple's Media context handles itself
        static UInt32 n2;
        if ((++n2 & 15) == 1) IrisXeVideoRecordPublish(sVideoAccel);
        return 0;
    }
    if ((id >> 8) == 0x82)                                   // v0.68.15: CodecHAL patch list, dry run
        dryRunCodecHAL(*(const UInt32 * const *)(p + 0x18), *(const UInt16 *)(p + 0x22), (void *)getGPUTask(),
                       *(void **)((UInt8 *)this + 0x5a0), p);
    static UInt32 n;
    if ((++n & 15) == 1) IrisXeVideoRecordPublish(sVideoAccel);
    return 0;                                         // accepted (the ok flag stays set)
}

IRISXE_SLOT_RET IrisXeVideoContext::discardSidebandToken(IOAccelCommandStreamInfo &info) {
    UInt16 id = *(const UInt16 *)((const UInt8 *)&info + 0x20);
    if ((id >> 8) < 0x80) return IOAccelVideoContext2::discardSidebandToken(info);
    return 0;
}

// v0.68.7: Apple's Intel video context config (IGAccelVideoContext::populateContextConfig, research round 45): the
// family's, then its first 8 bytes = 0x0001000000000080. Without it (v0.68.6) the VA driver's command streams held
// only the family's start / end tokens - none of its Intel tokens (SetFence / BatchBufferStart / CodecHAL).
IRISXE_SLOT_RET IrisXeVideoContext::populateContextConfig(IOAccelContextConfig *cfg) {
    IRISXE_SLOT_RET r = IOAccelVideoContext2::populateContextConfig(cfg);
    if (cfg) *(UInt64 *)cfg = 0x1000000000080ULL;
    xeCount("config", xeType, (UInt32)r);
    return r;
}
IRISXE_SLOT_RET IrisXeVideoContext::processSidebandBuffer(IOAccelCommandDescriptor *d, bool b) {
    IRISXE_SLOT_RET r = IOAccelVideoContext2::processSidebandBuffer(d, b);
    xeCount("sideband", b ? 1 : 0, (UInt32)r);
    return r;
}
IRISXE_SLOT_RET IrisXeVideoContext::processDataBuffers(unsigned int n) {
    IRISXE_SLOT_RET r = IOAccelVideoContext2::processDataBuffers(n);
    xeCount("databufs", n, (UInt32)r);
    static UInt32 k;
    if ((++k & 15) == 1) IrisXeVideoRecordPublish(sVideoAccel);
    return r;
}
IRISXE_SLOT_RET IrisXeVideoContext::beginCommandStream(IOAccelCommandStreamInfo &i) {
    IRISXE_SLOT_RET r = IOAccelVideoContext2::beginCommandStream(i);
    xeFence = -1; xeBad = false;                             // v0.69.0
    {   // v0.68.9: the stream info after the family's begin (command buffer base / size / offsets), first 3 per context
        static UInt32 bdumps[3];
        UInt32 t = xeType >= 0x100 && xeType <= 0x102 ? xeType - 0x100 : 0;
        if (bdumps[t] < 3) {
            bdumps[t]++;
            const UInt32 *w = (const UInt32 *)&i;
            recLock();
            recAdd("\n#begin ctx 0x%x info:", xeType);
            for (int k = 0; k < 0x48 / 4; k++) recAdd(" %08x", w[k]);
            recAdd("\n");
            recUnlock();
        }
    }
    xeCount("begin", xeType, (UInt32)r);
    return r;
}
IRISXE_SLOT_RET IrisXeVideoContext::endCommandStream(IOAccelCommandStreamInfo &i) {
    // v0.68.8: the GPU commands of this stream (research round 45: IGAccelVideoContext::endCommandStream submits the
    // command buffer from info +0x30 (byte offset) for info +0x34 dwords; the Intel tokens only patch addresses).
    // v0.68.7 showed streams holding only the start / end tokens, so the commands themselves are here.
    {
        const UInt8 *p = (const UInt8 *)&i;
        const UInt8 *base = *(const UInt8 * const *)(p + 0x08);
        UInt64 size = *(const UInt64 *)(p + 0x10);
        UInt32 off = *(const UInt32 *)(p + 0x30) & ~3u, n = *(const UInt32 *)(p + 0x34);
        xeCount("stream dwords", xeType, n > 0xFFFF ? 0xFFFF : n);
        static UInt32 dumps[3];
        UInt32 t = xeType >= 0x100 && xeType <= 0x102 ? xeType - 0x100 : 0;
        if (n && base && off + 4ull * n <= size && dumps[t] < 2) {
            dumps[t]++;
            const UInt32 *c = (const UInt32 *)(base + off);
            recLock();
            // v0.68.13: the whole stream, runs of zero dwords written as zN (v0.68.12: first 160 dwords only)
            recAdd("\n#stream ctx 0x%x dwords %u off 0x%x:", xeType, n, off);
            UInt32 lim = n < 4096 ? n : 4096;
            for (UInt32 k = 0; k < lim; ) {
                if (!c[k]) { UInt32 z = 0; while (k < lim && !c[k]) { z++; k++; } recAdd(" z%u", z); }
                else recAdd(" %x", c[k++]);
            }
            recAdd("\n");
            recUnlock();
            IrisXeVideoRecordPublish(sVideoAccel);
        }
    }
    if (sStage >= 2) videoRunStream(i);                       // v0.69.0
    IRISXE_SLOT_RET r = IOAccelVideoContext2::endCommandStream(i);
    xeCount("end", xeType, (UInt32)r);
    return r;
}

// v0.69.0: the end of a stream: run its commands on VCS0, then pass its fence
void IrisXeVideoContext::videoRunStream(IOAccelCommandStreamInfo &i) {
    UInt8 *p = (UInt8 *)&i;
    UInt32 off = *(const UInt32 *)(p + 0x30) & ~3u, n = *(const UInt32 *)(p + 0x34);
    if (xeType != 0x101) { if (n) sVOtherCtx++; videoPassFence(this, xeFence); return; }
    if (n < 3) { videoPassFence(this, xeFence); return; }   // fence-only stream (Apple: a stamp, no batch)
    const UInt8 *base = *(const UInt8 * const *)(p + 0x08);
    UInt64 size = *(const UInt64 *)(p + 0x10);
    UInt32 cmdDw = n - 2;
    bool fail = false;
    if (sDecodeOff || xeBad || !base || off + 4ull * n > size || cmdDw + 2 > kVBatchPages * 1024 || !videoBatchReady()) {
        sVNoRun++; fail = true;
    } else {
        int space = irisxeTaskSpace(sVideoAccel, (IOAccelTask *)getGPUTask());
        if (space <= 0) { sVNoRun++; fail = true; }
        else {
            IOLockLock(sVJobLock);
            UInt32 *b = (UInt32 *)sVBatch->getBytesNoCopy();
            memcpy(b, base + off, cmdDw * 4ull);
            b[cmdDw] = 0x05000000u;                          // MI_BATCH_BUFFER_END
            b[cmdDw + 1] = 0;
            __asm__ volatile("mfence" ::: "memory");
            UInt32 pages = (cmdDw * 4 + 8 + 4095) / 4096;
            bool mapped = sVideoAccel->coreMapPages((UInt32)space, kVBatchVA, sVBatchPA, pages);
            UInt32 us = 0;
            int r = mapped ? sVideoAccel->coreVideoRun((UInt32)space, kVBatchVA, 2000, &us) : -20;
            IOLockUnlock(sVJobLock);
            if (r == 0) { sVRun++; sVGpuUs += us; }
            else {
                sVFail++; fail = true;
                if (r == -3 || r == -9) { sVHangs++; if (sVHangs >= 3) sDecodeOff = true; }
            }
            static UInt32 recs;
            if (sStage == 2 && recs < 8) {
                recs++;
                recLock();
                recAdd("\n#run ctx 0x%x space %d dwords %u -> %d (%u us)", xeType, space, cmdDw, r, us);
                recUnlock();
                IrisXeVideoRecordPublish(sVideoAccel);
            }
        }
    }
    if (fail) p[0x3c] = 0;                                   // the stream failed: the VA driver gets an error
    videoPassFence(this, xeFence);
    if ((sVRun + sVFail + sVNoRun) % 32 == 1 || fail) videoPublish();
}

// v0.68.6: the family's own selectors (0..7) run as before, through a wrapper that records each one's result
static IOExternalMethod sFamOrig[8], sFamWrap[8];
typedef IOReturn (*XeVidFn)(IrisXeVideoContext *, void *, void *, void *, void *, void *, void *);
template <int K> static IOReturn xeFamWrap(IrisXeVideoContext *self, void *a, void *b, void *c, void *d, void *e, void *f) {
    IOMethod m = sFamOrig[K].func;
    IOService *obj = (IOService *)self;
    IOReturn r = (obj->*m)(a, b, c, d, e, f);
    xeCount("sel", K, (UInt32)r);
    return r;
}
static const XeVidFn sFamFns[8] = { (XeVidFn)xeFamWrap<0>, (XeVidFn)xeFamWrap<1>, (XeVidFn)xeFamWrap<2>, (XeVidFn)xeFamWrap<3>,
                                    (XeVidFn)xeFamWrap<4>, (XeVidFn)xeFamWrap<5>, (XeVidFn)xeFamWrap<6>, (XeVidFn)xeFamWrap<7> };

// selectors 0x100..0x102: IGAccelVideoContext::attach's method_descs (round 35): get_iosurface_accel_id (struct out),
// update_perf_capability (struct in), set_context_priority (struct in)
static IOReturn xeVidAccelId(IrisXeVideoContext *self, void *in, void *out, void *inSize, void *outSize, void *, void *) {
    UInt32 id = sVideoAccel ? *(const UInt32 *)((const UInt8 *)sVideoAccel + 0x178) : 0;   // IOSurface accelerator id
    IOByteCount *os = (IOByteCount *)outSize;
    if (out && os && *os >= 4) { *(UInt32 *)out = id; *os = 4; }
    else if (in) *(UInt32 *)in = id;
    IrisXeVideoRecordNote("accel id", id);
    return kIOReturnSuccess;
}
static IOReturn xeVidPerf(IrisXeVideoContext *, void *in, void *, void *inSize, void *, void *, void *) {
    IrisXeVideoRecordNote("perf", (UInt32)(uintptr_t)inSize);
    return kIOReturnSuccess;
}
static IOReturn xeVidPrio(IrisXeVideoContext *, void *in, void *, void *inSize, void *, void *, void *) {
    IrisXeVideoRecordNote("priority", in && (uintptr_t)inSize >= 4 ? *(const UInt32 *)in : 0xFFFFFFFF);
    return kIOReturnSuccess;
}
IOReturn IrisXeVideoContext::getAcceleratorId(void *out, IOByteCount *outSize) { return xeVidAccelId(this, nullptr, out, nullptr, outSize, nullptr, nullptr); }
IOReturn IrisXeVideoContext::perfCapability(void *in, IOByteCount inSize) { return xeVidPerf(this, in, nullptr, (void *)(uintptr_t)inSize, nullptr, nullptr, nullptr); }
IOReturn IrisXeVideoContext::setPriority(void *in, IOByteCount inSize) { return xeVidPrio(this, in, nullptr, (void *)(uintptr_t)inSize, nullptr, nullptr, nullptr); }

// IOExternalMethod calls (object->*func)(p1 .. p6): a member-function pointer whose function is one of the plain
// functions above (same registers: this in rdi, then six pointers)
union XeMethodPtr { IOMethod m; struct { void *fn; uintptr_t adj; } raw; };
IOExternalMethod *IrisXeVideoContext::getTargetAndMethodForIndex(IOService **target, UInt32 index) {
    static IOExternalMethod sMethods[3];
    static bool init;
    if (!init) {
        XeVidFn fns[3] = { xeVidAccelId, xeVidPerf, xeVidPrio };
        for (int i = 0; i < 3; i++) {
            XeMethodPtr u; u.raw.fn = (void *)fns[i]; u.raw.adj = 0;
            sMethods[i].object = nullptr;
            sMethods[i].func = u.m;
            sMethods[i].flags = kIOUCStructIStructO;
            sMethods[i].count0 = i == 0 ? 0 : kIOUCVariableStructureSize;
            sMethods[i].count1 = i == 0 ? kIOUCVariableStructureSize : 0;
        }
        init = true;
    }
    if (index >= 0x100 && index <= 0x102) {
        if (target) *target = this;
        return &sMethods[index - 0x100];
    }
    IOExternalMethod *m = _ZN15IOAccelContext226getTargetAndMethodForIndexEPP9IOServicej(this, target, index);
    if (m && index < 8 && m->func && (!target || *target == this)) {
        // v0.68.6: wrap it (same flags and sizes; object stays nullptr = the target, as in the family's table)
        sFamOrig[index] = *m;
        sFamWrap[index] = *m;
        XeMethodPtr u; u.raw.fn = (void *)sFamFns[index]; u.raw.adj = 0;
        sFamWrap[index].func = u.m;
        return &sFamWrap[index];
    }
    xeCount("method", index, m ? 1 : 0);
    return m;
}

// ================================================================ v0.68.2: the device connection (IrisXeDevice)
// IGAccelDevice (AppleIntelICLGraphics, research round 37/38): getTargetAndMethodForIndex sends 0..9 to the family
// (IOAccelDevice2::sDeviceMethods) and 10..24 to deviceStart()::methodDescs (15 IOExternalMethods, 0x30 bytes each):
//   10 get_wa_table  (struct in/out, 0xda bytes = IntelAccelerator +0xf36)
//   11 get_hw_caps   (struct in/out, 0x80 bytes = IntelAccelerator +0x1140; needs [+0x1188] and [+0x1154] non-zero)
//   12 debug_control  13..17 telemetry  18 logStatus  19..22 aub capture
//   23 get_vram_size (1 scalar out = IntelAccelerator +0xca8)
//   24 get_hw_info   (struct in/out, 0x288 bytes: caps 0x80, WA table at +0x80, SKU table (0x6e) at +0x15a,
//                     GT system info (0xc0) at +0x1c8)
OSDefineMetaClassAndStructors(IrisXeDevice, IOAccelDevice2)
extern "C" IOExternalMethod *_ZN14IOAccelDevice226getTargetAndMethodForIndexEPP9IOServicej(IOAccelDevice2 *, IOService **, unsigned int);

// what IntelAccelerator would hold for an Ice Lake GT2 (1 slice, 8 subslices, 8 EUs each), built the way its probe /
// getGPUInfo / initHardwareCaps fill it (research round 38/39). Offsets below are from IntelAccelerator +0x1140.
static UInt8 sCaps[0x80], sWA[0xda], sSKU[0x6e], sGTSys[0xc0];
static bool sDevInit;
static void put32(UInt8 *b, UInt32 off, UInt32 v) { *(UInt32 *)(b + off) = v; }
static void devInit() {
    if (sDevInit) return;
    bzero(sCaps, sizeof(sCaps)); bzero(sWA, sizeof(sWA)); bzero(sSKU, sizeof(sSKU)); bzero(sGTSys, sizeof(sGTSys));
    const UInt32 slices = 1, subslices = 8, euPerSS = 8, eus = subslices * euPerSS;
    put32(sCaps, 0x00, 5);                  // PCI revision (probe: > 2 = production part)
    put32(sCaps, 0x04, 0);                  // PCI config byte 0x1f
    put32(sCaps, 0x08, 0x8A528086);         // vendor | device << 16 (Ice Lake GT2)
    put32(sCaps, 0x10, 1);                  // GT type (getGPUInfo: 1 slice, 8 subslices -> 1)
    put32(sCaps, 0x14, eus);                // EU count (getEUCount)
    // initHardwareCaps, GT type 1
    put32(sCaps, 0x18, 1 * subslices);      // +0x1158
    put32(sCaps, 0x1c, 0x180); put32(sCaps, 0x20, 0x2d8);
    put32(sCaps, 0x24, 0x1b0); put32(sCaps, 0x28, 0x270);
    put32(sCaps, 0x2c, 0x1b0);
    put32(sCaps, 0x30, 0x40 * subslices);   // +0x1170
    put32(sCaps, 0x34, (8 - subslices) * 1);// +0x1174
    put32(sCaps, 0x38, 8);                  // +0x1178
    put32(sCaps, 0x3c, subslices);          // +0x117c
    put32(sCaps, 0x40, 1300);               // max frequency MHz (RP_STATE_CAP RP0 * 50)
    put32(sCaps, 0x44, 100);                // min frequency MHz (RPn * 50)
    put32(sCaps, 0x48, subslices);          // +0x1188 subslice count
    put32(sCaps, 0x4c, 0xFFFFFFFFu << subslices);   // +0x118c raw subslice-disable fuse (Gen11 meaning)
    put32(sCaps, 0x50, slices);             // +0x1190 slice count
    put32(sCaps, 0x54, 0x800);              // +0x1194
    put32(sCaps, 0x58, 8);                  // +0x1198 (6 or 8)
    put32(sCaps, 0x5c, slices << 4);        // +0x119c
    put32(sCaps, 0x60, euPerSS);            // +0x11a0 EUs per subslice
    put32(sCaps, 0x68, 7); put32(sCaps, 0x6c, 1);     // +0x11a8 (threads per EU 7)
    put32(sCaps, 0x70, 0x40); put32(sCaps, 0x74, 0x10);
    sCaps[0x78] |= 4;                       // +0x11b8 flag set for GT type 1
    put32(sCaps, 0x7c, 19200000);           // +0x11bc timestamp frequency (Tiger Lake: 19.2 MHz crystal)
    // GT system info (Intel GT_SYSTEM_INFO, first fields: EUCount, ThreadCount, SliceCount, SubSliceCount,
    // L3CacheSizeInKb, LLCCacheSizeInKb, EdramSizeInKb, L3BankCount, MaxFillRate) - the writer is not in the kext;
    // these are the common fields, the record shows whether the VA driver wants more
    put32(sGTSys, 0x00, eus); put32(sGTSys, 0x04, eus * 7); put32(sGTSys, 0x08, slices); put32(sGTSys, 0x0c, subslices);
    put32(sGTSys, 0x10, 3072); put32(sGTSys, 0x14, 0); put32(sGTSys, 0x18, 0); put32(sGTSys, 0x1c, 8);
    put32(sGTSys, 0x20, 8);
    sDevInit = true;
}

static UInt32 sDevCalls[16];
static void devNote(UInt32 sel, UInt32 size) {
    if (sel >= 10 && sel <= 24 && sDevCalls[sel - 10]++ < 2) IrisXeVideoRecordNote("device sel", (sel << 16) | (size & 0xFFFF));
}
// (struct in, struct out, in size, out size pointer) - the IOExternalMethod StructIStructO call
static IOReturn xeDevCopy(void *out, void *outSizeP, const void *src, UInt32 len, UInt32 sel) {
    IOByteCount *os = (IOByteCount *)outSizeP;
    devInit();
    UInt32 n = len;
    if (os && *os < n) n = (UInt32)*os;
    if (out) memcpy(out, src, n);
    if (os) *os = len;
    devNote(sel, n);
    return kIOReturnSuccess;
}
static IOReturn xeDevWA(IrisXeDevice *, void *, void *out, void *, void *os, void *, void *) { return xeDevCopy(out, os, sWA, sizeof(sWA), 10); }
static IOReturn xeDevCaps(IrisXeDevice *, void *, void *out, void *, void *os, void *, void *) { return xeDevCopy(out, os, sCaps, sizeof(sCaps), 11); }
static IOReturn xeDevVram(IrisXeDevice *, void *out, void *, void *, void *, void *, void *) {
    if (out) *(UInt64 *)out = 1536ULL << 20;
    devNote(23, 8);
    return kIOReturnSuccess;
}
static IOReturn xeDevInfo(IrisXeDevice *, void *, void *out, void *, void *os, void *, void *) {
    static UInt8 info[0x288];
    devInit();
    memcpy(info, sCaps, 0x80);
    memcpy(info + 0x80, sWA, 0xda);
    memcpy(info + 0x15a, sSKU, 0x6e);
    memcpy(info + 0x1c8, sGTSys, 0xc0);
    return xeDevCopy(out, os, info, sizeof(info), 24);
}
static IOReturn xeDevOther(IrisXeDevice *, void *, void *, void *, void *, void *, void *) { return kIOReturnUnsupported; }

IOExternalMethod *IrisXeDevice::getTargetAndMethodForIndex(IOService **target, UInt32 index) {
    static IOExternalMethod sDev[15];
    static bool init;
    if (!init) {
        for (int i = 0; i < 15; i++) {
            XeVidFn fn = (XeVidFn)xeDevOther;
            UInt32 flags = kIOUCStructIStructO, c0 = kIOUCVariableStructureSize, c1 = kIOUCVariableStructureSize;
            switch (i + 10) {
                case 10: fn = (XeVidFn)xeDevWA; break;
                case 11: fn = (XeVidFn)xeDevCaps; break;
                case 19: c1 = 0; break;                                    // start_aub_capture (struct in)
                case 20: c0 = 0; break;                                    // get_aub_capture (struct out)
                case 21: case 22: flags = kIOUCScalarIStructI; c0 = c1 = 0; break;
                case 23: fn = (XeVidFn)xeDevVram; flags = kIOUCScalarIScalarO; c0 = 0; c1 = 1; break;
                case 24: fn = (XeVidFn)xeDevInfo; break;
            }
            XeMethodPtr u; u.raw.fn = (void *)fn; u.raw.adj = 0;
            sDev[i].object = nullptr; sDev[i].func = u.m; sDev[i].flags = flags; sDev[i].count0 = c0; sDev[i].count1 = c1;
        }
        init = true;
    }
    if (index >= 10 && index <= 24) {
        if (target) *target = this;
        if (index != 10 && index != 11 && index != 23 && index != 24) devNote(index, 0);
        return &sDev[index - 10];
    }
    if (index > 24) IrisXeVideoRecordNote("device sel?", index);
    return _ZN14IOAccelDevice226getTargetAndMethodForIndexEPP9IOServicej(this, target, index);
}

IRISXE_SLOT_RET IrisXeVideoNewDevice() {
    if (!sVideoTest) return 0;
    static UInt32 n;
    if (n++ < 2) IrisXeVideoRecordNote("device", n);
    return (IRISXE_SLOT_RET)new IrisXeDevice;
}

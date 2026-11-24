// IrisXeFB - framebuffer driver for Intel Tiger Lake Iris Xe (8086:9a49)
//
// Takes over the screen that UEFI GOP set up and presents it to macOS.
// v0.4: reads the panel EDID over DP AUX (real panel identity + native timing),
// and publishes scaler info + detailed-timing support so macOS can offer HiDPI
// ("Looks like") sizes. No hardware scaling yet: only timings whose image size
// equals the native panel size are accepted.

#include "IrisXeFB.hpp"
#include "IrisXeBacklight.hpp"
#include "TGLRegs.h"
#include <string.h>
#include <libkern/libkern.h>
#include <IOKit/IODeviceTreeSupport.h>

#define LOG(fmt, ...) IOLog("IrisXeFB: " fmt "\n", ##__VA_ARGS__)

#define PP_CONTROL_REG      0xC7204
#define PP_STATUS_REG       0xC7200
#define PP_BLC_ENABLE       (1u << 2)
#define TRANSCONF_A_REG     0x70008

#define kIrisXeModeID   ((IODisplayModeID)1)
#define kIrisXeDepth0   ((IOIndex)0)
#define kEDIDBlockType  0          // kIODDCBlockTypeEDID
#define TIMING_MODE_ID(t) ((IODisplayModeID)((t)->__reservedA[0]))   // detailedTimingModeID

OSDefineMetaClassAndStructors(IrisXeFB, IOFramebuffer)

// ---------------------------------------------------------------- registers

UInt32 IrisXeFB::readReg(UInt32 off) const {
    if (!mmio) return 0;
    return *(volatile UInt32 *)(mmio + off);
}

void IrisXeFB::writeReg(UInt32 off, UInt32 val) {
    if (!mmio) return;
    *(volatile UInt32 *)(mmio + off) = val;
}

void IrisXeFB::logPlaneState() {
    UInt32 ctl = readReg(TGL_PLANE_CTL_1_A);
    UInt32 src = readReg(TGL_PIPESRC_A);
    LOG("plane1A ctl=0x%08x stride=0x%08x size=0x%08x surf=0x%08x pipesrc=0x%08x",
        ctl, readReg(TGL_PLANE_STRIDE_1_A), readReg(TGL_PLANE_SIZE_1_A),
        readReg(TGL_PLANE_SURF_1_A), src);
}

// ---------------------------------------------------------------- DP AUX

// One raw AUX transaction on channel A. Returns number of reply bytes, or <0.
int IrisXeFB::auxTransfer(const UInt8 *tx, int txLen, UInt8 *rx, int rxMax) {
    if (!mmio || txLen < 1 || txLen > 20) return -1;

    int i;
    for (i = 0; i < 100 && (readReg(TGL_DP_AUX_CH_CTL_A) & TGL_AUX_CTL_SEND_BUSY); i++)
        IODelay(10);
    if (i == 100) return -2;

    for (int r = 0; r < 5; r++) {
        UInt32 v = 0;
        for (int b = 0; b < 4; b++) {
            int idx = r * 4 + b;
            if (idx < txLen) v |= (UInt32)tx[idx] << ((3 - b) * 8);
        }
        writeReg(TGL_DP_AUX_CH_DATA_A(r), v);
    }

    UInt32 ctl = TGL_AUX_CTL_SEND_BUSY | TGL_AUX_CTL_DONE | TGL_AUX_CTL_TIME_OUT_ERROR |
                 TGL_AUX_CTL_TIME_OUT_MAX | TGL_AUX_CTL_RECEIVE_ERROR |
                 ((UInt32)txLen << TGL_AUX_CTL_MSG_SIZE_SHIFT) |
                 TGL_AUX_CTL_FW_SYNC(32) | TGL_AUX_CTL_SYNC(32);
    writeReg(TGL_DP_AUX_CH_CTL_A, ctl);

    UInt32 st = 0;
    for (i = 0; i < 1000; i++) {
        st = readReg(TGL_DP_AUX_CH_CTL_A);
        if (!(st & TGL_AUX_CTL_SEND_BUSY)) break;
        IODelay(10);
    }
    // clear sticky status bits
    writeReg(TGL_DP_AUX_CH_CTL_A, (st & ~TGL_AUX_CTL_SEND_BUSY) | TGL_AUX_CTL_DONE |
             TGL_AUX_CTL_TIME_OUT_ERROR | TGL_AUX_CTL_RECEIVE_ERROR);
    if (i == 1000) return -3;
    if (st & TGL_AUX_CTL_TIME_OUT_ERROR) return -4;
    if (st & TGL_AUX_CTL_RECEIVE_ERROR) return -5;

    int n = (int)((st & TGL_AUX_CTL_MSG_SIZE_MASK) >> TGL_AUX_CTL_MSG_SIZE_SHIFT);
    if (n < 1 || n > 20) return -6;
    if (n > rxMax) n = rxMax;
    for (int k = 0; k < n; k++) {
        UInt32 v = readReg(TGL_DP_AUX_CH_DATA_A(k / 4));
        rx[k] = (UInt8)(v >> ((3 - (k % 4)) * 8));
    }
    return n;
}

// I2C-over-AUX to address 0x50 (EDID). request = 4-bit AUX command.
// dataLen < 0 means address-only transaction (no length byte).
// Returns payload bytes received (after the reply byte), or <0.
int IrisXeFB::auxI2C(UInt8 request, const UInt8 *data, int dataLen, UInt8 *rx, int rxMax) {
    UInt8 tx[20];
    int txLen = 3;
    tx[0] = (UInt8)(request << 4);
    tx[1] = 0;
    tx[2] = 0x50;
    if (dataLen >= 0) {
        tx[3] = (UInt8)(dataLen > 0 ? dataLen - 1 : 0);
        txLen = 4;
        bool isWrite = !(request & 0x1);
        if (isWrite) {
            for (int i = 0; i < dataLen && txLen < 20; i++) tx[txLen++] = data[i];
        }
    }

    UInt8 reply[20];
    for (int attempt = 0; attempt < 8; attempt++) {
        int n = auxTransfer(tx, txLen, reply, sizeof(reply));
        if (n < 0) { IODelay(400); continue; }
        UInt8 nativeReply = (reply[0] >> 4) & 0x3;   // 0 ACK, 1 NACK, 2 DEFER
        UInt8 i2cReply    = (reply[0] >> 6) & 0x3;
        if (nativeReply == 2 || i2cReply == 2) { IODelay(500); continue; }
        if (nativeReply != 0 || i2cReply != 0) return -10;
        int payload = n - 1;
        if (payload > rxMax) payload = rxMax;
        for (int i = 0; i < payload; i++) rx[i] = reply[1 + i];
        return payload;
    }
    return -11;
}

bool IrisXeFB::readEDID() {
    edidLen = 0;
    bzero(edid, sizeof(edid));

    // I2C write (MOT): set EDID offset 0
    UInt8 offset = 0;
    if (auxI2C(0x4 /* MOT|WRITE */, &offset, 1, nullptr, 0) < 0) {
        setProperty("IrisXe-EDIDStatus", "offset write failed");
        return false;
    }

    UInt32 total = 128;
    UInt32 got = 0;
    int stalls = 0;
    while (got < total && stalls < 8) {
        UInt32 want = total - got;
        if (want > 16) want = 16;
        int n = auxI2C(0x5 /* MOT|READ */, nullptr, (int)want, edid + got, (int)want);
        if (n <= 0) { stalls++; continue; }
        got += (UInt32)n;
        if (got == 128 && edid[126] > 0) total = 256;   // one extension block
    }
    // end the I2C transaction (address-only READ without MOT)
    auxI2C(0x1, nullptr, -1, nullptr, 0);

    static const UInt8 header[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    if (got < 128 || memcmp(edid, header, 8) != 0) {
        setProperty("IrisXe-EDIDStatus", "bad header");
        return false;
    }
    UInt8 sum = 0;
    for (int i = 0; i < 128; i++) sum += edid[i];
    if (sum != 0) {
        setProperty("IrisXe-EDIDStatus", "bad checksum");
        return false;
    }
    edidLen = (got >= 256) ? 256 : 128;
    setProperty("IrisXe-EDID", edid, edidLen);
    setProperty("IrisXe-EDIDStatus", "ok");
    LOG("EDID read OK, %u bytes", edidLen);
    return true;
}

// Native AUX read of DPCD registers (panel capabilities / link status). Read-only.
int IrisXeFB::auxNativeRead(UInt32 addr, UInt8 *buf, int len) {
    if (len < 1 || len > 16) return -1;
    UInt8 tx[4] = {
        (UInt8)((0x9 << 4) | ((addr >> 16) & 0xF)),
        (UInt8)((addr >> 8) & 0xFF),
        (UInt8)(addr & 0xFF),
        (UInt8)(len - 1)
    };
    UInt8 reply[20];
    for (int attempt = 0; attempt < 8; attempt++) {
        int n = auxTransfer(tx, 4, reply, sizeof(reply));
        if (n < 0) { IODelay(400); continue; }
        UInt8 nativeReply = (reply[0] >> 4) & 0x3;
        if (nativeReply == 2) { IODelay(500); continue; }    // DEFER
        if (nativeReply != 0) return -10;                      // NACK
        int payload = n - 1;
        if (payload > len) payload = len;
        for (int i = 0; i < payload; i++) buf[i] = reply[1 + i];
        return payload;
    }
    return -11;
}

// Native AUX write of DPCD registers.
int IrisXeFB::auxNativeWrite(UInt32 addr, const UInt8 *buf, int len) {
    if (len < 1 || len > 16) return -1;
    UInt8 tx[20];
    tx[0] = (UInt8)((0x8 << 4) | ((addr >> 16) & 0xF));
    tx[1] = (UInt8)((addr >> 8) & 0xFF);
    tx[2] = (UInt8)(addr & 0xFF);
    tx[3] = (UInt8)(len - 1);
    for (int i = 0; i < len; i++) tx[4 + i] = buf[i];
    UInt8 reply[20];
    for (int attempt = 0; attempt < 8; attempt++) {
        int n = auxTransfer(tx, 4 + len, reply, sizeof(reply));
        if (n < 0) { IODelay(400); continue; }
        UInt8 nativeReply = (reply[0] >> 4) & 0x3;
        if (nativeReply == 2) { IODelay(500); continue; }
        return nativeReply == 0 ? len : -10;
    }
    return -11;
}

// eDP link training on transcoder A / DDI A, using the link the firmware chose:
// rate-select index 6 (4.32 Gbps), 2 lanes, enhanced framing, swing/pre-emph 0.
// Mirrors Linux intel_dp_link_training.c (clock recovery, then channel EQ).
#define DP_TP_CTL_A_REG     0x60540
#define DP_TP_STATUS_A_REG  0x60544
#define TP_MASK             (7u << 8)
#define TP_PAT1             (0u << 8)
#define TP_PAT2             (1u << 8)
#define TP_IDLE             (2u << 8)
#define TP_NORMAL           (3u << 8)

bool IrisXeFB::trainLink(char *report, size_t reportLen, bool finish) {
    UInt8 v;
    UInt8 st[6];
    UInt32 tp = readReg(DP_TP_CTL_A_REG);

    // 1. sink: link config (rate via rate-select, lanes + enhanced framing)
    v = 0x00; auxNativeWrite(0x100, &v, 1);
    v = 0x06; auxNativeWrite(0x115, &v, 1);
    v = 0x82; auxNativeWrite(0x101, &v, 1);

    // 2. clock recovery: TPS1, scrambling off, swing 0 / pre-emphasis 0
    writeReg(DP_TP_CTL_A_REG, (tp & ~TP_MASK) | TP_PAT1);
    UInt8 train[3] = { 0x21, 0x00, 0x00 };          // 0x102..0x104
    auxNativeWrite(0x102, train, 3);
    bool cr = false;
    for (int i = 0; i < 5 && !cr; i++) {
        IODelay(200);
        if (auxNativeRead(0x202, st, 6) == 6)
            cr = ((st[0] & 0x11) == 0x11);           // CR done, lanes 0 and 1
    }
    if (!cr) {
        snprintf(report, reportLen, "CR failed st=%02x%02x adj=%02x", st[0], st[1], st[4]);
        writeReg(DP_TP_CTL_A_REG, (tp & ~TP_MASK) | TP_NORMAL);
        v = 0; auxNativeWrite(0x102, &v, 1);
        return false;
    }

    // 3. channel equalization: TPS2
    writeReg(DP_TP_CTL_A_REG, (tp & ~TP_MASK) | TP_PAT2);
    v = 0x22; auxNativeWrite(0x102, &v, 1);
    bool eq = false;
    for (int i = 0; i < 5 && !eq; i++) {
        IODelay(500);
        if (auxNativeRead(0x202, st, 6) == 6)
            eq = ((st[0] & 0x77) == 0x77) && (st[2] & 0x01);   // CR+EQ+lock, aligned
    }

    // 4. end training: sink pattern off, source idle (-> normal unless the
    //    caller still has to enable the transcoder first)
    v = 0x00; auxNativeWrite(0x102, &v, 1);
    writeReg(DP_TP_CTL_A_REG, (tp & ~TP_MASK) | TP_IDLE);
    for (int i = 0; i < 20 && !(readReg(DP_TP_STATUS_A_REG) & (1u << 25)); i++) IODelay(100);
    if (finish) writeReg(DP_TP_CTL_A_REG, (tp & ~TP_MASK) | TP_NORMAL);

    snprintf(report, reportLen, "%s st=%02x%02x align=%02x",
             eq ? "trained" : "EQ failed", st[0], st[1], st[2]);
    return eq;
}

// Full retrain, following the Linux TGL order: the training pattern only takes
// effect while the transcoder is off. Disable transcoder -> DDI function ->
// DDI buffer -> DP_TP_CTL, then re-enable DP_TP_CTL (pattern 1) -> DDI buffer,
// train, then DDI function -> transcoder -> pattern normal.
#define TRANSCONF_A_R        0x70008
#define TRANS_DDI_FUNC_A_R   0x60400
#define DDI_BUF_CTL_A_R      0x64000

bool IrisXeFB::retrainFull(char *report, size_t reportLen) {
    UInt32 transconf = readReg(TRANSCONF_A_R);
    UInt32 ddiFunc   = readReg(TRANS_DDI_FUNC_A_R);
    UInt32 ddiBuf    = readReg(DDI_BUF_CTL_A_R);
    UInt32 tp        = readReg(DP_TP_CTL_A_REG);
    if (!(ddiBuf & (1u << 31)) || !(ddiFunc & (1u << 31))) {
        snprintf(report, reportLen, "not configured buf=%08x func=%08x", ddiBuf, ddiFunc);
        return false;
    }

    // --- disable ---
    writeReg(TRANSCONF_A_R, transconf & ~(1u << 31));
    for (int i = 0; i < 100 && (readReg(TRANSCONF_A_R) & (1u << 30)); i++) IODelay(500);
    writeReg(TRANS_DDI_FUNC_A_R, ddiFunc & ~(1u << 31));
    writeReg(DDI_BUF_CTL_A_R, ddiBuf & ~(1u << 31));
    for (int i = 0; i < 50 && !(readReg(DDI_BUF_CTL_A_R) & (1u << 7)); i++) IODelay(20);
    writeReg(DP_TP_CTL_A_REG, tp & ~(1u << 31));

    // --- enable link with training pattern 1 ---
    writeReg(DP_TP_CTL_A_REG, ((tp & ~TP_MASK) | TP_PAT1) | (1u << 31));
    writeReg(DDI_BUF_CTL_A_R, ddiBuf | (1u << 31));
    for (int i = 0; i < 50 && (readReg(DDI_BUF_CTL_A_R) & (1u << 7)); i++) IODelay(20);
    IODelay(600);

    char rep[64];
    bool ok = trainLink(rep, sizeof(rep), false);

    // --- transcoder back on, then normal pixels ---
    writeReg(TRANS_DDI_FUNC_A_R, ddiFunc | (1u << 31));
    writeReg(TRANSCONF_A_R, transconf | (1u << 31));
    for (int i = 0; i < 100 && !(readReg(TRANSCONF_A_R) & (1u << 30)); i++) IODelay(500);
    writeReg(DP_TP_CTL_A_REG, (readReg(DP_TP_CTL_A_REG) & ~TP_MASK) | TP_NORMAL);

    snprintf(report, reportLen, "full:%s conf=%08x", rep, readReg(TRANSCONF_A_R));
    return ok;
}

// ---------------------------------------------------------------- pipe colour state
// After system sleep the pipe CSC, gamma mode and palette come back as garbage,
// which shows up as a washed-out / grey picture. Save them at boot, restore on wake.
// Offsets: Linux i915 intel_color_regs.h, skl_universal_plane_regs.h.
#define CSC_BLOCK_A     0x49010   // 16 regs: coeffs, CSC_MODE (0x49028), pre/post offsets
#define GAMMA_MODE_A    0x4A480
#define PLANE_COLOR_1A  0x701CC
#define LGC_PALETTE_A   0x4A000   // 256 entries

void IrisXeFB::saveColor() {
    if (!mmio) return;
    unsigned i = 0;
    for (unsigned k = 0; k < 16; k++) colorRegs[i++] = readReg(CSC_BLOCK_A + k * 4);
    colorRegs[i++] = readReg(GAMMA_MODE_A);
    colorRegs[i++] = readReg(PLANE_COLOR_1A);
    for (unsigned k = 0; k < 256; k++) colorRegs[i++] = readReg(LGC_PALETTE_A + k * 4);
    haveColor = true;
    for (unsigned k = 0; k < 8; k++) wmRegs[k] = readReg(0x70240 + k * 4);
    wmRegs[8] = readReg(0x70268);
    bootPTE0[0] = readReg(0x800000);      // GGTT lives at BAR0 + 8 MB
    bootPTE0[1] = readReg(0x800004);
    UInt32 brief[4] = { colorRegs[6], colorRegs[16], colorRegs[17], colorRegs[18 + 128] };
    setProperty("IrisXe-SnapshotColor", brief, sizeof(brief));   // CSC_MODE, GAMMA_MODE, PLANE_COLOR, palette[128]
}

void IrisXeFB::restoreColor() {
    if (!mmio || !haveColor) return;
    unsigned i = 0;
    for (unsigned k = 0; k < 16; k++) writeReg(CSC_BLOCK_A + k * 4, colorRegs[i++]);
    UInt32 gamma = colorRegs[i++];
    UInt32 planeColor = colorRegs[i++];
    for (unsigned k = 0; k < 256; k++) writeReg(LGC_PALETTE_A + k * 4, colorRegs[i++]);
    writeReg(GAMMA_MODE_A, gamma);
    writeReg(PLANE_COLOR_1A, planeColor);
    for (unsigned k = 0; k < 8; k++) writeReg(0x70240 + k * 4, wmRegs[k]);   // all WM levels
    writeReg(0x70268, wmRegs[8]);
}

// Live diagnostics for 2 minutes after a wake: is the pipe really scanning
// new frames, did the scan-out page move, underruns, panel self refresh?
// Screen-content check used to time the backlight after a wake: a grid of
// pixels is sampled when the display goes dark, and the backlight is turned
// on only once macOS has drawn something new (normally the lock screen), so
// the old desktop frame is never shown. Gives up after kBLMaxWaitMS.
#define kBLGridX      32
#define kBLGridY      20
#define kBLPollMS     30
#define kBLMinWaitMS  60
#define kBLMaxWaitMS  700

void IrisXeFB::sampleScreen(UInt32 *out) const {
    for (UInt32 gy = 0; gy < kBLGridY; gy++) {
        UInt32 y = (2 * gy + 1) * height / (2 * kBLGridY);
        const volatile UInt32 *row = (const volatile UInt32 *)((const volatile UInt8 *)fbVA + (size_t)y * rowBytes);
        for (UInt32 gx = 0; gx < kBLGridX; gx++)
            out[gy * kBLGridX + gx] = row[(2 * gx + 1) * width / (2 * kBLGridX)] & 0x00FFFFFF;
    }
}

bool IrisXeFB::screenChanged() const {
    if (!fbVA || !haveScreenSig) return true;
    UInt32 now[kBLGridX * kBLGridY];
    sampleScreen(now);
    for (UInt32 i = 0; i < kBLGridX * kBLGridY; i++)
        if (now[i] != screenSig[i]) return true;
    return false;
}

void IrisXeFB::blTimerFired(OSObject *owner, IOTimerEventSource *sender) {
    IrisXeFB *self = OSDynamicCast(IrisXeFB, owner);
    if (!self || !self->mmio || self->powerState != 2) return;
    self->blWaitedMS += kBLPollMS;
    bool changed = self->screenChanged();
    if (!changed && self->blWaitedMS < kBLMaxWaitMS && sender) {
        sender->setTimeoutMS(kBLPollMS);             // old frame still there: keep dark
        return;
    }
    char note[48];
    snprintf(note, sizeof(note), "%s after %u ms", changed ? "new frame" : "timeout", self->blWaitedMS);
    self->setProperty("IrisXe-WakeBacklight", note);
    // macOS may already have set a brightness in the meantime; keep it if so
    UInt32 cur = self->readReg(TGL_BLC_PWM_DUTY1);
    self->writeReg(TGL_BLC_PWM_DUTY1, cur ? cur : self->pendingDuty);
    self->writeReg(TGL_BLC_PWM_CTL1, self->readReg(TGL_BLC_PWM_CTL1) | (1u << 31));
    self->writeReg(PP_CONTROL_REG, self->readReg(PP_CONTROL_REG) | PP_BLC_ENABLE);
}

void IrisXeFB::liveTimerFired(OSObject *owner, IOTimerEventSource *) {
    IrisXeFB *self = OSDynamicCast(IrisXeFB, owner);
    if (self) self->liveTick();
}

void IrisXeFB::liveTick() {
    if (!mmio) return;
    char buf[300];
    snprintf(buf, sizeof(buf),
             "t=%u frm=%u dsl=%u surf=%08x live=%08x iir=%08x psr=%08x/%08x "
             "pte0=%08x%08x boot=%08x%08x conf=%08x",
             liveTicks * 2, readReg(0x70040), readReg(0x70000) & 0x1FFF,
             readReg(0x7019C), readReg(0x701AC), readReg(0x44408),
             readReg(0x60800), readReg(0x60840),
             readReg(0x800004), readReg(0x800000), bootPTE0[1], bootPTE0[0],
             readReg(0x70008));
    setProperty("IrisXe-Live", buf);
    if (++liveTicks < 60 && liveTimer) liveTimer->setTimeoutMS(2000);
}

// ---------------------------------------------------------------- stage C restore

UInt32 IrisXeFB::snapReg(const char *key, bool phy) const {
    OSDictionary *d = phy ? bootPHY : bootRegs;
    OSNumber *n = d ? OSDynamicCast(OSNumber, d->getObject(key)) : nullptr;
    return n ? n->unsigned32BitValue() : 0;
}

bool IrisXeFB::waitBits(UInt32 off, UInt32 mask, UInt32 value, UInt32 timeoutMs) {
    for (UInt32 t = 0; t <= timeoutMs * 10; t++) {
        if ((readReg(off) & mask) == value) return true;
        IODelay(100);
    }
    return false;
}

// Order follows Linux i915 (TGL): power wells -> DBUF -> combo PHY init ->
// panel power -> DPLL0 -> DDI clock -> PHY lane levels -> transcoder ->
// link training -> transcoder enable -> plane -> backlight.
// CDCLK is left as the firmware set it on resume (higher than we need).
bool IrisXeFB::restoreDisplay(char *report, size_t reportLen) {
    if (!bootRegs || !bootPHY) { snprintf(report, reportLen, "no boot snapshot"); return false; }
    char step[24] = "start";
    bool ok = true;
    #define STEP(name, cond) do { strlcpy(step, name, sizeof(step)); if (!(cond)) { ok = false; goto done; } } while (0)

    // 1. power wells: PW1-4 requests, AUX A, DDI A IO
    writeReg(0x45404, readReg(0x45404) | 0xAA);
    STEP("pw", waitBits(0x45404, 0x55, 0x55, 20));
    writeReg(0x45444, readReg(0x45444) | 0x2);
    STEP("aux-pw", waitBits(0x45444, 0x1, 0x1, 20));
    writeReg(0x45454, readReg(0x45454) | 0x2);
    STEP("ddi-pw", waitBits(0x45454, 0x1, 0x1, 20));

    // 2. display buffer slice 1
    writeReg(0x45008, readReg(0x45008) | (1u << 31));
    STEP("dbuf", waitBits(0x45008, 1u << 30, 1u << 30, 10));

    // 3. combo PHY A init (procmon refs, IREFGEN, COMP_INIT, CL power-down enable)
    writeReg(0x64C00, readReg(0x64C00) & ~(1u << 23));
    writeReg(0x162104, (readReg(0x162104) & ~0x00FF00FFu) | (snapReg("COMP_DW1", true) & 0x00FF00FFu));
    writeReg(0x162124, snapReg("COMP_DW9", true));
    writeReg(0x162128, snapReg("COMP_DW10", true));
    writeReg(0x162120, readReg(0x162120) | (1u << 24));
    writeReg(0x162100, readReg(0x162100) | (1u << 31));
    writeReg(0x162014, snapReg("CL_DW5", true));
    IODelay(100);

    // 4. panel power (eDP standard delays: T3 210 ms, T8/T9 50 ms, T10 50 ms)
    writeReg(0xC7208, (2100u << 16) | 500u);
    writeReg(0xC720C, (500u << 16) | 500u);
    writeReg(0xC7204, (snapReg("PP_CONTROL") & 0xF0u) | 0x3u);
    STEP("panel-on", waitBits(0xC7200, (1u << 31) | (3u << 28), 1u << 31, 500));

    // 5. DPLL0: power, config, enable, lock
    writeReg(0x46010, readReg(0x46010) | (1u << 27));
    STEP("dpll-pwr", waitBits(0x46010, 1u << 26, 1u << 26, 5));
    writeReg(0x164284, snapReg("DPLL0_CFGCR0"));
    writeReg(0x164288, snapReg("DPLL0_CFGCR1"));
    writeReg(0x46010, readReg(0x46010) | (1u << 31));
    STEP("dpll-lock", waitBits(0x46010, 1u << 30, 1u << 30, 5));

    // 6. DDI A clock: select DPLL0, then ungate
    {
        UInt32 want = snapReg("DPCLKA_CFGCR0");
        writeReg(0x164280, (readReg(0x164280) & ~0x3u) | (want & 0x3u));
        writeReg(0x164280, want);
    }

    // 7. PHY lane config and voltage levels (level 0, as the firmware trained)
    writeReg(0x162028, snapReg("CL_DW10", true));
    writeReg(0x162030, snapReg("CL_DW12", true));
    for (unsigned l = 0; l < 4; l++) {
        char k[24];
        UInt32 base = 0x162000 + l * 0x100;
        snprintf(k, sizeof(k), "PCS_DW1_LN%u", l); writeReg(base + 0x804, snapReg(k, true));
        snprintf(k, sizeof(k), "TX_DW5_LN%u", l);  writeReg(base + 0x894, snapReg(k, true) & ~(1u << 31));
        snprintf(k, sizeof(k), "TX_DW2_LN%u", l);  writeReg(base + 0x888, snapReg(k, true));
        snprintf(k, sizeof(k), "TX_DW4_LN%u", l);  writeReg(base + 0x890, snapReg(k, true));
        snprintf(k, sizeof(k), "TX_DW7_LN%u", l);  writeReg(base + 0x89C, snapReg(k, true));
    }
    for (unsigned l = 0; l < 4; l++) {
        char k[24];
        snprintf(k, sizeof(k), "TX_DW5_LN%u", l);
        writeReg(0x162000 + l * 0x100 + 0x894, snapReg(k, true));   // training enable
    }

    // 8. transcoder A: clock, timings, M/N, MSA, function (not enabled yet)
    writeReg(0x46140, snapReg("TRANS_CLK_SEL_A"));
    writeReg(0x60000, snapReg("TRANS_HTOTAL_A"));
    writeReg(0x60004, snapReg("TRANS_HBLANK_A"));
    writeReg(0x60008, snapReg("TRANS_HSYNC_A"));
    writeReg(0x6000C, snapReg("TRANS_VTOTAL_A"));
    writeReg(0x60010, snapReg("TRANS_VBLANK_A"));
    writeReg(0x60014, snapReg("TRANS_VSYNC_A"));
    writeReg(0x6001C, snapReg("PIPESRC_A"));
    writeReg(0x60030, snapReg("TRANS_DATA_M1_A"));
    writeReg(0x60034, snapReg("TRANS_DATA_N1_A"));
    writeReg(0x60040, snapReg("TRANS_LINK_M1_A"));
    writeReg(0x60044, snapReg("TRANS_LINK_N1_A"));
    writeReg(0x60410, snapReg("TRANS_MSA_MISC_A"));
    writeReg(0x70030, snapReg("PIPE_MISC_A"));
    writeReg(0x60400, snapReg("TRANS_DDI_FUNC_CTL_A") & ~(1u << 31));

    // 9. wake the panel receiver, link on with pattern 1, train
    {
        UInt8 d0 = 0x01;
        auxNativeWrite(0x600, &d0, 1);
        IOSleep(2);
        UInt32 tp = snapReg("DP_TP_CTL_A");
        writeReg(0x60540, ((tp & ~TP_MASK) | TP_PAT1) | (1u << 31));
        writeReg(0x64000, snapReg("DDI_BUF_CTL_A") | (1u << 31));
        waitBits(0x64000, 1u << 7, 0, 2);
        IODelay(600);
        char rep[64];
        bool trained = trainLink(rep, sizeof(rep), false);
        if (!trained) { IOSleep(5); trained = trainLink(rep, sizeof(rep), false); }
        STEP(trained ? "trained" : "train", trained);
    }

    // 10. transcoder on, normal pixels
    writeReg(0x60400, snapReg("TRANS_DDI_FUNC_CTL_A") | (1u << 31));
    writeReg(0x70008, snapReg("TRANSCONF_A") | (1u << 31));
    STEP("transcoder", waitBits(0x70008, 1u << 30, 1u << 30, 50));
    writeReg(0x60540, (readReg(0x60540) & ~TP_MASK) | TP_NORMAL);

    // 11. pipe colour (CSC, gamma, palette), then plane 1A (SURF last: arms the update)
    restoreColor();
    writeReg(0x7027C, snapReg("PLANE_BUF_CFG_1_A"));
    writeReg(0x70240, snapReg("PLANE_WM_1_A_0"));
    writeReg(0x70188, snapReg("PLANE_STRIDE_1_A"));
    writeReg(0x7018C, snapReg("PLANE_POS_1_A"));
    writeReg(0x70190, snapReg("PLANE_SIZE_1_A"));
    writeReg(0x701A4, snapReg("PLANE_OFFSET_1_A"));
    writeReg(0x70180, snapReg("PLANE_CTL_1_A"));
    writeReg(0x7019C, snapReg("PLANE_SURF_1_A"));

    // 12. backlight PWM (duty restored by the caller)
    writeReg(TGL_BLC_PWM_FREQ1, snapReg("BLC_PWM_FREQ1"));
    STEP("done", true);

done:
    #undef STEP
    snprintf(report, reportLen, "%s at %s conf=%08x pp=%08x dpll=%08x",
             ok ? "restored" : "FAILED", step, readReg(0x70008), readReg(0xC7200), readReg(0x46010));
    return ok;
}

// Read-only snapshot of everything needed to re-light the panel after sleep.
// Published as ioreg properties so we can compare with the Linux i915 driver.
void IrisXeFB::snapshotDisplay(const char *regsKey, const char *phyKey, bool withDPCD) {
    if (!mmio) return;
    OSDictionary *d = OSDictionary::withCapacity(80);
    if (!d) return;

    static const struct { const char *name; UInt32 off; } regs[] = {
        // panel power sequencer (PCH)
        {"PP_STATUS", 0xC7200}, {"PP_CONTROL", 0xC7204},
        {"PP_ON_DELAYS", 0xC7208}, {"PP_OFF_DELAYS", 0xC720C},
        // power wells / fuses
        {"FUSE_STATUS", 0x42000}, {"PWR_WELL_CTL1_BIOS", 0x45400},
        {"PWR_WELL_CTL2_DRIVER", 0x45404}, {"PWR_WELL_CTL_AUX1", 0x45440},
        {"PWR_WELL_CTL_AUX2", 0x45444}, {"PWR_WELL_CTL_DDI1", 0x45450},
        {"PWR_WELL_CTL_DDI2", 0x45454}, {"DBUF_CTL_S1", 0x45008},
        // display clocks
        {"CDCLK_CTL", 0x46000}, {"CDCLK_PLL_ENABLE", 0x46070},
        {"DPLL0_ENABLE", 0x46010}, {"DPLL1_ENABLE", 0x46014},
        {"DPCLKA_CFGCR0", 0x164280},
        {"DPLL0_CFGCR0", 0x164284}, {"DPLL0_CFGCR1", 0x164288},
        {"DPLL1_CFGCR0", 0x16428C}, {"DPLL1_CFGCR1", 0x164290},
        {"TRANS_CLK_SEL_A", 0x46140}, {"TRANS_CLK_SEL_B", 0x46144},
        // ports
        {"DDI_BUF_CTL_A", 0x64000}, {"DDI_BUF_CTL_B", 0x64100},
        // which transcoder drives the panel
        {"TRANS_DDI_FUNC_CTL_A", 0x60400}, {"TRANS_DDI_FUNC_CTL_B", 0x61400},
        {"TRANS_DDI_FUNC_CTL_C", 0x62400}, {"TRANS_DDI_FUNC_CTL_D", 0x63400},
        {"TRANS_DDI_FUNC_CTL_EDP", 0x6F400},
        // transcoder A timings / link
        {"TRANS_HTOTAL_A", 0x60000}, {"TRANS_HBLANK_A", 0x60004},
        {"TRANS_HSYNC_A", 0x60008}, {"TRANS_VTOTAL_A", 0x6000C},
        {"TRANS_VBLANK_A", 0x60010}, {"TRANS_VSYNC_A", 0x60014},
        {"PIPESRC_A", 0x6001C},
        {"TRANS_DATA_M1_A", 0x60030}, {"TRANS_DATA_N1_A", 0x60034},
        {"TRANS_LINK_M1_A", 0x60040}, {"TRANS_LINK_N1_A", 0x60044},
        {"TRANS_MSA_MISC_A", 0x60410},
        {"DP_TP_CTL_A", 0x60540}, {"DP_TP_STATUS_A", 0x60544},
        {"TRANSCONF_A", 0x70008}, {"PIPE_MISC_A", 0x70030},
        // pipe scaler
        {"PS_CTRL_1A", 0x68180}, {"PS_WIN_POS_1A", 0x68170}, {"PS_WIN_SZ_1A", 0x68174},
        // plane 1A
        {"PLANE_CTL_1_A", 0x70180}, {"PLANE_STRIDE_1_A", 0x70188},
        {"PLANE_POS_1_A", 0x7018C}, {"PLANE_SIZE_1_A", 0x70190},
        {"PLANE_SURF_1_A", 0x7019C}, {"PLANE_OFFSET_1_A", 0x701A4},
        {"PLANE_BUF_CFG_1_A", 0x7027C}, {"PLANE_WM_1_A_0", 0x70240},
        {"CUR_CTL_A", 0x70080}, {"CUR_BASE_A", 0x70084},
        // backlight
        {"BLC_PWM_CTL1", TGL_BLC_PWM_CTL1}, {"BLC_PWM_FREQ1", TGL_BLC_PWM_FREQ1},
        {"BLC_PWM_DUTY1", TGL_BLC_PWM_DUTY1},
    };

    for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        OSNumber *n = OSNumber::withNumber((unsigned long long)readReg(regs[i].off), 32);
        if (n) { d->setObject(regs[i].name, n); n->release(); }
    }
    setProperty(regsKey, d);
    d->release();

    // Combo PHY A (eDP), DMC and DC states. Offsets from Linux i915
    // intel_combo_phy_regs.h / intel_dmc_regs.h. PHY A base = 0x162000.
    OSDictionary *p = OSDictionary::withCapacity(48);
    if (p) {
        static const struct { const char *name; UInt32 off; } phy[] = {
            {"CL_DW5", 0x162014}, {"CL_DW10", 0x162028}, {"CL_DW12", 0x162030},
            {"COMP_DW0", 0x162100}, {"COMP_DW1", 0x162104}, {"COMP_DW3", 0x16210C},
            {"COMP_DW8", 0x162120}, {"COMP_DW9", 0x162124}, {"COMP_DW10", 0x162128},
            {"PHY_MISC_A", 0x64C00},
            {"DC_STATE_EN", 0x45504}, {"DMC_SSP_BASE", 0x8F074}, {"DMC_HTP", 0x8F004},
        };
        for (unsigned i = 0; i < sizeof(phy) / sizeof(phy[0]); i++) {
            OSNumber *n = OSNumber::withNumber((unsigned long long)readReg(phy[i].off), 32);
            if (n) { p->setObject(phy[i].name, n); n->release(); }
        }
        // per-lane PCS / TX (lanes 0-3): base 0x162000 + 0x800/0x880 + lane*0x100
        static const struct { const char *name; UInt32 off; } lane[] = {
            {"PCS_DW1", 0x804}, {"TX_DW2", 0x888}, {"TX_DW4", 0x890},
            {"TX_DW5", 0x894}, {"TX_DW7", 0x89C},
        };
        for (unsigned l = 0; l < 4; l++) {
            for (unsigned i = 0; i < sizeof(lane) / sizeof(lane[0]); i++) {
                char key[24];
                snprintf(key, sizeof(key), "%s_LN%u", lane[i].name, l);
                UInt32 off = 0x162000 + lane[i].off + l * 0x100;
                OSNumber *n = OSNumber::withNumber((unsigned long long)readReg(off), 32);
                if (n) { p->setObject(key, n); n->release(); }
            }
        }
        setProperty(phyKey, p);
        p->release();
    }
    if (!withDPCD) return;

    // DPCD (panel side of the eDP link)
    static const struct { const char *name; UInt32 addr; int len; } dpcd[] = {
        {"IrisXe-DPCD-000", 0x000, 16},   // receiver caps (max rate, lanes)
        {"IrisXe-DPCD-100", 0x100, 2},    // link rate / lane count set
        {"IrisXe-DPCD-200", 0x200, 8},    // link status
        {"IrisXe-DPCD-700", 0x700, 1},    // eDP revision
        {"IrisXe-DPCD-010", 0x010, 16},   // eDP supported link rates table
        {"IrisXe-DPCD-101", 0x101, 4},    // lane count, training pattern, lane drive 0-1
        {"IrisXe-DPCD-115", 0x115, 1},    // eDP link rate select (index into table)
        {"IrisXe-DPCD-206", 0x206, 2},    // requested vswing / pre-emphasis
    };
    for (unsigned i = 0; i < sizeof(dpcd) / sizeof(dpcd[0]); i++) {
        UInt8 buf[16];
        int n = auxNativeRead(dpcd[i].addr, buf, dpcd[i].len);
        if (n > 0) setProperty(dpcd[i].name, buf, (unsigned)n);
        else setProperty(dpcd[i].name, "read failed");
    }
}

// Native timing from the first EDID detailed timing descriptor, else synthesized.
void IrisXeFB::buildNativeTiming() {
    bzero(&nativeTiming, sizeof(nativeTiming));
    IODetailedTimingInformationV2 *t = &nativeTiming;

    const UInt8 *d = edid + 54;
    UInt32 pclk10k = edidLen ? (UInt32)(d[0] | (d[1] << 8)) : 0;
    if (pclk10k) {
        t->pixelClock               = (UInt64)pclk10k * 10000ULL;
        t->horizontalActive         = d[2] | ((d[4] & 0xF0) << 4);
        t->horizontalBlanking       = d[3] | ((d[4] & 0x0F) << 8);
        t->verticalActive           = d[5] | ((d[7] & 0xF0) << 4);
        t->verticalBlanking         = d[6] | ((d[7] & 0x0F) << 8);
        t->horizontalSyncOffset     = d[8] | ((d[11] & 0xC0) << 2);
        t->horizontalSyncPulseWidth = d[9] | ((d[11] & 0x30) << 4);
        t->verticalSyncOffset       = (d[10] >> 4) | ((d[11] & 0x0C) << 2);
        t->verticalSyncPulseWidth   = (d[10] & 0x0F) | ((d[11] & 0x03) << 4);
        t->horizontalSyncConfig     = (d[17] & 0x02) ? kIOSyncPositivePolarity : 0;
        t->verticalSyncConfig       = (d[17] & 0x04) ? kIOSyncPositivePolarity : 0;
    }
    // EDID panel must match the scan-out we inherited; otherwise synthesize.
    if (!pclk10k || t->horizontalActive != width || t->verticalActive != height) {
        bzero(t, sizeof(*t));
        t->horizontalActive         = width;
        t->verticalActive           = height;
        t->horizontalBlanking       = 160;
        t->verticalBlanking         = 46;
        t->horizontalSyncOffset     = 48;
        t->horizontalSyncPulseWidth = 32;
        t->verticalSyncOffset       = 3;
        t->verticalSyncPulseWidth   = 6;
        t->horizontalSyncConfig     = kIOSyncPositivePolarity;
        t->pixelClock = (UInt64)(width + 160) * (UInt64)(height + 46) * 60ULL;
    }
    t->minPixelClock = t->pixelClock;
    t->maxPixelClock = t->pixelClock;
    t->__reservedA[0] = (UInt32)kIrisXeModeID;   // detailedTimingModeID
}

UInt32 IrisXeFB::refreshFixed(const IODetailedTimingInformationV2 *t) const {
    UInt64 total = (UInt64)(t->horizontalActive + t->horizontalBlanking) *
                   (UInt64)(t->verticalActive + t->verticalBlanking);
    if (!total || !t->pixelClock) return 60 << 16;
    return (UInt32)((t->pixelClock << 16) / total);
}

// ---------------------------------------------------------------- backlight

bool IrisXeFB::backlightAvailable() const {
    return mmio && (readReg(TGL_BLC_PWM_CTL1) & (1u << 31)) && readReg(TGL_BLC_PWM_FREQ1);
}

UInt32 IrisXeFB::backlightLevel(UInt32 maxLevel) const {
    UInt32 freq = readReg(TGL_BLC_PWM_FREQ1);
    if (!freq) return maxLevel;
    UInt64 duty = readReg(TGL_BLC_PWM_DUTY1);
    if (duty > freq) duty = freq;
    return (UInt32)((duty * maxLevel + freq / 2) / freq);
}

// Diagnostics: every display-parameter write macOS sends us, in order, with
// the display power state at that moment and the PWM duty afterwards.
// Short key names: b=brightness l=linear-brightness lp/bp=probes, others in full.
void IrisXeFB::noteBrightness(const char *key, UInt32 value) {
    const char *k = key;
    if (!strcmp(key, "brightness")) k = "b";
    else if (!strcmp(key, "linear-brightness")) k = "l";
    else if (!strcmp(key, "linear-brightness-probe")) k = "lp";
    else if (!strcmp(key, "brightness-probe")) k = "bp";
    char e[48];
    if (value == 0xFFFFFFFF) snprintf(e, sizeof(e), "%s=data@%u ", k, powerState);
    else snprintf(e, sizeof(e), "%s=%u@%u ", k, value, powerState);
    size_t n = strlen(brightLog), m = strlen(e);
    if (n + m >= sizeof(brightLog)) {          // keep the newest half
        size_t keep = sizeof(brightLog) / 2;
        memmove(brightLog, brightLog + n - keep, keep);
        brightLog[keep] = 0;
        char *sp = strchr(brightLog, ' ');
        if (sp) memmove(brightLog, sp + 1, strlen(sp + 1) + 1);
    }
    strlcat(brightLog, e, sizeof(brightLog));
    setProperty("IrisXe-BrightLog", brightLog);
}

void IrisXeFB::setBacklightLevel(UInt32 level, UInt32 maxLevel) {
    UInt32 freq = readReg(TGL_BLC_PWM_FREQ1);
    if (!mmio || !freq || !maxLevel) return;
    if (level > maxLevel) level = maxLevel;
    UInt64 duty = ((UInt64)freq * level) / maxLevel;
    UInt64 floor = freq / 100;            // never fully dark: 1%
    if (powerState < 2) {
        setProperty("IrisXe-BrightIgnored", (unsigned long long)level, 32);
        // Display is off: macOS sets brightness to 0 while sleeping. Keep the
        // backlight off, and only remember a real (non-zero) level for wake.
        // Ignore it completely: during sleep macOS fades brightness down to 0,
        // and those values must not replace the brightness we restore on wake.
        (void)floor;
        return;
    }
    if (duty < floor) duty = floor;
    writeReg(TGL_BLC_PWM_DUTY1, (UInt32)duty);
    setProperty("IrisXe-BacklightDuty", (unsigned long long)duty, 32);
}

// ---------------------------------------------------------------- display power (stage A)
//
// Stage A of wake support: display sleep turns the panel backlight off (the
// eDP link and panel stay powered, so nothing needs retraining). We also
// record whether the display pipeline survived, which tells us what full
// system-sleep resume will have to rebuild.


void IrisXeFB::recordWakeCheck(const char *when) {
    OSDictionary *d = OSDictionary::withCapacity(12);
    if (!d) return;
    static const struct { const char *name; UInt32 off; } regs[] = {
        {"TRANSCONF_A", TRANSCONF_A_REG}, {"PP_STATUS", PP_STATUS_REG},
        {"PP_CONTROL", PP_CONTROL_REG}, {"DPLL0_ENABLE", 0x46010},
        {"CDCLK_PLL_ENABLE", 0x46070}, {"PWR_WELL_CTL1_BIOS", 0x45400},
        {"PWR_WELL_CTL2_DRIVER", 0x45404}, {"DDI_BUF_CTL_A", 0x64000},
        {"PLANE_CTL_1_A", TGL_PLANE_CTL_1_A}, {"BLC_PWM_CTL1", TGL_BLC_PWM_CTL1},
        {"BLC_PWM_DUTY1", TGL_BLC_PWM_DUTY1},
    };
    for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        OSNumber *n = OSNumber::withNumber((unsigned long long)readReg(regs[i].off), 32);
        if (n) { d->setObject(regs[i].name, n); n->release(); }
    }
    OSString *w = OSString::withCString(when);
    if (w) { d->setObject("when", w); w->release(); }
    setProperty("IrisXe-WakeCheck", d);
    d->release();
}

void IrisXeFB::setDisplayPower(UInt32 state) {
    // IOFramebuffer power states: 2 = on, 1 = display sleep ("doze"),
    // 0 = system sleep. Both 1 and 0 mean the panel should be dark.
    if (state == 0) sawSystemSleep = true;
    if (!mmio || state == powerState) return;
    bool wantOn = (state == 2);
    bool isOn   = (powerState == 2);

    if (!wantOn && isOn) {
        // remember brightness, then backlight fully off: duty 0, PWM output
        // disabled, and the panel backlight-enable bit cleared
        savedDuty = readReg(TGL_BLC_PWM_DUTY1);
        if (backlight) backlight->saveLevel();   // v0.68.18: remembered on sleep too, not only at restart
        if (fbVA) { sampleScreen(screenSig); haveScreenSig = true; }
        powerState = state;                   // blocks brightness writes from now on
        writeReg(TGL_BLC_PWM_DUTY1, 0);
        writeReg(TGL_BLC_PWM_CTL1, readReg(TGL_BLC_PWM_CTL1) & ~(1u << 31));
        writeReg(PP_CONTROL_REG, readReg(PP_CONTROL_REG) & ~PP_BLC_ENABLE);

        // Stage B (default; -irisxenostageb turns it off): also put the panel's receiver to sleep (DPCD D3).
        // On wake we must wake it and retrain the eDP link.
        if (!PE_parse_boot_argn("-irisxenostageb", nullptr, 0)) {
            UInt8 d3 = 0x02;
            auxNativeWrite(0x600, &d3, 1);
        }
    } else if (wantOn && !isOn) {
        bool restored = false;
        if (powerState == 0 || sawSystemSleep) {
            sawSystemSleep = false;
            // Stage C: firmware does not re-light the panel after system sleep.
            if (!(readReg(0x70008) & (1u << 31)) && !PE_parse_boot_argn("-irisxenorestore", nullptr, 0)) {
                char rep[160];
                restoreDisplay(rep, sizeof(rep));
                setProperty("IrisXe-Restore", rep);
                restored = true;
            }
        }
        if (!restored && !PE_parse_boot_argn("-irisxenostageb", nullptr, 0)) {
            // wake the panel receiver (D0) and retrain the eDP link
            UInt8 d0 = 0x01;
            auxNativeWrite(0x600, &d0, 1);
            IODelay(1000);
            char rep1[96] = "-";
            bool ok = retrainFull(rep1, sizeof(rep1));
            if (!ok) { IODelay(2000); ok = retrainFull(rep1, sizeof(rep1)); }   // one retry
            if (!ok) setProperty("IrisXe-LinkTrainFailed", rep1);
        }
        wakeCursor = true;      // re-arm the cursor plane once powerState is 2

        // Backlight on after a short delay, so the screen memory already holds
        // the lock screen instead of the last pre-sleep frame.
        UInt32 freq = readReg(TGL_BLC_PWM_FREQ1);
        pendingDuty = savedDuty ? savedDuty : freq / 2;
        if (!blTimer) {
            blTimer = IOTimerEventSource::timerEventSource(this, &IrisXeFB::blTimerFired);
            if (blTimer && getWorkLoop()) getWorkLoop()->addEventSource(blTimer);
        }
        if (blTimer && !PE_parse_boot_argn("-irisxenobldelay", nullptr, 0)) {
            blWaitedMS = kBLMinWaitMS - kBLPollMS;
            blTimer->setTimeoutMS(kBLMinWaitMS);
        } else {
            writeReg(TGL_BLC_PWM_DUTY1, pendingDuty);
            writeReg(TGL_BLC_PWM_CTL1, readReg(TGL_BLC_PWM_CTL1) | (1u << 31));
            writeReg(PP_CONTROL_REG, readReg(PP_CONTROL_REG) | PP_BLC_ENABLE);
        }
    }
    powerState = state;
    setProperty("IrisXe-DisplayPower", state, 32);
    if (wakeCursor && state == 2) {
        wakeCursor = false;
        cursorHWSetup();        // GGTT entries, display buffer and watermarks after a rebuild
    }
}

// Gamma: CoreDisplay sets a gamma ("transfer") table at login and on every
// display wake. Unsupported returned 0xe00002c7 ("can't set device gamma"),
// which can leave its display-wake sequence unfinished. Accept the table; the
// pipe keeps the colour state saved at boot (restored after sleep).
IOReturn IrisXeFB::setGammaTable(UInt32 channelCount, UInt32 dataCount,
                                 UInt32 dataWidth, void *data) {
    (void)channelCount; (void)dataCount; (void)dataWidth; (void)data;
    return kIOReturnSuccess;
}

IOReturn IrisXeFB::setGammaTable(UInt32 channelCount, UInt32 dataCount,
                                 UInt32 dataWidth, void *data, bool) {
    return setGammaTable(channelCount, dataCount, dataWidth, data);
}

IOReturn IrisXeFB::setAttribute(IOSelect attribute, uintptr_t value) {
    // Like Apple's IONDRVFramebuffer: handle power ourselves and do NOT pass it
    // on. IOFramebuffer::setAttribute(kIOPowerStateAttribute) forwards to the
    // graphics-switching ("mux") path, which changes pendingPowerState behind
    // IOGraphics' back and can leave WindowServer waiting forever after a wake.
    switch (attribute) {
        case kIOPowerStateAttribute: {
            // IOFramebuffer requires the subclass to report power changes back
            // through handleEvent() (same order as Apple's IONDRVFramebuffer).
            // kIOFBNotifyDidWake is what sets pagingState = true again after a
            // system sleep; without it WindowServer's IOFBAcknowledgeNotification
            // sleeps forever (the post-wake freeze).
            UInt32 newState = (value > 2) ? 2 : (UInt32)value;
            UInt32 oldState = fbEventState;
            if (newState == oldState) return kIOReturnSuccess;
            IOIndex post = 0;
            if (oldState == 2) {
                handleEvent(kIOFBNotifyWillPowerOff);
                post = kIOFBNotifyDidPowerOff;
            } else if (newState == 2) {
                handleEvent(kIOFBNotifyWillPowerOn);
                post = kIOFBNotifyDidPowerOn;
            }
            if (newState == 0)
                handleEvent(kIOFBNotifyWillSleep, (void *)true);

            if (!PE_parse_boot_argn("-irisxenodpms", nullptr, 0))
                setDisplayPower(newState);
            fbEventState = newState;

            if (oldState == 0)
                handleEvent(kIOFBNotifyDidWake, (void *)true);
            if (post)
                handleEvent(post);

            char ev[24];
            snprintf(ev, sizeof(ev), "%u>%u%s ", oldState, newState,
                     oldState == 0 ? "W" : "");
            if (strlen(eventLog) + strlen(ev) >= sizeof(eventLog)) eventLog[0] = 0;
            strlcat(eventLog, ev, sizeof(eventLog));
            setProperty("IrisXe-EventLog", eventLog);
            return kIOReturnSuccess;
        }
        case kIOSystemPowerAttribute:
            return kIOReturnSuccess;
        default:
            return super::setAttribute(attribute, value);
    }
}

// ---------------------------------------------------------------- scan-out discovery

bool IrisXeFB::discoverFramebuffer() {
    PE_Video video;
    bzero(&video, sizeof(video));

    IOPlatformExpert *pe = getPlatform();
    if (pe && pe->getConsoleInfo(&video) == kIOReturnSuccess &&
        video.v_baseAddr && video.v_width && video.v_height && video.v_rowBytes) {
        fbPhys   = (IOPhysicalAddress)(video.v_baseAddr & ~(uintptr_t)3);
        width    = (UInt32)video.v_width;
        height   = (UInt32)video.v_height;
        rowBytes = (UInt32)video.v_rowBytes;
        LOG("console info: base=0x%llx %ux%u rowBytes=%u",
            (unsigned long long)fbPhys, width, height, rowBytes);
    } else {
        UInt32 ctl  = readReg(TGL_PLANE_CTL_1_A);
        UInt32 src  = readReg(TGL_PIPESRC_A);
        UInt32 surf = readReg(TGL_PLANE_SURF_1_A) & ~0xFFFu;
        if (!(ctl & TGL_PLANE_CTL_ENABLE) || (ctl & TGL_PLANE_CTL_TILING_MASK)) {
            LOG("no console info and plane 1A is off or tiled; cannot continue");
            return false;
        }
        IODeviceMemory *aperture = pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress2);
        if (!aperture) return false;
        width    = ((src >> 16) & 0xFFFF) + 1;
        height   = (src & 0xFFFF) + 1;
        rowBytes = (readReg(TGL_PLANE_STRIDE_1_A) & 0xFFF) * 64;
        fbPhys   = aperture->getPhysicalAddress() + surf;
    }

    if (rowBytes < width * 4) return false;
    fbSize = (IOByteCount)rowBytes * height;
    fbSize = (fbSize + 0xFFF) & ~(IOByteCount)0xFFF;
    return true;
}

// ---------------------------------------------------------------- IOService

IOService *IrisXeFB::probe(IOService *provider, SInt32 *score) {
    if (!OSDynamicCast(IOPCIDevice, provider)) return nullptr;
    if (PE_parse_boot_argn("-irisxeoff", nullptr, 0)) return nullptr;
    if (!PE_parse_boot_argn("-irisxefb", nullptr, 0)) return nullptr;
    return super::probe(provider, score);
}

// v0.55.0: IrisXeIntelFB - see IrisXeFB.hpp
OSDefineMetaClassAndStructors(IrisXeIntelFB, IrisXeFB)

// v0.64.3: the GPU-drawn desktop from boot is the default (tested 23 Nov with v0.64.2: step 4 "on since boot").
// Each boot gets a GPU step, published as IrisXe-GPUStep on IrisXeIntelFB (IrisXeAccel and IrisXeAGDC read it):
//   1: the framebuffer is named IrisXeIntelFB (as nvram irisxe-intelfb)    2: 1 + display link (froze the boot)
//   3: 1 + the AGDC made at boot                                         4: 3 + the display pipe hand-over at boot
// Default: 4, with -irisxefb and -irisxemetal, unless the boot-arg -irisxecpu is set (then the CPU-drawn desktop).
// One-boot test: sudo nvram irisxe-gpustep=N, read and ERASED here at the very start of the boot.
// Safety net: NVRAM irisxe-gpuboot is set here (written to the chip at once) and removed by IrisXeAccel after 300
// frames through the display pipe, or when the hand-over stops safely. A boot that finds it was a boot that never got
// there (frozen, or switched off before): this boot uses the CPU-drawn desktop instead, and the next one tries again.
static int sGPUStep;
static bool sGPUSafety;
static bool sNoCore;          // v0.68.21: IrisXeCore (class IrisXeProbe, OpenCore-injected) not running
// v0.68.21: is IrisXeCore attached to the GPU? It comes from OpenCore's kernel collection, so it is normally there
// long before this kext (/Library/Extensions) probes; wait up to 5 s anyway. 24 Nov 2026: its binary was missing in
// the EFI (Contents/MacOS gone after a Windows boot), the GPU desktop started without it and every Metal job was
// refused ("not in the process's GPU page tables"): a black screen with only the mouse pointer.
static bool coreAttached(IOService *pci) {
    for (int i = 0; i < 50; i++) {
        bool found = false;
        if (OSIterator *it = pci ? pci->getChildIterator(gIOServicePlane) : nullptr) {
            while (OSObject *o = it->getNextObject())
                if (IOService *sv = OSDynamicCast(IOService, o))
                    if (!strcmp(sv->getMetaClass()->getClassName(), "IrisXeProbe")) { found = true; break; }
            it->release();
        }
        if (found) return true;
        IOSleep(100);
    }
    return false;
}
static int nvStep(IORegistryEntry *opt) {
    OSObject *o = opt->getProperty("irisxe-gpustep");
    if (!o) return 0;
    char c = 0;
    if (OSData *d = OSDynamicCast(OSData, o)) { if (d->getLength()) c = *(const char *)d->getBytesNoCopy(); }
    else if (OSString *st = OSDynamicCast(OSString, o)) c = st->getCStringNoCopy()[0];
    else if (OSNumber *n = OSDynamicCast(OSNumber, o)) c = (char)('0' + (n->unsigned32BitValue() % 10));
    return (c >= '1' && c <= '4') ? c - '0' : 0;
}

IOService *IrisXeIntelFB::probe(IOService *provider, SInt32 *score) {
    bool once = false, test = false;
    IORegistryEntry *opt = IORegistryEntry::fromPath("/options", gIODTPlane);
    if (opt) {
        bool sync = false;
        if (opt->getProperty("irisxe-intelfb")) {
            once = true;
            opt->removeProperty("irisxe-intelfb");
            sync = true;
        }
        if (opt->getProperty("irisxe-gpustep")) {
            sGPUStep = nvStep(opt);
            test = sGPUStep != 0;
            opt->removeProperty("irisxe-gpustep");
            sync = true;
        }
        if (!test && !once && !PE_parse_boot_argn("-irisxecpu", nullptr, 0) && PE_parse_boot_argn("-irisxemetal", nullptr, 0)) {
            if (!coreAttached(provider)) {                   // v0.68.21: no GPU desktop without IrisXeCore
                sNoCore = true;
                IOLog("IrisXeFB: v%s IrisXeCore is not running: CPU-drawn desktop this boot\n", KEXT_VERSION);
            } else if (opt->getProperty("irisxe-gpuboot")) {        // the last GPU-desktop boot never got there
                opt->removeProperty("irisxe-gpuboot");
                sGPUSafety = true;
            } else {
                OSData *d = OSData::withBytes("1", 1);
                if (d) { opt->setProperty("irisxe-gpuboot", d); d->release(); }
                sGPUStep = 4;
            }
            sync = true;
        }
        // v0.55.1: write the change to the NVRAM chip now; without this a forced power-off kept the
        // flag and every following boot was a test boot again (same request as `nvram -s`)
        if (sync) opt->setProperty("IONVRAM-FORCESYNCNOW-PROPERTY", "IONVRAM-FORCESYNCNOW-PROPERTY");
        opt->release();
    }
    if (!once && !sGPUStep && !PE_parse_boot_argn("-irisxeintelfb", nullptr, 0)) return nullptr;   // IrisXeFB matches instead
    IOLog("IrisXeFB: v%s framebuffer named IrisXeIntelFB this boot (%s, GPU step %d)\n", KEXT_VERSION,
          test ? "nvram irisxe-gpustep, one boot" : sGPUStep ? "default" : once ? "nvram irisxe-intelfb, one boot" : "boot-arg -irisxeintelfb",
          sGPUStep);
    return IrisXeFB::probe(provider, score);
}

bool IrisXeIntelFB::start(IOService *provider) {
    setProperty("IrisXe-IntelName", "on this boot: CoreDisplay sees an Intel GPU (IOPresentment allowed)");
    setProperty("IrisXe-GPUStep", (unsigned long long)sGPUStep, 32);
    return IrisXeFB::start(provider);
}

bool IrisXeFB::start(IOService *provider) {
    if (sGPUSafety) setProperty("IrisXe-GPUSafety", true);    // v0.64.3: CPU-drawn desktop this boot (safety net)
    if (sNoCore) setProperty("IrisXe-NoCore", true);          // v0.68.21: IrisXeCore missing -> CPU-drawn desktop
    pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) return false;

    currentMode = kIrisXeModeID;
    pci->setMemoryEnable(true);
    pci->setBusMasterEnable(true);

    mmioMap = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if (mmioMap) {
        mmio = (volatile UInt8 *)mmioMap->getVirtualAddress();
        logPlaneState();
    }

    if (!discoverFramebuffer()) {
        OSSafeReleaseNULL(mmioMap);
        mmio = nullptr;
        return false;
    }
    {   // CPU view of the screen, read-only use (wake backlight timing)
        IODeviceMemory *mem = IODeviceMemory::withRange(fbPhys, fbSize);
        if (mem) {
            fbMap = mem->map();
            mem->release();
            if (fbMap) fbVA = (volatile UInt32 *)fbMap->getVirtualAddress();
        }
    }

    if (mmio && !PE_parse_boot_argn("-irisxenoedid", nullptr, 0))
        readEDID();
    if (mmio && !PE_parse_boot_argn("-irisxenosnap", nullptr, 0)) {
        snapshotDisplay();
        saveColor();
        bootRegs = OSDynamicCast(OSDictionary, getProperty("IrisXe-Snapshot"));
        bootPHY  = OSDynamicCast(OSDictionary, getProperty("IrisXe-SnapshotPHY"));
        if (bootRegs) bootRegs->retain();
        if (bootPHY)  bootPHY->retain();
    }
    // hardware cursor (opt-in while it is being tested)
    if (mmio && haveColor && PE_parse_boot_argn("-irisxehwcursor", nullptr, 0))
        cursorInit();
    buildNativeTiming();

    // Scaler info: lets macOS build scaled/HiDPI mode lists. Features = 0
    // because we cannot resample yet; only same-size timings get accepted.
    IODisplayScalerInformation scaler;
    bzero(&scaler, sizeof(scaler));
    scaler.scalerFeatures      = 0;
    scaler.maxHorizontalPixels = width * 2;
    scaler.maxVerticalPixels   = height * 2;
    setProperty(kIOFBScalerInfoKey, &scaler, sizeof(scaler));

    setProperty("IOFBMemorySize", (unsigned long long)fbSize, 64);
    setProperty(kIOFBGammaWidthKey, 8ULL, 32);
    setProperty(kIOFBGammaCountKey, 256ULL, 32);
    setProperty("model", "Intel Iris Xe Graphics (IrisXeFB)");
    setProperty("IrisXe-Version", KEXT_VERSION);

    // Brightness handler: IODisplay finds it in our registry subtree.
    if (backlightAvailable() && !PE_parse_boot_argn("-irisxenobl", nullptr, 0)) {
        backlight = IrisXeBacklight::withFramebuffer(this);
        if (backlight && !backlight->attach(this)) OSSafeReleaseNULL(backlight);
        setProperty("IrisXe-Backlight", backlight ? "handler attached" : "handler failed");
    } else {
        setProperty("IrisXe-Backlight", "not available");
    }

    if (!super::start(provider)) {
        if (backlight) { backlight->detach(this); OSSafeReleaseNULL(backlight); }
        OSSafeReleaseNULL(mmioMap);
        mmio = nullptr;
        return false;
    }

    LOG("started: %ux%u, EDID %s", width, height, edidLen ? "yes" : "no");
    return true;
}

void IrisXeFB::stop(IOService *provider) {
    if (blTimer) {
        blTimer->cancelTimeout();
        if (getWorkLoop()) getWorkLoop()->removeEventSource(blTimer);
        OSSafeReleaseNULL(blTimer);
    }
    if (liveTimer) {
        liveTimer->cancelTimeout();
        if (getWorkLoop()) getWorkLoop()->removeEventSource(liveTimer);
        OSSafeReleaseNULL(liveTimer);
    }
    if (backlight) { backlight->detach(this); OSSafeReleaseNULL(backlight); }
    cursorShutdown();
    fbVA = nullptr;
    OSSafeReleaseNULL(fbMap);
    OSSafeReleaseNULL(mmioMap);
    mmio = nullptr;
    super::stop(provider);
}

void IrisXeFB::systemWillShutdown(IOOptionBits specifier) {
    if (backlight) backlight->saveLevel();     // restart / shut down
    super::systemWillShutdown(specifier);
}

void IrisXeFB::free() {
    OSSafeReleaseNULL(detailedTimings);
    OSSafeReleaseNULL(bootRegs);
    OSSafeReleaseNULL(bootPHY);
    super::free();
}

// ---------------------------------------------------------------- mode table

bool IrisXeFB::timingIsUsable(const IODetailedTimingInformationV2 *t) const {
    if (t->horizontalActive != width || t->verticalActive != height) return false;
    if (t->horizontalScaledInset || t->verticalScaledInset) return false;
    if (t->scalerFlags & ~kIOScaleStretchToFit) return false;
    UInt32 hs = t->horizontalScaled, vs = t->verticalScaled;
    if (hs == 0 && vs == 0) return true;               // not scaled
    return (hs == width && vs == height);              // "scaled" to the same size
}

const IODetailedTimingInformationV2 *IrisXeFB::timingForMode(IODisplayModeID mode) const {
    if (mode == kIrisXeModeID) return &nativeTiming;
    if (!detailedTimings) return nullptr;
    for (unsigned i = 0; i < detailedTimings->getCount(); i++) {
        OSData *d = OSDynamicCast(OSData, detailedTimings->getObject(i));
        if (!d || d->getLength() < sizeof(IODetailedTimingInformationV2)) continue;
        const IODetailedTimingInformationV2 *t =
            (const IODetailedTimingInformationV2 *)d->getBytesNoCopy();
        if (TIMING_MODE_ID(t) == mode && timingIsUsable(t)) return t;
    }
    return nullptr;
}

IOItemCount IrisXeFB::getDisplayModeCount() {
    IOItemCount n = 1;
    if (detailedTimings) {
        for (unsigned i = 0; i < detailedTimings->getCount(); i++) {
            OSData *d = OSDynamicCast(OSData, detailedTimings->getObject(i));
            if (!d || d->getLength() < sizeof(IODetailedTimingInformationV2)) continue;
            const IODetailedTimingInformationV2 *t =
                (const IODetailedTimingInformationV2 *)d->getBytesNoCopy();
            if (timingIsUsable(t) && TIMING_MODE_ID(t) != kIrisXeModeID) n++;
        }
    }
    return n;
}

IOReturn IrisXeFB::getDisplayModes(IODisplayModeID *allDisplayModes) {
    if (!allDisplayModes) return kIOReturnBadArgument;
    IOItemCount n = 0;
    allDisplayModes[n++] = kIrisXeModeID;
    if (detailedTimings) {
        for (unsigned i = 0; i < detailedTimings->getCount(); i++) {
            OSData *d = OSDynamicCast(OSData, detailedTimings->getObject(i));
            if (!d || d->getLength() < sizeof(IODetailedTimingInformationV2)) continue;
            const IODetailedTimingInformationV2 *t =
                (const IODetailedTimingInformationV2 *)d->getBytesNoCopy();
            if (timingIsUsable(t) && TIMING_MODE_ID(t) != kIrisXeModeID)
                allDisplayModes[n++] = TIMING_MODE_ID(t);
        }
    }
    return kIOReturnSuccess;
}

IOReturn IrisXeFB::getInformationForDisplayMode(IODisplayModeID mode,
                                                IODisplayModeInformation *info) {
    const IODetailedTimingInformationV2 *t = timingForMode(mode);
    if (!t || !info) return kIOReturnUnsupportedMode;
    bzero(info, sizeof(*info));
    info->nominalWidth  = width;
    info->nominalHeight = height;
    info->refreshRate   = refreshFixed(t);
    info->maxDepthIndex = 0;
    info->flags         = kDisplayModeValidFlag | kDisplayModeSafeFlag;
    if (mode == kIrisXeModeID)
        info->flags |= kDisplayModeDefaultFlag | kDisplayModeNativeFlag;
    return kIOReturnSuccess;
}

IOReturn IrisXeFB::getTimingInfoForDisplayMode(IODisplayModeID mode, IOTimingInformation *info) {
    const IODetailedTimingInformationV2 *t = timingForMode(mode);
    if (!t || !info) return kIOReturnUnsupportedMode;
    info->appleTimingID = kIOTimingIDApple_FixedRateLCD;
    if (info->flags & kIODetailedTimingValid) {
        info->detailedInfo.v2 = *t;
        if (mode == kIrisXeModeID) info->detailedInfo.v2.__reservedA[0] = (UInt32)kIrisXeModeID;
        info->flags = kIODetailedTimingValid;
    } else {
        info->flags = 0;
    }
    return kIOReturnSuccess;
}

IOReturn IrisXeFB::validateDetailedTiming(void *description, IOByteCount descripSize) {
    IODetailedTimingInformationV2 *t = nullptr;
    IOFBDisplayModeDescription *desc = nullptr;

    if (descripSize == sizeof(IOFBDisplayModeDescription)) {
        desc = (IOFBDisplayModeDescription *)description;
        t = &desc->timingInfo.detailedInfo.v2;
    } else if (descripSize >= sizeof(IODetailedTimingInformationV2)) {
        t = (IODetailedTimingInformationV2 *)description;
    } else {
        return kIOReturnUnsupported;
    }
    if (!timingIsUsable(t)) return kIOReturnUnsupportedMode;

    if (desc) {
        desc->info.nominalWidth  = width;
        desc->info.nominalHeight = height;
        desc->info.refreshRate   = refreshFixed(t);
        desc->info.maxDepthIndex = 0;
        desc->info.flags |= kDisplayModeValidFlag | kDisplayModeSafeFlag;
    }
    return kIOReturnSuccess;
}

IOReturn IrisXeFB::setDetailedTimings(OSArray *array) {
    OSSafeReleaseNULL(detailedTimings);
    if (array) {
        array->retain();
        detailedTimings = array;
        setProperty(kIOFBDetailedTimingsKey, array);
    } else {
        removeProperty(kIOFBDetailedTimingsKey);
    }
    if (currentMode != kIrisXeModeID && !timingForMode(currentMode))
        currentMode = kIrisXeModeID;
    return kIOReturnSuccess;
}

// ---------------------------------------------------------------- IOFramebuffer basics

// Power management. IOFramebuffer joins the PM tree in start(), but the driver
// must register its power states, otherwise macOS never tells us about display
// sleep (setAttribute(kIOPowerStateAttribute) is never called). Three states,
// like Apple's IONDRVFramebuffer: 0 off, 1 doze/dim, 2 on.
// kIOPMPreventSystemSleep is cleared unless -irisxenosleep is set.
IOReturn IrisXeFB::enableController() {
    static IOPMPowerState states[3] = {
        { 1, 0,                                          0,           0,           0, 0, 0, 0, 0, 0, 0, 0 },
        { 1, kIOPMPreventSystemSleep,                    0,           IOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 1, IOPMDeviceUsable | kIOPMPreventSystemSleep, IOPMPowerOn, IOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 },
    };
    // System sleep is allowed (wake is fully supported since v0.10.7).
    // -irisxenosleep blocks it again, as a safety switch.
    if (!PE_parse_boot_argn("-irisxenosleep", nullptr, 0)) {
        states[1].capabilityFlags &= ~kIOPMPreventSystemSleep;
        states[2].capabilityFlags &= ~kIOPMPreventSystemSleep;
    }
    registerPowerDriver(this, states, 3);
    temporaryPowerClampOn();          // no sleep until children are ready
    changePowerStateTo(1);            // not below doze until system sleep
    setProperty("IrisXe-PM", "registered");
    return kIOReturnSuccess;
}

unsigned long IrisXeFB::maxCapabilityForDomainState(IOPMPowerFlags domainState) {
    return (domainState & IOPMPowerOn) ? 2 : 0;
}

unsigned long IrisXeFB::initialPowerStateForDomainState(IOPMPowerFlags domainState) {
    return (domainState & IOPMPowerOn) ? 2 : 0;
}

unsigned long IrisXeFB::powerStateForDomainState(IOPMPowerFlags domainState) {
    return (domainState & IOPMPowerOn) ? getPowerState() : 0;
}
bool IrisXeFB::isConsoleDevice() { return true; }

IODeviceMemory *IrisXeFB::getApertureRange(IOPixelAperture aperture) {
    if (aperture != kIOFBSystemAperture || !fbPhys) return nullptr;
    return IODeviceMemory::withRange(fbPhys, fbSize);   // caller releases
}

IODeviceMemory *IrisXeFB::getVRAMRange() {
    return getApertureRange(kIOFBSystemAperture);
}

const char *IrisXeFB::getPixelFormats() {
    static const char formats[] = IO32BitDirectPixels "\0";
    return formats;
}

UInt64 IrisXeFB::getPixelFormatsForDisplayMode(IODisplayModeID, IOIndex) {
    return 0;
}

IOReturn IrisXeFB::getPixelInformation(IODisplayModeID mode, IOIndex depth,
                                       IOPixelAperture aperture,
                                       IOPixelInformation *pixelInfo) {
    if (!timingForMode(mode) || depth != kIrisXeDepth0 ||
        aperture != kIOFBSystemAperture || !pixelInfo)
        return kIOReturnBadArgument;

    bzero(pixelInfo, sizeof(*pixelInfo));
    pixelInfo->bytesPerRow       = rowBytes;
    pixelInfo->bitsPerPixel      = 32;
    pixelInfo->pixelType         = kIORGBDirectPixels;
    pixelInfo->componentCount    = 3;
    pixelInfo->bitsPerComponent  = 8;
    pixelInfo->componentMasks[0] = 0x00FF0000;
    pixelInfo->componentMasks[1] = 0x0000FF00;
    pixelInfo->componentMasks[2] = 0x000000FF;
    strlcpy(pixelInfo->pixelFormat, IO32BitDirectPixels, sizeof(pixelInfo->pixelFormat));
    pixelInfo->activeWidth  = width;
    pixelInfo->activeHeight = height;
    return kIOReturnSuccess;
}

IOReturn IrisXeFB::getCurrentDisplayMode(IODisplayModeID *mode, IOIndex *depth) {
    if (mode)  *mode  = currentMode;
    if (depth) *depth = kIrisXeDepth0;
    return kIOReturnSuccess;
}

IOReturn IrisXeFB::setDisplayMode(IODisplayModeID mode, IOIndex depth) {
    // Every mode we accept scans out the same native surface, so this is a no-op
    // for the hardware.
    if (depth != kIrisXeDepth0 || !timingForMode(mode)) return kIOReturnUnsupportedMode;
    currentMode = mode;
    return kIOReturnSuccess;
}

IOItemCount IrisXeFB::getConnectionCount() { return 1; }

IOReturn IrisXeFB::getAttributeForConnection(IOIndex connectIndex, IOSelect attribute,
                                             uintptr_t *value) {
    // IODisplay asks this (with value == NULL) before it reads the EDID through
    // hasDDCConnect/getDDCBlock. Without a success here macOS never sees the EDID.
    if (connectIndex == 0 && attribute == kConnectionSupportsHLDDCSense)
        return (edidLen >= 128) ? kIOReturnSuccess : kIOReturnUnsupported;

    if (connectIndex != 0 || !value)
        return super::getAttributeForConnection(connectIndex, attribute, value);

    switch (attribute) {
        case kConnectionEnable:
        case kConnectionCheckEnable:
            *value = 1;
            return kIOReturnSuccess;
        case kConnectionFlags:
            *value = kIOConnectionBuiltIn;
            return kIOReturnSuccess;
        default:
            return super::getAttributeForConnection(connectIndex, attribute, value);
    }
}

IOReturn IrisXeFB::setAttributeForConnection(IOIndex connectIndex, IOSelect attribute,
                                             uintptr_t value) {
    if (connectIndex == 0 && attribute == kConnectionProbe)
        return kIOReturnSuccess;
    return super::setAttributeForConnection(connectIndex, attribute, value);
}

// Tells macOS the built-in connection is an LCD panel, which (together with
// the "backlight" nub from IrisXeCore) makes it use AppleBacklightDisplay.
IOReturn IrisXeFB::getAppleSense(IOIndex connectIndex, UInt32 *senseType, UInt32 *primary,
                                 UInt32 *extended, UInt32 *displayType) {
    if (connectIndex != 0) return kIOReturnUnsupported;
    if (senseType)   *senseType   = 0;
    if (primary)     *primary     = 0;
    if (extended)    *extended    = 0;
    if (displayType) *displayType = 2;   // kPanelTFTConnect (IOMacOSVideo.h)
    return kIOReturnSuccess;
}

bool IrisXeFB::hasDDCConnect(IOIndex connectIndex) {
    return connectIndex == 0 && edidLen >= 128;
}

IOReturn IrisXeFB::getDDCBlock(IOIndex connectIndex, UInt32 blockNumber, IOSelect blockType,
                               IOOptionBits, UInt8 *data, IOByteCount *length) {
    if (connectIndex != 0 || blockType != kEDIDBlockType || !data || !length)
        return kIOReturnUnsupported;
    if (blockNumber < 1 || blockNumber * 128 > edidLen)
        return kIOReturnNotFound;
    IOByteCount n = 128;
    if (*length < n) n = *length;
    memcpy(data, edid + (blockNumber - 1) * 128, n);
    *length = n;
    return kIOReturnSuccess;
}

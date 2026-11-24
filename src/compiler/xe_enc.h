// xe_enc.h - Iris Xe (Gen12 / Tiger Lake) instruction encoder, milestone 3c.
//
// Native 128-bit EU instructions, align1 only. Bit positions are the Gfx12
// columns of Mesa's src/intel/compiler/brw_eu_inst.h (MIT licence); every form
// we emit is checked bit for bit against Mesa's brw_asm by
// tools/xec/test_encoder.py.
#pragma once
#include <stdint.h>
#include <string.h>

namespace xe {

enum Type : uint8_t { UB = 0, UW = 1, UD = 2, B = 4, W = 5, D = 6, HF = 9, F = 10, V = 4 /* vector imm (src only) */ };
enum Op : uint8_t {
    OP_SYNC = 1, OP_SEND = 49, OP_MATH = 56, OP_ADD = 64, OP_MUL = 65, OP_FRC = 67,
    OP_MAD = 91, OP_MOV = 97, OP_SEL = 98, OP_NOT = 100, OP_AND = 101, OP_OR = 102,
    OP_XOR = 103, OP_SHR = 104, OP_SHL = 105, OP_ASR = 108, OP_CMP = 112,
};
enum Cond : uint8_t { C_NONE = 0, C_Z = 1, C_NZ = 2, C_G = 3, C_GE = 4, C_L = 5, C_LE = 6 };

struct Inst { uint64_t lo = 0, hi = 0; };

static inline void setBits(Inst &i, unsigned hiBit, unsigned loBit, uint64_t v) {
    unsigned n = hiBit - loBit + 1;
    uint64_t mask = (n >= 64) ? ~0ULL : ((1ULL << n) - 1);
    v &= mask;
    if (loBit >= 64) {
        i.hi = (i.hi & ~(mask << (loBit - 64))) | (v << (loBit - 64));
    } else if (hiBit < 64) {
        i.lo = (i.lo & ~(mask << loBit)) | (v << loBit);
    } else {                                        // field crosses bit 64
        unsigned nlo = 64 - loBit;
        uint64_t mlo = (1ULL << nlo) - 1;
        i.lo = (i.lo & ~(mlo << loBit)) | ((v & mlo) << loBit);
        uint64_t mhi = (1ULL << (n - nlo)) - 1;
        i.hi = (i.hi & ~mhi) | ((v >> nlo) & mhi);
    }
}

// operand
enum File : uint8_t { ARF_NULL = 0, GRF = 1, IMM = 2 };
struct Opnd {
    File file = GRF;
    uint8_t nr = 0, sub = 0;            // register, byte offset inside it
    Type type = UD;
    uint8_t vs = 8, w = 8, hs = 1;      // region <vs;w,hs> (sources); dst uses hs
    bool neg = false, abs = false;
    uint32_t imm = 0;
};
static inline Opnd reg(uint8_t nr, Type t) { Opnd o; o.nr = nr; o.type = t; return o; }
static inline Opnd scal(uint8_t nr, uint8_t dword, Type t) {     // nr.dword<0,1,0>
    Opnd o; o.nr = nr; o.sub = dword * 4; o.type = t; o.vs = 0; o.w = 1; o.hs = 0; return o;
}
static inline Opnd imm(uint32_t v, Type t) { Opnd o; o.file = IMM; o.type = t; o.imm = v; return o; }
static inline Opnd nullr(Type t) { Opnd o; o.file = ARF_NULL; o.type = t; return o; }

static inline unsigned logEnc(unsigned v) { unsigned r = 0; while ((1u << r) < v) r++; return r; }
static inline unsigned vsEnc(unsigned v) { return v == 0 ? 0 : logEnc(v) + 1; }
static inline unsigned hsEnc(unsigned v) { return v == 0 ? 0 : logEnc(v) + 1; }

struct Ctl {
    unsigned exec = 8;                  // 1, 8
    bool weAll = false;
    uint8_t swsb = 0;                   // raw SWSB byte (see swsb* below)
    uint8_t pred = 0;                   // 0 none, 1 normal
    bool predInv = false;
    uint8_t flag = 0;                   // flag subregister: 0 = f0.0, 2 = f1.0 (reg<<1 | sub)
    Cond cond = C_NONE;
    bool sat = false;
};
static inline uint8_t swsbDist(unsigned n) { return (uint8_t)(n & 7); }          // @n
static inline uint8_t swsbSet(unsigned sbid) { return (uint8_t)(0x40 | sbid); }   // $n
static inline uint8_t swsbDst(unsigned sbid) { return (uint8_t)(0x20 | sbid); }   // $n.dst
static inline uint8_t swsbSrc(unsigned sbid) { return (uint8_t)(0x30 | sbid); }   // $n.src
static inline uint8_t swsbDistSet(unsigned n, unsigned sbid) { return (uint8_t)(0x80 | (n << 4) | sbid); }  // @n $sbid

static inline void encCommon(Inst &i, Op op, const Ctl &c) {
    setBits(i, 6, 0, op);
    setBits(i, 15, 8, c.swsb);
    setBits(i, 18, 16, logEnc(c.exec));
    setBits(i, 23, 23, c.flag >> 1);
    setBits(i, 22, 22, c.flag & 1);
    setBits(i, 27, 24, c.pred);
    setBits(i, 28, 28, c.predInv);
    setBits(i, 31, 31, c.weAll);
    setBits(i, 34, 34, c.sat);
    setBits(i, 95, 92, c.cond);
}
static inline void encDst(Inst &i, const Opnd &d) {
    setBits(i, 39, 36, d.type);
    setBits(i, 50, 50, d.file == GRF);
    setBits(i, 63, 56, d.file == GRF ? d.nr : 0);
    setBits(i, 55, 51, d.file == GRF ? d.sub : 0);
    setBits(i, 49, 48, hsEnc(d.hs ? d.hs : 1));
}
static inline void encSrc0(Inst &i, const Opnd &s) {
    setBits(i, 43, 40, s.type);
    if (s.file == IMM) {
        setBits(i, 46, 46, 1);
        uint32_t v = s.imm;
        if (s.type == UW || s.type == W) v = (v & 0xFFFF) | (v << 16);
        setBits(i, 127, 96, v);
        return;
    }
    setBits(i, 66, 66, s.file == GRF);
    setBits(i, 79, 72, s.file == GRF ? s.nr : 0);
    setBits(i, 71, 67, s.sub);
    setBits(i, 87, 84, vsEnc(s.vs));
    setBits(i, 83, 81, logEnc(s.w));
    setBits(i, 65, 64, hsEnc(s.hs));
    setBits(i, 44, 44, s.abs);
    setBits(i, 45, 45, s.neg);
}
static inline void encSrc1(Inst &i, const Opnd &s) {
    setBits(i, 91, 88, s.type);
    if (s.file == IMM) {
        setBits(i, 47, 47, 1);
        uint32_t v = s.imm;
        if (s.type == UW || s.type == W) v = (v & 0xFFFF) | (v << 16);   // 16-bit immediates are replicated
        setBits(i, 127, 96, v);
        return;
    }
    setBits(i, 98, 98, s.file == GRF);
    setBits(i, 111, 104, s.file == GRF ? s.nr : 0);
    setBits(i, 103, 99, s.sub);
    setBits(i, 119, 116, vsEnc(s.vs));
    setBits(i, 115, 113, logEnc(s.w));
    setBits(i, 97, 96, hsEnc(s.hs));
    setBits(i, 120, 120, s.abs);
    setBits(i, 121, 121, s.neg);
}

static inline Inst alu1(Op op, const Ctl &c, const Opnd &dst, const Opnd &s0) {
    Inst i; encCommon(i, op, c); encDst(i, dst); encSrc0(i, s0); return i;
}
static inline Inst alu2(Op op, const Ctl &c, const Opnd &dst, const Opnd &s0, const Opnd &s1) {
    Inst i; encCommon(i, op, c); encDst(i, dst); encSrc0(i, s0); encSrc1(i, s1); return i;
}

// ---- sends (Gfx12 send layout) ---------------------------------------------
// Templates assembled by Mesa brw_asm (tools/xec/test_encoder.py checks them);
// dst [63:56], src0 [79:72], src1 [111:104], SWSB, predicate patched in.
static inline Inst sendPatch(Inst i, int dst, int src0, int src1, uint8_t swsb, bool pred, uint8_t flag) {
    if (dst >= 0)  setBits(i, 63, 56, (unsigned)dst);
    if (src0 >= 0) setBits(i, 79, 72, (unsigned)src0);
    if (src1 >= 0) setBits(i, 111, 104, (unsigned)src1);
    setBits(i, 15, 8, swsb);
    if (pred) { setBits(i, 27, 24, 1); setBits(i, 23, 23, flag >> 1); setBits(i, 22, 22, flag & 1); }
    return i;
}
// untyped surface read, surface 253 (stateless A32), SIMD8, 1 channel: mlen 1 rlen 1
static inline Inst sendRead1(uint8_t dst, uint8_t addr, uint8_t swsb, bool pred, uint8_t flag) {
    Inst t; t.lo = 0x080c000000030031ULL; t.hi = 0x1a0000cdfa060cULL;
    return sendPatch(t, dst, addr, -1, swsb, pred, flag);
}
// untyped surface write, surface 253, SIMD8, 1 channel: mlen 1 (address) ex_mlen 1 (data)
static inline Inst sendWrite1(uint8_t addr, uint8_t data, uint8_t swsb, bool pred, uint8_t flag) {
    Inst t; t.lo = 0x30031ULL; t.hi = 0x9a090ccdfa070cULL;
    return sendPatch(t, -1, addr, data, swsb, pred, flag);
}
// end of thread (thread spawner message), payload g127 = copy of g0
static inline Inst sendEOT(uint8_t swsb) {
    Inst t; t.lo = 0x480030031ULL; t.hi = 0x70007f0cULL;
    return sendPatch(t, -1, -1, -1, swsb, false, 0);
}
// sync nop (WE_all), used to wait on a send token: swsbDst(sbid)
static inline Inst syncNop(uint8_t swsb) {
    Inst t; t.lo = 0x20080030001ULL; t.hi = 0;
    setBits(t, 15, 8, swsb);
    return t;
}

// ---- general data-port sends (milestone 3e) -----------------------------------
// Gfx12 scatters the 32-bit message descriptor and extended descriptor over
// the instruction (Mesa brw_eu_inst.h: brw_eu_inst_set_send_desc / _ex_desc).
enum Sfid : uint8_t { SFID_SAMPLER = 2, SFID_GATEWAY = 3, SFID_RENDER = 5, SFID_URB = 6, SFID_HDC0 = 10, SFID_HDC1 = 12 };
static inline Inst sendMsg(Sfid sfid, uint32_t desc, uint32_t exdesc, int dst, uint8_t src0, int src1,
                           uint8_t swsb, bool pred, uint8_t flag) {
    Inst i;
    if (src1 >= 0) { i.lo = 0x30031ULL; i.hi = 0x9a090ccdfa070cULL; }        // write template
    else { i.lo = 0x080c000000030031ULL; i.hi = 0x1a0000cdfa060cULL; }       // read template
    // clear descriptor, extended descriptor and SFID fields
    setBits(i, 123, 122, 0); setBits(i, 71, 67, 0); setBits(i, 55, 51, 0); setBits(i, 121, 113, 0); setBits(i, 91, 81, 0);
    setBits(i, 127, 124, 0); setBits(i, 97, 96, 0); setBits(i, 65, 64, 0); setBits(i, 47, 35, 0); setBits(i, 103, 99, 0);
    setBits(i, 95, 92, 0);
    // descriptor
    setBits(i, 123, 122, desc >> 30); setBits(i, 71, 67, desc >> 25); setBits(i, 55, 51, desc >> 20);
    setBits(i, 121, 113, desc >> 11); setBits(i, 91, 81, desc);
    // extended descriptor (bits 10:6 = ex_mlen)
    setBits(i, 127, 124, exdesc >> 28); setBits(i, 97, 96, exdesc >> 26); setBits(i, 65, 64, exdesc >> 24);
    setBits(i, 47, 35, exdesc >> 11); setBits(i, 103, 99, exdesc >> 6);
    setBits(i, 95, 92, sfid);
    setBits(i, 50, 50, dst >= 0);
    setBits(i, 63, 56, dst >= 0 ? (unsigned)dst : 0);
    return sendPatch(i, -1, src0, src1, swsb, pred, flag);
}
// surfaces: 253 = stateless A32 (device/constant memory: GPU virtual addresses),
// 254 = shared local memory (threadgroup memory: offsets inside the group's SLM)
enum : uint32_t { BTI_A32 = 0xFD, BTI_SLM = 0xFE };
// untyped surface read/write of n (1..4) consecutive dwords per lane, SIMD8
static inline uint32_t descUntyped(bool write, unsigned n, uint32_t bti = BTI_A32) {
    uint32_t mask = (~((1u << n) - 1)) & 0xF;                // channel mask = channels NOT wanted
    return (1u << 25) | ((write ? 0 : n) << 20) | ((write ? 9u : 1u) << 14) | ((0x20u | mask) << 8) | bti;
}
// byte scattered read/write of 1, 2 or 4 bytes per lane (in the low bits of a dword), SIMD8
static inline uint32_t descByte(bool write, unsigned bytes, uint32_t bti = BTI_A32) {
    uint32_t ds = bytes == 1 ? 0 : bytes == 2 ? 1 : 2;
    return (1u << 25) | ((write ? 0 : 1u) << 20) | ((write ? 0xCu : 0x4u) << 14) | ((ds << 2) << 8) | bti;
}
// ---- milestone 3f: atomics, fences, barriers (Mesa brw_eu.h / brw_from_nir.cpp) ----
enum AtomicOp : uint8_t { AOP_AND = 1, AOP_OR = 2, AOP_XOR = 3, AOP_MOV = 4, AOP_INC = 5, AOP_DEC = 6,
                          AOP_ADD = 7, AOP_SUB = 8, AOP_IMAX = 10, AOP_IMIN = 11, AOP_UMAX = 12,
                          AOP_UMIN = 13, AOP_CMPWR = 14 };
// untyped atomic (HDC1 message 2), SIMD8, returns the old value (rlen 1);
// payload: address (mlen 1) + 1 data register (2 for CMPWR: compare, new value)
static inline uint32_t descAtomic(AtomicOp op, uint32_t bti = BTI_A32) {
    return (1u << 25) | (1u << 20) | (2u << 14) | ((uint32_t)(op | 0x10 | 0x20) << 8) | bti;
}
// data cache memory fence with commit (HDC0 message 7, header = g0, rlen 1):
// bti 254 = SLM fence, 0 = everything else
static inline uint32_t descFence(uint32_t bti) {
    return (1u << 25) | (1u << 20) | (1u << 19) | (7u << 14) | (1u << 13) | bti;
}
// gateway barrier message (mlen 1: header with the barrier id in dword 2)
static const uint32_t DESC_BARRIER = 0x02000004u;
// sends that ignore the execution mask (WE_all): fences and barriers
static inline Inst sendNoMask(Sfid sfid, uint32_t desc, int dst, uint8_t src0, uint8_t swsb) {
    Inst i = sendMsg(sfid, desc, 0, dst, src0, -1, swsb, false, 0);
    setBits(i, 31, 31, 1);                                   // WE_all (bit 34 would be EOT)
    return i;
}
// ---- milestone 5b: render stages --------------------------------------------------
// end of thread on a message (URB write / render target write): payload must be in g112+
static inline Inst withEOT(Inst i) { setBits(i, 34, 34, 1); return i; }
// sendc: waits for the pixel scoreboard (render target writes)
static inline Inst asSendc(Inst i) { setBits(i, 6, 0, 50); return i; }
// URB SIMD8 write, header (URB handles) + data split: mlen 1, ex_mlen = data registers
static inline uint32_t descUrbWrite(unsigned slotOffset) {
    return (1u << 25) | (1u << 19) | ((slotOffset & 0x7FF) << 4) | 7u;    // Mesa brw_urb_desc, SIMD8 write
}
// render target write, SIMD8 single source, headerless (Gfx11+): 4 data registers
// mlen 4 (colour), 5 with the source depth after it (3DSTATE_PS_EXTRA computed depth tells the hardware)
static inline uint32_t descRTWrite(unsigned bti, bool last, unsigned mlen = 4) {
    return ((mlen & 15u) << 25) | (12u << 14) | ((last ? 1u : 0u) << 12) | (4u << 8) | (bti & 0xFF);
}

// ---- milestone 4: textures (Mesa brw_eu.h brw_sampler_desc / brw_dp_typed_surface_rw_desc) ----
enum SamplerMsg : uint32_t { SMP_SAMPLE = 0, SMP_SAMPLE_B = 1, SMP_SAMPLE_L = 2, SMP_SAMPLE_C = 3, SMP_SAMPLE_LZ = 24, SMP_SAMPLE_C_LZ = 25, SMP_LD_LZ = 26, SMP_RESINFO = 10 };
// SIMD8, 32-bit return; no header (sampler index < 16, no offsets)
static inline uint32_t descSampler(SamplerMsg m, unsigned mlen, unsigned rlen, unsigned bti, unsigned sampler) {
    return (mlen << 25) | (rlen << 20) | (1u << 17) | ((uint32_t)m << 12) | ((sampler & 15) << 8) | (bti & 0xFF);
}
// typed surface read (msg 5) / write (msg 13), HDC1, SIMD8 low slot group, 4 channels;
// payload: u, v[, r] then (write) 4 data registers
static inline uint32_t descTyped(bool write, unsigned coords, unsigned bti) {
    unsigned mlen = coords + (write ? 4 : 0);
    return (mlen << 25) | ((write ? 0u : 4u) << 20) | ((write ? 13u : 5u) << 14) | (0x10u << 8) | (bti & 0xFF);
}

// sync bar: wait until every thread of the group has sent its barrier message
static inline Inst syncBar() {
    Inst t; t.lo = 0x20080030001ULL; t.hi = 0xe0000000ULL;
    return t;
}
// math unit (Gfx12: out of order, waited for with an SBID like a send)
enum MathFn : uint8_t { M_INV = 1, M_LOG = 2, M_EXP = 3, M_SQRT = 4, M_RSQ = 5, M_SIN = 6, M_COS = 7,
                        M_POW = 10 /* not on Gfx12: hangs the GPU, never emit */, M_IDIV = 12, M_IREM = 13 };

// ---- structured control flow (Gfx12), milestone 3d ------------------------
// Templates from Mesa brw_asm; JIP = bits [127:96], UIP = bits [95:64], both
// in bytes relative to the instruction (Mesa brw_eu_emit.c sets them the same way).
enum CfOp { CF_IF, CF_ELSE, CF_ENDIF, CF_BREAK, CF_WHILE };
static inline Inst cfInst(CfOp op, bool pred, bool inv, uint8_t flag, uint8_t swsb, int32_t jip, int32_t uip) {
    static const uint64_t tmpl[] = { 0x1c06000030022ULL, 0x1c06000030024ULL, 0x460000030025ULL,
                                     0x1c66000030028ULL, 0x1406000030027ULL };
    Inst i; i.lo = tmpl[op]; i.hi = 0;
    setBits(i, 15, 8, swsb);
    if (pred) {
        setBits(i, 27, 24, 1); setBits(i, 28, 28, inv);
        setBits(i, 23, 23, flag >> 1); setBits(i, 22, 22, flag & 1);
    }
    setBits(i, 127, 96, (uint32_t)jip);
    setBits(i, 95, 64, (uint32_t)uip);
    return i;
}
static inline void cfSetJip(Inst &i, int32_t jip) { setBits(i, 127, 96, (uint32_t)jip); }
static inline void cfSetUip(Inst &i, int32_t uip) { setBits(i, 95, 64, (uint32_t)uip); }
static inline unsigned opcodeOf(const Inst &i) { return (unsigned)(i.lo & 0x7f); }
enum { HW_IF = 34, HW_ELSE = 36, HW_ENDIF = 37, HW_BREAK = 40, HW_WHILE = 39 };

} // namespace xe

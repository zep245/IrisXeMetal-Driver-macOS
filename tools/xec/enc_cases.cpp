// prints our encoder's output for the cases listed in test_encoder.py (same order)
#include "../../src/compiler/xe_enc.h"
#include <stdio.h>
#include <initializer_list>
using namespace xe;
static void out(Inst i) { printf("%016llx%016llx\n", (unsigned long long)i.hi, (unsigned long long)i.lo); }
int main() {
    Ctl c; c.swsb = swsbDist(1);
    out(alu2(OP_ADD, c, reg(10, UD), reg(20, UD), reg(30, UD)));
    out(alu2(OP_ADD, c, reg(10, UD), reg(20, UD), imm(0x12345678, UD)));
    out(alu2(OP_ADD, c, reg(10, UD), scal(20, 3, UD), reg(30, UD)));
    { Opnd n = reg(30, D); n.neg = true; out(alu2(OP_ADD, c, reg(10, D), reg(20, D), n)); }
    out(alu2(OP_ADD, c, reg(10, F), reg(20, F), reg(30, F)));
    out(alu2(OP_MUL, c, reg(10, F), reg(20, F), scal(1, 2, F)));
    out(alu1(OP_MOV, c, reg(10, F), reg(20, D)));
    out(alu1(OP_MOV, c, reg(10, D), reg(20, F)));
    out(alu1(OP_MOV, c, reg(10, F), imm(0x3f800000, F)));
    { Ctl k = c; k.cond = C_L; k.flag = 0; out(alu2(OP_CMP, k, reg(10, D), reg(20, D), reg(30, D))); }
    { Ctl k = c; k.cond = C_GE; k.flag = 2; out(alu2(OP_CMP, k, nullr(UD), reg(20, UD), scal(1, 6, UD))); }
    { Ctl k = c; k.cond = C_NZ; out(alu1(OP_MOV, k, nullr(D), reg(20, D))); }
    { Ctl k = c; k.pred = 1; out(alu2(OP_SEL, k, reg(10, UD), reg(20, UD), reg(30, UD))); }
    { Ctl k = c; k.pred = 1; k.predInv = true; out(alu2(OP_SEL, k, reg(10, F), reg(20, F), imm(0x40000000, F))); }
    out(alu2(OP_SHL, c, reg(10, UD), reg(20, UD), imm(2, UD)));
    out(alu2(OP_ASR, c, reg(10, D), reg(20, D), reg(30, D)));
    out(alu2(OP_AND, c, reg(10, UD), reg(20, UD), imm(1, UD)));
    out(alu2(OP_MUL, c, reg(10, D), reg(20, D), imm(12, UW)));
    { Opnd lo = reg(30, UW); lo.vs = 16; lo.w = 8; lo.hs = 2; out(alu2(OP_MUL, c, reg(10, D), reg(20, D), lo)); }
    { Opnd hi = reg(30, UW); hi.sub = 2; hi.vs = 16; hi.w = 8; hi.hs = 2; out(alu2(OP_MUL, c, reg(11, D), reg(20, D), hi)); }
    { Opnd d = reg(10, UW); d.sub = 2; d.hs = 2; Opnd a = reg(10, UW); a.sub = 2; a.vs = 16; a.w = 8; a.hs = 2;
      Opnd b = reg(11, UW); b.vs = 16; b.w = 8; b.hs = 2; out(alu2(OP_ADD, c, d, a, b)); }
    { Ctl k; k.weAll = true; k.exec = 1; k.swsb = swsbDist(1); out(alu2(OP_MUL, k, reg(5, UD), scal(0, 1, UD), [](){ Opnd o = scal(1, 0, UW); o.sub = 28; return o; }())); }
    { Ctl k; k.weAll = true; out(alu1(OP_MOV, k, reg(126, UW), imm(0x76543210, V))); }
    { Ctl k; k.weAll = true; k.swsb = swsbDist(1); Opnd s = reg(126, UW); out(alu1(OP_MOV, k, reg(3, UD), s)); }
    { Opnd a = reg(20, F); a.abs = true; out(alu2(OP_ADD, c, reg(10, F), reg(20, F), a)); }
    { Ctl k = c; k.sat = true; out(alu1(OP_MOV, k, reg(10, F), reg(20, F))); }
    out(sendRead1(8, 6, swsbDistSet(2, 0), true, 2));
    out(sendWrite1(7, 9, swsbDistSet(1, 1), true, 2));
    out(syncNop(swsbDst(1)));
    out(sendEOT(swsbDist(1)));
    out(alu1(OP_NOT, c, reg(10, UD), reg(20, UD)));
    out(alu2(OP_XOR, c, reg(10, UD), reg(20, UD), reg(30, UD)));
    // general sends (3e)
    for (unsigned n = 1; n <= 4; n++) out(sendMsg(SFID_HDC1, descUntyped(false, n), 0, 8, 6, -1, swsbDistSet(1, 0), true, 2));
    for (unsigned n = 1; n <= 4; n++) out(sendMsg(SFID_HDC1, descUntyped(true, n), n << 6, -1, 6, 9, swsbDistSet(1, 0), true, 2));
    for (unsigned b : {1u, 2u, 4u}) out(sendMsg(SFID_HDC0, descByte(false, b), 0, 8, 6, -1, swsbDistSet(1, 0), true, 2));
    for (unsigned b : {1u, 2u, 4u}) out(sendMsg(SFID_HDC0, descByte(true, b), 1 << 6, -1, 6, 9, swsbDistSet(1, 0), true, 2));
    // math (3e)
    { Ctl k = c; k.cond = (Cond)M_INV; k.swsb = swsbDistSet(1, 0); out(alu2(OP_MATH, k, reg(10, F), reg(20, F), nullr(F))); }
    { Ctl k = c; k.cond = (Cond)M_POW; k.swsb = swsbDistSet(1, 0); out(alu2(OP_MATH, k, reg(10, F), reg(20, F), reg(30, F))); }
    { Ctl k = c; k.cond = (Cond)M_IDIV; k.swsb = swsbDistSet(1, 0); out(alu2(OP_MATH, k, reg(10, D), reg(20, D), reg(30, D))); }
    { Ctl k = c; k.cond = (Cond)M_IREM; k.swsb = swsbDistSet(1, 0); out(alu2(OP_MATH, k, reg(10, UD), reg(20, UD), reg(30, UD))); }
    // rounding, bits, 8/16-bit and half moves (3e)
    out(alu1((Op)69, c, reg(10, F), reg(20, F)));
    out(alu1((Op)70, c, reg(10, F), reg(20, F)));
    out(alu1((Op)71, c, reg(10, F), reg(20, F)));
    out(alu1(OP_FRC, c, reg(10, F), reg(20, F)));
    out(alu1((Op)77, c, reg(10, UD), reg(20, UD)));
    out(alu1((Op)74, c, reg(10, UD), reg(20, UD)));
    out(alu1((Op)76, c, reg(10, UD), reg(20, UD)));
    out(alu1((Op)119, c, reg(10, UD), reg(20, UD)));
    out(alu1(OP_MOV, c, reg(10, HF), reg(20, F)));
    { Opnd h = reg(20, HF); h.vs = 16; h.w = 8; h.hs = 2; out(alu1(OP_MOV, c, reg(10, F), h)); }
    { Opnd d = reg(10, HF); d.hs = 2; out(alu1(OP_MOV, c, d, reg(20, F))); }
    { Opnd b = reg(20, UB); b.vs = 32; b.w = 8; b.hs = 4; out(alu1(OP_MOV, c, reg(10, UD), b)); }
    { Opnd b = reg(20, B); b.vs = 32; b.w = 8; b.hs = 4; out(alu1(OP_MOV, c, reg(10, D), b)); }
    { Opnd w = reg(20, UW); w.vs = 16; w.w = 8; w.hs = 2; out(alu1(OP_MOV, c, reg(10, UD), w)); }
    { Opnd w = reg(20, W); w.vs = 16; w.w = 8; w.hs = 2; out(alu1(OP_MOV, c, reg(10, D), w)); }
    // 3f: SLM, atomics, fences, barrier
    out(sendMsg(SFID_HDC1, descUntyped(false, 2, BTI_SLM), 0, 8, 6, -1, swsbDistSet(1, 0), true, 2));
    out(sendMsg(SFID_HDC1, descUntyped(true, 1, BTI_SLM), 1 << 6, -1, 6, 9, swsbDistSet(1, 0), true, 2));
    out(sendMsg(SFID_HDC0, descByte(false, 1, BTI_SLM), 0, 8, 6, -1, swsbDistSet(1, 0), true, 2));
    out(sendMsg(SFID_HDC1, descAtomic(AOP_ADD), 1 << 6, 8, 6, 9, swsbDistSet(1, 0), true, 2));
    out(sendMsg(SFID_HDC1, descAtomic(AOP_UMAX, BTI_SLM), 1 << 6, 8, 6, 9, swsbDistSet(1, 0), true, 2));
    out(sendMsg(SFID_HDC1, descAtomic(AOP_CMPWR), 2 << 6, 8, 6, 9, swsbDistSet(1, 0), true, 2));
    out(sendNoMask(SFID_HDC0, descFence(BTI_SLM), 13, 0, swsbDistSet(1, 0)));
    out(sendNoMask(SFID_HDC0, descFence(0), 13, 0, swsbDistSet(1, 0)));
    out(sendNoMask(SFID_GATEWAY, DESC_BARRIER, -1, 12, swsbDistSet(1, 2)));
    out(syncBar());
    { Ctl w = c; w.weAll = true; out(alu1(OP_MOV, w, reg(12, UD), imm(0, UD))); }
    { Ctl w = c; w.weAll = true; w.exec = 1; Opnd d = reg(12, UD); d.sub = 8;
      out(alu2(OP_AND, w, d, scal(0, 2, UD), imm(0x7f000000, UD))); }
    out(syncNop(swsbSrc(2)));
    // milestone 4: sampler and typed surface messages
    out(sendMsg(SFID_SAMPLER, descSampler(SMP_LD_LZ, 2, 4, 0, 0), 0, 20, 12, -1, swsbDistSet(1, 0), true, 2));
    out(sendMsg(SFID_SAMPLER, descSampler(SMP_SAMPLE_LZ, 2, 4, 3, 1), 0, 20, 12, -1, swsbDistSet(1, 0), true, 2));
    out(sendMsg(SFID_SAMPLER, descSampler(SMP_RESINFO, 1, 4, 2, 0), 0, 20, 12, -1, swsbDistSet(1, 0), true, 2));
    out(sendMsg(SFID_HDC1, descTyped(true, 2, 1), 0, -1, 12, -1, swsbDistSet(1, 0), true, 2));
    out(sendMsg(SFID_HDC1, descTyped(true, 3, 0), 0, -1, 12, -1, swsbDistSet(1, 0), true, 2));
    out(sendMsg(SFID_HDC1, descTyped(false, 2, 0), 0, 20, 12, -1, swsbDistSet(1, 0), true, 2));
    // control flow (positions as in the test program in test_encoder.py)
    out(cfInst(CF_IF, true, false, 0, swsbDist(1), 16, 16));
    out(cfInst(CF_ENDIF, false, false, 0, 0, 16, 0));
    out(cfInst(CF_IF, true, false, 2, 0, 16, 16));
    out(cfInst(CF_ELSE, false, false, 0, 0, 32, 32));
    out(cfInst(CF_BREAK, true, true, 0, swsbDist(1), 16, 16));
    out(cfInst(CF_BREAK, false, false, 0, 0, 16, 16));
    out(cfInst(CF_WHILE, false, false, 0, 0, -16, 0));
    return 0;
}

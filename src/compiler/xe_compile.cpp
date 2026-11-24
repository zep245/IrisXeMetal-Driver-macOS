// xe_compile.cpp - IrisXe shader compiler, milestone 3c (first version).
//
// Metal hands a compute function over as AIR: LLVM bitcode with "air.*"
// metadata. We read it with LLVM's own bitcode reader (LLVM-C API, so any
// recent LLVM works) and generate Iris Xe (Gen12) code ourselves:
//
//   * SIMD8: one hardware thread runs 8 Metal threads, one per lane, so every
//     LLVM value lives in one 8 x 32-bit register (g10 and up, never reused).
//   * Registers on entry (our dispatch, IrisXeSpaceABI.h): g0 thread payload
//     (g0.1 = thread group X), g1 = { buffer 0..5 GPU addresses, grid width,
//     threads per group }, g2.0 = first lane of this thread in its group.
//   * Prologue: g3 = lane (0..7), g4 = thread_position_in_grid,
//     g5 = thread_position_in_threadgroup, f1.0 = "lane is inside the grid";
//     every memory access is predicated on f1.0.
//   * Memory: 32-bit loads/stores through the data port (untyped surface
//     read/write, surface 253 = stateless; GPU addresses are below 4 GB).
//   * Dependencies (Gen12 SWSB): every ALU instruction waits for the one
//     before it (@1) and every send is waited for right away. Slow but safe;
//     scheduling comes later.
//
// First version: straight-line kernels (one basic block): integer and float
// arithmetic, compares, select, conversions, getelementptr, 32-bit load/store.
// Control flow (if/else, loops) is the next step.
#include "xe_compile.h"
#include "xe_enc.h"
#include <llvm-c/Core.h>
#include <llvm/IR/Function.h>                 // v0.51.2: inlining helpers (C++ API, TransformUtils)
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>              // v0.61.0: hoistConstLoads
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/Local.h>        // v0.51.5: ConstantFoldTerminator, removeUnreachableBlocks
#include <llvm/Analysis/ConstantFolding.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Dominators.h>          // v0.56.4: sinking samples into the block that uses them
#include <llvm/Analysis/PostDominators.h>
#include <llvm/Analysis/LoopInfo.h>           // v0.62.0: unrolling short loops
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include <llvm/Transforms/Utils/ValueMapper.h>
#include <llvm-c/BitReader.h>
#include <llvm/Config/llvm-config.h>        // LLVM_VERSION_MAJOR (plain C defines)
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

using namespace xe;

namespace {

struct Val {
    enum Kind { NONE, REG, SCALAR, IMM } kind = NONE;
    uint8_t nr = 0, dw = 0;     // register; dword for SCALAR
    Type type = UD;
    uint32_t imm = 0;
};

struct Fail { std::string msg; };

class Compiler {
public:
    std::vector<Inst> code;
    std::map<LLVMValueRef, Val> vals;
    std::map<LLVMValueRef, std::vector<Val>> vvals;   // vector values: one Val per component (3e)
    int curComp = -1;                                // >= 0: compiling component k of a vector op
    // register reuse (v0.29.0): each register counts the values holding it; a
    // value lets go of its registers after its last use (see "register reuse" below)
    int REG_FIRST = 10, REG_END = 124;                  // g10..g123 (g126/g127: prologue / EOT); render stages: see bindRender
    int stage = 0;                                      // 0 compute, XE_STAGE_VERTEX, XE_STAGE_FRAGMENT (milestone 5b)
    uint8_t bufReg = 1;                                 // register holding the buffer addresses
    uint8_t maxReg = 9;                                 // highest register used (for the log)
    int ref[128] = {};
    bool inUse[128] = {};
    std::vector<uint8_t> pending;                       // allocated, not yet owned by a value
    std::map<LLVMValueRef, int> remaining;              // uses (and phi writes) still to compile
    std::map<LLVMValueRef, std::vector<int>> defStack;  // loops around a value's definition
    std::set<LLVMValueRef> released;
    // v0.51.8: constant-buffer loads (and their constant-index pointer chains) are re-emitted at each use
    std::set<LLVMValueRef> lazy;
    std::set<LLVMValueRef> preloaded;                   // v0.62.0: loads already done by blockConstLoads
    size_t blockLoads = 0, blockSends = 0;
    std::vector<LLVMValueRef> rematted;
    bool lazyPtr(LLVMValueRef p, int d = 0) {
        if (d > 6) return false;
        if (LLVMIsAArgument(p)) return LLVMGetPointerAddressSpace(LLVMTypeOf(p)) == 2;
        if (!LLVMIsAInstruction(p)) return false;
        LLVMOpcode op = LLVMGetInstructionOpcode(p);
        if (op == LLVMBitCast || op == LLVMAddrSpaceCast) return lazyPtr(LLVMGetOperand(p, 0), d + 1);
        if (op != LLVMGetElementPtr) return false;
        for (int k = 1; k < LLVMGetNumOperands(p); k++) if (!LLVMIsAConstantInt(LLVMGetOperand(p, k))) return false;
        return lazyPtr(LLVMGetOperand(p, 0), d + 1);
    }
    void findLazy(LLVMValueRef fn) {
        lazy.clear(); rematted.clear();
        // v0.61.0: up to 32 constant reads in the entry block (hoistConstLoads) are read once and kept
        LLVMBasicBlockRef entryB = LLVMGetEntryBasicBlock(fn);
        int entryConst = 0;
        for (LLVMValueRef I = LLVMGetFirstInstruction(entryB); I; I = LLVMGetNextInstruction(I))
            if (LLVMGetInstructionOpcode(I) == LLVMLoad && LLVMGetPointerAddressSpace(LLVMTypeOf(LLVMGetOperand(I, 0))) == 2) entryConst++;
        bool keepEntry = !getenv("XEC_NOOVERLAP") && !getenv("XEC_NOHOIST") && entryConst <= 32 && !spillMode;   // v0.62.2: not when spilling
        for (LLVMBasicBlockRef b = LLVMGetFirstBasicBlock(fn); b; b = LLVMGetNextBasicBlock(b))
            for (LLVMValueRef I = LLVMGetFirstInstruction(b); I; I = LLVMGetNextInstruction(I)) {
                LLVMOpcode op = LLVMGetInstructionOpcode(I);
                if (op == LLVMAlloca) lazy.insert(I);                        // address = scratch base + constant
                else if (op == LLVMLoad && LLVMGetPointerAddressSpace(LLVMTypeOf(LLVMGetOperand(I, 0))) == 2 && lazyPtr(LLVMGetOperand(I, 0)) &&
                         !(keepEntry && b == entryB)) lazy.insert(I);
                else if ((op == LLVMGetElementPtr || op == LLVMBitCast || op == LLVMAddrSpaceCast) && LLVMIsAInstruction(I) && lazyPtr(I)) lazy.insert(I);
            }
    }
    // ---- v0.51.9: register spilling during code generation ---------------------------------
    // When the registers run out, the value used furthest ahead is stored (the region's lanes) in this
    // lane's scratch area and its registers are freed; each later use loads it into temporaries (freed
    // after that step), so a reload never changes which values hold registers. Only values defined in
    // the current region (same if/else side and loop iteration, no jump in between) are chosen: their
    // store then runs whenever they exist (a skipped branch skips both).
    struct Spilled { uint32_t off, size; std::vector<Val> comps; bool vec; };
    bool spillMode = false;
    uint32_t spillReserve = 0, spillBase = 0, spillTop = 0, spillMax = 0, spillCount = 0;
    uint8_t spillAddr = 0;
    std::map<LLVMValueRef, Spilled> spilled;
    std::map<uint32_t, std::vector<uint32_t>> freeSlots;        // by size (bytes per lane)
    std::set<LLVMValueRef> touched;                             // read since the current step began
    std::map<LLVMValueRef, int> layoutPos;
    std::vector<int> regionStack;
    std::map<LLVMValueRef, std::vector<int>> defRegion;
    int regionIds = 0, curPos = 0;
    uint32_t slotAlloc(uint32_t sz) {
        auto &f = freeSlots[sz];
        if (!f.empty()) { uint32_t o = f.back(); f.pop_back(); return o; }
        uint32_t o = spillTop; spillTop += sz;
        if (spillTop > spillMax) spillMax = spillTop;
        return o;
    }
    void dropSpill(LLVMValueRef v) {                            // dead or defined again
        auto s1 = spilled.find(v);
        if (s1 == spilled.end()) return;
        freeSlots[s1->second.size].push_back(s1->second.off);
        spilled.erase(s1);
    }
    void spillSend(bool write, unsigned n, uint8_t data, uint32_t off) {
        Ctl a; a.weAll = true; a.swsb = swsbDist(1);
        emit(alu2(OP_ADD, a, reg(spillAddr, UD), reg(privReg, UD), imm(spillBase + off, UD)));
        // under the current mask: the lanes of the value's region (a lane that left a loop keeps its slot)
        Inst i = sendMsg(SFID_HDC1, descUntyped(write, n, BTI_A32), write ? (n << 6) : 0, write ? -1 : data, spillAddr,
                         write ? data : -1, swsbDistSet(1, 0), false, 0);
        emit(i);
        emit(syncNop(swsbDst(0)));
    }
    int nextUse(LLVMValueRef v) {
        int best = 1 << 30;
        for (LLVMUseRef u = LLVMGetFirstUse(v); u; u = LLVMGetNextUse(u)) {
            LLVMValueRef us = LLVMGetUser(u);
            if (!LLVMIsAInstruction(us)) continue;
            int p = 1 << 30;
            if (LLVMGetInstructionOpcode(us) == LLVMPHI) {          // used at the end of the incoming block
                for (unsigned k = 0; k < LLVMCountIncoming(us); k++)
                    if (LLVMGetIncomingValue(us, k) == v) {
                        auto t = layoutPos.find(LLVMGetBasicBlockTerminator(LLVMGetIncomingBlock(us, k)));
                        if (t != layoutPos.end() && t->second > curPos && t->second < p) p = t->second;
                    }
            } else {
                auto t = layoutPos.find(us);
                if (t == layoutPos.end() || t->second <= curPos) continue;
                p = t->second;
            }
            if (p < best) best = p;
        }
        return best;
    }
    bool evictable(LLVMValueRef v) {
        if (touched.count(v) || lazy.count(v) || released.count(v) || spilled.count(v)) return false;
        if (!LLVMIsAInstruction(v)) return false;
        LLVMTypeKind tk = LLVMGetTypeKind(LLVMTypeOf(v));
        if (tk == LLVMStructTypeKind || tk == LLVMArrayTypeKind) return false;
        if (LLVMGetInstructionOpcode(v) == LLVMAlloca) return false;
        if (LLVMGetInstructionOpcode(v) == LLVMPHI && loops.count(bidx[LLVMGetInstructionParent(v)])) return false;  // written at the back edge
        auto dr = defRegion.find(v);
        if (dr == defRegion.end() || dr->second != regionStack) return false;
        std::vector<uint8_t> rg = regsOf(v);
        if (rg.empty() || rg.size() > 4) return false;
        for (uint8_t r : rg) {
            if (ref[r] != 1) return false;
            if (std::find(pending.begin(), pending.end(), r) != pending.end()) return false;
        }
        return true;
    }
    void evict(LLVMValueRef v) {
        bool vec = vvals.count(v) && !vals.count(v);
        std::vector<Val> comps = vec ? vvals[v] : std::vector<Val>{ vals[v] };
        std::vector<uint8_t> rg;
        for (const Val &c : comps) if (c.kind == Val::REG && c.nr >= REG_FIRST) rg.push_back(c.nr);
        uint32_t sz = 4 * (uint32_t)rg.size(), off = slotAlloc(sz);
        bool contig = true;
        for (size_t k = 1; k < rg.size(); k++) if (rg[k] != rg[0] + k) contig = false;
        if (contig) spillSend(true, (unsigned)rg.size(), rg[0], off);
        else for (size_t k = 0; k < rg.size(); k++) spillSend(true, 1, rg[k], off + 4 * (uint32_t)k);
        spillCount++;
        for (uint8_t r : rg) if (--ref[r] <= 0) { ref[r] = 0; inUse[r] = false; }
        spilled[v] = Spilled{ off, sz, comps, vec };
        vals.erase(v); vvals.erase(v);
    }
    // a spilled value, for this step only: loaded into temporaries (freed at commit, like remat)
    void reload(LLVMValueRef v) {
        Spilled sp = spilled[v];
        unsigned n = 0;
        for (const Val &c : sp.comps) if (c.kind == Val::REG && c.nr >= REG_FIRST) n++;
        uint8_t d = newRegs(n);
        spillSend(false, n, d, sp.off);
        unsigned k = 0;
        for (Val &c : sp.comps) if (c.kind == Val::REG && c.nr >= REG_FIRST) { c.nr = (uint8_t)(d + k); k++; }
        if (sp.vec) vvals[v] = sp.comps; else vals[v] = sp.comps[0];
        rematted.push_back(v);
    }
    // v0.51.9: an earlier attempt ran out of registers inside this loop: free registers before it,
    // values the loop doesn't use first (hint level n: until 16 n registers are free)
    std::map<int, int> loopHint, loopFail;
    std::vector<int> loopHdr;
    bool usedIn(LLVMValueRef v, const std::vector<bool> &in) {
        for (LLVMUseRef u = LLVMGetFirstUse(v); u; u = LLVMGetNextUse(u)) {
            LLVMValueRef us = LLVMGetUser(u);
            if (!LLVMIsAInstruction(us)) continue;
            auto bi = bidx.find(LLVMGetInstructionParent(us));
            if (bi != bidx.end() && bi->second < (int)in.size() && in[bi->second]) return true;
        }
        return false;
    }
    void preEvict(int hdr, const std::vector<bool> &in) {
        auto h = loopHint.find(hdr);
        if (!spillMode || h == loopHint.end()) return;
        touched.clear();
        int want = 16 * h->second;
        std::vector<std::pair<std::pair<int, int>, LLVMValueRef>> cand;
        auto consider = [&](LLVMValueRef v) {
            if (!evictable(v)) return;
            cand.push_back({{usedIn(v, in) ? 1 : 0, -nextUse(v)}, v});
        };
        for (auto &kv : vals) consider(kv.first);
        for (auto &kv : vvals) if (!vals.count(kv.first)) consider(kv.first);
        std::sort(cand.begin(), cand.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
        for (auto &c : cand) {
            int fr = 0; for (int r = REG_FIRST; r < REG_END; r++) if (!inUse[r]) fr++;
            if (fr >= want) break;
            if (evictable(c.second)) evict(c.second);
        }
        commit();
    }
    std::vector<bool> ifBlocks(LLVMBasicBlockRef bt, LLVMBasicBlockRef bf, LLVMBasicBlockRef mb) {
        std::vector<bool> in(blocks.size(), false);
        std::vector<int> st;
        for (LLVMBasicBlockRef x : { bt, bf }) if (x != mb) st.push_back(bidx[x]);
        int mi = bidx.count(mb) ? bidx[mb] : -1;
        while (!st.empty()) {
            int k = st.back(); st.pop_back();
            if (k == mi || k < 0 || k >= (int)blocks.size() || in[k]) continue;
            in[k] = true;
            for (int sx : succ[k]) st.push_back(sx);
        }
        return in;
    }
    bool isSpilledNow(LLVMValueRef v) { return !spilled.empty() && spilled.count(v) && !vals.count(v) && !vvals.count(v); }
    // free registers by spilling (true if it did)
    bool spillSome() {
        if (!spillMode) return false;
        LLVMValueRef best = nullptr; int bd = -1; size_t bn = 0;
        auto consider = [&](LLVMValueRef v) {
            if (!evictable(v)) return;
            int d = nextUse(v);
            size_t n = regsOf(v).size();
            if (d > bd || (d == bd && n > bn)) { bd = d; best = v; bn = n; }
        };
        for (auto &kv : vals) consider(kv.first);
        for (auto &kv : vvals) if (!vals.count(kv.first)) consider(kv.first);
        if (!best) return false;
        evict(best);
        return true;
    }
    void remat(LLVMValueRef v) {
        if (LLVMGetInstructionOpcode(v) == LLVMLoad) instruction(v);
        else vals[v] = compute(v);
        rematted.push_back(v);
    }
    // v0.51.5: a block reached from two sides of an if (if (a) X else if (b) X) is emitted once per side;
    // a dry walk of region() counts emissions, uses are counted per emission, and a value defined in such a
    // block gets its use count back each time it is defined again
    std::map<LLVMBasicBlockRef, int> emits;
    std::map<std::pair<LLVMBasicBlockRef, LLVMBasicBlockRef>, int> edgeN;
    std::map<LLVMValueRef, int> perDef;                 // uses per definition (non-phi values)
    std::set<LLVMValueRef> defined;
    int traceSteps = 0;
    std::vector<int> loopStack;                         // loops being emitted (ids)
    int loopIds = 0;
    std::map<int, std::vector<LLVMValueRef>> deferred;  // released when that loop ends
    // milestone 3f: threadgroup memory (shared local memory) and barriers
    std::map<LLVMValueRef, uint32_t> slmOff;          // threadgroup arrays -> byte offset in the group's SLM
    uint32_t slmBytes = 0;
    bool usesBarrier = false;
    // v0.29.1: private arrays (alloca) live in a scratch buffer (buffer slot 5):
    // per lane privBytes, at g1.5 + (hardware thread id * 8 + lane) * privBytes;
    // g9 = this lane's base (the hardware thread id comes from sr0)
    std::map<LLVMValueRef, uint32_t> privOff;
    uint32_t privBytes = 0;
    uint8_t privReg = 9;                              // v0.48.0: render stages: a register of its own
    bool usesTextures = false;                        // milestone 4
    std::vector<uint64_t> constSamplers;              // constexpr samplers: slot 7 - k
    std::map<LLVMValueRef, unsigned> constSamplerSlot;
    LLVMModuleRef mod = nullptr;
    LLVMContextRef ctx = nullptr;

    [[noreturn]] void fail(const char *fmt, ...) {
        char b[400];
        va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
        throw Fail{b};
    }
    std::string valName(LLVMValueRef v) {
        char *s = LLVMPrintValueToString(v);
        std::string r = s ? s : "?";
        LLVMDisposeMessage(s);
        while (!r.empty() && (r[0] == ' ')) r.erase(0, 1);
        if (r.size() > 120) r = r.substr(0, 120) + "...";
        return r;
    }

    // ---- emission helpers ----------------------------------------------------
    Ctl ctl() { Ctl c; c.swsb = swsbDist(1); return c; }
    void emit(const Inst &i) { code.push_back(i); }
    uint8_t newReg() { return newRegs(1); }
    // n consecutive free registers (multi-dword loads / stores)
    uint8_t newRegs(unsigned n) {
        for (int r = REG_FIRST; r + (int)n <= REG_END; r++) {
            bool ok = true;
            for (unsigned k = 0; k < n && ok; k++) ok = !inUse[r + k];
            if (!ok) continue;
            for (unsigned k = 0; k < n; k++) { inUse[r + k] = true; pending.push_back((uint8_t)(r + k)); }
            if (r + (int)n - 1 > maxReg) maxReg = (uint8_t)(r + n - 1);
            return (uint8_t)r;
        }
        if (spillSome()) return newRegs(n);                       // v0.51.9
        if (spillMode) for (int h : loopHdr) loopFail[h]++;
        if (getenv("XEC_EV") && spillMode) {
            std::map<std::string, int> why;
            auto chk = [&](LLVMValueRef v) {
                std::vector<uint8_t> rg = regsOf(v); if (rg.empty() || released.count(v)) return;
                std::string w;
                if (touched.count(v)) w = "touched"; else if (lazy.count(v)) w = "lazy";
                else if (!LLVMIsAInstruction(v)) w = "arg";
                else if (LLVMGetTypeKind(LLVMTypeOf(v)) == LLVMStructTypeKind) w = "struct";
                else if (defStack.count(v) && defStack[v] != loopStack) w = "outer-loop(" + std::to_string(defStack[v].size()) + "/" + std::to_string(loopStack.size()) + ")";
                else if (!defStack.count(v)) w = "nodef";
                else if (rg.size() > 4) w = ">4";
                else { for (uint8_t r : rg) { if (ref[r] != 1) { w = "shared-ref" + std::to_string(ref[r]); break; }
                         if (std::find(pending.begin(), pending.end(), r) != pending.end()) { w = "pending"; break; } } }
                if (w.empty()) w = "evictable?";
                why[w] += (int)rg.size();
            };
            for (auto &kv : vals) chk(kv.first);
            for (auto &kv : vvals) if (!vals.count(kv.first)) chk(kv.first);
            for (auto &x : why) fprintf(stderr, "ev: %-20s %d regs\n", x.first.c_str(), x.second);
        }
        if (getenv("XEC_LIVE")) {                                   // debugging: who holds the registers
            std::map<std::string, int> hist;
            auto note = [&](LLVMValueRef v, const std::vector<uint8_t> &r) {
                if (r.empty() || released.count(v)) return;
                std::string k = LLVMIsAInstruction(v) ? std::string("op") + std::to_string((int)LLVMGetInstructionOpcode(v)) : std::string("arg");
                if (LLVMIsACallInst(v)) { LLVMValueRef cf = LLVMGetCalledValue(v); size_t l = 0; const char *nm = LLVMGetValueName2(cf, &l); k += std::string(":") + std::string(nm, l); }
                hist[k] += (int)r.size();
                { auto rm = remaining.find(v); int rv = rm == remaining.end() ? -999 : rm->second;
                  std::string cls = rv >= (1 << 27) ? "keep-forever" : rv <= 0 ? "done-not-freed" : "still-used";
                  bool dfr = false; for (auto &dq : deferred) for (LLVMValueRef x : dq.second) if (x == v) dfr = true;
                  if (dfr) cls = "deferred-loop";
                  hist["~" + cls] += (int)r.size(); }
                if (getenv("XEC_LIVE")[0] == '2') { char *pv = LLVMPrintValueToString(v); { auto rm = remaining.find(v); LLVMBasicBlockRef hb = LLVMIsAInstruction(v) ? LLVMGetInstructionParent(v) : nullptr;
                  fprintf(stderr, "  held [rem %d emits %d def %zu/%zu] %s :: %s\n", rm == remaining.end() ? -999 : rm->second, hb && emits.count(hb) ? emits[hb] : -1,
                          defStack.count(v) ? defStack[v].size() : 99, loopStack.size(), valName(v).c_str(), pv); } LLVMDisposeMessage(pv); }
            };
            for (auto &kv : vals) note(kv.first, regsOf(kv.first));
            for (auto &kv : vvals) if (!vals.count(kv.first)) note(kv.first, regsOf(kv.first));
            for (auto &h : hist) fprintf(stderr, "live: %-50s %d regs\n", h.first.c_str(), h.second);
            std::set<int> acc;
            for (auto &kv : vals) for (uint8_t q : regsOf(kv.first)) if (!released.count(kv.first)) acc.insert(q);
            for (auto &kv : vvals) for (uint8_t q : regsOf(kv.first)) if (!released.count(kv.first)) acc.insert(q);
            int pend = 0, unacc = 0;
            for (int q = REG_FIRST; q < REG_END; q++) { bool pd = std::find(pending.begin(), pending.end(), (uint8_t)q) != pending.end(); if (pd) pend++; if (inUse[q] && !acc.count(q) && !pd) { unacc++; fprintf(stderr, "unaccounted g%d ref=%d\n", q, (int)ref[q]); } }
            fprintf(stderr, "pending=%d unaccounted=%d REG_FIRST=%d curInst=%s\n", pend, unacc, REG_FIRST, curI ? LLVMPrintValueToString(curI) : "?");
        }
        pickSpills();
        fail("kernel too big: more than %d values live at the same time (out of registers; wanted %u contiguous)", REG_END - REG_FIRST, n);
    }
    static Opnd opnd(const Val &v, Type t) {
        switch (v.kind) {
        case Val::IMM:    return imm(v.imm, t);
        case Val::SCALAR: return scal(v.nr, v.dw, t);
        default:          return reg(v.nr, t);
        }
    }
    static Val regVal(uint8_t nr, Type t) { Val v; v.kind = Val::REG; v.nr = nr; v.type = t; return v; }
    // value in a full 8-lane register (copies immediates / scalars)
    Val inReg(const Val &v, Type t) {
        if (v.kind == Val::REG) { Val r = v; r.type = t; return r; }
        uint8_t d = newReg();
        emit(alu1(OP_MOV, ctl(), reg(d, t), opnd(v, t)));
        return regVal(d, t);
    }
    // a value usable as an ALU source 0 (register or scalar region, not immediate)
    Val notImm(const Val &v, Type t) { return v.kind == Val::IMM ? inReg(v, t) : v; }

    // ---- LLVM helpers --------------------------------------------------------
    static bool isVec(LLVMTypeRef t) { return LLVMGetTypeKind(t) == LLVMVectorTypeKind; }
    static LLVMTypeRef scalarTy(LLVMTypeRef t) { return isVec(t) ? LLVMGetElementType(t) : t; }
    static unsigned vecN(LLVMTypeRef t) { return isVec(t) ? LLVMGetVectorSize(t) : 1; }
    static unsigned intBits(LLVMTypeRef t) {
        t = scalarTy(t);
        return LLVMGetTypeKind(t) == LLVMIntegerTypeKind ? LLVMGetIntTypeWidth(t) : 0;
    }
    static bool isHalf(LLVMTypeRef t) { return LLVMGetTypeKind(scalarTy(t)) == LLVMHalfTypeKind; }
    // registers hold 32 bits per lane: i8/i16 zero-extended, half as a float
    // (rounded to half precision after each operation), i64 as its low 32 bits
    Type typeOf(LLVMTypeRef t) {
        t = scalarTy(t);
        switch (LLVMGetTypeKind(t)) {
        case LLVMFloatTypeKind: case LLVMHalfTypeKind: return F;
        case LLVMIntegerTypeKind: {
            unsigned b = LLVMGetIntTypeWidth(t);
            if (b == 1 || b == 8 || b == 16 || b == 32 || b == 64) return UD;
            fail("%u-bit integers are not supported yet", b);
        }
        case LLVMPointerTypeKind: return UD;
        default: {
            char *s = LLVMPrintTypeToString(t);
            std::string n = s; LLVMDisposeMessage(s);
            fail("type %s is not supported yet", n.c_str());
        }
        }
    }
    uint64_t typeAlign(LLVMTypeRef t) {
        switch (LLVMGetTypeKind(t)) {
        case LLVMIntegerTypeKind: { unsigned b = LLVMGetIntTypeWidth(t); return b <= 8 ? 1 : b / 8; }
        case LLVMHalfTypeKind: return 2;
        case LLVMFloatTypeKind: return 4;
        case LLVMDoubleTypeKind: return 8;
        case LLVMPointerTypeKind: return 8;
        case LLVMVectorTypeKind: {             // AIR: vectors aligned to their (power of two) size
            uint64_t s = typeAlign(LLVMGetElementType(t)) * LLVMGetVectorSize(t);
            uint64_t a = 1; while (a < s) a <<= 1;
            return a;
        }
        case LLVMArrayTypeKind: return typeAlign(LLVMGetElementType(t));
        case LLVMStructTypeKind: {
            if (LLVMIsPackedStruct(t)) return 1;
            uint64_t a = 1;
            for (unsigned i = 0; i < LLVMCountStructElementTypes(t); i++) {
                uint64_t e = typeAlign(LLVMStructGetTypeAtIndex(t, i)); if (e > a) a = e;
            }
            return a;
        }
        default: fail("can't lay out a type in memory yet");
        }
    }
    uint64_t typeSize(LLVMTypeRef t) {
        switch (LLVMGetTypeKind(t)) {
        case LLVMVectorTypeKind: return typeAlign(t);
        case LLVMArrayTypeKind: return typeSize(LLVMGetElementType(t)) * LLVMGetArrayLength(t);
        case LLVMStructTypeKind: {
            uint64_t off = 0;
            for (unsigned i = 0; i < LLVMCountStructElementTypes(t); i++) {
                LLVMTypeRef e = LLVMStructGetTypeAtIndex(t, i);
                if (!LLVMIsPackedStruct(t)) { uint64_t a = typeAlign(e); off = (off + a - 1) / a * a; }
                off += typeSize(e);
            }
            uint64_t a = typeAlign(t);
            return (off + a - 1) / a * a;
        }
        default: return typeAlign(t);
        }
    }
    uint64_t structOffset(LLVMTypeRef t, unsigned idx) {
        uint64_t off = 0;
        for (unsigned i = 0; i <= idx; i++) {
            LLVMTypeRef e = LLVMStructGetTypeAtIndex(t, i);
            if (!LLVMIsPackedStruct(t)) { uint64_t a = typeAlign(e); off = (off + a - 1) / a * a; }
            if (i == idx) return off;
            off += typeSize(e);
        }
        return off;
    }

    // address of a threadgroup array, or a constant getelementptr / cast of one
    bool constAddr(LLVMValueRef v, uint64_t &off) {
        if (LLVMIsAGlobalVariable(v)) {
            auto it = slmOff.find(v);
            if (it == slmOff.end()) return false;
            off = it->second;
            return true;
        }
        if (!LLVMIsAConstantExpr(v)) return false;
        LLVMOpcode op = LLVMGetConstOpcode(v);
        if (op == LLVMBitCast || op == LLVMAddrSpaceCast) return constAddr(LLVMGetOperand(v, 0), off);
        if (op != LLVMGetElementPtr) return false;
        uint64_t base = 0;
        if (!constAddr(LLVMGetOperand(v, 0), base)) return false;
        LLVMTypeRef ty = LLVMGetGEPSourceElementType(v);
        int64_t o = 0;
        for (unsigned k = 1; k < (unsigned)LLVMGetNumOperands(v); k++) {
            LLVMValueRef idx = LLVMGetOperand(v, k);
            if (!LLVMIsAConstantInt(idx)) return false;
            int64_t i = LLVMConstIntGetSExtValue(idx);
            if (k == 1) { o += i * (int64_t)typeSize(ty); continue; }
            if (LLVMGetTypeKind(ty) == LLVMStructTypeKind) {
                o += (int64_t)structOffset(ty, (unsigned)i);
                ty = LLVMStructGetTypeAtIndex(ty, (unsigned)i);
            } else if (LLVMGetTypeKind(ty) == LLVMArrayTypeKind || LLVMGetTypeKind(ty) == LLVMVectorTypeKind) {
                ty = LLVMGetElementType(ty);
                o += i * (int64_t)typeSize(ty);
            } else return false;
        }
        off = base + (uint64_t)o;
        return true;
    }
    bool usedIn(LLVMValueRef v, LLVMValueRef fn, int depth) {
        for (LLVMUseRef u = LLVMGetFirstUse(v); u; u = LLVMGetNextUse(u)) {
            LLVMValueRef us = LLVMGetUser(u);
            if (LLVMIsAInstruction(us)) {
                if (LLVMGetBasicBlockParent(LLVMGetInstructionParent(us)) == fn) return true;
            } else if (depth < 8 && LLVMIsAConstant(us) && usedIn(us, fn, depth + 1)) return true;
        }
        return false;
    }
    // threadgroup arrays (address space 3 globals) used by this kernel get SLM offsets
    void allocSLM(LLVMValueRef fn) {
        for (LLVMValueRef g = LLVMGetFirstGlobal(mod); g; g = LLVMGetNextGlobal(g)) {
            if (LLVMGetPointerAddressSpace(LLVMTypeOf(g)) != 3 || !usedIn(g, fn, 0)) continue;
            LLVMTypeRef t = LLVMGlobalGetValueType(g);
            uint64_t al = LLVMGetAlignment(g);
            if (al < typeAlign(t)) al = typeAlign(t);
            if (al < 4) al = 4;
            uint64_t off = (slmBytes + al - 1) / al * al, sz = typeSize(t);
            if (off + sz > 65536) fail("threadgroup memory: more than 64 KB");
            slmOff[g] = (uint32_t)off;
            slmBytes = (uint32_t)(off + sz);
        }
    }

    std::vector<Val> getv(LLVMValueRef v) {
        LLVMTypeRef t = LLVMTypeOf(v);
        if (!isVec(t)) return { get(v) };
        if (isSpilledNow(v)) reload(v);
        if (!lazy.empty() && lazy.count(v) && !vvals.count(v)) remat(v);
        touched.insert(v);
        auto it = vvals.find(v);
        if (it != vvals.end()) {
            if (released.count(v)) fail("internal: %s used after its registers were reused", valName(v).c_str());
            return it->second;
        }
        unsigned n = vecN(t);
        std::vector<Val> r;
        if (LLVMIsAConstantAggregateZero(v) || LLVMIsUndef(v) || LLVMIsPoison(v)) {
            for (unsigned k = 0; k < n; k++) { Val z; z.kind = Val::IMM; z.type = typeOf(t); r.push_back(z); }
            return r;
        }
        if (LLVMIsAConstantDataVector(v) || LLVMIsAConstantVector(v)) {
            for (unsigned k = 0; k < n; k++) {
                LLVMValueRef e = LLVMGetAggregateElement(v, k);
                if (!e) fail("vector constant element %u", k);
                r.push_back(get(e));
            }
            return r;
        }
        fail("vector value not supported yet: %s", valName(v).c_str());
    }
    Val get(LLVMValueRef v) {
        if (isVec(LLVMTypeOf(v))) {
            if (curComp < 0) fail("internal: vector value used as a scalar: %s", valName(v).c_str());
            std::vector<Val> c = getv(v);
            return c[(size_t)curComp < c.size() ? curComp : 0];
        }
        if (isSpilledNow(v)) reload(v);
        if (!lazy.empty() && lazy.count(v) && !vals.count(v)) remat(v);
        touched.insert(v);
        auto it = vals.find(v);
        if (it != vals.end()) {
            if (released.count(v)) fail("internal: %s used after its registers were reused", valName(v).c_str());
            return it->second;
        }
        if (LLVMIsAConstantInt(v)) {
            Val r; r.kind = Val::IMM; r.type = UD;
            unsigned b = intBits(LLVMTypeOf(v));
            long long s = LLVMConstIntGetSExtValue(v);
            if (b == 1) r.imm = s ? 0xFFFFFFFFu : 0;            // booleans are 0 / -1
            else if (b < 32) r.imm = (uint32_t)s & ((1u << b) - 1);   // i8 / i16: zero-extended
            else {
                if (b == 64 && (s > 0xFFFFFFFFLL || s < -0x80000000LL)) fail("64-bit constant %lld is out of range", s);
                r.imm = (uint32_t)s;
            }
            return r;
        }
        if (LLVMIsAConstantFP(v)) {
            LLVMTypeKind fk = LLVMGetTypeKind(LLVMTypeOf(v));
            if (fk != LLVMFloatTypeKind && fk != LLVMHalfTypeKind) fail("only 32-bit and half float constants are supported");
            LLVMBool lose = 0;
            float f = (float)LLVMConstRealGetDouble(v, &lose);
            Val r; r.kind = Val::IMM; r.type = F; memcpy(&r.imm, &f, 4);
            return r;
        }
        if (LLVMIsUndef(v) || LLVMIsPoison(v)) { Val r; r.kind = Val::IMM; r.type = UD; return r; }
        if (LLVMIsNull(v)) { Val r; r.kind = Val::IMM; r.type = UD; return r; }
        uint64_t off = 0;
        if (constAddr(v, off)) return immU((uint32_t)off);
        if (LLVMIsAGlobalVariable(v)) fail("global variable %s: only threadgroup arrays are supported yet", valName(v).c_str());
        fail("value not supported yet: %s", valName(v).c_str());
    }

    // ---- kernel arguments from !air.kernel metadata ----------------------------
    void bindArguments(LLVMValueRef fn) {
        unsigned nk = LLVMGetNamedMetadataNumOperands(mod, "air.kernel");
        if (!nk) fail("no air.kernel metadata: not a Metal compute kernel");
        std::vector<LLVMValueRef> ks(nk);
        LLVMGetNamedMetadataOperands(mod, "air.kernel", ks.data());
        LLVMValueRef km = nullptr;
        for (LLVMValueRef k : ks) {
            unsigned n = LLVMGetMDNodeNumOperands(k);
            if (n < 3) continue;
            std::vector<LLVMValueRef> ops(n);
            LLVMGetMDNodeOperands(k, ops.data());
            if (ops[0] == fn) { km = k; break; }
        }
        if (!km) fail("function is not listed as a kernel in air.kernel");
        std::vector<LLVMValueRef> kops(LLVMGetMDNodeNumOperands(km));
        LLVMGetMDNodeOperands(km, kops.data());
        LLVMValueRef argList = kops[2];
        unsigned na = LLVMGetMDNodeNumOperands(argList);
        std::vector<LLVMValueRef> args(na);
        LLVMGetMDNodeOperands(argList, args.data());
        for (unsigned a = 0; a < na; a++) {
            unsigned n = LLVMGetMDNodeNumOperands(args[a]);
            std::vector<LLVMValueRef> f(n);
            LLVMGetMDNodeOperands(args[a], f.data());
            if (n < 2 || !LLVMIsAConstantInt(f[0])) fail("bad argument metadata");
            unsigned idx = (unsigned)LLVMConstIntGetZExtValue(f[0]);
            unsigned len = 0;
            const char *kind = LLVMGetMDString(f[1], &len);
            std::string k = kind ? std::string(kind, len) : "";
            if (idx >= LLVMCountParams(fn)) fail("argument %u out of range", idx);
            LLVMValueRef p = LLVMGetParam(fn, idx);
            Val v;
            if (k == "air.buffer") {
                unsigned loc = 0; bool found = false;
                for (unsigned i = 2; i + 1 < n; i++) {
                    unsigned l2 = 0; const char *s = LLVMGetMDString(f[i], &l2);
                    if (s && std::string(s, l2) == "air.location_index" && LLVMIsAConstantInt(f[i + 1])) {
                        loc = (unsigned)LLVMConstIntGetZExtValue(f[i + 1]); found = true; break;
                    }
                }
                if (!found) fail("buffer argument %u without a location index", idx);
                if (loc >= 6) fail("buffer index %u: only buffers 0..5 are supported yet", loc);
                if (loc == 5 && privBytes) fail("this kernel has private arrays: buffer index 5 must stay free (scratch)");
                v.kind = Val::SCALAR; v.nr = bufReg; v.dw = (uint8_t)loc; v.type = UD;
            } else if (k == "air.texture" || k == "air.sampler") {
                // milestone 4: texture k = binding table entry k, sampler k = sampler state k
                unsigned loc = 0; bool found = false;
                for (unsigned i = 2; i + 1 < n; i++) {
                    unsigned l2 = 0; const char *s = LLVMGetMDString(f[i], &l2);
                    if (s && std::string(s, l2) == "air.location_index" && LLVMIsAConstantInt(f[i + 1])) {
                        loc = (unsigned)LLVMConstIntGetZExtValue(f[i + 1]); found = true; break;
                    }
                }
                if (!found) fail("%s argument %u without a location index", k.c_str(), idx);
                if (loc >= 8) fail("%s index %u: only 0..7 are supported yet", k == "air.texture" ? "texture" : "sampler", loc);
                v.kind = Val::IMM; v.imm = loc; v.type = UD;
                usesTextures = true;
            } else {
                // built-ins; uint2/uint3 forms
                auto sc = [](uint8_t nr, uint8_t dw) { Val x; x.kind = Val::SCALAR; x.nr = nr; x.dw = dw; x.type = UD; return x; };
                auto im = [](uint32_t i) { Val x; x.kind = Val::IMM; x.imm = i; x.type = UD; return x; };
                std::vector<Val> c;
                // v0.51.17: 2D / 3D thread groups (see prologue)
                if (k == "air.thread_position_in_grid")              c = { regVal(4, UD), regVal(10, UD), regVal(11, UD) };
                else if (k == "air.thread_position_in_threadgroup")  c = { regVal(5, UD), regVal(12, UD), regVal(13, UD) };
                else if (k == "air.thread_index_in_threadgroup")     c = { regVal(14, UD) };
                else if (k == "air.threadgroup_position_in_grid")    c = { sc(0, 1), sc(0, 6), sc(0, 7) };
                else if (k == "air.threads_per_threadgroup")         c = { sc(2, 1), sc(2, 2), sc(2, 5) };
                else if (k == "air.threads_per_grid")                c = { sc(1, 6), sc(2, 3), sc(2, 4) };
                else if (k == "air.thread_index_in_simdgroup")       c = { regVal(3, UD) };
                else if (k == "air.threads_per_simdgroup")           c = { im(8) };
                else fail("kernel argument kind %s is not supported yet", k.c_str());
                LLVMTypeRef pt = LLVMTypeOf(p);
                if (isVec(pt)) {
                    if (vecN(pt) > c.size()) fail("%s as a %u-component vector is not supported yet", k.c_str(), vecN(pt));
                    c.resize(vecN(pt));
                    vvals[p] = c;
                } else vals[p] = c[0];
                continue;
            }
            vals[p] = v;
        }
    }

    // ---- prologue / epilogue ---------------------------------------------------
    void prologue() {
        Ctl we; we.weAll = true;
        emit(alu1(OP_MOV, we, reg(126, UW), imm(0x76543210, V)));             // lane numbers as words
        we.swsb = swsbDist(1);
        emit(alu1(OP_MOV, we, reg(3, UD), reg(126, UW)));                     // g3 = lane
        // v0.51.17 (IrisXeCore ABI 8): thread groups of W x H x D threads. The walker runs a group as
        // W*H*D threads in a row (g2.0 = first one of this hardware thread); per-thread data
        // g2.1..g2.5 = W, H, rows of the grid, slices of the grid, D (the same for every thread).
        // i = lane + g2.0; x = i % W, y = (i / W) % H, z = i / (W H)  (float: exact for i < 2^20)
        //   g14 = i, g5 = x, g12 = y, g13 = z (in the group); g4, g10, g11 = x, y, z in the grid
        Ctl c = ctl();
        auto wsub = [](uint8_t nr, unsigned byte) { Opnd o = scal(nr, 0, UW); o.sub = (uint8_t)byte; return o; };
        emit(alu2(OP_ADD, c, reg(14, UD), reg(3, UD), scal(2, 0, UD)));       // i
        emit(alu1(OP_MOV, c, reg(6, F), reg(14, UD)));
        emit(alu2(OP_ADD, c, reg(6, F), reg(6, F), imm(0x3f000000u, F)));     // i + 0.5
        emit(alu1(OP_MOV, c, reg(7, F), scal(2, 1, UD)));                     // W
        { Ctl k = c; k.cond = (Cond)M_INV; k.swsb = swsbDistSet(1, 1);
          emit(alu2(OP_MATH, k, reg(7, F), reg(7, F), nullr(F))); emit(syncNop(swsbDst(1))); }
        emit(alu2(OP_MUL, c, reg(6, F), reg(6, F), reg(7, F)));
        emit(alu1((Op)69, c, reg(6, F), reg(6, F)));                           // rndd
        emit(alu1(OP_MOV, c, reg(8, UD), reg(6, F)));                         // q = i / W
        emit(alu2(OP_MUL, c, reg(7, UD), reg(8, UD), wsub(2, 4)));            // q W
        { Opnd n7 = reg(7, D); n7.neg = true;
          emit(alu2(OP_ADD, c, reg(5, D), reg(14, D), n7)); }                 // x = i - q W
        emit(alu1(OP_MOV, c, reg(6, F), reg(8, UD)));
        emit(alu2(OP_ADD, c, reg(6, F), reg(6, F), imm(0x3f000000u, F)));     // q + 0.5
        emit(alu1(OP_MOV, c, reg(7, F), scal(2, 2, UD)));                     // H
        { Ctl k = c; k.cond = (Cond)M_INV; k.swsb = swsbDistSet(1, 1);
          emit(alu2(OP_MATH, k, reg(7, F), reg(7, F), nullr(F))); emit(syncNop(swsbDst(1))); }
        emit(alu2(OP_MUL, c, reg(6, F), reg(6, F), reg(7, F)));
        emit(alu1((Op)69, c, reg(6, F), reg(6, F)));
        emit(alu1(OP_MOV, c, reg(13, UD), reg(6, F)));                        // z = q / H
        emit(alu2(OP_MUL, c, reg(7, UD), reg(13, UD), wsub(2, 8)));           // z H
        { Opnd n7 = reg(7, D); n7.neg = true;
          emit(alu2(OP_ADD, c, reg(12, D), reg(8, D), n7)); }                 // y = q - z H
        Ctl s1; s1.weAll = true; s1.exec = 1; s1.swsb = swsbDist(1);
        emit(alu2(OP_MUL, s1, reg(6, UD), scal(0, 1, UD), wsub(2, 4)));       // g6.0 = group x * W
        emit(alu2(OP_ADD, c, reg(4, UD), reg(5, UD), scal(6, 0, UD)));        // grid x
        emit(alu2(OP_MUL, s1, reg(6, UD), scal(0, 6, UD), wsub(2, 8)));       // group y * H
        emit(alu2(OP_ADD, c, reg(10, UD), reg(12, UD), scal(6, 0, UD)));      // grid y
        emit(alu2(OP_MUL, s1, reg(6, UD), scal(0, 7, UD), wsub(2, 20)));      // group z * D
        emit(alu2(OP_ADD, c, reg(11, UD), reg(13, UD), scal(6, 0, UD)));      // grid z
        Ctl f = ctl(); f.cond = C_L; f.flag = 2;
        emit(alu2(OP_CMP, f, nullr(UD), reg(4, UD), scal(1, 6, UD)));         // f1.0 = inside the grid:
        f.pred = 1;                                                            //   x, and y, and z
        emit(alu2(OP_CMP, f, nullr(UD), reg(10, UD), scal(2, 3, UD)));
        emit(alu2(OP_CMP, f, nullr(UD), reg(11, UD), scal(2, 4, UD)));
        if (privBytes) {
            // hardware thread id from sr0 (Gfx12: [2:0] thread, [5:4] EU, [7] EU row, [8] subslice,
            // [10:9] dual subslice, [13:11] slice; bits 3 and 6 unused): packed into 12 bits
            Inst m; m.lo = 0x0a05022080000161ULL; m.hi = 0x7000ULL;          // mov(1) g10 sr0.0 (brw_asm)
            setBits(m, 63, 56, 7);                                             // -> g7
            emit(m);
            emit(alu2(OP_AND, s1, reg(7, UD), scal(7, 0, UD), imm(0xffffffffu, UD)));   // Mesa: next one @1 too
            emit(alu2(OP_AND, s1, reg(8, UD), scal(7, 0, UD), imm(7, UD)));
            Opnd d8 = reg(8, UD); d8.sub = 4;
            emit(alu2(OP_SHR, s1, d8, scal(7, 0, UD), imm(1, UD)));
            emit(alu2(OP_AND, s1, d8, scal(8, 1, UD), imm(0x18, UD)));
            emit(alu2(OP_OR,  s1, reg(8, UD), scal(8, 0, UD), scal(8, 1, UD)));
            emit(alu2(OP_SHR, s1, d8, scal(7, 0, UD), imm(2, UD)));
            emit(alu2(OP_AND, s1, d8, scal(8, 1, UD), imm(0xFE0, UD)));
            emit(alu2(OP_OR,  s1, reg(8, UD), scal(8, 0, UD), scal(8, 1, UD)));   // g8.0 = id (< 4096)
            emit(alu2(OP_MUL, s1, reg(8, UD), scal(8, 0, UD), imm(privBytes * 8, UW)));
            emit(alu2(OP_ADD, s1, reg(8, UD), scal(8, 0, UD), scal(1, 5, UD)));  // + scratch buffer
            Ctl a = ctl(); a.weAll = true;
            emit(alu2(OP_MUL, a, reg(9, UD), reg(3, UD), imm(privBytes, UW)));
            emit(alu2(OP_ADD, a, reg(9, UD), reg(9, UD), scal(8, 0, UD)));       // g9 = this lane's area
        }
    }
    void epilogue() {
        Ctl we; we.weAll = true; we.swsb = swsbDist(1);
        emit(alu1(OP_MOV, we, reg(127, UD), reg(0, UD)));
        emit(sendEOT(swsbDist(1)));
    }

    // ---- instructions ------------------------------------------------------------
    Val binop(Op op, Type t, const Val &a, const Val &b, bool negB = false, bool commutative = true) {
        Val x = a, y = b;
        if (x.kind == Val::IMM && y.kind != Val::IMM && commutative && !negB) std::swap(x, y);
        x = notImm(x, t);
        uint8_t d = newReg();
        Opnd s1 = opnd(y, t);
        if (negB) {
            if (y.kind == Val::IMM) {
                if (t == F) s1.imm ^= 0x80000000u; else s1.imm = (uint32_t)(-(int32_t)s1.imm);
            } else s1.neg = true;
        }
        emit(alu2(op, ctl(), reg(d, t), opnd(x, t), s1));
        return regVal(d, t);
    }
    // 32 x 32 -> low 32 bits: Gen12 has no dword multiply (Mesa does the same)
    Val imul(const Val &a, const Val &b) {
        Val x = a, y = b;
        if (x.kind == Val::IMM && y.kind != Val::IMM) std::swap(x, y);
        if (y.kind == Val::IMM && y.imm <= 0xFFFF) {
            x = notImm(x, D);
            uint8_t d = newReg();
            emit(alu2(OP_MUL, ctl(), reg(d, D), opnd(x, D), imm(y.imm, UW)));
            return regVal(d, UD);
        }
        x = notImm(x, D);
        Val yr = inReg(y, D);
        uint8_t lo = newReg(), hi = newReg();
        Opnd ylo = reg(yr.nr, UW); ylo.vs = 16; ylo.w = 8; ylo.hs = 2;
        Opnd yhi = ylo; yhi.sub = 2;
        emit(alu2(OP_MUL, ctl(), reg(lo, D), opnd(x, D), ylo));
        emit(alu2(OP_MUL, ctl(), reg(hi, D), opnd(x, D), yhi));
        Opnd dst = reg(lo, UW); dst.sub = 2; dst.hs = 2;
        Opnd s0 = reg(lo, UW); s0.sub = 2; s0.vs = 16; s0.w = 8; s0.hs = 2;
        Opnd s1 = reg(hi, UW); s1.vs = 16; s1.w = 8; s1.hs = 2;
        emit(alu2(OP_ADD, ctl(), dst, s0, s1));
        return regVal(lo, UD);
    }
    Val compare(Cond c, Type t, const Val &a, const Val &b) {
        Val x = a, y = b;
        if (x.kind == Val::IMM && y.kind != Val::IMM) {          // swap operands: mirror the condition
            std::swap(x, y);
            c = c == C_L ? C_G : c == C_G ? C_L : c == C_LE ? C_GE : c == C_GE ? C_LE : c;
        }
        x = notImm(x, t);
        uint8_t d = newReg();
        Ctl k = ctl(); k.cond = c; k.flag = 0;
        emit(alu2(OP_CMP, k, reg(d, t), opnd(x, t), opnd(y, t)));
        cmpAt = code.size() - 1; cmpReg = d;                      // v0.51.9: f0.0 already = (d != 0)
        return regVal(d, UD);                                     // 0 / -1
    }
    // atan2(y, x), the usual minimax polynomial for atan on [0, 1] (error ~1e-5), then the octant / quadrant fixes
    Val atan2v(Val y, Val x) {
        Val ay = binop(OP_AND, UD, y, immU(0x7fffffffu)); ay.type = F;
        Val ax = binop(OP_AND, UD, x, immU(0x7fffffffu)); ax.type = F;
        Val mx = binop(OP_ADD, F, ax, immF(0.0f)), mn = mx;
        {   // max / min of |x|, |y|: two selects on one compare
            Val gt = compare(C_G, F, ay, ax);
            mx = selectVal(gt, ay, ax, F);
            mn = selectVal(gt, ax, ay, F);
        }
        Val a = fdiv(mn, selectVal(compare(C_G, F, mx, immF(1e-30f)), mx, immF(1e-30f), F));
        Val s = binop(OP_MUL, F, a, a);
        Val r = binop(OP_ADD, F, binop(OP_MUL, F, s, immF(-0.0464964749f)), immF(0.15931422f));
        r = binop(OP_ADD, F, binop(OP_MUL, F, r, s), immF(-0.327622764f));
        r = binop(OP_ADD, F, binop(OP_MUL, F, binop(OP_MUL, F, r, s), a), a);
        Val steep = compare(C_G, F, ay, ax);
        r = selectVal(steep, binop(OP_ADD, F, binop(OP_MUL, F, r, immF(-1.0f)), immF(1.57079633f)), r, F);
        Val left = compare(C_L, F, x, immF(0.0f));
        r = selectVal(left, binop(OP_ADD, F, binop(OP_MUL, F, r, immF(-1.0f)), immF(3.14159265f)), r, F);
        Val sg = binop(OP_AND, UD, y, immU(0x80000000u));
        Val res = binop(OP_OR, UD, r, sg); res.type = F;
        return res;
    }
    // cond ? a : b (cond is a 0 / -1 value); constants are fine on either side
    Val selectVal(const Val &cond, Val a, Val b, Type t) {
        if (cond.kind == Val::IMM) return cond.imm ? a : b;
        flagFrom(cond);
        bool inv = false;
        if (a.kind == Val::IMM && b.kind != Val::IMM) { std::swap(a, b); inv = true; }
        a = notImm(a, t);
        uint8_t d = newReg();
        Ctl k = ctl(); k.pred = 1; k.predInv = inv; k.flag = 0;
        emit(alu2(OP_SEL, k, reg(d, t), opnd(a, t), opnd(b, t)));
        return regVal(d, t);
    }
    size_t cmpAt = (size_t)-1; uint8_t cmpReg = 0;
    long edgeMoves = 0;
    std::map<std::string, long> edgeWhy;
    void flagFrom(const Val &cond) {                              // f0.0 = cond != 0
        if (cond.kind == Val::IMM) fail("constant select condition (should have been folded)");
        if (cond.kind == Val::REG && cond.nr == cmpReg && cmpAt + 1 == code.size()) return;   // the compare just set it
        Ctl k = ctl(); k.cond = C_NZ; k.flag = 0;
        emit(alu1(OP_MOV, k, nullr(D), opnd(cond, D)));
    }
    Val vecAddr(const Val &a) { return inReg(a, UD); }

    Val gep(LLVMValueRef I) {
        unsigned n = LLVMGetNumOperands(I);
        Val addr = get(LLVMGetOperand(I, 0));
        LLVMTypeRef ty = LLVMGetGEPSourceElementType(I);
        uint64_t constOff = 0;
        for (unsigned k = 1; k < n; k++) {
            LLVMValueRef idx = LLVMGetOperand(I, k);
            uint64_t esz;
            if (k == 1) esz = typeSize(ty);
            else if (LLVMGetTypeKind(ty) == LLVMStructTypeKind) {
                if (!LLVMIsAConstantInt(idx)) fail("non-constant struct index");
                unsigned f = (unsigned)LLVMConstIntGetZExtValue(idx);
                constOff += structOffset(ty, f);
                ty = LLVMStructGetTypeAtIndex(ty, f);
                continue;
            } else if (LLVMGetTypeKind(ty) == LLVMArrayTypeKind || LLVMGetTypeKind(ty) == LLVMVectorTypeKind) {
                ty = LLVMGetElementType(ty);
                esz = typeSize(ty);
            } else fail("getelementptr into this type is not supported");
            if (LLVMIsAConstantInt(idx)) { constOff += (uint64_t)LLVMConstIntGetSExtValue(idx) * esz; continue; }
            Val iv = get(idx), scaled;
            if (esz == 1) scaled = iv;
            else if ((esz & (esz - 1)) == 0) {
                unsigned sh = 0; while ((1ull << sh) < esz) sh++;
                scaled = binop(OP_SHL, UD, notImm(iv, UD), Val{Val::IMM, 0, 0, UD, sh}, false, false);
            } else scaled = imul(iv, Val{Val::IMM, 0, 0, UD, (uint32_t)esz});
            addr = binop(OP_ADD, UD, addr, scaled);
        }
        if (constOff) addr = binop(OP_ADD, UD, addr, Val{Val::IMM, 0, 0, UD, (uint32_t)constOff});
        return addr;
    }

    // ---- helpers for 3e ---------------------------------------------------------
    Val immU(uint32_t v) { Val r; r.kind = Val::IMM; r.type = UD; r.imm = v; return r; }
    Val immF(float f) { Val r; r.kind = Val::IMM; r.type = F; memcpy(&r.imm, &f, 4); return r; }
    Val mov(Type to, const Val &x, Type from) {
        uint8_t d = newReg();
        emit(alu1(OP_MOV, ctl(), reg(d, to), opnd(notImm(x, from), from)));
        return regVal(d, to == F ? F : UD);
    }
    Val unop(Op op, Type t, const Val &x) {
        uint8_t d = newReg();
        emit(alu1(op, ctl(), reg(d, t), opnd(notImm(x, t), t)));
        return regVal(d, t);
    }
    // keep an iN (N < 32) result zero-extended
    Val canon(const Val &v, unsigned bits) {
        if (bits == 0 || bits >= 32 || bits == 1) return v;
        if (v.kind == Val::IMM) return immU(v.imm & ((1u << bits) - 1));
        return binop(OP_AND, UD, v, immU((1u << bits) - 1));
    }
    // sign-extend an iN value (for signed compares, ashr, sdiv, sext, sitofp)
    Val sext(const Val &v, unsigned bits) {
        if (bits == 0 || bits >= 32) return v;
        if (bits == 1) return v;                                   // booleans are already 0 / -1
        if (v.kind == Val::IMM) return immU((uint32_t)((int32_t)(v.imm << (32 - bits)) >> (32 - bits)));
        Val t = binop(OP_SHL, UD, v, immU(32 - bits), false, false);
        return binop(OP_ASR, D, t, immU(32 - bits), false, false);
    }
    // round a float to half precision (half values live in float registers)
    // v0.51.9: Metal's half arithmetic is "fast" math: computed at float precision, no rounding after each
    // operation (a render target / memory store still converts). XEC_EXACTHALF=1: round as before.
    bool exactHalf = getenv("XEC_EXACTHALF") != nullptr;
    Val roundHalf(const Val &v) {
        if (!exactHalf) return v;
        return roundHalfExact(v);
    }
    Val roundHalfExact(const Val &v) {
        Val x = inReg(v, F);
        uint8_t h = newReg(), d = newReg();
        emit(alu1(OP_MOV, ctl(), reg(h, HF), reg(x.nr, F)));
        emit(alu1(OP_MOV, ctl(), reg(d, F), reg(h, HF)));
        return regVal(d, F);
    }
    // math unit (out of order on Gfx12: token $1, waited for right away)
    Val math(MathFn fn, Type t, const Val &a, const Val *b) {
        Val x = inReg(a, t);
        Opnd s1 = nullr(t);
        if (b) s1 = opnd(inReg(*b, t), t);
        uint8_t d = newReg();
        Ctl k = ctl(); k.cond = (Cond)fn; k.swsb = swsbDistSet(1, 1);
        emit(alu2(OP_MATH, k, reg(d, t), reg(x.nr, t), s1));
        emit(syncNop(swsbDst(1)));
        return regVal(d, t);
    }
    Val fdiv(const Val &a, const Val &b) {
        Val r = math(M_INV, F, b, nullptr);
        if (a.kind == Val::IMM && a.imm == 0x3f800000u) return r;
        return binop(OP_MUL, F, a, r);
    }
    Val intDivRem(bool rem, bool sgn, const Val &a, const Val &b, unsigned bits) {
        if (!sgn && b.kind == Val::IMM && b.imm && (b.imm & (b.imm - 1)) == 0) {   // by a power of two
            unsigned sh = 0; while ((1u << sh) < b.imm) sh++;
            return rem ? binop(OP_AND, UD, a, immU(b.imm - 1)) : binop(OP_SHR, UD, a, immU(sh), false, false);
        }
        Type t = sgn ? D : UD;
        Val x = sgn ? sext(a, bits) : a, y = sgn ? sext(b, bits) : b;
        return canon(math(rem ? M_IREM : M_IDIV, t, x, &y), bits);
    }

    // ---- memory -------------------------------------------------------------------
    unsigned addrSpace(LLVMValueRef ptr) { return LLVMGetPointerAddressSpace(LLVMTypeOf(ptr)); }
    unsigned elemBytes(LLVMTypeRef t, const char *what) {
        LLVMTypeRef e = scalarTy(t);
        LLVMTypeKind k = LLVMGetTypeKind(e);
        unsigned bytes = k == LLVMFloatTypeKind ? 4 : k == LLVMHalfTypeKind ? 2 :
                         k == LLVMIntegerTypeKind ? (LLVMGetIntTypeWidth(e) + 7) / 8 : 0;
        if (k == LLVMIntegerTypeKind && LLVMGetIntTypeWidth(e) == 1) bytes = 1;
        if (bytes != 1 && bytes != 2 && bytes != 4) {
            char *s = LLVMPrintTypeToString(t);
            std::string nm = s; LLVMDisposeMessage(s);
            fail("%s of %s: 64-bit memory accesses are not supported yet", what, nm.c_str());
        }
        if (vecN(t) > 4) fail("%s of a vector with more than 4 elements is not supported yet", what);
        return bytes;
    }
    // one send, waited for right away (token $0)
    void sendNow(Sfid sf, uint32_t desc, uint32_t exdesc, int dst, uint8_t addr, int data) {
        emit(sendMsg(sf, desc, exdesc, dst, addr, data, swsbDistSet(1, 0), true, 2));
        emit(syncNop(swsbDst(0)));
    }
    // device (1) and constant (2) memory: stateless surface; threadgroup (3): SLM
    uint32_t btiOf(LLVMValueRef p, const char *what, bool write) {
        unsigned as = addrSpace(p);
        if (as == 3) return BTI_SLM;
        if (as == 1 || (as == 2 && !write)) return BTI_A32;
        if (as == 0 && privBytes) return BTI_A32;             // private arrays: scratch buffer
        fail("%s address space %u is not supported yet", what, as);
    }

    // v0.62.0: constants (address space 2) read at fixed offsets of the same buffer argument in the entry block: one
    // untyped read gives 8 lanes x 4 dwords = 128 contiguous bytes (lane i reads base + start + 16 i), so up to 256 bytes
    // in two messages that overlap, instead of one message per vector, each waited for before the next (WindowServer's
    // colour shaders read 16 such vectors). Every lane gets each constant from a scalar region: no lane depends on its
    // own pixel. Off with XEC_NOOVERLAP or XEC_NOBLOCK.
    struct CLoad { LLVMValueRef I; int64_t off; unsigned n, eb; };
    bool constLoadInfo(LLVMValueRef L, llvm::Argument *&base, CLoad &c) {
        llvm::LoadInst *li = llvm::dyn_cast<llvm::LoadInst>(llvm::unwrap(L));
        if (!li || li->isVolatile() || li->getPointerAddressSpace() != 2) return false;
        LLVMTypeRef t = LLVMTypeOf(L);
        LLVMTypeRef e = scalarTy(t);
        LLVMTypeKind k = LLVMGetTypeKind(e);
        unsigned eb = k == LLVMFloatTypeKind ? 4 : k == LLVMHalfTypeKind ? 2 :
                      (k == LLVMIntegerTypeKind && (LLVMGetIntTypeWidth(e) == 32 || LLVMGetIntTypeWidth(e) == 16)) ? LLVMGetIntTypeWidth(e) / 8 : 0;
        unsigned n = vecN(t);
        if (!eb || n < 1 || n > 4 || li->getAlign().value() < eb) return false;
        const llvm::DataLayout &DL = li->getModule()->getDataLayout();
        llvm::APInt off(DL.getIndexTypeSizeInBits(li->getPointerOperandType()), 0);
        llvm::Value *b = li->getPointerOperand()->stripAndAccumulateConstantOffsets(DL, off, true);
        auto *arg = llvm::dyn_cast<llvm::Argument>(b);
        if (!arg || off.isNegative() || off.getSExtValue() > (1 << 20)) return false;
        if (base && arg != base) return false;
        base = arg;
        c = CLoad{L, off.getSExtValue(), n, eb};
        return true;
    }
    bool blockConstLoads(LLVMValueRef I) {
        if (getenv("XEC_NOOVERLAP") || getenv("XEC_NOBLOCK") || spillMode || !loopStack.empty()) return false;
        LLVMBasicBlockRef bb = LLVMGetInstructionParent(I);
        if (bb != LLVMGetEntryBasicBlock(LLVMGetBasicBlockParent(bb))) return false;
        llvm::Argument *base = nullptr;
        CLoad me;
        if (!constLoadInfo(I, base, me)) return false;
        std::vector<CLoad> cand{me};
        for (LLVMValueRef J = LLVMGetNextInstruction(I); J; J = LLVMGetNextInstruction(J)) {
            if (LLVMGetInstructionOpcode(J) != LLVMLoad || preloaded.count(J) || vals.count(J) || vvals.count(J)) continue;
            CLoad c;
            llvm::Argument *b2 = base;
            if (constLoadInfo(J, b2, c)) cand.push_back(c);
        }
        if (cand.size() < 2) return false;
        int64_t start = me.off;
        for (const CLoad &c : cand) start = std::min(start, c.off);
        start &= ~(int64_t)3;
        std::vector<CLoad> grp;
        int64_t end = start;
        for (const CLoad &c : cand) {
            int64_t e = c.off + (int64_t)(c.n * c.eb);
            if (e - start <= 256) { grp.push_back(c); end = std::max(end, e); }
        }
        if (grp.size() < 2 || grp[0].I != I) return false;
        unsigned chunks = (unsigned)((end - start + 15) / 16), sends = (chunks + 7) / 8;
        Val bv = get(llvm::wrap((llvm::Value *)base));
        Ctl a; a.weAll = true; a.swsb = swsbDist(1);
        uint8_t d[2];
        for (unsigned s = 0; s < sends; s++) {
            unsigned cs = std::min(8u, chunks - 8 * s);
            uint32_t lanes = 0;                                                 // lane numbers, spare lanes = the last chunk
            for (unsigned k = 0; k < 8; k++) lanes |= std::min(k, cs - 1) << (4 * k);
            uint8_t ad = newReg(), lw = newReg();
            emit(alu1(OP_MOV, a, reg(lw, UW), imm(lanes, V)));
            emit(alu2(OP_SHL, a, reg(ad, UD), reg(lw, UW), imm(4, UD)));       // x 16 bytes
            emit(alu2(OP_ADD, a, reg(ad, UD), reg(ad, UD), opnd(bv, UD)));
            emit(alu2(OP_ADD, a, reg(ad, UD), reg(ad, UD), imm((uint32_t)(start + 128 * s), UD)));
            d[s] = newRegs(4);
            emit(sendNoMask(SFID_HDC1, descUntyped(false, 4, BTI_A32), d[s], ad, swsbDistSet(1, 0)));
            emit(syncNop(swsbDst(0)));
        }
        blockSends += sends;
        for (const CLoad &c : grp) {
            LLVMTypeRef t = LLVMTypeOf(c.I);
            bool fl = LLVMGetTypeKind(scalarTy(t)) == LLVMFloatTypeKind || LLVMGetTypeKind(scalarTy(t)) == LLVMHalfTypeKind;
            Type st = c.eb == 2 ? (fl ? HF : UW) : (fl ? F : UD), dt = fl ? F : UD;
            std::vector<Val> r;
            for (unsigned k = 0; k < c.n; k++) {
                int64_t rel = c.off + k * c.eb - start;
                unsigned chunk = (unsigned)(rel / 16), s = chunk / 8, lane = chunk % 8, ch = (unsigned)((rel % 16) / 4), b = (unsigned)(rel % 4);
                Opnd src = reg((uint8_t)(d[s] + ch), st);
                src.sub = (uint8_t)(lane * 4 + b); src.vs = 0; src.w = 1; src.hs = 0;
                uint8_t v = newReg();
                emit(alu1(OP_MOV, ctl(), reg(v, dt), src));
                r.push_back(regVal(v, dt));
            }
            if (isVec(t)) vvals[c.I] = r; else vals[c.I] = r[0];
            if (c.I != I) { own(c.I); preloaded.insert(c.I); }
            blockLoads++;
        }
        return true;
    }
    std::vector<Val> loadv(LLVMValueRef I) {
        LLVMTypeRef t = LLVMTypeOf(I);
        LLVMValueRef p = LLVMGetOperand(I, 0);
        uint32_t bti = btiOf(p, "load from", false);
        unsigned n = vecN(t), eb = elemBytes(t, "load");
        Val a = inReg(get(p), UD);
        std::vector<Val> r;
        if (eb == 4) {                                         // n consecutive dwords in one message
            uint8_t d = newRegs(n);
            sendNow(SFID_HDC1, descUntyped(false, n, bti), 0, d, a.nr, -1);
            for (unsigned k = 0; k < n; k++) r.push_back(regVal((uint8_t)(d + k), typeOf(t)));
            return r;
        }
        // v0.61.0: 16-bit vectors at a 4-byte aligned address: one untyped read of the dwords holding them (was one
        // byte-scattered read per component: 4 messages for a half4), then each half taken from its dword
        if (eb == 2 && n >= 2 && n <= 8 && LLVMGetAlignment(I) >= 4 && !getenv("XEC_NOOVERLAP") && !getenv("XEC_NOPACK")) {
            unsigned m = (n + 1) / 2;
            uint8_t d = newRegs(m);
            sendNow(SFID_HDC1, descUntyped(false, m, bti), 0, d, a.nr, -1);
            for (unsigned k = 0; k < n; k++) {
                Opnd src = reg((uint8_t)(d + k / 2), isHalf(t) ? HF : UW);
                src.sub = (uint8_t)((k & 1) * 2); src.vs = 16; src.w = 8; src.hs = 2;
                uint8_t v = newReg();
                emit(alu1(OP_MOV, ctl(), reg(v, isHalf(t) ? F : UD), src));
                r.push_back(regVal(v, isHalf(t) ? F : UD));
            }
            return r;
        }
        for (unsigned k = 0; k < n; k++) {                     // 8/16-bit: one byte-scattered read each
            Val ak = k ? binop(OP_ADD, UD, a, immU(k * eb)) : a;
            uint8_t d = newReg();
            sendNow(SFID_HDC0, descByte(false, eb, bti), 0, d, ak.nr, -1);
            Opnd src = reg(d, eb == 1 ? UB : (isHalf(t) ? HF : UW));
            src.vs = eb == 1 ? 32 : 16; src.w = 8; src.hs = eb == 1 ? 4 : 2;
            uint8_t v = newReg();
            emit(alu1(OP_MOV, ctl(), reg(v, isHalf(t) ? F : UD), src));
            r.push_back(regVal(v, isHalf(t) ? F : UD));
        }
        return r;
    }
    void store(LLVMValueRef I) {
        LLVMValueRef v = LLVMGetOperand(I, 0), p = LLVMGetOperand(I, 1);
        LLVMTypeRef t = LLVMTypeOf(v);
        uint32_t bti = btiOf(p, "store to", true);
        unsigned n = vecN(t), eb = elemBytes(t, "store");
        std::vector<Val> c = getv(v);
        Val a = inReg(get(p), UD);
        if (eb == 4 && n == 1 && c[0].kind == Val::REG) {      // already in a register
            sendNow(SFID_HDC1, descUntyped(true, 1, bti), 1 << 6, -1, a.nr, c[0].nr);
            return;
        }
        if (eb == 4) {                                         // data in n consecutive registers
            Type ty = typeOf(t);
            uint8_t d = newRegs(n);
            for (unsigned k = 0; k < n; k++) emit(alu1(OP_MOV, ctl(), reg((uint8_t)(d + k), ty), opnd(c[k], ty)));
            sendNow(SFID_HDC1, descUntyped(true, n, bti), n << 6, -1, a.nr, d);
            return;
        }
        for (unsigned k = 0; k < n; k++) {
            Val ak = k ? binop(OP_ADD, UD, a, immU(k * eb)) : a;
            uint8_t d = newReg();
            if (isHalf(t)) {                                   // float -> half in the low word of each dword
                Opnd dst = reg(d, HF); dst.hs = 2;
                emit(alu1(OP_MOV, ctl(), dst, opnd(notImm(c[k], F), F)));
            } else emit(alu1(OP_MOV, ctl(), reg(d, UD), opnd(c[k], UD)));
            sendNow(SFID_HDC0, descByte(true, eb, bti), 1 << 6, -1, ak.nr, d);
        }
    }

    // ---- milestone 3f: fences, barriers, atomics ---------------------------------
    // memory fence with commit, waited for (Mesa: Gfx11+ fence, SLM via surface 254)
    void fence(uint32_t bti) {
        uint8_t d = newReg();
        emit(sendNoMask(SFID_HDC0, descFence(bti), d, 0, swsbDistSet(1, 0)));
        emit(syncNop(swsbDst(0)));
    }
    // Metal mem_flags: 1 = device, 2 = threadgroup, 4 = texture
    void fences(unsigned flags) {
        if (flags & 2) fence(BTI_SLM);
        if (flags & 5) fence(0);
    }
    // threadgroup_barrier: fences, then the gateway barrier message and sync.bar
    void barrier(unsigned flags) {
        fences(flags);
        usesBarrier = true;
        uint8_t p = newReg();
        Ctl w; w.weAll = true; w.swsb = swsbDist(1);
        emit(alu1(OP_MOV, w, reg(p, UD), imm(0, UD)));
        Ctl w1 = w; w1.exec = 1;
        Opnd d = reg(p, UD); d.sub = 8;                                   // payload.2 = barrier id (g0.2 bits 30:24)
        emit(alu2(OP_AND, w1, d, scal(0, 2, UD), imm(0x7f000000, UD)));
        emit(sendNoMask(SFID_GATEWAY, DESC_BARRIER, -1, p, swsbDistSet(1, 2)));
        emit(syncBar());
        emit(syncNop(swsbSrc(2)));
    }
    Val atomic(AtomicOp aop, LLVMValueRef ptr, const Val &v) {
        uint32_t bti = btiOf(ptr, "atomic on", true);
        Val a = inReg(get(ptr), UD);
        Val x = inReg(v, UD);
        uint8_t d = newReg();
        sendNow(SFID_HDC1, descAtomic(aop, bti), 1 << 6, d, a.nr, x.nr);
        return regVal(d, UD);
    }
    // air.atomic.<global|local>.<op>[.s|.u].<type>(ptr, value, order, scope, volatile)
    Val atomicCall(LLVMValueRef I, const std::string &name) {
        std::vector<std::string> part;
        for (size_t a = 0, b; a <= name.size(); a = b + 1) {
            b = name.find('.', a); if (b == std::string::npos) b = name.size();
            part.push_back(name.substr(a, b - a));
        }
        if (part.size() >= 3 && part[2] == "fence") { fences(7); return Val(); }
        if (part.size() < 5) fail("atomic %s: unexpected name", name.c_str());
        std::string op = part[3];
        if (op.rfind("fetch_", 0) == 0) op = op.substr(6);
        bool sgn = std::find(part.begin() + 4, part.end(), "s") != part.end();
        bool flt = name.find(".f32") != std::string::npos;
        LLVMValueRef ptr = LLVMGetOperand(I, 0);
        if (op == "load") {                              // a plain 32-bit load
            uint32_t bti = btiOf(ptr, "atomic load from", false);
            Val a = inReg(get(ptr), UD);
            uint8_t d = newReg();
            sendNow(SFID_HDC1, descUntyped(false, 1, bti), 0, d, a.nr, -1);
            return regVal(d, flt ? F : UD);
        }
        Val v = get(LLVMGetOperand(I, 1));
        if (op == "store") { atomic(AOP_MOV, ptr, v); return Val(); }
        if (op == "xchg" || op == "exchange") { Val r = atomic(AOP_MOV, ptr, v); r.type = flt ? F : UD; return r; }
        if (flt) fail("float atomic %s is not supported yet", name.c_str());
        AtomicOp a;
        if (op == "add") a = AOP_ADD;
        else if (op == "sub") a = AOP_SUB;
        else if (op == "and") a = AOP_AND;
        else if (op == "or") a = AOP_OR;
        else if (op == "xor") a = AOP_XOR;
        else if (op == "min") a = sgn ? AOP_IMIN : AOP_UMIN;
        else if (op == "max") a = sgn ? AOP_IMAX : AOP_UMAX;
        else fail("atomic %s is not supported yet", name.c_str());
        return atomic(a, ptr, v);
    }
    // LLVM's own atomicrmw instruction (AIR from other front ends uses it)
    Val atomicRMW(LLVMValueRef I) {
        LLVMValueRef ptr = LLVMGetOperand(I, 0);
        if (elemBytes(LLVMTypeOf(LLVMGetOperand(I, 1)), "atomic") != 4) fail("8/16-bit atomics are not supported");
        Val v = get(LLVMGetOperand(I, 1));
        AtomicOp a;
        switch (LLVMGetAtomicRMWBinOp(I)) {
        case LLVMAtomicRMWBinOpXchg: a = AOP_MOV; break;
        case LLVMAtomicRMWBinOpAdd: a = AOP_ADD; break;
        case LLVMAtomicRMWBinOpSub: a = AOP_SUB; break;
        case LLVMAtomicRMWBinOpAnd: a = AOP_AND; break;
        case LLVMAtomicRMWBinOpOr: a = AOP_OR; break;
        case LLVMAtomicRMWBinOpXor: a = AOP_XOR; break;
        case LLVMAtomicRMWBinOpMax: a = AOP_IMAX; break;
        case LLVMAtomicRMWBinOpMin: a = AOP_IMIN; break;
        case LLVMAtomicRMWBinOpUMax: a = AOP_UMAX; break;
        case LLVMAtomicRMWBinOpUMin: a = AOP_UMIN; break;
        default: fail("atomicrmw of this kind is not supported yet: %s", valName(I).c_str());
        }
        Val r = atomic(a, ptr, v);
        r.type = typeOf(LLVMTypeOf(I));
        return r;
    }

    // ---- milestone 4: textures ------------------------------------------------------
    // a texture or sampler argument: its slot (an immediate from bindArguments)
    unsigned slotOf(LLVMValueRef v, const char *what) {
        // a constexpr sampler: a constant global [2 x i64] (__air_sampler_state), maybe behind a cast
        while (LLVMIsAConstantExpr(v) && (LLVMGetConstOpcode(v) == LLVMBitCast || LLVMGetConstOpcode(v) == LLVMAddrSpaceCast))
            v = LLVMGetOperand(v, 0);
        if (LLVMIsAGlobalVariable(v)) {
            auto it = constSamplerSlot.find(v);
            if (it != constSamplerSlot.end()) return it->second;
            LLVMValueRef init = LLVMGetInitializer(v);
            LLVMValueRef w = init ? LLVMGetAggregateElement(init, 0) : nullptr;
            if (!w || !LLVMIsAConstantInt(w)) fail("constexpr sampler %s: unexpected encoding", valName(v).c_str());
            if (constSamplers.size() >= 4) fail("more than 4 constexpr samplers in one kernel");
            unsigned slot = 7 - (unsigned)constSamplers.size();
            constSamplers.push_back(LLVMConstIntGetZExtValue(w));
            constSamplerSlot[v] = slot;
            return slot;
        }
        Val x = get(v);
        if (x.kind != Val::IMM) fail("%s chosen at run time is not supported yet", what);
        return x.imm;
    }
    // n consecutive payload registers from scalar values (as type t)
    uint8_t payload(const std::vector<Val> &v, Type t) {
        uint8_t p = newRegs((unsigned)v.size());
        for (size_t k = 0; k < v.size(); k++) emit(alu1(OP_MOV, ctl(), reg((uint8_t)(p + k), t), opnd(v[k], t)));
        return p;
    }
    // AIR: air.read_texture_2d[_array][.u|.s][.i16].v4f32/v4i32/v4f16 (tex, coord, [slice,] lod, access) -> {v4, i8}
    //      air.write_texture_2d[_array][...](tex, coord, [slice,] value, lod, access)
    //      air.sample_texture_2d[_array].v4f32(tex, sampler, coord, [slice,] ...) -> {v4, i8}
    //      air.get_width/height_texture_2d[_array](tex, lod), air.get_array_size_texture_2d_array(tex)
    // v0.37.0: depth2d<float>: air.sample_depth_2d.f32 (tex, sampler, coord, ...), air.sample_compare_depth_2d.f32
    //   (tex, sampler, coord, float reference, ...), air.read_depth_2d.f32 (tex, coord, lod, ...), air.get_width /
    //   height_depth_2d (tex, lod). The depth is the R channel of an R32_FLOAT / R16_UNORM view; the compare
    //   function is the sampler's (SAMPLER_STATE shadow function). Anything else: the call text in the error.
    Val depthCall(LLVMValueRef I, const std::string &name) {
        auto bad = [&](const char *why) {
            char *t = LLVMPrintValueToString(I);
            std::string txt = t ? t : "?";
            if (t) LLVMDisposeMessage(t);
            if (txt.size() > 300) txt = txt.substr(0, 300) + "...";
            fail("%s: %s (%s)", name.c_str(), why, txt.c_str());
        };
        if (name.find("depth_2d_array") != std::string::npos || name.find("depth_2d_ms") != std::string::npos ||
            name.find("depth_cube") != std::string::npos) bad("only depth2d textures yet");
        unsigned bti = slotOf(LLVMGetOperand(I, 0), "a depth texture");
        auto done = [&](uint8_t d) -> Val {               // {float, i8} or float: the depth (R)
            Val x = regVal(d, F);
            if (LLVMGetTypeKind(LLVMTypeOf(I)) == LLVMStructTypeKind) { vvals[I] = { x }; return Val(); }
            return x;
        };
        if (name.rfind("air.get_", 0) == 0) {
            unsigned comp = name.rfind("air.get_width", 0) == 0 ? 0 : name.rfind("air.get_height", 0) == 0 ? 1 : 9;
            if (comp == 9) bad("not supported yet");
            uint8_t p = payload({ immU(0) }, UD);
            uint8_t d = newRegs(4);
            sendNow(SFID_SAMPLER, descSampler(SMP_RESINFO, 1, 4, bti, 0), 0, d, p, -1);
            return regVal((uint8_t)(d + comp), UD);
        }
        if (name.rfind("air.read_depth_2d", 0) == 0) {
            unsigned ci = 1;                               // the first vector argument is the coordinate, the lod after it
            while (ci < 4 && LLVMGetTypeKind(LLVMTypeOf(LLVMGetOperand(I, ci))) != LLVMVectorTypeKind) ci++;
            if (ci >= 4) bad("no coordinate argument");
            LLVMValueRef lod = LLVMGetOperand(I, ci + 1);
            if (!LLVMIsAConstantInt(lod) || LLVMConstIntGetZExtValue(lod) != 0) bad("only mip level 0 yet");
            std::vector<Val> c = getv(LLVMGetOperand(I, ci));
            if (c.size() < 2) bad("2D coordinates expected");
            c.resize(2);
            uint8_t p = payload(c, D);
            uint8_t d = newRegs(4);
            sendNow(SFID_SAMPLER, descSampler(SMP_LD_LZ, 2, 4, bti, 0), 0, d, p, -1);
            return done(d);
        }
        bool cmp = name.rfind("air.sample_compare_depth_2d", 0) == 0;
        if (!cmp && name.rfind("air.sample_depth_2d", 0) != 0) bad("not supported yet");
        unsigned smp = slotOf(LLVMGetOperand(I, 1), "a sampler");
        // Apple's AIR (v0.37.0 run): (tex, sampler, i32 1, <2 x float> coord, [float reference,] i1, offset, ...)
        unsigned ci = 2;
        while (ci < 5 && LLVMGetTypeKind(LLVMTypeOf(LLVMGetOperand(I, ci))) != LLVMVectorTypeKind) ci++;
        if (ci >= 5) bad("no coordinate argument");
        std::vector<Val> c = getv(LLVMGetOperand(I, ci));
        if (c.size() < 2) bad("2D coordinates expected");
        c.resize(2);
        LLVMValueRef o = LLVMGetOperand(I, ci + (cmp ? 3 : 2));          // texel offset: zero
        if (!(LLVMIsAConstantAggregateZero(o) || LLVMIsNull(o))) bad("texel offsets are not supported yet");
        std::vector<Val> prm;
        if (cmp) {                                         // sample_c: reference first, then u, v
            LLVMValueRef r = LLVMGetOperand(I, ci + 1);
            if (LLVMGetTypeKind(LLVMTypeOf(r)) != LLVMFloatTypeKind) bad("the reference is not after the coordinate");
            prm.push_back(get(r));
        }
        prm.push_back(c[0]); prm.push_back(c[1]);
        uint8_t p = payload(prm, F);
        uint8_t d = newRegs(4);
        bool frag = stage == XE_STAGE_FRAGMENT;
        SamplerMsg m = cmp ? (frag ? SMP_SAMPLE_C : SMP_SAMPLE_C_LZ) : (frag ? SMP_SAMPLE : SMP_SAMPLE_LZ);
        sendNow(SFID_SAMPLER, descSampler(m, (unsigned)prm.size(), 4, bti, smp), 0, d, p, -1);
        return done(d);
    }
    // v0.41.0: texturecube / texturecube_array: sample (the hardware picks the face: SURFTYPE_CUBE), size
    //   air.sample_texture_cube[_array].v4f32(tex, sampler, <3 x float> dir, [i32 index,] options...) -> {v4, i8}
    Val cubeCall(LLVMValueRef I, const std::string &name) {
        bool arr = name.find("texture_cube_array") != std::string::npos;
        unsigned bti = slotOf(LLVMGetOperand(I, 0), "a texture");
        bool intRes = name.find(".v4i32") != std::string::npos || name.find(".v4i16") != std::string::npos;
        bool halfRes = name.find(".v4f16") != std::string::npos;
        if (name.rfind("air.get_", 0) == 0) {
            unsigned comp = name.rfind("air.get_width", 0) == 0 ? 0 : name.rfind("air.get_height", 0) == 0 ? 1 : 9;
            if (comp == 9) fail("%s is not supported yet", name.c_str());
            uint8_t p = payload({ immU(0) }, UD);
            uint8_t d = newRegs(4);
            sendNow(SFID_SAMPLER, descSampler(SMP_RESINFO, 1, 4, bti, 0), 0, d, p, -1);
            return regVal((uint8_t)(d + comp), UD);
        }
        if (name.rfind("air.sample_texture", 0) != 0) fail("%s: cube textures can only be sampled yet", name.c_str());
        unsigned smp = slotOf(LLVMGetOperand(I, 1), "a sampler");
        std::vector<Val> c = getv(LLVMGetOperand(I, 2));
        if (c.size() < 3) fail("%s: a 3D direction expected", name.c_str());
        c.resize(3);
        unsigned next = 3;
        if (arr) { c.push_back(mov(F, get(LLVMGetOperand(I, 3)), UD)); next = 4; }   // cube index as float
        if (stage == XE_STAGE_FRAGMENT) {                 // options (bias / level / min LOD): only the plain sample
            unsigned na = (unsigned)LLVMGetNumArgOperands(I);
            for (unsigned a2 = next; a2 + 1 < na; a2++) {
                LLVMValueRef v = LLVMGetOperand(I, a2);
                LLVMBool lost = 0;
                bool zero = LLVMIsAConstantAggregateZero(v) || LLVMIsNull(v) ? true
                          : LLVMIsAConstantInt(v) ? LLVMConstIntGetZExtValue(v) == 0
                          : LLVMIsAConstantFP(v) ? LLVMConstRealGetDouble(v, &lost) == 0.0 : false;
                if (!zero) fail("%s: bias / level / gradient / min LOD options are not supported yet", name.c_str());
            }
        }
        uint8_t p = payload(c, F);
        uint8_t d = newRegs(4);
        SamplerMsg m = stage == XE_STAGE_FRAGMENT ? SMP_SAMPLE : SMP_SAMPLE_LZ;
        sendNow(SFID_SAMPLER, descSampler(m, (unsigned)c.size(), 4, bti, smp), 0, d, p, -1);
        std::vector<Val> r;
        for (unsigned k = 0; k < 4; k++) {
            Val x = regVal((uint8_t)(d + k), intRes ? UD : F);
            r.push_back(halfRes ? roundHalf(x) : x);
        }
        vvals[I] = r;
        return Val();
    }
    Val textureCall(LLVMValueRef I, const std::string &name) {
        if (name.find("texture_cube") != std::string::npos) return cubeCall(I, name);
        // v0.51.5: 1D textures (arrays) are 2D textures (arrays) one pixel tall: v = 0.5 (sample) / 0 (read, write)
        bool one = name.find("texture_1d") != std::string::npos;
        bool arr = name.find("texture_2d_array") != std::string::npos || name.find("texture_1d_array") != std::string::npos;
        bool three = name.find("texture_3d") != std::string::npos;     // v0.51.8: 3D textures: sampling only (u, v, w)
        if (three && name.rfind("air.sample_texture", 0) != 0) fail("%s: 3D textures can only be sampled yet", name.c_str());
        if (!one && !three && name.find("texture_2d") == std::string::npos)
            fail("%s: only 1D, 2D and 3D textures (and arrays of them) are supported yet", name.c_str());
        // v0.66.0: multisample textures have one sample (the driver makes them plain 2D textures): read(coord, sample)
        // reads that sample; the sample index sits where a 2D read has its mip level
        bool ms = name.find("texture_2d_ms") != std::string::npos;
        if (ms && name.find("texture_2d_ms_array") != std::string::npos) fail("%s: multisample texture arrays are not supported yet", name.c_str());
        if (ms && name.rfind("air.read_texture", 0) != 0 && name.rfind("air.get_", 0) != 0)
            fail("%s: multisample textures can only be read yet", name.c_str());
        unsigned bti = slotOf(LLVMGetOperand(I, 0), "a texture");
        bool intRes = name.find(".v4i32") != std::string::npos || name.find(".v4i16") != std::string::npos;
        bool halfRes = name.find(".v4f16") != std::string::npos;
        auto coords = [&](unsigned at, Type t) {          // u, v [, slice]
            std::vector<Val> c;
            if (one) c = { get(LLVMGetOperand(I, at)), immU(0) };
            else c = getv(LLVMGetOperand(I, at));
            if (c.size() < 2) fail("%s: 2D coordinates expected", name.c_str());
            c.resize(2);
            if (arr) c.push_back(get(LLVMGetOperand(I, at + 1)));
            (void)t;
            return c;
        };
        auto result4 = [&](uint8_t d) {                   // {<4 x T>, i8}: the vector part
            std::vector<Val> r;
            for (unsigned k = 0; k < 4; k++) {
                Val x = regVal((uint8_t)(d + k), intRes ? UD : F);
                r.push_back(halfRes ? roundHalf(x) : x);
            }
            vvals[I] = r;
            return Val();
        };
        if (name.rfind("air.get_", 0) == 0) {             // size: sampler resinfo, lod 0
            unsigned comp = name.rfind("air.get_width", 0) == 0 ? 0 : name.rfind("air.get_height", 0) == 0 ? 1 :
                            name.rfind("air.get_array_size", 0) == 0 ? 2 : 9;
            if (comp == 9) fail("%s is not supported yet", name.c_str());
            uint8_t p = payload({ immU(0) }, UD);
            uint8_t d = newRegs(4);
            sendNow(SFID_SAMPLER, descSampler(SMP_RESINFO, 1, 4, bti, 0), 0, d, p, -1);
            return regVal((uint8_t)(d + comp), UD);
        }
        if (name.rfind("air.read_texture", 0) == 0) {
            // access 3 = read_write: typed read (sees this kernel's writes); otherwise the sampler's ld
            LLVMValueRef acc = LLVMGetOperand(I, arr ? 4 : 3);
            bool rw = LLVMIsAConstantInt(acc) && LLVMConstIntGetZExtValue(acc) == 3;
            LLVMValueRef lod = LLVMGetOperand(I, arr ? 3 : 2);
            if (!ms && (!LLVMIsAConstantInt(lod) || LLVMConstIntGetZExtValue(lod) != 0)) fail("%s: only mip level 0 is supported yet", name.c_str());
            std::vector<Val> c = coords(1, D);
            uint8_t p = payload(c, D);
            uint8_t d = newRegs(4);
            if (rw) sendNow(SFID_HDC1, descTyped(false, (unsigned)c.size(), bti), 0, d, p, -1);
            else sendNow(SFID_SAMPLER, descSampler(SMP_LD_LZ, (unsigned)c.size(), 4, bti, 0), 0, d, p, -1);
            return result4(d);
        }
        if (name.rfind("air.write_texture", 0) == 0) {
            unsigned vi = arr ? 3 : 2;
            LLVMValueRef lod = LLVMGetOperand(I, vi + 1);
            if (!LLVMIsAConstantInt(lod) || LLVMConstIntGetZExtValue(lod) != 0) fail("%s: only mip level 0 is supported yet", name.c_str());
            std::vector<Val> c = coords(1, D);
            std::vector<Val> v = getv(LLVMGetOperand(I, vi));
            while (v.size() < 4) v.push_back(intRes ? immU(0) : immF(0.0f));
            uint8_t p = newRegs((unsigned)c.size() + 4);
            for (size_t k = 0; k < c.size(); k++) emit(alu1(OP_MOV, ctl(), reg((uint8_t)(p + k), D), opnd(c[k], D)));
            Type vt = intRes ? UD : F;
            for (size_t k = 0; k < 4; k++) emit(alu1(OP_MOV, ctl(), reg((uint8_t)(p + c.size() + k), vt), opnd(v[k], vt)));
            sendNow(SFID_HDC1, descTyped(true, (unsigned)c.size(), bti), 0, -1, p, -1);
            return Val();
        }
        if (name.rfind("air.sample_texture", 0) == 0) {
            unsigned smp = slotOf(LLVMGetOperand(I, 1), "a sampler");
            unsigned off = arr ? 5 : 4;                   // (tex, sampler, coord, [slice,] i1, offset <2 x i32>, ...)
            LLVMValueRef o = LLVMGetOperand(I, off);
            // v0.51.2: constant texel offsets (-8..7) go in the message header (M0.2: U 11:8, V 7:4), as Mesa
            uint32_t offBits = 0;
            if (one && LLVMIsAConstantInt(o)) {           // 1D: one i32 offset
                long long v = LLVMConstIntGetSExtValue(o);
                if (v < -8 || v > 7) fail("%s: texel offset %lld out of -8..7", name.c_str(), v);
                offBits = ((uint32_t)v & 15) << 8;
            } else if (one && !LLVMIsNull(o)) fail("%s: texel offsets must be constants", name.c_str());
            else if (!(LLVMIsAConstantAggregateZero(o) || LLVMIsNull(o))) {
                for (unsigned k = 0; k < (three ? 3u : 2u); k++) {
                    LLVMValueRef e = LLVMIsAConstantDataVector(o) || LLVMIsAConstantVector(o) ? LLVMGetAggregateElement(o, k) : nullptr;
                    if (!e || !LLVMIsAConstantInt(e)) fail("%s: texel offsets must be constants", name.c_str());
                    long long v = LLVMConstIntGetSExtValue(e);
                    if (v < -8 || v > 7) fail("%s: texel offset %lld out of -8..7", name.c_str(), v);
                    offBits |= ((uint32_t)v & 15) << (k == 0 ? 8 : k == 1 ? 4 : 0);
                }
            }
            std::vector<Val> c;
            if (one) c = { get(LLVMGetOperand(I, 2)), immF(0.5f) };
            else c = getv(LLVMGetOperand(I, 2));
            if (c.size() < (three ? 3u : 2u)) fail("%s: %s coordinates expected", name.c_str(), three ? "3D" : "2D");
            c.resize(three ? 3 : 2);
            if (arr) c.push_back(mov(F, get(LLVMGetOperand(I, 3)), UD));   // array layer as float
            // compute kernels have no derivatives: sample at level 0 (sample_lz); fragment functions (v0.36.0):
            // sample, the sampler takes the level of detail from the 2x2 pixel quad (mipmaps)
            // v0.42.1: (..., i1 level?, float bias-or-level, float min_lod_clamp, i32 mode): level(l) -> sample_l
            //   (sample_lz for 0), bias(b) -> sample_b (fragment only); the min LOD clamp is ignored yet.
            //   As Mesa: the bias / LOD goes first in the payload, then the coordinates.
            SamplerMsg m = stage == XE_STAGE_FRAGMENT ? SMP_SAMPLE : SMP_SAMPLE_LZ;
            Val lodv;
            unsigned na = (unsigned)LLVMGetNumArgOperands(I);
            if (off + 2 < na) {
                LLVMValueRef fl = LLVMGetOperand(I, off + 1), lv = LLVMGetOperand(I, off + 2);
                LLVMBool lost = 0;
                bool isLevel = LLVMIsAConstantInt(fl) ? LLVMConstIntGetZExtValue(fl) != 0 : true;
                if (!LLVMIsAConstantInt(fl)) fail("%s: a run-time choice between bias and level", name.c_str());
                bool lvZero = LLVMIsAConstantFP(lv) ? LLVMConstRealGetDouble(lv, &lost) == 0.0
                            : (LLVMIsAConstantAggregateZero(lv) || LLVMIsNull(lv));
                if (isLevel) {
                    if (lvZero) m = SMP_SAMPLE_LZ;
                    else { m = SMP_SAMPLE_L; lodv = get(lv); }
                } else if (!lvZero) {
                    if (stage == XE_STAGE_FRAGMENT) { m = SMP_SAMPLE_B; lodv = get(lv); }
                    // (no derivatives outside fragment functions: level 0, the bias doesn't matter)
                }
            }
            if (lodv.kind != Val::NONE) c.insert(c.begin(), lodv);
            if (offBits) {                                 // header (a copy of g0, offsets in .2) + the payload
                uint8_t p = newRegs((unsigned)c.size() + 1);
                Ctl w = ctl(); w.weAll = true;
                emit(alu1(OP_MOV, w, reg(p, UD), reg(0, UD)));
                Ctl w1 = ctl(); w1.weAll = true; w1.exec = 1;
                Opnd h2 = reg(p, UD); h2.sub = 8;          // p.2
                emit(alu1(OP_MOV, w1, h2, imm(offBits, UD)));
                for (size_t k = 0; k < c.size(); k++) emit(alu1(OP_MOV, ctl(), reg((uint8_t)(p + 1 + k), F), opnd(c[k], F)));
                uint8_t d = newRegs(4);
                sendNow(SFID_SAMPLER, descSampler(m, (unsigned)c.size() + 1, 4, bti, smp) | (1u << 19), 0, d, p, -1);
                return result4(d);
            }
            uint8_t p = payload(c, F);
            uint8_t d = newRegs(4);
            sendNow(SFID_SAMPLER, descSampler(m, (unsigned)c.size(), 4, bti, smp), 0, d, p, -1);
            return result4(d);
        }
        fail("%s is not supported yet", name.c_str());
    }

    // ---- intrinsics: air.* and llvm.* (lanewise for vectors) -------------------------
    Val call(LLVMValueRef I) {
        LLVMValueRef callee = LLVMGetCalledValue(I);
        size_t len = 0;
        const char *nm = LLVMGetValueName2(callee, &len);
        std::string name = nm ? std::string(nm, len) : "?";
        unsigned n = LLVMGetNumArgOperands(I);
        auto arg = [&](unsigned i) { return get(LLVMGetOperand(I, i)); };
        LLVMTypeRef rt = LLVMTypeOf(I);
        bool half = isHalf(rt);
        // 3f: barriers and atomics
        if (name == "air.wg.barrier" || name == "air.simdgroup.barrier") {
            Val f = arg(0);
            if (f.kind != Val::IMM) fail("%s with run-time flags", name.c_str());
            if (name == "air.wg.barrier") barrier(f.imm);
            else fences(f.imm);                  // a SIMD8 hardware thread is one simdgroup: lanes run together
            return Val();
        }
        if (name.rfind("air.atomic.", 0) == 0) return atomicCall(I, name);
        if (name.find("_texture_") != std::string::npos) return textureCall(I, name);
        if (name.find("_depth_2d") != std::string::npos) return depthCall(I, name);
        // base name: air.[fast_]<op>.<type>  or  llvm.<op>.<type>
        std::string base = name;
        if (base.rfind("air.", 0) == 0) base = base.substr(4);
        else if (base.rfind("llvm.", 0) == 0) base = base.substr(5);
        if (base.rfind("fast_", 0) == 0) base = base.substr(5);
        std::string op = base.substr(0, base.find('.'));
        std::string sfx = base.find('.') == std::string::npos ? "" : base.substr(base.find('.') + 1);
        bool sgn = sfx.rfind("s.", 0) == 0;
        auto fres = [&](const Val &v) { return half ? roundHalf(v) : v; };
        auto minmax = [&](bool isMax, Type t, const Val &x, const Val &y) {
            Val c = compare(isMax ? C_GE : C_L, t, x, y);
            flagFrom(c);
            Val a = notImm(x, t);
            uint8_t d = newReg();
            Ctl k = ctl(); k.pred = 1; k.flag = 0;
            emit(alu2(OP_SEL, k, reg(d, t), opnd(a, t), opnd(y, t)));
            return regVal(d, t);
        };
        bool isFloat = LLVMGetTypeKind(scalarTy(rt)) == LLVMFloatTypeKind || half;
        unsigned ib = intBits(rt);

        if (op == "fma" || op == "fmuladd") return fres(binop(OP_ADD, F, binop(OP_MUL, F, arg(0), arg(1)), arg(2)));
        // v0.42.0: derivatives (fragment functions): SIMD8 lanes 0-3 / 4-7 are 2x2 pixel quads (TL, TR, BL, BR);
        // coarse, as Mesa: ddx = q.TR - q.TL, ddy = q.BL - q.TL for the whole quad (regions <4;4,0>)
        if (op == "dfdx" || op == "dfdy" || op == "fwidth") {
            auto deriv = [&](const Val &v, bool y) -> Val {
                if (stage != XE_STAGE_FRAGMENT || v.kind != Val::REG) return immF(0.0f);   // uniform: 0
                uint8_t d = newReg();
                Opnd a = reg(v.nr, F); a.sub = y ? 8 : 4; a.vs = 4; a.w = 4; a.hs = 0;
                Opnd b = reg(v.nr, F); b.sub = 0; b.vs = 4; b.w = 4; b.hs = 0; b.neg = true;
                emit(alu2(OP_ADD, ctl(), reg(d, F), a, b));
                return regVal(d, F);
            };
            Val x = arg(0);
            if (op == "dfdx") return fres(deriv(x, false));
            if (op == "dfdy") return fres(deriv(x, true));
            Val dx = deriv(x, false), dy = deriv(x, true);
            if (dx.kind != Val::REG) return immF(0.0f);
            uint8_t d = newReg();
            Opnd a = reg(dx.nr, F); a.abs = true;
            Opnd b = reg(dy.nr, F); b.abs = true;
            emit(alu2(OP_ADD, ctl(), reg(d, F), a, b));
            return fres(regVal(d, F));
        }
        if (op == "mix" && n == 3)                                // x + (y - x) * a
            return fres(binop(OP_ADD, F, arg(0), binop(OP_MUL, F, binop(OP_ADD, F, arg(1), arg(0), true), arg(2))));
        if (op == "step" && n == 2) {                             // x < edge ? 0 : 1
            Val c = compare(C_L, F, arg(1), arg(0));
            flagFrom(c);
            uint8_t d = newReg();
            Ctl k = ctl(); k.pred = 1; k.flag = 0;
            Val z = inReg(immF(0.0f), F);
            emit(alu2(OP_SEL, k, reg(d, F), opnd(z, F), imm(0x3F800000u, F)));
            return fres(regVal(d, F));
        }
        if (op == "smoothstep" && n == 3) {                       // t = sat((x - e0) / (e1 - e0)); t * t * (3 - 2t)
            Val t = fdiv(binop(OP_ADD, F, arg(2), arg(0), true), binop(OP_ADD, F, arg(1), arg(0), true));
            Val tr = inReg(t, F); uint8_t d = newReg(); Ctl k = ctl(); k.sat = true;
            emit(alu1(OP_MOV, k, reg(d, F), reg(tr.nr, F)));
            Val ts = regVal(d, F);
            Val p = binop(OP_ADD, F, immF(3.0f), binop(OP_MUL, F, ts, immF(2.0f)), true);
            return fres(binop(OP_MUL, F, binop(OP_MUL, F, ts, ts), p));
        }
        if (op == "sign" && isFloat && n == 1) {                   // v0.51.2: (x < 0 ? -1 : 0) - (x > 0 ? -1 : 0)
            Val gt = compare(C_G, F, arg(0), immF(0.0f)), lt = compare(C_L, F, arg(0), immF(0.0f));
            return fres(binop(OP_ADD, F, mov(F, lt, D), mov(F, gt, D), true));
        }
        if (op == "fabs" || (op == "abs" && isFloat)) {
            Val x = inReg(arg(0), F); uint8_t d = newReg();
            Opnd s = reg(x.nr, F); s.abs = true;
            emit(alu1(OP_MOV, ctl(), reg(d, F), s));
            return regVal(d, F);
        }
        if (op == "abs") {                                        // integer abs: air.abs.s.i32 / llvm.abs.i32
            Val x = sext(arg(0), ib);
            Val neg = binop(OP_ADD, D, immU(0), x, true);
            return canon(minmax(true, D, x, neg), ib);
        }
        if (op == "saturate") {
            uint8_t d = newReg(); Ctl k = ctl(); k.sat = true;
            emit(alu1(OP_MOV, k, reg(d, F), opnd(notImm(arg(0), F), F)));
            return regVal(d, F);
        }
        if (op == "clamp" && n == 3) {
            if (isFloat) return minmax(false, F, minmax(true, F, arg(0), arg(1)), arg(2));
            Type t = sgn ? D : UD;
            return minmax(false, t, minmax(true, t, arg(0), arg(1)), arg(2));
        }
        if ((op == "fmax3" || (op == "max3" && isFloat)) && n == 3) return minmax(true, F, minmax(true, F, arg(0), arg(1)), arg(2));    // v0.51.8
        if ((op == "fmin3" || (op == "min3" && isFloat)) && n == 3) return minmax(false, F, minmax(false, F, arg(0), arg(1)), arg(2));
        if ((op == "fmedian3" || (op == "median3" && isFloat)) && n == 3) {                  // max(min(a,b), min(max(a,b), c))
            Val a = arg(0), b = arg(1), c = arg(2);
            return minmax(true, F, minmax(false, F, a, b), minmax(false, F, minmax(true, F, a, b), c));
        }
        if ((op == "fmax" || op == "maxnum" || (op == "max" && isFloat)) && n == 2) return minmax(true, F, arg(0), arg(1));
        if ((op == "fmin" || op == "minnum" || (op == "min" && isFloat)) && n == 2) return minmax(false, F, arg(0), arg(1));
        if ((op == "max" || op == "umax" || op == "smax") && n == 2) {
            bool s2 = sgn || op == "smax";
            return canon(minmax(true, s2 ? D : UD, s2 ? sext(arg(0), ib) : arg(0), s2 ? sext(arg(1), ib) : arg(1)), ib);
        }
        if ((op == "min" || op == "umin" || op == "smin") && n == 2) {
            bool s2 = sgn || op == "smin";
            return canon(minmax(false, s2 ? D : UD, s2 ? sext(arg(0), ib) : arg(0), s2 ? sext(arg(1), ib) : arg(1)), ib);
        }
        // float math: the GPU's math unit (2^x and log2 natively)
        const float LOG2E = 1.44269504f, LN2 = 0.69314718f, LOG10_2 = 0.30103f;
        if (isFloat && n == 1) {
            if (op == "sqrt") return fres(math(M_SQRT, F, arg(0), nullptr));
            if (op == "rsqrt") return fres(math(M_RSQ, F, arg(0), nullptr));
            if (op == "sin") return fres(math(M_SIN, F, arg(0), nullptr));
            if (op == "cos") return fres(math(M_COS, F, arg(0), nullptr));
            if (op == "tan") return fres(fdiv(math(M_SIN, F, arg(0), nullptr), math(M_COS, F, arg(0), nullptr)));
            if (op == "exp2") return fres(math(M_EXP, F, arg(0), nullptr));
            if (op == "exp") return fres(math(M_EXP, F, binop(OP_MUL, F, arg(0), immF(LOG2E)), nullptr));
            if (op == "exp10") return fres(math(M_EXP, F, binop(OP_MUL, F, arg(0), immF(3.32192809f)), nullptr));
            if (op == "log2") return fres(math(M_LOG, F, arg(0), nullptr));
            if (op == "log") return fres(binop(OP_MUL, F, math(M_LOG, F, arg(0), nullptr), immF(LN2)));
            if (op == "log10") return fres(binop(OP_MUL, F, math(M_LOG, F, arg(0), nullptr), immF(LOG10_2)));
            if (op == "floor") return unop((Op)69, F, arg(0));                      // rndd
            if (op == "rint" || op == "roundeven" || op == "nearbyint") return unop((Op)70, F, arg(0));   // rnde
            if (op == "trunc") return unop((Op)71, F, arg(0));                      // rndz
            if (op == "ceil") {                                                     // -floor(-x)
                Val x = inReg(arg(0), F); uint8_t d = newReg();
                Opnd s = reg(x.nr, F); s.neg = true;
                emit(alu1((Op)69, ctl(), reg(d, F), s));
                uint8_t e = newReg(); Opnd t2 = reg(d, F); t2.neg = true;
                emit(alu1(OP_MOV, ctl(), reg(e, F), t2));
                return regVal(e, F);
            }
            if (op == "atan") return fres(atan2v(arg(0), immF(1.0f)));      // v0.51.8
            if (op == "asin" || op == "acos") {                     // v0.68.27 (Brave / Skia Graphite strokes)
                Val x = arg(0);
                Val om = minmax(true, F, binop(OP_ADD, F, immF(1.0f), binop(OP_MUL, F, x, x), true), immF(0.0f));
                Val sq = math(M_SQRT, F, om, nullptr);              // sqrt(max(1 - x*x, 0))
                return fres(op == "asin" ? atan2v(x, sq) : atan2v(sq, x));
            }
            // v0.68.31: the rest of Metal's float math (the list from the old driver's AIR translator)
            if (op == "sinpi" || op == "cospi" || op == "tanpi") {
                Val px = binop(OP_MUL, F, arg(0), immF(3.14159265f));
                if (op == "sinpi") return fres(math(M_SIN, F, px, nullptr));
                if (op == "cospi") return fres(math(M_COS, F, px, nullptr));
                return fres(fdiv(math(M_SIN, F, px, nullptr), math(M_COS, F, px, nullptr)));
            }
            if (op == "sinh" || op == "cosh") {                     // (e^x -/+ e^-x) / 2
                Val x = minmax(false, F, minmax(true, F, arg(0), immF(-80.0f)), immF(80.0f));
                Val ep = math(M_EXP, F, binop(OP_MUL, F, x, immF(LOG2E)), nullptr);
                Val en = math(M_EXP, F, binop(OP_MUL, F, x, immF(-LOG2E)), nullptr);
                Val r = binop(OP_ADD, F, ep, en, op == "sinh");
                return fres(binop(OP_MUL, F, r, immF(0.5f)));
            }
            if (op == "asinh" || op == "acosh") {                   // log(|x| + sqrt(x*x +/- 1)), asinh odd
                Val x = arg(0);
                Val ax = binop(OP_AND, UD, x, immU(0x7fffffff)); ax.type = F;
                Val t = binop(OP_ADD, F, binop(OP_MUL, F, ax, ax), immF(1.0f), op == "acosh");
                Val sq = math(M_SQRT, F, minmax(true, F, t, immF(0.0f)), nullptr);
                Val r = binop(OP_MUL, F, math(M_LOG, F, binop(OP_ADD, F, ax, sq), nullptr), immF(LN2));
                if (op == "acosh") return fres(r);
                Val sb = binop(OP_AND, UD, x, immU(0x80000000u));
                Val o = binop(OP_OR, UD, r, sb); o.type = F;
                return fres(o);
            }
            if (op == "atanh") {                                    // 0.5 * log((1+x)/(1-x))
                Val x = arg(0);
                Val q = fdiv(binop(OP_ADD, F, immF(1.0f), x), binop(OP_ADD, F, immF(1.0f), x, true));
                return fres(binop(OP_MUL, F, math(M_LOG, F, q, nullptr), immF(0.5f * LN2)));
            }
            if (op == "tanh") {                                     // v0.68.27: 1 - 2 / (exp2(2x*log2e) + 1)
                Val x = minmax(false, F, minmax(true, F, arg(0), immF(-20.0f)), immF(20.0f));
                Val e = math(M_EXP, F, binop(OP_MUL, F, x, immF(2.0f * LOG2E)), nullptr);
                return fres(binop(OP_ADD, F, immF(1.0f), fdiv(immF(2.0f), binop(OP_ADD, F, e, immF(1.0f))), true));
            }
            if (op == "fract") return unop(OP_FRC, F, arg(0));
            if (op == "round") {                                                    // half away from zero
                Val x = arg(0);
                Val ax = inReg(x, F); uint8_t d = newReg(); Opnd s = reg(ax.nr, F); s.abs = true;
                emit(alu1(OP_MOV, ctl(), reg(d, F), s));                            // |x|
                Val r = unop((Op)69, F, binop(OP_ADD, F, regVal(d, F), immF(0.5f))); // floor(|x| + 0.5)
                Val c = compare(C_L, F, x, immF(0.0f));
                flagFrom(c);
                uint8_t e = newReg(); Opnd nr2 = reg(r.nr, F); nr2.neg = true;
                emit(alu1(OP_MOV, ctl(), reg(e, F), nr2));
                uint8_t o = newReg(); Ctl k = ctl(); k.pred = 1;
                emit(alu2(OP_SEL, k, reg(o, F), reg(e, F), reg(r.nr, F)));
                return regVal(o, F);
            }
        }
        if (isFloat && n == 2) {
            // Gfx12's math unit has no POW (Mesa: lower_fpow for ver >= 12; using it
            // hung the GPU in v0.27.0): pow(x, y) = exp2(y * log2(x))
            if (op == "pow" || op == "powr")
                return fres(math(M_EXP, F, binop(OP_MUL, F, math(M_LOG, F, arg(0), nullptr), arg(1)), nullptr));
            if (op == "atan2") return fres(atan2v(arg(0), arg(1)));       // v0.51.8
            if (op == "ldexp") {                                    // v0.68.28: x * 2^e (e an int; Skia Graphite)
                Val e = mov(F, arg(1), D);
                return fres(binop(OP_MUL, F, arg(0), math(M_EXP, F, e, nullptr)));
            }
            if (op == "copysign") {
                Val m = binop(OP_AND, UD, arg(0), immU(0x7fffffff));
                Val sbit = binop(OP_AND, UD, arg(1), immU(0x80000000u));
                Val r = binop(OP_OR, UD, m, sbit); r.type = F;
                return r;
            }
            if (op == "fmod") {                                     // v0.68.31: x - y * trunc(x / y)
                Val x = arg(0), y = arg(1);
                Val q = unop((Op)71, F, fdiv(x, y));
                return fres(binop(OP_ADD, F, x, binop(OP_MUL, F, y, q), true));
            }
            if (op == "fdim") { Val d = binop(OP_ADD, F, arg(0), arg(1), true); return minmax(true, F, d, immF(0.0f)); }
        }
        // v0.68.31: integer pairs (from the old driver's translator list)
        if (!isFloat && n == 2 && (op == "abs_diff" || op == "hadd" || op == "rhadd")) {
            Type t = sgn ? D : UD;
            Val a = sgn ? sext(arg(0), ib) : arg(0), b = sgn ? sext(arg(1), ib) : arg(1);
            if (op == "abs_diff") return canon(binop(OP_ADD, t, minmax(true, t, a, b), minmax(false, t, a, b), true), ib);
            Val h = binop(OP_ADD, t, binop(sgn ? OP_ASR : OP_SHR, t, a, immU(1)), binop(sgn ? OP_ASR : OP_SHR, t, b, immU(1)));
            Val lo = binop(OP_AND, UD, binop(op == "hadd" ? OP_AND : OP_OR, UD, a, b), immU(1));
            return canon(binop(OP_ADD, t, h, lo), ib);
        }
        // integer bit operations
        if (!isFloat && n == 1) {
            if (op == "popcount" || op == "ctpop") return unop((Op)77, UD, arg(0));             // cbit
            if (op == "clz" || op == "ctlz") {
                Val r = unop((Op)74, UD, arg(0));                                              // lzd (32-bit)
                return ib && ib < 32 ? binop(OP_ADD, D, r, immU(32 - ib), true) : r;
            }
            if (op == "ctz" || op == "cttz") {
                // fbl gives 0xFFFFFFFF for 0: Metal/LLVM want the bit width
                Val r = unop((Op)76, UD, arg(0));
                return minmax(false, UD, r, immU(ib ? ib : 32));
            }
            if (op == "reverse_bits" || op == "bitreverse") {
                Val r = unop((Op)119, UD, arg(0));
                return ib && ib < 32 ? binop(OP_SHR, UD, r, immU(32 - ib), false, false) : r;
            }
        }
        // two-argument forms: llvm.ctlz/cttz(x, is_zero_poison), air.clz/ctz.i32(x, i1)
        if (!isFloat && n == 2 && (op == "ctlz" || op == "cttz" || op == "clz" || op == "ctz")) {
            bool lead = op == "ctlz" || op == "clz";
            if (lead) {
                Val r = unop((Op)74, UD, arg(0));
                return ib && ib < 32 ? binop(OP_ADD, D, r, immU(32 - ib), true) : r;
            }
            return minmax(false, UD, unop((Op)76, UD, arg(0)), immU(ib ? ib : 32));
        }
        if (op == "convert" && n == 1) {
            // air.convert.<to kind>.<to type>.<from kind>.<from type>  (kinds f / s / u)
            char tk = 0, fk = 0; char tt[8] = "", ft[8] = "";
            if (sscanf(sfx.c_str(), "%c.%7[^.].%c.%7s", &tk, tt, &fk, ft) == 4) {
                auto bitsOf = [](const char *t) { return atoi(t + (t[0] == 'v' ? 2 : 1)); };
                bool toF = tk == 'f', fromF = fk == 'f';
                const char *tts = strchr(tt, 'f') ? strchr(tt, 'f') : strchr(tt, 'i') ? strchr(tt, 'i') : tt;
                const char *fts = strchr(ft, 'f') ? strchr(ft, 'f') : strchr(ft, 'i') ? strchr(ft, 'i') : ft;
                int tb = bitsOf(tts), fb = bitsOf(fts);
                Val x = arg(0);
                if (!fromF && fb == 1) x = binop(OP_AND, UD, x, immU(1));            // bool 0/-1 -> 0/1
                else if (!fromF && fk == 's') x = sext(x, fb);
                Val r;
                if (toF && fromF) r = x;
                else if (toF) r = mov(F, x, fk == 's' ? D : UD);
                else if (fromF) r = mov(tk == 's' ? D : UD, x, F);
                else r = x;
                if (toF && tb == 16) r = roundHalf(r);
                if (!toF) r = canon(r, tb);
                return r;
            }
        }
        if (name.rfind("llvm.memset.", 0) == 0) {           // constant size: dword stores
            LLVMValueRef lenV = LLVMGetOperand(I, 2), valV = LLVMGetOperand(I, 1);
            if (!LLVMIsAConstantInt(lenV) || !LLVMIsAConstantInt(valV)) fail("memset with a run-time size or value");
            uint64_t len = LLVMConstIntGetZExtValue(lenV);
            uint32_t b = (uint32_t)LLVMConstIntGetZExtValue(valV) & 0xFF;
            if (len % 4 || len > 4096) fail("memset of %llu bytes is not supported yet", (unsigned long long)len);
            LLVMValueRef p = LLVMGetOperand(I, 0);
            uint32_t bti = btiOf(p, "memset of", true);
            Val base = inReg(get(p), UD);
            uint8_t d = newReg();
            emit(alu1(OP_MOV, ctl(), reg(d, UD), imm(b * 0x01010101u, UD)));
            for (uint64_t k = 0; k < len; k += 4) {
                Val a = k ? binop(OP_ADD, UD, base, immU((uint32_t)k)) : base;
                sendNow(SFID_HDC1, descUntyped(true, 1, bti), 1 << 6, -1, a.nr, d);
            }
            return Val();
        }
        if (name.rfind("llvm.lifetime.", 0) == 0 || name.rfind("llvm.assume", 0) == 0 ||
            name.rfind("llvm.dbg.", 0) == 0 || name.rfind("llvm.experimental.noalias", 0) == 0) return Val();
        fail("function %s is not supported yet", name.c_str());
    }

    // ---- instructions --------------------------------------------------------------
    bool lanewise(LLVMOpcode op) {
        switch (op) {
        case LLVMAdd: case LLVMSub: case LLVMMul: case LLVMAnd: case LLVMOr: case LLVMXor:
        case LLVMShl: case LLVMLShr: case LLVMAShr: case LLVMUDiv: case LLVMSDiv: case LLVMURem: case LLVMSRem:
        case LLVMFAdd: case LLVMFSub: case LLVMFMul: case LLVMFDiv: case LLVMFRem: case LLVMFNeg:
        case LLVMICmp: case LLVMFCmp: case LLVMSelect: case LLVMZExt: case LLVMSExt: case LLVMTrunc:
        case LLVMBitCast: case LLVMSIToFP: case LLVMUIToFP: case LLVMFPToSI: case LLVMFPToUI:
        case LLVMFPTrunc: case LLVMFPExt: case LLVMCall: case LLVMFreeze:
            return true;
        default: return false;
        }
    }
    LLVMValueRef curI = nullptr;
    // v0.51.8: spilling. origIdx = original layout index of every instruction (before any spill was applied);
    // when the kernel runs out of registers, newSpills gets the values worth moving to scratch memory.
    std::map<LLVMValueRef, int> origIdx;
    std::vector<int> newSpills;
    LLVMValueRef curFn = nullptr;
    void pickSpills() {
        if (!curFn || !curI) return;
        std::map<LLVMValueRef, int> pos;
        int n = 0;
        for (LLVMBasicBlockRef b = LLVMGetFirstBasicBlock(curFn); b; b = LLVMGetNextBasicBlock(b))
            for (LLVMValueRef I = LLVMGetFirstInstruction(b); I; I = LLVMGetNextInstruction(I)) pos[I] = n++;
        int cur = pos.count(curI) ? pos[curI] : 0;
        std::vector<std::pair<int, std::pair<int, int>>> cand;      // distance, (orig index, regs)
        std::set<LLVMValueRef> seen;
        auto consider = [&](LLVMValueRef v) {
            if (!seen.insert(v).second || released.count(v) || lazy.count(v)) return;
            if (!LLVMIsAInstruction(v)) return;
            LLVMOpcode op = LLVMGetInstructionOpcode(v);
            if (op == LLVMAlloca) return;
            { LLVMTypeRef st = LLVMTypeOf(v); if (LLVMGetTypeKind(st) == LLVMVectorTypeKind) st = LLVMGetElementType(st);
              LLVMTypeKind k = LLVMGetTypeKind(st);
              if (!(k == LLVMFloatTypeKind || k == LLVMHalfTypeKind || (k == LLVMIntegerTypeKind && LLVMGetIntTypeWidth(st) > 1 && LLVMGetIntTypeWidth(st) <= 32))) return; }
            auto oi = origIdx.find(v);
            if (oi == origIdx.end()) return;                        // a reload: never spilled again
            std::vector<uint8_t> rg = regsOf(v);
            if (rg.size() < 1) return;
            int best = 1 << 28;
            for (LLVMUseRef u = LLVMGetFirstUse(v); u; u = LLVMGetNextUse(u)) {
                auto pi = pos.find(LLVMGetUser(u));
                if (pi == pos.end()) continue;
                int d = pi->second - cur;
                if (d <= 0) d += 1 << 20;                           // earlier in the layout: only reached through a loop
                if (d < best) best = d;
            }
            if (best < 4) return;
            cand.push_back({best, {oi->second, (int)rg.size()}});
        };
        for (auto &kv : vals) consider(kv.first);
        for (auto &kv : vvals) consider(kv.first);
        std::sort(cand.begin(), cand.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
        int regs = 0;
        for (auto &c : cand) { newSpills.push_back(c.second.first); regs += c.second.second; if (regs >= 48) break; }
    }
    void instruction(LLVMValueRef I) {
        curI = I;
        LLVMOpcode op = LLVMGetInstructionOpcode(I);
        LLVMTypeRef rt = LLVMTypeOf(I);
        bool vecRes = isVec(rt);
        bool vecOps = LLVMGetNumOperands(I) > 0 && isVec(LLVMTypeOf(LLVMGetOperand(I, 0)));
        switch (op) {
        case LLVMExtractElement: {
            LLVMValueRef ix = LLVMGetOperand(I, 1);
            if (!LLVMIsAConstantInt(ix)) {                   // v0.51.7: index known only at run time: a chain of selects
                std::vector<Val> c = getv(LLVMGetOperand(I, 0));
                Type t = typeOf(rt);
                Val idx = get(ix), r = c.back();
                for (int k = (int)c.size() - 2; k >= 0; k--) r = selectVal(compare(C_Z, UD, idx, immU((uint32_t)k)), c[k], r, t);
                vals[I] = r;
                return;
            }
            std::vector<Val> c = getv(LLVMGetOperand(I, 0));
            unsigned k = (unsigned)LLVMConstIntGetZExtValue(ix);
            if (k >= c.size()) fail("vector index out of range");
            vals[I] = c[k];
            return;
        }
        case LLVMInsertElement: {
            LLVMValueRef ix = LLVMGetOperand(I, 2);
            if (!LLVMIsAConstantInt(ix)) {                   // v0.51.7: every element keeps its value unless the index is its own
                std::vector<Val> c = getv(LLVMGetOperand(I, 0));
                Type t = typeOf(scalarTy(rt));
                Val idx = get(ix), nv = get(LLVMGetOperand(I, 1));
                for (size_t k = 0; k < c.size(); k++) c[k] = selectVal(compare(C_Z, UD, idx, immU((uint32_t)k)), nv, c[k], t);
                vvals[I] = c;
                return;
            }
            std::vector<Val> c = getv(LLVMGetOperand(I, 0));
            unsigned k = (unsigned)LLVMConstIntGetZExtValue(ix);
            if (k >= c.size()) fail("vector index out of range");
            c[k] = get(LLVMGetOperand(I, 1));
            vvals[I] = c;
            return;
        }
        case LLVMShuffleVector: {
            std::vector<Val> a = getv(LLVMGetOperand(I, 0)), b = getv(LLVMGetOperand(I, 1)), r;
            a.insert(a.end(), b.begin(), b.end());
            unsigned m = LLVMGetNumMaskElements(I);
            for (unsigned k = 0; k < m; k++) {
                int e = LLVMGetMaskValue(I, k);
                if (e < 0 || e >= (int)a.size()) r.push_back(immU(0)); else r.push_back(a[e]);
            }
            if (isVec(rt)) vvals[I] = r; else vals[I] = r[0];
            return;
        }
        case LLVMLoad: {
            // v0.68.32: a load straight from a constant global with a known value (Tint's tint_volatile_zero, used
            // in every Dawn loop): the value itself
            {
                LLVMValueRef gp = LLVMGetOperand(I, 0);
                while (LLVMIsAConstantExpr(gp) && (LLVMGetConstOpcode(gp) == LLVMBitCast || LLVMGetConstOpcode(gp) == LLVMAddrSpaceCast))
                    gp = LLVMGetOperand(gp, 0);
                if (LLVMIsAGlobalVariable(gp) && LLVMIsGlobalConstant(gp) && LLVMGetInitializer(gp) && !slmOff.count(gp)) {
                    LLVMValueRef init = LLVMGetInitializer(gp);
                    LLVMTypeRef it = LLVMTypeOf(init);
                    LLVMTypeKind ik = LLVMGetTypeKind(it);
                    if ((ik == LLVMIntegerTypeKind || ik == LLVMFloatTypeKind || ik == LLVMHalfTypeKind) && it == rt) {
                        vals[I] = get(init);
                        return;
                    }
                    if (ik == LLVMVectorTypeKind && it == rt) { vvals[I] = getv(init); return; }
                }
            }
            if (blockConstLoads(I)) return;
            std::vector<Val> r = loadv(I);
            if (vecRes) vvals[I] = r; else vals[I] = r[0];
            return;
        }
        case LLVMInsertValue: {                           // milestone 5b: structs (shader outputs) as flat component lists
            if (LLVMGetNumIndices(I) != 1) fail("insertvalue with nested indices is not supported yet");
            std::vector<Val> c = structVals(LLVMGetOperand(I, 0));
            unsigned k = LLVMGetIndices(I)[0], at = fieldStart(rt, k), n = compsOf(aggElem(rt, k));
            if (c.size() < compsOf(rt)) c.resize(compsOf(rt), immU(0));    // v0.68.28: a shorter start value (Tint)
            LLVMValueRef fv = LLVMGetOperand(I, 1);
            LLVMTypeRef ft = aggElem(rt, k);
            // v0.68.27: a struct inside the struct (Brave / Tint shaders) - its components, flattened
            std::vector<Val> f = isAgg(ft) ? structVals(fv) : getv(fv);
            if (f.size() != n && (LLVMIsUndef(fv) || LLVMIsPoison(fv) || LLVMIsAConstantAggregateZero(fv)))
                f.assign(n, immU(0));
            if (f.size() != n || at + n > c.size())
                fail("insertvalue: field %u (type kind %d) has %zu components, expected %u", k, (int)LLVMGetTypeKind(ft), f.size(), n);
            for (unsigned i = 0; i < n; i++) c[at + i] = f[i];
            vvals[I] = c;
            return;
        }
        case LLVMExtractValue: {                          // {<4 x T>, i8} from texture reads/samples
            LLVMValueRef agg = LLVMGetOperand(I, 0);
            if (LLVMGetNumIndices(I) != 1) fail("extractvalue with nested indices is not supported yet");
            unsigned k = LLVMGetIndices(I)[0];
            if (isAgg(LLVMTypeOf(agg)) && !LLVMIsACallInst(agg)) {   // 5b: our struct values (v0.68.30: + arrays)
                LLVMTypeRef st = LLVMTypeOf(agg);
                std::vector<Val> c = structVals(agg);
                if (c.size() < compsOf(st)) c.resize(compsOf(st), immU(0));
                unsigned at = fieldStart(st, k), n = compsOf(aggElem(st, k));
                std::vector<Val> f(c.begin() + at, c.begin() + at + n);
                if (vecRes || isAgg(rt)) vvals[I] = f; else vals[I] = f[0];
                return;
            }
            if (k == 0 && vvals.count(agg)) {
                if (vecRes) vvals[I] = vvals[agg]; else vals[I] = vvals[agg][0];     // v0.37.1: {float, i8} (depth)
                return;
            }
            if (k == 1) { vals[I] = immU(0); return; }   // residency status: always resident
            fail("extractvalue not supported here: %s", valName(I).c_str());
        }
        default: break;
        }
        if (op == LLVMCall && calledName(I) == "air.discard_fragment") {    // 5b: these lanes stop here
            if (stage != XE_STAGE_FRAGMENT) fail("discard_fragment outside a fragment function");
            Ctl c = ctl(); c.cond = C_NZ; c.flag = 2;
            emit(alu2(OP_CMP, c, nullr(UD), reg(0, UD), reg(0, UD)));          // f1.0 = 0 for the lanes running here
            usesDiscard = true;
            return;
        }
        if (op == LLVMBitCast && reshapeBitcast(I)) return;               // v0.48.2
        if (op == LLVMCall && (vecRes || vecOps) && vectorCall(I)) return;
        if ((vecRes || vecOps) && op != LLVMStore) {
            if (!lanewise(op)) fail("vector instruction not supported yet: %s", valName(I).c_str());
            if (op == LLVMBitCast && vecN(rt) != vecN(LLVMTypeOf(LLVMGetOperand(I, 0))))
                fail("bitcast between vectors of different lengths is not supported yet");
            unsigned nc = vecRes ? vecN(rt) : vecN(LLVMTypeOf(LLVMGetOperand(I, 0)));
            std::vector<Val> r;
            for (unsigned k = 0; k < nc; k++) { curComp = (int)k; r.push_back(compute(I)); }
            curComp = -1;
            if (vecRes) vvals[I] = r; else fail("vector to scalar operation not supported yet: %s", valName(I).c_str());
            return;
        }
        Val r = compute(I);
        if (op != LLVMStore && op != LLVMRet && op != LLVMBr) vals[I] = r;
    }
    // v0.48.2: bitcasts that change the element size (<2 x half> <-> i32, i32 <-> <4 x i8>, <4 x half> <-> <2 x i32> ..)
    // or reinterpret half bits. Each lane keeps an element in 32 bits (half as float, small ints zero-extended),
    // so this goes through the raw bits: half -> its 16 bits (mov to HF, read as UW), then shifts / masks / ors.
    static unsigned elemBits(LLVMTypeRef t) {
        LLVMTypeRef e = scalarTy(t);
        switch (LLVMGetTypeKind(e)) {
        case LLVMHalfTypeKind: return 16;
        case LLVMFloatTypeKind: return 32;
        case LLVMIntegerTypeKind: return LLVMGetIntTypeWidth(e);
        default: return 0;
        }
    }
    Val halfToBits(const Val &v) {
        Val x = inReg(v, F);
        uint8_t h = newReg(), d = newReg();
        emit(alu1(OP_MOV, ctl(), reg(h, HF), reg(x.nr, F)));
        emit(alu1(OP_MOV, ctl(), reg(d, UD), reg(h, UW)));
        return regVal(d, UD);
    }
    Val bitsToHalf(const Val &v) {
        Val x = inReg(v, UD);
        uint8_t h = newReg(), d = newReg();
        emit(alu1(OP_MOV, ctl(), reg(h, UW), reg(x.nr, UD)));
        emit(alu1(OP_MOV, ctl(), reg(d, F), reg(h, HF)));
        return regVal(d, F);
    }
    bool reshapeBitcast(LLVMValueRef I) {
        LLVMTypeRef rt = LLVMTypeOf(I), st = LLVMTypeOf(LLVMGetOperand(I, 0));
        if (LLVMGetTypeKind(scalarTy(rt)) == LLVMPointerTypeKind || LLVMGetTypeKind(scalarTy(st)) == LLVMPointerTypeKind) return false;
        unsigned sb = elemBits(st), db = elemBits(rt), sn = vecN(st), dn = vecN(rt);
        bool sh = isHalf(st), dh = isHalf(rt);
        if (sb == db && sh == dh) return false;               // same shape: the plain path
        if (sb == db && !sh && !dh) return false;
        if (!sb || !db || sb > 32 || db > 32 || sb * sn != db * dn)
            fail("bitcast %s is not supported yet", valName(I).c_str());
        std::vector<Val> src = isVec(st) ? getv(LLVMGetOperand(I, 0)) : std::vector<Val>{ get(LLVMGetOperand(I, 0)) };
        std::vector<Val> bits;
        for (auto &v : src) {
            if (sh) bits.push_back(halfToBits(v));
            else if (sb < 32) bits.push_back(binop(OP_AND, UD, v, immU((1u << sb) - 1)));
            else { Val x = v; x.type = UD; bits.push_back(x); }
        }
        std::vector<Val> out;
        for (unsigned j = 0; j < dn; j++) {
            Val r;
            if (db >= sb) {                                    // combine db / sb source elements
                unsigned per = db / sb;
                for (unsigned i = 0; i < per; i++) {
                    Val b = bits[j * per + i];
                    if (i) b = binop(OP_SHL, UD, b, immU(sb * i), false, false);
                    r = i ? binop(OP_OR, UD, r, b) : b;
                }
            } else {                                           // a piece of one source element
                unsigned per = sb / db, i = j % per;
                Val b = bits[j / per];
                if (i) b = binop(OP_SHR, UD, b, immU(db * i), false, false);
                r = binop(OP_AND, UD, b, immU((1u << db) - 1));
            }
            if (dh) r = bitsToHalf(r);
            else r.type = LLVMGetTypeKind(scalarTy(rt)) == LLVMFloatTypeKind ? F : UD;
            out.push_back(r);
        }
        if (isVec(rt)) vvals[I] = out; else vals[I] = out[0];
        return true;
    }
    // geometric functions on whole vectors: dot, length, distance, normalize, cross
    bool vectorCall(LLVMValueRef I) {
        size_t len = 0;
        const char *nm = LLVMGetValueName2(LLVMGetCalledValue(I), &len);
        std::string name = nm ? std::string(nm, len) : "";
        std::string b = name.rfind("air.", 0) == 0 ? name.substr(4) : name;
        if (b.rfind("fast_", 0) == 0) b = b.substr(5);
        std::string op = b.substr(0, b.find('.'));
        auto dot = [&](const std::vector<Val> &x, const std::vector<Val> &y) {
            Val acc = binop(OP_MUL, F, x[0], y[0]);
            for (size_t k = 1; k < x.size(); k++) acc = binop(OP_ADD, F, acc, binop(OP_MUL, F, x[k], y[k]));
            return acc;
        };
        bool h = isHalf(LLVMTypeOf(I));
        auto fr = [&](const Val &v) { return h ? roundHalf(v) : v; };
        if (op == "dot") { vals[I] = fr(dot(getv(LLVMGetOperand(I, 0)), getv(LLVMGetOperand(I, 1)))); return true; }
        if (op == "length") { auto x = getv(LLVMGetOperand(I, 0)); vals[I] = fr(math(M_SQRT, F, dot(x, x), nullptr)); return true; }
        if (op == "distance") {
            auto x = getv(LLVMGetOperand(I, 0)), y = getv(LLVMGetOperand(I, 1));
            for (size_t k = 0; k < x.size(); k++) x[k] = binop(OP_ADD, F, x[k], y[k], true);
            vals[I] = fr(math(M_SQRT, F, dot(x, x), nullptr));
            return true;
        }
        if (op == "normalize") {
            auto x = getv(LLVMGetOperand(I, 0));
            Val r = math(M_RSQ, F, dot(x, x), nullptr);
            for (auto &c : x) c = fr(binop(OP_MUL, F, c, r));
            vvals[I] = x;
            return true;
        }
        // v0.51.2: any() / all() of a bool vector (Core Animation's shaders): booleans are 0 / -1 (or 0 / 1)
        if (op == "any" || op == "all") {
            auto x = getv(LLVMGetOperand(I, 0));
            if (x.empty()) fail("%s of nothing", name.c_str());
            Val acc = x[0];
            for (size_t k = 1; k < x.size(); k++) acc = binop(op == "any" ? OP_OR : OP_AND, UD, acc, x[k]);
            vals[I] = acc;
            return true;
        }
        // v0.48.1: pack_unorm4x8 / snorm4x8 / unorm2x16 / snorm2x16 (vector -> uint) and the unpacks (uint -> vector)
        if (op == "pack" || op == "unpack") {
            std::string kind = b.substr(b.find('.') + 1);         // unorm4x8.v4f16 ...
            bool snorm = kind.rfind("snorm", 0) == 0;
            unsigned bits = kind.find("4x8") != std::string::npos ? 8 : kind.find("2x16") != std::string::npos ? 16 : 0;
            if ((!snorm && kind.rfind("unorm", 0) != 0) || !bits) fail("%s is not supported yet", name.c_str());
            unsigned cnt = 32 / bits;
            float scale = snorm ? (float)((1u << (bits - 1)) - 1) : (float)((1u << bits) - 1);
            uint32_t mask = bits == 8 ? 0xFFu : 0xFFFFu;
            if (op == "pack") {
                auto x = getv(LLVMGetOperand(I, 0));
                Val acc = immU(0);
                for (unsigned k = 0; k < cnt && k < x.size(); k++) {
                    Val v = inReg(x[k], F);
                    uint8_t c = newReg();
                    if (snorm) {                                     // clamp(v, -1, 1)
                        Ctl lo = ctl(); lo.cond = C_GE;
                        emit(alu2(OP_SEL, lo, reg(c, F), reg(v.nr, F), imm(0xBF800000u, F)));
                        Ctl hi = ctl(); hi.cond = C_L;
                        emit(alu2(OP_SEL, hi, reg(c, F), reg(c, F), imm(0x3F800000u, F)));
                    } else {
                        Ctl k1 = ctl(); k1.sat = true;
                        emit(alu1(OP_MOV, k1, reg(c, F), reg(v.nr, F)));
                    }
                    Val m = binop(OP_MUL, F, regVal(c, F), immF(scale));
                    // round to nearest: +-0.5 then convert (truncates toward zero)
                    Val half_ = immF(0.5f);
                    Val r;
                    if (snorm) {
                        Val c0 = compare(C_L, F, m, immF(0.0f)); flagFrom(c0);
                        uint8_t hh = newReg(); Ctl k2 = ctl(); k2.pred = 1; k2.flag = 0;
                        Val mp = inReg(immF(-0.5f), F);
                        emit(alu2(OP_SEL, k2, reg(hh, F), reg(mp.nr, F), imm(0x3F000000u, F)));
                        r = mov(D, binop(OP_ADD, F, m, regVal(hh, F)), F);
                    } else r = mov(UD, binop(OP_ADD, F, m, half_), F);
                    r = binop(OP_AND, UD, r, immU(mask));
                    if (k) r = binop(OP_SHL, UD, r, immU(bits * k), false, false);
                    acc = k ? binop(OP_OR, UD, acc, r) : r;
                }
                vals[I] = acc;
                return true;
            }
            Val u = get(LLVMGetOperand(I, 0));
            std::vector<Val> r;
            for (unsigned k = 0; k < cnt; k++) {
                Val f;
                if (snorm) {                                         // sign-extend the field, / scale, max -1
                    Val sh = binop(OP_SHL, UD, u, immU(32 - bits * (k + 1)), false, false);
                    Val sx = binop(OP_ASR, D, sh, immU(32 - bits), false, false);
                    Val fv = binop(OP_MUL, F, mov(F, sx, D), immF(1.0f / scale));
                    Val fr2 = inReg(fv, F); uint8_t c = newReg();
                    Ctl lo = ctl(); lo.cond = C_GE;
                    emit(alu2(OP_SEL, lo, reg(c, F), reg(fr2.nr, F), imm(0xBF800000u, F)));
                    f = regVal(c, F);
                } else {
                    Val fld = binop(OP_AND, UD, k ? binop(OP_SHR, UD, u, immU(bits * k), false, false) : u, immU(mask));
                    f = binop(OP_MUL, F, mov(F, fld, UD), immF(1.0f / scale));
                }
                r.push_back(fr(f));
            }
            unsigned want = vecN(LLVMTypeOf(I));
            while (r.size() < want) r.push_back(immF(0.0f));
            r.resize(want);
            vvals[I] = r;
            return true;
        }
        if (op == "cross") {
            auto x = getv(LLVMGetOperand(I, 0)), y = getv(LLVMGetOperand(I, 1));
            if (x.size() < 3) fail("cross of a %zu-component vector", x.size());
            auto c = [&](int i, int j) { return binop(OP_ADD, F, binop(OP_MUL, F, x[i], y[j]), binop(OP_MUL, F, x[j], y[i]), true); };
            std::vector<Val> r = { fr(c(1, 2)), fr(c(2, 0)), fr(c(0, 1)) };
            while (r.size() < x.size()) r.push_back(immF(0.0f));
            vvals[I] = r;
            return true;
        }
        return false;
    }
    Val compute(LLVMValueRef I) {
        LLVMOpcode op = LLVMGetInstructionOpcode(I);
        LLVMTypeRef rt = LLVMTypeOf(I);
        unsigned rb = intBits(rt);                         // result integer width (0 = not an int)
        bool half = isHalf(rt);
        auto o = [&](unsigned i) { return get(LLVMGetOperand(I, i)); };
        auto obits = [&](unsigned i) { return intBits(LLVMTypeOf(LLVMGetOperand(I, i))); };
        auto fres = [&](const Val &v) { return half ? roundHalf(v) : v; };
        Val r;
        switch (op) {
        case LLVMAdd:  return canon(binop(OP_ADD, UD, o(0), o(1)), rb);
        case LLVMSub:  return canon(binop(OP_ADD, D, o(0), o(1), true), rb);
        case LLVMMul:  return canon(imul(o(0), o(1)), rb);
        case LLVMAnd:  return binop(OP_AND, UD, o(0), o(1));
        case LLVMOr:   return binop(OP_OR,  UD, o(0), o(1));
        case LLVMXor:  return binop(OP_XOR, UD, o(0), o(1));
        case LLVMShl:  return canon(binop(OP_SHL, UD, o(0), o(1), false, false), rb);
        case LLVMLShr: return binop(OP_SHR, UD, o(0), o(1), false, false);
        case LLVMAShr: return canon(binop(OP_ASR, D, sext(o(0), rb), o(1), false, false), rb);
        case LLVMUDiv: return intDivRem(false, false, o(0), o(1), rb);
        case LLVMSDiv: return intDivRem(false, true, o(0), o(1), rb);
        case LLVMURem: return intDivRem(true, false, o(0), o(1), rb);
        case LLVMSRem: return intDivRem(true, true, o(0), o(1), rb);
        case LLVMFAdd: return fres(binop(OP_ADD, F, o(0), o(1)));
        case LLVMFSub: return fres(binop(OP_ADD, F, o(0), o(1), true));
        case LLVMFMul: return fres(binop(OP_MUL, F, o(0), o(1)));
        case LLVMFDiv: return fres(fdiv(o(0), o(1)));
        case LLVMFRem: {                                   // a - b * trunc(a / b)
            Val q = unop((Op)71, F, fdiv(o(0), o(1)));
            return fres(binop(OP_ADD, F, o(0), binop(OP_MUL, F, q, o(1)), true));
        }
        case LLVMFNeg: {
            Val x = inReg(o(0), F);
            uint8_t d = newReg();
            Opnd s2 = reg(x.nr, F); s2.neg = true;
            emit(alu1(OP_MOV, ctl(), reg(d, F), s2));
            return regVal(d, F);
        }
        case LLVMICmp: {
            LLVMIntPredicate p = LLVMGetICmpPredicate(I);
            Cond c; Type t = D; bool sgn = false;
            switch (p) {
            case LLVMIntEQ: c = C_Z; break;   case LLVMIntNE: c = C_NZ; break;
            case LLVMIntUGT: c = C_G; t = UD; break; case LLVMIntUGE: c = C_GE; t = UD; break;
            case LLVMIntULT: c = C_L; t = UD; break; case LLVMIntULE: c = C_LE; t = UD; break;
            case LLVMIntSGT: c = C_G; sgn = true; break;  case LLVMIntSGE: c = C_GE; sgn = true; break;
            case LLVMIntSLT: c = C_L; sgn = true; break;  case LLVMIntSLE: c = C_LE; sgn = true; break;
            default: fail("compare kind not supported");
            }
            unsigned b = obits(0);
            Val x = o(0), y = o(1);
            if (sgn) { x = sext(x, b); y = sext(y, b); }
            return compare(c, t, x, y);
        }
        case LLVMFCmp: {
            LLVMRealPredicate p = LLVMGetFCmpPredicate(I);
            Cond c;
            switch (p) {
            case LLVMRealOEQ: case LLVMRealUEQ: c = C_Z; break;
            case LLVMRealONE: case LLVMRealUNE: c = C_NZ; break;
            case LLVMRealOGT: case LLVMRealUGT: c = C_G; break;
            case LLVMRealOGE: case LLVMRealUGE: c = C_GE; break;
            case LLVMRealOLT: case LLVMRealULT: c = C_L; break;
            case LLVMRealOLE: case LLVMRealULE: c = C_LE; break;
            case LLVMRealORD: case LLVMRealUNO: {          // NaN test: x == x and y == y
                Val a = compare(C_Z, F, o(0), o(0)), b = compare(C_Z, F, o(1), o(1));
                Val both = binop(OP_AND, UD, a, b);
                return p == LLVMRealORD ? both : binop(OP_XOR, UD, both, immU(0xFFFFFFFFu));
            }
            case LLVMRealPredicateTrue: return immU(0xFFFFFFFFu);
            case LLVMRealPredicateFalse: return immU(0);
            default: fail("float compare kind %d not supported yet", (int)p);
            }
            return compare(c, F, o(0), o(1));
        }
        case LLVMSelect: {
            Type t = typeOf(rt);
            Val c = o(0), a = o(1), b = o(2);
            if (c.kind == Val::IMM) return c.imm ? a : b;
            flagFrom(c);
            bool inv = false;
            if (a.kind == Val::IMM && b.kind != Val::IMM) { std::swap(a, b); inv = true; }
            a = notImm(a, t);
            uint8_t d = newReg();
            Ctl k = ctl(); k.pred = 1; k.predInv = inv; k.flag = 0;
            emit(alu2(OP_SEL, k, reg(d, t), opnd(a, t), opnd(b, t)));
            return regVal(d, t);
        }
        case LLVMZExt: {
            Val x = o(0);
            if (obits(0) == 1) return x.kind == Val::IMM ? immU(x.imm & 1) : binop(OP_AND, UD, x, immU(1));
            return x;                                      // canonical values are zero-extended already
        }
        case LLVMSExt: {
            Val x = sext(o(0), obits(0));
            return canon(x, rb);
        }
        case LLVMTrunc: {
            Val x = o(0);
            if (rb == 1) {                                 // -> i1: bit 0
                Val bit = binop(OP_AND, UD, x, immU(1));
                return compare(C_NZ, UD, bit, immU(0));
            }
            return canon(x, rb);
        }
        case LLVMFreeze:
        case LLVMBitCast: case LLVMPtrToInt: case LLVMIntToPtr: case LLVMAddrSpaceCast: {
            Val x = o(0);
            if (op == LLVMBitCast && (isHalf(rt) || isHalf(LLVMTypeOf(LLVMGetOperand(I, 0)))) && typeOf(rt) != typeOf(LLVMTypeOf(LLVMGetOperand(I, 0))))
                fail("bitcast between half and a 16-bit integer is not supported yet");
            x.type = typeOf(rt);
            return x;
        }
        case LLVMFPTrunc: return half ? roundHalf(o(0)) : o(0);
        case LLVMFPExt: return o(0);
        case LLVMSIToFP: return fres(mov(F, sext(o(0), obits(0)), D));
        case LLVMUIToFP: return fres(mov(F, o(0), UD));
        case LLVMFPToSI: return canon(mov(D, o(0), F), rb);
        case LLVMFPToUI: return canon(mov(UD, o(0), F), rb);
        case LLVMGetElementPtr: return gep(I);
        case LLVMStore: store(I); return Val();
        case LLVMAtomicRMW: return atomicRMW(I);
        case LLVMAlloca: {
            uint32_t off = privOff[I];
            return off ? binop(OP_ADD, UD, regVal(privReg, UD), immU(off)) : regVal(privReg, UD);
        }
        case LLVMFence: fences(7); return Val();
        case LLVMAtomicCmpXchg: fail("compare-and-exchange atomics are not supported yet");
        case LLVMCall: return call(I);
        case LLVMRet: case LLVMBr: return Val();
        case LLVMPHI: case LLVMSwitch:
            fail("internal: phi/switch reached the instruction emitter");
        default:
            fail("instruction not supported yet: %s", valName(I).c_str());
        }
        return r;
    }

    // ---- control flow (milestone 3d) -------------------------------------------
    // SIMD8: the 8 lanes of a hardware thread can take different ways, so
    // branches become the GPU's structured IF / ELSE / ENDIF and loops become
    // (break ...) WHILE with per-lane masks. We rebuild that structure from
    // LLVM's CFG with dominators / post-dominators and natural loops; phi
    // nodes become register copies on the edges into their block.
    struct Loop { LLVMBasicBlockRef header = nullptr, latch = nullptr, exit = nullptr; std::vector<bool> in; };
    std::vector<LLVMBasicBlockRef> blocks;
    std::map<LLVMBasicBlockRef, int> bidx;
    std::vector<std::vector<int>> succ, pred;
    std::vector<int> idom, ipdom;            // -1 = none; ipdom: blocks.size() = virtual exit
    std::map<int, Loop> loops;               // by header index
    std::vector<int> retBlocks;

    // iterative dominators (Cooper, Harvey, Kennedy) on a numbering in reverse post order
    std::vector<int> dominators(int n, int root, const std::vector<std::vector<int>> &sx, const std::vector<std::vector<int>> &px) {
        std::vector<int> order, post(n, -1), d(n, -1);
        std::vector<bool> seen(n, false);
        std::vector<std::pair<int, size_t>> st{{root, 0}};
        seen[root] = true;
        while (!st.empty()) {
            auto &t = st.back();
            if (t.second < sx[t.first].size()) {
                int v = sx[t.first][t.second++];
                if (!seen[v]) { seen[v] = true; st.push_back({v, 0}); }
            } else { post[t.first] = (int)order.size(); order.push_back(t.first); st.pop_back(); }
        }
        d[root] = root;
        bool changed = true;
        while (changed) {
            changed = false;
            for (int k = (int)order.size() - 1; k >= 0; k--) {
                int v = order[k];
                if (v == root) continue;
                int nd = -1;
                for (int p : px[v]) {
                    if (d[p] < 0) continue;
                    if (nd < 0) { nd = p; continue; }
                    int x = p, y = nd;
                    while (x != y) {
                        while (post[x] < post[y]) x = d[x];
                        while (post[y] < post[x]) y = d[y];
                    }
                    nd = x;
                }
                if (nd != d[v]) { d[v] = nd; changed = true; }
            }
        }
        return d;
    }
    bool dominates(int a, int b) {                       // a dominates b
        while (b >= 0) { if (a == b) return true; if (idom[b] == b) return false; b = idom[b]; }
        return false;
    }
    void analyze(LLVMValueRef fn) {
        for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(fn); bb; bb = LLVMGetNextBasicBlock(bb)) {
            bidx[bb] = (int)blocks.size(); blocks.push_back(bb);
        }
        int n = (int)blocks.size();
        succ.assign(n + 1, {}); pred.assign(n + 1, {});
        for (int i = 0; i < n; i++) {
            LLVMValueRef t = LLVMGetBasicBlockTerminator(blocks[i]);
            if (!t) fail("block without terminator");
            LLVMOpcode op = LLVMGetInstructionOpcode(t);
            if (op == LLVMRet) { retBlocks.push_back(i); continue; }
            if (op != LLVMBr) fail("%s is not supported yet (only if/else and loops)", op == LLVMSwitch ? "switch" : "this kind of jump");
            for (unsigned k = 0; k < LLVMGetNumSuccessors(t); k++) {
                int s2 = bidx[LLVMGetSuccessor(t, k)];
                succ[i].push_back(s2); pred[s2].push_back(i);
            }
        }
        if (retBlocks.size() != 1) fail("%zu return points: early return is not supported yet", retBlocks.size());
        idom = dominators(n, 0, succ, pred);
        // post-dominators: reverse graph from a virtual exit n linked to the return block
        std::vector<std::vector<int>> rs(n + 1), rp(n + 1);
        for (int i = 0; i < n; i++) for (int s2 : succ[i]) { rs[s2].push_back(i); rp[i].push_back(s2); }
        for (int r : retBlocks) { rs[n].push_back(r); rp[r].push_back(n); }
        ipdom = dominators(n + 1, n, rs, rp);
        // natural loops: back edge u -> h with h dominating u
        for (int u = 0; u < n; u++) for (int h : succ[u]) {
            if (!dominates(h, u)) continue;
            if (loops.count(h)) fail("loop with more than one back edge (continue) is not supported yet");
            Loop L; L.header = blocks[h]; L.latch = blocks[u]; L.in.assign(n, false);
            std::vector<int> work{u}; L.in[h] = true;
            while (!work.empty()) {
                int v = work.back(); work.pop_back();
                if (L.in[v]) continue;
                L.in[v] = true;
                for (int p : pred[v]) work.push_back(p);
            }
            int exitB = -1;
            for (int v = 0; v < n; v++) if (L.in[v]) for (int s2 : succ[v]) if (!L.in[s2]) {
                if (exitB >= 0 && exitB != s2) fail("loop with more than one exit block is not supported yet");
                exitB = s2;
            }
            if (exitB < 0) fail("loop without an exit (endless loop)");
            L.exit = blocks[exitB];
            loops[h] = L;
        }
    }

    // ---- register reuse (v0.29.0) ---------------------------------------------------
    // Every use of a value is counted up front (phis also count each block that
    // writes them). After the last one the value's registers are free again,
    // except inside a loop the value was defined outside of: then they stay
    // until that loop's WHILE (the next iteration needs them). Header phis are
    // made at the edge into the loop, so this covers them too. Temporaries
    // inside one instruction are freed right after it. Every instruction waits
    // for the one before (@1) and every send is waited for, so reusing a
    // register can't race with an older instruction.
    // switch -> a chain of conditional branches, one per distinct target (cases
    // with the same target are or'ed together, so every block keeps one entry
    // from the chain); phis of the targets are rebuilt for the new edges
    // the value of case i (successor i; 0 is the default). LLVM 22 stopped
    // storing case values as operands (they were operand 2 * i before)
    LLVMValueRef caseValue(LLVMValueRef sw, unsigned i) {
#if LLVM_VERSION_MAJOR >= 22
        LLVMValueRef v = LLVMGetSwitchCaseValue(sw, i);
#else
        LLVMValueRef v = LLVMGetOperand(sw, 2 * i);
#endif
        if (!v || !LLVMIsAConstantInt(v)) fail("switch: could not read the value of case %u", i);
        return v;
    }
    void lowerSwitches(LLVMValueRef fn) {
        std::vector<LLVMValueRef> sws;
        for (LLVMBasicBlockRef b = LLVMGetFirstBasicBlock(fn); b; b = LLVMGetNextBasicBlock(b)) {
            LLVMValueRef t = LLVMGetBasicBlockTerminator(b);
            if (t && LLVMGetInstructionOpcode(t) == LLVMSwitch) sws.push_back(t);
        }
        if (sws.empty()) return;
        LLVMBuilderRef bld = LLVMCreateBuilderInContext(ctx);
        for (LLVMValueRef sw : sws) {
            LLVMBasicBlockRef S = LLVMGetInstructionParent(sw);
            LLVMValueRef cond = LLVMGetOperand(sw, 0);
            LLVMBasicBlockRef def = LLVMGetSwitchDefaultDest(sw);
            unsigned ns = LLVMGetNumSuccessors(sw);
            std::vector<LLVMBasicBlockRef> dests;                 // distinct case targets, in order
            std::map<LLVMBasicBlockRef, std::vector<LLVMValueRef>> vals;
            for (unsigned i = 1; i < ns; i++) {
                LLVMBasicBlockRef d = LLVMGetSuccessor(sw, i);
                if (d == def) continue;
                if (!vals.count(d)) dests.push_back(d);
                vals[d].push_back(caseValue(sw, i));
            }
            // old phi values coming from S, by target
            std::vector<LLVMBasicBlockRef> targets = dests;
            targets.push_back(def);
            std::map<LLVMValueRef, LLVMValueRef> fromS;           // phi -> its value from S
            for (LLVMBasicBlockRef d : targets)
                for (LLVMValueRef I = LLVMGetFirstInstruction(d); I && LLVMGetInstructionOpcode(I) == LLVMPHI; I = LLVMGetNextInstruction(I))
                    for (unsigned k = 0; k < LLVMCountIncoming(I); k++)
                        if (LLVMGetIncomingBlock(I, k) == S) { fromS[I] = LLVMGetIncomingValue(I, k); break; }
            // the chain: S tests the first target, a new block per further target
            std::vector<std::pair<LLVMBasicBlockRef, LLVMBasicBlockRef>> edges;   // (from, to)
            LLVMBasicBlockRef cur = S;
            LLVMPositionBuilderBefore(bld, sw);
            for (size_t k = 0; k < dests.size(); k++) {
                LLVMValueRef c = nullptr;
                for (LLVMValueRef v : vals[dests[k]]) {
                    LLVMValueRef e = LLVMBuildICmp(bld, LLVMIntEQ, cond, v, "sw.eq");
                    c = c ? LLVMBuildOr(bld, c, e, "sw.or") : e;
                }
                LLVMBasicBlockRef next = k + 1 < dests.size() ? LLVMAppendBasicBlockInContext(ctx, fn, "sw.next") : def;
                LLVMBuildCondBr(bld, c, dests[k], next);
                edges.push_back({cur, dests[k]});
                edges.push_back({cur, next});
                if (k + 1 < dests.size()) { cur = next; LLVMPositionBuilderAtEnd(bld, cur); }
            }
            if (dests.empty()) { LLVMBuildBr(bld, def); edges.push_back({S, def}); }
            LLVMInstructionEraseFromParent(sw);
            // rebuild the phis: entries from S -> entries from the chain blocks that reach the target
            for (auto &pv : fromS) {
                LLVMValueRef old = pv.first;
                LLVMBasicBlockRef d = LLVMGetInstructionParent(old);
                std::vector<LLVMValueRef> iv; std::vector<LLVMBasicBlockRef> ib;
                for (unsigned k = 0; k < LLVMCountIncoming(old); k++)
                    if (LLVMGetIncomingBlock(old, k) != S) { iv.push_back(LLVMGetIncomingValue(old, k)); ib.push_back(LLVMGetIncomingBlock(old, k)); }
                for (auto &e : edges) if (e.second == d) { iv.push_back(pv.second); ib.push_back(e.first); }
                LLVMPositionBuilderBefore(bld, old);
                LLVMValueRef np = LLVMBuildPhi(bld, LLVMTypeOf(old), "sw.phi");
                LLVMAddIncoming(np, iv.data(), ib.data(), (unsigned)iv.size());
                LLVMReplaceAllUsesWith(old, np);
                LLVMInstructionEraseFromParent(old);
            }
        }
        LLVMDisposeBuilder(bld);
    }
    // private arrays: an offset in each lane's scratch area
    void allocPrivate(LLVMValueRef fn) {
        for (LLVMBasicBlockRef b = LLVMGetFirstBasicBlock(fn); b; b = LLVMGetNextBasicBlock(b))
            for (LLVMValueRef I = LLVMGetFirstInstruction(b); I; I = LLVMGetNextInstruction(I)) {
                if (LLVMGetInstructionOpcode(I) != LLVMAlloca) continue;
                LLVMValueRef cnt = LLVMGetOperand(I, 0);
                if (!LLVMIsAConstantInt(cnt)) fail("private array with a run-time size");
                LLVMTypeRef t = LLVMGetAllocatedType(I);
                uint64_t al = typeAlign(t); if (al < 4) al = 4;
                uint64_t off = (privBytes + al - 1) / al * al;
                uint64_t sz = typeSize(t) * LLVMConstIntGetZExtValue(cnt);
                privOff[I] = (uint32_t)off;
                if (off + sz > 4000) fail("private arrays: more than 4000 bytes per thread are not supported yet");
                privBytes = (uint32_t)(off + sz);
            }
        privBytes = (privBytes + 15) & ~15u;
    }

    void traceRegion(LLVMBasicBlockRef b, LLVMBasicBlockRef stop, Loop *L) {
        while (b && b != stop) {
            if (++traceSteps > 400000) fail("control flow too complex");
            int bi = bidx[b];
            auto lit = loops.find(bi);
            if (lit != loops.end() && !(L && L->header == b)) {
                Loop &lp = lit->second;
                traceRegion(b, nullptr, &lp);
                b = lp.exit;
                continue;
            }
            emits[b]++;
            LLVMValueRef t = LLVMGetBasicBlockTerminator(b);
            if (LLVMGetInstructionOpcode(t) == LLVMRet) return;
            if (!LLVMIsConditional(t)) {
                LLVMBasicBlockRef s2 = LLVMGetSuccessor(t, 0);
                edgeN[{b, s2}]++;
                if (L && (s2 == L->header || s2 == L->exit)) return;
                b = s2;
                continue;
            }
            LLVMBasicBlockRef bt = LLVMGetSuccessor(t, 0), bf = LLVMGetSuccessor(t, 1);
            if (L && (bt == L->exit || bf == L->exit)) {
                LLVMBasicBlockRef other = bt == L->exit ? bf : bt;
                edgeN[{b, L->exit}]++;
                edgeN[{b, other}]++;
                if (other == L->header) return;
                b = other;
                continue;
            }
            if (L && (bt == L->header || bf == L->header)) return;
            int m = ipdom[bi];
            LLVMBasicBlockRef mb = m < (int)blocks.size() ? blocks[m] : nullptr;
            if (!mb) return;
            if (bt == mb && bf == mb) { b = mb; continue; }
            if (bt == mb || bf == mb) edgeN[{b, mb}]++;
            if (bt != mb) { edgeN[{b, bt}]++; traceRegion(bt, mb, L); }
            if (bf != mb) { edgeN[{b, bf}]++; traceRegion(bf, mb, L); }
            b = mb;
        }
    }
    void countUses(LLVMValueRef fn) {
        traceRegion(LLVMGetEntryBasicBlock(fn), nullptr, nullptr);
        if (getenv("XEC_STATS")) {
            long blk = 0, em = 0, insU = 0, insE = 0; int maxE = 0;
            for (LLVMBasicBlockRef b = LLVMGetFirstBasicBlock(fn); b; b = LLVMGetNextBasicBlock(b)) {
                int n = 0; for (LLVMValueRef I = LLVMGetFirstInstruction(b); I; I = LLVMGetNextInstruction(I)) n++;
                int e = emits.count(b) ? emits[b] : 0;
                blk++; em += e; insU += n; insE += (long)n * e; if (e > maxE) maxE = e;
            }
            fprintf(stderr, "stats: blocks %ld emissions %ld, IR instructions %ld emitted %ld, max emits of one block %d\n", blk, em, insU, insE, maxE);
        }
        std::map<LLVMValueRef, int> total;
        std::map<LLVMValueRef, LLVMBasicBlockRef> home;
        for (LLVMBasicBlockRef b = LLVMGetFirstBasicBlock(fn); b; b = LLVMGetNextBasicBlock(b))
            for (LLVMValueRef I = LLVMGetFirstInstruction(b); I; I = LLVMGetNextInstruction(I)) {
                total.emplace(I, 0);
                home[I] = b;
                int e = emits.count(b) ? emits[b] : 0;
                if (LLVMGetInstructionOpcode(I) == LLVMPHI) {
                    std::set<LLVMBasicBlockRef> from;
                    for (unsigned k = 0; k < LLVMCountIncoming(I); k++) {
                        LLVMBasicBlockRef f = LLVMGetIncomingBlock(I, k);
                        LLVMValueRef o = LLVMGetIncomingValue(I, k);
                        int n = edgeN.count({f, b}) ? edgeN[{f, b}] : 0;
                        if (o && (LLVMIsAInstruction(o) || LLVMIsAArgument(o))) total[o] += n;
                        if (from.insert(f).second) total[I] += n;
                    }
                    continue;
                }
                for (int k = 0; k < LLVMGetNumOperands(I); k++) {
                    LLVMValueRef o = LLVMGetOperand(I, k);
                    if (o && (LLVMIsAInstruction(o) || LLVMIsAArgument(o))) total[o] += e;
                }
            }
        for (auto &kv : total) {
            LLVMValueRef v = kv.first;
            int n = kv.second;
            if (LLVMIsAInstruction(v) && LLVMGetInstructionOpcode(v) != LLVMPHI) {
                int e = emits.count(home[v]) ? emits[home[v]] : 0;
                if (e > 1) n = n % e ? (1 << 28) : n / e;        // uneven: keep its registers (safe)
                perDef[v] = n;
            } else if (LLVMIsAInstruction(v)) {                   // v0.51.9: a phi in a block emitted e times: per copy
                int e = emits.count(home[v]) ? emits[home[v]] : 0;
                if (e > 1 && n % e == 0) { n /= e; perDef[v] = n; }
            }
            remaining[v] = n;                                     // phis and arguments: all emissions together
        }
    }
    std::vector<uint8_t> regsOf(LLVMValueRef v) {
        std::vector<uint8_t> r;
        auto add = [&](const Val &x) { if (x.kind == Val::REG && x.nr >= REG_FIRST) r.push_back(x.nr); };
        auto a = vals.find(v); if (a != vals.end()) add(a->second);
        auto b = vvals.find(v); if (b != vvals.end()) for (const Val &x : b->second) add(x);
        return r;
    }
    void own(LLVMValueRef v) {
        if (!spilled.empty() && spilled.count(v)) dropSpill(v);
        defRegion[v] = regionStack;
        if (!defined.insert(v).second) {                         // defined again (block emitted twice)
            released.erase(v);
            auto pd = perDef.find(v);
            if (pd != perDef.end()) remaining[v] = pd->second;
        }
        for (uint8_t r : regsOf(v)) ref[r]++;
        defStack[v] = loopStack;
        auto it = remaining.find(v);
        if (it != remaining.end() && it->second <= 0) done(v);       // never used
    }
    void release(LLVMValueRef v) {
        if (!released.insert(v).second) return;
        if (!spilled.empty() && spilled.count(v)) dropSpill(v);
        for (uint8_t r : regsOf(v)) if (--ref[r] <= 0) { ref[r] = 0; inUse[r] = false; }
    }
    void done(LLVMValueRef v) {
        const std::vector<int> &d = defStack[v];
        size_t i = 0;
        while (i < d.size() && i < loopStack.size() && d[i] == loopStack[i]) i++;
        if (i < loopStack.size()) deferred[loopStack[i]].push_back(v);   // keep until that loop ends
        else release(v);
    }
    void dec(LLVMValueRef v) {
        if (lazy.count(v)) return;
        auto it = remaining.find(v);
        if (it == remaining.end() || it->second <= 0) return;
        if (--it->second == 0) {
            if (spilled.count(v)) { if (vals.count(v) || vvals.count(v)) release(v); else { released.insert(v); dropSpill(v); } }
            else if (vals.count(v) || vvals.count(v)) done(v);
        }
    }
    void commit() {
        for (LLVMValueRef v : rematted) { vals.erase(v); vvals.erase(v); }
        rematted.clear();
        for (uint8_t r : pending) if (ref[r] == 0) inUse[r] = false;
        pending.clear();
    }

    // phi registers and edge copies (parallel copy through temporaries)
    std::vector<Val> phiVal(LLVMValueRef phi) {
        LLVMTypeRef t = LLVMTypeOf(phi);
        if (released.count(phi) && perDef.count(phi)) {             // v0.51.9: the next copy of its block: new registers
            vals.erase(phi); vvals.erase(phi);
        }
        if (!spilled.empty() && spilled.count(phi)) {              // a duplicated block: a new instance of the phi
            rematted.erase(std::remove(rematted.begin(), rematted.end(), phi), rematted.end());
            vals.erase(phi); vvals.erase(phi); dropSpill(phi);
        }
        touched.insert(phi);
        if (isVec(t)) {
            auto it = vvals.find(phi);
            if (it != vvals.end()) return it->second;
            std::vector<Val> c;
            for (unsigned k = 0; k < vecN(t); k++) c.push_back(regVal(newReg(), typeOf(t)));
            vvals[phi] = c;
            own(phi);
            return c;
        }
        auto it = vals.find(phi);
        if (it != vals.end()) return { it->second };
        Val v = regVal(newReg(), typeOf(t));
        vals[phi] = v;
        own(phi);
        return { v };
    }
    // v0.51.9: coalescing: a phi not yet in registers takes over its incoming value's registers when that
    // value dies on this edge (sole owner, not kept for a loop)
    bool adoptInto(LLVMValueRef phi, LLVMValueRef v) {
        if (getenv("XEC_NOADOPT")) return false;
        bool fresh = (!vals.count(phi) && !vvals.count(phi)) || (released.count(phi) && perDef.count(phi));
        if (!fresh || spilled.count(phi)) return false;
        if (!LLVMIsAInstruction(v) || LLVMGetInstructionOpcode(v) == LLVMPHI || lazy.count(v) || isSpilledNow(v)) return false;
        if (released.count(v)) return false;
        auto rm = remaining.find(v);
        if (rm == remaining.end() || rm->second != 1) return false;
        auto ds = defStack.find(v);
        if (ds == defStack.end() || ds->second != loopStack) return false;
        if (LLVMTypeOf(v) != LLVMTypeOf(phi)) return false;
        bool vec = isVec(LLVMTypeOf(v));
        if (vec ? !vvals.count(v) : !vals.count(v)) return false;
        std::vector<Val> c = vec ? vvals[v] : std::vector<Val>{ vals[v] };
        std::set<uint8_t> seen;
        for (const Val &x : c) {
            if (x.kind != Val::REG || x.nr < REG_FIRST || !seen.insert(x.nr).second) return false;
            if (ref[x.nr] != 1 || std::find(pending.begin(), pending.end(), x.nr) != pending.end()) return false;
        }
        vals.erase(phi); vvals.erase(phi);
        if (vec) vvals[phi] = c; else vals[phi] = c[0];
        own(phi);                                         // ref 2 until v's last use (this edge) releases it
        touched.insert(phi);
        return true;
    }
    // v0.51.9: a value whose only use is a phi that already has registers (the other side of an if wrote
    // it first) is computed straight into the phi's registers: its instructions (plain ALU only) are
    // renamed. Safe because the writes only touch this path's lanes; the edge copy then has nothing to do.
    static bool renameOK(unsigned op) {
        return op == 1 || op == 56 || (op >= 64 && op <= 72) || op == 74 || op == 76 || op == 77 || op == 97 || op == 98 ||
               (op >= 100 && op <= 105) || op == 108 || op == 112 || op == 119;
    }
    void redirectToPhi(LLVMValueRef I, size_t start) {
        if (getenv("XEC_NOREDIR") || start >= code.size()) { redirWhy[1]++; return; }
        LLVMUseRef u = LLVMGetFirstUse(I);
        if (!u || LLVMGetNextUse(u)) { redirWhy[2]++; return; }
        LLVMValueRef P = LLVMGetUser(u);
        if (!LLVMIsAInstruction(P) || LLVMGetInstructionOpcode(P) != LLVMPHI) { redirWhy[3]++; return; }
        if (loops.count(bidx[LLVMGetInstructionParent(P)])) { redirWhy[4]++; return; }
        if (released.count(P) || spilled.count(P) || lazy.count(I)) { redirWhy[5]++; return; }
        bool vec = isVec(LLVMTypeOf(I));
        if (vec ? !vvals.count(I) || !vvals.count(P) : !vals.count(I) || !vals.count(P)) { redirWhy[6]++; return; }
        std::vector<Val> ci = vec ? vvals[I] : std::vector<Val>{ vals[I] };
        std::vector<Val> cp = vec ? vvals[P] : std::vector<Val>{ vals[P] };
        if (ci.size() != cp.size()) { redirWhy[7]++; return; }
        std::map<uint8_t, uint8_t> mp;
        std::set<uint8_t> targets;
        for (size_t k = 0; k < ci.size(); k++) {
            if (ci[k].kind != Val::REG) continue;
            if (cp[k].kind != Val::REG || cp[k].nr < REG_FIRST || ci[k].nr < REG_FIRST) { redirWhy[8]++; return; }
            if (std::find(pending.begin(), pending.end(), ci[k].nr) == pending.end() || ref[ci[k].nr]) { redirWhy[9]++; return; }   // fresh, this step
            auto f = mp.find(ci[k].nr);
            if (f != mp.end() && f->second != cp[k].nr) { redirWhy[10]++; return; }
            if (!targets.insert(cp[k].nr).second && f == mp.end()) { redirWhy[11]++; return; }
            mp[ci[k].nr] = cp[k].nr;
        }
        if (mp.empty()) { redirWhy[12]++; return; }
        auto bit = [](const Inst &x, unsigned b) { return b < 64 ? (x.lo >> b) & 1 : (x.hi >> (b - 64)) & 1; };
        auto field = [](const Inst &x, unsigned lo) { return (unsigned)(lo < 64 ? (x.lo >> lo) & 0xFF : (x.hi >> (lo - 64)) & 0xFF); };
        for (size_t k = start; k < code.size(); k++) {          // check: only plain ALU, the phi's registers untouched
            const Inst &x = code[k];
            if (!renameOK((unsigned)(x.lo & 0x7F))) { redirWhy[13]++; return; }
            if (bit(x, 50) && targets.count((uint8_t)field(x, 56))) { redirWhy[14]++; return; }
            if (!bit(x, 46) && bit(x, 66) && targets.count((uint8_t)field(x, 72))) { redirWhy[15]++; return; }
            if (!bit(x, 47) && bit(x, 98) && targets.count((uint8_t)field(x, 104))) { redirWhy[16]++; return; }
        }
        for (size_t k = start; k < code.size(); k++) {
            Inst &x = code[k];
            if (bit(x, 50)) { auto f = mp.find((uint8_t)field(x, 56)); if (f != mp.end()) setBits(x, 63, 56, f->second); }
            if (!bit(x, 46) && bit(x, 66)) { auto f = mp.find((uint8_t)field(x, 72)); if (f != mp.end()) setBits(x, 79, 72, f->second); }
            if (!bit(x, 47) && bit(x, 98)) { auto f = mp.find((uint8_t)field(x, 104)); if (f != mp.end()) setBits(x, 111, 104, f->second); }
        }
        for (Val &c : ci) if (c.kind == Val::REG) { auto f = mp.find(c.nr); if (f != mp.end()) c.nr = f->second; }
        if (mp.count(cmpReg)) cmpReg = mp[cmpReg];
        if (vec) vvals[I] = ci; else vals[I] = ci[0];
        redirected++;
    }
    long redirected = 0;
    std::map<int, long> redirWhy;
    void edgeCopies(LLVMBasicBlockRef from, LLVMBasicBlockRef to) {
        touched.clear();
        { auto lp = layoutPos.find(LLVMGetBasicBlockTerminator(from)); if (lp != layoutPos.end()) curPos = lp->second; }
        std::vector<std::pair<Val, Val>> moves;     // dst phi register, source value
        for (LLVMValueRef I = LLVMGetFirstInstruction(to); I && LLVMGetInstructionOpcode(I) == LLVMPHI; I = LLVMGetNextInstruction(I)) {
            bool found = false;
            for (unsigned k = 0; k < LLVMCountIncoming(I); k++) {
                if (LLVMGetIncomingBlock(I, k) != from) continue;
                if (adoptInto(I, LLVMGetIncomingValue(I, k))) { found = true; break; }   // v0.51.9: no copy
                if (getenv("XEC_STATS")) {
                    LLVMValueRef v = LLVMGetIncomingValue(I, k); std::string w;
                    unsigned nc = isVec(LLVMTypeOf(I)) ? vecN(LLVMTypeOf(I)) : 1;
                    bool fresh = (!vals.count(I) && !vvals.count(I)) || (released.count(I) && perDef.count(I));
                    if (!LLVMIsAInstruction(v)) w = LLVMIsAArgument(v) ? "arg" : "const";
                    else if (LLVMGetInstructionOpcode(v) == LLVMPHI) w = "phi";
                    else if (!fresh) w = "phi-has-regs";
                    else if (remaining.count(v) && remaining[v] != 1) w = "src-lives-on";
                    else w = "other";
                    edgeWhy[w] += nc;
                }
                std::vector<Val> dst = phiVal(I), src = getv(LLVMGetIncomingValue(I, k));
                for (size_t c = 0; c < dst.size(); c++) moves.push_back({dst[c], src[c]});
                found = true;
                break;
            }
            if (!found) fail("phi without a value for one of its blocks");
        }
        // v0.51.9: sequentialized parallel copy: a move goes when no other pending move still reads its
        // destination; a cycle is broken by saving one destination in a temporary
        moves.erase(std::remove_if(moves.begin(), moves.end(), [](const std::pair<Val, Val> &m) {
            return m.second.kind == Val::REG && m.second.nr == m.first.nr; }), moves.end());
        while (!moves.empty()) {
            size_t pick = moves.size();
            for (size_t k = 0; k < moves.size() && pick == moves.size(); k++) {
                bool read = false;
                for (size_t j = 0; j < moves.size() && !read; j++)
                    if (j != k && moves[j].second.kind == Val::REG && moves[j].second.nr == moves[k].first.nr) read = true;
                if (!read) pick = k;
            }
            if (pick == moves.size()) {                    // a cycle: save moves[0]'s destination
                uint8_t d = moves[0].first.nr, t = newReg();
                emit(alu1(OP_MOV, ctl(), reg(t, UD), reg(d, UD)));
                for (auto &m : moves) if (m.second.kind == Val::REG && m.second.nr == d) m.second.nr = t;
                pick = 0;
            }
            auto m = moves[pick];
            edgeMoves++;
            emit(alu1(OP_MOV, ctl(), reg(m.first.nr, m.first.type), opnd(m.second, m.first.type)));
            moves.erase(moves.begin() + pick);
        }
        // this edge is done: its values were read, its phis written
        for (LLVMValueRef I = LLVMGetFirstInstruction(to); I && LLVMGetInstructionOpcode(I) == LLVMPHI; I = LLVMGetNextInstruction(I)) {
            bool any = false;
            for (unsigned k = 0; k < LLVMCountIncoming(I); k++)
                if (LLVMGetIncomingBlock(I, k) == from) { dec(LLVMGetIncomingValue(I, k)); any = true; }
            if (any) dec(I);
        }
        commit();
    }

    void emitBody(LLVMBasicBlockRef bb) {
        for (LLVMValueRef I = LLVMGetFirstInstruction(bb); I; I = LLVMGetNextInstruction(I)) {
            LLVMOpcode op = LLVMGetInstructionOpcode(I);
            if (op == LLVMPHI) { phiVal(I); defRegion[I] = regionStack; commit(); continue; }
            if (op == LLVMBr || op == LLVMRet) break;
            if (lazy.count(I)) continue;
            if (preloaded.erase(I)) {                      // v0.62.0: its value came with an earlier block read
                for (int k = 0; k < LLVMGetNumOperands(I); k++) {
                    LLVMValueRef o = LLVMGetOperand(I, k);
                    if (o && (LLVMIsAInstruction(o) || LLVMIsAArgument(o))) dec(o);
                }
                commit();
                continue;
            }
            touched.clear();
            { auto lp = layoutPos.find(I); if (lp != layoutPos.end()) curPos = lp->second; }
            size_t start = code.size();
            instruction(I);
            redirectToPhi(I, start);
            if (getenv("XEC_TRACE")) { char *pv = LLVMPrintValueToString(I); fprintf(stderr, "@%zu-%zu %.110s\n", start, code.size(), pv); LLVMDisposeMessage(pv); }
            own(I);
            for (int k = 0; k < LLVMGetNumOperands(I); k++) {
                LLVMValueRef o = LLVMGetOperand(I, k);
                if (o && (LLVMIsAInstruction(o) || LLVMIsAArgument(o))) dec(o);
            }
            commit();
        }
    }
    void cf(CfOp op, bool pred, bool inv, uint8_t flag) {
        emit(cfInst(op, pred, inv, flag, pred ? swsbDist(1) : 0, 0, 0));
    }
    // condition value -> f0.0, then a predicated control-flow instruction
    void cfOn(CfOp op, LLVMValueRef cond, bool invert) {
        Val c = get(cond);
        if (c.kind == Val::IMM) {                      // constant condition: plain (or no) jump
            bool t = (c.imm != 0) != invert;
            if (op == CF_BREAK && !t) return;
            if (op == CF_BREAK) { cf(CF_BREAK, false, false, 0); return; }
            fail("constant if condition (should have been folded)");
        }
        flagFrom(c);
        cf(op, true, invert, 0);
    }
    // the same, and the condition's use is done
    void cfOnUse(CfOp op, LLVMValueRef cond, bool invert) {
        cfOn(op, cond, invert);
        dec(cond);
        commit();
    }

    int depth = 0;
    // emit blocks from b until `stop`; `L` = innermost loop being emitted
    // v0.51.9: each if/else side, loop body and the rest of a loop body after a break: a region of its own
    void region(LLVMBasicBlockRef b, LLVMBasicBlockRef stop, Loop *L) {
        size_t keep = regionStack.size();
        regionStack.push_back(++regionIds);
        regionImpl(b, stop, L);
        regionStack.resize(keep);
    }
    void regionImpl(LLVMBasicBlockRef b, LLVMBasicBlockRef stop, Loop *L) {
        while (b && b != stop) {
            if (++depth > 400000) fail("control flow too complex");
            int bi = bidx[b];
            auto lit = loops.find(bi);
            if (lit != loops.end() && !(L && L->header == b)) {
                Loop &lp = lit->second;
                preEvict(bi * 2, lp.in);                  // v0.51.9
                size_t start = code.size();
                int id = ++loopIds;
                loopStack.push_back(id);
                loopHdr.push_back(bi * 2);
                region(b, nullptr, &lp);                  // ends at the latch's back edge
                loopHdr.pop_back();
                emit(cfInst(CF_WHILE, false, false, 0, 0, (int32_t)((long)start - (long)code.size()) * 16, 0));
                loopStack.pop_back();
                std::vector<LLVMValueRef> later = deferred[id];
                deferred.erase(id);
                for (LLVMValueRef v : later) done(v);     // may wait for an outer loop
                b = lp.exit;
                continue;
            }
            emitBody(b);
            LLVMValueRef t = LLVMGetBasicBlockTerminator(b);
            if (LLVMGetInstructionOpcode(t) == LLVMRet) {
                if (stop) fail("return inside an if/loop is not supported yet");
                return;
            }
            if (!LLVMIsConditional(t)) {
                LLVMBasicBlockRef s2 = LLVMGetSuccessor(t, 0);
                edgeCopies(b, s2);
                if (L && s2 == L->header) return;                       // end of the loop body
                if (L && s2 == L->exit) { cf(CF_BREAK, false, false, 0); return; }
                b = s2;
                continue;
            }
            LLVMValueRef cond = LLVMGetCondition(t);
            LLVMBasicBlockRef bt = LLVMGetSuccessor(t, 0), bf = LLVMGetSuccessor(t, 1);
            if (L && (bt == L->exit || bf == L->exit)) {                // leaving the loop: break
                bool exitOnTrue = bt == L->exit;
                LLVMBasicBlockRef other = exitOnTrue ? bf : bt;
                edgeCopies(b, L->exit);
                cfOnUse(CF_BREAK, cond, !exitOnTrue);
                regionStack.push_back(++regionIds);                     // v0.51.9: may be skipped (all lanes left)
                edgeCopies(b, other);
                if (other == L->header) return;
                b = other;
                continue;
            }
            if (L && (bt == L->header || bf == L->header)) fail("continue in the middle of a loop is not supported yet");
            int m = ipdom[bi];
            LLVMBasicBlockRef mb = m < (int)blocks.size() ? blocks[m] : nullptr;
            if (!mb) fail("if/else without a common end is not supported yet");
            if (L && !L->in[m]) fail("break inside an if is not supported yet");
            if (bt == mb && bf == mb) { dec(cond); b = mb; continue; }
            if (spillMode && loopHint.count(bi * 2 + 1)) preEvict(bi * 2 + 1, ifBlocks(bt, bf, mb));   // v0.51.9
            loopHdr.push_back(bi * 2 + 1);
            struct PopOnExit { std::vector<int> &v; size_t n; ~PopOnExit() { v.resize(n); } } popHdr{ loopHdr, loopHdr.size() - 1 };
            if (bt == mb || bf == mb) edgeCopies(b, mb);                // lanes going straight to the end
            if (bt == mb) {                                             // if (!c) { false side }
                cfOnUse(CF_IF, cond, true);
                size_t ifAt = code.size() - 1;
                edgeCopies(b, bf);
                region(bf, mb, L);
                closeIf(ifAt, (size_t)-1);
            } else if (bf == mb) {                                      // if (c) { true side }
                cfOnUse(CF_IF, cond, false);
                size_t ifAt = code.size() - 1;
                edgeCopies(b, bt);
                region(bt, mb, L);
                closeIf(ifAt, (size_t)-1);
            } else {                                                    // if (c) { .. } else { .. }
                cfOnUse(CF_IF, cond, false);
                size_t ifAt = code.size() - 1;
                edgeCopies(b, bt);
                region(bt, mb, L);
                cf(CF_ELSE, false, false, 0);
                size_t elseAt = code.size() - 1;
                edgeCopies(b, bf);
                region(bf, mb, L);
                closeIf(ifAt, elseAt);
            }
            b = mb;
        }
    }
    void closeIf(size_t ifAt, size_t elseAt) {
        cf(CF_ENDIF, false, false, 0);
        size_t endAt = code.size() - 1;
        if (elseAt == (size_t)-1) {
            cfSetJip(code[ifAt], (int32_t)(endAt - ifAt) * 16);
            cfSetUip(code[ifAt], (int32_t)(endAt - ifAt) * 16);
        } else {
            cfSetJip(code[ifAt], (int32_t)(elseAt - ifAt + 1) * 16);
            cfSetUip(code[ifAt], (int32_t)(endAt - ifAt) * 16);
            cfSetJip(code[elseAt], (int32_t)(endAt - elseAt) * 16);
            cfSetUip(code[elseAt], (int32_t)(endAt - elseAt) * 16);
        }
    }
    // Mesa brw_set_uip_jip: BREAK and ENDIF jump to the end of the innermost
    // enclosing block (next ENDIF / ELSE / WHILE); BREAK's UIP is its WHILE
    size_t nextBlockEnd(size_t from) {
        int d = 0;
        for (size_t k = from + 1; k < code.size(); k++) {
            unsigned o = opcodeOf(code[k]);
            if (o == HW_IF) d++;
            else if (o == HW_ENDIF) { if (d == 0) return k; d--; }
            else if (o == HW_WHILE) {
                int32_t j = (int32_t)(code[k].hi >> 32);
                if ((long)k + j / 16 > (long)from) continue;        // a sibling loop after us
                if (d == 0) return k;
            } else if (o == HW_ELSE) { if (d == 0) return k; }
        }
        return 0;
    }
    size_t loopEnd(size_t from) {
        for (size_t k = from + 1; k < code.size(); k++)
            if (opcodeOf(code[k]) == HW_WHILE && (long)k + (int32_t)(code[k].hi >> 32) / 16 <= (long)from) return k;
        fail("break outside a loop");
    }
    void fixJumps() {
        for (size_t k = 0; k < code.size(); k++) {
            unsigned o = opcodeOf(code[k]);
            if (o == HW_BREAK) {
                size_t e = nextBlockEnd(k);
                if (!e) fail("internal: break without block end");
                cfSetJip(code[k], (int32_t)(e - k) * 16);
                cfSetUip(code[k], (int32_t)(loopEnd(k) - k) * 16);
            } else if (o == HW_ENDIF) {
                size_t e = nextBlockEnd(k);
                cfSetJip(code[k], e ? (int32_t)(e - k) * 16 : 16);
            }
        }
    }


    // ---- v0.61.0: overlapping reads (memory and texture sends) ---------------------------------------------
    // Every read used to be "send @1 $0" + "sync.nop $0.dst": the thread waited for each result before going on, so a
    // shader with 100 texture samples paid the sampler's latency 100 times. Here each read gets its own token
    // ($3..$15) and the wait moves down to the first instruction that touches its result (or overwrites its message),
    // so up to 13 reads are in flight at once. At jumps, jump targets, writes, other sends and the end of the thread
    // every outstanding read is waited for first (conservative). Sync nops are not counted by the @n distances
    // (Mesa ordered_unit), so the inserted waits don't disturb the ALU dependencies. XEC_NOOVERLAP=1: off.
    static uint64_t ibits(const Inst &x, unsigned hi, unsigned lo) {
        uint64_t v = 0;
        for (unsigned b = lo; b <= hi; b++) {
            uint64_t bit = b < 64 ? (x.lo >> b) & 1 : (x.hi >> (b - 64)) & 1;
            v |= bit << (b - lo);
        }
        return v;
    }
    static unsigned tsize(unsigned t) {
        switch (t) { case 0: case 4: return 1; case 1: case 5: case 9: return 2; case 3: case 7: case 11: return 8; default: return 4; }
    }
    static unsigned stride(unsigned enc) { return enc ? 1u << (enc - 1) : 0; }
    // registers [a, b] an operand touches (conservative)
    struct RRange { int a = -1, b = -1; };
    static RRange dstRange(const Inst &x) {
        RRange r;
        if (!ibits(x, 50, 50)) return r;
        unsigned nr = (unsigned)ibits(x, 63, 56), sub = (unsigned)ibits(x, 55, 51);
        unsigned exec = 1u << ibits(x, 18, 16), ts = tsize((unsigned)ibits(x, 39, 36)), hs = stride((unsigned)ibits(x, 49, 48));
        unsigned bytes = exec * ts * (hs ? hs : 1);
        r.a = (int)nr; r.b = (int)(nr + (sub + bytes - 1) / 32);
        return r;
    }
    static RRange srcRange(const Inst &x, int which) {
        RRange r;
        unsigned immBit = which ? 47 : 46, fileBit = which ? 98 : 66;
        if (ibits(x, immBit, immBit) || !ibits(x, fileBit, fileBit)) return r;
        unsigned nr = which ? (unsigned)ibits(x, 111, 104) : (unsigned)ibits(x, 79, 72);
        unsigned sub = which ? (unsigned)ibits(x, 103, 99) : (unsigned)ibits(x, 71, 67);
        unsigned ts = tsize(which ? (unsigned)ibits(x, 91, 88) : (unsigned)ibits(x, 43, 40));
        unsigned vs = stride(which ? (unsigned)ibits(x, 119, 116) : (unsigned)ibits(x, 87, 84));
        unsigned w = 1u << (which ? ibits(x, 115, 113) : ibits(x, 83, 81));
        unsigned hs = stride(which ? (unsigned)ibits(x, 97, 96) : (unsigned)ibits(x, 65, 64));
        unsigned exec = 1u << ibits(x, 18, 16);
        unsigned rows = w ? (exec + w - 1) / w : exec;
        unsigned bytes = (rows ? rows - 1 : 0) * vs * ts + (w - 1) * hs * ts + ts;
        unsigned full = exec * ts * (hs ? hs : 1);               // also cover the plain <8;8,1> span (safety)
        if (vs || hs) bytes = std::max(bytes, full);
        r.a = (int)nr; r.b = (int)(nr + (sub + bytes - 1) / 32);
        return r;
    }
    static bool overlaps(RRange x, int a, int b) { return x.a >= 0 && a >= 0 && x.a <= b && a <= x.b; }

    size_t overlapped = 0, overlapWaits = 0;
    void overlapSends() {
        if (getenv("XEC_NOOVERLAP") || getenv("XEC_NOSB")) return;
        const size_t N = code.size();
        const Inst waitZero = syncNop(swsbDst(0));
        auto isCF = [](unsigned o) { return o >= 32 && o <= 47; };
        // absolute jump targets
        std::vector<char> target(N + 1, 0);
        struct J { size_t at; long jip = -1, uip = -1; };
        std::vector<J> jumps;
        for (size_t k = 0; k < N; k++) {
            unsigned o = opcodeOf(code[k]);
            if (!isCF(o)) continue;
            if (o != HW_IF && o != HW_ELSE && o != HW_ENDIF && o != HW_WHILE && o != HW_BREAK) return;   // unknown: leave as is
            J j; j.at = k;
            long jip = (long)(int32_t)(uint32_t)ibits(code[k], 127, 96) / 16, uip = (long)(int32_t)(uint32_t)ibits(code[k], 95, 64) / 16;
            j.jip = (long)k + jip;
            if (o == HW_IF || o == HW_ELSE || o == HW_BREAK) j.uip = (long)k + uip;
            if (j.jip < 0 || j.jip > (long)N || (j.uip != -1 && (j.uip < 0 || j.uip > (long)N))) return;
            target[(size_t)j.jip] = 1;
            if (j.uip >= 0) target[(size_t)j.uip] = 1;
            jumps.push_back(j);
        }
        struct Pend { bool on = false; RRange d, s0, s1; };
        Pend pend[16];
        unsigned nextTok = 3;
        std::vector<Inst> out;
        out.reserve(N + N / 4);
        std::vector<size_t> newIdx(N + 1, 0);
        auto wait = [&](unsigned t) { out.push_back(syncNop(swsbDst(t))); pend[t].on = false; overlapWaits++; };
        auto waitAll = [&]() { for (unsigned t = 3; t < 16; t++) if (pend[t].on) wait(t); };
        for (size_t k = 0; k < N; k++) {
            Inst I = code[k];
            unsigned o = opcodeOf(I);
            bool isSend = o == OP_SEND || o == 50;
            bool eot = isSend && ibits(I, 34, 34);
            bool movable = o == OP_SEND && !eot && ibits(I, 15, 8) == swsbDistSet(1, 0) && k + 1 < N &&
                           code[k + 1].lo == waitZero.lo && code[k + 1].hi == waitZero.hi && !target[k + 1] &&
                           ibits(I, 50, 50) && ibits(I, 55, 51) > 0;      // a read: it has a destination
            if (isCF(o) || target[k] || eot || (isSend && !movable)) waitAll();
            else if (o != OP_SYNC) {
                RRange d, s0, s1;
                if (isSend) {
                    d = dstRange(I);
                    if (d.a >= 0) d.b = d.a + (int)ibits(I, 55, 51) - 1;
                    s0.a = (int)ibits(I, 79, 72); s0.b = s0.a + std::max(1, (int)(ibits(I, 71, 67) & 0xF)) - 1;
                    unsigned xl = (unsigned)ibits(I, 103, 99);
                    if (xl) { s1.a = (int)ibits(I, 111, 104); s1.b = s1.a + (int)xl - 1; }
                } else { d = dstRange(I); s0 = srcRange(I, 0); s1 = srcRange(I, 1); }
                for (unsigned t = 3; t < 16; t++) {
                    if (!pend[t].on) continue;
                    Pend &p = pend[t];
                    bool hd = overlaps(p.d, d.a, d.b) || overlaps(p.d, s0.a, s0.b) || overlaps(p.d, s1.a, s1.b);
                    bool hs = overlaps(p.s0, d.a, d.b) || overlaps(p.s1, d.a, d.b);
                    if (hd) wait(t);
                    else if (hs) {                        // only overwrites its message: wait until it was read ($n.src)
                        out.push_back(syncNop(swsbSrc(t))); overlapWaits++;
                        p.s0 = RRange{}; p.s1 = RRange{};
                    }
                }
            }
            newIdx[k] = out.size();
            if (movable) {
                unsigned t = nextTok; nextTok = nextTok == 15 ? 3 : nextTok + 1;
                if (pend[t].on) wait(t);
                newIdx[k] = out.size();
                setBits(I, 15, 8, swsbDistSet(1, t));
                Pend &p = pend[t];
                p.on = true;
                p.d.a = (int)ibits(I, 63, 56); p.d.b = p.d.a + (int)ibits(I, 55, 51) - 1;
                p.s0.a = (int)ibits(I, 79, 72); p.s0.b = p.s0.a + std::max(1, (int)(ibits(I, 71, 67) & 0xF)) - 1;
                unsigned xl = (unsigned)ibits(I, 103, 99);
                if (xl) { p.s1.a = (int)ibits(I, 111, 104); p.s1.b = p.s1.a + (int)xl - 1; } else p.s1 = RRange{};
                out.push_back(I);
                newIdx[k + 1] = out.size();
                k++;                                      // its wait is dropped (placed where needed)
                overlapped++;
                continue;
            }
            out.push_back(I);
        }
        waitAll();
        newIdx[N] = out.size();
        for (const J &j : jumps) {
            size_t at = newIdx[j.at];
            cfSetJip(out[at], (int32_t)((long)newIdx[(size_t)j.jip] - (long)at) * 16);
            if (j.uip >= 0) cfSetUip(out[at], (int32_t)((long)newIdx[(size_t)j.uip] - (long)at) * 16);
        }
        code.swap(out);
    }

    // ---- milestone 5b: vertex and fragment functions -----------------------------
    XeShaderInfo rinfo = {};
    bool usesDiscard = false;
    std::string calledName(LLVMValueRef call) {
        size_t len = 0;
        const char *nm = LLVMGetValueName2(LLVMGetCalledValue(call), &len);
        return nm ? std::string(nm, len) : "";
    }
    // structs are kept as flat lists of components (a vector field = its components)
    unsigned compsOf(LLVMTypeRef t) {
        if (LLVMGetTypeKind(t) == LLVMStructTypeKind) {
            unsigned n = 0;
            for (unsigned i = 0; i < LLVMCountStructElementTypes(t); i++) n += compsOf(LLVMStructGetTypeAtIndex(t, i));
            return n;
        }
        if (LLVMGetTypeKind(t) == LLVMArrayTypeKind)                 // v0.68.29: arrays inside structs, flattened
            return LLVMGetArrayLength(t) * compsOf(LLVMGetElementType(t));
        return vecN(t);
    }
    // v0.68.30: arrays as aggregates too (Tint's float3x3 = [3 x <3 x float>])
    LLVMTypeRef aggElem(LLVMTypeRef t, unsigned k) {
        return LLVMGetTypeKind(t) == LLVMArrayTypeKind ? LLVMGetElementType(t) : LLVMStructGetTypeAtIndex(t, k);
    }
    static bool isAgg(LLVMTypeRef t) {
        LLVMTypeKind k = LLVMGetTypeKind(t);
        return k == LLVMStructTypeKind || k == LLVMArrayTypeKind;
    }
    unsigned fieldStart(LLVMTypeRef st, unsigned k) {
        if (LLVMGetTypeKind(st) == LLVMArrayTypeKind) return k * compsOf(LLVMGetElementType(st));
        unsigned n = 0;
        for (unsigned i = 0; i < k; i++) n += compsOf(LLVMStructGetTypeAtIndex(st, i));
        return n;
    }
    std::vector<Val> structVals(LLVMValueRef v) {
        LLVMTypeRef t = LLVMTypeOf(v);
        if (!isAgg(t)) return getv(v);
        auto it = vvals.find(v);
        if (it != vvals.end()) return it->second;
        if (LLVMIsUndef(v) || LLVMIsPoison(v) || LLVMIsAConstantAggregateZero(v))
            return std::vector<Val>(compsOf(t), immU(0));
        fail("struct value not supported yet: %s", valName(v).c_str());
    }
    std::string mdStr(LLVMValueRef md) {
        unsigned l = 0; const char *s = md ? LLVMGetMDString(md, &l) : nullptr;
        return s ? std::string(s, l) : "";
    }
    std::vector<LLVMValueRef> mdOps(LLVMValueRef md) {
        std::vector<LLVMValueRef> o(LLVMGetMDNodeNumOperands(md));
        LLVMGetMDNodeOperands(md, o.data());
        return o;
    }
    // "key" followed by an i32 in an argument's metadata
    bool mdInt(const std::vector<LLVMValueRef> &f, const char *key, unsigned &out) {
        for (size_t i = 0; i + 1 < f.size(); i++)
            if (mdStr(f[i]) == key && LLVMIsAConstantInt(f[i + 1])) { out = (unsigned)LLVMConstIntGetZExtValue(f[i + 1]); return true; }
        return false;
    }
    bool mdHas(const std::vector<LLVMValueRef> &f, const char *key) {
        for (LLVMValueRef x : f) if (mdStr(x) == key) return true;
        return false;
    }
    bool hasUses(LLVMValueRef v) { return LLVMGetFirstUse(v) != nullptr; }
    // components of a vector argument the code reads (extractelement / shufflevector), ~0 = all
    unsigned compsUsed(LLVMValueRef p) {
        unsigned m = 0;
        for (LLVMUseRef u = LLVMGetFirstUse(p); u; u = LLVMGetNextUse(u)) {
            LLVMValueRef I = LLVMGetUser(u);
            LLVMOpcode op = LLVMGetInstructionOpcode(I);
            if (op == LLVMExtractElement && LLVMIsAConstantInt(LLVMGetOperand(I, 1)))
                m |= 1u << LLVMConstIntGetZExtValue(LLVMGetOperand(I, 1));
            else if (op == LLVMShuffleVector) {
                unsigned n = vecN(LLVMTypeOf(p));
                for (unsigned k = 0; k < LLVMGetNumMaskElements(I); k++) {
                    int e = LLVMGetMaskValue(I, k);
                    if (e >= 0 && LLVMGetOperand(I, 0) == p && e < (int)n) m |= 1u << e;
                    if (e >= (int)n && LLVMGetOperand(I, 1) == p) m |= 1u << (e - n);
                }
            } else return ~0u;
        }
        return m;
    }
    LLVMValueRef findMD(const char *list, LLVMValueRef fn) {
        unsigned n = LLVMGetNamedMetadataNumOperands(mod, list);
        std::vector<LLVMValueRef> ks(n);
        if (n) LLVMGetNamedMetadataOperands(mod, list, ks.data());
        for (LLVMValueRef k : ks) {
            if (LLVMGetMDNodeNumOperands(k) < 3) continue;
            if (mdOps(k)[0] == fn) return k;
        }
        return nullptr;
    }
    Opnd sub(Opnd o, unsigned dw) { o.sub = (uint8_t)(o.sub + dw * 4); return o; }

    struct VsIn { LLVMValueRef p; unsigned loc; };
    struct FsIn { LLVMValueRef p; unsigned interp, comps; };
    std::vector<FsIn> fsIns;
    LLVMValueRef fsPos = nullptr, fsFront = nullptr, fsPointCoord = nullptr;
    unsigned fsSetup = 0, fsPersp = 0, fsLinear = 0, fsSrcDepth = 0, fsSrcW = 0;
    std::vector<std::pair<std::string, unsigned>> outFields;   // kind ("pos", "psize", "var:name", "rt:k"), components

    void bindRender(LLVMValueRef fn) {
        LLVMValueRef km = findMD(stage == XE_STAGE_VERTEX ? "air.vertex" : "air.fragment", fn);
        if (!km) fail("function is not listed as a %s function", stage == XE_STAGE_VERTEX ? "vertex" : "fragment");
        std::vector<LLVMValueRef> kops = mdOps(km);
        rinfo.version = XE_SHADER_INFO_VERSION;
        rinfo.stage = (uint32_t)stage;
        // outputs (return value fields)
        for (LLVMValueRef o : mdOps(kops[1])) {
            std::vector<LLVMValueRef> f = mdOps(o);
            std::string k = f.empty() ? "" : mdStr(f[0]);
            if (k == "air.position") outFields.push_back({"pos", 4});
            else if (k == "air.point_size") outFields.push_back({"psize", 1});
            else if (k == "air.vertex_output") outFields.push_back({"var:" + (f.size() > 1 ? mdStr(f[1]) : std::string("?")), 0});
            else if (k == "air.render_target") {
                unsigned idx = f.size() > 1 && LLVMIsAConstantInt(f[1]) ? (unsigned)LLVMConstIntGetZExtValue(f[1]) : 0;
                if (idx >= 8) fail("render target %u", idx);
                outFields.push_back({"rt:" + std::to_string(idx), 4});
            } else if (k == "air.depth" && stage == XE_STAGE_FRAGMENT) {
                outFields.push_back({"depth", 1});
                rinfo.flags |= XE_FS_DEPTH;
            } else fail("%s output %s is not supported yet", stage == XE_STAGE_VERTEX ? "vertex" : "fragment", k.c_str());
        }
        // field sizes from the return type
        LLVMTypeRef rt = LLVMGetReturnType(LLVMGlobalGetValueType(fn));
        bool isStruct = LLVMGetTypeKind(rt) == LLVMStructTypeKind;
        if (outFields.size() != (isStruct ? LLVMCountStructElementTypes(rt) : (LLVMGetTypeKind(rt) == LLVMVoidTypeKind ? 0u : 1u)))
            fail("output metadata does not match the return type");
        for (size_t i = 0; i < outFields.size(); i++) {
            LLVMTypeRef ft = isStruct ? LLVMStructGetTypeAtIndex(rt, (unsigned)i) : rt;
            outFields[i].second = compsOf(ft);
            if (outFields[i].first.rfind("rt:", 0) == 0) {
                unsigned idx = (unsigned)atoi(outFields[i].first.c_str() + 3);
                LLVMTypeRef e = isVec(ft) ? LLVMGetElementType(ft) : ft;
                rinfo.rtMask |= 1u << idx;
                rinfo.rtType[idx] = isHalf(e) ? XE_RT_HALF : LLVMGetTypeKind(e) == LLVMIntegerTypeKind ? XE_RT_UINT : XE_RT_FLOAT;
            }
        }
        // arguments
        std::vector<VsIn> vsIns;
        LLVMValueRef vid = nullptr, iid = nullptr;
        bool buffers = false;
        std::vector<std::vector<LLVMValueRef>> other;
        for (LLVMValueRef a : mdOps(kops[2])) {
            std::vector<LLVMValueRef> f = mdOps(a);
            if (f.size() < 2 || !LLVMIsAConstantInt(f[0])) fail("bad argument metadata");
            unsigned idx = (unsigned)LLVMConstIntGetZExtValue(f[0]);
            if (idx >= LLVMCountParams(fn)) fail("argument %u out of range", idx);
            LLVMValueRef p = LLVMGetParam(fn, idx);
            std::string k = mdStr(f[1]);
            if (k == "air.buffer") { buffers = true; other.push_back(f); continue; }
            if (k == "air.texture" || k == "air.sampler") { other.push_back(f); continue; }
            if (stage == XE_STAGE_VERTEX) {
                if (k == "air.vertex_input") {
                    unsigned loc = 0;
                    if (!mdInt(f, "air.location_index", loc)) fail("vertex input without a location");
                    if (hasUses(p)) vsIns.push_back({p, loc});
                } else if (k == "air.vertex_id") vid = p;
                else if (k == "air.instance_id") iid = p;
                else fail("vertex argument %s is not supported yet", k.c_str());
            } else {
                if (k == "air.position") fsPos = hasUses(p) ? p : nullptr;
                else if (k == "air.front_facing") fsFront = hasUses(p) ? p : nullptr;
                else if (k == "air.point_coord") fsPointCoord = hasUses(p) ? p : nullptr;   // v0.51.8: see the prologue
                else if (k == "air.fragment_input") {
                    if (!hasUses(p)) continue;
                    unsigned in = mdHas(f, "air.flat") ? XE_INTERP_FLAT : mdHas(f, "air.no_perspective") ? XE_INTERP_LINEAR : XE_INTERP_PERSPECTIVE;
                    if (fsIns.size() >= XE_MAX_VARYINGS) fail("more than %d fragment inputs", XE_MAX_VARYINGS);
                    snprintf(rinfo.varyingName[fsIns.size()], 64, "%s", f.size() > 2 ? mdStr(f[2]).c_str() : "?");
                    fsIns.push_back({p, in, compsOf(LLVMTypeOf(p))});
                    if (fsIns.back().comps > 4) fail("fragment input with more than 4 components");
                } else fail("fragment argument %s is not supported yet", k.c_str());
            }
        }
        // v0.68.26: used buffers 16..30 into free slots 0..15 (lowest first); bufSlot[loc] = the slot
        uint8_t bufSlot[31];
        for (unsigned i = 0; i < 31; i++) bufSlot[i] = (uint8_t)(i < 16 ? i : 0xFF);
        {
            bool taken[16] = {};
            for (auto &f : other) {
                unsigned loc = 0;
                if (mdStr(f[1]) == "air.buffer" && mdInt(f, "air.location_index", loc) && loc < 16) taken[loc] = true;
            }
            for (auto &f : other) {
                unsigned loc = 0;
                if (mdStr(f[1]) != "air.buffer" || !mdInt(f, "air.location_index", loc) || loc < 16 || loc > 30) continue;
                if (!hasUses(LLVMGetParam(fn, (unsigned)LLVMConstIntGetZExtValue(f[0])))) continue;
                for (unsigned sl = 0; sl < 16; sl++)
                    if (!taken[sl]) { taken[sl] = true; bufSlot[loc] = (uint8_t)sl; rinfo.bufSrc[sl] = (uint8_t)(loc + 1); break; }
            }
        }
        rinfo.pushRegs = buffers ? 1 : 0;
        for (auto &f : other) {                          // v0.51.8: buffers 8..15 (used) take a second push register
            unsigned loc = 0;
            if (mdStr(f[1]) == "air.buffer" && mdInt(f, "air.location_index", loc) && loc <= 30 && bufSlot[loc] != 0xFF &&
                bufSlot[loc] >= 8 && hasUses(LLVMGetParam(fn, (unsigned)LLVMConstIntGetZExtValue(f[0])))) rinfo.pushRegs = 2;
        }
        if (stage == XE_STAGE_VERTEX) {
            std::sort(vsIns.begin(), vsIns.end(), [](const VsIn &x, const VsIn &y) { return x.loc < y.loc; });
            unsigned nslots = (unsigned)vsIns.size() + ((vid || iid) ? 1 : 0);
            if (nslots > XE_MAX_VS_INPUTS) fail("more than %d vertex inputs", XE_MAX_VS_INPUTS);
            // 3DSTATE_VS Dispatch GRF Start = where the hardware puts the push constants,
            // with the vertex data right after them (v0.32.6: was 2 + push -> VS read
            // garbage buffer addresses, page fault, GPU hang)
            bufReg = 2;
            rinfo.grfStart = 2;
            const unsigned inBase = 2 + rinfo.pushRegs;
            for (unsigned sI = 0; sI < vsIns.size(); sI++) {
                unsigned base = inBase + 4 * sI;
                rinfo.inputLocation[sI] = (int32_t)vsIns[sI].loc;
                LLVMTypeRef t = LLVMTypeOf(vsIns[sI].p);
                unsigned n = vecN(t);
                if (n > 4) fail("vertex input with more than 4 components");
                rinfo.inputComps[sI] = n;
                std::vector<Val> c;
                for (unsigned k = 0; k < n; k++) c.push_back(regVal((uint8_t)(base + k), typeOf(t)));
                if (isVec(t)) vvals[vsIns[sI].p] = c; else vals[vsIns[sI].p] = c[0];
            }
            if (vid || iid) {
                unsigned sI = (unsigned)vsIns.size(), base = inBase + 4 * sI;
                rinfo.inputLocation[sI] = -1; rinfo.inputComps[sI] = 2;
                rinfo.sysSlot = sI;
                if (vid) { vals[vid] = regVal((uint8_t)base, UD); rinfo.sysValues |= XE_SYS_VERTEX_ID; }
                if (iid) { vals[iid] = regVal((uint8_t)(base + 1), UD); rinfo.sysValues |= XE_SYS_INSTANCE_ID; }
            }
            rinfo.numInputs = nslots;
            rinfo.urbReadLength = (nslots + 1) / 2;
            REG_FIRST = (int)(inBase + 4 * nslots);
        } else {
            unsigned r = 2;
            for (auto &x : fsIns) { if (x.interp == XE_INTERP_PERSPECTIVE) fsPersp = 1; if (x.interp == XE_INTERP_LINEAR) fsLinear = 1; }
            if (fsPersp) { fsPersp = r; r += 2; rinfo.flags |= XE_FS_PERSP; }
            if (fsLinear) { fsLinear = r; r += 2; rinfo.flags |= XE_FS_LINEAR; }
            unsigned posUse = fsPos ? compsUsed(fsPos) : 0;
            if (posUse == ~0u) posUse = 15;                    // used whole: every component
            if (fsPos && (posUse & 4)) {           // v0.35.0: [[position]].z = the source depth (after the barycentrics)
                fsSrcDepth = r; r += 1; rinfo.flags |= XE_FS_SRCDEPTH;
            }
            if (fsPos && (posUse & 8)) {           // v0.51.23: [[position]].w = 1 / source W (after the source depth)
                fsSrcW = r; r += 1; rinfo.flags |= XE_FS_SRCW;
            }
            rinfo.grfStart = r;
            bufReg = (uint8_t)r;
            fsSetup = r + rinfo.pushRegs;
            rinfo.numVaryings = (uint32_t)fsIns.size();
            for (size_t i = 0; i < fsIns.size(); i++) { rinfo.varyingInterp[i] = fsIns[i].interp; rinfo.varyingComps[i] = fsIns[i].comps; }
            if (fsPos) {
                if (posUse & ~15u) fail("[[position]] components 0x%x in a fragment function", posUse);
                rinfo.flags |= XE_FS_POSITION;
            }
            if (fsFront) rinfo.flags |= XE_FS_FRONTFACING;
            if (!rinfo.rtMask) { rinfo.rtMask = 1; rinfo.rtType[0] = XE_RT_FLOAT; rinfo.flags |= XE_FS_NOCOLOR; }   // v0.68.30
            REG_FIRST = (int)(fsSetup + 2 * fsIns.size());
        }
        if (REG_FIRST < 10) REG_FIRST = 10;
        if (REG_FIRST > 96) fail("too many shader inputs");
        REG_END = stage == XE_STAGE_VERTEX ? 112 : 120;                                   // g112..g127: end-of-thread payloads
        // buffers, textures, samplers: the compute rules (bufReg set above)
        for (auto &f : other) {
            unsigned idx = (unsigned)LLVMConstIntGetZExtValue(f[0]);
            LLVMValueRef p = LLVMGetParam(fn, idx);
            std::string k = mdStr(f[1]);
            unsigned loc = 0;
            if (!mdInt(f, "air.location_index", loc)) fail("%s argument %u without a location index", k.c_str(), idx);
            Val v;
            if (k == "air.buffer") {
                if (loc >= 8 && !hasUses(p)) continue;                  // v0.51.8: an unused buffer needs no address
                if (loc > 30 || bufSlot[loc] == 0xFF)
                    fail("buffer index %u: no free slot (16 buffers at most in %s functions)", loc, stage == XE_STAGE_VERTEX ? "vertex" : "fragment");
                unsigned sl = bufSlot[loc];                             // v0.68.26: 16..30 -> a free slot
                v.kind = Val::SCALAR; v.nr = (uint8_t)(bufReg + (sl >> 3)); v.dw = (uint8_t)(sl & 7); v.type = UD;
            } else {
                bool tex = k == "air.texture";
                unsigned lim = tex && stage == XE_STAGE_FRAGMENT ? 16 : 8;      // v0.51.8: 16 fragment textures (BT entries 8..23)
                if (loc >= lim && !hasUses(p)) continue;
                if (loc >= lim) fail("%s index %u: only 0..%u are supported yet", k.c_str() + 4, loc, lim - 1);
                // fragment: binding table 0..7 = render targets, texture k = entry 8 + k
                v.kind = Val::IMM; v.imm = loc + (k == "air.texture" && stage == XE_STAGE_FRAGMENT ? 8 : 0); v.type = UD;
                usesTextures = true; rinfo.usesTextures = 1;
            }
            vals[p] = v;
        }
    }

    // v0.48.0: private arrays in vertex / fragment functions, as in compute (v0.29.1): each lane gets
    // privBytes at scratch + (hardware thread id * 8 + lane) * privBytes. The scratch buffer's address is
    // XE_SCRATCH_MAGIC here; the driver puts the real address in when it has made the buffer.
    void privPrologueRender() {
        uint8_t t = newReg(), u = newReg(), l = newReg();
        privReg = newReg(); ref[privReg]++;                                     // lives for the whole shader
        Ctl s1; s1.weAll = true; s1.exec = 1; s1.swsb = swsbDist(1);
        Inst m; m.lo = 0x0a05022080000161ULL; m.hi = 0x7000ULL;              // mov(1) gN sr0.0 (brw_asm)
        setBits(m, 63, 56, t);
        emit(m);
        emit(alu2(OP_AND, s1, reg(t, UD), scal(t, 0, UD), imm(0xffffffffu, UD)));
        emit(alu2(OP_AND, s1, reg(u, UD), scal(t, 0, UD), imm(7, UD)));
        Opnd du = reg(u, UD); du.sub = 4;
        emit(alu2(OP_SHR, s1, du, scal(t, 0, UD), imm(1, UD)));
        emit(alu2(OP_AND, s1, du, scal(u, 1, UD), imm(0x18, UD)));
        emit(alu2(OP_OR,  s1, reg(u, UD), scal(u, 0, UD), scal(u, 1, UD)));
        emit(alu2(OP_SHR, s1, du, scal(t, 0, UD), imm(2, UD)));
        emit(alu2(OP_AND, s1, du, scal(u, 1, UD), imm(0xFE0, UD)));
        emit(alu2(OP_OR,  s1, reg(u, UD), scal(u, 0, UD), scal(u, 1, UD)));    // u.0 = id (< 4096)
        emit(alu2(OP_MUL, s1, reg(u, UD), scal(u, 0, UD), imm(privBytes * 8, UW)));
        emit(alu2(OP_ADD, s1, reg(u, UD), scal(u, 0, UD), imm(XE_SCRATCH_MAGIC, UD)));   // + the scratch buffer
        Ctl a; a.weAll = true; a.swsb = swsbDist(1);
        emit(alu1(OP_MOV, a, reg(l, UD), imm(0x76543210, V)));               // lane numbers
        emit(alu2(OP_MUL, a, reg(l, UD), reg(l, UD), imm(privBytes, UW)));
        emit(alu2(OP_ADD, a, reg(privReg, UD), reg(l, UD), scal(u, 0, UD)));   // this lane's area
        commit();
    }
    void prologueRender() {
        // f1.0 = the lanes that run (memory accesses are predicated on it; discard clears lanes)
        Ctl a = ctl(); a.weAll = true; a.cond = C_NZ; a.flag = 2;
        emit(alu2(OP_CMP, a, nullr(UD), reg(0, UD), reg(0, UD)));           // all 0
        Ctl b = ctl(); b.cond = C_Z; b.flag = 2;
        emit(alu2(OP_CMP, b, nullr(UD), reg(0, UD), reg(0, UD)));           // enabled lanes 1
        if (privBytes) privPrologueRender();
        if (spillMode) { spillAddr = newReg(); ref[spillAddr]++; commit(); }   // v0.51.9
        if (stage != XE_STAGE_FRAGMENT) return;
        if (fsPos) {
            // Mesa brw_compile_fs (Gfx12, SIMD8): subspan x/y in g1.2..g1.5 (words 4..7)
            uint8_t ux = newReg(), uy = newReg(), fx = newReg(), fy = newReg();
            Opnd sx = reg(1, UW); sx.sub = 8;  sx.vs = 2; sx.w = 4; sx.hs = 0;
            Opnd sy = reg(1, UW); sy.sub = 10; sy.vs = 2; sy.w = 4; sy.hs = 0;
            emit(alu2(OP_ADD, ctl(), reg(ux, UW), sx, imm(0x10101010, V)));
            emit(alu2(OP_ADD, ctl(), reg(uy, UW), sy, imm(0x11001100, V)));
            emit(alu1(OP_MOV, ctl(), reg(fx, F), reg(ux, UW)));
            emit(alu1(OP_MOV, ctl(), reg(fy, F), reg(uy, UW)));
            emit(alu2(OP_ADD, ctl(), reg(fx, F), reg(fx, F), imm(0x3f000000, F)));   // pixel centre
            emit(alu2(OP_ADD, ctl(), reg(fy, F), reg(fy, F), imm(0x3f000000, F)));
            // .w: Metal gives 1/w; the payload's source W is the (perspective-interpolated) clip w itself (Mesa: wpos_w = RCP(pixel_w))
            Val pw = fsSrcW ? math(M_INV, F, regVal((uint8_t)fsSrcW, F), nullptr) : immF(1.0f);
            std::vector<Val> c = { regVal(fx, F), regVal(fy, F), fsSrcDepth ? regVal((uint8_t)fsSrcDepth, F) : immF(0.0f), pw };
            c.resize(vecN(LLVMTypeOf(fsPos)));
            vvals[fsPos] = c;
            own(fsPos);
            commit();
        }
        if (fsPointCoord) {
            // v0.51.8: [[point_coord]] only has a value when points are drawn (point sprites); triangles never use it
            // (Core Animation's UberShader lists it for every variant): (0.5, 0.5), the middle of a sprite
            std::vector<Val> c = { immF(0.5f), immF(0.5f) };
            c.resize(vecN(LLVMTypeOf(fsPointCoord)));
            vvals[fsPointCoord] = c;
            own(fsPointCoord);
            commit();
        }
        if (fsFront) {
            // Mesa emit_frontfacing_interpolation (Gfx12): bit 15 of g1.1 (word 2) = back facing
            uint8_t t = newReg(), f = newReg();
            Opnd g = scal(1, 0, W); g.sub = 4;
            emit(alu2(OP_ASR, ctl(), reg(t, W), g, imm(15, D)));
            emit(alu1(OP_NOT, ctl(), reg(f, D), reg(t, W)));
            vals[fsFront] = regVal(f, UD);
            own(fsFront);
            commit();
        }
        for (size_t i = 0; i < fsIns.size(); i++) {
            const FsIn &in = fsIns[i];
            LLVMTypeRef t = LLVMTypeOf(in.p);
            Type ty = typeOf(t);
            std::vector<Val> c;
            uint8_t b0 = (uint8_t)(in.interp == XE_INTERP_LINEAR ? fsLinear : fsPersp);
            for (unsigned k = 0; k < in.comps; k++) {
                // plane parameters, 16 bytes per component: (a1 - a0, a2 - a0, -, a0) (Mesa remap_attr_reg)
                unsigned q = (unsigned)i * 4 + k;
                uint8_t pr = (uint8_t)(fsSetup + q / 2);
                Opnd P = scal(pr, (q % 2) * 4, F);
                uint8_t d = newReg();
                if (in.interp == XE_INTERP_FLAT) {
                    emit(alu1(OP_MOV, ctl(), reg(d, UD), sub(scal(pr, (q % 2) * 4, UD), 3)));
                    c.push_back(regVal(d, ty == F ? F : UD));
                    continue;
                }
                // nir_lower_interpolation: a0 + b0 (a1 - a0) + b1 (a2 - a0)
                uint8_t e = newReg();
                emit(alu2(OP_MUL, ctl(), reg(d, F), reg(b0, F), P));
                emit(alu2(OP_MUL, ctl(), reg(e, F), reg((uint8_t)(b0 + 1), F), sub(P, 1)));
                emit(alu2(OP_ADD, ctl(), reg(d, F), reg(d, F), reg(e, F)));
                emit(alu2(OP_ADD, ctl(), reg(d, F), reg(d, F), sub(P, 3)));
                c.push_back(regVal(d, F));
            }
            if (isVec(t)) vvals[in.p] = c; else vals[in.p] = c[0];
            own(in.p);
            commit();
        }
    }

    // copy n values into n consecutive registers starting at d
    void packRegs(uint8_t d, const std::vector<Val> &v, Type t) {
        for (size_t k = 0; k < v.size(); k++) emit(alu1(OP_MOV, ctl(), reg((uint8_t)(d + k), t), opnd(v[k], t)));
    }
    void epilogueRender(LLVMValueRef ret) {
        std::vector<Val> outs;
        if (ret && LLVMGetNumOperands(ret) > 0) outs = structVals(LLVMGetOperand(ret, 0));
        std::vector<std::vector<Val>> field;
        size_t at = 0;
        for (auto &f : outFields) {
            if (at + f.second > outs.size()) fail("internal: output components");
            field.push_back(std::vector<Val>(outs.begin() + at, outs.begin() + at + f.second));
            at += f.second;
        }
        if (stage == XE_STAGE_VERTEX) {
            // VUE: slot 0 header (dw3 point size), slot 1 position, then the varyings
            std::vector<std::vector<Val>> slots(2, std::vector<Val>(4, immF(0.0f)));
            bool havePos = false;
            for (size_t i = 0; i < outFields.size(); i++) {
                const std::string &k = outFields[i].first;
                if (k == "pos") { slots[1] = field[i]; havePos = true; }
                else if (k == "psize") { slots[0][3] = field[i][0]; rinfo.flags |= XE_VS_POINT_SIZE; }
                else {
                    if (rinfo.numVaryings >= XE_MAX_VARYINGS) fail("more than %d vertex outputs", XE_MAX_VARYINGS);
                    if (field[i].size() > 4) fail("vertex output with more than 4 components");
                    snprintf(rinfo.varyingName[rinfo.numVaryings], 64, "%s", k.c_str() + 4);
                    rinfo.varyingComps[rinfo.numVaryings] = (uint32_t)field[i].size();
                    rinfo.numVaryings++;
                    std::vector<Val> s4 = field[i];
                    while (s4.size() < 4) s4.push_back(immF(0.0f));
                    slots.push_back(s4);
                }
            }
            if (!havePos) fail("vertex function without a [[position]] output");
            rinfo.vueSlots = (uint32_t)slots.size();
            for (size_t sI = 0; sI < slots.size(); sI += 2) {
                bool last = sI + 2 >= slots.size();
                unsigned n = sI + 1 < slots.size() ? 2 : 1;
                std::vector<Val> data;
                for (unsigned k = 0; k < n; k++) data.insert(data.end(), slots[sI + k].begin(), slots[sI + k].end());
                if (!last) {
                    uint8_t d = newRegs(4 * n);
                    packRegs(d, data, UD);
                    emit(sendMsg(SFID_URB, descUrbWrite((unsigned)sI), (4 * n) << 6, -1, 1, d, swsbDistSet(1, 0), false, 0));
                    emit(syncNop(swsbDst(0)));
                    commit();
                } else {
                    packRegs(112, data, UD);
                    Ctl we; we.weAll = true; we.swsb = swsbDist(1);
                    emit(alu1(OP_MOV, we, reg(126, UD), reg(1, UD)));
                    emit(withEOT(sendMsg(SFID_URB, descUrbWrite((unsigned)sI), (4 * n) << 6, -1, 126, 112, swsbDist(1), false, 0)));
                }
            }
            return;
        }
        // fragment: one render target write per color output, the last one ends the thread
        std::vector<std::pair<unsigned, size_t>> rts;
        for (size_t i = 0; i < outFields.size(); i++)
            if (outFields[i].first.rfind("rt:", 0) == 0) rts.push_back({(unsigned)atoi(outFields[i].first.c_str() + 3), i});
        std::sort(rts.begin(), rts.end());
        if (rts.empty()) rts.push_back({0u, (size_t)-1});           // v0.68.30: no color output: a zero write (masked off)
        int depthField = -1;
        for (size_t i = 0; i < outFields.size(); i++) if (outFields[i].first == "depth") depthField = (int)i;
        if (depthField >= 0 && field[depthField].size() != 1) fail("[[depth]] output must be one float");
        for (size_t j = 0; j < rts.size(); j++) {
            bool last = j + 1 == rts.size();
            unsigned idx = rts[j].first;
            std::vector<Val> v = rts[j].second == (size_t)-1 ? std::vector<Val>() : field[rts[j].second];
            Type t = rinfo.rtType[idx] == XE_RT_UINT || rinfo.rtType[idx] == XE_RT_SINT ? UD : F;
            while (v.size() < 4) v.push_back(t == F ? immF(v.size() == 3 ? 1.0f : 0.0f) : immU(v.size() == 3 ? 1 : 0));
            unsigned mlen = depthField >= 0 ? 5 : 4;
            uint8_t d = last ? 120 : newRegs(mlen);
            packRegs(d, v, t);
            if (depthField >= 0) packRegs((uint8_t)(d + 4), field[depthField], F);
            Inst w = asSendc(sendMsg(SFID_RENDER, descRTWrite(idx, last, mlen), idx << 12, -1, d, -1,
                                     last ? swsbDist(1) : swsbDistSet(1, 0), usesDiscard, 2));
            if (last) emit(withEOT(w));
            else { emit(w); emit(syncNop(swsbDst(0))); commit(); }
        }
        if (usesDiscard) rinfo.flags |= XE_FS_DISCARD;
        // v0.62.1: helper pixels (the GPU's Vector Mask, so texture LOD at triangle edges comes from real
        // neighbours) run the whole shader; one that writes memory would write from them too: tell the driver
        if (curFn) {
            bool fx = false;
            for (LLVMBasicBlockRef b = LLVMGetFirstBasicBlock(curFn); b && !fx; b = LLVMGetNextBasicBlock(b))
                for (LLVMValueRef I = LLVMGetFirstInstruction(b); I && !fx; I = LLVMGetNextInstruction(I)) {
                    LLVMOpcode op = LLVMGetInstructionOpcode(I);
                    if (op == LLVMStore && LLVMGetPointerAddressSpace(LLVMTypeOf(LLVMGetOperand(I, 1))) != 0) fx = true;
                    else if (op == LLVMAtomicRMW || op == LLVMAtomicCmpXchg) fx = true;
                    else if (op == LLVMCall) {
                        std::string n = calledName(I);
                        if (n.find("atomic") != std::string::npos || n.find("write") != std::string::npos ||
                            n.find("fence") != std::string::npos) fx = true;
                    }
                }
            if (fx) rinfo.flags |= XE_FS_SIDEFX;
        } else rinfo.flags |= XE_FS_SIDEFX;
    }

    void runRender(LLVMValueRef fn) {
        lowerSwitches(fn);
        allocPrivate(fn);
        if (spillMode) {                               // v0.51.9: the spill area after the private arrays
            spillBase = privBytes;
            privBytes = (privBytes + spillReserve + 15) & ~15u;
            if (privBytes * 8 > 65535) fail("register spilling: more than 8 KB of scratch per lane");
        }
        rinfo.privBytes = privBytes;                   // v0.48.0 (was: not supported)
        { int n = 0; for (LLVMBasicBlockRef b = LLVMGetFirstBasicBlock(fn); b; b = LLVMGetNextBasicBlock(b))
            for (LLVMValueRef I = LLVMGetFirstInstruction(b); I; I = LLVMGetNextInstruction(I)) layoutPos[I] = n++; }
        analyze(fn);
        countUses(fn);
        findLazy(fn);
        allocSLM(fn);
        if (slmBytes) fail("threadgroup memory in a %s function", stage == XE_STAGE_VERTEX ? "vertex" : "fragment");
        bindRender(fn);
        prologueRender();
        LLVMValueRef ret = nullptr;
        for (LLVMBasicBlockRef b = LLVMGetFirstBasicBlock(fn); b; b = LLVMGetNextBasicBlock(b)) {
            LLVMValueRef t = LLVMGetBasicBlockTerminator(b);
            if (t && LLVMGetInstructionOpcode(t) == LLVMRet) { if (ret) fail("more than one return"); ret = t; }
        }
        region(LLVMGetEntryBasicBlock(fn), nullptr, nullptr);
        fixJumps();
        epilogueRender(ret);
        overlapSends();                                // v0.61.0
        if (getenv("XEC_STATS")) {
            fprintf(stderr, "stats: overlapped reads %zu, waits placed %zu\n", overlapped, overlapWaits); fprintf(stderr, "stats: edge moves %ld of %zu instructions, %ld values computed into their phi\n", edgeMoves, code.size(), redirected);
            for (auto &w : redirWhy) fprintf(stderr, "  redirect refused at return #%d: %ld\n", w.first, w.second);
            for (auto &w : edgeWhy) fprintf(stderr, "  edge source %-14s %ld\n", w.first.c_str(), w.second); }
    }

    void run(LLVMValueRef fn) {
        REG_FIRST = 15;                                  // v0.51.17: g10..g14 = thread position (prologue)
        lowerSwitches(fn);
        allocPrivate(fn);
        // test only (XEC_SPILLTEST=n): compute kernels with n registers, spilling as render functions do,
        // so the simulator can check the spill code
        int spillTest = getenv("XEC_SPILLTEST") ? atoi(getenv("XEC_SPILLTEST")) : 0;
        if (spillTest) {
            spillMode = true; spillReserve = 2048; spillBase = privBytes;
            privBytes = (privBytes + spillReserve + 15) & ~15u;
            int n = 0; for (LLVMBasicBlockRef b = LLVMGetFirstBasicBlock(fn); b; b = LLVMGetNextBasicBlock(b))
                for (LLVMValueRef I = LLVMGetFirstInstruction(b); I; I = LLVMGetNextInstruction(I)) layoutPos[I] = n++;
        }
        analyze(fn);
        countUses(fn);
        if (spillTest) findLazy(fn);
        allocSLM(fn);
        bindArguments(fn);
        prologue();
        if (spillTest) { REG_END = REG_FIRST + spillTest; spillAddr = newReg(); ref[spillAddr]++; commit(); }
        // lanes outside the grid sit out the whole kernel (so data-dependent
        // loops can't spin on garbage in those lanes)
        cf(CF_IF, true, false, 2);
        size_t gridIf = code.size() - 1;
        region(LLVMGetEntryBasicBlock(fn), nullptr, nullptr);
        closeIf(gridIf, (size_t)-1);
        fixJumps();
        epilogue();
        if (spillTest) fprintf(stderr, "spilltest: %u values spilled while generating code, %u bytes\n", spillCount, spillMax);
    }
};

} // namespace

// v0.51.2: calls to functions of the same module (Core Animation's shaders have helpers such as outline_alpha
// or path_cubic_vert_<n> that Apple's compiler did not inline): inline them first, with LLVM's own
// InlineFunction (TransformUtils), until none are left (8 rounds for helpers that call helpers).
static void inlineHelpers(LLVMValueRef entry) {
    llvm::Function *F = llvm::unwrap<llvm::Function>(entry);
    for (int round = 0; round < 8; round++) {
        std::vector<llvm::CallBase *> calls;
        for (auto &BB : *F)
            for (auto &I : BB)
                if (auto *cb = llvm::dyn_cast<llvm::CallBase>(&I))
                    if (llvm::Function *c = cb->getCalledFunction())
                        if (!c->isDeclaration() && c != F) calls.push_back(cb);
        if (calls.empty()) return;
        bool any = false;
        for (auto *cb : calls) {
            llvm::InlineFunctionInfo ifi;
            if (llvm::InlineFunction(*cb, ifi).isSuccess()) any = true;
        }
        if (!any) return;
    }
}

// v0.51.5: Metal fills in function constants before we get the AIR, which leaves branches on constants
// (any(false), x == 3 with x now 3): fold those and drop the code that can't run, so the structurizer
// doesn't meet shapes such as "if (false) X else if (false) X"
// v0.51.8: sink texture samples (and what only feeds them / reads them) down to just before their first use, inside
// one block, so a shader that samples N taps first and sums afterwards does not hold N results at once.

// v0.51.9: measurement: the most registers (scalar 32-bit components) live at once, from SSA liveness
static unsigned regsOfType(llvm::Type *t) {
    if (t->isVoidTy() || t->isLabelTy() || t->isMetadataTy()) return 0;
    if (auto *v = llvm::dyn_cast<llvm::FixedVectorType>(t)) return v->getNumElements();
    if (auto *st = llvm::dyn_cast<llvm::StructType>(t)) { unsigned n = 0; for (auto *e : st->elements()) n += regsOfType(e); return n; }
    if (t->isPointerTy()) return 1;
    return 1;
}
static void maxLiveStats(LLVMValueRef entry) {
    llvm::Function *F = llvm::unwrap<llvm::Function>(entry);
    std::map<llvm::BasicBlock *, std::set<llvm::Value *>> liveIn, liveOut;
    auto isVal = [](llvm::Value *v) { return llvm::isa<llvm::Instruction>(v) || llvm::isa<llvm::Argument>(v); };
    bool changed = true;
    std::vector<llvm::BasicBlock *> order;
    for (auto &BB : *F) order.push_back(&BB);
    while (changed) {
        changed = false;
        for (auto it = order.rbegin(); it != order.rend(); ++it) {
            llvm::BasicBlock *B = *it;
            std::set<llvm::Value *> out;
            for (llvm::BasicBlock *S : llvm::successors(B)) {
                for (llvm::Value *v : liveIn[S]) out.insert(v);
                for (auto &I : *S) { auto *P = llvm::dyn_cast<llvm::PHINode>(&I); if (!P) break;
                    llvm::Value *iv = P->getIncomingValueForBlock(B); if (isVal(iv)) out.insert(iv); }
            }
            std::set<llvm::Value *> in = out;
            for (auto ii = B->rbegin(); ii != B->rend(); ++ii) {
                llvm::Instruction &I = *ii;
                in.erase(&I);
                if (llvm::isa<llvm::PHINode>(I)) continue;
                for (llvm::Value *o : I.operands()) if (isVal(o) && !llvm::isa<llvm::BasicBlock>(o)) in.insert(o);
            }
            if (in != liveIn[B]) { liveIn[B] = in; changed = true; }
            liveOut[B] = out;
        }
    }
    unsigned best = 0; llvm::Instruction *at = nullptr; std::set<llvm::Value *> bestSet;
    for (auto &BB : *F) {
        std::set<llvm::Value *> live = liveOut[&BB];
        for (auto ii = BB.rbegin(); ii != BB.rend(); ++ii) {
            unsigned n = 0; for (llvm::Value *v : live) n += regsOfType(v->getType());
            if (n > best) { best = n; at = &*ii; bestSet = live; }
            live.erase(&*ii);
            if (!llvm::isa<llvm::PHINode>(*ii)) for (llvm::Value *o : ii->operands()) if (isVal(o)) live.insert(o);
        }
    }
    fprintf(stderr, "maxlive: %u scalar registers", best);
    if (at) { char *pv = LLVMPrintValueToString(llvm::wrap(at)); fprintf(stderr, " at%.100s", pv); LLVMDisposeMessage(pv); }
    fprintf(stderr, "\n");
    std::map<std::string, std::pair<int, int>> h;
    for (llvm::Value *v : bestSet) {
        std::string k;
        if (auto *I = llvm::dyn_cast<llvm::Instruction>(v)) {
            k = I->getOpcodeName();
            if (auto *C = llvm::dyn_cast<llvm::CallInst>(I)) if (C->getCalledFunction()) k += ":" + C->getCalledFunction()->getName().str();
            if (auto *L = llvm::dyn_cast<llvm::LoadInst>(I)) k += ":as" + std::to_string(L->getPointerAddressSpace());
        } else k = "arg";
        h[k].first++; h[k].second += regsOfType(v->getType());
    }
    std::vector<std::pair<int, std::string>> o;
    for (auto &x : h) o.push_back({x.second.second, x.first + " x" + std::to_string(x.second.first)});
    std::sort(o.rbegin(), o.rend());
    for (size_t i = 0; i < o.size() && i < 15; i++) fprintf(stderr, "  %5d %s\n", o[i].first, o[i].second.c_str());
}

// v0.51.9: spill to private memory in the IR, chosen from register pressure: wherever more than `target`
// scalar registers would be live, values live there (and not used there) are candidates; the ones that
// cross the most such points per use are moved (store after the definition, a load before each use).
static bool rematerializable(llvm::Value *v, int d = 0) {
    if (d > 6) return false;
    if (llvm::isa<llvm::Argument>(v)) return true;
    if (llvm::isa<llvm::Constant>(v)) return true;
    if (llvm::isa<llvm::AllocaInst>(v)) return true;
    if (auto *L = llvm::dyn_cast<llvm::LoadInst>(v)) return L->getPointerAddressSpace() == 2 && rematerializable(L->getPointerOperand(), d + 1);
    if (auto *G = llvm::dyn_cast<llvm::GetElementPtrInst>(v)) { if (!G->hasAllConstantIndices()) return false; return rematerializable(G->getPointerOperand(), d + 1); }
    if (auto *C = llvm::dyn_cast<llvm::CastInst>(v)) { if (!C->getType()->isPointerTy()) return false; return rematerializable(C->getOperand(0), d + 1); }
    return false;
}
static unsigned spillWeight(llvm::Value *v) {
    if (llvm::isa<llvm::Argument>(v)) return 0;
    if (rematerializable(v)) return 0;
    return regsOfType(v->getType());
}
static bool spillableType(llvm::Type *t) {
    if (auto *vt = llvm::dyn_cast<llvm::FixedVectorType>(t)) { if (vt->getNumElements() > 4) return false; t = vt->getElementType(); }
    if (t->isFloatTy() || t->isHalfTy()) return true;
    if (auto *it = llvm::dyn_cast<llvm::IntegerType>(t)) return it->getBitWidth() > 1 && it->getBitWidth() <= 32;
    return false;
}
static int pressureSpill(LLVMValueRef entry, unsigned target, int maxRounds) {
    llvm::Function *F = llvm::unwrap<llvm::Function>(entry);
    std::set<llvm::Instruction *> done;
    int total = 0;
    for (int round = 0; round < maxRounds; round++) {
        // liveness (instructions only; arguments and rematerializable values cost nothing)
        std::map<llvm::BasicBlock *, std::set<llvm::Instruction *>> liveIn;
        std::vector<llvm::BasicBlock *> order;
        for (auto &BB : *F) order.push_back(&BB);
        auto liveOutOf = [&](llvm::BasicBlock *B) {
            std::set<llvm::Instruction *> out;
            for (llvm::BasicBlock *S : llvm::successors(B)) {
                for (llvm::Instruction *v : liveIn[S]) out.insert(v);
                for (auto &I : *S) { auto *P = llvm::dyn_cast<llvm::PHINode>(&I); if (!P) break;
                    if (auto *iv = llvm::dyn_cast<llvm::Instruction>(P->getIncomingValueForBlock(B))) out.insert(iv); }
            }
            return out;
        };
        for (bool changed = true; changed;) {
            changed = false;
            for (auto it = order.rbegin(); it != order.rend(); ++it) {
                llvm::BasicBlock *B = *it;
                std::set<llvm::Instruction *> in = liveOutOf(B);
                for (auto ii = B->rbegin(); ii != B->rend(); ++ii) {
                    in.erase(&*ii);
                    if (llvm::isa<llvm::PHINode>(*ii)) continue;
                    for (llvm::Value *o : ii->operands()) if (auto *oi = llvm::dyn_cast<llvm::Instruction>(o)) in.insert(oi);
                }
                if (in != liveIn[B]) { liveIn[B] = in; changed = true; }
            }
        }
        // over-budget points: who is live across them
        std::map<llvm::Instruction *, double> benefit;
        unsigned worst = 0; int over = 0;
        for (auto &BB : *F) {
            std::set<llvm::Instruction *> live = liveOutOf(&BB);
            unsigned w = 0; for (auto *v : live) w += spillWeight(v);
            for (auto ii = BB.rbegin(); ii != BB.rend(); ++ii) {
                llvm::Instruction *I = &*ii;
                if (w > worst) worst = w;
                if (w > target) {
                    over++;
                    std::set<llvm::Value *> usedHere(I->op_begin(), I->op_end());
                    for (auto *v : live) if (!usedHere.count(v) && spillWeight(v)) benefit[v] += (double)(w - target) * spillWeight(v);
                }
                if (live.erase(I)) w -= spillWeight(I);
                if (!llvm::isa<llvm::PHINode>(I))
                    for (llvm::Value *o : I->operands()) if (auto *oi = llvm::dyn_cast<llvm::Instruction>(o)) if (live.insert(oi).second) w += spillWeight(oi);
            }
        }
        if (getenv("XEC_SPILLLOG")) fprintf(stderr, "  pressure round %d: worst %u, %d points over %u\n", round, worst, over, target);
        if (!over) break;
        std::vector<std::pair<double, llvm::Instruction *>> cand;
        for (auto &b : benefit) {
            llvm::Instruction *I = b.first;
            if (done.count(I) || !spillableType(I->getType()) || I->getType()->isVoidTy()) continue;
            if (llvm::isa<llvm::LandingPadInst>(I)) continue;
            double uses = 1 + (double)I->getNumUses();
            cand.push_back({b.second / uses, I});
        }
        if (cand.empty()) break;
        std::sort(cand.rbegin(), cand.rend());
        size_t take = std::max<size_t>(1, std::min<size_t>(cand.size(), 6));
        for (size_t k = 0; k < take; k++) {
            llvm::Instruction *I = cand[k].second;
            done.insert(I);
            if (auto *P = llvm::dyn_cast<llvm::PHINode>(I)) llvm::DemotePHIToStack(P);
            else llvm::DemoteRegToStack(*I);
            total++;
        }
    }
    return total;
}
static std::map<LLVMValueRef, int> numberInstructions(LLVMValueRef entry) {
    std::map<LLVMValueRef, int> m;
    int n = 0;
    for (LLVMBasicBlockRef b = LLVMGetFirstBasicBlock(entry); b; b = LLVMGetNextBasicBlock(b))
        for (LLVMValueRef I = LLVMGetFirstInstruction(b); I; I = LLVMGetNextInstruction(I)) m[I] = n++;
    return m;
}
// v0.51.8: move the chosen values to private (scratch) memory: a store after the definition, a load before each use
static void applySpills(LLVMValueRef entry, const std::map<LLVMValueRef, int> &num, const std::set<int> &spills) {
    std::vector<llvm::Instruction *> todo;
    for (auto &kv : num)
        if (spills.count(kv.second)) todo.push_back(llvm::unwrap<llvm::Instruction>(kv.first));
    (void)entry;
    for (llvm::Instruction *I : todo) {
        if (llvm::isa<llvm::PHINode>(I) || llvm::isa<llvm::AllocaInst>(I) || I->getType()->isVoidTy()) continue;
        llvm::DemoteRegToStack(*I);
    }
    for (llvm::Instruction *I : todo)                          // phis last: their stores go to the predecessors
        if (auto *P = llvm::dyn_cast<llvm::PHINode>(I)) llvm::DemotePHIToStack(P);
}
static bool sinkBarrier(llvm::Instruction &I) {
    if (llvm::isa<llvm::StoreInst>(I) || llvm::isa<llvm::AtomicRMWInst>(I) || llvm::isa<llvm::AtomicCmpXchgInst>(I) || llvm::isa<llvm::FenceInst>(I)) return true;
    if (auto *C = llvm::dyn_cast<llvm::CallInst>(&I)) {
        llvm::Function *cf = C->getCalledFunction();
        if (!cf) return true;
        llvm::StringRef n = cf->getName();
        if (n.starts_with("air.write") || n.starts_with("air.atomic") || n.starts_with("air.fence") || n.starts_with("air.wg") ||
            n.starts_with("air.discard") || n.starts_with("air.simdgroup") || n.starts_with("air.quad")) return true;
    }
    return false;
}
static void sinkSamples(LLVMValueRef entry) {
    llvm::Function *F = llvm::unwrap<llvm::Function>(entry);
    for (auto &BB : *F) {
        std::vector<llvm::Instruction *> order;
        for (auto &I : BB) order.push_back(&I);
        for (size_t k = order.size(); k-- > 0;) {
            llvm::Instruction *I = order[k];
            if (I->isTerminator() || llvm::isa<llvm::PHINode>(I) || I->use_empty()) continue;
            bool isSample = false;
            if (auto *C = llvm::dyn_cast<llvm::CallInst>(I)) {
                llvm::Function *cf = C->getCalledFunction();
                isSample = cf && (cf->getName().starts_with("air.sample_texture") || cf->getName().starts_with("air.read_texture"));
            }
            bool pure = llvm::isa<llvm::ExtractValueInst>(I) || llvm::isa<llvm::BinaryOperator>(I) || llvm::isa<llvm::CastInst>(I);
            if (!isSample && !pure) continue;
            if (pure && !llvm::isa<llvm::ExtractValueInst>(I) && !I->hasOneUse()) continue;
            llvm::Instruction *first = nullptr;
            bool ok = true;
            for (llvm::User *u : I->users()) {
                auto *ui = llvm::dyn_cast<llvm::Instruction>(u);
                if (!ui || ui->getParent() != &BB || llvm::isa<llvm::PHINode>(ui)) { ok = false; break; }
                if (!first || ui->comesBefore(first)) first = ui;
            }
            if (!ok || !first || first == I->getNextNode()) continue;
            if (isSample)                                           // memory reads may not cross writes
                for (llvm::Instruction *q = I->getNextNode(); q && q != first; q = q->getNextNode())
                    if (sinkBarrier(*q)) { ok = false; break; }
            if (!ok) continue;
            I->moveBefore(*first->getParent(), first->getIterator());
        }
    }
}
// v0.61.0: reads in groups. After sinkSamples every texture sample / memory read sits right before its first use,
// so the GPU waited for each one in turn. Here, inside a block, each read (with the pure one-use values that compute
// its coordinates) moves up to just after the previous read, up to XEC_GROUP (default 8) reads per group: the reads
// of a group are sent together and their waits (overlapSends) come later. Reads never cross a write or a barrier.
static bool isReadInst(llvm::Instruction *I) {
    if (auto *C = llvm::dyn_cast<llvm::CallInst>(I)) {
        llvm::Function *cf = C->getCalledFunction();
        return cf && (cf->getName().starts_with("air.sample_texture") || cf->getName().starts_with("air.read_texture"));
    }
    if (auto *L = llvm::dyn_cast<llvm::LoadInst>(I)) {
        unsigned as = L->getPointerAddressSpace();
        return !L->isVolatile() && !L->isAtomic() && (as == 1 || as == 2);
    }
    return false;
}
static bool isPureForGroup(llvm::Instruction *I) {
    return llvm::isa<llvm::BinaryOperator>(I) || llvm::isa<llvm::CastInst>(I) || llvm::isa<llvm::ExtractElementInst>(I) ||
           llvm::isa<llvm::InsertElementInst>(I) || llvm::isa<llvm::ShuffleVectorInst>(I) || llvm::isa<llvm::ExtractValueInst>(I) ||
           llvm::isa<llvm::GetElementPtrInst>(I) || llvm::isa<llvm::FCmpInst>(I) || llvm::isa<llvm::ICmpInst>(I) ||
           llvm::isa<llvm::SelectInst>(I);
}
// v0.61.0: constant-buffer reads (address space 2, a fixed offset from a buffer argument) move to the start of the
// function, one per address: WindowServer's colour-matching shader read the same 16 settings again in every loop
// pass and branch (66 reads per pixel). Constant buffers are read-only, and the offsets stay inside 4 KB of the
// argument (the struct the CPU side bound). Few enough of them (<= 32) are then kept in registers (findLazy).
static size_t hoistedLoads;
static void hoistConstLoads(LLVMValueRef entry) {
    llvm::Function *F = llvm::unwrap<llvm::Function>(entry);
    if (F->empty()) return;
    const llvm::DataLayout &DL = F->getParent()->getDataLayout();
    llvm::BasicBlock &E = F->getEntryBlock();
    std::vector<llvm::LoadInst *> loads;
    for (auto &BB : *F) for (auto &I : BB)
        if (auto *L = llvm::dyn_cast<llvm::LoadInst>(&I))
            if (L->isSimple() && L->getPointerAddressSpace() == 2) loads.push_back(L);
    std::map<std::tuple<llvm::Value *, int64_t, llvm::Type *>, llvm::LoadInst *> got;
    std::set<llvm::Instruction *> moving(loads.begin(), loads.end());
    llvm::Instruction *at = &*E.getFirstInsertionPt();       // the new reads go before this (not one being replaced)
    while (at && (llvm::isa<llvm::AllocaInst>(at) || moving.count(at))) at = at->getNextNode();
    if (!at) return;
    for (llvm::LoadInst *L : loads) {
        llvm::APInt off(64, 0);
        llvm::Value *base = L->getPointerOperand()->stripAndAccumulateConstantOffsets(DL, off, true);
        if (!llvm::isa<llvm::Argument>(base)) continue;
        int64_t o = off.getSExtValue();
        if (o < 0 || o + (int64_t)DL.getTypeStoreSize(L->getType()) > 4096) continue;
        auto key = std::make_tuple(base, o, L->getType());
        auto it = got.find(key);
        llvm::LoadInst *N;
        if (it != got.end()) N = it->second;
        else {
            llvm::IRBuilder<> B(at);
            llvm::Value *ptr = o ? B.CreateConstInBoundsGEP1_64(B.getInt8Ty(), base, (uint64_t)o) : base;
            N = B.CreateAlignedLoad(L->getType(), ptr, L->getAlign());
            got[key] = N;
        }
        if (N == L) continue;
        L->replaceAllUsesWith(N);
        L->eraseFromParent();
        hoistedLoads++;
    }
}
static size_t groupedReads;
static void groupReads(LLVMValueRef entry) {
    const char *g = getenv("XEC_GROUP");
    const int G = g ? atoi(g) : 8;
    if (G <= 1) return;
    llvm::Function *F = llvm::unwrap<llvm::Function>(entry);
    long moves = 0;
    for (auto &BB : *F) {
        llvm::Instruction *prev = nullptr;          // the last read of the current group
        int inGroup = 0;
        std::vector<llvm::Instruction *> order;
        for (auto &I : BB) order.push_back(&I);
        for (llvm::Instruction *I : order) {
            if (sinkBarrier(*I)) { prev = nullptr; inGroup = 0; continue; }
            if (!isReadInst(I)) continue;
            if (!prev || inGroup >= G) { prev = I; inGroup = 1; continue; }
            // what must move with it: pure values between prev and I that it (transitively) needs
            std::set<llvm::Instruction *> need;
            std::vector<llvm::Instruction *> st{I};
            bool ok = true;
            while (!st.empty() && ok) {
                llvm::Instruction *x = st.back(); st.pop_back();
                for (llvm::Value *op : x->operands()) {
                    auto *oi = llvm::dyn_cast<llvm::Instruction>(op);
                    if (!oi || oi->getParent() != &BB || !prev->comesBefore(oi) || need.count(oi)) continue;
                    if (!isPureForGroup(oi) || llvm::isa<llvm::PHINode>(oi)) { ok = false; break; }
                    need.insert(oi); st.push_back(oi);
                    if (need.size() > 24) { ok = false; break; }
                }
            }
            for (llvm::Instruction *q = prev->getNextNode(); ok && q && q != I; q = q->getNextNode())
                if (sinkBarrier(*q)) ok = false;
            if (!ok) { prev = I; inGroup = 1; continue; }
            static long maxMoves = getenv("XEC_GROUPMAX") ? atol(getenv("XEC_GROUPMAX")) : -1;   // debugging
            if (maxMoves >= 0 && moves >= maxMoves) { prev = I; inGroup = 1; continue; }
            moves++;
            // move them (in their order) and the read to just after prev
            std::vector<llvm::Instruction *> mv;
            for (llvm::Instruction *q = prev->getNextNode(); q && q != I; q = q->getNextNode()) if (need.count(q)) mv.push_back(q);
            mv.push_back(I);
            llvm::Instruction *at = prev;
            for (llvm::Instruction *q : mv) { q->moveAfter(at); at = q; }
            prev = I; inGroup++;
            groupedReads++;
        }
    }
}
// v0.56.4: WindowServer's window-shadow blur (ShadowVerticalBlurRGBAFragment) takes ~96 texture samples in the
// first block, runs a small loop, and only then adds the samples up in the block after the loop. All 96 results were
// live across the loop (far more than the 128 registers) and the shader could not be compiled, so WindowServer
// aborted. Here a texture sample (or a pure one-use value feeding one) whose users are all in one later block is
// moved into that block, just before its first use, when: its block dominates that block and that block
// post-dominates it (every lane that ran the sample still reaches it, so texture derivatives stay right), that block
// is not inside a loop (so the sample still runs once), and the function writes no memory (samples are plain reads).
static bool blockInCycle(llvm::BasicBlock *B) {
    std::vector<llvm::BasicBlock *> st(llvm::succ_begin(B), llvm::succ_end(B));
    std::set<llvm::BasicBlock *> seen;
    while (!st.empty()) {
        llvm::BasicBlock *x = st.back(); st.pop_back();
        if (x == B) return true;
        if (!seen.insert(x).second) continue;
        for (llvm::BasicBlock *y : llvm::successors(x)) st.push_back(y);
    }
    return false;
}
static void sinkAcrossBlocks(LLVMValueRef entry) {
    llvm::Function *F = llvm::unwrap<llvm::Function>(entry);
    for (auto &BB : *F) for (auto &I : BB) if (sinkBarrier(I)) return;     // writes memory: leave it alone
    llvm::DominatorTree DT(*F);
    llvm::PostDominatorTree PDT(*F);
    std::map<llvm::BasicBlock *, bool> cyc;
    auto inCycle = [&](llvm::BasicBlock *B) { auto it = cyc.find(B); if (it != cyc.end()) return it->second;
                                              return cyc[B] = blockInCycle(B); };
    for (int round = 0; round < 4; round++) {
        bool moved = false;
        std::vector<llvm::Instruction *> all;
        for (auto &BB : *F) for (auto &I : BB) all.push_back(&I);
        for (size_t k = all.size(); k-- > 0;) {
            llvm::Instruction *I = all[k];
            if (I->isTerminator() || llvm::isa<llvm::PHINode>(I) || I->use_empty()) continue;
            bool isSample = false;
            if (auto *C = llvm::dyn_cast<llvm::CallInst>(I)) {
                llvm::Function *cf = C->getCalledFunction();
                isSample = cf && (cf->getName().starts_with("air.sample_texture") || cf->getName().starts_with("air.read_texture"));
            }
            bool pure = llvm::isa<llvm::ExtractValueInst>(I) || llvm::isa<llvm::BinaryOperator>(I) || llvm::isa<llvm::CastInst>(I) ||
                        llvm::isa<llvm::InsertElementInst>(I) || llvm::isa<llvm::ExtractElementInst>(I);
            if (!isSample && !(pure && I->hasOneUse())) continue;
            llvm::BasicBlock *A = I->getParent(), *B = nullptr;
            llvm::Instruction *first = nullptr;
            bool ok = true;
            for (llvm::User *u : I->users()) {
                auto *ui = llvm::dyn_cast<llvm::Instruction>(u);
                if (!ui || llvm::isa<llvm::PHINode>(ui)) { ok = false; break; }
                if (!B) B = ui->getParent(); else if (B != ui->getParent()) { ok = false; break; }
                if (!first || ui->comesBefore(first)) first = ui;
            }
            if (!ok || !B || B == A || !DT.dominates(A, B) || !PDT.dominates(B, A) || inCycle(B)) continue;
            I->moveBefore(*B, first->getIterator());
            moved = true;
        }
        if (!moved) break;
    }
}
static void foldConstants(LLVMValueRef entry) {
    llvm::Function *F = llvm::unwrap<llvm::Function>(entry);
    const llvm::DataLayout &DL = F->getParent()->getDataLayout();
    for (int round = 0; round < 8; round++) {
        bool changed = false;
        for (auto &BB : *F)
            for (auto it = BB.begin(); it != BB.end();) {
                llvm::Instruction *I = &*it++;
                llvm::Constant *c = nullptr;
                if (auto *cb = llvm::dyn_cast<llvm::CallInst>(I)) {
                    llvm::Function *callee = cb->getCalledFunction();
                    if (callee && cb->arg_size() == 1 && I->getType()->isIntegerTy(1)) {
                        llvm::StringRef n = callee->getName();
                        bool isAny = n.starts_with("air.any."), isAll = n.starts_with("air.all.");
                        auto *a = llvm::dyn_cast<llvm::Constant>(cb->getArgOperand(0));
                        if ((isAny || isAll) && a && !llvm::isa<llvm::ConstantExpr>(a)) {
                            unsigned cnt = 1;
                            if (auto *vt = llvm::dyn_cast<llvm::FixedVectorType>(a->getType())) cnt = vt->getNumElements();
                            bool ok = true, anyT = false, allT = true;
                            for (unsigned k = 0; k < cnt; k++) {
                                llvm::Constant *e = a->getType()->isVectorTy() ? a->getAggregateElement(k) : a;
                                auto *ci = e ? llvm::dyn_cast<llvm::ConstantInt>(e) : nullptr;
                                if (!ci) { ok = false; break; }
                                if (ci->isOne()) anyT = true; else allT = false;
                            }
                            if (ok) c = llvm::ConstantInt::get(I->getType(), isAny ? anyT : allT);
                        }
                    }
                } else if (auto *ld = llvm::dyn_cast<llvm::LoadInst>(I)) {
                    // v0.68.33: a load (volatile too) of a constant global with a known value - Tint's
                    // tint_volatile_zero guard in Dawn shaders (Brave, Chrome, Electron apps). Folded here, the guard's
                    // compare and branch fold away too; kept as a run-time branch it hung the GPU (Claude Helper)
                    auto *gv = llvm::dyn_cast<llvm::GlobalVariable>(ld->getPointerOperand()->stripPointerCasts());
                    if (gv && gv->isConstant() && gv->hasDefinitiveInitializer() &&
                        gv->getInitializer()->getType() == I->getType() &&
                        (I->getType()->isIntegerTy() || I->getType()->isFloatingPointTy()))
                        c = gv->getInitializer();
                    else if (!I->mayHaveSideEffects()) c = llvm::ConstantFoldInstruction(I, DL);
                } else if (!llvm::isa<llvm::PHINode>(I) && !I->mayHaveSideEffects() && !I->getType()->isVoidTy())
                    c = llvm::ConstantFoldInstruction(I, DL);
                if (c) { I->replaceAllUsesWith(c); I->eraseFromParent(); changed = true; }
            }
        for (auto &BB : *F) if (llvm::ConstantFoldTerminator(&BB, true)) changed = true;
        if (llvm::removeUnreachableBlocks(*F)) changed = true;
        if (!changed) return;
    }
}

// v0.62.0: fully unroll short loops with a fixed step count (at most 8 steps). WindowServer's colour shaders walk the
// red, green and blue channels in a 3-step loop and pick the channel by the loop counter: as a GPU loop every pick is a
// chain of compares and selects (v0.51.7). Unrolled, the counter is a constant in every copy and a pick is a register.
static size_t unrolledLoops;
static bool simTrip(llvm::PHINode *iv, llvm::BasicBlock *pre, llvm::BasicBlock *latch, llvm::BasicBlock *header,
                    llvm::BasicBlock *&exitB, int &trips) {
    auto *start = llvm::dyn_cast<llvm::ConstantInt>(iv->getIncomingValueForBlock(pre));
    auto *next = llvm::dyn_cast<llvm::BinaryOperator>(iv->getIncomingValueForBlock(latch));
    if (!start || !next || next->getOperand(0) != iv) return false;
    auto *stepC = llvm::dyn_cast<llvm::ConstantInt>(next->getOperand(1));
    if (!stepC || (next->getOpcode() != llvm::Instruction::Add && next->getOpcode() != llvm::Instruction::Sub)) return false;
    auto *br = llvm::dyn_cast<llvm::BranchInst>(latch->getTerminator());
    if (!br || !br->isConditional()) return false;
    auto *cmp = llvm::dyn_cast<llvm::ICmpInst>(br->getCondition());
    if (!cmp) return false;
    llvm::Value *a = cmp->getOperand(0);
    auto *limC = llvm::dyn_cast<llvm::ConstantInt>(cmp->getOperand(1));
    if (!limC || (a != iv && a != next)) return false;
    bool contOnTrue = br->getSuccessor(0) == header;
    if (!contOnTrue && br->getSuccessor(1) != header) return false;
    exitB = br->getSuccessor(contOnTrue ? 1 : 0);
    unsigned bits = start->getBitWidth();
    if (bits > 64) return false;
    llvm::APInt i = start->getValue(), step = stepC->getValue(), lim = limC->getValue();
    for (int t = 1; t <= 8; t++) {
        llvm::APInt n = next->getOpcode() == llvm::Instruction::Add ? i + step : i - step;
        llvm::APInt v = a == iv ? i : n;
        bool r;
        switch (cmp->getPredicate()) {
        case llvm::CmpInst::ICMP_EQ: r = v == lim; break;
        case llvm::CmpInst::ICMP_NE: r = v != lim; break;
        case llvm::CmpInst::ICMP_ULT: r = v.ult(lim); break;
        case llvm::CmpInst::ICMP_ULE: r = v.ule(lim); break;
        case llvm::CmpInst::ICMP_UGT: r = v.ugt(lim); break;
        case llvm::CmpInst::ICMP_UGE: r = v.uge(lim); break;
        case llvm::CmpInst::ICMP_SLT: r = v.slt(lim); break;
        case llvm::CmpInst::ICMP_SLE: r = v.sle(lim); break;
        case llvm::CmpInst::ICMP_SGT: r = v.sgt(lim); break;
        case llvm::CmpInst::ICMP_SGE: r = v.sge(lim); break;
        default: return false;
        }
        if (r != contOnTrue) { trips = t; return true; }
        i = n;
    }
    return false;
}
static bool unrollOne(llvm::Function *F, llvm::Loop *L) {
    llvm::BasicBlock *H = L->getHeader(), *latch = L->getLoopLatch(), *pre = L->getLoopPredecessor();
    if (!latch || !pre || L->getExitingBlock() != latch || !L->getSubLoops().empty()) return false;
    if (pre->getTerminator()->getNumSuccessors() != 1) return false;     // a plain jump into the loop
    llvm::BasicBlock *exitB = nullptr;
    int trips = 0;
    llvm::PHINode *iv = nullptr;
    for (llvm::PHINode &P : H->phis()) {
        if (P.getNumIncomingValues() != 2) return false;
        llvm::BasicBlock *e = nullptr; int t = 0;
        if (!iv && P.getType()->isIntegerTy() && simTrip(&P, pre, latch, H, e, t)) { iv = &P; exitB = e; trips = t; }
    }
    if (!iv) return false;
    size_t n = 0;
    for (llvm::BasicBlock *B : L->blocks()) {
        n += B->size();
        for (llvm::Instruction &I : *B)
            if (auto *cb = llvm::dyn_cast<llvm::CallInst>(&I)) {
                llvm::Function *c = cb->getCalledFunction();
                if (!c) return false;
                llvm::StringRef nm = c->getName();      // barriers and atomics stay in their loop
                if (nm.contains("barrier") || nm.contains("atomic") || nm.contains("fence") || nm.contains("simd")) return false;
            }
    }
    if (n * trips > 1500) return false;
    std::vector<llvm::BasicBlock *> blocks(L->blocks().begin(), L->blocks().end());
    std::vector<llvm::ValueToValueMapTy *> maps;
    std::vector<llvm::BasicBlock *> heads, latches;
    for (int k = 0; k < trips; k++) {
        auto *VM = new llvm::ValueToValueMapTy;
        maps.push_back(VM);
        llvm::SmallVector<llvm::BasicBlock *, 16> clones;
        for (llvm::BasicBlock *B : blocks) {
            llvm::BasicBlock *C = llvm::CloneBasicBlock(B, *VM, ".u" + llvm::Twine(k), F);
            (*VM)[B] = C;
            clones.push_back(C);
        }
        for (llvm::PHINode &P : H->phis()) {                 // the header's values: from before the loop, or the last copy
            llvm::Value *in = k == 0 ? P.getIncomingValueForBlock(pre) : P.getIncomingValueForBlock(latch);
            if (k > 0) { auto it = maps[k - 1]->find(in); if (it != maps[k - 1]->end()) in = it->second; }
            auto *cp = llvm::cast<llvm::PHINode>((*VM)[&P]);
            (*VM)[&P] = in;
            cp->eraseFromParent();
        }
        llvm::remapInstructionsInBlocks(clones, *VM);
        heads.push_back(llvm::cast<llvm::BasicBlock>((*VM)[H]));
        latches.push_back(llvm::cast<llvm::BasicBlock>((*VM)[latch]));
    }
    for (int k = 0; k < trips; k++) {                         // copy k continues in copy k + 1, the last one leaves
        llvm::Instruction *t = latches[k]->getTerminator();
        { llvm::IRBuilder<> B(t); B.CreateBr(k + 1 < trips ? heads[k + 1] : exitB); }
        t->eraseFromParent();
    }
    pre->getTerminator()->replaceUsesOfWith(H, heads[0]);
    llvm::ValueToValueMapTy &last = *maps.back();
    for (llvm::PHINode &P : exitB->phis())
        for (unsigned j = 0; j < P.getNumIncomingValues(); j++)
            if (P.getIncomingBlock(j) == latch) {
                llvm::Value *v = P.getIncomingValue(j);
                auto it = last.find(v);
                if (it != last.end()) P.setIncomingValue(j, it->second);
                P.setIncomingBlock(j, latches.back());
            }
    std::set<llvm::BasicBlock *> inLoop(blocks.begin(), blocks.end());
    for (llvm::BasicBlock *B : blocks)                        // values used after the loop: the last copy's
        for (llvm::Instruction &I : *B) {
            auto it = last.find(&I);
            llvm::Value *nv = it != last.end() ? (llvm::Value *)it->second : nullptr;
            for (auto u = I.use_begin(); u != I.use_end();) {
                llvm::Use &U = *u++;
                auto *ui = llvm::dyn_cast<llvm::Instruction>(U.getUser());
                if (ui && !inLoop.count(ui->getParent())) {
                    if (!nv) return false;   // cannot happen for a value that reaches the exit
                    U.set(nv);
                }
            }
        }
    for (llvm::BasicBlock *B : blocks) B->dropAllReferences();
    for (llvm::BasicBlock *B : blocks) {
        for (llvm::Instruction &I : *B) if (!I.use_empty()) I.replaceAllUsesWith(llvm::PoisonValue::get(I.getType()));
    }
    for (llvm::BasicBlock *B : blocks) B->eraseFromParent();
    for (auto *m : maps) delete m;
    unrolledLoops++;
    return true;
}
static void tidyAfterUnroll(llvm::Function *F) {
    for (int round = 0; round < 8; round++) {
        bool changed = false;
        for (auto &BB : *F)
            for (auto it = BB.begin(); it != BB.end();) {
                llvm::Instruction *I = &*it++;
                if (auto *P = llvm::dyn_cast<llvm::PHINode>(I)) {
                    if (llvm::Value *v = P->hasConstantValue()) { if (v != P) { P->replaceAllUsesWith(v); P->eraseFromParent(); changed = true; } }
                    continue;
                }
                if (llvm::isInstructionTriviallyDead(I)) { I->eraseFromParent(); changed = true; }
            }
        for (auto it = F->begin(); it != F->end();) {
            llvm::BasicBlock *B = &*it++;
            if (B != &F->getEntryBlock() && llvm::MergeBlockIntoPredecessor(B)) changed = true;
        }
        if (!changed) return;
    }
}
// v0.65.1: sincos(x, cos&) - air.[fast_]sincos.<t>(x, ptr) returns sin(x) and stores cos(x) through the pointer
// (Sequoia's wallpaper renderer: "function air.fast_sincos.f32 is not supported yet"). Split into sin + cos + store,
// which the rest of the compiler already handles.
static void splitSincos(LLVMValueRef entry) {
    llvm::Function *F = llvm::unwrap<llvm::Function>(entry);
    llvm::Module *M = F->getParent();
    std::vector<llvm::CallInst *> todo;
    for (llvm::BasicBlock &BB : *F)
        for (llvm::Instruction &I : BB)
            if (auto *ci = llvm::dyn_cast<llvm::CallInst>(&I))
                if (llvm::Function *cf = ci->getCalledFunction()) {
                    llvm::StringRef n = cf->getName();
                    if ((n.starts_with("air.sincos.") || n.starts_with("air.fast_sincos.")) && ci->arg_size() == 2 &&
                        !ci->getType()->isVoidTy() && ci->getArgOperand(1)->getType()->isPointerTy())
                        todo.push_back(ci);
                }
    for (llvm::CallInst *ci : todo) {
        std::string n = ci->getCalledFunction()->getName().str();
        bool fast = n.rfind("air.fast_", 0) == 0;
        std::string sfx = n.substr(n.find("sincos.") + 7);
        llvm::Type *T = ci->getType();
        llvm::FunctionType *FT = llvm::FunctionType::get(T, {T}, false);
        llvm::FunctionCallee sinF = M->getOrInsertFunction(std::string("air.") + (fast ? "fast_" : "") + "sin." + sfx, FT);
        llvm::FunctionCallee cosF = M->getOrInsertFunction(std::string("air.") + (fast ? "fast_" : "") + "cos." + sfx, FT);
        llvm::IRBuilder<> B(ci);
        llvm::Value *x = ci->getArgOperand(0);
        llvm::Value *sv = B.CreateCall(sinF, {x});
        llvm::Value *cv = B.CreateCall(cosF, {x});
        B.CreateStore(cv, ci->getArgOperand(1));
        ci->replaceAllUsesWith(sv);
        ci->eraseFromParent();
    }
}
static void unrollShortLoops(LLVMValueRef entry) {
    llvm::Function *F = llvm::unwrap<llvm::Function>(entry);
    for (int round = 0; round < 16; round++) {
        llvm::DominatorTree DT(*F);
        llvm::LoopInfo LI(DT);
        bool did = false;
        for (llvm::Loop *L : LI.getLoopsInPreorder())
            if (L->getSubLoops().empty() && unrollOne(F, L)) { did = true; break; }
        if (!did) break;
        foldConstants(entry);
        tidyAfterUnroll(F);
    }
}

static std::map<int, int> gTestHints, gTestFails;      // XEC_SPILLTEST retries (test only)
static int compileComputeOnce(const void *bitcode, size_t len, const char *function,
                              uint32_t *out, uint32_t maxDW, uint32_t *outDW, uint32_t *outInfo,
                              uint64_t *outSamplers, char *log, size_t logLen);
extern "C" int irisxe_compile_air(const void *bitcode, size_t len, const char *function,
                                  uint32_t *out, uint32_t maxDW, uint32_t *outDW, uint32_t *outInfo,
                                  uint64_t *outSamplers, char *log, size_t logLen) {
    gTestHints.clear();
    int rc = 0;
    for (int attempt = 0; attempt < 30; attempt++) {
        gTestFails.clear();
        rc = compileComputeOnce(bitcode, len, function, out, maxDW, outDW, outInfo, outSamplers, log, logLen);
        if (!getenv("XEC_SPILLTEST") || rc != -3 || gTestFails.empty()) break;
        bool grew = false;
        for (auto &f : gTestFails) if (gTestHints[f.first] < 6) { gTestHints[f.first]++; grew = true; }
        if (!grew) break;
    }
    return rc;
}
static int compileComputeOnce(const void *bitcode, size_t len, const char *function,
                              uint32_t *out, uint32_t maxDW, uint32_t *outDW, uint32_t *outInfo,
                              uint64_t *outSamplers, char *log, size_t logLen) {
    if (log && logLen) log[0] = 0;
    if (outDW) *outDW = 0;
    if (outInfo) *outInfo = 0;
    Compiler c;
    c.loopHint = gTestHints;
    c.ctx = LLVMContextCreate();
    LLVMMemoryBufferRef buf = LLVMCreateMemoryBufferWithMemoryRange((const char *)bitcode, len, "air", 0);
    int rc = 0;
    if (LLVMParseBitcodeInContext2(c.ctx, buf, &c.mod) || !c.mod) {
        if (log) snprintf(log, logLen, "could not read the AIR bitcode (%zu bytes)", len);
        rc = -1;
    } else {
        LLVMValueRef fn = LLVMGetNamedFunction(c.mod, function);
        if (fn && !LLVMIsDeclaration(fn)) { inlineHelpers(fn); splitSincos(fn); foldConstants(fn); }     // v0.51.2, v0.51.5
        if (fn && !LLVMIsDeclaration(fn) && getenv("XEC_SPILLTEST") && getenv("XEC_SPILLIR"))
            pressureSpill(fn, (unsigned)atoi(getenv("XEC_SPILLIR")), 400);              // test only
        if (!fn || LLVMIsDeclaration(fn)) {
            if (log) snprintf(log, logLen, "function %s not found in the AIR", function);
            rc = -2;
        } else {
            try {
                c.run(fn);
                uint32_t n = (uint32_t)c.code.size() * 4;
                if (n > maxDW) {
                    if (log) snprintf(log, logLen, "kernel too big: %u dwords (max %u)", n, maxDW);
                    rc = -4;
                } else {
                    for (size_t i = 0; i < c.code.size(); i++) {
                        out[i * 4 + 0] = (uint32_t)c.code[i].lo;
                        out[i * 4 + 1] = (uint32_t)(c.code[i].lo >> 32);
                        out[i * 4 + 2] = (uint32_t)c.code[i].hi;
                        out[i * 4 + 3] = (uint32_t)(c.code[i].hi >> 32);
                    }
                    if (outDW) *outDW = n;
                    if (outInfo) *outInfo = c.slmBytes | (c.usesBarrier ? 0x80000000u : 0) | ((c.privBytes / 16) << 24) |
                                            ((uint32_t)c.constSamplers.size() << 17);
                    for (size_t k = 0; k < c.constSamplers.size() && outSamplers; k++) outSamplers[k] = c.constSamplers[k];
                    char extra[96] = "";
                    if (c.slmBytes || c.usesBarrier)
                        snprintf(extra, sizeof(extra), ", threadgroup memory %u bytes%s", c.slmBytes, c.usesBarrier ? ", barriers" : "");
                    if (c.privBytes)
                        snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra), ", private %u bytes", c.privBytes);
                    if (c.usesTextures) snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra), ", textures");
                    if (log) snprintf(log, logLen, "ok: %zu instructions, registers g%d..g%u%s", c.code.size(), c.REG_FIRST, c.maxReg, extra);
                }
            } catch (const Fail &f) {
                if (log) snprintf(log, logLen, "%s", f.msg.c_str());
                rc = -3;
                gTestFails = c.loopFail;
            }
        }
        LLVMDisposeModule(c.mod);
    }
    LLVMDisposeMemoryBuffer(buf);
    LLVMContextDispose(c.ctx);
    return rc;
}

static int compileRenderOnce(const void *bitcode, size_t len, const char *function,
                             uint32_t *out, uint32_t maxDW, uint32_t *outDW,
                             struct XeShaderInfo *info, char *log, size_t logLen,
                             const std::set<int> &spills, std::vector<int> *newSpills,
                             uint32_t spillReserve, uint32_t *spillNeed, uint32_t *spillCount,
                             std::map<int, int> &loopHints, std::map<int, int> *loopFails, unsigned pressureTarget) {
    if (log && logLen) log[0] = 0;
    if (outDW) *outDW = 0;
    if (info) memset(info, 0, sizeof(*info));
    Compiler c;
    c.spillMode = spillReserve != 0; c.spillReserve = spillReserve; c.loopHint = loopHints;
    c.ctx = LLVMContextCreate();
    LLVMMemoryBufferRef buf = LLVMCreateMemoryBufferWithMemoryRange((const char *)bitcode, len, "air", 0);
    int rc = 0;
    if (LLVMParseBitcodeInContext2(c.ctx, buf, &c.mod) || !c.mod) {
        if (log) snprintf(log, logLen, "could not read the AIR bitcode (%zu bytes)", len);
        rc = -1;
    } else {
        LLVMValueRef fn = LLVMGetNamedFunction(c.mod, function);
        if (fn && !LLVMIsDeclaration(fn)) { inlineHelpers(fn); splitSincos(fn); foldConstants(fn); if (!getenv("XEC_NOOVERLAP") && !getenv("XEC_NOUNROLL")) unrollShortLoops(fn); if (!getenv("XEC_NOOVERLAP") && !getenv("XEC_NOHOIST") && !c.spillMode && spills.empty() && !pressureTarget) hoistConstLoads(fn);   /* v0.62.2: not when spilling (white window outlines) */ if (getenv("XEC_NOSINK") == nullptr) { sinkAcrossBlocks(fn); sinkSamples(fn); } if (!getenv("XEC_NOOVERLAP") && !getenv("XEC_NOGROUP") && ((!c.spillMode && spills.empty() && !pressureTarget) || getenv("XEC_GROUPSPILL"))) groupReads(fn);   /* v0.61.1: not when spilling (the spill code then lost a sample's value: white window edges) */ if (getenv("XEC_DUMPIR")) LLVMPrintModuleToFile(c.mod, getenv("XEC_DUMPIR"), nullptr);
            if (getenv("XEC_STATS") && spills.empty()) maxLiveStats(fn);
            c.origIdx = numberInstructions(fn);
            if (!spills.empty()) applySpills(fn, c.origIdx, spills);
            if (pressureTarget) pressureSpill(fn, pressureTarget, 400);
            c.curFn = fn; }     // v0.51.2, v0.51.5, v0.51.8
        if (!fn || LLVMIsDeclaration(fn)) {
            if (log) snprintf(log, logLen, "function %s not found in the AIR", function);
            rc = -2;
        } else {
            c.stage = c.findMD("air.vertex", fn) ? XE_STAGE_VERTEX : c.findMD("air.fragment", fn) ? XE_STAGE_FRAGMENT : 0;
            try {
                if (!c.stage) c.fail("%s is not a vertex or fragment function", function);
                c.runRender(fn);
                uint32_t n = (uint32_t)c.code.size() * 4;
                if (n > maxDW) {
                    if (log) snprintf(log, logLen, "shader too big: %u dwords (max %u)", n, maxDW);
                    rc = -4;
                } else {
                    for (size_t i = 0; i < c.code.size(); i++) {
                        out[i * 4 + 0] = (uint32_t)c.code[i].lo;
                        out[i * 4 + 1] = (uint32_t)(c.code[i].lo >> 32);
                        out[i * 4 + 2] = (uint32_t)c.code[i].hi;
                        out[i * 4 + 3] = (uint32_t)(c.code[i].hi >> 32);
                    }
                    if (outDW) *outDW = n;
                    c.rinfo.numConstSamplers = (uint32_t)c.constSamplers.size();
                    for (size_t k = 0; k < c.constSamplers.size() && k < 4; k++) c.rinfo.constSampler[k] = c.constSamplers[k];
                    if (info) *info = c.rinfo;
                    if (log) snprintf(log, logLen, "ok: %s, %zu instructions, registers g%d..g%u, %u %s%s%s",
                                      c.stage == XE_STAGE_VERTEX ? "vertex" : "fragment", c.code.size(), c.REG_FIRST, c.maxReg,
                                      c.stage == XE_STAGE_VERTEX ? c.rinfo.numInputs : c.rinfo.numVaryings,
                                      c.stage == XE_STAGE_VERTEX ? "inputs" : "varyings",
                                      c.usesDiscard ? ", discard" : "", c.usesTextures ? ", textures" : "");
                }
            } catch (const Fail &f) {
                if (log) snprintf(log, logLen, "%s", f.msg.c_str());
                rc = -3;
                if (newSpills) *newSpills = c.newSpills;
            }
            if (spillNeed) *spillNeed = c.spillMax;
            if (loopFails) *loopFails = c.loopFail;
            if (spillCount) *spillCount = c.spillCount;
        }
        LLVMDisposeModule(c.mod);
    }
    LLVMDisposeMemoryBuffer(buf);
    LLVMContextDispose(c.ctx);
    return rc;
}
extern "C" int irisxe_compile_air_render(const void *bitcode, size_t len, const char *function,
                                         uint32_t *out, uint32_t maxDW, uint32_t *outDW,
                                         struct XeShaderInfo *info, char *log, size_t logLen) {
    // v0.51.9: 1) plain; 2) out of registers: spill to scratch while generating code (reserve grows to
    // what was used); 3) still out of registers (values live through loops): those values are moved to
    // private memory in the IR (v0.51.8) and it tries again
    std::set<int> spills;
    std::map<int, int> hints, fails;
    uint32_t reserve = 0, used = 0, count = 0;
    unsigned target = getenv("XEC_TARGET") ? (unsigned)atoi(getenv("XEC_TARGET")) : 0;
    int rc = 0;
    for (int attempt = 0; attempt < 60; attempt++) {
        std::vector<int> more;
        if (outDW) *outDW = 0;
        fails.clear();
        rc = compileRenderOnce(bitcode, len, function, out, maxDW, outDW, info, log, logLen, spills, &more, reserve, &used, &count,
                               hints, &fails, target);
        if (getenv("XEC_SPILLLOG")) fprintf(stderr, "attempt %d rc=%d target=%u reserve=%u used=%u spilled=%u irspills=%zu new=%zu: %s\n",
                                            attempt, rc, target, reserve, used, count, spills.size(), more.size(), log ? log : "");
        bool regs = rc == -3 && log && strstr(log, "out of registers");
        if (rc == 0 && reserve && used > reserve) { reserve = (used + 15) & ~15u; continue; }
        if (regs && !reserve && !getenv("XEC_TARGET")) { reserve = 256; continue; }
        if (regs && used > reserve) { reserve = (used + 15) & ~15u; continue; }
        if (getenv("XEC_TARGET")) break;
        if (regs && reserve && !fails.empty() && attempt < 30) {    // spill before the ifs / loops that ran out
            bool grew = false;
            for (auto &f : fails) if (hints[f.first] < 6) { hints[f.first]++; grew = true; }
            if (grew) continue;
        }
        if (regs && target != 40) { target = target ? (target > 52 ? target - 12 : 40) : 96; continue; }   // pressure-based IR spills
        if (!regs || more.empty()) break;
        size_t before = spills.size();
        spills.insert(more.begin(), more.end());
        if (spills.size() == before) break;
    }
    if (rc == 0 && (count || !spills.empty()) && log) {
        size_t l = strlen(log);
        if (l + 60 < logLen) snprintf(log + l, logLen - l, " [spilled %u, %u bytes scratch per lane]", count, reserve);
    }
    return rc;
}

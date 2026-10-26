#include "analysis.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <deque>
#include <optional>

using v810::Insn;
using v810::Op;

namespace {

// Registers written by an instruction (bit mask), for constant tracking.
uint32_t writtenRegs(const Insn& in) {
    switch (in.op) {
    case Op::CMP_R: case Op::CMP_I: case Op::JMP: case Op::CLI: case Op::SEI: case Op::TRAP:
    case Op::RETI: case Op::HALT: case Op::LDSR: case Op::BCOND: case Op::JR:
    case Op::ST_B: case Op::ST_H: case Op::ST_W: case Op::OUT_B: case Op::OUT_H: case Op::OUT_W:
    case Op::CMPF_S:
        return 0;
    case Op::MUL: case Op::DIV: case Op::MULU: case Op::DIVU:
        return (1u << in.reg2) | (1u << 30);
    case Op::BSTR:
        return 0x7Cu << 24; // r26-r30
    case Op::JAL:
        return 0xFFFFFFFFu; // calls clobber everything as far as we know
    default:
        return 1u << in.reg2;
    }
}

struct ConstState {
    std::array<std::optional<uint32_t>, 32> r;
    ConstState() { r[0] = 0; }

    void step(const Insn& in) {
        std::optional<uint32_t> v;
        auto get = [&](int i) { return r[i]; };
        switch (in.op) {
        case Op::MOV_R: v = get(in.reg1); break;
        case Op::MOV_I: v = (uint32_t)in.imm; break;
        case Op::ADD_I: if (get(in.reg2)) v = *get(in.reg2) + (uint32_t)in.imm; break;
        case Op::ADD_R: if (get(in.reg1) && get(in.reg2)) v = *get(in.reg1) + *get(in.reg2); break;
        case Op::MOVEA:
        case Op::ADDI: if (get(in.reg1)) v = *get(in.reg1) + (uint32_t)in.imm; break;
        case Op::MOVHI: if (get(in.reg1)) v = *get(in.reg1) + ((uint32_t)in.imm << 16); break;
        case Op::ORI: if (get(in.reg1)) v = *get(in.reg1) | (uint32_t)in.imm; break;
        default: break;
        }
        const uint32_t w = writtenRegs(in);
        for (int i = 1; i < 32; i++)
            if (w & (1u << i)) r[i].reset();
        if (v && in.reg2 != 0 && (w & (1u << in.reg2))) r[in.reg2] = v;
    }
};

} // namespace

void Analyzer::run() {
    struct Root {
        uint32_t addr;
        bool speculative;
    };
    std::deque<Root> queue;

    // Reset, interrupt and exception vectors.
    const uint32_t vectors[] = {
        0xFFFFFFF0, 0xFFFFFE00, 0xFFFFFE10, 0xFFFFFE20, 0xFFFFFE30, 0xFFFFFE40,
        0xFFFFFF60, 0xFFFFFF80, 0xFFFFFF90, 0xFFFFFFA0, 0xFFFFFFB0, 0xFFFFFFC0, 0xFFFFFFD0,
    };
    for (uint32_t v : vectors)
        if (rom_.contains(v)) queue.push_back({v, v != 0xFFFFFFF0 && v != 0xFFFFFE40});
    for (uint32_t e : opts_.extraEntries) queue.push_back({e, false});

    std::set<uint32_t> rejected;
    auto drain = [&]() {
        while (!queue.empty()) {
            Root root = queue.front();
            queue.pop_front();
            if (functions_.count(root.addr) || !rom_.contains(root.addr) || (root.addr & 1)) continue;
            if (root.speculative && rejected.count(root.addr)) continue;
            Function fn = analyzeFunction(root.addr, root.speculative);
            if (root.speculative && (fn.hasInvalid || !fn.externalTargets.empty())) {
                rejected.insert(root.addr);
                continue;
            }
            bool plausible = true;
            for (uint32_t x : fn.exits) plausible &= isPlausibleEntry(x);
            if (root.speculative && !plausible) {
                rejected.insert(root.addr);
                continue;
            }
            for (uint32_t x : fn.exits) {
                Function* owner = ownerOf(x);
                if (owner && !owner->labels.count(x)) {
                    owner->labels.insert(x);
                    buildBlocks(*owner);
                }
            }
            for (uint32_t c : fn.calls) queue.push_back({c, root.speculative});
            functions_.emplace(root.addr, std::move(fn));
        }
    };
    drain();

    if (opts_.scanCodePointers) {
        // Function pointers stored in ROM tables (e.g. state machines, callbacks).
        // A pointer into code we already have becomes an extra entry label of that
        // function instead of a new (mostly duplicated) function.
        for (int pass = 0; pass < 4; pass++) {
            size_t before = functions_.size();
            for (uint32_t p : scanCodePointers()) {
                if (Function* owner = ownerOf(p)) {
                    if (!isPlausibleEntry(p)) continue;
                    if (!owner->labels.count(p)) {
                        owner->labels.insert(p);
                        buildBlocks(*owner);
                    }
                } else {
                    queue.push_back({p, true});
                }
            }
            drain();
            if (functions_.size() == before) break;
        }
    }
}

// An address inside existing code is only believable as an entry point if it
// already starts a block or directly follows an unconditional control transfer.
bool Analyzer::isPlausibleEntry(uint32_t addr) {
    Function* owner = ownerOf(addr);
    if (!owner) return true;
    if (owner->labels.count(addr)) return true;
    for (auto& [e, fn] : functions_) {
        auto it = fn.insns.lower_bound(addr);
        if (it == fn.insns.begin()) continue;
        --it;
        const Insn& prev = it->second;
        if (prev.next() != addr) continue;
        switch (prev.op) {
        case Op::JMP: case Op::JR: case Op::RETI: return true;
        case Op::BCOND: return prev.cond == v810::C_T;
        default: return false;
        }
    }
    return false;
}

Function* Analyzer::ownerOf(uint32_t addr) {
    for (auto& [e, fn] : functions_)
        if (fn.insns.count(addr)) return &fn;
    return nullptr;
}

bool Analyzer::looksLikeCode(uint32_t addr) const {
    // Linear sweep: valid instructions until an unconditional transfer.
    for (int i = 0; i < 1024; i++) {
        if (!rom_.contains(addr) || addr + 4 < addr) return false;
        Insn in = rom_.decodeAt(addr);
        if (in.op == Op::INVALID) return false;
        if (in.raw == 0xFFFFFFFFu || (in.raw & 0xFFFF) == 0x0000) return false; // padding / "mov r0,r0"
        if (in.op == Op::JMP || in.op == Op::RETI || in.op == Op::JR) return true;
        if (in.op == Op::BCOND && in.cond == v810::C_T) return true;
        addr = in.next();
    }
    return false;
}

std::vector<uint32_t> Analyzer::scanCodePointers() const {
    // Only accept pointers into the span of code we have already found, landing on
    // an address that is not in the middle of a known instruction.
    uint32_t lo = 0xFFFFFFFF, hi = 0;
    std::set<uint32_t> insnStarts, insnMiddles;
    for (auto& [entry, fn] : functions_) {
        for (auto& [a, in] : fn.insns) {
            lo = std::min(lo, a);
            hi = std::max(hi, in.next());
            insnStarts.insert(a);
            if (in.len == 4) insnMiddles.insert(a + 2);
        }
    }
    std::vector<uint32_t> out;
    for (size_t o = 0; o + 4 <= rom_.data.size(); o += 4) {
        uint32_t v = rom_.read32(rom_.base + (uint32_t)o);
        if ((v & 1) || v < lo || v >= hi || !rom_.contains(v)) continue;
        if (functions_.count(v) || insnMiddles.count(v)) continue;
        if (!looksLikeCode(v)) continue;
        out.push_back(v);
    }
    return out;
}

// Recognizes compiler-generated switch tables:
//     shl 2, rI ; movhi hi(T), rI, rB ; ld.w lo(T)[rB], rX ; jmp [rX]
// (and variations with add/movea). Values are tracked symbolically as either a
// constant or "constant + scaled index".
std::vector<uint32_t> Analyzer::resolveJumpTable(const std::vector<Insn>& trace, const Function& fn,
                                                 const Insn& jmp) const {
    // INDEXED values are c + scale * index, assuming a single index variable.
    enum Kind { UNKNOWN, CONST, INDEXED, TABLE };
    struct Sym {
        Kind kind = UNKNOWN;
        uint32_t c = 0;
        int32_t scale = 0;
    };
    std::array<Sym, 32> r;
    r[0] = {CONST, 0, 0};
    auto lin = [](const Sym& s) { return s.kind == CONST || s.kind == INDEXED; };
    auto make = [](uint32_t c, int32_t scale) { return Sym{scale ? INDEXED : CONST, c, scale}; };

    for (const Insn& in : trace) {
        Sym res;
        const Sym a = r[in.reg1], b = r[in.reg2];
        switch (in.op) {
        case Op::MOV_R: res = a; break;
        case Op::MOV_I: res = {CONST, (uint32_t)in.imm, 0}; break;
        case Op::SHL_I:
            if (lin(b)) res = make(b.c << in.imm, b.scale << in.imm);
            else if (b.kind == UNKNOWN) res = make(0, 1 << in.imm);
            break;
        case Op::ADD_R:
            if (lin(a) && lin(b)) res = make(a.c + b.c, a.scale + b.scale);
            break;
        case Op::SUB: // reg2 - reg1
            if (lin(a) && lin(b)) res = make(b.c - a.c, b.scale - a.scale);
            break;
        case Op::ADD_I:
            if (lin(b)) res = make(b.c + (uint32_t)in.imm, b.scale);
            break;
        case Op::MOVEA:
        case Op::ADDI:
            if (lin(a)) res = make(a.c + (uint32_t)in.imm, a.scale);
            break;
        case Op::MOVHI:
            if (lin(a)) res = make(a.c + ((uint32_t)in.imm << 16), a.scale);
            break;
        case Op::LD_W:
            if (a.kind == INDEXED) res = {TABLE, a.c + (uint32_t)in.imm, 0};
            break;
        default: break;
        }
        const uint32_t w = writtenRegs(in);
        for (int i = 1; i < 32; i++)
            if (w & (1u << i)) r[i] = Sym();
        if (in.reg2 != 0 && (w & (1u << in.reg2))) r[in.reg2] = res;
    }

    const Sym t = r[jmp.reg1];
    if (t.kind == INDEXED && rom_.contains(t.c) && t.scale != 0 && (t.scale & 1) == 0) {
        // Computed jump into unrolled code: base + k*scale. A candidate is accepted
        // if straight-line decoding from the lower address lands exactly on the
        // higher one (i.e. it is on an instruction boundary of the unrolled body).
        auto straightLine = [&](uint32_t from, uint32_t to) {
            for (uint32_t a = from; a < to;) {
                if (!rom_.contains(a)) return false;
                Insn in = rom_.decodeAt(a);
                if (in.op == Op::INVALID || v810::isBlockTerminator(in)) return false;
                a = in.next();
                if (a == to) return true;
                if (a > to) return false;
            }
            return from == to;
        };
        (void)fn;
        std::vector<uint32_t> targets{t.c};
        uint32_t prev = t.c;
        for (int k = 1; k < 4096; k++) {
            const uint32_t a = t.c + (uint32_t)(t.scale * k);
            if (!rom_.contains(a)) break;
            if (!(t.scale < 0 ? straightLine(a, prev) : straightLine(prev, a))) break;
            targets.push_back(a);
            prev = a;
        }
        return targets;
    }
    if (t.kind != TABLE || !rom_.contains(t.c)) return {};

    // Look for a bounds check (cmp against a constant) just before the dispatch.
    uint32_t limit = 512;
    {
        auto it = fn.insns.lower_bound(jmp.addr);
        std::optional<uint32_t> movConst;
        int n = 0;
        while (it != fn.insns.begin() && n++ < 16) {
            --it;
            const Insn& in = it->second;
            if (in.op == Op::CMP_I) {
                limit = (uint32_t)in.imm + 1;
                break;
            }
            if (in.op == Op::CMP_R) {
                // The constant is loaded into reg1 by the preceding instruction.
                auto prev = it;
                if (prev != fn.insns.begin()) {
                    --prev;
                    const Insn& p = prev->second;
                    if ((p.op == Op::MOVEA || p.op == Op::MOV_I) && p.reg2 == in.reg1 &&
                        (p.op == Op::MOV_I || p.reg1 == 0))
                        limit = (uint32_t)p.imm + 1;
                }
                break;
            }
        }
        if (limit > 4096) limit = 512;
    }

    std::vector<uint32_t> targets;
    for (uint32_t i = 0; i < limit; i++) {
        uint32_t e = t.c + i * 4;
        if (!rom_.contains(e) || e + 4 < e) break;
        uint32_t v = rom_.read32(e);
        const int64_t dist = (int64_t)v - (int64_t)jmp.addr;
        if ((v & 1) || !rom_.contains(v) || dist > 0x40000 || dist < -0x40000) break;
        targets.push_back(v);
    }
    return targets;
}

Function Analyzer::analyzeFunction(uint32_t entry, bool speculative) {
    Function fn;
    fn.entry = entry;
    fn.labels.insert(entry);

    struct Work {
        uint32_t addr;
        ConstState consts;
    };
    std::deque<Work> work;
    work.push_back({entry, ConstState()});

    auto branchTo = [&](uint32_t target, const ConstState& cs) {
        if (!rom_.contains(target)) {
            fn.externalTargets.insert(target);
            return;
        }
        fn.labels.insert(target);
        work.push_back({target, cs});
    };

    while (!work.empty() && fn.insns.size() < opts_.maxFunctionInsns) {
        Work w = work.front();
        work.pop_front();
        uint32_t a = w.addr;
        ConstState cs = w.consts;
        std::vector<Insn> trace;

        for (;;) {
            if (fn.insns.count(a)) {
                fn.labels.insert(a);
                break;
            }
            if (!rom_.contains(a)) {
                fn.externalTargets.insert(a);
                break;
            }
            if (speculative && a != entry && ownerOf(a)) {
                // Guessed entry points jump into existing code instead of copying it.
                fn.exits.insert(a);
                break;
            }
            Insn in = rom_.decodeAt(a);
            if (in.len == 4 && a + 2 < a) { // runs off the end of the address space
                in.op = Op::INVALID;
                in.len = 2;
            }
            fn.insns[a] = in;

            bool stop = false;
            switch (in.op) {
            case Op::BCOND:
                if (in.cond == v810::C_T) {
                    branchTo(in.target(), cs);
                    stop = true;
                } else if (in.cond != v810::C_F) {
                    branchTo(in.target(), cs);
                    fn.labels.insert(in.next());
                }
                break;
            case Op::JR:
                branchTo(in.target(), cs);
                stop = true;
                break;
            case Op::JAL:
                fn.calls.insert(in.target());
                fn.labels.insert(in.next());
                break;
            case Op::JMP: {
                auto target = cs.r[in.reg1];
                auto ret = cs.r[31];
                if (in.reg1 != 31 && ret && rom_.contains(*ret)) {
                    // r31 holds a constant return address: an indirect call.
                    fn.indirectCalls[a] = *ret;
                    if (target && rom_.contains(*target)) fn.calls.insert(*target);
                    branchTo(*ret, ConstState());
                } else if (target) {
                    fn.constJumps[a] = *target;
                    branchTo(*target, cs);
                } else if (in.reg1 != 31) {
                    auto targets = resolveJumpTable(trace, fn, in);
                    if (!targets.empty()) {
                        for (uint32_t t : targets) branchTo(t, ConstState());
                        fn.jumpTables[a] = std::move(targets);
                    }
                }
                stop = true;
                break;
            }
            case Op::RETI:
            case Op::TRAP:
                stop = true;
                break;
            case Op::HALT:
                fn.labels.insert(in.next());
                break;
            case Op::INVALID:
                fn.hasInvalid = true;
                stop = true;
                break;
            default:
                break;
            }
            if (stop) break;
            trace.push_back(in);
            cs.step(in);
            if (in.op == Op::JAL) cs = ConstState();
            a = in.next();
        }
    }

    buildBlocks(fn);
    return fn;
}

void Analyzer::buildBlocks(Function& fn) {
    fn.blocks.clear();
    const Insn* prev = nullptr;
    for (auto& [a, in] : fn.insns) {
        bool newBlock = !prev || fn.labels.count(a) || v810::isBlockTerminator(*prev) || prev->next() != a;
        if (newBlock) {
            fn.blocks.push_back(Block{a, {}});
            fn.labels.insert(a);
        }
        fn.blocks.back().insns.push_back(in);
        prev = &in;
    }
    // Branch targets that landed inside another instruction are not real labels.
    for (auto it = fn.labels.begin(); it != fn.labels.end();) {
        if (!fn.insns.count(*it)) it = fn.labels.erase(it);
        else ++it;
    }
}

void Analyzer::printStats() const {
    size_t insns = 0, blocks = 0, dyn = 0, icalls = 0, cj = 0;
    std::set<uint32_t> unique;
    for (auto& [e, fn] : functions_) {
        insns += fn.insns.size();
        blocks += fn.blocks.size();
        icalls += fn.indirectCalls.size();
        cj += fn.constJumps.size();
        for (auto& [a, in] : fn.insns) {
            unique.insert(a);
            if (in.op == Op::JMP && in.reg1 != 31 && !fn.constJumps.count(a) && !fn.indirectCalls.count(a) &&
                !fn.jumpTables.count(a))
                dyn++;
        }
    }
    std::printf("functions: %zu\ninstructions: %zu emitted (%zu unique)\nblocks: %zu\n", functions_.size(), insns,
                unique.size(), blocks);
    std::printf("constant jumps: %zu, indirect calls: %zu, unresolved jumps: %zu\n", cj, icalls, dyn);
}

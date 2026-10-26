#include "codegen.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <vector>

using v810::Insn;
using v810::Op;

namespace {

std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

// Register read expression (r0 is hardwired to zero).
std::string R(int n) {
    return n == 0 ? std::string("0u") : fmt("ctx->r[%d]", n);
}

// Assignment to reg2; writes to r0 only keep side effects.
std::string assign(int reg, const std::string& expr) {
    if (reg == 0) return "(void)(" + expr + ");";
    return fmt("ctx->r[%d] = ", reg) + expr + ";";
}

std::string imm32(int32_t v) {
    return fmt("0x%Xu", (uint32_t)v);
}

std::string funcName(uint32_t a) {
    return fmt("rec_%08X", a);
}

std::string labelName(uint32_t a) {
    return fmt("L_%08X", a);
}

std::map<uint32_t, uint32_t> g_labelOwner; // label -> function entry

} // namespace

std::string CodeGen::jumpTo(const Function& fn, uint32_t target) const {
    if (fn.labels.count(target)) return "goto " + labelName(target) + ";";
    return fmt("{ ctx->pc = 0x%08Xu; return; }", target);
}

std::string CodeGen::callTo(uint32_t target) const {
    auto it = g_labelOwner.find(target);
    if (it != g_labelOwner.end()) return funcName(it->second) + "(ctx);";
    return "vb::dispatch(ctx);";
}

void CodeGen::emitInsn(std::string& o, const Function& fn, const Insn& in, bool fl) const {
    (void)fn;
    const int r1 = in.reg1, r2 = in.reg2;
    const std::string a = R(r1), b = R(r2);
    auto line = [&](const std::string& s) { o += "    " + s + "\n"; };
    auto ea = [&]() { return in.imm ? a + " + " + imm32(in.imm) : a; };

    switch (in.op) {
    case Op::MOV_R: line(assign(r2, a)); break;
    case Op::ADD_R: line(assign(r2, fl ? "vb::f_add(ctx, " + b + ", " + a + ")" : b + " + " + a)); break;
    case Op::SUB: line(assign(r2, fl ? "vb::f_sub(ctx, " + b + ", " + a + ")" : b + " - " + a)); break;
    case Op::CMP_R: if (fl) line("vb::f_sub(ctx, " + b + ", " + a + ");"); break;
    case Op::SHL_R: line(assign(r2, fl ? "vb::f_shl(ctx, " + b + ", " + a + " & 31)" : b + " << (" + a + " & 31)")); break;
    case Op::SHR_R: line(assign(r2, fl ? "vb::f_shr(ctx, " + b + ", " + a + " & 31)" : b + " >> (" + a + " & 31)")); break;
    case Op::SAR_R:
        line(assign(r2, fl ? "vb::f_sar(ctx, " + b + ", " + a + " & 31)"
                           : "(uint32_t)((int32_t)" + b + " >> (" + a + " & 31))"));
        break;
    case Op::MUL: line(fmt("vb::op_mul(ctx, %d, %d);", r1, r2)); break;
    case Op::MULU: line(fmt("vb::op_mulu(ctx, %d, %d);", r1, r2)); break;
    case Op::DIV: line(fmt("vb::op_div(ctx, %d, %d);", r1, r2)); break;
    case Op::DIVU: line(fmt("vb::op_divu(ctx, %d, %d);", r1, r2)); break;
    case Op::OR: line(assign(r2, fl ? "vb::f_logic(ctx, " + b + " | " + a + ")" : b + " | " + a)); break;
    case Op::AND: line(assign(r2, fl ? "vb::f_logic(ctx, " + b + " & " + a + ")" : b + " & " + a)); break;
    case Op::XOR: line(assign(r2, fl ? "vb::f_logic(ctx, " + b + " ^ " + a + ")" : b + " ^ " + a)); break;
    case Op::NOT: line(assign(r2, fl ? "vb::f_logic(ctx, ~" + a + ")" : "~" + a)); break;

    case Op::MOV_I: line(assign(r2, imm32(in.imm))); break;
    case Op::ADD_I: line(assign(r2, fl ? "vb::f_add(ctx, " + b + ", " + imm32(in.imm) + ")" : b + " + " + imm32(in.imm))); break;
    case Op::SETF: line(assign(r2, fmt("vb::cond(ctx->psw, %d) ? 1u : 0u", in.cond))); break;
    case Op::CMP_I: if (fl) line("vb::f_sub(ctx, " + b + ", " + imm32(in.imm) + ");"); break;
    case Op::SHL_I: line(assign(r2, fl ? fmt("vb::f_shl(ctx, %s, %d)", b.c_str(), in.imm) : fmt("%s << %d", b.c_str(), in.imm))); break;
    case Op::SHR_I: line(assign(r2, fl ? fmt("vb::f_shr(ctx, %s, %d)", b.c_str(), in.imm) : fmt("%s >> %d", b.c_str(), in.imm))); break;
    case Op::SAR_I:
        line(assign(r2, fl ? fmt("vb::f_sar(ctx, %s, %d)", b.c_str(), in.imm)
                           : fmt("(uint32_t)((int32_t)%s >> %d)", b.c_str(), in.imm)));
        break;
    case Op::CLI: line("vb::cli(ctx);"); break;
    case Op::SEI: line("ctx->psw |= vb::PSW_ID;"); break;
    case Op::LDSR: line(fmt("vb::ldsr(ctx, %d, %s);", in.sub, b.c_str())); break;
    case Op::STSR: line(assign(r2, fmt("vb::stsr(ctx, %d)", in.sub))); break;
    case Op::BSTR: line(fmt("vb::bitstring(ctx, %d);", in.sub)); break;

    case Op::MOVEA: line(assign(r2, a + " + " + imm32(in.imm))); break;
    case Op::ADDI: line(assign(r2, fl ? "vb::f_add(ctx, " + a + ", " + imm32(in.imm) + ")" : a + " + " + imm32(in.imm))); break;
    case Op::MOVHI: line(assign(r2, a + " + " + imm32(in.imm << 16))); break;
    case Op::ORI: line(assign(r2, fl ? "vb::f_logic(ctx, " + a + " | " + imm32(in.imm) + ")" : a + " | " + imm32(in.imm))); break;
    case Op::ANDI: line(assign(r2, fl ? "vb::f_logic(ctx, " + a + " & " + imm32(in.imm) + ")" : a + " & " + imm32(in.imm))); break;
    case Op::XORI: line(assign(r2, fl ? "vb::f_logic(ctx, " + a + " ^ " + imm32(in.imm) + ")" : a + " ^ " + imm32(in.imm))); break;

    case Op::LD_B: line(assign(r2, "(uint32_t)(int8_t)vb::mem_read8(" + ea() + ")")); break;
    case Op::LD_H: line(assign(r2, "(uint32_t)(int16_t)vb::mem_read16(" + ea() + ")")); break;
    case Op::LD_W: case Op::IN_W: line(assign(r2, "vb::mem_read32(" + ea() + ")")); break;
    case Op::IN_B: line(assign(r2, "(uint32_t)vb::mem_read8(" + ea() + ")")); break;
    case Op::IN_H: line(assign(r2, "(uint32_t)vb::mem_read16(" + ea() + ")")); break;
    case Op::ST_B: case Op::OUT_B: line("vb::mem_write8(" + ea() + ", (uint8_t)" + b + ");"); break;
    case Op::ST_H: case Op::OUT_H: line("vb::mem_write16(" + ea() + ", (uint16_t)" + b + ");"); break;
    case Op::ST_W: case Op::OUT_W: line("vb::mem_write32(" + ea() + ", " + b + ");"); break;
    case Op::CAXI: line(fmt("vb::op_caxi(ctx, %d, %d, %s);", r1, r2, imm32(in.imm).c_str())); break;

    case Op::CMPF_S: line(fmt("vb::op_cmpf(ctx, %d, %d);", r1, r2)); break;
    case Op::CVT_WS: line(fmt("vb::op_cvt_ws(ctx, %d, %d);", r1, r2)); break;
    case Op::CVT_SW: line(fmt("vb::op_cvt_sw(ctx, %d, %d);", r1, r2)); break;
    case Op::TRNC_SW: line(fmt("vb::op_trnc_sw(ctx, %d, %d);", r1, r2)); break;
    case Op::ADDF_S: line(fmt("vb::op_addf(ctx, %d, %d);", r1, r2)); break;
    case Op::SUBF_S: line(fmt("vb::op_subf(ctx, %d, %d);", r1, r2)); break;
    case Op::MULF_S: line(fmt("vb::op_mulf(ctx, %d, %d);", r1, r2)); break;
    case Op::DIVF_S: line(fmt("vb::op_divf(ctx, %d, %d);", r1, r2)); break;
    case Op::XB: line(assign(r2, "(" + b + " & 0xFFFF0000u) | ((" + b + " >> 8) & 0xFFu) | ((" + b + " & 0xFFu) << 8)")); break;
    case Op::XH: line(assign(r2, "(" + b + " << 16) | (" + b + " >> 16)")); break;
    case Op::REV: line(assign(r2, "vb::bit_reverse(" + a + ")")); break;
    case Op::MPYHW: line(fmt("vb::op_mpyhw(ctx, %d, %d);", r1, r2)); break;

    default:
        // Control flow is handled by emitTerminator.
        break;
    }
}

void CodeGen::emitTerminator(std::string& o, const Function& fn, const Block& blk, const Insn& in) const {
    auto line = [&](const std::string& s) { o += "    " + s + "\n"; };
    const uint32_t next = in.next();

    switch (in.op) {
    case Op::BCOND:
        if (in.cond == v810::C_T) line(jumpTo(fn, in.target()));
        else line(fmt("if (vb::cond(ctx->psw, %d)) ", in.cond) + jumpTo(fn, in.target()));
        break;
    case Op::JR:
        line(jumpTo(fn, in.target()));
        break;
    case Op::JAL:
        line(fmt("ctx->r[31] = 0x%08Xu;", next));
        line(fmt("ctx->pc = 0x%08Xu;", in.target()));
        line(callTo(in.target()));
        line(fmt("if (ctx->pc != 0x%08Xu) return;", next));
        break;
    case Op::JMP: {
        if (in.reg1 == 31 && !fn.constJumps.count(in.addr)) {
            line("ctx->pc = ctx->r[31];");
            line("return;");
            break;
        }
        auto cj = fn.constJumps.find(in.addr);
        if (cj != fn.constJumps.end()) {
            line(jumpTo(fn, cj->second));
            break;
        }
        auto ic = fn.indirectCalls.find(in.addr);
        if (ic != fn.indirectCalls.end()) {
            line(fmt("ctx->pc = %s;", R(in.reg1).c_str()));
            line("vb::dispatch(ctx);");
            line(fmt("if (ctx->pc != 0x%08Xu) return;", ic->second));
            line(jumpTo(fn, ic->second));
            break;
        }
        // Jump tables and other computed jumps: try the targets of this function first.
        line(fmt("ctx->pc = %s;", R(in.reg1).c_str()));
        line("goto dispatch;");
        break;
    }
    case Op::RETI:
        line("vb::reti(ctx);");
        line("return;");
        break;
    case Op::TRAP:
        line(fmt("ctx->pc = 0x%08Xu;", next));
        line(fmt("vb::trap(ctx, %d);", in.sub));
        line("return;");
        break;
    case Op::HALT:
        line(fmt("ctx->pc = 0x%08Xu;", next));
        line("vb::halt(ctx);");
        line(fmt("if (ctx->pc != 0x%08Xu) return;", next));
        break;
    case Op::INVALID:
        line(fmt("vb::illegal(ctx, 0x%08Xu);", in.addr));
        line("return;");
        break;
    default:
        break;
    }
    (void)blk;
}

static bool fallsThrough(const Insn& in) {
    switch (in.op) {
    case Op::JR: case Op::JMP: case Op::RETI: case Op::TRAP: case Op::INVALID: return false;
    case Op::BCOND: return in.cond != v810::C_T;
    default: return true;
    }
}

static bool needsDispatch(const Function& fn) {
    for (auto& [a, in] : fn.insns) {
        if (in.op != Op::JMP) continue;
        if (in.reg1 == 31 && !fn.constJumps.count(a)) continue;
        if (fn.constJumps.count(a) || fn.indirectCalls.count(a)) continue;
        return true;
    }
    return false;
}

std::string CodeGen::emitFunction(const Function& fn) const {
    std::string o;
    o.reserve(fn.insns.size() * 80);
    o += fmt("void %s(vb::Cpu* ctx) {\n", funcName(fn.entry).c_str());

    // Entry: functions can be (re)entered at any of their labels, which is how
    // returns into the middle of a function and computed jumps are resolved.
    o += fmt("    if (ctx->pc == 0x%08Xu) goto %s;\n", fn.entry, labelName(fn.entry).c_str());
    if (needsDispatch(fn)) o += "dispatch:\n";
    o += "    switch (ctx->pc) {\n";
    for (uint32_t l : fn.labels) o += fmt("    case 0x%08Xu: goto %s;\n", l, labelName(l).c_str());
    o += "    default: return;\n    }\n";

    for (size_t bi = 0; bi < fn.blocks.size(); bi++) {
        const Block& blk = fn.blocks[bi];

        // Dead flag analysis within the block (flags are assumed live at block exit).
        std::vector<bool> flagsLive(blk.insns.size(), true);
        if (opts_.deadFlags) {
            uint32_t live = v810::FLAGS_ALL;
            for (size_t i = blk.insns.size(); i-- > 0;) {
                const Insn& in = blk.insns[i];
                const uint32_t def = v810::flagsWritten(in);
                flagsLive[i] = (def & live) != 0;
                live = (live & ~def) | v810::flagsRead(in);
            }
        }

        int cyc = 0;
        for (const Insn& in : blk.insns) cyc += v810::cycles(in);

        o += fmt("%s:\n", labelName(blk.start).c_str());
        o += fmt("    VB_BLOCK(0x%08Xu, %d);\n", blk.start, cyc);
        for (size_t i = 0; i < blk.insns.size(); i++) {
            const Insn& in = blk.insns[i];
            if (opts_.comments) o += fmt("    // %08X: ", in.addr) + v810::disassemble(in) + "\n";
            if (v810::isBlockTerminator(in)) emitTerminator(o, fn, blk, in);
            else emitInsn(o, fn, in, flagsLive[i]);
        }

        const Insn& last = blk.insns.back();
        if (fallsThrough(last)) {
            const uint32_t succ = last.next();
            const bool nextIsSucc = bi + 1 < fn.blocks.size() && fn.blocks[bi + 1].start == succ;
            if (!nextIsSucc) o += "    " + jumpTo(fn, succ) + "\n";
        }
    }
    o += "}\n\n";
    return o;
}

bool CodeGen::write(const std::string& outDir, std::string& error) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(outDir, ec);
    if (ec) {
        error = "cannot create " + outDir + ": " + ec.message();
        return false;
    }

    const auto& funcs = an_.functions();
    g_labelOwner.clear();
    for (auto& [e, fn] : funcs) g_labelOwner[e] = e;
    for (auto& [e, fn] : funcs)
        for (uint32_t l : fn.labels) g_labelOwner.emplace(l, e);

    auto writeFile = [&](const std::string& name, const std::string& text) {
        std::string path = (fs::path(outDir) / name).string();
        // Leave untouched files alone so incremental builds stay incremental.
        {
            std::ifstream in(path, std::ios::binary);
            if (in) {
                std::string old((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                if (old == text) return true;
            }
        }
        std::ofstream f(path, std::ios::binary);
        if (!f) {
            error = "cannot write " + path;
            return false;
        }
        f << text;
        return true;
    };

    const std::string banner = fmt("// Generated by vbrecomp from \"%s\" (%s, CRC32 %08X). Do not edit.\n",
                                   rom_.title().c_str(), rom_.gameCode().c_str(), rom_.crc32());

    // Header with all function declarations.
    std::string h = banner + "#pragma once\n\n#include \"vbrt/recomp.h\"\n\n";
    for (auto& [e, fn] : funcs) h += fmt("void %s(vb::Cpu* ctx);\n", funcName(e).c_str());
    if (!writeFile("rec_funcs.h", h)) return false;

    // Distribute functions across chunks, balancing by instruction count.
    std::vector<std::string> chunks(opts_.chunks);
    std::vector<size_t> load(opts_.chunks, 0);
    std::vector<const Function*> order;
    for (auto& [e, fn] : funcs) order.push_back(&fn);
    std::stable_sort(order.begin(), order.end(),
                     [](const Function* x, const Function* y) { return x->insns.size() > y->insns.size(); });
    std::vector<std::vector<const Function*>> assigned(opts_.chunks);
    for (const Function* fn : order) {
        size_t best = std::min_element(load.begin(), load.end()) - load.begin();
        load[best] += fn->insns.size();
        assigned[best].push_back(fn);
    }
    for (int c = 0; c < opts_.chunks; c++) {
        std::sort(assigned[c].begin(), assigned[c].end(),
                  [](const Function* x, const Function* y) { return x->entry < y->entry; });
        std::string s = banner + "#include \"rec_funcs.h\"\n\n";
        for (const Function* fn : assigned[c]) s += emitFunction(*fn);
        if (!writeFile(fmt("rec_%d.cpp", c), s)) return false;
    }

    // Lookup table: every label -> function that can be entered there.
    std::string t = banner + "#include \"rec_funcs.h\"\n\n";
    t += fmt("extern const uint32_t vb_rec_rom_crc32 = 0x%08Xu;\n", rom_.crc32());
    t += fmt("extern const uint32_t vb_rec_rom_size = 0x%Xu;\n", (uint32_t)rom_.data.size());
    t += "extern const char vb_rec_game_title[] = \"" + rom_.title() + "\";\n\n";
    t += "extern const vb::RecEntry vb_rec_table[] = {\n";
    for (auto& [l, owner] : g_labelOwner) t += fmt("    {0x%08Xu, %s},\n", l, funcName(owner).c_str());
    t += "};\n";
    t += fmt("extern const size_t vb_rec_table_size = %zu;\n", g_labelOwner.size());
    if (!writeFile("rec_table.cpp", t)) return false;
    return true;
}

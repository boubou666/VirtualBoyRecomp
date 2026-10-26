#include "v810_decoder.h"

#include <cstdio>

namespace v810 {

namespace {

int32_t sext(uint32_t v, int bits) {
    uint32_t m = 1u << (bits - 1);
    v &= (1u << bits) - 1;
    return (int32_t)((v ^ m) - m);
}

const char* const kCondNames[16] = {
    "v", "c", "z", "nh", "n", "r", "lt", "le",
    "nv", "nc", "nz", "h", "p", "nop", "ge", "gt",
};

const char* const kSetfNames[16] = {
    "v", "c", "z", "nh", "n", "t", "lt", "le",
    "nv", "nc", "nz", "h", "p", "f", "ge", "gt",
};

std::string regName(int r) {
    switch (r) {
    case 2: return "fp";
    case 3: return "sp";
    case 4: return "gp";
    case 5: return "tp";
    case 31: return "lp";
    default: return "r" + std::to_string(r);
    }
}

std::string sysRegName(int r) {
    static const char* names[] = {"eipc", "eipsw", "fepc", "fepsw", "ecr", "psw", "pir", "tkcw"};
    if (r < 8) return names[r];
    if (r == 24) return "chcw";
    if (r == 25) return "adtre";
    return "sr" + std::to_string(r);
}

std::string hex(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%X", v);
    return buf;
}

std::string shex(int32_t v) {
    return v < 0 ? "-" + hex((uint32_t)-v) : hex((uint32_t)v);
}

} // namespace

Insn decode(uint32_t addr, uint16_t hw0, uint16_t hw1) {
    Insn in;
    in.addr = addr;
    in.raw = hw0 | ((uint32_t)hw1 << 16);
    const uint32_t opcode = hw0 >> 10;
    in.reg2 = (hw0 >> 5) & 31;
    in.reg1 = hw0 & 31;
    in.len = 2;

    if ((hw0 >> 13) == 0b100) {
        in.op = Op::BCOND;
        in.cond = (hw0 >> 9) & 15;
        in.imm = sext(hw0 & 0x1FF, 9);
        in.reg1 = in.reg2 = 0;
        return in;
    }

    if (opcode < 0x20) {
        static const Op formatI[16] = {
            Op::MOV_R, Op::ADD_R, Op::SUB, Op::CMP_R, Op::SHL_R, Op::SHR_R, Op::JMP, Op::SAR_R,
            Op::MUL, Op::DIV, Op::MULU, Op::DIVU, Op::OR, Op::AND, Op::XOR, Op::NOT,
        };
        if (opcode < 0x10) {
            in.op = formatI[opcode];
            return in;
        }
        const uint32_t imm5 = in.reg1;
        switch (opcode) {
        case 0x10: in.op = Op::MOV_I; in.imm = sext(imm5, 5); break;
        case 0x11: in.op = Op::ADD_I; in.imm = sext(imm5, 5); break;
        case 0x12: in.op = Op::SETF; in.cond = imm5 & 15; break;
        case 0x13: in.op = Op::CMP_I; in.imm = sext(imm5, 5); break;
        case 0x14: in.op = Op::SHL_I; in.imm = imm5; break;
        case 0x15: in.op = Op::SHR_I; in.imm = imm5; break;
        case 0x16: in.op = Op::CLI; break;
        case 0x17: in.op = Op::SAR_I; in.imm = imm5; break;
        case 0x18: in.op = Op::TRAP; in.sub = imm5; break;
        case 0x19: in.op = Op::RETI; break;
        case 0x1A: in.op = Op::HALT; break;
        case 0x1C: in.op = Op::LDSR; in.sub = imm5; break;
        case 0x1D: in.op = Op::STSR; in.sub = imm5; break;
        case 0x1E: in.op = Op::SEI; break;
        case 0x1F:
            in.sub = imm5;
            in.op = ((imm5 & 0x10) || (imm5 >= 4 && imm5 < 8)) ? Op::INVALID : Op::BSTR;
            break;
        default: in.op = Op::INVALID; break;
        }
        return in;
    }

    in.len = 4;
    switch (opcode) {
    case 0x2A:
    case 0x2B:
        in.op = opcode == 0x2A ? Op::JR : Op::JAL;
        in.imm = sext(((uint32_t)(hw0 & 0x3FF) << 16) | hw1, 26);
        in.reg1 = in.reg2 = 0;
        return in;
    case 0x28: in.op = Op::MOVEA; in.imm = (int16_t)hw1; return in;
    case 0x29: in.op = Op::ADDI; in.imm = (int16_t)hw1; return in;
    case 0x2C: in.op = Op::ORI; in.imm = hw1; return in;
    case 0x2D: in.op = Op::ANDI; in.imm = hw1; return in;
    case 0x2E: in.op = Op::XORI; in.imm = hw1; return in;
    case 0x2F: in.op = Op::MOVHI; in.imm = hw1; return in;
    case 0x30: in.op = Op::LD_B; break;
    case 0x31: in.op = Op::LD_H; break;
    case 0x33: in.op = Op::LD_W; break;
    case 0x34: in.op = Op::ST_B; break;
    case 0x35: in.op = Op::ST_H; break;
    case 0x37: in.op = Op::ST_W; break;
    case 0x38: in.op = Op::IN_B; break;
    case 0x39: in.op = Op::IN_H; break;
    case 0x3A: in.op = Op::CAXI; break;
    case 0x3B: in.op = Op::IN_W; break;
    case 0x3C: in.op = Op::OUT_B; break;
    case 0x3D: in.op = Op::OUT_H; break;
    case 0x3F: in.op = Op::OUT_W; break;
    case 0x3E: {
        static const Op formatVII[13] = {
            Op::CMPF_S, Op::INVALID, Op::CVT_WS, Op::CVT_SW, Op::ADDF_S, Op::SUBF_S, Op::MULF_S,
            Op::DIVF_S, Op::XB, Op::XH, Op::REV, Op::TRNC_SW, Op::MPYHW,
        };
        const uint32_t sub = hw1 >> 10;
        in.sub = (uint8_t)sub;
        in.op = sub < 13 ? formatVII[sub] : Op::INVALID;
        return in;
    }
    default:
        in.op = Op::INVALID;
        in.len = 2;
        return in;
    }
    in.imm = (int16_t)hw1; // load/store displacement
    return in;
}

std::string disassemble(const Insn& in) {
    const std::string r1 = regName(in.reg1), r2 = regName(in.reg2);
    auto rr = [&](const char* m) { return std::string(m) + " " + r1 + ", " + r2; };
    auto ir = [&](const char* m) { return std::string(m) + " " + shex(in.imm) + ", " + r2; };
    auto v = [&](const char* m) { return std::string(m) + " " + hex((uint16_t)in.imm) + ", " + r1 + ", " + r2; };
    auto mem = [&](const char* m) { return std::string(m) + " " + shex(in.imm) + "[" + r1 + "], " + r2; };
    auto st = [&](const char* m) { return std::string(m) + " " + r2 + ", " + shex(in.imm) + "[" + r1 + "]"; };

    switch (in.op) {
    case Op::MOV_R: return rr("mov");
    case Op::ADD_R: return rr("add");
    case Op::SUB: return rr("sub");
    case Op::CMP_R: return rr("cmp");
    case Op::SHL_R: return rr("shl");
    case Op::SHR_R: return rr("shr");
    case Op::JMP: return "jmp [" + r1 + "]";
    case Op::SAR_R: return rr("sar");
    case Op::MUL: return rr("mul");
    case Op::DIV: return rr("div");
    case Op::MULU: return rr("mulu");
    case Op::DIVU: return rr("divu");
    case Op::OR: return rr("or");
    case Op::AND: return rr("and");
    case Op::XOR: return rr("xor");
    case Op::NOT: return rr("not");
    case Op::MOV_I: return ir("mov");
    case Op::ADD_I: return ir("add");
    case Op::SETF: return std::string("setf ") + kSetfNames[in.cond] + ", " + r2;
    case Op::CMP_I: return ir("cmp");
    case Op::SHL_I: return ir("shl");
    case Op::SHR_I: return ir("shr");
    case Op::CLI: return "cli";
    case Op::SAR_I: return ir("sar");
    case Op::TRAP: return "trap " + hex(in.sub);
    case Op::RETI: return "reti";
    case Op::HALT: return "halt";
    case Op::LDSR: return "ldsr " + r2 + ", " + sysRegName(in.sub);
    case Op::STSR: return "stsr " + sysRegName(in.sub) + ", " + r2;
    case Op::SEI: return "sei";
    case Op::BSTR: {
        static const char* names[16] = {"sch0bsu", "sch0bsd", "sch1bsu", "sch1bsd", "?", "?", "?", "?",
                                        "orbsu", "andbsu", "xorbsu", "movbsu", "ornbsu", "andnbsu", "xornbsu", "notbsu"};
        return names[in.sub & 15];
    }
    case Op::BCOND:
        if (in.cond == C_F) return "nop";
        return std::string("b") + kCondNames[in.cond] + " " + hex(in.target());
    case Op::JR: return "jr " + hex(in.target());
    case Op::JAL: return "jal " + hex(in.target());
    case Op::MOVEA: return v("movea");
    case Op::ADDI: return v("addi");
    case Op::ORI: return v("ori");
    case Op::ANDI: return v("andi");
    case Op::XORI: return v("xori");
    case Op::MOVHI: return v("movhi");
    case Op::LD_B: return mem("ld.b");
    case Op::LD_H: return mem("ld.h");
    case Op::LD_W: return mem("ld.w");
    case Op::IN_B: return mem("in.b");
    case Op::IN_H: return mem("in.h");
    case Op::IN_W: return mem("in.w");
    case Op::CAXI: return mem("caxi");
    case Op::ST_B: return st("st.b");
    case Op::ST_H: return st("st.h");
    case Op::ST_W: return st("st.w");
    case Op::OUT_B: return st("out.b");
    case Op::OUT_H: return st("out.h");
    case Op::OUT_W: return st("out.w");
    case Op::CMPF_S: return rr("cmpf.s");
    case Op::CVT_WS: return rr("cvt.ws");
    case Op::CVT_SW: return rr("cvt.sw");
    case Op::ADDF_S: return rr("addf.s");
    case Op::SUBF_S: return rr("subf.s");
    case Op::MULF_S: return rr("mulf.s");
    case Op::DIVF_S: return rr("divf.s");
    case Op::XB: return "xb " + r2;
    case Op::XH: return "xh " + r2;
    case Op::REV: return rr("rev");
    case Op::TRNC_SW: return rr("trnc.sw");
    case Op::MPYHW: return rr("mpyhw");
    case Op::INVALID: break;
    }
    return "(invalid " + hex(in.raw & (in.len == 2 ? 0xFFFF : 0xFFFFFFFF)) + ")";
}

uint32_t flagsWritten(const Insn& in) {
    switch (in.op) {
    case Op::ADD_R: case Op::SUB: case Op::CMP_R: case Op::SHL_R: case Op::SHR_R: case Op::SAR_R:
    case Op::ADD_I: case Op::CMP_I: case Op::SHL_I: case Op::SHR_I: case Op::SAR_I: case Op::ADDI:
    case Op::CAXI: case Op::CMPF_S: case Op::CVT_WS: case Op::ADDF_S: case Op::SUBF_S:
    case Op::MULF_S: case Op::DIVF_S:
        return FLAGS_ALL;
    case Op::OR: case Op::AND: case Op::XOR: case Op::NOT: case Op::ORI: case Op::ANDI: case Op::XORI:
    case Op::MUL: case Op::DIV: case Op::MULU: case Op::DIVU: case Op::CVT_SW: case Op::TRNC_SW:
        return FLAG_Z | FLAG_S | FLAG_OV;
    default:
        return 0;
    }
}

uint32_t flagsRead(const Insn& in) {
    switch (in.op) {
    case Op::BCOND:
        return (in.cond == C_T || in.cond == C_F) ? 0 : FLAGS_ALL;
    case Op::SETF: case Op::STSR: case Op::LDSR: case Op::TRAP: case Op::RETI: case Op::HALT:
    case Op::BSTR: case Op::JMP: case Op::JR: case Op::JAL: case Op::INVALID:
    case Op::CLI: case Op::SEI:
        return FLAGS_ALL;
    default:
        return 0;
    }
}

int cycles(const Insn& in) {
    switch (in.op) {
    case Op::BCOND: return in.cond == C_F ? 1 : 3;
    case Op::JMP: case Op::JR: case Op::JAL: return 3;
    case Op::LD_B: case Op::LD_H: case Op::LD_W: case Op::IN_B: case Op::IN_H: case Op::IN_W: return 5;
    case Op::ST_B: case Op::ST_H: case Op::ST_W: case Op::OUT_B: case Op::OUT_H: case Op::OUT_W: return 4;
    case Op::MUL: case Op::MULU: return 13;
    case Op::DIV: return 38;
    case Op::DIVU: return 36;
    case Op::LDSR: case Op::STSR: return 8;
    case Op::CAXI: return 26;
    case Op::RETI: return 10;
    case Op::TRAP: return 15;
    case Op::CMPF_S: return 10;
    case Op::CVT_WS: case Op::CVT_SW: case Op::TRNC_SW: return 16;
    case Op::ADDF_S: case Op::SUBF_S: return 28;
    case Op::MULF_S: return 30;
    case Op::DIVF_S: return 44;
    case Op::MPYHW: return 9;
    case Op::BSTR: return 50;
    default: return 1;
    }
}

bool isBlockTerminator(const Insn& in) {
    switch (in.op) {
    case Op::BCOND: return in.cond != C_F;
    case Op::JMP: case Op::JR: case Op::JAL: case Op::RETI: case Op::TRAP: case Op::HALT:
    case Op::INVALID:
        return true;
    default:
        return false;
    }
}

} // namespace v810

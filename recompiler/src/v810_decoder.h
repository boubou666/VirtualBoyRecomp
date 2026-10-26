// NEC V810 instruction decoder and disassembler.
// The Virtual Boy CPU is a V810 with a few Nintendo-specific extensions
// (XB, XH, REV, MPYHW, TRNC.SW). Instructions are 16 or 32 bits, little endian.
#pragma once

#include <cstdint>
#include <string>

namespace v810 {

enum class Op : uint8_t {
    // Format I (reg1, reg2)
    MOV_R, ADD_R, SUB, CMP_R, SHL_R, SHR_R, JMP, SAR_R,
    MUL, DIV, MULU, DIVU, OR, AND, XOR, NOT,
    // Format II (imm5, reg2)
    MOV_I, ADD_I, SETF, CMP_I, SHL_I, SHR_I, CLI, SAR_I,
    TRAP, RETI, HALT, LDSR, STSR, SEI, BSTR,
    // Format III
    BCOND,
    // Format IV
    JR, JAL,
    // Format V
    MOVEA, ADDI, ORI, ANDI, XORI, MOVHI,
    // Format VI
    LD_B, LD_H, LD_W, ST_B, ST_H, ST_W,
    IN_B, IN_H, IN_W, OUT_B, OUT_H, OUT_W, CAXI,
    // Format VII (floating point + Nintendo extensions)
    CMPF_S, CVT_WS, CVT_SW, ADDF_S, SUBF_S, MULF_S, DIVF_S,
    XB, XH, REV, TRNC_SW, MPYHW,
    INVALID,
};

// Bit string sub-opcodes (imm5 field of opcode 0x1F).
enum BitStringOp : uint8_t {
    SCH0BSU = 0x0, SCH0BSD = 0x1, SCH1BSU = 0x2, SCH1BSD = 0x3,
    ORBSU = 0x8, ANDBSU = 0x9, XORBSU = 0xA, MOVBSU = 0xB,
    ORNBSU = 0xC, ANDNBSU = 0xD, XORNBSU = 0xE, NOTBSU = 0xF,
};

// Condition codes shared by Bcond and SETF.
enum Cond : uint8_t {
    C_V = 0, C_C = 1, C_Z = 2, C_NH = 3, C_N = 4, C_T = 5, C_LT = 6, C_LE = 7,
    C_NV = 8, C_NC = 9, C_NZ = 10, C_H = 11, C_P = 12, C_F = 13, C_GE = 14, C_GT = 15,
};

// PSW flag bits.
constexpr uint32_t FLAG_Z = 1, FLAG_S = 2, FLAG_OV = 4, FLAG_CY = 8, FLAGS_ALL = 0xF;

struct Insn {
    uint32_t addr = 0;
    uint8_t  len = 2;
    Op       op = Op::INVALID;
    uint8_t  reg1 = 0;
    uint8_t  reg2 = 0;
    uint8_t  cond = 0;   // Bcond / SETF condition
    uint8_t  sub = 0;    // bit string sub-op, system register id, trap vector
    int32_t  imm = 0;    // sign- or zero-extended immediate / displacement
    uint32_t raw = 0;

    uint32_t target() const { return addr + (uint32_t)imm; } // for BCOND/JR/JAL
    uint32_t next() const { return addr + len; }
};

// Decodes the instruction at `addr`. `hw0`/`hw1` are the first two halfwords.
Insn decode(uint32_t addr, uint16_t hw0, uint16_t hw1);

std::string disassemble(const Insn& in);

// Flags written / read by an instruction, used for dead flag elimination.
uint32_t flagsWritten(const Insn& in);
uint32_t flagsRead(const Insn& in);

// Approximate execution time in CPU cycles (20 MHz clock).
int cycles(const Insn& in);

bool isBlockTerminator(const Insn& in);

} // namespace v810

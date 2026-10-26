// V810 CPU state shared by the recompiled code and the runtime.
#pragma once

#include <cstddef>
#include <cstdint>

namespace vb {

// PSW bits.
constexpr uint32_t PSW_Z = 1u << 0;
constexpr uint32_t PSW_S = 1u << 1;
constexpr uint32_t PSW_OV = 1u << 2;
constexpr uint32_t PSW_CY = 1u << 3;
constexpr uint32_t PSW_FPR = 1u << 4;
constexpr uint32_t PSW_FUD = 1u << 5;
constexpr uint32_t PSW_FOV = 1u << 6;
constexpr uint32_t PSW_FZD = 1u << 7;
constexpr uint32_t PSW_FIV = 1u << 8;
constexpr uint32_t PSW_FRO = 1u << 9;
constexpr uint32_t PSW_ID = 1u << 12;
constexpr uint32_t PSW_AE = 1u << 13;
constexpr uint32_t PSW_EP = 1u << 14;
constexpr uint32_t PSW_NP = 1u << 15;

constexpr uint32_t CPU_CLOCK = 20000000; // 20 MHz

struct Cpu {
    uint32_t r[32];
    uint32_t pc;     // next address to execute when control returns to the dispatcher
    uint32_t psw;
    uint32_t eipc, eipsw, fepc, fepsw, ecr, chcw, adtre;
    int64_t cycles;     // elapsed CPU cycles
    int64_t next_event; // cycle count at which vb::service() must run
    bool reti_flag;     // set by RETI, used to detect the end of an interrupt handler
};

extern Cpu g_cpu;

using RecFunc = void (*)(Cpu*);
struct RecEntry {
    uint32_t addr;
    RecFunc func;
};

void cpu_reset(Cpu* ctx);
void cpu_run(Cpu* ctx); // never returns normally

// Runs the recompiled code at ctx->pc until it returns to the caller.
void dispatch(Cpu* ctx);

// Called when ctx->cycles >= ctx->next_event with ctx->pc set to a resumable
// address. Advances the hardware and takes pending interrupts. Returns true if
// execution must continue somewhere other than ctx->pc as it was on entry.
bool service(Cpu* ctx);

void trap(Cpu* ctx, uint32_t vector);
void reti(Cpu* ctx);
void halt(Cpu* ctx);
void cli(Cpu* ctx);
void ldsr(Cpu* ctx, int reg, uint32_t value);
uint32_t stsr(Cpu* ctx, int reg);
void bitstring(Cpu* ctx, int op);
[[noreturn]] void illegal(Cpu* ctx, uint32_t addr);

} // namespace vb

// CPU-level runtime: dispatching into recompiled code, interrupts, exceptions,
// system registers and the bit string instructions.

#include "system.h"
#include "vbrt/recomp.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace vb {

Cpu g_cpu;

namespace {

std::vector<RecFunc> g_rom_table; // indexed by (ROM offset / 2)
uint32_t g_rom_base = 0;

void build_table() {
    g_rom_table.assign((g_rom_mask + 1) / 2, nullptr);
    g_rom_base = ~g_rom_mask;
    for (size_t i = 0; i < vb_rec_table_size; i++) {
        const RecEntry& e = vb_rec_table[i];
        g_rom_table[(e.addr & g_rom_mask) >> 1] = e.func;
    }
}

[[noreturn]] void unrecompiled(uint32_t addr) {
    const uint32_t from = g_cpu.r[31];
    if (FILE* f = std::fopen("unrecompiled.txt", "a")) {
        std::fprintf(f, "0x%08X  # reached with lp=0x%08X\n", addr, from);
        std::fclose(f);
    }
    fatal("Execution reached address 0x%08X, which was not recompiled.\n\n"
          "If this is code, add it to the \"entries\" list in the game config and run the "
          "recompiler again. (Address logged to unrecompiled.txt; lp=0x%08X)",
          addr, from);
}

// Runs the interrupt/exception handler at ctx->pc until it returns to `resume`.
void run_handler(Cpu* c, uint32_t resume) {
    for (;;) {
        c->reti_flag = false;
        dispatch(c);
        if (c->reti_flag && c->pc == resume) break;
    }
    c->reti_flag = false;
}

bool take_interrupt(Cpu* c) {
    const int level = hw_pending_irq();
    if (level < 0) return false;
    if (c->psw & (PSW_NP | PSW_EP | PSW_ID)) return false;
    if (level < (int)((c->psw >> 16) & 15)) return false;

    const uint32_t resume = c->pc;
    c->eipc = resume;
    c->eipsw = c->psw;
    c->ecr = (c->ecr & 0xFFFF0000u) | (0xFE00u | ((uint32_t)level << 4));
    c->psw = (c->psw | PSW_EP | PSW_ID) & ~(PSW_AE | 0xF0000u);
    c->psw |= (uint32_t)std::min(level + 1, 15) << 16;
    c->pc = 0xFFFFFE00u | ((uint32_t)level << 4);
    run_handler(c, resume);
    return true;
}

} // namespace

void cpu_reset(Cpu* c) {
    *c = Cpu{};
    c->pc = 0xFFFFFFF0u;
    c->psw = PSW_NP;
    c->ecr = 0x0000FFF0u;
    c->next_event = 0;
    build_table();
}

void dispatch(Cpu* c) {
    const uint32_t pc = c->pc;
    if (((pc >> 24) & 7) == 7) {
        const uint32_t off = pc & g_rom_mask;
        if (RecFunc f = g_rom_table[off >> 1]) {
            c->pc = g_rom_base | off; // canonical mirror used by the generated code
            f(c);
            return;
        }
    }
    unrecompiled(pc);
}

void cpu_run(Cpu* c) {
    for (;;) dispatch(c);
}

bool service(Cpu* c) {
    const uint32_t resume = c->pc;
    c->next_event = hw_update(c->cycles);
    take_interrupt(c);
    return c->pc != resume;
}

void halt(Cpu* c) {
    // Sleep until an interrupt is accepted.
    for (;;) {
        if (c->cycles < c->next_event) c->cycles = c->next_event;
        c->next_event = hw_update(c->cycles);
        if (take_interrupt(c)) return;
    }
}

void reti(Cpu* c) {
    if (c->psw & PSW_NP) {
        c->pc = c->fepc;
        c->psw = c->fepsw;
    } else {
        c->pc = c->eipc;
        c->psw = c->eipsw;
    }
    c->reti_flag = true;
    c->next_event = c->cycles; // re-check pending interrupts
}

void trap(Cpu* c, uint32_t vector) {
    const uint32_t code = 0xFFA0u + (vector & 31);
    if (c->psw & PSW_EP) {
        // Exception while handling an exception: duplexed exception.
        c->fepc = c->pc;
        c->fepsw = c->psw;
        c->ecr = (c->ecr & 0xFFFFu) | (code << 16);
        c->psw |= PSW_NP | PSW_ID;
        c->pc = 0xFFFFFFD0u;
        return;
    }
    c->eipc = c->pc;
    c->eipsw = c->psw;
    c->ecr = (c->ecr & 0xFFFF0000u) | code;
    c->psw = (c->psw | PSW_EP | PSW_ID) & ~PSW_AE;
    c->pc = (vector & 0x10) ? 0xFFFFFFB0u : 0xFFFFFFA0u;
}

void cli(Cpu* c) {
    c->psw &= ~PSW_ID;
    c->next_event = c->cycles;
}

void zero_division(Cpu* c) {
    static bool warned = false;
    if (!warned) {
        log("warning: division by zero (lp=0x%08X); result left unchanged\n", c->r[31]);
        warned = true;
    }
}

void ldsr(Cpu* c, int reg, uint32_t v) {
    switch (reg) {
    case 0: c->eipc = v & ~1u; break;
    case 1: c->eipsw = v & 0x000FF3FFu; break;
    case 2: c->fepc = v & ~1u; break;
    case 3: c->fepsw = v & 0x000FF3FFu; break;
    case 5:
        c->psw = v & 0x000FF3FFu;
        c->next_event = c->cycles;
        break;
    case 24: c->chcw = v & 2u; break; // cache enable; cache dump/restore are not modelled
    case 25: c->adtre = v & ~1u; break;
    default: break; // ECR, PIR, TKCW are read-only
    }
}

uint32_t stsr(Cpu* c, int reg) {
    switch (reg) {
    case 0: return c->eipc;
    case 1: return c->eipsw;
    case 2: return c->fepc;
    case 3: return c->fepsw;
    case 4: return c->ecr;
    case 5: return c->psw;
    case 6: return 0x00005346u; // PIR: V810
    case 7: return 0x000000E0u; // TKCW
    case 24: return c->chcw;
    case 25: return c->adtre;
    default: return 0;
    }
}

void bitstring(Cpu* c, int op) {
    uint32_t* r = c->r;
    if (op < 4) {
        // Search for a 0 (SCH0) or 1 (SCH1) bit, upward (U) or downward (D).
        const bool down = op & 1;
        const uint32_t want = (op >> 1) & 1;
        uint32_t src = r[30] & ~3u, off = r[27] & 31, len = r[28], skipped = r[29];
        bool found = false;
        while (len) {
            if (((mem_read32(src) >> off) & 1) == want) {
                found = true;
                break;
            }
            skipped++;
            len--;
            if (down) {
                if (off-- == 0) { off = 31; src -= 4; }
            } else if (++off == 32) {
                off = 0;
                src += 4;
            }
        }
        c->cycles += 4 * (int64_t)(skipped - r[29] + 1);
        r[30] = src;
        r[27] = off;
        r[28] = len;
        r[29] = skipped;
        c->psw = (c->psw & ~PSW_Z) | (found ? 0 : PSW_Z);
        return;
    }

    uint32_t dst = r[29] & ~3u, src = r[30] & ~3u;
    uint32_t doff = r[26] & 31, soff = r[27] & 31, len = r[28];
    if (len) {
        uint32_t sw = mem_read32(src), dw = mem_read32(dst);
        bool dirty = false;
        while (len) {
            const uint32_t s = (sw >> soff) & 1, d = (dw >> doff) & 1;
            uint32_t v;
            switch (op) {
            case 0x8: v = s | d; break;       // ORBSU
            case 0x9: v = s & d; break;       // ANDBSU
            case 0xA: v = s ^ d; break;       // XORBSU
            case 0xB: v = s; break;           // MOVBSU
            case 0xC: v = (s ^ 1) | d; break; // ORNBSU
            case 0xD: v = (s ^ 1) & d; break; // ANDNBSU
            case 0xE: v = (s ^ 1) ^ d; break; // XORNBSU
            default: v = s ^ 1; break;        // NOTBSU
            }
            dw = (dw & ~(1u << doff)) | (v << doff);
            dirty = true;
            len--;
            c->cycles += 1;
            if (++soff == 32) {
                soff = 0;
                src += 4;
                if (len) sw = mem_read32(src);
            }
            if (++doff == 32) {
                mem_write32(dst, dw);
                dirty = false;
                doff = 0;
                dst += 4;
                if (len) dw = mem_read32(dst);
            }
        }
        if (dirty) mem_write32(dst, dw);
    }
    r[26] = doff;
    r[27] = soff;
    r[28] = 0;
    r[29] = dst;
    r[30] = src;
}

void illegal(Cpu* c, uint32_t addr) {
    (void)c;
    fatal("Illegal or undecodable instruction at 0x%08X.", addr);
}

} // namespace vb

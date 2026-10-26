// Everything a generated rec_*.cpp file needs.
#pragma once

#include "cpu.h"
#include "memory.h"
#include "ops.h"

#include <cstddef>
#include <cstdint>

// Start of a basic block: account for its cycles, and let the runtime advance
// the hardware / take interrupts when an event is due.
#define VB_BLOCK(addr, cyc)                                    \
    do {                                                       \
        ctx->cycles += (cyc);                                  \
        if (ctx->cycles >= ctx->next_event) {                  \
            ctx->pc = (addr);                                  \
            if (vb::service(ctx)) return;                      \
        }                                                      \
    } while (0)

// Provided by the generated rec_table.cpp.
extern const uint32_t vb_rec_rom_crc32;
extern const uint32_t vb_rec_rom_size;
extern const char vb_rec_game_title[];
extern const vb::RecEntry vb_rec_table[];
extern const size_t vb_rec_table_size;

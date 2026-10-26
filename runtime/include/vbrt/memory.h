// Virtual Boy memory map. Only the low 27 address bits are decoded:
//   0x00000000 VIP (video)          0x04000000 cartridge expansion
//   0x01000000 VSU (sound)          0x05000000 work RAM (64 KiB)
//   0x02000000 hardware control     0x06000000 cartridge RAM
//   0x03000000 unmapped             0x07000000 cartridge ROM
// Work RAM, ROM and VRAM are accessed inline; everything else goes through the
// slow path in memory.cpp.
#pragma once

#include <cstdint>
#include <cstring>

namespace vb {

extern uint8_t* g_rom;
extern uint32_t g_rom_mask;
extern uint8_t g_wram[0x10000];
extern uint8_t g_vram[0x40000];

uint8_t io_read8(uint32_t a);
uint16_t io_read16(uint32_t a);
uint32_t io_read32(uint32_t a);
void io_write8(uint32_t a, uint8_t v);
void io_write16(uint32_t a, uint16_t v);
void io_write32(uint32_t a, uint32_t v);

inline uint16_t load16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t load32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
inline void store16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
inline void store32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

inline bool is_vram(uint32_t a) { return (a & 0x07000000u) == 0 && (a & 0x7FFFFu) < 0x40000u; }

inline uint8_t mem_read8(uint32_t a) {
    switch ((a >> 24) & 7) {
    case 5: return g_wram[a & 0xFFFF];
    case 7: return g_rom[a & g_rom_mask];
    case 0: if (is_vram(a)) return g_vram[a & 0x3FFFF]; [[fallthrough]];
    default: return io_read8(a);
    }
}

inline uint16_t mem_read16(uint32_t a) {
    a &= ~1u;
    switch ((a >> 24) & 7) {
    case 5: return load16(g_wram + (a & 0xFFFF));
    case 7: return load16(g_rom + (a & g_rom_mask));
    case 0: if (is_vram(a)) return load16(g_vram + (a & 0x3FFFF)); [[fallthrough]];
    default: return io_read16(a);
    }
}

inline uint32_t mem_read32(uint32_t a) {
    a &= ~3u;
    switch ((a >> 24) & 7) {
    case 5: return load32(g_wram + (a & 0xFFFF));
    case 7: return load32(g_rom + (a & g_rom_mask));
    case 0: if (is_vram(a)) return load32(g_vram + (a & 0x3FFFF)); [[fallthrough]];
    default: return io_read32(a);
    }
}

inline void mem_write8(uint32_t a, uint8_t v) {
    switch ((a >> 24) & 7) {
    case 5: g_wram[a & 0xFFFF] = v; return;
    case 7: return;
    case 0: if (is_vram(a)) { g_vram[a & 0x3FFFF] = v; return; } [[fallthrough]];
    default: io_write8(a, v); return;
    }
}

inline void mem_write16(uint32_t a, uint16_t v) {
    a &= ~1u;
    switch ((a >> 24) & 7) {
    case 5: store16(g_wram + (a & 0xFFFF), v); return;
    case 7: return;
    case 0: if (is_vram(a)) { store16(g_vram + (a & 0x3FFFF), v); return; } [[fallthrough]];
    default: io_write16(a, v); return;
    }
}

inline void mem_write32(uint32_t a, uint32_t v) {
    a &= ~3u;
    switch ((a >> 24) & 7) {
    case 5: store32(g_wram + (a & 0xFFFF), v); return;
    case 7: return;
    case 0: if (is_vram(a)) { store32(g_vram + (a & 0x3FFFF), v); return; } [[fallthrough]];
    default: io_write32(a, v); return;
    }
}

} // namespace vb

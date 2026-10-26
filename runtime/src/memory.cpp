// Slow-path memory accesses: VIP registers, CHR mirror, VSU, hardware control,
// cartridge RAM and unmapped regions.

#include "system.h"
#include "vbrt/cpu.h"
#include "vbrt/memory.h"

namespace vb {

uint8_t* g_rom = nullptr;
uint32_t g_rom_mask = 0;
uint8_t g_wram[0x10000];
uint8_t g_vram[0x40000];
uint8_t g_sram[0x10000];

namespace {

// VIP space is 512 KiB, mirrored: 0x00000-0x3FFFF VRAM, 0x40000-0x5FFFF
// registers, 0x78000-0x7FFFF linear window onto the four CHR tables.
uint16_t vip_bus_read16(uint32_t a) {
    a &= 0x7FFFE;
    if (a < 0x40000) return load16(g_vram + a);
    if (a < 0x60000) return vip_read16(a);
    if (a >= 0x78000) return load16(vip_chr_mirror(a));
    return 0;
}

void vip_bus_write16(uint32_t a, uint16_t v) {
    a &= 0x7FFFE;
    if (a < 0x40000) store16(g_vram + a, v);
    else if (a < 0x60000) vip_write16(a, v);
    else if (a >= 0x78000) store16(vip_chr_mirror(a), v);
}

} // namespace

uint8_t io_read8(uint32_t a) {
    switch ((a >> 24) & 7) {
    case 0: {
        const uint32_t o = a & 0x7FFFF;
        if (o >= 0x78000) return *vip_chr_mirror(o);
        const uint16_t h = vip_bus_read16(a);
        return (uint8_t)((a & 1) ? h >> 8 : h);
    }
    case 2: return hwctl_read(a, g_cpu.cycles);
    case 6: return g_sram[a & 0xFFFF];
    default: return 0;
    }
}

uint16_t io_read16(uint32_t a) {
    switch ((a >> 24) & 7) {
    case 0: return vip_bus_read16(a);
    case 2: return hwctl_read(a, g_cpu.cycles);
    case 6: return load16(g_sram + (a & 0xFFFE));
    default: return 0;
    }
}

uint32_t io_read32(uint32_t a) {
    switch ((a >> 24) & 7) {
    case 0: return vip_bus_read16(a) | ((uint32_t)vip_bus_read16(a + 2) << 16);
    case 2: return hwctl_read(a, g_cpu.cycles);
    case 6: return load32(g_sram + (a & 0xFFFC));
    default: return 0;
    }
}

void io_write8(uint32_t a, uint8_t v) {
    switch ((a >> 24) & 7) {
    case 0: {
        const uint32_t o = a & 0x7FFFF;
        if (o >= 0x78000) *vip_chr_mirror(o) = v;
        else if (o >= 0x40000 && o < 0x60000) vip_write16(o & ~1u, v); // registers are 16-bit
        break;
    }
    case 1: vsu_write(a, v, g_cpu.cycles); break;
    case 2: hwctl_write(a, v, g_cpu.cycles); break;
    case 6: g_sram[a & 0xFFFF] = v; break;
    default: break;
    }
}

void io_write16(uint32_t a, uint16_t v) {
    switch ((a >> 24) & 7) {
    case 0: vip_bus_write16(a, v); break;
    case 1: vsu_write(a, (uint8_t)v, g_cpu.cycles); break;
    case 2: hwctl_write(a, (uint8_t)v, g_cpu.cycles); break;
    case 6: store16(g_sram + (a & 0xFFFE), v); break;
    default: break;
    }
}

void io_write32(uint32_t a, uint32_t v) {
    switch ((a >> 24) & 7) {
    case 0:
        vip_bus_write16(a, (uint16_t)v);
        vip_bus_write16(a + 2, (uint16_t)(v >> 16));
        break;
    case 1: vsu_write(a, (uint8_t)v, g_cpu.cycles); break;
    case 2: hwctl_write(a, (uint8_t)v, g_cpu.cycles); break;
    case 6: store32(g_sram + (a & 0xFFFC), v); break;
    default: break;
    }
}

} // namespace vb

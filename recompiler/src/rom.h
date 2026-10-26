// Virtual Boy cartridge ROM image.
// The ROM is mirrored throughout 0x07000000-0x07FFFFFF; games execute it from the
// top mirror, so the canonical address of ROM offset `o` is (0x100000000 - size) + o.
#pragma once

#include "v810_decoder.h"

#include <cstdint>
#include <string>
#include <vector>

struct Rom {
    std::vector<uint8_t> data;
    uint32_t mask = 0;  // size - 1
    uint32_t base = 0;  // canonical address of offset 0

    bool load(const std::string& path, std::string& error);

    // True if `addr` is inside the canonical ROM window.
    bool contains(uint32_t addr) const { return addr >= base; }
    uint32_t offset(uint32_t addr) const { return addr & mask; }

    uint16_t read16(uint32_t addr) const {
        uint32_t o = addr & mask & ~1u;
        return (uint16_t)(data[o] | (data[o + 1] << 8));
    }
    uint32_t read32(uint32_t addr) const {
        return read16(addr & ~3u) | ((uint32_t)read16((addr & ~3u) + 2) << 16);
    }
    v810::Insn decodeAt(uint32_t addr) const {
        return v810::decode(addr, read16(addr), read16(addr + 2));
    }

    std::string title() const;
    std::string gameCode() const;
    uint32_t crc32() const;
};

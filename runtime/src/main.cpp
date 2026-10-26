// Entry point of a recompiled Virtual Boy game.
//
//   <game> [path/to/rom.vb]
//
// The recompiled code contains the game's logic; the ROM is still needed at run
// time for its data (graphics, tables, music).

#include "system.h"
#include "vbrt/recomp.h"

#include <SDL.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifndef VB_DEFAULT_ROM
#define VB_DEFAULT_ROM ""
#endif

namespace {

uint32_t crc32(const std::vector<uint8_t>& data) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint8_t b : data) {
        crc ^= b;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> candidates;
    if (argc > 1) candidates.push_back(argv[1]);
    else {
        if (*VB_DEFAULT_ROM) candidates.push_back(VB_DEFAULT_ROM);
        candidates.push_back("rom.vb");
    }

    std::vector<uint8_t> rom;
    std::string romPath;
    for (const std::string& c : candidates) {
        if (read_file(c, rom)) {
            romPath = c;
            break;
        }
    }
    if (romPath.empty()) {
        std::fprintf(stderr, "usage: %s <rom.vb>\n(could not open %s)\n", argc > 0 ? argv[0] : "game",
                     candidates.front().c_str());
        return 1;
    }
    if (rom.size() != vb_rec_rom_size || crc32(rom) != vb_rec_rom_crc32) {
        std::fprintf(stderr,
                     "error: %s does not match the ROM this build was recompiled from\n"
                     "       (expected %u bytes, CRC32 %08X; got %zu bytes, CRC32 %08X)\n",
                     romPath.c_str(), vb_rec_rom_size, vb_rec_rom_crc32, rom.size(), crc32(rom));
        return 1;
    }

    vb::g_rom = rom.data();
    vb::g_rom_mask = (uint32_t)rom.size() - 1;

    const std::string savePath = romPath + ".sav";
    {
        std::vector<uint8_t> sav;
        if (read_file(savePath, sav)) std::copy_n(sav.begin(), std::min(sav.size(), sizeof vb::g_sram), vb::g_sram);
    }

    if (!vb::host_init(std::string(vb_rec_game_title) + " (recompiled)")) return 1;

    vb::hw_reset();
    vb::cpu_reset(&vb::g_cpu);
    try {
        vb::cpu_run(&vb::g_cpu);
    } catch (const vb::QuitRequest&) {
    }

    bool used = false;
    for (uint8_t b : vb::g_sram) used |= b != 0;
    if (used) {
        std::ofstream f(savePath, std::ios::binary);
        f.write(reinterpret_cast<const char*>(vb::g_sram), sizeof vb::g_sram);
    }
    vb::host_shutdown();
    return 0;
}

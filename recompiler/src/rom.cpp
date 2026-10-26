#include "rom.h"

#include <fstream>
#include <iterator>

bool Rom::load(const std::string& path, std::string& error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "cannot open " + path;
        return false;
    }
    data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    const size_t size = data.size();
    if (size < 1024 || (size & (size - 1)) != 0 || size > (16u << 20)) {
        error = "ROM size must be a power of two between 1 KiB and 16 MiB";
        return false;
    }
    mask = (uint32_t)size - 1;
    base = (uint32_t)(0x100000000ull - size);
    return true;
}

std::string Rom::title() const {
    // Header lives 0x220 bytes before the end of the ROM (0xFFFFFDE0).
    const size_t h = data.size() - 0x220;
    std::string t;
    for (size_t i = 0; i < 20; i++) {
        char c = (char)data[h + i];
        t += (c >= 0x20 && c < 0x7F && c != '"' && c != '\\') ? c : '?';
    }
    while (!t.empty() && t.back() == ' ') t.pop_back();
    return t;
}

std::string Rom::gameCode() const {
    const size_t h = data.size() - 0x220;
    return std::string(reinterpret_cast<const char*>(&data[h + 0x1B]), 4);
}

uint32_t Rom::crc32() const {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint8_t b : data) {
        crc ^= b;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

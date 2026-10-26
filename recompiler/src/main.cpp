// vbrecomp: static recompiler for Virtual Boy (NEC V810) ROMs.
//
//   vbrecomp <game.toml> [--rom <file.vb>] [--out <dir>] [--stats]
//   vbrecomp --disasm <file.vb> <hex address> [count]

#include "analysis.h"
#include "codegen.h"
#include "config.h"
#include "rom.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

static int usage() {
    std::fprintf(stderr,
                 "usage:\n"
                 "  vbrecomp <game.toml> [--rom <file.vb>] [--out <dir>] [--stats]\n"
                 "  vbrecomp --disasm <file.vb> <hex address> [count]\n");
    return 2;
}

static int disasm(int argc, char** argv) {
    if (argc < 4) return usage();
    Rom rom;
    std::string err;
    if (!rom.load(argv[2], err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    uint32_t addr = (uint32_t)std::strtoul(argv[3], nullptr, 16);
    int count = argc > 4 ? std::atoi(argv[4]) : 32;
    for (int i = 0; i < count && rom.contains(addr); i++) {
        v810::Insn in = rom.decodeAt(addr);
        std::printf("%08X: %s\n", addr, v810::disassemble(in).c_str());
        addr = in.next();
    }
    return 0;
}

static std::string resolve(const Config& cfg, const std::string& p) {
    if (p.empty() || fs::path(p).is_absolute()) return p;
    return (fs::path(cfg.baseDir) / p).string();
}

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    if (std::string(argv[1]) == "--disasm") return disasm(argc, argv);

    Config cfg;
    std::string err;
    if (!cfg.load(argv[1], err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    std::string romPath = resolve(cfg, cfg.getString("rom"));
    std::string outDir = resolve(cfg, cfg.getString("output", "generated"));
    bool stats = false;
    int chunks = (int)cfg.getInt("chunks", 16);
    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--rom" && i + 1 < argc) romPath = argv[++i];
        else if (a == "--out" && i + 1 < argc) outDir = argv[++i];
        else if (a == "--chunks" && i + 1 < argc) chunks = std::atoi(argv[++i]);
        else if (a == "--stats") stats = true;
        else return usage();
    }

    Rom rom;
    if (!rom.load(romPath, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    std::printf("ROM: \"%s\" [%s] %zu KiB, CRC32 %08X\n", rom.title().c_str(), rom.gameCode().c_str(),
                rom.data.size() / 1024, rom.crc32());

    const int64_t expectedCrc = cfg.getInt("crc32", -1);
    if (expectedCrc >= 0 && (uint32_t)expectedCrc != rom.crc32()) {
        std::fprintf(stderr, "error: ROM CRC32 %08X does not match the config (%08X)\n", rom.crc32(),
                     (uint32_t)expectedCrc);
        return 1;
    }

    AnalysisOptions aopts;
    for (int64_t e : cfg.getIntArray("entries")) aopts.extraEntries.push_back((uint32_t)e);
    aopts.scanCodePointers = cfg.getBool("scan_code_pointers", true);

    Analyzer analyzer(rom, aopts);
    analyzer.run();
    analyzer.printStats();
    if (stats) return 0;

    CodeGenOptions copts;
    copts.chunks = chunks < 1 ? 1 : chunks;
    copts.comments = cfg.getBool("comments", true);
    CodeGen gen(rom, analyzer, copts);
    if (!gen.write(outDir, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    std::printf("wrote %d source files to %s\n", copts.chunks + 2, outDir.c_str());
    return 0;
}

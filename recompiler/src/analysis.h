// Control-flow analysis: discovers functions and basic blocks in the ROM.
//
// A "function" here is everything reachable from an entry point through
// non-call control flow (branches, JR, fall-through, and JMP [reg] whose target
// is a known constant). JAL targets become new functions. Code that is reachable
// from several entry points is simply emitted into each of them, which is safe
// because all CPU state lives in the shared V810 context.
#pragma once

#include "rom.h"
#include "v810_decoder.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

struct Block {
    uint32_t start = 0;
    std::vector<v810::Insn> insns;
    uint32_t end() const { return insns.back().next(); }
};

struct Function {
    uint32_t entry = 0;
    std::map<uint32_t, v810::Insn> insns;
    std::set<uint32_t> labels;               // addresses that start a block
    std::map<uint32_t, uint32_t> constJumps; // JMP [reg] address -> resolved target
    std::map<uint32_t, uint32_t> indirectCalls; // JMP [reg] address -> return address (r31 set just before)
    std::map<uint32_t, std::vector<uint32_t>> jumpTables; // JMP [reg] address -> switch targets
    std::set<uint32_t> calls;                // JAL targets
    std::set<uint32_t> externalTargets;      // branch targets outside the ROM
    std::set<uint32_t> exits;                // jumps into code owned by another function
    std::vector<Block> blocks;
    bool hasInvalid = false;

    bool containsLabel(uint32_t a) const { return labels.count(a) != 0; }
};

struct AnalysisOptions {
    std::vector<uint32_t> extraEntries;
    bool scanCodePointers = true; // treat ROM words that look like code pointers as entries
    size_t maxFunctionInsns = 200000;
};

class Analyzer {
public:
    Analyzer(const Rom& rom, AnalysisOptions opts) : rom_(rom), opts_(std::move(opts)) {}

    void run();

    const std::map<uint32_t, Function>& functions() const { return functions_; }
    void printStats() const;

private:
    Function analyzeFunction(uint32_t entry, bool speculative = false);
    std::vector<uint32_t> resolveJumpTable(const std::vector<v810::Insn>& trace, const Function& fn,
                                           const v810::Insn& jmp) const;
    void buildBlocks(Function& fn);
    Function* ownerOf(uint32_t addr);
    bool isPlausibleEntry(uint32_t addr);
    bool looksLikeCode(uint32_t addr) const;
    std::vector<uint32_t> scanCodePointers() const;

    const Rom& rom_;
    AnalysisOptions opts_;
    std::map<uint32_t, Function> functions_;
};

// C++ code generator: turns analyzed V810 functions into C++ source files that
// link against the vbrt runtime.
#pragma once

#include "analysis.h"
#include "rom.h"

#include <string>

struct CodeGenOptions {
    int chunks = 16;          // number of rec_N.cpp files (fixed so build systems know the outputs)
    bool comments = true;     // emit disassembly as comments
    bool deadFlags = true;    // skip PSW updates that are overwritten before being read
};

class CodeGen {
public:
    CodeGen(const Rom& rom, const Analyzer& analyzer, CodeGenOptions opts)
        : rom_(rom), an_(analyzer), opts_(opts) {}

    bool write(const std::string& outDir, std::string& error);

private:
    std::string emitFunction(const Function& fn) const;
    void emitInsn(std::string& out, const Function& fn, const v810::Insn& in, bool flagsLive) const;
    void emitTerminator(std::string& out, const Function& fn, const Block& b, const v810::Insn& in) const;
    std::string jumpTo(const Function& fn, uint32_t target) const;
    std::string callTo(uint32_t target) const;

    const Rom& rom_;
    const Analyzer& an_;
    CodeGenOptions opts_;
};

#pragma once
// A small PowerPC assembler built on the disassembler's own opcode/operand tables (ppc-dis.c),
// enough for Xenia's instruction tests: one instruction per call, gas operand rules
// (optional operands, PPC_OPERAND_NEXT, d(rA)), registers, numbers, cr bit names and labels.
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

struct powerpc_opcode;

namespace rxt
{
    // Resolves a symbol (label) to an absolute guest address. Returns false if unknown.
    using LabelLookup = std::function<bool(const std::string&, int64_t&)>;

    class Assembler
    {
    public:
        Assembler();

        // Assembles one instruction (mnemonic plus the operand text after it) at pc.
        bool Assemble(const std::string& mnemonic, const std::string& operands, uint32_t pc,
            const LabelLookup& labels, uint32_t& word, std::string& error) const;

        // Evaluates an expression: numbers, registers (r/f/v/cr), cr bit names, labels, + - * ( ).
        static bool Evaluate(const std::string& text, const LabelLookup& labels, int64_t& value,
            char& regClass, bool& usedLabel, std::string& error);

        int dialect() const { return dialect_; }

    private:
        struct Token
        {
            std::string text;
            bool parenOuter = false; // the d in d(rA)
            bool parenInner = false; // the rA in d(rA)
        };

        bool Expand(const std::string& mnemonic, const std::vector<std::string>& groups,
            std::string& outMnemonic, std::vector<std::string>& outGroups, std::string& error) const;

        bool TryOpcode(const powerpc_opcode* op, const std::vector<Token>& tokens, size_t groupCount,
            uint32_t pc, const LabelLookup& labels, uint32_t& word, std::string& error, bool& clean) const;

        std::unordered_map<std::string, std::vector<const powerpc_opcode*>> byName_;
        int dialect_ = 0;
    };

    // Splits operand text on top-level commas.
    std::vector<std::string> SplitOperands(const std::string& text);
    std::string Trim(const std::string& s);
}

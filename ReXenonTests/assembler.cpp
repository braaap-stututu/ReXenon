#include "assembler.h"
#include "ppc_tables.h"

#include <dis-asm.h>
#include <cctype>
#include <cstring>

namespace rxt
{
    std::string Trim(const std::string& s)
    {
        size_t b = 0, e = s.size();
        while (b < e && std::isspace((unsigned char)s[b])) ++b;
        while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
        return s.substr(b, e - b);
    }

    std::vector<std::string> SplitOperands(const std::string& text)
    {
        std::vector<std::string> out;
        std::string t = Trim(text);
        if (t.empty())
            return out;
        int depth = 0;
        std::string cur;
        for (char c : t)
        {
            if (c == '(') depth++;
            if (c == ')') depth--;
            if (c == ',' && depth == 0)
            {
                out.push_back(Trim(cur));
                cur.clear();
                continue;
            }
            cur += c;
        }
        out.push_back(Trim(cur));
        return out;
    }

    namespace
    {
        struct Parser
        {
            const std::string& s;
            size_t i = 0;
            const LabelLookup& labels;
            bool usedLabel = false;
            int primaries = 0;
            char lastRegClass = 0;
            std::string error;

            void Skip() { while (i < s.size() && std::isspace((unsigned char)s[i])) ++i; }

            bool Expr(int64_t& v)
            {
                if (!Term(v)) return false;
                for (;;)
                {
                    Skip();
                    if (i < s.size() && (s[i] == '+' || s[i] == '-'))
                    {
                        char op = s[i++];
                        int64_t r;
                        if (!Term(r)) return false;
                        v = op == '+' ? v + r : v - r;
                    }
                    else if (i < s.size() && s[i] == '|')
                    {
                        ++i;
                        int64_t r;
                        if (!Term(r)) return false;
                        v |= r;
                    }
                    else if (i < s.size() && s[i] == '&')
                    {
                        ++i;
                        int64_t r;
                        if (!Term(r)) return false;
                        v &= r;
                    }
                    else return true;
                }
            }

            bool Term(int64_t& v)
            {
                if (!Unary(v)) return false;
                for (;;)
                {
                    Skip();
                    if (i < s.size() && (s[i] == '*' || s[i] == '/'))
                    {
                        char op = s[i++];
                        int64_t r;
                        if (!Unary(r)) return false;
                        if (op == '/' && r == 0) { error = "division by zero"; return false; }
                        v = op == '*' ? v * r : v / r;
                    }
                    else if (i + 1 < s.size() && (s.compare(i, 2, "<<") == 0 || s.compare(i, 2, ">>") == 0))
                    {
                        bool left = s[i] == '<';
                        i += 2;
                        int64_t r;
                        if (!Unary(r)) return false;
                        v = left ? int64_t(uint64_t(v) << r) : (v >> r);
                    }
                    else return true;
                }
            }

            bool Unary(int64_t& v)
            {
                Skip();
                if (i < s.size() && s[i] == '-') { ++i; if (!Unary(v)) return false; v = -v; return true; }
                if (i < s.size() && s[i] == '+') { ++i; return Unary(v); }
                if (i < s.size() && s[i] == '~') { ++i; if (!Unary(v)) return false; v = ~v; return true; }
                return Primary(v);
            }

            bool Primary(int64_t& v)
            {
                Skip();
                if (i >= s.size()) { error = "expected a value"; return false; }
                char c = s[i];
                if (c == '(')
                {
                    ++i;
                    if (!Expr(v)) return false;
                    Skip();
                    if (i >= s.size() || s[i] != ')') { error = "missing )"; return false; }
                    ++i;
                    primaries += 2; // a parenthesised expression is never a bare register
                    return true;
                }
                if (std::isdigit((unsigned char)c))
                {
                    size_t start = i;
                    uint64_t value = 0;
                    if (c == '0' && i + 1 < s.size() && (s[i + 1] == 'x' || s[i + 1] == 'X'))
                    {
                        i += 2;
                        size_t digits = i;
                        while (i < s.size() && std::isxdigit((unsigned char)s[i]))
                        {
                            char d = s[i++];
                            value = value * 16 + (std::isdigit((unsigned char)d) ? d - '0' : (std::tolower(d) - 'a' + 10));
                        }
                        if (i == digits) { error = "bad hex number"; return false; }
                    }
                    else
                    {
                        while (i < s.size() && std::isdigit((unsigned char)s[i]))
                            value = value * 10 + (s[i++] - '0');
                    }
                    if (i < s.size() && (std::isalpha((unsigned char)s[i]) || s[i] == '_'))
                    {
                        error = "bad number '" + s.substr(start) + "'";
                        return false;
                    }
                    v = int64_t(value);
                    primaries++;
                    lastRegClass = 0;
                    return true;
                }
                if (std::isalpha((unsigned char)c) || c == '_' || c == '.')
                {
                    size_t start = i;
                    while (i < s.size() && (std::isalnum((unsigned char)s[i]) || s[i] == '_' || s[i] == '.'))
                        ++i;
                    std::string id = s.substr(start, i - start);
                    primaries++;
                    lastRegClass = 0;
                    auto regNumber = [&](const char* prefix, int max, char cls) -> bool
                        {
                            size_t n = strlen(prefix);
                            if (id.size() <= n || id.compare(0, n, prefix) != 0) return false;
                            for (size_t k = n; k < id.size(); k++)
                                if (!std::isdigit((unsigned char)id[k])) return false;
                            int num = std::atoi(id.c_str() + n);
                            if (num > max) return false;
                            v = num;
                            lastRegClass = cls;
                            return true;
                        };
                    if (regNumber("cr", 7, 'c') || regNumber("r", 31, 'r') || regNumber("f", 31, 'f') || regNumber("v", 127, 'v'))
                        return true;
                    static const struct { const char* name; int value; } crBits[] = {
                        { "lt", 0 }, { "gt", 1 }, { "eq", 2 }, { "so", 3 }, { "un", 3 } };
                    for (auto& b : crBits)
                        if (id == b.name) { v = b.value; return true; }
                    if (labels && labels(id, v)) { usedLabel = true; return true; }
                    error = "unknown symbol '" + id + "'";
                    return false;
                }
                error = std::string("unexpected '") + c + "'";
                return false;
            }
        };

    }

    bool Assembler::Evaluate(const std::string& text, const LabelLookup& labels, int64_t& value,
        char& regClass, bool& usedLabel, std::string& error)
    {
        Parser p{ text, 0, labels };
        if (!p.Expr(value)) { error = p.error; return false; }
        p.Skip();
        if (p.i != text.size()) { error = "junk after expression: '" + text.substr(p.i) + "'"; return false; }
        regClass = p.primaries == 1 ? p.lastRegClass : 0;
        usedLabel = p.usedLabel;
        return true;
    }

    Assembler::Assembler()
    {
        dialect_ = rxt_dialect();
        int count = 0;
        const powerpc_opcode* ops = rxt_opcodes(&count);
        for (int i = 0; i < count; i++)
        {
            if ((ops[i].flags & dialect_) == 0)
                continue;
            byName_[ops[i].name].push_back(&ops[i]);
        }
    }

    // gas's PowerPC macros (powerpc_macros in ppc-dis.c), written out.
    bool Assembler::Expand(const std::string& mnemonic, const std::vector<std::string>& g,
        std::string& outMnemonic, std::vector<std::string>& outGroups, std::string& error) const
    {
        std::string base = mnemonic;
        bool dot = !base.empty() && base.back() == '.';
        if (dot) base.pop_back();

        struct Macro { const char* name; int args; };
        static const Macro macros[] = {
            { "extldi", 4 }, { "extrdi", 4 }, { "insrdi", 4 }, { "rotrdi", 3 }, { "sldi", 3 }, { "srdi", 3 },
            { "clrrdi", 3 }, { "clrlsldi", 4 }, { "extlwi", 4 }, { "extrwi", 4 }, { "inslwi", 4 }, { "insrwi", 4 },
            { "rotrwi", 3 }, { "slwi", 3 }, { "srwi", 3 }, { "clrrwi", 3 }, { "clrlslwi", 4 } };
        const Macro* m = nullptr;
        for (auto& x : macros)
            if (base == x.name) m = &x;
        if (!m)
            return false;
        if ((int)g.size() != m->args)
        {
            error = "macro " + mnemonic + " takes " + std::to_string(m->args) + " operands";
            return true;
        }
        int64_t a = 0, b = 0;
        char rc; bool ul;
        if (!Evaluate(g[2], nullptr, a, rc, ul, error)) return true;
        if (m->args == 4 && !Evaluate(g[3], nullptr, b, rc, ul, error)) return true;
        auto n = [](int64_t v) { return std::to_string(v); };
        std::string ins;
        std::vector<std::string> o = { g[0], g[1] };
        // a = first numeric operand (n), b = second (b, or n for the clrls forms)
        if (base == "extldi") { ins = "rldicr"; o.push_back(n(b)); o.push_back(n(a - 1)); }
        else if (base == "extrdi") { ins = "rldicl"; o.push_back(n(b + a)); o.push_back(n(64 - a)); }
        else if (base == "insrdi") { ins = "rldimi"; o.push_back(n(64 - (b + a))); o.push_back(n(b)); }
        else if (base == "rotrdi") { ins = "rldicl"; o.push_back(n((64 - a) & 63)); o.push_back("0"); }
        else if (base == "sldi") { ins = "rldicr"; o.push_back(n(a)); o.push_back(n(63 - a)); }
        else if (base == "srdi") { ins = "rldicl"; o.push_back(n((64 - a) & 63)); o.push_back(n(a)); }
        else if (base == "clrrdi") { ins = "rldicr"; o.push_back("0"); o.push_back(n(63 - a)); }
        else if (base == "clrlsldi") { ins = "rldic"; o.push_back(n(b)); o.push_back(n(a - b)); }
        else if (base == "extlwi") { ins = "rlwinm"; o.push_back(n(b)); o.push_back("0"); o.push_back(n(a - 1)); }
        else if (base == "extrwi") { ins = "rlwinm"; o.push_back(n((b + a) & 31)); o.push_back(n(32 - a)); o.push_back("31"); }
        else if (base == "inslwi") { ins = "rlwimi"; o.push_back(n((32 - b) & 31)); o.push_back(n(b)); o.push_back(n(b + a - 1)); }
        else if (base == "insrwi") { ins = "rlwimi"; o.push_back(n((32 - (b + a)) & 31)); o.push_back(n(b)); o.push_back(n(b + a - 1)); }
        else if (base == "rotrwi") { ins = "rlwinm"; o.push_back(n((32 - a) & 31)); o.push_back("0"); o.push_back("31"); }
        else if (base == "slwi") { ins = "rlwinm"; o.push_back(n(a)); o.push_back("0"); o.push_back(n(31 - a)); }
        else if (base == "srwi") { ins = "rlwinm"; o.push_back(n((32 - a) & 31)); o.push_back(n(a)); o.push_back("31"); }
        else if (base == "clrrwi") { ins = "rlwinm"; o.push_back("0"); o.push_back("0"); o.push_back(n(31 - a)); }
        else if (base == "clrlslwi") { ins = "rlwinm"; o.push_back(n(b)); o.push_back(n(a - b)); o.push_back(n(31 - b)); }
        outMnemonic = ins + (dot ? "." : "");
        outGroups = o;
        return true;
    }

    bool Assembler::TryOpcode(const powerpc_opcode* op, const std::vector<Token>& tokens, size_t groupCount,
        uint32_t pc, const LabelLookup& labels, uint32_t& word, std::string& error, bool& clean) const
    {
        clean = true;
        std::string warn;

        bool skipOptional = false;
        for (int k = 0; op->operands[k]; k++)
        {
            rxt_operand o;
            rxt_operand_info(op->operands[k], &o);
            if (o.flags & RXT_OPERAND_OPTIONAL)
            {
                size_t expected = 0;
                for (int j = 0; op->operands[j]; j++)
                {
                    rxt_operand q;
                    rxt_operand_info(op->operands[j], &q);
                    if (!(q.flags & RXT_OPERAND_FAKE)) expected++;
                }
                if (groupCount < expected) skipOptional = true;
                break;
            }
        }

        uint32_t insn = (uint32_t)op->opcode;
        size_t ti = 0;
        int nextIndex = 0;
        for (int k = 0; op->operands[k]; k++)
        {
            int index = nextIndex ? nextIndex : op->operands[k];
            nextIndex = 0;
            rxt_operand o;
            rxt_operand_info(index, &o);
            const char* errmsg = nullptr;

            if (o.flags & RXT_OPERAND_FAKE)
            {
                insn = rxt_insert(index, insn, 0, &errmsg);
                if (errmsg) { clean = false; warn = errmsg; }
                continue;
            }
            if ((o.flags & RXT_OPERAND_OPTIONAL) && skipOptional)
            {
                if (o.has_insert)
                    insn = rxt_insert(index, insn, 0, &errmsg);
                if (o.flags & RXT_OPERAND_NEXT)
                    nextIndex = op->operands[k] + 1;
                continue;
            }
            if (ti >= tokens.size())
            {
                error = "too few operands for " + std::string(op->name);
                return false;
            }
            const Token& tok = tokens[ti++];
            bool wantParenOuter = (o.flags & RXT_OPERAND_PARENS) != 0;
            if (wantParenOuter != tok.parenOuter)
            {
                error = "operand form doesn't match " + std::string(op->name) + " (d(rA) expected " + (wantParenOuter ? "here)" : "elsewhere)");
                return false;
            }
            int64_t value;
            char regClass;
            bool usedLabel;
            if (!Evaluate(tok.text, labels, value, regClass, usedLabel, error))
                return false;

            bool isGpr = (o.flags & (RXT_OPERAND_GPR | RXT_OPERAND_GPR_0)) != 0;
            bool isFpr = (o.flags & RXT_OPERAND_FPR) != 0;
            bool isVr = (o.flags & RXT_OPERAND_VR) != 0;
            bool isCr = (o.flags & RXT_OPERAND_CR) != 0;
            if ((regClass == 'r' && !isGpr) || (regClass == 'f' && !isFpr) || (regClass == 'v' && !isVr) || (regClass == 'c' && !isCr))
            {
                error = "register '" + tok.text + "' doesn't fit operand " + std::to_string(k + 1) + " of " + op->name;
                return false;
            }
            if ((isGpr || isFpr || isVr) && regClass == 0 && usedLabel)
            {
                error = "label used as a register";
                return false;
            }
            // a CR field operand (bitm 7) written crN is the field number; a CR bit operand takes 4*crN+bit
            if (o.flags & RXT_OPERAND_RELATIVE)
                value -= int64_t(pc);

            if (!o.has_insert)
            {
                int64_t bitm = o.bitm;
                int64_t right = bitm & -bitm;
                int64_t min = 0, max = bitm;
                if (o.flags & RXT_OPERAND_SIGNED)
                {
                    max = (bitm >> 1) & -right;
                    min = -(max + right);
                    if (o.flags & RXT_OPERAND_SIGNOPT)
                        max = bitm; // the full unsigned range is accepted too
                }
                if (o.flags & RXT_OPERAND_PLUS1) max++;
                if (o.flags & RXT_OPERAND_NEGATIVE) { int64_t t = min; min = -max; max = -t; }
                if ((value & (right - 1)) != 0 || value < min || value > max)
                {
                    clean = false;
                    warn = "operand " + std::to_string(k + 1) + " (" + tok.text + ") out of range";
                }
            }
            insn = rxt_insert(index, insn, (long)value, &errmsg);
            if (errmsg) { clean = false; warn = errmsg; }
        }
        if (ti != tokens.size())
        {
            error = "too many operands for " + std::string(op->name);
            return false;
        }
        word = insn;
        if (!clean) error = warn;
        return true;
    }

    bool Assembler::Assemble(const std::string& mnemonicIn, const std::string& operands, uint32_t pc,
        const LabelLookup& labels, uint32_t& word, std::string& error) const
    {
        std::string mnemonic = mnemonicIn;
        std::vector<std::string> groups = SplitOperands(operands);

        // Xenon spellings that the binutils table names differently or writes with an extra operand.
        if (mnemonic == "dcbz128")
            mnemonic = "dcbzl"; // dcbz with the L bit: Xenia's dcbz128 (0x7C2007EC)
        if ((mnemonic == "vupkhsb128" || mnemonic == "vupklsb128") && groups.size() == 2)
            groups.push_back("v0"); // the VA field is 0 for the byte forms (0x60 selects the halfword ones)

        // "." is the current address; xer/lr/ctr are SPR numbers (mfspr r3, lr)
        LabelLookup outer = labels;
        LabelLookup lookupWithPc = [&](const std::string& name, int64_t& v) -> bool
            {
                if (name == ".") { v = pc; return true; }
                if (outer && outer(name, v)) return true;
                if (name == "xer") { v = 1; return true; }
                if (name == "lr") { v = 8; return true; }
                if (name == "ctr") { v = 9; return true; }
                return false;
            };

        if (byName_.find(mnemonic) == byName_.end())
        {
            std::string m2;
            std::vector<std::string> g2;
            std::string err;
            if (Expand(mnemonic, groups, m2, g2, err))
            {
                if (!err.empty()) { error = err; return false; }
                mnemonic = m2;
                groups = g2;
            }
        }

        auto it = byName_.find(mnemonic);
        if (it == byName_.end())
        {
            error = "unknown mnemonic '" + mnemonic + "'";
            return false;
        }

        std::vector<Token> tokens;
        for (auto& g : groups)
        {
            // d(rA): the last top-level parenthesised suffix
            size_t close = g.size();
            if (!g.empty() && g.back() == ')')
            {
                int depth = 0;
                size_t open = std::string::npos;
                for (size_t k = g.size(); k-- > 0;)
                {
                    if (g[k] == ')') depth++;
                    else if (g[k] == '(') { if (--depth == 0) { open = k; break; } }
                }
                if (open != std::string::npos && open > 0)
                {
                    std::string outer = Trim(g.substr(0, open));
                    std::string inner = Trim(g.substr(open + 1, close - open - 2));
                    // "(x)" alone, or an expression like "(a)+(b)": only a register-ish inner part is d(rA)
                    char rc; bool ul; int64_t v; std::string e;
                    bool innerIsReg = Evaluate(inner, nullptr, v, rc, ul, e) && (rc == 'r' || (rc == 0 && inner.find_first_not_of("0123456789") == std::string::npos));
                    char last = outer.empty() ? 0 : outer.back();
                    bool outerEndsWithOperator = last == '+' || last == '-' || last == '*' || last == '/' || last == '(' || last == '&' || last == '|';
                    if (innerIsReg && !outerEndsWithOperator)
                    {
                        tokens.push_back({ outer, true, false });
                        tokens.push_back({ inner, false, true });
                        continue;
                    }
                }
            }
            tokens.push_back({ g, false, false });
        }

        std::string firstError, uncleanError;
        bool haveUnclean = false;
        uint32_t uncleanWord = 0;
        for (const powerpc_opcode* op : it->second)
        {
            uint32_t w;
            std::string err;
            bool clean;
            if (TryOpcode(op, tokens, groups.size(), pc, lookupWithPc, w, err, clean))
            {
                if (clean)
                {
                    word = w;
                    return true;
                }
                if (!haveUnclean)
                {
                    haveUnclean = true;
                    uncleanWord = w;
                    uncleanError = err;
                }
            }
            else if (firstError.empty())
                firstError = err;
        }
        if (haveUnclean)
        {
            word = uncleanWord;
            error = "warning: " + uncleanError;
            return true;
        }
        error = firstError;
        return false;
    }
}

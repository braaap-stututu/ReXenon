// ReXenonTests driver: assembles Xenia's PPC instruction tests, recompiles each test function
// with ReXenon's emitter (Recompiler::Recompile, default config) and writes, per suite:
//   gen/<suite>.cpp  the recompiled functions plus a table (rxt_functions)
//   gen/<suite>.vec  the tests: registers/memory in, expected registers/memory out
//   gen/<suite>.map  line ranges of each function in the .cpp (to drop one that fails to compile)
// plus manifest.tsv (every test and its static status), encoder.txt and problems.txt.
//
// Usage: ReXenonTestDriver <xenia testing dir> <out dir> [suite name filter]
#include "pch.h"
#include "recompiler.h"
#include "assembler.h"

#include <cinttypes>
#include <set>

using namespace rxt;

namespace
{
    constexpr uint32_t kBase = 0x82010000;

    struct Line
    {
        int number = 0;
        std::string mnemonic;    // empty for a .long
        std::string operands;
        std::string longExpr;    // .long operand
        uint32_t address = 0;
        uint32_t word = 0;
        bool ok = false;
        bool warned = false;
        std::string error;
    };

    struct Annotation
    {
        std::string key, value;
    };

    struct TestCase
    {
        std::string label;       // test_foo
        std::string name;        // foo (Xenia's name, used by skip.txt)
        uint32_t start = 0, end = 0;
        size_t firstLine = 0, lastLine = 0; // [first, last) into lines
        std::vector<Annotation> annotations;
    };

    struct Macro
    {
        std::vector<std::string> params;
        std::vector<std::string> body;
    };

    std::string StripComment(const std::string& s)
    {
        size_t hash = s.find('#');
        return hash == std::string::npos ? s : s.substr(0, hash);
    }

    struct Suite
    {
        std::string name;
        std::vector<Line> lines;
        std::vector<TestCase> tests;
        std::unordered_map<std::string, uint32_t> labels;
        std::vector<std::string> parseErrors;
    };

    void ParseLine(Suite& suite, const std::unordered_map<std::string, Macro>& macros, std::string text, int number, uint32_t& pc, int depth);

    bool ParseFile(const std::filesystem::path& path, Suite& suite)
    {
        std::ifstream in(path);
        if (!in) return false;
        std::unordered_map<std::string, Macro> macros;
        std::string raw;
        int number = 0;
        uint32_t pc = kBase;
        Macro* defining = nullptr;
        while (std::getline(in, raw))
        {
            ++number;
            if (!raw.empty() && raw.back() == '\r') raw.pop_back();
            std::string t = Trim(raw);

            if (defining)
            {
                if (Trim(StripComment(t)) == ".endm") { defining = nullptr; continue; }
                defining->body.push_back(t);
                continue;
            }

            if (t.rfind("#_", 0) == 0)
            {
                // annotation: "#_ KEY value"
                std::string rest = Trim(t.substr(2));
                size_t sp = rest.find(' ');
                if (sp != std::string::npos && !suite.tests.empty())
                    suite.tests.back().annotations.push_back({ rest.substr(0, sp), Trim(rest.substr(sp + 1)) });
                continue;
            }
            t = Trim(StripComment(t));
            if (t.empty()) continue;

            if (t.rfind(".macro", 0) == 0)
            {
                std::string decl = Trim(t.substr(6));
                size_t sp = decl.find_first_of(" \t");
                std::string mname = decl.substr(0, sp);
                Macro m;
                if (sp != std::string::npos)
                    for (auto& p : SplitOperands(decl.substr(sp + 1)))
                        m.params.push_back(p);
                defining = &macros[mname];
                *defining = m;
                continue;
            }
            ParseLine(suite, macros, t, number, pc, 0);
        }
        // close the last test
        if (!suite.tests.empty())
        {
            suite.tests.back().end = pc;
            suite.tests.back().lastLine = suite.lines.size();
        }
        return true;
    }

    void ParseLine(Suite& suite, const std::unordered_map<std::string, Macro>& macros, std::string t, int number, uint32_t& pc, int depth)
    {
        // labels (possibly several, possibly followed by an instruction)
        for (;;)
        {
            size_t colon = t.find(':');
            if (colon == std::string::npos) break;
            std::string label = Trim(t.substr(0, colon));
            bool ident = !label.empty() && label.find_first_of(" \t,()") == std::string::npos;
            if (!ident) break;
            if (label.rfind("test_", 0) == 0)
            {
                if (!suite.tests.empty())
                {
                    suite.tests.back().end = pc;
                    suite.tests.back().lastLine = suite.lines.size();
                }
                TestCase tc;
                tc.label = label;
                tc.name = label.substr(5);
                tc.start = pc;
                tc.firstLine = suite.lines.size();
                suite.tests.push_back(tc);
            }
            if (suite.labels.count(label))
                suite.parseErrors.push_back(fmt::format("line {}: duplicate label {}", number, label));
            suite.labels[label] = pc;
            t = Trim(t.substr(colon + 1));
        }
        if (t.empty()) return;

        size_t sp = t.find_first_of(" \t");
        std::string mnemonic = t.substr(0, sp);
        std::string operands = sp == std::string::npos ? "" : Trim(t.substr(sp + 1));

        if (mnemonic[0] == '.')
        {
            if (mnemonic == ".long" || mnemonic == ".int" || mnemonic == ".word")
            {
                for (auto& e : SplitOperands(operands))
                {
                    Line l;
                    l.number = number;
                    l.longExpr = e;
                    l.address = pc;
                    pc += 4;
                    suite.lines.push_back(l);
                }
            }
            else if (mnemonic == ".globl" || mnemonic == ".global" || mnemonic == ".text" || mnemonic == ".section" || mnemonic == ".align" || mnemonic == ".p2align")
            {
                // nothing to place (alignment: everything is 4-byte already)
            }
            else
                suite.parseErrors.push_back(fmt::format("line {}: unsupported directive {}", number, mnemonic));
            return;
        }

        auto m = macros.find(mnemonic);
        if (m != macros.end() && depth < 8)
        {
            auto args = SplitOperands(operands);
            // longest parameter names first so \ab isn't replaced as \a + b
            std::vector<size_t> order(m->second.params.size());
            for (size_t i = 0; i < order.size(); i++) order[i] = i;
            std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return m->second.params[a].size() > m->second.params[b].size(); });
            for (auto bodyLine : m->second.body)
            {
                for (size_t i : order)
                {
                    std::string key = "\\" + m->second.params[i];
                    std::string value = i < args.size() ? args[i] : "";
                    for (size_t pos; (pos = bodyLine.find(key)) != std::string::npos;)
                        bodyLine.replace(pos, key.size(), value);
                }
                bodyLine = Trim(StripComment(bodyLine));
                if (!bodyLine.empty())
                    ParseLine(suite, macros, bodyLine, number, pc, depth + 1);
            }
            return;
        }

        Line l;
        l.number = number;
        l.mnemonic = mnemonic;
        l.operands = operands;
        l.address = pc;
        pc += 4;
        suite.lines.push_back(l);
    }

    // ------------------------------------------------------------------ value parsing (Xenia's rules)

    bool ParseU64(std::string s, uint64_t& v)
    {
        s = Trim(s);
        while (!s.empty() && (s.back() == 'u' || s.back() == 'U' || s.back() == 'l' || s.back() == 'L'))
            s.pop_back(); // 0x...ull
        bool neg = false;
        if (!s.empty() && s[0] == '-') { neg = true; s = s.substr(1); }
        if (s.empty()) return false;
        char* end = nullptr;
        if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
            v = strtoull(s.c_str() + 2, &end, 16);
        else
            v = strtoull(s.c_str(), &end, 10);
        if (end == nullptr || *end != 0) return false;
        if (neg) v = uint64_t(-int64_t(v));
        return true;
    }

    bool ParseDoubleBits(std::string s, uint64_t& bits, bool& isHex)
    {
        s = Trim(s);
        isHex = s.find("0x") != std::string::npos || s.find("0X") != std::string::npos;
        if (isHex) return ParseU64(s, bits);
        char* end = nullptr;
        double d = strtod(s.c_str(), &end);
        if (end == nullptr || *end != 0) return false;
        memcpy(&bits, &d, 8);
        return true;
    }

    bool ParseVec(std::string s, uint32_t w[4])
    {
        size_t close = s.find(']');
        if (close != std::string::npos) s = s.substr(0, close + 1); // "[...]  # comment"
        s = Trim(s);
        if (s.size() < 2 || s.front() != '[' || s.back() != ']') return false;
        auto parts = SplitOperands(s.substr(1, s.size() - 2));
        if (parts.size() != 4) return false;
        for (int i = 0; i < 4; i++)
        {
            std::string p = Trim(parts[i]);
            if (p.size() > 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p = p.substr(2);
            char* end = nullptr;
            w[i] = uint32_t(strtoul(p.c_str(), &end, 16));
            if (end == nullptr || *end != 0) return false;
        }
        return true;
    }

    // Xenia's ParseMemoryBytes: after spaces, every two characters are read as hex (strtoul),
    // so "[AB, CD]" reads as 00 0B CD 00 ...; the expected values were written against that.
    std::string MemoryHex(const std::string& text)
    {
        std::string out;
        size_t i = 0;
        while (i < text.size())
        {
            while (i < text.size() && text[i] == ' ') ++i;
            if (i >= text.size()) break;
            std::string pair = text.substr(i, 2);
            out += fmt::format("{:02X}", uint8_t(strtoul(pair.c_str(), nullptr, 16)));
            i += 2;
        }
        return out;
    }

    bool RegisterLine(char dir, const std::string& value, std::string& out, std::string& error)
    {
        size_t sp = value.find(' ');
        if (sp == std::string::npos) { error = "no value"; return false; }
        std::string reg = value.substr(0, sp);
        std::string v = Trim(value.substr(sp + 1));
        if (size_t hash = v.find('#'); hash != std::string::npos && v[0] != '[')
            v = Trim(v.substr(0, hash));
        if (reg == "cr" || reg == "xer")
        {
            uint64_t x;
            if (!ParseU64(v, x)) { error = "bad value " + v; return false; }
            out = fmt::format("{} {} {:X}", dir, reg, x);
            return true;
        }
        if (reg.size() < 2 || !std::isdigit((unsigned char)reg[1])) { error = "unknown register " + reg; return false; }
        int n = std::atoi(reg.c_str() + 1);
        switch (reg[0])
        {
        case 'r':
        {
            if (n > 31) break;
            uint64_t x;
            if (!ParseU64(v, x)) { error = "bad value " + v; return false; }
            out = fmt::format("{} {} {:X}", dir, reg, x);
            return true;
        }
        case 'f':
        {
            if (n > 31) break;
            uint64_t bits;
            bool isHex;
            if (!ParseDoubleBits(v, bits, isHex)) { error = "bad value " + v; return false; }
            if (dir == 'O')
                out = fmt::format("O {} {} {:X}", reg, isHex ? 'b' : 'd', bits);
            else
                out = fmt::format("I {} {:X}", reg, bits);
            return true;
        }
        case 'v':
        {
            if (n > 127) break;
            uint32_t w[4];
            if (!ParseVec(v, w)) { error = "bad vector " + v; return false; }
            out = fmt::format("{} {} {:X} {:X} {:X} {:X}", dir, reg, w[0], w[1], w[2], w[3]);
            return true;
        }
        }
        error = "unknown register " + reg;
        return false;
    }

    std::string Sanitize(const std::string& s)
    {
        std::string o;
        for (char c : s) o += (c == '\t' || c == '\n' || c == '\r') ? ' ' : c;
        return o;
    }

    std::string Mnemonics(const Suite& suite, const TestCase& tc)
    {
        std::set<std::string> m;
        for (size_t i = tc.firstLine; i < tc.lastLine; i++)
            m.insert(suite.lines[i].mnemonic.empty() ? ".long" : suite.lines[i].mnemonic);
        std::string s;
        for (auto& x : m) { if (!s.empty()) s += ","; s += x; }
        return s;
    }
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        fmt::println("usage: ReXenonTestDriver <xenia testing dir> <out dir> [suite filter]");
        return 2;
    }
    std::filesystem::path testDir = argv[1];
    std::filesystem::path outDir = argv[2];
    std::string filter = argc > 3 ? argv[3] : "";
    std::filesystem::create_directories(outDir / "gen");

    std::unordered_set<std::string> skip;
    {
        std::ifstream s(testDir / "skip.txt");
        std::string line;
        while (std::getline(s, line))
        {
            line = Trim(line);
            if (!line.empty() && line[0] != '#') skip.insert(line);
        }
    }

    std::vector<std::filesystem::path> files;
    for (auto& e : std::filesystem::directory_iterator(testDir))
    {
        auto fn = e.path().filename().string();
        if (e.path().extension() == ".s" && fn.rfind("instr_", 0) == 0 && (filter.empty() || fn.find(filter) != std::string::npos))
            files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());

    Assembler assembler;

    FILE* manifest = fopen((outDir / "manifest.tsv").string().c_str(), "w");
    FILE* encoderLog = fopen((outDir / "encoder.txt").string().c_str(), "w");
    FILE* problemsLog = fopen((outDir / "problems.txt").string().c_str(), "w");
    FILE* suitesList = fopen((outDir / "suites.txt").string().c_str(), "w");

    // encoder statistics, over every instruction line (each line once, as written in the file)
    size_t linesTotal = 0, linesAssembled = 0, linesWarned = 0, linesDecoded = 0, linesStrict = 0, linesLoose = 0, longsTotal = 0;
    std::set<std::string> uniqueTotal, uniqueStrictOrLoose;
    std::vector<std::string> roundTripFailures;
    std::set<std::string> aliasSeen;
    std::vector<std::string> aliasLines;
    size_t testsTotal = 0, testsSkipped = 0, testsEncFailed = 0, testsRunnable = 0, functionsTotal = 0;

    for (auto& path : files)
    {
        Suite suite;
        suite.name = path.stem().string();
        if (!ParseFile(path, suite))
        {
            fmt::println("can't read {}", path.string());
            continue;
        }
        for (auto& e : suite.parseErrors)
            fmt::println(encoderLog, "{}: {}", suite.name, e);

        auto lookup = [&](const std::string& name, int64_t& v) -> bool
            {
                auto it = suite.labels.find(name);
                if (it == suite.labels.end()) return false;
                v = it->second;
                return true;
            };

        // assemble
        for (auto& l : suite.lines)
        {
            if (!l.mnemonic.empty())
            {
                ++linesTotal;
                std::string err;
                l.ok = assembler.Assemble(l.mnemonic, l.operands, l.address, lookup, l.word, err);
                if (l.ok && !err.empty()) { l.warned = true; ++linesWarned; fmt::println(encoderLog, "{}:{}: {} {}: {}", suite.name, l.number, l.mnemonic, l.operands, err); }
                if (!l.ok) { l.error = err; fmt::println(encoderLog, "{}:{}: ENCODE FAILED {} {}: {}", suite.name, l.number, l.mnemonic, l.operands, err); }
                else ++linesAssembled;
            }
            else
            {
                ++longsTotal;
                int64_t v;
                char rc;
                bool ul;
                std::string err;
                l.ok = Assembler::Evaluate(l.longExpr, lookup, v, rc, ul, err);
                l.word = uint32_t(v);
                if (!l.ok) { l.error = err; fmt::println(encoderLog, "{}:{}: .long {}: {}", suite.name, l.number, l.longExpr, err); }
            }
        }

        // round trip through XenonUtils' disassembler
        for (auto& l : suite.lines)
        {
            if (l.mnemonic.empty() || !l.ok) continue;
            std::string key = l.mnemonic + " " + l.operands;
            uniqueTotal.insert(key);
            uint32_t be = ByteSwap(l.word);
            ppc_insn insn;
            ppc::Disassemble(&be, 4, l.address, insn);
            if (insn.opcode == nullptr)
            {
                roundTripFailures.push_back(fmt::format("{}:{}: {} {} -> {:08X} doesn't decode", suite.name, l.number, l.mnemonic, l.operands, l.word));
                continue;
            }
            ++linesDecoded;

            // strict: same mnemonic and the same operand values
            bool strict = false;
            if (insn.opcode->name == l.mnemonic)
            {
                auto flatten = [&](const std::string& ops, std::vector<int64_t>& out) -> bool
                    {
                        for (auto& g : SplitOperands(ops))
                        {
                            std::string a = g, b;
                            if (!g.empty() && g.back() == ')')
                            {
                                size_t open = g.rfind('(');
                                if (open != std::string::npos && open > 0) { a = g.substr(0, open); b = g.substr(open + 1, g.size() - open - 2); }
                            }
                            for (auto* part : { &a, &b })
                            {
                                if (part->empty()) continue;
                                int64_t v; char rc; bool ul; std::string e;
                                if (!Assembler::Evaluate(*part, lookup, v, rc, ul, e)) return false;
                                out.push_back(v);
                            }
                        }
                        return true;
                    };
                std::vector<int64_t> a, b;
                strict = flatten(l.operands, a) && flatten(insn.op_str, b) && a == b;
            }
            // loose: assembling the disassembler's text gives the same word back
            bool loose = false;
            if (!strict)
            {
                uint32_t w2;
                std::string err;
                loose = assembler.Assemble(insn.opcode->name, insn.op_str, l.address, lookup, w2, err) && w2 == l.word;
            }
            if (strict) ++linesStrict;
            else if (loose)
            {
                ++linesLoose;
                if (aliasSeen.insert(key).second)
                    aliasLines.push_back(fmt::format("{}:{}: {} {} -> {:08X} -> {} {}", suite.name, l.number, l.mnemonic, l.operands, l.word, insn.opcode->name, insn.op_str));
            }
            if (strict || loose)
                uniqueStrictOrLoose.insert(key);
            else
                roundTripFailures.push_back(fmt::format("{}:{}: {} {} -> {:08X} -> {} {}", suite.name, l.number, l.mnemonic, l.operands, l.word, insn.opcode->name, insn.op_str));
        }

        // code image: the whole file, as Xenia assembles it
        uint32_t codeEnd = kBase;
        for (auto& l : suite.lines) codeEnd = std::max(codeEnd, l.address + 4);
        std::vector<uint8_t> code(codeEnd - kBase + 16, 0);
        for (auto& l : suite.lines)
        {
            uint32_t be = ByteSwap(l.ok ? l.word : 0u);
            memcpy(&code[l.address - kBase], &be, 4);
        }

        Recompiler recompiler;
        // Recompiler options for the run (run.py --options): comma-separated config names.
        if (const char* options = getenv("REXENON_TEST_OPTIONS"))
        {
            const std::string list = std::string(",") + options + ",";
            recompiler.config.exactEstimates = list.find(",exact_estimates,") != std::string::npos;
            recompiler.config.fusedMadd = list.find(",fused_madd,") != std::string::npos;
        }
        recompiler.image.base = 0;
        recompiler.image.size = uint32_t(code.size());
        recompiler.image.Map(".text", kBase, uint32_t(code.size()), SectionFlags_Code, code.data());

        // functions: one per distinct test body; a body that branches outside itself or uses
        // .long keeps its own copy (its meaning depends on where it is)
        std::map<std::vector<uint32_t>, int> bodies;
        std::vector<std::pair<uint32_t, uint32_t>> functions; // base, size
        std::vector<int> testFunction(suite.tests.size(), -1);
        std::vector<std::string> testStatus(suite.tests.size());
        for (size_t ti = 0; ti < suite.tests.size(); ti++)
        {
            auto& tc = suite.tests[ti];
            ++testsTotal;
            if (skip.count(tc.name)) { testStatus[ti] = "SKIP\t"; ++testsSkipped; continue; }
            std::string encError;
            bool positionDependent = false;
            std::vector<uint32_t> words;
            for (size_t i = tc.firstLine; i < tc.lastLine; i++)
            {
                auto& l = suite.lines[i];
                if (!l.ok && encError.empty())
                    encError = fmt::format("line {}: {} {}: {}", l.number, l.mnemonic.empty() ? ".long" : l.mnemonic, l.mnemonic.empty() ? l.longExpr : l.operands, l.error);
                if (l.mnemonic.empty()) positionDependent = true;
                words.push_back(l.word);
                uint32_t w = l.word;
                uint32_t op = w >> 26;
                if (op == 18 || op == 16)
                {
                    int64_t disp = op == 18 ? PPC_BI(w) : PPC_BD(w);
                    int64_t target = (w & 2) ? disp : int64_t(l.address) + disp;
                    if (target < tc.start || target >= tc.end) positionDependent = true;
                }
            }
            if (!encError.empty()) { testStatus[ti] = "ENC\t" + Sanitize(encError); ++testsEncFailed; continue; }
            if (tc.end <= tc.start) { testStatus[ti] = "ENC\tempty test"; ++testsEncFailed; continue; }
            int fnIndex;
            auto it = positionDependent ? bodies.end() : bodies.find(words);
            if (it != bodies.end())
                fnIndex = it->second;
            else
            {
                fnIndex = int(functions.size());
                functions.push_back({ tc.start, tc.end - tc.start });
                if (!positionDependent) bodies.emplace(words, fnIndex);
            }
            testFunction[ti] = fnIndex;
            testStatus[ti] = "RUN\t";
        }

        for (size_t i = 0; i < functions.size(); i++)
            recompiler.image.symbols.emplace(fmt::format("f{}", i), functions[i].first, functions[i].second, Symbol_Function);

        std::string cpp;
        cpp += "// Generated by ReXenonTestDriver from " + path.filename().string() + "\n";
        cpp += "#include \"rxt_runtime.h\"\n\n";
        auto lineCount = [](const std::string& s) { return (size_t)std::count(s.begin(), s.end(), '\n'); };
        std::string map;
        std::vector<std::string> fnProblems(functions.size());
        for (size_t i = 0; i < functions.size(); i++)
        {
            Function fn(functions[i].first, functions[i].second);
            recompiler.out.clear();
            size_t problemsBefore = recompiler.problems.size();
            recompiler.Recompile(fn);
            for (size_t p = problemsBefore; p < recompiler.problems.size(); p++)
            {
                fmt::println(problemsLog, "{} f{}: {}", suite.name, i, recompiler.problems[p]);
                if (!fnProblems[i].empty()) fnProblems[i] += "; ";
                fnProblems[i] += recompiler.problems[p];
            }
            cpp += fmt::format("#ifndef RXT_SKIP_F{}\n", i);
            size_t startLine = lineCount(cpp) + 1;
            cpp += recompiler.out;
            size_t endLine = lineCount(cpp);
            cpp += "#endif\n\n";
            map += fmt::format("{} {} {}\n", i, startLine, endLine);
        }
        functionsTotal += functions.size();

        cpp += "extern \"C\" const RxtEntry rxt_functions[] = {\n";
        for (size_t i = 0; i < functions.size(); i++)
            cpp += fmt::format("#ifndef RXT_SKIP_F{0}\n\t{{ \"f{0}\", f{0} }},\n#else\n\t{{ \"f{0}\", nullptr }},\n#endif\n", i);
        cpp += "\t{ nullptr, nullptr }\n};\n";
        cpp += fmt::format("extern \"C\" const int rxt_function_count = {};\n", functions.size());

        std::string vec;
        for (size_t ti = 0; ti < suite.tests.size(); ti++)
        {
            auto& tc = suite.tests[ti];
            std::string status = testStatus[ti];
            if (testFunction[ti] >= 0)
            {
                std::string body = fmt::format("T {} {}\n", tc.name, testFunction[ti]);
                std::string annError;
                for (auto& a : tc.annotations)
                {
                    std::string line, err;
                    if (a.key == "REGISTER_IN" || a.key == "REGISTER_OUT")
                    {
                        if (RegisterLine(a.key == "REGISTER_IN" ? 'I' : 'O', a.value, line, err)) body += line + "\n";
                        else if (annError.empty()) annError = a.key + " " + a.value + ": " + err;
                    }
                    else if (a.key == "MEMORY_IN" || a.key == "MEMORY_OUT")
                    {
                        size_t sp = a.value.find(' ');
                        std::string addr = a.value.substr(0, sp);
                        std::string bytes = sp == std::string::npos ? "" : a.value.substr(sp + 1);
                        uint32_t address = uint32_t(strtoul(addr.c_str(), nullptr, 16));
                        body += fmt::format("{} {:X} {}\n", a.key == "MEMORY_IN" ? 'M' : 'X', address, MemoryHex(bytes));
                    }
                }
                if (!annError.empty())
                {
                    status = "ENC\tannotation " + Sanitize(annError);
                    ++testsEncFailed;
                }
                else
                {
                    vec += body;
                    ++testsRunnable;
                    status = fmt::format("RUN\tf{}\t{}", testFunction[ti], Sanitize(fnProblems[testFunction[ti]]));
                }
            }
            fmt::println(manifest, "{}\t{}\t{}\t{}", suite.name, tc.name, Mnemonics(suite, tc), status);
        }

        auto write = [&](const std::string& ext, const std::string& text)
            {
                auto p = outDir / "gen" / (suite.name + ext);
                // leave an identical file alone so its object isn't rebuilt
                std::ifstream old(p, std::ios::binary);
                if (old)
                {
                    std::stringstream ss;
                    ss << old.rdbuf();
                    if (ss.str() == text) return;
                }
                old.close();
                std::ofstream o(p, std::ios::binary);
                o << text;
            };
        write(".cpp", cpp);
        write(".vec", vec);
        write(".map", map);
        fmt::println(suitesList, "{}\t{}\t{}", suite.name, suite.tests.size(), functions.size());
    }

    fmt::println(encoderLog, "");
    fmt::println(encoderLog, "Round-trip failures ({}):", roundTripFailures.size());
    for (auto& r : roundTripFailures) fmt::println(encoderLog, "  {}", r);
    fmt::println(encoderLog, "");
    fmt::println(encoderLog, "Round trips via an alias spelling (distinct lines, {}):", aliasLines.size());
    for (auto& r : aliasLines) fmt::println(encoderLog, "  {}", r);

    FILE* stats = fopen((outDir / "encoder_stats.txt").string().c_str(), "w");
    fmt::println(stats, "instruction_lines {}", linesTotal);
    fmt::println(stats, "assembled {}", linesAssembled);
    fmt::println(stats, "assembled_with_warning {}", linesWarned);
    fmt::println(stats, "decoded {}", linesDecoded);
    fmt::println(stats, "roundtrip_strict {}", linesStrict);
    fmt::println(stats, "roundtrip_alias {}", linesLoose);
    fmt::println(stats, "unique_lines {}", uniqueTotal.size());
    fmt::println(stats, "unique_roundtrip {}", uniqueStrictOrLoose.size());
    fmt::println(stats, "long_words {}", longsTotal);
    fmt::println(stats, "tests {}", testsTotal);
    fmt::println(stats, "tests_skipped {}", testsSkipped);
    fmt::println(stats, "tests_encoder_failed {}", testsEncFailed);
    fmt::println(stats, "tests_runnable {}", testsRunnable);
    fmt::println(stats, "functions {}", functionsTotal);
    fclose(stats);
    fclose(manifest);
    fclose(encoderLog);
    fclose(problemsLog);
    fclose(suitesList);

    fmt::println("{} instruction lines, {} assembled ({} with warnings), {} decode, {} round-trip exactly, {} via alias",
        linesTotal, linesAssembled, linesWarned, linesDecoded, linesStrict, linesLoose);
    fmt::println("{} tests: {} skipped, {} encoder-failed, {} runnable, {} distinct functions",
        testsTotal, testsSkipped, testsEncFailed, testsRunnable, functionsTotal);
    return 0;
}

#include "pch.h"
#include "recompiler.h"
#include <algorithm>
#include <cstring>
#include <disasm.h>

// non_volatile_as_local keeps r14-r31, f14-f31, v14-v31 and v64-v127 in C++ locals. That is only
// right for functions that follow the ABI: they never read one of those registers before writing
// it. Some code doesn't: MSVC's SEH __finally funclets run in their parent's frame and read (and
// sometimes hand back) the parent's registers, and hand-written routines such as the fiber swap
// read them directly. FindContextFunctions finds those with a must-be-written dataflow over each
// function's guest instructions and keeps them on the context ("context functions"); a call from
// a local-register function into one copies the registers it names to ctx and back around the
// call (see printFunctionCall). Functions that call setjmp are kept on the context too, so that
// "ctx = env" after a longjmp restores them, and so are the __save/__rest helpers.

namespace
{
    // Bit index of a non-volatile register: GPR 14-31 -> 0-17, FPR 14-31 -> 18-35,
    // VR 14-31 -> 36-53, VR 64-127 -> 54-117. -1 for anything else.
    int NvIndex(char kind, int n)
    {
        switch (kind)
        {
        case 'r': return n >= 14 && n <= 31 ? n - 14 : -1;
        case 'f': return n >= 14 && n <= 31 ? 18 + n - 14 : -1;
        case 'v':
            if (n >= 14 && n <= 31) return 36 + n - 14;
            if (n >= 64 && n <= 127) return 54 + n - 64;
            return -1;
        }
        return -1;
    }

    struct RegToken
    {
        char kind; // 'r', 'f', 'v', or 0 for a non-register token
        int n;
    };

    // Split "r31,-16(r1)" or "v64,r0,r11" into tokens, keeping only register tokens' kind and number.
    std::vector<RegToken> Tokens(const char* s)
    {
        std::vector<RegToken> out;
        std::string cur;
        auto flush = [&]()
            {
                if (cur.empty())
                    return;
                RegToken t{ 0, 0 };
                if ((cur[0] == 'r' || cur[0] == 'f' || cur[0] == 'v') && cur.size() > 1 &&
                    std::all_of(cur.begin() + 1, cur.end(), [](char c) { return c >= '0' && c <= '9'; }))
                {
                    t.kind = cur[0];
                    t.n = std::atoi(cur.c_str() + 1);
                }
                out.push_back(t);
                cur.clear();
            };
        for (const char* p = s; *p; ++p)
        {
            if (*p == ',' || *p == '(' || *p == ')' || *p == ' ')
                flush();
            else
                cur += *p;
        }
        flush();
        return out;
    }

    bool StartsWith(const char* s, const char* prefix)
    {
        return std::strncmp(s, prefix, std::strlen(prefix)) == 0;
    }

    struct DefUse
    {
        NvSet def;
        NvSet use;      // reads that need the value
        NvSet spill;    // full-width stores of the register (prologue saves); harmless if uninitialised
        NvSet mentioned;
    };

    void AddReg(NvSet& set, const RegToken& t)
    {
        const int i = NvIndex(t.kind, t.n);
        if (i >= 0)
            set.set(i);
    }

    DefUse Classify(const ppc_insn& insn)
    {
        DefUse du;
        const char* name = insn.opcode->name;
        auto tokens = Tokens(insn.op_str);
        std::vector<RegToken> regs;
        for (auto& t : tokens)
            if (t.kind != 0)
                regs.push_back(t);
        for (auto& t : regs)
            AddReg(du.mentioned, t);
        if (regs.empty())
            return du;

        const bool store = name[0] == 's' && name[1] == 't';
        const bool load = name[0] == 'l';
        // Update forms (lwzu, stwux, lfdu, stvewx is not one): rA is written as well as read.
        const size_t len = std::strlen(name);
        const bool update = (load || store) && len >= 3 &&
            (name[len - 1] == 'u' || (name[len - 2] == 'u' && name[len - 1] == 'x')) &&
            !StartsWith(name, "lvlx") && !StartsWith(name, "lvrx");

        if (StartsWith(name, "stmw"))
        {
            // stmw rS stores rS..r31: a save when it is addressed off the stack (as for the other
            // stores below), otherwise real reads.
            bool stackAddress = regs.size() > 1;
            for (size_t k = 1; k < regs.size(); ++k)
            {
                AddReg(du.use, regs[k]);
                if (!(regs[k].kind == 'r' && (regs[k].n == 0 || regs[k].n == 1 || regs[k].n == 11 || regs[k].n == 12)))
                    stackAddress = false;
            }
            for (int n = (std::max)(regs[0].n, 14); n <= 31; ++n)
                (stackAddress ? du.spill : du.use).set(NvIndex('r', n));
            return du;
        }
        if (StartsWith(name, "lmw"))
        {
            for (size_t k = 1; k < regs.size(); ++k) AddReg(du.use, regs[k]);
            for (int n = (std::max)(regs[0].n, 14); n <= 31; ++n) du.def.set(NvIndex('r', n));
            return du;
        }
        if (store)
        {
            // The stored register: a full-width store (std, stfd, stvx and their indexed/update
            // forms) addressed off the stack (r1, or r11/r12 set from it by the prologue, with r0
            // as the zero index) is a save. Anything else is a real read: a narrower store, or a
            // store into some other structure (a context switch saving registers into a thread).
            const bool full = StartsWith(name, "std") || StartsWith(name, "stfd") || StartsWith(name, "stvx");
            bool stackAddress = regs.size() > 1;
            for (size_t k = 1; k < regs.size(); ++k)
                if (!(regs[k].kind == 'r' && (regs[k].n == 0 || regs[k].n == 1 || regs[k].n == 11 || regs[k].n == 12)))
                    stackAddress = false;
            if (full && stackAddress) AddReg(du.spill, regs[0]); else AddReg(du.use, regs[0]);
            for (size_t k = 1; k < regs.size(); ++k) AddReg(du.use, regs[k]);
            if (update && regs.size() > 1) AddReg(du.def, regs[1]);
            return du;
        }

        // Instructions with no register result: every register operand is read.
        const bool noDest =
            StartsWith(name, "cmp") || StartsWith(name, "fcmp") ||
            name[0] == 'b' ||
            StartsWith(name, "tw") || StartsWith(name, "td") ||
            StartsWith(name, "dcb") || StartsWith(name, "icb") ||
            StartsWith(name, "mt") ||
            StartsWith(name, "sync") || StartsWith(name, "lwsync") || StartsWith(name, "eieio") || StartsWith(name, "isync");
        if (noDest)
        {
            for (auto& t : regs) AddReg(du.use, t);
            return du;
        }

        // Instructions that merge into their destination read it too.
        static const char* merging[] = { "rlwimi", "rldimi", "insrwi", "insrdi", "inslwi", "insldi",
            "vrlimi128", "vmaddfp128", "vmaddcfp128", "vnmsubfp128", "vpkd3d128" };
        bool merge = false;
        for (auto* m : merging)
            if (std::strcmp(name, m) == 0 || (StartsWith(name, m) && name[std::strlen(m)] == '.'))
                merge = true;

        for (size_t k = 1; k < regs.size(); ++k) AddReg(du.use, regs[k]);
        if (merge) AddReg(du.use, regs[0]);
        AddReg(du.def, regs[0]);
        if (load && update && regs.size() > 1) AddReg(du.def, regs[1]);
        return du;
    }
}

void Recompiler::FindContextFunctions()
{
    struct Result
    {
        NvSet live;      // registers read before written on some path (including through calls)
        NvSet mentioned; // registers named by the function's own instructions
        bool callsSetjmp = false;
        std::vector<uint32_t> callees; // direct call and tail-call targets
    };

    // reads: the live-in registers of the context functions found so far. A direct call or tail
    // call to one of them reads those registers at the call.
    std::unordered_map<size_t, NvSet> reads;

    auto analyse = [&](const Function& fn) -> Result
        {
            Result res;
            const size_t count = fn.size / 4;
            auto* data = reinterpret_cast<const uint32_t*>(image.Find(fn.base));
            if (data == nullptr || count == 0)
                return res;

            std::vector<DefUse> du(count);
            std::vector<std::vector<size_t>> succ(count);
            for (size_t i = 0; i < count; ++i)
            {
                const uint32_t addr = uint32_t(fn.base + i * 4);
                const uint32_t w = ByteSwap(data[i]);
                ppc_insn insn;
                ppc::Disassemble(data + i, 4, addr, insn);
                if (insn.opcode != nullptr)
                {
                    du[i] = Classify(insn);
                    res.mentioned |= du[i].mentioned;
                }

                auto inside = [&](uint32_t target) { return target >= fn.base && target < fn.base + fn.size; };
                auto call = [&](uint32_t target)
                    {
                        if (target == config.setJmpAddress && config.setJmpAddress != 0)
                            res.callsSetjmp = true; // any call form: bl, bcl, a tail branch, a fall-through
                        res.callees.push_back(target);
                        auto it = reads.find(target);
                        if (it != reads.end())
                            du[i].use |= it->second;
                    };
                const uint32_t op = w >> 26;
                const bool lk = (w & 1) != 0;
                const bool next = i + 1 < count;
                // Running off the end of the function is a tail call to the next one, when the
                // emitter makes it one (Recompile(const Function&)): not after padding, an
                // always-trap, or an unconditional branch or call (taken to be noreturn there).
                const bool isBranchOp = op == 18 || op == 16 ||
                    (op == 19 && (((w >> 1) & 0x3FF) == 16 || ((w >> 1) & 0x3FF) == 528));
                const bool branchAlways = op == 18 || (isBranchOp && (((w >> 21) & 31) & 0x14) == 0x14);
                const bool trapAlways = (op == 3 && ((w >> 21) & 31) == 31) ||
                    (op == 31 && ((w >> 1) & 0x3FF) == 4 && ((w >> 21) & 31) == 31);
                const bool fallsOffEnd = !(w == 0 || trapAlways || branchAlways);
                auto fall = [&]()
                    {
                        if (next)
                            succ[i].push_back(i + 1);
                        else if (fallsOffEnd)
                            call(uint32_t(fn.base + fn.size));
                    };
                if (op == 18) // b, bl
                {
                    const int32_t li = int32_t(w << 6) >> 6 & ~3;
                    const uint32_t target = (w & 2) ? uint32_t(li) : addr + li;
                    if (lk)
                    {
                        call(target);
                        fall();
                    }
                    else if (inside(target))
                        succ[i].push_back((target - fn.base) / 4);
                    else
                        call(target); // tail call
                }
                else if (op == 16) // bc
                {
                    const int32_t bd = int16_t(w & 0xFFFC);
                    const uint32_t target = (w & 2) ? uint32_t(bd) : addr + bd;
                    const uint32_t bo = (w >> 21) & 31;
                    const bool always = (bo & 0x14) == 0x14;
                    if (lk || !inside(target))
                        call(target);
                    else
                        succ[i].push_back((target - fn.base) / 4);
                    if (lk || !always)
                        fall();
                }
                else if (op == 19 && (((w >> 1) & 0x3FF) == 16 || ((w >> 1) & 0x3FF) == 528)) // bclr, bcctr
                {
                    const uint32_t bo = (w >> 21) & 31;
                    const bool always = (bo & 0x14) == 0x14;
                    auto sw = config.switchTables.find(addr);
                    if (sw != config.switchTables.end())
                        for (auto label : sw->second.labels)
                            if (inside(label)) succ[i].push_back((label - fn.base) / 4);
                    if (lk || !always) fall();
                }
                else
                    fall();
            }

            // Must-be-written dataflow: in[i] = registers written on every path from the entry.
            std::vector<NvSet> in(count);
            std::vector<bool> reached(count, false);
            in[0].reset();
            reached[0] = true;
            std::vector<size_t> work{ 0 };
            while (!work.empty())
            {
                const size_t i = work.back();
                work.pop_back();
                const NvSet outSet = in[i] | du[i].def;
                for (size_t s : succ[i])
                {
                    const NvSet merged = reached[s] ? (in[s] & outSet) : outSet;
                    if (!reached[s] || merged != in[s])
                    {
                        in[s] = merged;
                        reached[s] = true;
                        work.push_back(s);
                    }
                }
            }
            for (size_t i = 0; i < count; ++i)
                if (reached[i])
                    res.live |= du[i].use & ~in[i];
            return res;
        };

    auto nameOf = [&](size_t address)
        {
            auto symbol = image.symbols.find(address);
            return symbol != image.symbols.end() ? symbol->name : fmt::format("sub_{:X}", address);
        };
    auto isHelper = [&](size_t address)
        {
            const std::string name = nameOf(address);
            return name.find("__save") == 0 || name.find("__rest") == 0;
        };

    // Host code replaces these: the port's hooks (host_functions) and the setjmp/longjmp the
    // generated code calls the host's versions of.
    auto isHost = [&](size_t address)
        {
            return config.hostFunctions.count(uint32_t(address)) ||
                (config.setJmpAddress != 0 && address == config.setJmpAddress) ||
                (config.longJmpAddress != 0 && address == config.longJmpAddress);
        };

    // First pass over every function; then, whenever a context function's live-in set grows,
    // analyse its callers again (a caller that passes a register through without writing it
    // becomes a context function itself), until nothing changes.
    std::vector<Result> results(functions.size());
    std::unordered_map<size_t, std::vector<size_t>> callers;
    std::vector<bool> context(functions.size(), false);
    size_t helpers = 0;
    std::vector<size_t> changed;
    for (size_t k = 0; k < functions.size(); ++k)
    {
        if (isHelper(functions[k].base))
        {
            context[k] = true;
            ++helpers;
            continue;
        }
        if (isHost(functions[k].base))
            continue; // replaced by host code: an ordinary call
        results[k] = analyse(functions[k]);
        for (uint32_t c : results[k].callees)
            callers[c].push_back(k);
        if (config.forcedContextFunctions.count(uint32_t(functions[k].base)))
            context[k] = true; // listed in context_functions
        if (results[k].live.any() || results[k].callsSetjmp)
        {
            context[k] = true;
            if (results[k].live.any())
            {
                reads[functions[k].base] = results[k].live;
                changed.push_back(k);
            }
        }
    }
    size_t passes = 0;
    while (!changed.empty())
    {
        ++passes;
        std::vector<size_t> next;
        std::unordered_set<size_t> seen;
        for (size_t k : changed)
        {
            auto it = callers.find(functions[k].base);
            if (it == callers.end())
                continue;
            for (size_t c : it->second)
            {
                if (!seen.insert(c).second || isHelper(functions[c].base) || isHost(functions[c].base))
                    continue;
                Result r = analyse(functions[c]);
                if (r.live != results[c].live)
                {
                    results[c] = std::move(r);
                    context[c] = true;
                    reads[functions[c].base] = results[c].live;
                    next.push_back(c);
                }
            }
        }
        changed = std::move(next);
    }

    // The registers a call into a context function copies: its own, and those of the context
    // functions it calls directly (they work on the same ctx registers).
    for (size_t k = 0; k < functions.size(); ++k)
        if (context[k])
            contextFunctions[functions[k].base] = results[k].mentioned;
    for (bool grew = true; grew;)
    {
        grew = false;
        for (size_t k = 0; k < functions.size(); ++k)
        {
            if (!context[k] || isHelper(functions[k].base))
                continue;
            NvSet& set = contextFunctions[functions[k].base];
            for (uint32_t c : results[k].callees)
            {
                if (isHelper(c))
                    continue;
                auto it = contextFunctions.find(c);
                if (it != contextFunctions.end() && (it->second & ~set).any())
                {
                    set |= it->second;
                    grew = true;
                }
            }
        }
    }

    std::vector<std::string> report;
    size_t readBeforeWrite = 0, setjmps = 0, forced = 0;
    for (size_t k = 0; k < functions.size(); ++k)
    {
        if (!context[k] || isHelper(functions[k].base))
            continue;
        const auto& r = results[k];
        if (r.callsSetjmp) ++setjmps; else if (r.live.any()) ++readBeforeWrite; else ++forced;
        std::string regs;
        for (int b = 0; b < 118; ++b)
        {
            if (!r.live.test(b)) continue;
            if (!regs.empty()) regs += ' ';
            if (b < 18) regs += fmt::format("r{}", b + 14);
            else if (b < 36) regs += fmt::format("f{}", b - 18 + 14);
            else if (b < 54) regs += fmt::format("v{}", b - 36 + 14);
            else regs += fmt::format("v{}", b - 54 + 64);
        }
        report.push_back(fmt::format("{:X} {}{}{}", functions[k].base, nameOf(functions[k].base),
            r.callsSetjmp ? " setjmp" : "", regs.empty() ? (r.callsSetjmp ? "" : " (context_functions)") : " reads " + regs));
    }

    std::sort(report.begin(), report.end());
    println("# Functions kept on the context under non_volatile_as_local (see nonvolatile_context.cpp):");
    println("# they read a non-volatile register before writing it (directly or by passing it to such a");
    println("# function), or call setjmp. Direct calls to them from other functions copy the registers");
    println("# to ctx and back. An indirect call to one does not; check none is reached through a pointer.");
    for (auto& line : report)
        println("{}", line);
    SaveCurrentOutData("ppc_context_functions.txt");
    fmt::println("non_volatile_as_local: {} functions kept on the context ({} read before write, {} call setjmp, {} listed; {} caller passes), plus {} save/restore helpers",
        readBeforeWrite + setjmps + forced, readBeforeWrite, setjmps, forced, passes, helpers);
}

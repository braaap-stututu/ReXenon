// Runs one suite's recompiled test functions against the expectations in its .vec file.
// Usage: suite.exe <file.vec> [first test index]
// Prints one line per test: PASS|FAIL|UNIMPL|CRASH|NOFN <index> <name> [detail], and HANG on a
// test that doesn't return (the process then exits; the runner restarts after that test).
#include "rxt_runtime.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace
{
    constexpr DWORD kRxtException = 0xE0525854; // 'RXT'

    char g_reason[512];
    uint8_t* g_base;
    std::vector<uint8_t*> g_committed;
    bool g_commitOverflow;

    LONG CALLBACK CommitOnDemand(EXCEPTION_POINTERS* e)
    {
        auto* rec = e->ExceptionRecord;
        if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || rec->NumberParameters < 2)
            return EXCEPTION_CONTINUE_SEARCH;
        auto* addr = reinterpret_cast<uint8_t*>(rec->ExceptionInformation[1]);
        if (addr < g_base || addr >= g_base + PPC_MEMORY_SIZE)
            return EXCEPTION_CONTINUE_SEARCH;
        if (g_committed.size() >= 16384)
        {
            g_commitOverflow = true;
            return EXCEPTION_CONTINUE_SEARCH;
        }
        auto* page = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(addr) & ~uintptr_t(0xFFF));
        if (!VirtualAlloc(page, 0x1000, MEM_COMMIT, PAGE_READWRITE))
            return EXCEPTION_CONTINUE_SEARCH;
        g_committed.push_back(page);
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    void ResetMemory()
    {
        for (auto* page : g_committed)
            VirtualFree(page, 0x1000, MEM_DECOMMIT);
        g_committed.clear();
        g_commitOverflow = false;
    }

    [[noreturn]] void Raise()
    {
        RaiseException(kRxtException, EXCEPTION_NONCONTINUABLE, 0, nullptr);
        __builtin_unreachable();
    }

    DWORD g_exceptionCode;
    uintptr_t g_exceptionAddress;

    int Filter(EXCEPTION_POINTERS* e)
    {
        g_exceptionCode = e->ExceptionRecord->ExceptionCode;
        g_exceptionAddress = e->ExceptionRecord->NumberParameters >= 2 ? e->ExceptionRecord->ExceptionInformation[1] : 0;
        return EXCEPTION_EXECUTE_HANDLER;
    }

    // 0 = returned, 1 = exception
    __declspec(noinline) int Call(RxtFn* fn, PPCContext* ctx, uint8_t* base)
    {
        __try
        {
            fn(*ctx, base);
            return 0;
        }
        __except (Filter(GetExceptionInformation()))
        {
            return 1;
        }
    }

    PPCRegister* Gpr(PPCContext& c, int n)
    {
        switch (n)
        {
#define X(i) case i: return &c.r##i;
            X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15)
            X(16) X(17) X(18) X(19) X(20) X(21) X(22) X(23) X(24) X(25) X(26) X(27) X(28) X(29) X(30) X(31)
#undef X
        }
        return nullptr;
    }

    PPCRegister* Fpr(PPCContext& c, int n)
    {
        switch (n)
        {
#define X(i) case i: return &c.f##i;
            X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15)
            X(16) X(17) X(18) X(19) X(20) X(21) X(22) X(23) X(24) X(25) X(26) X(27) X(28) X(29) X(30) X(31)
#undef X
        }
        return nullptr;
    }

    PPCVRegister* Vr(PPCContext& c, int n)
    {
        return n >= 0 && n < 128 ? &c.v0 + n : nullptr;
    }

    PPCCRRegister* Cr(PPCContext& c, int n) { return &c.cr0 + n; }

    uint32_t GetCr(PPCContext& c)
    {
        uint32_t v = 0;
        for (int i = 0; i < 8; i++)
        {
            auto* f = Cr(c, i);
            uint32_t bits = (f->lt & 1) << 3 | (f->gt & 1) << 2 | (f->eq & 1) << 1 | (f->so & 1);
            v |= bits << ((7 - i) * 4);
        }
        return v;
    }

    void SetCr(PPCContext& c, uint32_t v)
    {
        for (int i = 0; i < 8; i++)
        {
            uint32_t bits = (v >> ((7 - i) * 4)) & 0xF;
            auto* f = Cr(c, i);
            f->lt = (bits >> 3) & 1;
            f->gt = (bits >> 2) & 1;
            f->eq = (bits >> 1) & 1;
            f->so = bits & 1;
        }
    }

    uint32_t GetXer(PPCContext& c)
    {
        return uint32_t(c.xer.so & 1) << 31 | uint32_t(c.xer.ov & 1) << 30 | uint32_t(c.xer.ca & 1) << 29;
    }

    void SetXer(PPCContext& c, uint32_t v)
    {
        c.xer.so = (v >> 31) & 1;
        c.xer.ov = (v >> 30) & 1;
        c.xer.ca = (v >> 29) & 1;
    }

    struct RegValue
    {
        std::string reg; // r3, f1, v4, cr, xer
        char kind = 0;   // r f v c x
        int index = 0;
        bool asDouble = false; // f: compare as double (decimal expectation) instead of bits
        uint64_t value = 0;
        uint32_t words[4]{};
    };

    struct MemValue
    {
        uint32_t address;
        std::vector<uint8_t> bytes;
    };

    struct Test
    {
        std::string name;
        int fn = -1;
        std::vector<RegValue> in, out;
        std::vector<MemValue> memIn, memOut;
    };

    bool ParseReg(const char*& p, RegValue& r, bool isOut)
    {
        char reg[16];
        int n = 0;
        if (sscanf(p, "%15s%n", reg, &n) != 1) return false;
        p += n;
        r.reg = reg;
        if (r.reg == "cr") { r.kind = 'c'; }
        else if (r.reg == "xer") { r.kind = 'x'; }
        else { r.kind = reg[0]; r.index = atoi(reg + 1); }
        if (r.kind == 'v')
        {
            for (auto& w : r.words)
            {
                if (sscanf(p, "%x%n", &w, &n) != 1) return false;
                p += n;
            }
            return true;
        }
        if (r.kind == 'f' && isOut)
        {
            char mode[4];
            if (sscanf(p, "%3s%n", mode, &n) != 1) return false;
            p += n;
            r.asDouble = mode[0] == 'd';
        }
        unsigned long long v;
        if (sscanf(p, "%llx%n", &v, &n) != 1) return false;
        p += n;
        r.value = v;
        return true;
    }

    bool ParseMem(const char* p, MemValue& m)
    {
        int n = 0;
        if (sscanf(p, "%x%n", &m.address, &n) != 1) return false;
        p += n;
        while (*p == ' ') ++p;
        while (p[0] && p[1] && p[0] != '\n' && p[0] != '\r')
        {
            char hex[3] = { p[0], p[1], 0 };
            m.bytes.push_back(uint8_t(strtoul(hex, nullptr, 16)));
            p += 2;
        }
        return true;
    }

    std::vector<Test> Load(const char* path)
    {
        std::vector<Test> tests;
        FILE* f = fopen(path, "rb");
        if (!f) { fprintf(stderr, "can't open %s\n", path); exit(2); }
        char line[4096];
        Test* cur = nullptr;
        while (fgets(line, sizeof(line), f))
        {
            const char* p = line + 2;
            switch (line[0])
            {
            case 'T':
            {
                tests.emplace_back();
                cur = &tests.back();
                char name[512];
                int fn;
                if (sscanf(p, "%511s %d", name, &fn) == 2) { cur->name = name; cur->fn = fn; }
                break;
            }
            case 'I': { RegValue r; if (ParseReg(p, r, false)) cur->in.push_back(r); break; }
            case 'O': { RegValue r; if (ParseReg(p, r, true)) cur->out.push_back(r); break; }
            case 'M': { MemValue m; if (ParseMem(p, m)) cur->memIn.push_back(m); break; }
            case 'X': { MemValue m; if (ParseMem(p, m)) cur->memOut.push_back(m); break; }
            }
        }
        fclose(f);
        return tests;
    }

    std::atomic<int64_t> g_testStart{ 0 };
    std::atomic<int> g_currentIndex{ -1 };
    std::string g_currentName;

    void Watchdog()
    {
        for (;;)
        {
            Sleep(250);
            int64_t start = g_testStart.load();
            if (start != 0 && GetTickCount64() - start > 3000)
            {
                printf("HANG %d %s\n", g_currentIndex.load(), g_currentName.c_str());
                fflush(stdout);
                TerminateProcess(GetCurrentProcess(), 3);
            }
        }
    }

    std::string Hex64(uint64_t v) { char b[32]; snprintf(b, sizeof(b), "0x%016" PRIX64, v); return b; }
    std::string Hex32(uint32_t v) { char b[32]; snprintf(b, sizeof(b), "0x%08X", v); return b; }
    std::string VecStr(const uint32_t* w) { char b[64]; snprintf(b, sizeof(b), "[%08X, %08X, %08X, %08X]", w[0], w[1], w[2], w[3]); return b; }
    std::string DoubleStr(uint64_t bits) { double d; memcpy(&d, &bits, 8); char b[64]; snprintf(b, sizeof(b), "%.17g (0x%016" PRIX64 ")", d, bits); return b; }
}

extern "C" [[noreturn]] void rxt_unimplemented(uint32_t address, const char* what)
{
    snprintf(g_reason, sizeof(g_reason), "unimplemented %s at 0x%X", what, address);
    Raise();
}

extern "C" [[noreturn]] void rxt_unresolved_branch(uint32_t from, uint32_t to)
{
    snprintf(g_reason, sizeof(g_reason), "unresolved branch 0x%X -> 0x%X", from, to);
    Raise();
}

extern "C" [[noreturn]] void rxt_indirect_call(uint32_t target)
{
    snprintf(g_reason, sizeof(g_reason), "indirect call to 0x%X", target);
    Raise();
}

alignas(64) static PPCContext g_ctx;

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: %s <file.vec> [first index]\n", argv[0]);
        return 2;
    }
    int first = argc > 2 ? atoi(argv[2]) : 0;
    auto tests = Load(argv[1]);

    g_base = static_cast<uint8_t*>(VirtualAlloc(nullptr, PPC_MEMORY_SIZE, MEM_RESERVE, PAGE_NOACCESS));
    if (!g_base) { fprintf(stderr, "can't reserve guest memory\n"); return 2; }
    AddVectoredExceptionHandler(1, CommitOnDemand);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    std::thread(Watchdog).detach();

    for (int i = first; i < (int)tests.size(); i++)
    {
        Test& t = tests[i];
        if (t.fn < 0 || t.fn >= rxt_function_count || rxt_functions[t.fn].fn == nullptr)
        {
            printf("NOFN %d %s\n", i, t.name.c_str());
            continue;
        }

        PPCContext& ctx = g_ctx;
        ctx = PPCContext{};
        for (auto& r : t.in)
        {
            switch (r.kind)
            {
            case 'r': if (auto* g = Gpr(ctx, r.index)) g->u64 = r.value; break;
            case 'f': if (auto* g = Fpr(ctx, r.index)) g->u64 = r.value; break;
            case 'v': if (auto* v = Vr(ctx, r.index)) for (int k = 0; k < 4; k++) v->u32[3 - k] = r.words[k]; break;
            case 'c': SetCr(ctx, uint32_t(r.value)); break;
            case 'x': SetXer(ctx, uint32_t(r.value)); break;
            }
        }
        for (auto& m : t.memIn)
            memcpy(g_base + m.address, m.bytes.data(), m.bytes.size());

        _mm_setcsr(0x1F80);
        ctx.fpscr.loadFromHost();

        g_currentName = t.name;
        g_currentIndex = i;
        g_testStart = GetTickCount64();
        g_reason[0] = 0;
        int threw = Call(rxt_functions[t.fn].fn, &ctx, g_base);
        g_testStart = 0;
        _mm_setcsr(0x1F80);

        if (threw)
        {
            if (g_exceptionCode == kRxtException)
                printf("UNIMPL %d %s %s\n", i, t.name.c_str(), g_reason);
            else
                printf("CRASH %d %s exception 0x%08lX address 0x%llX%s\n", i, t.name.c_str(), g_exceptionCode,
                    (unsigned long long)g_exceptionAddress, g_commitOverflow ? " (touched too much guest memory)" : "");
            ResetMemory();
            fflush(stdout);
            continue;
        }

        std::string fails;
        auto mismatch = [&](const std::string& reg, const std::string& expected, const std::string& actual)
            {
                if (!fails.empty()) fails += " ; ";
                fails += reg + " expected " + expected + " actual " + actual;
            };
        for (auto& r : t.out)
        {
            switch (r.kind)
            {
            case 'r':
            {
                uint64_t a = Gpr(ctx, r.index)->u64;
                if (a != r.value) mismatch(r.reg, Hex64(r.value), Hex64(a));
                break;
            }
            case 'f':
            {
                uint64_t a = Fpr(ctx, r.index)->u64;
                bool ok;
                if (r.asDouble)
                {
                    double ad, ed;
                    memcpy(&ad, &a, 8);
                    memcpy(&ed, &r.value, 8);
                    ok = ad == ed;
                }
                else ok = a == r.value;
                if (!ok) mismatch(r.reg, r.asDouble ? DoubleStr(r.value) : Hex64(r.value), r.asDouble ? DoubleStr(a) : Hex64(a));
                break;
            }
            case 'v':
            {
                uint32_t a[4];
                auto* v = Vr(ctx, r.index);
                for (int k = 0; k < 4; k++) a[k] = v->u32[3 - k];
                if (memcmp(a, r.words, 16) != 0) mismatch(r.reg, VecStr(r.words), VecStr(a));
                break;
            }
            case 'c':
            {
                uint32_t a = GetCr(ctx);
                if (a != uint32_t(r.value)) mismatch("cr", Hex32(uint32_t(r.value)), Hex32(a));
                break;
            }
            case 'x':
            {
                uint32_t a = GetXer(ctx);
                if (a != uint32_t(r.value)) mismatch("xer", Hex32(uint32_t(r.value)), Hex32(a));
                break;
            }
            }
        }
        for (auto& m : t.memOut)
        {
            std::string e, a;
            bool bad = false;
            for (size_t k = 0; k < m.bytes.size(); k++)
            {
                uint8_t actual = g_base[m.address + k];
                char b[8];
                snprintf(b, sizeof(b), "%02X", m.bytes[k]); e += b;
                snprintf(b, sizeof(b), "%02X", actual); a += b;
                if (actual != m.bytes[k]) bad = true;
            }
            if (bad) mismatch("mem@" + Hex32(m.address), e, a);
        }
        ResetMemory();

        if (fails.empty())
            printf("PASS %d %s\n", i, t.name.c_str());
        else
            printf("FAIL %d %s %s\n", i, t.name.c_str(), fails.c_str());
        fflush(stdout);
    }
    return 0;
}

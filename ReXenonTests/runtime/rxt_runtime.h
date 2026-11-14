#pragma once
// Included first by every generated test file (and the harness). The traps the recompiler emits
// for code it can't do faithfully report back to the harness instead of killing the process.

#define PPC_CONFIG_H_INCLUDED
#define PPC_IMAGE_BASE 0x82000000ull
#define PPC_IMAGE_SIZE 0x01000000ull
#define PPC_CODE_BASE 0x82010000ull
#define PPC_CODE_SIZE 0x00100000ull

#include <cstdint>

extern "C" [[noreturn]] void rxt_unimplemented(uint32_t address, const char* what);
extern "C" [[noreturn]] void rxt_unresolved_branch(uint32_t from, uint32_t to);
extern "C" [[noreturn]] void rxt_indirect_call(uint32_t target);

#define PPC_UNIMPLEMENTED(address, what) rxt_unimplemented(address, what)
#define PPC_UNRESOLVED_BRANCH(from, to) rxt_unresolved_branch(from, to)
#define PPC_CALL_INDIRECT_FUNC(x) rxt_indirect_call(x)
#ifndef PPC_MSR_CHANGED
#define PPC_MSR_CHANGED(ctx, oldMsr) ((void)(oldMsr))
#endif

#include <ppc_context.h>

typedef void RxtFn(PPCContext& __restrict ctx, uint8_t* base);

struct RxtEntry
{
    const char* name;
    RxtFn* fn; // null when the function was left out because it didn't compile
};

extern "C" const RxtEntry rxt_functions[];
extern "C" const int rxt_function_count;

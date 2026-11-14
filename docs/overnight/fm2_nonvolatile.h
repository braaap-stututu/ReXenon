// A/B measurement only (ReXenon roadmap item 5 step 2): guest loads and stores without volatile.
// MMIO stores (store + eieio) keep volatile. Force-included before ppc_context.h.
#pragma once
#define PPC_LOAD_U8(x) *(uint8_t*)(base + (x))
#define PPC_LOAD_U16(x) __builtin_bswap16(*(uint16_t*)(base + (x)))
#define PPC_LOAD_U32(x) __builtin_bswap32(*(uint32_t*)(base + (x)))
#define PPC_LOAD_U64(x) __builtin_bswap64(*(uint64_t*)(base + (x)))
#define PPC_STORE_U8(x, y) *(uint8_t*)(base + (x)) = (y)
#define PPC_STORE_U16(x, y) *(uint16_t*)(base + (x)) = __builtin_bswap16(y)
#define PPC_STORE_U32(x, y) *(uint32_t*)(base + (x)) = __builtin_bswap32(y)
#define PPC_STORE_U64(x, y) *(uint64_t*)(base + (x)) = __builtin_bswap64(y)
#define PPC_MM_STORE_U8(x, y) *(volatile uint8_t*)(base + (x)) = (y)
#define PPC_MM_STORE_U16(x, y) *(volatile uint16_t*)(base + (x)) = __builtin_bswap16(y)
#define PPC_MM_STORE_U32(x, y) *(volatile uint32_t*)(base + (x)) = __builtin_bswap32(y)
#define PPC_MM_STORE_U64(x, y) *(volatile uint64_t*)(base + (x)) = __builtin_bswap64(y)

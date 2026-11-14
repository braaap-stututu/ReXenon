#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct powerpc_opcode;

struct rxt_operand
{
    unsigned int bitm;
    int shift;
    int has_insert;
    unsigned long flags;
};

const struct powerpc_opcode* rxt_opcodes(int* count);
void rxt_operand_info(int index, struct rxt_operand* out);
int rxt_dialect(void);
uint32_t rxt_insert(int index, uint32_t insn, long value, const char** errmsg);

#ifdef __cplusplus
}
#endif

// Operand flags (copied from ppc-dis.c, which only defines them inside the .c file).
#define RXT_OPERAND_SIGNED 0x1
#define RXT_OPERAND_SIGNOPT 0x2
#define RXT_OPERAND_FAKE 0x4
#define RXT_OPERAND_PARENS 0x8
#define RXT_OPERAND_CR 0x10
#define RXT_OPERAND_GPR 0x20
#define RXT_OPERAND_GPR_0 0x40
#define RXT_OPERAND_FPR 0x80
#define RXT_OPERAND_RELATIVE 0x100
#define RXT_OPERAND_ABSOLUTE 0x200
#define RXT_OPERAND_OPTIONAL 0x400
#define RXT_OPERAND_NEXT 0x800
#define RXT_OPERAND_NEGATIVE 0x1000
#define RXT_OPERAND_VR 0x2000
#define RXT_OPERAND_DS 0x4000
#define RXT_OPERAND_DQ 0x8000
#define RXT_OPERAND_PLUS1 0x10000

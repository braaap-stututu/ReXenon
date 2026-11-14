/* A private copy of the disassembler's opcode and operand tables, for the test assembler.
   The insert functions in ppc-dis.c are static, so the file is included here with its global
   names renamed; the copy linked into XenonUtils (the real disassembler) is untouched. */
#define powerpc_opcodes rxt_powerpc_opcodes
#define powerpc_num_opcodes rxt_powerpc_num_opcodes
#define powerpc_operands rxt_powerpc_operands
#define num_powerpc_operands rxt_num_powerpc_operands
#define powerpc_macros rxt_powerpc_macros
#define powerpc_num_macros rxt_powerpc_num_macros
#define decode_insn_ppc rxt_decode_insn_ppc
#define print_insn_ppc rxt_print_insn_ppc
#define print_insn_big_powerpc rxt_print_insn_big_powerpc
#define print_insn_little_powerpc rxt_print_insn_little_powerpc
#define print_insn_rs6000 rxt_print_insn_rs6000

#include "../thirdparty/disasm/ppc-dis.c"

#include "ppc_tables.h"

const struct powerpc_opcode* rxt_opcodes(int* count)
{
    *count = rxt_powerpc_num_opcodes;
    return rxt_powerpc_opcodes;
}

void rxt_operand_info(int index, struct rxt_operand* out)
{
    const struct powerpc_operand* o = &rxt_powerpc_operands[index];
    out->bitm = o->bitm;
    out->shift = o->shift;
    out->has_insert = o->insert != NULL;
    out->flags = o->flags;
}

/* The same dialect XenonUtils' disassembler uses ("cell 64"). */
int rxt_dialect(void)
{
    disassemble_info info;
    memset(&info, 0, sizeof(info));
    info.disassembler_options = "cell 64";
    return powerpc_dialect(&info);
}

uint32_t rxt_insert(int index, uint32_t insn, long value, const char** errmsg)
{
    const struct powerpc_operand* o = &rxt_powerpc_operands[index];
    if (o->insert)
        return (uint32_t)o->insert(insn, value, rxt_dialect(), errmsg);
    return insn | (uint32_t)(((unsigned long)value & o->bitm) << o->shift);
}

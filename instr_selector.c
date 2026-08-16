#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "instr_selector.h"
#include "varmap.h"   /* sostituisce la VarMap locale */

/* =========================================================================
 * Nomi registri fisici — ordine identico all'enum MachPhysReg.
 * ========================================================================= */
static const char *physName64[] = {
    "%rax", "%rcx", "%rdx", "%rsi", "%rdi",
    "%r8",  "%r9",  "%r10", "%r11",
    "%rbx", "%r12", "%r13", "%r14", "%r15",
    "%rbp", "%rsp", "%al"
};

/* =========================================================================
 * Costruttori operandi.
 * ========================================================================= */
static inline MachOperand mo_none(void) {
    return (MachOperand){ .kind = MO_NONE };
}
static inline MachOperand mo_vreg(int id) {
    return (MachOperand){ .kind = MO_VREG, .vregId = id };
}
static inline MachOperand mo_phys(MachPhysReg r) {
    return (MachOperand){ .kind = MO_PHYS, .physReg = (int)r };
}
static inline MachOperand mo_imm(long v) {
    return (MachOperand){ .kind = MO_IMM, .imm = v };
}
static inline MachOperand mo_label(int id) {
    return (MachOperand){ .kind = MO_LABEL, .labelId = id };
}
static inline MachOperand mo_func(const char *name) {
    return (MachOperand){ .kind = MO_FUNC, .func = name };
}
static inline MachOperand mo_mem(int base, int index, int scale, int disp) {
    return (MachOperand){ .kind = MO_MEM,
                          .data.mem = { .baseVreg = base,
                                       .indexVreg = index,
                                       .scale     = scale,
                                       .disp      = disp } };
}

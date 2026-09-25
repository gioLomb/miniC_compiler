/**
 * @file ir_print.c
 * @brief IR pretty-printer and program teardown.
 *
 * Split from ir.c for readability.
 */

#include "ir.h"
#include <stdio.h>
#include <stdlib.h>

static void ir_print_operand(const Operand *o) {
    switch (o->kind) {
    case OPND_NONE:        break;
    case OPND_TEMP:        printf("t%d", o->data.tempId); break;
    case OPND_VAR:
        printf("v%d.%d", o->data.varLevel, o->data.varOffset);
        if (o->data.sourceName) printf("/*%s*/", o->data.sourceName); // debug hint only
        break;
    case OPND_GLOBAL:      printf("g%d", o->data.globalOffset); break;
    case OPND_CONST_INT:   printf("%d",  o->data.intVal);            break;
    case OPND_CONST_FLOAT: printf("%g",  (double)o->data.floatVal);  break;
    case OPND_LABEL:       printf("L%d", o->data.labelId);           break;
    case OPND_FUNC:        printf("%s",  o->data.funcName);          break;
    }
}

static const char *ir_op_mnemonic(IROp op) {
    switch (op) {
    case IR_ADD: return "+";  case IR_SUB: return "-";
    case IR_MUL: return "*";  case IR_DIV: return "/"; case IR_MOD: return "%";
    case IR_LT:  return "<";  case IR_LE:  return "<=";
    case IR_GT:  return ">";  case IR_GE:  return ">=";
    case IR_EQ:  return "=="; case IR_NE:  return "!=";
    default:     return "?";
    }
}

static void ir_print_instr(const IRInstr *in) {
    switch (in->op) {
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
    case IR_LT:  case IR_LE:  case IR_GT:  case IR_GE:
    case IR_EQ:  case IR_NE:
        printf("    "); ir_print_operand(&in->dst);
        printf(" = ");  ir_print_operand(&in->src1);
        printf(" %s ", ir_op_mnemonic(in->op));
        ir_print_operand(&in->src2);
        break;
    case IR_NEG:
        printf("    "); ir_print_operand(&in->dst);
        printf(" = -"); ir_print_operand(&in->src1);
        break;
    case IR_NOT:
        printf("    "); ir_print_operand(&in->dst);
        printf(" = !"); ir_print_operand(&in->src1);
        break;
    case IR_ITOF:
        printf("    "); ir_print_operand(&in->dst);
        printf(" = (float)"); ir_print_operand(&in->src1);
        break;
    case IR_ASSIGN:
        printf("    "); ir_print_operand(&in->dst);
        printf(" = ");  ir_print_operand(&in->src1);
        break;
    case IR_GLOBAL_ADDR:
        printf("    "); ir_print_operand(&in->dst);
        printf(" = &g%d", in->src1.data.globalOffset);
        break;
    case IR_LOAD_ARR:
        printf("    "); ir_print_operand(&in->dst);
        printf(" = ");  ir_print_operand(&in->src1);
        printf("[");    ir_print_operand(&in->src2);
        printf("]");
        break;
    case IR_STORE_ARR:
        printf("    "); ir_print_operand(&in->dst);
        printf("[");    ir_print_operand(&in->src1);
        printf("] = "); ir_print_operand(&in->src2);
        break;
    case IR_PARAM:
        printf("    param "); ir_print_operand(&in->src1);
        break;
    case IR_CALL:
        printf("    "); ir_print_operand(&in->dst);
        printf(" = call "); ir_print_operand(&in->src1);
        printf(", ");       ir_print_operand(&in->src2);
        break;
    case IR_RETURN:
        printf("    return "); ir_print_operand(&in->src1);
        break;
    case IR_GOTO:
        printf("    goto "); ir_print_operand(&in->dst);
        break;
    case IR_IF_FALSE:
        printf("    if_false "); ir_print_operand(&in->src1);
        printf(" goto ");        ir_print_operand(&in->dst);
        break;
    case IR_LABEL:
        ir_print_operand(&in->dst); printf(":");
        break;
    }
    printf("\n");
}

void ir_print(const IRProgram *prog) {
    if (prog->globalCount > 0) {
        printf("=== GLOBALI ===\n");
        for (int i = 0; i < prog->globalCount; i++) {
            const IRGlobalVar *g = &prog->globals[i];
            printf("  [offset=%d] %s %s%s",
                   g->symOffset,
                   g->dataType == T_INT ? "int" : "float",
                   g->name,
                   g->isArray ? "[]" : "");
            if (g->initCount > 0) {
                printf(" = {");
                for (int j = 0; j < g->initCount; j++) {
                    if (j) printf(", ");
                    printf("%ld", g->initVals[j]);
                }
                printf("}");
            }
            printf("\n");
        }
        printf("\n");
    }

    for (int i = 0; i < prog->count; i++) {
        IRFunction *f = prog->functions[i];
        printf("funzione %s:\n", f->name);
        for (int j = 0; j < f->count; j++) ir_print_instr(&f->instrs[j]);
        printf("\n");
    }
}


void ir_free(IRProgram *prog) {
    if (!prog) return;
    for (int i = 0; i < prog->count; i++) {
        free(prog->functions[i]->name);
        free(prog->functions[i]->instrs);
        free(prog->functions[i]->blocks);
        free(prog->functions[i]->labelToBlock);
        free(prog->functions[i]->params);
        free(prog->functions[i]);
    }
    free(prog->functions);

    for (int i = 0; i < prog->globalCount; i++) {
        free(prog->globals[i].name);
        free(prog->globals[i].initVals);
    }
    free(prog->globals);

    free(prog);
}



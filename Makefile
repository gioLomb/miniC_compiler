# ============================================================
# Makefile - miniC compiler project
# ============================================================
CC      := gcc
CFLAGS  := -Wall -Wextra -std=gnu11 -g -O3
LDFLAGS :=

ifeq ($(SANITIZE),1)
CFLAGS  += -fsanitize=address,undefined
LDFLAGS += -fsanitize=address,undefined
endif

BUILD_DIR := build
BIN_DIR   := bin

# ---- Sorgenti condivisi ----
SCANNER_SRC     := scanner/re2c/scanner_generated.c
AST_SRC         := parser/ast.c
ERROR_SRC       := parser/errorCollector.c
PARSER_SRC      := parser/parser.c
HASHTABLE_SRC   := hash_table.c
SYMTAB_SRC      := symbol_table.c
AST2SYM_SRC     := ast_to_symtab.c
SEMANTIC_SRC    := semantic.c
ARENA_SRC       := arena.c
OPTIMIZE_SRC    := ast_optimizer.c
IR_SRC          := ir.c
SVN_SRC         := svn.c
DCE_SRC         := dce.c
LIVENESS_SRC    := liveness.c
CP_SRC          := cp.c
LICM_SRC        := licm.c
LOOP_SRC        := loop.c
SR_SRC          := sr.c
SCHED_SRC       := sched.c
INSTR_SEL_SRC   := instr_selector.c
INSTR_QUERY_SRC := instr_query.c
INTERFERENCE_SRC := interference.c
REGALLOC_UTILS_SRC := regalloc_utils.c
REGALLOC_SRC    := regalloc.c
VARMAP_SRC      := varmap.c
BUCKET_SRC      := bucket.c
CONSTMAP_SRC    := constmap.c
RA_COALESCE_SRC := ra_coalesce.c
RA_COLOR_SRC    := ra_color.c
RA_SPILL_SRC    := ra_spill.c
SCHED_DAG_SRC   := sched_dag.c
DYN_ARR_SRC     := dynamic_array.c
GLOBAL_LOWER_SRC := global_lower.c

# NOTA: INSTR_QUERY_SRC aggiunto a COMMON_SRCS. Contiene le query pure su
# MachInstr (operand extraction, opcode predicates) precedentemente in
# regalloc_utils.c: sia lo scheduler (sched_dag.c/sched_utils.h) sia il
# register allocator (interference.c/ra_spill.c) dipendono ora da questo
# modulo, mai l'uno dall'altro (vedi instr_query.h per la motivazione).
COMMON_SRCS := $(SCANNER_SRC) $(AST_SRC) $(ERROR_SRC) $(PARSER_SRC) $(ARENA_SRC) \
               $(HASHTABLE_SRC) $(SYMTAB_SRC) $(AST2SYM_SRC) $(SEMANTIC_SRC) \
               $(OPTIMIZE_SRC) $(IR_SRC) $(SVN_SRC) $(DCE_SRC) $(VARMAP_SRC) \
               $(LIVENESS_SRC) $(CONSTMAP_SRC) $(CP_SRC) $(GLOBAL_LOWER_SRC) \
               $(DYN_ARR_SRC) $(LICM_SRC) $(LOOP_SRC) $(SR_SRC) $(SCHED_DAG_SRC) \
               $(SCHED_SRC) $(INSTR_SEL_SRC) $(INSTR_QUERY_SRC) $(INTERFERENCE_SRC) \
               $(RA_COALESCE_SRC) $(RA_COLOR_SRC) $(RA_SPILL_SRC) \
               $(REGALLOC_UTILS_SRC) $(BUCKET_SRC) $(REGALLOC_SRC)

MINICC_SRCS      := $(COMMON_SRCS) parser/main.c
TEST_SYMTAB_SRCS := $(ARENA_SRC) $(HASHTABLE_SRC) $(SYMTAB_SRC) tests/sym_main.c
TEST_ARENA_SRCS  := $(ARENA_SRC) tests/test_arena.c
TEST_PASS1_SRCS  := $(SCANNER_SRC) $(AST_SRC) $(ERROR_SRC) $(PARSER_SRC) $(ARENA_SRC) \
                    $(HASHTABLE_SRC) $(SYMTAB_SRC) $(AST2SYM_SRC) tests/test_pass1.c
TEST_SEMANTIC_SRCS := $(SCANNER_SRC) $(AST_SRC) $(ERROR_SRC) $(PARSER_SRC) $(ARENA_SRC) \
                      $(HASHTABLE_SRC) $(SYMTAB_SRC) $(AST2SYM_SRC) $(SEMANTIC_SRC) \
                      tests/test_semantic.c
TEST_OPTIMIZE_SRCS := $(SCANNER_SRC) $(AST_SRC) $(ERROR_SRC) $(PARSER_SRC) $(ARENA_SRC) \
                      $(HASHTABLE_SRC) $(SYMTAB_SRC) $(AST2SYM_SRC) $(SEMANTIC_SRC) \
                      $(OPTIMIZE_SRC) tests/test_optimize.c
TEST_IR_SRCS      := $(COMMON_SRCS) tests/test_ir.c
TEST_SVN_SRCS      := $(COMMON_SRCS) test_svn.c

# ---- Backend unit tests ----
# test_bucket: nessuna dipendenza da liveness/IR, solo arena + bucket.
TEST_BUCKET_SRCS         := $(ARENA_SRC) $(BUCKET_SRC) \
                            tests/test_bucket.c

# test_regalloc_utils: usa instr_query.h (instr_uses/instr_defs/instr_is_*)
# e regalloc_utils.h (regalloc_spill_weight). Non serve liveness -> no ir.c.
TEST_REGALLOC_UTILS_SRCS := $(ARENA_SRC) $(HASHTABLE_SRC) $(DYN_ARR_SRC) \
                            $(VARMAP_SRC) $(ERROR_SRC) $(REGALLOC_UTILS_SRC) $(INSTR_QUERY_SRC) \
                            $(INSTR_SEL_SRC) tests/test_regalloc_utils.c

# test_interference e test_ra_color usano liveness_computeMach() che chiama
# ir_defines_dst / ir_operand_is_storage definite in ir.c. ir.c trascina
# l'intera catena frontend+ottimizzatori (stesso insieme di COMMON_SRCS):
# e' la strada piu' semplice senza introdurre nuovi file nel progetto.
TEST_INTERFERENCE_SRCS   := $(COMMON_SRCS) \
                            tests/test_interference.c
TEST_RA_COLOR_SRCS       := $(COMMON_SRCS) \
                            tests/test_ra_color.c

# test_ra_spill: costruisce MachFunction sintetiche e chiama
# ra_spill_insert() direttamente. Richiede instr_query.c (instr_is_rmw,
# usata da ra_spill.c) oltre a ra_spill.c + arena.c.
TEST_RA_SPILL_SRCS       := $(ARENA_SRC) $(INSTR_QUERY_SRC) $(RA_SPILL_SRC) \
                            tests/test_ra_spill.c

# ============================================================
to_objs = $(patsubst %.c,$(BUILD_DIR)/%.o,$(1))

MINICC_OBJS          := $(call to_objs,$(MINICC_SRCS))
TEST_SYMTAB_OBJS     := $(call to_objs,$(TEST_SYMTAB_SRCS))
TEST_ARENA_OBJS      := $(call to_objs,$(TEST_ARENA_SRCS))
TEST_PASS1_OBJS      := $(call to_objs,$(TEST_PASS1_SRCS))
TEST_SEMANTIC_OBJS   := $(call to_objs,$(TEST_SEMANTIC_SRCS))
TEST_OPTIMIZE_OBJS   := $(call to_objs,$(TEST_OPTIMIZE_SRCS))
TEST_IR_OBJS         := $(call to_objs,$(TEST_IR_SRCS))
TEST_SVN_OBJS        := $(call to_objs,$(TEST_SVN_SRCS))
TEST_BUCKET_OBJS         := $(call to_objs,$(TEST_BUCKET_SRCS))
TEST_REGALLOC_UTILS_OBJS := $(call to_objs,$(TEST_REGALLOC_UTILS_SRCS))
TEST_INTERFERENCE_OBJS   := $(call to_objs,$(TEST_INTERFERENCE_SRCS))
TEST_RA_COLOR_OBJS       := $(call to_objs,$(TEST_RA_COLOR_SRCS))
TEST_RA_SPILL_OBJS       := $(call to_objs,$(TEST_RA_SPILL_SRCS))

.PHONY: all clean check check-backend regen-scanner

all: $(BIN_DIR)/minicc \
     $(BIN_DIR)/test_symtab \
     $(BIN_DIR)/test_arena \
     $(BIN_DIR)/test_pass1 \
     $(BIN_DIR)/test_semantic \
     $(BIN_DIR)/test_optimize \
     $(BIN_DIR)/test_ir \
     $(BIN_DIR)/test_svn \
     $(BIN_DIR)/test_bucket \
     $(BIN_DIR)/test_regalloc_utils \
     $(BIN_DIR)/test_interference \
     $(BIN_DIR)/test_ra_color \
     $(BIN_DIR)/test_ra_spill

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I. -c $< -o $@

# ---- Existing binaries ----
$(BIN_DIR)/minicc: $(MINICC_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(LDFLAGS) $^ -o $@

$(BIN_DIR)/test_symtab: $(TEST_SYMTAB_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(LDFLAGS) $^ -o $@

$(BIN_DIR)/test_arena: $(TEST_ARENA_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(LDFLAGS) $^ -o $@

$(BIN_DIR)/test_pass1: $(TEST_PASS1_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(LDFLAGS) $^ -o $@

$(BIN_DIR)/test_semantic: $(TEST_SEMANTIC_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(LDFLAGS) $^ -o $@

$(BIN_DIR)/test_optimize: $(TEST_OPTIMIZE_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(LDFLAGS) $^ -o $@

$(BIN_DIR)/test_ir: $(TEST_IR_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(LDFLAGS) $^ -o $@

$(BIN_DIR)/test_svn: $(TEST_SVN_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(LDFLAGS) $^ -o $@

# ---- Backend unit tests ----
$(BIN_DIR)/test_bucket: $(TEST_BUCKET_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(LDFLAGS) $^ -o $@

$(BIN_DIR)/test_regalloc_utils: $(TEST_REGALLOC_UTILS_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(LDFLAGS) $^ -o $@

$(BIN_DIR)/test_interference: $(TEST_INTERFERENCE_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(LDFLAGS) $^ -o $@

$(BIN_DIR)/test_ra_color: $(TEST_RA_COLOR_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(LDFLAGS) $^ -o $@

# test_ra_spill (ra_spill.c non aveva alcun test dedicato).
$(BIN_DIR)/test_ra_spill: $(TEST_RA_SPILL_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(LDFLAGS) $^ -o $@

# ---- Test runners ----
check: $(BIN_DIR)/test_symtab \
       $(BIN_DIR)/test_arena \
       $(BIN_DIR)/test_optimize \
       $(BIN_DIR)/test_ir \
       $(BIN_DIR)/test_svn \
       $(BIN_DIR)/test_bucket \
       $(BIN_DIR)/test_regalloc_utils \
       $(BIN_DIR)/test_interference \
       $(BIN_DIR)/test_ra_color \
       $(BIN_DIR)/test_ra_spill
	./$(BIN_DIR)/test_symtab
	./$(BIN_DIR)/test_arena
	./$(BIN_DIR)/test_optimize
	./$(BIN_DIR)/test_ir
	./$(BIN_DIR)/test_svn
	./$(BIN_DIR)/test_bucket
	./$(BIN_DIR)/test_regalloc_utils
	./$(BIN_DIR)/test_interference
	./$(BIN_DIR)/test_ra_color
	./$(BIN_DIR)/test_ra_spill

# Esegue solo i test backend (utile durante il debug del regalloc).
check-backend: $(BIN_DIR)/test_bucket \
               $(BIN_DIR)/test_regalloc_utils \
               $(BIN_DIR)/test_interference \
               $(BIN_DIR)/test_ra_color \
               $(BIN_DIR)/test_ra_spill
	./$(BIN_DIR)/test_bucket
	./$(BIN_DIR)/test_regalloc_utils
	./$(BIN_DIR)/test_interference
	./$(BIN_DIR)/test_ra_color
	./$(BIN_DIR)/test_ra_spill

regen-scanner:
	re2c scanner/re2c/scanner.re -o scanner/re2c/scanner_generated.c

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)
# ============================================================
# Makefile - miniC compiler project
# ============================================================
CC      := gcc
CFLAGS  := -Wall -Wextra -std=gnu11 -g
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
ERROR_SRC       := parser/error.c
PARSER_SRC      := parser/parser.c
HASHTABLE_SRC   := hash_table.c
SYMTAB_SRC      := symbol_table.c
AST2SYM_SRC     := ast_to_symtab.c
SEMANTIC_SRC    := semantic.c
ARENA_SRC       := arena.c
OPTIMIZE_SRC    := optimize.c
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
INTERFERENCE_SRC := interference.c
REGALLOC_UTILS_SRC := regalloc_utils.c
REGALLOC_SRC    := regalloc.c
VARMAP_SRC := varmap.c
BUCKET_SRC := bucket.c
CONSTMAP_SRC := constmap.c
COMMON_SRCS := $(SCANNER_SRC) $(AST_SRC) $(ERROR_SRC) $(PARSER_SRC) $(ARENA_SRC) \
               $(HASHTABLE_SRC) $(SYMTAB_SRC) $(AST2SYM_SRC) $(SEMANTIC_SRC) \
               $(OPTIMIZE_SRC) $(IR_SRC) $(SVN_SRC) $(DCE_SRC)  $(VARMAP_SRC) $(LIVENESS_SRC) $(CONSTMAP_SRC) \
               $(CP_SRC) $(LICM_SRC) $(LOOP_SRC) $(SR_SRC) $(SCHED_SRC) \
               $(INSTR_SEL_SRC) $(INTERFERENCE_SRC) $(REGALLOC_UTILS_SRC) $(BUCKET_SRC) $(REGALLOC_SRC)

MINICC_SRCS      := $(COMMON_SRCS) parser/main.c
TEST_SYMTAB_SRCS := $(HASHTABLE_SRC) $(SYMTAB_SRC) tests/sym_main.c
TEST_ARENA_SRCS  := $(ARENA_SRC) tests/test_arena.c
TEST_PASS1_SRCS  := $(SCANNER_SRC) $(AST_SRC) $(ERROR_SRC) $(PARSER_SRC) $(ARENA_SRC) \
                     $(HASHTABLE_SRC) $(SYMTAB_SRC) $(AST2SYM_SRC) tests/test_pass1.c
TEST_SEMANTIC_SRCS := $(SCANNER_SRC) $(AST_SRC) $(ERROR_SRC) $(PARSER_SRC) $(ARENA_SRC) \
                       $(HASHTABLE_SRC) $(SYMTAB_SRC) $(AST2SYM_SRC) $(SEMANTIC_SRC) \
                       tests/test_semantic.c
TEST_OPTIMIZE_SRCS := $(SCANNER_SRC) $(AST_SRC) $(ERROR_SRC) $(PARSER_SRC) $(ARENA_SRC) \
                       $(HASHTABLE_SRC) $(SYMTAB_SRC) $(AST2SYM_SRC) $(SEMANTIC_SRC) $(OPTIMIZE_SRC) \
                       tests/test_optimize.c
TEST_IR_SRCS      := $(COMMON_SRCS) tests/test_ir.c

to_objs = $(patsubst %.c,$(BUILD_DIR)/%.o,$(1))

MINICC_OBJS      := $(call to_objs,$(MINICC_SRCS))
TEST_SYMTAB_OBJS := $(call to_objs,$(TEST_SYMTAB_SRCS))
TEST_ARENA_OBJS  := $(call to_objs,$(TEST_ARENA_SRCS))
TEST_PASS1_OBJS  := $(call to_objs,$(TEST_PASS1_SRCS))
TEST_SEMANTIC_OBJS := $(call to_objs,$(TEST_SEMANTIC_SRCS))
TEST_OPTIMIZE_OBJS := $(call to_objs,$(TEST_OPTIMIZE_SRCS))
TEST_IR_OBJS      := $(call to_objs,$(TEST_IR_SRCS))

.PHONY: all clean check regen-scanner

all: $(BIN_DIR)/minicc $(BIN_DIR)/test_symtab $(BIN_DIR)/test_arena $(BIN_DIR)/test_pass1 $(BIN_DIR)/test_semantic $(BIN_DIR)/test_optimize $(BIN_DIR)/test_ir

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I. -c $< -o $@

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

check: $(BIN_DIR)/test_symtab $(BIN_DIR)/test_arena $(BIN_DIR)/test_optimize $(BIN_DIR)/test_ir
	./$(BIN_DIR)/test_symtab
	./$(BIN_DIR)/test_arena
	./$(BIN_DIR)/test_optimize
	./$(BIN_DIR)/test_ir

regen-scanner:
	re2c scanner/re2c/scanner.re -o scanner/re2c/scanner_generated.c

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)
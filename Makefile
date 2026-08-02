# ============================================================
# Makefile - miniC compiler project
# ============================================================
# Target principali:
#   make               -> costruisce tutti i binari in bin/
#   make minicc        -> solo lexer+parser (parse tree)
#   make test_symtab   -> solo unit test del modulo symbol table
#   make test_arena    -> solo unit test dell'arena allocator
#   make test_pass1    -> lexer+parser+Pass1 (popolamento scope globale)
#   make test_semantic -> lexer+parser+Pass1+Pass2/semantica (fuse)
#   make test_optimize -> lexer+parser+Pass1+semantica+ottimizzazioni AST
#   make test_ir       -> lexer+parser+Pass1+semantica+generazione IR lineare (TAC)
#   make test_isel     -> unit test instruction selector (IR→MachInstr x86-64)
#   make check         -> esegue test_symtab, test_arena, test_optimize, test_ir e stampa l'esito
#   make clean         -> rimuove build/ e bin/
#   make regen-scanner -> rigenera scanner_generated.c da scanner.re (richiede re2c)
#
# Sanitizer opzionali (utile in sviluppo, non per release):
#   make SANITIZE=1
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

# ---- Sorgenti condivisi da piu' binari ----
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
LOOP_SRC        := loop.c
LICM_SRC        := licm.c
SR_SRC          := sr.c
CP_SRC          := cp.c
SCHED_SRC       := sched.c
ISEL_SRC        := instr_selector.c

# ---- Sorgenti del frontend completo (riusato in piu' binari) ----
FRONTEND_SRCS := \
    $(SCANNER_SRC) $(AST_SRC) $(ERROR_SRC) $(PARSER_SRC) $(ARENA_SRC) \
    $(HASHTABLE_SRC) $(SYMTAB_SRC) $(AST2SYM_SRC) $(SEMANTIC_SRC)

# ---- Sorgenti backend IR + ottimizzatori (riusato in piu' binari) ----
BACKEND_SRCS := \
    $(DCE_SRC) $(LIVENESS_SRC) $(LOOP_SRC) $(LICM_SRC) \
    $(SR_SRC) $(CP_SRC) $(SCHED_SRC) $(IR_SRC) $(SVN_SRC)

# ---- Composizione dei binari ----
MINICC_SRCS := \
    $(FRONTEND_SRCS) $(OPTIMIZE_SRC) \
    $(BACKEND_SRCS) \
    $(ISEL_SRC) \
    parser/main.c

TEST_SYMTAB_SRCS := \
    $(HASHTABLE_SRC) $(SYMTAB_SRC) \
    tests/sym_main.c

TEST_ARENA_SRCS := \
    $(ARENA_SRC) \
    tests/test_arena.c

TEST_PASS1_SRCS := \
    $(FRONTEND_SRCS) \
    tests/test_pass1.c

TEST_SEMANTIC_SRCS := \
    $(FRONTEND_SRCS) \
    tests/test_semantic.c

TEST_OPTIMIZE_SRCS := \
    $(FRONTEND_SRCS) $(OPTIMIZE_SRC) \
    tests/test_optimize.c

TEST_IR_SRCS := \
    $(FRONTEND_SRCS) \
    $(BACKEND_SRCS) \
    $(ISEL_SRC) \
    tests/test_ir.c

TEST_ISEL_SRCS := \
    $(ISEL_SRC) \
    tests/test_isel.c

# Traduce ogni lista di sorgenti .c nei corrispondenti .o dentro build/
# (build/ rispecchia la struttura delle cartelle sorgente)
to_objs = $(patsubst %.c,$(BUILD_DIR)/%.o,$(1))

MINICC_OBJS        := $(call to_objs,$(MINICC_SRCS))
TEST_SYMTAB_OBJS   := $(call to_objs,$(TEST_SYMTAB_SRCS))
TEST_ARENA_OBJS    := $(call to_objs,$(TEST_ARENA_SRCS))
TEST_PASS1_OBJS    := $(call to_objs,$(TEST_PASS1_SRCS))
TEST_SEMANTIC_OBJS := $(call to_objs,$(TEST_SEMANTIC_SRCS))
TEST_OPTIMIZE_OBJS := $(call to_objs,$(TEST_OPTIMIZE_SRCS))
TEST_IR_OBJS       := $(call to_objs,$(TEST_IR_SRCS))
TEST_ISEL_OBJS     := $(call to_objs,$(TEST_ISEL_SRCS))

.PHONY: all minicc test_symtab test_arena test_pass1 test_semantic \
        test_optimize test_ir test_isel check clean regen-scanner

all: $(BIN_DIR)/minicc \
     $(BIN_DIR)/test_symtab \
     $(BIN_DIR)/test_arena \
     $(BIN_DIR)/test_pass1 \
     $(BIN_DIR)/test_semantic \
     $(BIN_DIR)/test_optimize \
     $(BIN_DIR)/test_ir \
     $(BIN_DIR)/test_isel

# ---- Regola generica: compila qualunque src/File.c in build/src/File.o ----
# -I. permette agli #include senza percorso (es. "symbol_table.h" da
# dentro tests/) di essere trovati nella root del progetto.
$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I. -c $< -o $@

# ---- Link dei binari ----
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

$(BIN_DIR)/test_isel: $(TEST_ISEL_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(LDFLAGS) $^ -o $@

# ---- Phony target per build singolo (comodo da riga di comando) ----
minicc:      $(BIN_DIR)/minicc
test_symtab: $(BIN_DIR)/test_symtab
test_arena:  $(BIN_DIR)/test_arena
test_pass1:  $(BIN_DIR)/test_pass1
test_semantic: $(BIN_DIR)/test_semantic
test_optimize: $(BIN_DIR)/test_optimize
test_ir:     $(BIN_DIR)/test_ir
test_isel:   $(BIN_DIR)/test_isel

# ---- check: esegue i test automatici dei moduli ----
check: $(BIN_DIR)/test_symtab \
       $(BIN_DIR)/test_arena \
       $(BIN_DIR)/test_optimize \
       $(BIN_DIR)/test_ir \
       $(BIN_DIR)/test_isel
	@echo "--- test_symtab ---"
	./$(BIN_DIR)/test_symtab
	@echo "--- test_arena ---"
	./$(BIN_DIR)/test_arena
	@echo "--- test_optimize ---"
	./$(BIN_DIR)/test_optimize
	@echo "--- test_ir ---"
	./$(BIN_DIR)/test_ir
	@echo "--- test_isel ---"
	./$(BIN_DIR)/test_isel

# ---- Rigenera lo scanner da scanner.re (richiede re2c installato) ----
regen-scanner:
	re2c scanner/re2c/scanner.re -o scanner/re2c/scanner_generated.c

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)
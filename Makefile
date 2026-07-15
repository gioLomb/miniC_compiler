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
#   make check         -> esegue test_symtab e test_arena e stampa l'esito
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

# ---- Composizione dei binari ----
MINICC_SRCS      := $(SCANNER_SRC) $(AST_SRC) $(ERROR_SRC) $(PARSER_SRC) $(ARENA_SRC) parser/main.c
TEST_SYMTAB_SRCS := $(HASHTABLE_SRC) $(SYMTAB_SRC) tests/sym_main.c
TEST_ARENA_SRCS  := $(ARENA_SRC) tests/test_arena.c
TEST_PASS1_SRCS  := $(SCANNER_SRC) $(AST_SRC) $(ERROR_SRC) $(PARSER_SRC) $(ARENA_SRC) \
                     $(HASHTABLE_SRC) $(SYMTAB_SRC) $(AST2SYM_SRC) tests/test_pass1.c
TEST_SEMANTIC_SRCS := $(SCANNER_SRC) $(AST_SRC) $(ERROR_SRC) $(PARSER_SRC) $(ARENA_SRC) \
                       $(HASHTABLE_SRC) $(SYMTAB_SRC) $(AST2SYM_SRC) $(SEMANTIC_SRC) \
                       tests/test_semantic.c

# Traduce ogni lista di sorgenti .c nei corrispondenti .o dentro build/
# (build/ rispecchia la struttura delle cartelle sorgente)
to_objs = $(patsubst %.c,$(BUILD_DIR)/%.o,$(1))

MINICC_OBJS      := $(call to_objs,$(MINICC_SRCS))
TEST_SYMTAB_OBJS := $(call to_objs,$(TEST_SYMTAB_SRCS))
TEST_ARENA_OBJS  := $(call to_objs,$(TEST_ARENA_SRCS))
TEST_PASS1_OBJS  := $(call to_objs,$(TEST_PASS1_SRCS))
TEST_SEMANTIC_OBJS := $(call to_objs,$(TEST_SEMANTIC_SRCS))

.PHONY: all clean check regen-scanner

all: $(BIN_DIR)/minicc $(BIN_DIR)/test_symtab $(BIN_DIR)/test_arena $(BIN_DIR)/test_pass1 $(BIN_DIR)/test_semantic

# ---- Regola generica: compila qualunque src/File.c in build/src/File.o ----
# -I. permette agli #include senza percorso (es. "symbol_table.h" da
# dentro tests/) di essere trovati nella root del progetto.
$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I. -c $< -o $@

# ---- Link dei tre binari ----
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

# ---- Comodo: esegue i test automatici dei moduli di base ----
check: $(BIN_DIR)/test_symtab $(BIN_DIR)/test_arena
	./$(BIN_DIR)/test_symtab
	./$(BIN_DIR)/test_arena

# ---- Rigenera lo scanner da scanner.re (richiede re2c installato) ----
regen-scanner:
	re2c scanner/re2c/scanner.re -o scanner/re2c/scanner_generated.c

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)
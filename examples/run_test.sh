#!/bin/bash
# run_tests.sh - differential testing: minicc vs gcc (oracolo).
#
# Per ogni file.c in TEST_DIR:
#   1) gcc compila file.c direttamente  -> binario di riferimento (comportamento atteso)
#   2) minicc compila file.c in assembly (-S)
#   3) gcc assembla+linka l'assembly     -> binario minicc
#   4) esegue entrambi i binari, confronta exit code ($?, gia' mod 256 su entrambi
#      i lati per convenzione POSIX, quindi il confronto resta valido anche se
#      il valore di ritorno reale supera 255)
#
# Uso: ./run_tests.sh [test_dir]   (default: ./examples)
#      MINICC=path/al/binario ./run_tests.sh
#
# Nota: richiede variabili inizializzate esplicitamente nei .c di test (niente UB
# da lettura di memoria non inizializzata), altrimenti gcc e minicc possono
# divergere per motivi non legati a bug del compilatore.

set -uo pipefail

MINICC="${MINICC:-./bin/minicc}"
TEST_DIR="${1:-./examples}"
GCC_REF_FLAGS="-std=gnu89 -w -fno-builtin"   # gnu89: miniC non ha forward-decl, serve implicit-decl permesso
TIMEOUT_BIN="$(command -v timeout || true)"  # opzionale, protegge da loop infiniti nei test

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'

# Precondizioni: binario minicc deve esistere ed essere eseguibile.
if [ ! -x "$MINICC" ]; then
    echo "Errore: binario minicc non trovato/eseguibile in '$MINICC'." >&2
    echo "Compila prima con 'make' (produce bin/minicc), oppure imposta MINICC=<path>." >&2
    exit 1
fi

shopt -s nullglob
files=("$TEST_DIR"/*.c)
if [ ${#files[@]} -eq 0 ]; then
    echo "Errore: nessun file .c trovato in '$TEST_DIR'." >&2
    exit 1
fi

run_with_timeout() {
    # Esegue $1 con timeout se disponibile, altrimenti diretto.
    if [ -n "$TIMEOUT_BIN" ]; then
        "$TIMEOUT_BIN" 10 "$1"
    else
        "$1"
    fi
}

pass=0; fail=0; skip=0

printf "%-28s %-10s %s\n" "TEST" "ESITO" "DETTAGLI"
printf '%.0s-' $(seq 1 60); echo

for src in "${files[@]}"; do
    name="$(basename "$src" .c)"
    ref_bin="$TMP_DIR/${name}_ref"
    asm_out="$TMP_DIR/${name}.s"
    mc_bin="$TMP_DIR/${name}_mc"

    # 1) Oracolo: gcc compila il sorgente cosi' com'e' -> comportamento atteso.
    if ! gcc $GCC_REF_FLAGS "$src" -o "$ref_bin" 2>"$TMP_DIR/${name}_ref.err"; then
        printf "%-28s ${YELLOW}%-10s${NC} %s\n" "$name" "SKIP" "gcc non compila il sorgente (vedi $TMP_DIR/${name}_ref.err)"
        skip=$((skip+1)); continue
    fi

    # 2) minicc genera assembly x86-64 AT&T.
    if ! "$MINICC" "$src" -S -o "$asm_out" 2>"$TMP_DIR/${name}_mc.err"; then
        printf "%-28s ${RED}%-10s${NC} %s\n" "$name" "FAIL" "minicc fallisce la compilazione"
        fail=$((fail+1)); continue
    fi

    # 3) Assemblaggio+link dell'assembly generato (gcc fa da assembler/linker,
    #    fornisce il runtime startup corretto per main).
    if ! gcc "$asm_out" -o "$mc_bin" 2>"$TMP_DIR/${name}_as.err"; then
        printf "%-28s ${RED}%-10s${NC} %s\n" "$name" "FAIL" "assembly non valido (vedi $TMP_DIR/${name}_as.err)"
        fail=$((fail+1)); continue
    fi

    # 4) Esecuzione e confronto exit code.
    run_with_timeout "$ref_bin"; ref_rc=$?
    run_with_timeout "$mc_bin";  mc_rc=$?

    if [ "$ref_rc" -eq "$mc_rc" ]; then
        printf "%-28s ${GREEN}%-10s${NC} rc=%d\n" "$name" "PASS" "$ref_rc"
        pass=$((pass+1))
    else
        printf "%-28s ${RED}%-10s${NC} atteso=%d ottenuto=%d\n" "$name" "FAIL" "$ref_rc" "$mc_rc"
        fail=$((fail+1))
    fi
done

echo
echo "Risultati: $pass passati, $fail falliti, $skip saltati (su ${#files[@]} totali)."

# Exit code script: 0 solo se nessun fallimento (utile per CI / make check).
[ "$fail" -eq 0 ]
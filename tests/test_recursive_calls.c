/* test_recursive_calls.c
 *
 * Obiettivo: ricorsione doppia (fibonacci) senza memoization, forte
 * profondita' di call stack. Verifica:
 *   - correttezza prologo/epilogo (pushq %rbp; movq %rsp,%rbp; subq frame)
 *     e save/restore dei callee-saved effettivamente usati per ogni
 *     invocazione ricorsiva (regalloc_save_restore_callee in regalloc.c).
 *   - IR_PARAM/IR_CALL: 'n' e' vivo attraverso le due chiamate ricorsive
 *     (fib(n-1) poi fib(n-2)), quindi deve sopravvivere alla prima CALL
 *     (crossesCall) prima di essere riletto per calcolare n-2.
 *   - IR_IF_FALSE / IR_LABEL generati da 'if' senza else.
 */

int fib(int n) {
    if (n < 2) {
        return n;
    }
    return fib(n - 1) + fib(n - 2);
}

int main() {
    int r;
    r = fib(20);
    return r;
}

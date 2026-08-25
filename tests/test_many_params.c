/* test_many_params.c
 *
 * Obiettivo: chiamata con esattamente 6 argomenti (NUM_ARG_REGS, il massimo
 * supportato senza passaggio via stack, vedi instr_selector.c). Verifica:
 *   - binding parametri formali <- registri ABI (rdi,rsi,rdx,rcx,r8,r9)
 *     nel prologo di sum6.
 *   - IR_PARAM emesso 6 volte prima della IR_CALL, con nArgs=6 nel test
 *     IR (vedi PASS 9 di test_ir.c per lo stesso pattern con 1 argomento).
 *   - chiamate annidate: sum6(...) usato come argomento di se stesso in
 *     seconda chiamata forza materializzazione del risultato prima di
 *     essere ri-passato come argomento.
 */

int sum6(int a, int b, int c, int d, int e, int f) {
    return a + b + c + d + e + f;
}

int main() {
    int r1;
    int r2;
    r1 = sum6(1, 2, 3, 4, 5, 6);
    r2 = sum6(r1, r1, r1, r1, r1, r1);
    return r2;
}

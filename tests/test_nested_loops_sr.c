/* test_nested_loops_sr.c
 *
 * Obiettivo: loop annidati su matrice linearizzata. Verifica:
 *   - 'base = i * 10' e' loop-invariant rispetto al loop interno (dipende
 *     solo da i, non modificato nel corpo interno): LICM deve issarlo nel
 *     pre-header del loop INTERNO ad ogni iterazione esterna.
 *   - 'j' e' basic induction variable del loop interno; 'idx = base + j'
 *     e' un derived pattern (somma, non moltiplicazione: SR in questo
 *     compilatore riconosce solo t = i*CONST, quindi qui NON deve scattare
 *     SR — verifica che l'assenza di strength reduction non comprometta
 *     comunque la correttezza del risultato).
 *   - STORE_ARR seguita da LOAD_ARR sullo stesso indice nella stessa
 *     iterazione: nessuna passata deve riordinarle (dipendenza di alias
 *     conservativa in DCE/LICM).
 */

int main() {
    int mat[100];
    int i;
    int j;
    int idx;
    int base;
    int total;

    total = 0;
    i = 0;
    while (i < 10) {
        base = i * 10;
        j = 0;
        while (j < 10) {
            idx = base + j;
            mat[idx] = idx;
            total = total + mat[idx];
            j = j + 1;
        }
        i = i + 1;
    }

    return total;
}

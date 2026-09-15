/* Test: nested loops on a linearized matrix.
 * Checks LICM of outer-loop-invariant 'base' into the inner pre-header,
 * and that derived idx = base + j does not trigger strength reduction.
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

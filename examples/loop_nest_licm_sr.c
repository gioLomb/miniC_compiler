// loop_nest_licm_sr.c
// Target passes: LICM (loop-invariant code motion), strength reduction (SR),
// loop nesting stresses scheduling too (sched) since inner body has independent ops.

int invariant_mix(int a, int b, int c) {
    // pure function of outer-loop-invariant args -> candidate to hoist out of inner loop
    return a * b + c * c - a;
}

int nested_licm_sr(int n, int m, int base) {
    int i, j, acc, k, invariant_val, idx4, idx7;

    acc = 0;
    i = 0;
    while (i < n) {
        // invariant_val depends only on i (outer loop var), invariant w.r.t. inner loop j
        invariant_val = invariant_mix(base, i, i + 1);

        j = 0;
        k = 0;
        while (j < m) {
            // idx4 = k*4 -> classic strength-reduction candidate (replace mul with add)
            idx4 = k * 4;
            // idx7 = k*7 -> another SR candidate, different stride
            idx7 = k * 7;

            // invariant_val recomputation avoided if LICM works: same value every j iter
            acc = acc + invariant_val + idx4 - idx7;

            // independent ops (no data dependency) -> good for instruction scheduling
            acc = acc + (j * 2) - (j / 2);

            k = k + 1;
            j = j + 1;
        }
        i = i + 1;
    }
    return acc;
}

int triple_nested(int n) {
    int i, j, l, acc, stride_mul;
    acc = 0;
    i = 0;
    while (i < n) {
        j = 0;
        while (j < n) {
            l = 0;
            while (l < n) {
                // stride_mul = l*8, SR candidate at innermost level (hottest loop)
                stride_mul = l * 8;
                acc = acc + stride_mul + i - j;
                l = l + 1;
            }
            j = j + 1;
        }
        i = i + 1;
    }
    return acc;
}

int main(void) {
    int r1, r2, total;

    r1 = nested_licm_sr(80, 80, 3);
    r2 = triple_nested(30);
    total = r1 + r2;

    if (total != 0) {
        return 0;
    }
    return 1;
}

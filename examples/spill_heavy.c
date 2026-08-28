// spill_heavy.c
// Target passes: register allocation (spill), local/global value numbering (SVN)
// Pattern: many simultaneously-live scalars combined at the end -> forces spill.
// Repeated identical subexpressions (a*b+c) across vars -> SVN candidates.

int combine6(int p0, int p1, int p2, int p3, int p4, int p5) {
    // simple mixer, keeps params alive across calls below
    int r;
    r = p0 + p1 * p2 - p3 + p4 * p5;
    return r;
}

int spill_kernel(int seed) {
    // 40 live ints forcing spill on most targets (regs << 40)
    int v0, v1, v2, v3, v4, v5, v6, v7, v8, v9;
    int v10, v11, v12, v13, v14, v15, v16, v17, v18, v19;
    int v20, v21, v22, v23, v24, v25, v26, v27, v28, v29;
    int v30, v31, v32, v33, v34, v35, v36, v37, v38, v39;
    int acc;

    v0 = seed + 1;
    v1 = seed + 2;
    v2 = seed + 3;
    v3 = seed + 4;
    v4 = seed + 5;
    v5 = seed + 6;
    v6 = seed + 7;
    v7 = seed + 8;
    v8 = seed + 9;
    v9 = seed + 10;
    v10 = v0 * v1 + v2;
    v11 = v1 * v2 + v3;
    v12 = v2 * v3 + v4;
    v13 = v3 * v4 + v5;
    v14 = v4 * v5 + v6;
    v15 = v5 * v6 + v7;
    v16 = v6 * v7 + v8;
    v17 = v7 * v8 + v9;
    v18 = v8 * v9 + v0;
    v19 = v9 * v0 + v1;
    // repeated identical subexpr (v0*v1+v2) reused -> SVN candidate
    v20 = (v0 * v1 + v2) + v10;
    v21 = (v0 * v1 + v2) - v11;
    v22 = v10 + v11 + v12;
    v23 = v13 + v14 + v15;
    v24 = v16 + v17 + v18;
    v25 = v19 + v20 + v21;
    v26 = v22 * v23;
    v27 = v24 * v25;
    v28 = v26 + v27;
    v29 = v28 - v0;
    v30 = v29 + v1;
    v31 = v30 + v2;
    v32 = v31 + v3;
    v33 = v32 + v4;
    v34 = v33 + v5;
    v35 = v34 + v6;
    v36 = v35 + v7;
    v37 = v36 + v8;
    v38 = v37 + v9;
    v39 = v38 + v10;

    // final combine keeps every single var alive up to this point -> spill pressure
    acc = v0 + v1 + v2 + v3 + v4 + v5 + v6 + v7 + v8 + v9;
    acc = acc + v10 + v11 + v12 + v13 + v14 + v15 + v16 + v17 + v18 + v19;
    acc = acc + v20 + v21 + v22 + v23 + v24 + v25 + v26 + v27 + v28 + v29;
    acc = acc + v30 + v31 + v32 + v33 + v34 + v35 + v36 + v37 + v38 + v39;

    acc = acc + combine6(v0, v1, v2, v3, v4, v5);
    return acc;
}

float spill_kernel_f(float seed) {
    // float version -> stresses fp register file / spill separately
    float f0, f1, f2, f3, f4, f5, f6, f7, f8, f9;
    float f10, f11, f12, f13, f14, f15, f16, f17, f18, f19;
    float acc;

    f0 = seed + 1.5;
    f1 = seed + 2.5;
    f2 = seed + 3.5;
    f3 = seed + 4.5;
    f4 = seed + 5.5;
    f5 = seed + 6.5;
    f6 = seed + 7.5;
    f7 = seed + 8.5;
    f8 = seed + 9.5;
    f9 = seed + 10.5;
    f10 = f0 * f1 + f2;
    f11 = f1 * f2 + f3;
    f12 = f2 * f3 + f4;
    f13 = f3 * f4 + f5;
    f14 = f4 * f5 + f6;
    f15 = f5 * f6 + f7;
    f16 = f6 * f7 + f8;
    f17 = f7 * f8 + f9;
    f18 = f8 * f9 + f0;
    f19 = f9 * f0 + f1;

    acc = f0 + f1 + f2 + f3 + f4 + f5 + f6 + f7 + f8 + f9;
    acc = acc + f10 + f11 + f12 + f13 + f14 + f15 + f16 + f17 + f18 + f19;
    return acc;
}

int main(void) {
    int i, total;
    float ftotal;

    total = 0;
    i = 0;
    // no 'for' allowed -> while
    while (i < 2000) {
        total = total + spill_kernel(i);
        i = i + 1;
    }

    ftotal = 0.0;
    i = 0;
    while (i < 2000) {
        ftotal = ftotal + spill_kernel_f(i);
        i = i + 1;
    }

    if (total > 0 && ftotal > 0.0) {
        return 0;
    }
    return 1;
}

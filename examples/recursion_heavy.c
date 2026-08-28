// recursion_heavy.c
// Target passes: register allocation across calls, instruction scheduling around calls,
// call-overhead pattern (no tail-call opt assumed -> real stack frames).

int fib(int n) {
    // classic exponential recursion -> deep call tree, heavy call/ret pattern
    if (n < 2) {
        return n;
    }
    return fib(n - 1) + fib(n - 2);
}

int is_even(int n);
int is_odd(int n);

int is_even(int n) {
    // mutual recursion -> stresses call graph / interprocedural regalloc decisions
    if (n == 0) {
        return 1;
    }
    return is_odd(n - 1);
}

int is_odd(int n) {
    if (n == 0) {
        return 0;
    }
    return is_even(n - 1);
}

int fact_acc(int n, int acc) {
    // tail-recursive style (accumulator passed through) -> candidate for TCO if compiler has it
    if (n <= 1) {
        return acc;
    }
    return fact_acc(n - 1, acc * n);
}

int ackermann_like(int m, int n) {
    // bounded ackermann-style: fast-growing recursion depth/width, keep args small
    if (m == 0) {
        return n + 1;
    }
    if (n == 0) {
        return ackermann_like(m - 1, 1);
    }
    return ackermann_like(m - 1, ackermann_like(m, n - 1));
}

int recursive_sum_with_locals(int n) {
    // recursion with several locals alive across the recursive call -> forces
    // callee-saved regs or spill around the call site (regalloc + call interplay)
    int a, b, c, d, result;
    if (n <= 0) {
        return 0;
    }
    a = n * 2;
    b = n * 3;
    c = n + a;
    d = c + b;
    result = d + recursive_sum_with_locals(n - 1);
    return result + a - b;
}

int main(void) {
    int i, total;

    total = 0;
    i = 0;
    while (i < 26) {
        total = total + fib(i);
        i = i + 1;
    }

    total = total + is_even(40);
    total = total + is_odd(41);
    total = total + fact_acc(10, 1);
    total = total + ackermann_like(2, 3);
    total = total + recursive_sum_with_locals(200);

    if (total != 0) {
        return 0;
    }
    return 1;
}

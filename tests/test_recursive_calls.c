/* Test: recursive Fibonacci (depth stress).
 * Checks prologue/epilogue, callee-saved save/restore, and live values across recursive calls.
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

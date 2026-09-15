/* Test: call with exactly 6 arguments (max in registers, no stack).
 * Checks ABI parameter binding and nested calls forcing result materialization.
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

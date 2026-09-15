/* Test: many live variables inside a loop that contains a call.
 * Forces spills that cross a call (prefer callee-saved) and checks
 * interaction with LICM/SR (helper call is not loop-invariant).
 */

int helper(int x) {
    return x + 1;
}

int main() {
    int a1; int a2; int a3; int a4; int a5; int a6; int a7; int a8;
    int a9; int a10; int a11; int a12; int a13; int a14; int a15; int a16;
    int i;

    a1 = 1; a2 = 2; a3 = 3; a4 = 4; a5 = 5; a6 = 6; a7 = 7; a8 = 8;
    a9 = 9; a10 = 10; a11 = 11; a12 = 12; a13 = 13; a14 = 14; a15 = 15; a16 = 16;
    i = 0;

    while (i < 100) {
        a1 = helper(a1);
        a2 = a2 + a1;
        a3 = a3 + a2;
        a4 = a4 + a3;
        a5 = a5 + a4;
        a6 = a6 + a5;
        a7 = a7 + a6;
        a8 = a8 + a7;
        a9 = a9 + a8;
        a10 = a10 + a9;
        a11 = a11 + a10;
        a12 = a12 + a11;
        a13 = a13 + a12;
        a14 = a14 + a13;
        a15 = a15 + a14;
        a16 = a16 + a15;
        i = i + 1;
    }

    return a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8 +
           a9 + a10 + a11 + a12 + a13 + a14 + a15 + a16;
}

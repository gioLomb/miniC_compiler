// shortcircuit_cp_dce.c
// Target passes: constant propagation (CP), dead code elimination (DCE),
// short-circuit && / || (control flow simplification), plus SVN leftover subexpr.

int side_effect_counter;

int bump(int x) {
    // has visible side effect -> compiler CANNOT drop the call even under DCE,
    // used to verify short-circuit skips calls correctly (correctness check, not just perf)
    side_effect_counter = side_effect_counter + x;
    return x;
}

int shortcircuit_and(int a, int b) {
    // if a==0, bump(b) must NOT execute (short circuit) -> control-flow dependent DCE
    if (a != 0 && bump(b) > 0) {
        return 1;
    }
    return 0;
}

int shortcircuit_or(int a, int b) {
    // if a!=0, bump(b) must NOT execute
    if (a != 0 || bump(b) > 0) {
        return 1;
    }
    return 0;
}

int const_prop_chain(int unused_param) {
int c1;
int c2;
int c3;
int c4;
int result;
int dead1;
int dead2;
    // pure constant chain -> should fold entirely to a single constant (CP)
    c1 = 5;
    c2 = c1 + 3;
    c3 = c2 * 2;
    c4 = c3 - 1;
    result = c4;

    // dead computations: values never used afterwards -> DCE candidates
    dead1 = c1 * c2 * c3 * c4;
    dead2 = dead1 + 999;
    dead2 = dead2 * 2;

    // unreachable-ish branch: condition always false after CP folds c4 == 15
    if (c4 == 999999) {
        result = result + dead2;
    }

    return result;
}

int mixed_cp_dce_svn(int x) {
    int a;
    int b;
    int e;
    int dead;

    a = x * 2 + 1;
    b = x * 2 + 1;
    e = a + b;

    // dead: computed but never read below (DCE target)
    dead = x * x * x;

    // condition with a literal false branch -> whole true-branch dead after CP
    if (0) {
        e = e + dead * 1000;
    }

    return e;
}

int main() {
    int r;

    side_effect_counter = 0;
    r = 0;

    r = r + shortcircuit_and(0, 5);
    r = r + shortcircuit_and(1, 5);
    r = r + shortcircuit_or(1, 5);
    r = r + shortcircuit_or(0, 5);

    r = r + const_prop_chain(0);
    r = r + mixed_cp_dce_svn(7);
    r = r + side_effect_counter;

    if (r != 0) {
        return 0;
    }
    return 1;
}
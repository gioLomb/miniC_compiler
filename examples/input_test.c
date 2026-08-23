#include <stdio.h>

int force_spill(int x);   // dichiarata extern, definita in force_spill.s

int main(void) {
    int r0 = force_spill(0);
    int r1 = force_spill(1);
    int r10 = force_spill(10);

    printf("force_spill(0)  = %d  (atteso 210)\n", r0);
    printf("force_spill(1)  = %d  (atteso 230)\n", r1);
    printf("force_spill(10) = %d  (atteso 410)\n", r10);

    if (r0 == 210 && r1 == 230 && r10 == 410)
        printf("OK\n");
    else
        printf("BUG CONFERMATO\n");

    return 0;
}
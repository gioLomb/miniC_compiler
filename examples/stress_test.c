/* stress_test.c
 *
 * File pensato per generare carico di lavoro sufficiente al profiling
 * (perf/gprof/callgrind), non per testare correttezza di un singolo
 * pattern isolato. Combina:
 *
 *   - ricorsione doppia (fib) -> stress su IR_CALL/IR_PARAM, crossesCall,
 *     salvataggio/ripristino registri.
 *   - loop annidati su array globali/locali -> stress su LICM (base = i*N
 *     e' invariante nel loop interno), SR (moltiplicazioni per costante),
 *     liveness cross-block su CFG piu' articolato.
 *   - funzioni helper chiamate DENTRO loop -> stress su interferenza
 *     vreg-attraverso-CALL (excl[]/crossesCall), non solo a livello foglia.
 *   - blocchi con 20+ variabili vive contemporaneamente -> forza spill,
 *     stress su ra_simplify/ra_select_colors/ra_spill_insert.
 *   - pattern costanti (if(0), while(0), x*1, x+0, 3+2) -> stress su
 *     optimize_ast (constant folding, dead branch elimination) e su
 *     cp.c/dce.c lato IR.
 *   - somme ripetute su array con indice dinamico -> stress su
 *     IR_LOAD_ARR/IR_STORE_ARR e sul loro trattamento (no alias analysis).
 *
 * Con questo file una singola esecuzione del compilatore dovrebbe generare
 * decine/centinaia di istruzioni IR per funzione e diversi round del
 * ciclo di ottimizzazione (SVN->DCE->(CP+DCE)*->LICM+SR->(CP+DCE)*),
 * quindi tempo di esecuzione utile abbastanza alto da non essere
 * sommerso dall'overhead fisso di processo (fork/exec/linking) visto
 * nel profiling precedente.
 */

int globalArr[50];
int globalAccum;

/* ---- Ricorsione doppia: stress su call-stack, crossesCall, IR_PARAM ---- */
int fib(int n) {
    if (n < 2) {
        return n;
    }
    return fib(n - 1) + fib(n - 2);
}

/* ---- Helper chiamato dentro loop: stress su interferenza attraverso CALL ---- */
int helper(int x, int y) {
    int t;
    t = x * 2 + y;
    return t - 1;
}

/* ---- Funzione con molti parametri (6, il massimo via registri ABI) ---- */
int sum6(int a, int b, int c, int d, int e, int f) {
    return a + b + c + d + e + f;
}

/* ---- Costant folding / dead branch elimination: verifica ottimizzatore AST ---- */
int deadCodeAndFolding() {
    int x;
    int y;
    int z;

    x = 3 + 2;          /* fold -> 5 */
    y = x * 1;           /* fold -> x */
    z = y + 0;           /* fold -> y */

    if (0) {
        z = 999;          /* ramo morto, deve sparire */
    } else {
        z = z + 1;
    }

    while (0) {
        z = z + 100;       /* loop morto, deve sparire interamente */
    }

    return z;
}

/* ---- Loop annidati su matrice linearizzata: stress su LICM/SR ---- */
int matrixSumSquares(int n) {
    int i;
    int j;
    int base;
    int idx;
    int total;

    total = 0;
    i = 0;
    while (i < n) {
        base = i * n;              /* invariante nel loop interno: LICM deve issarlo */
        j = 0;
        while (j < n) {
            idx = base + j;
            globalArr[idx % 50] = idx;
            total = total + globalArr[idx % 50] * globalArr[idx % 50];
            j = j + 1;
        }
        i = i + 1;
    }
    return total;
}

/* ---- Loop con chiamata a funzione ad ogni iterazione: stress liveness+CALL ---- */
int accumulateWithCalls(int n) {
    int i;
    int acc;
    int tmp;

    acc = 0;
    i = 0;
    while (i < n) {
        tmp = helper(i, acc);
        acc = acc + tmp;
        i = i + 1;
    }
    return acc;
}

/* ---- Blocco con molte variabili vive insieme: forza spill (> 14 allocabili) ---- */
int manyLiveVars(int seed) {
    int v1;  int v2;  int v3;  int v4;  int v5;  int v6;  int v7;  int v8;
    int v9;  int v10; int v11; int v12; int v13; int v14; int v15; int v16;
    int v17; int v18; int v19; int v20;

    v1  = seed + 1;   v2  = seed + 2;   v3  = seed + 3;   v4  = seed + 4;
    v5  = seed + 5;   v6  = seed + 6;   v7  = seed + 7;   v8  = seed + 8;
    v9  = seed + 9;   v10 = seed + 10;  v11 = seed + 11;  v12 = seed + 12;
    v13 = seed + 13;  v14 = seed + 14;  v15 = seed + 15;  v16 = seed + 16;
    v17 = seed + 17;  v18 = seed + 18;  v19 = seed + 19;  v20 = seed + 20;

    /* tutte vive qui contemporaneamente: forza lo spill */
    return v1+v2+v3+v4+v5+v6+v7+v8+v9+v10+
           v11+v12+v13+v14+v15+v16+v17+v18+v19+v20;
}

/* ---- Catena additiva lunga: stress su balanceAssocChain (ribilanciamento AST) ---- */
int longAddChain(int a, int b, int c, int d, int e, int f) {
    int result;
    result = a + b + c + d + e + f;
    return result;
}

/* ---- Espressioni logiche short-circuit annidate: stress su IR_IF_FALSE/GOTO ---- */
int shortCircuitStress(int a, int b, int c, int d) {
    int r;
    r = 0;
    if ((a > 0 && b > 0) || (c < 0 && d < 0)) {
        r = 1;
    } else {
        if (a == b && c != d) {
            r = 2;
        } else {
            r = 3;
        }
    }
    return r;
}

/* ---- Array con inizializzatore + somma con indice non costante ---- */
int arrayInitAndSum() {
    int local[10];
    int i;
    int total;

    local[0] = 1; local[1] = 2; local[2] = 3; local[3] = 4; local[4] = 5;
    local[5] = 6; local[6] = 7; local[7] = 8; local[8] = 9; local[9] = 10;

    total = 0;
    i = 0;
    while (i < 10) {
        total = total + local[i];
        i = i + 1;
    }
    return total;
}

/* ---- main: orchestratore che tocca tutte le funzioni sopra ---- */
int main() {
    int i;
    int acc;
    int f;

    acc = 0;

    /* ricorsione ripetuta: fib(20) ricalcolato piu' volte per aumentare il carico */
    i = 0;
    while (i < 15) {
        f = fib(18);
        acc = acc + f;
        i = i + 1;
    }

    acc = acc + deadCodeAndFolding();
    acc = acc + matrixSumSquares(7);
    acc = acc + accumulateWithCalls(200);
    acc = acc + manyLiveVars(acc);
    acc = acc + longAddChain(1, 2, 3, 4, 5, 6);
    acc = acc + shortCircuitStress(acc, 1, -1, -2);
    acc = acc + arrayInitAndSum();
    acc = acc + sum6(acc, 1, 2, 3, 4, 5);

    globalAccum = acc;
    return globalAccum;
}

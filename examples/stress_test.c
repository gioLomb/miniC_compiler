/* stress_test.c
 *
 * File pensato per generare carico di lavoro sufficiente al profiling
 * (perf/gprof/callgrind), non per testare correttezza di un singolo
 * pattern isolato. Combina:
 *
 *   - ricorsione doppia (fib) -> stress su IR_CALL/IR_PARAM, crossesCall,
 *     salvataggio/ripristino registri.
 *   - ricorsione mutua (mutualEven/mutualOdd) -> pattern di CALL diverso
 *     dalla self-recursion: due funzioni si richiamano a vicenda, stress
 *     su ordine di salvataggio/ripristino attraverso funzioni distinte.
 *   - loop annidati su array globali/locali -> stress su LICM (base = i*N
 *     e' invariante nel loop interno), SR (moltiplicazioni per costante),
 *     liveness cross-block su CFG piu' articolato.
 *   - loop a 3 livelli -> stress LICM multi-livello (invarianti a piu'
 *     profondita' di nesting issate in punti diversi).
 *   - funzioni helper chiamate DENTRO loop -> stress su interferenza
 *     vreg-attraverso-CALL (excl[]/crossesCall), non solo a livello foglia.
 *   - blocchi con 20+ / 30+ variabili vive contemporaneamente -> forza
 *     spill, stress su ra_simplify/ra_select_colors/ra_spill_insert.
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

/* ---- Ricorsione mutua: stress su pattern di CALL incrociato tra funzioni ---- */
int mutualEven(int n) {
    if (n == 0) {
        return 1;
    }
    return mutualOdd(n - 1);
}

int mutualOdd(int n) {
    if (n == 0) {
        return 0;
    }
    return mutualEven(n - 1);
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

/* ---- Loop a 3 livelli: stress LICM multi-profondita' ---- */
int tripleNestedLoop(int n, int m, int p) {
    int i;
    int j;
    int k;
    int base1;
    int base2;
    int total;

    total = 0;
    i = 0;
    while (i < n) {
        base1 = i * m * p;       /* invariante solo nel loop piu' esterno */
        j = 0;
        while (j < m) {
            base2 = base1 + j * p;  /* invariante nel loop medio, non nel piu' interno */
            k = 0;
            while (k < p) {
                total = total + (base2 + k) % 50;
                k = k + 1;
            }
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

/* ---- Blocco con 32 variabili vive e USO INTERMEDIO: spill ancora piu' duro ----
 * differenza rispetto a manyLiveVars: qui le variabili non sono solo sommate
 * alla fine, ma combinate a coppie in step intermedi, quindi il vreg di ogni
 * var resta live attraverso piu' istruzioni consecutive (grafo interferenza
 * piu' denso, non solo "tutte vive su una riga sola"). */
int extraSpillStress(int seed) {
    int a1;  int a2;  int a3;  int a4;  int a5;  int a6;  int a7;  int a8;
    int a9;  int a10; int a11; int a12; int a13; int a14; int a15; int a16;
    int a17; int a18; int a19; int a20; int a21; int a22; int a23; int a24;
    int a25; int a26; int a27; int a28; int a29; int a30; int a31; int a32;
    int total;

    a1 = seed + 1;   a2 = seed + 2;   a3 = seed + 3;   a4 = seed + 4;
    a5 = seed + 5;   a6 = seed + 6;   a7 = seed + 7;   a8 = seed + 8;
    a9 = seed + 9;   a10 = seed + 10; a11 = seed + 11; a12 = seed + 12;
    a13 = seed + 13; a14 = seed + 14; a15 = seed + 15; a16 = seed + 16;
    a17 = seed + 17; a18 = seed + 18; a19 = seed + 19; a20 = seed + 20;
    a21 = seed + 21; a22 = seed + 22; a23 = seed + 23; a24 = seed + 24;
    a25 = seed + 25; a26 = seed + 26; a27 = seed + 27; a28 = seed + 28;
    a29 = seed + 29; a30 = seed + 30; a31 = seed + 31; a32 = seed + 32;

    /* combinazioni a coppie: tutte le var restano live fino a qui */
    total = (a1*a2) + (a3*a4) + (a5*a6) + (a7*a8) +
            (a9*a10) + (a11*a12) + (a13*a14) + (a15*a16) +
            (a17*a18) + (a19*a20) + (a21*a22) + (a23*a24) +
            (a25*a26) + (a27*a28) + (a29*a30) + (a31*a32);

    return total;
}

/* ---- Loop stretto su array globale: stress puro LOAD_ARR/STORE_ARR, no alias opt ---- */
int denseArrayAccess(int n) {
    int i;
    int total;

    total = 0;
    i = 0;
    while (i < n) {
        globalArr[i % 50] = globalArr[i % 50] + i;   /* read-modify-write stesso slot */
        total = total + globalArr[(i + 1) % 50];      /* read su slot adiacente */
        i = i + 1;
    }
    return total;
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

/* ---- main: orchestratore che tocca tutte le funzioni sopra, piu' volte ---- */
int main() {
    int i;
    int acc;
    int f;

    acc = 0;

    /* ricorsione ripetuta: fib(20) ricalcolato piu' volte per aumentare il carico */
    i = 0;
    while (i < 18) {
        f = fib(20);
        acc = acc + f;
        i = i + 1;
    }

    /* ricorsione mutua ripetuta: pattern di CALL incrociato, piu' round */
    i = 0;
    while (i < 500) {
        acc = acc + mutualEven(30);
        acc = acc + mutualOdd(30);
        i = i + 1;
    }

    acc = acc + deadCodeAndFolding();
    acc = acc + matrixSumSquares(9);
    acc = acc + tripleNestedLoop(6, 6, 6);
    acc = acc + accumulateWithCalls(400);

    i = 0;
    while (i < 20) {
        acc = acc + manyLiveVars(acc);
        acc = acc + extraSpillStress(acc);
        i = i + 1;
    }

    acc = acc + longAddChain(1, 2, 3, 4, 5, 6);
    acc = acc + shortCircuitStress(acc, 1, -1, -2);
    acc = acc + arrayInitAndSum();
    acc = acc + sum6(acc, 1, 2, 3, 4, 5);
    acc = acc + denseArrayAccess(2000);

    globalAccum = acc;
    return globalAccum;
}
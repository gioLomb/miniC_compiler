/* =========================================================
 * Test suite per la Strength Reduction in miniC
 * Ogni funzione testa un caso diverso.
 * ========================================================= */

/* ---- CASO 1: caso base ----------------------------------------
 * t = i * 4 dentro un while.
 * Atteso: t = i*4 diventa addizione t_sr = t_sr + 4 nel loop,
 * inizializzazione t_sr = 0*4 = 0 nel pre-header (poi CP piega a 0).
 * ---------------------------------------------------------------- */
int caso1() {
    int i;
    int t;
    i = 0;
    while (i < 10) {
        t = i * 4;
        i = i + 1;
    }
    return t;
}

/* ---- CASO 2: moltiplicatore piu' grande -----------------------
 * stride = 1 * 100 = 100 ad ogni iterazione.
 * ---------------------------------------------------------------- */
int caso2() {
    int i;
    int offset;
    i = 0;
    while (i < 50) {
        offset = i * 100;
        i = i + 1;
    }
    return offset;
}

/* ---- CASO 3: passo diverso da 1 ------------------------------
 * i incrementa di 2, stride = 2 * 4 = 8.
 * ---------------------------------------------------------------- */
int caso3() {
    int i;
    int t;
    i = 0;
    while (i < 20) {
        t = i * 4;
        i = i + 2;
    }
    return t;
}

/* ---- CASO 4: LICM + SR insieme --------------------------------
 * a+b e' invariante (LICM lo porta fuori),
 * i*4 e' derivata (SR lo converte in addizione).
 * ---------------------------------------------------------------- */
int caso4() {
    int i;
    int a;
    int b;
    int inv;
    int der;
    a = 3;
    b = 5;
    i = 0;
    while (i < 10) {
        inv = a + b;
        der = i * 4;
        i = i + 1;
    }
    return inv + der;
}

/* ---- CASO 5: due derivate dalla stessa induttiva --------------
 * t1 = i * 4  e  t2 = i * 7: entrambe devono essere ridotte.
 * ---------------------------------------------------------------- */
int caso5() {
    int i;
    int t1;
    int t2;
    i = 0;
    while (i < 10) {
        t1 = i * 4;
        t2 = i * 7;
        i = i + 1;
    }
    return t1 + t2;
}

/* ---- CASO 6: induttiva con passo negativo (sottrazione) -------
 * i = i - 1: la SR deve calcolare stride = (-1) * 3 = -3.
 * ---------------------------------------------------------------- */
int caso6() {
    int i;
    int t;
    i = 9;
    while (i > 0) {
        t = i * 3;
        i = i - 1;
    }
    return t;
}

/* ---- CASO 7: SR NON deve applicarsi -------------------------
 * i viene ridefinita due volte nel loop: non e' una induttiva
 * valida (piu' di una definizione), quindi SR non tocca nulla.
 * ---------------------------------------------------------------- */
int caso7() {
    int i;
    int t;
    int cond;
    i = 0;
    while (i < 10) {
        t = i * 4;
        if (cond) {
            i = i + 2;
        }
        i = i + 1;
    }
    return t;
}

/* ---- CASO 8: accesso array (caso pratico tipico) ---------------
 * Il pattern classico che motiva la SR: calcolo dell'offset
 * per accedere a elementi di un array.
 * ---------------------------------------------------------------- */
int caso8() {
    int arr[20];
    int i;
    int s;
    s = 0;
    i = 0;
    while (i < 20) {
        arr[i] = i * 2;
        i = i + 1;
    }
    i = 0;
    while (i < 20) {
        s = s + arr[i];
        i = i + 1;
    }
    return s;
}

int main() {
    return caso1() + caso2() + caso3() + caso4()
         + caso5() + caso6() + caso7() + caso8();
}

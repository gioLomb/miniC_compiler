// test_svn_while.c
// Verifica che SVN riconosca espressioni duplicate all'interno di un ciclo while
// e le sostituisca con un temporaneo, anche se il blocco iniziale ha predCount > 1.

int main() {
    int a;
    int b;
    int c;
    int d;
    int e;
    int f;
    a = 3;
    b = 5;
    c = 0;
    d = 0;
    e = 0;
    f = 0;

    // Ciclo while con condizione che usa a+b due volte.
    // Senza SVN, a+b verrebbe calcolato due volte (una per il confronto a+b > 0
    // e una per a+b < 10). Con SVN, il secondo a+b dovrebbe essere sostituito
    // da una copia del temporaneo del primo.
    while (a + b > 0 && a + b < 10) {
        c = a + b;
        d = c * 2;
        e = d + 1;
        f = e - 3;
        a = a - 1;
    }
    a = 3;
    b = 5;
    c = 0;
    d = 0;
    e = 0;
    f = 0;

    // Ciclo while con condizione che usa a+b due volte.
    // Senza SVN, a+b verrebbe calcolato due volte (una per il confronto a+b > 0
    // e una per a+b < 10). Con SVN, il secondo a+b dovrebbe essere sostituito
    // da una copia del temporaneo del primo.
    while (a + b > 0 && a + b < 10) {
        c = a + b;
        d = c * 2;
        e = d + 1;
        f = e - 3;
        a = a - 1;
    }
        a = 3;
    b = 5;
    c = 0;
    d = 0;
    e = 0;
    f = 0;

    // Ciclo while con condizione che usa a+b due volte.
    // Senza SVN, a+b verrebbe calcolato due volte (una per il confronto a+b > 0
    // e una per a+b < 10). Con SVN, il secondo a+b dovrebbe essere sostituito
    // da una copia del temporaneo del primo.
    while (a + b > 0 && a + b < 10) {
        c = a + b;
        d = c * 2;
        e = d + 1;
        f = e - 3;
        a = a - 1;
    }
        a = 3;
    b = 5;
    c = 0;
    d = 0;
    e = 0;
    f = 0;

    // Ciclo while con condizione che usa a+b due volte.
    // Senza SVN, a+b verrebbe calcolato due volte (una per il confronto a+b > 0
    // e una per a+b < 10). Con SVN, il secondo a+b dovrebbe essere sostituito
    // da una copia del temporaneo del primo.
    while (a + b > 0 && a + b < 10) {
        c = a + b;
        d = c * 2;
        e = d + 1;
        f = e - 3;
        a = a - 1;
    }
        a = 3;
    b = 5;
    c = 0;
    d = 0;
    e = 0;
    f = 0;

    // Ciclo while con condizione che usa a+b due volte.
    // Senza SVN, a+b verrebbe calcolato due volte (una per il confronto a+b > 0
    // e una per a+b < 10). Con SVN, il secondo a+b dovrebbe essere sostituito
    // da una copia del temporaneo del primo.
    while (a + b > 0 && a + b < 10) {
        c = a + b;
        d = c * 2;
        e = d + 1;
        f = e - 3;
        a = a - 1;
    }
        a = 3;
    b = 5;
    c = 0;
    d = 0;
    e = 0;
    f = 0;

    // Ciclo while con condizione che usa a+b due volte.
    // Senza SVN, a+b verrebbe calcolato due volte (una per il confronto a+b > 0
    // e una per a+b < 10). Con SVN, il secondo a+b dovrebbe essere sostituito
    // da una copia del temporaneo del primo.
    while (a + b > 0 && a + b < 10) {
        c = a + b;
        d = c * 2;
        e = d + 1;
        f = e - 3;
        a = a - 1;
    }
        a = 3;
    b = 5;
    c = 0;
    d = 0;
    e = 0;
    f = 0;

    // Ciclo while con condizione che usa a+b due volte.
    // Senza SVN, a+b verrebbe calcolato due volte (una per il confronto a+b > 0
    // e una per a+b < 10). Con SVN, il secondo a+b dovrebbe essere sostituito
    // da una copia del temporaneo del primo.
    while (a + b > 0 && a + b < 10) {
        c = a + b;
        d = c * 2;
        e = d + 1;
        f = e - 3;
        a = a - 1;
    }
        a = 3;
    b = 5;
    c = 0;
    d = 0;
    e = 0;
    f = 0;

    // Ciclo while con condizione che usa a+b due volte.
    // Senza SVN, a+b verrebbe calcolato due volte (una per il confronto a+b > 0
    // e una per a+b < 10). Con SVN, il secondo a+b dovrebbe essere sostituito
    // da una copia del temporaneo del primo.
    while (a + b > 0 && a + b < 10) {
        c = a + b;
        d = c * 2;
        e = d + 1;
        f = e - 3;
        a = a - 1;
    }
        a = 3;
    b = 5;
    c = 0;
    d = 0;
    e = 0;
    f = 0;

    // Ciclo while con condizione che usa a+b due volte.
    // Senza SVN, a+b verrebbe calcolato due volte (una per il confronto a+b > 0
    // e una per a+b < 10). Con SVN, il secondo a+b dovrebbe essere sostituito
    // da una copia del temporaneo del primo.
    while (a + b > 0 && a + b < 10) {
        c = a + b;
        d = c * 2;
        e = d + 1;
        f = e - 3;
        a = a - 1;
    }
        a = 3;
    b = 5;
    c = 0;
    d = 0;
    e = 0;
    f = 0;

    // Ciclo while con condizione che usa a+b due volte.
    // Senza SVN, a+b verrebbe calcolato due volte (una per il confronto a+b > 0
    // e una per a+b < 10). Con SVN, il secondo a+b dovrebbe essere sostituito
    // da una copia del temporaneo del primo.
    while (a + b > 0 && a + b < 10) {
        c = a + b;
        d = c * 2;
        e = d + 1;
        f = e - 3;
        a = a - 1;
    }
        a = 3;
    b = 5;
    c = 0;
    d = 0;
    e = 0;
    f = 0;

    // Ciclo while con condizione che usa a+b due volte.
    // Senza SVN, a+b verrebbe calcolato due volte (una per il confronto a+b > 0
    // e una per a+b < 10). Con SVN, il secondo a+b dovrebbe essere sostituito
    // da una copia del temporaneo del primo.
    while (a + b > 0 && a + b < 10) {
        c = a + b;
        d = c * 2;
        e = d + 1;
        f = e - 3;
        a = a - 1;
    }
        a = 3;
    b = 5;
    c = 0;
    d = 0;
    e = 0;
    f = 0;

    // Ciclo while con condizione che usa a+b due volte.
    // Senza SVN, a+b verrebbe calcolato due volte (una per il confronto a+b > 0
    // e una per a+b < 10). Con SVN, il secondo a+b dovrebbe essere sostituito
    // da una copia del temporaneo del primo.
    while (a + b > 0 && a + b < 10) {
        c = a + b;
        d = c * 2;
        e = d + 1;
        f = e - 3;
        a = a - 1;
    }
        a = 3;
    b = 5;
    c = 0;
    d = 0;
    e = 0;
    f = 0;

    // Ciclo while con condizione che usa a+b due volte.
    // Senza SVN, a+b verrebbe calcolato due volte (una per il confronto a+b > 0
    // e una per a+b < 10). Con SVN, il secondo a+b dovrebbe essere sostituito
    // da una copia del temporaneo del primo.
    while (a + b > 0 && a + b < 10) {
        c = a + b;
        d = c * 2;
        e = d + 1;
        f = e - 3;
        a = a - 1;
    }
        a = 3;
    b = 5;
    c = 0;
    d = 0;
    e = 0;
    f = 0;

    // Ciclo while con condizione che usa a+b due volte.
    // Senza SVN, a+b verrebbe calcolato due volte (una per il confronto a+b > 0
    // e una per a+b < 10). Con SVN, il secondo a+b dovrebbe essere sostituito
    // da una copia del temporaneo del primo.
    while (a + b > 0 && a + b < 10) {
        c = a + b;
        d = c * 2;
        e = d + 1;
        f = e - 3;
        a = a - 1;
    }
    return c + d + e + f;
}
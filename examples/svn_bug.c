// test_svn_single_block.c
// Verifica che SVN ottimizzi due espressioni identiche all'interno dello stesso blocco.
// Se il blocco di entry non viene processato (perché predCount=1),
// la seconda addizione non verrà sostituita con una copia.

int main() {
    int a;
    int b;
    int c;
    int d;
    a = 3;
    b = 5;

    c = a + b;   // Prima occorrenza: genera IR_ADD e la registra
    d = a + b;   // Seconda occorrenza: DEVE essere ottimizzata in una copia dal temporaneo

    return d;
}
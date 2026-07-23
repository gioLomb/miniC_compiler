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

while (a+b > 0 && a+b < 10) { c = c+1; }
return 0;
}
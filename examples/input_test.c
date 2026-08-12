// test_licm_position.c
// Verifica che LICM posizioni le istruzioni invarianti nel pre-header
// nell'ordine corretto rispetto al flusso logico.
// Il vecchio LICM le metteva in posizione 0, prima di tutto.
// Il nuovo LICM le mette prima dell'header del loop (dopo le inizializzazioni).

int main() {
    int a = 3;
    int b = 5;
    int i = 1;
    int t = 0;

    // a e b non cambiano nel loop, quindi a+b è invariante.
    // LICM dovrebbe hoistare t = a + b fuori dal loop.
    i=0;
    while(i < 10) {
        t = a + b;
        i = i + 1;
    }

    // i = 5;
    // if(i<10){
    //     return a;
    // }

    return a;
}
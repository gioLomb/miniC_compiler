/* 
 * Questo scenario simula il posizionamento errato del pre-header.
 * Se l'ottimizzatore piazza l'inizializzazione del temporaneo SR 
 * DOPO il ciclo (o in fondo alla funzione), il ciclo userà spazzatura.
 */
int test_bug_inizializzazione() {
    int i;
    int t;
    
    i = 0;
    // Un ciclo banalissimo. La SR intercetta t = i * 8.
    // Genererà un temporaneo, es: t_sr.
    // Se t_sr = 0 viene piazzato in fondo alla funzione per un errore di indice,
    // alla prima iterazione 't_sr' non conterrà 0, ma il valore residuo dello stack.
    while (i < 5) {
        t = i * 8; 
        i = i + 1;
    }
    return t;
}

/* 
 * Scenario 2: Cicli multipli e dipendenze nascoste (DCE Trigger)
 * Qui usiamo lo stesso temporaneo o la stessa variabile in due cicli vicini.
 * Se il pre-header del secondo ciclo si sovrappone o viene iniettato
 * in una posizione fisica errata (es. prima del reset i = 0),
 * il comportamento salta completamente.
 */
int test_bug_cicli_multipli() {
    int i;
    int t1;
    int t2;
    
    // Primo ciclo
    i = 0;
    while (i < 5) {
        t1 = i * 4;
        i = i + 1;
    }
    
    // Secondo ciclo: reset di i
    // Se il pre-header di questo ciclo viene calcolato male e piazzato 
    // PRIMA di "i = 0", l'inizializzazione della SR leggerà il valore di 'i' 
    // rimasto dal ciclo precedente (cioè 5), calcolando 5 * 4 = 20 invece di 0!
    i = 0; 
    while (i < 5) {
        t2 = i * 4;
        i = i + 1;
    }
    
    return t1 + t2;
}

int main() {
    int res1 = test_bug_inizializzazione();
    int res2 = test_bug_cicli_multipli();
    
    // Valori attesi matematicamente se il compilatore è corretto:
    // test_bug_inizializzazione: all'ultima iterazione i=4 -> 4 * 8 = 32

    
    if (res1 != 32 || res2 != 32) {
        return 1;
    }
    
    return 0;
}
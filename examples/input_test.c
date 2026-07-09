// MiniC Test Program: Calcolo del fattoriale e verifica tipi
int main() {
    int n;
    int i;
    int fattoriale;
    float tasso_variazione;
    float soglia;

    n = 10;
    fattoriale = i = 1;
    tasso_variazione = 0.75;
    soglia = 0.005;

    /* Ciclo di controllo principale
       Verifica gli operatori logici e relazionali */
    while (i <= n && fattoriale != 0) {
        fattoriale = fattoriale * i
        i = i + 1;
    }

    if(fattoriale <= 2){
        return 1;
    }
    if (fattoriale >= 5000 || tasso_variazione < soglia) {
        return 1;
    } else {
        return 0;
    }
}
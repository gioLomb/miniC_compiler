#ifndef ERROR_H
#define ERROR_H

/* Segnala un errore di sintassi alla riga indicata. Se e' gia' stato
   segnalato un errore per lo statement corrente (vedi hadAnyError),
   il messaggio viene soppresso: evita la cascata di messaggi duplicati
   che si genera quando l'errore si propaga attraverso piu' livelli
   della gerarchia di parsing (Expr -> ... -> Factor) prima di essere
   gestito da chi puo' fare recovery. */
void reportError(int line, const char *fmt, ...);

/* true se e' stato segnalato un errore non ancora gestito da un recovery */
int hadAnyError(void);

/* segnala che il recovery e' stato completato: pronti a riportare
   (e a lasciar propagare) il prossimo errore */
void clearError(void);

/* numero totale di errori riportati nell'intera compilazione (mai azzerato):
   usato da main() per decidere l'exit code finale */
int totalErrorCount(void);

#endif

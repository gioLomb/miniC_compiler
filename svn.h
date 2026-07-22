#ifndef SVN_H
#define SVN_H

#include "ir.h"

/*
 * Superlocal Value Numbering (SVN) sull'IR lineare, eseguita da
 * ir_generate() subito dopo la costruzione del CFG di ogni funzione
 * (vedi resolveCFG()/irFunction() in ir.c). Riconosce e riscrive come
 * semplice copia ogni ricalcolo di un'espressione gia' vista lungo lo
 * stesso cammino di esecuzione:
 *
 *     t1 = a + b
 *     ...
 *     t2 = a + b        ->   t2 = t1
 *
 * "Stesso cammino" e' l'estensione di un blocco base (EBB, Extended
 * Basic Block): la visita segue senza interruzioni ogni catena di
 * blocchi con un solo predecessore, e riparte da zero (nessuna
 * conoscenza ereditata) ad ogni punto di confluenza - un blocco con
 * piu' di un arco entrante puo' essere raggiunto da cammini diversi, che
 * potrebbero non aver eseguito la stessa espressione, quindi propagare
 * lì un valore visto su un solo ramo sarebbe scorretto.
 *
 * Il problema del leader stantio (perche' serve un controllo di
 * validita' AL MOMENTO DELL'USO, non solo alla creazione)
 * ---------------------------------------------------------------------
 * Con la generazione a destinazione diretta (irExprInto), il risultato
 * di un'espressione puo' finire direttamente in una variabile con nome,
 * non solo in un temporaneo ("cane = a+b" scrive "cane", non un "t").
 * Se quella variabile viene registrata come "leader" del valore a+b, e
 * piu' avanti lungo lo stesso EBB viene riassegnata ("cane = 99;"), il
 * leader registrato punterebbe a una variabile che non contiene piu'
 * quel valore. I temporanei non hanno questo problema (assegnati una
 * volta sola per costruzione); le variabili si'.
 *
 * La soluzione qui NON e' invalidare il leader alla riassegnazione
 * (mutare la lista di nomi di uno scope antenato romperebbe l'invariante
 * "sheaf": un ramo fratello non ancora esplorato vedrebbe un cambiamento
 * che non gli appartiene). Ogni nome resta nella sua lista per sempre;
 * viene scartato per filtraggio, a costo zero, nel momento in cui
 * servirebbe come leader (nameStillValid/findValidLeader), confrontando
 * il valore CORRENTE della variabile (da 'values', gia' scoping-aware)
 * con il valore per cui era stata registrata.
 *
 * Limiti noti (occasioni mancate, non bug di correttezza)
 * ---------------------------------------------------------------------
 * - IR_LOAD_ARR e IR_CALL non vengono mai memoizzate: senza analisi
 *   degli alias, due arr[i] non sono provatamente lo stesso valore se in
 *   mezzo c'e' una IR_STORE_ARR (anche su un array diverso, staticamente
 *   indistinguibile qui); una IR_CALL puo' avere effetti collaterali o
 *   restituire valori diversi a ogni chiamata. Entrambe ricevono un
 *   numero di valore fresco, mai riusabile altrove.
 * - Al massimo SVN_MAX_NAMES nomi tracciati per valore (vedi svn.c):
 *   oltre quel numero, l'unificazione su nomi aggiuntivi viene persa
 *   silenziosamente (nessun rischio di scorrettezza).
 * - La copia introdotta ("t2 = t1") non viene ulteriormente propagata:
 *   la copy propagation e' rimandata a un passo successivo.
 */
void svn_optimize(IRFunction *f);

#endif

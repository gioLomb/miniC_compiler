#ifndef BLOCK_H
#define BLOCK_H

/*
 * BasicBlock: campi comuni a tutti i blocchi base del compilatore.
 *
 * Usata direttamente dove prima esistevano RBlock (regalloc_utils.h)
 * e LivenessBlock (liveness.h), che sono stati rimossi in favore di
 * questo tipo unico. IRBlock la incorpora come campo nominato 'bb'
 * aggiungendo predCount.
 */
typedef struct {
    int start;      /* indice prima istruzione del blocco (incluso)  */
    int end;        /* indice prima istruzione del blocco dopo (escluso) */
    int succ[2];    /* successori nel CFG; -1 = assente              */
} BasicBlock;

#endif /* BLOCK_H */
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "../symbol_table.h"

int main(void) {
    /* --- PASS 1: scope globale, registra le signature delle funzioni ---
       (main() chiama somma() anche se somma e' "dichiarata dopo": questo
       e' esattamente il caso di forward reference che la strategia a
       due passate risolve) */
    Scope *global = scope_create(NULL);

    Symbol sommaSym = {0};
    sommaSym.kind = SYM_FUNC;
    sommaSym.dataType = T_INT;              /* tipo di ritorno */
    sommaSym.paramCount = 2;
    symtab_pack_param_type(&sommaSym.paramTypes, 0, T_INT);
    symtab_pack_param_type(&sommaSym.paramTypes, 1, T_INT);
    assert(symtab_declare(global, "somma", &sommaSym) == 1);

    Symbol mainSym = {0};
    mainSym.kind = SYM_FUNC;
    mainSym.dataType = T_INT;
    mainSym.paramCount = 0;
    assert(symtab_declare(global, "main", &mainSym) == 1);

    /* redeclaration nello stesso scope: deve fallire */
    assert(symtab_declare(global, "somma", &sommaSym) == 0);
    printf("PASS 1 ok: funzioni globali registrate, redeclaration rifiutata.\n");

    /* --- PASS 2: entriamo nel corpo di main() --- */
    Scope *mainScope = scope_create(global);

    Symbol xSym = {0};
    xSym.kind = SYM_VAR;
    xSym.dataType = T_INT;
    xSym.offset = 0;
    assert(symtab_declare(mainScope, "x", &xSym) == 1);

    /* lookup di 'somma' da dentro mainScope: risale fino a 'global' */
    Symbol found;
    assert(symtab_lookup(mainScope, "somma", &found) == 1);
    assert(found.kind == SYM_FUNC);
    assert(found.paramCount == 2);
    assert(symtab_unpack_param_type(found.paramTypes, 0) == T_INT);
    assert(symtab_unpack_param_type(found.paramTypes, 1) == T_INT);
    printf("PASS 2 ok: lookup di 'somma' risale correttamente da mainScope a global, "
           "paramTypes decompattati correttamente.\n");

    /* --- Scope annidato: un blocco 'if' dentro main(), con una
       variabile locale 'x' che fa SHADOWING di quella di main() --- */
    Scope *ifScope = scope_create(mainScope);

    Symbol innerX = {0};
    innerX.kind = SYM_VAR;
    innerX.dataType = T_FLOAT;
    innerX.offset = 4;
    assert(symtab_declare(ifScope, "x", &innerX) == 1);   /* NON e' redeclaration:
                                                              scope diverso */

    /* il lookup da dentro ifScope deve trovare la 'x' PIU' VICINA (float,
       quella del blocco if), non quella di main() (int) */
    assert(symtab_lookup(ifScope, "x", &found) == 1);
    assert(found.dataType == T_FLOAT);
    printf("PASS 3 ok: shadowing corretto, 'x' interna (float) nasconde quella esterna (int).\n");

    /* usciamo dal blocco if: la 'x' di main() torna visibile */
    Scope *backToMain = scope_exit(ifScope);
    assert(backToMain == mainScope);
    assert(symtab_lookup(backToMain, "x", &found) == 1);
    assert(found.dataType == T_INT);
    printf("PASS 4 ok: uscendo dal blocco, torna visibile la 'x' di main() (int).\n");

    /* nome mai dichiarato in nessuno scope: deve fallire */
    assert(symtab_lookup(ifScope, "nonEsiste", &found) == 0);
    printf("PASS 5 ok: lookup di un nome inesistente fallisce correttamente.\n");

    /* --- Verifica dedicata del packing: 16 parametri, tutti e 3 i tipi --- */
    Symbol manyParams = {0};
    manyParams.kind = SYM_FUNC;
    manyParams.dataType = T_VOID;
    manyParams.paramCount = SYM_MAX_PARAMS;
    for (int i = 0; i < SYM_MAX_PARAMS; i++) {
        DataType t = (DataType)(i % 3);   /* alterna T_INT/T_FLOAT/T_VOID */
        symtab_pack_param_type(&manyParams.paramTypes, i, t);
    }
    for (int i = 0; i < SYM_MAX_PARAMS; i++) {
        DataType expected = (DataType)(i % 3);
        assert(symtab_unpack_param_type(manyParams.paramTypes, i) == expected);
    }
    printf("PASS 6 ok: packing/unpacking corretto su tutti i %d parametri (2 bit ciascuno).\n",
           SYM_MAX_PARAMS);

    /* cleanup finale: distrugge l'intero albero a partire dalla radice */
    symtab_destroy_tree(global);
    printf("\nTutti i test sono passati. Cleanup completato senza errori.\n");

    return 0;
}

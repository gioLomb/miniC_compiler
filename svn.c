#include <stdlib.h>
#include <string.h>
#include "svn.h"
#include "hash_table.h"

/* ---- Grafo dei blocchi (minimo indispensabile: successori + predCount) */

typedef struct {
    int start, end;    /* range [start, end) in f->instrs */
    int succ[2];       /* indici di blocco, -1 se assente */
    int predCount;
} Block;

static int isTerminator(IROp op) {
    return op == IR_GOTO || op == IR_IF_FALSE || op == IR_RETURN;
}

static int findBlockStartingWithLabel(IRFunction *f, Block *blocks, int nBlocks, int labelId) {
    for (int b = 0; b < nBlocks; b++) {
        IRInstr *first = &f->instrs[blocks[b].start];
        if (first->op == IR_LABEL && first->dst.as.labelId == labelId) return b;
    }
    return -1;   /* non dovrebbe succedere: ogni bersaglio di salto e'
                    generato insieme alla sua LABEL da ir_generate */
}

static Block *buildBlocks(IRFunction *f, int *outCount) {
    int n = f->count;
    if (n == 0) { *outCount = 0; return NULL; }

    int *isStart = calloc((size_t) n, sizeof(int));
    isStart[0] = 1;
    for (int i = 0; i < n; i++) {
        if (f->instrs[i].op == IR_LABEL) isStart[i] = 1;
        if (isTerminator(f->instrs[i].op) && i + 1 < n) isStart[i + 1] = 1;
    }

    int nBlocks = 0;
    for (int i = 0; i < n; i++) if (isStart[i]) nBlocks++;

    Block *blocks = malloc((size_t) nBlocks * sizeof(Block));
    int b = -1;
    for (int i = 0; i < n; i++) {
        if (isStart[i]) {
            b++;
            blocks[b].start = i;
            blocks[b].succ[0] = blocks[b].succ[1] = -1;
            blocks[b].predCount = 0;
        }
        blocks[b].end = i + 1;
    }
    free(isStart);

    for (int bi = 0; bi < nBlocks; bi++) {
        int last = blocks[bi].end - 1;
        IROp op = f->instrs[last].op;
        if (op == IR_GOTO) {
            blocks[bi].succ[0] = findBlockStartingWithLabel(f, blocks, nBlocks, f->instrs[last].dst.as.labelId);
        } else if (op == IR_IF_FALSE) {
            blocks[bi].succ[0] = (bi + 1 < nBlocks) ? bi + 1 : -1;   /* fallthrough */
            blocks[bi].succ[1] = findBlockStartingWithLabel(f, blocks, nBlocks, f->instrs[last].dst.as.labelId);
        } else if (op != IR_RETURN) {
            blocks[bi].succ[0] = (bi + 1 < nBlocks) ? bi + 1 : -1;   /* fallthrough semplice (es. prima di una LABEL) */
        }
    }

    for (int bi = 0; bi < nBlocks; bi++) {
        for (int k = 0; k < 2; k++) {
            int s = blocks[bi].succ[k];
            if (s >= 0) blocks[s].predCount++;
        }
    }

    *outCount = nBlocks;
    return blocks;
}

/* ---- Sheaf di tabelle -------------------------------------------------- */

typedef struct SVNScope {
    Hash_Table *values;    /* ValueKey -> vn (int): cosa contiene ADESSO questa identita' */
    Hash_Table *exprs;     /* ExprKey  -> vn (int): questa espressione e' gia' stata calcolata? */
    Hash_Table *leaders;   /* vn (int) -> NameList: tutti i nomi che hanno MAI contenuto questo valore */
    struct SVNScope *parent;
} SVNScope;

typedef struct {
    int kind;          /* 0=var, 1=temp, 2=constInt, 3=constFloat */
    long intVal;
    double floatVal;
    int a, b;          /* kind==0: level,offset; kind==1: tempId,0 */
} ValueKey;

typedef struct {
    int op;
    int vn1, vn2;       /* vn2 = -1 per gli operatori unari */
} ExprKey;

/* Mappa valori-nomi "attiva" (vedi svn.h): invece di un unico leader
 * fisso, ogni numero di valore porta con se' TUTTI i nomi che l'hanno
 * mai contenuto lungo il cammino corrente. Non c'e' una vera rimozione
 * alla ridefinizione (mutare la lista di uno scope antenato romperebbe
 * l'invariante della sheaf - un ramo fratello non ancora esplorato
 * vedrebbe un cambiamento che non gli appartiene): la "rimozione" e'
 * ottenuta a costo zero, per filtraggio, al momento della lettura (vedi
 * nameStillValid) - un nome resta nella lista per sempre, ma viene
 * scartato se il suo valore ATTUALE (in 'values', gia' correttamente
 * scoping-aware) non coincide piu' con il vn per cui e' stato inserito.
 * Capacita' fissa (4): un limite dichiarato, non un'illusione di
 * generalita' - oltre la quarta variabile che condivide lo stesso valore
 * smettiamo di tracciarne altre (nessun rischio di scorrettezza, solo
 * un'eventuale occasione di ottimizzazione mancata in un caso raro). */
#define SVN_MAX_NAMES 4
typedef struct {
    int count;
    Operand names[SVN_MAX_NAMES];
} NameList;

static unsigned long svnHash(const void *key, size_t keySize) {
    const unsigned char *bytes = key;
    unsigned long h = 2166136261UL;
    for (size_t i = 0; i < keySize; i++) { h ^= bytes[i]; h *= 16777619UL; }
    return h;
}

static SVNScope *svnScopeCreate(SVNScope *parent) {
    SVNScope *s = malloc(sizeof(SVNScope));
    s->values  = ht_create(31, svnHash);
    s->exprs   = ht_create(31, svnHash);
    s->leaders = ht_create(31, svnHash);
    s->parent  = parent;
    return s;
}

static void svnScopeDestroy(SVNScope *s) {
    ht_destroy(s->values, NULL);
    ht_destroy(s->exprs, NULL);
    ht_destroy(s->leaders, NULL);
    free(s);
}

static Operand mkNone(void) {
    Operand o; o.kind = OPND_NONE; return o;
}

static ValueKey keyForOperand(Operand op) {
    ValueKey k = {0};
    switch (op.kind) {
    case OPND_VAR:         k.kind = 0; k.a = op.as.var.level; k.b = op.as.var.offset; break;
    case OPND_TEMP:        k.kind = 1; k.a = op.as.tempId; break;
    case OPND_CONST_INT:   k.kind = 2; k.intVal = op.as.intVal; break;
    case OPND_CONST_FLOAT: k.kind = 3; k.floatVal = op.as.floatVal; break;
    default:               k.kind = -1; break;   /* NONE/LABEL/FUNC: non pertinente */
    }
    return k;
}

/* Aggiunge 'name' alla lista dei nomi che contengono 'vn': legge la lista
 * gia' esistente risalendo la catena (se 'vn' e' stato creato in uno
 * scope antenato, la sua lista vive li'), la estende, e la riscrive
 * SEMPRE nello scope locale - non modifica mai la tabella del genitore,
 * la "ombreggia" per questo ramo in avanti, esattamente come 'values'. */
static void addNameForValue(int vn, Operand name, SVNScope *scope) {
    NameList list = {0};
    for (SVNScope *s = scope; s; s = s->parent) {
        if (ht_get(s->leaders, &vn, sizeof(vn), &list, sizeof(list))) break;
    }
    if (list.count < SVN_MAX_NAMES) {
        list.names[list.count++] = name;
    }
    ht_set(scope->leaders, &vn, sizeof(vn), &list, sizeof(list));
}

/* Registra che 'dst' ora contiene il valore 'vn' (SEMPRE nello scope
 * locale, mai nel genitore - una scrittura deve "coprire" ogni voce
 * precedente per questa identita' nei blocchi discendenti) e aggiunge
 * 'dst' come nuovo nome disponibile per 'vn'. E' l'unico punto che
 * aggiorna sia 'values' sia 'leaders' per una destinazione: se 'dst' non
 * e' una variabile/temporaneo (istruzioni senza vera destinazione
 * scalare: IF_FALSE/GOTO/LABEL/STORE_ARR/PARAM/RETURN) non fa nulla. */
static void defineValue(Operand dst, int vn, SVNScope *scope) {
    if (dst.kind != OPND_VAR && dst.kind != OPND_TEMP) return;
    ValueKey k = keyForOperand(dst);
    ht_set(scope->values, &k, sizeof(k), &vn, sizeof(vn));
    addNameForValue(vn, dst, scope);
}

/* Vero se 'name' contiene ANCORA il valore 'vn' in questo punto del
 * cammino: le costanti e i temporanei lo sono sempre (mai riassegnati:
 * le costanti per natura, i temporanei per costruzione in ir.c); una
 * variabile lo e' solo se il suo valore CORRENTE (da 'values', non dalla
 * lista stessa) e' ancora 'vn'. */
static int nameStillValid(Operand name, int vn, SVNScope *scope) {
    if (name.kind != OPND_VAR) return 1;
    ValueKey k = keyForOperand(name);
    for (SVNScope *s = scope; s; s = s->parent) {
        int currentVN;
        if (ht_get(s->values, &k, sizeof(k), &currentVN, sizeof(currentVN))) return currentVN == vn;
    }
    return 0;
}

/* Cerca, tra tutti i nomi mai associati a 'vn', il primo ancora valido.
 * Se nessuno lo e' (tutti ridefiniti nel frattempo), il valore non e'
 * piu' disponibile da nessuna parte: bisogna ricalcolarlo davvero. */
static int findValidLeader(int vn, SVNScope *scope, Operand *outLeader) {
    NameList list = {0};
    int haveList = 0;
    for (SVNScope *s = scope; s; s = s->parent) {
        if (ht_get(s->leaders, &vn, sizeof(vn), &list, sizeof(list))) { haveList = 1; break; }
    }
    if (!haveList) return 0;

    for (int i = 0; i < list.count; i++) {
        if (nameStillValid(list.names[i], vn, scope)) {
            *outLeader = list.names[i];
            return 1;
        }
    }
    return 0;
}

/* Risolve il numero di valore CORRENTE di 'op', risalendo la catena. Se
 * e' la prima volta che questa identita' viene letta lungo il cammino,
 * le assegna un numero fresco, registrato localmente insieme a se' stessa
 * come primo nome disponibile per quel valore. */
static int valueNumberOf(Operand op, SVNScope *scope, int *nextVN) {
    ValueKey k = keyForOperand(op);
    if (k.kind < 0) return -1;

    int vn;
    for (SVNScope *s = scope; s; s = s->parent) {
        if (ht_get(s->values, &k, sizeof(k), &vn, sizeof(vn))) return vn;
    }

    vn = (*nextVN)++;
    ht_set(scope->values, &k, sizeof(k), &vn, sizeof(vn));
    addNameForValue(vn, op, scope);
    return vn;
}

static int isCommutative(IROp op) {
    return op == IR_ADD || op == IR_MUL || op == IR_EQ || op == IR_NE;
}

/* Cerca 'ek' nella catena; se trovata E almeno un nome che la contiene e'
 * ancora valido, riscrive 'in' come una copia da quel nome. Altrimenti
 * registra 'ek' come nuova espressione, con 'in->dst' come suo primo
 * nome. In entrambi i casi aggiorna il valore corrente di 'in->dst'. */
static void memoizeOrRewrite(IRInstr *in, ExprKey ek, SVNScope *scope, int *nextVN) {
    int exprVN;
    int found = 0;
    for (SVNScope *s = scope; s; s = s->parent) {
        if (ht_get(s->exprs, &ek, sizeof(ek), &exprVN, sizeof(exprVN))) { found = 1; break; }
    }

    Operand leader;
    if (found && findValidLeader(exprVN, scope, &leader)) {
        in->op = IR_ASSIGN;
        in->src1 = leader;
        in->src2 = mkNone();
        defineValue(in->dst, exprVN, scope);
        return;
    }

    /* non trovata, oppure trovata ma nessun nome sopravvissuto valido:
       (ri)registra 'ek' come se fosse nuova, con 'in->dst' come nome */
    exprVN = (*nextVN)++;
    ht_set(scope->exprs, &ek, sizeof(ek), &exprVN, sizeof(exprVN));
    defineValue(in->dst, exprVN, scope);
}

static void svnProcessInstr(IRInstr *in, SVNScope *scope, int *nextVN) {
    switch (in->op) {

    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
    case IR_LT:  case IR_LE:  case IR_GT:  case IR_GE:  case IR_EQ: case IR_NE: {
        int vn1 = valueNumberOf(in->src1, scope, nextVN);
        int vn2 = valueNumberOf(in->src2, scope, nextVN);
        if (isCommutative(in->op) && vn1 > vn2) { int t = vn1; vn1 = vn2; vn2 = t; }
        ExprKey ek = {0};
        ek.op = (int) in->op; ek.vn1 = vn1; ek.vn2 = vn2;
        memoizeOrRewrite(in, ek, scope, nextVN);
        break;
    }

    case IR_NEG: case IR_NOT: {
        int vn1 = valueNumberOf(in->src1, scope, nextVN);
        ExprKey ek = {0};
        ek.op = (int) in->op; ek.vn1 = vn1; ek.vn2 = -1;
        memoizeOrRewrite(in, ek, scope, nextVN);
        break;
    }

    case IR_ASSIGN: {
        int vn1 = valueNumberOf(in->src1, scope, nextVN);
        defineValue(in->dst, vn1, scope);   /* dst diventa anche un nome valido per vn1 */
        break;
    }

    case IR_LOAD_ARR:
    case IR_CALL: {
        /* non memoizzate (vedi limiti in svn.h): la destinazione riceve
           comunque un'identita' di valore fresca, mai riusabile altrove. */
        int fresh = (*nextVN)++;
        defineValue(in->dst, fresh, scope);
        break;
    }

    case IR_STORE_ARR:
    case IR_PARAM:
    case IR_RETURN:
    case IR_GOTO:
    case IR_IF_FALSE:
    case IR_LABEL:
        break;   /* nessuna destinazione scalare, nessuna espressione da memoizzare */
    }
}

/* ---- Visita: ricorsione lungo l'EBB + worklist ai punti di confluenza -- */

static void svnProcessBlock(IRFunction *f, Block *blocks, int blockIdx, SVNScope *parent,
                             int *nextVN, int *visited, int *queue, int *qTail) {
    SVNScope *scope = svnScopeCreate(parent);

    for (int i = blocks[blockIdx].start; i < blocks[blockIdx].end; i++) {
        svnProcessInstr(&f->instrs[i], scope, nextVN);
    }

    for (int k = 0; k < 2; k++) {
        int s = blocks[blockIdx].succ[k];
        if (s < 0) continue;
        if (blocks[s].predCount == 1) {
            svnProcessBlock(f, blocks, s, scope, nextVN, visited, queue, qTail);
        } else if (!visited[s]) {
            visited[s] = 1;
            queue[(*qTail)++] = s;
        }
    }

    svnScopeDestroy(scope);
}

void svn_optimize(IRFunction *f) {
    int nBlocks;
    Block *blocks = buildBlocks(f, &nBlocks);
    if (nBlocks == 0) { free(blocks); return; }

    int nextVN = 0;
    int *visited = calloc((size_t) nBlocks, sizeof(int));
    int *queue = malloc((size_t) nBlocks * sizeof(int));
    int qHead = 0, qTail = 0;

    queue[qTail++] = 0;
    visited[0] = 1;
    while (qHead < qTail) {
        int leader = queue[qHead++];
        svnProcessBlock(f, blocks, leader, NULL, &nextVN, visited, queue, &qTail);
    }

    free(visited);
    free(queue);
    free(blocks);
}
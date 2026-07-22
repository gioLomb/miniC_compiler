#include <stdlib.h>
#include <string.h>
#include "svn.h"
#include "hash_table.h"

/* ---- Sheaf di tabelle ---------------------------------------------------
 *
 * Uno SVNScope per blocco, creato e distrutto una volta per blocco: vive
 * come variabile automatica dentro svnProcessBlock (vedi sotto), non su
 * arena ne' su heap - la sua durata di vita combacia esattamente con lo
 * stack frame che lo crea, e nessun figlio lo consulta dopo che quello
 * stack frame e' ritornato. Le 3 hash table interne restano sull'heap
 * (ht_create/ht_destroy fanno le loro malloc/free, la libreria non sa
 * nulla di stack o arena), ma il contenitore che le raggruppa e' gratis. */

typedef struct SVNScope {
    Hash_Table *values;    /* ValueKey -> vn (int): cosa contiene ADESSO questa identita' */
    Hash_Table *exprs;     /* ExprKey  -> vn (int): questa espressione e' gia' stata calcolata? */
    Hash_Table *leaders;   /* vn (int) -> NameList: tutti i nomi che hanno MAI contenuto questo valore */
    struct SVNScope *parent;
} SVNScope;

/* Capacita' iniziale piccola e non 'default' (101): queste tabelle sono
 * a vita breve, create/distrutte una volta per OGNI blocco della
 * funzione, mai riusate. Il costo dominante e' il calloc dei bucket a
 * ogni creazione (fatto per il caso comune: un blocco piccolo, poche
 * decine di istruzioni al piu'), non l'eventuale resize (ammortizzato
 * O(1), e scatta solo per un blocco insolitamente denso di valori
 * distinti - il caso raro, che puo' permettersi di pagarlo). Pre-allocare
 * una capacita' grande "per evitare resize" avrebbe senso per una tabella
 * a vita lunga; qui pagherebbe il caso raro ad ogni singolo blocco. */
#define SVN_SCOPE_TABLE_CAPACITY 7

typedef struct {
    long intVal;        /* kind==2 */
    double floatVal;     /* kind==3 */
    int kind;             /* 0=var, 1=temp, 2=constInt, 3=constFloat, -1=non pertinente */
    int a, b;             /* kind==0: level,offset; kind==1: tempId,0 */
} ValueKey;

typedef struct {
    int op;
    int vn1, vn2;       /* vn2 = -1 per gli operatori unari */
} ExprKey;

/* Mappa valori-nomi "attiva": invece di un unico leader fisso, ogni
 * numero di valore porta con se' TUTTI i nomi che l'hanno mai contenuto
 * lungo il cammino corrente. Non c'e' una vera rimozione alla
 * ridefinizione (mutare la lista di uno scope antenato romperebbe
 * l'invariante della sheaf): la "rimozione" e' ottenuta a costo zero,
 * per filtraggio, al momento della lettura (vedi nameStillValid) - un
 * nome resta nella lista per sempre, ma viene scartato se il suo valore
 * ATTUALE (in 'values', gia' correttamente scoping-aware) non coincide
 * piu' con il vn per cui e' stato inserito. Capacita' fissa (4): un
 * limite dichiarato, non un'illusione di generalita'. */
#define SVN_MAX_NAMES 4
typedef struct {
    int count;
    Operand names[SVN_MAX_NAMES];
} NameList;

/* Hash FNV-1a sui byte grezzi della chiave. E' sicura SOLO perche' ogni
 * chiave (ValueKey/ExprKey) viene azzerata per intero con memset PRIMA
 * di valorizzare i singoli campi (vedi keyForOperand/memoizeOrRewrite):
 * la comparazione in hash_table.c e' un memcmp byte-per-byte, compreso
 * l'eventuale padding di struct - senza lo zeroing esplicito, due chiavi
 * logicamente uguali potrebbero avere byte di padding diversi (non
 * inizializzati) e apparire diverse a ht_get/ht_set, un bug di
 * correttezza silenzioso e difficile da riprodurre. Con lo zeroing,
 * l'hash sui byte grezzi e' corretto quanto uno per-campo, e piu' veloce
 * (un solo giro sequenziale su una struct piccola, niente istruzioni
 * extra per estrarre campo per campo). */
static unsigned long svnHash(const void *key, size_t keySize) {
    const unsigned char *bytes = key;
    unsigned long h = 2166136261UL;
    for (size_t i = 0; i < keySize; i++) { h ^= bytes[i]; h *= 16777619UL; }
    return h;
}

static void svnScopeInit(SVNScope *scope, SVNScope *parent) {
    scope->values  = ht_create(SVN_SCOPE_TABLE_CAPACITY, svnHash);
    scope->exprs   = ht_create(SVN_SCOPE_TABLE_CAPACITY, svnHash);
    scope->leaders = ht_create(SVN_SCOPE_TABLE_CAPACITY, svnHash);
    scope->parent  = parent;
}

static void svnScopeDestroy(SVNScope *scope) {
    ht_destroy(scope->values, NULL);
    ht_destroy(scope->exprs, NULL);
    ht_destroy(scope->leaders, NULL);
}

static Operand mkNone(void) {
    Operand o; o.kind = OPND_NONE; return o;
}

static ValueKey keyForOperand(Operand op) {
    ValueKey k;
    memset(&k, 0, sizeof k);   /* azzera anche l'eventuale padding: vedi svnHash */
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
    NameList list;
    memset(&list, 0, sizeof list);
    for (SVNScope *s = scope; s; s = s->parent) {
        if (ht_get(s->leaders, &vn, sizeof(vn), &list, sizeof(list))) break;
    }
    if (list.count < SVN_MAX_NAMES) {
        list.names[list.count++] = name;
    }
    ht_set(scope->leaders, &vn, sizeof(vn), &list, sizeof(list));
}

/* Registra che 'dst' ora contiene il valore 'vn' (SEMPRE nello scope
 * locale, mai nel genitore) e aggiunge 'dst' come nuovo nome disponibile
 * per 'vn'. Se 'dst' non e' una variabile/temporaneo (istruzioni senza
 * vera destinazione scalare) non fa nulla. */
static void defineValue(Operand dst, int vn, SVNScope *scope) {
    if (dst.kind != OPND_VAR && dst.kind != OPND_TEMP) return;
    ValueKey k = keyForOperand(dst);
    ht_set(scope->values, &k, sizeof(k), &vn, sizeof(vn));
    addNameForValue(vn, dst, scope);
}

/* Vero se 'name' contiene ANCORA il valore 'vn' in questo punto del
 * cammino: le costanti e i temporanei lo sono sempre (mai riassegnati);
 * una variabile lo e' solo se il suo valore CORRENTE (da 'values', non
 * dalla lista stessa) e' ancora 'vn'. E' questo il controllo "al momento
 * dell'uso" che risolve il problema del leader stantio descritto in
 * svn.h: una variabile con nome puo' essere stata riassegnata dopo
 * essere stata registrata come leader, e va scartata qui, non alla
 * creazione (che romperebbe l'invariante sheaf). */
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
    NameList list;
    memset(&list, 0, sizeof list);
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
        ExprKey ek;
        memset(&ek, 0, sizeof ek);
        ek.op = (int) in->op; ek.vn1 = vn1; ek.vn2 = vn2;
        memoizeOrRewrite(in, ek, scope, nextVN);
        break;
    }

    case IR_NEG: case IR_NOT: {
        int vn1 = valueNumberOf(in->src1, scope, nextVN);
        ExprKey ek;
        memset(&ek, 0, sizeof ek);
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

/* ---- Visita: ricorsione lungo l'EBB, nessun tracking di visita --------
 *
 * svnProcessBlock ricorre in un successore SOLO se quel successore ha
 * predCount == 1 (un solo arco entrante nell'intero CFG: il suo). Per
 * costruzione questo rende impossibile visitare due volte lo stesso
 * blocco, o saltarne uno raggiungibile, SENZA bisogno di alcun array
 * 'visited', campo, o bitmap:
 *
 *   - un blocco con predCount != 1 e' raggiunto SOLO dal giro statico in
 *     svn_optimize (mai per ricorsione, che entra solo se predCount==1);
 *   - un blocco con predCount == 1 e' raggiunto SOLO per ricorsione, e da
 *     un unico possibile chiamante (se due chiamate diverse potessero
 *     raggiungerlo, avrebbe per definizione predCount >= 2).
 *
 * Le due categorie sono disgiunte e ciascun blocco rientra in esattamente
 * una di esse: zero byte di bookkeeping extra, contro gli N int (un
 * campo visited) o gli N bit (una bitmap) di qualunque alternativa - qui
 * non serve nemmeno quello, perche' non c'e' nulla da tracciare. */
static void svnProcessBlock(IRFunction *f, int blockIdx, SVNScope *parent, int *nextVN) {
    SVNScope scope;
    svnScopeInit(&scope, parent);

    for (int i = f->blocks[blockIdx].start; i < f->blocks[blockIdx].end; i++) {
        svnProcessInstr(&f->instrs[i], &scope, nextVN);
    }

    for (int k = 0; k < 2; k++) {
        int s = f->blocks[blockIdx].succ[k];
        if (s >= 0 && f->blocks[s].predCount == 1) {
            svnProcessBlock(f, s, &scope, nextVN);
        }
    }

    svnScopeDestroy(&scope);
}

void svn_optimize(IRFunction *f) {
    if (f->blockCount == 0) return;

    int nextVN = 0;
    /* Ogni blocco con predCount != 1 (l'entry, predCount==0, inclusa) e'
       la testa di un EBB: un singolo giro statico sull'array di blocchi
       gia' costruito basta a coprirli tutti, esattamente una volta a
       testa (vedi commento sopra svnProcessBlock). */
    for (int i = 0; i < f->blockCount; i++) {
        if (f->blocks[i].predCount != 1) {
            svnProcessBlock(f, i, NULL, &nextVN);
        }
    }
}

#include <stdio.h>
#include <stdarg.h>
#include "error.h"

static int errorFlag = 0;     /* errore dello statement corrente, non ancora recuperato */
static int errorCount = 0;    /* totale errori dell'intera compilazione, mai azzerato */

void reportError(int line, const char *fmt, ...) {
    if (errorFlag) {
        /* un errore per questo statement e' gia' stato segnalato:
           sopprime i messaggi a cascata generati */
        return;
    }

    fprintf(stderr, "Errore di sintassi (linea %d): ", line);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");

    errorFlag = 1;
    errorCount++;
}

int hadAnyError(void) {
    return errorFlag;
}

void clearError(void) {
    errorFlag = 0;
}

int totalErrorCount(void) {
    return errorCount;
}

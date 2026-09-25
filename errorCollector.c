#include <stdio.h>
#include "errorCollector.h"

static int pendingFlag = 0;  /* cascade window: one printed diagnostic until clear */
static int totalCount  = 0;  /* grand total, all phases, never reset */

void ec_report_cascadingv(int line, const char *fmt, va_list args) {
    if (pendingFlag) return;

    if (line >= 0)
        fprintf(stderr, "Errore di sintassi (linea %d): ", line);
    vfprintf(stderr, fmt, args);
    /* Parser msgs omit trailing \\n; semantic msgs already include it. */
    if (line >= 0)
        fprintf(stderr, "\n");

    pendingFlag = 1;
    totalCount++;
}

void ec_report_cascading(int line, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ec_report_cascadingv(line, fmt, args);
    va_end(args);
}

void ec_reportv(const char *fmt, va_list args) {
    /* Unconditional path: no cascade check (pass1 duplicates, etc.). */
    vfprintf(stderr, fmt, args);
    totalCount++;
}

void ec_report(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ec_reportv(fmt, args);
    va_end(args);
}

int ec_pending_error(void)  { return pendingFlag; }
void ec_clear_pending(void) { pendingFlag = 0; }
int ec_error_count(void)    { return totalCount; }

#include <stdio.h>
#include "errorCollector.h"

static int pendingFlag = 0;  // per-statement suppression flag, parser-only
static int totalCount  = 0;  // grand total, all phases, never reset

void ec_report_cascading(int line, const char *fmt, ...) {
    if (pendingFlag) return; // cascade from same statement: drop silently

    fprintf(stderr, "Errore di sintassi (linea %d): ", line);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");

    pendingFlag = 1;
    totalCount++;
}

void ec_reportv(const char *fmt, va_list args) {
    // no prefix added here: callers' fmt already carries the full message
    // (mirrors old semantic.c behaviour, preserved to avoid churning every
    // call-site's format string)
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
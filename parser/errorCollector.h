#ifndef ERROR_COLLECTOR_H
#define ERROR_COLLECTOR_H

#include <stdarg.h>

/**
 * @file error_collector.h
 * @brief Centralized error reporting/counting for every compiler phase.
 *
 * Two report modes funnel into one shared running total:
 *  - ec_report_cascading(): parser-only. Suppresses duplicate messages
 *    for the same erroneous statement (a syntax error found deep in the
 *    grammar - Expr -> ... -> Factor - would otherwise re-trigger at
 *    every enclosing rule before panic-mode recovery runs). Caller must
 *    call ec_clear_pending() once recovery completes.
 *  - ec_report()/ec_reportv(): unconditional, used by every other phase
 *    (semantic analysis, future passes). No suppression: each distinct
 *    call is a distinct diagnostic.
 */

/**
 * @brief Report a syntax error, suppressed while a statement error is pending.
 * @param line Source line of the error.
 * @param fmt  printf-style format string.
 */
void ec_report_cascading(int line, const char *fmt, ...);

/**
 * @brief Report an error unconditionally (convenience varargs wrapper).
 * @param fmt printf-style format string; caller embeds any needed prefix
 *            (e.g. line info) directly, since not every phase has a line
 *            number available (AST nodes carry none yet).
 */
void ec_report(const char *fmt, ...);

/**
 * @brief Same as ec_report() but takes an already-started va_list.
 *
 * Lets a phase-local wrapper (e.g. semantic.c's reportError, which also
 * bumps a local per-call counter) forward its varargs here instead of
 * duplicating the print logic.
 */
void ec_reportv(const char *fmt, va_list args);

/** @return 1 if ec_report_cascading() flagged an error not yet cleared. */
int ec_pending_error(void);

/** Clear the pending-error flag; call after panic-mode recovery. */
void ec_clear_pending(void);

/** @return total errors reported so far across every call site (never reset). */
int ec_error_count(void);

#endif /* ERROR_COLLECTOR_H */
#ifndef ERROR_COLLECTOR_H
#define ERROR_COLLECTOR_H


/**
 * @file errorCollector.h
 * @brief Centralized error reporting/counting for every compiler phase.
 *
 * Collects diagnostic messages together with optional source location.
 * Cascade suppression uses a single pending flag shared by parser and
 * semantic analysis: the first diagnostic in a recovery window is printed;
 * further ones are dropped until ec_clear_pending().
 */

#include <stdarg.h>

/**
 * @brief Report an error with cascade suppression.
 *
 * If a previous cascading error is still pending, the call is a no-op
 * (no print, no count bump). Otherwise prints the message, sets pending,
 * and increments the global error total.
 *
 * @param line  Source line; if @c line < 0, no line prefix is printed
 *              (for phases whose messages already embed context, e.g. semantic).
 * @param fmt   printf-style format string.
 */
void ec_report_cascading(int line, const char *fmt, ...);

/**
 * @brief Same as ec_report_cascading() but takes an already-started va_list.
 */
void ec_report_cascadingv(int line, const char *fmt, va_list args);

/**
 * @brief Report an error unconditionally (no cascade suppression).
 * @param fmt printf-style format string; caller embeds any needed prefix.
 */
void ec_report(const char *fmt, ...);

/**
 * @brief Same as ec_report() but takes an already-started va_list.
 */
void ec_reportv(const char *fmt, va_list args);

/** @return 1 if a cascading error is pending (not yet cleared). */
int ec_pending_error(void);

/** Clear the pending-error flag; call at statement / recovery boundaries. */
void ec_clear_pending(void);

/** @return total errors reported so far across every call site (never reset). */
int ec_error_count(void);

#endif /* ERROR_COLLECTOR_H */

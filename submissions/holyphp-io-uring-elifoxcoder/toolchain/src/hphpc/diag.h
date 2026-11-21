#ifndef HPHP_DIAG_H
#define HPHP_DIAG_H

#include <stddef.h>

/* Compiler diagnostics in the style of cargo/rustc.
 *
 * A terminal gets the full frame with the offending source line, a caret and
 * the location; a pipe keeps the one-line `file:line:col: error: msg` form
 * that tools (the editor extension, the playground, test scripts) parse.
 * Colours are used only on a terminal and honour NO_COLOR.
 *
 *   error: cannot assign string to int
 *    --> tests/negative/assign_to_let.hphp:3:4
 *     |
 *   3 | $n = "text";
 *     |    ^^^^^^^^
 *     |
 */

void diag_init(void);
int  diag_is_tty(void);

/* Report one diagnostic; `len` bytes are underlined (0 => one column). */
void diag_at(const char *severity, const char *file, size_t line, size_t col,
             size_t len, const char *msg);

/* A message without a location (missing file, internal limit, …). */
void diag_plain(const char *severity, const char *msg);

/* Format a fatal message, recognise an embedded `file:line:col:` location and
 * render it. Does not exit; `fatal()` in util.c calls this and then exits. */
void diag_fatal_message(char *msg);

/* cargo-style closing line after a failed check/build. */
void diag_summary(const char *path);

/* The "nothing to report" line (cargo's `Finished`). */
void diag_ok(const char *path);

int diag_errors(void);
int diag_warnings(void);

#endif

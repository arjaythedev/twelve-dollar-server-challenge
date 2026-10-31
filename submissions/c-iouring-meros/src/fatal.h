/* fatal: report an unrecoverable startup error and exit. */
#ifndef FATAL_H
#define FATAL_H

__attribute__((noreturn, format(printf, 1, 2)))
void fatal(const char *format, ...);

#endif

#include "fatal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

void fatal(const char *format, ...)
{
	va_list args;

	fputs("fatal: ", stderr);
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
	fputc('\n', stderr);
	exit(EXIT_FAILURE);
}

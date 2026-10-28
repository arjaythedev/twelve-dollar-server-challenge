/* IO utilities for HolyC */
#ifndef IO_HC
#define IO_HC

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

/* Basic I/O functions */
extern U0 Print(U8 *fmt, ...);
extern I64 MPrint(U8 *fmt, ...);
extern U8 *MStrPrint(U8 *fmt, ...);

#endif

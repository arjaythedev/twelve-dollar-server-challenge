/* IO utilities for HolyC */
#ifndef IO_HC
#define IO_HC

/* I/O declarations */
extern I32 read(I32 fd, U8 *buf, I64 count);
extern I32 write(I32 fd, U8 *buf, I64 count);
extern I32 printf(U8 *fmt, ...);
extern I64 time(I64 *t);

#define Print(fmt, ...) printf(fmt, __VA_ARGS__)

#endif
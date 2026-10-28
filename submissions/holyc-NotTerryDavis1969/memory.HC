/* Memory utilities for HolyC */
#ifndef MEMORY_HC
#define MEMORY_HC

extern U8 *malloc(I64 size);
extern void free(U8 *ptr);
extern U8 *calloc(I64 count, I64 size);
extern U8 *realloc(U8 *ptr, I64 size);

#define MAlloc(x) malloc(x)
#define Free(x) free(x)
#define CAlloc(c,s) calloc(c,s)
#define ReAlloc(p,s) realloc(p,s)

#endif
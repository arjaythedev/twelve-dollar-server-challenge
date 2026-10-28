/* Memory utilities for HolyC */
#ifndef MEMORY_HC
#define MEMORY_HC

#include <stdlib.h>
#include <string.h>

#define MAlloc(x) malloc(x)
#define Free(x) free(x)
#define CAlloc(c,s) calloc(c,s)
#define ReAlloc(p,s) realloc(p,s)

#endif

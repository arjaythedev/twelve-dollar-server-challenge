/* Allocation that ends the program when memory runs out: on a 2 GB box there is nothing to recover to. */
#ifndef MEMORY_H
#define MEMORY_H

#include <stdlib.h>

#include "fatal.h"

static inline void *xrealloc(void *ptr, size_t size)
{
	void *result = realloc(ptr, size);

	if (result == NULL)
		fatal("out of memory (%zu bytes)", size);
	return result;
}

static inline void *xcalloc(size_t count, size_t size)
{
	void *result = calloc(count, size);

	if (result == NULL)
		fatal("out of memory (%zu bytes)", count * size);
	return result;
}

#endif

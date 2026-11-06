#ifndef ASC_TREE_H
#define ASC_TREE_H
#include "abi.h"
#include "sqlite3.h"
typedef struct AscTree AscTree;
enum { TREE_FALLBACK = -1 };
/* Bit 0 selects vector record decoding; bit 1 selects logarithmic leaf runs. */
AscTree *asc_tree_open(sqlite3 *db,unsigned flags);
void asc_tree_close(AscTree *tree);
int asc_tree_read(AscTree *tree, int64_t id, int feed, PostVisitor emit, void *state);
#endif

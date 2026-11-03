/* codegen.h — C source emission from the typed AST. */
#ifndef HPHPC_CODEGEN_H
#define HPHPC_CODEGEN_H

#include "ast.h"
#include "util.h"

typedef struct Codegen Codegen;

/* Emit complete C translation unit for the program into out. */
void codegen_emit(Program *prog, Buf *out);

#endif

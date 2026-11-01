/* parser.h — recursive descent parser producing a Program. */
#ifndef HPHPC_PARSER_H
#define HPHPC_PARSER_H

#include "ast.h"

typedef struct Parser {
    TokenStream *ts;
    const char *file;
    int loop_depth;
} Parser;

Program *parse_program(const char *src, const char *filename);
Expr *parse_embedded_expr(const char *src, const char *file);

Parser *parser_new(PtrVec toks, const char *file);

/* shared with sema/codegen diagnostics */
const char *dup_text(const Token *t);

#endif

/* sema.h — semantic analysis for HolyPHP.
 *
 * Responsibilities:
 *   - resolve names (functions, classes, enums, variables)
 *   - infer & check types, automatic numeric promotion (int -> float)
 *   - enforce the memory model:
 *       * value semantics (PHP-like), &x borrows
 *       * borrow checker: at most one &mut, no &mut while & alive
 *       * own<T> / Rc<T> smart pointers with move semantics
 *       * raw pointers *T restricted to unsafe blocks / unsafe fn
 *   - catch misuse (unknown functions, wrong arg counts, ...)
 */
#ifndef HPHPC_SEMA_H
#define HPHPC_SEMA_H

#include "ast.h"

typedef struct VarSym {
    const char *name;
    const Type *type;
    Token tok;
    bool is_mut;
    bool is_param;
    bool is_param_ref;         /* declared `&$x` — aliases the caller's slot */
    bool captured;
    bool moved_from;
    bool is_auto;              /* declared by assignment (PHP-style) */
    bool is_undeclared_write;
    bool is_preg_out;          /* $matches out-param of preg_match(_all) */
    bool captured_byref;       /* captured by a closure via use(&$x) */
    bool is_global_alias;      /* bound by `global $x;` — storage lives at top level */
    bool is_program_global;    /* the shared file-scope VarSym behind `global $x;` */
    int uid;
    int decl_seq;              /* scope seq where declared (capture analysis) */
    unsigned write_seq;        /* last write: a fact older than this is stale */
} VarSym;

typedef struct FnSig {
    const Type **params;
    size_t nparams;
    const Type *ret;
    bool variadic;
    bool is_unsafe;
} FnSig;

void sema_run(Program *prog);
Type *sema_typeof(Expr *e);                 /* returns e->type */
const FnSig *sema_fn_sig(AstFn *fn);
bool sema_result_ok(void);

/* diagnostics */
void sema_error(const Token *tok, const char *fmt, ...);

#endif

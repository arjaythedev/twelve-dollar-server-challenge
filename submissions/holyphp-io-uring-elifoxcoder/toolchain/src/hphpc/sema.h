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
    int scope_depth;           /* lexical nesting level at declaration */
    bool is_param_ref;         /* declared `&$x` — aliases the caller's slot */
    bool captured;
    bool is_auto;              /* declared by assignment (PHP-style) */
    bool is_undeclared_write;
    bool is_preg_out;          /* $matches out-param of preg_match(_all) */
    bool captured_byref;       /* captured by a closure via use(&$x) */
    bool is_global_alias;      /* bound by `global $x;` — storage lives at top level */
    bool is_program_global;    /* the shared file-scope VarSym behind `global $x;` */
    int uid;
    int decl_seq;              /* scope seq where declared (capture analysis) */
    unsigned write_seq;        /* last write: a fact older than this is stale */
    const struct VarSym *borrow_origin; /* referent held by a checked borrow */
    const struct VarSym *borrow_parent; /* reference temporarily reborrowed */
    const Expr *borrow_place;           /* field/index projection of the referent, NULL = whole root */
    const struct VarSym *alias_origin;  /* known shared heap-object identity */
    bool alias_unknown;                 /* may alias any object of its type */
    bool shared_alias;                  /* mutable heap alias derived from &T */
    bool captured_heap_alias;           /* a closure may retain this object */
    bool region_local;                  /* safety-mode value proven function-local */
    const Stmt *region_loop;             /* iteration region for proven loop-local value */
    AstFn *owner_fn;                     /* declaration scope for effect propagation */
    bool borrow_mut;
    bool borrow_pending;       /* reference origin awaits a forward-call summary */
    bool borrow_scope_error_reported;
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
bool sema_type_is_linear(const Type *type); /* contains an own<T> and cannot be copied */
void borrow_check_function(AstFn *fn);
void borrow_check_script(PtrVec stmts);
void borrow_infer_summaries(PtrVec fns);
/* True only when every method selected by virtual dispatch borrows this arg. */
bool sema_method_param_borrow_only(const Expr *call, size_t arg_index);

/* diagnostics */
void sema_error(const Token *tok, const char *fmt, ...);

#endif

/* sema.c — semantic analysis for HolyPHP.
 *
 * Implements:
 *   - name resolution (functions, classes, enums, variables, methods)
 *   - type inference & checking with PHP-style numeric promotion
 *   - the memory model:
 *       * value semantics for scalars/arrays (PHP-like, COW at runtime)
 *       * &x / &mut x borrows, checked like Rust: many readers XOR one writer
 *       * own<T> moves, Rc<T> shared smart pointers
 *       * raw pointers *T only inside unsafe blocks / unsafe fns
 */
#include "sema.h"
#include "builtins.h"
#include "diag.h"
#include <stdarg.h>

/* ---------------- global symbol tables ---------------- */
typedef struct FnSym {
    const char *name;
    AstFn *fn;
    const FnSig *sig;
    struct FnSym *next;
} FnSym;

typedef struct ClassSym {
    const char *name;
    AstClass *cls;
    struct ClassSym *next;
} ClassSym;

typedef struct EnumSym {
    const char *name;
    AstEnum *enm;
    const Type *type;
    struct EnumSym *next;
} EnumSym;

static FnSym *fns = NULL;
static ClassSym *classes = NULL;
static EnumSym *enums = NULL;
static bool sema_ok = true;
static size_t var_uid = 0;

/* scopes */
#define MAX_SCOPE 64
typedef struct Scope {
    VarSym *vars[MAX_SCOPE];
    size_t nvars;
    struct Scope *parent;
    AstFn *cur_fn;
    AstClass *cur_class;
    bool in_unsafe;
    bool enforce_memory_safety;
    int loop_depth;
    int depth;
    const Type *self_type;
    int seq;                   /* creation order, for capture analysis */
} Scope;

static void check_stmt(Scope *sc, Stmt *s);
static const Type *check_expr(Scope *sc, Expr *e);
static Type *class_type_of(AstClass *c);

static const Type *nullable_type_of(const Type *inner) {
    Type *t = type_new(TY_NULLABLE);
    t->elem = inner;
    return t;
}

/* ---- closure capture collection ----
 * While checking a closure body, every variable that resolves OUTSIDE the
 * closure is recorded as a capture (by value, PHP-like). Scope structs get
 * sequence numbers; a var is "outer" iff it was declared in a scope with a
 * smaller seq than the closure body scope. Nested closures propagate their
 * captures (and this-ness) outward. */
static AstFn *g_clo_fn[32];
static int g_clo_body_seq[32];
static PtrVec g_clo_caps[32];
static bool g_clo_this[32];
static int g_clo_depth = 0;
static int g_scope_seq = 0;

/* Program-wide registry for `global $x;` variables. All ST_GLOBAL stmts
 * (in any function) resolve to the SAME VarSym so codegen emits one file-
 * scope definition per name and every use shares it. */
static PtrVec g_program_globals = {0};
static PtrVec g_borrow_fns = {0};

static VarSym *expr_root_var(Expr *e) {
    if (!e) return NULL;
    switch (e->kind) {
    case EX_VAR: return (VarSym *)e->u.var.sym;
    case EX_FIELD: return expr_root_var(e->u.field.obj);
    case EX_INDEX: return expr_root_var(e->u.index.obj);
    case EX_DEREF: return expr_root_var(e->u.deref.operand);
    default: return NULL;
    }
}

/* True when evaluating this place traverses a shared reference anywhere in
 * its base chain. Looking only at the final expression type misses paths such
 * as `get_ref()->child->value`, whose visible type is just the child type. */
static bool expr_through_shared_ref(const Expr *e) {
    if (!e) return false;
    if (e->type && e->type->kind == TY_REF) return true;
    if (e->borrow_origin) {
        const VarSym *origin = e->borrow_origin;
        if (origin->shared_alias ||
            (origin->type && origin->type->kind == TY_REF)) return true;
    }
    switch (e->kind) {
    case EX_VAR:
        return e->u.var.sym && ((VarSym *)e->u.var.sym)->shared_alias;
    case EX_FIELD: return expr_through_shared_ref(e->u.field.obj);
    case EX_INDEX: return expr_through_shared_ref(e->u.index.obj);
    case EX_DEREF: return expr_through_shared_ref(e->u.deref.operand);
    case EX_ASSIGN: return expr_through_shared_ref(e->u.assign.value);
    case EX_BIN:
        return expr_through_shared_ref(e->u.bin.l) ||
               expr_through_shared_ref(e->u.bin.r);
    case EX_TERNARY:
        return expr_through_shared_ref(e->u.tern.then) ||
               expr_through_shared_ref(e->u.tern.els);
    case EX_MATCH:
        for (size_t i = 0; i < e->u.matchexpr.cases.len; i++) {
            MatchCase *c = e->u.matchexpr.cases.items[i];
            if (expr_through_shared_ref(c->body)) return true;
        }
        return expr_through_shared_ref(e->u.matchexpr.dflt);
    case EX_ARRAY_LIT:
        for (size_t i = 0; i < e->u.arr.elems.len; i++)
            if (expr_through_shared_ref(e->u.arr.elems.items[i])) return true;
        return false;
    case EX_MAP_LIT:
        for (size_t i = 0; i < e->u.map.vals.len; i++) {
            if (expr_through_shared_ref(e->u.map.keys.items[i]) ||
                expr_through_shared_ref(e->u.map.vals.items[i])) return true;
        }
        return false;
    case EX_TUPLE_LIT:
        for (size_t i = 0; i < e->u.tuple.elems.len; i++)
            if (expr_through_shared_ref(e->u.tuple.elems.items[i])) return true;
        return false;
    case EX_CAST: return expr_through_shared_ref(e->u.cast.operand);
    default: return false;
    }
}

static bool type_is_mutable_alias(const Type *t) {
    if (!t) return false;
    switch (t->kind) {
    case TY_CLASS: case TY_STRUCT: case TY_INTERFACE:
    case TY_ARRAY: case TY_VEC: case TY_MAP: case TY_SET:
    case TY_OWN: case TY_RC: case TY_PTR: case TY_FN: case TY_MIXED:
        return true;
    case TY_OPTION: case TY_NULLABLE: case TY_REF: case TY_MUTREF:
        return type_is_mutable_alias(t->elem);
    case TY_RESULT:
        return type_is_mutable_alias(t->ok) || type_is_mutable_alias(t->err);
    case TY_TUPLE: case TY_UNION:
        for (size_t i = 0; i < t->nmembers; i++)
            if (type_is_mutable_alias(t->members[i])) return true;
        return false;
    default: return false;
    }
}

/* Cross-thread captures are limited to immutable value types for now.
 * Class-name conventions are not a Sync proof: user code can define a
 * mutable class named Channel/Mutex/etc. Arrays and all class instances are
 * rejected until the language has a verifiable Sync contract. */
static bool thread_capture_is_sync(const Type *t) {
    if (!t) return false;
    switch (t->kind) {
    case TY_INT: case TY_I8: case TY_I16: case TY_I32: case TY_I64:
    case TY_FLOAT: case TY_F32: case TY_F64: case TY_BOOL: case TY_STRING:
    case TY_ENUM: case TY_NULL:
        return true;
    case TY_CLASS: case TY_STRUCT: case TY_INTERFACE:
        return false;
    case TY_NULLABLE: case TY_OPTION:
        return thread_capture_is_sync(t->elem);
    case TY_FN:
        return t->thread_safe_callback;
    default:
        return false;
    }
}

static bool thread_capture_is_movable_value(const Type *t);

static bool validate_thread_callback(Expr *value, const Type *type,
                                     const Token *where, const char *callee) {
    if (type && (type->kind == TY_NULLABLE || type->kind == TY_OPTION))
        type = type->elem;
    AstFn *closure = type && type->kind == TY_FN ? type->closure_fn : NULL;
    if (!closure && value && value->kind == EX_CLOSURE)
        closure = value->u.closure.fn;
    if (type && type->kind == TY_FN && type->thread_safe_callback) return true;
    if (!closure) {
        sema_error(where ? where : (value ? &value->tok : NULL),
                   "%s() requires a statically known thread_callable value",
                   callee);
        return false;
    }
    closure->thread_worker = true;
    for (size_t i = 0; i < closure->captures.len; i++) {
        VarSym *capture = closure->captures.items[i];
        if (capture->captured_byref) {
            sema_error(&capture->tok,
                       "thread closure captures mutable or non-Sync value $%s; use a value container or a channel",
                       capture->name);
        } else if (!thread_capture_is_sync(capture->type)) {
            if (thread_capture_is_movable_value(capture->type))
                ptrvec_push(&closure->thread_moves, capture);
            else
                sema_error(&capture->tok,
                           "thread closure captures mutable or non-Sync value $%s; use a value container or a channel",
                           capture->name);
        }
    }
    if (closure->uses_this &&
        !thread_capture_is_sync(class_type_of(closure->owner_cls)))
        sema_error(where ? where : &closure->name_tok,
                   "thread closure captures mutable $this; use a channel or synchronized primitive");
    if (type && type->kind == TY_FN) ((Type *)type)->thread_safe_callback = true;
    return true;
}

/* Value containers have COW semantics, so a transferred container owner can
 * be used by a worker while aliases detach on write. Its elements still have
 * to be safe value types: a container of mutable object handles is not Sync. */
static bool thread_capture_is_movable_value(const Type *t) {
    if (!t) return false;
    switch (t->kind) {
    case TY_ARRAY: case TY_VEC: case TY_SET:
        return thread_capture_is_sync(t->elem) ||
               (t->elem && (t->elem->kind == TY_ARRAY || t->elem->kind == TY_VEC ||
                            t->elem->kind == TY_MAP || t->elem->kind == TY_SET) &&
                thread_capture_is_movable_value(t->elem));
    case TY_MAP:
        return thread_capture_is_sync(t->key) &&
               (thread_capture_is_sync(t->val) || thread_capture_is_movable_value(t->val));
    default: return false;
    }
}

static bool thread_sendable_value(const Type *t) {
    if (!t) return false;
    if (thread_capture_is_sync(t) || t->kind == TY_MIXED) return true;
    switch (t->kind) {
    case TY_ARRAY: case TY_VEC: case TY_SET:
        return thread_sendable_value(t->elem);
    case TY_MAP:
        return thread_sendable_value(t->key) && thread_sendable_value(t->val);
    case TY_NULLABLE: case TY_OPTION:
        return thread_sendable_value(t->elem);
    default: break;
    }
    return t && t->kind == TY_OWN && t->elem &&
           (t->elem->kind == TY_CLASS || t->elem->kind == TY_STRUCT ||
            t->elem->kind == TY_INTERFACE);
}

static void enqueue_borrow_fn(AstFn *fn) {
    if (!fn) return;
    for (size_t i = 0; i < g_borrow_fns.len; i++)
        if (g_borrow_fns.items[i] == fn) return;
    ptrvec_push(&g_borrow_fns, fn);
}
static VarSym *program_global_find(const char *name) {
    for (size_t i = 0; i < g_program_globals.len; i++) {
        VarSym *v = g_program_globals.items[i];
        if (v->name == name) return v;   /* names are interned */
    }
    return NULL;
}
static VarSym *program_global_get(const char *name, Token tok) {
    VarSym *v = program_global_find(name);
    if (!v) {
        v = xcalloc(1, sizeof(VarSym));
        v->name = name;
        v->type = ty_mixed;
        v->tok = tok;
        v->is_mut = true;
        v->is_auto = true;
        v->is_program_global = true;
        v->uid = (int)var_uid++;   /* unique C name for the file-scope storage */
        ptrvec_push(&g_program_globals, v);
    }
    return v;
}

static void note_capture(VarSym *v) {
    PtrVec *list = &g_clo_caps[g_clo_depth - 1];
    for (size_t i = 0; i < list->len; i++)
        if (list->items[i] == v) return;
    ptrvec_push(list, v);
    v->captured = true;
}

void sema_error(const Token *tok, const char *fmt, ...) {
    sema_ok = false;
    char msg[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (tok && tok->file)
        diag_at("error", tok->file, tok->line, tok->col, tok->len, msg);
    else
        diag_at("error", "?", 0, 0, 1, msg);
}

bool sema_result_ok(void) { return sema_ok; }

/* ---------------- type helpers ---------------- */
typedef struct SemaTypePair {
    const Type *a, *b;
} SemaTypePair;

static bool heap_types_may_alias_walk(const Type *a, const Type *b,
                                     SemaTypePair *seen, size_t *nseen, int depth);

static bool heap_type_children_may_alias(const Type *t, const Type *other,
                                        SemaTypePair *seen, size_t *nseen, int depth) {
    if (!t) return false;
    const Type *base = t;
    while (base && (base->kind == TY_REF || base->kind == TY_MUTREF ||
                    base->kind == TY_PTR || base->kind == TY_OWN || base->kind == TY_RC))
        base = base->elem;
    if (base && (base->kind == TY_CLASS || base->kind == TY_STRUCT ||
                 base->kind == TY_INTERFACE)) {
        AstClass *cls = (AstClass *)base->decl;
        for (AstClass *c = cls; c; c = c->base)
            for (size_t i = 0; i < c->fields.len; i++) {
                StructField *field = c->fields.items[i];
                if (heap_types_may_alias_walk(field->type, other, seen, nseen, depth + 1))
                    return true;
            }
    }
    switch (t->kind) {
    case TY_REF: case TY_MUTREF: case TY_PTR: case TY_OWN: case TY_RC:
    case TY_OPTION: case TY_NULLABLE:
    case TY_ARRAY: case TY_VEC: case TY_SET:
        return heap_types_may_alias_walk(t->elem, other, seen, nseen, depth + 1);
    case TY_MAP:
        return heap_types_may_alias_walk(t->key, other, seen, nseen, depth + 1) ||
               heap_types_may_alias_walk(t->val, other, seen, nseen, depth + 1);
    case TY_RESULT:
        return heap_types_may_alias_walk(t->ok, other, seen, nseen, depth + 1) ||
               heap_types_may_alias_walk(t->err, other, seen, nseen, depth + 1);
    case TY_TUPLE: case TY_UNION:
        for (size_t i = 0; i < t->nmembers; i++)
            if (heap_types_may_alias_walk(t->members[i], other, seen, nseen, depth + 1))
                return true;
        return false;
    case TY_FN:
        return true; /* opaque closure environment may retain any heap alias */
    default:
        return false;
    }
}

static bool same_mutable_storage_family(TypeKind a, TypeKind b) {
    bool a_collection = a == TY_ARRAY || a == TY_VEC || a == TY_MAP ||
                       a == TY_SET || a == TY_TUPLE;
    bool b_collection = b == TY_ARRAY || b == TY_VEC || b == TY_MAP ||
                       b == TY_SET || b == TY_TUPLE;
    return a_collection && b_collection;
}

static bool heap_types_may_alias_walk(const Type *a, const Type *b,
                                     SemaTypePair *seen, size_t *nseen, int depth) {
    if (!a || !b) return false;
    if (depth > 64) return true; /* fail closed for unusually deep type graphs */
    if (a->kind == TY_MIXED || b->kind == TY_MIXED ||
        a->kind == TY_FN || b->kind == TY_FN) return true;
    /* Arrays, maps, vectors, sets and tuples all use mutable harr* storage.
     * Element types alone cannot prove their backing allocations disjoint. */
    if (same_mutable_storage_family(a->kind, b->kind)) return true;
    bool a_heap = a->kind == TY_CLASS || a->kind == TY_STRUCT || a->kind == TY_INTERFACE;
    bool b_heap = b->kind == TY_CLASS || b->kind == TY_STRUCT || b->kind == TY_INTERFACE;
    if (a_heap && b_heap) {
        /* One concrete class can satisfy both interfaces. */
        if (a->kind == TY_INTERFACE && b->kind == TY_INTERFACE) return true;
        if (a->kind == b->kind && a->name == b->name) return true;
        AstClass *ac = (AstClass *)a->decl;
        AstClass *bc = (AstClass *)b->decl;
        for (AstClass *c = ac; c; c = c->base) if (c == bc && bc) return true;
        for (AstClass *c = bc; c; c = c->base) if (c == ac && ac) return true;
        if (a->kind == TY_INTERFACE && bc) {
            for (AstClass *c = bc; c; c = c->base)
                for (size_t i = 0; i < c->interfaces.len; i++)
                    if (a->name && strcmp(c->interfaces.items[i], a->name) == 0) return true;
        }
        if (b->kind == TY_INTERFACE && ac) {
            for (AstClass *c = ac; c; c = c->base)
                for (size_t i = 0; i < c->interfaces.len; i++)
                    if (b->name && strcmp(c->interfaces.items[i], b->name) == 0) return true;
        }
    }
    for (size_t i = 0; i < *nseen; i++)
        if ((seen[i].a == a && seen[i].b == b) ||
            (seen[i].a == b && seen[i].b == a)) return false;
    if (*nseen >= 128) return true;
    seen[(*nseen)++] = (SemaTypePair){a, b};
    if (heap_type_children_may_alias(a, b, seen, nseen, depth) ||
        heap_type_children_may_alias(b, a, seen, nseen, depth)) {
        (*nseen)--;
        return true;
    }
    (*nseen)--;
    return false;
}

static bool heap_types_may_alias(const Type *a, const Type *b) {
    SemaTypePair seen[128];
    size_t nseen = 0;
    return heap_types_may_alias_walk(a, b, seen, &nseen, 0);
}

static bool type_can_contain_heap_alias(const Type *t) {
    if (!t) return false;
    switch (t->kind) {
    case TY_CLASS: case TY_STRUCT: case TY_INTERFACE: case TY_MIXED: case TY_FN:
    case TY_ARRAY: case TY_VEC: case TY_SET: case TY_MAP: case TY_TUPLE:
        return true;
    case TY_REF: case TY_MUTREF: case TY_PTR: case TY_OWN: case TY_RC:
    case TY_OPTION: case TY_NULLABLE:
        return type_can_contain_heap_alias(t->elem);
    case TY_RESULT:
        return type_can_contain_heap_alias(t->ok) || type_can_contain_heap_alias(t->err);
    case TY_UNION:
        for (size_t i = 0; i < t->nmembers; i++)
            if (type_can_contain_heap_alias(t->members[i])) return true;
        return false;
    default:
        return false;
    }
}

static bool expr_is_fresh_heap_value(const Expr *e) {
    if (!e) return false;
    return e->kind == EX_NEW || e->kind == EX_CLOSURE ||
           e->kind == EX_ARRAY_LIT || e->kind == EX_MAP_LIT ||
           e->kind == EX_TUPLE_LIT;
}

static bool scope_has_borrow_of_heap_alias(Scope *sc, VarSym *object) {
    const VarSym *identity = object->alias_origin ? object->alias_origin : object;
    for (Scope *s = sc; s; s = s->parent) for (size_t i = 0; i < s->nvars; i++) {
        VarSym *v = s->vars[i];
        if (!v || !v->borrow_origin) continue;
        const VarSym *borrowed = v->borrow_origin->alias_origin
            ? v->borrow_origin->alias_origin : v->borrow_origin;
        if (borrowed == identity) return true;
        if ((object->alias_unknown || identity->is_param ||
             v->borrow_origin->alias_unknown || v->borrow_origin->is_param ||
             type_can_contain_heap_alias(identity->type)) &&
            heap_types_may_alias(identity->type, v->borrow_origin->type)) return true;
        if (object->type && object->type->kind == TY_MIXED) return true;
    }
    return false;
}

static void mark_heap_capture(Scope *sc, VarSym *object, const Token *tok,
                              bool by_reference) {
    if (!object || (!by_reference && !type_is_mutable_alias(object->type))) return;
    const VarSym *identity = object->alias_origin ? object->alias_origin : object;
    if ((by_reference || !object->captured_heap_alias) &&
        !identity->captured_heap_alias && scope_has_borrow_of_heap_alias(sc, object))
        sema_error(tok, "cannot capture $%s while it is borrowed; closure capture lifetime tracking is unavailable",
                   object->name);
    object->captured_heap_alias = true;
    if (identity != object) ((VarSym *)identity)->captured_heap_alias = true;
    AstFn *owner = identity->owner_fn ? identity->owner_fn : object->owner_fn;
    if (owner && (identity->is_param || object->is_param))
        for (size_t i = 0; i < owner->params.len; i++) {
            Param *pm = owner->params.items[i];
            if (pm->sym == identity || pm->sym == object)
                pm->captures_heap_alias = true;
        }
    for (Scope *s = sc; s; s = s->parent) for (size_t i = 0; i < s->nvars; i++) {
        VarSym *v = s->vars[i];
        if (!v || !type_is_mutable_alias(v->type)) continue;
        const VarSym *vid = v->alias_origin ? v->alias_origin : v;
        if (vid == identity || object->alias_unknown ||
            (object->type && object->type->kind == TY_MIXED) ||
            (type_can_contain_heap_alias(object->type) &&
             heap_types_may_alias(object->type, v->type)) ||
            (v->alias_unknown && heap_types_may_alias(object->type, v->type)))
            v->captured_heap_alias = true;
    }
}

static bool heap_alias_captured(Scope *sc, VarSym *object) {
    if (!object) return false;
    /* A reference binding is a different local symbol from the place it
     * borrows. Closure capture marks the captured place, so resolve that
     * place before checking whether a new borrow could outlive its owner. */
    const VarSym *referent = object->borrow_origin ? object->borrow_origin : object;
    const VarSym *identity = referent->alias_origin ? referent->alias_origin : referent;
    if (object->captured_heap_alias || identity->captured_heap_alias) return true;
    for (Scope *s = sc; s; s = s->parent) for (size_t i = 0; i < s->nvars; i++) {
        VarSym *v = s->vars[i];
        if (!v || !v->captured_heap_alias) continue;
        const VarSym *captured = v->borrow_origin ? v->borrow_origin : v;
        const VarSym *vid = captured->alias_origin ? captured->alias_origin : captured;
        if (vid == identity || object->alias_unknown || referent->alias_unknown ||
            v->alias_unknown ||
            ((object->is_param || referent->is_param ||
              type_can_contain_heap_alias(captured->type) ||
              type_can_contain_heap_alias(referent->type)) &&
             heap_types_may_alias(referent->type, captured->type))) return true;
    }
    return false;
}

static const Type *unify_numeric(const Type *a, const Type *b) {
    if (!a || !b) return ty_mixed;
    bool af = a->kind == TY_FLOAT || a->kind == TY_F32 || a->kind == TY_F64;
    bool bf = b->kind == TY_FLOAT || b->kind == TY_F32 || b->kind == TY_F64;
    if (af || bf) {
        if (a->kind == TY_F32 && b->kind == TY_F32) return ty_f32;
        if ((a->kind == TY_F64 || b->kind == TY_F64)) return ty_f64;
        return ty_float;
    }
    if (a->kind == TY_INT && b->kind == TY_INT) return ty_int;
    /* mixed-size ints: widen to the widest operand; i128 wins over all of
     * them, so `$wide + 1` stays exact instead of dropping to 64 bits */
    if (type_is_integral(a->kind) && type_is_integral(b->kind)) {
        if (a->kind == TY_I128 || b->kind == TY_I128) return ty_i128;
        if (a->kind == TY_I8 && b->kind == TY_I8) return ty_i8;
        if (a->kind == TY_I16 && b->kind == TY_I16) return ty_i16;
        if (a->kind == TY_I32 && b->kind == TY_I32) return ty_i32;
        return ty_i64;
    }
    return ty_mixed;
}

static bool types_compatible(const Type *want, const Type *got);

/* ---------- i128 is not an hval payload ----------
 *
 * hval's union holds 64 bits, so an i128 cannot be stored in an array, a
 * map, a mixed slot or a closure environment without losing its top half.
 * The language refuses that silently-lossy conversion and says what to do
 * instead. Narrowing to a smaller integer is refused too: it is only
 * allowed through an explicit cast. */
static void reject_i128_lossy(const Token *tok, const Type *want, const Type *got,
                              const char *what) {
    if (!got || got->kind != TY_I128 || !want || want->kind == TY_I128) return;
    switch (want->kind) {
    case TY_MIXED: case TY_ARRAY: case TY_VEC: case TY_MAP: case TY_SET:
    case TY_TUPLE: case TY_OPTION: case TY_RESULT: case TY_NULLABLE:
    case TY_UNION: case TY_NULL:
        sema_error(tok, "%s: i128 does not fit in %s; convert with (int) or strval() first",
                   what, type_to_string(want));
        return;
    case TY_STRING: case TY_BOOL:
        sema_error(tok, "%s: i128 does not convert to %s implicitly; use an explicit cast or strval()",
                   what, type_to_string(want));
        return;
    default:
        break;
    }
    if (type_is_integral(want->kind))
        sema_error(tok, "%s: cannot narrow i128 to %s implicitly; use an explicit cast",
                   what, type_to_string(want));
}

static void reject_untracked_reference(const Expr *arg, const Type *got,
                                       const Type *param, const char *where) {
    if (got && got->kind != TY_REF && got->kind != TY_MUTREF &&
        type_is_mutable_alias(got) && expr_through_shared_ref(arg))
        sema_error(arg ? &arg->tok : NULL,
                   "cannot pass mutable alias derived from a shared reference to %s",
                   where);
    if (!got || (got->kind != TY_REF && got->kind != TY_MUTREF)) return;
    if (!param || (param->kind != TY_REF && param->kind != TY_MUTREF))
        sema_error(arg ? &arg->tok : NULL,
                   "cannot pass a reference to %s because its parameter has no reference lifetime contract",
                   where);
}

/* ---------------- narrowing ----------------
 *
 * A nullable slot holds a value or null, and a branch that proves which one
 * it is refines the variable for the rest of that branch:
 *
 *     $x: ?string = null;
 *     if ($x !== null) { strlen($x); }   // $x is a string in here
 *
 * The refinements live in a stack that is pushed for a branch and popped
 * when the branch ends, so a refinement never escapes the block that
 * established it. A write to the variable makes the refinement about its
 * old value stale - unless the new value has the same shape, in which case
 * it still holds. */
#define MAX_REFINES 256
typedef struct Refinement {
    VarSym *var;
    const Type *type;      /* the type inside the branch */
    bool dead;             /* the value was written: the proof is stale */
} Refinement;

static Refinement g_refines[MAX_REFINES];
static int g_nrefines = 0;

/* Counts writes to variables.
 *
 * A fact is only about a value that was still the variable's when the fact
 * was made, so a branch that survived an early exit has to be checked
 * against this: it may have written the variable itself. */
static unsigned g_write_seq = 0;

static int refine_mark(void) { return g_nrefines; }
static void refine_restore(int mark) { g_nrefines = mark; }

/* the type of $v as it is seen right now */
static const Type *refined_type_of(const VarSym *v) {
    if (!v) return NULL;
    for (int i = g_nrefines - 1; i >= 0; i--) {
        if (g_refines[i].var != v) continue;
        /* the newest proof wins; a stale one means the value is unknown */
        if (g_refines[i].dead) return v->type;
        return g_refines[i].type;
    }
    return v->type;
}

/* A proof is pushed, never merged into an older one.
 *
 * A branch that proves something new about a variable shadows the proof the
 * outer branch had, and the outer proof is back when the branch ends (the
 * stack is cut back to the mark). Merging them instead would let the inner
 * proof escape: the outer branch would keep the inner one. */
static void refine_push(VarSym *v, const Type *t) {
    if (!v || !t) return;
    if (g_nrefines >= MAX_REFINES) return;
    g_refines[g_nrefines].var = v;
    g_refines[g_nrefines].type = t;
    g_refines[g_nrefines].dead = false;
    g_nrefines++;
}

/* Do two types have the same payload shape in an hval slot?
 *
 * A nullable/union slot holds an hval, and the payload is tagged by the
 * value's own type: an int lands in u.i, a float in u.f, a string in u.s, an
 * object in u.p. A proof that claims "a string" therefore only survives an
 * assignment of something that is stored the same way - an int does not fit
 * a float proof, and a Gadget does not fit a Widget proof. */
static bool type_same_payload(const Type *a, const Type *b) {
    if (!a || !b) return false;
    /* a bool has its own payload byte - it is not an int that happens to
     * be 0 or 1 */
    if (a->kind == TY_BOOL || b->kind == TY_BOOL) return a->kind == b->kind;
    if (a->kind == b->kind) {
        switch (a->kind) {
        case TY_CLASS: case TY_STRUCT: case TY_INTERFACE:
            return type_same_member(a, b);
        default:
            return true;
        }
    }
    if (type_is_integral(a->kind) && type_is_integral(b->kind)) return true;
    if ((a->kind == TY_FLOAT || a->kind == TY_F32 || a->kind == TY_F64) &&
        (b->kind == TY_FLOAT || b->kind == TY_F32 || b->kind == TY_F64)) return true;
    return false;
}

/* Does a nullable/union slot hold a member with the same payload shape? */
static bool slot_admits_payload(const Type *slot, const Type *got) {
    if (!slot || !got) return false;
    if (slot->kind == TY_NULLABLE) return type_same_payload(slot->elem, got);
    if (slot->kind == TY_UNION) {
        for (size_t i = 0; i < slot->nmembers; i++)
            if (type_same_payload(slot->members[i], got)) return true;
    }
    return false;
}

/* The variable was written to.
 *
 * The old value is gone, so a proof about it only holds if the new value has
 * the same shape (a string in a string slot). The proofs of the outer
 * branches are marked stale rather than removed: their place in the stack is
 * what keeps the branches apart, and a stale proof still has to say "I know
 * nothing" once the branch that made it is over. */
static void refine_assign(VarSym *v, const Type *got) {
    if (!v) return;
    /* the write is recorded even when nothing can be proven about the new
     * value: an older fact must not outlive it either way */
    v->write_seq = ++g_write_seq;
    for (int i = g_nrefines - 1; i >= 0; i--) {
        if (g_refines[i].var != v) continue;
        if (!g_refines[i].dead && got && type_same_payload(g_refines[i].type, got)) continue;
        g_refines[i].dead = true;
    }
    /* the slot holds the assigned value from here on */
    if (got && slot_admits_payload(v->type, got)) refine_push(v, got);
}

/* What a condition tells us about the variables it mentions. */
#define MAX_FACTS 8
typedef struct Fact {
    VarSym *var;
    const Type *type;      /* the type the variable has while the fact holds */
} Fact;
typedef struct FactSet {
    Fact items[MAX_FACTS];
    size_t len;
} FactSet;

static void fact_add(FactSet *fs, VarSym *v, const Type *t) {
    if (!v || !t) return;
    for (size_t i = 0; i < fs->len; i++)
        if (fs->items[i].var == v) { fs->items[i].type = t; return; }
    if (fs->len >= MAX_FACTS) return;
    fs->items[fs->len].var = v;
    fs->items[fs->len].type = t;
    fs->len++;
}

static void fact_merge(FactSet *dst, const FactSet *src) {
    for (size_t i = 0; i < src->len; i++)
        fact_add(dst, src->items[i].var, src->items[i].type);
}

/* the fact a set holds for this variable, if any */
static const Type *fact_type_of(const FactSet *fs, const VarSym *v) {
    for (size_t i = 0; i < fs->len; i++)
        if (fs->items[i].var == v) return fs->items[i].type;
    return NULL;
}

/* set the fact for a variable - a NULL type takes the fact away again */
static void fact_set(FactSet *fs, VarSym *v, const Type *t) {
    if (!v) return;
    for (size_t i = 0; i < fs->len; i++) {
        if (fs->items[i].var != v) continue;
        if (t) { fs->items[i].type = t; return; }
        fs->items[i] = fs->items[fs->len - 1];
        fs->len--;
        return;
    }
    if (!t) return;
    fact_add(fs, v, t);
}

/* Add a fact, unless the set already holds a different one for the same
 * variable.
 *
 * Several patterns in one match arm mean "any of these matched", so the arm
 * only knows what all of them agree on: an arm of `"subtle", "secondary"`
 * proves a string, while one of `is string, is int` proves nothing. */
static void fact_add_agree(FactSet *fs, VarSym *v, const Type *t) {
    if (!v || !t) return;
    const Type *have = fact_type_of(fs, v);
    if (!have) { fact_add(fs, v, t); return; }
    if (!type_same_member(have, t)) fact_set(fs, v, NULL);
}

static void push_facts(const FactSet *fs) {
    for (size_t i = 0; i < fs->len; i++)
        refine_push(fs->items[i].var, fs->items[i].type);
}

/* Can this variable be narrowed at all?
 *
 * Only a nullable/union slot is: it is the one that holds an hval, so a
 * branch can pick the payload out of it. A class slot is a plain pointer
 * and a scalar slot a plain value - there is no payload to narrow to. */
static bool slot_narrowable(const VarSym *v) {
    if (!v || !v->type) return false;
    return v->type->kind == TY_NULLABLE || v->type->kind == TY_UNION;
}

/* Does the slot hold a member of this type? (`is T` on a slot that cannot
 * hold a T is always false, so there is nothing to narrow.) */
static bool slot_admits(const Type *slot, const Type *t) {
    if (!slot || !t) return false;
    if (t->kind == TY_NULL) {
        if (slot->kind == TY_NULLABLE) return true;
        if (slot->kind == TY_UNION) {
            for (size_t i = 0; i < slot->nmembers; i++)
                if (slot->members[i] && slot->members[i]->kind == TY_NULL) return true;
        }
        return false;
    }
    if (slot->kind == TY_NULLABLE) return type_same_member(slot->elem, t);
    if (slot->kind == TY_UNION) {
        for (size_t i = 0; i < slot->nmembers; i++)
            if (type_same_member(slot->members[i], t)) return true;
    }
    return false;
}

/* PHP's falsy literals: null, false, 0, 0.0, "" and "0" - and only those.
 *
 * (A literal expression only. Anything else could be anything, and proves
 * nothing.) */
static bool literal_is_truthy(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    case EX_NULL:  return false;
    case EX_BOOL:  return e->u.boolean.b;
    case EX_INT:   return e->u.i.ival != 0;
    case EX_FLOAT: return e->u.f.fval != 0.0;
    case EX_STR: {
        const char *s = e->u.str.s;
        return s && !(s[0] == '\0' || (s[1] == '\0' && s[0] == '0'));
    }
    default: return false;
    }
}

/* The one member a nullable/union slot can hold besides null - if there is
 * exactly one and it is a scalar.
 *
 * PHP's `==` lets null take a falsy case value and every other scalar
 * member take one too (`true == "a"`, and the runtime coerces `0 == "a"`
 * through floats), so a switch arm can only be pinned to a member when null
 * is the one member the case value cannot be equal to - and a second member
 * would be a second way in. */
static const Type *sole_nonnull_scalar_member(const Type *slot) {
    if (!slot) return NULL;
    const Type *m = NULL;
    if (slot->kind == TY_NULLABLE) {
        m = slot->elem;
    } else if (slot->kind == TY_UNION) {
        for (size_t i = 0; i < slot->nmembers; i++) {
            const Type *mi = slot->members[i];
            if (!mi || mi->kind == TY_NULL) continue;
            if (m) return NULL;   /* a second member: no arm proof */
            m = mi;
        }
    } else {
        return NULL;
    }
    if (!m) return NULL;
    if (m->kind == TY_STRING || m->kind == TY_BOOL ||
        type_is_integral(m->kind) ||
        m->kind == TY_FLOAT || m->kind == TY_F32 || m->kind == TY_F64)
        return m;
    return NULL;   /* objects and arrays compare in ways not worth checking */
}

/* Collect what a condition proves.
 *
 * Only the identity comparisons are used: `$x == null` is true for "" and
 * 0 as well (PHP's loose rule), so it proves nothing. `$x === null` and
 * `$x !== null` are exact, and so is `$x is null`. */
static void collect_facts(const Expr *e, FactSet *tf, FactSet *ff) {
    if (!e) return;
    switch (e->kind) {
    case EX_BIN: {
        TokKind op = e->u.bin.op.kind;
        if (op == T_ANDAND) {
            /* A && B: what A says and what B says both hold */
            FactSet at = {0}, af = {0}, bt = {0}, bf = {0};
            collect_facts(e->u.bin.l, &at, &af);
            collect_facts(e->u.bin.r, &bt, &bf);
            fact_merge(tf, &at);
            fact_merge(tf, &bt);
        } else if (op == T_OROR) {
            /* A || B: only one of them held, so nothing follows */
            FactSet at = {0}, af = {0}, bt = {0}, bf = {0};
            collect_facts(e->u.bin.l, &at, &af);
            collect_facts(e->u.bin.r, &bt, &bf);
            fact_merge(ff, &af);
            fact_merge(ff, &bf);
        } else if (op == T_EQ2 || op == T_NEQ2) {
            const Expr *var = NULL;
            const Expr *other = NULL;
            if (e->u.bin.l->kind == EX_VAR) { var = e->u.bin.l; other = e->u.bin.r; }
            else if (e->u.bin.r->kind == EX_VAR) { var = e->u.bin.r; other = e->u.bin.l; }
            if (!var || !other) return;
            VarSym *v = (VarSym *)var->u.var.sym;
            if (!v || !slot_narrowable(v)) return;

            if (other->kind == EX_NULL) {
                /* null is the only value of its type, so the comparison says
                 * both what the variable is and what it is not */
                const Type *nn = type_without_null(v->type);
                if (op == T_EQ2) {
                    fact_add(tf, v, ty_null);
                    fact_add(ff, v, nn);
                } else {
                    fact_add(tf, v, nn);
                    fact_add(ff, v, ty_null);
                }
                return;
            }

            /* === against any other value proves only one side of it. $x === 3
             * pins $x to an int (=== compares tag and value, so no other
             * member can be equal to it), but $x !== 3 leaves $x free to hold
             * 4 - the same member. The types have to line up member for
             * member: an int pattern can never equal a float slot, so that
             * comparison proves nothing at all. */
            const Type *ot = other->type;
            if (!ot || !slot_admits(v->type, ot)) return;
            if (op == T_EQ2) fact_add(tf, v, ot);
            else fact_add(ff, v, ot);
        }
        break;
    }
    case EX_UN: {
        if (e->u.un.op.kind == T_NOT) {
            FactSet ot = {0}, of = {0};
            collect_facts(e->u.un.operand, &ot, &of);
            fact_merge(tf, &of);
            fact_merge(ff, &ot);
        }
        break;
    }
    case EX_IS: {
        const Expr *var = e->u.ischeck.operand;
        if (!var || var->kind != EX_VAR) return;
        VarSym *v = (VarSym *)var->u.var.sym;
        if (!v || !slot_narrowable(v)) return;
        const Type *to = e->u.ischeck.to;
        if (!to) return;
        if (!slot_admits(v->type, to)) return;   /* it can never be that */
        if (to->kind == TY_NULL) {
            fact_add(tf, v, ty_null);
            fact_add(ff, v, type_without_null(v->type));
        } else {
            /* the members are matched by name: `Widget|Gadget` without
             * `Widget` is `Gadget`, not an empty union */
            fact_add(tf, v, to);
            fact_add(ff, v, type_without_type(v->type, to));
        }
        break;
    }
    default: break;
    }
}

/* Does this type have a null member? */
static bool type_has_null_member(const Type *t) {
    if (!t) return false;
    if (t->kind == TY_NULL) return true;
    if (t->kind == TY_NULLABLE) return true;
    if (t->kind == TY_UNION) {
        for (size_t i = 0; i < t->nmembers; i++)
            if (type_has_null_member(t->members[i])) return true;
    }
    return false;
}

/* does this statement (or any statement nested in it) return a value? */
static bool stmt_chain_returns(Stmt *s) {
    if (!s) return false;
    switch (s->kind) {
    case ST_RETURN: return s->u.ret.value != NULL;
    case ST_IF: {
        for (size_t i = 0; i < s->u.if_.then.len; i++)
            if (stmt_chain_returns(s->u.if_.then.items[i])) return true;
        for (size_t i = 0; i < s->u.if_.els.len; i++)
            if (stmt_chain_returns(s->u.if_.els.items[i])) return true;
        return false;
    }
    case ST_WHILE:
    case ST_DO:
        for (size_t i = 0; i < s->u.while_.body.len; i++)
            if (stmt_chain_returns(s->u.while_.body.items[i])) return true;
        return false;
    case ST_FOR:
        for (size_t i = 0; i < s->u.for_.body.len; i++)
            if (stmt_chain_returns(s->u.for_.body.items[i])) return true;
        return false;
    case ST_FOREACH:
        for (size_t i = 0; i < s->u.foreach.body.len; i++)
            if (stmt_chain_returns(s->u.foreach.body.items[i])) return true;
        return false;
    case ST_TRY:
        for (size_t i = 0; i < s->u.try.body.len; i++)
            if (stmt_chain_returns(s->u.try.body.items[i])) return true;
        return false;
    case ST_BLOCK:
        for (size_t i = 0; i < s->u.block.stmts.len; i++)
            if (stmt_chain_returns(s->u.block.stmts.items[i])) return true;
        return false;
    default:
        return false;
    }
}

/* Does this statement leave the block it is in without falling off its end?
 *
 * A return, a throw, a break and a continue never reach the statement after
 * themselves, and neither does a block whose last statement does not. An if
 * only does when it has an else and both branches leave - otherwise one way
 * through it reaches the code below. Loops are not followed: a break inside
 * one belongs to the loop, not to the block around it. */
static bool stmt_always_exits(Stmt *s);

static bool block_always_exits(PtrVec stmts) {
    if (!stmts.len) return false;
    return stmt_always_exits(stmts.items[stmts.len - 1]);
}

static bool stmt_always_exits(Stmt *s) {
    if (!s) return false;
    switch (s->kind) {
    case ST_RETURN:
    case ST_THROW:
    case ST_BREAK:
    case ST_CONTINUE:
        return true;
    case ST_BLOCK:
        return block_always_exits(s->u.block.stmts);
    case ST_UNSAFE_BLOCK:
        return block_always_exits(s->u.unsafe.stmts);
    case ST_IF:
        return s->u.if_.els.len > 0 &&
               block_always_exits(s->u.if_.then) &&
               block_always_exits(s->u.if_.els);
    default:
        return false;
    }
}

static bool types_compatible(const Type *want, const Type *got);

static bool types_equal(const Type *a, const Type *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->kind != b->kind) {
        /* int fits any int width by promotion, treat widths loosely */
        if (type_is_integral(a->kind) && type_is_integral(b->kind)) return true;
        if (type_is_numeric(a->kind) && type_is_numeric(b->kind)) return true;
        if (a->kind == TY_STRING && b->kind == TY_STRING) return true;
        return false;
    }
    switch (a->kind) {
    case TY_ARRAY: case TY_VEC: case TY_SET: case TY_OPTION:
    case TY_REF: case TY_MUTREF: case TY_PTR: case TY_OWN: case TY_RC:
        /* PHP-style: a plain `array` annotation is generic; accept any
         * element type written into the annotation or inferred. */
        if (a->kind == TY_ARRAY && a->elem && a->elem->kind == TY_MIXED) return true;
        return types_equal(a->elem, b->elem);
    case TY_NULLABLE:
        return types_equal(a->elem, b->elem);
    case TY_UNION: {
        /* the same set of members, in any order */
        if (a->nmembers != b->nmembers) return false;
        for (size_t i = 0; i < a->nmembers; i++) {
            bool found = false;
            for (size_t j = 0; j < b->nmembers && !found; j++)
                if (types_equal(a->members[i], b->members[j])) found = true;
            if (!found) return false;
        }
        return true;
    }
    case TY_MAP:
        return types_equal(a->key, b->key) && types_equal(a->val, b->val);
    case TY_RESULT:
        return types_equal(a->ok, b->ok) && types_equal(a->err, b->err);
    case TY_FN: {
        if (a->fn.nparams != b->fn.nparams) return false;
        for (size_t i = 0; i < a->fn.nparams; i++)
            if (!types_equal(a->fn.params[i], b->fn.params[i])) return false;
        return types_equal(a->fn.ret, b->fn.ret);
    }
    case TY_CLASS: case TY_STRUCT: case TY_INTERFACE: case TY_ENUM:
        return a->name == b->name;
    default:
        return true;
    }
}

bool sema_type_is_linear(const Type *t) {
    if (!t) return false;
    switch (t->kind) {
    case TY_OWN:
        return true;
    case TY_ARRAY: case TY_VEC: case TY_SET: case TY_OPTION: case TY_NULLABLE:
        return sema_type_is_linear(t->elem);
    case TY_MAP:
        return sema_type_is_linear(t->key) || sema_type_is_linear(t->val);
    case TY_RESULT:
        return sema_type_is_linear(t->ok) || sema_type_is_linear(t->err);
    case TY_TUPLE: case TY_UNION:
        for (size_t i = 0; i < t->nmembers; i++)
            if (sema_type_is_linear(t->members[i])) return true;
        return false;
    default:
        return false;
    }
}

static Expr *compose_borrow_projection(const Expr *prefix, const Expr *relative) {
    if (!relative || relative->kind == EX_VAR) return (Expr *)prefix;
    Expr *base = relative->kind == EX_FIELD ? relative->u.field.obj :
                 relative->kind == EX_INDEX ? relative->u.index.obj : NULL;
    if (!base) return (Expr *)prefix;
    Expr *copy = xmalloc(sizeof(*copy));
    *copy = *relative;
    Expr *composed_base = compose_borrow_projection(prefix, base);
    if (relative->kind == EX_FIELD) copy->u.field.obj = composed_base;
    else copy->u.index.obj = composed_base;
    return copy;
}

static bool expr_is_movable_place(const Expr *e) {
    return e && (e->kind == EX_VAR || e->kind == EX_FIELD || e->kind == EX_INDEX);
}

static bool types_compatible(const Type *want, const Type *got) {
    if (!want) return true;
    if (!got) return true;
    if (want->kind == TY_MIXED) return !sema_type_is_linear(got);
    /* PHP coercive typing: a dynamically typed value (untyped property, map
     * entry, mixed param) converts to any declared type at the boundary —
     * codegen emits the hp_val_to_* conversion. */
    if (sema_type_is_linear(want) && got->kind == TY_MIXED) return false;
    if (got->kind == TY_MIXED) return true;
    if (got->kind == TY_OPTION || got->kind == TY_RESULT) {
        if (sema_type_is_linear(want)) return types_equal(want, got);
        return true;
    }
    /* `callable`/variadic fn types accept any closure */
    if (want->kind == TY_FN && want->fn.variadic && got->kind == TY_FN) return true;
    if (got->kind == TY_FN && got->fn.variadic) return true;
    /* ---- null ----
     *
     * null fits a nullable slot (?T, or a union with a null member) and a
     * class slot: a class-typed property holds a NULL pointer when it is
     * unset, and the whole library relies on that ("pub Widget $w = null;").
     * It does NOT fit a scalar or an array slot - that is the point of
     * writing ?T instead of T. */
    if (got->kind == TY_NULL) {
        if (want->kind == TY_NULL) return true;
        if (want->kind == TY_NULLABLE) return true;
        if (want->kind == TY_UNION) return type_has_null_member(want);
        if (want->kind == TY_CLASS || want->kind == TY_STRUCT ||
            want->kind == TY_INTERFACE || want->kind == TY_OWN ||
            want->kind == TY_RC || want->kind == TY_FN)
            return true;
        return false;
    }
    if (want->kind == TY_NULL) return false;   /* only null fits a null slot */
    /* ---- nullable and union slots ---- */
    if (want->kind == TY_NULLABLE) {
        const Type *el = want->elem;
        if (got->kind == TY_NULLABLE) return types_compatible(el, got->elem);
        if (got->kind == TY_UNION) {
            for (size_t i = 0; i < got->nmembers; i++)
                if (!types_compatible(el, got->members[i])) return false;
            return true;
        }
        return types_compatible(el, got);
    }
    if (want->kind == TY_UNION) {
        if (got->kind == TY_UNION) {
            for (size_t i = 0; i < got->nmembers; i++) {
                bool fits = false;
                for (size_t j = 0; j < want->nmembers && !fits; j++)
                    if (types_compatible(want->members[j], got->members[i])) fits = true;
                if (!fits) return false;
            }
            return true;
        }
        for (size_t j = 0; j < want->nmembers; j++)
            if (types_compatible(want->members[j], got)) return true;
        return false;
    }
    /* a union value never fits a single-typed slot */
    if (got->kind == TY_UNION) return false;
    if (types_equal(want, got)) return true;
    /* numeric widening */
    if (type_is_numeric(want->kind) && type_is_numeric(got->kind)) return true;
    /* own<T> is linear. Do not erase it into a copyable T slot, or invent
     * ownership by wrapping a potentially aliased T pointer. Callers must
     * preserve the ownership type at every storage and call boundary. */
    /* References must be explicit (`&$x`). Implicit auto-borrowing would
     * create a loan that has no expression or lifetime record to attach to. */
    if (want->kind == TY_MUTREF && got->kind != TY_MUTREF) return false;
    return false;
}

/* `new C(...)` is typed as a plain, copyable object handle C (see EX_NEW):
 * that is what PHP code means by it, and it may be aliased, passed and
 * stored freely. It is also the one C-typed value that may initialise an
 * explicit `own C` slot: the object was allocated by this very expression,
 * so no other handle to it exists and handing it to an exclusive owner
 * cannot create an alias. Any other C value (a variable, a field, a call
 * result) may still be aliased elsewhere, so types_compatible keeps
 * refusing C -> own C for those. */
static bool fresh_object_fits_own(const Type *want, const Expr *value) {
    if (!want || !value) return false;
    if (want->kind == TY_NULLABLE) want = want->elem;   /* ?own C */
    if (!want || want->kind != TY_OWN || !want->elem) return false;
    /* `$c ? new C() : new C()` / `$c ? new C() : null`: whichever arm runs
     * yields a fresh object (or null, which every own slot accepts). */
    if (value->kind == EX_TERNARY) {
        const Expr *arms[2] = { value->u.tern.then, value->u.tern.els };
        for (int i = 0; i < 2; i++)
            if (!arms[i] || !(arms[i]->kind == EX_NULL ||
                              fresh_object_fits_own(want, arms[i])))
                return false;
        return true;
    }
    if (value->kind != EX_NEW) return false;
    const Type *got = value->type;
    if (!got || (got->kind != TY_CLASS && got->kind != TY_STRUCT)) return false;
    return types_compatible(want->elem, got);
}

/* types_compatible() for a concrete value expression flowing into a slot of
 * type `want` (assignment, initialisation, argument, return). */
static bool value_fits_slot(const Type *want, const Type *got, const Expr *value) {
    return types_compatible(want, got) || fresh_object_fits_own(want, value);
}

/* ---------------- scopes ---------------- */
static void scope_init(Scope *s, Scope *parent) {
    memset(s, 0, sizeof(*s));
    s->parent = parent;
    s->depth = parent ? parent->depth + 1 : 0;
    s->seq = g_scope_seq++;
}

static void scope_child_init(Scope *child, Scope *parent) {
    scope_init(child, parent);
    child->cur_fn = parent->cur_fn;
    child->cur_class = parent->cur_class;
    child->in_unsafe = parent->in_unsafe;
    child->enforce_memory_safety = parent->enforce_memory_safety;
    child->loop_depth = parent->loop_depth;
    child->self_type = parent->self_type;
}

static VarSym *scope_find(Scope *s, const char *name) {
    for (Scope *sc = s; sc; sc = sc->parent) {
        for (size_t i = sc->nvars; i > 0; i--) {
            VarSym *v = sc->vars[i - 1];
            if (v->name == intern(name)) return v;
        }
    }
    return NULL;
}

static VarSym *scope_declare(Scope *s, const char *name, const Type *t, Token tok, bool is_mut) {
    if (s->nvars >= MAX_SCOPE) {
        sema_error(&tok, "too many variables in one scope");
        return NULL;
    }
    VarSym *v = xcalloc(1, sizeof(VarSym));
    v->name = intern(name);
    v->type = t;
    v->tok = tok;
    v->is_mut = is_mut;
    v->scope_depth = s->depth;
    v->uid = (int)var_uid++;
    v->decl_seq = s->seq;
    v->owner_fn = s->cur_fn;
    s->vars[s->nvars++] = v;
    return v;
}

static void validate_ref_holder_scope(VarSym *holder, const Token *tok) {
    if (!holder || !holder->borrow_origin || holder->borrow_scope_error_reported)
        return;
    const VarSym *origin = holder->borrow_origin;
    if (origin->is_program_global || origin->is_global_alias) return;
    if (holder->scope_depth >= origin->scope_depth) return;
    sema_error(tok ? tok : &holder->tok,
               "reference $%s outlives the block containing its referent $%s",
               holder->name, origin->name);
    holder->borrow_scope_error_reported = true;
}

/* ---------------- forward decls ---------------- */
static const Type *check_expr(Scope *sc, Expr *e);
static void check_stmt(Scope *sc, Stmt *s);
static const Type *resolve_type(Scope *sc, Type *t, Token tok);
static void check_fn_body(Scope *parent, AstFn *fn, const Type *self_type);
static bool fn_has_value_return(AstFn *fn);
/* number of required params: params after the first defaulted one are optional */
static size_t fn_min_args(AstFn *fn) {
    size_t n = 0;
    for (size_t i = 0; i < fn->params.len; i++) {
        Param *pm = fn->params.items[i];
        if (!pm->dflt) n = i + 1;
    }
    return n;
}
/* PHP by-ref arguments alias the caller's slot: codegen hands the callee a
 * pointer of the parameter's C type, so the argument has to BE a slot of that
 * shape (see emit_byref_arg in codegen.c). A typed `array &$a` wants the
 * caller's harr*, which a plain local already is. An untyped `&$x` wants an
 * hval slot instead, and a typed local is not one - so box it here, exactly
 * as a `use (&$x)` capture does, which is also what PHP does when such a
 * parameter gives the variable a new type. */
static void check_byref_arg(Scope *sc, Param *pm, Expr *arg) {
    if (!pm || !pm->by_ref) return;
    if (arg->kind != EX_VAR || !arg->u.var.sym) {
        /* a literal, an arithmetic result or a property has no slot to
         * hand out; PHP would copy-and-write-back, which is not done here */
        sema_error(&arg->tok, "a by-reference argument must be a variable");
        return;
    }
    const Type *pt = pm->type ? resolve_type(sc, pm->type, pm->name_tok) : NULL;
    VarSym *v = (VarSym *)arg->u.var.sym;
    if (v->type && (v->type->kind == TY_REF || v->type->kind == TY_MUTREF)) {
        sema_error(&arg->tok,
                   "cannot pass reference binding $%s itself to a PHP by-reference parameter",
                   v->name);
        return;
    }
    if (!v->is_mut)
        sema_error(&arg->tok, "cannot pass immutable $%s to a by-reference parameter", v->name);
    bool wants_hval = !pt || pt->kind == TY_MIXED || pt->kind == TY_TUPLE ||
                      pt->kind == TY_OPTION || pt->kind == TY_RESULT;
    if (!wants_hval) return;
    if (!v->is_auto || v->is_param) return;
    if (v->is_program_global || v->is_global_alias) return;  /* file-scope slot */
    if (v->captured_byref) return;                           /* already boxed */
    v->captured_byref = true;
    v->type = type_new(TY_MIXED);
}

/* Do two types describe the same slot representation?
 *
 * Array literals use this to decide their element type, and the answer has to
 * be about the *slot*, not about the Type pointer: a field read resolves its
 * annotation to a fresh Type every time, so `[$this->x, 2]` (both int) used
 * to come out mixed. Numerics stay unmixed on purpose - the runtime keeps one
 * hval per element, so [1, 2.5] must not become a float array (see the map
 * case below). */
/* A null element makes a container dynamic: the runtime keeps one hval per
 * slot, and "null" is the absence of a value rather than a value of its own
 * kind. So a literal that holds a null gets a mixed element type. */
static const Type *slot_elem_type(const Type *t) {
    return (t && t->kind == TY_NULL) ? ty_mixed : t;
}

static bool same_slot_type(const Type *a, const Type *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;    switch (a->kind) {
    case TY_CLASS: case TY_STRUCT: case TY_INTERFACE:
        if (a->name == b->name) return true;
        return a->name && b->name && strcmp(a->name, b->name) == 0;
    case TY_OWN: case TY_RC:
        return same_slot_type(a->elem, b->elem);
    case TY_ARRAY: case TY_VEC: case TY_SET:
        return same_slot_type(a->elem, b->elem);
    case TY_MAP:
        return same_slot_type(a->elem, b->elem) && same_slot_type(a->key, b->key);
    case TY_FN:
        /* closure signatures are structural; pointer identity is enough to
         * keep a mixed list of callbacks mixed */
        return a == b;
    default:
        return true;
    }
}

static const Type *check_builtin_call(Scope *sc, Expr *e, const Builtin *b);
static const char *expr_kind_name(ExprKind k);
static const char *s_this;
static const char *s_argc;
static const char *s_argv;

/* ---------------- declarations pass ---------------- */
static AstFn *class_find_method(AstClass *c, const char *name) {
    for (size_t i = 0; i < c->methods.len; i++) {
        AstFn *m = c->methods.items[i];
        if (m->name == intern(name)) return m;
    }
    return NULL;
}

static StructField *class_find_field(AstClass *c, const char *name) {
    for (AstClass *cc = c; cc; cc = cc->base)
        for (size_t i = 0; i < cc->fields.len; i++) {
            StructField *f = cc->fields.items[i];
            if (f->name == intern(name)) {
                if (!f->cls) f->cls = cc;   /* record defining class once */
                return f;
            }
        }
    return NULL;
}

static Type *class_type_of(AstClass *c) {
    Type *t = type_new(c->is_interface ? TY_INTERFACE : TY_CLASS);
    t->name = intern(c->name);
    t->decl = c;
    return t;
}

static Type *enum_type_of(AstEnum *e) {
    Type *t = type_new(TY_ENUM);
    t->name = intern(e->name);
    return t;
}

static const Type *sym_type_of_enum(AstEnum *e) {
    for (EnumSym *s = enums; s; s = s->next)
        if (s->enm == e) return s->type;
    return enum_type_of(e);
}

static FnSig *fn_make_sig(AstFn *fn) {
    FnSig *sig = xcalloc(1, sizeof(FnSig));
    sig->nparams = fn->params.len;
    sig->params = xmalloc(sizeof(Type *) * (fn->params.len ? fn->params.len : 1));
    sig->is_unsafe = fn->is_unsafe;
    sig->ret = fn->ret; /* may be NULL: inferred later */
    return sig;
}

static void declare_decls(Program *prog) {
    /* functions */
    for (size_t i = 0; i < prog->decls.len; i++) {
        Decl *d = prog->decls.items[i];
        if (d->kind != DK_FN) continue;
        AstFn *fn = d->u.fn;
        if (fn->name == intern("main")) {
            if (fn->params.len != 0)
                sema_error(&fn->name_tok, "main() takes no parameters");
            if (fn->ret && fn->ret->kind != TY_VOID && fn->ret->kind != TY_INT)
                sema_error(&fn->name_tok, "main() returns void or int");
        }
        FnSym *sym = xcalloc(1, sizeof(FnSym));
        sym->name = intern(fn->name);
        sym->fn = fn;
        sym->sig = fn_make_sig(fn);
        sym->next = fns;
        fns = sym;
        fn->ftype = type_new(TY_FN);
    }
    /* classes / interfaces / traits */
    for (size_t i = 0; i < prog->decls.len; i++) {
        Decl *d = prog->decls.items[i];
        if (d->kind != DK_CLASS && d->kind != DK_INTERFACE && d->kind != DK_TRAIT) continue;
        AstClass *c = d->u.cls;
        ClassSym *sym = xcalloc(1, sizeof(ClassSym));
        sym->name = intern(c->name);
        sym->cls = c;
        sym->next = classes;
        classes = sym;
    }
    /* enums */
    for (size_t i = 0; i < prog->decls.len; i++) {
        Decl *d = prog->decls.items[i];
        if (d->kind != DK_ENUM) continue;
        AstEnum *en = d->u.enm;
        EnumSym *sym = xcalloc(1, sizeof(EnumSym));
        sym->name = intern(en->name);
        sym->enm = en;
        sym->type = enum_type_of(en);
        sym->next = enums;
        enums = sym;
    }
    /* resolve bases */
    for (ClassSym *cs = classes; cs; cs = cs->next) {
        AstClass *c = cs->cls;
        if (c->extends) {
            for (ClassSym *b = classes; b; b = b->next) {
                if (b->name == intern(c->extends)) {
                    c->base = b->cls;
                    break;
                }
            }
            if (!c->base)
                sema_error(&c->name_tok, "unknown base class '%s'", c->extends);
        }
    }
}

static AstFn *find_function(const char *name) {
    for (FnSym *s = fns; s; s = s->next)
        if (s->name == intern(name)) return s->fn;
    return NULL;
}

static int returned_ref_param_expr(AstFn *fn, Expr *e) {
    if (!fn || !e) return -1;
    if (e->kind == EX_TERNARY) {
        int then_source = returned_ref_param_expr(fn, e->u.tern.then);
        int else_source = returned_ref_param_expr(fn, e->u.tern.els);
        return then_source == else_source ? then_source : -2;
    }
    if (e->kind == EX_MATCH) {
        bool seen = false;
        int source = -1;
        for (size_t i = 0; i < e->u.matchexpr.cases.len; i++) {
            MatchCase *arm = e->u.matchexpr.cases.items[i];
            int arm_source = returned_ref_param_expr(fn, arm->body);
            if (!seen) {
                source = arm_source;
                seen = true;
            } else if (source != arm_source) {
                return -2;
            }
        }
        if (e->u.matchexpr.dflt) {
            int default_source = returned_ref_param_expr(fn, e->u.matchexpr.dflt);
            if (seen && source != default_source) return -2;
            source = default_source;
            seen = true;
        }
        return seen ? source : -2;
    }
    /* A returned local reference can be a reborrow of a parameter, e.g.
     * `let $r: &mut int = $x; return $r;`. Follow the semantic borrow origin
     * instead of requiring the source spelling to be the parameter name. */
    VarSym *value = e->kind == EX_VAR ? (VarSym *)e->u.var.sym :
                    e->borrow_origin ? (VarSym *)e->borrow_origin : NULL;
    if (!value && (e->kind == EX_BORROW || e->kind == EX_MUTBORROW))
        value = expr_root_var(e->u.borrow.operand);
    if (value && value->borrow_pending) return -2;
    VarSym *origin = value && value->borrow_origin
        ? (VarSym *)value->borrow_origin : value;
    if (fn->this_sym && origin == fn->this_sym) return -3; /* implicit receiver */
    for (size_t i = 0; i < fn->params.len; i++) {
        Param *p = fn->params.items[i];
        VarSym *param = (VarSym *)p->sym;
        bool reference_param = p->by_ref || (p->type &&
            (p->type->kind == TY_REF || p->type->kind == TY_MUTREF));
        if (!origin || origin != param || !reference_param) continue;
        return (int)i;
    }
    return -2;
}

static void scan_return_ref_param_seq(AstFn *fn, PtrVec stmts, int *found) {
    for (size_t i = 0; i < stmts.len; i++) {
        Stmt *s = stmts.items[i];
        if (!s) continue;
        if (s->kind == ST_RETURN) {
            if (!s->u.ret.value) continue;
            int idx = returned_ref_param_expr(fn, s->u.ret.value);
            if (idx >= 0 || idx == -3) {
                if (*found == -1 || *found == idx) *found = idx;
                else *found = -2;
            } else if (fn->ret && (fn->ret->kind == TY_REF || fn->ret->kind == TY_MUTREF)) {
                *found = -2;
            }
        } else if (s->kind == ST_IF) {
            scan_return_ref_param_seq(fn, s->u.if_.then, found);
            scan_return_ref_param_seq(fn, s->u.if_.els, found);
        } else if (s->kind == ST_WHILE) scan_return_ref_param_seq(fn, s->u.while_.body, found);
        else if (s->kind == ST_DO) scan_return_ref_param_seq(fn, s->u.do_.body, found);
        else if (s->kind == ST_FOR) scan_return_ref_param_seq(fn, s->u.for_.body, found);
        else if (s->kind == ST_FOREACH) scan_return_ref_param_seq(fn, s->u.foreach.body, found);
        else if (s->kind == ST_BLOCK) scan_return_ref_param_seq(fn, s->u.block.stmts, found);
        else if (s->kind == ST_UNSAFE_BLOCK) scan_return_ref_param_seq(fn, s->u.unsafe.stmts, found);
        else if (s->kind == ST_TRY) {
            scan_return_ref_param_seq(fn, s->u.try.body, found);
            scan_return_ref_param_seq(fn, s->u.try.fin, found);
            for (size_t j = 0; j < s->u.try.catches.len; j++)
                scan_return_ref_param_seq(fn, ((CatchClause *)s->u.try.catches.items[j])->body, found);
        } else if (s->kind == ST_SWITCH) {
            for (size_t j = 0; j < s->u.switch_.cases.len; j++)
                scan_return_ref_param_seq(fn, ((SwitchCase *)s->u.switch_.cases.items[j])->body, found);
        }
    }
}

static int fn_return_ref_param(AstFn *fn) {
    if (!fn || !fn->ret || (fn->ret->kind != TY_REF && fn->ret->kind != TY_MUTREF)) return -1;
    int found = -1;
    scan_return_ref_param_seq(fn, fn->body, &found);
    return found;
}

static bool same_receiver_projection(const Expr *a, const Expr *b) {
    if (a == b) return true;
    if (!a || !b || a->kind != b->kind) return false;
    if (a->kind == EX_VAR) {
        if (a->u.var.sym == b->u.var.sym) return true;
        VarSym *av = (VarSym *)a->u.var.sym;
        VarSym *bv = (VarSym *)b->u.var.sym;
        return av && bv && av->name == s_this && bv->name == s_this;
    }
    if (a->kind == EX_FIELD)
        return a->u.field.name && b->u.field.name &&
               strcmp(a->u.field.name, b->u.field.name) == 0 &&
               same_receiver_projection(a->u.field.obj, b->u.field.obj);
    if (a->kind == EX_INDEX)
        return a->u.index.idx == b->u.index.idx &&
               same_receiver_projection(a->u.index.obj, b->u.index.obj);
    if (a->kind == EX_DEREF)
        return same_receiver_projection(a->u.deref.operand, b->u.deref.operand);
    /* Index projections need value-sensitive alias reasoning; use the whole
     * receiver if separate return branches select indexed places. */
    return false;
}

static const Expr *returned_ref_place(const Expr *e) {
    if (!e) return NULL;
    if (e->borrow_place) return e->borrow_place;
    if (e->kind == EX_VAR && e->u.var.sym)
        return ((VarSym *)e->u.var.sym)->borrow_place;
    if (e->kind == EX_BORROW || e->kind == EX_MUTBORROW)
        return e->u.borrow.operand;
    return NULL;
}

static void scan_receiver_return_places(PtrVec stmts, AstFn *fn,
                                        const Expr **place, bool *seen,
                                        bool *ambiguous) {
    for (size_t i = 0; i < stmts.len; i++) {
        Stmt *s = stmts.items[i];
        if (!s) continue;
        if (s->kind == ST_RETURN && s->u.ret.value &&
            returned_ref_param_expr(fn, s->u.ret.value) == -3) {
            const Expr *candidate = returned_ref_place(s->u.ret.value);
            if (!*seen) { *place = candidate; *seen = true; }
            else if (!same_receiver_projection(*place, candidate)) *ambiguous = true;
        }
        switch (s->kind) {
        case ST_IF:
            scan_receiver_return_places(s->u.if_.then, fn, place, seen, ambiguous);
            scan_receiver_return_places(s->u.if_.els, fn, place, seen, ambiguous);
            break;
        case ST_WHILE: scan_receiver_return_places(s->u.while_.body, fn, place, seen, ambiguous); break;
        case ST_DO: scan_receiver_return_places(s->u.do_.body, fn, place, seen, ambiguous); break;
        case ST_FOR:
            scan_receiver_return_places(s->u.for_.init, fn, place, seen, ambiguous);
            scan_receiver_return_places(s->u.for_.step, fn, place, seen, ambiguous);
            scan_receiver_return_places(s->u.for_.body, fn, place, seen, ambiguous);
            break;
        case ST_FOREACH: scan_receiver_return_places(s->u.foreach.body, fn, place, seen, ambiguous); break;
        case ST_BLOCK: scan_receiver_return_places(s->u.block.stmts, fn, place, seen, ambiguous); break;
        case ST_UNSAFE_BLOCK: scan_receiver_return_places(s->u.unsafe.stmts, fn, place, seen, ambiguous); break;
        case ST_TRY:
            scan_receiver_return_places(s->u.try.body, fn, place, seen, ambiguous);
            scan_receiver_return_places(s->u.try.fin, fn, place, seen, ambiguous);
            for (size_t j = 0; j < s->u.try.catches.len; j++)
                scan_receiver_return_places(((CatchClause *)s->u.try.catches.items[j])->body,
                                            fn, place, seen, ambiguous);
            break;
        case ST_SWITCH:
            for (size_t j = 0; j < s->u.switch_.cases.len; j++)
                scan_receiver_return_places(((SwitchCase *)s->u.switch_.cases.items[j])->body,
                                            fn, place, seen, ambiguous);
            break;
        default: break;
        }
    }
}

static const Expr *fn_return_receiver_place(AstFn *fn) {
    if (!fn || fn_return_ref_param(fn) != -3) return NULL;
    if (!fn->borrow_return_place_resolved) {
        const Expr *place = NULL;
        bool seen = false, ambiguous = false;
        scan_receiver_return_places(fn->body, fn, &place, &seen, &ambiguous);
        fn->borrow_return_place = seen && !ambiguous ? place : NULL;
        fn->borrow_return_place_resolved = true;
    }
    return fn->borrow_return_place;
}

static VarSym *ref_return_origin(Expr *e) {
    if (!e) return NULL;
    if (e->borrow_origin) return (VarSym *)e->borrow_origin;
    if (e->kind == EX_VAR) {
        VarSym *v = (VarSym *)e->u.var.sym;
        return v && v->borrow_origin ? (VarSym *)v->borrow_origin : v;
    }
    if (e->kind == EX_BORROW || e->kind == EX_MUTBORROW) {
        Expr *operand = e->u.borrow.operand;
        if (operand && operand->kind == EX_VAR)
            return (VarSym *)operand->u.var.sym;
    }
    return NULL;
}

static bool is_global_lifetime_origin(const VarSym *v) {
    return v && (v->is_program_global || v->is_global_alias);
}

static void scan_return_ref_global_seq(AstFn *fn, PtrVec stmts,
                                       VarSym **found, bool *ambiguous) {
    (void)fn;
    for (size_t i = 0; i < stmts.len; i++) {
        Stmt *s = stmts.items[i];
        if (!s) continue;
        if (s->kind == ST_RETURN && s->u.ret.value) {
            const Type *t = s->u.ret.value->type;
            if (!t || (t->kind != TY_REF && t->kind != TY_MUTREF)) continue;
            VarSym *origin = ref_return_origin(s->u.ret.value);
            if (!is_global_lifetime_origin(origin)) {
                *ambiguous = true;
            } else if (!*found || (*found)->uid == origin->uid) {
                *found = origin;
            } else {
                *ambiguous = true;
            }
        } else if (s->kind == ST_IF) {
            scan_return_ref_global_seq(fn, s->u.if_.then, found, ambiguous);
            scan_return_ref_global_seq(fn, s->u.if_.els, found, ambiguous);
        } else if (s->kind == ST_WHILE) scan_return_ref_global_seq(fn, s->u.while_.body, found, ambiguous);
        else if (s->kind == ST_DO) scan_return_ref_global_seq(fn, s->u.do_.body, found, ambiguous);
        else if (s->kind == ST_FOR) scan_return_ref_global_seq(fn, s->u.for_.body, found, ambiguous);
        else if (s->kind == ST_FOREACH) scan_return_ref_global_seq(fn, s->u.foreach.body, found, ambiguous);
        else if (s->kind == ST_BLOCK) scan_return_ref_global_seq(fn, s->u.block.stmts, found, ambiguous);
        else if (s->kind == ST_UNSAFE_BLOCK) scan_return_ref_global_seq(fn, s->u.unsafe.stmts, found, ambiguous);
        else if (s->kind == ST_TRY) {
            scan_return_ref_global_seq(fn, s->u.try.body, found, ambiguous);
            scan_return_ref_global_seq(fn, s->u.try.fin, found, ambiguous);
            for (size_t j = 0; j < s->u.try.catches.len; j++)
                scan_return_ref_global_seq(fn, ((CatchClause *)s->u.try.catches.items[j])->body,
                                           found, ambiguous);
        } else if (s->kind == ST_SWITCH) {
            for (size_t j = 0; j < s->u.switch_.cases.len; j++)
                scan_return_ref_global_seq(fn, ((SwitchCase *)s->u.switch_.cases.items[j])->body,
                                           found, ambiguous);
        }
    }
}

static VarSym *fn_return_ref_global(AstFn *fn) {
    if (!fn || !fn->ret || (fn->ret->kind != TY_REF && fn->ret->kind != TY_MUTREF))
        return NULL;
    VarSym *found = NULL;
    bool ambiguous = false;
    scan_return_ref_global_seq(fn, fn->body, &found, &ambiguous);
    return ambiguous ? NULL : found;
}

/* Virtual dispatch may select an override at runtime, while a call site's
 * borrow summary is derived from its statically resolved method. Require an
 * override to preserve the inherited reference source so that summary stays
 * valid for every possible receiver class. */
static void validate_virtual_ref_return_contracts(void) {
    for (size_t i = 0; i < g_borrow_fns.len; i++) {
        AstFn *method = g_borrow_fns.items[i];
        if (!method || !method->is_method || !method->cls || !method->cls->base)
            continue;
        AstFn *inherited = NULL;
        for (AstClass *base = method->cls->base; base && !inherited; base = base->base)
            inherited = class_find_method(base, method->name);
        if (!inherited || !inherited->ret || !method->ret) continue;
        bool inherited_ref = inherited->ret->kind == TY_REF ||
                             inherited->ret->kind == TY_MUTREF;
        bool method_ref = method->ret->kind == TY_REF ||
                          method->ret->kind == TY_MUTREF;
        if (!inherited_ref && !method_ref) continue;
        if (!inherited_ref || !method_ref ||
            !types_compatible(inherited->ret, method->ret)) {
            sema_error(&method->name_tok,
                       "override %s::%s() must preserve the inherited reference return type",
                       method->cls->name, method->name);
            continue;
        }

        int inherited_param = fn_return_ref_param(inherited);
        int method_param = fn_return_ref_param(method);
        VarSym *inherited_global = fn_return_ref_global(inherited);
        VarSym *method_global = fn_return_ref_global(method);
        bool inherited_known = inherited_param >= 0 || inherited_param == -3 ||
                               inherited_global;
        if (!inherited_known) continue; /* an abstract/ambiguous base has no usable summary */

        bool same_source = inherited_param == -3
            ? method_param == -3 && !method_global
            : inherited_param >= 0
            ? method_param == inherited_param && !method_global
            : method_global && method_global->uid == inherited_global->uid &&
              method_param < 0;
        if (!same_source)
            sema_error(&method->name_tok,
                       "override %s::%s() must return a reference from the same parameter or global as the inherited method",
                       method->cls->name, method->name);
        else if (inherited_param == -3) {
            const Expr *base_place = fn_return_receiver_place(inherited);
            const Expr *override_place = fn_return_receiver_place(method);
            /* A broad base summary (NULL) already covers every receiver
             * projection. A precise base summary requires every override to
             * preserve that projection or a base-typed call could undertrack. */
            if (base_place && !same_receiver_projection(base_place, override_place))
                sema_error(&method->name_tok,
                           "override %s::%s() must preserve the inherited receiver field borrow",
                           method->cls->name, method->name);
        }
    }
}

static AstFn *ref_return_target(const Expr *e) {
    if (!e) return NULL;
    switch (e->kind) {
    case EX_CALL: return e->u.call.target;
    case EX_METHOD: return e->u.method.target;
    case EX_STATIC: return e->u.staticcall.target;
    default: return NULL;
    }
}

static bool target_returns_ref(const AstFn *fn) {
    return fn && fn->ret &&
        (fn->ret->kind == TY_REF || fn->ret->kind == TY_MUTREF);
}

static AstClass *find_class(const char *name) {
    for (ClassSym *s = classes; s; s = s->next)
        if (s->name == intern(name)) return s->cls;
    return NULL;
}

static AstEnum *find_enum(const char *name) {
    for (EnumSym *s = enums; s; s = s->next)
        if (s->name == intern(name)) return s->enm;
    return NULL;
}

static bool class_derives(AstClass *c, const AstClass *base) {
    for (AstClass *b = c; b; b = b->base)
        if (b == base) return true;
    return false;
}

/* ---------------- type resolution ---------------- */
static bool type_holds_i128(const Type *t) {
    if (!t) return false;
    switch (t->kind) {
    case TY_I128: return true;
    case TY_ARRAY: case TY_VEC: case TY_SET: case TY_OPTION: case TY_NULLABLE:
    case TY_OWN: case TY_RC:
        return type_holds_i128(t->elem);
    case TY_MAP: return type_holds_i128(t->key) || type_holds_i128(t->val);
    case TY_RESULT: return type_holds_i128(t->ok) || type_holds_i128(t->err);
    case TY_UNION:
        for (size_t i = 0; i < t->nmembers; i++)
            if (type_holds_i128(t->members[i])) return true;
        return false;
    default: return false;
    }
}

static const Type *resolve_type(Scope *sc, Type *t, Token tok) {
    (void)sc;
    if (!t) return NULL;
    switch (t->kind) {
    case TY_ARRAY: t->elem = resolve_type(sc, (Type *)t->elem, tok); break;
    case TY_VEC: case TY_SET: case TY_OPTION:
        t->elem = resolve_type(sc, (Type *)t->elem, tok); break;
    case TY_NULLABLE:
        t->elem = resolve_type(sc, (Type *)t->elem, tok); break;
    case TY_UNION:
        for (size_t i = 0; i < t->nmembers; i++)
            ((Type **)t->members)[i] = resolve_type(sc, (Type *)t->members[i], tok);
        break;
    case TY_MAP:
        t->key = resolve_type(sc, (Type *)t->key, tok);
        t->val = resolve_type(sc, (Type *)t->val, tok); break;
    case TY_RESULT:
        t->ok = resolve_type(sc, (Type *)t->ok, tok);
        t->err = resolve_type(sc, (Type *)t->err, tok); break;
    case TY_REF: case TY_MUTREF: case TY_PTR: case TY_OWN: case TY_RC:
        t->elem = resolve_type(sc, (Type *)t->elem, tok); break;
    case TY_FN: {
        for (size_t i = 0; i < t->fn.nparams; i++)
            ((Type **)t->fn.params)[i] = resolve_type(sc, (Type *)t->fn.params[i], tok);
        t->fn.ret = resolve_type(sc, (Type *)t->fn.ret, tok);
        break;
    }
    case TY_CLASS: {
        if (strcmp(t->name, "self") == 0 && sc && sc->self_type) return sc->self_type;
        /* builtin Exception type (thrown by the runtime, caught by user code) */
        if (strcmp(t->name, "Exception") == 0) return t;
        AstClass *c = find_class(t->name);
        if (c) {
            t->decl = c;
            t->name = intern(c->name);
        } else {
            AstEnum *en = find_enum(t->name);
            if (en) {
                t->kind = TY_ENUM;
                t->name = intern(en->name);
            } else {
                sema_error(&tok, "unknown type '%s'", t->name);
                t->kind = TY_ERROR;
            }
        }
        break;
    }
    default: break;
    }
    switch (t->kind) {
    case TY_ARRAY: case TY_VEC: case TY_MAP: case TY_SET:
    case TY_OPTION: case TY_RESULT: case TY_NULLABLE: case TY_UNION:
        if (type_holds_i128(t))
            sema_error(&tok,
                       "i128 cannot be stored in %s; arrays and dynamic values hold 64-bit integers (convert with (int) or strval() first)",
                       type_to_string(t));
        break;
    default: break;
    }
    return t;
}

static bool borrow_origin_outlives_function(const VarSym *v) {
    return is_global_lifetime_origin(v) || (v && v->is_param &&
           (v->is_param_ref || (v->type &&
            (v->type->kind == TY_REF || v->type->kind == TY_MUTREF))));
}

static const Type *deref_pointerish(const Type *t) {
    if (!t) return NULL;
    if (t->kind == TY_REF || t->kind == TY_MUTREF || t->kind == TY_PTR)
        return t->elem;
    return t;
}

static const Type *check_bin(Scope *sc, Expr *e) {
    TokKind op = e->u.bin.op.kind;
    const Type *lt, *rt;
    if (op == T_ANDAND) {
        /* the right side only runs when the left one held, so what the left
         * one proves is true there ("$x !== null && strlen($x) > 0") */
        lt = check_expr(sc, e->u.bin.l);
        FactSet tf = {0}, ff = {0};
        collect_facts(e->u.bin.l, &tf, &ff);
        int mark = refine_mark();
        push_facts(&tf);
        rt = check_expr(sc, e->u.bin.r);
        refine_restore(mark);
    } else {
        lt = check_expr(sc, e->u.bin.l);
        rt = check_expr(sc, e->u.bin.r);
    }
    /* null coalescing */
    if (e->is_nullcoal) {
        if ((lt && lt->kind == TY_I128) || (rt && rt->kind == TY_I128))
            sema_error(&e->tok,
                       "i128 cannot flow through ??; convert with (int) or strval() first");
        e->type = lt;
        return e->type;
    }
    /* string concat */
    if (op == T_DOT) {
        e->type = ty_string;
        return e->type;
    }
    /* comparisons */
    if (op == T_EQ || op == T_NEQ || op == T_NEQ2 || op == T_EQ2 || op == T_LT ||
        op == T_GT || op == T_LE || op == T_GE) {
        if (type_is_numeric(lt->kind) && type_is_numeric(rt->kind))
            unify_numeric(lt, rt);
        e->type = ty_bool;
        return e->type;
    }
    /* spaceship yields int -1/0/1 in PHP (not bool) */
    if (op == T_SPACESHIP) {
        if (type_is_numeric(lt->kind) && type_is_numeric(rt->kind))
            unify_numeric(lt, rt);
        e->type = ty_int;
        return e->type;
    }
    /* logical */
    if (op == T_ANDAND || op == T_OROR) {
        e->type = ty_bool;
        return e->type;
    }
    /* shifts << >> — the result is always an integer; numeric strings and
     * hval operands are converted by the runtime (PHP semantics). An i128
     * operand keeps the result 128 bits wide. */
    if (op == T_SHL || op == T_SHR) {
        bool lok = type_is_numeric(lt->kind) || lt->kind == TY_STRING || lt->kind == TY_MIXED;
        bool rok = type_is_numeric(rt->kind) || rt->kind == TY_STRING || rt->kind == TY_MIXED;
        if (!lok || !rok)
            sema_error(&e->u.bin.op, "%s requires numeric operands, got %s and %s",
                       tok_kind_name(op), type_to_string(lt), type_to_string(rt));
        e->type = (lt->kind == TY_I128 || rt->kind == TY_I128) ? ty_i128 : ty_int;
        return e->type;
    }
    /* bitwise */
    if (op == T_AMP || op == T_PIPE || op == T_CARET) {
        if (!type_is_integral(lt->kind) || !type_is_integral(rt->kind)) {
            sema_error(&e->u.bin.op, "bitwise operator requires integer operands, got %s and %s",
                       type_to_string(lt), type_to_string(rt));
        }
        e->type = unify_numeric(lt, rt);
        return e->type;
    }
    /* arithmetic */
    if (op == T_PLUS || op == T_MINUS || op == T_STAR || op == T_SLASH ||
        op == T_PERCENT || op == T_POW) {
        if (type_is_numeric(lt->kind) && type_is_numeric(rt->kind)) {
            /* i128 / i128 is exact 128-bit integer division, not PHP's
             * float division: converting to double would throw away the
             * precision the type exists for. Mixed with a float stays
             * float. */
            bool wide_div = op == T_SLASH &&
                type_is_integral(lt->kind) && type_is_integral(rt->kind) &&
                (lt->kind == TY_I128 || rt->kind == TY_I128);
            e->type = (op == T_SLASH && !wide_div) ? ty_float : unify_numeric(lt, rt);
            return e->type;
        }
        /* PHP numeric-string arithmetic: "1" + "2" is 3, "1.5" * 2 is
         * 3.0, "abc" + "x" (no numeric meaning) stays our friendly
         * concatenation. The runtime picks int vs float from the values,
         * so the static type is hval — not a guaranteed hstr*. */
        if (op != T_PERCENT && op != T_POW &&
            (lt->kind == TY_STRING || rt->kind == TY_STRING) &&
            (type_is_numeric(lt->kind) || lt->kind == TY_STRING) &&
            (type_is_numeric(rt->kind) || rt->kind == TY_STRING)) {
            e->type = ty_mixed;
            return e->type;
        }
        if (lt->kind == TY_MIXED || rt->kind == TY_MIXED) {
            /* PHP-style: mixed operands (e.g. untyped closure params)
             * flow through the generic runtime arithmetic. */
            e->type = op == T_SLASH ? ty_float : ty_mixed;
            return e->type;
        }
        sema_error(&e->u.bin.op, "unsupported operand types for '%s': %s and %s",
                   tok_kind_name(op), type_to_string(lt), type_to_string(rt));
        e->type = ty_mixed;
        return e->type;
    }
    /* slice a[lo..hi] */
    if (e->is_slice) {
        e->type = lt;
        return e->type;
    }
    e->type = ty_mixed;
    return e->type;
}

static const Type *elem_type_of(const Type *t, const Type *idx) {
    if (!t) return ty_mixed;
    switch (t->kind) {
    case TY_ARRAY: case TY_VEC: case TY_SET:
        return t->elem;
    case TY_MAP:
        return t->val;
    case TY_STRING:
        return ty_string;
    case TY_TUPLE:
        return ty_mixed;
    default:
        (void)idx;
        return NULL;
    }
}

static const Type *check_method_call(Scope *sc, Expr *e) {
    Expr *obj = e->u.method.obj;
    const Type *ot = check_expr(sc, obj);
    const char *mname = e->u.method.name;
    const Type *receiver_base = ot;
    while (receiver_base && (receiver_base->kind == TY_REF ||
                             receiver_base->kind == TY_MUTREF ||
                             receiver_base->kind == TY_OWN ||
                             receiver_base->kind == TY_RC))
        receiver_base = receiver_base->elem;
    /* builtin Exception support: getMessage() */
    if (receiver_base && receiver_base->kind == TY_CLASS &&
        receiver_base->name == intern("Exception")) {
        if (strcmp(mname, "getMessage") == 0) {
            e->builtin = "exception_get_message";
            e->type = ty_string;
            return e->type;
        }
    }
    if (receiver_base && (receiver_base->kind == TY_ARRAY || receiver_base->kind == TY_VEC ||
                          receiver_base->kind == TY_MAP || receiver_base->kind == TY_SET)) {
        /* collection builtins */
        static const struct { const char *n; const Type *ret; } coll[] = {
            {"len", NULL}, {"push", NULL}, {"pop", NULL},
            {"get", NULL}, {"has", NULL}, {"keys", NULL}, {"values", NULL},
            {NULL, NULL},
        };
        (void)coll;
        if (strcmp(mname, "len") == 0) { e->builtin = "count"; e->type = ty_int; return e->type; }
        if (strcmp(mname, "push") == 0) {
            if (e->u.method.args.len != 1)
                sema_error(&e->u.method.name_tok, "push() expects exactly one value");
            for (size_t i = 0; i < e->u.method.args.len; i++) {
                Expr *arg = e->u.method.args.items[i];
                const Type *at = check_expr(sc, arg);
                reject_untracked_reference(arg, at, NULL, "collection push()");
                reject_i128_lossy(&arg->tok, ty_mixed, at, "value");
                if (sema_type_is_linear(at)) {
                    if (!sema_type_is_linear(receiver_base->kind == TY_MAP
                                                ? receiver_base->val : receiver_base->elem))
                        sema_error(&arg->tok, "cannot insert an ownership value into a copyable collection");
                    else if (expr_is_movable_place(arg)) arg->is_move_source = true;
                }
            }
            e->builtin = "array_push"; e->type = ty_int; return e->type;
        }
        if (strcmp(mname, "pop") == 0) { e->builtin = "array_pop"; e->type = elem_type_of(receiver_base, NULL); return e->type ? e->type : ty_mixed; }
        if (strcmp(mname, "get") == 0) {
            e->builtin = "array_get";
            e->type = elem_type_of(receiver_base, NULL) ? elem_type_of(receiver_base, NULL) : ty_mixed;
            if (sema_type_is_linear(e->type))
                sema_error(&e->u.method.name_tok, "get() cannot copy an ownership value; use pop() or index move syntax");
            return e->type;
        }
        if (strcmp(mname, "has") == 0) { e->builtin = "array_key_exists"; e->type = ty_bool; return e->type; }
        if (strcmp(mname, "keys") == 0) { e->builtin = "array_keys"; e->type = ty_mixed; return e->type; }
        if (strcmp(mname, "values") == 0) {
            if (sema_type_is_linear(receiver_base))
                sema_error(&e->u.method.name_tok, "values() cannot copy a collection containing ownership values");
            e->builtin = "array_values"; e->type = ty_mixed; return e->type;
        }
    }
    /* smart pointer auto-deref */
    if (ot->kind == TY_OWN || ot->kind == TY_RC || ot->kind == TY_REF || ot->kind == TY_MUTREF) {
        const Type *inner = ot->elem;
        if (inner && (inner->kind == TY_CLASS || inner->kind == TY_STRUCT)) {
            AstClass *c = inner->decl;
            if (!c) {
                AstClass *cc = find_class(inner->name);
                inner = NULL;
                if (cc) {
                    ((Type *)ot->elem)->decl = cc;
                    inner = ot->elem;
                    c = cc;
                }
            }
            if (c) {
                AstFn *m = class_find_method(c, mname);
                if (!m && c->base) m = class_find_method(c->base, mname);
                if (m) {
                    e->u.method.target = m;
                    /* check args against params */
                    Scope dummy;
                    scope_init(&dummy, NULL);                     size_t na = e->u.method.args.len;
                     size_t nreq = fn_min_args(m);
                     if (na < nreq || na > m->params.len)
                         sema_error(&e->u.method.name_tok, "method %s::%s() expects %zu..%zu argument(s), got %zu",
                                    c->name, mname, nreq, m->params.len, na);
                    for (size_t i = 0; i < na && i < m->params.len; i++) {
                        Param *pm = m->params.items[i];
                        const Type *pt = resolve_type(sc, pm->type, pm->name_tok);
                        Expr *arg = e->u.method.args.items[i];
                        const Type *at = check_expr(sc, arg);
                        if (pt && pt->kind == TY_FN && pt->thread_safe_callback)
                            validate_thread_callback(arg, at, &arg->tok, mname);
                        reject_untracked_reference(arg, at, pt, mname);
                        reject_i128_lossy(&arg->tok, pt ? pt : ty_mixed, at, "argument");
                        if (pt && !value_fits_slot(pt, at, arg))
                            sema_error(&arg->tok, "argument %zu to %s::%s(): expected %s, got %s",
                                       i + 1, c->name, mname, type_to_string(pt), type_to_string(at));
                        check_byref_arg(sc, pm, arg);
                    }
                    if (!m->ret) m->ret = fn_has_value_return(m) ? ty_mixed : ty_void;
                    e->type = resolve_type(sc, m->ret, m->name_tok);
                    return e->type;
                }
            }
        }
    }
    /* plain object */
    if (ot->kind == TY_CLASS || ot->kind == TY_STRUCT) {
        AstClass *c = ot->decl;
        if (!c) c = find_class(ot->name);
        if (!c) {
            sema_error(&e->u.method.name_tok, "call to method '%s' on unknown type %s",
                       mname, type_to_string(ot));
            e->type = ty_mixed;
            return e->type;
        }
        AstFn *m = class_find_method(c, mname);
        if (!m && c->base) m = class_find_method(c->base, mname);
        if (!m) {
            /* PHP-style callable property: $this->onMessage($client, $msg).
             * No method of that name — if a field holds a closure or an
             * untyped value, this is a dynamic call. */
            StructField *f = class_find_field(c, mname);
            if (!f && c->base) f = class_find_field(c->base, mname);
            if (f) {
                for (size_t i = 0; i < e->u.method.args.len; i++) {
                    Expr *arg = e->u.method.args.items[i];
                    const Type *at = check_expr(sc, arg);
                    if (sema_type_is_linear(at) &&
                        (arg->kind == EX_VAR || arg->kind == EX_FIELD || arg->kind == EX_INDEX))
                        sema_error(&arg->tok, "cannot pass an ownership value to a dynamically typed method");
                    reject_untracked_reference(arg, at, NULL, "a dynamic method call");
                    reject_i128_lossy(&arg->tok, ty_mixed, at, "argument");
                }
                e->u.method.sf = f;
                e->u.method.target = NULL;
                e->type = (f->type && f->type->kind == TY_FN && f->type->fn.ret)
                              ? f->type->fn.ret : ty_mixed;
                return e->type;
            }
            sema_error(&e->u.method.name_tok, "no method '%s' on class %s", mname, c->name);
            e->type = ty_mixed;
            return e->type;
        }
        e->u.method.target = m;
        size_t na = e->u.method.args.len;
        size_t nreq = fn_min_args(m);
        if (na < nreq || na > m->params.len)
            sema_error(&e->u.method.name_tok, "method %s::%s() expects %zu..%zu argument(s), got %zu",
                       c->name, mname, nreq, m->params.len, na);
        for (size_t i = 0; i < na && i < m->params.len; i++) {
            Param *pm = m->params.items[i];
            const Type *pt = resolve_type(sc, pm->type, pm->name_tok);
            Expr *arg = e->u.method.args.items[i];
            const Type *at = check_expr(sc, arg);
            if (pt && pt->kind == TY_FN && pt->thread_safe_callback)
                validate_thread_callback(arg, at, &arg->tok, mname);
            if (c->name && strcmp(c->name, "Channel") == 0 && i == 0 &&
                (strcmp(mname, "send") == 0 || strcmp(mname, "trySend") == 0)) {
                if (!thread_sendable_value(at)) {
                    sema_error(&arg->tok, "Channel::%s() cannot send non-Sync value %s",
                               mname, type_to_string(at));
                } else if (at && (at->kind == TY_STRING || at->kind == TY_ARRAY ||
                                  at->kind == TY_VEC || at->kind == TY_MAP ||
                                  at->kind == TY_SET || sema_type_is_linear(at)) &&
                           expr_is_movable_place(arg)) {
                    arg->is_move_source = true;
                }
            }
            if (sema_type_is_linear(at) && !pm->by_ref &&
                (arg->kind == EX_VAR || arg->kind == EX_FIELD || arg->kind == EX_INDEX)) {
                arg->is_move_source = true;
            }
            reject_untracked_reference(arg, at, pt, mname);
            reject_i128_lossy(&arg->tok, pt ? pt : ty_mixed, at, "argument");
            if (pt && !value_fits_slot(pt, at, arg))
                sema_error(&arg->tok, "argument %zu to %s::%s(): expected %s, got %s",
                           i + 1, c->name, mname, type_to_string(pt), type_to_string(at));
            check_byref_arg(sc, pm, arg);
        }
        if (!m->ret) m->ret = fn_has_value_return(m) ? ty_mixed : ty_void;
        e->type = resolve_type(sc, m->ret, m->name_tok);
        return e->type;
    }
    sema_error(&e->u.method.name_tok, "cannot call method '%s' on type %s", mname, type_to_string(ot));
    e->type = ty_mixed;
    return e->type;
}

static const Type *check_builtin_call(Scope *sc, Expr *e, const Builtin *b) {
    size_t na = e->u.call.args.len;
    e->builtin = b->name; /* codegen needs the builtin name too */
    /* preg_match/preg_match_all with an out-var: declare $matches BEFORE the
     * arguments are checked so later argument positions see it too. */
    if (strcmp(b->name, "preg_match") == 0 || strcmp(b->name, "preg_match_all") == 0) {
        if (na < 2 || na > 3) sema_error(&e->tok, "%s() expects 2 or 3 arguments, got %zu", b->name, na);
        if (na == 3) {
            Expr *mv = e->u.call.args.items[2];
            if (mv->kind != EX_VAR) {
                sema_error(&mv->tok, "%s(): third argument must be a variable (fills it with the matches)", b->name);
            } else {
                if (!scope_find(sc, mv->u.var.name))
                    mv->u.var.sym = scope_declare(sc, mv->u.var.name, type_array_of(ty_mixed), mv->tok, true);
                if (mv->u.var.sym) {
                    ((VarSym *)mv->u.var.sym)->is_auto = true;
                    ((VarSym *)mv->u.var.sym)->is_preg_out = true;
                }
            }
            e->type = ty_int;
            for (size_t i = 0; i < na; i++) {
                Expr *arg = e->u.call.args.items[i];
                const Type *at = check_expr(sc, arg);
                reject_untracked_reference(arg, at, NULL, b->name);
            }
            return e->type;
        }
    }
    for (size_t i = 0; i < na; i++) {
        Expr *arg = e->u.call.args.items[i];
        const Type *at = check_expr(sc, arg);
        if (strcmp(b->name, "unset") == 0 &&
            (arg->kind == EX_FIELD || arg->kind == EX_INDEX || arg->kind == EX_DEREF) &&
            expr_through_shared_ref(arg))
            sema_error(&arg->tok,
                       "cannot unset a place reached through a shared reference; use &mut");
        reject_untracked_reference(arg, at, NULL, b->name);
    }
    if ((strcmp(b->name, "thr_spawn") == 0 ||
         strcmp(b->name, "thr_parallel_map") == 0) && na > 0) {
        Expr *worker = e->u.call.args.items[0];
        validate_thread_callback(worker, worker->type, &worker->tok, b->name);
    }
    if ((strcmp(b->name, "chan_send") == 0 ||
         strcmp(b->name, "chan_try_send") == 0) && na == 2) {
        Expr *value = e->u.call.args.items[1];
        if (!thread_sendable_value(value->type)) {
            sema_error(&value->tok,
                       "%s() cannot send non-Sync value %s; move a value container or an own value",
                       b->name, type_to_string(value->type));
        } else if (value->type &&
                   (value->type->kind == TY_STRING || value->type->kind == TY_ARRAY ||
                    value->type->kind == TY_VEC || value->type->kind == TY_MAP ||
                    value->type->kind == TY_SET || sema_type_is_linear(value->type)) &&
                   expr_is_movable_place(value)) {
            value->is_move_source = true;
        }
    }
    if (strcmp(b->name, "array_push") == 0 && na >= 2) {
        const Type *container = ((Expr *)e->u.call.args.items[0])->type;
        const Type *element = container && container->kind == TY_MAP
            ? container->val : container && (container->kind == TY_ARRAY ||
              container->kind == TY_VEC || container->kind == TY_SET) ? container->elem : NULL;
        for (size_t i = 1; i < na; i++) {
            Expr *value = e->u.call.args.items[i];
            if (!sema_type_is_linear(value->type)) continue;
            if (!sema_type_is_linear(element))
                sema_error(&value->tok, "cannot insert an ownership value into a copyable collection");
            else if (expr_is_movable_place(value)) value->is_move_source = true;
        }
    }
    if ((strcmp(b->name, "array_values") == 0 || strcmp(b->name, "array_merge") == 0) && na) {
        for (size_t i = 0; i < na; i++) {
            Expr *arg = e->u.call.args.items[i];
            if (sema_type_is_linear(arg->type))
                sema_error(&arg->tok, "%s() cannot copy a collection containing ownership values", b->name);
        }
    }
    switch (b->shape) {
    case B_STR1:
        if (na != 1) sema_error(&e->tok, "%s() expects 1 argument, got %zu", b->name, na);
        e->type = b->ret;
        break;
    case B_STR2:
        if (na != 2) sema_error(&e->tok, "%s() expects 2 arguments, got %zu", b->name, na);
        e->type = b->ret;
        break;
    case B_INT_STR:
        if (na != 2) sema_error(&e->tok, "%s() expects 2 arguments, got %zu", b->name, na);
        e->type = b->ret;
        break;
    case B_STR_INT:
        if (na < 1) sema_error(&e->tok, "%s() expects at least 1 argument", b->name);
        e->type = b->ret;
        break;
    case B_LEN:
        if (na != 1) sema_error(&e->tok, "%s() expects 1 argument, got %zu", b->name, na);
        e->type = ty_int;
        break;
    case B_CMP:
        if (na != 2) sema_error(&e->tok, "%s() expects 2 arguments, got %zu", b->name, na);
        e->type = ty_int;
        break;
    case B_PRINT:
        e->type = b->ret;
        break;
    case B_PRINTF_COLOR:
    case B_PRINTLN_COLOR:
        if (na < 1 || na > 2) sema_error(&e->tok, "%s() expects 1 or 2 arguments, got %zu", b->name, na);
        e->type = b->ret;
        break;
    case B_READLINE_COLOR:
        if (na < 1 || na > 2) sema_error(&e->tok, "%s() expects 1 or 2 arguments, got %zu", b->name, na);
        e->type = b->ret;
        break;
    case B_MISC:
        /* compact("a", "b", ...): rewrite to a real map literal keyed by
         * the argument strings, pulling values from local scope. */
        if (strcmp(b->name, "compact") == 0) {
            Expr *map = expr_new(EX_MAP_LIT, e->tok);
            for (size_t i = 0; i < e->u.call.args.len; i++) {
                Expr *a = e->u.call.args.items[i];
                if (a->kind != EX_STR) continue;   /* non-literal: dropped */
                const char *vn = a->u.str.s;
                VarSym *vs = scope_find(sc, vn);
                if (!vs) continue;                 /* unknown: skipped, PHP-like */
                if (sema_type_is_linear(vs->type)) {
                    sema_error(&a->tok,
                               "compact() cannot erase ownership type %s into a mixed map",
                               type_to_string(vs->type));
                    continue;
                }
                Expr *k = expr_new(EX_STR, a->tok);
                k->u.str.s = vn;
                k->type = ty_string;
                Expr *ref = expr_new(EX_VAR, a->tok);
                ref->u.var.name = vn;
                ref->u.var.sym = vs;
                ref->type = vs->type;
                ptrvec_push(&map->u.map.keys, k);
                ptrvec_push(&map->u.map.vals, ref);
            }
            *e = *map;
            e->type = type_array_of(ty_mixed);
            return e->type;
        }
        if (strcmp(b->name, "array_pop") == 0 && na) {
            const Type *container = ((Expr *)e->u.call.args.items[0])->type;
            e->type = container && (container->kind == TY_ARRAY || container->kind == TY_VEC ||
                                    container->kind == TY_SET) ? container->elem : b->ret;
        } else {
            e->type = b->ret;
        }
        break;
    case B_ZERO:
        if (na != 0) sema_error(&e->tok, "%s() expects no arguments", b->name);
        e->type = b->ret;
        break;
    case B_ARRAY:
        e->type = type_array_of(ty_mixed);
        for (size_t i = 0; i < na; i++)
            if (sema_type_is_linear(((Expr *)e->u.call.args.items[i])->type))
                sema_error(&((Expr *)e->u.call.args.items[i])->tok,
                           "array() cannot infer a single linear element type; use a typed array literal");
        break;
    case B_MAXMIN:
        if (na == 0) sema_error(&e->tok, "%s() expects at least 1 argument", b->name);
        e->type = ty_mixed;
        break;
    case B_ABS:
        if (na != 1) sema_error(&e->tok, "%s() expects 1 argument, got %zu", b->name, na);
        e->type = b->ret;
        break;
    case B_INT_INT:
        if (na != 1) sema_error(&e->tok, "%s() expects 1 argument, got %zu", b->name, na);
        e->type = b->ret;
        break;
    case B_JSON: e->type = b->ret; break;
    case B_SPLIT:
        if (na != 2) sema_error(&e->tok, "%s() expects 2 arguments, got %zu", b->name, na);
        e->type = b->ret;
        break;
    case B_JOIN: e->type = ty_string; break;
    case B_KEYS: case B_VALUES: case B_SORTISH: case B_RANGE:
        e->type = b->ret;
        break;
    case B_SORT:
        if (na != 1) sema_error(&e->tok, "%s() expects 1 argument, got %zu", b->name, na);
        e->type = ty_bool;
        break;
    }
    return e->type;
}

static const Type *check_call(Scope *sc, Expr *e) {
    Expr *callee = e->u.call.fn;
    /* method call written as $obj->m(args) already comes as EX_METHOD */
    if (callee->kind == EX_VAR) {
        const char *name = callee->u.var.name;
        /* a local holding a closure/fn value shadows function names — but a
         * bare name (`foo()`) always means the function, never $foo */
        VarSym *local = callee->u.var.bare_name ? NULL : scope_find(sc, name);
        if (!local || (local->type && local->type->kind != TY_FN)) {
            /* A declared function takes precedence over a builtin with the
             * same spelling. Otherwise a perfectly valid user declaration
             * such as `function passthru(int &$x)` is emitted as a call to
             * the runtime builtin instead of the declared function. */
            AstFn *fn = find_function(name);
            const Builtin *b = builtin_lookup(name);
            if (b && !fn) {
                if (b->unsafe && sc->enforce_memory_safety) {
                    sema_error(&e->tok, "unsafe builtin '%s' is forbidden by declare(enforce_memory_safety=1)", name);
                } else if (b->unsafe && !sc->in_unsafe) {
                    sema_error(&e->tok, "call to unsafe function '%s' requires an unsafe block", name);
                }
                return check_builtin_call(sc, e, b);
            }
            if (!fn && !local) {
                sema_error(&e->tok, "call to undefined function '%s()'", name);
                for (size_t i = 0; i < e->u.call.args.len; i++)
                    check_expr(sc, e->u.call.args.items[i]);
                e->type = ty_mixed;
                return e->type;
            }
            if (fn) {
                e->u.call.target = fn;
                size_t na = e->u.call.args.len;
                size_t np = fn->params.len;
                size_t nreq = fn_min_args(fn);
                if (na < nreq || na > np)
                    sema_error(&e->tok, "function %s() expects %zu..%zu argument(s), got %zu", name, nreq, np, na);
                for (size_t i = 0; i < na && i < np; i++) {
                    Param *pm = fn->params.items[i];
                    const Type *pt = resolve_type(sc, pm->type, pm->name_tok);
                    Expr *arg = e->u.call.args.items[i];
                    const Type *at = check_expr(sc, arg);
                    if (pt && pt->kind == TY_FN && pt->thread_safe_callback)
                        validate_thread_callback(arg, at, &arg->tok, name);
                    if (sema_type_is_linear(at) && !pm->by_ref &&
                        (arg->kind == EX_VAR || arg->kind == EX_FIELD || arg->kind == EX_INDEX)) {
                        arg->is_move_source = true;
                    }
                    reject_untracked_reference(arg, at, pt, name);
                    reject_i128_lossy(&arg->tok, pt ? pt : ty_mixed, at, "argument");
                    if (pt && pm->type && !value_fits_slot(pt, at, arg))
                        sema_error(&arg->tok, "argument %zu to %s(): expected %s, got %s",
                                   i + 1, name, type_to_string(pt), type_to_string(at));
                    check_byref_arg(sc, pm, arg);
                }
                /* a function with no written return type is inferred before
                 * the body is ever checked (check_fn_body does it PHP-style:
                 * mixed when it returns values, void otherwise). Calling this
                 * function must not clobber that inferred type. */
                if (!fn->ret_inferred && !fn->ret) {
                    fn->ret = fn_has_value_return(fn) ? ty_mixed : ty_void;
                    fn->ret_inferred = true;
                }
                e->type = fn->ret ? resolve_type(sc, fn->ret, fn->name_tok) : ty_mixed;
                int rbp = fn_return_ref_param(fn);
                if (rbp >= 0 && (size_t)rbp < na) {
                    e->borrow_param_index = rbp;
                    e->has_borrow_param = true;
                    Expr *arg = e->u.call.args.items[rbp];
                    VarSym *src = arg && arg->kind == EX_VAR
                        ? (VarSym *)arg->u.var.sym : NULL;
                    if (src) {
                        e->borrow_origin = (void *)(src->borrow_origin ? src->borrow_origin : src);
                        e->borrow_place = src->borrow_place;
                        if (src->type && (src->type->kind == TY_REF || src->type->kind == TY_MUTREF))
                            e->borrow_parent = src;
                    } else if (arg && (arg->kind == EX_BORROW || arg->kind == EX_MUTBORROW)) {
                        e->borrow_origin = arg->borrow_origin
                            ? arg->borrow_origin : expr_root_var(arg->u.borrow.operand);
                        e->borrow_parent = arg->borrow_parent;
                        e->borrow_place = arg->borrow_place;
                    }
                } else {
                    VarSym *global = fn_return_ref_global(fn);
                    if (global) e->borrow_origin = global;
                }
                return e->type;
            }
        }
    }
    /* first-class fn value call */
    const Type *ft = check_expr(sc, callee);
    if (ft->kind == TY_FN) {
        size_t na = e->u.call.args.len;
        bool any = ft->fn.variadic;   /* `callable`: arity not pinned */
        if (!any && na != ft->fn.nparams)
            sema_error(&e->tok, "function expects %zu argument(s), got %zu", ft->fn.nparams, na);
        for (size_t i = 0; i < na; i++) {
            const Type *pt = (!any && i < ft->fn.nparams) ? ft->fn.params[i] : NULL;
            Expr *arg = e->u.call.args.items[i];
            const Type *at = check_expr(sc, arg);
            if (sema_type_is_linear(at) &&
                (arg->kind == EX_VAR || arg->kind == EX_FIELD || arg->kind == EX_INDEX))
                arg->is_move_source = true;
            reject_untracked_reference(arg, at, pt, "an indirect call");
            reject_i128_lossy(&arg->tok, pt ? pt : ty_mixed, at, "argument");
            if (pt && !value_fits_slot(pt, at, arg))
                sema_error(&arg->tok, "argument %zu: expected %s, got %s",
                           i + 1, type_to_string(pt), type_to_string(at));
        }
        e->type = ft->fn.ret ? ft->fn.ret : ty_mixed;
        return e->type;
    }
    /* An hval-typed value may hold a closure at runtime (a property, a map
     * entry, an untyped parameter): allowed like PHP, dispatched by the
     * runtime, which throws a catchable error if it isn't callable. */
    if (ft->kind == TY_MIXED) {
        for (size_t i = 0; i < e->u.call.args.len; i++) {
            Expr *arg = e->u.call.args.items[i];
            const Type *at = check_expr(sc, arg);
            if (sema_type_is_linear(at) &&
                (arg->kind == EX_VAR || arg->kind == EX_FIELD || arg->kind == EX_INDEX))
                sema_error(&arg->tok, "cannot pass an ownership value to a dynamically typed call");
            reject_untracked_reference(arg, at, NULL, "a dynamic call");
            reject_i128_lossy(&arg->tok, ty_mixed, at, "argument");
        }
        e->type = ty_mixed;
        return e->type;
    }
    for (size_t i = 0; i < e->u.call.args.len; i++)
        check_expr(sc, e->u.call.args.items[i]);
    sema_error(&e->tok, "expression is not callable (%s)", type_to_string(ft));
    e->type = ty_mixed;
    return e->type;
}

static const Type *check_expr_inner(Scope *sc, Expr *e) {
    if (!e) return ty_mixed;
    switch (e->kind) {
    case EX_INT: e->type = e->u.i.is_wide ? ty_i128 : ty_int; return e->type;
    case EX_FLOAT: e->type = ty_float; return e->type;
    case EX_STR: e->type = ty_string; return e->type;
    case EX_BOOL: e->type = ty_bool; return e->type;
    case EX_NULL: e->type = ty_null; return e->type;
    case EX_TPL: {
        for (size_t i = 0; i < e->u.tpl.exprs.len; i++)
            check_expr(sc, e->u.tpl.exprs.items[i]);
        e->type = ty_string;
        return e->type;
    }    case EX_VAR: {
        const char *name = e->u.var.name;
        /* PHP superglobals: $argc / $argv (read-only, available everywhere) */
        if (name == s_argc || name == s_argv) {
            e->u.var.sym = NULL;
            e->u.var.is_global = true;
            e->type = (name == s_argc) ? (const Type *)ty_int
                                       : (const Type *)type_array_of(ty_string);
            return e->type;
        }
        if (name == s_this && (sc->cur_class || (sc->cur_fn && sc->cur_fn->is_method))) {
            if (!sc->cur_class && sc->cur_fn->cls) sc->cur_class = sc->cur_fn->cls;
            const Type *st = sc->self_type;
            if (!st && sc->cur_class) {
                Type *t = type_new(sc->cur_class->is_interface ? TY_INTERFACE : TY_CLASS);
                t->name = intern(sc->cur_class->name);
                t->decl = sc->cur_class;
                sc->self_type = st = t;
            }
            if (g_clo_depth > 0) {
                /* $this inside a closure: mark the closure (and outer ones) */
                for (int d = g_clo_depth - 1; d >= 0; d--) g_clo_this[d] = true;
            }
            e->u.var.sym = g_clo_depth == 0 && sc->cur_fn
                ? sc->cur_fn->this_sym : NULL;
            e->type = st ? st : ty_mixed;
            return e->type;
        }
        VarSym *v = scope_find(sc, e->u.var.name);
        if (!v) {
            /* not a local: is it a program-wide `global` variable that some
             * function has declared? Functions are checked before the top
             * level, so a top-level read must still resolve to it. */
            v = program_global_find(e->u.var.name);
        }
        if (!v && e->u.var.name && (strncmp(e->u.var.name, "PHP_", 4) == 0 || strncmp(e->u.var.name, "JSON_", 5) == 0 ||
            strcmp(e->u.var.name, "M_PI") == 0 || strcmp(e->u.var.name, "M_E") == 0 ||
            strcmp(e->u.var.name, "RESET") == 0 || strcmp(e->u.var.name, "BLACK") == 0 ||
            strcmp(e->u.var.name, "RED") == 0 || strcmp(e->u.var.name, "GREEN") == 0 ||
            strcmp(e->u.var.name, "YELLOW") == 0 || strcmp(e->u.var.name, "BLUE") == 0 ||
            strcmp(e->u.var.name, "MAGENTA") == 0 || strcmp(e->u.var.name, "CYAN") == 0 ||
            strcmp(e->u.var.name, "WHITE") == 0 || strcmp(e->u.var.name, "BRIGHT_BLACK") == 0 ||
            strcmp(e->u.var.name, "BRIGHT_RED") == 0 || strcmp(e->u.var.name, "BRIGHT_GREEN") == 0 ||
            strcmp(e->u.var.name, "BRIGHT_YELLOW") == 0 || strcmp(e->u.var.name, "BRIGHT_BLUE") == 0 ||
            strcmp(e->u.var.name, "BRIGHT_MAGENTA") == 0 || strcmp(e->u.var.name, "BRIGHT_CYAN") == 0 ||
            strcmp(e->u.var.name, "BRIGHT_WHITE") == 0)) {
            /* M_PI / M_E are handled by the codegen constant branch below; here
             * they only need to pass the unknown-name guard with float type. */
            /* PHP predefined constants: PHP_INT_MAX, PHP_EOL, PHP_OS, ... */
            const char *n = e->u.var.name;
            if (strcmp(n, "PHP_INT_SIZE") == 0 || strcmp(n, "PHP_INT_DIGITS") == 0 ||
                strcmp(n, "PHP_INT_MAX") == 0 || strcmp(n, "PHP_INT_MIN") == 0 ||
                strcmp(n, "PHP_ROUND_HALF_UP") == 0 || strcmp(n, "PHP_ROUND_HALF_DOWN") == 0 ||
                strcmp(n, "PHP_ROUND_HALF_EVEN") == 0 || strcmp(n, "PHP_ROUND_HALF_ODD") == 0 ||
                strcmp(n, "PHP_FILE_APPEND") == 0 || strcmp(n, "PHP_FILE_IGNORE_NEW_LINES") == 0 ||
                strcmp(n, "PHP_FILE_SKIP_EMPTY_LINES") == 0 || strcmp(n, "PHP_FILE_USE_INCLUDE_PATH") == 0)
                e->type = ty_int;
            else if (strcmp(n, "PHP_FLOAT_EPSILON") == 0 || strcmp(n, "PHP_EULER") == 0 ||
                     strcmp(n, "PHP_PI") == 0 || strcmp(n, "M_PI") == 0 || strcmp(n, "M_E") == 0) {
                e->type = ty_float;
                return e->type;
            }
            else if (strcmp(n, "PHP_OS") == 0 || strcmp(n, "PHP_EOL") == 0 ||
                     strcmp(n, "PHP_VERSION") == 0 || strcmp(n, "PHP_SAPI") == 0 ||
                     strcmp(n, "PHP_UNAME") == 0)
                e->type = ty_string;
            else if (strcmp(n, "JSON_PRETTY_PRINT") == 0 || strcmp(n, "JSON_UNESCAPED_SLASHES") == 0 ||
                     strcmp(n, "JSON_NUMERIC_CHECK") == 0 || strcmp(n, "JSON_FORCE_OBJECT") == 0)
                e->type = ty_int;
            else if (strcmp(n, "PHP_TRUE") == 0 || strcmp(n, "PHP_FALSE") == 0 || strcmp(n, "PHP_NULL") == 0)
                e->type = ty_mixed;
            else if (strcmp(n, "RESET") == 0 || strcmp(n, "BLACK") == 0 || strcmp(n, "RED") == 0 ||
                     strcmp(n, "GREEN") == 0 || strcmp(n, "YELLOW") == 0 || strcmp(n, "BLUE") == 0 ||
                     strcmp(n, "MAGENTA") == 0 || strcmp(n, "CYAN") == 0 || strcmp(n, "WHITE") == 0 ||
                     strcmp(n, "BRIGHT_BLACK") == 0 || strcmp(n, "BRIGHT_RED") == 0 ||
                     strcmp(n, "BRIGHT_GREEN") == 0 || strcmp(n, "BRIGHT_YELLOW") == 0 ||
                     strcmp(n, "BRIGHT_BLUE") == 0 || strcmp(n, "BRIGHT_MAGENTA") == 0 ||
                     strcmp(n, "BRIGHT_CYAN") == 0 || strcmp(n, "BRIGHT_WHITE") == 0)
                e->type = ty_int;
            else
                sema_error(&e->tok, "undefined constant %s", n);
            return e->type;
        }
        if (!v) {
            sema_error(&e->tok, "undefined variable $%s (assign it a value first)", e->u.var.name);
            e->type = ty_mixed;
            return e->type;
        }
        /* capture analysis: var declared outside the innermost closure? */
        if (g_clo_depth > 0 && !v->is_program_global &&
            v->decl_seq < g_clo_body_seq[g_clo_depth - 1]) {
            if (v->type && (v->type->kind == TY_REF || v->type->kind == TY_MUTREF))
                sema_error(&e->tok,
                           "closures cannot capture reference binding $%s without closure lifetime tracking",
                           v->name);
            else
                mark_heap_capture(sc, v, &e->tok, false);
            note_capture(v);
            for (int d = g_clo_depth - 2; d >= 0; d--) {
                if (v->decl_seq >= g_clo_body_seq[d]) break;
                for (size_t i = 0; i < g_clo_caps[d].len; i++)
                    if (g_clo_caps[d].items[i] == v) goto next_d;
                ptrvec_push(&g_clo_caps[d], v);
            next_d:;
            }
        }
        e->u.var.sym = v;
        e->type = refined_type_of(v);
        return e->type;
    }
    case EX_TUPLE_LIT: {
        Type *t = type_new(TY_TUPLE);
        t->nmembers = e->u.tuple.elems.len;
        t->members = t->nmembers ? xcalloc(t->nmembers, sizeof(*t->members)) : NULL;
        for (size_t i = 0; i < e->u.tuple.elems.len; i++) {
            Expr *item = e->u.tuple.elems.items[i];
            const Type *et = check_expr(sc, item);
            if (et && et->kind == TY_I128)
                sema_error(&item->tok,
                           "cannot store i128 in a tuple; convert with (int) or strval() first");
            ((const Type **)t->members)[i] = et;
            if (et && (et->kind == TY_REF || et->kind == TY_MUTREF))
                sema_error(&e->tok, "references cannot be stored in tuples because this type has no lifetime tracking");
            if (sema_type_is_linear(et) && expr_is_movable_place(item))
                item->is_move_source = true;
        }
        e->type = t;
        return e->type;
    }
    case EX_ARRAY_LIT: {
        const Type *elem = NULL;
        bool is_map = e->u.map.vals.len > 0;
        if (is_map) {
            Type *mt = type_new(TY_MAP);
            const Type *kt = NULL, *vt = NULL;
            bool has_linear_value = false;
            for (size_t i = 0; i < e->u.map.vals.len; i++) {
                const Type *k2 = check_expr(sc, e->u.map.keys.items[i]);
                const Type *v2 = check_expr(sc, e->u.map.vals.items[i]);
                Expr *key_expr = e->u.map.keys.items[i];
                Expr *value_expr = e->u.map.vals.items[i];
                if (k2 && k2->kind == TY_I128)
                    sema_error(&key_expr->tok,
                               "cannot store i128 in a map key; convert with (int) or strval() first");
                if (v2 && v2->kind == TY_I128)
                    sema_error(&value_expr->tok,
                               "cannot store i128 in a map; convert with (int) or strval() first");
                if (sema_type_is_linear(k2))
                    sema_error(&key_expr->tok, "ownership values cannot be copied into map keys");
                if (sema_type_is_linear(v2)) {
                    has_linear_value = true;
                    if (expr_is_movable_place(value_expr)) value_expr->is_move_source = true;
                }
                if ((k2 && (k2->kind == TY_REF || k2->kind == TY_MUTREF)) ||
                    (v2 && (v2->kind == TY_REF || v2->kind == TY_MUTREF)))
                    sema_error(&e->tok, "references cannot be stored in maps because this type has no lifetime tracking");
                /* Every entry contributes: a mixed-value map like
                 * ["name" => "Ada", "alter" => 25] must be hval-valued,
                 * otherwise the int entry gets compiled as a string.
                 * Numeric entries are NOT unified either — the runtime keeps
                 * one hval per slot, so declaring [1, 2.5] as float would
                 * reinterpret the int slot's bits as a double. Any difference
                 * means "mixed", and reads then convert properly. */
                if (!kt) kt = k2;
                else if (!same_slot_type(kt, k2)) kt = ty_mixed;
                if (!vt) vt = v2;
                else if (!same_slot_type(vt, v2)) vt = ty_mixed;
            }
            mt->key = kt ? slot_elem_type(kt) : ty_mixed;
            mt->val = vt ? slot_elem_type(vt) : ty_mixed;
            if (has_linear_value && !sema_type_is_linear(mt->val))
                sema_error(&e->tok, "a map containing own values must have one statically known linear value type");
            e->type = mt;
            return e->type;
        }
        bool has_linear_elem = false;
        for (size_t i = 0; i < e->u.arr.elems.len; i++) {
            Expr *item = e->u.arr.elems.items[i];
            const Type *et = check_expr(sc, item);
            if (et && (et->kind == TY_REF || et->kind == TY_MUTREF))
                sema_error(&e->tok, "references cannot be stored in arrays because this type has no lifetime tracking");
            if (et && et->kind == TY_I128)
                sema_error(&item->tok,
                           "cannot store i128 in an array; convert with (int) or strval() first");
            if (sema_type_is_linear(et)) {
                has_linear_elem = true;
                if (expr_is_movable_place(item)) item->is_move_source = true;
            }
            if (!elem) elem = et;
            /* see the map case: slots hold hvals, so never unify numerics
             * here — [1, 2.5] must stay mixed, not become float */
            else if (!same_slot_type(elem, et)) elem = ty_mixed;
        }
        Type *at = type_new(TY_ARRAY);
        at->elem = elem ? slot_elem_type(elem) : ty_mixed;
        if (has_linear_elem && !sema_type_is_linear(at->elem))
            sema_error(&e->tok, "an array containing own values must have one statically known linear element type");
        e->type = at;
        return e->type;
    }
    case EX_INDEX: {
        const Type *ot = check_expr(sc, e->u.index.obj);
        const Type *it = e->u.index.idx ? check_expr(sc, e->u.index.idx) : ty_int;
        if (ot->kind == TY_ARRAY || ot->kind == TY_VEC || ot->kind == TY_MAP || ot->kind == TY_SET) {
            const Type *want = ot->kind == TY_MAP ? ot->key : ty_int;
            /* PHP-style: a string key promotes a plain array to a map */
            if (ot->kind == TY_ARRAY && it && it->kind == TY_STRING) {
                e->type = ty_mixed;
                return e->type;
            }
            if (it && want && !types_compatible(want, it) &&
                want->kind != TY_MIXED && it->kind != TY_MIXED)
                sema_error(&e->tok, "index must be %s, got %s", type_to_string(want), type_to_string(it));
            e->type = elem_type_of(ot, it);
            if (!e->type) e->type = ty_mixed;
            return e->type;
        }
        if (ot->kind == TY_STRING) { e->type = ty_string; return e->type; }
        /* PHP-style: indexing a mixed value (e.g. a map from a foreach over
         * an untyped array) is allowed and yields mixed. */
        if (ot->kind == TY_MIXED) { e->type = ty_mixed; return e->type; }
        sema_error(&e->tok, "cannot index value of type %s", type_to_string(ot));
        e->type = ty_mixed;
        return e->type;
    }
    case EX_FIELD: {
        const Type *ot = check_expr(sc, e->u.field.obj);
        if (ot->kind == TY_OWN || ot->kind == TY_RC || ot->kind == TY_REF || ot->kind == TY_MUTREF)
            ot = ot->elem;
        if (ot->kind == TY_CLASS || ot->kind == TY_STRUCT) {
            AstClass *c = ot->decl;
            if (!c) c = find_class(ot->name);
            if (c) {
                StructField *f = class_find_field(c, e->u.field.name);
                if (!f && c->base) f = class_find_field(c->base, e->u.field.name);
                if (f) {
                    e->u.field.sf = f;
                    e->u.field.vcls = c;      /* static type of the receiver */
                    e->u.field.owner = ot;
                    /* an untyped property is hval-valued, never NULL-typed:
                     * `pub $cb = null;` is the PHP-style dynamic member */
                    const Type *ft = f->type ? resolve_type(sc, f->type, f->name_tok) : ty_mixed;
                    e->type = f->is_weak ? nullable_type_of(ft) : ft;
                    return e->type;
                }
            }
            sema_error(&e->tok, "no field '%s' on %s", e->u.field.name, type_to_string(ot));
            e->type = ty_mixed;
            return e->type;
        }
        if (e->u.field.is_static) {
            /* Cls::CONST or Enum::CASE */
            AstEnum *en = find_enum(e->u.field.obj->kind == EX_VAR ? e->u.field.obj->u.var.name : e->u.field.name);
            if (en && e->u.field.obj->kind == EX_VAR) {
                AstEnum *en2 = find_enum(e->u.field.obj->u.var.name);
                if (en2) {
                    for (size_t i = 0; i < en2->cases.len; i++) {
                        EnumCase *ec = en2->cases.items[i];
                        if (ec->name == intern(e->u.field.name)) {
                            e->type = sym_type_of_enum(en2);
                            e->u.field.case_index = (int)ec->index;
                            return e->type;
                        }
                    }
                    sema_error(&e->tok, "enum %s has no case '%s'", en2->name, e->u.field.name);
                    e->type = ty_mixed;
                    return e->type;
                }
            }
            AstClass *c = find_class(e->u.field.obj->kind == EX_VAR ? e->u.field.obj->u.var.name : "?");
            if (c) {
                StructField *f = class_find_field(c, e->u.field.name);
                if (f) {
                    e->u.field.sf = f;
                    e->type = resolve_type(sc, f->type, f->name_tok);
                    return e->type;
                }
            }
        }
        sema_error(&e->tok, "no member '%s' on type %s", e->u.field.name, type_to_string(ot));
        e->type = ty_mixed;
        return e->type;
    }
    case EX_CALL: return check_call(sc, e);
    case EX_METHOD: return check_method_call(sc, e);
    case EX_STATIC: {
        const Builtin *b = builtin_lookup(e->u.staticcall.name);
        AstFn *fn = find_function(e->u.staticcall.name);
        AstClass *cls = find_class(e->u.staticcall.cls);
        if (cls) {
            AstFn *m = class_find_method(cls, e->u.staticcall.name);
            if (!m && cls->base) m = class_find_method(cls->base, e->u.staticcall.name);
            if (m) {
                e->u.staticcall.target = m;
                size_t na = e->u.staticcall.args.len;
                size_t nreq = fn_min_args(m);
                if (na < nreq || na > m->params.len)
                    sema_error(&e->tok, "%s::%s() expects %zu..%zu argument(s), got %zu",
                               cls->name, m->name, nreq, m->params.len, na);
                for (size_t i = 0; i < na && i < m->params.len; i++) {
                    Param *pm = m->params.items[i];
                    const Type *pt = resolve_type(sc, pm->type, pm->name_tok);
                    Expr *arg = e->u.staticcall.args.items[i];
                    const Type *at = check_expr(sc, arg);
                    reject_untracked_reference(arg, at, pt, m->name);
                    reject_i128_lossy(&arg->tok, pt ? pt : ty_mixed, at, "argument");
                    if (pt && pm->type && !value_fits_slot(pt, at, arg))
                        sema_error(&arg->tok, "argument %zu: expected %s, got %s",
                                   i + 1, type_to_string(pt), type_to_string(at));
                    check_byref_arg(sc, pm, arg);
                }
                if (!m->ret) m->ret = fn_has_value_return(m) ? ty_mixed : ty_void;
                e->type = resolve_type(sc, m->ret, m->name_tok);
                return e->type;
            }
        }
        if (b) {
            Expr tmp;
            memset(&tmp, 0, sizeof(tmp));
            tmp.kind = EX_CALL;
            tmp.tok = e->tok;
            tmp.u.call.fn = expr_new(EX_VAR, e->tok);
            tmp.u.call.fn->u.var.name = e->u.staticcall.name;
            tmp.u.call.args = e->u.staticcall.args;
            const Type *t = check_builtin_call(sc, &tmp, b);
            e->builtin = e->u.staticcall.name;
            e->type = t;
            return e->type;
        }
        if (fn) {
            size_t na = e->u.staticcall.args.len;
            size_t nreq = fn_min_args(fn);
            if (na < nreq || na > fn->params.len)
                sema_error(&e->tok, "%s() expects %zu..%zu argument(s), got %zu", fn->name, nreq, fn->params.len, na);
            for (size_t i = 0; i < na && i < fn->params.len; i++) {
                Param *pm = fn->params.items[i];
                const Type *pt = resolve_type(sc, pm->type, pm->name_tok);
                Expr *arg = e->u.staticcall.args.items[i];
                const Type *at = check_expr(sc, arg);
                reject_untracked_reference(arg, at, pt, fn->name);
                reject_i128_lossy(&arg->tok, pt ? pt : ty_mixed, at, "argument");
                if (pt && pm->type && !value_fits_slot(pt, at, arg))
                    sema_error(&arg->tok, "argument %zu: expected %s, got %s",
                               i + 1, type_to_string(pt), type_to_string(at));
                check_byref_arg(sc, pm, arg);
            }
            if (!fn->ret) fn->ret = fn_has_value_return(fn) ? ty_mixed : ty_void;
            e->type = resolve_type(sc, fn->ret, fn->name_tok);
            return e->type;
        }
        sema_error(&e->tok, "unknown function or class '%s'", e->u.staticcall.cls);
        e->type = ty_mixed;
        return e->type;
    }
    case EX_NEW: {
        AstClass *c = find_class(e->u.newexpr.cls);
        if (!c && strcmp(e->u.newexpr.cls, "Exception") == 0) {
            /* builtin Exception class: 0..1 args, throws carry a message */
            size_t na = e->u.newexpr.args.len;
            if (na > 1)
                sema_error(&e->tok, "Exception::__construct() expects at most 1 argument");
            for (size_t i = 0; i < na; i++) {
                Expr *arg = e->u.newexpr.args.items[i];
                const Type *at = check_expr(sc, arg);
                if (sema_type_is_linear(at) &&
                    (arg->kind == EX_VAR || arg->kind == EX_FIELD || arg->kind == EX_INDEX))
                    arg->is_move_source = true;
                reject_untracked_reference(arg, at, NULL, "Exception::__construct");
                reject_i128_lossy(&arg->tok, ty_mixed, at, "argument");
            }
            Type *t = type_new(TY_CLASS);
            t->name = intern("Exception");
            e->type = t;
            return e->type;
        }
        if (!c) {
            sema_error(&e->tok, "unknown class '%s'", e->u.newexpr.cls);
            e->type = ty_mixed;
            return e->type;
        }
        e->u.newexpr.cls_res = c;
        AstFn *ctor = class_find_method(c, "__construct");
        if (!ctor && c->base) ctor = class_find_method(c->base, "__construct");
        size_t na = e->u.newexpr.args.len;
        if (ctor) {
            size_t np = ctor->params.len;
            size_t nreq = fn_min_args(ctor);
            if (na < nreq || na > np)
                sema_error(&e->tok, "%s::__construct() expects %zu..%zu argument(s), got %zu",
                           c->name, nreq, np, na);
            for (size_t i = 0; i < na && i < np; i++) {
                Param *pm = ctor->params.items[i];
                const Type *pt = resolve_type(sc, pm->type, pm->name_tok);
                Expr *arg = e->u.newexpr.args.items[i];
                const Type *at = check_expr(sc, arg);
                if (pt && pt->kind == TY_FN && pt->thread_safe_callback)
                    validate_thread_callback(arg, at, &arg->tok, "a constructor");
                if (sema_type_is_linear(at) && !pm->by_ref &&
                    (arg->kind == EX_VAR || arg->kind == EX_FIELD || arg->kind == EX_INDEX))
                    arg->is_move_source = true;
                reject_untracked_reference(arg, at, pt, "a constructor");
                reject_i128_lossy(&arg->tok, pt ? pt : ty_mixed, at, "argument");
                if (pt && pm->type && !value_fits_slot(pt, at, arg))
                    sema_error(&arg->tok, "argument %zu to %s(): expected %s, got %s",
                               i + 1, c->name, type_to_string(pt), type_to_string(at));
                /* a constructor is built by hpu_%s_new_args(), which has no
                 * pointer slot to write back through */
                if (pm->by_ref)
                    sema_error(&arg->tok, "%s::__construct() cannot take a parameter by reference", c->name);
            }
        } else if (na != 0) {
            sema_error(&e->tok, "%s has no constructor, expected 0 arguments, got %zu", c->name, na);
        }
        /* `new C` is an ordinary PHP object handle of type C, not own<C>.
         * own<C> is linear (moved on every assignment/pass/return, never
         * erased back to C), so typing every `new` as own<C> turned plain
         * PHP such as `$s = new C(); f($s); f($s);` or
         * `$this->w = new Widget();` into ownership errors. Ownership is
         * opt-in: an explicit `own C` slot accepts this fresh value via
         * fresh_object_fits_own(), and is linear from then on. */
        e->type = class_type_of(c);
        return e->type;
    }
    case EX_BIN: return check_bin(sc, e);
    case EX_UN: {
        const Type *t = check_expr(sc, e->u.un.operand);
        switch (e->u.un.op.kind) {
        case T_NOT: e->type = ty_bool; break;
        case T_MINUS: case T_PLUS:
            if (!type_is_numeric(t->kind))
                sema_error(&e->u.un.op, "unary '%s' requires a numeric operand, got %s",
                           tok_kind_name(e->u.un.op.kind), type_to_string(t));
            e->type = t;
            break;
        case T_TILDE:
            if (!type_is_integral(t->kind))
                sema_error(&e->u.un.op, "unary '~' requires an integer operand, got %s", type_to_string(t));
            e->type = t;
            break;
        default: e->type = t; break;
        }
        return e->type;
    }
    case EX_ASSIGN: {
        Expr *tgt = e->u.assign.target;
        /* list destructuring "[$a, $b] = expr;" / "[$k => $v] = expr;" */
        if (tgt->kind == EX_ARRAY_LIT && e->u.assign.op.kind == T_ASSIGN) {
            const Type *st = e->u.assign.value ? check_expr(sc, e->u.assign.value) : ty_mixed;
            if (sema_type_is_linear(st))
                sema_error(&tgt->tok, "cannot destructure a collection containing ownership values");
            bool keyed = tgt->u.map.vals.len > 0;
            if (keyed) {
                /* pattern keys participate in checking so codegen can box them */
                for (size_t i = 0; i < tgt->u.map.keys.len; i++)
                    check_expr(sc, (Expr *)tgt->u.map.keys.items[i]);
            }
            size_t n = keyed ? tgt->u.map.vals.len : tgt->u.arr.elems.len;
            for (size_t i = 0; i < n; i++) {
                Expr *slot = keyed ? (Expr *)tgt->u.map.vals.items[i]
                                   : (Expr *)tgt->u.arr.elems.items[i];
                if (!slot) continue;   /* skip slot: [$a, , $c] */
                if (slot->kind != EX_VAR)
                    sema_error(&slot->tok, "destructuring target must be a variable");
                else {
                    VarSym *sv = scope_find(sc, slot->u.var.name);
                    if (!sv) {
                        sv = scope_declare(sc, slot->u.var.name, ty_mixed, slot->tok, true);
                        sv->is_auto = true;
                    } else if (!sv->is_mut) {
                        sema_error(&slot->tok,
                                   "cannot assign to immutable variable $%s (declare with let mut)",
                                   slot->u.var.name);
                    }
                    slot->u.var.sym = sv;
                    const Type *slot_type = st;
                    if (keyed && st && st->kind == TY_MAP)
                        slot_type = st->val;
                    else if (!keyed && st && st->kind == TY_TUPLE && i < st->nmembers)
                        slot_type = st->members[i];
                    else if (!keyed && st &&
                             (st->kind == TY_ARRAY || st->kind == TY_VEC ||
                              st->kind == TY_SET))
                        slot_type = st->elem;
                    if (type_is_mutable_alias(slot_type) &&
                        expr_through_shared_ref(e->u.assign.value)) {
                        if (sv->is_program_global || sv->is_global_alias)
                            sema_error(&slot->tok,
                                       "cannot store destructured mutable alias derived from a shared reference in global $%s",
                                       sv->name);
                        else
                            sv->shared_alias = true;
                    }
                }
            }
            e->type = (st && (st->kind == TY_ARRAY || st->kind == TY_VEC)) ? st : ty_mixed;
            return e->type;
        }
        const Type *vt = e->u.assign.value ? check_expr(sc, e->u.assign.value) : ty_mixed;
        /* determine target type & mutability */
        const Type *tt = NULL;
        if (tgt->kind == EX_VAR) {
            VarSym *v = scope_find(sc, tgt->u.var.name);
            bool newly_declared = false;
            if (!v && sc->cur_fn == NULL)
                v = program_global_find(tgt->u.var.name);   /* `global $x;` seen earlier */
            if (!v) {
                /* PHP-style: assignment declares the variable. Optional
                 * type annotation: "$x: int = 5". */
                const Type *ann = e->u.assign.ann
                    ? resolve_type(sc, e->u.assign.ann, tgt->tok) : NULL;
                if (ann && sema_type_is_linear(vt) && !sema_type_is_linear(ann))
                    sema_error(&e->tok,
                               "cannot store ownership value %s in copyable variable $%s (%s)",
                               type_to_string(vt), tgt->u.var.name, type_to_string(ann));
                if (ann && e->u.assign.value &&
                    !value_fits_slot(ann, vt, e->u.assign.value) && ann->kind != TY_MIXED)
                    sema_error(&e->tok, "cannot initialize $%s (type %s) with %s",
                               tgt->u.var.name, type_to_string(ann), type_to_string(vt));
                /* `$x = null;` declares a dynamic variable: PHP allows a later
                 * `$x = 5;`, so the slot stays mixed until something says
                 * otherwise. A `?T` annotation is what pins it down. */
                const Type *vt2 = (!ann && vt && vt->kind == TY_NULL) ? ty_mixed : vt;
                v = scope_declare(sc, tgt->u.var.name, ann ? ann : vt2, tgt->tok, true);
                newly_declared = true;
                v->is_auto = true;
                /* register so later `global $x;` in functions binds THIS sym
                 * (is_program_global is set by the ST_GLOBAL binder itself) */
                if (sc->cur_fn == NULL && !program_global_find(tgt->u.var.name))
                    ptrvec_push(&g_program_globals, v);
                tgt->u.var.sym = v;
                tt = v->type;
            } else {
                tgt->u.var.sym = v; /* codegen needs the symbol for naming */
                tt = v->type;
                /* the variable is written to: a refinement the branch
                 * established about its old value does not hold any more.
                 * A plain write hands the slot the value it stores, a
                 * compound one writes a result of its own, so only the
                 * plain one can say what the slot holds now. */
                refine_assign(v, e->u.assign.op.kind == T_ASSIGN ? vt : NULL);
                if (!v->is_mut)
                    sema_error(&tgt->tok, "cannot assign to immutable variable $%s (declare with let mut)", tgt->u.var.name);
                if (v->type && (v->type->kind == TY_REF || v->type->kind == TY_MUTREF))
                    sema_error(&tgt->tok, "reassigning reference bindings is not supported; declare a new reference binding");
            }
            if (type_can_contain_heap_alias(v->type) && e->u.assign.op.kind == T_ASSIGN) {
                Expr *rhs = e->u.assign.value;
                VarSym *src = rhs && rhs->kind == EX_VAR ? rhs->u.var.sym : NULL;
                bool source_was_unaliased = src && !src->is_mut && !src->alias_unknown;
                if (newly_declared && source_was_unaliased) {
                    v->alias_origin = src->alias_origin ? src->alias_origin : src;
                } else if (!newly_declared || !expr_is_fresh_heap_value(rhs)) {
                    v->alias_origin = NULL;
                    v->alias_unknown = true;
                }
                /* A second binding to a heap value makes the source a shared
                 * owner too. Move-out checks must not treat either slot as
                 * uniquely owning its fields after this point. */
                if (src && src != v) src->alias_unknown = true;
            }
            if (e->u.assign.value && vt && vt->kind != TY_REF && vt->kind != TY_MUTREF &&
                type_is_mutable_alias(vt) &&
                expr_through_shared_ref(e->u.assign.value)) {
                if (v->is_program_global || v->is_global_alias)
                    sema_error(&tgt->tok,
                               "cannot store mutable alias derived from a shared reference in global $%s",
                               v->name);
                else
                    v->shared_alias = true;
            }
            if (vt && (vt->kind == TY_REF || vt->kind == TY_MUTREF)) {
                if (v->is_program_global || v->is_global_alias)
                    sema_error(&tgt->tok,
                               "cannot store a borrowed reference in global $%s; global reference lifetimes are not tracked",
                               v->name);
                if (!v->type || (v->type->kind != TY_REF && v->type->kind != TY_MUTREF))
                    sema_error(&tgt->tok,
                               "cannot erase a reference lifetime by storing the reference as %s",
                               type_to_string(v->type ? v->type : ty_mixed));
                Expr *rhs = e->u.assign.value;
                if (!newly_declared || !rhs) {
                    sema_error(&tgt->tok, "reference values may only initialize a tracked local binding");
                } else if (rhs->borrow_origin) {
                    v->borrow_origin = rhs->borrow_origin;
                    v->borrow_parent = rhs->borrow_parent;
                    v->borrow_place = rhs->borrow_place;
                    v->borrow_mut = vt->kind == TY_MUTREF;
                } else if (rhs->kind == EX_CALL && rhs->u.call.target &&
                           rhs->u.call.target->ret &&
                           (rhs->u.call.target->ret->kind == TY_REF ||
                            rhs->u.call.target->ret->kind == TY_MUTREF)) {
                    v->borrow_pending = true;
                } else if (rhs->kind == EX_VAR) {
                    VarSym *src = (VarSym *)rhs->u.var.sym;
                    if (!src || !src->type ||
                        (src->type->kind != TY_REF && src->type->kind != TY_MUTREF)) {
                        sema_error(&tgt->tok, "reference values may only initialize a tracked local binding");
                    } else if (src->borrow_pending) {
                        v->borrow_pending = true;
                    } else {
                        v->borrow_origin = src->borrow_origin ? src->borrow_origin : src;
                        v->borrow_parent = src;
                        v->borrow_mut = vt->kind == TY_MUTREF;
                    }
                } else if (rhs->kind == EX_BORROW || rhs->kind == EX_MUTBORROW) {
                    Expr *operand = rhs->u.borrow.operand;
                    VarSym *src = rhs->borrow_origin
                        ? (VarSym *)rhs->borrow_origin : expr_root_var(operand);
                    if (src && src->borrow_pending) {
                        v->borrow_pending = true;
                    } else if (src) {
                        v->borrow_origin = src->borrow_origin ? src->borrow_origin : src;
                        v->borrow_parent = (src->type &&
                            (src->type->kind == TY_REF || src->type->kind == TY_MUTREF)) ? src : NULL;
                        v->borrow_mut = vt->kind == TY_MUTREF;
                    } else {
                        sema_error(&tgt->tok, "reference values may only initialize a tracked local binding");
                    }
                } else {
                    sema_error(&tgt->tok, "reference values may only initialize a tracked local binding");
                }
                validate_ref_holder_scope(v, &tgt->tok);
            }
        } else if (tgt->kind == EX_DEREF) {
            if (tgt->u.deref.operand->kind != EX_VAR)
                sema_error(&tgt->tok,
                           "writing through nested dereferences requires reborrow tracking, which is not available");
            const Type *pt = check_expr(sc, tgt->u.deref.operand);
            if (pt && pt->kind == TY_REF)
                sema_error(&tgt->tok, "cannot write through an immutable reference; use &mut");
            else if (!pt || pt->kind != TY_MUTREF)
                sema_error(&tgt->tok, "writing through a dereference requires a mutable reference");
            tt = pt && pt->elem ? pt->elem : ty_mixed;
            tgt->type = tt;
        } else if (tgt->kind == EX_INDEX || tgt->kind == EX_FIELD) {
            Expr *base = tgt->kind == EX_INDEX ? tgt->u.index.obj : tgt->u.field.obj;
            const Type *bt = check_expr(sc, base);
            if ((bt && bt->kind == TY_REF) || expr_through_shared_ref(base))
                sema_error(&tgt->tok, "cannot mutate through an immutable reference; use &mut");
            tt = check_expr(sc, tgt);
            if (tgt->kind == EX_FIELD && tgt->u.field.sf &&
                ((StructField *)tgt->u.field.sf)->is_weak &&
                e->u.assign.op.kind != T_ASSIGN)
                sema_error(&tgt->tok, "weak properties support plain assignment only");
            if (vt && (vt->kind == TY_REF || vt->kind == TY_MUTREF))
                sema_error(&tgt->tok, "references cannot be stored in fields or collection slots without lifetime tracking");
            /* element assignment on collections is allowed by runtime;
             * PHP-style: elements accept any value (arrays are dynamic) */
            if (tgt->kind == EX_INDEX) tt = NULL; /* skip element-type check */
        } else {
            tt = check_expr(sc, tgt);
            sema_error(&tgt->tok, "invalid assignment target");
        }
        if ((e->u.assign.op.kind == T_ASSIGN || e->u.assign.op.kind == T_QQASSIGN) &&
            tt && tt->kind == TY_FN && tt->thread_safe_callback &&
            e->u.assign.value)
            validate_thread_callback(e->u.assign.value, vt, &tgt->tok,
                                     "thread_callable assignment");
        if (tgt->kind != EX_VAR && e->u.assign.value && vt &&
            vt->kind != TY_REF && vt->kind != TY_MUTREF && type_is_mutable_alias(vt) &&
            expr_through_shared_ref(e->u.assign.value))
            sema_error(&tgt->tok,
                       "cannot store mutable alias %s derived from a shared reference",
                       type_to_string(vt));
        /* i128 has no 64-bit boxing: refuse the lossy stores (see
         * reject_i128_lossy). A plain `$a[0] = $wide;` lands here with the
         * element type or NULL; an element slot is an hval, so check the
         * container's element type explicitly when sema skipped it. */
        if (tgt->kind == EX_INDEX && vt && vt->kind == TY_I128) {
            const Type *ot = tgt->u.index.obj->type;
            const Type *elem = ot && ot->kind == TY_MAP ? ot->val :
                               ot && (ot->kind == TY_ARRAY || ot->kind == TY_VEC ||
                                      ot->kind == TY_SET) ? ot->elem : NULL;
            if (!elem || elem->kind != TY_I128)
                sema_error(&tgt->tok,
                           "cannot store i128 in an array element; convert with (int) or strval() first");
        } else {
            reject_i128_lossy(&e->tok, tt, vt, "cannot assign");
        }
        /* compound assignment: a .= b, a += b ... (??= behaves like =) */
        TokKind op = e->u.assign.op.kind;
        if (op != T_ASSIGN && op != T_QQASSIGN) {
            if (op == T_DOTASSIGN) {
                if (vt->kind != TY_STRING || (tt && tt->kind != TY_STRING && tt->kind != TY_MIXED))
                    sema_error(&e->u.assign.op, ".= requires string operands");
            } else if (type_is_numeric(tt ? tt->kind : TY_ERROR) && type_is_numeric(vt->kind)) {
                /* ok */
            } else if ((op == T_SHLASSIGN || op == T_SHRASSIGN) &&
                       (vt->kind == TY_STRING || vt->kind == TY_MIXED || type_is_numeric(vt->kind)) &&
                       (!tt || tt->kind == TY_MIXED || tt->kind == TY_STRING ||
                        type_is_numeric(tt->kind))) {
                /* shifts accept numeric strings and hval operands */
            } else if (op != T_DOTASSIGN &&
                       ((tt && tt->kind == TY_MIXED) || vt->kind == TY_MIXED)) {
                /* PHP-style: mixed operands (e.g. map element of untyped
                 * map) flow through the generic runtime arithmetic. */
            } else {
                sema_error(&e->u.assign.op, "invalid operand types for compound assignment");
            }
            if (op != T_DOTASSIGN && vt && vt->kind == TY_I128 &&
                tt && tt->kind != TY_I128)
                reject_i128_lossy(&e->tok, tt, vt, "compound assignment");
        }
        /* An empty array literal fits any array slot: `$xs = [];` is how a
         * typed list is cleared, and there are no elements to contradict the
         * annotation. Without this the assignment was rejected as [mixed]. */
        bool empty_lit = e->u.assign.value && e->u.assign.value->kind == EX_ARRAY_LIT &&
                         e->u.assign.value->u.arr.elems.len == 0 &&
                         e->u.assign.value->u.map.vals.len == 0;
        if ((op == T_ASSIGN || op == T_QQASSIGN) && sema_type_is_linear(vt) &&
            e->u.assign.value &&
            (e->u.assign.value->kind == EX_VAR || e->u.assign.value->kind == EX_FIELD ||
             e->u.assign.value->kind == EX_INDEX))
            e->u.assign.value->is_move_source = true;
        if (tt && !empty_lit && !value_fits_slot(tt, vt, e->u.assign.value) &&
            tt->kind != TY_MIXED && vt->kind != TY_MIXED)
            sema_error(&e->tok, "cannot assign %s to %s", type_to_string(vt), type_to_string(tt));
        if (tt && sema_type_is_linear(vt) && !sema_type_is_linear(tt))
            sema_error(&e->tok, "cannot assign ownership value %s to copyable type %s",
                       type_to_string(vt), type_to_string(tt));
        if (tt && sema_type_is_linear(tt) && !sema_type_is_linear(vt) &&
            !value_fits_slot(tt, vt, e->u.assign.value))
            sema_error(&e->tok, "cannot assign non-linear value %s to ownership type %s",
                       type_to_string(vt), type_to_string(tt));
        e->type = tt ? tt : ty_mixed;
        return e->type;
    }
    case EX_TERNARY: {
        const Type *ct = check_expr(sc, e->u.tern.cond);
        (void)ct;
        /* the branch the condition selects knows what the condition proved */
        FactSet tf = {0}, ff = {0};
        collect_facts(e->u.tern.cond, &tf, &ff);
        int mark = refine_mark();
        push_facts(&tf);
        const Type *a = check_expr(sc, e->u.tern.then);
        refine_restore(mark);
        push_facts(&ff);
        const Type *b = check_expr(sc, e->u.tern.els);
        refine_restore(mark);
        e->type = types_compatible(a, b) ? a : (types_compatible(b, a) ? b : ty_mixed);
        if (sema_type_is_linear(a) != sema_type_is_linear(b) ||
            ((sema_type_is_linear(a) || sema_type_is_linear(b)) &&
             !sema_type_is_linear(e->type)))
            sema_error(&e->tok, "ternary arms cannot erase or mix ownership types");
        /* The selected arm transfers its value into the ternary result. Mark
         * each arm separately; borrow.c records the arms as exclusive paths. */
        if (sema_type_is_linear(a) && expr_is_movable_place(e->u.tern.then))
            e->u.tern.then->is_move_source = true;
        if (sema_type_is_linear(b) && expr_is_movable_place(e->u.tern.els))
            e->u.tern.els->is_move_source = true;
        return e->type;
    }
    case EX_MATCH: {
        const Type *st = check_expr(sc, e->u.matchexpr.subject);
        (void)st;
        const Type *rt = NULL;
        /* The default arm: none of the arms matched, so a variable that a
         * type test ruled out is not of that type here either. */
        FactSet df = {0};
        for (size_t i = 0; i < e->u.matchexpr.cases.len; i++) {
            MatchCase *mc = e->u.matchexpr.cases.items[i];
            for (size_t k = 0; k < mc->patterns.len; k++)
                check_expr(sc, mc->patterns.items[k]);
            /* What the arm proves about the variables it mentions.
             *
             * A type test (`$x is T`) is a condition, so the body sees the
             * type it proved - and the default arm, where the arm was not
             * taken, sees the type it ruled out.
             *
             * A value pattern proves something too, because match compares
             * with ===: a pattern of type T can only be taken by a T. It
             * proves nothing about the default arm though - `"zz"` misses
             * `"a"` and is still a string. */
            VarSym *subjv = e->u.matchexpr.subject->kind == EX_VAR
                ? (VarSym *)e->u.matchexpr.subject->u.var.sym : NULL;
            FactSet af = {0};
            for (size_t k = 0; k < mc->patterns.len; k++) {
                const Expr *pat = mc->patterns.items[k];

                if (pat->kind == EX_IS) {
                    FactSet tf = {0}, ff = {0};
                    collect_facts(pat, &tf, &ff);
                    for (size_t j = 0; j < tf.len; j++)
                        fact_add_agree(&af, tf.items[j].var, tf.items[j].type);
                    for (size_t j = 0; j < tf.len; j++) {
                        VarSym *v = tf.items[j].var;
                        const Type *cur = fact_type_of(&df, v);
                        if (!cur) cur = v->type;
                        fact_set(&df, v, type_without_type(cur, tf.items[j].type));
                    }
                    continue;
                }

                if (!subjv || !pat->type) continue;
                if (!slot_narrowable(subjv)) continue;
                if (!slot_admits(subjv->type, pat->type)) continue;  /* never true */
                fact_add_agree(&af, subjv, pat->type);
            }
            int mark = refine_mark();
            push_facts(&af);
            const Type *bt = check_expr(sc, mc->body);
            refine_restore(mark);
            if (sema_type_is_linear(bt) && expr_is_movable_place(mc->body))
                mc->body->is_move_source = true;
            if (!rt) rt = bt;
            else if (!types_compatible(rt, bt)) rt = ty_mixed;
        }
        if (e->u.matchexpr.dflt) {
            int mark = refine_mark();
            push_facts(&df);
            const Type *bt = check_expr(sc, e->u.matchexpr.dflt);
            refine_restore(mark);
            if (sema_type_is_linear(bt) && expr_is_movable_place(e->u.matchexpr.dflt))
                e->u.matchexpr.dflt->is_move_source = true;
            if (!rt) rt = bt;
        } else if (e->u.matchexpr.cases.len == 0) {
            sema_error(&e->tok, "match expression has no arms");
        }
        e->type = rt ? rt : ty_mixed;
        if (sema_type_is_linear(e->type)) {
            /* all selected arms must retain a compatible linear result */
            for (size_t i = 0; i < e->u.matchexpr.cases.len; i++) {
                MatchCase *mc = e->u.matchexpr.cases.items[i];
                if (!sema_type_is_linear(mc->body->type))
                    sema_error(&mc->body->tok, "match arms must preserve the ownership result type");
            }
            if (e->u.matchexpr.dflt && !sema_type_is_linear(e->u.matchexpr.dflt->type))
                sema_error(&e->u.matchexpr.dflt->tok,
                           "match arms must preserve the ownership result type");
        } else {
            bool has_linear_arm = false;
            for (size_t i = 0; i < e->u.matchexpr.cases.len; i++)
                if (sema_type_is_linear(((MatchCase *)e->u.matchexpr.cases.items[i])->body->type))
                    has_linear_arm = true;
            if (e->u.matchexpr.dflt && sema_type_is_linear(e->u.matchexpr.dflt->type))
                has_linear_arm = true;
            if (has_linear_arm)
                sema_error(&e->tok, "match expression cannot erase an ownership value into a copyable result");
        }
        return e->type;
    }
    case EX_CLOSURE: {
        AstFn *fn = e->u.closure.fn;
        fn->is_closure = true;
        if (g_clo_depth >= 32) {
            sema_error(&e->tok, "closures nested too deeply");
            e->type = ty_mixed;
            return e->type;
        }
        Type *ft = type_new(TY_FN);
        ft->fn.nparams = fn->params.len;
        ft->fn.params = xmalloc(sizeof(Type *) * (fn->params.len ? fn->params.len : 1));
        /* infer param types from context: untyped closure params resolve to
           the expected signature's types when a target is known */
        for (size_t i = 0; i < fn->params.len; i++) {
            Param *pm = fn->params.items[i];
            const Type *pt = pm->type ? resolve_type(sc, pm->type, pm->name_tok) : ty_mixed;
            ((const Type **)ft->fn.params)[i] = pt;
            pm->resolved = pt;
        }
        /* push closure context; body scope seq marks the capture boundary */
        Scope fs;
        int d = g_clo_depth++;
        g_clo_fn[d] = fn;
        g_clo_caps[d] = (PtrVec){0};
        g_clo_this[d] = false;
        scope_init(&fs, sc);
        fs.cur_fn = fn;
        fs.cur_class = sc->cur_class;
        fs.self_type = sc->self_type;
        fs.in_unsafe = sc->in_unsafe;
        fs.enforce_memory_safety = fn->enforce_memory_safety;
        g_clo_body_seq[d] = fs.seq;
        /* declare real params */
        for (size_t i = 0; i < fn->params.len; i++) {
            Param *pm = fn->params.items[i];
            VarSym *v = scope_declare(&fs, pm->name, pm->resolved, pm->name_tok, pm->is_mut);
            pm->sym = v;
            v->is_param = true;
            v->owner_fn = fn;
            /* closures are called with hvals (hfn_t returns hval, arguments
             * are boxed), so a 128-bit parameter has no representation */
            if (pm->resolved && pm->resolved->kind == TY_I128)
                sema_error(&pm->name_tok,
                           "a closure cannot take an i128 parameter; convert with (int) or strval() first");
            /* PHP by-ref parameters alias the caller's variable: the callee
             * sees writes, the caller sees the callee's writes. Codegen passes
             * &slot (typed pointer); is_param_ref makes reads/writes emit
             * (*x) so the body operates through the pointer. A closure is
             * called with its arguments boxed into hvals, so it has no slot to
             * alias: say so instead of miscompiling it. */
            if (pm->by_ref) {
                sema_error(&pm->name_tok,
                           "a closure cannot take $%s by reference; capture it with use (&$%s) instead",
                           pm->name, pm->name);
            }
        }
        /* declare use() captures: by-value copies into env hvals; by-ref
         * captures (&$x) alias the outer variable through a shared heap
         * box so writes inside the closure are visible outside. */
        for (size_t i = 0; i < fn->use_vars.len; i++) {
            Param *pm = fn->use_vars.items[i];
            VarSym *outer = scope_find(sc, pm->name);
            if (!outer) {
                sema_error(&pm->name_tok, "use(): undefined variable $%s", pm->name);
                continue;
            }
            /* a closure environment holds hvals; an i128 would lose its top
             * half on the way in (and the captured read would see a string) */
            if (outer->type && outer->type->kind == TY_I128) {
                sema_error(&pm->name_tok,
                           "cannot capture i128 variable $%s in a closure; convert with (int) or strval() first",
                           pm->name);
                continue;
            }
            if (outer->is_global_alias || outer->is_program_global) {
                /* `global $x;` + `use (&$x)`: the file-scope slot is already
                 * shared, so the closure body references it directly. Boxing
                 * it here would alias the value instead of the slot and emit
                 * (*v) for a slot that is not a pointer. */
                if (fs.nvars < MAX_SCOPE) fs.vars[fs.nvars++] = outer;
                continue;
            }
            if (outer->type && (outer->type->kind == TY_REF || outer->type->kind == TY_MUTREF)) {
                sema_error(&pm->name_tok,
                           "closures cannot capture reference binding $%s without closure lifetime tracking",
                           pm->name);
                continue;
            }
            mark_heap_capture(sc, outer, &pm->name_tok, pm->by_ref);
            if (pm->by_ref && outer->is_param_ref) {
                /* the parameter is a raw pointer into the caller's frame;
                 * sharing it with a closure needs a write-back on every exit,
                 * which does not exist yet. Say so instead of emitting a box
                 * the writes never reach. */
                sema_error(&pm->name_tok,
                           "cannot capture the by-reference parameter $%s by reference; capture the caller's variable instead",
                           pm->name);
                continue;
            }
            if (pm->by_ref) {
                outer->captured_byref = true;
                outer->type = type_new(TY_MIXED);   /* boxed as hval */
            }
            if (fs.nvars < MAX_SCOPE) fs.vars[fs.nvars++] = outer;
            note_capture(outer);
        }
        /* closures without an explicit return type infer it from their
         * first `return expr;` — like PHP's ReturnTypeWillChange, simple */
        if (!fn->ret) fn->ret = type_new(TY_MIXED);
        else if (fn->ret->kind == TY_I128)
            sema_error(&fn->name_tok,
                       "a closure cannot return i128; closures return a dynamic value; convert with (int) or strval() first");
        for (size_t i = 0; i < fn->body.len; i++)
            check_stmt(&fs, fn->body.items[i]);
        fn->captures = g_clo_caps[d];
        fn->uses_this = g_clo_this[d];
        const Type *ret = fn->ret ? resolve_type(sc, fn->ret, fn->name_tok) : ty_void;
        ft->fn.ret = ret;
        fn->ftype = ft;
        ft->closure_fn = fn;
        e->type = ft;
        enqueue_borrow_fn(fn);
        g_clo_depth--;
        return e->type;
    }
    case EX_BORROW: case EX_MUTBORROW: {
        Expr *op = e->u.borrow.operand;
        if (!op || (op->kind != EX_VAR && op->kind != EX_FIELD &&
                    op->kind != EX_DEREF)) {
            sema_error(&e->tok, "can only borrow a variable, field, or dereferenced place (found %s expression)",
                       expr_kind_name(op ? op->kind : EX_NULL));
            e->type = ty_mixed;
            return e->type;
        }
        const Type *source_type = check_expr(sc, op);
        VarSym *v = expr_root_var(op);
        if (!v) {
            if (op->kind == EX_VAR)
                sema_error(&op->tok, "undefined variable $%s", op->u.var.name);
            else
                sema_error(&op->tok, "cannot determine the owner of this field place");
            e->type = ty_mixed;
            return e->type;
        }
        if (g_clo_depth > 0 && v->decl_seq < g_clo_body_seq[g_clo_depth - 1])
            note_capture(v);
        bool is_mut = e->kind == EX_MUTBORROW;
        bool reborrow = v->type &&
            (v->type->kind == TY_REF || v->type->kind == TY_MUTREF);
    if (!reborrow && heap_alias_captured(sc, v) && type_is_mutable_alias(v->type))
            sema_error(&e->tok,
                       "cannot borrow object $%s while a closure may retain an alias; capture lifetime tracking is unavailable",
                       v->name);
        if (is_mut && reborrow && v->type->kind != TY_MUTREF)
            sema_error(&e->tok, "cannot mutably reborrow shared reference $%s", v->name);
        if (is_mut && v->shared_alias)
            sema_error(&e->tok,
                       "cannot create a mutable borrow from $%s because it aliases data reached through a shared reference",
                       v->name);
        if (is_mut && !reborrow && !v->is_mut)
            sema_error(&e->tok, "cannot mutably borrow immutable $%s", v->name);
        if (op->kind == EX_VAR) op->u.var.sym = v; /* codegen needs the resolved slot */
        Type *t = type_new(is_mut ? TY_MUTREF : TY_REF);
        /* Borrowing the reference variable itself reborrows its referent.
         * Borrowing a projected field through that reference instead has the
         * field's type, while retaining the same underlying loan origin. */
        t->elem = reborrow && op->kind == EX_VAR ? v->type->elem : source_type;
        e->borrow_origin = v->borrow_origin ? (void *)v->borrow_origin : (void *)v;
        e->borrow_parent = reborrow ? v : NULL;
        if (reborrow && op->kind == EX_FIELD)
            e->borrow_place = v->borrow_place
                ? compose_borrow_projection(v->borrow_place, op) : op;
        else if (reborrow && op->kind == EX_DEREF)
            e->borrow_place = v->borrow_place ? v->borrow_place : op;
        else
            e->borrow_place = reborrow && v->borrow_place ? v->borrow_place : op;
        e->type = t;
        return e->type;
    }
    case EX_DEREF: {
        const Type *t = check_expr(sc, e->u.deref.operand);
        if (!type_is_pointerish(t->kind)) {
            sema_error(&e->tok, "cannot dereference non-pointer type %s", type_to_string(t));
            e->type = ty_mixed;
            return e->type;
        }
        if (t->kind == TY_PTR && !sc->in_unsafe)
            sema_error(&e->tok, "dereferencing a raw pointer requires an unsafe block");
        e->type = t->elem ? t->elem : ty_mixed;
        return e->type;
    }
    case EX_CAST: {
        const Type *from = check_expr(sc, e->u.cast.operand);
        const Type *to = resolve_type(sc, (Type *)e->u.cast.to, e->u.cast.ty_tok);
        if ((sema_type_is_linear(from) || sema_type_is_linear(to)) &&
            !types_equal(from, to))
            sema_error(&e->tok,
                       "casts cannot change an ownership type; use an explicit move with the same type");
        if (from && (from->kind == TY_REF || from->kind == TY_MUTREF))
            sema_error(&e->tok, "references cannot be cast because casts would bypass lifetime checking");
        if (to->kind == TY_PTR && !sc->in_unsafe)
            sema_error(&e->tok, "cast to raw pointer '%s' requires an unsafe block", type_to_string(to));
        if (to->kind == TY_PTR && !type_is_pointerish(from->kind) && from->kind != TY_INT)
            sema_error(&e->tok, "cannot cast %s to raw pointer", type_to_string(from));
        /* i128 only converts to scalars (and to another i128); anything else
         * would have to go through an hval, which has 64 bits */
        if (from && from->kind == TY_I128) {
            switch (to->kind) {
            case TY_INT: case TY_I8: case TY_I16: case TY_I32: case TY_I64:
            case TY_I128: case TY_FLOAT: case TY_F32: case TY_F64:
            case TY_BOOL: case TY_STRING:
                break;
            default:
                sema_error(&e->tok,
                           "cannot cast i128 to %s; convert with (int) or strval() first",
                           type_to_string(to));
                break;
            }
        }
        e->type = to;
        return e->type;
    }
    case EX_IS: {
        check_expr(sc, e->u.ischeck.operand);
        resolve_type(sc, (Type *)e->u.ischeck.to, e->u.ischeck.ty_tok);
        e->type = ty_bool;
        return e->type;
    }
    }
    e->type = ty_mixed;
    return e->type;
}

/* Parser recovery and a few partially-built expressions can leave an inner
 * checker without a type. Keep that representation local: every caller of
 * check_expr receives a real error-tolerant type and can keep diagnosing. */
static const Type *check_expr(Scope *sc, Expr *e) {
    if (!e) return ty_mixed;
    const Type *t = check_expr_inner(sc, e);
    if (!t) t = ty_mixed;
    e->type = t;
    return t;
}

const char *expr_kind_name(ExprKind k) {
    switch (k) {
    case EX_INT: return "int";
    case EX_FLOAT: return "float";
    case EX_STR: return "string";
    case EX_TPL: return "string interpolation";
    case EX_BOOL: return "bool";
    case EX_NULL: return "null";
    case EX_VAR: return "variable";
    case EX_INDEX: return "index";
    case EX_FIELD: return "field access";
    case EX_CALL: return "function call";
    case EX_METHOD: return "method call";
    case EX_STATIC: return "static call";
    case EX_NEW: return "new";
    case EX_BIN: return "binary";
    case EX_UN: return "unary";
    case EX_ASSIGN: return "assignment";
    case EX_TERNARY: return "ternary";
    case EX_MATCH: return "match";
    case EX_CLOSURE: return "closure";
    case EX_ARRAY_LIT: return "array literal";
    case EX_MAP_LIT: return "map literal";
    case EX_TUPLE_LIT: return "tuple";
    case EX_BORROW: return "borrow";
    case EX_MUTBORROW: return "mutable borrow";
    case EX_DEREF: return "dereference";
    case EX_CAST: return "cast";
    case EX_IS: return "is";
    default: return "expression";
    }
}

/* Can this value be used as a condition?
 *
 * A bool is a bool. A dynamic value - mixed, a nullable, a union - is
 * truthiness-tested at runtime, the way PHP does it: null is falsy, "" and
 * 0 as well. Everything else has to be a bool already. */
static bool type_is_condition(const Type *t) {
    if (!t) return true;
    switch (t->kind) {
    case TY_BOOL: case TY_MIXED: case TY_NULL: case TY_NULLABLE: case TY_UNION:
        return true;
    default:
        return false;
    }
}

/* ---------------- statements ---------------- */
static void check_block(Scope *sc, PtrVec stmts);

/* Find a direct assignment to a variable in a branch.  If both arms of an
 * if assign a previously unseen name, PHP-style function scope makes that
 * binding available after the if.  Predeclare it in the enclosing scope so
 * both assignments and later reads resolve to the same VarSym. */
static Expr *if_direct_assignment(PtrVec stmts, const char *name) {
    for (size_t i = 0; i < stmts.len; i++) {
        Stmt *stmt = stmts.items[i];
        if (!stmt || stmt->kind != ST_EXPR || !stmt->u.expr.expr) continue;
        Expr *expr = stmt->u.expr.expr;
        if (expr->kind == EX_ASSIGN && expr->u.assign.target &&
            expr->u.assign.target->kind == EX_VAR &&
            expr->u.assign.target->u.var.name == intern(name))
            return expr;
    }
    return NULL;
}

/* The scalar type an expression will certainly have, judged before it is
 * checked and without side effects: literals, typed variables already in
 * scope, builtins with a fixed int return, and integer arithmetic of those.
 * NULL means "not sure"; the caller then keeps the dynamic slot. */
static const Type *peek_scalar_type(Scope *sc, Expr *e) {
    if (!e) return NULL;
    switch (e->kind) {
    case EX_INT: return ty_int;
    case EX_FLOAT: return ty_float;
    case EX_BOOL: return ty_bool;
    case EX_VAR: {
        VarSym *v = scope_find(sc, e->u.var.name);
        if (!v || !v->type || v->captured_byref) return NULL;
        if (v->type->kind == TY_INT) return ty_int;
        if (v->type->kind == TY_FLOAT) return ty_float;
        if (v->type->kind == TY_BOOL) return ty_bool;
        return NULL;
    }
    case EX_CALL: {
        Expr *callee = e->u.call.fn;
        if (!callee || callee->kind != EX_VAR) return NULL;
        const char *name = callee->u.var.name;
        if (!callee->u.var.bare_name && scope_find(sc, name)) return NULL;
        if (find_function(name)) return NULL;
        const Builtin *b = builtin_lookup(name);
        if (!b || b->ret != ty_int) return NULL;
        if (b->shape == B_MAXMIN || b->shape == B_ABS) return NULL;
        return ty_int;
    }
    case EX_UN:
        return NULL;
    case EX_BIN: {
        switch (e->u.bin.op.kind) {
        case T_PLUS: case T_MINUS: case T_STAR: case T_AMP: case T_PIPE:
        case T_CARET: case T_SHL: case T_SHR:
            break;
        default:
            return NULL;
        }
        const Type *l = peek_scalar_type(sc, e->u.bin.l);
        const Type *r = peek_scalar_type(sc, e->u.bin.r);
        return (l == ty_int && r == ty_int) ? ty_int : NULL;
    }
    default:
        return NULL;
    }
}

static void promote_if_definite_assignments(Scope *sc, PtrVec then_stmts,
                                            PtrVec else_stmts) {
    for (size_t i = 0; i < then_stmts.len; i++) {
        Stmt *stmt = then_stmts.items[i];
        if (!stmt || stmt->kind != ST_EXPR || !stmt->u.expr.expr) continue;
        Expr *expr = stmt->u.expr.expr;
        if (expr->kind != EX_ASSIGN || !expr->u.assign.target ||
            expr->u.assign.target->kind != EX_VAR) continue;
        const char *name = expr->u.assign.target->u.var.name;
        Expr *other = scope_find(sc, name) ? NULL : if_direct_assignment(else_stmts, name);
        if (!other) continue;
        /* Both arms certainly store the same scalar type: the slot gets that
         * type instead of a dynamic one (no boxing on every use). */
        const Type *slot = ty_mixed;
        if (expr->u.assign.op.kind == T_ASSIGN && other->u.assign.op.kind == T_ASSIGN &&
            !expr->u.assign.ann && !other->u.assign.ann) {
            const Type *a = peek_scalar_type(sc, expr->u.assign.value);
            const Type *b = peek_scalar_type(sc, other->u.assign.value);
            if (a && a == b) slot = a;
        }
        VarSym *v = scope_declare(sc, name, slot, expr->u.assign.target->tok, true);
        if (!v) continue;
        v->is_auto = true;
        if (sc->cur_fn == NULL && !program_global_find(name))
            ptrvec_push(&g_program_globals, v);
    }
}

static void check_stmt(Scope *sc, Stmt *s) {
    switch (s->kind) {
    case ST_EXPR:
        check_expr(sc, s->u.expr.expr);
        break;
    case ST_GLOBAL: {
        /* PHP `global $a, $b;` — bind each name to the program-global symbol
         * (one shared VarSym per name across the whole program). */
        for (size_t i = 0; i < s->u.globals.names.len; i++) {
            const char *name = intern(s->u.globals.names.items[i]);
            Token tok = *(Token *)s->u.globals.toks.items[i];
            /* prefer an existing top-level declaration ($x = ... at script
             * scope) so both bind to one C variable */
            Scope *gsc = sc;
            while (gsc->parent) gsc = gsc->parent;
            VarSym *gv = NULL;
            for (size_t j = 0; j < gsc->nvars; j++) {
                if (gsc->vars[j]->name == name && !gsc->vars[j]->is_global_alias) {
                    gv = gsc->vars[j];
                    break;
                }
            }
            if (!gv) gv = program_global_find(name);
            if (!gv) gv = program_global_get(name, tok);
            else if (!program_global_find(name)) ptrvec_push(&g_program_globals, gv);
            /* file-scope `global` storage is an hval slot */
            if (gv->type && gv->type->kind == TY_I128) {
                sema_error(&tok, "cannot share i128 variable $%s through global; convert with (int) or strval() first",
                           name);
                break;
            }
            gv->is_mut = true;
            gv->is_auto = true;
            gv->is_program_global = true;   /* storage becomes file-scope */
            /* expose the SAME symbol under this local scope so all uses in
             * this function share one uid => one C variable */
            VarSym *lv = xcalloc(1, sizeof(VarSym));
            *lv = *gv;                       /* same name/type/uid */
            lv->is_global_alias = true;
            lv->is_mut = true;
            if (sc->nvars < MAX_SCOPE) sc->vars[sc->nvars++] = lv;
            ptrvec_push(&s->u.globals.syms, gv);   /* codegen: file-scope storage */
        }
        break;
    }
    case ST_LET: {
        /* `let x = e` always has an initialiser. `let mut $x: T;` is a
         * declaration without one: the form for that is `$x: T;`, and a
         * binding with neither a value nor a type has nothing to be. Both
         * used to be read through a NULL init and take the compiler down. */
        if (!s->u.let.init && !s->u.let.ann) {
            sema_error(&s->tok, "let binding $%s has neither a type nor an initial value",
                       s->u.let.name);
            break;
        }
        if (!s->u.let.init) {
            /* a declared type with no value: a slot that gets assigned
             * later. It is hoisted and zero-initialised like the `$x: T;`
             * form, so appending to an array or assigning a string has
             * something to write into. */
            const Type *at = resolve_type(sc, s->u.let.ann, s->tok);
            VarSym *dv = scope_declare(sc, s->u.let.name, at, s->u.let.name_tok, s->u.let.is_mut);
            dv->is_auto = true;
            s->u.let.sym = dv;
            break;
        }
        const Type *it = check_expr(sc, s->u.let.init);
        const Type *t = NULL;
        if (s->u.let.ann) {
            t = resolve_type(sc, s->u.let.ann, s->tok);
            if (sema_type_is_linear(it) && !sema_type_is_linear(t))
                sema_error(&s->tok,
                           "cannot initialize copyable $%s with ownership value %s",
                           s->u.let.name, type_to_string(it));
            if (!value_fits_slot(t, it, s->u.let.init) && t->kind != TY_MIXED)
                sema_error(&s->tok, "initial value for $%s has type %s, expected %s",
                           s->u.let.name, type_to_string(it), type_to_string(t));
        } else {
            /* `$x = null` leaves the binding dynamic (PHP-style) */
            t = (it && it->kind == TY_NULL) ? ty_mixed : it;
        }
        VarSym *v = scope_declare(sc, s->u.let.name, t, s->u.let.name_tok, s->u.let.is_mut);
        s->u.let.sym = v;
        if (t && t->kind == TY_FN && t->thread_safe_callback)
            validate_thread_callback(s->u.let.init, it, &s->tok,
                                     "thread_callable binding");
        if (type_can_contain_heap_alias(t)) {
            Expr *init = s->u.let.init;
            if (init && init->kind == EX_VAR) {
                VarSym *src = init->u.var.sym;
                bool source_was_unaliased = src && !src->is_mut && !src->alias_unknown;
                if (!v->is_mut && source_was_unaliased)
                    v->alias_origin = src->alias_origin ? src->alias_origin : src;
                else
                    v->alias_unknown = true;
                if (src) src->alias_unknown = true;
            } else if (!expr_is_fresh_heap_value(init)) {
                v->alias_unknown = true;
            }
        }
        if (it && (it->kind == TY_REF || it->kind == TY_MUTREF)) {
            if (!t || (t->kind != TY_REF && t->kind != TY_MUTREF))
                sema_error(&s->tok,
                           "cannot erase a reference lifetime by storing the reference as %s",
                           type_to_string(t ? t : ty_mixed));
            if (s->u.let.init->kind == EX_BORROW || s->u.let.init->kind == EX_MUTBORROW) {
                Expr *borrow = s->u.let.init;
                Expr *operand = borrow->u.borrow.operand;
                VarSym *src = borrow->borrow_origin
                    ? (VarSym *)borrow->borrow_origin : expr_root_var(operand);
                if (!src) {
                    sema_error(&s->tok, "cannot determine the lifetime of this borrow");
                } else if (src->borrow_pending) {
                    v->borrow_pending = true;
                } else {
                    v->borrow_origin = borrow->borrow_origin
                        ? borrow->borrow_origin : (src->borrow_origin ? src->borrow_origin : src);
                    v->borrow_parent = borrow->borrow_parent ? borrow->borrow_parent :
                        ((operand->kind == EX_VAR && src->type &&
                          (src->type->kind == TY_REF || src->type->kind == TY_MUTREF)) ? src : NULL);
                    v->borrow_place = borrow->borrow_place;
                    v->borrow_mut = it->kind == TY_MUTREF;
                }
            } else if (s->u.let.init->kind == EX_VAR) {
                VarSym *src = scope_find(sc, s->u.let.init->u.var.name);
                if (src && src->type &&
                    (src->type->kind == TY_REF || src->type->kind == TY_MUTREF)) {
                    if (src->borrow_pending) {
                        v->borrow_pending = true;
                    } else {
                        v->borrow_origin = src->borrow_origin ? src->borrow_origin : src;
                        v->borrow_parent = src;
                        v->borrow_place = src->borrow_place;
                        v->borrow_mut = it->kind == TY_MUTREF;
                    }
                } else {
                    sema_error(&s->tok, "cannot track the lifetime of this reference value");
                }
            } else if (s->u.let.init->borrow_origin) {
                v->borrow_origin = s->u.let.init->borrow_origin;
                v->borrow_parent = s->u.let.init->borrow_parent;
                v->borrow_place = s->u.let.init->borrow_place;
                v->borrow_mut = it->kind == TY_MUTREF;
            } else if (target_returns_ref(ref_return_target(s->u.let.init))) {
                /* A forward-declared callee's borrow-return summary is not
                 * known until all function bodies have been checked. */
                v->borrow_pending = true;
            } else {
                sema_error(&s->tok, "cannot store a reference with an unknown lifetime");
            }
        }
        if (it && it->kind != TY_REF && it->kind != TY_MUTREF &&
            type_is_mutable_alias(it) && expr_through_shared_ref(s->u.let.init))
            v->shared_alias = true;
        validate_ref_holder_scope(v, &s->tok);
        /* Storing a linear value consumes its source place. */
        if (it && sema_type_is_linear(s->u.let.init->type) &&
            types_compatible(it, s->u.let.init->type) &&
            (s->u.let.init->kind == EX_VAR || s->u.let.init->kind == EX_FIELD ||
             s->u.let.init->kind == EX_INDEX))
            s->u.let.init->is_move_source = true;
        s->u.let.init->is_stmt_value = false;
        (void)v;
        break;
    }
    case ST_RETURN: {
        const Type *rt = sc->cur_fn && sc->cur_fn->ret ? resolve_type(sc, sc->cur_fn->ret, s->tok) : ty_void;
        if (s->u.ret.value) {
            const Type *vt = check_expr(sc, s->u.ret.value);
            if (vt && vt->kind != TY_REF && vt->kind != TY_MUTREF &&
                type_is_mutable_alias(vt) && expr_through_shared_ref(s->u.ret.value))
                sema_error(&s->tok,
                           "cannot return mutable alias %s derived from a shared reference",
                           type_to_string(vt));
            if (sema_type_is_linear(vt) &&
                (s->u.ret.value->kind == EX_VAR || s->u.ret.value->kind == EX_FIELD ||
                 s->u.ret.value->kind == EX_INDEX))
                s->u.ret.value->is_move_source = true;
            if (vt && (vt->kind == TY_REF || vt->kind == TY_MUTREF)) {
                VarSym *origin = NULL;
                if (s->u.ret.value->borrow_origin)
                    origin = s->u.ret.value->borrow_origin;
                else if (s->u.ret.value->kind == EX_BORROW || s->u.ret.value->kind == EX_MUTBORROW)
                    origin = s->u.ret.value->borrow_origin
                        ? (VarSym *)s->u.ret.value->borrow_origin
                        : expr_root_var(s->u.ret.value->u.borrow.operand);
                else if (s->u.ret.value->kind == EX_VAR) {
                    VarSym *rv = scope_find(sc, s->u.ret.value->u.var.name);
                    origin = rv ? (VarSym *)rv->borrow_origin : NULL;
                    if (rv && rv->is_param) origin = rv;
                }
                bool pending_call = target_returns_ref(
                    ref_return_target(s->u.ret.value));
                bool pending_local = s->u.ret.value->kind == EX_VAR &&
                    s->u.ret.value->u.var.sym &&
                    ((VarSym *)s->u.ret.value->u.var.sym)->borrow_pending;
                if (pending_call || pending_local)
                    s->u.ret.value->borrow_lifetime_deferred = true;
                if (!borrow_origin_outlives_function(origin) && !pending_call && !pending_local)
                    sema_error(&s->tok, "cannot return a reference to a local variable; the referent would not live long enough");
            }
            /* an hval-typed value (untyped property, mixed call, map entry)
             * converts at runtime — PHP-style dynamic returns are allowed */
            reject_i128_lossy(&s->tok, rt, vt, "cannot return");
            bool boxed = vt && (vt->kind == TY_MIXED || vt->kind == TY_OPTION ||
                                vt->kind == TY_RESULT);
            if (sema_type_is_linear(vt) && !sema_type_is_linear(rt))
                sema_error(&s->tok,
                           "cannot return ownership value %s through copyable return type %s",
                           type_to_string(vt), type_to_string(rt ? rt : ty_mixed));
            if (rt && rt->kind != TY_VOID && rt->kind != TY_MIXED &&
                (!boxed || sema_type_is_linear(rt) || sema_type_is_linear(vt)) &&
                !value_fits_slot(rt, vt, s->u.ret.value))
                sema_error(&s->tok, "return type mismatch: expected %s, got %s",
                           type_to_string(rt), type_to_string(vt));
            if (rt && rt->kind == TY_VOID)
                sema_error(&s->tok, "returning a value from a void function");
        } else if (rt && rt->kind != TY_VOID && rt->kind != TY_MIXED) {
            sema_error(&s->tok, "non-void function must return a value of %s", type_to_string(rt));
        }
        break;
    }
    case ST_ECHO:
        for (size_t i = 0; i < s->u.echo.exprs.len; i++)
            check_expr(sc, s->u.echo.exprs.items[i]);
        break;
    case ST_PRINT:
        check_expr(sc, s->u.print.expr);
        break;
    case ST_IF: {
        const Type *ct = check_expr(sc, s->u.if_.cond);
        if (!type_is_condition(ct))
            sema_error(&s->tok, "if condition must be bool, got %s", type_to_string(ct));
        promote_if_definite_assignments(sc, s->u.if_.then, s->u.if_.els);
        /* what the condition proves holds inside the branch it guards */
        FactSet tf = {0}, ff = {0};
        collect_facts(s->u.if_.cond, &tf, &ff);
        int mark = refine_mark();
        unsigned then_seq = g_write_seq;
        push_facts(&tf);
        Scope then_sc;
        scope_child_init(&then_sc, sc);
        check_block(&then_sc, s->u.if_.then);
        refine_restore(mark);
        unsigned else_seq = g_write_seq;
        push_facts(&ff);
        Scope else_sc;
        scope_child_init(&else_sc, sc);
        check_block(&else_sc, s->u.if_.els);
        refine_restore(mark);
        /* A branch that never falls through - return, throw, break,
         * continue - leaves only one way to reach the code below the
         * statement: the other branch's condition held, so its facts hold
         * there too (check_block cuts the stack back at the end of the
         * block around the if). The branch that survived may have written
         * the variable itself though, and then the fact is about a value
         * that is gone. */
        bool then_exits = block_always_exits(s->u.if_.then);
        bool else_exits = s->u.if_.els.len > 0 && block_always_exits(s->u.if_.els);
        if (then_exits && !else_exits) {
            for (size_t k = 0; k < ff.len; k++)
                if (ff.items[k].var->write_seq <= else_seq)
                    refine_push(ff.items[k].var, ff.items[k].type);
        } else if (else_exits && !then_exits) {
            for (size_t k = 0; k < tf.len; k++)
                if (tf.items[k].var->write_seq <= then_seq)
                    refine_push(tf.items[k].var, tf.items[k].type);
        }
        break;
    }
    case ST_WHILE: {
        const Type *ct = check_expr(sc, s->u.while_.cond);
        if (!type_is_condition(ct))
            sema_error(&s->tok, "while condition must be bool, got %s", type_to_string(ct));
        sc->loop_depth++;
        /* the condition is checked again on every turn, so what it proves
         * holds at the top of each iteration */
        FactSet tf = {0}, ff = {0};
        collect_facts(s->u.while_.cond, &tf, &ff);
        int mark = refine_mark();
        push_facts(&tf);
        Scope body_sc;
        scope_child_init(&body_sc, sc);
        check_block(&body_sc, s->u.while_.body);
        refine_restore(mark);
        sc->loop_depth--;
        break;
    }
    case ST_DO: {
        sc->loop_depth++;
        Scope body_sc;
        scope_child_init(&body_sc, sc);
        check_block(&body_sc, s->u.do_.body);
        sc->loop_depth--;
        const Type *ct = check_expr(sc, s->u.do_.cond);
        if (!type_is_condition(ct))
            sema_error(&s->tok, "do-while condition must be bool, got %s", type_to_string(ct));
        break;
    }
    case ST_SWITCH: {
        const Type *ct = check_expr(sc, s->u.switch_.cond);
        bool cond_num = ct->kind == TY_INT || ct->kind == TY_I8 ||
                        ct->kind == TY_I16 || ct->kind == TY_I32 ||
                        ct->kind == TY_I64 || ct->kind == TY_FLOAT ||
                        ct->kind == TY_F32 || ct->kind == TY_F64;
        bool cond_str = ct->kind == TY_STRING || ct->kind == TY_MIXED;
        /* a nullable/union slot (or a mixed one) holds an hval: its cases
         * compare with PHP's loose ==, and a case can narrow the slot */
        bool cond_dyn = ct->kind == TY_NULLABLE || ct->kind == TY_UNION ||
                        ct->kind == TY_MIXED;
        if (!cond_num && !cond_str && !cond_dyn)
            sema_error(&s->tok, "switch condition must be int or string, got %s",
                       type_to_string(ct));
        /* the only slot a case can pin down: a narrowable variable subject */
        VarSym *subjv = NULL;
        if (cond_dyn && s->u.switch_.cond->kind == EX_VAR) {
            VarSym *v = (VarSym *)s->u.switch_.cond->u.var.sym;
            if (v && slot_narrowable(v)) subjv = v;
        }
        sc->loop_depth++;  /* 'break' exits a switch, like PHP */
        /* PHP runs the body from the matching label onward, so a body can
         * also be reached by falling out of the case above it - and then it
         * knows nothing: the case above matched a different value. */
        bool prev_falls_out = false;
        for (size_t i = 0; i < s->u.switch_.cases.len; i++) {
            SwitchCase *cse = s->u.switch_.cases.items[i];
            FactSet tf = {0};
            const Type *sole = NULL;
            bool prove = subjv && !prev_falls_out && !cse->is_default;
            if (prove) {
                sole = sole_nonnull_scalar_member(subjv->type);
                if (!sole) prove = false;
            }
            for (size_t k = 0; k < cse->patterns.len; k++) {
                Expr *pat = cse->patterns.items[k];
                const Type *pt = check_expr(sc, pat);
                if (!cond_dyn) {
                    bool ok = (cond_num && (pt->kind == TY_INT || pt->kind == TY_I8 ||
                                            pt->kind == TY_I16 || pt->kind == TY_I32 ||
                                            pt->kind == TY_I64 || pt->kind == TY_FLOAT ||
                                            pt->kind == TY_F32 || pt->kind == TY_F64 ||
                                            pt->kind == TY_MIXED)) ||
                              (cond_str && (pt->kind == TY_STRING || pt->kind == TY_MIXED));
                    if (!ok)
                        sema_error(&pat->tok, "case type %s does not match switch type %s",
                                   type_to_string(pt), type_to_string(ct));
                }
                /* A truthy literal cannot be equal to null, so the arm ran
                 * for the one non-null member the slot has - while a falsy
                 * one is taken by null as well. One pattern that proves
                 * nothing lets the whole arm prove nothing: the arm ran for
                 * any of them. */
                if (prove && !literal_is_truthy(pat)) prove = false;
            }
            int mark = refine_mark();
            if (prove) {
                fact_add(&tf, subjv, sole);
                push_facts(&tf);
            }
            Scope case_sc;
            scope_child_init(&case_sc, sc);
            check_block(&case_sc, cse->body);
            refine_restore(mark);
            prev_falls_out = !block_always_exits(cse->body);
        }
        sc->loop_depth--;
        break;
    }
    case ST_FOR: {
        for (size_t i = 0; i < s->u.for_.init.len; i++)
            check_stmt(sc, s->u.for_.init.items[i]);
        if (s->u.for_.cond) {
            const Type *ct = check_expr(sc, s->u.for_.cond);
            if (!type_is_condition(ct))
                sema_error(&s->tok, "for condition must be bool, got %s", type_to_string(ct));
        }
        for (size_t i = 0; i < s->u.for_.step.len; i++)
            check_stmt(sc, s->u.for_.step.items[i]);
        sc->loop_depth++;
        Scope body_sc;
        scope_child_init(&body_sc, sc);
        check_block(&body_sc, s->u.for_.body);
        sc->loop_depth--;
        break;
    }
    case ST_FOREACH: {
        const Type *it = check_expr(sc, s->u.foreach.iter);
        if (sema_type_is_linear(it))
            sema_error(&s->tok, "foreach cannot copy elements from a collection containing ownership values");
        const Type *elem = NULL, *key = NULL;
        if (it->kind == TY_ARRAY) {
            /* an `array` becomes a keyed map as soon as a string key is
             * assigned (PHP semantics), so its keys are dynamic: typing them
             * int read a string key's pointer as a number */
            elem = it->elem;
            key = ty_mixed;
        } else if (it->kind == TY_VEC || it->kind == TY_SET) {
            elem = it->elem;
            key = ty_int;
        } else if (it->kind == TY_MAP) {
            elem = it->val;
            key = it->key;
        } else if (it->kind == TY_STRING) {
            elem = ty_string;
            key = ty_int;
        } else if (it->kind == TY_MIXED || it->kind == TY_OPTION ||
                   it->kind == TY_RESULT) {
            /* dynamically typed container: the runtime iterator decides */
            elem = ty_mixed;
            key = ty_mixed;
        } else {
            sema_error(&s->tok, "foreach expects an iterable, got %s", type_to_string(it));
            elem = ty_mixed;
        }
        Scope inner;
        scope_init(&inner, sc);
        inner.cur_fn = sc->cur_fn;
        inner.cur_class = sc->cur_class;
        inner.in_unsafe = sc->in_unsafe;
        inner.loop_depth = sc->loop_depth + 1;
        inner.self_type = sc->self_type;
        scope_declare(&inner, s->u.foreach.var, elem, s->u.foreach.var_tok, false);
        if (s->u.foreach.kvar) scope_declare(&inner, s->u.foreach.kvar, key, s->u.foreach.var_tok, false);
        s->u.foreach.vsym = inner.vars[inner.nvars - (s->u.foreach.kvar ? 2 : 1)];
        if (s->u.foreach.kvar) s->u.foreach.ksym = inner.vars[inner.nvars - 1];
        /* foreach copies the element slot, but heap values inside that slot
         * remain aliases of values reachable through the collection (and may
         * also alias values elsewhere). The runtime stores class/container
         * values as pointers, so treating the loop binding as a fresh owner
         * would let writes through it bypass a live borrow. */
        if (elem && type_is_mutable_alias(elem))
            ((VarSym *)s->u.foreach.vsym)->alias_unknown = true;
        if (s->u.foreach.ksym && key && type_is_mutable_alias(key))
            ((VarSym *)s->u.foreach.ksym)->alias_unknown = true;
        if (expr_through_shared_ref(s->u.foreach.iter)) {
            if (elem && elem->kind != TY_REF && elem->kind != TY_MUTREF &&
                type_is_mutable_alias(elem))
                ((VarSym *)s->u.foreach.vsym)->shared_alias = true;
            if (s->u.foreach.ksym && key && key->kind != TY_REF && key->kind != TY_MUTREF &&
                type_is_mutable_alias(key))
                ((VarSym *)s->u.foreach.ksym)->shared_alias = true;
        }
        check_block(&inner, s->u.foreach.body);
        break;
    }
    case ST_BREAK: case ST_CONTINUE:
        if (sc->loop_depth == 0)
            sema_error(&s->tok, "'%s' outside of a loop", s->kind == ST_BREAK ? "break" : "continue");
        break;
    case ST_BLOCK: {
        Scope inner;
        scope_init(&inner, sc);
        inner.cur_fn = sc->cur_fn;
        inner.cur_class = sc->cur_class;
        inner.in_unsafe = sc->in_unsafe;
        inner.loop_depth = sc->loop_depth;
        inner.self_type = sc->self_type;
        check_block(&inner, s->u.block.stmts);
        break;
    }
    case ST_UNSAFE_BLOCK: {
        if (sc->enforce_memory_safety)
            sema_error(&s->tok, "unsafe block is forbidden by declare(enforce_memory_safety=1)");
        Scope inner;
        scope_init(&inner, sc);
        inner.cur_fn = sc->cur_fn;
        inner.cur_class = sc->cur_class;
        inner.in_unsafe = true;
        inner.loop_depth = sc->loop_depth;
        inner.self_type = sc->self_type;
        check_block(&inner, s->u.unsafe.stmts);
        break;
    }
    case ST_TRY: {
        Scope try_sc;
        scope_child_init(&try_sc, sc);
        check_block(&try_sc, s->u.try.body);
        for (size_t i = 0; i < s->u.try.catches.len; i++) {
            CatchClause *cc = s->u.try.catches.items[i];
            Scope inner;
            scope_init(&inner, sc);
            inner.cur_fn = sc->cur_fn;
            inner.cur_class = sc->cur_class;
            inner.in_unsafe = sc->in_unsafe;
            inner.loop_depth = sc->loop_depth;
            inner.self_type = sc->self_type;
            const Type *et = cc->type ? resolve_type(sc, (Type *)cc->type, cc->var_tok) : ty_mixed;
            VarSym *cv = scope_declare(&inner, cc->var, et, cc->var_tok, false);
            cc->sym = cv;
            check_block(&inner, cc->body);
        }
        if (s->u.try.fin.len) check_block(sc, s->u.try.fin);
        break;
    }
    case ST_THROW: {
        const Type *t = check_expr(sc, s->u.throw.expr);
        if (t->kind != TY_STRING && t->kind != TY_MIXED &&
            t->kind != TY_CLASS && t->kind != TY_OWN)
            sema_error(&s->tok, "can only throw strings or exceptions, got %s", type_to_string(t));
        break;
    }
    }
}

static void check_block(Scope *sc, PtrVec stmts) {
    /* a refinement a statement in this block established does not outlive
     * it: the block is the branch that proved it */
    int mark = refine_mark();
    for (size_t i = 0; i < stmts.len; i++)
        check_stmt(sc, stmts.items[i]);
    refine_restore(mark);
}

/* ---------------- function & class bodies ---------------- */
/* does this function contain any `return <value>;`? (stmt_chain_returns
 * already walks nested blocks, loops and ifs) */
static bool fn_has_value_return(AstFn *fn) {
    for (size_t i = 0; i < fn->body.len; i++)
        if (stmt_chain_returns(fn->body.items[i])) return true;
    return false;
}

static void check_fn_body(Scope *parent, AstFn *fn, const Type *self_type) {
    /* The checker state is shared across declarations. Never let a loan from
     * a previous function influence this function, or escape this one. */
    Scope sc;
    scope_init(&sc, parent);
    sc.cur_fn = fn;
    sc.self_type = self_type;
    sc.enforce_memory_safety = fn->enforce_memory_safety;
    for (size_t i = 0; i < fn->params.len; i++) {
        Param *pm = fn->params.items[i];
        const Type *pt = pm->type ? resolve_type(&sc, pm->type, pm->name_tok) : ty_mixed;
        pm->resolved = pt;
        VarSym *v = scope_declare(&sc, pm->name, pt, pm->name_tok, pm->is_mut);
        v->is_param = true;
        v->owner_fn = fn;
        /* by-ref param: alias the caller's slot (see check_fn_body) */
        if (pm->by_ref) {
            v->is_param_ref = true;
            v->is_mut = true;
        }
        pm->sym = v;
    }
    if (fn->is_method && self_type) {
        Type *receiver_ref = type_new(TY_MUTREF);
        receiver_ref->elem = self_type;
        VarSym *receiver = scope_declare(&sc, s_this, receiver_ref,
                                        fn->name_tok, true);
        receiver->is_param = true;
        receiver->owner_fn = fn;
        fn->this_sym = receiver;
    } else {
        fn->this_sym = NULL;
    }
    /* PHP-style: a function with no written return type is checked as if it
     * returned mixed when any statement returns a value (its type is then
     * inferred before any call site is checked). Only a fn with no value
     * returns at all behaves as void. */
    if (fn && !fn->ret && fn_has_value_return(fn))
        fn->ret = ty_mixed;
    /* refinements do not outlive the function (fn bodies are walked
     * statement by statement, not through check_block) */
    int rmark = refine_mark();
    for (size_t i = 0; i < fn->body.len; i++)
        check_stmt(&sc, fn->body.items[i]);
    refine_restore(rmark);
    enqueue_borrow_fn(fn);
    if (fn->ret && fn->ret->kind != TY_VOID && fn->body.len) {
        /* not a full flow analysis; simple check: last stmt return? */
        Stmt *last = fn->body.items[fn->body.len - 1];
        if (last->kind != ST_RETURN && fn->ret->kind != TY_MIXED) {
            /* warn only, codegen inserts return 0 */
        }
    }
}

static void check_class(AstClass *c) {
    Scope sc;
    scope_init(&sc, NULL);
    sc.cur_class = c;
    sc.self_type = class_type_of(c);
    /* field types — an untyped field's default also fixes its type, so
     * `pub $x = 1;` is an int property and `pub $cb = null;` is hval */
    for (size_t i = 0; i < c->fields.len; i++) {
        StructField *f = c->fields.items[i];
        if (f->type) {
            const Type *ft = resolve_type(&sc, f->type, f->name_tok);
            if (f->is_weak && ft->kind != TY_CLASS && ft->kind != TY_INTERFACE)
                sema_error(&f->name_tok, "weak property $%s must have a class or interface type", f->name);
            if (f->is_weak && f->is_static)
                sema_error(&f->name_tok, "weak properties cannot be static");
            if (f->is_weak && f->dflt && f->dflt->kind != EX_NULL)
                sema_error(&f->name_tok, "weak property defaults must be null");
        } else if (f->dflt) {
            const Type *dt = check_expr(&sc, f->dflt);
            /* `pub $cb = null;` is a dynamic slot: the field holds a
             * callback later, so it is an hval, not a null-only slot. */
            f->type = (dt && dt->kind == TY_NULL) ? (Type *)ty_mixed
                                                   : (dt ? (Type *)dt : type_new(TY_MIXED));
        } else {
            sema_error(&f->name_tok, "field $%s needs a type annotation", f->name);
        }
        if (f->is_weak && !f->type)
            sema_error(&f->name_tok, "weak property $%s needs an explicit class type", f->name);
    }
    /* methods */
    for (size_t i = 0; i < c->methods.len; i++) {
        AstFn *m = c->methods.items[i];
        m->cls = c;
        if (strcmp(m->name, "__construct") == 0) m->is_ctor = true;
        check_fn_body(&sc, m, sc.self_type);
    }
    /* interface methods must be abstract */
    if (c->is_interface) {
        for (size_t i = 0; i < c->methods.len; i++) {
            AstFn *m = c->methods.items[i];
            if (m->body.len) {
                /* allowed (default impl) */
            }
        }
    }
}

/* Resolve reference-returning calls after every function body has a borrow
 * summary. This makes forward calls behave like calls to earlier functions. */
static bool resolve_ref_calls_stmt(Stmt *s);

static bool resolve_ref_call_summary(Expr *e, AstFn *target, PtrVec args) {
    if (!e || !target_returns_ref(target)) return false;
    int index = fn_return_ref_param(target);
    if (index == -3 && e->kind == EX_METHOD) {
        Expr *receiver_expr = e->u.method.obj;
        VarSym *src = expr_root_var(receiver_expr);
        if (!src) return false;
        VarSym *origin = src->borrow_origin ? (VarSym *)src->borrow_origin : src;
        VarSym *parent = src->type &&
            (src->type->kind == TY_REF || src->type->kind == TY_MUTREF) ? src : NULL;
        if (e->has_borrow_receiver && e->borrow_origin == origin &&
            e->borrow_parent == parent) return false;
        const Expr *receiver_place = src->borrow_origin ? src->borrow_place :
            (parent ? (src->borrow_place ? src->borrow_place : receiver_expr)
                    : receiver_expr);
        if (receiver_expr && receiver_expr->kind != EX_VAR &&
            src->borrow_origin && src->borrow_place)
            receiver_place = compose_borrow_projection(src->borrow_place, receiver_expr);
        const Expr *relative_place = fn_return_receiver_place(target);
        const Expr *place = relative_place && receiver_place
            ? compose_borrow_projection(receiver_place, relative_place)
            : receiver_place;
        if (e->type->kind == TY_MUTREF &&
            (parent ? src->type->kind != TY_MUTREF : !src->is_mut)) {
            sema_error(&e->tok,
                       "cannot return a mutable borrow from method %s() through immutable receiver",
                       target->name);
        }
        e->has_borrow_receiver = true;
        e->has_borrow_param = false;
        e->borrow_param_index = -1;
        e->borrow_origin = origin;
        e->borrow_parent = parent;
        e->borrow_place = place;
        return true;
    }
    if (index >= 0 && (size_t)index < args.len) {
        Expr *arg = args.items[index];
        VarSym *src = arg && arg->kind == EX_VAR ? (VarSym *)arg->u.var.sym : NULL;
        VarSym *origin = src && !src->borrow_pending
            ? (VarSym *)(src->borrow_origin ? src->borrow_origin : src) : NULL;
        VarSym *parent = src && src->type &&
            (src->type->kind == TY_REF || src->type->kind == TY_MUTREF) ? src : NULL;
        if (!src && arg && arg->borrow_origin) {
            VarSym *borrowed = (VarSym *)arg->borrow_origin;
            if (!borrowed->borrow_pending) {
                origin = borrowed;
                parent = (VarSym *)arg->borrow_parent;
            }
        } else if (!src && arg &&
                   (arg->kind == EX_BORROW || arg->kind == EX_MUTBORROW)) {
            VarSym *borrowed = expr_root_var(arg->u.borrow.operand);
            if (borrowed && !borrowed->borrow_pending)
                origin = borrowed->borrow_origin ?
                    (VarSym *)borrowed->borrow_origin : borrowed;
        }
        const Expr *place = src ? src->borrow_place : (arg ? arg->borrow_place : NULL);
        if (!origin) return false;
        if (e->has_borrow_param && e->borrow_param_index == index &&
            e->borrow_origin == origin && e->borrow_parent == parent &&
            e->borrow_place == place) return false;
        e->borrow_param_index = index;
        e->has_borrow_param = true;
        e->borrow_origin = origin;
        e->borrow_parent = parent;
        e->borrow_place = place;
        return true;
    }
    VarSym *global = fn_return_ref_global(target);
    if (global && (e->borrow_origin != global || e->borrow_parent ||
                   e->has_borrow_param)) {
        e->borrow_origin = global;
        e->borrow_parent = NULL;
        e->borrow_place = NULL;
        e->has_borrow_param = false;
        return true;
    }
    return false;
}

static bool resolve_ref_calls_expr(Expr *e) {
    if (!e) return false;
    bool changed = false;
#define REX(x) do { if (resolve_ref_calls_expr((x))) changed = true; } while (0)
    switch (e->kind) {
    case EX_CALL:
        REX(e->u.call.fn);
        for (size_t i = 0; i < e->u.call.args.len; i++) REX(e->u.call.args.items[i]);
        if (resolve_ref_call_summary(e, e->u.call.target, e->u.call.args)) changed = true;
        break;
    case EX_METHOD:
        REX(e->u.method.obj);
        for (size_t i = 0; i < e->u.method.args.len; i++) REX(e->u.method.args.items[i]);
        if (resolve_ref_call_summary(e, e->u.method.target, e->u.method.args)) changed = true;
        break;
    case EX_STATIC:
        for (size_t i = 0; i < e->u.staticcall.args.len; i++) REX(e->u.staticcall.args.items[i]);
        if (resolve_ref_call_summary(e, e->u.staticcall.target, e->u.staticcall.args)) changed = true;
        break;
    case EX_NEW:
        for (size_t i = 0; i < e->u.newexpr.args.len; i++) REX(e->u.newexpr.args.items[i]);
        break;
    case EX_INDEX: REX(e->u.index.obj); REX(e->u.index.idx); break;
    case EX_FIELD: REX(e->u.field.obj); break;
    case EX_BIN: REX(e->u.bin.l); REX(e->u.bin.r); REX(e->u.bin.slice_hi); break;
    case EX_UN: REX(e->u.un.operand); break;
    case EX_ASSIGN:
        REX(e->u.assign.target);
        REX(e->u.assign.value);
        if (e->u.assign.target && e->u.assign.target->kind == EX_VAR && e->u.assign.value) {
            VarSym *v = (VarSym *)e->u.assign.target->u.var.sym;
            Expr *rhs = e->u.assign.value;
            VarSym *src = NULL;
            VarSym *origin = NULL;
            VarSym *parent = NULL;
            const Expr *place = NULL;
            if (rhs->borrow_origin) {
                origin = (VarSym *)rhs->borrow_origin;
                parent = (VarSym *)rhs->borrow_parent;
                place = rhs->borrow_place;
            } else if (rhs->kind == EX_VAR) {
                src = (VarSym *)rhs->u.var.sym;
                if (src && !src->borrow_pending) {
                    origin = src->borrow_origin ? (VarSym *)src->borrow_origin : src;
                    parent = src;
                    place = src->borrow_place;
                }
            } else if (rhs->kind == EX_BORROW || rhs->kind == EX_MUTBORROW) {
                Expr *operand = rhs->u.borrow.operand;
                src = rhs->borrow_origin ? (VarSym *)rhs->borrow_origin : expr_root_var(operand);
                if (src && !src->borrow_pending) {
                    origin = src->borrow_origin ? (VarSym *)src->borrow_origin : src;
                    if (src->type && (src->type->kind == TY_REF || src->type->kind == TY_MUTREF))
                        parent = src;
                    place = rhs->borrow_place;
                }
            }
            if (v && v->borrow_pending && origin) {
                v->borrow_origin = origin;
                v->borrow_parent = parent;
                v->borrow_place = place;
                v->borrow_mut = v->type && v->type->kind == TY_MUTREF;
                v->borrow_pending = false;
                changed = true;
            }
            validate_ref_holder_scope(v, &rhs->tok);
        }
        break;
    case EX_TERNARY: REX(e->u.tern.cond); REX(e->u.tern.then); REX(e->u.tern.els); break;
    case EX_MATCH:
        REX(e->u.matchexpr.subject);
        for (size_t i = 0; i < e->u.matchexpr.cases.len; i++) {
            MatchCase *mc = e->u.matchexpr.cases.items[i];
            for (size_t j = 0; j < mc->patterns.len; j++) REX(mc->patterns.items[j]);
            REX(mc->body);
        }
        REX(e->u.matchexpr.dflt);
        break;
    case EX_CLOSURE:
        if (e->u.closure.fn)
            for (size_t i = 0; i < e->u.closure.fn->body.len; i++)
                if (resolve_ref_calls_stmt(e->u.closure.fn->body.items[i])) changed = true;
        break;
    case EX_ARRAY_LIT:
        if (e->u.map.vals.len) {
            for (size_t i = 0; i < e->u.map.keys.len; i++) REX(e->u.map.keys.items[i]);
            for (size_t i = 0; i < e->u.map.vals.len; i++) REX(e->u.map.vals.items[i]);
        } else {
            for (size_t i = 0; i < e->u.arr.elems.len; i++) REX(e->u.arr.elems.items[i]);
        }
        break;
    case EX_MAP_LIT:
        for (size_t i = 0; i < e->u.map.keys.len; i++) REX(e->u.map.keys.items[i]);
        for (size_t i = 0; i < e->u.map.vals.len; i++) REX(e->u.map.vals.items[i]);
        break;
    case EX_TUPLE_LIT:
        for (size_t i = 0; i < e->u.tuple.elems.len; i++) REX(e->u.tuple.elems.items[i]);
        break;
    case EX_BORROW: case EX_MUTBORROW: REX(e->u.borrow.operand); break;
    case EX_DEREF: REX(e->u.deref.operand); break;
    case EX_CAST: REX(e->u.cast.operand); break;
    case EX_IS: REX(e->u.ischeck.operand); break;
    case EX_ELLIPSIS: REX(e->u.ellipsis.inner); break;
    default: break;
    }
#undef REX
    return changed;
}

static bool resolve_ref_calls_seq(PtrVec stmts);

static bool resolve_ref_calls_stmt(Stmt *s) {
    if (!s) return false;
    bool changed = false;
#define RST(x) do { if (resolve_ref_calls_seq((x))) changed = true; } while (0)
#define RSE(x) do { if (resolve_ref_calls_expr((x))) changed = true; } while (0)
    switch (s->kind) {
    case ST_EXPR: RSE(s->u.expr.expr); break;
    case ST_LET:
        RSE(s->u.let.init);
        if (s->u.let.sym && s->u.let.init && s->u.let.init->borrow_origin) {
            VarSym *v = s->u.let.sym;
            if (v->borrow_origin != s->u.let.init->borrow_origin ||
                v->borrow_parent != s->u.let.init->borrow_parent ||
                v->borrow_place != s->u.let.init->borrow_place || v->borrow_pending) {
                v->borrow_origin = s->u.let.init->borrow_origin;
                v->borrow_parent = s->u.let.init->borrow_parent;
                v->borrow_place = s->u.let.init->borrow_place;
                v->borrow_mut = v->type && v->type->kind == TY_MUTREF;
                v->borrow_pending = false;
                changed = true;
            }
        } else if (s->u.let.sym && s->u.let.init &&
                   (s->u.let.init->kind == EX_VAR || s->u.let.init->kind == EX_BORROW ||
                    s->u.let.init->kind == EX_MUTBORROW)) {
            VarSym *v = s->u.let.sym;
            Expr *init = s->u.let.init;
            VarSym *src = init->kind == EX_VAR ? (VarSym *)init->u.var.sym :
                init->borrow_origin ? (VarSym *)init->borrow_origin :
                expr_root_var(init->u.borrow.operand);
            if (src && !src->borrow_pending) {
                VarSym *origin = src->borrow_origin ? (VarSym *)src->borrow_origin : src;
                VarSym *parent = init->kind == EX_VAR ? src :
                    (src->type && (src->type->kind == TY_REF || src->type->kind == TY_MUTREF) ? src : NULL);
                if (v->borrow_origin != origin || v->borrow_parent != parent ||
                    v->borrow_place != init->borrow_place || v->borrow_pending) {
                    v->borrow_origin = origin;
                    v->borrow_parent = parent;
                    v->borrow_place = init->borrow_place;
                    v->borrow_mut = v->type && v->type->kind == TY_MUTREF;
                    v->borrow_pending = false;
                    changed = true;
                }
            }
        }
        if (s->u.let.sym) validate_ref_holder_scope(s->u.let.sym, &s->tok);
        break;
    case ST_RETURN: RSE(s->u.ret.value); break;
    case ST_ECHO:
        for (size_t i = 0; i < s->u.echo.exprs.len; i++) RSE(s->u.echo.exprs.items[i]);
        break;
    case ST_PRINT: RSE(s->u.print.expr); break;
    case ST_IF: RSE(s->u.if_.cond); RST(s->u.if_.then); RST(s->u.if_.els); break;
    case ST_WHILE: RSE(s->u.while_.cond); RST(s->u.while_.body); break;
    case ST_DO: RSE(s->u.do_.cond); RST(s->u.do_.body); break;
    case ST_FOR: RST(s->u.for_.init); RSE(s->u.for_.cond); RST(s->u.for_.step); RST(s->u.for_.body); break;
    case ST_FOREACH: RSE(s->u.foreach.iter); RST(s->u.foreach.body); break;
    case ST_BLOCK: RST(s->u.block.stmts); break;
    case ST_UNSAFE_BLOCK: RST(s->u.unsafe.stmts); break;
    case ST_TRY:
        RST(s->u.try.body); RST(s->u.try.fin);
        for (size_t i = 0; i < s->u.try.catches.len; i++)
            RST(((CatchClause *)s->u.try.catches.items[i])->body);
        break;
    case ST_THROW: RSE(s->u.throw.expr); break;
    case ST_SWITCH:
        RSE(s->u.switch_.cond);
        for (size_t i = 0; i < s->u.switch_.cases.len; i++) {
            SwitchCase *cs = s->u.switch_.cases.items[i];
            for (size_t j = 0; j < cs->patterns.len; j++) RSE(cs->patterns.items[j]);
            RST(cs->body);
        }
        break;
    default: break;
    }
#undef RST
#undef RSE
    return changed;
}

static bool resolve_ref_calls_seq(PtrVec stmts) {
    bool changed = false;
    for (size_t i = 0; i < stmts.len; i++)
        if (resolve_ref_calls_stmt(stmts.items[i])) changed = true;
    return changed;
}

/* A call's borrow summary can be unavailable when its caller is checked (for
 * example, a forward declaration). Such returns are deferred above. Once
 * summaries have reached a fixed point, recheck that the resolved referent
 * outlives the current function; otherwise codegen would return a pointer to
 * one of this function's stack slots. */
static void check_deferred_ref_returns(AstFn *fn, PtrVec stmts) {
    if (!fn) return;
    for (size_t i = 0; i < stmts.len; i++) {
        Stmt *s = stmts.items[i];
        if (!s) continue;
        if (s->kind == ST_RETURN && s->u.ret.value &&
            s->u.ret.value->borrow_lifetime_deferred) {
            Expr *value = s->u.ret.value;
            const VarSym *origin = (const VarSym *)value->borrow_origin;
            if (!origin && value->kind == EX_VAR && value->u.var.sym) {
                VarSym *holder = value->u.var.sym;
                origin = holder->borrow_origin
                    ? (const VarSym *)holder->borrow_origin
                    : (holder->borrow_pending ? NULL : holder);
            }
            if (origin && !borrow_origin_outlives_function(origin))
                sema_error(&s->tok,
                           "cannot return a reference to a local variable; the referent would not live long enough");
        }
        switch (s->kind) {
        case ST_IF:
            check_deferred_ref_returns(fn, s->u.if_.then);
            check_deferred_ref_returns(fn, s->u.if_.els);
            break;
        case ST_WHILE: check_deferred_ref_returns(fn, s->u.while_.body); break;
        case ST_DO: check_deferred_ref_returns(fn, s->u.do_.body); break;
        case ST_FOR:
            check_deferred_ref_returns(fn, s->u.for_.init);
            check_deferred_ref_returns(fn, s->u.for_.step);
            check_deferred_ref_returns(fn, s->u.for_.body);
            break;
        case ST_FOREACH: check_deferred_ref_returns(fn, s->u.foreach.body); break;
        case ST_BLOCK: check_deferred_ref_returns(fn, s->u.block.stmts); break;
        case ST_UNSAFE_BLOCK: check_deferred_ref_returns(fn, s->u.unsafe.stmts); break;
        case ST_TRY:
            check_deferred_ref_returns(fn, s->u.try.body);
            check_deferred_ref_returns(fn, s->u.try.fin);
            for (size_t j = 0; j < s->u.try.catches.len; j++)
                check_deferred_ref_returns(fn,
                    ((CatchClause *)s->u.try.catches.items[j])->body);
            break;
        case ST_SWITCH:
            for (size_t j = 0; j < s->u.switch_.cases.len; j++)
                check_deferred_ref_returns(fn,
                    ((SwitchCase *)s->u.switch_.cases.items[j])->body);
            break;
        default: break;
        }
    }
}

static void diagnose_unresolved_ref_returns(PtrVec stmts) {
    for (size_t i = 0; i < stmts.len; i++) {
        Stmt *s = stmts.items[i];
        if (!s) continue;
        if (s->kind == ST_LET && s->u.let.sym &&
            ((VarSym *)s->u.let.sym)->borrow_pending) {
            sema_error(&s->tok, "cannot resolve the lifetime returned by this function call");
        } else if (s->kind == ST_EXPR && s->u.expr.expr &&
                   s->u.expr.expr->kind == EX_ASSIGN &&
                   s->u.expr.expr->u.assign.target &&
                   s->u.expr.expr->u.assign.target->kind == EX_VAR &&
                   s->u.expr.expr->u.assign.target->u.var.sym &&
                   ((VarSym *)s->u.expr.expr->u.assign.target->u.var.sym)->borrow_pending) {
            sema_error(&s->tok, "cannot resolve the lifetime returned by this function call");
        } else if (s->kind == ST_RETURN && s->u.ret.value &&
                   (ref_return_target(s->u.ret.value) ||
                    s->u.ret.value->kind == EX_VAR)) {
            Expr *value = s->u.ret.value;
            bool pending = target_returns_ref(ref_return_target(value)) &&
                !value->borrow_origin;
            pending = pending || (value->kind == EX_VAR && value->u.var.sym &&
                                  ((VarSym *)value->u.var.sym)->borrow_pending);
            if (pending)
                sema_error(&s->tok, "cannot resolve the lifetime returned by this function call");
        }
        switch (s->kind) {
        case ST_IF:
            diagnose_unresolved_ref_returns(s->u.if_.then);
            diagnose_unresolved_ref_returns(s->u.if_.els);
            break;
        case ST_WHILE: diagnose_unresolved_ref_returns(s->u.while_.body); break;
        case ST_DO: diagnose_unresolved_ref_returns(s->u.do_.body); break;
        case ST_FOR: diagnose_unresolved_ref_returns(s->u.for_.init);
            diagnose_unresolved_ref_returns(s->u.for_.step);
            diagnose_unresolved_ref_returns(s->u.for_.body); break;
        case ST_FOREACH: diagnose_unresolved_ref_returns(s->u.foreach.body); break;
        case ST_BLOCK: diagnose_unresolved_ref_returns(s->u.block.stmts); break;
        case ST_UNSAFE_BLOCK: diagnose_unresolved_ref_returns(s->u.unsafe.stmts); break;
        case ST_TRY:
            diagnose_unresolved_ref_returns(s->u.try.body);
            diagnose_unresolved_ref_returns(s->u.try.fin);
            for (size_t j = 0; j < s->u.try.catches.len; j++)
                diagnose_unresolved_ref_returns(((CatchClause *)s->u.try.catches.items[j])->body);
            break;
        case ST_SWITCH:
            for (size_t j = 0; j < s->u.switch_.cases.len; j++)
                diagnose_unresolved_ref_returns(((SwitchCase *)s->u.switch_.cases.items[j])->body);
            break;
        default: break;
        }
    }
}

/* Conservative first escape summary for safety mode. A parameter is
 * borrow-only when every direct use is an argument to a read-only builtin or
 * another parameter already proven borrow-only. All other expression forms
 * recurse conservatively, so return/store/closure/unknown-call uses disqualify
 * the summary. */
static bool escape_expr_safe(Expr *e, VarSym *param);

bool sema_method_param_borrow_only(const Expr *call, size_t arg_index) {
    if (!call || call->kind != EX_METHOD || !call->u.method.obj || call->builtin ||
        !call->u.method.target || !call->u.method.target->is_method ||
        call->u.method.target->is_static ||
        arg_index >= call->u.method.target->params.len)
        return false;

    AstFn *target = call->u.method.target;
    Type *receiver_type = (Type *)call->u.method.obj->type;
    while (receiver_type && (receiver_type->kind == TY_OWN ||
                             receiver_type->kind == TY_RC))
        receiver_type = (Type *)receiver_type->elem;
    if (!receiver_type || receiver_type->kind != TY_CLASS ||
        !receiver_type->decl || !target->cls)
        return false; /* interfaces and unknown dispatch stay reference-counted */
    AstClass *static_cls = (AstClass *)receiver_type->decl;
    if (!class_derives(static_cls, target->cls)) return false;

    Param *base_param = target->params.items[arg_index];
    if (!base_param || !base_param->borrow_only) return false;

    /* The statically resolved implementation applies unless a compiled
     * descendant overrides it; include it even when its owner is an ancestor
     * of the receiver's static class. */
    bool saw_impl = target->has_body && !target->is_abstract;
    for (size_t i = 0; i < g_borrow_fns.len; i++) {
        AstFn *candidate = g_borrow_fns.items[i];
        if (!candidate || !candidate->is_method || candidate->is_static ||
            !candidate->cls || candidate->cls->is_interface ||
            strcmp(candidate->name, target->name) != 0 ||
            !class_derives(candidate->cls, static_cls))
            continue;
        /* Codegen's method table currently treats same-named private methods
         * in descendants as dispatchable entries too, so include them here. */
        if (candidate->is_abstract && !candidate->has_body) continue;
        if (!candidate->has_body || arg_index >= candidate->params.len)
            return false;
        Param *pm = candidate->params.items[arg_index];
        if (!pm || !pm->borrow_only) return false;
        saw_impl = true;
    }
    return saw_impl;
}

static bool escape_arg_safe(Expr *arg, VarSym *param) {
    if (arg && arg->kind == EX_VAR && arg->u.var.sym == param) return true;
    return escape_expr_safe(arg, param);
}

static bool escape_expr_safe(Expr *e, VarSym *param) {
    if (!e) return true;
#define SAFE_CHILD(x) do { if (!escape_expr_safe((x), param)) return false; } while (0)
    switch (e->kind) {
    case EX_VAR:
        return e->u.var.sym != param;
    case EX_CALL: {
        bool borrow_builtin = e->builtin &&
            (!strcmp(e->builtin, "count") || !strcmp(e->builtin, "array_sum") ||
             !strcmp(e->builtin, "strlen"));
        AstFn *target = e->u.call.target;
        for (size_t i = 0; i < e->u.call.args.len; i++) {
            Expr *arg = e->u.call.args.items[i];
            bool borrow_arg = borrow_builtin;
            if (!borrow_arg && target && i < target->params.len)
                borrow_arg = ((Param *)target->params.items[i])->borrow_only;
            if (borrow_arg) {
                if (!escape_arg_safe(arg, param)) return false;
            } else SAFE_CHILD(arg);
        }
        if (e->u.call.fn && e->u.call.fn->kind != EX_VAR)
            SAFE_CHILD(e->u.call.fn);
        return true;
    }
    case EX_TPL:
        for (size_t i = 0; i < e->u.tpl.exprs.len; i++) SAFE_CHILD(e->u.tpl.exprs.items[i]);
        return true;
    case EX_INDEX: SAFE_CHILD(e->u.index.obj); SAFE_CHILD(e->u.index.idx); return true;
    case EX_FIELD: SAFE_CHILD(e->u.field.obj); return true;
    case EX_METHOD: {
        SAFE_CHILD(e->u.method.obj);
        for (size_t i = 0; i < e->u.method.args.len; i++) {
            Expr *arg = e->u.method.args.items[i];
            bool borrow_arg = sema_method_param_borrow_only(e, i);
            if (borrow_arg) {
                if (!escape_arg_safe(arg, param)) return false;
            } else SAFE_CHILD(arg);
        }
        return true;
    }
    case EX_STATIC:
        for (size_t i = 0; i < e->u.staticcall.args.len; i++) SAFE_CHILD(e->u.staticcall.args.items[i]);
        return true;
    case EX_NEW:
        for (size_t i = 0; i < e->u.newexpr.args.len; i++) SAFE_CHILD(e->u.newexpr.args.items[i]);
        return true;
    case EX_BIN: SAFE_CHILD(e->u.bin.l); SAFE_CHILD(e->u.bin.r); SAFE_CHILD(e->u.bin.slice_hi); return true;
    case EX_UN: SAFE_CHILD(e->u.un.operand); return true;
    case EX_ASSIGN: SAFE_CHILD(e->u.assign.target); SAFE_CHILD(e->u.assign.value); return true;
    case EX_TERNARY: SAFE_CHILD(e->u.tern.cond); SAFE_CHILD(e->u.tern.then); SAFE_CHILD(e->u.tern.els); return true;
    case EX_MATCH:
        SAFE_CHILD(e->u.matchexpr.subject);
        for (size_t i = 0; i < e->u.matchexpr.cases.len; i++) {
            MatchCase *mc = e->u.matchexpr.cases.items[i];
            for (size_t j = 0; j < mc->patterns.len; j++) SAFE_CHILD(mc->patterns.items[j]);
            SAFE_CHILD(mc->body);
        }
        SAFE_CHILD(e->u.matchexpr.dflt); return true;
    case EX_CLOSURE:
        /* Capture summaries are not a complete escape proof yet. */
        return false;
    case EX_ARRAY_LIT:
        for (size_t i = 0; i < e->u.arr.elems.len; i++) SAFE_CHILD(e->u.arr.elems.items[i]);
        return true;
    case EX_MAP_LIT:
        for (size_t i = 0; i < e->u.map.keys.len; i++) SAFE_CHILD(e->u.map.keys.items[i]);
        for (size_t i = 0; i < e->u.map.vals.len; i++) SAFE_CHILD(e->u.map.vals.items[i]);
        return true;
    case EX_TUPLE_LIT:
        for (size_t i = 0; i < e->u.tuple.elems.len; i++) SAFE_CHILD(e->u.tuple.elems.items[i]);
        return true;
    case EX_BORROW: case EX_MUTBORROW: SAFE_CHILD(e->u.borrow.operand); return true;
    case EX_DEREF: SAFE_CHILD(e->u.deref.operand); return true;
    case EX_CAST: SAFE_CHILD(e->u.cast.operand); return true;
    case EX_IS: SAFE_CHILD(e->u.ischeck.operand); return true;
    case EX_ELLIPSIS: SAFE_CHILD(e->u.ellipsis.inner); return true;
    default: return true;
    }
#undef SAFE_CHILD
}

static bool escape_stmts_safe(PtrVec stmts, VarSym *param) {
    for (size_t i = 0; i < stmts.len; i++) {
        Stmt *s = stmts.items[i];
        if (!s) continue;
#define SAFE_STMTS(v) do { if (!escape_stmts_safe((v), param)) return false; } while (0)
        switch (s->kind) {
        case ST_EXPR: if (!escape_expr_safe(s->u.expr.expr, param)) return false; break;
        case ST_LET: if (!escape_expr_safe(s->u.let.init, param)) return false; break;
        case ST_RETURN: if (!escape_expr_safe(s->u.ret.value, param)) return false; break;
        case ST_ECHO:
            for (size_t j=0;j<s->u.echo.exprs.len;j++) if(!escape_expr_safe(s->u.echo.exprs.items[j],param)) return false;
            break;
        case ST_PRINT: if (!escape_expr_safe(s->u.print.expr, param)) return false; break;
        case ST_IF:
            if (!escape_expr_safe(s->u.if_.cond,param)) return false;
            SAFE_STMTS(s->u.if_.then); SAFE_STMTS(s->u.if_.els); break;
        case ST_WHILE: if(!escape_expr_safe(s->u.while_.cond,param)) return false; SAFE_STMTS(s->u.while_.body); break;
        case ST_DO: if(!escape_expr_safe(s->u.do_.cond,param)) return false; SAFE_STMTS(s->u.do_.body); break;
        case ST_FOR:
            SAFE_STMTS(s->u.for_.init); if(!escape_expr_safe(s->u.for_.cond,param)) return false;
            SAFE_STMTS(s->u.for_.step); SAFE_STMTS(s->u.for_.body); break;
        case ST_FOREACH: if(!escape_expr_safe(s->u.foreach.iter,param)) return false; SAFE_STMTS(s->u.foreach.body); break;
        case ST_BLOCK: SAFE_STMTS(s->u.block.stmts); break;
        case ST_UNSAFE_BLOCK: SAFE_STMTS(s->u.unsafe.stmts); break;
        case ST_THROW: if(!escape_expr_safe(s->u.throw.expr,param)) return false; break;
        case ST_TRY:
            SAFE_STMTS(s->u.try.body); SAFE_STMTS(s->u.try.fin);
            for(size_t j=0;j<s->u.try.catches.len;j++) SAFE_STMTS(((CatchClause*)s->u.try.catches.items[j])->body);
            break;
        case ST_SWITCH:
            if(!escape_expr_safe(s->u.switch_.cond,param)) return false;
            for(size_t j=0;j<s->u.switch_.cases.len;j++) {
                SwitchCase *scase=s->u.switch_.cases.items[j];
                for(size_t k=0;k<scase->patterns.len;k++)
                    if(!escape_expr_safe(scase->patterns.items[k],param)) return false;
                SAFE_STMTS(scase->body);
            }
            break;
        default: break;
        }
#undef SAFE_STMTS
    }
    return true;
}

static bool escape_summary_type(const Type *t) {
    if (!t) return false;
    switch (t->kind) {
    case TY_STRING: case TY_ARRAY: case TY_VEC: case TY_MAP: case TY_SET:
        return true;
    default:
        return false;
    }
}

static void infer_borrow_only_params(Program *prog) {
    PtrVec functions = {0};
    for (size_t i = 0; i < prog->decls.len; i++) {
        Decl *d = prog->decls.items[i];
        if (d->kind == DK_FN) ptrvec_push(&functions, d->u.fn);
        else if (d->kind == DK_CLASS || d->kind == DK_TRAIT || d->kind == DK_INTERFACE) {
            AstClass *cls = d->u.cls;
            for (size_t j = 0; j < cls->methods.len; j++)
                ptrvec_push(&functions, cls->methods.items[j]);
        }
    }
    for (size_t i = 0; i < functions.len; i++) {
        AstFn *fn = functions.items[i];
        if (!fn || !fn->has_body || !fn->enforce_memory_safety ||
            fn->is_closure || fn->is_abstract) continue;
        for (size_t j = 0; j < fn->params.len; j++) {
            Param *pm = fn->params.items[j];
            pm->borrow_only = !pm->by_ref && escape_summary_type(pm->resolved);
        }
    }
    bool changed;
    do {
        changed = false;
        for (size_t i = 0; i < functions.len; i++) {
            AstFn *fn = functions.items[i];
            if (!fn || !fn->has_body || !fn->enforce_memory_safety ||
                fn->is_closure || fn->is_abstract) continue;
            for (size_t j = 0; j < fn->params.len; j++) {
                Param *pm = fn->params.items[j];
                if (!pm->borrow_only) continue;
                if (!escape_stmts_safe(fn->body, (VarSym *)pm->sym)) {
                    pm->borrow_only = false;
                    changed = true;
                }
            }
        }
    } while (changed);
    free(functions.items);
}

static bool scalar_array_literal(const Expr *e) {
    if (!e || e->kind != EX_ARRAY_LIT) return false;
    for (size_t i = 0; i < e->u.arr.elems.len; i++) {
        Expr *item = e->u.arr.elems.items[i];
        if (!item || (item->kind != EX_INT && item->kind != EX_FLOAT &&
                      item->kind != EX_BOOL && item->kind != EX_NULL)) return false;
    }
    return true;
}

static bool region_string_init(const Expr *e) {
    return e && e->type && e->type->kind == TY_STRING &&
           (e->kind == EX_BIN || e->kind == EX_TPL || e->kind == EX_STR);
}

static bool expr_uses_sym(const Expr *e, const VarSym *sym) {
    if (!e) return false;
#define USES(x) do { if (expr_uses_sym((x), sym)) return true; } while (0)
    switch (e->kind) {
    case EX_VAR: return e->u.var.sym == sym;
    case EX_FIELD: USES(e->u.field.obj); break;
    case EX_INDEX: USES(e->u.index.obj); USES(e->u.index.idx); break;
    case EX_CALL:
        USES(e->u.call.fn);
        for (size_t i = 0; i < e->u.call.args.len; i++) USES(e->u.call.args.items[i]);
        break;
    case EX_METHOD:
        USES(e->u.method.obj);
        for (size_t i = 0; i < e->u.method.args.len; i++) USES(e->u.method.args.items[i]);
        break;
    case EX_STATIC:
        for (size_t i = 0; i < e->u.staticcall.args.len; i++) USES(e->u.staticcall.args.items[i]);
        break;
    case EX_NEW:
        for (size_t i = 0; i < e->u.newexpr.args.len; i++) USES(e->u.newexpr.args.items[i]);
        break;
    case EX_TPL:
        for (size_t i = 0; i < e->u.tpl.exprs.len; i++) USES(e->u.tpl.exprs.items[i]);
        break;
    case EX_BIN: USES(e->u.bin.l); USES(e->u.bin.r); USES(e->u.bin.slice_hi); break;
    case EX_UN: USES(e->u.un.operand); break;
    case EX_ASSIGN: USES(e->u.assign.target); USES(e->u.assign.value); break;
    case EX_TERNARY: USES(e->u.tern.cond); USES(e->u.tern.then); USES(e->u.tern.els); break;
    case EX_MATCH:
        USES(e->u.matchexpr.subject);
        for (size_t i = 0; i < e->u.matchexpr.cases.len; i++) {
            MatchCase *mc = e->u.matchexpr.cases.items[i];
            for (size_t j = 0; j < mc->patterns.len; j++) USES(mc->patterns.items[j]);
            USES(mc->body);
        }
        USES(e->u.matchexpr.dflt);
        break;
    case EX_ARRAY_LIT:
        for (size_t i = 0; i < e->u.arr.elems.len; i++) USES(e->u.arr.elems.items[i]);
        break;
    case EX_MAP_LIT:
        for (size_t i = 0; i < e->u.map.keys.len; i++) USES(e->u.map.keys.items[i]);
        for (size_t i = 0; i < e->u.map.vals.len; i++) USES(e->u.map.vals.items[i]);
        break;
    case EX_TUPLE_LIT:
        for (size_t i = 0; i < e->u.tuple.elems.len; i++) USES(e->u.tuple.elems.items[i]);
        break;
    case EX_BORROW: case EX_MUTBORROW: USES(e->u.borrow.operand); break;
    case EX_DEREF: USES(e->u.deref.operand); break;
    case EX_CAST: USES(e->u.cast.operand); break;
    case EX_IS: USES(e->u.ischeck.operand); break;
    case EX_ELLIPSIS: USES(e->u.ellipsis.inner); break;
    case EX_CLOSURE:
        for (size_t i = 0; e->u.closure.fn && i < e->u.closure.fn->captures.len; i++)
            if (e->u.closure.fn->captures.items[i] == sym) return true;
        break;
    default: break;
    }
#undef USES
    return false;
}

static bool stmts_use_sym_outside_loop(PtrVec stmts, const Stmt *loop,
                                       const VarSym *sym);
static bool stmt_uses_sym_outside_loop(Stmt *s, const Stmt *loop,
                                       const VarSym *sym) {
    if (!s) return false;
#define EXPR_USES(e) expr_uses_sym((e), sym)
#define CHILD(v) stmts_use_sym_outside_loop((v), loop, sym)
    if (s == loop) {
        switch (s->kind) {
        case ST_WHILE: return EXPR_USES(s->u.while_.cond);
        case ST_DO: return EXPR_USES(s->u.do_.cond);
        case ST_FOR:
            for (size_t i = 0; i < s->u.for_.init.len; i++)
                if (stmt_uses_sym_outside_loop(s->u.for_.init.items[i], loop, sym)) return true;
            for (size_t i = 0; i < s->u.for_.step.len; i++)
                if (stmt_uses_sym_outside_loop(s->u.for_.step.items[i], loop, sym)) return true;
            return EXPR_USES(s->u.for_.cond);
        case ST_FOREACH: return EXPR_USES(s->u.foreach.iter);
        default: return true;
        }
    }
    switch (s->kind) {
    case ST_EXPR: return EXPR_USES(s->u.expr.expr);
    case ST_LET: return EXPR_USES(s->u.let.init);
    case ST_RETURN: return EXPR_USES(s->u.ret.value);
    case ST_ECHO:
        for (size_t i = 0; i < s->u.echo.exprs.len; i++) if (EXPR_USES(s->u.echo.exprs.items[i])) return true;
        break;
    case ST_PRINT: return EXPR_USES(s->u.print.expr);
    case ST_IF: return EXPR_USES(s->u.if_.cond) || CHILD(s->u.if_.then) || CHILD(s->u.if_.els);
    case ST_WHILE: return EXPR_USES(s->u.while_.cond) || CHILD(s->u.while_.body);
    case ST_DO: return EXPR_USES(s->u.do_.cond) || CHILD(s->u.do_.body);
    case ST_FOR:
        for (size_t i = 0; i < s->u.for_.init.len; i++) if (stmt_uses_sym_outside_loop(s->u.for_.init.items[i], loop, sym)) return true;
        for (size_t i = 0; i < s->u.for_.step.len; i++) if (stmt_uses_sym_outside_loop(s->u.for_.step.items[i], loop, sym)) return true;
        return EXPR_USES(s->u.for_.cond) || CHILD(s->u.for_.body);
    case ST_FOREACH: return EXPR_USES(s->u.foreach.iter) || CHILD(s->u.foreach.body);
    case ST_BLOCK: return CHILD(s->u.block.stmts);
    case ST_UNSAFE_BLOCK: return CHILD(s->u.unsafe.stmts);
    case ST_TRY:
        if (CHILD(s->u.try.body) || CHILD(s->u.try.fin)) return true;
        for (size_t i = 0; i < s->u.try.catches.len; i++)
            if (CHILD(((CatchClause *)s->u.try.catches.items[i])->body)) return true;
        break;
    case ST_THROW: return EXPR_USES(s->u.throw.expr);
    case ST_SWITCH:
        if (EXPR_USES(s->u.switch_.cond)) return true;
        for (size_t i = 0; i < s->u.switch_.cases.len; i++) {
            SwitchCase *scase = s->u.switch_.cases.items[i];
            for (size_t j = 0; j < scase->patterns.len; j++) if (EXPR_USES(scase->patterns.items[j])) return true;
            if (CHILD(scase->body)) return true;
        }
        break;
    default: break;
    }
#undef CHILD
#undef EXPR_USES
    return false;
}
static bool stmts_use_sym_outside_loop(PtrVec stmts, const Stmt *loop,
                                       const VarSym *sym) {
    for (size_t i = 0; i < stmts.len; i++)
        if (stmt_uses_sym_outside_loop(stmts.items[i], loop, sym)) return true;
    return false;
}

static bool iteration_region_init(const Expr *init, const VarSym *v) {
    if (!init || !v || !v->type) return false;
    if (v->type->kind == TY_ARRAY) return scalar_array_literal(init);
    if (v->type->kind == TY_STRING)
        return init->type && init->type->kind == TY_STRING && region_string_init(init);
    return false;
}

static void infer_iteration_regions_block(AstFn *fn, Stmt *loop, PtrVec stmts) {
    for (size_t i = 0; i < stmts.len; i++) {
        Stmt *s = stmts.items[i];
        if (!s) continue;
        if (s->kind == ST_LET && s->u.let.sym) {
            VarSym *v = (VarSym *)s->u.let.sym;
            if (iteration_region_init(s->u.let.init, v) && !v->is_param &&
                !v->is_program_global && !v->is_global_alias && !v->captured_byref &&
                !v->captured && !v->captured_heap_alias && escape_stmts_safe(fn->body, v) &&
                !stmts_use_sym_outside_loop(fn->body, loop, v)) {
                v->region_local = true;
                v->region_loop = loop;
                loop->has_region_locals = true;
            }
            continue;
        }
        if (s->kind == ST_BLOCK) infer_iteration_regions_block(fn, loop, s->u.block.stmts);
        else if (s->kind == ST_IF) {
            infer_iteration_regions_block(fn, loop, s->u.if_.then);
            infer_iteration_regions_block(fn, loop, s->u.if_.els);
        } else if (s->kind == ST_UNSAFE_BLOCK)
            infer_iteration_regions_block(fn, loop, s->u.unsafe.stmts);
        else if (s->kind == ST_SWITCH) {
            for (size_t j = 0; j < s->u.switch_.cases.len; j++)
                infer_iteration_regions_block(fn, loop,
                    ((SwitchCase *)s->u.switch_.cases.items[j])->body);
        } else if (s->kind == ST_WHILE || s->kind == ST_DO ||
                   s->kind == ST_FOR || s->kind == ST_FOREACH) {
            /* Nested loops own a separate reset boundary. */
            infer_iteration_regions_block(fn, s, (s->kind == ST_WHILE) ? s->u.while_.body :
                (s->kind == ST_DO) ? s->u.do_.body :
                (s->kind == ST_FOR) ? s->u.for_.body : s->u.foreach.body);
        }
    }
}

static void infer_local_regions_block(AstFn *fn, PtrVec stmts) {
    for (size_t j = 0; j < stmts.len; j++) {
        Stmt *s = stmts.items[j];
        if (!s) continue;
        if (s->kind == ST_LET && s->u.let.sym &&
            scalar_array_literal(s->u.let.init)) {
            VarSym *v = (VarSym *)s->u.let.sym;
            if (v->type && v->type->kind == TY_ARRAY && !v->is_param &&
                !v->is_program_global && !v->is_global_alias &&
                !v->captured_byref && !v->captured && !v->captured_heap_alias &&
                escape_stmts_safe(fn->body, v))
                v->region_local = true;
            continue;
        }
        /* Loop values use a reusable iteration region instead of the
         * function-lifetime region used by ordinary local candidates. */
        if (s->kind == ST_BLOCK) {
            infer_local_regions_block(fn, s->u.block.stmts);
        } else if (s->kind == ST_IF) {
            infer_local_regions_block(fn, s->u.if_.then);
            infer_local_regions_block(fn, s->u.if_.els);
        } else if (s->kind == ST_UNSAFE_BLOCK) {
            infer_local_regions_block(fn, s->u.unsafe.stmts);
        } else if (s->kind == ST_SWITCH) {
            for (size_t k = 0; k < s->u.switch_.cases.len; k++)
                infer_local_regions_block(fn, ((SwitchCase *)s->u.switch_.cases.items[k])->body);
        } else if (s->kind == ST_WHILE) {
            infer_iteration_regions_block(fn, s, s->u.while_.body);
        } else if (s->kind == ST_DO) {
            infer_iteration_regions_block(fn, s, s->u.do_.body);
        } else if (s->kind == ST_FOR) {
            infer_iteration_regions_block(fn, s, s->u.for_.body);
        } else if (s->kind == ST_FOREACH) {
            infer_iteration_regions_block(fn, s, s->u.foreach.body);
        }
    }
}

static void infer_local_regions(Program *prog) {
    PtrVec functions = {0};
    for (size_t i = 0; i < prog->decls.len; i++) {
        Decl *d = prog->decls.items[i];
        if (d->kind == DK_FN) ptrvec_push(&functions, d->u.fn);
        else if (d->kind == DK_CLASS || d->kind == DK_TRAIT || d->kind == DK_INTERFACE) {
            AstClass *cls = d->u.cls;
            for (size_t j = 0; j < cls->methods.len; j++)
                ptrvec_push(&functions, cls->methods.items[j]);
        }
    }
    for (size_t i = 0; i < functions.len; i++) {
        AstFn *fn = functions.items[i];
        if (!fn || !fn->enforce_memory_safety || fn->is_closure || fn->is_abstract)
            continue;
        infer_local_regions_block(fn, fn->body);
    }
    free(functions.items);
}

void sema_run(Program *prog) {
    builtins_init();
    sema_ok = true;
    g_borrow_fns.len = 0;
    s_this = intern("this");
    s_argc = intern("argc");
    s_argv = intern("argv");
    declare_decls(prog);

    /* classes */
    for (size_t i = 0; i < prog->decls.len; i++) {
        Decl *d = prog->decls.items[i];
        if (d->kind == DK_CLASS || d->kind == DK_INTERFACE || d->kind == DK_TRAIT)
            check_class(d->u.cls);
    }
    /* free functions */
    for (size_t i = 0; i < prog->decls.len; i++) {
        Decl *d = prog->decls.items[i];
        if (d->kind == DK_FN)
            check_fn_body(NULL, d->u.fn, NULL);
    }
    /* top-level statements (the script part) */
    Scope sc;
    scope_init(&sc, NULL);
    sc.enforce_memory_safety = prog->enforce_memory_safety;
    check_block(&sc, prog->stmts);
    for (size_t pass = 0; pass <= g_borrow_fns.len; pass++) {
        bool changed = resolve_ref_calls_seq(prog->stmts);
        for (size_t i = 0; i < g_borrow_fns.len; i++) {
            AstFn *fn = g_borrow_fns.items[i];
            if (resolve_ref_calls_seq(fn->body)) changed = true;
        }
        if (!changed) break;
    }
    validate_virtual_ref_return_contracts();
    for (size_t i = 0; i < g_borrow_fns.len; i++) {
        AstFn *fn = g_borrow_fns.items[i];
        check_deferred_ref_returns(fn, fn->body);
    }
    diagnose_unresolved_ref_returns(prog->stmts);
    for (size_t i = 0; i < g_borrow_fns.len; i++)
        diagnose_unresolved_ref_returns(((AstFn *)g_borrow_fns.items[i])->body);
    borrow_infer_summaries(g_borrow_fns);
    borrow_check_script(prog->stmts);
    for (size_t i = 0; i < g_borrow_fns.len; i++)
        borrow_check_function(g_borrow_fns.items[i]);
    infer_borrow_only_params(prog);
    infer_local_regions(prog);
}

const FnSig *sema_fn_sig(AstFn *fn) { return fn_make_sig(fn); }
Type *sema_typeof(Expr *e) { return (Type *)e->type; }

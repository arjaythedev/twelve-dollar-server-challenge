/* borrow.c — resolved control-flow borrow checking.
 *
 * Sema first resolves every variable occurrence to its VarSym. This pass
 * then builds a conservative CFG and solves liveness with a backwards fixed
 * point. A loan held by a reference binding is active precisely at nodes
 * where that binding is live. */
#include "sema.h"
#include "diag.h"
#include "util.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef struct BorrowEvent {
    const VarSym *origin;
    const Expr *place;
    const VarSym *holder; /* NULL for a full-expression temporary */
    const VarSym *parent;
    bool is_mut;
    bool slot_only; /* by-ref call borrows a scalar binding, not its heap value */
    size_t flow_index;
    Token tok;
    PtrVec alternatives; /* copied exclusive-branch choices */
} BorrowEvent;

typedef struct BorrowAlternative {
    size_t group;
    size_t arm;
} BorrowAlternative;

typedef struct BorrowFlowEvent {
    const VarSym *var;
    const Expr *place; /* NULL means the entire root binding */
    Token tok;
    enum { FLOW_USE, FLOW_MOVE, FLOW_INIT } kind;
    bool reborrow_source;
    bool borrow_source;
    PtrVec alternatives;
} BorrowFlowEvent;

typedef struct MovePlace {
    const VarSym *root;
    const Expr *place;
    Token tok;
    int next_root;
} MovePlace;

typedef struct BorrowCfg BorrowCfg;

typedef struct BorrowWrite {
    const VarSym *origin;
    const Expr *place;
    const VarSym *through; /* mutref holder authorizing this write, if any */
    const Type *object_type;
    bool may_alias;
    bool closure_escape_arg;
    bool unknown_global;
    bool unknown_object;
    size_t flow_index;
    bool shared_receiver_violation;
    Token call_tok;
    PtrVec alternatives;
} BorrowWrite;

typedef struct BorrowNode {
    BorrowCfg *cfg;
    Stmt *stmt;
    PtrVec succ;       /* encoded node indexes */
    PtrVec pred;       /* encoded node indexes */
    PtrVec uses;       /* VarSym* */
    PtrVec defs;       /* VarSym* */
    PtrVec flow;       /* ordered uses and moves within this statement */
    PtrVec writes;     /* BorrowWrite* */
    PtrVec borrows;    /* BorrowEvent* */
} BorrowNode;

struct BorrowCfg {
    PtrVec nodes;      /* BorrowNode* */
    PtrVec active_alternatives; /* BorrowAlternative* during expression walk */
    PtrVec synthetic_places; /* composed projections owned by this CFG */
    size_t next_alternative;
    int exit;
    bool failed;
};

static PtrVec g_borrow_functions;
static Type borrow_mixed_type = {.kind = TY_MIXED};
static const Type *heap_object_base(const Type *t);
static const VarSym *root_var(const Expr *e);
static void write_unknown_heap_escape(BorrowNode *n, const Type *object_type);
static void write_unknown_heap_object(BorrowNode *n, const Type *object_type,
                                      const Expr *place);
static bool class_is_subclass(AstClass *cls, const AstClass *base);
static bool class_implements(AstClass *cls, const char *iface);

static bool expr_uses_shared_ref(const Expr *e) {
    if (!e) return false;
    if (e->type && e->type->kind == TY_REF) return true;
    if (e->borrow_origin) {
        const VarSym *origin = e->borrow_origin;
        if (origin->shared_alias ||
            (origin->type && origin->type->kind == TY_REF)) return true;
    }
    switch (e->kind) {
    case EX_VAR: return e->u.var.sym && ((VarSym *)e->u.var.sym)->shared_alias;
    case EX_FIELD: return expr_uses_shared_ref(e->u.field.obj);
    case EX_INDEX: return expr_uses_shared_ref(e->u.index.obj);
    case EX_DEREF: return expr_uses_shared_ref(e->u.deref.operand);
    case EX_ASSIGN: return expr_uses_shared_ref(e->u.assign.value);
    case EX_BIN: return expr_uses_shared_ref(e->u.bin.l) ||
                        expr_uses_shared_ref(e->u.bin.r);
    case EX_TERNARY: return expr_uses_shared_ref(e->u.tern.then) ||
                             expr_uses_shared_ref(e->u.tern.els);
    case EX_MATCH:
        for (size_t i = 0; i < e->u.matchexpr.cases.len; i++)
            if (expr_uses_shared_ref(((MatchCase *)e->u.matchexpr.cases.items[i])->body))
                return true;
        return expr_uses_shared_ref(e->u.matchexpr.dflt);
    case EX_ARRAY_LIT:
        for (size_t i = 0; i < e->u.arr.elems.len; i++)
            if (expr_uses_shared_ref(e->u.arr.elems.items[i])) return true;
        return false;
    case EX_MAP_LIT:
        for (size_t i = 0; i < e->u.map.vals.len; i++)
            if (expr_uses_shared_ref(e->u.map.keys.items[i]) ||
                expr_uses_shared_ref(e->u.map.vals.items[i])) return true;
        return false;
    case EX_TUPLE_LIT:
        for (size_t i = 0; i < e->u.tuple.elems.len; i++)
            if (expr_uses_shared_ref(e->u.tuple.elems.items[i])) return true;
        return false;
    case EX_CAST: return expr_uses_shared_ref(e->u.cast.operand);
    default: return false;
    }
}

typedef struct LoopTargets {
    int *breaks;
    int *continues;
    size_t nbreaks, ncontinues;
} LoopTargets;

static LoopTargets targets_clone(const LoopTargets *src) {
    LoopTargets out = {0};
    if (!src) return out;
    if (src->nbreaks) {
        out.breaks = xmalloc(src->nbreaks * sizeof(int));
        memcpy(out.breaks, src->breaks, src->nbreaks * sizeof(int));
        out.nbreaks = src->nbreaks;
    }
    if (src->ncontinues) {
        out.continues = xmalloc(src->ncontinues * sizeof(int));
        memcpy(out.continues, src->continues, src->ncontinues * sizeof(int));
        out.ncontinues = src->ncontinues;
    }
    return out;
}

static void targets_push_break(LoopTargets *targets, int id) {
    targets->breaks = xrealloc(targets->breaks, (targets->nbreaks + 1) * sizeof(int));
    targets->breaks[targets->nbreaks++] = id;
}

static void targets_push_continue(LoopTargets *targets, int id) {
    targets->continues = xrealloc(targets->continues, (targets->ncontinues + 1) * sizeof(int));
    targets->continues[targets->ncontinues++] = id;
}

static void targets_dispose(LoopTargets *targets) {
    free(targets->breaks);
    free(targets->continues);
    memset(targets, 0, sizeof(*targets));
}

static void add_unique(PtrVec *v, void *p) {
    if (!p) return;
    for (size_t i = 0; i < v->len; i++)
        if (v->items[i] == p) return;
    ptrvec_push(v, p);
}

/* `global $x` creates a scope-local VarSym view with the same uid as the
 * program-wide slot. Borrow liveness is indexed by uid, so keep one holder
 * per slot identity here instead of admitting duplicate symbol views. */
static void add_unique_holder(PtrVec *holders, VarSym *candidate) {
    if (!holders || !candidate) return;
    for (size_t i = 0; i < holders->len; i++) {
        VarSym *existing = holders->items[i];
        if (existing == candidate ||
            (candidate->uid >= 0 && existing->uid == candidate->uid)) return;
    }
    ptrvec_push(holders, candidate);
}

static void add_indexed_holder(PtrVec *holders, VarSym *candidate,
                               int *holder_by_uid, size_t uid_count) {
    if (!candidate) return;
    if (candidate->uid < 0 || (size_t)candidate->uid >= uid_count) {
        add_unique_holder(holders, candidate);
        return;
    }
    if (holder_by_uid[candidate->uid] >= 0) return;
    holder_by_uid[candidate->uid] = (int)holders->len;
    ptrvec_push(holders, candidate);
}

static void flow_event(BorrowCfg *cfg, BorrowNode *n, const VarSym *v,
                       const Expr *place, const Token *tok, int kind) {
    if (!v) return;
    BorrowFlowEvent *event = xcalloc(1, sizeof(*event));
    event->var = v;
    event->place = place;
    if (tok) event->tok = *tok;
    event->kind = kind;
    for (size_t i = 0; cfg && i < cfg->active_alternatives.len; i++) {
        BorrowAlternative *copy = xmalloc(sizeof(*copy));
        *copy = *(BorrowAlternative *)cfg->active_alternatives.items[i];
        ptrvec_push(&event->alternatives, copy);
    }
    ptrvec_push(&n->flow, event);
}

static int node_new(BorrowCfg *cfg, Stmt *stmt) {
    BorrowNode *n = xcalloc(1, sizeof(*n));
    n->cfg = cfg;
    n->stmt = stmt;
    ptrvec_push(&cfg->nodes, n);
    return (int)cfg->nodes.len - 1;
}

static BorrowNode *node_at(BorrowCfg *cfg, int i) {
    return (BorrowNode *)cfg->nodes.items[i];
}

static void edge(BorrowCfg *cfg, int from, int to) {
    if (from < 0 || to < 0) return;
    add_unique(&node_at(cfg, from)->succ, (void *)(intptr_t)(to + 1));
    add_unique(&node_at(cfg, to)->pred, (void *)(intptr_t)(from + 1));
}

static void clear_successors(BorrowCfg *cfg, int from) {
    if (!cfg || from < 0) return;
    BorrowNode *n = node_at(cfg, from);
    for (size_t i = 0; i < n->succ.len; i++) {
        int to = (int)(intptr_t)n->succ.items[i] - 1;
        if (to < 0) continue;
        BorrowNode *target = node_at(cfg, to);
        intptr_t encoded = (intptr_t)from + 1;
        for (size_t p = 0; p < target->pred.len; p++) {
            if ((intptr_t)target->pred.items[p] != encoded) continue;
            memmove(&target->pred.items[p], &target->pred.items[p + 1],
                    (target->pred.len - p - 1) * sizeof(void *));
            target->pred.len--;
            break;
        }
    }
    n->succ.len = 0;
}

static void use_var(BorrowNode *n, const VarSym *v) {
    add_unique(&n->uses, (void *)v);
}

static void def_var(BorrowNode *n, const VarSym *v) {
    add_unique(&n->defs, (void *)v);
}

static void init_var(BorrowCfg *cfg, BorrowNode *n, const VarSym *v,
                     const Expr *place, const Token *tok) {
    flow_event(cfg, n, v, place, tok, FLOW_INIT);
}

static void write_push(BorrowNode *n, BorrowWrite *w);

static void write_var(BorrowNode *n, const VarSym *v) {
    if (!v) return;
    BorrowWrite *w = xcalloc(1, sizeof(*w));
    w->origin = v;
    w->place = NULL;
    w->through = NULL;
    w->object_type = NULL;
    w->may_alias = false;
    w->closure_escape_arg = false;
    w->unknown_global = false;
    w->unknown_object = false;
    w->flow_index = n->flow.len;
    w->shared_receiver_violation = false;
    memset(&w->call_tok, 0, sizeof(w->call_tok));
    write_push(n, w);
    def_var(n, v);
}

static bool global_storage(const VarSym *v) {
    return v && (v->is_program_global || v->is_global_alias);
}

static bool same_global(const VarSym *a, const VarSym *b) {
    return a == b || (global_storage(a) && global_storage(b) && a->uid == b->uid);
}

static bool alternatives_same(const PtrVec *a, const PtrVec *b) {
    if (!a || !b || a->len != b->len) return false;
    for (size_t i = 0; i < a->len; i++) {
        BorrowAlternative *aa = a->items[i];
        BorrowAlternative *bb = b->items[i];
        if (aa->group != bb->group || aa->arm != bb->arm) return false;
    }
    return true;
}

static void write_push(BorrowNode *n, BorrowWrite *w) {
    if (!n || !w) return;
    PtrVec *active = n->cfg ? &n->cfg->active_alternatives : NULL;
    for (size_t i = 0; active && i < active->len; i++) {
        BorrowAlternative *copy = xmalloc(sizeof(*copy));
        *copy = *(BorrowAlternative *)active->items[i];
        ptrvec_push(&w->alternatives, copy);
    }
    ptrvec_push(&n->writes, w);
}

static void write_global_effect(BorrowNode *n, const VarSym *global) {
    if (!n || !global) return;
    for (size_t i = 0; i < n->writes.len; i++) {
        BorrowWrite *prior = n->writes.items[i];
        if (!prior->unknown_global && same_global(prior->origin, global) &&
            alternatives_same(&prior->alternatives, &n->cfg->active_alternatives)) return;
    }
    BorrowWrite *w = xcalloc(1, sizeof(*w));
    w->origin = global;
    w->place = NULL;
    w->through = NULL;
    w->object_type = global->type;
    w->may_alias = true;
    w->closure_escape_arg = false;
    w->unknown_global = false;
    w->unknown_object = false;
    w->flow_index = n->flow.len;
    w->shared_receiver_violation = false;
    memset(&w->call_tok, 0, sizeof(w->call_tok));
    write_push(n, w);
}

static void write_global_escape_effect(BorrowNode *n, const VarSym *global) {
    if (!n || !global) return;
    write_global_effect(n, global);
    for (size_t i = 0; i < n->writes.len; i++) {
        BorrowWrite *w = n->writes.items[i];
        if (!same_global(w->origin, global) ||
            !alternatives_same(&w->alternatives, &n->cfg->active_alternatives)) continue;
        w->closure_escape_arg = true;
        w->may_alias = true;
        return;
    }
}

static void write_unknown_global_effect(BorrowNode *n) {
    if (!n) return;
    for (size_t i = 0; i < n->writes.len; i++) {
        BorrowWrite *prior = n->writes.items[i];
        if (prior->unknown_global &&
            alternatives_same(&prior->alternatives, &n->cfg->active_alternatives)) return;
    }
    BorrowWrite *w = xcalloc(1, sizeof(*w));
    w->unknown_global = true;
    w->flow_index = n->flow.len;
    write_push(n, w);
}

static void collect_global_effects(BorrowNode *n, AstFn *target) {
    if (!n || !target) return;
    for (size_t i = 0; i < target->borrow_global_writes.len; i++)
        write_global_effect(n, target->borrow_global_writes.items[i]);
    for (size_t i = 0; i < target->borrow_global_escapes.len; i++)
        write_global_escape_effect(n, target->borrow_global_escapes.items[i]);
    for (size_t i = 0; i < target->borrow_heap_escapes.len; i++)
        write_unknown_heap_escape(n, target->borrow_heap_escapes.items[i]);
    if (target->borrow_unknown_global_write || !target->body.len)
        write_unknown_global_effect(n);
}

static bool method_builtin_mutates_receiver(const Expr *call) {
    if (!call || !call->builtin) return true;
    const char *name = call->builtin;
    return strcmp(name, "count") != 0 &&
           strcmp(name, "array_key_exists") != 0 &&
           strcmp(name, "array_get") != 0 &&
           strcmp(name, "array_keys") != 0 &&
           strcmp(name, "array_values") != 0 &&
           strcmp(name, "exception_get_message") != 0;
}

static bool collect_method_effects(BorrowNode *n, Expr *call) {
    if (!n || !call) return true;
    AstFn *target = call ? call->u.method.target : NULL;
    if (call->builtin) return method_builtin_mutates_receiver(call);
    /* Callable fields are dynamic closure calls, not virtual method dispatch.
     * A same-named method on a subclass must not make their effects look
     * known: the closure may mutate globals or retain/mutate any alias. */
    if (!target && call->u.method.sf) {
        write_unknown_global_effect(n);
        write_unknown_heap_object(n, &borrow_mixed_type, NULL);
        return true;
    }
    bool mutates = !target || target->borrow_mutates_receiver;
    const Type *recv = heap_object_base(call->u.method.obj
        ? call->u.method.obj->type : NULL);
    AstClass *static_cls = recv ? (AstClass *)recv->decl : NULL;
    if (!static_cls || (recv->kind != TY_CLASS && recv->kind != TY_INTERFACE)) {
        collect_global_effects(n, target);
        write_unknown_global_effect(n);
        return true;
    }

    bool saw_implementation = target && target->body.len && !target->is_abstract;
    bool target_effects_collected = false;
    if (saw_implementation) {
        collect_global_effects(n, target);
        target_effects_collected = true;
    }
    for (size_t i = 0; i < g_borrow_functions.len; i++) {
        AstFn *candidate = g_borrow_functions.items[i];
        if (!candidate || !candidate->is_method || candidate->is_static ||
            strcmp(candidate->name, call->u.method.name) != 0 || !candidate->cls)
            continue;
        bool dispatchable = class_is_subclass(candidate->cls, static_cls) ||
            (recv->kind == TY_INTERFACE && class_implements(candidate->cls, static_cls->name));
        if (!dispatchable || (candidate->is_abstract && !candidate->body.len)) continue;
        saw_implementation = true;
        if (candidate != target || !target_effects_collected) {
            collect_global_effects(n, candidate);
            if (candidate == target) target_effects_collected = true;
        }
        if (!candidate->body.len) {
            mutates = true;
            write_unknown_heap_object(n, recv, NULL);
        } else if (candidate->borrow_mutates_receiver) {
            mutates = true;
        }
    }
    if (!saw_implementation) {
        collect_global_effects(n, target);
        write_unknown_global_effect(n);
        write_unknown_heap_object(n, recv, NULL);
    }
    return mutates || !saw_implementation;
}

static const Expr *compose_borrow_place(BorrowCfg *cfg, const Expr *base,
                                        const Expr *relative) {
    if (!relative || relative->kind == EX_VAR) return base;
    const Expr *relative_base = relative->kind == EX_FIELD ? relative->u.field.obj :
                                relative->kind == EX_INDEX ? relative->u.index.obj : NULL;
    if (!relative_base) return base;
    const Expr *composed_base = compose_borrow_place(cfg, base, relative_base);
    Expr *composed = xmalloc(sizeof(*composed));
    *composed = *relative;
    if (relative->kind == EX_FIELD) composed->u.field.obj = (Expr *)composed_base;
    else composed->u.index.obj = (Expr *)composed_base;
    ptrvec_push(&cfg->synthetic_places, composed);
    return composed;
}

static void write_through(BorrowCfg *cfg, BorrowNode *n, const VarSym *holder,
                          const Expr *relative_place) {
    if (!holder) return;
    const VarSym *origin = holder->borrow_origin ? holder->borrow_origin :
                           (holder->is_param && holder->type &&
                            holder->type->kind == TY_MUTREF ? holder : NULL);
    if (!origin) return;
    BorrowWrite *w = xcalloc(1, sizeof(*w));
    w->origin = origin;
    w->place = root_var(relative_place) == holder
        ? (holder->borrow_place
            ? compose_borrow_place(cfg, holder->borrow_place, relative_place)
            : relative_place)
        : holder->borrow_place;
    w->through = holder;
    w->object_type = holder->type &&
        (holder->type->kind == TY_REF || holder->type->kind == TY_MUTREF)
            ? holder->type->elem : origin->type;
    w->may_alias = origin->is_param || origin->alias_unknown;
    w->closure_escape_arg = false;
    w->unknown_global = false;
    w->unknown_object = false;
    w->flow_index = n->flow.len;
    w->shared_receiver_violation = false;
    memset(&w->call_tok, 0, sizeof(w->call_tok));
    write_push(n, w);
}

static bool heap_types_overlap(const Type *a, const Type *b);

static const Type *heap_object_base(const Type *t) {
    while (t && (t->kind == TY_REF || t->kind == TY_MUTREF || t->kind == TY_RC))
        t = t->elem;
    return t && (t->kind == TY_CLASS || t->kind == TY_STRUCT ||
                 t->kind == TY_INTERFACE) ? t : NULL;
}

static bool class_is_subclass(AstClass *cls, const AstClass *base) {
    for (AstClass *c = cls; c; c = c->base) if (c == base) return true;
    return false;
}

static bool class_implements(AstClass *cls, const char *iface) {
    for (AstClass *c = cls; c; c = c->base)
        for (size_t i = 0; i < c->interfaces.len; i++)
            if (strcmp(c->interfaces.items[i], iface) == 0) return true;
    return false;
}

static bool heap_types_overlap(const Type *a, const Type *b) {
    a = heap_object_base(a);
    b = heap_object_base(b);
    if (!a || !b) return false;
    /* A concrete class may implement both interfaces; without a whole-program
     * implementor set, distinct interface names do not prove disjointness. */
    if (a->kind == TY_INTERFACE && b->kind == TY_INTERFACE) return true;
    if (a->kind == b->kind && a->name == b->name) return true;
    AstClass *ac = (AstClass *)a->decl;
    AstClass *bc = (AstClass *)b->decl;
    if (ac && bc && (class_is_subclass(ac, bc) || class_is_subclass(bc, ac)))
        return true;
    if (ac && b->kind == TY_INTERFACE && class_implements(ac, b->name)) return true;
    if (bc && a->kind == TY_INTERFACE && class_implements(bc, a->name)) return true;
    return false;
}

typedef struct TypePair {
    const Type *a, *b;
} TypePair;

static bool heap_types_related_walk(const Type *a, const Type *b,
                                   TypePair *seen, size_t *nseen, int depth);

static bool heap_type_children_related(const Type *t, const Type *other,
                                       TypePair *seen, size_t *nseen, int depth) {
    if (!t) return false;
    const Type *base = heap_object_base(t);
    if (base) {
        AstClass *cls = (AstClass *)base->decl;
        for (AstClass *c = cls; c; c = c->base)
            for (size_t i = 0; i < c->fields.len; i++) {
                StructField *field = c->fields.items[i];
                if (heap_types_related_walk(field->type, other, seen, nseen, depth + 1))
                    return true;
            }
    }
    switch (t->kind) {
    case TY_REF: case TY_MUTREF: case TY_PTR: case TY_OWN: case TY_RC:
    case TY_OPTION: case TY_NULLABLE:
    case TY_ARRAY: case TY_VEC: case TY_SET:
        return heap_types_related_walk(t->elem, other, seen, nseen, depth + 1);
    case TY_MAP:
        return heap_types_related_walk(t->key, other, seen, nseen, depth + 1) ||
               heap_types_related_walk(t->val, other, seen, nseen, depth + 1);
    case TY_RESULT:
        return heap_types_related_walk(t->ok, other, seen, nseen, depth + 1) ||
               heap_types_related_walk(t->err, other, seen, nseen, depth + 1);
    case TY_TUPLE: case TY_UNION:
        for (size_t i = 0; i < t->nmembers; i++)
            if (heap_types_related_walk(t->members[i], other, seen, nseen, depth + 1))
                return true;
        return false;
    case TY_FN:
        return true; /* opaque closure environment may retain heap aliases */
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

static bool heap_types_related_walk(const Type *a, const Type *b,
                                   TypePair *seen, size_t *nseen, int depth) {
    if (!a || !b) return false;
    if (depth > 64) return true; /* fail closed for unusually deep type graphs */
    if (a->kind == TY_MIXED || b->kind == TY_MIXED ||
        a->kind == TY_FN || b->kind == TY_FN) return true;
    /* All collection kinds share the harr* runtime representation. With RC
     * hooks currently absent, two copyable bindings of compatible collection
     * types may still point at the same mutable allocation. */
    if (same_mutable_storage_family(a->kind, b->kind)) return true;
    if (heap_types_overlap(a, b)) return true;
    for (size_t i = 0; i < *nseen; i++)
        if ((seen[i].a == a && seen[i].b == b) ||
            (seen[i].a == b && seen[i].b == a)) return false;
    if (*nseen >= 128) return true;
    seen[(*nseen)++] = (TypePair){a, b};
    if (heap_type_children_related(a, b, seen, nseen, depth) ||
        heap_type_children_related(b, a, seen, nseen, depth)) {
        (*nseen)--;
        return true;
    }
    (*nseen)--;
    return false;
}

static bool heap_types_related(const Type *a, const Type *b) {
    TypePair seen[128];
    size_t nseen = 0;
    return heap_types_related_walk(a, b, seen, &nseen, 0);
}

static bool scalar_ref_origins_may_alias(const VarSym *a, const VarSym *b) {
    if (!a || !b) return false;
    bool a_ref_param = a->is_param && (a->is_param_ref ||
        (a->type && (a->type->kind == TY_REF || a->type->kind == TY_MUTREF)));
    bool b_ref_param = b->is_param && (b->is_param_ref ||
        (b->type && (b->type->kind == TY_REF || b->type->kind == TY_MUTREF)));
    if (!((a_ref_param && (b_ref_param || global_storage(b))) ||
          (b_ref_param && global_storage(a)))) return false;
    const Type *at = a->type;
    const Type *bt = b->type;
    if (at && (at->kind == TY_REF || at->kind == TY_MUTREF)) at = at->elem;
    else if (!a->is_param_ref) return false;
    if (bt && (bt->kind == TY_REF || bt->kind == TY_MUTREF)) bt = bt->elem;
    else if (!b->is_param_ref) return false;
    if (!at || !bt || at->kind == TY_MIXED || bt->kind == TY_MIXED) return true;
    if (at->kind != bt->kind) return false;
    if (at->kind == TY_CLASS || at->kind == TY_STRUCT || at->kind == TY_INTERFACE)
        return heap_types_related(at, bt);
    return !at->name || !bt->name || at->name == bt->name;
}

static bool borrow_origins_may_alias(const VarSym *a, const VarSym *b) {
    if (!a || !b) return false;
    if (same_global(a, b)) return true;
    const VarSym *ai = a->alias_origin ? a->alias_origin : a;
    const VarSym *bi = b->alias_origin ? b->alias_origin : b;
    if (same_global(ai, bi)) return true;
    if ((a->type && a->type->kind == TY_MIXED) ||
        (b->type && b->type->kind == TY_MIXED))
        return true; /* an opaque hval may contain the same heap object */
    if (heap_types_related(a->type, b->type) &&
        (a->alias_unknown || b->alias_unknown || a->is_param || b->is_param ||
         ai->is_param || bi->is_param))
        return true;
    return scalar_ref_origins_may_alias(a, b);
}

static void write_object(BorrowNode *n, const VarSym *root, const Type *object_type,
                         const Expr *place) {
    if (!root) return;
    const VarSym *origin = root->alias_origin ? root->alias_origin : root;
    BorrowWrite *w = xcalloc(1, sizeof(*w));
    w->origin = origin;
    w->place = place;
    w->through = NULL;
    w->object_type = object_type ? object_type : root->type;
    w->may_alias = root->alias_unknown || origin->is_param || origin->alias_unknown ||
                   (object_type && !heap_types_overlap(root->type, object_type));
    w->closure_escape_arg = false;
    w->unknown_global = false;
    w->unknown_object = false;
    w->flow_index = n->flow.len;
    w->shared_receiver_violation = false;
    memset(&w->call_tok, 0, sizeof(w->call_tok));
    write_push(n, w);
}

static void write_unknown_heap_object(BorrowNode *n, const Type *object_type,
                                      const Expr *place) {
    if (!n || !object_type) return;
    BorrowWrite *w = xcalloc(1, sizeof(*w));
    w->place = place;
    w->object_type = object_type;
    w->may_alias = true;
    w->unknown_object = true;
    w->flow_index = n->flow.len;
    write_push(n, w);
}

static void write_unknown_heap_escape(BorrowNode *n, const Type *object_type) {
    if (!n || !object_type) return;
    for (size_t i = 0; i < n->writes.len; i++) {
        BorrowWrite *prior = n->writes.items[i];
        if (prior->unknown_object && prior->closure_escape_arg &&
            prior->object_type == object_type &&
            alternatives_same(&prior->alternatives, &n->cfg->active_alternatives)) return;
    }
    write_unknown_heap_object(n, object_type, NULL);
    BorrowWrite *w = n->writes.items[n->writes.len - 1];
    w->closure_escape_arg = true;
}

static const VarSym *root_var(const Expr *e) {
    if (!e) return NULL;
    switch (e->kind) {
    case EX_VAR: return (const VarSym *)e->u.var.sym;
    case EX_FIELD: return root_var(e->u.field.obj);
    case EX_INDEX: return root_var(e->u.index.obj);
    case EX_DEREF: return root_var(e->u.deref.operand);
    default: return NULL;
    }
}

static void event_add(BorrowCfg *cfg, BorrowNode *n, const VarSym *origin,
                      const VarSym *holder, const Expr *place, bool is_mut, Token tok) {
    if (!origin) return;
    BorrowEvent *e = xcalloc(1, sizeof(*e));
    e->origin = origin;
    e->place = place;
    e->holder = holder;
    e->parent = NULL;
    e->is_mut = is_mut;
    e->flow_index = n->flow.len;
    e->tok = tok;
    if (cfg) for (size_t i = 0; i < cfg->active_alternatives.len; i++) {
        BorrowAlternative *copy = xmalloc(sizeof(*copy));
        *copy = *(BorrowAlternative *)cfg->active_alternatives.items[i];
        ptrvec_push(&e->alternatives, copy);
    }
    ptrvec_push(&n->borrows, e);
}

static void alternative_push(BorrowCfg *cfg, size_t group, size_t arm) {
    BorrowAlternative *alt = xmalloc(sizeof(*alt));
    alt->group = group;
    alt->arm = arm;
    ptrvec_push(&cfg->active_alternatives, alt);
}

static void alternative_pop(BorrowCfg *cfg) {
    if (!cfg || !cfg->active_alternatives.len) return;
    free(cfg->active_alternatives.items[--cfg->active_alternatives.len]);
}

static bool alternatives_exclusive(const BorrowEvent *a, const BorrowEvent *b) {
    if (!a || !b) return false;
    for (size_t i = 0; i < a->alternatives.len; i++) {
        BorrowAlternative *aa = a->alternatives.items[i];
        for (size_t j = 0; j < b->alternatives.len; j++) {
            BorrowAlternative *bb = b->alternatives.items[j];
            if (aa->group == bb->group && aa->arm != bb->arm) return true;
        }
    }
    return false;
}

static bool borrow_write_exclusive(const BorrowEvent *borrow,
                                   const BorrowWrite *write) {
    if (!borrow || !write) return false;
    for (size_t i = 0; i < borrow->alternatives.len; i++) {
        BorrowAlternative *ba = borrow->alternatives.items[i];
        for (size_t j = 0; j < write->alternatives.len; j++) {
            BorrowAlternative *wa = write->alternatives.items[j];
            if (ba->group == wa->group && ba->arm != wa->arm) return true;
        }
    }
    return false;
}

static bool borrow_flow_exclusive(const BorrowEvent *borrow,
                                  const BorrowFlowEvent *flow) {
    if (!borrow || !flow) return false;
    for (size_t i = 0; i < borrow->alternatives.len; i++) {
        BorrowAlternative *ba = borrow->alternatives.items[i];
        for (size_t j = 0; j < flow->alternatives.len; j++) {
            BorrowAlternative *fa = flow->alternatives.items[j];
            if (ba->group == fa->group && ba->arm != fa->arm) return true;
        }
    }
    return false;
}

static void borrow_note(const Token *tok, const char *message) {
    if (!tok || !tok->file || !tok->line) return;
    diag_at("note", tok->file, tok->line, tok->col, tok->len, message);
}

static bool builtin_mutates_first(const char *name) {
    static const char *names[] = {
        "sort", "rsort", "asort", "arsort", "ksort", "krsort",
        "usort", "uasort", "uksort", "shuffle", "array_pop",
        "array_push", "array_shift", "array_unshift", "array_splice",
        "array_walk", "array_walk_recursive", "unset", NULL
    };
    for (size_t i = 0; names[i]; i++)
        if (name && strcmp(name, names[i]) == 0) return true;
    return false;
}

static void collect_expr(BorrowCfg *cfg, BorrowNode *n, Expr *e,
                         const VarSym *holder);

static void collect_ref_call_result(BorrowCfg *cfg, BorrowNode *n, Expr *e,
                                    PtrVec args, const VarSym *holder) {
    if (!holder || !e || !e->borrow_origin || !e->type ||
        (e->type->kind != TY_REF && e->type->kind != TY_MUTREF)) return;
    Expr *borrow_arg = e->has_borrow_param && e->borrow_param_index >= 0 &&
        (size_t)e->borrow_param_index < args.len
            ? args.items[e->borrow_param_index] : NULL;
    bool global_result = global_storage(e->borrow_origin);
    if (!borrow_arg && !global_result && !e->has_borrow_receiver) return;
    event_add(cfg, n, e->borrow_origin, holder, e->borrow_place,
              e->type->kind == TY_MUTREF, e->tok);
    if (n->borrows.len)
        ((BorrowEvent *)n->borrows.items[n->borrows.len - 1])->parent = e->borrow_parent;
}

/* Evaluate the dynamic keys needed to address an assignment place without
 * reading the slot being overwritten. In particular, assigning to a moved
 * array element is a reinitialization, not a read of that element. */
static void collect_place_indices(BorrowCfg *cfg, BorrowNode *n, Expr *place) {
    if (!place) return;
    if (place->kind == EX_FIELD) {
        collect_place_indices(cfg, n, place->u.field.obj);
    } else if (place->kind == EX_INDEX) {
        collect_place_indices(cfg, n, place->u.index.obj);
        collect_expr(cfg, n, place->u.index.idx, NULL);
    } else if (place->kind == EX_DEREF) {
        collect_expr(cfg, n, place->u.deref.operand, NULL);
    } else if (place->kind != EX_VAR) {
        collect_expr(cfg, n, place, NULL);
    }
}

static void collect_call_args(BorrowCfg *cfg, BorrowNode *n, PtrVec args,
                              AstFn *target, const char *name,
                              int return_borrow_param, const VarSym *return_holder,
                              bool may_escape_heap_args) {
    for (size_t i = 0; i < args.len; i++) {
        Expr *arg = args.items[i];
        size_t flow_start = n->flow.len;
        collect_expr(cfg, n, arg,
                     (int)i == return_borrow_param ? return_holder : NULL);
        if (arg && arg->is_move_source && root_var(arg))
            write_var(n, root_var(arg));
        Param *pm = target && i < target->params.len ? target->params.items[i] : NULL;
        const Type *param_type = pm ? (pm->resolved ? pm->resolved : pm->type) : NULL;
        bool borrow_arg = (pm && pm->by_ref) ||
            (param_type && (param_type->kind == TY_REF || param_type->kind == TY_MUTREF)) ||
            (!pm && arg && arg->type &&
             (arg->type->kind == TY_REF || arg->type->kind == TY_MUTREF));
        if (borrow_arg) {
            for (size_t fi = flow_start; fi < n->flow.len; fi++) {
                BorrowFlowEvent *flow = n->flow.items[fi];
                if (flow->kind == FLOW_USE) flow->borrow_source = true;
            }
        }
        bool reference_value = arg && arg->type &&
            (arg->type->kind == TY_REF || arg->type->kind == TY_MUTREF);
        bool may_mutate_heap_arg = !target || !pm || !target->has_body ||
            target->is_method ||
            pm->mutates_heap_alias || pm->captures_heap_alias;
        if (may_mutate_heap_arg && arg && !(pm && pm->by_ref) && !reference_value &&
            (heap_object_base(arg->type) || (arg->type && arg->type->kind == TY_MIXED) ||
             heap_types_related(arg->type, arg->type))) {
            const VarSym *root = root_var(arg);
            if (root) write_object(n, root, arg->type, arg);
            else write_unknown_heap_object(n, arg->type, NULL);
            if (may_escape_heap_args && n->writes.len) {
                BorrowWrite *w = n->writes.items[n->writes.len - 1];
                w->closure_escape_arg = true;
                w->may_alias = true;
            }
            if (pm && pm->captures_heap_alias && n->writes.len) {
                BorrowWrite *w = n->writes.items[n->writes.len - 1];
                w->closure_escape_arg = true;
                w->may_alias = true;
            }
        }
        if (pm && pm->by_ref && arg && arg->kind == EX_VAR) {
            if (!((int)i == return_borrow_param && return_holder)) {
                const VarSym *v = arg->u.var.sym;
                event_add(cfg, n, v, NULL, arg, true, arg->tok);
                const Type *byref_type = pm->resolved ? pm->resolved : pm->type;
                if (n->borrows.len && byref_type &&
                    (byref_type->kind == TY_BOOL || byref_type->kind == TY_INT ||
                     byref_type->kind == TY_FLOAT || byref_type->kind == TY_STRING))
                    ((BorrowEvent *)n->borrows.items[n->borrows.len - 1])->slot_only = true;
            }
        } else if (((pm && (pm->resolved ? pm->resolved : pm->type) &&
                     ((pm->resolved ? pm->resolved : pm->type)->kind == TY_REF ||
                      (pm->resolved ? pm->resolved : pm->type)->kind == TY_MUTREF)) ||
                    (!pm && arg && arg->type &&
                     (arg->type->kind == TY_REF || arg->type->kind == TY_MUTREF))) &&
                   arg && arg->kind == EX_VAR) {
            const VarSym *v = arg->u.var.sym;
            const Type *pt = pm ? (pm->resolved ? pm->resolved : pm->type) : arg->type;
            bool is_mut = pt->kind == TY_MUTREF;
            const VarSym *origin = v->borrow_origin ? v->borrow_origin : v;
            /* Returning this exact parameter creates the result holder event
             * below; avoid recording the same temporary loan twice. */
            if (!((int)i == return_borrow_param && return_holder)) {
                event_add(cfg, n, origin, NULL, v->borrow_place, is_mut, arg->tok);
                if (n->borrows.len && v->type &&
                    (v->type->kind == TY_REF || v->type->kind == TY_MUTREF))
                    ((BorrowEvent *)n->borrows.items[n->borrows.len - 1])->parent = v;
            }
        } else if ((!target && builtin_mutates_first(name) && i == 0) && arg &&
                   arg->kind == EX_VAR) {
            const VarSym *v = arg->u.var.sym;
            write_var(n, v); /* immediate in-place mutation, not a live loan */
        } else if (!target && builtin_mutates_first(name) && i == 0 && arg &&
                   (arg->kind == EX_FIELD || arg->kind == EX_INDEX || arg->kind == EX_DEREF)) {
            const VarSym *root = root_var(arg);
            if (root && root->type && root->type->kind == TY_MUTREF)
                write_through(cfg, n, root, arg);
            else
                write_object(n, root, arg->type, arg);
        }
        /* preg_match's matches output and sort-like APIs are modeled as
         * writes even when their built-in signature has no Param objects. */
        if ((!target && name &&
             (strcmp(name, "preg_match") == 0 || strcmp(name, "preg_match_all") == 0) && i == 2) &&
            arg && arg->kind == EX_VAR)
            write_var(n, (const VarSym *)arg->u.var.sym);
    }
}

static AstFn *constructor_of(AstClass *c) {
    if (!c) return NULL;
    for (size_t i = 0; i < c->methods.len; i++) {
        AstFn *fn = c->methods.items[i];
        if (strcmp(fn->name, "__construct") == 0) return fn;
    }
    return c->base ? constructor_of(c->base) : NULL;
}

static void collect_expr(BorrowCfg *cfg, BorrowNode *n, Expr *e,
                         const VarSym *holder) {
    (void)cfg;
    if (!e) return;
    switch (e->kind) {
    case EX_VAR:
        use_var(n, (const VarSym *)e->u.var.sym);
        flow_event(cfg, n, (const VarSym *)e->u.var.sym, e, &e->tok, FLOW_USE);
        if (e->is_move_source) {
            flow_event(cfg, n, (const VarSym *)e->u.var.sym, e, &e->tok, FLOW_MOVE);
        }
        break;
    case EX_INDEX:
        if (root_var(e)) {
            const VarSym *root = root_var(e);
            use_var(n, root);
            flow_event(cfg, n, root, e, &e->tok, FLOW_USE);
            if (e->is_move_source) flow_event(cfg, n, root, e, &e->tok, FLOW_MOVE);
        } else {
            collect_expr(cfg, n, e->u.index.obj, NULL);
        }
        collect_expr(cfg, n, e->u.index.idx, NULL);
        break;
    case EX_FIELD:
        if (root_var(e)) {
            const VarSym *root = root_var(e);
            use_var(n, root);
            flow_event(cfg, n, root, e, &e->tok, FLOW_USE);
            if (e->is_move_source) {
                flow_event(cfg, n, root, e, &e->tok, FLOW_MOVE);
            }
        } else {
            collect_expr(cfg, n, e->u.field.obj, NULL);
        }
        break;
    case EX_CALL:
        collect_expr(cfg, n, e->u.call.fn, NULL);
        collect_ref_call_result(cfg, n, e, e->u.call.args, holder);
        collect_call_args(cfg, n, e->u.call.args, e->u.call.target,
                          e->u.call.fn && e->u.call.fn->kind == EX_VAR
                            ? e->u.call.fn->u.var.name : NULL,
                          e->has_borrow_param ? e->borrow_param_index : -1,
                          holder, !e->builtin &&
                              ((!e->u.call.target && e->u.call.fn && e->u.call.fn->type && e->u.call.fn->type->kind == TY_FN) || (e->u.call.target && !e->u.call.target->has_body)));
        collect_global_effects(n, e->u.call.target);
        if (!e->u.call.target && !e->builtin) {
            write_unknown_global_effect(n);
            write_unknown_heap_object(n, &borrow_mixed_type, NULL);
        }
        break;
    case EX_CLOSURE:
        if (e->u.closure.fn) {
            AstFn *fn = e->u.closure.fn;
            for (size_t i = 0; i < fn->captures.len; i++) {
                const VarSym *capture = fn->captures.items[i];
                bool thread_move = false;
                for (size_t j = 0; j < fn->thread_moves.len; j++)
                    if (fn->thread_moves.items[j] == capture) { thread_move = true; break; }
                if (!capture || capture->captured_byref ||
                    (!sema_type_is_linear(capture->type) && !thread_move)) continue;
                use_var(n, capture);
                flow_event(cfg, n, capture, NULL, &e->tok, FLOW_USE);
                flow_event(cfg, n, capture, NULL, &e->tok, FLOW_MOVE);
                write_var(n, capture);
            }
        }
        break;
    case EX_METHOD:
        collect_expr(cfg, n, e->u.method.obj, NULL);
        collect_ref_call_result(cfg, n, e, e->u.method.args, holder);
        {
        size_t method_effect_start = n->writes.len;
        bool mutates_receiver = collect_method_effects(n, e);
        size_t writes_before_receiver = n->writes.len;
        if (mutates_receiver && root_var(e->u.method.obj)) {
            const VarSym *recv = root_var(e->u.method.obj);
            if (recv->type && recv->type->kind == TY_MUTREF)
                write_through(cfg, n, recv, e->u.method.obj);
            else
                write_object(n, recv, e->u.method.obj->type, e->u.method.obj);
        } else if (mutates_receiver &&
                   heap_types_related(e->u.method.obj->type,
                                      e->u.method.obj->type)) {
            write_unknown_heap_object(n, e->u.method.obj->type, NULL);
        }
        if (mutates_receiver && expr_uses_shared_ref(e->u.method.obj) &&
            n->writes.len > writes_before_receiver) {
            BorrowWrite *w = n->writes.items[n->writes.len - 1];
            w->shared_receiver_violation = true;
            w->call_tok = e->u.method.name_tok;
        }
        size_t method_effect_end = n->writes.len;
        collect_call_args(cfg, n, e->u.method.args, e->u.method.target,
                          e->u.method.name,
                          e->has_borrow_param ? e->borrow_param_index : -1,
                          holder, !e->builtin &&
                              (e->u.method.target && !e->u.method.target->has_body));
        /* Receiver and callee effects happen after every argument has been
         * evaluated. Keep nested argument effects at their own positions. */
        for (size_t i = method_effect_start; i < method_effect_end; i++)
            ((BorrowWrite *)n->writes.items[i])->flow_index = n->flow.len;
        }
        break;
    case EX_STATIC:
        collect_ref_call_result(cfg, n, e, e->u.staticcall.args, holder);
        collect_call_args(cfg, n, e->u.staticcall.args, e->u.staticcall.target,
                          e->u.staticcall.name,
                          e->has_borrow_param ? e->borrow_param_index : -1,
                          holder, !e->builtin &&
                              (e->u.staticcall.target && !e->u.staticcall.target->has_body));
        collect_global_effects(n, e->u.staticcall.target);
        if (!e->u.staticcall.target && !e->builtin) {
            write_unknown_global_effect(n);
            write_unknown_heap_object(n, &borrow_mixed_type, NULL);
        }
        break;
    case EX_NEW:
        collect_call_args(cfg, n, e->u.newexpr.args,
                          constructor_of(e->u.newexpr.cls_res), "__construct", -1, NULL,
                          constructor_of(e->u.newexpr.cls_res) &&
                              !constructor_of(e->u.newexpr.cls_res)->has_body);
        collect_global_effects(n, constructor_of(e->u.newexpr.cls_res));
        break;
    case EX_TPL:
        for (size_t i = 0; i < e->u.tpl.exprs.len; i++)
            collect_expr(cfg, n, e->u.tpl.exprs.items[i], NULL);
        break;
    case EX_BIN:
        collect_expr(cfg, n, e->u.bin.l, NULL);
        collect_expr(cfg, n, e->u.bin.r, NULL);
        collect_expr(cfg, n, e->u.bin.slice_hi, NULL);
        break;
    case EX_UN:
        collect_expr(cfg, n, e->u.un.operand, NULL);
        break;
    case EX_ASSIGN: {
        Expr *target = e->u.assign.target;
        size_t writes_before_target = n->writes.len;
        if (target && target->kind == EX_VAR) {
            const VarSym *v = (const VarSym *)target->u.var.sym;
            def_var(n, v);
            if (e->u.assign.op.kind != T_ASSIGN && e->u.assign.op.kind != T_QQASSIGN)
            {
                use_var(n, v);
                flow_event(cfg, n, v, target, &target->tok, FLOW_USE);
            }
            write_var(n, v);
        } else {
            if (target && root_var(target) &&
                (target->kind == EX_FIELD || target->kind == EX_INDEX))
                collect_place_indices(cfg, n, target);
            else
                collect_expr(cfg, n, target, NULL);
            const VarSym *root = root_var(target);
            if (root && root->type && root->type->kind == TY_MUTREF)
                write_through(cfg, n, root, target);
            else if (target && (target->kind == EX_FIELD || target->kind == EX_INDEX))
            {
                const Type *object_type = target->kind == EX_FIELD
                    ? target->u.field.obj->type : target->u.index.obj->type;
                if (root) write_object(n, root, object_type, target);
                else write_unknown_heap_object(n, object_type, target);
            }
            else
                write_var(n, root);
        }
        BorrowWrite *target_write = n->writes.len > writes_before_target
            ? n->writes.items[n->writes.len - 1] : NULL;
        const VarSym *target_holder = target && target->kind == EX_VAR
            ? (const VarSym *)target->u.var.sym : NULL;
        if (target_holder && target_holder->borrow_origin && e->u.assign.value &&
            (e->u.assign.value->kind == EX_BORROW || e->u.assign.value->kind == EX_MUTBORROW))
            target_holder = NULL; /* illegal ref rebinding already diagnosed */
        collect_expr(cfg, n, e->u.assign.value,
                     target_holder && target_holder->borrow_origin ? target_holder : holder);
        /* The assignment's store happens after its RHS has been evaluated. */
        if (target_write) target_write->flow_index = n->flow.len;
        if (target && target->kind == EX_VAR)
            init_var(cfg, n, (const VarSym *)target->u.var.sym, NULL, &target->tok);
        else if (target && (target->kind == EX_FIELD || target->kind == EX_INDEX) &&
                 root_var(target))
            init_var(cfg, n, root_var(target), target, &target->tok);
        if (e->u.assign.value && e->u.assign.value->is_move_source &&
            root_var(e->u.assign.value) && root_var(e->u.assign.value) != target_holder)
            write_var(n, root_var(e->u.assign.value));
        break;
    }
    case EX_TERNARY:
        collect_expr(cfg, n, e->u.tern.cond, NULL);
        {
        size_t group = ++cfg->next_alternative;
        alternative_push(cfg, group, 0);
        collect_expr(cfg, n, e->u.tern.then, NULL);
        alternative_pop(cfg);
        alternative_push(cfg, group, 1);
        collect_expr(cfg, n, e->u.tern.els, NULL);
        alternative_pop(cfg);
        }
        break;
    case EX_MATCH:
        collect_expr(cfg, n, e->u.matchexpr.subject, NULL);
        {
        size_t group = ++cfg->next_alternative;
        for (size_t i = 0; i < e->u.matchexpr.cases.len; i++) {
            MatchCase *c = e->u.matchexpr.cases.items[i];
            for (size_t j = 0; j < c->patterns.len; j++)
                collect_expr(cfg, n, c->patterns.items[j], NULL);
            alternative_push(cfg, group, i);
            collect_expr(cfg, n, c->body, NULL);
            alternative_pop(cfg);
        }
        if (e->u.matchexpr.dflt) alternative_push(cfg, group, e->u.matchexpr.cases.len);
        collect_expr(cfg, n, e->u.matchexpr.dflt, NULL);
        if (e->u.matchexpr.dflt) alternative_pop(cfg);
        }
        break;
    case EX_ARRAY_LIT:
        if (e->u.map.vals.len) {
            for (size_t i = 0; i < e->u.map.vals.len; i++) {
                collect_expr(cfg, n, e->u.map.keys.items[i], NULL);
                collect_expr(cfg, n, e->u.map.vals.items[i], NULL);
            }
        } else {
            for (size_t i = 0; i < e->u.arr.elems.len; i++)
                collect_expr(cfg, n, e->u.arr.elems.items[i], NULL);
        }
        break;
    case EX_MAP_LIT:
        for (size_t i = 0; i < e->u.map.keys.len; i++) {
            collect_expr(cfg, n, e->u.map.keys.items[i], NULL);
            collect_expr(cfg, n, e->u.map.vals.items[i], NULL);
        }
        break;
    case EX_TUPLE_LIT:
        for (size_t i = 0; i < e->u.tuple.elems.len; i++)
            collect_expr(cfg, n, e->u.tuple.elems.items[i], NULL);
        break;
    case EX_BORROW: case EX_MUTBORROW: {
        Expr *op = e->u.borrow.operand;
        const VarSym *source = root_var(op);
        const VarSym *origin = e->borrow_origin ? (const VarSym *)e->borrow_origin :
            source && source->borrow_origin ? source->borrow_origin : source;
        size_t flow_start = n->flow.len;
        collect_expr(cfg, n, op, NULL);
        for (size_t i = flow_start; i < n->flow.len; i++) {
            BorrowFlowEvent *use = n->flow.items[i];
            if (use->kind == FLOW_USE) use->borrow_source = true;
        }
        if (source && source->type &&
            (source->type->kind == TY_REF || source->type->kind == TY_MUTREF)) {
            for (size_t i = flow_start; i < n->flow.len; i++) {
                BorrowFlowEvent *use = n->flow.items[i];
                if (use->kind == FLOW_USE && use->var == source)
                    use->reborrow_source = true;
            }
        }
        event_add(cfg, n, origin, holder, e->borrow_place ? e->borrow_place : op,
                  e->kind == EX_MUTBORROW, e->tok);
        if (n->borrows.len)
            ((BorrowEvent *)n->borrows.items[n->borrows.len - 1])->parent =
                (const VarSym *)e->borrow_parent;
        break;
    }
    case EX_DEREF:
        collect_expr(cfg, n, e->u.deref.operand, NULL);
        break;
    case EX_CAST:
        collect_expr(cfg, n, e->u.cast.operand, NULL);
        break;
    case EX_IS:
        collect_expr(cfg, n, e->u.ischeck.operand, NULL);
        break;
    case EX_ELLIPSIS:
        collect_expr(cfg, n, e->u.ellipsis.inner, NULL);
        break;
    default:
        break;
    }
}

static int cfg_build_seq(BorrowCfg *cfg, PtrVec stmts, int next,
                         const LoopTargets *targets);

static int cfg_simple(BorrowCfg *cfg, Stmt *s, int next,
                      const LoopTargets *targets) {
    int id = node_new(cfg, s);
    edge(cfg, id, next);
    BorrowNode *n = node_at(cfg, id);
    switch (s->kind) {
    case ST_EXPR: collect_expr(cfg, n, s->u.expr.expr, NULL); break;
    case ST_LET: {
        VarSym *v = s->u.let.sym;
        if (s->u.let.init)
            collect_expr(cfg, n, s->u.let.init,
                         v && v->borrow_origin ? v : NULL);
        def_var(n, v);
        init_var(cfg, n, v, NULL, &s->tok);
        Expr *init = s->u.let.init;
        if (init && init->is_move_source && root_var(init))
            write_var(n, root_var(init)); /* moving invalidates the source place */
        break;
    }
    case ST_RETURN: collect_expr(cfg, n, s->u.ret.value, NULL); clear_successors(cfg, id); break;
    case ST_ECHO:
        for (size_t i = 0; i < s->u.echo.exprs.len; i++)
            collect_expr(cfg, n, s->u.echo.exprs.items[i], NULL);
        break;
    case ST_PRINT: collect_expr(cfg, n, s->u.print.expr, NULL); break;
    case ST_THROW: collect_expr(cfg, n, s->u.throw.expr, NULL); clear_successors(cfg, id); break;
    case ST_GLOBAL: break;
    case ST_BREAK:
        clear_successors(cfg, id);
        if (targets->nbreaks) {
            size_t depth = s->u.brk.depth > 0 ? (size_t)s->u.brk.depth : 1;
            if (depth <= targets->nbreaks)
                edge(cfg, id, targets->breaks[targets->nbreaks - depth]);
            else
                edge(cfg, id, next);
        } else edge(cfg, id, next);
        break;
    case ST_CONTINUE:
        clear_successors(cfg, id);
        if (targets->ncontinues) {
            size_t depth = s->u.brk.depth > 0 ? (size_t)s->u.brk.depth : 1;
            if (depth <= targets->ncontinues)
                edge(cfg, id, targets->continues[targets->ncontinues - depth]);
            else
                edge(cfg, id, next);
        } else edge(cfg, id, next);
        break;
    default: break;
    }
    return id;
}

static int cfg_build_stmt(BorrowCfg *cfg, Stmt *s, int next,
                          const LoopTargets *targets) {
    if (!s) return next;
    switch (s->kind) {
    case ST_BLOCK: return cfg_build_seq(cfg, s->u.block.stmts, next, targets);
    case ST_UNSAFE_BLOCK: return cfg_build_seq(cfg, s->u.unsafe.stmts, next, targets);
    case ST_IF: {
        int id = node_new(cfg, s);
        collect_expr(cfg, node_at(cfg, id), s->u.if_.cond, NULL);
        int then_entry = cfg_build_seq(cfg, s->u.if_.then, next, targets);
        int else_entry = cfg_build_seq(cfg, s->u.if_.els, next, targets);
        edge(cfg, id, then_entry);
        edge(cfg, id, else_entry);
        return id;
    }
    case ST_WHILE: {
        int cond = node_new(cfg, s);
        collect_expr(cfg, node_at(cfg, cond), s->u.while_.cond, NULL);
        LoopTargets body_targets = targets_clone(targets);
        targets_push_break(&body_targets, next);
        targets_push_continue(&body_targets, cond);
        int body = cfg_build_seq(cfg, s->u.while_.body, cond, &body_targets);
        targets_dispose(&body_targets);
        edge(cfg, cond, body);
        edge(cfg, cond, next);
        return cond;
    }
    case ST_DO: {
        int cond = node_new(cfg, s);
        collect_expr(cfg, node_at(cfg, cond), s->u.do_.cond, NULL);
        LoopTargets body_targets = targets_clone(targets);
        targets_push_break(&body_targets, next);
        targets_push_continue(&body_targets, cond);
        int body = cfg_build_seq(cfg, s->u.do_.body, cond, &body_targets);
        targets_dispose(&body_targets);
        edge(cfg, cond, body);
        edge(cfg, cond, next);
        return body;
    }
    case ST_FOR: {
        int cond = node_new(cfg, s);
        collect_expr(cfg, node_at(cfg, cond), s->u.for_.cond, NULL);
        int step = cfg_build_seq(cfg, s->u.for_.step, cond, targets);
        LoopTargets body_targets = targets_clone(targets);
        targets_push_break(&body_targets, next);
        targets_push_continue(&body_targets, step);
        int body = cfg_build_seq(cfg, s->u.for_.body, step, &body_targets);
        targets_dispose(&body_targets);
        edge(cfg, cond, body);
        edge(cfg, cond, next);
        return cfg_build_seq(cfg, s->u.for_.init, cond, targets);
    }
    case ST_FOREACH: {
        int head = node_new(cfg, s);
        BorrowNode *n = node_at(cfg, head);
        collect_expr(cfg, n, s->u.foreach.iter, NULL);
        def_var(n, s->u.foreach.vsym);
        def_var(n, s->u.foreach.ksym);
        init_var(cfg, n, s->u.foreach.vsym, NULL, &s->tok);
        init_var(cfg, n, s->u.foreach.ksym, NULL, &s->tok);
        LoopTargets body_targets = targets_clone(targets);
        targets_push_break(&body_targets, next);
        targets_push_continue(&body_targets, head);
        int body = cfg_build_seq(cfg, s->u.foreach.body, head, &body_targets);
        targets_dispose(&body_targets);
        edge(cfg, head, body);
        edge(cfg, head, next);
        return head;
    }
    case ST_SWITCH: {
        int id = node_new(cfg, s);
        BorrowNode *n = node_at(cfg, id);
        collect_expr(cfg, n, s->u.switch_.cond, NULL);
        int entry = next;
        LoopTargets case_targets = targets_clone(targets);
        targets_push_break(&case_targets, next);
        int *cases = xmalloc(sizeof(int) * (s->u.switch_.cases.len ? s->u.switch_.cases.len : 1));
        for (size_t i = s->u.switch_.cases.len; i > 0; i--) {
            SwitchCase *c = s->u.switch_.cases.items[i - 1];
            for (size_t j = 0; j < c->patterns.len; j++)
                collect_expr(cfg, n, c->patterns.items[j], NULL);
            entry = cfg_build_seq(cfg, c->body, entry, &case_targets);
            cases[i - 1] = entry;
        }
        for (size_t i = 0; i < s->u.switch_.cases.len; i++) edge(cfg, id, cases[i]);
        edge(cfg, id, next);
        free(cases);
        targets_dispose(&case_targets);
        return id;
    }
    case ST_TRY: {
        int fin = cfg_build_seq(cfg, s->u.try.fin, next, targets);
        size_t catch_start = cfg->nodes.len;
        int *catches = xmalloc(sizeof(int) * (s->u.try.catches.len ? s->u.try.catches.len : 1));
        for (size_t i = s->u.try.catches.len; i > 0; i--) {
            CatchClause *c = s->u.try.catches.items[i - 1];
            int ci = cfg_build_seq(cfg, c->body, fin, targets);
            catches[i - 1] = ci;
        }
        size_t catch_end = cfg->nodes.len;
        if (s->u.try.fin.len)
            for (size_t i = catch_start; i < catch_end; i++) edge(cfg, (int)i, fin);
        size_t before = cfg->nodes.len;
        int body = cfg_build_seq(cfg, s->u.try.body, fin, targets);
        for (size_t i = before; i < cfg->nodes.len; i++) {
            for (size_t j = 0; j < s->u.try.catches.len; j++)
                edge(cfg, (int)i, catches[j]);
            if (s->u.try.fin.len) edge(cfg, (int)i, fin);
        }
        free(catches);
        return body;
    }
    default:
        return cfg_simple(cfg, s, next, targets);
    }
}

static int cfg_build_seq(BorrowCfg *cfg, PtrVec stmts, int next,
                         const LoopTargets *targets) {
    int entry = next;
    for (size_t i = stmts.len; i > 0; i--)
        entry = cfg_build_stmt(cfg, stmts.items[i - 1], entry, targets);
    return entry;
}

static bool borrow_ancestor(const VarSym *ancestor, const VarSym *child) {
    for (const VarSym *v = child; v; v = v->borrow_parent)
        if (v == ancestor) return true;
    return false;
}

static void live_set(uint64_t *bits, size_t bit) {
    bits[bit >> 6] |= UINT64_C(1) << (bit & 63);
}

static void live_clear(uint64_t *bits, size_t bit) {
    bits[bit >> 6] &= ~(UINT64_C(1) << (bit & 63));
}

static bool live_test(const uint64_t *bits, size_t bit) {
    return ((bits[bit >> 6] >> (bit & 63)) & UINT64_C(1)) != 0;
}

/* Compare projection segments from the aliased base outward. */
static size_t place_segments(const Expr *e, const Expr **out, size_t n, size_t cap) {
    if (!e || e->kind == EX_VAR) return n;
    const Expr *base = e->kind == EX_FIELD ? e->u.field.obj :
                       e->kind == EX_INDEX ? e->u.index.obj : NULL;
    if (!base) return n;
    n = place_segments(base, out, n, cap);
    if (n < cap) out[n++] = e;
    return n;
}

static size_t place_segment_count(const Expr *e) {
    if (!e || (e->kind != EX_FIELD && e->kind != EX_INDEX)) return 0;
    const Expr *base = e->kind == EX_FIELD ? e->u.field.obj : e->u.index.obj;
    return 1 + place_segment_count(base);
}

/* Scope-local views created by `global $x` intentionally share a uid with
 * the file-scope VarSym. Dataflow state belongs to the storage slot, not the
 * particular symbol view used by an expression. */
static bool same_var_slot(const VarSym *a, const VarSym *b) {
    return a == b || (a && b && a->uid >= 0 && a->uid == b->uid);
}

static bool same_index_key(const Expr *a, const Expr *b) {
    if (a == b) return true;
    if (!a || !b || a->kind != b->kind) return false;
    switch (a->kind) {
    case EX_INT: return a->u.i.ival == b->u.i.ival;
    case EX_STR:
        return a->u.str.s && b->u.str.s && strcmp(a->u.str.s, b->u.str.s) == 0;
    /* A variable's value may change between the move and reinitialization;
     * name equality alone does not prove that both accesses hit one slot. */
    case EX_VAR: return false;
    case EX_BOOL: return a->u.boolean.b == b->u.boolean.b;
    case EX_NULL: return true;
    default: return false;
    }
}

static bool same_place_segment(const Expr *a, const Expr *b) {
    if (!a || !b || a->kind != b->kind) return false;
    if (a->kind == EX_FIELD)
        return a->u.field.name && b->u.field.name &&
               strcmp(a->u.field.name, b->u.field.name) == 0;
    if (a->kind == EX_INDEX)
        return same_index_key(a->u.index.idx, b->u.index.idx);
    return false;
}

static bool place_prefix(const VarSym *root, const Expr *prefix, const Expr *place) {
    if (!root) return false;
    if (!prefix) return true;
    if (!place || !same_var_slot(root_var(prefix), root) ||
        !same_var_slot(root_var(place), root)) return false;
    size_t prefix_count = place_segment_count(prefix);
    size_t place_count = place_segment_count(place);
    if (prefix_count > place_count) return false;
    /* This predicate is on the move-analysis hot path. Keep it allocation
     * free; on pathological nesting, refusing to prove a prefix is safe. */
    const size_t cap = 128;
    if (prefix_count > cap) return false;
    const Expr *prefix_parts[128], *place_parts[128];
    place_segments(prefix, prefix_parts, 0, prefix_count);
    place_segments(place, place_parts, 0, prefix_count);
    bool matches = true;
    for (size_t i = 0; i < prefix_count; i++)
        if (!same_place_segment(prefix_parts[i], place_parts[i])) {
            matches = false;
            break;
        }
    return matches;
}

static bool place_paths_overlap(const Expr *a, const Expr *b) {
    const Expr *as[128], *bs[128];
    size_t an = place_segments(a, as, 0, 128);
    size_t bn = place_segments(b, bs, 0, 128);
    size_t n = an < bn ? an : bn;
    for (size_t i = 0; i < n; i++) {
        if (as[i]->kind == EX_FIELD && bs[i]->kind == EX_FIELD &&
            as[i]->u.field.name && bs[i]->u.field.name &&
            strcmp(as[i]->u.field.name, bs[i]->u.field.name) != 0) {
            /* Different pointer-valued fields can still lead to the same
             * heap object when either path continues through the pointer. */
            bool a_indirect = i + 1 < an && heap_object_base(as[i]->type);
            bool b_indirect = i + 1 < bn && heap_object_base(bs[i]->type);
            if (a_indirect || b_indirect) return true;
            return false;
        }
    }
    /* A path prefix overlaps its descendants (moving a whole field prevents
     * reading its subfields); dynamic indices conservatively alias. */
    return true;
}

static bool places_overlap(const MovePlace *a, const MovePlace *b) {
    if (!a || !b || !borrow_origins_may_alias(a->root, b->root)) return false;
    /* A NULL path denotes an explicit transfer of the whole variable (used
     * when a closure capture has no source Expr node), so it conflicts with
     * every later access through that root. */
    if (!a->place || !b->place) return true;
    /* Moving an element removes that slot from the collection; it does not
     * consume the collection handle itself. Keep root reads (len, push, etc.)
     * available while conservatively treating other indexed places as
     * potentially aliased because the compiler cannot prove dynamic indices
     * distinct yet. */
    bool a_root = !a->place || a->place->kind == EX_VAR;
    bool b_root = !b->place || b->place->kind == EX_VAR;
    if ((a->place && a->place->kind == EX_INDEX && b_root) ||
        (b->place && b->place->kind == EX_INDEX && a_root))
        return false;
    return place_paths_overlap(a->place, b->place);
}

static bool borrowed_places_overlap(const VarSym *a_origin, const Expr *a_place,
                                    const VarSym *b_origin, const Expr *b_place) {
    return borrow_origins_may_alias(a_origin, b_origin) &&
           place_paths_overlap(a_place, b_place);
}

static void append_place_path(const Expr *place, char *out, size_t cap) {
    if (!place || !out || !cap) return;
    if (place->kind == EX_FIELD) {
        append_place_path(place->u.field.obj, out, cap);
        size_t used = strlen(out);
        if (used < cap) snprintf(out + used, cap - used, "->%s", place->u.field.name);
    } else if (place->kind == EX_INDEX) {
        append_place_path(place->u.index.obj, out, cap);
        size_t used = strlen(out);
        if (used < cap) snprintf(out + used, cap - used, "[?]");
    }
}

static void describe_place(const VarSym *root, const Expr *place, char *out, size_t cap) {
    if (!out || !cap) return;
    snprintf(out, cap, "$%s", root && root->name ? root->name : "?");
    append_place_path(place, out, cap);
}

static bool place_equal(const MovePlace *a, const BorrowFlowEvent *b) {
    if (!a || !b || !same_var_slot(a->root, b->var)) return false;
    /* Closure capture moves have no source Expr node: the capture list stores
     * its VarSym directly. Treat that root-place event as the corresponding
     * variable expression for later use-after-move diagnostics. */
    if (!a->place)
        return !b->place || (b->place->kind == EX_VAR &&
                             same_var_slot(root_var(b->place), a->root));
    return
        place_prefix(a->root, a->place, b->place) &&
        place_prefix(a->root, b->place, a->place);
}

static int move_place_index(const PtrVec *places, const VarSym *root,
                            const Expr *place, const int *root_heads,
                            size_t uid_count) {
    BorrowFlowEvent query = {.var = root, .place = place};
    if (root && root->uid >= 0 && (size_t)root->uid < uid_count) {
        for (int i = root_heads[root->uid]; i >= 0;
             i = ((MovePlace *)places->items[i])->next_root)
            if (place_equal(places->items[i], &query)) return i;
        return -1;
    }
    for (size_t i = 0; i < places->len; i++)
        if (place_equal(places->items[i], &query)) return (int)i;
    return -1;
}

static bool flow_events_exclusive(const BorrowFlowEvent *a, const BorrowFlowEvent *b) {
    if (!a || !b) return false;
    for (size_t i = 0; i < a->alternatives.len; i++) {
        BorrowAlternative *aa = a->alternatives.items[i];
        for (size_t j = 0; j < b->alternatives.len; j++) {
            BorrowAlternative *bb = b->alternatives.items[j];
            if (aa->group == bb->group && aa->arm != bb->arm) return true;
        }
    }
    return false;
}

static void check_parent_move_candidate(const MovePlace *moved, size_t moved_index,
                                        const BorrowFlowEvent *init,
                                        const BorrowNode *node, size_t event_index,
                                        size_t node_index, size_t words,
                                        const uint64_t *ins) {
    if (!place_prefix(moved->root, moved->place, init->place) ||
        place_prefix(moved->root, init->place, moved->place)) return;
    bool moved_now = live_test(&ins[node_index * words], moved_index);
    for (size_t p = 0; p < event_index; p++) {
        BorrowFlowEvent *prior = node->flow.items[p];
        if (flow_events_exclusive(prior, init) ||
            !same_var_slot(prior->var, moved->root)) continue;
        if (prior->kind == FLOW_MOVE && place_equal(moved, prior))
            moved_now = true;
        else if (prior->kind == FLOW_INIT &&
                 place_prefix(moved->root, prior->place, moved->place))
            moved_now = false;
    }
    if (!moved_now) return;
    char moved_desc[256], init_desc[256];
    describe_place(moved->root, moved->place, moved_desc, sizeof(moved_desc));
    describe_place(init->var, init->place, init_desc, sizeof(init_desc));
    sema_error(&init->tok,
               "cannot initialize %s while its parent place %s is moved",
               init_desc, moved_desc);
    borrow_note(&moved->tok, "the parent place was moved here");
}

/* Forward may-moved analysis. At a join, a place is considered moved if any
 * incoming path moved it. Ordered expression events catch use-after-move
 * within a statement; exclusive alternatives are compared only along paths
 * that can execute together. Conditional initialization stays conservative. */
static void check_moved_values(BorrowCfg *cfg) {
    PtrVec places = {0};
    size_t count = cfg->nodes.len;
    size_t uid_count = 1;
    for (size_t i = 0; i < count; i++) {
        BorrowNode *n = node_at(cfg, (int)i);
        for (size_t j = 0; j < n->flow.len; j++) {
            BorrowFlowEvent *event = n->flow.items[j];
            if (event->var && event->var->uid >= 0 &&
                (size_t)event->var->uid >= uid_count)
                uid_count = (size_t)event->var->uid + 1;
        }
    }
    int *root_heads = xmalloc(uid_count * sizeof(int));
    for (size_t i = 0; i < uid_count; i++) root_heads[i] = -1;
    for (size_t i = 0; i < count; i++) {
        BorrowNode *n = node_at(cfg, (int)i);
        for (size_t j = 0; j < n->flow.len; j++) {
            BorrowFlowEvent *event = n->flow.items[j];
            if (event->kind != FLOW_MOVE || !event->var) continue;
            if (event->place && event->place->kind != EX_VAR && event->var->type &&
                (event->var->type->kind == TY_REF ||
                 event->var->type->kind == TY_MUTREF)) {
                sema_error(&event->tok,
                           "cannot move an ownership value out through borrowed reference $%s",
                           event->var->name);
                borrow_note(&event->var->tok,
                            "the reference does not transfer ownership of its referent");
            } else if (event->place && event->place->kind != EX_VAR &&
                       ((event->var->type && event->var->type->kind == TY_RC) ||
                        event->var->is_param || event->var->alias_unknown ||
                        (event->var->alias_origin &&
                         event->var->alias_origin != event->var))) {
                sema_error(&event->tok,
                           "cannot move an ownership value out of possibly aliased $%s",
                           event->var->name);
                borrow_note(&event->var->tok,
                            "this object may be reachable through another binding");
            }
            if (move_place_index(&places, event->var, event->place,
                                 root_heads, uid_count) >= 0) continue;
            MovePlace *place = xmalloc(sizeof(*place));
            place->root = event->var;
            place->place = event->place;
            place->tok = event->tok;
            place->next_root = event->var->uid >= 0 &&
                (size_t)event->var->uid < uid_count
                    ? root_heads[event->var->uid] : -1;
            ptrvec_push(&places, place);
            if (event->var->uid >= 0 && (size_t)event->var->uid < uid_count)
                root_heads[event->var->uid] = (int)places.len - 1;
        }
    }
    if (!places.len || !count) {
        for (size_t i = 0; i < places.len; i++) free(places.items[i]);
        free(places.items);
        free(root_heads);
        return;
    }
    size_t words = (places.len + 63) / 64;
    uint64_t *ins = xcalloc(count * words, sizeof(uint64_t));
    uint64_t *outs = xcalloc(count * words, sizeof(uint64_t));
    uint64_t *next_in = xcalloc(words, sizeof(uint64_t));
    uint64_t *next_out = xcalloc(words, sizeof(uint64_t));
    bool *queued = xcalloc(count, sizeof(bool));
    int *queue = xmalloc(count * sizeof(int));
    size_t head = 0, tail = 0, pending = 0;
    for (size_t i = 0; i < count; i++) {
        queue[tail] = (int)i; tail = (tail + 1) % count;
        queued[i] = true; pending++;
    }
    while (pending) {
        int id = queue[head]; head = (head + 1) % count;
        pending--; queued[id] = false;
        BorrowNode *n = node_at(cfg, id);
        memset(next_in, 0, words * sizeof(uint64_t));
        for (size_t p = 0; p < n->pred.len; p++) {
            int pred = (int)(intptr_t)n->pred.items[p] - 1;
            if (pred < 0) continue;
            const uint64_t *pred_out = &outs[(size_t)pred * words];
            for (size_t w = 0; w < words; w++) next_in[w] |= pred_out[w];
        }
        memcpy(next_out, next_in, words * sizeof(uint64_t));
        for (size_t e = 0; e < n->flow.len; e++) {
            BorrowFlowEvent *event = n->flow.items[e];
            if (event->kind == FLOW_MOVE) {
                int bit = move_place_index(&places, event->var, event->place,
                                           root_heads, uid_count);
                if (bit >= 0) live_set(next_out, (size_t)bit);
            } else if (event->kind == FLOW_INIT && !event->alternatives.len) {
                if (event->var->uid >= 0 &&
                    (size_t)event->var->uid < uid_count) {
                    for (int m = root_heads[event->var->uid]; m >= 0;
                         m = ((MovePlace *)places.items[m])->next_root) {
                        MovePlace *moved = places.items[m];
                        if (place_prefix(moved->root, event->place, moved->place))
                            live_clear(next_out, (size_t)m);
                    }
                } else {
                    for (size_t m = 0; m < places.len; m++) {
                        MovePlace *moved = places.items[m];
                        if (same_var_slot(moved->root, event->var) &&
                            place_prefix(moved->root, event->place, moved->place))
                            live_clear(next_out, m);
                    }
                }
            }
        }
        uint64_t *node_in = &ins[(size_t)id * words];
        uint64_t *node_out = &outs[(size_t)id * words];
        bool in_changed = memcmp(node_in, next_in, words * sizeof(uint64_t)) != 0;
        bool out_changed = memcmp(node_out, next_out, words * sizeof(uint64_t)) != 0;
        if (in_changed) memcpy(node_in, next_in, words * sizeof(uint64_t));
        if (out_changed) memcpy(node_out, next_out, words * sizeof(uint64_t));
        if (out_changed) {
            for (size_t s = 0; s < n->succ.len; s++) {
                int succ = (int)(intptr_t)n->succ.items[s] - 1;
                if (succ < 0 || queued[succ]) continue;
                queue[tail] = succ; tail = (tail + 1) % count;
                queued[succ] = true; pending++;
            }
        }
    }
    /* A place can be reinitialized after it was moved, but only if all of
     * its parents still exist. Writing `x.field` cannot resurrect `x` after
     * the whole `x` was moved, and writing `x.field.child` cannot proceed if
     * `x.field` itself was moved. Without this check generated field writes
     * may dereference the null slot left by hp_move_ptr(). */
    for (size_t i = 0; i < count; i++) {
        BorrowNode *n = node_at(cfg, (int)i);
        for (size_t e = 0; e < n->flow.len; e++) {
            BorrowFlowEvent *init = n->flow.items[e];
            if (init->kind != FLOW_INIT || !init->var) continue;
            bool indexed_root = init->var->uid >= 0 &&
                (size_t)init->var->uid < uid_count;
            int root_head = indexed_root ? root_heads[init->var->uid] : -1;
            if (indexed_root) {
                for (int m = root_head; m >= 0;
                     m = ((MovePlace *)places.items[m])->next_root)
                    check_parent_move_candidate(places.items[m], (size_t)m, init,
                                                n, e, i, words, ins);
            } else {
                for (size_t m = 0; m < places.len; m++) {
                    MovePlace *moved = places.items[m];
                    if (!same_var_slot(moved->root, init->var)) continue;
                    check_parent_move_candidate(moved, m, init, n, e, i,
                                                words, ins);
                }
            }
        }
    }
    for (size_t i = 0; i < count; i++) {
        BorrowNode *n = node_at(cfg, (int)i);
        for (size_t e = 0; e < n->flow.len; e++) {
            BorrowFlowEvent *use = n->flow.items[e];
            if (use->kind != FLOW_USE) continue;
            MovePlace *moved_by = NULL;
            bool indexed_root = use->var->uid >= 0 &&
                (size_t)use->var->uid < uid_count;
            int root_head = indexed_root ? root_heads[use->var->uid] : -1;
            for (int mi = indexed_root ? root_head : 0;
                 indexed_root ? mi >= 0 : (size_t)mi < places.len;
                 mi = indexed_root
                     ? ((MovePlace *)places.items[mi])->next_root : mi + 1) {
                size_t m = (size_t)mi;
                MovePlace *candidate = places.items[m];
                if ((!indexed_root && !same_var_slot(candidate->root, use->var)) ||
                    !places_overlap(candidate, &(MovePlace){.root = use->var, .place = use->place})) continue;
                bool candidate_moved = live_test(&ins[i * words], m);
                for (size_t p = 0; p < e; p++) {
                    BorrowFlowEvent *prior = n->flow.items[p];
                    if (flow_events_exclusive(prior, use) ||
                        !same_var_slot(prior->var, candidate->root)) continue;
                    if (prior->kind == FLOW_MOVE && place_equal(candidate, prior))
                        candidate_moved = true;
                    else if (prior->kind == FLOW_INIT &&
                             place_prefix(candidate->root, prior->place, candidate->place))
                        candidate_moved = false;
                }
                if (candidate_moved) moved_by = candidate;
            }
            if (moved_by) {
                char used_desc[256], moved_desc[256];
                describe_place(use->var, use->place, used_desc, sizeof(used_desc));
                describe_place(moved_by->root, moved_by->place, moved_desc, sizeof(moved_desc));
                sema_error(&use->tok, "use of %s while overlapping place %s is moved",
                           used_desc, moved_desc);
                borrow_note(&moved_by->tok, "the overlapping place was moved here");
            }
        }
    }
    free(queue); free(queued); free(next_out); free(next_in);
    free(outs); free(ins);
    free(root_heads);
    for (size_t i = 0; i < places.len; i++) free(places.items[i]);
    free(places.items);
}

static void liveness_all(const BorrowCfg *cfg, uint64_t *in, uint64_t *out,
                         size_t words, const int *holder_by_uid,
                         size_t uid_count) {
    size_t count = cfg->nodes.len;
    if (!count) return;
    uint64_t *new_out = xcalloc(words, sizeof(uint64_t));
    uint64_t *new_in = xcalloc(words, sizeof(uint64_t));
    int *queue = xmalloc(count * sizeof(int));
    bool *queued = xcalloc(count, sizeof(bool));
    size_t head = 0, tail = 0, pending = 0;
    for (size_t i = 0; i < count; i++) {
        queue[tail] = (int)i;
        tail = (tail + 1) % count;
        queued[i] = true;
        pending++;
    }
    while (pending) {
        int id = queue[head];
        head = (head + 1) % count;
        pending--;
        queued[id] = false;
        BorrowNode *n = node_at((BorrowCfg *)cfg, id);
        memset(new_out, 0, words * sizeof(uint64_t));
        for (size_t j = 0; j < n->succ.len; j++) {
            int to = (int)(intptr_t)n->succ.items[j] - 1;
            if (to < 0) continue;
            const uint64_t *succ_in = &in[(size_t)to * words];
            for (size_t w = 0; w < words; w++) new_out[w] |= succ_in[w];
        }
        memcpy(new_in, new_out, words * sizeof(uint64_t));
        for (size_t j = 0; j < n->defs.len; j++) {
            VarSym *v = n->defs.items[j];
            if (v && v->uid >= 0 && (size_t)v->uid < uid_count &&
                holder_by_uid[v->uid] >= 0)
                live_clear(new_in, (size_t)holder_by_uid[v->uid]);
        }
        for (size_t j = 0; j < n->uses.len; j++) {
            VarSym *v = n->uses.items[j];
            if (v && v->uid >= 0 && (size_t)v->uid < uid_count &&
                holder_by_uid[v->uid] >= 0)
                live_set(new_in, (size_t)holder_by_uid[v->uid]);
        }
        uint64_t *node_in = &in[(size_t)id * words];
        uint64_t *node_out = &out[(size_t)id * words];
        bool in_changed = memcmp(node_in, new_in, words * sizeof(uint64_t)) != 0;
        bool out_changed = memcmp(node_out, new_out, words * sizeof(uint64_t)) != 0;
        if (in_changed) memcpy(node_in, new_in, words * sizeof(uint64_t));
        if (out_changed) memcpy(node_out, new_out, words * sizeof(uint64_t));
        if (in_changed) {
            for (size_t j = 0; j < n->pred.len; j++) {
                int pred = (int)(intptr_t)n->pred.items[j] - 1;
                if (pred < 0 || queued[pred]) continue;
                queue[tail] = pred;
                tail = (tail + 1) % count;
                queued[pred] = true;
                pending++;
            }
        }
    }
    free(queued);
    free(queue);
    free(new_in);
    free(new_out);
}

static bool fn_captures_symbol(const AstFn *fn, const VarSym *sym) {
    if (!fn || !sym) return false;
    const VarSym *identity = sym->alias_origin ? sym->alias_origin : sym;
    for (size_t i = 0; i < fn->captures.len; i++) {
        const VarSym *cap = fn->captures.items[i];
        if (cap == sym || cap == identity || cap->alias_origin == identity) return true;
    }
    return false;
}

static bool escape_may_alias(const BorrowWrite *escape, const VarSym *origin) {
    if (!escape || !origin) return false;
    if (escape->unknown_object) {
        /* A retained closure value cannot capture a local created in the
         * receiving function. Its environment was formed in the caller. Do
         * not treat an opaque callback as an alias of every local in this
         * frame; parameters and globals remain conservative because they can
         * already be shared with that caller. Explicit captures are tracked
         * at their creation site by sema's captured_heap_alias marker. */
        if (!origin->is_param && !global_storage(origin)) return false;
        return !escape->object_type || escape->object_type->kind == TY_MIXED ||
               (origin->type && origin->type->kind == TY_MIXED) ||
               heap_types_related(origin->type, escape->object_type);
    }
    if (!escape->origin) return false;
    const VarSym *escape_identity = escape->origin->alias_origin
        ? escape->origin->alias_origin : escape->origin;
    const VarSym *origin_identity = origin->alias_origin ? origin->alias_origin : origin;
    if (same_global(escape_identity, origin_identity)) return true;
    if (escape_identity == origin_identity) return true;
    /* A callable held in a parameter (or a global) may have been created by
     * the caller and may retain aliases shared with that caller. A closure's
     * capture environment cannot name an unrelated local introduced later in
     * this function, even when its opaque type is `mixed` or `callable`. */
    if (!origin->is_param && !global_storage(origin)) return false;
    return escape->may_alias && escape->object_type &&
           (origin->is_param || origin->alias_unknown ||
            escape->origin->is_param || escape->origin->alias_unknown) &&
           heap_types_related(origin->type, escape->object_type);
}

static void cfg_mark_reachable(const BorrowCfg *cfg, int from, bool *seen, int *queue) {
    memset(seen, 0, cfg->nodes.len * sizeof(bool));
    size_t head = 0, tail = 0;
    seen[from] = true;
    queue[tail++] = from;
    while (head < tail) {
        int at = queue[head++];
        BorrowNode *n = node_at((BorrowCfg *)cfg, at);
        for (size_t i = 0; i < n->succ.len; i++) {
            int next = (int)(intptr_t)n->succ.items[i] - 1;
            if (next < 0 || seen[next]) continue;
            seen[next] = true;
            queue[tail++] = next;
        }
    }
}

static void check_closure_escapes(BorrowCfg *cfg, AstFn *fn) {
    size_t count = cfg->nodes.len;
    bool *seen = xcalloc(count ? count : 1, sizeof(bool));
    int *queue = xmalloc((count ? count : 1) * sizeof(int));
    for (size_t i = 0; i < count; i++) {
        BorrowNode *from = node_at(cfg, (int)i);
        bool has_escape = false;
        for (size_t wi = 0; wi < from->writes.len; wi++) {
            BorrowWrite *escape = from->writes.items[wi];
            if (escape->closure_escape_arg) { has_escape = true; break; }
        }
        if (!has_escape) continue;
        /* All closure escapes at one CFG node share the same future. Compute
         * its reachable set once instead of running a BFS for every pair of
         * escape and destination node. */
        cfg_mark_reachable(cfg, (int)i, seen, queue);
        for (size_t wi = 0; wi < from->writes.len; wi++) {
            BorrowWrite *escape = from->writes.items[wi];
            if (!escape->closure_escape_arg) continue;
            const Token *escape_tok = from->stmt ? &from->stmt->tok :
                (escape->origin ? &escape->origin->tok : NULL);
            for (size_t j = 0; j < count; j++) {
                if (!seen[j]) continue;
                BorrowNode *to = node_at(cfg, (int)j);
                for (size_t b = 0; b < to->borrows.len; b++) {
                    BorrowEvent *ev = to->borrows.items[b];
                    if (escape_may_alias(escape, ev->origin)) {
                        sema_error(&ev->tok,
                                   "cannot borrow $%s after it escapes into a closure",
                                   ev->origin->name);
                        borrow_note(escape_tok, "the closure receives this object here");
                    }
                }
                for (size_t w = 0; w < to->writes.len; w++) {
                    BorrowWrite *write = to->writes.items[w];
                    if (write == escape || write->closure_escape_arg ||
                        fn_captures_symbol(fn, write->origin)) continue;
                    if (escape_may_alias(escape, write->origin)) {
                        sema_error(to->stmt ? &to->stmt->tok : escape_tok,
                                   "cannot mutate $%s after it escapes into a closure",
                                   write->origin->name);
                        borrow_note(escape_tok, "the closure receives this object here");
                    }
                }
            }
        }
    }
    free(queue);
    free(seen);
}

static void check_cfg(BorrowCfg *cfg, AstFn *fn) {
    check_moved_values(cfg);
    PtrVec holders = {0};
    size_t count = cfg->nodes.len;
    size_t uid_count = 1;
    for (size_t i = 0; i < cfg->nodes.len; i++) {
        BorrowNode *n = cfg->nodes.items[i];
        for (size_t j = 0; j < n->uses.len; j++) {
            VarSym *v = n->uses.items[j];
            if (v && v->borrow_origin && v->uid >= 0 &&
                (size_t)v->uid >= uid_count) uid_count = (size_t)v->uid + 1;
        }
        for (size_t j = 0; j < n->defs.len; j++) {
            VarSym *v = n->defs.items[j];
            if (v && v->borrow_origin && v->uid >= 0 &&
                (size_t)v->uid >= uid_count) uid_count = (size_t)v->uid + 1;
        }
    }
    int *holder_by_uid = xmalloc(uid_count * sizeof(int));
    for (size_t i = 0; i < uid_count; i++) holder_by_uid[i] = -1;
    for (size_t i = 0; i < cfg->nodes.len; i++) {
        BorrowNode *n = cfg->nodes.items[i];
        for (size_t j = 0; j < n->uses.len; j++) {
            VarSym *v = n->uses.items[j];
            if (v && v->borrow_origin)
                add_indexed_holder(&holders, v, holder_by_uid, uid_count);
        }
        for (size_t j = 0; j < n->defs.len; j++) {
            VarSym *v = n->defs.items[j];
            if (v && v->borrow_origin)
                add_indexed_holder(&holders, v, holder_by_uid, uid_count);
        }
    }
    size_t words = (holders.len + 63) / 64;
    if (!words) words = 1;
    uint64_t *lins = xcalloc((count ? count : 1) * words, sizeof(uint64_t));
    uint64_t *louts = xcalloc((count ? count : 1) * words, sizeof(uint64_t));
    liveness_all(cfg, lins, louts, words, holder_by_uid, uid_count);
    check_closure_escapes(cfg, fn);
    for (size_t i = 0; i < count; i++) {
        BorrowNode *n = cfg->nodes.items[i];
        for (size_t a = 0; a < n->borrows.len; a++) {
            BorrowEvent *ev = n->borrows.items[a];
            for (size_t h = 0; h < holders.len; h++) {
                VarSym *holder = holders.items[h];
                if (holder == ev->holder || !live_test(&lins[i * words], h) ||
                    !borrowed_places_overlap(holder->borrow_origin, holder->borrow_place,
                                             ev->origin, ev->place)) continue;
                if (borrow_ancestor(holder, ev->parent)) continue;
                if (ev->is_mut || holder->borrow_mut)
                {
                    sema_error(&ev->tok,
                               "conflicting %s borrow of $%s: $%s still holds a live %s borrow",
                               ev->is_mut ? "mutable" : "shared", ev->origin->name,
                               holder->name, holder->borrow_mut ? "mutable" : "shared");
                    borrow_note(&holder->tok, "this binding keeps the earlier borrow live");
                }
            }
            for (size_t b = 0; b < a; b++) {
                BorrowEvent *other = n->borrows.items[b];
                if (alternatives_exclusive(other, ev)) continue;
                bool distinct_scalar_slots = other->slot_only && ev->slot_only &&
                    other->origin != ev->origin &&
                    !other->origin->is_param && !ev->origin->is_param &&
                    !other->origin->alias_unknown && !ev->origin->alias_unknown &&
                    !other->origin->alias_origin && !ev->origin->alias_origin &&
                    !same_global(other->origin, ev->origin);
                if (!distinct_scalar_slots &&
                    borrowed_places_overlap(other->origin, other->place,
                                            ev->origin, ev->place) &&
                    (other->is_mut || ev->is_mut)) {
                    sema_error(&ev->tok,
                               "conflicting borrows of $%s in one expression",
                               ev->origin->name);
                    borrow_note(&other->tok, "the other borrow starts here");
                }
            }
            if (ev->is_mut) {
                for (size_t u = ev->flow_index; u < n->flow.len; u++) {
                    BorrowFlowEvent *use = n->flow.items[u];
                    if (use->kind != FLOW_USE || use->borrow_source ||
                        use->reborrow_source || borrow_flow_exclusive(ev, use) ||
                        !borrowed_places_overlap(ev->origin, ev->place,
                                                 use->var, use->place)) continue;
                    sema_error(&use->tok,
                               "cannot access $%s while an exclusive borrow is active in this expression",
                               ev->origin->name);
                    borrow_note(&ev->tok, "the exclusive borrow starts here");
                }
            }
            /* Writes from a later subexpression of the same full expression
             * must conflict with this temporary loan too. Branch tags keep
             * mutually exclusive arms from being compared as one path. */
            {
                for (size_t w = 0; w < n->writes.len; w++) {
                    BorrowWrite *write = n->writes.items[w];
                    if (write->flow_index < ev->flow_index ||
                        borrow_write_exclusive(ev, write)) continue;
                    bool overlaps = borrowed_places_overlap(
                        ev->origin, ev->place, write->origin, write->place);
                    if (write->unknown_global &&
                        (global_storage(ev->origin) ||
                         global_storage(ev->origin->alias_origin)))
                        overlaps = true;
                    if (write->unknown_object &&
                        place_paths_overlap(ev->place, write->place) &&
                        (write->object_type->kind == TY_MIXED ||
                         (ev->origin->type && ev->origin->type->kind == TY_MIXED) ||
                         heap_types_related(ev->origin->type, write->object_type)))
                        overlaps = true;
                    if (!overlaps) continue;
                    sema_error(&n->stmt->tok,
                               "cannot write $%s while a %s borrow is active in this expression",
                               ev->origin->name, ev->is_mut ? "mutable" : "shared");
                    borrow_note(&ev->tok, "the borrow starts here");
                }
            }
        }
        for (size_t u = 0; u < n->flow.len; u++) {
            BorrowFlowEvent *use = n->flow.items[u];
            if (use->kind != FLOW_USE || use->reborrow_source) continue;
            const VarSym *used = use->var;
            for (size_t h = 0; h < holders.len; h++) {
                VarSym *child = holders.items[h];
                bool suspends_parent = child->borrow_mut ||
                    (used->type && used->type->kind == TY_MUTREF);
                if (!suspends_parent || !borrow_ancestor(used, child) ||
                    same_var_slot(child, used) || !live_test(&lins[i * words], h))
                    continue;
                const VarSym *parent_origin = used->borrow_origin
                    ? used->borrow_origin : used;
                const Expr *parent_place = use->place;
                if (used->borrow_origin && used->borrow_place) {
                    parent_place = use->place && root_var(use->place) == used
                        ? compose_borrow_place(cfg, used->borrow_place, use->place)
                        : used->borrow_place;
                }
                if (borrowed_places_overlap(child->borrow_origin, child->borrow_place,
                                            parent_origin, parent_place)) {
                    sema_error(&use->tok,
                               "cannot use reborrowed reference $%s while child borrow $%s is live",
                               used->name, child->name);
                    borrow_note(&child->tok, "this binding holds the child reborrow");
                }
            }
            if (use->borrow_source) continue; /* the BorrowEvent reports conflicts at loan creation */
            for (size_t h = 0; h < holders.len; h++) {
                VarSym *holder = holders.items[h];
                if (!holder->borrow_mut || !live_test(&lins[i * words], h) ||
                    same_var_slot(holder, used) || borrow_ancestor(holder, used) ||
                    borrow_ancestor(used, holder) ||
                    !borrowed_places_overlap(holder->borrow_origin, holder->borrow_place,
                                             used, use->place)) continue;
                char accessed[256], borrowed[256];
                describe_place(used, use->place, accessed, sizeof(accessed));
                describe_place(holder->borrow_origin, holder->borrow_place,
                               borrowed, sizeof(borrowed));
                sema_error(&use->tok,
                           "cannot access %s while $%s holds a mutable borrow of %s",
                           accessed, holder->name, borrowed);
                borrow_note(&holder->tok, "this binding keeps the mutable borrow live");
            }
        }
        for (size_t w = 0; w < n->writes.len; w++) {
            BorrowWrite *write = n->writes.items[w];
            if (write->shared_receiver_violation)
                sema_error(&write->call_tok,
                           "cannot call a method that mutates its receiver through a shared reference");
            for (size_t h = 0; h < holders.len; h++) {
                VarSym *holder = holders.items[h];
                bool paths_overlap = place_paths_overlap(holder->borrow_place, write->place);
                if (live_test(&lins[i * words], h) &&
                    borrow_ancestor(holder, write->through))
                    continue; /* the write is authorized through this loan or its child */
                if (live_test(&lins[i * words], h) &&
                    ((write->unknown_global &&
                      (global_storage(holder->borrow_origin) ||
                       global_storage(holder->borrow_origin->alias_origin))) ||
                     (write->unknown_object && paths_overlap &&
                      (write->object_type->kind == TY_MIXED ||
                       (holder->borrow_origin->type &&
                        holder->borrow_origin->type->kind == TY_MIXED) ||
                       heap_types_related(holder->borrow_origin->type,
                                          write->object_type))) ||
                     borrowed_places_overlap(holder->borrow_origin, holder->borrow_place,
                                             write->origin, write->place) ||
                     (write->object_type && write->object_type->kind == TY_MIXED && paths_overlap) ||
                     (holder->borrow_origin->type && paths_overlap &&
                      holder->borrow_origin->type->kind == TY_MIXED && write->object_type) ||
                     (write->object_type && paths_overlap &&
                      heap_types_related(holder->borrow_origin->type, write->object_type) &&
                      (write->may_alias || holder->borrow_origin->is_param ||
                       holder->borrow_origin->alias_unknown))))
                {
                    if (write->unknown_global) {
                        sema_error(&n->stmt->tok,
                                   "cannot prove this call preserves a borrowed global ($%s)",
                                   holder->borrow_origin->name);
                    } else if (write->unknown_object) {
                        sema_error(&n->stmt->tok,
                                   "cannot prove this call preserves a borrowed heap object through $%s",
                                   holder->name);
                    } else {
                        sema_error(&n->stmt->tok,
                                   "cannot mutate $%s while a %s borrow through $%s is live",
                                   write->origin->name,
                                   holder->borrow_mut ? "mutable" : "shared",
                                   holder->name);
                    }
                    borrow_note(&holder->tok, "this binding keeps the borrow live at the write");
                }
            }
        }
    }
    free(lins);
    free(louts);
    free(holder_by_uid);
    free(holders.items);
}

static void cfg_dispose(BorrowCfg *cfg) {
    for (size_t i = 0; i < cfg->nodes.len; i++) {
        BorrowNode *n = cfg->nodes.items[i];
        for (size_t j = 0; j < n->borrows.len; j++) {
            BorrowEvent *ev = n->borrows.items[j];
            for (size_t k = 0; k < ev->alternatives.len; k++) free(ev->alternatives.items[k]);
            free(ev->alternatives.items);
            free(ev);
        }
        for (size_t j = 0; j < n->writes.len; j++) {
            BorrowWrite *write = n->writes.items[j];
            for (size_t k = 0; k < write->alternatives.len; k++)
                free(write->alternatives.items[k]);
            free(write->alternatives.items);
            free(write);
        }
        free(n->succ.items);
        free(n->pred.items);
        free(n->uses.items);
        free(n->defs.items);
        for (size_t j = 0; j < n->flow.len; j++) {
            BorrowFlowEvent *event = n->flow.items[j];
            for (size_t k = 0; k < event->alternatives.len; k++) free(event->alternatives.items[k]);
            free(event->alternatives.items);
            free(event);
        }
        free(n->flow.items);
        free(n->writes.items);
        free(n->borrows.items);
        free(n);
    }
    for (size_t i = 0; i < cfg->active_alternatives.len; i++)
        free(cfg->active_alternatives.items[i]);
    free(cfg->active_alternatives.items);
    for (size_t i = 0; i < cfg->synthetic_places.len; i++)
        free(cfg->synthetic_places.items[i]);
    free(cfg->synthetic_places.items);
    free(cfg->nodes.items);
}

static void cfg_build_function(BorrowCfg *cfg, AstFn *fn) {
    cfg->exit = node_new(cfg, NULL);
    LoopTargets targets = {0};
    (void)cfg_build_seq(cfg, fn->body, cfg->exit, &targets);
}

static bool mark_escaping_params(AstFn *fn, BorrowCfg *cfg) {
    bool changed = false;
    for (size_t i = 0; i < cfg->nodes.len; i++) {
        BorrowNode *n = cfg->nodes.items[i];
        for (size_t w = 0; w < n->writes.len; w++) {
            BorrowWrite *escape = n->writes.items[w];
            if (!escape->closure_escape_arg) continue;
            for (size_t p = 0; p < fn->params.len; p++) {
                Param *pm = fn->params.items[p];
                VarSym *param = pm->sym;
                if (!param || pm->captures_heap_alias) continue;
                if (escape->unknown_object) {
                    bool uncertain = !escape->object_type ||
                        escape->object_type->kind == TY_MIXED ||
                        (param->type && param->type->kind == TY_MIXED) ||
                        heap_types_related(param->type, escape->object_type);
                    if (uncertain) {
                        pm->captures_heap_alias = true;
                        changed = true;
                    }
                    continue;
                }
                if (!escape->origin) continue;
                const VarSym *identity = escape->origin->alias_origin
                    ? escape->origin->alias_origin : escape->origin;
                bool exact = param == escape->origin || param == identity ||
                             param->alias_origin == identity;
                bool uncertain = escape->may_alias &&
                    (escape->origin->alias_unknown || param->alias_unknown || param->is_param) &&
                    heap_types_related(param->type, escape->object_type);
                if (exact || uncertain) {
                    pm->captures_heap_alias = true;
                    changed = true;
                }
            }
        }
    }
    return changed;
}

static bool add_global_write_summary(AstFn *fn, const VarSym *global) {
    if (!fn || !global_storage(global)) return false;
    for (size_t i = 0; i < fn->borrow_global_writes.len; i++)
        if (same_global(fn->borrow_global_writes.items[i], global)) return false;
    ptrvec_push(&fn->borrow_global_writes, (void *)global);
    return true;
}

static bool add_global_escape_summary(AstFn *fn, const VarSym *global) {
    if (!fn || !global_storage(global)) return false;
    for (size_t i = 0; i < fn->borrow_global_escapes.len; i++)
        if (same_global(fn->borrow_global_escapes.items[i], global)) return false;
    ptrvec_push(&fn->borrow_global_escapes, (void *)global);
    return true;
}

static bool infer_global_writes(AstFn *fn, BorrowCfg *cfg) {
    bool changed = false;
    for (size_t i = 0; i < cfg->nodes.len; i++) {
        BorrowNode *n = cfg->nodes.items[i];
        for (size_t j = 0; j < n->writes.len; j++) {
            BorrowWrite *w = n->writes.items[j];
            if (w->unknown_global && !fn->borrow_unknown_global_write) {
                fn->borrow_unknown_global_write = true;
                changed = true;
            }
            if (global_storage(w->origin) && add_global_write_summary(fn, w->origin))
                changed = true;
            if (w->closure_escape_arg && global_storage(w->origin) &&
                add_global_escape_summary(fn, w->origin))
                changed = true;
        }
    }
    return changed;
}

static bool add_heap_escape_summary(AstFn *fn, const Type *type) {
    if (!fn || !type) return false;
    for (size_t i = 0; i < fn->borrow_heap_escapes.len; i++)
        if (fn->borrow_heap_escapes.items[i] == type) return false;
    ptrvec_push(&fn->borrow_heap_escapes, (void *)type);
    return true;
}

static bool infer_heap_escapes(AstFn *fn, BorrowCfg *cfg) {
    bool changed = false;
    for (size_t i = 0; i < cfg->nodes.len; i++) {
        BorrowNode *n = cfg->nodes.items[i];
        for (size_t j = 0; j < n->writes.len; j++) {
            BorrowWrite *write = n->writes.items[j];
            if (write->closure_escape_arg && write->unknown_object &&
                add_heap_escape_summary(fn, write->object_type))
                changed = true;
        }
    }
    return changed;
}

static bool infer_heap_mutations(AstFn *fn, BorrowCfg *cfg) {
    bool changed = false;
    VarSym *receiver = fn->this_sym;
    const VarSym *receiver_identity = receiver && receiver->alias_origin
        ? receiver->alias_origin : receiver;
    for (size_t i = 0; i < cfg->nodes.len; i++) {
        BorrowNode *n = cfg->nodes.items[i];
        for (size_t j = 0; j < n->writes.len; j++) {
            BorrowWrite *write = n->writes.items[j];
            if (!write->origin || !write->object_type || write->unknown_object ||
                write->unknown_global) continue;
            const VarSym *identity = write->origin->alias_origin
                ? write->origin->alias_origin : write->origin;
            if (receiver && !fn->borrow_mutates_receiver &&
                (receiver == write->origin || receiver == identity ||
                 receiver_identity == identity)) {
                fn->borrow_mutates_receiver = true;
                changed = true;
            }
            for (size_t p = 0; p < fn->params.len; p++) {
                Param *pm = fn->params.items[p];
                VarSym *param = pm ? pm->sym : NULL;
                if (!param || pm->mutates_heap_alias) continue;
                const VarSym *param_identity = param->alias_origin
                    ? param->alias_origin : param;
                if (param == write->origin || param == identity ||
                    param_identity == identity) {
                    pm->mutates_heap_alias = true;
                    changed = true;
                }
            }
        }
    }
    return changed;
}

void borrow_infer_summaries(PtrVec fns) {
    /* These summaries depend on one another: wrapper calls propagate global
     * writes, heap mutations, and closure escapes. Solve them together so
     * every iteration builds each function CFG only once. All effects grow
     * monotonically, so recursive call graphs reach a fixed point. */
    g_borrow_functions = fns;
    for (size_t pass = 0; pass <= fns.len; pass++) {
        bool changed = false;
        for (size_t i = 0; i < fns.len; i++) {
            AstFn *fn = fns.items[i];
            if (!fn || !fn->body.len) continue;
            BorrowCfg cfg = {0};
            cfg_build_function(&cfg, fn);
            if (infer_global_writes(fn, &cfg)) changed = true;
            if (mark_escaping_params(fn, &cfg)) changed = true;
            if (infer_heap_escapes(fn, &cfg)) changed = true;
            if (infer_heap_mutations(fn, &cfg)) changed = true;
            cfg_dispose(&cfg);
        }
        if (!changed) break;
    }
}

void borrow_check_function(AstFn *fn) {
    if (!fn || !fn->body.len) return;
    BorrowCfg cfg = {0};
    cfg_build_function(&cfg, fn);
    check_cfg(&cfg, fn);
    cfg_dispose(&cfg);
}

void borrow_check_script(PtrVec stmts) {
    if (!stmts.len) return;
    BorrowCfg cfg = {0};
    cfg.exit = node_new(&cfg, NULL);
    LoopTargets targets = {0};
    (void)cfg_build_seq(&cfg, stmts, cfg.exit, &targets);
    check_cfg(&cfg, NULL);
    cfg_dispose(&cfg);
}

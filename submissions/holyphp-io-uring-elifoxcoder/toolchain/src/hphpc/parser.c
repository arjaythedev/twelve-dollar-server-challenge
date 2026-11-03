/* parser.c — recursive descent parser for HolyPHP.
 *
 * Precedence (low → high):
 *   assignment  =  +=  -=  *=  /=  %=  .=  **=   (right assoc)
 *   ternary  ? :   /  ??  /  ?:
 *   ||   &&
 *   |    ^    &
 *   ==  !=  ===  !==  <=>
 *   <  >  <=  >=
 *   .  (string concat)
 *   +  -
 *   *  /  %
 *   **  (pow, right assoc)
 *   unary  !  -  +  &  &mut  *
 *   postfix  []  ->  ::  ()  as  is
 */
#include "parser.h"
#include <ctype.h>

static Expr *parse_expr(Parser *p);
static Expr *parse_assign(Parser *p);
static Expr *parse_ternary(Parser *p);
static Expr *parse_binop_level(Parser *p, int lvl);
static Expr *parse_equality(Parser *p);
static Expr *parse_concat(Parser *p);
static Expr *parse_additive(Parser *p);
static Expr *parse_multiplicative(Parser *p);
static Expr *parse_pow(Parser *p);
static Expr *parse_unary(Parser *p);
static Expr *parse_postfix(Parser *p);
static Expr *parse_primary(Parser *p);
static Stmt *parse_stmt(Parser *p);
static Type *parse_type(Parser *p);
static AstFn *parse_fn_after_name(Parser *p, const Token *name, bool is_method, bool is_static);
static void parse_class_body(Parser *p, AstClass *c);
static Decl *parse_decl(Parser *p);

const char *dup_text(const Token *t) {
    return xstrndup(t->text ? t->text : "", t->len);
}

static void skip_nl(Parser *p) {
    while (ts_check(p->ts, T_NEWLINE)) ts_advance(p->ts);
}
static void expect_nl_or_semi(Parser *p) {
    skip_nl(p);
    if (ts_match(p->ts, T_SEMI)) skip_nl(p);
}

Parser *parser_new(PtrVec toks, const char *file) {
    Parser *p = xcalloc(1, sizeof(Parser));
    p->ts = xmalloc(sizeof(TokenStream));
    ts_init(p->ts, toks, file);
    p->file = file;
    return p;
}

/* --- newline handling -------------------------------------------------
 * The lexer emits a T_NEWLINE for every '\n'. PHP lets an expression span
 * lines, so a newline that cannot *terminate* a statement is dropped here:
 *   - anywhere inside ( ) or [ ]      (argument lists, array literals)
 *   - right after an operator/comma   ($x = 1 +\n    2;)
 *   - right before a token that only makes sense to the left of it
 *                                     ($x = foo()\n ->bar();)
 * Newlines inside { } stay, because they separate statements there.
 */
static bool tok_needs_operand(TokKind k) {
    switch (k) {
    case T_LPAREN: case T_LBRACKET: case T_COMMA:
    case T_COLON: case T_COLONCOLON: case T_ARROW:
    case T_DOUBLEARROW: case T_FATARROW:
    case T_PLUS: case T_MINUS: case T_STAR: case T_SLASH: case T_PERCENT:
    case T_POW: case T_DOT: case T_SHL: case T_SHR:
    case T_ASSIGN: case T_PLUSASSIGN: case T_MINUSASSIGN: case T_STARASSIGN:
    case T_SLASHASSIGN: case T_PERCENTASSIGN: case T_DOTASSIGN: case T_POWASSIGN:
    case T_EQ: case T_NEQ: case T_NEQ2: case T_EQ2: case T_SPACESHIP:
    case T_LT: case T_GT: case T_LE: case T_GE:
    case T_ANDAND: case T_OROR: case T_AMP: case T_PIPE: case T_CARET:
    case T_TILDE: case T_NOT: case T_QUESTION: case T_QUESTIONQUESTION:
    case T_KW_ECHO: case T_KW_PRINT: case T_HPHP_THROW: case T_KW_NEW:
    case T_KW_IF: case T_KW_ELSEIF: case T_KW_WHILE: case T_KW_FOR:
    case T_KW_FOREACH: case T_KW_SWITCH: case T_KW_TRY: case T_HPHP_FINALLY:
    case T_KW_DO: case T_KW_MATCH: case T_KW_AS: case T_KW_USE: case T_KW_ELSE:
        return true;
    default:
        return false;
    }
}

/* tokens that bind to the expression on their left */
static bool tok_continues_with(TokKind k) {
    switch (k) {
    case T_RPAREN: case T_RBRACKET: case T_COMMA: case T_SEMI:
    case T_ARROW: case T_COLONCOLON: case T_DOUBLEARROW: case T_FATARROW:
    case T_COLON: case T_QUESTION: case T_QUESTIONQUESTION:
    case T_PLUS: case T_MINUS: case T_STAR: case T_SLASH: case T_PERCENT:
    case T_POW: case T_DOT: case T_SHL: case T_SHR:
    case T_EQ: case T_NEQ: case T_NEQ2: case T_EQ2: case T_SPACESHIP:
    case T_LT: case T_GT: case T_LE: case T_GE:
    case T_ANDAND: case T_OROR: case T_AMP: case T_PIPE: case T_CARET:
        return true;
    default:
        return false;
    }
}

static PtrVec lex_all(const char *src, const char *file) {
    Lexer lx;
    lex_init(&lx, src, file);
    Token t;
    PtrVec raw = {0};
    while (lex_next(&lx, &t)) {
        Token *copy = xmalloc(sizeof(Token));
        *copy = t;
        ptrvec_push(&raw, copy);
    }
    Token *eof = xcalloc(1, sizeof(Token));
    eof->kind = T_EOF;
    eof->line = t.line;
    eof->file = file;
    ptrvec_push(&raw, eof);

    /* drop newlines that merely continue an expression */
    PtrVec toks = {0};
    char stack[256];
    int depth = 0;
    TokKind prev = T_EOF;
    for (size_t i = 0; i < raw.len; i++) {
        Token *tk = raw.items[i];
        TokKind k = tk->kind;
        if (k == T_LPAREN || k == T_LBRACKET) {
            if (depth < (int)sizeof stack) stack[depth++] = 'p';
        } else if (k == T_LBRACE) {
            if (depth < (int)sizeof stack) stack[depth++] = 'b';
        } else if (k == T_RPAREN || k == T_RBRACKET) {
            if (depth > 0 && stack[depth - 1] == 'p') depth--;
        } else if (k == T_RBRACE) {
            if (depth > 0 && stack[depth - 1] == 'b') depth--;
        }
        if (k == T_NEWLINE) {
            TokKind next = T_EOF;
            for (size_t j = i + 1; j < raw.len; j++) {
                TokKind nk = ((Token *)raw.items[j])->kind;
                if (nk != T_NEWLINE) { next = nk; break; }
            }
            bool in_brackets = depth > 0 && stack[depth - 1] == 'p';
            if (in_brackets || tok_needs_operand(prev) || tok_continues_with(next))
                continue; /* continuation: swallowed */
            ptrvec_push(&toks, tk);
            prev = T_NEWLINE;
            continue;
        }
        ptrvec_push(&toks, tk);
        prev = k;
    }
    return toks;
}

Program *parse_program(const char *src, const char *filename) {
    Parser *p = parser_new(lex_all(src, filename), filename);
    Program *prog = xcalloc(1, sizeof(Program));
    prog->file = intern(filename);

    skip_nl(p);
    while (!ts_check(p->ts, T_EOF)) {
        if (ts_check(p->ts, T_NEWLINE) || ts_check(p->ts, T_SEMI)) { ts_advance(p->ts); continue; }
        if (ts_check(p->ts, T_KW_IMPORT)) {
            Token it = *ts_advance(p->ts);
            const Token *pth = ts_expect(p->ts, T_STR, "import path");
            Import *im = xcalloc(1, sizeof(Import));
            im->tok = it;
            im->path = dup_text(pth);
            ptrvec_push(&prog->imports, im);
            expect_nl_or_semi(p);
            continue;
        }
        if (ts_check(p->ts, T_KW_FN) || ts_check(p->ts, T_KW_FUNCTION) ||
            ts_check(p->ts, T_KW_CLASS) ||
            ts_check(p->ts, T_KW_INTERFACE) || ts_check(p->ts, T_KW_TRAIT) ||
            ts_check(p->ts, T_KW_ENUM)) {
            Decl *d = parse_decl(p);
            ptrvec_push(&prog->decls, d);
            continue;
        }
        ptrvec_push(&prog->stmts, parse_stmt(p));
    }
    return prog;
}

Expr *parse_embedded_expr(const char *src, const char *file) {
    Parser *p = parser_new(lex_all(src, file), file);
    Expr *e = parse_expr(p);
    return e;
}

static bool at_ident(Parser *p, const char *kw) {
    const Token *t = ts_peek(p->ts);
    return t->kind == T_IDENT && t->len == strlen(kw) && memcmp(t->text, kw, t->len) == 0;
}
/* contextual keyword: also accept the reserved keyword form (e.g. "extends"
 * lexed as T_KW_EXTENDS) so class headers read like PHP */
static bool at_kw_ident(Parser *p, const char *kw, TokKind k) {
    return at_ident(p, kw) || ts_check(p->ts, k);
}
static bool eat_ident_or_kw(Parser *p, const char *kw, TokKind k) {
    if (at_kw_ident(p, kw, k)) { ts_advance(p->ts); return true; }
    return false;
}
static bool eat_ident(Parser *p, const char *kw) {
    if (at_ident(p, kw)) { ts_advance(p->ts); return true; }
    return false;
}
/* contextual keyword: IN for foreach, AS, MUT as ident are handled inline */

static Decl *parse_decl(Parser *p) {
    Token start = *ts_peek(p->ts);
    Decl *d = xcalloc(1, sizeof(Decl));
    d->tok = start;
    if (ts_match(p->ts, T_KW_FN) || ts_match(p->ts, T_KW_FUNCTION)) {
        d->kind = DK_FN;
        const Token *nm = ts_expect(p->ts, T_IDENT, "function name");
        d->u.fn = parse_fn_after_name(p, nm, false, false);
        d->name = d->u.fn->name;
        expect_nl_or_semi(p);
        return d;
    }
    if (ts_match(p->ts, T_KW_CLASS)) {
        d->kind = DK_CLASS;
        const Token *nm = ts_expect(p->ts, T_IDENT, "class name");
        d->name = dup_text(nm);
        AstClass *c = xcalloc(1, sizeof(AstClass));
        c->name = d->name;
        c->name_tok = *nm;
        if (eat_ident_or_kw(p, "extends", T_KW_EXTENDS)) {
            const Token *b = ts_expect(p->ts, T_IDENT, "base class");
            c->extends = dup_text(b);
        }
        if (eat_ident_or_kw(p, "implements", T_KW_IMPLEMENTS)) {
            do {
                const Token *in = ts_expect(p->ts, T_IDENT, "interface name");
                ptrvec_push(&c->interfaces, (void *)dup_text(in));
            } while (ts_match(p->ts, T_COMMA));
        }
        parse_class_body(p, c);
        d->u.cls = c;
        return d;
    }
    if (ts_match(p->ts, T_KW_INTERFACE)) {
        d->kind = DK_INTERFACE;
        const Token *nm = ts_expect(p->ts, T_IDENT, "interface name");
        d->name = dup_text(nm);
        AstClass *c = xcalloc(1, sizeof(AstClass));
        c->name = d->name;
        c->name_tok = *nm;
        c->is_interface = true;
        if (eat_ident_or_kw(p, "extends", T_KW_EXTENDS)) {
            do {
                const Token *in = ts_expect(p->ts, T_IDENT, "interface name");
                ptrvec_push(&c->interfaces, (void *)dup_text(in));
            } while (ts_match(p->ts, T_COMMA));
        }
        parse_class_body(p, c);
        d->u.cls = c;
        return d;
    }
    if (ts_match(p->ts, T_KW_TRAIT)) {
        d->kind = DK_TRAIT;
        const Token *nm = ts_expect(p->ts, T_IDENT, "trait name");
        d->name = dup_text(nm);
        AstClass *c = xcalloc(1, sizeof(AstClass));
        c->name = d->name;
        c->name_tok = *nm;
        c->is_trait = true;
        parse_class_body(p, c);
        d->u.cls = c;
        return d;
    }
    if (ts_match(p->ts, T_KW_ENUM)) {
        d->kind = DK_ENUM;
        const Token *nm = ts_expect(p->ts, T_IDENT, "enum name");
        d->name = dup_text(nm);
        AstEnum *e = xcalloc(1, sizeof(AstEnum));
        e->name = d->name;
        e->name_tok = *nm;
        if (ts_match(p->ts, T_COLON)) e->backing = parse_type(p);
        ts_expect(p->ts, T_LBRACE, "'{' to start enum body");
        skip_nl(p);
        size_t idx = 0;
        while (!ts_check(p->ts, T_RBRACE) && !ts_check(p->ts, T_EOF)) {
            if (ts_check(p->ts, T_NEWLINE) || ts_check(p->ts, T_SEMI)) { ts_advance(p->ts); continue; }
            const Token *cn = ts_expect(p->ts, T_IDENT, "enum case");
            EnumCase *ec = xcalloc(1, sizeof(EnumCase));
            ec->name = dup_text(cn);
            ec->name_tok = *cn;
            ec->index = idx++;
            if (ts_match(p->ts, T_ASSIGN)) {
                ec->value = parse_expr(p);
                ec->has_value = true;
            }
            ptrvec_push(&e->cases, ec);
            skip_nl(p);
            ts_match(p->ts, T_COMMA);
            skip_nl(p);
        }
        ts_expect(p->ts, T_RBRACE, "'}' to close enum body");
        expect_nl_or_semi(p);
        d->u.enm = e;
        return d;
    }
    fatal("%s:%zu:%zu: expected declaration", start.file, start.line, start.col);
    return NULL;
}

static AstFn *parse_fn_after_name(Parser *p, const Token *name, bool is_method, bool is_static) {
    AstFn *fn = xcalloc(1, sizeof(AstFn));
    fn->name = intern(dup_text(name));
    fn->name_tok = *name;
    fn->is_method = is_method;
    fn->is_static = is_static;
    ts_expect(p->ts, T_LPAREN, "'(' after function name");
    while (!ts_check(p->ts, T_RPAREN) && !ts_check(p->ts, T_EOF)) {
        Param *pm = xcalloc(1, sizeof(Param));
        if (ts_match(p->ts, T_KW_MUT)) pm->is_mut = true;
        if (ts_match(p->ts, T_KW_OWN)) pm->is_own = true;
        if (ts_check(p->ts, T_AMP)) {
            ts_advance(p->ts);
            pm->by_ref = true;
            eat_ident(p, "mut");
            if (at_ident(p, "mut")) { ts_advance(p->ts); pm->is_mut = true; }
        }
        if (ts_match(p->ts, T_ELLIPSIS)) pm->variadic = true;
        /* accepted forms:  $name: Type   |   Type $name   |   Type &$name */
        if (ts_check(p->ts, T_VAR)) {
            const Token *vn = ts_expect(p->ts, T_VAR, "parameter name");
            pm->name = dup_text(vn);
            pm->name_tok = *vn;
            if (ts_match(p->ts, T_COLON)) pm->type = parse_type(p);
        } else {
            pm->type = parse_type(p);
            if (ts_check(p->ts, T_AMP)) {   /* PHP "array &$x" by-ref form */
                ts_advance(p->ts);
                pm->by_ref = true;
                pm->is_mut = true;
            }
            const Token *vn = ts_expect(p->ts, T_VAR, "parameter name");
            pm->name = dup_text(vn);
            pm->name_tok = *vn;
        }
        if (ts_match(p->ts, T_ASSIGN)) pm->dflt = parse_expr(p);
        ptrvec_push(&fn->params, pm);
        if (!ts_match(p->ts, T_COMMA)) break;
    }
    ts_expect(p->ts, T_RPAREN, "')' after parameters");
    if (ts_match(p->ts, T_COLON)) fn->ret = parse_type(p);
    /* methods without a written return type get PHP semantics: `: void` only
     * when nothing is returned; otherwise the type is inferred in sema */
    fn->ret_inferred = fn->ret == NULL;
    skip_nl(p);
    if (ts_match(p->ts, T_LBRACE)) {
        skip_nl(p);
        while (!ts_check(p->ts, T_RBRACE) && !ts_check(p->ts, T_EOF)) {
            if (ts_check(p->ts, T_NEWLINE) || ts_check(p->ts, T_SEMI)) { ts_advance(p->ts); continue; }
            ptrvec_push(&fn->body, parse_stmt(p));
        }
        ts_expect(p->ts, T_RBRACE, "'}' to close function body");
        skip_nl(p);
    } else {
        expect_nl_or_semi(p); /* abstract method */
        fn->is_abstract = true;
    }
    return fn;
}

static StructField *parse_field(Parser *p, Vis vis, bool is_const) {
    bool is_static = is_const || at_ident(p, "static");
    if (at_ident(p, "static")) ts_advance(p->ts);
    if (ts_match(p->ts, T_KW_STATIC)) is_static = true;   /* keyword form */
    bool is_mut = ts_match(p->ts, T_KW_MUT);
    ts_match(p->ts, T_KW_OWN);
    StructField *f = xcalloc(1, sizeof(StructField));
    f->vis = vis;
    f->is_mut = is_mut || is_const;
    f->is_static = is_static;
    /* accepted forms:  $name: Type  |  Type $name  |  $name = expr  */
    if (ts_check(p->ts, T_VAR)) {
        const Token *vn = ts_expect(p->ts, T_VAR, "field name");
        f->name = intern(dup_text(vn));
        f->name_tok = *vn;
        if (ts_match(p->ts, T_COLON)) f->type = parse_type(p);
    } else {
        f->type = parse_type(p);
        const Token *vn = ts_expect(p->ts, T_VAR, "field name");
        f->name = intern(dup_text(vn));
        f->name_tok = *vn;
    }
    if (ts_match(p->ts, T_ASSIGN)) f->dflt = parse_expr(p);
    expect_nl_or_semi(p);
    return f;
}

static void parse_class_body(Parser *p, AstClass *c) {
    ts_expect(p->ts, T_LBRACE, "'{' after class header");
    skip_nl(p);
    while (!ts_check(p->ts, T_RBRACE) && !ts_check(p->ts, T_EOF)) {
        if (ts_check(p->ts, T_NEWLINE) || ts_check(p->ts, T_SEMI)) { ts_advance(p->ts); continue; }
        Vis vis = VIS_PUBLIC;
        if (ts_check(p->ts, T_KW_PRIVATE)) { vis = VIS_PRIVATE; ts_advance(p->ts); }
        else if (ts_check(p->ts, T_KW_PROTECTED)) { vis = VIS_PROTECTED; ts_advance(p->ts); }
        else if (ts_check(p->ts, T_KW_PUBLIC) || at_ident(p, "pub")) ts_advance(p->ts);
        bool is_const = ts_match(p->ts, T_KW_CONST);
        bool lead_static = ts_match(p->ts, T_KW_STATIC);   /* `static fn f()` / `static $x` */
        if (ts_check(p->ts, T_KW_FN) || ts_check(p->ts, T_KW_FUNCTION)) {
            ts_advance(p->ts);
            bool is_static = lead_static || at_ident(p, "static");
            if (is_static && !lead_static) ts_advance(p->ts);
    /* method name */
    const Token *nm = ts_peek(p->ts);
    if (nm->kind == T_IDENT && nm->len == 11 && memcmp(nm->text, "__construct", 11) == 0) {
        ts_advance(p->ts);
        AstFn *m = parse_fn_after_name(p, nm, true, is_static);
        m->vis = vis;
        m->is_ctor = true;
        ptrvec_push(&c->methods, m);
        continue;
    }
    ts_expect(p->ts, T_IDENT, "method name");
    AstFn *m = parse_fn_after_name(p, nm, true, is_static);
    m->vis = vis;
    ptrvec_push(&c->methods, m);
        } else if (ts_check(p->ts, T_VAR) || ts_check(p->ts, T_KW_MUT) || ts_check(p->ts, T_KW_OWN) ||
                   ts_check(p->ts, T_INT) || ts_check(p->ts, T_IDENT) ||
                   /* a field may lead with its type, and a collection type
                    * starts with '[' -- the same form parameters accept */
                   ts_check(p->ts, T_LBRACKET)) {
            StructField *f = parse_field(p, vis, is_const);
            ptrvec_push(&c->fields, f);
        } else if (lead_static || ts_check(p->ts, T_KW_STATIC) || is_const) {
            /* class constant / static field: `static $x = 1` or `const X = 1` */
            StructField *f = parse_field(p, vis, is_const || lead_static || ts_check(p->ts, T_KW_STATIC));
            ptrvec_push(&c->fields, f);
        } else {
            const Token *t = ts_peek(p->ts);
            fatal("%s:%zu:%zu: expected field or method in class body, found '%.*s'",
                  t->file, t->line, t->col, (int)t->len, t->text ? t->text : "");
        }
    }
    ts_expect(p->ts, T_RBRACE, "'}' to close class body");
    skip_nl(p);
}

/* ---- Types ---- */
static Type *parse_type_base(Parser *p) {
    Type *t = NULL;
    /* `null` is a keyword, not an identifier: as a type it names the null
     * type ("$x is null", "string|null"). ts_match already advances. */
    if (ts_match(p->ts, T_HPHP_NULL)) {
        return type_new(TY_NULL);
    }
    if (ts_match(p->ts, T_AMP)) {
        bool mut = eat_ident(p, "mut");
        Type *el = parse_type(p);
        t = type_new(mut ? TY_MUTREF : TY_REF);
        t->elem = el;
        return t;
    }
    if (ts_match(p->ts, T_KW_OWN)) {
        Type *el = parse_type(p);
        t = type_new(TY_OWN);
        t->elem = el;
        return t;
    }
    if (ts_check(p->ts, T_STAR)) {
        ts_advance(p->ts);
        Type *el = parse_type(p);
        t = type_new(TY_PTR);
        t->elem = el;
        return t;
    }
    if (ts_check(p->ts, T_LBRACKET)) {
        ts_advance(p->ts);
        Type *el = parse_type(p);
        ts_expect(p->ts, T_RBRACKET, "']' in array type");
        t = type_new(TY_ARRAY);
        t->elem = el;
        return t;
    }
    /* `fn` is a keyword, so a fn(...) type annotation arrives as T_KW_FN */
    const Token *nm = ts_check(p->ts, T_KW_FN) ? ts_advance(p->ts)
                                              : ts_expect(p->ts, T_IDENT, "type name");
    const char *n = dup_text(nm);
    (void)nm;
    static const struct { const char *n; TypeKind k; } prims[] = {
        {"int", TY_INT}, {"i8", TY_I8}, {"i16", TY_I16}, {"i32", TY_I32},
        {"i64", TY_I64}, {"u8", TY_I8}, {"u16", TY_I16}, {"u32", TY_I32},
        {"u64", TY_I64}, {"usize", TY_I64}, {"isize", TY_I64},
        {"float", TY_FLOAT}, {"f32", TY_F32}, {"f64", TY_F64},
        {"double", TY_FLOAT},
        {"bool", TY_BOOL}, {"string", TY_STRING}, {"str", TY_STRING},
        {"array", TY_ARRAY}, {"list", TY_ARRAY},
        {"void", TY_VOID}, {"mixed", TY_MIXED},
        {NULL, TY_ERROR},
    };
    /* "array" with no element type: make it a fresh generic array, but
     * never share the singleton — sema resolves elem types in place. */
    if (strcmp(n, "array") == 0 || strcmp(n, "list") == 0) {
        Type *a = type_new(TY_ARRAY);
        a->elem = type_new(TY_MIXED);
        return a;
    }
    for (int i = 0; prims[i].n; i++) {
        if (strcmp(prims[i].n, n) == 0) return type_new(prims[i].k);
    }
    /* `callable` as a type name: any closure (signature not pinned) */
    if (strcmp(n, "callable") == 0 && !ts_check(p->ts, T_LPAREN)) {
        Type *ft = type_new(TY_FN);
        ft->fn.nparams = 0;
        ft->fn.variadic = true;   /* sema: any arity, unchecked args */
        ft->fn.ret = type_new(TY_MIXED);
        return ft;
    }
    /* function type: fn(T1, T2) -> R — `callable(...)` spells the same */
    if ((strcmp(n, "fn") == 0 || strcmp(n, "callable") == 0) &&
        ts_check(p->ts, T_LPAREN)) {
        ts_advance(p->ts);
        Type *ft = type_new(TY_FN);
        PtrVec ps = {0};
        while (!ts_check(p->ts, T_RPAREN) && !ts_check(p->ts, T_EOF)) {
            ptrvec_push(&ps, parse_type(p));
            if (!ts_match(p->ts, T_COMMA)) break;
        }
        ts_expect(p->ts, T_RPAREN, "')' in fn type");
        if (ts_match(p->ts, T_ARROW)) ft->fn.ret = parse_type(p);
        else ft->fn.ret = type_new(TY_VOID);
        ft->fn.nparams = ps.len;
        ft->fn.params = xmalloc(sizeof(Type *) * (ps.len ? ps.len : 1));
        for (size_t i = 0; i < ps.len; i++) ((Type **)ft->fn.params)[i] = ps.items[i];
        return ft;
    }
    if (strcmp(n, "Vec") == 0) {
        t = type_new(TY_VEC);
        if (ts_match(p->ts, T_LT)) {
            t->elem = parse_type(p);
            ts_expect(p->ts, T_GT, "'>' closing Vec<");
        } else t->elem = type_new(TY_MIXED);
        return t;
    }
    if (strcmp(n, "Set") == 0) {
        t = type_new(TY_SET);
        if (ts_match(p->ts, T_LT)) {
            t->elem = parse_type(p);
            ts_expect(p->ts, T_GT, "'>' closing Set<");
        } else t->elem = type_new(TY_MIXED);
        return t;
    }
    if (strcmp(n, "Map") == 0 || strcmp(n, "map") == 0 ||
        strcmp(n, "dict") == 0) {
        t = type_new(TY_MAP);
        if (ts_match(p->ts, T_LT)) {
            t->key = parse_type(p);
            ts_expect(p->ts, T_COMMA, "',' in Map<K, V>");
            t->val = parse_type(p);
            ts_expect(p->ts, T_GT, "'>' closing Map<");
        } else {
            t->key = type_new(TY_MIXED);
            t->val = type_new(TY_MIXED);
        }
        return t;
    }
    if (strcmp(n, "Option") == 0) {
        t = type_new(TY_OPTION);
        if (ts_match(p->ts, T_LT)) {
            t->elem = parse_type(p);
            ts_expect(p->ts, T_GT, "'>' closing Option<");
        } else t->elem = type_new(TY_MIXED);
        return t;
    }
    if (strcmp(n, "Result") == 0) {
        t = type_new(TY_RESULT);
        if (ts_match(p->ts, T_LT)) {
            t->ok = parse_type(p);
            t->err = type_new(TY_STRING);
            if (ts_match(p->ts, T_COMMA)) t->err = parse_type(p);
            ts_expect(p->ts, T_GT, "'>' closing Result<");
        } else {
            t->ok = type_new(TY_MIXED);
            t->err = type_new(TY_STRING);
        }
        return t;
    }
    /* function type: fn(T1, T2) -> R */
    if (strcmp(n, "fn") == 0 && ts_check(p->ts, T_LPAREN)) {
        ts_advance(p->ts);
        t = type_new(TY_FN);
        PtrVec ps = {0};
        while (!ts_check(p->ts, T_RPAREN) && !ts_check(p->ts, T_EOF)) {
            ptrvec_push(&ps, parse_type(p));
            if (!ts_match(p->ts, T_COMMA)) break;
        }
        ts_expect(p->ts, T_RPAREN, "')' in fn type");
        if (ts_match(p->ts, T_ARROW)) t->fn.ret = parse_type(p);
        else t->fn.ret = type_new(TY_VOID);
        t->fn.nparams = ps.len;
        t->fn.params = xmalloc(sizeof(Type *) * (ps.len ? ps.len : 1));
        for (size_t i = 0; i < ps.len; i++) ((Type **)t->fn.params)[i] = ps.items[i];
        return t;
    }
    t = type_new(TY_CLASS);
    t->name = intern(n);
    return t;
}

/* A type annotation is `?T` (nullable) or `A|B` (union).
 *
 * `?T` is sugar for the union {T, null}: both are stored in an hval slot, so
 * the two spellings mean the same thing at runtime. A union of more than one
 * non-null member keeps its own kind, but it is stored the same way. */
static Type *parse_type(Parser *p) {
    if (ts_match(p->ts, T_QUESTION)) {
        Type *el = parse_type(p);
        Type *t = type_new(TY_NULLABLE);
        t->elem = el;
        return t;
    }
    Type *first = parse_type_base(p);
    if (!ts_check(p->ts, T_PIPE)) return first;
    PtrVec members = {0};
    ptrvec_push(&members, first);
    while (ts_match(p->ts, T_PIPE)) {
        if (ts_check(p->ts, T_EOF)) break;
        ptrvec_push(&members, parse_type_base(p));
    }
    /* {T, null} is a nullable T, not a general union */
    if (members.len == 2) {
        const Type *a = members.items[0], *b = members.items[1];
        const Type *other = NULL;
        if (a && a->kind == TY_NULL) other = b;
        else if (b && b->kind == TY_NULL) other = a;
        if (other) {
            Type *t = type_new(TY_NULLABLE);
            t->elem = other;
            free(members.items);
            return t;
        }
    }
    Type *u = type_new(TY_UNION);
    u->nmembers = members.len;
    u->members = xmalloc(sizeof(Type *) * (members.len ? members.len : 1));
    for (size_t i = 0; i < members.len; i++)
        ((Type **)u->members)[i] = members.items[i];
    free(members.items);
    return u;
}

/* ---- Statements ---- */
static PtrVec parse_block_body(Parser *p) {
    PtrVec v = {0};
    skip_nl(p);
    while (!ts_check(p->ts, T_RBRACE) && !ts_check(p->ts, T_EOF)) {
        if (ts_check(p->ts, T_NEWLINE) || ts_check(p->ts, T_SEMI)) { ts_advance(p->ts); continue; }
        ptrvec_push(&v, parse_stmt(p));
    }
    ts_expect(p->ts, T_RBRACE, "'}' to close block");
    skip_nl(p);
    return v;
}

static Stmt *parse_simple_stmt(Parser *p) {
    /* statement without brace-block, used for for-init/steps */
    const Token *t0 = ts_peek(p->ts);
    if (t0->kind == T_KW_LET) {
        ts_advance(p->ts);
        Stmt *s = stmt_new(ST_LET, *t0);
        s->u.let.is_own = ts_match(p->ts, T_KW_OWN);
        if (ts_match(p->ts, T_KW_MUT)) s->u.let.is_mut = true;
        const Token *vn = ts_expect(p->ts, T_VAR, "variable name");
        s->u.let.name = dup_text(vn);
        s->u.let.name_tok = *vn;
        if (ts_match(p->ts, T_COLON)) s->u.let.ann = parse_type(p);
        if (ts_match(p->ts, T_ASSIGN)) s->u.let.init = parse_expr(p);
        return s;
    }
    Expr *e = parse_expr(p);
    Stmt *s = stmt_new(ST_EXPR, e->tok);
    s->u.expr.expr = e;
    s->u.expr.discard = true;
    return s;
}

static Stmt *parse_stmt(Parser *p) {
    const Token *t0 = ts_peek(p->ts);
    switch (t0->kind) {
    case T_KW_LET: {
        Stmt *s = parse_simple_stmt(p);
        expect_nl_or_semi(p);
        return s;
    }
    case T_KW_RETURN: {
        ts_advance(p->ts);
        Stmt *s = stmt_new(ST_RETURN, *t0);
        if (!ts_check(p->ts, T_NEWLINE) && !ts_check(p->ts, T_SEMI) &&
            !ts_check(p->ts, T_RBRACE) && !ts_check(p->ts, T_EOF))
            s->u.ret.value = parse_expr(p);
        expect_nl_or_semi(p);
        return s;
    }
    case T_KW_ECHO: {
        ts_advance(p->ts);
        Stmt *s = stmt_new(ST_ECHO, *t0);
        do {
            ptrvec_push(&s->u.echo.exprs, parse_expr(p));
        } while (ts_match(p->ts, T_COMMA));
        expect_nl_or_semi(p);
        return s;
    }
    case T_KW_PRINT: {
        ts_advance(p->ts);
        Stmt *s = stmt_new(ST_PRINT, *t0);
        s->u.print.expr = parse_expr(p);
        expect_nl_or_semi(p);
        return s;
    }
    case T_KW_IF: {
        ts_advance(p->ts);
        Stmt *s = stmt_new(ST_IF, *t0);
        ts_expect(p->ts, T_LPAREN, "'(' after 'if'");
        s->u.if_.cond = parse_expr(p);
        ts_expect(p->ts, T_RPAREN, "')' after if condition");
        skip_nl(p);
        ts_expect(p->ts, T_LBRACE, "'{' to start if body");
        s->u.if_.then = parse_block_body(p);
        skip_nl(p);
        if (ts_check(p->ts, T_KW_ELSEIF)) {
            ts_advance(p->ts);
            /* elseif = nested if with fresh token */
            Token nt = *ts_peek(p->ts);
            nt.kind = T_KW_IF;
            PtrVec save = p->ts->toks;
            size_t save_idx = p->ts->idx;
            /* trick: rewrite stream: insert IF token */
            PtrVec nv = {0};
            ptrvec_push(&nv, (void *)&nt);
            for (size_t i = save_idx; i < save.len; i++) ptrvec_push(&nv, save.items[i]);
            p->ts->toks = nv;
            p->ts->idx = 0;
            Stmt *elif = parse_stmt(p);
            ptrvec_push(&s->u.if_.els, elif);
        } else if (ts_match(p->ts, T_KW_ELSE)) {
            skip_nl(p);
            if (ts_check(p->ts, T_KW_IF)) {
                ptrvec_push(&s->u.if_.els, parse_stmt(p));
            } else {
                ts_expect(p->ts, T_LBRACE, "'{' to start else body");
                s->u.if_.els = parse_block_body(p);
            }
        }
        return s;
    }
    case T_KW_WHILE: {
        ts_advance(p->ts);
        Stmt *s = stmt_new(ST_WHILE, *t0);
        ts_expect(p->ts, T_LPAREN, "'(' after 'while'");
        s->u.while_.cond = parse_expr(p);
        ts_expect(p->ts, T_RPAREN, "')' after while condition");
        skip_nl(p);
        ts_expect(p->ts, T_LBRACE, "'{' to start while body");
        p->loop_depth++;
        s->u.while_.body = parse_block_body(p);
        p->loop_depth--;
        return s;
    }
    case T_KW_FOR: {
        ts_advance(p->ts);
        Stmt *s = stmt_new(ST_FOR, *t0);
        ts_expect(p->ts, T_LPAREN, "'(' after 'for'");
        while (!ts_check(p->ts, T_SEMI) && !ts_check(p->ts, T_RPAREN) && !ts_check(p->ts, T_EOF)) {
            ptrvec_push(&s->u.for_.init, parse_simple_stmt(p));
            if (!ts_match(p->ts, T_COMMA)) break;
        }
        ts_match(p->ts, T_SEMI);
        if (!ts_check(p->ts, T_SEMI) && !ts_check(p->ts, T_RPAREN))
            s->u.for_.cond = parse_expr(p);
        ts_match(p->ts, T_SEMI);
        while (!ts_check(p->ts, T_RPAREN) && !ts_check(p->ts, T_EOF)) {
            ptrvec_push(&s->u.for_.step, parse_simple_stmt(p));
            if (!ts_match(p->ts, T_COMMA)) break;
        }
        ts_expect(p->ts, T_RPAREN, "')' after for header");
        skip_nl(p);
        ts_expect(p->ts, T_LBRACE, "'{' to start for body");
        p->loop_depth++;
        s->u.for_.body = parse_block_body(p);
        p->loop_depth--;
        return s;
    }
    case T_KW_SWITCH: {
        /* PHP-style switch with fallthrough-free semantics per case block:
         * switch (expr) { case v: stmts... break; ... default: stmts... } */
        ts_advance(p->ts);
        Stmt *s = stmt_new(ST_SWITCH, *t0);
        ts_expect(p->ts, T_LPAREN, "'(' after 'switch'");
        s->u.switch_.cond = parse_expr(p);
        ts_expect(p->ts, T_RPAREN, "')' after switch condition");
        skip_nl(p);
        ts_expect(p->ts, T_LBRACE, "'{' to start switch body");
        skip_nl(p);
        p->loop_depth++;  /* 'break' may exit a switch, like PHP */
        while (!ts_check(p->ts, T_RBRACE) && !ts_check(p->ts, T_EOF)) {
            SwitchCase *sc = xcalloc(1, sizeof(SwitchCase));
            if (ts_check(p->ts, T_KW_CASE)) {
                ts_advance(p->ts);
                /* PHP's `case 1, 2:` - several values, one arm */
                do {
                    ptrvec_push(&sc->patterns, parse_expr(p));
                } while (ts_match(p->ts, T_COMMA));
            } else if (ts_check(p->ts, T_KW_DEFAULT)) {
                ts_advance(p->ts);
                sc->is_default = true;
            } else {
                fatal("%s:%zu:%zu: expected 'case' or 'default' in switch body",
                      ts_peek(p->ts)->file, ts_peek(p->ts)->line,
                      ts_peek(p->ts)->col);
            }
            if (!ts_match(p->ts, T_COLON)) {
                /* tolerate PHP's optional 'case v;' style */
                ts_match(p->ts, T_SEMI);
            }
            skip_nl(p);
            while (!ts_check(p->ts, T_KW_CASE) && !ts_check(p->ts, T_KW_DEFAULT) &&
                   !ts_check(p->ts, T_RBRACE) && !ts_check(p->ts, T_EOF)) {
                ptrvec_push(&sc->body, parse_stmt(p));
                skip_nl(p);
            }
            ptrvec_push(&s->u.switch_.cases, sc);
        }
        p->loop_depth--;
        ts_expect(p->ts, T_RBRACE, "'}' to close switch");
        return s;
    }
    case T_KW_DO: {
        /* do { ... } while (cond); */
        ts_advance(p->ts);
        Stmt *s = stmt_new(ST_DO, *t0);
        skip_nl(p);
        ts_expect(p->ts, T_LBRACE, "'{' to start do body");
        p->loop_depth++;
        s->u.do_.body = parse_block_body(p);
        p->loop_depth--;
        skip_nl(p);
        ts_expect(p->ts, T_KW_WHILE, "'while' after do body");
        ts_expect(p->ts, T_LPAREN, "'(' after 'while'");
        s->u.do_.cond = parse_expr(p);
        ts_expect(p->ts, T_RPAREN, "')' after do-while condition");
        expect_nl_or_semi(p);
        return s;
    }
    case T_KW_FOREACH: {
        ts_advance(p->ts);
        Stmt *s = stmt_new(ST_FOREACH, *t0);
        ts_expect(p->ts, T_LPAREN, "'(' after 'foreach'");
        /* PHP-style:  foreach (<iter> as $v)  |  foreach (<iter> as $k => $v)
           also accepts Rust-style:  foreach ($v in <iter>)                  */
        s->u.foreach.iter = parse_expr(p);
        if (ts_check(p->ts, T_KW_AS) || at_ident(p, "as")) {
            ts_advance(p->ts);
            const Token *kv = ts_expect(p->ts, T_VAR, "foreach variable after 'as'");
            if (ts_match(p->ts, T_DOUBLEARROW)) {
                s->u.foreach.kvar = dup_text(kv);
                const Token *vv = ts_expect(p->ts, T_VAR, "foreach value variable");
                s->u.foreach.var = dup_text(vv);
                s->u.foreach.var_tok = *vv;
            } else {
                s->u.foreach.var = dup_text(kv);
                s->u.foreach.var_tok = *kv;
            }
        } else {
            fatal("%s:%zu:%zu: expected 'as' or 'in' in foreach",
                  ts_peek(p->ts)->file, ts_peek(p->ts)->line, ts_peek(p->ts)->col);
        }
        ts_expect(p->ts, T_RPAREN, "')' after foreach header");
        skip_nl(p);
        ts_expect(p->ts, T_LBRACE, "'{' to start foreach body");
        p->loop_depth++;
        s->u.foreach.body = parse_block_body(p);
        p->loop_depth--;
        return s;
    }
    case T_KW_BREAK: case T_KW_CONTINUE: {
        ts_advance(p->ts);
        Stmt *s = stmt_new(t0->kind == T_KW_BREAK ? ST_BREAK : ST_CONTINUE, *t0);
        s->u.brk.depth = 1;
        if (p->loop_depth == 0)
            fatal("%s:%zu:%zu: 'break'/'continue' outside of a loop", t0->file, t0->line, t0->col);
        expect_nl_or_semi(p);
        return s;
    }
    case T_KW_UNSAFE: {
        ts_advance(p->ts);
        Stmt *s = stmt_new(ST_UNSAFE_BLOCK, *t0);
        skip_nl(p);
        ts_expect(p->ts, T_LBRACE, "'{' after 'unsafe'");
        s->u.unsafe.stmts = parse_block_body(p);
        return s;
    }
    case T_KW_TRY: {
        ts_advance(p->ts);
        Stmt *s = stmt_new(ST_TRY, *t0);
        skip_nl(p);
        ts_expect(p->ts, T_LBRACE, "'{' after 'try'");
        s->u.try.body = parse_block_body(p);
        skip_nl(p);
        while (ts_check(p->ts, T_HPHP_CATCH)) {
            ts_advance(p->ts);
            CatchClause *cc = xcalloc(1, sizeof(CatchClause));
            ts_expect(p->ts, T_LPAREN, "'(' after 'catch'");
            if (ts_check(p->ts, T_VAR)) {
                const Token *vn = ts_advance(p->ts);
                cc->var = dup_text(vn);
                cc->var_tok = *vn;
            } else {
                cc->type = parse_type(p);
                const Token *vn = ts_expect(p->ts, T_VAR, "exception variable");
                cc->var = dup_text(vn);
                cc->var_tok = *vn;
            }
            ts_expect(p->ts, T_RPAREN, "')' after catch header");
            skip_nl(p);
            ts_expect(p->ts, T_LBRACE, "'{' to start catch body");
            cc->body = parse_block_body(p);
            skip_nl(p);
            ptrvec_push(&s->u.try.catches, cc);
        }
        if (ts_match(p->ts, T_HPHP_FINALLY)) {
            skip_nl(p);
            ts_expect(p->ts, T_LBRACE, "'{' after 'finally'");
            s->u.try.fin = parse_block_body(p);
            skip_nl(p);
        }
        return s;
    }
    case T_HPHP_THROW: {
        ts_advance(p->ts);
        Stmt *s = stmt_new(ST_THROW, *t0);
        s->u.throw.expr = parse_expr(p);
        expect_nl_or_semi(p);
        return s;
    }
    case T_KW_GLOBAL: {
        /* PHP `global $a, $b;` — binds function-local names to globals */
        ts_advance(p->ts);
        Stmt *s = stmt_new(ST_GLOBAL, *t0);
        while (!ts_check(p->ts, T_EOF)) {
            const Token *vn = ts_expect(p->ts, T_VAR, "variable after 'global'");
            Token *vt = xcalloc(1, sizeof(Token));
            *vt = *vn;
            ptrvec_push(&s->u.globals.names, (void *)dup_text(vn));
            ptrvec_push(&s->u.globals.toks, vt);
            if (ts_match(p->ts, T_COMMA)) continue;
            break;
        }
        expect_nl_or_semi(p);
        return s;
    }
    default: {
        Expr *e = parse_expr(p);
        Stmt *s = stmt_new(ST_EXPR, e->tok);
        s->u.expr.expr = e;
        s->u.expr.discard = true;
        expect_nl_or_semi(p);
        return s;
    }
    }
}

/* ---- Expressions ---- */
static Expr *parse_expr(Parser *p) { return parse_assign(p); }

/* Tokens that can begin a type annotation. This is what tells a typed
 * declaration ("$x: int = 5") apart from a ternary branch
 * ("$c ? $x : 0"): after a ':' inside an expression only a type may follow
 * a variable, so anything else is ordinary expression syntax. */
static bool tok_starts_type(TokKind k) {
    return k == T_IDENT || k == T_AMP || k == T_STAR || k == T_LBRACKET ||
           k == T_KW_FN || k == T_KW_OWN || k == T_QUESTION;
}

static Expr *parse_assign(Parser *p) {
    /* typed declaration FIRST: "$x: int = 5" or bare "$x: int;"
     * (colon comes before the '=' , like PHP 8 property types) */
    if (ts_check(p->ts, T_VAR) && ts_peek2(p->ts)->kind == T_COLON &&
        tok_starts_type(ts_peek3(p->ts)->kind)) {
        const Token *vn = ts_advance(p->ts); /* $x */
        ts_advance(p->ts);                   /* : */
        Type *ann = parse_type(p);
        Expr *e = expr_new(EX_ASSIGN, *vn);
        memset(&e->u.assign.op, 0, sizeof(Token));
        e->u.assign.op.kind = T_ASSIGN;
        e->u.assign.op.file = vn->file;
        e->u.assign.op.line = vn->line;
        e->u.assign.op.col = vn->col;
        Expr *tgt = expr_new(EX_VAR, *vn);
        tgt->u.var.name = intern(dup_text(vn));
        e->u.assign.target = tgt;
        e->u.assign.ann = ann;
        if (ts_match(p->ts, T_ASSIGN))
            e->u.assign.value = parse_assign(p);
        return e;
    }
    Expr *lhs = parse_ternary(p);
    const Token *t = ts_peek(p->ts);
    switch (t->kind) {
    case T_ASSIGN: case T_PLUSASSIGN: case T_MINUSASSIGN: case T_STARASSIGN:
    case T_SLASHASSIGN: case T_PERCENTASSIGN: case T_DOTASSIGN: case T_POWASSIGN:
    case T_SHLASSIGN: case T_SHRASSIGN: case T_QQASSIGN: {
        ts_advance(p->ts);
        /* PHP: ??= / ?? are non-associative; the lazy short-circuit for ??=
         * is applied in codegen (target is evaluated only once, and only
         * written when it was null). */
        Expr *rhs = (t->kind == T_QQASSIGN) ? parse_ternary(p) : parse_assign(p);
        Expr *e = expr_new(EX_ASSIGN, *t);
        e->u.assign.op = *t;
        e->u.assign.target = lhs;
        e->u.assign.value = rhs;
        if (t->kind == T_QQASSIGN) e->is_nullcoal = true;
        return e;
    }
    case T_INCR: case T_DECR: {
        /* PHP-style postfix increment/decrement as a statement expression:
         * desugar to "$v = $v +/- 1" so codegen needs no new machinery. */
        ts_advance(p->ts);
        Expr *e = expr_new(EX_ASSIGN, *t);
        e->u.assign.op.kind = T_ASSIGN;
        e->u.assign.target = lhs;
        Expr *one = expr_new(EX_INT, *t);
        one->u.i.ival = 1;
        Expr *bin = expr_new(EX_BIN, *t);
        bin->u.bin.op = *t;
        bin->u.bin.op.kind = (t->kind == T_INCR) ? T_PLUS : T_MINUS;
        bin->u.bin.l = lhs;
        bin->u.bin.r = one;
        e->u.assign.value = bin;
        return e;
    }
    default:
        /* PHP destructuring: "[$a, $b] = expr;", "[$a,, $c] = expr;",
         * "[$k => $v] = expr;" and nested lists. Parsed as an assignment
         * whose target is an array literal; codegen expands it. */
        if (lhs->kind == EX_ARRAY_LIT && ts_check(p->ts, T_ASSIGN)) {
            ts_advance(p->ts);
            Expr *rhs = parse_assign(p);
            Expr *e = expr_new(EX_ASSIGN, *t);
            e->u.assign.op.kind = T_ASSIGN;
            e->u.assign.op.file = t->file;
            e->u.assign.op.line = t->line;
            e->u.assign.op.col = t->col;
            e->u.assign.target = lhs;
            e->u.assign.value = rhs;
            return e;
        }
        return lhs;
    }
}

static Expr *parse_bitor_l(Parser *p) { return parse_binop_level(p, 0); }

static Expr *parse_ternary(Parser *p) {
    Expr *cond = parse_bitor_l(p);
    if (ts_match(p->ts, T_QUESTIONQUESTION)) {
        Expr *els = parse_ternary(p);
        Expr *e = expr_new(EX_BIN, cond->tok);
        e->u.bin.op.kind = T_QUESTIONQUESTION;
        e->u.bin.l = cond;
        e->u.bin.r = els;
        e->is_nullcoal = true;
        return e;
    }
    if (ts_match(p->ts, T_QUESTION)) {
        Expr *then = parse_assign(p);
        ts_expect(p->ts, T_COLON, "':' in ternary");
        Expr *els = parse_ternary(p);
        Expr *e = expr_new(EX_TERNARY, cond->tok);
        e->u.tern.cond = cond;
        e->u.tern.then = then;
        e->u.tern.els = els;
        return e;
    }
    return cond;
}

/* binary operator precedence table */
static struct Level {
    int nops;
    TokKind ops[6];
} levels[] = {
    {1, {T_OROR}},                                    /* || */
    {1, {T_ANDAND}},                                  /* && */
    {1, {T_PIPE}},                                    /* |  */
    {1, {T_CARET}},                                   /* ^  */
    {1, {T_AMP}},                                     /* &  */
    {5, {T_EQ, T_NEQ, T_NEQ2, T_EQ2, T_SPACESHIP}},   /* equality */
    {4, {T_LT, T_GT, T_LE, T_GE}},                    /* relational */
    {1, {T_DOT}},                                     /* concat */
    {2, {T_SHL, T_SHR}},                              /* shift */
    {2, {T_PLUS, T_MINUS}},                           /* additive */
    {3, {T_STAR, T_SLASH, T_PERCENT}},                /* multiplicative */
};
#define NLEVELS 11

static Expr *parse_binop_level(Parser *p, int lvl) {
    if (lvl >= NLEVELS) return parse_pow(p);
    Expr *l = parse_binop_level(p, lvl + 1);
    for (;;) {
        TokKind k = ts_peek(p->ts)->kind;
        bool found = false;
        for (int i = 0; i < levels[lvl].nops; i++)
            if (levels[lvl].ops[i] == k) { found = true; break; }
        if (!found) return l;
        Token op = *ts_advance(p->ts);
        Expr *r = parse_binop_level(p, lvl + 1);
        Expr *e = expr_new(EX_BIN, op);
        e->u.bin.op = op;
        e->u.bin.l = l;
        e->u.bin.r = r;
        l = e;
    }
}

static Expr *parse_bitor(Parser *p) { return parse_binop_level(p, 0); }
static Expr *parse_equality(Parser *p) { return parse_binop_level(p, 5); }
static Expr *parse_concat(Parser *p) { return parse_binop_level(p, 7); }
static Expr *parse_shift(Parser *p) { return parse_binop_level(p, 8); }
static Expr *parse_additive(Parser *p) { return parse_binop_level(p, 9); }
static Expr *parse_multiplicative(Parser *p) { return parse_binop_level(p, 10); }

static Expr *parse_pow(Parser *p) {
    Expr *base = parse_unary(p);
    if (ts_check(p->ts, T_POW)) {
        Token op = *ts_advance(p->ts);
        Expr *exp = parse_pow(p); /* right assoc */
        Expr *e = expr_new(EX_BIN, op);
        e->u.bin.op = op;
        e->u.bin.l = base;
        e->u.bin.r = exp;
        return e;
    }
    return base;
}

static Expr *parse_unary(Parser *p) {
    const Token *t = ts_peek(p->ts);
    if (t->kind == T_NOT || t->kind == T_MINUS || t->kind == T_PLUS || t->kind == T_TILDE) {
        Token op = *ts_advance(p->ts);
        Expr *operand = parse_unary(p);
        Expr *e = expr_new(EX_UN, op);
        e->u.un.op = op;
        e->u.un.operand = operand;
        return e;
    }
    if (t->kind == T_AMP) {
        /* borrow or bitwise-and of unary: borrow if followed by $var/ident/&mut */
        if (ts_peek2(p->ts)->kind == T_VAR || at_ident(p, "mut") ||
            ts_peek2(p->ts)->kind == T_AMP) {
            Token amp = *ts_advance(p->ts);
            bool is_mut = false;
            if (at_ident(p, "mut")) { ts_advance(p->ts); is_mut = true; }
            Expr *operand = parse_unary(p);
            Expr *e = expr_new(is_mut ? EX_MUTBORROW : EX_BORROW, amp);
            e->u.borrow.operand = operand;
            return e;
        }
    }
    if (t->kind == T_STAR) {
        Token star = *ts_advance(p->ts);
        Expr *operand = parse_unary(p);
        Expr *e = expr_new(EX_DEREF, star);
        e->u.deref.operand = operand;
        return e;
    }
    return parse_postfix(p);
}

static Expr *parse_call_args(Parser *p, PtrVec *args) {
    ts_expect(p->ts, T_LPAREN, "'(' to start arguments");
    while (!ts_check(p->ts, T_RPAREN) && !ts_check(p->ts, T_EOF)) {
        Expr *a = parse_expr(p);
        ptrvec_push(args, a);
        if (!ts_match(p->ts, T_COMMA)) break;
    }
    ts_expect(p->ts, T_RPAREN, "')' to end arguments");
    return NULL;
}

static Expr *parse_postfix(Parser *p) {
    Expr *e = parse_primary(p);
    for (;;) {
        const Token *t = ts_peek(p->ts);
        if (t->kind == T_LBRACKET) {
            ts_advance(p->ts);
            Expr *idx = NULL;
            if (!ts_check(p->ts, T_RBRACKET)) idx = parse_expr(p);
            /* slice [a..b] */
            if (idx && ts_check(p->ts, T_ELLIPSIS)) {
                ts_advance(p->ts);
                Expr *hi = NULL;
                if (!ts_check(p->ts, T_RBRACKET)) hi = parse_expr(p);
                ts_expect(p->ts, T_RBRACKET, "']' after slice");
                Expr *sl = expr_new(EX_BIN, e->tok);
                sl->u.bin.op.kind = T_COLON;
                sl->u.bin.l = e;
                sl->u.bin.r = idx;
                sl->is_slice = true;
                sl->u.bin.slice_hi = hi;
                e = sl;
                continue;
            }
            ts_expect(p->ts, T_RBRACKET, "']' after index");
            Expr *ix = expr_new(EX_INDEX, *t);
            ix->u.index.obj = e;
            ix->u.index.idx = idx;
            e = ix;
            continue;
        }
        if (t->kind == T_ARROW) {
            ts_advance(p->ts);
            const Token *nm = ts_expect(p->ts, T_IDENT, "member name after '->'");
            if (ts_check(p->ts, T_LPAREN)) {
                Expr *m = expr_new(EX_METHOD, *t);
                m->u.method.obj = e;
                m->u.method.name = dup_text(nm);
                m->u.method.name_tok = *nm;
                parse_call_args(p, &m->u.method.args);
                e = m;
            } else {
                Expr *f = expr_new(EX_FIELD, *t);
                f->u.field.obj = e;
                f->u.field.name = dup_text(nm);
                f->u.field.name_tok = *nm;
                e = f;
            }
            continue;
        }
        if (t->kind == T_COLONCOLON) {
            ts_advance(p->ts);
            const Token *nm = ts_expect(p->ts, T_IDENT, "member after '::'");
            if (ts_check(p->ts, T_LPAREN)) {
                Expr *m = expr_new(EX_STATIC, *t);
                m->u.staticcall.cls = e->kind == EX_VAR ? e->u.var.name : dup_text(nm);
                if (e->kind != EX_VAR)
                    fatal("%s:%zu:%zu: '::' expects a class name", t->file, t->line, t->col);
                m->u.staticcall.name = dup_text(nm);
                parse_call_args(p, &m->u.staticcall.args);
                e = m;
            } else {
                /* static property / enum case / class constant */
                Expr *f = expr_new(EX_FIELD, *t);
                f->u.field.obj = e;
                f->u.field.name = dup_text(nm);
                f->u.field.name_tok = *nm;
                f->u.field.is_static = true;
                e = f;
            }
            continue;
        }
        /* call on any postfix expression: ($cb)(), ($obj->prop)(), $fns[0]()
         * EX_VAR is allowed when it is a real $variable (came out of a
         * paren group); bare_name vars are function names and were already
         * consumed as calls in parse_primary. */
        if (t->kind == T_LPAREN &&
            (e->kind != EX_VAR || !e->u.var.bare_name)) {
            Expr *c = expr_new(EX_CALL, *t);
            c->u.call.fn = e;
            parse_call_args(p, &c->u.call.args);
            e = c;
            continue;
        }
        /* 'as' is a cast ONLY when a type follows; in foreach it separates
           the iterable from the loop variables (handled there). */
        if ((t->kind == T_KW_AS || at_ident(p, "as")) &&
            ts_peek2(p->ts)->kind != T_VAR) {
            ts_advance(p->ts);
            const Type *to = parse_type(p);
            Expr *c = expr_new(EX_CAST, e->tok);
            c->u.cast.to = to;
            c->u.cast.operand = e;
            c->u.cast.ty_tok = *t;
            e = c;
            continue;
        }
        if (at_ident(p, "is")) {
            ts_advance(p->ts);
            const Type *to = parse_type(p);
            Expr *c = expr_new(EX_IS, e->tok);
            c->u.ischeck.to = to;
            c->u.ischeck.operand = e;
            c->u.ischeck.ty_tok = *t;
            e = c;
            continue;
        }
        return e;
    }
}

static Expr *parse_closure_after_fn(Parser *p) {
    AstFn *fn = xcalloc(1, sizeof(AstFn));
    fn->name = xstrdup("<closure>");
    ts_expect(p->ts, T_LPAREN, "'(' after 'fn'");
    while (!ts_check(p->ts, T_RPAREN) && !ts_check(p->ts, T_EOF)) {
        Param *pm = xcalloc(1, sizeof(Param));
        if (ts_match(p->ts, T_KW_MUT)) pm->is_mut = true;
        if (ts_check(p->ts, T_AMP)) {
            ts_advance(p->ts);
            pm->by_ref = true;
            pm->is_mut = true;   /* by-ref params are writeable */
        }
        if (ts_check(p->ts, T_VAR)) {
            const Token *vn = ts_expect(p->ts, T_VAR, "parameter name");
            pm->name = dup_text(vn);
            pm->name_tok = *vn;
            if (ts_match(p->ts, T_COLON)) pm->type = parse_type(p);
        } else {
            pm->type = parse_type(p);
            if (ts_check(p->ts, T_AMP)) {   /* PHP "array &$x" by-ref form */
                ts_advance(p->ts);
                pm->by_ref = true;
                pm->is_mut = true;
            }
            const Token *vn = ts_expect(p->ts, T_VAR, "parameter name");
            pm->name = dup_text(vn);
            pm->name_tok = *vn;
        }
        ptrvec_push(&fn->params, pm);
        if (!ts_match(p->ts, T_COMMA)) break;
    }
    ts_expect(p->ts, T_RPAREN, "')' after closure parameters");
    /* PHP by-value captures: function($x) use ($base) { ... } */
    if (ts_check(p->ts, T_KW_USE) || at_ident(p, "use")) {
        ts_advance(p->ts);
        ts_expect(p->ts, T_LPAREN, "'(' after 'use'");
        while (!ts_check(p->ts, T_RPAREN) && !ts_check(p->ts, T_EOF)) {
            Param *pm = xcalloc(1, sizeof(Param));
            bool byref = ts_check(p->ts, T_AMP);
            if (byref) ts_advance(p->ts);
            const Token *cv = ts_expect(p->ts, T_VAR, "use() variable");
            pm->name = dup_text(cv);
            pm->name_tok = *cv;
            pm->by_ref = byref;
            pm->is_mut = byref;
            pm->dflt = NULL;
            pm->type = NULL;
            ptrvec_push(&fn->use_vars, pm);
            if (!ts_match(p->ts, T_COMMA)) break;
        }
        ts_expect(p->ts, T_RPAREN, "')' after use(...) list");
    }
    if (ts_match(p->ts, T_COLON)) fn->ret = parse_type(p);
    skip_nl(p);
    if (ts_match(p->ts, T_LBRACE)) {
        skip_nl(p);
        while (!ts_check(p->ts, T_RBRACE) && !ts_check(p->ts, T_EOF)) {
            if (ts_check(p->ts, T_NEWLINE) || ts_check(p->ts, T_SEMI)) { ts_advance(p->ts); continue; }
            ptrvec_push(&fn->body, parse_stmt(p));
        }
        ts_expect(p->ts, T_RBRACE, "'}' to close closure body");
        skip_nl(p);
    } else {
        /* expression closure: fn(x) => x * 2 */
        ts_expect(p->ts, T_DOUBLEARROW, "'=>' or '{' in closure");
        Expr *e = parse_expr(p);
        expect_nl_or_semi(p);
        Stmt *st = stmt_new(ST_RETURN, e->tok);
        st->u.ret.value = e;
        ptrvec_push(&fn->body, st);
    }
    Expr *e = expr_new(EX_CLOSURE, fn->name_tok);
    e->u.closure.fn = fn;
    return e;
}

/* '[' array literal; a bare comma ("[$a, , $c]") is an empty destructuring
 * slot, represented as a NULL entry in elems. */
static Expr *parse_primary_lbracket(Parser *p) {
    const Token *t = ts_peek(p->ts);
    ts_advance(p->ts);
    Expr *e = expr_new(EX_ARRAY_LIT, *t);
    skip_nl(p);
    while (!ts_check(p->ts, T_RBRACKET) && !ts_check(p->ts, T_EOF)) {
        skip_nl(p);
        if (ts_check(p->ts, T_COMMA)) {
            /* empty slot: list(,,$c) style skip position */
            ptrvec_push(&e->u.arr.elems, NULL);
            ts_advance(p->ts);
            skip_nl(p);
            continue;
        }
        Expr *k = parse_ternary(p);
        if (ts_match(p->ts, T_DOUBLEARROW)) {
            Expr *v = parse_ternary(p);
            ptrvec_push(&e->u.map.keys, k);
            ptrvec_push(&e->u.map.vals, v);
        } else {
            ptrvec_push(&e->u.arr.elems, k);
        }
        skip_nl(p);
        if (!ts_match(p->ts, T_COMMA)) break;
        skip_nl(p);
    }
    ts_expect(p->ts, T_RBRACKET, "']' to close literal");
    skip_nl(p);
    return e;
}

static Expr *parse_primary(Parser *p) {
    const Token *t = ts_peek(p->ts);
    switch (t->kind) {
    case T_INT: {
        ts_advance(p->ts);
        Expr *e = expr_new(EX_INT, *t);
        e->u.i.ival = t->ival;
        return e;
    }
    case T_FLOAT: {
        ts_advance(p->ts);
        Expr *e = expr_new(EX_FLOAT, *t);
        e->u.f.fval = t->fval;
        return e;
    }
    case T_STR: {
        ts_advance(p->ts);
        Expr *e = expr_new(EX_STR, *t);
        e->u.str.s = dup_text(t);
        return e;
    }
    case T_TPL: {
        ts_advance(p->ts);
        Expr *e = expr_new(EX_TPL, *t);
        for (size_t i = 0; i < t->tpl.strs.len; i++)
            ptrvec_push(&e->u.tpl.strs, t->tpl.strs.items[i]);
        for (size_t i = 0; i < t->tpl.exprs.len; i++) {
            const char *src = t->tpl.exprs.items[i];
            if (!src) continue;
            ptrvec_push(&e->u.tpl.exprs, parse_embedded_expr(src, t->file));
        }
        return e;
    }
    case T_HPHP_TRUE: case T_HPHP_FALSE: {
        ts_advance(p->ts);
        Expr *e = expr_new(EX_BOOL, *t);
        e->u.boolean.b = t->kind == T_HPHP_TRUE;
        return e;
    }
    case T_HPHP_NULL: {
        ts_advance(p->ts);
        return expr_new(EX_NULL, *t);
    }
    case T_VAR: {
        ts_advance(p->ts);
        Expr *e = expr_new(EX_VAR, *t);
        e->u.var.name = intern(dup_text(t));
        /* PHP-style variable call: $fn(args) */
        if (ts_check(p->ts, T_LPAREN)) {
            Expr *call = expr_new(EX_CALL, *t);
            call->u.call.fn = e;
            parse_call_args(p, &call->u.call.args);
            return call;
        }
        return e;
    }
    case T_KW_FN: case T_KW_FUNCTION: {
        ts_advance(p->ts);
        return parse_closure_after_fn(p);
    }
    case T_KW_NEW: {
        ts_advance(p->ts);
        const Token *cn = ts_expect(p->ts, T_IDENT, "class name after 'new'");
        Expr *e = expr_new(EX_NEW, *t);
        e->u.newexpr.cls = dup_text(cn);
        parse_call_args(p, &e->u.newexpr.args);
        return e;
    }
    case T_KW_MATCH: {
        ts_advance(p->ts);
        ts_expect(p->ts, T_LPAREN, "'(' after 'match'");
        Expr *subj = parse_expr(p);
        ts_expect(p->ts, T_RPAREN, "')' after match subject");
        skip_nl(p);
        ts_expect(p->ts, T_LBRACE, "'{' to start match arms");
        skip_nl(p);
        Expr *e = expr_new(EX_MATCH, *t);
        e->u.matchexpr.subject = subj;
        while (!ts_check(p->ts, T_RBRACE) && !ts_check(p->ts, T_EOF)) {
            if (ts_check(p->ts, T_NEWLINE) || ts_check(p->ts, T_SEMI)) { ts_advance(p->ts); continue; }
            MatchCase *mc = xcalloc(1, sizeof(MatchCase));
            if (ts_check(p->ts, T_KW_ELSE) || at_ident(p, "_")) {
                ts_advance(p->ts);
                ts_expect(p->ts, T_DOUBLEARROW, "'=>' in match arm");
                mc->body = parse_ternary(p);
                e->u.matchexpr.dflt = mc->body;
            } else {
                do {
                    ptrvec_push(&mc->patterns, parse_ternary(p));
                } while (ts_match(p->ts, T_COMMA));
                ts_expect(p->ts, T_DOUBLEARROW, "'=>' in match arm");
                mc->body = parse_ternary(p);
                ptrvec_push(&e->u.matchexpr.cases, mc);
            }
            skip_nl(p);
            ts_match(p->ts, T_COMMA);
            skip_nl(p);
        }
        ts_expect(p->ts, T_RBRACE, "'}' to close match arms");
        skip_nl(p);
        return e;
    }
    case T_LPAREN: {
        ts_advance(p->ts);
        /* tuple (a, b, c)? only if comma follows first element */
        if (!ts_check(p->ts, T_RPAREN)) {
            Expr *first = parse_expr(p);
            if (ts_check(p->ts, T_COMMA)) {
                PtrVec elems = {0};
                ptrvec_push(&elems, first);
                while (ts_match(p->ts, T_COMMA)) {
                    if (ts_check(p->ts, T_RPAREN)) break;
                    ptrvec_push(&elems, parse_expr(p));
                }
                ts_expect(p->ts, T_RPAREN, "')' to close tuple");
                Expr *e = expr_new(EX_TUPLE_LIT, *t);
                e->u.tuple.elems = elems;
                return e;
            }
            ts_expect(p->ts, T_RPAREN, "')'");
            return first;
        }
        ts_expect(p->ts, T_RPAREN, "')'");
        Expr *e = expr_new(EX_TUPLE_LIT, *t); /* empty () — should not happen */
        return e;
    }
    case T_LBRACKET:
        return parse_primary_lbracket(p);
    case T_IDENT: {
        /* function call, class name, or plain identifier */
        const Token *nm = ts_advance(p->ts);
        const char *name = dup_text(nm);
        if (ts_check(p->ts, T_LPAREN)) {
            Expr *e = expr_new(EX_CALL, *nm);
            e->u.call.fn = expr_new(EX_VAR, *nm);
            e->u.call.fn->u.var.name = name;
            /* a bare name is a *function* call, never a variable holding a
             * closure — `foo()` must not resolve to $foo (PHP agrees) */
            e->u.call.fn->u.var.bare_name = true;
            parse_call_args(p, &e->u.call.args);
            return e;
        }
        Expr *e = expr_new(EX_VAR, *nm);
        e->u.var.name = intern(name);
        return e;
    }
    default:
        fatal("%s:%zu:%zu: unexpected token '%.*s' in expression",
              t->file, t->line, t->col, (int)t->len, t->text ? t->text : "");
        return NULL;
    }
}

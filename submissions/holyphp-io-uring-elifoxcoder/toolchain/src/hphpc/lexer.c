/* lexer.c — HolyPHP lexer.
 *
 * Produces a token vector in one pass (needed for string interpolation,
 * which re-lexes embedded expressions). Statements are newline-terminated
 * like PHP; `;` is also accepted.
 */
#include "token.h"
#include <ctype.h>

static const struct { const char *w; TokKind k; } keywords[] = {
    {"fn", T_KW_FN}, {"let", T_KW_LET}, {"mut", T_KW_MUT}, {"own", T_KW_OWN},
    {"unsafe", T_KW_UNSAFE}, {"match", T_KW_MATCH},
    {"if", T_KW_IF}, {"else", T_KW_ELSE}, {"elseif", T_KW_ELSEIF},
    {"while", T_KW_WHILE}, {"for", T_KW_FOR}, {"foreach", T_KW_FOREACH},
    {"as", T_KW_AS}, {"in", T_KW_IN}, {"return", T_KW_RETURN}, {"echo", T_KW_ECHO},
    {"print", T_KW_PRINT}, {"break", T_KW_BREAK}, {"continue", T_KW_CONTINUE},
    {"class", T_KW_CLASS}, {"extends", T_KW_EXTENDS}, {"implements", T_KW_IMPLEMENTS},
    {"interface", T_KW_INTERFACE}, {"trait", T_KW_TRAIT},
    {"new", T_KW_NEW}, {"this", T_KW_THIS}, {"self", T_KW_SELF}, {"parent", T_KW_PARENT},
    {"public", T_KW_PUBLIC}, {"protected", T_KW_PROTECTED}, {"private", T_KW_PRIVATE},
    {"static", T_KW_STATIC}, {"abstract", T_KW_ABSTRACT}, {"final", T_KW_FINAL},
    {"const", T_KW_CONST}, {"enum", T_KW_ENUM},
    {"import", T_KW_IMPORT}, {"export", T_KW_EXPORT}, {"use", T_KW_USE},
    {"function", T_KW_FUNCTION},
    {"global", T_KW_GLOBAL},
    {"try", T_KW_TRY}, {"throw", T_HPHP_THROW}, {"catch", T_HPHP_CATCH},
    {"finally", T_HPHP_FINALLY}, {"yield", T_HPHP_YIELD},
    {"switch", T_KW_SWITCH}, {"case", T_KW_CASE}, {"default", T_KW_DEFAULT},
    {"do", T_KW_DO},
    {"true", T_HPHP_TRUE}, {"TRUE", T_HPHP_TRUE}, {"false", T_HPHP_FALSE},
    {"FALSE", T_HPHP_FALSE}, {"null", T_HPHP_NULL}, {"NULL", T_HPHP_NULL},
    {NULL, T_EOF},
};

static TokKind kw_lookup(const char *s, size_t n) {
    for (int i = 0; keywords[i].w; i++)
        if (strlen(keywords[i].w) == n && memcmp(keywords[i].w, s, n) == 0)
            return keywords[i].k;
    return T_IDENT;
}

const char *tok_kind_name(TokKind k) {
    switch (k) {
    case T_EOF: return "end of file";
    case T_IDENT: return "identifier";
    case T_VAR: return "variable";
    case T_INT: return "integer";
    case T_FLOAT: return "float";
    case T_STR: return "string literal";
    case T_TPL: return "string with interpolation";
    default: {
        /* best-effort names for punctuation */
        switch (k) {
        case T_LPAREN: return "'('";
        case T_RPAREN: return "')'";
        case T_LBRACE: return "'{'";
        case T_RBRACE: return "'}'";
        case T_LBRACKET: return "'['";
        case T_RBRACKET: return "']'";
        case T_SEMI: return "';'";
        case T_COLON: return "':'";
        case T_ARROW: return "'->'";
        case T_DOUBLEARROW: return "'=>'";
        case T_ASSIGN: return "'='";
        case T_COMMA: return "','";
        case T_PLUS: return "'+'";
        case T_MINUS: return "'-'";
        case T_STAR: return "'*'";
        case T_SLASH: return "'/'";
        case T_PERCENT: return "'%'";
        case T_POW: return "'**'";
        case T_DOT: return "'.'";
        case T_EQ: return "'=='";
        case T_EQ2: return "'==='";
        case T_NEQ: return "'!='";
        case T_NEQ2: return "'!=='";
        case T_SPACESHIP: return "'<=>'";
        case T_LT: return "'<'";
        case T_GT: return "'>'";
        case T_LE: return "'<='";
        case T_GE: return "'>='";
        case T_ANDAND: return "'&&'";
        case T_OROR: return "'||'";
        case T_AMP: return "'&'";
        case T_PIPE: return "'|'";
        case T_CARET: return "'^'";
        case T_SHL: return "'<<'";
        case T_SHR: return "'>>'";
        case T_NOT: return "'!'";
        case T_QUESTION: return "'?'";
        case T_QUESTIONQUESTION: return "'??'";
        case T_INCR: return "'++'";
        case T_DECR: return "'--'";
        default: return "token";
        }
    }
    }
}

static char peek(Lexer *lx, size_t off) {
    return lx->pos + off < lx->len ? lx->src[lx->pos + off] : '\0';
}
static char cur(Lexer *lx) { return peek(lx, 0); }
static bool eof(Lexer *lx) { return lx->pos >= lx->len; }
static char adv(Lexer *lx) {
    char c = lx->src[lx->pos++];
    if (c == '\n') { lx->line++; lx->col = 1; } else lx->col++;
    return c;
}
static bool matchc(Lexer *lx, char c) {
    if (cur(lx) == c) { adv(lx); return true; }
    return false;
}

static void set_text(Token *t, Lexer *lx, size_t start) {
    t->text = lx->src + start;
    t->len = lx->pos - start;
}

/* Unescape a string body (no interpolation) into a fresh string. */
static char *unescape(const char *s, size_t n) {
    Buf b; buf_init(&b);
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\\' && i + 1 < n) {
            char e = s[++i];
            switch (e) {
            case 'n': buf_putc(&b, '\n'); break;
            case 't': buf_putc(&b, '\t'); break;
            case 'r': buf_putc(&b, '\r'); break;
            case '0': buf_putc(&b, '\0'); break;
            case '\\': buf_putc(&b, '\\'); break;
            case '"': buf_putc(&b, '"'); break;
            case '\'': buf_putc(&b, '\''); break;
            case '$': buf_putc(&b, '$'); break;
            default: buf_putc(&b, '\\'); buf_putc(&b, e); break;
            }
        } else buf_putc(&b, c);
    }
    return buf_take(&b);
}

/* Lex the embedded expressions of "text $var text {$expr} text" and attach
 * them as sub-token-vectors via raw source slices: we store each expr's
 * source in a fresh string and lex lazily in the parser. */
static void lex_template_parts(Lexer *lx, Token *t, size_t body_start) {
    /* body spans lx->src[body_start .. lx->pos-2] (closing quote consumed) */
    const char *b = lx->src + body_start;
    size_t blen = lx->pos - 1 - body_start; /* exclude closing quote */
    Buf chunk; buf_init(&chunk);
    size_t i = 0;
    while (i < blen) {
        char c = b[i];
        if (c == '\\' && i + 1 < blen) {
            char e = b[i + 1];
            switch (e) {
            case 'n': buf_putc(&chunk, '\n'); break;
            case 't': buf_putc(&chunk, '\t'); break;
            case 'r': buf_putc(&chunk, '\r'); break;
            case '0': buf_putc(&chunk, '\0'); break;
            case '"': buf_putc(&chunk, '"'); break;
            case '\\': buf_putc(&chunk, '\\'); break;
            case '$': buf_putc(&chunk, '$'); break;
            default: buf_putc(&chunk, '\\'); buf_putc(&chunk, e); break;
            }
            i += 2;
            continue;
        }
        if (c == '$' && i + 1 < blen &&
            (isalpha((unsigned char)b[i + 1]) || b[i + 1] == '_')) {
            /* $var interpolation */
            size_t j = i + 1;
            while (j < blen && (isalnum((unsigned char)b[j]) || b[j] == '_')) j++;
            char *name = xstrndup(b + i + 1, j - i - 1);
            ptrvec_push(&t->tpl.strs, buf_take(&chunk));
            ptrvec_push(&t->tpl.exprs, name); /* simple variable */
            i = j;
            continue;
        }
        if (c == '{' && i + 1 < blen && b[i + 1] == '$') {
            /* {$expr} interpolation: find matching close brace */
            size_t j = i + 1, depth = 1;
            while (j < blen && depth > 0) {
                if (b[j] == '{') depth++;
                else if (b[j] == '}') { depth--; if (!depth) break; }
                else if (b[j] == '"' || b[j] == '\'') {
                    char q = b[j++];
                    while (j < blen && b[j] != q) { if (b[j] == '\\' && j + 1 < blen) j++; j++; }
                }
                j++;
            }
            size_t expr_len = j - i - 1; /* between { and } */
            char *expr = xstrndup(b + i + 1, expr_len);
            ptrvec_push(&t->tpl.strs, buf_take(&chunk));
            ptrvec_push(&t->tpl.exprs, expr);
            i = j + 1;
            continue;
        }
        buf_putc(&chunk, c);
        i++;
    }
    ptrvec_push(&t->tpl.strs, buf_take(&chunk));
    ptrvec_push(&t->tpl.exprs, NULL); /* terminator */
}

static void lex_number(Lexer *lx, Token *t) {
    size_t start = lx->pos;
    bool isf = false;
    if (cur(lx) == '0' && (peek(lx, 1) == 'x' || peek(lx, 1) == 'X')) {
        adv(lx); adv(lx);
        while (isxdigit((unsigned char)cur(lx))) adv(lx);
        set_text(t, lx, start);
        t->kind = T_INT;
        t->ival = (long long)strtoull(xstrndup(t->text, t->len) + 2, NULL, 16);
        return;
    }
    if (cur(lx) == '0' && (peek(lx, 1) == 'b' || peek(lx, 1) == 'B')) {
        adv(lx); adv(lx);
        while (cur(lx) == '0' || cur(lx) == '1') adv(lx);
        set_text(t, lx, start);
        t->kind = T_INT;
        t->ival = strtoll(xstrndup(t->text, t->len) + 2, NULL, 2);
        return;
    }
    while (isdigit((unsigned char)cur(lx)) || cur(lx) == '_') adv(lx);
    if (cur(lx) == '.' && isdigit((unsigned char)peek(lx, 1))) {
        isf = true; adv(lx);
        while (isdigit((unsigned char)cur(lx)) || cur(lx) == '_') adv(lx);
    }
    if (cur(lx) == 'e' || cur(lx) == 'E') {
        size_t save = lx->pos;
        adv(lx);
        if (cur(lx) == '+' || cur(lx) == '-') adv(lx);
        if (isdigit((unsigned char)cur(lx))) {
            isf = true;
            while (isdigit((unsigned char)cur(lx))) adv(lx);
        } else lx->pos = save;
    }
    set_text(t, lx, start);
    char *clean = xmalloc(t->len + 1);
    size_t ci = 0;
    for (size_t k = 0; k < t->len; k++) if (t->text[k] != '_') clean[ci++] = t->text[k];
    clean[ci] = 0;
    if (isf) { t->kind = T_FLOAT; t->fval = strtod(clean, NULL); }
    else { t->kind = T_INT; t->ival = strtoll(clean, NULL, 10); }
    free(clean);
}

/* PHP's tags. HolyPHP needs none of them - a file with no tags at all is the
 * normal case - but every pasted PHP snippet carries them, and dying on a stray
 * `<` is a terrible first impression. They are skipped here, where whitespace
 * and comments are skipped, so a tagged file lexes exactly like an untagged one.
 *
 *   <?php   open (must be followed by whitespace, as in PHP)
 *   <?      short open tag (same rule)
 *   <?=     open + echo; lex_next turns it into the `echo` keyword
 *   ?>      close tag; the rest of its line goes with it, because past that tag
 *           PHP would be printing HTML and HolyPHP has nowhere to put it
 */
static bool php_open_tag(Lexer *lx) {
    if (cur(lx) != '<' || peek(lx, 1) != '?') return false;
    char a = peek(lx, 2), b = peek(lx, 3), c = peek(lx, 4), d = peek(lx, 5);
    bool php = tolower((unsigned char)a) == 'p' && tolower((unsigned char)b) == 'h' &&
               tolower((unsigned char)c) == 'p';
    /* whitespace (or end of file) has to follow, so `<?phpx` stays an error */
    char after = php ? d : a;
    if (!(after == ' ' || after == '\t' || after == '\r' || after == '\n' || after == '\0'))
        return false;
    adv(lx); adv(lx);                       /* <? */
    if (php) { adv(lx); adv(lx); adv(lx); } /* php */
    return true;
}

static void skip_trivia(Lexer *lx) {
    for (;;) {
        if (php_open_tag(lx)) continue;
        if (cur(lx) == '?' && peek(lx, 1) == '>') {
            adv(lx); adv(lx); /* ?> */
            while (!eof(lx) && cur(lx) != '\n') adv(lx);
            continue;
        }
        if (cur(lx) == ' ' || cur(lx) == '\t' || cur(lx) == '\r') { adv(lx); continue; }
        if (cur(lx) == '/' && peek(lx, 1) == '/') {
            while (!eof(lx) && cur(lx) != '\n') adv(lx);
            continue;
        }
        if (cur(lx) == '#' && peek(lx, 1) != '[') { /* #[ is attribute */
            while (!eof(lx) && cur(lx) != '\n') adv(lx);
            continue;
        }
        if (cur(lx) == '/' && peek(lx, 1) == '*') {
            adv(lx); adv(lx);
            while (!eof(lx) && !(cur(lx) == '*' && peek(lx, 1) == '/')) adv(lx);
            adv(lx); adv(lx); /* consume */
            continue;
        }
        break;
    }
}

void lex_init(Lexer *lx, const char *src, const char *file) {
    memset(lx, 0, sizeof(*lx));
    lx->src = src;
    lx->len = strlen(src);
    lx->file = file;
    lx->line = 1;
}

bool lex_next(Lexer *lx, Token *out) {
    skip_trivia(lx);
    if (eof(lx)) {
        Token t = {0};
        t.kind = T_EOF; t.line = lx->line; t.col = lx->col; t.pos = lx->pos; t.file = lx->file;
        *out = t;
        return false;
    }
    size_t sl = lx->line, sc = lx->col, sp = lx->pos;
    char c = cur(lx);
    Token t; memset(&t, 0, sizeof(t));
    t.line = sl; t.col = sc; t.pos = sp; t.file = lx->file;

    if (isalpha((unsigned char)c) || c == '_') {
        while (isalnum((unsigned char)cur(lx)) || cur(lx) == '_') adv(lx);
        set_text(&t, lx, sp);
        t.kind = kw_lookup(t.text, t.len);
        *out = t;
        return true;
    }
    if (isdigit((unsigned char)c) || (c == '.' && isdigit((unsigned char)peek(lx, 1)))) {
        lex_number(lx, &t);
        *out = t;
        return true;
    }
    if (c == '"') {
        adv(lx);
        size_t body = lx->pos;
        bool has_interp = false;
        while (!eof(lx) && cur(lx) != '"') {
            if (cur(lx) == '\\') { adv(lx); if (!eof(lx)) adv(lx); continue; }
            if (cur(lx) == '{' && peek(lx, 1) == '$') {
                /* interpolated expression: skip balanced braces, honoring
                 * nested strings so inner quotes don't end the literal */
                has_interp = true;
                adv(lx); /* { */
                size_t depth = 1;
                while (!eof(lx) && depth > 0) {
                    if (cur(lx) == '"' || cur(lx) == '\'') {
                        char q = cur(lx);
                        adv(lx);
                        while (!eof(lx) && cur(lx) != q) {
                            if (cur(lx) == '\\') adv(lx);
                            adv(lx);
                        }
                        adv(lx);
                        continue;
                    }
                    if (cur(lx) == '{') depth++;
                    else if (cur(lx) == '}') { depth--; if (!depth) break; }
                    adv(lx);
                }
                adv(lx); /* } */
                continue;
            }
            if (cur(lx) == '$' && (isalpha((unsigned char)peek(lx, 1)) || peek(lx, 1) == '_')) { has_interp = true; }
            adv(lx);
        }
        if (eof(lx)) fatal("%s:%zu:%zu: unterminated string literal", lx->file, sl, sc);
        adv(lx); /* closing quote */
        if (has_interp) {
            t.kind = T_TPL;
            t.text = lx->src + body; t.len = lx->pos - 1 - body;
            lex_template_parts(lx, &t, body);
        } else {
            t.kind = T_STR;
            char *un = unescape(lx->src + body, lx->pos - 1 - body);
            t.text = un;
            t.len = strlen(un);
        }
        *out = t;
        return true;
    }
    if (c == '\'') {
        adv(lx);
        size_t body = lx->pos;
        while (!eof(lx) && cur(lx) != '\'') {
            if (cur(lx) == '\\') adv(lx);
            adv(lx);
        }
        if (eof(lx)) fatal("%s:%zu:%zu: unterminated string literal", lx->file, sl, sc);
        adv(lx);
        t.kind = T_STR;
        t.text = lx->src + body; t.len = lx->pos - 1 - body;
        /* single quotes: no escapes except \' and \\ (PHP semantics) */
        Buf b; buf_init(&b);
        for (size_t i = 0; i < t.len; i++) {
            if (t.text[i] == '\\' && i + 1 < t.len &&
                (t.text[i + 1] == '\'' || t.text[i + 1] == '\\')) i++;
            buf_putc(&b, t.text[i]);
        }
        t.text = buf_take(&b);
        t.len = strlen(t.text);
        *out = t;
        return true;
    }
    if (c == '$') {
        adv(lx);
        if (!(isalpha((unsigned char)cur(lx)) || cur(lx) == '_'))
            fatal("%s:%zu: expected identifier after '$'", lx->file, sl);
        while (isalnum((unsigned char)cur(lx)) || cur(lx) == '_') adv(lx);
        set_text(&t, lx, sp + 1);
        t.kind = T_VAR;
        *out = t;
        return true;
    }

    /* punctuation & operators */
    adv(lx);
    TokKind k = T_ERROR;
    switch (c) {
    case '(': k = T_LPAREN; break;
    case ')': k = T_RPAREN; break;
    case '{': k = T_LBRACE; break;
    case '}': k = T_RBRACE; break;
    case '[': k = T_LBRACKET; break;
    case ']': k = T_RBRACKET; break;
    case ',': k = T_COMMA; break;
    case ';': k = T_SEMI; break;
    case '#':
        if (cur(lx) == '[') { adv(lx); k = T_ANNOTATION; }
        else k = T_HASH;
        break;
    case '@': k = T_AT; break;
    case '\\': k = T_BACKSLASH; break;
    case ':':
        if (matchc(lx, ':')) k = T_COLONCOLON; else k = T_COLON;
        break;
    case '+':
        if (matchc(lx, '+')) k = T_INCR;
        else if (matchc(lx, '=')) k = T_PLUSASSIGN;
        else k = T_PLUS;
        break;
    case '-':
        if (matchc(lx, '>')) k = T_ARROW;
        else if (matchc(lx, '-')) k = T_DECR;
        else if (matchc(lx, '=')) k = T_MINUSASSIGN;
        else k = T_MINUS;
        break;
    case '*':
        if (matchc(lx, '*')) k = T_POW;
        else if (matchc(lx, '=')) k = T_STARASSIGN;
        else k = T_STAR;
        break;
    case '/':
        if (matchc(lx, '=')) k = T_SLASHASSIGN; else k = T_SLASH;
        break;
    case '%':
        if (matchc(lx, '=')) k = T_PERCENTASSIGN; else k = T_PERCENT;
        break;
    case '=':
        /* === (T_EQ2) is a true identity compare: tag AND value must match.
         * == (T_EQ) stays PHP-style loose equality. */
        if (matchc(lx, '=')) {
            if (matchc(lx, '=')) k = T_EQ2; else k = T_EQ;
        } else if (matchc(lx, '>')) k = T_DOUBLEARROW;
        else k = T_ASSIGN;
        break;
    case '!':
        if (matchc(lx, '=')) {
            if (matchc(lx, '=')) k = T_NEQ2;  /* !== */
            else k = T_NEQ;                   /* != and <> */
        }
        else k = T_NOT;
        break;
    case '<':
        /* <?= $x ?> is PHP's echo shorthand */
        if (cur(lx) == '?' && peek(lx, 1) == '=') {
            adv(lx); adv(lx);
            k = T_KW_ECHO;
            break;
        }
        if (matchc(lx, '=')) {
            if (matchc(lx, '>')) k = T_SPACESHIP;
            else k = T_LE;
        }
        else if (matchc(lx, '<')) {
            if (matchc(lx, '=')) k = T_SHLASSIGN;
            else k = T_SHL;
        }
        else if (matchc(lx, '>')) k = T_NEQ2;
        else k = T_LT;
        break;
    case '>':
        if (matchc(lx, '>')) {
            if (matchc(lx, '=')) k = T_SHRASSIGN;
            else k = T_SHR;
        }
        else if (matchc(lx, '=')) k = T_GE;
        else k = T_GT;
        break;
    case '<' + 256: break; /* unreachable */
    case '&':
        if (matchc(lx, '&')) k = T_ANDAND; else k = T_AMP;
        break;
    case '|':
        if (matchc(lx, '|')) k = T_OROR;
        else k = T_PIPE;
        break;
    case '^': k = T_CARET; break;
    case '~': k = T_TILDE; break;
    case '.':
        if (matchc(lx, '=')) k = T_DOTASSIGN;
        else if (matchc(lx, '.') && peek(lx, 0) == '.') { adv(lx); k = T_ELLIPSIS; }
        else k = T_DOT;
        break;
    case '?':
        if (matchc(lx, '?')) { if (matchc(lx, '=')) k = T_QQASSIGN; else if (matchc(lx, ':')) k = T_QUESTIONCOLON; else k = T_QUESTIONQUESTION; }
        else k = T_QUESTION;
        break;
    case '\n':
        k = T_NEWLINE;
        break;
    default: {
        char buf[2] = { c, 0 };
        fatal("%s:%zu:%zu: unexpected character '%s'", lx->file, sl, sc, buf);
    }
    }
    t.kind = k;
    set_text(&t, lx, sp);
    *out = t;
    return true;
}

void ts_init(TokenStream *ts, PtrVec toks, const char *file) {
    ts->toks = toks;
    ts->idx = 0;
    ts->file = file;
}
const Token *ts_peek(TokenStream *ts) {
    return ts->toks.items[ts->idx < ts->toks.len ? ts->idx : ts->toks.len - 1];
}
const Token *ts_peek2(TokenStream *ts) {
    return ts->toks.items[ts->idx + 1 < ts->toks.len ? ts->idx + 1 : ts->toks.len - 1];
}
const Token *ts_peek3(TokenStream *ts) {
    return ts->toks.items[ts->idx + 2 < ts->toks.len ? ts->idx + 2 : ts->toks.len - 1];
}
const Token *ts_advance(TokenStream *ts) {
    const Token *t = ts_peek(ts);
    if (ts->idx < ts->toks.len - 1) ts->idx++;
    return t;
}
bool ts_check(TokenStream *ts, TokKind k) { return ts_peek(ts)->kind == k; }
bool ts_match(TokenStream *ts, TokKind k) {
    if (ts_check(ts, k)) { ts_advance(ts); return true; }
    return false;
}
const Token *ts_expect(TokenStream *ts, TokKind k, const char *what) {
    if (!ts_check(ts, k))
        fatal("%s:%zu:%zu: expected %s (%s), found '%.*s'",
              ts_peek(ts)->file, ts_peek(ts)->line, ts_peek(ts)->col,
              tok_kind_name(k), what ? what : "",
              (int)ts_peek(ts)->len, ts_peek(ts)->text ? ts_peek(ts)->text : "");
    return ts_advance(ts);
}

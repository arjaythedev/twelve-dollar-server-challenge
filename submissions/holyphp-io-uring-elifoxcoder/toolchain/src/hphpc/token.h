/* token.h — lexical tokens for HolyPHP (.hphp).
 *
 * HolyPHP keeps PHP's surface feel ($variables, `fn`, `=>`, string
 * interpolation) while compiling to native code through C.
 */
#ifndef HPHPC_TOKEN_H
#define HPHPC_TOKEN_H

#include "util.h"

typedef enum {
    T_EOF = 0,
    T_ERROR,
    T_IDENT,
    T_VAR,          /* $name */
    T_INT,
    T_FLOAT,
    T_STR,          /* simple string literal, no interpolation */
    T_TPL,          /* double-quoted with interpolation -> Token.tpl.parts */
    T_KW_FN, T_KW_LET, T_KW_MUT, T_KW_OWN, T_KW_UNSAFE, T_KW_MATCH,
    T_KW_IF, T_KW_ELSE, T_KW_ELSEIF, T_KW_WHILE, T_KW_FOR, T_KW_FOREACH,
    T_KW_AS, T_KW_IN, T_KW_RETURN, T_KW_ECHO, T_KW_PRINT, T_KW_BREAK, T_KW_CONTINUE,
    T_KW_CLASS, T_KW_EXTENDS, T_KW_IMPLEMENTS, T_KW_INTERFACE, T_KW_TRAIT,
    T_KW_NEW, T_KW_THIS, T_KW_SELF, T_KW_PARENT, T_KW_PUBLIC, T_KW_PROTECTED,
    T_KW_PRIVATE, T_KW_STATIC, T_KW_ABSTRACT, T_KW_FINAL, T_KW_CONST,
    T_KW_ENUM, T_KW_IMPORT, T_KW_EXPORT, T_KW_USE, T_KW_FUNCTION,
    T_KW_GLOBAL,
    T_KW_TRY, T_HPHP_THROW, T_HPHP_CATCH, T_HPHP_FINALLY, T_HPHP_YIELD,
    T_KW_SWITCH, T_KW_CASE, T_KW_DEFAULT, T_KW_DO,
    T_KW_ARROW, /* fn => arrow fns */
    T_HPHP_TRUE, T_HPHP_FALSE, T_HPHP_NULL,
    T_LPAREN, T_RPAREN, T_LBRACE, T_RBRACE, T_LBRACKET, T_RBRACKET,
    T_COMMA, T_SEMI, T_COLON, T_COLONCOLON, T_ARROW,  /* -> */
    T_DOUBLEARROW,                                   /* => */
    T_FATARROW,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT, T_POW,
    T_ASSIGN, T_PLUSASSIGN, T_MINUSASSIGN, T_STARASSIGN, T_SLASHASSIGN,
    T_PERCENTASSIGN, T_DOTASSIGN, T_POWASSIGN, T_SHLASSIGN, T_SHRASSIGN,
    T_SHL, T_SHR,          /* << >> : bit shifts, needed for binary formats */
    T_EQ, T_NEQ, T_EQ2, T_NEQ2, T_SPACESHIP, T_LT, T_GT, T_LE, T_GE,
    T_ANDAND, T_OROR, T_NOT, T_AMP, T_PIPE, T_CARET, T_TILDE, T_QUESTION,
    T_QUESTIONQUESTION, T_QUESTIONCOLON, T_QQASSIGN,   /* ??= */
    T_DOT, T_INCR, T_DECR,
    T_AT, T_ELLIPSIS, T_HASH, T_BACKSLASH,
    T_NEWLINE,
    T_ANNOTATION, /* #[...] */
} TokKind;

typedef struct Token Token;
struct Token {
    TokKind kind;
    size_t line, col, pos;
    const char *file;
    const char *text;   /* identifier / literal source text */
    size_t len;
    long long ival;
    double fval;
    /* T_TPL parts: alternating (string chunk, expr source) */
    struct { PtrVec strs, exprs; } tpl;
    const char *error;
};

const char *tok_kind_name(TokKind k);
const char *tok_text(const Token *t); /* NUL-terminated copy of text */

typedef struct {
    const char *src;
    size_t len, pos, line, col;
    const char *file;
    Token tok;
    PtrVec toks;
} Lexer;

void lex_init(Lexer *lx, const char *src, const char *file);
bool lex_next(Lexer *lx, Token *out);

/* Token stream with peeking */
typedef struct {
    PtrVec toks;
    size_t idx;
    const char *file;
} TokenStream;

void ts_init(TokenStream *ts, PtrVec toks, const char *file);
const Token *ts_peek(TokenStream *ts);
const Token *ts_peek2(TokenStream *ts);
const Token *ts_peek3(TokenStream *ts);
const Token *ts_advance(TokenStream *ts);
bool ts_check(TokenStream *ts, TokKind k);
bool ts_match(TokenStream *ts, TokKind k);
const Token *ts_expect(TokenStream *ts, TokKind k, const char *what);

#endif

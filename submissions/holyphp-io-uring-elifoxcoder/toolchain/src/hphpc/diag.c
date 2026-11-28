/* diag.c — cargo/rustc-style compiler diagnostics.
 *
 * Human output (a terminal) gets the source frame and colours; machine output
 * (a pipe, the editor extension, the playground) keeps the compact one-line
 * format. The two are the same information, so nothing that parses the old
 * format breaks.
 */
#include "diag.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#define HP_ISATTY(f) _isatty(_fileno(f))
#else
#include <unistd.h>
#define HP_ISATTY(f) isatty(STDERR_FILENO)
#endif

#define C_RESET  "\x1b[0m"
#define C_BOLD   "\x1b[1m"
#define C_RED    "\x1b[1;31m"
#define C_YELLOW "\x1b[1;33m"
#define C_BLUE   "\x1b[1;34m"
#define C_GREEN  "\x1b[1;32m"
#define C_DIM    "\x1b[2m"

static int g_tty;
static int g_color;
static int g_quiet;
static int g_errors;
static int g_warnings;

#ifdef _WIN32
static UINT g_old_out_cp, g_old_in_cp;
static int g_cp_saved;

static void restore_cp(void) {
    if (!g_cp_saved) return;
    SetConsoleOutputCP(g_old_out_cp);
    SetConsoleCP(g_old_in_cp);
}
#endif

void diag_init(void) {
    g_tty = HP_ISATTY(stderr);
    const char *mode = getenv("HPHP_DIAG");
    if (mode && strcmp(mode, "quiet") == 0) {
        /* the fuzzers feed thousands of invalid programs per minute; count the
         * diagnostics but do not print them */
        g_quiet = 1;
        g_tty = 0;
    }
    else if (mode && strcmp(mode, "rich") == 0) g_tty = 1;
    else if (mode && strcmp(mode, "compact") == 0) g_tty = 0;
    const char *no_color = getenv("NO_COLOR");
    const char *term = getenv("TERM");
    g_color = g_tty && !(no_color && *no_color) &&
              !(term && strcmp(term, "dumb") == 0);
    if (mode && strcmp(mode, "rich") == 0 && !no_color) g_color = 1;
#ifdef _WIN32
    if (g_tty) {
        HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
        DWORD mode;
        if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode))
            SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        /* A console defaults to an OEM codepage (437/850); source lines with
         * umlauts would come out as mojibake. Only touch a real console —
         * pipes already carry the raw UTF-8 bytes. */
        g_old_out_cp = GetConsoleOutputCP();
        g_old_in_cp = GetConsoleCP();
        g_cp_saved = 1;
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
        atexit(restore_cp);
    }
#endif
}

int diag_is_tty(void) { return g_tty; }
int diag_errors(void) { return g_errors; }
int diag_warnings(void) { return g_warnings; }

static size_t utf8_len(unsigned char c) {
    if (c < 0x80) return 1;
    if (c < 0xC0) return 1;   /* stray continuation byte */
    if (c < 0xE0) return 2;
    if (c < 0xF0) return 3;
    return 4;
}

/* Display width of n bytes: UTF-8 characters count as one cell, tabs advance
 * to the next multiple of four. */
static size_t display_width(const char *s, size_t n) {
    size_t w = 0, i = 0;
    while (i < n && s[i]) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\t') { w += 4 - (w % 4); i++; }
        else if (c < 0x80) { w++; i++; }
        else { w++; i += utf8_len(c); }
    }
    return w;
}

static char *expand_tabs(const char *s) {
    size_t cap = strlen(s) * 4 + 1;
    char *out = malloc(cap);
    if (!out) return NULL;
    size_t w = 0, col = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; ) {
        if (*p == '\t') {
            size_t next = col + (4 - col % 4);
            while (col < next) { out[w++] = ' '; col++; }
            p++;
        } else {
            size_t n = utf8_len(*p);
            for (size_t k = 0; k < n && p[k]; k++) out[w++] = (char)p[k];
            p += n;
            col++;
        }
    }
    out[w] = 0;
    return out;
}

/* The want-th line of a file (1-based), without the newline. NULL on failure. */
static char *read_line(const char *path, size_t want) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = 0;

    char *p = buf;
    size_t line = 1;
    while (line < want) {
        char *nl = strchr(p, '\n');
        if (!nl) { free(buf); return NULL; }
        p = nl + 1;
        line++;
    }
    char *end = p;
    while (*end && *end != '\n' && *end != '\r') end++;
    *end = 0;

    size_t len = (size_t)(end - p);
    char *out = malloc(len + 1);
    if (out) memcpy(out, p, len + 1);
    free(buf);
    return out;
}

static void emit(const char *s) {
    if (s && *s) fputs(s, stderr);
}

static void colour(const char *c) {
    if (g_color) fputs(c, stderr);
}

/* file:line:col: message   or   file:line: message */
static int split_location(char *msg, char **file, size_t *line, size_t *col,
                          char **rest) {
    for (char *p = strchr(msg, ':'); p; p = strchr(p + 1, ':')) {
        char *q = p + 1;
        if (!isdigit((unsigned char)*q)) continue;
        char *e1 = NULL;
        unsigned long l = strtoul(q, &e1, 10);
        if (!e1 || *e1 != ':') continue;
        char *r = e1 + 1;
        if (isdigit((unsigned char)*r)) {
            char *e2 = NULL;
            unsigned long c = strtoul(r, &e2, 10);
            if (!e2 || *e2 != ':') continue;
            char *m = e2 + 1;
            if (*m == ' ') m++;
            *p = 0;
            *file = msg;
            *line = (size_t)l;
            *col = (size_t)c;
            *rest = m;
            return 1;
        }
        char *m = r;
        if (*m == ' ') m++;
        *p = 0;
        *file = msg;
        *line = (size_t)l;
        *col = 0;
        *rest = m;
        return 1;
    }
    return 0;
}

void diag_at(const char *severity, const char *file, size_t line, size_t col,
             size_t len, const char *msg) {
    if (!severity) severity = "error";
    if (!msg) msg = "";
    if (!file || !*file) file = "?";
    if (strcmp(severity, "error") == 0) g_errors++;
    else if (strcmp(severity, "warning") == 0) g_warnings++;
    if (g_quiet) return;

    if (!g_tty) {
        fprintf(stderr, "%s:%zu:%zu: %s: %s\n", file, line, col, severity, msg);
        return;
    }

    const char *sev_col = strcmp(severity, "error") == 0 ? C_RED : C_YELLOW;

    colour(sev_col);
    emit(severity);
    colour(C_RESET);
    colour(C_BOLD);
    fputs(": ", stderr);
    emit(msg);
    colour(C_RESET);
    fputc('\n', stderr);

    char *src = line ? read_line(file, line) : NULL;
    int width = (int)snprintf(NULL, 0, "%zu", line);

    colour(C_BLUE);
    fprintf(stderr, "%*s--> ", width, "");
    colour(C_RESET);
    fprintf(stderr, "%s:%zu:%zu\n", file, line, col);
    if (!src) return;

    char *shown = expand_tabs(src);
    size_t start = col > 0 ? col - 1 : 0;
    if (start > strlen(src)) start = strlen(src);
    size_t pad = display_width(src, start);
    size_t carets = len ? display_width(src + start, len) : 1;
    if (carets < 1) carets = 1;

    colour(C_BLUE);
    fprintf(stderr, "%*s|\n", width + 1, "");
    colour(C_RESET);
    colour(C_BLUE);
    fprintf(stderr, "%*zu | ", width, line);
    colour(C_RESET);
    fprintf(stderr, "%s\n", shown ? shown : src);

    colour(C_BLUE);
    fprintf(stderr, "%*s| ", width + 1, "");
    colour(C_RESET);
    for (size_t i = 0; i < pad; i++) fputc(' ', stderr);
    colour(sev_col);
    for (size_t i = 0; i < carets; i++) fputc('^', stderr);
    colour(C_RESET);
    fputc('\n', stderr);

    colour(C_BLUE);
    fprintf(stderr, "%*s|\n", width + 1, "");
    colour(C_RESET);

    free(shown);
    free(src);
}

void diag_plain(const char *severity, const char *msg) {
    if (!severity) severity = "error";
    if (strcmp(severity, "error") == 0) g_errors++;
    else if (strcmp(severity, "warning") == 0) g_warnings++;
    if (g_quiet) return;
    if (!g_tty) {
        fprintf(stderr, "hphpc: %s: %s\n", severity, msg ? msg : "");
        return;
    }
    colour(strcmp(severity, "error") == 0 ? C_RED : C_YELLOW);
    emit(severity);
    colour(C_RESET);
    colour(C_BOLD);
    fprintf(stderr, ": %s", msg ? msg : "");
    colour(C_RESET);
    fputc('\n', stderr);
}

void diag_fatal_message(char *msg) {
    char *file = NULL, *rest = NULL;
    size_t line = 0, col = 0;
    if (split_location(msg, &file, &line, &col, &rest))
        diag_at("error", file, line, col, 1, rest);
    else
        diag_plain("error", msg);
}

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *bslash = strrchr(path, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
    return slash ? slash + 1 : path;
}

void diag_summary(const char *path) {
    if (g_quiet) return;
    if (!g_tty) {
        fprintf(stderr, "hphp: compilation failed\n");
        return;
    }
    int n = g_errors ? g_errors : 1;
    fprintf(stderr, "\n");
    colour(C_RED);
    emit("error");
    colour(C_RESET);
    colour(C_BOLD);
    fprintf(stderr, ": could not compile `%s` due to %d previous error%s\n",
            path ? base_name(path) : "program", n, n == 1 ? "" : "s");
    colour(C_RESET);
}

void diag_ok(const char *path) {
    if (g_quiet) return;
    if (!g_tty) {
        fprintf(stderr, "%s: OK (types and borrows check)\n", path);
        return;
    }
    colour(C_GREEN);
    emit("Finished");
    colour(C_RESET);
    fprintf(stderr, " checking `%s` — no problems\n", path ? base_name(path) : "program");
}

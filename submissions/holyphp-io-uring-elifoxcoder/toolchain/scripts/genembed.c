/* genembed.c — generate src/hphpc/runtime_embed.h from the runtime sources.
 *
 * The hphp driver embeds its runtime so an installed hphp binary is fully
 * self-contained. Rebuild the header after changing runtime/*.c:
 *
 *     gcc -O2 -std=c11 scripts/genembed.c -o genembed && ./genembed \
 *         src/hphpc/runtime_embed.h src/hphpc/runtime
 *
 * The SQLite parts are optional and passed as options:
 *
 *     ./genembed out.h rtdir --sqlite-h third_party/sqlite/sqlite3.h \
 *                            --sqlite-lib build/sqlite/<plat>/libsqlite3.a \
 *                            --sqlite-embed src/hphpc/sqlite_embed.h
 *
 * The header is embedded as text, the static archive as base64 (a byte array
 * would be six times the size). They go into their own generated header
 * because the archive is built per platform: the tracked runtime_embed.h
 * stays identical everywhere, and an installed compiler only needs the one
 * generated file its own build produced.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void emit(FILE *o, const char *sym, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    size_t got = fread(b, 1, (size_t)n, f);
    b[got] = 0;
    fclose(f);
    fprintf(o, "static const char %s[] =\n", sym);
    const char *p = b;
    while (*p) {
        fputc('"', o);
        size_t linelen = 0;
        while (*p && linelen < 60) {
            unsigned char c = (unsigned char)*p;
            if (c == '"')
                fputs("\\\"", o);
            else if (c == '\\')
                fputs("\\\\", o);
            else if (c == '\n') {
                fputs("\\n", o);
                p++;
                linelen++;
                break;
            } else if (c == '\t')
                fputs("\\t", o);
            else if (c < 32)
                fprintf(o, "\\x%02x\"\"", c);
            else
                fputc(c, o);
            p++;
            linelen++;
        }
        fputs("\"\n", o);
    }
    fputs(";\n\n", o);
    free(b);
}

/* Base64 keeps the embedded archive to 4/3 of its size; a byte-array
 * initializer would need about six characters per byte. */
static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void emit_b64(FILE *o, const char *sym, const char *len_sym, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)n);
    size_t got = fread(b, 1, (size_t)n, f);
    fclose(f);
    size_t out_n = (got + 2) / 3 * 4;
    fprintf(o, "/* %s: %zu bytes, base64 */\nstatic const char %s[] =\n", path, got, sym);
    size_t col = 0;
    fputc('"', o);
    for (size_t i = 0; i < got; i += 3) {
        unsigned v = (unsigned)b[i] << 16;
        if (i + 1 < got) v |= (unsigned)b[i + 1] << 8;
        if (i + 2 < got) v |= (unsigned)b[i + 2];
        char q[4] = {
            B64[(v >> 18) & 63], B64[(v >> 12) & 63],
            i + 1 < got ? B64[(v >> 6) & 63] : '=',
            i + 2 < got ? B64[v & 63] : '='
        };
        for (int k = 0; k < 4; k++) {
            if (col == 76) { fputs("\"\n\"", o); col = 0; }
            fputc(q[k], o);
            col++;
        }
    }
    fputs("\";\n", o);
    fprintf(o, "static const size_t %s = %zu;\n\n", len_sym, out_n);
    free(b);
}

/* The libraries passed on the command line become bundled libraries: their
 * sources are compiled into hphp, so `import "websocket";` works on an
 * installed compiler with no files on disk.
 *
 * They are named by their path INSIDE lib/ ("ui/ui.hphp", "html/dom.hphp"),
 * not by base name: a multi-file library keeps its parts apart, and two
 * packages may both have a file called view.hphp. */
static const char *lib_rel(const char *path) {
    const char *p = path;
    if (!strncmp(p, "lib/", 4) || !strncmp(p, "lib\\", 4)) return p + 4;
    for (const char *s = p; *s; s++) {
        if ((s[0] == '/' || s[0] == '\\') &&
            (s[1] == 'l') && (s[2] == 'i') && (s[3] == 'b') &&
            (s[4] == '/' || s[4] == '\\'))
            return s + 5;
    }
    return p;
}

static void sanitize(const char *in, char *out, size_t outsz) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 1 < outsz; i++) {
        char c = in[i];
        out[j++] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9')) ? c : '_';
    }
    out[j] = 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: genembed out.h rtdir [--sqlite-h H] [--sqlite-lib A] "
                        "[--sqlite-embed OUT.h] [lib.hphp ...]\n");
        return 1;
    }
    const char *sqlite_h = NULL, *sqlite_lib = NULL, *sqlite_embed = NULL;
    const char **libs = calloc((size_t)argc, sizeof *libs);
    int nlibs = 0;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--sqlite-h") && i + 1 < argc) sqlite_h = argv[++i];
        else if (!strcmp(argv[i], "--sqlite-lib") && i + 1 < argc) sqlite_lib = argv[++i];
        else if (!strcmp(argv[i], "--sqlite-embed") && i + 1 < argc) sqlite_embed = argv[++i];
        else libs[nlibs++] = argv[i];
    }

    FILE *o = fopen(argv[1], "wb");
    if (!o) { perror(argv[1]); return 1; }
    fprintf(o,
            "/* runtime_embed.h — runtime sources embedded in hphp.\n"
            " * Generated by scripts/genembed.c — DO NOT EDIT. */\n"
            "#ifndef HPHP_RUNTIME_EMBED_H\n"
            "#define HPHP_RUNTIME_EMBED_H\n\n");
    char p[512];
    snprintf(p, sizeof p, "%s/hphp_rt.c", argv[2]);
    emit(o, "hp_embed_rt_c", p);
    snprintf(p, sizeof p, "%s/hphp_std.c", argv[2]);
    emit(o, "hp_embed_std_c", p);
    snprintf(p, sizeof p, "%s/hphp_thr.c", argv[2]);
    emit(o, "hp_embed_thr_c", p);
    snprintf(p, sizeof p, "%s/hphp_ui.c", argv[2]);
    emit(o, "hp_embed_ui_c", p);
    snprintf(p, sizeof p, "%s/hphp_wv2.c", argv[2]);
    emit(o, "hp_embed_wv2_c", p);
    snprintf(p, sizeof p, "%s/hphp_uring.c", argv[2]);
    emit(o, "hp_embed_uring_c", p);
    snprintf(p, sizeof p, "%s/hphp_sqlite.c", argv[2]);
    emit(o, "hp_embed_sqlite_c", p);
    snprintf(p, sizeof p, "%s/hphp_rt.h", argv[2]);
    emit(o, "hp_embed_rt_h", p);
    snprintf(p, sizeof p, "%s/hphp_modules.h", argv[2]);
    emit(o, "hp_embed_modules_h", p);
    snprintf(p, sizeof p, "%s/hphp_ext.h", argv[2]);
    emit(o, "hp_embed_ext_h", p);
    if (sqlite_embed) {
        FILE *s = fopen(sqlite_embed, "wb");
        if (!s) { perror(sqlite_embed); return 1; }
        fprintf(s,
                "/* sqlite_embed.h — the vendored SQLite built for this host.\n"
                " * Generated by scripts/genembed.c — DO NOT EDIT, not checked in: the\n"
                " * static archive differs per platform. */\n"
                "#ifndef HPHP_SQLITE_EMBED_H\n"
                "#define HPHP_SQLITE_EMBED_H\n\n");
        if (sqlite_h) {
            emit(s, "hp_embed_sqlite3_h", sqlite_h);
        } else {
            fputs("static const char hp_embed_sqlite3_h[] = \"\";\n\n", s);
        }
        if (sqlite_lib) {
            emit_b64(s, "hp_embed_sqlite_lib_b64", "hp_embed_sqlite_lib_b64_len", sqlite_lib);
        } else {
            fputs("static const char hp_embed_sqlite_lib_b64[] = \"\";\n"
                  "static const size_t hp_embed_sqlite_lib_b64_len = 0;\n\n", s);
        }
        fputs("#endif\n", s);
        fclose(s);
        printf("generated %s\n", sqlite_embed);
    }

    /* bundled libraries: lib-relative name -> source */
    fprintf(o, "/* bundled libraries (lib/<name>/...), by path inside lib/ */\n"
               "typedef struct { const char *name; const char *src; } HpEmbeddedLib;\n");
    for (int i = 0; i < nlibs; i++) {
        char sym[256];
        snprintf(sym, sizeof sym, "hp_embed_lib_%d", i);
        emit(o, sym, libs[i]);
    }
    fprintf(o, "static const HpEmbeddedLib hp_embed_libs[] = {\n");
    for (int i = 0; i < nlibs; i++)
        fprintf(o, "    { \"%s\", hp_embed_lib_%d },\n", lib_rel(libs[i]), i);
    fprintf(o, "    { NULL, NULL }\n};\n");
    fprintf(o, "static const size_t hp_embed_libs_len = %d;\n", nlibs);
    fprintf(o, "#endif\n");
    fclose(o);
    free(libs);
    printf("generated %s\n", argv[1]);
    return 0;
}

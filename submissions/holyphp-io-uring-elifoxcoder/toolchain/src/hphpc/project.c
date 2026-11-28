/* project.c — HolyPHP projects: hphp create / install / update / remove /
 * test / deps. See project.h for the layout.
 *
 * The project file is hphp.yaml (a small YAML subset, see yaml_* below).
 * hphp.json is still read, for older projects and for package archives,
 * and a packed hphp.yaml project carries a generated hphp.json so the
 * registry and older clients understand it. */
#define _DEFAULT_SOURCE 1   /* realpath, popen, fileno, S_ISDIR on glibc with -std=c11 */
#include "project.h"
#include "pkgcore.h"
#include "util.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <windows.h>
#define hp_isatty(fd) _isatty(fd)
#define hp_fileno(f) _fileno(f)
#else
#include <dirent.h>
#include <unistd.h>
#define hp_isatty(fd) isatty(fd)
#define hp_fileno(f) fileno(f)
#endif

/* ------------------------------------------------------------------ */
/* small file helpers                                                  */
/* ------------------------------------------------------------------ */

static bool p_is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool p_is_file(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static void p_mkdir1(const char *p) {
#ifdef _WIN32
    _mkdir(p);
#else
    mkdir(p, 0755);
#endif
}

static void p_mkdir_p(const char *path) {
    char *tmp = xstrdup(path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char c = *p;
            *p = 0;
            if (!(p > tmp && p[-1] == ':')) p_mkdir1(tmp);
            *p = c;
        }
    }
    p_mkdir1(tmp);
    free(tmp);
}

static char *p_read(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *d = xmalloc((size_t)n + 1);
    size_t got = fread(d, 1, (size_t)n, f);
    fclose(f);
    d[got] = 0;
    if (len) *len = got;
    return d;
}

static void p_write(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f) fatal("cannot write %s", path);
    fputs(text, f);
    fclose(f);
}

static bool p_rename(const char *from, const char *to) {
#ifdef _WIN32
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) != 0;
#else
    return rename(from, to) == 0;
#endif
}

/* Names of the entries of a directory (no "." / ".."), sorted. */
static char **p_list(const char *dir, size_t *n_out) {
    PtrVec v = {0};
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    char *pat = fmt("%s/*", dir);
    HANDLE h = FindFirstFileA(pat, &fd);
    free(pat);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, ".."))
                ptrvec_push(&v, xstrdup(fd.cFileName));
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    DIR *dp = opendir(dir);
    if (dp) {
        struct dirent *de;
        while ((de = readdir(dp)))
            if (strcmp(de->d_name, ".") && strcmp(de->d_name, ".."))
                ptrvec_push(&v, xstrdup(de->d_name));
        closedir(dp);
    }
#endif
    char **names = (char **)v.items;
    for (size_t i = 1; i < v.len; i++)        /* insertion sort: tiny lists */
        for (size_t j = i; j > 0 && strcmp(names[j - 1], names[j]) > 0; j--) {
            char *t = names[j]; names[j] = names[j - 1]; names[j - 1] = t;
        }
    *n_out = v.len;
    return names;
}

/* Absolute, forward-slashed form of a directory path. */
static char *p_abs_dir(const char *dir) {
#ifdef _WIN32
    char buf[2048];
    if (!_fullpath(buf, dir, sizeof buf)) return xstrdup(dir);
#else
    char buf[4096];
    if (!realpath(dir, buf)) return xstrdup(dir);
#endif
    for (char *p = buf; *p; p++) if (*p == '\\') *p = '/';
    size_t n = strlen(buf);
    while (n > 1 && buf[n - 1] == '/' && !(n == 3 && buf[1] == ':')) buf[--n] = 0;
    return xstrdup(buf);
}

/* Parent of an absolute directory, or NULL at the root. */
static char *p_parent(const char *dir) {
    const char *cut = strrchr(dir, '/');
    if (!cut) return NULL;
    if (cut == dir) return dir[1] ? xstrdup("/") : NULL;
    if (cut == dir + 2 && dir[1] == ':') return cut[1] ? xstrndup(dir, 3) : NULL;
    return xstrndup(dir, (size_t)(cut - dir));
}

/* ------------------------------------------------------------------ */
/* dependencies                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    char *name;
    char *spec;      /* constraint, or "path:<dir>" */
    bool dev;
} Dep;

typedef struct {
    Dep *v;
    size_t n, cap;
} DepList;

static void deps_add(DepList *l, const char *name, const char *spec, bool dev) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->v = xrealloc(l->v, l->cap * sizeof(Dep));
    }
    l->v[l->n].name = xstrdup(name);
    l->v[l->n].spec = xstrdup(spec);
    l->v[l->n].dev = dev;
    l->n++;
}

static Dep *deps_find(DepList *l, const char *name) {
    for (size_t i = 0; i < l->n; i++)
        if (!strcmp(l->v[i].name, name)) return &l->v[i];
    return NULL;
}

/* ------------------------------------------------------------------ */
/* a little JSON: hphp.json of older projects and package archives     */
/* ------------------------------------------------------------------ */

static const char *j_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

static const char *j_skip_str(const char *p) {   /* p at the opening quote */
    p++;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) p++;
        p++;
    }
    return *p ? p + 1 : p;
}

static const char *j_skip_val(const char *p) {
    p = j_ws(p);
    if (*p == '"') return j_skip_str(p);
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (*p) {
            if (*p == '"') { p = j_skip_str(p); continue; }
            if (*p == '{' || *p == '[') depth++;
            else if (*p == '}' || *p == ']') {
                if (--depth == 0) return p + 1;
            }
            p++;
        }
        return p;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']' &&
           *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t') p++;
    return p;
}

static char *j_unquote(const char *p) {           /* p at the opening quote */
    Buf b = {0};
    p++;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            p++;
            buf_putc(&b, *p == 'n' ? '\n' : *p == 't' ? '\t' : *p);
        } else {
            buf_putc(&b, *p);
        }
        p++;
    }
    buf_putc(&b, 0);
    return b.data;
}

typedef struct { const char *ks, *ke, *vs, *ve; } JMember;

/* Walk the members of the object at `obj`; *it = NULL to start. */
static bool j_next(const char *obj, const char **it, JMember *m) {
    const char *p = *it;
    if (!p) {
        p = j_ws(obj);
        if (*p != '{') return false;
        p++;
    }
    p = j_ws(p);
    if (*p == ',') p = j_ws(p + 1);
    if (*p != '"') return false;
    m->ks = p + 1;
    const char *after = j_skip_str(p);
    m->ke = after - 1;
    p = j_ws(after);
    if (*p != ':') return false;
    p = j_ws(p + 1);
    m->vs = p;
    m->ve = j_skip_val(p);
    *it = m->ve;
    return true;
}

static bool j_find(const char *obj, const char *key, JMember *out) {
    const char *it = NULL;
    JMember m;
    size_t kl = strlen(key);
    while (j_next(obj, &it, &m)) {
        if ((size_t)(m.ke - m.ks) == kl && !strncmp(m.ks, key, kl)) { *out = m; return true; }
    }
    return false;
}

static char *j_get_str(const char *obj, const char *key) {
    JMember m;
    if (!j_find(obj, key, &m) || *m.vs != '"') return NULL;
    return j_unquote(m.vs);
}

static void j_put_str(Buf *b, const char *s) {
    buf_putc(b, '"');
    for (const char *p = s; *p; p++) {
        if (*p == '"' || *p == '\\') buf_putc(b, '\\');
        if (*p == '\n') { buf_puts(b, "\\n"); continue; }
        buf_putc(b, *p);
    }
    buf_putc(b, '"');
}

static void j_deps(const char *json, const char *key, bool dev, DepList *out) {
    JMember obj;
    if (!j_find(json, key, &obj) || *obj.vs != '{') return;
    char *text = xstrndup(obj.vs, (size_t)(obj.ve - obj.vs));
    const char *it = NULL;
    JMember m;
    while (j_next(text, &it, &m)) {
        char *name = xstrndup(m.ks, (size_t)(m.ke - m.ks));
        char *spec = *m.vs == '"' ? j_unquote(m.vs) : xstrdup("*");
        deps_add(out, name, spec, dev);
        free(name);
        free(spec);
    }
    free(text);
}

/* ------------------------------------------------------------------ */
/* a little YAML: what hphp.yaml and hphp.lock need                    */
/*                                                                     */
/*   # comment                                                         */
/*   key: value              plain, "double" or 'single' quoted        */
/*   map:                    one nested level of key: value            */
/*     name: "^1.0"                                                    */
/*   empty: {}                                                         */
/*   list:                   a list of maps (hphp.lock only)           */
/*     - name: async                                                   */
/*       version: 1.0.0                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    int indent;          /* spaces before the first character */
    bool item;           /* line starts with "- " */
    char *key;           /* NULL for a line without "key:" */
    char *val;           /* NULL when nothing follows the colon */
    int lineno;
} YLine;

typedef struct {
    YLine *v;
    size_t n;
} YDoc;

static char *y_scalar(const char *s, const char *end, const char *file, int lineno) {
    while (s < end && (*s == ' ' || *s == '\t')) s++;
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) end--;
    if (s == end) return NULL;
    Buf b = {0};
    if (*s == '"') {
        const char *p = s + 1;
        while (p < end && *p != '"') {
            if (*p == '\\' && p + 1 < end) {
                p++;
                buf_putc(&b, *p == 'n' ? '\n' : *p == 't' ? '\t' : *p);
            } else {
                buf_putc(&b, *p);
            }
            p++;
        }
        if (p >= end) fatal("%s:%d: unterminated \"string\"", file, lineno);
    } else if (*s == '\'') {
        const char *p = s + 1;
        while (p < end) {
            if (*p == '\'') {
                if (p + 1 < end && p[1] == '\'') { buf_putc(&b, '\''); p += 2; continue; }
                break;
            }
            buf_putc(&b, *p++);
        }
        if (p >= end) fatal("%s:%d: unterminated 'string'", file, lineno);
    } else {
        buf_write(&b, s, (size_t)(end - s));
    }
    buf_putc(&b, 0);
    return b.data;
}

/* End of the meaningful part of a line: before a " #" comment that is not
 * inside quotes. */
static const char *y_content_end(const char *s, const char *end) {
    char q = 0;
    for (const char *p = s; p < end; p++) {
        if (q) {
            if (*p == q) q = 0;
            else if (q == '"' && *p == '\\') p++;
            continue;
        }
        if (*p == '"' || *p == '\'') q = *p;
        else if (*p == '#' && (p == s || p[-1] == ' ' || p[-1] == '\t')) return p;
    }
    return end;
}

static YDoc y_parse(const char *text, const char *file) {
    YDoc d = {0};
    size_t cap = 0;
    int lineno = 0;
    for (const char *line = text; *line;) {
        const char *nl = strchr(line, '\n');
        const char *end = nl ? nl : line + strlen(line);
        lineno++;
        const char *p = line;
        int indent = 0;
        while (p < end && (*p == ' ' || *p == '\t')) {
            if (*p == '\t') fatal("%s:%d: use spaces, not tabs, to indent", file, lineno);
            indent++;
            p++;
        }
        const char *cend = y_content_end(p, end);
        while (cend > p && (cend[-1] == ' ' || cend[-1] == '\r')) cend--;
        if (cend > p && !(cend - p == 3 && !strncmp(p, "---", 3))) {
            YLine L = {indent, false, NULL, NULL, lineno};
            if (*p == '-' && (p + 1 == cend || p[1] == ' ')) {
                L.item = true;
                p++;
                while (p < cend && *p == ' ') p++;
                L.indent = (int)(p - line);
            }
            /* key: the colon must be followed by a space or end the line,
             * and must not be inside quotes */
            const char *colon = NULL;
            char q = 0;
            for (const char *c = p; c < cend; c++) {
                if (q) { if (*c == q) q = 0; continue; }
                if (*c == '"' || *c == '\'') { q = *c; continue; }
                if (*c == ':' && (c + 1 == cend || c[1] == ' ')) { colon = c; break; }
            }
            if (colon) {
                L.key = y_scalar(p, colon, file, lineno);
                if (!L.key) fatal("%s:%d: empty key", file, lineno);
                L.val = y_scalar(colon + 1, cend, file, lineno);
            } else if (p < cend) {
                L.val = y_scalar(p, cend, file, lineno);
            }
            if (d.n == cap) {
                cap = cap ? cap * 2 : 32;
                d.v = xrealloc(d.v, cap * sizeof(YLine));
            }
            d.v[d.n++] = L;
        }
        if (!nl) break;
        line = nl + 1;
    }
    return d;
}

/* Top-level scalar. */
static char *y_get(YDoc *d, const char *key) {
    for (size_t i = 0; i < d->n; i++)
        if (d->v[i].indent == 0 && !d->v[i].item && d->v[i].key && !strcmp(d->v[i].key, key))
            return d->v[i].val && strcmp(d->v[i].val, "{}") ? xstrdup(d->v[i].val) : NULL;
    return NULL;
}

/* Top-level map of name: spec. */
static void y_deps(YDoc *d, const char *key, bool dev, const char *file, DepList *out) {
    for (size_t i = 0; i < d->n; i++) {
        YLine *L = &d->v[i];
        if (L->indent != 0 || L->item || !L->key || strcmp(L->key, key)) continue;
        if (L->val && strcmp(L->val, "{}"))
            fatal("%s:%d: \"%s\" must be a list of `name: version` lines below it", file,
                  L->lineno, key);
        for (size_t j = i + 1; j < d->n && d->v[j].indent > 0; j++) {
            YLine *E = &d->v[j];
            if (E->item || !E->key)
                fatal("%s:%d: expected `package: \"version\"` under %s", file, E->lineno, key);
            deps_add(out, E->key, E->val ? E->val : "*", dev);
        }
        return;
    }
}

static bool y_plain_ok(const char *s) {
    if (!*s) return false;
    if (strchr("!&*|>%@`\"'{}[],#?-:^~<=", s[0])) return false;
    for (const char *p = s; *p; p++)
        if (*p == '#' || *p == ':' || *p == '\n' || *p == '"') return false;
    size_t n = strlen(s);
    if (s[n - 1] == ' ') return false;
    if (!strcmp(s, "true") || !strcmp(s, "false") || !strcmp(s, "null") || !strcmp(s, "yes") ||
        !strcmp(s, "no")) return false;
    return true;
}

/* A scalar in its simplest safe spelling. */
static void y_put(Buf *b, const char *s) {
    if (y_plain_ok(s)) { buf_puts(b, s); return; }
    j_put_str(b, s);
}

/* ------------------------------------------------------------------ */
/* manifests: hphp.yaml (preferred) or hphp.json                       */
/* ------------------------------------------------------------------ */

typedef struct {
    char *path;       /* the file read */
    char *text;
    bool yaml;
    YDoc doc;
} Manifest;

/* Path of the manifest in `dir`, or NULL. */
static char *mf_path(const char *dir) {
    char *y = fmt("%s/" PROJECT_MANIFEST, dir);
    if (p_is_file(y)) return y;
    free(y);
    char *j = fmt("%s/" PKG_MANIFEST, dir);
    if (p_is_file(j)) return j;
    free(j);
    return NULL;
}

static bool mf_load(const char *dir, Manifest *m) {
    memset(m, 0, sizeof *m);
    m->path = mf_path(dir);
    if (!m->path) return false;
    m->text = p_read(m->path, NULL);
    if (!m->text) return false;
    size_t pl = strlen(m->path);
    m->yaml = pl > 5 && !strcmp(m->path + pl - 5, ".yaml");
    if (m->yaml) m->doc = y_parse(m->text, m->path);
    return true;
}

static char *mf_get(Manifest *m, const char *key) {
    return m->yaml ? y_get(&m->doc, key) : j_get_str(m->text, key);
}

static void mf_deps(Manifest *m, const char *key, bool dev, DepList *out) {
    size_t before = out->n;
    if (m->yaml) y_deps(&m->doc, key, dev, m->path, out);
    else j_deps(m->text, key, dev, out);
    for (size_t i = before; i < out->n; i++)
        if (!pkg_name_ok(out->v[i].name))
            fatal("%s: \"%s\" in %s is not a valid package name (lowercase letters, digits, _)",
                  m->path, out->v[i].name, key);
}

static void mf_require(const char *root, Manifest *m) {
    if (!mf_load(root, m))
        fatal("%s: no " PROJECT_MANIFEST " (create a project with: hphp create)", root);
}

/* The deps of one kind, serialized for YAML or JSON. */
static void deps_serialize(Buf *b, DepList *l, bool dev, bool yaml) {
    size_t count = 0;
    for (size_t i = 0; i < l->n; i++) if (l->v[i].dev == dev) count++;
    if (count == 0) { buf_puts(b, yaml ? " {}\n" : "{}"); return; }
    buf_puts(b, yaml ? "\n" : "{\n");
    size_t k = 0;
    for (size_t i = 0; i < l->n; i++) {
        if (l->v[i].dev != dev) continue;
        if (yaml) {
            buf_puts(b, "  ");
            buf_puts(b, l->v[i].name);
            buf_puts(b, ": ");
            j_put_str(b, l->v[i].spec);     /* always quoted: "*", "^1.0" */
            buf_puts(b, "\n");
        } else {
            buf_puts(b, "    ");
            j_put_str(b, l->v[i].name);
            buf_puts(b, ": ");
            j_put_str(b, l->v[i].spec);
            buf_puts(b, ++k < count ? ",\n" : "\n");
        }
    }
    if (!yaml) buf_puts(b, "  }");
}

/* YAML: replace the block of a top-level key (key line + indented lines). */
static char *y_set_block(const char *text, const char *key, const char *body) {
    Buf out = {0};
    bool done = false;
    size_t kl = strlen(key);
    for (const char *line = text; *line;) {
        const char *nl = strchr(line, '\n');
        const char *end = nl ? nl + 1 : line + strlen(line);
        if (!done && !strncmp(line, key, kl) && line[kl] == ':') {
            buf_puts(&out, key);
            buf_puts(&out, ":");
            buf_puts(&out, body);
            done = true;
            /* skip the old block: indented, blank or comment-only lines */
            const char *p = end;
            while (*p) {
                const char *n2 = strchr(p, '\n');
                const char *e2 = n2 ? n2 + 1 : p + strlen(p);
                const char *q = p;
                while (q < e2 && (*q == ' ' || *q == '\r' || *q == '\n')) q++;
                bool blank = q == e2;
                if (!(p[0] == ' ' || blank)) break;
                /* keep blank lines that separate the next section */
                if (blank) {
                    const char *look = e2;
                    while (*look == '\n' || *look == '\r') look++;
                    if (*look != ' ') break;
                }
                p = e2;
            }
            line = p;
            continue;
        }
        buf_write(&out, line, (size_t)(end - line));
        line = end;
    }
    if (!done) {
        if (out.len && out.data[out.len - 1] != '\n') buf_putc(&out, '\n');
        buf_puts(&out, key);
        buf_puts(&out, ":");
        buf_puts(&out, body);
    }
    buf_putc(&out, 0);
    return out.data;
}

/* JSON: replace (or add) the value of a top-level key. */
static char *j_set(const char *js, const char *key, const char *value) {
    Buf b = {0};
    JMember m;
    if (j_find(js, key, &m)) {
        buf_write(&b, js, (size_t)(m.vs - js));
        buf_puts(&b, value);
        buf_puts(&b, m.ve);
    } else {
        const char *end = js + strlen(js);
        while (end > js && end[-1] != '}') end--;
        if (end == js) fatal("hphp.json is not a JSON object");
        const char *close = end - 1;
        const char *last = close;
        while (last > js && (last[-1] == ' ' || last[-1] == '\n' ||
                             last[-1] == '\r' || last[-1] == '\t')) last--;
        buf_write(&b, js, (size_t)(last - js));
        if (last > js && last[-1] != '{') buf_putc(&b, ',');
        buf_puts(&b, "\n  ");
        j_put_str(&b, key);
        buf_puts(&b, ": ");
        buf_puts(&b, value);
        buf_puts(&b, "\n");
        buf_puts(&b, close);
    }
    buf_putc(&b, 0);
    return b.data;
}

static void mf_write_deps(const char *root, DepList *deps) {
    Manifest m;
    mf_require(root, &m);
    Buf req = {0}, dev = {0};
    deps_serialize(&req, deps, false, m.yaml);
    buf_putc(&req, 0);
    deps_serialize(&dev, deps, true, m.yaml);
    buf_putc(&dev, 0);
    char *a, *b;
    if (m.yaml) {
        a = y_set_block(m.text, "require", req.data);
        b = y_set_block(a, "require-dev", dev.data);
    } else {
        a = j_set(m.text, "require", req.data);
        b = j_set(a, "require-dev", dev.data);
    }
    p_write(m.path, b);
    free(a); free(b); free(req.data); free(dev.data);
}

/* The manifest of `dir` as hphp.json text, when the folder has an
 * hphp.yaml (used by hphp pkg pack/check/publish); NULL otherwise. */
char *project_manifest_as_json(const char *dir) {
    char *y = fmt("%s/" PROJECT_MANIFEST, dir);
    bool has = p_is_file(y);
    free(y);
    if (!has) return NULL;
    Manifest m;
    if (!mf_load(dir, &m)) return NULL;
    static const char *keys[] = {"name", "version", "type", "description", "entry",
                                 "author", "license", "homepage"};
    Buf b = {0};
    buf_puts(&b, "{\n");
    bool first = true;
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        char *v = mf_get(&m, keys[i]);
        if (!v) continue;
        buf_puts(&b, first ? "  " : ",\n  ");
        first = false;
        j_put_str(&b, keys[i]);
        buf_puts(&b, ": ");
        j_put_str(&b, v);
        free(v);
    }
    DepList deps = {0};
    mf_deps(&m, "require", false, &deps);
    if (deps.n) {
        buf_puts(&b, first ? "  " : ",\n  ");
        buf_puts(&b, "\"require\": ");
        deps_serialize(&b, &deps, false, false);
    }
    buf_puts(&b, "\n}\n");
    buf_putc(&b, 0);
    return b.data;
}

/* ------------------------------------------------------------------ */
/* versions                                                            */
/* ------------------------------------------------------------------ */

static int v_parse(const char *s, long v[3]) {
    v[0] = v[1] = v[2] = 0;
    while (*s == 'v' || *s == 'V' || *s == ' ') s++;
    int parts = 0;
    for (int i = 0; i < 3; i++) {
        if (!isdigit((unsigned char)*s)) break;
        v[i] = strtol(s, (char **)&s, 10);
        parts++;
        if (*s != '.') break;
        s++;
    }
    return parts;
}

static int v_cmp(const long a[3], const long b[3]) {
    for (int i = 0; i < 3; i++)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

/* One comparator such as "^1.2", ">=2", "1.4" or "*". */
static bool v_one(const long ver[3], const char *c) {
    while (*c == ' ') c++;
    if (!*c || !strcmp(c, "*") || !strcmp(c, "x")) return true;
    char op[3] = {0};
    if (!strncmp(c, ">=", 2) || !strncmp(c, "<=", 2)) { op[0] = c[0]; op[1] = '='; c += 2; }
    else if (*c == '>' || *c == '<' || *c == '^' || *c == '~' || *c == '=') { op[0] = *c; c++; }
    long base[3];
    int parts = v_parse(c, base);
    if (parts == 0) return false;
    int cmp = v_cmp(ver, base);
    if (!strcmp(op, ">=")) return cmp >= 0;
    if (!strcmp(op, "<=")) return cmp <= 0;
    if (op[0] == '>') return cmp > 0;
    if (op[0] == '<') return cmp < 0;
    long upper[3] = {0, 0, 0};
    if (op[0] == '^') {
        if (base[0] > 0 || parts == 1) upper[0] = base[0] + 1;
        else if (base[1] > 0 || parts == 2) upper[1] = base[1] + 1;
        else upper[2] = base[2] + 1;
        return cmp >= 0 && v_cmp(ver, upper) < 0;
    }
    if (op[0] == '~') {
        if (parts == 1) upper[0] = base[0] + 1;
        else { upper[0] = base[0]; upper[1] = base[1] + 1; }
        return cmp >= 0 && v_cmp(ver, upper) < 0;
    }
    /* plain or "=": "1.2.3" is exact, "1.2" means 1.2.*, "1" means 1.* */
    if (parts == 3) return cmp == 0;
    if (parts == 2) return ver[0] == base[0] && ver[1] == base[1];
    return ver[0] == base[0];
}

bool project_version_satisfies(const char *version, const char *constraint) {
    long ver[3];
    if (!constraint || !*constraint) return true;
    if (v_parse(version, ver) == 0) return !strcmp(constraint, "*");
    char *alts = xstrdup(constraint);
    bool any = false;
    for (char *alt = alts; alt && !any;) {
        char *bar = strstr(alt, "||");
        if (bar) { *bar = 0; bar += 2; }
        bool all = true;
        char *copy = xstrdup(alt);
        for (char *tok = strtok(copy, " ,"); tok && all; tok = strtok(NULL, " ,"))
            all = v_one(ver, tok);
        free(copy);
        any = all;
        alt = bar;
    }
    free(alts);
    return any;
}

/* "^<major>.<minor>" for a freshly added dependency. */
static char *v_caret(const char *version) {
    long v[3];
    if (v_parse(version, v) == 0) return xstrdup("*");
    if (v[0] == 0) return fmt("^0.%ld.%ld", v[1], v[2]);
    return fmt("^%ld.%ld", v[0], v[1]);
}

/* FNV-1a 64 over the package archive */
static char *checksum_of(const char *data, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char)data[i];
        h *= 1099511628211ULL;
    }
    return fmt("fnv1a64:%016llx", (unsigned long long)h);
}

/* ------------------------------------------------------------------ */
/* project root and import hook                                        */
/* ------------------------------------------------------------------ */

char *project_find_root(const char *start_dir) {
    char *dir = p_abs_dir(start_dir && *start_dir ? start_dir : ".");
    for (int depth = 0; dir && depth < 32; depth++) {
        char *mf = mf_path(dir);
        if (mf) { free(mf); return dir; }
        char *up = p_parent(dir);
        free(dir);
        dir = up;
    }
    free(dir);
    return NULL;
}

char *project_default_entry(void) {
    char *root = project_find_root(".");
    if (!root) return NULL;
    Manifest m;
    char *entry = mf_load(root, &m) ? mf_get(&m, "entry") : NULL;
    char *path = NULL;
    if (entry) {
        path = fmt("%s/%s", root, entry);
        if (!p_is_file(path)) {
            fprintf(stderr, "hphp: %s names entry \"%s\", but %s does not exist\n",
                    m.path, entry, path);
            free(path);
            path = NULL;
        }
    } else {
        fprintf(stderr, "hphp: %s has no \"entry\" (e.g. entry: src/main.hphp)\n",
                m.path ? m.path : root);
    }
    free(entry);
    free(root);
    return path;
}

char *project_default_output(void) {
    char *root = project_find_root(".");
    if (!root) return NULL;
    Manifest m;
    char *name = mf_load(root, &m) ? mf_get(&m, "name") : NULL;
    char *dir = fmt("%s/build", root);
    p_mkdir_p(dir);
    char *out = fmt("%s/%s", dir, name ? name : "app");
    free(dir);
    free(name);
    free(root);
    return out;
}

char *project_vendor_candidate(const char *importer_dir, const char *spec) {
    char pkg[128];
    const char *rest = NULL;
    const char *slash = strchr(spec, '/');
    size_t sl = slash ? (size_t)(slash - spec) : strlen(spec);
    if (!slash && sl > 5 && !strcmp(spec + sl - 5, ".hphp")) sl -= 5;
    if (sl == 0 || sl >= sizeof pkg) return NULL;
    memcpy(pkg, spec, sl);
    pkg[sl] = 0;
    if (!pkg_name_ok(pkg)) return NULL;
    if (slash) rest = slash + 1;

    char *dir = p_abs_dir(importer_dir && *importer_dir ? importer_dir : ".");
    for (int depth = 0; dir && depth < 32; depth++) {
        char *vdir = fmt("%s/" PROJECT_VENDOR "/%s", dir, pkg);
        if (p_is_dir(vdir)) {
            char *cand = NULL;
            if (rest) {
                cand = fmt("%s/%s", vdir, rest);
            } else {
                Manifest m;
                char *entry = mf_load(vdir, &m) ? mf_get(&m, "entry") : NULL;
                cand = entry ? fmt("%s/%s", vdir, entry) : fmt("%s/%s.hphp", vdir, pkg);
                free(entry);
            }
            free(vdir);
            free(dir);
            return cand;
        }
        free(vdir);
        char *up = p_parent(dir);
        free(dir);
        dir = up;
    }
    free(dir);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* lock file (YAML)                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    char *name, *version, *source, *checksum;
    bool dev;
} Locked;

typedef struct {
    Locked *v;
    size_t n, cap;
} LockList;

static void lock_add(LockList *l, Locked e) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->v = xrealloc(l->v, l->cap * sizeof(Locked));
    }
    l->v[l->n++] = e;
}

static Locked *lock_find(LockList *l, const char *name) {
    for (size_t i = 0; i < l->n; i++)
        if (!strcmp(l->v[i].name, name)) return &l->v[i];
    return NULL;
}

static void lock_read(const char *root, LockList *out) {
    char *path = fmt("%s/" PROJECT_LOCK, root);
    char *text = p_read(path, NULL);
    if (!text) { free(path); return; }
    YDoc d = y_parse(text, path);
    Locked cur = {0};
    bool open = false;
    for (size_t i = 0; i < d.n; i++) {
        YLine *L = &d.v[i];
        if (L->indent == 0) continue;            /* lock-version, packages: */
        if (L->item) {
            if (open && cur.name && cur.version) lock_add(out, cur);
            memset(&cur, 0, sizeof cur);
            open = true;
        }
        if (!L->key || !open) continue;
        char *v = L->val ? xstrdup(L->val) : xstrdup("");
        if (!strcmp(L->key, "name")) cur.name = v;
        else if (!strcmp(L->key, "version")) cur.version = v;
        else if (!strcmp(L->key, "source")) cur.source = v;
        else if (!strcmp(L->key, "checksum")) cur.checksum = v;
        else if (!strcmp(L->key, "dev")) { cur.dev = !strcmp(v, "true"); free(v); }
        else free(v);
    }
    if (open && cur.name && cur.version) lock_add(out, cur);
    free(text);
    free(path);
}

static const char *trust_of(const char *source);

static void lock_write(const char *root, LockList *l) {
    for (size_t i = 1; i < l->n; i++)            /* sort by name */
        for (size_t j = i; j > 0 && strcmp(l->v[j - 1].name, l->v[j].name) > 0; j--) {
            Locked t = l->v[j]; l->v[j] = l->v[j - 1]; l->v[j - 1] = t;
        }
    Buf b = {0};
    buf_puts(&b, "# hphp.lock - written by hphp install. Commit it; do not edit it.\n");
    buf_puts(&b, "lock-version: 1\n");
    buf_puts(&b, l->n ? "packages:\n" : "packages: []\n");
    for (size_t i = 0; i < l->n; i++) {
        buf_puts(&b, "  - name: ");     y_put(&b, l->v[i].name);
        buf_puts(&b, "\n    version: "); y_put(&b, l->v[i].version);
        buf_puts(&b, "\n    source: ");  y_put(&b, l->v[i].source && *l->v[i].source ? l->v[i].source : "unknown");
        buf_puts(&b, "\n    checksum: "); y_put(&b, l->v[i].checksum && *l->v[i].checksum ? l->v[i].checksum : "none");
        buf_puts(&b, "\n    trust: ");   buf_puts(&b, trust_of(l->v[i].source));
        buf_puts(&b, "\n    dev: ");     buf_puts(&b, l->v[i].dev ? "true" : "false");
        buf_puts(&b, "\n");
    }
    buf_putc(&b, 0);
    char *path = fmt("%s/" PROJECT_LOCK, root);
    p_write(path, b.data);
    free(path);
    free(b.data);
}

/* ------------------------------------------------------------------ */
/* resolving and installing                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *root;
    bool use_lock;            /* keep locked versions */
    const char *refresh;      /* package to re-resolve even when locked */
    bool no_dev;
    bool quiet;
    bool trust;               /* --trust: install community packages without asking */
} Resolve;

static char *vendor_dir(const char *root) { return fmt("%s/" PROJECT_VENDOR, root); }

/* Version of the package currently in .hphp/packages/<name>, or NULL. */
static char *vendor_version(const char *root, const char *name) {
    char *dir = fmt("%s/" PROJECT_VENDOR "/%s", root, name);
    Manifest m;
    char *v = mf_load(dir, &m) ? mf_get(&m, "version") : NULL;
    free(dir);
    return v;
}

/* `hphp install <pkg>` writes the new dependency before resolving; when the
 * user then declines a package, the project file goes back to how it was. */
static char *g_manifest_path = NULL, *g_manifest_backup = NULL;

static void restore_manifest(void) {
    if (g_manifest_path && g_manifest_backup) {
        p_write(g_manifest_path, g_manifest_backup);
        fprintf(stderr, "  %s left unchanged.\n", g_manifest_path);
    }
}

/* official | community | local, from where a package came */
static const char *trust_of(const char *source) {
    if (!source || !*source) return "community";
    if (!strcmp(source, "bundled")) return "official";
    if (!strncmp(source, "path:", 5)) return "local";
    return "community";
}

/* Fetch one dependency into .hphp/packages/<name>. `lk` is its entry in the
 * old hphp.lock (or NULL): a community package whose exact archive was
 * accepted before is not reviewed again. */
static void fetch_one(const Resolve *r, const Dep *d, const Locked *lk, Locked *out) {
    char *hpx = NULL;
    char *source = NULL;
    bool is_path = !strncmp(d->spec, "path:", 5);
    if (is_path) {
        const char *rel = d->spec + 5;
        char *src = (rel[0] == '/' || (rel[0] && rel[1] == ':'))
                        ? xstrdup(rel) : fmt("%s/%s", r->root, rel);
        if (!p_is_dir(src))
            fatal("%s: path dependency %s does not exist", d->name, src);
        char *cache = pkg_home_file("cache");
        p_mkdir_p(cache);
        hpx = fmt("%s/path-%s.hpx", cache, d->name);
        pkg_pack_dir(src, hpx);
        source = xstrdup(d->spec);
        free(cache);
        free(src);
    } else {
        hpx = pkg_fetch_hpx(d->name, &source);
        if (!hpx)
            fatal("cannot install \"%s\": not on the registry and not bundled with hphp\n"
                  "  (check the name with: hphp pkg search %s)", d->name, d->name);
    }
    size_t n = 0;
    char *data = p_read(hpx, &n);
    if (!data) fatal("cannot read %s", hpx);
    char *sum = checksum_of(data, n);
    free(data);

    char *vdir = vendor_dir(r->root);
    p_mkdir_p(vdir);
    char *tmp = fmt("%s/.tmp-%s", vdir, d->name);
    pkg_remove_tree(tmp);
    if (pkg_extract(hpx, tmp) <= 0) fatal("%s: empty package archive", d->name);
    Manifest m;
    if (!mf_load(tmp, &m)) fatal("%s: package has no " PROJECT_MANIFEST " or " PKG_MANIFEST, d->name);
    char *pname = mf_get(&m, "name");
    char *pver = mf_get(&m, "version");
    if (!pname || strcmp(pname, d->name))
        fatal("%s: the archive contains package \"%s\"", d->name, pname ? pname : "?");
    if (!pver) fatal("%s: package manifest has no version", d->name);
    if (!is_path && !project_version_satisfies(pver, d->spec))
        fatal("%s: the project requires \"%s\", but %s offers %s\n"
              "  (the registry serves the newest version only; relax the constraint)",
              d->name, d->spec, source, pver);
    if (!strcmp(trust_of(source), "community")) {
        bool accepted = lk && lk->checksum && !strcmp(lk->checksum, sum);
        if (!accepted && !project_review_package(tmp, d->name, pver, source, r->trust)) {
            pkg_remove_tree(tmp);
            restore_manifest();
            if (g_manifest_backup)
                fatal("%s was not installed.", d->name);
            fatal("%s was not installed. Install it after reviewing it with\n"
                  "  `hphp install %s` in a terminal, or drop it with `hphp remove %s`.",
                  d->name, d->name, d->name);
        }
    }
    char *dest = fmt("%s/%s", vdir, d->name);
    pkg_remove_tree(dest);
    if (!p_rename(tmp, dest)) {
        pkg_remove_tree(tmp);
        if (pkg_extract(hpx, dest) <= 0) fatal("%s: cannot install into %s", d->name, dest);
    }
    out->name = xstrdup(d->name);
    out->version = pver;
    out->source = source;
    out->checksum = sum;
    out->dev = d->dev;
    if (!r->quiet) printf("  + %-18s %-10s (%s)\n", d->name, pver, source);
    free(pname); free(dest); free(tmp); free(vdir); free(hpx);
}

/* Resolve the project's requirements (and theirs), install them into
 * .hphp/packages/, prune what is no longer needed and write hphp.lock. */
static int resolve(const Resolve *r) {
    Manifest m;
    mf_require(r->root, &m);
    DepList queue = {0};
    mf_deps(&m, "require", false, &queue);
    if (!r->no_dev) mf_deps(&m, "require-dev", true, &queue);

    LockList old = {0};
    if (r->use_lock) lock_read(r->root, &old);

    LockList done = {0};
    DepList seen = {0};
    size_t fetched = 0, kept = 0;
    for (size_t qi = 0; qi < queue.n; qi++) {
        Dep d = queue.v[qi];
        bool path_dep = !strncmp(d.spec, "path:", 5);
        if (deps_find(&seen, d.name)) {
            Locked *have = lock_find(&done, d.name);
            if (!path_dep && have && !project_version_satisfies(have->version, d.spec))
                fatal("version conflict for \"%s\": %s is installed, but another package needs \"%s\"",
                      d.name, have->version, d.spec);
            if (have && !d.dev) have->dev = false;   /* also needed at run time */
            continue;
        }
        deps_add(&seen, d.name, d.spec, d.dev);

        Locked e = {0};
        Locked *lk = lock_find(&old, d.name);
        bool refresh = r->refresh && !strcmp(r->refresh, d.name);
        char *have = vendor_version(r->root, d.name);
        if (lk && !refresh && !path_dep && have && !strcmp(have, lk->version) &&
            project_version_satisfies(lk->version, d.spec)) {
            e.name = xstrdup(lk->name);
            e.version = xstrdup(lk->version);
            e.source = xstrdup(lk->source ? lk->source : "");
            e.checksum = xstrdup(lk->checksum ? lk->checksum : "");
            e.dev = d.dev;
            kept++;
        } else {
            fetch_one(r, &d, lk, &e);
            if (lk && !refresh && !path_dep && strcmp(lk->version, e.version) &&
                project_version_satisfies(lk->version, d.spec))
                fatal("%s: hphp.lock pins %s, but only %s is available now\n"
                      "  (run `hphp update %s` to move to %s)",
                      d.name, lk->version, e.version, d.name, e.version);
            if (lk && !refresh && !path_dep && !strcmp(lk->version, e.version) &&
                lk->checksum && strncmp(lk->checksum, "fnv1a64:", 8) == 0 &&
                strcmp(lk->checksum, e.checksum))
                fatal("%s %s: checksum %s does not match hphp.lock (%s)\n"
                      "  the published package changed; check it, then run `hphp update %s`",
                      d.name, e.version, e.checksum, lk->checksum, d.name);
            fetched++;
        }
        free(have);
        lock_add(&done, e);

        /* the package's own requirements (its require-dev is not ours) */
        char *pdir = fmt("%s/" PROJECT_VENDOR "/%s", r->root, d.name);
        Manifest pm;
        if (mf_load(pdir, &pm)) {
            DepList sub = {0};
            mf_deps(&pm, "require", d.dev, &sub);
            for (size_t i = 0; i < sub.n; i++) deps_add(&queue, sub.v[i].name, sub.v[i].spec, d.dev);
        }
        free(pdir);
    }

    /* prune packages nobody needs anymore */
    char *vdir = vendor_dir(r->root);
    size_t nn = 0;
    char **names = p_list(vdir, &nn);
    size_t pruned = 0;
    for (size_t i = 0; i < nn; i++) {
        if (names[i][0] == '.' || lock_find(&done, names[i])) continue;
        char *dead = fmt("%s/%s", vdir, names[i]);
        pkg_remove_tree(dead);
        if (!r->quiet) printf("  - %s\n", names[i]);
        pruned++;
        free(dead);
    }
    free(vdir);

    lock_write(r->root, &done);
    if (!r->quiet) {
        if (done.n == 0) printf("no dependencies (add one with: hphp install <package>)\n");
        else printf("%zu package%s: %zu installed, %zu up to date%s\n",
                    done.n, done.n == 1 ? "" : "s", fetched, kept,
                    pruned ? ", unused ones removed" : "");
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* reviewing community packages                                        */
/* ------------------------------------------------------------------ */

/* What a package's code can do, found by looking for the builtins that do
 * it. A hint for the reader, not a sandbox: anything that runs can do harm. */
typedef struct {
    const char *what;
    const char *calls[12];
    int hits;
    char *first;           /* "file.hphp:12  shell_exec(" */
} Capability;

static bool ident_char(char c) {
    return isalnum((unsigned char)c) || c == '_' || c == '$' || c == '>' || c == ':';
}

static void scan_file(const char *path, const char *rel, Capability *caps, size_t ncaps) {
    char *text = p_read(path, NULL);
    if (!text) return;
    int lineno = 0;
    for (char *line = text; *line;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        lineno++;
        const char *t = line;
        while (*t == ' ' || *t == '\t') t++;
        bool comment = !strncmp(t, "//", 2) || *t == '*' || !strncmp(t, "/*", 2) || *t == '#';
        for (size_t c = 0; c < ncaps && !comment; c++) {
            for (size_t k = 0; caps[c].calls[k]; k++) {
                const char *needle = caps[c].calls[k];
                for (const char *hit = strstr(line, needle); hit; hit = strstr(hit + 1, needle)) {
                    /* "exec(" must not match "my_exec(" or "$x->exec(" */
                    if (isalpha((unsigned char)needle[0]) && hit > line && ident_char(hit[-1])) continue;
                    caps[c].hits++;
                    if (!caps[c].first) {
                        char snippet[48];
                        snprintf(snippet, sizeof snippet, "%s", t);
                        caps[c].first = fmt("%s:%d  %s%s", rel, lineno, snippet,
                                            strlen(t) > 47 ? "..." : "");
                    }
                    break;
                }
            }
        }
        if (!nl) break;
        line = nl + 1;
    }
    free(text);
}

static void scan_dir(const char *dir, const char *rel, Capability *caps, size_t ncaps,
                     int *files, int *sources) {
    size_t n = 0;
    char **names = p_list(dir, &n);
    for (size_t i = 0; i < n; i++) {
        char *full = fmt("%s/%s", dir, names[i]);
        char *sub = *rel ? fmt("%s/%s", rel, names[i]) : xstrdup(names[i]);
        if (p_is_dir(full)) {
            scan_dir(full, sub, caps, ncaps, files, sources);
        } else {
            (*files)++;
            size_t ln = strlen(names[i]);
            if (ln > 5 && !strcmp(names[i] + ln - 5, ".hphp")) {
                (*sources)++;
                scan_file(full, sub, caps, ncaps);
            }
        }
        free(sub);
        free(full);
    }
}

bool project_review_package(const char *dir, const char *name, const char *version,
                            const char *source, bool trust) {
    Capability caps[] = {
        {"run shell commands",      {"exec(", "shell_exec(", "system(", "passthru(", "proc_open(", "popen(", NULL}, 0, NULL},
        {"write or delete files",   {"unlink(", "rmdir(", "file_put_contents(", "rename(", "fwrite(", "copy(", "mkdir(", NULL}, 0, NULL},
        {"open network connections",{"stream_socket_client(", "stream_socket_server(", "fsockopen(", "http_get(", "http_post(", "async_get(", "->listen(", "->connect(", NULL}, 0, NULL},
        {"use unsafe code",         {"unsafe {", "unsafe{", "malloc(", "memcpy(", "memset(", "u8_set(", "i64_set(", NULL}, 0, NULL},
        {"load native libraries",   {"ffi_load(", "ffi_call(", NULL}, 0, NULL},
        {"change the environment",  {"putenv(", "chdir(", NULL}, 0, NULL},
        {"read files",              {"file_get_contents(", "fopen(", "scandir(", "glob(", NULL}, 0, NULL},
    };
    size_t ncaps = sizeof caps / sizeof caps[0];
    int files = 0, sources = 0;
    scan_dir(dir, "", caps, ncaps, &files, &sources);

    Manifest m;
    char *author = mf_load(dir, &m) ? mf_get(&m, "author") : NULL;
    char *desc = m.text ? mf_get(&m, "description") : NULL;

    fprintf(stderr, "\n  ! community package: %s %s\n", name, version);
    fprintf(stderr, "    from:    %s\n", source);
    fprintf(stderr, "    author:  %s   (as written in its manifest, not verified)\n",
            author && *author ? author : "unknown");
    if (desc && *desc) fprintf(stderr, "    about:   %s\n", desc);
    fprintf(stderr, "    files:   %d (%d HolyPHP source%s)\n", files, sources, sources == 1 ? "" : "s");
    bool any = false;
    for (size_t c = 0; c < ncaps; c++) {
        if (!caps[c].hits) continue;
        fprintf(stderr, "    %-9s%-26s %s%s\n", any ? "" : "it can:", caps[c].what, caps[c].first,
                caps[c].hits > 1 ? fmt("  (+%d more)", caps[c].hits - 1) : "");
        any = true;
    }
    if (!any) fprintf(stderr, "    it can:  nothing risky found in its source\n");
    fprintf(stderr, "\n    Official libraries ship with hphp and are reviewed with it. Community\n"
                    "    packages are written by anyone: install only what you trust. The list\n"
                    "    above is a hint from reading the source, not a guarantee.\n\n");
    free(author);
    free(desc);

    if (trust) {
        fprintf(stderr, "    --trust given: installing without asking.\n\n");
        return true;
    }
    fprintf(stderr, "  Install %s %s? [y/N] ", name, version);
    fflush(stderr);
    char line[64];
    if (!fgets(line, sizeof line, stdin)) {
        fprintf(stderr, "\n  no answer (no terminal?): not installed. Pass --trust to install\n"
                        "  community packages in scripts and CI.\n");
        return false;
    }
    char c = line[0];
    while (c == ' ') c = line[1];
    bool yes = c == 'y' || c == 'Y' || c == 'j' || c == 'J';
    if (!yes) fprintf(stderr, "  not installed.\n");
    return yes;
}

/* ------------------------------------------------------------------ */
/* commands                                                            */
/* ------------------------------------------------------------------ */

static bool flag(int argc, char **argv, const char *name) {
    for (int i = 0; i < argc; i++) if (!strcmp(argv[i], name)) return true;
    return false;
}

static void read_all_deps(const char *root, DepList *deps) {
    Manifest m;
    mf_require(root, &m);
    mf_deps(&m, "require", false, deps);
    mf_deps(&m, "require-dev", true, deps);
}

/* hphp install                 everything the project requires (keeps hphp.lock)
 * hphp install <pkg>[@spec]    add to "require" and install
 * hphp install <pkg> --dev     add to "require-dev"
 * hphp install ../mylib        add a local package folder (path:../mylib)
 * hphp install --no-dev        skip require-dev (deployments) */
int project_cmd_install(const char *root, int argc, char **argv) {
    Resolve r = {root, true, NULL, flag(argc, argv, "--no-dev"), false, flag(argc, argv, "--trust")};
    bool dev = flag(argc, argv, "--dev");
    const char *what = NULL;
    for (int i = 0; i < argc; i++) if (argv[i][0] != '-') { what = argv[i]; break; }
    if (!what) {
        printf("installing dependencies of %s\n", root);
        return resolve(&r);
    }

    char *name = xstrdup(what);
    char *spec = NULL;
    char *at = strchr(name, '@');
    if (at) { *at = 0; spec = xstrdup(at + 1); }
    if (!strncmp(what, "./", 2) || !strncmp(what, "../", 3) || p_is_dir(what)) {
        Manifest pm;
        if (!mf_load(what, &pm)) fatal("%s: no " PROJECT_MANIFEST " in that folder", what);
        free(name);
        name = mf_get(&pm, "name");
        if (!name) fatal("%s: manifest has no name", pm.path);
        char *abs = p_abs_dir(what);
        char *rootabs = p_abs_dir(root);
        size_t rl = strlen(rootabs);
        if (!strncmp(abs, rootabs, rl) && abs[rl] == '/') {
            spec = fmt("path:%s", abs + rl + 1);
        } else {
            /* relative to the project, so the project can move */
            char *rel = NULL;
            const char *cwdrel = what;
            char *cwdabs = p_abs_dir(".");
            if (!strcmp(cwdabs, rootabs)) rel = xstrdup(cwdrel);
            spec = rel ? fmt("path:%s", rel) : fmt("path:%s", abs);
            free(rel);
            free(cwdabs);
        }
        free(abs);
        free(rootabs);
    }
    if (!pkg_name_ok(name)) fatal("\"%s\" is not a valid package name", name);

    DepList deps = {0};
    read_all_deps(root, &deps);
    Dep *d = deps_find(&deps, name);
    if (d) {
        if (spec) { free(d->spec); d->spec = xstrdup(spec); }
        if (dev) d->dev = true;
    } else {
        deps_add(&deps, name, spec ? spec : "*", dev);
    }
    g_manifest_path = mf_path(root);
    g_manifest_backup = g_manifest_path ? p_read(g_manifest_path, NULL) : NULL;
    mf_write_deps(root, &deps);
    r.refresh = name;
    printf("adding %s to %s\n", name, dev ? "require-dev" : "require");
    int rc = resolve(&r);
    if (rc == 0 && !spec) {
        /* pin a sensible range for the version that was installed */
        char *v = vendor_version(root, name);
        Dep *nd = deps_find(&deps, name);
        if (v && nd && !strcmp(nd->spec, "*")) {
            free(nd->spec);
            nd->spec = v_caret(v);
            mf_write_deps(root, &deps);
            printf("%s: %s: \"%s\"\n", PROJECT_MANIFEST, name, nd->spec);
        }
        free(v);
    }
    printf("use it with:  import \"%s\";\n", name);
    return rc;
}

/* hphp update [pkg]   re-resolve everything (or one package) ignoring the lock */
int project_cmd_update(const char *root, int argc, char **argv) {
    const char *only = NULL;
    for (int i = 0; i < argc; i++) if (argv[i][0] != '-') { only = argv[i]; break; }
    Resolve r = {root, only != NULL, only, flag(argc, argv, "--no-dev"), false, flag(argc, argv, "--trust")};
    if (only) printf("updating %s\n", only);
    else printf("updating all dependencies\n");
    return resolve(&r);
}

/* hphp remove <pkg> */
int project_cmd_remove(const char *root, int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "usage: hphp remove <package>\n"); return 1; }
    DepList deps = {0}, kept = {0};
    read_all_deps(root, &deps);
    bool found = false;
    for (size_t i = 0; i < deps.n; i++) {
        if (!strcmp(deps.v[i].name, argv[0])) { found = true; continue; }
        deps_add(&kept, deps.v[i].name, deps.v[i].spec, deps.v[i].dev);
    }
    if (!found) {
        fprintf(stderr, "%s is not a dependency of this project\n", argv[0]);
        return 1;
    }
    mf_write_deps(root, &kept);
    printf("removed %s\n", argv[0]);
    Resolve r = {root, true, NULL, false, false};
    return resolve(&r);
}

/* hphp deps   list the locked dependencies */
int project_cmd_deps(const char *root) {
    LockList l = {0};
    lock_read(root, &l);
    if (l.n == 0) {
        printf("no locked dependencies (run: hphp install)\n");
        return 0;
    }
    printf("%-20s %-10s %-10s %-5s %s\n", "PACKAGE", "VERSION", "TRUST", "DEV", "SOURCE");
    for (size_t i = 0; i < l.n; i++) {
        char *have = vendor_version(root, l.v[i].name);
        printf("%-20s %-10s %-10s %-5s %s%s\n", l.v[i].name, l.v[i].version,
               trust_of(l.v[i].source), l.v[i].dev ? "yes" : "",
               l.v[i].source ? l.v[i].source : "",
               have ? "" : "   (missing: run hphp install)");
        free(have);
    }
    return 0;
}

/* hphp test [filter]   run tests/<x>.hphp; each passes when it exits 0 */
int project_cmd_test(const char *root, int argc, char **argv, const char *self) {
    const char *filter = NULL;
    bool verbose = false;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) verbose = true;
        else filter = argv[i];
    }
    char *tdir = fmt("%s/tests", root);
    size_t n = 0;
    char **names = p_list(tdir, &n);
    int pass = 0, fail = 0;
    for (size_t i = 0; i < n; i++) {
        size_t ln = strlen(names[i]);
        if (ln < 6 || strcmp(names[i] + ln - 5, ".hphp")) continue;
        if (filter && !strstr(names[i], filter)) continue;
        char *path = fmt("%s/%s", tdir, names[i]);
#ifdef _WIN32
        const char *sink = verbose ? "" : " > NUL 2>&1";
        char *cmd = fmt("\"\"%s\" run \"%s\"%s\"", self, path, sink);
#else
        const char *sink = verbose ? "" : " > /dev/null 2>&1";
        char *cmd = fmt("'%s' run '%s'%s", self, path, sink);
#endif
        fflush(stdout);
        int rc = system(cmd);
        if (rc == 0) { printf("PASS %s\n", names[i]); pass++; }
        else {
            printf("FAIL %s   (details: hphp run tests/%s)\n", names[i], names[i]);
            fail++;
        }
        free(cmd);
        free(path);
    }
    if (pass + fail == 0) {
        printf("no tests in %s (add tests/<name>.hphp files that exit 0 when they pass)\n", tdir);
        free(tdir);
        return filter ? 1 : 0;
    }
    printf("%d passed, %d failed\n", pass, fail);
    free(tdir);
    return fail ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* hphp create                                                         */
/* ------------------------------------------------------------------ */

typedef struct { bool interactive; } Asker;

static char *ask(Asker *a, const char *question, const char *def) {
    if (!a->interactive) return xstrdup(def ? def : "");
    if (def && *def) printf("%s [%s]: ", question, def);
    else printf("%s: ", question);
    fflush(stdout);
    char line[512];
    if (!fgets(line, sizeof line, stdin)) { printf("\n"); return xstrdup(def ? def : ""); }
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = 0;
    char *s = line;
    while (*s == ' ') s++;
    return xstrdup(*s ? s : (def ? def : ""));
}

static bool ask_yes(Asker *a, const char *question, bool def) {
    char *q = fmt("%s (%s)", question, def ? "Y/n" : "y/N");
    char *ans = ask(a, q, "");
    bool yes = def;
    if (ans[0] == 'y' || ans[0] == 'Y' || ans[0] == 'j' || ans[0] == 'J') yes = true;
    else if (ans[0] == 'n' || ans[0] == 'N') yes = false;
    free(ans);
    free(q);
    return yes;
}

static const char *TEMPLATES[][2] = {
    {"console", "console app  - reads input, prints colored output"},
    {"web",     "web server   - small HTTP server with routes (uses the async library)"},
    {"lib",     "library      - a reusable package with tests, ready for hphp pkg publish"},
    {"empty",   "empty        - just hphp.yaml and an empty main file"},
};
#define N_TEMPLATES (sizeof TEMPLATES / sizeof TEMPLATES[0])

static int template_index(const char *name) {
    for (size_t i = 0; i < N_TEMPLATES; i++)
        if (!strcmp(TEMPLATES[i][0], name)) return (int)i;
    if (isdigit((unsigned char)name[0]) && !name[1]) {
        int k = name[0] - '1';
        if (k >= 0 && k < (int)N_TEMPLATES) return k;
    }
    return -1;
}

static char *sanitize_name(const char *s) {
    Buf b = {0};
    for (const char *p = s; *p; p++) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c == '-' || c == ' ' || c == '.') c = '_';
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') buf_putc(&b, c);
    }
    buf_putc(&b, 0);
    return b.data;
}

static bool project_name_ok(const char *n) {
    return n && pkg_name_ok(n) && n[0] >= 'a' && n[0] <= 'z' && strlen(n) <= 64;
}

static char *git_user_name(void) {
#ifdef _WIN32
    FILE *p = _popen("git config user.name 2>NUL", "r");
#else
    FILE *p = popen("git config user.name 2>/dev/null", "r");
#endif
    if (!p) return xstrdup("");
    char line[256] = {0};
    if (!fgets(line, sizeof line, p)) line[0] = 0;
#ifdef _WIN32
    _pclose(p);
#else
    pclose(p);
#endif
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    return xstrdup(line);
}

static void put_file(const char *dir, const char *rel, const char *text) {
    char *path = fmt("%s/%s", dir, rel);
    char *d = xstrdup(path);
    char *cut = strrchr(d, '/');
    if (cut) { *cut = 0; p_mkdir_p(d); }
    p_write(path, text);
    printf("  created %s\n", rel);
    free(d);
    free(path);
}

/* Replace every {{key}} in a template. */
static char *render(const char *tpl, const char **kv, size_t nkv) {
    Buf b = {0};
    for (const char *p = tpl; *p;) {
        if (p[0] == '{' && p[1] == '{') {
            const char *end = strstr(p + 2, "}}");
            bool hit = false;
            if (end) {
                for (size_t i = 0; i + 1 < nkv; i += 2) {
                    size_t kl = strlen(kv[i]);
                    if ((size_t)(end - p - 2) == kl && !strncmp(p + 2, kv[i], kl)) {
                        buf_puts(&b, kv[i + 1]);
                        p = end + 2;
                        hit = true;
                        break;
                    }
                }
            }
            if (hit) continue;
        }
        buf_putc(&b, *p++);
    }
    buf_putc(&b, 0);
    return b.data;
}

static const char *TPL_CONSOLE_MAIN =
    "<?php\n"
    "{{declare}}"
    "/* {{name}} - {{description}}\n"
    " *\n"
    " *   hphp run          run it\n"
    " *   hphp test         run tests/*.hphp\n"
    " *   hphp build        build a native executable\n"
    " */\n"
    "import \"./greeting.hphp\";\n"
    "\n"
    "$name = input(\"What is your name? \");\n"
    "println(greeting($name), GREEN);\n";

static const char *TPL_CONSOLE_GREETING =
    "<?php\n"
    "{{declare}}"
    "/* Keep logic in small functions like this one: tests/ can import and\n"
    " * check them without running the whole program. */\n"
    "function greeting(string $name): string {\n"
    "    $who = trim($name);\n"
    "    if ($who == \"\") {\n"
    "        $who = \"world\";\n"
    "    }\n"
    "    return \"Hello, \" . $who . \"!\";\n"
    "}\n";

static const char *TPL_CONSOLE_TEST =
    "<?php\n"
    "/* A test passes when it exits with 0. `hphp test` runs every file here. */\n"
    "import \"testing\";\n"
    "import \"../src/greeting.hphp\";\n"
    "\n"
    "$t = new Test(\"greeting\");\n"
    "$t->equal(greeting(\"Ada\"), \"Hello, Ada!\", \"greets by name\");\n"
    "$t->equal(greeting(\"  Ada \"), \"Hello, Ada!\", \"trims input\");\n"
    "$t->equal(greeting(\"\"), \"Hello, world!\", \"empty means world\");\n"
    "$t->done();\n";

static const char *TPL_WEB_MAIN =
    "<?php\n"
    "{{declare}}"
    "/* {{name}} - {{description}}\n"
    " *\n"
    " *   hphp install      fetch the async library into .hphp/packages/ (once)\n"
    " *   hphp run          start the server, then open http://127.0.0.1:8080\n"
    " */\n"
    "import \"async\";\n"
    "import \"./routes.hphp\";\n"
    "\n"
    "$port = 8080;\n"
    "$loop = new Loop();\n"
    "$loop->listen(\"tcp://127.0.0.1:\" . $port, function($client, $peer) use ($loop) {\n"
    "    /* the loop reads the socket and hands each chunk to the watcher */\n"
    "    $loop->watch($client, function($sock, $chunk, $eof) use ($loop) {\n"
    "        if ($eof) {\n"
    "            return;\n"
    "        }\n"
    "        $k = intval($sock);\n"
    "        $request = $chunk;\n"
    "        if (array_key_exists($k, $loop->bufs)) {\n"
    "            $request = $loop->bufs[$k] . $chunk;\n"
    "        }\n"
    "        if (strpos($request, \"\\r\\n\\r\\n\") === false) {\n"
    "            $loop->bufs[$k] = $request;     /* headers not complete yet */\n"
    "            return;\n"
    "        }\n"
    "        stream_send($sock, handle_request($request));\n"
    "        $loop->unwatch($sock);\n"
    "        stream_close($sock);\n"
    "    });\n"
    "});\n"
    "echo \"{{name}} listening on http://127.0.0.1:\" . $port;\n"
    "$loop->run();\n";

static const char *TPL_WEB_ROUTES =
    "<?php\n"
    "{{declare}}"
    "/* Request handling without sockets, so tests/ can call it directly. */\n"
    "\n"
    "function response(int $status, string $type, string $body): string {\n"
    "    $reason = \"OK\";\n"
    "    if ($status == 404) {\n"
    "        $reason = \"Not Found\";\n"
    "    }\n"
    "    return \"HTTP/1.1 \" . $status . \" \" . $reason . \"\\r\\n\"\n"
    "        . \"Content-Type: \" . $type . \"\\r\\n\"\n"
    "        . \"Content-Length: \" . strlen($body) . \"\\r\\n\"\n"
    "        . \"Connection: close\\r\\n\\r\\n\" . $body;\n"
    "}\n"
    "\n"
    "/* \"GET /hello?name=Ada HTTP/1.1\" -> \"/hello\" */\n"
    "function request_path(string $request): string {\n"
    "    $lines = explode(\"\\r\\n\", $request);\n"
    "    $parts = explode(\" \", $lines[0]);\n"
    "    if (count($parts) < 2) {\n"
    "        return \"/\";\n"
    "    }\n"
    "    $path = $parts[1];\n"
    "    $q = strpos($path, \"?\");\n"
    "    if ($q !== false) {\n"
    "        $path = substr($path, 0, $q);\n"
    "    }\n"
    "    return $path;\n"
    "}\n"
    "\n"
    "function handle_request(string $request): string {\n"
    "    $path = request_path($request);\n"
    "    if ($path == \"/\") {\n"
    "        return response(200, \"text/html; charset=utf-8\",\n"
    "            \"<h1>{{name}}</h1><p>Served by HolyPHP. Try <a href='/api/status'>/api/status</a>.</p>\");\n"
    "    }\n"
    "    if ($path == \"/api/status\") {\n"
    "        return response(200, \"application/json\", \"{\\\"status\\\":\\\"ok\\\",\\\"app\\\":\\\"{{name}}\\\"}\");\n"
    "    }\n"
    "    return response(404, \"text/plain\", \"not found\");\n"
    "}\n";

static const char *TPL_WEB_TEST =
    "<?php\n"
    "import \"testing\";\n"
    "import \"../src/routes.hphp\";\n"
    "\n"
    "$t = new Test(\"routes\");\n"
    "$t->equal(request_path(\"GET /api/status?x=1 HTTP/1.1\\r\\n\\r\\n\"), \"/api/status\", \"path without query\");\n"
    "$t->contains(handle_request(\"GET / HTTP/1.1\\r\\n\\r\\n\"), \"200 OK\", \"home page\");\n"
    "$t->contains(handle_request(\"GET /api/status HTTP/1.1\\r\\n\\r\\n\"), \"application/json\", \"status is JSON\");\n"
    "$t->contains(handle_request(\"GET /nope HTTP/1.1\\r\\n\\r\\n\"), \"404\", \"unknown path\");\n"
    "$t->done();\n";

static const char *TPL_LIB_MAIN =
    "<?php\n"
    "{{declare}}"
    "/* {{name}} - {{description}}\n"
    " *\n"
    " *   import \"{{name}}\";\n"
    " *   echo {{name}}_slug(\"Hello World\");   // hello-world\n"
    " */\n"
    "\n"
    "/* Lowercase letters and digits; everything else becomes one dash. */\n"
    "function {{name}}_slug(string $text): string {\n"
    "    $out = \"\";\n"
    "    $dash = false;\n"
    "    $lower = strtolower($text);\n"
    "    for ($i = 0; $i < strlen($lower); $i++) {\n"
    "        $c = substr($lower, $i, 1);\n"
    "        $o = ord($c);\n"
    "        if (($o >= 97 && $o <= 122) || ($o >= 48 && $o <= 57)) {\n"
    "            $out = $out . $c;\n"
    "            $dash = false;\n"
    "        } elseif (!$dash && $out != \"\") {\n"
    "            $out = $out . \"-\";\n"
    "            $dash = true;\n"
    "        }\n"
    "    }\n"
    "    return rtrim($out, \"-\");\n"
    "}\n";

static const char *TPL_LIB_TEST =
    "<?php\n"
    "import \"testing\";\n"
    "import \"../src/{{name}}.hphp\";\n"
    "\n"
    "$t = new Test(\"{{name}}\");\n"
    "$t->equal({{name}}_slug(\"Hello World\"), \"hello-world\", \"spaces\");\n"
    "$t->equal({{name}}_slug(\"  A -- b!! \"), \"a-b\", \"punctuation\");\n"
    "$t->equal({{name}}_slug(\"\"), \"\", \"empty\");\n"
    "$t->done();\n";

static const char *TPL_EMPTY_MAIN =
    "<?php\n"
    "{{declare}}"
    "echo \"Hello from {{name}}\";\n";

static const char *TPL_README =
    "# {{name}}\n"
    "\n"
    "{{description}}\n"
    "\n"
    "## Commands\n"
    "\n"
    "```\n"
    "hphp install      # fetch the dependencies from hphp.yaml into .hphp/packages/\n"
    "hphp run          # run {{entry}}\n"
    "hphp test         # run every tests/*.hphp (a test passes when it exits 0)\n"
    "hphp build        # build a native executable\n"
    "{{lib_cmds}}"
    "```\n"
    "\n"
    "## Layout\n"
    "\n"
    "| Path | What |\n"
    "|---|---|\n"
    "| `hphp.yaml` | name, version, entry file and dependencies (`require`, `require-dev`) |\n"
    "| `hphp.lock` | exact versions and checksums, written by `hphp install`; commit it |\n"
    "| `src/` | the code; `{{entry}}` is the entry file |\n"
    "| `tests/` | test programs run by `hphp test` |\n"
    "| `.hphp/` | installed dependencies (`.hphp/packages/`), recreated by `hphp install`; not committed |\n";

static const char *TPL_GITIGNORE =
    "/.hphp/\n"
    "/build/\n"
    "*.exe\n"
    "*.o\n";

int project_cmd_create(int argc, char **argv) {
    const char *arg_name = NULL, *arg_tpl = NULL, *arg_desc = NULL, *arg_author = NULL;
    const char *arg_license = NULL;
    bool yes = false, safe = false, safe_set = false, no_git = false, no_install = false;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if ((!strcmp(a, "--template") || !strcmp(a, "-t")) && i + 1 < argc) arg_tpl = argv[++i];
        else if (!strcmp(a, "--description") && i + 1 < argc) arg_desc = argv[++i];
        else if (!strcmp(a, "--author") && i + 1 < argc) arg_author = argv[++i];
        else if (!strcmp(a, "--license") && i + 1 < argc) arg_license = argv[++i];
        else if (!strcmp(a, "--yes") || !strcmp(a, "-y")) yes = true;
        else if (!strcmp(a, "--safe")) { safe = true; safe_set = true; }
        else if (!strcmp(a, "--no-safe")) { safe = false; safe_set = true; }
        else if (!strcmp(a, "--no-git")) no_git = true;
        else if (!strcmp(a, "--no-install")) no_install = true;
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            printf("usage: hphp create [name] [options]\n\n"
                   "Asks a few questions and creates a new project folder. Without a\n"
                   "terminal (or with --yes) it uses the defaults and the options below.\n\n"
                   "  -t, --template <t>   console | web | lib | empty   (default: console)\n"
                   "  --description <s>    one-line description\n"
                   "  --author <s>         author (default: git config user.name)\n"
                   "  --license <s>        license id (default: MIT)\n"
                   "  --safe / --no-safe   start files with declare(enforce_memory_safety=1);\n"
                   "  --no-git             do not run git init\n"
                   "  --no-install         do not install dependencies now\n"
                   "  -y, --yes            accept every default, ask nothing\n");
            return 0;
        }
        else if (a[0] == '-') { fprintf(stderr, "hphp create: unknown option %s (see: hphp create --help)\n", a); return 1; }
        else if (!arg_name) arg_name = a;
    }
    Asker as = {!yes && hp_isatty(hp_fileno(stdin))};
    if (as.interactive) printf("New HolyPHP project (Enter keeps the [default])\n\n");

    char *name = NULL;
    for (;;) {
        char *raw = arg_name ? xstrdup(arg_name) : ask(&as, "Project name", "my_app");
        name = sanitize_name(raw);
        if (strcmp(raw, name) && *name) printf("  using \"%s\" (lowercase letters, digits and _)\n", name);
        free(raw);
        if (project_name_ok(name)) break;
        fprintf(stderr, "  a name starts with a letter and uses lowercase letters, digits and _\n");
        if (!as.interactive || arg_name) return 1;
        free(name);
    }
    if (p_is_dir(name) || p_is_file(name)) {
        fprintf(stderr, "hphp create: \"%s\" already exists here\n", name);
        return 1;
    }

    int ti = -1;
    if (arg_tpl) {
        ti = template_index(arg_tpl);
        if (ti < 0) {
            fprintf(stderr, "hphp create: unknown template \"%s\" (console, web, lib, empty)\n", arg_tpl);
            return 1;
        }
    } else if (as.interactive) {
        printf("Template:\n");
        for (size_t i = 0; i < N_TEMPLATES; i++) printf("  %zu) %s\n", i + 1, TEMPLATES[i][1]);
        while (ti < 0) {
            char *t = ask(&as, "Choose 1-4", "1");
            ti = template_index(t);
            free(t);
        }
    } else {
        ti = 0;
    }
    const char *tpl = TEMPLATES[ti][0];
    bool lib = !strcmp(tpl, "lib");
    bool web = !strcmp(tpl, "web");

    const char *defdesc = lib ? "A HolyPHP library." : web ? "A HolyPHP web server." : "A HolyPHP program.";
    char *desc = arg_desc ? xstrdup(arg_desc) : ask(&as, "Description", defdesc);
    char *gitname = git_user_name();
    char *author = arg_author ? xstrdup(arg_author) : ask(&as, "Author", gitname);
    char *license = arg_license ? xstrdup(arg_license) : ask(&as, "License", "MIT");
    if (!safe_set && as.interactive)
        safe = ask_yes(&as, "Strict memory safety (Rust-like borrow checks in every file)?", false);
    bool git = !no_git && (!as.interactive || ask_yes(&as, "Initialize a git repository?", true));

    char *entry = lib ? fmt("src/%s.hphp", name) : xstrdup("src/main.hphp");
    const char *declare = safe ? "declare(enforce_memory_safety=1);\n" : "";
    const char *lib_cmds = lib ? "hphp pkg check    # type-check the package entry\n"
                                 "hphp pkg publish  # upload it to the registry\n" : "";
    const char *kv[] = {"name", name, "description", desc, "declare", declare,
                        "entry", entry, "lib_cmds", lib_cmds};
    size_t nkv = sizeof kv / sizeof kv[0];

    printf("\ncreating %s/ (%s)\n", name, tpl);
    p_mkdir_p(name);

    Buf mf = {0};
    buf_puts(&mf, "# HolyPHP project file - https://hphp.ch/pkg.html\n");
    buf_puts(&mf, "name: ");        y_put(&mf, name);
    buf_puts(&mf, "\nversion: 0.1.0\ntype: ");
    buf_puts(&mf, lib ? "library" : "project");
    buf_puts(&mf, "\ndescription: "); y_put(&mf, desc);
    buf_puts(&mf, "\nentry: ");       y_put(&mf, entry);
    buf_puts(&mf, "\nauthor: ");      y_put(&mf, *author ? author : "unknown");
    buf_puts(&mf, "\nlicense: ");     y_put(&mf, license);
    buf_puts(&mf, "\n\n# packages this project needs: name: \"version constraint\"\n");
    buf_puts(&mf, "# (\"*\" any, \"^1.2\" 1.x from 1.2, \"~1.2\" 1.2.x, \"path:../lib\" a local folder)\n");
    buf_puts(&mf, web ? "require:\n  async: \"*\"\n" : "require: {}\n");
    buf_puts(&mf, "\n# only for development and tests\n");
    buf_puts(&mf, strcmp(tpl, "empty") ? "require-dev:\n  testing: \"*\"\n" : "require-dev: {}\n");
    buf_putc(&mf, 0);
    put_file(name, PROJECT_MANIFEST, mf.data);
    free(mf.data);

    struct { const char *rel; const char *tpl; } files[3];
    size_t nf = 0;
    if (!strcmp(tpl, "console")) {
        files[nf].rel = "src/main.hphp";            files[nf++].tpl = TPL_CONSOLE_MAIN;
        files[nf].rel = "src/greeting.hphp";        files[nf++].tpl = TPL_CONSOLE_GREETING;
        files[nf].rel = "tests/greeting_test.hphp"; files[nf++].tpl = TPL_CONSOLE_TEST;
    } else if (web) {
        files[nf].rel = "src/main.hphp";            files[nf++].tpl = TPL_WEB_MAIN;
        files[nf].rel = "src/routes.hphp";          files[nf++].tpl = TPL_WEB_ROUTES;
        files[nf].rel = "tests/routes_test.hphp";   files[nf++].tpl = TPL_WEB_TEST;
    } else if (lib) {
        files[nf].rel = NULL;                       files[nf++].tpl = TPL_LIB_MAIN;
        files[nf].rel = NULL;                       files[nf++].tpl = TPL_LIB_TEST;
    } else {
        files[nf].rel = "src/main.hphp";            files[nf++].tpl = TPL_EMPTY_MAIN;
    }
    for (size_t i = 0; i < nf; i++) {
        char *rel = files[i].rel ? xstrdup(files[i].rel)
                  : (i == 0 ? xstrdup(entry) : fmt("tests/%s_test.hphp", name));
        char *text = render(files[i].tpl, kv, nkv);
        put_file(name, rel, text);
        free(text);
        free(rel);
    }
    char *readme = render(TPL_README, kv, nkv);
    put_file(name, "README.md", readme);
    free(readme);
    put_file(name, ".gitignore", TPL_GITIGNORE);
    char *testdir = fmt("%s/tests", name);
    p_mkdir_p(testdir);
    free(testdir);

    char *root = p_abs_dir(name);
    int rc = 0;
    bool has_deps = strcmp(tpl, "empty") != 0;     /* testing, and async for web */
    if (has_deps && !no_install) {
        printf("\ninstalling dependencies\n");
        Resolve r = {root, false, NULL, false, false};
        rc = resolve(&r);
    } else if (!has_deps) {
        Resolve r = {root, false, NULL, false, true};
        rc = resolve(&r);              /* writes an empty hphp.lock */
        printf("  created " PROJECT_LOCK "\n");
    }

    if (git) {
        char *cmd = fmt("git init -q \"%s\"", name);
        if (system(cmd) == 0) printf("  initialized a git repository\n");
        else printf("  (git init failed; is git installed?)\n");
        free(cmd);
    }

    printf("\ndone. next steps:\n\n  cd %s\n", name);
    if (has_deps && no_install) printf("  hphp install\n");
    if (lib) printf("  hphp test\n  hphp pkg check\n");
    else printf("  hphp run\n  hphp test\n");
    printf("\n");

    free(root); free(entry); free(desc); free(author);
    free(gitname); free(license); free(name);
    return rc;
}

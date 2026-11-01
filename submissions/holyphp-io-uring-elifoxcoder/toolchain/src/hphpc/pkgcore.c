/* pkgcore.c — HolyPHP package manager core: manifests, pkg home, .hpx files.
 *
 * Zero dependencies beyond libc/Win32; all I/O is plain C stdio. The .hpx
 * format is deliberately trivial (tar-like): a sequence of
 *   name \0 size(ascii) \n payload...
 * followed by "HPHX1\n" terminator. Good enough, tiny, and portable.
 */
#define _DEFAULT_SOURCE 1   /* S_IFDIR/S_IFREG, dirent on glibc */
#include "pkgcore.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>       /* opendir/readdir for pkg list/cache dirs */
#include <unistd.h>       /* rmdir */
#include <ctype.h>

#ifdef _WIN32
#include <direct.h>
#include <shlobj.h>
#else
#include <sys/types.h>
#endif

/* ------------------------------------------------------------------ */
/* small fs helpers                                                    */

static void mkdir_p(const char *path) {
    char tmp[1024];
    size_t n = strlen(path);
    if (n >= sizeof tmp) return;
    memcpy(tmp, path, n + 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char c = *p;
            *p = 0;
#ifdef _WIN32
            _mkdir(tmp);
#else
            mkdir(tmp, 0755);
#endif
            *p = c;
        }
    }
#ifdef _WIN32
    _mkdir(tmp);
#else
    mkdir(tmp, 0755);
#endif
}

static bool is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && (st.st_mode & S_IFDIR);
}

static bool is_file(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && (st.st_mode & S_IFREG);
}

/* Dynamic char buffer: reuse the shared Buf from util.h */

static char *read_all(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    char *d = xmalloc((size_t)sz + 1);
    size_t rd = fread(d, 1, (size_t)sz, f);
    fclose(f);
    d[rd] = 0;
    if (out_len) *out_len = rd;
    return d;
}

static void write_all(const char *path, const void *d, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) fatal("cannot write %s", path);
    if (n) fwrite(d, 1, n, f);
    fclose(f);
}

static void write_all_mkdir(const char *path, const void *d, size_t n) {
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    char *slash = strrchr(tmp, '/');
#ifdef _WIN32
    if (!slash) slash = strrchr(tmp, '\\');
#endif
    if (slash) { *slash = 0; mkdir_p(tmp); }
    write_all(path, d, n);
}

bool pkg_remove_tree(const char *path) {
    /* recursive delete; returns true when path is gone */
    if (is_dir(path)) {
        char pattern[1024], item[1024];
        snprintf(pattern, sizeof pattern, "%s/*", path);
#ifdef _WIN32
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
                snprintf(item, sizeof item, "%s/%s", path, fd.cFileName);
                pkg_remove_tree(item);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        return RemoveDirectoryA(path) != 0;
#else
        DIR *dp = opendir(path);
        if (!dp) return false;
        struct dirent *de;
        while ((de = readdir(dp))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            snprintf(item, sizeof item, "%s/%s", path, de->d_name);
            pkg_remove_tree(item);
        }
        closedir(dp);
        return rmdir(path) == 0;
#endif
    }
    return remove(path) == 0;
}

/* ------------------------------------------------------------------ */
/* manifest                                                            */

bool pkg_name_ok(const char *name) {
    if (!name || !*name) return false;
    for (const char *p = name; *p; p++) {
        char c = *p;
        if (!(islower((unsigned char)c) || isdigit((unsigned char)c) || c == '_'))
            return false;
    }
    return true;
}

int pkg_version_cmp(const char *a, const char *b) {
    long av[3] = {0,0,0}, bv[3] = {0,0,0};
    sscanf(a, "%ld.%ld.%ld", &av[0], &av[1], &av[2]);
    sscanf(b, "%ld.%ld.%ld", &bv[0], &bv[1], &bv[2]);
    for (int i = 0; i < 3; i++) {
        if (av[i] != bv[i]) return av[i] < bv[i] ? -1 : 1;
    }
    return 0;
}

/* Fetch "key": "value" (first occurrence) from a flat JSON object. */
static char *json_get_str(const char *js, const char *key) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *k = strstr(js, pat);
    if (!k) return NULL;
    const char *p = k + strlen(pat);
    while (*p && (*p == ' ' || *p == ':' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    if (*p != '"') return NULL;
    p++;
    Buf b = {0};
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) p++;   /* skip escaped char (no \u decoding needed) */
        buf_write(&b, p, 1);
        p++;
    }
    buf_putc(&b, 0);
    return b.data;
}

PkgManifest pkg_manifest_parse(const char *js, const char *dir) {
    PkgManifest m = {0};
    if (!js || !*js) fatal("%s: empty or missing " PKG_MANIFEST, dir ? dir : "package");
    m.name    = json_get_str(js, "name");
    m.version = json_get_str(js, "version");
    m.entry   = json_get_str(js, "entry");
    m.desc    = json_get_str(js, "description");
    m.author  = json_get_str(js, "author");
    if (!m.name || !pkg_name_ok(m.name))
        fatal("%s: manifest needs a valid \"name\" (lowercase letters, digits, _)", dir ? dir : "package");
    if (!m.version || !*m.version)
        fatal("%s: manifest needs a \"version\" (e.g. \"1.0.0\")", dir ? dir : "package");
    if (!m.entry) m.entry = fmt("%s.hphp", m.name);
    if (!m.desc)  m.desc  = xstrdup("");
    if (!m.author) m.author = xstrdup("");
    return m;
}

void pkg_manifest_free(PkgManifest *m) {
    free(m->name); free(m->version); free(m->entry);
    free(m->desc); free(m->author);
}

/* ------------------------------------------------------------------ */
/* pkg home                                                            */

char *pkg_home(void) {
    const char *ov = getenv("HPHP_HOME");
    if (ov && *ov) {
        char *h = fmt("%s", ov);
        mkdir_p(h);
        return h;
    }
    char base[768];
#ifdef _WIN32
    base[0] = 0;
    if (!SHGetSpecialFolderPathA(NULL, base, CSIDL_APPDATA, FALSE) || !base[0]) {
        const char *ad = getenv("APPDATA");
        snprintf(base, sizeof base, "%s", ad ? ad : "C:/hphp-data");
    }
#else
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && *xdg) snprintf(base, sizeof base, "%s", xdg);
    else {
        const char *h = getenv("HOME");
        snprintf(base, sizeof base, "%s/.local/share", h ? h : ".");
    }
#endif
    char *home = fmt("%s/hphp", base);
    mkdir_p(home);
    return home;
}

char *pkg_home_file(const char *rel) {
    char *h = pkg_home();
    char *p = fmt("%s/%s", h, rel);
    free(h);
    return p;
}

char *pkg_dir_of(const char *name) {
    return pkg_home_file(fmt("packages/%s", name));
}

char *pkg_registry_url(void) {
    char *f = pkg_home_file(PKG_REGISTRY_FILE);
    char *v = read_all(f, NULL);
    free(f);
    if (!v) return NULL;
    /* trim whitespace/newlines */
    char *end = v + strlen(v);
    while (end > v && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
    char *s = v;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) { free(v); return NULL; }
    memmove(v, s, strlen(s) + 1);
    return v;
}

/* ------------------------------------------------------------------ */
/* registry.json (installed packages)                                  */

static char *slurp_file_or(const char *path, const char *fallback) {
    size_t n;
    char *d = read_all(path, &n);
    if (!d) return xstrdup(fallback);
    return d;
}

InstalledPkg *pkg_registry_read(size_t *n_out) {
    char *f = pkg_home_file(PKG_INDEX);
    char *d = slurp_file_or(f, "[]");
    free(f);
    /* parse a flat array of {"name":"x","version":"1.2.3"} */
    size_t cap = 8, n = 0;
    InstalledPkg *v = xmalloc(cap * sizeof *v);
    const char *p = d;
    while ((p = strstr(p, "\"name\""))) {
        char *nm = json_get_str(p, "name");
        char *vr = json_get_str(p, "version");
        if (!nm) break;
        if (n == cap) { cap *= 2; v = xrealloc(v, cap * sizeof *v); }
        v[n].name = nm;
        v[n].version = vr ? vr : xstrdup("");
        n++;
        p += 6;
    }
    free(d);
    *n_out = n;
    return v;
}

void pkg_registry_add(const char *name, const char *version) {
    size_t n;
    InstalledPkg *v = pkg_registry_read(&n);
    Buf b = {0};
    buf_puts(&b, "[\n");
    bool replaced = false;
    for (size_t i = 0; i < n; i++) {
        if (!strcmp(v[i].name, name)) {
            v[i].version = xstrdup(version);   /* upgrade in place */
            replaced = true;
        }
        buf_printf(&b, "  {\"name\": \"%s\", \"version\": \"%s\"}%s\n",
                   v[i].name, v[i].version, i + 1 < n ? "," : "");
    }
    if (!replaced)
        buf_printf(&b, "  {\"name\": \"%s\", \"version\": \"%s\"}%s\n",
                   name, version, n ? "," : "");
    buf_puts(&b, "]\n");
    char *f = pkg_home_file(PKG_INDEX);
    write_all(f, b.data, b.len);
    free(f); free(b.data);
}

void pkg_registry_remove(const char *name) {
    size_t n;
    InstalledPkg *v = pkg_registry_read(&n);
    Buf b = {0};
    buf_puts(&b, "[\n");
    size_t kept = 0;
    for (size_t i = 0; i < n; i++) {
        if (!strcmp(v[i].name, name)) continue;
        buf_printf(&b, "  {\"name\": \"%s\", \"version\": \"%s\"}%s\n",
                   v[i].name, v[i].version, kept + 1 < n ? "," : "");
        kept++;
    }
    buf_puts(&b, "]\n");
    char *f = pkg_home_file(PKG_INDEX);
    write_all(f, b.data, b.len);
    free(f); free(b.data);
}

bool pkg_is_installed(const char *name) {
    size_t n;
    InstalledPkg *v = pkg_registry_read(&n);
    bool found = false;
    for (size_t i = 0; i < n; i++)
        if (!strcmp(v[i].name, name)) { found = true; break; }
    return found;
}

char *pkg_installed_version(const char *name) {
    size_t n;
    InstalledPkg *v = pkg_registry_read(&n);
    char *ver = NULL;
    for (size_t i = 0; i < n; i++)
        if (!strcmp(v[i].name, name)) { ver = xstrdup(v[i].version); break; }
    return ver;
}

/* ------------------------------------------------------------------ */
/* .hpx archive                                                        */

typedef struct {
    char *files[1024];
    size_t lens[1024];
    size_t n;
} PackList;

static void collect_dir(const char *dir, const char *rel, PackList *pl) {
    char pattern[1024];
    snprintf(pattern, sizeof pattern, "%s%s%s",
             dir, rel[0] ? "/" : "", rel[0] ? rel : "");
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(fmt("%s/*", pattern), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        if (!strcmp(fd.cFileName, "hphp.lock")) continue;
        char *sub = rel[0] ? fmt("%s/%s", rel, fd.cFileName) : fmt("%s", fd.cFileName);
        char *full = fmt("%s/%s", dir, sub);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) collect_dir(dir, sub, pl);
        else if (pl->n < 1024) {
            size_t ln;
            char *d = read_all(full, &ln);
            if (d) { pl->files[pl->n] = xstrdup(sub); pl->lens[pl->n] = ln; pl->n++; }
            free(d);
        }
        free(sub); free(full);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *dp = opendir(pattern);
    if (!dp) return;
    struct dirent *de;
    while ((de = readdir(dp))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (!strcmp(de->d_name, "hphp.lock")) continue;
        char *sub = rel[0] ? fmt("%s/%s", rel, de->d_name) : fmt("%s", de->d_name);
        char *full = fmt("%s/%s", dir, sub);
        struct stat st;
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) collect_dir(dir, sub, pl);
        else if (pl->n < 1024) {
            size_t ln;
            char *d = read_all(full, &ln);
            if (d) { pl->files[pl->n] = xstrdup(sub); pl->lens[pl->n] = ln; pl->n++; }
            free(d);
        }
        free(sub); free(full);
    }
    closedir(dp);
#endif
}

void pkg_pack_dir(const char *dir, const char *hpx_path) {
    { char tmp[1024]; snprintf(tmp, sizeof tmp, "%s", hpx_path); char *sl = strrchr(tmp, '/'); if (sl) { *sl = 0; mkdir_p(tmp); } }
    if (!is_dir(dir)) fatal("not a directory: %s", dir);
    PackList pl = {0};
    collect_dir(dir, "", &pl);
    Buf b = {0};
    for (size_t i = 0; i < pl.n; i++) {
        size_t ln = pl.lens[i];
        buf_printf(&b, "%s %zu\n", pl.files[i], ln);
        char *full = fmt("%s/%s", dir, pl.files[i]);
        size_t got;
        char *d = read_all(full, &got);
        buf_write(&b, d ? d : "", got);
        free(d); free(full);
    }
    buf_puts(&b, "HPHX1\n");
    write_all(hpx_path, b.data, b.len);
    free(b.data);
}

static void path_mkdir_for(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    char *slash = strrchr(tmp, '/');
#ifdef _WIN32
    if (!slash) { slash = strrchr(tmp, '\\'); }
    else { char *bs = strrchr(tmp, '\\'); if (bs && bs > slash) slash = bs; }
#endif
    if (slash) {
        *slash = 0;
        mkdir_p(tmp);
    }
}

int pkg_extract(const char *hpx_path, const char *destdir) {
    size_t n;
    char *d = read_all(hpx_path, &n);
    if (!d) fatal("cannot read %s", hpx_path);
    mkdir_p(destdir);
    char *p = d, *end = d + n;
    int files = 0;
    while (p < end) {
        /* header: "<name> <size>\n" */
        char *nl = memchr(p, '\n', (size_t)(end - p));
        if (!nl) break;
        char hdr[1024];
        size_t hl = (size_t)(nl - p);
        if (hl >= sizeof hdr) break;
        memcpy(hdr, p, hl); hdr[hl] = 0;
        p = nl + 1;
        if (!strcmp(hdr, "HPHX1")) break;
        char *sp = strchr(hdr, ' ');
        if (!sp) break;
        *sp = 0;
        const char *name = hdr;
        unsigned long long sz = strtoull(sp + 1, NULL, 10);
        if (p + sz > end) break;
        /* sanitize: no absolute paths / .. */
        if (name[0] == '/' || strstr(name, "..")) { p += sz; continue; }
        char *out = fmt("%s/%s", destdir, name);
        path_mkdir_for(out);
        write_all(out, p, (size_t)sz);
        free(out);
        p += sz;
        files++;
    }
    free(d);
    return files;
}

char **pkg_list_entries(const char *hpx_path, size_t *n_out) {
    size_t n;
    char *d = read_all(hpx_path, &n);
    if (!d) fatal("cannot read %s", hpx_path);
    char **names = xmalloc(sizeof(char *) * 64);
    size_t cap = 64, cnt = 0;
    char *p = d, *end = d + n;
    while (p < end) {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        if (!nl) break;
        char hdr[1024];
        size_t hl = (size_t)(nl - p);
        if (hl >= sizeof hdr) break;
        memcpy(hdr, p, hl); hdr[hl] = 0;
        p = nl + 1;
        if (!strcmp(hdr, "HPHX1")) break;
        char *sp = strchr(hdr, ' ');
        if (!sp) break;
        *sp = 0;
        unsigned long long sz = strtoull(sp + 1, NULL, 10);
        if (cnt == cap) { cap *= 2; names = xrealloc(names, cap * sizeof(char *)); }
        names[cnt++] = xstrdup(hdr);
        p += sz;
    }
    free(d);
    *n_out = cnt;
    return names;
}

/* ------------------------------------------------------------------ */
/* search                                                              */

int pkg_search(const char *query) {
    size_t n;
    InstalledPkg *v = pkg_registry_read(&n);
    int matches = 0;
    printf("%-16s %-10s %s\n", "PACKAGE", "VERSION", "SOURCE");
    for (size_t i = 0; i < n; i++) {
        if (query && *query && !strstr(v[i].name, query)) continue;
        char *dir = pkg_dir_of(v[i].name);
        char *desc = NULL;
        char *mf = fmt("%s/" PKG_MANIFEST, dir);
        size_t mn;
        char *mjs = read_all(mf, &mn);
        if (mjs) desc = json_get_str(mjs, "description");
        printf("%-16s %-10s %s%s\n", v[i].name, v[i].version, "installed",
               desc && *desc ? " — " : "");
        if (desc && *desc) printf("%-28s%s\n", "", desc);
        matches++;
        free(mjs); free(mf); free(dir);
    }
    /* cached .hpx files (downloaded or packed) */
    char *cache = pkg_home_file("cache");
    if (is_dir(cache)) {
#ifdef _WIN32
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(fmt("%s/*.hpx", cache), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (query && *query && !strstr(fd.cFileName, query)) continue;
                char *full = fmt("%s/%s", cache, fd.cFileName);
                size_t cnt;
                char **entries = pkg_list_entries(full, &cnt);
                char *mfname = NULL;
                for (size_t i = 0; i < cnt; i++)
                    if (!strcmp(entries[i], PKG_MANIFEST)) mfname = entries[i];
                printf("%-16s %-10s %s\n", "(cached)", "", fd.cFileName);
                matches++;
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
#else
        DIR *dp = opendir(cache);
        if (dp) {
            struct dirent *de;
            while ((de = readdir(dp))) {
                if (!strstr(de->d_name, ".hpx")) continue;
                if (query && *query && !strstr(de->d_name, query)) continue;
                printf("%-16s %-10s %s\n", "(cached)", "", de->d_name);
                matches++;
            }
            closedir(dp);
        }
#endif
    }
    free(cache);
    return matches;
}

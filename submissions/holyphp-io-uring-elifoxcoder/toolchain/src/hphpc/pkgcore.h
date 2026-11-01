/* pkgcore.h — HolyPHP package manager core.
 *
 * A package is a directory with hphp.json + .hphp sources. A .hpx file is a
 * tar-like archive of that directory. Installed packages live in the pkg
 * home:
 *
 *   <home>/hphp/packages/<name>/<version>/   (extracted package)
 *   <home>/hphp/registry.json                (list of installed packages)
 *
 * The default home is %APPDATA% on Windows / $XDG_DATA_HOME or ~/.local/share
 * elsewhere; HPHP_HOME overrides it (also where the registry URL is set:
 * <home>/hphp/registry_url).
 *
 * Networking is optional and best-effort: without a registry URL (or when the
 * registry is unreachable) everything still works against local .hpx files.
 */
#ifndef HPHPC_PKGCORE_H
#define HPHPC_PKGCORE_H

#include <stdbool.h>
#include <stddef.h>

#define PKG_MANIFEST      "hphp.json"
#define PKG_INDEX         "registry.json"
#define PKG_REGISTRY_FILE "registry_url"

typedef struct {
    char *name;        /* e.g. "mathx" (lowercase letters, digits, _) */
    char *version;     /* semver-ish, e.g. "1.2.0" */
    char *entry;       /* main file inside the package, e.g. "mathx.hphp" */
    char *desc;
    char *author;
} PkgManifest;

typedef struct {
    char *name;
    char *version;     /* installed version, "" if unknown */
} InstalledPkg;

/* --- manifest --- */
/* Parse hphp.json content. Fatal on malformed manifest. */
PkgManifest pkg_manifest_parse(const char *json_text, const char *dir);
void pkg_manifest_free(PkgManifest *m);
bool pkg_name_ok(const char *name);
/* version_cmp: <0, 0, >0 as semver comparison (numeric per component). */
int pkg_version_cmp(const char *a, const char *b);

/* --- paths --- */
char *pkg_home(void);                 /* <home>/hphp (created if missing) */
char *pkg_home_file(const char *rel); /* pkg_home() + "/" + rel */
char *pkg_dir_of(const char *name);   /* packages dir for one pkg (may not exist) */
char *pkg_registry_url(void);         /* NULL when unset; reads registry_url file */

/* --- registry.json (installed packages) --- */
InstalledPkg *pkg_registry_read(size_t *n);
void pkg_registry_add(const char *name, const char *version);
void pkg_registry_remove(const char *name);
bool pkg_is_installed(const char *name);
/* Best installed version of `name`, or NULL. Caller frees. */
char *pkg_installed_version(const char *name);

/* --- .hpx archives (tar-like: name\0 size\0 payload, repeated; END) --- */
/* Pack directory `dir` (recursively, skipping hphp.lock) into hpx_path. */
void pkg_pack_dir(const char *dir, const char *hpx_path);
/* Extract hpx into dir `destdir` (created). Returns number of files. */
int pkg_extract(const char *hpx_path, const char *destdir);
/* Recursively delete a file or directory tree. Returns true when gone. */
bool pkg_remove_tree(const char *path);
/* List entry names in an .hpx (malloc'd strings, n out). */
char **pkg_list_entries(const char *hpx_path, size_t *n);

/* --- search --- */
/* Search installed packages + any local *.hpx in pkg home's cache/ dir.
 * Writes a human-readable listing to stdout. Returns match count. */
int pkg_search(const char *query);

#endif

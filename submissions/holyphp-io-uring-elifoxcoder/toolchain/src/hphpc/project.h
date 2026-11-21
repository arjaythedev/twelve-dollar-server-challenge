/* project.h — HolyPHP projects: hphp create / install / update / remove / test.
 *
 * A project is a folder with a manifest. The
 * project file is hphp.yaml (hphp.json is still read). Dependencies are
 * listed under "require" (and "require-dev"), installed into the project's
 * own .hphp/packages/ folder, and pinned in hphp.lock:
 *
 *   myapp/
 *     hphp.yaml        name, version, entry, require, require-dev
 *     hphp.lock        exact versions + checksums (commit it)
 *     src/main.hphp    the entry file
 *     tests/<x>.hphp   run by `hphp test`
 *     .hphp/packages/<pkg>/  installed dependencies (do not commit)
 *
 * `import "pkg";` finds .hphp/packages/<pkg>/ in the importing file's project before
 * the per-user package home, so every project has its own versions.
 */
#ifndef HPHPC_PROJECT_H
#define HPHPC_PROJECT_H

#include <stdbool.h>

#define PROJECT_MANIFEST "hphp.yaml"
#define PROJECT_LOCK   "hphp.lock"
#define PROJECT_VENDOR ".hphp/packages"

/* Implemented in main.c: obtain a package archive for `name` (registry,
 * falling back to the libraries bundled in the compiler). Returns the path
 * of a .hpx file and sets *source ("registry <url>" or "bundled"), or NULL. */
char *pkg_fetch_hpx(const char *name, char **source);
/* Implemented in main.c: true for a library bundled with hphp (official). */
bool pkg_is_official(const char *name);

/* Show what a community package in `dir` can do (shell commands, files,
 * network, unsafe code, ...) and ask whether to install it. Returns true to
 * go ahead: when the user says yes, or when `trust` (--trust) is set. Without
 * an answer (no input) it refuses. */
bool project_review_package(const char *dir, const char *name, const char *version,
                            const char *source, bool trust);

/* Nearest folder at or above `start_dir` that holds an hphp.json, or NULL. */
char *project_find_root(const char *start_dir);
/* Entry file of the project around the current directory, or NULL. */
char *project_default_entry(void);
/* <root>/build/<name> for `hphp build` in a project (creates build/). */
char *project_default_output(void);
/* .hphp/packages/<pkg>/... candidate for an import spec ("pkg.hphp" or
 * "pkg/file.hphp"), searched upward from the importer's folder; or NULL. */
char *project_vendor_candidate(const char *importer_dir, const char *spec);

int project_cmd_create(int argc, char **argv);
int project_cmd_install(const char *root, int argc, char **argv);
int project_cmd_update(const char *root, int argc, char **argv);
int project_cmd_remove(const char *root, int argc, char **argv);
int project_cmd_test(const char *root, int argc, char **argv, const char *self);
int project_cmd_deps(const char *root);

/* The hphp.yaml of `dir` converted to hphp.json text (for package archives
 * and the registry), or NULL when the folder has no hphp.yaml. */
char *project_manifest_as_json(const char *dir);

/* Version constraints: "*", "1.2.3", "1.2", "^1.2", "~1.2", ">=1.0 <2.0",
 * "^1.0 || ^2.0". Exposed for the CLI tests. */
bool project_version_satisfies(const char *version, const char *constraint);

#endif

/* hphp_wv2.c - the embedded browser backend.
 *
 * The engine is the Chromium Embedded Framework, reached through cefshim.dll
 * (src/hphpc/native/cefshim.cpp): a small C ABI built with MSVC that owns CEF
 * and its UI thread, so this file stays plain C compiled by gcc like the rest
 * of the runtime. The exported names are unchanged from the old WebView2
 * backend, so hphp_std.c and every app keep working untouched.
 *
 * The shim hands callbacks back on a queue rather than on its own thread -
 * script must only ever run on the VM thread - and wakes the UI message loop
 * with WM_APP+0x51CE. hpui_dispatch() drains that queue.
 *
 * Search order for the engine: HOLYCEF_SHIM, then next to the executable, in
 * cef/ next to it, in ..\cef_runtime\, then in the repository's
 * build\cef_runtime.
 */

#include "hphp_rt.h"

/* Windows-only. The POSIX stubs at the bottom keep the language checking. */
#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hphp_rt.h"

/* ================= the shim's C ABI ================= */

typedef int      (*fn_init)(void);
typedef void     (*fn_void)(void);
typedef int64_t  (*fn_ll)(int64_t);
typedef int64_t  (*fn_ll_ll)(int64_t, int64_t);
typedef int64_t  (*fn_ll_int)(int64_t, int);
typedef int64_t  (*fn_ll_str)(int64_t, const char *);
typedef int64_t  (*fn_ll_str_ll)(int64_t, const char *, int64_t);
typedef int64_t  (*fn_ll_str_out)(int64_t, char **);
typedef int64_t  (*fn_ll_double)(int64_t, double);
typedef int64_t  (*fn_ll_double_out)(int64_t, double *);
typedef int64_t  (*fn_ll_4int)(int64_t, int, int, int, int);
typedef int      (*fn_avail)(char *, int);
typedef int      (*fn_void_i)(void);

typedef struct {
    HMODULE         dll;
    fn_init         init;
    fn_void         shutdown;
    fn_void         pump;
    fn_ll_str_ll    create;
    fn_ll           destroy;
    fn_ll           ident;
    fn_ll_4int      fit;
    fn_ll_str       go;
    fn_ll_str       html;
    fn_ll           back;
    fn_ll           forward;
    fn_ll           reload;
    fn_ll           stop;
    fn_ll           can_back;
    fn_ll           can_forward;
    fn_ll_str_out   url;
    fn_ll_str_out   title;
    int64_t  (*favicon)(int64_t, char *, int);
    fn_ll_double     zoom;
    fn_ll_double_out zoom_get;
    fn_ll_int       devtools;
    fn_ll           devtools_ready;
    fn_ll_4int      devtools_fit;
    fn_ll           print;
    fn_ll_str       post;
    fn_ll_int       visible;
    fn_avail        available;
    fn_void_i       closing;
} Shim;

static Shim g_shim;
static int  g_searched = 0;
static int  g_started = 0;
static char g_shim_path[MAX_PATH * 2];

/* Why each candidate was rejected, so the message on screen names the actual
 * problem instead of just saying "not found" - a missing cefshim.dll and a
 * missing libcef.dll need very different fixes. */
static char g_tried[4096];

static void tried_add(const char *path, const char *why) {
    if (!g_tried[0]) return;
    size_t n = strlen(g_tried);
    int w = snprintf(g_tried + n, sizeof g_tried - n, "\n  %s -- %s", path, why);
    (void)w;
}

static void shim_try(const char *path) {
    if (g_shim.dll || !path || !path[0]) return;
    DWORD a = GetFileAttributesA(path);
    if (a == INVALID_FILE_ATTRIBUTES) { tried_add(path, "no such file"); return; }
    if (a & FILE_ATTRIBUTE_DIRECTORY) { tried_add(path, "is a directory"); return; }

    HMODULE h = LoadLibraryA(path);
    if (!h) {
        /* libcef.dll and chrome_elf.dll live next to the shim, and a plain
         * LoadLibrary searches the *executable's* directory first. This flag
         * starts the search at the directory of the file being loaded. */
        h = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    }
    if (!h) {
        char why[64];
        snprintf(why, sizeof why, "load failed, error %lu",
                 (unsigned long)GetLastError());
        tried_add(path, why);
        return;
    }
    /* One export is enough to prove this is the shim and not something else. */
    if (!GetProcAddress(h, "cefsh_create")) {
        tried_add(path, "loaded, but it is not the CEF shim");
        FreeLibrary(h);
        return;
    }
    g_shim.dll = h;
    lstrcpynA(g_shim_path, path, sizeof g_shim_path);
#define BIND(field, name) \
    *(void **)(&g_shim.field) = (void *)GetProcAddress(h, name)
    BIND(init,      "cefsh_init");
    BIND(shutdown,  "cefsh_shutdown");
    BIND(pump,      "cefsh_pump");
    BIND(create,    "cefsh_create");
    BIND(destroy,   "cefsh_destroy");
    BIND(ident,     "cefsh_ident");
    BIND(fit,       "cefsh_fit");
    BIND(go,        "cefsh_go");
    BIND(html,      "cefsh_html");
    BIND(back,      "cefsh_back");
    BIND(forward,   "cefsh_forward");
    BIND(reload,    "cefsh_reload");
    BIND(stop,      "cefsh_stop");
    BIND(can_back,    "cefsh_can_back");
    BIND(can_forward, "cefsh_can_forward");
    BIND(url,       "cefsh_url");
    BIND(title,     "cefsh_title");
    BIND(favicon,   "cefsh_favicon");
    BIND(zoom,      "cefsh_zoom");
    BIND(zoom_get,  "cefsh_zoom_get");
    BIND(devtools,  "cefsh_devtools");
    BIND(devtools_ready, "cefsh_devtools_ready");
    BIND(devtools_fit,   "cefsh_devtools_fit");
    BIND(print,     "cefsh_print");
    BIND(post,      "cefsh_post");
    BIND(visible,   "cefsh_visible");
    BIND(available, "cefsh_available");
    BIND(closing,   "cefsh_closing");
#undef BIND
    if (!g_shim.create || !g_shim.available) {
        g_shim.dll = NULL;
        FreeLibrary(h);
        tried_add(path, "loaded, but cefsh_create/cefsh_available missing");
    }
}

/* Relative sub-paths probed beside the executable, nearest first. cef_runtime
 * is where build_cefshim.bat stages the runtime, so an app sitting in build/
 * or in the repository root both find it. */
static const char *RELPATHS[] = {
    "cefshim.dll",
    "cef\\cefshim.dll",
    "cef_runtime\\cefshim.dll",
    "..\\cef_runtime\\cefshim.dll",
    "..\\build\\cef_runtime\\cefshim.dll",
    "..\\..\\build\\cef_runtime\\cefshim.dll",
    "..\\..\\cef_runtime\\cefshim.dll",
};

static int shim_debug(void);

static void shim_probe_base(const char *base) {
    char p[MAX_PATH * 4];
    for (size_t i = 0; i < sizeof RELPATHS / sizeof RELPATHS[0]; i++) {
        snprintf(p, sizeof p, "%s\\%s", base, RELPATHS[i]);
        shim_try(p);
        if (g_shim.dll) return;
    }
}

static void shim_search(void) {
    if (g_searched) return;
    g_searched = 1;
    snprintf(g_tried, sizeof g_tried,
             "Put cef_runtime/ beside the executable, or set HOLYCEF_SHIM.");

    const char *env = getenv("HOLYCEF_SHIM");
    if (env && env[0]) shim_try(env);
    if (g_shim.dll) return;

    char d[MAX_PATH * 2];
    DWORD k = GetModuleFileNameA(NULL, d, (DWORD)sizeof d);
    if (k && k < sizeof d) {
        char *slash = strrchr(d, '\\');
        if (slash) *slash = 0;
        if (d[0]) shim_probe_base(d);
    }
    if (g_shim.dll) return;

    /* Also probe the working directory, so launching from elsewhere still
     * finds a runtime unpacked beside the shell's cwd. */
    char cwd[MAX_PATH * 2];
    if (GetCurrentDirectoryA((DWORD)sizeof cwd, cwd)) shim_probe_base(cwd);

    if (!g_shim.dll && shim_debug()) {
        fprintf(stderr, "[cef] cefshim.dll not found. Tried:%s\n", g_tried);
        fflush(stderr);
    }
}

static int shim_debug(void) {
    const char *e = getenv("HOLYCEF_DEBUG");
    return e && e[0] && e[0] != '0';
}

static void wverr(const char *msg, long code) {
    fprintf(stderr, "[cef] %s (%ld)\n", msg, code);
    fflush(stderr);
}

/* ================= guarded callbacks into HolyPHP ================= */

typedef struct Wv2Job {
    hval    cb;
    char   *arg;
    int64_t num;
} Wv2Job;

static hval wv2_body(void *env, hval exv) {
    (void)exv;
    Wv2Job *j = (Wv2Job *)env;
    hval a[1];
    a[0] = hp_of_str(hp_str_new(j->arg, strlen(j->arg)));
    return hp_call_value(j->cb, 1, a);
}

static hval wv2_int_body(void *env, hval exv) {
    (void)exv;
    Wv2Job *j = (Wv2Job *)env;
    hval a[1];
    a[0] = hp_of_int(j->num);
    return hp_call_value(j->cb, 1, a);
}

static void report(hval r) {
    if (r.tag == HV_NULL) return;
    hstr *m = (r.tag == HV_STR) ? r.u.s : hp_val_to_str(r);
    hstr *full = hp_str_concat2(hp_str_lit("[browser] uncaught in event handler: "),
                                m ? m : hp_str_lit(""));
    fprintf(stderr, "%s\n", full ? full->data : "");
    fflush(stderr);
}

static void fire(hval cb, const char *s) {
    if (!s || cb.tag != HV_CLO || !cb.u.c) return;
    Wv2Job j;
    j.cb = cb;
    j.arg = (char *)s;
    j.num = 0;
    report(hp_try_run(wv2_body, NULL, &j));
}

static void fire_int(hval cb, int64_t n) {
    if (cb.tag != HV_CLO || !cb.u.c) return;
    Wv2Job j;
    j.cb = cb;
    j.arg = NULL;
    j.num = n;
    report(hp_try_run(wv2_int_body, NULL, &j));
}

/* ================= instances ================= */

#define WV2_MAX_KEYS 32

typedef struct Wv2Key {
    int  mod, vk, cmd;
    bool used;
    hval cb;
} Wv2Key;

typedef struct Wv2 {
    int64_t  id;
    long long cef_id;        /* CEF's own browser identifier */
    HWND     parent;
    int      top;            /* top inset, logical px */
    bool     shown;          /* false while the tab is in the background */
    hval     cb_nav, cb_load, cb_msg, cb_new;
    Wv2Key   keys[WV2_MAX_KEYS];
} Wv2;

#define WV2_MAX_INSTANCES 32
static Wv2 g_tbl[WV2_MAX_INSTANCES];
static int  g_len = 0;
static int64_t g_next = 1;

static Wv2 *wv2_find(int64_t id) {
    for (int i = 0; i < g_len; i++)
        if (g_tbl[i].id == id) return &g_tbl[i];
    return NULL;
}

static Wv2 *wv2_by_cef(long long cef_id) {
    if (!cef_id) return NULL;
    for (int i = 0; i < g_len; i++)
        if (g_tbl[i].id && g_tbl[i].cef_id == cef_id) return &g_tbl[i];
    return NULL;
}

/* ================= events from the shim ================= */

/* Called by cefshim.dll on the VM thread, after it has drained everything the
 * CEF thread queued. The signature is fixed by that DLL's import library. */
__declspec(dllexport) int64_t hpcef_dispatch(int ev, long long id, const char *text) {
    Wv2 *e = wv2_by_cef(id);
    if (!e) return 0;
    switch (ev) {
    case 1: fire(e->cb_nav, text); break;
    case 2: fire(e->cb_load, text ? text : "0"); break;
    case 3: fire(e->cb_msg, text); break;
    case 4: fire(e->cb_new, text); break;
    case 5: {
        /* "mod,vk"; the table and its alias handling live here, exactly as
         * they did when this arrived on WebView2's accelerator event. */
        if (!text) break;
        int mod = 0, vk = 0;
        if (sscanf(text, "%d,%d", &mod, &vk) != 2) break;
        int variants[4] = {vk, vk, vk, vk};
        if (vk == VK_OEM_PLUS)  { variants[1] = VK_ADD;      variants[2] = 187; variants[3] = 107; }
        if (vk == VK_OEM_MINUS) { variants[1] = VK_SUBTRACT; variants[2] = 189; variants[3] = 109; }
        for (int i = 0; i < WV2_MAX_KEYS; i++) {
            Wv2Key *k = &e->keys[i];
            if (!k->used || k->mod != mod) continue;
            if (k->vk != variants[0] && k->vk != variants[1] &&
                k->vk != variants[2] && k->vk != variants[3]) continue;
            fire_int(k->cb, k->cmd);
            break;
        }
        break;
    }
    default: break;
    }
    return 0;
}

/* Called from the UI message loop; see hpui_dispatch. */
void hpwv2_drain_events(void) {
    /* No search here: the pump runs on every message, so probing for the shim
     * at this point would load CEF into processes that only wanted to draw a
     * button. Only drain once something has actually asked for the engine. */
    if (!g_shim.dll) return;
    if (g_shim.pump) g_shim.pump();
}

/* ================= entry points ================= */

/* Bring the engine up exactly once. Called from hpwv2_available() rather than
 * hpwv2_create(): this app asks "is the engine there?" before it creates any
 * window, and that is the only place early enough to notice that we are a
 * Chromium subprocess - the shim ends those processes outright. */
static void shim_start(void) {
    if (g_started) return;
    if (g_shim.init() != 0) {
        wverr("cefsh_init failed", -1);
        return;
    }
    g_started = 1;
    if (shim_debug())
        fprintf(stderr, "[cef] engine started (pid %lu): %s\n",
                (unsigned long)GetCurrentProcessId(), g_shim_path);
}

int64_t hpwv2_available(void) {
    shim_search();
    if (!g_shim.dll) return 0;
    shim_start();
    return g_started ? 1 : 0;
}

/* ui_browser_loader() feeds the on-screen report, so when the engine is
 * missing this hands back what was tried and why, not an empty string. */
const char *hpwv2_loader_path(void) {
    shim_search();
    return g_shim.dll ? g_shim_path : g_tried;
}

int64_t hpwv2_create(int64_t parent_hwnd, const char *user_data_dir) {
    shim_search();
    if (!g_shim.dll) { wverr("cefshim.dll not found", -1); return -1; }
    HWND parent = (HWND)(intptr_t)parent_hwnd;
    if (!parent || !IsWindow(parent)) {
        wverr("parent window is not a live HWND", (long)(intptr_t)parent);
        return -1;
    }
    shim_start();
    if (!g_started) { wverr("engine not running", -1); return -1; }

    int slot = -1;
    for (int i = 0; i < g_len; i++)
        if (g_tbl[i].id == 0) { slot = i; break; }
    if (slot < 0) {
        if (g_len >= WV2_MAX_INSTANCES) { wverr("no free browser slot", WV2_MAX_INSTANCES); return -1; }
        slot = g_len++;
    }
    Wv2 *e = &g_tbl[slot];
    memset(e, 0, sizeof *e);
    e->id = g_next++;
    e->parent = parent;
    e->shown = true;              /* a fresh view is the foreground one */
    e->cb_nav = e->cb_load = e->cb_msg = e->cb_new = hp_null;

    int64_t id = g_shim.create((int64_t)(intptr_t)parent, user_data_dir, 0);
    if (id <= 0) {
        wverr("cefsh_create failed", (long)id);
        e->id = 0;
        return -1;
    }
    e->cef_id = g_shim.ident(id);
    if (shim_debug()) fprintf(stderr, "[cef] tab %lld -> cef browser %lld\n", id, e->cef_id);
    return id;
}

int64_t hpwv2_destroy(int64_t id) {
    Wv2 *e = wv2_find(id);
    if (!e) return -1;
    if (g_shim.destroy) g_shim.destroy(id);
    e->id = 0;
    e->cef_id = 0;
    return 0;
}

/* Re-fits the view to its parent. `top_logical` is the height of the toolbar
 * the script drew above it, in the same logical pixels lib/ui.hphp uses.
 *
 * The CEF browser is a real child window of `parent`, so plain client
 * coordinates are already correct - no DPI round trip is needed. */
int64_t hpwv2_fit(int64_t id, int top_logical, int width_logical) {
    Wv2 *e = wv2_find(id);
    if (!e) return -1;
    e->top = top_logical;
    if (!g_shim.fit) return -1;

    if (!e->shown || IsIconic(e->parent)) {
        g_shim.visible(id, 0);
        return 0;
    }
    RECT rc;
    if (!GetClientRect(e->parent, &rc)) return -1;
    int cw = rc.right - rc.left;
    int ch = rc.bottom - rc.top;
    /* `top` arrives in the same 96-dpi logical pixels lib/ui.hphp lays out
     * in, but this process is per-monitor DPI aware, so GetClientRect and
     * SetWindowPos both work in device pixels. Handing Chromium the unscaled
     * number put the page window 1.5x too high on a 150% display - it covered
     * the bottom of the toolbar and swallowed every click meant for the
     * address bar and the tab strip. */
    int dpi = (int)hpui_dpi_scale();
    if (dpi <= 0) dpi = 96;
    int top = MulDiv(top_logical, dpi, 96);
    if (top < 0) top = 0;
    if (top > ch) top = ch;
    int w = cw;
    if (width_logical > 0) {
        w = MulDiv(width_logical, dpi, 96);
        if (w > cw) w = cw;
        if (w < 0) w = 0;
    }
    /* The layout tick runs on every message-pump turn, so an unconditional
     * trace here filled the log with tens of thousands of identical lines and
     * pushed the interesting events (a navigation, a bridge call) off the end.
     * Only a change in the geometry is worth a line. */
    static int last_top = -1, last_w = -1, last_h = -1, last_ch = -1;
    int h = ch - top;
    if (shim_debug() && (top != last_top || w != last_w || h != last_h)) {
        fprintf(stderr, "[cef] fit id=%lld -> %d,%d %dx%d (dpi=%d)\n",
                (long long)id, 0, top, w, h, dpi);
        last_top = top;
        last_w = w;
        last_h = h;
    }
    (void)last_ch;
    g_shim.visible(id, 1);
    return g_shim.fit(id, 0, top, w, h);
}

int64_t hpwv2_navigate(int64_t id, const char *url) {
    Wv2 *e = wv2_find(id);
    if (!e || !url) return -1;
    return g_shim.go(id, url);
}

int64_t hpwv2_html(int64_t id, const char *html) {
    Wv2 *e = wv2_find(id);
    if (!e || !html) return -1;
    return g_shim.html(id, html);
}

int64_t hpwv2_back(int64_t id) {
    Wv2 *e = wv2_find(id);
    return e ? g_shim.back(id) : -1;
}

int64_t hpwv2_forward(int64_t id) {
    Wv2 *e = wv2_find(id);
    return e ? g_shim.forward(id) : -1;
}

int64_t hpwv2_reload(int64_t id) {
    Wv2 *e = wv2_find(id);
    return e ? g_shim.reload(id) : -1;
}

int64_t hpwv2_stop(int64_t id) {
    Wv2 *e = wv2_find(id);
    return e ? g_shim.stop(id) : -1;
}

int64_t hpwv2_can_back(int64_t id) {
    Wv2 *e = wv2_find(id);
    return (e && g_shim.can_back(id)) ? 1 : 0;
}

int64_t hpwv2_can_forward(int64_t id) {
    Wv2 *e = wv2_find(id);
    return (e && g_shim.can_forward(id)) ? 1 : 0;
}

int64_t hpwv2_url(int64_t id, char **out) {
    Wv2 *e = wv2_find(id);
    if (out) *out = NULL;
    if (!e) return -1;
    return g_shim.url(id, out);
}

int64_t hpwv2_title(int64_t id, char **out) {
    Wv2 *e = wv2_find(id);
    if (out) *out = NULL;
    if (!e) return -1;
    return g_shim.title(id, out);
}

/* The tab's site icon URL, or "" while the page has none. */
int64_t hpwv2_favicon(int64_t id, char **out) {
    Wv2 *e = wv2_find(id);
    if (out) *out = NULL;
    if (!e || !g_shim.favicon) return -1;
    char buf[4096];
    if (g_shim.favicon(id, buf, (int)sizeof buf) <= 0) return -1;
    if (out) { char *d = (char *)malloc(strlen(buf) + 1); if (d) strcpy(d, buf); *out = d; }
    return 0;
}

int64_t hpwv2_zoom(int64_t id, double factor) {
    Wv2 *e = wv2_find(id);
    if (!e) return -1;
    return g_shim.zoom(id, factor);
}

int64_t hpwv2_zoom_get(int64_t id, double *out) {
    Wv2 *e = wv2_find(id);
    if (!e) return -1;
    return g_shim.zoom_get(id, out);
}

/* `on` opens or closes the docked devtools panel for this view. */
int64_t hpwv2_devtools(int64_t id, bool on) {
    Wv2 *e = wv2_find(id);
    if (!e || !g_shim.devtools) return -1;
    return g_shim.devtools(id, on ? 1 : 0);
}

/* Where the docked panel goes, in physical client pixels. Called every layout
 * tick: CEF attaches the window a moment after ShowDevTools returns. */
int64_t hpwv2_devtools_fit(int64_t id, int x, int y, int w, int hgt) {
    Wv2 *e = wv2_find(id);
    if (!e || !g_shim.devtools_fit) return -1;
    if (g_shim.devtools_ready && !g_shim.devtools_ready(id)) return 0;
    return g_shim.devtools_fit(id, x, y, w, hgt);
}

int64_t hpwv2_devtools_ready(int64_t id) {
    Wv2 *e = wv2_find(id);
    if (!e || !g_shim.devtools_ready) return 0;
    return g_shim.devtools_ready(id);
}

int64_t hpwv2_print(int64_t id) {
    Wv2 *e = wv2_find(id);
    if (!e || !g_shim.print) return -1;
    return g_shim.print(id);
}

int64_t hpwv2_post(int64_t id, const char *json) {
    Wv2 *e = wv2_find(id);
    if (!e || !json) return -1;
    return g_shim.post(id, json);
}

/* Show or hide this view. Several browsers share one window, one per tab;
 * only the foreground tab is rendered and takes input. */
int64_t hpwv2_visible(int64_t id, bool on) {
    Wv2 *e = wv2_find(id);
    if (!e) return -1;
    e->shown = on;
    return g_shim.visible(id, on ? 1 : 0);
}

/* How many tabs are still being torn down. A browser that is a child of a
 * window this runtime owns asks to close by sending WM_CLOSE to that OWNING
 * window, so without this the window procedure cannot tell "this tab is going"
 * from "the user closed the app". */
int hpwv2_closing(void) {
    return (g_shim.dll && g_shim.closing) ? g_shim.closing() : 0;
}

/* mod: 1 = Ctrl, 2 = Shift, 4 = Alt (exact combination, no extras allowed).
 * The callback receives `cmd`, so one closure can serve many bindings. */
int64_t hpwv2_shortcut(int64_t id, int64_t mod, int64_t vk, int64_t cmd, hval cb) {
    Wv2 *e = wv2_find(id);
    if (!e) return -1;
    for (int i = 0; i < WV2_MAX_KEYS; i++) {
        Wv2Key *k = &e->keys[i];
        if (!k->used) continue;
        bool match_vk = k->vk == (int)vk;
        if (vk == VK_OEM_PLUS)  match_vk = match_vk || k->vk == VK_ADD || k->vk == 187 || k->vk == 107;
        if (vk == VK_OEM_MINUS) match_vk = match_vk || k->vk == VK_SUBTRACT || k->vk == 189 || k->vk == 109;
        if (k->mod == (int)mod && match_vk) {
            k->cmd = (int)cmd;
            k->cb = cb;
            return 0;
        }
    }
    for (int i = 0; i < WV2_MAX_KEYS; i++) {
        Wv2Key *k = &e->keys[i];
        if (k->used) continue;
        k->used = true;
        k->mod = (int)mod;
        k->vk = (int)vk;
        k->cmd = (int)cmd;
        k->cb = cb;
        return 0;
    }
    return -1;
}

/* evtype: 1 = navigating to <url>, 2 = page finished ("1"/"0"),
 *          3 = page posted JSON, 4 = window.open(<url>)                */
int64_t hpwv2_on(int64_t id, int64_t evtype, hval cb) {
    Wv2 *e = wv2_find(id);
    if (!e) return -1;
    switch (evtype) {
    case 1: e->cb_nav = cb; break;
    case 2: e->cb_load = cb; break;
    case 3: e->cb_msg = cb; break;
    case 4: e->cb_new = cb; break;
    default: return -1;
    }
    return 0;
}

/* Called when the app is on its way out: let CEF shut down cleanly so the
 * profile is flushed instead of being left for the next crash. */
void hpwv2_shutdown(void) {
    shim_search();
    if (g_started && g_shim.shutdown) {
        g_shim.shutdown();
        g_started = 0;
    }
}

#else  /* !_WIN32 - inert stubs so the language still checks on POSIX */
#include <stdint.h>
#include <stdbool.h>

int64_t hpwv2_available(void) { return 0; }
const char *hpwv2_loader_path(void) { return ""; }
int64_t hpwv2_create(int64_t p, const char *d) { (void)p; (void)d; return -1; }
int64_t hpwv2_destroy(int64_t i) { (void)i; return -1; }
int64_t hpwv2_fit(int64_t i, int t, int w) { (void)i; (void)t; (void)w; return -1; }
int64_t hpwv2_navigate(int64_t i, const char *u) { (void)i; (void)u; return -1; }
int64_t hpwv2_html(int64_t i, const char *h) { (void)i; (void)h; return -1; }
int64_t hpwv2_back(int64_t i) { (void)i; return -1; }
int64_t hpwv2_forward(int64_t i) { (void)i; return -1; }
int64_t hpwv2_reload(int64_t i) { (void)i; return -1; }
int64_t hpwv2_stop(int64_t i) { (void)i; return -1; }
int64_t hpwv2_can_back(int64_t i) { (void)i; return 0; }
int64_t hpwv2_can_forward(int64_t i) { (void)i; return 0; }
int64_t hpwv2_url(int64_t i, char **o) { (void)i; if (o) *o = NULL; return -1; }
int64_t hpwv2_title(int64_t i, char **o) { (void)i; if (o) *o = NULL; return -1; }
int64_t hpwv2_favicon(int64_t i, char **o) { (void)i; if (o) *o = NULL; return -1; }
int64_t hpwv2_zoom(int64_t i, double z) { (void)i; (void)z; return -1; }
int64_t hpwv2_zoom_get(int64_t i, double *o) { (void)i; if (o) *o = 1.0; return -1; }
int64_t hpwv2_devtools(int64_t i, bool on) { (void)i; (void)on; return -1; }
int64_t hpwv2_devtools_fit(int64_t i, int a, int b, int c, int d) {
    (void)i; (void)a; (void)b; (void)c; (void)d; return -1;
}
int64_t hpwv2_devtools_ready(int64_t i) { (void)i; return 0; }
int64_t hpwv2_print(int64_t i) { (void)i; return -1; }
int64_t hpwv2_post(int64_t i, const char *j) { (void)i; (void)j; return -1; }
int64_t hpwv2_on(int64_t i, int64_t e, hval c) { (void)i; (void)e; (void)c; return -1; }
int64_t hpwv2_visible(int64_t i, bool on) { (void)i; (void)on; return -1; }
int64_t hpwv2_shortcut(int64_t i, int64_t m, int64_t v, int64_t c, hval cb) {
    (void)i; (void)m; (void)v; (void)c; (void)cb; return -1;
}
void hpwv2_drain_events(void) {}
void hpwv2_shutdown(void) {}
int hpwv2_closing(void) { return 0; }

#endif /* _WIN32 */

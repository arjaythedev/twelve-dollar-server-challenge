/* hphp_ui.c — HolyPHP UI 2.0: native Win32 toolkit (themed, owner-drawn).
 *
 * Design goals (see docs/lib-ui.html): the widgets must look like a modern
 * app, not like 1995, and the HolyPHP wrapper on top must stay tiny.
 *
 * How the "modern" part is achieved without a third-party toolkit:
 *   - one global Theme (bg / surface / text / muted / accent / border,
 *     radius, type scale) that every control reads, plus per-control
 *     overrides and per-control "roles" (primary, subtle, ghost, danger,
 *     success, link);
 *   - everything that Win32 draws as a grey box is owner-drawn here:
 *     buttons, checkboxes, cards, sections, list boxes, sliders, progress
 *     bars and combo boxes (including its popup);
 *   - rounded rectangles everywhere, Segoe UI, and real DPI awareness.
 *
 * Two operating modes, unchanged:
 *   - ui_dispatch(ms): pump events for up to ms milliseconds (run mode)
 *   - ui_run_main(): blocking GetMessage loop (build mode)
 *
 * Handles are int64 ids into a runtime-side table; user callbacks are hval
 * closures invoked through hp_call_value. Every callback runs under
 * hp_try_run so a HolyPHP exception inside a handler prints and keeps the
 * app alive instead of unwinding through C.
 */
#include "hphp_rt.h"

#ifdef _WIN32
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#ifdef _WIN32_WINNT
#undef _WIN32_WINNT
#endif
#define _WIN32_WINNT 0x0601
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shlobj.h>
#include <commdlg.h>
#include <gdiplus/gdiplus.h>
#endif

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32

/* Set once from HPHP_UI_DEBUG; declared up here because the paint
 * trace below is the earliest caller. */
static int hpui_win_debug(void);

/* ================= optional Windows APIs (loaded lazily) ================= */
/* uxtheme/dwmapi are resolved with LoadLibrary so the binary still starts on
 * systems where they are missing, and so no import-table dependency is made. */
typedef UINT  (WINAPI *fn_GetDpiForWindow)(HWND);
typedef HANDLE (WINAPI *fn_SetDpiCtx)(HANDLE);
typedef HRESULT (WINAPI *fn_SetWindowTheme)(HWND, LPCWSTR, LPCWSTR);
typedef HRESULT (WINAPI *fn_DwmSetWindowAttribute)(HWND, DWORD, LPCVOID, DWORD);

static fn_GetDpiForWindow       p_GetDpiForWindow;
static fn_SetDpiCtx             p_SetDpiCtx;
static fn_SetWindowTheme        p_SetWindowTheme;
static fn_DwmSetWindowAttribute p_DwmSetWindowAttribute;
static int   g_dpi = 96;
static bool  g_dpi_aware = true;

static void ensure_common(void);   /* needed by hpui_textw, defined further down */

static void load_optional_apis(void) {
    static bool done = false;
    if (done) return;
    HMODULE ux = LoadLibraryW(L"uxtheme.dll");
    HMODULE dw = LoadLibraryW(L"dwmapi.dll");
    HMODULE us = GetModuleHandleW(L"user32.dll");
    if (ux) p_SetWindowTheme     = (fn_SetWindowTheme)(void *)GetProcAddress(ux, "SetWindowTheme");
    if (dw) p_DwmSetWindowAttribute =
        (fn_DwmSetWindowAttribute)(void *)GetProcAddress(dw, "DwmSetWindowAttribute");
    if (us) {
        p_GetDpiForWindow = (fn_GetDpiForWindow)(void *)GetProcAddress(us, "GetDpiForWindow");
        p_SetDpiCtx = (fn_SetDpiCtx)(void *)GetProcAddress(us, "SetProcessDpiAwarenessContext");
    }
    /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 is the pseudo-handle -4.
     * If the call fails the OS is bitmap-virtualising us, so we must NOT
     * scale anything by hand or every coordinate gets scaled twice. */
    if (p_SetDpiCtx) {
        g_dpi_aware = p_SetDpiCtx((HANDLE)(LONG_PTR)-4) != NULL;
        if (!g_dpi_aware) g_dpi = 96;
    } else {
        g_dpi_aware = false;
    }
    if (g_dpi_aware) {
        /* Learn the real scale BEFORE the first window exists, so the window
         * and the widgets inside it agree on what a pixel is worth. */
        HDC dc = GetDC(NULL);
        int lpx = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 0;
        if (dc) ReleaseDC(NULL, dc);
        if (lpx >= 72 && lpx <= 480) g_dpi = lpx;
    }
    done = true;
}

static int sc(int v) { return MulDiv(v, g_dpi, 96); }

/* The DPI scale the whole UI is laid out in, so other parts of the runtime can
 * convert a logical rectangle (what a script writes) into the physical pixels
 * Win32 wants. Everything here is per-monitor DPI aware, so a plain logical
 * number handed to SetWindowPos lands 1.5x too far away on a 150% display. */
int64_t hpui_dpi_scale(void) { return g_dpi > 0 ? g_dpi : 96; }

/* ================= theme ================= */
typedef struct UiTheme {
    COLORREF bg, surface, elevated, text, muted, accent, on_accent, border;
    int radius;        /* corner radius, px            */
    int font_pt;       /* body type size, points       */
    int title_pt;      /* heading type size, points    */
    int row_h;         /* list / popup row height, px  */
    int pad;           /* default horizontal padding   */
    bool dark;
} UiTheme;

/* roles: 0 default, 1 primary, 2 subtle, 3 ghost, 4 danger, 5 success, 6 link */
#define UI_ROLE_DEFAULT 0
#define UI_ROLE_PRIMARY 1
#define UI_ROLE_SUBTLE  2
#define UI_ROLE_GHOST   3
#define UI_ROLE_DANGER  4
#define UI_ROLE_SUCCESS 5
#define UI_ROLE_LINK    6

static UiTheme g_theme = {
    RGB(0x0F, 0x11, 0x15),   /* bg         */
    RGB(0x17, 0x1A, 0x21),   /* surface    */
    RGB(0x1E, 0x22, 0x2B),   /* elevated   */
    RGB(0xE8, 0xEA, 0xF0),   /* text       */
    RGB(0x96, 0x9F, 0xB0),   /* muted      */
    RGB(0x4C, 0x7D, 0xFF),   /* accent     */
    RGB(0xFF, 0xFF, 0xFF),   /* on_accent  */
    RGB(0x2B, 0x31, 0x3D),   /* border     */
    8, 10, 16, 26, 12, true
};

/* colour maths -------------------------------------------------------- */
static COLORREF mix(COLORREF a, COLORREF b, int pct) {
    int ar = GetRValue(a), ag = GetGValue(a), ab = GetBValue(a);
    int br = GetRValue(b), bg = GetGValue(b), bb = GetBValue(b);
    int t = pct;
    return RGB((ar * (100 - t) + br * t) / 100,
               (ag * (100 - t) + bg * t) / 100,
               (ab * (100 - t) + bb * t) / 100);
}
static COLORREF lighten(COLORREF c, int pct) { return mix(c, RGB(255, 255, 255), pct); }
/* blend() with a 0..1 weight, for the hover fade */
static COLORREF blend(COLORREF a, COLORREF b, double t) {
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    int pct = (int)(t * 100.0 + 0.5);
    return mix(a, b, pct);
}
static COLORREF darken(COLORREF c, int pct)  { return mix(c, RGB(0, 0, 0), pct); }
/* a soft tint of a colour: what a modern UI uses for hover/selected fills */
static COLORREF tint(COLORREF c) {
    return g_theme.dark ? mix(c, g_theme.surface, 35) : mix(c, RGB(255, 255, 255), 88);
}

/* ================= font cache ================= */
typedef struct { int pt; bool bold; HFONT f; const wchar_t *fam; } FontEnt;
static FontEnt g_fonts[64];
static int      g_nfonts = 0;

/* the canvas keeps its own font table (family + style are part of the key
 * there, which the widget cache above cannot express); it has to go the same
 * way on a DPI change, or every canvas frame is drawn with a stale face */
static void cvfonts_drop(void);

static void fonts_drop(void) {
    for (int i = 0; i < g_nfonts; i++)
        if (g_fonts[i].f) DeleteObject(g_fonts[i].f);
    g_nfonts = 0;
    cvfonts_drop();
}

static HFONT ui_font_family(int pt, bool bold, const wchar_t *fam) {
    if (pt <= 0) pt = g_theme.font_pt;
    for (int i = 0; i < g_nfonts; i++)
        if (g_fonts[i].pt == pt && g_fonts[i].bold == bold &&
            g_fonts[i].fam == fam)
            return g_fonts[i].f;
    if (g_nfonts >= (int)(sizeof g_fonts / sizeof g_fonts[0])) fonts_drop();
    HFONT f = CreateFontW(-MulDiv(pt, g_dpi, 72), 0, 0, 0,
                          bold ? FW_SEMIBOLD : FW_NORMAL,
                          FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                          OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                          fam);
    g_fonts[g_nfonts].pt = pt;
    g_fonts[g_nfonts].bold = bold;
    g_fonts[g_nfonts].fam = fam;
    g_fonts[g_nfonts].f = f;
    g_nfonts++;
    return f;
}

static HFONT ui_font(int pt, bool bold) {
    return ui_font_family(pt, bold, L"Segoe UI");
}

/* Segoe UI covers Latin, Greek, Cyrillic and the punctuation blocks, and
 * stops there. The characters apps reach for when they want an icon -- the
 * gear, the cross, the house, the refresh arrow -- live in Segoe UI Symbol,
 * and asking GDI for a glyph the face does not have draws a placeholder box
 * or a stray stroke instead of the icon. So ask the face whether it covers
 * the string and hand back the symbol face when it does not. Measuring has
 * to make the same choice, or the width reserved is the width of the wrong
 * font and every button that holds an icon comes out lopsided. */
static bool font_covers(HDC dc, const wchar_t *s) {
    for (const wchar_t *p = s; p && *p; p++) {
        if (*p < 0x80) continue;              /* plain ASCII is always there */
        WORD idx = 0xFFFF;
        if (GetGlyphIndicesW(dc, p, 1, &idx, GGI_MARK_NONEXISTING_GLYPHS)
                == GDI_ERROR)
            return true;                      /* cannot tell, so do not guess */
        if (idx == 0xFFFF) return false;
    }
    return true;
}

static HFONT font_for_wstr(const wchar_t *s, HFONT base) {
    if (!s || !*s || !base) return base;
    HDC dc = CreateCompatibleDC(NULL);
    if (!dc) return base;
    HFONT old = (HFONT)SelectObject(dc, base);
    bool ok = font_covers(dc, s);
    SelectObject(dc, old);
    DeleteDC(dc);
    if (ok) return base;
    /* same size and weight as the face that could not draw it; the cache is
       keyed on family, so look the original entry up rather than guess */
    int pt = g_theme.font_pt;
    bool bold = false;
    for (int i = 0; i < g_nfonts; i++)
        if (g_fonts[i].f == base) { pt = g_fonts[i].pt; bold = g_fonts[i].bold; break; }
    return ui_font_family(pt, bold, L"Segoe UI Symbol");
}

/* UTF-8 entry point, for the measuring paths */
static HFONT font_for_text(const char *text, int pt, bool bold) {
    HFONT base = ui_font(pt, bold);
    if (!text || !*text) return base;
    wchar_t w[8];
    int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, w, 8);
    if (n <= 1) return base;
    return font_for_wstr(w, base);
}

static void fonts_apply(HWND hw) {
    SendMessageW(hw, WM_SETFONT, (WPARAM)ui_font(g_theme.font_pt, false), TRUE);
}

/* ================= handle table ================= */
typedef struct UiEntry {
    int64_t id;
    int     kind;
    HWND    hwnd;
    HMENU   menu_id;             /* menu item command id / popup HMENU */
    hval    cb_click, cb_change, cb_action, cb_key, cb_drag;
    int     drag_x, drag_y;               /* where the last drag started */
    bool    drag_active;
    bool    checked_flag;
    /* --- v2 presentation --- */
    int64_t ov_bg, ov_fg, ov_accent, ov_border;   /* -1 = inherit theme */
    int     ov_radius, ov_font_pt;
    bool    ov_font_bold;
    int     role;
    bool    hovered, pressed, focused;
    int     hover_index;
    float   anim;                           /* hover fade, 0..1 */
    int     value, min_v, max_v;                 /* slider / progress */
    wchar_t **items;                             /* combo items       */
    int     n_items, cap_items;
    wchar_t *svg;                                /* fixed-size vector icon markup */
    char   *pic;                                 /* local image file drawn in the icon slot */
    int     sel;                                 /* combo selection   */
    WNDPROC old_proc;                            /* subclassed wndproc*/
    /* --- scrolling (window entry owns the offset) --- */
    int     scroll_y, content_h;
    int64_t parent_id;                       /* owning window, for Tab order */
    /* --- logical, unscrolled geometry: scrolling replays these --- */
    int     base_x, base_y, base_w, base_h;
    /* --- canvas (HPUI_CANVAS): an offscreen surface the script paints --- */
    HDC     cv_dc;                           /* backbuffer DC: the target */
    HBITMAP cv_bmp, cv_old_bmp;              /* backbuffer bitmap      */
    int     cv_w, cv_h;                      /* backbuffer size, device px */
    int     cv_cursor;                       /* requested pointer shape    */
    bool    cv_track;                        /* TME_LEAVE armed            */
    bool    cv_captured;                     /* this canvas holds the mouse*/
    bool    cv_drag;                         /* a press is in progress      */
    hval    cb_ev[HPUI_EV_NEV];              /* canvas input events        */
} UiEntry;

static UiEntry *tbl = NULL;
static size_t tbl_len = 0, tbl_cap = 0;
static int64_t next_id = 1;

/* defined with the rest of the canvas code further down */
static bool cv_ensure(UiEntry *e);

static UiEntry *tbl_find(int64_t id) {
    for (size_t i = 0; i < tbl_len; i++)
        if (tbl[i].id == id) return &tbl[i];
    return NULL;
}
static UiEntry *entry_of(HWND hw) {
    if (!hw) return NULL;
    int64_t id = (int64_t)GetWindowLongPtrW(hw, GWLP_USERDATA);
    return id ? tbl_find(id) : NULL;
}

static void items_free(UiEntry *e) {
    for (int i = 0; i < e->n_items; i++) free(e->items[i]);
    free(e->items);
    e->items = NULL;
    e->n_items = e->cap_items = 0;
}

static void tbl_add(int64_t id, int kind, HWND hwnd) {
    if (tbl_len == tbl_cap) {
        tbl_cap = tbl_cap ? tbl_cap * 2 : 32;
        tbl = (UiEntry *)realloc(tbl, tbl_cap * sizeof(UiEntry));
    }
    UiEntry *e = &tbl[tbl_len++];
    memset(e, 0, sizeof *e);
    e->id = id;
    e->kind = kind;
    e->hwnd = hwnd;
    e->cb_click = e->cb_change = e->cb_action = e->cb_key = hp_null;
    e->svg = NULL;
    e->ov_bg = e->ov_fg = e->ov_accent = e->ov_border = -1;
    e->ov_radius = -1;
    e->ov_font_pt = -1;
    e->role = UI_ROLE_DEFAULT;
    e->hover_index = -1;
    e->min_v = 0;
    e->max_v = 100;
    e->sel = -1;
}

/* ================= painting helpers ================= */
static void fill_rect(HDC dc, RECT r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

/* Same, but only inside an arbitrary region. Used to erase a window's
 * background without touching the boxes its owner-drawn children own. */
static void fill_region(HDC dc, HRGN rgn, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRgn(dc, rgn, b);
    DeleteObject(b);
}

/* rounded fill + optional 1px outline; radius <= 0 falls back to a plain rect */
static void fill_round(HDC dc, RECT r, int radius, COLORREF fill,
                       COLORREF border, int bwidth) {
    HBRUSH br = fill == (COLORREF)-1 ? NULL : CreateSolidBrush(fill);
    HPEN   pen = (bwidth > 0 && border != (COLORREF)-1)
                     ? CreatePen(PS_SOLID, bwidth, border) : (HPEN)GetStockObject(NULL_PEN);
    HBRUSH ob = (HBRUSH)SelectObject(dc, br ? br : GetStockObject(NULL_BRUSH));
    HPEN   op = (HPEN)SelectObject(dc, pen);
    /* GDI centres a pen on the outline, so a border drawn on the exact edge
     * loses its outer half to the clip: a focused field ended up with a bright
     * half-line along the top and nothing round the sides. Inset by half the
     * pen width and the ring lands entirely inside the control. */
    int in = bwidth > 0 ? bwidth / 2 : 0;
    int l = r.left + in, t = r.top + in;
    int rt = r.right - in, bt = r.bottom - in;
    int rr = radius * 2;
    if (rt <= l) rt = l + 1;
    if (bt <= t) bt = t + 1;
    if (rr > 0) RoundRect(dc, l, t, rt, bt, rr, rr);
    else        Rectangle(dc, l, t, rt, bt);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    if (br) DeleteObject(br);
    if (bwidth > 0 && border != (COLORREF)-1) DeleteObject(pen);
}

static void draw_str(HDC dc, RECT r, const wchar_t *s, COLORREF fg, HFONT f,
                     UINT flags) {
    if (!s || !*s) return;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, fg);
    HFONT base = f ? f : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HFONT of = (HFONT)SelectObject(dc, font_for_wstr(s, base));
    DrawTextW(dc, s, -1, &r, flags | DT_NOPREFIX);
    SelectObject(dc, of);
}

/* ================= SVG vector icons ================= */
/* Browser controls pass ordinary, self-contained SVG. The native painter
 * intentionally implements the tiny subset used by the chrome (viewBox and
 * path d=, with M/L/H/V/C/Z commands); it never loads fonts or external data. */
static bool svg_num(const wchar_t **p, REAL *out) {
    while (**p == L' ' || **p == L',' || **p == L'\t' || **p == L'\n' || **p == L'\r') ++*p;
    if (**p == L'+' || **p == L'-') {
        wchar_t sign = **p;
        if ((*p)[1] == sign) return false;
    }
    wchar_t *end = NULL;
    double d = wcstod(*p, &end);
    if (end == *p) return false;
    *out = (REAL)d;
    *p = end;
    return true;
}

static const wchar_t *svg_attr(const wchar_t *tag, const wchar_t *end,
                               const wchar_t *name, size_t *len) {
    size_t n = wcslen(name);
    const wchar_t *p = tag;
    while (p < end) {
        while (p < end && !((*p >= L'A' && *p <= L'Z') || (*p >= L'a' && *p <= L'z'))) ++p;
        const wchar_t *key = p;
        while (p < end && ((*p >= L'A' && *p <= L'Z') || (*p >= L'a' && *p <= L'z') || *p == L'-')) ++p;
        if ((size_t)(p - key) != n || wcsncmp(key, name, n) != 0) continue;
        while (p < end && (*p == L' ' || *p == L'\t')) ++p;
        if (p >= end || *p++ != L'=') continue;
        while (p < end && (*p == L' ' || *p == L'\t')) ++p;
        if (p >= end || (*p != L'\"' && *p != L'\'')) continue;
        wchar_t quote = *p++;
        const wchar_t *value = p;
        while (p < end && *p != quote) ++p;
        if (p >= end) return NULL;
        *len = (size_t)(p - value);
        return value;
    }
    return NULL;
}

static void svg_path_draw(GpGraphics *g, const wchar_t *d, COLORREF color,
                          REAL vx, REAL vy, REAL ox, REAL oy, REAL scale) {
    GpPath *path = NULL;
    if (GdipCreatePath(FillModeWinding, &path) != Ok || !path) return;
    const wchar_t *p = d;
    REAL cx = 0, cy = 0, sx = 0, sy = 0;
    wchar_t op = 0;
    bool have = false;
    while (*p) {
        while (*p == L' ' || *p == L',' || *p == L'\t' || *p == L'\n' || *p == L'\r') ++p;
        if ((*p >= L'A' && *p <= L'Z') || (*p >= L'a' && *p <= L'z')) op = *p++;
        if (!op) break;
        REAL a, b;
        if (op == L'M' || op == L'm') {
            if (!svg_num(&p, &a) || !svg_num(&p, &b)) break;
            if (op == L'm') { a += cx; b += cy; }
            GdipStartPathFigure(path);
            cx = sx = a; cy = sy = b; have = true;
            op = (op == L'm') ? L'l' : L'L';
        } else if (op == L'L' || op == L'l') {
            if (!svg_num(&p, &a) || !svg_num(&p, &b)) break;
            if (op == L'l') { a += cx; b += cy; }
            if (have) GdipAddPathLine(path, ox + (cx - vx) * scale, oy + (cy - vy) * scale,
                                           ox + (a - vx) * scale, oy + (b - vy) * scale);
            cx = a; cy = b;
        } else if (op == L'H' || op == L'h') {
            if (!svg_num(&p, &a)) break;
            if (op == L'h') a += cx;
            if (have) GdipAddPathLine(path, ox + (cx - vx) * scale, oy + (cy - vy) * scale,
                                           ox + (a - vx) * scale, oy + (cy - vy) * scale);
            cx = a;
        } else if (op == L'V' || op == L'v') {
            if (!svg_num(&p, &b)) break;
            if (op == L'v') b += cy;
            if (have) GdipAddPathLine(path, ox + (cx - vx) * scale, oy + (cy - vy) * scale,
                                           ox + (cx - vx) * scale, oy + (b - vy) * scale);
            cy = b;
        } else if (op == L'C' || op == L'c') {
            REAL c, e, f, h;
            if (!svg_num(&p, &a) || !svg_num(&p, &b) || !svg_num(&p, &c) ||
                !svg_num(&p, &e) || !svg_num(&p, &f) || !svg_num(&p, &h)) break;
            if (op == L'c') { a += cx; b += cy; c += cx; e += cy; f += cx; h += cy; }
            if (have) GdipAddPathBezier(path, ox + (cx - vx) * scale, oy + (cy - vy) * scale,
                ox + (a - vx) * scale, oy + (b - vy) * scale,
                ox + (c - vx) * scale, oy + (e - vy) * scale,
                ox + (f - vx) * scale, oy + (h - vy) * scale);
            cx = f; cy = h;
        } else if (op == L'Z' || op == L'z') {
            if (have) GdipClosePathFigure(path);
            cx = sx; cy = sy; op = 0;
        } else {
            break; /* unsupported SVG command: fail closed */
        }
    }
    ARGB argb = 0xFF000000u | ((GetRValue(color) & 255u) << 16) |
                ((GetGValue(color) & 255u) << 8) | (GetBValue(color) & 255u);
    GpPen *pen = NULL;
    if (GdipCreatePen1(argb, 1.8f, UnitPixel, &pen) == Ok && pen) {
        GdipSetPenLineCap197819(pen, LineCapRound, LineCapRound, DashCapRound);
        GdipSetPenLineJoin(pen, LineJoinRound);
        GdipDrawPath(g, pen, path);
        GdipDeletePen(pen);
    }
    GdipDeletePath(path);
}

static ULONG_PTR g_gdip_token;
static void draw_svg_icon(HDC dc, RECT rc, const wchar_t *svg, COLORREF color) {
    if (!svg || !*svg || !g_gdip_token) return;
    /* HPHP_UI_DEBUG=2 paints every icon in a colour nothing else uses, so
       "is this glyph being drawn at all" stops being a judgement call about
       whether the colour is dark enough to see against the background. It is
       what settled a close button that was drawing in the right place and
       still could not be seen. */
    {
        const char *dbg = getenv("HPHP_UI_DEBUG");
        if (dbg && dbg[0] == '2') color = RGB(255, 0, 0);
    }
    const wchar_t *root = wcsstr(svg, L"<svg");
    if (!root) return;
    const wchar_t *root_end = wcschr(root, L'>');
    if (!root_end) return;
    size_t vb_len = 0;
    const wchar_t *vb = svg_attr(root, root_end, L"viewBox", &vb_len);
    if (!vb || vb_len >= 128) return;
    wchar_t box[128];
    memcpy(box, vb, vb_len * sizeof(wchar_t)); box[vb_len] = 0;
    const wchar_t *bp = box;
    REAL vx, vy, vw, vh;
    if (!svg_num(&bp, &vx) || !svg_num(&bp, &vy) || !svg_num(&bp, &vw) ||
        !svg_num(&bp, &vh) || vw <= 0 || vh <= 0) return;
    int side = rc.right - rc.left;
    if (rc.bottom - rc.top < side) side = rc.bottom - rc.top;
    if (side <= 0) return;
    REAL scale = (REAL)side / (vw > vh ? vw : vh);
    REAL ox = (REAL)rc.left + ((REAL)(rc.right - rc.left) - vw * scale) * 0.5f;
    REAL oy = (REAL)rc.top + ((REAL)(rc.bottom - rc.top) - vh * scale) * 0.5f;
    GpGraphics *g = NULL;
    if (GdipCreateFromHDC(dc, &g) != Ok || !g) return;
    GdipSetSmoothingMode(g, SmoothingModeAntiAlias);
    const wchar_t *tag = root_end + 1;
    while ((tag = wcsstr(tag, L"<path")) != NULL) {
        const wchar_t *tag_end = wcschr(tag, L'>');
        if (!tag_end) break;
        size_t dlen = 0;
        const wchar_t *d = svg_attr(tag, tag_end, L"d", &dlen);
        if (d && dlen && dlen < 4096) {
                wchar_t *commands = (wchar_t *)malloc((dlen + 1) * sizeof(wchar_t));
            if (!commands) break;
            memcpy(commands, d, dlen * sizeof(wchar_t)); commands[dlen] = 0;
            svg_path_draw(g, commands, color, vx, vy, ox, oy, scale);
            free(commands);
        }
        tag = tag_end + 1;
    }
    GdipDeleteGraphics(g);
}

static void input_region(UiEntry *e);

/* An owner-drawn control does not paint itself.
 *
 * Its WM_DRAWITEM is dispatched by the PARENT, from inside the parent's own
 * WM_PAINT, and only for the part of the parent that is actually in the
 * update region. Invalidating the child alone therefore does nothing once the
 * parent has finished painting itself: the child's update region is non-empty
 * but nobody ever walks it, so no WM_DRAWITEM is sent and the change is
 * dropped on the floor.
 *
 * That is exactly what the browser chrome was doing. A button's first paint
 * happens inside CreateWindowExW, before ->svg() has been called, so the ring
 * drawn by that first paint stuck and every later ->svg()/->radius()/->style()
 * was silently discarded. The tab survived only because render() restyles it
 * on every pass while the row beside it was built once and never touched
 * again - which is why one icon showed up and the entire toolbar did not.
 *
 * Dirty the parent over the control's own box as well, so the next parent
 * paint really does redraw it.
 */
static void hpui_dirty_ex(UiEntry *e, BOOL erase) {
    if (!e || !e->hwnd || !IsWindow(e->hwnd)) return;
    InvalidateRect(e->hwnd, NULL, erase);
    HWND par = GetParent(e->hwnd);
    if (!par) return;
    RECT r;
    GetWindowRect(e->hwnd, &r);
    POINT pts[2] = { { r.left, r.top }, { r.right, r.bottom } };
    MapWindowPoints(HWND_DESKTOP, par, pts, 2);
    RECT pr = { pts[0].x, pts[0].y, pts[1].x, pts[1].y };
    InvalidateRect(par, &pr, erase);
}
static void hpui_dirty(UiEntry *e) { hpui_dirty_ex(e, TRUE); }
/* Same, but no erase: for controls that clear their own box while drawing
 * (sliders, progress bars, the hover fade) and must not flicker. */
static void hpui_dirty_soft(UiEntry *e) { hpui_dirty_ex(e, FALSE); }

int64_t hpui_ctrl_svg(int64_t h, const char *source) {
    UiEntry *e = tbl_find(h);
    if (!e || !e->hwnd || (e->kind != HPUI_BUTTON && e->kind != HPUI_LABEL) ||
        !source || !strstr(source, "<svg") || !strstr(source, "</svg>")) return -1;
    for (const char *p = source; *p; p++) {
        /* Every shape an icon is allowed to be made of. Leaving out the
           primitives beyond <path> did not fail loudly: the markup was
           rejected whole, so the button simply came out blank. */
        static const char *const ok_tags[] = {
            "<svg", "</svg", "<path", "</path", "<g", "</g",
            "<rect", "</rect", "<circle", "</circle", "<ellipse", "</ellipse",
            "<line", "</line", "<polyline", "</polyline",
            "<polygon", "</polygon", "<defs", "</defs",
            "<?xml", "<!--", "</"
        };
        size_t t;
        for (t = 0; t < sizeof(ok_tags) / sizeof(ok_tags[0]); t++)
            if (strncmp(p, ok_tags[t], strlen(ok_tags[t])) == 0) break;
        if (t < sizeof(ok_tags) / sizeof(ok_tags[0])) continue;
        if (*p == '<') return -1;
    }
    if (strstr(source, "<script") || strstr(source, "href=") || strstr(source, "url(")) return -1;
    const char *vg = strstr(source, "viewBox=");
    const char *viewbox = strstr(source, "viewbox=");
    if (!vg || (viewbox && viewbox < vg)) vg = viewbox;
    if (!vg) return -1;
    const char *eq = strchr(vg, '=');
    if (!eq || (eq[1] != '\"' && eq[1] != '\'')) return -1;
    if (strlen(source) > 8192) return -1;
    int n = MultiByteToWideChar(CP_UTF8, 0, source, -1, NULL, 0);
    if (n <= 1 || n > 8192) return -1;
    wchar_t *w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!w) return -1;
    if (!MultiByteToWideChar(CP_UTF8, 0, source, -1, w, n)) { free(w); return -1; }
    size_t svg_len = wcslen(w);
    wchar_t *close_svg = wcsstr(w, L"</svg>");
    if (svg_len > 8192 || !close_svg || close_svg[6] != 0) {
        free(w);
        return -1;
    }
    free(e->svg);
    e->svg = w;
    /* Erase, like every other visual setter here. A non-erasing invalidate on
       an owner-drawn button does not bring WM_DRAWITEM back: the parent paints
       over the control's update region and the button is never asked to draw
       again, so a button whose only change was ->svg() stayed blank. The
       close button escaped it purely because render() calls ->style() on it
       afterwards, and style() does invalidate with an erase - which is why one
       icon showed up and the whole toolbar did not. */
    hpui_dirty(e);
    return 0;
}

/* ================= resolved colours per control ================= */
typedef struct UiPaint {
    COLORREF bg, fg, border, accent, track;
    int      radius;
    bool     bold;
} UiPaint;

static int entry_radius(UiEntry *e) {
    if (e->ov_radius >= 0) return e->ov_radius;
    return g_theme.radius;
}
static HFONT entry_font(UiEntry *e, bool role_bold) {
    int pt = e->ov_font_pt > 0 ? e->ov_font_pt : g_theme.font_pt;
    return ui_font(pt, e->ov_font_bold || role_bold);
}

/* Width of a string in the font it will actually be drawn with, in the SAME
 * logical units the layout engine works in.
 *
 * The library used to guess strlen * pt * 0.62, which is fine for average
 * text but leaves "iiii" swimming in padding and "WWWW" clipped -- that is
 * what made button padding look inconsistent from one label to the next.
 *
 * Measuring it for real is only half the job. ui_font() builds the font at
 * -MulDiv(pt, g_dpi, 72), so GetTextExtentPoint32W reports DEVICE pixels,
 * while Flow hands out rectangles in 96-DPI "logical" pixels that
 * hpui_ctrl_new() multiplies by sc() on the way to CreateWindow. Returning
 * the raw extent therefore inflated every measurement by the DPI scale (1.5x
 * on a 150% display): a button then reserved 1.5x the text it would draw,
 * which is exactly how "Danger" ended up as "Dan...". Divide it back out.
 */
int64_t hpui_textw(const char *text, int pt, bool bold) {
    if (!text || !*text) return 0;
    ensure_common();
    wchar_t w[1024];
    int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, w, 1024);
    if (n <= 1) return 0;
    n -= 1; /* n counted the terminating NUL; the extent API wants the real
             * length. GetTextExtentPoint32W rejects -1 outright (it fails with
             * ERROR_INVALID_PARAMETER), which silently produced garbage
             * widths and hence the lopsided button padding. */
    /* the fallback has to be in logical units too */
    int64_t guess = (int64_t)strlen(text) * (pt > 0 ? pt : 12) * 13 / 20;
    HDC dc = CreateCompatibleDC(NULL);
    if (!dc) return guess;
    HFONT old = (HFONT)SelectObject(dc, font_for_text(text, pt, bold));
    SIZE sz;
    sz.cx = sz.cy = 0;
    BOOL ok = GetTextExtentPoint32W(dc, w, n, &sz);
    SelectObject(dc, old);
    DeleteDC(dc);
    if (!ok || sz.cx <= 0) return guess;
    /* device pixels -> logical pixels, rounding UP so a run of text is never
     * measured a pixel short of what GDI will actually draw */
    int64_t logical = ((int64_t)sz.cx * 96 + g_dpi - 1) / (g_dpi > 0 ? g_dpi : 96);
    return logical < 1 ? 1 : logical;
}
/* How many lines `text` needs when word-wrapped to `maxw` LOGICAL pixels.
 * A label wider than its container used to be silently ellipsised; the layout
 * engine now asks this first and grows the label instead, so a long sentence
 * wraps rather than disappearing. Returns 0 for empty text, 1 if it fits. */
int64_t hpui_text_lines(const char *text, int pt, bool bold, int maxw) {
    if (!text || !*text || maxw <= 0) return text && *text ? 1 : 0;
    ensure_common();
    wchar_t w[1024];
    int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, w, 1024);
    if (n <= 1) return 0;
    n -= 1;
    HDC dc = CreateCompatibleDC(NULL);
    if (!dc) return 1;
    HFONT font = font_for_text(text, pt, bold);
    HFONT old = (HFONT)SelectObject(dc, font);
    /* BOTH numbers below must come from the same space. DT_CALCRECT reports
     * DEVICE pixels, so the line height has to be a device height too --
     * dividing it by a logical value (or worse, by the width of "Ag") made
     * every label report two lines and double the height of the whole page. */
    TEXTMETRICW tm;
    memset(&tm, 0, sizeof tm);
    GetTextMetricsW(dc, &tm);
    int lh = (int)tm.tmHeight;
    if (lh <= 0) lh = MulDiv(pt, g_dpi, 72) * 6 / 5;
    RECT r = { 0, 0, sc(maxw), sc(maxw) * 4 };
    DrawTextW(dc, w, n, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    DeleteDC(dc);
    if (r.bottom <= 0) return 1;
    /* ceil, because DT_CALCRECT can come back a hair under a whole number of
     * line boxes and would otherwise lose the final line */
    int64_t lines = ((int64_t)r.bottom + lh - 1) / lh;
    return lines < 1 ? 1 : lines;
}

/* HolyPHP colours are 0xRRGGBB integers; Win32 COLORREF is 0x00BBGGRR.
 * Every colour entering the layer from the language goes through here, or
 * the red and blue channels come out swapped. */
static COLORREF rgbv(int64_t v) {
    return RGB((COLORREF)((v >> 16) & 255), (COLORREF)((v >> 8) & 255),
               (COLORREF)(v & 255));
}
static COLORREF ov(COLORREF from, int64_t over) { return over >= 0 ? rgbv(over) : from; }

/* ================= images =================
 * A decoded image is cached by path and shared: a browser asks for the same
 * favicon once per tab, and GDI+ decoding is far too expensive to repeat on
 * every paint. The cache lives until the process ends; favicons are a handful
 * of kilobytes each and the app has at most MAX_TABS of them. */

typedef struct PicCache PicCache;
struct PicCache {
    char      *path;
    void      *bmp;                    /* GpBitmap* */
    int        w, h;
    PicCache  *next;
};

static PicCache *g_pics = NULL;

static PicCache *pic_find(const char *path) {
    for (PicCache *c = g_pics; c; c = c->next)
        if (strcmp(c->path, path) == 0) return c;
    return NULL;
}

/* Only a path on this machine is ever loaded. A URL is not a file, and
 * quietly turning a remote string into a file name is how a browser ends up
 * drawing somebody else's bytes. */
static PicCache *pic_get(const char *path) {
    if (!path || !*path) return NULL;
    if (strstr(path, "://") || strncmp(path, "data:", 5) == 0) return NULL;
    PicCache *c = pic_find(path);
    if (c) return c->bmp ? c : NULL;
    int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (n <= 1) return NULL;
    wchar_t *w = (wchar_t *)malloc(sizeof(wchar_t) * n);
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, path, -1, w, n);
    GpBitmap *bmp = NULL;
    if (GdipCreateBitmapFromFile(w, &bmp) != Ok || !bmp) { free(w); return NULL; }
    free(w);
    int iw = 0, ih = 0;
    GdipGetImageWidth((GpImage *)bmp, &iw);
    GdipGetImageHeight((GpImage *)bmp, &ih);
    if (iw <= 0 || ih <= 0) { GdipDisposeImage((GpImage *)bmp); return NULL; }
    c = (PicCache *)calloc(1, sizeof(PicCache));
    if (!c) { GdipDisposeImage((GpImage *)bmp); return NULL; }
    c->path = _strdup(path);
    c->bmp = bmp;
    c->w = iw;
    c->h = ih;
    c->next = g_pics;
    g_pics = c;
    return c;
}

/* Draw a picture fitted into `box`, centred, keeping its aspect ratio.
 * The image is composited onto `bg` first: a plain HDC cannot hold per-pixel
 * alpha, so a favicon with a transparent background would otherwise come out
 * with a black box behind it on a dark theme. */
static void pic_draw(HDC dc, const RECT *box, PicCache *c, COLORREF bg) {
    if (!c || !c->bmp) return;
    int bw = box->right - box->left, bh = box->bottom - box->top;
    if (bw <= 0 || bh <= 0) return;
    double k = (double)bw / (double)c->w;
    if ((double)bh / (double)c->h < k) k = (double)bh / (double)c->h;
    int dw = (int)((double)c->w * k + 0.5), dh = (int)((double)c->h * k + 0.5);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    if (dw > bw) dw = bw;
    if (dh > bh) dh = bh;

    HDC mem = CreateCompatibleDC(dc);
    if (!mem) return;
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = dw;
    bi.bmiHeader.biHeight = -dh;                 /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!dib) { DeleteDC(mem); return; }
    HGDIOBJ prev = SelectObject(mem, dib);
    RECT all = { 0, 0, dw, dh };
    HBRUSH br = CreateSolidBrush(bg);
    FillRect(mem, &all, br);
    DeleteObject(br);
    GpGraphics *g = NULL;
    if (GdipCreateFromHDC(mem, &g) == Ok && g) {
        GdipDrawImageRectI(g, (GpImage *)c->bmp, 0, 0, dw, dh);
        GdipDeleteGraphics(g);
    }
    BitBlt(dc, box->left + (bw - dw) / 2, box->top + (bh - dh) / 2, dw, dh,
           mem, 0, 0, SRCCOPY);
    SelectObject(mem, prev);
    DeleteObject(dib);
    DeleteDC(mem);
}

/* The icon slot of a button or label: a decoded picture when there is one,
 * otherwise the inline SVG, otherwise nothing. */
static void draw_icon_slot(HDC dc, const RECT *box, UiEntry *e, COLORREF fg,
                           COLORREF bg) {
    if (e->pic && *e->pic) {
        PicCache *c = pic_get(e->pic);
        if (c) { pic_draw(dc, box, c, bg); return; }
    }
    if (e->svg && *e->svg) draw_svg_icon(dc, *box, e->svg, fg);
}

static void resolve_paint(UiEntry *e, UiPaint *p) {
    p->bg = g_theme.bg;
    p->fg = g_theme.text;
    p->border = g_theme.border;
    p->accent = g_theme.accent;
    p->track = g_theme.border;
    p->radius = entry_radius(e);
    p->bold = false;
    bool on = IsWindowEnabled(e->hwnd);

    switch (e->kind) {
    case HPUI_BUTTON: {
        switch (e->role) {
        case UI_ROLE_PRIMARY:
            p->bg = p->accent; p->fg = g_theme.on_accent;
            p->border = p->accent; p->bold = true; break;
        case UI_ROLE_DANGER:
            p->bg = RGB(0xE5, 0x4B, 0x4B); p->fg = RGB(0xFF, 0xFF, 0xFF);
            p->border = p->bg; p->bold = true; break;
        case UI_ROLE_SUCCESS:
            p->bg = RGB(0x2F, 0xA0, 0x6A); p->fg = RGB(0xFF, 0xFF, 0xFF);
            p->border = p->bg; p->bold = true; break;
        case UI_ROLE_GHOST:
            p->bg = g_theme.bg; p->fg = g_theme.text; p->border = g_theme.bg; break;
        case UI_ROLE_LINK:
            p->bg = g_theme.bg; p->fg = p->accent; p->border = g_theme.bg; break;
        default: /* subtle */
            p->bg = g_theme.surface; p->fg = g_theme.text; p->border = g_theme.border; break;
        }
        /* A ghost button has no fill of its own, so the theme radius is what
         * gives it a shape at all; without this it painted as a hard square. */
        if (e->ov_radius <= 0 && g_theme.radius > 0) p->radius = g_theme.radius;
        if (e->anim > 0.004f) {
            /* hover fades in rather than snapping: the colour walks from the
             * resting fill towards the highlighted one */
            COLORREF hi;
            if (e->role == UI_ROLE_PRIMARY || e->role == UI_ROLE_DANGER ||
                e->role == UI_ROLE_SUCCESS) hi = lighten(p->bg, 10);
            else hi = g_theme.elevated;
            p->bg = blend(p->bg, hi, e->anim);
        }
        if (e->pressed && on) p->bg = darken(p->bg, 12);
        break;
    }
    case HPUI_CHECKBOX:
        p->bg = g_theme.bg; p->fg = g_theme.text; p->border = g_theme.border;
        break;
    case HPUI_INPUT:
    case HPUI_COMBO:
        p->bg = g_theme.elevated; p->fg = g_theme.text;
        p->border = e->focused ? p->accent : g_theme.border;
        p->radius = entry_radius(e) > 6 ? entry_radius(e) : 8;
        break;
    case HPUI_LIST:
        p->bg = g_theme.surface; p->fg = g_theme.text; p->border = g_theme.border;
        p->accent = e->focused ? g_theme.accent : g_theme.border;
        p->track = p->bg;
        break;
    case HPUI_PROGRESS:
    case HPUI_SLIDER:
        p->bg = g_theme.bg; p->fg = g_theme.text;
        p->track = g_theme.elevated; p->accent = g_theme.accent;
        break;
    case HPUI_PANEL:
        p->bg = g_theme.surface; p->fg = g_theme.text; p->border = g_theme.border;
        p->radius = entry_radius(e) + 2;
        break;
    case HPUI_GROUP:
        p->bg = g_theme.bg; p->fg = g_theme.muted; p->border = g_theme.border;
        p->bold = true;
        break;
    case HPUI_DIVIDER:
        p->bg = g_theme.bg; p->fg = g_theme.text; p->border = g_theme.border;
        break;
    default: /* LABEL and anything else */
        p->bg = g_theme.bg; p->fg = g_theme.text; p->border = g_theme.bg;
        break;
    }
    if (!on && e->kind != HPUI_LABEL && e->kind != HPUI_GROUP) {
        p->fg = g_theme.muted;
        if (e->kind == HPUI_BUTTON) { p->bg = g_theme.elevated; p->border = g_theme.border; }
    }
    p->bg = ov(p->bg, e->ov_bg);
    p->fg = ov(p->fg, e->ov_fg);
    p->border = ov(p->border, e->ov_border);
    p->accent = ov(p->accent, e->ov_accent);
}

/* ================= callback plumbing ================= */
static void ui_call_guarded(hval cb, hval *args, int nargs);

static void call_cb(hval cb, int64_t arg1, int64_t arg2) {
    if (cb.tag != HV_CLO || !cb.u.c) return;
    hval args[2];
    args[0] = hp_of_int(arg1);
    args[1] = hp_of_int(arg2);
    ui_call_guarded(cb, args, 2);
}

typedef struct CbJob {
    hval cb;
    hval args[3];
    int  nargs;
} CbJob;

static hval cb_body(void *env, hval exv) {
    (void)exv;
    CbJob *j = (CbJob *)env;
    return hp_call_value(j->cb, j->nargs, j->args);
}

static void ui_call_guarded(hval cb, hval *args, int nargs) {
    CbJob j;
    j.cb = cb;
    j.args[0] = args[0];
    j.args[1] = args[1];
    j.args[2] = nargs > 2 ? args[2] : hp_null;
    j.nargs = nargs;
    hval r = hp_try_run(cb_body, NULL, &j);
    if (r.tag != HV_NULL) {
        hstr *m = (r.tag == HV_STR) ? r.u.s : hp_val_to_str(r);
        hstr *full = hp_str_concat2(hp_str_lit("[hphp ui] uncaught in event handler: "),
                                    m ? m : hp_str_lit(""));
        fprintf(stderr, "%s\n", full ? full->data : "");
        fflush(stderr);
    }
}

static void fire(UiEntry *e, hval *slot, int64_t a1, int64_t a2) {
    if (e && slot && slot->tag == HV_CLO && slot->u.c) {
        if (hpui_win_debug())
            fprintf(stderr, "[ui] fire entry=%lld kind=%d cb=%p\n",
                    (long long)e->id, (int)e->kind, (void *)slot->u.c);
        call_cb(*slot, a1, a2);
    }
}

static void call_cb3(hval cb, int64_t arg1, int64_t arg2, int64_t arg3) {
    if (cb.tag != HV_CLO || !cb.u.c) return;
    hval args[3] = { hp_of_int(arg1), hp_of_int(arg2), hp_of_int(arg3) };
    ui_call_guarded(cb, args, 3);
}

/* repaint every live control of a window: used after a theme change */
static void repaint_window(int64_t win) {
    UiEntry *w = tbl_find(win);
    if (!w || !w->hwnd) return;
    InvalidateRect(w->hwnd, NULL, TRUE);
    for (size_t i = 0; i < tbl_len; i++)
        if (tbl[i].hwnd && IsWindow(tbl[i].hwnd)) InvalidateRect(tbl[i].hwnd, NULL, TRUE);
}

/* ================= init / event loop ================= */
static void ensure_common(void) {
    static bool done = false;
    if (!done) {
        GdiplusStartupInput gsi = {1, NULL, FALSE, FALSE};
        GdiplusStartup(&g_gdip_token, &gsi, NULL);
        INITCOMMONCONTROLSEX icc;
        icc.dwSize = sizeof icc;
        icc.dwICC = ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES |
                    ICC_STANDARD_CLASSES;
        InitCommonControlsEx(&icc);
        load_optional_apis();
        done = true;
    }
}

void hpui_init(void) { ensure_common(); }

/* internal wakeup message so a parked GetMessage loop returns to the caller */
#define HPUI_WM_WAKE (WM_APP + 0x5150)

/* HPHP_UI_DEBUG=1 traces why a window went away; tracing a browser that
 * "restarts" needs the message that ended it. */
static int hpui_win_debug(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("HPHP_UI_DEBUG"); v = (e && e[0] && e[0] != '0') ? 1 : 0; }
    return v;
}

/* Every message the runtime handles goes through here, in both the blocking
 * hpui_run_main() loop and the timed hpui_dispatch() one. The embedded browser
 * shares this thread and loop, so this is also where its pending work runs:
 * hpwv2_drain_events() calls CefDoMessageLoopWork() and runs whatever the
 * engine queued for the script. Without it CEF sits idle - it only ever posts
 * a wake message - and the browser window never appears. */
/* ================= Tab order ================= */

/* Win32 gives a window no tab order on its own: the dialog manager is what
 * walks one, and this is not a dialog. Tab therefore did nothing at all. Walk
 * the window's own controls in the order the script created them, which is
 * also the order they are laid out in reading order. */
static void focus_step(HWND win, int dir);
static void anim_tick(void);
static bool focusable(UiEntry *e) {
    if (!e->hwnd || !IsWindow(e->hwnd)) return false;
    if (!IsWindowVisible(e->hwnd) || !IsWindowEnabled(e->hwnd)) return false;
    switch (e->kind) {
    case HPUI_BUTTON: case HPUI_INPUT: case HPUI_CHECKBOX:
    case HPUI_LIST: case HPUI_COMBO: case HPUI_SLIDER:
        return true;
    default:
        return false;
    }
}

static void focus_step(HWND win, int dir) {
    HWND cur = GetFocus();
    int64_t cur_id = cur ? (int64_t)GetWindowLongPtrW(cur, GWLP_USERDATA) : 0;
    int n = (int)tbl_len;
    if (n <= 0) return;
    int start = -1;
    for (int i = 0; i < n; i++)
        if (tbl[i].parent_id && GetWindowLongPtrW(win, GWLP_USERDATA) == tbl[i].parent_id &&
            tbl[i].id == cur_id) { start = i; break; }
    for (int k = 1; k <= n; k++) {
        int i = start < 0 ? (k - 1) : (((start + dir * k) % n) + n) % n;
        UiEntry *e = &tbl[i];
        if (e->parent_id != (int64_t)GetWindowLongPtrW(win, GWLP_USERDATA)) continue;
        if (!focusable(e)) continue;
        SetFocus(e->hwnd);
        return;
    }
    /* nothing focusable left to move to: keep the caret where it is */
}

static int pump_one(bool block) {
    MSG msg;
    if (block) {
        if (!GetMessageW(&msg, NULL, 0, 0)) return 0;
    } else {
        if (!PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { hpwv2_drain_events(); return 0; }
    }
    if (msg.message != WM_QUIT) TranslateMessage(&msg);
    if (msg.message != WM_QUIT && msg.message != HPUI_WM_WAKE)
        DispatchMessageW(&msg);
    hpwv2_drain_events();
    anim_tick();
    if (msg.message == WM_QUIT) return 0;
    return 1;
}

int64_t hpui_dispatch(int ms) {
    if (ms <= 0) {
        for (;;) {
            MSG msg;
            if (!GetMessageW(&msg, NULL, 0, 0)) return 0;
            if (msg.message != WM_QUIT) TranslateMessage(&msg);
            if (msg.message != WM_QUIT && msg.message != HPUI_WM_WAKE)
                DispatchMessageW(&msg);
            hpwv2_drain_events();
            anim_tick();
            if (msg.message == WM_QUIT) return 0;
            if (msg.message == HPUI_WM_WAKE) return 1;
        }
    }
    DWORD start = GetTickCount();
    for (;;) {
        while (pump_one(false)) { }
        DWORD spent = GetTickCount() - start;
        if (spent >= (DWORD)ms) return 1;
        MsgWaitForMultipleObjects(0, NULL, FALSE, (DWORD)(ms - spent), QS_ALLINPUT);
    }
}

void hpui_run_main(void) {
    if (hpui_win_debug()) fprintf(stderr, "[ui] main loop enter (pid %lu)\n",
                                  (unsigned long)GetCurrentProcessId());
    while (pump_one(true)) { }
    if (hpui_win_debug()) fprintf(stderr, "[ui] main loop EXIT (pid %lu)\n",
                                  (unsigned long)GetCurrentProcessId());
}
void hpui_quit(void) {
    if (hpui_win_debug()) fprintf(stderr, "[ui] ui_quit -> PostQuitMessage\n");
    PostQuitMessage(0);
}

/* ================= custom control classes ================= */
static LRESULT CALLBACK ui_wndproc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK track_wndproc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK bar_wndproc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK combo_wndproc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK poplist_wndproc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK canvas_wndproc(HWND, UINT, WPARAM, LPARAM);

static void apply_dark_decor(HWND hw) {
    if (!p_DwmSetWindowAttribute) return;
    BOOL dark = g_theme.dark ? 1 : 0;
    DWORD attr = 20;                 /* DWMWA_USE_IMMERSIVE_DARK_MODE (22000+) */
    p_DwmSetWindowAttribute(hw, attr, &dark, sizeof dark);
    attr = 19;                       /* same flag on older builds */
    p_DwmSetWindowAttribute(hw, attr, &dark, sizeof dark);
    int pref = 2;                    /* DWMWCP_ROUND */
    attr = 33;                       /* DWMWA_WINDOW_CORNER_PREFERENCE */
    p_DwmSetWindowAttribute(hw, attr, &pref, sizeof pref);
}

static void register_classes(void) {
    static bool done = false;
    if (done) return;
    WNDCLASSW wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = ui_wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;              /* we erase ourselves */
    wc.lpszClassName = L"HolyPHPWnd";
    RegisterClassW(&wc);

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = track_wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpszClassName = L"HolyPHPTrack";
    RegisterClassW(&wc);

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = bar_wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"HolyPHPBar";
    RegisterClassW(&wc);

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = combo_wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpszClassName = L"HolyPHPCombo";
    RegisterClassW(&wc);

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = poplist_wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.style = CS_DROPSHADOW;
    wc.lpszClassName = L"HolyPHPPopList";
    RegisterClassW(&wc);

    /* The canvas.
     *
     * CS_DBLCLKS because a code editor without double-click-to-select-word
     * is not an editor, and CS_HREDRAW/CS_VREDRAW so a resize asks for the
     * whole surface rather than leaving strips of the old frame behind -
     * the backbuffer is blitted, not painted, so nothing else fills it in. */
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = canvas_wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpszClassName = L"HolyPHPCanvas";
    RegisterClassW(&wc);
    done = true;
}

/* ================= window ================= */
int64_t hpui_window_new(const char *title, int x, int y, int w, int h) {
    ensure_common();
    register_classes();
    int sw = sc(w), sh = sc(h);
    if (x == -32000) {   /* sentinel: center on screen */
        x = (GetSystemMetrics(SM_CXSCREEN) - sw) / 2;
        y = (GetSystemMetrics(SM_CYSCREEN) - sh) / 2;
        if (y < 0) y = 0;
    }
    int64_t id = next_id++;
    tbl_add(id, HPUI_WINDOW, NULL);
    wchar_t wtitle[512];
    MultiByteToWideChar(CP_UTF8, 0, title ? title : "", -1, wtitle, 512);
    HWND hw = CreateWindowExW(0, L"HolyPHPWnd", wtitle,
                              WS_OVERLAPPEDWINDOW,
                              x, y, sw, sh, NULL, NULL, GetModuleHandleW(NULL), NULL);
    UiEntry *e = tbl_find(id);
    if (e) e->hwnd = hw;
    if (hw) {
        SetWindowLongPtrW(hw, GWLP_USERDATA, (LONG_PTR)id);
        if (p_GetDpiForWindow) {
            UINT d = p_GetDpiForWindow(hw);
            if (d >= 72 && d <= 480) {
                if (d != (UINT)g_dpi) {
                    g_dpi = (int)d;
                    fonts_drop();
                    /* The frame was sized from the *guessed* DPI. On a monitor
                     * with a different scale that guess is wrong, and every
                     * widget inside is scaled by the real one -- so the window
                     * ends up too small, the flow wraps, and rows overlap.
                     * Re-measure now that we know. */
                    SetWindowPos(hw, NULL, 0, 0, sc(w), sc(h),
                                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                }
            }
        }
        apply_dark_decor(hw);
        ShowWindow(hw, SW_SHOW);
        UpdateWindow(hw);
    }
    return id;
}

int64_t hpui_window_show(int64_t win, bool visible) {
    UiEntry *e = tbl_find(win);
    if (!e || e->kind != HPUI_WINDOW || !e->hwnd) return -1;
    ShowWindow(e->hwnd, visible ? SW_SHOW : SW_HIDE);
    return 0;
}

int64_t hpui_window_title(int64_t win, const char *title) {
    UiEntry *e = tbl_find(win);
    if (!e || !e->hwnd) return -1;
    wchar_t w[512];
    MultiByteToWideChar(CP_UTF8, 0, title ? title : "", -1, w, 512);
    SetWindowTextW(e->hwnd, w);
    return 0;
}

int64_t hpui_window_size(int64_t win, int w, int h) {
    UiEntry *e = tbl_find(win);
    if (!e || !e->hwnd) return -1;
    SetWindowPos(e->hwnd, NULL, 0, 0, sc(w), sc(h), SWP_NOMOVE | SWP_NOZORDER);
    return 0;
}

int64_t hpui_window_pos(int64_t win, int x, int y) {
    UiEntry *e = tbl_find(win);
    if (!e || !e->hwnd) return -1;
    SetWindowPos(e->hwnd, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    return 0;
}

int64_t hpui_window_close(int64_t win) {
    UiEntry *e = tbl_find(win);
    if (!e || !e->hwnd) return -1;
    DestroyWindow(e->hwnd);
    e->hwnd = NULL;
    return 0;
}

bool hpui_window_alive(int64_t win) {
    UiEntry *e = tbl_find(win);
    return e && e->hwnd && IsWindow(e->hwnd);
}

/* ================= brush cache ================= */
/* One solid brush per colour. WM_CTLCOLOR* is called on every repaint and
 * allocating a GDI object each time leaks handles fast. */
static HBRUSH brush_for(COLORREF c) {
    static HBRUSH b[16];
    static COLORREF bc[16];
    static int bn = 0;
    for (int i = 0; i < bn; i++)
        if (bc[i] == c) return b[i];
    if (bn == 16) { DeleteObject(b[0]); memmove(b, b + 1, 15 * sizeof(HBRUSH));
                    memmove(bc, bc + 1, 15 * sizeof(COLORREF)); bn = 15; }
    b[bn] = CreateSolidBrush(c);
    bc[bn] = c;
    return b[bn++];
}

/* Clip an edit to the rounded field shape so it can paint an opaque
 * background without losing the rounded corners. */
static void input_region(UiEntry *e) {
    if (!e->hwnd || e->kind != HPUI_INPUT) return;
    RECT rc;
    GetClientRect(e->hwnd, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    UiPaint p;
    resolve_paint(e, &p);
    int rr = (p.radius > 0 ? p.radius : 6) * 2;
    if (rr > h) rr = h;
    HRGN r = CreateRoundRectRgn(0, 0, w + 1, h + 1, rr, rr);
    SetWindowRgn(e->hwnd, r, TRUE);   /* system owns r on success */
}

/* ================= scrolling =================
 * A window taller than its content is fine; content taller than the window
 * used to simply run off the bottom with no way to reach it. Every control
 * remembers its unscrolled logical box, so scrolling is just replaying those
 * boxes at an offset -- no re-layout, no second window. */
static void scroll_place(UiEntry *win) {
    if (!win->hwnd) return;
    RECT rc;
    GetClientRect(win->hwnd, &rc);
    int view = rc.bottom - rc.top;
    int max = win->content_h - view;
    if (max < 0) max = 0;
    if (win->scroll_y > max) win->scroll_y = max;
    if (win->scroll_y < 0) win->scroll_y = 0;
    for (size_t i = 0; i < tbl_len; i++) {
        UiEntry *c = &tbl[i];
        if (!c->hwnd || c->hwnd == win->hwnd) continue;
        if (GetParent(c->hwnd) != win->hwnd) continue;
        SetWindowPos(c->hwnd, NULL, sc(c->base_x), sc(c->base_y - win->scroll_y),
                     sc(c->base_w), sc(c->base_h),
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

static void scroll_refresh(UiEntry *win) {
    if (!win->hwnd) return;
    int max = 0;
    for (size_t i = 0; i < tbl_len; i++) {
        UiEntry *c = &tbl[i];
        if (!c->hwnd || c->hwnd == win->hwnd) continue;
        if (GetParent(c->hwnd) != win->hwnd) continue;
        int b = c->base_y + c->base_h;
        if (b > max) max = b;
    }
    win->content_h = max;
    scroll_place(win);
}

static void scroll_by(UiEntry *win, int dy) {
    if (!win->hwnd) return;
    int before = win->scroll_y;
    win->scroll_y += dy;
    scroll_place(win);
    if (win->scroll_y != before)
        InvalidateRect(win->hwnd, NULL, FALSE);
}

/* Scroll state, for scripts that want to drive it themselves and for the
 * headless layout test. out_scroll receives the clamped offset, out_content
 * the full content height; either may be NULL. */
int64_t hpui_window_scroll(int64_t h, int dy, int to, int64_t *out_scroll,
                           int64_t *out_content) {
    UiEntry *win = tbl_find(h);
    if (!win || win->kind != HPUI_WINDOW) return -1;
    if (to >= 0) win->scroll_y = to;
    if (dy) scroll_by(win, dy);
    if (!win->hwnd) return -1;
    scroll_place(win);
    RECT rc;
    GetClientRect(win->hwnd, &rc);
    if (out_scroll) *out_scroll = win->scroll_y;
    if (out_content) *out_content = win->content_h;
    return (int64_t)(rc.bottom - rc.top);
}

/* ================= hover / state subclass for native controls ================= */
/* Buttons, checkboxes and edits are real Win32 controls, but they look like
 * Win95 without help, so we subclass them to track hover/press/focus and to
 * paint our own rounded background under the text. */
static void hover_enter(UiEntry *e) {
    if (e->hovered) return;
    e->hovered = true;
    TRACKMOUSEEVENT t;
    memset(&t, 0, sizeof t);
    t.cbSize = sizeof t;
    t.dwFlags = TME_LEAVE;
    t.hwndTrack = e->hwnd;
    t.dwHoverTime = 0;
    TrackMouseEvent(&t);
    hpui_dirty(e);
}
static void hover_leave(UiEntry *e) {
    if (!e->hovered) return;
    e->hovered = false;
    e->hover_index = -1;
    hpui_dirty(e);
}

/* Ease every hover fade a step towards its target and repaint the ones still
 * moving. Nothing here runs on a timer of its own: the UI pump calls it once
 * per message-loop turn, so an idle window costs nothing. */
static void anim_tick(void) {
    DWORD now = GetTickCount();
    static DWORD last = 0;
    if (last == 0) last = now;
    DWORD dt = now - last;
    if (dt == 0 || dt > 250) dt = 16;
    last = now;
    float step = 0.016f * (float)dt / 16.0f;      /* ~60 ms fade 0 -> 1 */
    for (size_t i = 0; i < tbl_len; i++) {
        UiEntry *e = &tbl[i];
        if (!e->hwnd || !IsWindow(e->hwnd)) continue;
        if (e->kind != HPUI_BUTTON && e->kind != HPUI_CHECKBOX &&
            e->kind != HPUI_INPUT) continue;
        float want = e->hovered ? 1.0f : 0.0f;
        float before = e->anim;
        if (e->anim < want) { e->anim += step; if (e->anim > 1.0f) e->anim = 1.0f; }
        else if (e->anim > want) { e->anim -= step; if (e->anim < 0.0f) e->anim = 0.0f; }
        if (before != e->anim) hpui_dirty_soft(e);
    }
}

static LRESULT CALLBACK hover_proc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    UiEntry *e = entry_of(hw);
    if (!e) return DefWindowProcW(hw, msg, wp, lp);
    switch (msg) {
    case WM_NCHITTEST:
        /* A label that only decorates must not eat clicks meant for whatever
         * is under it. A browser draws the lock or the site glyph inside its
         * address bar, but a native edit cannot host one, so the glyph has to
         * be a sibling label stacked on top - and without this, clicking the
         * left of the bar to put the caret there did nothing at all. */
        if (e->kind == HPUI_LABEL && e->cb_click.tag != HV_CLO)
            return HTTRANSPARENT;
        break;
    case WM_MOUSEMOVE:
        hover_enter(e);
        /* Captured moves report screen coordinates, so the delta is measured
           from the press rather than from the control's own origin. */
        if (e->drag_active && e->cb_drag.tag == HV_CLO && e->cb_drag.u.c) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hw, &pt);
            call_cb3(e->cb_drag, (int64_t)(pt.x - e->drag_x),
                     (int64_t)(pt.y - e->drag_y), 1);  /* phase 1: move */
            return 0;
        }
        break;
    case WM_MOUSELEAVE: hover_leave(e); break;
    /* Every class is registered with IDC_ARROW, so the pointer stayed an
     * arrow over the buttons that do something and an I-beam bar over the
     * text fields. Say what the control is. */
    case WM_SETCURSOR:
        if (e->kind == HPUI_BUTTON || e->kind == HPUI_CHECKBOX) {
            SetCursor(LoadCursorW(NULL, IDC_HAND));
            return TRUE;
        }
        if (e->kind == HPUI_INPUT) {
            SetCursor(LoadCursorW(NULL, IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN:
        if (hpui_win_debug())
            fprintf(stderr, "[ui] ctrl down entry=%lld kind=%d\n",
                    (long long)e->id, (int)e->kind);
        e->pressed = true;
        /* A drag handler takes the mouse: the control has to see the moves and
         * the release even though the cursor may leave it long before either. */
        if (e->cb_drag.tag == HV_CLO && e->cb_drag.u.c) {
            e->drag_x = GET_X_LPARAM(lp);
            e->drag_y = GET_Y_LPARAM(lp);
            SetCapture(hw);
            e->drag_active = true;
            call_cb3(e->cb_drag, 0, 0, 0);          /* phase 0: press */
            return 0;
        }
        /* An edit MUST still see the click: swallowing it left the control
         * with no caret, so typing looked dead and Ctrl+A had nothing to
         * select. Track the state, then let the real control have it. */
        if (e->kind == HPUI_INPUT) {
            LRESULT r = CallWindowProcW(e->old_proc, hw, msg, wp, lp);
            InvalidateRect(hw, NULL, TRUE);
            return r;
        }
        InvalidateRect(hw, NULL, TRUE);
        break;
    /* A drag that ends anywhere, not just over the control. */
    case WM_LBUTTONUP:
        if (hpui_win_debug())
            fprintf(stderr, "[ui] ctrl up entry=%lld kind=%d\n",
                    (long long)e->id, (int)e->kind);
        e->pressed = false;
        if (e->drag_active && e->cb_drag.tag == HV_CLO && e->cb_drag.u.c) {
            e->drag_active = false;
            /* ReleaseCapture sends WM_CAPTURECHANGED straight away, and that
               path also ends the drag - the flag is what stops it happening
               twice for one gesture. */
            ReleaseCapture();
            call_cb3(e->cb_drag, 0, 0, 2);           /* phase 2: release */
            return 0;
        }
        if (e->kind == HPUI_INPUT) {
            LRESULT r = CallWindowProcW(e->old_proc, hw, msg, wp, lp);
            InvalidateRect(hw, NULL, TRUE);
            return r;
        }
        InvalidateRect(hw, NULL, TRUE);
        if (e->kind == HPUI_CHECKBOX) {
            e->checked_flag = !e->checked_flag;
            InvalidateRect(hw, NULL, TRUE);
            fire(e, &e->cb_change, e->id, e->checked_flag ? 1 : 0);
        }
        break;
    case WM_CAPTURECHANGED:
        e->pressed = false;
        if (e->drag_active && e->cb_drag.tag == HV_CLO && e->cb_drag.u.c) {
            e->drag_active = false;
            call_cb3(e->cb_drag, 0, 0, 2);           /* phase 2: cancelled */
        }
        break;
    case WM_SETFOCUS:  e->focused = true;  InvalidateRect(hw, NULL, TRUE); break;
    case WM_KILLFOCUS: e->focused = false; InvalidateRect(hw, NULL, TRUE); break;
    case WM_GETDLGCODE:
        /* the caret has to move with the arrow keys even though this is not a
         * dialog -- without DLGC_WANTARROWS the parent eats them */
        if (e->kind == HPUI_INPUT)
            return (LRESULT)(DefWindowProcW(hw, msg, wp, lp) | DLGC_WANTARROWS);
        break;
    case WM_KEYDOWN:
        /* A control with WS_TABSTOP eats Tab and nothing forwards it, so
         * focus could never leave a button or an edit. Send it up instead. */
        if ((int)wp == VK_TAB && !(GetKeyState(VK_CONTROL) & 0x8000)) {
            HWND par = GetParent(hw);
            if (par) {
                focus_step(par, (GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1);
                return 0;
            }
        }
        if (e->kind == HPUI_INPUT && e->cb_key.tag == HV_CLO && e->cb_key.u.c) {
            int vk = (int)wp;
            BYTE state[256];
            GetKeyboardState(state);
            wchar_t text[8];
            int n = ToUnicode((UINT)vk, (UINT)((lp >> 16) & 0xFF), state, text, 8, 0);
            int64_t ch = n > 0 ? (int64_t)text[0] : 0;
            if (vk == VK_RETURN) ch = '\r';
            else if (vk == VK_BACK) ch = '\b';
            int mods = 0;
            if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= 1;
            if (GetAsyncKeyState(VK_SHIFT) & 0x8000) mods |= 2;
            if (GetAsyncKeyState(VK_MENU) & 0x8000) mods |= 4;
            call_cb3(e->cb_key, vk, ch, mods);
            if ((mods & 1) && (vk == 'A' || vk == 'L')) SendMessageW(hw, EM_SETSEL, 0, -1);
            if (vk == VK_RETURN || vk == VK_UP || vk == VK_DOWN ||
                (mods & 1 && (vk == VK_TAB || vk == 'A' || vk == 'L' || vk == 'T' || vk == 'W' || vk == 'J' ||
                              vk == 'R' || vk == 'Q' || vk == '0' || vk == VK_OEM_COMMA ||
                              vk == VK_OEM_PLUS || vk == VK_OEM_MINUS)) ||
                (mods & 4 && (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_HOME)) ||
                (!(mods & 7) && (vk == VK_F5 || vk == VK_F12))) return 0;
        }
        /* Ctrl+A. The stock edit only honours it on some builds, and a
         * subclassed control is exactly the case where it goes missing, so
         * do it here rather than leave select-all silently dead. */
        if (e->kind == HPUI_INPUT && (int)wp == 'A' &&
            (GetKeyState(VK_CONTROL) & 0x8000) &&
            !(GetKeyState(VK_SHIFT) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000)) {
            SendMessageW(hw, EM_SETSEL, 0, -1);
            return 0;
        }
        break;
    case WM_ENABLE:
        InvalidateRect(hw, NULL, TRUE);
        break;
    case WM_ERASEBKGND:
        /* edits paint their own rounded field here; the text that follows is
         * drawn on top of it (opaquely, so nothing ghosts) */
        if (e->kind == HPUI_INPUT) {
            UiPaint p;
            resolve_paint(e, &p);
            RECT rc;
            GetClientRect(hw, &rc);
            input_region(e);
            fill_round((HDC)wp, rc, p.radius, p.bg, p.border, e->focused ? 2 : 1);
            return 1;
        }
        break;
    default: break;
    }
    return CallWindowProcW(e->old_proc, hw, msg, wp, lp);
}

static void subclass(UiEntry *e) {
    LONG_PTR r = SetWindowLongPtrW(e->hwnd, GWLP_WNDPROC, (LONG_PTR)hover_proc);
    if (r) e->old_proc = (WNDPROC)r;
}

static void untheme(HWND hw) {
    /* drop the visual styles so Win32 does not repaint our flat colours */
    if (p_SetWindowTheme) p_SetWindowTheme(hw, L"", L"");
}

/* ================= control creation ================= */
int64_t hpui_ctrl_new(int64_t kind, int64_t parent, const char *text,
                      int x, int y, int w, int h) {
    UiEntry *p = tbl_find(parent);
    if (!p || !p->hwnd) return -1;
    ensure_common();
    register_classes();
    int64_t id = next_id++;
    HWND hw = NULL;
    wchar_t wt[512];
    MultiByteToWideChar(CP_UTF8, 0, text ? text : "", -1, wt, 512);
    DWORD base = WS_CHILD | WS_VISIBLE;
    HWND host = p->hwnd;
    int sx = sc(x), sy = sc(y), sw = sc(w), sh = sc(h);

    switch (kind) {
    case HPUI_BUTTON:
        hw = CreateWindowExW(0, L"BUTTON", wt, base | BS_OWNERDRAW,
                             sx, sy, sw, sh, host, NULL, GetModuleHandleW(NULL), NULL);
        break;
    case HPUI_LABEL:
        /* owner-draw, so labels get the theme colour, vertical centring and
         * ellipsis behaviour instead of whatever the system font does */
        hw = CreateWindowExW(0, L"STATIC", wt, base | SS_OWNERDRAW,
                             sx, sy, sw, sh, host, NULL, GetModuleHandleW(NULL), NULL);
        untheme(hw);
        break;
    case HPUI_INPUT:
        hw = CreateWindowExW(0, L"EDIT", wt,
                             base | ES_AUTOHSCROLL | ES_LEFT,
                             sx, sy, sw, sh, host, NULL, GetModuleHandleW(NULL), NULL);
        untheme(hw);
        break;
    case HPUI_CHECKBOX:
        hw = CreateWindowExW(0, L"BUTTON", wt, base | BS_OWNERDRAW,
                             sx, sy, sw, sh, host, NULL, GetModuleHandleW(NULL), NULL);
        break;
    case HPUI_LIST:
        hw = CreateWindowExW(0, L"LISTBOX", wt,
                             base | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT |
                             LBS_OWNERDRAWFIXED | WS_VSCROLL,
                             sx, sy, sw, sh, host, NULL, GetModuleHandleW(NULL), NULL);
        untheme(hw);
        SendMessageW(hw, LB_SETITEMHEIGHT, 0, (LPARAM)sc(g_theme.row_h));
        break;
    case HPUI_COMBO:
        hw = CreateWindowExW(0, L"HolyPHPCombo", wt, base | WS_TABSTOP,
                             sx, sy, sw, sh, host, NULL, GetModuleHandleW(NULL), NULL);
        break;
    case HPUI_PROGRESS:
        hw = CreateWindowExW(0, L"HolyPHPBar", NULL, base,
                             sx, sy, sw, sh, host, NULL, GetModuleHandleW(NULL), NULL);
        break;
    case HPUI_SLIDER:
        hw = CreateWindowExW(0, L"HolyPHPTrack", NULL, base | WS_TABSTOP,
                             sx, sy, sw, sh, host, NULL, GetModuleHandleW(NULL), NULL);
        break;
    case HPUI_GROUP:
    case HPUI_PANEL:
        hw = CreateWindowExW(0, L"STATIC", wt, base | SS_OWNERDRAW,
                             sx, sy, sw, sh, host, NULL, GetModuleHandleW(NULL), NULL);
        untheme(hw);
        break;
    case HPUI_DIVIDER:
        hw = CreateWindowExW(0, L"STATIC", NULL, base | SS_OWNERDRAW,
                             sx, sy, sw, sh, host, NULL, GetModuleHandleW(NULL), NULL);
        untheme(hw);
        break;
    case HPUI_CANVAS:
        /* No theme, no text, no system painting: the script owns every pixel
         * and the toolkit only hands it a surface and the input. WS_TABSTOP
         * is deliberately absent - Tab inside the editor must reach the
         * canvas' own key handler, not move focus to the next control. */
        hw = CreateWindowExW(0, L"HolyPHPCanvas", NULL, base,
                             sx, sy, sw, sh, host, NULL, GetModuleHandleW(NULL), NULL);
        break;
    default:
        return -1;
    }
    if (!hw) return -1;
    tbl_add(id, (int)kind, hw);
    UiEntry *e = tbl_find(id);
    if (kind == HPUI_CANVAS) {
        for (int i = 0; i < HPUI_EV_NEV; i++) e->cb_ev[i] = hp_null;
        e->cv_cursor = HPUI_CUR_ARROW;
        cv_ensure(e);
    }
    e->parent_id = parent;
    e->base_x = x; e->base_y = y; e->base_w = w; e->base_h = h;
    /* An edit's text starts hard against the window edge, which is what made
     * the omnibox look cramped. A real left and right margin gives it the
     * breathing room a browser address bar has. */
    if (kind == HPUI_INPUT) {
        RECT m;
        m.left = m.right = sc(g_theme.pad - 4 > 0 ? g_theme.pad - 4 : 4);
        SendMessageW(hw, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     (LPARAM)&m);
    }
    /* honour whatever the window has already scrolled to */
    if (p && p->scroll_y != 0)
        SetWindowPos(hw, NULL, sx, sc(y - p->scroll_y), sw, sh,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowLongPtrW(hw, GWLP_USERDATA, (LONG_PTR)id);
    fonts_apply(hw);
    if (kind == HPUI_BUTTON || kind == HPUI_CHECKBOX || kind == HPUI_INPUT ||
        kind == HPUI_LABEL)
        subclass(e);
        /* Clip to the rounded field NOW rather than waiting for the first
         * WM_ERASEBKGND. An edit only gets one if something else asks the
         * system to erase it first, and nothing here does, so a control whose
         * whole look is its rounded corners came out a hard square - and then
         * WM_CTLCOLOREDIT painted the opaque field right over the top of the
         * corners the region was supposed to preserve. */
        input_region(e);
    if (p_SetWindowTheme) {
        p_SetWindowTheme(hw, g_theme.dark ? L"DarkMode_Explorer" : L"Explorer",
                         g_theme.dark ? NULL : L"");
    }
    /* The very first paint happens inside CreateWindowExW, before tbl_add(),
     * so an owner-drawn control would find no entry and draw nothing. Ask it
     * to paint again now that it is fully registered. */
    InvalidateRect(hw, NULL, FALSE);
    /* a new child changes how far the window has to scroll */
    if (p && p->kind == HPUI_WINDOW) scroll_refresh(p);
    return id;
}

int64_t hpui_ctrl_set_text(int64_t h, const char *text) {
    UiEntry *e = tbl_find(h);
    if (!e || !e->hwnd) return -1;
    wchar_t w[1024];
    MultiByteToWideChar(CP_UTF8, 0, text ? text : "", -1, w, 1024);
    if (e->kind == HPUI_COMBO && e->sel >= 0 && e->sel < e->n_items) {
        wcsncpy(e->items[e->sel], w, 1023);
        e->items[e->sel][1023] = 0;
    }
    SetWindowTextW(e->hwnd, w);
    hpui_dirty(e);
    return 0;
}

int64_t hpui_ctrl_text(int64_t h, hstr **out) {
    UiEntry *e = tbl_find(h);
    if (!e || !e->hwnd) return -1;
    int n = GetWindowTextLengthW(e->hwnd);
    wchar_t *w = (wchar_t *)malloc((size_t)(n + 1) * sizeof(wchar_t));
    if (!w) return -1;
    GetWindowTextW(e->hwnd, w, n + 1);
    int need = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *u = (char *)malloc(need ? (size_t)need : 1);
    if (!u) { free(w); return -1; }
    if (need) WideCharToMultiByte(CP_UTF8, 0, w, -1, u, need, NULL, NULL);
    free(w);
    *out = hp_str_new(u, (size_t)(need > 0 ? need - 1 : 0));
    free(u);
    return 0;
}

int64_t hpui_ctrl_show(int64_t h, bool visible) {
    UiEntry *e = tbl_find(h);
    if (!e || !e->hwnd) return -1;
    ShowWindow(e->hwnd, visible ? SW_SHOW : SW_HIDE);
    /* The autocomplete is a list overlay and is created before its WebView2
     * controller. Raise only that popup; raising every shown child would put
     * tab buttons above their own close and favicon controls. */
    if (visible && e->kind == HPUI_LIST)
        SetWindowPos(e->hwnd, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    return 0;
}

int64_t hpui_ctrl_enable(int64_t h, bool enabled) {
    UiEntry *e = tbl_find(h);
    if (!e || !e->hwnd) return -1;
    EnableWindow(e->hwnd, enabled);
    hpui_dirty(e);
    return 0;
}

int64_t hpui_ctrl_move(int64_t h, int x, int y, int w, int hgt) {
    UiEntry *e = tbl_find(h);
    if (!e || !e->hwnd) return -1;
    e->base_x = x; e->base_y = y; e->base_w = w; e->base_h = hgt;
    /* the card a Layout resizes may now be taller than the window */
    HWND par = GetParent(e->hwnd);
    if (par) {
        UiEntry *pe = entry_of(par);
        if (pe) scroll_refresh(pe);
    }
    MoveWindow(e->hwnd, sc(x), sc(y), sc(w), sc(hgt), TRUE);
    input_region(e);   /* the corner radius is a fraction of the height */
    return 0;
}

/* Where a control is actually painted, in window coordinates: its own
 * rectangle plus every ancestor's offset, minus whatever the window has
 * scrolled. Two siblings that report intersecting rects really are drawn on
 * top of each other, and a control that scrolled off the fold reports a
 * negative y -- which is exactly what a headless layout test needs. */
int64_t hpui_ctrl_rect(int64_t h, int64_t *x, int64_t *y, int64_t *w, int64_t *ht) {
    UiEntry *e = tbl_find(h);
    if (!e) return -1;
    int64_t ax = 0, ay = 0;
    for (UiEntry *c = e; c; ) {
        ax += c->base_x;
        ay += c->base_y;
        UiEntry *par = (c->hwnd && GetParent(c->hwnd)) ? entry_of(GetParent(c->hwnd)) : NULL;
        if (par) ay -= par->scroll_y;
        c = par;
    }
    *x = ax; *y = ay; *w = e->base_w; *ht = e->base_h;
    return 0;
}

int64_t hpui_ctrl_focus(int64_t h) {
    UiEntry *e = tbl_find(h);
    if (!e || !e->hwnd) return -1;
    SetFocus(e->hwnd);
    return 0;
}

int64_t hpui_ctrl_select_all(int64_t h) {
    UiEntry *e = tbl_find(h);
    if (!e || !e->hwnd || e->kind != HPUI_INPUT) return -1;
    SendMessageW(e->hwnd, EM_SETSEL, 0, -1);
    return 0;
}

/* ================= list / combo items ================= */
static void combo_add(UiEntry *e, const wchar_t *s) {
    if (e->n_items == e->cap_items) {
        e->cap_items = e->cap_items ? e->cap_items * 2 : 8;
        e->items = (wchar_t **)realloc(e->items, (size_t)e->cap_items * sizeof(wchar_t *));
    }
    size_t n = wcslen(s);
    wchar_t *copy = (wchar_t *)malloc((n + 1) * sizeof(wchar_t));
    memcpy(copy, s, (n + 1) * sizeof(wchar_t));
    e->items[e->n_items++] = copy;
    if (e->sel < 0 && e->n_items == 1) e->sel = 0;
}

/* Owner-draw one listbox row. LBS_OWNERDRAWFIXED means Windows never holds
 * the text itself, so it is read from the entry's own item array. */
static void draw_list_item(DRAWITEMSTRUCT *di) {
    UiEntry *e = entry_of(di->hwndItem);
    int idx = (int)di->itemID;
    RECT rc = di->rcItem;
    HDC dc = di->hDC;
    if (!e || idx < 0 || idx >= e->n_items) return;
    if (di->itemState & ODS_SELECTED) fill_round(dc, rc, sc(6), tint(g_theme.accent), (COLORREF)-1, 0);
    else if (!(di->itemState & ODS_SELECTED) && (di->itemState & ODS_HOTLIGHT))
        fill_round(dc, rc, sc(6),
                   g_theme.dark ? lighten(g_theme.elevated, 6) : darken(g_theme.elevated, 6),
                   (COLORREF)-1, 0);
    RECT tr = rc;
    tr.left += sc(8);
    tr.right -= sc(6);
    COLORREF fg = (di->itemState & ODS_SELECTED) ? g_theme.on_accent : g_theme.text;
    draw_str(dc, tr, e->items[idx], fg, ui_font(g_theme.font_pt, false),
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

int64_t hpui_list_add(int64_t h, const char *item) {
    UiEntry *e = tbl_find(h);
    if (!e || !e->hwnd) return -1;
    wchar_t w[1024];
    MultiByteToWideChar(CP_UTF8, 0, item ? item : "", -1, w, 1024);
    /* e->items is the single source of truth: LBS_OWNERDRAWFIXED listboxes
     * report no text through LB_GETTEXT, so we must keep our own copy. */
    combo_add(e, w);
    if (e->kind == HPUI_LIST)
        SendMessageW(e->hwnd, LB_ADDSTRING, 0, (LPARAM)w);
    hpui_dirty(e);
    return e->n_items - 1;
}

int64_t hpui_list_clear(int64_t h) {
    UiEntry *e = tbl_find(h);
    if (!e || !e->hwnd) return -1;
    items_free(e);
    e->sel = -1;
    if (e->kind == HPUI_LIST) SendMessageW(e->hwnd, LB_RESETCONTENT, 0, 0);
    hpui_dirty(e);
    return 0;
}

int64_t hpui_list_remove(int64_t h, int64_t index) {
    UiEntry *e = tbl_find(h);
    if (!e || !e->hwnd) return -1;
    if (index < 0 || index >= e->n_items) return -1;
    free(e->items[index]);
    for (int i = (int)index; i < e->n_items - 1; i++) e->items[i] = e->items[i + 1];
    e->n_items--;
    if (e->sel >= e->n_items) e->sel = e->n_items - 1;
    if (e->kind == HPUI_LIST) SendMessageW(e->hwnd, LB_DELETESTRING, (WPARAM)index, 0);
    hpui_dirty(e);
    return 0;
}

int64_t hpui_list_count(int64_t h, int64_t *out) {
    UiEntry *e = tbl_find(h);
    if (!e || !e->hwnd) return -1;
    *out = e->n_items;
    return 0;
}

int64_t hpui_list_selected(int64_t h, int64_t *out) {
    UiEntry *e = tbl_find(h);
    if (!e || !e->hwnd) return -1;
    if (e->kind == HPUI_COMBO) { *out = e->sel; return 0; }
    LRESULT r = SendMessageW(e->hwnd, LB_GETCURSEL, 0, 0);
    *out = (r == LB_ERR) ? -1 : (int64_t)r;
    return 0;
}

int64_t hpui_list_select(int64_t h, int64_t index) {
    UiEntry *e = tbl_find(h);
    if (!e || !e->hwnd) return -1;
    if (e->kind == HPUI_COMBO) {
        e->sel = (index >= 0 && index < e->n_items) ? (int)index : -1;
        hpui_dirty(e);
        return 0;
    }
    SendMessageW(e->hwnd, LB_SETCURSEL, (WPARAM)index, 0);
    return 0;
}

int64_t hpui_list_text(int64_t h, int64_t index, hstr **out) {
    UiEntry *e = tbl_find(h);
    if (!e || !e->hwnd) return -1;
    wchar_t w[1024];
    /* owner-draw listboxes answer LB_GETTEXT with nothing, so read our copy */
    if (index < 0 || index >= e->n_items) return -1;
    wcsncpy(w, e->items[index], 1023);
    w[1023] = 0;
    int need = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *u = (char *)malloc(need ? (size_t)need : 1);
    if (!u) return -1;
    if (need) WideCharToMultiByte(CP_UTF8, 0, w, -1, u, need, NULL, NULL);
    *out = hp_str_new(u, (size_t)(need > 0 ? need - 1 : 0));
    free(u);
    return 0;
}

/* ================= checkbox / progress / slider state ================= */
int64_t hpui_check_get(int64_t h, bool *out) {
    UiEntry *e = tbl_find(h);
    if (!e || !e->hwnd) return -1;
    *out = e->checked_flag;
    return 0;
}

int64_t hpui_check_set(int64_t h, bool on) {
    UiEntry *e = tbl_find(h);
    if (!e || !e->hwnd) return -1;
    e->checked_flag = on;
    hpui_dirty(e);
    return 0;
}

int64_t hpui_progress_set(int64_t h, int64_t pct) {
    UiEntry *e = tbl_find(h);
    if (!e || !e->hwnd) return -1;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    e->value = (int)pct;
    hpui_dirty_soft(e);
    return 0;
}

int64_t hpui_progress_get(int64_t h) {
    UiEntry *e = tbl_find(h);
    return e ? e->value : 0;
}

int64_t hpui_slider_get(int64_t h, int64_t *out) {
    UiEntry *e = tbl_find(h);
    if (!e || !e->hwnd) return -1;
    *out = e->value;
    return 0;
}

int64_t hpui_slider_set(int64_t h, int64_t pos) {
    UiEntry *e = tbl_find(h);
    if (!e || !e->hwnd) return -1;
    if (pos < e->min_v) pos = e->min_v;
    if (pos > e->max_v) pos = e->max_v;
    e->value = (int)pos;
    hpui_dirty_soft(e);
    return 0;
}

int64_t hpui_range_set(int64_t h, int64_t lo, int64_t hi) {
    UiEntry *e = tbl_find(h);
    if (!e || !e->hwnd) return -1;
    if (hi <= lo) return -1;
    e->min_v = (int)lo;
    e->max_v = (int)hi;
    if (e->value < e->min_v) e->value = e->min_v;
    if (e->value > e->max_v) e->value = e->max_v;
    hpui_dirty_soft(e);
    return 0;
}

int64_t hpui_ctrl_enabled(int64_t h) {
    UiEntry *e = tbl_find(h);
    if (!e || !e->hwnd) return 0;
    return IsWindowEnabled(e->hwnd) ? 1 : 0;
}

int64_t hpui_ctrl_checked(int64_t h) {
    UiEntry *e = tbl_find(h);
    return (e && e->checked_flag) ? 1 : 0;
}

/* ================= colours / fonts / roles ================= */
int64_t hpui_ctrl_set_bg(int64_t h, int64_t rgb) {
    UiEntry *e = tbl_find(h);
    if (!e) return -1;
    if (e->kind == HPUI_WINDOW) { g_theme.bg = rgbv(rgb); repaint_window(h); return 0; }
    e->ov_bg = rgb;
    hpui_dirty(e);
    return 0;
}

int64_t hpui_ctrl_set_fg(int64_t h, int64_t rgb) {
    UiEntry *e = tbl_find(h);
    if (!e) return -1;
    if (e->kind == HPUI_WINDOW) { g_theme.text = rgbv(rgb); repaint_window(h); return 0; }
    e->ov_fg = rgb;
    hpui_dirty(e);
    return 0;
}

int64_t hpui_ctrl_accent(int64_t h, int64_t rgb) {
    UiEntry *e = tbl_find(h);
    if (!e || !e->hwnd) return -1;
    e->ov_accent = rgb;
    hpui_dirty(e);
    return 0;
}

int64_t hpui_ctrl_radius(int64_t h, int64_t r) {
    UiEntry *e = tbl_find(h);
    if (!e || !e->hwnd) return -1;
    e->ov_radius = (int)r;
    input_region(e);   /* the window region is what shows the radius at all */
    hpui_dirty(e);
    return 0;
}

/* ui_input_margin(h, left, right) - inset an edit's text from its own edges,
 * in logical pixels.
 *
 * A control drawn by this library lays its own icon out inside its box; a
 * native edit cannot, because its text always starts wherever the system says.
 * An address bar with a leading site/lock glyph needs a real left margin or
 * the glyph and the first letter end up on top of each other. */
int64_t hpui_input_margin(int64_t h, int64_t l, int64_t r) {
    UiEntry *e = tbl_find(h);
    if (!e || !e->hwnd || e->kind != HPUI_INPUT) return -1;
    RECT m;
    m.left = sc((int)l);
    m.right = sc((int)r);
    if (m.left < 0) m.left = 0;
    if (m.right < 0) m.right = 0;
    SendMessageW(e->hwnd, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 (LPARAM)&m);
    hpui_dirty(e);
    return 0;
}

int64_t hpui_ctrl_style(int64_t h, int64_t role) {
    UiEntry *e = tbl_find(h);
    if (!e || !e->hwnd) return -1;
    e->role = (int)role;
    hpui_dirty(e);
    return 0;
}

int64_t hpui_ctrl_font(int64_t h, int64_t size, bool bold) {
    UiEntry *e = tbl_find(h);
    if (!e || !e->hwnd) return -1;
    if (size > 0) e->ov_font_pt = (int)size;
    e->ov_font_bold = bold;
    SendMessageW(e->hwnd, WM_SETFONT,
                 (WPARAM)entry_font(e, false), TRUE);
    hpui_dirty(e);
    return 0;
}

/* Creation order is z-order, and that is not enough: a control that is shown
 * again after being hidden jumps to the top, so which of two overlapping
 * siblings receives a click depends on the order they were last shown in.
 * Anything meant to sit on top of another control - a tab's close button over
 * its own tab - has to say so. */
int64_t hpui_ctrl_raise(int64_t h) {
    UiEntry *e = tbl_find(h);
    if (!e || !e->hwnd) return -1;
    SetWindowPos(e->hwnd, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    return 0;
}

int64_t hpui_picture_load(int64_t h, const char *path) {
    UiEntry *e = tbl_find(h);
    if (!e || !e->hwnd) return -1;
    if (e->kind != HPUI_BUTTON && e->kind != HPUI_LABEL &&
        e->kind != HPUI_PANEL && e->kind != HPUI_GROUP) return -1;
    ensure_common();
    char *want = (path && *path) ? _strdup(path) : NULL;
    if (!path || !*path) { free(e->pic); e->pic = NULL; hpui_dirty(e); return 0; }
    /* Decode once up front so a bad path is reported now rather than as a
       blank slot that silently falls back to the SVG. */
    if (!pic_get(path)) { free(want); return -1; }
    free(e->pic);
    e->pic = want;
    hpui_dirty(e);
    return 0;
}

/* ui_theme(win, bg, surface, elevated, text, muted, accent, onAccent, border,
 *         radius, fontPt, titlePt, rowH, dark)
 * Sets the universal look for the window and every control inside it. */
int64_t hpui_theme_set(int64_t win, const int64_t *v, int n) {
    if (n < 14) return -1;
    g_theme.bg        = rgbv(v[0]);
    g_theme.surface   = rgbv(v[1]);
    g_theme.elevated  = rgbv(v[2]);
    g_theme.text      = rgbv(v[3]);
    g_theme.muted     = rgbv(v[4]);
    g_theme.accent    = rgbv(v[5]);
    g_theme.on_accent = rgbv(v[6]);
    g_theme.border    = rgbv(v[7]);
    g_theme.radius    = (int)v[8];
    g_theme.font_pt   = (int)v[9] > 0 ? (int)v[9] : 10;
    g_theme.title_pt  = (int)v[10] > 0 ? (int)v[10] : 16;
    g_theme.row_h     = (int)v[11] > 0 ? (int)v[11] : 26;
    g_theme.pad       = (int)v[12] > 0 ? (int)v[12] : 12;
    g_theme.dark      = v[13] != 0;
    /* every control follows the theme, so clear stale explicit settings */
    for (size_t i = 0; i < tbl_len; i++) {
        if (tbl[i].kind == HPUI_WINDOW) continue;
        tbl[i].ov_bg = tbl[i].ov_fg = tbl[i].ov_accent = tbl[i].ov_border = -1;
        if (tbl[i].hwnd && IsWindow(tbl[i].hwnd))
            fonts_apply(tbl[i].hwnd);
    }
    if (g_theme.row_h > 0) {
        for (size_t i = 0; i < tbl_len; i++)
            if (tbl[i].kind == HPUI_LIST && tbl[i].hwnd)
                SendMessageW(tbl[i].hwnd, LB_SETITEMHEIGHT, 0, (LPARAM)sc(g_theme.row_h));
    }
    UiEntry *w = tbl_find(win);
    if (w && w->hwnd) apply_dark_decor(w->hwnd);
    repaint_window(win);
    return 0;
}

/* ================= menus ================= */
int64_t hpui_menu_new(int64_t win, const char *label) {
    UiEntry *w = tbl_find(win);
    if (!w || !w->hwnd) return -1;
    HMENU menubar = GetMenu(w->hwnd);
    if (!menubar) {
        menubar = CreateMenu();
        SetMenu(w->hwnd, menubar);
    }
    HMENU sub = CreatePopupMenu();
    wchar_t wt[256];
    MultiByteToWideChar(CP_UTF8, 0, label ? label : "", -1, wt, 256);
    int64_t id = next_id++;
    AppendMenuW(menubar, MF_POPUP, (UINT_PTR)sub, wt);
    tbl_add(id, HPUI_MENU, NULL);
    UiEntry *e = tbl_find(id);
    e->hwnd = w->hwnd;
    e->menu_id = sub;
    return id;
}

int64_t hpui_menu_item(int64_t menu, const char *label, int64_t cmdid) {
    UiEntry *m = tbl_find(menu);
    if (!m || !m->menu_id) return -1;
    wchar_t wt[256];
    MultiByteToWideChar(CP_UTF8, 0, label ? label : "", -1, wt, 256);
    int64_t id = next_id++;
    AppendMenuW(m->menu_id, MF_STRING, (UINT_PTR)cmdid, wt);
    tbl_add(id, HPUI_MENUITEM, NULL);
    UiEntry *e = tbl_find(id);
    e->hwnd = m->hwnd;
    e->menu_id = (HMENU)cmdid;
    return id;
}

int64_t hpui_menu_sep(int64_t menu) {
    UiEntry *m = tbl_find(menu);
    if (!m || !m->menu_id) return -1;
    AppendMenuW(m->menu_id, MF_SEPARATOR, 0, NULL);
    return 0;
}

/* ================= events ================= */
int64_t hpui_on_event(int64_t handle, int64_t evtype, hval cb) {
    UiEntry *e = tbl_find(handle);
    if (!e) return -1;
    switch (evtype) {
    case 1: e->cb_click = cb; break;
    case 2: e->cb_change = cb; break;
    case 3: e->cb_action = cb; break;
    case 4: e->cb_key = cb; break;
    case 5: e->cb_drag = cb; break;
    default:
        /* the canvas input events (6..15): mouse in logical pixels, wheel
         * notches, resize, typed characters and focus changes */
        if (evtype > 5 && evtype < HPUI_EV_NEV) { e->cb_ev[evtype] = cb; break; }
        return -1;
    }
    return 0;
}

/* The raw HWND of a window, so something that must be a real child window
 * (an embedded Chromium view) can be parented inside it. */
/* The window's client area in the same 96-dpi logical pixels every other
 * coordinate in this library uses, so a script can lay itself out against a
 * window the user has resized. */
int64_t hpui_client_size(int64_t win, int64_t *out_w, int64_t *out_h) {
    UiEntry *e = tbl_find(win);
    if (!e || !e->hwnd) return -1;
    RECT rc;
    GetClientRect(e->hwnd, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    int dpi = (int)hpui_dpi_scale();
    if (dpi <= 0) dpi = 96;
    if (out_w) *out_w = (w * 96 + dpi / 2) / dpi;
    if (out_h) *out_h = (h * 96 + dpi / 2) / dpi;
    return 0;
}

int64_t hpui_hwnd(int64_t win) {
    UiEntry *e = tbl_find(win);
    if (!e || !e->hwnd) return 0;
    return (int64_t)(intptr_t)e->hwnd;
}

int64_t hpui_repaint(int64_t h) {
    UiEntry *e = tbl_find(h);
    if (!e || !e->hwnd) return -1;
    hpui_dirty(e);
    return 0;
}

/* ================= timers ================= */
#define HPUI_TIMER_ID_BASE 1000

int64_t hpui_timer(int64_t ms, hval cb) {
    if (ms <= 0) return -1;
    UiEntry *w = NULL;
    for (size_t i = 0; i < tbl_len; i++)
        if (tbl[i].kind == HPUI_WINDOW && tbl[i].hwnd && IsWindow(tbl[i].hwnd)) { w = &tbl[i]; break; }
    if (!w) return -1;
    static int64_t tid = HPUI_TIMER_ID_BASE;
    int64_t id = tid++;
    tbl_add(id, HPUI_TIMER, NULL);
    UiEntry *e = tbl_find(id);
    e->cb_click = cb;
    SetTimer(w->hwnd, (UINT_PTR)id, (UINT)ms, NULL);
    return id;
}

/* ================= dialogs ================= */
int64_t hpui_msgbox(int64_t win, const char *title, const char *text, int type) {
    UiEntry *w = tbl_find(win);
    HWND owner = (w && w->hwnd) ? w->hwnd : NULL;
    wchar_t wt[256], tx[2048];
    MultiByteToWideChar(CP_UTF8, 0, title ? title : "", -1, wt, 256);
    MultiByteToWideChar(CP_UTF8, 0, text ? text : "", -1, tx, 2048);
    UINT flags;
    switch (type) {
    case 1:  flags = MB_ICONWARNING | MB_OK; break;
    case 2:  flags = MB_ICONERROR | MB_OK; break;
    case 3:  flags = MB_ICONQUESTION | MB_YESNO; break;
    default: flags = MB_ICONINFORMATION | MB_OK; break;
    }
    int r = MessageBoxW(owner, tx, wt, flags);
    if (type == 3) return (r == IDYES) ? 1 : 0;
    return (r == IDOK || r == IDYES) ? 1 : 0;
}

static bool pick_path(HWND owner, bool save, const char *filter, char *out, int outsz) {
    wchar_t fname[1024];
    fname[0] = 0;
    wchar_t wfilter[1024];
    const char *f = filter ? filter : "All files|*.*";
    size_t j = 0;
    for (size_t i = 0; f[i] && j < 1022; i++)
        wfilter[j++] = f[i] == '|' ? 0 : (wchar_t)(unsigned char)f[i];
    wfilter[j] = 0; wfilter[j + 1] = 0;
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = wfilter;
    ofn.lpstrFile = fname;
    ofn.nMaxFile = 1024;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    BOOL ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
    if (!ok) return false;
    WideCharToMultiByte(CP_UTF8, 0, fname, -1, out, outsz, NULL, NULL);
    return true;
}

hval hpui_open_file(int64_t win, const char *filter) {
    UiEntry *w = tbl_find(win);
    HWND owner = (w && w->hwnd) ? w->hwnd : NULL;
    char buf[1024];
    if (!pick_path(owner, false, filter, buf, sizeof buf)) return hp_null;
    return hp_of_str(hp_str_new(buf, strlen(buf)));
}

hval hpui_save_file(int64_t win, const char *filter) {
    UiEntry *w = tbl_find(win);
    HWND owner = (w && w->hwnd) ? w->hwnd : NULL;
    char buf[1024];
    if (!pick_path(owner, true, filter, buf, sizeof buf)) return hp_null;
    return hp_of_str(hp_str_new(buf, strlen(buf)));
}

hval hpui_pick_folder(int64_t win) {
    UiEntry *w = tbl_find(win);
    HWND owner = (w && w->hwnd) ? w->hwnd : NULL;
    wchar_t path[MAX_PATH];
    BROWSEINFOW bi;
    memset(&bi, 0, sizeof bi);
    bi.hwndOwner = owner;
    bi.lpszTitle = L"Choose a folder";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return hp_null;
    bool ok = SHGetPathFromIDListW(pidl, path);
    CoTaskMemFree(pidl);
    if (!ok) return hp_null;
    char u8[MAX_PATH * 4];
    WideCharToMultiByte(CP_UTF8, 0, path, -1, u8, sizeof u8, NULL, NULL);
    return hp_of_str(hp_str_new(u8, strlen(u8)));
}

hval hpui_color_pick(int64_t win, int64_t init_rgb) {
    UiEntry *w = tbl_find(win);
    HWND owner = (w && w->hwnd) ? w->hwnd : NULL;
    CHOOSECOLORW cc;
    static COLORREF custom[16];
    memset(&cc, 0, sizeof cc);
    cc.lStructSize = sizeof cc;
    cc.hwndOwner = owner;
    cc.rgbResult = RGB((init_rgb >> 16) & 0xff, (init_rgb >> 8) & 0xff, init_rgb & 0xff);
    cc.lpCustColors = custom;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&cc)) return hp_null;
    COLORREF c = cc.rgbResult;
    return hp_of_int(((int64_t)GetRValue(c) << 16) | ((int64_t)GetGValue(c) << 8) |
                     (int64_t)GetBValue(c));
}

/* ================= clipboard ================= */
int64_t ui_clip_set(const char *text) {
    if (!OpenClipboard(NULL)) return -1;
    EmptyClipboard();
    size_t n = strlen(text) + 1;
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n);
    if (!g) { CloseClipboard(); return -1; }
    memcpy(GlobalLock(g), text, n);
    GlobalUnlock(g);
    SetClipboardData(CF_TEXT, g);
    CloseClipboard();
    return 0;
}

hval ui_clip_get(void) {
    if (!OpenClipboard(NULL)) return hp_null;
    HANDLE h = GetClipboardData(CF_TEXT);
    if (!h) { CloseClipboard(); return hp_null; }
    const char *s = (const char *)GlobalLock(h);
    hval r = s ? hp_of_str(hp_str_new(s, strlen(s))) : hp_null;
    GlobalUnlock(h);
    CloseClipboard();
    return r;
}

/* ================= owner drawing ================= */
static void draw_check(HDC dc, int cx, int cy, int r, COLORREF color, int width) {
    HPEN pen = CreatePen(PS_SOLID, width, color);
    HPEN op = (HPEN)SelectObject(dc, pen);
    MoveToEx(dc, cx - r / 2, cy, NULL);
    LineTo(dc, cx - r / 6, cy + r / 2);
    LineTo(dc, cx + r / 2, cy - r / 2);
    SelectObject(dc, op);
    DeleteObject(pen);
}

static void draw_chevron(HDC dc, int cx, int cy, int r, COLORREF color, int width,
                         bool down) {
    HPEN pen = CreatePen(PS_SOLID, width, color);
    HPEN op = (HPEN)SelectObject(dc, pen);
    if (down) {
        MoveToEx(dc, cx - r, cy - r / 2, NULL);
        LineTo(dc, cx, cy + r / 2);
        LineTo(dc, cx + r, cy - r / 2);
    } else {
        MoveToEx(dc, cx - r, cy + r / 2, NULL);
        LineTo(dc, cx, cy - r / 2);
        LineTo(dc, cx + r, cy + r / 2);
    }
    SelectObject(dc, op);
    DeleteObject(pen);
}

static void draw_control_at(HWND hw, HDC dc, const RECT *where);

/* A control's rectangle in its parent's client coordinates. Done through the
 * screen rather than MapWindowPoints, whose "window coordinates" wording is a
 * reliable way to be off by the child's position exactly when it matters. */
static bool self_rect_in_parent(HWND hw, HWND parent, RECT *out) {
    RECT wr;
    if (!GetWindowRect(hw, &wr)) return false;
    POINT tl = {wr.left, wr.top}, br = {wr.right, wr.bottom};
    ScreenToClient(parent, &tl);
    ScreenToClient(parent, &br);
    out->left = tl.x;
    out->top = tl.y;
    out->right = br.x;
    out->bottom = br.y;
    return true;
}

/* ================= owner-drawn siblings share one surface =================
 *
 * An owner-drawn control has no bitmap of its own: its pixels are drawn into
 * the PARENT's DC while the parent handles WM_DRAWITEM. Windows therefore
 * keeps no copy of them, and repainting a control erases whatever was stacked
 * on top of it in the same rectangle unless those repaint too.
 *
 * That is why a tab's close button used to disappear every time the tab
 * refreshed its favicon: the tab is painted nine times, the cross three, and
 * the last word belongs to whoever painted last.
 *
 * tbl is in creation order, which is also z-order, so anything later in the
 * table is above anything earlier. After a control paints, everything above it
 * that overlaps is repainted immediately, on top. */

static void repaint_above(const UiEntry *self, const RECT *where) {
    size_t idx = 0;
    for (size_t i = 0; i < tbl_len; i++)
        if (&tbl[i] == self) { idx = i; break; }
    if (!self->hwnd) return;
    HWND parent = GetParent(self->hwnd);
    if (!parent) return;

    /* The rectangle this control just painted, in the parent's client space. */
    RECT mine;
    if (where) mine = *where;
    else if (!self_rect_in_parent(self->hwnd, parent, &mine)) return;

    for (size_t i = idx + 1; i < tbl_len; i++) {
        UiEntry *e = &tbl[i];
        if (!e->hwnd || !IsWindow(e->hwnd) || !IsWindowVisible(e->hwnd)) continue;
        if (GetParent(e->hwnd) != parent) continue;
        if (e->kind != HPUI_BUTTON && e->kind != HPUI_LABEL) continue;
        RECT other, hit;
        if (!self_rect_in_parent(e->hwnd, parent, &other)) continue;
        if (!IntersectRect(&hit, &mine, &other)) continue;
        /* Paint straight into the parent: going through WM_DRAWITEM from here
         * would recurse into the dispatcher we are already inside. */
        HDC dc = GetDC(parent);
        if (dc) {
            draw_control_at(e->hwnd, dc, &other);
            ReleaseDC(parent, dc);
        }
    }
}

static void draw_control_at(HWND hw, HDC dc, const RECT *where) {
    UiEntry *e = entry_of(hw);
    if (!e) return;
    UiPaint p;
    resolve_paint(e, &p);
    RECT rc;
    /* rc stays in the DC's own coordinates - the parent's client area when a
     * rectangle was supplied, the control's own client area when it was not -
     * and the viewport is left alone.
     *
     * Shifting the viewport and rebasing rc at the same time is a double
     * offset, and shifting one without the other is a single offset. Both
     * were wrong, in opposite directions: with the rectangle parent-relative
     * and the viewport moved, every fill, rounded rect and string went to the
     * DC's origin and the whole chrome piled into the window's top-left
     * corner; with the rectangle rebased, the same drawing went off the top
     * left of the control and clipped away to nothing. Keeping the rectangle
     * and the DC in one coordinate system is the only version that needs no
     * explanation - and it leaves the DC exactly as the caller found it, so
     * repainting an overlapping sibling cannot leak a shift into the next
     * control that shares the DC. */
    if (where) rc = *where;
    else GetClientRect(hw, &rc);
    /* The DC that arrives with WM_DRAWITEM is the PARENT's, and Windows has
       already clipped it to the item rectangle it believes in - which, as the
       comment at that handler says, is not always the control's real place.
       A wrong clip region does not move the drawing, it deletes it: GDI drops
       every operation outside the clip, so a control whose rect was computed
       correctly still painted nothing at all. That is why the whole toolbar
       (back, forward, reload, home, print) was blank while the tab's close
       cross - whose rcItem happened to be right - drew every time.
       Pinning the clip to the rectangle this function just worked out makes
       the paint independent of what the item structure claimed. */
    int clip_saved = -1;
    HRGN clip_rgn = NULL;
    if (where) {
        clip_saved = SaveDC(dc);
        if (clip_saved != -1) {
            clip_rgn = CreateRectRgn(rc.left, rc.top, rc.right, rc.bottom);
            if (clip_rgn) SelectClipRgn(dc, clip_rgn);
            else clip_saved = -1;
        }
    }
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (hpui_win_debug()) {
        RECT wr;
        GetWindowRect(hw, &wr);
        fprintf(stderr,
                "[ui] paint id=%lld kind=%d rect=%d,%d %dx%d role=%d "
                "win=%d,%d %dx%d where=%s\n",
                (long long)e->id, (int)e->kind, rc.left, rc.top, w, h,
                (int)e->role,
                wr.left, wr.top, wr.right - wr.left,
                wr.bottom - wr.top, where ? "given" : "own");
    }
    int font_pt = e->ov_font_pt > 0 ? e->ov_font_pt : g_theme.font_pt;
    /* Owner-drawn and custom controls are never erased for us, so clear the
     * frame first. Without this a slider leaves its old thumb behind on every
     * repaint -- the track proc claims WM_ERASEBKGND and never paints one, so
     * every drag draws another thumb on top of the last. Labels and inputs are
     * exempt: a label sits on whatever its container drew, and an input erases
     * its own rounded field in WM_ERASEBKGND. */
    if (e->kind != HPUI_LABEL && e->kind != HPUI_INPUT)
        fill_rect(dc, rc, p.bg);

    switch (e->kind) {
    case HPUI_LABEL: {
        if ((e->pic && *e->pic) || (e->svg && *e->svg)) {
            draw_icon_slot(dc, &rc, e, p.fg, p.bg);
            break;
        }
        wchar_t buf[1024];
        GetWindowTextW(hw, buf, 1024);
        RECT tr = rc;
        /* Multi-line labels word-wrap instead of being ellipsised. The layout
         * engine sizes them with ui_text_lines(), so a long sentence keeps all
         * of its words rather than losing the tail to "...". Short labels are
         * unaffected: they still get DT_SINGLELINE vertical centring. */
        int oneLine = sc(18);
        bool wraps = h > oneLine + sc(6);
        UINT fl = DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX;
        if (wraps) { fl = DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS; }
        else { fl |= DT_VCENTER | DT_SINGLELINE; }
        draw_str(dc, tr, buf, p.fg, entry_font(e, false), fl);
        break;
    }
    case HPUI_BUTTON: {
        fill_round(dc, rc, p.radius, p.bg, p.border, 1);
        wchar_t buf[512];
        GetWindowTextW(hw, buf, 512);
        bool has_icon = (e->pic && *e->pic) || (e->svg && *e->svg);
        if (has_icon && !buf[0]) {
            /* Icon-only button: the artwork is centred in the whole button. */
            int side = w - sc(10), sh = h - sc(10);
            if (sh < side) side = sh;
            if (side < sc(10)) side = sc(10);
            RECT ir = { rc.left + (w - side) / 2, rc.top + (h - side) / 2,
                        rc.left + (w + side) / 2, rc.top + (h + side) / 2 };
            draw_icon_slot(dc, &ir, e, p.fg, p.bg);
            break;
        }
        RECT tr = rc;
        if (has_icon) {
            int side = sc(18);
            if (h - side < sc(8)) side = h - sc(8);
            if (side < sc(8)) side = sc(8);
            RECT ir = { rc.left + sc(8), rc.top + (h - side) / 2,
                        rc.left + sc(8) + side, rc.top + (h + side) / 2 };
            draw_icon_slot(dc, &ir, e, p.fg, p.bg);
            tr.left = ir.right + sc(7);
        }
        /* inset BOTH edges. Centring inside a box that is only inset on the
         * left pushes the label right by half the inset, which reads as less
         * padding on the right than on the left. */
        if (e->role != UI_ROLE_LINK) {
            int in = sc(g_theme.pad);
            tr.left += in;
            tr.right -= in;
            if (tr.right < tr.left) tr.right = tr.left;
        }
        draw_str(dc, tr, buf, p.fg, entry_font(e, p.bold),
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        break;
    }
    case HPUI_CHECKBOX: {
        wchar_t buf[512];
        GetWindowTextW(hw, buf, 512);
        int box = sc(18);
        int cy = h / 2;
        RECT br = { rc.left, cy - box / 2, rc.left + box, cy + box / 2 };
        COLORREF fill = e->checked_flag ? p.accent : g_theme.elevated;
        COLORREF line = e->checked_flag ? p.accent : (e->hovered ? g_theme.muted : g_theme.border);
        fill_round(dc, br, sc(5), fill, line, 1);
        if (e->checked_flag)
            draw_check(dc, (br.left + br.right) / 2, cy, sc(9),
                       g_theme.on_accent, sc(2));
        RECT tr = { rc.left + box + sc(10), rc.top, rc.right, rc.bottom };
        draw_str(dc, tr, buf, p.fg, entry_font(e, false),
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        break;
    }
    case HPUI_PANEL: {
        RECT r2 = rc;
        fill_round(dc, r2, p.radius, p.bg, p.border, 1);
        break;
    }
    case HPUI_GROUP: {
        wchar_t buf[512];
        GetWindowTextW(hw, buf, 512);
        int txt_w = 0;
        if (buf[0]) {
            HFONT f = entry_font(e, true);
            HDC mem = CreateCompatibleDC(dc);
            HFONT of = (HFONT)SelectObject(mem, f);
            RECT t = { 0, 0, 0, 0 };
            DrawTextW(mem, buf, -1, &t, DT_CALCRECT | DT_SINGLELINE);
            txt_w = t.right - t.left;
            SelectObject(mem, of);
            DeleteDC(mem);
        }
        int cap_y = sc(10);
        RECT line = { rc.left, cap_y, rc.right, cap_y + 1 };
        /* outline with a gap for the caption */
        fill_round(dc, rc, p.radius, (COLORREF)-1, p.border, 1);
        if (txt_w > 0) {
            RECT gap = { rc.left + sc(12), cap_y - 1, rc.left + sc(12) + txt_w + sc(6), cap_y + 2 };
            fill_rect(dc, gap, g_theme.bg);
            RECT tr = { rc.left + sc(12), rc.top, rc.left + sc(18) + txt_w, rc.bottom };
            draw_str(dc, tr, buf, p.fg, entry_font(e, true),
                     DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        (void)line;
        break;
    }
    case HPUI_DIVIDER: {
        int y = h / 2;
        RECT line = { rc.left, y, rc.right, y + 1 };
        fill_rect(dc, line, p.border);
        break;
    }
    case HPUI_LIST: {
        int row = sc(g_theme.row_h);
        if (row <= 0) row = 1;
        int sel = (int)SendMessageW(hw, LB_GETCURSEL, 0, 0);
        int top = (int)SendMessageW(hw, LB_GETTOPINDEX, 0, 0);
        if (sel == LB_ERR) sel = -1;
        fill_rect(dc, rc, p.bg);
        int visible = (h + row - 1) / row;
        for (int i = 0; i < visible; i++) {
            int idx = top + i;
            if (idx < 0 || idx >= e->n_items) continue;
            const wchar_t *buf = e->items[idx];
            RECT ir = { rc.left + sc(2), i * row, rc.right - sc(2), (i + 1) * row };
            if (idx == sel)      fill_round(dc, ir, sc(6), tint(p.accent), (COLORREF)-1, 0);
            else if (idx == e->hover_index)
                fill_round(dc, ir, sc(6),
                                                g_theme.dark ? lighten(g_theme.surface, 6)
                                                             : darken(g_theme.surface, 5),
                                                (COLORREF)-1, 0);
            RECT tr = ir;
            tr.left += sc(8);
            draw_str(dc, tr, buf, idx == sel ? g_theme.text : p.fg,
                     entry_font(e, false), DT_LEFT | DT_VCENTER | DT_SINGLELINE |
                     DT_END_ELLIPSIS);
        }
        if (e->focused) {
            RECT fr = { 0, 0, w - 1, h - 1 };
            fill_round(dc, fr, p.radius, (COLORREF)-1, p.accent, 1);
        }
        break;
    }
    case HPUI_PROGRESS: {
        int bar = sc(8);
        if (bar > h) bar = h;
        int y = (h - bar) / 2;
        RECT tr = { 0, y, w, y + bar };
        fill_round(dc, tr, bar / 2, g_theme.dark ? lighten(g_theme.elevated, 4) : g_theme.border,
                   (COLORREF)-1, 0);
        int fw = (int)((int64_t)w * e->value / 100);
        if (fw > 0) {
            RECT fr = { 0, y, fw, y + bar };
            fill_round(dc, fr, bar / 2, p.accent, (COLORREF)-1, 0);
        }
        break;
    }
    case HPUI_SLIDER: {
        int bar = sc(6);
        int cy = h / 2;
        RECT tr = { 0, cy - bar / 2, w, cy + bar / 2 };
        fill_round(dc, tr, bar / 2, g_theme.dark ? lighten(g_theme.elevated, 6) : g_theme.border,
                   (COLORREF)-1, 0);
        int thumb = sc(18);
        int range = w - thumb;
        int span = e->max_v - e->min_v;
        int pos = 0;
        if (span > 0) pos = range - (int)(((int64_t)(e->max_v - e->value) * range) / span);
        if (pos < 0) pos = 0;
        if (pos > range) pos = range;
        if (range > 0) {
            RECT fr = { 0, cy - bar / 2, pos + thumb / 2, cy + bar / 2 };
            fill_round(dc, fr, bar / 2, p.accent, (COLORREF)-1, 0);
        }
        RECT cr = { pos, cy - thumb / 2, pos + thumb, cy + thumb / 2 };
        COLORREF cfill = g_theme.dark ? lighten(g_theme.text, 4) : RGB(0xFF, 0xFF, 0xFF);
        if (e->focused) {
            RECT ring = { cr.left - sc(3), cr.top - sc(3), cr.right + sc(3), cr.bottom + sc(3) };
            fill_round(dc, ring, sc(12), tint(p.accent), (COLORREF)-1, 0);
        }
        fill_round(dc, cr, sc(9), cfill, (COLORREF)-1, 0);
        break;
    }
    case HPUI_COMBO: {
        fill_round(dc, rc, p.radius, p.bg, p.border, e->focused ? 2 : 1);
        wchar_t buf[1024];
        buf[0] = 0;
        if (e->sel >= 0 && e->sel < e->n_items) wcsncpy(buf, e->items[e->sel], 1023);
        buf[1023] = 0;
        RECT tr = { rc.left + sc(12), rc.top, rc.right - sc(28), rc.bottom };
        draw_str(dc, tr, buf, e->sel >= 0 ? p.fg : g_theme.muted, entry_font(e, false),
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        int cx = rc.right - sc(16), cy = rc.top + h / 2;
        draw_chevron(dc, cx, cy, sc(4), g_theme.muted, sc(2), true);
        break;
    }
    default:
        break;
    }
    if (clip_rgn) DeleteObject(clip_rgn);
    if (clip_saved != -1) RestoreDC(dc, clip_saved);
    (void)font_pt;
}

/* ================= combo dropdown popup ================= */
static HWND       g_popup = NULL;
static int64_t    g_popup_owner = 0;

static void popup_close(void) {
    if (!g_popup) return;
    HWND p = g_popup;
    g_popup = NULL;
    g_popup_owner = 0;
    DestroyWindow(p);
}

static void combo_choose(int idx) {
    UiEntry *e = tbl_find(g_popup_owner);
    if (!e) return;
    if (idx >= 0 && idx < e->n_items && idx != e->sel) {
        e->sel = idx;
        hpui_dirty(e);
        fire(e, &e->cb_change, e->id, idx);
    }
}

static void combo_open(HWND combo) {
    UiEntry *e = entry_of(combo);
    if (!e || e->n_items == 0) return;
    popup_close();
    g_popup_owner = e->id;
    RECT wr;
    GetWindowRect(combo, &wr);
    int row = sc(g_theme.row_h);
    int pad = sc(6);
    int hgt = e->n_items * row + pad * 2;
    int maxh = GetSystemMetrics(SM_CYSCREEN) / 2;
    if (hgt > maxh) hgt = maxh;
    int x = wr.left, y = wr.bottom + sc(4);
    if (y + hgt > GetSystemMetrics(SM_CYSCREEN)) {
        y = wr.top - hgt - sc(4);
        if (y < 0) y = 0;
    }
    HWND owner = GetParent(combo);
    g_popup = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"HolyPHPPopList", L"",
                              WS_POPUP, x, y, wr.right - wr.left, hgt,
                              owner, NULL, GetModuleHandleW(NULL), NULL);
    if (!g_popup) { g_popup = NULL; g_popup_owner = 0; return; }
    int r = sc(g_theme.radius);
    HRGN reg = CreateRoundRectRgn(0, 0, (wr.right - wr.left) + 1, hgt + 1, r * 2, r * 2);
    SetWindowRgn(g_popup, reg, TRUE);
    if (p_SetWindowTheme) p_SetWindowTheme(g_popup, L"", L"");
    ShowWindow(g_popup, SW_SHOWNA);
    SetCapture(g_popup);
    InvalidateRect(g_popup, NULL, FALSE);
}

static LRESULT CALLBACK poplist_wndproc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    UiEntry *e = tbl_find(g_popup_owner);
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hw, &ps);
        RECT rc;
        GetClientRect(hw, &rc);
        fill_round(dc, rc, sc(g_theme.radius), g_theme.elevated, g_theme.border, 1);
        if (e) {
            int row = sc(g_theme.row_h);
            int pad = sc(6);
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hw, &pt);
            for (int i = 0; i < e->n_items; i++) {
                RECT ir = { sc(4), pad + i * row, rc.right - sc(4), pad + (i + 1) * row };
                bool over = pt.y >= ir.top && pt.y < ir.bottom;
                if (i == e->sel)      fill_round(dc, ir, sc(6), tint(g_theme.accent), (COLORREF)-1, 0);
                else if (over)        fill_round(dc, ir, sc(6),
                                                g_theme.dark ? lighten(g_theme.elevated, 6)
                                                             : darken(g_theme.elevated, 6),
                                                (COLORREF)-1, 0);
                RECT tr = ir;
                tr.left += sc(8);
                draw_str(dc, tr, e->items[i], g_theme.text, ui_font(g_theme.font_pt, false),
                         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
        }
        EndPaint(hw, &ps);
        return 0;
    }
    case WM_MOUSEMOVE:
        InvalidateRect(hw, NULL, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        int row = sc(g_theme.row_h), pad = sc(6);
        int idx = (GET_Y_LPARAM(lp) - pad) / row;
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT rc;
        GetClientRect(hw, &rc);
        bool inside = pt.x >= 0 && pt.x < rc.right && pt.y >= 0 && pt.y < rc.bottom;
        if (inside && idx >= 0 && e && idx < e->n_items) combo_choose(idx);
        if (inside) { if (e) SetFocus(e->hwnd); }
        ReleaseCapture();
        popup_close();
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { ReleaseCapture(); popup_close(); return 0; }
        break;
    case WM_CAPTURECHANGED:
        popup_close();
        return 0;
    case WM_KILLFOCUS:
        popup_close();
        return 0;
    default: break;
    }
    return DefWindowProcW(hw, msg, wp, lp);
}

/* ================= custom slider ================= */
static void track_set_from_x(UiEntry *e, int x) {
    RECT rc;
    GetClientRect(e->hwnd, &rc);
    int thumb = sc(18);
    int range = rc.right - thumb;
    if (range <= 0) return;
    int span = e->max_v - e->min_v;
    if (span <= 0) return;
    int v = (int)(((int64_t)x * span) / range) + e->min_v;
    if (v < e->min_v) v = e->min_v;
    if (v > e->max_v) v = e->max_v;
    if (v == e->value) return;
    e->value = v;
    hpui_dirty_soft(e);
    fire(e, &e->cb_change, e->id, v);
}

static LRESULT CALLBACK track_wndproc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    UiEntry *e = entry_of(hw);
    if (!e) return DefWindowProcW(hw, msg, wp, lp);
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hw, &ps);
        draw_control_at(hw, dc, NULL);
        EndPaint(hw, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
        SetFocus(hw);
        SetCapture(hw);
        e->pressed = true;
        track_set_from_x(e, GET_X_LPARAM(lp));
        return 0;
    case WM_LBUTTONUP:
        if (e->pressed) { e->pressed = false; ReleaseCapture(); }
        return 0;
    case WM_CAPTURECHANGED:
        e->pressed = false;
        return 0;
    case WM_MOUSEMOVE:
        if (e->pressed) track_set_from_x(e, GET_X_LPARAM(lp));
        return 0;
    case WM_KEYDOWN: {
        int step = 1, v = e->value;
        RECT rc;
        GetClientRect(hw, &rc);
        int page = (rc.right - sc(18)) / 10;
        if (page < 1) page = 1;
        if (wp == VK_LEFT)  v -= step;
        else if (wp == VK_RIGHT) v += step;
        else if (wp == VK_UP)    v += step;
        else if (wp == VK_DOWN)  v -= step;
        else if (wp == VK_PRIOR)   v += page;
        else if (wp == VK_NEXT)    v -= page;
        else if (wp == VK_HOME) v = e->min_v;
        else if (wp == VK_END)  v = e->max_v;
        else return DefWindowProcW(hw, msg, wp, lp);
        if (v < e->min_v) v = e->min_v;
        if (v > e->max_v) v = e->max_v;
        if (v != e->value) {
            e->value = v;
            InvalidateRect(hw, NULL, FALSE);
            fire(e, &e->cb_change, e->id, v);
        }
        return 0;
    }
    case WM_SETFOCUS:  e->focused = true;  InvalidateRect(hw, NULL, FALSE); return 0;
    case WM_KILLFOCUS: e->focused = false; InvalidateRect(hw, NULL, FALSE); return 0;
    default: break;
    }
    return DefWindowProcW(hw, msg, wp, lp);
}

/* ================= custom progress bar ================= */
static LRESULT CALLBACK bar_wndproc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hw, &ps);
        draw_control_at(hw, dc, NULL);
        EndPaint(hw, &ps);
        return 0;
    }
    return DefWindowProcW(hw, msg, wp, lp);
}

/* ================= custom combo box ================= */
static LRESULT CALLBACK combo_wndproc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    UiEntry *e = entry_of(hw);
    if (!e) return DefWindowProcW(hw, msg, wp, lp);
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hw, &ps);
        draw_control_at(hw, dc, NULL);
        EndPaint(hw, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
        SetFocus(hw);
        combo_open(hw);
        return 0;
    case WM_KEYDOWN: {
        if (wp == VK_ESCAPE && g_popup) { ReleaseCapture(); popup_close(); return 0; }
        if (wp == VK_DOWN || wp == VK_UP || wp == VK_SPACE || wp == VK_RETURN) {
            combo_open(hw);
            return 0;
        }
        if (e->n_items == 0) return 0;
        int s = e->sel;
        if (wp == VK_DOWN) s += 1;
        else if (wp == VK_UP) s -= 1;
        else return DefWindowProcW(hw, msg, wp, lp);
        if (s < 0) s = e->n_items - 1;
        if (s >= e->n_items) s = 0;
        if (s != e->sel) {
            e->sel = s;
            InvalidateRect(hw, NULL, TRUE);
            fire(e, &e->cb_change, e->id, s);
        }
        return 0;
    }
    case WM_SETFOCUS:  e->focused = true;  InvalidateRect(hw, NULL, TRUE); return 0;
    case WM_KILLFOCUS: e->focused = false; InvalidateRect(hw, NULL, TRUE); return 0;
    case WM_DESTROY:   if (g_popup_owner == e->id) popup_close(); break;
    default: break;
    }
    return DefWindowProcW(hw, msg, wp, lp);
}

/* ================= window proc: routes events ================= */
/* What does the OS think owns the mouse? Reported from inside the process,
 * because GetCapture() is thread-local and reading it from a test harness
 * answers a different thread's question. */
static const char *hpui_capture_name(void) {
    static char buf[96];
    HWND c = GetCapture();
    if (!c) { return "none"; }
    wchar_t cls[64];
    GetClassNameW(c, cls, 64);
    WideCharToMultiByte(CP_UTF8, 0, cls, -1, buf, sizeof buf, NULL, NULL);
    return buf;
}

static LRESULT CALLBACK ui_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    int64_t id = (int64_t)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    UiEntry *e = id ? tbl_find(id) : NULL;

    switch (msg) {
    case HPUI_WM_WAKE:
        return 0;

    /* Window-level shortcuts. Escape closes; anything the script binds
     * through $app->onKey() is delivered as (vk, character). */
    case WM_KEYDOWN: {
        UiEntry *we = entry_of(hwnd);
        int vk = (int)wp;
        /* Tab / Shift+Tab move focus between the controls, unless the script
         * asked for that combination itself. */
        if (vk == VK_TAB && !(GetKeyState(VK_CONTROL) & 0x8000) &&
            !(we && we->cb_key.tag == HV_CLO && we->cb_key.u.c)) {
            focus_step(hwnd, (GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1);
            return 0;
        }
        if (vk == VK_ESCAPE && we && we->kind == HPUI_WINDOW)
            DestroyWindow(hwnd);
        /* PageUp/Down and Ctrl+Home/End scroll a window whose content does not
         * fit. The arrow keys are deliberately left alone: a script that binds
         * them through onKey() (the gallery cycles themes with Left/Right)
         * must keep getting them. */
        if (we && we->kind == HPUI_WINDOW) {
            RECT rc; GetClientRect(hwnd, &rc);
            int page = rc.bottom - rc.top - sc(40);
            if (page < 1) page = 1;
            if (vk == VK_PRIOR) scroll_by(we, -page);
            else if (vk == VK_NEXT) scroll_by(we, page);
            else if (vk == VK_HOME && GetKeyState(VK_CONTROL) < 0) scroll_by(we, -we->content_h);
            else if (vk == VK_END && GetKeyState(VK_CONTROL) < 0) scroll_by(we, we->content_h);
        }
        if (we && we->cb_key.tag == HV_CLO && we->cb_key.u.c) {
            /* WPARAM is the *virtual key*, not the character. Casting it to
             * wchar gave '[' as VK_OEM_4 (0xDB), so every arrow-key and
             * punctuation binding in a script silently never fired.
             * Translate through the keyboard layout instead. */
            BYTE state[256];
            GetKeyboardState(state);
            wchar_t buf[8];
            int n = ToUnicode(vk, (int)(lp >> 16), state, buf, 8, 0);
            int64_t ch = (n > 0) ? (int64_t)buf[0] : 0;
            if (ch == VK_RETURN) ch = '\r';
            if (ch == VK_BACK)   ch = '\b';
            /* The modifiers come along for the same reason the input's key
               callback passes them: a window-level shortcut that cannot see
               Ctrl from T is not a shortcut, it is a letter. Same bit
               meanings as everywhere else - 1 Ctrl, 2 Shift, 4 Alt. */
            int mods = 0;
            if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= 1;
            if (GetAsyncKeyState(VK_SHIFT) & 0x8000) mods |= 2;
            if (GetAsyncKeyState(VK_MENU) & 0x8000) mods |= 4;
            call_cb3(we->cb_key, vk, ch, mods);
        }
        if (!we || we->cb_key.tag != HV_CLO || !we->cb_key.u.c)
            return DefWindowProcW(hwnd, msg, wp, lp);
        return 0;
    }

    /* Wheel scrolling: content taller than the window used to be unreachable. */
    case WM_MOUSEWHEEL: {
        UiEntry *we = entry_of(hwnd);
        if (!we || we->kind != HPUI_WINDOW) break;
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        int lines = 3;
        SystemParametersInfoA(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        if (lines <= 0) lines = 3;
        int step = (abs(delta) / WHEEL_DELTA) * lines * sc(g_theme.row_h);
        scroll_by(we, delta > 0 ? step : -step);
        return 0;
    }

    /* Input at the window itself, not its children: the first place a mouse
     * message can appear. HPHP_UI_DEBUG=1 only; WM_MOUSEMOVE and WM_TIMER are
     * deliberately left out or the log drowns in them. */
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN:
    case WM_RBUTTONUP: case WM_CAPTURECHANGED:
        if (hpui_win_debug())
            fprintf(stderr, "[ui] WIN msg=0x%04X lparam=0x%llX capture=%s focus=%p\n",
                    msg, (unsigned long long)lp, hpui_capture_name(),
                    (void *)GetFocus());
        break;

    case WM_SIZE: {
        UiEntry *we = entry_of(hwnd);
        if (we && we->kind == HPUI_WINDOW) scroll_place(we);
        break;
    }

    case WM_COMMAND: {
        int code = HIWORD(wp);
        HWND src = (HWND)lp;
        int64_t sid = src ? (int64_t)GetWindowLongPtrW(src, GWLP_USERDATA) : 0;
        UiEntry *se = sid ? tbl_find(sid) : NULL;
        if (hpui_win_debug())
            fprintf(stderr, "[ui] WM_COMMAND code=%d src=%p sid=%lld kind=%d\n",
                    code, (void *)src, (long long)sid, se ? (int)se->kind : -1);
        if (se) {
            if (code == BN_CLICKED && se->kind == HPUI_BUTTON) {
                fire(se, &se->cb_click, sid, 0);
                return 0;
            }
            if ((code == EN_CHANGE && se->kind == HPUI_INPUT) ||
                (code == LBN_SELCHANGE && se->kind == HPUI_LIST)) {
                if (se->kind == HPUI_LIST) {
                    LRESULT r = SendMessageW(se->hwnd, LB_GETCURSEL, 0, 0);
                    se->sel = (r == LB_ERR) ? -1 : (int)r;
                }
                fire(se, &se->cb_change, sid, 0);
                return 0;
            }
        }
        for (size_t i = 0; i < tbl_len; i++)
            if (tbl[i].kind == HPUI_MENUITEM && tbl[i].menu_id == (HMENU)wp) {
                fire(&tbl[i], &tbl[i].cb_click, tbl[i].id, (int64_t)wp);
                return 0;
            }
        /* A control of ours sent a notification we have no handler for
           (BN_SETFOCUS, BN_DBLCLK, ...). DefWindowProc is not neutral here:
           it ends up in the default button handling, which rings the system
           error bell. Swallow it instead. */
        if (se) return 0;
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    /* DefWindowProc answers an unhandled WM_MENUCHAR by calling
       MessageBeep(MB_ICONHAND) - the "Windows Error" sound. There is no menu
       here at all, so every keystroke that produces a menu character is a
       guaranteed error beep. Claim the message and stay quiet. */
    case WM_MENUCHAR:
        return 0;

    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *di = (DRAWITEMSTRUCT *)lp;
        if (di->CtlType == ODT_BUTTON || di->CtlType == ODT_STATIC) {
            /* rcItem is the control's rectangle in the parent's client space -
             * in theory. In practice it comes back as {0,0,w,h} for some paints
             * and the real position for others, with the size right both times,
             * and a control drawn at the origin landed at the top-left corner
             * of the window. That is how a tab's cross ended up drawn in the
             * corner instead of inside the tab.
             *
             * The window knows where it is, so ask it. rcItem stays only as a
             * fallback for the one case where the query itself fails. */
            RECT at;
            const RECT *where = self_rect_in_parent(di->hwndItem, hwnd, &at)
                                    ? &at
                                    : &di->rcItem;
            draw_control_at(di->hwndItem, di->hDC, where);
            /* whoever is stacked on top of this control has just been erased */
            UiEntry *below = entry_of(di->hwndItem);
            if (below) repaint_above(below, where);
            return TRUE;
        }
        if (di->CtlType == ODT_LISTBOX) {
            draw_list_item(di);
            return TRUE;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    case WM_TIMER:
        if (wp >= HPUI_TIMER_ID_BASE) {
            UiEntry *t = tbl_find((int64_t)wp);
            if (t) fire(t, &t->cb_click, (int64_t)wp, 0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
        HWND src = (HWND)lp;
        UiEntry *se = src ? entry_of(src) : NULL;
        UiPaint p;
        if (se) resolve_paint(se, &p);
        else { p.fg = g_theme.text; p.bg = g_theme.bg; p.border = g_theme.border; p.radius = 0; }
        SetTextColor((HDC)wp, p.fg);
        /* An edit must own its background. With a NULL_BRUSH it never clears
         * the pixels it stops covering -- scrolling the caret or shrinking a
         * selection smears the old glyphs into the new text. The rounded field
         * is kept by clipping the control to a rounded region (input_region),
         * so an opaque brush no longer squares off the corners. */
        if (se && se->kind == HPUI_INPUT) {
            SetBkColor((HDC)wp, p.bg);
            SetBkMode((HDC)wp, OPAQUE);
            return (LRESULT)brush_for(p.bg);
        }
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetStockObject(NULL_BRUSH);
    }
    case WM_CTLCOLORLISTBOX: {
        UiPaint p;
        p.bg = g_theme.surface; p.fg = g_theme.text;
        SetBkColor((HDC)wp, p.bg);
        SetTextColor((HDC)wp, p.fg);
        static HBRUSH br = NULL;
        static COLORREF br_c = 0;
        if (br && br_c != p.bg) { DeleteObject(br); br = NULL; }
        if (!br) { br = CreateSolidBrush(p.bg); br_c = p.bg; }
        return (LRESULT)br;
    }
    case WM_CTLCOLORBTN: {
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetStockObject(NULL_BRUSH);
    }

    /* Erase ONLY the region that is actually invalid.
     *
     * The HDC handed to WM_ERASEBKGND is the plain window DC: it is not
     * clipped to the update region the way a BeginPaint DC would be. Filling
     * GetClientRect() with the background therefore painted over the whole
     * window every time anything at all asked for a repaint, and because an
     * owner-drawn button sends its WM_DRAWITEM from its OWN WM_PAINT, a
     * button with an empty update region was simply never redrawn after that.
     *
     * That is what made the entire toolbar vanish while the tab beside it
     * stayed: a 120ms layout timer invalidated the window, the erase wiped
     * every icon, and only the controls render() restyles each tick came back.
     * The tab strip was never broken; it was simply the only thing still
     * asking to be painted.
     */
    /* Erase the invalid region MINUS every visible child.
     *
     * An owner-drawn control does not own its pixels. Its WM_DRAWITEM is sent
     * from its OWN WM_PAINT, so it only redraws when its own update region is
     * non-empty - and it stops redrawing the moment something paints over its
     * box. This window used to erase the whole client rect here, because the
     * HDC given to WM_ERASEBKGND is the plain window DC and is not clipped to
     * the update region the way a BeginPaint DC would be.
     *
     * Any repaint anywhere therefore wiped the toolbar, and only the controls
     * that something had just invalidated came back. That is the whole reason
     * the toolbar looked empty while the tab beside it was fine: a 120ms
     * layout timer asked for a repaint, the erase cleared every icon, and
     * render() only ever restyles the tab strip. Nothing was wrong with the
     * icons, their colours, or the SVG parser - they were painted, then
     * deleted a few milliseconds later, and nobody asked for them again.
     *
     * Subtracting the children keeps the background correct everywhere it is
     * actually visible while leaving every owner-drawn control to paint itself.
     */
    case WM_ERASEBKGND: {
        RECT upd;
        if (!GetUpdateRect(hwnd, &upd, FALSE)) return 1;
        RECT cl;
        GetClientRect(hwnd, &cl);
        RECT want = { upd.left   < cl.left   ? cl.left   : upd.left,
                      upd.top    < cl.top    ? cl.top    : upd.top,
                      upd.right  > cl.right  ? cl.right  : upd.right,
                      upd.bottom > cl.bottom ? cl.bottom : upd.bottom };
        if (want.right <= want.left || want.bottom <= want.top) return 1;
        HRGN keep = CreateRectRgn(want.left, want.top, want.right, want.bottom);
        if (!keep) return 1;
        for (size_t i = 0; i < tbl_len; i++) {
            UiEntry *k = &tbl[i];
            if (!k->hwnd || !IsWindow(k->hwnd) || !IsWindowVisible(k->hwnd)) continue;
            if (GetParent(k->hwnd) != hwnd) continue;
            RECT kr;
            if (!self_rect_in_parent(k->hwnd, hwnd, &kr)) continue;
            HRGN cut = CreateRectRgn(kr.left, kr.top, kr.right, kr.bottom);
            if (cut) { CombineRgn(keep, keep, cut, RGN_DIFF); DeleteObject(cut); }
        }
        if (hpui_win_debug())
            fprintf(stderr, "[ui] parent erase %d,%d %dx%d\n",
                    want.left, want.top, want.right - want.left,
                    want.bottom - want.top);
        fill_region((HDC)wp, keep, g_theme.bg);
        DeleteObject(keep);
        return 1;
    }

    case WM_DPICHANGED: {
        int nd = LOWORD(lp);
        if (!g_dpi_aware) return DefWindowProcW(hwnd, msg, wp, lp);
        if (nd >= 72 && nd <= 480 && nd != g_dpi) {
            g_dpi = nd;
            fonts_drop();
            for (size_t i = 0; i < tbl_len; i++)
                if (tbl[i].hwnd && IsWindow(tbl[i].hwnd)) fonts_apply(tbl[i].hwnd);
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) popup_close();
        return DefWindowProcW(hwnd, msg, wp, lp);

    case WM_CLOSE:
        /* A tab being closed arrives here too: a browser window that is a
         * child of this window asks Chromium to close it by sending WM_CLOSE
         * to its OWNER, which is us. Treating that as "the user closed the
         * app" would take every tab down with it. */
        if (hpwv2_closing() > 0) {
            if (hpui_win_debug())
                fprintf(stderr, "[ui] WM_CLOSE on hwnd=%p id=%lld swallowed, "
                        "%d browser(s) still closing\n",
                        (void *)hwnd, (long long)id, hpwv2_closing());
            return 0;
        }
        popup_close();
        if (hpui_win_debug())
            fprintf(stderr, "[ui] WM_CLOSE on hwnd=%p id=%lld\n", (void *)hwnd, (long long)id);
        if (e) fire(e, &e->cb_action, id, 0);
        if (e && e->cb_action.tag == HV_CLO) return 0;   /* handler decides */
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY: {
        popup_close();
        int windows_left = 0;
        for (size_t i = 0; i < tbl_len; i++)
            if (tbl[i].kind == HPUI_WINDOW && tbl[i].hwnd &&
                tbl[i].id != id && IsWindow(tbl[i].hwnd))
                windows_left++;
        if (e) { e->hwnd = NULL; }
        if (hpui_win_debug())
            fprintf(stderr, "[ui] WM_DESTROY id=%lld left=%d\n",
                    (long long)id, windows_left);
        if (windows_left == 0) PostQuitMessage(0);
        return 0;
    }
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* =====================================================================
 *  Canvas — an offscreen surface a script paints itself
 * =====================================================================
 *
 *  Every other control in this library owns its pixels: the toolkit decides
 *  what a button looks like. A canvas owns none — it keeps a backbuffer the
 *  exact size of the control, hands the script GDI drawing calls that write
 *  into it, and blits it on WM_PAINT. That is what makes an HTML/CSS engine
 *  written in HolyPHP possible: it computes boxes and colours, this draws
 *  them, and nothing in between decides anything.
 *
 *  Coordinates are logical 96-dpi pixels like everywhere else in the
 *  library, so one box tree looks identical on a 100% and a 150% display.
 *  Input events report the pointer in the same units (see HPUI_EV_*), and a
 *  press captures the mouse so a drag that leaves the control still gets its
 *  moves and its release — without that, a text selection broke the moment
 *  the pointer crossed the editor's edge.
 *
 *  Fonts are a separate table from the widget cache: family, size, weight
 *  and slant are all part of the key here, because a code editor needs a
 *  monospace face at an exact size while the chrome beside it uses Segoe UI.
 *  Both the table and the measure DC are dropped on a DPI change.
 */

typedef struct CvFont {
    wchar_t fam[64];
    int     pt;
    bool    bold, italic;
    HFONT   f;
    int     lh;            /* line height, logical px */
} CvFont;

static CvFont *g_cvf = NULL;
static size_t  g_ncvf = 0, g_capcvf = 0;
static HDC     g_cv_dc = NULL;      /* scratch DC for measuring */

static void cvfonts_drop(void) {
    for (size_t i = 0; i < g_ncvf; i++)
        if (g_cvf[i].f) DeleteObject(g_cvf[i].f);
    g_ncvf = 0;
    if (g_cv_dc) { DeleteDC(g_cv_dc); g_cv_dc = NULL; }
}

static HDC cv_measure_dc(void) {
    if (!g_cv_dc) g_cv_dc = CreateCompatibleDC(NULL);
    return g_cv_dc;
}

/* Cross a device pixel count into logical pixels (the inverse of sc()).
 * Truncating rather than rounding keeps a drag that leaves the surface
 * monotonic: -3px must not come back as -3 after scaling up and down. */
/* Physical pixels -> the logical 96-dpi pixels a script lays out in. Rounds
 * like hpui_client_size() above; truncating here and rounding there made a
 * canvas report 784 logical pixels while its window's client area reported
 * 783, and a UI that lays itself out from both ends up one pixel tall. */
static int cv_unscale(int v) {
    int d = g_dpi > 0 ? g_dpi : 96;
    return (v * 96 + d / 2) / d;
}

/* Same crossing, but rounding UP. A width the layout reserves for a string
 * has to be at least the width GDI will draw, or the last glyph is clipped:
 * a 25px string is 31.25px at 125%, rounding to nearest reserved 31 and GDI
 * dropped the final letter -- "Save" came out "Sav", every string on screen
 * a character short. Rounding up costs at most one pixel of padding. */
static int cv_unscale_up(int v) {
    int d = g_dpi > 0 ? g_dpi : 96;
    return (v * 96 + d - 1) / d;
}

static CvFont *cv_font_at(int64_t id) {
    if (id <= 0 || (size_t)id > g_ncvf) return NULL;
    return &g_cvf[id - 1];
}

/* Build (or find) a face. id is 1-based; 0 means "nothing" so a script can
 * use 0 as a sentinel without colliding with a real handle. */
int64_t hpui_font_id(const char *family, int pt, bool bold, bool italic) {
    wchar_t fam[64];
    MultiByteToWideChar(CP_UTF8, 0, family && *family ? family : "Segoe UI", -1, fam, 64);
    if (pt <= 0) pt = 9;
    if (pt > 200) pt = 200;
    for (size_t i = 0; i < g_ncvf; i++)
        if (g_cvf[i].pt == pt && g_cvf[i].bold == bold &&
            g_cvf[i].italic == italic && wcscmp(g_cvf[i].fam, fam) == 0)
            return (int64_t)i + 1;
    if (g_ncvf == g_capcvf) {
        g_capcvf = g_capcvf ? g_capcvf * 2 : 16;
        g_cvf = (CvFont *)realloc(g_cvf, g_capcvf * sizeof(CvFont));
    }
    CvFont *c = &g_cvf[g_ncvf];
    memset(c, 0, sizeof *c);
    wcsncpy(c->fam, fam, 63);
    c->pt = pt;
    c->bold = bold;
    c->italic = italic;
    /* -MulDiv(pt, dpi, 72) is the point->pixel conversion that keeps a 10pt
     * body text the same physical size at every display scale. */
    c->f = CreateFontW(-MulDiv(pt, g_dpi, 72), 0, 0, 0,
                       bold ? FW_SEMIBOLD : FW_NORMAL, italic ? TRUE : FALSE,
                       FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, fam);
    if (!c->f) c->f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    TEXTMETRICW tm;
    int lh = pt * g_dpi / 72 + 3;
    HDC dc = cv_measure_dc();
    if (dc) {
        HFONT of = (HFONT)SelectObject(dc, c->f);
        if (GetTextMetricsW(dc, &tm)) lh = tm.tmHeight + tm.tmExternalLeading;
        SelectObject(dc, of);
    }
    c->lh = lh > 1 ? cv_unscale(lh) : 12;
    g_ncvf++;
    return (int64_t)g_ncvf;
}

int64_t hpui_font_w(int64_t id, const char *s) {
    CvFont *c = cv_font_at(id);
    HDC dc = cv_measure_dc();
    if (!c || !dc || !s || !*s) return 0;
    wchar_t *w = NULL;
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 1) return 0;
    w = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)n);
    if (!w) return 0;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    HFONT of = (HFONT)SelectObject(dc, c->f);
    SIZE sz = {0, 0};
    GetTextExtentPoint32W(dc, w, n - 1, &sz);
    SelectObject(dc, of);
    free(w);
    return (int64_t)cv_unscale_up(sz.cx);
}

int64_t hpui_font_h(int64_t id) {
    CvFont *c = cv_font_at(id);
    return c ? (int64_t)c->lh : 0;
}

static void cv_free_backbuffer(UiEntry *e) {
    if (e->cv_dc) {
        if (e->cv_old_bmp) SelectObject(e->cv_dc, e->cv_old_bmp);
        DeleteDC(e->cv_dc);
        e->cv_dc = NULL;
    }
    if (e->cv_bmp) { DeleteObject(e->cv_bmp); e->cv_bmp = NULL; }
    e->cv_old_bmp = NULL;
    e->cv_w = e->cv_h = 0;
}

/* Make sure the backbuffer matches the control's client size. */
static bool cv_ensure(UiEntry *e) {
    if (!e || !e->hwnd || !IsWindow(e->hwnd)) return false;
    RECT rc;
    GetClientRect(e->hwnd, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (e->cv_dc && w == e->cv_w && h == e->cv_h) return true;
    cv_free_backbuffer(e);
    HDC wdc = GetDC(e->hwnd);
    HDC mem = wdc ? CreateCompatibleDC(wdc) : NULL;
    HBITMAP bmp = wdc ? CreateCompatibleBitmap(wdc, w, h) : NULL;
    if (wdc) ReleaseDC(e->hwnd, wdc);
    if (!mem || !bmp) {
        if (mem) DeleteDC(mem);
        if (bmp) DeleteObject(bmp);
        return false;
    }
    e->cv_old_bmp = (HBITMAP)SelectObject(mem, bmp);
    e->cv_dc = mem;
    e->cv_bmp = bmp;
    e->cv_w = w;
    e->cv_h = h;
    SetBkMode(mem, TRANSPARENT);
    /* A fresh surface starts as the theme background rather than black: a
     * frame the script has only partly drawn should look like empty space in
     * the app, not like a hole in the window. */
    RECT all;
    all.left = 0; all.top = 0; all.right = w; all.bottom = h;
    fill_rect(mem, all, g_theme.bg);
    return true;
}

/* The handle a script passed must be a live canvas, with a usable surface. */
static UiEntry *cv_of(int64_t h) {
    UiEntry *e = tbl_find(h);
    if (!e || e->kind != HPUI_CANVAS || !e->hwnd || !IsWindow(e->hwnd)) return NULL;
    if (!cv_ensure(e)) return NULL;
    return e;
}

/* show the backbuffer now, without waiting for the paint queue: a script
 * animating from a timer needs its frame on screen before the next tick */
static void cv_present(UiEntry *e) {
    if (!e->cv_dc || !e->hwnd) return;
    HDC dc = GetDC(e->hwnd);
    if (!dc) return;
    BitBlt(dc, 0, 0, e->cv_w, e->cv_h, e->cv_dc, 0, 0, SRCCOPY);
    ReleaseDC(e->hwnd, dc);
}

/* The callback slot behind an event number, keeping the original 1..5
 * meanings so existing ui_on() bindings keep working on a canvas too. */
static hval *cv_slot(UiEntry *e, int ev) {
    switch (ev) {
    case HPUI_EV_CLICK:  return &e->cb_click;
    case HPUI_EV_CHANGE: return &e->cb_change;
    case HPUI_EV_ACTION: return &e->cb_action;
    case HPUI_EV_KEY:    return &e->cb_key;
    case HPUI_EV_DRAG:   return &e->cb_drag;
    default:
        if (ev > 5 && ev < HPUI_EV_NEV) return &e->cb_ev[ev];
        return NULL;
    }
}

static void cv_fire(UiEntry *e, int ev, int64_t a1, int64_t a2, int64_t a3) {
    hval *slot = cv_slot(e, ev);
    if (!slot || slot->tag != HV_CLO || !slot->u.c) return;
    hval args[3];
    args[0] = hp_of_int(a1);
    args[1] = hp_of_int(a2);
    args[2] = hp_of_int(a3);
    ui_call_guarded(*slot, args, 3);
}

/* A key press, with the character it produced as a STRING.
 *
 * The first argument is the virtual key, the second the text the key typed,
 * the third the modifiers -- that is what every handler is written against
 * (`key($vk, $ch, $mods)` inserts $ch). Handing the code point over as an
 * int made the editor insert its number instead: pressing `a` typed "97".
 * Keys that produce no text (Return, Escape, the arrows) send an empty
 * string, so a handler can tell "typed nothing" from "typed a space". */
static void cv_fire_key(UiEntry *e, int ev, int64_t vk, wchar_t ch, int64_t mods) {
    hval *slot = cv_slot(e, ev);
    if (!slot || slot->tag != HV_CLO || !slot->u.c) return;
    char utf8[8];
    int len = 0;
    if (ch >= 32 && ch != 127) {
        len = WideCharToMultiByte(CP_UTF8, 0, &ch, 1, utf8, (int)sizeof utf8, NULL, NULL);
        if (len < 0) len = 0;
    }
    hval args[3];
    args[0] = hp_of_int(vk);
    args[1] = len > 0 ? hp_of_str(hp_str_new(utf8, (size_t)len)) : hp_of_str(hp_null_str());
    args[2] = hp_of_int(mods);
    ui_call_guarded(*slot, args, 3);
}

static void cv_mouse(UiEntry *e, int ev, LPARAM lp, int64_t button) {
    cv_fire(e, ev, cv_unscale(GET_X_LPARAM(lp)), cv_unscale(GET_Y_LPARAM(lp)), button);
}

/* ---------------- drawing API ---------------- */

int64_t hpui_draw_clear(int64_t h, int64_t rgb) {
    UiEntry *e = cv_of(h);
    if (!e) return -1;
    RECT r;
    r.left = 0; r.top = 0; r.right = e->cv_w; r.bottom = e->cv_h;
    fill_rect(e->cv_dc, r, rgbv(rgb));
    return 0;
}

int64_t hpui_draw_rect(int64_t h, int x, int y, int w, int ht, int64_t rgb) {
    UiEntry *e = cv_of(h);
    if (!e) return -1;
    RECT r;
    r.left = sc(x); r.top = sc(y);
    r.right = sc(x + w); r.bottom = sc(y + ht);
    if (r.right <= r.left || r.bottom <= r.top) return 0;
    fill_rect(e->cv_dc, r, rgbv(rgb));
    return 0;
}

int64_t hpui_draw_round(int64_t h, int x, int y, int w, int ht, int radius,
                        int64_t fill, int64_t stroke, int sw) {
    UiEntry *e = cv_of(h);
    if (!e) return -1;
    RECT r;
    r.left = sc(x); r.top = sc(y);
    r.right = sc(x + w); r.bottom = sc(y + ht);
    if (r.right <= r.left || r.bottom <= r.top) return 0;
    fill_round(e->cv_dc, r, sc(radius),
               fill < 0 ? (COLORREF)-1 : rgbv(fill),
               stroke < 0 ? (COLORREF)-1 : rgbv(stroke), sc(sw));
    return 0;
}

int64_t hpui_draw_line(int64_t h, int x1, int y1, int x2, int y2,
                       int64_t rgb, int w) {
    UiEntry *e = cv_of(h);
    if (!e) return -1;
    HPEN pen = CreatePen(PS_SOLID, w > 0 ? sc(w) : 1, rgbv(rgb));
    HPEN op = (HPEN)SelectObject(e->cv_dc, pen);
    MoveToEx(e->cv_dc, sc(x1), sc(y1), NULL);
    LineTo(e->cv_dc, sc(x2), sc(y2));
    SelectObject(e->cv_dc, op);
    DeleteObject(pen);
    return 0;
}

/* Draw text in the box (x, y, w, ht).
 *
 * flags: 1 = ellipsis when it does not fit, 2 = centre vertically,
 *        4 = right align, 8 = no clipping (draw past the box).
 * The box is what makes this usable for a UI: a label knows where it may
 * spill to, and GDI clips to it instead of overrunning its neighbour. */
int64_t hpui_draw_text(int64_t h, int x, int y, int w, int ht, const char *s,
                       int64_t rgb, int64_t fontid, int flags) {
    UiEntry *e = cv_of(h);
    CvFont *c = cv_font_at(fontid);
    if (!e) return -1;
    if (!s || !*s) return 0;
    if (!c) c = cv_font_at(hpui_font_id("Segoe UI", g_theme.font_pt, false, false));
    if (!c) return -1;
    wchar_t *wbuf = NULL;
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 1) return 0;
    wbuf = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)n);
    if (!wbuf) return -1;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, wbuf, n);

    RECT r;
    r.left = sc(x);
    r.top = sc(y);
    r.right = sc(x + w);
    r.bottom = sc(y + ht);
    UINT f = DT_SINGLELINE | DT_NOPREFIX;
    f |= (flags & 2) ? DT_VCENTER : DT_TOP;
    f |= (flags & 4) ? DT_RIGHT : DT_LEFT;
    if (flags & 1) f |= DT_END_ELLIPSIS;
    /* One device pixel of slack on a plain draw. The rect is a rounded
     * crossing of a logical width; without the slack a string whose device
     * width lands on .5 loses its last glyph (see cv_unscale_up). An
     * ellipsised draw keeps its exact edge, because there the width IS the
     * answer -- "Dan..." has to stop where the box stops. */
    if (!(flags & 1)) r.right += 1;
    if (flags & 8) {
        f &= ~(UINT)DT_END_ELLIPSIS;
        r.right = r.left + (int)hpui_font_w(fontid, s) + sc(4);
        if (!(flags & 4)) r.left = sc(x);
    }
    SetBkMode(e->cv_dc, TRANSPARENT);
    SetTextColor(e->cv_dc, rgbv(rgb));
    HFONT of = (HFONT)SelectObject(e->cv_dc, c->f);
    DrawTextW(e->cv_dc, wbuf, n - 1, &r, f);
    SelectObject(e->cv_dc, of);
    free(wbuf);
    return 0;
}

int64_t hpui_draw_clip(int64_t h, int x, int y, int w, int ht) {
    UiEntry *e = cv_of(h);
    if (!e) return -1;
    IntersectClipRect(e->cv_dc, sc(x), sc(y), sc(x + w), sc(y + ht));
    return 0;
}

int64_t hpui_draw_unclip(int64_t h) {
    UiEntry *e = cv_of(h);
    if (!e) return -1;
    SelectClipRgn(e->cv_dc, NULL);
    return 0;
}

int64_t hpui_canvas_invalidate(int64_t h) {
    UiEntry *e = tbl_find(h);
    if (!e || !e->hwnd) return -1;
    InvalidateRect(e->hwnd, NULL, FALSE);
    return 0;
}

int64_t hpui_canvas_flush(int64_t h) {
    UiEntry *e = cv_of(h);
    if (!e) return -1;
    cv_present(e);
    return 0;
}

/* The colour a drawn pixel actually has, straight out of the backbuffer.
 *
 * A rendering engine is otherwise verified by looking at it, which no test
 * can do: an HTML/CSS box tree can be laid out perfectly and painted onto a
 * surface that is never blitted, or blitted one pixel off, and every unit
 * test of the layout still passes. Reading a pixel back is what lets a test
 * assert "the sidebar is 48px wide and dark grey" rather than "no crash".
 * Returns -1 when the canvas or the coordinate is outside the surface. */
int64_t hpui_canvas_pixel(int64_t h, int x, int y) {
    UiEntry *e = cv_of(h);
    if (!e) return -1;
    int px = sc(x), py = sc(y);
    if (px < 0 || py < 0 || px >= e->cv_w || py >= e->cv_h) return -1;
    COLORREF c = GetPixel(e->cv_dc, px, py);
    if (c == CLR_INVALID) return -1;
    return (int64_t)((GetRValue(c) << 16) | (GetGValue(c) << 8) | GetBValue(c));
}

int64_t hpui_canvas_size(int64_t h, int64_t *ow, int64_t *oh) {
    UiEntry *e = tbl_find(h);
    if (!e || !e->hwnd) return -1;
    RECT rc;
    GetClientRect(e->hwnd, &rc);
    if (ow) *ow = cv_unscale((int)(rc.right - rc.left));
    if (oh) *oh = cv_unscale((int)(rc.bottom - rc.top));
    return 0;
}

int64_t hpui_cursor_set(int64_t h, int64_t shape) {
    UiEntry *e = tbl_find(h);
    if (!e) return -1;
    e->cv_cursor = (int)shape;
    return 0;
}

/* ---------------- the canvas window procedure ---------------- */

static void cv_apply_cursor(UiEntry *e) {
    LPCWSTR id = IDC_ARROW;
    switch (e->cv_cursor) {
    case HPUI_CUR_IBEAM:   id = IDC_IBEAM; break;
    case HPUI_CUR_HAND:    id = IDC_HAND; break;
    case HPUI_CUR_SIZEWE:  id = IDC_SIZEWE; break;
    case HPUI_CUR_SIZENS:  id = IDC_SIZENS; break;
    case HPUI_CUR_SIZEALL: id = IDC_SIZEALL; break;
    case HPUI_CUR_CROSS:   id = IDC_CROSS; break;
    case HPUI_CUR_NO:      id = IDC_NO; break;
    default: break;
    }
    SetCursor(LoadCursorW(NULL, id));
}

static LRESULT CALLBACK canvas_wndproc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    UiEntry *e = entry_of(hw);
    if (!e) return DefWindowProcW(hw, msg, wp, lp);
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;                       /* the blit covers everything */

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hw, &ps);
        if (cv_ensure(e)) BitBlt(dc, 0, 0, e->cv_w, e->cv_h, e->cv_dc, 0, 0, SRCCOPY);
        EndPaint(hw, &ps);
        return 0;
    }

    case WM_SIZE:
        /* A resize throws the old pixels away with the old buffer, so tell
         * the script the new size and let it redraw. */
        cv_ensure(e);
        cv_fire(e, HPUI_EV_RESIZE, cv_unscale(LOWORD(lp)), cv_unscale(HIWORD(lp)), 0);
        return 0;

    case WM_MOUSEMOVE:
        if (!e->cv_track) {
            TRACKMOUSEEVENT t;
            memset(&t, 0, sizeof t);
            t.cbSize = sizeof t;
            t.dwFlags = TME_LEAVE;
            t.hwndTrack = hw;
            TrackMouseEvent(&t);
            e->cv_track = true;
            cv_fire(e, HPUI_EV_ENTER, 0, 0, 0);
        }
        cv_mouse(e, HPUI_EV_MOVE, lp, 0);
        return 0;

    case WM_MOUSELEAVE:
        e->cv_track = false;
        cv_fire(e, HPUI_EV_LEAVE, 0, 0, 0);
        return 0;

    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: {
        int64_t button = msg == WM_LBUTTONDOWN ? 1 : (msg == WM_RBUTTONDOWN ? 2 : 3);
        /* The canvas has no WS_TABSTOP: typing only reaches it after a click,
         * which is exactly how a code editor is supposed to behave. */
        SetFocus(hw);
        if (button == 1) {
            SetCapture(hw);
            e->cv_captured = true;
            e->cv_drag = true;
        }
        cv_mouse(e, HPUI_EV_DOWN, lp, button);
        return 0;
    }

    case WM_LBUTTONDBLCLK:
        cv_mouse(e, HPUI_EV_DBL, lp, 1);
        return 0;

    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: {
        int64_t button = msg == WM_LBUTTONUP ? 1 : (msg == WM_RBUTTONUP ? 2 : 3);
        bool pressed = e->cv_drag;
        if (button == 1 && e->cv_captured) {
            e->cv_captured = false;
            e->cv_drag = false;
            ReleaseCapture();
        }
        cv_mouse(e, HPUI_EV_UP, lp, button);
        /* A click is a press *and* a release, and it is reported after the
         * release so a handler that acts on a click already knows the final
         * pointer position. */
        if (button == 1 && pressed) cv_mouse(e, HPUI_EV_CLICK, lp, 1);
        return 0;
    }

    case WM_CAPTURECHANGED:
        e->cv_captured = false;
        e->cv_drag = false;
        return 0;

    case WM_MOUSEWHEEL: {
        POINT pt;
        pt.x = GET_X_LPARAM(lp);
        pt.y = GET_Y_LPARAM(lp);
        ScreenToClient(hw, &pt);
        int notches = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
        if (notches == 0) notches = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1 : -1;
        cv_fire(e, HPUI_EV_WHEEL, cv_unscale(pt.x), cv_unscale(pt.y), notches);
        return 0;
    }

    case WM_KEYDOWN: {
        BYTE state[256];
        GetKeyboardState(state);
        wchar_t buf[8];
        int vk = (int)wp;
        int n = ToUnicode(vk, (int)(lp >> 16), state, buf, 8, 0);
        wchar_t ch = n > 0 ? buf[0] : 0;
        if (ch < 32) ch = 0;
        int mods = 0;
        if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= 1;
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000) mods |= 2;
        if (GetAsyncKeyState(VK_MENU) & 0x8000) mods |= 4;
        /* Ctrl+S is a shortcut, not text; the buffer must not receive the
         * invisible 0x13 ToUnicode reports for it. Ctrl+Alt is the exception:
         * that is AltGr, and on a German keyboard it is how `@`, `{` and `\`
         * are typed -- throwing it away would make the editor unable to
         * write its own language. */
        bool ctrl = (mods & 1) != 0, alt = (mods & 4) != 0;
        if (ctrl != alt) ch = 0;
        cv_fire_key(e, HPUI_EV_KEY, vk, ch, mods);
        return 0;    /* claim it: DefWindowProc would beep on Tab and arrows */
    }

    case WM_CHAR:
        cv_fire(e, HPUI_EV_CHAR, (int64_t)wp, 0, 0);
        return 0;

    case WM_SETFOCUS:
        cv_fire(e, HPUI_EV_FOCUS, 1, 0, 0);
        return 0;
    case WM_KILLFOCUS:
        cv_fire(e, HPUI_EV_FOCUS, 0, 0, 0);
        return 0;

    case WM_GETDLGCODE:
        /* A canvas is a text surface: it wants Tab, the arrows and every
         * character. Without this the parent's dialog manager steals Tab and
         * the arrow keys, and an editor cannot indent or move its caret. */
        return DLGC_WANTCHARS | DLGC_WANTARROWS | DLGC_WANTTAB;

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            cv_apply_cursor(e);
            return TRUE;
        }
        break;

    case WM_DESTROY:
        cv_free_backbuffer(e);
        return 0;
    default: break;
    }
    return DefWindowProcW(hw, msg, wp, lp);
}

#endif /* _WIN32 */

/* ================= non-Windows stubs (API-complete, inert) ================= */
#ifndef _WIN32
void hpui_init(void) {}
int64_t hpui_dpi_scale(void) { return 96; }
int64_t hpui_dispatch(int ms) { (void)ms; return 0; }
void hpui_run_main(void) {}
void hpui_quit(void) {}
int64_t hpui_window_new(const char *t, int x, int y, int w, int h) {
    (void)t; (void)x; (void)y; (void)w; (void)h; return -1;
}
int64_t hpui_window_show(int64_t w, bool v) { (void)w; (void)v; return -1; }
int64_t hpui_window_title(int64_t w, const char *t) { (void)w; (void)t; return -1; }
int64_t hpui_window_size(int64_t w, int a, int b) { (void)w; (void)a; (void)b; return -1; }
int64_t hpui_window_pos(int64_t w, int a, int b) { (void)w; (void)a; (void)b; return -1; }
int64_t hpui_window_close(int64_t w) { (void)w; return -1; }
int64_t hpui_window_scroll(int64_t w, int a, int b, int64_t *s, int64_t *c) {
    (void)w; (void)a; (void)b; if (s) *s = 0; if (c) *c = 0; return -1;
}
bool hpui_window_alive(int64_t w) { (void)w; return false; }
int64_t hpui_ctrl_new(int64_t k, int64_t p, const char *t, int x, int y, int w, int h) {
    (void)k; (void)p; (void)t; (void)x; (void)y; (void)w; (void)h; return -1;
}
int64_t hpui_ctrl_set_text(int64_t h, const char *t) { (void)h; (void)t; return -1; }
int64_t hpui_ctrl_text(int64_t h, hstr **o) { (void)h; (void)o; return -1; }
int64_t hpui_ctrl_show(int64_t h, bool v) { (void)h; (void)v; return -1; }
int64_t hpui_ctrl_enable(int64_t h, bool v) { (void)h; (void)v; return -1; }
int64_t hpui_ctrl_move(int64_t h, int a, int b, int c, int d) {
    (void)h; (void)a; (void)b; (void)c; (void)d; return -1;
}
int64_t hpui_ctrl_rect(int64_t h, int64_t *x, int64_t *y, int64_t *w, int64_t *ht) {
    (void)h; *x = *y = *w = *ht = 0; return -1;
}
int64_t hpui_ctrl_focus(int64_t h) { (void)h; return -1; }
int64_t hpui_ctrl_select_all(int64_t h) { (void)h; return -1; }
int64_t hpui_list_add(int64_t h, const char *i) { (void)h; (void)i; return -1; }
int64_t hpui_list_clear(int64_t h) { (void)h; return -1; }
int64_t hpui_list_remove(int64_t h, int64_t i) { (void)h; (void)i; return -1; }
int64_t hpui_list_count(int64_t h, int64_t *o) { (void)h; (void)o; return -1; }
int64_t hpui_list_selected(int64_t h, int64_t *o) { (void)h; (void)o; return -1; }
int64_t hpui_list_select(int64_t h, int64_t i) { (void)h; (void)i; return -1; }
int64_t hpui_list_text(int64_t h, int64_t i, hstr **o) { (void)h; (void)i; (void)o; return -1; }
int64_t hpui_check_get(int64_t h, bool *o) { (void)h; (void)o; return -1; }
int64_t hpui_check_set(int64_t h, bool v) { (void)h; (void)v; return -1; }
int64_t hpui_progress_set(int64_t h, int64_t p) { (void)h; (void)p; return -1; }
int64_t hpui_progress_get(int64_t h) { (void)h; return 0; }
int64_t hpui_slider_get(int64_t h, int64_t *o) { (void)h; (void)o; return -1; }
int64_t hpui_slider_set(int64_t h, int64_t p) { (void)h; (void)p; return -1; }
int64_t hpui_range_set(int64_t h, int64_t a, int64_t b) { (void)h; (void)a; (void)b; return -1; }
int64_t hpui_ctrl_enabled(int64_t h) { (void)h; return 0; }
int64_t hpui_ctrl_checked(int64_t h) { (void)h; return 0; }
int64_t hpui_menu_new(int64_t w, const char *l) { (void)w; (void)l; return -1; }
int64_t hpui_menu_item(int64_t m, const char *l, int64_t i) { (void)m; (void)l; (void)i; return -1; }
int64_t hpui_menu_sep(int64_t m) { (void)m; return -1; }
int64_t hpui_ctrl_set_bg(int64_t h, int64_t r) { (void)h; (void)r; return -1; }
int64_t hpui_ctrl_set_fg(int64_t h, int64_t r) { (void)h; (void)r; return -1; }
int64_t hpui_ctrl_accent(int64_t h, int64_t r) { (void)h; (void)r; return -1; }
int64_t hpui_ctrl_radius(int64_t h, int64_t r) { (void)h; (void)r; return -1; }
int64_t hpui_input_margin(int64_t h, int64_t l, int64_t r) {
    (void)h; (void)l; (void)r; return -1;
}
int64_t hpui_ctrl_style(int64_t h, int64_t r) { (void)h; (void)r; return -1; }
int64_t hpui_ctrl_font(int64_t h, int64_t s, bool b) { (void)h; (void)s; (void)b; return -1; }
int64_t hpui_picture_load(int64_t h, const char *p) { (void)h; (void)p; return -1; }
int64_t hpui_ctrl_pic(int64_t h, const char *p) { (void)h; (void)p; return -1; }
int64_t hpui_theme_set(int64_t w, const int64_t *v, int n) { (void)w; (void)v; (void)n; return -1; }
int64_t hpui_repaint(int64_t h) { (void)h; return -1; }
int64_t hpui_ctrl_svg(int64_t h, const char *s) { (void)h; (void)s; return -1; }
int64_t hpui_ctrl_raise(int64_t h) { (void)h; return -1; }
int64_t hpui_on_event(int64_t h, int64_t e, hval c) { (void)h; (void)e; (void)c; return -1; }
int64_t hpui_client_size(int64_t win, int64_t *a, int64_t *b) {
    (void)win; if (a) *a = 0; if (b) *b = 0; return -1;
}
int64_t hpui_hwnd(int64_t win) { (void)win; return 0; }
int64_t hpui_timer(int64_t ms, hval c) { (void)ms; (void)c; return -1; }
int64_t hpui_msgbox(int64_t w, const char *t, const char *x, int ty) {
    (void)w; (void)t; (void)x; (void)ty; return 0;
}
hval hpui_open_file(int64_t w, const char *f) { (void)w; (void)f; return hp_null; }
hval hpui_save_file(int64_t w, const char *f) { (void)w; (void)f; return hp_null; }
hval hpui_pick_folder(int64_t w) { (void)w; return hp_null; }
hval hpui_color_pick(int64_t w, int64_t r) { (void)w; (void)r; return hp_null; }
int64_t ui_clip_set(const char *t) { (void)t; return -1; }
hval ui_clip_get(void) { return hp_null; }
int64_t hpui_text_lines(const char *t, int pt, bool b, int mw) {
    (void)t; (void)pt; (void)b; (void)mw; return 1;
}
int64_t hpui_textw(const char *t, int pt, bool b) {
    (void)t; (void)pt; (void)b; return (t ? (int64_t)strlen(t) * 8 : 0);
}
/* Canvas: API-complete but inert off Windows. A program that draws into a
 * canvas still runs (and still gets its layout computed) on a machine with
 * no window system - it just has nowhere to show the result. */
int64_t hpui_font_id(const char *f, int pt, bool b, bool it) {
    (void)f; (void)pt; (void)b; (void)it; return 0;
}
int64_t hpui_font_w(int64_t id, const char *s) { (void)id; (void)s; return 0; }
int64_t hpui_font_h(int64_t id) { (void)id; return 12; }
int64_t hpui_draw_clear(int64_t h, int64_t rgb) { (void)h; (void)rgb; return -1; }
int64_t hpui_draw_rect(int64_t h, int x, int y, int w, int ht, int64_t rgb) {
    (void)h; (void)x; (void)y; (void)w; (void)ht; (void)rgb; return -1;
}
int64_t hpui_draw_round(int64_t h, int x, int y, int w, int ht, int r,
                        int64_t fill, int64_t stroke, int sw) {
    (void)h; (void)x; (void)y; (void)w; (void)ht; (void)r;
    (void)fill; (void)stroke; (void)sw; return -1;
}
int64_t hpui_draw_line(int64_t h, int x1, int y1, int x2, int y2,
                       int64_t rgb, int w) {
    (void)h; (void)x1; (void)y1; (void)x2; (void)y2; (void)rgb; (void)w; return -1;
}
int64_t hpui_draw_text(int64_t h, int x, int y, int w, int ht, const char *s,
                       int64_t rgb, int64_t fontid, int flags) {
    (void)h; (void)x; (void)y; (void)w; (void)ht; (void)s; (void)rgb;
    (void)fontid; (void)flags; return -1;
}
int64_t hpui_draw_clip(int64_t h, int x, int y, int w, int ht) {
    (void)h; (void)x; (void)y; (void)w; (void)ht; return -1;
}
int64_t hpui_draw_unclip(int64_t h) { (void)h; return -1; }
int64_t hpui_canvas_pixel(int64_t h, int x, int y) {
    (void)h; (void)x; (void)y; return -1;
}
int64_t hpui_canvas_invalidate(int64_t h) { (void)h; return -1; }
int64_t hpui_canvas_flush(int64_t h) { (void)h; return -1; }
int64_t hpui_canvas_size(int64_t h, int64_t *w, int64_t *ht) {
    (void)h; if (w) *w = 0; if (ht) *ht = 0; return -1;
}
int64_t hpui_cursor_set(int64_t h, int64_t shape) { (void)h; (void)shape; return -1; }
#endif

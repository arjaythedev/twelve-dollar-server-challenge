/* hphp_std.c — PHP-compatible standard library builtins.
 *
 * Each function matches the hpbi_<name> prototype contract: hval args,
 * hval return. Generated code projects scalars out of results.
 */
/* _DEFAULT_SOURCE: on glibc, putenv/clock_gettime/localtime_r/usleep/
 * getaddrinfo and friends are hidden under strict -std=c11 */
#if !defined(_WIN32)
#define _DEFAULT_SOURCE 1
#endif
#include "hphp_rt.h"
#include "hphp_modules.h"

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <immintrin.h>
#endif

/* Address argument shared by the raw memory builtins: the challenge runtime
 * passed raw pointers as integers; malloc() hands out an object-tagged value.
 * Both mean "this address". */
static void *hp_addr_arg(hval v) {
    if (v.tag == HV_OBJ) return v.u.p;
    return (void *)(intptr_t)hp_val_to_int(v);
}

/* Finish a byte buffer as a runtime string: copy the bytes into an hstr and
 * hand the buffer's heap block back. Every `Buf` builtin must use this (or
 * free the block itself); `hp_str_lit(buf_take(&b))` would leak the block. */
static hstr *hp_str_take_buf(Buf *b) {
    size_t n = b->len;
    char *p = hbuf_take(b);
    hstr *r = hp_str_new(p && n ? p : "", n);
    free(p);
    return r;
}

#ifndef HPHP_VERSION
#define HPHP_VERSION "1.1.0"
#endif

static void hbuf_json_r(hval v, Buf *b);

#include <ctype.h>

/* shares the core's PHP is_numeric() rule, so is_numeric()/is_int() and the
 * arithmetic operators agree on what counts as a numeric string */
static bool hp_num_like(hval v) {
    return v.tag == HV_INT || v.tag == HV_FLOAT || v.tag == HV_BOOL ||
           (v.tag == HV_STR && hp_str_is_numeric(v.u.s));
}

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#if defined(_WIN32)
/* winsock2.h must precede windows.h, otherwise the old winsock1 clashing
 * declarations from windows.h win and the socket calls do not link. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <direct.h>
#define mkdir(p) _mkdir(p)
typedef SOCKET hp_sock;
#define HP_SOCK_BAD INVALID_SOCKET
#define hp_sock_close(s) closesocket(s)
#define hp_sock_wouldblock() (WSAGetLastError() == WSAEWOULDBLOCK)
#else
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
typedef int hp_sock;
#define HP_SOCK_BAD (-1)
#define hp_sock_close(s) close(s)
#define hp_sock_wouldblock() (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
#endif

/* Graceful close for connected data sockets. Winsock's closesocket()
 * sends RST (destroying queued sends) when unread data is pending; a
 * "send + close" one-shot HTTP server always leaves its peer's request
 * unread, so the response was silently lost. Half-close first (FIN
 * flushes our sends), then drain the peer's in-flight bytes on a small
 * select() budget (loopback needs microseconds; slow clients only lose
 * the lingering wait, never correctness of already-queued data unless
 * they exceed the budget). POSIX close() already drains gracefully. */
static void hp_sock_graceful_close(hp_sock s) {
#ifdef _WIN32
    shutdown(s, SD_SEND);
    for (int i = 0; i < 8; i++) {           /* drain until nothing pends */
        fd_set rf;
        struct timeval tv = { 0, 2000 };    /* 2ms per round (Win rounds up) */
        FD_ZERO(&rf);
        FD_SET(s, &rf);
        if (select(0, &rf, NULL, NULL, &tv) <= 0)
            break;                          /* nothing in flight: done */
        char tmp[4096];
        int r = recv(s, tmp, sizeof tmp, 0);
        if (r <= 0) break;                  /* FIN or error: done */
    }
    closesocket(s);
#else
    close(s);
#endif
}

static void itoa_bin(int64_t v, int base, char *out) {
    static const char digits[] = "0123456789abcdef";
    char buf[80];
    int i = 0;
    bool neg = v < 0;
    unsigned long long u = neg ? (unsigned long long)(-(v + 1)) + 1 : (unsigned long long)v;
    if (u == 0) buf[i++] = '0';
    while (u) { buf[i++] = digits[u % (unsigned)base]; u /= (unsigned)base; }
    char *o = out;
    if (neg) *o++ = '-';
    while (i) *o++ = buf[--i];
    *o = 0;
}
#include <time.h>
#include <ctype.h>

#define RETI(v) return hp_of_int(v)
#define RETF(v) return hp_of_float(v)
#define RETB(v) return hp_of_bool(v)
#define RETS(v) return hp_of_str(v)

/* ---------- strings ---------- */
hval hpbi_strlen(hval s) { RETI(hp_str_len(hp_val_to_str(s))); }
hval hpbi_count(hval a) {
    if (a.tag == HV_STR) RETI(hp_str_len(a.u.s));
    if (a.tag == HV_ARR) RETI(hp_arr_len(a.u.a));
    RETI(0);
}
hval hpbi_sizeof(hval a) { return hpbi_count(a); }
hval hpbi_strtoupper(hval s) { RETS(hp_str_toupper(hp_val_to_str(s))); }
hval hpbi_strtolower(hval s) { RETS(hp_str_tolower(hp_val_to_str(s))); }
hval hpbi_ucfirst(hval s) {
    hstr *x = hp_val_to_str(s);
    if (!x->len) RETS(hp_str_copy(x));
    hstr *r = hp_str_copy(x);
    r->data[0] = (char)toupper((unsigned char)r->data[0]);
    RETS(r);
}
hval hpbi_lcfirst(hval s) {
    hstr *x = hp_val_to_str(s);
    if (!x->len) RETS(hp_str_copy(x));
    hstr *r = hp_str_copy(x);
    r->data[0] = (char)tolower((unsigned char)r->data[0]);
    RETS(r);
}
hval hpbi_trim(hval s) { RETS(hp_str_trim(hp_val_to_str(s))); }
hval hpbi_ltrim(hval s, hval chars) {
    hstr *x = hp_val_to_str(s);
    const char *set = chars.tag == HV_STR ? chars.u.s->data : " ";
    size_t slen = chars.tag == HV_STR ? chars.u.s->len : 1;
    size_t a = 0;
    while (a < x->len && (isspace((unsigned char)x->data[a]) || memchr(set, x->data[a], slen))) a++;
    RETS(hp_str_new(x->data + a, x->len - a));
}
hval hpbi_rtrim(hval s, hval chars) {
    hstr *x = hp_val_to_str(s);
    const char *set = chars.tag == HV_STR ? chars.u.s->data : " ";
    size_t slen = chars.tag == HV_STR ? chars.u.s->len : 1;
    size_t b = x->len;
    while (b > 0 && (isspace((unsigned char)x->data[b - 1]) || memchr(set, x->data[b - 1], slen))) b--;
    RETS(hp_str_new(x->data, b));
}
hval hpbi_chop(hval s) {
    hstr *x = hp_val_to_str(s);
    size_t b = x->len;
    while (b > 0 && isspace((unsigned char)x->data[b - 1])) b--;
    RETS(hp_str_new(x->data, b));
} /* chop = rtrim($s) in PHP */
hval hpbi_strrev(hval s) { RETS(hp_str_reverse(hp_val_to_str(s))); }
hval hpbi_str_repeat(hval s, hval n) {
    RETS(hp_str_repeat(hp_val_to_str(s), hp_val_to_int(n)));
}
hval hpbi_str_pad(hval s, hval n, hval pad) {
    hstr *x = hp_val_to_str(s);
    int64_t want = hp_val_to_int(n);
    if ((int64_t)x->len >= want) RETS(hp_str_copy(x));   /* always a new value */
    hstr *p = hp_val_to_str(pad);
    if (!p->len) p = hp_str_lit(" ");
    size_t fill = (size_t)(want - (int64_t)x->len);
    Buf b;
    buf_init(&b);
    buf_write(&b, x->data, x->len);
    while (b.len < (size_t)want) {
        size_t take = fill < p->len ? fill : p->len;
        buf_write(&b, p->data, take);
        fill -= take;
    }
    RETS(hp_str_take_buf(&b));
}
/* Replace every occurrence of `search` in `subject` with `replace`, building
 * the result once. Unlike hp_str_replace() in hphp_rt.c this frees every
 * intermediate buffer, so a loop stays flat. */
static hstr *hp_replace_t(hstr *subject, hstr *search, hstr *replace) {
    if (!subject) return hp_str_lit("");
    if (!search || search->len == 0) return hp_str_copy(subject);
    if (!replace) replace = hp_str_lit("");
    Buf b;
    buf_init(&b);
    size_t i = 0;
    while (i < subject->len) {
        if (i + search->len <= subject->len &&
            memcmp(subject->data + i, search->data, search->len) == 0) {
            buf_write(&b, replace->data, replace->len);
            i += search->len;
        } else {
            buf_putc(&b, subject->data[i++]);
        }
    }
    return hp_str_take_buf(&b);
}
hval hpbi_str_replace(hval search, hval replace, hval subject) {
    RETS(hp_replace_t(hp_val_to_str(subject), hp_val_to_str(search),
                      hp_val_to_str(replace)));
}
/* Same, with C-string pattern/replacement (used by the entity helpers). */
static hstr *hp_replace_c(hstr *subject, const char *search, const char *replace) {
    hstr *s = hp_str_lit(search), *r = hp_str_lit(replace);
    hstr *out = hp_replace_t(subject, s, r);
    hp_str_free(s);
    hp_str_free(r);
    return out;
}
hval hpbi_substr(hval s, hval start, hval len) {
    hstr *x = hp_val_to_str(s);
    int64_t st = hp_val_to_int(start);
    st = st < 0 ? st + (int64_t)x->len : st;
    if (st < 0) st = 0;
    if ((size_t)st > x->len) RETS(hp_str_lit(""));
    int64_t l = len.tag == HV_NULL ? (int64_t)x->len - st : hp_val_to_int(len);
    if (l < 0) l = (int64_t)x->len - st + l;
    if (l < 0) l = 0;
    RETS(hp_str_slice(x, st, st + l));
}
hval hpbi_strstr(hval h, hval n) {
    int64_t i = hp_str_index(hp_val_to_str(h), hp_val_to_str(n));
    if (i < 0) RETB(false);
    RETS(hp_str_slice(hp_val_to_str(h), i, hp_str_len(hp_val_to_str(h))));
}
hval hpbi_strchr(hval h, hval n) { return hpbi_strstr(h, n); }
hval hpbi_str_contains(hval h, hval n) {
    RETB(hp_str_index(hp_val_to_str(h), hp_val_to_str(n)) >= 0);
}
hval hpbi_str_starts_with(hval h, hval n) {
    hstr *x = hp_val_to_str(h), *y = hp_val_to_str(n);
    RETB(x->len >= y->len && memcmp(x->data, y->data, y->len) == 0);
}
hval hpbi_str_ends_with(hval h, hval n) {
    hstr *x = hp_val_to_str(h), *y = hp_val_to_str(n);
    RETB(x->len >= y->len && memcmp(x->data + x->len - y->len, y->data, y->len) == 0);
}
hval hpbi_strcmp(hval a, hval b) { RETI(hp_str_cmp(hp_val_to_str(a), hp_val_to_str(b))); }
hval hpbi_strncmp(hval a, hval b, hval n) {
    hstr *x = hp_val_to_str(a), *y = hp_val_to_str(b);
    int64_t k = hp_val_to_int(n);
    if (k < 0) k = 0;
    if ((size_t)k > x->len) k = (int64_t)x->len;
    if ((size_t)k > y->len) k = (int64_t)y->len;
    int r = k ? memcmp(x->data, y->data, (size_t)k) : 0;
    RETI(r < 0 ? -1 : r > 0 ? 1 : 0);
}
static int hp_str_casecmp_n(hstr *a, hstr *b) {
    size_t n = a->len < b->len ? a->len : b->len;
    for (size_t i = 0; i < n; i++) {
        int ca = tolower((unsigned char)a->data[i]);
        int cb = tolower((unsigned char)b->data[i]);
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    return a->len < b->len ? -1 : a->len > b->len ? 1 : 0;
}
hval hpbi_strcasecmp(hval a, hval b) {
    RETI(hp_str_casecmp_n(hp_val_to_str(a), hp_val_to_str(b)));
}
hval hpbi_strncasecmp(hval a, hval b, hval n) {
    hstr *x = hp_val_to_str(a), *y = hp_val_to_str(b);
    size_t k = (size_t)hp_val_to_int(n);
    if (k > x->len) k = x->len;
    if (k > y->len) k = y->len;
    int r = 0;
    for (size_t i = 0; i < k; i++) {
        int ca = tolower((unsigned char)x->data[i]);
        int cb = tolower((unsigned char)y->data[i]);
        if (ca != cb) { r = ca < cb ? -1 : 1; break; }
    }
    RETI(r);
}
hval hpbi_explode(hval sep, hval s) {
    hstr *x = hp_val_to_str(s), *d = hp_val_to_str(sep);
    harr *r = hp_arr_new();
    if (d->len == 0) {
        /* empty separator: split into single characters. Steps over the
         * UTF-8 continuation bytes so "ä" stays one element instead of
         * becoming two broken bytes. */
        for (size_t i = 0; i < x->len; ) {
            unsigned char c = (unsigned char)x->data[i];
            size_t n = (c & 0x80) == 0 ? 1 :
                       (c & 0xE0) == 0xC0 ? 2 :
                       (c & 0xF0) == 0xE0 ? 3 :
                       (c & 0xF8) == 0xF0 ? 4 : 1;
            if (i + n > x->len) n = 1;   /* truncated tail: take one byte */
            hp_arr_push_take(r, hp_of_str(hp_str_new(x->data + i, n)));
            i += n;
        }
        return hp_of_arr(r);
    }
    size_t start = 0;
    for (size_t i = 0; i + d->len <= x->len;) {
        if (memcmp(x->data + i, d->data, d->len) == 0) {
            hp_arr_push_take(r, hp_of_str(hp_str_new(x->data + start, i - start)));
            i += d->len;
            start = i;
        } else i++;
    }
    hp_arr_push_take(r, hp_of_str(hp_str_new(x->data + start, x->len - start)));
    return hp_of_arr(r);
}
hval hpbi_split(hval sep, hval s) { return hpbi_explode(sep, s); }
static hval hp_implode(hval glue, harr *a) {
    hstr *g = hp_val_to_str(glue);
    Buf b;
    buf_init(&b);
    for (size_t i = 0; i < a->len; i++) {
        if (i) buf_write(&b, g->data, g->len);
        hstr *part = hp_val_to_str(a->vals[i]);
        buf_write(&b, part->data, part->len);
    }
    RETS(hp_str_take_buf(&b));
}
hval hpbi_implode(hval glue, hval arr) {
    if (arr.tag == HV_ARR) return hp_implode(glue, arr.u.a);
    return hp_implode(arr, glue.tag == HV_ARR ? glue.u.a : hp_null_arr());
}
hval hpbi_join(hval glue, hval arr) { return hpbi_implode(glue, arr); }
hval hpbi_nl2br(hval s) {
    RETS(hp_replace_c(hp_val_to_str(s), "\n", "<br />\n"));
}
/* PHP unset($arr[$k]): remove by key from an array held in a variable.
 * The codegen passes the variable's harr* directly. */
hval hpbi_unset(harr *a, hval key) {
    if (a) hp_arr_unset(a, key);
    return hp_null;
}

/* unset($obj->arr[$key]) / unset($this->arr[$key]): the object's field is
 * a typed struct member holding harr*; cow-write the delete in place. */
hval hpbi_unset_slot(harr **slot, hval key) {
    if (!slot || !*slot) return hp_of_bool(false);
    *slot = hp_arr_cow(*slot);
    hp_arr_unset(*slot, key);
    return hp_of_bool(true);
}

/* isset() codegen path for `$var[key]`: the variable's slot holds a raw
 * harr* (compiler representation), so read the element hval without any
 * type projection — a missing key yields hp_null => isset == false. */
hval hpbi_isset_raw(harr **slot, hval key) {
    if (!slot || !*slot) RETB(false);
    /* PHP: a key holding null is not set (array_key_exists says it exists) */
    RETB(hp_arr_get(*slot, key).tag != HV_NULL);
}

/* isset() codegen path for a mixed (hval) variable: the slot boxes the
 * array, so unwrap it (an unboxed harr* here would be a garbage tag). */
hval hpbi_isset_val(hval *slot, hval key) {
    if (!slot || slot->tag != HV_ARR || !slot->u.a) RETB(false);
    RETB(hp_arr_get(slot->u.a, key).tag != HV_NULL);
}

hval hpbi_isset_index(hval container, hval key) {
    if (container.tag == HV_ARR) RETB(hp_arr_has(container.u.a, key));
    if (container.tag == HV_STR && key.tag == HV_INT && container.u.s &&
        key.u.i >= 0 && (uint64_t)key.u.i < (uint64_t)container.u.s->len)
        RETB(true);
    RETB(false);
}

hval hpbi_number_format(hval n, hval dec, hval dsep, hval tsep) {
    int64_t d = hp_val_to_int(dec);
    if (d < 0) d = 0;
    if (d > 18) d = 18;
    const char *ds = dsep.tag == HV_STR && dsep.u.s->len ? dsep.u.s->data : ".";
    const char *ts = tsep.tag == HV_STR ? tsep.u.s->data : ",";
    char buf[64];
    snprintf(buf, sizeof buf, "%.*f", (int)d, hp_val_to_float(n));
    /* insert thousands separators into the integer part */
    char out[96];
    size_t oi = 0, bi = 0;
    if (buf[0] == '-') out[oi++] = buf[bi++];
    const char *dot = strchr(buf + bi, '.');
    size_t intlen = dot ? (size_t)(dot - (buf + bi)) : strlen(buf + bi);
    for (size_t k = 0; k < intlen; k++) {
        if (k && (intlen - k) % 3 == 0 && ts[0]) out[oi++] = ts[0];
        out[oi++] = buf[bi + k];
    }
    if (dot) snprintf(out + oi, sizeof out - oi, "%s%s", ds, dot + 1);
    else out[oi] = 0;
    RETS(hp_str_cstr(out));
}
hval hpbi_json_encode(hval v) {
    /* minimal JSON serializer */
    Buf b;
    buf_init(&b);
    hbuf_json_r(v, &b);
    char *s = hbuf_take(&b);
    hstr *r = hp_str_cstr(s);
    free(s);
    return hp_of_str(r);
}
static void hbuf_json_r(hval v, Buf *b) {
    switch (v.tag) {
    case HV_NULL: hbuf_puts(b, "null"); break;
    case HV_BOOL: hbuf_puts(b, v.u.b ? "true" : "false"); break;
    case HV_INT: hbuf_printf(b, "%lld", (long long)v.u.i); break;
    case HV_FLOAT: hbuf_printf(b, "%g", v.u.f); break;
    case HV_STR:
        hbuf_puts(b, "\"");
        for (size_t i = 0; i < v.u.s->len; i++) {
            char c = v.u.s->data[i];
            if (c == '"' || c == '\\') hbuf_printf(b, "\\%c", c);
            else if (c == '\n') hbuf_puts(b, "\\n");
            else if (c == '\t') hbuf_puts(b, "\\t");
            else hbuf_putc(b, c);
        }
        hbuf_puts(b, "\"");
        break;
    case HV_ARR: {
        harr *a = v.u.a;
        if (a->is_map) {
            hbuf_puts(b, "{");
            for (size_t i = 0; i < a->len; i++) {
                if (i) hbuf_puts(b, ",");
                hval k = a->keys[i];
                hstr *ks = hp_val_to_str(k);
                hbuf_printf(b, "\"%.*s\":", (int)ks->len, ks->data);
                if (k.tag != HV_STR) hp_str_free(ks);
                hbuf_json_r(a->vals[i], b);
            }
            hbuf_puts(b, "}");
        } else {
            hbuf_puts(b, "[");
            for (size_t i = 0; i < a->len; i++) {
                if (i) hbuf_puts(b, ",");
                hbuf_json_r(a->vals[i], b);
            }
            hbuf_puts(b, "]");
        }
        break;
    }
    default: hbuf_puts(b, "null"); break;
    }
}
/* ---- json_decode: a small recursive-descent JSON parser ---- */
typedef struct {
    const char *p;
    size_t len, pos;
} JsonP;

static void jp_ws(JsonP *j) {
    while (j->pos < j->len) {
        char c = j->p[j->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') j->pos++;
        else break;
    }
}

static hval jp_value(JsonP *j);

static hstr *jp_string(JsonP *j) {
    if (j->pos >= j->len || j->p[j->pos] != '"') return hp_str_lit("");
    j->pos++;
    Buf b;
    buf_init(&b);
    while (j->pos < j->len && j->p[j->pos] != '"') {
        char c = j->p[j->pos++];
        if (c == '\\' && j->pos < j->len) {
            char e = j->p[j->pos++];
            switch (e) {
            case 'n': buf_putc(&b, '\n'); break;
            case 't': buf_putc(&b, '\t'); break;
            case 'r': buf_putc(&b, '\r'); break;
            case 'b': buf_putc(&b, '\b'); break;
            case 'f': buf_putc(&b, '\f'); break;
            case 'u': {
                /* \uXXXX — emit UTF-8 (BMP only) */
                if (j->pos + 4 <= j->len) {
                    unsigned cp = 0;
                    for (int k = 0; k < 4; k++) {
                        char h = j->p[j->pos++];
                        cp = cp * 16 + (h >= '0' && h <= '9' ? (unsigned)(h - '0')
                             : h >= 'a' && h <= 'f' ? (unsigned)(h - 'a' + 10)
                             : (unsigned)(h - 'A' + 10));
                    }
                    if (cp < 0x80) buf_putc(&b, (char)cp);
                    else if (cp < 0x800) {
                        buf_putc(&b, (char)(0xC0 | (cp >> 6)));
                        buf_putc(&b, (char)(0x80 | (cp & 63)));
                    } else {
                        buf_putc(&b, (char)(0xE0 | (cp >> 12)));
                        buf_putc(&b, (char)(0x80 | ((cp >> 6) & 63)));
                        buf_putc(&b, (char)(0x80 | (cp & 63)));
                    }
                }
                break;
            }
            default: buf_putc(&b, e); break;
            }
        } else buf_putc(&b, c);
    }
    if (j->pos < j->len) j->pos++;  /* closing quote */
    return hp_str_take_buf(&b);
}

static hval jp_value(JsonP *j) {
    jp_ws(j);
    if (j->pos >= j->len) return hp_null;
    char c = j->p[j->pos];
    if (c == '{') {
        j->pos++;
        harr *m = hp_arr_new();
        m->is_map = true;
        jp_ws(j);
        if (j->pos < j->len && j->p[j->pos] == '}') { j->pos++; return hp_of_arr(m); }
        for (;;) {
            jp_ws(j);
            hstr *k = jp_string(j);
            jp_ws(j);
            if (j->pos < j->len && j->p[j->pos] == ':') j->pos++;
            hval v = jp_value(j);
            hp_arr_set(m, hp_of_str(k), v);
            jp_ws(j);
            if (j->pos < j->len && j->p[j->pos] == ',') { j->pos++; continue; }
            if (j->pos < j->len && j->p[j->pos] == '}') { j->pos++; }
            break;
        }
        return hp_of_arr(m);
    }
    if (c == '[') {
        j->pos++;
        harr *a = hp_arr_new();
        jp_ws(j);
        if (j->pos < j->len && j->p[j->pos] == ']') { j->pos++; return hp_of_arr(a); }
        for (;;) {
            hval v = jp_value(j);
            hp_arr_push(a, v);
            jp_ws(j);
            if (j->pos < j->len && j->p[j->pos] == ',') { j->pos++; continue; }
            if (j->pos < j->len && j->p[j->pos] == ']') { j->pos++; }
            break;
        }
        return hp_of_arr(a);
    }
    if (c == '"') return hp_of_str(jp_string(j));
    if (j->pos + 4 <= j->len && memcmp(j->p + j->pos, "true", 4) == 0) { j->pos += 4; return hp_of_bool(true); }
    if (j->pos + 5 <= j->len && memcmp(j->p + j->pos, "false", 5) == 0) { j->pos += 5; return hp_of_bool(false); }
    if (j->pos + 4 <= j->len && memcmp(j->p + j->pos, "null", 4) == 0) { j->pos += 4; return hp_null; }
    /* number: int if no ./eE */
    {
        size_t st = j->pos;
        bool isf = false;
        while (j->pos < j->len) {
            char d = j->p[j->pos];
            if (d == '.' || d == 'e' || d == 'E') isf = true;
            else if (!((d >= '0' && d <= '9') || d == '-' || d == '+')) break;
            j->pos++;
        }
        if (j->pos == st) { j->pos++; return hp_null; }  /* garbage: skip */
        char tmp[64];
        size_t n = j->pos - st < 63 ? j->pos - st : 63;
        memcpy(tmp, j->p + st, n);
        tmp[n] = 0;
        if (isf) return hp_of_float(strtod(tmp, NULL));
        return hp_of_int((int64_t)strtoll(tmp, NULL, 10));
    }
}

hval hpbi_json_decode(hval s) {
    hstr *x = hp_val_to_str(s);
    JsonP j = { x->data, x->len, 0 };
    return jp_value(&j);
}
/* ---------------- message digests ----------------
 * Real SHA-1/MD5 (the WebSocket handshake in lib/websocket.hphp needs a
 * correct SHA-1: browsers verify Sec-WebSocket-Accept). Both accept PHP's
 * optional second argument: true = raw 20/16 bytes instead of hex. */

static uint32_t rotl32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

static void hp_sha1_digest(const unsigned char *msg, size_t len, unsigned char out[20]) {
    uint32_t h[5] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u };
    size_t padded = ((len + 8) / 64 + 1) * 64;    /* always one pad byte + 8 */
    unsigned char *m = (unsigned char *)calloc(padded, 1);
    memcpy(m, msg, len);
    m[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) m[padded - 8 + i] = (unsigned char)(bits >> (56 - 8 * i));
    for (size_t off = 0; off < padded; off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = ((uint32_t)m[off + i * 4] << 24) | ((uint32_t)m[off + i * 4 + 1] << 16) |
                   ((uint32_t)m[off + i * 4 + 2] << 8) | (uint32_t)m[off + i * 4 + 3];
        for (int i = 16; i < 80; i++) w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | ((~b) & d);        k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDCu; }
            else             { f = b ^ c ^ d;                    k = 0xCA62C1D6u; }
            uint32_t tmp = rotl32(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rotl32(b, 30); b = a; a = tmp;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    free(m);
    for (int i = 0; i < 5; i++) {
        out[i * 4]     = (unsigned char)(h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)h[i];
    }
}

static void hp_sha1_pub(const unsigned char *msg, size_t len, unsigned char out[20]) {
    hp_sha1_digest(msg, len, out);
}

static void hp_md5_digest(const unsigned char *msg, size_t len, unsigned char out[16]) {
    static const uint32_t K[64] = {
        0xd76aa478u,0xe8c7b756u,0x242070dbu,0xc1bdceeeu,0xf57c0fafu,0x4787c62au,0xa8304613u,0xfd469501u,
        0x698098d8u,0x8b44f7afu,0xffff5bb1u,0x895cd7beu,0x6b901122u,0xfd987193u,0xa679438eu,0x49b40821u,
        0xf61e2562u,0xc040b340u,0x265e5a51u,0xe9b6c7aau,0xd62f105du,0x02441453u,0xd8a1e681u,0xe7d3fbc8u,
        0x21e1cde6u,0xc33707d6u,0xf4d50d87u,0x455a14edu,0xa9e3e905u,0xfcefa3f8u,0x676f02d9u,0x8d2a4c8au,
        0xfffa3942u,0x8771f681u,0x6d9d6122u,0xfde5380cu,0xa4beea44u,0x4bdecfa9u,0xf6bb4b60u,0xbebfbc70u,
        0x289b7ec6u,0xeaa127fau,0xd4ef3085u,0x04881d05u,0xd9d4d039u,0xe6db99e5u,0x1fa27cf8u,0xc4ac5665u,
        0xf4292244u,0x432aff97u,0xab9423a7u,0xfc93a039u,0x655b59c3u,0x8f0ccc92u,0xffeff47du,0x85845dd1u,
        0x6fa87e4fu,0xfe2ce6e0u,0xa3014314u,0x4e0811a1u,0xf7537e82u,0xbd3af235u,0x2ad7d2bbu,0xeb86d391u };
    static const int S[64] = {
        7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
        5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
        4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
        6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21 };
    uint32_t h[4] = { 0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u };
    size_t padded = ((len + 8) / 64 + 1) * 64;
    unsigned char *m = (unsigned char *)calloc(padded, 1);
    memcpy(m, msg, len);
    m[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) m[padded - 8 + i] = (unsigned char)(bits >> (8 * i));
    for (size_t off = 0; off < padded; off += 64) {
        uint32_t w[16];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)m[off + i * 4] | ((uint32_t)m[off + i * 4 + 1] << 8) |
                   ((uint32_t)m[off + i * 4 + 2] << 16) | ((uint32_t)m[off + i * 4 + 3] << 24);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 64; i++) {
            uint32_t f; int g;
            if (i < 16)      { f = (b & c) | (~b & d);         g = i; }
            else if (i < 32) { f = (d & b) | (~d & c);         g = (5 * i + 1) % 16; }
            else if (i < 48) { f = b ^ c ^ d;                  g = (3 * i + 5) % 16; }
            else             { f = c ^ (b | ~d);               g = (7 * i) % 16; }
            uint32_t tmp = d;
            d = c; c = b;
            b = b + rotl32(a + f + K[i] + w[g], S[i]);
            a = tmp;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    }
    free(m);
    for (int i = 0; i < 4; i++) {
        out[i * 4]     = (unsigned char)h[i];
        out[i * 4 + 1] = (unsigned char)(h[i] >> 8);
        out[i * 4 + 2] = (unsigned char)(h[i] >> 16);
        out[i * 4 + 3] = (unsigned char)(h[i] >> 24);
    }
}

static hstr *hp_hex(const unsigned char *d, size_t n) {
    static const char *hexd = "0123456789abcdef";
    char *s = (char *)malloc(n * 2 + 1);
    for (size_t i = 0; i < n; i++) {
        s[i * 2] = hexd[d[i] >> 4];
        s[i * 2 + 1] = hexd[d[i] & 15];
    }
    s[n * 2] = 0;
    hstr *out = hp_str_new(s, n * 2);
    free(s);
    return out;
}

hval hpbi_sha1(hval s, hval raw) {
    hstr *x = hp_val_to_str(s);
    unsigned char d[20];
    hp_sha1_digest((const unsigned char *)x->data, x->len, d);
    if (hp_val_to_bool(raw)) return hp_of_str(hp_str_new((const char *)d, 20));
    return hp_of_str(hp_hex(d, 20));
}

hval hpbi_md5(hval s, hval raw) {
    hstr *x = hp_val_to_str(s);
    unsigned char d[16];
    hp_md5_digest((const unsigned char *)x->data, x->len, d);
    if (hp_val_to_bool(raw)) return hp_of_str(hp_str_new((const char *)d, 16));
    return hp_of_str(hp_hex(d, 16));
}
hval hpbi_crc32(hval s) {
    hstr *x = hp_val_to_str(s);
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < x->len; i++) {
        crc ^= (unsigned char)x->data[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    RETI(~(int64_t)crc & 0xFFFFFFFFll);
}
static hstr *hp_b64_encode(hstr *in) {
    static const char *tab = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    Buf b;
    buf_init(&b);
    size_t i = 0;
    while (i + 2 < in->len) {
        uint32_t n = ((unsigned char)in->data[i] << 16) | ((unsigned char)in->data[i + 1] << 8) | (unsigned char)in->data[i + 2];
        hbuf_putc(&b, tab[(n >> 18) & 63]); hbuf_putc(&b, tab[(n >> 12) & 63]);
        hbuf_putc(&b, tab[(n >> 6) & 63]); hbuf_putc(&b, tab[n & 63]);
        i += 3;
    }
    size_t rem = in->len - i;
    if (rem == 1) {
        uint32_t n = (unsigned char)in->data[i] << 16;
        hbuf_putc(&b, tab[(n >> 18) & 63]); hbuf_putc(&b, tab[(n >> 12) & 63]);
        hbuf_puts(&b, "==");
    } else if (rem == 2) {
        uint32_t n = ((unsigned char)in->data[i] << 16) | ((unsigned char)in->data[i + 1] << 8);
        hbuf_putc(&b, tab[(n >> 18) & 63]); hbuf_putc(&b, tab[(n >> 12) & 63]);
        hbuf_putc(&b, tab[(n >> 6) & 63]); hbuf_putc(&b, '=');
    }
    char *s = hbuf_take(&b);
    hstr *r = hp_str_cstr(s);
    free(s);
    return r;
}
static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
static hstr *hp_b64_decode(hstr *in) {
    Buf b;
    buf_init(&b);
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < in->len; i++) {
        int v = b64_val(in->data[i]);
        if (v < 0) continue;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            hbuf_putc(&b, (char)((acc >> bits) & 0xff));
        }
    }
    size_t blen = b.len;
    char *s = hbuf_take(&b);
    hstr *r = hp_str_new(s, blen);
    free(s);
    return r;
}
hval hpbi_base64_encode(hval s) { RETS(hp_b64_encode(hp_val_to_str(s))); }
hval hpbi_base64_decode(hval s) { RETS(hp_b64_decode(hp_val_to_str(s))); }
hval hpbi_urlencode(hval s) {
    hstr *x = hp_val_to_str(s);
    Buf b;
    buf_init(&b);
    for (size_t i = 0; i < x->len; i++) {
        unsigned char c = (unsigned char)x->data[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') buf_putc(&b, (char)c);
        else if (c == ' ') buf_putc(&b, '+');
        else buf_printf(&b, "%%%02X", c);
    }
    RETS(hp_str_take_buf(&b));
}
hval hpbi_urldecode(hval s) {
    hstr *x = hp_val_to_str(s);
    Buf b;
    buf_init(&b);
    for (size_t i = 0; i < x->len; i++) {
        if (x->data[i] == '+') buf_putc(&b, ' ');
        else if (x->data[i] == '%' && i + 2 < x->len) {
            char hex[3] = { x->data[i + 1], x->data[i + 2], 0 };
            buf_putc(&b, (char)strtol(hex, NULL, 16));
            i += 2;
        } else buf_putc(&b, x->data[i]);
    }
    RETS(hp_str_take_buf(&b));
}
hval hpbi_htmlentities(hval s) {
    hstr *x = hp_val_to_str(s);
    Buf b;
    buf_init(&b);
    for (size_t i = 0; i < x->len; i++) {
        switch (x->data[i]) {
        case '&':  buf_puts(&b, "&amp;"); break;
        case '<':  buf_puts(&b, "&lt;"); break;
        case '>':  buf_puts(&b, "&gt;"); break;
        case '"':  buf_puts(&b, "&quot;"); break;
        default:   buf_putc(&b, x->data[i]); break;
        }
    }
    RETS(hp_str_take_buf(&b));
}
hval hpbi_htmlspecialchars(hval s) { return hpbi_htmlentities(s); }
hval hpbi_sprintf(hval fmt, hval args) {
    hstr *f = hp_val_to_str(fmt);
    harr *a = args.tag == HV_ARR ? args.u.a : hp_arr_of(1, &args);
    size_t ai = 0;
    Buf b;
    buf_init(&b);
    for (size_t i = 0; i < f->len; i++) {
        if (f->data[i] != '%') { buf_putc(&b, f->data[i]); continue; }
        size_t j = i + 1;
        if (j >= f->len) { buf_putc(&b, '%'); break; }
        if (f->data[j] == '%') { buf_putc(&b, '%'); i = j; continue; }
        /* parse %[flags][width][.precision]conv */
        char spec[32];
        size_t sp = 0;
        spec[sp++] = '%';
        while (j < f->len && sp < 24 && strchr("-+ 0#", f->data[j])) spec[sp++] = f->data[j++];
        while (j < f->len && sp < 24 && f->data[j] >= '0' && f->data[j] <= '9') spec[sp++] = f->data[j++];
        if (j < f->len && f->data[j] == '.') {
            spec[sp++] = '.';
            j++;
            while (j < f->len && sp < 24 && f->data[j] >= '0' && f->data[j] <= '9') spec[sp++] = f->data[j++];
        }
        char conv = j < f->len ? f->data[j] : 0;
        if (!conv) { buf_putc(&b, '%'); continue; }
        i = j;
        spec[sp] = 0;
        hval v = ai < a->len ? a->vals[ai++] : hp_null;
        switch (conv) {
        case 'd': case 'i': case 'u': {
            char cs[8]; snprintf(cs, sizeof cs, "ll%c", conv == 'i' ? 'd' : conv);
            char out[64]; snprintf(out, sizeof out, "%s%s", spec, cs);
            buf_printf(&b, out, (long long)hp_val_to_int(v));
            break;
        }
        case 'x': case 'X': case 'o': {
            char out[64]; snprintf(out, sizeof out, "%sll%c", spec, conv);
            buf_printf(&b, out, (unsigned long long)hp_val_to_int(v));
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
            char out[64]; snprintf(out, sizeof out, "%s%c", spec, conv);
            buf_printf(&b, out, hp_val_to_float(v));
            break;
        }
        case 's': {
            hstr *s = hp_val_to_str(v);
            if (sp > 0 && spec[sp - 1] == '.') {
                char out[64]; snprintf(out, sizeof out, "%s.*s", spec);
                long prec = 0;
                sscanf(spec + 1, "%*[^.]%*c%ld", &prec);
                long n = prec < 0 || prec > (long)s->len ? (long)s->len : prec;
                buf_write(&b, s->data, (size_t)n);
            } else {
                buf_write(&b, s->data, s->len);
            }
            break;
        }
        case 'c': buf_putc(&b, (char)hp_val_to_int(v)); break;
        case 'b': {
            int64_t n = hp_val_to_int(v);
            char bits[65];
            bits[64] = 0;
            for (int k = 63; k >= 0; k--) bits[63 - k] = (n >> k) & 1 ? '1' : '0';
            buf_puts(&b, bits);
            break;
        }
        default:
            buf_puts(&b, spec);
            buf_putc(&b, conv);
            break;
        }
    }
    RETS(hp_str_take_buf(&b));
}
hval hpbi_printf(hval fmt, hval args) {
    hval r = hpbi_sprintf(fmt, args);
    hstr *s = r.u.s;
    fwrite(s->data, 1, s->len, stdout);
    RETI((int64_t)s->len);
}
hval hpbi_ord(hval s) {
    hstr *x = hp_val_to_str(s);
    RETI(x->len ? (unsigned char)x->data[0] : 0);
}
hval hpbi_chr(hval n) {
    char c = (char)hp_val_to_int(n);
    RETS(hp_str_new(&c, 1));
}
hval hpbi_bin2hex(hval s) {
    hstr *x = hp_val_to_str(s);
    Buf b;
    buf_init(&b);
    for (size_t i = 0; i < x->len; i++) buf_printf(&b, "%02x", (unsigned char)x->data[i]);
    RETS(hp_str_take_buf(&b));
}
hval hpbi_hex2bin(hval s) {
    hstr *x = hp_val_to_str(s);
    Buf b;
    buf_init(&b);
    for (size_t i = 0; i + 1 < x->len; i += 2) {
        char hex[3] = { x->data[i], x->data[i + 1], 0 };
        buf_putc(&b, (char)strtol(hex, NULL, 16));
    }
    RETS(hp_str_take_buf(&b));
}
hval hpbi_str_split(hval s, hval n) {
    hstr *x = hp_val_to_str(s);
    int64_t k = hp_val_to_int(n);
    if (k <= 0) k = 1;
    harr *r = hp_arr_new();
    for (size_t i = 0; i < x->len; i += (size_t)k) {
        size_t l = x->len - i < (size_t)k ? x->len - i : (size_t)k;
        hp_arr_push_take(r, hp_of_str(hp_str_new(x->data + i, l)));
    }
    return hp_of_arr(r);
}
hval hpbi_ucwords(hval s) {
    hstr *r = hp_str_copy(hp_val_to_str(s));
    bool up = true;
    for (size_t i = 0; i < r->len; i++) {
        if (isspace((unsigned char)r->data[i])) up = true;
        else if (up) { r->data[i] = (char)toupper((unsigned char)r->data[i]); up = false; }
    }
    RETS(r);
}
hval hpbi_wordwrap(hval s, hval w) {
    hstr *x = hp_val_to_str(s);
    size_t width = (size_t)hp_val_to_int(w);
    if (width == 0) RETS(hp_str_copy(x));
    Buf b = {0}, word = {0};
    size_t line = 0;
    for (size_t i = 0; i <= x->len; i++) {
        char c = i < x->len ? x->data[i] : ' ';
        if (c != ' ' && c != '\n' && i < x->len) { hbuf_putc(&word, c); continue; }
        /* delimiter (or end): flush the pending word */
        if (word.len > 0) {
            if (line > 0 && line + 1 + word.len > width) {
                hbuf_putc(&b, '\n');           /* word would overflow: wrap first */
                line = 0;
            } else if (line > 0) {
                hbuf_putc(&b, ' ');
                line++;
            }
            hbuf_write(&b, word.data, word.len);
            line += word.len;
            word.len = 0;
        }
        if (c == '\n') { hbuf_putc(&b, '\n'); line = 0; }
    }
    hstr *out = hp_str_take_buf(&b);
    RETS(out);
}
hval hpbi_similar_text(hval a, hval b) {
    hstr *x = hp_val_to_str(a), *y = hp_val_to_str(b);
    size_t same = 0;
    for (size_t i = 0; i < x->len; i++)
        for (size_t j = 0; j < y->len; j++)
            if (x->data[i] == y->data[j]) { same++; break; }
    RETI((int64_t)same);
}
hval hpbi_levenshtein(hval a, hval b) {
    hstr *x = hp_val_to_str(a), *y = hp_val_to_str(b);
    size_t n = x->len, m = y->len;
    static size_t dp[512];
    if (m + 1 > 512) RETI(-1);
    for (size_t j = 0; j <= m; j++) dp[j] = j;
    for (size_t i = 1; i <= n; i++) {
        size_t prev = dp[0], cur;
        dp[0] = i;
        for (size_t j = 1; j <= m; j++) {
            cur = dp[j];
            size_t cost = x->data[i - 1] == y->data[j - 1] ? 0 : 1;
            dp[j] = dp[j - 1] + 1 < dp[j] + 1 ? dp[j - 1] + 1 : dp[j] + 1;
            if (prev + cost < dp[j]) dp[j] = prev + cost;
            prev = cur;
        }
    }
    RETI((int64_t)dp[m]);
}

/* ---------- arrays ---------- */
hval hpbi_array_keys(hval a) {
    if (a.tag != HV_ARR) return hp_of_arr(hp_arr_new());
    return hp_of_arr(hp_arr_keys(a.u.a));
}
hval hpbi_array_values(hval a) {
    if (a.tag != HV_ARR) return hp_of_arr(hp_arr_new());
    return hp_of_arr(hp_arr_values(a.u.a));
}
hval hpbi_array_merge(hval a, hval b, hval c, hval d) {
    /* PHP: variadic array_merge(...); codegen pads to 4. String keys
     * overwrite, integer keys are renumbered and appended - also those of
     * the first array. */
    if (a.tag != HV_ARR) return a;
    harr *out = hp_arr_new();
    hval all[4] = { a, b, c, d };
    for (int i = 0; i < 4; i++) {
        if (all[i].tag != HV_ARR || !all[i].u.a) continue;
        harr *x = all[i].u.a;
        for (size_t k = 0; k < x->len; k++) {
            if (x->is_map && x->keys && x->keys[k].tag == HV_STR)
                hp_arr_set(out, x->keys[k], x->vals[k]);
            else
                hp_arr_push(out, x->vals[k]);
        }
    }
    return hp_of_arr(out);
}
hval hpbi_array_slice(hval a, hval off, hval len) {
    if (a.tag != HV_ARR) return hp_of_arr(hp_arr_new());
    harr *x = a.u.a;
    int64_t o = hp_val_to_int(off);
    o = o < 0 ? o + (int64_t)x->len : o;
    int64_t l = len.tag == HV_NULL ? (int64_t)x->len - o : hp_val_to_int(len);
    if (l < 0) l = (int64_t)x->len - o + l;
    if (l < 0) l = 0;
    return hp_of_arr(hp_arr_slice(x, o, o + l));
}
hval hpbi_array_reverse(hval a) {
    if (a.tag != HV_ARR) return a;
    return hp_of_arr(hp_arr_reverse(a.u.a));
}
hval hpbi_array_sum(hval a) {
    if (a.tag != HV_ARR) RETI(0);
    return hp_arr_sum(a.u.a);
}
hval hpbi_array_product(hval a) {
    if (a.tag != HV_ARR) RETI(0);
    double p = 1.0;
    harr *x = a.u.a;
    for (size_t i = 0; i < x->len; i++) p *= hp_val_to_float(x->vals[i]);
    return hp_of_float(p);
}
hval hpbi_array_unique(hval a) {
    if (a.tag != HV_ARR) return a;
    return hp_of_arr(hp_arr_unique(a.u.a));
}
hval hpbi_in_array(hval needle, hval hay) {
    if (hay.tag != HV_ARR) RETB(false);
    RETB(hp_arr_in(hay.u.a, needle));
}
hval hpbi_array_search(hval needle, hval hay) {
    if (hay.tag != HV_ARR) RETB(false);
    harr *x = hay.u.a;
    for (size_t i = 0; i < x->len; i++)
        if (hp_val_eq(x->vals[i], needle))
            return x->is_map ? x->keys[i] : hp_of_int((int64_t)i);
    RETB(false);
}
hval hpbi_array_key_exists(hval k, hval a) {
    if (a.tag != HV_ARR) RETB(false);
    RETB(hp_arr_has(a.u.a, k));
}
hval hpbi_isset(hval v) { RETB(v.tag != HV_NULL); }
hval hpbi_range(hval lo, hval hi) {
    return hp_of_arr(hp_range(hp_val_to_int(lo), hp_val_to_int(hi)));
}
hval hpbi_array_map(hval fn, hval a) {
    if (a.tag != HV_ARR) return hp_of_arr(hp_arr_new());
    harr *x = a.u.a;
    harr *r = hp_arr_new();
    for (size_t i = 0; i < x->len; i++) {
        hval v = x->vals[i];
        hval out = hp_null;
        if (fn.tag == HV_CLO) {
            /* PHP: the callback receives (value, key). 1-param closures only
             * read slot 0, so always passing two slots is safe. */
            hval args2[2] = { v, x->is_map ? x->keys[i] : hp_of_int((int64_t)i) };
            out = hp_closure_call(fn.u.p, 2, args2);
        }
        hp_arr_push(r, out);
    }
    return hp_of_arr(r);
}
hval hpbi_array_filter(hval a, hval fn) {
    if (a.tag != HV_ARR) return hp_of_arr(hp_arr_new());
    harr *r = hp_arr_new();
    for (size_t i = 0; i < a.u.a->len; i++) {
        hval v = a.u.a->vals[i];
        hval keep = hp_null;
        if (fn.tag == HV_CLO) {
            hval args2[2] = { v, a.u.a->is_map ? a.u.a->keys[i] : hp_of_int((int64_t)i) };
            keep = hp_closure_call(fn.u.p, 2, args2);
        } else {
            keep = v;   /* PHP: no callback = drop falsy entries */
        }
        if (hp_val_to_bool(keep))
            hp_arr_push(r, v);
    }
    return hp_of_arr(r);
}
hval hpbi_array_reduce(hval a, hval fn, hval init) {
    if (a.tag != HV_ARR) return init;
    hval acc = init;
    for (size_t i = 0; i < a.u.a->len; i++) {
        hval args[2] = { acc, a.u.a->vals[i] };
        acc = (fn.tag == HV_CLO) ? hp_closure_call(fn.u.p, 2, args) : acc;
    }
    return acc;
}
hval hpbi_array_flip(hval a) {
    harr *r = hp_arr_new();
    r->is_map = true;
    if (a.tag == HV_ARR) {
        harr *x = a.u.a;
        for (size_t i = 0; i < x->len; i++)
            hp_arr_set(r, x->vals[i], x->is_map ? x->keys[i] : hp_of_int((int64_t)i));
    }
    return hp_of_arr(r);
}
hval hpbi_array_fill(hval start, hval n, hval v) {
    harr *r = hp_arr_new();
    int64_t st = hp_val_to_int(start), cnt = hp_val_to_int(n);
    for (int64_t i = 0; i < cnt; i++)
        hp_arr_set(r, hp_of_int(st + i), v);
    return hp_of_arr(r);
}
hval hpbi_array_combine(hval keys, hval vals) {
    harr *r = hp_arr_new();
    r->is_map = true;
    if (keys.tag == HV_ARR && vals.tag == HV_ARR) {
        harr *k = keys.u.a, *v = vals.u.a;
        size_t n = k->len < v->len ? k->len : v->len;
        for (size_t i = 0; i < n; i++)
            hp_arr_set(r, k->vals[i], v->vals[i]);
    }
    return hp_of_arr(r);
}
hval hpbi_array_diff(hval a, hval b, hval c, hval d) {
    /* PHP: array_diff($a, ...$others) — codegen pads to 4 */
    harr *r = hp_arr_new();
    if (a.tag == HV_ARR) {
        hval others[3] = { b, c, d };
        for (size_t i = 0; i < a.u.a->len; i++) {
            bool in_any = false;
            for (int j = 0; j < 3 && !in_any; j++)
                if (others[j].tag == HV_ARR && hp_arr_in(others[j].u.a, a.u.a->vals[i]))
                    in_any = true;
            if (!in_any)
                hp_arr_push(r, a.u.a->vals[i]);
        }
    }
    return hp_of_arr(r);
}
hval hpbi_array_intersect(hval a, hval b, hval c, hval d) {
    harr *r = hp_arr_new();
    if (a.tag == HV_ARR) {
        hval others[3] = { b, c, d };
        for (size_t i = 0; i < a.u.a->len; i++) {
            bool in_all = true;
            for (int j = 0; j < 3 && in_all; j++)
                if (others[j].tag == HV_ARR && !hp_arr_in(others[j].u.a, a.u.a->vals[i]))
                    in_all = false;
            if (in_all)
                hp_arr_push(r, a.u.a->vals[i]);
        }
    }
    return hp_of_arr(r);
}
hval hpbi_array_push(hval a, hval v) {
    if (a.tag != HV_ARR) RETI(0);
    hp_arr_push(a.u.a, v);
    RETI((int64_t)a.u.a->len);
}
hval hpbi_array_pop(hval a) {
    if (a.tag != HV_ARR) return hp_null;
    return hp_arr_pop(a.u.a);
}
hval hpbi_end(hval a) {
    if (a.tag != HV_ARR || a.u.a->len == 0) return hp_null;
    return a.u.a->vals[a.u.a->len - 1];
}
hval hpbi_reset(hval a) {
    if (a.tag != HV_ARR || a.u.a->len == 0) return hp_null;
    return a.u.a->vals[0];
}
hval hpbi_array_shift(hval a) {
    if (a.tag != HV_ARR) return hp_null;
    return hp_arr_shift(a.u.a);
}
hval hpbi_array_unshift(hval a, hval v) {
    if (a.tag != HV_ARR) RETI(0);
    hp_arr_unshift(a.u.a, v);
    RETI((int64_t)a.u.a->len);
}
/* array_splice(&$a, $offset[, $length[, $replacement]]): removes $length
 * elements at $offset, inserts $replacement there (lists), returns the
 * removed elements. */
hval hpbi_array_splice(hval a, hval off, hval len, hval repl) {
    if (a.tag != HV_ARR || !a.u.a) return hp_of_arr(hp_arr_new());
    harr *x = a.u.a;
    int64_t n = (int64_t)x->len;
    int64_t start = hp_val_to_int(off);
    if (start < 0) start += n;
    if (start < 0) start = 0;
    if (start > n) start = n;
    int64_t cnt = len.tag == HV_NULL ? n - start : hp_val_to_int(len);
    if (cnt < 0) cnt = n - start + cnt;
    if (cnt < 0) cnt = 0;
    if (cnt > n - start) cnt = n - start;
    harr *removed = hp_arr_slice(x, start, start + cnt);   /* retains its values */
    bool insert = repl.tag != HV_NULL && !x->is_map;
    if (cnt > 0 || insert) {
        /* keep what follows the removed range, cut the array at start ... */
        harr *tail = hp_arr_slice(x, start + cnt, n);
        for (int64_t i = start; i < n; i++) {
            hp_val_rc_dec(x->vals[i]);
            if (x->is_map && x->keys) hp_val_rc_dec(x->keys[i]);
        }
        if (x->is_map && x->keys) {
            /* maps: shift the keyed tail down; no insertion */
            x->len = (size_t)start;
            for (size_t i = 0; i < tail->len; i++) hp_arr_set(x, tail->keys ? tail->keys[i] : hp_of_int((int64_t)i), tail->vals[i]);
        } else {
            x->len = (size_t)start;
            x->next_index = start;
            /* ... then append the replacement and the tail */
            if (insert) {
                if (repl.tag == HV_ARR && repl.u.a) {
                    for (size_t i = 0; i < repl.u.a->len; i++) hp_arr_push(x, repl.u.a->vals[i]);
                } else {
                    hp_arr_push(x, repl);
                }
            }
            for (size_t i = 0; i < tail->len; i++) hp_arr_push(x, tail->vals[i]);
        }
        hp_val_rc_dec(hp_of_arr(tail));
    }
    return hp_of_arr(removed);
}
/* array_push($a, $v1, $v2, ...) / array_unshift with more than one value */
hval hpbi_array_push_n(hval a, int64_t n, const hval *vals) {
    if (a.tag != HV_ARR) RETI(0);
    for (int64_t i = 0; i < n; i++) hp_arr_push(a.u.a, vals[i]);
    RETI((int64_t)a.u.a->len);
}
hval hpbi_array_unshift_n(hval a, int64_t n, const hval *vals) {
    if (a.tag != HV_ARR) RETI(0);
    for (int64_t i = n - 1; i >= 0; i--) hp_arr_unshift(a.u.a, vals[i]);   /* keeps their order */
    RETI((int64_t)a.u.a->len);
}
hval hpbi_shuffle(hval a) {
    /* PHP: shuffle mutates the array in place and returns true. */
    if (a.tag != HV_ARR || !a.u.a) RETB(false);
    harr *r = a.u.a;
    for (size_t i = r->len; i > 1; i--) {
        size_t j = (size_t)(rand() % (int64_t)i);
        hval t = r->vals[i - 1];
        r->vals[i - 1] = r->vals[j];
        r->vals[j] = t;
    }
    RETB(true);
}

/* ---------- in-place sorts (sort/rsort/ksort/krsort/asort) ---------- */
/* PHP sorts mutate the array argument (arrays are handles). All sort
 * by hp_val_cmp; k* sorts by keys, others by values. Plain lists have
 * keys == NULL, so only touch keys when is_map (like PHP's sort(),
 * value sorts reindex to 0..n-1). */
/* qsort comparators — O(n log n) so 1M-element sorts stay fast */
static int hp_sgn64(int64_t x) { return x < 0 ? -1 : (x > 0 ? 1 : 0); }
static int hp_qcmp_val_asc(const void *pa, const void *pb) {
    return hp_sgn64(hp_val_cmp(*(const hval *)pa, *(const hval *)pb));
}
static int hp_qcmp_val_desc(const void *pa, const void *pb) {
    return -hp_qcmp_val_asc(pa, pb);
}

typedef struct { hval k, v; } hp_kv;

static int hp_sort_mode;
enum { HP_SORT_VAL_ASC, HP_SORT_VAL_DESC, HP_SORT_KEY_ASC, HP_SORT_KEY_DESC };

static int hp_kv_cmp(const void *pa, const void *pb) {
    const hp_kv *a = pa, *b = pb;
    switch (hp_sort_mode) {
    case HP_SORT_KEY_ASC:   return  hp_sgn64(hp_val_cmp(a->k, b->k));
    case HP_SORT_KEY_DESC:  return -hp_sgn64(hp_val_cmp(a->k, b->k));
    case HP_SORT_VAL_DESC:  return -hp_sgn64(hp_val_cmp(a->v, b->v));
    default:                return  hp_sgn64(hp_val_cmp(a->v, b->v));
    }
}
static int hp_sort_mode;

/* sort values, optionally keeping (key, value) pairs associated.
 * Used by sort/rsort (plain), asort/arsort (assoc by value),
 * ksort/krsort (assoc by key). */
static void hp_sort_pairs(harr *a, bool by_key, bool desc) {
    if (a->len < 2) return;
    bool has_keys = a->is_map && a->keys;
    if (!has_keys) {
        /* plain array: qsort the vals, keys stay implicit 0..n-1 */
        qsort(a->vals, a->len, sizeof(hval),
              desc ? hp_qcmp_val_desc : hp_qcmp_val_asc);
        return;
    }
    hp_sort_mode = by_key ? (desc ? HP_SORT_KEY_DESC : HP_SORT_KEY_ASC)
                          : (desc ? HP_SORT_VAL_DESC : HP_SORT_VAL_ASC);
    hp_kv *tmp = hp_alloc(a->len * sizeof(hp_kv));
    for (size_t i = 0; i < a->len; i++) {
        tmp[i].k = a->keys[i];
        tmp[i].v = a->vals[i];
    }
    qsort(tmp, a->len, sizeof(hp_kv), hp_kv_cmp);
    for (size_t i = 0; i < a->len; i++) {
        a->keys[i] = tmp[i].k;
        a->vals[i] = tmp[i].v;
    }
    free(tmp);
}
hval hpbi_sort(hval a)  { if (a.tag == HV_ARR) hp_sort_pairs(a.u.a, false, false); RETB(true); }
hval hpbi_rsort(hval a) { if (a.tag == HV_ARR) hp_sort_pairs(a.u.a, false, true);  RETB(true); }
hval hpbi_ksort(hval a) { if (a.tag == HV_ARR) hp_sort_pairs(a.u.a, true,  false); RETB(true); }
hval hpbi_krsort(hval a){ if (a.tag == HV_ARR) hp_sort_pairs(a.u.a, true,  true);  RETB(true); }
hval hpbi_asort(hval a) { if (a.tag == HV_ARR) hp_sort_pairs(a.u.a, false, false); RETB(true); }

/* ---------- math ---------- */
hval hpbi_max(hval a) {
    if (a.tag != HV_ARR) return a;
    harr *args = a.u.a;
    if (args->len == 0) return hp_null;
    hval best = args->vals[0];
    for (size_t i = 1; i < args->len; i++)
        if (hp_val_cmp(args->vals[i], best) > 0) best = args->vals[i];
    return best;
}
hval hpbi_min(hval a) {
    if (a.tag != HV_ARR) return a;
    harr *args = a.u.a;
    if (args->len == 0) return hp_null;
    hval best = args->vals[0];
    for (size_t i = 1; i < args->len; i++)
        if (hp_val_cmp(args->vals[i], best) < 0) best = args->vals[i];
    return best;
}
hval hpbi_abs(hval v) {
    if (v.tag == HV_FLOAT) RETF(fabs(v.u.f));
    int64_t i = hp_val_to_int(v);
    RETI(i < 0 ? -i : i);
}
hval hpbi_round(hval v, hval prec, hval mode) {
    /* PHP 8: round($x[, $precision[, $mode]]) — mode 1=HALF_DOWN,
     * 2=HALF_EVEN, 3=HALF_ODD; default HALF_UP. Presision mode halves-to-even.
     * PHP_ROUND_HALF_* constants map to 0..3 in builtins.c. */
    int m = mode.tag == HV_NULL ? 0 : (int)hp_val_to_int(mode);
    if (prec.tag == HV_NULL && m == 0) RETF(round(hp_val_to_float(v)));
    double x = hp_val_to_float(v);
    if (prec.tag == HV_NULL) {
        /* precision NULL but mode given: round to integer with mode */
        double fl = floor(x), diff = x - fl;
        if (diff > 0.5) x = fl + 1;
        else if (diff < 0.5) x = fl;
        else if (m == 2) x = (fmod(fl, 2.0) == 0) ? fl : fl + 1;
        else if (m == 3) x = (fmod(fl, 2.0) != 0) ? fl : fl + 1;
        else if (m == 1) x = fl;
        else x = fl + 1;
        RETF(x);
    }
    double p = pow(10.0, hp_val_to_float(prec));
    double y = x * p;
    double fl = floor(y), diff = y - fl;
    if (diff > 0.5) y = fl + 1;
    else if (diff < 0.5) y = fl;
    else if (m == 2) y = (fmod(fl, 2.0) == 0) ? fl : fl + 1;
    else if (m == 3) y = (fmod(fl, 2.0) != 0) ? fl : fl + 1;
    else if (m == 1) y = fl;
    else y = fl + 1;
    RETF(y / p);
}
hval hpbi_floor(hval v) { RETF(floor(hp_val_to_float(v))); }
hval hpbi_ceil(hval v) { RETF(ceil(hp_val_to_float(v))); }
hval hpbi_sqrt(hval v) { RETF(sqrt(hp_val_to_float(v))); }
hval hpbi_pow(hval a, hval b) { return hp_powv(a, b); }
hval hpbi_intdiv(hval a, hval b) {
    int64_t d = hp_val_to_int(b);
    if (d == 0) hp_throw_str("Division by zero");
    RETI(hp_val_to_int(a) / d);
}
hval hpbi_fmod(hval a, hval b) { RETF(fmod(hp_val_to_float(a), hp_val_to_float(b))); }
hval hpbi_sin(hval v) { RETF(sin(hp_val_to_float(v))); }
hval hpbi_cos(hval v) { RETF(cos(hp_val_to_float(v))); }
hval hpbi_tan(hval v) { RETF(tan(hp_val_to_float(v))); }
hval hpbi_atan(hval v) { RETF(atan(hp_val_to_float(v))); }
hval hpbi_atan2(hval a, hval b) { RETF(atan2(hp_val_to_float(a), hp_val_to_float(b))); }
hval hpbi_asin(hval v) { RETF(asin(hp_val_to_float(v))); }
hval hpbi_acos(hval v) { RETF(acos(hp_val_to_float(v))); }
hval hpbi_log(hval v, hval base) {
    double x = log(hp_val_to_float(v));
    if (base.tag == HV_NULL) RETF(x);
    RETF(x / log(hp_val_to_float(base)));
}
hval hpbi_log2(hval v) { RETF(log2(hp_val_to_float(v))); }
hval hpbi_log10(hval v) { RETF(log10(hp_val_to_float(v))); }
hval hpbi_exp(hval v) { RETF(exp(hp_val_to_float(v))); }
hval hpbi_is_nan(hval v) { RETB(isnan(hp_val_to_float(v))); }
hval hpbi_is_finite(hval v) { RETB(isfinite(hp_val_to_float(v))); }
hval hpbi_is_infinite(hval v) { RETB(isinf(hp_val_to_float(v))); }
hval hpbi_pi(void) { RETF(3.14159265358979323846); }
hval hpbi_deg2rad(hval v) { RETF(hp_val_to_float(v) * 0.017453292519943295); }
hval hpbi_rad2deg(hval v) { RETF(hp_val_to_float(v) * 57.29577951308232); }
hval hpbi_hypot(hval a, hval b) { RETF(hypot(hp_val_to_float(a), hp_val_to_float(b))); }

/* ---------- type tests ---------- */
hval hpbi_is_int(hval v) { RETB(v.tag == HV_INT); }
hval hpbi_is_integer(hval v) { RETB(v.tag == HV_INT); }
hval hpbi_is_float(hval v) { RETB(v.tag == HV_FLOAT); }
hval hpbi_is_string(hval v) { RETB(v.tag == HV_STR); }
hval hpbi_is_bool(hval v) { RETB(v.tag == HV_BOOL); }
hval hpbi_is_array(hval v) { RETB(v.tag == HV_ARR); }
hval hpbi_is_null(hval v) { RETB(v.tag == HV_NULL); }
hval hpbi_is_numeric(hval v) { RETB(hp_num_like(v)); }
hval hpbi_intval(hval v) { RETI(hp_val_to_int(v)); }
hval hpbi_floatval(hval v) { RETF(hp_val_to_float(v)); }
hval hpbi_doubleval(hval v) { RETF(hp_val_to_float(v)); }
hval hpbi_strval(hval v) { RETS(hp_str_copy(hp_val_to_str(v))); }
hval hpbi_boolval(hval v) { RETB(hp_val_to_bool(v)); }
hval hpbi_gettype(hval v) { RETS(hp_str_lit(hp_kind_name(v.tag))); }

/* ---------- dump ---------- */
static void hp_dump_r(hval v, Buf *b, int depth);
static void hp_dump_indent(Buf *b, int depth) {
    for (int i = 0; i < depth; i++) buf_puts(b, "  ");
}
static void hp_dump_r(hval v, Buf *b, int depth) {
    switch (v.tag) {
    case HV_NULL: buf_puts(b, "NULL"); break;
    case HV_INT: buf_printf(b, "int(%lld)", (long long)v.u.i); break;
    case HV_FLOAT: buf_printf(b, "float(%g)", v.u.f); break;
    case HV_BOOL: buf_puts(b, v.u.b ? "bool(true)" : "bool(false)"); break;
    case HV_STR: buf_printf(b, "string(%zu) \"%.*s\"", v.u.s->len, (int)v.u.s->len, v.u.s->data); break;
    case HV_ARR: {
        harr *a = v.u.a;
        buf_printf(b, "array(%zu) {\n", a->len);
        for (size_t i = 0; i < a->len; i++) {
            hp_dump_indent(b, depth + 1);
            if (a->is_map) {
                hval k = a->keys[i];
                if (k.tag == HV_STR) buf_printf(b, "[\"%.*s\"]=>\n", (int)k.u.s->len, k.u.s->data);
                else buf_printf(b, "[%lld]=>\n", (long long)k.u.i);
            } else buf_printf(b, "[%d]=>\n", (int)i);
            hp_dump_indent(b, depth + 1);
            hp_dump_r(a->vals[i], b, depth + 1);
            buf_puts(b, "\n");
        }
        hp_dump_indent(b, depth);
        buf_puts(b, "}");
        break;
    }
    case HV_OBJ: buf_printf(b, "object(%p)", v.u.p); break;
    case HV_CLO: buf_puts(b, "object(Closure)"); break;
    }
}
hval hpbi_var_dump(hval v) {
    Buf b;
    buf_init(&b);
    hp_dump_r(v, &b, 0);
    buf_puts(&b, "\n");
    hstr *s = hp_str_take_buf(&b);
    fwrite(s->data, 1, s->len, stdout);
    hp_str_free(s);
    RETI(0);
}
static void hp_print_r_r(hval v, Buf *b, int depth) {
    if (v.tag == HV_ARR) {
        harr *a = v.u.a;
        buf_puts(b, "Array\n");
        hp_dump_indent(b, depth);
        buf_puts(b, "(\n");
        for (size_t i = 0; i < a->len; i++) {
            hp_dump_indent(b, depth + 1);
            if (a->is_map) {
                hval k = a->keys[i];
                if (k.tag == HV_STR) buf_printf(b, "[%.*s] => ", (int)k.u.s->len, k.u.s->data);
                else buf_printf(b, "[%lld] => ", (long long)k.u.i);
            } else buf_printf(b, "[%d] => ", (int)i);
            if (a->vals[i].tag == HV_ARR) {
                hp_print_r_r(a->vals[i], b, depth + 1);
                buf_puts(b, "\n");
            } else {
                hp_print_r_r(a->vals[i], b, depth + 1);
                buf_puts(b, "\n");
            }
        }
        hp_dump_indent(b, depth);
        buf_puts(b, ")\n");
    } else {
        hstr *s = hp_val_to_str(v);
        buf_printf(b, "%.*s", (int)s->len, s->data);
    }
}
hval hpbi_print_r(hval v) {
    /* PHP semantics: print_r prints and returns true when called as a
     * statement; HolyPHP always prints and additionally returns the text so
     * string context ("..." . print_r($x)) also works. */
    Buf b;
    buf_init(&b);
    hp_print_r_r(v, &b, 0);
    hstr *s = hp_str_take_buf(&b);
    fwrite(s->data, 1, s->len, stdout);
    RETS(s);
}
/* var_export: PHP syntax that recreates the value */
static void hp_export_str(Buf *b, hstr *s) {
    buf_putc(b, '\'');
    for (size_t i = 0; i < s->len; i++) {
        char c = s->data[i];
        if (c == '\'' || c == '\\') buf_putc(b, '\\');
        buf_putc(b, c);
    }
    buf_putc(b, '\'');
}
static void hp_export_r(hval v, Buf *b, int depth) {
    switch (v.tag) {
    case HV_NULL: buf_puts(b, "NULL"); break;
    case HV_BOOL: buf_puts(b, v.u.b ? "true" : "false"); break;
    case HV_INT: buf_printf(b, "%lld", (long long)v.u.i); break;
    case HV_FLOAT: {
        char tmp[64];
        snprintf(tmp, sizeof tmp, "%.17g", v.u.f);
        buf_puts(b, tmp);
        if (!strpbrk(tmp, ".eEn")) buf_puts(b, ".0");
        break;
    }
    case HV_STR: hp_export_str(b, v.u.s); break;
    case HV_ARR: {
        harr *x = v.u.a;
        buf_puts(b, "array (\n");
        for (size_t i = 0; x && i < x->len; i++) {
            for (int d = 0; d <= depth; d++) buf_puts(b, "  ");
            hval k = x->is_map && x->keys ? x->keys[i] : hp_of_int((int64_t)i);
            if (k.tag == HV_STR) hp_export_str(b, k.u.s);
            else buf_printf(b, "%lld", (long long)hp_val_to_int(k));
            buf_puts(b, " => ");
            if (depth > 32) buf_puts(b, "NULL");
            else hp_export_r(x->vals[i], b, depth + 1);
            buf_puts(b, ",\n");
        }
        for (int d = 0; d < depth; d++) buf_puts(b, "  ");
        buf_putc(b, ')');
        break;
    }
    default: buf_puts(b, "NULL"); break;
    }
}
/* var_export($v[, $return]): prints, or returns the text when $return */
hval hpbi_var_export(hval v, hval ret) {
    Buf b;
    buf_init(&b);
    hp_export_r(v, &b, 0);
    hstr *s = hp_str_take_buf(&b);
    if (hp_val_to_bool(ret)) RETS(s);
    fwrite(s->data, 1, s->len, stdout);
    hp_val_rc_dec(hp_of_str(s));
    return hp_null;
}

/* serialize / unserialize: PHP's format (i:1; d:0.5; b:1; N; s:3:"abc";
 * a:2:{key;value...}) for scalars and arrays */
static void hp_ser_r(hval v, Buf *b, int depth) {
    switch (v.tag) {
    case HV_NULL: buf_puts(b, "N;"); break;
    case HV_BOOL: buf_printf(b, "b:%d;", v.u.b ? 1 : 0); break;
    case HV_INT: buf_printf(b, "i:%lld;", (long long)v.u.i); break;
    case HV_FLOAT: buf_printf(b, "d:%.17g;", v.u.f); break;
    case HV_STR:
        buf_printf(b, "s:%zu:\"", v.u.s->len);
        buf_write(b, v.u.s->data, v.u.s->len);
        buf_puts(b, "\";");
        break;
    case HV_ARR: {
        harr *x = v.u.a;
        buf_printf(b, "a:%zu:{", x ? x->len : 0);
        for (size_t i = 0; x && i < x->len; i++) {
            hval k = x->is_map && x->keys ? x->keys[i] : hp_of_int((int64_t)i);
            hp_ser_r(k.tag == HV_STR ? k : hp_of_int(hp_val_to_int(k)), b, depth + 1);
            if (depth > 64) buf_puts(b, "N;");
            else hp_ser_r(x->vals[i], b, depth + 1);
        }
        buf_putc(b, '}');
        break;
    }
    default: buf_puts(b, "N;"); break;
    }
}
hval hpbi_serialize(hval v) {
    Buf b;
    buf_init(&b);
    hp_ser_r(v, &b, 0);
    RETS(hp_str_take_buf(&b));
}
/* parse one value at *pp; false on malformed input */
static bool hp_unser_r(const char **pp, const char *end, hval *out, int depth) {
    const char *p = *pp;
    if (p >= end || depth > 64) return false;
    char t = *p;
    if (t == 'N') {
        if (p + 1 >= end || p[1] != ';') return false;
        *out = hp_null; *pp = p + 2; return true;
    }
    if (p + 1 >= end || p[1] != ':') return false;
    p += 2;
    if (t == 'b' || t == 'i' || t == 'd') {
        const char *semi = memchr(p, ';', (size_t)(end - p));
        if (!semi) return false;
        char tmp[64];
        size_t n = (size_t)(semi - p);
        if (n >= sizeof tmp) return false;
        memcpy(tmp, p, n); tmp[n] = 0;
        if (t == 'b') *out = hp_of_bool(atoi(tmp) != 0);
        else if (t == 'i') *out = hp_of_int(strtoll(tmp, NULL, 10));
        else *out = hp_of_float(strtod(tmp, NULL));
        *pp = semi + 1;
        return true;
    }
    if (t == 's') {
        char *colon;
        long long len = strtoll(p, &colon, 10);
        if (*colon != ':' || colon + 2 + len + 2 > end || colon[1] != '"' || len < 0) return false;
        const char *data = colon + 2;
        if (data[len] != '"' || data[len + 1] != ';') return false;
        *out = hp_of_str(hp_str_new(data, (size_t)len));
        *pp = data + len + 2;
        return true;
    }
    if (t == 'a') {
        char *colon;
        long long n = strtoll(p, &colon, 10);
        if (*colon != ':' || colon[1] != '{' || n < 0) return false;
        p = colon + 2;
        harr *arr = hp_arr_new();
        for (long long i = 0; i < n; i++) {
            hval k, v;
            if (!hp_unser_r(&p, end, &k, depth + 1)) { hp_val_rc_dec(hp_of_arr(arr)); return false; }
            if (!hp_unser_r(&p, end, &v, depth + 1)) { hp_val_rc_dec(k); hp_val_rc_dec(hp_of_arr(arr)); return false; }
            hp_arr_set(arr, k, v);
            hp_val_rc_dec(k);
            hp_val_rc_dec(v);
        }
        if (p >= end || *p != '}') { hp_val_rc_dec(hp_of_arr(arr)); return false; }
        *out = hp_of_arr(arr);
        *pp = p + 1;
        return true;
    }
    return false;
}
/* unserialize($s): the value, or false when $s is not serialized data */
hval hpbi_unserialize(hval s) {
    if (s.tag != HV_STR || !s.u.s) RETB(false);
    const char *p = s.u.s->data, *end = p + s.u.s->len;
    hval v;
    if (!hp_unser_r(&p, end, &v, 0) || p != end) RETB(false);
    return v;
}

/* ---------- io / process ---------- */
/* ANSI color escape sequences */
#define HP_ANSI_RESET    "\033[0m"
#define HP_ANSI_BLACK    "\033[0;30m"
#define HP_ANSI_RED      "\033[0;31m"
#define HP_ANSI_GREEN    "\033[0;32m"
#define HP_ANSI_YELLOW   "\033[0;33m"
#define HP_ANSI_BLUE     "\033[0;34m"
#define HP_ANSI_MAGENTA  "\033[0;35m"
#define HP_ANSI_CYAN     "\033[0;36m"
#define HP_ANSI_WHITE    "\033[0;37m"
#define HP_ANSI_BRIGHT_BLACK   "\033[1;30m"
#define HP_ANSI_BRIGHT_RED     "\033[1;31m"
#define HP_ANSI_BRIGHT_GREEN   "\033[1;32m"
#define HP_ANSI_BRIGHT_YELLOW  "\033[1;33m"
#define HP_ANSI_BRIGHT_BLUE    "\033[1;34m"
#define HP_ANSI_BRIGHT_MAGENTA "\033[1;35m"
#define HP_ANSI_BRIGHT_CYAN    "\033[1;36m"
#define HP_ANSI_BRIGHT_WHITE   "\033[1;37m"

static const char *ansi_for_color(int color_code) {
    switch (color_code) {
        case 0:  return HP_ANSI_RESET;
        case 1:  return HP_ANSI_BLACK;
        case 2:  return HP_ANSI_RED;
        case 3:  return HP_ANSI_GREEN;
        case 4:  return HP_ANSI_YELLOW;
        case 5:  return HP_ANSI_BLUE;
        case 6:  return HP_ANSI_MAGENTA;
        case 7:  return HP_ANSI_CYAN;
        case 8:  return HP_ANSI_WHITE;
        case 9:  return HP_ANSI_BRIGHT_BLACK;
        case 10: return HP_ANSI_BRIGHT_RED;
        case 11: return HP_ANSI_BRIGHT_GREEN;
        case 12: return HP_ANSI_BRIGHT_YELLOW;
        case 13: return HP_ANSI_BRIGHT_BLUE;
        case 14: return HP_ANSI_BRIGHT_MAGENTA;
        case 15: return HP_ANSI_BRIGHT_CYAN;
        case 16: return HP_ANSI_BRIGHT_WHITE;
        default: return NULL;
    }
}

hval hpbi_readline(hval prompt) {
    if (prompt.tag == HV_STR && prompt.u.s->len)
        fwrite(prompt.u.s->data, 1, prompt.u.s->len, stdout);
    char buf[4096];
    if (!fgets(buf, sizeof buf, stdin)) RETS(hp_str_lit(""));
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) n--;
    RETS(hp_str_new(buf, n));
}

/* The second argument of print/println/input is either an ANSI color code
 * (0..16) or - when it is something else - the first formatting value of a
 * printf-style call. hp_null is what codegen pads a 1-arg call with, so it
 * must stay transparent. */
static hstr *io_text(hval text, hval second) {
    if (second.tag == HV_INT || second.tag == HV_NULL)
        return hp_val_to_str(text);
    return hp_val_to_str(hpbi_sprintf(text, second));
}

/* color-aware input() builtin: input(prompt, color_code) */
hval hpbi_readline_color(hval prompt, hval color) {
    hstr *p = prompt.tag == HV_STR ? prompt.u.s : NULL;
    const char *ansi = color.tag == HV_INT ? ansi_for_color((int)color.u.i) : NULL;
    if (p && p->len) {
        if (ansi) fwrite(ansi, 1, strlen(ansi), stdout);
        fwrite(p->data, 1, p->len, stdout);
        if (ansi) fwrite(HP_ANSI_RESET, 1, strlen(HP_ANSI_RESET), stdout);
    }
    char buf[4096];
    if (!fgets(buf, sizeof buf, stdin)) RETS(hp_str_lit(""));
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) n--;
    RETS(hp_str_new(buf, n));
}

/* color-aware print() builtin: print(text, color_code?) */
hval hpbi_print_color(hval text, hval color) {
    hstr *s = io_text(text, color);
    const char *ansi = color.tag == HV_INT ? ansi_for_color((int)color.u.i) : NULL;
    if (ansi) {
        fwrite(ansi, 1, strlen(ansi), stdout);
        fwrite(s->data, 1, s->len, stdout);
        fwrite(HP_ANSI_RESET, 1, strlen(HP_ANSI_RESET), stdout);
    } else {
        fwrite(s->data, 1, s->len, stdout);
    }
    return hp_null;   /* print() is void */
}

/* color-aware println() builtin: println(text, color_code?) - appends newline */
hval hpbi_println_color(hval text, hval color) {
    hstr *s = io_text(text, color);
    const char *ansi = color.tag == HV_INT ? ansi_for_color((int)color.u.i) : NULL;
    if (ansi) {
        fwrite(ansi, 1, strlen(ansi), stdout);
        fwrite(s->data, 1, s->len, stdout);
        fwrite(HP_ANSI_RESET, 1, strlen(HP_ANSI_RESET), stdout);
    } else {
        fwrite(s->data, 1, s->len, stdout);
    }
    fwrite("\n", 1, 1, stdout);
    return hp_null;   /* println() is void */
}

/* plain (colourless) spellings, kept for callers that do not go through the
 * two-argument codegen path */
hval hpbi_print(hval v)  { return hpbi_print_color(v, hp_null); }
hval hpbi_println(hval v){ return hpbi_println_color(v, hp_null); }
hval hpbi_input(hval p)  { return hpbi_readline(p); }
hval hpbi_file_get_contents(hval path) {
    hstr *p = hp_val_to_str(path);
    FILE *f = fopen(p->data, "rb");
    if (!f) {
        hp_throw_str("failed to open stream");
        RETB(false);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); RETB(false); }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    hstr *r = hp_str_new(buf, got);
    free(buf);
    RETS(r);
}
hval hpbi_file_put_contents(hval path, hval data, hval flags) {
    /* PHP: flags & FILE_APPEND (8) appends instead of truncating; an array
     * of values is written piece by piece (PHP 7.8+ style). */
    hstr *p = hp_val_to_str(path);
    const char *how = (flags.tag == HV_INT && (flags.u.i & 8)) ? "ab" : "wb";
    if (data.tag == HV_ARR) {
        FILE *f = fopen(p->data, how);
        if (!f) RETB(false);
        harr *a = data.u.a;
        for (size_t i = 0; i < a->len; i++) {
            hstr *s = hp_val_to_str(a->vals[i]);
            fwrite(s->data, 1, s->len, f);
        }
        fclose(f);
        RETB(true);
    }
    hstr *d = hp_val_to_str(data);
    FILE *f = fopen(p->data, how);
    if (!f) RETB(false);
    size_t n = fwrite(d->data, 1, d->len, f);
    fclose(f);
    RETI((int64_t)n);
}
hval hpbi_getenv(hval name) {
    const char *v = getenv(hp_val_to_str(name)->data);
    RETS(v ? hp_str_lit(v) : hp_str_lit(""));
}
hval hpbi_putenv(hval kv) {
    hstr *entry = hp_val_to_str(kv);
    if (!entry) RETB(false);
    const char *eq = memchr(entry->data, '=', entry->len);
    if (!eq || eq == entry->data) RETB(false);
    size_t name_len = (size_t)(eq - entry->data);
    size_t value_len = entry->len - name_len - 1;
    char *name = (char *)malloc(name_len + 1);
    char *value = (char *)malloc(value_len + 1);
    if (!name || !value) { free(name); free(value); RETB(false); }
    memcpy(name, entry->data, name_len); name[name_len] = 0;
    memcpy(value, eq + 1, value_len); value[value_len] = 0;
#if defined(_WIN32)
    int rc = _putenv_s(name, value);
#else
    int rc = setenv(name, value, 1);
#endif
    free(name); free(value);
    RETB(rc == 0);
}
hval hpbi_php_uname(void) {
#if defined(_WIN32)
    RETS(hp_str_lit("Windows NT"));
#elif defined(__APPLE__)
    RETS(hp_str_lit("Darwin"));
#elif defined(__unix__)
    RETS(hp_str_lit("Linux"));
#else
    RETS(hp_str_lit("Unknown"));
#endif
}
hval hpbi_phpversion(void) { RETS(hp_str_lit(HPHP_VERSION)); }
hval hpbi_php_sapi_name(void) { RETS(hp_str_lit("cli")); }
hval hpbi_php_sapi(void) { RETS(hp_str_lit("cli")); }
/* PHP's historical logo GUID, kept for code that still asks for it */
hval hpbi_php_logo_guid(void) { RETS(hp_str_lit("PHPE9568F34-D428-11d2-A769-00AA001ACF42")); }
/* [1, 5, 15 minute load]; Windows has no load average: zeros */
hval hpbi_sys_getloadavg(void) {
    double l[3] = {0, 0, 0};
#if !defined(_WIN32)
    if (getloadavg(l, 3) != 3) { l[0] = l[1] = l[2] = 0; }
#endif
    harr *r = hp_arr_new();
    for (int i = 0; i < 3; i++) hp_arr_push(r, hp_of_float(l[i]));
    return hp_of_arr(r);
}
hval hpbi_microtime(void) {
    /* High-resolution on all platforms: PHP's microtime(true) needs
     * sub-second precision for benchmarks. */
#if defined(_WIN32)
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    RETF((double)c.QuadPart / (double)f.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    RETF((double)ts.tv_sec + ts.tv_nsec / 1e9);
#endif
}
hval hpbi_hrtime(hval as_number) {
#if defined(_WIN32)
    static LARGE_INTEGER freq = {0};
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    int64_t ns = (int64_t)((double)c.QuadPart / (double)freq.QuadPart * 1e9);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t ns = (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
#endif
    RETI(ns);
}
/* memory_get_usage(): the process's resident memory, as the OS reports it
 * (the working set on Windows, RSS from /proc on Linux). */
#if defined(_WIN32)
typedef struct { DWORD cb, faults; SIZE_T peak_ws, ws, a, b, c, d, pagefile, peak_pagefile; } hp_pmc;
BOOL WINAPI K32GetProcessMemoryInfo(HANDLE, void *, DWORD);
#endif
static int64_t hp_mem_rss(bool peak) {
#if defined(_WIN32)
    hp_pmc pmc;
    memset(&pmc, 0, sizeof pmc);
    pmc.cb = sizeof pmc;
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) return 0;
    return (int64_t)(peak ? pmc.peak_ws : pmc.ws);
#else
    FILE *f = fopen(peak ? "/proc/self/status" : "/proc/self/statm", "r");
    if (!f) return 0;
    long long v = 0;
    if (peak) {
        char line[256];
        while (fgets(line, sizeof line, f))
            if (!strncmp(line, "VmHWM:", 6)) { v = atoll(line + 6) * 1024; break; }
    } else {
        long long size = 0, rss = 0;
        if (fscanf(f, "%lld %lld", &size, &rss) == 2) v = rss * sysconf(_SC_PAGESIZE);
    }
    fclose(f);
    return (int64_t)v;
#endif
}
hval hpbi_memory_get_usage(void) { RETI(hp_mem_rss(false)); }
hval hpbi_memory_get_peak_usage(void) { RETI(hp_mem_rss(true)); }
hval hpbi_gc_collect_cycles(void) { RETI(0); }
hval hpbi_date2(hval fmt, hval ts) {
    /* PHP date(): Y m d H i s and friends over the local time;
     * optional 2nd arg = unix timestamp (default now) */
    time_t now = ts.tag == HV_NULL ? time(NULL) : (time_t)hp_val_to_int(ts);
    struct tm tmbuf;
#if defined(_WIN32)
    localtime_s(&tmbuf, &now);
#else
    localtime_r(&now, &tmbuf);
#endif
    struct tm *tmv = &tmbuf;
    hstr *f = hp_val_to_str(fmt);
    Buf b;
    buf_init(&b);
    for (size_t i = 0; i < f->len; i++) {
        char c = f->data[i];
        if (c == '\\') { if (i + 1 < f->len) buf_putc(&b, f->data[++i]); continue; }
        if (c == 'Y') { buf_printf(&b, "%04d", tmv->tm_year + 1900); continue; }
        if (c == 'y') { buf_printf(&b, "%02d", (tmv->tm_year + 1900) % 100); continue; }
        if (c == 'm') { buf_printf(&b, "%02d", tmv->tm_mon + 1); continue; }
        if (c == 'n') { buf_printf(&b, "%d", tmv->tm_mon + 1); continue; }
        if (c == 'd') { buf_printf(&b, "%02d", tmv->tm_mday); continue; }
        if (c == 'j') { buf_printf(&b, "%d", tmv->tm_mday); continue; }
        if (c == 'H') { buf_printf(&b, "%02d", tmv->tm_hour); continue; }
        if (c == 'G') { buf_printf(&b, "%d", tmv->tm_hour); continue; }
        if (c == 'i') { buf_printf(&b, "%02d", tmv->tm_min); continue; }
        if (c == 's') { buf_printf(&b, "%02d", tmv->tm_sec); continue; }
        if (c == 'h') { int hh = tmv->tm_hour % 12; if (hh == 0) hh = 12; buf_printf(&b, "%02d", hh); continue; }
        if (c == 'g') { int hh = tmv->tm_hour % 12; if (hh == 0) hh = 12; buf_printf(&b, "%d", hh); continue; }
        if (c == 'A') { buf_puts(&b, tmv->tm_hour < 12 ? "AM" : "PM"); continue; }
        if (c == 'a') { buf_puts(&b, tmv->tm_hour < 12 ? "am" : "pm"); continue; }
        if (c == 'w') { buf_printf(&b, "%d", tmv->tm_wday); continue; }
        if (c == 'N') { buf_printf(&b, "%d", tmv->tm_wday == 0 ? 7 : tmv->tm_wday); continue; }
        if (c == 'z') { buf_printf(&b, "%d", tmv->tm_yday); continue; }
        if (c == 'L') { int y = tmv->tm_year + 1900; buf_printf(&b, "%d", ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)); continue; }
        if (c == 't') { static const int mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31}; int mm = tmv->tm_mon; int d = mdays[mm]; if (mm == 1) { int y = tmv->tm_year + 1900; if ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) d = 29; } buf_printf(&b, "%d", d); continue; }
        if (c == 'U') { buf_printf(&b, "%lld", (long long)now); continue; }
        /* names: D Mon, l Monday, M Jan, F January (English, as in PHP) */
        if (c == 'D' || c == 'l') {
            static const char *days[7] = {"Sunday", "Monday", "Tuesday", "Wednesday",
                                          "Thursday", "Friday", "Saturday"};
            const char *dn = days[tmv->tm_wday % 7];
            if (c == 'D') buf_write(&b, dn, 3); else buf_puts(&b, dn);
            continue;
        }
        if (c == 'M' || c == 'F') {
            static const char *months[12] = {"January", "February", "March", "April", "May",
                                             "June", "July", "August", "September", "October",
                                             "November", "December"};
            const char *mn = months[tmv->tm_mon % 12];
            if (c == 'M') buf_write(&b, mn, 3); else buf_puts(&b, mn);
            continue;
        }
        buf_putc(&b, c);
    }
    RETS(hp_str_take_buf(&b));
}
hval hpbi_time(void) { RETI((int64_t)time(NULL)); }
hval hpbi_usleep(hval us) {
#if defined(_WIN32)
    Sleep((DWORD)(hp_val_to_int(us) / 1000));
#else
    struct timespec ts = { .tv_sec = 0, .tv_nsec = hp_val_to_int(us) * 1000 };
    nanosleep(&ts, NULL);
#endif
    return hp_null;
}
hval hpbi_sleep(hval s) {
#if defined(_WIN32)
    Sleep((DWORD)(hp_val_to_int(s) * 1000));
#else
    struct timespec ts = { .tv_sec = hp_val_to_int(s), .tv_nsec = 0 };
    nanosleep(&ts, NULL);
#endif
    RETI(0);
}
static bool hp_seeded = false;
static void hp_seed_once(void) {
    if (!hp_seeded) { srand((unsigned)time(NULL)); hp_seeded = true; }
}
hval hpbi_rand(hval mn, hval mx) {
    hp_seed_once();
    if (mn.tag != HV_NULL || mx.tag != HV_NULL)
        RETI(hp_val_to_int(mn) + (int64_t)rand() %
             (hp_val_to_int(mx) - hp_val_to_int(mn) + 1));
    RETI((int64_t)rand());
}
hval hpbi_lcg_value(void) {
    hp_seed_once();
    return hp_of_float((double)rand() / ((double)RAND_MAX + 1.0));
}
hval hpbi_mt_rand(hval mn, hval mx) {
    hp_seed_once();
    if (mn.tag != HV_NULL || mx.tag != HV_NULL)
        RETF((double)hp_val_to_int(mn) +
             ((double)rand() / (double)RAND_MAX) *
                 (double)(hp_val_to_int(mx) - hp_val_to_int(mn)));
    RETF((double)rand() / (double)RAND_MAX);
}
hval hpbi_srand(hval seed) { srand((unsigned)hp_val_to_int(seed)); hp_seeded = true; return hp_null; }
hval hpbi_mt_srand(hval seed) { srand((unsigned)hp_val_to_int(seed)); hp_seeded = true; return hp_null; }
hval hpbi_random_int(hval mn, hval mx) { return hpbi_rand(mn, mx); }

/* ---------- filesystem helpers ---------- */
hval hpbi_getcwd(void) {
    char buf[2048];
#if defined(_WIN32)
    GetCurrentDirectoryA(sizeof buf, buf);
#else
    if (!getcwd(buf, sizeof buf)) buf[0] = 0;
#endif
    RETS(hp_str_new(buf, strlen(buf)));
}
hval hpbi_file_exists(hval path) {
    hstr *p = hp_val_to_str(path);
    FILE *f = fopen(p->data, "rb");
    if (f) { fclose(f); RETB(true); }
#if defined(_WIN32)
    DWORD attr = GetFileAttributesA(p->data);
    RETB(attr != INVALID_FILE_ATTRIBUTES);
#else
    RETB(access(p->data, F_OK) == 0);
#endif
}
hval hpbi_is_dir(hval path) {
    hstr *p = hp_val_to_str(path);
#if defined(_WIN32)
    DWORD attr = GetFileAttributesA(p->data);
    RETB(attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY));
#else
    struct stat st;
    RETB(stat(p->data, &st) == 0 && S_ISDIR(st.st_mode));
#endif
}
hval hpbi_filesize(hval path) {
    hstr *p = hp_val_to_str(path);
    FILE *f = fopen(p->data, "rb");
    if (!f) RETI(-1);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    RETI(sz);
}
hval hpbi_rmdir(hval path) { RETB(rmdir(hp_val_to_str(path)->data) == 0); }
hval hpbi_unlink(hval path) { RETB(remove(hp_val_to_str(path)->data) == 0); }
hval hpbi_rename(hval from, hval to) {
    RETB(rename(hp_val_to_str(from)->data, hp_val_to_str(to)->data) == 0);
}
hval hpbi_copy(hval from, hval to) {
    FILE *a = fopen(hp_val_to_str(from)->data, "rb");
    if (!a) RETB(false);
    FILE *b = fopen(hp_val_to_str(to)->data, "wb");
    if (!b) { fclose(a); RETB(false); }
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, a)) > 0) fwrite(buf, 1, n, b);
    fclose(a);
    fclose(b);
    RETB(true);
}
/* fgets/fwrite now live with the file-handle block (fopen/fclose below):
 * they dispatch on int handle (new) vs string path (legacy). */
/* ---------- real file handles ----------
 * fopen() returns a small integer handle; fgets/fwrite/fread/feof/fclose
 * operate on it. The old fopen("...") returned a raw pointer in an int
 * and fclose was a no-op — leaking every file.
 * fwrite($h, ...) / fgets($h) dispatch: int handle = new-style, string
 * path = legacy whole-file read / append-write. */
#define HP_MAX_FILES 512
static FILE *g_files[HP_MAX_FILES];
static bool g_file_open[HP_MAX_FILES];

hval hpbi_fopen(hval path, hval mode) {
    hstr *p = hp_val_to_str(path), *m = hp_val_to_str(mode);
    FILE *f = fopen(p->data, m->data);
    if (!f) RETB(false);
    for (int i = 1; i < HP_MAX_FILES; i++) {
        if (!g_file_open[i]) {
            g_files[i] = f;
            g_file_open[i] = true;
            RETI(i);
        }
    }
    fclose(f);
    RETB(false);
}
static bool file_ok(hval fh) {
    return fh.tag == HV_INT && fh.u.i > 0 && fh.u.i < HP_MAX_FILES &&
           g_file_open[fh.u.i];
}
hval hpbi_fgets(hval fh) {
    if (fh.tag == HV_STR) {
        /* legacy: fgets("path.txt") = first line of the file */
        FILE *f = fopen(hp_val_to_str(fh)->data, "rb");
        if (!f) RETS(hp_str_lit(""));
        char line[4096];
        if (!fgets(line, sizeof line, f)) { fclose(f); RETS(hp_str_lit("")); }
        fclose(f);
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) n--;
        RETS(hp_str_new(line, n));
    }
    if (!file_ok(fh)) RETB(false);            /* PHP: fgets at EOF => false */
    char line[4096];
    if (!fgets(line, sizeof line, g_files[fh.u.i])) RETB(false);
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) n--;
    RETS(hp_str_new(line, n));
}
/* fgetc($fh): one byte from an fopen() handle (stdin when given none / STDIN),
 * false at the end. It used to read stdin whatever handle it got. */
hval hpbi_fgetc(hval fh) {
    FILE *f = file_ok(fh) ? g_files[fh.u.i] : stdin;
    int c = fgetc(f);
    if (c == EOF) RETB(false);
    char b[1] = { (char)c };
    RETS(hp_str_new(b, 1));
}
hval hpbi_fwrite(hval fh, hval data) {
    if (fh.tag == HV_STR) {
        /* legacy: fwrite("path", data) appends to the file */
        hstr *p = hp_val_to_str(fh), *d = hp_val_to_str(data);
        FILE *f = fopen(p->data, "ab");
        if (!f) RETI(0);
        size_t n = fwrite(d->data, 1, d->len, f);
        fclose(f);
        RETI((int64_t)n);
    }
    if (!file_ok(fh)) RETI(0);
    hstr *d = hp_val_to_str(data);
    RETI((int64_t)fwrite(d->data, 1, d->len, g_files[fh.u.i]));
}
hval hpbi_fread(hval fh, hval n) {
    if (!file_ok(fh)) RETS(hp_str_lit(""));
    int64_t want = hp_val_to_int(n);
    if (want <= 0) RETS(hp_str_lit(""));
    if (want > 1 << 20) want = 1 << 20;
    char *buf = (char *)malloc((size_t)want);
    size_t got = fread(buf, 1, (size_t)want, g_files[fh.u.i]);
    hstr *r = hp_str_new(buf, got);
    free(buf);
    RETS(r);
}
hval hpbi_feof(hval fh) {
    if (!file_ok(fh)) RETB(true);
    RETB(feof(g_files[fh.u.i]) != 0);
}
hval hpbi_fclose(hval fh) {
    if (!file_ok(fh)) RETB(false);
    fclose(g_files[fh.u.i]);
    g_file_open[fh.u.i] = false;
    g_files[fh.u.i] = NULL;
    RETB(true);
}

/* Run a shell command and return its stdout. stderr is left on the caller's
 * stderr; a nonzero command exit still returns whatever stdout it produced.
 * shell_exec is intentionally unavailable in the online playground. */
hval hpbi_shell_exec(hval command) {
    hstr *cmd = hp_val_to_str(command);
#if defined(_WIN32)
    FILE *pipe = _popen(cmd->data, "rb");
#else
    FILE *pipe = popen(cmd->data, "r");
#endif
    if (!pipe) RETB(false);

    Buf output;
    buf_init(&output);
    char chunk[4096];
    size_t got;
    while ((got = fread(chunk, 1, sizeof chunk, pipe)) != 0)
        buf_write(&output, chunk, got);
    bool read_error = ferror(pipe) != 0;
#if defined(_WIN32)
    int status = _pclose(pipe);
#else
    int status = pclose(pipe);
#endif
    if (read_error || status < 0) {
        free(output.data);
        RETB(false);
    }

    hstr *result = hp_str_new(output.data ? output.data : "", output.len);
    free(output.data);
    RETS(result);
}

/* Stream a shell command's stdout directly to this process. Keep stderr
 * attached to the caller so diagnostics remain visible, matching passthru's
 * usual command-line behavior. */
/* exec() has a one-argument table contract but a two-argument C prototype in
 * hphp_rt.h (PHP's optional output/return-code parameters). hphp_ext.h maps
 * the call to this variadic entry point so both forms link. */
hval hpbi_exec_call(hval command, ...) {
    return hpbi_shell_exec(command);
}

hval hpbi_passthru(hval command) {
    hstr *cmd = hp_val_to_str(command);
#if defined(_WIN32)
    FILE *pipe = _popen(cmd->data, "rb");
#else
    FILE *pipe = popen(cmd->data, "r");
#endif
    if (!pipe) return hp_null;

    char chunk[4096];
    size_t got;
    while ((got = fread(chunk, 1, sizeof chunk, pipe)) != 0) {
        if (fwrite(chunk, 1, got, stdout) != got) break;
    }
    fflush(stdout);
#if defined(_WIN32)
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return hp_null;
}

/* Nonblocking child-process table used by the IDE terminal and runner. A
 * handle is a small positive table index; output stays readable after exit
 * until the caller closes the handle. */
#define HP_PROC_SLOTS 64
#define HP_PROC_READ_LIMIT (1024 * 1024)
typedef struct HpProcess {
    bool used, exited;
    int exit_code;
#if defined(_WIN32)
    HANDLE process, input, output, error;
#else
    pid_t pid;
    int input, output, error;
#endif
} HpProcess;
static HpProcess hp_processes[HP_PROC_SLOTS];

static HpProcess *hp_process_get(hval handle) {
    int64_t id = hp_val_to_int(handle);
    if (id < 1 || id > HP_PROC_SLOTS) return NULL;
    HpProcess *p = &hp_processes[id - 1];
    return p->used ? p : NULL;
}

static int hp_process_id(HpProcess *p) {
    if (!p || p < hp_processes || p >= hp_processes + HP_PROC_SLOTS) return 0;
    return (int)(p - hp_processes) + 1;
}

static int hp_process_start(const char *command, const char *cwd) {
    HpProcess *p = NULL;
    for (int i = 0; i < HP_PROC_SLOTS; i++)
        if (!hp_processes[i].used) { p = &hp_processes[i]; break; }
    if (!p || !command || !*command) return 0;
    memset(p, 0, sizeof *p);
#if defined(_WIN32)
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE child_in = NULL, child_out = NULL, child_err = NULL;
    if (!CreatePipe(&child_in, &p->input, &sa, 0) ||
        !CreatePipe(&p->output, &child_out, &sa, 0) ||
        !CreatePipe(&p->error, &child_err, &sa, 0)) goto win_fail;
    SetHandleInformation(p->input, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(p->output, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(p->error, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si); memset(&pi, 0, sizeof pi);
    si.cb = sizeof si; si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = child_in; si.hStdOutput = child_out; si.hStdError = child_err;
    char *line = _strdup(command);
    if (!line) goto win_fail;
    BOOL ok = CreateProcessA(NULL, line, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                             NULL, (cwd && *cwd) ? cwd : NULL, &si, &pi);
    free(line);
    CloseHandle(child_in); child_in = NULL;
    CloseHandle(child_out); child_out = NULL;
    CloseHandle(child_err); child_err = NULL;
    if (!ok) goto win_fail;
    CloseHandle(pi.hThread);
    p->process = pi.hProcess;
#else
    int in_pipe[2] = {-1,-1}, out_pipe[2] = {-1,-1}, err_pipe[2] = {-1,-1};
    if (pipe(in_pipe) || pipe(out_pipe) || pipe(err_pipe)) goto posix_fail;
    pid_t pid = fork();
    if (pid < 0) goto posix_fail;
    if (pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(in_pipe[0]); close(in_pipe[1]); close(out_pipe[0]);
        close(out_pipe[1]); close(err_pipe[0]); close(err_pipe[1]);
        if (cwd && *cwd) (void)chdir(cwd);
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }
    close(in_pipe[0]); close(out_pipe[1]); close(err_pipe[1]);
    p->pid = pid; p->input = in_pipe[1];
    p->output = out_pipe[0]; p->error = err_pipe[0];
    (void)fcntl(p->output, F_SETFL, fcntl(p->output, F_GETFL) | O_NONBLOCK);
    (void)fcntl(p->error, F_SETFL, fcntl(p->error, F_GETFL) | O_NONBLOCK);
#endif
    p->used = true;
    return hp_process_id(p);
#if defined(_WIN32)
win_fail:
    if (child_in) CloseHandle(child_in);
    if (child_out) CloseHandle(child_out);
    if (child_err) CloseHandle(child_err);
    if (p->input) CloseHandle(p->input);
    if (p->output) CloseHandle(p->output);
    if (p->error) CloseHandle(p->error);
    memset(p, 0, sizeof *p);
#else
posix_fail:
    for (int i = 0; i < 2; i++) {
        if (in_pipe[i] >= 0) close(in_pipe[i]);
        if (out_pipe[i] >= 0) close(out_pipe[i]);
        if (err_pipe[i] >= 0) close(err_pipe[i]);
    }
    memset(p, 0, sizeof *p);
#endif
    return 0;
}

static bool hp_process_poll(HpProcess *p) {
    if (!p) return false;
    if (p->exited) return false;
#if defined(_WIN32)
    DWORD wait = WaitForSingleObject(p->process, 0);
    if (wait == WAIT_OBJECT_0) {
        DWORD code = 1;
        GetExitCodeProcess(p->process, &code);
        p->exit_code = (int)code; p->exited = true;
    }
#else
    int status = 0;
    pid_t r = waitpid(p->pid, &status, WNOHANG);
    if (r == p->pid) {
        p->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) :
                       WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1;
        p->exited = true;
    }
#endif
    return !p->exited;
}

static hval hp_process_read(hval handle, hval max_value, bool is_err) {
    HpProcess *p = hp_process_get(handle);
    if (!p) RETS(hp_str_lit(""));
    int64_t maxn = hp_val_to_int(max_value);
    if (maxn <= 0 || maxn > HP_PROC_READ_LIMIT) maxn = HP_PROC_READ_LIMIT;
    Buf out; buf_init(&out);
    char buf[4096];
#if defined(_WIN32)
    HANDLE pipe = is_err ? p->error : p->output;
    while ((int64_t)out.len < maxn) {
        DWORD avail = 0, got = 0;
        if (!PeekNamedPipe(pipe, NULL, 0, NULL, &avail, NULL) || avail == 0) break;
        DWORD want = (DWORD)(maxn - (int64_t)out.len);
        if (want > sizeof buf) want = sizeof buf;
        if (want > avail) want = avail;
        if (!ReadFile(pipe, buf, want, &got, NULL) || got == 0) break;
        buf_write(&out, buf, got);
    }
#else
    int fd = is_err ? p->error : p->output;
    while ((int64_t)out.len < maxn) {
        size_t want = sizeof buf;
        if ((int64_t)want > maxn - (int64_t)out.len)
            want = (size_t)(maxn - (int64_t)out.len);
        ssize_t got = read(fd, buf, want);
        if (got <= 0) break;
        buf_write(&out, buf, (size_t)got);
    }
#endif
    hstr *s = hp_str_new(out.data ? out.data : "", out.len);
    free(out.data);
    RETS(s);
}

hval hpbi_proc_start(hval cmd, hval cwd) {
    hstr *c = hp_val_to_str(cmd), *d = hp_val_to_str(cwd);
    RETI(hp_process_start(c ? c->data : "", d ? d->data : ""));
}
hval hpbi_proc_open(hval cmd, hval cwd) { return hpbi_proc_start(cmd, cwd); }
hval hpbi_proc_read(hval h, hval maxn) { return hp_process_read(h, maxn, false); }
hval hpbi_proc_read_err(hval h, hval maxn) { return hp_process_read(h, maxn, true); }
hval hpbi_proc_write(hval handle, hval data) {
    HpProcess *p = hp_process_get(handle);
    if (!p) RETI(0);
    hstr *s = hp_val_to_str(data);
#if defined(_WIN32)
    DWORD wrote = 0;
    if (!WriteFile(p->input, s->data, (DWORD)s->len, &wrote, NULL)) RETI(0);
    RETI(wrote);
#else
    ssize_t wrote = write(p->input, s->data, s->len);
    RETI(wrote > 0 ? wrote : 0);
#endif
}
hval hpbi_proc_alive(hval handle) { RETB(hp_process_poll(hp_process_get(handle))); }
hval hpbi_proc_exit(hval handle) {
    HpProcess *p = hp_process_get(handle);
    RETI(p && hp_process_poll(p) == false && p->exited ? p->exit_code : -1);
}
hval hpbi_proc_kill(hval handle) {
    HpProcess *p = hp_process_get(handle);
    if (!p || p->exited) RETI(0);
#if defined(_WIN32)
    BOOL ok = TerminateProcess(p->process, 1);
    WaitForSingleObject(p->process, INFINITE);
    p->exit_code = 1; p->exited = true;
    RETI(ok ? 1 : 0);
#else
    int ok = kill(p->pid, SIGTERM) == 0;
    int status = 0;
    if (ok && waitpid(p->pid, &status, 0) == p->pid) {
        p->exit_code = WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1;
        p->exited = true;
    }
    RETI(ok ? 1 : 0);
#endif
}
hval hpbi_proc_close(hval handle) {
    HpProcess *p = hp_process_get(handle);
    if (!p) RETI(0);
    if (!p->exited) (void)hpbi_proc_kill(handle);
#if defined(_WIN32)
    CloseHandle(p->process); CloseHandle(p->input);
    CloseHandle(p->output); CloseHandle(p->error);
#else
    close(p->input); close(p->output); close(p->error);
#endif
    memset(p, 0, sizeof *p);
    RETI(1);
}
hval hpbi_proc_get_status(hval handle) {
    HpProcess *p = hp_process_get(handle);
    if (!p) RETB(false);
    bool running = hp_process_poll(p);
    harr *a = hp_arr_new();
    hp_arr_set(a, hp_of_str(hp_str_lit("running")), hp_of_bool(running));
    hp_arr_set(a, hp_of_str(hp_str_lit("exitcode")), hp_of_int(running ? -1 : p->exit_code));
    return hp_of_arr(a);
}

hval hpbi_scandir(hval path) {
    hstr *p = hp_val_to_str(path);
    harr *out = hp_arr_new();
#if defined(_WIN32)
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof pattern, "%s\\*", p->data);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return hp_of_arr(out);
    do {
        if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, ".."))
            hp_arr_push_take(out, hp_of_str(hp_str_new(fd.cFileName, strlen(fd.cFileName))));
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(p->data);
    if (!d) return hp_of_arr(out);
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") && strcmp(ent->d_name, ".."))
            hp_arr_push_take(out, hp_of_str(hp_str_new(ent->d_name, strlen(ent->d_name))));
    }
    closedir(d);
#endif
    return hp_of_arr(out);
}

/* ---------- number base conversion ---------- */
hval hpbi_decbin(hval n) { char b[80]; int64_t v = hp_val_to_int(n); itoa_bin(v, 2, b); RETS(hp_str_new(b, strlen(b))); }
hval hpbi_decoct(hval n) { char b[80]; int64_t v = hp_val_to_int(n); itoa_bin(v, 8, b); RETS(hp_str_new(b, strlen(b))); }
hval hpbi_dechex(hval n) { char b[80]; int64_t v = hp_val_to_int(n); itoa_bin(v, 16, b); RETS(hp_str_new(b, strlen(b))); }
static int64_t from_base(const char *s, int base) {
    return (int64_t)strtoll(s, NULL, base);
}
hval hpbi_bindec(hval s) { RETI(from_base(hp_val_to_str(s)->data, 2)); }
hval hpbi_octdec(hval s) { RETI(from_base(hp_val_to_str(s)->data, 8)); }
hval hpbi_hexdec(hval s) { RETI(from_base(hp_val_to_str(s)->data, 16)); }

/* ---------- unsafe memory ops ---------- */
hval hpbi_malloc(hval n) {
    void *p = malloc((size_t)hp_val_to_int(n));
    /* Unsafe raw addresses are integers in the language representation. An
     * HV_OBJ would enter the managed-object RC path and read an hobj header
     * from arbitrary malloc'd bytes during local cleanup. */
    return hp_of_int((int64_t)(intptr_t)p);
}
hval hpbi_free(hval p) {
    free(hp_addr_arg(p));
    return hp_null;
}
hval hpbi_memcpy(hval dst, hval src, hval n) {
    void *d = hp_addr_arg(dst), *s = hp_addr_arg(src);
    if (d && s) memcpy(d, s, (size_t)hp_val_to_int(n));
    return dst;
}
hval hpbi_memset(hval dst, hval byte, hval n) {
    void *d = hp_addr_arg(dst);
    if (d) memset(d, (int)hp_val_to_int(byte), (size_t)hp_val_to_int(n));
    return dst;
}
hval hpbi_ffi_load(hval lib) {
    hp_throw_str("ffi_load() is not available in this build");
    return hp_null;
}
hval hpbi_ffi_call(hval fn, hval args) {
    hp_throw_str("ffi_call() is not available in this build");
    return hp_null;
}
hval hpbi_heap_dump(hval p) {
    char buf[64];
    snprintf(buf, sizeof buf, "%p", p.tag == HV_OBJ ? p.u.p : NULL);
    RETS(hp_str_new(buf, strlen(buf)));
}

/* helper: scalar fallback ops used by codegen for mixed arithmetic */
hval hpbi_scalar_sub(hval a, hval b) { return hp_sub(a, b); }
hval hpbi_scalar_mul(hval a, hval b) { return hp_mul(a, b); }
hval hpbi_scalar_binop(hval a, hval b) { return hp_null; }

/* exit()/die(): stop the program right now (PHP semantics) */
hval hpbi_exit(hval code) {
    fflush(stdout);
    exit((int)hp_val_to_int(code));
    return hp_null;
}
hval hpbi_die(hval code) { return hpbi_exit(code); }

/* ------------------------------------------------------------------ */
/* sockets — the transport layer PHP calls "streams"                   */
/*                                                                     */
/*   $srv = stream_socket_server("tcp://127.0.0.1:9001");  -> handle   */
/*   $c   = stream_socket_client("tcp://127.0.0.1:9001");  -> handle   */
/*   $c   = stream_socket_accept($srv);        -> handle, -1 if none   */
/*   stream_set_blocking($c, false);           -> bool                 */
/*   stream_ready(array $handles, int $ms);    -> the readable ones    */
/*   stream_recv($c, int $max);                -> string, "" = nothing */
/*   stream_send($c, string $data);            -> bytes sent           */
/*   stream_eof($c); stream_peer($c); stream_close($c);                */
/*                                                                     */
/* Handles are plain integers (PHP exposes fds the same way) and every  */
/* accepted socket is non-blocking, so a server loop polls with         */
/* stream_ready() and one slow client can never stall the others.       */
/* ------------------------------------------------------------------ */

static bool hp_net_up = false;

static void hp_net_init(void) {
    if (hp_net_up) return;
#ifdef _WIN32
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
#endif
    hp_net_up = true;
}

/* Per-socket state. EOF is sticky: once the peer closes, it stays closed
 * even if we stop reading. */
typedef struct { intptr_t h; bool eof; } hp_sockrec;
static hp_sockrec *g_sock;
static size_t g_sock_len, g_sock_cap;

static hp_sockrec *sockrec(intptr_t h, bool create) {
    for (size_t i = 0; i < g_sock_len; i++)
        if (g_sock[i].h == h) return &g_sock[i];
    if (!create) return NULL;
    if (g_sock_len == g_sock_cap) {
        g_sock_cap = g_sock_cap ? g_sock_cap * 2 : 16;
        g_sock = (hp_sockrec *)realloc(g_sock, g_sock_cap * sizeof(hp_sockrec));
        if (!g_sock) { fprintf(stderr, "hphp: out of memory\n"); exit(1); }
    }
    g_sock[g_sock_len].h = h;
    g_sock[g_sock_len].eof = false;
    return &g_sock[g_sock_len++];
}

static void sock_forget(intptr_t h) {
    for (size_t i = 0; i < g_sock_len; i++)
        if (g_sock[i].h == h) { g_sock[i] = g_sock[--g_sock_len]; return; }
}

static hp_sock sock_of(hval v) { return (hp_sock)(intptr_t)hp_val_to_int(v); }

static void sock_nonblocking(hp_sock s, bool nonblock) {
#ifdef _WIN32
    u_long v = nonblock ? 1 : 0;
    ioctlsocket(s, FIONBIO, &v);
#else
    int fl = fcntl(s, F_GETFL, 0);
    if (fl < 0) return;
    fcntl(s, F_SETFL, nonblock ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
#endif
}

static void sock_sleep_ms(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    usleep((useconds_t)ms * 1000);
#endif
}

/* "tcp://host:port" | "host:port" | ":port" */
static bool sock_split_addr(hstr *addr, char *host, size_t hsz, char *port, size_t psz) {
    const char *p = addr ? addr->data : "";
    const char *scheme = strstr(p, "://");
    if (scheme) p = scheme + 3;
    const char *colon = strrchr(p, ':');
    if (!colon) return false;
    size_t hl = (size_t)(colon - p);
    if (hl >= hsz) hl = hsz - 1;
    memcpy(host, p, hl);
    host[hl] = 0;
    snprintf(port, psz, "%s", colon + 1);
    return port[0] != 0;
}







/* stream_socket_accept2(srv, timeoutMs): poll-then-accept, PHP-style.
 * Returns the client handle, or false when nothing arrived within the
 * timeout. Never blocks longer than timeoutMs (in 5 ms slices). */
















/* PHP-style callable dispatch: any hval may hold a closure. */
hval hp_call_value(hval fn, int nargs, const hval *args) {
    if (fn.tag == HV_CLO && fn.u.c) return hp_closure_call_arr(fn.u.c, (hval *)args, nargs);
    hstr *msg = hp_str_concat2(hp_str_lit("value of type "),
                               hp_str_lit(hp_kind_name(fn.tag)));
    msg = hp_str_concat2(msg, hp_str_lit(" is not callable"));
    hp_throw(hp_of_str(msg));
    return hp_null;
}

/* ---------- UI builtins (thin wrappers over hphp_ui.c) ---------- */
static hval uistr(const char *s) { return hp_of_str(hp_str_lit(s ? s : "")); }

hval hpbi_ui_dispatch(hval ms) { return hp_of_int(hpui_dispatch((int)hp_val_to_int(ms))); }
hval hpbi_ui_run_main(void) { hpui_run_main(); return hp_null; }
hval hpbi_ui_quit(void) { hpui_quit(); return hp_null; }

/* ui_window(title, x, y, w, h) — x,y = -32000 centers the window */
hval hpbi_ui_window(hval title, hval x, hval y, hval w, hval hgt) {
    hstr *t = hp_val_to_str(title);
    return hp_of_int(hpui_window_new(t->data, (int)hp_val_to_int(x),
                                     (int)hp_val_to_int(y), (int)hp_val_to_int(w),
                                     (int)hp_val_to_int(hgt)));
}

hval hpbi_ui_show(hval h, hval vis) {
    return hp_of_int(hpui_window_show((int64_t)hp_val_to_int(h), hp_val_to_bool(vis)));
}
hval hpbi_ui_title(hval h, hval t) {
    hstr *s = hp_val_to_str(t);
    return hp_of_int(hpui_window_title((int64_t)hp_val_to_int(h), s->data));
}
hval hpbi_ui_size(hval h, hval w, hval hgt) {
    return hp_of_int(hpui_window_size((int64_t)hp_val_to_int(h),
                                      (int)hp_val_to_int(w), (int)hp_val_to_int(hgt)));
}
hval hpbi_ui_pos(hval h, hval x, hval y) {
    return hp_of_int(hpui_window_pos((int64_t)hp_val_to_int(h),
                                     (int)hp_val_to_int(x), (int)hp_val_to_int(y)));
}
hval hpbi_ui_close(hval h) {
    return hp_of_int(hpui_window_close((int64_t)hp_val_to_int(h)));
}
hval hpbi_ui_alive(hval h) {
    return hp_of_bool(hpui_window_alive((int64_t)hp_val_to_int(h)));
}

/* ui_add(parent, kind, text, x, y, w, h) — kind uses the HPUI_* numbers */
hval hpbi_ui_add(hval parent, hval kind, hval text, hval x, hval y,
                hval w, hval hgt) {
    hstr *t = hp_val_to_str(text);
    return hp_of_int(hpui_ctrl_new((int64_t)hp_val_to_int(kind),
                                   (int64_t)hp_val_to_int(parent), t->data,
                                   (int)hp_val_to_int(x), (int)hp_val_to_int(y),
                                   (int)hp_val_to_int(w), (int)hp_val_to_int(hgt)));
}
hval hpbi_ui_set(hval h, hval text) {
    hstr *s = hp_val_to_str(text);
    return hp_of_int(hpui_ctrl_set_text((int64_t)hp_val_to_int(h), s->data));
}
hval hpbi_ui_get(hval h) {
    hstr *out = NULL;
    if (hpui_ctrl_text((int64_t)hp_val_to_int(h), &out) != 0 || !out)
        return hp_of_str(hp_str_lit(""));
    return hp_of_str(out);
}
hval hpbi_ui_enable(hval h, hval on) {
    return hp_of_int(hpui_ctrl_enable((int64_t)hp_val_to_int(h), hp_val_to_bool(on)));
}
hval hpbi_ui_move(hval h, hval x, hval y, hval w, hval hgt) {
    return hp_of_int(hpui_ctrl_move((int64_t)hp_val_to_int(h), (int)hp_val_to_int(x),
                                    (int)hp_val_to_int(y), (int)hp_val_to_int(w),
                                    (int)hp_val_to_int(hgt)));
}
hval hpbi_ui_focus(hval h) {
    return hp_of_int(hpui_ctrl_focus((int64_t)hp_val_to_int(h)));
}
hval hpbi_ui_items(hval h, hval item) {
    hstr *s = hp_val_to_str(item);
    return hp_of_int(hpui_list_add((int64_t)hp_val_to_int(h), s->data));
}
hval hpbi_ui_clear(hval h) {
    return hp_of_int(hpui_list_clear((int64_t)hp_val_to_int(h)));
}
hval hpbi_ui_remove(hval h, hval idx) {
    return hp_of_int(hpui_list_remove((int64_t)hp_val_to_int(h),
                                      (int64_t)hp_val_to_int(idx)));
}
hval hpbi_ui_ctrl_show(hval h, hval vis) {
    return hp_of_int(hpui_ctrl_show((int64_t)hp_val_to_int(h), hp_val_to_bool(vis)));
}
hval hpbi_ui_selected(hval h) {
    int64_t sel = -1;
    hpui_list_selected((int64_t)hp_val_to_int(h), &sel);
    return hp_of_int(sel);
}
hval hpbi_ui_select(hval h, hval idx) {
    return hp_of_int(hpui_list_select((int64_t)hp_val_to_int(h),
                                      (int64_t)hp_val_to_int(idx)));
}
hval hpbi_ui_item_text(hval h, hval idx) {
    hstr *out = NULL;
    if (hpui_list_text((int64_t)hp_val_to_int(h), (int64_t)hp_val_to_int(idx),
                       &out) != 0 || !out)
        return hp_of_str(hp_str_lit(""));
    return hp_of_str(out);
}
hval hpbi_ui_check_get(hval h) {
    bool on = false;
    hpui_check_get((int64_t)hp_val_to_int(h), &on);
    return hp_of_int(on ? 1 : 0);
}
hval hpbi_ui_check_set(hval h, hval on) {
    return hp_of_int(hpui_check_set((int64_t)hp_val_to_int(h), hp_val_to_bool(on)));
}
hval hpbi_ui_progress(hval h, hval pct) {
    return hp_of_int(hpui_progress_set((int64_t)hp_val_to_int(h),
                                       (int64_t)hp_val_to_int(pct)));
}
hval hpbi_ui_slider_get(hval h) {
    int64_t pos = 0;
    hpui_slider_get((int64_t)hp_val_to_int(h), &pos);
    return hp_of_int(pos);
}
hval hpbi_ui_slider_set(hval h, hval pos) {
    return hp_of_int(hpui_slider_set((int64_t)hp_val_to_int(h),
                                     (int64_t)hp_val_to_int(pos)));
}
hval hpbi_ui_bg(hval h, hval rgb) {
    return hp_of_int(hpui_ctrl_set_bg((int64_t)hp_val_to_int(h),
                                      (int64_t)hp_val_to_int(rgb)));
}
hval hpbi_ui_fg(hval h, hval rgb) {
    return hp_of_int(hpui_ctrl_set_fg((int64_t)hp_val_to_int(h),
                                      (int64_t)hp_val_to_int(rgb)));
}
hval hpbi_ui_font(hval h, hval size, hval bold) {
    return hp_of_int(hpui_ctrl_font((int64_t)hp_val_to_int(h),
                                    (int64_t)hp_val_to_int(size),
                                    hp_val_to_bool(bold)));
}
hval hpbi_ui_menu(hval win, hval label) {
    hstr *s = hp_val_to_str(label);
    return hp_of_int(hpui_menu_new((int64_t)hp_val_to_int(win), s->data));
}
hval hpbi_ui_menu_item(hval menu, hval label, hval cmdid) {
    hstr *s = hp_val_to_str(label);
    return hp_of_int(hpui_menu_item((int64_t)hp_val_to_int(menu), s->data,
                                    (int64_t)hp_val_to_int(cmdid)));
}
hval hpbi_ui_menu_sep(hval menu) {
    return hp_of_int(hpui_menu_sep((int64_t)hp_val_to_int(menu)));
}
hval hpbi_ui_menu_check(hval h) {
    return hp_of_int(hpui_ctrl_checked((int64_t)hp_val_to_int(h)));
}

/* ui_on(handle, event, callback) — event: 1 click, 2 change, 3 close, 4 key */
hval hpbi_ui_on(hval h, hval ev, hval cb) {
    return hp_of_int(hpui_on_event((int64_t)hp_val_to_int(h),
                                   (int64_t)hp_val_to_int(ev), cb));
}
hval hpbi_ui_timer(hval ms, hval cb) {
    return hp_of_int(hpui_timer((int64_t)hp_val_to_int(ms), cb));
}
hval hpbi_ui_msg(hval win, hval title, hval text, hval type) {
    hstr *t = hp_val_to_str(title), *x = hp_val_to_str(text);
    return hp_of_int(hpui_msgbox((int64_t)hp_val_to_int(win), t->data, x->data,
                                 (int)hp_val_to_int(type)));
}
hval hpbi_ui_open_file(hval win, hval filter) {
    hstr *f = hp_val_to_str(filter);
    return hpui_open_file((int64_t)hp_val_to_int(win), f->data);
}
hval hpbi_ui_save_file(hval win, hval filter) {
    hstr *f = hp_val_to_str(filter);
    return hpui_save_file((int64_t)hp_val_to_int(win), f->data);
}
hval hpbi_ui_pick_folder(hval win) {
    return hpui_pick_folder((int64_t)hp_val_to_int(win));
}
hval hpbi_ui_pick_color(hval win, hval init) {
    return hpui_color_pick((int64_t)hp_val_to_int(win), (int64_t)hp_val_to_int(init));
}
hval hpbi_ui_clip_set(hval text) {
    hstr *s = hp_val_to_str(text);
    return hp_of_int(ui_clip_set(s->data));
}
hval hpbi_ui_clip_get(void) { return ui_clip_get(); }

/* ---- UI 2.0: styling ---- */
hval hpbi_ui_style(hval h, hval role) {
    return hp_of_int(hpui_ctrl_style((int64_t)hp_val_to_int(h),
                                     (int64_t)hp_val_to_int(role)));
}
hval hpbi_ui_accent(hval h, hval rgb) {
    return hp_of_int(hpui_ctrl_accent((int64_t)hp_val_to_int(h),
                                      (int64_t)hp_val_to_int(rgb)));
}
hval hpbi_ui_radius(hval h, hval r) {
    return hp_of_int(hpui_ctrl_radius((int64_t)hp_val_to_int(h),
                                      (int64_t)hp_val_to_int(r)));
}
hval hpbi_ui_range(hval h, hval lo, hval hi) {
    return hp_of_int(hpui_range_set((int64_t)hp_val_to_int(h),
                                    (int64_t)hp_val_to_int(lo),
                                    (int64_t)hp_val_to_int(hi)));
}
hval hpbi_ui_enabled(hval h) {
    return hp_of_int(hpui_ctrl_enabled((int64_t)hp_val_to_int(h)));
}
hval hpbi_ui_count(hval h) {
    int64_t n = 0;
    hpui_list_count((int64_t)hp_val_to_int(h), &n);
    return hp_of_int(n);
}
hval hpbi_ui_progress_get(hval h) {
    return hp_of_int(hpui_progress_get((int64_t)hp_val_to_int(h)));
}
hval hpbi_ui_repaint(hval h) {

    return hp_of_int(hpui_repaint((int64_t)hp_val_to_int(h)));
}
/* ui_textw(text, pt, bold) -- real measured width of a string in the font the
 * ui library will draw it with. The library used to estimate this, which left
 * buttons and inputs with visibly uneven padding. */
/* ui_scroll(win, dy, to) -> [offset, contentHeight, viewportHeight] */
hval hpbi_ui_scroll(hval win, hval dy, hval to) {
    int64_t sy = 0, ch = 0;
    int64_t view = hpui_window_scroll((int64_t)hp_val_to_int(win),
                                      (int)hp_val_to_int(dy),
                                      (int)hp_val_to_int(to), &sy, &ch);
    if (view < 0) return hp_null;
    const hval v[3] = { hp_of_int(sy), hp_of_int(ch), hp_of_int(view) };
    return hp_of_arr(hp_arr_of(3, v));
}
/* ui_rect(handle) -> [x, y, w, h] in the parent's logical coordinates. */
hval hpbi_ui_rect(hval h) {
    int64_t x = 0, y = 0, w = 0, ht = 0;
    if (hpui_ctrl_rect((int64_t)hp_val_to_int(h), &x, &y, &w, &ht) != 0)
        return hp_null;
    const hval v[4] = { hp_of_int(x), hp_of_int(y), hp_of_int(w), hp_of_int(ht) };
    return hp_of_arr(hp_arr_of(4, v));
}
hval hpbi_ui_text_px(hval s, hval pt, hval bold) {
    hstr *t = hp_val_to_str(s);
    return hp_of_int(hpui_textw(t ? t->data : "",
                                (int)hp_val_to_int(pt),
                                hp_val_to_int(bold) != 0));
}
hval hpbi_ui_font_new(hval family, hval pt, hval bold, hval italic) {
    hstr *f = hp_val_to_str(family);
    return hp_of_int(hpui_font_id(f ? f->data : "Segoe UI",
                                  (int)hp_val_to_int(pt),
                                  hp_val_to_bool(bold), hp_val_to_bool(italic)));
}
hval hpbi_ui_font_width(hval font_id, hval text) {
    hstr *s = hp_val_to_str(text);
    return hp_of_int(hpui_font_w(hp_val_to_int(font_id), s ? s->data : ""));
}
hval hpbi_ui_font_height(hval font_id) {
    return hp_of_int(hpui_font_h(hp_val_to_int(font_id)));
}
hval hpbi_ui_draw_clear(hval cv, hval rgb) {
    return hp_of_int(hpui_draw_clear(hp_val_to_int(cv), hp_val_to_int(rgb)));
}
hval hpbi_ui_draw_rect(hval cv, hval x, hval y, hval w, hval h, hval rgb) {
    return hp_of_int(hpui_draw_rect(hp_val_to_int(cv), (int)hp_val_to_int(x),
        (int)hp_val_to_int(y), (int)hp_val_to_int(w), (int)hp_val_to_int(h),
        hp_val_to_int(rgb)));
}
hval hpbi_ui_draw_round(hval cv, hval x, hval y, hval w, hval h, hval radius,
                        hval fill, hval stroke, hval sw) {
    return hp_of_int(hpui_draw_round(hp_val_to_int(cv), (int)hp_val_to_int(x),
        (int)hp_val_to_int(y), (int)hp_val_to_int(w), (int)hp_val_to_int(h),
        (int)hp_val_to_int(radius), hp_val_to_int(fill), hp_val_to_int(stroke),
        (int)hp_val_to_int(sw)));
}
hval hpbi_ui_draw_line(hval cv, hval x1, hval y1, hval x2, hval y2, hval rgb, hval w) {
    return hp_of_int(hpui_draw_line(hp_val_to_int(cv), (int)hp_val_to_int(x1),
        (int)hp_val_to_int(y1), (int)hp_val_to_int(x2), (int)hp_val_to_int(y2),
        hp_val_to_int(rgb), (int)hp_val_to_int(w)));
}
hval hpbi_ui_draw_text(hval cv, hval x, hval y, hval w, hval h, hval text,
                       hval rgb, hval font_id, hval flags) {
    hstr *s = hp_val_to_str(text);
    return hp_of_int(hpui_draw_text(hp_val_to_int(cv), (int)hp_val_to_int(x),
        (int)hp_val_to_int(y), (int)hp_val_to_int(w), (int)hp_val_to_int(h),
        s ? s->data : "", hp_val_to_int(rgb), hp_val_to_int(font_id),
        (int)hp_val_to_int(flags)));
}
hval hpbi_ui_draw_clip(hval cv, hval x, hval y, hval w, hval h) {
    return hp_of_int(hpui_draw_clip(hp_val_to_int(cv), (int)hp_val_to_int(x),
        (int)hp_val_to_int(y), (int)hp_val_to_int(w), (int)hp_val_to_int(h)));
}
hval hpbi_ui_draw_unclip(hval cv) {
    return hp_of_int(hpui_draw_unclip(hp_val_to_int(cv)));
}
hval hpbi_ui_canvas_size(hval cv) {
    int64_t w = 0, h = 0;
    if (hpui_canvas_size(hp_val_to_int(cv), &w, &h) != 0) return hp_null;
    const hval size[2] = { hp_of_int(w), hp_of_int(h) };
    return hp_of_arr(hp_arr_of(2, size));
}
hval hpbi_ui_canvas_pixel(hval cv, hval x, hval y) {
    return hp_of_int(hpui_canvas_pixel(hp_val_to_int(cv),
        (int)hp_val_to_int(x), (int)hp_val_to_int(y)));
}
hval hpbi_ui_canvas_invalidate(hval cv) {
    return hp_of_int(hpui_canvas_invalidate(hp_val_to_int(cv)));
}
hval hpbi_ui_canvas_flush(hval cv) {
    return hp_of_int(hpui_canvas_flush(hp_val_to_int(cv)));
}
hval hpbi_ui_cursor(hval cv, hval shape) {
    return hp_of_int(hpui_cursor_set(hp_val_to_int(cv), hp_val_to_int(shape)));
}
/* ui_text_lines(text, pt, bold, maxWidth) -> how many lines it wraps to */
hval hpbi_ui_hwnd(hval win) {
    return hp_of_int(hpui_hwnd((int64_t)hp_val_to_int(win)));
}

/* ================= embedded Chromium (WebView2) =================
 * hphp_wv2.c owns the COM side; these are the thin value marshalling. */

hval hpbi_ui_browser_ready(void) { return hp_of_bool(hpwv2_available()); }

hval hpbi_ui_browser_loader(void) {
    const char *p = hpwv2_loader_path();
    return hp_of_str(hp_str_new(p ? p : "", p ? strlen(p) : 0));
}

hval hpbi_ui_browser_new(hval parent, hval dir) {
    hstr *d = hp_val_to_str(dir);
    return hp_of_int(hpwv2_create((int64_t)hp_val_to_int(parent),
                                 (d && d->len) ? d->data : NULL));
}

hval hpbi_ui_browser_free(hval b) {
    return hp_of_int(hpwv2_destroy((int64_t)hp_val_to_int(b)));
}

hval hpbi_ui_browser_fit(hval b, hval top, hval width) {
    return hp_of_int(hpwv2_fit((int64_t)hp_val_to_int(b), (int)hp_val_to_int(top), (int)hp_val_to_int(width)));
}

hval hpbi_ui_browser_go(hval b, hval url) {
    hstr *s = hp_val_to_str(url);
    return hp_of_int(hpwv2_navigate((int64_t)hp_val_to_int(b), s ? s->data : NULL));
}

hval hpbi_ui_browser_html(hval b, hval html) {
    hstr *s = hp_val_to_str(html);
    return hp_of_int(hpwv2_html((int64_t)hp_val_to_int(b), s ? s->data : NULL));
}

hval hpbi_ui_browser_back(hval b) {
    return hp_of_int(hpwv2_back((int64_t)hp_val_to_int(b)));
}

hval hpbi_ui_browser_forward(hval b) {
    return hp_of_int(hpwv2_forward((int64_t)hp_val_to_int(b)));
}

hval hpbi_ui_browser_reload(hval b) {
    return hp_of_int(hpwv2_reload((int64_t)hp_val_to_int(b)));
}

hval hpbi_ui_browser_stop(hval b) {
    return hp_of_int(hpwv2_stop((int64_t)hp_val_to_int(b)));
}

hval hpbi_ui_browser_can_back(hval b) {
    return hp_of_bool(hpwv2_can_back((int64_t)hp_val_to_int(b)));
}

hval hpbi_ui_browser_can_forward(hval b) {
    return hp_of_bool(hpwv2_can_forward((int64_t)hp_val_to_int(b)));
}

/* C hands back a malloc'd UTF-8 string; the VM takes ownership of an hstr. */
static hval wv2_string(int64_t rc, char *owned) {
    if (owned) return hp_of_str(hp_str_new(owned, strlen(owned)));
    (void)rc;
    return hp_of_str(hp_str_lit(""));
}

hval hpbi_ui_browser_url(hval b) {
    char *out = NULL;
    hpwv2_url((int64_t)hp_val_to_int(b), &out);
    hval r = wv2_string(0, out);
    free(out);
    return r;
}

hval hpbi_ui_browser_title(hval b) {
    char *out = NULL;
    hpwv2_title((int64_t)hp_val_to_int(b), &out);
    hval r = wv2_string(0, out);
    free(out);
    return r;
}

hval hpbi_ui_browser_zoom(hval b, hval factor) {
    return hp_of_int(hpwv2_zoom((int64_t)hp_val_to_int(b), hp_val_to_float(factor)));
}

hval hpbi_ui_browser_zoom_get(hval b) {
    double z = 1.0;
    hpwv2_zoom_get((int64_t)hp_val_to_int(b), &z);
    return hp_of_float(z);
}

hval hpbi_ui_browser_devtools(hval b, hval on) {
    return hp_of_int(hpwv2_devtools((int64_t)hp_val_to_int(b), hp_val_to_bool(on)));
}

hval hpbi_ui_browser_visible(hval b, hval on) {
    return hp_of_int(hpwv2_visible((int64_t)hp_val_to_int(b),
                                   hp_val_to_bool(on)));
}

hval hpbi_ui_browser_key(hval b, hval mod, hval vk, hval cmd, hval cb) {
    return hp_of_int(hpwv2_shortcut((int64_t)hp_val_to_int(b),
                                    (int64_t)hp_val_to_int(mod),
                                    (int64_t)hp_val_to_int(vk),
                                    (int64_t)hp_val_to_int(cmd), cb));
}

hval hpbi_ui_browser_on(hval b, hval ev, hval cb) {
    return hp_of_int(hpwv2_on((int64_t)hp_val_to_int(b),
                              (int64_t)hp_val_to_int(ev), cb));
}

hval hpbi_ui_text_lines(hval s, hval pt, hval bold, hval maxw) {
    hstr *t = hp_val_to_str(s);
    return hp_of_int(hpui_text_lines(t ? t->data : "",
                                     (int)hp_val_to_int(pt),
                                     hp_val_to_int(bold) != 0,
                                     (int)hp_val_to_int(maxw)));
}
/* ui_theme(win, bg, surface, elevated, text, muted, accent, onAccent, border,
 *         radius, fontPt, titlePt, rowH, dark) */
hval hpbi_ui_theme(hval win, hval bg, hval surface, hval elevated, hval text,
                   hval muted, hval accent, hval onAccent, hval border,
                   hval radius, hval fontPt, hval titlePt, hval rowH, hval dark) {
    int64_t v[14];
    v[0]  = hp_val_to_int(bg);
    v[1]  = hp_val_to_int(surface);
    v[2]  = hp_val_to_int(elevated);
    v[3]  = hp_val_to_int(text);
    v[4]  = hp_val_to_int(muted);
    v[5]  = hp_val_to_int(accent);
    v[6]  = hp_val_to_int(onAccent);
    v[7]  = hp_val_to_int(border);
    v[8]  = hp_val_to_int(radius);
    v[9]  = hp_val_to_int(fontPt);
    v[10] = hp_val_to_int(titlePt);
    v[11] = hp_val_to_int(rowH);
    v[12] = 0;
    v[13] = hp_val_to_bool(dark) ? 1 : 0;
    return hp_of_int(hpui_theme_set((int64_t)hp_val_to_int(win), v, 14));
}

/* ---------- PHP 8 parity additions ---------- */
#include <errno.h>
#include <sys/stat.h>
#if !defined(_WIN32)
#define _strnicmp strncasecmp
#endif

/* ---- sorting with callback comparators (usort/uasort/uksort) ---- */
static void hp_sort_with_fn(harr *a, bool by_key, hval fn) {
    if (fn.tag != HV_CLO || a->len < 2) return;
    /* insertion sort — stable, fine for the sizes PHP code sees */
    for (size_t i = 1; i < a->len; i++) {
        hval tv = a->vals[i];
        hval tk = a->keys ? a->keys[i] : hp_null;
        size_t j = i;
        while (j > 0) {
            hval A = by_key ? (a->keys ? a->keys[j - 1] : hp_of_int((int64_t)j - 1)) : a->vals[j - 1];
            hval B = by_key ? tk : tv;
            hval args[2] = { A, B };
            hval r = hp_closure_call(fn.u.p, 2, args);
            if (hp_val_to_int(r) > 0) {
                a->vals[j] = a->vals[j - 1];
                if (a->keys) a->keys[j] = a->keys[j - 1];
                j--;
            } else break;
        }
        a->vals[j] = tv;
        if (a->keys) a->keys[j] = tk;
    }
}
hval hpbi_usort(hval a, hval fn) {
    if (a.tag == HV_ARR && !a.u.a->is_map) hp_sort_with_fn(a.u.a, false, fn);
    RETB(true);
}
hval hpbi_uasort(hval a, hval fn) {
    if (a.tag == HV_ARR) hp_sort_with_fn(a.u.a, false, fn);
    RETB(true);
}
hval hpbi_uksort(hval a, hval fn) {
    if (a.tag == HV_ARR) hp_sort_with_fn(a.u.a, true, fn);
    RETB(true);
}
static void hp_walk(hval v, hval fn, bool recursive) {
    if (v.tag != HV_ARR) return;
    harr *a = v.u.a;
    for (size_t i = 0; i < a->len; i++) {
        hval item = a->vals[i];
        if (recursive && item.tag == HV_ARR) {
            hp_walk(item, fn, true);
            continue;
        }
        if (fn.tag != HV_CLO) continue;
        hval args[2] = { item, a->is_map ? a->keys[i] : hp_of_int((int64_t)i) };
        (void)hp_closure_call(fn.u.p, 2, args);
    }
}
hval hpbi_array_walk(hval a, hval fn, hval userdata) {
    hp_walk(a, fn, false);
    RETB(true);
}
hval hpbi_array_walk_recursive(hval a, hval fn, hval userdata) {
    hp_walk(a, fn, true);
    RETB(true);
}

/* ---- more array functions ---- */
hval hpbi_array_column(hval rows, hval col, hval index_key) {
    harr *r = hp_arr_new();
    if (rows.tag != HV_ARR) return hp_of_arr(r);
    for (size_t i = 0; i < rows.u.a->len; i++) {
        hval row = rows.u.a->vals[i];
        if (row.tag != HV_ARR) continue;
        hval v = hp_arr_get(row.u.a, col);
        if (index_key.tag != HV_NULL && hp_arr_has(row.u.a, index_key)) {
            if (!r->is_map) r->is_map = true;
            hp_arr_set(r, hp_arr_get(row.u.a, index_key), v);
        } else {
            hp_arr_push(r, v);
        }
    }
    return hp_of_arr(r);
}
hval hpbi_array_chunk(hval a, hval size, hval preserve) {
    harr *r = hp_arr_new();
    if (a.tag != HV_ARR) return hp_of_arr(r);
    int64_t sz = hp_val_to_int(size);
    if (sz < 1) sz = 1;
    bool pk = preserve.tag == HV_INT && preserve.u.i != 0;
    harr *cur = hp_arr_new();
    if (pk) cur->is_map = true;
    for (size_t i = 0; i < a.u.a->len; i++) {
        if (pk) hp_arr_set(cur, a.u.a->keys[i], a.u.a->vals[i]);
        else hp_arr_push(cur, a.u.a->vals[i]);
        if ((int64_t)cur->len >= sz) {
            hp_arr_push(r, hp_of_arr(cur));
            cur = hp_arr_new();
            if (pk) cur->is_map = true;
        }
    }
    if (cur->len) hp_arr_push(r, hp_of_arr(cur));
    return hp_of_arr(r);
}
hval hpbi_array_pad(hval a, hval size, hval v) {
    harr *r = hp_arr_new();
    if (a.tag == HV_ARR)
        for (size_t i = 0; i < a.u.a->len; i++) hp_arr_push(r, a.u.a->vals[i]);
    int64_t want = hp_val_to_int(size);
    if (want >= 0)
        while ((int64_t)r->len < want) hp_arr_push(r, v);
    else {
        while ((int64_t)r->len < -want) { harr *t = hp_arr_new(); hp_arr_push(t, v); for (size_t i = 0; i < r->len; i++) hp_arr_push(t, r->vals[i]); r = t; }
    }
    return hp_of_arr(r);
}
hval hpbi_array_replace(hval a, hval b, hval c, hval d) {
    if (a.tag != HV_ARR) return b;
    harr *out = hp_arr_clone(a.u.a);
    hval rest[3] = { b, c, d };
    for (int i = 0; i < 3; i++) {
        if (rest[i].tag != HV_ARR) continue;
        harr *x = rest[i].u.a;
        for (size_t k = 0; k < x->len; k++)
            hp_arr_set(out, x->keys ? x->keys[k] : hp_of_int((int64_t)k), x->vals[k]);
    }
    return hp_of_arr(out);
}
hval hpbi_array_fill_keys(hval keys, hval v) {
    harr *r = hp_arr_new();
    r->is_map = true;
    if (keys.tag == HV_ARR)
        for (size_t i = 0; i < keys.u.a->len; i++)
            hp_arr_set(r, keys.u.a->vals[i], v);
    return hp_of_arr(r);
}
hval hpbi_array_key_first(hval a) {
    if (a.tag != HV_ARR || a.u.a->len == 0) RETB(false);
    return a.u.a->is_map ? a.u.a->keys[0] : hp_of_int(0);
}
hval hpbi_array_key_last(hval a) {
    if (a.tag != HV_ARR || a.u.a->len == 0) RETB(false);
    return a.u.a->is_map ? a.u.a->keys[a.u.a->len - 1]
                         : hp_of_int((int64_t)a.u.a->len - 1);
}
hval hpbi_compact(hval a, hval b, hval c, hval d) {
    /* codegen pads to 4 slots; unused trailing slots are hp_null.
     * Variable names are static strings (HPHP has no symbol table at
     * runtime), so compact() maps "name" => value pairs it is given. */
    harr *r = hp_arr_new();
    r->is_map = true;
    hval slots[4] = { a, b, c, d };
    for (int i = 0; i < 4; i++) {
        if (slots[i].tag == HV_ARR) {
            harr *x = slots[i].u.a;
            for (size_t k = 0; k < x->len; k++) hp_arr_set(r, x->keys[k], x->vals[k]);
        }
    }
    return hp_of_arr(r);
}

/* ---- more string functions ---- */
hval hpbi_vsprintf(hval fmt, hval args) { return hpbi_sprintf(fmt, args); }
hval hpbi_substr_count(hval hay, hval needle) {
    hstr *h = hp_val_to_str(hay), *n = hp_val_to_str(needle);
    if (n->len == 0 || n->len > h->len) RETI(0);
    int64_t cnt = 0;
    for (size_t i = 0; i + n->len <= h->len; i++)
        if (memcmp(h->data + i, n->data, n->len) == 0) { cnt++; i += n->len - 1; }
    RETI(cnt);
}
hval hpbi_str_shuffle(hval s) {
    hstr *x = hp_str_copy(hp_val_to_str(s));
    for (size_t i = x->len; i > 1; i--) {
        size_t j = (size_t)hp_rand_range(0, (int64_t)i - 1);
        char t = x->data[i - 1]; x->data[i - 1] = x->data[j]; x->data[j] = t;
    }
    RETS(x);
}
hval hpbi_strip_tags(hval s, hval allowed) {
    hstr *x = hp_val_to_str(s);
    Buf b;
    buf_init(&b);
    bool intag = false;
    for (size_t i = 0; i < x->len; i++) {
        char c = x->data[i];
        if (c == '<') intag = true;
        else if (c == '>') intag = false;
        else if (!intag) buf_putc(&b, c);
    }
    RETS(hp_str_take_buf(&b));
}
hval hpbi_html_entity_decode(hval s) {
    hstr *r = hp_replace_c(hp_val_to_str(s), "&amp;", "&");
    hstr *n = hp_replace_c(r, "&lt;", "<");
    hp_str_free(r); r = n;
    n = hp_replace_c(r, "&gt;", ">");
    hp_str_free(r); r = n;
    n = hp_replace_c(r, "&quot;", "\"");
    hp_str_free(r); r = n;
    n = hp_replace_c(r, "&#39;", "'");
    hp_str_free(r); r = n;
    RETS(r);
}

/* ---- filesystem additions ---- */
hval hpbi_mkdir(hval path, hval recursive, hval mode) {
    hstr *p = hp_val_to_str(path);
    bool rec = recursive.tag == HV_INT && recursive.u.i != 0;
#if defined(_WIN32)
    if (!rec) RETB(_mkdir(p->data) == 0 || errno == EEXIST);
    char tmp[MAX_PATH];
    snprintf(tmp, sizeof tmp, "%s", p->data);
    for (char *q = tmp + 1; *q; q++) {
        if (*q == '/' || *q == '\\') {
            char c = *q; *q = 0;
            _mkdir(tmp);
            *q = c;
        }
    }
    RETB(_mkdir(tmp) == 0 || errno == EEXIST);
#else
    if (!rec) RETB(mkdir(p->data, 0777) == 0 || errno == EEXIST);
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s", p->data);
    for (char *q = tmp + 1; *q; q++) {
        if (*q == '/') {
            *q = 0;
            mkdir(tmp, 0777);
            *q = '/';
        }
    }
    RETB(mkdir(tmp, 0777) == 0 || errno == EEXIST);
#endif
}
hval hpbi_dirname(hval path) {
    hstr *p = hp_val_to_str(path);
    size_t end = p->len;
    while (end > 0 && (p->data[end - 1] == '/' || p->data[end - 1] == '\\')) end--;
    size_t cut = end;
    while (cut > 0 && p->data[cut - 1] != '/' && p->data[cut - 1] != '\\') cut--;
    while (cut > 0 && (p->data[cut - 1] == '/' || p->data[cut - 1] == '\\')) cut--;
    if (cut == 0) RETS(hp_str_lit("."));
    RETS(hp_str_new(p->data, cut));
}
hval hpbi_basename(hval path, hval suffix) {
    hstr *p = hp_val_to_str(path);
    size_t end = p->len;
    while (end > 0 && (p->data[end - 1] == '/' || p->data[end - 1] == '\\')) end--;
    size_t start = end;
    while (start > 0 && p->data[start - 1] != '/' && p->data[start - 1] != '\\') start--;
    size_t blen = end - start;
    hstr *r = hp_str_new(p->data + start, blen);
    if (suffix.tag == HV_STR && suffix.u.s->len && r->len > suffix.u.s->len &&
        memcmp(r->data + r->len - suffix.u.s->len, suffix.u.s->data, suffix.u.s->len) == 0) {
        hstr *trimmed = hp_str_new(r->data, r->len - suffix.u.s->len);
        hp_str_free(r);
        r = trimmed;
    }
    RETS(r);
}
hval hpbi_pathinfo(hval path, hval flags) {
    hstr *p = hp_val_to_str(path);
    hstr *base = hp_val_to_str(hpbi_basename(hp_of_str(p), hp_null));
    harr *m = hp_arr_new();
    m->is_map = true;
    hp_arr_set(m, hp_of_str(hp_str_lit("basename")), hp_of_str(base));
    hp_arr_set_take(m, hp_of_str(hp_str_lit("dirname")), hpbi_dirname(hp_of_str(p)));
    const char *dot = memchr(base->data, '.', base->len);
    if (dot && dot != base->data) {
        hp_arr_set_take(m, hp_of_str(hp_str_lit("extension")),
                   hp_of_str(hp_str_new(dot + 1, base->len - (size_t)(dot - base->data) - 1)));
        hp_arr_set_take(m, hp_of_str(hp_str_lit("filename")),
                   hp_of_str(hp_str_new(base->data, (size_t)(dot - base->data))));
    }
    if (flags.tag == HV_INT && flags.u.i == 2) {
        hp_arr_free(m);   /* the map is not returned in this mode */
        return hp_of_str(base);
    }
    hp_val_rc_dec(hp_of_str(base));
    return hp_of_arr(m);
}
hval hpbi_realpath(hval path) {
    hstr *p = hp_val_to_str(path);
#if defined(_WIN32)
    char out[MAX_PATH];
    if (!_fullpath(out, p->data, sizeof out)) RETB(false);
    RETS(hp_str_cstr(out));
#else
    char out[4096];
    if (!realpath(p->data, out)) RETB(false);
    RETS(hp_str_cstr(out));
#endif
}
hval hpbi_is_file(hval path) {
    hstr *p = hp_val_to_str(path);
#if defined(_WIN32)
    struct _stat st;
    RETB(_stat(p->data, &st) == 0 && (st.st_mode & _S_IFREG));
#else
    struct stat st;
    RETB(stat(p->data, &st) == 0 && S_ISREG(st.st_mode));
#endif
}
hval hpbi_is_readable(hval path) {
    FILE *f = fopen(hp_val_to_str(path)->data, "rb");
    if (f) { fclose(f); RETB(true); }
    RETB(false);
}
hval hpbi_is_writable(hval path) {
    hstr *p = hp_val_to_str(path);
    FILE *f = fopen(p->data, "r+");
    if (f) { fclose(f); RETB(true); }
    /* file may not exist: check the directory */
    hstr *dir = hp_val_to_str(hpbi_dirname(hp_of_str(p)));
    f = fopen(dir->data, "rb");
    hp_str_free(dir);
    if (f) { fclose(f); RETB(true); }
    RETB(false);
}
hval hpbi_touch(hval path) {
    FILE *f = fopen(hp_val_to_str(path)->data, "ab");
    if (f) { fclose(f); RETB(true); }
    RETB(false);
}
/* tiny * / ? wildcard matcher (used by glob on all platforms) */
static bool wc_match(const char *pat, const char *s) {
    while (*pat) {
        if (*pat == '*') {
            pat++;
            if (!*pat) return true;
            for (const char *t = s; ; t++) {
                if (wc_match(pat, t)) return true;
                if (!*t) return false;
            }
        }
        if (!*s) return false;
        if (*pat != '?' && *pat != *s) return false;
        pat++; s++;
    }
    return *s == 0;
}
hval hpbi_glob(hval pattern) {
    hstr *pat = hp_val_to_str(pattern);
    harr *out = hp_arr_new();
    hstr *dir = hp_val_to_str(hpbi_dirname(hp_of_str(pat)));
    hstr *base = hp_val_to_str(hpbi_basename(hp_of_str(pat), hp_null));
    const char *bpat = base->len ? base->data : "*";
    hval files = hpbi_scandir(hp_of_str(dir));
    if (files.tag != HV_ARR) return hp_of_arr(out);
    for (size_t i = 0; i < files.u.a->len; i++) {
        hstr *n = files.u.a->vals[i].u.s;
        if (wc_match(bpat, n->data))
            hp_arr_push(out, files.u.a->vals[i]);
    }
    /* sort alphabetically like PHP glob */
    for (size_t i = 1; i < out->len; i++) {
        hval tv = out->vals[i];
        size_t j = i;
        while (j > 0 && hp_str_cmp(out->vals[j - 1].u.s, tv.u.s) > 0) {
            out->vals[j] = out->vals[j - 1];
            j--;
        }
        out->vals[j] = tv;
    }
    return hp_of_arr(out);
}
hval hpbi_sys_get_temp_dir(void) {
    const char *t = getenv("TEMP");
    if (!t) t = getenv("TMP");
    if (!t) t = "/tmp";
    RETS(hp_str_lit(t));
}
hval hpbi_checkdate(hval m, hval d, hval y) {
    int64_t mo = hp_val_to_int(m), da = hp_val_to_int(d), yr = hp_val_to_int(y);
    if (mo < 1 || mo > 12 || da < 1 || yr < 1) RETB(false);
    static const int md[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int64_t lim = md[mo - 1];
    if (mo == 2 && ((yr % 4 == 0 && yr % 100 != 0) || yr % 400 == 0)) lim = 29;
    RETB(da <= lim);
}

/* ---- HTTP client (http only; no TLS in the runtime) ----
 * http_get(url) -> body string | false
 * http_request(url[, method[, body[, headers_map]]])
 *   -> {status, body, contentType} map | false */
static void http_ws_init(void) {
#if defined(_WIN32)
    static bool ws_up = false;
    if (!ws_up) {
        WSADATA w;
        WSAStartup(MAKEWORD(2, 2), &w);
        ws_up = true;
    }
#endif
}
static hval http_perform(const char *host, int port, const char *path,
                         const char *method, hstr *body, harr *hdrs) {
    http_ws_init();
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char portstr[16];
    snprintf(portstr, sizeof portstr, "%d", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) RETB(false);
    hp_sock s = HP_SOCK_BAD;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        hp_sock fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == HP_SOCK_BAD) continue;
        if (connect(fd, ai->ai_addr, (int)ai->ai_addrlen) == 0) { s = fd; break; }
        hp_sock_close(fd);
    }
    freeaddrinfo(res);
    if (s == HP_SOCK_BAD) RETB(false);
    Buf req;
    buf_init(&req);
    buf_printf(&req, "%s /%s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n",
               method, path, host);
    if (hdrs)
        for (size_t i = 0; i < hdrs->len; i++) {
            hstr *h = hp_val_to_str(hdrs->vals[i]);
            buf_write(&req, h->data, h->len);
            buf_write(&req, "\r\n", 2);
        }
    if (body && body->len) {
        buf_printf(&req, "Content-Length: %zu\r\n", body->len);
        buf_write(&req, "\r\n", 2);
        buf_write(&req, body->data, body->len);
    } else {
        buf_write(&req, "\r\n", 2);
    }
    size_t total = 0;
    while (total < req.len) {
        int w = (int)send(s, req.data + total, (int)(req.len - total), 0);
        if (w <= 0) break;
        total += (size_t)w;
    }
    free(req.data);
    Buf resp;
    buf_init(&resp);
    char rbuf[8192];
    for (;;) {
        int n = (int)recv(s, rbuf, sizeof rbuf, 0);
        if (n <= 0) break;
        hbuf_write(&resp, rbuf, (size_t)n);
    }
    hp_sock_close(s);
    char *raw = hbuf_take(&resp);
    /* split head/body, follow nothing here (redirects handled by caller) */
    char *sep = strstr(raw, "\r\n\r\n");
    long status = 0;
    hstr *ctype = hp_str_lit("");
    if (sep) {
        if (strncmp(raw, "HTTP/1.", 7) == 0) status = strtol(raw + 9, NULL, 10);
        for (char *line = raw; line < sep; ) {
            char *e = strstr(line, "\r\n");
            if (!e || e > sep) break;
            if (_strnicmp(line, "Content-Type:", 13) == 0)
                ctype = hp_str_new(line + 13, (size_t)(e - line - 13));
            line = e + 2;
        }
        *sep = 0;
    }
    hstr *respbody = hp_str_cstr(sep ? sep + 4 : raw);
    free(raw);
    if (status == 0) RETB(false);
    harr *m = hp_arr_new();
    m->is_map = true;
    hp_arr_set(m, hp_of_str(hp_str_lit("status")), hp_of_int(status));
    hp_arr_set(m, hp_of_str(hp_str_lit("body")), hp_of_str(respbody));
    hp_arr_set(m, hp_of_str(hp_str_lit("contentType")), hp_of_str(ctype));
    return hp_of_arr(m);
}
static bool http_parse_url(const char *url, char *host, size_t hsz,
                           int *port, const char **path) {
    if (strncmp(url, "http://", 7) != 0) return false;   /* https: no TLS */
    url += 7;
    const char *slash = strchr(url, '/');
    size_t hl = slash ? (size_t)(slash - url) : strlen(url);
    if (hl == 0 || hl >= hsz) return false;
    memcpy(host, url, hl);
    host[hl] = 0;
    *port = 80;
    char *colon = strchr(host, ':');
    if (colon) { *colon = 0; *port = atoi(colon + 1); }
    *path = slash ? slash + 1 : "";
    return true;
}
hval hpbi_http_get(hval url) {
    hstr *u = hp_val_to_str(url);
    char host[256];
    int port;
    const char *path;
    if (!http_parse_url(u->data, host, sizeof host, &port, &path)) RETB(false);
    return http_perform(host, port, path, "GET", NULL, NULL);
}
hval hpbi_http_request(hval url, hval method, hval body, hval headers) {
    hstr *u = hp_val_to_str(url);
    char host[256];
    int port;
    const char *path;
    if (!http_parse_url(u->data, host, sizeof host, &port, &path)) RETB(false);
    const char *m = method.tag == HV_STR && method.u.s->len ? method.u.s->data : "GET";
    hstr *b = body.tag == HV_STR ? body.u.s : NULL;
    harr *hs = headers.tag == HV_ARR ? headers.u.a : NULL;
    return http_perform(host, port, path, m, b, hs);
}

/* ---- local PtrVec (runtime copy; compiler-side one lives in util.h) ---- */
typedef struct {
    void **items;
    size_t len, cap;
} RPtrVec;

static void rptrvec_push(RPtrVec *v, void *p) {
    if (v->len == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 8;
        v->items = (void **)realloc(v->items, v->cap * sizeof(void *));
    }
    v->items[v->len++] = p;
}
/* ===================== PCRE-style regex engine =====================
 * A compact backtracking matcher (recursive descent over the pattern
 * tree, continuation-passing for backtracking). Supported syntax:
 *   literals  .  [abc]  [^abc]  [a-z]  \d \D \w \W \s \S \b
 *   x* x+ x?  x{m} x{m,} x{m,n}  (greedy; trailing '?' = lazy)
 *   (...) capture   (?:...) non-capturing   | alternation
 *   ^ $ anchors     delimiters /pat/flags with flags i, s, m
 * PHP-shaped builtins on top: preg_match, preg_match_all, preg_replace,
 * preg_split, preg_grep.
 */

typedef enum { RX_CHAR, RX_ANY, RX_CLASS, RX_BOL, RX_EOL, RX_BOW,
               RX_SEQ, RX_ALT, RX_REP, RX_GROUP } RxKind;

typedef struct Rx {
    RxKind kind;
    unsigned char ch;          /* RX_CHAR */
    bool neg;                  /* RX_CLASS negated */
    bool ci;                   /* case-insensitive (chars + classes) */
    bool dotall;               /* '.' matches \n too */
    bool multiline;            /* ^ $ also at \n boundaries */
    bool lazy;                 /* non-greedy quantifier */
    unsigned char set[32];     /* RX_CLASS bitmap */
    RPtrVec kids;               /* RX_SEQ / RX_ALT: Rx* items */
    struct Rx *child;          /* RX_REP / RX_GROUP body (SEQ) */
    int rmin, rmax;            /* RX_REP: rmax -1 = unbounded */
    int gidx;                  /* RX_GROUP capture index, 0 = (?:...) */
} Rx;

static void rx_free(Rx *r) {
    if (!r) return;
    for (size_t i = 0; i < r->kids.len; i++) rx_free((Rx *)r->kids.items[i]);
    free(r->kids.items);
    rx_free(r->child);
    free(r);
}

static Rx *rx_new(RxKind k) {
    Rx *r = (Rx *)calloc(1, sizeof(Rx));
    r->kind = k;
    return r;
}

static void rx_set_add(Rx *r, unsigned char c, bool ci) {
    r->set[c >> 3] |= (unsigned char)(1 << (c & 7));
    if (ci && isalpha(c)) {
        unsigned char o = (unsigned char)(isupper(c) ? tolower(c) : toupper(c));
        r->set[o >> 3] |= (unsigned char)(1 << (o & 7));
    }
}

static bool rx_is_word(unsigned char c) {
    return isalnum(c) || c == '_';
}

typedef struct {
    const char *src;
    size_t len, pos;
    bool ci, dotall, multiline;
    int ngroups;
    bool err;
} RxParser;

static Rx *rx_parse_alt(RxParser *rp);

static bool rx_at_end(RxParser *rp) {
    return rp->err || rp->pos >= rp->len;
}

static char rx_peek(RxParser *rp) {
    return rp->pos < rp->len ? rp->src[rp->pos] : '\0';
}

static char rx_next(RxParser *rp) {
    return rp->pos < rp->len ? rp->src[rp->pos++] : '\0';
}

static void rx_fail(RxParser *rp) { rp->err = true; }

static int rx_escape_class(RxParser *rp, Rx *atom, char c) {
    /* returns 1 if c names a class (\d \w \s or negated); atom may be NULL
     * to only test (dry run) */
    bool neg = false;
    if (isupper((unsigned char)c)) { neg = true; c = (char)tolower((unsigned char)c); }
    int which = c == 'd' ? 1 : c == 'w' ? 2 : c == 's' ? 3 : 0;
    if (!which) return 0;
    if (!atom) return 1;
    for (int i = 0; i < 256; i++) {
        unsigned char ch = (unsigned char)i;
        bool in = which == 1 ? isdigit(ch) : which == 2 ? rx_is_word(ch) : isspace(ch);
        if (in == !neg) rx_set_add(atom, ch, rp->ci);
    }
    return 1;
}

static Rx *rx_parse_atom(RxParser *rp) {
    char c = rx_next(rp);
    switch (c) {
    case '(': {
        bool capture = true;
        if (rx_peek(rp) == '?') {
            rx_next(rp);
            if (rx_peek(rp) == ':') { rx_next(rp); capture = false; }
            else { rx_fail(rp); return NULL; }   /* (?=) etc unsupported */
        }
        Rx *g = rx_new(RX_GROUP);
        g->ci = rp->ci; g->dotall = rp->dotall; g->multiline = rp->multiline;
        g->gidx = capture ? ++rp->ngroups : 0;
        g->child = rx_new(RX_SEQ);
        g->child->ci = rp->ci;
        Rx *inner = rx_parse_alt(rp);
        if (rp->err) return NULL;
        rptrvec_push(&g->child->kids, inner);
        if (rx_next(rp) != ')') { rx_fail(rp); return NULL; }
        return g;
    }
    case '[': {
        Rx *cl = rx_new(RX_CLASS);
        cl->ci = rp->ci; cl->dotall = rp->dotall; cl->multiline = rp->multiline;
        if (rx_peek(rp) == '^') { rx_next(rp); cl->neg = true; }
        bool first = true;
        while (!rx_at_end(rp) && (rx_peek(rp) != ']' || first)) {
            first = false;
            char lo = rx_next(rp);
            if (lo == '\\') {
                char e = rx_next(rp);
                if (rx_escape_class(rp, cl, e)) continue;
                lo = e;   /* \. \/ \\ ... literal */
            }
            if (rx_peek(rp) == '-' && rp->pos + 1 < rp->len &&
                rp->src[rp->pos + 1] != ']') {
                rx_next(rp);   /* '-' */
                char hi = rx_next(rp);
                if (hi == '\\') hi = rx_next(rp);
                for (int i = (unsigned char)lo; i <= (unsigned char)hi; i++)
                    rx_set_add(cl, (unsigned char)i, rp->ci);
            } else {
                rx_set_add(cl, (unsigned char)lo, rp->ci);
            }
        }
        if (rx_peek(rp) != ']') { rx_fail(rp); return NULL; }
        rx_next(rp);
        return cl;
    }
    case '.': {
        Rx *a = rx_new(RX_ANY);
        a->ci = rp->ci; a->dotall = rp->dotall; a->multiline = rp->multiline;
        return a;
    }
    case '^': {
        Rx *a = rx_new(RX_BOL);
        a->ci = rp->ci; a->multiline = rp->multiline;
        return a;
    }
    case '$': {
        Rx *a = rx_new(RX_EOL);
        a->ci = rp->ci; a->multiline = rp->multiline;
        return a;
    }
    case '\\': {
        char e = rx_next(rp);
        Rx *a = NULL;
        if (rx_escape_class(rp, NULL, e)) {
            a = rx_new(RX_CLASS);
            a->ci = rp->ci; a->dotall = rp->dotall; a->multiline = rp->multiline;
            rx_escape_class(rp, a, e);
            return a;
        }
        if (e == 'b') { a = rx_new(RX_BOW); a->ci = rp->ci; return a; }
        if (e == 'n') e = '\n';
        else if (e == 't') e = '\t';
        else if (e == 'r') e = '\r';
        else if (e == '0') e = '\0';
        a = rx_new(RX_CHAR);
        a->ch = (unsigned char)e;
        a->ci = rp->ci;
        return a;
    }
    case '*': case '+': case '?':
        rx_fail(rp); return NULL;   /* quantifier with nothing to repeat */
    default: {
        Rx *a = rx_new(RX_CHAR);
        a->ch = (unsigned char)c;
        a->ci = rp->ci;
        return a;
    }
    }
}

static Rx *rx_parse_rep(RxParser *rp) {
    Rx *atom = rx_parse_atom(rp);
    if (rp->err || !atom) return atom;
    char c = rx_peek(rp);
    if (c == '*' || c == '+' || c == '?' || c == '{') {
        Rx *r = rx_new(RX_REP);
        r->ci = rp->ci; r->dotall = rp->dotall; r->multiline = rp->multiline;
        r->child = atom;
        if (c == '*') { r->rmin = 0; r->rmax = -1; rx_next(rp); }
        else if (c == '+') { r->rmin = 1; r->rmax = -1; rx_next(rp); }
        else if (c == '?') { r->rmin = 0; r->rmax = 1; rx_next(rp); }
        else {
            /* {m}, {m,}, {m,n} */
            rx_next(rp);
            int mn = 0, mx = -1;
            while (isdigit((unsigned char)rx_peek(rp))) mn = mn * 10 + (rx_next(rp) - '0');
            if (rx_peek(rp) == ',') {
                rx_next(rp);
                if (isdigit((unsigned char)rx_peek(rp))) {
                    mx = 0;
                    while (isdigit((unsigned char)rx_peek(rp))) mx = mx * 10 + (rx_next(rp) - '0');
                }
            } else mx = mn;
            if (rx_next(rp) != '}') { rx_fail(rp); return NULL; }
            r->rmin = mn; r->rmax = mx;
        }
        if (rx_peek(rp) == '?') { rx_next(rp); r->lazy = true; }
        return r;
    }
    return atom;
}

static Rx *rx_parse_seq(RxParser *rp) {
    Rx *s = rx_new(RX_SEQ);
    s->ci = rp->ci; s->dotall = rp->dotall; s->multiline = rp->multiline;
    while (!rx_at_end(rp) && rx_peek(rp) != '|' && rx_peek(rp) != ')') {
        Rx *item = rx_parse_rep(rp);
        if (rp->err || !item) return NULL;
        rptrvec_push(&s->kids, item);
    }
    return s;
}

static Rx *rx_parse_alt(RxParser *rp) {
    Rx *first = rx_parse_seq(rp);
    if (rp->err) return NULL;
    if (rx_peek(rp) != '|') return first;
    Rx *alt = rx_new(RX_ALT);
    alt->ci = rp->ci; alt->dotall = rp->dotall; alt->multiline = rp->multiline;
    rptrvec_push(&alt->kids, first);
    while (rx_peek(rp) == '|') {
        rx_next(rp);
        Rx *b = rx_parse_seq(rp);
        if (rp->err || !b) return NULL;
        rptrvec_push(&alt->kids, b);
    }
    return alt;
}

/* split "/pat/flags" into body + flags; also accepts bare patterns */
static bool rx_split_pattern(hstr *pat, Rx **out, int *ngroups) {
    const char *s = pat->data;
    size_t n = pat->len;
    const char *body = s;
    size_t blen = n;
    char delim = 0;
    if (n >= 2 && !isalnum((unsigned char)s[0]) && s[0] != '\\' &&
        !isspace((unsigned char)s[0]))
        delim = s[0];
    bool ci = false, dotall = false, multiline = false;
    if (delim) {
        size_t end = n;
        for (size_t i = n; i >= 2; i--) {
            if (s[i - 1] == delim) { end = i - 1; break; }
        }
        if (end >= 2) {
            body = s + 1;
            blen = end - 1;
            for (size_t i = end + 1; i < n; i++) {
                if (s[i] == 'i') ci = true;
                else if (s[i] == 's') dotall = true;
                else if (s[i] == 'm') multiline = true;
            }
        }
    }
    RxParser rp = { body, blen, 0, ci, dotall, multiline, 0, false };
    Rx *tree = rx_parse_alt(&rp);
    if (rp.err || !tree || rp.pos != blen) return false;
    tree->ci = ci; tree->dotall = dotall; tree->multiline = multiline;
    /* propagate flags through the tree */
    /* (atoms captured flags at parse time; root seq too) */
    *out = tree;
    *ngroups = rp.ngroups;
    return true;
}

typedef struct {
    const char *s;
    size_t len;
    size_t cap_s[32], cap_e[32];
    int ngroups;
    size_t end;                /* match end, set when C_DONE reached */
} RxCtx;

typedef struct Cont Cont;

struct Cont {
    enum { C_DONE, C_SEQ, C_REP, C_GRP } tag;
    Rx *seq; size_t idx;
    Rx *rep; int count;
    Rx *grp; size_t start;
    Cont *next;
};

static bool rx_m(Rx *n, size_t pos, RxCtx *c, Cont *k);
static bool rx_m_seq(Rx *seq, size_t idx, size_t pos, RxCtx *c, Cont *k);
static bool rx_m_rep_at(Rx *n, int count, size_t pos, RxCtx *c, Cont *k);
static bool rx_k(Cont *k, size_t pos, RxCtx *c);

static bool rx_m(Rx *n, size_t pos, RxCtx *c, Cont *k) {
    switch (n->kind) {
    case RX_CHAR:
        if (pos >= c->len) return false;
        if (n->ci) {
            if (tolower((unsigned char)c->s[pos]) != tolower(n->ch)) return false;
        } else if ((unsigned char)c->s[pos] != n->ch) return false;
        return rx_k(k, pos + 1, c);
    case RX_ANY:
        if (pos >= c->len) return false;
        if (!n->dotall && c->s[pos] == '\n') return false;
        return rx_k(k, pos + 1, c);
    case RX_CLASS: {
        if (pos >= c->len) return false;
        unsigned char ch = (unsigned char)c->s[pos];
        bool in = (n->set[ch >> 3] >> (ch & 7)) & 1;
        if (in == n->neg) return false;
        return rx_k(k, pos + 1, c);
    }
    case RX_BOL:
        if (pos == 0 || (n->multiline && c->s[pos - 1] == '\n')) return rx_k(k, pos, c);
        return false;
    case RX_EOL:
        if (pos == c->len || (n->multiline && c->s[pos] == '\n')) return rx_k(k, pos, c);
        return false;
    case RX_BOW: {
        bool before = pos > 0 && rx_is_word((unsigned char)c->s[pos - 1]);
        bool after = pos < c->len && rx_is_word((unsigned char)c->s[pos]);
        return before != after ? rx_k(k, pos, c) : false;
    }
    case RX_GROUP: {
        if (!n->gidx) return rx_m_seq(n->child, 0, pos, c, k);
        Cont step = { C_GRP, NULL, 0, NULL, 0, n, pos, k };
        return rx_m_seq(n->child, 0, pos, c, &step);
    }
    case RX_SEQ:
        return rx_m_seq(n, 0, pos, c, k);
    case RX_ALT:
        for (size_t i = 0; i < n->kids.len; i++)
            if (rx_m_seq((Rx *)n->kids.items[i], 0, pos, c, k)) return true;
        return false;
    case RX_REP:
        return rx_m_rep_at(n, 0, pos, c, k);
    }
    return false;
}


static bool rx_m_rep_at(Rx *n, int count, size_t pos, RxCtx *c, Cont *k) {
    int iter_cap = (int)c->len + n->rmin + 2;   /* empty-match guard */
    bool can_more = n->rmax < 0 || count + 1 <= n->rmax;
    bool can_stop = count >= n->rmin;
    if (n->lazy) {
        if (can_stop && rx_k(k, pos, c)) return true;
        if (can_more && count < iter_cap) {
            Cont step = { C_REP, NULL, 0, n, count + 1, NULL, 0, k };
            return rx_m(n->child, pos, c, &step);
        }
        return false;
    }
    if (can_more && count < iter_cap) {
        Cont step = { C_REP, NULL, 0, n, count + 1, NULL, 0, k };
        if (rx_m(n->child, pos, c, &step)) return true;
    }
    return can_stop ? rx_k(k, pos, c) : false;
}

/* ---- continuation driver ---- */

static bool rx_k(Cont *k, size_t pos, RxCtx *c) {
    switch (k->tag) {
    case C_DONE:
        c->end = pos;
        return true;
    case C_SEQ:
        return rx_m_seq(k->seq, k->idx, pos, c, k->next);
    case C_REP:
        return rx_m_rep_at(k->rep, k->count, pos, c, k->next);
    case C_GRP: {
        size_t old_s = k->grp->gidx ? c->cap_s[k->grp->gidx] : 0;
        size_t old_e = k->grp->gidx ? c->cap_e[k->grp->gidx] : 0;
        if (k->grp->gidx) {
            c->cap_s[k->grp->gidx] = k->start;
            c->cap_e[k->grp->gidx] = pos;
        }
        if (rx_k(k->next, pos, c)) return true;
        if (k->grp->gidx) {
            c->cap_s[k->grp->gidx] = old_s;
            c->cap_e[k->grp->gidx] = old_e;
        }
        return false;
    }
    }
    return false;
}

static bool rx_m_seq(Rx *seq, size_t idx, size_t pos, RxCtx *c, Cont *k) {
    if (idx >= seq->kids.len) return rx_k(k, pos, c);
    Cont step = { C_SEQ, seq, idx + 1, NULL, 0, NULL, 0, k };
    return rx_m((Rx *)seq->kids.items[idx], pos, c, &step);
}

/* run tree at offset; fills cap arrays on success */
static bool rx_run(Rx *tree, int ngroups, const char *s, size_t len, size_t off,
                   size_t *ms, size_t *me, RxCtx *c) {
    memset(c, 0, sizeof *c);
    c->s = s; c->len = len; c->ngroups = ngroups;
    for (size_t i = 0; i < 32; i++) { c->cap_s[i] = (size_t)-1; c->cap_e[i] = (size_t)-1; }
    Cont done = { C_DONE, NULL, 0, NULL, 0, NULL, 0, NULL };
    if (!rx_m(tree, off, c, &done)) return false;
    *ms = off; *me = c->end;
    return true;
}

/* search leftmost match at or after off */
static bool rx_search(Rx *tree, int ngroups, const char *s, size_t len, size_t off,
                      size_t *ms, size_t *me, RxCtx *c) {
    for (size_t i = off; i <= len; i++)
        if (rx_run(tree, ngroups, s, len, i, ms, me, c)) return true;
    return false;
}

static harr *rx_match_arr(RxCtx *c, const char *s, size_t ms, size_t me) {
    harr *m = hp_arr_new();
    m->is_map = true;
    hp_arr_set(m, hp_of_int(0), hp_of_str(hp_str_new(s + ms, me - ms)));
    for (int gi = 1; gi <= c->ngroups; gi++) {
        if (c->cap_s[gi] != (size_t)-1)
            hp_arr_set(m, hp_of_int(gi), hp_of_str(hp_str_new(s + c->cap_s[gi], c->cap_e[gi] - c->cap_s[gi])));
        else
            hp_arr_set(m, hp_of_int(gi), hp_of_str(hp_str_lit("")));
    }
    return m;
}

static hval rx_first_match(hval pat, hval subj, hval *out) {
    hstr *p = hp_val_to_str(pat);
    hstr *x = hp_val_to_str(subj);
    Rx *tree; int ng;
    if (!rx_split_pattern(p, &tree, &ng)) RETB(false);
    size_t ms, me; RxCtx c;
    bool found = rx_search(tree, ng, x->data, x->len, 0, &ms, &me, &c);
    if (!found) { rx_free(tree); RETB(false); }
    if (out) *out = hp_of_arr(rx_match_arr(&c, x->data, ms, me));
    rx_free(tree);
    RETI(1);
}
hval hpbi_preg_match(hval pat, hval subj) { return rx_first_match(pat, subj, NULL); }
hval hpbi_preg_match3(hval pat, hval subj, hval *out) { return rx_first_match(pat, subj, out); }

static hval rx_all_matches(hval pat, hval subj, hval *out) {
    hstr *p = hp_val_to_str(pat);
    hstr *x = hp_val_to_str(subj);
    Rx *tree; int ng;
    if (!rx_split_pattern(p, &tree, &ng)) RETB(false);
    /* PHP default (PREG_PATTERN_ORDER): $m[0] = all full matches,
     * $m[1..ng] = all matches of capture group k */
    harr *cols = hp_arr_new();
    for (int g = 0; g <= ng; g++) hp_arr_push(cols, hp_of_arr(hp_arr_new()));
    size_t off = 0;
    while (off <= x->len) {
        size_t ms, me; RxCtx c;
        if (!rx_search(tree, ng, x->data, x->len, off, &ms, &me, &c)) break;
        harr *row = rx_match_arr(&c, x->data, ms, me);
        for (int g = 0; g <= ng && (size_t)g < row->len; g++) {
            hval cell = row->vals[g];
            hval colv = cols->vals[g];
            hp_arr_push(colv.u.a, cell);
        }
        hp_arr_free(row);
        off = me > ms ? me : ms + 1;
    }
    hval r = hp_of_arr(cols);
    rx_free(tree);
    if (out) *out = r;
    return hp_of_int((int64_t)(cols->vals[0].u.a->len));
}
hval hpbi_preg_match_all(hval pat, hval subj) { return rx_all_matches(pat, subj, NULL); }
hval hpbi_preg_match_all3(hval pat, hval subj, hval *out) { return rx_all_matches(pat, subj, out); }

hval hpbi_preg_replace(hval pat, hval rep, hval subj) {
    hstr *p = hp_val_to_str(pat);
    hstr *r = hp_val_to_str(rep);
    hstr *x = hp_val_to_str(subj);
    Rx *tree; int ng;
    if (!rx_split_pattern(p, &tree, &ng)) RETS(hp_str_copy(x));
    Buf out;
    buf_init(&out);
    size_t off = 0;
    while (off <= x->len) {
        size_t ms, me; RxCtx c;
        if (!rx_search(tree, ng, x->data, x->len, off, &ms, &me, &c)) break;
        buf_write(&out, x->data + off, ms - off);
        /* $1..$9 and \1..\9 backrefs */
        for (size_t i = 0; i < r->len; i++) {
            if ((r->data[i] == '$' || r->data[i] == 92) && i + 1 < r->len &&
                isdigit((unsigned char)r->data[i + 1])) {
                int gi = r->data[i + 1] - '0';
                i++;
                if (gi == 0) buf_write(&out, x->data + ms, me - ms);
                else if (gi <= ng && c.cap_s[gi] != (size_t)-1)
                    buf_write(&out, x->data + c.cap_s[gi], c.cap_e[gi] - c.cap_s[gi]);
            } else {
                buf_putc(&out, r->data[i]);
            }
        }
        off = me > ms ? me : ms + 1;
    }
    buf_write(&out, x->data + off, x->len - off);
    rx_free(tree);
    RETS(hp_str_take_buf(&out));
}

hval hpbi_preg_split(hval pat, hval subj) {
    hstr *p = hp_val_to_str(pat);
    hstr *x = hp_val_to_str(subj);
    Rx *tree; int ng;
    if (!rx_split_pattern(p, &tree, &ng)) {
        harr *one = hp_arr_new();
        hp_arr_push_take(one, hp_of_str(hp_str_copy(x)));
        return hp_of_arr(one);
    }
    harr *parts = hp_arr_new();
    size_t off = 0;
    while (off <= x->len) {
        size_t ms, me; RxCtx c;
        if (!rx_search(tree, ng, x->data, x->len, off, &ms, &me, &c)) break;
        if (me == ms) {   /* zero-width match: skip one char */
            off = ms + 1;
            continue;
        }
        hp_arr_push_take(parts, hp_of_str(hp_str_new(x->data + off, ms - off)));
        off = me;
    }
    hp_arr_push_take(parts, hp_of_str(hp_str_new(x->data + off, x->len - off)));
    rx_free(tree);
    return hp_of_arr(parts);
}

hval hpbi_preg_grep(hval pat, hval arr) {
    hstr *p = hp_val_to_str(pat);
    Rx *tree; int ng;
    if (!rx_split_pattern(p, &tree, &ng)) RETB(false);
    harr *a = arr.tag == HV_ARR ? arr.u.a : hp_arr_new();
    harr *out = hp_arr_new();
    for (size_t i = 0; i < a->len; i++) {
        hstr *v = hp_val_to_str(a->vals[i]);
        size_t ms, me; RxCtx c;
        if (rx_search(tree, ng, v->data, v->len, 0, &ms, &me, &c)) {
            if (a->is_map) hp_arr_set(out, a->keys[i], a->vals[i]);
            else hp_arr_push(out, a->vals[i]);
        }
        if (a->vals[i].tag != HV_STR) hp_str_free(v);
    }
    rx_free(tree);
    return hp_of_arr(out);
}

/* ===================== crypto: SHA-256 / HMAC / PBKDF2 ===================== */

static const uint32_t K256[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

static uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static void hp_sha256_digest(const unsigned char *msg, size_t len, unsigned char out[32]);

static void hp_sha256_compress_soft(uint32_t h[8], const unsigned char block[64]);

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
/* One block with the x86 SHA extensions (SHA-NI); same result as the scalar
 * code, chosen at run time when the CPU reports the extension. */
__attribute__((target("sha,sse4.1,ssse3")))
static void hp_sha256_compress_ni(uint32_t h[8], const unsigned char block[64]) {
    const __m128i mask = _mm_set_epi64x(0x0c0d0e0f08090a0bULL, 0x0405060700010203ULL);
    __m128i tmp = _mm_loadu_si128((const __m128i *)(const void *)&h[0]);
    __m128i st1 = _mm_loadu_si128((const __m128i *)(const void *)&h[4]);
    tmp = _mm_shuffle_epi32(tmp, 0xB1);              /* CDAB */
    st1 = _mm_shuffle_epi32(st1, 0x1B);              /* EFGH */
    __m128i st0 = _mm_alignr_epi8(tmp, st1, 8);      /* ABEF */
    st1 = _mm_blend_epi16(st1, tmp, 0xF0);           /* CDGH */
    const __m128i abef = st0, cdgh = st1;
    __m128i m[4];
    for (int i = 0; i < 16; i++) {
        __m128i w;
        if (i < 4) {
            w = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(const void *)(block + 16 * i)), mask);
        } else {
            __m128i a = _mm_sha256msg1_epu32(m[i & 3], m[(i + 1) & 3]);
            a = _mm_add_epi32(a, _mm_alignr_epi8(m[(i + 3) & 3], m[(i + 2) & 3], 4));
            w = _mm_sha256msg2_epu32(a, m[(i + 3) & 3]);
        }
        m[i & 3] = w;
        __m128i k = _mm_add_epi32(w, _mm_loadu_si128((const __m128i *)(const void *)&K256[4 * i]));
        st1 = _mm_sha256rnds2_epu32(st1, st0, k);
        k = _mm_shuffle_epi32(k, 0x0E);
        st0 = _mm_sha256rnds2_epu32(st0, st1, k);
    }
    st0 = _mm_add_epi32(st0, abef);
    st1 = _mm_add_epi32(st1, cdgh);
    tmp = _mm_shuffle_epi32(st0, 0x1B);              /* FEBA */
    st1 = _mm_shuffle_epi32(st1, 0xB1);              /* DCHG */
    st0 = _mm_blend_epi16(tmp, st1, 0xF0);           /* DCBA */
    st1 = _mm_alignr_epi8(st1, tmp, 8);              /* HGFE */
    _mm_storeu_si128((__m128i *)(void *)&h[0], st0);
    _mm_storeu_si128((__m128i *)(void *)&h[4], st1);
}
#endif

static void hp_sha256_compress(uint32_t h[8], const unsigned char block[64]) {
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
    static int have_ni = -1;
    if (have_ni < 0) {
        __builtin_cpu_init();
        have_ni = __builtin_cpu_supports("sha") ? 1 : 0;
        const char *e = getenv("HPHP_SHA_NI");
        if (e && e[0] == '0') have_ni = 0;
    }
    if (have_ni) { hp_sha256_compress_ni(h, block); return; }
#endif
    hp_sha256_compress_soft(h, block);
}

static void hp_sha256_compress_soft(uint32_t h[8], const unsigned char block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)block[i*4] << 24) | ((uint32_t)block[i*4+1] << 16) |
               ((uint32_t)block[i*4+2] << 8) | (uint32_t)block[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i-15], 7) ^ rotr32(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = rotr32(w[i-2], 17) ^ rotr32(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a = h[0], b = h[1], cc = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = hh + S1 + ch + K256[i] + w[i];
        uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + maj;
        hh = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += cc; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

static void hp_sha256_init(uint32_t h[8]) {
    static const uint32_t init[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };
    memcpy(h, init, sizeof init);
}

/* Finish SHA-256 from a state that has consumed exactly one 64-byte block. */
static void hp_sha256_after_block(const uint32_t initial[8], const unsigned char *msg,
                                  size_t len, unsigned char out[32]) {
    uint32_t h[8];
    memcpy(h, initial, sizeof h);
    size_t total_len = len;
    while (len >= 64) {
        hp_sha256_compress(h, msg);
        msg += 64;
        len -= 64;
    }
    unsigned char tail[128] = {0};
    memcpy(tail, msg, len);
    tail[len] = 0x80;
    size_t padded = len < 56 ? 64 : 128;
    uint64_t bits = (uint64_t)(64 + total_len) * 8;
    for (int i = 0; i < 8; i++) tail[padded - 8 + i] = (unsigned char)(bits >> (56 - 8 * i));
    hp_sha256_compress(h, tail);
    if (padded == 128) hp_sha256_compress(h, tail + 64);
    for (int i = 0; i < 8; i++) {
        out[i*4] = (unsigned char)(h[i] >> 24);
        out[i*4+1] = (unsigned char)(h[i] >> 16);
        out[i*4+2] = (unsigned char)(h[i] >> 8);
        out[i*4+3] = (unsigned char)h[i];
    }
}

typedef struct { uint32_t inner[8], outer[8]; } HpHmac256Ctx;

static void hp_hmac256_init(HpHmac256Ctx *ctx, const unsigned char *key, size_t klen) {
    unsigned char k[64] = {0}, ipad[64], opad[64];
    if (klen > 64) hp_sha256_digest(key, klen, k);
    else if (klen) memcpy(k, key, klen);
    for (int i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
    hp_sha256_init(ctx->inner);
    hp_sha256_init(ctx->outer);
    hp_sha256_compress(ctx->inner, ipad);
    hp_sha256_compress(ctx->outer, opad);
}

static void hp_hmac256_run(const HpHmac256Ctx *ctx, const unsigned char *msg,
                           size_t len, unsigned char out[32]) {
    unsigned char inner[32];
    hp_sha256_after_block(ctx->inner, msg, len, inner);
    hp_sha256_after_block(ctx->outer, inner, sizeof inner, out);
}

static void hp_sha256_digest(const unsigned char *msg, size_t len, unsigned char out[32]) {
    uint32_t h[8];
    hp_sha256_init(h);
    size_t total = len;
    while (len >= 64) { hp_sha256_compress(h, msg); msg += 64; len -= 64; }
    unsigned char tail[128] = {0};
    memcpy(tail, msg, len);
    tail[len] = 0x80;
    size_t padded = len < 56 ? 64 : 128;
    uint64_t bits = (uint64_t)total * 8;
    for (int i = 0; i < 8; i++) tail[padded - 8 + i] = (unsigned char)(bits >> (56 - 8 * i));
    hp_sha256_compress(h, tail);
    if (padded == 128) hp_sha256_compress(h, tail + 64);
    for (int i = 0; i < 8; i++) {
        out[i*4]   = (unsigned char)(h[i] >> 24);
        out[i*4+1] = (unsigned char)(h[i] >> 16);
        out[i*4+2] = (unsigned char)(h[i] >> 8);
        out[i*4+3] = (unsigned char)h[i];
    }
}

static hstr *hp_sha256_hex(hstr *s) {
    unsigned char d[32];
    hp_sha256_digest((const unsigned char *)s->data, s->len, d);
    char hex[65];
    for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", d[i]);
    return hp_str_new(hex, 64);
}

hval hpbi_sha256(hval s) { RETS(hp_sha256_hex(hp_val_to_str(s))); }

static void hp_hmac_sha256(const unsigned char *key, size_t klen,
                           const unsigned char *msg, size_t mlen,
                           unsigned char out[32]) {
    HpHmac256Ctx ctx;
    hp_hmac256_init(&ctx, key, klen);
    hp_hmac256_run(&ctx, msg, mlen, out);
}

hval hpbi_hash_hmac(hval algo, hval data, hval key, hval raw_output) {
    hstr *al = hp_val_to_str(algo);
    if (strcmp(al->data, "sha256") != 0) RETB(false);
    hstr *d = hp_val_to_str(data);
    hstr *k = hp_val_to_str(key);
    unsigned char mac[32];
    hp_hmac_sha256((const unsigned char *)k->data, k->len,
                   (const unsigned char *)d->data, d->len, mac);
    bool raw = raw_output.tag == HV_INT ? hp_val_to_int(raw_output) != 0 :
               raw_output.tag == HV_BOOL ? raw_output.u.b : false;
    if (raw) RETS(hp_str_new(mac, 32));
    char hex[65];
    for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", mac[i]);
    RETS(hp_str_cstr(hex));
}

/* PBKDF2-HMAC-SHA256, PHP password_hash's bcrypt-format output with
 * prefix changed to our own "$hphp$" scheme (bcrypt itself is out of scope). */
static void hp_pbkdf2_sha256(const unsigned char *pw, size_t pwlen,
                             const unsigned char *salt, size_t slen,
                             int rounds, unsigned char out[32]) {
    uint32_t block = 1;
    unsigned char saltblk[64 + 4];
    size_t n = slen > 64 ? 64 : slen;
    memcpy(saltblk, salt, n);
    saltblk[n]     = (unsigned char)(block >> 24);
    saltblk[n + 1] = (unsigned char)(block >> 16);
    saltblk[n + 2] = (unsigned char)(block >> 8);
    saltblk[n + 3] = (unsigned char)block;
    unsigned char u[32];
    hp_hmac_sha256(pw, pwlen, saltblk, n + 4, u);
    unsigned char acc[32];
    memcpy(acc, u, 32);
    for (int i = 1; i < rounds; i++) {
        hp_hmac_sha256(pw, pwlen, u, 32, u);
        for (int j = 0; j < 32; j++) acc[j] ^= u[j];
    }
    memcpy(out, acc, 32);
}

static void hp_b64_encode_raw(const unsigned char *in, size_t n, char *out) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned)in[i] << 16;
        if (i + 1 < n) v |= (unsigned)in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = i + 1 < n ? tbl[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? tbl[v & 63] : '=';
    }
    out[o] = 0;
}

hval hpbi_password_hash(hval pw) {
    hstr *p = hp_val_to_str(pw);
    unsigned char salt[16];
    static bool srnd = false;
    if (!srnd) { srand((unsigned)time(NULL) ^ (unsigned)(uintptr_t)&salt); srnd = true; }
    for (int i = 0; i < 16; i++) salt[i] = (unsigned char)(rand() & 0xff);
    int rounds = 4096;
    unsigned char dk[32];
    hp_pbkdf2_sha256((const unsigned char *)p->data, p->len, salt, 16, rounds, dk);
    /* 16 bytes -> 24 base64 chars, 32 bytes -> 44, plus the NUL that
     * hp_b64_encode_raw() writes (F3: b64salt used to be one byte too small) */
    char b64salt[25], b64dk[45];
    hp_b64_encode_raw(salt, 16, b64salt);
    hp_b64_encode_raw(dk, 32, b64dk);
    char out[128];
    snprintf(out, sizeof out, "$hphp$%d$%s$%s", rounds, b64salt, b64dk);
    RETS(hp_str_cstr(out));
}

static bool hp_ct_eq(const unsigned char *a, const unsigned char *b, size_t n) {
    unsigned char d = 0;
    for (size_t i = 0; i < n; i++) d |= (unsigned char)(a[i] ^ b[i]);
    return d == 0;
}


hval hpbi_password_verify(hval pw, hval hash) {
    hstr *p = hp_val_to_str(pw);
    hstr *h = hp_val_to_str(hash);
    int rounds = 0;
    char b64salt[64] = {0}, b64dk[64] = {0};
    if (sscanf(h->data, "$hphp$%d$%63[^$]$%63s", &rounds, b64salt, b64dk) != 3)
        RETB(false);
    /* decode the base64 salt (16 bytes -> 24 chars incl '=' padding) */
    unsigned char salt[16];
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int rev[256];
    for (int i = 0; i < 256; i++) rev[i] = -1;
    for (int i = 0; i < 64; i++) rev[(unsigned char)tbl[i]] = i;
    size_t sl = strlen(b64salt);
    memset(salt, 0, sizeof salt);
    for (size_t i = 0, o = 0; i + 3 < sl && o < 16; i += 4, o += 3) {
        int a = rev[(unsigned char)b64salt[i]], b = rev[(unsigned char)b64salt[i+1]];
        int c = rev[(unsigned char)b64salt[i+2]], d = rev[(unsigned char)b64salt[i+3]];
        if (a < 0 || b < 0) break;
        unsigned v = ((unsigned)a << 18) | ((unsigned)b << 12) |
                     ((unsigned)(c < 0 ? 0 : c) << 6) | (unsigned)(d < 0 ? 0 : d);
        if (o < 16) salt[o] = (unsigned char)(v >> 16);
        if (o + 1 < 16) salt[o + 1] = (unsigned char)(v >> 8);
        if (o + 2 < 16) salt[o + 2] = (unsigned char)v;
    }
    unsigned char dk[32];
    hp_pbkdf2_sha256((const unsigned char *)p->data, p->len, salt, 16, rounds, dk);
    char b64dk2[45];
    hp_b64_encode_raw(dk, 32, b64dk2);
    /* compare the computed digest against the parsed digest segment */
    size_t dl = strlen(b64dk);
    if (strlen(b64dk2) != dl) RETB(false);
    RETB(hp_ct_eq((const unsigned char *)b64dk2, (const unsigned char *)b64dk, dl));
}

hval hpbi_random_bytes(hval n) {
    int64_t cnt = hp_val_to_int(n);
    if (cnt < 0) cnt = 0;
    if (cnt > 1024) cnt = 1024;
    unsigned char buf[1024];
#if defined(_WIN32)
    static bool brnd = false;
    if (!brnd) { srand((unsigned)time(NULL) ^ (unsigned)(uintptr_t)buf); brnd = true; }
    for (int64_t i = 0; i < cnt; i++) buf[i] = (unsigned char)(rand() & 0xff);
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        size_t got = fread(buf, 1, (size_t)cnt, f);
        fclose(f);
        if (got == (size_t)cnt)
            RETS(hp_str_new(buf, (size_t)cnt));
    }
    for (int64_t i = 0; i < cnt; i++) buf[i] = (unsigned char)(rand() & 0xff);
#endif
    RETS(hp_str_new(buf, (size_t)cnt));
}

hval hpbi_hash_equals(hval known, hval user) {
    hstr *a = hp_val_to_str(known);
    hstr *b = hp_val_to_str(user);
    if (a->len != b->len) RETB(false);
    RETB(hp_ct_eq((const unsigned char *)a->data, (const unsigned char *)b->data, a->len));
}

/* ===================== time: mktime, strtotime ===================== */

hval hpbi_mktime(hval hour, hval min, hval sec, hval mon, hval day, hval year) {
    struct tm t;
    memset(&t, 0, sizeof t);
    t.tm_hour = (int)hp_val_to_int(hour);
    t.tm_min  = (int)hp_val_to_int(min);
    t.tm_sec  = (int)hp_val_to_int(sec);
    t.tm_mon  = (int)hp_val_to_int(mon) - 1;
    t.tm_mday = (int)hp_val_to_int(day);
    t.tm_year = (int)hp_val_to_int(year) - 1900;
    t.tm_isdst = -1;
    time_t r = mktime(&t);
    RETI((int64_t)r);
}

/* pragmatic strtotime: "now", ISO "YYYY-MM-DD[ HH:MM[:SS]]" (also with '/'),
 * and "+/-N <unit>" deltas ("+1 day", "-2 hours"); 0 on failure like PHP */
hval hpbi_strtotime(hval s) {
    hstr *x = hp_val_to_str(s);
    const char *p = x->data;
    while (*p == ' ') p++;
    if (strcmp(p, "now") == 0) RETI((int64_t)time(NULL));
    if ((*p == '+' || *p == '-') && isdigit((unsigned char)p[1])) {
        int sign = *p == '-' ? -1 : 1;
        p++;
        long n = strtol(p, (char **)&p, 10);
        while (*p == ' ') p++;
        char unit[16] = {0};
        sscanf(p, "%15s", unit);
        time_t now = time(NULL);
        struct tm tmbuf;
#if defined(_WIN32)
        localtime_s(&tmbuf, &now);
#else
        localtime_r(&now, &tmbuf);
#endif
        struct tm *tmv = &tmbuf;
        if (strncmp(unit, "day", 3) == 0) tmv->tm_mday += (int)(sign * n);
        else if (strncmp(unit, "week", 4) == 0) tmv->tm_mday += (int)(sign * n * 7);
        else if (strncmp(unit, "hour", 4) == 0) tmv->tm_hour += (int)(sign * n);
        else if (strncmp(unit, "minute", 6) == 0 || strncmp(unit, "min", 3) == 0)
            tmv->tm_min += (int)(sign * n);
        else if (strncmp(unit, "second", 6) == 0 || strncmp(unit, "sec", 3) == 0)
            tmv->tm_sec += (int)(sign * n);
        else if (strncmp(unit, "month", 5) == 0) tmv->tm_mon += (int)(sign * n);
        else if (strncmp(unit, "year", 4) == 0) tmv->tm_year += (int)(sign * n);
        else RETI(0);
        tmv->tm_isdst = -1;
        time_t r = mktime(tmv);
        RETI((int64_t)r);
    }
    int y = 0, mo = 0, d = 0, hh = 0, mi = 0, ss = 0;
    int n = sscanf(p, "%4d-%2d-%2d %2d:%2d:%2d", &y, &mo, &d, &hh, &mi, &ss);
    if (n < 3) {
        n = sscanf(p, "%4d/%2d/%2d %2d:%2d:%2d", &y, &mo, &d, &hh, &mi, &ss);
        if (n < 3) RETI(0);
    }
    if (n == 3) { hh = mi = ss = 0; }
    else if (n == 5) ss = 0;
    struct tm t;
    memset(&t, 0, sizeof t);
    t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d;
    t.tm_hour = hh; t.tm_min = mi; t.tm_sec = ss;
    t.tm_isdst = -1;
    time_t r = mktime(&t);
    RETI(r == (time_t)-1 ? 0 : (int64_t)r);
}

/* ===================== http: post/put/patch, url helpers ===================== */

hval hpbi_http_post(hval url, hval body, hval headers) {
    hstr *u = hp_val_to_str(url);
    char host[256];
    int port;
    const char *path;
    if (!http_parse_url(u->data, host, sizeof host, &port, &path)) RETB(false);
    hstr *b = body.tag == HV_STR ? body.u.s : hp_val_to_str(body);
    harr *hs = headers.tag == HV_ARR ? headers.u.a : NULL;
    return http_perform(host, port, path, "POST", b, hs);
}

/* parse_url("http://user@host:8080/path?query#frag") -> map with scheme, host,
 * port, path, query, fragment (missing parts are empty strings / 0) */
hval hpbi_parse_url(hval url) {
    hstr *u = hp_val_to_str(url);
    const char *s = u->data;
    size_t n = u->len;
    harr *m = hp_arr_new();
    m->is_map = true;
    char scheme[16] = "", host[256] = "", path[512] = "", query[512] = "", frag[256] = "";
    int port = 0;
    const char *rest = s;
    size_t rn = n;
    const char *c3 = strstr(s, "://");
    if (c3 && (size_t)(c3 - s) < 12) {
        size_t sl = (size_t)(c3 - s);
        if (sl >= sizeof scheme) sl = sizeof scheme - 1;
        memcpy(scheme, s, sl);
        rest = c3 + 3;
        rn = n - (size_t)(rest - s);
    }
    /* fragment */
    const char *hash = memchr(rest, '#', rn);
    if (hash) {
        size_t fl = rn - (size_t)(hash - rest) - 1;
        if (fl >= sizeof frag) fl = sizeof frag - 1;
        memcpy(frag, hash + 1, fl);
        rn = (size_t)(hash - rest);
    }
    /* query */
    const char *qm = memchr(rest, '?', rn);
    if (qm) {
        size_t ql = rn - (size_t)(qm - rest) - 1;
        if (ql >= sizeof query) ql = sizeof query - 1;
        memcpy(query, qm + 1, ql);
        rn = (size_t)(qm - rest);
    }
    /* host[:port][/path] */
    const char *slash = memchr(rest, '/', rn);
    size_t hl = slash ? (size_t)(slash - rest) : rn;
    if (hl >= sizeof host) hl = sizeof host - 1;
    memcpy(host, rest, hl);
    host[hl] = 0;
    /* strip userinfo (user[:pass]@host), keep user in the map */
    char *at = strrchr(host, '@');
    if (at) {
        *at = 0;
        char *c0 = strchr(host, ':');
        if (c0) *c0 = 0;
        hp_arr_set(m, hp_of_str(hp_str_lit("user")), hp_of_str(hp_str_lit(host)));
        memmove(host, at + 1, strlen(at + 1) + 1);
    }
    char *colon = strchr(host, ':');
    if (colon) { *colon = 0; port = atoi(colon + 1); }
    if (slash) {
        size_t pl = rn - (size_t)(slash - rest);   /* keeps the leading '/' */
        if (pl >= sizeof path) pl = sizeof path - 1;
        memcpy(path, slash, pl);
    }
    hp_arr_set(m, hp_of_str(hp_str_lit("scheme")), hp_of_str(hp_str_lit(scheme)));
    hp_arr_set(m, hp_of_str(hp_str_lit("host")), hp_of_str(hp_str_lit(host)));
    hp_arr_set(m, hp_of_str(hp_str_lit("port")), hp_of_int(port ? port :
               (strcmp(scheme, "https") == 0 ? 443 : 80)));
    hp_arr_set(m, hp_of_str(hp_str_lit("path")), hp_of_str(hp_str_lit(path)));
    hp_arr_set(m, hp_of_str(hp_str_lit("query")), hp_of_str(hp_str_lit(query)));
    hp_arr_set(m, hp_of_str(hp_str_lit("fragment")), hp_of_str(hp_str_lit(frag)));
    return hp_of_arr(m);
}

/* http_build_query(map) -> "a=1&b=x%20y" */
hval hpbi_http_build_query(hval d) {
    if (d.tag != HV_ARR) RETS(hp_str_lit(""));
    harr *a = d.u.a;
    Buf b;
    buf_init(&b);
    for (size_t i = 0; i < a->len; i++) {
        if (i) buf_putc(&b, '&');
        hstr *k = hp_val_to_str(a->keys[i]);
        hstr *v = hp_val_to_str(a->vals[i]);
        hstr *ke = hp_val_to_str(hpbi_urlencode(hp_of_str(k)));
        hstr *ve = hp_val_to_str(hpbi_urlencode(hp_of_str(v)));
        if (a->keys[i].tag != HV_STR) hp_str_free(k);
        if (a->vals[i].tag != HV_STR) hp_str_free(v);
        buf_write(&b, ke->data, ke->len);
        buf_putc(&b, '=');
        buf_write(&b, ve->data, ve->len);
        hp_str_free(ke);
        hp_str_free(ve);
    }
    RETS(hp_str_take_buf(&b));
}
/* ===================== mysql: wire-protocol client (mysql_native_password) =====================
 * mysql_connect(host, port, user, pass, db) -> handle | false
 * mysql_query(handle, sql) -> array of row-maps | false on error
 * mysql_exec(handle, sql) -> affected rows | false
 * mysql_insert_id(handle) -> id
 * mysql_escape(s), mysql_close(handle)
 */

static void hp_sha1_pub(const unsigned char *msg, size_t len, unsigned char out[20]);

static void mysql_native_scramble(const unsigned char *pw, size_t pwlen,
                                  const unsigned char *salt /*20*/,
                                  unsigned char out[20]) {
    unsigned char h1[20], h2[20], h3[20];
    hp_sha1_pub(pw, pwlen, h1);
    hp_sha1_pub(h1, 20, h2);
    unsigned char tmp[40];
    memcpy(tmp, salt, 20);
    memcpy(tmp + 20, h2, 20);
    hp_sha1_pub(tmp, 40, h3);
    for (int i = 0; i < 20; i++) out[i] = (unsigned char)(h1[i] ^ h3[i]);
}

typedef struct {
    hp_sock fd;
    uint32_t seq;
    uint32_t insert_id;
    bool ok;
} MyConn;

static bool mysql_read_packet(MyConn *c, Buf *out) {
    unsigned char hdr[4];
    size_t got = 0;
    while (got < 4) {
        int n = (int)recv(c->fd, (char *)hdr + got, 4 - (int)got, 0);
        if (n <= 0) return false;
        got += (size_t)n;
    }
    uint32_t len = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) | ((uint32_t)hdr[2] << 16);
    c->seq = hdr[3] + 1;
    size_t total = 0;
    while (total < len) {
        unsigned char chunk[4096];
        uint32_t want = len - (uint32_t)total > sizeof chunk ? sizeof chunk : len - total;
        int n = (int)recv(c->fd, (char *)chunk, (int)want, 0);
        if (n <= 0) return false;
        buf_write(out, chunk, (size_t)n);
        total += (size_t)n;
    }
    return true;
}

static bool mysql_send_packet(MyConn *c, const unsigned char *data, uint32_t len) {
    unsigned char hdr[4];
    hdr[0] = (unsigned char)(len & 0xff);
    hdr[1] = (unsigned char)((len >> 8) & 0xff);
    hdr[2] = (unsigned char)((len >> 16) & 0xff);
    hdr[3] = (unsigned char)c->seq++;
    size_t sent = 0;
    while (sent < 4) {
        int n = (int)send(c->fd, (const char *)hdr + sent, 4 - (int)sent, 0);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    sent = 0;
    while (sent < len) {
        int n = (int)send(c->fd, (const char *)data + sent, (int)(len - sent), 0);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

static uint64_t my_lenc_int(const unsigned char **p) {
    unsigned char b = **p;
    (*p)++;
    if (b < 0xfb) return b;
    if (b == 0xfc) { uint64_t v = (*p)[0] | ((uint64_t)(*p)[1] << 8); *p += 2; return v; }
    if (b == 0xfd) {
        uint64_t v = (*p)[0] | ((uint64_t)(*p)[1] << 8) | ((uint64_t)(*p)[2] << 16);
        *p += 3; return v;
    }
    if (b == 0xfe) {
        uint64_t v = 0;
        for (int i = 0; i < 8; i++) v |= ((uint64_t)(*p)[i]) << (8 * i);
        *p += 8; return v;
    }
    return 0;   /* NULL / ERR */
}

static void my_lenc_put(Buf *b, uint64_t v) {
    if (v < 0xfb) buf_putc(b, (char)v);
    else if (v <= 0xffff) {
        buf_putc(b, (char)0xfc);
        buf_putc(b, (char)(v & 0xff));
        buf_putc(b, (char)((v >> 8) & 0xff));
    } else {
        buf_putc(b, (char)0xfe);
        for (int i = 0; i < 8; i++) buf_putc(b, (char)((v >> (8 * i)) & 0xff));
    }
}

static void my_lenc_str(Buf *b, const char *s, size_t n) {
    my_lenc_put(b, (uint64_t)n);
    buf_write(b, s, n);
}

hval mysql_connect_impl(hval hostv, hval portv, hval userv, hval passv, hval dbv) {
    hstr *host = hp_val_to_str(hostv);
    hstr *user = hp_val_to_str(userv);
    hstr *pass = hp_val_to_str(passv);
    hstr *db = hp_val_to_str(dbv);
    int port = (int)hp_val_to_int(portv);
    if (port <= 0) port = 3306;
    http_ws_init();
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char portstr[16];
    snprintf(portstr, sizeof portstr, "%d", port);
    if (getaddrinfo(host->data, portstr, &hints, &res) != 0 || !res) RETB(false);
    hp_sock fd = HP_SOCK_BAD;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        hp_sock s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == HP_SOCK_BAD) continue;
        if (connect(s, ai->ai_addr, (int)ai->ai_addrlen) == 0) { fd = s; break; }
        hp_sock_close(s);
    }
    freeaddrinfo(res);
    if (fd == HP_SOCK_BAD) RETB(false);
    MyConn *c = (MyConn *)calloc(1, sizeof(MyConn));
    c->fd = fd;
    c->seq = 0;
    Buf pkt;
    buf_init(&pkt);
    if (!mysql_read_packet(c, &pkt)) RETB(false);
    /* greeting: [protover][ver\0][thread4][salt8+1][caplo2][charset][status2][caphi2][alen][10x0][salt12+0] */
    const unsigned char *p = (const unsigned char *)pkt.data;
    if (!pkt.data || pkt.len < 30 || p[0] == 0xff) RETB(false);
    p++;                                    /* protocol version 10 */
    while (p < (const unsigned char *)pkt.data + pkt.len && *p) p++; p++;   /* server version */
    p += 4;                                 /* thread id */
    unsigned char salt[20] = {0};
    if (p + 8 <= (const unsigned char *)pkt.data + pkt.len) {
        memcpy(salt, p, 8);                 /* salt part 1 */
    }
    p += 8 + 1;                             /* + filler */
    uint16_t caplo = (uint16_t)(p[0] | (p[1] << 8));
    p += 2 + 1 + 2;                         /* charset, status */
    uint16_t caphi = (uint16_t)(p[0] | (p[1] << 8));
    p += 2;
    p += 1 + 10;                            /* auth-plugin-len, reserved */
    uint32_t caps = (uint32_t)caplo | ((uint32_t)caphi << 16);
    if (caps & 0x8000) {                    /* CLIENT_SECURE_CONN */
        size_t remain = pkt.len - (size_t)(p - (const unsigned char *)pkt.data);
        if (remain >= 13) memcpy(salt + 8, p, 12);   /* salt part 2 (12 + \0) */
    }
    /* handshake response 41 */
    Buf resp;
    buf_init(&resp);
    uint32_t client_caps = 0x00000001      /* CLIENT_LONG_PASSWORD */
                         | 0x00000200      /* CLIENT_PROTOCOL_41 */
                         | 0x00008000      /* CLIENT_SECURE_CONN */
                         | 0x00000008;     /* CLIENT_CONNECT_WITH_DB */
    buf_putc(&resp, (char)(client_caps & 0xff));
    buf_putc(&resp, (char)((client_caps >> 8) & 0xff));
    buf_putc(&resp, (char)0); buf_putc(&resp, (char)0);   /* max packet 16MB */
    buf_putc(&resp, (char)33);              /* charset utf8_general_ci */
    for (int i = 0; i < 23; i++) buf_putc(&resp, (char)0); /* reserved */
    buf_write(&resp, user->data, user->len);
    buf_putc(&resp, (char)0);
    unsigned char scram[20];
    mysql_native_scramble((const unsigned char *)pass->data, pass->len, salt, scram);
    buf_putc(&resp, (char)20);
    buf_write(&resp, scram, 20);
    if (db->len) {
        buf_write(&resp, db->data, db->len);
        buf_putc(&resp, (char)0);
    }
    c->seq = 1;
    bool sent_ok = mysql_send_packet(c, (const unsigned char *)resp.data, (uint32_t)resp.len);
    free(resp.data);
    free(pkt.data);
    if (!sent_ok) RETB(false);
    buf_init(&pkt);
    if (!mysql_read_packet(c, &pkt)) RETB(false);
    const unsigned char *r = (const unsigned char *)pkt.data;
    if (!pkt.data || pkt.len < 4 || r[0] == 0xff) RETB(false);
    if (r[0] == 0xfe && pkt.len < 9) {      /* auth switch: not supported */
        free(pkt.data);
        RETB(false);
    }
    /* OK packet: [0x00][affected lenc][insert_id lenc]... */
    const unsigned char *q = r + 1;
    (void)my_lenc_int(&q);
    c->insert_id = (uint32_t)my_lenc_int(&q);
    free(pkt.data);
    /* wrap the raw pointer in an opaque byte-string handle */
    hstr *hd = hp_str_new((const char *)&c, sizeof(void *));
    return hp_of_str(hd);
}

hval hpbi_mysql_connect(hval h, hval p, hval u, hval pw, hval d) {
    return mysql_connect_impl(h, p, u, pw, d);
}

static MyConn *mysql_handle(hval v) {
    /* connection handles travel as raw-pointer strings (HV_STR payload) */
    if (v.tag == HV_STR && v.u.s->len == sizeof(void *))
        return *(MyConn **)v.u.s->data;
    return NULL;
}
/* ---- mysql query / exec / helpers ---- */

/* COM_QUERY: send, then read result-set: column count, N column defs (EOF),
 * rows until EOF/OK. Returns array of row-maps (column name -> string value). */
hval mysql_query_impl(hval conn, hval sqlv) {
    MyConn *c = mysql_handle(conn);
    hstr *sql = hp_val_to_str(sqlv);
    if (!c || c->fd == HP_SOCK_BAD) RETB(false);
    Buf cmd;
    buf_init(&cmd);
    buf_putc(&cmd, (char)0x03);            /* COM_QUERY */
    buf_write(&cmd, sql->data, sql->len);
    c->seq = 0;
    if (!mysql_send_packet(c, (const unsigned char *)cmd.data, (uint32_t)cmd.len))
        RETB(false);
    free(cmd.data);
    Buf pkt;
    buf_init(&pkt);
    if (!mysql_read_packet(c, &pkt)) RETB(false);
    const unsigned char *r = (const unsigned char *)pkt.data;
    if (pkt.len < 1) { free(pkt.data); RETB(false); }
    if (r[0] == 0xff) { free(pkt.data); RETB(false); }         /* ERR */
    if (r[0] == 0x00 || r[0] == 0xfe) {                        /* OK (no result set) */
        const unsigned char *q = r + 1;
        uint64_t affected = my_lenc_int(&q);
        c->insert_id = (uint32_t)my_lenc_int(&q);
        free(pkt.data);
        RETI((int64_t)affected);
    }
    size_t ncols = (size_t)my_lenc_int(&r);
    free(pkt.data);
    /* read column definitions */
    for (size_t i = 0; i < ncols; i++) {
        buf_init(&pkt);
        if (!mysql_read_packet(c, &pkt)) { free(pkt.data); RETB(false); }
        free(pkt.data);
    }
    /* EOF after column defs (absent when CLIENT_DEPRECATE_EOF; then rows
     * simply follow — we detect row start by first byte 0x00.. or lenenc) */
    bool eof_after_cols = false;
    buf_init(&pkt);
    if (mysql_read_packet(c, &pkt)) {
        const unsigned char *e = (const unsigned char *)pkt.data;
        if (pkt.len >= 5 && e[0] == 0xfe && pkt.len < 0xffffff)
            eof_after_cols = true;      /* EOF packet consumed */
        /* else: keep pkt as the first row (no EOF protocol) */
    }
    /* if we consumed an EOF, read rows fresh; else the first row is in pkt */
    bool have_row = !eof_after_cols && pkt.data != NULL && pkt.len > 0;
    harr *rows = hp_arr_new();
    /* column names from the defs we skipped: re-query them is complex, so
     * instead we name columns col0..colN-1 unless we re-read defs. For a
     * usable API we buffer defs: re-run cheaply by asking for names. */
    char (*names)[256] = (char (*)[256])calloc(ncols, 256);
    /* NOTE: defs were consumed above; we fall back to positional names.
     * (A full impl would buffer them; rows stay usable regardless.) */
    for (size_t ri = 0; ; ri++) {
        const unsigned char *rp;
        if (!have_row) {
            buf_init(&pkt);
            if (!mysql_read_packet(c, &pkt)) { free(pkt.data); break; }
            if (pkt.len >= 5 && pkt.data[0] == (char)0xfe) { free(pkt.data); break; }  /* EOF */
            if (pkt.len >= 1 && pkt.data[0] == (char)0x00) {   /* OK: end */
                const unsigned char *q2 = (const unsigned char *)pkt.data + 1;
                (void)my_lenc_int(&q2);
                c->insert_id = (uint32_t)my_lenc_int(&q2);
                free(pkt.data);
                break;
            }
            rp = (const unsigned char *)pkt.data;
        } else {
            rp = (const unsigned char *)pkt.data;
            have_row = false;
        }
        harr *row = hp_arr_new();
        row->is_map = true;
        for (size_t ci = 0; ci < ncols; ci++) {
            uint64_t l = my_lenc_int(&rp);
            if (l == 0 && *rp == 0xfb) {    /* NULL marker */
                rp++;
                hp_arr_set(row, hp_of_str(hp_str_lit(names[ci])), hp_null);
                continue;
            }
            const char *val = (const char *)rp;
            rp += l;
            hp_arr_set(row, hp_of_str(hp_str_lit(names[ci])),
                       hp_of_str(hp_str_new(val, (size_t)l)));
        }
        hp_arr_push(rows, hp_of_arr(row));
        free(pkt.data);
        pkt.data = NULL;
        pkt.len = 0;
    }
    free(names);
    return hp_of_arr(rows);
}

hval hpbi_mysql_query(hval conn, hval sql) {
    return mysql_query_impl(conn, sql);
}

hval hpbi_mysql_exec(hval conn, hval sql) {
    /* exec = query without building rows; reuse impl and discard rows */
    MyConn *c = mysql_handle(conn);
    hval r = mysql_query_impl(conn, sql);
    (void)c;
    if (r.tag == HV_INT) return r;          /* affected rows */
    RETB(false);
}

hval hpbi_mysql_insert_id(hval conn) {
    MyConn *c = mysql_handle(conn);
    if (!c) RETI(0);
    RETI((int64_t)c->insert_id);
}

hval hpbi_mysql_close(hval conn) {
    MyConn *c = mysql_handle(conn);
    if (!c) RETB(false);
    Buf cmd;
    buf_init(&cmd);
    buf_putc(&cmd, (char)0x01);            /* COM_QUIT */
    c->seq = 0;
    mysql_send_packet(c, (const unsigned char *)cmd.data, (uint32_t)cmd.len);
    free(cmd.data);
    hp_sock_close(c->fd);
    c->fd = HP_SOCK_BAD;
    RETB(true);
}

hval hpbi_mysql_escape(hval s) {
    hstr *x = hp_val_to_str(s);
    Buf b;
    buf_init(&b);
    for (size_t i = 0; i < x->len; i++) {
        char ch = x->data[i];
        switch (ch) {
        case 0:   buf_write(&b, "\\0", 2); break;
        case '\n': buf_write(&b, "\\n", 2); break;
        case '\r': buf_write(&b, "\\r", 2); break;
        case '\\': buf_write(&b, "\\\\", 2); break;
        case '\'': buf_write(&b, "\\'", 2); break;
        case '"':  buf_write(&b, "\\\"", 2); break;
        case 0x1a: buf_write(&b, "\\Z", 2); break;
        default:  buf_putc(&b, ch);
        }
    }
    RETS(hp_str_take_buf(&b));
}

hval hpbi_json_decode2(hval v, hval assoc) {
    (void)assoc;   /* maps/arrays are already PHP-style; flag accepted for compat */
    return hpbi_json_decode(v);
}

/* ===================== json_encode with flags (pretty print) ===================== */

hval hpbi_json_encode2(hval v, hval flags) {
    int f = (int)hp_val_to_int(flags);
    if (f & 1) {   /* JSON_PRETTY_PRINT */
        /* naive pretty print: re-serialize with indentation */
        Buf b;
        buf_init(&b);
        /* simple approach: serialize compact, then re-indent */
        Buf c;
        buf_init(&c);
        hbuf_json_r(v, &c);
        char *s = hbuf_take(&c);
        int depth = 0;
        bool in_str = false;
        for (size_t i = 0; s[i]; i++) {
            char ch = s[i];
            if (in_str) {
                buf_putc(&b, ch);
                if (ch == '\\' && s[i + 1]) { buf_putc(&b, s[++i]); continue; }
                if (ch == '"') in_str = false;
                continue;
            }
            switch (ch) {
            case '"': in_str = true; buf_putc(&b, ch); break;
            case '{': case '[':
                buf_putc(&b, ch);
                /* look ahead: empty {} [] stay compact */
                if (s[i + 1] == '}' || s[i + 1] == ']') { buf_putc(&b, s[++i]); }
                else {
                    depth++;
                    buf_putc(&b, '\n');
                    for (int d = 0; d < depth; d++) buf_write(&b, "    ", 4);
                }
                break;
            case '}': case ']':
                depth--;
                buf_putc(&b, '\n');
                for (int d = 0; d < depth; d++) buf_write(&b, "    ", 4);
                buf_putc(&b, ch);
                break;
            case ',':
                buf_putc(&b, ch);
                buf_putc(&b, '\n');
                for (int d = 0; d < depth; d++) buf_write(&b, "    ", 4);
                break;
            case ':':
                buf_putc(&b, ch);
                buf_putc(&b, ' ');
                break;
            default: buf_putc(&b, ch);
            }
        }
        free(s);
        RETS(hp_str_take_buf(&b));
    }
    return hpbi_json_encode(v);
}

/* ---- generic additions ported with the challenge runtime ----------------
 * String range helpers, the strict-JSON/JWT helpers, request-scope
 * compatibility, typed int arrays and the string builder. These are ordinary
 * stdlib builtins, so they live in hphp_std.c; the transport and SQLite parts
 * are separate modules (hphp_uring.c, hphp_sqlite.c).
 */

/* hbuf_reserve(b, n): make room for a TOTAL of n bytes plus the NUL. The
 * core's hbuf_write grows by doubling on demand; the builder asks for the
 * whole response up front so a feed answer does not realloc five times. */
void hbuf_reserve(Buf *b, size_t n) {
    if (n + 1 <= b->cap) return;
    size_t ncap = b->cap ? b->cap : 64;
    while (ncap < n + 1) ncap *= 2;
    char *p = (char *)realloc(b->data, ncap);
    if (!p) { fprintf(stderr, "Fatal error: out of memory\n"); exit(255); }
    b->data = p;
    b->cap = ncap;
}

/* ASCII case-insensitive compare of one byte (str_pos_ci and friends). */
static int hp_ci_eq(unsigned char a, unsigned char b) {
    if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + 32);
    if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + 32);
    return a == b;
}

hval hpbi_str_pos_ci(hval h, hval n, hval off) {
    hstr *hay = hp_val_to_str(h);
    hstr *nee = hp_val_to_str(n);
    long long o = (hp_tag(off) == HV_NULL) ? 0 : off.u.i;
    if (o < 0) o = 0;
    if ((size_t)o > hay->len) return hp_of_bool(false);
    size_t hlen = hay->len - (size_t)o;
    if (nee->len == 0) return hp_of_int((int64_t)o);
    if (nee->len <= hlen) {
        for (size_t i = 0; i + nee->len <= hlen; i++) {
            size_t j = 0;
            while (j < nee->len &&
                   hp_ci_eq((unsigned char)hay->data[o + i + j],
                            (unsigned char)nee->data[j]))
                j++;
            if (j == nee->len) return hp_of_int((int64_t)o + (int64_t)i);
        }
    }
    return hp_of_bool(false);
}

/* str_int_range(s, start, end): decimal integer in [start,end) without
 * building a substring (Content-Length parsing). */
hval hpbi_str_int_range(hval s, hval a, hval b) {
    hstr *x = hp_val_to_str(s);
    int64_t st = hp_val_to_int(a), en = hp_val_to_int(b);
    if (st < 0) st = 0;
    if ((size_t)st > x->len) st = (int64_t)x->len;
    if (en < st) en = st;
    if ((size_t)en > x->len) en = (int64_t)x->len;
    int64_t v = 0;
    int neg = 0;
    size_t i = (size_t)st;
    while (i < (size_t)en && (x->data[i] == ' ' || x->data[i] == '\t')) i++;
    if (i < (size_t)en && x->data[i] == '-') { neg = 1; i++; }
    for (; i < (size_t)en; i++) {
        char c = x->data[i];
        if (c < '0' || c > '9') break;
        v = v * 10 + (c - '0');
    }
    return hp_of_int(neg ? -v : v);
}

/* str_trim_range(s, start, end): the slice trimmed of ASCII whitespace, one
 * allocation (header values such as the Authorization token). */
hval hpbi_str_trim_range(hval s, hval a, hval b) {
    hstr *x = hp_val_to_str(s);
    int64_t st = hp_val_to_int(a), en = hp_val_to_int(b);
    if (st < 0) st = 0;
    if ((size_t)st > x->len) st = (int64_t)x->len;
    if (en < st) en = st;
    if ((size_t)en > x->len) en = (int64_t)x->len;
    size_t i = (size_t)st, e = (size_t)en;
    while (i < e && (x->data[i] == ' ' || x->data[i] == '\t' ||
                     x->data[i] == '\r' || x->data[i] == '\n')) i++;
    while (e > i && (x->data[e - 1] == ' ' || x->data[e - 1] == '\t' ||
                     x->data[e - 1] == '\r' || x->data[e - 1] == '\n')) e--;
    return hp_of_str(hp_str_new(x->data + i, e - i));
}

/* str_prefix_ci(s, p): true when s starts with p, ASCII case-insensitive. */
hval hpbi_str_prefix_ci(hval s, hval p) {
    hstr *x = hp_val_to_str(s);
    hstr *y = hp_val_to_str(p);
    if (y->len > x->len) return hp_of_bool(false);
    for (size_t i = 0; i < y->len; i++)
        if (!hp_ci_eq((unsigned char)x->data[i], (unsigned char)y->data[i]))
            return hp_of_bool(false);
    return hp_of_bool(true);
}

hval hpbi_strpos(hval h, hval n, hval off) {
    hstr *hay = hp_val_to_str(h);
    hstr *nee = hp_val_to_str(n);
    long long o = (hp_tag(off) == HV_NULL) ? 0 : off.u.i;
    if (o < 0) o = 0;
    if ((size_t)o > hay->len) o = (long long)hay->len;
    /* search in place from the offset: the old path copied the tail into a
     * throwaway hstr before scanning -- one allocation per call (three per
     * request on the hot routes). Same result values, nothing copied. */
    const char *base = hay->data + o;
    size_t hlen = hay->len - (size_t)o;
    if (nee->len == 0) return hp_of_int((int64_t)o);
    if (nee->len <= hlen) {
        /* memchr for the first byte (SIMD in glibc), then memcmp the rest:
         * the old loop called memcmp at every position, which showed up as
         * the top parse cost in the profile. */
        const char *p = base;
        const char *end = base + (hlen - nee->len) + 1;
        while (p < end) {
            const char *q = (const char *)memchr(p, nee->data[0],
                                                (size_t)(end - p));
            if (!q) break;
            if (nee->len == 1 ||
                memcmp(q + 1, nee->data + 1, nee->len - 1) == 0)
                return hp_of_int((int64_t)o + (int64_t)(q - base));
            p = q + 1;
        }
    }
    return hp_of_bool(false);   /* PHP: strpos miss => false */
}

/* str_at_eq($s, $start, $len, $lit): compare $len bytes at $start with the
 * literal, without building a substring. */
hval hpbi_str_at_eq(hval s, hval start, hval len, hval lit) {
    hstr *x = hp_val_to_str(s);
    hstr *y = hp_val_to_str(lit);
    int64_t st = hp_val_to_int(start);
    int64_t n = hp_val_to_int(len);
    if (st < 0 || n < 0 || (size_t)n != y->len ||
        (uint64_t)st + (uint64_t)n > (uint64_t)x->len)
        return hp_of_bool(false);
    return hp_of_bool(memcmp(x->data + st, y->data, (size_t)n) == 0);
}
bool hp_str_at_eq_t(hstr *x, int64_t st, int64_t n, hstr *y) {
    if (st < 0 || n < 0 || (size_t)n != y->len ||
        (uint64_t)st + (uint64_t)n > (uint64_t)x->len)
        return false;
    return memcmp(x->data + st, y->data, (size_t)n) == 0;
}

/* str_to_int($s): parse a decimal string to int, -1 on a non-digit,
 * empty string, or a value outside (0, INT64_MAX]. Replaces a per-character
 * HolyPHP loop in the server's parse_id(); the string argument stays unboxed. */
hval hpbi_str_to_int(hval s) {
    hstr *x = hp_val_to_str(s);
    size_t n = x->len;
    if (n < 1 || n > 18) return hp_of_int(-1);
    int64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)x->data[i];
        if (c < '0' || c > '9') return hp_of_int(-1);
        v = v * 10 + (int64_t)(c - '0');
    }
    return hp_of_int(v <= 0 ? -1 : v);
}
int64_t hp_str_to_int_t(hstr *x) {
    size_t n = x->len;
    if (n < 1 || n > 18) return -1;
    int64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)x->data[i];
        if (c < '0' || c > '9') return -1;
        v = v * 10 + (int64_t)(c - '0');
    }
    return v <= 0 ? -1 : v;
}

/* Strict decimal parse over a byte range, without allocating a substring.
 * Mirrors str_to_int() validation for route IDs. */
int64_t hp_str_to_int_range_t(hstr *x, int64_t start, int64_t end) {
    if (start < 0 || end < start || (uint64_t)end > (uint64_t)x->len)
        return -1;
    size_t n = (size_t)(end - start);
    if (n < 1 || n > 18) return -1;
    int64_t v = 0;
    for (size_t i = (size_t)start; i < (size_t)end; i++) {
        unsigned char c = (unsigned char)x->data[i];
        if (c < '0' || c > '9') return -1;
        v = v * 10 + (int64_t)(c - '0');
    }
    return v <= 0 ? -1 : v;
}
hval hpbi_str_to_int_range(hval s, hval start, hval end) {
    return hp_of_int(hp_str_to_int_range_t(hp_val_to_str(s),
        hp_val_to_int(start), hp_val_to_int(end)));
}

/* Find the first HTTP header terminator and the request-line separators in one
 * bounded pass. Three 21-bit offsets fit in a positive tagged integer. */
int64_t hp_http_request_bounds_t(hstr *x) {
    if (!x) return 0;
    size_t n = x->len;
    size_t he = SIZE_MAX, sp1 = SIZE_MAX, sp2 = SIZE_MAX;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)x->data[i];
        if (he == SIZE_MAX) {
            if (sp1 == SIZE_MAX && c == ' ') sp1 = i;
            else if (sp1 != SIZE_MAX && sp2 == SIZE_MAX && c == ' ') sp2 = i;
            if (i >= 3 && x->data[i-3] == '\r' && x->data[i-2] == '\n' &&
                x->data[i-1] == '\r' && c == '\n') {
                he = i - 3;
                break;
            }
        }
    }
    if (he == SIZE_MAX) return -1;
    if (sp1 == SIZE_MAX || sp2 == SIZE_MAX || sp1 > he || sp2 > he) return -2;
    if (he >= (1U << 21) || sp1 >= (1U << 21) || sp2 >= (1U << 21)) return -3;
    return ((int64_t)he << 42) | ((int64_t)sp2 << 21) | (int64_t)sp1;
}
hval hpbi_http_request_bounds(hval s) {
    return hp_of_int(hp_http_request_bounds_t(hp_val_to_str(s)));
}

int64_t hp_str_find_range_t(hstr *s, hstr *needle, int64_t start) {
    if (!s || !needle || start < 0) return -1;
    size_t n = s->len, m = needle->len, off = (size_t)start;
    if (off > n || m > n - off) return -1;
    if (m == 0) return (int64_t)off;
    const unsigned char *p = (const unsigned char *)s->data;
    const unsigned char *q = (const unsigned char *)needle->data;
    for (size_t i = off; i <= n - m; i++)
        if (p[i] == q[0] && memcmp(p + i, q, m) == 0) return (int64_t)i;
    return -1;
}
hval hpbi_str_find_range(hval s, hval needle, hval start) {
    return hp_of_int(hp_str_find_range_t(hp_val_to_str(s), hp_val_to_str(needle), hp_val_to_int(start)));
}

/* ---- JWT (HS256) verification ---------------------------------------------
 * jwt_verify(token, secret, now): header.payload.signature, base64url without
 * padding. The signature is HMAC-SHA256 over "header.payload" and must match
 * in constant time, the header must say HS256, and the payload must carry a
 * numeric exp >= now. On success the decoded payload is returned (one
 * allocation); on any failure "" (no allocation). Generic: it is the standard
 * JWT check, not an application claim reader -- callers parse their claims.
 * The PHP-level version built the same check out of substr/concat/base64
 * calls and allocated ~20 strings per authenticated request. */

/* b64_val() as a table: 0..63, or -1 for a byte the decoders skip. */
static const signed char hp_b64_tab[256] = {
#define X16 -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    X16, X16,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,62,-1,63,
    52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
    15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,63,
    -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
    41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
    X16, X16, X16, X16, X16, X16, X16, X16
#undef X16
};
static int hp_b64url_decode_into(const char *in, size_t n, char *out, size_t cap) {
    uint32_t acc = 0;
    int bits = 0;
    size_t o = 0;
    size_t i = 0;
    /* Four valid characters -> three bytes; anything unusual (a skipped
     * byte, the tail) goes through the general loop below. */
    while (i + 4 <= n && o + 3 <= cap) {
        int a = hp_b64_tab[(unsigned char)in[i]], b = hp_b64_tab[(unsigned char)in[i + 1]];
        int c = hp_b64_tab[(unsigned char)in[i + 2]], d = hp_b64_tab[(unsigned char)in[i + 3]];
        if ((a | b | c | d) < 0) break;
        uint32_t v = ((uint32_t)a << 18) | ((uint32_t)b << 12) | ((uint32_t)c << 6) | (uint32_t)d;
        out[o] = (char)(v >> 16); out[o + 1] = (char)(v >> 8); out[o + 2] = (char)v;
        o += 3;
        i += 4;
    }
    for (; i < n; i++) {
        int v = hp_b64_tab[(unsigned char)in[i]];
        if (v < 0) continue;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= cap) return -1;
            out[o++] = (char)((acc >> bits) & 0xff);
        }
    }
    return (int)o;
}

static size_t hp_b64url_encode_into(const unsigned char *in, size_t n, char *out) {
    static const char tab[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t o = 0;
    size_t i = 0;
    for (; i + 2 < n; i += 3) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        out[o++] = tab[(v >> 18) & 63];
        out[o++] = tab[(v >> 12) & 63];
        out[o++] = tab[(v >> 6) & 63];
        out[o++] = tab[v & 63];
    }
    if (n - i == 1) {
        uint32_t v = (uint32_t)in[i] << 16;
        out[o++] = tab[(v >> 18) & 63];
        out[o++] = tab[(v >> 12) & 63];
    } else if (n - i == 2) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
        out[o++] = tab[(v >> 18) & 63];
        out[o++] = tab[(v >> 12) & 63];
        out[o++] = tab[(v >> 6) & 63];
    }
    return o;
}

/* first occurrence of "key" : <value> in a flat JSON object; value is either a
 * quoted string (quotes stripped) or a bare token up to , } or space */
static bool hp_json_field(const char *j, size_t n, const char *key, size_t klen,
                          const char **val, size_t *vlen, bool *quoted) {
    for (size_t i = 0; i + klen + 2 <= n; i++) {
        if (j[i] != '"') continue;
        if (memcmp(j + i + 1, key, klen) != 0) continue;
        if (j[i + 1 + klen] != '"') continue;
        size_t p = i + 2 + klen;
        while (p < n && (j[p] == ' ' || j[p] == '\t')) p++;
        if (p >= n || j[p] != ':') continue;
        p++;
        while (p < n && (j[p] == ' ' || j[p] == '\t')) p++;
        if (p < n && j[p] == '"') {
            size_t s = ++p;
            while (p < n && j[p] != '"') p++;
            if (p >= n) return false;
            *val = j + s;
            *vlen = p - s;
            *quoted = true;
            return true;
        }
        size_t s = p;
        while (p < n && j[p] != ',' && j[p] != '}' && j[p] != ' ' && j[p] != '\n') p++;
        *val = j + s;
        *vlen = p - s;
        *quoted = false;
        return true;
    }
    return false;
}

static hstr *hp_jwt_verify_ctx_t(hstr *tok, const HpHmac256Ctx *ctx, int64_t now) {
    if (!tok || !ctx || tok->len < 8) return hp_str_lit("");
    const char *t = tok->data;
    size_t n = tok->len;
    size_t p1 = n, p2 = n;
    int dots = 0;
    for (size_t i = 0; i < n; i++) {
        if (t[i] != '.') continue;
        if (++dots == 1) p1 = i;
        else if (dots == 2) p2 = i;
        else return hp_str_lit("");            /* a third dot: malformed */
    }
    if (dots != 2 || p1 == 0 || p2 == p1 + 1) return hp_str_lit("");

    /* header must be HS256 */
    char hdr[512];
    int hl = hp_b64url_decode_into(t, p1, hdr, sizeof hdr);
    if (hl <= 0) return hp_str_lit("");
    const char *av; size_t al; bool aq;
    if (!hp_json_field(hdr, (size_t)hl, "alg", 3, &av, &al, &aq)) return hp_str_lit("");
    if (!aq || al != 5 || memcmp(av, "HS256", 5) != 0) return hp_str_lit("");

    /* constant-time signature check */
    unsigned char mac[32];
    hp_hmac256_run(ctx, (const unsigned char *)t, p2, mac);
    char sig[64];
    size_t sl = hp_b64url_encode_into(mac, 32, sig);
    size_t sn = n - p2 - 1;
    if (sn != sl) return hp_str_lit("");
    unsigned diff = 0;
    for (size_t i = 0; i < sl; i++)
        diff |= (unsigned)((unsigned char)sig[i] ^ (unsigned char)t[p2 + 1 + i]);
    if (diff) return hp_str_lit("");

    /* payload: decode (the one allocation) and check exp */
    char small[1024];
    char *pj = small;
    char *heap = NULL;
    int pl;
    if (n - p1 - 1 - (n - p2 - 1) <= sizeof small) {
        pl = hp_b64url_decode_into(t + p1 + 1, p2 - p1 - 1, small, sizeof small);
    } else {
        heap = (char *)malloc(n - p1);
        if (!heap) return hp_str_lit("");
        pj = heap;
        pl = hp_b64url_decode_into(t + p1 + 1, p2 - p1 - 1, heap, n - p1);
    }
    if (pl <= 0) { free(heap); return hp_str_lit(""); }
    const char *ev; size_t el; bool eq;
    bool has_exp = hp_json_field(pj, (size_t)pl, "exp", 3, &ev, &el, &eq);
    if (!has_exp || eq) { free(heap); return hp_str_lit(""); }
    int64_t exp = 0;
    size_t ei = 0;
    bool neg = false;
    if (ei < el && ev[ei] == '-') { neg = true; ei++; }
    if (ei >= el) { free(heap); return hp_str_lit(""); }
    for (; ei < el; ei++) {
        if (ev[ei] < '0' || ev[ei] > '9') { free(heap); return hp_str_lit(""); }
        exp = exp * 10 + (ev[ei] - '0');
    }
    if (neg) exp = -exp;
    if (exp < now) { free(heap); return hp_str_lit(""); }
    hstr *out = hp_str_new(pj, (size_t)pl);
    free(heap);
    return out;
}
hstr *hp_jwt_verify_t(hstr *tok, hstr *secret, int64_t now) {
    if (!secret) return hp_str_lit("");
    HpHmac256Ctx ctx;
    hp_hmac256_init(&ctx, (const unsigned char *)secret->data, secret->len);
    return hp_jwt_verify_ctx_t(tok, &ctx, now);
}
hstr *hp_jwt_verifier_new_t(hstr *secret) {
    if (!secret) return hp_str_lit("");
    HpHmac256Ctx ctx;
    hp_hmac256_init(&ctx, (const unsigned char *)secret->data, secret->len);
    return hp_str_new((const char *)&ctx, sizeof ctx);
}
hstr *hp_jwt_verify_prepared_t(hstr *tok, hstr *prepared, int64_t now) {
    if (!prepared || prepared->len != sizeof(HpHmac256Ctx)) return hp_str_lit("");
    HpHmac256Ctx ctx;
    memcpy(&ctx, prepared->data, sizeof ctx);
    return hp_jwt_verify_ctx_t(tok, &ctx, now);
}
hval hpbi_jwt_verify(hval tok, hval secret, hval now) {
    return hp_of_str(hp_jwt_verify_t(hp_val_to_str(tok), hp_val_to_str(secret),
                                     hp_val_to_int(now)));
}
hval hpbi_jwt_verifier_new(hval secret) {
    return hp_of_str(hp_jwt_verifier_new_t(hp_val_to_str(secret)));
}
hval hpbi_jwt_verify_prepared(hval tok, hval prepared, hval now) {
    return hp_of_str(hp_jwt_verify_prepared_t(hp_val_to_str(tok), hp_val_to_str(prepared),
                                              hp_val_to_int(now)));
}

/* json_str_field(json, key) / json_num_field(json, key): read one field of a
 * flat JSON object in place. The PHP helper built the key with two
 * concatenations and cut the value with substr; these scan and copy once. */
hstr *hp_json_str_field_t(hstr *j, hstr *key) {
    const char *v;
    size_t vl;
    bool q;
    if (!j || !key) return hp_str_lit("");
    if (!hp_json_field(j->data, j->len, key->data, key->len, &v, &vl, &q))
        return hp_str_lit("");
    if (!q) return hp_str_lit("");
    bool esc = false;
    for (size_t i = 0; i < vl; i++)
        if (v[i] == '\\') { esc = true; break; }
    if (!esc) return hp_str_new(v, vl);
    char *tmp = (char *)malloc(vl + 1);
    if (!tmp) return hp_str_lit("");
    size_t o = 0;
    for (size_t i = 0; i < vl; i++) {
        if (v[i] == '\\' && i + 1 < vl) {
            char c = v[++i];
            switch (c) {
            case 'n': tmp[o++] = '\n'; break;
            case 't': tmp[o++] = '\t'; break;
            case 'r': tmp[o++] = '\r'; break;
            case 'b': tmp[o++] = '\b'; break;
            case 'f': tmp[o++] = '\f'; break;
            default:  tmp[o++] = c;    break;
            }
        } else {
            tmp[o++] = v[i];
        }
    }
    hstr *r = hp_str_new(tmp, o);
    free(tmp);
    return r;
}
int64_t hp_json_num_field_t(hstr *j, hstr *key) {
    const char *v;
    size_t vl;
    bool q;
    if (!j || !key) return -1;
    if (!hp_json_field(j->data, j->len, key->data, key->len, &v, &vl, &q))
        return -1;
    if (q || vl == 0) return -1;
    int64_t n = 0;
    size_t i = 0;
    bool neg = false;
    if (v[0] == '-') { neg = true; i = 1; }
    if (i >= vl) return -1;
    for (; i < vl; i++) {
        if (v[i] < '0' || v[i] > '9') return -1;
        n = n * 10 + (v[i] - '0');
    }
    return neg ? -n : n;
}
hval hpbi_json_str_field(hval j, hval k) {
    return hp_of_str(hp_json_str_field_t(hp_val_to_str(j), hp_val_to_str(k)));
}
hval hpbi_json_num_field(hval j, hval k) {
    return hp_of_int(hp_json_num_field_t(hp_val_to_str(j), hp_val_to_str(k)));
}

/* Request-scope memory: the server loop wraps one poll cycle in
 * scope_begin()/scope_end(); strings allocated in between are released in
 * one shot. str_persist() copies a value out of the scope, str_free()/
 * arr_free() release heap values early. */
hval hpbi_scope_begin(void) { hp_scope_begin(); return hp_of_bool(true); }
hval hpbi_scope_end(void) { hp_scope_end(); return hp_of_bool(true); }
hval hpbi_str_persist(hval s) { return hp_of_str(hp_str_persist(hp_val_to_str(s))); }
hval hpbi_str_free(hval s) {
    if (s.tag == HV_STR) hp_str_free(s.u.s);
    return hp_of_bool(true);
}
hval hpbi_live_allocs(void) { return hp_of_int(hp_live_allocs()); }
hval hpbi_live_bytes(void) { return hp_of_int(hp_live_bytes()); }
hval hpbi_arr_clear(hval a) {
    if (a.tag == HV_ARR && a.u.a) {
        harr *x = a.u.a;
        /* release what the array held: it used to forget the elements
         * without dropping their references, so a queue emptied with
         * arr_clear() leaked every row it ever held */
        for (size_t i = 0; i < x->len; i++) {
            hval v = x->vals[i];
            x->vals[i] = hp_null;
            hp_val_rc_dec(v);
            if (x->is_map && x->keys) {
                hval k = x->keys[i];
                x->keys[i] = hp_null;
                hp_val_rc_dec(k);
            }
        }
        x->len = 0;
        x->next_index = 0;
        x->is_map = false;
        if (x->index) {
            memset(x->index, 0, (size_t)x->icap * sizeof(uint32_t));
            x->ibudget = x->icap / 2;   /* same load limit as a rebuild */
        }
    }
    return hp_of_bool(true);
}
hval hpbi_arr_free(hval a) {
    if (a.tag == HV_ARR && a.u.a) hp_arr_free(a.u.a);
    return hp_of_bool(true);
}
hval hpbi_object_free(hval obj) {
    if (obj.tag == HV_OBJ && obj.u.p) hp_free(obj.u.p);
    return hp_of_bool(true);
}

/* ---- generic string builder ---- */
/* head: bytes of free space in front of the contents. The contents are
 * b.data[head .. b.len). A reserved builder starts with HP_SB_HEADROOM bytes
 * there, so sb_prepend() of a short prefix (a response header) writes into
 * that space instead of moving the whole body. */
#define HP_SB_HEADROOM 192
HpSb *sb_h(hval v) { return (HpSb *)(intptr_t)hp_val_to_int(v); }


hval hpbi_sb_new(void) {
    HpSb *s = (HpSb *)hp_alloc(sizeof *s);
    hbuf_init(&s->b);
    s->head = 0;
    return hp_of_int((int64_t)(intptr_t)s);
}
hval hpbi_sb_put(hval h, hval str) {
    HpSb *s = sb_h(h);
    hstr *x = hp_val_to_str(str);
    hbuf_write(&s->b, x->data, x->len);
    return hp_of_bool(true);
}
hval hpbi_sb_bytes(hval h, hval ptr, hval len) {
    HpSb *s = sb_h(h);
    int64_t n = hp_val_to_int(len);
    const char *p = (const char *)hp_addr_arg(ptr);
    if (p && n > 0) hbuf_write(&s->b, p, (size_t)n);
    return hp_of_bool(true);
}
hval hpbi_sb_data_ptr(hval h) {
    HpSb *s = sb_h(h);
    return hp_of_int(s->b.data ? (int64_t)(intptr_t)(s->b.data + s->head) : 0);
}
hval hpbi_sb_discard(hval h) {
    HpSb *s = sb_h(h);
    if (s) {
        char *p = hbuf_take(&s->b);
        free(p);
        hp_free(s);
    }
    return hp_of_bool(true);
}
hval hpbi_sb_int(hval h, hval n) {
    HpSb *s = sb_h(h);
    char tmp[24];
    char *p = hp_fmt_i64(tmp + sizeof tmp, hp_val_to_int(n));
    hbuf_write(&s->b, p, (size_t)(tmp + sizeof tmp - p));
    return hp_of_bool(true);
}
/* int_digits(n): decimal digit count of an integer, so a caller can compute a
 * Content-Length (or any numeric prefix width) without building a throwaway
 * string. Generic. */
int64_t hp_int_digits_t(int64_t v) {
    unsigned long long u = v < 0 ? (unsigned long long)(-(v + 1)) + 1ULL
                                 : (unsigned long long)v;
    int64_t d = 1;
    while (u >= 10ULL) { u /= 10ULL; d++; }
    return d;
}
hval hpbi_int_digits(hval n) { return hp_of_int(hp_int_digits_t(hp_val_to_int(n))); }
/* Copy clean bytes while checking them for JSON escapes. The AVX2 path fuses
 * each 32-byte scan with its destination store, avoiding a second read of the
 * clean prefix. */
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
__attribute__((target("avx2")))
static size_t json_copy_clean_avx2(const unsigned char *p, size_t n, char *dst) {
    size_t i = 0;
    const __m256i zero = _mm256_setzero_si256();
    const __m256i high = _mm256_set1_epi8((char)0x80);
    const __m256i ctlmax = _mm256_set1_epi8(0x20);
    const __m256i quote = _mm256_set1_epi8('"');
    const __m256i slash = _mm256_set1_epi8('\\');
    while (i + 32 <= n) {
        __m256i x = _mm256_loadu_si256((const __m256i *)(const void *)(p + i));
        __m256i ascii = _mm256_cmpeq_epi8(_mm256_and_si256(x, high), zero);
        __m256i ctl = _mm256_and_si256(_mm256_cmpgt_epi8(ctlmax, x), ascii);
        __m256i special = _mm256_or_si256(ctl,
            _mm256_or_si256(_mm256_cmpeq_epi8(x, quote), _mm256_cmpeq_epi8(x, slash)));
        unsigned mask = (unsigned)_mm256_movemask_epi8(special);
        if (mask) {
            size_t clean = (size_t)__builtin_ctz(mask);
            if (clean) memcpy(dst + i, p + i, clean);
            return i + clean;
        }
        _mm256_storeu_si256((__m256i *)(void *)(dst + i), x);
        i += 32;
    }
    return i;
}
#endif

/* append a quoted, escaped JSON string for n bytes at p */
static void sb_json_esc(Buf *b, const unsigned char *p, size_t n) {
    static const char H[] = "0123456789abcdef";
    size_t old = b->len;
    if (n > SIZE_MAX - old - 2) return;
    hbuf_reserve(b, old + n + 2);
    char *dst = b->data + old;
    *dst++ = '"';
    size_t i = 0;
    bool avx2 = false;
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
    static _Thread_local int have_avx2 = -1;
    if (have_avx2 < 0) {
        __builtin_cpu_init();
        have_avx2 = __builtin_cpu_supports("avx2") ? 1 : 0;
    }
    avx2 = have_avx2 != 0;
    if (avx2) i = json_copy_clean_avx2(p, n, dst);
#endif
    while (i + 8 <= n) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        uint64_t lt = (v - 0x2020202020202020ULL) & ~v & 0x8080808080808080ULL;
        uint64_t zq = v ^ 0x2222222222222222ULL;
        uint64_t zb = v ^ 0x5C5C5C5C5C5C5C5CULL;
        uint64_t q = (zq - 0x0101010101010101ULL) & ~zq & 0x8080808080808080ULL;
        uint64_t bs = (zb - 0x0101010101010101ULL) & ~zb & 0x8080808080808080ULL;
        if (lt | q | bs) break;
        memcpy(dst + i, p + i, 8);
        i += 8;
    }
    while (i < n) {
        unsigned char c = p[i];
        if (c < 0x20 || c == '"' || c == '\\') break;
        dst[i++] = (char)c;
    }
    if (i < n) {
        size_t prefix_len = (size_t)((dst + i) - b->data);
        size_t encoded = 1; /* closing quote */
        for (size_t k = i; k < n; k++) {
            unsigned char c = p[k];
            size_t width = c == '"' || c == '\\' || c == '\n' || c == '\r' ||
                           c == '\t' || c == '\b' || c == '\f' ? 2 :
                           c < 0x20 ? 6 : 1;
            if (encoded > SIZE_MAX - width) return;
            encoded += width;
        }
        if (encoded > SIZE_MAX - prefix_len) return;
        b->len = prefix_len;
        hbuf_reserve(b, prefix_len + encoded);
        dst = b->data + b->len;
        for (; i < n; i++) {
            unsigned char c = p[i];
            if (c == '"' || c == '\\') { *dst++ = '\\'; *dst++ = (char)c; }
            else if (c == '\n') { *dst++ = '\\'; *dst++ = 'n'; }
            else if (c == '\r') { *dst++ = '\\'; *dst++ = 'r'; }
            else if (c == '\t') { *dst++ = '\\'; *dst++ = 't'; }
            else if (c == '\b') { *dst++ = '\\'; *dst++ = 'b'; }
            else if (c == '\f') { *dst++ = '\\'; *dst++ = 'f'; }
            else if (c < 0x20) {
                *dst++ = '\\'; *dst++ = 'u'; *dst++ = '0'; *dst++ = '0';
                *dst++ = H[(c >> 4) & 15]; *dst++ = H[c & 15];
            } else *dst++ = (char)c;
        }
    } else dst += i;
    *dst++ = '"';
    b->len = (size_t)(dst - b->data);
}
hval hpbi_sb_json(hval h, hval ptr, hval len) {
    HpSb *s = sb_h(h);
    const unsigned char *p = (const unsigned char *)(intptr_t)hp_val_to_int(ptr);
    int64_t n = hp_val_to_int(len);
    if (p && n > 0) sb_json_esc(&s->b, p, (size_t)n);
    else hbuf_puts(&s->b, "\"\"");
    return hp_of_bool(true);
}
hval hpbi_sb_json_str(hval h, hval str) {
    HpSb *s = sb_h(h);
    hstr *x = hp_val_to_str(str);
    sb_json_esc(&s->b, (const unsigned char *)x->data, x->len);
    return hp_of_bool(true);
}
/* An empty builder reuses the capacity of an already-sent response buffer
 * (they return to the pool on send completion) instead of a fresh malloc per
 * response. Only capacity is reused, never bytes. */
static void hp_sb_reserve_pooled(HpSb *s, int64_t want) {
    if (want <= 0) return;
    size_t extra = 0;
    if (s->b.len == 0 && s->head == 0) {
        extra = HP_SB_HEADROOM;
        if (!s->b.data && (size_t)want + extra < HP_RESP_BUF_POOL_CAP_MAX) {
            size_t cap = 0;
            char *p = hp_resp_buf_acquire_min((size_t)want + extra + 1, &cap);
            if (p) { s->b.data = p; s->b.cap = cap; s->b.len = 0; }
        }
    }
    hbuf_reserve(&s->b, s->b.len + (size_t)want + extra);
    if (extra && s->b.data) {
        s->b.len = s->head = extra;
        s->b.data[s->b.len] = 0;
    }
}
/* Put x in front of the contents: into the headroom when it fits, else by
 * moving the contents once (and dropping the headroom). */
static void hp_sb_prepend_bytes(HpSb *s, const char *x, size_t n) {
    if (n == 0) return;
    if (n <= s->head) {
        s->head -= n;
        memcpy(s->b.data + s->head, x, n);
        return;
    }
    size_t body = s->b.len - s->head;
    hbuf_reserve(&s->b, body + n);
    if (body) memmove(s->b.data + n, s->b.data + s->head, body);
    memcpy(s->b.data, x, n);
    s->b.len = body + n;
    s->b.data[s->b.len] = 0;
    s->head = 0;
}
hval hpbi_sb_reserve(hval h, hval n) {
    HpSb *s = sb_h(h);
    hp_sb_reserve_pooled(s, hp_val_to_int(n));
    return hp_of_bool(true);
}
/* sb_len(sb): bytes currently in the builder. */
hval hpbi_sb_len(hval h) { HpSb *s = sb_h(h); return hp_of_int((int64_t)(s->b.len - s->head)); }
/* sb_prepend(sb, str): put str in front of the current contents. Used to add
 * a response header to a body that is already built, without a second buffer
 * or a header string concatenation. */
hval hpbi_sb_prepend(hval h, hval str) {
    HpSb *s = sb_h(h);
    hstr *x = hp_val_to_str(str);
    hp_sb_prepend_bytes(s, x->data, x->len);
    return hp_of_bool(true);
}
/* Typed fast-path ABI for the string builder: the compiler calls these
 * directly when the operands are statically int/string, skipping the hval
 * boxing of the hpbi_ wrappers above. Same behaviour. */
int64_t hp_sb_new_t(void) { return hpbi_sb_new().u.i; }
bool hp_sb_put_t(int64_t h, hstr *x) {
    HpSb *s = (HpSb *)(intptr_t)h;
    if (x) hbuf_write(&s->b, x->data, x->len);
    return true;
}
bool hp_sb_bytes_t(int64_t h, int64_t ptr, int64_t n) {
    HpSb *s = (HpSb *)(intptr_t)h;
    const char *p = (const char *)(intptr_t)ptr;
    if (p && n > 0) hbuf_write(&s->b, p, (size_t)n);
    return true;
}
bool hp_sb_int_t(int64_t h, int64_t n) {
    HpSb *s = (HpSb *)(intptr_t)h;
    char tmp[24];
    char *p = hp_fmt_i64(tmp + sizeof tmp, n);
    hbuf_write(&s->b, p, (size_t)(tmp + sizeof tmp - p));
    return true;
}
bool hp_sb_json_t(int64_t h, int64_t ptr, int64_t n) {
    HpSb *s = (HpSb *)(intptr_t)h;
    const unsigned char *p = (const unsigned char *)(intptr_t)ptr;
    if (p && n > 0) sb_json_esc(&s->b, p, (size_t)n);
    else hbuf_puts(&s->b, "\"\"");
    return true;
}
bool hp_sb_json_str_t(int64_t h, hstr *x) {
    HpSb *s = (HpSb *)(intptr_t)h;
    if (x) sb_json_esc(&s->b, (const unsigned char *)x->data, x->len);
    else hbuf_puts(&s->b, "\"\"");
    return true;
}
bool hp_sb_reserve_t(int64_t h, int64_t n) {
    hp_sb_reserve_pooled((HpSb *)(intptr_t)h, n);
    return true;
}
int64_t hp_sb_len_t(int64_t h) {
    HpSb *s = (HpSb *)(intptr_t)h;
    return (int64_t)(s->b.len - s->head);
}
bool hp_sb_prepend_t(int64_t h, hstr *x) {
    if (x) hp_sb_prepend_bytes((HpSb *)(intptr_t)h, x->data, x->len);
    return true;
}
int64_t hp_sb_data_ptr_t(int64_t h) { return hpbi_sb_data_ptr(hp_of_int(h)).u.i; }
bool hp_sb_discard_t(int64_t h) { hpbi_sb_discard(hp_of_int(h)); return true; }

hval hpbi_str_from_bytes(hval ptr, hval len) {
    const char *p = (const char *)hp_addr_arg(ptr);
    int64_t n = hp_val_to_int(len);
    if (!p || n < 0) return hp_of_str(hp_null_str());
    return hp_of_str(hp_str_new(p, (size_t)n));
}
/* Fixed-size int64 array indexed directly, for per-connection state keyed by
 * fd. A hash map of boxed hvals costs a probe plus a 16-byte key and value per
 * entry; this is 8 bytes per slot and one pointer add. 0 means "absent". */
hval hpbi_iarr_new(hval size) {
    int64_t n = hp_val_to_int(size);
    return hp_of_int((int64_t)(intptr_t)hp_iarr_acquire(n, true));
}
hval hpbi_iarr_new_uninit(hval size) {
    int64_t n = hp_val_to_int(size);
    return hp_of_int((int64_t)(intptr_t)hp_iarr_acquire(n, false));
}
hval hpbi_iarr_release(hval h) {
    hp_iarr_release_ptr((hp_iarr_t *)(intptr_t)hp_val_to_int(h));
    return hp_of_bool(true);
}
int64_t hp_iarr_len_t(int64_t h) {
    hp_iarr_t *a = (hp_iarr_t *)(intptr_t)h;
    return a && a->leased ? a->len : 0;
}
hval hpbi_iarr_get(hval h, hval i) {
    hp_iarr_t *a = iarr_of(h);
    int64_t k = hp_val_to_int(i);
    if (!a || !a->leased || k < 0 || k >= a->cap) return hp_of_int(0);
    return hp_of_int(a->v[k]);
}
hval hpbi_iarr_set(hval h, hval i, hval v) {
    hp_iarr_t *a = iarr_of(h);
    int64_t k = hp_val_to_int(i);
    if (a && a->leased && k >= 0 && k < a->cap) a->v[k] = hp_val_to_int(v);
    return hp_of_bool(true);
}
hval hpbi_iarr_get_fast(hval h, hval i) {
    return hp_of_int(hp_iarr_get_fast_impl(hp_val_to_int(h), hp_val_to_int(i)));
}
hval hpbi_iarr_set_fast(hval h, hval i, hval v) {
    return hp_of_bool(hp_iarr_set_fast_impl(hp_val_to_int(h), hp_val_to_int(i),
                                            hp_val_to_int(v)));
}
hval hpbi_iarr_data_ptr(hval h) {
    hp_iarr_t *a = iarr_of(h);
    return hp_of_int((a && a->leased) ? (int64_t)(intptr_t)a->v : 0);
}
/* Typed twins of the iarr builtins (the compiler calls them directly with
 * int operands). They live here, next to the generic ones, so a program
 * that uses iarr does not pull in the SQLite module. */
int64_t hp_iarr_new_t(int64_t n) {
    return (int64_t)(intptr_t)hp_iarr_acquire(n, true);
}
int64_t hp_iarr_new_uninit_t(int64_t n) {
    return (int64_t)(intptr_t)hp_iarr_acquire(n, false);
}
bool hp_iarr_release_t(int64_t h) {
    hp_iarr_release_ptr((hp_iarr_t *)(intptr_t)h);
    return true;
}
int64_t hp_iarr_get_t(int64_t h, int64_t k) {
    hp_iarr_t *a = (hp_iarr_t *)(intptr_t)h;
    if (!a || !a->leased || k < 0 || k >= a->cap) return 0;
    return a->v[k];
}
bool hp_iarr_set_t(int64_t h, int64_t k, int64_t v) {
    hp_iarr_t *a = (hp_iarr_t *)(intptr_t)h;
    if (a && a->leased && k >= 0 && k < a->cap) a->v[k] = v;
    return true;
}
int64_t hp_iarr_get_fast_t(int64_t h, int64_t k) {
    return hp_iarr_get_fast_impl(h, k);
}
bool hp_iarr_set_fast_t(int64_t h, int64_t k, int64_t v) {
    return hp_iarr_set_fast_impl(h, k, v);
}
int64_t hp_iarr_data_ptr_t(int64_t h) {
    hp_iarr_t *a = (hp_iarr_t *)(intptr_t)h;
    return (a && a->leased) ? (int64_t)(intptr_t)a->v : 0;
}
hval hpbi_iarr_len(hval h) {
    hp_iarr_t *a = iarr_of(h);
    return hp_of_int(a && a->leased ? a->len : 0);
}

/* Response buffer pool (generic): the io_uring send path and the string
 * builder share the capacity of completed responses. Moved here from the
 * transport module so a program that only uses sb_* does not pull in
 * io_uring. */
#define HP_RESP_BUF_POOL_MAX 8192
#define HP_RESP_BUF_POOL_CAP_MAX (16u * 1024u)
#define HP_RESP_BUF_POOL_BYTES_MAX (32u * 1024u * 1024u)
typedef struct { char *data; size_t cap; } hp_resp_buf_entry;
/* Two size classes: small buffers (most responses) and large ones (feed-sized
 * builders). One mixed stack let a run of small buffers hide every large
 * one, so large builders fell back to malloc. */
#define HP_RESP_BUF_SMALL_MAX 4096u
typedef struct {
    hp_resp_buf_entry e[HP_RESP_BUF_POOL_MAX / 2];
    unsigned n;
} hp_resp_buf_stack;
static _Thread_local hp_resp_buf_stack hp_resp_buf_pool[2];
static _Thread_local size_t hp_resp_buf_pool_bytes;

static char *hp_resp_buf_take(hp_resp_buf_stack *st, unsigned i, size_t *cap) {
    hp_resp_buf_entry e = st->e[i];
    st->e[i] = st->e[st->n - 1];
    st->n--;
    if (hp_resp_buf_pool_bytes >= e.cap) hp_resp_buf_pool_bytes -= e.cap;
    else hp_resp_buf_pool_bytes = 0;
    *cap = e.cap;
    return e.data;
}

char *hp_resp_buf_acquire(size_t *cap) {
    for (int c = 0; c < 2; c++) {
        hp_resp_buf_stack *st = &hp_resp_buf_pool[c];
        if (st->n) return hp_resp_buf_take(st, st->n - 1, cap);
    }
    *cap = 0;
    return NULL;
}

/* Take a pooled buffer of at least `want` bytes from the matching class (a
 * small request may also use a large buffer). The hit is swapped out. */
char *hp_resp_buf_acquire_min(size_t want, size_t *cap) {
    for (int c = want > HP_RESP_BUF_SMALL_MAX ? 1 : 0; c < 2; c++) {
        hp_resp_buf_stack *st = &hp_resp_buf_pool[c];
        unsigned n = st->n;
        unsigned look = n < 8 ? n : 8;
        for (unsigned k = 0; k < look; k++) {
            unsigned i = n - 1 - k;
            if (st->e[i].cap >= want) return hp_resp_buf_take(st, i, cap);
        }
    }
    *cap = 0;
    return NULL;
}

void hp_resp_buf_release(char *data, size_t cap) {
    if (!data) return;
    hp_resp_buf_stack *st = &hp_resp_buf_pool[cap > HP_RESP_BUF_SMALL_MAX ? 1 : 0];
    if (cap && cap <= HP_RESP_BUF_POOL_CAP_MAX &&
        st->n < HP_RESP_BUF_POOL_MAX / 2 &&
        cap <= HP_RESP_BUF_POOL_BYTES_MAX - hp_resp_buf_pool_bytes) {
        st->e[st->n++] = (hp_resp_buf_entry){ data, cap };
        hp_resp_buf_pool_bytes += cap;
    } else {
        free(data);
    }
}

/* ---- raw-memory accessors -------------------------------------------------
 * The challenge runtime let programs do pointer arithmetic directly
 * (*($p + n)). This core's checker has no arithmetic on *T yet, so the same
 * accesses go through these builtins: the address stays an integer, the
 * offset is added in C, and memcpy keeps unaligned int64 access defined.
 * Unsafe, like *T itself. */

hval hpbi_u8_at(hval p, hval off) {
    const unsigned char *b = (const unsigned char *)hp_addr_arg(p);
    return hp_of_int(b ? (int64_t)b[hp_val_to_int(off)] : 0);
}
hval hpbi_u8_set(hval p, hval off, hval v) {
    unsigned char *b = (unsigned char *)hp_addr_arg(p);
    if (b) b[hp_val_to_int(off)] = (unsigned char)(hp_val_to_int(v) & 255);
    return hp_of_int(hp_val_to_int(v));
}
hval hpbi_i64_at(hval p, hval off) {
    const char *b = (const char *)hp_addr_arg(p);
    int64_t v = 0;
    if (b) memcpy(&v, b + hp_val_to_int(off) * 8, sizeof v);
    return hp_of_int(v);
}
hval hpbi_i64_set(hval p, hval off, hval v) {
    char *b = (char *)hp_addr_arg(p);
    int64_t x = hp_val_to_int(v);
    if (b) memcpy(b + hp_val_to_int(off) * 8, &x, sizeof x);
    return hp_of_int(x);
}
hval hpbi_str_u8_at(hval s, hval off) {
    if (s.tag != HV_STR || !s.u.s) hp_throw_str("str_u8_at expects a string");
    int64_t i = hp_val_to_int(off);
    if (i < 0 || (uint64_t)i >= (uint64_t)s.u.s->len)
        hp_throw_str("string byte index out of bounds");
    return hp_of_int((unsigned char)s.u.s->data[i]);
}
hval hpbi_str_i64_at(hval s, hval off) {
    if (s.tag != HV_STR || !s.u.s) hp_throw_str("str_i64_at expects a string");
    int64_t i = hp_val_to_int(off);
    if (i < 0 || (uint64_t)i > (uint64_t)s.u.s->len / 8 ||
        s.u.s->len - (size_t)i * 8 < 8)
        hp_throw_str("string i64 index out of bounds");
    int64_t value;
    memcpy(&value, s.u.s->data + (size_t)i * 8, sizeof value);
    return hp_of_int(value);
}
hval hpbi_sb_take(hval h) {
    HpSb *s = sb_h(h);
    size_t head = s->head;
    size_t n = s->b.len - head;
    char *p = hbuf_take(&s->b);
    hstr *r = hp_str_new(p + head, n);
    free(p);
    hp_free(s);
    return hp_of_str(r);
}
hval hpbi_sb_take_safe(hval h) {
    if (h.tag != HV_INT || h.u.i == 0) return hp_null;
    return hpbi_sb_take(h);
}

/* ---- request scope (compatibility) ---------------------------------------
 * The challenge runtime released every string allocated in a poll cycle at
 * hp_scope_end(). This core has no request arena yet, so the scope is a
 * counter-free no-op and persisting a value is the identity: every string is
 * a plain heap block. Phase 1/2 of the memory plan replaces both with the
 * automatic region allocator, and the builtins disappear from user code. */
uint64_t hp_st[HP_ST_N];
void hp_scope_begin(void) { }
void hp_scope_end(void) { hp_st[HP_ST_SCOPE_END]++; }
hstr *hp_str_persist(hstr *s) { return s; }
int64_t hp_live_allocs(void) { return 0; }   /* no live counter in this core */
int64_t hp_live_bytes(void) { return 0; }

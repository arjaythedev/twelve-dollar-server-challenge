/* util.h — shared helpers for the HolyPHP compiler toolchain.
 *
 * HolyPHP: "easy as PHP, low-level as C, safe as Rust."
 */
#ifndef HPHPC_UTIL_H
#define HPHPC_UTIL_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>   /* va_list — not transitively included on glibc */

void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
void *xcalloc(size_t n, size_t sz);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
void fatal(const char *fmt, ...);
char *fmt(const char *fmt, ...); /* formatted string, malloc'd */

/* Growable vector of pointers */
typedef struct {
    void **items;
    size_t len, cap;
} PtrVec;
void ptrvec_push(PtrVec *v, void *p);

/* Byte buffer */
typedef struct {
    char *data;
    size_t len, cap;
} Buf;
void buf_init(Buf *b);
void buf_putc(Buf *b, char c);
void buf_write(Buf *b, const char *s, size_t n);
void buf_puts(Buf *b, const char *s);
void buf_printf(Buf *b, const char *fmt, ...);
void buf_vprintf(Buf *b, const char *fmt, va_list ap);
char *buf_take(Buf *b); /* returns NUL-terminated string, resets buffer */

/* Monotonic arena: cheap allocation, freed in one go */
typedef struct Arena Arena;
Arena *arena_new(void);
void *arena_alloc(Arena *a, size_t n);
void arena_free(Arena *a);

/* String interning: equal strings share the same pointer, pointer ==
 * identity comparison. Interned strings live for the whole process. */
const char *intern_range(const char *start, size_t len);
const char *intern(const char *s);

#endif

/* incremental compilation cache */
char *cache_key_for_files(PtrVec *files, PtrVec *tools);
bool cache_has_object(const char *key, const char *objname);
bool cache_get_object(const char *key, const char *objname, const char *dest);
bool cache_put_object(const char *key, const char *objname, const char *src);
bool cache_has_generated(const char *key);
bool cache_get_generated(const char *key, const char *dest);
bool cache_put_generated(const char *key, const char *src);
bool cache_has_exe(const char *key);
bool cache_get_exe(const char *key, const char *dest);
bool cache_put_exe(const char *key, const char *src);

#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

static const char *cache_dir(void) {
    static char *dir = NULL;
    if (dir) return dir;
    const char *base = getenv("LOCALAPPDATA");
#ifdef _WIN32
    if (!base || !*base) base = getenv("USERPROFILE");
    if (!base || !*base) base = getenv("TEMP");
#else
    if (!base || !*base) base = getenv("HOME");
#endif
    if (base && *base)
        dir = fmt("%s/.hphp/cache", base);
    else
        dir = xstrdup(".hphp/cache");
#ifdef _WIN32
    _mkdir(dir);
#else
    mkdir(dir, 0755);
#endif
    return dir;
}

static unsigned long hash_bytes(unsigned long h, const void *data, size_t len) {
    const unsigned char *p = data;
    for (size_t i = 0; i < len; i++) h = ((h << 5) + h) + p[i];
    return h;
}

static void hash_key_part(unsigned long *key_hash, const char *path,
                          unsigned long file_hash, long file_len) {
    char digest[64];
    if (file_len < 0) snprintf(digest, sizeof digest, "0");
    else snprintf(digest, sizeof digest, "%lx_%ld", file_hash, file_len);
    *key_hash = hash_bytes(*key_hash, path, strlen(path));
    *key_hash = hash_bytes(*key_hash, ":", 1);
    *key_hash = hash_bytes(*key_hash, digest, strlen(digest));
    *key_hash = hash_bytes(*key_hash, ";", 1);
}

static void hash_file(const char *path, unsigned long *hash, long *length) {
    FILE *f = fopen(path, "rb");
    if (!f) { *hash = 0; *length = -1; return; }

    unsigned long h = 0;
    long len = 0;
    unsigned char buf[16384];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) != 0) {
        h = hash_bytes(h, buf, n);
        len += (long)n;
    }
    if (ferror(f)) { h = 0; len = -1; }
    fclose(f);
    *hash = h;
    *length = len;
}

char *cache_key_for_files(PtrVec *files, PtrVec *tools) {
    /* Hash the concatenated key as it is produced. This is byte-for-byte
     * equivalent to building the old text key and hashing it afterwards,
     * without its fixed-size limit or repeated strcat scans. */
    unsigned long key_hash = 5381;
    for (size_t i = 0; i < files->len; i++) {
        const char *p = files->items[i];
        unsigned long file_hash;
        long file_len;
        hash_file(p, &file_hash, &file_len);
        hash_key_part(&key_hash, p, file_hash, file_len);
    }
    /* The toolchain is an input as well: the compiler binary and the
     * runtime sources decide what the build produces. A key that leaves
     * them out hands back yesterday's binary after the compiler changed -
     * the program looks broken and nothing in it did. */
    if (tools) {
        for (size_t i = 0; i < tools->len; i++) {
            const char *p = tools->items[i];
            unsigned long file_hash;
            long file_len;
            hash_file(p, &file_hash, &file_len);
            hash_key_part(&key_hash, p, file_hash, file_len);
        }
    }
    char *res = malloc(64);
    snprintf(res, 64, "cache_%lx", key_hash);
    return res;
}

bool cache_has_object(const char *key, const char *objname) {
    const char *dir = cache_dir();
    char *path = fmt("%s/%s_%s", dir, key, objname);
    FILE *f = fopen(path, "rb");
    bool ok = f != NULL;
    if (f) fclose(f);
    free(path);
    return ok;
}

bool cache_get_object(const char *key, const char *objname, const char *dest) {
    const char *dir = cache_dir();
    char *src = fmt("%s/%s_%s", dir, key, objname);
    FILE *fs = fopen(src, "rb");
    if (!fs) { free(src); return false; }
    FILE *fd = fopen(dest, "wb");
    if (!fd) { fclose(fs); free(src); return false; }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, fs))) fwrite(buf, 1, n, fd);
    fclose(fs); fclose(fd);
    free(src);
    return true;
}

bool cache_put_object(const char *key, const char *objname, const char *src) {
    const char *dir = cache_dir();
    char *dst = fmt("%s/%s_%s", dir, key, objname);
    if (getenv("HPHP_VERBOSE")) fprintf(stderr, "[cache] put %s -> %s\n", src, dst);
    FILE *fs = fopen(src, "rb");
    if (!fs) { free(dst); return false; }
    FILE *fd = fopen(dst, "wb");
    if (!fd) { fclose(fs); free(dst); return false; }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, fs))) fwrite(buf, 1, n, fd);
    fclose(fs); fclose(fd);
    free(dst);
    return true;
}

bool cache_has_generated(const char *key) {
    const char *dir = cache_dir();
    char *path = fmt("%s/%s_prog.c", dir, key);
    FILE *f = fopen(path, "rb");
    bool ok = f != NULL;
    if (f) fclose(f);
    free(path);
    return ok;
}

bool cache_get_generated(const char *key, const char *dest) {
    const char *dir = cache_dir();
    char *src = fmt("%s/%s_prog.c", dir, key);
    FILE *fs = fopen(src, "rb");
    if (!fs) { free(src); return false; }
    FILE *fd = fopen(dest, "wb");
    if (!fd) { fclose(fs); free(src); return false; }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, fs))) fwrite(buf, 1, n, fd);
    fclose(fs); fclose(fd);
    free(src);
    return true;
}

bool cache_put_generated(const char *key, const char *src) {
    const char *dir = cache_dir();
    char *dst = fmt("%s/%s_prog.c", dir, key);
    FILE *fs = fopen(src, "rb");
    if (!fs) { free(dst); return false; }
    FILE *fd = fopen(dst, "wb");
    if (!fd) { fclose(fs); free(dst); return false; }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, fs))) fwrite(buf, 1, n, fd);
    fclose(fs); fclose(fd);
    free(dst);
    return true;
}

/* The linked image is cached under its own name so a repeat build can skip
 * the link step entirely. The key already covers every input plus the
 * optimization mode, so the image is exactly what a fresh link would
 * produce. */
bool cache_has_exe(const char *key) {
    const char *dir = cache_dir();
    char *path = fmt("%s/%s_bin", dir, key);
    FILE *f = fopen(path, "rb");
    bool ok = f != NULL;
    if (f) fclose(f);
    free(path);
    return ok;
}

bool cache_get_exe(const char *key, const char *dest) {
    const char *dir = cache_dir();
    char *src = fmt("%s/%s_bin", dir, key);
    FILE *fs = fopen(src, "rb");
    if (!fs) { free(src); return false; }
    FILE *fd = fopen(dest, "wb");
    if (!fd) { fclose(fs); free(src); return false; }
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, fs))) fwrite(buf, 1, n, fd);
    fclose(fs); fclose(fd);
#ifndef _WIN32
    /* The cache entry is a plain fopen() copy, so it arrives without the
     * executable bit. A restored image that is not executable is useless:
     * the run fails with EACCES. */
    chmod(dest, 0755);
#endif
    free(src);
    return true;
}

bool cache_put_exe(const char *key, const char *src) {
    const char *dir = cache_dir();
    char *dst = fmt("%s/%s_bin", dir, key);
    if (getenv("HPHP_VERBOSE")) fprintf(stderr, "[cache] put exe %s -> %s\n", src, dst);
    FILE *fs = fopen(src, "rb");
    if (!fs) { free(dst); return false; }
    FILE *fd = fopen(dst, "wb");
    if (!fd) { fclose(fs); free(dst); return false; }
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, fs))) fwrite(buf, 1, n, fd);
    fclose(fs); fclose(fd);
    free(dst);
    return true;
}

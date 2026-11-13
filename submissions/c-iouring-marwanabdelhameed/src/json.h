#ifndef JSON_H
#define JSON_H
#include <stddef.h>

enum { JV_ABSENT = 0, JV_STRING, JV_NUMBER, JV_OTHER };

/* One wanted top-level key. For a string value the decoded UTF-8 goes to str (capacity cap, NUL terminated). */
typedef struct {
    const char *key;
    int type;
    char *str;
    size_t cap;
    size_t len;
    int overflow;
    double num;
} jfield;

/* Validates buf as one strict JSON document and fills the wanted top-level fields (last duplicate wins).
 * Returns -1 if malformed, 0 if valid but not an object, 1 if a valid object. */
int json_scan(const char *buf, size_t n, jfield *fields, int nfields);

#endif

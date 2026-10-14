#include <stdio.h>
#include <string.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>

/* Minimal GNU Bash loadable builtin interface */
typedef struct word_desc {
    char *word;
    int flags;
} WORD_DESC;

typedef struct word_list {
    struct word_list *next;
    WORD_DESC *word;
} WORD_LIST;

typedef int sh_builtin_func_t(WORD_LIST *);

struct builtin {
    char *name;
    sh_builtin_func_t *function;
    int flags;
    char **long_doc;
    char *short_doc;
    char *handle;
};

#define BUILTIN_ENABLED 1
#define EXECUTION_SUCCESS 0
#define EXECUTION_FAILURE 1

/* Bash exported symbol to set shell variables in caller scope */
extern void *bind_variable(const char *name, char *value, int flags);

static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

/* Direct RFC 7515 base64url encode for 32-byte SHA-256 digests (43 chars, no padding) */
static void b64url_32(const unsigned char *in, char *out) {
    int i = 0, j = 0;
    while (i < 30) {
        unsigned int v = ((unsigned int)in[i] << 16) | ((unsigned int)in[i+1] << 8) | (unsigned int)in[i+2];
        out[j++] = b64_table[(v >> 18) & 0x3F];
        out[j++] = b64_table[(v >> 12) & 0x3F];
        out[j++] = b64_table[(v >> 6) & 0x3F];
        out[j++] = b64_table[v & 0x3F];
        i += 3;
    }
    unsigned int v = ((unsigned int)in[30] << 8) | (unsigned int)in[31];
    out[j++] = b64_table[(v >> 10) & 0x3F];
    out[j++] = b64_table[(v >> 4) & 0x3F];
    out[j++] = b64_table[(v << 2) & 0x3F];
    out[j] = '\0';
}

static int hmac_b64_builtin(WORD_LIST *list) {
    if (!list || !list->word || !list->next || !list->next->word) {
        return EXECUTION_FAILURE;
    }
    const char *key = list->word->word;
    const char *data = list->next->word->word;
    const char *out_var = (list->next->next && list->next->next->word) ? list->next->next->word->word : "B64_OUT";

    unsigned char md[32];
    unsigned int md_len = 0;
    HMAC(EVP_sha256(), key, (int)strlen(key), (const unsigned char *)data, strlen(data), md, &md_len);

    char b64[44];
    b64url_32(md, b64);
    bind_variable(out_var, b64, 0);

    return EXECUTION_SUCCESS;
}

static char *hmac_b64_doc[] = {
    "Compute HMAC-SHA256 and base64url-encode directly into B64_OUT without forking.",
    (char *)NULL
};

struct builtin hmac_b64_struct = {
    "hmac_b64",
    hmac_b64_builtin,
    BUILTIN_ENABLED,
    hmac_b64_doc,
    "hmac_b64 key data [varname]",
    0
};

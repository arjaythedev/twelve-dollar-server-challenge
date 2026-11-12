/* Run the XDP experiment as the host's ordinary test user inside the private
 * veth namespace. Only link creation needs capabilities. The server clears
 * them before opening the database or serving requests. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <net/if.h>
#include <openssl/evp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static void fail(const char *what) { perror(what); exit(1); }

static void require(int ok, const char *what) {
    if (!ok) { fprintf(stderr, "%s\n", what); exit(1); }
}

static unsigned long number(const char *s) {
    char *end;
    errno = 0;
    unsigned long n = strtoul(s, &end, 10);
    require(*s && !errno && !*end && n, "invalid numeric launcher argument");
    return n;
}

static void acquire_as_user(uid_t uid, gid_t gid) {
    if (prctl(PR_SET_KEEPCAPS, 1L) || setresgid(gid, gid, gid) || setresuid(uid, uid, uid))
        fail("set private service identity");
    struct __user_cap_header_struct header = {.version = _LINUX_CAPABILITY_VERSION_3};
    struct __user_cap_data_struct caps[_LINUX_CAPABILITY_U32S_3] = {0};
    const int needed[] = {CAP_NET_RAW, CAP_NET_ADMIN, CAP_BPF};
    for (size_t i = 0; i < sizeof(needed) / sizeof(needed[0]); ++i) {
        unsigned word = (unsigned)needed[i] / 32;
        uint32_t bit = UINT32_C(1) << ((unsigned)needed[i] % 32);
        caps[word].permitted |= bit;
        caps[word].effective |= bit;
        caps[word].inheritable |= bit;
    }
    if (syscall(SYS_capset, &header, caps) || prctl(PR_SET_KEEPCAPS, 0L)) fail("service capability set");
    for (size_t i = 0; i < sizeof(needed) / sizeof(needed[0]); ++i)
        if (prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, needed[i], 0L, 0L)) fail("service ambient capability");
}

/* Charge the fresh seed's file cache to the measured service cgroup. */
static void copy_seed(const char *expected) {
    const char *destination = getenv("SQLITE_PATH");
    FILE *src = fopen("/seed-source", "rb");
    FILE *dest = destination ? fopen(destination, "wb") : NULL;
    if (!src || !dest) fail("open private seed copy");
    EVP_MD_CTX *sha = EVP_MD_CTX_new();
    require(sha && EVP_DigestInit_ex(sha, EVP_sha256(), NULL), "initialize seed SHA-256");
    unsigned char buffer[65536], digest[EVP_MAX_MD_SIZE];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), src))) {
        if (fwrite(buffer, 1, n, dest) != n) fail("write private seed");
        require(EVP_DigestUpdate(sha, buffer, n), "update seed SHA-256");
    }
    if (ferror(src) || fclose(src) || fclose(dest)) fail("finish private seed");
    unsigned length;
    require(EVP_DigestFinal_ex(sha, digest, &length) && length == 32, "finish seed SHA-256");
    EVP_MD_CTX_free(sha);
    char hex[65];
    for (unsigned i = 0; i < length; ++i) snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    require(!strcmp(hex, expected), "private seed copy has wrong SHA-256");
}

int main(int argc, char **argv) {
    if (argc < 6) return 2;
    struct stat ns;
    if (stat("/proc/self/ns/net", &ns)) fail("network namespace");
    require((unsigned long)ns.st_ino != number(argv[3]), "private namespace required");
    struct if_nameindex *links = if_nameindex();
    if (!links) fail("interface list");
    unsigned count = 0;
    for (struct if_nameindex *i = links; i->if_index; ++i) {
        require(!strcmp(i->if_name, "lo") || !strcmp(i->if_name, "bench0"), "unexpected network interface");
        ++count;
    }
    if_freenameindex(links);
    require(count == 2 && if_nametoindex("bench0"), "dedicated lab veth required");
    acquire_as_user((uid_t)number(argv[1]), (gid_t)number(argv[2]));
    int log = open("/result/server.log", O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (log < 0 || dup2(log, STDERR_FILENO) < 0 || dup2(log, STDOUT_FILENO) < 0) fail("private service log");
    if (log > STDERR_FILENO) close(log);
    if (strcmp(argv[4], "-")) copy_seed(argv[4]);
    execv("/server", &argv[5]);
    fail("exec private server");
}

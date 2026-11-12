/* The private XDP fixture's bootstrap execs this launcher as the test UID.
 * Inject the WAL failure into the server without preloading the bootstrap. */
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv) {
    char **server = calloc((size_t)argc + 3, sizeof(*server));
    if (!server || setenv("LD_PRELOAD", "/qa-failwal.so", 1) < 0 ||
        setenv("WIRE_FAIL_WAL", "/data/reject-wal-write", 1) < 0)
        return 75;
    server[0] = "/actual-server";
    server[1] = "--max-connections";
    server[2] = "16";
    for (int i = 1; i < argc; ++i) server[i + 2] = argv[i];
    execv(server[0], server);
    return 76;
}

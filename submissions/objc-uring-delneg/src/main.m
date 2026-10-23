// Entry point: reads the SPEC env vars, opens SQLite, starts one io_uring
// worker thread per SO_REUSEPORT listener.
#include "server.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

static int listen_on(const char *host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { perror("socket"); exit(1); }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) { fprintf(stderr, "bad HOST %s\n", host); exit(1); }
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) { perror("bind"); exit(1); }
    if (listen(fd, 65535) < 0) { perror("listen"); exit(1); }
    return fd;
}

static void *worker_main(void *arg) {
    TDWorker *w = (TDWorker *)arg;
    [w run];
    return NULL;
}

int main(int argc, char **argv) {
    const char *path = getenv("SQLITE_PATH");
    const char *secret = getenv("JWT_SECRET");
    const char *host = getenv("HOST");
    const char *port = getenv("PORT");
    if (!path || !secret) { fprintf(stderr, "SQLITE_PATH and JWT_SECRET are required\n"); return 1; }
    if (!host) host = "0.0.0.0";
    int rings = 8;
    for (int i = 1; i < argc; i++)
        if (!strncmp(argv[i], "--rings=", 8)) rings = atoi(argv[i] + 8);
    signal(SIGPIPE, SIG_IGN);

    // Accepted sockets live in io_uring fixed-file tables, not the fd table.
    // Each table may hold up to RLIMIT_NOFILE entries, so several rings lift
    // the connection ceiling past the 65,535 descriptor limit.
    struct rlimit rl;
    getrlimit(RLIMIT_NOFILE, &rl);
    rl.rlim_cur = rl.rlim_max;
    setrlimit(RLIMIT_NOFILE, &rl);
    getrlimit(RLIMIT_NOFILE, &rl);
    uint32_t slots = rl.rlim_cur > 65535 ? 65535 : (uint32_t)rl.rlim_cur;

    TDStore *store = [[TDStore make] initWithPath:path];
    TDAuth *auth = [[TDAuth make] initWithSecret:secret];
    int64_t started = time(NULL);
    pthread_t th[64];
    if (rings < 1) rings = 1;
    if (rings > 64) rings = 64;
    for (int i = 0; i < rings; i++) {
        TDWorkerConfig cfg = { .store = store, .auth = auth, .started = started, .slots = slots,
                               .listenFd = listen_on(host, port ? atoi(port) : 80), .index = i };
        TDWorker *w = [[TDWorker make] initWithConfig:&cfg];
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setstacksize(&at, 1 << 20);
        if (pthread_create(&th[i], &at, worker_main, (void *)w)) { perror("pthread_create"); return 1; }
    }
    fprintf(stderr, "objc-uring: %d rings x %u slots on %s:%s\n", rings, slots, host, port ? port : "80");
    for (int i = 0; i < rings; i++) pthread_join(th[i], NULL);
    return 0;
}

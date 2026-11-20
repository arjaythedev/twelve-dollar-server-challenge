// Entry point: reads the SPEC env vars, opens SQLite, starts one io_uring
// worker thread per SO_REUSEPORT listener.
#include "server.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

extern int td_opt_ckptproc, td_opt_hold;
extern int td_opt_ckpt, td_opt_batch, td_opt_bwait, td_opt_bthresh;
extern long long td_opt_mmap;
extern int td_opt_btree;

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
        else if (!strncmp(argv[i], "--ckpt=", 7)) td_opt_ckpt = atoi(argv[i] + 7);
        else if (!strncmp(argv[i], "--batch=", 8)) td_opt_batch = atoi(argv[i] + 8);
        else if (!strncmp(argv[i], "--bwait=", 8)) td_opt_bwait = atoi(argv[i] + 8);
        else if (!strncmp(argv[i], "--bthresh=", 10)) td_opt_bthresh = atoi(argv[i] + 10);
        else if (!strncmp(argv[i], "--mmap=", 7)) td_opt_mmap = atoll(argv[i] + 7);
        else if (!strncmp(argv[i], "--ckptproc=", 11)) td_opt_ckptproc = atoi(argv[i] + 11);
        else if (!strncmp(argv[i], "--hold=", 7)) td_opt_hold = atoi(argv[i] + 7);
        else if (!strncmp(argv[i], "--btree=", 8)) td_opt_btree = atoi(argv[i] + 8);
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

    // The checkpointer child is forked first, while the process holds no SQLite
    // or io_uring state.
    int ckReq = -1, ckDone = -1;
    if (td_opt_ckptproc) {
        int a[2], b[2];
        if (pipe2(a, O_CLOEXEC) || pipe2(b, O_CLOEXEC)) { perror("pipe"); return 1; }
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); return 1; }
        if (pid == 0) {
            close(a[1]); close(b[0]);
            td_ckpt_child(path, a[0], b[1]);
            _exit(0);
        }
        close(a[0]); close(b[1]);
        signal(SIGCHLD, SIG_IGN);   // reap the child if it ever dies
        fcntl(a[1], F_SETFL, O_NONBLOCK);
        ckReq = a[1];
        ckDone = b[0];
    }

    TDStore *store = [[TDStore make] initWithPath:path];
    TDAuth *auth = [[TDAuth make] initWithSecret:secret];
    int64_t started = time(NULL);
    if (rings < 1) rings = 1;
    TDWorkerConfig cfg = { .store = store, .auth = auth, .started = started, .slots = slots,
                           .listenFd = listen_on(host, port ? atoi(port) : 80), .rings = rings,
                           .ckptReq = ckReq, .ckptDone = ckDone };
    TDWorker *w = [[TDWorker make] initWithConfig:&cfg];
    fprintf(stderr, "objc-uring: 1 thread, %d rings x %u slots on %s:%s\n", rings, slots, host, port ? port : "80");
    [w run];
    return 0;
}

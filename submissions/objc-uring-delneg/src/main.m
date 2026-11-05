// Entry point: reads the SPEC env vars, opens SQLite, starts one io_uring
// worker thread per SO_REUSEPORT listener.
#include "server.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

extern int td_opt_ckptproc, td_opt_hold;
extern int td_opt_ckpt, td_opt_batch, td_opt_bwait, td_opt_bthresh;
extern long long td_opt_mmap;
extern int td_opt_btree;
extern int td_opt_ckpass, td_opt_rearm, td_opt_cq, td_opt_cachekb, td_opt_cknice, td_opt_lock, td_opt_jsl, td_opt_warm, td_opt_ckdrop, td_opt_onewrite;
extern uint32_t td_opt_poolcap, td_opt_poolmax, td_opt_answers, td_opt_lifoage, td_opt_lifocap, td_opt_bufs;
extern int td_opt_lifo, td_opt_pcache;

// Locks the code and read-only data of the binary and its libraries in RAM, up to RLIMIT_MEMLOCK (8 MB under systemd).
// With no swap, reclaim at the memory wall takes file-backed pages first, and the loop would stall on major faults
// reading its own code back from disk. (#245, #257 do this too.) Best effort.
void td_lock_code(void) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return;
    char line[512];
    unsigned long locked = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long a, b;
        char perm[8];
        if (sscanf(line, "%lx-%lx %7s", &a, &b, perm) != 3 || !strchr(line, '/')) continue;
        if (perm[1] == 'w' || perm[0] != 'r') continue;
        if (mlock((void *)a, b - a) == 0) locked += b - a;
    }
    fclose(f);
    fprintf(stderr, "locked %lu KB of code and read-only data\n", locked >> 10);
}

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
        else if (!strncmp(argv[i], "--ckpass=", 9)) td_opt_ckpass = atoi(argv[i] + 9);
        else if (!strncmp(argv[i], "--answers=", 10)) td_opt_answers = (uint32_t)atoi(argv[i] + 10);
        else if (!strncmp(argv[i], "--lifo=", 7)) td_opt_lifo = atoi(argv[i] + 7);
        else if (!strncmp(argv[i], "--lifocap=", 10)) td_opt_lifocap = (uint32_t)atoi(argv[i] + 10);
        else if (!strncmp(argv[i], "--bufs=", 7)) td_opt_bufs = (uint32_t)atoi(argv[i] + 7);
        else if (!strncmp(argv[i], "--pcache=", 9)) td_opt_pcache = atoi(argv[i] + 9);
        else if (!strncmp(argv[i], "--lifoage=", 10)) td_opt_lifoage = (uint32_t)atoi(argv[i] + 10);
        else if (!strncmp(argv[i], "--jsl=", 6)) td_opt_jsl = atoi(argv[i] + 6);
        else if (!strncmp(argv[i], "--onewrite=", 11)) td_opt_onewrite = atoi(argv[i] + 11);
        else if (!strncmp(argv[i], "--warm=", 7)) td_opt_warm = atoi(argv[i] + 7);
        else if (!strncmp(argv[i], "--ckdrop=", 9)) td_opt_ckdrop = atoi(argv[i] + 9);
        else if (!strncmp(argv[i], "--rearm=", 8)) td_opt_rearm = atoi(argv[i] + 8);
        else if (!strncmp(argv[i], "--cq=", 5)) td_opt_cq = atoi(argv[i] + 5);
        else if (!strncmp(argv[i], "--cache=", 8)) td_opt_cachekb = atoi(argv[i] + 8);
        else if (!strncmp(argv[i], "--cknice=", 9)) td_opt_cknice = atoi(argv[i] + 9);
        else if (!strncmp(argv[i], "--lock=", 7)) td_opt_lock = atoi(argv[i] + 7);
        else if (!strncmp(argv[i], "--poolcap=", 10)) td_opt_poolcap = (uint32_t)atoi(argv[i] + 10);
        else if (!strncmp(argv[i], "--poolmax=", 10)) td_opt_poolmax = (uint32_t)atoi(argv[i] + 10);
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

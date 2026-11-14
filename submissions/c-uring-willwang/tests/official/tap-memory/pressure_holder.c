/* Disposable cgroup-only fixture. Requests on stdin exercise clean-cache reclaim
 * or private anonymous allocation. This binary is never part of a server entry. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

static void die(const char *s) {
    perror(s);
    exit(1);
}

int main(int argc, char **argv) {
    if (argc != 2)
        return 2;
    int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(19494)};
    if (listener < 0 || inet_pton(AF_INET, "198.18.203.1", &a.sin_addr) != 1 ||
        bind(listener, (void *)&a, sizeof(a)) < 0 || listen(listener, 4096) < 0)
        die("private listener");
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("READY %d\n", getpid());
    char line[32];
    while (fgets(line, sizeof(line), stdin)) {
        if (!strcmp(line, "cache\n")) {
            int fd = open(argv[1], O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
            if (fd < 0)
                die("private cache file");
            static const char data[16384] = {1};
            for (int i = 0; i < 4096; i++)
                if (write(fd, data, sizeof(data)) != sizeof(data))
                    die("cache write");
            if (fsync(fd) || close(fd))
                die("cache sync");
            puts("CACHED");
        } else if (!strcmp(line, "anon\n")) {
            puts("ANON");
            /* Availability is bounded by this test's 27 MiB no-swap leaf. Normal
             * faulting here exercises its lower modeled parent first. */
            for (int i = 0; i < 1024; i++) {
                size_t size = 65536;
                volatile unsigned char *p =
                    mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                if (p == MAP_FAILED)
                    die("private anonymous mapping");
                for (size_t j = 0; j < size; j += 4096)
                    p[j] = 1;
            }
            puts("REACHED_64_MIB");
            return 3;
        } else if (!strcmp(line, "quit\n"))
            break;
        else
            return 2;
    }
    close(listener);
    return 0;
}

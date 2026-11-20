/* Keep this private client namespace alive so its kernel can finish TCP close
 * after the stock Go child exits. Never participate in benchmark requests. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
static void fail(const char *name) {
    perror(name);
    exit(1);
}
int main(int argc, char **argv) {
    if (argc < 2)
        return 2;
    pid_t p = fork();
    if (p < 0)
        fail("fork scorer");
    if (!p) {
        FILE *log = freopen("/out/scorer.log", "w", stdout);
        if (!log || dup2(fileno(log), STDERR_FILENO) < 0)
            fail("capture stock scorer progress");
        execv(argv[1], argv + 1);
        fail("start unmodified scorer");
    }
    int status;
    while (waitpid(p, &status, 0) < 0)
        if (errno != EINTR)
            fail("wait scorer");
    FILE *result = fopen("/out/exit.pending", "w");
    if (!result)
        fail("open child status");
    fprintf(result, "%d\n", WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
    if (fclose(result) || rename("/out/exit.pending", "/out/exit"))
        fail("publish child status");
    while (access("/out/release", F_OK))
        usleep(100000);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

#define _GNU_SOURCE
#include <glob.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static void show(const char *p) {
    FILE *fp = fopen(p, "r");
    if (!fp) {
        perror(p);
        return;
    }
    char buf[256];
    if (fgets(buf, sizeof(buf), fp))
        printf("%s %s", p, buf);
    fclose(fp);
}
int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "wait")) {
        for (;;)
            pause();
    }
    show("/proc/sys/net/core/rps_default_mask");
    glob_t list = {0};
    if (!glob("/sys/class/net/*/queues/rx-*/rps_cpus", 0, NULL, &list)) {
        for (size_t i = 0; i < list.gl_pathc; ++i)
            show(list.gl_pathv[i]);
    }
    globfree(&list);
    return 0;
}

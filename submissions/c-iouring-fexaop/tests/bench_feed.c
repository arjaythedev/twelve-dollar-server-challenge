#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "db.h"
int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: bench_feed <db> [n] [sql]\n"); return 2; }
    int n = argc > 2 ? atoi(argv[2]) : 1000000;
    int sql = argc > 3 && atoi(argv[3]);
    char err[256];
    if (db_open(argv[1], err, sizeof err) != 0) { fprintf(stderr, "db: %s\n", err); return 1; }
    static char arena[32768];
    struct row rows[20];
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (int i = 0; i < n; i++) {
        int rc = sql ? db_feed_sql(rows, arena, sizeof arena) : db_feed(rows, arena, sizeof arena);
        if (rc != 0) { fprintf(stderr, "feed failed at %d\n", i); return 1; }
        if ((i & 7) == 7 && db_commit() != 0) { fprintf(stderr, "commit failed\n"); return 1; }
    }
    db_commit();
    clock_gettime(CLOCK_MONOTONIC, &b);
    double ns = (b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec);
    printf("n=%d sql=%d total=%.3fs per_feed=%.3fus rows=%lld/%d\n", n, sql, ns / 1e9, ns / n / 1000.0,
           rows[0].id, rows[0].likes);
    return 0;
}

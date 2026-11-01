/* The whole server is one translation unit: SQLite's amalgamation, the runtime, and the
 * application layer generated from the Lean specification (build/app_gen.c). */
#define _GNU_SOURCE
#include "sqlite3.c"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "rt.h"
#include "out.c"
#include "json.c"
#include "jwt.c"
#include "http.c"
#include "db.c"
#include "uring.c"
#include "app_gen.c"

int main(void) {
  const char *path = getenv("SQLITE_PATH"), *secret = getenv("JWT_SECRET");
  const char *host = getenv("HOST"), *port = getenv("PORT");
  if (!path || !secret) { fprintf(stderr, "SQLITE_PATH and JWT_SECRET are required\n"); return 1; }
  if (!host) host = "0.0.0.0";
  signal(SIGPIPE, SIG_IGN);
  hmac_init((const u8 *)secret, strlen(secret));
  db_open(path);
  app_init();
  app_check_inv();
  transport_init(host, port ? atoi(port) : 3000, 600000);
  fprintf(stderr, "listening on %s:%s\n", host, port ? port : "3000");
  transport_run();
  return 0;
}

/* One translation unit: unpatched SQLite amalgamation, then the server.
 * B-tree symbols stay visible because they are compiled into this file. */
#define _GNU_SOURCE
#include "sqlite3.c"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt.h"
#include "out.c"
#include "json.c"
#include "jwt.c"
#include "http.c"
#include "db.c"
#include "app.c"
#include "uring.c"

int main(void) {
  const char *path = getenv("SQLITE_PATH");
  const char *secret = getenv("JWT_SECRET");
  const char *host = getenv("HOST");
  const char *ports = getenv("PORT");
  if (!path || !secret) {
    fprintf(stderr, "SQLITE_PATH and JWT_SECRET are required\n");
    return 1;
  }
  if (!host || !*host) host = "0.0.0.0";
  int port = (ports && *ports) ? atoi(ports) : 3000;
  signal(SIGPIPE, SIG_IGN);
  out_module_init();
  jwt_fast_init(secret, strlen(secret));
  db_open(path);
  app_init();
  db_check_inv();
  transport_init(host, port);
  fprintf(stderr, "listening on %s:%d\n", host, port);
  transport_run();
  return 0;
}

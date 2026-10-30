#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rt.h"
#include "sqlite_format.h"
#include "raw_sqlite.h"
#include "out.c"
#include "json.c"
#include "jwt.c"
#include "http.c"
#include "raw_sqlite.c"
#include "app.c"
#include "uring.c"

static void on_sigterm(int sig) {
  (void)sig;
  raw_sqlite_close();
  _exit(0);
}

int main(void) {
  const char *path = getenv("SQLITE_PATH");
  const char *secret = getenv("JWT_SECRET");
  const char *host = getenv("HOST");
  const char *port = getenv("PORT");

  if (!path || !secret) {
    fprintf(stderr, "SQLITE_PATH and JWT_SECRET are required\n");
    return 1;
  }
  if (!host) host = "0.0.0.0";

  signal(SIGPIPE, SIG_IGN);
  signal(SIGTERM, on_sigterm);
  signal(SIGINT, on_sigterm);
  hmac_init((const u8 *)secret, strlen(secret));

  if (raw_sqlite_open(path) != 0) {
    fprintf(stderr, "failed to open database at %s\n", path);
    return 1;
  }
  app_init();

  const char *conns_env = getenv("CONNS");
  u32 want_conns = conns_env ? (u32)atoi(conns_env) : 500000;
  transport_init(host, port ? atoi(port) : 3000, want_conns);

  fprintf(stderr, "c-rawsqlite-larvance listening on %s:%s\n", host, port ? port : "3000");
  transport_run();
  raw_sqlite_close();
  return 0;
}

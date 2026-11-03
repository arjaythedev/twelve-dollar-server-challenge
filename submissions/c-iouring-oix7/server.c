#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/mman.h>
#endif

/* Single translation unit: SQLite amalgamation + runtime + application. */
#include "sqlite/sqlite3.c"
#include "runtime/rt.h"

i64 now_s;

#include "runtime/out.c"
#include "runtime/json.c"
#include "runtime/jwt.c"
#include "runtime/http.c"
#include "runtime/db.c"
#include "gen/app_gen.c"

#if defined(__linux__) && !defined(FORCE_POLL)
#include "runtime/uring.c"
#else
#include "runtime/poll.c"
#endif

int main(void) {
  signal(SIGPIPE, SIG_IGN);
#if defined(__linux__)
  /* Lock read-only code pages in RAM within RLIMIT_MEMLOCK so page reclaim never stalls the loop. */
  mlockall(MCL_CURRENT);
#endif
  now_s = (i64)time(0);

  const char *db_path = getenv("SQLITE_PATH");
  if (!db_path || !*db_path) db_path = "seed/feed.db";

  const char *secret = getenv("JWT_SECRET");
  if (!secret || !*secret) secret = "twelve-dollar-challenge";

  const char *host = getenv("HOST");
  if (!host || !*host) host = "0.0.0.0";

  const char *port_s = getenv("PORT");
  int port = 80;
  if (port_s && *port_s) {
    errno = 0;
    char *end;
    long v = strtol(port_s, &end, 10);
    if (errno != 0 || end == port_s || *end != '\0' || v < 1 || v > 65535) {
      fprintf(stderr, "fatal: PORT must be 1..65535 (got '%s')\n", port_s);
      exit(1);
    }
    port = (int)v;
  }

  hmac_init((const u8 *)secret, strlen(secret));
  db_open(db_path);
  app_init();
  app_check_inv();

  transport_init(host, port, 400000);
  transport_run();
  return 0;
}

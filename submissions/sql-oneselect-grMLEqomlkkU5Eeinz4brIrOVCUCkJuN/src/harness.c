/* Test harness, no network: feeds raw HTTP bytes from stdin through the same parser, virtual table and
 * server.sql as the real server, one batch at a time, and prints the raw responses to stdout.
 *
 *   SQLITE_PATH=<db> JWT_SECRET=<secret> bin/harness src/server.sql < requests.http */
#include "app.h"

int main(int argc, char **argv) {
  if (app_init(getenv("SQLITE_PATH"), argv[1], getenv("JWT_SECRET"))) return 1;
  conn c = {.fd = -1, .open = 1};
  c.in_cap = 1 << 20;
  c.in = malloc(c.in_cap);
  c.in_len = (uint32_t)fread(c.in, 1, c.in_cap, stdin);
  if (argc > 2) {  /* bin/harness server.sql batch: every request on its own connection, all in one batch */
    static conn cs[BATCH_MAX];
    int k = 0;
    for (; k < BATCH_MAX && c.in_len; k++) {
      cs[k] = c; cs[k].busy = 0; cs[k].out = 0; cs[k].out_len = cs[k].out_cap = 0;
      if (queue_request(&cs[k]) != 1) break;
      c.in = cs[k].in; c.in_len = cs[k].in_len;
    }
    run_batch();
    for (int i = 0; i < k; i++) fwrite(cs[i].out, 1, cs[i].out_len, stdout);
    return 0;
  }
  for (;;) {
    int r = queue_request(&c);
    if (r == 2) { run_batch(); c.busy = 0; continue; }
    if (r <= 0) break;
    run_batch();
    c.busy = 0;
  }
  fwrite(c.out, 1, c.out_len, stdout);
  return 0;
}

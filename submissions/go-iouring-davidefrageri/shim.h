// The C side of the server, seen from Go. One call per pass: td_batch runs every database
// request the pass read inside one SQLite transaction and writes each complete HTTP response
// (status line, headers, JSON body) into the buffer Go reserved for it.
#ifndef TD_SHIM_H
#define TD_SHIM_H
#include <stdint.h>

enum { TD_FEED = 1, TD_POST = 2, TD_LIKE = 3, TD_INSERT = 4, TD_HEALTH = 5 };

// status: the low bits say what happened; TD_SPILL says the response did not fit in `out` and
// was written to a buffer the shim owns (also at `out`, which it rewrote) that stays valid until
// the next td_batch call.
enum { TD_OK = 0, TD_MISSING = 1, TD_REPEAT = 2, TD_ERR = 3 };
#define TD_SPILL 0x80

typedef struct {
  uint8_t kind;
  uint8_t status;
  uint16_t resv;
  int32_t n;           // out: response bytes written at out
  int64_t id;          // in: post id (TD_POST, TD_LIKE), uptime seconds (TD_HEALTH); out: new id (TD_INSERT)
  int64_t uid;         // in: the token's user id (TD_LIKE, TD_INSERT)
  const uint8_t *body; // in: the trimmed post body (TD_INSERT)
  const uint8_t *name; // in: the token's username (TD_INSERT)
  int32_t nbody, nname;
  uint8_t *out;        // in: where the response goes; out: replaced when TD_SPILL
  int32_t cap;         // in: bytes available at out
  int32_t resv2;
} td_req_t;

// Opens the database (and the checkpoint connection). 0 ok, -1 with a message in err.
int td_open(const char *path, char *err, int n);
// Runs n requests in one transaction and commits. 0 committed (or nothing to commit); -1 the
// transaction was lost: every response of the batch that claims success must not be sent.
int td_batch(td_req_t *reqs, int n);
// Between passes, with no transaction open: drives the checkpoint state machine.
void td_tick(void);
// 1 while writes must wait for a checkpoint to finish.
int td_writes_held(void);
// Counters for the log line.
void td_stats(int64_t *out, int n);
// Frames in the WAL after the last commit.
int td_wal_frames(void);

#endif

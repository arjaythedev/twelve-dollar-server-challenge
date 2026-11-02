/* The C runtime, one translation unit: SQLite (pinned amalgamation), the storage primitives,
 * the byte leaves and the io_uring transport. Everything about the API is in Lean (Feed/). */
#include "sqlite3.c"
#include "rt.h"
#include "walvfs.c"
#include "db.c"
#include "leaves.c"
#include "uring.c"
#include "train.c"

/* Feed.Main.dbOpen */
LEAN_EXPORT lean_obj_res feed_db_open(b_lean_obj_arg path) {
  db_open(lean_string_cstr(path));
  return lean_io_result_mk_ok(lean_box(0));
}

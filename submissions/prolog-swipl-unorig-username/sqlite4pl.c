/* The SQLite binding: three predicates over the SQLite C API, loaded into swipl as a foreign library.
 * It knows nothing about posts, HTTP or JSON; server.pl decides every query and every response.
 *
 *   sqlite_open(+Path)                       open the one connection of this process
 *   sqlite_prepare(+Sql, -Stmt)              compile a statement once, reuse it for every request
 *   sqlite_run(+Stmt, +Params, -Result)      bind Params (integers, text), step once, reset:
 *                                              row(Text)   the first column of the first row
 *                                              done(N)     no row; N rows changed by this statement
 *                                              error(Msg)  SQLite's message
 *
 * Text goes in as Unicode (bound as UTF-8) and comes out as the raw UTF-8 bytes, one code per byte,
 * because server.pl writes it to an octet stream and needs its length in bytes. */
#include <SWI-Prolog.h>
#include <sqlite3.h>
#include <stdint.h>

static sqlite3 *db; /* one connection per process: the server is one thread */

static int error(const char *what) { /* raises sqlite_error(What, Message) */
  term_t ex = PL_new_term_ref();
  return PL_unify_term(ex, PL_FUNCTOR_CHARS, "sqlite_error", 2, PL_CHARS, what,
                       PL_UTF8_STRING, db ? sqlite3_errmsg(db) : "out of memory") && PL_raise_exception(ex);
}

static foreign_t pl_open(term_t path) {
  char *p;
  if (!PL_get_chars(path, &p, CVT_ATOM | CVT_STRING | REP_UTF8 | CVT_EXCEPTION)) return FALSE;
  if (sqlite3_open_v2(p, &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) return error("sqlite_open");
  return TRUE;
}

static foreign_t pl_prepare(term_t sql, term_t stmt) {
  char *s;
  sqlite3_stmt *st;
  if (!PL_get_chars(sql, &s, CVT_ATOM | CVT_STRING | REP_UTF8 | CVT_EXCEPTION)) return FALSE;
  if (sqlite3_prepare_v3(db, s, -1, SQLITE_PREPARE_PERSISTENT, &st, NULL) != SQLITE_OK) return error("sqlite_prepare");
  return PL_unify_int64(stmt, (int64_t)(intptr_t)st);
}

static foreign_t pl_run(term_t stmt, term_t params, term_t result) {
  int64_t h, i64;
  if (!PL_get_int64_ex(stmt, &h)) return FALSE;
  sqlite3_stmt *st = (sqlite3_stmt *)(intptr_t)h;
  term_t head = PL_new_term_ref(), list = PL_copy_term_ref(params);
  for (int i = 1; PL_get_list(list, head, list); i++) {
    char *s;
    size_t len;
    if (PL_get_int64(head, &i64)) sqlite3_bind_int64(st, i, i64);
    else if (PL_get_nchars(head, &len, &s, CVT_ATOM | CVT_STRING | REP_UTF8 | CVT_EXCEPTION))
      sqlite3_bind_text(st, i, s, (int)len, SQLITE_TRANSIENT);
    else return FALSE;
  }
  int rc = sqlite3_step(st), ok;
  if (rc == SQLITE_ROW) {
    const char *t = (const char *)sqlite3_column_text(st, 0);
    term_t text = PL_new_term_ref();
    ok = PL_put_chars(text, PL_STRING | REP_ISO_LATIN_1, (size_t)sqlite3_column_bytes(st, 0), t ? t : "") &&
         PL_unify_term(result, PL_FUNCTOR_CHARS, "row", 1, PL_TERM, text);
  } else if (rc == SQLITE_DONE) {
    ok = PL_unify_term(result, PL_FUNCTOR_CHARS, "done", 1, PL_INT64, (int64_t)sqlite3_changes(db));
  } else {
    ok = PL_unify_term(result, PL_FUNCTOR_CHARS, "error", 1, PL_UTF8_STRING, sqlite3_errmsg(db));
  }
  sqlite3_reset(st);
  return ok;
}

install_t install_sqlite4pl(void) {
  PL_register_foreign("sqlite_open", 1, pl_open, 0);
  PL_register_foreign("sqlite_prepare", 2, pl_prepare, 0);
  PL_register_foreign("sqlite_run", 3, pl_run, 0);
}

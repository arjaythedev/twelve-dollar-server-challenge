// import <sqlite_path> <store_dir>: converts feed.db into the store's first snapshot.
//
// This is the only program that reads SQLite. If <store_dir> already holds a snapshot imported from
// this same file (same device, inode, size and mtime), it does nothing, so a restart keeps the
// snapshot and WAL. Otherwise it empties <store_dir> and imports.
#include <dirent.h>
#include <sys/stat.h>

#include <cerrno>

#include "sqlite3.h"
#include "store.hpp"

using namespace ms;

namespace {

void wipe(const std::string& dir) {
  DIR* d = opendir(dir.c_str());
  if (!d) return;
  while (dirent* e = readdir(d)) {
    std::string_view name = e->d_name;
    if (name.substr(0, 8) == "snapshot" || name.substr(0, 4) == "wal.") unlink((dir + "/" + e->d_name).c_str());
  }
  closedir(d);
}

sqlite3_stmt* prepare(sqlite3* db, const char* sql) {
  sqlite3_stmt* st;
  if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) die(sql, sqlite3_errmsg(db));
  return st;
}

const char* text(sqlite3_stmt* st, int col) { return reinterpret_cast<const char*>(sqlite3_column_text(st, col)); }

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) die("usage: import <sqlite_path> <store_dir>");
  const char* db_path = argv[1];
  std::string dir = argv[2];
  SrcId src = src_id(db_path);

  SnapHeader h;
  if (read_snapshot_header(dir, &h) && same_src(h.src, src)) {
    std::fprintf(stderr, "import: %s is up to date\n", dir.c_str());
    return 0;
  }
  if (mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) die("cannot create", dir.c_str());
  wipe(dir);

  sqlite3* db;
  if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
    die("cannot open database", sqlite3_errmsg(db));

  Store s;
  sqlite3_stmt* st = prepare(db, "SELECT id, username FROM users");
  while (sqlite3_step(st) == SQLITE_ROW)
    s.add_user(sqlite3_column_int64(st, 0), std::string_view(text(st, 1), sqlite3_column_bytes(st, 1)));
  sqlite3_finalize(st);

  st = prepare(db, "SELECT id, user_id, body, created_at FROM posts");
  while (sqlite3_step(st) == SQLITE_ROW) {
    if (sqlite3_column_bytes(st, 3) != static_cast<int>(kTsLen)) die("unexpected created_at", text(st, 3));
    int64_t id = sqlite3_column_int64(st, 0);
    s.add_post(id, sqlite3_column_int64(st, 1), text(st, 3),
               std::string_view(text(st, 2), sqlite3_column_bytes(st, 2)), false);
    s.order.push_back(static_cast<uint32_t>(id));
  }
  sqlite3_finalize(st);
  s.sort_order();

  st = prepare(db, "SELECT count(*) FROM likes");
  sqlite3_step(st);
  s.likes.reserve(sqlite3_column_int64(st, 0));
  sqlite3_finalize(st);
  st = prepare(db, "SELECT user_id, post_id FROM likes");
  while (sqlite3_step(st) == SQLITE_ROW) {
    int64_t post = sqlite3_column_int64(st, 1);
    if (s.has_post(post)) s.like(sqlite3_column_int64(st, 0), post);  // a like of a missing post is never served
  }
  sqlite3_finalize(st);
  sqlite3_close(db);

  if (!write_snapshot(s, dir, 1, src)) die("cannot write snapshot in", dir.c_str());
  std::fprintf(stderr, "import: %zu users, %zu posts, %zu likes -> %s/snapshot\n",
               s.user_exists.size() ? static_cast<size_t>(std::count(s.user_exists.begin(), s.user_exists.end(), 1)) : 0,
               s.order.size(), s.likes.size(), dir.c_str());
  return 0;
}

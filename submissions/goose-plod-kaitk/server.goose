// The root file: Plod and its packages at their tags, the configuration from
// SPEC.md's environment, and one worker that serves app.goose's routes.
import std;
import os;
import http from "github.com/kaitk/goose-http@v0.5.16";
import hash from "github.com/kaitk/goose-hash@v0.0.4";
import plod from "github.com/kaitk/plod@v0.5.11";
import sqlite from "../../../goose-sqlite-query";
import app;

thread_fn worker(cfg: http::Config) {
    var path: u8[>..] = [];
    var secret: u8[>..] = [];
    guard env("SQLITE_PATH", path) else { abort("SQLITE_PATH is not set"); }
    guard env("JWT_SECRET", secret) else { abort("JWT_SECRET is not set"); }
    let keys = [plod::JwtKey { secret: secret }];
    var db = sqlite::Conn { db: sqlite::Db {}, .. };
    let failed = sqlite::run() {
        db.db = sqlite::open(path, sqlite::READ_WRITE | sqlite::NO_MUTEX);
        // WAL, as the seed is; NORMAL syncs at checkpoints, not at each
        // commit, which SPEC.md's rule 6 allows. Another process's writer
        // holds the lock for one statement, so a wait is short.
        sqlite::exec(db.db, """
            PRAGMA journal_mode = WAL;
            PRAGMA synchronous = NORMAL;
            PRAGMA cache_size = -16000;
            PRAGMA wal_autocheckpoint = 4000;
            """);
        sqlite::busy_timeout(db.db, 5000);
    };
    for e in failed { abort(str("SQLite: ", e.message)); }
    var a = App { db: db, keys: keys, started_ns: time_ns() };
    let e = plod::serve(cfg, a, St {}) { cx => handle(cx); };
    sqlite::close(db);
    if e.code != 0 { http::report(e); }
}

fn main() {
    http::exit_on_error(http::stop_on_signals());
    var host: u8[>..] = [];
    var port: u8[>..] = [];
    var cfg = http::Config { workers: 1, max_conns: 64000, idle_ms: 120000, server: "", .. };
    if env("HOST", host) && host != "0.0.0.0" { format(cfg.address, host); }
    if env("PORT", port) {
        let p, ok = parse_int(trim(port));
        guard ok else { abort("PORT: not a port"); }
        cfg.port = p as i32;
    }
    http::run(cfg, worker);
}

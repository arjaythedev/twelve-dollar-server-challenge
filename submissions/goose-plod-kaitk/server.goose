// The root file: Plod and its packages at their tags, the configuration from
// SPEC.md's environment, and one worker that serves app.goose's routes.
import std;
import os;
import http from "github.com/kaitk/goose-http@direct-descriptors";
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
        // commit, which SPEC.md's rule 6 allows. The one process holds the
        // database alone, so EXCLUSIVE takes its lock once, and keeps the
        // WAL's index in memory rather than in a shared -shm file. A
        // checkpoint every 16,000 pages copies a page written many times
        // once.
        sqlite::exec(db.db, """
            PRAGMA locking_mode = EXCLUSIVE;
            PRAGMA journal_mode = WAL;
            PRAGMA synchronous = NORMAL;
            PRAGMA cache_size = -16000;
            PRAGMA wal_autocheckpoint = 16000;
            """);
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
    // One worker on the box's one vCPU. Every client holds a connection, and
    // the box gives a process 65,535 descriptors, so connections are direct
    // descriptors, in as many io_uring rings as they need. A wait holds up
    // to 1 ms for more completions, so the worker wakes once for many.
    var cfg = http::Config { workers: 1, max_conns: 1000000, idle_ms: 120000, server: "",
                             ring_direct: true, ring_batch_us: 1000, .. };
    if env("HOST", host) && host != "0.0.0.0" { format(cfg.address, host); }
    if env("PORT", port) {
        let p, ok = parse_int(trim(port));
        guard ok else { abort("PORT: not a port"); }
        cfg.port = p as i32;
    }
    http::run(cfg, worker);
}

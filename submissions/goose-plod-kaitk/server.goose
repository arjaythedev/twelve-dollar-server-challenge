// The root file: Plod and its packages at their tags, the configuration from
// SPEC.md's environment, one worker that serves app.goose's routes, and a lane
// that checkpoints the WAL beside it.
import std;
import os;
import http from "github.com/kaitk/goose-http@v0.5.19";
import hash from "github.com/kaitk/goose-hash@v0.0.4";
import plod from "github.com/kaitk/plod@v0.5.12";
import sqlite from "github.com/kaitk/goose-sqlite@v0.0.4";
import app;

// SQLITE_OPEN_URI, which goose-sqlite passes through: the path is a URI, so
// that it can name the VFS.
let OPEN_URI: i32 = 64;

// SQLITE_PATH as a URI on the unix-excl VFS: the process locks the file once
// and keeps the lock, and its two connections share the WAL's index in
// memory, with no -shm file and no file locks per transaction.
fn database() -> u8[>..] {
    var path: u8[>..] = [];
    guard env("SQLITE_PATH", path) else { abort("SQLITE_PATH is not set"); }
    var uri: u8[>..] = [];
    uri.append("file:");
    for c in path {
        if c == '%' { uri.append("%25"); }
        else if c == '?' { uri.append("%3f"); }
        else if c == '#' { uri.append("%23"); }
        else { uri.push(c); }
    }
    uri.append("?vfs=unix-excl");
    return uri;
}

// The checkpoint lane's thread. Each job copies the WAL back into the
// database with a PASSIVE checkpoint, which never holds the worker's writes,
// and waits out its fsyncs here, not on the worker. Writes keep arriving
// while it runs, so it never quite catches up; the worker's own checkpoint
// does that, copying only what was written since.
thread_fn checkpointer(box: http::Box) {
    var db = sqlite::Db {};
    let failed = sqlite::run() {
        db = sqlite::open(database(), sqlite::READ_WRITE | sqlite::NO_MUTEX | OPEN_URI);
        // It reads no tables: a checkpoint copies frames from the WAL.
        sqlite::exec(db, """
            PRAGMA synchronous = NORMAL;
            PRAGMA wal_autocheckpoint = 0;
            PRAGMA cache_size = 16;
            """);
        sqlite::busy_timeout(db, 1000);
    };
    for e in failed { abort(str("SQLite: ", e.message)); }
    http::lane(box) { tag, bytes =>
        let ran = sqlite::run() {
            sqlite::exec(db, "PRAGMA wal_checkpoint(PASSIVE)");
        };
        for e in ran { write_stderr(str("SQLite checkpoint: ", e.message, "\n")); }
    };
    sqlite::close(db);
}

thread_fn worker(cfg: http::Config, checkpoints: http::Lane) {
    var secret: u8[>..] = [];
    guard env("JWT_SECRET", secret) else { abort("JWT_SECRET is not set"); }
    let keys = [plod::JwtKey { secret: secret }];
    var db = sqlite::Conn { db: sqlite::Db {}, .. };
    let failed = sqlite::run() {
        // Without foreign keys: a post's author and a like's user are the
        // token's, which SPEC.md says isn't looked up, and the like's post
        // is checked in its INSERT, so enforcement would only add seeks.
        db.db = sqlite::open(database(), sqlite::READ_WRITE | sqlite::NO_MUTEX | OPEN_URI, false);
        // WAL, as the seed is; NORMAL syncs at checkpoints, not at each
        // commit, which SPEC.md's rule 6 allows. The lane copies most of the
        // WAL back; at 16,000 pages this connection checkpoints the rest
        // itself, which lets the next write start the WAL from its beginning.
        sqlite::exec(db.db, """
            PRAGMA journal_mode = WAL;
            PRAGMA synchronous = NORMAL;
            PRAGMA cache_size = -16000;
            PRAGMA wal_autocheckpoint = 16000;
            """);
        sqlite::busy_timeout(db.db, 5000);
    };
    for e in failed { abort(str("SQLite: ", e.message)); }
    var a = App { db: db, keys: keys, started_ns: time_ns(), checkpoints: checkpoints };
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
    // to 1 ms for more completions, so the worker wakes once for many. Each
    // connection with data in a pass takes a receive buffer: past the ring's
    // buffers, the rest end their receives and are queued again, pass after
    // pass, so there are 1,024 buffers rather than goose-http's 64.
    var cfg = http::Config { workers: 1, max_conns: 1000000, idle_ms: 120000, server: "",
                             ring_direct: true, ring_batch_us: 1000, ring_buffers: 1024, .. };
    if env("HOST", host) && host != "0.0.0.0" { format(cfg.address, host); }
    if env("PORT", port) {
        let p, ok = parse_int(trim(port));
        guard ok else { abort("PORT: not a port"); }
        cfg.port = p as i32;
    }
    // Two slots: one checkpoint running and one asked for; an ask past them
    // is dropped, as the one queued will copy its frames.
    let checkpoints = http::lane_open(http::LaneConfig { slots: 2, .. }, checkpointer);
    http::run(cfg, checkpoints, worker);
    http::lane_stop(checkpoints);
}

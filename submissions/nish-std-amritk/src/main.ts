// The twelve-dollar server challenge's feed API
// (https://github.com/arjaythedev/twelve-dollar-server-challenge), in Nish on
// its standard library: nish/net/http1-server, nish/sqlite, nish/json-tokenizer
// and nish/crypto's HMAC. README.md has the design and the measurements.
//
// Configured by SQLITE_PATH, JWT_SECRET, HOST and PORT (SPEC.md). A
// supervisor (nish/net/prefork, restarts off) starts WORKERS serving
// processes, which share the port with SO_REUSEPORT, and one checkpointer.
// Each serving process holds at most SLOTS connections (60,000), under the
// 65,535 descriptors a process gets: serving direct, every user holds a
// keep-alive connection, so 300,000 users need five. README.md lists the
// other optional variables.
import { pollCreate, pollWait, tcpListenFlags } from "nish:net"
import { getenv } from "nish:process"
import { PREFORK_STOPPED, PreforkConfig, preforkWith } from "nish/net/prefork"
import { Database, OPEN_READWRITE, openDatabase } from "nish/sqlite"
import { FeedServer, ServerConfig } from "./server"

/** `tcpListenFlags`: SO_REUSEPORT, and TCP_NODELAY on every accepted socket. */
const LISTEN_FLAGS: i32 = 3

/** The environment variable `name` as a positive number, or `fallback`. */
const envInt = (name: string, fallback: i32): i32 => {
  const v: string | null = getenv(name)
  if (v === null || v.length === 0 || v.length > 9) {
    return fallback
  }
  for (let k: i32 = 0; k < toI32(v.length); k++) {
    const c: i32 = toI32(v.charCodeAt(k))
    if (c < 48 || c > 57) {
      return fallback
    }
  }
  const n: i32 = parseInt(v)
  return n > 0 ? n : fallback
}

/** Opens the database and applies `pragmas`; null, having said why, when it cannot. */
const open = (path: string, pragmas: string[]): Database | null => {
  const opened = openDatabase(path, OPEN_READWRITE)
  if (opened.isErr()) {
    console.log(`feed: cannot open ${path}: ${opened.error.message}`)
    return null
  }
  const db = opened.value
  for (let k: i32 = 0; k < toI32(pragmas.length); k++) {
    const r = db.exec(pragmas[k])
    if (r.isErr()) {
      console.log(`feed: ${pragmas[k]}: ${db.error(r.error).message}`)
      return null
    }
  }
  return db
}

/**
 * The checkpointer, so that no serving process runs a checkpoint, and its
 * fsync, inline. Every `everyMs` a PASSIVE checkpoint copies what the WAL
 * holds back into the database file without waiting for anyone. Under
 * steady writes that never copies everything (frames land while it runs),
 * so the log would never start again from its beginning; once it holds
 * `restartFrames`, a RESTART checkpoint holds new writers off (they find the
 * lock busy and retry on their next pass, server.ts `commitWrites`), copies
 * the rest and syncs, and the next write starts the log again.
 */
const checkpointer = (path: string, everyMs: i32, restartFrames: i64): i32 => {
  const db = open(path, ["PRAGMA busy_timeout=250"])
  if (db === null) {
    return 1
  }
  const tick: i32 = pollCreate()
  const ready: i32[] = new Array<i32>(2)
  const passive = db.prepare("PRAGMA wal_checkpoint(PASSIVE)").expect("checkpoint")
  const restart = db.prepare("PRAGMA wal_checkpoint(RESTART)").expect("checkpoint")
  while (true) {
    pollWait(tick, ready, everyMs)
    // One row: (busy, frames in the log, frames checkpointed).
    let frames: i64 = 0
    const r = passive.step()
    if (r.isOk() && r.value) {
      frames = passive.columnI64(1)
    }
    const p = passive.reset()
    if (p.isOk() && frames >= restartFrames) {
      const q = restart.step()
      const done = restart.reset()
      if (q.isErr() || done.isErr()) {
        // Busy past the timeout: the next tick tries again.
        continue
      }
    }
  }
}

export const main = (): i32 => {
  const cfg = new ServerConfig()
  cfg.sqlitePath = getenv("SQLITE_PATH") ?? ""
  cfg.jwtSecret = getenv("JWT_SECRET") ?? ""
  if (cfg.sqlitePath.length === 0 || cfg.jwtSecret.length === 0) {
    console.log("feed: SQLITE_PATH and JWT_SECRET must be set")
    return 2
  }
  cfg.host = getenv("HOST") ?? "0.0.0.0"
  cfg.port = envInt("PORT", 3000)
  cfg.slots = envInt("SLOTS", 60000)
  cfg.idleTimeoutMs = envInt("IDLE_TIMEOUT_MS", 600000)
  cfg.cacheKib = envInt("CACHE_KIB", 16384)
  const mmapMib: i32 = envInt("MMAP_MIB", 0)
  const workers: i32 = envInt("WORKERS", 5)
  const prefork = new PreforkConfig()
  prefork.workers = workers + 1
  prefork.restart = false
  const index: i32 = preforkWith(process.argv, prefork)
  if (index < 0) {
    return index === PREFORK_STOPPED ? 0 : 1
  }
  if (index === workers) {
    return checkpointer(cfg.sqlitePath, envInt("CHECKPOINT_MS", 50), toI64(envInt("RESTART_FRAMES", 2048)))
  }
  // WAL with synchronous=NORMAL: a commit is in the log before it is
  // answered (rule 6). busy_timeout=0: a group that finds the write lock
  // taken waits for the next pass instead of sleeping in SQLite.
  const db = open(cfg.sqlitePath, [
    "PRAGMA journal_mode=WAL",
    "PRAGMA synchronous=NORMAL",
    "PRAGMA busy_timeout=0",
    `PRAGMA cache_size=-${cfg.cacheKib}`,
    "PRAGMA temp_store=MEMORY",
    `PRAGMA mmap_size=${toI64(mmapMib) * 1048576}`,
    `PRAGMA wal_autocheckpoint=${envInt("AUTOCHECKPOINT", 16384)}`,
  ])
  if (db === null) {
    return 1
  }
  const listener: i32 = tcpListenFlags(cfg.host, cfg.port, 4096, LISTEN_FLAGS)
  if (listener < 0) {
    console.log(`feed: cannot listen on ${cfg.host}:${cfg.port} (${listener})`)
    return 1
  }
  if (index === 0) {
    console.log(`feed: ${workers} workers on ${cfg.host}:${cfg.port}`)
  }
  return new FeedServer(cfg, listener, db).run()
}

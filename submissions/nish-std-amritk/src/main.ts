// The twelve-dollar server challenge's feed API
// (https://github.com/arjaythedev/twelve-dollar-server-challenge), in Nish on
// its standard library: nish/net/http1-server, nish/sqlite, nish/json-tokenizer
// and nish/crypto's HMAC. README.md has the design and the measurements.
//
// Configured by SQLITE_PATH, JWT_SECRET, HOST and PORT (SPEC.md). A
// supervisor (nish/net/prefork, restarts off) starts the serving processes,
// which share the port with SO_REUSEPORT, and one checkpointer. Serving
// direct, every user holds a keep-alive connection. Where the kernel gives
// io_uring rings, connections live in ring slots rather than descriptors, so
// one serving process holds them all, past the 65,535 descriptors a process
// gets. Where it does not, five processes on descriptors hold SLOTS (60,000)
// each. README.md lists the other optional variables.
import { NET_LISTEN_RING, netRingState, pollCreate, pollWait, tcpListenFlags } from "nish:net"
import { getenv } from "nish:process"
import { PREFORK_STOPPED, PreforkConfig, preforkWith } from "nish/net/prefork"
import { Database, OPEN_READWRITE, openDatabase } from "nish/sqlite"
import { FeedServer, ServerConfig } from "./server"

/** `tcpListenFlags`: SO_REUSEPORT, and TCP_NODELAY on every accepted socket. */
const LISTEN_FLAGS: i32 = 3

/** The argument that makes the program the ring probe instead of the server. */
const PROBE_ARG: string = "--feed-ring-probe"

/**
 * The ring probe, run as a child of the supervisor before any worker starts:
 * opens a ring listener on an ephemeral loopback port and exits 0 when it
 * accepts into rings, 1 when the kernel refused them (Docker's default
 * seccomp profile, a kernel without io_uring) and it would serve descriptors.
 * A child, so that the supervisor, whose workers fork from it, never makes a
 * ring of its own.
 */
const probeRings = (): i32 => {
  const fd: i32 = tcpListenFlags("127.0.0.1", 0, 1, NET_LISTEN_RING)
  const state: i32 = netRingState()
  if (fd < 0 || state !== 1) {
    console.log(`feed: no io_uring rings (${fd < 0 ? fd : state}), serving descriptors`)
    return 1
  }
  return 0
}

/** Whether a probe child got rings; false when it could not be run. */
const ringsWork = (): boolean => spawnSync([process.argv[0], PROBE_ARG]) === 0

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
  if (toI32(process.argv.length) === 2 && process.argv[1] === PROBE_ARG) {
    return probeRings()
  }
  const cfg = new ServerConfig()
  cfg.sqlitePath = getenv("SQLITE_PATH") ?? ""
  cfg.jwtSecret = getenv("JWT_SECRET") ?? ""
  if (cfg.sqlitePath.length === 0 || cfg.jwtSecret.length === 0) {
    console.log("feed: SQLITE_PATH and JWT_SECRET must be set")
    return 2
  }
  cfg.host = getenv("HOST") ?? "0.0.0.0"
  cfg.port = envInt("PORT", 3000)
  cfg.idleTimeoutMs = envInt("IDLE_TIMEOUT_MS", 600000)
  cfg.cacheKib = envInt("CACHE_KIB", 16384)
  cfg.cursors = (getenv("CURSORS") ?? "1") !== "0"
  const mmapMib: i32 = envInt("MMAP_MIB", 0)
  // RING=0 keeps the descriptors. Otherwise a child probes for rings; the
  // workers fork after it, so every one listens the same way.
  const rings: boolean = (getenv("RING") ?? "1") !== "0" && ringsWork()
  const workers: i32 = envInt("WORKERS", rings ? 1 : 5)
  cfg.slots = envInt("SLOTS", rings ? 400000 : 60000)
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
  const listener: i32 = tcpListenFlags(cfg.host, cfg.port, 4096, rings ? LISTEN_FLAGS | NET_LISTEN_RING : LISTEN_FLAGS)
  if (listener < 0) {
    console.log(`feed: cannot listen on ${cfg.host}:${cfg.port} (${listener})`)
    return 1
  }
  if (index === 0) {
    const kind: string = rings && netRingState() === 1 ? "io_uring slots" : "descriptors"
    console.log(`feed: ${workers} workers on ${cfg.host}:${cfg.port}, ${cfg.slots} connections each in ${kind}`)
  }
  return new FeedServer(cfg, listener, db).run()
}

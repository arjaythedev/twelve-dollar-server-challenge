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
import { NET_LISTEN_RING, NET_LISTEN_RING_WIDE, netRingState, pollCreate, pollWait, tcpListenFlags } from "nish:net"
import { getenv, monotonicNanos } from "nish:process"
import { PREFORK_STOPPED, PreforkConfig, preforkWith } from "nish/net/prefork"
import { Database, OPEN_READWRITE, openDatabase } from "nish/sqlite"
import { FeedServer, ServerConfig } from "./server"

/** `tcpListenFlags`: SO_REUSEPORT, and TCP_NODELAY on every accepted socket. */
const LISTEN_FLAGS: i32 = 3

/** The argument that makes the program the ring probe instead of the server. */
const PROBE_ARG: string = "--feed-ring-probe"

/** libc's setpriority(2): `which` 0 is PRIO_PROCESS, `who` 0 this process. */
declare function setpriority(which: i32, who: u32, prio: i32): i32
/** libc's openat(2) and fsync(2), for the checkpointer's flush (-100 is AT_FDCWD). */
declare function openat(dirfd: i32, path: string, flags: i32): i32
declare function fsync(fd: i32): i32

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
 * the rest and syncs, and the next write starts the log again. The database
 * file is synced just before each RESTART, so the RESTART holds the lock only
 * to copy and sync its last frames.
 */
const checkpointer = (path: string, everyMs: i32, restartFrames: i64, nice: i32, stats: boolean): i32 => {
  // On one CPU the checkpointer would take the server's core for its whole
  // copy; at a low priority it copies in the time the server leaves idle.
  if (nice > 0) {
    setpriority(0, toU32(0), nice)
  }
  // busy_timeout=0: a RESTART the server's read transaction blocks gives up
  // at once, letting go of the write lock, and is tried again a millisecond
  // later; SQLite's busy handler would sleep 1, 2, 5, 10, ... ms holding it.
  const db = open(path, ["PRAGMA busy_timeout=0"])
  if (db === null) {
    return 1
  }
  const tick: i32 = pollCreate()
  const ready: i32[] = new Array<i32>(2)
  // The database file, read-only, only to flush it: SQLite syncs it only at a
  // checkpoint that copied the whole log, which under steady writes is the
  // RESTART, so every page the PASSIVE copies since would be written out
  // while RESTART holds the write lock (measured: 70-125 ms a RESTART).
  // Flushed here first, the RESTART has only its last frames to sync.
  const dbFd: i32 = openat(-100, path, 0)
  // A fresh copy of the database is dirty in the page cache; written out now,
  // before load, rather than by the first RESTART's flush.
  if (dbFd >= 0) {
    fsync(dbFd)
  }
  const passive = db.prepare("PRAGMA wal_checkpoint(PASSIVE)").expect("checkpoint")
  const restart = db.prepare("PRAGMA wal_checkpoint(RESTART)").expect("checkpoint")
  let nP: i64 = 0
  let pNs: i64 = 0
  let pMax: i64 = 0
  let nR: i64 = 0
  let rNs: i64 = 0
  let rMax: i64 = 0
  let rFail: i64 = 0
  let maxFrames: i64 = 0
  let at: i64 = monotonicNanos()
  while (true) {
    pollWait(tick, ready, everyMs)
    const t0: i64 = monotonicNanos()
    // One row: (busy, frames in the log, frames checkpointed).
    let frames: i64 = 0
    const r = passive.step()
    if (r.isOk() && r.value) {
      frames = passive.columnI64(1)
    }
    const p = passive.reset()
    const t1: i64 = monotonicNanos()
    nP++
    pNs += t1 - t0
    if (t1 - t0 > pMax) pMax = t1 - t0
    if (frames > maxFrames) maxFrames = frames
    if (stats && t1 - at >= toI64(10000) * toI64(1000000)) {
      const ms: i64 = toI64(1000000)
      console.log(`ckpt passive=${nP} ${pNs / ms}ms max=${pMax / ms}ms restart=${nR} ${rNs / ms}ms max=${rMax / ms}ms fail=${rFail} maxFrames=${maxFrames}`)
      nP = 0
      pNs = 0
      pMax = 0
      nR = 0
      rNs = 0
      rMax = 0
      rFail = 0
      maxFrames = 0
      at = t1
    }
    if (p.isOk() && frames >= restartFrames) {
      if (dbFd >= 0) {
        fsync(dbFd)
      }
      // One row: (1 when readers or a writer kept it from finishing, ...).
      let tries: i32 = 0
      let finished: boolean = false
      while (!finished && tries < 200) {
        const q = restart.step()
        finished = q.isOk() && q.value && restart.columnI64(0) === toI64(0)
        const rr = restart.reset()
        finished = finished && rr.isOk()
        tries++
        if (!finished) {
          pollWait(tick, ready, 1)
        }
      }
      const t2: i64 = monotonicNanos()
      nR++
      rNs += t2 - t1
      if (t2 - t1 > rMax) rMax = t2 - t1
      if (!finished) {
        rFail++
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
  cfg.cursors = (getenv("CURSORS") ?? "1") !== "0"
  cfg.stats = (getenv("STATS") ?? "0") === "1"
  cfg.batchMin = envInt("BATCH_MIN", 64)
  cfg.batchMs = (getenv("BATCH_MS") ?? "1") === "0" ? 0 : envInt("BATCH_MS", 1)
  const mmapMib: i32 = envInt("MMAP_MIB", 0)
  // RING=0 keeps the descriptors. Otherwise a child probes for rings; the
  // workers fork after it, so every one listens the same way.
  const rings: boolean = (getenv("RING") ?? "1") !== "0" && ringsWork()
  const workers: i32 = envInt("WORKERS", rings ? 1 : 5)
  cfg.slots = envInt("SLOTS", rings ? 400000 : 60000)
  // One process on rings keeps a 64 MiB page cache, which measured 2.4% less
  // CPU a request than 16 MiB; five on descriptors keep 16 MiB each.
  cfg.cacheKib = envInt("CACHE_KIB", rings ? 65536 : 16384)
  const prefork = new PreforkConfig()
  prefork.workers = workers + 1
  prefork.restart = false
  const index: i32 = preforkWith(process.argv, prefork)
  if (index < 0) {
    return index === PREFORK_STOPPED ? 0 : 1
  }
  if (index === workers) {
    return checkpointer(cfg.sqlitePath, envInt("CHECKPOINT_MS", 50), toI64(envInt("RESTART_FRAMES", 2048)), envInt("CHECKPOINT_NICE", 0), (getenv("STATS") ?? "0") === "1")
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
  const listener: i32 = tcpListenFlags(cfg.host, cfg.port, 4096, rings ? LISTEN_FLAGS | NET_LISTEN_RING | NET_LISTEN_RING_WIDE : LISTEN_FLAGS)
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

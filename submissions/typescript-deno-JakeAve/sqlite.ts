// Minimal FFI binding to the SQLite that build.sh compiles into bin/: only the calls server.ts makes.
// node:sqlite can't load a custom build, and this one is THREADSAFE=2, MEMSTATUS=0, -O3 -march=native.
const ROW = 100, DONE = 101;
const TRANSIENT = Deno.UnsafePointer.create(0xffffffffffffffffn); // SQLITE_TRANSIENT: SQLite copies the text

const lib = Deno.dlopen(
  new URL(
    `./bin/libsqlite3.${Deno.build.os === "darwin" ? "dylib" : "so"}`,
    import.meta.url,
  ),
  {
    sqlite3_open_v2: {
      parameters: ["buffer", "buffer", "i32", "pointer"],
      result: "i32",
    },
    sqlite3_exec: {
      parameters: ["pointer", "buffer", "pointer", "pointer", "pointer"],
      result: "i32",
    },
    sqlite3_prepare_v3: {
      parameters: ["pointer", "buffer", "i32", "u32", "buffer", "pointer"],
      result: "i32",
    },
    sqlite3_bind_int: { parameters: ["pointer", "i32", "i32"], result: "i32" },
    sqlite3_bind_text: {
      parameters: ["pointer", "i32", "buffer", "i32", "pointer"],
      result: "i32",
    },
    sqlite3_step: { parameters: ["pointer"], result: "i32" },
    sqlite3_reset: { parameters: ["pointer"], result: "i32" },
    sqlite3_column_blob: { parameters: ["pointer", "i32"], result: "pointer" },
    sqlite3_column_bytes: { parameters: ["pointer", "i32"], result: "i32" },
    sqlite3_column_int: { parameters: ["pointer", "i32"], result: "i32" },
    sqlite3_column_text: { parameters: ["pointer", "i32"], result: "pointer" },
    sqlite3_changes: { parameters: ["pointer"], result: "i32" },
    sqlite3_errmsg: { parameters: ["pointer"], result: "pointer" },
    // render.c, compiled into the same library: steps rows and writes their JSON into a buffer.
    render_posts: {
      parameters: ["pointer", "buffer", "i32", "i32"],
      result: "i32",
    },
    // render.c's off-thread checkpointer.
    ckpt_start: { parameters: ["buffer", "i32", "i32"], result: "i32" },
    ckpt_stats: { parameters: ["buffer", "i32"], result: "void" },
    ckpt_counters: { parameters: [], result: "pointer" },
    // render.c's commit thread.
    wr_start: { parameters: ["pointer", "i32"], result: "i32" },
    wr_commit: { parameters: [], result: "void" },
    wr_stats: { parameters: ["buffer", "i32"], result: "void" },
    wr_counters: { parameters: [], result: "pointer" },
  },
).symbols;

const enc = new TextEncoder();
const cstr = (s: string) => enc.encode(s + "\0");
const out = new BigUint64Array(1); // out-param for sqlite3_open_v2 / sqlite3_prepare_v3
const textBuf = new Uint8Array(2048); // bound text: ≤ 500 UTF-16 units is ≤ 1500 UTF-8 bytes

export class Db {
  readonly h: Deno.PointerObject;
  constructor(path: string) {
    // READWRITE | CREATE
    const rc = lib.sqlite3_open_v2(
      cstr(path),
      new Uint8Array(out.buffer),
      6,
      null,
    );
    this.h = Deno.UnsafePointer.create(out[0])!;
    if (rc !== 0) throw this.error();
  }
  error(): Error {
    return new Error(
      Deno.UnsafePointerView.getCString(lib.sqlite3_errmsg(this.h)!),
    );
  }
  exec(sql: string): void {
    if (lib.sqlite3_exec(this.h, cstr(sql), null, null, null) !== 0) {
      throw this.error();
    }
  }
  prepare(sql: string): Stmt {
    // SQLITE_PREPARE_PERSISTENT: these live for the whole process.
    if (
      lib.sqlite3_prepare_v3(
        this.h,
        cstr(sql),
        -1,
        1,
        new Uint8Array(out.buffer),
        null,
      ) !== 0
    ) {
      throw this.error();
    }
    return new Stmt(this, Deno.UnsafePointer.create(out[0])!);
  }
  changes(): number {
    return lib.sqlite3_changes(this.h);
  }
}

export class Stmt {
  constructor(readonly db: Db, readonly h: Deno.PointerObject) {}
  int(i: number, v: number): this {
    lib.sqlite3_bind_int(this.h, i, v);
    return this;
  }
  text(i: number, s: string): this {
    const { written } = enc.encodeInto(s, textBuf);
    lib.sqlite3_bind_text(this.h, i, textBuf, written, TRANSIENT);
    return this;
  }
  // true when a row is ready. Always call done() afterwards.
  step(): boolean {
    const rc = lib.sqlite3_step(this.h);
    if (rc === ROW) return true;
    if (rc === DONE) return false;
    const e = this.db.error();
    lib.sqlite3_reset(this.h);
    throw e;
  }
  // Resets the statement; in autocommit this is where a write that returned a row commits.
  done(): void {
    if (lib.sqlite3_reset(this.h) !== 0) throw this.db.error();
  }
  blob(col: number): Uint8Array<ArrayBuffer> {
    const n = lib.sqlite3_column_bytes(this.h, col);
    const b = new Uint8Array(n);
    if (n > 0) {
      Deno.UnsafePointerView.copyInto(lib.sqlite3_column_blob(this.h, col)!, b);
    }
    return b;
  }
  // render.c's render_posts: steps this bound statement and writes the JSON body of its
  // (id, body, created_at, author, like_count) rows into out. single: {"post":{...}} from the first
  // row, else {"posts":[...]}. Returns the bytes written, 0 when single finds no row. Resets.
  render(out: Uint8Array, single: boolean): number {
    const n = lib.render_posts(this.h, out, out.length, single ? 1 : 0);
    if (n === -2) throw this.db.error();
    if (n < 0) throw new Error("render_posts: output buffer too small");
    return n;
  }
  colInt(col: number): number {
    return lib.sqlite3_column_int(this.h, col);
  }
  colText(col: number): string {
    return Deno.UnsafePointerView.getCString(
      lib.sqlite3_column_text(this.h, col)!,
    );
  }
}

// Starts render.c's checkpointer: a pthread with its own connection to path (synchronous = sync, 0–3 as in
// PRAGMA synchronous) that runs a checkpoint every intervalMs: PASSIVE, or TRUNCATE once the WAL is past 4096
// frames. Once per process; throws on failure.
export function ckptStart(
  path: string,
  intervalMs: number,
  sync: number,
): void {
  const rc = lib.ckpt_start(cstr(path), intervalMs, sync);
  if (rc !== 0) throw new Error(`ckpt_start: SQLite error ${rc}`);
}

const stats = new BigInt64Array(4);
const ckptCounters = new BigInt64Array(
  Deno.UnsafePointerView.getArrayBuffer(lib.ckpt_counters()!, 40),
);
// True while a TRUNCATE checkpoint waits for readers on old snapshots: refresh the read connection.
export function ckptWaiting(): boolean {
  return Atomics.load(ckptCounters, 4) !== 0n;
}
// Checkpoints run, the slowest in ms (since the last resetWorst call), and the last one's WAL frames (log) and frames backfilled (ckpt).
export function ckptStats(resetWorst = false): {
  runs: number;
  worstMs: number;
  log: number;
  ckpt: number;
} {
  lib.ckpt_stats(new Uint8Array(stats.buffer), resetWorst ? 1 : 0);
  return {
    runs: Number(stats[0]),
    worstMs: Number(stats[1]),
    log: Number(stats[2]),
    ckpt: Number(stats[3]),
  };
}

// Starts render.c's commit thread on db's connection (poller only): wrCommit hands it the open transaction for
// COMMIT; BEGIN, and wrDone() moving past the handed count means it is back. wakeFd: poller.rs's px_wake_open.
export function wrStart(db: Db, wakeFd: number): void {
  const rc = lib.wr_start(db.h, wakeFd);
  if (rc !== 0) throw new Error(`wr_start: SQLite error ${rc}`);
}
export function wrCommit(): void {
  lib.wr_commit();
}
const wrCounters = new BigInt64Array(
  Deno.UnsafePointerView.getArrayBuffer(lib.wr_counters()!, 24),
);
export function wrDone(): number {
  return Number(Atomics.load(wrCounters, 0));
}
const wrOut = new BigInt64Array(3);
// COMMITs landed, the slowest in ms (since the last reset), and the last one's SQLite result code (0: ok).
export function wrStats(
  resetWorst = false,
): { commits: number; worstMs: number; rc: number } {
  lib.wr_stats(new Uint8Array(wrOut.buffer), resetWorst ? 1 : 0);
  return {
    commits: Number(wrOut[0]),
    worstMs: Number(wrOut[1]),
    rc: Number(wrOut[2]),
  };
}

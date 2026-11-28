#!/usr/bin/env python3
"""Exercise the real C checkpoint return handling with a stub SQLite API."""
from pathlib import Path
import subprocess
import sys
import tempfile

shim = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1] / 'src/shim.c'
source = shim.read_text(encoding='utf-8')
body = source[source.index('#define CK_FORCE_FRAMES'):source.index('long long dbx_wal_frames_total(')]
header = (shim.parent / 'shim.h').read_text(encoding='utf-8')
stats_type = header[header.index('typedef struct dbx_checkpoint_stats {'):
                    header.index('dbx_checkpoint_stats dbx_ck_stats(')]
prefix = r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define SQLITE_OK 0
#define SQLITE_IOERR 10
#define SQLITE_BUSY 5
#define SQLITE_CHECKPOINT_PASSIVE 0
#define shim_log(...) ((void)0)
static int rc, log_frames, done_frames, calls;
static int ck_gate_frames(void) { return 16384; }
static long long tick;
static long long mono_ns(void) { return ++tick; }
static void usleep(unsigned n) { (void)n; }

typedef struct dbx {
    void *ckdb;
    _Atomic int wal_frames, ck_state, ck_full, gate_held;
    _Atomic long long gate_since;
    uint64_t ck_force_calls, ck_force_max_ns, ck_hold_timeouts;
    _Atomic uint64_t ck_tail_incomplete;
} dbx;

static int sqlite3_wal_checkpoint_v2(void *db, const char *name, int mode, int *log, int *done) {
    (void)db;
    assert(strcmp(name, "main") == 0 && mode == SQLITE_CHECKPOINT_PASSIVE);
    ++calls;
    *log = log_frames;
    *done = done_frames;
    return rc;
}
'''
checks = r'''
static void test(int result, int log, int done, int expected) {
    dbx h = { .wal_frames = CK_FORCE_FRAMES };
    rc = result;
    log_frames = log;
    done_frames = done;
    calls = 0;
    assert(dbx_ck_kick(&h) == 0 && calls == 1);
    assert(h.wal_frames == expected);
    dbx_checkpoint_stats stats = dbx_ck_stats(&h);
    assert(stats.state == CK_IDLE && stats.force_calls == 1 && stats.force_max_ns == 1);
    assert(stats.wal_frames == expected && !stats.held);
}

static void test_tail(void) {
    dbx h = { .ck_state = CK_TAIL, .gate_held = 1, .gate_since = tick };
    assert(dbx_writes_held(&h));
    tick += CK_HOLD_NS;
    assert(!dbx_writes_held(&h) && !dbx_writes_held(&h));
    dbx_checkpoint_stats stats = dbx_ck_stats(&h);
    assert(stats.state == CK_TAIL && stats.hold_timeouts == 1 && !stats.held);
    log_frames = CK_TAIL_FRAMES * 2;
    done_frames = CK_TAIL_FRAMES;
    dbx_ck_run(&h, 1);
    stats = dbx_ck_stats(&h);
    assert(stats.state == CK_TAIL_DONE && stats.tail_incomplete == 1);
    done_frames = log_frames;
    dbx_ck_run(&h, 1);
    assert(dbx_ck_stats(&h).tail_incomplete == 1);
}

int main(void) {
    test(SQLITE_IOERR, 0, 0, CK_FORCE_FRAMES);
    test(SQLITE_BUSY, 0, 0, CK_FORCE_FRAMES);
    test(SQLITE_IOERR, -1, -1, CK_FORCE_FRAMES);
    test(SQLITE_BUSY, -1, -1, CK_FORCE_FRAMES);
    test(SQLITE_OK, -1, -1, CK_FORCE_FRAMES);
    test(SQLITE_OK, CK_FORCE_FRAMES, -1, CK_FORCE_FRAMES);
    test(SQLITE_OK, CK_FORCE_FRAMES, 4096, CK_FORCE_FRAMES);
    test(SQLITE_OK, CK_FORCE_FRAMES, CK_FORCE_FRAMES, 0);
    test(SQLITE_OK, 0, 0, 0);
    test_tail();
}
'''
with tempfile.TemporaryDirectory(prefix='checkpoint-force-') as tmp:
    filename = Path(tmp) / 'check.c'
    binary = Path(tmp) / 'check'
    filename.write_text(prefix + stats_type + body + checks, encoding='utf-8')
    subprocess.run(['gcc', '-std=c11', '-O2', str(filename), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS: synchronous checkpoint preserves WAL estimate on failed, missing or partial results')

/* qc.c: a runtime query compiler for SQLite prepared statements. #include after sqlite3.c (SQLite internals).
**
**   int sqlite3_qc(sqlite3_stmt *)          lift the statement's VDBE program into a plan; 1 = compiled, 0 = not this class
**   int sqlite3_qc_exec(stmt, cb, ctx)      run the plan: all rows at once, zero-copy (pointers into the database pages,
**                                           valid during the callback). QC_FALLBACK = run the statement normally instead
**
** The lifter reads the bytecode SQLite's planner produced and recognises one class of programs, checking every operand
** that matters and refusing anything else:
**   driver:   an index scan with a constant LIMIT (Rewind/Next on an index cursor, DecrJumpZero), or a rowid lookup on a
**             bound parameter (Variable + SeekRowid)
**   row:      the table row behind the driver (DeferredSeek + IdxRowid, or the SeekRowid itself)
**   joins:    inner joins by rowid on a column of that row (SeekRowid, a miss skips the row)
**   scalars:  correlated count(*) subqueries over an index equality on the row's rowid (BeginSubrtn ... Return)
**   output:   ResultRow of rowids, columns and counts
** The plan then runs a cheaper program with the same result:
**   - the driver's rows are collected first, then the table rows are fetched (stepping back one row when rowids
**     descend by one), keeping their pages pinned;
**   - each join is one batched B-tree descent for all rows, prefetching child pages level by level;
**   - each count is one range walk of the index over [min, max] of the keys (per-key seeks if the range is wide);
**   - values are decoded straight from the records, never through Mem.
** Every execution reads the live tables under the statement's transaction (no state is kept between executions).
** Anything unexpected at run time (a NULL or non-integer join key, a missing row, an overflow record, a value whose type
** the lifted program would have converted) returns QC_FALLBACK before any row is handed out, and the caller steps the
** statement through the VDBE, which produces whatever SQLite produces (including errors). */

#ifdef _WIN32   /* (clang-cl, the Node binding's Windows builds: no pthread.h; sqlite3.c has included windows.h) */
typedef SRWLOCK qc_mutex_t;
#define QC_MUTEX_INIT SRWLOCK_INIT
#define qc_mutex_lock(m) AcquireSRWLockExclusive(m)
#define qc_mutex_unlock(m) ReleaseSRWLockExclusive(m)
#else
#include <pthread.h>
typedef pthread_mutex_t qc_mutex_t;
#define QC_MUTEX_INIT PTHREAD_MUTEX_INITIALIZER
#define qc_mutex_lock(m) pthread_mutex_lock(m)
#define qc_mutex_unlock(m) pthread_mutex_unlock(m)
#endif
#define QC_FALLBACK (-1)
#define QC_ITIMED (-4)   /* qc_adapt: a served statement's interpreter sample, run as a parked statement runs (qc_serve_itimed) */
static _Thread_local const char *qc_why = "";   /* the last fallback's reason on this thread: the call's, read after it (tests, QC_STATS, the refusals kept) */
#define QC_FB(w) (qc_why = (w), QC_FALLBACK)
/* ---------------------------------------------------------------- QC_STOCK=1: an unmodified amalgamation
** The patched build (qc/patch.py) keeps qc's state in struct Vdbe and struct sqlite3, and SQLite calls qc from hooks.
** Built with -DQC_STOCK=1, sqlite3.c is SQLite's own file, byte for byte, and:
**   - a statement's state (QcVs) and a connection's (QcDs) live in side tables: QcDs is the connection's client data
**     (sqlite3_set_clientdata, whose destructor drops everything when the connection closes: hook 5), and it holds its
**     statements' QcVs by address. Each API call looks its statement and connection up once and keeps them in two
**     thread-locals, so QCV(v) and QCD(db) are a compare each in the call (the side tables for anything else); in the
**     patched build they are v and db themselves;
**   - nothing is served through sqlite3_step (hooks 6, 7): sqlite3_qc_exec only, and QC_FALLBACK means the caller steps;
**   - sqlite3_qc_forget(stmt) frees a statement's plan before sqlite3_finalize (sqlite3_qc_finalize does both: hook 4).
**     A statement SQLite re-prepared (a schema change, a new binding its program looked at) is seen at qc's next call
**     on it, its program and REPREPARE count changed: its plan goes to the plan cache, and it is lifted again;
**   - a plan's cursors are closed at the end of every sqlite3_qc_exec (hook 3: no hook sees a transaction end), so in an
**     explicit transaction a plan opens its cursors and checks the schema cookie again on every run;
**   - the plan cache compares every program's signature (hook 8: no count of sqlite3ExpirePreparedStatements calls to
**     stamp a plan with);
**   - qc's copies of the SQLite functions it calls (qc/patch.py --copies, step 10) are qc_copies.c, generated from the
**     stock amalgamation and included here (-DQC_STOCK_COPIES=0: left out, qc calls SQLite's own). */
#ifndef QC_STOCK
#define QC_STOCK 0
#endif
#if QC_STOCK
#if !defined(QC_STOCK_COPIES) || QC_STOCK_COPIES
#include "qc_copies.c"
#endif
static void (*sqlite3QcTxnEnd)(Btree *), (*sqlite3QcFree)(Vdbe *), (*sqlite3QcClose)(sqlite3 *), (*sqlite3QcStart)(Vdbe *), (*sqlite3QcReset)(Vdbe *);
static int (*sqlite3QcServe)(Vdbe *);   /* (the patched build's hooks: set, never called) */
typedef struct QcVs {   /* a statement's (patch.py steps 1, 2, 6, 7) */
  void *pQc, *pQcTmpl, *pQcRows; u32 qcPark; u8 qcWant, qcServe, qcNo;
  Vdbe *v; const Op *aOp; u32 nRep; const char *zSql;   /* the program it is for (qc_vs_enter) */
} QcVs;
typedef struct QcDs {   /* a connection's (step 9), its client data */
  void *pQcConn, *pQgConn, *pQvConn; u32 qcEpoch; u8 qcBusy, qcServing;
  sqlite3 *db; Vdbe *lastV; QcVs *lastS;   /* the last statement looked up */
  QcVs **tab; u32 ntab, mask;              /* its statements' QcVs, open addressing by address (mask + 1 slots) */
} QcDs;
#define QC_DS_NAME "cursed-sqlite-turbo"
static QcVs qc_vs_none; static QcDs qc_ds_none;   /* (no state yet: read as zeros; the API calls make it before anything is written) */
/* the connection and statement this thread's API call is on (qc_cur_db at its start, qc_vs_enter, qc_conn_make, the
   destructor): set from the side tables in the call itself, so a pointer left by an earlier call is never trusted */
static _Thread_local sqlite3 *qc_cdb; static _Thread_local QcDs *qc_cd;
static _Thread_local Vdbe *qc_cv; static _Thread_local QcVs *qc_cs;
static void qc_ds_destroy(void *p);
static __attribute__((noinline)) QcDs *qc_ds_find(const sqlite3 *db) {   /* (another client data in front of qc's) */
  for (DbClientData *p = db->pDbData; p; p = p->pNext) if (p->xDestructor == qc_ds_destroy) return p->pData;
  return &qc_ds_none;
}
static inline QcDs *qc_ds(const sqlite3 *db) { DbClientData *p = db->pDbData; return p && p->xDestructor == qc_ds_destroy ? p->pData : qc_ds_find(db); }
static inline u32 qc_vs_slot(const QcDs *d, const Vdbe *v) { return (u32)(((uintptr_t)v >> 4) * 0x9e3779b97f4a7c15ull >> 32) & d->mask; }
static QcVs **qc_vs_at(QcDs *d, const Vdbe *v) {   /* v's slot, or the empty one it would take */
  u32 i = qc_vs_slot(d, v); while (d->tab[i] && d->tab[i]->v != v) i = (i + 1) & d->mask;
  return &d->tab[i];
}
static __attribute__((noinline)) QcVs *qc_vs_find(QcDs *d, Vdbe *v) {   /* v's QcVs, made if it has none */
  if (d == &qc_ds_none) return &qc_vs_none;
  if (!d->tab || (d->ntab + 1) * 2 > d->mask + 1) {   /* (at most half full) */
    u32 n = d->tab ? (d->mask + 1) * 2 : 16; QcVs **t = sqlite3MallocZero(n * sizeof *t), **o = d->tab; if (!t) return &qc_vs_none;
    u32 m = d->mask; d->tab = t; d->mask = n - 1;
    if (o) { for (u32 i = 0; i <= m; i++) if (o[i]) *qc_vs_at(d, o[i]->v) = o[i]; sqlite3_free(o); }
  }
  QcVs **e = qc_vs_at(d, v), *s = *e;
  if (!s) {
    if (!(s = sqlite3MallocZero(sizeof *s))) return &qc_vs_none;
    s->v = v; s->aOp = v->aOp; s->nRep = v->aCounter[SQLITE_STMTSTATUS_REPREPARE]; s->zSql = v->zSql; *e = s; d->ntab++;
  }
  d->lastV = v; d->lastS = s; return s;
}
static inline QcVs *qc_vs(Vdbe *v) { QcDs *d = qc_ds(v->db); return d->lastV == v ? d->lastS : qc_vs_find(d, v); }
static void qc_vs_del(QcDs *d, Vdbe *v) {   /* v's QcVs freed (its plan and template already are) */
  if (!d->tab) return;
  QcVs **e = qc_vs_at(d, v); if (!*e) return;
  sqlite3_free(*e); *e = 0; d->ntab--; if (d->lastV == v) d->lastV = 0; if (qc_cv == v) qc_cv = 0;
  for (u32 j = ((u32)(e - d->tab) + 1) & d->mask; d->tab[j]; j = (j + 1) & d->mask) {   /* (the run after it, placed again) */
    QcVs *x = d->tab[j]; d->tab[j] = 0; *qc_vs_at(d, x->v) = x;
  }
}
static inline void qc_cur_db(sqlite3 *db) { qc_cdb = db; qc_cd = qc_ds(db); qc_cv = 0; }
static inline QcDs *qc_ds_cur(const sqlite3 *db) { return db == qc_cdb ? qc_cd : qc_ds(db); }
static inline QcVs *qc_vs_cur(Vdbe *v) { return v == qc_cv ? qc_cs : qc_vs(v); }
static QcVs *qc_vs_enter(Vdbe *v);
#define QCV(v) qc_vs_cur(v)
#define QCD(db) qc_ds_cur(db)
#else
#define QCV(v) (v)
#define QCD(db) (db)
#endif
#ifndef QC_GATE_OPS
#define QC_GATE_OPS 40   /* qc_gate_small: a program body of at most this many ops, */
#endif
#ifndef QC_GATE_CUR
#define QC_GATE_CUR 2    /* and cursors (a table, or an index and its table) */
#endif
#ifndef SQLITE_OMIT_PROGRESS_CALLBACK
#define QC_PROGRESS(db) ((db)->xProgress != 0)   /* a progress handler: left to the interpreter (no xProgress without it: better-sqlite3's build) */
#else
#define QC_PROGRESS(db) 0
#endif
/* QC_STATS=1: process-wide counts, printed at exit or by sqlite3_qc_stats(): executions by tier, fallbacks and failed
   lifts by reason, the type-feedback masks of Column and comparison opcodes (once per plan, when its feedback runs
   are over), region nodes widened, plan cache outcomes. Off, they cost one test per execution */
typedef struct QcWhy { const char *why; unsigned long long n; } QcWhy;
static struct QcStats {
  int on; unsigned long long exec[4], ok, lift, liftOk, relift, reliftFail, widen, mask[2][32];
  unsigned long long sample, cheap, keep, park, unpark;
  unsigned long long stream, drain, replay;   /* streamed runs; drained before another statement; replayed after a failure */
  QcWhy fb[64], lf[64];
} qcs = { -1 };
#define QC_ST (qcs.on > 0)
/* the environment's switches (A/B and tests) and the clock's calibration, read once by qc_init (pthread_once, before any
   connection's state is made): process-wide and read-only after it */
static struct QcCfg {
  int noReoffer, noGate, noScanGate, park, streamFailAt, vmOnly, adaptOn, adDetail, genLookups, numColInt, noHash, noTopn, rgOn, cacheOn, noStamp, streamOn;
  unsigned streamMin; double smallUs, adaptMinUs, margin, smallWin, clockNs;
} qcf;
#define QC_INC(n) __atomic_fetch_add(&(n), 1, __ATOMIC_RELAXED)   /* (QC_STATS's counts: connections on several threads add to them) */
static qc_mutex_t qc_st_mu = QC_MUTEX_INIT;
static void qc_st_why(QcWhy *w, const char *why) {   /* (QC_STATS only: under a lock, as the reason may be a new entry) */
  qc_mutex_lock(&qc_st_mu);
  int i = 0; while (i < 63 && w[i].why && w[i].why != why && strcmp(w[i].why, why)) i++;
  if (!w[i].why) w[i].why = why;
  w[i].n++;
  qc_mutex_unlock(&qc_st_mu);
}
/* a general or qvm plan measured against the interpreter (qc_adapt): executions through it, its state (learning,
   kept, parked), the execution at which that next changes, how often it was parked (R) and kept (K), whether the
   interpreter's warm-up run was had (iw), the samples (µs) */
typedef struct QcAdapt { unsigned n, next; unsigned char st, R, K, nq, ni, iw, pw, wt, nfb; double tq[4], ti[4]; } QcAdapt;   /* pw, wt: the plan's, the interpreter's turn warmed (served); nfb: its runs in a row that fell back */
#define QC_STR2(x) #x
#define QC_STR(x) QC_STR2(x)
#define QC_MAXROWS 64
#define QC_MAXOUT 16
#define QC_MAXJOIN 4
#define QC_MAXCNT 4
#define QC_MAXCOL 16
#define QC_RANGE_MAX 1024

typedef struct QcVal { union { i64 i; double r; const u8 *z; }; int n, type; } QcVal;   /* 16 bytes; type: SQLITE_INTEGER ... SQLITE_NULL */
typedef int (*qc_row_cb)(void *ctx, int nrow, int ncol, const QcVal *vals);
/* sqlite3_qc_exec(stmt, 0, &out): the body. param: the integer parameters ?1.. as plain values (a compiled query is
   called like a function), or 0 to use the statement's bindings */
typedef struct QcOut { char *buf; size_t cap, n; int rows; const i64 *param; int nparam; } QcOut;

enum { S_NONE, S_INT, S_PARAM, S_NULL, S_ROWID, S_COL, S_COUNT };   /* symbolic register values */
typedef struct { u8 kind; i8 src; i16 col; i64 v; u8 real; } QcSym;   /* src: -1 the driver index, 0 the row, 1+j join j; count c for S_COUNT; real: OP_RealAffinity applied */

/* expected layout of a record source, from the schema: one class per column up to the last one needed */
enum { K_ANY, K_NUL, K_INT, K_TXT, K_REAL };   /* (K_REAL: no generated decoder, qc_decode_fast) */
typedef struct { u8 ok, n; u8 cls[QC_MAXCOL]; } QcShape;
typedef struct QcPlan {
  int kind;                  /* 1: this specialised plan (2: a general plan, qcg.c) */
  int scan;                  /* 1: index scan with limit; 0: rowid = ?param, or = key (param 0: an integer literal) */
  Pgno idxRoot; KeyInfo *idxKi; int limit; int param; i64 key;
  Pgno tabRoot;
  int njoin; struct { Pgno root; int keycol; } join[QC_MAXJOIN];
  int ncnt; struct { Pgno root; KeyInfo *ki; UnpackedRecord *uk; } cnt[QC_MAXCNT];
  int nout; QcSym out[QC_MAXOUT];
  u32 idxCols, tabCols, joinCols[QC_MAXJOIN];   /* bitmasks of the columns each side needs (< QC_MAXCOL) */
  QcShape idxShape, tabShape, joinShape[QC_MAXJOIN];  /* idxShape also covers the index's last field, the rowid */
  int idxRowidField;                                 /* that field's number, or -1 */
  int idxW, tabW, joinW[QC_MAXJOIN];   /* row widths in the run state */
  int (*idxDec)(const u8 *, u32, QcVal *), (*tabDec)(const u8 *, u32, QcVal *), (*joinDec[QC_MAXJOIN])(const u8 *, u32, QcVal *);
  u32 idxCode, tabCode, joinCode[QC_MAXJOIN];   /* the same decoders by signature (qc_dec_run: inlined, no pointer call) */
  int iDb, cookie, gen, nTxn;
  /* cursors kept open between executions while the transaction lasts (closed by sqlite3QcTxnEnd, at its end) */
  Btree *bt; BtCursor *ci, *ct, *cj[QC_MAXJOIN], *cc[QC_MAXCNT]; u8 oi, ot, oj[QC_MAXJOIN], oc[QC_MAXCNT];
  u8 txnChecked;   /* the schema cookie was checked in this transaction (cleared at its end, like the cursors) */
  int slot, act;   /* counted in QcConn's nplans (1), index + 1 in its act (0: not in them) */
  u8 lanes;        /* overlapped descents for the rows and counts (QC_LANES=1 at lift time; off by default: RESULTS 5e) */
  u8 nfb;                    /* runs in a row it fell back on (qc_exec_plan demotes it to qvm at 2) */
} QcPlan;
/* the feed's hot loops start on a 64-byte boundary, so code added before them doesn't move their branches: on the Xeon
   (Cascade Lake, JCC erratum microcode) a jump that crosses a 32-byte boundary runs from the legacy decoders, and an
   unrelated change had made the feed 18 % slower in cycles at the same instructions (RESULTS §13) */
#if defined(__GNUC__)
#define QC_HOT __attribute__((aligned(64)))
#else
#define QC_HOT
#endif
#ifndef QC_MAXPLANS   /* live plans per tier, per connection (-DQC_MAXPLANS=N); past it a statement is not lifted ("plans") */
#define QC_MAXPLANS 1024
#endif
/* a tier's plans, counted (slot: 1 when counted), and of those the ones used in the current transaction (act), whose
   cursors its end closes: index + 1 in the array, 0 when not in it; removal moves the last one into the gap */
#define QC_COUNT_ADD(n, P) do { (n)++; (P)->slot = 1; } while (0)
#define QC_COUNT_DEL(n, P) do { if ((P)->slot) { (n)--; (P)->slot = 0; } } while (0)
#define QC_LIST_ADD(a, n, P, f) do { (a)[(n)] = (P); (P)->f = ++(n); } while (0)
#define QC_LIST_DEL(a, n, P, f) do { if ((P)->f) { int k_ = (P)->f - 1; (a)[k_] = (a)[--(n)]; (a)[k_]->f = k_ + 1; (P)->f = 0; } } while (0)

/* ---------------------------------------------------------------- per-connection state
** Everything qc keeps between calls lives with the connection: this (db->pQcConn), qcg.c's QgConn (pQgConn) and qvm.c's
** QvConn (pQvConn), made together at the connection's first lift (qc_conn) and freed with it (sqlite3QcClose). Every hook
** runs under the connection's mutex (or, in multi-thread mode, on the one thread the application lets use it), so
** connections on different threads share nothing that changes and no lock is taken. New state goes in one of these
** three, never in a file-scope static (process-wide: read-only after qc_init, or an atomic counter). */
#define QC_SPOOL 8
typedef struct QcQpsg { sqlite3 *db; u32 h; u64 prog; } QcQpsg;   /* (qc_qpsg's) prog: the QPSG program's qc_prog_hash */
typedef struct QcConn {
  int nplans, nact; QcPlan *act[QC_MAXPLANS];   /* the specialised plans: counted, and the ones in this transaction */
  struct QcRun *run;                            /* the specialised plans' run state (made at the first run) */
  struct QcLane *lanes;                         /* their overlapped descents (QC_LANES=1: made at the first such run) */
  int ownTxn;   /* sqlite3_qc_exec began the read transaction for this run (in autocommit): the plans run in it */
  /* serving (sqlite3QcStart; db->qcServing: in it, qc's own interpreter samples step the statement normally) */
  double *adTq;     /* qc_adapt's plan sample of the execution that just ran (qc_serve_start passes it on to the handing out) */
  struct QcAdapt *adIA;   /* qc_adapt's plan state for the interpreter sample it handed back (QC_ITIMED) */
  int adWant; double adTs;   /* qc_serve_start's plan sample: wanted, and its start (sqlite3_qc_exec's) */
  int mayStream;    /* in qc_serve_start's run: a plan may stream (QvConn's streamOk, where qc_adapt isn't timing it) */
  Vdbe *strmV; Btree *strmOwn[8]; int strmNown;   /* the suspended stream's statement and the read transactions qc began for it */
  struct QcServe *spool[QC_SPOOL]; int nspool;    /* finalized statements' QcServes, for the next prepares (qc_serve_free) */
  struct QcServe *big;   /* one finalized statement's large QcServe (its rows, values and text bytes, up to QC_SERVE_KEEP), for the
                            next prepare of the same text: a statement prepared per execution reuses them instead of faulting them
                            in again (bench exp1159: calibre books_table_columns, trilium revisions 260-670 page faults a prepare).
                            Only its text's: handed to any statement, a small one prepared after a big one walked its 16k Mems
                            when it took it and again when it was freed (bench corpus8: prep up to 3.9x stock, ftl
                            network.name_from_ip_fallback, ha states_meta.find_ids) */
  u32 bigH; int bigN;    /* its text's hash and length */
  QcVal *irVal; char *irBuf; size_t irCv, irCb;   /* qc_interp_rows' rows, kept between samples */
  QcQpsg qpsg[256];                               /* texts re-prepared under QPSG's rule (qc_qpsg) */
} QcConn;
#define QC_CONN(db) ((QcConn *)QCD((db))->pQcConn)
static int qc_conn_make(sqlite3 *db);   /* (after qcg.c and qvm.c: all three at once) 1 = made */
static inline QcConn *qc_conn(sqlite3 *db) { return QCD(db)->pQcConn || qc_conn_make(db) ? QCD(db)->pQcConn : 0; }
static void qc_close(BtCursor *c, u8 *open) { if (*open) { sqlite3BtreeCloseCursor(c); *open = 0; } }
static void qc_txn_end(Btree *bt) {
  QcConn *C = QCD(bt->db)->pQcConn; if (!C) return;
  for (int k = 0; k < C->nact; ) {
    QcPlan *P = C->act[k]; if (P->bt != bt) { k++; continue; }
    P->txnChecked = 0;
    qc_close(P->ci, &P->oi); qc_close(P->ct, &P->ot);
    for (int j = 0; j < P->njoin; j++) qc_close(P->cj[j], &P->oj[j]);   /* (only a plan's own joins and counts open theirs) */
    for (int c = 0; c < P->ncnt; c++) qc_close(P->cc[c], &P->oc[c]);
    QC_LIST_DEL(C->act, C->nact, P, act);   /* (the last one moves to k) */
  }
}

/* ---------------------------------------------------------------- shapes */
static int qc_class(Table *t, int col) {
  if (col < 0) return col == XN_ROWID ? K_INT : K_ANY;
  if (col == t->iPKey) return K_NUL;   /* an INTEGER PRIMARY KEY is stored as NULL: the value is the rowid */
  if (col >= t->nCol) return K_ANY;
  char a = t->aCol[col].affinity;
  return a == SQLITE_AFF_INTEGER ? K_INT : a == SQLITE_AFF_TEXT ? K_TXT : a == SQLITE_AFF_REAL ? K_REAL : K_ANY;
}
/* the table or index whose b-tree is root; *pIdx is set for an index */
static Table *qc_find(Schema *sc, Pgno root, Index **pIdx) {
  *pIdx = 0;
  for (HashElem *k = sqliteHashFirst(&sc->tblHash); k; k = sqliteHashNext(k)) {
    Table *t = sqliteHashData(k);
    if (IsVirtual(t) || IsView(t)) continue;
    if (t->tnum == root) return HasRowid(t) ? t : 0;
    for (Index *ix = t->pIndex; ix; ix = ix->pNext) if (ix->tnum == root) { *pIdx = ix; return HasRowid(t) ? t : 0; }
  }
  return 0;
}
/* the shape of the columns in mask (+ field extra, if >= 0); ok only if each of them has a definite class */
static void qc_shape(QcShape *sh, Table *t, Index *ix, u32 mask, int extra) {
  memset(sh, 0, sizeof *sh);
  if (!t) return;
  u32 all = mask | (extra >= 0 ? 1u << extra : 0); int n = 0;
  for (int c = 0; c < QC_MAXCOL; c++) if (all & (1u << c)) n = c + 1;
  for (int c = 0; c < n; c++) {
    int col = ix ? (c < ix->nColumn ? ix->aiColumn[c] : XN_EXPR) : c;
    /* in an index the INTEGER PRIMARY KEY column holds the integer itself (only the table row stores it as NULL) */
    sh->cls[c] = (u8)(ix && col >= 0 && col == t->iPKey ? K_INT : qc_class(t, col));
    if ((all & (1u << c)) && sh->cls[c] == K_ANY) return;
    if (sh->cls[c] == K_ANY) return;   /* a column we skip over still needs a known width class */
  }
  sh->n = (u8)n; sh->ok = n > 0;
}

static int (*qc_dec_pick(const QcShape *sh))(const u8 *, u32, QcVal *);   /* qc_dec.h */
static u32 qc_dec_code(const QcShape *sh);

/* ---------------------------------------------------------------- lifter */
static int qc_fail(const char *why, int pc) { qc_why = why; (void)pc; return 0; }
#define QC_REFUSE(why) return qc_fail(why, pc)
#define OPN(k) (&aOp[k])

static int qc_lift_special(Vdbe *v) {
  Op *aOp = v->aOp; int nOp = v->nOp, pc = 0;
  if (v->nMem > 200) return 0;
  QcPlan P; memset(&P, 0, sizeof P); P.kind = 1; P.iDb = -1;
  QcSym reg[201]; memset(reg, 0, sizeof reg);
  struct { Pgno root; KeyInfo *ki; int open; } cur[16]; memset(cur, 0, sizeof cur);
  int curRole[16]; for (int k = 0; k < 16; k++) curRole[k] = -1;   /* -2 driver index, -3 row table, j>=0 join j, 100+c count c */
  /* Init -> Transaction(s) -> Goto back */
  if (aOp[0].opcode != OP_Init) QC_REFUSE("init");
  int t = aOp[0].p2;
  while (t < nOp && aOp[t].opcode == OP_Transaction) {
    Op *o = OPN(t);
    if (o->p2 != 0 || o->p5 != 1 || P.nTxn) QC_REFUSE("transaction");   /* one read transaction, schema checked */
    if (o->p4type != P4_INT32) QC_REFUSE("transaction p4");
    P.iDb = o->p1; P.cookie = o->p3; P.gen = o->p4.i; P.nTxn++; t++;
  }
  if (!P.nTxn || t >= nOp || aOp[t].opcode != OP_Goto || aOp[t].p2 != 1) QC_REFUSE("preamble");
  int endPreamble = aOp[0].p2;
  pc = 1;
  int haltPc = -1, loopHead = -1, limitReg = -1, idxCur = -1, tabCur = -1;
  /* setup: Noop, Integer (limit), Variable, OpenRead */
  for (;; pc++) {
    if (pc >= endPreamble) QC_REFUSE("no body");
    Op *o = OPN(pc);
    if (o->opcode == OP_Noop) continue;
    if (o->opcode == OP_Integer) { if (o->p2 > v->nMem) QC_REFUSE("reg"); reg[o->p2] = (QcSym){S_INT, 0, 0, o->p1}; continue; }
    if (o->opcode == OP_Variable) { if (o->p2 > v->nMem) QC_REFUSE("reg"); reg[o->p2] = (QcSym){S_PARAM, 0, 0, o->p1}; continue; }
    if (o->opcode == OP_OpenRead) {
      if (o->p1 < 0 || o->p1 >= 16 || o->p3 != P.iDb || (o->p5 & ~OPFLAG_SEEKEQ) || (o->p4type != P4_INT32 && o->p4type != P4_KEYINFO)) QC_REFUSE("openread");
      cur[o->p1].root = (Pgno)o->p2; cur[o->p1].ki = o->p4type == P4_KEYINFO ? o->p4.pKeyInfo : 0; cur[o->p1].open = 1; continue;
    }
    break;
  }
  Op *o = OPN(pc);
  if (o->opcode == OP_Rewind) {                 /* index scan driver */
    if (!cur[o->p1].open || !cur[o->p1].ki) QC_REFUSE("rewind");
    P.scan = 1; idxCur = o->p1; haltPc = o->p2; P.idxRoot = cur[idxCur].root; P.idxKi = cur[idxCur].ki; curRole[idxCur] = -2;
    loopHead = ++pc;
    o = OPN(pc);
    /* DeferredSeek P1 = the index cursor, P3 = its table's cursor, no P4 alternate column map */
    if (o->opcode != OP_DeferredSeek || o->p1 != idxCur || o->p3 < 0 || o->p3 >= 16 || !cur[o->p3].open || cur[o->p3].ki || o->p4type != P4_NOTUSED)
      QC_REFUSE("deferredseek");
    tabCur = o->p3; P.tabRoot = cur[tabCur].root; curRole[tabCur] = -3; pc++;
  } else if (o->opcode == OP_SeekRowid) {       /* rowid = ?param driver */
    if (!cur[o->p1].open || cur[o->p1].ki || (reg[o->p3].kind != S_PARAM && reg[o->p3].kind != S_INT)) QC_REFUSE("seekrowid driver");
    P.scan = 0; if (reg[o->p3].kind == S_PARAM) P.param = (int)reg[o->p3].v; else P.key = reg[o->p3].v; tabCur = o->p1; P.tabRoot = cur[tabCur].root; curRole[tabCur] = -3; haltPc = o->p2; pc++;
  } else QC_REFUSE("driver");
  if (haltPc < 0 || haltPc >= nOp || aOp[haltPc].opcode != OP_Halt || aOp[haltPc].p1 != 0 || aOp[haltPc].p2 != 0) QC_REFUSE("halt");
  int nextPc = -1;   /* where a skipped row continues: the Next of the scan, or Halt */
  for (; pc < endPreamble; pc++) {
    o = OPN(pc);
    switch (o->opcode) {
      case OP_Column: {
        if (o->p5 || o->p4type != P4_NOTUSED || o->p3 > v->nMem || o->p2 >= QC_MAXCOL) QC_REFUSE("column");
        int r = curRole[o->p1];
        if (r == -3) { reg[o->p3] = (QcSym){S_COL, 0, (i16)o->p2, 0}; P.tabCols |= 1u << o->p2; }
        else if (r == -2) { reg[o->p3] = (QcSym){S_COL, -1, (i16)o->p2, 0}; P.idxCols |= 1u << o->p2; }   /* a covered column */
        else if (r >= 0 && r < QC_MAXJOIN) { reg[o->p3] = (QcSym){S_COL, (i8)(1 + r), (i16)o->p2, 0}; P.joinCols[r] |= 1u << o->p2; }
        else QC_REFUSE("column cursor");
        break;
      }
      case OP_IdxRowid: case OP_Rowid: {
        int r = curRole[o->p1];
        if (!((o->opcode == OP_IdxRowid && r == -2) || (o->opcode == OP_Rowid && r == -3)) || o->p2 > v->nMem) QC_REFUSE("rowid");
        reg[o->p2] = (QcSym){S_ROWID, 0, 0, 0}; break;
      }
      case OP_SeekRowid: {                     /* inner join by rowid */
        QcSym k = reg[o->p3];
        if (!cur[o->p1].open || cur[o->p1].ki || curRole[o->p1] != -1 || P.njoin >= QC_MAXJOIN || k.kind != S_COL || k.src != 0 || k.real) QC_REFUSE("join");
        if (nextPc < 0) nextPc = o->p2; else if (nextPc != o->p2) QC_REFUSE("join target");
        P.join[P.njoin].root = cur[o->p1].root; P.join[P.njoin].keycol = k.col; curRole[o->p1] = P.njoin++;
        break;
      }
      case OP_BeginSubrtn: {                   /* scalar count(*) over an index equality on the row's rowid */
        int rRet = o->p2, b = pc;
        Op *q = &aOp[b];
        /* the exact shape SQLite emits for (SELECT count(*) FROM t WHERE t.c = outer.rowid) */
        int rRes = q[1].p2;
        if (b + 14 >= nOp) QC_REFUSE("subquery length");
        if (o->p1 != 0) QC_REFUSE("subquery shape 1");
        if (q[1].opcode != OP_Null) QC_REFUSE("subquery shape 2");
        if (q[1].p3 != rRes) QC_REFUSE("subquery shape 3");
        if (q[2].opcode != OP_Integer) QC_REFUSE("subquery shape 4");
        if (q[2].p1 != 1) QC_REFUSE("subquery shape 5");
        if (q[3].opcode != OP_Null) QC_REFUSE("subquery shape 6");
        if (q[3].p2 != q[3].p3) QC_REFUSE("subquery shape 7");
        if (q[4].opcode != OP_OpenRead) QC_REFUSE("subquery shape 8");
        if (q[4].p4type != P4_KEYINFO) QC_REFUSE("subquery shape 9");
        if (q[4].p3 != P.iDb) QC_REFUSE("subquery shape 10");
        if ((q[5].opcode != OP_IdxRowid && q[5].opcode != OP_Rowid)) QC_REFUSE("subquery shape 11");
        if (q[6].opcode != OP_SeekGE) QC_REFUSE("subquery shape 12");
        if (q[6].p1 != q[4].p1) QC_REFUSE("subquery shape 13");
        if (q[6].p3 != q[5].p2) QC_REFUSE("subquery shape 14");
        if (q[6].p4type != P4_INT32) QC_REFUSE("subquery shape 15");
        if (q[6].p4.i != 1) QC_REFUSE("subquery shape 16");
        if (q[7].opcode != OP_IdxGT) QC_REFUSE("subquery shape 17");
        if (q[7].p1 != q[4].p1) QC_REFUSE("subquery shape 18");
        if (q[7].p3 != q[5].p2) QC_REFUSE("subquery shape 19");
        if (q[7].p4.i != 1) QC_REFUSE("subquery shape 20");
        if (q[7].p2 != q[6].p2) QC_REFUSE("subquery shape 21");
        /* AggStep turns itself into AggStep1, with a context in P4, the first time it runs */
        if (!((q[8].opcode == OP_AggStep && q[8].p4type == P4_FUNCDEF) || (q[8].opcode == OP_AggStep1 && q[8].p4type == P4_FUNCCTX)))
          QC_REFUSE("subquery aggstep");
        if (q[8].p1 != 0) QC_REFUSE("subquery shape 24");
        if (q[8].p3 != q[3].p2) QC_REFUSE("subquery shape 25");
        if (q[9].opcode != OP_Next) QC_REFUSE("subquery shape 26");
        if (q[9].p1 != q[4].p1) QC_REFUSE("subquery shape 27");
        if (q[9].p2 != b + 7) QC_REFUSE("subquery shape 28");
        if (q[10].opcode != OP_AggFinal) QC_REFUSE("subquery shape 29");
        if (q[10].p1 != q[3].p2) QC_REFUSE("subquery shape 30");
        if (q[10].p2 != 0) QC_REFUSE("subquery shape 31");
        if (q[6].p2 != b + 10) QC_REFUSE("subquery shape 32");
        if (q[11].opcode != OP_Copy) QC_REFUSE("subquery shape 33");
        if (q[11].p1 != q[3].p2) QC_REFUSE("subquery shape 34");
        if (q[11].p2 != rRes) QC_REFUSE("subquery shape 35");
        if (q[11].p3 != 0) QC_REFUSE("subquery shape 36");
        if (q[12].opcode != OP_DecrJumpZero) QC_REFUSE("subquery shape 37");
        if (q[12].p1 != q[2].p2) QC_REFUSE("subquery shape 38");
        if (q[12].p2 != b + 13) QC_REFUSE("subquery shape 39");
        if (q[13].opcode != OP_Return) QC_REFUSE("subquery shape 40");
        if (q[13].p1 != rRet) QC_REFUSE("subquery shape 41");
        if (q[13].p3 != 1) QC_REFUSE("subquery shape 42");
        if (q[13].p2 != b + 1) QC_REFUSE("subquery shape 43");
        /* AggStep turns into AggStep1 (P4 a context) once it has run */
        FuncDef *f = q[8].opcode == OP_AggStep ? q[8].p4.pFunc : q[8].p4.pCtx->pFunc;
        if (sqlite3StrICmp(f->zName, "count") || f->nArg != 0 || q[8].p5 != 0) QC_REFUSE("not count(*)");
        QcSym key = reg[q[5].p2];
        if ((q[5].opcode == OP_IdxRowid && curRole[q[5].p1] != -2) || (q[5].opcode == OP_Rowid && curRole[q[5].p1] != -3)) QC_REFUSE("subquery key");
        (void)key;
        KeyInfo *ki = q[4].p4.pKeyInfo;
        if (P.ncnt >= QC_MAXCNT || ki->nKeyField < 1 || ki->aSortFlags[0] || (ki->aColl[0] && sqlite3StrICmp(ki->aColl[0]->zName, "BINARY")))
          QC_REFUSE("count index");
        P.cnt[P.ncnt].root = (Pgno)q[4].p2; P.cnt[P.ncnt].ki = ki;
        reg[rRes] = (QcSym){S_COUNT, (i8)P.ncnt, 0, 0}; P.ncnt++;
        pc = b + 13;
        break;
      }
      case OP_Copy: {
        if (o->p3 != 0 || o->p1 > v->nMem || o->p2 > v->nMem) QC_REFUSE("copy");
        reg[o->p2] = reg[o->p1]; break;
      }
      case OP_RealAffinity:   /* a REAL column: an integer stored for it reads as a float (the output stage converts) */
        if (o->p1 > v->nMem || reg[o->p1].kind != S_COL) QC_REFUSE("realaffinity");
        reg[o->p1].real = 1; break;
      case OP_ResultRow: {
        if (o->p2 > QC_MAXOUT) QC_REFUSE("too many columns");
        for (int k = 0; k < o->p2; k++) {
          QcSym s = reg[o->p1 + k];
          if (s.kind != S_ROWID && s.kind != S_COL && s.kind != S_COUNT) QC_REFUSE("output");
          P.out[k] = s;
        }
        P.nout = o->p2;
        /* after the row: scan -> DecrJumpZero limit -> Next idx -> loop; point -> Halt */
        Op *a = OPN(pc + 1);
        if (P.scan) {
          Op *n = OPN(pc + 2);
          if (a->opcode != OP_DecrJumpZero || a->p2 != haltPc || reg[a->p1].kind != S_INT || reg[a->p1].v < 1 || reg[a->p1].v > QC_MAXROWS ||
              n->opcode != OP_Next || n->p1 != idxCur || n->p2 != loopHead || pc + 3 != haltPc || (nextPc >= 0 && nextPc != pc + 2))
            QC_REFUSE("scan tail");
          P.limit = (int)reg[a->p1].v; limitReg = a->p1;
        } else {
          if (pc + 1 != haltPc || (nextPc >= 0 && nextPc != haltPc)) QC_REFUSE("point tail");
          P.limit = 1;
        }
        goto lifted;
      }
      default: QC_REFUSE("opcode");
    }
  }
  QC_REFUSE("no result row");
lifted:
  (void)limitReg;
  for (int j = 0; j < P.njoin; j++) if (P.join[j].keycol >= QC_MAXCOL) QC_REFUSE("key col");
  for (int c = 0; c < P.ncnt; c++) if (!(P.cnt[c].uk = sqlite3VdbeAllocUnpackedRecord(P.cnt[c].ki))) return 0;
  {   /* record shapes for the fast decoders (an unknown layout keeps the generic decoder) */
    Schema *sc = v->db->aDb[P.iDb].pSchema; Index *ix; Table *t;
    P.idxRowidField = -1;
    if (P.scan && (t = qc_find(sc, P.idxRoot, &ix)) && ix && ix->nColumn == ix->nKeyCol + 1 && ix->nColumn <= QC_MAXCOL &&
        ix->aiColumn[ix->nColumn - 1] == XN_ROWID) {
      qc_shape(&P.idxShape, t, ix, P.idxCols, ix->nColumn - 1);
      if (P.idxShape.ok) P.idxRowidField = ix->nColumn - 1;
    }
    if ((t = qc_find(sc, P.tabRoot, &ix)) && !ix) qc_shape(&P.tabShape, t, 0, P.tabCols, -1);
    for (int j = 0; j < P.njoin; j++) if ((t = qc_find(sc, P.join[j].root, &ix)) && !ix) qc_shape(&P.joinShape[j], t, 0, P.joinCols[j], -1);
    /* the unrolled decoder for each shape, where one was generated (up to 4 columns) */
    P.idxDec = P.idxRowidField >= 0 ? qc_dec_pick(&P.idxShape) : 0; P.tabDec = qc_dec_pick(&P.tabShape);
    /* widths: the widest column any decoder writes (generic: the highest needed one; unrolled: the shape's n) */
    #define QC_HI(m) (32 - __builtin_clz((m) | 1))
    P.idxW = QC_HI(P.idxCols); P.tabW = QC_HI(P.tabCols); if (P.tabShape.n > P.tabW) P.tabW = P.tabShape.n;
    for (int j = 0; j < P.njoin; j++) { P.joinW[j] = QC_HI(P.joinCols[j]); if (P.joinShape[j].n > P.joinW[j]) P.joinW[j] = P.joinShape[j].n; }
    for (int j = 0; j < P.njoin; j++) P.joinDec[j] = qc_dec_pick(&P.joinShape[j]);
    P.idxCode = P.idxDec ? qc_dec_code(&P.idxShape) : 0; P.tabCode = P.tabDec ? qc_dec_code(&P.tabShape) : 0;
    for (int j = 0; j < P.njoin; j++) P.joinCode[j] = P.joinDec[j] ? qc_dec_code(&P.joinShape[j]) : 0;
  }
  P.lanes = getenv("QC_LANES") != 0;   /* off: slower on the Xeon hot and cold (RESULTS 5e) */
  QcConn *C = QCD(v->db)->pQcConn; if (C->nplans >= QC_MAXPLANS) return 0;
  QcPlan *pp = sqlite3_malloc(sizeof P); if (!pp) return 0;
  *pp = P; QCV(v)->pQc = pp; QC_COUNT_ADD(C->nplans, pp);
  return 1;
}

/* ---------------------------------------------------------------- run time: records */
static inline u64 qc_varint(const u8 *p, int *n) {
  if (!(p[0] & 0x80)) { *n = 1; return p[0]; }
  if (!(p[1] & 0x80)) { *n = 2; return ((u64)(p[0] & 0x7f) << 7) | p[1]; }
  u64 x; *n = sqlite3GetVarint(p, &x); return x;
}
static inline __attribute__((always_inline)) u64 qc_be64(const u8 *d) {   /* a record's 8-byte big-endian field */
#if SQLITE_BYTEORDER == 1234
  u64 x; memcpy(&x, d, 8); return __builtin_bswap64(x);
#elif SQLITE_BYTEORDER == 4321
  u64 x; memcpy(&x, d, 8); return x;
#else
  u64 x = 0; for (int k = 0; k < 8; k++) x = (x << 8) | d[k]; return x;
#endif
}
/* field value from serial type t at d; 0 = a type the lifted program would not have handed out as is */
static inline __attribute__((always_inline)) int qc_value(u64 t, const u8 *d, QcVal *o) {
  switch (t) {
    case 0: o->type = SQLITE_NULL; return 1;
    case 1: o->type = SQLITE_INTEGER; o->i = (i8)d[0]; return 1;
    case 2: o->type = SQLITE_INTEGER; o->i = (i16)((d[0] << 8) | d[1]); return 1;
    case 3: o->type = SQLITE_INTEGER; o->i = ((int32_t)((u32)d[0] << 24 | (u32)d[1] << 16 | (u32)d[2] << 8)) >> 8; return 1;
    case 4: o->type = SQLITE_INTEGER; o->i = (int32_t)((u32)d[0] << 24 | (u32)d[1] << 16 | (u32)d[2] << 8 | d[3]); return 1;
    case 5: { u64 x = 0; for (int k = 0; k < 6; k++) x = (x << 8) | d[k]; o->type = SQLITE_INTEGER; o->i = (i64)(x ^ 0x800000000000ull) - (i64)0x800000000000ll; return 1; }
    case 6: o->type = SQLITE_INTEGER; o->i = (i64)qc_be64(d); return 1;
    case 7: { u64 x = qc_be64(d); double r; memcpy(&r, &x, 8);
              if (r != r) { o->type = SQLITE_NULL; return 1; }   /* sqlite3VdbeSerialGet: NaN reads as NULL */
              o->type = SQLITE_FLOAT; o->r = r; return 1; }
    case 8: o->type = SQLITE_INTEGER; o->i = 0; return 1;
    case 9: o->type = SQLITE_INTEGER; o->i = 1; return 1;
    case 10: case 11: return 0;
  }
  o->type = (t & 1) ? SQLITE_TEXT : SQLITE_BLOB; o->z = d; o->n = (int)((t - 12) >> 1); return 1;
}
static const u8 qc_stl[12] = {0, 1, 2, 3, 4, 6, 8, 8, 0, 0, 0, 0};
static inline u32 qc_stlen(u64 t) { return t < 12 ? qc_stl[t] : (u32)((t - 12) >> 1); }
/* decode the columns in mask from a table record (rec, sz all local); missing trailing columns are NULL (no defaults:
   the lifter refused Column with a P4 default). 0 = malformed or an unusable type. OP_Column's corruption checks, so
   a record SQLite calls malformed isn't read here (0: the statement runs in the interpreter, which reports it): a
   header size within the header, every type varint inside it and at most 0xffffffff, and once the header is read to
   its end, the fields ending exactly at the payload's end */
static int qc_decode(const u8 *rec, u32 sz, u32 mask, QcVal *cols) {
  int n; u64 hdr = qc_varint(rec, &n); const u8 *h = rec + n, *he = rec + hdr, *d = rec + hdr, *end = rec + sz;
  if (hdr > sz || (u64)n >= hdr || hdr > 0xffffffffu) return 0;
  for (int c = 0; mask; c++, mask >>= 1) {
    if (h >= he) { if (mask & 1) cols[c].type = SQLITE_NULL; continue; }
    u32 t = *h, len;
    if (t < 0x80) { h++; len = t < 12 ? qc_stl[t] : (t - 12) >> 1; }   /* (a one-byte type: most of them, and every one skipped over) */
    else { u64 w = qc_varint(h, &n); h += n; if (h > he || w > 0xffffffffu) return 0; t = (u32)w; len = qc_stlen(w); }
    if (mask & 1) { if (d + len > end || !qc_value(t, d, &cols[c])) return 0; }
    d += len;
  }
  return h < he ? d <= end : d == end;
}
/* qc_decode for a record of the expected shape: one pass over the header, no per-type dispatch beyond the class.
   0 = not that shape (a different type in some column, a long header, a short record): use qc_decode. The values are
   the ones qc_decode would give for the same record. */
static const u8 qc_iw[10] = {0, 1, 2, 3, 4, 6, 8, 0, 0, 0};
static const u8 qc_ikw[256] = {[1] = 2, [2] = 3, [3] = 4, [4] = 5, [5] = 7, [6] = 9, [8] = 1, [9] = 1};   /* integer serial types: width + 1 */
/* a big-endian integer of serial type t (1-6, 8, 9), inline */
static inline __attribute__((always_inline)) i64 qc_int(u32 t, const u8 *d) {
  switch (t) {
    case 1: return (i8)d[0];
    case 2: return (i16)((d[0] << 8) | d[1]);
    case 3: return ((int32_t)((u32)d[0] << 24 | (u32)d[1] << 16 | (u32)d[2] << 8)) >> 8;
    case 4: return (int32_t)((u32)d[0] << 24 | (u32)d[1] << 16 | (u32)d[2] << 8 | d[3]);
    case 8: return 0;
    case 9: return 1;
    case 5: return (i64)(i16)((d[0] << 8) | d[1]) * 4294967296ll + (i64)((u32)d[2] << 24 | (u32)d[3] << 16 | (u32)d[4] << 8 | d[5]);   /* (two loads, as
                                     vdbe.c's serial get: the byte loop was 9 % of a region reading 48-bit timestamps, bench exp1239) */
    default: return (i64)qc_be64(d);
  }
}
static inline __attribute__((always_inline)) int qc_decode_fast(const u8 *rec, u32 sz, const QcShape *sh, u32 mask, QcVal *cols) {
  u32 hl = rec[0]; if (hl >= 0x80 || hl > sz) return 0;
  const u8 *h = rec + 1, *he = rec + hl, *d = rec + hl;
  for (int c = 0; c < sh->n; c++, mask >>= 1) {
    if (h >= he) return 0;
    u32 t = *h++;
    switch (sh->cls[c]) {
      case K_NUL:
        if (t) return 0;
        if (mask & 1) cols[c].type = SQLITE_NULL;
        break;
      case K_INT:
        if (t == 0 || t == 7 || t > 9) return 0;
        if (mask & 1) { cols[c].type = SQLITE_INTEGER; cols[c].i = qc_int(t, d); }
        d += qc_iw[t];
        break;
      case K_REAL:   /* a float, or an integer stored for an integral value */
        if (t == 7) {
          if (mask & 1) { u64 x = qc_be64(d); double r; memcpy(&r, &x, 8); if (r != r) return 0; cols[c].type = SQLITE_FLOAT; cols[c].r = r; }   /* (NaN: qc_decode's NULL) */
          d += 8;
        } else {
          if (t == 0 || t > 9) return 0;
          if (mask & 1) { cols[c].type = SQLITE_INTEGER; cols[c].i = qc_int(t, d); }
          d += qc_iw[t];
        }
        break;
      default: {   /* K_TXT */
        if (t >= 0x80) { if (h >= he || (*h & 0x80)) return 0; t = ((t & 0x7f) << 7) | *h++; }
        if (t < 13 || !(t & 1)) return 0;
        u32 len = (t - 13) >> 1;
        if (mask & 1) { cols[c].type = SQLITE_TEXT; cols[c].z = d; cols[c].n = (int)len; }
        d += len;
      }
    }
  }
  return h < he ? d <= rec + sz : d == rec + sz;   /* (qc_decode's end check: a header read to its end accounts for the payload exactly) */
}

/* straight-line decoders per shape signature (qc/gen_decoders.py, generated at build time) */
#include "qc_dec.h"

/* the first field of an index cell, compared as SQLite compares it with an integer key: kind 1 = numerically an integer
   (*key; integral REALs compare equal to integers), 0 = never equal but sorts among the numbers (NULL, non-integral
   REAL: *key = floor for the range test, or INT64_MIN for NULL), 2 = sorts after every number (TEXT/BLOB), -1 = malformed */
/* the first field of the index cell at c, which lies in the page's cell content area (ce: the end of its usable
   part); see qc_idxkey */
static inline __attribute__((always_inline)) int qc_idxkey_at(const MemPage *pg, const u8 *c, const u8 *ce, i64 *key) {
  if (!pg->leaf) { c += 4; if (c + 3 > ce) return -1; }   /* (a leaf cell's first 4 bytes are inside: the caller checked c <= ce - 4) */
  /* the common leaf cell of an integer index: [size < 128][header size < 128][int type][...][key] */
  u32 sz0 = c[0], hl = c[1], w = qc_ikw[c[2]];   /* w: an integer serial type's width + 1, 0 for any other type */
  if (__builtin_expect(((sz0 | hl) & 0x80) == 0 && w && hl >= 2 && hl + w - 1 <= sz0 && c + 1 + sz0 <= ce, 1)) {
    *key = qc_int(c[2], c + 1 + hl); return 1;
  }
  int n; u64 sz = qc_varint(c, &n); c += n;
  if (sz > pg->maxLocal || c + sz > ce) return -1;
  u64 hdr = qc_varint(c, &n); u64 t = qc_varint(c + n, &n);
  if (hdr > sz) return -1;
  if (t == 0) { *key = INT64_MIN; return 0; }
  if (t >= 12) return 2;
  if (t == 10 || t == 11) return -1;
  QcVal v; qc_value(t, c + hdr, &v);
  if (v.type == SQLITE_INTEGER) { *key = v.i; return 1; }
  if (v.type == SQLITE_NULL) { *key = INT64_MIN; return 0; }
  double r = v.r;
  if (r >= 9223372036854775808.0) { *key = INT64_MAX; return 0; }
  if (r < -9223372036854775808.0) { *key = INT64_MIN; return 0; }
  i64 f = (i64)r; if ((double)f > r) f--;
  *key = f; return (double)f == r ? 1 : 0;
}
/* the cell must lie in the page's cell content area (btreeCellSizeCheck's bounds); a corrupt page goes to SQLite's
   own code, which reports it */
static inline __attribute__((always_inline)) const u8 *qc_cell_lo(const MemPage *pg) { return pg->aCellIdx + 2 * pg->nCell; }
static inline __attribute__((always_inline)) const u8 *qc_cell_end(const MemPage *pg) { return pg->aData + pg->pBt->usableSize; }   /* (not aDataEnd: reserved bytes are no cell's) */
static inline __attribute__((always_inline)) int qc_idxkey(const MemPage *pg, int ix, i64 *key) {
  const u8 *c = pg->aData + get2byteAligned(&pg->aCellIdx[2 * ix]), *ce = qc_cell_end(pg);
  if (c < qc_cell_lo(pg) || c > ce - 4) return -1;
  return qc_idxkey_at(pg, c, ce, key);
}

/* ---------------------------------------------------------------- run time: batched rowid seeks (from explore/sqlite-simd) */
typedef struct { MemPage *leaf; int ix; int found; } qc_res;
#define QC_PF(p) __builtin_prefetch((const void *)(p), 0, 3)
/* sqlite3VarintValue's own algorithm, inlined here: a call from qc as well made gcc stop inlining SQLite's copy into
   sqlite3BtreeTableMoveto, its one caller (+90 instructions per point lookup through SQLite's seek) */
static inline __attribute__((always_inline)) i64 qc_varint_value(const u8 *p) {
  u64 k = p[0]; u8 x;
  if (k < 0x80) return (i64)k;
  k = (k << 7) ^ (x = *++p);
  if (x < 0x80) return (i64)(k ^ 0x4000);
  k = (k << 7) ^ (x = *++p);
  if (x < 0x80) return (i64)(k ^ 0x204000);
  k = (k << 7) ^ 0x10204000 ^ (x = *++p);
  for (int j = 0; j < 4 && x >= 0x80; j++) k = (k << 7) ^ 0x4000 ^ (x = *++p);
  if (x >= 0x80) k = (k << 8) ^ 0x8000 ^ (*++p);
  return (i64)k;
}
static inline i64 qc_cellkey(MemPage *pg, int i) {
  u8 *c = findCellPastPtr(pg, i);
  if (pg->intKeyLeaf) while (0x80 <= *(c++)) {}
  return qc_varint_value(c);
}
/* first cell with key >= k (nCell if none). Interpolation first: dense rowids put k almost where a linear guess says,
   so 6-10 binary-search probes become 1-3 (from engine.c's ms_lower_bound). */
static inline int qc_lower_bound(MemPage *pg, i64 k) {
  int n = pg->nCell, lo = 0, len = n;
  if (n >= 8) {
    i64 k0 = qc_cellkey(pg, 0), kn = qc_cellkey(pg, n - 1);
    if (k <= k0) return 0;
    if (k > kn) return n;
    int g = (int)((double)(k - k0) * (n - 1) / (double)(kn - k0));
    if (g < 1) g = 1; if (g > n - 1) g = n - 1;
    if (qc_cellkey(pg, g) >= k) {
      for (int s = 0; s < 4; s++) { if (qc_cellkey(pg, g - 1) < k) return g; g--; }
      lo = 1; len = g;
    } else {
      for (int s = 0; s < 4; s++) { g++; if (qc_cellkey(pg, g) >= k) return g; }
      lo = g + 1; len = n - 1 - g;
    }
  }
  while (len > 0) { int h = len >> 1; if (qc_cellkey(pg, lo + h) < k) { lo += h + 1; len -= h + 1; } else len = h; }
  return lo;
}
static inline PgHdr1 **qc_bucket(BtShared *bt, Pgno pg) { PCache1 *pc = (PCache1 *)bt->pPager->pPCache->pCache; return &pc->apHash[pg % pc->nHash]; }
QC_HOT static int qc_seek_batch(Btree *p, Pgno root, const i64 *keys_in, int n, qc_res *r) {
  BtShared *bt = p->pBt; MemPage *cur[QC_MAXROWS]; Pgno ch[QC_MAXROWS]; int rc, ord[QC_MAXROWS]; i64 keys[QC_MAXROWS];
  int pcache = bt->pPager->xGet == getPageNormal;   /* the prefetch peeks at pcache1's hash: not with mmap */
  for (int i = 0; i < n; i++) { int j = i; while (j > 0 && keys_in[ord[j - 1]] > keys_in[i]) { ord[j] = ord[j - 1]; j--; } ord[j] = i; }
  for (int i = 0; i < n; i++) keys[i] = keys_in[ord[i]];
  if ((rc = getAndInitPage(bt, root, &cur[0], PAGER_GET_READONLY))) return rc;
  if (cur[0]->nCell == 0 && cur[0]->leaf) { for (int i = 0; i < n; i++) { r[i].leaf = 0; r[i].found = 0; } releasePage(cur[0]); return SQLITE_OK; }
  for (int i = 1; i < n; i++) { sqlite3PagerRef(cur[0]->pDbPage); cur[i] = cur[0]; }
  for (int depth = 0; !cur[0]->leaf; depth++) {
    if (depth > 20) { rc = SQLITE_CORRUPT_BKPT; goto fail; }
    for (int i = 0; i < n; i++) {
      MemPage *pg = cur[i]; int lb = qc_lower_bound(pg, keys[i]);
      ch[i] = lb >= pg->nCell ? get4byte(&pg->aData[pg->hdrOffset + 8]) : get4byte(findCell(pg, lb));
      if (pcache && (!i || ch[i] != ch[i - 1])) QC_PF(qc_bucket(bt, ch[i]));
    }
    if (pcache) for (int i = 0; i < n; i++) {
      if (i && ch[i] == ch[i - 1]) continue;
      PgHdr1 *h = *qc_bucket(bt, ch[i]); while (h && h->iKey != ch[i]) h = h->pNext;
      if (h) { u8 *d = h->page.pBuf; QC_PF(d); QC_PF(d + 64); QC_PF(h->page.pExtra); }
    }
    for (int i = 0; i < n; i++) { releasePage(cur[i]); cur[i] = 0; }
    for (int i = 0; i < n; i++) {
      if (i && ch[i] == ch[i - 1]) { sqlite3PagerRef(cur[i - 1]->pDbPage); cur[i] = cur[i - 1]; continue; }
      if ((rc = getAndInitPage(bt, ch[i], &cur[i], PAGER_GET_READONLY))) goto fail;
      if (cur[i]->nCell < 1 || cur[i]->intKey != 1 || cur[i]->leaf != cur[0]->leaf) { rc = SQLITE_CORRUPT_PGNO(ch[i]); goto fail; }
    }
  }
  for (int i = 0; i < n; i++) {
    MemPage *pg = cur[i]; int lb = qc_lower_bound(pg, keys[i]), found = 0;
    if (lb < pg->nCell) found = qc_cellkey(pg, lb) == keys[i];
    qc_res *o = &r[ord[i]]; o->leaf = pg; o->ix = lb; o->found = found;
  }
  return SQLITE_OK;
fail:
  for (int i = 0; i < n; i++) if (cur[i]) releasePage(cur[i]);
  return rc;
}

/* ---------------------------------------------------------------- run time: the plan */
/* the plan's cursor for root: kept open (and positioned) from the last execution in this transaction, or opened now */
static BtCursor *qc_cursor(QcPlan *P, Btree *bt, Pgno root, KeyInfo *ki, BtCursor **slot, u8 *open) {
  if (*open) return *slot;
  if (!*slot && !(*slot = sqlite3MallocZero(sqlite3BtreeCursorSize()))) return 0;
  sqlite3BtreeCursorZero(*slot);
  if (sqlite3BtreeCursor(bt, root, 0, ki, *slot) != SQLITE_OK) return 0;
  *open = 1; P->bt = bt; return *slot;
}
typedef struct QcRun {
  DbPage *pins[QC_MAXROWS]; int npin;
  qc_res jr[QC_MAXJOIN][QC_MAXROWS]; int jn[QC_MAXJOIN];
  u32 counts[QC_RANGE_MAX];
  /* decoded values: here rather than on the stack (~200 KB: stack-clash probing alone cost ~0.3 µs per call on the Xeon) */
  /* rows of each source at the plan's own width (its widest decoded column), so a request touches few cache lines */
  QcVal ivf[QC_MAXROWS * QC_MAXCOL], tvf[QC_MAXROWS * QC_MAXCOL], jvf[QC_MAXJOIN][QC_MAXROWS * QC_MAXCOL], outv[QC_MAXROWS * QC_MAXOUT];
  i64 cnt[QC_MAXCNT][QC_MAXROWS];
  QcVal rv[QC_MAXROWS][1], cv[QC_MAXCNT][QC_MAXROWS][1];   /* rowids and counts as values, for the flat writer */
} QcRun;
static QcRun *qc_run_alloc(QcConn *C) { if (!C->run) C->run = sqlite3MallocZero(sizeof *C->run); return C->run; }

/* count of index entries equal to integer key, for keys[] (n rows); 0 ok */
/* one key (GET /posts/:id): the seek and the walk, nothing else. 0 ok, 1 = take the fallback */
static inline __attribute__((always_inline)) int qc_count1(Btree *bt, QcPlan *P, int c, i64 key, i64 *out) {
  BtCursor *cur = P->oc[c] ? P->cc[c] : qc_cursor(P, bt, P->cnt[c].root, P->cnt[c].ki, &P->cc[c], &P->oc[c]); if (!cur) return 1;
  UnpackedRecord *uk = P->cnt[c].uk;
  uk->nField = 1; uk->default_rc = +1; uk->eqSeen = 0; uk->errCode = 0; uk->aMem[0].flags = MEM_Int; uk->aMem[0].u.i = key;
  int res; if (sqlite3BtreeIndexMoveto(cur, uk, &res) != SQLITE_OK) return 1;
  i64 cnt = 0;
  if (cur->eState == CURSOR_VALID) {
    if (res < 0) { int r = sqlite3BtreeNext(cur, 0); if (r == SQLITE_DONE) { *out = 0; return 0; } if (r) return 1; }
    for (;;) {
      MemPage *pg = cur->pPage; int ix = cur->ix, end = pg->leaf ? pg->nCell : ix + 1;
      for (; ix < end; ix++) {
        i64 k; int kind = qc_idxkey(pg, ix, &k);
        if (kind < 0) return 1;
        if (kind == 2 || k > key) { *out = cnt; return 0; }
        cnt += kind == 1 && k == key;
      }
      if (pg->leaf) cur->ix = (u16)(pg->nCell - 1);
      int r = sqlite3BtreeNext(cur, 0);
      if (r == SQLITE_DONE) break;
      if (r) return 1;
    }
  }
  *out = cnt; return 0;
}
QC_HOT static int qc_counts(QcRun *R, Btree *bt, QcPlan *P, int c, const i64 *keys, int n, i64 *out) {
  i64 lo = INT64_MAX, hi = INT64_MIN;
  for (int i = 0; i < n; i++) { if (keys[i] < lo) lo = keys[i]; if (keys[i] > hi) hi = keys[i]; }
  int ranged = n > 1 && (u64)(hi - lo) < QC_RANGE_MAX;   /* one key: counted directly */
  BtCursor *cur = qc_cursor(P, bt, P->cnt[c].root, P->cnt[c].ki, &P->cc[c], &P->oc[c]); if (!cur) return 1;
  UnpackedRecord *uk = P->cnt[c].uk; int rc = 0;
  for (int pass = 0; pass < (ranged ? 1 : n); pass++) {
    i64 a = ranged ? lo : keys[pass], b = ranged ? hi : keys[pass];
    if (ranged) memset(R->counts, 0, sizeof(u32) * (size_t)(hi - lo + 1));
    uk->nField = 1; uk->default_rc = +1; uk->eqSeen = 0; uk->errCode = 0; uk->aMem[0].flags = MEM_Int; uk->aMem[0].u.i = a;
    int res; if (sqlite3BtreeIndexMoveto(cur, uk, &res) != SQLITE_OK) { rc = 1; break; }
    i64 cnt = 0;
    if (cur->eState == CURSOR_VALID) {
      if (res < 0 && (rc = sqlite3BtreeNext(cur, 0)) != SQLITE_OK) { if (rc == SQLITE_DONE) rc = 0; else { rc = 1; break; } goto done1; }
      for (;;) {
        MemPage *pg = cur->pPage; int ix = cur->ix, stop = 0, end = pg->leaf ? pg->nCell : ix + 1;   /* interior: this one cell */
        for (; ix < end; ix++) {
          i64 k; int kind = qc_idxkey(pg, ix, &k);
          if (kind < 0) { rc = 1; stop = 1; break; }
          if (kind == 2 || k > b) { stop = 1; break; }
          if (kind == 1 && k >= a) { if (ranged) R->counts[k - lo]++; else cnt++; }
        }
        if (stop) break;
        if (pg->leaf) cur->ix = (u16)(pg->nCell - 1);
        int r2 = sqlite3BtreeNext(cur, 0);
        if (r2 == SQLITE_DONE) break;
        if (r2) { rc = 1; break; }
      }
      if (rc) break;
    }
  done1:
    if (!ranged) out[pass] = cnt;
  }
  if (!rc && ranged) for (int i = 0; i < n; i++) out[i] = R->counts[keys[i] - lo];
  return rc;
}

/* ---------------------------------------------------------------- JSON output: the plan writes the response body itself
** A template is the body's prefix and suffix, the separator between rows, and per row the literal fragments around the
** columns: frag[0] col0 frag[1] col1 ... frag[n]. A column is 'i' (an INTEGER, written in decimal) or 's' (TEXT, written
** as a JSON string with JSON.stringify's escapes). Anything else (NULL, REAL, BLOB, an INTEGER in an 's' column) returns
** QC_FALLBACK before a byte is written, so the caller's own formatting decides those. */
typedef struct QcJson { const char *open, *close, *sep; int ncol; const char *kind; const char *frag[QC_MAXOUT + 1]; } QcJson;
/* fragments live in 32-byte zero-padded slots: each is copied with two fixed 16-byte moves (no memcpy call) */
#define QC_FRAG 32
typedef struct QcTmpl { char kind[QC_MAXOUT]; int ncol; char open[QC_FRAG], close[QC_FRAG], sep[QC_FRAG], frag[QC_MAXOUT + 1][QC_FRAG];
                        u32 lopen, lclose, lsep, lfrag[QC_MAXOUT + 1], lrow; } QcTmpl;
static int qc_slot(char *dst, u32 *len, const char *src) {
  size_t n = strlen(src); if (n > QC_FRAG) return 0;
  memset(dst, 0, QC_FRAG); memcpy(dst, src, n); *len = (u32)n; return 1;
}
static inline char *qc_frag(char *p, const char *slot, u32 len) {   /* writes 32 bytes, advances len (<= 32) */
  memcpy(p, slot, 16); memcpy(p + 16, slot + 16, 16); return p + len;
}
static int qc_plan_nout(const void *plan);
static void qc_free(Vdbe *v);
SQLITE_API int sqlite3_qc_json(sqlite3_stmt *pStmt, const QcJson *j) {
  Vdbe *v = (Vdbe *)pStmt;
#if QC_STOCK
  if (!v || !v->db) return 0;
  sqlite3_mutex_enter(v->db->mutex); QcVs *vs = qc_vs_enter(v); sqlite3_mutex_leave(v->db->mutex); if (!vs) return 0;
#endif
  void *P = v ? QCV(v)->pQc : 0;
  if (!P || j->ncol != qc_plan_nout(P) || j->ncol > QC_MAXOUT) return 0;
  sqlite3QcFree = qc_free;   /* (frees the template with the handle) */
  QcTmpl *t = QCV(v)->pQcTmpl ? QCV(v)->pQcTmpl : sqlite3_malloc(sizeof *t); if (!t) return 0;   /* on the handle: survives re-prepares */
  t->ncol = j->ncol; t->lrow = 0;
  int ok = qc_slot(t->open, &t->lopen, j->open) && qc_slot(t->close, &t->lclose, j->close) && qc_slot(t->sep, &t->lsep, j->sep);
  for (int k = 0; ok && k <= j->ncol; k++) { ok = qc_slot(t->frag[k], &t->lfrag[k], j->frag[k]); t->lrow += t->lfrag[k]; }
  if (!ok) { if (t != QCV(v)->pQcTmpl) sqlite3_free(t); return 0; }
  for (int k = 0; k < j->ncol; k++) { if (j->kind[k] != 'i' && j->kind[k] != 's') { sqlite3_free(t); return 0; } t->kind[k] = j->kind[k]; }
  QCV(v)->pQcTmpl = t; return 1;
}
static const char QC_HEX[] = "0123456789abcdef";
static inline char *qc_esc1(char *o, unsigned char c) {   /* JSON.stringify's escapes */
  switch (c) {
    case '"': o[0] = '\\'; o[1] = '"'; return o + 2;
    case '\\': o[0] = '\\'; o[1] = '\\'; return o + 2;
    case '\b': o[0] = '\\'; o[1] = 'b'; return o + 2;
    case '\f': o[0] = '\\'; o[1] = 'f'; return o + 2;
    case '\n': o[0] = '\\'; o[1] = 'n'; return o + 2;
    case '\r': o[0] = '\\'; o[1] = 'r'; return o + 2;
    case '\t': o[0] = '\\'; o[1] = 't'; return o + 2;
    default: memcpy(o, "\\u00", 4); o[4] = QC_HEX[c >> 4]; o[5] = QC_HEX[c & 15]; return o + 6;
  }
}
static inline char *qc_escape(char *o, const u8 *s, u32 n) {
  for (u32 i = 0; i < n; i++) { unsigned c = s[i]; if (c < 0x20 || c == '"' || c == '\\') o = qc_esc1(o, (unsigned char)c); else *o++ = (char)c; }
  return o;
}
/* the string at s as a JSON string. vec: s may be read up to 15 bytes past its end (pcache page or scratch); the copy
   is 16 bytes at a time with the escape mask OR-ed up, and a field that needs escaping is rewritten. Writes up to 15
   bytes past the result (the caller keeps 32 bytes of slack). */
#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
typedef uint8x16_t qc_v16;
static inline qc_v16 qc_ld(const u8 *p) { return vld1q_u8(p); }
static inline void qc_st(char *p, qc_v16 v) { vst1q_u8((uint8_t *)p, v); }
static inline uint64_t qc_mask(qc_v16 v) {
  uint8x16_t m = vorrq_u8(vorrq_u8(vceqq_u8(v, vdupq_n_u8('"')), vceqq_u8(v, vdupq_n_u8('\\'))), vcltq_u8(v, vdupq_n_u8(0x20)));
  return vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(m), 4)), 0);
}
#define QC_LANE 4
#define QC_VEC 1
#elif defined(__SSE2__)
#include <emmintrin.h>
typedef __m128i qc_v16;
static inline qc_v16 qc_ld(const u8 *p) { return _mm_loadu_si128((const __m128i *)p); }
static inline void qc_st(char *p, qc_v16 v) { _mm_storeu_si128((__m128i *)p, v); }
static inline uint64_t qc_mask(qc_v16 v) {
  __m128i m = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(v, _mm_set1_epi8('"')), _mm_cmpeq_epi8(v, _mm_set1_epi8('\\'))),
                           _mm_cmpeq_epi8(_mm_min_epu8(v, _mm_set1_epi8(0x1f)), v));
  return (uint64_t)(unsigned)_mm_movemask_epi8(m);
}
#define QC_LANE 1
#define QC_VEC 1
#else
#define QC_VEC 0
#endif
static inline __attribute__((always_inline)) char *qc_jstr(char *p, const u8 *s, u32 n, int vec) {
  *p++ = '"';
#if QC_VEC
  if (vec) {
    uint64_t acc = 0; u32 i = 0;
    for (; i + 16 <= n; i += 16) { qc_v16 x = qc_ld(s + i); qc_st(p + i, x); acc |= qc_mask(x); }
    if (i < n) { qc_v16 x = qc_ld(s + i); qc_st(p + i, x); acc |= qc_mask(x) & ((1ull << (QC_LANE * (n - i))) - 1); }
    p = acc ? qc_escape(p, s, n) : p + n;
    *p++ = '"'; return p;
  }
#endif
  (void)vec; p = qc_escape(p, s, n); *p++ = '"'; return p;
}
static const char QC_D2[] = "00010203040506070809101112131415161718192021222324252627282930313233343536373839"
                            "40414243444546474849505152535455565758596061626364656667686970717273747576777879"
                            "8081828384858687888990919293949596979899";
static inline __attribute__((always_inline)) char *qc_jint(char *p, i64 v) {
  u64 u; if (v < 0) { *p++ = '-'; u = (u64)0 - (u64)v; } else u = (u64)v;
  char t[40]; int k = 20;   /* digits end at t[20]; the copy moves a fixed 20 bytes into the output's slack */
  while (u >= 100) { u64 q = u / 100; k -= 2; memcpy(t + k, QC_D2 + 2 * (u - q * 100), 2); u = q; }
  if (u >= 10) { k -= 2; memcpy(t + k, QC_D2 + 2 * u, 2); } else t[--k] = (char)('0' + u);
  memcpy(p, t + k, 16); memcpy(p + 16, t + k + 16, 4); return p + 20 - k;
}


/* QC_ORACLE (an experiment, not a mode): the feed/get plan's decoders called directly and its writer unrolled with
   literal keys, i.e. what per-plan code generation would produce for exactly this plan, to measure its ceiling. A
   plan of any other shape takes the normal path. */
static inline __attribute__((always_inline)) int qc_dec_idx(QcPlan *P, const u8 *rec, u32 sz, QcVal *tmp) {
#ifdef QC_ORACLE
  if (P->idxDec == qc_dec_TII) return qc_dec_TII(rec, sz, tmp);
#endif
  return P->idxDec ? P->idxDec(rec, sz, tmp) : qc_decode_fast(rec, sz, &P->idxShape, P->idxCols | (1u << P->idxRowidField), tmp);
}
#ifdef QC_ORACLE
#define QC_DEC_TAB(rec, sz, out) (P->tabDec == qc_dec_NIT ? qc_dec_NIT(rec, sz, out) : P->tabDec && P->tabDec(rec, sz, out))
#define QC_DEC_JOIN(j, rec, sz, out) (P->joinDec[j] == qc_dec_NT ? qc_dec_NT(rec, sz, out) : P->joinDec[j] && P->joinDec[j](rec, sz, out))
#else
#define QC_DEC_TAB(rec, sz, out) (P->tabDec && P->tabDec(rec, sz, out))
#define QC_DEC_JOIN(j, rec, sz, out) (P->joinDec[j] && P->joinDec[j](rec, sz, out))
#endif

/* ---------------------------------------------------------------- run time: overlapped descents ("lanes")
** One level-by-level pass over several B-tree descents at once: every lane's page is searched, every next child page is
** prefetched (pcache hash bucket, then the page and its header), and only then are the children fetched. Misses of
** independent descents (the post rows and the likes index in GET /posts/:id, the 20 post rows and the likes range in the
** feed) overlap instead of queueing. A table lane ends at the leaf cell of its rowid; an index lane keeps its whole path,
** from which qc_idx_walk counts entries in key order without a BtCursor. */
#define QC_MAXDEPTH 20
#define QC_MAXLANE (QC_MAXROWS + QC_MAXCNT)
typedef struct QcLane {
  Pgno root; u8 isIdx, found, done; i64 key;
  int depth;                                   /* index lanes: pg[0] root .. pg[depth-1] leaf; table lanes: pg[0] only */
  MemPage *pg[QC_MAXDEPTH]; int ix[QC_MAXDEPTH];   /* ix: the child taken at each interior level, the position at the leaf */
  Pgno ch;                                     /* the next child (during the descent) */
} QcLane;
static QcLane *qc_lanes(QcConn *C) { if (!C->lanes) C->lanes = sqlite3_malloc64(sizeof(QcLane) * QC_MAXLANE); return C->lanes; }
/* sign of (the first field of index cell i) - key, as SQLite orders values; 2 = a cell we do not handle */
/* a cell's first field (qc_idxkey's kind and value) against an integer key: -1 0 1, or 2 for a cell not handled */
static inline __attribute__((always_inline)) int qc_idx_cmp_kind(int kind, i64 k, i64 key) {
  if (kind < 0) return 2;
  if (kind == 2) return 1;                                   /* TEXT/BLOB: after every number */
  if (kind == 1) return k < key ? -1 : k > key;
  if (k == INT64_MIN) return -1;                             /* NULL (or a REAL below every integer) */
  return k >= key ? 1 : -1;                                  /* a non-integral REAL r: r > key iff floor(r) >= key */
}
static inline int qc_idx_cmp(const MemPage *pg, int i, i64 key) { i64 k; int kind = qc_idxkey(pg, i, &k); return qc_idx_cmp_kind(kind, k, key); }
/* first cell whose first field is >= key (nCell if none); -1 on a cell we do not handle */
static inline int qc_idx_lb(const MemPage *pg, i64 key) {
  int lo = 0, len = pg->nCell;
  while (len > 0) {
    int h = len >> 1, c = qc_idx_cmp(pg, lo + h, key);
    if (c == 2) return -1;
    if (c < 0) { lo += h + 1; len -= h + 1; } else len = h;
  }
  return lo;
}
static inline Pgno qc_child(const MemPage *pg, int i) { return i >= pg->nCell ? get4byte(&pg->aData[pg->hdrOffset + 8]) : get4byte(findCell(pg, i)); }
static void qc_lanes_release(QcLane *L, int n) {
  for (int l = 0; l < n; l++) { for (int d = 0; d < L[l].depth; d++) if (L[l].pg[d]) releasePage(L[l].pg[d]); L[l].depth = 0; }
}
/* run n lanes to their leaves; SQLITE_OK, or an error / an unexpected page (the caller falls back) */
static int qc_descend(BtShared *bt, QcLane *L, int n) {
  int pcache = bt->pPager->xGet == getPageNormal, rc;
  for (int l = 0; l < n; l++) {
    L[l].depth = 0; L[l].found = 0; L[l].done = 0; L[l].ch = 0;
    if (l && L[l - 1].root == L[l].root && L[l - 1].depth) { sqlite3PagerRef(L[l - 1].pg[0]->pDbPage); L[l].pg[0] = L[l - 1].pg[0]; }
    else if ((rc = getAndInitPage(bt, L[l].root, &L[l].pg[0], PAGER_GET_READONLY))) { qc_lanes_release(L, l); return rc; }
    L[l].depth = 1;
    if (L[l].pg[0]->intKey != !L[l].isIdx) { qc_lanes_release(L, l + 1); return SQLITE_CORRUPT_BKPT; }
  }
  for (int round = 0;; round++) {
    int active = 0;
    if (round > QC_MAXDEPTH - 2) { qc_lanes_release(L, n); return SQLITE_CORRUPT_BKPT; }
    for (int l = 0; l < n; l++) {          /* 1. search this level; leaves finish here */
      QcLane *x = &L[l]; MemPage *pg = x->pg[x->depth - 1];
      if (x->done) { x->ch = 0; continue; }
      int lb = x->isIdx ? qc_idx_lb(pg, x->key) : qc_lower_bound(pg, x->key);
      if (lb < 0) { qc_lanes_release(L, n); return SQLITE_CORRUPT_BKPT; }
      x->ix[x->depth - 1] = lb;
      if (pg->leaf) { if (!x->isIdx) x->found = lb < pg->nCell && qc_cellkey(pg, lb) == x->key; x->ch = 0; x->done = 1; continue; }
      x->ch = qc_child(pg, lb); active++;
      if (pcache && (!l || x->ch != L[l - 1].ch)) QC_PF(qc_bucket(bt, x->ch));
    }
    if (!active) return SQLITE_OK;
    if (pcache) for (int l = 0; l < n; l++) {   /* 2. the children's pages and headers */
      if (!L[l].ch || (l && L[l].ch == L[l - 1].ch)) continue;
      PgHdr1 *h = *qc_bucket(bt, L[l].ch); while (h && h->iKey != L[l].ch) h = h->pNext;
      if (h) { u8 *d = h->page.pBuf; QC_PF(d); QC_PF(d + 64); QC_PF(h->page.pExtra); }
    }
    for (int l = 0; l < n; l++) {          /* 3. fetch (shared with the previous lane when it took the same child) */
      QcLane *x = &L[l]; if (!x->ch) continue;
      MemPage *c;
      if (l && L[l - 1].ch == x->ch && L[l - 1].root == x->root) { c = L[l - 1].pg[L[l - 1].depth - 1]; sqlite3PagerRef(c->pDbPage); }
      else if ((rc = getAndInitPage(bt, x->ch, &c, PAGER_GET_READONLY))) { qc_lanes_release(L, n); return rc; }
      if (c->intKey != !x->isIdx || c->nCell < (x->isIdx ? 0 : 1)) { releasePage(c); qc_lanes_release(L, n); return SQLITE_CORRUPT_PGNO(x->ch); }
      if (x->isIdx) { if (x->depth >= QC_MAXDEPTH) { releasePage(c); qc_lanes_release(L, n); return SQLITE_CORRUPT_BKPT; } x->pg[x->depth++] = c; }
      else { releasePage(x->pg[0]); x->pg[0] = c; }   /* a table lane needs only its current page */
    }
  }
}
/* count the entries of an index lane, from its position on, whose first field is an integer in [lo, hi]: counts[k - lo]
   (counts != 0) or *one. Stops at the first entry above hi. 0 ok, 1 = a cell or page we do not handle */
static int qc_idx_walk(BtShared *bt, QcLane *x, i64 lo, i64 hi, u32 *counts, i64 *one) {
  int d = x->depth - 1, ix = x->ix[d];
  for (;;) {
    MemPage *pg = x->pg[d];
    for (; ix < pg->nCell; ix++) {
      i64 k; int kind = qc_idxkey(pg, ix, &k);
      if (kind < 0) return 1;
      if (kind == 2 || (kind == 1 && k > hi) || (kind == 0 && k != INT64_MIN && k >= hi)) return 0;
      if (kind == 1 && k >= lo) { if (counts) counts[k - lo]++; else (*one)++; }
    }
    for (;;) {   /* up to the first ancestor with an entry after the child we came from */
      if (d == 0) return 0;
      d--;
      if (x->ix[d] < x->pg[d]->nCell) break;
    }
    pg = x->pg[d]; ix = x->ix[d];
    { i64 k; int kind = qc_idxkey(pg, ix, &k);        /* the interior entry itself */
      if (kind < 0) return 1;
      if (kind == 2 || (kind == 1 && k > hi) || (kind == 0 && k != INT64_MIN && k >= hi)) return 0;
      if (kind == 1 && k >= lo) { if (counts) counts[k - lo]++; else (*one)++; } }
    x->ix[d] = ix + 1;                                 /* then the leftmost leaf of the next child */
    for (int e = d + 1; e < x->depth; e++) { releasePage(x->pg[e]); x->pg[e] = 0; }
    x->depth = d + 1;
    Pgno ch = qc_child(pg, ix + 1);
    for (;;) {
      MemPage *c; if (x->depth >= QC_MAXDEPTH || getAndInitPage(bt, ch, &c, PAGER_GET_READONLY)) return 1;
      x->pg[x->depth] = c; x->ix[x->depth] = 0; x->depth++;
      if (c->intKey) return 1;
      if (c->leaf) break;
      ch = qc_child(c, 0);
    }
    d = x->depth - 1; ix = 0;
  }
}

#define QG_DEMOTE (-2)   /* (qcg.c's: this plan can't run it, qvm takes the statement) */
static int qc_exec_special(Vdbe *v, qc_row_cb cb, void *ctx) {
  QcPlan *P = QCV(v)->pQc; sqlite3 *db = v->db;
  if (!P || v->eVdbeState != VDBE_READY_STATE || (db->autoCommit && !QC_CONN(db)->ownTxn) || P->iDb >= db->nDb) return QC_FB("L1");
  Btree *bt = db->aDb[P->iDb].pBt; if (!bt) return QC_FB("L2");
  if (bt->pBt->pPager->bUseFetch) { qc_why = "mmap"; return QG_DEMOTE; }   /* (as qg_exec: its lanes, batched seeks and pins add page references a memory-mapped page can't take) */
  /* OP_Transaction's read path: begin (or join) the read transaction, then the schema cookie check. Within one
     transaction the schema changes only by this connection's own DDL. That expires the statement (checked below) for
     the temp schema only; for the others OP_SetCookie moves the in-memory cookie, compared every run. So the btree's
     cookie is read once per transaction, and again after the connection's own DDL (ALTER TABLE ... ADD COLUMN in a
     transaction: a SELECT * served the old columns) */
  if (sqlite3BtreeTxnState(bt) == SQLITE_TXN_NONE) { P->txnChecked = 0;
    int iMeta; if (sqlite3BtreeBeginTrans(bt, 0, &iMeta) != SQLITE_OK) return QC_FB("L3");
  }
  if (v->expired) return QC_FB("L5");
  if (!P->txnChecked || P->bt != bt || db->aDb[P->iDb].pSchema->schema_cookie != P->cookie) {
    int iMeta; sqlite3BtreeGetMeta(bt, BTREE_SCHEMA_VERSION, (u32 *)&iMeta);
    if (iMeta != P->cookie || db->aDb[P->iDb].pSchema->iGeneration != P->gen) return QC_FB("L4");   /* SQLITE_SCHEMA: SQLite re-prepares */
    P->txnChecked = 1; P->bt = bt;   /* (P->bt also names the btree the hook clears this for) */
    if (!P->act) { QcConn *C = QCD(db)->pQcConn; QC_LIST_ADD(C->act, C->nact, P, act); }
  }
  QcRun *R = qc_run_alloc(QCD(db)->pQcConn); if (!R) return QC_FB("L6");
  i64 rowid[QC_MAXROWS]; int nrow = 0, st = QC_FALLBACK;
  R->npin = 0; for (int j = 0; j < P->njoin; j++) R->jn[j] = 0;
  QcVal *outv = R->outv;
#define IV(i) (R->ivf + (size_t)(i) * P->idxW)
#define TV(i) (R->tvf + (size_t)(i) * P->tabW)
#define JV(j, i) (R->jvf[j] + (size_t)(i) * P->joinW[j])
  i64 (*cnt)[QC_MAXROWS] = R->cnt;
  /* 1. driver */
  if (P->scan) {
    BtCursor *ci = qc_cursor(P, bt, P->idxRoot, P->idxKi, &P->ci, &P->oi); if (!ci) return QC_FB("L7");
    int res;
    if (sqlite3BtreeFirst(ci, &res) != SQLITE_OK) { sqlite3BtreeCloseCursor(ci); return QC_FB("L8"); }
    if (!res) do {
      i64 r; u32 sz = sqlite3BtreePayloadSize(ci), amt; const u8 *rec = sqlite3BtreePayloadFetch(ci, &amt);
      int fast = 0;
      if (P->idxRowidField >= 0 && amt >= sz) {   /* the covered columns and the rowid in one pass */
        QcVal tmp[QC_MAXCOL];
        if (qc_dec_idx(P, rec, sz, tmp)) {
          for (int c = 0; c < P->idxRowidField && c < P->idxW; c++) IV(nrow)[c] = tmp[c];
          r = tmp[P->idxRowidField].i; fast = 1;
        }
      }
      if (!fast && sqlite3VdbeIdxRowid(db, ci, &r) != SQLITE_OK) goto out_ci;
      if (P->idxCols) {   /* covered columns straight from the index record; its page stays pinned */
        if (amt < sz || (!fast && !qc_decode(rec, sz, P->idxCols, IV(nrow)))) goto out_ci;
        DbPage *pg = ci->pPage->pDbPage;
        if (!R->npin || R->pins[R->npin - 1] != pg) { sqlite3PagerRef(pg); R->pins[R->npin++] = pg; }
      }
      rowid[nrow++] = r;
    } while (nrow < P->limit && sqlite3BtreeNext(ci, 0) == SQLITE_OK);
    if (0) { out_ci: for (int i = 0; i < R->npin; i++) sqlite3PagerUnref(R->pins[i]); return QC_FB("L9"); }
  } else {
    const QcOut *O = cb ? 0 : ctx;
    if (!P->param) rowid[nrow++] = P->key;
    else if (O && O->param) {
      if (P->param > O->nparam) return QC_FB("param");
      rowid[nrow++] = O->param[P->param - 1];
    } else {
      Mem *m = &v->aVar[P->param - 1];
      if (!(m->flags & MEM_Int) || (m->flags & (MEM_Null | MEM_Str | MEM_Real | MEM_Blob))) return QC_FB("L10");   /* SeekRowid would convert */
      rowid[nrow++] = m->u.i;
    }
  }
  /* 2. table rows (and, overlapped with them, the start of every count's index range) */
  QcLane *lanes = 0; int nl = 0, cntLane[QC_MAXCNT]; i64 clo[QC_MAXCNT], chi[QC_MAXCNT];
  for (int c = 0; c < P->ncnt; c++) cntLane[c] = -1;
  if (P->lanes && nrow && (lanes = qc_lanes(QCD(db)->pQcConn))) {   /* (no memory for them: the cursors' way) */
    for (int i = 0; i < nrow; i++) { lanes[nl].root = P->tabRoot; lanes[nl].isIdx = 0; lanes[nl].key = rowid[i]; nl++; }
    for (int c = 0; c < P->ncnt; c++) {
      i64 lo = INT64_MAX, hi = INT64_MIN;
      for (int i = 0; i < nrow; i++) { if (rowid[i] < lo) lo = rowid[i]; if (rowid[i] > hi) hi = rowid[i]; }
      if ((u64)(hi - lo) >= QC_RANGE_MAX) continue;          /* a wide range: per-key cursor seeks below */
      clo[c] = lo; chi[c] = hi; cntLane[c] = nl;
      lanes[nl].root = P->cnt[c].root; lanes[nl].isIdx = 1; lanes[nl].key = lo; nl++;
    }
    if (qc_descend(bt->pBt, lanes, nl) != SQLITE_OK) return QC_FB("descend");
    for (int i = 0; i < nrow; i++) {
      QcLane *x = &lanes[i];
      if (!x->found) { if (P->scan) { qc_why = "lane miss"; goto out; } nrow = 0; break; }   /* point: no such row */
      MemPage *pg = x->pg[0]; CellInfo info; pg->xParseCell(pg, findCell(pg, x->ix[0]), &info);
      if (info.nLocal != info.nPayload ||
          !((QC_DEC_TAB(info.pPayload, info.nPayload, TV(i))) || (P->tabShape.ok && qc_decode_fast(info.pPayload, info.nPayload, &P->tabShape, P->tabCols, TV(i))) ||
            qc_decode(info.pPayload, info.nPayload, P->tabCols, TV(i)))) { qc_why = "lane row"; goto out; }
    }
    goto rows_done;
  }
  BtCursor *ct = qc_cursor(P, bt, P->tabRoot, 0, &P->ct, &P->ot); if (!ct) return QC_FB("L11");
  int nkept = 0; i64 prev = INT64_MIN;
  for (int i = 0; i < nrow; i++) {
    int found = 0, res;
    if (prev == rowid[i] + 1 && ct->eState == CURSOR_VALID) {
      int rc = sqlite3BtreePrevious(ct, 0);
      if (rc == SQLITE_OK && sqlite3BtreeIntegerKey(ct) == rowid[i]) found = 1; else if (rc != SQLITE_OK && rc != SQLITE_DONE) { qc_why = "out@" QC_STR(__LINE__); goto out; }
    }
    if (!found) { if (sqlite3BtreeTableMoveto(ct, rowid[i], 0, &res) != SQLITE_OK) { qc_why = "out@" QC_STR(__LINE__); goto out; } found = res == 0 && ct->eState == CURSOR_VALID; }
    prev = rowid[i];
    if (!found) { if (P->scan) { qc_why = "out@" QC_STR(__LINE__); goto out; } nrow = 0; break; }     /* scan: a dangling index entry (corrupt): SQLite decides */
    u32 sz = sqlite3BtreePayloadSize(ct), amt; const u8 *rec = sqlite3BtreePayloadFetch(ct, &amt);
    if (amt < sz || !((QC_DEC_TAB(rec, sz, TV(i))) || (P->tabShape.ok && qc_decode_fast(rec, sz, &P->tabShape, P->tabCols, TV(i))) || qc_decode(rec, sz, P->tabCols, TV(i)))) { qc_why = "out@" QC_STR(__LINE__); goto out; }
    if (P->scan) {   /* the cursor moves on to the next row: keep this page; a single lookup leaves the cursor (and page) here */
      DbPage *pg = ct->pPage->pDbPage;
      if (!R->npin || R->pins[R->npin - 1] != pg) { sqlite3PagerRef(pg); R->pins[R->npin++] = pg; }
    }
    nkept++;
  }
  (void)nkept;
rows_done:
  /* 3. joins: one batched descent per join */
  for (int j = 0; j < P->njoin && nrow; j++) {
    i64 keys[QC_MAXROWS];
    for (int i = 0; i < nrow; i++) {
      QcVal *k = &TV(i)[P->join[j].keycol];
      if (k->type != SQLITE_INTEGER) { qc_why = "out@" QC_STR(__LINE__); goto out; }           /* NULL, real or text key: SeekRowid's conversions, not ours */
      keys[i] = k->i;
    }
    if (nrow == 1) {   /* one row: a plain seek on the plan's cursor, which stays where it is until the next execution */
      BtCursor *cj = qc_cursor(P, bt, P->join[j].root, 0, &P->cj[j], &P->oj[j]); int res;
      if (!cj || sqlite3BtreeTableMoveto(cj, keys[0], 0, &res) != SQLITE_OK) { qc_why = "out@" QC_STR(__LINE__); goto out; }
      if (res != 0 || cj->eState != CURSOR_VALID) { if (P->scan) goto out; nrow = 0; break; }
      u32 sz = sqlite3BtreePayloadSize(cj), amt; const u8 *rec = sqlite3BtreePayloadFetch(cj, &amt);
      if (amt < sz || !((QC_DEC_JOIN(j, rec, sz, JV(j, 0))) || (P->joinShape[j].ok && qc_decode_fast(rec, sz, &P->joinShape[j], P->joinCols[j], JV(j, 0))) || qc_decode(rec, sz, P->joinCols[j], JV(j, 0)))) { qc_why = "out@" QC_STR(__LINE__); goto out; }
      continue;
    }
    if (qc_seek_batch(bt, P->join[j].root, keys, nrow, R->jr[j]) != SQLITE_OK) { qc_why = "out@" QC_STR(__LINE__); goto out; }
    R->jn[j] = nrow;
    for (int i = 0; i < nrow; i++) {
      qc_res *q = &R->jr[j][i];
      if (!q->found) { if (P->scan) { qc_why = "out@" QC_STR(__LINE__); goto out; } nrow = 0; break; }   /* inner join drops the row: LIMIT would read further */
      MemPage *pg = q->leaf; CellInfo info; pg->xParseCell(pg, findCell(pg, q->ix), &info);
      if (info.nLocal != info.nPayload || !((QC_DEC_JOIN(j, info.pPayload, info.nPayload, JV(j, i))) || (P->joinShape[j].ok && qc_decode_fast(info.pPayload, info.nPayload, &P->joinShape[j], P->joinCols[j], JV(j, i))) ||
                                            qc_decode(info.pPayload, info.nPayload, P->joinCols[j], JV(j, i)))) { qc_why = "out@" QC_STR(__LINE__); goto out; }
    }
  }
  /* 4. counts: from the lane's position (one in-order walk of the range), or cursor seeks */
  for (int c = 0; c < P->ncnt && nrow; c++) {
    if (cntLane[c] >= 0) {
      if (nrow == 1) { i64 one = 0; if (qc_idx_walk(bt->pBt, &lanes[cntLane[c]], clo[c], chi[c], 0, &one)) { qc_why = "walk"; goto out; } cnt[c][0] = one; }
      else {
        memset(R->counts, 0, sizeof(u32) * (size_t)(chi[c] - clo[c] + 1));
        if (qc_idx_walk(bt->pBt, &lanes[cntLane[c]], clo[c], chi[c], R->counts, 0)) { qc_why = "walk"; goto out; }
        for (int i = 0; i < nrow; i++) cnt[c][i] = R->counts[rowid[i] - clo[c]];
      }
    } else if (nrow == 1 ? qc_count1(bt, P, c, rowid[0], &cnt[c][0]) : qc_counts(R, bt, P, c, rowid, nrow, cnt[c])) { qc_why = "out@" QC_STR(__LINE__); goto out; }
  }
  /* 5. rows out: the response body (template), or zero-copy rows to the callback */
  if (!cb) {
    QcTmpl *T = QCV(v)->pQcTmpl; QcOut *O = ctx;
    if (!T || T->ncol != P->nout) goto out;   /* (a re-lifted plan with a different column count keeps the fallback) */
    int vec = bt->pBt->pPager->xGet == getPageNormal;   /* page-resident values: reading past a field stays in the page's allocation */
    /* flat: each output column is a base and a stride into the run state, fixed for this plan (no per-value dispatch).
       One pass: each value is checked as it is written (a fallback leaves a partial body the caller discards), and each
       row first checks its worst case against the room left (+64: the fixed 32-byte fragment moves and vector tails) */
#ifdef QC_ORACLE
    /* the unrolled writer for exactly (rowid, row col 2, index col 0, join 0 col 1, count 0) as i s s s i */
    if (T->ncol == 5 && P->out[0].kind == S_ROWID && P->out[1].kind == S_COL && P->out[1].src == 0 && P->out[1].col == 2 &&
        P->out[2].kind == S_COL && P->out[2].src == -1 && P->out[2].col == 0 && P->out[3].kind == S_COL && P->out[3].src == 1 &&
        P->out[3].col == 1 && P->out[4].kind == S_COUNT && P->out[4].src == 0 && !strcmp(T->kind, "isssi") &&
        !strcmp(T->frag[0], "{\"id\":") && !strcmp(T->frag[1], ",\"body\":") && !strcmp(T->frag[2], ",\"created_at\":") &&
        !strcmp(T->frag[3], ",\"author\":") && !strcmp(T->frag[4], ",\"like_count\":") && !strcmp(T->frag[5], "}")) {
      char *p = O->buf, *end = O->buf + O->cap;
      #define QC_W(lit) (memcpy(p, lit, sizeof(lit) - 1), p += sizeof(lit) - 1)
      if ((size_t)(end - p) < T->lopen + T->lclose + 64) goto out;
      p = qc_frag(p, T->open, T->lopen);
      for (int i = 0; i < nrow; i++) {
        const QcVal *b = &TV(i)[2], *c = &IV(i)[0], *a = &JV(0, i)[1];
        if (b->type != SQLITE_TEXT || c->type != SQLITE_TEXT || a->type != SQLITE_TEXT ||
            (size_t)(end - p) < 200 + 6 * ((size_t)b->n + (size_t)c->n + (size_t)a->n)) goto out;
        if (i) *p++ = ',';
        QC_W("{\"id\":"); p = qc_jint(p, rowid[i]);
        QC_W(",\"body\":"); p = qc_jstr(p, b->z, (u32)b->n, vec);
        QC_W(",\"created_at\":"); p = qc_jstr(p, c->z, (u32)c->n, vec);
        QC_W(",\"author\":"); p = qc_jstr(p, a->z, (u32)a->n, vec);
        QC_W(",\"like_count\":"); p = qc_jint(p, cnt[0][i]); *p++ = '}';
      }
      p = qc_frag(p, T->close, T->lclose);
      O->n = (size_t)(p - O->buf); O->rows = nrow; st = SQLITE_OK; goto out;
    }
#endif
    /* one compact op per output column: where its values are, how they are written, its fragment */
    struct { const char *frag; u32 lfrag; u8 op; const QcVal *v; const i64 *iv; size_t stride; } ops[QC_MAXOUT];
    for (int k = 0; k < T->ncol; k++) {
      QcSym sy = P->out[k]; ops[k].frag = T->frag[k]; ops[k].lfrag = T->lfrag[k]; ops[k].v = 0; ops[k].iv = 0; ops[k].stride = 0;
      if (sy.kind == S_ROWID || sy.kind == S_COUNT) {
        if (T->kind[k] != 'i') goto out;
        ops[k].op = 0; ops[k].iv = sy.kind == S_ROWID ? rowid : cnt[sy.src];
      } else {
        if (sy.real) goto out;   /* (the body writer has no float form) */
        ops[k].op = T->kind[k] == 'i' ? 1 : 2;
        if (sy.src < 0) { ops[k].v = &IV(0)[sy.col]; ops[k].stride = (size_t)P->idxW; }
        else if (sy.src == 0) { ops[k].v = &TV(0)[sy.col]; ops[k].stride = (size_t)P->tabW; }
        else { ops[k].v = &JV(sy.src - 1, 0)[sy.col]; ops[k].stride = (size_t)P->joinW[sy.src - 1]; }
      }
    }
    char *p = O->buf, *end = O->buf + O->cap; const int ncol = T->ncol;
    if ((size_t)(end - p) < T->lopen + T->lclose + 64) goto out;
    p = qc_frag(p, T->open, T->lopen);
    /* room: checked before each value against its worst case + the fixed fragment moves (QC_FRAG) + 64 */
    for (int i = 0; i < nrow; i++) {
      if ((size_t)(end - p) < T->lsep + QC_FRAG + 64) goto out;
      if (i) p = qc_frag(p, T->sep, T->lsep);
      for (int k = 0; k < ncol; k++) {
        if (ops[k].op == 0) {   /* a rowid or a count: always an integer */
          if ((size_t)(end - p) < 2 * QC_FRAG + 21 + 64) goto out;
          p = qc_frag(p, ops[k].frag, ops[k].lfrag); p = qc_jint(p, ops[k].iv[i]); continue;
        }
        const QcVal *x = ops[k].v + i * ops[k].stride;
        if (ops[k].op == 1) {
          if (x->type != SQLITE_INTEGER || (size_t)(end - p) < 2 * QC_FRAG + 21 + 64) goto out;
          p = qc_frag(p, ops[k].frag, ops[k].lfrag); p = qc_jint(p, x->i);
        } else {
          if (x->type != SQLITE_TEXT || (size_t)(end - p) < 2 * QC_FRAG + 2 + 6 * (size_t)x->n + 64) goto out;
          p = qc_frag(p, ops[k].frag, ops[k].lfrag); p = qc_jstr(p, x->z, (u32)x->n, vec);
        }
      }
      if ((size_t)(end - p) < 2 * QC_FRAG + 64) goto out;
      p = qc_frag(p, T->frag[ncol], T->lfrag[ncol]);
    }
    if ((size_t)(end - p) < QC_FRAG + 64) goto out;
    p = qc_frag(p, T->close, T->lclose);
    O->n = (size_t)(p - O->buf); O->rows = nrow; st = SQLITE_OK;
  } else {
    for (int i = 0; i < nrow; i++) for (int k = 0; k < P->nout; k++) {
      QcSym s = P->out[k]; QcVal *o = &outv[i * P->nout + k];
      if (s.kind == S_ROWID) { o->type = SQLITE_INTEGER; o->i = rowid[i]; }
      else if (s.kind == S_COUNT) { o->type = SQLITE_INTEGER; o->i = cnt[s.src][i]; }
      else {
        *o = s.src < 0 ? IV(i)[s.col] : s.src == 0 ? TV(i)[s.col] : JV(s.src - 1, i)[s.col];
        if (s.real && o->type == SQLITE_INTEGER) { o->type = SQLITE_FLOAT; o->r = (double)o->i; }   /* OP_RealAffinity */
      }
    }
    st = cb(ctx, nrow, P->nout, outv);
  }
out:
  if (nl) qc_lanes_release(lanes, nl);
  for (int j = 0; j < P->njoin; j++) for (int i = 0; i < R->jn[j]; i++) if (R->jr[j][i].leaf) releasePage(R->jr[j][i].leaf);
  for (int i = 0; i < R->npin; i++) sqlite3PagerUnref(R->pins[i]);
  return st;
}

/* ---------------------------------------------------------------- general plans, and the public entry points */
/* length()'s count of a text value (lengthFunc): characters up to the first NUL, a lead byte >= 0xc0 swallowing the
   continuation bytes after it; 8 bytes at a time while they are ASCII and not NUL (each one character) */
static inline i64 qc_utf8_chars(const u8 *z, int n) {
  const u8 *e = z + n; i64 len = 0;
  while (z < e) {
    if (e - z >= 8) {
      u64 w; memcpy(&w, z, 8);
      if (!((w | (w - 0x0101010101010101ULL)) & 0x8080808080808080ULL)) { len += 8; z += 8; continue; }
    }
    if (!*z) break;
    len++; if (*z++ >= 0xc0) while (z < e && (*z & 0xc0) == 0x80) z++;
  }
  return len;
}
/* ---------------------------------------------------------------- program signatures (the plan cache, qv_cache_*)
** Every opcode and operand of a program, P4 by value (text, blobs, KeyInfo, Mem, integer arrays) or by the
** connection-owned object it names (FuncDef, CollSeq): two programs with equal signatures run alike, and a plan lifted
** from one runs the other once re-pointed at it. 0: a P4 it cannot compare (then the plan is not cached). */
/* what vdbe.c's OP_String8 does on its first run (UTF-8): an OP_String with the length. qv_lift does it to the
   program, and qv_cache_get to a program that gets a cached plan (whose region nodes read the length) */
static int qv_str8_ok(const Op *o, const sqlite3 *db) { return o->opcode == OP_String8 && o->p4.z && strlen(o->p4.z) <= (size_t)db->aLimit[SQLITE_LIMIT_LENGTH]; }
static void qv_str8(Vdbe *v) {
  for (int pc = 0; pc < v->nOp; pc++) if (qv_str8_ok(&v->aOp[pc], v->db)) { v->aOp[pc].p1 = sqlite3Strlen30(v->aOp[pc].p4.z); v->aOp[pc].opcode = OP_String; }
}
typedef struct QvSig { u8 *b; u32 n, cap; const u8 *cmp; u32 ncmp; } QvSig;
/* what a program generated from the same text depends on besides the text: the connection's flags, optimisations and
   limits, the prepare flags, the schemas' generations, and sqlite3ExpirePreparedStatements' count (functions, collations,
   modules, ANALYZE). A cached plan stamped when its program was known to be its own (lifted, or matched by signature)
   skips the signature check while the stamp holds (about 1,100 instructions of a prepared lookup's 25k) */
typedef struct QvStamp { u64 flags, gens, lim; u32 opt, epoch, nDb; u8 prep, ok; } QvStamp;
static void qv_stamp(const Vdbe *v, QvStamp *s) {
  const sqlite3 *db = v->db; u64 g = 0, l = 1469598103934665603ULL;
  for (int i = 0; i < db->nDb; i++) {   /* (a generation changes when a schema is reloaded, the cookie with any DDL) */
    const Schema *sc = db->aDb[i].pSchema;
    g = (g ^ (sc ? (u64)(u32)sc->iGeneration << 32 | (u32)sc->schema_cookie : ~0ULL)) * 1099511628211ULL;
  }
  for (int i = 0; i < SQLITE_N_LIMIT; i++) l = (l ^ (u64)(u32)db->aLimit[i]) * 1099511628211ULL;
  s->flags = db->flags; s->gens = g; s->lim = l; s->opt = db->dbOptFlags; s->epoch = QCD(db)->qcEpoch; s->nDb = (u32)db->nDb;
  s->prep = v->prepFlags;
  s->ok = !v->expmask;   /* (a program that depends on its bindings, STAT4's, is no stamp's: the same text in the same state can
                            prepare to another program, and a plan stamped from one matched the other unchecked: a crash) */
#if QC_STOCK
  s->ok = 0;   /* (no qcEpoch: a function, collation or module changed is seen only by the signature) */
#endif
}
static int qv_stamp_eq(const QvStamp *a, const QvStamp *b) {
  return a->ok && b->ok && a->flags == b->flags && a->gens == b->gens && a->lim == b->lim && a->opt == b->opt &&
         a->epoch == b->epoch && a->nDb == b->nDb && a->prep == b->prep;
}   /* cmp: compare with it instead of building (no copy) */
static __attribute__((noinline)) int qv_sig_grow(QvSig *s, u32 n);
/* (inlined: QV_SIG's sizes are constants, so each compare and copy is a load or two, not a call) */
static inline __attribute__((always_inline)) int qv_sig_eq(const u8 *a, const void *p, u32 n) {   /* memcmp's == 0, in line for the fixed sizes */
  if (__builtin_constant_p(n) && (n == 1 || n == 2 || n == 4 || n == 8 || n == 16)) {
    if (n == 16) { u64 x0, x1, y0, y1; memcpy(&x0, a, 8); memcpy(&x1, a + 8, 8); memcpy(&y0, p, 8); memcpy(&y1, (const u8 *)p + 8, 8); return ((x0 ^ y0) | (x1 ^ y1)) == 0; }
    if (n == 8) { u64 x, y; memcpy(&x, a, 8); memcpy(&y, p, 8); return x == y; }
    if (n == 4) { u32 x, y; memcpy(&x, a, 4); memcpy(&y, p, 4); return x == y; }
    if (n == 2) { u16 x, y; memcpy(&x, a, 2); memcpy(&y, p, 2); return x == y; }
    return *a == *(const u8 *)p;
  }
  return memcmp(a, p, n) == 0;
}
static inline __attribute__((always_inline)) int qv_sig_put(QvSig *s, const void *p, u32 n) {
  if (s->cmp) { if (s->n + n > s->ncmp || !qv_sig_eq(s->cmp + s->n, p, n)) return 0; s->n += n; return 1; }   /* (a cached plan's check, each prepare: per op a 16-byte record, compared in line where bcmp was called) */
  if (s->n + n > s->cap && !qv_sig_grow(s, n)) return 0;
  memcpy(s->b + s->n, p, n); s->n += n; return 1;
}
static __attribute__((noinline)) int qv_sig_grow(QvSig *s, u32 n) {
  u32 c = s->cap ? s->cap : 1024; while (c < s->n + n) c *= 2;
  u8 *b = sqlite3_realloc64(s->b, c); if (!b) return 0;
  s->b = b; s->cap = c; return 1;
}
#define QV_SIG(x) qv_sig_put(&s, &(x), sizeof(x))
static int qv_sig(Vdbe *v, QvSig *out) {   /* out->cmp set: 1 = v's signature is that one */
  QvSig s = {0, 0, 0, out->cmp, out->ncmp}; int ok = 1;
  ok &= QV_SIG(v->nOp) & QV_SIG(v->nMem) & QV_SIG(v->nCursor) & QV_SIG(v->nResColumn) & QV_SIG(v->prepFlags);
  ok &= QV_SIG(v->db->flags) & QV_SIG(v->db->enc);
  for (int pc = 0; ok && pc < v->nOp; pc++) {
    const Op *o = &v->aOp[pc]; i8 t = o->p4type; u8 opc = o->opcode; int p1 = o->p1;
    if (qv_str8_ok(o, v->db)) { opc = OP_String; p1 = sqlite3Strlen30(o->p4.z); }   /* (as qv_str8 leaves it: equal before and after) */
    u8 rec[16]; rec[0] = opc; rec[1] = (u8)t; memcpy(rec + 2, &o->p5, 2); memcpy(rec + 4, &p1, 4); memcpy(rec + 8, &o->p2, 4); memcpy(rec + 12, &o->p3, 4);
    ok &= qv_sig_put(&s, rec, 16);   /* (one record per op: one bounds check, one 16-byte compare) */
    switch (t) {
      case P4_NOTUSED: break;
      case P4_INT32: ok &= QV_SIG(o->p4.i); break;
      case P4_STATIC: case P4_DYNAMIC: {   /* text, or for Blob and String P1 bytes */
        u32 n = (opc == OP_Blob || opc == OP_String) ? (u32)p1 : o->p4.z ? (u32)strlen(o->p4.z) : 0;
        ok &= QV_SIG(n) && (!n || qv_sig_put(&s, o->p4.z, n)); break;
      }
      case P4_COLLSEQ: ok &= QV_SIG(o->p4.pColl); break;
      case P4_FUNCDEF: ok &= QV_SIG(o->p4.pFunc); break;
      case P4_FUNCCTX: ok &= QV_SIG(o->p4.pCtx->pFunc) & QV_SIG(o->p4.pCtx->argc); break;
      case P4_KEYINFO: {
        const KeyInfo *k = o->p4.pKeyInfo;
        ok &= QV_SIG(k->enc) & QV_SIG(k->nKeyField) & QV_SIG(k->nAllField);
        ok &= qv_sig_put(&s, k->aSortFlags, k->nKeyField) && qv_sig_put(&s, k->aColl, sizeof(CollSeq *) * k->nAllField);
        break;
      }
      case P4_MEM: {
        const Mem *m = o->p4.pMem; u16 f = m->flags & (MEM_Null | MEM_Str | MEM_Int | MEM_Real | MEM_Blob | MEM_IntReal);
        ok &= QV_SIG(f) & QV_SIG(m->enc);
        if (f & (MEM_Str | MEM_Blob)) ok &= QV_SIG(m->n) && qv_sig_put(&s, m->z, (u32)m->n);
        else if (f & (MEM_Int | MEM_IntReal)) ok &= QV_SIG(m->u.i);
        else if (f & MEM_Real) ok &= QV_SIG(m->u.r);
        if (m->flags & MEM_Zero) ok = 0;
        break;
      }
      case P4_INTARRAY: ok &= qv_sig_put(&s, o->p4.ai, sizeof(u32) * (o->p4.ai[0] + 1)); break;
      case P4_SUBRTNSIG: {
        const SubrtnSig *g = o->p4.pSubrtnSig; u32 n = g->zAff ? (u32)strlen(g->zAff) : 0;
        ok &= QV_SIG(g->selId) & QV_SIG(g->iTable) & QV_SIG(g->iAddr) & QV_SIG(g->regReturn) & QV_SIG(n) && (!n || qv_sig_put(&s, g->zAff, n));
        break;
      }
      default: ok = 0;   /* tables, indexes, subprograms, virtual tables, expressions: not compared */
    }
  }
  if (s.cmp) return ok && s.n == s.ncmp;
  if (!ok) { sqlite3_free(s.b); return 0; }
  *out = s; return 1;
}
#undef QV_SIG
/* Lookaside is the connection's pool of small blocks for SQLite's own short-lived allocations (a prepare, a run). What a
   plan keeps past its program (the plan cache: up to QC_CACHE_PLANS plans) is moved off it, or SQLite's later prepares
   find the pool full and go to malloc (bench's corpus, 2026-10-28: 116 slots held after vaultwarden's statements,
   stock 6; every prepare 20-40 % more instructions on the Xeon, served or not). */
static KeyInfo *qc_ki_keep(KeyInfo *k) {   /* a reference to k, or a copy of it on the heap */
  if (!k || !isLookaside(k->db, k)) return sqlite3KeyInfoRef(k);
  size_t n = SZ_KEYINFO(0) + (size_t)k->nAllField * (sizeof(CollSeq *) + 1);
  KeyInfo *p = sqlite3Malloc(n); if (!p) return 0;
  memcpy(p, k, n); p->aSortFlags = (u8 *)&p->aColl[p->nAllField]; p->nRef = 1;   /* (freed by sqlite3KeyInfoUnref) */
  return p;
}
static UnpackedRecord *qc_unpacked_keep(KeyInfo *k) {   /* sqlite3VdbeAllocUnpackedRecord's, on the heap */
  UnpackedRecord *p = sqlite3Malloc(ROUND8P(sizeof(UnpackedRecord)) + sizeof(Mem) * (k->nKeyField + 1)); if (!p) return 0;
  p->aMem = (Mem *)&((char *)p)[ROUND8P(sizeof(UnpackedRecord))]; p->pKeyInfo = k; p->nField = k->nKeyField + 1;
  memset(p->aMem, 0, sizeof(Mem) * (k->nKeyField + 1));   /* (once: qcg's seeks then set only the fields a compare reads, qg_key) */
  return p;
}
static void qc_mem_keep(Mem *m) {   /* m's buffer, moved to the heap with its value (left where it is if malloc fails) */
  if (!m->szMalloc || !isLookaside(m->db, m->zMalloc)) return;
  char *o = m->zMalloc, *z = sqlite3Malloc(m->szMalloc); if (!z) return;
  memcpy(z, o, (size_t)m->szMalloc);
  if (m->z >= o && m->z < o + m->szMalloc) m->z = z + (m->z - o);
  m->zMalloc = z; sqlite3DbFreeNN(m->db, o);
}
static inline void qc_cpy(char *d, const u8 *s, size_t n) {   /* memcpy, inline for the short texts most values are (a UUID: 36) */
  if (n > 32) { memcpy(d, s, n); return; }
  if (n >= 16) { memcpy(d, s, 16); memcpy(d + n - 16, s + n - 16, 16); return; }   /* (overlapping moves of fixed size) */
  if (n >= 8) { memcpy(d, s, 8); memcpy(d + n - 8, s + n - 8, 8); return; }
  if (n >= 4) { memcpy(d, s, 4); memcpy(d + n - 4, s + n - 4, 4); return; }
  for (size_t i = 0; i < n; i++) d[i] = (char)s[i];
}
static int qc_serve_rows(void *ctx, int nrow, int ncol, const QcVal *x);   /* (qvm writes its rows straight into a QvSink) */
#include "qcg.c"
#include "qvm.c"

static int qc_plan_nout(const void *plan) {
  int k = *(const int *)plan;
  return k == 1 ? ((const QcPlan *)plan)->nout : k == 2 ? ((const QcGPlan *)plan)->nout : ((const QvPlan *)plan)->nout;
}
static void qc_txn_end_all(Btree *bt) { qc_txn_end(bt); qg_txn_end(bt); qv_txn_end(bt); }
#if QC_STOCK
static void qc_stock_txn_end(sqlite3 *db) {   /* every plan used in the run: its cursors closed, whatever its B-tree */
  QcConn *C = QC_CONN(db); QgConn *GC = QCD(db)->pQgConn; QvConn *VC = QV_CONN(db);
  while (C->nact) qc_txn_end(C->act[0]->bt);
  while (GC->nact) qg_txn_end(GC->act[0]->bt);
  while (VC->nact) qv_txn_end(VC->act[0]->bt);
}
#endif
static void qc_free_special(QcPlan *P, sqlite3 *db) {
  qc_close(P->ci, &P->oi); qc_close(P->ct, &P->ot); sqlite3_free(P->ci); sqlite3_free(P->ct);
  for (int j = 0; j < QC_MAXJOIN; j++) { qc_close(P->cj[j], &P->oj[j]); sqlite3_free(P->cj[j]); }
  for (int c = 0; c < QC_MAXCNT; c++) { qc_close(P->cc[c], &P->oc[c]); sqlite3_free(P->cc[c]); }
  QcConn *C = QCD(db)->pQcConn; QC_COUNT_DEL(C->nplans, P); QC_LIST_DEL(C->act, C->nact, P, act);
  sqlite3_free(P);
}
/* ---------------------------------------------------------------- serving through sqlite3_step (drop-in)
** With the amalgamation patched by qc/patch.py, two hooks let an unchanged program use qc through SQLite's own API:
**   sqlite3QcStart, at the top of sqlite3_step (the connection's mutex held): a statement about to start (READY) is
**     lifted if it isn't yet, and run by qc in a read transaction of its own (sqlite3_qc_exec's); its rows are kept with
**     the statement (QcServe, reused by every execution of the handle);
**   sqlite3QcServe, where sqlite3Step would call sqlite3VdbeExec: the kept rows are handed out one per call through the
**     statement's result row, and the last call halts the statement as OP_Halt does. Everything around them (the
**     statement's state, the connection's counters, errors, the profile callback) is sqlite3Step's own.
** A statement qc doesn't take, or that falls back, runs in sqlite3VdbeExec from its first opcode, as before. Not taken
** at all: writes, EXPLAIN, a connection with a trace or progress callback, and a statement qc refused (remembered on
** the handle until the schema changes). On by sqlite3_qc_serve(1) or QC_SERVE=1 at startup; QC_SERVE=0 off. */
typedef struct QcServe {
  QvSink k;   /* (first: qv_exec takes the callback's ctx as it) rows up to QC_SERVE_MEMROWS values kept as the result row's Mems: a
                step points pResultRow at the next row */
  int nrow, ncol, pos;   /* the kept rows (in k: Mems, or a large result's values and their text and blob bytes) */
  Mem *mem; int nmem;                                              /* the result row they are handed out through */
  u8 ready, active;                                                /* rows waiting / being served */
  void *fastP;   /* the plan its last run served, the checks fixed for its program passed: qc_serve_fast's */
  QcAdapt *tA; void *tAP; double it, ilate, i0; int isteps, nlate, nun; u8 tall;   /* i0: its first step (QC_STATS_ADAPT); tall: every step timed */   /* strm 3: the interpreter sample being timed (qc_serve_itimed) */
  void *keptA;   /* (strm 4, or 2: a stream drained for another statement) the arena its last row may point into */
  u8 strm; int sent;            /* strm: 1 streamed (qvm's rows as it reaches them, qv_stream_next), 2 replay, 3 an interpreter sample, 4 drained for another statement (its last row kept until it steps); sent: rows handed out so far; replay:
                                   the stream failed after handing them out, the interpreter runs the statement and skips them */
  double *tq, hand, hlate; void *tP; int hk, hnl, hnun;
  double sb[3];   /* QC_STATS_ADAPT: a fast-path plan sample's parts (µs): to its transaction begun, its run, the rest */   /* a plan run qc_adapt timed: its sample, which the handing out of its rows (hand, µs) adds to */
  sqlite3 *db; u8 asMem;        /* asMem: the rows are k's */
} QcServe;
#ifndef QC_MAXFB
#define QC_MAXFB 4   /* a plan's runs in a row that fell back before it is parked for good */
#endif
#ifndef QC_SERVE_KEEP
#define QC_SERVE_KEEP (64u << 20)   /* the largest QcServe a connection keeps for its next prepare (QcConn's big) */
#endif
#ifndef QC_SERVE_MEMROWS
#define QC_SERVE_MEMROWS 16384   /* (56 bytes a value, against 16 kept as QcVal: larger results take the QcVal path) */
#endif
static int qc_lift(Vdbe *v);
SQLITE_API int sqlite3_qc_exec(sqlite3_stmt *pStmt, qc_row_cb cb, void *ctx);
static double qc_now_us(void);
static int qc_ad_detail(void);
static double qc_clock_ns(void);
static int qc_adapt_parked(Vdbe *v);
static void qc_adapt_isample(QcAdapt *A, double t, Vdbe *v);
/* QC_ADAPT_SMALL_US=N (default 0, off): a plan whose fastest sample is under N µs is kept only if it beats the interpreter's
   fastest by QC_ADAPT_SMALL_WIN (1.05), else parked. At 5-8 µs the samples can't see a 5-15 % loss (bench exp1034/1035:
   every small lookup kept at margin 1.0 with the floor off, while box2 timed them 1.04-1.12 of stock), and a parked plan
   now costs nothing a run */
static double qc_small_us(void) { return qcf.smallUs; }
static void qc_stream_drain_all(QcConn *C);
static void qc_stream_kept_free(QcServe *S) {   /* a drained stream's last row's memory, held until its statement steps on */
  for (QvBlk *b = S->keptA, *n; b; b = n) { n = b->next; sqlite3_free(b); }
  S->keptA = 0;
}
/* a finalized statement's QcServe with small buffers, kept for the next prepare's (a prepared lookup: three mallocs and
   frees of about 300 instructions in each prepare's 25k) */
static size_t qc_sink_hw(const QvSink *K) {   /* the Mems that may hold a value: up to the most a run wrote (the reused large QcServe's
                                               16k Mems walked at every finalize cost ha statistics.during_period.hourly 124k
                                               instructions a prepare, +2.8 %) */
  size_t hw = K->hw > K->nv ? K->hw : K->nv; return hw < K->crow ? hw : K->crow;
}
static u32 qc_text_hash(const char *z, int n) { u32 h = 2166136261u; for (int i = 0; i < n; i++) h = (h ^ (u8)z[i]) * 16777619u; return h; }   /* (QcConn's big: a reuse check only) */
static void qc_serve_drop(QcServe *S) { sqlite3_free(S->k.row); sqlite3_free(S->k.val); sqlite3_free(S->k.buf); sqlite3_free(S->mem); sqlite3_free(S); }   /* (its Mems released) */
static void qc_serve_free(Vdbe *v) {
  QcConn *C = QCD(v->db)->pQcConn;
  QcServe *S = QCV(v)->pQcRows; if (!S) return;
  QCV(v)->pQcRows = 0;
  qc_stream_kept_free(S);
  for (int i = 0; i < S->nmem; i++) sqlite3VdbeMemRelease(&S->mem[i]);
  for (size_t i = 0, hw = qc_sink_hw(&S->k); i < hw; i++) sqlite3VdbeMemRelease(&S->k.row[i]);   /* (the Mems past hw never held a value) */
  if (C->nspool < QC_SPOOL && S->k.crow <= 64 && S->k.cbuf <= 4096 && !S->k.val && !S->mem) {
    Mem *row = S->k.row; size_t crow = S->k.crow; char *buf = S->k.buf; size_t cbuf = S->k.cbuf;
    memset(S, 0, sizeof *S); S->k.row = row; S->k.crow = crow; S->k.buf = buf; S->k.cbuf = cbuf;
    C->spool[C->nspool++] = S; return;
  }
  if (v->zSql && S->k.crow * sizeof(Mem) + S->k.cval * sizeof(QcVal) + S->k.cbuf + (size_t)S->nmem * sizeof(Mem) <= QC_SERVE_KEEP) {
    QvSink k = S->k; Mem *mem = S->mem; int nmem = S->nmem;   /* (its Mems released: zMalloc 0) */
    memset(S, 0, sizeof *S);
    S->k.row = k.row; S->k.crow = k.crow; S->k.val = k.val; S->k.cval = k.cval; S->k.buf = k.buf; S->k.cbuf = k.cbuf; S->mem = mem; S->nmem = nmem;
    if (C->big) qc_serve_drop(C->big);   /* (the latest statement's: a text prepared per execution is the one prepared again) */
    C->big = S; C->bigN = (int)strlen(v->zSql); C->bigH = qc_text_hash(v->zSql, C->bigN); return;
  }
  sqlite3_free(S->k.row); sqlite3_free(S->k.val); sqlite3_free(S->k.buf); sqlite3_free(S->mem); sqlite3_free(S);
}
static QcServe *qc_serve_new(Vdbe *v) {
  sqlite3 *db = v->db; QcConn *C = QCD(db)->pQcConn;
  QcServe *S; int n;
  if (C->big && v->zSql && (n = (int)strlen(v->zSql)) == C->bigN && qc_text_hash(v->zSql, n) == C->bigH) { S = C->big; C->big = 0; }   /* (its Mems are the connection's already) */
  else if (C->nspool) { S = C->spool[--C->nspool]; for (size_t i = 0; i < S->k.crow; i++) S->k.row[i].db = db; }   /* (its Mems released: zMalloc 0) */
  else if (!(S = sqlite3MallocZero(sizeof *S))) return 0;
  S->db = S->k.db = db; S->k.cap = QC_SERVE_MEMROWS;
  return S;
}
/* a large result, kept as QcVals (16 bytes a value) and handed out one row at a time through S->mem: its own function,
   out of line, as its loops compile on x86 as before 821d5d7 (inside qc_serve_rows they were 1.2 % slower on calibre
   books_table_columns, bench exp998) */
static int qc_serve_hand(QcServe *S, int nrow, int ncol) {   /* a large result kept: the result row it is handed out through */
  if (S->nmem < ncol) {
    Mem *m = sqlite3_realloc64(S->mem, sizeof *m * (u64)ncol); if (!m) return SQLITE_NOMEM;
    for (int i = S->nmem; i < ncol; i++) sqlite3VdbeMemInit(&m[i], S->db, MEM_Null);
    S->mem = m; S->nmem = ncol;
  }
  S->asMem = 0; S->nrow = nrow; S->ncol = ncol;
  return SQLITE_OK;
}
static __attribute__((noinline)) int qc_serve_vals(QcServe *S, int nrow, int ncol, const QcVal *x) {
  QvSink *K = &S->k; size_t nv = (size_t)nrow * (size_t)ncol, nb = 0;
  if (nv > K->cval) { size_t c = nv * 2; QcVal *t = sqlite3_realloc64(K->val, c * sizeof *t); if (!t) return SQLITE_NOMEM; K->val = t; K->cval = c; }
  for (size_t k = 0; k < nv; k++) {   /* (one pass: a text or blob value keeps its offset in buf, which may still move) */
    QcVal *o = &K->val[k]; *o = x[k];
    if (o->type == SQLITE_TEXT || o->type == SQLITE_BLOB) {
      size_t n = (size_t)o->n;
      if (nb + n + 1 > K->cbuf) { size_t c = (nb + n + 1) * 2 + 64; char *t = sqlite3_realloc64(K->buf, c); if (!t) return SQLITE_NOMEM; K->buf = t; K->cbuf = c; }
      qc_cpy(K->buf + nb, o->z, n);
      K->buf[nb + n] = 0; o->i = (i64)nb; nb += n + 1;
    }
  }
  return qc_serve_hand(S, nrow, ncol);
}
static inline __attribute__((always_inline)) void qc_serve_val_row(QcServe *S) {   /* a large result's next row into S->mem */
  const QcVal *x = &S->k.val[(size_t)S->pos * (size_t)S->ncol]; Mem *m = S->mem; const char *b = S->k.buf;
  /* (the Mems handed out last time need no clearing, as the small path's: column_* conversions leave only a zMalloc, never
     MEM_Dyn or MEM_Agg; m->db is the connection's since sqlite3VdbeMemInit) */
  static const u16 fl[8] = {MEM_Null, MEM_Int, MEM_Real, MEM_Str | MEM_Static | MEM_Term, MEM_Blob | MEM_Static, MEM_Null, MEM_Null, MEM_Null};
  for (int i = 0, nc = S->ncol; i < nc; i++, m++) {
    unsigned t = (unsigned)x[i].type;
    m->flags = fl[t & 7]; m->u.i = x[i].i;   /* (an integer's or a real's 8 bytes alike; types are 1-5) */
    if (t - SQLITE_TEXT < 2) { m->z = (char *)b + x[i].i; m->n = x[i].n; m->enc = SQLITE_UTF8; }   /* (column_text16 may have translated it) */
  }
}
/* a result of up to K->cap values from qcg or a special plan (qvm's write into K themselves): out of line, as the large
   path is. (On x86 with -mbranches-within-32B-boundaries the loop's compare and jump now cross a 32-byte line where they did
   not before, and take a padding nop each value: datasette table.page_default, atuin store.next +2.4k instructions a run
   in callgrind, the same instructions otherwise.) */
static __attribute__((noinline)) int qc_serve_mems(QcServe *S, int nrow, int ncol, const QcVal *x) {
  QvSink *K = &S->k; size_t nv = (size_t)nrow * (size_t)ncol;
  if (nv > K->crow) {   /* (a new statement's: just its rows, as a prepare pays for it; then doubled) */
    size_t c = K->crow ? nv * 2 : nv; if (c > K->cap) c = K->cap;
    Mem *t = sqlite3_realloc64(K->row, c * sizeof *t); if (!t) return SQLITE_NOMEM;
    for (size_t k = K->crow; k < c; k++) { t[k].flags = MEM_Null; t[k].eSubtype = 0; t[k].db = S->db; t[k].szMalloc = 0; }
    K->row = t; K->crow = c;
  }
  /* (the Mems handed out last time need no clearing: column_* conversions leave only a zMalloc, which the Mem keeps and
     reuses, never MEM_Dyn or MEM_Agg; the text buffer grows as it fills, and the values already in it are moved with it) */
  char *b = K->buf, *be = K->buf + K->cbuf; Mem *m = K->row;
  static const u16 fl[6] = {MEM_Null, MEM_Int, MEM_Real, MEM_Str | MEM_Static | MEM_Term, MEM_Blob | MEM_Static, MEM_Null};
  for (size_t k = 0; k < nv; k++, m++) {
    const QcVal *i = &x[k]; unsigned t = (unsigned)i->type;
    m->flags = fl[t < 6 ? t : 0]; m->u.i = i->i;   /* (an integer's or a real's 8 bytes alike; a text's u, unread) */
    if (t - SQLITE_TEXT < 2) {
        size_t n = (size_t)i->n;
        if ((size_t)(be - b) < n + 1) {
          size_t used = (size_t)(b - K->buf), c = (used + n + 1) * 2 + 256; char *o = K->buf, *t = sqlite3_realloc64(o, c); if (!t) return SQLITE_NOMEM;
          if (t != o) for (Mem *w = K->row; w < m; w++) if (w->flags & (MEM_Str | MEM_Blob)) w->z = t + (w->z - o);
          K->buf = t; K->cbuf = c; b = t + used; be = t + c;
        }
        qc_cpy(b, i->z, n); b[n] = 0; m->z = b; m->n = (int)n; b += n + 1; m->enc = SQLITE_UTF8;
    }
  }
  if (nv > K->hw) K->hw = nv;
  S->asMem = 1; S->nrow = nrow; S->ncol = ncol; K->big = 0;   /* (a result that fits: qvm's next run writes into K again) */
  return SQLITE_OK;
}
static int qc_serve_rows(void *ctx, int nrow, int ncol, const QcVal *x) {   /* qc's rows (valid during the call), kept */
  QcServe *S = ctx; QvSink *K = &S->k; size_t nv = (size_t)nrow * (size_t)ncol;
  if (K->on) {   /* qvm's run wrote them into K (x: none) */
    K->on = 0;
    if (!K->vm) { S->asMem = 1; S->nrow = nrow; S->ncol = ncol; return SQLITE_OK; }
    if (K->crow) {   /* (it went past cap this run: a large result's Mems are not kept) */
      for (size_t i = 0, hw = qc_sink_hw(K); i < hw; i++) sqlite3VdbeMemRelease(&K->row[i]);
      sqlite3_free(K->row); K->row = 0; K->crow = 0; K->hw = 0;
    }
    K->big = nv > K->cap;   /* (a result that fits again: Mems from the next run's first row) */
    return qc_serve_hand(S, nrow, ncol);
  } else if (nv <= K->cap) return qc_serve_mems(S, nrow, ncol, x);   /* as Mems: no work per value at each step (bench's corpus, ORDER
                                                                     BY shapes: 25-30 instructions a value in qc_serve_step) */
  return qc_serve_vals(S, nrow, ncol, x);
}
/* the suspended stream's statement, and the read transactions qc began for it (committed when it ends) */
static void qc_stream_end_txn(QcConn *C, int commit) {
  for (int i = 0; i < C->strmNown; i++) if (commit) sqlite3BtreeCommit(C->strmOwn[i]);   /* (else the interpreter's: a replay) */
  C->strmNown = 0;
}
/* the row a suspended stream last handed out, made the statement's own (S->mem) before its run goes on for another
   statement: the run's registers take the next rows, and until the statement steps again its row (and the text or blob a
   column_* call returned for it) stays as SQLite leaves a row, another statement's steps or not. The buffers move to S->mem
   (a pointer handed out stays valid), a value pointing into a page or with a destructor is copied there, and the registers
   get copies (the program may read them again). Out of memory: left as it was */
static void qc_stream_keep_row(Vdbe *v, QcServe *S) {
  Mem *res = QV_CONN(v->db)->x.res; int n = v->nResColumn;
  if (!res || v->pResultRow != res || n <= 0) return;
  if (S->nmem < n) {
    Mem *m = sqlite3_realloc64(S->mem, sizeof *m * (u64)n); if (!m) return;
    for (int i = S->nmem; i < n; i++) sqlite3VdbeMemInit(&m[i], v->db, MEM_Null);
    S->mem = m; S->nmem = n;
  }
  for (int i = 0; i < n; i++) {
    Mem *k = &S->mem[i];
    sqlite3VdbeMemMove(k, &res[i]);
    if (k->flags & (MEM_Ephem | MEM_Dyn)) sqlite3VdbeMemMakeWriteable(k);
    sqlite3VdbeMemCopy(&res[i], k);
  }
  v->pResultRow = S->mem;
}
static void qc_stream_drain_all(QcConn *C) {   /* anything else about to run on its connection: the suspended stream's remaining rows kept */
  Vdbe *v = C->strmV; QcServe *S = QCV(v)->pQcRows; C->strmV = 0; QCD(v->db)->qcBusy = 0;
  qc_stream_keep_row(v, S);
  if (S->tq) { *S->tq = 1e300; S->tq = 0; }   /* (a timed stream's rest, run here untimed: the sample void) */
  QvConn *VC = QV_CONN(v->db);
  sqlite3BtreeEnterAll(v->db);
  VC->keepRun = 1; int st = qv_stream_drain(v->db); VC->keepRun = 0;
  sqlite3BtreeLeaveAll(v->db);
  S->keptA = VC->keptA; VC->keptA = 0;
  S->strm = 4;
  if (st == SQLITE_OK) { qc_stream_end_txn(C, 1); S->pos = 0; if (QC_ST) QC_INC(qcs.drain); }   /* (the rest from the sink: qc_serve_rows set nrow) */
  else { qc_stream_end_txn(C, 0); S->strm = 2; S->nrow = 0; }
}
static void qc_serve_reset(Vdbe *v) {   /* sqlite3QcReset: v reset or finalized */
  QcServe *S = QCV(v)->pQcRows; if (!S) return;
  QcConn *C = QCD(v->db)->pQcConn;
  if (C->strmV == v) { C->strmV = 0; QCD(v->db)->qcBusy = 0; qv_stream_abort(v->db); qc_stream_end_txn(C, 1); }
  qc_stream_kept_free(S); S->strm = 0; S->sent = 0;
}
/* a stream that failed after handing out rows: the interpreter runs the statement from its start, in the same read
   transaction (handed to it), and its first S->sent rows are skipped (a plan that streams has no non-deterministic
   function, qv_streams) */
static int qc_serve_replay(Vdbe *p, QcServe *S) {
  int skip = S->sent; S->strm = 0; S->sent = 0; S->active = 0; QCV(p)->qcServe = 0; p->pc = 0;
  if (QC_ST) QC_INC(qcs.replay);
  while (skip-- > 0) { int rc = sqlite3VdbeExec(p); if (rc != SQLITE_ROW) return rc; }
  return sqlite3VdbeExec(p);
}
#ifndef QC_ADAPT_WARM
#define QC_ADAPT_WARM 3
#endif
enum { QA_LEARN, QA_KEEP, QA_PARK };   /* (qc_adapt's states) */
static double qc_adapt_min_us(void);
static int qc_exec_plan(Vdbe *v, qc_row_cb cb, void *ctx);
static int qc_adapt_on(void);
/* a served statement's next run on the plan its last run served (S->fastP): what qc_serve_start and sqlite3_qc_exec check
   once for a program is not checked again, a kept plan's execution is counted as qc_adapt counts it, and the read
   transaction is begun and ended here (small lookups: about 700 instructions of a 6-10k run went on those layers, anki
   card.get_by_id). 0: not taken (the general path decides) */
static int qc_serve_fast(Vdbe *v, QcServe *S) {
  sqlite3 *db = v->db; void *P = QCV(v)->pQc; int k = *(int *)P;
  QcConn *C = QCD(db)->pQcConn;
  QcAdapt *A = k == 2 ? &((QcGPlan *)P)->ad : &((QvPlan *)P)->ad;
  int t = v->aOp[0].opcode == OP_Init ? v->aOp[0].p2 : v->nOp;
  const Op *o = t + 1 < v->nOp ? &v->aOp[t] : 0;
  if (!o || o->opcode != OP_Transaction || v->aOp[t + 1].opcode == OP_Transaction || o->p2 != 0 || o->p1 < 0 || o->p1 >= db->nDb) { S->fastP = 0; return 0; }
  Btree *bt = db->aDb[o->p1].pBt; if (!bt) { S->fastP = 0; return 0; }
  int ad = qc_adapt_on(), samp = 0;   /* (counted as qc_adapt counts it: a kept plan inside its window, a learning plan's warm-up runs,
                                          and its timed samples, here, where serving it runs: the general path's layers made a plan's
                                          samples 0.4 us dearer than serving it, uptime_kuma important_heartbeat_count parked at 0.72 of
                                          stock in instructions, bench exp1071) */
  if (ad) {
    if (A->st == QA_KEEP && A->n + 1 < A->next) A->n++;
    else if (A->st == QA_LEARN && A->n + 1 <= QC_ADAPT_WARM) A->n++;
    else if (A->st == QA_LEARN && A->nq <= A->ni && A->nq < 4) { A->n++; if (!A->pw) A->pw = 1; else { A->pw = 0; samp = 1; } }   /* (the plan's turn, its first run untimed; the interpreter's is qc_adapt's) */
    else return 0;
  }
  double t0 = samp && qc_ad_detail() ? qc_now_us() : 0, ts = 0, t2 = 0;
  v->iCurrentTime = 0;
  QCD(db)->qcServing = 1; S->nrow = 0;
  if (S->tq) { if (QCV(v)->pQc == S->tP) *S->tq += S->hand; S->tq = 0; }
  sqlite3BtreeEnterAll(db);
  if (db->autoCommit) {   /* (the read transaction OP_Transaction would begin, left to the statement's halt to end, as vdbe.c's:
                             the statement is a reader from sqlite3Step's start, so no other halt commits it before) */
    if (sqlite3BtreeTxnState(bt) == SQLITE_TXN_NONE && sqlite3BtreeBeginTrans(bt, 0, 0) != SQLITE_OK) { sqlite3BtreeLeaveAll(db); QCD(db)->qcServing = 0; S->fastP = 0; if (ad) A->n--; return 0; }
    C->ownTxn = 1;
  }
  if (samp) ts = qc_now_us();   /* (timed from here: the begin is the same work as the interpreter's, untimed there too) */
  QV_CONN(db)->streamOk = 1; qc_why = "";   /* (a sample streams as the plan is served, timed to its first row and then by qc_serve_step) */
  int rc = qc_exec_plan(v, qc_serve_rows, S);
  if (samp && qc_ad_detail()) t2 = qc_now_us();
  QV_CONN(db)->streamOk = 0; C->ownTxn = 0;
  sqlite3BtreeLeaveAll(db);
  QCD(db)->qcServing = 0;
  if (QC_ST) { QC_INC(qcs.exec[k & 3]); if (rc == QC_FALLBACK) qc_st_why(qcs.fb, *qc_why ? qc_why : "?"); else QC_INC(qcs.ok); }
  if (rc == QC_STREAM) {
    S->strm = 1; S->sent = 0; S->nrow = 0; S->ncol = v->nResColumn; S->ready = 1; S->pos = 0; QCV(v)->qcServe = 1;
    C->strmV = v; QCD(db)->qcBusy = 1; if (QC_ST) QC_INC(qcs.stream);
    if (samp) { S->sb[0] = -1; A->tq[A->nq] = qc_now_us() - ts; S->tq = &A->tq[A->nq++]; S->tP = QCV(v)->pQc; S->hand = S->hlate = 0; S->hk = S->hnl = S->hnun = 0; }
    return 1;
  }
  if (rc == SQLITE_OK && S->ncol == v->nResColumn) {
    S->ready = 1; S->pos = 0; QCV(v)->qcServe = 1;
    if (samp) {   /* the sample: this span, and the hand-out (qc_serve_step) */
      double te = qc_now_us(), tt = te - ts;
      if (t0 > 0) { S->sb[0] = ts - t0; S->sb[1] = t2 - ts; S->sb[2] = te - t2; } else S->sb[0] = -1;
      if (A->nq == 0 && tt < qc_adapt_min_us() && !qc_small_us()) { A->st = QA_KEEP; A->next = A->n + 256; if (QC_ST) QC_INC(qcs.cheap); }
      else { A->tq[A->nq] = tt; S->tq = &A->tq[A->nq++]; S->tP = QCV(v)->pQc; S->hand = S->hlate = 0; S->hk = S->hnl = S->hnun = 0; }
    }
    return 1;
  }
  S->fastP = 0;
  if (rc != QC_FALLBACK) sqlite3_reset((sqlite3_stmt *)v);
  return 1;   /* (failed: the interpreter's, as the general path leaves it) */
}
/* B1, the size gate: a program qc can't win on, given back to SQLite at its first start (qcNo: the hook's two-byte test from
   then on, no lift, no adapt learning). A point lookup: no loop, no function, aggregate, subquery, sorter or ephemeral
   table, a few cursors and ops. Its plan's opcodes run no faster than the interpreter's, and serving's fixed set-up
   (qv_exec, the serve layer: 200-500 instructions) is most of a 4-9 k-instruction statement (docs/TOUGH_PROBLEMS.md B1).
   One seek: two (a join through rowids, chromium segment.url_for_segment) qvm wins, 0.92-0.98 */
enum { QG_OK, QG_CUR, QG_RES, QG_SEEK, QG_NO, QG_GOTO };   /* qc_gate_small's opcode classes */
static const u8 qc_gate_cls[256] = {   /* (constant: no state to share between connections) */
  [OP_OpenRead] = QG_CUR, [OP_ReopenIdx] = QG_CUR, [OP_ResultRow] = QG_RES, [OP_Goto] = QG_GOTO,
  [OP_SeekRowid] = QG_SEEK, [OP_NotExists] = QG_SEEK, [OP_SeekGE] = QG_SEEK, [OP_SeekGT] = QG_SEEK, [OP_SeekLE] = QG_SEEK,
  [OP_SeekLT] = QG_SEEK, [OP_Found] = QG_SEEK, [OP_NotFound] = QG_SEEK, [OP_NoConflict] = QG_SEEK,
  [OP_Next] = QG_NO, [OP_Prev] = QG_NO, [OP_SorterNext] = QG_NO, [OP_VNext] = QG_NO, [OP_Rewind] = QG_NO, [OP_Last] = QG_NO,   /* a loop */
  [OP_Function] = QG_NO, [OP_PureFunc] = QG_NO, [OP_AggStep] = QG_NO, [OP_AggStep1] = QG_NO, [OP_AggFinal] = QG_NO,
  [OP_AggValue] = QG_NO, [OP_AggInverse] = QG_NO, [OP_Once] = QG_NO, [OP_InitCoroutine] = QG_NO, [OP_Gosub] = QG_NO,
  [OP_OpenEphemeral] = QG_NO, [OP_OpenAutoindex] = QG_NO, [OP_SorterOpen] = QG_NO, [OP_OpenDup] = QG_NO, [OP_OpenPseudo] = QG_NO,
  [OP_VOpen] = QG_NO, [OP_VFilter] = QG_NO, [OP_Program] = QG_NO, [OP_RowSetAdd] = QG_NO, [OP_RowSetTest] = QG_NO,
  [OP_RowSetRead] = QG_NO, [OP_Yield] = QG_NO,
};
static int qc_gate_small(const Vdbe *v) {
  int off = qcf.noGate;   /* QC_NOGATE=1: every program lifted (A/B) */
  if (off || !v->nOp || v->aOp[0].opcode != OP_Init) return 0;
  int end = v->aOp[0].p2, ncur = 0, nres = 0, nseek = 0;   /* (the body: Init jumps past it to the Transaction) */
  if (end <= 0 || end > QC_GATE_OPS || end > v->nOp) return 0;
  for (int pc = 1; pc < end; pc++) {   /* (out at the first op that disqualifies it, by a table: a prepare of a statement it keeps pays a few ops) */
    const Op *o = &v->aOp[pc];
    switch (qc_gate_cls[o->opcode]) {
      case QG_CUR: if (++ncur > QC_GATE_CUR) return 0; break;
      case QG_RES: if (++nres > 1) return 0; break;
      case QG_SEEK: if (++nseek > 1) return 0; break;
      case QG_NO: return 0;
      case QG_GOTO: if (o->p2 <= pc) return 0; break;   /* (a loop's way back that isn't one of the above) */
    }
  }
  return 1;
}
/* A1, the scan gate: a program whose one loop only reads columns into its result row (Column, Rowid, Copy, RealAffinity,
   index bounds; no filter, function, aggregate or sorter), and seeks in its body (an index's row looked up in its table:
   a DeferredSeek, a rowid seek), has nothing for qvm to win: no region takes a loop that seeks, so qv_run decodes the
   fields the interpreter decodes, and serving adds its glue per row (150-300 instructions). Given back to SQLite at its
   first start, as the size gate does. corpus7 measured 33 of the 55 such statements, none under 1.0 of stock, median
   1.03. A scan without a seek stays: its region reads each row once, and wins (calibre load.books_table_columns 0.92)
   (docs/TOUGH_PROBLEMS.md A1) */
#ifndef QC_SCAN_OPS
#define QC_SCAN_OPS 120   /* (a bigger body isn't scanned) */
#endif
static const u8 qc_scan_cls[256] = {   /* 1: allowed, 2: the loop's Next/Prev, 3: a seek; anything else disqualifies */
  [OP_Next] = 2, [OP_Prev] = 2, [OP_DeferredSeek] = 3, [OP_SeekRowid] = 3, [OP_NotExists] = 3,
  [OP_SeekGE] = 3, [OP_SeekGT] = 3, [OP_SeekLE] = 3, [OP_SeekLT] = 3,
  [OP_OpenRead] = 1, [OP_ReopenIdx] = 1, [OP_Column] = 1, [OP_Rowid] = 1, [OP_Copy] = 1, [OP_SCopy] = 1, [OP_RealAffinity] = 1,
  [OP_ResultRow] = 1, [OP_IdxGT] = 1, [OP_IdxGE] = 1, [OP_IdxLT] = 1, [OP_IdxLE] = 1, [OP_IdxRowid] = 1, [OP_Goto] = 1,
  [OP_Noop] = 1, [OP_Explain] = 1, [OP_Rewind] = 1, [OP_Last] = 1, [OP_FinishSeek] = 1, [OP_Variable] = 1, [OP_Integer] = 1,
  [OP_Int64] = 1, [OP_String8] = 1, [OP_Null] = 1, [OP_Affinity] = 1, [OP_IsNull] = 1, [OP_NotNull] = 1, [OP_MustBeInt] = 1,
  [OP_ColumnsUsed] = 1, [OP_CursorHint] = 1, [OP_DecrJumpZero] = 1, [OP_IfPos] = 1, [OP_OffsetLimit] = 1, [OP_If] = 1,
  [OP_IfNot] = 1, [OP_Halt] = 1,
};
static int qc_gate_scan(const Vdbe *v) {
  if (qcf.noScanGate || !v->nOp || v->aOp[0].opcode != OP_Init) return 0;   /* (QC_NOSCANGATE=1: this gate alone off) */
  int end = v->aOp[0].p2, N = -1, seek = -1;
  if (end <= 0 || end > QC_SCAN_OPS || end > v->nOp) return 0;
  for (int pc = 1; pc < end; pc++) switch (qc_scan_cls[v->aOp[pc].opcode]) {
    case 0: return 0;
    case 2: if (N >= 0) return 0; N = pc; break;
    case 3: seek = pc; break;   /* (the last one: in the body if it's past the loop's top) */
  }
  return N > 0 && seek >= v->aOp[N].p2 && seek < N;
}
static void qc_serve_start(Vdbe *v) {   /* sqlite3QcStart (v is READY, the connection's mutex is held) */
  sqlite3 *db = v->db; QcServe *S = QCV(v)->pQcRows; QcConn *C = qc_conn(db); if (!C) return;
  if (C->strmV && !QCD(db)->qcServing) qc_stream_drain_all(C);   /* (a statement starting: qcNo ones too, while db->qcBusy) */
  if (QCV(v)->qcNo) return;
  QCV(v)->qcServe = 0; if (S) { S->ready = 0; S->strm = 0; if (S->keptA) qc_stream_kept_free(S); }
  if (QCV(v)->qcPark) { QCV(v)->qcPark--; return; }   /* (parked: called only to drain a stream, db->qcBusy) */
  if (QCD(db)->qcServing) return;
  if (S && S->fastP && S->fastP == QCV(v)->pQc && !v->expired && !QC_PROGRESS(db) && !db->mTrace && qc_serve_fast(v, S)) return;
  if (!v->readOnly || !v->zSql) { QCV(v)->qcNo = 1; return; }   /* (for good: fixed for this program) */
  if (!QCV(v)->pQc && (qc_gate_small(v) || qc_gate_scan(v))) { QCV(v)->qcNo = 1; return; }   /* (B1, A1: for good too) */
  if (v->explain || v->expired || QC_PROGRESS(db) || db->mTrace || v->nResColumn == 0) return;
  int noRe = qcf.noReoffer;   /* QC_NOREOFFER=1: a re-prepared statement (a binding's, a schema change's) is left to SQLite, as before c85cc3b (A/B) */
  if (noRe && v->aCounter[SQLITE_STMTSTATUS_REPREPARE]) { QCV(v)->qcNo = 1; return; }
  int ok = QCV(v)->pQc != 0;
  if (!ok) { QCD(db)->qcServing = 1; sqlite3BtreeEnterAll(db); ok = qc_lift(v); sqlite3BtreeLeaveAll(db); QCD(db)->qcServing = 0; }
  if (!ok) { QCV(v)->qcNo = 1; return; }   /* refused: not tried again until a re-prepare (a schema change) gives a new program */
  if (qcf.park &&   /* QC_PARK=1 (A/B): every plan parked for good, as adapt parks one */ qc_adapt_on() && QCV(v)->pQc && *(int *)QCV(v)->pQc != 1) {
    QcAdapt *A = *(int *)QCV(v)->pQc == 2 ? &((QcGPlan *)QCV(v)->pQc)->ad : &((QvPlan *)QCV(v)->pQc)->ad;
    if (A->st != QA_PARK) { A->st = QA_PARK; A->R = 10; A->next = ~0u; }
  }
  if (qc_adapt_parked(v)) return;   /* (the interpreter's, with no read transaction of qc's begun and ended first) */
  /* a plan sample's span: from its read transaction begun (sqlite3_qc_exec) to the end of this, with the commit and qc's layers,
     and then the hand-out, as the interpreter's sample has its steps and Halt, its transaction begun untimed too (timing the
     plan's run alone favoured it, bench exp1034/1035) */
  C->adWant = 0; C->adTs = 0;
  if (qc_adapt_on() && QCV(v)->pQc && *(int *)QCV(v)->pQc != 1) {
    QcAdapt *A = *(int *)QCV(v)->pQc == 2 ? &((QcGPlan *)QCV(v)->pQc)->ad : &((QvPlan *)QCV(v)->pQc)->ad;
    if ((A->st == QA_LEARN || A->n + 1 >= A->next) && A->n + 1 > QC_ADAPT_WARM && A->nq <= A->ni) C->adWant = 1;   /* (a plan sample is likely:
                                      sqlite3_qc_exec takes the time once its read transaction is begun, the begin untimed as the
                                      interpreter's is; timed from here, prep mode's fresh statements, all sampled on this path,
                                      parked datasette facets.expand_fk_labels and ha states_meta.find_ids, 13-16 % cheaper served,
                                      bench exp1076/1077) */
  }
  if (!S) { if (!(S = qc_serve_new(v))) return; QCV(v)->pQcRows = S; }
  QCD(db)->qcServing = 1;
  S->nrow = 0;
  if (S->tq) { if (QCV(v)->pQc == S->tP) *S->tq += S->hand; S->tq = 0; }   /* (the last timed run's rows, handed out) */
  C->adTq = 0;
  C->mayStream = 1; int rc = sqlite3_qc_exec((sqlite3_stmt *)v, qc_serve_rows, S); C->mayStream = 0;
  QCD(db)->qcServing = 0;
  if (rc == QC_ITIMED) {   /* qc_adapt's interpreter sample: SQLite's own run, its steps timed */
    S->strm = 3; S->tA = C->adIA; S->tAP = QCV(v)->pQc; S->it = S->ilate = 0; S->isteps = S->nlate = S->nun = 0; S->tall = 0; S->i0 = 0; QCV(v)->qcServe = 1;
    /* its read transaction begun here, untimed, as qc_serve_fast begins a plan's before timing it: OP_Transaction finds it open
       and the halt commits it, on both sides. The begin (locks, the header read: about 4.5 us of a small lookup's 5.5-6 us on
       box2) is the same work either way, and timed on one side only it decided the verdict (bench exp1074: uptime_kuma
       important_heartbeat_count, its plan's run 0.49 us, parked) */
    int t = v->aOp[0].opcode == OP_Init ? v->aOp[0].p2 : -1;
    if (db->autoCommit && t >= 0 && t < v->nOp && v->aOp[t].opcode == OP_Transaction && v->aOp[t].p2 == 0 && v->aOp[t].p1 >= 0 && v->aOp[t].p1 < db->nDb) {
      Btree *bt = db->aDb[v->aOp[t].p1].pBt;
      if (bt && sqlite3BtreeTxnState(bt) == SQLITE_TXN_NONE) { sqlite3BtreeEnterAll(db); sqlite3BtreeBeginTrans(bt, 0, 0); sqlite3BtreeLeaveAll(db); }
    }
    return;
  }
  if (rc == QC_STREAM) {   /* its first row in its QvX's res */
    S->strm = 1; S->sent = 0; S->nrow = 0; S->ncol = v->nResColumn; S->ready = 1; S->pos = 0; QCV(v)->qcServe = 1;
    C->strmV = v; QCD(db)->qcBusy = 1; if (QC_ST) QC_INC(qcs.stream);
    C->adWant = 0;
    if (C->adTq) { if (C->adTs > 0) *C->adTq = qc_now_us() - C->adTs; S->tq = C->adTq; S->tP = QCV(v)->pQc; S->hand = S->hlate = 0; S->hk = S->hnl = S->hnun = 0; C->adTq = 0; }   /* (a timed
                                       plan run that streams: to its first row, then its steps, qc_serve_step) */
    return;
  }
  C->adWant = 0;
  if (C->adTq) { if (C->adTs > 0) *C->adTq = qc_now_us() - C->adTs; S->tq = C->adTq; S->tP = QCV(v)->pQc; S->hand = S->hlate = 0; S->hk = S->hnl = S->hnun = 0; C->adTq = 0; }
  if (rc == SQLITE_OK && S->ncol == v->nResColumn) {
    S->ready = 1; S->pos = 0; QCV(v)->qcServe = 1;
    S->fastP = !noRe && QCV(v)->pQc && *(int *)QCV(v)->pQc != 1 ? QCV(v)->pQc : 0;   /* (its next run, unless something changed: qc_serve_fast) */
  } else { S->fastP = 0; if (rc != QC_FALLBACK) sqlite3_reset((sqlite3_stmt *)v); }   /* (an error in qc's own run: the statement starts afresh in the interpreter) */
}
/* a streamed statement's next row (qc_serve_step's; out of line, so the served path stays as it was): SQLITE_ROW, or
   SQLITE_DONE when it has ended (qc_serve_step halts it), or the replay's result */
static __attribute__((noinline)) int qc_serve_stream(Vdbe *p, QcServe *S) {
  QcConn *C = QCD(p->db)->pQcConn;
  if (S->sent++ == 0) { p->pResultRow = QV_CONN(p->db)->x.res; return SQLITE_ROW; }
  int failAt = qcf.streamFailAt;   /* (QC_STREAM_FAILAT=N, tests: the replay) */
  sqlite3BtreeEnterAll(p->db);
  int r = failAt && S->sent == failAt + 1 ? (qv_stream_abort(p->db), QC_FALLBACK) : qv_stream_next(p->db);
  sqlite3BtreeLeaveAll(p->db);
  if (r == SQLITE_ROW) { p->pResultRow = QV_CONN(p->db)->x.res; return SQLITE_ROW; }
  C->strmV = 0; QCD(p->db)->qcBusy = 0; S->strm = 0; S->sent--;
  if (r != SQLITE_DONE) { qc_stream_end_txn(C, 0); return qc_serve_replay(p, S); }
  qc_stream_end_txn(C, 1); S->nrow = S->pos = 0;
  return SQLITE_DONE;
}
/* an interpreter sample (qc_adapt's, QC_ITIMED): the statement's own run, each of its first 8 steps timed and every 64th
   after, the untimed ones counted at the later timed ones' mean (a clock read a row would cost a large result's sample
   about 3 %); at its end the sample goes to qc_adapt_isample. Nothing is copied: its cost is a parked statement's */
static __attribute__((noinline)) int qc_serve_itimed(Vdbe *p, QcServe *S) {
  int k = S->isteps++, timed = k < 8 || S->tall || (k & 63) == 0;   /* (every step past the 8th when those took over 1 us each: a clock
                                                                      read is then under 3 %, and one step in 64 stood for the rest of a
                                                                      GROUP BY whose groups differ, wakapi last_active_after's 72 rows read 2 ms
                                                                      dearer than the run and kept a plan 9 % slower, bench exp1237-1238) */
  double t0 = timed ? qc_now_us() : 0;
  int rc = sqlite3VdbeExec(p);
  if (timed) { double dt = qc_now_us() - t0; S->it += dt; if (k >= 8) { S->ilate += dt; S->nlate++; } if (!k) S->i0 = dt; if (k == 7 && S->it - S->i0 > 7.0) S->tall = 1; } else S->nun++;
  if (rc != SQLITE_ROW) {   /* the run's end */
    S->strm = 0; QCV(p)->qcServe = 0;
    if (rc == SQLITE_DONE && QCV(p)->pQc == S->tAP) {
      double t = S->it + (S->nlate ? S->ilate / S->nlate * S->nun : 0);
      if (qc_ad_detail()) fprintf(stderr, "qc adapt sample: interpreter %.2f = first step %.2f rest %.2f us\n", t, S->i0, t - S->i0);
      qc_adapt_isample(S->tA, t, p);
    }
  }
  return rc;
}
static __attribute__((noinline)) void qc_hand_add(QcServe *S, double t0) {
  double dt = qc_now_us() - t0; S->hand += dt;
  if (S->hk > 8) { S->hlate += dt; S->hnl++; }
}
static int qc_serve_step(Vdbe *p) {   /* sqlite3QcServe: in sqlite3Step, in place of sqlite3VdbeExec */
  QcServe *S = QCV(p)->pQcRows;
  if (!S || QCD(p->db)->qcServing) { QCV(p)->qcServe = 0; return sqlite3VdbeExec(p); }   /* (sqlite3Step calls this only while qcServe is set) */
  if (S->strm == 3) return qc_serve_itimed(p, S);
  if (p->pc == 0) {   /* a fresh run: the rows qc_serve_start kept, if any */
    if (!S->ready) { S->active = 0; QCV(p)->qcServe = 0; return sqlite3VdbeExec(p); }
    S->ready = 0; S->active = 1; S->pos = 0; p->pc = 1;   /* (pc 1: being served; sqlite3Step sets 0 at every start) */
  } else if (!S->active || p->pc != 1) { QCV(p)->qcServe = 0; return sqlite3VdbeExec(p); }
  int tk = 0; double t0 = 0;   /* (a timed plan run's hand-out: as an interpreter sample's steps, the first 8 and every 64th) */
  if (S->tq) { int k = S->hk++; tk = k < 8 || (k & 63) == 0; if (tk) t0 = qc_now_us(); else S->hnun++; }
  if (S->strm) {   /* streamed (DONE: ended, halted below), or its replay. A timed run's streamed steps are its hand-out,
                      the plan's work for each row in them: qc_adapt's sample is the whole stream */
    if (S->strm == 4) { qc_stream_kept_free(S); S->strm = 0; }   /* (drained for another statement: the rest from the sink) */
    else {
      if (S->strm == 2) { qc_stream_kept_free(S); if (S->tq) { *S->tq = 1e300; S->tq = 0; } return qc_serve_replay(p, S); }   /* (a replayed sample: the plan's loss) */
      int rc = qc_serve_stream(p, S);
      if (rc == SQLITE_ROW && tk) qc_hand_add(S, t0);
      if (rc != SQLITE_DONE) { if (rc != SQLITE_ROW && S->tq) { *S->tq = 1e300; S->tq = 0; } return rc; }
    }
  }
  if (S->pos < S->nrow) {
    if (S->asMem) {
      p->pResultRow = &S->k.row[(size_t)S->pos * (size_t)S->ncol];
      S->pos++;
      if (tk) qc_hand_add(S, t0);
      return SQLITE_ROW;
    }
    qc_serve_val_row(S);
    S->pos++;
    p->pResultRow = S->mem;
    if (tk) qc_hand_add(S, t0);
    return SQLITE_ROW;
  }
  S->active = 0; QCV(p)->qcServe = 0;   /* done: halted as OP_Halt halts it */
  p->rc = SQLITE_OK; p->errorAction = OE_Abort;
  int nMem = p->nMem; p->nMem = 0;   /* (a served run leaves the program's registers as the last halt left them, released:
                                        closeAllCursors' releaseMemArray over them is skipped; vaultwarden users.find_by_uuid,
                                        377 of 17k instructions a run) */
  int rc = sqlite3VdbeHalt(p);
  p->nMem = nMem;
  if (S->tq) {   /* (qc_adapt's sample: the whole served run, its untimed rows at the later timed ones' mean) */
    if (tk) qc_hand_add(S, t0);
    S->hand += S->hnl ? S->hlate / S->hnl * S->hnun : 0;
    if (QCV(p)->pQc == S->tP) {
      if (qc_ad_detail() && S->sb[0] >= 0) fprintf(stderr, "qc adapt sample: plan %.2f = run %.2f rest %.2f hand-out %.2f us (begin, untimed, %.2f; a clock read %.0f ns)\n",
                                                      *S->tq + S->hand, S->sb[1], S->sb[2], S->hand, S->sb[0], qc_clock_ns());
      *S->tq += S->hand;
    }
    S->tq = 0;
  }
  if (rc == SQLITE_BUSY) { p->rc = SQLITE_BUSY; return SQLITE_BUSY; }
  return p->rc ? SQLITE_ERROR : SQLITE_DONE;
}
#if QC_STOCK
SQLITE_API void sqlite3_qc_serve(int on) { (void)on; }   /* (no hooks in sqlite3_step to serve through) */
#else
SQLITE_API void sqlite3_qc_serve(int on) { sqlite3QcStart = on ? qc_serve_start : 0; sqlite3QcServe = on ? qc_serve_step : 0; sqlite3QcReset = on ? qc_serve_reset : 0; }
#endif
#ifndef QC_SERVE_DEFAULT
#define QC_SERVE_DEFAULT 0
#endif
/* qc_init: what is process-wide, set once (pthread_once) before the first connection's state is made: the hooks that
   keep plans in step with their programs, transactions and connections */
static void qc_free(Vdbe *v);
static void qc_conn_close(sqlite3 *db);
static void qc_init_once(void);   /* (at the end: the settings' defaults are defined throughout) */
#ifdef _WIN32
static INIT_ONCE qc_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK qc_init_cb(PINIT_ONCE o, PVOID p, PVOID *c) { (void)o; (void)p; (void)c; qc_init_once(); return TRUE; }
static void qc_init(void) { InitOnceExecuteOnce(&qc_once, qc_init_cb, NULL, NULL); }
#else
static pthread_once_t qc_once = PTHREAD_ONCE_INIT;
static void qc_init(void) { pthread_once(&qc_once, qc_init_once); }
#endif
static int qc_conn_make(sqlite3 *db) {   /* the connection's QcConn, QgConn and QvConn, in one allocation */
  qc_init();
  size_t a = (sizeof(QcConn) + 15) & ~(size_t)15, b = (sizeof(QgConn) + 15) & ~(size_t)15;
#if QC_STOCK
  QcDs *d = qc_ds(db);
  if (d == &qc_ds_none) {   /* (its client data, freed by sqlite3_set_clientdata if that fails) */
    if (!(d = sqlite3MallocZero(sizeof *d))) return 0;
    d->db = db; if (sqlite3_set_clientdata(db, QC_DS_NAME, d, qc_ds_destroy) != SQLITE_OK) return 0;
  }
  qc_cdb = db; qc_cd = d;
#endif
  char *m = sqlite3MallocZero(a + b + sizeof(QvConn)); if (!m) return 0;
  QCD(db)->pQcConn = m; QCD(db)->pQgConn = m + a; QCD(db)->pQvConn = m + a + b;
  return 1;
}
static void qc_conn_close(sqlite3 *db) {   /* sqlite3QcClose: the connection is being freed (its statements are gone) */
  if (!QCD(db)->pQcConn) return;
  QcConn *C = QCD(db)->pQcConn;
  while (C->nspool) { QcServe *S = C->spool[--C->nspool]; sqlite3_free(S->k.row); sqlite3_free(S->k.buf); sqlite3_free(S); }
  if (C->big) { qc_serve_drop(C->big); C->big = 0; }
  sqlite3_free(C->irVal); sqlite3_free(C->irBuf); sqlite3_free(C->run); sqlite3_free(C->lanes);
  qg_conn_free(QCD(db)->pQgConn); qv_conn_free(QCD(db)->pQvConn);
  sqlite3_free(QCD(db)->pQcConn); QCD(db)->pQcConn = QCD(db)->pQgConn = QCD(db)->pQvConn = 0;
}
__attribute__((constructor)) static void qc_serve_init(void) {   /* QC_SERVE=1 / 0 at startup, else QC_SERVE_DEFAULT */
  const char *e = getenv("QC_SERVE"); int on = e ? *e == '1' : QC_SERVE_DEFAULT;
  if (on) { qc_init(); sqlite3_qc_serve(1); }
}
/* sqlite3QcFree: a statement's program is being deleted (finalize, or the old program of a re-prepare) */
static void qc_free_plan(Vdbe *v) {
  void *P = QCV(v)->pQc; QCV(v)->pQc = 0;
  if (P && QCV(v)->qcPark) {   /* (parked executions counted ahead that this handle won't run: the plan's again, for its next statement) */
    int k = *(int *)P; if (k == 2 || k == 3) { QcAdapt *A = k == 2 ? &((QcGPlan *)P)->ad : &((QvPlan *)P)->ad; A->n -= QCV(v)->qcPark; }
    QCV(v)->qcPark = 0;
  }
  if (P) {
    int k = *(int *)P; const QcAdapt *A = k == 2 ? &((QcGPlan *)P)->ad : k == 3 ? &((QvPlan *)P)->ad : 0;
    if (A && A->st == QA_PARK && A->next == ~0u && v->zSql) qv_neg_put(v, "parked");   /* (B2: parked for good, its verdict kept by its text: a new
                                      handle, a re-prepare, an ORM's prepare per execution is the interpreter's at once, until the schema changes) */
    else if (k != 1 && qv_cache_put(v, P)) P = 0;   /* (general and qvm plans go to the plan cache) */
    if (P) { if (k == 1) qc_free_special(P, v->db); else if (k == 2) qg_free(P, v->db); else qv_free(P, v->db); }
  }
}
static void qc_free(Vdbe *v) {
  qc_free_plan(v);
  sqlite3_free(QCV(v)->pQcTmpl); QCV(v)->pQcTmpl = 0;
  qc_serve_free(v);
}
#if QC_STOCK
static void qc_plan_drop(void *P, sqlite3 *db) {   /* a plan whose program is gone, freed (not cached: its text isn't known) */
  int k = *(int *)P; if (k == 1) qc_free_special(P, db); else if (k == 2) qg_free(P, db); else qv_free(P, db);
}
/* at each API call on v, under the connection's mutex: its QcVs (0: out of memory), checked against its program. Another
   program means SQLite re-prepared it (the same text, a higher REPREPARE count: as sqlite3VdbeSwap, the plan goes and
   qcWant and the template stay) or v is a new statement at a finalized one's address (sqlite3_qc_forget wasn't called:
   all of it goes). Either way the old program is freed, and freeing a plan reads none of it (a general plan holds
   references to its KeyInfos, qvm's kept ephemeral tables close without them) */
static QcVs *qc_vs_enter(Vdbe *v) {
  qc_cur_db(v->db); QcVs *s = qc_vs(v); if (s == &qc_vs_none) return 0;
  qc_cv = v; qc_cs = s;
  u32 rep = v->aCounter[SQLITE_STMTSTATUS_REPREPARE];
  if (s->aOp == v->aOp && s->nRep == rep) return s;
  if (s->zSql == v->zSql && rep > s->nRep) { if (s->pQc) qc_free_plan(v); }
  else {
    if (s->pQc) qc_plan_drop(s->pQc, v->db);
    sqlite3_free(s->pQcTmpl); s->pQcTmpl = 0; s->qcWant = 0;
  }
  s->pQc = 0; s->qcPark = 0; s->qcNo = s->qcServe = 0; s->aOp = v->aOp; s->nRep = rep; s->zSql = v->zSql;
  return s;
}
static void qc_conn_close(sqlite3 *db);
static void qc_ds_destroy(void *p) {   /* the connection is closing (its statements finalized), or sqlite3_qc_close */
  QcDs *d = p; qc_cdb = d->db; qc_cd = d; qc_cv = 0;   /* (sqlite3_close unlinks the client data before calling this) */
  for (u32 i = 0; d->tab && i <= d->mask; i++) {   /* (statements finalized without sqlite3_qc_forget) */
    QcVs *s = d->tab[i]; if (!s) continue;
    if (s->pQc) qc_plan_drop(s->pQc, d->db);
    sqlite3_free(s->pQcTmpl); sqlite3_free(s);
  }
  sqlite3_free(d->tab); d->tab = 0; d->ntab = 0; d->lastV = 0;
  qc_conn_close(d->db);
  qc_cdb = 0; qc_cv = 0; sqlite3_free(d);
}
#endif
/* a statement's plan and template, freed before sqlite3_finalize (QC_STOCK: no hook frees them with its program; in the
   patched build sqlite3QcFree does, and this does nothing) */
SQLITE_API void sqlite3_qc_forget(sqlite3_stmt *pStmt) {
#if QC_STOCK
  Vdbe *v = (Vdbe *)pStmt; if (!v || !v->db) return;
  sqlite3 *db = v->db; sqlite3_mutex_enter(db->mutex);
  QcDs *d = qc_ds(db);
  if (d != &qc_ds_none && d->tab && *qc_vs_at(d, v)) { qc_vs_enter(v); qc_free(v); qc_vs_del(d, v); }
  qc_cdb = 0; qc_cv = 0;
  sqlite3_mutex_leave(db->mutex);
#else
  (void)pStmt;
#endif
}
SQLITE_API int sqlite3_qc_finalize(sqlite3_stmt *pStmt) { sqlite3_qc_forget(pStmt); return sqlite3_finalize(pStmt); }
/* a connection's qc state dropped before sqlite3_close (QC_STOCK; sqlite3_close drops it too, the client data's destructor) */
SQLITE_API void sqlite3_qc_close(sqlite3 *db) {
#if QC_STOCK
  if (db) sqlite3_set_clientdata(db, QC_DS_NAME, 0, 0);
#else
  (void)db;
#endif
}
/* QC_STATS=1: the counts so far, to stderr (also at exit) */
static void qc_st_print_why(const char *what, const QcWhy *w) {
  for (int i = 0; i < 64 && w[i].why; i++) fprintf(stderr, "qc stats: %s %-28s %llu\n", what, w[i].why, w[i].n);
}
static void qc_st_print(void) {
  if (!QC_ST) return;
  fprintf(stderr, "qc stats: exec special %llu general %llu qvm %llu none %llu; ok %llu\n",
          qcs.exec[1], qcs.exec[2], qcs.exec[3], qcs.exec[0], qcs.ok);
  fprintf(stderr, "qc stats: lifts %llu (ok %llu); lifts on execution %llu (failed %llu); cache hits %llu misses %llu; nodes widened %llu\n",
          qcs.lift, qcs.liftOk, qcs.relift, qcs.reliftFail, qv_cache_hits, qv_cache_misses, qcs.widen);
  fprintf(stderr, "qc stats: against the interpreter: %llu samples; kept %llu (cheap %llu); parked %llu, unparked %llu\n",
          qcs.sample, qcs.keep, qcs.cheap, qcs.park, qcs.unpark);
  if (qcs.stream) fprintf(stderr, "qc stats: streamed %llu (drained %llu, replayed %llu)\n", qcs.stream, qcs.drain, qcs.replay);
  qc_st_print_why("fallback", qcs.fb); qc_st_print_why("no lift ", qcs.lf);
  static const char *K[2] = {"column ", "compare"};
  for (int k = 0; k < 2; k++)
    for (int m = 1; m < 32; m++) if (qcs.mask[k][m])
      fprintf(stderr, "qc stats: mask %s %s%s%s%s%s %llu\n", K[k], m & QV_FB_NULL ? "n" : "", m & QV_FB_INT ? "i" : "",
              m & QV_FB_REAL ? "r" : "", m & QV_FB_TEXT ? "t" : "", m & QV_FB_BLOB ? "b" : "", qcs.mask[k][m]);
}
SQLITE_API void sqlite3_qc_stats(void) { qc_init(); qc_st_print(); }
/* the memory qc keeps between executions (run state, arenas, cursor pools) is the connection's, freed when it closes
   (qc_conn_close): nothing is left to release (kept for the callers' leak checks at shutdown) */
SQLITE_API void sqlite3_qc_release(void) {}

/* compile a prepared statement: the specialised feed-shaped plan first, then a general plan. 1 = compiled */
static int qc_lift(Vdbe *v);
SQLITE_API int sqlite3_qc(sqlite3_stmt *pStmt) {   /* (under the connection's locks, as sqlite3_qc_exec) */
  Vdbe *v = (Vdbe *)pStmt; if (!v || !v->db) return 0;
  sqlite3 *db = v->db; sqlite3_mutex_enter(db->mutex); sqlite3BtreeEnterAll(db);
#if QC_STOCK
  qc_cur_db(db); int ok = qc_conn(db) && qc_vs_enter(v) ? qc_lift(v) : (qc_why = "oom", 0);
#else
  int ok = qc_lift(v);
#endif
  sqlite3BtreeLeaveAll(db); sqlite3_mutex_leave(db->mutex);
  return ok;
}
static int qc_lift_do(Vdbe *v);
static void qc_st_print(void);
static int qc_lift(Vdbe *v) {
  qc_why = "";
  int ok = v && v->db && qc_conn(v->db) ? qc_lift_do(v) : (qc_why = "oom", 0);
  if (QC_ST) { QC_INC(qcs.lift); if (ok) QC_INC(qcs.liftOk); else qc_st_why(qcs.lf, *qc_why ? qc_why : "lift"); }
  return ok;
}
/* every field of a program, P4's values too (pointers only where they name a schema object or a built-in, the same in
   two prepares on one connection); 0: a P4 not compared (a subprogram, an expression, a vtab) */
static u64 qc_prog_mix(u64 h, u64 w) { h = (h ^ w) * 0xff51afd7ed558ccdull; return h ^ (h >> 31); }
static u64 qc_prog_hash(const Vdbe *v) {
  u64 h = 0x9e3779b97f4a7c15ull ^ (u64)v->nOp;
  for (int pc = 0; pc < v->nOp; pc++) {
    const Op *o = &v->aOp[pc];
    h = qc_prog_mix(h, (u64)o->opcode | (u64)(u8)o->p4type << 8 | (u64)o->p5 << 16);
    h = qc_prog_mix(h, (u64)(u32)o->p1 | (u64)(u32)o->p2 << 32); h = qc_prog_mix(h, (u64)(u32)o->p3);
    switch (o->p4type) {
      case P4_NOTUSED: break;
      case P4_INT32: h = qc_prog_mix(h, (u64)(u32)o->p4.i); break;
      case P4_STATIC: case P4_DYNAMIC: { const char *z = o->p4.z; size_t n = z ? strlen(z) : 0; h = qc_prog_mix(h, n);
        for (size_t i = 0; i < n; i++) h = qc_prog_mix(h, (u8)z[i]); break; }
      case P4_COLLSEQ: case P4_TABLE: case P4_INDEX: case P4_FUNCDEF: h = qc_prog_mix(h, (u64)(uintptr_t)o->p4.p); break;
      case P4_FUNCCTX: h = qc_prog_mix(h, (u64)(uintptr_t)o->p4.pCtx->pFunc); h = qc_prog_mix(h, (u64)o->p4.pCtx->argc); break;
      case P4_KEYINFO: { const KeyInfo *k = o->p4.pKeyInfo; h = qc_prog_mix(h, (u64)k->nKeyField | (u64)k->nAllField << 16 | (u64)k->enc << 32);
        for (int i = 0; i < k->nAllField; i++) { h = qc_prog_mix(h, (u64)(uintptr_t)k->aColl[i]); h = qc_prog_mix(h, k->aSortFlags[i]); } break; }
      case P4_MEM: { const Mem *m = o->p4.pMem; h = qc_prog_mix(h, m->flags);
        if (m->flags & (MEM_Int | MEM_IntReal)) h = qc_prog_mix(h, (u64)m->u.i);
        else if (m->flags & MEM_Real) { u64 w; memcpy(&w, &m->u.r, 8); h = qc_prog_mix(h, w); }
        else if (m->flags & (MEM_Str | MEM_Blob)) { if (m->flags & MEM_Zero) return 0; h = qc_prog_mix(h, (u64)m->n); for (int i = 0; i < m->n; i++) h = qc_prog_mix(h, (u8)m->z[i]); }
        break; }
      default: return 0;
    }
  }
  return h | 1;
}
/* A statement whose plan looked at a binding (expmask: a LIKE pattern, a bound LIMIT, a 0/1 integer) is re-prepared at
   each new binding, by stock too. Once per text, qc re-prepares it under QPSG's rule (a plan that looks at no binding):
   that program is kept as a hash, and whenever SQLite's own program for the text is that one to the last field (now, or
   at a later re-prepare: one that baked a bound LIMIT in differs) no binding changes it, and expmask is cleared. A
   different program is discarded for SQLite's own (a re-prepare): kept, it could be far worse (bench's corpus, claude-mem's observations_newest_per_key 7.95x stock). bench,
   vaultwarden orgs_with_full_access (access_all = ?3, bound 1): a re-prepare every run, 1.044x stock on x86. */
static void qc_qpsg(Vdbe *v) {
  QcConn *C = QCD(v->db)->pQcConn;
  sqlite3 *db = v->db; int n = (int)strlen(v->zSql); u32 th = qv_hash(v->zSql, n); QcQpsg *e = &C->qpsg[th & 255];
  u64 ph = qc_prog_hash(v); if (!ph) return;
  if (e->db == db && e->h == th) { if (e->prog == ph) v->expmask = v->smimask = 0; return; }
  u64 f = db->flags; db->flags |= SQLITE_EnableQPSG; int rc = sqlite3Reprepare(v); db->flags = f;
  if (rc != SQLITE_OK) return;
  u64 q = qc_prog_hash(v); if (!q) return;
  *e = (QcQpsg){db, th, q};
  if (q == ph) { v->expmask = v->smimask = 0; return; }   /* (sqlite3VdbeSwap carries the old masks over) */
  sqlite3Reprepare(v);   /* SQLite's own plan back */
}
static int qc_lift_do(Vdbe *v) {
  if (!v || QCV(v)->pQc || v->explain || ENC(v->db) != SQLITE_UTF8) return 0;
  QCV(v)->qcWant = 1;   /* lifted again after a re-prepare */
  if (v->expmask && v->aCounter[SQLITE_STMTSTATUS_REPREPARE] >= 2 && v->zSql && !(v->db->flags & SQLITE_EnableQPSG) && v->eVdbeState == VDBE_READY_STATE)
    qc_qpsg(v);   /* (a handle bound again and again: a fresh prepare's first binding re-prepares it once, by stock too, and a handle bound once never again) */
  int onlyVm = qcf.vmOnly;   /* QC_VM_ONLY=1 (tests): every statement through the general executor */
  if (qv_cache_get(v)) return 1;   /* this text's plan, kept from an earlier prepare whose program had the same signature */
  if (qv_neg_get(v)) return 0;     /* refused before: qc_why is why */
  {   /* a program that opens no cursor (a constant SELECT, SELECT of parameters or functions): the interpreter's, as a
         lift and a plan cost more than such a statement does (speedtest1's parsenumber tests: one-shot constant SELECTs) */
    int any = 0;
    for (int pc = 0; pc < v->nOp && !any; pc++) switch (v->aOp[pc].opcode) {
      case OP_OpenRead: case OP_ReopenIdx: case OP_OpenEphemeral: case OP_OpenAutoindex: case OP_SorterOpen: case OP_OpenPseudo: case OP_OpenDup: any = 1;
    }
    if (!any) { qc_why = "no table"; return 0; }
  }
  if (!onlyVm) {
    if (qc_lift_special(v)) return 1;
    if (qc_lift_general(v)) return 1;
  }
  if (qv_lift(v)) return 1;   /* any other read-only program */
  qv_neg_put(v, *qv_why ? qv_why : *qc_why ? qc_why : "refused");
  return 0;
}
static int qc_exec_locked(Vdbe *v, qc_row_cb cb, void *ctx);
static int qc_exec_plan(Vdbe *v, qc_row_cb cb, void *ctx);
static int qc_adapt_on(void);
static int qc_adapt(Vdbe *v, qc_row_cb cb, void *ctx);
/* as sqlite3_step: under the connection's mutex and its B-trees' (shared cache), which are no-ops when SQLite is
   built single-threaded. qc's own run state is the connection's (QcConn), so connections run qc on any threads at once. */
SQLITE_API int sqlite3_qc_exec(sqlite3_stmt *pStmt, qc_row_cb cb, void *ctx) {
  Vdbe *v = (Vdbe *)pStmt; if (!v || !v->db) return QC_FALLBACK;
  sqlite3 *db = v->db; sqlite3_mutex_enter(db->mutex);
#if QC_STOCK
  qc_cur_db(db);
#endif
  QcConn *C = qc_conn(db); if (!C) { sqlite3_mutex_leave(db->mutex); return QC_FB("oom"); }
#if QC_STOCK
  if (!qc_vs_enter(v)) { sqlite3_mutex_leave(db->mutex); return QC_FB("oom"); }
#endif
  if (C->strmV) qc_stream_drain_all(C);
  sqlite3BtreeEnterAll(db);
  if (v->eVdbeState == VDBE_READY_STATE) v->iCurrentTime = 0;   /* as sqlite3Step at a start: 'now' is read afresh once per run
                                                                  (left alone, a new statement read uninitialised memory) */
  /* in autocommit, the read transaction a read-only program needs is begun here and ended after the run, on the
     B-trees that had none, as vdbe.c's OP_Transaction and Halt do: no BEGIN and COMMIT statements around every call
     (a point lookup through a drop-in wrapper: two extra statement runs per lookup) */
  Btree *own[8]; int nown = 0, rc;
  if (db->autoCommit && v->readOnly && v->eVdbeState == VDBE_READY_STATE) {
    int ok = 1, t = v->aOp[0].opcode == OP_Init ? v->aOp[0].p2 : v->nOp;
    if (QCV(v)->pQc && t + 1 < v->nOp && v->aOp[t].opcode == OP_Transaction && v->aOp[t + 1].opcode != OP_Transaction) {
      /* a lifted program: every tier takes exactly one OP_Transaction, the Init block's, so no scan of the program */
      const Op *o = &v->aOp[t]; Btree *bt = o->p1 >= 0 && o->p1 < db->nDb ? db->aDb[o->p1].pBt : 0;
      if (!bt || o->p2 != 0) ok = 0;
      else if (sqlite3BtreeTxnState(bt) == SQLITE_TXN_NONE) { if (sqlite3BtreeBeginTrans(bt, 0, 0) != SQLITE_OK) ok = 0; else own[nown++] = bt; }
    } else for (int pc = 0; pc < v->nOp && ok; pc++) {
      const Op *o = &v->aOp[pc]; if (o->opcode != OP_Transaction) continue;
      Btree *bt = o->p1 >= 0 && o->p1 < db->nDb ? db->aDb[o->p1].pBt : 0;
      if (!bt || o->p2 != 0 || nown == 8) { ok = 0; break; }
      if (sqlite3BtreeTxnState(bt) == SQLITE_TXN_NONE) {
        if (sqlite3BtreeBeginTrans(bt, 0, 0) != SQLITE_OK) { ok = 0; break; }
        own[nown++] = bt;
      }
    }
    if (!ok) { for (int i = 0; i < nown; i++) sqlite3BtreeCommit(own[i]); nown = 0; qc_why = "txn"; rc = QC_FALLBACK; goto done; }
    C->ownTxn = 1;
  }
  if (C->adWant) { C->adTs = qc_now_us(); C->adWant = 0; }   /* (qc_serve_start's plan sample: timed from its transaction begun) */
  rc = qc_exec_locked(v, cb, ctx);
  C->ownTxn = 0;
  if (rc == QC_STREAM) { for (int i = 0; i < nown; i++) C->strmOwn[i] = own[i]; C->strmNown = nown; nown = 0; }   /* (the stream's until it ends) */
#if QC_STOCK
  if (rc != QC_STREAM) qc_stock_txn_end(db);   /* (as the end hook would, for the transaction begun here; and for one begun
                                                   outside: no hook would see it end, so no cursor outlives the run) */
#endif
  for (int i = 0; i < nown; i++) sqlite3BtreeCommit(own[i]);   /* (a read transaction: its locks released, qc's cursors closed by the end hook) */
done:
  if (QC_ST) {
    QC_INC(qcs.exec[QCV(v)->pQc ? *(int *)QCV(v)->pQc & 3 : 0]);
    if (rc == QC_FALLBACK) qc_st_why(qcs.fb, *qc_why ? qc_why : "?"); else QC_INC(qcs.ok);
  }
  sqlite3BtreeLeaveAll(db); sqlite3_mutex_leave(db->mutex);
  return rc;
}
static int qc_exec_locked(Vdbe *v, qc_row_cb cb, void *ctx) {
  qc_why = "";
  if (!QCV(v)->pQc && QCV(v)->qcWant && v->eVdbeState == VDBE_READY_STATE && !v->expired) {   /* re-prepared (the locks are held) */
    int ok = qc_lift(v);
    if (QC_ST) { QC_INC(qcs.relift); if (!ok) QC_INC(qcs.reliftFail); }
    if (!ok) { if (!*qc_why) qc_why = "lift"; return QC_FALLBACK; }
  }
  if (!QCV(v)->pQc) return QC_FB("no plan");
  int k = *(int *)QCV(v)->pQc;
  if (k != 1 && cb && v->eVdbeState == VDBE_READY_STATE && qc_adapt_on()) return qc_adapt(v, cb, ctx);
  QV_CONN(v->db)->streamOk = QC_CONN(v->db)->mayStream; int st = qc_exec_plan(v, cb, ctx); QV_CONN(v->db)->streamOk = 0;
  return st;
}
/* a general or qvm plan's run that fell back, counted: after QC_MAXFB in a row the plan is parked for good (with adapt off,
   the statement refused), so it stops paying for its own work and then the interpreter's. calibre
   annotations.count_for_book's json_extract fell back on every run (turbo-corpus7: 1.18 of stock, unserved) */
static __attribute__((noinline)) int qc_fb_count(Vdbe *v) {   /* a run that fell back: QC_FALLBACK */
  int k = QCV(v)->pQc ? *(int *)QCV(v)->pQc : 0; if (k != 2 && k != 3) return QC_FALLBACK;
  QcAdapt *A = k == 2 ? &((QcGPlan *)QCV(v)->pQc)->ad : &((QvPlan *)QCV(v)->pQc)->ad;
  if (++A->nfb >= QC_MAXFB) {
    if (qc_adapt_on()) { A->st = QA_PARK; A->R = 10; A->next = ~0u; } else QCV(v)->qcNo = 1;
    if (QC_ST) qc_st_why(qcs.fb, "fell back in a row: parked");
  }
  return QC_FALLBACK;
}
/* (the run's result, counted: the served path pays a byte's test, as the call cost a small lookup 40 instructions a run) */
#define QC_FB_NOTE(v, T, st) ((st) == QC_FALLBACK ? qc_fb_count(v) : (QCV((v))->pQc && ((T *)QCV((v))->pQc)->ad.nfb && (((T *)QCV((v))->pQc)->ad.nfb = 0), (st)))
static int qc_exec_plan(Vdbe *v, qc_row_cb cb, void *ctx) {
  int k = *(int *)QCV(v)->pQc;
  if (k == 2) {
    int st = qg_exec(v, QCV(v)->pQc, cb, ctx);
    if (st != QG_DEMOTE) return QC_FB_NOTE(v, QcGPlan, st);
    qg_free(QCV(v)->pQc, v->db); QCV(v)->pQc = 0;   /* more rows than a general plan buffers: qvm has no such limit */
    if (!qv_lift(v)) return QC_FALLBACK;
    { int r = qv_exec(v, QCV(v)->pQc, cb, ctx); return QC_FB_NOTE(v, QvPlan, r); }
  }
  if (k == 1) {
    int st = qc_exec_special(v, cb, ctx);
    QcPlan *Q = QCV(v)->pQc;
    if (st == QC_FALLBACK) { if (++Q->nfb >= 2 || !strcmp(qc_why, "L10")) st = QG_DEMOTE; }   /* (falling back run after
                                     run, or at once for a parameter it can't take: datasette row.select_by_pk's text key for an
                                     integer rowid, L10: 1.6x stock in time unserved, where qvm serves it at 0.97, bench exp1062.
                                     At once, so a prepared statement's first run demotes it, and the qvm plan is cached) */
    else if (st != QG_DEMOTE) Q->nfb = 0;
    if (st != QG_DEMOTE) return st;
    qc_free_special(QCV(v)->pQc, v->db); QCV(v)->pQc = 0;   /* a memory-mapped database, or one it can't serve: qvm */
    if (!qv_lift(v)) return QC_FALLBACK;
    { int r = qv_exec(v, QCV(v)->pQc, cb, ctx); return QC_FB_NOTE(v, QvPlan, r); }
  }
  { int r = qv_exec(v, QCV(v)->pQc, cb, ctx); return QC_FB_NOTE(v, QvPlan, r); }
}
/* The statement run by SQLite's interpreter, here, with its rows handed to cb as a plan hands them (copied out of the
   interpreter's registers): qc_adapt's other arm. *t: the time until the rows are handed over, SQLite's cost alone (a
   program that isn't served doesn't copy its rows; the values are read off the result row, not through column_*, and
   the buffers kept between samples: bench's corpus, 2026-10-28, atuin store.next served at 102 µs against stock's
   93 and never parked). Any error: QC_FALLBACK before a row is handed out, and the caller steps the statement again,
   which reports it */
static int qc_interp_rows(Vdbe *v, qc_row_cb cb, void *ctx, double *t) {
  QcConn *C = QCD(v->db)->pQcConn;
  sqlite3_stmt *s = (sqlite3_stmt *)v; int nc = v->nResColumn, rc, st = QC_FALLBACK;
  size_t nv = 0, nb = 0, nr = 0, nx = 0; double t0 = qc_now_us(), cx = 0, c0 = 0;
  /* the sample is the interpreter's steps alone, as a parked statement runs: the copying of its rows (which a parked
     statement's caller does not do; the plan's sample counts its own, as serving does it) is timed on every 8th row (every
     64th past 64) and taken off, scaled to all of them (kodi top100.songs, 77 values a row: kept at 1.08 of stock with it in, box2 exp1018) */
  while ((rc = sqlite3Step(v)) == SQLITE_ROW) {
    size_t r = nr++; int timed = (r & (r < 64 ? 7 : 63)) == 0; if (timed) c0 = qc_now_us();   /* (past 64 rows every 64th: a clock read costs a sample of 100k rows 0.7 %, bench exp1023) */
    if (nv + (size_t)nc > C->irCv) { size_t c = C->irCv ? C->irCv * 2 : (size_t)nc * 64; QcVal *x = sqlite3_realloc64(C->irVal, c * sizeof *x); if (!x) goto out; C->irVal = x; C->irCv = c; }
    const Mem *m = v->pResultRow;
    for (int i = 0; i < nc; i++, m++) {
      QcVal *o = &C->irVal[nv++]; u16 f = m->flags; o->n = 0; o->i = 0;
      const void *z = 0; int n = 0;
      if ((f & (MEM_Zero | MEM_Str)) == MEM_Str && m->enc == SQLITE_UTF8 && !(f & (MEM_Int | MEM_Real | MEM_IntReal | MEM_Null | MEM_Blob))) { o->type = SQLITE_TEXT; z = m->z; n = m->n; }
      else if ((f & (MEM_Int | MEM_Real | MEM_IntReal | MEM_Null | MEM_Str | MEM_Blob)) == MEM_Int) { o->type = SQLITE_INTEGER; o->i = m->u.i; continue; }
      else if ((f & (MEM_Int | MEM_Real | MEM_IntReal | MEM_Null | MEM_Str | MEM_Blob)) == MEM_Real) { o->type = SQLITE_FLOAT; o->r = m->u.r; continue; }
      else if (f & MEM_Null) { o->type = SQLITE_NULL; continue; }
      else {   /* (anything else, a zeroblob, a value of two types, another encoding: through the API, as before) */
        int ty = sqlite3_column_type(s, i); o->type = ty;
        if (ty == SQLITE_INTEGER) { o->i = sqlite3_column_int64(s, i); continue; }
        if (ty == SQLITE_FLOAT) { o->r = sqlite3_column_double(s, i); continue; }
        if (ty != SQLITE_TEXT && ty != SQLITE_BLOB) continue;
        z = ty == SQLITE_TEXT ? (const void *)sqlite3_column_text(s, i) : sqlite3_column_blob(s, i); n = sqlite3_column_bytes(s, i);
      }
      if (nb + (size_t)n > C->irCb) { size_t c = (nb + (size_t)n) * 2 + 256; char *x = sqlite3_realloc64(C->irBuf, c); if (!x) goto out; C->irBuf = x; C->irCb = c; }
      if (n) memcpy(C->irBuf + nb, z, (size_t)n);
      o->i = (i64)nb; o->n = n; nb += (size_t)n;   /* (an offset until the buffer stops moving) */
    }
    if (timed) { cx += qc_now_us() - c0; nx++; }
  }
  *t = qc_now_us() - t0 - (nx ? cx * (double)nr / (double)nx : 0);
  if (rc != SQLITE_DONE) goto out;
  for (size_t i = 0; i < nv; i++) if (C->irVal[i].type == SQLITE_TEXT || C->irVal[i].type == SQLITE_BLOB) C->irVal[i].z = (const u8 *)C->irBuf + C->irVal[i].i;
  st = cb(ctx, nc ? (int)(nv / (size_t)nc) : 0, nc, C->irVal);
out:
  sqlite3_reset(s);
  if (st == QC_FALLBACK) qc_why = "sample";
  return st;
}
/* Is the plan faster than SQLite's interpreter? A general or qvm plan run with a row callback is measured: after its
   first QC_ADAPT_WARM executions (type feedback, regions), one execution is timed. A plan under QC_ADAPT_MIN_US µs is
   kept without more measurement (and timed again after 256 executions, in case its data grew). Otherwise the next
   executions alternate between four timed runs of the plan and four of the interpreter (qc_interp_rows); two
   interpreter runs both over 1.25 times the slowest plan run end it early. The interpreter's first run is not timed
   (a served handle's program has not run: it allocates its registers and cursors). The fastest of each side's four runs
   are compared (a busy host only adds time): unless the plan's is at least 2 % faster (QC_ADAPT_MARGIN 0.98), the
   statement is parked: QC_FALLBACK ("parked": the
   caller steps it) for the next 64 × 2^R executions, then measured again; after 10 parks it stays parked. A kept plan
   is measured again after 256 × 2^K executions (K up to 8). The state belongs to the plan, so it survives the plan
   cache. QC_NOADAPT=1: off */
#ifndef QC_ADAPT_MIN_US   /* (QC_ADAPT_MIN_US=N in the environment overrides it; 50 until bench's corpus showed plans of 40-200 µs
                             losing ~20 % to the interpreter, never measured: atuin store.next, signal unread.select_unseen; 5
                             until the samples left the read transaction out (09d5bcc, a4788b1): at 5-8 µs a small lookup was
                             never measured and kept at 1.07-1.25 of stock, and now parks, while the wins stay served, box2
                             exp1076-1080) */
#define QC_ADAPT_MIN_US 0
#endif
#ifndef QC_ADAPT_MARGIN   /* (medians and 5 % until the broker's exp994/995: on box2 the verdicts on 10-100 µs plans flipped from
                             run to run, atuin store.next served 5 -> 51 -> 5 %, whatever the margin; 1.03 until bench's sweep,
                             exp1261-1277: once 92b48c2 timed the interpreter fairly, 1.03 kept 49 of 418 statement-modes that
                             then lost over 2 % in cycles, 0.98 keeps 9, at 0.949 of stock's post-verdict cycles against
                             0.961, with no more flips between rounds) */
#define QC_ADAPT_MARGIN 0.98
#endif
#ifndef QC_PARK_FINAL   /* B2: a plan parked this many times and never kept is parked for good (it was the 10th, re-learned after
                           64 x 2^(R-1) executions: 115 of 600 executions of a parked lookup were learning runs) */
#define QC_PARK_FINAL 3
#endif
#ifndef QC_PARK_BASE    /* and its R-th park lasts QC_PARK_BASE x 2^(R-1) executions */
#define QC_PARK_BASE 256u
#endif
static int qc_adapt_on(void) { return qcf.adaptOn; }   /* (QC_NOADAPT=1: off) */
static double qc_adapt_min_us(void) { return qcf.adaptMinUs; }
#ifdef _WIN32
static double qc_now_us(void) {   /* (QueryPerformanceCounter: no clock_gettime with clang-cl or mingw's win32 threads) */
  static double us = 0; LARGE_INTEGER c;
  if (us == 0) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); us = 1e6 / (double)f.QuadPart; }   /* (the same value from every thread) */
  QueryPerformanceCounter(&c); return (double)c.QuadPart * us;
}
#else
static double qc_now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec * 1e6 + (double)t.tv_nsec / 1e3; }
#endif
static int qc_ad_detail(void) { return qcf.adDetail; }   /* (QC_STATS=1 QC_STATS_ADAPT=1: each sample's parts) */
static double qc_clock_ns(void) { return qcf.clockNs; }   /* one clock read's cost (for QC_STATS_ADAPT: qc_init) */
static int qc_adapt_parked(Vdbe *v) {   /* serving: a parked plan's execution counted as qc_adapt counts it, and 1 */
  int k = QCV(v)->pQc ? *(int *)QCV(v)->pQc : 0; if ((k != 2 && k != 3) || !qc_adapt_on()) return 0;
  QcAdapt *A = k == 2 ? &((QcGPlan *)QCV(v)->pQc)->ad : &((QvPlan *)QCV(v)->pQc)->ad;
  if (A->st != QA_PARK || A->n + 1 >= A->next) return 0;
  A->n++; if (QC_ST) qc_st_why(qcs.fb, "parked");
  if (A->n + 1 < A->next) {   /* its further parked executions counted now, and run by sqlite3_step without sqlite3QcStart (v->qcPark):
                                 a parked lookup paid about 120 instructions a run for being asked (sqlite3_step, qc_serve_start) */
    unsigned sk = A->next - 1 - A->n; if (sk > 65535) sk = 65535;
    QCV(v)->qcPark = sk; A->n += sk;
  }
  return 1;
}
/* an interpreter sample of t µs (the first is a warm-up, not counted); four of each side: the verdict */
static void qc_adapt_isample(QcAdapt *A, double t, Vdbe *v) {
  if (!A->iw) { A->iw = 1; return; }   /* (the program's first run on a served handle: its registers and cursors new) */
  A->ti[A->ni++] = t; if (QC_ST) QC_INC(qcs.sample);
  double qmax = 0, imin = 1e300, imax = 0;
  for (int j = 0; j < A->nq; j++) if (A->tq[j] > qmax) qmax = A->tq[j];
  for (int j = 0; j < A->ni; j++) { if (A->ti[j] < imin) imin = A->ti[j]; if (A->ti[j] > imax) imax = A->ti[j]; }
  if (A->ni == 2 && imin > qmax * 1.25) A->ni = 4, imax = 0, imin = 1e300;   /* clearly slower: keep the plan now */
  if (A->ni == 4) {
    double q = 1e300, i = imin > imax ? 1e300 : imin;   /* (the fastest run of each: interference only adds time) */
    for (int j = 0; j < 4; j++) if (A->tq[j] < q) q = A->tq[j];
    double margin = qcf.margin;   /* (QC_ADAPT_MARGIN=1.0: park a plan any slower) */
    if (qc_ad_detail()) fprintf(stderr, "qc adapt: plan %.1f interpreter %.1f us (fastest of 4): %s: %.60s\n", q, i, i * margin < q ? "park" : "keep", v->zSql ? v->zSql : "");
    double smallWin = qcf.smallWin;
    if (i * margin < q || (q < qc_small_us() && q * smallWin > i)) {   /* the plan's fastest run not 2 % under the interpreter's fastest (QC_ADAPT_MARGIN);
                                                                            a plan under QC_ADAPT_SMALL_US µs kept only if it wins by QC_ADAPT_SMALL_WIN */
      A->st = QA_PARK; if (A->R < 255) A->R++;
      A->next = A->R >= (A->K ? 10 : QC_PARK_FINAL) ? ~0u : A->n + (QC_PARK_BASE << (A->R - 1)); if (QC_ST) QC_INC(qcs.park);   /* (the R-th park: QC_PARK_BASE x 2^(R-1)
                                      executions; for good at the QC_PARK_FINAL-th when every verdict parked, else the 10th) */
    } else {
      A->st = QA_KEEP; A->next = A->n + (256u << (A->K < 8 ? A->K : 8)); if (A->K < 255) A->K++;
      if (QC_ST) QC_INC(qcs.keep);
    }
  }
}
static int qc_adapt(Vdbe *v, qc_row_cb cb, void *ctx) {
  QcConn *C = QCD(v->db)->pQcConn;
  QcAdapt *A = *(int *)QCV(v)->pQc == 2 ? &((QcGPlan *)QCV(v)->pQc)->ad : &((QvPlan *)QCV(v)->pQc)->ad;
  A->n++;
  if (A->st == QA_PARK) {
    if (A->n < A->next) return QC_FB("parked");
    A->st = QA_LEARN; A->nq = A->ni = A->iw = A->pw = A->wt = 0; if (QC_ST) QC_INC(qcs.unpark);
  } else if (A->st == QA_KEEP) {
    if (A->n < A->next) { QV_CONN(v->db)->streamOk = C->mayStream; int st = qc_exec_plan(v, cb, ctx); QV_CONN(v->db)->streamOk = 0; return st; }   /* (kept, not timed: it may stream) */
    A->st = QA_LEARN; A->nq = A->ni = A->iw = A->pw = A->wt = 0;
  }
  if (A->n <= QC_ADAPT_WARM) return qc_exec_plan(v, cb, ctx);
  if (A->nq > A->ni) {   /* the interpreter's turn */
    if (cb == qc_serve_rows && v->eVdbeState == VDBE_READY_STATE) {   /* served: the statement runs in the interpreter as a parked one
                                      does, its steps timed (qc_serve_step), nothing copied; each turn's first run untimed, so the
                                      side timed runs warm, as a statement served or parked run after run does (bench exp1072: a plan
                                      turn after an interpreter turn read 0.2-0.3 us dearer than serving it) */
      if (!A->wt) { A->wt = 1; return QC_FB("sample warm-up"); }
      A->wt = 0; C->adIA = A; return QC_ITIMED;
    }   /* (sqlite3_qc_exec's callers: qc_interp_rows) */
    double t = 0; int st = qc_interp_rows(v, cb, ctx, &t);
    if (st == QC_FALLBACK) return st;
    qc_adapt_isample(A, t, v);
    return st;
  }
  if (cb == qc_serve_rows && !A->pw) { A->pw = 1; QV_CONN(v->db)->streamOk = C->mayStream; int st = qc_exec_plan(v, cb, ctx); QV_CONN(v->db)->streamOk = 0; return st; }   /* (the plan's
                                     turn warmed, as the interpreter's) */
  A->pw = 0;
  void *P0 = QCV(v)->pQc;
  QV_CONN(v->db)->streamOk = C->mayStream;   /* (timed as it is served: a plan that streams, streamed, its steps timed by qc_serve_step; it
                                     sampled unstreamed and parked, calibre books_table_columns in prep, box2 exp1103) */
  double t = qc_now_us(); int st = qc_exec_plan(v, cb, ctx); t = qc_now_us() - t;
  QV_CONN(v->db)->streamOk = 0;
  if (st == QC_FALLBACK || QCV(v)->pQc != P0) return st;   /* (a general plan that demoted to qvm was freed with its state) */
  double minUs = qc_adapt_min_us();
  if (A->nq == 0 && t < minUs && !qc_small_us()) { A->st = QA_KEEP; A->next = A->n + 256; if (QC_ST) QC_INC(qcs.cheap); return st; }   /* (with the small-plan rule on, a short plan is measured too) */
  A->tq[A->nq] = t; C->adTq = &A->tq[A->nq++];   /* (served: the handing out of its rows is added, qc_serve_step) */
  return st;
}
/* qc_init's: the environment's switches into qcf (and qcs.on), qv_region's table, the hooks */
static void qc_init_once(void) {
  const char *e;
  qcs.on = getenv("QC_STATS") != 0; if (QC_ST) atexit(qc_st_print);
  qcf.noReoffer = getenv("QC_NOREOFFER") != 0; qcf.noGate = getenv("QC_NOGATE") != 0; qcf.noScanGate = qcf.noGate || getenv("QC_NOSCANGATE") != 0; qcf.park = getenv("QC_PARK") != 0; qcf.vmOnly = getenv("QC_VM_ONLY") != 0;
  qcf.streamFailAt = (e = getenv("QC_STREAM_FAILAT")) ? atoi(e) : 0;
  qcf.adaptOn = getenv("QC_NOADAPT") == 0; qcf.adDetail = QC_ST && getenv("QC_STATS_ADAPT") != 0;
  qcf.smallUs = (e = getenv("QC_ADAPT_SMALL_US")) ? atof(e) : 0;
  qcf.adaptMinUs = (e = getenv("QC_ADAPT_MIN_US")) ? atof(e) : QC_ADAPT_MIN_US;
  qcf.margin = (e = getenv("QC_ADAPT_MARGIN")) && atof(e) > 0 ? atof(e) : QC_ADAPT_MARGIN;
  qcf.smallWin = (e = getenv("QC_ADAPT_SMALL_WIN")) && atof(e) > 0 ? atof(e) : 1.05;
  if (qcf.adDetail) {   /* one clock read's cost: the fastest of 64 back-to-back pairs */
    double best = 1e9; for (int i = 0; i < 64; i++) { double a = qc_now_us(), b = qc_now_us(); if (b - a < best) best = b - a; }
    qcf.clockNs = best * 1e3;
  }
  qcf.genLookups = getenv("QC_GEN_LOOKUPS") != 0;
  qcf.numColInt = (e = getenv("QV_NUMCOL")) && !strcmp(e, "int"); qcf.noHash = getenv("QV_NOHASH") != 0; qcf.noTopn = getenv("QV_NOTOPN") != 0;
  qcf.rgOn = getenv("QV_NOREGION") == 0; qcf.cacheOn = QC_CACHE_PLANS > 0 && !getenv("QC_NOCACHE"); qcf.noStamp = getenv("QC_NOSTAMP") != 0;
  qcf.streamMin = (e = getenv("QC_STREAM_MIN")) ? (unsigned)atoi(e) : QV_STREAM_MIN;
  qcf.streamOn = (e = getenv("QC_STREAM")) ? *e == '1' : QV_STREAM_DEFAULT;
  qv_region(0, 0, 0);   /* (qv_rgl: its handlers' table) */
  sqlite3QcTxnEnd = qc_txn_end_all; sqlite3QcFree = qc_free; sqlite3QcClose = qc_conn_close;
}

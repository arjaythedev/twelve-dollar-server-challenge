/* qvm.c: qc's general executor. #included by qc.c after the general plans (qcg.c).
**
** The plans in qc.c and qcg.c lift known shapes into rewritten programs. Everything else read-only (coroutines,
** compound SELECTs, CTEs, window functions, DISTINCT, sorter GROUP BY, subqueries of any kind) runs here: the
** statement's own VDBE program, opcode by opcode, with registers that are SQLite Mems, as vdbe.c runs it. What
** makes it cheaper than vdbe.c:
**   columns    text and blobs read from the database stay in its pages, pinned until the execution ends, as
**              MEM_Static values: no copy into each register (vdbe.c's OP_Column copies them) and none on Copy.
**              Past QV_PIN_CHEAP pinned pages (a scan over many pages) values are copied as vdbe.c does;
**   rows       ResultRow appends to a row buffer that is handed out once, when the program halts: no
**              sqlite3_step() round trip and no sqlite3_column_*() call per value;
**   cursors    database cursors stay open for the whole read transaction, registers and function contexts for the
**              life of the statement;
**   natives    count() accumulates in a plain integer, length() of text runs in line, the sorter is an in-memory
**              stable merge sort on keys decoded once (what SQLite's in-memory sorter computes).
**   sets       an ephemeral index that is only probed and added to (DISTINCT) is a hash set of its keys;
**   top-N      the ORDER BY ... LIMIT N sorter (an ephemeral index kept at N entries) is a sorted array of records,
**              moved into a B-tree past QV_TOPN_MAX entries;
**   regions    after two runs of type feedback, single-cursor loop bodies run as specialised direct-threaded nodes
**              (see the regions section below).
** Everything else is vdbe.c's own code path: the opcode bodies follow vdbe.c, comparisons and affinities call its
** helpers on the registers, ephemeral tables are SQLite's in-memory B-trees with SQLite records, functions and
** aggregates (window ones included) are called through their FuncDefs with the registers as arguments.
** The program is checked opcode by opcode when the statement is lifted; at run time anything not reproduced exactly
** (an error, a write to a real table, a zeroblob, an oversized result) returns QC_FALLBACK before any row is handed
** out, so the caller steps the statement instead. Ephemeral state is private to the execution: nothing to undo. */

#define QV_ARENA_MAX (256u << 20)
#ifndef QV_MAXPINS   /* distinct database pages one execution may hold pinned (each once; past it: QC_FALLBACK, "pins"). -DQV_MAXPINS=N */
#define QV_MAXPINS 65536
#endif
#if QV_MAXPINS < 256
#error "QV_MAXPINS must be at least 256 (the pin array starts at 256 entries)"
#endif
#define QV_MAXARGS 32
#if defined(__GNUC__)
#define QV_ALIGN64 __attribute__((aligned(64)))
#else
#define QV_ALIGN64
#endif
#if defined(__GNUC__)
#define QV_INLINE inline __attribute__((always_inline))
#define QV_NOINLINE __attribute__((noinline))
#define QV_COLD __attribute__((noinline, cold))
#else
#define QV_INLINE inline
#define QV_NOINLINE
#define QV_COLD
#endif

static _Thread_local const char *qv_why = "";   /* (as qc_why: this thread's last) */

/* ---------------------------------------------------------------- arena */
typedef struct QvBlk { struct QvBlk *next; size_t used, cap; unsigned idle; char d[]; } QvBlk;   /* idle: runs a spare went unused */
typedef struct QvArena { QvBlk *head; size_t total; QvBlk *spare; size_t nspare; } QvArena;   /* spare: earlier runs' blocks, free */
#ifndef QV_ARENA_KEEP
#define QV_ARENA_KEEP (256u << 20)   /* the spare blocks a connection keeps between runs, at most */
#endif
#ifndef QV_ARENA_IDLE
#define QV_ARENA_IDLE 16   /* runs a spare may go unused before it is freed */
#endif
static void *qv_alloc(QvArena *A, size_t n) {
  n = (n + 15) & ~(size_t)15;
  QvBlk *b = A->head;
  if (!b || b->used + n > b->cap) {
    size_t cap = A->total > (64u << 10) ? A->total : (64u << 10); if (cap < n) cap = n;   /* (a new block's size: at least what the
                                                                                          arena holds already, so a run's blocks double, and n) */
    QvBlk **pb = 0, **pf = 0;   /* an earlier run's block first: the smallest spare of that size or more, else the largest that takes n
                                   (a small statement doesn't take a big one's block, which then ages out if nothing needs it) */
    for (QvBlk **q = &A->spare; *q; q = &(*q)->next) {
      size_t c = (*q)->cap;
      if (c >= cap && (!pb || c < (*pb)->cap)) pb = q;
      if (c >= n && (!pf || c > (*pf)->cap)) pf = q;
    }
    if (!pb) pb = pf;
    if (pb) { b = *pb; *pb = b->next; A->nspare -= b->cap; }
    else {
      if (A->total + cap > QV_ARENA_MAX) cap = n > (64u << 10) ? n : (64u << 10);
      if (A->total + cap > QV_ARENA_MAX) return 0;
      b = sqlite3_malloc64(sizeof *b + cap + 16); if (!b) return 0;   /* 16 bytes of slack: vector loads */
      b->cap = cap;
    }
    b->next = A->head; b->used = 0; A->head = b; A->total += b->cap;
  }
  void *p = b->d + b->used; b->used += n; return p;
}
/* a run's blocks kept as spares for the next runs, up to QV_ARENA_KEEP (the smallest freed first past it): the next run of
   this statement, or of another one on the connection, takes them before it allocates. Keeping only the largest block, a
   run past it allocated (mmap past glibc's 128 KB) and freed (munmap) a block of that size every run, its pages faulted in
   again: bench exp1183, box2's glibc, one 4 MiB block a run, 513-643 page faults (freshrss entry_count_per_day, zotero
   load_creators); macOS's allocator caches such a block, so locally there were none. One block of a run's whole size
   (f8bf4d9, reverted) was re-made whenever the statements of one connection needed different sizes. A spare unused for
   QV_ARENA_IDLE runs is freed, so a statement that keeps needing a lot keeps its blocks (calibre view.meta: about 128 MB a
   run, past a 32 MB cap, 26-32k page faults a run, a quarter of its time) and a one-off gives them back soon after */
static void qv_arena_reset(QvArena *A) {
  for (QvBlk **q = &A->spare; *q; ) {   /* (the spares this run didn't take: one run idler; past QV_ARENA_IDLE, freed) */
    QvBlk *x = *q;
    if (++x->idle > QV_ARENA_IDLE) { *q = x->next; A->nspare -= x->cap; sqlite3_free(x); } else q = &x->next;
  }
  for (QvBlk *x = A->head, *y; x; x = y) { y = x->next; x->used = 0; x->idle = 0; x->next = A->spare; A->spare = x; A->nspare += x->cap; }
  A->head = 0; A->total = 0;
  while (A->nspare > QV_ARENA_KEEP) {
    QvBlk **ps = &A->spare;
    for (QvBlk **q = &A->spare; *q; q = &(*q)->next) if ((*q)->cap < (*ps)->cap) ps = q;
    QvBlk *x = *ps; *ps = x->next; A->nspare -= x->cap; sqlite3_free(x);
  }
}

/* ---------------------------------------------------------------- plan and run state */
enum { QVC_NONE, QVC_BT, QVC_EPH, QVC_SORT, QVC_PSEUDO, QVC_NULLONLY, QVC_VEC, QVC_HASH, QVC_TOPN };
/* an ephemeral rowid table held as a vector (QVC_VEC): entries in rowid order, records in the arena, deletes as
   tombstones. Used for the tables whose cursors only append (NewRowid / Insert), scan, seek by rowid and delete:
   window frames, materialized and recursive CTEs. Anything outside what it reproduces deoptimises the plan to B-trees. */
typedef struct QvVecEnt { i64 rowid; const u8 *rec; u32 len, dead; } QvVecEnt;
typedef struct QvVec { QvVecEnt *e; int n, cap, first, live; struct QvCur *ref[8]; int nref; } QvVec;   /* ref: the cursors on it, its owner and OpenDups (nref 9: more, look at every cursor) */
/* a small ephemeral index held as a sorted array (QVC_TOPN): the ORDER BY ... LIMIT N sorter (Last, an Idx compare
   against the last entry, Delete, IdxInsert, then Sort and a scan). Records in the arena, at most QV_TOPN_MAX of
   them; past that, or on anything else, the plan deoptimises to a B-tree */
#ifndef QV_TOPN_MAX
#define QV_TOPN_MAX 256
#endif
typedef struct QvTopn { const u8 *rec[QV_TOPN_MAX]; u32 len[QV_TOPN_MAX]; int n; UnpackedRecord *u; } QvTopn;
typedef struct QvHset { u64 *hv; const u8 **rec; u32 n, cap; } QvHset;   /* open addressing; rec: the inserted records (0: empty) */
typedef struct QvSort { const u8 **rec; u32 *len; QcVal *key; int n, cap, pos, nk, nf, cur; KeyInfo *ki; i64 mem, lim, mn, copied; VdbeCursor *vc; } QvSort;   /* key: nk decoded fields per record; mem, lim, mn: see qv_sort_limits; nf: SorterOpen's P3; cur: its cursor; vc: SQLite's own sorter, once the records outgrew memory (qv_sort_spill) */
/* a cursor's overflow-row state, out of QvCur (whose size every cursor pays): an overflow row's field count parsed
   and record size, and the last large field read from overflow pages (an RCStr) and where it was (qv_colbig) */
typedef struct QvOvf { int ovN; u32 ovPay; char *big; i64 bigCell; u32 bigOff, bigRun; } QvOvf;
typedef struct QvCur {
  /* first, what OP_Column and the region nodes read (one cache line): the state, the B-tree cursor, the current row
     and how far its header is parsed */
  u8 kind, nullRow, deferred, rowOk, copyVals, ovfl;   /* ovfl: the row is an overflow record, C->row its cell part (qv_hdr_ovfl, qv_colbig) */
  int pseudoReg, nField, nHdr; u32 szRow, payload, hdrOff;
  const u8 *row; u32 *aType, *aOff;
  BtCursor *bc;
  DbPage *pinned, *rowPage;               /* rowPage: the database page the row's record is in (pinned once a value points into it) */
  u32 rowGen;                             /* ephemeral: the X->ephGen the cached row was read at */
  u8 isTable, eqOnly, noReuse, owner;     /* owner: opened the ephemeral B-tree */
  u16 ep5;                                /* OpenEphemeral's P5 (B-tree flags) */
  QvVec *vec; int vi, rowVi; u8 vbad;     /* QVC_VEC: the entry the cursor is on; vbad: 1 past the end, 2 unknown position */
  KeyInfo *ki;
  Btree *ebt; Pgno eroot, cpgno;          /* ephemeral: its B-tree (BTREE_SINGLE: closed with its last cursor); cpgno: see cpg */
  i64 movetoTarget; struct QvCur *alt; const int *altMap;
  int seekResult, seekHit; i64 seqCount;
  QvSort *srt;
  QvHset *hs; int hk, capField;           /* QVC_HASH: the set; its key's field count. capField: aType / aOff are kept (malloc) between executions */
  QvTopn *tn;                             /* QVC_TOPN: the entries (vi, vbad as for QVC_VEC) */
  const MemPage *cpg; const u8 *clo, *cend;      /* QVC_BT: the page whose cell area [clo, cend) qv_idxcmp holds (cleared every run) */
  struct QvOvf *ov;                       /* overflow rows (qv_row_ovfl): allocated at the first, kept with the cursor */
} QV_ALIGN64 QvCur;   /* (P->c is 64-byte aligned: every cursor's first line is its own) */
static void qv_cur_reset(QvCur *C) { u32 *t = C->aType, *o = C->aOff; int cap = C->capField; QvOvf *ov = C->ov; if (ov && ov->big) { sqlite3RCStrUnref(ov->big); ov->big = 0; } memset(C, 0, sizeof *C); C->aType = t; C->aOff = o; C->capField = cap; C->ov = ov; }
typedef struct QvPlan {
  int kind;                                /* 3 */
  int iDb, cookie, gen, nout;
  int nMem, nCur, nOp;
  Btree *bt; u8 txnChecked; int slot, act;   /* (QC_LIST) */
  BtCursor **bc; Pgno *bcRoot; u8 *bopen; /* database cursors by cursor number, open for the transaction */
  /* kept between executions, as a Vdbe keeps them: registers (with their buffers), cursors with their header
     arrays, per-opcode function contexts */
  Mem *r; struct QvCur *c; void *cmem; void **octx; u8 *once; u8 hasOnce, hasAcc;   /* cmem: the allocation c is aligned in; hasOnce: an OP_Once to reset; hasAcc: a count or sum kept in cnt / sacc */
  u8 *isCnt; i64 *cnt;                    /* accumulators only ever count(): a native counter (countStep / countInverse) */
  /* ephemeral B-trees kept open between executions, by cursor number: emptied (ClearTable) when the program opens
     the cursor again, which is what vdbe.c does when a cursor is reopened within one run */
  struct QvEphKeep { Btree *bt; BtCursor *bc; Pgno root; KeyInfo *ki; u16 p5; } *keep;
  u8 *isVec, vecOff;                      /* cursor numbers whose ephemeral table can be a vector; vecOff: a run deoptimised */
  u8 *isHash, hashOff;                    /* ephemeral indexes only probed and added to: a hash set of keys of isHash[c] fields */
  u8 *isTopn, topnOff;                    /* ephemeral indexes used as a top-N sorter: a sorted array (QVC_TOPN) */
  u8 *srtSkip;                            /* sorters by cursor: runs left that sort without hashing (qv_sort_hash) */
  /* accumulators only ever sum() / total() / avg() (1 2 3): integers natively; the first non-integer or overflow hands
     the state to SQLite's own SumCtx in the register and sumStep / sumInverse from then on */
  u8 *isSum; struct QvSum { i64 iSum, cnt; u8 spilled; } *sacc;
  u8 *isAcc;                              /* isCnt or isSum: a native accumulator (qv_out's one test) */
  /* type feedback, by opcode: the value classes (QV_FB_*) seen; Column: the value it produced; comparisons and
     arithmetic: both operands; AggStep: its argument */
  u8 *fb; u32 runs;
  /* Bloom filters (OP_Filter), when the program has any: a run's probes and rejections by opcode, the Filter opcodes'
     pcs, and by filter register how many of them probe it this run. A Filter that probed 1024 times in a run and
     rejected nothing is not probed in the next runs (a filter can only skip work, and hashing every probe costs more
     than a filter that never rejects saves); every 16th run probes them all again. Decided between runs: within one,
     a filter that rejects nothing early can reject most probes later (Gitea's issue search). A register no Filter
     probes in a run is not built in it (FilterAdd) */
  struct QvFlt { u32 n, hit; u8 off; } *flt; u16 *fltOn; int *fpc, nfpc;
  struct QvRegion *rg; int nrg; int *rgAt;   /* loop regions (step 4 of docs/IR.md); rgAt: by the pc of the loop's Next, index + 1 */
  u8 *sig; u32 nsig;                       /* the program it was lifted from (qv_sig; 0: not cacheable) */
  QvStamp stamp;                           /* the state that program was generated in (qv_stamp) */
  char *csql; int ncsql; u32 csh;           /* its text in the plan cache, kept while the plan is out (qv_cache_get, _put) */
  QcAdapt ad;                              /* measured against the interpreter (qc_adapt) */
  u8 strmChecked, strmOk;                  /* qv_streams' answer */
  int *ctxPc, nctx;                        /* the ops octx has a function context for (qv_ctx adds them): a cached plan's attach and
                                              release walk these, not every op (about 160 instructions a prepared lookup) */
  u32 rowsLast;                            /* rows its last run handed out (a stream counts its own: X->nrow) */
  u8 sinkTxt;                              /* its last result kept by qc_serve had texts or blobs (streamed: as it was) */
  u32 ticksLast;                           /* its last run's tick checks (a loop's 64 iterations, a page a region entered) */
  u8 mayDyn;                               /* an op that can leave a register MEM_Dyn or MEM_Agg (a function, an aggregate, a
                                              RowSet): the registers are released after every run */
  u8 sinkBig;                              /* its last result kept by qc_serve was large: a new statement's first run writes
                                              QcVals from its first row (grafana unified.resource_list prep: +387k a prepare
                                              writing 16,384 Mems and turning them into QcVals, bench exp1011) */
} QvPlan;
/* qc_serve's kept rows (the first member of its QcServe, whose callback is qc_serve_rows): a ResultRow is written straight
   into them, its texts copied once into buf, where the QcVal path copies a text into the arena and serving copies it
   again (kodi top100.songs: 100 rows of 77 values, 108k instructions a run). Up to cap values as the result row's Mems; a
   larger result as QcVals (vm) in the form qc_serve_val_row hands out, a text as its offset into buf (calibre
   load.books_table_columns: 220k values, 13.6M instructions a run in the arena, its copies and qc_serve_vals). */
typedef struct QvSink {
  Mem *row; size_t crow, nv, cap;   /* the Mems (crow allocated); nv values written this run; at most cap of them as Mems */
  size_t hw;                        /* the most values a run has written so far: the Mems past it never held one (released only up to it) */
  QcVal *val; size_t cval;          /* a large result's values (vm) */
  char *buf; size_t cbuf, used;     /* the texts and blobs */
  sqlite3 *db; u8 on, vm, big;      /* on: written by the run that called back (qv_exec); vm: as QcVals; big: the last result
                                       was past cap, so a run writes QcVals from its first row (grafana unified.resource_list:
                                       16,384 values written as Mems and turned into QcVals every run, +2.6 %) */
} QvSink;
typedef struct QvX {
  QvPlan *P; Vdbe *v; sqlite3 *db; const QcOut *O;
  QvSink *snk;                            /* the rows written into qc_serve's (0: as QcVals into out) */
  u8 strm, strmNodes, dyn; Mem *res;     /* streamed (qv_stream): a ResultRow returns 3 with its registers in res, X->pc0 next;
                                             strmNodes: the regions' RG_RESULTS swapped in; dyn: a MEM_Dyn value read this run */
  struct QvRegion *rgRes;                 /* the region a streamed row left (resumed in it) */
  Mem *r; QvCur *c; u8 *once; void **octx;
  QvArena A;
  DbPage **pins; int npin, cpin;
  DbPage **pset; u32 cset, nset;          /* past a few pins, the pinned pages as a set (valid while nset == npin), so each is pinned once */
  QcVal *out; int nrow; size_t cout;     /* rows handed out; capacity in values (shared by every plan: widths differ) */
  int iCompare; const u32 *aPermute;
  u32 ephGen;                             /* bumped by every ephemeral write: cached ephemeral rows older than it are stale */
  u8 *fb; int *rgAt;                      /* the plan's feedback; its regions (0 until they are built) */
  const u8 *acc;                          /* the plan's isAcc */
  unsigned nprog; int tick;               /* loop iterations since the progress handler last ran; until the next check */
  u32 usable;                             /* the database's usable page size */
  int pc0, fbLeft;                        /* QV_REENTER_OPS: where QV_RUN starts again; jumps before it returns */
  int nticks;                             /* qv_tick_check calls this run (a long first run builds its regions early; streaming) */
} QvX;
/* feedback classes, by a Mem's flags & 0x1f (Null Str Int Real Blob; IntReal alone counts as real); the first one
   of Null, Int, Real, Str, Blob that is set wins */
#define QV_FB_NULL 1
#define QV_FB_INT 2
#define QV_FB_REAL 4
#define QV_FB_TEXT 8
#define QV_FB_BLOB 16
static const u8 qv_fbt[32] = {4, 1, 8, 1, 2, 1, 2, 1, 4, 1, 4, 1, 2, 1, 2, 1, 16, 1, 8, 1, 2, 1, 2, 1, 4, 1, 4, 1, 2, 1, 2, 1};
static inline u8 qv_fbc(const Mem *m) { return qv_fbt[m->flags & 0x1f]; }
/* the feedback a region's fast Column (RG_COLI) takes: numbers and NULL, at least one number (QV_NUMCOL=int: integers only) */
/* (not monotone at the bottom: {NULL} alone is no number class, {NULL, INT} is; fine while regions are built once, at
   run 3, but a rebuild from widened feedback would have to treat {NULL} -> {NULL, INT} as a narrowing) */
static int qv_fb_num(u8 m) {
  return qcf.numColInt ?   /* (QV_NUMCOL=int) */ m == QV_FB_INT : m && !(m & ~(QV_FB_NULL | QV_FB_INT | QV_FB_REAL)) && m != QV_FB_NULL;
}
#ifndef QC_CACHE_PLANS
#define QC_CACHE_PLANS 256
#endif
#ifndef QV_NEG
#define QV_NEG 64
#endif
typedef struct QvCached { sqlite3 *db; char *sql; int nsql; u32 h; u64 used; void *P; } QvCached;   /* (the plan cache's: qv_cache_put) */
typedef struct QvNeg { sqlite3 *db; char *sql; int nsql; u32 h; u64 gen; const char *why; } QvNeg;   /* (refused texts': qv_neg_put) */
typedef struct QvConn {   /* qvm.c's state for a connection (db->pQvConn: qc.c's per-connection state) */
  int nplans, nact; QvPlan *act[QC_MAXPLANS];   /* qvm's plans: counted, and the ones in this transaction */
  QvX x;                                        /* the run state (one run at a time on a connection) */
  int streamOk; QvPlan *strmP; QvSink *strmK;   /* set by qc around a run that may stream; the suspended stream */
  void *bcpool[64]; int nbc;                    /* BtCursor memory for ephemeral cursors, reused */
  QvCached cache[QC_CACHE_PLANS > 0 ? QC_CACHE_PLANS : 1]; int ncache; u64 cacheClock;   /* the plan cache */
  QvNeg neg[QV_NEG]; int nneg, negi;            /* texts no tier would lift */
  int keepRun; QvBlk *keptA;   /* a stream drained for another statement: the arena its last row may point into, for its
                                  statement (qc_stream_drain_all) */
} QvConn;
#define QV_CONN(db) ((QvConn *)QCD((db))->pQvConn)
static void qv_txn_end(Btree *bt) {
  QvConn *VC = QCD(bt->db)->pQvConn; if (!VC) return;
  for (int k = 0; k < VC->nact; ) {
    QvPlan *P = VC->act[k]; if (P->bt != bt) { k++; continue; }
    P->txnChecked = 0;
    for (int c = 0; c < P->nCur; c++) if (P->bopen[c]) { sqlite3BtreeCloseCursor(P->bc[c]); P->bopen[c] = 0; }
    QC_LIST_DEL(VC->act, VC->nact, P, act);
  }
}

/* ---------------------------------------------------------------- registers */
/* out2Prerelease: the register about to be written (an accumulator's context released, a counter reset) */
#ifdef __wasm__
static QV_INLINE Mem *qv_out(QvX *X, int r) {   /* (emcc leaves it out of line otherwise) */
#else
static inline Mem *qv_out(QvX *X, int r) {
#endif
  Mem *m = &X->r[r];
  if (X->acc[r]) {
    if (X->P->isCnt[r]) X->P->cnt[r] = 0;
    if (X->P->isSum[r]) { X->P->sacc[r].iSum = X->P->sacc[r].cnt = 0; X->P->sacc[r].spilled = 0; }
  }
  if (VdbeMemDynamic(m)) sqlite3VdbeMemSetNull(m);
  return m;
}
static inline void qv_int(Mem *m, i64 v) { m->flags = MEM_Int; m->u.i = v; }
static inline void qv_static(Mem *m, const void *z, int n, int text, int term) {   /* stable memory: pinned page, arena, P4 */
  m->flags = text ? (u16)(MEM_Str | MEM_Static | (term ? MEM_Term : 0)) : (MEM_Blob | MEM_Static);
  m->z = (char *)z; m->n = n; m->enc = SQLITE_UTF8;
}
static inline int qv_isnull(const Mem *m) { return (m->flags & MEM_Null) != 0; }
/* a register's value for the caller: stable memory as it is, anything else copied to the arena */
static int qv_val(QvX *X, const Mem *m, QcVal *o) {
  u16 f = m->flags;
  if (f & MEM_Null) { o->type = SQLITE_NULL; o->n = 0; o->i = 0; return 1; }
  if (f & MEM_Int) { o->type = SQLITE_INTEGER; o->i = m->u.i; return 1; }
  if (f & MEM_IntReal) { o->type = SQLITE_FLOAT; o->r = (double)m->u.i; return 1; }
  if (f & MEM_Real) { o->type = SQLITE_FLOAT; o->r = m->u.r; return 1; }
  if (f & (MEM_Str | MEM_Blob)) {
    if (f & MEM_Zero) { qv_why = "zeroblob"; return 0; }
    if ((f & MEM_Str) && m->enc != SQLITE_UTF8) { qv_why = "encoding"; return 0; }
    o->type = (f & MEM_Str) ? SQLITE_TEXT : SQLITE_BLOB; o->n = m->n;
    if (f & MEM_Static) { o->z = (const u8 *)m->z; return 1; }
    char *d = qv_alloc(&X->A, (size_t)m->n + 1); if (!d) { qv_why = "arena"; return 0; }
    if (m->n) memcpy(d, m->z, (size_t)m->n);
    d[m->n] = 0; o->z = (const u8 *)d; return 1;
  }
  qv_why = "value"; return 0;
}
/* a QcVal as a shallow Mem (the sorter's keys; nothing to release unless a conversion made one) */
static void qv_mem(Mem *m, const QcVal *x, sqlite3 *db) {
  m->szMalloc = 0; m->zMalloc = 0; m->eSubtype = 0; m->db = db; m->enc = SQLITE_UTF8; m->z = 0; m->n = 0; m->u.i = 0;
  switch (x->type) {
    case SQLITE_INTEGER: m->flags = MEM_Int; m->u.i = x->i; break;
    case SQLITE_FLOAT: m->flags = MEM_Real; m->u.r = x->r; break;
    case SQLITE_TEXT: m->flags = MEM_Str | MEM_Ephem; m->z = (char *)x->z; m->n = x->n; break;
    case SQLITE_BLOB: m->flags = MEM_Blob | MEM_Ephem; m->z = (char *)x->z; m->n = x->n; break;
    default: m->flags = MEM_Null;
  }
}
/* sqlite3MemCompare of two QcVals (the sorter). Collations go through the CollSeq: any fast path of qvm's own for
   NOCASE (a lower-cased memcmp, a NOCASE hash set or sort prefix) must stop at the first NUL as SQLite's does
   ('a' || char(0) || 'x' = 'a' || char(0) || 'y' COLLATE NOCASE is true) */
static QV_INLINE int qv_cmp(QvX *X, const QcVal *a, const QcVal *b, const CollSeq *coll) {
  if (a->type == SQLITE_INTEGER && b->type == SQLITE_INTEGER) return a->i < b->i ? -1 : a->i > b->i;
  if (a->type == SQLITE_TEXT && b->type == SQLITE_TEXT && (!coll || coll->xCmp == sqlite3BinaryCompare)) {
    int n = a->n < b->n ? a->n : b->n, c = memcmp(a->z, b->z, (size_t)n); return c ? c : a->n - b->n;
  }
  if (a->type == SQLITE_TEXT && b->type == SQLITE_TEXT && coll->enc == SQLITE_UTF8) return coll->xCmp(coll->pUser, a->n, a->z, b->n, b->z);   /* (vdbeCompareMemString: no Mems) */
  Mem ma, mb; qv_mem(&ma, a, X->db); qv_mem(&mb, b, X->db);
  int c = sqlite3MemCompare(&ma, &mb, coll);
  sqlite3VdbeMemRelease(&ma); sqlite3VdbeMemRelease(&mb); return c;
}

/* ---------------------------------------------------------------- records */
static inline u32 qv_phash(const DbPage *pg) { u64 x = (u64)(uintptr_t)pg >> 4; x *= 0x9e3779b97f4a7c15ull; return (u32)(x >> 32); }
static int qv_pin(QvX *X, DbPage *pg) {
  int n = X->npin; u32 h = 0;
  if (n && X->pins[n - 1] == pg) return 1;
  if (n < 8) { for (int i = 0; i < n - 1; i++) if (X->pins[i] == pg) return 1; }
  else {
    if (X->nset != (u32)n || 2 * (u32)n >= X->cset) {   /* (re)built: open addressing, at most half full */
      u32 c = X->cset ? X->cset : 64; while (c <= 4 * (u32)n) c *= 2;
      if (c != X->cset) { DbPage **ns = sqlite3_realloc64(X->pset, sizeof *ns * (u64)c); if (!ns) { qv_why = "oom"; return 0; } X->pset = ns; X->cset = c; }
      memset(X->pset, 0, sizeof *X->pset * (size_t)X->cset);
      for (int i = 0; i < n; i++) { u32 k = qv_phash(X->pins[i]) & (X->cset - 1); while (X->pset[k]) k = (k + 1) & (X->cset - 1); X->pset[k] = X->pins[i]; }
      X->nset = (u32)n;
    }
    for (h = qv_phash(pg) & (X->cset - 1); X->pset[h]; h = (h + 1) & (X->cset - 1)) if (X->pset[h] == pg) return 1;
  }
  if (X->npin == X->cpin) {
    if (X->cpin >= QV_MAXPINS) { qv_why = "pins"; return 0; }
    int nc = X->cpin ? X->cpin * 2 : 256; DbPage **np = sqlite3_realloc64(X->pins, sizeof *np * (u64)nc);
    if (!np) return 0;
    X->pins = np; X->cpin = nc;
  }
  sqlite3PagerRef(pg); X->pins[X->npin++] = pg;
  if (n >= 8) { X->pset[h] = pg; X->nset++; }
  return 1;
}
static QV_INLINE double qv_real(const u8 *d) {   /* a record's big-endian double */
#if SQLITE_BYTEORDER == 1234
  u64 u; memcpy(&u, d, 8); u = __builtin_bswap64(u);
#elif SQLITE_BYTEORDER == 4321
  u64 u; memcpy(&u, d, 8);
#else
  u64 u = 0; for (int k = 0; k < 8; k++) u = (u << 8) | d[k];
#endif
  double r; memcpy(&r, &u, 8); return r;
}
/* the value of serial type t at d into a register (the register has been released); copy: text and blobs to the arena */
static QV_INLINE int qv_serial_mem(QvX *X, u32 t, const u8 *d, Mem *m, int copy) {
  switch (t) {
    case 0: m->flags = MEM_Null; return 1;
    case 1: case 2: case 3: case 4: case 5: case 6: qv_int(m, qc_int(t, d)); return 1;
    case 7: { double r = qv_real(d); if (r != r) { m->flags = MEM_Null; return 1; } m->flags = MEM_Real; m->u.r = r; return 1; }   /* NaN reads as NULL */
    case 8: case 9: qv_int(m, (i64)t - 8); return 1;
    case 10: case 11: qv_why = "serial type"; return 0;
  }
  u32 n = (t - 12) >> 1;
  if (copy) {
    char *p = qv_alloc(&X->A, n + 1); if (!p) { qv_why = "arena"; return 0; }
    memcpy(p, d, n); p[n] = 0; qv_static(m, p, (int)n, t & 1, 1);
  } else qv_static(m, d, (int)n, t & 1, 0);
  return 1;
}
/* the same into a QcVal (sorter keys; values point into the record) */
static int qv_serial_val(u32 t, const u8 *d, QcVal *o) {
  switch (t) {
    case 0: o->type = SQLITE_NULL; o->n = 0; return 1;
    case 1: case 2: case 3: case 4: case 5: case 6: o->type = SQLITE_INTEGER; o->i = qc_int(t, d); return 1;
    case 7: { double r = qv_real(d); if (r != r) { o->type = SQLITE_NULL; o->n = 0; return 1; } o->type = SQLITE_FLOAT; o->r = r; return 1; }
    case 8: case 9: o->type = SQLITE_INTEGER; o->i = (i64)t - 8; return 1;
    case 10: case 11: return 0;
  }
  o->type = (t & 1) ? SQLITE_TEXT : SQLITE_BLOB; o->n = (int)((t - 12) >> 1); o->z = d; return 1;
}
/* MakeRecord's serial types (minWriteFileFormat >= 4: 0 and 1 as types 8 and 9) */
static QV_INLINE u32 qv_stype(const Mem *m, int ff4, u32 *len) {
  u16 f = m->flags;
  if (f & MEM_Null) { *len = 0; return 0; }
  if (f & (MEM_Int | MEM_IntReal)) {
    i64 i = m->u.i; u64 uu = i < 0 ? ~(u64)i : (u64)i;
    if (uu <= 127) { if ((i & 1) == i && ff4) { *len = 0; return 8 + (u32)uu; } *len = 1; return 1; }
    if (uu <= 32767) { *len = 2; return 2; }
    if (uu <= 8388607) { *len = 3; return 3; }
    if (uu <= 2147483647) { *len = 4; return 4; }
    if (uu <= 140737488355327LL) { *len = 6; return 5; }
    *len = 8; return (f & MEM_IntReal) ? 7 : 6;   /* an 8-byte IntReal is stored as a double */
  }
  if (f & MEM_Real) { *len = 8; return 7; }
  *len = (u32)m->n; return (u32)m->n * 2 + ((f & MEM_Str) ? 13 : 12);
}
static int qv_make_record(QvX *X, Mem *f, int n, Mem *out) {
  int ff4 = X->v->minWriteFileFormat >= 4; u32 nHdr = 0, nData = 0, st[256], ln[256];
  if (n > 256) { qv_why = "record width"; return 0; }
  for (int k = 0; k < n; k++) {
    if (f[k].flags & MEM_Zero) { qv_why = "zeroblob"; return 0; }
    st[k] = qv_stype(&f[k], ff4, &ln[k]); nHdr += st[k] < 0x80 ? 1 : (u32)sqlite3VarintLen(st[k]); nData += ln[k];
  }
  if (nHdr <= 126) nHdr += 1;   /* the header-size varint counts itself (as MakeRecord does) */
  else { int nv = sqlite3VarintLen(nHdr); nHdr += (u32)nv; if (nv < sqlite3VarintLen(nHdr)) nHdr++; }
  u8 *p = qv_alloc(&X->A, nHdr + nData + 8); if (!p) { qv_why = "arena"; return 0; }   /* (+8: an integer is stored with one 8-byte write) */
  u8 *h = p + putVarint32(p, nHdr), *d = p + nHdr;
  for (int k = 0; k < n; k++) {
    Mem *x = &f[k];
    if (st[k] < 0x80) *h++ = (u8)st[k]; else h += putVarint32(h, st[k]);
    switch (st[k]) {
      case 0: case 8: case 9: break;
      case 7: {
        u64 u; double r = (x->flags & MEM_Real) ? x->u.r : (double)x->u.i; memcpy(&u, &r, 8);
#if SQLITE_BYTEORDER == 1234
        u = __builtin_bswap64(u); memcpy(d, &u, 8);
#else
        for (int b = 7; b >= 0; b--) { d[b] = (u8)u; u >>= 8; }
#endif
        d += 8; break;
      }
      case 1: case 2: case 3: case 4: case 5: case 6: {
        u64 u = (u64)x->u.i; u32 len = ln[k];
#if SQLITE_BYTEORDER == 1234
        u = __builtin_bswap64(u << (8 * (8 - len))); memcpy(d, &u, 8);   /* (big-endian, its len low bytes first; the rest is overwritten by what follows) */
#else
        for (int b = (int)len - 1; b >= 0; b--) { d[b] = (u8)u; u >>= 8; }
#endif
        d += len; break;
      }
      default: if (ln[k]) memcpy(d, x->z, ln[k]); d += ln[k];
    }
  }
  qv_static(out, p, (int)(nHdr + nData), 0, 0); return 1;
}

/* ---------------------------------------------------------------- cursors */
static int qv_cur_alloc(QvX *X, QvCur *C, int nField) {
  C->nField = nField; (void)X;
  if (C->capField < nField + 2) {
    sqlite3_free(C->aType); sqlite3_free(C->aOff);
    C->aType = sqlite3_malloc64(sizeof(u32) * (u64)(nField + 2)); C->aOff = sqlite3_malloc64(sizeof(u32) * (u64)(nField + 2));
    if (!C->aType || !C->aOff) { C->capField = 0; qv_why = "oom"; return 0; }
    C->capField = nField + 2;
  }
  return 1;
}
static BtCursor *qv_bc_alloc(QvConn *VC) {   /* (VC->bcpool: BtCursor memory for ephemeral cursors, reused) */
  BtCursor *c = VC->nbc ? VC->bcpool[--VC->nbc] : sqlite3_malloc64(sqlite3BtreeCursorSize());
  if (c) sqlite3BtreeCursorZero(c);
  return c;
}
static void qv_bc_free(QvConn *VC, BtCursor *c) { if (VC->nbc < 64) VC->bcpool[VC->nbc++] = c; else sqlite3_free(c); }
static void qv_sort_unspill(sqlite3 *db, QvSort *S);
static void qv_cur_close(QvX *X, QvCur *C) {   /* (closing the last cursor of a BTREE_SINGLE B-tree closes the B-tree) */
  if (C->kind == QVC_EPH && C->bc) { sqlite3BtreeCloseCursor(C->bc); qv_bc_free(QV_CONN(X->db), C->bc); }
  if (C->kind == QVC_SORT && C->srt && C->srt->vc) qv_sort_unspill(C->srt->ki->db, C->srt);   /* (the KeyInfo's connection: the sorter's own may not exist) */
  qv_cur_reset(C);
}
static inline void qv_stale(QvCur *C) { C->rowOk = 0; }
static inline void qv_stale_eph(QvX *X) { X->ephGen++; }
static int qv_vec_grow(QvX *X, QvVec *V) {
  int nc = V->cap ? V->cap * 2 : 32; QvVecEnt *ne = qv_alloc(&X->A, sizeof *ne * (size_t)nc);
  if (!ne) { qv_why = "arena"; return 0; }
  if (V->n) memcpy(ne, V->e, sizeof *ne * (size_t)V->n);
  V->e = ne; V->cap = nc; return 1;
}
static int qv_vec_find(const QvVec *V, i64 rowid, int *at) {   /* binary search among [first, n); 1 found (maybe dead) */
  int lo = V->first, hi = V->n;
  while (lo < hi) { int m = (lo + hi) >> 1; if (V->e[m].rowid < rowid) lo = m + 1; else hi = m; }
  *at = lo; return lo < V->n && V->e[lo].rowid == rowid;
}
static inline int qv_vec_next(const QvVec *V, int i) { for (i++; i < V->n && V->e[i].dead; i++) {} return i; }
static inline int qv_vec_prev(const QvVec *V, int i) { for (i--; i >= V->first && V->e[i].dead; i--) {} return i; }
static void qv_vec_stale(QvX *X, const QvVec *V) {   /* (a cursor in ref may since have been reopened as something else) */
  if (V->nref <= 8) { for (int i = 0; i < V->nref; i++) if (V->ref[i]->kind == QVC_VEC && V->ref[i]->vec == V) V->ref[i]->rowOk = 0; return; }
  for (int c = 0; c < X->P->nCur; c++) if (X->c[c].kind == QVC_VEC && X->c[c].vec == V) X->c[c].rowOk = 0;
}
#define QV_VEC_DEOPT(why) do { X->P->vecOff = 1; qv_why = "vec deopt: " why; return 0; } while (0)   /* a write moved cells: no cached ephemeral row is valid (vdbe.c: colCacheCtr++) */
#define QV_TOPN_DEOPT(why) do { X->P->topnOff = 1; qv_why = "topn deopt: " why; return 0; } while (0)
/* an overflow record (out of line: rare, and qv_row stays the size it was). A table or index row whose header is in the
   cell: its fields read where they are, those past the cell from the overflow pages as OP_Column reads them
   (qv_colbig), not the whole payload copied for every row read (a 100 KB JSONB document per row: speedtest1's json
   test 140). Otherwise (an ephemeral row, a header past the cell) the whole payload, copied. */
static QV_COLD int qv_row_ovfl(QvX *X, QvCur *C, u32 amt) {
  BtCursor *bc = C->bc; u32 hl;
  if (C->kind == QVC_BT && amt >= 1 && (C->row[0] < 0x80 ? (hl = C->row[0], 1) : (sqlite3GetVarint32(C->row, &hl), 1)) && hl <= amt
      && (C->ov || (C->ov = sqlite3MallocZero(sizeof *C->ov)))) {
    C->ovfl = 1; C->rowPage = bc->pPage->pDbPage; C->ov->ovPay = C->payload; C->ov->ovN = 0; C->payload = amt;   /* (payload: the cell, so qv_hdr stops at it: qv_hdr_cut) */
    return 1;
  }
  u8 *buf = qv_alloc(&X->A, C->payload + 1); if (!buf) { qv_why = "arena"; return 0; }
  if (sqlite3BtreePayload(bc, 0, C->payload, buf) != SQLITE_OK) { qv_why = "payload"; return 0; }
  C->row = buf; C->szRow = C->payload;
  return 1;
}
/* the cursor's position is final and its row's record is at C->row (header not yet parsed): 0 = fallback,
   with C->nullRow set when the row turned out to be gone */
static int qv_row(QvX *X, QvCur *C) {
  if (C->kind == QVC_PSEUDO) {   /* the register's record, whenever it changes */
    Mem *r = &X->r[C->pseudoReg];
    if (!(r->flags & MEM_Blob)) { qv_why = "pseudo"; return 0; }
    if (C->rowOk && C->row == (const u8 *)r->z && C->szRow == (u32)r->n) return 1;
    C->row = (const u8 *)r->z; C->payload = C->szRow = (u32)r->n;
  } else if (C->kind == QVC_VEC) {
    QvVec *V = C->vec;
    if (C->vbad == 2) { X->P->vecOff = 1; qv_why = "vec deopt: row after a missed seek"; return 0; }
    if (C->vbad || C->vi < V->first || C->vi >= V->n || V->e[C->vi].dead) { C->nullRow = 1; return 1; }   /* deleted under us: vdbe.c's moved cursor, a NULL row */
    if (C->rowOk && C->rowVi == C->vi) return 1;
    C->row = V->e[C->vi].rec; C->payload = C->szRow = V->e[C->vi].len; C->rowVi = C->vi; C->rowPage = 0;
  } else if (C->kind == QVC_TOPN) {
    QvTopn *T = C->tn;
    if (C->vbad == 2) QV_TOPN_DEOPT("row from an unknown position");
    if (C->vbad || C->vi < 0 || C->vi >= T->n) { C->nullRow = 1; return 1; }
    if (C->rowOk && C->rowVi == C->vi) return 1;
    C->row = T->rec[C->vi]; C->payload = C->szRow = T->len[C->vi]; C->rowVi = C->vi; C->rowPage = 0;
  } else if (C->rowOk && (C->kind != QVC_EPH || C->rowGen == X->ephGen)) return 1;
  else if (C->kind == QVC_SORT) {
    QvSort *S = C->srt; if (S->vc || S->pos >= S->n) { qv_why = "sorter row"; return 0; }
    C->row = S->rec[S->pos]; C->payload = C->szRow = S->len[S->pos];
  } else {
    BtCursor *bc = C->bc;
    if (C->deferred) {   /* sqlite3VdbeFinishMoveto */
      int res; if (sqlite3BtreeTableMoveto(bc, C->movetoTarget, 0, &res) != SQLITE_OK || res) { qv_why = "deferred seek"; return 0; }
      C->deferred = 0;
    } else if (sqlite3BtreeCursorHasMoved(bc)) {
      int diff = 0; if (sqlite3BtreeCursorRestore(bc, &diff) != SQLITE_OK) { qv_why = "restore"; return 0; }
      if (diff) { C->nullRow = 1; return 1; }
    }
    u32 amt; C->payload = sqlite3BtreePayloadSize(bc); C->row = sqlite3BtreePayloadFetch(bc, &amt); C->szRow = amt;
    C->rowPage = 0; C->ovfl = 0;
    if (__builtin_expect(amt < C->payload, 0)) { if (!qv_row_ovfl(X, C, amt)) return 0; }
    else if (C->kind == QVC_BT) C->rowPage = bc->pPage->pDbPage;   /* pinned when a text or blob value points into it */
  }
  const u8 *z = C->row;
  if (C->szRow < 1) { qv_why = "empty record"; return 0; }
  u32 hdr; C->hdrOff = z[0] < 0x80 ? (hdr = z[0], 1) : sqlite3GetVarint32(z, &hdr);
  if (hdr > C->szRow || hdr > 98307) { qv_why = "corrupt record"; return 0; }
  C->aOff[0] = hdr; C->nHdr = 0; C->rowOk = 1; C->rowGen = X->ephGen;
  return 1;
}
/* qv_row's answer without calling it, when the cached row is still the current one (nothing to read) */
static QV_INLINE int qv_row_ok(QvX *X, const QvCur *C) {
  if (!C->rowOk) return 0;
  if (C->kind == QVC_PSEUDO) { const Mem *r = &X->r[C->pseudoReg]; return (r->flags & MEM_Blob) && C->row == (const u8 *)r->z && C->szRow == (u32)r->n; }
  if (C->kind == QVC_VEC) { const QvVec *V = C->vec; return !C->vbad && C->rowVi == C->vi && C->vi >= V->first && C->vi < V->n && !V->e[C->vi].dead; }
  if (C->kind == QVC_TOPN) return !C->vbad && C->rowVi == C->vi && C->vi < C->tn->n;
  return C->kind != QVC_EPH || C->rowGen == X->ephGen;
}
/* a database value of serial type t at d into register m: a non-empty text or blob stays in its page, pinned until
   the execution ends (a MEM_Static value, no copy), for the first QV_PIN_CHEAP pages a run pins. Past that it is
   copied into the register's own buffer, as vdbe.c's OP_Column does: a scan of more pages than the page cache holds
   would otherwise hold them all, and the cache would read them again on every run (Gitea's top-N over 79k rows:
   1.8x the interpreter's time, mostly pread). Such a value is not MEM_Static, so a ResultRow or an insert copies it */
#ifndef QV_PIN_CHEAP
#define QV_PIN_CHEAP 128
#endif
/* a field past an overflow record's cell, as vdbe.c's OP_Column reads it. A large text or blob of a table row is kept
   (vdbeColumnFromOverflow's cache, an RCStr shared by reference) so the same field read again for the same row, in
   the same run, isn't read from the overflow pages again (speedtest1 140: four ->> on one JSONB document per row) */
static QV_NOINLINE int qv_colbig(QvX *X, QvCur *C, u32 t, u32 off, u32 len, Mem *m) {
  if ((u64)off + len > C->ov->ovPay) { qv_why = "corrupt record"; return 0; }
  if ((int)len > X->db->aLimit[SQLITE_LIMIT_LENGTH]) { qv_why = "too big"; return 0; }
  m->enc = SQLITE_UTF8;
  if (len > 4000 && t >= 12 && !C->ki) {
    i64 cell = sqlite3BtreeOffset(C->bc); QvOvf *O = C->ov;
    if (!O->big || O->bigRun != X->P->runs || O->bigOff != off || O->bigCell != cell) {
      if (O->big) { sqlite3RCStrUnref(O->big); O->big = 0; }
      char *b = sqlite3RCStrNew(len + 3); if (!b) { qv_why = "oom"; return 0; }
      if (sqlite3BtreePayload(C->bc, off, len, b) != SQLITE_OK) { sqlite3RCStrUnref(b); qv_why = "overflow read"; return 0; }
      b[len] = b[len + 1] = b[len + 2] = 0;
      O->big = b; O->bigRun = X->P->runs; O->bigOff = off; O->bigCell = cell;
    }
    sqlite3RCStrRef(O->big); X->dyn = 1;   /* (a MEM_Dyn register: released at the run's end) */
    if (sqlite3VdbeMemSetStr(m, O->big, len, (t & 1) ? SQLITE_UTF8 : 0, sqlite3RCStrUnref) != SQLITE_OK) { qv_why = "oom"; return 0; }
    if (t & 1) m->flags |= MEM_Term;
    return 1;
  }
  if (sqlite3VdbeMemFromBtree(C->bc, off, len, m) != SQLITE_OK) { qv_why = "overflow read"; return 0; }
  sqlite3VdbeSerialGet((const u8 *)m->z, t, m); m->flags &= ~MEM_Ephem;
  return 1;
}
static QV_INLINE int qv_colval(QvX *X, QvCur *C, u32 t, const u8 *d, Mem *m) {
  if (t >= 14 && C->rowPage && C->pinned != C->rowPage && !C->copyVals) {
    if (X->npin >= QV_PIN_CHEAP || (C->rowPage->flags & PGHDR_MMAP)) {   /* (a memory-mapped page has no cache entry to hold a reference on) */
      u32 n = (t - 12) >> 1;
      if (sqlite3VdbeMemClearAndResize(m, (int)n + 2)) { qv_why = "oom"; return 0; }
      memcpy(m->z, d, n); m->n = (int)n; m->enc = SQLITE_UTF8;
      if (t & 1) { m->z[n] = 0; m->z[n + 1] = 0; m->flags = MEM_Str | MEM_Term; } else m->flags = MEM_Blob;
      return 1;
    }
    if (!qv_pin(X, C->rowPage)) return 0;
    C->pinned = C->rowPage;
  }
  return qv_serial_mem(X, t, d, m, C->copyVals);
}
/* the current row's header parsed through field p2, or to its end (fewer fields); 0 = corrupt */
/* an overflow row's header parse, reached through qv_hdr's corruption test (its payload is the cell's, so a field past
   the cell fails it): checked against the whole record, then nHdr counts only the leading fields inside the cell, so
   no in-place path (qv_colval, the region nodes) reads past it; qv_column reads those with qv_colbig. The header
   position goes back to the first field past the cell (every later one is past it too), so parsing resumes there. */
static QV_COLD int qv_hdr_cut(QvCur *C, u32 i, const u8 *h, u64 off) {
  const u8 *z = C->row, *zEnd = z + C->aOff[0];
  if ((h >= zEnd && (h > zEnd || off != C->ov->ovPay)) || off > C->ov->ovPay) { qv_why = "corrupt record"; return 0; }
  C->ov->ovN = (int)i;
  u32 k = (u32)C->nHdr; while (k < i && C->aOff[k + 1] <= C->szRow) k++;
  const u8 *p = z + C->hdrOff; for (u32 j = (u32)C->nHdr; j < k; j++) { u32 t; if ((t = p[0]) < 0x80) p++; else p += sqlite3GetVarint32(p, &t); }
  C->nHdr = (int)k; C->hdrOff = (u32)(p - z);
  return 1;
}
/* qv_hdr's fields not ending where the record does: an overflow row's cell (qv_hdr_cut), an empty record (aOff[0] 0:
   no fields), or corruption */
static QV_COLD int qv_hdr_odd(QvCur *C, u32 i, const u8 *h, u64 off) {
  if (C->ovfl) return qv_hdr_cut(C, i, h, off);
  if (C->aOff[0] != 0) { qv_why = "corrupt record"; return 0; }
  (void)i; (void)h; C->nHdr = 0; C->hdrOff = C->aOff[0];
  return 1;
}
static QV_INLINE int qv_hdr(QvCur *C, int p2) {
  const u8 *z = C->row, *zEnd = z + C->aOff[0];
  u32 i = (u32)C->nHdr; u64 off = C->aOff[i]; const u8 *h = z + C->hdrOff;
  u32 *at = C->aType, *ao = C->aOff;   /* (locals: their stores may alias C's fields) */
  while (i <= (u32)p2 && h < zEnd) {   /* (a one-byte type's length from the table, as OP_Column: the call cost a wide row's header
                                          about 35 % of a run on x86, jellyfin browse.series_unplayed 1.16 of stock) */
    u32 t;
    if ((t = h[0]) < 0x80) { h++; off += sqlite3SmallTypeSizes[t]; }
    else { h += sqlite3GetVarint32(h, &t); off += sqlite3VdbeSerialTypeLen(t); }
    at[i] = t; ao[++i] = (u32)off;
  }
  if (__builtin_expect((h >= zEnd && (h > zEnd || off != C->payload)) || off > C->payload, 0)) return qv_hdr_odd(C, i, h, off);
  C->nHdr = (int)i; C->hdrOff = (u32)(h - z);
  return 1;
}
/* a field past what qv_hdr parsed: past an overflow row's cell (qv_colbig), or past a record with fewer fields than the
   table now has: the column's default (ALTER TABLE ADD COLUMN) */
static QV_COLD int qv_column_past(QvX *X, QvCur *C, int p2, const Op *op, Mem *m) {
  if (C->ovfl && p2 < C->ov->ovN) { u32 t = C->aType[p2]; return qv_colbig(X, C, t, C->aOff[p2], sqlite3VdbeSerialTypeLen(t), m); }
  if (op->p4type == P4_MEM) sqlite3VdbeMemShallowCopy(m, op->p4.pMem, MEM_Static);
  else m->flags = MEM_Null;
  return 1;
}
/* OP_Column: field p2 of the cursor's current row into register m (released); 0 = fallback */
static QV_INLINE int qv_column(QvX *X, QvCur *C, int p2, const Op *op, Mem *m) {
  for (;;) {
    if (C->nullRow && C->kind != QVC_PSEUDO) { m->flags = MEM_Null; return 1; }
    if (C->deferred && C->altMap && C->altMap[1 + p2] > 0) { p2 = C->altMap[1 + p2] - 1; C = C->alt; continue; }
    break;
  }
  if (!qv_row_ok(X, C)) {
    if (!qv_row(X, C)) return 0;
    if (C->nullRow && C->kind != QVC_PSEUDO) { m->flags = MEM_Null; return 1; }
  }
  if (p2 >= C->nField) { qv_why = "column index"; return 0; }
  if (C->nHdr <= p2) {
    if (!qv_hdr(C, p2)) return 0;
    if (__builtin_expect(C->nHdr <= p2, 0)) return qv_column_past(X, C, p2, op, m);
  }
  return qv_colval(X, C, C->aType[p2], C->row + C->aOff[p2], m);
}

/* ---------------------------------------------------------------- sorter: stable, as vdbeSorterSort */
typedef struct QvHk { u8 c; i64 i; double r; const u8 *z; int n; } QvHk;   /* c: 0 null, 1 a number with an integer value, 2 another real, 3 text, 4 blob */
static inline void qv_hk_num(QvHk *k, double r) {
  if (r >= -9223372036854775808.0 && r < 9223372036854775808.0 && (double)(i64)r == r) { k->c = 1; k->i = (i64)r; } else { k->c = 2; k->r = r; }
}
static inline u64 qv_hmix(u64 h, u64 v) { h = (h ^ v) * 0x9e3779b97f4a7c15ull; return h ^ (h >> 29); }
static int nocaseCollatingFunc(void *, int, const void *, int, const void *);
static int rtrimCollFunc(void *, int, const void *, int, const void *);
static int qv_decode(const u8 *z, u32 n, int nk, QcVal *o) {   /* the first nk fields of a record (values point into it) */
  u32 hdr, off, t; const u8 *h = z, *he;
  h += z[0] < 0x80 ? (hdr = z[0], 1) : sqlite3GetVarint32(z, &hdr);
  if (hdr > n) return 0;
  he = z + hdr; off = hdr;
  for (int k = 0; k < nk; k++) {
    if (h >= he) { for (; k < nk; k++) { o[k].type = SQLITE_NULL; o[k].n = 0; } return 1; }
    if ((t = *h) < 0x80) h++; else h += sqlite3GetVarint32(h, &t);
    u32 len = sqlite3VdbeSerialTypeLen(t);
    if (off + len > n || !qv_serial_val(t, z + off, &o[k])) return 0;
    off += len;
  }
  return 1;
}
/* vdbeRecordCompare on decoded keys: per field the value order under its collation, DESC and BIGNULL as the
   KeyInfo says; equal keys compare equal (the sort keeps them in insertion order) */
static QV_INLINE int qv_keycmp(QvX *X, const KeyInfo *ki, const QcVal *a, const QcVal *b, int nk) {
  for (int i = 0; i < nk; i++) {
    int c = qv_cmp(X, &a[i], &b[i], ki->aColl[i]);
    if (!c) continue;
    int f = ki->aSortFlags[i];
    if (f && (!(f & KEYINFO_ORDER_BIGNULL) || ((f & KEYINFO_ORDER_DESC) != 0) != (a[i].type == SQLITE_NULL || b[i].type == SQLITE_NULL))) c = -c;
    return c;
  }
  return 0;
}
/* the same, called: the hashing's probes and groups. qv_sort's merge is then its only caller in line (a second one in line
   and the compiler called it from the merge too: joplin folders.all_for_display +1.1 %) */
static QV_NOINLINE int qv_keycmp_out(QvX *X, const KeyInfo *ki, const QcVal *a, const QcVal *b, int nk) { return qv_keycmp(X, ki, a, b, nk); }
/* a one-field key that is an integer in every record, or a real in every record: a stable LSD radix sort of the indices,
   a pass per byte of the keys' range (max - min), none when they are all equal. The merge's order: ascending (DESC: the
   key inverted), equal keys in insertion order. A real as its bits made unsigned-ordered (negatives inverted, -0.0 as
   0.0: they compare equal; NaN reads as NULL). 0: not that case. freshrss stats.entry_count_per_day's GROUP BY day: 30
   days over its rows, one pass, where the merge's qv_keycmp cost more than vdbeSorterSort and vdbeSorterCompareInt;
   calibre annotations.all_for_user's ORDER BY a REAL timestamp, where every compare went through sqlite3MemCompare */
static QV_NOINLINE int qv_sort_int(QvX *X, QvSort *S, int **pix, int **ptx) {   /* (out of line: inlined, the merge loop below cost 1 % more) */
  int n = S->n, t; if (S->nk != 1 || ((t = S->key[0].type) != SQLITE_INTEGER && t != SQLITE_FLOAT)) return 0;   /* (before the arena is asked: a text key's sort pays nothing) */
  const QcVal *key = S->key; int desc = (S->ki->aSortFlags[0] & KEYINFO_ORDER_DESC) != 0;
  u64 *ka = qv_alloc(&X->A, sizeof *ka * (size_t)n), *kb = qv_alloc(&X->A, sizeof *kb * (size_t)n), mn = ~(u64)0, mx = 0;
  if (!ka || !kb) return 0;
  if (t == SQLITE_INTEGER) for (int i = 0; i < n; i++) {
    if (key[i].type != SQLITE_INTEGER) return 0;
    u64 u = (u64)key[i].i ^ ((u64)1 << 63); if (desc) u = ~u;   /* (signed order as unsigned) */
    ka[i] = u; if (u < mn) mn = u; if (u > mx) mx = u;
  } else for (int i = 0; i < n; i++) {   /* (a loop of its own: in the integer one, the type test cost freshrss 0.5 %) */
    if (key[i].type != SQLITE_FLOAT) return 0;
    double r = key[i].r == 0 ? 0.0 : key[i].r; u64 u; memcpy(&u, &r, 8);
    u = (u >> 63) ? ~u : u | ((u64)1 << 63); if (desc) u = ~u;
    ka[i] = u; if (u < mn) mn = u; if (u > mx) mx = u;
  }
  int *ix = *pix, *tx = *ptx, nb = 0;
  for (u64 r = mx - mn; r; r >>= 8) nb++;
  for (int i = 0; nb && i < n; i++) ka[i] -= mn;
  for (int b = 0; b < nb; b++) {
    u32 cnt[256]; memset(cnt, 0, sizeof cnt); int sh = 8 * b;
    for (int i = 0; i < n; i++) cnt[(ka[i] >> sh) & 255]++;
    for (u32 d = 0, p = 0; d < 256; d++) { u32 c = cnt[d]; cnt[d] = p; p += c; }
    for (int i = 0; i < n; i++) { u32 p = cnt[(ka[i] >> sh) & 255]++; kb[p] = ka[i]; tx[p] = ix[i]; }
    u64 *t = ka; ka = kb; kb = t; int *q = ix; ix = tx; tx = q;
  }
  *pix = ix; *ptx = tx; return 1;
}
/* bottom-up merge sort of n record indices by their keys; on ties the earlier insertion first. The sorted indices are
   in *pix on return (*ptx the scratch). (qv_sort_hash's groups; qv_sort has the same merge in line: called, it cost
   joplin folders.all_for_display 1.3 %) */
static int qv_msort_pk(QvX *X, const KeyInfo *ki, const QcVal *key, int nk, int **pix, int **ptx, int n);
static QV_NOINLINE void qv_msort(QvX *X, const KeyInfo *ki, const QcVal *key, int nk, int **pix, int **ptx, int n) {
  if (qv_msort_pk(X, ki, key, nk, pix, ptx, n)) return;
  int *ix = *pix, *tx = *ptx;
  for (int w = 1; w < n; w *= 2) {
    for (int lo = 0; lo < n; lo += 2 * w) {
      int mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n, i = lo, j = mid, k = lo;
      while (i < mid && j < hi) tx[k++] = qv_keycmp_out(X, ki, &key[(size_t)ix[i] * nk], &key[(size_t)ix[j] * nk], nk) <= 0 ? ix[i++] : ix[j++];
      while (i < mid) tx[k++] = ix[i++];
      while (j < hi) tx[k++] = ix[j++];
    }
    int *t = ix; ix = tx; tx = t;
  }
  *pix = ix; *ptx = tx;
}
/* the merge with abbreviated keys (PostgreSQL's): when the first key field is text in every record, under BINARY, NOCASE
   or RTRIM, its first 8 bytes as a big-endian u64 (NOCASE: lower-cased, zero from the first NUL on; RTRIM: without
   trailing spaces; zero-padded; inverted for DESC), kept beside the indices. Different prefixes order the records as
   qv_keycmp would; equal ones go to it. 0: not that case, or every prefix the same */
#ifndef QV_SORT_PK_MIN
#define QV_SORT_PK_MIN 64   /* records: fewer are merged plainly */
#endif
static QV_NOINLINE int qv_msort_pk(QvX *X, const KeyInfo *ki, const QcVal *key, int nk, int **pix, int **ptx, int n) {
  if (n < QV_SORT_PK_MIN) return 0;
  const CollSeq *c = ki->aColl[0]; int cl;
  if (!c || c->xCmp == sqlite3BinaryCompare) cl = 1; else if (c->xCmp == nocaseCollatingFunc) cl = 2; else if (c->xCmp == rtrimCollFunc) cl = 3; else return 0;
  int *ix = *pix, *tx = *ptx; u64 inv = (ki->aSortFlags[0] & KEYINFO_ORDER_DESC) ? ~(u64)0 : 0, mn = ~(u64)0, mx = 0;
  for (int i = 0; i < n; i++) if (key[(size_t)ix[i] * nk].type != SQLITE_TEXT) return 0;
  u64 *pk = qv_alloc(&X->A, sizeof *pk * (size_t)n), *tp = qv_alloc(&X->A, sizeof *tp * (size_t)n);
  if (!pk || !tp) return 0;
  for (int i = 0; i < n; i++) {
    const QcVal *k = &key[(size_t)ix[i] * nk]; const u8 *z = k->z; int m = k->n; u8 b[8] = {0};
    if (cl == 3) while (m && z[m - 1] == ' ') m--;
    if (m > 8) m = 8;
    if (cl == 2) { for (int j = 0; j < m && z[j]; j++) b[j] = sqlite3UpperToLower[z[j]]; }
    else memcpy(b, z, (size_t)m);
    u64 u; memcpy(&u, b, 8);
#if SQLITE_BYTEORDER == 1234
    u = __builtin_bswap64(u);
#elif SQLITE_BYTEORDER != 4321
    u = 0; for (int j = 0; j < 8; j++) u = (u << 8) | b[j];
#endif
    u ^= inv; pk[i] = u; if (u < mn) mn = u; if (u > mx) mx = u;
  }
  if (mn == mx) return 0;   /* (one shared prefix: nothing to decide by it) */
  for (int w = 1; w < n; w *= 2) {
    for (int lo = 0; lo < n; lo += 2 * w) {
      int mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n, i = lo, j = mid, k = lo;
      while (i < mid && j < hi) {
        int le = pk[i] != pk[j] ? pk[i] < pk[j] : qv_keycmp_out(X, ki, &key[(size_t)ix[i] * nk], &key[(size_t)ix[j] * nk], nk) <= 0;
        if (le) { tp[k] = pk[i]; tx[k++] = ix[i++]; } else { tp[k] = pk[j]; tx[k++] = ix[j++]; }
      }
      while (i < mid) { tp[k] = pk[i]; tx[k++] = ix[i++]; }
      while (j < hi) { tp[k] = pk[j]; tx[k++] = ix[j++]; }
    }
    int *t = ix; ix = tx; tx = t; u64 *q = pk; pk = tp; tp = q;
  }
  *pix = ix; *ptx = tx; return 1;
}
/* a key's hash under the KeyInfo's collations, equal for keys qv_keycmp finds equal: numbers by value (an integral real
   as its integer), text under BINARY by its bytes, NOCASE by its length and its lower-cased bytes up to the first NUL
   (sqlite3StrNICmp stops there), RTRIM by its bytes less trailing spaces, blobs by their bytes */
static QV_INLINE u64 qv_skhash(const QcVal *k, int nk, const u8 *coll) {
  u64 h = 0x243f6a8885a308d3ull;
  for (int i = 0; i < nk; i++, k++) {
    switch (k->type) {
      case SQLITE_INTEGER: h = qv_hmix(h, 1); h = qv_hmix(h, (u64)k->i); break;
      case SQLITE_FLOAT: { QvHk q; qv_hk_num(&q, k->r); h = qv_hmix(h, q.c); if (q.c == 1) h = qv_hmix(h, (u64)q.i); else { u64 b; memcpy(&b, &q.r, 8); h = qv_hmix(h, b); } break; }
      case SQLITE_TEXT: case SQLITE_BLOB: {
        const u8 *z = k->z; int n = k->n, j = 0; h = qv_hmix(h, (u64)k->type);
        if (k->type == SQLITE_TEXT && coll[i] == 2) {
          h = qv_hmix(h, (u64)n);
          for (; j < n && z[j]; j++) h = qv_hmix(h, sqlite3UpperToLower[z[j]]);
          break;
        }
        if (k->type == SQLITE_TEXT && coll[i] == 3) while (n && z[n - 1] == ' ') n--;
        for (; j + 8 <= n; j += 8) { u64 w; memcpy(&w, z + j, 8); h = qv_hmix(h, w); }
        u64 w = 0; memcpy(&w, z + j, (size_t)(n - j)); h = qv_hmix(h, w ^ ((u64)n << 56));
        break;
      }
      default: h = qv_hmix(h, 0);
    }
  }
  return h;
}
/* the stable sort by hashing: records with equal keys (qv_keycmp 0, found through a hash table of the distinct keys)
   are grouped in insertion order, only the groups' first records are sorted, and each group follows in that order. The
   same order as the merge sort, by n hashes and g log g compares for g distinct keys instead of n log n: GROUP BY over
   few groups (joplin folders.order_by_last_modified: 20,000 rows, 300 groups), ORDER BY a key of few values. Collations
   BINARY, NOCASE and RTRIM only. Given up (0) when the first 1,024 records hold more than 512 keys, where the table
   costs more than it saves; then, and after a run with fewer than 4 records a key, the sorter's next 15 runs merge
   without trying (jellyfin browse.favorites: a GROUP BY of a few hundred distinct keys, +0.6 % hashed; joplin
   folders.all_for_display: a NOCASE key, whose probes compare through sqlite3MemCompare, +1.3 %) */
#ifndef QV_SORT_HASH_MIN
#define QV_SORT_HASH_MIN 64   /* records: fewer are merged */
#endif
static QV_NOINLINE int qv_sort_hash(QvX *X, QvSort *S, int **pix, int **ptx) {
  int n = S->n, nk = S->nk; const KeyInfo *ki = S->ki; const QcVal *key = S->key;
  if (n < QV_SORT_HASH_MIN || nk > QV_MAXARGS) return 0;
  u8 *skip = &X->P->srtSkip[S->cur]; if (*skip) { (*skip)--; return 0; }
  u8 coll[QV_MAXARGS];
  for (int i = 0; i < nk; i++) {
    const CollSeq *c = ki->aColl[i];
    if (!c || c->xCmp == sqlite3BinaryCompare) coll[i] = 1;
    else if (c->xCmp == nocaseCollatingFunc) coll[i] = 2;
    else if (c->xCmp == rtrimCollFunc) coll[i] = 3;
    else return 0;
  }
  u32 cap = 1024, g = 0, gcap = 512;
  int *tab = qv_alloc(&X->A, sizeof *tab * cap), *gid = qv_alloc(&X->A, sizeof *gid * (size_t)n);
  int *first = qv_alloc(&X->A, sizeof *first * gcap), *cnt = qv_alloc(&X->A, sizeof *cnt * gcap); u64 *gh = qv_alloc(&X->A, sizeof *gh * gcap);
  if (!tab || !gid || !first || !cnt || !gh) return 0;
  memset(tab, 0, sizeof *tab * cap);
  for (int i = 0; i < n; i++) {
    const QcVal *k = &key[(size_t)i * nk]; u64 h = qv_skhash(k, nk, coll); u32 s = (u32)h & (cap - 1);
    for (;; s = (s + 1) & (cap - 1)) {
      int e = tab[s];
      if (!e) break;
      if (gh[e - 1] == h && !qv_keycmp_out(X, ki, &key[(size_t)first[e - 1] * nk], k, nk)) break;
    }
    if (tab[s]) { int e = tab[s] - 1; gid[i] = e; cnt[e]++; continue; }
    if (g == gcap) {
      if (i < 1024) { *skip = 15; return 0; }   /* (more than half the first 1,024 records are distinct) */
      u32 nc = gcap * 2; int *nf = qv_alloc(&X->A, sizeof *nf * nc), *nn = qv_alloc(&X->A, sizeof *nn * nc); u64 *nh = qv_alloc(&X->A, sizeof *nh * nc);
      if (!nf || !nn || !nh) return 0;
      memcpy(nf, first, sizeof *nf * g); memcpy(nn, cnt, sizeof *nn * g); memcpy(nh, gh, sizeof *nh * g);
      first = nf; cnt = nn; gh = nh; gcap = nc;
    }
    first[g] = i; cnt[g] = 1; gh[g] = h; gid[i] = (int)g; g++;
    if (2 * g > cap) {   /* (rehashed from the groups' hashes: at most half full) */
      u32 nc = cap * 2; int *nt = qv_alloc(&X->A, sizeof *nt * nc); if (!nt) return 0;
      memset(nt, 0, sizeof *nt * nc);
      for (u32 e = 0; e < g; e++) { u32 t = (u32)gh[e] & (nc - 1); while (nt[t]) t = (t + 1) & (nc - 1); nt[t] = (int)e + 1; }
      tab = nt; cap = nc;
    } else tab[s] = (int)g;
  }
  if (4 * g > (u32)n) *skip = 15;
  int *ix = *pix, *tx = *ptx;   /* the groups' first records, sorted (ix and tx have n entries, g <= n) */
  for (u32 e = 0; e < g; e++) ix[e] = first[e];
  qv_msort(X, ki, key, nk, &ix, &tx, (int)g);
  for (u32 e = 0, p = 0; e < g; e++) { int d = gid[ix[e]]; u32 c = (u32)cnt[d]; cnt[d] = (int)p; p += c; }   /* cnt: each group's start */
  for (int i = 0; i < n; i++) tx[cnt[gid[i]]++] = i;
  *pix = tx; *ptx = ix; return 1;
}
static int qv_sort(QvX *X, QvSort *S) {
  int n = S->n, nk = S->nk; if (n < 2) return 1;
  int *ix = qv_alloc(&X->A, sizeof *ix * (size_t)n), *tx = qv_alloc(&X->A, sizeof *tx * (size_t)n);
  const u8 **nr = qv_alloc(&X->A, sizeof *nr * (size_t)n); u32 *nl = qv_alloc(&X->A, sizeof *nl * (size_t)n);
  if (!ix || !tx || !nr || !nl) { qv_why = "arena"; return 0; }
  for (int i = 0; i < n; i++) ix[i] = i;
  const KeyInfo *ki = S->ki; const QcVal *key = S->key;   /* (locals: qv_keycmp's call would reload them each compare) */
  if (!qv_sort_int(X, S, &ix, &tx) && !qv_sort_hash(X, S, &ix, &tx) && !qv_msort_pk(X, S->ki, S->key, nk, &ix, &tx, n)) for (int w = 1; w < n; w *= 2) {   /* bottom-up merge sort of indices; on ties the earlier insertion first */
    for (int lo = 0; lo < n; lo += 2 * w) {
      int mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n, i = lo, j = mid, k = lo;
      while (i < mid && j < hi) tx[k++] = qv_keycmp(X, ki, &key[(size_t)ix[i] * nk], &key[(size_t)ix[j] * nk], nk) <= 0 ? ix[i++] : ix[j++];
      while (i < mid) tx[k++] = ix[i++];
      while (j < hi) tx[k++] = ix[j++];
    }
    int *t = ix; ix = tx; tx = t;
  }
  for (int i = 0; i < n; i++) { nr[i] = S->rec[ix[i]]; nl[i] = S->len[ix[i]]; }
  memcpy(S->rec, nr, sizeof *nr * (size_t)n); memcpy(S->len, nl, sizeof *nl * (size_t)n);   /* in place: the arrays keep their capacity */
  return 1;   /* (the keys are not needed after the sort; a ResetSorter starts them again) */
}

/* ---------------------------------------------------------------- functions and aggregates */
static int qv_policy(const FuncDef *f) {   /* built-ins (not unsafe or direct-only), or deterministic user functions */
  if (f->funcFlags & (SQLITE_FUNC_UNSAFE | SQLITE_FUNC_DIRECT | SQLITE_FUNC_INTERNAL)) return 0;
  return (f->funcFlags & SQLITE_FUNC_BUILTIN) || (f->funcFlags & SQLITE_FUNC_CONSTANT);
}
/* a function's context, built once per opcode (vdbe.c keeps one in P4); the arguments are the registers */
typedef struct QvLike { int n, m; u8 ok, pre, any, esc; u8 p[66], w[64]; } QvLike;   /* qv_like: the last pattern seen (n bytes, p) and escape, analysed; pre: 'w%'; any: w has a '_' (0) */
typedef struct QvCtx { Mem res; u8 plain; QvLike lk; union { sqlite3_context c; char b[sizeof(sqlite3_context) + QV_MAXARGS * sizeof(sqlite3_value *)]; } u; } QvCtx;   /* plain: OP_Function's function is called as it is (qv_function) */
static QvCtx *qv_ctx(QvX *X, int iOp, FuncDef *f, int argc, int p2) {
  QvCtx *k = X->octx[iOp];
  if (!k) {
    if (!(k = sqlite3_malloc64(sizeof *k))) { qv_why = "oom"; return 0; }
    memset(&k->u.c, 0, sizeof k->u.c); sqlite3VdbeMemInit(&k->res, X->db, MEM_Null); k->plain = 0; k->lk.n = -1; k->lk.ok = 0;
    k->u.c.pFunc = f; k->u.c.pVdbe = X->v; k->u.c.iOp = iOp; k->u.c.enc = SQLITE_UTF8; k->u.c.argc = (u16)argc;
    for (int i = 0; i < argc; i++) k->u.c.argv[i] = &X->r[p2 + i];
    QvPlan *P = X->P;
    int *t = sqlite3_realloc64(P->ctxPc, sizeof *t * (u64)(P->nctx + 1)); if (!t) { sqlite3VdbeMemRelease(&k->res); sqlite3_free(k); qv_why = "oom"; return 0; }
    P->ctxPc = t; P->ctxPc[P->nctx++] = iOp;
    X->octx[iOp] = k;
  }
  k->u.c.isError = 0; k->u.c.skipFlag = 0;
  return k;
}
/* length() of text or a blob in line (lengthFunc: characters up to the first NUL, a lead byte >= 0xc0 swallows the
   continuation bytes after it); numbers go to lengthFunc */
static int qv_length(const Mem *x, Mem *o) {
  u16 f = x->flags;
  if (f & MEM_Null) { o->flags = MEM_Null; return 1; }
  if ((f & (MEM_Int | MEM_Real | MEM_IntReal)) || (f & MEM_Zero)) return 0;
  if (f & MEM_Blob) { qv_int(o, x->n); return 1; }
  if (!(f & MEM_Str) || x->enc != SQLITE_UTF8) return 0;
  qv_int(o, qc_utf8_chars((const u8 *)x->z, x->n)); return 1;
}
/* scalar min()/max() and abs(), and with math functions ceil()/floor() and the one-argument math functions (math1Func),
   in line when every argument is a plain integer, a plain real or NULL: as minmaxFunc (sqlite3MemCompare, the best
   argument copied, NULL if any is NULL), absFunc, ceilingFunc and math1Func (sqlite3_result_double: NaN is NULL);
   0 = call the function */
#define QV_NUM(fl) ((fl) == MEM_Int || (fl) == MEM_Real)
/* sqlite3AddInt64 / SubInt64 / MulInt64 (1 = overflow), for the in-line paths: the compiler's checks where it has them
   (the amalgamation uses them only for gcc, so under clang MulInt64 divides); on overflow *a is not used */
#if defined(__GNUC__)
#define QV_ADD64(a, b) __builtin_add_overflow(*(a), (b), (a))
#define QV_SUB64(a, b) __builtin_sub_overflow(*(a), (b), (a))
#define QV_MUL64(a, b) __builtin_mul_overflow(*(a), (b), (a))
#else
#define QV_ADD64(a, b) sqlite3AddInt64((a), (b))
#define QV_SUB64(a, b) sqlite3SubInt64((a), (b))
#define QV_MUL64(a, b) sqlite3MulInt64((a), (b))
#endif
static QV_INLINE int qv_numcmp(const Mem *a, const Mem *b) {   /* sqlite3MemCompare(a, b) of two QV_NUM values */
  if (a->flags == MEM_Int) return b->flags == MEM_Int ? (a->u.i < b->u.i ? -1 : a->u.i > b->u.i) : sqlite3IntFloatCompare(a->u.i, b->u.r);
  if (b->flags == MEM_Int) return -sqlite3IntFloatCompare(b->u.i, a->u.r);
  return a->u.r < b->u.r ? -1 : a->u.r > b->u.r;
}
static QV_INLINE int qv_numfn(const FuncDef *f, int argc, const Mem *a, Mem *o) {
  void (*x)(sqlite3_context *, int, sqlite3_value **) = f->xSFunc;
  u16 f0 = a[0].flags;
  if (x == minmaxFunc) {
    int best = 0, mask = f->pUserData ? -1 : 0, nul = 0;
    if (argc < 2) return 0;
    for (int i = 0; i < argc; i++) { if (a[i].flags == MEM_Null) nul = 1; else if (!QV_NUM(a[i].flags)) return 0; }
    if (nul) { o->flags = MEM_Null; return 1; }
    for (int i = 1; i < argc; i++) if ((qv_numcmp(&a[best], &a[i]) ^ mask) >= 0) best = i;
    o->flags = a[best].flags; o->u = a[best].u; return 1;
  }
  if (argc != 1) return 0;
  if (x == absFunc) {
    if (f0 == MEM_Int) { if (a->u.i == SMALLEST_INT64) return 0; qv_int(o, a->u.i < 0 ? -a->u.i : a->u.i); return 1; }
    if (f0 == MEM_Real) { double r = a->u.r; if (r < 0) r = -r; o->flags = MEM_Real; o->u.r = r; return 1; }
    if (f0 == MEM_Null) { o->flags = MEM_Null; return 1; }
    return 0;
  }
#ifdef SQLITE_ENABLE_MATH_FUNCTIONS
  if (x == ceilingFunc || x == math1Func) {
    if (f0 == MEM_Null) { o->flags = MEM_Null; return 1; }
    if (!QV_NUM(f0)) return 0;
    if (x == ceilingFunc && f0 == MEM_Int) { qv_int(o, a->u.i); return 1; }
    double r = ((double (*)(double))f->pUserData)(f0 == MEM_Int ? (double)a->u.i : a->u.r);
    if (sqlite3IsNaN(r)) o->flags = MEM_Null; else { o->flags = MEM_Real; o->u.r = r; }
    return 1;
  }
#endif
  return 0;
}
/* like(pattern, text) for the commonest pattern, '%word%' with an ASCII word and no other wildcard (two arguments, the
   case-insensitive default: likeInfoNorm), as a search for the word with ASCII case folded. patternCompare's answer for
   that pattern: the text up to its first NUL contains the word, ASCII letters matching either case and every other byte
   only itself (a byte < 0x80 is always a character of its own to sqlite3Utf8Read). 0: not that case, call likeFunc */
/* a pattern qv_like answers: '%w%' (contains) or 'w%' (starts with), w of 1-64 ASCII characters and no wildcard but '_'
   (any one character: 0 in L->w), where an ESCAPE character makes the next one literal ('\\_%' ESCAPE '\\':
   starts with "_"); L->w lower case. Anything else is
   likeFunc's (trilium revisions.edited_on_date: "LIKE '2025-03-17%'" and "NOT LIKE '\\_%' ESCAPE '\\'" a row) */
static int qv_like_scan(QvLike *L, const u8 *p, int n, int esc, sqlite3 *db);
static int qv_like_pat(QvLike *L, const Mem *pat, int esc, sqlite3 *db) {
  int n = pat->n; const u8 *p = (const u8 *)pat->z;
  if (n == L->n && esc == L->esc && !memcmp(p, L->p, (size_t)n)) return L->ok;   /* (the pattern of the last call: a constant, or an equal binding) */
  L->n = -1; L->ok = 0;
  int ok = qv_like_scan(L, p, n, esc, db);
  if (n <= 66) { memcpy(L->p, p, (size_t)n); L->n = n; L->esc = (u8)esc; L->ok = (u8)ok; }   /* (a pattern for likeFunc remembered too: chromium
                                       visit.google_search_visits' four 'https://www.google.__/search%' a row were scanned to their '_' each time, 1.16 -> 1.59) */
  return ok;
}
static int qv_like_scan(QvLike *L, const u8 *p, int n, int esc, sqlite3 *db) {
  if (n < 2 || n > 66 || n > db->aLimit[SQLITE_LIMIT_LIKE_PATTERN_LENGTH] || p[n - 1] != '%') return 0;
  int i = p[0] == '%', end = n - 1, m = 0, any = 0;
  for (; i < end; i++) {
    u8 c = p[i];
    if (esc && c == esc) { if (i + 1 >= end) return 0; c = p[++i]; if (c == 0 || c >= 0x80) return 0; }   /* (an escaped trailing '%': likeFunc's) */
    else if (c == '_') { if (m == 64) return 0; L->w[m++] = 0; any = 1; continue; }   /* (one character) */
    else if (c == 0 || c >= 0x80 || c == '%' || c == '_') return 0;
    if (m == 64) return 0;
    L->w[m++] = sqlite3UpperToLower[c];
  }
  if (m < 1) return 0;
  L->m = m; L->pre = p[0] != '%'; L->any = (u8)any; return 1;
}
/* how many of w's m characters match z from its start: a '_' (0 in w) is one character as sqlite3Utf8Read reads it (a lead
   byte >= 0xc0 and its continuation bytes; any other byte alone), not a NUL; a NUL in z ends it (chromium
   visit.google_search_visits: 'https://www.google.__/search%' and three more a row) */
static QV_INLINE int qv_like_any(const u8 *z, int n, const u8 *w, int m) {
  int k = 0;
  for (int i = 0; k < m && i < n; k++) {
    u8 c = z[i++];
    if (w[k]) { if (sqlite3UpperToLower[c] != w[k]) break; }
    else if (c == 0) break;
    else if (c >= 0xc0) while (i < n && (z[i] & 0xc0) == 0x80) i++;
  }
  return k;
}
static int qv_like_find(const u8 *z, int n, const u8 *w, int m) {   /* the first i where w (lower case) matches z, folded; -1 */
  int i = 0, last = n - m;
#if SQLITE_BYTEORDER == 1234
  const u64 ones = 0x0101010101010101ULL, highs = 0x8080808080808080ULL;
  const u64 b0 = ones * w[0], fold = w[0] >= 'a' && w[0] <= 'z' ? ones * 0x20 : 0;   /* (b | 0x20) == a lower-case letter: that letter in either case */
  while (i <= last) {
    if (i + 8 <= n) {
      u64 x; memcpy(&x, z + i, 8); x = (x | fold) ^ b0;
      u64 t = (x - ones) & ~x & highs;   /* the lowest flagged byte is a real zero */
      if (!t) { i += 8; continue; }
      i += __builtin_ctzll(t) >> 3;
      if (i > last) break;
    } else if (sqlite3UpperToLower[z[i]] != w[0]) { i++; continue; }
    int k = 1; while (k < m && sqlite3UpperToLower[z[i + k]] == w[k]) k++;
    if (k == m && sqlite3UpperToLower[z[i]] == w[0]) return i;
    i++;
  }
#else
  for (; i <= last; i++) { int k = 0; while (k < m && sqlite3UpperToLower[z[i + k]] == w[k]) k++; if (k == m) return i; }
#endif
  return -1;
}
static int qv_like(QvLike *L, const FuncDef *f, const Mem *pat, const Mem *txt, int esc, Mem *o, sqlite3 *db) {
  if (f->pUserData != (void *)&likeInfoNorm) return 0;
  if ((pat->flags & (MEM_Str | MEM_Blob | MEM_Null)) != MEM_Str || pat->enc != SQLITE_UTF8 || !qv_like_pat(L, pat, esc, db)) return 0;
  if (txt->flags & MEM_Null) { o->flags = MEM_Null; return 1; }   /* (likeFunc sets no result: NULL) */
  if ((txt->flags & (MEM_Str | MEM_Blob)) != MEM_Str || txt->enc != SQLITE_UTF8) return 0;
  const u8 *z = (const u8 *)txt->z;
  if (L->pre) {   /* starts with w (a NUL in the text can't equal one of w's characters) */
    int k = 0;
    if (!L->any) { if (txt->n >= L->m) while (k < L->m && sqlite3UpperToLower[z[k]] == L->w[k]) k++; }
    else k = qv_like_any(z, txt->n, L->w, L->m);
    qv_int(o, k == L->m); return 1;
  }
  if (L->any) {   /* contains w with a '_': from each character's start, as patternCompare tries them after a '%', up to a NUL
                     (joplin resources.note_resources_page: LIKE '%scan_1%' on two columns a row) */
    int n = txt->n, hit = 0, w0 = L->w[0];
    for (int i = 0; i < n && z[i]; ) {
      if ((!w0 || sqlite3UpperToLower[z[i]] == w0) && qv_like_any(z + i, n - i, L->w, L->m) == L->m) { hit = 1; break; }
      if (z[i++] >= 0xc0) while (i < n && (z[i] & 0xc0) == 0x80) i++;
    }
    qv_int(o, hit); return 1;
  }
  int at = qv_like_find(z, txt->n, L->w, L->m);
  qv_int(o, at >= 0 && !memchr(z, 0, (size_t)at + (size_t)L->m));   /* (a NUL before the word ends the text for patternCompare) */
  return 1;
}
/* substr(x, p1[, p2]) in line for UTF-8 text x and plain integers p1, p2: substrFunc's arithmetic (a negative p1 counts
   from the end, a negative p2 takes the characters before p1, p1 0 is before the first) and its walk (a character ends at
   the next byte that isn't a continuation byte; a NUL ends the text), bounded by x->n since qvm's texts are unterminated,
   and the characters copied into o's own buffer as sqlite3_result_text's SQLITE_TRANSIENT; 0 = call substrFunc. trilium
   entity_changes.depending_entity: "SUBSTR(orig_sector.entityId, 1, 1) = ?1" on 39k index entries a run; firefox
   history.fetch_page_info: sixteen substr(:page_url, -k, 1) */
static int qv_substr(int argc, const Mem *x, Mem *o) {
  if ((x->flags & (MEM_Str | MEM_Blob | MEM_Zero)) != MEM_Str || x->enc != SQLITE_UTF8 || o == x) return 0;
  if (x[1].flags != MEM_Int || (argc == 3 && x[2].flags != MEM_Int)) return 0;
  i64 p1 = x[1].u.i, p2 = argc == 3 ? x[2].u.i : LARGEST_INT64;
  const u8 *z = (const u8 *)x->z; int n = x->n, i = 0;
  if (p1 > 0 && p2 >= 0) p1--;   /* (the common case first) */
  else {
    if (p1 < -((i64)1 << 40) || p2 < -((i64)1 << 40)) return 0;   /* (no overflow in the arithmetic below) */
#ifdef SQLITE_SUBSTR_COMPATIBILITY
    if (p1 == 0) p1 = 1;
#endif
    if (p1 < 0) {
      p1 += qc_utf8_chars(z, n);   /* (lengthFunc's count is substrFunc's: up to a NUL, a lead byte >= 0xc0 taking its continuations) */
      if (p1 < 0) { p2 = p2 < 0 ? 0 : p2 + p1; p1 = 0; }
    } else if (p1 > 0) p1--;
    else if (p2 > 0) p2--;
    if (p2 < 0) { if (p2 < -p1) p2 = p1; else p2 = -p2; p1 -= p2; }
  }
#define QV_SKIP_CHAR() { if (i >= n || z[i] == 0) break; if (z[i++] >= 0x80) while (i < n && (z[i] & 0xc0) == 0x80) i++; }
  for (; p1 > 0; p1--) QV_SKIP_CHAR();
  int at = i;
  for (; p2 > 0; p2--) QV_SKIP_CHAR();
#undef QV_SKIP_CHAR
  int m = i - at;
  if (o->szMalloc < m + 1 && sqlite3VdbeMemClearAndResize(o, m + 1 < 32 ? 32 : m + 1)) return 0;
  memcpy(o->zMalloc, z + at, (size_t)m); o->zMalloc[m] = 0;
  o->z = o->zMalloc; o->n = m; o->enc = SQLITE_UTF8; o->flags = MEM_Str | MEM_Term;
  return 1;
}
/* lower(x) / upper(x) in line for UTF-8 text x: lowerFunc's / upperFunc's ASCII folding (sqlite3Tolower, sqlite3Toupper:
   a byte >= 0x80 unchanged) over x's bytes, into o's own buffer as their sqlite3_result_text; NULL gives NULL; 0 = call
   the function (a number or a blob, which sqlite3_value_text converts). joplin resources.note_resources_page:
   LOWER(COALESCE(x, '')) LIKE ? on two columns, a context, a call and a malloc and free a row each (D2) */
static int qv_case(const Mem *x, Mem *o, int up) {
  if (x->flags & MEM_Null) { o->flags = MEM_Null; return 1; }
  if ((x->flags & (MEM_Str | MEM_Blob | MEM_Zero)) != MEM_Str || x->enc != SQLITE_UTF8 || o == x || x->z == o->zMalloc) return 0;
  int n = x->n; const u8 *z = (const u8 *)x->z;
  if (o->szMalloc < n + 1 && sqlite3VdbeMemClearAndResize(o, n + 1 < 32 ? 32 : n + 1)) return 0;
  u8 *d = (u8 *)o->zMalloc;
  if (up) for (int i = 0; i < n; i++) d[i] = (u8)sqlite3Toupper(z[i]);
  else for (int i = 0; i < n; i++) d[i] = (u8)sqlite3Tolower(z[i]);
  d[n] = 0; o->z = o->zMalloc; o->n = n; o->enc = SQLITE_UTF8; o->flags = MEM_Str | MEM_Term;
  return 1;
}
static int qv_numfn_has(const FuncDef *f) {   /* a function qv_numfn, qv_length or qv_substr may run in line */
  void (*x)(sqlite3_context *, int, sqlite3_value **) = f->xSFunc;
#ifdef SQLITE_ENABLE_MATH_FUNCTIONS
  if (x == ceilingFunc || x == math1Func) return 1;
#endif
  return x == minmaxFunc || x == absFunc || x == lengthFunc || x == likeFunc || x == substrFunc || x == lowerFunc || x == upperFunc;
}
/* a text argument as OP_Column leaves it: copied into the register's own buffer with its terminator. qvm's columns point
   into the page unterminated, so sqlite3_value_text in the function would otherwise add one through valueToText and
   sqlite3VdbeMemGrow on every row (LIKE over a name column: 1.15x the interpreter's instructions) */
static QV_NOINLINE int qv_term(Mem *a) {
  int n = a->n;
  if (a->z != a->zMalloc) {
    if (a->szMalloc < n + 2) { char *z = a->z; if (sqlite3VdbeMemClearAndResize(a, n + 2 < 32 ? 32 : n + 2)) return 0; memcpy(a->zMalloc, z, (size_t)n); }   /* (z: a page or the arena, not the old buffer) */
    else memmove(a->zMalloc, a->z, (size_t)n);   /* (memmove: a value already inside the buffer, at an offset) */
    a->z = a->zMalloc;
  } else if (a->szMalloc < n + 2) return 0;
  a->z[n] = 0; a->z[n + 1] = 0;
  a->flags = (u16)((a->flags & ~(MEM_Static | MEM_Ephem)) | MEM_Term);
  return 1;
}
static QV_INLINE int qv_call(QvX *X, QvCtx *k, FuncDef *f, int argc, Mem *out, int mask) {   /* xSFunc into out, as vdbe.c's OP_Function (mask: its P1) */
  for (int i = 0; i < argc; i++) {
    Mem *a = k->u.c.argv[i];
    if ((a->flags & (MEM_Str | MEM_Term | MEM_Dyn | MEM_Agg)) == MEM_Str && !qv_term(a)) { qv_why = "oom"; return 0; }
  }
  k->u.c.pOut = out; k->u.c.pMem = 0;
  MemSetTypeFlag(out, MEM_Null);
  f->xSFunc(&k->u.c, argc, k->u.c.argv);
  if (k->u.c.isError) {   /* (-1: sqlite3_set_auxdata added an entry, not an error: as OP_Function, the entries of arguments that
                             aren't constant deleted. calibre annotations.count_for_book's json_extract caches its parse so, and
                             fell back on every run) */
    if (k->u.c.isError > 0) { qv_why = "function error"; return 0; }
    sqlite3VdbeDeleteAuxData(X->db, &X->v->pAuxData, k->u.c.iOp, mask); k->u.c.isError = 0;
  }
  if (X->db->mallocFailed) { qv_why = "function error"; return 0; }
  if ((out->flags & MEM_Str) && out->enc != SQLITE_UTF8) { qv_why = "encoding"; return 0; }
  return 1;
}
static QV_INLINE int qv_like_esc(const Mem *esc) {   /* LIKE's ESCAPE qv_like takes: one ASCII byte, neither wildcard; else -1 */
  if ((esc->flags & (MEM_Str | MEM_Blob | MEM_Null)) != MEM_Str || esc->n != 1) return -1;
  u8 c = (u8)esc->z[0];
  return c == '%' || c == '_' || c == 0 || c >= 0x80 ? -1 : c;
}
/* likeFunc's answer for a NULL pattern without calling it: NULL, when it would neither convert the text in place (a string
   or NULL) nor raise an ESCAPE error (a NULL escape, or one ASCII character). beets' items_anyfield_flex binds no value to
   its six anonymous parameters in bench's corpus: six NULL patterns a row, each through the slow path and likeFunc */
static QV_INLINE int qv_like_null_ok(int argc, const Mem *txt, const Mem *esc) {
  if (!(txt->flags & (MEM_Str | MEM_Null)) || (txt->flags & (MEM_Blob | MEM_Zero))) return 0;
  if (argc == 2) return 1;
  if (esc->flags & MEM_Null) return 1;
  return (esc->flags & (MEM_Str | MEM_Blob | MEM_Zero)) == MEM_Str && esc->n == 1 && (u8)esc->z[0] < 0x80;
}
static QV_NOINLINE int qv_function_slow(QvX *X, const Op *op, int pc, Mem *out) {   /* the first call, and the built-ins run in line */
  sqlite3_context *pc4 = op->p4.pCtx; FuncDef *f = pc4->pFunc; int argc = pc4->argc;
  void (*x)(sqlite3_context *, int, sqlite3_value **) = f->xSFunc;   /* (one test per function: beets' six LIKEs a row come here) */
  if (x == likeFunc) {
    if ((X->r[op->p2].flags & MEM_Null) && qv_like_null_ok(argc, &X->r[op->p2 + 1], argc > 2 ? &X->r[op->p2 + 2] : 0)) {
      out->flags = MEM_Null; return 1;   /* a NULL pattern: NULL, as likeFunc leaves it */
    }
  } else if (x == substrFunc) { if (argc >= 2 && qv_substr(argc, &X->r[op->p2], out)) return 1; }
  else if (x == lengthFunc) { if (argc == 1 && qv_length(&X->r[op->p2], out)) return 1; }
  else if (x == lowerFunc || x == upperFunc) { if (argc == 1 && qv_case(&X->r[op->p2], out, x == upperFunc)) return 1; }
  else if (argc >= 1 && qv_numfn(f, argc, &X->r[op->p2], out)) return 1;
  QvCtx *k = qv_ctx(X, pc, f, argc, op->p2); if (!k) return 0;
  if (x == likeFunc) {
    int esc = argc == 3 ? qv_like_esc(&X->r[op->p2 + 2]) : 0;
    if ((argc == 2 || (argc == 3 && esc > 0)) && qv_like(&k->lk, f, &X->r[op->p2], &X->r[op->p2 + 1], esc, out, X->db)) return 1;
  }
  if (x != likeFunc && x != substrFunc && x != lengthFunc && x != lowerFunc && x != upperFunc) k->plain = !qv_numfn_has(f);   /* (those three never are: 11 instructions a call
                                       on google_search_visits' four rejected LIKEs a row) */
  return qv_call(X, k, f, argc, out, op->p1);
}
static QV_INLINE int qv_function(QvX *X, const Op *op, int pc) {   /* OP_Function: as vdbe.c, into register P3 */
  Mem *out = qv_out(X, op->p3); QvCtx *k = X->octx[pc];
  if (k && k->plain) {   /* any other function: straight to it (its context is built) */
    k->u.c.isError = 0; k->u.c.skipFlag = 0;
    return qv_call(X, k, op->p4.pCtx->pFunc, op->p4.pCtx->argc, out, op->p1);
  }
  return qv_function_slow(X, op, pc, out);
}
/* the native integer sum becomes SQLite's SumCtx in the accumulator register, as sumStep would have built it */
static int qv_sum_spill(QvX *X, const Op *op, int r) {
  struct QvSum *a = &X->P->sacc[r];
  sqlite3_context c; memset(&c, 0, sizeof c);
  c.pMem = &X->r[r]; c.pFunc = op->p4type == P4_FUNCDEF ? op->p4.pFunc : op->p4.pCtx->pFunc;
  if (c.pMem->flags & MEM_Agg) { qv_why = "sum state"; return 0; }
  SumCtx *p = sqlite3_aggregate_context(&c, sizeof *p);
  if (!p) { qv_why = "oom"; return 0; }
  p->iSum = a->iSum; p->cnt = a->cnt; p->approx = 0; p->ovrfl = 0;
  a->spilled = 1;
  return 1;
}
static int qv_aggstep(QvX *X, const Op *op, int pc, int inverse) {   /* OP_AggStep1 */
  FuncDef *f = op->p4type == P4_FUNCDEF ? op->p4.pFunc : op->p4.pCtx->pFunc; int argc = op->p5;
  Mem *acc = &X->r[op->p3];
  if (f->xSFunc == minmaxStep && !inverse && argc == 1 && (acc->flags & MEM_Agg)) {   /* min() / max() of two integers in line: minmaxStep's
                                                    sqlite3MemCompare and copy without its calls (wakapi user_heartbeats_range, two a row) */
    Mem *b = (Mem *)acc->z, *a = &X->r[op->p2];   /* (qv_ctx's argv[0]) */
    if ((b->flags & (MEM_TypeMask | MEM_IntReal | MEM_Subtype)) == MEM_Int && (a->flags & (MEM_TypeMask | MEM_IntReal | MEM_Subtype)) == MEM_Int) {
      acc->n++;
      if (f->pUserData ? b->u.i < a->u.i : b->u.i > a->u.i) { memcpy(b, a, MEMCELLSIZE); b->flags &= ~MEM_Dyn; }
      else if (pc > 0 && X->v->aOp[pc - 1].opcode == OP_CollSeq && X->v->aOp[pc - 1].p1) qv_int(qv_out(X, X->v->aOp[pc - 1].p1), 1);   /* (sqlite3SkipAccumulatorLoad) */
      return 1;
    }
  }
  QvCtx *k = qv_ctx(X, pc, f, argc, op->p2); if (!k) return 0;
  k->u.c.pMem = acc; k->u.c.pOut = &k->res;
  acc->n++;
  if (inverse) f->xInverse(&k->u.c, argc, k->u.c.argv); else f->xSFunc(&k->u.c, argc, k->u.c.argv);
  int ok = k->u.c.isError <= 0 && !X->db->mallocFailed;   /* (-1: sqlite3SkipAccumulatorLoad, a min/max row that changed nothing) */
  if (!ok) qv_why = "aggregate error";
  if (ok && k->u.c.skipFlag && pc > 0 && X->v->aOp[pc - 1].opcode == OP_CollSeq && X->v->aOp[pc - 1].p1) qv_int(qv_out(X, X->v->aOp[pc - 1].p1), 1);
  if (VdbeMemDynamic(&k->res) || k->res.szMalloc) sqlite3VdbeMemRelease(&k->res);
  k->res.flags = MEM_Null;
  return ok;
}
static int qv_aggfinal(QvX *X, const Op *op) {   /* AggFinal, or AggValue (P3) */
  FuncDef *f = op->p4.pFunc; Mem *acc = &X->r[op->p1]; int rc;
  if (op->p3) rc = sqlite3VdbeMemAggValue(acc, qv_out(X, op->p3), f);
  else rc = sqlite3VdbeMemFinalize(acc, f);
  if (rc) { qv_why = "aggregate final"; return 0; }
  Mem *m = op->p3 ? &X->r[op->p3] : acc;
  if ((m->flags & MEM_Str) && m->enc != SQLITE_UTF8) { qv_why = "encoding"; return 0; }
  return 1;
}

/* ---------------------------------------------------------------- the lifter: is every opcode one qvm runs? */
static int qv_op_ok(Vdbe *v, const Op *o) {
  switch (o->opcode) {
    case OP_Init: case OP_Goto: case OP_Gosub: case OP_Return: case OP_BeginSubrtn: case OP_InitCoroutine:
    case OP_Yield: case OP_EndCoroutine: case OP_Halt: case OP_Integer: case OP_Int64: case OP_Real: case OP_String8:
    case OP_Blob: case OP_Variable: case OP_Copy: case OP_SCopy: case OP_IntCopy: case OP_Move: case OP_SoftNull:
    case OP_Null: case OP_ResultRow: case OP_Noop: case OP_Explain: case OP_Once: case OP_If: case OP_IfNot:
    case OP_IsNull: case OP_NotNull: case OP_ZeroOrNull: case OP_IfPos: case OP_IfNotZero: case OP_DecrJumpZero:
    case OP_OffsetLimit: case OP_AddImm: case OP_MustBeInt: case OP_RealAffinity: case OP_Cast: case OP_Affinity:
    case OP_Eq: case OP_Ne: case OP_Lt: case OP_Le: case OP_Gt: case OP_Ge: case OP_ElseEq: case OP_Compare:
    case OP_Jump: case OP_Permutation: case OP_Not: case OP_BitNot: case OP_And: case OP_Or: case OP_IsTrue:
    case OP_BitAnd: case OP_BitOr: case OP_ShiftLeft: case OP_ShiftRight: case OP_Add: case OP_Subtract:
    case OP_Multiply: case OP_Divide: case OP_Remainder: case OP_Concat: case OP_AggValue: case OP_AggFinal:
    case OP_CollSeq: case OP_OpenEphemeral: case OP_OpenAutoindex: case OP_OpenDup: case OP_OpenPseudo:
    case OP_SorterOpen: case OP_Close: case OP_ColumnsUsed: case OP_CursorHint: case OP_NullRow: case OP_IfNullRow:
    case OP_Rewind: case OP_Sort: case OP_SorterSort: case OP_Last: case OP_Next: case OP_Prev: case OP_SorterNext:
    case OP_SorterData: case OP_SorterInsert: case OP_SorterCompare: case OP_Sequence: case OP_SequenceTest:
    case OP_SeekGE: case OP_SeekGT: case OP_SeekLE: case OP_SeekLT: case OP_SeekRowid: case OP_NotExists:
    case OP_IdxGT: case OP_IdxGE: case OP_IdxLT: case OP_IdxLE: case OP_DeferredSeek: case OP_FinishSeek:
    case OP_IdxRowid: case OP_Rowid: case OP_Column: case OP_RowData: case OP_MakeRecord: case OP_IdxInsert:
    case OP_Insert: case OP_NewRowid: case OP_Delete: case OP_ResetSorter: case OP_Found: case OP_NotFound:
    case OP_NoConflict: case OP_IfNoHope: case OP_SeekHit: case OP_Filter: case OP_FilterAdd: case OP_Count:
    case OP_SeekScan: case OP_IfEmpty: case OP_Transaction: case OP_TableLock: case OP_RowSetAdd: case OP_RowSetRead:
    case OP_ClrSubtype:
      return 1;
    case OP_RowSetTest: return o->p4type == P4_INT32;
    case OP_String: return o->p3 == 0 || (o->p3 > 0 && o->p3 <= v->nMem);   /* P3: the LIKE optimization's blob pass (qv_str) */
    case OP_OpenRead: case OP_ReopenIdx: return o->p4type == P4_INT32 || o->p4type == P4_KEYINFO;
    case OP_Function: case OP_PureFunc:
      return o->p4type == P4_FUNCCTX && o->p4.pCtx->argc <= QV_MAXARGS && qv_policy(o->p4.pCtx->pFunc);
    case OP_AggStep: case OP_AggInverse: case OP_AggStep1: {
      const FuncDef *f = o->p4type == P4_FUNCDEF ? o->p4.pFunc : o->p4type == P4_FUNCCTX ? o->p4.pCtx->pFunc : 0;
      return f && o->p5 <= QV_MAXARGS && qv_policy(f) && (o->opcode != OP_AggInverse || f->xInverse);
    }
  }
  (void)v; return 0;
}
static int qv_cursor_op(int opc) {   /* opcodes whose P1 is a cursor */
  switch (opc) {
    case OP_Rewind: case OP_Last: case OP_Next: case OP_Prev: case OP_Column: case OP_Rowid: case OP_RowData: case OP_SeekRowid:
    case OP_NotExists: case OP_NewRowid: case OP_Insert: case OP_Delete: case OP_ResetSorter: case OP_NullRow: case OP_IfNullRow:
    case OP_IfEmpty: case OP_Close: case OP_Count: case OP_OpenDup: case OP_OpenEphemeral: case OP_Sort: case OP_SorterSort:
    case OP_Found: case OP_NotFound: case OP_IdxInsert: case OP_NoConflict: case OP_SeekGE: case OP_SeekGT: case OP_SeekLE:
    case OP_SeekLT: case OP_IdxGT: case OP_IdxGE: case OP_IdxLT: case OP_IdxLE: case OP_IdxRowid: case OP_OpenRead: case OP_ReopenIdx:
    case OP_OpenPseudo: case OP_SorterOpen: case OP_SorterInsert: case OP_SorterData: case OP_SorterNext: case OP_SorterCompare:
    case OP_Sequence: case OP_SequenceTest: case OP_IfNoHope: case OP_SeekHit: case OP_FinishSeek: case OP_OpenAutoindex:
    case OP_DeferredSeek: case OP_IdxDelete: case OP_SeekEnd: case OP_CursorHint: case OP_ColumnsUsed: case OP_Filter: case OP_FilterAdd:
      return 1;
  }
  return 0;
}
static void qv_free(QvPlan *P, sqlite3 *db);
static int qv_lift(Vdbe *v) {
  Op *aOp = v->aOp; int nOp = v->nOp, ntx = 0, iDb = -1, cookie = 0, gen = 0, nout = -1;
  if (v->nMem > 65536 || v->nCursor > 1024 || aOp[0].opcode != OP_Init) { qc_why = "qvm size"; return 0; }
  for (int pc = 0; pc < nOp; pc++) {
    Op *o = &aOp[pc];
    if (!qv_op_ok(v, o)) { qc_why = sqlite3OpcodeName(o->opcode); return 0; }
    switch (o->opcode) {
      case OP_Transaction:
        if (o->p2 != 0 || ntx++ || o->p4type != P4_INT32) { qc_why = "qvm transaction"; return 0; }
        iDb = o->p1; cookie = o->p3; gen = o->p4.i; break;
      case OP_TableLock: if (o->p3) { qc_why = "write lock"; return 0; } break;
      case OP_ResultRow:
        if (nout >= 0 && nout != o->p2) { qc_why = "result widths"; return 0; }
        nout = o->p2; break;
      case OP_NewRowid: if (o->p3) { qc_why = "autoincrement"; return 0; } break;
    }
  }
  qv_str8(v);
  if (nout < 0 || nout > QC_MAXOUT * 4) { qc_why = "qvm program"; return 0; }   /* no Transaction: a program that reads no table */
  for (int pc = 0; pc < nOp; pc++) if ((aOp[pc].opcode == OP_OpenRead || aOp[pc].opcode == OP_ReopenIdx) && aOp[pc].p3 != iDb) { qc_why = "qvm database"; return 0; }
  QvPlan *P = sqlite3MallocZero(sizeof *P + (sizeof(BtCursor *) + sizeof(Pgno) + 1) * (size_t)(v->nCursor + 1));
  if (!P) return 0;
  P->kind = 3; P->iDb = iDb; P->cookie = cookie; P->gen = gen; P->nout = nout; P->nMem = v->nMem; P->nCur = v->nCursor; P->nOp = nOp;
  P->bc = (BtCursor **)(P + 1); P->bcRoot = (Pgno *)(P->bc + v->nCursor + 1); P->bopen = (u8 *)(P->bcRoot + v->nCursor + 1);
  P->r = sqlite3MallocZero(sizeof(Mem) * (u64)(P->nMem + 1));
  if ((P->cmem = sqlite3MallocZero(sizeof(QvCur) * (u64)(P->nCur + 1) + 63))) P->c = (QvCur *)(((uintptr_t)P->cmem + 63) & ~(uintptr_t)63);
  P->octx = sqlite3MallocZero(sizeof(void *) * (u64)nOp);
  P->once = sqlite3MallocZero((u64)(nOp / 8 + 1));
  for (int pc = 0; pc < nOp; pc++) if (aOp[pc].opcode == OP_Once) P->hasOnce = 1;
  P->isCnt = sqlite3MallocZero((u64)(P->nMem + 1)); P->cnt = sqlite3MallocZero(sizeof(i64) * (u64)(P->nMem + 1));
  P->keep = sqlite3MallocZero(sizeof *P->keep * (u64)(P->nCur + 1));
  P->isVec = sqlite3MallocZero((u64)(P->nCur + 1));
  if (P->isVec) {   /* ephemeral rowid tables whose cursor family (OpenDup) only uses what a vector reproduces */
    int *fam = sqlite3MallocZero(sizeof(int) * (u64)(P->nCur + 1));
    if (fam) {
      for (int c = 0; c <= P->nCur; c++) fam[c] = -1;
      for (int pc = 0; pc < nOp; pc++) if ((aOp[pc].opcode == OP_OpenEphemeral) && aOp[pc].p4type != P4_KEYINFO && aOp[pc].p1 <= P->nCur) fam[aOp[pc].p1] = aOp[pc].p1;
      for (int pc = 0; pc < nOp; pc++) if (aOp[pc].opcode == OP_OpenDup && aOp[pc].p2 <= P->nCur && fam[aOp[pc].p2] >= 0) fam[aOp[pc].p1] = fam[aOp[pc].p2];
      for (int c = 0; c <= P->nCur; c++) if (fam[c] >= 0) P->isVec[c] = 1;
      for (int pc = 0; pc < nOp; pc++) {
        Op *o = &aOp[pc]; int c = -1, ok = 0;
        switch (o->opcode) {   /* opcodes whose P1 is a cursor */
          case OP_Rewind: case OP_Last: case OP_Next: case OP_Prev: case OP_Column: case OP_Rowid: case OP_RowData:
          case OP_SeekRowid: case OP_NotExists: case OP_NewRowid: case OP_Insert: case OP_Delete: case OP_ResetSorter:
          case OP_NullRow: case OP_IfNullRow: case OP_IfEmpty: case OP_Close: case OP_Count: case OP_OpenDup: case OP_OpenEphemeral:
            c = o->p1; ok = 1; break;
          case OP_Sort: case OP_SorterSort: case OP_Found: case OP_NotFound: case OP_IdxInsert: case OP_NoConflict: case OP_SeekGE:
          case OP_SeekGT: case OP_SeekLE: case OP_SeekLT: case OP_IdxGT: case OP_IdxGE: case OP_IdxLT: case OP_IdxLE: case OP_IdxRowid:
          case OP_OpenRead: case OP_ReopenIdx: case OP_OpenPseudo: case OP_SorterOpen: case OP_SorterInsert: case OP_SorterData:
          case OP_SorterNext: case OP_SorterCompare: case OP_Sequence: case OP_SequenceTest: case OP_IfNoHope: case OP_SeekHit:
          case OP_FinishSeek: case OP_OpenAutoindex:
            c = o->p1; ok = 0; break;
          case OP_DeferredSeek: c = o->p3; ok = 0; break;
        }
        if (c >= 0 && c <= P->nCur && fam[c] >= 0 && !ok) { int f = fam[c]; for (int k = 0; k <= P->nCur; k++) if (fam[k] == f) P->isVec[k] = 0; }
        if (o->opcode == OP_DeferredSeek && o->p1 <= P->nCur && fam[o->p1] >= 0) { int f = fam[o->p1]; for (int k = 0; k <= P->nCur; k++) if (fam[k] == f) P->isVec[k] = 0; }
      }
      sqlite3_free(fam);
    }
  }
  P->isHash = sqlite3MallocZero((u64)(P->nCur + 1));
  if (P->isHash && !qcf.noHash) {   /* (QV_NOHASH=1: A/B against the B-tree) */   /* an index whose cursor only sees OpenEphemeral, Found/NotFound on N registers, IdxInsert of a MakeRecord of N, Close, ResetSorter */
    for (int pc = 0; pc < nOp; pc++) {
      Op *o = &aOp[pc]; if (o->opcode != OP_OpenEphemeral || o->p4type != P4_KEYINFO || o->p1 > P->nCur) continue;
      int c = o->p1, n = 0, ok = 1; const KeyInfo *ki = o->p4.pKeyInfo;
      for (int q = 0; q < nOp && ok; q++) {
        Op *x = &aOp[q]; int on = -1;
        switch (x->opcode) {   /* (every opcode whose P1, or DeferredSeek's P3, can be a cursor) */
          case OP_OpenEphemeral: if (x->p1 == c && (x->p4type != P4_KEYINFO || x->p4.pKeyInfo != ki)) ok = 0; break;
          case OP_Close: case OP_ResetSorter: break;
          case OP_Found: case OP_NotFound: if (x->p1 == c) on = x->p4.i > 0 ? x->p4.i : 0; break;
          case OP_IdxInsert:
            if (x->p1 != c) break;
            on = (q > 0 && aOp[q - 1].opcode == OP_MakeRecord && aOp[q - 1].p3 == x->p2) ? aOp[q - 1].p2 : 0;
            for (int j = 0; j < nOp && on; j++) {   /* reached only from that MakeRecord */
              const Op *y = &aOp[j];
              if (((sqlite3OpcodeProperty[y->opcode] & OPFLG_JUMP) && y->p2 == q) || (y->opcode == OP_Jump && (y->p1 == q || y->p3 == q))) on = 0;
            }
            break;
          case OP_DeferredSeek: if (x->p3 == c || x->p1 == c) ok = 0; break;
          case OP_OpenDup: if (x->p2 == c || x->p1 == c) ok = 0; break;
          default: if (x->p1 == c && qv_cursor_op(x->opcode)) ok = 0; break;
        }
        if (on == 0) ok = 0;
        else if (on > 0) { if (n && n != on) ok = 0; n = on; }
      }
      if (ok && n > 0 && n <= QV_MAXARGS && n <= ki->nAllField) {
        for (int i = 0; i < n; i++) if (ki->aColl[i] && !sqlite3IsBinary(ki->aColl[i])) ok = 0;   /* equality is byte equality */
        if (ok) P->isHash[c] = (u8)n;
      }
    }
  }
  P->isTopn = sqlite3MallocZero((u64)(P->nCur + 1)); P->srtSkip = sqlite3MallocZero((u64)(P->nCur + 1));
  if (P->isTopn && P->isHash && !qcf.noTopn) {   /* (QV_NOTOPN=1: A/B against the B-tree) */   /* an index whose cursor only sees what a top-N sorter does (and at least one Last) */
    for (int pc = 0; pc < nOp; pc++) {
      Op *o = &aOp[pc]; if (o->opcode != OP_OpenEphemeral || o->p4type != P4_KEYINFO || o->p1 > P->nCur || P->isHash[o->p1]) continue;
      int c = o->p1, ok = 1, last = 0; const KeyInfo *ki = o->p4.pKeyInfo;
      for (int q = 0; q < nOp && ok; q++) {
        Op *x = &aOp[q];
        switch (x->opcode) {
          case OP_OpenEphemeral: if (x->p1 == c && (x->p4type != P4_KEYINFO || x->p4.pKeyInfo != ki)) ok = 0; break;
          case OP_Last: if (x->p1 == c) last = 1; break;
          case OP_Close: case OP_ResetSorter: case OP_Sequence: case OP_Rewind: case OP_Sort: case OP_Next: case OP_Prev:
          case OP_Column: case OP_NullRow: case OP_IfNullRow: case OP_IdxLE: case OP_IdxGT: case OP_IdxLT: case OP_IdxGE:
          case OP_IdxInsert: break;
          case OP_Delete: if (x->p1 == c && (x->p5 & OPFLAG_SAVEPOSITION)) ok = 0; break;
          case OP_DeferredSeek: if (x->p3 == c || x->p1 == c) ok = 0; break;
          case OP_OpenDup: if (x->p2 == c || x->p1 == c) ok = 0; break;
          default: if (x->p1 == c && qv_cursor_op(x->opcode)) ok = 0; break;
        }
      }
      if (ok && last) P->isTopn[c] = 1;
    }
  }
  if (!P->r || !P->c || !P->octx || !P->once || !P->isCnt || !P->cnt || !P->keep || !P->isVec || !P->isHash || !P->isTopn || !P->srtSkip || ((QvConn *)QCD(v->db)->pQvConn)->nplans >= QC_MAXPLANS) { qc_why = "plans"; qv_free(P, v->db); return 0; }   /* (out of memory: what was allocated so far, freed) */
  for (int r = 0; r <= P->nMem; r++) sqlite3VdbeMemInit(&P->r[r], v->db, MEM_Null);
  for (int pc = 0; pc < nOp; pc++) {   /* a register is a native counter when every aggregate opcode on it is count() */
    Op *o = &aOp[pc]; int r = -1;
    if (o->opcode == OP_AggStep || o->opcode == OP_AggInverse || o->opcode == OP_AggStep1) r = o->p3;
    else if (o->opcode == OP_AggFinal || o->opcode == OP_AggValue) r = o->p1;
    if (r < 0) continue;
    const FuncDef *f = o->p4type == P4_FUNCDEF ? o->p4.pFunc : o->p4type == P4_FUNCCTX ? o->p4.pCtx->pFunc : 0;
    int isCount = f && f->xSFunc == countStep && f->xFinalize == countFinalize && (o->opcode == OP_AggFinal || o->opcode == OP_AggValue || o->p5 <= 1);
    P->isCnt[r] = P->isCnt[r] == 2 ? 2 : isCount ? 1 : 2;
  }
  for (int r = 0; r <= P->nMem; r++) if (P->isCnt[r] == 2) P->isCnt[r] = 0;
  for (int pc = 0; pc < nOp; pc++) switch (aOp[pc].opcode) {
    case OP_Function: case OP_PureFunc: case OP_AggStep: case OP_AggStep1: case OP_AggInverse: case OP_AggFinal: case OP_AggValue:
    case OP_RowSetAdd: case OP_RowSetRead: case OP_RowSetTest: P->mayDyn = 1;
  }
  P->isSum = sqlite3MallocZero((u64)(P->nMem + 1)); P->sacc = sqlite3MallocZero(sizeof *P->sacc * (u64)(P->nMem + 1));
  P->fb = sqlite3MallocZero((u64)nOp + 1);
  for (int pc = 0; pc < nOp; pc++) if (aOp[pc].opcode == OP_Filter) {
    P->flt = sqlite3MallocZero(sizeof *P->flt * (u64)nOp); P->fltOn = sqlite3MallocZero(sizeof *P->fltOn * (u64)(P->nMem + 1));
    P->fpc = sqlite3MallocZero(sizeof *P->fpc * (u64)nOp);
    if (!P->flt || !P->fltOn || !P->fpc) { qc_why = "plans"; qv_free(P, v->db); return 0; }
    for (int k = 0; k < nOp; k++) if (aOp[k].opcode == OP_Filter) {
      P->fpc[P->nfpc++] = k; if (aOp[k].p1 > 0 && aOp[k].p1 <= P->nMem) P->fltOn[aOp[k].p1]++;
    }
    break;
  }
  if (!P->isSum || !P->sacc || !P->fb) { qc_why = "plans"; qv_free(P, v->db); return 0; }
  for (int pc = 0; pc < nOp; pc++) {   /* sum / total / avg of one argument, the same function on every opcode of the register */
    Op *o = &aOp[pc]; int r = -1;
    if (o->opcode == OP_AggStep || o->opcode == OP_AggInverse || o->opcode == OP_AggStep1) r = o->p3;
    else if (o->opcode == OP_AggFinal || o->opcode == OP_AggValue) r = o->p1;
    if (r < 0) continue;
    const FuncDef *f = o->p4type == P4_FUNCDEF ? o->p4.pFunc : o->p4type == P4_FUNCCTX ? o->p4.pCtx->pFunc : 0;
    int k = 0;
    if (f && f->xSFunc == sumStep && (o->opcode == OP_AggFinal || o->opcode == OP_AggValue || o->p5 == 1))
      k = f->xFinalize == sumFinalize ? 1 : f->xFinalize == totalFinalize ? 2 : f->xFinalize == avgFinalize ? 3 : 0;
    P->isSum[r] = (P->isSum[r] == 255 || !k || (P->isSum[r] && P->isSum[r] != k)) ? 255 : (u8)k;
  }
  for (int r = 0; r <= P->nMem; r++) if (P->isSum[r] == 255 || P->isCnt[r]) P->isSum[r] = 0;
  if (!(P->isAcc = sqlite3MallocZero((u64)(P->nMem + 1)))) { qc_why = "plans"; qv_free(P, v->db); return 0; }
  for (int r = 0; r <= P->nMem; r++) if ((P->isAcc[r] = P->isCnt[r] || P->isSum[r])) P->hasAcc = 1;
  { QvSig g = {0}; if (qv_sig(v, &g)) { P->sig = g.b; P->nsig = g.n; qv_stamp(v, &P->stamp); } }   /* (none: the plan isn't cached) */
  QCV(v)->pQc = P; QCV(v)->qcWant = 1; QC_COUNT_ADD(((QvConn *)QCD(v->db)->pQvConn)->nplans, P);
  return 1;
}

/* ---------------------------------------------------------------- the interpreter */
static const u8 qv_LT[] = {[OP_Ne] = 1, [OP_Eq] = 0, [OP_Gt] = 0, [OP_Le] = 1, [OP_Lt] = 1, [OP_Ge] = 0};
static const u8 qv_EQ[] = {[OP_Ne] = 0, [OP_Eq] = 1, [OP_Gt] = 0, [OP_Le] = 1, [OP_Lt] = 0, [OP_Ge] = 1};
static const u8 qv_GT[] = {[OP_Ne] = 1, [OP_Eq] = 0, [OP_Gt] = 1, [OP_Le] = 0, [OP_Lt] = 0, [OP_Ge] = 1};
/* OP_Eq..OP_Ge as vdbe.c: 1 jump, 0 not; sets iCompare */
/* applyNumericAffinity(p, 0) for a string in place in its page (no NUL after it): a short one is tried on a copy on the
   stack, as sqlite3MemRealValueRCSlowPath's sqlite3DbStrNDup copy without the allocation (netalertx sessions_calendar's
   datetime columns, NUMERIC affinity, two a compare); a string that reads as a number goes to applyNumericAffinity */
static QV_INLINE void qv_numaff(Mem *p) {
  if (!(p->flags & MEM_Term) && p->enc == SQLITE_UTF8 && p->z && p->n > 0 && p->n < 64) {
    char b[64]; double r; memcpy(b, p->z, (size_t)p->n); b[p->n] = 0;
    if (sqlite3AtoF(b, &r) <= 0) return;   /* (not a number: the string is left as it is) */
  }
  applyNumericAffinity(p, 0);
}
static int qv_compare_slow(QvX *X, const Op *op) {
  Mem *pIn1 = &X->r[op->p1], *pIn3 = &X->r[op->p3];
  u16 flags1 = pIn1->flags, flags3 = pIn3->flags; int res;
  if (flags1 & flags3 & MEM_Int) {
    res = pIn3->u.i > pIn1->u.i ? 1 : pIn3->u.i < pIn1->u.i ? -1 : 0;
    X->iCompare = res;
    return res < 0 ? qv_LT[op->opcode] : res == 0 ? qv_EQ[op->opcode] : qv_GT[op->opcode];
  }
  if ((flags1 | flags3) & MEM_Null) {
    if (op->p5 & SQLITE_NULLEQ) {
      res = ((flags1 & flags3 & MEM_Null) && !(flags3 & MEM_Cleared)) ? 0 : ((flags3 & MEM_Null) ? -1 : +1);
    } else {
      X->iCompare = 1;
      return (op->p5 & SQLITE_JUMPIFNULL) ? 1 : 0;
    }
  } else {
    char affinity = op->p5 & SQLITE_AFF_MASK;
    if (affinity >= SQLITE_AFF_NUMERIC) {
      if ((flags1 | flags3) & MEM_Str) {
        if ((flags1 & (MEM_Int | MEM_IntReal | MEM_Real | MEM_Str)) == MEM_Str) { qv_numaff(pIn1); flags3 = pIn3->flags; }
        if ((flags3 & (MEM_Int | MEM_IntReal | MEM_Real | MEM_Str)) == MEM_Str) qv_numaff(pIn3);
      }
    } else if (affinity == SQLITE_AFF_TEXT && ((flags1 | flags3) & MEM_Str)) {
      if (flags1 & MEM_Str) pIn1->flags &= ~(MEM_Int | MEM_Real | MEM_IntReal);
      else if (flags1 & (MEM_Int | MEM_Real | MEM_IntReal)) {
        sqlite3VdbeMemStringify(pIn1, SQLITE_UTF8, 1);
        flags1 = (pIn1->flags & ~MEM_TypeMask) | (flags1 & MEM_TypeMask);
        if (pIn1 == pIn3) flags3 = flags1 | MEM_Str;
      }
      if (flags3 & MEM_Str) pIn3->flags &= ~(MEM_Int | MEM_Real | MEM_IntReal);
      else if (flags3 & (MEM_Int | MEM_Real | MEM_IntReal)) {
        sqlite3VdbeMemStringify(pIn3, SQLITE_UTF8, 1);
        flags3 = (pIn3->flags & ~MEM_TypeMask) | (flags3 & MEM_TypeMask);
      }
    }
    res = sqlite3MemCompare(pIn3, pIn1, op->p4type == P4_COLLSEQ ? op->p4.pColl : 0);
  }
  X->iCompare = res;
  pIn3->flags = flags3; pIn1->flags = flags1;   /* undo applyAffinity()'s changes */
  return res < 0 ? qv_LT[op->opcode] : res == 0 ? qv_EQ[op->opcode] : qv_GT[op->opcode];
}
#define QV_TXT(fl) (((fl) & (MEM_Str | MEM_Null | MEM_Int | MEM_Real | MEM_IntReal | MEM_Blob)) == MEM_Str)
/* qv_compare_slow's two strings under BINARY (or no collation) with an affinity of TEXT or below (none changes a
   string): sqlite3BinaryCompare's memcmp, then the shorter first. Out of line, with few registers to save
   (qv_compare_slow's entry cost a third of a text compare; in line, qv_run's other ops lost up to 1 %) */
static QV_NOINLINE int qv_compare_str(QvX *X, const Op *op) {
  const Mem *pIn1 = &X->r[op->p1], *pIn3 = &X->r[op->p3]; int res;
  if (!QV_TXT(pIn1->flags) || !QV_TXT(pIn3->flags) || (op->p5 & SQLITE_AFF_MASK) > SQLITE_AFF_TEXT
      || (op->p4type == P4_COLLSEQ && (!sqlite3IsBinary(op->p4.pColl) || pIn3->enc != op->p4.pColl->enc))) return qv_compare_slow(X, op);
  int n = pIn3->n < pIn1->n ? pIn3->n : pIn1->n;
  if (!(res = memcmp(pIn3->z, pIn1->z, (size_t)n))) res = pIn3->n - pIn1->n;
  X->iCompare = res;
  return res < 0 ? qv_LT[op->opcode] : res == 0 ? qv_EQ[op->opcode] : qv_GT[op->opcode];
}
/* qv_compare_slow, with two integers, or plain integers and reals, in line (no affinity applies to them;
   sqlite3MemCompare's order of numbers); two strings through qv_compare_str */
static QV_INLINE int qv_compare_op(QvX *X, const Op *op) {
  const Mem *pIn1 = &X->r[op->p1], *pIn3 = &X->r[op->p3]; u16 flags1 = pIn1->flags, flags3 = pIn3->flags; int res;
  if (flags1 & flags3 & MEM_Int) res = pIn3->u.i > pIn1->u.i ? 1 : pIn3->u.i < pIn1->u.i ? -1 : 0;
  else if (QV_NUM(flags1) && QV_NUM(flags3)) res = qv_numcmp(pIn3, pIn1);
  else if (((flags1 | flags3) & MEM_Null) && !(op->p5 & SQLITE_NULLEQ)) { X->iCompare = 1; return (op->p5 & SQLITE_JUMPIFNULL) != 0; }   /* (vdbe.c's
                                    NULL without NULLEQ, in line: a nullable column's NULL took two calls, n8n find_soft_deleted's deletedAt) */
  else return qv_compare_str(X, op);
  X->iCompare = res;
  return res < 0 ? qv_LT[op->opcode] : res == 0 ? qv_EQ[op->opcode] : qv_GT[op->opcode];
}
/* qv_arith's common cases in line: two integers (no overflow, no division by zero), and Add..Divide of plain
   integers and reals with a real result (vdbe.c's fp_math: NaN and division by zero go to qv_arith); 0 = qv_arith */
static QV_INLINE int qv_arith_fast(QvX *X, const Op *op) {
  const Mem *a = &X->r[op->p1], *b = &X->r[op->p2]; u16 fa = a->flags, fb = b->flags; int opc = op->opcode;
  if (!QV_NUM(fa) || !QV_NUM(fb)) return 0;
  if (fa & fb & MEM_Int) {
    i64 iA = a->u.i, iB = b->u.i;
    switch (opc) {
      case OP_Add: if (QV_ADD64(&iB, iA)) return 0; break;
      case OP_Subtract: if (QV_SUB64(&iB, iA)) return 0; break;
      case OP_Multiply: if (QV_MUL64(&iB, iA)) return 0; break;
      case OP_Divide: if (iA == 0 || (iA == -1 && iB == SMALLEST_INT64)) return 0; iB /= iA; break;
      default: if (iA == 0) return 0; if (iA == -1) iA = 1; iB %= iA; break;
    }
    qv_int(qv_out(X, op->p3), iB); return 1;
  }
  if (opc == OP_Remainder) return 0;
  double rA = fa == MEM_Int ? (double)a->u.i : a->u.r, rB = fb == MEM_Int ? (double)b->u.i : b->u.r;
  switch (opc) {
    case OP_Add: rB += rA; break;
    case OP_Subtract: rB -= rA; break;
    case OP_Multiply: rB *= rA; break;
    default: if (rA == (double)0) return 0; rB /= rA; break;
  }
  if (sqlite3IsNaN(rB)) return 0;
  Mem *o = qv_out(X, op->p3); o->u.r = rB; o->flags = MEM_Real; return 1;
}
/* OP_Add..OP_Remainder as vdbe.c */
static void qv_arith(QvX *X, const Op *op) {
  Mem *pIn1 = &X->r[op->p1], *pIn2 = &X->r[op->p2]; u16 type1 = pIn1->flags, type2 = pIn2->flags; i64 iA, iB; double rA, rB;
  Mem *pOut = qv_out(X, op->p3);
  if (type1 & type2 & MEM_Int) {
  int_math:
    iA = pIn1->u.i; iB = pIn2->u.i;
    switch (op->opcode) {
      case OP_Add: if (sqlite3AddInt64(&iB, iA)) goto fp_math; break;
      case OP_Subtract: if (sqlite3SubInt64(&iB, iA)) goto fp_math; break;
      case OP_Multiply: if (sqlite3MulInt64(&iB, iA)) goto fp_math; break;
      case OP_Divide: if (iA == 0) goto is_null; if (iA == -1 && iB == SMALLEST_INT64) goto fp_math; iB /= iA; break;
      default: if (iA == 0) goto is_null; if (iA == -1) iA = 1; iB %= iA; break;
    }
    qv_int(pOut, iB); return;
  } else if ((type1 | type2) & MEM_Null) goto is_null;
  type1 = numericType(pIn1); type2 = numericType(pIn2);
  if (type1 & type2 & MEM_Int) goto int_math;
fp_math:
  rA = sqlite3VdbeRealValue(pIn1); rB = sqlite3VdbeRealValue(pIn2);
  switch (op->opcode) {
    case OP_Add: rB += rA; break;
    case OP_Subtract: rB -= rA; break;
    case OP_Multiply: rB *= rA; break;
    case OP_Divide: if (rA == (double)0) goto is_null; rB /= rA; break;
    default: iA = sqlite3VdbeIntValue(pIn1); iB = sqlite3VdbeIntValue(pIn2); if (iA == 0) goto is_null; if (iA == -1) iA = 1; rB = (double)(iB % iA); break;
  }
  if (sqlite3IsNaN(rB)) goto is_null;
  pOut->u.r = rB; pOut->flags = MEM_Real; return;
is_null:
  pOut->flags = MEM_Null;
}
static int qv_cursor_db(QvX *X, const Op *op, QvCur *C) {   /* OpenRead / ReopenIdx: the plan's cursor for this number */
  QvPlan *P = X->P; int i = op->p1; Pgno root = (Pgno)op->p2; KeyInfo *ki = op->p4type == P4_KEYINFO ? op->p4.pKeyInfo : 0;
  if (P->bopen[i] && P->bcRoot[i] != root) { sqlite3BtreeCloseCursor(P->bc[i]); P->bopen[i] = 0; }
  if (!P->bopen[i]) {
    if (!P->bc[i] && !(P->bc[i] = sqlite3MallocZero(sqlite3BtreeCursorSize()))) return 0;
    sqlite3BtreeCursorZero(P->bc[i]);
    if (sqlite3BtreeCursor(P->bt, root, 0, ki, P->bc[i]) != SQLITE_OK) { qv_why = "cursor"; return 0; }
    P->bopen[i] = 1; P->bcRoot[i] = root;
  }
  qv_cur_reset(C);
  C->kind = QVC_BT; C->bc = P->bc[i]; C->ki = ki; C->isTable = !ki; C->nullRow = 1;
  C->eqOnly = (op->p5 & OPFLAG_SEEKEQ) != 0;
  return qv_cur_alloc(X, C, ki ? ki->nAllField : op->p4.i);
}
/* ---------------------------------------------------------------- hash sets
** A membership-only ephemeral index (DISTINCT, count(DISTINCT)) as a hash set of its keys. Equality is what the
** index's comparison (sqlite3VdbeRecordCompare, BINARY collations, default_rc 0) decides: NULL equals NULL, numbers
** by value (an integer and an integral real are equal), text and blobs by their bytes, no type equal to another. */
static inline int qv_hk_mem(const Mem *m, QvHk *k) {   /* in RecordCompare's order of a register's types */
  u16 f = m->flags;
  if (f & (MEM_Int | MEM_IntReal)) { k->c = 1; k->i = m->u.i; }
  else if (f & MEM_Real) qv_hk_num(k, m->u.r);
  else if (f & MEM_Str) { if (m->enc != SQLITE_UTF8) return 0; k->c = 3; k->z = (const u8 *)m->z; k->n = m->n; }
  else if (f & MEM_Blob) { if (f & MEM_Zero) return 0; k->c = 4; k->z = (const u8 *)m->z; k->n = m->n; }
  else k->c = 0;
  return 1;
}
static inline void qv_hk_val(const QcVal *v, QvHk *k) {
  switch (v->type) {
    case SQLITE_INTEGER: k->c = 1; k->i = v->i; break;
    case SQLITE_FLOAT: qv_hk_num(k, v->r); break;
    case SQLITE_TEXT: k->c = 3; k->z = v->z; k->n = v->n; break;
    case SQLITE_BLOB: k->c = 4; k->z = v->z; k->n = v->n; break;
    default: k->c = 0;
  }
}
static u64 qv_hk_hash(const QvHk *k, int nk) {
  u64 h = 0x243f6a8885a308d3ull;
  for (int i = 0; i < nk; i++, k++) {
    h = qv_hmix(h, k->c);
    if (k->c == 1) h = qv_hmix(h, (u64)k->i);
    else if (k->c == 2) { u64 b; memcpy(&b, &k->r, 8); h = qv_hmix(h, b); }
    else if (k->c >= 3) {
      int j = 0; for (; j + 8 <= k->n; j += 8) { u64 w; memcpy(&w, k->z + j, 8); h = qv_hmix(h, w); }
      u64 w = 0; memcpy(&w, k->z + j, (size_t)(k->n - j)); h = qv_hmix(h, w ^ ((u64)k->n << 56));
    }
  }
  return h;
}
static inline int qv_hk_eq(const QvHk *a, const QvHk *b, int nk) {
  for (int i = 0; i < nk; i++) {
    if (a[i].c != b[i].c) return 0;
    if (a[i].c == 1 ? a[i].i != b[i].i : a[i].c == 2 ? a[i].r != b[i].r : a[i].c >= 3 && (a[i].n != b[i].n || memcmp(a[i].z, b[i].z, (size_t)a[i].n))) return 0;
  }
  return 1;
}
/* the slot of key k (hash h): 1 present, 0 the empty slot it would take; -1 a stored record that doesn't decode */
static int qv_hs_find(const QvHset *H, const QvHk *k, int nk, u64 h, u32 *slot) {
  if (!H->cap) { *slot = 0; return 0; }
  QcVal v[QV_MAXARGS]; QvHk s[QV_MAXARGS];
  for (u32 i = (u32)h & (H->cap - 1);; i = (i + 1) & (H->cap - 1)) {
    if (!H->rec[i]) { *slot = i; return 0; }
    if (H->hv[i] != h) continue;
    const u8 *z = H->rec[i]; u32 n = (u32)(H->hv[H->cap + i]);   /* (the length is kept after the hashes) */
    if (!qv_decode(z, n, nk, v)) return -1;
    for (int j = 0; j < nk; j++) qv_hk_val(&v[j], &s[j]);
    if (qv_hk_eq(k, s, nk)) { *slot = i; return 1; }
  }
}
static int qv_hs_insert(QvX *X, QvHset *H, const u8 *rec, u32 len, int nk) {
  QcVal v[QV_MAXARGS]; QvHk k[QV_MAXARGS]; u32 slot;
  if (!qv_decode(rec, len, nk, v)) { qv_why = "hash record"; return 0; }
  for (int j = 0; j < nk; j++) qv_hk_val(&v[j], &k[j]);
  u64 h = qv_hk_hash(k, nk);
  if ((H->n + 1) * 2 > H->cap) {   /* grown in the arena (a set lives for one execution) */
    u32 nc = H->cap ? H->cap * 2 : 64;
    u64 *nh = qv_alloc(&X->A, sizeof *nh * 2 * (size_t)nc); const u8 **nr = qv_alloc(&X->A, sizeof *nr * (size_t)nc);
    if (!nh || !nr) { qv_why = "arena"; return 0; }
    memset(nr, 0, sizeof *nr * (size_t)nc);
    for (u32 i = 0; i < H->cap; i++) if (H->rec[i]) {
      u32 j = (u32)H->hv[i] & (nc - 1); while (nr[j]) j = (j + 1) & (nc - 1);
      nh[j] = H->hv[i]; nh[nc + j] = H->hv[H->cap + i]; nr[j] = H->rec[i];
    }
    H->hv = nh; H->rec = nr; H->cap = nc;
  }
  int f = qv_hs_find(H, k, nk, h, &slot);
  if (f < 0) { qv_why = "hash record"; return 0; }
  if (f) return 1;   /* (an equal key: the index would replace it, a set already has it) */
  H->hv[slot] = h; H->hv[H->cap + slot] = len; H->rec[slot] = rec; H->n++;
  return 1;
}
static int qv_open_eph(QvX *X, const Op *op, QvCur *C) {
  static const int vfsFlags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_EXCLUSIVE | SQLITE_OPEN_DELETEONCLOSE | SQLITE_OPEN_TRANSIENT_DB;
  KeyInfo *ki = op->p4type == P4_KEYINFO ? op->p4.pKeyInfo : 0;
  if (op->p3 > 0) { Mem *e = qv_out(X, op->p3); qv_static(e, "", 0, 0, 0); }   /* an empty record for Insert */
  if (!ki && X->P->isVec[op->p1] && !X->P->vecOff) {   /* a vector table */
    if (C->kind == QVC_VEC && !C->noReuse) { QvVec *V = C->vec; V->n = V->first = V->live = 0; C->nullRow = 1; C->vbad = 1; qv_stale(C); C->seqCount = 0; return 1; }
    if (C->kind) qv_cur_close(X, C);
    if (!(C->vec = qv_alloc(&X->A, sizeof *C->vec))) { qv_why = "arena"; return 0; }
    memset(C->vec, 0, sizeof *C->vec); C->vec->ref[0] = C; C->vec->nref = 1;
    C->kind = QVC_VEC; C->isTable = 1; C->nullRow = 1; C->vbad = 1; C->owner = 1;
    return qv_cur_alloc(X, C, op->p2);
  }
  if (ki && X->P->isTopn[op->p1] && !X->P->topnOff && !X->P->keep[op->p1].bt) {   /* a top-N sorter (unless an earlier run spilled it: then its kept B-tree) */
    if (C->kind == QVC_TOPN && !C->noReuse) { C->tn->n = 0; C->nullRow = 1; C->vbad = 1; qv_stale(C); C->seqCount = 0; return 1; }
    if (C->kind) qv_cur_close(X, C);
    size_t ur = ROUND8P(sizeof(UnpackedRecord)) + sizeof(Mem) * (size_t)(ki->nKeyField + 1);
    if (!(C->tn = qv_alloc(&X->A, sizeof *C->tn)) || !(C->tn->u = qv_alloc(&X->A, ur))) { qv_why = "arena"; return 0; }
    C->tn->n = 0; memset(C->tn->u, 0, ur);
    C->tn->u->aMem = (Mem *)&((char *)C->tn->u)[ROUND8P(sizeof(UnpackedRecord))]; C->tn->u->pKeyInfo = ki;
    C->kind = QVC_TOPN; C->ki = ki; C->nullRow = 1; C->vbad = 1; C->owner = 1; C->ep5 = op->p5;
    return qv_cur_alloc(X, C, ki->nAllField);
  }
  if (ki && X->P->isHash[op->p1] && !X->P->hashOff) {   /* a hash set */
    if (C->kind) qv_cur_close(X, C);
    if (!(C->hs = qv_alloc(&X->A, sizeof *C->hs))) { qv_why = "arena"; return 0; }
    memset(C->hs, 0, sizeof *C->hs);
    C->kind = QVC_HASH; C->ki = ki; C->hk = X->P->isHash[op->p1]; C->nullRow = 1;
    return 1;
  }
  if (C->kind == QVC_EPH && !C->noReuse && C->ebt) {   /* reopened, no duplicates: emptied (vdbe: ClearTable) */
    C->seqCount = 0; qv_stale(C); C->nullRow = 1;
    if (sqlite3BtreeClearTable(C->ebt, (int)C->eroot, 0) != SQLITE_OK) { qv_why = "eph clear"; return 0; }
    qv_stale_eph(X);
    return 1;
  }
  if (C->kind) qv_cur_close(X, C);
  struct QvEphKeep *k = &X->P->keep[op->p1];
  if (k->bt && k->ki == ki && k->p5 == op->p5) {   /* the one this statement kept from its last run: emptied */
    C->kind = QVC_EPH; C->copyVals = 1; C->ki = ki; C->isTable = !ki; C->nullRow = 1; C->owner = 1; C->ep5 = op->p5;
    C->ebt = k->bt; C->bc = k->bc; C->eroot = k->root; k->bt = 0;
    if (sqlite3BtreeClearTable(C->ebt, (int)C->eroot, 0) != SQLITE_OK) { qv_why = "eph clear"; return 0; }
    qv_stale_eph(X);
    return qv_cur_alloc(X, C, ki ? ki->nAllField : op->p2);
  }
  C->kind = QVC_EPH; C->copyVals = 1; C->ki = ki; C->isTable = !ki; C->nullRow = 1; C->owner = 1; C->ep5 = op->p5;
  if (!(C->bc = qv_bc_alloc(QV_CONN(X->db)))) return 0;
  if (sqlite3BtreeOpen(X->db->pVfs, 0, X->db, &C->ebt, BTREE_OMIT_JOURNAL | BTREE_SINGLE | op->p5, vfsFlags) != SQLITE_OK) { C->ebt = 0; qv_why = "eph open"; return 0; }
  if (sqlite3BtreeBeginTrans(C->ebt, 1, 0) != SQLITE_OK || (ki && sqlite3BtreeCreateTable(C->ebt, &C->eroot, BTREE_BLOBKEY | op->p5) != SQLITE_OK) ||
      sqlite3BtreeCursor(C->ebt, ki ? C->eroot : SCHEMA_ROOT, BTREE_WRCSR, ki, C->bc) != SQLITE_OK) {
    sqlite3BtreeClose(C->ebt); C->ebt = 0; qv_bc_free(QV_CONN(X->db), C->bc); C->bc = 0; C->kind = 0; qv_why = "eph open"; return 0;   /* no cursor yet: closed here */
  }
  if (!ki) C->eroot = SCHEMA_ROOT;
  return qv_cur_alloc(X, C, ki ? ki->nAllField : op->p2);
}
/* a top-N sorter past QV_TOPN_MAX entries (a large LIMIT): its entries moved into an ephemeral B-tree, as
   OpenEphemeral would have made it, and the cursor one on that B-tree from here on */
static int qv_topn_spill(QvX *X, QvCur *C) {
  static const int vfsFlags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_EXCLUSIVE | SQLITE_OPEN_DELETEONCLOSE | SQLITE_OPEN_TRANSIENT_DB;
  QvTopn *T = C->tn; KeyInfo *ki = C->ki; u16 p5 = C->ep5; i64 seq = C->seqCount; Btree *bt = 0; BtCursor *bc = qv_bc_alloc(QV_CONN(X->db)); Pgno root = 0;
  if (!bc) { qv_why = "oom"; return 0; }
  if (sqlite3BtreeOpen(X->db->pVfs, 0, X->db, &bt, BTREE_OMIT_JOURNAL | BTREE_SINGLE | p5, vfsFlags) != SQLITE_OK) { qv_bc_free(QV_CONN(X->db), bc); qv_why = "eph open"; return 0; }
  if (sqlite3BtreeBeginTrans(bt, 1, 0) != SQLITE_OK || sqlite3BtreeCreateTable(bt, &root, BTREE_BLOBKEY | p5) != SQLITE_OK ||
      sqlite3BtreeCursor(bt, root, BTREE_WRCSR, ki, bc) != SQLITE_OK) { sqlite3BtreeClose(bt); qv_bc_free(QV_CONN(X->db), bc); qv_why = "eph open"; return 0; }
  for (int i = 0; i < T->n; i++) {   /* (in key order: appends) */
    BtreePayload x; memset(&x, 0, sizeof x); x.pKey = T->rec[i]; x.nKey = T->len[i];
    if (sqlite3BtreeInsert(bc, &x, OPFLAG_APPEND, 0) != SQLITE_OK) { sqlite3BtreeCloseCursor(bc); qv_bc_free(QV_CONN(X->db), bc); qv_why = "eph insert"; return 0; }
  }
  qv_cur_reset(C);
  C->kind = QVC_EPH; C->copyVals = 1; C->ki = ki; C->nullRow = 1; C->owner = 1; C->ep5 = p5; C->seqCount = seq;
  C->ebt = bt; C->bc = bc; C->eroot = root; qv_stale_eph(X);
  return qv_cur_alloc(X, C, ki->nAllField);
}
static int qv_found(QvX *X, QvCur *C, const Op *op) {   /* Found / NotFound / NoConflict / IfNoHope: the seek; 1 ok */
  int nf = op->p4.i, rc;
  if (C->kind == QVC_HASH) {   /* (the lifter saw nf == the set's key width) */
    QvHk k[QV_MAXARGS]; u32 slot;
    for (int i = 0; i < nf; i++) if (!qv_hk_mem(&X->r[op->p3 + i], &k[i])) { X->P->hashOff = 1; qv_why = "hash deopt: key"; return 0; }
    rc = qv_hs_find(C->hs, k, nf, qv_hk_hash(k, nf), &slot);
    if (rc < 0) { qv_why = "hash record"; return 0; }
    C->seekResult = !rc;
    return 1;
  }
  if (nf > 0) {
    UnpackedRecord r; memset(&r, 0, sizeof r);
    r.pKeyInfo = C->ki; r.nField = (u16)nf; r.default_rc = 0; r.aMem = &X->r[op->p3];
    rc = sqlite3BtreeIndexMoveto(C->bc, &r, &C->seekResult);
  } else {
    Mem *x = &X->r[op->p3];
    if (!(x->flags & MEM_Blob) || (x->flags & MEM_Zero)) { qv_why = "found key"; return 0; }
    UnpackedRecord *pk = sqlite3VdbeAllocUnpackedRecord(C->ki); if (!pk) return 0;
    sqlite3VdbeRecordUnpack(x->n, x->z, pk); pk->default_rc = 0;
    rc = sqlite3BtreeIndexMoveto(C->bc, pk, &C->seekResult);
    sqlite3DbFree(X->db, pk);
  }
  if (rc) { qv_why = "found"; return 0; }
  return 1;
}

/* ---------------------------------------------------------------- opcode bodies shared by qv_run and the regions
   (in line in both: qv_run had them in its cases) */
/* OP_String (and String8, made String by the lifter) as vdbe.c: without SQLITE_LIKE_DOESNT_MATCH_BLOBS, register P3
   equal to P5 makes the value a BLOB (the second pass of the LIKE optimization's range over an index) */
static QV_INLINE void qv_str(QvX *X, Mem *R, const Op *op, Mem *o) {
  qv_static(o, op->p4.z, op->p1, 1, op->p4.z[op->p1] == 0);
#ifndef SQLITE_LIKE_DOESNT_MATCH_BLOBS
  if (op->p3 > 0 && R[op->p3].u.i == op->p5) o->flags = MEM_Blob | MEM_Static | MEM_Term;
#endif
  (void)X;
}
/* a loop's back edge, where vdbe.c checks too (jump_to_p2_and_check_for_interrupt): sqlite3_interrupt() and the
   progress handler stop the run, and the statement goes back to SQLite, which reports them as it would. The handler
   runs every nProgressOps iterations here, not opcodes: as a time limit that is the same thing. */
#define QV_TICK 64   /* iterations between checks: the loops stay cheap, and stopping 64 rows later is the same stop */
static void qv_rg_build(QvPlan *P, Vdbe *v);
static int qv_rg_on(void);
#ifndef QV_RG_EARLY
#define QV_RG_EARLY 1024   /* tick checks (x QV_TICK loop iterations) into a feedback run before its regions are built */
#endif
static QV_NOINLINE int qv_tick_check(QvX *X) {
  sqlite3 *db = X->db;
  X->tick = QV_TICK;
  /* a first run long enough to have its feedback: the regions now, not after two runs (a one-shot statement over a
     big loop, speedtest1's subquery in the result set: 25M rows, run once). Its next Next enters them (qv_run's) */
  if (++X->nticks == QV_RG_EARLY && X->fb && !X->rgAt && X->P && !X->P->rgAt && qv_rg_on()) { qv_rg_build(X->P, X->v); X->rgAt = X->P->rgAt; }
  if (AtomicLoad(&db->u1.isInterrupted)) { qv_why = "interrupted"; return 0; }
#ifndef SQLITE_OMIT_PROGRESS_CALLBACK
  if (db->xProgress) {
    for (X->nprog += QV_TICK; X->nprog >= db->nProgressOps; X->nprog -= db->nProgressOps)
      if (db->xProgress(db->pProgressArg)) { qv_why = "progress handler"; return 0; }
  }
#endif
  return 1;
}
static QV_INLINE int qv_tick(QvX *X) { return --X->tick > 0 || qv_tick_check(X); }
/* a streamed row handed out: its texts NUL-terminated, as vdbe.c's OP_Column leaves them (a text read in place from a
   pinned page has none, and column_text would add one through valueToText and a buffer grown on every call:
   vaultwarden users.find_by_uuid +1.9k instructions a run) */
static QV_NOINLINE int qv_strm_term1(Mem *m) {   /* (into the register's own buffer, kept from row to row: as OP_Column copies) */
  int n = m->n;
  if (m->szMalloc < n + 2) { if (sqlite3VdbeMemGrow(m, n + 2, 1)) { qv_why = "oom"; return 0; } }
  else if (m->z != m->zMalloc) { qc_cpy(m->zMalloc, (const u8 *)m->z, (size_t)n); m->z = m->zMalloc; }
  m->z[n] = 0; m->z[n + 1] = 0; m->flags = (m->flags & ~(MEM_Static | MEM_Ephem | MEM_Dyn)) | MEM_Term;
  return 1;
}
static QV_INLINE int qv_strm_term(Mem *R, int n) {
  for (int k = 0; k < n; k++) if ((R[k].flags & (MEM_Str | MEM_Term)) == MEM_Str && !qv_strm_term1(&R[k])) return 0;
  return 1;
}
/* a large result's row as QcVals (0: failed, qv_why). A row of one or two values inline (zotero
   collections.load_child_items, one integer a row: about 70 instructions a row through a call), a wider one out of line
   with its texts copied in its loop (calibre load.books_table_columns, 11 values: 2 % more with a call for each text) */
static QV_NOINLINE int qv_sink_grow(QvSink *K, size_t nv) {
  if (nv > ((size_t)1 << 22)) { qv_why = "rows"; return 0; }
  size_t c = K->cval ? K->cval * 2 : 4096; while (c < nv) c *= 2;
  QcVal *t = sqlite3_realloc64(K->val, c * sizeof *t); if (!t) { qv_why = "oom"; return 0; }
  K->val = t; K->cval = c;
  return 1;
}
static QV_INLINE int qv_sink_val(QvSink *K, const Mem *s, QcVal *o, char **pbuf, size_t *pused, size_t *pcbuf) {
  u16 f = s->flags;
  if ((f & (MEM_TypeMask | MEM_Zero)) == MEM_Str && s->enc == SQLITE_UTF8) {   /* a plain text first: most values of a wide row */
    size_t len = (size_t)s->n, used = *pused;
    if (*pcbuf - used < len + 1) {
      size_t c = (used + len + 1) * 2 + 256; char *t = sqlite3_realloc64(*pbuf, c); if (!t) { qv_why = "oom"; return 0; }
      K->buf = *pbuf = t; K->cbuf = *pcbuf = c;
    }
    char *b = *pbuf + used; qc_cpy(b, (const u8 *)s->z, len); b[len] = 0;
    o->type = SQLITE_TEXT; o->n = (int)len; o->i = (i64)used; *pused = used + len + 1;
    return 1;
  }
  if (f & MEM_Null) { o->type = SQLITE_NULL; o->n = 0; o->i = 0; return 1; }
  if (f & MEM_Int) { o->type = SQLITE_INTEGER; o->i = s->u.i; return 1; }
  if (f & MEM_IntReal) { o->type = SQLITE_FLOAT; o->r = (double)s->u.i; return 1; }
  if (f & MEM_Real) { o->type = SQLITE_FLOAT; o->r = s->u.r; return 1; }
  if (!(f & (MEM_Str | MEM_Blob))) { qv_why = "value"; return 0; }
  if (f & MEM_Zero) { qv_why = "zeroblob"; return 0; }
  if ((f & MEM_Str) && s->enc != SQLITE_UTF8) { qv_why = "encoding"; return 0; }
  size_t len = (size_t)s->n, used = *pused;
  if (*pcbuf - used < len + 1) {   /* (offsets: nothing to move) */
    size_t c = (used + len + 1) * 2 + 256; char *t = sqlite3_realloc64(*pbuf, c); if (!t) { qv_why = "oom"; return 0; }
    K->buf = *pbuf = t; K->cbuf = *pcbuf = c;
  }
  char *b = *pbuf + used; qc_cpy(b, (const u8 *)s->z, len); b[len] = 0;
  o->type = (f & MEM_Str) ? SQLITE_TEXT : SQLITE_BLOB; o->n = (int)len; o->i = (i64)used; *pused = used + len + 1;
  return 1;
}
static QV_NOINLINE int qv_sink_wide(QvX *X, QvSink *K, const Mem *V, int n) {
  size_t nv = K->nv + (size_t)n;
  if (nv > K->cval && !qv_sink_grow(K, nv)) return 0;
  QcVal *o = K->val + K->nv; char *buf = K->buf; size_t used = K->used, cbuf = K->cbuf;   /* (locals: a QcVal's i64 may alias them) */
  for (int k = 0; k < n; k++) {
    if (V[k].flags & MEM_Int) { o[k].type = SQLITE_INTEGER; o[k].i = V[k].u.i; continue; }   /* (an integer first: never with NULL,
                                       and a text that is also one kept as qv_sink_val keeps it; chromium segment.usage_all's
                                       27k rows of three integers took the text and NULL tests first) */
    if (!qv_sink_val(K, &V[k], &o[k], &buf, &used, &cbuf)) { K->used = used; return 0; }
  }
  K->used = used; K->nv = nv; X->nrow++;
  return 1;
}
static QV_NOINLINE int qv_sink_one(QvSink *K, const Mem *s, QcVal *o) {   /* a narrow row's text or blob */
  char *buf = K->buf; size_t used = K->used, cbuf = K->cbuf;
  int ok = qv_sink_val(K, s, o, &buf, &used, &cbuf); K->used = used;
  return ok;
}
static QV_INLINE int qv_sink_vals(QvX *X, QvSink *K, const Mem *V, int n) {
  if (n > 2) return qv_sink_wide(X, K, V, n);
  size_t nv = K->nv + (size_t)n;
  if (nv > K->cval && !qv_sink_grow(K, nv)) return 0;
  QcVal *o = K->val + K->nv;
  for (int k = 0; k < n; k++, o++) {
    const Mem *s = &V[k]; u16 f = s->flags;
    if (f & MEM_Null) { o->type = SQLITE_NULL; o->n = 0; o->i = 0; }
    else if (f & MEM_Int) { o->type = SQLITE_INTEGER; o->i = s->u.i; }
    else if (f & MEM_Real) { o->type = SQLITE_FLOAT; o->r = s->u.r; }
    else if (!qv_sink_one(K, s, o)) return 0;
  }
  K->nv = nv; X->nrow++;
  return 1;
}
static QV_NOINLINE int qv_sink_spill(QvX *X, QvSink *K, const Mem *V, int n) {   /* past cap: the Mems so far as QcVals, then V */
  size_t nv = K->nv, c = nv * 2 > 4096 ? nv * 2 : 4096;
  if (c > K->cval) { QcVal *t = sqlite3_realloc64(K->val, c * sizeof *t); if (!t) { qv_why = "oom"; return 0; } K->val = t; K->cval = c; }
  for (size_t k = 0; k < nv; k++) {
    const Mem *m = &K->row[k]; QcVal *o = &K->val[k];
    if (m->flags & MEM_Int) { o->type = SQLITE_INTEGER; o->i = m->u.i; }
    else if (m->flags & MEM_Real) { o->type = SQLITE_FLOAT; o->r = m->u.r; }
    else if (m->flags & (MEM_Str | MEM_Blob)) { o->type = (m->flags & MEM_Str) ? SQLITE_TEXT : SQLITE_BLOB; o->n = m->n; o->i = (i64)(m->z - K->buf); }
    else { o->type = SQLITE_NULL; o->n = 0; o->i = 0; }
  }
  K->vm = 1;
  return qv_sink_vals(X, K, V, n);
}
/* a row into the sink (0: failed, qv_why) */
static QV_INLINE int qv_sink_row(QvX *X, QvSink *K, const Mem *V, int n) {
  if (K->vm) return qv_sink_vals(X, K, V, n);
  size_t nv = K->nv + (size_t)n;
  if (nv > K->crow) {
    if (nv > K->cap) return qv_sink_spill(X, K, V, n);
    size_t c = K->crow ? nv * 2 : nv; if (c > K->cap) c = K->cap;   /* (a new statement's first row exactly: one-row results) */
    Mem *t = sqlite3_realloc64(K->row, c * sizeof *t); if (!t) { qv_why = "oom"; return 0; }
    for (size_t k = K->crow; k < c; k++) { t[k].flags = MEM_Null; t[k].eSubtype = 0; t[k].db = K->db; t[k].szMalloc = 0; }
    K->row = t; K->crow = c;
  }
  /* (the Mems handed out last time need no clearing: column_* conversions leave only a zMalloc, which the Mem keeps and
     reuses, never MEM_Dyn or MEM_Agg) */
  Mem *m = K->row + K->nv; char *buf = K->buf; size_t used = K->used, cbuf = K->cbuf;   /* (locals: a Mem's i64 may alias them) */
  for (int k = 0; k < n; k++, m++) {
    const Mem *s = &V[k]; u16 f = s->flags;
    if (f & MEM_Null) m->flags = MEM_Null;
    else if (f & MEM_Int) { m->flags = MEM_Int; m->u.i = s->u.i; }
    else if (f & MEM_IntReal) { m->flags = MEM_Real; m->u.r = (double)s->u.i; }
    else if (f & MEM_Real) { m->flags = MEM_Real; m->u.r = s->u.r; }
    else if (f & (MEM_Str | MEM_Blob)) {
      if (f & MEM_Zero) { qv_why = "zeroblob"; return 0; }
      if ((f & MEM_Str) && s->enc != SQLITE_UTF8) { qv_why = "encoding"; return 0; }
      size_t len = (size_t)s->n;
      if (cbuf - used < len + 1) {   /* (grown: the values already in it moved with it) */
        size_t c = (used + len + 1) * 2 + 256; char *t = sqlite3_realloc64(buf, c); if (!t) { K->used = used; qv_why = "oom"; return 0; }
        if (t != buf) for (Mem *w = K->row; w < m; w++) if (w->flags & (MEM_Str | MEM_Blob)) w->z = t + (w->z - buf);
        K->buf = buf = t; K->cbuf = cbuf = c;
      }
      char *b = buf + used; qc_cpy(b, (const u8 *)s->z, len); b[len] = 0; used += len + 1;
      m->flags = (f & MEM_Str) ? MEM_Str | MEM_Static | MEM_Term : MEM_Blob | MEM_Static; m->z = b; m->n = (int)len; m->enc = SQLITE_UTF8;
    } else { qv_why = "value"; return 0; }
  }
  K->used = used; K->nv = nv; X->nrow++;
  return 1;
}
static QV_INLINE int qv_result(QvX *X, Mem *R, QvCur *Cs, const Op *op) {   /* OP_ResultRow: the row appended to the output */
  if (X->snk) return qv_sink_row(X, X->snk, &R[op->p1], op->p2);
  size_t need = ((size_t)X->nrow + 1) * (size_t)op->p2;
  if (need > X->cout) {
    size_t nc = X->cout ? X->cout * 2 : 256; while (nc < need) nc *= 2;
    if (nc > ((size_t)1 << 22)) { qv_why = "rows"; return 0; }
    QcVal *no = sqlite3_realloc64(X->out, sizeof *no * (u64)nc); if (!no) return 0;
    X->out = no; X->cout = nc;
  }
  QcVal *o = &X->out[(size_t)X->nrow * (size_t)op->p2]; const Mem *V = &R[op->p1];
  for (int k = 0; k < op->p2; k++) if (!qv_val(X, &V[k], &o[k])) return 0;
  X->nrow++;
  return 1;
}
static QV_INLINE int qv_aggop(QvX *X, Mem *R, QvCur *Cs, const Op *op, int pc) {   /* OP_AggStep / AggStep1 / AggInverse */
  int inv = op->opcode == OP_AggInverse || (op->opcode == OP_AggStep1 && op->p1); QvPlan *P = X->P;
  if (P->isCnt[op->p3]) {   /* count(*) / count(x) */
    if (op->p5 == 0 || !qv_isnull(&R[op->p2])) P->cnt[op->p3] += inv ? -1 : 1;
    return 1;
  }
  if (P->isSum[op->p3] && !P->sacc[op->p3].spilled) {   /* sum / total / avg of integers */
    struct QvSum *a = &P->sacc[op->p3]; Mem *x = &R[op->p2];
    if (x->flags & MEM_Null) return 1;
    if (x->flags & MEM_Int) {
      i64 t = a->iSum;
      if (!(inv ? sqlite3SubInt64(&t, x->u.i) : sqlite3AddInt64(&t, x->u.i))) { a->iSum = t; a->cnt += inv ? -1 : 1; return 1; }
    }
    if (!qv_sum_spill(X, op, op->p3)) return 0;   /* SQLite's own path from here on */
  }
  if (P->isSum[op->p3] && !inv) {   /* spilled: SQLite's SumCtx, stepped here exactly as sumStep steps it (a float, or an
                                       integer after one), without the call through a context for every row */
    Mem *acc = &R[op->p3]; Mem *x = &R[op->p2];
    if ((acc->flags & MEM_Agg) && acc->z && acc->u.pDef && acc->u.pDef->xSFunc == sumStep && (x->flags & MEM_Str ? x->enc == SQLITE_UTF8 : 1)) {
      SumCtx *p = (SumCtx *)acc->z;
      int type = sqlite3_value_numeric_type(x);   /* (text: numeric affinity applied in place, as sumStep's call does) */
      if (type == SQLITE_NULL) return 1;
      p->cnt++;
      if (p->approx == 0) {
        if (type != SQLITE_INTEGER) { kahanBabuskaNeumaierInit(p, p->iSum); p->approx = 1; kahanBabuskaNeumaierStep(p, sqlite3_value_double(x)); }
        else {
          i64 t = p->iSum, iv = sqlite3_value_int64(x);
          if (sqlite3AddInt64(&t, iv) == 0) p->iSum = t;
          else { p->ovrfl = 1; kahanBabuskaNeumaierInit(p, p->iSum); p->approx = 1; kahanBabuskaNeumaierStepInt64(p, iv); }
        }
      } else if (type == SQLITE_INTEGER) kahanBabuskaNeumaierStepInt64(p, sqlite3_value_int64(x));
      else { p->ovrfl = 0; kahanBabuskaNeumaierStep(p, sqlite3_value_double(x)); }
      return 1;
    }
  }
  return qv_aggstep(X, op, pc, inv);
}
static QV_INLINE int qv_idxcmp(QvX *X, Mem *R, QvCur *Cs, const Op *op) {   /* OP_IdxLE / IdxGT / IdxLT / IdxGE: 1 jump, 0 not, -1 fallback */
  QvCur *C = &Cs[op->p1]; UnpackedRecord r; int res;
  BtCursor *bc = C->bc; const Mem *k = &R[op->p3];
  if (op->p4.i == 1 && (k->flags & MEM_Int) && bc && bc->eState == CURSOR_VALID && !bc->curIntKey && C->ki && C->ki->aSortFlags[0] == 0) {
    /* one integer key against an ascending first field: read it from the cell (qc_idx_cmp: NULL < numbers < text <
       blob, reals between the integers), as vdbeRecordCompareInt would; equal keys give default_rc */
    const MemPage *pg = bc->pPage; int c;
    if (C->kind == QVC_BT) {   /* a database page, read-only for the run: its cell area once per page (pointer and number: a reused buffer is another page) */
      if (C->cpg != pg || C->cpgno != pg->pgno) { C->cpg = pg; C->cpgno = pg->pgno; C->clo = qc_cell_lo(pg); C->cend = pg->aData + X->usable; }
      const u8 *cell = pg->aData + get2byteAligned(&pg->aCellIdx[2 * bc->ix]);
      i64 v = 0; int kind = cell < C->clo || cell > C->cend - 4 ? -1 : qc_idxkey_at(pg, cell, C->cend, &v);   /* (its own statement: v is read after the call writes it, not in the same argument list) */
      c = qc_idx_cmp_kind(kind, v, k->u.i);
    } else c = qc_idx_cmp(pg, bc->ix, k->u.i);
    if (c != 2) {
      res = c ? c : (op->opcode < OP_IdxLT ? -1 : 0);
      if ((op->opcode & 1) == (OP_IdxLT & 1)) res = -res; else res++;
      return res > 0;
    }
  }
  memset(&r, 0, sizeof r);   /* otherwise vdbe.c's own comparison (sqlite3VdbeIdxKeyCompare) */
  r.pKeyInfo = C->ki; r.nField = (u16)op->p4.i; r.default_rc = op->opcode < OP_IdxLT ? -1 : 0; r.aMem = &R[op->p3];
  if (C->kind == QVC_TOPN) {
    if (C->vbad || C->vi >= C->tn->n) { X->P->topnOff = 1; qv_why = "topn deopt: compare with no row"; return -1; }
    res = sqlite3VdbeFindCompare(&r)((int)C->tn->len[C->vi], C->tn->rec[C->vi], &r);
    if (r.errCode) { qv_why = "idx compare"; return -1; }
    if ((op->opcode & 1) == (OP_IdxLT & 1)) res = -res; else res++;
    return res > 0;
  }
  i64 sz = sqlite3BtreePayloadSize(C->bc); u32 amt;
  if (sz <= 0 || sz > 0x7fffffff) { qv_why = "corrupt index record"; return -1; }
  const u8 *rec = sqlite3BtreePayloadFetch(C->bc, &amt);
  if (amt < sz) {   /* an overflow record, copied */
    u8 *buf = qv_alloc(&X->A, (size_t)sz + 1); if (!buf || sqlite3BtreePayload(C->bc, 0, (u32)sz, buf) != SQLITE_OK) { qv_why = "idx payload"; return -1; }
    rec = buf;
  }
  res = sqlite3VdbeRecordCompareWithSkip((int)sz, rec, &r, 0);
  if (r.errCode) { qv_why = "idx compare"; return -1; }
  if ((op->opcode & 1) == (OP_IdxLT & 1)) res = -res; else res++;
  return res > 0;
}
static QV_INLINE int qv_copy(QvX *X, Mem *R, QvCur *Cs, const Op *op) {   /* OP_Copy */
  for (int k = 0; k <= op->p3; k++) {
    Mem *o = qv_out(X, op->p2 + k), *in = &R[op->p1 + k];
    if (!(in->flags & (MEM_Str | MEM_Blob | MEM_Agg | MEM_Dyn | MEM_Subtype))) { o->flags = in->flags & ~(MEM_Ephem | MEM_Static); o->u = in->u; o->n = in->n; continue; }   /* a number or NULL: nothing to own */
    sqlite3VdbeMemShallowCopy(o, in, MEM_Ephem);
    if ((o->flags & MEM_Ephem) && sqlite3VdbeMemMakeWriteable(o)) return 0;   /* Deephemeralize */
    if ((o->flags & MEM_Subtype) && (op->p5 & 0x0002)) o->flags &= ~MEM_Subtype;
  }
  return 1;
}
static QV_INLINE void qv_scopy(QvX *X, Mem *R, QvCur *Cs, const Op *op) {   /* OP_SCopy */
  Mem *o = qv_out(X, op->p2), *in = &R[op->p1];
  if (!(in->flags & (MEM_Str | MEM_Blob | MEM_Agg | MEM_Dyn | MEM_Subtype))) { o->flags = in->flags & ~(MEM_Ephem | MEM_Static); o->u = in->u; o->n = in->n; return; }
  sqlite3VdbeMemShallowCopy(o, in, MEM_Ephem);
}
static QV_INLINE void qv_null(QvX *X, Mem *R, QvCur *Cs, const Op *op) {   /* OP_Null / BeginSubrtn */
  u16 nf = op->p1 ? (MEM_Null | MEM_Cleared) : MEM_Null;
  for (int k = op->p2; k <= (op->p3 > op->p2 ? op->p3 : op->p2); k++) { Mem *o = qv_out(X, k); o->flags = nf; o->n = 0; }
}
static QV_INLINE int qv_colop(QvX *X, Mem *R, QvCur *Cs, const Op *op) {   /* OP_Column */
  QvCur *C = &Cs[op->p1];
  if (!C->kind) { qv_why = "column cursor"; return 0; }
  Mem *o = qv_out(X, op->p3);
  if (C->kind == QVC_PSEUDO && C->pseudoReg == op->p3) { qv_why = "column into its record"; return 0; }
  return qv_column(X, C, op->p2, op, o);
}

static QV_INLINE int qv_mkrec(QvX *X, Mem *R, QvCur *Cs, const Op *op) {   /* OP_MakeRecord */
  if (op->p4type == P4_STATIC || op->p4type == P4_DYNAMIC) {
    const char *z = op->p4.z; Mem *x = &R[op->p1];
    for (int k = 0; z[k]; k++, x++) {
      applyAffinity(x, z[k], SQLITE_UTF8);
      if (z[k] == SQLITE_AFF_REAL && (x->flags & MEM_Int)) { x->flags |= MEM_IntReal; x->flags &= ~MEM_Int; }
    }
  }
  Mem rec; memset(&rec, 0, sizeof rec);
  if (!qv_make_record(X, &R[op->p1], op->p2, &rec)) return 0;
  Mem *o = qv_out(X, op->p3); qv_static(o, rec.z, rec.n, 0, 0);
  return 1;
}
/* vdbesort.c's limits: its records stay in memory while they fit in mxPmaSize (the cache size in bytes, at least szPma
   pages); past that it writes sorted runs to disk and merges them. That merge is stable: each run is sorted stably,
   and of equal keys it takes the older run's first (vdbeMergeEngineStep), so the order is one in-memory sort's, ties
   included. qvm sorts in memory past mxPmaSize, up to QV_SORT_MEM bytes of records (counted as vdbesort counts them;
   at least mxPmaSize), then hands the records so far, in insertion order, to SQLite's own sorter (qv_sort_spill) and
   goes on with it, which spills and merges as the interpreter's does: the same order either way. wakapi
   settings.user_agents' GROUP BY of four text keys went to SQLite's sorter at the 2 MB default cache and is 0.44x the
   instructions sorted (and grouped by hashing) in memory. No limit with an in-memory temp store, as vdbesort.c. */
#ifndef QV_SORT_MEM
#define QV_SORT_MEM ((i64)64 << 20)
#endif
static void qv_sort_limits(QvX *X, QvSort *S) {
  sqlite3 *db = X->db;
  if (sqlite3TempInMemory(db)) { S->lim = S->mn = 0; return; }
  i64 pgsz = sqlite3BtreeGetPageSize(db->aDb[0].pBt), mx = db->aDb[0].pSchema->cache_size;
  mx = mx < 0 ? mx * -1024 : mx * pgsz;
  if (mx > SQLITE_MAX_PMASZ) mx = SQLITE_MAX_PMASZ;
  S->mn = (i64)sqlite3GlobalConfig.szPma * pgsz;
  S->lim = mx > S->mn ? mx : S->mn;
  if (S->lim < QV_SORT_MEM) S->lim = QV_SORT_MEM;
}
#ifndef QV_SPILL_COPY_MAX
#define QV_SPILL_COPY_MAX ((i64)256 << 20)   /* bytes of a spilled sorter's rows qvm copies in one run (SorterData) before handing the statement back */
#endif
static void qv_sort_unspill(sqlite3 *db, QvSort *S) { sqlite3VdbeSorterClose(db, S->vc); sqlite3_free(S->vc); S->vc = 0; }
static QV_NOINLINE int qv_sort_spill(QvX *X, QvSort *S) {   /* SQLite's sorter (OP_SorterOpen's), given the records so far */
#if SQLITE_MAX_WORKER_THREADS > 0
  /* with worker threads (PRAGMA threads), each sorted run goes to whichever thread is idle, so how equal keys merge
     depends on timing, even between two interpreter runs: handed back, as before (the default is no threads) */
  if (!sqlite3TempInMemory(X->db) && sqlite3GlobalConfig.bCoreMutex && X->db->aLimit[SQLITE_LIMIT_WORKER_THREADS] > 0) { qv_why = "sorter would spill (threads)"; return 0; }
#endif
  VdbeCursor *vc = sqlite3MallocZero(sizeof *vc); if (!vc) { qv_why = "oom"; return 0; }
  vc->eCurType = CURTYPE_SORTER; vc->pKeyInfo = S->ki; S->vc = vc;
  if (sqlite3VdbeSorterInit(X->db, S->nf, vc) != SQLITE_OK) { qv_why = "sorter init"; return 0; }   /* (closed with the cursor) */
  for (int i = 0; i < S->n; i++) {
    Mem m; sqlite3VdbeMemInit(&m, X->db, MEM_Blob | MEM_Ephem); m.z = (char *)S->rec[i]; m.n = (int)S->len[i];
    if (sqlite3VdbeSorterWrite(vc, &m) != SQLITE_OK) { qv_why = "sorter write"; return 0; }
  }
  S->n = S->pos = 0; S->mem = 0;
  return 1;
}
static QV_INLINE int qv_sorterins(QvX *X, Mem *R, QvCur *Cs, const Op *op) {   /* OP_SorterInsert */
  QvCur *C = &Cs[op->p1]; QvSort *S = C->srt; Mem *x = &R[op->p2];
  if (!(x->flags & MEM_Blob) || (x->flags & MEM_Zero)) { qv_why = "sorter record"; return 0; }
  if (S->vc) { if (sqlite3VdbeSorterWrite(S->vc, x) != SQLITE_OK) { qv_why = "sorter write"; return 0; } return 1; }
  const u8 *rec = (const u8 *)x->z;
  if (!(x->flags & MEM_Static)) { u8 *c = qv_alloc(&X->A, (size_t)x->n + 1); if (!c) { qv_why = "arena"; return 0; } memcpy(c, x->z, (size_t)x->n); rec = c; }
  if (S->n == S->cap) {
    int nc = S->cap ? S->cap * 2 : 64;
    const u8 **nr = qv_alloc(&X->A, sizeof *nr * (size_t)nc); u32 *nl = qv_alloc(&X->A, sizeof *nl * (size_t)nc);
    QcVal *nk = qv_alloc(&X->A, sizeof *nk * (size_t)nc * (size_t)S->nk);
    if (!nr || !nl || !nk) { qv_why = "arena"; return 0; }
    if (S->n) { memcpy(nr, S->rec, sizeof *nr * (size_t)S->n); memcpy(nl, S->len, sizeof *nl * (size_t)S->n); memcpy(nk, S->key, sizeof *nk * (size_t)S->n * (size_t)S->nk); }
    S->rec = nr; S->len = nl; S->key = nk; S->cap = nc;
  }
  if (S->lim) {   /* (vdbesort's own count of the memory) */
    S->mem += ((i64)x->n + (i64)sizeof(SorterRecord) + 7) & ~(i64)7;
    if (S->mem > S->lim || (S->mem > S->mn && sqlite3HeapNearlyFull())) {
      if (!qv_sort_spill(X, S) || sqlite3VdbeSorterWrite(S->vc, x) != SQLITE_OK) { if (!*qv_why) qv_why = "sorter write"; return 0; }
      return 1;
    }
  }
  if (!qv_decode(rec, (u32)x->n, S->nk, &S->key[(size_t)S->n * S->nk])) { qv_why = "sorter key"; return 0; }
  S->rec[S->n] = rec; S->len[S->n] = (u32)x->n; S->n++;
  return 1;
}

static QV_INLINE void qv_cmpvec(QvX *X, Mem *R, QvCur *Cs, const Op *op) {   /* OP_Compare */
  const u32 *perm = (op->p5 & OPFLAG_PERMUTE) ? X->aPermute : 0; const KeyInfo *ki = op->p4.pKeyInfo;
  int done = 0;
  for (int i = 0; i < op->p3 && !done; i++) {
    u32 idx = perm ? perm[i] : (u32)i; Mem *a = &R[op->p1 + idx], *b = &R[op->p2 + idx];
    if ((a->flags & b->flags & MEM_Int) && !((a->flags | b->flags) & MEM_Null)) X->iCompare = a->u.i < b->u.i ? -1 : a->u.i > b->u.i;   /* sqlite3MemCompare's integer case */
    else X->iCompare = sqlite3MemCompare(a, b, ki->aColl[i]);
    if (X->iCompare) {
      if ((ki->aSortFlags[i] & KEYINFO_ORDER_BIGNULL) && ((R[op->p1 + idx].flags & MEM_Null) || (R[op->p2 + idx].flags & MEM_Null))) X->iCompare = -X->iCompare;
      if (ki->aSortFlags[i] & KEYINFO_ORDER_DESC) X->iCompare = -X->iCompare;
      done = 1;
    }
  }
}
static QV_INLINE int qv_move(QvX *X, Mem *R, QvCur *Cs, const Op *op) {   /* OP_Move */
  for (int k = 0; k < op->p3; k++) {
    Mem *o = qv_out(X, op->p2 + k);
    if (X->P->isCnt[op->p1 + k]) { qv_why = "move of a counter"; return 0; }
    sqlite3VdbeMemMove(o, &R[op->p1 + k]);
    if ((o->flags & MEM_Ephem) && sqlite3VdbeMemMakeWriteable(o)) return 0;
  }
  return 1;
}
static QV_INLINE void qv_bitop(QvX *X, Mem *R, QvCur *Cs, const Op *op) {   /* OP_BitAnd / BitOr / ShiftLeft / ShiftRight */
  Mem *pIn1 = &R[op->p1], *pIn2 = &R[op->p2];
  if ((pIn1->flags | pIn2->flags) & MEM_Null) { qv_out(X, op->p3)->flags = MEM_Null; return; }
  i64 iA = pIn2->flags == MEM_Int ? pIn2->u.i : sqlite3VdbeIntValue(pIn2), iB = pIn1->flags == MEM_Int ? pIn1->u.i : sqlite3VdbeIntValue(pIn1); u64 uA; u8 o = op->opcode;
  if (o == OP_BitAnd) iA &= iB;
  else if (o == OP_BitOr) iA |= iB;
  else if (iB != 0) {
    if (iB < 0) { o = 2 * OP_ShiftLeft + 1 - o; iB = iB > (-64) ? -iB : 64; }
    if (iB >= 64) iA = (iA >= 0 || o == OP_ShiftLeft) ? 0 : -1;
    else {
      memcpy(&uA, &iA, sizeof uA);
      if (o == OP_ShiftLeft) uA <<= iB; else { uA >>= iB; if (iA < 0) uA |= ((((u64)0xffffffff) << 32) | 0xffffffff) << (64 - iB); }
      memcpy(&iA, &uA, sizeof iA);
    }
  }
  qv_int(qv_out(X, op->p3), iA);
}

#ifdef QV_TRACE
static volatile int qv_trace_pc = -1, qv_trace_op = -1;   /* for a crash handler: where qvm was */
#define QV_TRACE_AT(pc, op) (qv_trace_pc = (pc), qv_trace_op = (op))
#else
#define QV_TRACE_AT(pc, op) ((void)0)
#endif
/* WebAssembly: qv_run returns every QV_REENTER_OPS jumps and is called again from the next opcode (the opcodes keep
   no state but pc). V8 swaps a function for its optimised (TurboFan) code only on its next call, with no on-stack
   replacement, so a qv_run that never returned would run a long execution in the baseline compiler's code (Turso's
   Doom: 21 against 33 frames/s). The feedback loop hands over to the plain one at the first return. Natively there is
   nothing to gain: about 1 % on Doom, and the variable start costs other queries more (QV_FB_LEAVE is empty). */
#if defined(__wasm__) && !defined(QV_REENTER_OPS)
#define QV_REENTER_OPS (1 << 20)
#endif
#ifdef QV_REENTER_OPS
#define QV_FB_LEAVE do { if (--X->fbLeft < 0) { X->pc0 = pc; return 2; } } while (0)
#else
#define QV_FB_LEAVE do { } while (0)
#endif
/* dispatch: threaded (each opcode jumps straight to the next one's code) where the compiler has labels as values,
   otherwise the switch */
#if defined(__GNUC__) && !defined(QV_NO_THREADED)
#define QL(x) L_##x:
#define QDISP do { op = &aOp[pc]; QV_TRACE_AT(pc, op->opcode); goto *qv_disp[op->opcode]; } while (0)
#define QJUMP do { QV_FB_LEAVE; QDISP; } while (0)   /* a jump (every loop has one): where the hand-over is counted */
#define QNEXT do { pc++; QDISP; } while (0)
#define QV_UNUSED_LABEL __attribute__((unused))
#else
#define QL(x)
#define QJUMP goto qv_jump
#define QNEXT goto qv_next
#define QV_UNUSED_LABEL
#endif
/* ---------------------------------------------------------------- regions: single-cursor loops, specialised
** A region is the body of a loop [B, N] whose N is a Next/Prev on one cursor, built once the plan has run a few
** times and has type feedback. Each opcode becomes a node: its handler (a label in qv_region, chosen for the opcode,
** its operands and the feedback) and its operands resolved to pointers. The handlers are the opcode bodies above, or
** in-line fast paths that check their preconditions and otherwise take that body: a region writes exactly what
** qv_run would, so leaving it is free at any opcode. The loop's own Next fetches the next row and parses its header
** through the last field the body reads (one decode per row instead of one per Column), and a Column the feedback
** says is always an integer reads its field directly; when that guess fails the node becomes the generic Column.
** An opcode without a node leaves the region (qv_run runs it and re-enters at the next Next); a region that leaves
** on more than a quarter of its rows turns itself off. */
enum { RG_EXIT, RG_NOP, RG_GOTO, RG_COL, RG_COLI, RG_ROWID, RG_IDXROWID, RG_CMP, RG_CMPI, RG_CMPT, RG_REALAFF, RG_COLLSEQ, RG_ONCE, RG_IF, RG_IFNOT, RG_ISNULL,
       RG_NOTNULL, RG_INT, RG_INT64, RG_STR, RG_NULL, RG_SCOPY, RG_COPY, RG_ARITH, RG_FUNC, RG_AGG, RG_RESULT, RG_IFPOS,
       RG_IFNZ, RG_DJZ, RG_IDXCMP, RG_MKREC, RG_SORTINS, RG_COMPARE, RG_JUMPCMP, RG_MOVE, RG_BITOP, RG_PERM, RG_GOSUB, RG_NEXT,
       RG_RESULTS, RG_N };   /* (RG_RESULTS: a streamed run's ResultRow, swapped in for RG_RESULT while it streams: qv_strm_nodes) */
typedef struct QvNode {
  const void *h; const Op *op; int pc, jn;   /* handler; the opcode; jn: the jump target's node, or -1 (leave at P2) */
  QvCur *C; Mem *a, *b, *o;                  /* cursor P1; inputs; the output register */
  u8 plain, ar, lt, eq, gt;                  /* plain: the output is no counter or sum; ar: + - * in line; the comparison's jumps */
  int jx[3];                                 /* OP_Jump: its three targets' nodes, or -1 */
} QvNode;
typedef struct QvRegion {
  int B, N, cur, maxf, maxfAll;              /* N: the Next/Prev of cursor cur; maxf: the last field its Columns on cur read before a branch to N; maxfAll: at all */
  QvNode *n;
  u64 iters, exits, misses; u8 off;
} QvRegion;
/* a region's slow path for Column: out of line, so the region's code stays small (qv_run has its own copy in line) */
static QV_NOINLINE int qv_colop_cold(QvX *X, Mem *R, QvCur *Cs, const Op *op) { return qv_colop(X, R, Cs, op); }
static const void *const *qv_rgl;            /* qv_region's handlers by kind (filled by qc_init's call without a region) */
static inline Mem *qv_rel(Mem *m) { if (VdbeMemDynamic(m)) sqlite3VdbeMemSetNull(m); return m; }
#if defined(__GNUC__) && !defined(QV_NO_THREADED)
static int qv_region(QvX *X, QvRegion *G, int *pcOut) {
  static const void *const L[RG_N] = {
    [RG_EXIT] = &&rg_exit, [RG_NOP] = &&rg_nop, [RG_GOTO] = &&rg_goto, [RG_COL] = &&rg_col, [RG_COLI] = &&rg_coli,
    [RG_ROWID] = &&rg_rowid, [RG_IDXROWID] = &&rg_idxrowid, [RG_CMP] = &&rg_cmp, [RG_CMPI] = &&rg_cmpi, [RG_CMPT] = &&rg_cmpt, [RG_REALAFF] = &&rg_realaff, [RG_COLLSEQ] = &&rg_collseq, [RG_ONCE] = &&rg_once, [RG_IF] = &&rg_if,
    [RG_IFNOT] = &&rg_ifnot, [RG_ISNULL] = &&rg_isnull, [RG_NOTNULL] = &&rg_notnull, [RG_INT] = &&rg_int, [RG_INT64] = &&rg_int64,
    [RG_STR] = &&rg_str, [RG_NULL] = &&rg_null, [RG_SCOPY] = &&rg_scopy, [RG_COPY] = &&rg_copy, [RG_ARITH] = &&rg_arith,
    [RG_FUNC] = &&rg_func, [RG_AGG] = &&rg_agg, [RG_RESULT] = &&rg_result, [RG_IFPOS] = &&rg_ifpos, [RG_IFNZ] = &&rg_ifnz,
    [RG_DJZ] = &&rg_djz, [RG_IDXCMP] = &&rg_idxcmp, [RG_MKREC] = &&rg_mkrec, [RG_SORTINS] = &&rg_sortins,
    [RG_COMPARE] = &&rg_compare, [RG_JUMPCMP] = &&rg_jumpcmp, [RG_MOVE] = &&rg_move, [RG_BITOP] = &&rg_bitop, [RG_PERM] = &&rg_perm, [RG_GOSUB] = &&rg_gosub, [RG_NEXT] = &&rg_next, [RG_RESULTS] = &&rg_results };
  if (!G) { qv_rgl = L; return 1; }
  QvNode *n = &G->n[*pcOut - G->B]; Mem *R = X->r; QvCur *Cs = X->c;
#define RG_GO(i) do { n = &G->n[i]; QV_TRACE_AT(n->pc, n->op->opcode); goto *n->h; } while (0)
#define RG_NEXT do { n++; QV_TRACE_AT(n->pc, n->op->opcode); goto *n->h; } while (0)
#define RG_JUMP do { if (n->jn >= 0) RG_GO(n->jn); *pcOut = n->op->p2; return 1; } while (0)   /* (out of the body: the loop ends) */
#define RG_OUT(r) (n->plain ? qv_rel(n->o) : qv_out(X, (r)))
  /* the region cursor's header parsed through the body's last field, the first time a Column needs one not yet parsed */
#define RG_HDR(C, p2) do { if ((p2) >= (C)->nHdr && n->op->p1 == G->cur && (C)->rowOk && !(C)->nullRow && !(C)->deferred && (C)->kind == QVC_BT) { \
    int f_ = G->maxfAll < (C)->nField ? G->maxfAll : (C)->nField - 1; if (f_ < (p2)) f_ = (p2); if (!qv_hdr((C), f_)) return 0; } } while (0)
  QV_TRACE_AT(n->pc, n->op->opcode);
  goto *n->h;
rg_exit:   /* an opcode without a node: qv_run's, and back at the next Next */
  *pcOut = n->pc;
  if (++G->exits > 64 && G->exits > G->iters / 4) G->off = 1;   /* it leaves too often: qv_run's from now on */
  return 1;
rg_nop: RG_NEXT;
rg_goto: RG_JUMP;
rg_col: {
    QvCur *C = n->C; int p2 = n->op->p2;
    RG_HDR(C, p2);
    if (C->rowOk && !C->nullRow && !C->deferred && C->kind == QVC_BT && p2 < C->nHdr) {   /* the row's header is parsed: qv_column's last step */
      Mem *o = RG_OUT(n->op->p3); u32 t = C->aType[p2];
      if (!qv_colval(X, C, t, C->row + C->aOff[p2], o)) return 0;
      RG_NEXT;
    }
    if (!qv_colop_cold(X, R, Cs, n->op)) return 0;
    RG_NEXT;
  }
rg_coli: {   /* speculated: a number or NULL (serial types 0-9; integers tested first) */
    QvCur *C = n->C; int p2 = n->op->p2;
    RG_HDR(C, p2);
    if (C->rowOk && !C->nullRow && !C->deferred && C->kind == QVC_BT && p2 < C->nHdr) {
      u32 t = C->aType[p2];
      if (t - 1 < 6) { qv_int(qv_rel(n->o), qc_int(t, C->row + C->aOff[p2])); RG_NEXT; }
      if (t - 8 < 2) { qv_int(qv_rel(n->o), (i64)t - 8); RG_NEXT; }
      if (t == 7) { double r = qv_real(C->row + C->aOff[p2]); Mem *o = qv_rel(n->o); if (r != r) o->flags = MEM_Null; else { o->flags = MEM_Real; o->u.r = r; } RG_NEXT; }   /* (NaN reads as NULL) */
      if (t == 0) { qv_rel(n->o)->flags = MEM_Null; RG_NEXT; }
      n->h = L[RG_COL]; G->misses++; if (QC_ST) QC_INC(qcs.widen);   /* the guess failed: a generic Column from now on */
    }
    goto rg_col;
  }
rg_rowid: {
    QvCur *C = n->C;
    if (C->kind != QVC_BT || C->nullRow || C->deferred || sqlite3BtreeCursorHasMoved(C->bc)) goto rg_exit;
    qv_int(RG_OUT(n->op->p2), sqlite3BtreeIntegerKey(C->bc));
    RG_NEXT;
  }
rg_idxrowid: {
    QvCur *C = n->C; i64 rowid = 0;
    if (C->kind != QVC_BT || C->nullRow || sqlite3BtreeCursorHasMoved(C->bc)) goto rg_exit;
    if (sqlite3VdbeIdxRowid(X->db, C->bc, &rowid) != SQLITE_OK) { qv_why = "idxrowid"; return 0; }
    qv_int(RG_OUT(n->op->p2), rowid);
    RG_NEXT;
  }
rg_cmp: if (qv_compare_op(X, n->op)) RG_JUMP; RG_NEXT;
rg_cmpi: {   /* speculated: two integers (qv_compare_op's first case) */
    Mem *a = n->a, *b = n->b;
    if (a->flags & b->flags & MEM_Int) {
      int res = b->u.i > a->u.i ? 1 : b->u.i < a->u.i ? -1 : 0; X->iCompare = res;
      if (res < 0 ? n->lt : res ? n->gt : n->eq) RG_JUMP;
      RG_NEXT;
    }
    n->h = L[RG_CMP]; G->misses++; if (QC_ST) QC_INC(qcs.widen);
    goto rg_cmp;
  }
rg_cmpt: {   /* speculated: two plain UTF-8 strings, the op's collation BINARY or none and its affinity TEXT or below (checked when
                the node was made): qv_compare_str's memcmp, without its call and its checks of the op (D1) */
    const Mem *a = n->a, *b = n->b;
    if (QV_TXT(a->flags) && QV_TXT(b->flags) && a->enc == SQLITE_UTF8 && b->enc == SQLITE_UTF8) {
      int k = b->n < a->n ? b->n : a->n, res = memcmp(b->z, a->z, (size_t)k); if (!res) res = b->n - a->n;
      X->iCompare = res;
      if (res < 0 ? n->lt : res ? n->gt : n->eq) RG_JUMP;
      RG_NEXT;
    }
    if (((a->flags | b->flags) & MEM_Null) && !(n->op->p5 & SQLITE_NULLEQ)) {   /* a NULL (feedback of text or NULL): qv_compare_slow's
                                                                                   result without its call (a NULL bound to a scan's filter) */
      X->iCompare = 1;
      if (n->op->p5 & SQLITE_JUMPIFNULL) RG_JUMP;
      RG_NEXT;
    }
    n->h = L[RG_CMP]; G->misses++; if (QC_ST) QC_INC(qcs.widen);
    goto rg_cmp;
  }
rg_realaff: if (n->a->flags & (MEM_Int | MEM_IntReal)) sqlite3VdbeMemRealify(n->a); RG_NEXT;   /* (qv_run's: a REAL column's integer, a float) */
rg_collseq: if (n->op->p1) qv_int(RG_OUT(n->op->p1), 0); RG_NEXT;   /* (qv_run's: min()'s and max()'s flag, set by their AggStep) */
rg_once: if (X->once[n->pc >> 3] & (1 << (n->pc & 7))) RG_JUMP; X->once[n->pc >> 3] |= (u8)(1 << (n->pc & 7)); RG_NEXT;   /* (qv_run's) */
rg_if: if (sqlite3VdbeBooleanValue(n->a, n->op->p3)) RG_JUMP; RG_NEXT;
rg_ifnot: if (!sqlite3VdbeBooleanValue(n->a, !n->op->p3)) RG_JUMP; RG_NEXT;
rg_isnull: if (qv_isnull(n->a)) RG_JUMP; RG_NEXT;
rg_notnull: if (!qv_isnull(n->a)) RG_JUMP; RG_NEXT;
rg_int: qv_int(RG_OUT(n->op->p2), n->op->p1); RG_NEXT;
rg_int64: qv_int(RG_OUT(n->op->p2), INT32_TO_64(n->op->p1, n->op->p3)); RG_NEXT;
rg_str: qv_str(X, R, n->op, RG_OUT(n->op->p2)); RG_NEXT;
rg_null: qv_null(X, R, Cs, n->op); RG_NEXT;
rg_scopy: qv_scopy(X, R, Cs, n->op); RG_NEXT;
rg_copy: if (!qv_copy(X, R, Cs, n->op)) return 0; RG_NEXT;
rg_arith: {
    Mem *a = n->a, *b = n->b;   /* P1, P2: P3 = P2 op P1 */
    if (n->ar && n->plain && (a->flags & b->flags & MEM_Int)) {
      i64 iB = b->u.i; int ovf = n->ar == 1 ? QV_ADD64(&iB, a->u.i) : n->ar == 2 ? QV_SUB64(&iB, a->u.i) : QV_MUL64(&iB, a->u.i);
      if (!ovf) { qv_int(qv_rel(n->o), iB); RG_NEXT; }
    }
    if (!qv_arith_fast(X, n->op)) qv_arith(X, n->op);
    RG_NEXT;
  }
rg_func: if (!qv_function(X, n->op, n->pc)) return 0; RG_NEXT;
rg_agg: if (!qv_aggop(X, R, Cs, n->op, n->pc)) return 0; RG_NEXT;
rg_result: if (!qv_result(X, R, Cs, n->op)) return 0; RG_NEXT;
rg_results:   /* streamed: the row handed out, the region entered again at the next node (qv_stream_resume) */
  if (!qv_strm_term(&R[n->op->p1], n->op->p2)) return 0;
  *pcOut = X->pc0 = n->pc + 1; X->res = &R[n->op->p1]; X->rgRes = G; X->nrow++; return 3;
rg_ifpos: { Mem *a = n->a; if (a->u.i > 0) { a->u.i -= n->op->p3; RG_JUMP; } RG_NEXT; }
rg_ifnz: { Mem *a = n->a; if (a->u.i) { if (a->u.i > 0) a->u.i--; RG_JUMP; } RG_NEXT; }
rg_djz: { Mem *a = n->a; if (a->u.i > SMALLEST_INT64) a->u.i--; if (a->u.i == 0) RG_JUMP; RG_NEXT; }
rg_idxcmp: { int r = qv_idxcmp(X, R, Cs, n->op); if (r < 0) return 0; if (r) RG_JUMP; RG_NEXT; }
rg_mkrec: if (!qv_mkrec(X, R, Cs, n->op)) return 0; RG_NEXT;
rg_sortins: if (!qv_sorterins(X, R, Cs, n->op)) return 0; RG_NEXT;
rg_compare: qv_cmpvec(X, R, Cs, n->op); RG_NEXT;
rg_jumpcmp: {   /* OP_Jump: P1 / P2 / P3 by the last Compare */
    int t = X->iCompare < 0 ? 0 : X->iCompare == 0 ? 1 : 2;
    if (n->jx[t] >= 0) RG_GO(n->jx[t]);
    *pcOut = t == 0 ? n->op->p1 : t == 1 ? n->op->p2 : n->op->p3;
    return 1;
  }
rg_move: if (!qv_move(X, R, Cs, n->op)) return 0; RG_NEXT;
rg_bitop: qv_bitop(X, R, Cs, n->op); RG_NEXT;
rg_perm: X->aPermute = (const u32 *)n->op->p4.ai + 1; RG_NEXT;
rg_gosub: qv_int(qv_out(X, n->op->p1), n->pc); RG_JUMP;   /* a subroutine (a group's output): qv_run's, back at the next Next */
rg_next: {   /* the loop's Next / Prev (qv_run's B-tree case), then the next row fetched and decoded */
    QvCur *C = n->C; const Op *op = n->op; int rc;
    G->iters++;
    if (C->kind != QVC_BT && C->kind != QVC_EPH) goto rg_exit;
    rc = op->opcode == OP_Next ? sqlite3BtreeNext(C->bc, op->p3) : sqlite3BtreePrevious(C->bc, op->p3);
    qv_stale(C);
    if (rc == SQLITE_OK) {
      C->nullRow = 0;
      if (C->bc->ix == 0 && !qv_tick_check(X)) return 0;   /* (a region walks a finite B-tree: checked per page it enters, not per row) */
      if (G->maxfAll >= 0 && C->kind == QVC_BT) {   /* the row loaded for the body's Columns; its header parsed through those before a branch */
        if (!qv_row(X, C)) return 0;
        int f = G->maxf < C->nField ? G->maxf : C->nField - 1;
        if (f >= 0 && !C->nullRow && C->nHdr <= f && !qv_hdr(C, f)) return 0;
      }
      RG_GO(0);
    }
    if (rc != SQLITE_DONE) { qv_why = "next"; return 0; }
    C->nullRow = 1; *pcOut = n->pc + 1;
    return 1;
  }
#undef RG_GO
#undef RG_NEXT
#undef RG_JUMP
#undef RG_OUT
#undef RG_HDR
}
#else
static int qv_region(QvX *X, QvRegion *G, int *pcOut) { (void)X; (void)G; (void)pcOut; return 1; }   /* (regions need computed goto) */
#endif
/* the plan's regions, from its loops and its feedback so far */
static void qv_rg_build(QvPlan *P, Vdbe *v) {
  if (!qv_rgl) qv_region(0, 0, 0);
  if (!qv_rgl) return;
  Op *aOp = v->aOp; int nOp = P->nOp;
  int *at = sqlite3MallocZero(sizeof(int) * (u64)nOp); QvRegion *rg = sqlite3MallocZero(sizeof *rg * 64); int nrg = 0;
  if (!at || !rg) { sqlite3_free(at); sqlite3_free(rg); return; }
  for (int N = 1; N < nOp && nrg < 64; N++) {
    Op *o = &aOp[N]; int B = o->p2, ok = 1, cur = o->p1;
    if ((o->opcode != OP_Next && o->opcode != OP_Prev) || B <= 0 || B > N) continue;
    for (int pc = B; pc < N && ok; pc++) {
      Op *x = &aOp[pc];
      switch (x->opcode) {   /* inner loops, seeks into other tables, coroutines: not a single-cursor loop */
        case OP_Next: case OP_Prev: case OP_SorterNext: case OP_Rewind: case OP_Last: case OP_Sort: case OP_SorterSort:
        case OP_SeekGE: case OP_SeekGT: case OP_SeekLE: case OP_SeekLT: case OP_SeekRowid: case OP_NotExists:
        case OP_DeferredSeek: case OP_Yield: case OP_InitCoroutine: case OP_EndCoroutine: case OP_Return:
          ok = 0; break;
      }
      if ((sqlite3OpcodeProperty[x->opcode] & OPFLG_JUMP) && x->p2 >= B && x->p2 < pc) ok = 0;   /* a backward jump: another loop */
      if (x->opcode == OP_Jump && ((x->p1 >= B && x->p1 < pc) || (x->p3 >= B && x->p3 < pc))) ok = 0;
    }
    if (!ok) continue;
    QvNode *nd = sqlite3MallocZero(sizeof *nd * (u64)(N - B + 1)); if (!nd) break;
    QvRegion *G = &rg[nrg]; G->B = B; G->N = N; G->cur = cur; G->maxf = G->maxfAll = -1; G->n = nd; int pre = 1;
    for (int pc = B; pc <= N; pc++) {
      Op *x = &aOp[pc]; QvNode *d = &nd[pc - B]; int k = RG_EXIT, out = -1;
      if (pc > B && (sqlite3OpcodeProperty[aOp[pc - 1].opcode] & OPFLG_JUMP) && aOp[pc - 1].p2 == N) pre = 0;   /* (past the body's first branch that skips the row: an OFFSET, a WHERE term) */
      d->op = x; d->pc = pc; d->jn = (x->p2 >= B && x->p2 <= N) ? x->p2 - B : -1;
      if (x->p1 >= 0 && x->p1 < P->nCur) d->C = &P->c[x->p1];
      switch (x->opcode) {
        case OP_Noop: case OP_Explain: case OP_CursorHint: case OP_ColumnsUsed: k = RG_NOP; break;   /* (Filter, FilterAdd: qv_run's) */
        case OP_Once: k = RG_ONCE; break;   /* (a 'now' function's value, factored behind a Once in the loop: without it each row left the region, n8n find_soft_deleted's turned itself off) */
        case OP_CollSeq: k = RG_COLLSEQ; out = x->p1 ? x->p1 : -1; break;   /* (before min() and max(): without it a grouped scan's rows each left the region, atuin store.status's turned itself off) */
        case OP_RealAffinity: k = RG_REALAFF; d->a = &P->r[x->p1]; break;   /* (a REAL sort key: without it each row left the region, calibre annotations.all_for_user's turned itself off) */
        case OP_Goto: k = RG_GOTO; break;
        case OP_Column:
          out = x->p3;
          k = (x->p1 == cur && qv_fb_num(P->fb[pc]) && !P->isCnt[out] && !P->isSum[out]) ? RG_COLI : RG_COL;
          if (pre && x->p1 == cur && x->p2 > G->maxf) G->maxf = x->p2;   /* decoded with each row: the fields read before a branch that skips it (a row an OFFSET or a filter skips doesn't pay for the rest) */
          if (x->p1 == cur && x->p2 > G->maxfAll) G->maxfAll = x->p2;   /* the rest at once, when a row that passes first needs them */
          break;
        case OP_Rowid: k = RG_ROWID; out = x->p2; break;
        case OP_IdxRowid: k = RG_IDXROWID; out = x->p2; break;
        case OP_Eq: case OP_Ne: case OP_Lt: case OP_Le: case OP_Gt: case OP_Ge:
          k = P->fb[pc] == QV_FB_INT ? RG_CMPI : RG_CMP;
          if (P->fb[pc] && !(P->fb[pc] & ~(QV_FB_NULL | QV_FB_TEXT)) && (x->p5 & SQLITE_AFF_MASK) <= SQLITE_AFF_TEXT
              && (x->p4type != P4_COLLSEQ || (sqlite3IsBinary(x->p4.pColl) && x->p4.pColl->enc == SQLITE_UTF8))) k = RG_CMPT;   /* (text or NULL, BINARY) */
          d->a = &P->r[x->p1]; d->b = &P->r[x->p3];
          d->lt = qv_LT[x->opcode]; d->eq = qv_EQ[x->opcode]; d->gt = qv_GT[x->opcode];
          break;
        case OP_If: k = RG_IF; d->a = &P->r[x->p1]; break;
        case OP_IfNot: k = RG_IFNOT; d->a = &P->r[x->p1]; break;
        case OP_IsNull: k = RG_ISNULL; d->a = &P->r[x->p1]; break;
        case OP_NotNull: k = RG_NOTNULL; d->a = &P->r[x->p1]; break;
        case OP_IfPos: k = RG_IFPOS; d->a = &P->r[x->p1]; break;
        case OP_IfNotZero: k = RG_IFNZ; d->a = &P->r[x->p1]; break;
        case OP_DecrJumpZero: k = RG_DJZ; d->a = &P->r[x->p1]; break;
        case OP_Integer: k = RG_INT; out = x->p2; break;
        case OP_Int64: k = RG_INT64; out = x->p2; break;
        case OP_String: k = RG_STR; out = x->p2; break;
        case OP_Null: case OP_BeginSubrtn: k = RG_NULL; break;
        case OP_SCopy: k = RG_SCOPY; break;
        case OP_Copy: k = RG_COPY; break;
        case OP_Add: case OP_Subtract: case OP_Multiply: case OP_Divide: case OP_Remainder:
          k = RG_ARITH; out = x->p3; d->a = &P->r[x->p1]; d->b = &P->r[x->p2];
          d->ar = x->opcode == OP_Add ? 1 : x->opcode == OP_Subtract ? 2 : x->opcode == OP_Multiply ? 3 : 0;
          break;
        case OP_Function: case OP_PureFunc: k = RG_FUNC; break;
        case OP_AggStep: case OP_AggStep1: case OP_AggInverse: k = RG_AGG; break;
        case OP_ResultRow: k = RG_RESULT; break;
        case OP_IdxLE: case OP_IdxGT: case OP_IdxLT: case OP_IdxGE: k = RG_IDXCMP; break;
        case OP_MakeRecord: k = RG_MKREC; break;
        case OP_SorterInsert: k = RG_SORTINS; break;
        case OP_Compare: k = RG_COMPARE; break;
        case OP_Jump:
          k = RG_JUMPCMP;
          for (int t = 0; t < 3; t++) { int to = t == 0 ? x->p1 : t == 1 ? x->p2 : x->p3; d->jx[t] = (to >= B && to <= N) ? to - B : -1; }
          break;
        case OP_Move: k = RG_MOVE; break;
        case OP_Permutation: k = RG_PERM; break;
        case OP_Gosub: k = RG_GOSUB; break;
        case OP_BitAnd: case OP_BitOr: case OP_ShiftLeft: case OP_ShiftRight: k = RG_BITOP; break;
      }
      if (pc == N) k = RG_NEXT;
      if (out >= 0) { d->o = &P->r[out]; d->plain = !P->isCnt[out] && !P->isSum[out]; }
      d->h = qv_rgl[k];
    }
    at[N] = ++nrg;
  }
  P->rg = rg; P->nrg = nrg; P->rgAt = at;
}
/* the feedback and regions of a qvm statement, for the corpus (QFB=1) */
static void qv_fb_dump(Vdbe *v, FILE *f) {
  QvPlan *P = QCV(v)->pQc; if (!P || P->kind != 3) return;
  static const char *K[RG_N] = {"exit", "nop", "goto", "col", "col:int", "rowid", "idxrowid", "cmp", "cmp:int", "cmp:text", "realaff", "collseq", "once", "if", "ifnot", "isnull",
    "notnull", "int", "int64", "str", "null", "scopy", "copy", "arith", "func", "agg", "result", "ifpos", "ifnz", "djz", "idxcmp", "mkrec",
    "sortins", "compare", "jump", "move", "bitop", "perm", "gosub", "next"};
  fprintf(f, "    feedback (%u runs):", P->runs);
  for (int pc = 0; pc < P->nOp; pc++) {
    u8 m = P->fb[pc]; if (!m) continue;
    fprintf(f, " %d:%s=%s%s%s%s%s", pc, sqlite3OpcodeName(v->aOp[pc].opcode), m & QV_FB_NULL ? "n" : "", m & QV_FB_INT ? "i" : "",
            m & QV_FB_REAL ? "r" : "", m & QV_FB_TEXT ? "t" : "", m & QV_FB_BLOB ? "b" : "");
  }
  fprintf(f, "\n");
  for (int c = 0; c < P->nCur; c++) if (P->isTopn[c]) fprintf(f, "    top-n sorter: cursor %d%s\n", c, P->topnOff ? " (deoptimised)" : "");
  for (int c = 0; c < P->nCur; c++) if (P->isHash[c]) fprintf(f, "    hash set: cursor %d, %d-field keys%s\n", c, P->isHash[c], P->hashOff ? " (deoptimised)" : "");
  for (int g = 0; g < P->nrg; g++) {
    QvRegion *G = &P->rg[g];
    fprintf(f, "    region [%d, %d] cursor %d%s: rows %llu, exits %llu, misses %llu:", G->B, G->N, G->cur, G->off ? " (off)" : "",
            (unsigned long long)G->iters, (unsigned long long)G->exits, (unsigned long long)G->misses);
    for (int i = 0; i <= G->N - G->B; i++) {
      int k = 0; while (k < RG_N && qv_rgl[k] != G->n[i].h) k++;
      fprintf(f, " %s", k < RG_N ? K[k] : "?");
      if (k == RG_EXIT) fprintf(f, "(%s)", sqlite3OpcodeName(G->n[i].op->opcode));
    }
    fprintf(f, "\n");
  }
}
static int qv_rg_on(void) { return qcf.rgOn; }   /* (QV_NOREGION=1: off) */

#define QV_RUN qv_run_fb
#define QV_FB 1
#include "qvm_run.h"
#undef QV_RUN
#undef QV_FB
#define QV_RUN qv_run
#define QV_FB 0
#include "qvm_run.h"
#undef QV_RUN
#define QV_RUN qv_run_rs
#define QV_RESUME
#include "qvm_run.h"
#undef QV_RESUME
#undef QV_RUN
#undef QV_FB

static int qv_write_json(Vdbe *v, const QcVal *rows, int nrow, int ncol, QcOut *O, int vec) {
  QcTmpl *T = QCV(v)->pQcTmpl;
  if (!T || T->ncol != ncol) return QC_FALLBACK;
  char *p = O->buf, *end = O->buf + O->cap;
  if ((size_t)(end - p) < T->lopen + T->lclose + 64) return QC_FALLBACK;
  p = qc_frag(p, T->open, T->lopen);
  for (int i = 0; i < nrow; i++) {
    if ((size_t)(end - p) < T->lsep + QC_FRAG + 64) return QC_FALLBACK;
    if (i) p = qc_frag(p, T->sep, T->lsep);
    for (int k = 0; k < ncol; k++) {
      const QcVal *x = &rows[(size_t)i * (size_t)ncol + k];
      if (T->kind[k] == 'i') { if (x->type != SQLITE_INTEGER || (size_t)(end - p) < 2 * QC_FRAG + 21 + 64) return QC_FALLBACK; p = qc_frag(p, T->frag[k], T->lfrag[k]); p = qc_jint(p, x->i); }
      else { if (x->type != SQLITE_TEXT || (size_t)(end - p) < 2 * QC_FRAG + 2 + 6 * (size_t)x->n + 64) return QC_FALLBACK; p = qc_frag(p, T->frag[k], T->lfrag[k]); p = qc_jstr(p, x->z, (u32)x->n, vec); }
    }
    if ((size_t)(end - p) < 2 * QC_FRAG + 64) return QC_FALLBACK;
    p = qc_frag(p, T->frag[ncol], T->lfrag[ncol]);
  }
  if ((size_t)(end - p) < QC_FRAG + 64) return QC_FALLBACK;
  p = qc_frag(p, T->close, T->lclose);
  O->n = (size_t)(p - O->buf); O->rows = nrow; return SQLITE_OK;
}

static void qv_free(QvPlan *P, sqlite3 *db) {   /* the statement's program was deleted */
  for (int c = 0; c <= P->nCur; c++) { if (P->bopen[c]) sqlite3BtreeCloseCursor(P->bc[c]); sqlite3_free(P->bc[c]); }
  for (int c = 0; c <= P->nCur; c++) if (P->keep && P->keep[c].bt) { sqlite3BtreeCloseCursor(P->keep[c].bc); qv_bc_free(QV_CONN(db), P->keep[c].bc); }   /* (the last cursor of a BTREE_SINGLE B-tree closes it) */
  if (P->r) for (int r = 0; r <= P->nMem; r++) sqlite3VdbeMemRelease(&P->r[r]);
  if (P->c) for (int c = 0; c <= P->nCur; c++) { sqlite3_free(P->c[c].aType); sqlite3_free(P->c[c].aOff); if (P->c[c].ov) { if (P->c[c].ov->big) sqlite3RCStrUnref(P->c[c].ov->big); sqlite3_free(P->c[c].ov); } }
  if (P->octx) for (int i = 0; i < P->nctx; i++) { QvCtx *k = P->octx[P->ctxPc[i]]; sqlite3VdbeMemRelease(&k->res); sqlite3_free(k); }
  sqlite3_free(P->ctxPc);
  for (int g = 0; g < P->nrg; g++) sqlite3_free(P->rg[g].n);
  sqlite3_free(P->rg); sqlite3_free(P->rgAt);
  sqlite3_free(P->r); sqlite3_free(P->cmem); sqlite3_free(P->octx); sqlite3_free(P->once); sqlite3_free(P->isCnt); sqlite3_free(P->cnt);
  sqlite3_free(P->keep); sqlite3_free(P->isVec); sqlite3_free(P->isHash); sqlite3_free(P->isTopn); sqlite3_free(P->srtSkip); sqlite3_free(P->isSum); sqlite3_free(P->isAcc); sqlite3_free(P->sacc); sqlite3_free(P->fb);
  sqlite3_free(P->sig); sqlite3_free(P->csql); sqlite3_free(P->flt); sqlite3_free(P->fltOn); sqlite3_free(P->fpc);
  QvConn *VC = QCD(db)->pQvConn; QC_COUNT_DEL(VC->nplans, P); QC_LIST_DEL(VC->act, VC->nact, P, act);
  sqlite3_free(P);
}

/* ---------------------------------------------------------------- the plan cache across prepares
** A finalized statement's plan (qvm's or a general one) is kept, keyed by its connection and SQL text, with its
** program's signature. The next prepare of the same text on the same connection whose program has the same signature
** gets the plan back: no lift, and a qvm plan keeps its type feedback and loop regions. A changed program (schema,
** flags, functions, collations, statistics) does not match and is lifted afresh.
** - Going in, a plan closes what holds the old program's memory: B-tree cursors and qvm's kept ephemeral tables hold
**   its KeyInfos.
** - A general plan holds no other pointer into the program: its texts are copies, its KeyInfos references.
** - Coming out, a qvm plan's region nodes and function contexts are pointed at the new program.
** Bounded (QC_CACHE_PLANS, least recently used out), purged when the connection closes, one thread as the rest of qc.
** QC_NOCACHE=1 turns it off. The specialised plans (kind 1) are cheap to lift and not cached. */
static u64 qv_cache_hits, qv_cache_misses;   /* (for the tests) */
static int qv_cache_on(void) { return qcf.cacheOn; }   /* (QC_NOCACHE=1: off) */
static u32 qv_hash(const char *z, int n) {   /* a statement text's hash (the plan and negative caches: this process only), 8 bytes at a time */
  u64 h = 0x9e3779b97f4a7c15ull ^ (u64)n; int i = 0;
  for (; i + 8 <= n; i += 8) { u64 w; memcpy(&w, z + i, 8); h = (h ^ w) * 0xff51afd7ed558ccdull; h ^= h >> 29; }
  for (; i < n; i++) h = (h ^ (u8)z[i]) * 0x100000001b3ull;
  h ^= h >> 32; return (u32)h;
}
/* the fields both cacheable kinds have, by kind (2 general, 3 qvm) */
#define QV_CF(P, f) (*(int *)(P) == 2 ? &((QcGPlan *)(P))->f : &((QvPlan *)(P))->f)
static void qv_cache_drop(QvConn *VC, int i) {   /* entry i freed, the last moved into its place */
  void *P = VC->cache[i].P; *QV_CF(P, csql) = VC->cache[i].sql;   /* (freed with the plan) */
  if (*(int *)P == 2) qg_free(P, VC->cache[i].db); else qv_free(P, VC->cache[i].db);
  VC->cache[i] = VC->cache[--VC->ncache];
}
/* texts no tier would lift, by connection and schema generation: a statement prepared again and again (an ORM's) isn't
   walked by every lifter each time only to be refused again (speedtest1 orm/110: "qvm program" on every fresh handle).
   Only refusals of the program's shape are kept, not passing ones (memory, a full plan list); an entry can only cost a
   missed lift, never a result. A ring of QV_NEG entries, purged with the connection. */
static u64 qv_neg_hits;   /* (for the tests) */
static u64 qv_neg_gen(sqlite3 *db) { u64 g = 0; for (int i = 0; i < db->nDb; i++) if (db->aDb[i].pSchema) g = g * 1000003u + (u64)db->aDb[i].pSchema->iGeneration; return g; }
static int qv_neg_get(Vdbe *v) {   /* 1: refused before (qc_why says why) */
  QvConn *VC = QV_CONN(v->db); if (!VC->nneg || !v->zSql) return 0;
  int n = (int)strlen(v->zSql); u32 h = qv_hash(v->zSql, n);
  for (int i = 0; i < VC->nneg; i++) {
    QvNeg *e = &VC->neg[i];
    if (e->h == h && e->nsql == n && !memcmp(e->sql, v->zSql, (size_t)n)) {
      if (e->gen != qv_neg_gen(v->db)) { sqlite3_free(e->sql); *e = VC->neg[--VC->nneg]; return 0; }   /* the schema changed: tried again */
      qc_why = e->why; QC_INC(qv_neg_hits); return 1;
    }
  }
  return 0;
}
static void qv_neg_put(Vdbe *v, const char *why) {
  if (!qv_cache_on() || !v->zSql || !v->db || v->db->mallocFailed || !strcmp(why, "plans") || !strcmp(why, "oom") || !strcmp(why, "arena")) return;
  int n = (int)strlen(v->zSql); char *z = sqlite3_malloc(n + 1); if (!z) return;
  memcpy(z, v->zSql, (size_t)n + 1);
  QvConn *VC = QV_CONN(v->db);
  QvNeg *e; if (VC->nneg < QV_NEG) e = &VC->neg[VC->nneg++]; else { e = &VC->neg[VC->negi]; VC->negi = (VC->negi + 1) % QV_NEG; sqlite3_free(e->sql); }
  *e = (QvNeg){v->db, z, n, qv_hash(z, n), qv_neg_gen(v->db), why};
}
static void qv_conn_free(QvConn *VC) {   /* (sqlite3QcClose) the connection is being freed: its plan cache, refusals, run state */
  while (VC->ncache) qv_cache_drop(VC, 0);
  while (VC->nneg) sqlite3_free(VC->neg[--VC->nneg].sql);
  QvX *X = &VC->x;
  for (QvBlk *b = X->A.head, *n; b; b = n) { n = b->next; sqlite3_free(b); }
  for (QvBlk *b = X->A.spare, *n; b; b = n) { n = b->next; sqlite3_free(b); }
  sqlite3_free(X->pins); sqlite3_free(X->pset); sqlite3_free(X->out);
  while (VC->nbc) sqlite3_free(VC->bcpool[--VC->nbc]);
}
/* qc_free's first step for a general or qvm plan: 1 = kept in the cache (the caller does not free it) */
static int qv_cache_put(Vdbe *v, void *Pv) {
  int kind = *(int *)Pv;
  if (!qv_cache_on() || !*QV_CF(Pv, sig) || !v->zSql || !v->db || v->db->mallocFailed) return 0;
  int n = (int)strlen(v->zSql); char *z, **cs = QV_CF(Pv, csql); u32 h;
  if (*cs && *QV_CF(Pv, ncsql) == n && !memcmp(*cs, v->zSql, (size_t)n)) { z = *cs; h = *QV_CF(Pv, csh); }   /* (it came from the cache) */
  else { if (!(z = sqlite3_malloc(n + 1))) return 0; memcpy(z, v->zSql, (size_t)n + 1); h = qv_hash(z, n); sqlite3_free(*cs); }
  *cs = 0;
  if (kind == 2) {
    QcGPlan *P = Pv;
    for (int c = 0; c < QG_NLVL * 2; c++) if (P->bopen[c]) { sqlite3BtreeCloseCursor(P->bc[c]); P->bopen[c] = 0; }
    P->txnChecked = 0;
    QgConn *GC = QCD(v->db)->pQgConn; QC_COUNT_DEL(GC->nplans, P); QC_LIST_DEL(GC->act, GC->nact, P, act);
  } else {
    QvPlan *P = Pv;
    for (int c = 0; c <= P->nCur; c++) if (P->bopen[c]) { sqlite3BtreeCloseCursor(P->bc[c]); P->bopen[c] = 0; }
    for (int c = 0; c <= P->nCur; c++) if (P->keep[c].bt) { sqlite3BtreeCloseCursor(P->keep[c].bc); qv_bc_free(QV_CONN(v->db), P->keep[c].bc); memset(&P->keep[c], 0, sizeof P->keep[c]); }
    for (int i = 0; i < P->nctx; i++) { QvCtx *k = P->octx[P->ctxPc[i]]; sqlite3VdbeMemRelease(&k->res); k->res.flags = MEM_Null; k->u.c.pVdbe = 0; }
    for (int r = 0; r <= P->nMem; r++) qc_mem_keep(&P->r[r]);   /* (registers keep their values and buffers) */
    P->txnChecked = 0; P->vecOff = P->hashOff = P->topnOff = 0;
    QvConn *VC = QCD(v->db)->pQvConn; QC_COUNT_DEL(VC->nplans, P); QC_LIST_DEL(VC->act, VC->nact, P, act);
  }
  QvConn *VC = QV_CONN(v->db);
  for (int i = 0; i < VC->ncache; i++)   /* one plan per text (on the connection): a newer one replaces it */
    if (VC->cache[i].h == h && VC->cache[i].nsql == n && !memcmp(VC->cache[i].sql, z, (size_t)n)) { qv_cache_drop(VC, i); break; }
  if (VC->ncache == QC_CACHE_PLANS) {
    int o = 0; for (int i = 1; i < VC->ncache; i++) if (VC->cache[i].used < VC->cache[o].used) o = i;
    qv_cache_drop(VC, o);
  }
  VC->cache[VC->ncache++] = (QvCached){v->db, z, n, h, ++VC->cacheClock, Pv};
  return 1;
}
/* qc_lift's first step: the cached plan for v's text, when v's program has its signature. 1 = v has it */
static int qv_cache_get(Vdbe *v) {
  QvConn *VC = QV_CONN(v->db); if (!VC->ncache || !v->zSql) return 0;
  int n = (int)strlen(v->zSql); u32 h = qv_hash(v->zSql, n), i;
  for (i = 0; i < (u32)VC->ncache; i++)
    if (VC->cache[i].h == h && VC->cache[i].nsql == n && !memcmp(VC->cache[i].sql, v->zSql, (size_t)n)) break;
  if (i == (u32)VC->ncache) return 0;
  void *Pv = VC->cache[i].P; int kind = *(int *)Pv; QvSig g = {0, 0, 0, *QV_CF(Pv, sig), *QV_CF(Pv, nsig)};
  int iDb = *QV_CF(Pv, iDb);   /* the schema it was lifted against (the cookie is in the program: OP_Transaction) */
  QvStamp now; now.ok = 0;   /* (a program that depends on its bindings, expmask, is always compared, and isn't stamped:
                                uptime_kuma's paid the stamp on top, +145 a prepare, bench exp1068) */
  int noStamp = qcf.noStamp;   /* QC_NOSTAMP=1: always compared (A/B) */
  if (!v->expmask) qv_stamp(v, &now);
  int same = !noStamp && now.ok && qv_stamp_eq(&now, QV_CF(Pv, stamp));
  if (same) {   /* (and the schema cookie the new program's OP_Transaction expects: another connection's DDL changes it on disk
                   without changing this connection's schema until it reloads; bench's qtest: 3x the "v schema" fallbacks without) */
    int t = v->aOp[0].opcode == OP_Init ? v->aOp[0].p2 : -1;
    same = t >= 0 && t < v->nOp && v->aOp[t].opcode == OP_Transaction && v->aOp[t].p3 == *QV_CF(Pv, cookie);
  }
  if ((!same && !qv_sig(v, &g)) || (iDb >= 0 && (iDb >= v->db->nDb || !v->db->aDb[iDb].pSchema || v->db->aDb[iDb].pSchema->iGeneration != *QV_CF(Pv, gen)))) {
    qv_cache_drop(VC, (int)i); QC_INC(qv_cache_misses); return 0;   /* the program or the schema changed: lifted afresh */
  }
  if (kind == 2) {
    QcGPlan *P = Pv; QgConn *GC = QCD(v->db)->pQgConn; if (GC->nplans >= QC_MAXPLANS) return 0;
    QC_COUNT_ADD(GC->nplans, P);
  } else {
    QvPlan *P = Pv; QvConn *VC = QCD(v->db)->pQvConn; if (VC->nplans >= QC_MAXPLANS) return 0;
    qv_str8(v);
    for (int r = 0; r < P->nrg; r++) for (int k = 0; k <= P->rg[r].N - P->rg[r].B; k++) P->rg[r].n[k].op = &v->aOp[P->rg[r].n[k].pc];
    for (int i = 0; i < P->nctx; i++) ((QvCtx *)P->octx[P->ctxPc[i]])->u.c.pVdbe = v;
    QC_COUNT_ADD(VC->nplans, P);
  }
  *QV_CF(Pv, csql) = VC->cache[i].sql; *QV_CF(Pv, ncsql) = n; *QV_CF(Pv, csh) = h; VC->cache[i] = VC->cache[--VC->ncache];
  if (now.ok) *QV_CF(Pv, stamp) = now;   /* (matched: the program is the plan's in this state) */
  QCV(v)->pQc = Pv; QCV(v)->qcWant = 1;
  QC_INC(qv_cache_hits);
  return 1;
}

/* Streaming (QC_STREAM=1): a served qvm plan past its feedback runs hands out each row as it reaches it, in its own
   registers (X->res), as vdbe.c's OP_ResultRow does, instead of keeping the whole result first: ResultRow returns 3, and
   qv_run_rs resumes from X->pc0 at the next step. One stream is suspended at a time on a connection (its QvX is one); before anything else
   runs qvm or starts a statement, it is drained, its remaining rows written into its sink as an unstreamed run's are. */
#define QC_STREAM (-3)
#ifndef QV_STREAM_MIN
#ifndef QV_STREAM_DEFAULT
#define QV_STREAM_DEFAULT 1   /* QC_STREAM=0 / 1 at run time. On since box2's exp1099: servestream against serve, 15 statements,
                                 hot instructions 0.992 and time 0.975 geomean, none slower in instructions (calibre
                                 books_table_columns 1.166 -> 1.106, time 1.118 -> 0.984) */
#endif
#define QV_STREAM_PER_TICK 8u   /* rows handed out per tick check, at least: the resumable loop (qv_run_rs) costs every row a
                                   streamed run reads, and the copies it saves are of the rows handed out. chromium
                                   visit.google_search_visits (1,611 rows of 150k read, 2 a tick) 1.013 -> 1.037 of stock
                                   streamed; calibre books_table_columns (64 a tick) 1.166 -> 1.106 */
#define QV_STREAM_MIN 32   /* rows the plan's last run handed out, at least, with texts or blobs or 4 values a row: a short result
                              gains nothing streamed (vaultwarden users.find_by_uuid, one row: +1k instructions a run). 0: any (tests) */
#endif
static QV_INLINE int qv_stream_on(void) { return qcf.streamOn; }   /* (QC_STREAM=0 / 1; QC_STREAM_MIN=N at run time, qcf.streamMin: the tests' 0) */
/* a plan that may stream: on, and its rows the same when run again (a stream that fails after handing out rows is
   replayed by the interpreter, which skips them): no function that isn't deterministic (random(), changes(), a user
   function without SQLITE_DETERMINISTIC) */
static QV_NOINLINE int qv_streams(QvPlan *P, Vdbe *v) {
  if (!P->strmChecked) {
    P->strmChecked = 1; P->strmOk = 1;
    for (int pc = 0; pc < v->nOp; pc++) {
      const Op *o = &v->aOp[pc]; const FuncDef *f = 0;
      if (o->p4type == P4_FUNCCTX) f = o->p4.pCtx->pFunc; else if (o->p4type == P4_FUNCDEF) f = o->p4.pFunc;
      if (f && !(f->funcFlags & (SQLITE_FUNC_CONSTANT | SQLITE_FUNC_SLOCHNG))) { P->strmOk = 0; break; }
    }
  }
  return P->strmOk;
}
/* a streamed run's regions: their ResultRow nodes yield (RG_RESULTS) while it streams, and only then (an unstreamed run's
   RG_RESULT has no test for it) */
static void qv_strm_nodes(QvPlan *P, int on) {
  if (!qv_rgl) return;
  const void *from = qv_rgl[on ? RG_RESULT : RG_RESULTS], *to = qv_rgl[on ? RG_RESULTS : RG_RESULT];
  for (int g = 0; g < P->nrg; g++) { QvRegion *G = &P->rg[g]; for (int i = 0; i <= G->N - G->B; i++) if (G->n[i].h == from) G->n[i].h = to; }
}
/* a run's end (qv_exec's, a stream's): registers, cursors, pins and the arena as the next run wants them */
static QV_INLINE int qv_exec_end(QvX *X, QvPlan *P, QvSink *K, int st) {
  Vdbe *v = X->v; sqlite3 *db = X->db;
  if (X->strmNodes) { qv_strm_nodes(P, 0); X->strmNodes = 0; }
  P->rowsLast = X->nrow;
  P->ticksLast = (u32)X->nticks;
  if (K) K->on = 0;   /* (qc_serve_rows has taken them, or the run failed) */
  /* registers: aggregate contexts finalized and freed, dynamic values released, buffers (zMalloc) kept. A run that ended well
     with no op or overflow value that leaves one MEM_Dyn or MEM_Agg leaves them as they are: a program writes a register
     before it reads it (beets get_album: albums.*'s registers, 870 of 5k instructions a run) */
  if (st != SQLITE_OK || P->mayDyn || X->dyn) for (int r = 0; r <= P->nMem; r++) { Mem *m = &P->r[r]; if (VdbeMemDynamic(m)) sqlite3VdbeMemSetNull(m); m->flags = MEM_Null; }
  for (int c = 0; c < P->nCur; c++)   /* duplicates first: then an owner's cursor is its B-tree's only one */
    if (X->c[c].kind == QVC_EPH && !X->c[c].owner) qv_cur_close(X, &X->c[c]);
  for (int c = 0; c < P->nCur; c++) {
    QvCur *C = &X->c[c];
    if (C->kind == QVC_EPH && st == SQLITE_OK && !P->keep[c].bt) {   /* kept for the next run (a failed run closes everything) */
      P->keep[c].bt = C->ebt; P->keep[c].bc = C->bc; P->keep[c].root = C->eroot; P->keep[c].ki = C->ki; P->keep[c].p5 = C->ep5;
      qv_cur_reset(C);
    } else if (C->kind && C->kind != QVC_BT) qv_cur_close(X, C);
    else qv_cur_reset(C);
  }
  for (int i = 0; i < X->npin; i++) sqlite3PagerUnref(X->pins[i]);   /* (pages are never kept past a run: a write's balance
                                                                         would find them referenced, SQLITE_CORRUPT) */
  X->npin = 0; X->nset = 0;
  QvConn *VC = QV_CONN(db);
  if (VC->keepRun) { VC->keptA = X->A.head; X->A.head = 0; X->A.total = 0; }   /* (a stream drained for another statement: the
                                                                                  arena its last row may point into stays its statement's) */
  else qv_arena_reset(&X->A);
  if (v->pAuxData) sqlite3VdbeDeleteAuxData(db, &v->pAuxData, -1, 0);   /* functions' auxdata (sqlite3_set_auxdata), freed as vdbe.c's Cleanup frees it */
  if (P->nfpc) {   /* the Bloom filters probed in the next run (P->flt) */
    int recheck = P->runs % 16 == 0;
    for (int i = 0; i < P->nfpc; i++) { struct QvFlt *f = &P->flt[P->fpc[i]]; f->off = recheck ? 0 : f->off ? 1 : f->n >= 1024 && !f->hit; f->n = f->hit = 0; }
    memset(P->fltOn, 0, sizeof *P->fltOn * (size_t)(P->nMem + 1));
    for (int i = 0; i < P->nfpc; i++) { const Op *o = &v->aOp[P->fpc[i]]; if (!P->flt[P->fpc[i]].off && o->p1 > 0 && o->p1 <= P->nMem) P->fltOn[o->p1]++; }
  }
  return st;
}
static int qv_exec(Vdbe *v, QvPlan *P, qc_row_cb cb, void *ctx) {
  sqlite3 *db = v->db;
  if (v->eVdbeState != VDBE_READY_STATE) return QC_FB("v not ready");
  if (db->autoCommit && !QC_CONN(db)->ownTxn) return QC_FB("v autocommit");
  if (P->iDb >= db->nDb || v->expired) return QC_FB("v expired");
  Btree *bt = P->iDb >= 0 ? db->aDb[P->iDb].pBt : 0; if (P->iDb >= 0 && !bt) return QC_FB("v btree");
  if (bt && sqlite3BtreeTxnState(bt) == SQLITE_TXN_NONE) { int m; P->txnChecked = 0; if (sqlite3BtreeBeginTrans(bt, 0, &m) != SQLITE_OK) return QC_FB("v txn"); }
  if (bt && (!P->txnChecked || P->bt != bt || db->aDb[P->iDb].pSchema->schema_cookie != P->cookie)) {   /* (qc_exec_special's note: the connection's own DDL) */
    for (int c = 0; c < P->nCur; c++) if (P->bopen[c] && P->bt != bt) { sqlite3BtreeCloseCursor(P->bc[c]); P->bopen[c] = 0; }
    int iMeta; sqlite3BtreeGetMeta(bt, BTREE_SCHEMA_VERSION, (u32 *)&iMeta);
    if (iMeta != P->cookie || db->aDb[P->iDb].pSchema->iGeneration != P->gen) return QC_FB("v schema");
    P->txnChecked = 1; P->bt = bt;
    if (!P->act) { QvConn *VC = QCD(db)->pQvConn; QC_LIST_ADD(VC->act, VC->nact, P, act); }
  }
  QvConn *VC = QV_CONN(db); QvX *X = &VC->x; QvArena A = X->A; DbPage **pins = X->pins, **pset = X->pset; int cpin = X->cpin; u32 cset = X->cset; QcVal *out = X->out; size_t cout = X->cout;
  memset(X, 0, sizeof *X);
  X->A = A; X->pins = pins; X->cpin = cpin; X->pset = pset; X->cset = cset; X->out = out; X->cout = cout;
  X->P = P; X->acc = P->isAcc; X->v = v; X->db = db; X->O = cb ? 0 : ctx; qv_why = "";
  QvSink *K = cb == qc_serve_rows ? ctx : 0;
  if (K) { if (K->nv > K->hw) K->hw = K->nv; K->on = 1; K->vm = K->big | P->sinkBig; K->nv = 0; K->used = 0; X->snk = K; }
  X->r = P->r; X->c = P->c; X->once = P->once; X->octx = P->octx;
  if (++P->runs == 3 && !P->rgAt && qv_rg_on()) qv_rg_build(P, v);   /* two runs of feedback first */
  if (P->runs == 3 && QC_ST)   /* (QC_STATS: the masks the feedback runs left) */
    for (int pc = 0; pc < P->nOp; pc++) if (P->fb[pc]) {
      int o = v->aOp[pc].opcode;
      if (o == OP_Column) QC_INC(qcs.mask[0][P->fb[pc] & 31]);
      else if (o == OP_Eq || o == OP_Ne || o == OP_Lt || o == OP_Le || o == OP_Gt || o == OP_Ge) QC_INC(qcs.mask[1][P->fb[pc] & 31]);
    }
  X->tick = QV_TICK; X->usable = bt ? bt->pBt->usableSize : 0;
#ifdef QV_REENTER_OPS
  X->fbLeft = QV_REENTER_OPS;
#endif
  X->fb = P->runs <= 2 ? P->fb : 0; X->rgAt = P->rgAt;   /* (feedback only until the regions are built) */
  int st = QC_FALLBACK;
  if (P->hasOnce) memset(X->once, 0, (size_t)(P->nOp / 8 + 1));
  if (P->hasAcc) { memset(P->cnt, 0, sizeof(i64) * (size_t)(P->nMem + 1)); memset(P->sacc, 0, sizeof *P->sacc * (size_t)(P->nMem + 1)); }
  X->strm = VC->streamOk && K && !X->fb && qv_stream_on() && ((P->rowsLast >= qcf.streamMin && (P->sinkTxt || P->nout >= 4) && P->rowsLast >= QV_STREAM_PER_TICK * P->ticksLast) || !qcf.streamMin) && qv_streams(P, v);   /* (a row of a few integers: a pause per row costs more than the copies it saves, zotero collections.load_child_items +4 % in cycles, bench exp1027) */
  if (X->strm && P->nrg) { qv_strm_nodes(P, 1); X->strmNodes = 1; }
  int ran = X->fb ? qv_run_fb(X) : qv_run(X);
#ifdef QV_REENTER_OPS
  while (ran == 2) { X->fbLeft = QV_REENTER_OPS; ran = qv_run(X); }   /* (from X->pc0) */
#endif
  if (ran == 3) { K->on = 0; VC->strmP = P; VC->strmK = K; return QC_STREAM; }   /* (its first row: X->res) */
  if (!ran) { qc_why = *qv_why ? qv_why : "v run"; return qv_exec_end(X, P, K, st); }
  if (cb) st = cb(ctx, X->nrow, P->nout, X->out);
  else st = qv_write_json(v, X->out, X->nrow, P->nout, ctx, bt && bt->pBt->pPager->xGet == getPageNormal);
  if (K) { P->sinkBig = K->big; P->sinkTxt = K->used > 0; }   /* (texts or blobs kept: what streaming saves copying) */
  return qv_exec_end(X, P, K, st);
}
/* a streamed run on from its last row: in the region it left (its next row fetched there, as an unstreamed run's), then
   qv_run_rs from where the region leaves */
static int qv_stream_resume(QvX *X) {
  QvRegion *G = X->rgRes; X->rgRes = 0;
  if (G && !G->off) { int pc = X->pc0, r = qv_region(X, G, &pc); if (r != 1) return r; X->pc0 = pc; }
  return qv_run_rs(X);
}
/* the suspended stream's next row: SQLITE_ROW (in X->res), SQLITE_DONE (ended), QC_FALLBACK (failed; ended) */
static int qv_stream_next(sqlite3 *db) {
  QvConn *VC = QV_CONN(db); QvX *X = &VC->x; QvPlan *P = VC->strmP; QvSink *K = VC->strmK;
  int ran = qv_stream_resume(X);
  if (ran == 3) return SQLITE_ROW;
  VC->strmP = 0; VC->strmK = 0;
  if (!ran) { qc_why = *qv_why ? qv_why : "v stream"; return qv_exec_end(X, P, K, QC_FALLBACK); }
  qv_exec_end(X, P, K, SQLITE_OK);
  return SQLITE_DONE;
}
/* the suspended stream's remaining rows into its sink, as an unstreamed run writes them (qc_serve_rows takes them):
   SQLITE_OK, or QC_FALLBACK (failed) */
static int qv_stream_drain(sqlite3 *db) {
  QvConn *VC = QV_CONN(db); QvX *X = &VC->x; QvPlan *P = VC->strmP; QvSink *K = VC->strmK; VC->strmP = 0; VC->strmK = 0;
  u32 sent = (u32)X->nrow; X->strm = 0; X->nrow = 0; X->snk = K; K->on = 1; K->vm = K->big | P->sinkBig; if (K->nv > K->hw) K->hw = K->nv; K->nv = 0; K->used = 0;
  if (X->strmNodes) { qv_strm_nodes(P, 0); X->strmNodes = 0; }   /* (its rows from here into K) */
  int ran = qv_stream_resume(X), st = QC_FALLBACK;
  if (ran == 1) { st = qc_serve_rows(K, X->nrow, P->nout, X->out); P->sinkBig = K->big; }
  else qc_why = *qv_why ? qv_why : "v drain";
  st = qv_exec_end(X, P, K, st); P->rowsLast += sent;   /* (the rows streamed before it, too) */
  return st;
}
static void qv_stream_abort(sqlite3 *db) {   /* the suspended stream ended where it is (its statement reset or finalized) */
  QvConn *VC = QV_CONN(db); QvPlan *P = VC->strmP; QvSink *K = VC->strmK; VC->strmP = 0; VC->strmK = 0;
  qv_exec_end(&VC->x, P, K, QC_FALLBACK);
}

/* qcg.c: qc's general plans. #included by qc.c after its specialised (feed-shaped) plans.
**
** The lifter reads a SELECT's bytecode as the loop nest SQLite's planner emitted:
**   level     an access path on one cursor: a scan (Rewind/Last), a seek with an optional end test (Seek{GE,GT,LE,LT} +
**             Idx{GT,GE,LT,LE} on an index, or Rowid + Gt/Ge/Lt/Le on a table), a rowid point (SeekRowid), or the
**             values of an IN list. A loop level closes with Next/Prev on its cursor; that address is where "skip this
**             row" jumps go. A seek on a UNIQUE index with every key column equal has no closer (at most one row). A
**             DeferredSeek binds the table row behind an index cursor. A LEFT JOIN point level emits a NULL row when
**             nothing matches (SQLite's match flag: Integer 0/1 around the level, then IfPos / NullRow / Goto).
**   subquery  a BeginSubrtn ... Return body run in line: a correlated count(*), or EXISTS. Lifted as a level of its own
**             and run when its value is needed (it reads no state but the outer rows, so when does not matter).
**   IN list   a BeginSubrtn / Once / OpenEphemeral body filling an ephemeral index from values; the loop over that
**             index becomes a level over the values, sorted and de-duplicated as the index would hold them.
**   body      Column / Rowid / IdxRowid / Copy into symbolic registers; filters (compare ops, If/IfNot, IsNull/NotNull
**             jumping to a level's continue address); deterministic built-in functions, called through SQLite's own
**             implementations; nested levels; ResultRow or AggStep.
**   tail      DecrJumpZero (LIMIT); for aggregates, AggFinal + Copy + ResultRow after the loops.
**   GROUP BY  over rows that arrive in key order (an index): Compare / Jump against the previous key, with the output
**             and reset subroutines (Gosub) SQLite emits; a group's row is output when the next key differs.
**   ORDER BY  through a sorter (SorterOpen / SorterInsert / SorterSort ... SorterNext) or, with LIMIT, a top-N
**             ephemeral index (Sequence / IfNotZero / Last / IdxLE / Delete / IdxInsert ... Sort ... Next). Both come
**             out as a stable sort on the key fields (SQLite's in-memory merge keeps insertion order on ties; the
**             top-N index orders ties by the sequence number), which is what qc does.
** Seeks and end tests run exactly as vdbe.c runs them (sqlite3BtreeIndexMoveto / TableMoveto with the opcode's
** default_rc and Next/Previous fix-up; sqlite3VdbeRecordCompareWithSkip for Idx* end tests). Values are decoded from
** the records directly. Anything the bytecode would have converted (a key of the wrong type for its affinity, a
** mixed-type comparison, a non-BINARY collation, an overflow record, a function error or REAL result, too many rows)
** returns QC_FALLBACK before any row is handed out. */

#define QG_MAXLVL 6             /* loop levels; subquery levels follow them in P->lvl */
#define QG_SUB0 QG_MAXLVL
#define QG_MAXSUB 4
#define QG_NLVL (QG_MAXLVL + QG_MAXSUB)
#define QG_MAXKEY 6
#define QG_MAXFILT 8
#define QG_MAXROWS 256
#ifndef QG_DEMOTE
#define QG_DEMOTE (-2)   /* qg_exec: a row it can't reproduce, or a result that outgrew its buffers; the caller re-lifts the statement for qvm */
#endif
#define QG_MAXAGG 4
#define QG_MAXFN 8
#define QG_MAXIN 32
#define QG_MAXEX 32

enum { G_NONE, G_PARAM, G_INT, G_NULL, G_TEXT, G_ADD, G_SUB, G_COL, G_ROWID, G_AGG, G_SUBQ, G_FUNC, G_INVAL, G_CAP, G_SREC, G_SEQ };
/* a, b: operands of G_ADD/G_SUB (indices into P->ex); a: G_FUNC's index into P->fn; col: G_SUBQ's level, G_AGG's agg,
   G_CAP's captured column (GROUP BY), G_SREC's field of a sorted record */
typedef struct GSym { u8 kind; i8 lvl; i16 col; i64 v; const char *z; int a, b; } GSym;
enum { GL_SCAN = 1, GL_SEEK, GL_ROWID, GL_INLIST };
enum { F_CMP = 1, F_IF, F_IFNOT, F_ISNULL, F_NOTNULL };
typedef struct GFilt { u8 kind, op, p5, p3; GSym l, r; } GFilt;   /* F_CMP: skip the row when r <op> l (vdbe's P3 <op> P1) */
typedef struct GLevel {
  u8 kind, isIdx, dir, once;           /* dir: 1 Next, 0 Prev; once: no loop (at most one row) */
  u8 left, subKind;                    /* left: LEFT JOIN; subKind: 1 count(*) subquery, 2 EXISTS */
  u8 batch, cntEq;                     /* batch: table rows fetched in batches; cntEq: count(*) of one integer key, by a cell walk */
  int cur; Pgno root; KeyInfo *ki;
  u8 seekOp; int nkey; GSym key[QG_MAXKEY]; char aff[QG_MAXKEY + 1]; u8 keyNullExit;   /* 1: NULL seek key, 2: NULL end key: no rows */
  u8 endOp; int nend; GSym end[QG_MAXKEY];        /* index end test: OP_IdxGT..; table end test: OP_Gt.. against the rowid */
  char endAff[QG_MAXKEY + 1]; u8 endNull;          /* a computed end bound's Affinity; IsNull on end keys (bit j): no rows */
  u8 endP5;
  GSym rowidKey;                                   /* GL_ROWID */
  int tabCur; Pgno tabRoot;                        /* DeferredSeek: the table row behind this index cursor, or -1 */
  int nfilt; GFilt filt[QG_MAXFILT];
  int nin; GSym in[QG_MAXIN]; char inAff;          /* GL_INLIST: the values and the index's affinity */
  int head, cont, exitPc, leftPc;                  /* addresses (lifter only) */
  u32 needTab, needIdx;                            /* columns decoded from the table / index record */
  u32 fltTab;                                      /* the table columns the filters read, decoded alone first (the rest only for a row that passes); 0: all at once */
  QcShape tabShape, idxShape; int (*tabDec)(const u8 *, u32, QcVal *), (*idxDec)(const u8 *, u32, QcVal *);
} GLevel;
typedef struct GAgg { u8 fn; GSym arg; int reg, flagReg; } GAgg;   /* fn: 1 count(*), 2 min, 3 max; flagReg: CollSeq's "row lost" flag */
typedef struct GFn { FuncDef *f; int argc, iOp; GSym arg[4]; } GFn;
typedef struct QcGPlan {
  int kind;                                        /* QC_KIND_GENERAL */
  int iDb, cookie, gen;
  int nlvl, nsub; GLevel lvl[QG_NLVL];
  int nout; GSym out[QC_MAXOUT];
  u8 lookup; u32 lastRows, nexec;                  /* a point lookup's shape; its last run's rows; its runs (qg_exec's demotion) */
  int nagg; GAgg agg[QG_MAXAGG];
  int nfn; GFn fn[QG_MAXFN];
  int hasLimit; GSym limit;
  int nex; GSym ex[QG_MAXEX];                      /* operands of computed values */
  /* GROUP BY over rows in key order: a group ends when its key changes; captures: bare columns from its first row */
  int ngk; GSym gk[QG_MAXKEY]; int ncap; GSym cap[QC_MAXCOL]; int nhav; GFilt hav[QG_MAXFILT];
  int capAgg;   /* -1: captures from a group's first row; a: from the row min()/max() aggregate a last took (SQLite's bare columns) */
  /* ORDER BY through a sorter: the loop's rows become records (keys first), stably sorted, then output */
  int nrec, nsk; GSym rec[QC_MAXCOL]; u8 sflag[QG_MAXKEY]; i64 topN;
  int ncur; struct { int cur; Pgno root; KeyInfo *ki; u8 isTab, eph; } curs[QG_NLVL * 2];
  Btree *bt; BtCursor *bc[QG_NLVL * 2]; u8 bopen[QG_NLVL * 2]; u8 txnChecked; int slot, act;   /* (QC_LIST) */
  UnpackedRecord *uk[QG_NLVL];
  char **str; int nstr, cstr;              /* its text constants, copied (it keeps no pointer into the program: the plan cache) */
  u8 *sig; u32 nsig;                       /* the program it was lifted from (qv_sig; 0: not cacheable) */
  QvStamp stamp;                           /* the state that program was generated in (qv_stamp) */
  char *csql; int ncsql; u32 csh;          /* its text in the plan cache, kept while the plan is out */
  QcAdapt ad;                                      /* measured against the interpreter (qc_adapt) */
} QcGPlan;

typedef struct QgConn {   /* qcg.c's state for a connection (db->pQgConn: qc.c's per-connection state) */
  int nplans, nact; QcGPlan *act[QC_MAXPLANS];   /* the general plans: counted, and the ones in this transaction */
  struct QgRun *run;                              /* the run state (made at the first run) */
} QgConn;
static void qg_txn_end(Btree *bt) {
  QgConn *GC = QCD(bt->db)->pQgConn; if (!GC) return;
  for (int k = 0; k < GC->nact; ) {
    QcGPlan *P = GC->act[k]; if (P->bt != bt) { k++; continue; }
    P->txnChecked = 0;
    for (int c = 0; c < P->ncur; c++) if (P->bopen[c]) { sqlite3BtreeCloseCursor(P->bc[c]); P->bopen[c] = 0; }
    QC_LIST_DEL(GC->act, GC->nact, P, act);
  }
}

static void qg_unlift(QcGPlan *P, sqlite3 *db) {   /* what a lifted plan holds (also a lift that refuses) */
  for (int l = 0; l < QG_NLVL; l++) sqlite3DbFree(db, P->uk[l]);
  for (int c = 0; c < P->ncur; c++) if (P->curs[c].ki) sqlite3KeyInfoUnref(P->curs[c].ki);
  for (int i = 0; i < P->nstr; i++) sqlite3_free(P->str[i]);
  sqlite3_free(P->str); sqlite3_free(P->sig); sqlite3_free(P->csql);
  sqlite3_free(P);
}
static void qg_free(QcGPlan *P, sqlite3 *db) {   /* the statement's program was deleted */
  for (int c = 0; c < QG_NLVL * 2; c++) { if (P->bopen[c]) sqlite3BtreeCloseCursor(P->bc[c]); sqlite3_free(P->bc[c]); }
  QgConn *GC = QCD(db)->pQgConn; QC_COUNT_DEL(GC->nplans, P); QC_LIST_DEL(GC->act, GC->nact, P, act);
  qg_unlift(P, db);
}
static void qg_sig(Vdbe *v, QcGPlan *P) { QvSig g = {0}; if (qv_sig(v, &g)) { P->sig = g.b; P->nsig = g.n; qv_stamp(v, &P->stamp); } }
static const char *qg_keep(QcGPlan *P, const char *z, i64 n) {   /* a copy of a program's text constant, owned by the plan */
  if (P->nstr == P->cstr) {
    int c = P->cstr ? P->cstr * 2 : 8; char **a = sqlite3_realloc64(P->str, sizeof *a * (u64)c); if (!a) return 0;
    P->str = a; P->cstr = c;
  }
  char *k = sqlite3_malloc64((u64)n + 1); if (!k) return 0;
  memcpy(k, z, (size_t)n); k[n] = 0; P->str[P->nstr++] = k; return k;
}

/* ---------------------------------------------------------------- lifter */
#define QG_REFUSE(why) do { qc_why = (why); return 0; } while (0)
typedef struct GLift {
  QcGPlan *P; Op *aOp; int nMem, endInit;
  GSym reg[256]; signed char lvlOfCur[64]; u8 tabOfCur[64];
  int aggMode, haveOut, haltPc, limitPc, aggEndPc;
  int stack[QG_NLVL], nstack;          /* open levels (indices into P->lvl), innermost last */
  int ninl; struct { int cur, nin; GSym in[QG_MAXIN]; char aff; } inl[2];   /* IN-list values, until their loop opens */
  /* GROUP BY: the subroutines' return registers and addresses, the "use" flag, and where LIMIT goes */
  int grp, rOut, rReset, rUse, outPc, outRet, abortPc, bodyPc, inOut;
  /* ORDER BY: the sorter (or top-N index) cursor, the pseudo cursor over its records, the output loop */
  int sortCur, pseudoCur, sortTop, sortStart, sortOut, sortExit, ngkSeen, collReg; KeyInfo *sortKi;
} GLift;
static int qg_curslot(QcGPlan *P, int cur) { for (int k = 0; k < P->ncur; k++) if (P->curs[k].cur == cur) return k; return -1; }
static int qg_value_sym(const GSym *s) { return s->kind == G_PARAM || s->kind == G_INT || s->kind == G_NULL || s->kind == G_TEXT || s->kind == G_ADD || s->kind == G_SUB; }
static int qg_row_sym(const GSym *s) { return s->kind == G_COL || s->kind == G_ROWID || s->kind == G_SUBQ || s->kind == G_FUNC || s->kind == G_INVAL ||
                                              s->kind == G_AGG || s->kind == G_CAP || s->kind == G_SREC; }
static int qg_any_sym(const GSym *s) { return qg_value_sym(s) || qg_row_sym(s); }
static int qg_point(const GLevel *L) { return L->kind == GL_ROWID || L->once; }
static void qg_pop_points(GLift *G) { while (G->nstack && qg_point(&G->P->lvl[G->stack[G->nstack - 1]])) G->nstack--; }
static int qg_expr(GLift *G, const Op *e) {   /* Add / Subtract on values: P3 = P2 + P1, P3 = P2 - P1 */
  QcGPlan *P = G->P;
  if (!qg_value_sym(&G->reg[e->p1]) || !qg_value_sym(&G->reg[e->p2]) || P->nex + 2 > QG_MAXEX || e->p3 > G->nMem) QG_REFUSE("expr");
  int a = P->nex++, b = P->nex++; P->ex[a] = G->reg[e->p1]; P->ex[b] = G->reg[e->p2];
  G->reg[e->p3] = (GSym){(u8)(e->opcode == OP_Add ? G_ADD : G_SUB), 0, 0, 0, 0, b, a};
  return 1;
}
/* the open level whose continue address is pc (a filter's jump target), or -1 */
static int qg_level_at(GLift *G, int pc) {
  for (int k = G->nstack - 1; k >= 0; k--) if (G->P->lvl[G->stack[k]].cont == pc) return G->stack[k];
  return -1;
}
/* constants into registers; 1 = handled */
static int qg_constop(GLift *G, const Op *o) {
  GSym *reg = G->reg; int nMem = G->nMem;
  switch (o->opcode) {
    case OP_Integer: if (o->p2 > nMem) return 0; reg[o->p2] = (GSym){G_INT, 0, 0, o->p1}; return 1;
    case OP_Int64: if (o->p2 > nMem) return 0; reg[o->p2] = (GSym){G_INT, 0, 0, INT32_TO_64(o->p1, o->p3)}; return 1;   /* 3.54: the value in P1/P3 */
    case OP_Variable: if (o->p2 > nMem) return 0; reg[o->p2] = (GSym){G_PARAM, 0, 0, o->p1}; return 1;
    case OP_String8:   /* becomes OP_String on its first run */
      if (o->p2 > nMem || o->p4type != P4_STATIC || o->p3) return 0;   /* (P3: the LIKE optimization's blob pass, not reproduced here) */
      { i64 n = (i64)strlen(o->p4.z); const char *z = qg_keep(G->P, o->p4.z, n); if (!z) return 0; reg[o->p2] = (GSym){G_TEXT, 0, 0, n, z}; } return 1;
    case OP_String:
      if (o->p2 > nMem || (o->p4type != P4_STATIC && o->p4type != P4_DYNAMIC) || o->p3 || o->p5) return 0;
      { const char *z = qg_keep(G->P, o->p4.z, o->p1); if (!z) return 0; reg[o->p2] = (GSym){G_TEXT, 0, 0, o->p1, z}; } return 1;
    case OP_Null:
      if (o->p1 || o->p2 > nMem || o->p3 > nMem) return 0;
      for (int r = o->p2; r <= (o->p3 > o->p2 ? o->p3 : o->p2); r++) reg[r] = (GSym){G_NULL};
      return 1;
  }
  return 0;
}
/* the level opened by the op at *pcp, into P->lvl[idx]; *pcp ends on the last op consumed */
static int qg_open(GLift *G, int *pcp, int idx, int sub) {
  QcGPlan *P = G->P; Op *aOp = G->aOp; GSym *reg = G->reg; int pc = *pcp; Op *o = &aOp[pc];
  int k = qg_curslot(P, o->p1);
  if (k < 0 || o->p1 >= 64 || G->nstack >= QG_NLVL) QG_REFUSE("level");
  GLevel *L = &P->lvl[idx]; memset(L, 0, sizeof *L);
  L->cur = o->p1; L->root = P->curs[k].root; L->ki = P->curs[k].ki; L->isIdx = !P->curs[k].isTab; L->tabCur = -1;
  L->exitPc = o->p2;
  if (P->curs[k].eph) {   /* the IN list's ephemeral index: a level over the values */
    int j = -1; for (int i = 0; i < G->ninl; i++) if (G->inl[i].cur == o->p1) j = i;
    if (o->opcode != OP_Rewind || j < 0) QG_REFUSE("in-list loop");
    L->kind = GL_INLIST; L->head = pc + 1; L->dir = 1; L->isIdx = 0;
    L->nin = G->inl[j].nin; memcpy(L->in, G->inl[j].in, sizeof L->in); L->inAff = G->inl[j].aff;
  } else if (o->opcode == OP_SeekRowid) {
    if (L->isIdx || !qg_any_sym(&reg[o->p3])) QG_REFUSE("seekrowid key");
    L->kind = GL_ROWID; L->rowidKey = reg[o->p3]; L->head = pc + 1; L->cont = o->p2; L->dir = 1;
  } else if (o->opcode == OP_Rewind || o->opcode == OP_Last) {
    L->kind = GL_SCAN; L->head = pc + 1; L->dir = o->opcode == OP_Rewind;
  } else {
    L->kind = GL_SEEK; L->seekOp = (u8)o->opcode; L->head = pc + 1;
    L->dir = o->opcode == OP_SeekGE || o->opcode == OP_SeekGT;
    if (L->isIdx) {
      if (o->p4type != P4_INT32 || o->p4.i < 1 || o->p4.i > QG_MAXKEY) QG_REFUSE("seek keys");
      L->nkey = o->p4.i;
    } else L->nkey = 1;
    for (int j = 0; j < L->nkey; j++) {
      GSym s = reg[o->p3 + j];
      if (!qg_any_sym(&s)) QG_REFUSE("seek key");
      L->key[j] = s;
    }
    /* affinity and IsNull applied to the key registers just before the seek */
    for (Op *q = o - 1; q > aOp && (q->opcode == OP_Affinity || q->opcode == OP_IsNull || q->opcode == OP_Variable ||
                                    q->opcode == OP_IdxRowid || q->opcode == OP_Rowid || q->opcode == OP_Column); q--) {
      if (q->opcode == OP_IsNull && q->p1 >= o->p3 && q->p1 < o->p3 + L->nkey && q->p2 == o->p2) L->keyNullExit |= 1;
      if (q->opcode != OP_Affinity) continue;
      int strp4 = q->p4type == P4_STATIC || q->p4type == P4_DYNAMIC;
      if (q->p1 != o->p3 || q->p2 > L->nkey || !strp4) QG_REFUSE("affinity");
      for (int j = 0; j < q->p2; j++) L->aff[j] = q->p4.z[j];
    }
    /* the end test at the loop head, maybe after computed bounds (BETWEEN ?1 AND ?1 + 20) */
    int q = pc + 1, nnull = 0, nullReg[QG_MAXKEY], affReg = -1; const char *affZ = 0; int affN = 0;
    for (;; q++) {   /* computed end bounds: Add / Subtract, then IsNull (exits) and Affinity on them */
      Op *e = &aOp[q];
      if (e->opcode == OP_Add || e->opcode == OP_Subtract) { if (!qg_expr(G, e)) return 0; }
      else if (e->opcode == OP_IsNull && e->p2 == o->p2 && nnull < QG_MAXKEY) nullReg[nnull++] = e->p1;
      else if (e->opcode == OP_Affinity && (e->p4type == P4_STATIC || e->p4type == P4_DYNAMIC) && affReg < 0) { affReg = e->p1; affN = e->p2; affZ = e->p4.z; }
      else break;
    }
    Op *h = &aOp[q];
    int idxEnd = L->isIdx && h->opcode >= OP_IdxLE && h->opcode <= OP_IdxGE && h->p1 == o->p1 && h->p2 == o->p2;
    if (nnull || affReg >= 0) {
      if (!idxEnd || h->p4type != P4_INT32 || h->p4.i < 1 || h->p4.i > QG_MAXKEY) QG_REFUSE("end key prep");
      for (int k = 0; k < nnull; k++) { int j = nullReg[k] - h->p3; if (j < 0 || j >= h->p4.i) QG_REFUSE("end isnull"); L->endNull |= (u8)(1u << j); }
      if (affReg >= 0) { if (affReg != h->p3 || affN > h->p4.i) QG_REFUSE("end affinity"); for (int j = 0; j < affN; j++) L->endAff[j] = affZ[j]; }
    }
    if (L->isIdx && h->opcode >= OP_IdxLE && h->opcode <= OP_IdxGE && h->p1 == o->p1 && h->p2 == o->p2) {
      if (h->p4type != P4_INT32 || h->p4.i < 1 || h->p4.i > QG_MAXKEY) QG_REFUSE("end keys");
      L->endOp = (u8)h->opcode; L->nend = h->p4.i;
      for (int j = 0; j < L->nend; j++) { GSym s = reg[h->p3 + j]; if (!qg_any_sym(&s)) QG_REFUSE("end key"); L->end[j] = s; }
      L->head = q; pc = q;   /* Next/Prev jumps back to the end test itself */
    } else if (!L->isIdx && aOp[q].opcode == OP_Rowid && aOp[q].p1 == o->p1 && aOp[q + 1].opcode >= OP_Gt && aOp[q + 1].opcode <= OP_Ge &&
               aOp[q + 1].p3 == aOp[q].p2 && aOp[q + 1].p2 == o->p2 && qg_value_sym(&reg[aOp[q + 1].p1])) {
      L->endOp = (u8)aOp[q + 1].opcode; L->end[0] = reg[aOp[q + 1].p1]; L->nend = 1; L->endP5 = aOp[q + 1].p5;
      L->head = q; pc = q + 1;   /* the Rowid at the head is re-read by the body if it uses it */
      reg[aOp[q].p2] = (GSym){G_ROWID, (i8)idx};
    } else { L->head = q; pc = q - 1; }
  }
  /* the closer: the first Next/Prev on this cursor after the head that jumps back to the head. A seek on a UNIQUE
     index with every key column equal has none (at most one row): it runs once, like a point level */
  if (L->kind != GL_ROWID) {
    int c = -1;
    for (int q = L->head; q < G->endInit; q++)
      if ((aOp[q].opcode == OP_Next || aOp[q].opcode == OP_Prev) && aOp[q].p1 == o->p1) { c = q; break; }
    if (c < 0 && L->kind == GL_SEEK && L->isIdx && L->endOp) { L->once = 1; L->cont = L->exitPc; }
    else if (c < 0 || aOp[c].p2 != L->head || (aOp[c].opcode == OP_Next) != L->dir) QG_REFUSE("closer");
    else L->cont = c;
  }
  L->subKind = (u8)sub;
  G->lvlOfCur[o->p1] = (signed char)idx;
  G->stack[G->nstack++] = idx;
  *pcp = pc;
  return 1;
}
static int qg_walk(GLift *G, int from, int to, int sub);
static int qg_is_opener(int op) { return op == OP_Rewind || op == OP_Last || op == OP_SeekRowid || (op >= OP_SeekLT && op <= OP_SeekGT); }
/* the BeginSubrtn at pc: an IN-list builder, an EXISTS or a correlated count(*). Returns its Return's address; 0 refused */
static int qg_subrtn(GLift *G, int pc) {
  QcGPlan *P = G->P; Op *aOp = G->aOp; Op *b = &aOp[pc];
  int ret = -1;
  for (int q = pc + 1; q < G->endInit; q++) if (aOp[q].opcode == OP_Return && aOp[q].p1 == b->p2) { ret = q; break; }
  if (ret < 0) QG_REFUSE("subroutine end");
  int q = pc + 1, once = 0;
  if (aOp[q].opcode == OP_Once) { if (aOp[q].p2 != ret) QG_REFUSE("once"); once = 1; q++; }
  if (aOp[q].opcode == OP_OpenEphemeral) {   /* x IN (?, ?, ...): Variable; MakeRecord; IdxInsert per value */
    Op *e = &aOp[q];
    if (!once || e->p1 >= 64 || e->p2 != 1 || e->p4type != P4_KEYINFO || e->p4.pKeyInfo->nKeyField != 1 ||
        P->ncur >= QG_NLVL * 2 || G->ninl >= 2) QG_REFUSE("in-list");
    CollSeq *cs = e->p4.pKeyInfo->aColl[0];
    if (cs && sqlite3StrICmp(cs->zName, "BINARY")) QG_REFUSE("in-list collation");
    int k = P->ncur++; P->curs[k].cur = e->p1; P->curs[k].eph = 1; P->curs[k].isTab = 1;
    int j = G->ninl++; G->inl[j].cur = e->p1; G->inl[j].nin = 0;
    for (q++; q < ret; q++) {
      Op *o = &aOp[q];
      if (qg_constop(G, o)) continue;
      if (o->opcode == OP_MakeRecord && o->p2 == 1) {
        char a = (o->p4type == P4_STATIC || o->p4type == P4_DYNAMIC) ? o->p4.z[0] : 0;
        if (G->inl[j].nin && a != G->inl[j].aff) QG_REFUSE("in-list affinity");
        if (G->inl[j].nin >= QG_MAXIN || !qg_value_sym(&G->reg[o->p1])) QG_REFUSE("in-list value");
        G->inl[j].aff = a; G->inl[j].in[G->inl[j].nin++] = G->reg[o->p1];
        Op *ins = o + 1; if (ins->opcode != OP_IdxInsert || ins->p1 != e->p1 || ins->p2 != o->p3) QG_REFUSE("in-list insert");
        q++; continue;
      }
      if (o->opcode == OP_NullRow && o->p1 == e->p1) continue;
      QG_REFUSE("in-list op");
    }
    return ret;
  }
  /* EXISTS: Integer 0 rRes; Integer 1 rLim; <level>; Integer 1 rRes; DecrJumpZero rLim; Return
     count:  Null rRes; Integer 1 rLim; Null rAgg; <level>; AggStep count rAgg; Next; AggFinal rAgg; Copy rAgg rRes;
             DecrJumpZero rLim; Return */
  if (P->nsub >= QG_MAXSUB) QG_REFUSE("subqueries");
  int idx = QG_SUB0 + P->nsub, kind, rRes, rAgg = -1;
  if (aOp[q].opcode == OP_Integer && aOp[q].p1 == 0) kind = 2;
  else if (aOp[q].opcode == OP_Null && aOp[q].p1 == 0 && aOp[q].p2 == aOp[q].p3) kind = 1;
  else QG_REFUSE("subquery shape");
  rRes = aOp[q].p2; q++;
  if (aOp[q].opcode != OP_Integer || aOp[q].p1 != 1) QG_REFUSE("subquery limit");
  q++;
  if (kind == 1) { if (aOp[q].opcode != OP_Null || aOp[q].p1 || aOp[q].p2 != aOp[q].p3) QG_REFUSE("subquery agg"); rAgg = aOp[q].p2; q++; }
  int opened = 0, finalPc = -1, nstack0 = G->nstack;
  for (; q < ret; q++) {
    Op *o = &aOp[q];
    if (!opened && qg_is_opener(o->opcode)) { if (!qg_open(G, &q, idx, kind)) return 0; opened = 1; continue; }
    if (opened && kind == 2 && o->opcode == OP_Integer && o->p1 == 1 && o->p2 == rRes) {   /* the hit */
      if (aOp[q + 1].opcode != OP_DecrJumpZero || aOp[q + 1].p2 != ret) QG_REFUSE("exists tail");
      q++; continue;
    }
    if (opened && kind == 1 && o->opcode == OP_AggStep) {
      if (o->p4type != P4_FUNCDEF || sqlite3StrICmp(o->p4.pFunc->zName, "count") || o->p5 || o->p3 != rAgg) QG_REFUSE("subquery aggstep");
      continue;
    }
    if (opened && (o->opcode == OP_Next || o->opcode == OP_Prev)) {
      if (o->p1 != P->lvl[idx].cur || q != P->lvl[idx].cont) QG_REFUSE("subquery closer");
      continue;
    }
    if (opened && kind == 1 && o->opcode == OP_AggFinal && o->p1 == rAgg) {
      if (aOp[q + 1].opcode != OP_Copy || aOp[q + 1].p1 != rAgg || aOp[q + 1].p2 != rRes || aOp[q + 1].p3 ||
          aOp[q + 2].opcode != OP_DecrJumpZero || q + 3 != ret) QG_REFUSE("subquery final");
      finalPc = q; q = ret; break;
    }
    switch (o->opcode) {   /* the rest goes through the body walker; nothing that ends a query */
      case OP_ResultRow: case OP_AggStep: case OP_AggFinal: case OP_DecrJumpZero: case OP_Halt: case OP_BeginSubrtn:
      case OP_Next: case OP_Prev: case OP_IfPos: QG_REFUSE("subquery op");
    }
    if (qg_is_opener(o->opcode)) QG_REFUSE("subquery levels");
    if (!qg_walk(G, q, q + 1, idx)) return 0;
  }
  if (!opened) QG_REFUSE("subquery level");
  GLevel *L = &P->lvl[idx];
  if (L->exitPc != (kind == 1 ? finalPc : ret) || (kind == 1 && finalPc < 0)) QG_REFUSE("subquery exit");
  if (qg_point(L)) L->cont = L->exitPc;
  P->nsub++;
  G->nstack = nstack0;
  G->reg[rRes] = (GSym){G_SUBQ, 0, (i16)idx};
  return ret;
}
/* the ops in [from, to); sub: the subquery level being lifted, or 0 */
static int qg_walk(GLift *G, int from, int to, int sub) {
  QcGPlan *P = G->P; Op *aOp = G->aOp; GSym *reg = G->reg; int nMem = G->nMem;
  for (int pc = from; pc < to; pc++) {
    Op *o = &aOp[pc];
    if (qg_constop(G, o)) continue;
    switch (o->opcode) {
      case OP_Noop: case OP_Affinity: break;   /* key affinities: read by qg_open */
      case OP_OpenRead: {
        if (o->p1 < 0 || o->p1 >= 64 || o->p3 != P->iDb || (o->p5 & ~OPFLAG_SEEKEQ) || P->ncur >= QG_NLVL * 2 ||
            (o->p4type != P4_INT32 && o->p4type != P4_KEYINFO)) QG_REFUSE("openread");
        int k = P->ncur++; P->curs[k].cur = o->p1; P->curs[k].root = (Pgno)o->p2; P->curs[k].ki = o->p4type == P4_KEYINFO ? qc_ki_keep(o->p4.pKeyInfo) : 0;   /* (it outlives the program in the plan cache: off lookaside) */
        P->curs[k].isTab = o->p4type != P4_KEYINFO;
        if (!P->curs[k].isTab && !P->curs[k].ki) QG_REFUSE("oom");
        if (P->curs[k].ki && P->curs[k].ki->nAllField > QG_MAXKEY + 2) QG_REFUSE("wide index");
        break;
      }
      case OP_IsNull: case OP_NotNull: {
        int l = qg_level_at(G, o->p2);
        if (l >= 0) {   /* a filter */
          GLevel *L = &P->lvl[l];
          if (L->nfilt >= QG_MAXFILT || !qg_any_sym(&reg[o->p1])) QG_REFUSE("null filter");
          GFilt *f = &L->filt[L->nfilt++]; memset(f, 0, sizeof *f);
          f->kind = o->opcode == OP_IsNull ? F_ISNULL : F_NOTNULL; f->r = reg[o->p1];
          break;
        }
        /* before a seek: a NULL key means no rows at that level (recorded by qg_open as keyNullExit) */
        Op *n = o + 1;
        while (n < aOp + G->endInit && (n->opcode == OP_Affinity || n->opcode == OP_Variable || n->opcode == OP_IsNull)) n++;
        if (o->opcode != OP_IsNull || !(n->opcode >= OP_SeekLT && n->opcode <= OP_SeekGT) || o->p2 != n->p2) QG_REFUSE("isnull");
        break;
      }
      case OP_Add: case OP_Subtract: if (!qg_expr(G, o)) return 0; break;
      case OP_BeginSubrtn: {
        if (sub) QG_REFUSE("nested subquery");
        int r = qg_subrtn(G, pc); if (!r) return 0;
        pc = r; break;
      }
      case OP_Rewind: case OP_Last:
      case OP_SeekGE: case OP_SeekGT: case OP_SeekLE: case OP_SeekLT:
      case OP_SeekRowid: {
        if (sub || G->aggMode || G->haveOut || P->nlvl >= QG_MAXLVL) QG_REFUSE("level");
        int idx = P->nlvl, open = pc;
        /* LEFT JOIN: Integer 0 rFlag before the level, Integer 1 rFlag at its body, and after the body
           IfPos rFlag <enclosing continue>; NullRow cur; Goto <the Integer 1> */
        if (!qg_open(G, &pc, idx, 0)) return 0;
        P->nlvl++;
        GLevel *L = &P->lvl[idx]; int e = L->exitPc, flag = aOp[pc + 1].p2;
        if (G->nstack > 1 && aOp[pc + 1].opcode == OP_Integer && aOp[pc + 1].p1 == 1 && aOp[e].opcode == OP_IfPos && aOp[e].p1 == flag) {
          int set0 = 0;   /* Integer 0 rFlag in the enclosing level's body, before this level */
          for (int q = P->lvl[G->stack[G->nstack - 2]].head; q < open; q++) if (aOp[q].opcode == OP_Integer && aOp[q].p1 == 0 && aOp[q].p2 == flag) set0 = 1;
          if (!set0 || aOp[e + 1].opcode != OP_NullRow ||
              aOp[e + 1].p1 != aOp[open].p1 || aOp[e + 2].opcode != OP_Goto || aOp[e + 2].p2 != pc + 1) QG_REFUSE("left join");
          if (!qg_point(L)) QG_REFUSE("left join loop");
          L->left = 1; L->leftPc = e;
          pc++;   /* the Integer 1 rFlag */
        }
        break;
      }
      case OP_DeferredSeek: {
        int l = G->lvlOfCur[o->p1]; int k = qg_curslot(P, o->p3);
        if (l < 0 || k < 0 || o->p3 >= 64 || !P->curs[k].isTab || o->p4type != P4_NOTUSED || P->lvl[l].tabCur >= 0) QG_REFUSE("deferredseek");
        P->lvl[l].tabCur = o->p3; P->lvl[l].tabRoot = P->curs[k].root; G->lvlOfCur[o->p3] = (signed char)l; G->tabOfCur[o->p3] = 1;
        break;
      }
      case OP_Column: {
        if (G->sortOut && (o->p1 == G->pseudoCur || o->p1 == G->sortCur)) {   /* a field of a sorted record */
          if (o->p5 || o->p4type != P4_NOTUSED || o->p3 > nMem || o->p2 >= P->nrec || P->rec[o->p2].kind == G_SEQ) QG_REFUSE("sorted column");
          reg[o->p3] = (GSym){G_SREC, 0, (i16)o->p2}; break;
        }
        int l = o->p1 < 64 ? G->lvlOfCur[o->p1] : -1;
        if (l < 0 || (o->p5 & ~(OPFLAG_LENGTHARG | OPFLAG_TYPEOFARG)) || o->p4type != P4_NOTUSED || o->p3 > nMem || o->p2 >= QC_MAXCOL) QG_REFUSE("column");
        GLevel *L = &P->lvl[l];
        if (L->kind == GL_INLIST) { if (o->p2) QG_REFUSE("in-list column"); reg[o->p3] = (GSym){G_INVAL, (i8)l}; }
        else if (L->isIdx && o->p1 == L->cur) reg[o->p3] = (GSym){G_COL, (i8)l, (i16)(o->p2 | 0x100)};   /* 0x100: a field of the index record */
        else if ((L->isIdx && o->p1 == L->tabCur) || (!L->isIdx && o->p1 == L->cur)) reg[o->p3] = (GSym){G_COL, (i8)l, (i16)o->p2};
        else QG_REFUSE("column cursor");
        break;
      }
      case OP_Rowid: case OP_IdxRowid: {
        int l = o->p1 < 64 ? G->lvlOfCur[o->p1] : -1; if (l < 0 || o->p2 > nMem || P->lvl[l].kind == GL_INLIST) QG_REFUSE("rowid");
        if ((o->opcode == OP_IdxRowid) != (P->lvl[l].isIdx && !G->tabOfCur[o->p1])) QG_REFUSE("rowid kind");
        reg[o->p2] = (GSym){G_ROWID, (i8)l};
        break;
      }
      case OP_Copy: case OP_SCopy: {   /* Copy: P3 + 1 registers */
        int n = o->opcode == OP_Copy ? o->p3 + 1 : 1;
        if (o->p2 + n - 1 > nMem || o->p1 + n - 1 > nMem || n < 1) QG_REFUSE("copy");
        for (int k = 0; k < n; k++) reg[o->p2 + k] = reg[o->p1 + k];
        break;
      }
      case OP_Function: case OP_PureFunc: {   /* a deterministic built-in, called through SQLite's implementation */
        sqlite3_context *c = o->p4type == P4_FUNCCTX ? o->p4.pCtx : 0;
        if (!c || c->argc > 4 || P->nfn >= QG_MAXFN || o->p3 > nMem) QG_REFUSE("function");   /* P1: constant-argument mask */
        FuncDef *f = c->pFunc;
        if (!(f->funcFlags & SQLITE_FUNC_CONSTANT) || (f->funcFlags & (SQLITE_FUNC_UNSAFE | SQLITE_FUNC_INTERNAL | SQLITE_FUNC_NEEDCOLL)) || !f->xSFunc)
          QG_REFUSE("function kind");
        GFn *g = &P->fn[P->nfn]; g->f = f; g->argc = c->argc; g->iOp = pc;
        for (int i = 0; i < c->argc; i++) {
          if (!qg_any_sym(&reg[o->p2 + i])) QG_REFUSE("function arg");
          /* a function of a column value (LIKE, upper(), substr() on each row): qvm runs these faster, since a register
             keeps its value's copy and NUL terminator between rows, where a call here builds them every row (speedtest1
             145: 1.19x the interpreter here, 1.03x in qvm; the corpus's LIKE filter -20 % here, -31 % in qvm) */
          if (reg[o->p2 + i].kind == G_COL && f->xSFunc != lengthFunc) QG_REFUSE("function of a column");
          g->arg[i] = reg[o->p2 + i];
        }
        reg[o->p3] = (GSym){G_FUNC, 0, 0, 0, 0, P->nfn++};
        break;
      }
      case OP_Eq: case OP_Ne: case OP_Lt: case OP_Le: case OP_Gt: case OP_Ge: {   /* a filter: jumps to a level's continue */
        int hav = G->inOut && (o->p2 == G->outRet || o->p2 == G->outPc + 1);   /* or HAVING, in the output subroutine */
        int l = hav ? -1 : qg_level_at(G, o->p2);
        if (hav ? P->nhav >= QG_MAXFILT : (l < 0 || P->lvl[l].nfilt >= QG_MAXFILT)) QG_REFUSE("compare target");
        if (o->p4type != P4_NOTUSED && !(o->p4type == P4_COLLSEQ && !sqlite3StrICmp(o->p4.pColl->zName, "BINARY"))) QG_REFUSE("collation");
        if (o->p5 & ~(SQLITE_AFF_MASK | SQLITE_JUMPIFNULL)) QG_REFUSE("compare flags");
        if (!qg_any_sym(&reg[o->p1]) || !qg_any_sym(&reg[o->p3])) QG_REFUSE("compare operand");
        GFilt *f = hav ? &P->hav[P->nhav++] : &P->lvl[l].filt[P->lvl[l].nfilt++]; memset(f, 0, sizeof *f);
        f->kind = F_CMP; f->op = (u8)o->opcode; f->p5 = (u8)o->p5; f->l = reg[o->p1]; f->r = reg[o->p3];
        break;
      }
      case OP_If: case OP_IfNot: {
        int best = -1;   /* bare columns beside min()/max(): captured when that aggregate took the row */
        if (G->grp && o->opcode == OP_If) for (int a = 0; a < P->nagg; a++) if (P->agg[a].flagReg && P->agg[a].flagReg == o->p1) best = a;
        if (G->grp && o->opcode == OP_If && (o->p1 == G->rUse || best >= 0)) {   /* capture bare columns */
          int x = o->p2;
          if (P->ncap || x <= pc || aOp[x].opcode != OP_Integer || aOp[x].p1 != 1 || aOp[x].p2 != G->rUse) QG_REFUSE("group capture");
          for (int a = 0; a < P->nagg; a++) if (a != best && P->agg[a].fn != 1 && best >= 0) QG_REFUSE("capture with two min/max");
          P->capAgg = best;
          for (int q = pc + 1; q < x; q++) {
            Op *c = &aOp[q]; int dst;
            if (c->opcode == OP_Column) dst = c->p3;
            else if (c->opcode == OP_Rowid || c->opcode == OP_IdxRowid) dst = c->p2;
            else QG_REFUSE("group capture op");
            if (!qg_walk(G, q, q + 1, sub)) return 0;
            if (P->ncap >= QC_MAXCOL) QG_REFUSE("captures");
            P->cap[P->ncap] = reg[dst]; reg[dst] = (GSym){G_CAP, 0, (i16)P->ncap++};
          }
          pc = x; break;   /* the Integer 1 rUse */
        }
        if (G->inOut && (o->p2 == G->outRet || o->p2 == G->outPc + 1)) {   /* HAVING: to one of the output's Returns */
          if (P->nhav >= QG_MAXFILT || !qg_any_sym(&reg[o->p1])) QG_REFUSE("having");
          GFilt *f = &P->hav[P->nhav++]; memset(f, 0, sizeof *f);
          f->kind = o->opcode == OP_If ? F_IF : F_IFNOT; f->p3 = (u8)(o->p3 != 0); f->r = reg[o->p1];
          break;
        }
        int l = qg_level_at(G, o->p2);
        if (l < 0 || P->lvl[l].nfilt >= QG_MAXFILT || !qg_any_sym(&reg[o->p1])) QG_REFUSE("if target");
        GFilt *f = &P->lvl[l].filt[P->lvl[l].nfilt++]; memset(f, 0, sizeof *f);
        f->kind = o->opcode == OP_If ? F_IF : F_IFNOT; f->p3 = (u8)(o->p3 != 0); f->r = reg[o->p1];
        break;
      }
      case OP_AggStep: {
        if (o->p4type != P4_FUNCDEF || P->nagg >= QG_MAXAGG || G->sortCur >= 0) QG_REFUSE("aggstep");
        FuncDef *f = o->p4.pFunc; GAgg *a = &P->agg[P->nagg++]; a->reg = o->p3; a->flagReg = 0;
        if (!sqlite3StrICmp(f->zName, "count") && o->p5 == 0) a->fn = 1;
        else if ((!sqlite3StrICmp(f->zName, "min") || !sqlite3StrICmp(f->zName, "max")) && o->p5 == 1 && f->nArg == 1) {
          a->fn = sqlite3StrICmp(f->zName, "min") ? 3 : 2; a->arg = reg[o->p2];
          if (aOp[pc - 1].opcode == OP_CollSeq) { a->flagReg = G->collReg; if (a->flagReg) reg[a->flagReg] = (GSym){G_NONE}; }
          if (!qg_row_sym(&a->arg)) QG_REFUSE("agg arg");
        } else QG_REFUSE("aggregate");
        G->aggMode = 1; break;
      }
      case OP_CollSeq:
        if (o->p4type != P4_COLLSEQ || sqlite3StrICmp(o->p4.pColl->zName, "BINARY") || o->p1 > nMem) QG_REFUSE("collseq");
        G->collReg = o->p1; break;   /* the next min()/max() step sets r[P1] when the row is not the new best */
      case OP_AggFinal: {
        int a = -1; for (int k = 0; k < P->nagg; k++) if (P->agg[k].reg == o->p1) a = k;
        qg_pop_points(G);
        if (a < 0 || G->nstack) QG_REFUSE("aggfinal");
        if (G->aggEndPc < 0) G->aggEndPc = pc;
        reg[o->p1] = (GSym){G_AGG, 0, (i16)a};
        break;
      }
      case OP_ResultRow: {
        int agg = G->aggMode || G->grp;
        int out = G->sortCur >= 0 ? G->sortOut && !G->nstack : G->grp ? G->inOut && !G->nstack : agg ? !G->nstack : G->nstack == P->nlvl;
        if (o->p2 > QC_MAXOUT || G->haveOut || !out) QG_REFUSE("resultrow");
        for (int k = 0; k < o->p2; k++) {
          GSym s = reg[o->p1 + k];
          int ok = G->sortCur >= 0 ? s.kind == G_SREC || qg_value_sym(&s) :
                   agg ? s.kind == G_AGG || s.kind == G_CAP || qg_value_sym(&s) : qg_any_sym(&s) && s.kind != G_AGG && s.kind != G_CAP;
          if (!ok) QG_REFUSE("output");
          P->out[k] = s;
        }
        P->nout = o->p2; G->haveOut = 1;
        break;
      }
      case OP_DecrJumpZero: {
        if (P->hasLimit || reg[o->p1].kind != G_INT || reg[o->p1].v < 1 || !G->haveOut) QG_REFUSE("limit");
        if (G->grp && o->p2 != G->abortPc) QG_REFUSE("group limit");
        P->hasLimit = 1; P->limit = reg[o->p1]; G->limitPc = G->grp ? -1 : o->p2;
        break;
      }
      case OP_Next: case OP_Prev: {
        if (G->sortOut && o->p1 == G->sortCur) {   /* the end of the top-N index's output loop */
          if (o->opcode != OP_Next || o->p2 != G->sortStart + 1) QG_REFUSE("sorted loop");
          G->sortOut = 0; break;
        }
        qg_pop_points(G);   /* point levels inside this loop close here */
        if (!G->nstack) QG_REFUSE("closer");
        GLevel *L = &P->lvl[G->stack[G->nstack - 1]];
        if (o->p1 != L->cur || pc != L->cont) QG_REFUSE("closer order");
        G->nstack--;
        break;
      }
      case OP_IfPos: {   /* a LEFT JOIN level's tail: IfPos; NullRow; Goto (checked when the level opened) */
        int ok = 0;
        for (int l = 0; l < P->nlvl; l++) if (P->lvl[l].left && P->lvl[l].leftPc == pc) ok = 1;
        if (!ok) QG_REFUSE("IfPos");
        pc += 2; break;
      }
      /* ---- GROUP BY (shape checked by qg_group_shape) */
      case OP_Gosub:
        if (!G->grp) QG_REFUSE("Gosub");
        if (o->p1 == G->rReset) break;   /* reset the accumulators: per group at run time */
        if (o->p1 != G->rOut || G->nstack || G->inOut) QG_REFUSE("group output");
        G->aggEndPc = pc;   /* where the loop exits */
        G->inOut = 1; if (!qg_walk(G, G->outPc + 2, G->outRet, 0)) return 0; G->inOut = 0;
        break;
      case OP_Goto:
        if (!G->grp || G->inOut || aOp[o->p2].opcode != OP_Halt || o->p2 <= pc) QG_REFUSE("Goto");
        pc = o->p2 - 1; break;
      case OP_Compare: {
        if (!G->grp || G->ngkSeen || o->p3 < 1 || o->p3 > QG_MAXKEY || (o->p5 & OPFLAG_PERMUTE) || o->p4type != P4_KEYINFO) QG_REFUSE("Compare");
        KeyInfo *ki = o->p4.pKeyInfo;
        for (int k = 0; k < o->p3; k++) {
          if (ki->aColl[k] && sqlite3StrICmp(ki->aColl[k]->zName, "BINARY")) QG_REFUSE("group collation");
          if (!qg_any_sym(&reg[o->p2 + k])) QG_REFUSE("group key");
          P->gk[k] = reg[o->p2 + k];
        }
        if (G->nstack != P->nlvl) QG_REFUSE("group depth");
        P->ngk = o->p3; G->ngkSeen = 1;
        pc = G->bodyPc - 1; break;   /* past the new-group block: the run time does what it does */
      }
      /* ---- ORDER BY */
      case OP_SorterOpen: case OP_OpenEphemeral: {
        if (sub || G->sortCur >= 0 || P->nlvl || o->p1 >= 64 || o->p4type != P4_KEYINFO || o->p2 > QC_MAXCOL) QG_REFUSE("sorter");
        KeyInfo *ki = o->p4.pKeyInfo;
        if (ki->nKeyField < 1 || ki->nKeyField > QG_MAXKEY) QG_REFUSE("sort keys");
        for (int k = 0; k < ki->nKeyField; k++) {
          if (ki->aColl[k] && sqlite3StrICmp(ki->aColl[k]->zName, "BINARY")) QG_REFUSE("sort collation");
          P->sflag[k] = ki->aSortFlags[k];
        }
        G->sortCur = o->p1; G->sortKi = ki; G->sortTop = o->opcode == OP_OpenEphemeral; P->nsk = ki->nKeyField;
        break;
      }
      case OP_Sequence:
        if (o->p1 != G->sortCur || !G->sortTop || o->p2 > nMem) QG_REFUSE("sequence");
        reg[o->p2] = (GSym){G_SEQ}; break;
      case OP_IfNotZero: {   /* top-N: when full, Last; IdxLE (skip the row unless it sorts before the last); Delete */
        GSym n = reg[o->p1];
        int l = G->nstack ? G->stack[G->nstack - 1] : -1;
        if (!G->sortTop || n.kind != G_INT || n.v < 1 || o->p2 != pc + 4 || l < 0 ||
            aOp[pc + 1].opcode != OP_Last || aOp[pc + 1].p1 != G->sortCur ||
            aOp[pc + 2].opcode != OP_IdxLE || aOp[pc + 2].p1 != G->sortCur || aOp[pc + 2].p4type != P4_INT32 || aOp[pc + 2].p4.i != P->nsk ||
            aOp[pc + 3].opcode != OP_Delete || aOp[pc + 3].p1 != G->sortCur) QG_REFUSE("top-n");
        int k = G->nstack - 1; while (k >= 0 && qg_point(&P->lvl[G->stack[k]])) k--;   /* the innermost loop */
        if (k < 0 || aOp[pc + 2].p2 != P->lvl[G->stack[k]].cont) QG_REFUSE("top-n skip");
        P->topN = n.v; pc += 3; break;
      }
      case OP_MakeRecord: {   /* a row into the sorter */
        Op *ins = o + 1;
        if (G->sortCur < 0 || P->nrec || o->p2 < P->nsk || o->p2 > QC_MAXCOL || o->p4type != P4_NOTUSED || o->p5 ||
            !((ins->opcode == OP_SorterInsert && !G->sortTop) || (ins->opcode == OP_IdxInsert && G->sortTop)) ||
            ins->p1 != G->sortCur || ins->p2 != o->p3 || G->nstack != P->nlvl || !P->nlvl) QG_REFUSE("sort record");
        if (G->sortTop && (!P->topN || reg[o->p1 + P->nsk].kind != G_SEQ)) QG_REFUSE("top-n record");
        for (int k = 0; k < o->p2; k++) {
          GSym s = reg[o->p1 + k];
          if (s.kind != G_SEQ && (!qg_any_sym(&s) || s.kind == G_AGG)) QG_REFUSE("sort field");
          if (s.kind == G_SEQ && k != P->nsk) QG_REFUSE("sequence field");
          P->rec[k] = s;
        }
        P->nrec = o->p2; pc++; break;
      }
      case OP_OpenPseudo:
        qg_pop_points(G);   /* point levels around the loop end here too */
        if (G->sortTop || G->sortCur < 0 || !P->nrec || G->nstack || o->p1 >= 64) QG_REFUSE("pseudo");
        G->pseudoCur = o->p1; G->sortStart = pc; break;
      case OP_SorterSort: case OP_Sort: {
        int top = o->opcode == OP_Sort;
        qg_pop_points(G);
        if (o->p1 != G->sortCur || top != G->sortTop || !P->nrec || G->nstack || (!top && G->pseudoCur < 0)) QG_REFUSE("sort");
        if (top) G->sortStart = pc;
        G->sortExit = o->p2; G->sortOut = 1;
        if (!top) {   /* SorterData into the pseudo cursor, then the loop body */
          Op *d = o + 1; if (d->opcode != OP_SorterData || d->p1 != G->sortCur || d->p3 != G->pseudoCur) QG_REFUSE("sorter data");
          pc++;
        }
        break;
      }
      case OP_SorterNext:
        if (!G->sortOut || o->p1 != G->sortCur || o->p2 != G->sortStart + 2) QG_REFUSE("sorter next");
        G->sortOut = 0; break;
      case OP_Halt:
        if (o->p1 || o->p2) QG_REFUSE("halt");
        G->haltPc = pc; return 1;
      default: qc_why = sqlite3OpcodeName(o->opcode); return 0;
    }
  }
  return 1;
}
static void qg_need(QcGPlan *P, const GSym *s) {   /* the record fields a symbol reads */
  if (s->kind == G_COL) {
    GLevel *L = &P->lvl[s->lvl];
    if (s->col & 0x100) L->needIdx |= 1u << (s->col & 0xff); else L->needTab |= 1u << s->col;
  } else if (s->kind == G_ROWID && P->lvl[s->lvl].isIdx) P->lvl[s->lvl].needIdx |= 1u << (P->lvl[s->lvl].ki->nAllField - 1);
  else if (s->kind == G_FUNC) { const GFn *f = &P->fn[s->a]; for (int i = 0; i < f->argc; i++) qg_need(P, &f->arg[i]); }
}

/* does s read level l's table record (directly, or through a function or a subquery's keys and filters)? */
static int qg_reads_tab(const QcGPlan *P, const GSym *s, int l) {
  if (s->kind == G_COL) return s->lvl == l && !(s->col & 0x100);
  if (s->kind == G_FUNC) { const GFn *f = &P->fn[s->a]; for (int i = 0; i < f->argc; i++) if (qg_reads_tab(P, &f->arg[i], l)) return 1; return 0; }
  if (s->kind == G_SUBQ) {
    const GLevel *S = &P->lvl[s->col];
    for (int j = 0; j < S->nkey; j++) if (qg_reads_tab(P, &S->key[j], l)) return 1;
    for (int j = 0; j < S->nend; j++) if (qg_reads_tab(P, &S->end[j], l)) return 1;
    if (S->kind == GL_ROWID && qg_reads_tab(P, &S->rowidKey, l)) return 1;
    for (int f = 0; f < S->nfilt; f++) if (qg_reads_tab(P, &S->filt[f].l, l) || qg_reads_tab(P, &S->filt[f].r, l)) return 1;
  }
  return 0;
}
static int qg_same(const GSym *a, const GSym *b) { return a->kind == b->kind && a->lvl == b->lvl && a->col == b->col && a->v == b->v && a->z == b->z && a->a == b->a && a->b == b->b; }
static void qg_modes(QcGPlan *P) {
  if (!P->nlvl) return;
  GLevel *L = &P->lvl[P->nlvl - 1];
  /* the innermost index level with a deferred table row and filters on the index alone: batch the table seeks */
  if (L->isIdx && L->tabCur >= 0 && !L->left && !L->once && L->kind != GL_INLIST) {
    int ok = 1;
    for (int f = 0; f < L->nfilt && ok; f++) if (qg_reads_tab(P, &L->filt[f].l, P->nlvl - 1) || qg_reads_tab(P, &L->filt[f].r, P->nlvl - 1)) ok = 0;
    L->batch = (u8)ok;
  }
  /* count(*) ... WHERE indexed = ?, as the query or as a correlated subquery: count the cells */
  for (int l = 0; l < QG_NLVL; l++) {
    L = &P->lvl[l];
    if (!(L->kind == GL_SEEK && L->isIdx && L->tabCur < 0 && !L->nfilt && L->seekOp == OP_SeekGE && L->endOp == OP_IdxGT &&
          L->nkey == 1 && L->nend == 1 && qg_same(&L->key[0], &L->end[0]))) continue;
    if (L->subKind == 1) { L->cntEq = 1; continue; }
    if (l || P->nlvl != 1 || !P->nagg) continue;
    int all = 1; for (int a = 0; a < P->nagg; a++) if (P->agg[a].fn != 1) all = 0;
    L->cntEq = (u8)all;
  }
}

/* GROUP BY over rows in key order, as SQLite emits it (select.c, "the GROUP BY processing loop"):
     Compare rPrev rKey n; Jump A B A
   A: Gosub rOut OUT; Move rKey rPrev n; IfPos rAbort <halt>; Gosub rReset RESET
   B: AggStep ...; If rUse X; <captures>; X: Integer 1 rUse; ... Next
      Gosub rOut OUT; Goto <halt>
      Integer 1 rAbort; Return rOut                          (LIMIT reached: the abort)
   OUT: IfPos rUse +2; Return rOut; AggFinal ...; <HAVING jumps to the Return>; ResultRow; [DecrJumpZero abort]; Return rOut
   RESET: Null ...; Integer 0 rUse; Return rReset
   Sets the lifter's group fields; 1 = not a GROUP BY or the shape holds, 0 = refused */
static int qg_group_shape(GLift *G) {
  Op *aOp = G->aOp; int c = -1;
  for (int q = 1; q + 1 < G->endInit; q++) if (aOp[q].opcode == OP_Compare && aOp[q + 1].opcode == OP_Jump) { c = q; break; }
  if (c < 0) return 1;
  Op *cm = &aOp[c], *j = &aOp[c + 1]; int A = c + 2, B = j->p2;
  if (j->p1 != A || j->p3 != A || B != A + 4) QG_REFUSE("group jump");
  Op *go = &aOp[A], *mv = &aOp[A + 1], *ab = &aOp[A + 2], *gr = &aOp[A + 3];
  if (go->opcode != OP_Gosub || mv->opcode != OP_Move || mv->p1 != cm->p2 || mv->p2 != cm->p1 || mv->p3 != cm->p3 ||
      ab->opcode != OP_IfPos || aOp[ab->p2].opcode != OP_Halt || gr->opcode != OP_Gosub) QG_REFUSE("group new");
  int out = go->p2, rOut = go->p1, rAbort = ab->p1, reset = gr->p2, rReset = gr->p1;
  if (out <= B || out + 2 >= G->endInit || aOp[out].opcode != OP_IfPos || aOp[out].p2 != out + 2 ||
      aOp[out + 1].opcode != OP_Return || aOp[out + 1].p1 != rOut) QG_REFUSE("group output");
  int ret = -1; for (int q = out + 2; q < G->endInit; q++) if (aOp[q].opcode == OP_Return) { ret = q; break; }
  if (ret < 0 || aOp[ret].p1 != rOut) QG_REFUSE("group output end");
  int rUse = aOp[out].p1, sawUse = 0, q = reset;
  for (; q < G->endInit && aOp[q].opcode != OP_Return; q++) {
    if (aOp[q].opcode == OP_Null && !aOp[q].p1) continue;
    if (aOp[q].opcode == OP_Integer && !aOp[q].p1 && aOp[q].p2 == rUse) { sawUse = 1; continue; }
    QG_REFUSE("group reset");
  }
  if (q >= G->endInit || aOp[q].p1 != rReset || !sawUse) QG_REFUSE("group reset end");
  int abortPc = -1;
  for (int k = 1; k + 1 < G->endInit; k++)
    if (aOp[k].opcode == OP_Integer && aOp[k].p1 == 1 && aOp[k].p2 == rAbort && aOp[k + 1].opcode == OP_Return && aOp[k + 1].p1 == rOut) abortPc = k;
  G->grp = 1; G->rOut = rOut; G->rReset = rReset; G->rUse = rUse; G->outPc = out; G->outRet = ret; G->abortPc = abortPc; G->bodyPc = B;
  return 1;
}

static int qc_lift_general(Vdbe *v) {
  Op *aOp = v->aOp; int nOp = v->nOp;
  if (v->nMem > 255) QG_REFUSE("registers");
  if (aOp[0].opcode != OP_Init) QG_REFUSE("init");
  GLift G; memset(&G, 0, sizeof G);   /* (on the stack: about 11 KB, once per lift) */
  QcGPlan *P = sqlite3MallocZero(sizeof *P); if (!P) return 0;
  G.P = P; G.aOp = aOp; G.nMem = v->nMem; G.haltPc = G.limitPc = G.aggEndPc = -1; G.endInit = aOp[0].p2;
  memset(G.lvlOfCur, -1, sizeof G.lvlOfCur);
  G.sortCur = G.pseudoCur = -1;
  P->kind = 2; P->iDb = -1; P->capAgg = -1;
  const char *bad = 0;
  /* init block: Transaction, then constants, then Goto 1 */
  int t = aOp[0].p2, ntx = 0;
  for (; t < nOp && !bad; t++) {
    Op *o = &aOp[t];
    if (o->opcode == OP_Transaction) {
      if (o->p2 != 0 || o->p5 != 1 || ntx++ || o->p4type != P4_INT32) bad = "transaction";
      P->iDb = o->p1; P->cookie = o->p3; P->gen = o->p4.i;
    } else if (o->opcode == OP_Goto && o->p2 == 1) break;
    else if (!qg_constop(&G, o)) bad = "init op";
  }
  if (!bad && (!ntx || t >= nOp)) bad = "preamble";
  if (bad) { qg_unlift(P, v->db); QG_REFUSE(bad); }
  if (!qg_group_shape(&G) || !qg_walk(&G, 1, G.endInit, 0)) { qg_unlift(P, v->db); return 0; }
  if (G.haltPc < 0 || !G.haveOut) bad = "no result";
  if (!bad && G.grp && !P->ngk) bad = "group keys";
  if (!bad && G.sortCur >= 0 && (!P->nrec || G.sortOut || G.sortExit != G.haltPc)) bad = "sort shape";
  /* each level exits to its enclosing loop's continue address (through point levels, which have no loop, to theirs);
     the outermost to Halt (or AggFinal), a LEFT JOIN level to its IfPos, whose own jump goes to that enclosing
     continue. A point level's own continue is its exit. LIMIT exits to Halt. */
  if (!bad && G.limitPc >= 0 && G.limitPc != G.haltPc) bad = "limit target";
  for (int l = 0; l < P->nlvl && !bad; l++) {
    GLevel *L = &P->lvl[l];
    int e = l - 1; while (e >= 0 && qg_point(&P->lvl[e])) e--;
    int want = e < 0 ? (G.aggMode || G.grp ? G.aggEndPc : G.sortCur >= 0 ? G.sortStart : G.haltPc) : P->lvl[e].cont;
    if (L->left) { if (aOp[L->leftPc].p2 != want) bad = "left join exit"; want = L->leftPc; }
    if (L->exitPc != want) bad = "level exits";
    if (qg_point(L)) L->cont = L->exitPc;
  }
  for (int l = 0; l < QG_NLVL && !bad; l++)
    if (P->lvl[l].kind && P->lvl[l].isIdx && !(P->uk[l] = qc_unpacked_keep(P->lvl[l].ki))) bad = "oom";
  /* what each level decodes, and the decoder for that record shape (from the schema) */
  if (!bad) {
    for (int k = 0; k < P->nout; k++) qg_need(P, &P->out[k]);
    for (int a = 0; a < P->nagg; a++) if (P->agg[a].fn != 1) qg_need(P, &P->agg[a].arg);
    for (int k = 0; k < P->ngk; k++) qg_need(P, &P->gk[k]);
    for (int k = 0; k < P->ncap; k++) qg_need(P, &P->cap[k]);
    for (int k = 0; k < P->nrec; k++) qg_need(P, &P->rec[k]);
    for (int l = 0; l < QG_NLVL; l++) {
      GLevel *L = &P->lvl[l]; if (!L->kind) continue;
      for (int f = 0; f < L->nfilt; f++) { qg_need(P, &L->filt[f].l); qg_need(P, &L->filt[f].r); }
      for (int j = 0; j < L->nkey; j++) qg_need(P, &L->key[j]);
      for (int j = 0; j < L->nend; j++) qg_need(P, &L->end[j]);
      if (L->kind == GL_ROWID) qg_need(P, &L->rowidKey);
      if (L->isIdx && L->tabCur >= 0) L->needIdx |= 1u << (L->ki->nAllField - 1);   /* the deferred seek reads the rowid */
    }
    Schema *sc = v->db->aDb[P->iDb].pSchema;
    for (int l = 0; l < QG_NLVL && !bad; l++) {
      GLevel *L = &P->lvl[l]; Index *ix; Table *tb;
      if (!L->kind || L->kind == GL_INLIST) continue;
      if (L->needIdx >> QC_MAXCOL || L->needTab >> QC_MAXCOL) { bad = "wide record"; break; }
      if (L->isIdx && (tb = qc_find(sc, L->root, &ix)) && ix) { qc_shape(&L->idxShape, tb, ix, L->needIdx, -1); L->idxDec = qc_dec_pick(&L->idxShape); }
      Pgno tr = L->isIdx ? (L->tabCur >= 0 ? L->tabRoot : 0) : L->root;
      if (tr && (tb = qc_find(sc, tr, &ix)) && !ix) { qc_shape(&L->tabShape, tb, 0, L->needTab, -1); L->tabDec = qc_dec_pick(&L->tabShape); }
      /* a record read by the generic decoder: the filters' columns alone first, so a row the filters skip has only
         those decoded, as OP_Column would (vaultwarden's devices by refresh_token, 10 columns over a full scan) */
      if (!L->tabDec && !L->tabShape.ok && L->nfilt) {
        u32 keep[QG_NLVL]; for (int k = 0; k < QG_NLVL; k++) { keep[k] = P->lvl[k].needTab; P->lvl[k].needTab = 0; }
        for (int f = 0; f < L->nfilt; f++) { qg_need(P, &L->filt[f].l); qg_need(P, &L->filt[f].r); }
        u32 m = L->needTab; for (int k = 0; k < QG_NLVL; k++) P->lvl[k].needTab = keep[k];
        if (m && m != L->needTab) L->fltTab = m;
      }
    }
  }
  if (!bad) qg_modes(P);
  /* a full scan of a table without LIMIT: per-row work dominates, and qvm does it cheaper (count(*) ... WHERE b > 0 over
     50k rows: 1.04x the interpreter here, 0.87x in qvm); loop plans win where an index seek or a LIMIT bounds the rows.
     A LIMIT doesn't bound a scan whose filters skip rows (vaultwarden's devices by refresh_token, LIMIT 1 after 42
     rows: 36.0k instructions here, 32.6k in qvm, 31.6k the interpreter) */
  if (!bad && P->lvl[0].kind == GL_SCAN && (!P->hasLimit || P->lvl[0].nfilt)) bad = "full scan";
  /* E1: nor a scan's first N rows on their own (one level, no aggregate, grouping, sort or subquery): qvm's region reads
     each row once, where the loop plan pays its per-row machinery (datasette table.page_default, 101 rows of a table: 1.135
     of stock here, 0.919 in qvm, x86); a join or an aggregate over the scan stays (docs/TOUGH_PROBLEMS.md E1) */
  if (!bad && P->lvl[0].kind == GL_SCAN && P->nlvl == 1 && !P->nsub && !P->nagg && !P->ngk && !P->nrec) bad = "scan limit";
  /* a point lookup's shape (one level, a rowid or index seek, no aggregate, grouping, sort or subquery): one that hands
     out at most one row is demoted to qvm at its next run (qg_exec), which runs it cheaper (box2 corpus6, 1c5891d: zotero
     creators.id_from_data 1.215 of stock here, 1.097 in qvm; locally 11 of 11 single-row lookups 1-11 % fewer
     instructions in qvm); loops, joins, aggregates and seeks of several rows stay (uptime_kuma important_heartbeat_count
     0.726 here, 0.953 in qvm; vaultwarden favorites.cipher_uuids_by_user, 12 rows: 4 % more in qvm) */
  if (!bad) P->lookup = P->nlvl == 1 && !P->nsub && !P->nagg && !P->ngk && !P->nrec &&
                        (P->lvl[0].kind == GL_ROWID || (P->lvl[0].kind == GL_SEEK && !P->lvl[0].cntEq));
  if (!bad && ((QgConn *)QCD(v->db)->pQgConn)->nplans >= QC_MAXPLANS) bad = "plans";
  if (bad) { qg_unlift(P, v->db); QG_REFUSE(bad); }
  qg_sig(v, P);   /* (none: the plan isn't cached) */
  QCV(v)->pQc = P; QCV(v)->qcWant = 1; QC_COUNT_ADD(((QgConn *)QCD(v->db)->pQgConn)->nplans, P);
  return 1;
}

/* ---------------------------------------------------------------- run time */
typedef struct QgRun {
  QcVal out[QG_MAXROWS * QC_MAXOUT]; int nrow;
  DbPage *pins[QG_MAXROWS * 4]; int npin;
  QcVal bidx[QC_MAXROWS][QC_MAXCOL]; i64 bkey[QC_MAXROWS]; qc_res bres[QC_MAXROWS];   /* a batch: index values, rowids, table rows */
  QcVal col[QG_NLVL][2][QC_MAXCOL]; u8 have[QG_NLVL][2]; u8 nullrow[QG_NLVL];   /* [0] table record, [1] index record */
  QcVal inv[QG_NLVL][QG_MAXIN]; int ini[QG_NLVL];                                /* IN-list values (sorted) and position */
  i64 aggCnt[QG_MAXAGG]; QcVal aggV[QG_MAXAGG]; u8 aggSet[QG_MAXAGG];
  QcVal gkv[QG_MAXKEY], capv[QC_MAXCOL]; int ingrp; u8 took[QG_MAXAGG];         /* GROUP BY: the open group */
  QcVal srow[QG_MAXROWS][QC_MAXCOL], stmp[QG_MAXROWS][QC_MAXCOL]; int sidx[QG_MAXROWS], sortt[QG_MAXROWS], nsrow, cut;   /* ORDER BY: records; cut: top-N cut to N once (srow[topN - 1] is the worst kept) */
  i64 limit;
  int arenaUsed; char arena[1 << 16];                                            /* function results */
} QgRun;
static void qg_conn_free(QgConn *GC) { sqlite3_free(GC->run); GC->run = 0; }   /* (sqlite3QcClose) */

static int qg_cursor(QcGPlan *P, Btree *bt, int slot, BtCursor **out) {
  if (P->bopen[slot]) { *out = P->bc[slot]; return 0; }
  if (!P->bc[slot] && !(P->bc[slot] = sqlite3MallocZero(sqlite3BtreeCursorSize()))) return 1;
  sqlite3BtreeCursorZero(P->bc[slot]);
  if (sqlite3BtreeCursor(bt, P->curs[slot].root, 0, P->curs[slot].ki, P->bc[slot]) != SQLITE_OK) return 1;
  P->bopen[slot] = 1; P->bt = bt; *out = P->bc[slot]; return 0;
}
static int qg_pin(QgRun *R, BtCursor *c) {   /* 0: no room (values would point into an unpinned page) */
  DbPage *pg = c->pPage->pDbPage;
  for (int k = R->npin - 1; k >= 0 && k >= R->npin - 4; k--) if (R->pins[k] == pg) return 1;
  if (R->npin >= (int)(sizeof R->pins / sizeof *R->pins)) return 0;
  sqlite3PagerRef(pg); R->pins[R->npin++] = pg; return 1;
}
/* value of a value-kind symbol; 0 = a type the bytecode would convert, or an overflow */
static int qg_eval(QcGPlan *P, Vdbe *v, const QcOut *O, const GSym *s, QcVal *o) {
  switch (s->kind) {
    case G_INT: o->type = SQLITE_INTEGER; o->i = s->v; return 1;
    case G_NULL: o->type = SQLITE_NULL; o->n = 0; return 1;
    case G_TEXT: o->type = SQLITE_TEXT; o->z = (const u8 *)s->z; o->n = (int)s->v; return 1;
    case G_PARAM: {
      if (O && O->param && s->v <= O->nparam) { o->type = SQLITE_INTEGER; o->i = O->param[s->v - 1]; return 1; }
      Mem *m = &v->aVar[s->v - 1];
      if (m->flags & MEM_Null) { o->type = SQLITE_NULL; o->n = 0; return 1; }
      if ((m->flags & (MEM_Int | MEM_Real | MEM_Str | MEM_Blob)) == MEM_Int) { o->type = SQLITE_INTEGER; o->i = m->u.i; return 1; }
      if ((m->flags & (MEM_Int | MEM_Real | MEM_Str | MEM_Blob)) == MEM_Str && m->enc == SQLITE_UTF8) {
        o->type = SQLITE_TEXT; o->z = (const u8 *)m->z; o->n = m->n; return 1; }
      return 0;
    }
    case G_ADD: case G_SUB: {
      QcVal a, b; if (!qg_eval(P, v, O, &P->ex[s->a], &a) || !qg_eval(P, v, O, &P->ex[s->b], &b)) return 0;
      if (a.type == SQLITE_NULL || b.type == SQLITE_NULL) { o->type = SQLITE_NULL; o->n = 0; return 1; }
      if (a.type != SQLITE_INTEGER || b.type != SQLITE_INTEGER) return 0;   /* text arithmetic: SQLite's conversions */
      i64 r; if (s->kind == G_ADD ? __builtin_add_overflow(a.i, b.i, &r) : __builtin_sub_overflow(a.i, b.i, &r)) return 0;
      o->type = SQLITE_INTEGER; o->i = r; return 1;
    }
  }
  return 0;
}
/* apply a key's affinity the way OP_Affinity would, or refuse when it would change the value's type */
static int qg_aff_ok(char aff, const QcVal *x) {
  if (x->type == SQLITE_NULL) return 1;
  switch (aff) {
    case 0: case SQLITE_AFF_BLOB: case SQLITE_AFF_NONE: return 1;
    case SQLITE_AFF_TEXT: return x->type == SQLITE_TEXT;
    case SQLITE_AFF_INTEGER: case SQLITE_AFF_NUMERIC: case SQLITE_AFF_REAL: return x->type == SQLITE_INTEGER;
  }
  return 0;
}
static void qg_mem(Mem *m, const QcVal *x) {
  memset(m, 0, sizeof *m); m->enc = SQLITE_UTF8;   /* (a blob too: a function reading it as text must not take it for UTF-16) */
  if (x->type == SQLITE_INTEGER) { m->flags = MEM_Int; m->u.i = x->i; }
  else if (x->type == SQLITE_TEXT) { m->flags = MEM_Str | MEM_Ephem; m->z = (char *)x->z; m->n = x->n; m->enc = SQLITE_UTF8; }
  else if (x->type == SQLITE_FLOAT) { m->flags = MEM_Real; m->u.r = x->r; }
  else if (x->type == SQLITE_BLOB) { m->flags = MEM_Blob | MEM_Ephem; m->z = (char *)x->z; m->n = x->n; }
  else m->flags = MEM_Null;
}
/* a seek key's value into its UnpackedRecord Mem: only what sqlite3VdbeRecordCompare reads (the rest zeroed once,
   qc_unpacked_keep); qg_mem's memset of each Mem cost a small lookup about 30 instructions a key */
static inline void qg_key(Mem *m, const QcVal *x) {
  m->enc = SQLITE_UTF8;
  if (x->type == SQLITE_INTEGER) { m->flags = MEM_Int; m->u.i = x->i; }
  else if (x->type == SQLITE_TEXT) { m->flags = MEM_Str | MEM_Ephem; m->z = (char *)x->z; m->n = x->n; }
  else if (x->type == SQLITE_FLOAT) { m->flags = MEM_Real; m->u.r = x->r; }
  else if (x->type == SQLITE_BLOB) { m->flags = MEM_Blob | MEM_Ephem; m->z = (char *)x->z; m->n = x->n; }
  else m->flags = MEM_Null;
}
/* decode what the row needs at level l: the table record ([0]) or the index record ([1]); 0 = fallback */
/* have: 1 all of need, 2 (the table record) only the filters' columns; col: the column wanted, -1 any */
static int qg_fetch(QcGPlan *P, QgRun *R, BtCursor *c, int which, int l, int col) {
  GLevel *L = &P->lvl[l]; u8 h = R->have[l][which];
  if (h == 1 || (h == 2 && col >= 0 && (L->fltTab >> col & 1))) return 1;
  u32 need = which ? L->needIdx : L->needTab;
  if (!which && !h && col >= 0 && (L->fltTab >> col & 1)) need = L->fltTab;   /* (fltTab set: no decoder, no shape) */
  int (*dec)(const u8 *, u32, QcVal *) = which ? L->idxDec : L->tabDec; const QcShape *sh = which ? &L->idxShape : &L->tabShape;
  u32 sz = sqlite3BtreePayloadSize(c), amt; const u8 *rec = sqlite3BtreePayloadFetch(c, &amt);
  if (amt < sz || !((dec && dec(rec, sz, R->col[l][which])) || (!dec && sh->ok && qc_decode_fast(rec, sz, sh, need, R->col[l][which])) ||   /* (a shape with no generated decoder: a REAL column) */
                    qc_decode(rec, sz, need, R->col[l][which]))) return 0;
  if (!qg_pin(R, c)) return 0;
  R->have[l][which] = need == (which ? L->needIdx : L->needTab) ? 1 : 2; return 1;
}
typedef struct QgCtx {
  QcGPlan *P; Vdbe *v; const QcOut *O; Btree *bt; QgRun *R; BtCursor *cur[QG_NLVL], *tab[QG_NLVL];
  int err, done, over; i64 subCnt; const QcVal *srow;   /* over: more rows than a run buffers (the statement moves to qvm) */
} QgCtx;
static int qg_val(QgCtx *X, const GSym *s, QcVal *o);
static int qg_level(QgCtx *X, int l);
/* a deterministic built-in function, through SQLite's own implementation; a text result is copied to the arena */
static int qg_func(QgCtx *X, const GFn *g, QcVal *o) {
  if (g->f->xSFunc == lengthFunc && g->argc == 1) {   /* length() of text or a blob natively (lengthFunc's count) */
    QcVal a; if (!qg_val(X, &g->arg[0], &a)) return 0;
    if (a.type == SQLITE_NULL) { o->type = SQLITE_NULL; o->n = 0; return 1; }
    if (a.type == SQLITE_BLOB) { o->type = SQLITE_INTEGER; o->i = a.n; return 1; }
    if (a.type == SQLITE_TEXT) { o->type = SQLITE_INTEGER; o->i = qc_utf8_chars(a.z, a.n); return 1; }
  }
  sqlite3 *db = X->v->db; Mem arg[4], res; int n = 0, ok = 0;
  union { sqlite3_context c; char b[sizeof(sqlite3_context) + 4 * sizeof(sqlite3_value *)]; } u;
  memset(&u, 0, sizeof u);
  for (; n < g->argc; n++) {
    QcVal a; if (!qg_val(X, &g->arg[n], &a)) goto done;
    qg_mem(&arg[n], &a); arg[n].db = db; u.c.argv[n] = &arg[n];
  }
  sqlite3VdbeMemInit(&res, db, MEM_Null);
  u.c.pOut = &res; u.c.pFunc = g->f; u.c.pVdbe = X->v; u.c.iOp = g->iOp; u.c.enc = SQLITE_UTF8; u.c.argc = (u16)g->argc;
  g->f->xSFunc(&u.c, g->argc, u.c.argv);
  if (!u.c.isError && !db->mallocFailed) {
    if (res.flags & MEM_Null) { o->type = SQLITE_NULL; o->n = 0; ok = 1; }
    else if ((res.flags & (MEM_Int | MEM_Real | MEM_Str | MEM_Blob)) == MEM_Int) { o->type = SQLITE_INTEGER; o->i = res.u.i; ok = 1; }
    else if ((res.flags & (MEM_Int | MEM_Real | MEM_Str | MEM_Blob)) == MEM_Str && res.enc == SQLITE_UTF8) {
      QgRun *R = X->R;   /* 16 bytes of slack after each: qc_jstr's vector loads */
      if (R->arenaUsed + res.n + 16 <= (int)sizeof R->arena) {
        memcpy(R->arena + R->arenaUsed, res.z, (size_t)res.n);
        o->type = SQLITE_TEXT; o->z = (const u8 *)R->arena + R->arenaUsed; o->n = res.n; R->arenaUsed += res.n; ok = 1;
      }
    }   /* REAL, BLOB: SQLite's path */
  }
  sqlite3VdbeMemRelease(&res);
done:
  for (int i = 0; i < n; i++) sqlite3VdbeMemRelease(&arg[i]);
  return ok;
}
/* value of a row symbol at the current position; 0 = fallback */
static int qg_rowval(QgCtx *X, const GSym *s, QcVal *o) {
  QgRun *R = X->R; int l = s->lvl;
  switch (s->kind) {
    case G_SUBQ: {   /* run the subquery's level: count its rows, or stop at the first for EXISTS */
      i64 save = X->subCnt; X->subCnt = 0;
      qg_level(X, s->col);
      if (X->err) return 0;
      o->type = SQLITE_INTEGER; o->i = X->P->lvl[s->col].subKind == 2 ? X->subCnt > 0 : X->subCnt;
      X->subCnt = save; return 1;
    }
    case G_FUNC: return qg_func(X, &X->P->fn[s->a], o);
    case G_INVAL: *o = R->inv[l][R->ini[l]]; return 1;
    case G_AGG: {
      const GAgg *g = &X->P->agg[s->col];
      if (g->fn == 1) { o->type = SQLITE_INTEGER; o->i = R->aggCnt[s->col]; }
      else if (R->aggSet[s->col]) *o = R->aggV[s->col]; else { o->type = SQLITE_NULL; o->n = 0; }
      return 1;
    }
    case G_CAP: *o = R->capv[s->col]; return 1;
    case G_SREC: *o = X->srow[s->col]; return 1;
  }
  GLevel *L = &X->P->lvl[l];
  if (R->nullrow[l]) { o->type = SQLITE_NULL; o->n = 0; return 1; }   /* a LEFT JOIN's NULL row */
  if (s->kind == G_ROWID) {
    if (L->isIdx) {   /* IdxRowid: the index record's last field */
      if (!qg_fetch(X->P, R, X->cur[l], 1, l, -1)) return 0;
      int nf = L->ki->nAllField - 1; QcVal *f = &R->col[l][1][nf];
      if (nf >= QC_MAXCOL || f->type != SQLITE_INTEGER) return 0;
      *o = *f; return 1;
    }
    o->type = SQLITE_INTEGER; o->i = sqlite3BtreeIntegerKey(X->cur[l]); return 1;
  }
  if (s->col & 0x100) { if (!qg_fetch(X->P, R, X->cur[l], 1, l, -1)) return 0; *o = R->col[l][1][s->col & 0xff]; return 1; }
  BtCursor *c = L->isIdx ? X->tab[l] : X->cur[l];
  if (L->isIdx && !R->have[l][0]) {   /* the deferred seek of the table row behind the index entry */
    QcVal rid; GSym rs = {G_ROWID, (i8)l}; if (!qg_rowval(X, &rs, &rid)) return 0;
    int res; if (sqlite3BtreeTableMoveto(c, rid.i, 0, &res) != SQLITE_OK || res != 0) return 0;
  }
  if (!qg_fetch(X->P, R, c, 0, l, s->col)) return 0;
  *o = R->col[l][0][s->col]; return 1;
}
static int qg_val(QgCtx *X, const GSym *s, QcVal *o) { return qg_row_sym(s) ? qg_rowval(X, s, o) : qg_eval(X->P, X->v, X->O, s, o); }
/* vdbe compare semantics for INTEGER/TEXT (BINARY) and NULL: 1 jump, 0 no jump, -1 fallback */
static int qg_cmp_jump(u8 op, u8 p5, const QcVal *r3, const QcVal *r1) {
  if (r3->type == SQLITE_NULL || r1->type == SQLITE_NULL) return (p5 & SQLITE_JUMPIFNULL) ? 1 : 0;
  int c;
  if (r3->type == SQLITE_INTEGER && r1->type == SQLITE_INTEGER) c = r3->i < r1->i ? -1 : r3->i > r1->i;
  else if (r3->type == SQLITE_TEXT && r1->type == SQLITE_TEXT && (p5 & SQLITE_AFF_MASK) <= SQLITE_AFF_TEXT) {
    int n = r3->n < r1->n ? r3->n : r1->n; c = memcmp(r3->z, r1->z, (size_t)n); if (!c) c = r3->n - r1->n;
  } else return -1;
  switch (op) { case OP_Eq: return c == 0; case OP_Ne: return c != 0; case OP_Lt: return c < 0; case OP_Le: return c <= 0; case OP_Gt: return c > 0; default: return c >= 0; }
}
/* OP_If / OP_IfNot on an INTEGER, or NULL (jumps when P3 is set): 1 jump, 0 no jump, -1 fallback */
static int qg_if_jump(const GFilt *f, const QcVal *x) {
  if (x->type == SQLITE_NULL) return f->p3;
  if (x->type != SQLITE_INTEGER) return -1;
  return f->kind == F_IF ? x->i != 0 : x->i == 0;
}
/* the row at level l is positioned: filters, then the next level or the output. 1 continue, 0 stop */
static int qg_emit(QgCtx *X, int l);
static int qg_filters(QgCtx *X, const GFilt *flt, int n) {   /* 1 the row passes, 0 skip it, -1 fallback */
  for (int f = 0; f < n; f++) {
    const GFilt *g = &flt[f]; QcVal a, b; int j;
    if (!qg_val(X, &g->r, &a)) return -1;
    switch (g->kind) {
      case F_CMP: if (!qg_val(X, &g->l, &b)) return -1; j = qg_cmp_jump(g->op, g->p5, &a, &b); break;
      case F_ISNULL: j = a.type == SQLITE_NULL; break;
      case F_NOTNULL: j = a.type != SQLITE_NULL; break;
      default: j = qg_if_jump(g, &a);
    }
    if (j) return j < 0 ? -1 : 0;
  }
  return 1;
}
static int qg_filter(QgCtx *X, int l) { return qg_filters(X, X->P->lvl[l].filt, X->P->lvl[l].nfilt); }
static int qg_row(QgCtx *X, int l) {
  QgRun *R = X->R; GLevel *L = &X->P->lvl[l];
  R->have[l][0] = R->have[l][1] = 0;
  int f = qg_filter(X, l);
  if (f < 0) { X->err = 1; return 0; }
  if (!f) return 1;   /* skip this row */
  if (L->subKind) { X->subCnt++; return L->subKind != 2; }   /* EXISTS: the first row decides */
  return qg_emit(X, l);
}
static int qg_aggstep(QgCtx *X) {   /* one row into the aggregates; 0 = fallback */
  QcGPlan *P = X->P; QgRun *R = X->R;
  for (int a = 0; a < P->nagg; a++) {
    GAgg *g = &P->agg[a];
    if (g->fn == 1) { R->aggCnt[a]++; continue; }
    QcVal x; if (!qg_rowval(X, &g->arg, &x)) return 0;
    /* minmaxStep: a NULL row is taken while there is no value yet (the result stays NULL); ties keep the earlier row */
    if (x.type == SQLITE_NULL) { R->took[a] = !R->aggSet[a]; continue; }
    if (x.type != SQLITE_INTEGER && x.type != SQLITE_TEXT) return 0;
    if (!R->aggSet[a]) { R->aggV[a] = x; R->aggSet[a] = 1; R->took[a] = 1; continue; }
    QcVal *cur = &R->aggV[a]; int c;
    if (cur->type != x.type) c = x.type == SQLITE_INTEGER ? -1 : 1;   /* numbers sort before text */
    else if (x.type == SQLITE_INTEGER) c = x.i < cur->i ? -1 : x.i > cur->i;
    else { int n = x.n < cur->n ? x.n : cur->n; c = memcmp(x.z, cur->z, (size_t)n); if (!c) c = x.n - cur->n; }
    R->took[a] = (g->fn == 2 && c < 0) || (g->fn == 3 && c > 0);
    if (R->took[a]) *cur = x;
  }
  return 1;
}
/* one output row from the plan's outputs; 1 continue, 0 stop (LIMIT reached, or fallback) */
static int qg_output(QgCtx *X) {
  QcGPlan *P = X->P; QgRun *R = X->R;
  if (R->nrow >= QG_MAXROWS) { X->err = X->over = 1; return 0; }
  QcVal *o = &R->out[(size_t)R->nrow * P->nout];
  for (int k = 0; k < P->nout; k++) if (!qg_val(X, &P->out[k], &o[k])) { X->err = 1; return 0; }
  R->nrow++;
  if (P->hasLimit && --R->limit == 0) { X->done = 1; return 0; }
  return 1;
}
static int qg_group_out(QgCtx *X) {   /* the open group's row, unless HAVING drops it */
  int f = qg_filters(X, X->P->hav, X->P->nhav);
  if (f < 0) { X->err = 1; return 0; }
  return f ? qg_output(X) : 1;
}
static int qg_sortable(const QcVal *x) { return x->type == SQLITE_NULL || x->type == SQLITE_INTEGER || x->type == SQLITE_TEXT; }
static int qg_order(const QcVal *a, const QcVal *b);
static int qg_group_row(QgCtx *X) {
  QcGPlan *P = X->P; QgRun *R = X->R; QcVal kv[QG_MAXKEY];
  for (int k = 0; k < P->ngk; k++) if (!qg_val(X, &P->gk[k], &kv[k]) || !qg_sortable(&kv[k])) { X->err = 1; return 0; }
  int same = R->ingrp;
  for (int k = 0; k < P->ngk && same; k++) if (qg_order(&kv[k], &R->gkv[k])) same = 0;
  if (!same) {   /* a new group: the last one out, the accumulators reset, this row's bare columns captured */
    if (R->ingrp && !qg_group_out(X)) return 0;
    for (int a = 0; a < P->nagg; a++) { R->aggCnt[a] = 0; R->aggSet[a] = 0; }
    memcpy(R->gkv, kv, sizeof(QcVal) * (size_t)P->ngk);
    if (P->capAgg < 0) for (int c = 0; c < P->ncap; c++) if (!qg_val(X, &P->cap[c], &R->capv[c])) { X->err = 1; return 0; }
    R->ingrp = 1;
  }
  if (!qg_aggstep(X)) { X->err = 1; return 0; }
  if (P->capAgg >= 0 && R->took[P->capAgg])   /* the row min()/max() took: its bare columns */
    for (int c = 0; c < P->ncap; c++) if (!qg_val(X, &P->cap[c], &R->capv[c])) { X->err = 1; return 0; }
  return 1;
}
static int qg_sort_cmp(QgCtx *X, int ia, int ib) {   /* the KeyInfo's order (sqlite3VdbeRecordCompare), then insertion */
  const QcVal *a = X->R->srow[ia], *b = X->R->srow[ib];
  for (int k = 0; k < X->P->nsk; k++) {
    int c = qg_order(&a[k], &b[k]);
    if (!c) continue;
    int f = X->P->sflag[k];
    if (f && (!(f & KEYINFO_ORDER_BIGNULL) || ((f & KEYINFO_ORDER_DESC) != 0) != (a[k].type == SQLITE_NULL || b[k].type == SQLITE_NULL))) c = -c;
    return c;
  }
  return ia - ib;
}
static int qg_key_cmp(QgCtx *X, const QcVal *a, const QcVal *b) {   /* qg_sort_cmp's key order, no tie-break */
  for (int k = 0; k < X->P->nsk; k++) {
    int c = qg_order(&a[k], &b[k]);
    if (!c) continue;
    int f = X->P->sflag[k];
    if (f && (!(f & KEYINFO_ORDER_BIGNULL) || ((f & KEYINFO_ORDER_DESC) != 0) != (a[k].type == SQLITE_NULL || b[k].type == SQLITE_NULL))) c = -c;
    return c;
  }
  return 0;
}
/* the rows' indices in qg_sort_cmp's order: a merge sort (bottom up, through R->sortt), as qsort had them (the order is
   total: no two rows compare equal), with the context passed in rather than in a static qsort's comparator would need */
static void qg_sort(QgCtx *X) {
  QgRun *R = X->R; int n = R->nsrow, *a = R->sidx, *t = R->sortt;
  for (int i = 0; i < n; i++) a[i] = i;
  for (int w = 1; w < n; w *= 2) {
    for (int lo = 0; lo < n; lo += 2 * w) {
      int mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n, i = lo, j = mid, k = lo;
      while (i < mid && j < hi) t[k++] = qg_sort_cmp(X, a[j], a[i]) < 0 ? a[j++] : a[i++];
      while (i < mid) t[k++] = a[i++];
      while (j < hi) t[k++] = a[j++];
    }
    int *x = a; a = t; t = x;
  }
  if (a != R->sidx) memcpy(R->sidx, a, sizeof *a * (size_t)n);
}
static int qg_sort_row(QgCtx *X) {
  QcGPlan *P = X->P; QgRun *R = X->R;
  if (R->nsrow >= QG_MAXROWS) {   /* top-N: keep the first N in order, which is all the index would keep */
    if (!P->topN || P->topN >= QG_MAXROWS) { X->err = X->over = 1; return 0; }
    qg_sort(X);
    for (int i = 0; i < P->topN; i++) memcpy(R->stmp[i], R->srow[R->sidx[i]], sizeof R->stmp[i]);
    memcpy(R->srow, R->stmp, sizeof R->srow[0] * (size_t)P->topN); R->nsrow = (int)P->topN; R->cut = 1;
  }
  QcVal *row = R->srow[R->nsrow]; int k0 = 0;
  if (R->cut) {   /* as SQLite's IdxLE against Last: a row not ahead of the worst kept one (a tie: the later row loses) is dropped
                     before its other columns are evaluated */
    for (; k0 < P->nsk; k0++) {
      if (P->rec[k0].kind == G_SEQ) { row[k0].type = SQLITE_NULL; row[k0].n = 0; continue; }
      if (!qg_val(X, &P->rec[k0], &row[k0]) || !qg_sortable(&row[k0])) { X->err = 1; return 0; }
    }
    if (qg_key_cmp(X, row, R->srow[P->topN - 1]) >= 0) return 1;
  }
  for (int k = k0; k < P->nrec; k++) {
    if (P->rec[k].kind == G_SEQ) { row[k].type = SQLITE_NULL; row[k].n = 0; continue; }
    if (!qg_val(X, &P->rec[k], &row[k])) { X->err = 1; return 0; }
    if (k < P->nsk && !qg_sortable(&row[k])) { X->err = 1; return 0; }
  }
  R->nsrow++; return 1;
}
static void qg_sort_out(QgCtx *X) {
  QgRun *R = X->R; qg_sort(X);
  int n = R->nsrow; if (X->P->topN && n > X->P->topN) n = (int)X->P->topN;
  for (int i = 0; i < n; i++) { X->srow = R->srow[R->sidx[i]]; if (!qg_output(X)) break; }
}
/* the row at level l passed its filters: the next level, a group or sort step, an aggregate step or an output row.
   1 continue, 0 stop */
static int qg_emit(QgCtx *X, int l) {
  QcGPlan *P = X->P;
  if (l + 1 < P->nlvl) return qg_level(X, l + 1);
  if (P->ngk) return qg_group_row(X);
  if (P->nrec) return qg_sort_row(X);
  if (P->nagg) { if (!qg_aggstep(X)) { X->err = 1; return 0; } return 1; }
  return qg_output(X);
}
static int qg_order(const QcVal *a, const QcVal *b) {   /* index order of NULL / INTEGER / TEXT (BINARY) */
  int ca = a->type == SQLITE_NULL ? 0 : a->type == SQLITE_INTEGER ? 1 : 2, cb = b->type == SQLITE_NULL ? 0 : b->type == SQLITE_INTEGER ? 1 : 2;
  if (ca != cb) return ca - cb;
  if (ca == 1) return a->i < b->i ? -1 : a->i > b->i;
  if (ca == 2) { int n = a->n < b->n ? a->n : b->n, c = memcmp(a->z, b->z, (size_t)n); return c ? c : a->n - b->n; }
  return 0;
}
/* a batch of index rows that passed their filters: their table rows in one descent pass (prefetching, as the feed's
   joins), then each row on. 1 continue, 0 stop */
static int qg_flush(QgCtx *X, int l, int n) {
  QcGPlan *P = X->P; QgRun *R = X->R; GLevel *L = &P->lvl[l];
  if (qc_seek_batch(X->bt, L->tabRoot, R->bkey, n, R->bres) != SQLITE_OK) { X->err = 1; return 0; }
  int err = 0;
  for (int i = 0; i < n; i++) {   /* the leaves' references become pins (one per page) */
    MemPage *pg = R->bres[i].leaf; if (!pg) { err = 1; continue; }
    if (R->npin && R->pins[R->npin - 1] == pg->pDbPage) releasePage(pg);
    else if (R->npin < (int)(sizeof R->pins / sizeof *R->pins)) R->pins[R->npin++] = pg->pDbPage;
    else { releasePage(pg); err = 1; }
  }
  if (err) { X->err = 1; return 0; }
  for (int i = 0; i < n; i++) {
    qc_res *q = &R->bres[i]; if (!q->found) { X->err = 1; return 0; }   /* an index entry without its row: corrupt */
    MemPage *pg = q->leaf; CellInfo info; pg->xParseCell(pg, findCell(pg, q->ix), &info);
    if (info.nLocal != info.nPayload || !((L->tabDec && L->tabDec(info.pPayload, info.nPayload, R->col[l][0])) ||
                                          (!L->tabDec && L->tabShape.ok && qc_decode_fast(info.pPayload, info.nPayload, &L->tabShape, L->needTab, R->col[l][0])) ||
                                          qc_decode(info.pPayload, info.nPayload, L->needTab, R->col[l][0]))) { X->err = 1; return 0; }
    memcpy(R->col[l][1], R->bidx[i], sizeof R->bidx[i]);
    R->have[l][0] = R->have[l][1] = 1;
    if (!qg_emit(X, l)) return 0;
  }
  return 1;
}
/* count(*) of the index entries equal to an integer key, from the cells (qc_count1's walk); 0 ok */
static int qg_count_eq(BtCursor *cur, i64 key, i64 *out) {
  i64 cnt = 0;
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
  *out = cnt; return 0;
}
/* run level l to its end (or until stop); 1 = carry on with the enclosing level, 0 = stop everything */
static int qg_level(QgCtx *X, int l) {
  QcGPlan *P = X->P; QgRun *R = X->R; GLevel *L = &P->lvl[l]; BtCursor *c = X->cur[l]; int res, rc;
  if (L->kind == GL_INLIST) {   /* the values as the ephemeral index holds them: sorted, one of each */
    QcVal *iv = R->inv[l]; int n = 0;
    for (int j = 0; j < L->nin; j++) {
      QcVal x; if (!qg_eval(P, X->v, X->O, &L->in[j], &x) || !qg_aff_ok(L->inAff, &x)) { X->err = 1; return 0; }
      int at = n; while (at > 0 && qg_order(&iv[at - 1], &x) > 0) at--;
      if (at > 0 && !qg_order(&iv[at - 1], &x)) continue;
      memmove(&iv[at + 1], &iv[at], sizeof *iv * (size_t)(n - at)); iv[at] = x; n++;
    }
    for (R->ini[l] = 0; R->ini[l] < n; R->ini[l]++) if (!qg_row(X, l)) return 0;
    return 1;
  }
  if (L->kind == GL_ROWID) {
    QcVal k; if (!qg_val(X, &L->rowidKey, &k)) { X->err = 1; return 0; }
    int hit = 0;
    if (k.type == SQLITE_INTEGER) {
      if (sqlite3BtreeTableMoveto(c, k.i, 0, &res) != SQLITE_OK) { X->err = 1; return 0; }
      hit = res == 0 && c->eState == CURSOR_VALID;
    } else if (k.type != SQLITE_NULL) { X->err = 1; return 0; }   /* SeekRowid on NULL: not found */
    if (hit) return qg_row(X, l);
    if (!L->left) return 1;
    R->nullrow[l] = 1; rc = qg_row(X, l); R->nullrow[l] = 0;   /* LEFT JOIN: one NULL row */
    return rc;
  }
  if (L->kind == GL_SCAN) {
    rc = L->dir ? sqlite3BtreeFirst(c, &res) : sqlite3BtreeLast(c, &res);
    if (rc != SQLITE_OK) { X->err = 1; return 0; }
    if (res) return 1;
  } else {
    /* the seek, as OP_Seek* */
    QcVal kv[QG_MAXKEY]; int oc = L->seekOp;
    for (int j = 0; j < L->nkey; j++) {
      if (!qg_val(X, &L->key[j], &kv[j])) { X->err = 1; return 0; }
      if (kv[j].type == SQLITE_NULL && (L->keyNullExit & 1)) return 1;
      if (!qg_aff_ok(L->aff[j], &kv[j])) { X->err = 1; return 0; }
    }
    if (L->left) { X->err = 1; return 0; }   /* lifted only on point levels */
    if (!L->isIdx) {
      if (kv[0].type != SQLITE_INTEGER) { X->err = 1; return 0; }        /* applyNumericAffinity / real keys: SQLite's path */
      if (sqlite3BtreeTableMoveto(c, kv[0].i, 0, &res) != SQLITE_OK) { X->err = 1; return 0; }
    } else {
      UnpackedRecord *r = P->uk[l]; r->nField = (u16)L->nkey; r->default_rc = ((1 & (oc - OP_SeekLT)) ? -1 : +1);
      r->eqSeen = 0; r->errCode = 0;
      for (int j = 0; j < L->nkey; j++) qg_key(&r->aMem[j], &kv[j]);
      if (sqlite3BtreeIndexMoveto(c, r, &res) != SQLITE_OK) { X->err = 1; return 0; }
    }
    if (oc >= OP_SeekGE) {
      if (res < 0 || (res == 0 && oc == OP_SeekGT)) { rc = sqlite3BtreeNext(c, 0); if (rc == SQLITE_DONE) return 1; if (rc) { X->err = 1; return 0; } }
    } else {
      if (res > 0 || (res == 0 && oc == OP_SeekLT)) { rc = sqlite3BtreePrevious(c, 0); if (rc == SQLITE_DONE) return 1; if (rc) { X->err = 1; return 0; } }
      else if (sqlite3BtreeEof(c)) return 1;
    }
    if (L->cntEq && kv[0].type == SQLITE_INTEGER) {   /* SeekGE k; IdxGT k; AggStep count(*): every aggregate counts */
      i64 n; if (qg_count_eq(c, kv[0].i, &n)) { X->err = 1; return 0; }
      if (L->subKind) X->subCnt += n; else for (int a = 0; a < P->nagg; a++) R->aggCnt[a] += n;
      return 1;
    }
  }
  QcVal ev[QG_MAXKEY];
  for (int j = 0; j < L->nend; j++) {
    if (!qg_val(X, &L->end[j], &ev[j])) { X->err = 1; return 0; }
    if (ev[j].type == SQLITE_NULL && (L->endNull >> j & 1)) return 1;   /* IsNull on the bound: no rows */
    if (!qg_aff_ok(L->endAff[j], &ev[j])) { X->err = 1; return 0; }
  }
  int nb = 0;   /* rows waiting for their table rows */
  for (;;) {
    /* the end test at the loop head */
    if (L->endOp) {
      if (L->isIdx) {
        u32 sz = sqlite3BtreePayloadSize(c), amt; const u8 *rec = sqlite3BtreePayloadFetch(c, &amt);
        if (amt < sz) { X->err = 1; return 0; }
        UnpackedRecord *r = P->uk[l]; r->nField = (u16)L->nend; r->default_rc = L->endOp < OP_IdxLT ? -1 : 0; r->eqSeen = 0; r->errCode = 0;
        for (int j = 0; j < L->nend; j++) qg_key(&r->aMem[j], &ev[j]);
        int cr = sqlite3VdbeFindCompare(r)((int)sz, rec, r);   /* IndexMoveto's comparison (in-page record: its padding) */
        if (r->errCode) { X->err = 1; return 0; }
        if ((L->endOp & 1) == (OP_IdxLT & 1)) cr = -cr; else cr++;
        if (cr > 0) break;
      } else {
        QcVal rid = {.type = SQLITE_INTEGER}; rid.i = sqlite3BtreeIntegerKey(c);
        int j = qg_cmp_jump(L->endOp, L->endP5, &rid, &ev[0]); if (j < 0) { X->err = 1; return 0; }
        if (j) break;
      }
    }
    if (!L->batch) { if (!qg_row(X, l)) return 0; }
    else {   /* filters read only the index record: keep the row, fetch its table row with the batch */
      R->have[l][0] = R->have[l][1] = 0;
      int f = qg_filter(X, l); if (f < 0) { X->err = 1; return 0; }
      if (f) {
        GSym rs = {G_ROWID, (i8)l}; QcVal rid;
        if (!qg_rowval(X, &rs, &rid)) { X->err = 1; return 0; }
        memcpy(R->bidx[nb], R->col[l][1], sizeof R->bidx[nb]); R->bkey[nb++] = rid.i;
        /* a batch never reads past the LIMIT: one row per output, or (GROUP BY: rows per group unknown) 8 at a time */
        int cap = !P->hasLimit || P->nrec ? QC_MAXROWS : P->ngk ? 8 : P->nagg ? QC_MAXROWS : R->limit < QC_MAXROWS ? (int)R->limit : QC_MAXROWS;
        if (nb >= cap) { int r = qg_flush(X, l, nb); nb = 0; if (!r) return 0; }
      }
    }
    if (L->once) break;
    rc = L->dir ? sqlite3BtreeNext(c, 0) : sqlite3BtreePrevious(c, 0);
    if (rc == SQLITE_DONE) break;
    if (rc) { X->err = 1; return 0; }
  }
  return nb ? qg_flush(X, l, nb) : 1;
}

static int qg_exec(Vdbe *v, QcGPlan *P, qc_row_cb cb, void *ctx) {
  sqlite3 *db = v->db;
  if (v->eVdbeState != VDBE_READY_STATE) return QC_FB("g not ready"); if (db->autoCommit && !QC_CONN(db)->ownTxn) return QC_FB("g autocommit"); if (P->iDb >= db->nDb || v->expired) return QC_FB("g expired");   /* e.g. a re-bound parameter the plan depends on (expmask): SQLite re-prepares */
  Btree *bt = db->aDb[P->iDb].pBt; if (!bt) return QC_FB("g btree");
  if (bt->pBt->pPager->bUseFetch) { qc_why = "g mmap"; return QG_DEMOTE; }   /* memory-mapped pages can't take the extra references its pins and batched seeks hold: qvm copies instead */
  if (P->lookup && P->nexec && P->lastRows <= 1 && !qcf.genLookups)   /* QC_GEN_LOOKUPS=1: kept here (A/B) */ { qc_why = "g lookup"; return QG_DEMOTE; }   /* (qc_lift_general's note) */
  if (sqlite3BtreeTxnState(bt) == SQLITE_TXN_NONE) { int m; P->txnChecked = 0; if (sqlite3BtreeBeginTrans(bt, 0, &m) != SQLITE_OK) return QC_FB("g txn"); }
  if (!P->txnChecked || P->bt != bt || db->aDb[P->iDb].pSchema->schema_cookie != P->cookie) {   /* (qc_exec_special's note: the connection's own DDL) */
    int iMeta; sqlite3BtreeGetMeta(bt, BTREE_SCHEMA_VERSION, (u32 *)&iMeta);
    if (iMeta != P->cookie || db->aDb[P->iDb].pSchema->iGeneration != P->gen) return QC_FB("g schema");
    P->txnChecked = 1; P->bt = bt;
    if (!P->act) { QgConn *GC = QCD(db)->pQgConn; QC_LIST_ADD(GC->act, GC->nact, P, act); }
  }
  QgConn *GC = QCD(db)->pQgConn; if (!GC->run && !(GC->run = sqlite3MallocZero(sizeof *GC->run))) return QC_FB("g oom");
  QgRun *R = GC->run; R->nrow = 0; R->npin = 0; R->arenaUsed = 0; R->ingrp = 0; R->nsrow = 0; R->cut = 0;
  QgCtx X; memset(&X, 0, sizeof X); X.P = P; X.v = v; X.O = cb ? 0 : ctx; X.bt = bt; X.R = R;
  for (int l = 0; l < QG_SUB0 + P->nsub; l++) {   /* (the levels in use: P->nlvl loops, then P->nsub subqueries from QG_SUB0) */
    if (l == P->nlvl && l < QG_SUB0) { l = QG_SUB0 - 1; continue; }
    GLevel *L = &P->lvl[l]; if (!L->kind || L->kind == GL_INLIST) continue;
    int k = qg_curslot(P, L->cur); if (k < 0 || qg_cursor(P, bt, k, &X.cur[l])) return QC_FB("g cursor");
    if (L->tabCur >= 0) { int t = qg_curslot(P, L->tabCur); if (t < 0 || qg_cursor(P, bt, t, &X.tab[l])) return QC_FB("g cursor"); }
  }
  for (int a = 0; a < P->nagg; a++) { R->aggCnt[a] = 0; R->aggSet[a] = 0; }
  R->limit = P->hasLimit ? P->limit.v : 0;
  int st = QC_FALLBACK;
  if (P->nlvl) qg_level(&X, 0);
  else if (!P->nagg) {   /* no loop (SELECT EXISTS (...)): one row */
    for (int k = 0; k < P->nout && !X.err; k++) if (!qg_val(&X, &P->out[k], &R->out[k])) X.err = 1;
    R->nrow = 1;
  }
  if (!X.err && !X.done && P->ngk && R->ingrp) qg_group_out(&X);   /* the last group */
  if (!X.err && P->nrec) qg_sort_out(&X);
  if (X.err) { qc_why = X.over ? "g rows" : "g run"; st = QG_DEMOTE; goto out; }   /* anything this plan can't reproduce (an overflow record, a type it doesn't compare, too many rows): qvm runs the statement from now on, as vdbe.c would */
  if (P->nagg && !P->ngk) {   /* one output row from the aggregates */
    QcVal *o = R->out;
    for (int k = 0; k < P->nout; k++) {
      GSym s = P->out[k];
      if (s.kind != G_AGG) { if (!qg_eval(P, v, X.O, &s, &o[k])) { qc_why = "g agg out"; goto out; } continue; }
      GAgg *g = &P->agg[s.col];
      if (g->fn == 1) { o[k].type = SQLITE_INTEGER; o[k].i = R->aggCnt[s.col]; }
      else if (R->aggSet[s.col]) o[k] = R->aggV[s.col]; else { o[k].type = SQLITE_NULL; o[k].n = 0; }
    }
    R->nrow = 1;
  }
  P->nexec++; P->lastRows = (u32)R->nrow;
  if (cb) st = cb(ctx, R->nrow, P->nout, R->out);
  else {   /* the JSON template, from the rows */
    QcOut *O = ctx; QcTmpl *T = QCV(v)->pQcTmpl;
    if (!T || T->ncol != P->nout) goto out;
    int vec = bt->pBt->pPager->xGet == getPageNormal;
    char *p = O->buf, *end = O->buf + O->cap;
    if ((size_t)(end - p) < T->lopen + T->lclose + 64) goto out;
    p = qc_frag(p, T->open, T->lopen);
    for (int i = 0; i < R->nrow; i++) {
      if ((size_t)(end - p) < T->lsep + QC_FRAG + 64) goto out;
      if (i) p = qc_frag(p, T->sep, T->lsep);
      for (int k = 0; k < T->ncol; k++) {
        const QcVal *x = &R->out[(size_t)i * P->nout + k];
        if (T->kind[k] == 'i') { if (x->type != SQLITE_INTEGER || (size_t)(end - p) < 2 * QC_FRAG + 21 + 64) goto out; p = qc_frag(p, T->frag[k], T->lfrag[k]); p = qc_jint(p, x->i); }
        else { if (x->type != SQLITE_TEXT || (size_t)(end - p) < 2 * QC_FRAG + 2 + 6 * (size_t)x->n + 64) goto out; p = qc_frag(p, T->frag[k], T->lfrag[k]); p = qc_jstr(p, x->z, (u32)x->n, vec); }
      }
      if ((size_t)(end - p) < 2 * QC_FRAG + 64) goto out;
      p = qc_frag(p, T->frag[T->ncol], T->lfrag[T->ncol]);
    }
    if ((size_t)(end - p) < QC_FRAG + 64) goto out;
    p = qc_frag(p, T->close, T->lclose);
    O->n = (size_t)(p - O->buf); O->rows = R->nrow; st = SQLITE_OK;
  }
out:
  for (int i = 0; i < R->npin; i++) sqlite3PagerUnref(R->pins[i]);
  if (v->pAuxData) sqlite3VdbeDeleteAuxData(v->db, &v->pAuxData, -1, 0);   /* functions' auxdata, as vdbe.c's Cleanup frees it */
  return st;
}

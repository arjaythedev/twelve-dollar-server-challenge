/* qvm_run.h: qvm's interpreter loop, #included twice by qvm.c: QV_RUN with QV_FB 1 records type feedback (the
** first runs of a plan), with QV_FB 0 it is the plain loop every later run takes. In WebAssembly (QV_REENTER_OPS)
** both return 2 every so many jumps and are called again from X->pc0 (qvm.c, QV_FB_LEAVE). */
static int QV_RUN(QvX *X) {
#if defined(QV_REENTER_OPS) || defined(QV_RESUME)   /* (QV_RESUME: qv_run_rs, a streamed run's next row from X->pc0) */
  Vdbe *v = X->v; Op *aOp = v->aOp; Mem *R = X->r; QvCur *Cs = X->c; int pc = X->pc0, res, rc; Op *op;
#else   /* (natively the loop always starts at 0: a start read from X costs clang's register allocation 5 % more instructions) */
  Vdbe *v = X->v; Op *aOp = v->aOp; Mem *R = X->r; QvCur *Cs = X->c; int pc = 0, res, rc; Op *op;
#endif
#if defined(__GNUC__) && !defined(QV_NO_THREADED)
  static const void *const qv_disp[256] = { [0 ... 255] = &&L_default,
    [OP_Add] = &&L_OP_Add, [OP_AddImm] = &&L_OP_AddImm, [OP_Affinity] = &&L_OP_Affinity, [OP_AggFinal] = &&L_OP_AggFinal,
    [OP_AggInverse] = &&L_OP_AggInverse, [OP_AggStep] = &&L_OP_AggStep, [OP_AggStep1] = &&L_OP_AggStep1, [OP_AggValue] = &&L_OP_AggValue,
    [OP_And] = &&L_OP_And, [OP_BeginSubrtn] = &&L_OP_BeginSubrtn, [OP_BitAnd] = &&L_OP_BitAnd, [OP_BitNot] = &&L_OP_BitNot,
    [OP_BitOr] = &&L_OP_BitOr, [OP_Blob] = &&L_OP_Blob, [OP_Cast] = &&L_OP_Cast, [OP_Close] = &&L_OP_Close,
    [OP_CollSeq] = &&L_OP_CollSeq, [OP_Column] = &&L_OP_Column, [OP_ColumnsUsed] = &&L_OP_ColumnsUsed, [OP_Compare] = &&L_OP_Compare,
    [OP_Concat] = &&L_OP_Concat, [OP_Copy] = &&L_OP_Copy, [OP_Count] = &&L_OP_Count, [OP_CursorHint] = &&L_OP_CursorHint,
    [OP_DecrJumpZero] = &&L_OP_DecrJumpZero, [OP_DeferredSeek] = &&L_OP_DeferredSeek, [OP_Delete] = &&L_OP_Delete, [OP_Divide] = &&L_OP_Divide,
    [OP_ElseEq] = &&L_OP_ElseEq, [OP_EndCoroutine] = &&L_OP_EndCoroutine, [OP_Eq] = &&L_OP_Eq, [OP_Explain] = &&L_OP_Explain,
    [OP_Filter] = &&L_OP_Filter, [OP_FilterAdd] = &&L_OP_FilterAdd, [OP_FinishSeek] = &&L_OP_FinishSeek, [OP_Found] = &&L_OP_Found,
    [OP_Function] = &&L_OP_Function, [OP_Ge] = &&L_OP_Ge, [OP_Gosub] = &&L_OP_Gosub, [OP_Goto] = &&L_OP_Goto,
    [OP_Gt] = &&L_OP_Gt, [OP_Halt] = &&L_OP_Halt, [OP_IdxGE] = &&L_OP_IdxGE, [OP_IdxGT] = &&L_OP_IdxGT,
    [OP_IdxInsert] = &&L_OP_IdxInsert, [OP_IdxLE] = &&L_OP_IdxLE, [OP_IdxLT] = &&L_OP_IdxLT, [OP_IdxRowid] = &&L_OP_IdxRowid,
    [OP_If] = &&L_OP_If, [OP_IfEmpty] = &&L_OP_IfEmpty, [OP_IfNoHope] = &&L_OP_IfNoHope, [OP_IfNot] = &&L_OP_IfNot,
    [OP_IfNotZero] = &&L_OP_IfNotZero, [OP_IfNullRow] = &&L_OP_IfNullRow, [OP_IfPos] = &&L_OP_IfPos, [OP_Init] = &&L_OP_Init,
    [OP_InitCoroutine] = &&L_OP_InitCoroutine, [OP_Insert] = &&L_OP_Insert, [OP_Int64] = &&L_OP_Int64, [OP_IntCopy] = &&L_OP_IntCopy,
    [OP_Integer] = &&L_OP_Integer, [OP_IsNull] = &&L_OP_IsNull, [OP_IsTrue] = &&L_OP_IsTrue, [OP_Jump] = &&L_OP_Jump,
    [OP_Last] = &&L_OP_Last, [OP_Le] = &&L_OP_Le, [OP_Lt] = &&L_OP_Lt, [OP_MakeRecord] = &&L_OP_MakeRecord,
    [OP_Move] = &&L_OP_Move, [OP_Multiply] = &&L_OP_Multiply, [OP_MustBeInt] = &&L_OP_MustBeInt, [OP_Ne] = &&L_OP_Ne,
    [OP_NewRowid] = &&L_OP_NewRowid, [OP_Next] = &&L_OP_Next, [OP_NoConflict] = &&L_OP_NoConflict, [OP_Noop] = &&L_OP_Noop,
    [OP_Not] = &&L_OP_Not, [OP_NotExists] = &&L_OP_NotExists, [OP_NotFound] = &&L_OP_NotFound, [OP_NotNull] = &&L_OP_NotNull,
    [OP_Null] = &&L_OP_Null, [OP_NullRow] = &&L_OP_NullRow, [OP_OffsetLimit] = &&L_OP_OffsetLimit, [OP_Once] = &&L_OP_Once,
    [OP_OpenAutoindex] = &&L_OP_OpenAutoindex, [OP_OpenDup] = &&L_OP_OpenDup, [OP_OpenEphemeral] = &&L_OP_OpenEphemeral, [OP_OpenPseudo] = &&L_OP_OpenPseudo,
    [OP_OpenRead] = &&L_OP_OpenRead, [OP_Or] = &&L_OP_Or, [OP_Permutation] = &&L_OP_Permutation, [OP_Prev] = &&L_OP_Prev,
    [OP_PureFunc] = &&L_OP_PureFunc, [OP_Real] = &&L_OP_Real, [OP_RealAffinity] = &&L_OP_RealAffinity, [OP_Remainder] = &&L_OP_Remainder,
    [OP_ReopenIdx] = &&L_OP_ReopenIdx, [OP_ResetSorter] = &&L_OP_ResetSorter, [OP_RowSetAdd] = &&L_OP_RowSetAdd, [OP_RowSetRead] = &&L_OP_RowSetRead, [OP_RowSetTest] = &&L_OP_RowSetTest, [OP_ClrSubtype] = &&L_OP_ClrSubtype, [OP_ResultRow] = &&L_OP_ResultRow, [OP_Return] = &&L_OP_Return,
    [OP_Rewind] = &&L_OP_Rewind, [OP_RowData] = &&L_OP_RowData, [OP_Rowid] = &&L_OP_Rowid, [OP_SCopy] = &&L_OP_SCopy,
    [OP_SeekGE] = &&L_OP_SeekGE, [OP_SeekGT] = &&L_OP_SeekGT, [OP_SeekHit] = &&L_OP_SeekHit, [OP_SeekLE] = &&L_OP_SeekLE,
    [OP_SeekLT] = &&L_OP_SeekLT, [OP_SeekRowid] = &&L_OP_SeekRowid, [OP_SeekScan] = &&L_OP_SeekScan, [OP_Sequence] = &&L_OP_Sequence,
    [OP_SequenceTest] = &&L_OP_SequenceTest, [OP_ShiftLeft] = &&L_OP_ShiftLeft, [OP_ShiftRight] = &&L_OP_ShiftRight, [OP_SoftNull] = &&L_OP_SoftNull,
    [OP_Sort] = &&L_OP_Sort, [OP_SorterCompare] = &&L_OP_SorterCompare, [OP_SorterData] = &&L_OP_SorterData, [OP_SorterInsert] = &&L_OP_SorterInsert,
    [OP_SorterNext] = &&L_OP_SorterNext, [OP_SorterOpen] = &&L_OP_SorterOpen, [OP_SorterSort] = &&L_OP_SorterSort, [OP_String] = &&L_OP_String,
    [OP_String8] = &&L_OP_String8, [OP_Subtract] = &&L_OP_Subtract, [OP_TableLock] = &&L_OP_TableLock, [OP_Transaction] = &&L_OP_Transaction,
    [OP_Variable] = &&L_OP_Variable, [OP_Yield] = &&L_OP_Yield, [OP_ZeroOrNull] = &&L_OP_ZeroOrNull };
  QJUMP;
#endif
  for (;;) {
#if !defined(__GNUC__) || defined(QV_NO_THREADED)
    if (0) { qv_jump: QV_FB_LEAVE; }   /* (QJUMP: the hand-over, then the dispatch) */
#endif
  qv_top: QV_UNUSED_LABEL;
    op = &aOp[pc];
    QV_TRACE_AT(pc, op->opcode);
    switch (op->opcode) {
      case OP_Init: QL(OP_Init) pc = op->p2; QJUMP;
      case OP_TableLock: QL(OP_TableLock)   /* shared cache: the read lock vdbe.c takes (a conflict goes back to SQLite, which reports SQLITE_LOCKED) */
#ifndef SQLITE_OMIT_SHARED_CACHE
        if ((op->p3 || !(X->db->flags & SQLITE_ReadUncommit)) && sqlite3BtreeLockTable(X->db->aDb[op->p1].pBt, op->p2, (u8)op->p3) != SQLITE_OK) { qv_why = "table locked"; return 0; }
#endif
        QNEXT;
      case OP_Transaction: QL(OP_Transaction) case OP_Noop: QL(OP_Noop) case OP_Explain: QL(OP_Explain) case OP_ColumnsUsed: QL(OP_ColumnsUsed) case OP_CursorHint: QL(OP_CursorHint)
        QNEXT;
      case OP_FilterAdd: QL(OP_FilterAdd) {   /* as vdbe.c: the key's bit set in the Bloom filter (register P1, a blob) */
        if (X->P->fltOn && op->p1 > 0 && op->p1 <= X->P->nMem && !X->P->fltOn[op->p1]) QNEXT;   /* (no Filter probes it any more) */
        Mem *f = &R[op->p1]; if (!(f->flags & MEM_Blob) || f->n <= 0 || op->p4type != P4_INT32) { qv_why = "filter"; return 0; }
        u64 h = filterHash(R, op) % ((u64)f->n * 8); ((u8 *)f->z)[h / 8] |= (u8)(1 << (h & 7));
        QNEXT;
      }
      case OP_Filter: QL(OP_Filter) {   /* the key's bit clear: no row has it, jump (vdbe.c) */
        QvPlan *P = X->P; struct QvFlt *st = &P->flt[pc];
        if (st->off) QNEXT;   /* (it rejected nothing in an earlier run: qv_exec's end decides) */
        Mem *f = &R[op->p1]; if (!(f->flags & MEM_Blob) || f->n <= 0 || op->p4type != P4_INT32) { qv_why = "filter"; return 0; }
        u64 h = filterHash(R, op) % ((u64)f->n * 8); st->n++;
        if (!(((const u8 *)f->z)[h / 8] & (1 << (h & 7)))) { st->hit++; pc = op->p2; QJUMP; }
        QNEXT;
      }
      case OP_SeekScan: QL(OP_SeekScan)   /* (a seek optimisation that can only skip work: never jump) */
        QNEXT;
      case OP_Goto: QL(OP_Goto) if (op->p2 <= pc && !qv_tick(X)) return 0; pc = op->p2; QJUMP;   /* (backwards: a loop) */
      case OP_Gosub: QL(OP_Gosub) qv_int(qv_out(X, op->p1), pc); pc = op->p2; QJUMP;
      case OP_Return: QL(OP_Return)
        if (R[op->p1].flags & MEM_Int) { pc = (int)R[op->p1].u.i + 1; QJUMP; }
        QNEXT;
      case OP_InitCoroutine: QL(OP_InitCoroutine)
        qv_int(qv_out(X, op->p1), op->p3 - 1);
        if (op->p2) { pc = op->p2; QJUMP; }
        QNEXT;
      case OP_Yield: QL(OP_Yield) { int dest = (int)R[op->p1].u.i; qv_int(&R[op->p1], pc); pc = dest + 1; QJUMP; }
      case OP_EndCoroutine: QL(OP_EndCoroutine) { Op *caller = &aOp[R[op->p1].u.i]; qv_int(&R[op->p1], pc - 1); pc = caller->p2; QJUMP; }
      case OP_Halt: QL(OP_Halt)
        if (op->p1) { qv_why = "halt with error"; return 0; }
        return 1;
      case OP_Integer: QL(OP_Integer) qv_int(qv_out(X, op->p2), op->p1); QNEXT;
      case OP_Int64: QL(OP_Int64) qv_int(qv_out(X, op->p2), INT32_TO_64(op->p1, op->p3)); QNEXT;
      case OP_Real: QL(OP_Real) { u64 ii = (u64)INT32_TO_64(op->p1, op->p3); swapMixedEndianFloat(ii); Mem *o = qv_out(X, op->p2); o->flags = MEM_Real; memcpy(&o->u.r, &ii, 8); QNEXT; }
      case OP_String8: QL(OP_String8) qv_static(qv_out(X, op->p2), op->p4.z, (int)strlen(op->p4.z), 1, 1); QNEXT;
      case OP_String: QL(OP_String) qv_str(X, R, op, qv_out(X, op->p2)); QNEXT;
      case OP_Blob: QL(OP_Blob)
        if (op->p4.z) qv_static(qv_out(X, op->p2), op->p4.z, op->p1, 0, 0);
        else {   /* no data: as vdbe.c, a zeroblob of P1 bytes, expanded (a Bloom filter's bits) */
          u8 *z = qv_alloc(&X->A, (size_t)op->p1 + 1); if (!z) { qv_why = "arena"; return 0; }
          memset(z, 0, (size_t)op->p1 + 1); qv_static(qv_out(X, op->p2), z, op->p1, 0, 0);
        }
        QNEXT;
      case OP_Variable: QL(OP_Variable) {
        Mem *o = qv_out(X, op->p2);
        if (X->O && X->O->param && op->p1 <= X->O->nparam) { qv_int(o, X->O->param[op->p1 - 1]); QNEXT; }
        Mem *pVar = &v->aVar[op->p1 - 1];
        if (pVar->flags & MEM_Zero) { qv_why = "zeroblob parameter"; return 0; }
        memcpy(o, pVar, MEMCELLSIZE); o->flags &= ~(MEM_Dyn | MEM_Ephem); o->flags |= MEM_Static | MEM_FromBind;   /* as vdbe.c */
        QNEXT;
      }
      case OP_Copy: QL(OP_Copy) if (!qv_copy(X, R, Cs, op)) return 0; QNEXT;
      case OP_SCopy: QL(OP_SCopy) qv_scopy(X, R, Cs, op); QNEXT;
      case OP_IntCopy: QL(OP_IntCopy) sqlite3VdbeMemSetInt64(qv_out(X, op->p2), R[op->p1].u.i); QNEXT;
      case OP_Move: QL(OP_Move) if (!qv_move(X, R, Cs, op)) return 0; QNEXT;
      case OP_SoftNull: QL(OP_SoftNull) { Mem *o = qv_out(X, op->p1); o->flags = (o->flags & ~(MEM_Undefined | MEM_AffMask)) | MEM_Null; QNEXT; }
      case OP_BeginSubrtn: QL(OP_BeginSubrtn) case OP_Null: QL(OP_Null) qv_null(X, R, Cs, op); QNEXT;
      case OP_ResultRow: QL(OP_ResultRow)
#if !QV_FB
        if (X->strm) { if (!qv_strm_term(&R[op->p1], op->p2)) return 0; X->pc0 = pc + 1; X->res = &R[op->p1]; X->nrow++; return 3; }   /* streamed: the row is the registers, as vdbe.c's */
#endif
        if (!qv_result(X, R, Cs, op)) return 0; QNEXT;
      case OP_Once: QL(OP_Once) {
        int bit = 1 << (pc & 7);
        if (X->once[pc >> 3] & bit) { pc = op->p2; QJUMP; }
        X->once[pc >> 3] |= (u8)bit;
        QNEXT;
      }
      case OP_If: QL(OP_If) if (sqlite3VdbeBooleanValue(&R[op->p1], op->p3)) { pc = op->p2; QJUMP; } QNEXT;
      case OP_IfNot: QL(OP_IfNot) if (!sqlite3VdbeBooleanValue(&R[op->p1], !op->p3)) { pc = op->p2; QJUMP; } QNEXT;
      case OP_IsNull: QL(OP_IsNull) if (qv_isnull(&R[op->p1])) { pc = op->p2; QJUMP; } QNEXT;
      case OP_NotNull: QL(OP_NotNull) if (!qv_isnull(&R[op->p1])) { pc = op->p2; QJUMP; } QNEXT;
      case OP_ZeroOrNull: QL(OP_ZeroOrNull)
        if (qv_isnull(&R[op->p1]) || qv_isnull(&R[op->p3])) qv_out(X, op->p2)->flags = MEM_Null; else qv_int(qv_out(X, op->p2), 0);
        QNEXT;
      case OP_IfPos: QL(OP_IfPos) if (R[op->p1].u.i > 0) { R[op->p1].u.i -= op->p3; pc = op->p2; QJUMP; } QNEXT;
      case OP_IfNotZero: QL(OP_IfNotZero) if (R[op->p1].u.i) { if (R[op->p1].u.i > 0) R[op->p1].u.i--; pc = op->p2; QJUMP; } QNEXT;
      case OP_DecrJumpZero: QL(OP_DecrJumpZero)
        if (R[op->p1].u.i > SMALLEST_INT64) R[op->p1].u.i--;
        if (R[op->p1].u.i == 0) { pc = op->p2; QJUMP; }
        QNEXT;
      case OP_OffsetLimit: QL(OP_OffsetLimit) {
        i64 x = R[op->p1].u.i, off = R[op->p3].u.i;
        qv_int(qv_out(X, op->p2), (x <= 0 || sqlite3AddInt64(&x, off > 0 ? off : 0)) ? -1 : x);
        QNEXT;
      }
      case OP_AddImm: QL(OP_AddImm) { Mem *x = &R[op->p1]; sqlite3VdbeMemIntegerify(x); *(u64 *)&x->u.i += (u64)op->p2; QNEXT; }
      case OP_MustBeInt: QL(OP_MustBeInt) {
        Mem *x = &R[op->p1];
        if (!(x->flags & MEM_Int)) {
          if (x->flags & MEM_Real) {
            i64 ix;
            if (x->u.r >= -9223372036854775808.0 && x->u.r < 9223372036854775808.0 && (double)(ix = sqlite3RealToI64(x->u.r)) == x->u.r) { x->u.i = ix; MemSetTypeFlag(x, MEM_Int); }
          } else applyAffinity(x, SQLITE_AFF_NUMERIC, SQLITE_UTF8);
          if (!(x->flags & MEM_Int)) { if (!op->p2) { qv_why = "mismatch"; return 0; } pc = op->p2; QJUMP; }
        }
        MemSetTypeFlag(x, MEM_Int);
        QNEXT;
      }
      case OP_RealAffinity: QL(OP_RealAffinity) if (R[op->p1].flags & (MEM_Int | MEM_IntReal)) sqlite3VdbeMemRealify(&R[op->p1]); QNEXT;
      case OP_Cast: QL(OP_Cast)
        if (ExpandBlob(&R[op->p1]) || sqlite3VdbeMemCast(&R[op->p1], (u8)op->p2, SQLITE_UTF8) != SQLITE_OK) { qv_why = "cast"; return 0; }   /* (a zeroblob expanded first, as vdbe.c)*/
        QNEXT;
      case OP_Affinity: QL(OP_Affinity) {
        const char *z = op->p4.z; Mem *x = &R[op->p1];
        for (; z[0]; z++, x++) {   /* (no break inside: the dispatch below is threaded) */
          applyAffinity(x, z[0], SQLITE_UTF8);
          if (z[0] == SQLITE_AFF_REAL && (x->flags & MEM_Int)) { x->u.r = (double)x->u.i; x->flags |= MEM_Real; x->flags &= ~(MEM_Int | MEM_Str); }
        }
        QNEXT;
      }
      case OP_Eq: QL(OP_Eq) case OP_Ne: QL(OP_Ne) case OP_Lt: QL(OP_Lt) case OP_Le: QL(OP_Le) case OP_Gt: QL(OP_Gt) case OP_Ge: QL(OP_Ge)
        if (QV_FB) X->fb[pc] |= qv_fbc(&R[op->p1]) | qv_fbc(&R[op->p3]);
        if (qv_compare_op(X, op)) { pc = op->p2; QJUMP; }
        QNEXT;
      case OP_ElseEq: QL(OP_ElseEq) if (X->iCompare == 0) { pc = op->p2; QJUMP; } QNEXT;
      case OP_Permutation: QL(OP_Permutation) X->aPermute = (const u32 *)op->p4.ai + 1; QNEXT;
      case OP_Compare: QL(OP_Compare) qv_cmpvec(X, R, Cs, op); QNEXT;
      case OP_Jump: QL(OP_Jump) pc = X->iCompare < 0 ? op->p1 : X->iCompare == 0 ? op->p2 : op->p3; QJUMP;
      case OP_Not: QL(OP_Not) {
        Mem *x = &R[op->p1];
        if (!qv_isnull(x)) { int b = !sqlite3VdbeBooleanValue(x, 0); sqlite3VdbeMemSetInt64(qv_out(X, op->p2), b); }
        else qv_out(X, op->p2)->flags = MEM_Null;
        QNEXT;
      }
      case OP_BitNot: QL(OP_BitNot) {
        Mem *x = &R[op->p1];
        if (!qv_isnull(x)) { i64 b = ~sqlite3VdbeIntValue(x); sqlite3VdbeMemSetInt64(qv_out(X, op->p2), b); }
        else qv_out(X, op->p2)->flags = MEM_Null;
        QNEXT;
      }
      case OP_IsTrue: QL(OP_IsTrue) { int b = sqlite3VdbeBooleanValue(&R[op->p1], op->p3) ^ op->p4.i; sqlite3VdbeMemSetInt64(qv_out(X, op->p2), b); QNEXT; }
      case OP_And: QL(OP_And) case OP_Or: QL(OP_Or) {
        static const u8 and_logic[] = {0, 0, 0, 0, 1, 2, 0, 2, 2}, or_logic[] = {0, 1, 2, 1, 1, 1, 2, 1, 2};
        int v1 = sqlite3VdbeBooleanValue(&R[op->p1], 2), v2 = sqlite3VdbeBooleanValue(&R[op->p2], 2);
        v1 = op->opcode == OP_And ? and_logic[v1 * 3 + v2] : or_logic[v1 * 3 + v2];
        Mem *o = qv_out(X, op->p3);
        if (v1 == 2) o->flags = MEM_Null; else qv_int(o, v1);
        QNEXT;
      }
      case OP_BitAnd: QL(OP_BitAnd) case OP_BitOr: QL(OP_BitOr) case OP_ShiftLeft: QL(OP_ShiftLeft) case OP_ShiftRight: QL(OP_ShiftRight) qv_bitop(X, R, Cs, op); QNEXT;
      case OP_Add: QL(OP_Add) case OP_Subtract: QL(OP_Subtract) case OP_Multiply: QL(OP_Multiply) case OP_Divide: QL(OP_Divide) case OP_Remainder: QL(OP_Remainder) if (QV_FB) X->fb[pc] |= qv_fbc(&R[op->p1]) | qv_fbc(&R[op->p2]); if (!qv_arith_fast(X, op)) qv_arith(X, op); QNEXT;
      case OP_Concat: QL(OP_Concat) {   /* P3 = P2 || P1, into the arena */
        Mem *pIn1 = &R[op->p1], *pIn2 = &R[op->p2];
        if ((pIn1->flags | pIn2->flags) & MEM_Null) { qv_out(X, op->p3)->flags = MEM_Null; QNEXT; }
        u16 flags1 = pIn1->flags, flags2;
        if (!(flags1 & (MEM_Str | MEM_Blob))) { if (sqlite3VdbeMemStringify(pIn1, SQLITE_UTF8, 0)) return 0; flags1 = pIn1->flags & ~MEM_Str; }
        else if (flags1 & MEM_Zero) { if (sqlite3VdbeMemExpandBlob(pIn1)) return 0; flags1 = pIn1->flags & ~MEM_Str; }
        flags2 = pIn2->flags;
        if (!(flags2 & (MEM_Str | MEM_Blob))) { if (sqlite3VdbeMemStringify(pIn2, SQLITE_UTF8, 0)) return 0; flags2 = pIn2->flags & ~MEM_Str; }
        else if (flags2 & MEM_Zero) { if (sqlite3VdbeMemExpandBlob(pIn2)) return 0; flags2 = pIn2->flags & ~MEM_Str; }
        i64 n = (i64)pIn1->n + pIn2->n;
        if (n > X->db->aLimit[SQLITE_LIMIT_LENGTH]) { qv_why = "too big"; return 0; }
        char *d = qv_alloc(&X->A, (size_t)n + 1); if (!d) { qv_why = "arena"; return 0; }
        memcpy(d, pIn2->z, (size_t)pIn2->n); memcpy(d + pIn2->n, pIn1->z, (size_t)pIn1->n); d[n] = 0;
        pIn1->flags = flags1; if (pIn1 != pIn2) pIn2->flags = flags2;   /* as vdbe.c: the stringified inputs keep their types */
        qv_static(qv_out(X, op->p3), d, (int)n, 1, 1);
        QNEXT;
      }
      case OP_Function: QL(OP_Function) case OP_PureFunc: QL(OP_PureFunc) if (!qv_function(X, op, pc)) return 0; QNEXT;
      case OP_AggStep: QL(OP_AggStep) case OP_AggStep1: QL(OP_AggStep1) case OP_AggInverse: QL(OP_AggInverse)
        if (QV_FB && op->p5) X->fb[pc] |= qv_fbc(&R[op->p2]);
        if (!qv_aggop(X, R, Cs, op, pc)) return 0;
        QNEXT;
      case OP_AggValue: QL(OP_AggValue) case OP_AggFinal: QL(OP_AggFinal)
        if (X->P->isSum[op->p1] && !X->P->sacc[op->p1].spilled) {   /* sumFinalize / totalFinalize / avgFinalize */
          struct QvSum *a = &X->P->sacc[op->p1]; i64 iSum = a->iSum, cnt = a->cnt; int kind = X->P->isSum[op->p1];
          Mem *o = qv_out(X, op->p3 ? op->p3 : op->p1);   /* (on AggFinal this restarts the accumulator) */
          if (kind == 2) { o->flags = MEM_Real; o->u.r = (double)iSum; }
          else if (cnt <= 0) o->flags = MEM_Null;
          else if (kind == 1) qv_int(o, iSum);
          else { o->flags = MEM_Real; o->u.r = (double)iSum / (double)cnt; }
          QNEXT;
        }
        if (X->P->isSum[op->p1] && !op->p3) { struct QvSum *a = &X->P->sacc[op->p1]; a->spilled = 0; a->iSum = a->cnt = 0; }   /* AggFinal consumes SQLite's SumCtx */
        if (X->P->isCnt[op->p1]) {
          i64 n = X->P->cnt[op->p1];
          if (op->p3) qv_int(qv_out(X, op->p3), n); else qv_int(qv_out(X, op->p1), n);   /* (qv_out restarts the counter) */
          QNEXT;
        }
        if (!qv_aggfinal(X, op)) return 0;
        QNEXT;
      case OP_CollSeq: QL(OP_CollSeq) if (op->p1) qv_int(qv_out(X, op->p1), 0); QNEXT;
      /* ---- cursors */
      case OP_OpenRead: QL(OP_OpenRead) case OP_ReopenIdx: QL(OP_ReopenIdx) {
        QvCur *C = &Cs[op->p1];
        if (op->opcode == OP_ReopenIdx && C->kind == QVC_BT && X->P->bcRoot[op->p1] == (Pgno)op->p2) {   /* the same index: cleared, and its seek hint set again (vdbe.c: open_cursor_set_hints) */
          C->nullRow = 1; qv_stale(C); sqlite3BtreeClearCursor(C->bc); C->eqOnly = (op->p5 & OPFLAG_SEEKEQ) != 0;
          QNEXT;
        }
        if (C->kind && C->kind != QVC_BT) qv_cur_close(X, C);
        if (!qv_cursor_db(X, op, C)) return 0;
        QNEXT;
      }
      case OP_OpenEphemeral: QL(OP_OpenEphemeral) case OP_OpenAutoindex: QL(OP_OpenAutoindex) if (!qv_open_eph(X, op, &Cs[op->p1])) return 0; QNEXT;
      case OP_OpenDup: QL(OP_OpenDup) {
        QvCur *O = &Cs[op->p2], *C = &Cs[op->p1];
        if (O->kind == QVC_VEC) {
          if (C->kind) qv_cur_close(X, C);
          O->noReuse = 1;
          C->kind = QVC_VEC; C->vec = O->vec; C->isTable = 1; C->nullRow = 1; C->vbad = 1; C->noReuse = 1;
          { QvVec *V = O->vec; if (V->nref < 8) V->ref[V->nref++] = C; else V->nref = 9; }
          if (!qv_cur_alloc(X, C, O->nField)) return 0;
          QNEXT;
        }
        if (O->kind != QVC_EPH) { qv_why = "opendup"; return 0; }
        if (C->kind) qv_cur_close(X, C);
        O->noReuse = 1;
        C->kind = QVC_EPH; C->ebt = O->ebt; C->eroot = O->eroot; C->ki = O->ki; C->isTable = O->isTable; C->copyVals = 1; C->nullRow = 1; C->noReuse = 1;
        if (!(C->bc = qv_bc_alloc(QV_CONN(X->db)))) return 0;
        if (sqlite3BtreeCursor(C->ebt, C->eroot, BTREE_WRCSR, C->ki, C->bc) != SQLITE_OK) { qv_why = "opendup cursor"; return 0; }
        if (!qv_cur_alloc(X, C, O->nField)) return 0;
        QNEXT;
      }
      case OP_OpenPseudo: QL(OP_OpenPseudo) {
        QvCur *C = &Cs[op->p1]; if (C->kind) qv_cur_close(X, C);
        C->kind = QVC_PSEUDO; C->nullRow = 1; C->pseudoReg = op->p2; C->isTable = 1;
        if (!qv_cur_alloc(X, C, op->p3)) return 0;
        QNEXT;
      }
      case OP_SorterOpen: QL(OP_SorterOpen) {
        QvCur *C = &Cs[op->p1]; if (C->kind) qv_cur_close(X, C);
        C->kind = QVC_SORT; C->ki = op->p4.pKeyInfo; C->nullRow = 1;
        if (!(C->srt = qv_alloc(&X->A, sizeof *C->srt))) return 0;
        memset(C->srt, 0, sizeof *C->srt); C->srt->ki = C->ki; C->srt->nk = C->ki->nKeyField; C->srt->nf = op->p3; C->srt->cur = op->p1;
        qv_sort_limits(X, C->srt);
        if (!qv_cur_alloc(X, C, op->p2)) return 0;
        QNEXT;
      }
      case OP_Close: QL(OP_Close) { QvCur *C = &Cs[op->p1]; if (C->kind == QVC_BT) qv_cur_reset(C); else if (C->kind) qv_cur_close(X, C); QNEXT; }
      case OP_NullRow: QL(OP_NullRow) {
        QvCur *C = &Cs[op->p1];
        if (!C->kind) { C->kind = QVC_NULLONLY; C->isTable = 1; }
        C->nullRow = op->p2 == 0; qv_stale(C);
        if (C->kind == QVC_VEC || C->kind == QVC_TOPN) C->vbad = 2;   /* a B-tree cursor would be cleared: its next move is a reposition */
        if ((C->kind == QVC_BT || C->kind == QVC_EPH) && C->bc) sqlite3BtreeClearCursor(C->bc);
        QNEXT;
      }
      case OP_IfNullRow: QL(OP_IfNullRow) {
        QvCur *C = &Cs[op->p1];
        if (C->kind && C->nullRow) { qv_out(X, op->p3)->flags = MEM_Null; pc = op->p2; QJUMP; }
        QNEXT;
      }
      case OP_Rewind: QL(OP_Rewind) case OP_Sort: QL(OP_Sort) case OP_SorterSort: QL(OP_SorterSort) {
        QvCur *C = &Cs[op->p1];
        if (C->kind == QVC_SORT && C->srt->vc) {
          if (sqlite3VdbeSorterRewind(C->srt->vc, &res) != SQLITE_OK) { qv_why = "sorter rewind"; return 0; }
        } else if (C->kind == QVC_SORT) {
          if (!qv_sort(X, C->srt)) return 0;
          C->srt->pos = 0; res = C->srt->n == 0;
        } else if (C->kind == QVC_VEC) {
          QvVec *V = C->vec; res = V->live == 0; C->vi = V->first; C->vbad = (u8)res;
        } else if (C->kind == QVC_TOPN) {
          res = C->tn->n == 0; C->vi = 0; C->vbad = (u8)res;
        } else {
          if (sqlite3BtreeFirst(C->bc, &res) != SQLITE_OK) { qv_why = "rewind"; return 0; }
          C->deferred = 0;
        }
        qv_stale(C); C->nullRow = (u8)res;
        if (op->p2 > 0 && res) { pc = op->p2; QJUMP; }
        QNEXT;
      }
      case OP_Last: QL(OP_Last) {
        QvCur *C = &Cs[op->p1];
        if (C->kind == QVC_SORT) { qv_why = "last on sorter"; return 0; }
        if (C->kind == QVC_TOPN) {
          res = C->tn->n == 0; C->vi = C->tn->n - 1; C->vbad = (u8)res;
          C->nullRow = (u8)res; qv_stale(C);
          if (op->p2 > 0 && res) { pc = op->p2; QJUMP; }
          QNEXT;
        }
        if (C->kind == QVC_VEC) {
          QvVec *V = C->vec; res = V->live == 0; C->vi = V->n - 1; C->vbad = (u8)res;
          C->nullRow = (u8)res; qv_stale(C);
          if (op->p2 > 0 && res) { pc = op->p2; QJUMP; }
          QNEXT;
        }
        if (sqlite3BtreeLast(C->bc, &res) != SQLITE_OK) { qv_why = "last"; return 0; }
        C->nullRow = (u8)res; C->deferred = 0; qv_stale(C);
        if (op->p2 > 0 && res) { pc = op->p2; QJUMP; }
        QNEXT;
      }
      case OP_IfEmpty: QL(OP_IfEmpty) {
        QvCur *C = &Cs[op->p1];
        if (C->kind == QVC_VEC) res = C->vec->live == 0;
        else if (sqlite3BtreeIsEmpty(C->bc, &res) != SQLITE_OK) { qv_why = "ifempty"; return 0; }
        if (res) { pc = op->p2; QJUMP; }
        QNEXT;
      }
      case OP_Next: QL(OP_Next) case OP_Prev: QL(OP_Prev) {
        QvCur *C = &Cs[op->p1];
        if (C->kind == QVC_VEC) {
          QvVec *V = C->vec; qv_stale(C);
          if (C->vbad == 2) QV_VEC_DEOPT("move from an unknown position");
          if (!C->vbad) {
            int j = op->opcode == OP_Next ? qv_vec_next(V, C->vi) : qv_vec_prev(V, C->vi);
            if (j >= V->first && j < V->n) { C->vi = j; C->nullRow = 0; if (!qv_tick(X)) return 0; pc = op->p2; QJUMP; }
          }
          C->vbad = 1; C->nullRow = 1;
          QNEXT;
        }
        if (C->kind == QVC_TOPN) {
          qv_stale(C);
          if (C->vbad == 2) QV_TOPN_DEOPT("move from an unknown position");
          if (!C->vbad) {
            int j = op->opcode == OP_Next ? C->vi + 1 : C->vi - 1;
            if (j >= 0 && j < C->tn->n) { C->vi = j; C->nullRow = 0; if (!qv_tick(X)) return 0; pc = op->p2; QJUMP; }
          }
          C->vbad = 1; C->nullRow = 1;
          QNEXT;
        }
        rc = op->opcode == OP_Next ? sqlite3BtreeNext(C->bc, op->p3) : sqlite3BtreePrevious(C->bc, op->p3);
        qv_stale(C);
        if (rc == SQLITE_OK) {
          C->nullRow = 0;
          if (!qv_tick(X)) return 0;
          int g = X->rgAt ? X->rgAt[pc] : 0;
          pc = op->p2;
#ifdef QV_REENTER_OPS
          if (g && !X->P->rg[g - 1].off) { int rpc = pc, r = qv_region(X, &X->P->rg[g - 1], &rpc); if (r != 1) { if (r == 3) X->pc0 = rpc; return r; } pc = rpc; }   /* (pc's address not taken: it stays a wasm local) */
#else
          if (g && !X->P->rg[g - 1].off) { int r = qv_region(X, &X->P->rg[g - 1], &pc); if (r != 1) { if (r == 3) X->pc0 = pc; return r; } }   /* the loop's next rows (3: a streamed row, from pc) */
#endif
          QJUMP;
        }
        if (rc != SQLITE_DONE) { qv_why = "next"; return 0; }
        C->nullRow = 1;
        QNEXT;
      }
      case OP_SorterNext: QL(OP_SorterNext) {
        QvCur *C = &Cs[op->p1]; qv_stale(C);
        if (C->srt->vc) {
          rc = sqlite3VdbeSorterNext(X->db, C->srt->vc);
          if (rc == SQLITE_OK) { C->nullRow = 0; if (!qv_tick(X)) return 0; pc = op->p2; QJUMP; }
          if (rc != SQLITE_DONE) { qv_why = "sorter next"; return 0; }
          C->nullRow = 1; QNEXT;
        }
        if (++C->srt->pos < C->srt->n) { C->nullRow = 0; if (!qv_tick(X)) return 0; pc = op->p2; QJUMP; }
        C->nullRow = 1;
        QNEXT;
      }
      case OP_SorterInsert: QL(OP_SorterInsert) if (!qv_sorterins(X, R, Cs, op)) return 0; QNEXT;
      case OP_SorterData: QL(OP_SorterData) {
        QvCur *C = &Cs[op->p1]; QvSort *S = C->srt;
        if (S->vc) {   /* the record copied to the arena: values read from it stay valid until the execution ends, as the in-memory records */
          int nk = 0; const void *k = vdbeSorterRowkey(S->vc->uc.pSorter, &nk);
          if ((S->copied += nk) > QV_SPILL_COPY_MAX) { qv_why = "sorter copy size"; return 0; }
          u8 *c = qv_alloc(&X->A, (size_t)nk + 1); if (!c) { qv_why = "arena"; return 0; }
          memcpy(c, k, (size_t)nk); qv_static(qv_out(X, op->p2), c, nk, 0, 0); qv_stale(&Cs[op->p3]);
          QNEXT;
        }
        if (S->pos >= S->n) { qv_why = "sorter data"; return 0; }
        qv_static(qv_out(X, op->p2), S->rec[S->pos], (int)S->len[S->pos], 0, 0);
        qv_stale(&Cs[op->p3]);
        QNEXT;
      }
      case OP_SorterCompare: QL(OP_SorterCompare) {
        QvCur *C = &Cs[op->p1]; QvSort *S = C->srt; Mem *x = &R[op->p3]; int nk = op->p4.i;
        if (S->vc) {
          if (sqlite3VdbeSorterCompare(S->vc, x, nk, &res) != SQLITE_OK) { qv_why = "sorter compare"; return 0; }
          if (res) { pc = op->p2; QJUMP; }
          QNEXT;
        }
        if (S->pos >= S->n || !(x->flags & MEM_Blob)) { qv_why = "sorter compare"; return 0; }
        UnpackedRecord *u = sqlite3VdbeAllocUnpackedRecord(C->ki); if (!u) return 0;
        u->nField = (u16)nk; sqlite3VdbeRecordUnpack((int)S->len[S->pos], S->rec[S->pos], u);
        res = 0; for (int i = 0; i < nk && !res; i++) if (u->aMem[i].flags & MEM_Null) res = -1;
        if (!res) res = sqlite3VdbeRecordCompare(x->n, x->z, u);
        sqlite3DbFree(X->db, u);
        if (res) { pc = op->p2; QJUMP; }
        QNEXT;
      }
      case OP_Sequence: QL(OP_Sequence) qv_int(qv_out(X, op->p2), Cs[op->p1].seqCount++); QNEXT;
      case OP_SequenceTest: QL(OP_SequenceTest) if ((Cs[op->p1].seqCount++) == 0) { pc = op->p2; QJUMP; } QNEXT;
      case OP_SeekLT: QL(OP_SeekLT) case OP_SeekLE: QL(OP_SeekLE) case OP_SeekGE: QL(OP_SeekGE) case OP_SeekGT: QL(OP_SeekGT) {
        QvCur *C = &Cs[op->p1]; int oc = op->opcode, eqOnly = 0;
        C->nullRow = 0; C->deferred = 0; qv_stale(C);
        if (C->isTable) {
          Mem *pIn3 = &R[op->p3]; u16 flags3 = pIn3->flags, newType; i64 iKey;
          if ((flags3 & (MEM_Int | MEM_Real | MEM_IntReal | MEM_Str)) == MEM_Str) applyNumericAffinity(pIn3, 0);
          iKey = sqlite3VdbeIntValue(pIn3); newType = pIn3->flags; pIn3->flags = flags3;
          if ((newType & (MEM_Int | MEM_IntReal)) == 0) {
            if ((newType & MEM_Real) == 0) {
              if ((newType & MEM_Null) || oc >= OP_SeekGE) { pc = op->p2; QJUMP; }
              if (sqlite3BtreeLast(C->bc, &res) != SQLITE_OK) { qv_why = "seek"; return 0; }
              goto seek_not_found;
            }
            int c = sqlite3IntFloatCompare(iKey, pIn3->u.r);
            if (c > 0) { if ((oc & 0x0001) == (OP_SeekGT & 0x0001)) oc--; }
            else if (c < 0) { if ((oc & 0x0001) == (OP_SeekLT & 0x0001)) oc++; }
          }
          if (sqlite3BtreeTableMoveto(C->bc, (u64)iKey, 0, &res) != SQLITE_OK) { qv_why = "seek"; return 0; }
          C->movetoTarget = iKey;
        } else {
          eqOnly = C->eqOnly;
          UnpackedRecord r; memset(&r, 0, sizeof r);
          r.pKeyInfo = C->ki; r.nField = (u16)op->p4.i; r.default_rc = ((1 & (oc - OP_SeekLT)) ? -1 : +1); r.aMem = &R[op->p3];
          if (sqlite3BtreeIndexMoveto(C->bc, &r, &res) != SQLITE_OK) { qv_why = "seek"; return 0; }
          if (eqOnly && !r.eqSeen) goto seek_not_found;
        }
        if (oc >= OP_SeekGE) {
          if (res < 0 || (res == 0 && oc == OP_SeekGT)) {
            res = 0; rc = sqlite3BtreeNext(C->bc, 0);
            if (rc != SQLITE_OK) { if (rc == SQLITE_DONE) res = 1; else { qv_why = "seek"; return 0; } }
          } else res = 0;
        } else {
          if (res > 0 || (res == 0 && oc == OP_SeekLT)) {
            res = 0; rc = sqlite3BtreePrevious(C->bc, 0);
            if (rc != SQLITE_OK) { if (rc == SQLITE_DONE) res = 1; else { qv_why = "seek"; return 0; } }
          } else res = sqlite3BtreeEof(C->bc);
        }
      seek_not_found:
        if (res) { pc = op->p2; QJUMP; }
        if (eqOnly) pc++;   /* skip the IdxGT / IdxLT that follows */
        QNEXT;
      }
      case OP_SeekRowid: QL(OP_SeekRowid) case OP_NotExists: QL(OP_NotExists) {
        QvCur *C = &Cs[op->p1]; Mem *pIn3 = &R[op->p3]; i64 iKey;
        if (op->opcode == OP_SeekRowid && !(pIn3->flags & (MEM_Int | MEM_IntReal))) {
          Mem x = pIn3[0];
          if (x.flags & MEM_Str) applyNumericAffinity(&x, 1);
          if (x.flags & MEM_Int) iKey = x.u.i;
          else if (!(x.flags & MEM_Real) || x.u.r < -9223372036854775808.0 || x.u.r > 9223372036854774784.0 || (double)(iKey = sqlite3RealToI64(x.u.r)) != x.u.r) { pc = op->p2; QJUMP; }
        } else iKey = pIn3->u.i;
        if (C->kind == QVC_VEC) {
          int at, found = qv_vec_find(C->vec, iKey, &at) && !C->vec->e[at].dead;
          C->movetoTarget = iKey; C->nullRow = 0; qv_stale(C); C->deferred = 0; C->seekResult = !found;
          if (found) { C->vi = at; C->vbad = 0; QNEXT; }
          C->vbad = 2;   /* a B-tree would be on a neighbour: what comes next must reposition */
          if (!op->p2) { qv_why = "corrupt"; return 0; }
          pc = op->p2; QJUMP;
        }
        if (sqlite3BtreeTableMoveto(C->bc, iKey, 0, &res) != SQLITE_OK) { qv_why = "seekrowid"; return 0; }
        C->movetoTarget = iKey; C->nullRow = 0; qv_stale(C); C->deferred = 0; C->seekResult = res;
        if (res) { if (!op->p2) { qv_why = "corrupt"; return 0; } pc = op->p2; QJUMP; }
        QNEXT;
      }
      case OP_IdxLE: QL(OP_IdxLE) case OP_IdxGT: QL(OP_IdxGT) case OP_IdxLT: QL(OP_IdxLT) case OP_IdxGE: QL(OP_IdxGE)
        if ((res = qv_idxcmp(X, R, Cs, op)) < 0) return 0;
        if (res) { pc = op->p2; QJUMP; }
        QNEXT;
      case OP_DeferredSeek: QL(OP_DeferredSeek) case OP_IdxRowid: QL(OP_IdxRowid) {
        QvCur *C = &Cs[op->p1];
        if (C->kind == QVC_BT && sqlite3BtreeCursorHasMoved(C->bc)) { int d = 0; if (sqlite3BtreeCursorRestore(C->bc, &d) != SQLITE_OK) return 0; if (d) C->nullRow = 1; }
        if (!C->nullRow) {
          i64 rowid = 0;
          if (sqlite3VdbeIdxRowid(X->db, C->bc, &rowid) != SQLITE_OK) { qv_why = "idxrowid"; return 0; }
          if (op->opcode == OP_DeferredSeek) {
            QvCur *T = &Cs[op->p3];
            T->nullRow = 0; T->movetoTarget = rowid; T->deferred = 1; qv_stale(T);
            T->altMap = op->p4type == P4_INTARRAY ? (const int *)op->p4.ai : 0; T->alt = C;
          } else qv_int(qv_out(X, op->p2), rowid);
        } else if (op->opcode == OP_IdxRowid) qv_out(X, op->p2)->flags = MEM_Null;
        QNEXT;
      }
      case OP_FinishSeek: QL(OP_FinishSeek) { QvCur *C = &Cs[op->p1]; if (C->deferred && !qv_row(X, C)) return 0; QNEXT; }
      case OP_Rowid: QL(OP_Rowid) {
        QvCur *C = &Cs[op->p1]; Mem *o = qv_out(X, op->p2);
        if (C->nullRow) { o->flags = MEM_Null; QNEXT; }
        if (C->deferred) { qv_int(o, C->movetoTarget); QNEXT; }
        if (C->kind == QVC_VEC) {
          if (!qv_row(X, C)) return 0;
          if (C->nullRow) o->flags = MEM_Null; else qv_int(o, C->vec->e[C->vi].rowid);
          QNEXT;
        }
        if (C->kind != QVC_BT && C->kind != QVC_EPH) { qv_why = "rowid cursor"; return 0; }
        if (sqlite3BtreeCursorHasMoved(C->bc)) { int d = 0; if (sqlite3BtreeCursorRestore(C->bc, &d) != SQLITE_OK) return 0; if (d) { C->nullRow = 1; o->flags = MEM_Null; QNEXT; } }
        qv_int(o, sqlite3BtreeIntegerKey(C->bc));
        QNEXT;
      }
      case OP_Column: QL(OP_Column)
        if (!qv_colop(X, R, Cs, op)) return 0;
        if (QV_FB) X->fb[pc] |= qv_fbc(&R[op->p3]);
        QNEXT;
      case OP_RowData: QL(OP_RowData) {
        QvCur *C = &Cs[op->p1];
        if (!qv_row(X, C)) return 0;
        if (C->kind == QVC_VEC) {   /* the record is already in the arena */
          if (C->nullRow) QV_VEC_DEOPT("rowdata of a missing row");
          qv_static(qv_out(X, op->p2), C->row, (int)C->payload, 0, 0); QNEXT;
        }
        u32 sz = C->ovfl ? C->ov->ovPay : C->payload;
        u8 *d = qv_alloc(&X->A, sz + 1); if (!d) { qv_why = "arena"; return 0; }
        if (C->ovfl) { if (sqlite3BtreePayload(C->bc, 0, sz, d) != SQLITE_OK) { qv_why = "payload"; return 0; } }   /* (only the cell is at C->row) */
        else memcpy(d, C->row, sz);
        qv_static(qv_out(X, op->p2), d, (int)sz, 0, 0);
        QNEXT;
      }
      case OP_MakeRecord: QL(OP_MakeRecord) if (!qv_mkrec(X, R, Cs, op)) return 0; QNEXT;
      case OP_IdxInsert: QL(OP_IdxInsert) {
        QvCur *C = &Cs[op->p1]; Mem *k = &R[op->p2];
        if (C->kind == QVC_HASH && (k->flags & MEM_Blob) && !(k->flags & MEM_Zero)) {
          const u8 *rec = (const u8 *)k->z;
          if (!(k->flags & MEM_Static)) { u8 *c = qv_alloc(&X->A, (size_t)k->n + 1); if (!c) { qv_why = "arena"; return 0; } memcpy(c, k->z, (size_t)k->n); rec = c; }
          if (!qv_hs_insert(X, C->hs, rec, (u32)k->n, C->hk)) return 0;
          QNEXT;
        }
        if (C->kind == QVC_TOPN && (k->flags & MEM_Blob) && !(k->flags & MEM_Zero)) {   /* binary search; an equal key replaces, as in a B-tree */
          QvTopn *T = C->tn; UnpackedRecord *u = T->u; const u8 *rec = (const u8 *)k->z;
          if (!(k->flags & MEM_Static)) { u8 *c = qv_alloc(&X->A, (size_t)k->n + 1); if (!c) { qv_why = "arena"; return 0; } memcpy(c, k->z, (size_t)k->n); rec = c; }
          u->nField = (u16)(C->ki->nKeyField + 1); u->default_rc = 0; u->errCode = 0;
          sqlite3VdbeRecordUnpack(k->n, rec, u);
          RecordCompare xc = sqlite3VdbeFindCompare(u);   /* (as a B-tree's seek compares) */
          int lo = 0, hi = T->n, eq = 0, c;
          if (hi && (c = xc((int)T->len[0], T->rec[0], u)) >= 0) { hi = 0; eq = !c; }   /* (a DESC top-N's new rows: first) */
          while (lo < hi) {
            int m = (lo + hi) >> 1; c = xc((int)T->len[m], T->rec[m], u);
            if (c < 0) lo = m + 1; else { hi = m; eq = !c; }
          }
          if (u->errCode) { qv_why = "idx compare"; return 0; }
          if (eq && lo < T->n) { T->rec[lo] = rec; T->len[lo] = (u32)k->n; }
          else if (T->n == QV_TOPN_MAX) { if (!qv_topn_spill(X, C)) return 0; goto idxinsert_eph; }
          else {
            memmove(&T->rec[lo + 1], &T->rec[lo], sizeof T->rec[0] * (size_t)(T->n - lo)); memmove(&T->len[lo + 1], &T->len[lo], sizeof T->len[0] * (size_t)(T->n - lo));
            T->rec[lo] = rec; T->len[lo] = (u32)k->n; T->n++;
          }
          C->vbad = 2; qv_stale(C);   /* (the B-tree cursor's position after an insert: not relied on) */
          QNEXT;
        }
      idxinsert_eph:
        if (C->kind != QVC_EPH || !(k->flags & MEM_Blob) || (k->flags & MEM_Zero)) { qv_why = "idxinsert"; return 0; }
        BtreePayload x; memset(&x, 0, sizeof x); x.pKey = k->z; x.nKey = k->n;
        if (op->p3 > 0 && op->p4.i > 0) {   /* the key's registers, as vdbe.c's: the B-tree compares them, not the record unpacked again
                                               (MakeRecord applied their affinity in place; jellyfin genres' IN chain, 1.4 M a run unpacking) */
          int a = 1; for (int j = 0; j < op->p4.i; j++) if (X->P->isAcc[op->p3 + j]) { a = 0; break; }   /* (a native counter's Mem isn't its value) */
          if (a) { x.aMem = &R[op->p3]; x.nMem = (u16)op->p4.i; }
        }
        rc = sqlite3BtreeInsert(C->bc, &x, op->p5 & (OPFLAG_APPEND | OPFLAG_SAVEPOSITION), (op->p5 & OPFLAG_USESEEKRESULT) ? C->seekResult : 0);
        qv_stale_eph(X);
        if (rc) { qv_why = "idxinsert"; return 0; }
        QNEXT;
      }
      case OP_Insert: QL(OP_Insert) {
        QvCur *C = &Cs[op->p1]; Mem *d = &R[op->p2], *k = &R[op->p3];
        if (C->kind == QVC_VEC) {
          QvVec *V = C->vec; i64 key = k->u.i; int at;
          if (!(k->flags & MEM_Int) || (!(d->flags & MEM_Blob) && !qv_isnull(d)) || (d->flags & MEM_Zero)) { qv_why = "insert"; return 0; }
          const u8 *rec = qv_isnull(d) ? (const u8 *)"" : (const u8 *)d->z; u32 len = qv_isnull(d) ? 0 : (u32)d->n;
          if (len && !(d->flags & MEM_Static)) { u8 *c = qv_alloc(&X->A, len + 1); if (!c) { qv_why = "arena"; return 0; } memcpy(c, rec, len); rec = c; }
          if (V->live == 0 || key > V->e[V->n - 1].rowid) {   /* the common case: an append */
            if (V->live == 0) V->n = V->first = 0;
            if (V->n == V->cap && !qv_vec_grow(X, V)) return 0;
            V->e[V->n].rowid = key; V->e[V->n].rec = rec; V->e[V->n].len = len; V->e[V->n].dead = 0;
            C->vi = V->n++; V->live++;
          } else if (qv_vec_find(V, key, &at) && !V->e[at].dead) {   /* a replace */
            V->e[at].rec = rec; V->e[at].len = len; C->vi = at; qv_vec_stale(X, V);
          } else QV_VEC_DEOPT("insert into the middle");
          C->vbad = 0; C->deferred = 0; qv_stale(C);
          QNEXT;
        }
        if (C->kind != QVC_EPH || !(k->flags & MEM_Int) || (!(d->flags & MEM_Blob) && !qv_isnull(d)) || (d->flags & MEM_Zero)) { qv_why = "insert"; return 0; }
        BtreePayload x; memset(&x, 0, sizeof x); x.nKey = k->u.i; x.pData = qv_isnull(d) ? 0 : d->z; x.nData = qv_isnull(d) ? 0 : d->n;
        rc = sqlite3BtreeInsert(C->bc, &x, op->p5 & (OPFLAG_APPEND | OPFLAG_SAVEPOSITION), (op->p5 & OPFLAG_USESEEKRESULT) ? C->seekResult : 0);
        C->deferred = 0; qv_stale_eph(X);
        if (rc) { qv_why = "insert"; return 0; }
        QNEXT;
      }
      case OP_NewRowid: QL(OP_NewRowid) {
        QvCur *C = &Cs[op->p1]; i64 nv = 1;
        if (C->kind == QVC_VEC) {
          QvVec *V = C->vec;
          if (V->live) { nv = V->e[V->n - 1].rowid; if (nv >= LARGEST_INT64) QV_VEC_DEOPT("rowid range"); nv++; }
          qv_int(qv_out(X, op->p2), nv);
          QNEXT;
        }
        if (C->kind != QVC_EPH) { qv_why = "newrowid"; return 0; }
        if (sqlite3BtreeLast(C->bc, &res) != SQLITE_OK) { qv_why = "newrowid"; return 0; }
        if (!res) { nv = sqlite3BtreeIntegerKey(C->bc); if (nv >= LARGEST_INT64) { qv_why = "rowid range"; return 0; } nv++; }
        qv_int(qv_out(X, op->p2), nv);
        QNEXT;
      }
      case OP_Delete: QL(OP_Delete) {
        QvCur *C = &Cs[op->p1];
        if (C->kind == QVC_VEC) {
          QvVec *V = C->vec;
          if (C->vbad || C->vi < V->first || C->vi >= V->n || V->e[C->vi].dead) QV_VEC_DEOPT("delete of no row");
          V->e[C->vi].dead = 1; V->live--;
          while (V->first < V->n && V->e[V->first].dead) V->first++;
          while (V->n > V->first && V->e[V->n - 1].dead) V->n--;   /* so NewRowid (max rowid + 1) sees what BtreeLast would */
          if (!(op->p5 & OPFLAG_SAVEPOSITION)) C->vbad = 2;   /* a B-tree cursor is left at its root: the next move must reposition */
          qv_vec_stale(X, V); C->seekResult = 0;
          QNEXT;
        }
        if (C->kind == QVC_TOPN) {
          QvTopn *T = C->tn;
          if (C->vbad || C->vi < 0 || C->vi >= T->n) QV_TOPN_DEOPT("delete of no row");
          memmove(&T->rec[C->vi], &T->rec[C->vi + 1], sizeof T->rec[0] * (size_t)(T->n - C->vi - 1)); memmove(&T->len[C->vi], &T->len[C->vi + 1], sizeof T->len[0] * (size_t)(T->n - C->vi - 1));
          T->n--; C->vbad = 2; qv_stale(C); C->seekResult = 0;
          QNEXT;
        }
        if (C->kind != QVC_EPH) { qv_why = "delete"; return 0; }
        rc = sqlite3BtreeDelete(C->bc, op->p5); qv_stale_eph(X); C->seekResult = 0;
        if (rc) { qv_why = "delete"; return 0; }
        QNEXT;
      }
      case OP_ResetSorter: QL(OP_ResetSorter) {
        QvCur *C = &Cs[op->p1];
        if (C->kind == QVC_HASH) { memset(C->hs, 0, sizeof *C->hs); QNEXT; }
        if (C->kind == QVC_SORT) { if (C->srt->vc) qv_sort_unspill(X->db, C->srt); C->srt->n = 0; C->srt->pos = 0; C->srt->mem = 0; }
        else if (C->kind == QVC_VEC) { QvVec *V = C->vec; V->n = V->first = V->live = 0; C->vbad = 1; qv_vec_stale(X, V); }
        else if (C->kind == QVC_TOPN) { C->tn->n = 0; C->vbad = 1; }
        else if (C->kind == QVC_EPH) { if (sqlite3BtreeClearTableOfCursor(C->bc) != SQLITE_OK) { qv_why = "reset"; return 0; } qv_stale_eph(X); }
        else { qv_why = "reset cursor"; return 0; }
        qv_stale(C);
        QNEXT;
      }
      case OP_IfNoHope: QL(OP_IfNoHope) if (Cs[op->p1].seekHit >= op->p4.i) QNEXT; /* fall through */
      case OP_NoConflict: QL(OP_NoConflict) case OP_NotFound: QL(OP_NotFound) case OP_Found: QL(OP_Found) {
        QvCur *C = &Cs[op->p1];
        if (!qv_found(X, C, op)) return 0;
        int exists = C->seekResult == 0;
        C->nullRow = (u8)!exists; C->deferred = 0; qv_stale(C);
        if (op->opcode == OP_Found) { if (exists) { pc = op->p2; QJUMP; } }
        else {
          if (!exists) { pc = op->p2; QJUMP; }
          if (op->opcode == OP_NoConflict) {
            int jn = 0; for (int i = 0; i < op->p4.i; i++) if (qv_isnull(&R[op->p3 + i])) jn = 1;
            if (jn) { pc = op->p2; QJUMP; }
          }
          if (op->opcode == OP_IfNoHope) C->seekHit = op->p4.i;
        }
        QNEXT;
      }
      case OP_SeekHit: QL(OP_SeekHit) {
        QvCur *C = &Cs[op->p1];
        if (C->seekHit < op->p2) C->seekHit = op->p2; else if (C->seekHit > op->p3) C->seekHit = op->p3;
        QNEXT;
      }
      /* ---- rowid sets (OR terms over several indexes), as vdbe.c: SQLite's own RowSet in the register */
      case OP_ClrSubtype: QL(OP_ClrSubtype) R[op->p1].flags &= ~MEM_Subtype; QNEXT;   /* as vdbe.c */
      case OP_RowSetAdd: QL(OP_RowSetAdd) {
        Mem *s = &R[op->p1];
        if (!(s->flags & MEM_Blob) && sqlite3VdbeMemSetRowSet(s)) { qv_why = "oom"; return 0; }
        sqlite3RowSetInsert((RowSet *)s->z, R[op->p2].u.i);
        QNEXT;
      }
      case OP_RowSetRead: QL(OP_RowSetRead) {
        Mem *s = &R[op->p1]; i64 val;
        if (!(s->flags & MEM_Blob) || sqlite3RowSetNext((RowSet *)s->z, &val) == 0) { sqlite3VdbeMemSetNull(s); pc = op->p2; QJUMP; }
        qv_int(qv_out(X, op->p3), val);
        QNEXT;
      }
      case OP_RowSetTest: QL(OP_RowSetTest) {
        Mem *s = &R[op->p1]; int iSet = op->p4.i;
        if (!(s->flags & MEM_Blob) && sqlite3VdbeMemSetRowSet(s)) { qv_why = "oom"; return 0; }
        if (iSet && sqlite3RowSetTest((RowSet *)s->z, iSet, R[op->p3].u.i)) { pc = op->p2; QJUMP; }
        if (iSet >= 0) sqlite3RowSetInsert((RowSet *)s->z, R[op->p3].u.i);
        QNEXT;
      }
      case OP_Count: QL(OP_Count) {
        QvCur *C = &Cs[op->p1]; i64 n = 0;
        if (C->kind == QVC_VEC) n = C->vec->live;
        else if (op->p3) n = sqlite3BtreeRowCountEst(C->bc);
        else if (sqlite3BtreeCount(X->db, C->bc, &n) != SQLITE_OK) { qv_why = "count"; return 0; }
        qv_int(qv_out(X, op->p2), n);
        QNEXT;
      }
      default: QL(default) qv_why = sqlite3OpcodeName(op->opcode); return 0;
    }
  qv_next: QV_UNUSED_LABEL;
    pc++;
  }
}

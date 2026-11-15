/* sqlite_shim.c: the pinned SQLite amalgamation, plus a plain function for
 * each private call or field db.nx uses (sqlite_shim.h lists them).
 *
 * The options are the C entry's (its build.sh SQLITE_OPTS), defined here
 * rather than on the compiler's command line so that every build of the
 * Nexium entry compiles SQLite the same way. -DNDEBUG is not needed:
 * sqlite3.c defines NDEBUG itself unless SQLITE_DEBUG is set. */
#define SQLITE_THREADSAFE 2
#define SQLITE_DEFAULT_MEMSTATUS 0
#define SQLITE_DQS 0
#define SQLITE_DEFAULT_WAL_SYNCHRONOUS 1
#define SQLITE_TEMP_STORE 3
#define SQLITE_USE_ALLOCA 1
#define SQLITE_LIKE_DOESNT_MATCH_BLOBS 1
#define SQLITE_MAX_EXPR_DEPTH 0
#define SQLITE_OMIT_DEPRECATED 1
#define SQLITE_OMIT_SHARED_CACHE 1
#define SQLITE_OMIT_LOAD_EXTENSION 1
#define SQLITE_OMIT_PROGRESS_CALLBACK 1
#define SQLITE_OMIT_DECLTYPE 1
#define SQLITE_OMIT_JSON 1

#include "sqlite3.c"
#include "sqlite_shim.h"

/* The literals in sqlite_shim.h must be SQLite's own values. */
_Static_assert(SHIM_MEM_INT == MEM_Int, "MEM_Int");
_Static_assert(SHIM_KEYINFO_ORDER_DESC == KEYINFO_ORDER_DESC, "KEYINFO_ORDER_DESC");
_Static_assert(SHIM_BTREE_WRCSR == BTREE_WRCSR, "BTREE_WRCSR");
_Static_assert(SHIM_BTREE_APPEND == BTREE_APPEND, "BTREE_APPEND");
_Static_assert(SHIM_SIZEOF_MEM == sizeof(Mem), "sizeof(Mem)");

Btree *shim_main_btree(sqlite3 *db) { return db->aDb[0].pBt; }
Schema *shim_main_schema(sqlite3 *db) { return db->aDb[0].pSchema; }
int shim_schema_file_format(Schema *s) { return s->file_format; }
int shim_db_enc(sqlite3 *db) { return ENC(db); }

KeyInfo *shim_keyinfo_alloc(sqlite3 *db, int n_key, int n_extra) { return sqlite3KeyInfoAlloc(db, n_key, n_extra); }
uint8_t *shim_keyinfo_sort_flags(KeyInfo *ki) { return ki->aSortFlags; }
UnpackedRecord *shim_unpacked_alloc(KeyInfo *ki) { return sqlite3VdbeAllocUnpackedRecord(ki); }
sqlite3_value *shim_unpacked_mem(UnpackedRecord *r, int i) { return &r->aMem[i]; }
uint16_t *shim_unpacked_nfield(UnpackedRecord *r) { return &r->nField; }
int8_t *shim_unpacked_default_rc(UnpackedRecord *r) { return &r->default_rc; }
uint8_t *shim_unpacked_eq_seen(UnpackedRecord *r) { return &r->eqSeen; }
uint8_t *shim_unpacked_err_code(UnpackedRecord *r) { return &r->errCode; }
uint16_t *shim_mem_flags(sqlite3_value *m) { return &m->flags; }
/* u.i is SQLite's i64, a long long. db.nx writes it through an int64_t *
 * (a long), the type @cImport gives an i64: the same size and representation
 * here, and the C Nexium emits is compiled with -fno-strict-aliasing. */
int64_t *shim_mem_int(sqlite3_value *m) { return (int64_t *)&m->u.i; }
uint8_t *shim_mem_enc(sqlite3_value *m) { return &m->enc; }
sqlite3 **shim_mem_db(sqlite3_value *m) { return &m->db; }

int shim_begin_trans(Btree *bt, int wrflag) { return sqlite3BtreeBeginTrans(bt, wrflag, 0); }
int shim_commit(Btree *bt) { return sqlite3BtreeCommit(bt); }

int shim_cursor_size(void) { return sqlite3BtreeCursorSize(); }
void shim_cursor_zero(BtCursor *c) { sqlite3BtreeCursorZero(c); }
int shim_cursor_open(Btree *bt, uint32_t root, int flags, KeyInfo *ki, BtCursor *c) { return sqlite3BtreeCursor(bt, root, flags, ki, c); }
int shim_cursor_close(BtCursor *c) { return sqlite3BtreeCloseCursor(c); }
int shim_first(BtCursor *c, int *empty) { return sqlite3BtreeFirst(c, empty); }
int shim_last(BtCursor *c, int *empty) { return sqlite3BtreeLast(c, empty); }
int shim_next(BtCursor *c, int flags) { return sqlite3BtreeNext(c, flags); }
int shim_previous(BtCursor *c, int flags) { return sqlite3BtreePrevious(c, flags); }
int shim_eof(BtCursor *c) { return sqlite3BtreeEof(c); }
int shim_table_moveto(BtCursor *c, int64_t key, int bias, int *res) { return sqlite3BtreeTableMoveto(c, key, bias, res); }
int shim_index_moveto(BtCursor *c, UnpackedRecord *key, int *res) { return sqlite3BtreeIndexMoveto(c, key, res); }
int64_t shim_integer_key(BtCursor *c) { return sqlite3BtreeIntegerKey(c); }
uint32_t shim_payload_size(BtCursor *c) { return sqlite3BtreePayloadSize(c); }
const uint8_t *shim_payload_fetch(BtCursor *c, uint32_t *amt) { return sqlite3BtreePayloadFetch(c, amt); }
int shim_payload_copy(BtCursor *c, uint32_t offset, uint32_t amt, uint8_t *buf) { return sqlite3BtreePayload(c, offset, amt, buf); }

/* The one wrapper that builds an argument: BtreePayload is private, so the
 * caller passes its fields. Zeroed first, as db.c's put_row and put_index do. */
int shim_insert(BtCursor *c, const uint8_t *key, int64_t nkey, const uint8_t *data, int ndata, int flags, int seek_result) {
    BtreePayload x;
    memset(&x, 0, sizeof x);
    x.pKey = key;
    x.nKey = nkey;
    x.pData = data;
    x.nData = ndata;
    return sqlite3BtreeInsert(c, &x, flags, seek_result);
}

int shim_bind_text_static(sqlite3_stmt *s, int i, const char *p, int n) { return sqlite3_bind_text(s, i, p, n, SQLITE_STATIC); }
int shim_exec(sqlite3 *db, const char *sql, unsigned char **err) { return sqlite3_exec(db, sql, 0, 0, (char **)err); }
int shim_prepare(sqlite3 *db, const char *sql, int n, unsigned flags, sqlite3_stmt **out) { return sqlite3_prepare_v3(db, sql, n, flags, out, 0); }

/* SQLite calls this after every commit in WAL mode. It has to be C: nx 1.5.0
 * cannot hand one of its functions to C as a function pointer. It does what
 * db.c's wal_hook_cb does, into two i64 counters that db.nx owns: the frame
 * count (relaxed, as db.c) and the commit count (release, as db.c). The frame
 * count is stored as db.c's uint32_t would hold it. */
static int shim_wal_cb(void *ctx, sqlite3 *db, const char *name, int nframe) {
    int64_t *w = (int64_t *)ctx;
    (void)db, (void)name;
    __atomic_store_n(&w[0], (int64_t)(uint32_t)nframe, __ATOMIC_RELAXED);
    __atomic_fetch_add(&w[1], 1, __ATOMIC_RELEASE);
    return SQLITE_OK;
}
void shim_wal_hook(sqlite3 *db, int64_t *counters) { sqlite3_wal_hook(db, shim_wal_cb, counters); }

const char *shim_err_str(int rc) { return sqlite3ErrStr(rc); }

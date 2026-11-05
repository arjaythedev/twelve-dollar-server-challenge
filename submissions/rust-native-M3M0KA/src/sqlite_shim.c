/* Compiled in the same translation unit as the SQLite amalgamation, which is
 * the only way to reach SQLite's internal B-tree API. SQLite itself is not
 * modified: the accessors below only read through its pager, WAL and locking,
 * inside the transaction SQLite already owns.
 *
 * Writes always go through ordinary SQL. Reads may use these cursors to skip
 * the bytecode interpreter for plans whose shape is known ahead of time. */

#include "sqlite3.c"

void *feed_btree(sqlite3 *db)
{
    return (void *)db->aDb[0].pBt;
}

int feed_cursor_size(void)
{
    return sqlite3BtreeCursorSize();
}

/* KeyInfo for an index whose leading key columns use the default (BINARY)
 * collation, which is what schema.sql declares.
 *
 * `nextra` must cover the trailing rowid: a non-unique index compares its key
 * columns and then the rowid, so an index on one column needs room for two
 * collations or the comparison reads past the allocation. */
void *feed_keyinfo(sqlite3 *db, int nkey, int nextra)
{
    KeyInfo *k = sqlite3KeyInfoAlloc(db, nkey, nextra);
    if (!k) return 0;
    for (int i = 0; i < nkey + nextra; i++) k->aColl[i] = db->pDfltColl;
    k->nKeyField = nkey;
    k->nAllField = nkey + nextra;
    return k;
}

/* KeyInfo for an index whose first `nkey` key columns are all DESC, as
 * posts_created_at_id_idx declares. Sequential walks do not compare, but the
 * cursor must be opened as an index cursor, which requires key information. */
void *feed_keyinfo_desc(sqlite3 *db, int nkey, int nextra)
{
    KeyInfo *k = (KeyInfo *)feed_keyinfo(db, nkey, nextra);
    if (!k) return 0;
    for (int i = 0; i < nkey; i++) k->aSortFlags[i] = KEYINFO_ORDER_DESC;
    return k;
}

/* Forces the read transaction open. The data version is reported for callers
 * that ask for it; nothing in the read plans depends on it. */
int feed_begin_read(void *bt, unsigned *version)
{
    int rc = sqlite3BtreeBeginTrans((Btree *)bt, 0, 0);
    if (rc != SQLITE_OK) return rc;
    if (version) {
        u32 v = 0;
        sqlite3BtreeGetMeta((Btree *)bt, BTREE_DATA_VERSION, &v);
        *version = v;
    }
    return 0;
}

int feed_cursor_open(void *bt, unsigned root, void *mem, void *keyinfo)
{
    BtCursor *c = (BtCursor *)mem;
    sqlite3BtreeCursorZero(c);
    return sqlite3BtreeCursor((Btree *)bt, root, 0, (KeyInfo *)keyinfo, c);
}

int feed_cursor_close(void *mem)
{
    return sqlite3BtreeCloseCursor((BtCursor *)mem);
}

/* KeyInfo holds a reference owned by the caller; released when a plan is
 * disabled or at shutdown. */
void feed_keyinfo_free(void *k)
{
    if (k) sqlite3KeyInfoUnref((KeyInfo *)k);
}

int feed_first(void *mem)
{
    int empty = 0;
    if (sqlite3BtreeFirst((BtCursor *)mem, &empty) != SQLITE_OK) return -1;
    return empty ? 1 : 0;
}

int feed_last(void *mem)
{
    int empty = 0;
    if (sqlite3BtreeLast((BtCursor *)mem, &empty) != SQLITE_OK) return -1;
    return empty ? 1 : 0;
}

int feed_next(void *mem)
{
    int rc = sqlite3BtreeNext((BtCursor *)mem, 0);
    return rc == SQLITE_OK ? 0 : rc == SQLITE_DONE ? 1 : -1;
}

int feed_prev(void *mem)
{
    int rc = sqlite3BtreePrevious((BtCursor *)mem, 0);
    return rc == SQLITE_OK ? 0 : rc == SQLITE_DONE ? 1 : -1;
}

long long feed_rowid(void *mem)
{
    return (long long)sqlite3BtreeIntegerKey((BtCursor *)mem);
}

int feed_seek(void *mem, long long rowid)
{
    int res = 0;
    if (sqlite3BtreeTableMoveto((BtCursor *)mem, (i64)rowid, 0, &res) != SQLITE_OK) return -1;
    return res == 0 ? 0 : 1;
}

/* Positions on the first index entry whose leading integer key is >= k.
 * 0 = on an entry, 1 = none, -1 = error. */
int feed_index_ge(void *mem, void *keyinfo, long long k)
{
    Mem m;
    UnpackedRecord r;
    int res = 0;
    memset(&m, 0, sizeof m);
    m.flags = MEM_Int;
    m.u.i = (i64)k;
    memset(&r, 0, sizeof r);
    r.pKeyInfo = (KeyInfo *)keyinfo;
    r.aMem = &m;
    r.nField = 1;
    r.default_rc = 1;
    if (sqlite3BtreeIndexMoveto((BtCursor *)mem, &r, &res) != SQLITE_OK) return -1;
    if (res < 0) return feed_next(mem);
    return sqlite3BtreeEof((BtCursor *)mem) ? 1 : 0;
}

/* KeyInfo holds a reference owned by the caller; released when a plan is
 * disabled or at shutdown. */
/* Record image of the entry the cursor is on.
 *
 * The fast path returns a pointer straight into the page. When the record is
 * not entirely local (SQLite reports fewer available bytes than the payload
 * size) it is copied into the caller's scratch buffer instead. Returns NULL
 * only if the scratch buffer is too small. */
const unsigned char *feed_payload(void *mem, unsigned char *scratch,
                                  unsigned cap, unsigned *len)
{
    BtCursor *c = (BtCursor *)mem;
    unsigned total = sqlite3BtreePayloadSize(c);
    unsigned local = 0;
    const unsigned char *p = sqlite3BtreePayloadFetch(c, &local);
    if (p && local == total) {
        *len = total;
        return p;
    }
    if (total > cap) {
        *len = 0;
        return 0;
    }
    if (sqlite3BtreePayload(c, 0, (int)total, scratch) != SQLITE_OK) {
        *len = 0;
        return 0;
    }
    *len = total;
    return scratch;
}

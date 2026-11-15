/* sqlite_shim.h: the private SQLite calls db.nx makes, as plain functions.
 *
 * db.c reaches into SQLite's B-tree layer (cursors, record keys, inserts),
 * which the amalgamation keeps static. sqlite_shim.c includes sqlite3.c and
 * exports one wrapper per call or field, so db.nx can make the same calls.
 * A wrapper does one call or touches one field: everything db.c does with
 * them (records, loops, decisions) is in db.nx.
 *
 * db.nx imports this header alone: it includes sqlite3.h, so the public API
 * and these wrappers share one set of types. */
#ifndef SQLITE_SHIM_H
#define SQLITE_SHIM_H

#include <stdint.h>
#include "sqlite3.h"

/* SQLite's private types, opaque here. A Mem is a sqlite3_value. */
typedef struct Btree Btree;
typedef struct BtCursor BtCursor;
typedef struct KeyInfo KeyInfo;
typedef struct UnpackedRecord UnpackedRecord;
typedef struct Schema Schema;

/* Private constants, spelled as literals so @cImport takes them;
 * sqlite_shim.c checks each against sqlite3.c's own definition. */
#define SHIM_MEM_INT 4                /* MEM_Int */
#define SHIM_KEYINFO_ORDER_DESC 1     /* KEYINFO_ORDER_DESC */
#define SHIM_BTREE_WRCSR 4            /* BTREE_WRCSR: a read-write cursor */
#define SHIM_BTREE_APPEND 8           /* BTREE_APPEND: the insert is likely an append */
#define SHIM_SIZEOF_MEM 56            /* sizeof(Mem) */

/* The connection: its main database's B-tree, schema and text encoding. */
Btree *shim_main_btree(sqlite3 *db);              /* db->aDb[0].pBt */
Schema *shim_main_schema(sqlite3 *db);            /* db->aDb[0].pSchema */
int shim_schema_file_format(Schema *s);           /* s->file_format */
int shim_db_enc(sqlite3 *db);                     /* ENC(db) */

/* Key descriptions and search keys for the index cursors. */
KeyInfo *shim_keyinfo_alloc(sqlite3 *db, int n_key, int n_extra);   /* sqlite3KeyInfoAlloc */
uint8_t *shim_keyinfo_sort_flags(KeyInfo *ki);    /* ki->aSortFlags */
UnpackedRecord *shim_unpacked_alloc(KeyInfo *ki); /* sqlite3VdbeAllocUnpackedRecord */
sqlite3_value *shim_unpacked_mem(UnpackedRecord *r, int i);   /* &r->aMem[i] */
uint16_t *shim_unpacked_nfield(UnpackedRecord *r);           /* &r->nField */
int8_t *shim_unpacked_default_rc(UnpackedRecord *r);         /* &r->default_rc */
uint8_t *shim_unpacked_eq_seen(UnpackedRecord *r);           /* &r->eqSeen */
uint8_t *shim_unpacked_err_code(UnpackedRecord *r);          /* &r->errCode */
uint16_t *shim_mem_flags(sqlite3_value *m);       /* &m->flags */
int64_t *shim_mem_int(sqlite3_value *m);          /* &m->u.i, as an int64_t (see sqlite_shim.c) */
uint8_t *shim_mem_enc(sqlite3_value *m);          /* &m->enc */
sqlite3 **shim_mem_db(sqlite3_value *m);          /* &m->db */

/* Transactions on the B-tree. */
int shim_begin_trans(Btree *bt, int wrflag);      /* sqlite3BtreeBeginTrans(bt, wrflag, 0) */
int shim_commit(Btree *bt);                       /* sqlite3BtreeCommit */

/* Cursors. */
int shim_cursor_size(void);                       /* sqlite3BtreeCursorSize */
void shim_cursor_zero(BtCursor *c);               /* sqlite3BtreeCursorZero */
int shim_cursor_open(Btree *bt, uint32_t root, int flags, KeyInfo *ki, BtCursor *c);   /* sqlite3BtreeCursor */
int shim_cursor_close(BtCursor *c);               /* sqlite3BtreeCloseCursor */
int shim_first(BtCursor *c, int *empty);          /* sqlite3BtreeFirst */
int shim_last(BtCursor *c, int *empty);           /* sqlite3BtreeLast */
int shim_next(BtCursor *c, int flags);            /* sqlite3BtreeNext */
int shim_previous(BtCursor *c, int flags);        /* sqlite3BtreePrevious */
int shim_eof(BtCursor *c);                        /* sqlite3BtreeEof */
int shim_table_moveto(BtCursor *c, int64_t key, int bias, int *res);    /* sqlite3BtreeTableMoveto */
int shim_index_moveto(BtCursor *c, UnpackedRecord *key, int *res);      /* sqlite3BtreeIndexMoveto */
int64_t shim_integer_key(BtCursor *c);            /* sqlite3BtreeIntegerKey */
uint32_t shim_payload_size(BtCursor *c);          /* sqlite3BtreePayloadSize */
const uint8_t *shim_payload_fetch(BtCursor *c, uint32_t *amt);          /* sqlite3BtreePayloadFetch */
int shim_payload_copy(BtCursor *c, uint32_t offset, uint32_t amt, uint8_t *buf);   /* sqlite3BtreePayload */
/* sqlite3BtreeInsert: a table row (key = rowid, data) or an index entry
 * (key, nkey = its length, no data). SQLite takes these in a private
 * struct, BtreePayload, which this fills from the arguments. */
int shim_insert(BtCursor *c, const uint8_t *key, int64_t nkey, const uint8_t *data, int ndata, int flags, int seek_result);

/* Public calls @cImport cannot make. bind_text and exec take a C function
 * pointer (SQLITE_STATIC, a row callback), which it cannot translate. exec
 * and prepare take a char **, which it spells uint8_t ** in the C it emits,
 * and gcc 14 and later reject that as an incompatible pointer type. So these
 * take unsigned char ** (what uint8_t ** is), or leave the argument out. */
int shim_bind_text_static(sqlite3_stmt *s, int i, const char *p, int n);   /* sqlite3_bind_text(.., SQLITE_STATIC) */
int shim_exec(sqlite3 *db, const char *sql, unsigned char **err);          /* sqlite3_exec(db, sql, 0, 0, err) */
int shim_prepare(sqlite3 *db, const char *sql, int n, unsigned flags, sqlite3_stmt **out);   /* sqlite3_prepare_v3(.., out, 0) */

/* sqlite3_wal_hook with the shim's callback: after each commit it stores the
 * WAL's frame count in counters[0] (a relaxed store) and adds one to
 * counters[1] (a release add), the two i64s db.nx reads with sync.atomic_load. */
void shim_wal_hook(sqlite3 *db, int64_t *counters);

const char *shim_err_str(int rc);                 /* sqlite3ErrStr */

#endif

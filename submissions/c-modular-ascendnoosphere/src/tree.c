/* SQLite internals and cursors share the pager's WAL snapshot. */
#include "sqlite3.c"
#include "tree.h"
#include "record.h"
_Static_assert(SQLITE_VERSION_NUMBER==3054001,"Review the B-tree binding when upgrading SQLite");
#ifndef ASC_TREE_BACKSTEP
#define ASC_TREE_BACKSTEP 1
#endif
#ifndef ASC_TREE_REVERSE_COUNTS
#define ASC_TREE_REVERSE_COUNTS 1
#endif

struct AscTree {
    sqlite3 *db;
    Btree *bt;
    int cookie;
    Pgno roots[4];                 /* posts, users, feed, likes */
    KeyInfo *feed_key,*likes_key;
    Buffer scratch;
    unsigned flags;
    int (*decode)(const unsigned char *,size_t,unsigned,AscField[4]);
};
typedef struct {
    int64_t id,likes,user;
    uint32_t body,body_n,created,created_n,author,author_n;
} AscRow;
typedef struct { int active,key_valid; int64_t previous,key; } AscCountState;

static int asc_table(Table *table,const char *const *columns,unsigned n) {
    if (!table || table->eTabType!=TABTYP_NORM || !HasRowid(table) ||
        table->nCol!=(int)n || table->nNVCol!=(int)n || table->tnum<2 ||
        (table->tabFlags&TF_HasGenerated)) return 0;
    for (unsigned i=0;i<n;i++) if (strcmp(table->aCol[i].zCnName,columns[i])) return 0;
    return 1;
}
static int asc_index(Index *index,Table *table,const int *columns,const unsigned *desc,unsigned n) {
    if (!index || index->pTable!=table || index->tnum<2 || index->nKeyCol!=n ||
        index->nColumn!=n+1 || index->pPartIdxWhere || index->bHasExpr) return 0;
    for (unsigned i=0;i<n;i++)
        if (index->aiColumn[i]!=columns[i] || index->aSortOrder[i]!=desc[i] ||
            sqlite3_stricmp(index->azColl[i],"BINARY")) return 0;
    return index->aiColumn[n]==XN_ROWID;
}
static KeyInfo *asc_key(sqlite3 *db,Index *index) {
    KeyInfo *key=sqlite3KeyInfoAlloc(db,index->nKeyCol,1);
    if (!key) return NULL;
    for (unsigned i=0;i<index->nColumn;i++) {
        key->aColl[i]=db->pDfltColl;
        key->aSortFlags[i]=index->aSortOrder[i];
    }
    return key;
}
AscTree *asc_tree_open(sqlite3 *db,unsigned flags) {
    const char *const pc[]={"id","user_id","body","created_at"};
    const char *const uc[]={"id","username","created_at"};
    const char *const lc[]={"user_id","post_id","created_at"};
    Table *posts=sqlite3FindTable(db,"posts","main"), *users=sqlite3FindTable(db,"users","main");
    Table *likes=sqlite3FindTable(db,"likes","main");
    Index *feed=sqlite3FindIndex(db,"posts_created_at_id_idx","main");
    Index *counts=sqlite3FindIndex(db,"likes_post_id_idx","main");
    const int fc[]={3,0}, cc[]={1};
    const unsigned fd[]={1,1}, cd[]={0};
    if (ENC(db)!=SQLITE_UTF8 || !asc_table(posts,pc,4) || !asc_table(users,uc,3) ||
        !asc_table(likes,lc,3) || posts->iPKey!=0 || users->iPKey!=0 ||
        !asc_index(feed,posts,fc,fd,2) || !asc_index(counts,likes,cc,cd,1)) return NULL;
    AscTree *tree=calloc(1,sizeof(*tree));
    if (!tree) return NULL;
    tree->db=db; tree->bt=db->aDb[0].pBt; tree->cookie=db->aDb[0].pSchema->schema_cookie;
    tree->flags=flags;
    tree->roots[0]=posts->tnum; tree->roots[1]=users->tnum;
    tree->roots[2]=feed->tnum; tree->roots[3]=counts->tnum;
    tree->feed_key=asc_key(db,feed); tree->likes_key=asc_key(db,counts);
    tree->decode=asc_record_scalar;
#if ASC_X86
    __builtin_cpu_init();
    if ((flags&1u) && __builtin_cpu_supports("ssse3") && __builtin_cpu_supports("sse4.1")) tree->decode=asc_record_vector;
#endif
    if (!tree->feed_key || !tree->likes_key) { asc_tree_close(tree); return NULL; }
    return tree;
}
void asc_tree_close(AscTree *tree) {
    if (!tree) return;
    sqlite3KeyInfoUnref(tree->feed_key); sqlite3KeyInfoUnref(tree->likes_key);
    buffer_free(&tree->scratch); free(tree);
}
static inline __attribute__((always_inline)) const unsigned char *asc_payload(AscTree *tree,BtCursor *cursor,unsigned count,AscField fields[4]) {
    unsigned size=sqlite3BtreePayloadSize(cursor), local=0;
    const unsigned char *p=sqlite3BtreePayloadFetch(cursor,&local);
    /* Overflow payloads and uncommon record types fall back to SQL. */
    if (!p || local<size) return NULL;
    int decoded=tree->decode==asc_record_scalar?
        asc_record_scalar(p,size,count,fields):tree->decode(p,size,count,fields);
    if (!decoded) return NULL;
    return p;
}
static int asc_index_key(AscTree *tree,BtCursor *cursor,int64_t *id) {
    unsigned size=sqlite3BtreePayloadSize(cursor), local=0;
    const unsigned char *p=sqlite3BtreePayloadFetch(cursor,&local);
    if (!p || local<size) return 0;
    if (asc_record_integer_pair(p,size,id)) return 1;
    AscField fields[4];
    int decoded=tree->decode==asc_record_scalar?
        asc_record_scalar(p,size,2,fields):tree->decode(p,size,2,fields);
    return decoded && asc_record_integer(p,fields[0],id);
}
static int asc_text(AscTree *tree,const unsigned char *p,AscField field,uint32_t *offset,uint32_t *n) {
    if (field.type<13 || !(field.type&1)) return 0;
    *offset=tree->scratch.n; *n=field.length;
    return buffer_add(&tree->scratch,p+field.offset,field.length);
}
static int asc_count(AscTree *tree,BtCursor *cursor,int64_t id,int64_t *count) {
    Mem value={0}; value.flags=MEM_Int; value.u.i=id;
    UnpackedRecord search={0}; search.pKeyInfo=tree->likes_key; search.aMem=&value;
    search.nField=1; search.default_rc=1; search.r1=-1; search.r2=1;
    int position=0, rc=sqlite3BtreeIndexMoveto(cursor,&search,&position);
    if (rc!=SQLITE_OK) return 0;
    *count=0;
    if (!sqlite3BtreeCursorIsValidNN(cursor)) return 1;
    if (position<0) { rc=sqlite3BtreeNext(cursor,0); if (rc==SQLITE_DONE) return 1; }
    if (rc!=SQLITE_OK) return 0;
    while (sqlite3BtreeCursorIsValidNN(cursor)) {
        int64_t found;
        if (!asc_index_key(tree,cursor,&found)) return 0;
        if (found!=id) return 1;
        if (*count==INT64_MAX) return 0;
        ++*count;
        rc=sqlite3BtreeNext(cursor,0);
        if (rc==SQLITE_DONE) return 1;
        if (rc!=SQLITE_OK) return 0;
    }
    return 1;
}

/* Reuse the cursor for descending ids; seek again when ids are out of order. */
static int asc_count_reverse(AscTree *tree,BtCursor *cursor,AscCountState *state,
                             int64_t id,int64_t *count) {
    int position=0,rc=SQLITE_OK,seek=1;
    int64_t found=0;
    *count=0;
    if (state->active && id<state->previous) {
        if (!sqlite3BtreeCursorIsValidNN(cursor)) {
            state->previous=id; state->key_valid=0; return 1;
        }
        if (!state->key_valid) {
            if (!asc_index_key(tree,cursor,&state->key)) return 0;
            state->key_valid=1;
        }
        found=state->key;
        if (found<=id) seek=0;
    }
    if (seek) {
        state->key_valid=0;
        Mem value={0}; value.flags=MEM_Int; value.u.i=id;
        UnpackedRecord search={0}; search.pKeyInfo=tree->likes_key; search.aMem=&value;
        search.nField=1; search.default_rc=-1; search.r1=-1; search.r2=1;
        rc=sqlite3BtreeIndexMoveto(cursor,&search,&position);
        if (rc!=SQLITE_OK) return 0;
        if (sqlite3BtreeCursorIsValidNN(cursor) && position>0) {
            rc=sqlite3BtreePrevious(cursor,0);
            if (rc!=SQLITE_OK && rc!=SQLITE_DONE) return 0;
        }
    }
    state->active=1; state->previous=id;
    while (sqlite3BtreeCursorIsValidNN(cursor)) {
        if (!state->key_valid) {
            if (!asc_index_key(tree,cursor,&state->key)) return 0;
            state->key_valid=1;
        }
        found=state->key;
        if (found>id) return 0;
        if (found!=id) return 1;
        /* Invalidate the cached key before moving the cursor. */
        state->key_valid=0;
        if (*count==INT64_MAX) return 0;
        ++*count;
        rc=sqlite3BtreePrevious(cursor,0);
        if (rc==SQLITE_DONE) return 1;
        if (rc!=SQLITE_OK) return 0;
    }
    return 1;
}

/* Consecutive table keys can step without another seek. SQLite owns the cursor. */
static int asc_table_move(BtCursor *cursor,int64_t id,int *position) {
    if (ASC_TREE_BACKSTEP && sqlite3BtreeCursorIsValidNN(cursor)) {
        int64_t previous=sqlite3BtreeIntegerKey(cursor);
        if (previous>INT64_MIN && id==previous-1) {
            int rc=sqlite3BtreePrevious(cursor,0);
            if (rc==SQLITE_OK && sqlite3BtreeIntegerKey(cursor)==id) {
                *position=0; return SQLITE_OK;
            }
            if (rc!=SQLITE_OK && rc!=SQLITE_DONE) return rc;
        }
    }
    return sqlite3BtreeTableMoveto(cursor,id,1,position);
}
static int asc_author(AscTree *tree,BtCursor *cursor,int64_t id,AscRow *row) {
    int position=0;
    if (sqlite3BtreeTableMoveto(cursor,id,0,&position)!=SQLITE_OK) return -1;
    if (position) return 0;
    AscField fields[4];
    const unsigned char *p=asc_payload(tree,cursor,3,fields);
    if (!p || !asc_text(tree,p,fields[1],&row->author,&row->author_n)) return -1;
    return 1;
}
static int asc_authors(AscTree *tree,BtCursor *cursor,AscRow rows[20],int first,int count) {
    unsigned char order[20];
    int n=count-first;
    for (int i=0;i<n;i++) {
        int at=i;
        unsigned char index=(unsigned char)(first+i);
        while (at && rows[order[at-1]].user>rows[index].user) {
            order[at]=order[at-1]; --at;
        }
        order[at]=index;
    }
    for (int i=0;i<n;) {
        AscRow *row=&rows[order[i]];
        int found=asc_author(tree,cursor,row->user,row);
        if (found<0) return -1;
        if (!found) { row->author=0; row->author_n=UINT32_MAX; }
        int next=i+1;
        while (next<n && rows[order[next]].user==row->user) {
            AscRow *same=&rows[order[next++]];
            same->author=row->author; same->author_n=row->author_n;
        }
        i=next;
    }
    int kept=first;
    for (int i=first;i<count;i++)
        if (rows[i].author_n!=UINT32_MAX) rows[kept++]=rows[i];
    return kept;
}
static int asc_row_likes(AscTree *tree,BtCursor cursors[4],AscCountState *counts,AscRow *row) {
    return (counts?asc_count_reverse(tree,&cursors[3],counts,row->id,&row->likes):
                   asc_count(tree,&cursors[3],row->id,&row->likes))?1:-1;
}
static int asc_row(AscTree *tree,BtCursor cursors[4],AscCountState *counts,int deferred,int64_t id,AscRow *row) {
    int position=0;
    if (asc_table_move(&cursors[0],id,&position)!=SQLITE_OK) return -1;
    if (position) return 0;
    AscField fields[4]; int64_t user;
    const unsigned char *p=asc_payload(tree,&cursors[0],4,fields);
    if (!p || !asc_record_integer(p,fields[1],&user)) return -1;
    row->id=id; row->likes=0; row->user=user;
    if (fields[2].type<13 || !(fields[2].type&1u) ||
        fields[3].type<13 || !(fields[3].type&1u)) return -1;
    /* Copy adjacent fields before another cursor operation invalidates the payload. */
    row->body=tree->scratch.n; row->body_n=fields[2].length;
    row->created=row->body+row->body_n; row->created_n=fields[3].length;
    if (!buffer_add(&tree->scratch,p+fields[2].offset,
                    (size_t)row->body_n+row->created_n)) return -1;
    if (deferred) return 1;
    int author=asc_author(tree,&cursors[1],user,row);
    return author<=0?author:asc_row_likes(tree,cursors,counts,row);
}
int asc_tree_read(AscTree *tree,int64_t id,int feed,PostVisitor emit,void *state) {
    if (!tree || !sqlite3_get_autocommit(tree->db)) return TREE_FALLBACK;
    int version=0, result=TREE_FALLBACK, count=0;
    BtCursor cursors[4]; AscRow rows[20]; AscCountState counts={0};
    for (unsigned i=0;i<4;i++) sqlite3BtreeCursorZero(&cursors[i]);
    buffer_clear(&tree->scratch);
    sqlite3BtreeEnter(tree->bt);
    if (sqlite3BtreeBeginTrans(tree->bt,0,&version)!=SQLITE_OK) goto unlock;
    /* Schema changes require SQL to reprepare. */
    if (version!=tree->cookie) goto done;
    for (unsigned i=0;i<4;i++) {
        if (i==2 && !feed) continue;
        KeyInfo *key=i==2?tree->feed_key:(i==3?tree->likes_key:NULL);
        if (sqlite3BtreeCursor(tree->bt,tree->roots[i],0,key,&cursors[i])!=SQLITE_OK) goto done;
    }
    if (feed) {
        int empty=0, rc=sqlite3BtreeFirst(&cursors[2],&empty);
        if (rc!=SQLITE_OK) goto done;
        while (!empty && count<20) {
            int first=count;
            do {
                AscField fields[4];
                const unsigned char *p=asc_payload(tree,&cursors[2],3,fields);
                if (!p || !asc_record_integer(p,fields[1],&id)) goto done;
                int found=asc_row(tree,cursors,NULL,1,id,&rows[count]);
                if (found<0) goto done;
                count+=found;
                if (count==20) break;
                rc=sqlite3BtreeNext(&cursors[2],0);
                if (rc==SQLITE_DONE) { empty=1; break; }
                if (rc!=SQLITE_OK) goto done;
            } while (!empty);
            count=asc_authors(tree,&cursors[1],rows,first,count);
            if (count<0) goto done;
            for (int i=first;i<count;i++)
                if (asc_row_likes(tree,cursors,ASC_TREE_REVERSE_COUNTS && (tree->flags&2u)?&counts:NULL,&rows[i])<0) goto done;
            if (count==20 || empty) break;
            rc=sqlite3BtreeNext(&cursors[2],0);
            if (rc==SQLITE_DONE) break;
            if (rc!=SQLITE_OK) goto done;
        }
    } else {
        count=asc_row(tree,cursors,NULL,0,id,&rows[0]);
        if (count<0) goto done;
    }
    result=count || feed?STORE_OK:STORE_MISSING;
    /* Emit only after the read succeeds; fallback must not duplicate rows. */
    for (int i=0;i<count;i++) {
        AscRow *r=&rows[i];
        Post post={.id=r->id,.likes=r->likes,
                   .body={tree->scratch.p+r->body,r->body_n},
                   .created={tree->scratch.p+r->created,r->created_n},
                   .author={tree->scratch.p+r->author,r->author_n}};
        if (!emit(state,&post)) { result=STORE_ERROR; break; }
    }
done:
    for (unsigned i=0;i<4;i++) sqlite3BtreeCloseCursor(&cursors[i]);
    if (sqlite3BtreeCommit(tree->bt)!=SQLITE_OK) {
        sqlite3BtreeRollback(tree->bt,SQLITE_ABORT_ROLLBACK,0); result=STORE_ERROR;
    }
unlock:
    sqlite3BtreeLeave(tree->bt);
    /* Retain capacity only; rebuild row contents on each read. */
    buffer_clear(&tree->scratch);
    return result;
}

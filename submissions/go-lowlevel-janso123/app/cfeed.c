#include "cfeed.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SQL_OK=0, SQL_INTEGER=1, SQL_NULL=5, SQL_ROW=100, SQL_DONE=101,
       SQL_OPEN_READWRITE=2, PINNED_SQLITE_VERSION=3053004 };

struct cf_reader { sqlite3 *db; sqlite3_stmt *feed; sqlite3_stmt *post; };
typedef struct { char *data; size_t cap; size_t len; int overflow; } sink;

static void bytes(sink *s, const char *p, size_t n) {
    if (n > SIZE_MAX - s->len) { s->overflow = 1; return; }
    if (n <= s->cap - (s->len < s->cap ? s->len : s->cap)) {
        if (!s->overflow && s->len <= s->cap) memcpy(s->data + s->len, p, n);
    } else s->overflow = 1;
    s->len += n;
}
static void lit(sink *s, const char *p) { bytes(s, p, strlen(p)); }

/* Match encoding/json's string encoder, including replacement of each invalid
 * UTF-8 byte and HTML/U+2028/U+2029 escaping. SQLite text is length-delimited. */
static void quote(sink *s, const unsigned char *p, size_t n) {
    static const char hex[] = "0123456789abcdef";
    bytes(s, "\"", 1);
    for (size_t i=0; i<n;) {
        unsigned char c=p[i];
        if (c<0x80) {
            const char *special=NULL;
            switch (c) {
            case '"': special="\\\""; break; case '\\': special="\\\\"; break;
            case '\b': special="\\b"; break; case '\f': special="\\f"; break;
            case '\n': special="\\n"; break; case '\r': special="\\r"; break;
            case '\t': special="\\t"; break;
            case '<': special="\\u003c"; break; case '>': special="\\u003e"; break;
            case '&': special="\\u0026"; break;
            }
            if (special) lit(s,special);
            else if (c<0x20) {
                char esc[6]={'\\','u','0','0',hex[c>>4],hex[c&15]};
                bytes(s,esc,6);
            } else bytes(s,(const char *)&p[i],1);
            i++;
            continue;
        }
        size_t count=0;
        if (c>=0xc2 && c<=0xdf) count=2;
        else if (c>=0xe0 && c<=0xef) count=3;
        else if (c>=0xf0 && c<=0xf4) count=4;
        int valid=count && count<=n-i;
        if (valid) {
            for (size_t k=1;k<count;k++) if (p[i+k]<0x80 || p[i+k]>0xbf) valid=0;
            if (valid && count==3 && ((c==0xe0 && p[i+1]<0xa0) || (c==0xed && p[i+1]>=0xa0))) valid=0;
            if (valid && count==4 && ((c==0xf0 && p[i+1]<0x90) || (c==0xf4 && p[i+1]>0x8f))) valid=0;
        }
        if (!valid) { lit(s,"\xef\xbf\xbd"); i++; continue; }
        if (count==3 && c==0xe2 && p[i+1]==0x80 && (p[i+2]==0xa8 || p[i+2]==0xa9)) {
            lit(s,p[i+2]==0xa8 ? "\\u2028" : "\\u2029");
        } else bytes(s,(const char *)&p[i],count);
        i+=count;
    }
    bytes(s,"\"",1);
}

size_t cf_quote(const char *input, size_t length, char *out, size_t capacity) {
    sink s={out,capacity,0,0}; quote(&s,(const unsigned char *)input,length); return s.len;
}

static int number(sink *s, int64_t value) {
    char buf[32]; int n=snprintf(buf,sizeof(buf),"%lld",(long long)value);
    if (n<0 || (size_t)n>=sizeof(buf)) return 0;
    bytes(s,buf,(size_t)n); return 1;
}
static int post(sink *s, sqlite3_stmt *stmt) {
    if (sqlite3_column_type(stmt,0)==SQL_NULL || sqlite3_column_type(stmt,4)==SQL_NULL) return 0;
    for (int i=1;i<=3;i++) if (sqlite3_column_type(stmt,i)==SQL_NULL) return 0;
    lit(s,"{\"id\":"); if (!number(s,sqlite3_column_int64(stmt,0))) return 0;
    for (int i=1;i<=3;i++) {
        lit(s,i==1 ? ",\"body\":" : i==2 ? ",\"created_at\":" : ",\"author\":");
        const unsigned char *p=sqlite3_column_text(stmt,i);
        int n=sqlite3_column_bytes(stmt,i);
        if (!p || n<0) return 0;
        quote(s,p,(size_t)n);
    }
    lit(s,",\"like_count\":"); if (!number(s,sqlite3_column_int64(stmt,4))) return 0;
    lit(s,"}"); return 1;
}

cf_reader *cf_open(const char *path, const char *feed_sql, const char *post_sql) {
    if (sqlite3_libversion_number()!=PINNED_SQLITE_VERSION) return NULL;
    cf_reader *r=calloc(1,sizeof(*r)); if (!r) return NULL;
    if (sqlite3_open_v2(path,&r->db,SQL_OPEN_READWRITE,NULL)!=SQL_OK) goto fail;
    const char *pragmas[]={"PRAGMA locking_mode=NORMAL", "PRAGMA journal_mode=WAL",
        "PRAGMA synchronous=NORMAL", "PRAGMA foreign_keys=ON", "PRAGMA busy_timeout=0",
        "PRAGMA cache_size=-8192", "PRAGMA mmap_size=268435456", "PRAGMA query_only=ON"};
    for (size_t i=0;i<sizeof(pragmas)/sizeof(pragmas[0]);i++)
        if (sqlite3_exec(r->db,pragmas[i],NULL,NULL,NULL)!=SQL_OK) goto fail;
    if (sqlite3_prepare_v2(r->db,feed_sql,-1,&r->feed,NULL)!=SQL_OK) goto fail;
    if (sqlite3_prepare_v2(r->db,post_sql,-1,&r->post,NULL)!=SQL_OK) goto fail;
    return r;
fail: cf_close(r); return NULL;
}

void cf_close(cf_reader *r) {
    if (!r) return;
    if (r->feed) sqlite3_finalize(r->feed);
    if (r->post) sqlite3_finalize(r->post);
    if (r->db) sqlite3_close(r->db);
    free(r);
}

int cf_read(cf_reader *r, int feed, int64_t id, char *out, size_t cap, size_t *used) {
    if (!r || !out || !used) return CF_ERROR;
    sqlite3_stmt *st=feed ? r->feed : r->post;
    sink s={out,cap,0,0};
    int status=CF_OK, rows=0, rc;
    if (sqlite3_reset(st)!=SQL_OK || sqlite3_clear_bindings(st)!=SQL_OK) {
        fprintf(stderr,"cfeed_error=prepare sqlite_code=%d message=%s\n",sqlite3_errcode(r->db),sqlite3_errmsg(r->db));
        return CF_ERROR;
    }
    if (!feed && sqlite3_bind_int64(st,1,id)!=SQL_OK) {
        fprintf(stderr,"cfeed_error=bind sqlite_code=%d message=%s\n",sqlite3_errcode(r->db),sqlite3_errmsg(r->db));
        return CF_ERROR;
    }
    lit(&s,feed ? "{\"posts\":[" : "{\"post\":");
    while ((rc=sqlite3_step(st))==SQL_ROW) {
        if (++rows>20 || (!feed && rows>1)) { status=CF_ERROR; break; }
        if (feed && rows>1) lit(&s,",");
        if (!post(&s,st)) { status=CF_ERROR; break; }
    }
    if (rc!=SQL_DONE) status=CF_ERROR;
    if (status==CF_OK && !feed && rows==0) status=CF_NOT_FOUND;
    if (status==CF_OK) lit(&s,feed ? "]}" : "}");
    int reset_rc=sqlite3_reset(st);
    if (reset_rc!=SQL_OK) status=CF_ERROR;
    if (status==CF_ERROR)
        fprintf(stderr,"cfeed_error=read step_rc=%d reset_rc=%d sqlite_code=%d message=%s\n",
                rc,reset_rc,sqlite3_errcode(r->db),sqlite3_errmsg(r->db));
    *used=s.len;
    if (status==CF_OK && s.overflow) status=CF_TOO_SMALL;
    return status;
}

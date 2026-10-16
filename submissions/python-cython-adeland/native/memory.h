#ifndef TWELVE_MEMORY_H
#define TWELVE_MEMORY_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/uio.h>

#define POOL_CAP (128ULL * 1024 * 1024)
#define OUTPUT_CAP 1048576
#define AUTH_CAP 8192
#define BODY_CAP 16384
#define RESPONSE_HEADROOM 256
#define POOL_PAGES (POOL_CAP / 4096)

/* Sanitizers also see logical frees inside the arena, not just munmap(). */
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define POOL_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define POOL_ASAN 1
#endif
#ifdef POOL_ASAN
#include <sanitizer/asan_interface.h>
#define POISON(p,n) __asan_poison_memory_region((p),(n))
#define UNPOISON(p,n) __asan_unpoison_memory_region((p),(n))
#else
#define POISON(p,n) ((void)(p), (void)(n))
#define UNPOISON(p,n) ((void)(p), (void)(n))
#endif
#define REQUIRE(condition) do { if (!(condition)) abort(); } while (0)
typedef struct PoolPage { int32_t next, prev; uint8_t state; } PoolPage;
typedef struct Pool {
    int32_t free[9];
    size_t inuse;
    char *arena;
    PoolPage *pages;
} Pool;
_Static_assert(sizeof(Pool) == 64, "arena metadata must fit in one cache line");
typedef struct Buffer { char *data; uint32_t len, cap; } Buffer;
typedef struct __attribute__((aligned(64))) Chunk {
    struct Chunk *next;
    uint32_t len;
    char data[4032] __attribute__((aligned(64)));
} Chunk;
_Static_assert(sizeof(Chunk)==4096, "response chunks must use the minimum pool class");
_Static_assert(offsetof(Chunk,data)==64, "chunk payload must start on a cache line");
typedef struct Writer { Chunk *head, *tail; uint32_t len; } Writer;
typedef struct Output { Chunk *head; uint32_t len, sent; } Output;
typedef struct __attribute__((aligned(64))) Request {
    uint32_t url_len, header_len;
    uint8_t auth_seen, header_auth, header_value, method;
    Buffer auth, body;
    char header[64], url[1024];
} Request;
static void request_reset(Request*r){memset(r,0,offsetof(Request,header));}
static int pool_index(size_t n) { int i=0; size_t cap=4096; while(cap<n && i<8) {cap*=2;i++;} return cap<n?-1:i; }
/* Page state: 0=interior, 1..9=free head, 0x81..0x89=allocated head. */
static void pool_link(Pool *p, int32_t page, int order) {
    POISON(p->arena+(size_t)page*4096, (size_t)4096<<order);
    PoolPage *node=&p->pages[page];
    node->prev=-1;
    node->next=p->free[order];
    if (node->next>=0) p->pages[node->next].prev=page;
    p->free[order]=page;
    node->state=order+1;
}
static void pool_unlink(Pool *p, int32_t page, int order) {
    PoolPage *node=&p->pages[page];
    if (node->prev>=0) p->pages[node->prev].next=node->next;
    else p->free[order]=node->next;
    if (node->next>=0) p->pages[node->next].prev=node->prev;
    node->state=0;
}
static int pool_init(Pool *p) {
    p->arena=mmap(NULL,POOL_CAP,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if (p->arena==MAP_FAILED) { p->arena=NULL; return -1; }
    p->pages=calloc(POOL_PAGES,sizeof(*p->pages));
    if (!p->pages) { munmap(p->arena,POOL_CAP); p->arena=NULL; return -1; }
    for (int i=0; i<9; i++) p->free[i]=-1;
    for (int32_t page=0; page<(int32_t)POOL_PAGES; page+=256) pool_link(p,page,8);
    return 0;
}
static void *pool_get(Pool *p, size_t n) {
    int order=pool_index(n), found=order;
    if (!n || order<0 || !p->arena) return NULL;
    while (found<9 && p->free[found]<0) found++;
    if (found==9) return NULL;
    int32_t page=p->free[found];
    pool_unlink(p,page,found);
    while (found>order) { found--; pool_link(p,page+(1<<found),found); }
    size_t size=(size_t)4096<<order;
    char *data=p->arena+(size_t)page*4096;
    UNPOISON(data,size);
    p->pages[page].state=0x80 | (order+1);
    p->inuse+=size;
    return data;
}
static void pool_put(Pool *p, void *data, size_t n) {
    if (!data) return;
    int order=pool_index(n);
    uintptr_t address=(uintptr_t)data, base=(uintptr_t)p->arena;
    REQUIRE(n && order>=0 && address>=base && address-base<POOL_CAP);
    size_t off=address-base;
    REQUIRE(off%((size_t)4096<<order)==0);
    int32_t page=off/4096;
    REQUIRE(p->pages[page].state==(0x80 | (order+1)));
    p->inuse-=(size_t)4096<<order;
    p->pages[page].state=0;
    while (order<8) {
        int32_t buddy=page^(1<<order);
        if (p->pages[buddy].state!=order+1) break;
        pool_unlink(p,buddy,order);
        if (buddy<page) page=buddy;
        order++;
    }
    pool_link(p,page,order);
}
static void pool_destroy(Pool *p) {
    REQUIRE(p->inuse==0);
    if (p->arena) { UNPOISON(p->arena,POOL_CAP); munmap(p->arena,POOL_CAP); }
    free(p->pages);
    memset(p,0,sizeof(*p));
}
static int buf_reserve(Pool *p, Buffer *b, size_t need) {
    if(need<=b->cap)return 0;
    int i=pool_index(need);if(i<0)return -1;
    uint32_t cap=4096U<<i;char*data=pool_get(p,cap);if(!data)return -1;
    if(b->len)memcpy(data,b->data,b->len);
    pool_put(p,b->data,b->cap);b->data=data;b->cap=cap;return 0;
}
static int buf_append(Pool *p,Buffer*b,const char*data,size_t n) {
    if (b->len>OUTPUT_CAP || n>OUTPUT_CAP-b->len) return -1;
    if (!n) return 0;
    if (buf_reserve(p,b,b->len+n)) return -1;
    memcpy(b->data+b->len,data,n);
    b->len+=n;
    return 0;
}
/* Output grows by linking chunks. Encoded bytes never move or get repacked. */
static int writer_append(Pool *p, Writer *w, const char *data, size_t n) {
    if (w->len>OUTPUT_CAP || n>OUTPUT_CAP-w->len) return -1;
    while (n) {
        if (!w->tail || w->tail->len==sizeof(w->tail->data)) {
            Chunk *chunk=pool_get(p,sizeof(*chunk));
            if (!chunk) return -1;
            chunk->next=NULL;
            chunk->len=0;
            if (w->tail) w->tail->next=chunk;
            else w->head=chunk;
            w->tail=chunk;
        }
        size_t take=sizeof(w->tail->data)-w->tail->len;
        if (take>n) take=n;
        memcpy(w->tail->data+w->tail->len,data,take);
        w->tail->len+=take;
        w->len+=take;
        data+=take;
        n-=take;
    }
    return 0;
}
static void writer_release(Pool *p, Writer *w) {
    Chunk *chunk=w->head;
    while (chunk) { Chunk *next=chunk->next; pool_put(p,chunk,sizeof(*chunk)); chunk=next; }
    memset(w,0,sizeof(*w));
}
static int writer_reset(Pool *p, Writer *w, uint32_t prefix) {
    REQUIRE(prefix<=sizeof(((Chunk*)0)->data));
    if (!w->head) {
        w->head=pool_get(p,sizeof(Chunk));
        if (!w->head) return -1;
        w->head->next=NULL;
    }
    Chunk *chunk=w->head->next;
    while (chunk) { Chunk *next=chunk->next; pool_put(p,chunk,sizeof(*chunk)); chunk=next; }
    w->head->next=NULL;
    w->head->len=prefix;
    w->tail=w->head;
    w->len=prefix;
    return 0;
}
static int writer_literal(Pool *p, Writer *w, const char *s) { return writer_append(p,w,s,strlen(s)); }
static int writer_int64(Pool *p, Writer *w, int64_t n) {
    char text[21], *end=text+sizeof(text), *out=end;
    uint64_t magnitude=n<0 ? (uint64_t)0-(uint64_t)n : (uint64_t)n;
    do { *--out='0'+magnitude%10; magnitude/=10; } while (magnitude);
    if (n<0) *--out='-';
    return writer_append(p,w,out,end-out);
}
static int writer_string(Pool *p, Writer *w, const char *s, size_t n) {
    static const char hex[]="0123456789abcdef";
    if (writer_literal(p,w,"\"")) return -1;
    size_t start=0;
    for (size_t i=0; i<n; i++) {
        unsigned char c=s[i];
        if (c>=32 && c!='"' && c!='\\') continue;
        if (writer_append(p,w,s+start,i-start)) return -1;
        char escape[6]={'\\',c,0,0,0,0};
        size_t len=2;
        if (c<32) { escape[1]='u'; escape[2]='0'; escape[3]='0'; escape[4]=hex[c>>4]; escape[5]=hex[c&15]; len=6; }
        if (writer_append(p,w,escape,len)) return -1;
        start=i+1;
    }
    if (writer_append(p,w,s+start,n-start)) return -1;
    return writer_literal(p,w,"\"");
}
static void output_release(Pool *p, Output *out) {
    Chunk *chunk=out->head;
    while (chunk) { Chunk *next=chunk->next; pool_put(p,chunk,sizeof(*chunk)); chunk=next; }
    memset(out,0,sizeof(*out));
}
static void output_take(Output *out, Writer *w, uint32_t skip) {
    REQUIRE(!out->head && w->head && skip<=w->head->len);
    out->head=w->head;
    out->len=w->len-skip;
    out->sent=skip;
    memset(w,0,sizeof(*w));
}
static ssize_t output_send(Pool *p, int fd, Output *out) {
    struct iovec vectors[32];
    struct msghdr message={0};
    Chunk *chunk=out->head;
    size_t count=0, offset=out->sent;
    while (chunk && count<32) {
        vectors[count].iov_base=chunk->data+offset;
        vectors[count].iov_len=chunk->len-offset;
        count++;
        chunk=chunk->next;
        offset=0;
    }
    if (!count) return 0;
    message.msg_iov=vectors;
    message.msg_iovlen=count;
    ssize_t sent=sendmsg(fd,&message,MSG_NOSIGNAL);
    if (sent<=0) return sent;
    REQUIRE((size_t)sent<=out->len);
    size_t remaining=sent;
    out->len-=sent;
    while (remaining) {
        chunk=out->head;
        size_t available=chunk->len-out->sent;
        if (remaining<available) { out->sent+=remaining; break; }
        remaining-=available;
        out->head=chunk->next;
        out->sent=0;
        pool_put(p,chunk,sizeof(*chunk));
    }
    return sent;
}
static void buf_release(Pool*p,Buffer*b){pool_put(p,b->data,b->cap);memset(b,0,sizeof(*b));}
/* Rotate request records across 32 cache-line offsets to vary L1 set placement. */
static Request *request_acquire(Pool *p) {
    char *block=pool_get(p,4096);
    if (!block) return NULL;
    size_t page=(block-p->arena)/4096;
    return (Request*)(block+(page%32)*64);
}
_Static_assert(sizeof(Request)+31*64<=4096, "colored requests must fit their block");
static void request_release(Pool *p, Request *r) {
    if (!r) return;
    buf_release(p,&r->auth);
    buf_release(p,&r->body);
    size_t page=((char*)r-p->arena)/4096;
    pool_put(p,p->arena+page*4096,4096);
}
static size_t request_prefix_bytes(void) { return offsetof(Request, header); }

#endif

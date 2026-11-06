#define _GNU_SOURCE
#include "abi.h"
#include <errno.h>
#include <liburing.h>
#if ASC_X86 && defined(__SSE2__)
#include <emmintrin.h>
#endif
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

/* Keep offsets across input reallocations. */
enum { HTTP_HEADER,HTTP_FIXED,HTTP_SIZE,HTTP_DATA,HTTP_TRAILER };
typedef struct {
    uint32_t scanned,header_end,cursor,line_seen,chunk_length;
    uint16_t method_n,path_start,path_n,auth_start,auth_n,body_length;
    uint8_t phase,close,expect;
    Buffer decoded;
} HttpMachine;
_Static_assert(sizeof(HttpMachine)==64,"private HTTP state has a bounded 64-byte stride");
static void http_reset(HttpMachine *s) {
    /* Only scanned changes while a header is incomplete. */
    if(s->phase==HTTP_HEADER && !s->decoded.p) {
        if(s->scanned)s->scanned=0;
        return;
    }
    if(s->decoded.p)free(s->decoded.p);
    memset(s,0,sizeof(*s));
}
static int http_fail(HttpMachine *s,int result) {
    http_reset(s);return result;
}
static void http_remember(HttpMachine *s,const Buffer *input,const Request *q,
                          size_t end,size_t length,unsigned chunks) {
    s->header_end=(uint32_t)end;
    s->method_n=(uint16_t)q->method.n;
    s->path_start=(uint16_t)(q->path.p-input->p);s->path_n=(uint16_t)q->path.n;
    s->auth_start=q->authorization.p?(uint16_t)(q->authorization.p-input->p):0;
    s->auth_n=(uint16_t)q->authorization.n;
    s->body_length=(uint16_t)length;
    s->close=(uint8_t)q->close;s->expect=(uint8_t)q->expect_continue;
    s->cursor=(uint32_t)end;s->line_seen=0;
    s->phase=(uint8_t)(chunks?HTTP_SIZE:HTTP_FIXED);
}
static void http_restore(const HttpMachine *s,const Buffer *input,Request *q) {
    q->method=(Slice){input->p,s->method_n};
    q->path=(Slice){input->p+s->path_start,s->path_n};
    q->authorization=(Slice){s->auth_n?input->p+s->auth_start:NULL,s->auth_n};
    q->close=s->close;q->expect_continue=s->expect;
}

static Buffer response_workspace, request_workspace;
#ifndef ASC_REQUEST_SLICE
#define ASC_REQUEST_SLICE 16u
#endif
#ifndef ASC_ACCEPT_SLICE
#define ASC_ACCEPT_SLICE 64u
#endif
_Static_assert(ASC_REQUEST_SLICE>0 && ASC_ACCEPT_SLICE>0,"work slices must be positive");
__attribute__((destructor)) static void release_workspace(void) {
    buffer_free(&response_workspace); buffer_free(&request_workspace);
}
static void retain_input(Connection *c) {
    /* Pool only empty input buffers; partial requests stay with their connection. */
    if (c->input.cap<=65536 && c->input.cap>=request_workspace.cap) {
        if(request_workspace.p)free(request_workspace.p);
        request_workspace=c->input;
        buffer_clear(&request_workspace);
        memset(&c->input,0,sizeof(c->input));
    } else buffer_free(&c->input);
}
static void borrow_input(Connection *c) {
    if (!c->input.p && request_workspace.p) {
        c->input=request_workspace;
        memset(&request_workspace,0,sizeof(request_workspace));
    }
}
static void retain_response(Response *response) {
    if(response->body.cap<=65536 && response->body.cap>=response_workspace.cap) {
        if(response_workspace.p)free(response_workspace.p);
        response_workspace=response->body;buffer_clear(&response_workspace);
        memset(&response->body,0,sizeof(response->body));
    } else buffer_free(&response->body);
}

static int equal_ci(Slice s, const char *literal) {
    size_t n=strlen(literal); unsigned different=0;
    if (s.n!=n) return 0;
    for (size_t i=0;i<n;++i) different|=((unsigned char)s.p[i]|32u) ^ ((unsigned char)literal[i]|32u);
    return different==0;
}
static size_t crlf(const char *p, size_t n) {
    size_t i=0;
#if ASC_X86 && defined(__SSE2__)
    const __m128i cr=_mm_set1_epi8('\r'),lf=_mm_set1_epi8('\n');
    for(;n-i>=16;i+=15) {
        __m128i v=_mm_loadu_si128((const __m128i *)(p+i));
        unsigned c=(unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(v,cr));
        unsigned l=(unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(v,lf));
        unsigned matches=c&(l>>1);
        if(matches)return i+(unsigned)__builtin_ctz(matches);
    }
#endif
    for(;i+1<n;++i)if(p[i]=='\r' && p[i+1]=='\n')return i;
    return SIZE_MAX;
}

static unsigned token_character(unsigned c) {

    static const uint64_t alphabet[4]={
        UINT64_C(0x03ff6cfa00000000), UINT64_C(0x57ffffffc7fffffe), 0, 0
    };
    return (unsigned)((alphabet[c>>6]>>(c&63u))&1u);
}
/* Reject control bytes except tab, plus DEL. */
static int valid_header_value(Slice value) {
    const unsigned char *p=(const unsigned char *)value.p;
    size_t i=0;
#if ASC_X86 && defined(__SSE2__)
    const __m128i low=_mm_set1_epi8(31), zero=_mm_setzero_si128();
    const __m128i tab=_mm_set1_epi8(9), del=_mm_set1_epi8(127);
    for (;value.n-i>=16;i+=16) {
        __m128i v=_mm_loadu_si128((const __m128i *)(p+i));
        __m128i control=_mm_cmpeq_epi8(_mm_subs_epu8(v,low),zero);
        __m128i bad=_mm_or_si128(_mm_andnot_si128(_mm_cmpeq_epi8(v,tab),control),
                                _mm_cmpeq_epi8(v,del));
        if (_mm_movemask_epi8(bad)) return 0;
    }
#endif
    for (;i<value.n;++i) {
        unsigned c=p[i];
        if ((c<32 && c!=9) || c==127) return 0;
    }
    return 1;
}
static Slice trim(Slice s) {
    while (s.n && (s.p[0]==' ' || s.p[0]=='\t')) { ++s.p; --s.n; }
    while (s.n && (s.p[s.n-1]==' ' || s.p[s.n-1]=='\t')) --s.n;
    return s;
}
static unsigned has_token(Slice s, const char *word) {
    unsigned found=0;
    while (s.n) {
        const char *comma=memchr(s.p,',',s.n);
        size_t n=comma?(size_t)(comma-s.p):s.n;
        found|=(unsigned)equal_ci(trim((Slice){s.p,n}),word);
        if (!comma) break;
        s.n-=n+1; s.p=comma+1;
    }
    return found;
}

static int request_headers(Context *ctx,Buffer *input,size_t end,Request *q,size_t *length_out,unsigned *chunks_out) {
    size_t line=crlf(input->p,end);
    if (line==SIZE_MAX) return -400;
    const char *first=memchr(input->p,' ',line);
    if (!first || first==input->p) return -400;
    const char *second=memchr(first+1,' ',line-(size_t)(first+1-input->p));
    if (!second || second==first+1) return -400;
    q->method=(Slice){input->p,(size_t)(first-input->p)};
    q->path=(Slice){first+1,(size_t)(second-first-1)};
    for (size_t i=0;i<q->method.n;++i) if (!token_character((unsigned char)q->method.p[i])) return -400;
    if (q->path.p[0]!='/') return -400;
    for (size_t i=0;i<q->path.n;++i) if ((unsigned char)q->path.p[i]<=32 || q->path.p[i]==127) return -400;
    Slice version={second+1,line-(size_t)(second+1-input->p)};
    int http11=version.n==8 && !memcmp(version.p,"HTTP/1.1",8);
    if (!http11 && !(version.n==8 && !memcmp(version.p,"HTTP/1.0",8))) return -400;
    const char *query=memchr(q->path.p,'?',q->path.n);
    if (query) q->path.n=(size_t)(query-q->path.p);
    int64_t length=0; unsigned have_length=0, chunks=0, have_auth=0, close=0, keep=0;
    size_t position=line+2;
    while (position<end-2) {
        line=crlf(input->p+position,end-position);
        if (line==SIZE_MAX || !line) return -400;
        const char *colon=memchr(input->p+position,':',line);
        if (!colon || colon==input->p+position) return -400;
        Slice key={input->p+position,(size_t)(colon-(input->p+position))};
        for (size_t i=0;i<key.n;++i) if (!token_character((unsigned char)key.p[i])) return -400;
        Slice value={colon+1,line-key.n-1};
        if (!valid_header_value(value)) return -400;
        value=trim(value);
        switch((unsigned char)key.p[0]|32u) {
        case 'c':
            if(equal_ci(key,"content-length")) {
                if(have_length++||!ctx->math->decimal(value,&length))return -400;
                if(length>ASC_BODY_MAX)return -413;
            } else if(equal_ci(key,"connection")) {
                close|=has_token(value,"close");keep|=has_token(value,"keep-alive");
            }
            break;
        case 't':
            if(equal_ci(key,"transfer-encoding")) {
                if(chunks||!equal_ci(value,"chunked"))return -400;
                chunks=1;
            }
            break;
        case 'a':
            if(equal_ci(key,"authorization")) {
                if(have_auth++)return -400;
                q->authorization=value;
            }
            break;
        case 'e':
            if(equal_ci(key,"expect")) {
                if(!equal_ci(value,"100-continue"))return -417;
                q->expect_continue=1;
            }
            break;
        }
        position+=line+2;
    }
    q->close=(int)(close|((!http11)&(!keep)));
    if (chunks && have_length) return -400;
    *length_out=(size_t)length;*chunks_out=chunks;return 1;
}

static int parse(Context *ctx,Buffer *input,Request *q,Buffer *decoded,size_t *used,HttpMachine *s) {
    memset(q,0,sizeof(*q));
    if(s->phase==HTTP_HEADER) {
        size_t start=s->scanned>3?s->scanned-3:0;
        size_t end=ctx->math->header_end((const unsigned char *)input->p+start,input->n-start);
        if(!end) {
            s->scanned=input->n;
            return input->n>ASC_HEADER_MAX?http_fail(s,-431):0;
        }
        end+=start;
        if(end>ASC_HEADER_MAX)return http_fail(s,-431);
        size_t length=0;unsigned chunks=0;
        int result=request_headers(ctx,input,end,q,&length,&chunks);
        if(result<0)return http_fail(s,result);
        if(!chunks && input->n-end>=length) {
            q->body=(Slice){input->p+end,length};*used=end+length;
            if(s->scanned)s->scanned=0;
            return 1;
        }
        http_remember(s,input,q,end,length,chunks);
    } else http_restore(s,input,q);
    if(s->phase==HTTP_FIXED) {
        if(input->n-s->header_end<s->body_length)return 0;
        q->body=(Slice){input->p+s->header_end,s->body_length};
        *used=s->header_end+s->body_length;http_reset(s);return 1;
    }
    for(;;) {
        if(s->phase==HTTP_DATA) {
            if(input->n-s->cursor<s->chunk_length+2)return 0;
            if(memcmp(input->p+s->cursor+s->chunk_length,"\r\n",2))return http_fail(s,-400);
            if(!buffer_add(&s->decoded,input->p+s->cursor,s->chunk_length))return http_fail(s,-500);
            s->cursor+=s->chunk_length+2;s->phase=HTTP_SIZE;s->line_seen=0;
            continue;
        }
        size_t start=s->cursor+(s->line_seen?s->line_seen-1:0);
        size_t line=crlf(input->p+start,input->n-start);
        if(line==SIZE_MAX) {s->line_seen=input->n-s->cursor;return 0;}
        line+=start-s->cursor;
        if(s->phase==HTTP_TRAILER) {
            if(!line) {
                *used=s->cursor+2;*decoded=s->decoded;
                memset(&s->decoded,0,sizeof(s->decoded));
                q->body=(Slice){decoded->p,decoded->n};http_reset(s);return 1;
            }
            const char *colon=memchr(input->p+s->cursor,':',line);
            if(!colon || colon==input->p+s->cursor)return http_fail(s,-400);
            s->cursor+=(uint32_t)(line+2);s->line_seen=0;
            continue;
        }
        size_t digits=0,size=0;
        while(digits<line && input->p[s->cursor+digits]!=';') {
            unsigned c=(unsigned char)input->p[s->cursor+digits],lower=c|32u;
            unsigned decimal=(c-'0')<10,alpha=(lower-'a')<6;
            if(!(decimal|alpha))return http_fail(s,-400);
            unsigned value=(c-'0')*decimal+(lower-'a'+10)*alpha;
            if(size>((ASC_BODY_MAX-value)>>4))return http_fail(s,-413);
            size=(size<<4)+value;++digits;
        }
        if(!digits)return http_fail(s,-400);
        for(size_t i=digits;i<line;++i)if((unsigned char)input->p[s->cursor+i]<32)return http_fail(s,-400);
        if(size>ASC_BODY_MAX-s->decoded.n)return http_fail(s,-413);
        s->cursor+=(uint32_t)(line+2);s->line_seen=0;s->chunk_length=(uint32_t)size;
        s->phase=(uint8_t)(size?HTTP_DATA:HTTP_TRAILER);
    }
}

static size_t decimal_u32(char *out,uint32_t value) {
    static const char pairs[]=
        "0001020304050607080910111213141516171819"
        "2021222324252627282930313233343536373839"
        "4041424344454647484950515253545556575859"
        "6061626364656667686970717273747576777879"
        "8081828384858687888990919293949596979899";
    /* Estimate decimal digits from bit width, then correct against powers of ten. */
    static const uint32_t powers[10]={0,10,100,1000,10000,100000,1000000,10000000,100000000,1000000000};
    unsigned estimate=((32u-(unsigned)__builtin_clz(value|1u))*1233u)>>12;
    unsigned count=estimate+(value>=powers[estimate]);
    char *p=out+count;
    while(value>=100u) {
        uint32_t quotient=value/100u;
        p-=2;memcpy(p,pairs+2u*(value-100u*quotient),2);value=quotient;
    }
    if(value<10u)*--p=(char)('0'+value);
    else {p-=2;memcpy(p,pairs+2u*value,2);}
    return count;
}

static size_t response_header(char out[256],int status,uint32_t length,int close) {
    Slice prefix={0};
#define ASC_PREFIX(code,text) case code: { \
    static const char bytes[]="HTTP/1.1 " #code " " text "\r\nContent-Type: application/json\r\nContent-Length: "; \
    prefix=(Slice){bytes,sizeof(bytes)-1}; break; }
    switch (status) {
        ASC_PREFIX(200,"OK"); ASC_PREFIX(201,"Created");
        ASC_PREFIX(400,"Bad Request"); ASC_PREFIX(401,"Unauthorized");
        ASC_PREFIX(404,"Not Found"); ASC_PREFIX(413,"Content Too Large");
        ASC_PREFIX(417,"Expectation Failed");
        ASC_PREFIX(431,"Request Header Fields Too Large");
        ASC_PREFIX(500,"Internal Server Error"); ASC_PREFIX(503,"Service Unavailable");
    }
#undef ASC_PREFIX
    size_t used=0;
    if (prefix.p) { memcpy(out,prefix.p,prefix.n); used=prefix.n; }
    else {
        static const char initial[]="HTTP/1.1 ";
        static const char suffix[]=" Internal Server Error\r\nContent-Type: application/json\r\nContent-Length: ";
        memcpy(out,initial,sizeof(initial)-1); used=sizeof(initial)-1;
        uint32_t magnitude=(uint32_t)status;
        if (status<0) { out[used++]='-'; magnitude=0u-magnitude; }
        used+=decimal_u32(out+used,magnitude);
        memcpy(out+used,suffix,sizeof(suffix)-1); used+=sizeof(suffix)-1;
    }
    used+=decimal_u32(out+used,length);
    static const char middle[]="\r\nConnection: ";
    memcpy(out+used,middle,sizeof(middle)-1); used+=sizeof(middle)-1;
    const Slice ending=close?(Slice){"close\r\n\r\n",9}:(Slice){"keep-alive\r\n\r\n",14};
    memcpy(out+used,ending.p,ending.n); used+=ending.n;
    /* Maximum response header length is below 256 bytes. */
    return used;
}
enum { CONNECTION_LIVE=1u,RECEIVING=2u,SENDING=4u,POLLING=8u,
       CLOSING=16u,CANCELING=32u,WAITING_BUFFER=64u };
enum { OP_RECEIVE=1,OP_SEND,OP_POLL,OP_CANCEL };
enum { RING_ENTRIES=512,RECEIVE_BUFFERS=256,RECEIVE_BYTES=1024,SEND_BLOCK=64 };
#define NO_SLOT UINT32_MAX

typedef struct {
    struct msghdr message;
    struct iovec vectors[2];
    Buffer body;
    uint32_t next,offset,header_n;
    char header[256];
} SendOp;
typedef struct Ring Ring;
typedef struct {
    Ring *rings,*available;
    uint32_t slots;
    uint64_t ring_count,active,peak;
    int epoll,stopping;
} Fleet;
struct Ring {
    struct io_uring uring;
    Fleet *fleet;
    Ring *next;
    Connection *connections;
    HttpMachine *machines;
    uint32_t *ready,*waiting;
    struct io_uring_buf_ring *buffers;
    unsigned char *receive;
    SendOp **send_blocks;
    uint32_t send_block_count,send_free;
    uint32_t slots,allocated,free_slot,active;
    uint32_t ready_head,ready_n,waiting_head,waiting_n,returned;
    unsigned inflight;
    int initialized,watched,error;
};
static uint64_t operation(unsigned slot,unsigned type) {
    return ((uint64_t)slot<<3)|type;
}
static void *zero_mapping(size_t bytes) {
    void *p=mmap(NULL,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    return p==MAP_FAILED?NULL:p;
}
static SendOp *send_op(Ring *r,uint32_t id) {
    --id;return &r->send_blocks[id/SEND_BLOCK][id%SEND_BLOCK];
}
static uint32_t allocate_send(Ring *r) {
    if(!r->send_free) {
        if(r->send_block_count>=UINT32_MAX/SEND_BLOCK)return 0;
        SendOp *block=calloc(SEND_BLOCK,sizeof(*block));
        if(!block)return 0;
        SendOp **blocks=realloc(r->send_blocks,((size_t)r->send_block_count+1)*sizeof(*blocks));
        if(!blocks){free(block);return 0;}
        r->send_blocks=blocks;blocks[r->send_block_count]=block;
        uint32_t base=r->send_block_count++*SEND_BLOCK;
        for(unsigned i=SEND_BLOCK;i;--i) {
            block[i-1].next=r->send_free;r->send_free=base+i;
        }
    }
    uint32_t id=r->send_free;SendOp *op=send_op(r,id);
    r->send_free=op->next;op->next=0;op->offset=0;
    return id;
}
static void release_send(Ring *r,uint32_t id) {
    SendOp *op=send_op(r,id);
    Response response={.body=op->body};retain_response(&response);
    memset(&op->body,0,sizeof(op->body));
    op->next=r->send_free;r->send_free=id;
}
static int flush_ring(Ring *r) {
    while(io_uring_sq_ready(&r->uring)) {
        int result;
        do{result=io_uring_submit(&r->uring);}while(result==-EINTR);
        if(result==-EBUSY)return 1;
        if(result<0){r->error=-result;return 0;}
        if(!result)break;
    }
    return 1;
}
static struct io_uring_sqe *get_sqe(Ring *r) {
    struct io_uring_sqe *sqe=io_uring_get_sqe(&r->uring);
    if(!sqe && flush_ring(r))sqe=io_uring_get_sqe(&r->uring);
    return sqe;
}
static void schedule(Ring *r,Connection *c) {
    if(c->scheduled)return;
    if(r->ready_n==r->slots){r->error=EPROTO;return;}
    uint32_t slot=(uint32_t)(c-r->connections);
    r->ready[(r->ready_head+r->ready_n)%r->slots]=slot;
    ++r->ready_n;c->scheduled=1;
}
static void wait_for_buffer(Ring *r,Connection *c) {
    if(c->scheduled || r->waiting_n==r->slots){r->error=EPROTO;return;}
    uint32_t slot=(uint32_t)(c-r->connections);
    r->waiting[(r->waiting_head+r->waiting_n)%r->slots]=slot;
    ++r->waiting_n;c->scheduled=2;c->events|=WAITING_BUFFER;
}
static void retry_waiters(Ring *r) {
    while(r->waiting_n && (r->returned || r->fleet->stopping)) {
        uint32_t slot=r->waiting[r->waiting_head];
        r->waiting_head=(r->waiting_head+1)%r->slots;--r->waiting_n;
        Connection *c=&r->connections[slot];
        c->scheduled=0;c->events&=~(uint32_t)WAITING_BUFFER;
        if(r->returned)--r->returned;
        schedule(r,c);
    }
}
static void finish_close(Ring *r,Connection *c) {
    if((c->events&(RECEIVING|SENDING|POLLING|CANCELING)) || c->scheduled)return;
    uint32_t slot=(uint32_t)(c-r->connections);
    if(!r->fleet->stopping) {
        int absent=-1,result;
        do{result=io_uring_register_files_update(&r->uring,slot,&absent,1);}while(result==-EINTR);
        if(result!=1){r->error=result<0?-result:EIO;return;}
    }
    http_reset(&r->machines[slot]);
    buffer_free(&c->input);buffer_free(&c->output);
    while(c->sent) {
        uint32_t id=c->sent;c->sent=send_op(r,id)->next;release_send(r,id);
    }
    memset(c,0,sizeof(*c));c->fd=(int)r->free_slot;r->free_slot=slot;
    --r->active;--r->fleet->active;r->fleet->available=r;
}
static void close_connection(Ring *r,Connection *c) {
    c->events|=CLOSING;
    unsigned type=(c->events&RECEIVING)?OP_RECEIVE:(c->events&SENDING)?OP_SEND:OP_POLL;
    if((c->events&(RECEIVING|SENDING|POLLING)) && !(c->events&CANCELING)) {
        struct io_uring_sqe *sqe=get_sqe(r);
        if(!sqe){schedule(r,c);return;}
        uint32_t slot=(uint32_t)(c-r->connections);
        io_uring_prep_cancel64(sqe,operation(slot,type),0);
        sqe->user_data=operation(slot,OP_CANCEL);
        c->events|=CANCELING;++r->inflight;
    }
    finish_close(r,c);
}
static void queue_receive(Ring *r,Connection *c) {
    struct io_uring_sqe *sqe=get_sqe(r);
    if(!sqe){schedule(r,c);return;}
    uint32_t slot=(uint32_t)(c-r->connections);
    io_uring_prep_recv(sqe,(int)slot,NULL,RECEIVE_BYTES,0);
    sqe->flags=IOSQE_FIXED_FILE|IOSQE_BUFFER_SELECT;
    sqe->buf_group=0;sqe->ioprio|=IORING_RECVSEND_POLL_FIRST;
    sqe->user_data=operation(slot,OP_RECEIVE);
    c->events|=RECEIVING;++r->inflight;
}
static void queue_send(Ring *r,Connection *c) {
    struct io_uring_sqe *sqe=get_sqe(r);
    if(!sqe){schedule(r,c);return;}
    uint32_t slot=(uint32_t)(c-r->connections);
    SendOp *op=send_op(r,c->sent);
    size_t header_sent=op->offset<op->header_n?op->offset:op->header_n;
    size_t body_sent=op->offset-header_sent;
    op->vectors[0]=(struct iovec){op->header+header_sent,op->header_n-header_sent};
    op->vectors[1]=(struct iovec){op->body.p?op->body.p+body_sent:NULL,op->body.n-body_sent};
    op->message.msg_iov=op->vectors;op->message.msg_iovlen=2;
    io_uring_prep_sendmsg(sqe,(int)slot,&op->message,MSG_NOSIGNAL);
    sqe->flags=IOSQE_FIXED_FILE;sqe->user_data=operation(slot,OP_SEND);
    c->events|=SENDING;++r->inflight;
}
static void queue_poll(Ring *r,Connection *c) {
    struct io_uring_sqe *sqe=get_sqe(r);
    if(!sqe){schedule(r,c);return;}
    uint32_t slot=(uint32_t)(c-r->connections);
    io_uring_prep_poll_add(sqe,(int)slot,POLLOUT|POLLERR|POLLHUP);
    sqe->flags=IOSQE_FIXED_FILE;sqe->user_data=operation(slot,OP_POLL);
    c->events|=POLLING;++r->inflight;
}
static void consume_input(Connection *c,size_t consumed) {
    if(!consumed)return;
    c->input.n-=(uint32_t)consumed;
    if(c->input.n)memmove(c->input.p,c->input.p+consumed,c->input.n);
    else retain_input(c);
}
static void service(Ring *r,Connection *c,Context *ctx,TransportState *s) {
    if(c->events&CLOSING){close_connection(r,c);return;}
    if(c->events&(RECEIVING|SENDING|POLLING))return;
    if(c->sent){queue_send(r,c);return;}
    if(c->close_after){close_connection(r,c);return;}
    size_t consumed=0;unsigned handled=0;uint32_t tail=0;int good=1;
    while(consumed<c->input.n && handled<ASC_REQUEST_SLICE) {
        Buffer visible=c->input;visible.p+=consumed;visible.n-=(uint32_t)consumed;
        Request q;Response response={0};Buffer decoded={0};size_t used=0;
        uint32_t slot=(uint32_t)(c-r->connections);
        int result=parse(ctx,&visible,&q,&decoded,&used,&r->machines[slot]);
        if(!result) {
            if(decoded.p)buffer_free(&decoded);
            if(c->eof){good=0;break;}
            if(q.expect_continue && !c->continue_sent) {
                uint32_t id=allocate_send(r);if(!id){good=0;break;}
                SendOp *op=send_op(r,id);
                static const char bytes[]="HTTP/1.1 100 Continue\r\n\r\n";
                memcpy(op->header,bytes,sizeof(bytes)-1);op->header_n=sizeof(bytes)-1;
                if(tail)send_op(r,tail)->next=id;else c->sent=id;
                tail=id;c->continue_sent=1;
            }
            break;
        }
        response.body=response_workspace;memset(&response_workspace,0,sizeof(response_workspace));
        if(result<0) {
            response.status=-result;q.close=1;used=visible.n;
            buffer_text(&response.body,"{\"error\":\"malformed HTTP request\"}");
        } else {ctx->app->handle(ctx,&q,&response);++s->requests;}
        ++handled;c->continue_sent=0;c->close_after=(uint8_t)q.close;
        uint32_t id=response.body.failed?0:allocate_send(r);
        if(!id) {
            retain_response(&response);if(decoded.p)buffer_free(&decoded);good=0;break;
        }
        SendOp *op=send_op(r,id);op->body=response.body;
        op->header_n=(uint32_t)response_header(op->header,response.status,op->body.n,q.close);
        if(tail)send_op(r,tail)->next=id;else c->sent=id;
        tail=id;if(decoded.p)buffer_free(&decoded);consumed+=used;
        if(q.close)break;
    }
    consume_input(c,consumed);
    if(!good){close_connection(r,c);return;}
    if(c->sent){queue_send(r,c);return;}
    if(c->eof || c->close_after){close_connection(r,c);return;}
    queue_receive(r,c);
}
static void run_ready(Ring *r,Context *ctx,TransportState *s,unsigned budget) {
    unsigned count=r->ready_n<budget?r->ready_n:budget;
    for(unsigned i=0;i<count;++i) {
        uint32_t slot=r->ready[r->ready_head];
        r->ready_head=(r->ready_head+1)%r->slots;--r->ready_n;
        Connection *c=&r->connections[slot];c->scheduled=0;
        service(r,c,ctx,s);
    }
}
static void completion(Ring *r,struct io_uring_cqe *cqe,Context *ctx,TransportState *s) {
    unsigned type=(unsigned)(cqe->user_data&7u);
    uint64_t index=cqe->user_data>>3;
    if(!r->inflight || index>=r->allocated){r->error=EPROTO;return;}
    --r->inflight;
    Connection *c=&r->connections[index];
    uint32_t pending=type==OP_RECEIVE?RECEIVING:type==OP_SEND?SENDING:type==OP_POLL?POLLING:CANCELING;
    if(!(c->events&pending)){r->error=EPROTO;return;}
    c->events&=~pending;
    int result=cqe->res;
    if(type==OP_RECEIVE) {
        unsigned selected=(cqe->flags&IORING_CQE_F_BUFFER)!=0;
        unsigned bid=cqe->flags>>IORING_CQE_BUFFER_SHIFT;
        if(selected && bid>=RECEIVE_BUFFERS){r->error=EPROTO;return;}
        if(result>0 && !(c->events&CLOSING)) {
            if(!selected || result>RECEIVE_BYTES)r->error=EPROTO;
            else {
                borrow_input(c);
                if((unsigned)result>131072u-c->input.n ||
                   !buffer_add(&c->input,r->receive+(size_t)bid*RECEIVE_BYTES,(size_t)result))c->events|=CLOSING;
            }
        }
        if(selected) {
            io_uring_buf_ring_add(r->buffers,r->receive+(size_t)bid*RECEIVE_BYTES,
                                 RECEIVE_BYTES,(unsigned short)bid,RECEIVE_BUFFERS-1,0);
            io_uring_buf_ring_advance(r->buffers,1);
        }
        /* EOF/errors can recycle a selected buffer inside the kernel. */
        if((selected || (result<=0 && result!=-ENOBUFS)) && r->returned<RECEIVE_BUFFERS)++r->returned;
        if(c->events&CLOSING){close_connection(r,c);return;}
        if(r->error)return;
        if(!result)c->eof=1;
        else if(result==-ENOBUFS){wait_for_buffer(r,c);return;}
        else if(result<0 && result!=-EINTR && result!=-EAGAIN){close_connection(r,c);return;}
        service(r,c,ctx,s);return;
    }
    if(type==OP_CANCEL) {
        if(result<0 && result!=-ENOENT && result!=-EALREADY && result!=-ECANCELED)r->error=-result;
        finish_close(r,c);return;
    }
    if(c->events&CLOSING){close_connection(r,c);return;}
    if(type==OP_POLL) {
        if(result==-EINTR){queue_poll(r,c);return;}
        if(result<0 || !(result&POLLOUT)){close_connection(r,c);return;}
        queue_send(r,c);return;
    }
    if(type!=OP_SEND || !c->sent){r->error=EPROTO;return;}
    if(result==-EINTR){queue_send(r,c);return;}
    if(result==-EAGAIN){queue_poll(r,c);return;}
    if(result<=0){close_connection(r,c);return;}
    SendOp *op=send_op(r,c->sent);
    size_t total=(size_t)op->header_n+op->body.n;
    if((size_t)result>total-op->offset){r->error=EPROTO;return;}
    op->offset+=(uint32_t)result;
    if(op->offset==total) {
        uint32_t id=c->sent;c->sent=op->next;release_send(r,id);
    }
    service(r,c,ctx,s);
}
static unsigned drain_completions(Ring *r,Context *ctx,TransportState *s) {
    struct io_uring_cqe *batch[256];
    unsigned count=io_uring_peek_batch_cqe(&r->uring,batch,256);
    for(unsigned i=0;i<count;++i)completion(r,batch[i],ctx,s);
    io_uring_cq_advance(&r->uring,count);
    retry_waiters(r);return count;
}
static void free_ring(Ring *r) {
    if(r->watched)epoll_ctl(r->fleet->epoll,EPOLL_CTL_DEL,r->uring.ring_fd,NULL);
    if(r->buffers)io_uring_free_buf_ring(&r->uring,r->buffers,RECEIVE_BUFFERS,0);
    if(r->initialized)io_uring_queue_exit(&r->uring);
    if(r->receive)munmap(r->receive,RECEIVE_BUFFERS*RECEIVE_BYTES);
    if(r->connections)munmap(r->connections,(size_t)r->slots*sizeof(*r->connections));
    if(r->machines)munmap(r->machines,(size_t)r->slots*sizeof(*r->machines));
    if(r->ready)munmap(r->ready,(size_t)r->slots*sizeof(*r->ready));
    if(r->waiting)munmap(r->waiting,(size_t)r->slots*sizeof(*r->waiting));
    for(uint32_t i=0;i<r->send_block_count;++i)free(r->send_blocks[i]);
    free(r->send_blocks);free(r);
}
static Ring *new_ring(Fleet *f) {
    Ring *r=calloc(1,sizeof(*r));if(!r)return NULL;
    r->fleet=f;r->slots=f->slots;r->free_slot=NO_SLOT;
    struct io_uring_params parameters={0};
    int result=io_uring_queue_init_params(RING_ENTRIES,&r->uring,&parameters);
    if(result<0)goto fail;
    r->initialized=1;
    if(!(parameters.features&IORING_FEAT_NODROP)){result=-EOPNOTSUPP;goto fail;}
    result=io_uring_register_files_sparse(&r->uring,r->slots);if(result<0)goto fail;
    r->buffers=io_uring_setup_buf_ring(&r->uring,RECEIVE_BUFFERS,0,0,&result);
    if(!r->buffers)goto fail;
    r->receive=zero_mapping(RECEIVE_BUFFERS*RECEIVE_BYTES);
    r->connections=zero_mapping((size_t)r->slots*sizeof(*r->connections));
    r->machines=zero_mapping((size_t)r->slots*sizeof(*r->machines));
    r->ready=zero_mapping((size_t)r->slots*sizeof(*r->ready));
    r->waiting=zero_mapping((size_t)r->slots*sizeof(*r->waiting));
    if(!r->receive || !r->connections || !r->machines || !r->ready || !r->waiting){result=-ENOMEM;goto fail;}
    for(unsigned i=0;i<RECEIVE_BUFFERS;++i)
        io_uring_buf_ring_add(r->buffers,r->receive+(size_t)i*RECEIVE_BYTES,RECEIVE_BYTES,
                             (unsigned short)i,RECEIVE_BUFFERS-1,(int)i);
    io_uring_buf_ring_advance(r->buffers,RECEIVE_BUFFERS);
    struct epoll_event event={.events=EPOLLIN,.data.ptr=r};
    if(epoll_ctl(f->epoll,EPOLL_CTL_ADD,r->uring.ring_fd,&event)){result=-errno;goto fail;}
    r->watched=1;r->next=f->rings;f->rings=r;f->available=r;++f->ring_count;
    fprintf(stderr,"io_uring: ring=%llu fixed_slots=%u receive_buffers=%u\n",
            (unsigned long long)f->ring_count,r->slots,RECEIVE_BUFFERS);
    return r;
fail:
    fprintf(stderr,"io_uring required: %s\n",strerror(-result));
    free_ring(r);errno=-result;return NULL;
}
static Ring *available_ring(Fleet *f) {
    Ring *r=f->available;
    if(r && r->active<r->slots)return r;
    for(r=f->rings;r;r=r->next)if(r->active<r->slots){f->available=r;return r;}
    return new_ring(f);
}
static int accept_connections(TransportState *s) {
    Fleet *f=s->internal;
    for(unsigned accepted=0;accepted<ASC_ACCEPT_SLICE;) {
        int client=accept4(s->listener,NULL,NULL,SOCK_NONBLOCK|SOCK_CLOEXEC);
        if(client<0) {
            if(errno==EINTR)continue;
            if(errno==EAGAIN || errno==EWOULDBLOCK)return 1;
            if(errno==ECONNABORTED || errno==EPROTO || errno==ENETDOWN ||
               errno==ENONET || errno==ENETUNREACH || errno==EHOSTDOWN ||
               errno==EHOSTUNREACH || errno==EOPNOTSUPP || errno==ENOPROTOOPT)return 1;
            return 0;
        }
        ++accepted;int one=1;
        if(setsockopt(client,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one))){close(client);continue;}
        Ring *r=available_ring(f);
        if(!r){int saved=errno;close(client);errno=saved;return 0;}
        uint32_t slot=r->free_slot;
        if(slot!=NO_SLOT)r->free_slot=(uint32_t)r->connections[slot].fd;
        else slot=r->allocated++;
        int result;
        do{result=io_uring_register_files_update(&r->uring,slot,&client,1);}while(result==-EINTR);
        int saved=result<0?-result:EIO;close(client);
        if(result!=1) {
            r->connections[slot].fd=(int)r->free_slot;r->free_slot=slot;errno=saved;return 0;
        }
        Connection *c=&r->connections[slot];memset(c,0,sizeof(*c));
        c->fd=(int)slot;c->events=CONNECTION_LIVE;
        ++r->active;++f->active;if(f->active>f->peak)f->peak=f->active;
        ++s->accepted;queue_receive(r,c);
        if(r->error){errno=r->error;return 0;}
    }
    return 1;
}
static TransportState *open_transport(const char *host,const char *port) {
    struct rlimit limit;
    if(getrlimit(RLIMIT_NOFILE,&limit))return NULL;
    if(!limit.rlim_cur){errno=EMFILE;return NULL;}
    struct addrinfo hints={.ai_family=AF_UNSPEC,.ai_socktype=SOCK_STREAM,.ai_flags=AI_PASSIVE},*addresses=NULL;
    if(getaddrinfo(host,port,&hints,&addresses)){errno=EINVAL;return NULL;}
    int listener=-1,one=1;
    for(struct addrinfo *a=addresses;a;a=a->ai_next) {
        listener=socket(a->ai_family,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
        if(listener<0)continue;
        setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
        if(!bind(listener,a->ai_addr,a->ai_addrlen) && !listen(listener,SOMAXCONN))break;
        close(listener);listener=-1;
    }
    freeaddrinfo(addresses);if(listener<0)return NULL;
    TransportState *s=calloc(1,sizeof(*s));Fleet *f=calloc(1,sizeof(*f));
    if(!s || !f){free(s);free(f);close(listener);return NULL;}
    s->listener=listener;s->epoll=epoll_create1(EPOLL_CLOEXEC);s->internal=f;
    f->epoll=s->epoll;f->slots=limit.rlim_cur<65535?(uint32_t)limit.rlim_cur:65535u;
    if(s->epoll<0)goto fail;
    struct epoll_event event={.events=EPOLLIN,.data.ptr=NULL};
    if(epoll_ctl(s->epoll,EPOLL_CTL_ADD,listener,&event) || !new_ring(f))goto fail;
    return s;
fail:
    {int saved=errno;close(listener);if(s->epoll>=0)close(s->epoll);free(f);free(s);errno=saved;return NULL;}
}
static int step(TransportState *s,Context *ctx,int timeout) {
    Fleet *f=s->internal;
    for(Ring *r=f->rings;r;r=r->next) {
        retry_waiters(r);run_ready(r,ctx,s,256);
        if(!flush_ring(r) || r->error){errno=r->error;return -1;}
        if(r->ready_n || io_uring_sq_ready(&r->uring))timeout=0;
    }
    struct epoll_event events[256];
    int count=epoll_wait(s->epoll,events,256,timeout);
    if(count<0)return errno==EINTR?0:-1;
    for(int i=0;i<count;++i) {
        if(events[i].data.fd==-1)continue;
        Ring *r=events[i].data.ptr;
        if(!r){if(!accept_connections(s))return -1;}
        else {
            drain_completions(r,ctx,s);run_ready(r,ctx,s,256);
            if(r->error){errno=r->error;return -1;}
        }
    }
    for(Ring *r=f->rings;r;r=r->next)
        if(!flush_ring(r) || r->error){errno=r->error;return -1;}
    return 0;
}
static int stop_ring(Ring *r,TransportState *s) {
    for(uint32_t i=0;i<r->allocated;++i) {
        Connection *c=&r->connections[i];
        if(c->events){c->events|=CLOSING;if(!c->scheduled)schedule(r,c);}
    }
    retry_waiters(r);
    while(r->active || r->inflight) {
        run_ready(r,NULL,s,256);
        if(!flush_ring(r))return 0;
        unsigned completed=drain_completions(r,NULL,s);
        if(r->error)return 0;
        if(!completed && !r->ready_n && r->inflight && !io_uring_sq_ready(&r->uring)) {
            int result;
            do{result=io_uring_submit_and_wait(&r->uring,1);}while(result==-EINTR);
            if(result<0 && result!=-EBUSY){r->error=-result;return 0;}
        }
    }
    return 1;
}
static void close_transport(TransportState *s) {
    if(!s)return;
    Fleet *f=s->internal;f->stopping=1;
    for(Ring *r=f->rings;r;r=r->next)if(!stop_ring(r,s)) {
        fprintf(stderr,"io_uring shutdown: %s; retaining live buffers until process exit\n",strerror(r->error));
        return;
    }
    fprintf(stderr,"io_uring: rings=%llu peak_connections=%llu active=%llu\n",
            (unsigned long long)f->ring_count,(unsigned long long)f->peak,(unsigned long long)f->active);
    Ring *r=f->rings;
    while(r){Ring *next=r->next;free_ring(r);r=next;}
    close(s->listener);close(s->epoll);free(f);free(s);
}
const void *ASC_MODULE_ENTRY(void) {
    static const TransportAPI api={{ASC_ABI,sizeof(TransportAPI),"io_uring/fixed-file-rings"},
        sizeof(TransportState),sizeof(Connection),open_transport,step,close_transport};
    return &api;
}

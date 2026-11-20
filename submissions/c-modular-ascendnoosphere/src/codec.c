#include "abi.h"
#include "yyjson.h"
#include <math.h>

typedef struct {
    _Alignas(max_align_t) unsigned char bytes[2048];
    yyjson_alc allocator;
} JsonScratch;
static yyjson_doc *read_json(Slice input,JsonScratch *scratch) {
    size_t required=yyjson_read_max_memory_usage(input.n,0);
    if(required && required<=sizeof(scratch->bytes) &&
       yyjson_alc_pool_init(&scratch->allocator,scratch->bytes,sizeof(scratch->bytes)))
        return yyjson_read_opts((char *)input.p,input.n,0,&scratch->allocator,NULL);
    return yyjson_read(input.p,input.n,0);
}

static uint32_t codepoint(const unsigned char *p, size_t *width) {
    unsigned c = p[0];
    if (c < 128) { *width=1; return c; }
    if (c < 224) { *width=2; return ((c&31)<<6)|(p[1]&63); }
    if (c < 240) { *width=3; return ((c&15)<<12)|((p[1]&63)<<6)|(p[2]&63); }
    *width=4; return ((c&7)<<18)|((p[1]&63)<<12)|((p[2]&63)<<6)|(p[3]&63);
}
static unsigned whitespace(uint32_t c) {
    return ((c>=9)&(c<=13)) | (c==32) | (c==0xa0) | (c==0x1680) |
           ((c>=0x2000)&(c<=0x200a)) | (c==0x2028) | (c==0x2029) |
           (c==0x202f) | (c==0x205f) | (c==0x3000) | (c==0xfeff);
}
static int body(Slice input, Body *out) {
    JsonScratch scratch;
    yyjson_doc *doc=read_json(input,&scratch);
    if (!doc) return BODY_MALFORMED;
    yyjson_val *value = yyjson_obj_get(yyjson_doc_get_root(doc),"body");
    int status = BODY_REQUIRED;
    if (yyjson_is_str(value)) {
        const unsigned char *p = (const unsigned char *)yyjson_get_str(value);
        size_t n=yyjson_get_len(value), start=0, end=n;
        while (start<n) {
            size_t width; uint32_t cp=codepoint(p+start,&width);
            if (!whitespace(cp)) break;
            start+=width;
        }
        /* yyjson has already validated UTF-8. */
        while (end>start) {
            size_t last=end-1;
            while (last>start && (p[last]&0xc0u)==0x80u) --last;
            size_t width; uint32_t cp=codepoint(p+last,&width);
            if (!whitespace(cp)) break;
            end=last;
        }
        if (end>start) {
            status=BODY_LONG;
            size_t bytes=end-start;
            if (bytes<=2000) {

                unsigned count=0;
                for (size_t i=start;i<end;++i) count+=(p[i]&0xc0u)!=0x80u;
                if (count<=500) {
                    out->n=bytes; out->characters=count;
                    memcpy(out->text,p+start,bytes); out->text[bytes]=0; status=BODY_OK;
                }
            }
        }
    }
    yyjson_doc_free(doc); return status;
}
static int jwt_header_general(Slice input) {
    JsonScratch scratch;
    yyjson_doc *doc=read_json(input,&scratch);
    if (!doc) return 0;
    yyjson_val *root=yyjson_doc_get_root(doc);
    yyjson_val *algorithm=NULL,*key,*value;size_t idx,max;unsigned seen=0;
    int valid=yyjson_is_obj(root);
    yyjson_obj_foreach(root,idx,max,key,value) {
        if(yyjson_equals_str(key,"alg")) {
            if(seen++){valid=0;break;}
            algorithm=value;
        }
    }
    valid=valid && yyjson_equals_str(algorithm,"HS256");
    yyjson_doc_free(doc); return valid;
}
static int jwt_header(Slice input) {
    static const char a[]="{\"alg\":\"HS256\",\"typ\":\"JWT\"}";
    static const char b[]="{\"alg\":\"HS256\"}";
    if((input.n==sizeof(a)-1&&!memcmp(input.p,a,sizeof(a)-1)) ||
       (input.n==sizeof(b)-1&&!memcmp(input.p,b,sizeof(b)-1)))return 1;
    return jwt_header_general(input);
}

static int claims(Slice input, Claims *out) {

    out->sub_n=0; out->username_n=0; out->exp=0;
    out->payload_valid=0; out->exp_valid=0;
    out->sub[0]=0; out->username[0]=0;
    JsonScratch scratch;
    yyjson_doc *doc=read_json(input,&scratch);
    if (!doc) return 0;
    yyjson_val *root=yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root)) { yyjson_doc_free(doc); return 0; }
    yyjson_val *sub=NULL,*name=NULL,*exp=NULL,*key,*value;size_t idx,max;unsigned seen=0;
    yyjson_obj_foreach(root,idx,max,key,value) {
        unsigned bit=yyjson_equals_str(key,"sub")?1u:yyjson_equals_str(key,"username")?2u:yyjson_equals_str(key,"exp")?4u:0u;
        if(!bit)continue;
        if(seen&bit){yyjson_doc_free(doc);return 0;}
        seen|=bit;
        if(bit==1)sub=value;else if(bit==2)name=value;else exp=value;
    }
    out->exp=yyjson_get_num(exp);
    out->exp_valid=yyjson_is_num(exp) && isfinite(out->exp);
    out->payload_valid=yyjson_is_str(sub) && yyjson_is_str(name) &&
        yyjson_get_len(sub)<sizeof(out->sub) && yyjson_get_len(name)<sizeof(out->username);
    if (out->payload_valid) {
        out->sub_n=yyjson_get_len(sub); out->username_n=yyjson_get_len(name);
        memcpy(out->sub,yyjson_get_str(sub),out->sub_n);
        memcpy(out->username,yyjson_get_str(name),out->username_n);
        out->sub[out->sub_n]=0; out->username[out->username_n]=0;
    }
    yyjson_doc_free(doc); return 1;
}
/* The caller already scanned the first ordinary run. */
static int string_sparse(Buffer *b, Slice s, const MathAPI *math, size_t first) {
    static const char hex[]="0123456789abcdef";
    buffer_add(b,"\"",1);
    buffer_add(b,s.p,first);
    size_t i=first;
    while (i<s.n) {
        unsigned char c=(unsigned char)s.p[i++];
        char encoded[2][6]={{'\\','u','0','0',hex[c>>4],hex[c&15]},{'\\',(char)c,0,0,0,0}};
        unsigned simple=(c=='"')|(c=='\\');
        buffer_add(b,encoded[simple],6-4*simple);
        if (i==s.n) break;
        size_t run=math->special((const unsigned char *)s.p+i,s.n-i);
        buffer_add(b,s.p+i,run); i+=run;
    }
    buffer_add(b,"\"",1); return !b->failed;
}
static int string_from(Buffer *b, Slice s, const MathAPI *math, size_t first) {
    static const char hex[]="0123456789abcdef";
    if (b->failed) return 0;
    /* Check lengths before adding to avoid overflow. */
    if (b->n>ASC_BUFFER_MAX || ASC_BUFFER_MAX-b->n<2 ||
        s.n>ASC_BUFFER_MAX-b->n-2) { b->failed=1; return 0; }
    if (first==SIZE_MAX) first=math->special((const unsigned char *)s.p,s.n);
    if (first<s.n) {
        /* Use a full expansion scan only for nearby escapes. */
        size_t window=s.n-first;
        if (window>8) window=8;
        unsigned specials=1;
        for (size_t offset=1;offset<window;++offset) {
            unsigned c=(unsigned char)s.p[first+offset];
            specials+=(c<32)|(c=='"')|(c=='\\');
        }
        if (specials<2) return string_sparse(b,s,math,first);
    }
    unsigned expansion=0;
    for (size_t i=first;i<s.n;++i) {
        unsigned c=(unsigned char)s.p[i];
        /* Controls add five bytes; quotes and backslashes add one. */
        expansion+=5u*(c<32)+((c=='"')|(c=='\\'));
    }
    size_t length=s.n+2+expansion;
    if (length>ASC_BUFFER_MAX-b->n) { b->failed=1; return 0; }
    size_t need=b->n+length;
    if (need>b->cap) {
        size_t capacity=b->cap ? b->cap : 512;
        while (capacity<need) capacity<<=1;
        char *next=realloc(b->p,capacity);
        if (!next) { b->failed=1; return 0; }
        b->p=next; b->cap=(uint32_t)capacity;
    }
    char *cursor=b->p+b->n;
    *cursor++='"';
    if (first) { memcpy(cursor,s.p,first); cursor+=first; }
    size_t i=first;
    while (i<s.n) {

        unsigned c;
        do {
            c=(unsigned char)s.p[i++];
            if (c<32) {
                cursor[0]='\\'; cursor[1]='u'; cursor[2]='0'; cursor[3]='0';
                cursor[4]=hex[c>>4]; cursor[5]=hex[c&15]; cursor+=6;
            } else {
                cursor[0]='\\'; cursor[1]=(char)c; cursor+=2;
            }
            if (i==s.n) break;
            c=(unsigned char)s.p[i];
        } while ((c<32)|(c=='"')|(c=='\\'));
        if (i==s.n) break;
        /* Copy short ordinary runs without restarting the scanner. */
        unsigned copied=0;
        while (copied<8 && i<s.n) {
            c=(unsigned char)s.p[i];
            if ((c<32)|(c=='"')|(c=='\\')) break;
            *cursor++=(char)c; ++i; ++copied;
        }
        if (copied==8 && i<s.n) {
            size_t run=math->special((const unsigned char *)s.p+i,s.n-i);
            memcpy(cursor,s.p+i,run); cursor+=run; i+=run;
        }
    }
    *cursor='"'; b->n=(uint32_t)need; return 1;
}
static int string(Buffer *b, Slice s, const MathAPI *math) {
    return string_from(b,s,math,SIZE_MAX);
}
static Slice integer_text(char buf[21], int64_t value) {
    static const char digits[]=
        "0001020304050607080910111213141516171819"
        "2021222324252627282930313233343536373839"
        "4041424344454647484950515253545556575859"
        "6061626364656667686970717273747576777879"
        "8081828384858687888990919293949596979899";
    char *end=buf+21, *p=end;
    unsigned negative=value<0;
    uint64_t mask=0-(uint64_t)negative;
    uint64_t n=((uint64_t)value^mask)+negative;
    while (n>=100) { unsigned rem=(unsigned)(n%100); n/=100; p-=2; memcpy(p,digits+2*rem,2); }
    /* Scratch has room for the unused leading digit. */
    p-=2; memcpy(p,digits+2*n,2); p+=(n<10);
    if (negative) *--p='-';
    return (Slice){p,(size_t)(end-p)};
}
static int integer(Buffer *b, int64_t value) {
    char text[21]; Slice result=integer_text(text,value);
    return buffer_add(b,result.p,result.n);
}
static int post_fallback(Buffer *b, const Post *p, const MathAPI *math,
                         size_t body_first, size_t created_first, size_t author_first) {
    buffer_text(b,"{\"id\":"); integer(b,p->id);
    buffer_text(b,",\"body\":"); string_from(b,p->body,math,body_first);
    buffer_text(b,",\"created_at\":"); string_from(b,p->created,math,created_first);
    buffer_text(b,",\"author\":"); string_from(b,p->author,math,author_first);
    buffer_text(b,",\"like_count\":"); integer(b,p->likes);
    return buffer_add(b,"}",1);
}
/* Reserve the complete unescaped record once. */
static int post(Buffer *b, const Post *p, const MathAPI *math) {
    static const char a[]="{\"id\":";
    static const char c[] =",\"body\":\"";
    static const char d[] ="\",\"created_at\":\"";
    static const char e[] ="\",\"author\":\"";
    static const char f[] ="\",\"like_count\":";
    if (b->failed) return 0;
    /* Bound each length before summing to avoid overflow. */
    if (p->body.n>ASC_BUFFER_MAX || p->created.n>ASC_BUFFER_MAX || p->author.n>ASC_BUFFER_MAX) {
        b->failed=1; return 0;
    }
    size_t body_first=math->special((const unsigned char *)p->body.p,p->body.n);
    size_t created_first=math->special((const unsigned char *)p->created.p,p->created.n);
    size_t author_first=math->special((const unsigned char *)p->author.p,p->author.n);
    /* Reuse scanned prefixes in the escape path. */
    if (body_first!=p->body.n || created_first!=p->created.n || author_first!=p->author.n)
        return post_fallback(b,p,math,body_first,created_first,author_first);
    char id_text[21], like_text[21];
    Slice id=integer_text(id_text,p->id), likes=integer_text(like_text,p->likes);
    size_t length=(sizeof(a)-1)+(sizeof(c)-1)+(sizeof(d)-1)+(sizeof(e)-1)+(sizeof(f)-1)+1+
                  p->body.n+p->created.n+p->author.n+id.n+likes.n;
    if (length>ASC_BUFFER_MAX-b->n) { b->failed=1; return 0; }
    size_t need=b->n+length;
    if (need>b->cap) {
        size_t capacity=b->cap ? b->cap : 512;
        while (capacity<need) capacity<<=1;
        char *next=realloc(b->p,capacity);
        if (!next) { b->failed=1; return 0; }
        b->p=next; b->cap=(uint32_t)capacity;
    }
    char *cursor=b->p+b->n;
#define APPEND_BYTES(source,length) do { \
    size_t piece_length=(length); \
    if (piece_length) memcpy(cursor,(source),piece_length); \
    cursor+=piece_length; \
} while (0)
    APPEND_BYTES(a,sizeof(a)-1);
    APPEND_BYTES(id.p,id.n);
    APPEND_BYTES(c,sizeof(c)-1);
    APPEND_BYTES(p->body.p,p->body.n);
    APPEND_BYTES(d,sizeof(d)-1);
    APPEND_BYTES(p->created.p,p->created.n);
    APPEND_BYTES(e,sizeof(e)-1);
    APPEND_BYTES(p->author.p,p->author.n);
    APPEND_BYTES(f,sizeof(f)-1);
    APPEND_BYTES(likes.p,likes.n);
    *cursor='}';
#undef APPEND_BYTES
    b->n=(uint32_t)need;
    return 1;
}
const void *ASC_MODULE_ENTRY(void) {
    static const CodecAPI api={{ASC_ABI,sizeof(CodecAPI),"yyjson-0.10.0/prefix-indicator-adaptive-escapes/scan-reuse/short-runs"},body,jwt_header,claims,string,integer,post};
    return &api;
}

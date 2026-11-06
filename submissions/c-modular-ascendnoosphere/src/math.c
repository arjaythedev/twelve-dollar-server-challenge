#include "abi.h"
#include <limits.h>
#include <stdio.h>
#if ASC_X86
#include <immintrin.h>
#endif
#ifndef MATH_FORCE
#define MATH_FORCE 0
#endif

static size_t special_scalar(const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if ((p[i] < 32) | (p[i] == '"') | (p[i] == '\\')) return i;
    return n;
}

static size_t header_scalar(const unsigned char *p, size_t n) {
    for (size_t i = 0; i + 4 <= n; ++i)
        if ((p[i] == 13) & (p[i+1] == 10) & (p[i+2] == 13) & (p[i+3] == 10)) return i+4;
    return 0;
}

static int digits8_scalar(const unsigned char *p, uint32_t *out) {
    uint32_t value = 0, invalid = 0;
    for (unsigned i = 0; i < 8; ++i) {
        uint32_t d = (uint32_t)p[i] - '0';
        invalid |= d > 9;
        value = 10*value + d;
    }
    *out = value; return !invalid;
}

static int decimal_scalar(Slice s, int64_t *out) {
    if (!s.n || s.n > 19) return 0;
    uint64_t value = 0; size_t i = 0;
    for (; s.n-i >= 8; i += 8) {
        uint32_t part;
        if (!digits8_scalar((const unsigned char *)s.p+i, &part)) return 0;
        value = value*100000000u + part;
    }
    unsigned invalid=0;
    for (; i < s.n; ++i) {
        unsigned d = (unsigned char)s.p[i] - (unsigned)'0';
        invalid |= d>9;
        value = 10*value + d;
    }
    /* 19 decimal digits fit in uint64_t. */
    if (invalid || value>(uint64_t)INT64_MAX) return 0;
    *out = (int64_t)value; return 1;
}

static unsigned equal(Slice s, const char *p, size_t n) { return s.n == n && !memcmp(s.p,p,n); }

static unsigned route(Slice method,Slice path,Slice *id) {
    *id=(Slice){path.p,0};
    if(equal(method,"GET",3)) {
        if(path.n==5)return equal(path,"/feed",5)?ROUTE_FEED:ROUTE_NONE;
        if(path.n==7)return equal(path,"/health",7)?ROUTE_HEALTH:ROUTE_NONE;
        if(path.n<8 || memcmp(path.p,"/posts/",7))return ROUTE_NONE;
        if(memchr(path.p+7,'/',path.n-7))return ROUTE_NONE;
        *id=(Slice){path.p+7,path.n-7};return ROUTE_POST;
    }
    if(!equal(method,"POST",4))return ROUTE_NONE;
    if(path.n==6)return equal(path,"/posts",6)?ROUTE_CREATE:ROUTE_NONE;
    if(path.n<13 || memcmp(path.p,"/posts/",7) || memcmp(path.p+path.n-5,"/like",5))return ROUTE_NONE;
    if(memchr(path.p+7,'/',path.n-12))return ROUTE_NONE;
    *id=(Slice){path.p+7,path.n-12};return ROUTE_LIKE;
}

static const unsigned char base64_inverse[256]={
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,62,255,255,
    52,53,54,55,56,57,58,59,60,61,255,255,255,255,255,255,
    255,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,
    15,16,17,18,19,20,21,22,23,24,25,255,255,255,255,63,
    255,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
    41,42,43,44,45,46,47,48,49,50,51,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
};

static int base64_scalar(Slice s,unsigned char *out,size_t cap,size_t *length) {
    if((s.n&3u)==1 || (s.n>>2)*3+(((s.n&3u)*3)>>2)>cap)return 0;
    size_t i=0,n=0;
    for(;s.n-i>=4;i+=4,n+=3) {
        unsigned a=base64_inverse[(unsigned char)s.p[i]],b=base64_inverse[(unsigned char)s.p[i+1]];
        unsigned c=base64_inverse[(unsigned char)s.p[i+2]],d=base64_inverse[(unsigned char)s.p[i+3]];
        if((a|b|c|d)>63)return 0;
        out[n]=(unsigned char)((a<<2)|(b>>4));
        out[n+1]=(unsigned char)((b<<4)|(c>>2));out[n+2]=(unsigned char)((c<<6)|d);
    }
    size_t tail=s.n-i;
    if(tail) {
        unsigned a=base64_inverse[(unsigned char)s.p[i]],b=base64_inverse[(unsigned char)s.p[i+1]];
        if((a|b)>63)return 0;
        out[n++]=(unsigned char)((a<<2)|(b>>4));
        if(tail==2) {if(b&15u)return 0;}
        else {
            unsigned c=base64_inverse[(unsigned char)s.p[i+2]];
            if(c>63 || (c&3u))return 0;
            out[n++]=(unsigned char)((b<<4)|(c>>2));
        }
    }
    *length=n;return 1;
}

#if ASC_X86

__attribute__((target("avx2")))
static size_t special_avx2(const unsigned char *p, size_t n) {
    size_t i = 0;
    const __m256i quote = _mm256_set1_epi8('"'), slash = _mm256_set1_epi8('\\');
    const __m256i low = _mm256_set1_epi8(31), zero = _mm256_setzero_si256();
    for (; n-i >= 32; i += 32) {
        __m256i v = _mm256_loadu_si256((const __m256i *)(p+i));
        __m256i mask = _mm256_or_si256(_mm256_cmpeq_epi8(v, quote), _mm256_cmpeq_epi8(v, slash));
        mask = _mm256_or_si256(mask, _mm256_cmpeq_epi8(_mm256_subs_epu8(v, low), zero));
        uint32_t bits = (uint32_t)_mm256_movemask_epi8(mask);
        if (bits) return i + (unsigned)__builtin_ctz(bits);
    }
    if (i && n-i) {
        /* Overlap the tail with the last in-bounds vector. */
        __m256i v = _mm256_loadu_si256((const __m256i *)(p+n-32));
        __m256i mask = _mm256_or_si256(_mm256_cmpeq_epi8(v, quote), _mm256_cmpeq_epi8(v, slash));
        mask = _mm256_or_si256(mask, _mm256_cmpeq_epi8(_mm256_subs_epu8(v, low), zero));
        uint32_t bits = (uint32_t)_mm256_movemask_epi8(mask);
        return bits ? n-32+(unsigned)__builtin_ctz(bits) : n;
    }
    return i + special_scalar(p+i, n-i);
}

__attribute__((target("avx512f,avx512bw")))
static size_t special_avx512(const unsigned char *p, size_t n) {
    size_t i = 0;
    for (; n-i >= 64; i += 64) {
        __m512i v = _mm512_loadu_si512(p+i);
        __mmask64 classes=_kor_mask64(_mm512_cmplt_epu8_mask(v, _mm512_set1_epi8(32)),
            _kor_mask64(_mm512_cmpeq_epi8_mask(v, _mm512_set1_epi8('"')),
                        _mm512_cmpeq_epi8_mask(v, _mm512_set1_epi8('\\'))));
        uint64_t bits=(uint64_t)classes;
        if (bits) return i + (unsigned)__builtin_ctzll(bits);
    }
    /* Mask inactive lanes, which compare as controls. */
    size_t tail = n-i;
    if (!tail) return n;
    uint64_t lanes = (UINT64_C(1) << tail)-1;
    __m512i v = _mm512_maskz_loadu_epi8((__mmask64)lanes, p+i);
    __mmask64 classes=_kor_mask64(_mm512_cmplt_epu8_mask(v, _mm512_set1_epi8(32)),
        _kor_mask64(_mm512_cmpeq_epi8_mask(v, _mm512_set1_epi8('"')),
                    _mm512_cmpeq_epi8_mask(v, _mm512_set1_epi8('\\'))));
    uint64_t bits=(uint64_t)_kand_mask64((__mmask64)lanes,classes);
    return bits ? i + (unsigned)__builtin_ctzll(bits) : n;
}

__attribute__((target("avx2")))
static size_t header_avx2(const unsigned char *p, size_t n) {
    size_t i = 0;
    __m256i cr = _mm256_set1_epi8(13), lf = _mm256_set1_epi8(10);
    /* Two CRLF pairs two bytes apart form the terminator. */
    for (; n-i >= 32; i += 29) {
        __m256i v = _mm256_loadu_si256((const __m256i *)(p+i));
        uint32_t a = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, cr));
        uint32_t b = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, lf));
        uint32_t pairs = a & (b >> 1), bits = pairs & (pairs >> 2);
        if (bits) return i + (unsigned)__builtin_ctz(bits) + 4;
    }
    /* Overlap the last vector to cover the remaining candidates. */
    if (i && n-i>=4) {
        __m256i v = _mm256_loadu_si256((const __m256i *)(p+n-32));
        uint32_t a = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, cr));
        uint32_t b = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, lf));
        uint32_t pairs = a & (b >> 1), bits = pairs & (pairs >> 2);
        return bits ? n-32+(unsigned)__builtin_ctz(bits)+4 : 0;
    }
    size_t tail = header_scalar(p+i, n-i);
    return tail ? i+tail : 0;
}

__attribute__((target("avx512f,avx512bw")))
static size_t header_masked_residual(const unsigned char *p,size_t n) {
    /* Inactive zero lanes cannot match CR or LF. */
    if (n<4) return 0;
    uint64_t active=(UINT64_C(1)<<n)-1;
    __m512i bytes=_mm512_maskz_loadu_epi8((__mmask64)active,p);
    uint64_t cr=_mm512_cmpeq_epi8_mask(bytes,_mm512_set1_epi8(13));
    uint64_t lf=_mm512_cmpeq_epi8_mask(bytes,_mm512_set1_epi8(10));
    uint64_t pairs=cr&(lf>>1), hits=pairs&(pairs>>2);
    return hits ? (unsigned)__builtin_ctzll(hits)+4 : 0;
}

__attribute__((target("avx512f,avx512bw")))
static size_t header_avx512(const unsigned char *p,size_t n) {
    size_t i=0;
    for(;n-i>=64;i+=61) {
        __m512i v=_mm512_loadu_si512(p+i);
        uint64_t cr=(uint64_t)_mm512_cmpeq_epi8_mask(v,_mm512_set1_epi8(13)),lf=(uint64_t)_mm512_cmpeq_epi8_mask(v,_mm512_set1_epi8(10));
        uint64_t pairs=cr&(lf>>1),hits=pairs&(pairs>>2);
        if(__builtin_expect(hits!=0,0))return i+(unsigned)__builtin_ctzll(hits)+4;
    }
    size_t rest=header_masked_residual(p+i,n-i);return rest?i+rest:0;
}

__attribute__((target("ssse3,sse4.1")))
static int digits8_sse(const unsigned char *p, uint32_t *out) {
    __m128i d = _mm_sub_epi8(_mm_loadl_epi64((const __m128i *)p), _mm_set1_epi8('0'));
    __m128i bad = _mm_or_si128(_mm_cmplt_epi8(d, _mm_setzero_si128()), _mm_cmpgt_epi8(d, _mm_set1_epi8(9)));
    unsigned invalid = (unsigned)_mm_movemask_epi8(bad) & 255;
    __m128i pairs = _mm_maddubs_epi16(d, _mm_setr_epi8(10,1,10,1,10,1,10,1,0,0,0,0,0,0,0,0));
    __m128i quads = _mm_madd_epi16(pairs, _mm_setr_epi16(100,1,100,1,0,0,0,0));
    *out = (uint32_t)_mm_cvtsi128_si32(quads)*10000u + (uint32_t)_mm_extract_epi32(quads,1);
    return !invalid;
}

__attribute__((target("ssse3,sse4.1")))
static int decimal_sse(Slice s, int64_t *out) {
    if (!s.n || s.n > 19) return 0;
    uint64_t value = 0; size_t i = 0;
    for (; s.n-i >= 8; i += 8) {
        uint32_t part;
        if (!digits8_sse((const unsigned char *)s.p+i, &part)) return 0;
        value = value*100000000u + part;
    }
    unsigned invalid=0;
    for (; i < s.n; ++i) {
        unsigned d = (unsigned char)s.p[i] - (unsigned)'0';
        invalid |= d>9;
        value = 10*value + d;
    }
    /* 19 decimal digits fit in uint64_t. */
    if (invalid || value>(uint64_t)INT64_MAX) return 0;
    *out = (int64_t)value; return 1;
}

__attribute__((target("avx512f,avx512bw,avx512vl,ssse3,sse4.1")))
static int small_dot(const unsigned char *p,size_t n,uint32_t *out) {
    static const signed char align[8][16]={
        {-1,-1,-1,-1,-1,-1,-1,0,-1,-1,-1,-1,-1,-1,-1,-1},
        {-1,-1,-1,-1,-1,-1,0,1,-1,-1,-1,-1,-1,-1,-1,-1},
        {-1,-1,-1,-1,-1,0,1,2,-1,-1,-1,-1,-1,-1,-1,-1},
        {-1,-1,-1,-1,0,1,2,3,-1,-1,-1,-1,-1,-1,-1,-1},
        {-1,-1,-1,0,1,2,3,4,-1,-1,-1,-1,-1,-1,-1,-1},
        {-1,-1,0,1,2,3,4,5,-1,-1,-1,-1,-1,-1,-1,-1},
        {-1,0,1,2,3,4,5,6,-1,-1,-1,-1,-1,-1,-1,-1},
        {0,1,2,3,4,5,6,7,-1,-1,-1,-1,-1,-1,-1,-1}};
    __m128i d=_mm_sub_epi8(_mm_maskz_loadu_epi8((__mmask16)((1u<<n)-1),p),_mm_set1_epi8('0'));
    d=_mm_shuffle_epi8(d,_mm_loadu_si128((const __m128i *)align[n-1]));
    unsigned invalid=(unsigned)_mm_movemask_epi8(_mm_or_si128(_mm_cmplt_epi8(d,_mm_setzero_si128()),_mm_cmpgt_epi8(d,_mm_set1_epi8(9))));
    __m128i pairs=_mm_maddubs_epi16(d,_mm_setr_epi8(10,1,10,1,10,1,10,1,0,0,0,0,0,0,0,0));
    __m128i quads=_mm_madd_epi16(pairs,_mm_setr_epi16(100,1,100,1,0,0,0,0));
    *out=(uint32_t)_mm_cvtsi128_si32(quads)*10000u+(uint32_t)_mm_extract_epi32(quads,1);
    return !invalid;
}

__attribute__((target("avx512f,avx512bw,avx512vl,ssse3,sse4.1")))
static int decimal_masked(Slice s,int64_t *out) {
    static const uint32_t power[]={1,10,100,1000,10000,100000,1000000,10000000,100000000};
    if (!s.n || s.n>19) return 0;
    uint64_t value=0; size_t i=0;
    while (s.n-i>8) {
        uint32_t part;
        if (!digits8_sse((const unsigned char *)s.p+i,&part)) return 0;
        value=value*100000000u+part; i+=8;
    }
    uint32_t tail; size_t n=s.n-i;
    if (!small_dot((const unsigned char *)s.p+i,n,&tail)) return 0;
    value=value*power[n]+tail;
    if (value>(uint64_t)INT64_MAX) return 0;
    *out=(int64_t)value; return 1;
}

__attribute__((target("avx2")))
static __m256i between_avx2(__m256i c, unsigned char start, char width) {
    __m256i delta=_mm256_sub_epi8(c,_mm256_set1_epi8((char)start));
    return _mm256_cmpeq_epi8(_mm256_subs_epu8(delta,_mm256_set1_epi8(width)),_mm256_setzero_si256());
}

__attribute__((target("avx2")))
static int base64_avx2(Slice s, unsigned char *out, size_t cap, size_t *length) {
    if(!s.n){*length=0;return 1;}
    if ((s.n&3u)==1 || (s.n>>2)*3+(((s.n&3u)*3)>>2)>cap) return 0;
    size_t i=0,n=0; uint32_t invalid=0;
    for (;s.n-i>=32;i+=32,n+=24) {
        __m256i c=_mm256_loadu_si256((const __m256i *)(s.p+i));
        __m256i upper=between_avx2(c,'A',25), lower=between_avx2(c,'a',25), digit=between_avx2(c,'0',9);
        __m256i minus=_mm256_cmpeq_epi8(c,_mm256_set1_epi8('-')), under=_mm256_cmpeq_epi8(c,_mm256_set1_epi8('_'));
        __m256i valid=_mm256_or_si256(_mm256_or_si256(upper,lower),_mm256_or_si256(digit,_mm256_or_si256(minus,under)));
        invalid|=~(uint32_t)_mm256_movemask_epi8(valid);
        __m256i bias=_mm256_set1_epi8(65);
        bias=_mm256_blendv_epi8(bias,_mm256_set1_epi8(71),lower);
        bias=_mm256_blendv_epi8(bias,_mm256_set1_epi8(-4),digit);
        bias=_mm256_blendv_epi8(bias,_mm256_set1_epi8(-17),minus);
        bias=_mm256_blendv_epi8(bias,_mm256_set1_epi8(32),under);
        __m256i values=_mm256_sub_epi8(c,bias);
        __m256i pairs=_mm256_maddubs_epi16(values,_mm256_set1_epi32(0x01400140));
        __m256i triples=_mm256_madd_epi16(pairs,_mm256_set1_epi32(0x00011000));
        __m256i packed=_mm256_shuffle_epi8(triples,_mm256_setr_epi8(
            2,1,0,6,5,4,10,9,8,14,13,12,-1,-1,-1,-1,
            2,1,0,6,5,4,10,9,8,14,13,12,-1,-1,-1,-1));
        unsigned char bytes[32]; _mm256_storeu_si256((__m256i *)bytes,packed);
        memcpy(out+n,bytes,12); memcpy(out+n+12,bytes+16,12);
    }
    size_t tail=0;
    int ok=base64_scalar((Slice){s.p+i,s.n-i},out+n,cap-n,&tail);
    *length=n+tail; return ok && !invalid;
}

__attribute__((target("avx512f,avx512bw")))
static int base64_tail_wide(Slice s, unsigned char *out, size_t cap, size_t *length) {
    if(!s.n){*length=0;return 1;}
    if ((s.n&3u)==1 || (s.n>>2)*3+(((s.n&3u)*3)>>2)>cap) return 0;
    size_t i=0,n=0; unsigned invalid=0;
    while (i<s.n) {
        size_t width=s.n-i;
        if (width>64) width=64;
        /* width is 1..64; keep shifts below 64. */
        uint64_t active=UINT64_MAX>>(64-width);
        __m512i c=_mm512_maskz_loadu_epi8((__mmask64)active,s.p+i);
        uint64_t upper=_mm512_cmple_epu8_mask(_mm512_sub_epi8(c,_mm512_set1_epi8('A')),_mm512_set1_epi8(25));
        uint64_t lower=_mm512_cmple_epu8_mask(_mm512_sub_epi8(c,_mm512_set1_epi8('a')),_mm512_set1_epi8(25));
        uint64_t digit=_mm512_cmple_epu8_mask(_mm512_sub_epi8(c,_mm512_set1_epi8('0')),_mm512_set1_epi8(9));
        uint64_t minus=_mm512_cmpeq_epi8_mask(c,_mm512_set1_epi8('-'));
        uint64_t under=_mm512_cmpeq_epi8_mask(c,_mm512_set1_epi8('_'));
        __mmask64 classes=_kor_mask64(_kor_mask64((__mmask64)upper,(__mmask64)lower),
            _kor_mask64((__mmask64)digit,_kor_mask64((__mmask64)minus,(__mmask64)under)));
        __mmask64 bad=_kandn_mask64(classes,(__mmask64)active);
        invalid|=!_kortestz_mask64_u8(bad,bad);
        __m512i bias=_mm512_set1_epi8(65);
        bias=_mm512_mask_mov_epi8(bias,(__mmask64)lower,_mm512_set1_epi8(71));
        bias=_mm512_mask_mov_epi8(bias,(__mmask64)digit,_mm512_set1_epi8(-4));
        bias=_mm512_mask_mov_epi8(bias,(__mmask64)minus,_mm512_set1_epi8(-17));
        bias=_mm512_mask_mov_epi8(bias,(__mmask64)under,_mm512_set1_epi8(32));
        /* Clear inactive sextets before the dot product. */
        __m512i values=_mm512_maskz_sub_epi8((__mmask64)active,c,bias);
        if ((width&3u)) {
            /* Check unused low bits in the last active sextet. */
            __mmask64 last=_kxor_mask64((__mmask64)active,
                                       _kshiftri_mask64((__mmask64)active,1));
            __m512i low=_mm512_set1_epi8((char)((width&3u)==2?15:3));
            __mmask64 padding=_mm512_mask_test_epi8_mask(last,values,low);
            invalid|=!_kortestz_mask64_u8(padding,padding);
        }
        __m512i pairs=_mm512_maddubs_epi16(values,_mm512_set1_epi32(0x01400140));
        __m512i triples=_mm512_madd_epi16(pairs,_mm512_set1_epi32(0x00011000));
        __m512i order=_mm512_broadcast_i32x4(_mm_setr_epi8(2,1,0,6,5,4,10,9,8,14,13,12,-1,-1,-1,-1));
        __m512i packed=_mm512_shuffle_epi8(triples,order);
        packed=_mm512_permutexvar_epi32(_mm512_setr_epi32(0,1,2,4,5,6,8,9,10,12,13,14,0,0,0,0),packed);
        size_t produced=(width>>2)*3+(((width&3u)*3)>>2);
        _mm512_mask_storeu_epi8(out+n,(__mmask64)((UINT64_C(1)<<produced)-1),packed);
        i+=width; n+=produced;
    }
    *length=n; return !invalid;
}

__attribute__((target("avx512f,avx512bw,avx512vl")))
static int base64_avx512_wide_tail(Slice s, unsigned char *out, size_t cap, size_t *length) {
    if(!s.n){*length=0;return 1;}
    if ((s.n&3u)==1 || (s.n>>2)*3+(((s.n&3u)*3)>>2)>cap) return 0;
    size_t i=0,n=0; uint64_t invalid=0;
    for (;s.n-i>=64;i+=64,n+=48) {
        __m512i c=_mm512_loadu_si512(s.p+i);
        uint64_t upper=_mm512_cmple_epu8_mask(_mm512_sub_epi8(c,_mm512_set1_epi8('A')),_mm512_set1_epi8(25));
        uint64_t lower=_mm512_cmple_epu8_mask(_mm512_sub_epi8(c,_mm512_set1_epi8('a')),_mm512_set1_epi8(25));
        uint64_t digit=_mm512_cmple_epu8_mask(_mm512_sub_epi8(c,_mm512_set1_epi8('0')),_mm512_set1_epi8(9));
        uint64_t minus=_mm512_cmpeq_epi8_mask(c,_mm512_set1_epi8('-')), under=_mm512_cmpeq_epi8_mask(c,_mm512_set1_epi8('_'));
        __mmask64 classes=_kor_mask64(_kor_mask64((__mmask64)upper,(__mmask64)lower),
            _kor_mask64((__mmask64)digit,_kor_mask64((__mmask64)minus,(__mmask64)under)));
        invalid|=(uint64_t)_knot_mask64(classes);
        __m512i bias=_mm512_set1_epi8(65);
        bias=_mm512_mask_mov_epi8(bias,lower,_mm512_set1_epi8(71));
        bias=_mm512_mask_mov_epi8(bias,digit,_mm512_set1_epi8(-4));
        bias=_mm512_mask_mov_epi8(bias,minus,_mm512_set1_epi8(-17));
        bias=_mm512_mask_mov_epi8(bias,under,_mm512_set1_epi8(32));
        __m512i values=_mm512_sub_epi8(c,bias);
        __m512i pairs=_mm512_maddubs_epi16(values,_mm512_set1_epi32(0x01400140));
        __m512i triples=_mm512_madd_epi16(pairs,_mm512_set1_epi32(0x00011000));
        __m512i order=_mm512_broadcast_i32x4(_mm_setr_epi8(2,1,0,6,5,4,10,9,8,14,13,12,-1,-1,-1,-1));
        __m512i packed=_mm512_shuffle_epi8(triples,order);
        packed=_mm512_permutexvar_epi32(_mm512_setr_epi32(0,1,2,4,5,6,8,9,10,12,13,14,0,0,0,0),packed);
        _mm512_mask_storeu_epi8(out+n,0x0000ffffffffffffULL,packed);
    }
    size_t tail=0;
    int ok=base64_tail_wide((Slice){s.p+i,s.n-i},out+n,cap-n,&tail);
    *length=n+tail; return ok && !invalid;
}

#endif

const void *ASC_MODULE_ENTRY(void) {
    unsigned tier=MATH_FORCE==1?0:MATH_FORCE==2?1:2;
    const char *selected=getenv("ASC_SIMD");
    if(selected) {
        if(!strcmp(selected,"auto"))tier=2;
        else if(!strcmp(selected,"scalar"))tier=0;
        else if(!strcmp(selected,"avx2"))tier=1;
        else if(!strcmp(selected,"avx512"))tier=2;
        else {fprintf(stderr,"invalid ASC_SIMD: expected auto, scalar, avx2 or avx512\n");return NULL;}
    }
    static MathAPI api;
    api=(MathAPI){{ASC_ABI,sizeof(MathAPI),"scalar/table-base64"},special_scalar,header_scalar,decimal_scalar,route,base64_scalar};
#if ASC_X86
    __builtin_cpu_init();
    if(tier>=1 && __builtin_cpu_supports("avx2") && __builtin_cpu_supports("ssse3") && __builtin_cpu_supports("sse4.1")) {
        api.h.name="avx2/table-tail";api.special=special_avx2;api.header_end=header_avx2;
        api.decimal=decimal_sse;api.base64url=base64_avx2;
    }
    if(tier>=2 && __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw") &&
       __builtin_cpu_supports("avx512vl") && __builtin_cpu_supports("ssse3") && __builtin_cpu_supports("sse4.1")) {
        api.h.name="avx512/one-load-header+masked-tail";api.special=special_avx512;api.header_end=header_avx512;
        api.decimal=decimal_masked;api.base64url=base64_avx512_wide_tail;
    }
#else
    (void)tier;
#endif
    return &api;
}

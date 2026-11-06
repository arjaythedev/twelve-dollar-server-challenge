#ifndef ASC_RECORD_H
#define ASC_RECORD_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#if !defined(ASC_PORTABLE) && (defined(__x86_64__) || defined(__i386__))
#include <immintrin.h>
#endif

/* Unsupported layouts fall back to SQL before emission. */
typedef struct { uint32_t type, offset, length; } AscField;
static inline __attribute__((always_inline)) int asc_record_scalar(const unsigned char *p,size_t n,unsigned fields,AscField out[4]) {
    if (!n || !fields || fields>4 || p[0]>n || p[0]<fields+1 || p[0]>=128) return 0;
    size_t header=p[0], position=1, offset=header;
    static const unsigned char widths[12]={0,1,2,3,4,6,8,8,0,0,0,0};
    for (unsigned i=0;i<fields;i++) {
        if (position>=header) return 0;
        uint32_t type=p[position++];
        if (type&128u) {
            if (position>=header || (p[position]&128u)) return 0;
            type=((type&127u)<<7)|p[position++];
        }
        if (type==10 || type==11) return 0;
        size_t length=type<12?widths[type]:((type-12)>>1);
        if (length>n-offset) return 0;
        out[i]=(AscField){type,(uint32_t)offset,(uint32_t)length};
        offset+=length;
    }
    return position==header && offset==n;
}
#if !defined(ASC_PORTABLE) && (defined(__x86_64__) || defined(__i386__))
__attribute__((target("ssse3,sse4.1")))
static int asc_record_vector(const unsigned char *p,size_t n,unsigned fields,AscField out[4]) {
    if (n<17 || !fields || fields>4 || p[0]>9 || p[0]<fields+1) return asc_record_scalar(p,n,fields,out);
    unsigned header=p[0], bits=(1u<<(header-1))-1u;
    __m128i bytes=_mm_loadu_si128((const __m128i *)(p+1));
    unsigned ends=(~(unsigned)_mm_movemask_epi8(bytes))&bits;
    if ((unsigned)__builtin_popcount(ends)!=fields) return 0;
    unsigned char selectors[16]; memset(selectors,128,sizeof(selectors));
    unsigned start=0;
    for (unsigned i=0;i<fields;i++) {
        unsigned end=(unsigned)__builtin_ctz(ends), length=end-start+1;
        if (length>2) return 0;
        selectors[i<<1]=(unsigned char)(length==2?start:128);
        selectors[(i<<1)+1]=(unsigned char)end;
        start=end+1; ends&=ends-1;
    }
    if (start!=header-1) return 0;
    __m128i digits=_mm_and_si128(_mm_shuffle_epi8(bytes,_mm_loadu_si128((const __m128i *)selectors)),_mm_set1_epi8(127));
    /* Decode four pairs of base-128 digits. */
    __m128i types=_mm_maddubs_epi16(_mm_set1_epi16(0x0180),digits);
    __m128i widths=_mm_setr_epi8(0,1,2,3,4,6,8,8,0,0,0,0,0,0,0,0);
    __m128i small=_mm_shuffle_epi8(widths,_mm_or_si128(types,_mm_set1_epi16((short)0x8000)));
    __m128i large=_mm_srli_epi16(_mm_sub_epi16(types,_mm_set1_epi16(12)),1);
    __m128i sizes=_mm_blendv_epi8(small,large,_mm_cmpgt_epi16(types,_mm_set1_epi16(11)));
    /* Prefix-sum field lengths into payload offsets. */
    __m128i sums=_mm_add_epi16(sizes,_mm_slli_si128(sizes,2));
    sums=_mm_add_epi16(sums,_mm_slli_si128(sums,4));
    uint16_t t[8], lengths[8], offsets[8];
    _mm_storeu_si128((__m128i *)t,types);
    _mm_storeu_si128((__m128i *)lengths,sizes);
    _mm_storeu_si128((__m128i *)offsets,sums);
    if ((size_t)header+offsets[fields-1]!=n) return 0;
    for (unsigned i=0;i<fields;i++) {
        if (t[i]==10 || t[i]==11) return 0;
        out[i]=(AscField){t[i],header+(i?offsets[i-1]:0u),lengths[i]};
    }
    return 1;
}
#endif
static inline int asc_record_integer(const unsigned char *p,AscField field,int64_t *value) {
    if (field.type==8 || field.type==9) { *value=(int64_t)(field.type-8); return 1; }
    if (!field.type || field.type>6) return 0;
    unsigned n=field.length;
    const unsigned char *bytes=p+field.offset;
    uint64_t v;
    uint32_t word;
    uint16_t half;
    switch (n) {
        case 1: v=bytes[0]; break;
        case 2:
            memcpy(&half,bytes,2);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
            half=__builtin_bswap16(half);
#endif
            v=half; break;
        case 3:
            memcpy(&half,bytes,2);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
            half=__builtin_bswap16(half);
#endif
            v=((uint64_t)half<<8)|bytes[2]; break;
        case 4:
            memcpy(&word,bytes,4);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
            word=__builtin_bswap32(word);
#endif
            v=word; break;
        case 6:
            memcpy(&word,bytes,4); memcpy(&half,bytes+4,2);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
            word=__builtin_bswap32(word); half=__builtin_bswap16(half);
#endif
            v=((uint64_t)word<<16)|half; break;
        case 8:
            memcpy(&v,bytes,8);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
            v=__builtin_bswap64(v);
#endif
            memcpy(value,&v,sizeof(v)); return 1;
        default: return 0;
    }
    uint64_t sign=UINT64_C(1)<<((n<<3)-1);
    v=(v^sign)-sign;
    memcpy(value,&v,sizeof(v));
    return 1;
}
/* Fast path for a two-integer index record. */
static int asc_record_integer_pair(const unsigned char *p,size_t n,int64_t *value) {
    if (n<3 || p[0]!=3) return 0;
    unsigned a=p[1],b=p[2];
    if (!a || a==7 || a>9 || !b || b==7 || b>9) return 0;
    static const unsigned char widths[10]={0,1,2,3,4,6,8,0,0,0};
    unsigned first=widths[a],second=widths[b];
    if (n!=(size_t)(3+first+second)) return 0;
    AscField field={a,3,first};
    return asc_record_integer(p,field,value);
}

#endif

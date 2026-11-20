#include "abi.h"
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <openssl/crypto.h>
#include <limits.h>

#if ASC_X86 && defined(__SSE2__)
#include <emmintrin.h>
/* OR byte differences to avoid cancellation. */
static __attribute__((noinline)) int signature_equal32(const unsigned char *a,const unsigned char *b) {
    __m128i x=_mm_xor_si128(_mm_loadu_si128((const __m128i *)a),_mm_loadu_si128((const __m128i *)b));
    __m128i y=_mm_xor_si128(_mm_loadu_si128((const __m128i *)(a+16)),_mm_loadu_si128((const __m128i *)(b+16)));
    __m128i equal_bytes=_mm_cmpeq_epi8(_mm_or_si128(x,y),_mm_setzero_si128());
    return (unsigned)_mm_movemask_epi8(equal_bytes)==65535u;
}
#else
static int signature_equal32(const unsigned char *a,const unsigned char *b) {
    return !CRYPTO_memcmp(a,b,32);
}
#endif

typedef struct { SHA256_CTX inner,outer; } Auth;
static void *open_auth(const char *secret) {
    size_t n=strlen(secret);if(n>INT_MAX)return NULL;
    Auth *a=calloc(1,sizeof(*a));if(!a)return NULL;
    unsigned char key[64]={0},ipad[64],opad[64];
    if(n>64) {if(!SHA256((const unsigned char *)secret,n,key)){free(a);return NULL;}}
    else memcpy(key,secret,n);
    for(unsigned i=0;i<64;i++){ipad[i]=key[i]^0x36;opad[i]=key[i]^0x5c;}
    int ok=SHA256_Init(&a->inner)&&SHA256_Update(&a->inner,ipad,64)&&
           SHA256_Init(&a->outer)&&SHA256_Update(&a->outer,opad,64);
    OPENSSL_cleanse(key,sizeof(key));OPENSSL_cleanse(ipad,sizeof(ipad));OPENSSL_cleanse(opad,sizeof(opad));
    if(!ok){free(a);return NULL;}return a;
}
static void close_auth(void *state) {
    if(state){OPENSSL_cleanse(state,sizeof(Auth));free(state);}
}
static unsigned verify(void *state, Slice header, const CodecAPI *codec, const MathAPI *math, User *user) {
    if (header.n<7 || memcmp(header.p,"Bearer ",7)) return AUTH_MISSING;
    Slice token={header.p+7,header.n-7};
    /* An HS256 signature has 43 unpadded Base64url characters. */
    if (token.n<44) return AUTH_INVALID;
    const char *two=token.p+token.n-44;
    if (*two!='.') return AUTH_INVALID;
    const char *one=memchr(token.p,'.',(size_t)(two-token.p));
    if (!one) return AUTH_INVALID;
    Slice h={token.p,(size_t)(one-token.p)}, p={one+1,(size_t)(two-one-1)};
    Slice signature={two+1,token.n-(size_t)(two+1-token.p)};
    unsigned char decoded_header[2048], payload[ASC_HEADER_MAX], sig[32], digest[32];
    size_t hn=0,pn=0,sn=0;
    if (!math->base64url(h,decoded_header,sizeof(decoded_header),&hn) ||
        !math->base64url(p,payload,sizeof(payload),&pn) ||
        !math->base64url(signature,sig,sizeof(sig),&sn) || sn!=32 ||
        !codec->jwt_header((Slice){(char *)decoded_header,hn})) return AUTH_INVALID;
    Auth *a=state;
    /* Copy the precomputed inner and outer HMAC states. */
    SHA256_CTX inner=a->inner,outer=a->outer;unsigned char intermediate[32];
    if(!SHA256_Update(&inner,token.p,(size_t)(two-token.p)) ||
       !SHA256_Final(intermediate,&inner) || !SHA256_Update(&outer,intermediate,32) ||
       !SHA256_Final(digest,&outer) || !signature_equal32(sig,digest))return AUTH_INVALID;
    Claims claims;
    if (!codec->claims((Slice){(char *)payload,pn},&claims) || !claims.exp_valid || claims.exp<=(double)time(NULL)) return AUTH_INVALID;
    if (!claims.payload_valid || !math->decimal((Slice){claims.sub,claims.sub_n},&user->id) || user->id<=0) return AUTH_PAYLOAD;
    user->username_n=claims.username_n;
    memcpy(user->username,claims.username,claims.username_n);
    user->username[claims.username_n]=0;
    return AUTH_OK;
}
const void *ASC_MODULE_ENTRY(void) {
    static const AuthAPI api={{ASC_ABI,sizeof(AuthAPI),"openssl-hs256/fixed-signature-boundary"},open_auth,close_auth,verify};
    return &api;
}

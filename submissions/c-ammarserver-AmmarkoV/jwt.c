/* HS256 bearer token verification. See jwt.h . */
#include <string.h>
#include <stdlib.h>

#include <openssl/hmac.h>

#include "jwt.h"

/* --- HS256 with the key's schedule prepared once -----------------------------------------------------
 * HMAC pads the key out to a 64 byte block and hashes ipad||message , then opad||inner . The first block
 * of each of those two hashes depends on the key alone, so it is the same block for every token this
 * server will ever see. It is hashed here the first time a secret turns up and the two SHA-256 states are
 * copied per token after that, which is the whole of the saving : OpenSSL's one-shot HMAC() re-derives
 * them, and on OpenSSL 3 also fetches the MAC out of the provider, every single call.
 * Nothing about a *token* is remembered - every signature is still computed over the bytes that arrived
 * and compared in full ( rule 5 ). This is the key's schedule, the same kind of thing a prepared
 * statement caches about a query.
 * The handler runs on one thread ( see epollAPIServer.h ), so these need no locking. */
static EVP_MD_CTX * hmacAfterIpad = 0;
static EVP_MD_CTX * hmacAfterOpad = 0;
static EVP_MD_CTX * hmacWork      = 0;
static unsigned char * hmacSecret = 0;
static unsigned int    hmacSecretLength = 0;

/* @retval 1=the two states above are ready for this secret , 0=OpenSSL would not play along */
static int HmacPrepare(const char * secret,unsigned int secretLength)
{
  if ( (hmacSecret!=0) && (secretLength==hmacSecretLength) && (memcmp(hmacSecret,secret,secretLength)==0) ) { return 1; }

  unsigned char block[64]; memset(block,0,sizeof(block));
  if (secretLength>sizeof(block))
   {
     unsigned int digestLength=0;
     if (!EVP_Digest(secret,secretLength,block,&digestLength,EVP_sha256(),0)) { return 0; }
   }
  else { memcpy(block,secret,secretLength); }

  unsigned char ipad[64],opad[64];
  unsigned int i=0;
  for (i=0; i<sizeof(block); i++) { ipad[i]=block[i]^0x36; opad[i]=block[i]^0x5C; }

  if (hmacAfterIpad==0) { hmacAfterIpad=EVP_MD_CTX_new(); }
  if (hmacAfterOpad==0) { hmacAfterOpad=EVP_MD_CTX_new(); }
  if (hmacWork==0)      { hmacWork     =EVP_MD_CTX_new(); }
  if ( (hmacAfterIpad==0) || (hmacAfterOpad==0) || (hmacWork==0) ) { return 0; }

  if (!EVP_DigestInit_ex(hmacAfterIpad,EVP_sha256(),0)) { return 0; }
  if (!EVP_DigestUpdate(hmacAfterIpad,ipad,sizeof(ipad))) { return 0; }
  if (!EVP_DigestInit_ex(hmacAfterOpad,EVP_sha256(),0)) { return 0; }
  if (!EVP_DigestUpdate(hmacAfterOpad,opad,sizeof(opad))) { return 0; }

  unsigned char * kept = (unsigned char *) realloc(hmacSecret,(secretLength!=0)?secretLength:1);
  if (kept==0) { return 0; }
  memcpy(kept,secret,secretLength);
  hmacSecret = kept;
  hmacSecretLength = secretLength;
  return 1;
}

/* @retval 1=signature written to out , 0=failed */
static int HmacSha256(const char * secret,unsigned int secretLength,const unsigned char * message,unsigned int messageLength,unsigned char out[32])
{
  if (!HmacPrepare(secret,secretLength)) { return 0; }

  unsigned char inner[32];
  unsigned int length=0;

  if (!EVP_MD_CTX_copy_ex(hmacWork,hmacAfterIpad)) { return 0; }
  if (!EVP_DigestUpdate(hmacWork,message,messageLength)) { return 0; }
  if (!EVP_DigestFinal_ex(hmacWork,inner,&length)) { return 0; }

  if (!EVP_MD_CTX_copy_ex(hmacWork,hmacAfterOpad)) { return 0; }
  if (!EVP_DigestUpdate(hmacWork,inner,sizeof(inner))) { return 0; }
  if (!EVP_DigestFinal_ex(hmacWork,out,&length)) { return 0; }

  return (length==32);
}

/* base64url alphabet as a reverse lookup, 0xFF = not a base64url character. Built once on first use ; the
 * API handler this is called from runs on a single thread ( see epollAPIServer.h ), so there is no race to
 * protect against here. */
static unsigned char base64UrlReverse[256] = { [0 ... 255] = 0xFF };
static int base64UrlReverseReady = 0;

static void Base64UrlInit(void)
{
  static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  unsigned int i=0;
  for (i=0; i<64; i++) { base64UrlReverse[(unsigned char) alphabet[i]] = (unsigned char) i; }
  base64UrlReverseReady=1;
}

/* Decode base64url ( no padding ) into out. Returns the number of bytes written, or -1 if the input is not
 * valid base64url or does not fit. */
static int Base64UrlDecode(const char * in,unsigned int inLength,unsigned char * out,unsigned int outSize)
{
  if (!base64UrlReverseReady) { Base64UrlInit(); }

  unsigned int outUsed=0;
  unsigned int accumulator=0;
  unsigned int bitsHeld=0;
  unsigned int i=0;

  for (i=0; i<inLength; i++)
   {
     unsigned char value = base64UrlReverse[(unsigned char) in[i]];
     if (value==0xFF) { return -1; }

     accumulator = (accumulator<<6) | value;
     bitsHeld += 6;

     if (bitsHeld>=8)
      {
        bitsHeld -= 8;
        if (outUsed>=outSize) { return -1; }
        out[outUsed++] = (unsigned char)( (accumulator>>bitsHeld) & 0xFF );
      }
   }

  /* A base64url group of 6,12,18.. bits can only be a truncated encoding, and whatever bits are left over
   * at the end must be zero padding - anything else is a malformed token rather than a decodable one. */
  if (bitsHeld>=6) { return -1; }
  if ( (accumulator & ((1u<<bitsHeld)-1)) != 0 ) { return -1; }

  return (int) outUsed;
}

/* --- just enough JSON to read a flat token header/payload ------------------------------------------- */

/* Find "key" at the top level of a flat JSON object and hand back the raw value: where it starts, how long
 * it is, and whether it was written as a string. Returns 0 when the key is not there.
 * A token header/payload is a flat object of strings and numbers, so this does not need to walk nested
 * values - and a token whose payload is not flat fails the claim checks below anyway. */
static int JsonFindFlatValue(const char * doc,unsigned int docLength,const char * key,const char ** valueStart,unsigned int * valueLength,int * isString)
{
  unsigned int keyLength = strlen(key);
  unsigned int i=0;

  while (i<docLength)
   {
     /* walk to the next '"' that starts a key: one that follows '{' or ',' */
     if ( (doc[i]!='{') && (doc[i]!=',') ) { ++i; continue; }
     ++i;
     while ( (i<docLength) && ((doc[i]==' ')||(doc[i]=='\t')||(doc[i]=='\n')||(doc[i]=='\r')) ) { ++i; }
     if ( (i>=docLength) || (doc[i]!='"') ) { continue; }
     ++i;

     unsigned int nameStart=i;
     while ( (i<docLength) && (doc[i]!='"') ) { if (doc[i]=='\\') { ++i; } ++i; }
     if (i>=docLength) { return 0; }
     unsigned int nameLength = i-nameStart;
     ++i;

     while ( (i<docLength) && ((doc[i]==' ')||(doc[i]=='\t')||(doc[i]=='\n')||(doc[i]=='\r')) ) { ++i; }
     if ( (i>=docLength) || (doc[i]!=':') ) { continue; }
     ++i;
     while ( (i<docLength) && ((doc[i]==' ')||(doc[i]=='\t')||(doc[i]=='\n')||(doc[i]=='\r')) ) { ++i; }
     if (i>=docLength) { return 0; }

     int thisIsString = (doc[i]=='"');
     unsigned int thisValueStart=0;
     unsigned int thisValueLength=0;

     if (thisIsString)
      {
        ++i;
        thisValueStart=i;
        while ( (i<docLength) && (doc[i]!='"') ) { if (doc[i]=='\\') { ++i; } ++i; }
        if (i>=docLength) { return 0; }
        thisValueLength = i-thisValueStart;
        ++i;
      } else
      {
        thisValueStart=i;
        while ( (i<docLength) && (doc[i]!=',') && (doc[i]!='}') ) { ++i; }
        thisValueLength = i-thisValueStart;
        while ( (thisValueLength>0) && ((doc[thisValueStart+thisValueLength-1]==' ')||(doc[thisValueStart+thisValueLength-1]=='\n')||(doc[thisValueStart+thisValueLength-1]=='\r')||(doc[thisValueStart+thisValueLength-1]=='\t')) ) { --thisValueLength; }
      }

     if ( (nameLength==keyLength) && (memcmp(doc+nameStart,key,keyLength)==0) )
      {
        *valueStart = doc+thisValueStart;
        *valueLength = thisValueLength;
        *isString = thisIsString;
        return 1;
      }

     --i; /* step back onto the ',' or '}' that ended this pair, so the outer loop sees it */
   }

  return 0;
}

/* A whole-number JSON value, for "exp". Returns 0 if it is not one. */
static int JsonValueToLongLong(const char * value,unsigned int valueLength,long long * out)
{
  if (valueLength==0) { return 0; }

  unsigned int i=0;
  int negative=0;
  if (value[0]=='-') { negative=1; i=1; }
  if (i>=valueLength) { return 0; }

  long long result=0;
  for (; i<valueLength; i++)
   {
     if ( (value[i]<'0') || (value[i]>'9') ) { return 0; }
     if (result > 922337203685477580LL) { return 0; } /* would overflow on the next digit */
     result = (result*10) + (value[i]-'0');
   }

  *out = negative ? -result : result;
  return 1;
}

enum JwtResult JwtVerifyToken(
                                const char * token,
                                unsigned int tokenLength,
                                const char * secret,
                                unsigned int secretLength,
                                long long now,
                                struct JwtClaims * claims,
                                char * scratch,
                                unsigned int scratchSize
                              )
{
  if ( (token==0) || (tokenLength==0) ) { return JWT_INVALID; }

  /* header.payload.signature */
  const char * firstDot = (const char *) memchr(token,'.',tokenLength);
  if (firstDot==0) { return JWT_INVALID; }
  unsigned int afterFirst = (unsigned int)(firstDot-token)+1;
  const char * secondDot = (const char *) memchr(token+afterFirst,'.',tokenLength-afterFirst);
  if (secondDot==0) { return JWT_INVALID; }

  const char * headerB64    = token;
  unsigned int headerB64Length = (unsigned int)(firstDot-token);
  const char * payloadB64   = firstDot+1;
  unsigned int payloadB64Length = (unsigned int)(secondDot-(firstDot+1));
  const char * signatureB64 = secondDot+1;
  unsigned int signatureB64Length = tokenLength-((unsigned int)(secondDot-token)+1);

  if ( (headerB64Length==0) || (payloadB64Length==0) || (signatureB64Length==0) ) { return JWT_INVALID; }
  if (memchr(signatureB64,'.',signatureB64Length)!=0) { return JWT_INVALID; } /* more than three parts */

  /* HS256 only, and the alg is read out of the header rather than assumed - an alg=none token carries a
   * payload that would otherwise sail straight through. */
  char decodedHeader[256];
  int decodedHeaderLength = Base64UrlDecode(headerB64,headerB64Length,(unsigned char*) decodedHeader,sizeof(decodedHeader));
  if (decodedHeaderLength<=0) { return JWT_INVALID; }

  const char * algValue=0; unsigned int algLength=0; int algIsString=0;
  if (!JsonFindFlatValue(decodedHeader,(unsigned int) decodedHeaderLength,"alg",&algValue,&algLength,&algIsString)) { return JWT_INVALID; }
  if ( (!algIsString) || (algLength!=5) || (memcmp(algValue,"HS256",5)!=0) ) { return JWT_INVALID; }

  /* The signature covers "header.payload" exactly as it arrived on the wire, base64url and all. */
  unsigned char expectedSignature[32];
  int expectedSignatureLength = Base64UrlDecode(signatureB64,signatureB64Length,expectedSignature,sizeof(expectedSignature));
  if (expectedSignatureLength!=32) { return JWT_INVALID; }

  unsigned char computedSignature[32];
  unsigned int signedLength = headerB64Length+1+payloadB64Length;
  if (!HmacSha256(secret,secretLength,(const unsigned char*) headerB64,signedLength,computedSignature)) { return JWT_INVALID; }

  if (CRYPTO_memcmp(computedSignature,expectedSignature,32)!=0) { return JWT_INVALID; }

  /* Signature is good, so the claims can be trusted to be the ones that were signed. */
  int decodedPayloadLength = Base64UrlDecode(payloadB64,payloadB64Length,(unsigned char*) scratch,scratchSize);
  if (decodedPayloadLength<=0) { return JWT_INVALID; }

  const char * expValue=0; unsigned int expLength=0; int expIsString=0;
  long long expiry=0;
  if (!JsonFindFlatValue(scratch,(unsigned int) decodedPayloadLength,"exp",&expValue,&expLength,&expIsString)) { return JWT_INVALID; }
  if (expIsString) { return JWT_INVALID; }
  if (!JsonValueToLongLong(expValue,expLength,&expiry)) { return JWT_INVALID; }
  if (expiry<=now) { return JWT_INVALID; }

  /* Everything from here on is "the token is genuine but its payload is not usable", which the spec
   * distinguishes from the failures above with its own message. */
  const char * subValue=0; unsigned int subLength=0; int subIsString=0;
  if (!JsonFindFlatValue(scratch,(unsigned int) decodedPayloadLength,"sub",&subValue,&subLength,&subIsString)) { return JWT_BAD_PAYLOAD; }
  if ( (!subIsString) || (subLength==0) || (subLength>18) ) { return JWT_BAD_PAYLOAD; }

  long long userId=0;
  unsigned int i=0;
  for (i=0; i<subLength; i++)
   {
     if ( (subValue[i]<'0') || (subValue[i]>'9') ) { return JWT_BAD_PAYLOAD; }
     userId = (userId*10) + (subValue[i]-'0');
   }
  if (userId<=0) { return JWT_BAD_PAYLOAD; }

  const char * usernameValue=0; unsigned int usernameLength=0; int usernameIsString=0;
  if (!JsonFindFlatValue(scratch,(unsigned int) decodedPayloadLength,"username",&usernameValue,&usernameLength,&usernameIsString)) { return JWT_BAD_PAYLOAD; }
  if (!usernameIsString) { return JWT_BAD_PAYLOAD; }

  claims->userId = userId;
  /* Deliberately the raw bytes from inside the token's JSON string, escapes and all : the API writes them
   * straight back out into its own JSON string, where exactly the same escaping rules apply, so copying
   * them verbatim round-trips the username while re-escaping them would double every backslash. */
  claims->username = usernameValue;
  claims->usernameLength = usernameLength;

  return JWT_OK;
}

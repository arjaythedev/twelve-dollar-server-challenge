/* The B-tree readers for GET /feed and GET /posts/:id. They run SQLite's own cursors inside the
 * request connection's open SQL transaction and write the same bytes as store.rs's SQL path, or
 * answer TWELVE_FALLBACK so that path runs instead. Compiled only inside src/sqlite_unit.c, after
 * the amalgamation, whose static internals (Btree, KeyInfo, UnpackedRecord) they use.
 *
 * Why C: the readers reach SQLite's B-tree internals inside the amalgamation, which only C text compiled
 * after it can see. External entry points: twelve_readers_open, twelve_feed, twelve_post, twelve_readers_end,
 * twelve_readers_close and twelve_reader_reason; every other name is static. Exception to building
 * alone: the file compiles only inside src/sqlite_unit.c after the amalgamation, whose static internals
 * it uses, so it includes nothing for them.
 * Owns: the one object readers, holding the root pages, the two key descriptors, the probe, the four
 * cursor areas, the set of open cursors and the last call's reason.
 * Threading: single owner thread, the loop thread that owns store.rs's request connection;
 * SQLITE_THREADSAFE=0.
 * Between calls: ready is 0 or 1, the cookie stays as twelve_readers_open read it, a cursor opened by a
 * twelve_feed or twelve_post call that answered TWELVE_OK, TWELVE_NOT_FOUND or TWELVE_SPACE stays open, inside
 * the caller's transaction, until twelve_readers_end, which the caller runs before it writes in, commits or
 * rolls back that transaction (SQL reads beside the open cursors are allowed); a TWELVE_FALLBACK answer leaves
 * no cursor open, and every twelve_feed or twelve_post call sets the reason. */

/* build.rs passes -DTWELVE_SAFE_BUILD=1 in the safe build, which follows every rule an unsafe path of the
 * fast build skips; code reads the switch as a constant condition, so both forms compile in every build. */
#ifndef TWELVE_SAFE_BUILD
#define TWELVE_SAFE_BUILD 0
#endif

/* A check kept out of the fast build for its cost: compiled and type-checked in both builds, active in the
 * safe build only. */
#define assert_hot(condition) do { if (TWELVE_SAFE_BUILD) { assert(condition); } } while (0)

#define TWELVE_OK 0
#define TWELVE_NOT_FOUND 1
#define TWELVE_FALLBACK 2
#define TWELVE_SPACE 3

/* Fall-back reasons, read through twelve_reader_reason after a TWELVE_FALLBACK result. */
#define TWELVE_REASON_NONE 0
#define TWELVE_REASON_TXN 1
#define TWELVE_REASON_COOKIE 2
#define TWELVE_REASON_CURSOR 3
#define TWELVE_REASON_RECORD 4
#define TWELVE_REASON_SPAN 5
#define TWELVE_REASON_MISSING 6

#if defined(__x86_64__)
#include <emmintrin.h>
#endif

/* A caller-owned output area: bytes go to ptr[0..cap); need reports the size a TWELVE_SPACE retry needs. */
struct twelve_buf {
  uint8_t *ptr;
  size_t cap;
  size_t len;
  size_t need;
};

static_assert(sizeof(size_t) == 8, "struct twelve_buf's size_t fields are store.rs's ReaderBuffer usize fields");
static_assert(sizeof(struct twelve_buf) == 32, "struct twelve_buf matches store.rs's ReaderBuffer: 32 bytes");
static_assert(offsetof(struct twelve_buf, ptr) == 0, "store.rs's ReaderBuffer has ptr at offset 0");
static_assert(offsetof(struct twelve_buf, cap) == 8, "store.rs's ReaderBuffer has cap at offset 8");
static_assert(offsetof(struct twelve_buf, len) == 16, "store.rs's ReaderBuffer has len at offset 16");
static_assert(offsetof(struct twelve_buf, need) == 24, "store.rs's ReaderBuffer has need at offset 24");

/* Set once by twelve_readers_open; it never holds a row, an id, a count or output (rule 5). */
struct reader_state {
  sqlite3 *db;
  Btree *bt;
  Pgno posts;
  Pgno users;
  Pgno created_idx;
  Pgno likes_idx;
  int newest_first;
  u32 cookie;
  KeyInfo *created_key;
  KeyInfo *likes_key;
  UnpackedRecord *probe;
  BtCursor *cursor[4];
  /* Bit k set while cursor area k holds an open cursor (between calls, see the file comment). */
  unsigned open;
  int ready;
  /* The latest twelve_feed or twelve_post call's fall-back reason; a diagnostic code, never a row, id,
   * count or output. */
  int reason;
};

/* The process's one reader state, owned by the loop thread that owns store.rs's request connection. */
static struct reader_state readers;

/* A write position into one twelve_buf: a byte is stored only below cap, the position always advances. */
struct twelve_out {
  struct twelve_buf *buf;
  size_t pos;
  int reserved;
};

enum {
  /* The widest unsigned 64-bit decimal. */
  OUT_NUMBER_BYTES = 20,
  /* The bytes of one feed row beside its three texts' own bytes: the separator (1), {"id": (6), ,"body": (8),
   * ,"created_at": (14), ,"author": (10), ,"like_count": (14), the closing brace (1), six quotes (6) and two
   * numbers of OUT_NUMBER_BYTES each (40). */
  FEED_ROW_FIXED_BYTES = 100,
};

/* One decoded record header: the first four fields' serial types and data offsets, and the last field's. */
struct twelve_record {
  const uint8_t *p;
  u32 amt;
  size_t count;
  u64 type[4];
  u64 offset[4];
  u64 last_type;
  u64 last_offset;
};

/* Fragments of one feed post encoded into scratch on the scratch path. */
struct twelve_fragment {
  size_t body;
  size_t body_len;
  size_t created;
  size_t created_len;
  size_t name;
  size_t name_len;
  i64 user;
};

/* A view of bytes: the pointer and its byte count travel together. */
struct twelve_span {
  const uint8_t *bytes;
  size_t len;
};

static struct twelve_span span(const void *bytes, size_t len){
  return (struct twelve_span){.bytes = bytes, .len = len};
}

/* UNSAFE PATH (speed): forced inlining without a measured effect; twelve-dollar-lab RESULT.md's "Speed profile at 310,000" measures out_put at 0.127 and its copy at 0.128 us a request as calls of their own, so the fast build inlines out_put into every writer, where a literal fragment's length is a constant and its copy compiles to plain stores; the bytes written are the same */
#if TWELVE_SAFE_BUILD
#define TWELVE_INLINE
#else
#define TWELVE_INLINE inline __attribute__((always_inline))
#endif

/* One capacity decision covers a whole known output extent. A failed reservation keeps the
 * bounded counting writer, so need remains the exact size, not a pessimistic escape bound. */
static void out_reserve(struct twelve_out *w, size_t need){
  w->reserved = !TWELVE_SAFE_BUILD && w->pos<=w->buf->cap && need<=w->buf->cap - w->pos;
}

/* Reserves one feed row holding `text` text bytes: FEED_ROW_FIXED_BYTES plus six output bytes for each text
 * byte, the most its escapes take. Without that room the row goes through the bounded counting writer. */
static void out_row_reserve(struct twelve_out *w, size_t text){
  size_t bound = 0;
  w->reserved = 0;
  if( !__builtin_mul_overflow(text, (size_t)6, &bound)
      && !__builtin_add_overflow(bound, (size_t)FEED_ROW_FIXED_BYTES, &bound) ){
    out_reserve(w, bound);
  }
}

static TWELVE_INLINE void out_copy(uint8_t *dst, const uint8_t *src, size_t len){
  /* A literal fragment's constant length compiles to plain stores. An empty span may hold a null pointer,
   * which memcpy must not get. */
  if( len!=0 ){
    memcpy(dst, src, len);
  }
}

static TWELVE_INLINE void out_put(struct twelve_out *w, struct twelve_span s){
  size_t pos = w->pos;
  if( w->reserved ){
    /* UNSAFE PATH (speed): CX20 per-fragment bounds omitted; out_reserve proved room for the whole feed or string before its first store, and every fragment is included in that bound */
    out_copy(w->buf->ptr + pos, s.bytes, s.len);
  }else if( s.len!=0 && pos<w->buf->cap ){
    size_t room = w->buf->cap - pos;
    out_copy(w->buf->ptr + pos, s.bytes, s.len<=room ? s.len : room);
  }
  if( TWELVE_SAFE_BUILD ){
    size_t end = 0;
    int overflow = __builtin_add_overflow(pos, s.len, &end);
    assert(!overflow);
    w->pos = end;
  }else{
    /* UNSAFE PATH (speed): CX20 unchecked output offsets; at most 20 SQLite records of at most SQLITE_MAX_LENGTH bytes expand at most sixfold, below SIZE_MAX on this asserted 64-bit target */
    w->pos = pos + s.len;
  }
}

/* Stores the 16 bytes at src to dst and returns 1 when none of them needs escaping (json.rs's 16-byte check).
 * The check reads the loaded bytes, never the bytes just stored: a 16-byte load over fresh narrower stores
 * waits for them to leave the store buffer. */
#if defined(__x86_64__) && !TWELVE_SAFE_BUILD
/* UNSAFE PATH (speed): vector code without a benchmark against the byte loop; twelve-dollar-lab RESULT.md's "SPD4G profile" measures the two-step copy and check it replaces at 0.685 us a request (plain16 0.420, out_copy 0.265), SSE2 is part of every x86-64 CPU, and the escape-scan test proves the bytes equal the byte loop's at every position */
static TWELVE_INLINE int copy16_plain(uint8_t *dst, const uint8_t *src){
  __m128i v; /* written before read: memcpy fills it */
  memcpy(&v, src, 16);
  memcpy(dst, &v, 16);
  __m128i bad = _mm_or_si128(
    _mm_or_si128(_mm_cmpeq_epi8(v, _mm_set1_epi8(34)), _mm_cmpeq_epi8(v, _mm_set1_epi8(92))),
    _mm_cmpeq_epi8(_mm_min_epu8(v, _mm_set1_epi8(31)), v));
  return _mm_movemask_epi8(bad)==0;
}
#else
static int copy16_plain(uint8_t *dst, const uint8_t *src){
  int plain = 1;
  for(size_t i=0; i<16; i++){
    dst[i] = src[i];
    if( src[i]<32 || src[i]=='"' || src[i]=='\\' ){
      plain = 0;
    }
  }
  return plain;
}
#endif

/* S(x): x between quotes with json.rs escape_into's escapes; every other byte is copied unchanged. */
static void out_string(struct twelve_out *w, struct twelve_span s){
  static const uint8_t hex[] = "0123456789abcdef";
  size_t i = 0;
  /* run: the text bytes just before i that were stored unchanged, one output byte each.
   * slow: the bytes from i that the byte loop takes before the 16-byte step is tried again. */
  size_t run = 0;
  size_t slow = 0;
  size_t bound = 0;
  int saved_reservation = w->reserved;
  if( !saved_reservation && !__builtin_mul_overflow(s.len, (size_t)6, &bound)
      && !__builtin_add_overflow(bound, (size_t)2, &bound) ){
    out_reserve(w, bound);
  }
  out_put(w, span("\"", 1));
  while( i<s.len ){
    if( w->reserved && slow==0 ){
      /* The 16-byte step takes the next 16 bytes, or the text's last 16: the back bytes before i that it
       * stores again were stored unchanged (run), so it rewrites them with the same values. A step that meets
       * an escape leaves its bytes to the byte loop, which overwrites the speculative copy before any length
       * is published; the sixfold reservation includes those stores. */
      size_t left = s.len - i;
      size_t back = left>=16 ? 0 : 16 - left;
      if( back<=run ){
        size_t step = 16 - back;
        if( copy16_plain(w->buf->ptr + w->pos - back, s.bytes + i - back) ){
          w->pos += step;
          i += step;
          run += step;
          continue;
        }
        slow = step;
      }
    }
    uint8_t c = s.bytes[i];
    if( c<32 || c=='"' || c=='\\' ){
      switch( c ){
        case '"': out_put(w, span("\\\"", 2)); break;
        case '\\': out_put(w, span("\\\\", 2)); break;
        case '\n': out_put(w, span("\\n", 2)); break;
        case '\r': out_put(w, span("\\r", 2)); break;
        case '\t': out_put(w, span("\\t", 2)); break;
        default: {
          const uint8_t u[6] = {
            [0] = '\\', [1] = 'u', [2] = '0', [3] = '0',
            [4] = hex[(unsigned)c >> 4U], [5] = hex[(unsigned)c & 15U],
          };
          out_put(w, span(u, 6));
        }
      }
      run = 0;
    }
    else{
      out_put(w, span(s.bytes + i, 1));
      run++;
    }
    if( slow!=0 ){
      slow--;
    }
    i++;
  }
  out_put(w, span("\"", 1));
  w->reserved = saved_reservation;
}

/* The decimal of the value as unsigned 64-bit, as Rust's `as u64` prints it. */
static void out_number(struct twelve_out *w, u64 v){
  static const uint8_t decimal_digits[] =
    "0001020304050607080910111213141516171819"
    "2021222324252627282930313233343536373839"
    "4041424344454647484950515253545556575859"
    "6061626364656667686970717273747576777879"
    "8081828384858687888990919293949596979899";
  /* Twice the widest decimal: a reserved writer copies OUT_NUMBER_BYTES from the first digit, whatever the
   * digit count, and the bytes past the digits are these zeros. */
  uint8_t digits[2 * OUT_NUMBER_BYTES] = {0};
  size_t i = OUT_NUMBER_BYTES;
  while( v>=100 ){
    size_t pair = (size_t)(v % 100U) * 2;
    i -= 2;
    digits[i] = decimal_digits[pair];
    digits[i + 1] = decimal_digits[pair + 1];
    v /= 100U;
  }
  if( v>=10 ){
    i -= 2;
    digits[i] = decimal_digits[(size_t)v * 2];
    digits[i + 1] = decimal_digits[(size_t)v * 2 + 1];
  }else{
    i--;
    digits[i] = decimal_digits[(size_t)v * 2 + 1];
  }
  if( w->reserved ){
    /* UNSAFE PATH (speed): CX20 per-fragment bounds omitted; every reservation that spans a number counts OUT_NUMBER_BYTES of room from that number's first digit, so this fixed-length copy stays inside it, and the position advances by the digits alone */
    memcpy(w->buf->ptr + w->pos, digits + i, OUT_NUMBER_BYTES);
    w->pos += OUT_NUMBER_BYTES - i;
  }else{
    out_put(w, span(digits + i, OUT_NUMBER_BYTES - i));
  }
}

/* The helpers below return TWELVE_OK, or the TWELVE_REASON_ code of their first failing check. */

/* Bytes a serial type occupies; 10 and 11 are refused. */
static int serial_size(u64 t, u64 *size){
  static const unsigned char small[10] = {
    [0] = 0, [1] = 1, [2] = 2, [3] = 3, [4] = 4, [5] = 6, [6] = 8, [7] = 8, [8] = 0, [9] = 0,
  };
  if( t<10 ){
    *size = small[t];
    return TWELVE_OK;
  }
  if( t<12 ){
    return TWELVE_REASON_RECORD;
  }
  *size = (t - 12)/2;
  return TWELVE_OK;
}

/* SQLite's varint at p, reading only bytes below bound: bytes 1 to 8 give their 7 low bits while their high
 * bit is set, a 9th byte gives all 8. Returns its length, 1 to 9, or 0 when it needs a byte at or past
 * bound. */
static size_t varint_bounded(const uint8_t *p, u64 bound, u64 *value){
  u64 v = 0;
  size_t i = 0;
  while( i<8 && i<bound ){
    v = (v<<7) | (p[i] & 0x7fU);
    if( (p[i] & 0x80U)==0 ){
      *value = v;
      return i + 1;
    }
    i++;
  }
  if( bound<=8 ){
    return 0;
  }
  *value = (v<<8) | p[8];
  return 9;
}

/* Decodes the header of the cursor's current entry; the payload must be wholly local. sqlite3BtreePayloadSize
 * runs first: it parses the cell (getCellInfo), and sqlite3BtreePayloadFetch reads that parse, which a cursor
 * move resets. */
static int record_read(BtCursor *cur, struct twelve_record *r){
  u32 payload = sqlite3BtreePayloadSize(cur);
  r->p = sqlite3BtreePayloadFetch(cur, &r->amt);
  if( r->amt==0 || r->amt!=payload ){
    return TWELVE_REASON_RECORD;
  }
  u64 hdr = 0;
  u64 at = 0;
  if( TWELVE_SAFE_BUILD ){
    at = varint_bounded(r->p, r->amt, &hdr);
    if( at==0 ){
      return TWELVE_REASON_RECORD;
    }
  }else{
    u32 hdr32 = 0;
    /* getVarint32 handles a one-byte varint itself and calls sqlite3GetVarint32 only for longer ones, which is
     * that function's precondition (util.c). */
    /* UNSAFE PATH (speed): varint reads without a per-read length bound; getVarint32 and sqlite3GetVarint read at most 9 bytes from a start below the header end, inside the cell's page image, and a header that runs past the payload is refused before any byte past it is used */
    at = getVarint32(r->p, hdr32);
    hdr = hdr32;
  }
  if( hdr>r->amt || hdr<at ){
    return TWELVE_REASON_RECORD;
  }
  u64 data = hdr;
  size_t k = 0;
  while( at<hdr ){
    u64 t = 0;
    size_t n = 0;
    if( TWELVE_SAFE_BUILD ){
      n = varint_bounded(r->p + at, hdr - at, &t);
      if( n==0 ){
        return TWELVE_REASON_RECORD;
      }
    }else{
      /* UNSAFE PATH (speed): varint reads without a per-read length bound; getVarint32 and sqlite3GetVarint read at most 9 bytes from a start below the header end, inside the cell's page image, and a header that runs past the payload is refused before any byte past it is used */
      /* A one-byte serial type and a two-byte one, a text of 58 to 8,185 bytes, are decoded here; only longer
       * ones call sqlite3GetVarint: twelve-dollar-lab RESULT.md's "Speed profile at 310,000" measures that call
       * at 0.271 us a request. at < hdr <= amt, so each byte read is inside the payload. */
      if( r->p[at]<0x80U ){
        t = r->p[at];
        n = 1;
      }else if( hdr - at>=2 && r->p[at + 1]<0x80U ){
        t = ((u64)(r->p[at] & 0x7fU) << 7) | r->p[at + 1];
        n = 2;
      }else{
        n = sqlite3GetVarint(r->p + at, &t);
      }
    }
    if( n>hdr - at ){
      return TWELVE_REASON_RECORD;
    }
    at += n;
    u64 size = 0;
    if( serial_size(t, &size)!=TWELVE_OK ){
      return TWELVE_REASON_RECORD;
    }
    if( size>(u64)r->amt - data ){
      return TWELVE_REASON_RECORD;
    }
    if( k<4 ){
      r->type[k] = t;
      r->offset[k] = data;
    }
    r->last_type = t;
    r->last_offset = data;
    data += size;
    k++;
  }
  r->count = k;
  return k>0 ? TWELVE_OK : TWELVE_REASON_RECORD;
}

/* An integer field: serial types 1 to 6 (big-endian two's complement), 8 (0) and 9 (1). */
static int record_int(const struct twelve_record *r, u64 t, u64 offset, i64 *v){
  static const unsigned char width[7] = {[0] = 0, [1] = 1, [2] = 2, [3] = 3, [4] = 4, [5] = 6, [6] = 8};
  const uint8_t *d = r->p + offset;
  u64 x = 0;
  if( t==8 || t==9 ){
    *v = (t==9);
    return TWELVE_OK;
  }
  if( t<1 || t>6 ){
    return TWELVE_REASON_RECORD;
  }
  for(size_t i=0; i<width[t]; i++){
    x = (x<<8) | d[i];
  }
  if( width[t]<8 && (x & ((u64)1<<(8*width[t]-1)))!=0 ){
    x |= ~(u64)0<<(8*width[t]);
  }
  *v = x <= (u64)INT64_MAX ? (i64)x : -(i64)(~x) - 1;
  return TWELVE_OK;
}

/* The first field of the cursor's current entry as an integer: the likes scans' post_id. */
static int record_first_int(BtCursor *cur, struct twelve_record *r, i64 *v){
  if( !TWELVE_SAFE_BUILD ){
    /* UNSAFE PATH (speed): first field only; a likes index entry whose header size and first serial type are one byte each is checked only as far as that first field, the one value the scans read, inside the payload; the rest of SQLite's own index record is not decoded, and twelve-dollar-lab RESULT.md's "Speed profile at 310,000" measures the whole-header decode, record_read, at 0.153 us a request */
    u32 payload = sqlite3BtreePayloadSize(cur);
    r->p = sqlite3BtreePayloadFetch(cur, &r->amt);
    if( r->amt>=2 && r->amt==payload && r->p[0]<0x80U && r->p[1]<0x80U ){
      u64 hdr = r->p[0];
      u64 t = r->p[1];
      u64 size = 0;
      if( hdr<2 || hdr>r->amt || serial_size(t, &size)!=TWELVE_OK || size>(u64)r->amt - hdr ){
        return TWELVE_REASON_RECORD;
      }
      return record_int(r, t, hdr, v);
    }
  }
  int why = record_read(cur, r);
  if( why!=TWELVE_OK ){
    return why;
  }
  return record_int(r, r->type[0], r->offset[0], v);
}

/* A text field: odd serial types of at least 13; the length comes from the header, never a NUL scan. */
static int record_text(const struct twelve_record *r, int k, struct twelve_span *text){
  u64 t = r->type[k];
  if( t<13 || (t&1)==0 ){
    return TWELVE_REASON_RECORD;
  }
  *text = span(r->p + r->offset[k], (t - 13)/2);
  return TWELVE_OK;
}

/* Opens read-only cursor k in its zeroed area, or reuses it when it is open already: cursor k always names
 * one root and key, and every use positions it first (a First, Last, Moveto or seek). */
static int cursor_open(int k, Pgno root, KeyInfo *key){
  if( (readers.open & (1U << k))!=0 ){
    return TWELVE_OK;
  }
  sqlite3BtreeCursorZero(readers.cursor[k]);
  if( sqlite3BtreeCursor(readers.bt, root, 0, key, readers.cursor[k])!=SQLITE_OK ){
    return TWELVE_REASON_CURSOR;
  }
  readers.open |= 1U << k;
  return TWELVE_OK;
}

/* Closes every open cursor. */
static void cursors_close(void){
  for(int k=0; k<4; k++){
    if( (readers.open & (1U << k))!=0 ){
      /* SQLite 3.54.1's sqlite3BtreeCloseCursor returns SQLITE_OK on every path (btree.c). */
      (void)sqlite3BtreeCloseCursor(readers.cursor[k]);
      readers.open &= ~(1U << k);
    }
  }
}

/* Steps a scan; SQLITE_DONE ends it with the cursor at EOF. */
static int scan_next(BtCursor *cur){
  int rc = sqlite3BtreeNext(cur, 0);
  return rc==SQLITE_OK || rc==SQLITE_DONE ? TWELVE_OK : TWELVE_REASON_CURSOR;
}

/* The schema cookie's int value as the u32 that BTREE_SCHEMA_VERSION's meta holds: C's conversion to an
 * unsigned type, modulo 2^32. */
static u32 cookie_bits(int cookie){
  return (u32)cookie;
}

/* The reads join the SQL transaction store.rs opened, under an unchanged schema cookie. */
static int reader_begin(sqlite3 *db){
  u32 cookie = 0;
  if( sqlite3_get_autocommit(db)!=0 ){
    return TWELVE_REASON_TXN;
  }
  if( sqlite3BtreeTxnState(readers.bt)==SQLITE_TXN_NONE ){
    int version = 0;
    if( sqlite3BtreeBeginTrans(readers.bt, 0, &version)!=SQLITE_OK ){
      return TWELVE_REASON_TXN;
    }
    cookie = cookie_bits(version);
  }else{
    sqlite3BtreeGetMeta(readers.bt, BTREE_SCHEMA_VERSION, &cookie);
  }
  return cookie==readers.cookie ? TWELVE_OK : TWELVE_REASON_COOKIE;
}

/* Positions likes cursor 1 on the first likes_post_id_idx entry with post_id >= value (OP_SeekGE). */
static int likes_seek(i64 value){
  UnpackedRecord *probe = readers.probe;
  int res = 0;
  probe->nField = 1;
  probe->default_rc = 1;
  probe->errCode = 0;
  probe->eqSeen = 0;
  sqlite3VdbeMemSetInt64(&probe->aMem[0], value);
  if( sqlite3BtreeIndexMoveto(readers.cursor[1], probe, &res)!=SQLITE_OK ){
    return TWELVE_REASON_CURSOR;
  }
  if( res<0 ){
    return scan_next(readers.cursor[1]);
  }
  return TWELVE_OK;
}

/* Seeks users cursor 3 to the post's author and returns the username field. */
static int author(i64 user, struct twelve_span *name, struct twelve_record *u){
  int res = 0;
  if( sqlite3BtreeTableMoveto(readers.cursor[3], user, 0, &res)!=SQLITE_OK ){
    return TWELVE_REASON_CURSOR;
  }
  if( res!=0 ){
    return TWELVE_REASON_MISSING;
  }
  if( record_read(readers.cursor[3], u)!=TWELVE_OK || u->count<2 ){
    return TWELVE_REASON_RECORD;
  }
  return record_text(u, 1, name);
}

/* Decodes the posts row under cursor 2: user_id, body and created_at. */
static int post_row(struct twelve_record *r, i64 *user, struct twelve_span *body, struct twelve_span *created){
  if( record_read(readers.cursor[2], r)!=TWELVE_OK || r->count<4 ){
    return TWELVE_REASON_RECORD;
  }
  if( record_int(r, r->type[1], r->offset[1], user)!=TWELVE_OK ){
    return TWELVE_REASON_RECORD;
  }
  if( record_text(r, 2, body)!=TWELVE_OK ){
    return TWELVE_REASON_RECORD;
  }
  return record_text(r, 3, created);
}

/* N(v)'s value: C's conversion of the rowid to unsigned 64-bit, modulo 2^64. */
static u64 rowid_bits(i64 id){
  return (u64)id;
}

static void out_post_head(struct twelve_out *o, i64 id){
  out_put(o, span("{\"id\":", 6));
  out_number(o, rowid_bits(id));
  out_put(o, span(",\"body\":", 8));
}

static void out_post_tail(struct twelve_out *o, struct twelve_span name, u64 likes){
  out_put(o, span(",\"author\":", 10));
  out_string(o, name);
  out_put(o, span(",\"like_count\":", 14));
  out_number(o, likes);
  out_put(o, span("}", 1));
}

/* Copies an S() fragment from scratch; past scratch's capacity only the position advances. */
static void out_fragment(struct twelve_out *o, const struct twelve_out *s, size_t start, size_t len){
  if( s->pos<=s->buf->cap ){
    out_put(o, span(s->buf->ptr + start, len));
  }else{
    o->pos += len;
  }
}

/* How a reader call ends: each buffer's length, or TWELVE_SPACE with need set for each too-small buffer. */
static int finish(int result, struct twelve_out *o, struct twelve_out *s){
  int space = o->pos>o->buf->cap || (s && s->pos>s->buf->cap);
  if( space ){
    if( o->pos>o->buf->cap ){
      o->buf->need = o->pos;
    }
    o->buf->len = 0;
    if( s ){
      if( s->pos>s->buf->cap ){
        s->buf->need = s->pos;
      }
      s->buf->len = 0;
    }
    return TWELVE_SPACE;
  }
  o->buf->len = o->pos;
  if( s ){
    s->buf->len = s->pos;
  }
  return result;
}

int twelve_readers_open(sqlite3 *db);
int twelve_feed(sqlite3 *db, struct twelve_buf *out, struct twelve_buf *scratch);
int twelve_post(sqlite3 *db, sqlite3_int64 id, struct twelve_buf *out);
void twelve_readers_end(void);
void twelve_readers_close(void);
int twelve_reader_reason(void);

static void readers_free(sqlite3 *db){
  cursors_close();
  for(int k=0; k<4; k++){
    sqlite3_free(readers.cursor[k]);
    readers.cursor[k] = 0;
  }
  sqlite3DbFree(db, readers.probe);
  readers.probe = 0;
  sqlite3KeyInfoUnref(readers.created_key);
  readers.created_key = 0;
  sqlite3KeyInfoUnref(readers.likes_key);
  readers.likes_key = 0;
}

int twelve_readers_open(sqlite3 *db){
  int saved_errno = errno;
  assert(db != NULL);
  assert(readers.ready == 0);
  Table *posts = sqlite3FindTable(db, "posts", "main");
  Table *users = sqlite3FindTable(db, "users", "main");
  Table *likes = sqlite3FindTable(db, "likes", "main");
  if( !posts || !users || !likes ){
    errno = saved_errno;
    return 1;
  }
  Index *created = sqlite3FindIndex(db, "posts_created_at_id_idx", "main");
  Index *liked = sqlite3FindIndex(db, "likes_post_id_idx", "main");
  if( !created || !liked ){
    errno = saved_errno;
    return 2;
  }
  if( ENC(db)!=SQLITE_UTF8 ){
    errno = saved_errno;
    return 3;
  }
  if( created->pTable!=posts || created->nKeyCol!=2 || created->aiColumn[0]!=3
   || (created->aiColumn[1]!=posts->iPKey && created->aiColumn[1]!=XN_ROWID)
   || created->aSortOrder[0]!=created->aSortOrder[1] ){
    errno = saved_errno;
    return 2;
  }
  if( liked->pTable!=likes || liked->nKeyCol!=1 || liked->aiColumn[0]!=1 ){
    errno = saved_errno;
    return 2;
  }

  readers.created_key = sqlite3KeyInfoAlloc(db, 2, 1);
  readers.likes_key = sqlite3KeyInfoAlloc(db, 1, 1);
  if( readers.likes_key ){
    readers.probe = sqlite3VdbeAllocUnpackedRecord(readers.likes_key);
  }
  for(int k=0; k<4; k++){
    readers.cursor[k] = sqlite3_malloc64((sqlite3_uint64)sqlite3BtreeCursorSize());
  }
  if( !readers.created_key || !readers.likes_key || !readers.probe
   || !readers.cursor[0] || !readers.cursor[1] || !readers.cursor[2] || !readers.cursor[3] ){
    readers_free(db);
    errno = saved_errno;
    return 4;
  }
  readers.created_key->aSortFlags[0] = created->aSortOrder[0];
  readers.created_key->aSortFlags[1] = created->aSortOrder[1];
  readers.created_key->aColl[0] = db->pDfltColl;
  readers.created_key->aColl[1] = db->pDfltColl;
  readers.likes_key->aSortFlags[0] = 0;
  readers.likes_key->aColl[0] = db->pDfltColl;
  /* sqlite3VdbeAllocUnpackedRecord leaves aMem uninitialized. */
  sqlite3VdbeMemInit(&readers.probe->aMem[0], db, MEM_Null);

  readers.db = db;
  readers.bt = db->aDb[0].pBt;
  readers.posts = posts->tnum;
  readers.users = users->tnum;
  readers.created_idx = created->tnum;
  readers.likes_idx = liked->tnum;
  readers.newest_first = created->aSortOrder[0]==SQLITE_SO_DESC;
  readers.cookie = cookie_bits(db->aDb[0].pSchema->schema_cookie);
  readers.ready = 1;
  errno = saved_errno;
  return 0;
}

/* The feed slot of the i-th id, below FEED_POSTS_ITEMS. */
static int8_t slot_of(size_t i){
  assert_hot(i <= INT8_MAX);
  /* UNSAFE PATH (speed): unchecked narrowing cast; i < FEED_POSTS_ITEMS (20) by the loop bound, so the int8_t slot value cannot lose bits */
  return (int8_t)i;
}

int twelve_feed(sqlite3 *db, struct twelve_buf *out, struct twelve_buf *scratch){ // NOLINT(readability-function-size): the feed's five steps share the id, like, slot and fragment arrays and the four cursors, and each step runs once
  enum { FEED_POSTS_ITEMS = 20, SLOT_SPAN_ITEMS = 256 };
  /* UNSAFE PATH (speed): errno not saved and restored; the one caller, store.rs, reads no errno after a reader call: it maps result codes */
  int saved_errno = TWELVE_SAFE_BUILD ? errno : 0;
  /* UNSAFE PATH (speed): precondition asserts only in the safe build; store.rs calls the readers only after a 0 from twelve_readers_open, on that connection, with its own buffers */
  assert_hot(readers.ready == 1);
  assert_hot(db == readers.db);
  assert_hot(out != NULL && (out->ptr != NULL || out->cap == 0));
  assert_hot(scratch != NULL && (scratch->ptr != NULL || scratch->cap == 0));
  struct twelve_out o = {.buf = out, .pos = 0};
  struct twelve_out s = {.buf = scratch, .pos = 0};
  struct twelve_record r; /* written before read: record_read fills it */
  struct twelve_record u; /* written before read: author's record_read fills it */
  struct twelve_fragment frag[FEED_POSTS_ITEMS]; /* written before read: step 5's scratch path fills it */
  i64 ids[FEED_POSTS_ITEMS]; /* written before read: step 1 fills ids[0..n) */
  u64 likes[FEED_POSTS_ITEMS]; /* written before read: step 3 zeroes likes[0..n) */
  int8_t slots[SLOT_SPAN_ITEMS]; /* written before read: step 3 fills it */
  size_t n = 0;
  int why = TWELVE_OK;
  int result = TWELVE_OK;
  i64 min = 0;
  i64 max = 0;

  readers.reason = TWELVE_REASON_NONE;
  out->need = 0;
  scratch->need = 0;
  if( (why = reader_begin(db))!=0 ){
    goto out_close_cursors;
  }

  /* Step 1: up to 20 ids in created_at DESC, id DESC order from posts_created_at_id_idx. */
  {
    BtCursor *c0 = readers.cursor[0];
    int res = 0;
    int rc = SQLITE_OK;
    if( (why = cursor_open(0, readers.created_idx, readers.created_key))!=0 ){
      goto out_close_cursors;
    }
    if( readers.newest_first ){
      rc = sqlite3BtreeFirst(c0, &res);
    }else{
      rc = sqlite3BtreeLast(c0, &res);
    }
    if( rc!=SQLITE_OK ){
      why = TWELVE_REASON_CURSOR;
      goto out_close_cursors;
    }
    if( res==0 ){
      while( n<FEED_POSTS_ITEMS ){
        if( (why = record_read(c0, &r))!=0 ){
          goto out_close_cursors;
        }
        if( (why = record_int(&r, r.last_type, r.last_offset, &ids[n]))!=0 ){
          goto out_close_cursors;
        }
        n++;
        if( n==FEED_POSTS_ITEMS ){
          break;
        }
        if( readers.newest_first ){
          rc = sqlite3BtreeNext(c0, 0);
        }else{
          rc = sqlite3BtreePrevious(c0, 0);
        }
        if( rc==SQLITE_DONE ){
          break;
        }
        if( rc!=SQLITE_OK ){
          why = TWELVE_REASON_CURSOR;
          goto out_close_cursors;
        }
      }
    }
  }

  /* Step 2: an empty feed. */
  if( n==0 ){
    out_put(&o, span("{\"posts\":[]}", 12));
    goto out_finish;
  }

  /* Step 3: the id span and each id's slot. */
  {
    i64 id_span = 0;
    min = ids[0];
    max = ids[0];
    for(size_t i=1; i<n; i++){
      if( ids[i]<min ){
        min = ids[i];
      }
      if( ids[i]>max ){
        max = ids[i];
      }
    }
    if( __builtin_sub_overflow(max, min, &id_span) || id_span>=SLOT_SPAN_ITEMS ){
      why = TWELVE_REASON_SPAN;
      goto out_close_cursors;
    }
    memset(slots, -1, sizeof(slots));
    for(size_t i=0; i<n; i++){
      if( slots[ids[i] - min]>=0 ){
        why = TWELVE_REASON_SPAN;
        goto out_close_cursors;
      }
      slots[ids[i] - min] = slot_of(i);
      likes[i] = 0;
    }
  }

  /* Step 4: one scan of likes_post_id_idx over [min, max]. */
  {
    BtCursor *c1 = readers.cursor[1];
    if( (why = cursor_open(1, readers.likes_idx, readers.likes_key))!=0 ){
      goto out_close_cursors;
    }
    if( (why = likes_seek(min))!=0 ){
      goto out_close_cursors;
    }
    while( !sqlite3BtreeEof(c1) ){
      i64 post = 0;
      if( (why = record_first_int(c1, &r, &post))!=0 ){
        goto out_close_cursors;
      }
      if( post>max ){
        break;
      }
      if( post>=min && slots[post - min]>=0 ){
        likes[slots[post - min]]++;
      }
      if( (why = scan_next(c1))!=0 ){
        goto out_close_cursors;
      }
    }
  }

  /* Step 5: rows and authors, directly when the ids prove a contiguous descending layout. */
  {
    BtCursor *c2 = readers.cursor[2];
    int contiguous = 1;
    int res = 0;
    for(size_t i=1; i<n; i++){
      i64 expected = 0;
      if( __builtin_sub_overflow(ids[0], i, &expected) || ids[i]!=expected ){
        contiguous = 0;
      }
    }
    if( (why = cursor_open(2, readers.posts, 0))!=0 ){
      goto out_close_cursors;
    }
    if( contiguous ){
      if( (why = cursor_open(3, readers.users, 0))!=0 ){
        goto out_close_cursors;
      }
      if( sqlite3BtreeTableMoveto(c2, ids[0], 0, &res)!=SQLITE_OK ){
        why = TWELVE_REASON_CURSOR;
        goto out_close_cursors;
      }
      if( res!=0 ){
        why = TWELVE_REASON_MISSING;
        goto out_close_cursors;
      }
      out_put(&o, span("{\"posts\":[", 10));
      for(size_t i=0; i<n; i++){
        i64 user = 0;
        struct twelve_span body = {0};
        struct twelve_span created = {0};
        struct twelve_span name = {0};
        if( i>0 ){
          int rc = sqlite3BtreePrevious(c2, 0);
          if( rc==SQLITE_DONE ){
            why = TWELVE_REASON_MISSING;
            goto out_close_cursors;
          }
          if( rc!=SQLITE_OK ){
            why = TWELVE_REASON_CURSOR;
            goto out_close_cursors;
          }
          if( sqlite3BtreeIntegerKey(c2)!=ids[i] ){
            why = TWELVE_REASON_MISSING;
            goto out_close_cursors;
          }
        }
        if( (why = post_row(&r, &user, &body, &created))!=0 ){
          goto out_close_cursors;
        }
        if( (why = author(user, &name, &u))!=0 ){
          goto out_close_cursors;
        }
        /* The row goes straight to the output. Its texts stay readable until the next row moves the cursors:
         * the post's lie in cursor 2's page, which stays pinned while cursor 3 seeks, the author's in cursor 3's.
         * Each text is at most a record's u32 size, so the sum cannot wrap on this asserted 64-bit target. */
        out_row_reserve(&o, body.len + created.len + name.len);
        if( i>0 ){
          out_put(&o, span(",", 1));
        }
        out_post_head(&o, ids[i]);
        out_string(&o, body);
        out_put(&o, span(",\"created_at\":", 14));
        out_string(&o, created);
        out_post_tail(&o, name, likes[i]);
        o.reserved = 0;
      }
      out_put(&o, span("]}", 2));
      goto out_finish;
    }else{
      size_t found = 0;
      if( sqlite3BtreeTableMoveto(c2, min, 0, &res)!=SQLITE_OK ){
        why = TWELVE_REASON_CURSOR;
        goto out_close_cursors;
      }
      if( res<0 && (why = scan_next(c2))!=0 ){
        goto out_close_cursors;
      }
      while( !sqlite3BtreeEof(c2) ){
        i64 key = sqlite3BtreeIntegerKey(c2);
        if( key>max ){
          break;
        }
        if( key>=min && slots[key - min]>=0 ){
          struct twelve_fragment *f = &frag[slots[key - min]];
          struct twelve_span body = {0};
          struct twelve_span created = {0};
          if( (why = post_row(&r, &f->user, &body, &created))!=0 ){
            goto out_close_cursors;
          }
          f->body = s.pos;
          out_string(&s, body);
          f->body_len = s.pos - f->body;
          f->created = s.pos;
          out_string(&s, created);
          f->created_len = s.pos - f->created;
          found++;
        }
        if( (why = scan_next(c2))!=0 ){
          goto out_close_cursors;
        }
      }
      if( found!=n ){
        why = TWELVE_REASON_MISSING;
        goto out_close_cursors;
      }
      if( (why = cursor_open(3, readers.users, 0))!=0 ){
        goto out_close_cursors;
      }
      for(size_t i=0; i<n; i++){
        struct twelve_span name = {0};
        if( (why = author(frag[i].user, &name, &u))!=0 ){
          goto out_close_cursors;
        }
        frag[i].name = s.pos;
        out_string(&s, name);
        frag[i].name_len = s.pos - frag[i].name;
      }
    }
  }
  {
    /* The scattered-id path: every text fragment is encoded exactly once in caller-owned scratch, in id order.
     * The feed makes one aggregate reservation, then copies those fragments in feed order without rescanning. */
    size_t need = 12 + n - 1;
    for(size_t i=0; i<n; i++){
      struct twelve_buf count_buf = {.ptr = NULL, .cap = 0, .len = 0, .need = 0};
      struct twelve_out count_out = {.buf = &count_buf, .pos = 0};
      out_number(&count_out, rowid_bits(ids[i]));
      out_number(&count_out, likes[i]);
      size_t row = 6 + 8 + 14 + 10 + 14 + 1 + count_out.pos;
      size_t text_bytes = frag[i].body_len + frag[i].created_len + frag[i].name_len;
      if( __builtin_add_overflow(row, text_bytes, &row)
          || __builtin_add_overflow(need, row, &need) ){
        why = TWELVE_REASON_RECORD;
        goto out_close_cursors;
      }
    }
    {
      /* OUT_NUMBER_BYTES of room past the exact size: out_number's fixed-length copy of the last number. */
      size_t padded = 0;
      if( !__builtin_add_overflow(need, (size_t)OUT_NUMBER_BYTES, &padded) ){
        out_reserve(&o, padded);
      }
    }
    if( s.pos>scratch->cap ){
      o.pos = need;
      goto out_finish;
    }
    out_put(&o, span("{\"posts\":[", 10));
    for(size_t i=0; i<n; i++){
      if( i>0 ){
        out_put(&o, span(",", 1));
      }
      out_post_head(&o, ids[i]);
      out_fragment(&o, &s, frag[i].body, frag[i].body_len);
      out_put(&o, span(",\"created_at\":", 14));
      out_fragment(&o, &s, frag[i].created, frag[i].created_len);
      out_put(&o, span(",\"author\":", 10));
      out_fragment(&o, &s, frag[i].name, frag[i].name_len);
      out_put(&o, span(",\"like_count\":", 14));
      out_number(&o, likes[i]);
      out_put(&o, span("}", 1));
    }
    out_put(&o, span("]}", 2));
  }

  /* The cursors stay open for the caller's next reader call, until twelve_readers_end. */
out_finish:
  result = finish(TWELVE_OK, &o, &s);
  if( TWELVE_SAFE_BUILD ){
    errno = saved_errno;
  }
  return result;

out_close_cursors:
  cursors_close();
  readers.reason = why;
  if( TWELVE_SAFE_BUILD ){
    errno = saved_errno;
  }
  return TWELVE_FALLBACK;
}

int twelve_post(sqlite3 *db, sqlite3_int64 id, struct twelve_buf *out){
  /* UNSAFE PATH (speed): errno not saved and restored; the one caller, store.rs, reads no errno after a reader call: it maps result codes */
  int saved_errno = TWELVE_SAFE_BUILD ? errno : 0;
  /* UNSAFE PATH (speed): precondition asserts only in the safe build; store.rs calls the readers only after a 0 from twelve_readers_open, on that connection, with its own buffers */
  assert_hot(readers.ready == 1);
  assert_hot(db == readers.db);
  assert_hot(out != NULL && (out->ptr != NULL || out->cap == 0));
  assert_hot(id > 0);
  struct twelve_out o = {.buf = out, .pos = 0};
  struct twelve_record r; /* written before read: post_row's record_read fills it */
  struct twelve_record u; /* written before read: author's record_read fills it */
  struct twelve_record l; /* written before read: record_first_int fills it */
  int res = 0;
  int why = TWELVE_OK;
  int result = TWELVE_OK;
  i64 user = 0;
  u64 count = 0;
  struct twelve_span body = {0};
  struct twelve_span created = {0};
  struct twelve_span name = {0};
  BtCursor *c1 = readers.cursor[1];
  BtCursor *c2 = readers.cursor[2];

  readers.reason = TWELVE_REASON_NONE;
  out->need = 0;
  if( (why = reader_begin(db))!=0 ){
    goto out_close_cursors;
  }

  if( (why = cursor_open(2, readers.posts, 0))!=0 ){
    goto out_close_cursors;
  }
  if( sqlite3BtreeTableMoveto(c2, id, 0, &res)!=SQLITE_OK ){
    why = TWELVE_REASON_CURSOR;
    goto out_close_cursors;
  }
  if( res!=0 || sqlite3BtreeEof(c2) ){
    if( TWELVE_SAFE_BUILD ){
      errno = saved_errno;
    }
    return TWELVE_NOT_FOUND;
  }
  if( (why = post_row(&r, &user, &body, &created))!=0 ){
    goto out_close_cursors;
  }
  if( (why = cursor_open(3, readers.users, 0))!=0 ){
    goto out_close_cursors;
  }
  if( (why = author(user, &name, &u))!=0 ){
    goto out_close_cursors;
  }

  if( (why = cursor_open(1, readers.likes_idx, readers.likes_key))!=0 ){
    goto out_close_cursors;
  }
  if( (why = likes_seek(id))!=0 ){
    goto out_close_cursors;
  }
  while( !sqlite3BtreeEof(c1) ){
    i64 post = 0;
    if( (why = record_first_int(c1, &l, &post))!=0 ){
      goto out_close_cursors;
    }
    if( post!=id ){
      break;
    }
    count++;
    if( (why = scan_next(c1))!=0 ){
      goto out_close_cursors;
    }
  }

  out_put(&o, span("{\"post\":", 8));
  out_post_head(&o, id);
  out_string(&o, body);
  out_put(&o, span(",\"created_at\":", 14));
  out_string(&o, created);
  out_post_tail(&o, name, count);
  out_put(&o, span("}", 1));
  result = finish(TWELVE_OK, &o, 0);
  if( TWELVE_SAFE_BUILD ){
    errno = saved_errno;
  }
  return result;

out_close_cursors:
  cursors_close();
  readers.reason = why;
  if( TWELVE_SAFE_BUILD ){
    errno = saved_errno;
  }
  return TWELVE_FALLBACK;
}

void twelve_readers_end(void){
  int saved_errno = errno;
  assert(readers.ready == 1 || readers.open == 0);
  cursors_close();
  errno = saved_errno;
}

void twelve_readers_close(void){
  int saved_errno = errno;
  assert(readers.ready == 0 || readers.db != NULL);
  if( readers.ready ){
    readers_free(readers.db);
    readers.ready = 0;
  }
  errno = saved_errno;
}

int twelve_reader_reason(void){
  return readers.reason;
}

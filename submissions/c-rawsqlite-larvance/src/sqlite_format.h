#ifndef SQLITE_FORMAT_H
#define SQLITE_FORMAT_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifndef RT_H
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef unsigned long long u64;
typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef long long i64;

#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
#endif

#define SQLITE_PAGE_SIZE 4096
#define WAL_HDR_SIZE     32
#define WAL_FRAME_HDR_SZ 24
#define WAL_MAX_VERSION  3007000
#define WAL_MAGIC_LE     0x377f0682
#define WAL_MAGIC_BE     0x377f0683

#define BTREE_INT_INDEX  0x02
#define BTREE_INT_TABLE  0x05
#define BTREE_LEAF_INDEX 0x0a
#define BTREE_LEAF_TABLE 0x0d

static inline u16 rd_be16(const u8 *p) { return (u16)((u16)p[0] << 8 | p[1]); }
static inline u32 rd_be24(const u8 *p) { return (u32)p[0] << 16 | (u32)p[1] << 8 | p[2]; }
static inline u32 rd_be32(const u8 *p) {
  u32 v;
  memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}
static inline u64 rd_be48(const u8 *p) {
  return ((u64)rd_be16(p) << 32) | (u64)rd_be32(p + 2);
}
static inline u64 rd_be64(const u8 *p) {
  u64 v;
  memcpy(&v, p, 8);
  return __builtin_bswap64(v);
}

static inline void wr_be16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static inline void wr_be24(u8 *p, u32 v) { p[0] = (u8)(v >> 16); p[1] = (u8)(v >> 8); p[2] = (u8)v; }
static inline void wr_be32(u8 *p, u32 v) {
  u32 b = __builtin_bswap32(v);
  memcpy(p, &b, 4);
}
static inline void wr_be64(u8 *p, u64 v) {
  u64 b = __builtin_bswap64(v);
  memcpy(p, &b, 8);
}

static inline const u8 *rd_varint(const u8 *p, const u8 *end, u64 *val) {
  if (UNLIKELY(p >= end)) return NULL;
  u8 b = *p++;
  if (LIKELY((b & 0x80) == 0)) { *val = b; return p; }
  u64 x = b & 0x7f;
  for (int i = 0; i < 7; i++) {
    if (UNLIKELY(p >= end)) return NULL;
    b = *p++;
    x = (x << 7) | (b & 0x7f);
    if ((b & 0x80) == 0) { *val = x; return p; }
  }
  if (UNLIKELY(p >= end)) return NULL;
  b = *p++;
  x = (x << 8) | b;
  *val = x;
  return p;
}

static inline const u8 *rd_varint_fast(const u8 *p, u64 *val) {
  u8 b0 = p[0];
  if (LIKELY((b0 & 0x80) == 0)) {
    *val = b0;
    return p + 1;
  }
  u8 b1 = p[1];
  if (LIKELY((b1 & 0x80) == 0)) {
    *val = ((u64)(b0 & 0x7f) << 7) | b1;
    return p + 2;
  }
  u8 b2 = p[2];
  if (LIKELY((b2 & 0x80) == 0)) {
    *val = ((u64)(b0 & 0x7f) << 14) | ((u64)(b1 & 0x7f) << 7) | b2;
    return p + 3;
  }
  u64 x = ((u64)(b0 & 0x7f) << 14) | ((u64)(b1 & 0x7f) << 7) | (b2 & 0x7f);
  p += 3;
  for (int i = 0; i < 5; i++) {
    u8 b = *p++;
    x = (x << 7) | (b & 0x7f);
    if ((b & 0x80) == 0) { *val = x; return p; }
  }
  *val = (x << 8) | *p++;
  return p;
}

static inline int wr_varint(u8 *p, u64 v) {
  if (v <= 0x7f) {
    p[0] = (u8)v;
    return 1;
  }
  if (v <= 0x3fff) {
    p[0] = (u8)((v >> 7) | 0x80);
    p[1] = (u8)(v & 0x7f);
    return 2;
  }
  if (v <= 0x1fffff) {
    p[0] = (u8)((v >> 14) | 0x80);
    p[1] = (u8)(((v >> 7) & 0x7f) | 0x80);
    p[2] = (u8)(v & 0x7f);
    return 3;
  }
  u8 buf[9];
  int i = 0;
  if (v & ((u64)0xff << 56)) {
    buf[8] = (u8)v;
    v >>= 8;
    for (i = 7; i >= 0; i--) {
      buf[i] = (u8)((v & 0x7f) | 0x80);
      v >>= 7;
    }
    memcpy(p, buf, 9);
    return 9;
  }
  while (v > 0) {
    buf[i++] = (u8)((v & 0x7f) | 0x80);
    v >>= 7;
  }
  buf[0] &= 0x7f;
  for (int j = 0; j < i; j++) {
    p[j] = buf[i - 1 - j];
  }
  return i;
}

static inline u8 encode_int_serial(i64 v, u8 *out, u32 *out_len) {
  if (v == 0) { *out_len = 0; return 8; }
  if (v == 1) { *out_len = 0; return 9; }
  if (v >= -128 && v <= 127) { out[0] = (u8)v; *out_len = 1; return 1; }
  if (v >= -32768 && v <= 32767) { wr_be16(out, (u16)v); *out_len = 2; return 2; }
  if (v >= -8388608 && v <= 8388607) { wr_be24(out, (u32)v); *out_len = 3; return 3; }
  if (v >= -2147483648LL && v <= 2147483647LL) { wr_be32(out, (u32)v); *out_len = 4; return 4; }
  wr_be64(out, (u64)v); *out_len = 8; return 6;
}

static inline u32 serial_len(u64 t) {
  static const u8 tbl[12] = {0, 1, 2, 3, 4, 6, 8, 8, 0, 0, 0, 0};
  return t < 12 ? tbl[t] : (u32)((t - 12) >> 1);
}

static inline int col_is_int(u64 t) {
  return (t >= 1 && t <= 6) || t == 8 || t == 9;
}
static inline int col_is_text(u64 t) {
  return t >= 13 && (t & 1);
}

static inline i64 col_decode_int(u64 t, const u8 *p) {
  switch (t) {
    case 3: {
      u32 v = rd_be24(p);
      return (v & 0x800000) ? (i64)(v | 0xff000000U) : (i64)v;
    }
    case 2: return (i16)rd_be16(p);
    case 1: return (i8)p[0];
    case 4: return (i32)rd_be32(p);
    case 8: return 0;
    case 9: return 1;
    case 5: {
      u64 v = rd_be48(p);
      return (v & 0x800000000000ULL) ? (i64)(v | 0xffff000000000000ULL) : (i64)v;
    }
    case 6: return (i64)rd_be64(p);
    default: return 0;
  }
}

typedef struct {
  u64 t;
  const u8 *p;
} col_ref_t;

static inline int parse_record_cols(const u8 *payload, u32 payload_len, int max_cols, col_ref_t *cols) {
  u64 hdr_len;
  const u8 *p = rd_varint_fast(payload, &hdr_len);
  if (UNLIKELY(hdr_len > payload_len)) return -1;
  const u8 *hdr_end = payload + hdr_len;
  const u8 *data_p = hdr_end;
  for (int i = 0; i < max_cols; i++) {
    if (UNLIKELY(p >= hdr_end)) return -1;
    p = rd_varint_fast(p, &cols[i].t);
    cols[i].p = data_p;
    data_p += serial_len(cols[i].t);
  }
  return LIKELY(data_p <= payload + payload_len) ? 0 : -1;
}

static inline void parse_post_cols(const u8 *payload, i64 *uid, const u8 **body, u32 *body_len,
                                   const u8 **created, u32 *created_len) {
  u64 hdr_len, t0, t1, t2, t3;
  const u8 *p = rd_varint_fast(payload, &hdr_len);
  p = rd_varint_fast(p, &t0);
  p = rd_varint_fast(p, &t1);
  p = rd_varint_fast(p, &t2);
  rd_varint_fast(p, &t3);
  const u8 *d = payload + hdr_len + serial_len(t0);
  *uid = col_decode_int(t1, d);
  d += serial_len(t1);
  u32 blen = (u32)((t2 - 12) >> 1);
  *body = d;
  *body_len = blen;
  *created = d + blen;
  *created_len = (u32)((t3 - 12) >> 1);
}

static inline void wal_checksum_bytes(int native_cksum, const u8 *data, int nbytes, const u32 *in, u32 *out) {
  u32 s1 = in ? in[0] : 0;
  u32 s2 = in ? in[1] : 0;
  const u32 *w = (const u32 *)data;
  const u32 *w_end = (const u32 *)(data + nbytes);

  if (!native_cksum) {
    while (w < w_end) {
      s1 += __builtin_bswap32(w[0]) + s2;
      s2 += __builtin_bswap32(w[1]) + s1;
      w += 2;
    }
  } else {
    while (w + 8 <= w_end) {
      s1 += w[0] + s2; s2 += w[1] + s1;
      s1 += w[2] + s2; s2 += w[3] + s1;
      s1 += w[4] + s2; s2 += w[5] + s1;
      s1 += w[6] + s2; s2 += w[7] + s1;
      w += 8;
    }
    while (w < w_end) {
      s1 += w[0] + s2;
      s2 += w[1] + s1;
      w += 2;
    }
  }
  out[0] = s1;
  out[1] = s2;
}

typedef struct __attribute__((packed)) {
  u32 magic;
  u32 version;
  u32 page_size;
  u32 seq;
  u32 salt1;
  u32 salt2;
  u32 cksum1;
  u32 cksum2;
} wal_hdr_t;

typedef struct __attribute__((packed)) {
  u32 page_no;
  u32 n_truncate;
  u32 salt1;
  u32 salt2;
  u32 cksum1;
  u32 cksum2;
} wal_frame_hdr_t;

#endif

// http.bpf.c — everything but SQLite: connection adoption, HTTP framing, routing, HS256 JWT, request-body JSON,
// error responses and response headers. The shim (shim.c) only runs SQLite and writes JSON bodies.
//
//   sockops        on_sockops   passive TCP sockets on our port go into the `clients` sockhash (key: socket cookie),
//                               and their close (FIN or RST) is reported on the `closes` ring buffer
//   sk_skb parser  on_parse     strparser framing: header length + Content-Length
//   sk_skb verdict on_request   one whole request: route, JWT, body; in-place 4xx back out of the same socket, or a
//                               job record (proto.h) redirected into the shim's unix socket
//   sk_skb verdict on_reply     on the shim's unix socket: one reply datagram -> status line + headers, redirected out
//                               of the client socket named by its cookie
//
// Verifier notes (kernel 6.8): every loop is bpf_for, and every piece of state that crosses a loop iteration lives in
// the per-CPU scratch map rather than in registers or on the stack, so the verifier sees no loop-carried scalars and
// every loop converges in a couple of passes. Every buffer index is masked to the buffer's power-of-two size.
#include <linux/bpf.h>
#include <linux/types.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>
#include "proto.h"

char LICENSE[] SEC("license") = "Dual MIT/GPL";

// bpf_for with its own loop variable; break and continue behave as in a plain for
#define each(i, a, b) for (int i, ___once = 1; ___once; ___once = 0) bpf_for(i, a, b)
// Force an explicit mask the verifier can see; clang can't optimize across the asm, so the index stays bounded.
#ifndef DBG
#define DBG 0
#endif
#define dbg(...) do { if (DBG) bpf_printk(__VA_ARGS__); } while (0)
#define BOUND(x, m) ({ unsigned long _b = (unsigned long)(x); asm volatile("%0 &= " #m : "+r"(_b)); (u32)_b; })

#define BUF_SZ 32768  // request copy [0, MSG_CAP) + decoded JWT parts [DEC_OFF, BUF_SZ)
#define BUF_MASK (BUF_SZ - 1)
#define MSG_CAP 16384
#define HDR_SCAN 2048   // header block must end within this many bytes (verifier jump-complexity limit is 8192;
                        // sequential byte-loops accumulate toward it, so keep each one small and few)
#define DEC_HDR (MSG_CAP)          // decoded JWT header
#define DEC_PAY (MSG_CAP + 8192)   // decoded JWT payload
#define DEC_MAX 8192
#define CAP_SZ 8192   // decoded JSON strings we keep: body / username / sub / alg
#define CAP_MASK (CAP_SZ - 1)
#define OUT_SZ 2048   // in-place error responses
#define OUT_MASK (OUT_SZ - 1)
#define UNAME_OFF 4096  // username capture inside cap (body capture uses [0, 4096))
#define UNAME_MAX 255
#define SUB_OFF 4608
#define ALG_OFF 4672
#define BODY_CAP 2048   // decoded body bytes kept; a longer one is over 500 characters unless it's mostly whitespace

// ---- maps ----
struct {
  __uint(type, BPF_MAP_TYPE_SOCKHASH);
  __uint(max_entries, 1 << 19);
  __type(key, u64);
  __type(value, u64);
} clients SEC(".maps");

struct {
  __uint(type, BPF_MAP_TYPE_SOCKMAP);
  __uint(max_entries, 1);
  __type(key, u32);
  __type(value, u64);
} shim SEC(".maps");

struct {
  __uint(type, BPF_MAP_TYPE_SOCKMAP);
  __uint(max_entries, 1);
  __type(key, u32);
  __type(value, u64);
} shim_reply SEC(".maps");

struct {
  __uint(type, BPF_MAP_TYPE_RINGBUF);
  __uint(max_entries, 1 << 20);
} closes SEC(".maps");

struct {
  __uint(type, BPF_MAP_TYPE_ARRAY);
  __uint(max_entries, 1);
  __type(key, u32);
  __type(value, struct config);
} cfg SEC(".maps");

enum jmode { JM_HEADER = 1, JM_PAYLOAD, JM_BODY };
enum jkey { K_NONE = 0, K_OTHER, K_BODY, K_SUB, K_USERNAME, K_EXP, K_NBF, K_ALG };

struct scratch {
  u8 buf[BUF_SZ + 64];
  u8 cap[CAP_SZ + 64];
  u8 out[OUT_SZ + 256];
  u8 blk[64];
  u8 mac[4096];  // padded HMAC inner-hash message (branchless block build)
  u32 W[64];
  u32 H[8];
  // http
  u32 n, hdr_end, len, cl, cl_bad, path_s, path_e, auth_s, auth_e, cl_s, idl, id_end, id_bad;
  u32 p_crlf, p_ls, p_st, p_col, p_cld, p_clv, p_clbad;  // parse_msg loop-carried state (in map memory so the verifier merges)
  s64 id;
  u32 tok_s, tok_e, dot1, dot2, dots;
  u32 dec_n, dec_bad;
  u32 nblk;
  // json scanner
  u32 j_src, j_n, j_mode, j_st, j_err, j_depth, j_top_obj, j_maxd;
  u64 j_stk;
  u32 j_kind, j_klen, j_curkey, j_vkey, j_cbase, j_cmax, j_dlen, j_over, j_capture;
  u64 j_kacc;
  u32 j_need, j_lo, j_hi, j_hexn, j_hexv, j_pend, j_lit, j_litn;
  u32 j_seen, j_bad, j_alg_n, j_have_body, j_body_len, j_uname_len, j_sub_len, j_alg_len, j_uname_over;
  u32 j_num_key, j_num_s, j_exp_s, j_exp_e, j_nbf_s, j_nbf_e, j_body_over;
  // number compare
  u64 nm_m, nm_p;
  s32 nm_x;
  u32 nm_sd, nm_drop, nm_neg, nm_frac, nm_expn, nm_eneg, nm_state, nm_ez;
  s64 uid;
  u32 trim_s, trim_e, units, sc_i, sc_k;
  u32 want_like;
  u64 cookie;
  struct job_hdr jh;
  struct job_like jl;
  struct job_create jc;
  u8 sig[32];
};

struct {
  __uint(type, BPF_MAP_TYPE_ARRAY);
  __uint(max_entries, 2);  // the shim sets 2 * possible CPUs: one slot per CPU per nesting level (task, softirq)
  __type(key, u32);
  __type(value, struct scratch);
} scr SEC(".maps");

struct {
  __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
  __uint(max_entries, 1);
  __type(key, u32);
  __type(value, u32);
} busy SEC(".maps");

// Tail-call target: authenticated requests hand the JWT work to on_auth so it gets its own 512-byte stack budget
// (parse + route + full JWT in one program exceeds the limit). The shim populates progs[1] = on_auth.
struct {
  __uint(type, BPF_MAP_TYPE_PROG_ARRAY);
  __uint(max_entries, 3);
  __type(key, u32);
  __type(value, u32);
} progs SEC(".maps");

// The scratch slot in flight on this CPU, so on_auth reuses the slot grab() chose before the tail call.
struct {
  __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
  __uint(max_entries, 1);
  __type(key, u32);
  __type(value, u32);
} cur SEC(".maps");

// A program running in task context (strparser's work item) can be interrupted by a softirq running the same
// programs on another socket. Nesting is at most two deep and an interrupting program finishes before the one it
// interrupted resumes, so a per-CPU busy flag and two slots per CPU are enough.
static __always_inline struct scratch *grab(u32 *slot) {
  u32 z = 0;
  u32 *b = bpf_map_lookup_elem(&busy, &z);
  if (!b) return 0;
  u32 k = *b ? 1 : 0;
  if (!k) *b = 1;
  *slot = k;
  u32 key = bpf_get_smp_processor_id() * 2 + k;
  u32 z2 = 0;
  u32 *cv = bpf_map_lookup_elem(&cur, &z2);
  if (cv) *cv = k;
  return bpf_map_lookup_elem(&scr, &key);
}
// Stage 2 (after a tail call): reuse the slot grab() chose, without touching the busy flag.
static __always_inline struct scratch *grab_cur(u32 *slot) {
  u32 z = 0;
  u32 *cv = bpf_map_lookup_elem(&cur, &z);
  if (!cv) return 0;
  *slot = *cv & 1;
  u32 key = bpf_get_smp_processor_id() * 2 + (*cv & 1);
  return bpf_map_lookup_elem(&scr, &key);
}
static __always_inline void drop_slot(u32 slot) {
  u32 z = 0;
  u32 *b = bpf_map_lookup_elem(&busy, &z);
  if (b && !slot) *b = 0;
}

#define B(s, x) ((s)->buf[(x) & BUF_MASK])
static __always_inline int is_digit(u32 c) { return c - '0' < 10; }
static __always_inline u32 lc(u32 c) { return c | 0x20; }

static __always_inline void report_close(u64 cookie) {
  struct close_ev *e = bpf_ringbuf_reserve(&closes, sizeof *e, 0);
  if (!e) return;
  e->cookie = cookie;
  bpf_ringbuf_submit(e, 0);
}

// ===================================================================================================================
// sockops: adopt connections, report closes
// ===================================================================================================================
SEC("sockops")
int on_sockops(struct bpf_sock_ops *skops) {
  u32 z = 0;
  struct config *c = bpf_map_lookup_elem(&cfg, &z);
  if (!c || !c->ready) return 1;
  switch (skops->op) {
  case BPF_SOCK_OPS_PASSIVE_ESTABLISHED_CB: {
    if (skops->local_port != c->port) return 1;
    u64 ck = bpf_get_socket_cookie(skops);
    if (bpf_sock_hash_update(skops, &clients, &ck, BPF_NOEXIST) == 0)
      bpf_sock_ops_cb_flags_set(skops, BPF_SOCK_OPS_STATE_CB_FLAG);
    break;
  }
  case BPF_SOCK_OPS_STATE_CB:
    // args[0] = old state, args[1] = new. The peer's FIN takes us to CLOSE_WAIT; an RST straight to CLOSE.
    if (skops->args[1] == BPF_TCP_CLOSE_WAIT ||
        (skops->args[1] == BPF_TCP_CLOSE && skops->args[0] == BPF_TCP_ESTABLISHED))
      report_close(bpf_get_socket_cookie(skops));
    break;
  }
  return 1;
}

// ===================================================================================================================
// HTTP framing
// ===================================================================================================================
// One pass over the message: copy up to MSG_CAP bytes, find the header-block end, and capture the Authorization and
// Content-Length values as it goes. 0 ok, -1 incomplete/oversized header. Collapsing framing into a single bounded
// loop keeps the verifier's cumulative jump count (limit 8192) low; three separate scans blew past it.
enum { HS_SOL = 0, HS_NAME, HS_AUTH, HS_CL, HS_OTHER };
static __noinline int parse_msg(struct __sk_buff *skb, struct scratch *s) {
  if (!s) return 0;
  u32 n = skb->len;
  if (n > MSG_CAP) n = MSG_CAP;
  if (n < 4) return -1;
  if (bpf_skb_load_bytes(skb, 0, s->buf, n)) return -1;
  s->n = n;
  s->hdr_end = 0;
  s->auth_s = s->auth_e = s->cl = s->cl_bad = s->cl_s = 0;
  u32 hs = n < HDR_SCAN ? n : HDR_SCAN;
  s->p_crlf = 0; s->p_ls = 0; s->p_st = HS_SOL; s->p_col = 0; s->p_cld = 0; s->p_clv = 0; s->p_clbad = 0;
  each(i, 0, hs) {
    u32 ch = B(s, i);
    u32 crlf = s->p_crlf;
    if ((crlf == 0 || crlf == 2) && ch == '\r') s->p_crlf = crlf + 1;
    else if ((crlf == 1 || crlf == 3) && ch == '\n') { if (crlf == 3) { s->hdr_end = i + 1; break; } s->p_crlf = crlf + 1; }
    else s->p_crlf = (ch == '\r') ? 1 : 0;
    if (ch == '\n') { s->p_st = HS_SOL; s->p_col = 0; s->p_ls = i + 1; continue; }
    if (ch == '\r') continue;
    if (s->p_st == HS_SOL && s->p_col == 0) { s->p_ls = i; s->p_st = HS_NAME; }
    if (s->p_st == HS_NAME) {
      if (ch == ':') {
        u32 nl = i - s->p_ls;
        if (nl == 13) {
          u32 d = 0;
          #pragma unroll
          for (int k = 0; k < 13; k++) d |= lc(B(s, s->p_ls + k)) ^ (u32)"authorization"[k];
          s->p_st = d ? HS_OTHER : HS_AUTH;
        } else if (nl == 14) {
          u32 d = 0;
          #pragma unroll
          for (int k = 0; k < 14; k++) d |= (k == 7 ? B(s, s->p_ls + k) : lc(B(s, s->p_ls + k))) ^ (u32)"content-length"[k];
          s->p_st = d ? HS_OTHER : HS_CL;
        } else s->p_st = HS_OTHER;
        s->p_cld = 0; s->p_clv = 0; s->p_clbad = 0;
        s->p_col++;
        continue;
      }
      s->p_col++;
      continue;
    }
    if (s->p_st == HS_AUTH) {
      if (!s->auth_s) {
        if (ch == ' ' || ch == '\t') { s->p_col++; continue; }
        s->auth_s = i;
      }
      s->auth_e = i + 1;
      s->p_col++;
      continue;
    }
    if (s->p_st == HS_CL) {
      if (ch == ' ' || ch == '\t') { if (s->p_cld) s->p_clbad = 1; s->p_col++; continue; }
      if (ch < '0' || ch > '9' || s->p_cld >= 9) { s->p_clbad = 1; s->p_col++; continue; }
      s->p_clv = s->p_clv * 10 + (ch - '0');
      s->p_cld++;
      s->cl = s->p_clv;
      s->cl_bad = (s->p_clbad || !s->p_cld) ? 1 : 0;
      s->cl_s = 1;
      s->p_col++;
      continue;
    }
    s->p_col++;
  }
  if (!s->hdr_end) return -1;
  if (s->cl_s && s->cl_bad) s->cl_bad = 1;
  // trim trailing OWS off the Authorization value
  if (s->auth_s) {
    each(k, 0, 8) {
      u32 c = B(s, s->auth_e - 1);
      if (s->auth_e <= s->auth_s || (c != ' ' && c != '\t')) break;
      s->auth_e--;
    }
  }
  return 0;
}

SEC("sk_skb/stream_parser")
int on_parse(struct __sk_buff *skb) {
  u32 slot;
  struct scratch *s = grab(&slot);
  if (!s) return 0;
  int ret = 0;
  if (parse_msg(skb, s) == 0) {
    if (s->cl_bad || s->cl > MSG_CAP) {
      // unparseable or oversized framing: answer this header block alone; the verdict sends 400 and closes
      ret = s->hdr_end;
    } else {
      ret = s->hdr_end + s->cl;
    }
  } else if (skb->len >= MSG_CAP) {
    ret = skb->len;  // no header end in 16 KB: the verdict answers 400 and closes
  }
  drop_slot(slot);
  return ret;
}

// ===================================================================================================================
// Response formatting
// ===================================================================================================================
// Writes the status line and headers at dst (a bounded pointer into a map value with >= 160 bytes of room),
// returns their length. pad >= 0 adds "X-Pad: " + pad 'a's (callers fill those in).
static __always_inline u32 put_u32(u8 *d, u32 v) {
  // digit count first, then write right-to-left into d (map memory, so a variable index is fine; no stack temp)
  u32 n = v < 10 ? 1 : v < 100 ? 2 : v < 1000 ? 3 : v < 10000 ? 4 : v < 100000 ? 5 : v < 1000000 ? 6 : v < 10000000 ? 7 : 8;
#pragma unroll
  for (int i = 0; i < 8; i++) {
    if (i < n) d[(n - 1 - i) & 7] = '0' + v % 10;
    v /= 10;
  }
  return n;
}

static __always_inline u32 status_line(u8 *d, u32 status) {
  switch (status) {
  case 200: __builtin_memcpy(d, "HTTP/1.1 200 OK\r\n", 17); return 17;
  case 201: __builtin_memcpy(d, "HTTP/1.1 201 Created\r\n", 22); return 22;
  case 400: __builtin_memcpy(d, "HTTP/1.1 400 Bad Request\r\n", 26); return 26;
  case 401: __builtin_memcpy(d, "HTTP/1.1 401 Unauthorized\r\n", 27); return 27;
  case 404: __builtin_memcpy(d, "HTTP/1.1 404 Not Found\r\n", 24); return 24;
  case 503: __builtin_memcpy(d, "HTTP/1.1 503 Service Unavailable\r\n", 34); return 34;
  default: __builtin_memcpy(d, "HTTP/1.1 500 Internal Server Error\r\n", 36); return 36;
  }
}

static __always_inline u32 err_status(u32 e) {
  switch (e) {
  case E_NOT_FOUND: return 404;
  case E_NO_BEARER: case E_BAD_TOKEN: case E_BAD_PAYLOAD: return 401;
  default: return 400;
  }
}
static __always_inline u32 err_body_len(u32 e) {
  switch (e) {
  case E_NOT_FOUND: return 21;
  case E_BAD_ID: return 27;
  case E_NO_BEARER: return 32;
  case E_BAD_TOKEN: return 36;
  case E_BAD_PAYLOAD: return 33;
  case E_MALFORMED: return 31;
  case E_REQUIRED: return 28;
  case E_TOO_LONG: return 48;
  default: return 23;
  }
}
static __always_inline void err_body(u8 *d, u32 e) {
  switch (e) {
  case E_NOT_FOUND: __builtin_memcpy(d, "{\"error\":\"not found\"}", 21); break;
  case E_BAD_ID: __builtin_memcpy(d, "{\"error\":\"invalid post id\"}", 27); break;
  case E_NO_BEARER: __builtin_memcpy(d, "{\"error\":\"missing bearer token\"}", 32); break;
  case E_BAD_TOKEN: __builtin_memcpy(d, "{\"error\":\"invalid or expired token\"}", 36); break;
  case E_BAD_PAYLOAD: __builtin_memcpy(d, "{\"error\":\"invalid token payload\"}", 33); break;
  case E_MALFORMED: __builtin_memcpy(d, "{\"error\":\"malformed JSON body\"}", 31); break;
  case E_REQUIRED: __builtin_memcpy(d, "{\"error\":\"body is required\"}", 28); break;
  case E_TOO_LONG: __builtin_memcpy(d, "{\"error\":\"body must be at most 500 characters\"}", 48); break;
  default: __builtin_memcpy(d, "{\"error\":\"bad request\"}", 23); break;
  }
}

// Headers for a JSON body of clen bytes. Returns the header length.
static __always_inline u32 put_head(u8 *base, u32 mask, u32 status, u32 clen, u32 close) {
  u32 o = status_line(base, status);
  __builtin_memcpy(base + (o & mask), "Content-Type: application/json\r\nContent-Length: ", 48);
  o += 48;
  o += put_u32(base + (o & mask), clen);
  __builtin_memcpy(base + (o & mask), "\r\n", 2);
  o += 2;
  if (close) {
    __builtin_memcpy(base + (o & mask), "Connection: close\r\n", 19);
    o += 19;
  }
  __builtin_memcpy(base + (o & mask), "\r\n", 2);
  return o + 2;
}

// ===================================================================================================================
// SHA-256 / HMAC
// ===================================================================================================================
static const u32 K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
    0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
    0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
    0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};

static __always_inline u32 ror(u32 x, u32 n) { return (x >> n) | (x << (32 - n)); }

// Compresses s->blk into s->H. Plain bounded loops (unrolled, clang spills the schedule past the 512-byte stack).
static __always_inline int sha_block(struct scratch *s) {
  if (!s) return 0;
  u32 *W = s->W;
#pragma clang loop unroll(disable)
  for (int i = 0; i < 16; i++)
    W[i] = (u32)s->blk[4 * i] << 24 | (u32)s->blk[4 * i + 1] << 16 | (u32)s->blk[4 * i + 2] << 8 | s->blk[4 * i + 3];
#pragma clang loop unroll(disable)
  for (int i = 16; i < 64; i++) {
    u32 a = W[i - 15], b = W[i - 2];
    W[i] = W[i - 16] + (ror(a, 7) ^ ror(a, 18) ^ (a >> 3)) + W[i - 7] + (ror(b, 17) ^ ror(b, 19) ^ (b >> 10));
  }
  u32 a = s->H[0], b = s->H[1], c = s->H[2], d = s->H[3], e = s->H[4], f = s->H[5], g = s->H[6], h = s->H[7];
#pragma clang loop unroll(disable)
  for (int i = 0; i < 64; i++) {
    u32 t1 = h + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + W[i];
    u32 t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  s->H[0] += a; s->H[1] += b; s->H[2] += c; s->H[3] += d;
  s->H[4] += e; s->H[5] += f; s->H[6] += g; s->H[7] += h;
  return 0;
}

// HMAC-SHA256 of buf[tok_s, dot2) (the JWT's signing input) with the midstates from cfg. Leaves the tag in s->H.
static __noinline int hmac(struct scratch *s, struct config *c) {
  if (!s) return 0;
  u32 m = s->dot2 - s->tok_s;
  if (m > 2048) return -1;
  // assemble (message || 0x80 || zeros || 64-bit bit-length) once, then hash fixed 64-byte blocks with no per-byte
  // branches in the loop (those branches were what exploded the verifier's state count).
  each(i, 0, m) s->mac[i & 2047] = B(s, s->tok_s + i);
  s->nblk = (m + 9 + 63) / 64;
  u32 padded = s->nblk * 64;
  each(i, m, padded) s->mac[i & 4095] = 0;   // at most 72 bytes
  s->mac[m & 2047] = 0x80;
  u64 bits = (u64)(64 + m) * 8;
#pragma unroll
  for (int k = 0; k < 8; k++) s->mac[(padded - 8 + k) & 4095] = (u8)(bits >> (8 * (7 - k)));
#pragma unroll
  for (int i = 0; i < 8; i++) s->H[i] = c->ih[i];
  each(bl, 0, s->nblk) {
    u32 base = (bl * 64) & 4095;
#pragma unroll
    for (int j = 0; j < 64; j++) s->blk[j] = s->mac[(base + j) & 4095];
    sha_block(s);
  }
  // outer: opad midstate, then the 32-byte inner hash + padding (bit length (64 + 32) * 8 = 768)
#pragma unroll
  for (int i = 0; i < 8; i++) {
    u32 h = s->H[i];
    s->blk[4 * i] = h >> 24; s->blk[4 * i + 1] = h >> 16; s->blk[4 * i + 2] = h >> 8; s->blk[4 * i + 3] = h;
  }
  s->blk[32] = 0x80;
#pragma unroll
  for (int i = 33; i < 62; i++) s->blk[i] = 0;
  s->blk[62] = 0x03;
  s->blk[63] = 0x00;
#pragma unroll
  for (int i = 0; i < 8; i++) s->H[i] = c->oh[i];
  sha_block(s);
  return 0;
}

// base64url (no padding) decode, branchless and fixed-stride so the verifier prunes it: copy the input into mac with
// three 'A' (=0) pad bytes, then decode whole 4->3 quanta writing at a fixed i*3 offset, OR-ing a validity flag. The
// output length is arithmetic from n, never accumulated in the loop (an accumulated offset exploded the state count).
static const u8 b64tab[256] SEC(".rodata") = {128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 62, 128, 128, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 128, 128, 128, 128, 128, 128, 128, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 128, 128, 128, 128, 63, 128, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128};
static __noinline void b64_decode(struct scratch *s, u32 a, u32 e, u32 dst, u32 maxin) {
  if (!s) return;
  s->dec_n = 0;
  s->dec_bad = 0;
  a &= (BUF_SZ - 1);
  dst &= (BUF_SZ - 1);
  u32 n = e - a;
  if (n > maxin || n > 1400 || (n & 3) == 1) { s->dec_bad = 1; return; }
  u32 nb = BOUND(n, 2047);   // one asm-forced mask; the verifier tracks the derived bounds from here
  each(i, 0, nb) s->mac[i & 2047] = B(s, a + i);
  s->mac[nb & 2047] = 'A';
  s->mac[(nb + 1) & 2047] = 'A';
  s->mac[(nb + 2) & 2047] = 'A';
  u32 q = (nb + 3) / 4;   // <= 512
  u32 bad = 0;
  each(i, 0, q) {
    u32 p = (i * 4) & 2047;
    u32 v0 = b64tab[s->mac[p & 2047]], v1 = b64tab[s->mac[(p + 1) & 2047]];
    u32 v2 = b64tab[s->mac[(p + 2) & 2047]], v3 = b64tab[s->mac[(p + 3) & 2047]];
    bad |= (v0 | v1 | v2 | v3) & 0x80;
    u32 v = (v0 & 63) << 18 | (v1 & 63) << 12 | (v2 & 63) << 6 | (v3 & 63);
    u32 o = (dst + i * 3) & (BUF_SZ - 1);
    B(s, o & (BUF_SZ - 1)) = v >> 16;
    B(s, (o + 1) & (BUF_SZ - 1)) = v >> 8;
    B(s, (o + 2) & (BUF_SZ - 1)) = v;
  }
  if (bad) { s->dec_bad = 1; return; }
  s->dec_n = (nb >> 2) * 3 + ((nb & 3) ? (nb & 3) - 1 : 0);
}

// ===================================================================================================================
// Strict JSON (RFC 8259, UTF-8 validated), one byte per step, state in scratch. Ported from submission/components/
// body.c (request bodies) and cursed-jwt's strict parser (header / payload claims); same decisions, see those files.
// Depth is limited to 32 (JWT parts, as cursed-jwt) or 64 (bodies; body.c allows 32768, only reachable past 32 KB).
// ===================================================================================================================
enum { J_VALUE, J_AFTER, J_KEY, J_OBJ_FIRST, J_ARR_FIRST, J_COLON, J_STR, J_ESC, J_UHEX, J_NMINUS, J_NZERO, J_NINT,
       J_NDOT, J_NFRAC, J_NE, J_NESIGN, J_NEXP, J_LIT };
enum { JK_SKIP = 0, JK_KEY = 1, JK_CAP = 2 };

static __always_inline int ws(u32 c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

static __always_inline void j_emit(struct scratch *s, u32 c) {
  if (s->j_kind == JK_KEY) {
    if (s->j_klen < 8) s->j_kacc |= (u64)(c & 0xff) << (8 * s->j_klen);
    s->j_klen++;
  } else if (s->j_capture) {
    if (s->j_dlen < s->j_cmax) s->cap[(s->j_cbase + s->j_dlen) & CAP_MASK] = c;
    else s->j_over = 1;
    s->j_dlen++;
  }
}
static __always_inline void j_emit_cp(struct scratch *s, u32 cp) {
  if (cp < 0x80) {
    j_emit(s, cp);
  } else if (cp < 0x800) {
    j_emit(s, 0xC0 | cp >> 6); j_emit(s, 0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    j_emit(s, 0xE0 | cp >> 12); j_emit(s, 0x80 | ((cp >> 6) & 0x3F)); j_emit(s, 0x80 | (cp & 0x3F));
  } else {
    j_emit(s, 0xF0 | cp >> 18); j_emit(s, 0x80 | ((cp >> 12) & 0x3F));
    j_emit(s, 0x80 | ((cp >> 6) & 0x3F)); j_emit(s, 0x80 | (cp & 0x3F));
  }
}
static __always_inline void j_flush(struct scratch *s) {  // a lone high surrogate escape becomes U+FFFD
  if (s->j_pend) { j_emit_cp(s, 0xFFFD); s->j_pend = 0; }
}

static __always_inline u32 key_id(struct scratch *s) {
  u64 k = s->j_kacc;
  switch (s->j_klen) {
  case 3:
    if (k == ('s' | 'u' << 8 | 'b' << 16)) return K_SUB;
    if (k == ('e' | 'x' << 8 | 'p' << 16)) return K_EXP;
    if (k == ('n' | 'b' << 8 | 'f' << 16)) return K_NBF;
    if (k == ('a' | 'l' << 8 | 'g' << 16)) return K_ALG;
    return K_OTHER;
  case 4:
    return k == ('b' | 'o' << 8 | 'd' << 16 | (u64)'y' << 24) ? K_BODY : K_OTHER;
  case 8:
    return k == ((u64)'u' | (u64)'s' << 8 | (u64)'e' << 16 | (u64)'r' << 24 | (u64)'n' << 32 | (u64)'a' << 40 |
                 (u64)'m' << 48 | (u64)'e' << 56) ? K_USERNAME : K_OTHER;
  }
  return K_OTHER;
}

// A value is about to start with byte c at position i; applies the claim/body rules for top-level members.
static __always_inline void j_member_begin(struct scratch *s, u32 c, u32 i) {
  s->j_capture = 0;
  s->j_vkey = 0;
  if (s->j_depth != 1 || !s->j_top_obj) return;
  u32 k = s->j_curkey;
  s->j_vkey = k;
  if (s->j_mode == JM_HEADER) {
    if (k == K_ALG) {
      if (s->j_alg_n++ || c != '"') { s->j_err = 1; return; }
      s->j_capture = 1; s->j_cbase = ALG_OFF; s->j_cmax = 8;
    }
  } else if (s->j_mode == JM_PAYLOAD) {
    u32 bit = 0;
    if (k == K_SUB) bit = 1;
    else if (k == K_USERNAME) bit = 2;
    else if (k == K_EXP) bit = 4;
    else if (k == K_NBF) bit = 8;
    if (!bit) return;
    if (s->j_seen & bit) { s->j_err = 1; return; }
    s->j_seen |= bit;
    if (k == K_EXP || k == K_NBF) {
      if (c != '-' && !is_digit(c)) { s->j_err = 1; return; }
      s->j_num_key = k;
      s->j_num_s = i;
    } else if (c != '"') {
      s->j_bad = 1;
    } else if (k == K_SUB) {
      s->j_capture = 1; s->j_cbase = SUB_OFF; s->j_cmax = 32;
    } else {
      s->j_capture = 1; s->j_cbase = UNAME_OFF; s->j_cmax = UNAME_MAX;
    }
  } else if (k == K_BODY) {
    s->j_have_body = 0;  // a later duplicate replaces any earlier value
    if (c == '"') { s->j_capture = 1; s->j_cbase = 0; s->j_cmax = BODY_CAP; }
  }
  s->j_dlen = 0;
  s->j_over = 0;
}
static __always_inline void j_string_end(struct scratch *s) {
  if (!s->j_capture) return;
  switch (s->j_vkey) {
  case K_ALG: s->j_alg_len = s->j_over ? 99 : s->j_dlen; break;
  case K_SUB: s->j_sub_len = s->j_dlen; if (s->j_over) s->j_bad = 1; break;
  case K_USERNAME: s->j_uname_len = s->j_dlen; if (s->j_over) s->j_bad = 1; break;
  case K_BODY: s->j_have_body = 1; s->j_body_len = s->j_dlen; s->j_body_over = s->j_over; break;
  }
  s->j_capture = 0;
}
static __always_inline void j_number_end(struct scratch *s, u32 i) {
  if (s->j_depth != 1 || !s->j_top_obj || s->j_mode != JM_PAYLOAD) return;
  if (s->j_vkey == K_EXP) { s->j_exp_s = s->j_num_s; s->j_exp_e = i; }
  else if (s->j_vkey == K_NBF) { s->j_nbf_s = s->j_num_s; s->j_nbf_e = i; }
}

static __always_inline void j_push(struct scratch *s, u32 obj) {
  if (s->j_depth >= s->j_maxd) { s->j_err = 1; return; }
  if (obj) s->j_stk |= 1ULL << (s->j_depth & 63);
  else s->j_stk &= ~(1ULL << (s->j_depth & 63));
  s->j_depth++;
}
static __always_inline u32 j_top_is_obj(struct scratch *s) { return (s->j_stk >> ((s->j_depth - 1) & 63)) & 1; }

static __always_inline void j_begin_value(struct scratch *s, u32 c, u32 i) {
  if (s->j_depth == 0) s->j_top_obj = (c == '{');
  j_member_begin(s, c, i);
  if (s->j_err) return;
  if (c == '{') { j_push(s, 1); s->j_st = J_OBJ_FIRST; }
  else if (c == '[') { j_push(s, 0); s->j_st = J_ARR_FIRST; }
  else if (c == '"') { s->j_kind = JK_SKIP; s->j_st = J_STR; s->j_pend = 0; s->j_need = 0; }
  else if (c == '-') s->j_st = J_NMINUS;
  else if (c == '0') s->j_st = J_NZERO;
  else if (is_digit(c)) s->j_st = J_NINT;
  else if (c == 't') { s->j_lit = 1; s->j_litn = 1; s->j_st = J_LIT; }
  else if (c == 'f') { s->j_lit = 2; s->j_litn = 1; s->j_st = J_LIT; }
  else if (c == 'n') { s->j_lit = 3; s->j_litn = 1; s->j_st = J_LIT; }
  else s->j_err = 1;
}

static __noinline void j_step(struct scratch *s, u32 c, u32 i) {
  if (!s) return;
  u32 st = s->j_st;
  // ---- numbers: a byte that can't continue the number ends it and is handled as J_AFTER below ----
  if (st >= J_NMINUS && st <= J_NEXP) {
    u32 d = is_digit(c);
    switch (st) {
    case J_NMINUS: if (c == '0') { s->j_st = J_NZERO; return; } if (d) { s->j_st = J_NINT; return; } s->j_err = 1; return;
    case J_NZERO: if (c == '.') { s->j_st = J_NDOT; return; } if (lc(c) == 'e') { s->j_st = J_NE; return; } break;
    case J_NINT: if (d) return; if (c == '.') { s->j_st = J_NDOT; return; } if (lc(c) == 'e') { s->j_st = J_NE; return; } break;
    case J_NDOT: if (d) { s->j_st = J_NFRAC; return; } s->j_err = 1; return;
    case J_NFRAC: if (d) return; if (lc(c) == 'e') { s->j_st = J_NE; return; } break;
    case J_NE: if (c == '+' || c == '-') { s->j_st = J_NESIGN; return; } if (d) { s->j_st = J_NEXP; return; } s->j_err = 1; return;
    case J_NESIGN: if (d) { s->j_st = J_NEXP; return; } s->j_err = 1; return;
    case J_NEXP: if (d) return; break;
    }
    j_number_end(s, i);
    s->j_st = st = J_AFTER;
  }
  switch (st) {
  case J_VALUE:
    if (ws(c)) return;
    j_begin_value(s, c, i);
    return;
  case J_ARR_FIRST:
    if (ws(c)) return;
    if (c == ']') { s->j_depth--; s->j_st = J_AFTER; return; }
    j_begin_value(s, c, i);
    return;
  case J_OBJ_FIRST:
    if (ws(c)) return;
    if (c == '}') { s->j_depth--; s->j_st = J_AFTER; return; }
    if (c != '"') { s->j_err = 1; return; }
    s->j_kind = JK_KEY; s->j_klen = 0; s->j_kacc = 0; s->j_pend = 0; s->j_need = 0; s->j_st = J_STR;
    return;
  case J_KEY:
    if (ws(c)) return;
    if (c != '"') { s->j_err = 1; return; }
    s->j_kind = JK_KEY; s->j_klen = 0; s->j_kacc = 0; s->j_pend = 0; s->j_need = 0; s->j_st = J_STR;
    return;
  case J_COLON:
    if (ws(c)) return;
    if (c != ':') { s->j_err = 1; return; }
    s->j_st = J_VALUE;
    return;
  case J_AFTER:
    if (ws(c)) return;
    if (s->j_depth == 0) { s->j_err = 1; return; }  // trailing garbage after the top-level value
    if (c == ',') { s->j_st = j_top_is_obj(s) ? J_KEY : J_VALUE; return; }
    if (c == '}' && j_top_is_obj(s)) { s->j_depth--; return; }
    if (c == ']' && !j_top_is_obj(s)) { s->j_depth--; return; }
    s->j_err = 1;
    return;
  case J_LIT: {
    u32 want;
    u32 n = s->j_litn;
    if (s->j_lit == 1) want = n == 1 ? 'r' : n == 2 ? 'u' : 'e';
    else if (s->j_lit == 2) want = n == 1 ? 'a' : n == 2 ? 'l' : n == 3 ? 's' : 'e';
    else want = n == 1 ? 'u' : 'l';
    if (c != want) { s->j_err = 1; return; }
    s->j_litn = ++n;
    if ((s->j_lit == 2 && n == 5) || (s->j_lit != 2 && n == 4)) s->j_st = J_AFTER;
    return;
  }
  case J_STR:
    if (s->j_need) {
      if (c < s->j_lo || c > s->j_hi) { s->j_err = 1; return; }
      s->j_need--;
      s->j_lo = 0x80; s->j_hi = 0xBF;
      j_emit(s, c);
      return;
    }
    if (c == '"') {
      j_flush(s);
      if (s->j_kind == JK_KEY) {
        s->j_curkey = (s->j_depth == 1 && s->j_top_obj) ? key_id(s) : K_OTHER;
        s->j_kind = JK_SKIP;
        s->j_st = J_COLON;
      } else {
        if (s->j_depth == 1 && s->j_top_obj) j_string_end(s);
        s->j_capture = 0;
        s->j_st = J_AFTER;
      }
      return;
    }
    if (c == '\\') { s->j_st = J_ESC; return; }
    if (c < 0x20) { s->j_err = 1; return; }
    j_flush(s);
    if (c >= 0x80) {
      s->j_lo = 0x80; s->j_hi = 0xBF;
      if (c >= 0xC2 && c <= 0xDF) s->j_need = 1;
      else if (c == 0xE0) { s->j_need = 2; s->j_lo = 0xA0; }
      else if (c == 0xED) { s->j_need = 2; s->j_hi = 0x9F; }
      else if (c >= 0xE1 && c <= 0xEF) s->j_need = 2;
      else if (c == 0xF0) { s->j_need = 3; s->j_lo = 0x90; }
      else if (c >= 0xF1 && c <= 0xF3) s->j_need = 3;
      else if (c == 0xF4) { s->j_need = 3; s->j_hi = 0x8F; }
      else { s->j_err = 1; return; }
    }
    j_emit(s, c);
    return;
  case J_ESC: {
    u32 o = 0;
    switch (c) {
    case '"': o = '"'; break;
    case '\\': o = '\\'; break;
    case '/': o = '/'; break;
    case 'b': o = 8; break;
    case 'f': o = 12; break;
    case 'n': o = 10; break;
    case 'r': o = 13; break;
    case 't': o = 9; break;
    case 'u': s->j_hexn = 0; s->j_hexv = 0; s->j_st = J_UHEX; return;
    default: s->j_err = 1; return;
    }
    j_flush(s);
    j_emit(s, o);
    s->j_st = J_STR;
    return;
  }
  case J_UHEX: {
    u32 d;
    if (is_digit(c)) d = c - '0';
    else if (lc(c) - 'a' < 6) d = lc(c) - 'a' + 10;
    else { s->j_err = 1; return; }
    s->j_hexv = s->j_hexv << 4 | d;
    if (++s->j_hexn < 4) return;
    u32 cp = s->j_hexv;
    s->j_st = J_STR;
    if (cp >= 0xD800 && cp <= 0xDBFF) {
      j_flush(s);
      s->j_pend = cp;
    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
      if (s->j_pend) {
        j_emit_cp(s, 0x10000 + ((s->j_pend - 0xD800) << 10) + (cp - 0xDC00));
        s->j_pend = 0;
      } else {
        j_emit_cp(s, 0xFFFD);
      }
    } else {
      j_flush(s);
      j_emit_cp(s, cp);
    }
    return;
  }
  }
}

// Scans buf[src, src + n). Returns 0 if the document is valid JSON (and leaves the member results in s).
static __noinline int json_scan(struct scratch *s, u32 src, u32 n, u32 mode, u32 cap) {
  if (!s) return 0;
  s->j_src = src; s->j_n = n; s->j_mode = mode;
  s->j_st = J_VALUE; s->j_err = 0; s->j_depth = 0; s->j_top_obj = 0; s->j_stk = 0;
  s->j_maxd = mode == JM_BODY ? 64 : 32;
  s->j_kind = JK_SKIP; s->j_curkey = 0; s->j_vkey = 0; s->j_capture = 0; s->j_need = 0; s->j_pend = 0;
  s->j_seen = 0; s->j_bad = 0; s->j_alg_n = 0; s->j_have_body = 0; s->j_body_len = 0; s->j_body_over = 0;
  s->j_uname_len = 0; s->j_sub_len = 0; s->j_alg_len = 0; s->j_exp_s = s->j_exp_e = s->j_nbf_s = s->j_nbf_e = 0;
  u32 jn = n < cap ? n : cap;
  each(i, 0, jn) {
    j_step(s, B(s, s->j_src + i), i);
    if (s->j_err) break;
  }
  if (jn < n) return -1;
  if (s->j_err) return -1;
  u32 st = s->j_st;
  if (st == J_NZERO || st == J_NINT || st == J_NFRAC || st == J_NEXP) {
    j_number_end(s, n);
    st = J_AFTER;
  }
  return (st == J_AFTER && s->j_depth == 0) ? 0 : -1;
}

// Compares the JSON number at buf[a, e) with now (whole seconds): -1 less, 0 equal, 1 greater. Exact for integers,
// decimals and exponents (cursed-jwt uses strtod for non-integers; this differs only past double precision).
static __noinline int num_cmp(struct scratch *s, u32 a, u32 e, s64 now) {
  if (!s) return 0;
  s->nm_m = 0; s->nm_x = 0; s->nm_sd = 0; s->nm_drop = 0; s->nm_neg = 0;
  s->nm_state = 0; s->nm_expn = 0; s->nm_eneg = 0; s->nm_frac = 0;
  if (B(s, a) == '-') { s->nm_neg = 1; a++; }
  each(i, a, e) {
    u32 c = B(s, i);
    if (c == '.') { s->nm_state = 1; continue; }
    if (lc(c) == 'e') { s->nm_state = 2; continue; }
    if (s->nm_state == 2) {
      if (c == '-') { s->nm_eneg = 1; continue; }
      if (c == '+') continue;
      if (s->nm_expn < 100000) s->nm_expn = s->nm_expn * 10 + (c - '0');
      continue;
    }
    // mantissa digit
    if (s->nm_m == 0 && c == '0') {
      if (s->nm_state == 1) s->nm_x--;
      continue;
    }
    if (s->nm_sd < 18) {
      s->nm_m = s->nm_m * 10 + (c - '0');
      s->nm_sd++;
      if (s->nm_state == 1) s->nm_x--;
    } else {
      if (c != '0') s->nm_drop = 1;
      if (s->nm_state != 1) s->nm_x++;
    }
  }
  if (s->nm_m == 0) return now > 0 ? -1 : now < 0 ? 1 : 0;  // zero (any sign)
  if (s->nm_neg) return -1;  // now is never negative
  // value = m * 10^x (plus a little more if digits past 18 were dropped)
  s64 x = s->nm_x + (s->nm_eneg ? -(s64)s->nm_expn : (s64)s->nm_expn);
  if (x > 12) return 1;
  if (x < -18) return -1;  // < 1
  s->nm_p = 1;
  u32 k = x >= 0 ? x : -x;
  each(i, 0, 18) {
    if (i >= k) break;
    s->nm_p *= 10;
  }
  u64 ip, frac;
  if (x >= 0) {
    ip = s->nm_m * s->nm_p;
    frac = s->nm_drop;
  } else {
    ip = s->nm_m / s->nm_p;
    frac = (s->nm_m % s->nm_p) != 0 || s->nm_drop;
  }
  if (ip > (u64)now) return 1;
  if (ip < (u64)now) return -1;
  return frac ? 1 : 0;
}

// ===================================================================================================================
// JWT
// ===================================================================================================================
// Two halves, called one after the other from on_auth (not nested) so the HMAC frame and the claims frame never
// sit on the same call chain: 0 ok (s->uid, username in cap[UNAME_OFF, +j_uname_len)), E_BAD_TOKEN or E_BAD_PAYLOAD.
static __noinline u32 jwt_sig(struct scratch *s, struct config *c) {
  if (!s) return 0;
  // dots
  s->dot1 = s->dot2 = 0;
  s->dots = 0;
  each(i, s->tok_s, s->tok_e) {
    if (B(s, i) != '.') continue;
    s->dots++;
    if (s->dots == 1) s->dot1 = i;
    else if (s->dots == 2) s->dot2 = i;
  }
  if (s->dots != 2 || s->tok_e - s->tok_s > 2048) return E_BAD_TOKEN;
  // signature: 43 chars -> 32 bytes
  if (s->tok_e - s->dot2 - 1 != 43) return E_BAD_TOKEN;
  b64_decode(s, s->dot2 + 1, s->tok_e, DEC_HDR, 44);
  if (s->dec_bad || s->dec_n != 32) return E_BAD_TOKEN;
#pragma unroll
  for (int i = 0; i < 32; i++) s->sig[i] = s->buf[(DEC_HDR + i) & BUF_MASK];
  if (hmac(s, c)) return E_BAD_TOKEN;
  u32 diff = 0;
#pragma unroll
  for (int i = 0; i < 8; i++) {
    u32 h = s->H[i];
    diff |= (s->sig[4 * i] ^ (u8)(h >> 24)) | (s->sig[4 * i + 1] ^ (u8)(h >> 16)) | (s->sig[4 * i + 2] ^ (u8)(h >> 8)) |
            (s->sig[4 * i + 3] ^ (u8)h);
  }
  if (diff) return E_BAD_TOKEN;
  return 0;
}
static __noinline u32 jwt_claims(struct scratch *s, struct config *c) {
  if (!s) return 0;
  // header: {"alg":"HS256",...}
  b64_decode(s, s->tok_s, s->dot1, DEC_HDR, 700);
  if (s->dec_bad) return E_BAD_TOKEN;
  if (json_scan(s, DEC_HDR, s->dec_n, JM_HEADER, 64) || !s->j_top_obj || s->j_alg_n != 1 || s->j_alg_len != 5)
    return E_BAD_TOKEN;
  u8 *a = &s->cap[ALG_OFF & CAP_MASK];
  if (a[0] != 'H' || a[1] != 'S' || a[2] != '2' || a[3] != '5' || a[4] != '6') return E_BAD_TOKEN;
  // payload claims
  b64_decode(s, s->dot1 + 1, s->dot2, DEC_PAY, 1400);
  if (s->dec_bad) return E_BAD_TOKEN;
  if (json_scan(s, DEC_PAY, s->dec_n, JM_PAYLOAD, 1024) || !s->j_top_obj) return E_BAD_TOKEN;
  if (!(s->j_seen & 4)) return E_BAD_TOKEN;  // no exp
  s64 now = (s64)(bpf_ktime_get_tai_ns() / 1000000000ULL) - c->tai_off_s;
  if (num_cmp(s, DEC_PAY + s->j_exp_s, DEC_PAY + s->j_exp_e, now) <= 0) return E_BAD_TOKEN;
  if ((s->j_seen & 8) && num_cmp(s, DEC_PAY + s->j_nbf_s, DEC_PAY + s->j_nbf_e, now) > 0) return E_BAD_TOKEN;
  if (s->j_bad || !(s->j_seen & 1) || !(s->j_seen & 2)) return E_BAD_PAYLOAD;
  // sub: digits only, 1 <= value <= INT64_MAX
  if (s->j_sub_len == 0 || s->j_sub_len > 19) return E_BAD_PAYLOAD;
  s->uid = 0;
  s->sc_k = 0;
  each(i, 0, s->j_sub_len) {
    u32 ch = s->cap[(SUB_OFF + i) & CAP_MASK];
    u64 v = (u64)s->uid;
    if (!is_digit(ch) || v > (0x7fffffffffffffffULL - (ch - '0')) / 10) { s->sc_k = 1; break; }
    s->uid = v * 10 + (ch - '0');
  }
  if (s->sc_k || s->uid <= 0) return E_BAD_PAYLOAD;
  return 0;
}

// ===================================================================================================================
// POST /posts body: trim (JS String.prototype.trim) and count code points
// ===================================================================================================================
static __always_inline u32 cap_at(struct scratch *s, u32 k) { return s->cap[k & CAP_MASK]; }
// Code point of the UTF-8 sequence starting at cap[k] (valid by construction); its length in s->sc_k.
static __always_inline u32 cp_at(struct scratch *s, u32 k) {
  u32 c = cap_at(s, k);
  if (c < 0x80) { s->sc_k = 1; return c; }
  if (c < 0xE0) { s->sc_k = 2; return (c & 0x1F) << 6 | (cap_at(s, k + 1) & 0x3F); }
  if (c < 0xF0) { s->sc_k = 3; return (c & 0x0F) << 12 | (cap_at(s, k + 1) & 0x3F) << 6 | (cap_at(s, k + 2) & 0x3F); }
  s->sc_k = 4;
  return (c & 0x07) << 18 | (cap_at(s, k + 1) & 0x3F) << 12 | (cap_at(s, k + 2) & 0x3F) << 6 | (cap_at(s, k + 3) & 0x3F);
}
static __always_inline int js_ws(u32 cp) {
  return (cp >= 0x09 && cp <= 0x0D) || cp == 0x20 || cp == 0xA0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) ||
         cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x3000 || cp == 0xFEFF;
}
static __noinline u32 check_body(struct scratch *s) {
  if (!s) return 0;
  if (json_scan(s, s->hdr_end, s->len - s->hdr_end, JM_BODY, 2048)) return E_MALFORMED;
  if (!s->j_top_obj || !s->j_have_body) return E_REQUIRED;
  u32 n = s->j_body_len;
  if (s->j_body_over) n = BODY_CAP;
  s->trim_s = 0;
  s->trim_e = n;
  each(i, 0, BODY_CAP) {
    if (s->trim_s >= s->trim_e) break;
    u32 cp = cp_at(s, s->trim_s);
    if (!js_ws(cp)) break;
    s->trim_s += s->sc_k;
  }
  each(i, 0, BODY_CAP) {
    if (s->trim_e <= s->trim_s) break;
    u32 k = s->trim_e - 1;
    // back up over continuation bytes (at most 3)
    if ((cap_at(s, k) & 0xC0) == 0x80 && k > s->trim_s) k--;
    if ((cap_at(s, k) & 0xC0) == 0x80 && k > s->trim_s) k--;
    if ((cap_at(s, k) & 0xC0) == 0x80 && k > s->trim_s) k--;
    u32 cp = cp_at(s, k);
    if (!js_ws(cp)) break;
    s->trim_e = k;
  }
  if (s->trim_s >= s->trim_e) return E_REQUIRED;
  if (s->j_body_over) return E_TOO_LONG;  // > 4 KB decoded that isn't mostly whitespace is > 500 code points
  s->units = 0;
  each(i, s->trim_s, s->trim_e) {
    if ((cap_at(s, i) & 0xC0) != 0x80) s->units++;
  }
  if (s->units > 500) return E_TOO_LONG;
  return 0;
}

// ===================================================================================================================
// Request verdict
// ===================================================================================================================
static __always_inline int send_job(struct __sk_buff *skb, struct scratch *s, struct job_hdr *h) {
  h->len = s->len;
  if (bpf_skb_store_bytes(skb, 0, h, sizeof *h, 0)) return SK_DROP;
  return bpf_sk_redirect_map(skb, &shim, 0, 0);
}

// All 4xx/400-close responses go back through the shim's reply path (a unix datagram, free to resize), formatted by
// on_reply. Errors are the uncommon path under load, so the extra hop is fine and nothing writes variable-length data
// into a fixed skb here. The R_ERROR job is just the 16-byte header, which always fits in a request.
static __always_inline int fail(struct __sk_buff *skb, struct scratch *s, u32 e, u64 *cookie) {
  s->jh = (struct job_hdr){.route = R_ERROR, .err = e, .close = e == E_BAD_REQUEST, .cookie = *cookie};
  if (e == E_BAD_REQUEST) report_close(*cookie);
  return send_job(skb, s, &s->jh);
}

static __always_inline int route(struct __sk_buff *skb, struct scratch *s, u64 *cookie) {
  u32 z = 0;
  struct config *c = bpf_map_lookup_elem(&cfg, &z);
  if (!c) return SK_DROP;
  // the message length: what on_parse returned for this message
  if (parse_msg(skb, s)) { s->len = skb->len; return fail(skb, s, E_BAD_REQUEST, cookie); }
  if (s->cl_bad || s->cl > MSG_CAP) { s->len = s->hdr_end; return fail(skb, s, E_BAD_REQUEST, cookie); }
  s->len = s->hdr_end + s->cl;
  if (s->len > s->n) return fail(skb, s, E_BAD_REQUEST, cookie);
  // request line: GET|POST SP path SP HTTP/1.x
  u32 post;
  if (B(s, 0) == 'G' && B(s, 1) == 'E' && B(s, 2) == 'T' && B(s, 3) == ' ') post = 0, s->path_s = 4;
  else if (B(s, 0) == 'P' && B(s, 1) == 'O' && B(s, 2) == 'S' && B(s, 3) == 'T' && B(s, 4) == ' ') post = 1, s->path_s = 5;
  else return fail(skb, s, E_NOT_FOUND, cookie);
  s->path_e = 0;
  each(i, s->path_s, s->hdr_end) {
    u32 ch = B(s, i);
    if (ch == ' ' || ch == '\r') { s->path_e = i; break; }
  }
  if (!s->path_e) return fail(skb, s, E_BAD_REQUEST, cookie);
  u32 p = s->path_s, pl = s->path_e - s->path_s;
#define P(k) B(s, p + (k))
  int is_posts = pl >= 6 && P(0) == '/' && P(1) == 'p' && P(2) == 'o' && P(3) == 's' && P(4) == 't' && P(5) == 's';
  if (!post && pl == 5 && P(0) == '/' && P(1) == 'f' && P(2) == 'e' && P(3) == 'e' && P(4) == 'd') {
    struct job_hdr h = {.route = R_FEED, .cookie = *cookie};
    return send_job(skb, s, &h);
  }
  if (!post && pl == 7 && P(0) == '/' && P(1) == 'h' && P(2) == 'e' && P(3) == 'a' && P(4) == 'l' && P(5) == 't' &&
      P(6) == 'h') {
    struct job_hdr h = {.route = R_HEALTH, .cookie = *cookie};
    return send_job(skb, s, &h);
  }
  if (!is_posts) return fail(skb, s, E_NOT_FOUND, cookie);
  u32 want_like = 0;
  if (pl == 6) {
    if (!post) return fail(skb, s, E_NOT_FOUND, cookie);
  } else {
    if (P(6) != '/') return fail(skb, s, E_NOT_FOUND, cookie);
    // id segment: up to '/' or the end of the path
    s->id = 0; s->idl = 0; s->id_bad = 0; s->id_end = s->path_e;
    each(k, 0, 64) {
      u32 x = p + 7 + k;
      if (x >= s->path_e) break;
      u32 ch = B(s, x);
      if (ch == '/') { s->id_end = x; break; }
      if (!is_digit(ch) || s->idl >= 18) s->id_bad = 1;
      else s->id = s->id * 10 + (ch - '0');
      s->idl++;
    }
    u32 rest = s->path_e - s->id_end;
    if (rest == 5 && B(s, s->id_end) == '/' && B(s, s->id_end + 1) == 'l' && B(s, s->id_end + 2) == 'i' &&
        B(s, s->id_end + 3) == 'k' && B(s, s->id_end + 4) == 'e') {
      if (!post) return fail(skb, s, E_NOT_FOUND, cookie);
      want_like = 1;
    } else if (rest != 0 || post) {
      return fail(skb, s, E_NOT_FOUND, cookie);
    }
    if (!want_like) {  // GET /posts/:id
      if (s->id_bad || s->idl == 0 || s->id <= 0) return fail(skb, s, E_BAD_ID, cookie);
      struct job_hdr h = {.route = R_POST, .cookie = *cookie};
      struct job_post jp = {.post_id = s->id};
      if (s->len < sizeof h + sizeof jp) return fail(skb, s, E_BAD_REQUEST, cookie);
      if (bpf_skb_store_bytes(skb, sizeof h, &jp, sizeof jp, 0)) return SK_DROP;
      return send_job(skb, s, &h);
    }
  }
#undef P
  // authenticated routes: check the Bearer prefix here (cheap), then hand the JWT work to on_auth via a tail call so
  // it verifies on its own stack. auth is checked before the id/body, per the spec.
  (void)c;
  if (!s->auth_s || s->auth_e - s->auth_s < 7 || B(s, s->auth_s) != 'B' || B(s, s->auth_s + 1) != 'e' ||
      B(s, s->auth_s + 2) != 'a' || B(s, s->auth_s + 3) != 'r' || B(s, s->auth_s + 4) != 'e' ||
      B(s, s->auth_s + 5) != 'r' || B(s, s->auth_s + 6) != ' ')
    return fail(skb, s, E_NO_BEARER, cookie);
  s->tok_s = s->auth_s + 7;
  s->tok_e = s->auth_e;
  s->want_like = want_like;
  s->cookie = *cookie;
  bpf_tail_call(skb, &progs, 1);
  return SK_DROP;  // only reached if progs[1] isn't set up
}

SEC("sk_skb/stream_verdict")
int on_request(struct __sk_buff *skb) {
  u64 cookie = bpf_get_socket_cookie(skb);
  u32 slot;
  struct scratch *s = grab(&slot);
  if (!s) return SK_DROP;
  int v = route(skb, s, &cookie);
  // a successful tail call never returns here; this runs for direct (unauthenticated/error) replies
  drop_slot(slot);
  return v;
}

// Stage 2: HS256 verification only, on a fresh stack (tail-called from on_request). On success it hands the
// job/body work to on_job (another fresh stack); errors reply from here.
SEC("sk_skb/stream_verdict")
int on_auth(struct __sk_buff *skb) {
  u32 slot;
  struct scratch *s = grab_cur(&slot);
  if (!s) return SK_DROP;
  u32 z = 0;
  struct config *c = bpf_map_lookup_elem(&cfg, &z);
  if (!c) { drop_slot(slot); return SK_DROP; }
  u64 cookie = s->cookie;
  u32 e = jwt_sig(s, c);
  if (!e) e = jwt_claims(s, c);
  if (e) {
    int v = fail(skb, s, e, &cookie);
    drop_slot(slot);
    return v;
  }
  bpf_tail_call(skb, &progs, 2);
  drop_slot(slot);
  return SK_DROP;  // only if progs[2] isn't set up
}

// Stage 3: the authenticated jobs (like / create + body validation), on a fresh stack.
SEC("sk_skb/stream_verdict")
int on_job(struct __sk_buff *skb) {
  u32 slot;
  struct scratch *s = grab_cur(&slot);
  if (!s) return SK_DROP;
  u64 cookie = s->cookie;
  int v;
  if (s->want_like) {
    if (s->id_bad || s->idl == 0 || s->id <= 0) {
      v = fail(skb, s, E_BAD_ID, &cookie);
    } else {
      s->jh = (struct job_hdr){.route = R_LIKE, .cookie = cookie};
      s->jl = (struct job_like){.uid = s->uid, .post_id = s->id};
      if (s->len < sizeof s->jh + sizeof s->jl) v = fail(skb, s, E_BAD_REQUEST, &cookie);
      else if (bpf_skb_store_bytes(skb, sizeof s->jh, &s->jl, sizeof s->jl, 0)) v = SK_DROP;
      else v = send_job(skb, s, &s->jh);
    }
  } else {
    u32 e = check_body(s);
    if (e) {
      v = fail(skb, s, e, &cookie);
    } else {
      u32 un = s->j_uname_len, bn = s->trim_e - s->trim_s;
      if (un > UNAME_MAX || bn > BODY_CAP || s->trim_e < s->trim_s) {
        v = fail(skb, s, E_BAD_REQUEST, &cookie);
      } else {
        un &= 0xff;
        bn &= (BODY_CAP - 1);
        s->jh = (struct job_hdr){.route = R_CREATE, .cookie = cookie};
        s->jc = (struct job_create){.uid = s->uid, .uname_len = un, .body_len = bn};
        u32 need = sizeof s->jh + sizeof s->jc + un + bn;
        u32 off = sizeof s->jh + sizeof s->jc;
        if (need > s->len) v = fail(skb, s, E_BAD_REQUEST, &cookie);
        else if (bpf_skb_store_bytes(skb, sizeof s->jh, &s->jc, sizeof s->jc, 0)) v = SK_DROP;
        // lengths as the verifier wants them: provably in [1, max] inside the non-zero branch
        else if (un && bpf_skb_store_bytes(skb, off, &s->cap[UNAME_OFF], ((un - 1) & 0xff) + 1, 0)) v = SK_DROP;
        else if (bn && bpf_skb_store_bytes(skb, off + un, &s->cap[s->trim_s & (BODY_CAP - 1)], ((bn - 1) & (BODY_CAP - 1)) + 1, 0)) v = SK_DROP;
        else v = send_job(skb, s, &s->jh);
      }
    }
  }
  drop_slot(slot);
  return v;
}

// ===================================================================================================================
// Reply verdict (shim's unix socket): status line + headers, out of the client's socket
// ===================================================================================================================
struct rscratch {
  u8 b[REPLY_ROOM + 256];
};
struct {
  __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
  __uint(max_entries, 1);
  __type(key, u32);
  __type(value, struct rscratch);
} rscr SEC(".maps");

SEC("sk_skb")
int on_reply(struct __sk_buff *skb) {
  struct reply_hdr h;
  u32 z = 0;
  if (bpf_skb_load_bytes(skb, 0, &h, sizeof h)) return SK_DROP;
  struct rscratch *r = bpf_map_lookup_elem(&rscr, &z);
  if (!r) return SK_DROP;
  u32 n;
  if (h.status == 0) {  // an error the request was too short to carry: headers + error body, all in the room
    u32 e = h.err;
    u32 blen = err_body_len(e);
    n = put_head(r->b, 255, err_status(e), blen, h.close);
    err_body(r->b + (n & 255), e);
    n += blen;
    if (n > REPLY_ROOM) return SK_DROP;
  } else {
    n = put_head(r->b, 255, h.status, h.body_len, h.close);
    if (n > REPLY_ROOM) return SK_DROP;
  }
  u32 at = REPLY_BODY_OFF - n;  // headers end where the body starts (for an echoed error, the datagram ends there)
  if (n == 0 || n > REPLY_ROOM) return SK_DROP;
  if (bpf_skb_store_bytes(skb, at, r->b, n, 0)) return SK_DROP;
  if (bpf_skb_adjust_room(skb, -(s32)at, 0, 0)) return SK_DROP;
  return bpf_sk_redirect_hash(skb, &clients, &h.cookie, 0);
}

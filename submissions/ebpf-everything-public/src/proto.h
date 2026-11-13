// proto.h — records exchanged between the BPF programs (http.bpf.c) and the SQLite shim (shim.c).
//
// Jobs (BPF -> shim). The client stream_verdict rewrites a request in place into a job and redirects it into the shim's
// unix socket. Under strparser the kernel forwards exactly the request's bytes (stm->offset, stm->full_len), whatever
// the program does to the skb's length. So a job is as long as the request it replaces: the shim reads job_hdr.len
// bytes and skips whatever follows the fields it needs. Every request is at least 18 bytes ("GET / HTTP/1.1\r\n\r\n"),
// and a job that carries more than the 16-byte header always replaces a request that carries more than that header.
//
// Replies (shim -> BPF). One datagram per response: reply_hdr, then REPLY_ROOM bytes the reply verdict fills with the
// status line and headers (right-aligned against the body), then the JSON body. The reply verdict pulls the unused
// front off and redirects what's left out of the client's socket. Unix datagrams aren't strparser messages, so length
// changes stick.
#ifndef EBPF_SUB_PROTO_H
#define EBPF_SUB_PROTO_H

#ifdef __BPF__
typedef __u8 u8;
typedef __u16 u16;
typedef __u32 u32;
typedef __u64 u64;
typedef __s32 s32;
typedef __s64 s64;
#else
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef int64_t s64;
#endif

enum route {
  R_HEALTH = 1,  // job_hdr only
  R_FEED = 2,    // job_hdr only
  R_POST = 3,    // job_hdr + job_post
  R_LIKE = 4,    // job_hdr + job_like
  R_CREATE = 5,  // job_hdr + job_create + username bytes + body bytes
  R_ERROR = 6,   // job_hdr.err: an error whose response didn't fit in the request's bytes; the shim sends it back as is
};

// Error responses the BPF side writes (index = job_hdr.err / reply_hdr.err).
enum http_err {
  E_NONE = 0,
  E_NOT_FOUND,     // 404 {"error":"not found"}
  E_BAD_ID,        // 400 {"error":"invalid post id"}
  E_NO_BEARER,     // 401 {"error":"missing bearer token"}
  E_BAD_TOKEN,     // 401 {"error":"invalid or expired token"}
  E_BAD_PAYLOAD,   // 401 {"error":"invalid token payload"}
  E_MALFORMED,     // 400 {"error":"malformed JSON body"}
  E_REQUIRED,      // 400 {"error":"body is required"}
  E_TOO_LONG,      // 400 {"error":"body must be at most 500 characters"}
  E_BAD_REQUEST,   // 400 {"error":"bad request"} + close: unparseable HTTP
  E_COUNT,
};

struct job_hdr {
  u32 len;     // bytes in this job (= the request's length); the next job starts len bytes after this one
  u8 route;    // enum route
  u8 err;      // R_ERROR: enum http_err
  u8 close;    // 1: close the connection after the response
  u8 pad;
  u64 cookie;  // socket cookie of the client connection (key in the clients sockhash)
};
struct job_post {
  s64 post_id;
};
struct job_like {
  s64 uid;
  s64 post_id;
};
struct job_create {
  s64 uid;
  u16 uname_len;  // username bytes follow this struct, then body_len body bytes (decoded, trimmed UTF-8)
  u16 body_len;
  u32 pad;
};

#define REPLY_ROOM 160  // longest status line + headers (+ error body for R_ERROR echoes) that the reply verdict writes

struct reply_hdr {
  u64 cookie;
  u16 status;    // 0: R_ERROR echo, err says which
  u8 err;        // enum http_err when status == 0
  u8 close;      // 1: add Connection: close (the shim closes the fd after sending)
  u32 body_len;  // JSON body bytes after the room
};

#define REPLY_BODY_OFF (sizeof(struct reply_hdr) + REPLY_ROOM)

// Close events (BPF -> shim, ring buffer): the shim closes the fd it holds for this connection.
struct close_ev {
  u64 cookie;
};

// Configuration the shim writes before it adds any socket (array map, one entry). The HMAC midstates are a function
// of JWT_SECRET alone (cursed-jwt keeps the same two states); nothing request-derived is ever stored in a map.
struct config {
  u32 ih[8];       // SHA-256 state after compressing (key ^ ipad)
  u32 oh[8];       // SHA-256 state after compressing (key ^ opad)
  s64 tai_off_s;   // CLOCK_TAI - CLOCK_REALTIME in seconds (bpf_ktime_get_tai_ns is the only wall clock BPF has)
  u32 port;        // listening port (host order): sockops only adopts passive sockets on it
  u32 ready;       // 1 once the shim's unix socket is in the shim map
};

#endif

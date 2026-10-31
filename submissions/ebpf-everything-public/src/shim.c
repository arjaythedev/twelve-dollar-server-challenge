// shim.c — the only userspace code that runs SQLite. Everything else (accept bookkeeping, HTTP parsing, routing, HS256
// JWT, responses) is in http.bpf.c. The shim:
//   1. loads http.bpf.o, writes the config map (HMAC midstates, port, TAI offset), fills the prog-array
//   2. attaches the sockops to its own cgroup and the sk_skb parser/verdict to the `clients` sockhash
//   3. puts its job socket in `shim` and its reply socket in `shim_reply`, and attaches on_reply to the latter
//   4. listens on HOST:PORT (SO_REUSEPORT), and accepts in batches so the queue drains — it never reads client sockets
//   5. reads job records, runs ONE SQLite transaction per batch (components/engine.c, direct B-tree reads), and writes
//      each reply back tagged with its connection cookie; the BPF reply verdict adds the status line and headers.
//
// SQLite rules: WAL + synchronous=NORMAL, every response built from a read during the batch, nothing cached across
// batches. The engine is #158's, unchanged (engine.c #includes sqlite3.c and reads B-trees directly).
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "engine.h"
#include "proto.h"

#define MAX_BATCH 256
#define JOB_BUF (1 << 20)
#define REPLY_BUF (1 << 20)

static volatile sig_atomic_t stop;
static void on_sig(int s) { (void)s; stop = 1; }

static int die(const char *m) { fprintf(stderr, "shim: %s: %s\n", m, strerror(errno)); return 1; }

// HMAC-SHA256 ipad/opad midstates for JWT_SECRET (the BPF side keeps only these; it never stores a token).
// Self-contained SHA-256 block compression (no OpenSSL) so the shim has no crypto dependency.
static const u32 K256s[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,
    0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,
    0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,
    0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,
    0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
static u32 rl(u32 x, u32 n) { return (x >> n) | (x << (32 - n)); }
// Compress one 64-byte block into h[8] (h starts at SHA-256 IV for a first block).
static void sha_block_s(u32 h[8], const unsigned char b[64]) {
  u32 W[64];
  for (int i = 0; i < 16; i++)
    W[i] = (u32)b[4*i]<<24 | (u32)b[4*i+1]<<16 | (u32)b[4*i+2]<<8 | b[4*i+3];
  for (int i = 16; i < 64; i++) {
    u32 x = W[i-15], y = W[i-2];
    W[i] = W[i-16] + (rl(x,7)^rl(x,18)^(x>>3)) + W[i-7] + (rl(y,17)^rl(y,19)^(y>>10));
  }
  u32 a=h[0],bb=h[1],c=h[2],d=h[3],e2=h[4],f=h[5],g=h[6],hh=h[7];
  for (int i = 0; i < 64; i++) {
    u32 t1 = hh + (rl(e2,6)^rl(e2,11)^rl(e2,25)) + ((e2&f)^(~e2&g)) + K256s[i] + W[i];
    u32 t2 = (rl(a,2)^rl(a,13)^rl(a,22)) + ((a&bb)^(a&c)^(bb&c));
    hh=g; g=f; f=e2; e2=d+t1; d=c; c=bb; bb=a; a=t1+t2;
  }
  h[0]+=a; h[1]+=bb; h[2]+=c; h[3]+=d; h[4]+=e2; h[5]+=f; h[6]+=g; h[7]+=hh;
}
static void sha256(const unsigned char *m, size_t n, unsigned char out[32]) {
  u32 h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
  unsigned char blk[64]; size_t i = 0;
  for (; i + 64 <= n; i += 64) sha_block_s(h, m + i);
  unsigned char tail[128]; size_t r = n - i; memcpy(tail, m + i, r);
  tail[r++] = 0x80; size_t pad = (r <= 56) ? 64 : 128; while (r < pad - 8) tail[r++] = 0;
  u64 bits = (u64)n * 8; for (int k = 0; k < 8; k++) tail[pad-8+k] = (unsigned char)(bits >> (8*(7-k)));
  sha_block_s(h, tail); if (pad == 128) sha_block_s(h, tail + 64);
  for (int k = 0; k < 8; k++) { out[4*k]=h[k]>>24; out[4*k+1]=h[k]>>16; out[4*k+2]=h[k]>>8; out[4*k+3]=h[k]; (void)blk; }
}
static void hmac_midstates(const char *secret, u32 ih[8], u32 oh[8]) {
  unsigned char key[64] = {0};
  size_t n = strlen(secret);
  if (n > 64) sha256((const unsigned char *)secret, n, key);
  else memcpy(key, secret, n);
  unsigned char ip[64], op[64];
  for (int i = 0; i < 64; i++) { ip[i] = key[i] ^ 0x36; op[i] = key[i] ^ 0x5c; }
  u32 hi[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
  u32 ho[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
  sha_block_s(hi, ip); sha_block_s(ho, op);
  for (int i = 0; i < 8; i++) { ih[i] = hi[i]; oh[i] = ho[i]; }
}

// Resolve the cgroup v2 path this process is in (for attaching the sockops above our service).
static int my_cgroup_fd(void) {
  FILE *f = fopen("/proc/self/cgroup", "r");
  if (!f) return -1;
  char line[512], rel[512] = "";
  while (fgets(line, sizeof line, f)) {
    // v2 line is "0::/path"
    if (!strncmp(line, "0::", 3)) {
      char *p = line + 3, *nl = strchr(p, '\n');
      if (nl) *nl = 0;
      snprintf(rel, sizeof rel, "%s", p);
      break;
    }
  }
  fclose(f);
  char path[1024];
  snprintf(path, sizeof path, "/sys/fs/cgroup%s", rel[0] ? rel : "");
  return open(path, O_RDONLY);
}

static int map_fd(struct bpf_object *o, const char *name) {
  struct bpf_map *m = bpf_object__find_map_by_name(o, name);
  if (!m) { fprintf(stderr, "shim: no map %s\n", name); exit(1); }
  return bpf_map__fd(m);
}
static int prog_fd(struct bpf_object *o, const char *name) {
  struct bpf_program *p = bpf_object__find_program_by_name(o, name);
  if (!p) { fprintf(stderr, "shim: no prog %s\n", name); exit(1); }
  return bpf_program__fd(p);
}

int main(void) {
  const char *path = getenv("SQLITE_PATH");
  const char *secret = getenv("JWT_SECRET");
  const char *host = getenv("HOST");
  const char *ports = getenv("PORT");
  if (!path || !secret) { fprintf(stderr, "shim: SQLITE_PATH and JWT_SECRET required\n"); return 1; }
  if (!host) host = "0.0.0.0";
  int port = ports ? atoi(ports) : 80;

  signal(SIGINT, on_sig);
  signal(SIGTERM, on_sig);
  signal(SIGPIPE, SIG_IGN);

  // --- SQLite engine (the only userspace data path) ---
  eng_opts eo;
  eng_default_opts(&eo);
  char err[256];
  eng *e = eng_open(path, &eo, err, sizeof err);
  if (!e) { fprintf(stderr, "shim: eng_open: %s\n", err); return 1; }

  // --- load BPF ---
  struct bpf_object *obj = bpf_object__open_file("http.bpf.o", NULL);
  if (!obj || libbpf_get_error(obj)) return die("open http.bpf.o");
  if (bpf_object__load(obj)) return die("load bpf");

  int cfg = map_fd(obj, "cfg");
  int clients = map_fd(obj, "clients");
  int shim = map_fd(obj, "shim");
  int shim_reply = map_fd(obj, "shim_reply");
  int progs = map_fd(obj, "progs");
  int closes = map_fd(obj, "closes");

  // config
  struct config cf;
  memset(&cf, 0, sizeof cf);
  hmac_midstates(secret, cf.ih, cf.oh);
  struct timespec tr, tt;
  clock_gettime(CLOCK_REALTIME, &tr);
  clock_gettime(CLOCK_TAI, &tt);
  cf.tai_off_s = tt.tv_sec - tr.tv_sec;
  cf.port = port;
  cf.ready = 0;
  u32 z = 0;
  bpf_map_update_elem(cfg, &z, &cf, BPF_ANY);

  // prog-array: progs[1]=on_auth, progs[2]=on_job
  int fa = prog_fd(obj, "on_auth"), fj = prog_fd(obj, "on_job");
  u32 k1 = 1, k2 = 2;
  if (bpf_map_update_elem(progs, &k1, &fa, BPF_ANY) || bpf_map_update_elem(progs, &k2, &fj, BPF_ANY))
    return die("prog_array");

  // userspace add after accept (so sk->sk_socket is set; sockops-add leaves it NULL pre-accept and the verdict
  // data_ready no-ops). attach sk_skb verdict to the clients sockhash
  // verdict-only mode on this kernel (stream_parser never delivers via sockmap here); the verdict re-parses
  // the whole skb, so single-segment requests work. Pipelining/segmentation handling is a known gap (README).
  if (bpf_prog_attach(prog_fd(obj, "on_request"), clients, BPF_SK_SKB_VERDICT, 0)) return die("attach verdict");

  // job socket: a stream socketpair; one end goes in `shim`, the shim reads the other end's data via the first.
  int jp[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, jp)) return die("job socketpair");
  // reply socket: the shim writes replies to rp[1]; rp[0] is in shim_reply with on_reply attached.
  int rp[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, rp)) return die("reply socketpair");

  u32 zero = 0;
  u64 jv = jp[0];
  if (bpf_map_update_elem(shim, &zero, &jv, BPF_ANY)) return die("shim map");
  u64 rv = rp[0];
  if (bpf_map_update_elem(shim_reply, &zero, &rv, BPF_ANY)) return die("shim_reply map");
  if (bpf_prog_attach(prog_fd(obj, "on_reply"), shim_reply, BPF_SK_SKB_STREAM_VERDICT, 0)) return die("attach reply");

  // listen
  int ls = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  if (ls < 0) return die("socket");
  int one = 1;
  setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  setsockopt(ls, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  inet_pton(AF_INET, host, &a.sin_addr);
  if (bind(ls, (struct sockaddr *)&a, sizeof a)) return die("bind");
  if (listen(ls, 4096)) return die("listen");

  // now that everything is wired, let sockops adopt new connections
  cf.ready = 1;
  bpf_map_update_elem(cfg, &z, &cf, BPF_ANY);

  fprintf(stderr, "shim: up on %s:%d, SQLITE_PATH=%s\n", host, port, path);

  // The shim holds every accepted fd so the accept queue drains and the socket stays open; it never reads them.
  // Jobs arrive on jp[0]; replies go out on rp[1].
  int jr = jp[1];  // jobs are redirected (egress) out of jp[0] to its peer jp[1], which we read
  fcntl(jr, F_SETFL, O_NONBLOCK);
  static char jbuf[JOB_BUF];
  size_t jlen = 0;
  static char rbuf[REPLY_BUF];
  int64_t t0 = time(NULL);

  struct pollfd pfd[2] = {{ls, POLLIN, 0}, {jr, POLLIN, 0}};
  while (!stop) {
    int n = poll(pfd, 2, 1000);
    if (n < 0 && errno != EINTR) break;

    // drain the accept queue; hold the fds (never closed until the client goes away, reported via `closes`)
    if (pfd[0].revents & POLLIN) {
      for (int i = 0; i < 1024; i++) {
        int c = accept4(ls, NULL, NULL, SOCK_NONBLOCK);
        if (c < 0) break;
        u64 cookie = 0; socklen_t cl = sizeof cookie;
        getsockopt(c, SOL_SOCKET, SO_COOKIE, &cookie, &cl);
        u64 cv = c; bpf_map_update_elem(clients, &cookie, &cv, BPF_ANY);
        char pk; recv(c, &pk, 1, MSG_PEEK | MSG_DONTWAIT);  // best-effort: nudge data_ready for bytes buffered before the add
      }
    }

    // process jobs: read as much as we can, parse records, run one SQLite batch, write replies
    if (pfd[1].revents & POLLIN) {
      ssize_t got = read(jr, jbuf + jlen, sizeof jbuf - jlen);
      if (got > 0) {
        jlen += got;
        size_t off = 0, rlen = 0;
        eng_begin_batch(e);
        struct { u64 cookie; u16 status; u8 err; u8 close; u32 blen; size_t body_off; } rep[MAX_BATCH];
        int nrep = 0;
        while (off + sizeof(struct job_hdr) <= jlen && nrep < MAX_BATCH) {
          struct job_hdr *h = (struct job_hdr *)(jbuf + off);
          if (h->len < sizeof *h || off + h->len > jlen) break;
          char *body = rbuf + rlen;
          size_t cap = sizeof rbuf - rlen, bn = 0;
          int st = 500;
          switch (h->route) {
          case R_HEALTH: st = eng_health(e, (int64_t)(time(NULL) - t0), body, cap, &bn); break;
          case R_FEED: st = eng_feed(e, body, cap, &bn); break;
          case R_POST: {
            struct job_post *jp2 = (struct job_post *)(h + 1);
            st = eng_get_post(e, jp2->post_id, body, cap, &bn);
            break;
          }
          case R_LIKE: {
            struct job_like *jl = (struct job_like *)(h + 1);
            st = eng_like(e, jl->uid, jl->post_id, body, cap, &bn);
            break;
          }
          case R_CREATE: {
            struct job_create *jc = (struct job_create *)(h + 1);
            const char *un = (const char *)(jc + 1);
            const char *bd = un + jc->uname_len;
            st = eng_create(e, jc->uid, un, jc->uname_len, bd, jc->body_len, body, cap, &bn, NULL, NULL);
            break;
          }
          case R_ERROR:
            rep[nrep].cookie = h->cookie;
            rep[nrep].status = 0;
            rep[nrep].err = h->err;
            rep[nrep].close = h->close;
            rep[nrep].blen = 0;
            rep[nrep].body_off = 0;
            nrep++;
            off += h->len;
            continue;
          }
          rep[nrep].cookie = h->cookie;
          rep[nrep].status = (u16)st;
          rep[nrep].err = 0;
          rep[nrep].close = 0;
          rep[nrep].blen = (u32)bn;
          rep[nrep].body_off = rlen;
          rlen += bn;
          nrep++;
          off += h->len;
        }
        if (eng_commit_batch(e) != 0) {
          // commit failed: turn every reply into 500
          for (int i = 0; i < nrep; i++) {
            rep[i].status = 500;
            rep[i].err = 0;
          }
        }
        // write one reply datagram per response: reply_hdr, REPLY_ROOM blank bytes (the verdict fills them), body
        for (int i = 0; i < nrep; i++) {
          struct iovec iov[3];
          struct reply_hdr rh = {.cookie = rep[i].cookie, .status = rep[i].status, .err = rep[i].err,
                                 .close = rep[i].close, .body_len = rep[i].blen};
          static char room[REPLY_ROOM];
          iov[0].iov_base = &rh;
          iov[0].iov_len = sizeof rh;
          iov[1].iov_base = room;
          iov[1].iov_len = REPLY_ROOM;
          iov[2].iov_base = rbuf + rep[i].body_off;
          iov[2].iov_len = rep[i].blen;
          struct msghdr mh = {0};
          mh.msg_iov = iov;
          mh.msg_iovlen = 3;
          sendmsg(rp[1], &mh, MSG_NOSIGNAL);
        }
        eng_after_batch(e);
        // shift any partial job record to the front
        if (off < jlen) memmove(jbuf, jbuf + off, jlen - off);
        jlen -= off;
      }
    }

    // closed connections: close the fds we hold
    // (ring buffer drain omitted in this prototype; fds are reclaimed on process exit)
  }

  eng_close(e);
  bpf_object__close(obj);
  return 0;
}

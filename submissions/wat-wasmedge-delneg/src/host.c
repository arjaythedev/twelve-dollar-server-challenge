// host.c: a thin WasmEdge embedder, about 100 lines. It runs the AOT-compiled app (hand-written WAT plus
// stock SQLite) with WASI for SQLite's file I/O, and gives it eight Linux socket/epoll calls as the "sys"
// import module, because WASI's poll_oneoff re-registers every connection on every call. No request logic
// lives here: each function only moves its arguments between wasm memory and one syscall.
#define _GNU_SOURCE
#include <wasmedge/wasmedge.h>
#include <errno.h>
#include <libgen.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#define P(i) WasmEdge_ValueGetI32(in[i])
#define RET(v) do { out[0] = WasmEdge_ValueGenI32((int32_t)(v)); return WasmEdge_Result_Success; } while (0)
#define FN(name) static WasmEdge_Result name(void *d, const WasmEdge_CallingFrameContext *f, \
                                             const WasmEdge_Value *in, WasmEdge_Value *out)
static inline uint8_t *mem(const WasmEdge_CallingFrameContext *f, uint32_t off, uint32_t len) {
  return WasmEdge_MemoryInstanceGetPointer(WasmEdge_CallingFrameGetMemoryInstance(f, 0), off, len);
}
static inline int32_t rv(long r) { return r < 0 ? -errno : (int32_t)r; }

// listen(ip as s_addr, port, backlog) -> fd | -errno
FN(h_listen) { (void)d; (void)f;
  int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0); if (fd < 0) RET(-errno);
  int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in a; memset(&a, 0, sizeof a);
  a.sin_family = AF_INET; a.sin_port = htons((uint16_t)P(1)); a.sin_addr.s_addr = (uint32_t)P(0);
  if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(fd, P(2)) < 0) { int e = errno; close(fd); RET(-e); }
  RET(fd);
}
FN(h_epoll_create) { (void)d; (void)f; (void)in; RET(rv(epoll_create1(EPOLL_CLOEXEC))); }
// epoll_ctl(ep, op, fd, events): the event's data is the fd itself
FN(h_epoll_ctl) { (void)d; (void)f;
  struct epoll_event e; e.events = (uint32_t)P(3); e.data.u64 = (uint32_t)P(2);
  RET(rv(epoll_ctl(P(0), P(1), P(2), &e)));
}
// epoll_wait(ep, ptr, max, timeout_ms) -> n; writes n pairs of i32 (fd, events) at ptr
FN(h_epoll_wait) { (void)d;
  struct epoll_event ev[1024]; int max = P(2) > 1024 ? 1024 : P(2);
  int n = epoll_wait(P(0), ev, max, P(3)); if (n <= 0) RET(n < 0 ? -errno : 0);
  int32_t *o = (int32_t *)mem(f, (uint32_t)P(1), (uint32_t)n * 8); if (!o) RET(-EFAULT);
  for (int i = 0; i < n; i++) { o[2 * i] = (int32_t)ev[i].data.u64; o[2 * i + 1] = (int32_t)ev[i].events; }
  RET(n);
}
FN(h_accept) { (void)d; (void)f; RET(rv(accept4(P(0), NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC))); }
FN(h_recv) { (void)d; uint8_t *p = mem(f, (uint32_t)P(1), (uint32_t)P(2)); if (!p) RET(-EFAULT); RET(rv(recv(P(0), p, (size_t)P(2), 0))); }
FN(h_send) { (void)d; uint8_t *p = mem(f, (uint32_t)P(1), (uint32_t)P(2)); if (!p) RET(-EFAULT); RET(rv(send(P(0), p, (size_t)P(2), MSG_NOSIGNAL))); }
FN(h_close) { (void)d; (void)f; RET(rv(close(P(0)))); }

static void add(WasmEdge_ModuleInstanceContext *m, const char *name, WasmEdge_HostFunc_t fn, int np) {
  WasmEdge_ValType p[4], r[1] = {WasmEdge_ValTypeGenI32()};
  for (int i = 0; i < np; i++) p[i] = WasmEdge_ValTypeGenI32();
  WasmEdge_FunctionTypeContext *t = WasmEdge_FunctionTypeCreate(p, (uint32_t)np, r, 1);
  WasmEdge_String s = WasmEdge_StringCreateByCString(name);
  WasmEdge_ModuleInstanceAddFunction(m, s, WasmEdge_FunctionInstanceCreate(t, fn, NULL, 0));
  WasmEdge_StringDelete(s); WasmEdge_FunctionTypeDelete(t);
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s app.so\n", argv[0]); return 2; }
  signal(SIGPIPE, SIG_IGN);
  struct rlimit rl; if (getrlimit(RLIMIT_NOFILE, &rl) == 0) { rl.rlim_cur = rl.rlim_max; setrlimit(RLIMIT_NOFILE, &rl); }
  WasmEdge_ConfigureContext *conf = WasmEdge_ConfigureCreate();
  WasmEdge_ConfigureAddHostRegistration(conf, WasmEdge_HostRegistration_Wasi);
  WasmEdge_ConfigureSetRunMode(conf, WasmEdge_RunMode_AOT);   // load the AOT-compiled native code
  WasmEdge_VMContext *vm = WasmEdge_VMCreate(conf, NULL);

  // WASI: the SPEC's env vars, and the database's directory pre-opened at the same path
  static const char *names[] = {"SQLITE_PATH", "JWT_SECRET", "HOST", "PORT"};
  const char *envs[4]; int ne = 0;
  for (int i = 0; i < 4; i++) {
    const char *v = getenv(names[i]); if (!v) continue;
    char *e = malloc(strlen(names[i]) + strlen(v) + 2); sprintf(e, "%s=%s", names[i], v); envs[ne++] = e;
  }
  const char *sp = getenv("SQLITE_PATH");
  char *dir = dirname(strdup(sp ? sp : "./x")), *pre = malloc(2 * strlen(dir) + 2);
  sprintf(pre, "%s:%s", dir, dir);
  const char *pres[] = {pre}, *args[] = {argv[1]};
  WasmEdge_ModuleInstanceInitWASI(WasmEdge_VMGetImportModuleContext(vm, WasmEdge_HostRegistration_Wasi),
                                  args, 1, envs, (uint32_t)ne, pres, 1);

  WasmEdge_String sys = WasmEdge_StringCreateByCString("sys");
  WasmEdge_ModuleInstanceContext *m = WasmEdge_ModuleInstanceCreate(sys);
  add(m, "listen", h_listen, 3); add(m, "epoll_create", h_epoll_create, 0); add(m, "epoll_ctl", h_epoll_ctl, 4);
  add(m, "epoll_wait", h_epoll_wait, 4); add(m, "accept", h_accept, 1); add(m, "recv", h_recv, 3);
  add(m, "send", h_send, 3); add(m, "close", h_close, 1);
  WasmEdge_Result r = WasmEdge_VMRegisterModuleFromImport(vm, m);
  if (!WasmEdge_ResultOK(r)) { fprintf(stderr, "register: %s\n", WasmEdge_ResultGetMessage(r)); return 1; }

  WasmEdge_String start = WasmEdge_StringCreateByCString("_start");
  r = WasmEdge_VMRunWasmFromFile(vm, argv[1], start, NULL, 0, NULL, 0);
  if (!WasmEdge_ResultOK(r)) { fprintf(stderr, "wasm: %s\n", WasmEdge_ResultGetMessage(r)); return 1; }
  return 0;
}

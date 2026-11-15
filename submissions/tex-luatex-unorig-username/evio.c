/* evio.c: epoll and non-blocking sockets for server.lua. luasocket waits with select(), which stops at
   descriptor 1023; this lets one LuaTeX process hold every keep-alive connection. Sockets are plain fds. */
#define _GNU_SOURCE
#include <lua.h>
#include <lauxlib.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

/* listen(host, port, backlog) -> fd; host is a dotted IPv4 address */
static int l_listen(lua_State *L) {
  const char *host = luaL_checkstring(L, 1);
  int port = (int)luaL_checkinteger(L, 2), backlog = (int)luaL_checkinteger(L, 3), one = 1;
  struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
  if (inet_pton(AF_INET, host, &a.sin_addr) != 1) return luaL_error(L, "HOST must be an IPv4 address: %s", host);
  int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0 || setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one)
      || bind(fd, (struct sockaddr *)&a, sizeof a) || listen(fd, backlog))
    return luaL_error(L, "listen on %s:%d: %s", host, port, strerror(errno));
  lua_pushinteger(L, fd);
  return 1;
}

static int l_epoll(lua_State *L) {
  int ep = epoll_create1(EPOLL_CLOEXEC);
  if (ep < 0) return luaL_error(L, "epoll_create1: %s", strerror(errno));
  lua_pushinteger(L, ep);
  return 1;
}

/* add/mod(ep, fd, want): want 1 = readable, 2 = writable, 3 = both, 0 = neither (level-triggered) */
static int ctl(lua_State *L, int op) {
  int ep = (int)luaL_checkinteger(L, 1), fd = (int)luaL_checkinteger(L, 2), w = (int)luaL_optinteger(L, 3, 1);
  struct epoll_event e = { .events = (w & 1 ? EPOLLIN : 0) | (w & 2 ? EPOLLOUT : 0), .data.fd = fd };
  lua_pushboolean(L, epoll_ctl(ep, op, fd, &e) == 0);
  return 1;
}
static int l_add(lua_State *L) { return ctl(L, EPOLL_CTL_ADD); }
static int l_mod(lua_State *L) { return ctl(L, EPOLL_CTL_MOD); }

/* wait(ep, ms, fds, evs) -> n; fills fds[1..n] and evs[1..n] (1 = read, 2 = write; a hangup or error sets both) */
static int l_wait(lua_State *L) {
  static struct epoll_event ev[1024];
  int ep = (int)luaL_checkinteger(L, 1), ms = (int)luaL_checkinteger(L, 2);
  luaL_checktype(L, 3, LUA_TTABLE);
  luaL_checktype(L, 4, LUA_TTABLE);
  int n = epoll_wait(ep, ev, 1024, ms);
  if (n < 0) n = 0; /* EINTR */
  for (int i = 0; i < n; i++) {
    unsigned e = ev[i].events, bad = e & (EPOLLHUP | EPOLLERR);
    lua_pushinteger(L, ev[i].data.fd);
    lua_rawseti(L, 3, i + 1);
    lua_pushinteger(L, (e & EPOLLIN || bad ? 1 : 0) | (e & EPOLLOUT || bad ? 2 : 0));
    lua_rawseti(L, 4, i + 1);
  }
  lua_pushinteger(L, n);
  return 1;
}

/* accept(fd) -> new non-blocking fd with TCP_NODELAY, or nothing */
static int l_accept(lua_State *L) {
  int one = 1, fd = accept4((int)luaL_checkinteger(L, 1), NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
  if (fd < 0) return 0;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  lua_pushinteger(L, fd);
  return 1;
}

/* recv(fd) -> data, or nil and "timeout" (nothing yet) or "closed" (EOF or error) */
static int l_recv(lua_State *L) {
  static char buf[65536];
  ssize_t n = recv((int)luaL_checkinteger(L, 1), buf, sizeof buf, 0);
  if (n > 0) { lua_pushlstring(L, buf, (size_t)n); return 1; }
  lua_pushnil(L);
  lua_pushstring(L, n < 0 && (errno == EAGAIN || errno == EINTR) ? "timeout" : "closed");
  return 2;
}

/* send(fd, s) -> bytes taken (0 if the socket is full), or nil and the error */
static int l_send(lua_State *L) {
  size_t len;
  const char *s = luaL_checklstring(L, 2, &len);
  ssize_t n = send((int)luaL_checkinteger(L, 1), s, len, MSG_NOSIGNAL);
  if (n >= 0 || errno == EAGAIN || errno == EINTR) { lua_pushinteger(L, n < 0 ? 0 : n); return 1; }
  lua_pushnil(L);
  lua_pushstring(L, strerror(errno));
  return 2;
}

static int l_close(lua_State *L) { close((int)luaL_checkinteger(L, 1)); return 0; }

int luaopen_evio(lua_State *L) {
  static const luaL_Reg f[] = { { "listen", l_listen }, { "epoll", l_epoll }, { "add", l_add }, { "mod", l_mod },
    { "wait", l_wait }, { "accept", l_accept }, { "recv", l_recv }, { "send", l_send }, { "close", l_close }, { NULL, NULL } };
  luaL_newlib(L, f);
  return 1;
}

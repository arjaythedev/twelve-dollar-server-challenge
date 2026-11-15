// epoll4pl.c: epoll and send() for server.pl's event loop. poll() (what wait_for_input/3 uses) walks every open
// connection on every call; epoll returns only the ready ones, so 15,000 idle keep-alive users cost nothing.
// Level-triggered: a connection with unread bytes is reported again on the next wait. It parses nothing.
#include <SWI-Prolog.h>
#include <sys/epoll.h>
#include <errno.h>
#include <sys/socket.h>

static int ep = -1;

// epoll_add(+Fd): watch Fd for input. Fails if the kernel refuses.
static foreign_t pl_epoll_add(term_t t)
{ int fd;
  if ( !PL_get_integer_ex(t, &fd) ) return FALSE;
  struct epoll_event e = { .events = EPOLLIN, .data.fd = fd };
  return ep >= 0 && epoll_ctl(ep, EPOLL_CTL_ADD, fd, &e) == 0;
}

// epoll_wait(-Fds): blocks until at least one watched descriptor is readable. A closed fd leaves the set by itself.
static foreign_t pl_epoll_wait(term_t list)
{ static struct epoll_event ev[1024];
  int n;
  while ( (n = epoll_wait(ep, ev, 1024, -1)) < 0 )
  { if ( errno != EINTR || PL_handle_signals() < 0 ) return FALSE;
  }
  term_t l = PL_copy_term_ref(list), h = PL_new_term_ref();
  for (int i = 0; i < n; i++)
    if ( !PL_unify_list(l, h, l) || !PL_unify_integer(h, ev[i].data.fd) ) return FALSE;
  return PL_unify_nil(l);
}

// epoll_watch(+Fd, +What): What is in (wait for requests) or out (wait until the socket takes more output).
static foreign_t pl_epoll_watch(term_t tf, term_t tw)
{ int fd; char *w;
  if ( !PL_get_integer_ex(tf, &fd) || !PL_get_atom_chars(tw, &w) ) return FALSE;
  struct epoll_event e = { .events = w[0] == 'o' ? EPOLLOUT : EPOLLIN, .data.fd = fd };
  return epoll_ctl(ep, EPOLL_CTL_MOD, fd, &e) == 0;
}

// fd_send(+Fd, +Bytes, -Left): writes what socket Fd takes of Bytes (a string of codes 0..255) without
// blocking; Left is the rest ("" when all went out). Fails if the peer is gone. Responses go out through
// here, not a Prolog output stream, so an idle connection holds no output buffer.
static foreign_t pl_fd_send(term_t tf, term_t tb, term_t tl)
{ int fd; size_t len; char *s;
  if ( !PL_get_integer_ex(tf, &fd) ||
       !PL_get_nchars(tb, &len, &s, CVT_STRING | CVT_ATOM | REP_ISO_LATIN_1 | CVT_EXCEPTION) ) return FALSE;
  while ( len > 0 )
  { ssize_t n = send(fd, s, len, MSG_NOSIGNAL | MSG_DONTWAIT);
    if ( n > 0 ) { s += n; len -= (size_t)n; }
    else if ( n < 0 && errno == EINTR ) continue;
    else if ( n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) ) break;
    else return FALSE;
  }
  return PL_unify_chars(tl, PL_STRING | REP_ISO_LATIN_1, len, s);
}

install_t install_epoll4pl(void)
{ ep = epoll_create1(EPOLL_CLOEXEC);
  PL_register_foreign("epoll_add", 1, pl_epoll_add, 0);
  PL_register_foreign("epoll_wait", 1, pl_epoll_wait, 0);
  PL_register_foreign("epoll_watch", 2, pl_epoll_watch, 0);
  PL_register_foreign("fd_send", 3, pl_fd_send, 0);
}

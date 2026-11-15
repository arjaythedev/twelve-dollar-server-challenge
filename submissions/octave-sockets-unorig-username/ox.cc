// ox.oct: the only C++ in this server. Octave cannot wait on many sockets or open SQLite by
// itself, so this file gives it seven verbs and nothing else:
//   ox("listen", host, port)          bind and listen; set up epoll
//   [fds, chunks] = ox("wait", ms)    accept new clients, return the bytes each ready socket sent
//                                      (a negative fd: that client hung up, close it)
//   ok = ox("send", fd, bytes)        send a response batch; what the socket does not take now is
//                                      queued and sent on EPOLLOUT (false: the client is gone)
//   ox("close", fd)                   drop a client (after its queued bytes are sent)
//   ox("open", path)                  open the SQLite database
//   [row, changes] = ox("q", sql, ...) run one statement to completion with text/integer args;
//                                      row is a cell of the first row's columns as text, or {}
// HTTP parsing, routing, JWT checks, JSON validation and the responses are all in server.m.
#include <octave/oct.h>
#include <octave/Cell.h>
#include <sqlite3.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>

static int ep = -1, lfd = -1;
static sqlite3 *db = nullptr;
static std::unordered_map<std::string, sqlite3_stmt *> stmts; // prepared once, reused (plans, not data)
static char rbuf[65536];
// Replies a client has not taken yet. While it has some, it is not read (so it cannot pile up more),
// and no other client waits for it.
static std::unordered_map<int, std::string> outq;
static std::unordered_set<int> closing; // close once its queue is sent

static void watch (int fd, uint32_t events)
{
  epoll_event ev {};
  ev.events = events;
  ev.data.fd = fd;
  epoll_ctl (ep, EPOLL_CTL_MOD, fd, &ev);
}

// Send what the socket takes without blocking and drop it from S. false: the client is gone.
static bool push (int fd, std::string& s)
{
  size_t off = 0;
  while (off < s.size ())
    {
      ssize_t w = send (fd, s.data () + off, s.size () - off, MSG_NOSIGNAL | MSG_DONTWAIT);
      if (w > 0) off += w;
      else if (w < 0 && errno == EINTR) continue;
      else if (w < 0 && errno == EAGAIN) break;
      else return false;
    }
  s.erase (0, off);
  return true;
}

static void do_listen (const std::string& host, int port)
{
  lfd = socket (AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  int one = 1;
  setsockopt (lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in a {};
  a.sin_family = AF_INET;
  a.sin_port = htons (port);
  if (inet_pton (AF_INET, host.c_str (), &a.sin_addr) != 1)
    error ("ox: bad HOST %s", host.c_str ());
  if (bind (lfd, (sockaddr *) &a, sizeof a) || listen (lfd, 4096))
    error ("ox: cannot listen on %s:%d: %s", host.c_str (), port, strerror (errno));
  ep = epoll_create1 (EPOLL_CLOEXEC);
  epoll_event ev {};
  ev.events = EPOLLIN;
  ev.data.fd = lfd;
  epoll_ctl (ep, EPOLL_CTL_ADD, lfd, &ev);
}

static void do_wait (int ms, octave_value_list& out)
{
  static epoll_event evs[512];
  int n = epoll_wait (ep, evs, 512, ms);
  RowVector fds;
  Cell chunks;
  octave_idx_type k = 0;
  auto put = [&] (double fd, const std::string& s)
    {
      if (k == fds.numel ()) { fds.resize (2 * k + 16); chunks.resize (dim_vector (1, 2 * k + 16)); }
      fds(k) = fd;
      chunks(k++) = octave_value (s);
    };
  for (int i = 0; i < n; i++)
    {
      int fd = evs[i].data.fd;
      if (fd == lfd)
        {
          // Client sockets stay blocking; every recv and send uses MSG_DONTWAIT.
          int c;
          while ((c = accept4 (lfd, nullptr, nullptr, SOCK_CLOEXEC)) >= 0)
            {
              int one = 1;
              setsockopt (c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
              epoll_event ev {};
              ev.events = EPOLLIN;
              ev.data.fd = c;
              epoll_ctl (ep, EPOLL_CTL_ADD, c, &ev);
            }
          continue;
        }
      auto q = outq.find (fd);
      if (q != outq.end ()) // writable (or failed) while replies are queued
        {
          if (! push (fd, q->second))
            {
              outq.erase (q);
              closing.erase (fd);
              put (-fd, ""); // server.m closes it
            }
          else if (q->second.empty ())
            {
              outq.erase (q);
              if (closing.erase (fd)) close (fd);
              else watch (fd, EPOLLIN);
            }
          continue;
        }
      // One read per wake-up: a client that pipelines a lot is served a slice at a time.
      std::string got;
      bool gone = false;
      ssize_t r;
      while ((r = recv (fd, rbuf, sizeof rbuf, MSG_DONTWAIT)) < 0 && errno == EINTR)
        ;
      if (r > 0) got.assign (rbuf, r);
      else if (! (r < 0 && errno == EAGAIN)) gone = true; // EOF or a hard error
      if (! got.empty ()) put (fd, got);
      if (gone) put (-fd, ""); // server.m answers what came before the EOF, then closes it
    }
  fds.resize (k);
  chunks.resize (dim_vector (1, k));
  out(0) = fds;
  out(1) = chunks;
}

static bool do_send (int fd, std::string s)
{
  if (! push (fd, s)) { close (fd); return false; }
  if (! s.empty ())
    {
      outq[fd] = std::move (s);
      watch (fd, EPOLLOUT); // stop reading it until the rest is sent
    }
  return true;
}

static sqlite3_stmt *stmt (const std::string& sql)
{
  auto it = stmts.find (sql);
  if (it != stmts.end ()) return it->second;
  sqlite3_stmt *s;
  if (sqlite3_prepare_v3 (db, sql.c_str (), -1, SQLITE_PREPARE_PERSISTENT, &s, nullptr) != SQLITE_OK)
    error ("sqlite: %s", sqlite3_errmsg (db));
  stmts[sql] = s;
  return s;
}

static void do_query (const octave_value_list& args, octave_value_list& out)
{
  sqlite3_stmt *s = stmt (args(1).string_value ());
  for (int i = 2; i < args.length (); i++)
    if (args(i).is_string ())
      {
        std::string v = args(i).string_value ();
        sqlite3_bind_text (s, i - 1, v.data (), v.size (), SQLITE_TRANSIENT);
      }
    else
      sqlite3_bind_int64 (s, i - 1, (sqlite3_int64) args(i).double_value ());
  Cell row;
  int rc;
  // Step to the end: in autocommit mode the COMMIT happens here, before we return.
  while ((rc = sqlite3_step (s)) == SQLITE_ROW)
    if (row.isempty ())
      {
        int n = sqlite3_column_count (s);
        row.resize (dim_vector (1, n));
        for (int c = 0; c < n; c++)
          {
            const char *t = (const char *) sqlite3_column_text (s, c);
            row(c) = octave_value (std::string (t ? t : "", sqlite3_column_bytes (s, c)));
          }
      }
  sqlite3_reset (s);
  sqlite3_clear_bindings (s);
  if (rc != SQLITE_DONE)
    error ("sqlite: %s", sqlite3_errmsg (db));
  out(0) = row;
  out(1) = (double) sqlite3_changes (db);
}

DEFUN_DLD (ox, args, nargout, "ox (VERB, ...): sockets and SQLite for server.m")
{
  octave_value_list out;
  std::string verb = args(0).string_value ();
  if (verb == "wait")
    do_wait (args(1).int_value (), out);
  else if (verb == "q")
    do_query (args, out);
  else if (verb == "send")
    out(0) = do_send (args(1).int_value (), args(2).string_value ());
  else if (verb == "close")
    {
      int fd = args(1).int_value ();
      if (outq.count (fd)) closing.insert (fd);
      else close (fd);
    }
  else if (verb == "listen")
    do_listen (args(1).string_value (), args(2).int_value ());
  else if (verb == "open")
    {
      if (sqlite3_open_v2 (args(1).string_value ().c_str (), &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK)
        error ("sqlite: cannot open %s", args(1).string_value ().c_str ());
      sqlite3_busy_timeout (db, 5000);
    }
  else
    error ("ox: unknown verb %s", verb.c_str ());
  return out;
}

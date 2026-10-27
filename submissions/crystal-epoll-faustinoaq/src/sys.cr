require "c/sys/socket"
require "c/sys/resource"
require "c/sys/epoll"
require "c/string"
require "c/stdlib"
require "c/unistd"
require "c/time"

# Thin Linux bindings. Only what the server needs, declared here so the hot path never
# goes through the stdlib's IO/Socket/EventLoop layers (they allocate, and the GC is off).
lib LibSys
  alias Int = LibC::Int
  alias SizeT = LibC::SizeT
  alias SSizeT = LibC::SSizeT

  AF_INET     =      2
  SOCK_STREAM =      1
  SOCK_NONBLOCK = 0o4000
  SOCK_CLOEXEC = 0o2000000
  SOL_SOCKET  =      1
  SO_REUSEADDR =     2
  SO_REUSEPORT =    15
  SO_KEEPALIVE =     9
  IPPROTO_TCP =      6
  TCP_NODELAY =      1
  TCP_DEFER_ACCEPT = 9
  TCP_QUICKACK = 12
  MSG_DONTWAIT = 0x40
  MSG_NOSIGNAL = 0x4000


  EAGAIN = 11
  EINTR  = 4

  RLIMIT_NOFILE = 7
  CLOCK_REALTIME = 0
  CLOCK_MONOTONIC = 1
  CLOCK_REALTIME_COARSE = 5
  CLOCK_MONOTONIC_COARSE = 6

  struct SockaddrIn
    sin_family : UInt16
    sin_port : UInt16
    sin_addr : UInt32
    sin_zero : UInt8[8]
  end

  struct Rlimit
    cur : UInt64
    max : UInt64
  end

  struct Iovec
    base : Void*
    len : SizeT
  end

  fun writev(fd : Int, iov : Iovec*, n : Int) : SSizeT
  fun setrlimit(res : Int, rl : Rlimit*) : Int
  fun calloc(n : SizeT, sz : SizeT) : Void*
  fun memmem(h : Void*, hl : SizeT, n : Void*, nl : SizeT) : Void*
  fun inet_addr(s : UInt8*) : UInt32
  fun sched_getaffinity(pid : Int, size : SizeT, mask : UInt8*) : Int
end

@[Link("crypto")]
lib LibSha
  # SHA256_CTX is 112 bytes in OpenSSL 3; keep it opaque and copy it by value.
  struct ShaCtx
    data : UInt64[14]
  end

  fun sha256_init = SHA256_Init(c : ShaCtx*) : LibC::Int
  fun sha256_update = SHA256_Update(c : ShaCtx*, data : Void*, len : LibC::SizeT) : LibC::Int
  fun sha256_final = SHA256_Final(md : UInt8*, c : ShaCtx*) : LibC::Int
end

module Sys
  {% for c in LibSys.constants %}
    {% if LibSys.constant(c).is_a?(NumberLiteral) %}
      {{c}} = LibSys::{{c}}
    {% end %}
  {% end %}
  alias EpollEvent = LibC::EpollEvent
  EPOLLIN    = LibC::EPOLLIN
  EPOLLOUT   = LibC::EPOLLOUT
  EPOLLERR   = LibC::EPOLLERR
  EPOLLHUP   = LibC::EPOLLHUP
  EPOLLRDHUP = LibC::EPOLLRDHUP
  EPOLLET    = LibC::EPOLLET
  EPOLL_CTL_ADD = 1
  EPOLL_CTL_DEL = 2
  EPOLL_CTL_MOD = 3

  @[AlwaysInline]
  def self.epoll(ep : Int32, op : Int32, fd : Int32, events : UInt32) : Int32
    ev = LibC::EpollEvent.new
    ev.events = events
    d = LibC::EpollDataT.new
    d.u64 = fd.to_u64
    ev.data = d
    LibC.epoll_ctl(ep, op, fd, pointerof(ev))
  end
  alias SockaddrIn = LibSys::SockaddrIn
  alias Rlimit = LibSys::Rlimit
  alias Timespec = LibC::Timespec
  @[AlwaysInline]
  def self.errno : Int32
    LibC.__errno_location.value
  end

  def self.env(name : String) : String?
    p = LibC.getenv(name.to_unsafe)
    p.null? ? nil : String.new(p)
  end

  def self.die(msg : String) : NoReturn
    STDERR.puts msg
    exit 1
  end

  def self.raise_nofile : UInt64
    rl = uninitialized Rlimit
    LibC.getrlimit(RLIMIT_NOFILE, pointerof(rl).as(LibC::Rlimit*))
    rl.cur = rl.max
    LibSys.setrlimit(RLIMIT_NOFILE, pointerof(rl))
    rl.max
  end

  def self.listen_tcp(host : String, port : Int32, reuseport : Bool) : Int32
    fd = LibC.socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)
    die "socket failed" if fd < 0
    one = 1
    LibC.setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, pointerof(one).as(Void*), 4)
    LibC.setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, pointerof(one).as(Void*), 4) if reuseport
    addr = SockaddrIn.new
    addr.sin_family = AF_INET.to_u16
    addr.sin_port = ((port & 0xff) << 8 | (port >> 8) & 0xff).to_u16
    addr.sin_addr = LibSys.inet_addr(host.to_unsafe)
    die "bind #{host}:#{port} failed (errno #{errno})" if LibC.bind(fd, pointerof(addr).as(LibC::Sockaddr*), sizeof(SockaddrIn).to_u32) != 0
    die "listen failed" if LibC.listen(fd, 65535) != 0
    fd
  end

  @[AlwaysInline]
  def self.now_ns(clk = CLOCK_REALTIME) : Int64
    ts = uninitialized Timespec
    LibC.clock_gettime(clk, pointerof(ts))
    ts.tv_sec.to_i64 &* 1_000_000_000 &+ ts.tv_nsec.to_i64
  end
end

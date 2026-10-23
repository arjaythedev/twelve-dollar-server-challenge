using System.Buffers;
using System.Buffers.Binary;
using System.Buffers.Text;
using System.Net;
using System.Runtime.InteropServices;
using System.Text;
using static Feed.Native;

namespace Feed;

/// <summary>
/// A single-threaded HTTP/1.1 server on epoll. Each epoll_wait result is one batch: every
/// complete request in it runs inside one SQLite transaction, the batch commits once, and only
/// then are the responses sent. So a 201 always follows its commit (rule 6), and writes from
/// many requests share one commit (group commit).
/// </summary>
internal sealed unsafe class Server {
    private const int MaxEvents = 512;           // events taken from one epoll_wait
    private const int ReadSize = 64 * 1024;      // bytes requested per read
    private const int MaxRequestBytes = 1 << 20; // largest body, and largest partial request kept
    private const int MaxPendingBytes = 4 << 20; // unsent output a client may build up before it is dropped

    /// <summary>Per-connection state, kept in _conns by fd and reused. Idle connections hold no buffers.</summary>
    private sealed class Conn {
        public bool Open;
        public int Generation;     // bumped on every accept, so a reused fd is not mistaken for the old connection
        public byte[]? Input;      // a partial request carried over to the next read (pooled)
        public int InputLength;
        public byte[]? Pending;    // response bytes the socket would not take yet (pooled)
        public int PendingStart, PendingLength;
        public bool CloseWhenFlushed; // close once the response is sent and Pending is drained
    }

    /// <summary>One response in _out, at [Start, End), waiting for the batch to commit. Generation must still match the connection's when it is sent.</summary>
    private struct Segment {
        public int Fd, Generation, Start, End;
        public bool Close;
    }

    private readonly Api _api;
    private readonly Db _db;
    private readonly int _epoll, _listen;
    private Conn?[] _conns = new Conn?[1024];
    private Segment[] _segments = new Segment[1024];
    private int _segmentCount;
    private readonly Out _out = new(1 << 20);   // all responses of the current batch
    private readonly Out _body = new(64 * 1024); // body of the response being built
    private readonly byte[] _read = GC.AllocateArray<byte>(ReadSize, pinned: true);
    private readonly byte[] _date = new byte[29]; // cached Date header value (RFC 1123, 29 bytes)
    private long _dateSecond = -1;
    private bool _inBatchTransaction;
    private bool _batchBroken;   // the batch transaction failed or was rolled back: answer the rest with 500s
    private bool _acceptPaused;  // out of file descriptors: stop watching the listener until a connection closes

    /// <summary>
    /// Opens a non-blocking IPv4 listening socket and the epoll instance. <paramref name="host"/> must be
    /// an IP address, not a name. Throws if any system call fails.
    /// </summary>
    public Server(string host, int port, Api api, Db db) {
        _api = api;
        _db = db;

        _listen = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (_listen < 0) {
            Fail("socket");
        }

        int one = 1;
        setsockopt(_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(int));
        setsockopt(_listen, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(int)); // inherited by accepted sockets
        SockAddrIn address = new() {
            Family = AF_INET,
            Port = BinaryPrimitives.ReverseEndianness((ushort)port),
            Addr = BitConverter.ToUInt32(IPAddress.Parse(host).GetAddressBytes()),
        };
        if (bind(_listen, &address, (uint)sizeof(SockAddrIn)) < 0) {
            Fail($"bind {host}:{port}");
        }

        if (listen(_listen, 65535) < 0) {
            Fail("listen");
        }

        _epoll = epoll_create1(0);
        if (_epoll < 0) {
            Fail("epoll_create1");
        }

        Watch(EPOLL_CTL_ADD, _listen, EPOLLIN);
    }

    /// <summary>Runs the event loop. Never returns.</summary>
    public void Run() {
        EpollEvent* events = (EpollEvent*)NativeMemory.Alloc((nuint)(MaxEvents * sizeof(EpollEvent)));
        while (true) {
            int n = epoll_wait(_epoll, events, MaxEvents, -1);
            if (n < 0) {
                if (Marshal.GetLastPInvokeError() == EINTR) {
                    continue;
                }

                Fail("epoll_wait");
            }

            _out.Reset();
            _segmentCount = 0;
            _inBatchTransaction = false;
            _batchBroken = false;

            for (int i = 0; i < n; i++) {
                int fd = (int)events[i].Data;
                uint ev = events[i].Events;
                if (fd == _listen) {
                    Accept();
                    continue;
                }
                Conn? conn = _conns[fd];
                if (conn is null || !conn.Open) {
                    continue;
                }

                if ((ev & EPOLLOUT) != 0) {
                    Flush(fd, conn);
                }

                if (!conn.Open) {
                    continue;
                }

                if ((ev & EPOLLIN) != 0) {
                    Receive(fd, conn);
                } else if ((ev & (EPOLLERR | EPOLLHUP)) != 0) {
                    Close(fd, conn);
                }
            }

            if (_batchBroken) {
                if (_db.InTransaction) {
                    _db.Rollback();
                }

                FailBatch();
            } else if (_inBatchTransaction && !_db.Commit()) {
                FailBatch();
            }
            SendBatch();
            _db.MaybeCheckpoint();
        }
    }

    /// <summary>Accepts every connection waiting on the listening socket. Out of file descriptors, it stops watching the listener until a connection closes.</summary>
    private void Accept() {
        while (true) {
            int fd = accept4(_listen, null, null, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd < 0) {
                int errno = Marshal.GetLastPInvokeError();
                if (errno is EMFILE or ENFILE) {
                    // The listener stays readable while no fd is free, so watching it would make this loop spin.
                    Watch(EPOLL_CTL_DEL, _listen, 0);
                    _acceptPaused = true;
                }
                if (errno is not EAGAIN and not EINTR) {
                    Console.Error.WriteLine($"accept4 failed: errno {errno}");
                }

                return;
            }
            if (fd >= _conns.Length) {
                Array.Resize(ref _conns, Math.Max(_conns.Length * 2, fd + 1));
            }

            Conn conn = _conns[fd] ??= new Conn();
            conn.Open = true;
            conn.Generation++;
            Watch(EPOLL_CTL_ADD, fd, EPOLLIN);
        }
    }

    /// <summary>
    /// Reads once and handles every complete request in the data. Leftover bytes of a partial request
    /// stay in conn.Input. Closes on EOF, on a read error, or when a partial request exceeds MaxRequestBytes.
    /// </summary>
    private void Receive(int fd, Conn conn) {
        nint got;
        if (conn.Input is null) {
            fixed (byte* p = _read) {
                got = read(fd, p, ReadSize);
            }

            if (got > 0) {
                int used = Parse(fd, conn, _read.AsSpan(0, (int)got));
                if (!conn.Open || conn.CloseWhenFlushed) {
                    return;
                }

                if (used < got) {
                    Carry(conn, _read.AsSpan(used, (int)got - used));
                }
            }
        } else {
            if (conn.Input.Length - conn.InputLength < ReadSize) {
                byte[] bigger = ArrayPool<byte>.Shared.Rent(conn.InputLength + ReadSize);
                conn.Input.AsSpan(0, conn.InputLength).CopyTo(bigger);
                ArrayPool<byte>.Shared.Return(conn.Input);
                conn.Input = bigger;
            }
            fixed (byte* p = conn.Input) {
                got = read(fd, p + conn.InputLength, ReadSize);
            }

            if (got > 0) {
                conn.InputLength += (int)got;
                int used = Parse(fd, conn, conn.Input.AsSpan(0, conn.InputLength));
                if (!conn.Open) {
                    return;
                }

                conn.InputLength -= used;
                if (conn.InputLength == 0 || conn.CloseWhenFlushed) {
                    ArrayPool<byte>.Shared.Return(conn.Input);
                    conn.Input = null;
                    conn.InputLength = 0;
                } else {
                    conn.Input.AsSpan(used, conn.InputLength).CopyTo(conn.Input);
                    if (conn.InputLength > MaxRequestBytes) {
                        Close(fd, conn);
                    }
                }
            }
        }

        if (got == 0) {
            Close(fd, conn);
        } else if (got < 0 && Marshal.GetLastPInvokeError() is not (EAGAIN or EINTR)) {
            Close(fd, conn);
        }
    }

    /// <summary>Copies a partial request into a pooled buffer on the connection.</summary>
    private static void Carry(Conn conn, ReadOnlySpan<byte> partial) {
        conn.Input = ArrayPool<byte>.Shared.Rent(Math.Max(partial.Length, 4096));
        partial.CopyTo(conn.Input);
        conn.InputLength = partial.Length;
    }

    /// <summary>
    /// Handles every complete request in <paramref name="data"/>; returns the bytes consumed. Closes the
    /// connection on a bad request line, a bad, oversized or conflicting Content-Length, whitespace
    /// before a header colon, or any Transfer-Encoding.
    /// Stops after a request that asks for Connection: close.
    /// </summary>
    private int Parse(int fd, Conn conn, ReadOnlySpan<byte> data) {
        int consumed = 0;
        while (consumed < data.Length && !conn.CloseWhenFlushed) {
            ReadOnlySpan<byte> rest = data[consumed..];
            int headEnd = rest.IndexOf("\r\n\r\n"u8);
            if (headEnd < 0) {
                break;
            }

            ReadOnlySpan<byte> head = rest[..headEnd];
            int lineEnd = head.IndexOf("\r\n"u8);
            ReadOnlySpan<byte> requestLine = lineEnd < 0 ? head : head[..lineEnd];
            int sp1 = requestLine.IndexOf((byte)' ');
            int sp2 = sp1 < 0 ? -1 : requestLine.LastIndexOf((byte)' ');
            if (sp1 <= 0 || sp2 <= sp1) {
                Close(fd, conn);
                return data.Length;
            }
            ReadOnlySpan<byte> method = requestLine[..sp1];
            ReadOnlySpan<byte> target = requestLine[(sp1 + 1)..sp2];
            bool http11 = requestLine[(sp2 + 1)..].SequenceEqual("HTTP/1.1"u8);

            int contentLength = 0;
            bool sawLength = false, hasAuth = false, close = !http11;
            ReadOnlySpan<byte> auth = default;
            ReadOnlySpan<byte> headers = lineEnd < 0 ? default : head[(lineEnd + 2)..];
            while (!headers.IsEmpty) {
                int end = headers.IndexOf("\r\n"u8);
                ReadOnlySpan<byte> line = end < 0 ? headers : headers[..end];
                headers = end < 0 ? default : headers[(end + 2)..];
                int colon = line.IndexOf((byte)':');
                if (colon <= 0) {
                    continue;
                }

                ReadOnlySpan<byte> name = line[..colon];
                ReadOnlySpan<byte> value = line[(colon + 1)..].Trim(" \t"u8);
                if (name.IndexOfAny((byte)' ', (byte)'\t') >= 0) {
                    Close(fd, conn); // whitespace before the colon is invalid and could hide a Content-Length
                    return data.Length;
                }
                if (Ascii.EqualsIgnoreCase(name, "content-length"u8)) {
                    if (!Utf8Parser.TryParse(value, out int length, out int used) || used != value.Length
                        || length < 0 || length > MaxRequestBytes || (sawLength && length != contentLength)) {
                        Close(fd, conn);
                        return data.Length;
                    }
                    contentLength = length;
                    sawLength = true;
                } else if (Ascii.EqualsIgnoreCase(name, "authorization"u8)) {
                    auth = value;
                    hasAuth = true;
                } else if (Ascii.EqualsIgnoreCase(name, "connection"u8)) {
                    if (Ascii.EqualsIgnoreCase(value, "close"u8)) {
                        close = true;
                    } else if (Ascii.EqualsIgnoreCase(value, "keep-alive"u8)) {
                        close = false;
                    }
                } else if (Ascii.EqualsIgnoreCase(name, "transfer-encoding"u8)) {
                    Close(fd, conn); // chunked bodies are out of scope
                    return data.Length;
                }
            }

            int total = headEnd + 4 + contentLength;
            if (rest.Length < total) {
                break;
            }

            Handle(fd, conn, method, target, auth, hasAuth, rest.Slice(headEnd + 4, contentLength), close);
            consumed += total;
        }
        return consumed;
    }

    /// <summary>
    /// Runs one request through the Api inside the batch transaction (started on the first request of
    /// the batch) and records its response in _out. An exception from the Api becomes a 500. Once the
    /// batch transaction has failed to start or been rolled back, later requests are not run and get a 500.
    /// </summary>
    private void Handle(int fd, Conn conn, ReadOnlySpan<byte> method, ReadOnlySpan<byte> target,
        ReadOnlySpan<byte> auth, bool hasAuth, ReadOnlySpan<byte> body, bool close) {
        if (!_batchBroken && !_inBatchTransaction) {
            if (_db.InTransaction) {
                _db.Rollback(); // never run a batch inside a transaction left over from a failure
            }

            _inBatchTransaction = _db.Begin();
            _batchBroken = !_inBatchTransaction;
        }

        _body.Reset();
        int status = 500;
        if (!_batchBroken) {
            try {
                status = _api.Handle(method, target, auth, hasAuth, body, _body);
            } catch (Exception ex) {
                Console.Error.WriteLine(ex);
                status = 500;
            }
            // SQLite rolls the whole transaction back on some errors; anything after that would autocommit.
            if (!_db.InTransaction) {
                _batchBroken = true;
            }
        }
        if (status == 500) {
            _body.Reset();
            _body.Bytes("{\"error\":\"internal server error\"}"u8);
        }

        int start = _out.Len;
        WriteResponse(status, _body.Written, close);
        if (_segmentCount == _segments.Length) {
            Array.Resize(ref _segments, _segments.Length * 2);
        }

        _segments[_segmentCount++] = new Segment {
            Fd = fd,
            Generation = conn.Generation,
            Start = start,
            End = _out.Len,
            Close = close,
        };
        if (close) {
            conn.CloseWhenFlushed = true;
        }
    }

    /// <summary>The batch did not commit or was rolled back: every response in it is rewritten as a 500.</summary>
    private void FailBatch() {
        for (int i = 0; i < _segmentCount; i++) {
            ref Segment s = ref _segments[i];
            s.Start = _out.Len;
            WriteResponse(500, "{\"error\":\"internal server error\"}"u8, s.Close);
            s.End = _out.Len;
        }
    }

    /// <summary>
    /// Sends the batch's responses. Skips connections that closed since, and queues behind bytes
    /// already pending so responses stay in order.
    /// </summary>
    private void SendBatch() {
        for (int i = 0; i < _segmentCount; i++) {
            ref Segment s = ref _segments[i];
            Conn? conn = _conns[s.Fd];
            if (conn is null || !conn.Open || conn.Generation != s.Generation) {
                continue;
            }

            ReadOnlySpan<byte> bytes = _out.Buf.AsSpan(s.Start, s.End - s.Start);
            if (conn.PendingLength > 0) {
                Queue(conn, bytes);
            } else {
                Send(s.Fd, conn, bytes);
            }

            if (conn.PendingLength > MaxPendingBytes) {
                Close(s.Fd, conn); // the client is not reading
            } else if (s.Close && conn.PendingLength == 0) {
                Close(s.Fd, conn);
            }
        }
    }

    /// <summary>Sends now; whatever the socket will not take is queued and EPOLLOUT is switched on.</summary>
    private void Send(int fd, Conn conn, ReadOnlySpan<byte> bytes) {
        nint sent;
        fixed (byte* p = bytes) {
            sent = send(fd, p, bytes.Length, MSG_NOSIGNAL);
        }

        if (sent == bytes.Length) {
            return;
        }

        if (sent < 0) {
            if (Marshal.GetLastPInvokeError() is not (EAGAIN or EINTR)) {
                Close(fd, conn);
                return;
            }
            sent = 0;
        }
        Queue(conn, bytes[(int)sent..]);
        Watch(EPOLL_CTL_MOD, fd, EPOLLIN | EPOLLOUT);
    }

    /// <summary>Appends bytes to the connection's pending buffer, growing it if needed.</summary>
    private static void Queue(Conn conn, ReadOnlySpan<byte> bytes) {
        if (conn.Pending is null) {
            conn.Pending = ArrayPool<byte>.Shared.Rent(bytes.Length);
            conn.PendingStart = conn.PendingLength = 0;
        } else if (conn.Pending.Length - conn.PendingStart - conn.PendingLength < bytes.Length) {
            byte[] bigger = ArrayPool<byte>.Shared.Rent(conn.PendingLength + bytes.Length);
            conn.Pending.AsSpan(conn.PendingStart, conn.PendingLength).CopyTo(bigger);
            ArrayPool<byte>.Shared.Return(conn.Pending);
            conn.Pending = bigger;
            conn.PendingStart = 0;
        }
        bytes.CopyTo(conn.Pending.AsSpan(conn.PendingStart + conn.PendingLength));
        conn.PendingLength += bytes.Length;
    }

    /// <summary>Writes pending bytes after EPOLLOUT. When drained, stops watching for EPOLLOUT and closes if asked to.</summary>
    private void Flush(int fd, Conn conn) {
        if (conn.PendingLength == 0) {
            return;
        }

        nint sent;
        fixed (byte* p = conn.Pending) {
            sent = send(fd, p + conn.PendingStart, conn.PendingLength, MSG_NOSIGNAL);
        }

        if (sent < 0) {
            if (Marshal.GetLastPInvokeError() is not (EAGAIN or EINTR)) {
                Close(fd, conn);
            }

            return;
        }
        conn.PendingStart += (int)sent;
        conn.PendingLength -= (int)sent;
        if (conn.PendingLength > 0) {
            return;
        }

        ArrayPool<byte>.Shared.Return(conn.Pending!);
        conn.Pending = null;
        if (conn.CloseWhenFlushed) {
            Close(fd, conn);
        } else {
            Watch(EPOLL_CTL_MOD, fd, EPOLLIN);
        }
    }

    /// <summary>Closes the socket and returns the connection's buffers. Safe to call twice.</summary>
    private void Close(int fd, Conn conn) {
        if (!conn.Open) {
            return;
        }

        conn.Open = false;
        conn.CloseWhenFlushed = false;
        if (conn.Input is not null) {
            ArrayPool<byte>.Shared.Return(conn.Input);
        }

        if (conn.Pending is not null) {
            ArrayPool<byte>.Shared.Return(conn.Pending);
        }

        conn.Input = conn.Pending = null;
        conn.InputLength = conn.PendingLength = conn.PendingStart = 0;
        close(fd); // also removes it from the epoll set
        if (_acceptPaused) {
            _acceptPaused = false;
            Watch(EPOLL_CTL_ADD, _listen, EPOLLIN);
        }
    }

    /// <summary>Writes a full response into _out. Statuses without a case below get the 500 status line.</summary>
    private void WriteResponse(int status, ReadOnlySpan<byte> body, bool close) {
        _out.Bytes(status switch {
            200 => "HTTP/1.1 200 OK\r\n"u8,
            201 => "HTTP/1.1 201 Created\r\n"u8,
            400 => "HTTP/1.1 400 Bad Request\r\n"u8,
            401 => "HTTP/1.1 401 Unauthorized\r\n"u8,
            404 => "HTTP/1.1 404 Not Found\r\n"u8,
            503 => "HTTP/1.1 503 Service Unavailable\r\n"u8,
            _ => "HTTP/1.1 500 Internal Server Error\r\n"u8,
        });
        _out.Bytes("Content-Type: application/json\r\nDate: "u8);
        _out.Bytes(Date());
        _out.Bytes("\r\nContent-Length: "u8);
        _out.Int(body.Length);
        _out.Bytes(close ? "\r\nConnection: close\r\n\r\n"u8 : "\r\n\r\n"u8);
        _out.Bytes(body);
    }

    /// <summary>The Date header value, reformatted at most once per second.</summary>
    private ReadOnlySpan<byte> Date() {
        long second = Environment.TickCount64 / 1000;
        if (second != _dateSecond) {
            _dateSecond = second;
            Utf8Formatter.TryFormat(DateTime.UtcNow, _date, out _, new StandardFormat('R'));
        }
        return _date;
    }

    /// <summary>Adds or changes an fd's epoll registration. The fd is stored as the event data. Failure is logged only.</summary>
    private void Watch(int op, int fd, uint events) {
        EpollEvent ev = new() { Events = events, Data = (ulong)fd };
        if (epoll_ctl(_epoll, op, fd, &ev) < 0) {
            Console.Error.WriteLine($"epoll_ctl({op}, {fd}) failed: errno {Marshal.GetLastPInvokeError()}");
        }
    }

    /// <summary>Throws with the errno of the failed system call.</summary>
    private static void Fail(string what) =>
        throw new InvalidOperationException($"{what} failed: errno {Marshal.GetLastPInvokeError()}");
}

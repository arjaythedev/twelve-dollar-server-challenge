using System.Buffers;
using System.Buffers.Binary;
using System.Buffers.Text;
using System.Net;
using System.Runtime.InteropServices;
using System.Text;
using static Feed.Native;

namespace Feed;

/// <summary>
/// Per-connection state, one per (ring, slot) and reused. Idle connections hold no buffers.
/// The socket is a direct descriptor: slot <see cref="Slot"/> of the ring's file table, not a process fd.
/// </summary>
internal sealed unsafe class Conn {
    public Ring Ring = null!;
    public int Slot;
    public int Id;             // ring index << 20 | slot, the connection's name in Segments
    public bool InUse;         // the slot is taken, from accept until its file table entry is closed
    public bool Open;          // accepted and not closed by us or the client
    public int Generation;     // bumped on every accept, so a reused slot is not mistaken for the old connection
    public bool Receiving;     // a multishot receive is armed and has not ended
    public bool Sending;       // a send is in flight: FPtr belongs to the kernel
    public bool ShutdownSent;  // the shutdown that ends the receive has been queued
    public bool InSendList;    // already listed for sending in this batch
    public bool Answering;     // has a response in the current batch that is not queued yet
    public bool CloseWhenFlushed; // close once the response is sent and the queue is drained
    public byte[]? Input;      // a partial request carried over to the next read (pooled)
    public int InputLength;
    public byte* QPtr;         // output waiting for the current send to finish (native, not yet submitted)
    public int QLen, QCap;
    public byte* FPtr;         // output the kernel is sending; never moved or freed while Sending
    public int FLen, FCap, FSent;
}

/// <summary>
/// A single-threaded HTTP/1.1 server on io_uring. Sockets are accepted straight into each ring's
/// registered file table, so they never use a process fd and the fd limit does not cap connections.
/// Each loop pass is one batch: every complete request in all completions gathered in it runs inside
/// one SQLite transaction, the batch commits once, and only then are the responses queued for
/// sending. So a 201 always follows its commit (rule 6), and writes from many requests share one
/// commit (group commit).
/// </summary>
internal sealed unsafe class Server {
    private const int MaxRequestBytes = 1 << 20; // largest body, and largest partial request kept
    private const int MaxPendingBytes = 4 << 20; // unsent output a client may build up before it is dropped
    private const int SmallBufSize = 2 * 1024;   // send buffer for a post, like or error response
    private const int LargeBufSize = 16 * 1024;  // send buffer for a feed response
    private const int SmallPoolMax = 8192;       // most small send buffers kept for reuse (16 MiB)
    private const int LargePoolMax = 2048;       // most large send buffers kept for reuse (32 MiB)
    private const int MaxRings = 8;
    private const int TargetSlots = 524280;      // connections wanted across all rings

    private const int OpAccept = 1, OpRecv = 2, OpSend = 3, OpIgnore = 4; // low byte of user_data

    /// <summary>One response in _out, at [Start, End), waiting for the batch to commit. Generation must still match the connection's when it is sent.</summary>
    private struct Segment {
        public int Id, Generation, Start, End;
        public bool Close;
    }

    /// <summary>A connection whose receive ended for lack of buffers and must be armed again.</summary>
    private struct Retry {
        public int Id, Generation;
    }

    private readonly Api _api;
    private readonly Db _db;
    private readonly int _listen;
    private readonly Ring[] _rings;
    private readonly int _reserve, _resume; // free slots below which a ring stops accepting, and at which it starts again
    private Segment[] _segments = new Segment[1024];
    private int _segmentCount;
    private int[] _sendList = new int[1024];
    private int _sendCount;
    private readonly List<Retry> _retry = new();
    private bool _needTimer;     // work was deferred (retry, backlog, accept back-off): do not wait long
    private bool _firstRingShort; // ring 0 kept entries the kernel did not take on the last wait
    private readonly nint[] _smallPool = new nint[SmallPoolMax];
    private readonly nint[] _largePool = new nint[LargePoolMax];
    private int _smallCount, _largeCount;
    private readonly Out _out = new(1 << 20);   // all responses of the current batch
    private readonly Out _body = new(64 * 1024); // body of the response being built
    private readonly byte[] _date = new byte[29]; // cached Date header value (RFC 1123, 29 bytes)
    private long _dateSecond = -1;
    private bool _inBatchTransaction;
    private bool _batchBroken;   // the batch transaction failed or was rolled back: answer the rest with 500s

    /// <summary>
    /// Opens a non-blocking IPv4 listening socket and the io_uring rings. <paramref name="host"/> must be an IP address, not a name. Throws if any system call fails.
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

        // A file table holds at most RLIMIT_NOFILE entries, so raise the soft limit to the hard one.
        RLimit limit = default;
        if (getrlimit(RLIMIT_NOFILE, &limit) < 0) {
            Fail("getrlimit");
        }

        if (limit.Cur < limit.Max) {
            RLimit raised = new() { Cur = limit.Max, Max = limit.Max };
            if (setrlimit(RLIMIT_NOFILE, &raised) == 0) {
                limit = raised;
            }
        }

        int slots = (int)Math.Min(limit.Cur, 65535UL);
        int rings = Math.Clamp((TargetSlots + slots - 1) / slots, 1, MaxRings);
        _reserve = Math.Min(256, slots / 4);
        _resume = Math.Min(512, slots / 2);
        _rings = new Ring[rings];
        for (int i = 0; i < rings; i++) {
            _rings[i] = new Ring(i, slots);
        }

        Console.WriteLine($"io_uring: {rings} rings x {slots} slots, {(_rings[0].Deferred ? "DEFER_TASKRUN" : "no DEFER_TASKRUN")}, {(_rings[0].LegacyBuffers ? "provided buffers" : "buffer ring")}");
    }

    /// <summary>Runs the event loop. Never returns.</summary>
    public void Run() {
        while (true) {
            _out.Reset();
            _segmentCount = 0;
            _inBatchTransaction = false;
            _batchBroken = false;

            int handled = ProcessAll();
            if (_batchBroken) {
                if (_db.InTransaction) {
                    _db.Rollback();
                }

                FailBatch();
            } else if (_inBatchTransaction && !_db.Commit()) {
                FailBatch();
            }
            SendBatch();
            foreach (Ring ring in _rings) {
                ring.PublishBuffers();
            }

            DrainRetries();
            RearmAccepts();
            Wait(handled);
            _db.MaybeCheckpoint();
        }
    }

    /// <summary>
    /// Ends a pass: submits everything queued in it and sleeps until completions are ready. Responses are
    /// queued only after the commit, so this is also where they reach the kernel.
    /// Rings other than 0 are entered without waiting, once per pass, only while they have work. With
    /// DEFER_TASKRUN their completions show up only when we enter them. Ring 0 is the one we sleep on:
    /// it is the only ring that takes connections until it is nearly full, and an idle server has to
    /// sleep on something. While another ring is active the sleep is short, so its completions wait
    /// at most 4 ms.
    /// After a busy pass (4 or more completions of any kind, sends included) the wait is for up to 128
    /// completions or 4 ms, the values C #26 uses, so a burst is batched into one pass. After a quiet
    /// pass the first completion wakes us.
    /// </summary>
    private void Wait(int handled) {
        bool otherActive = false, left = false;
        for (int i = 1; i < _rings.Length; i++) {
            Ring ring = _rings[i];
            if (ring.OpenCount > 0 || ring.AcceptArmed || ring.HasUnsubmitted) {
                otherActive = true;
                ring.Enter(0, 0);
                left |= ring.HasUnsubmitted;
            }
        }

        _needTimer = left || _retry.Count > 0 || NeedsAcceptTimer();
        Ring first = _rings[0];
        if (_firstRingShort) {
            // Last time the kernel did not take every entry (EBUSY while completions overflow). It then
            // returns at once without waiting, so submit what it takes and pause instead of spinning.
            first.Enter(0, 0);
            Thread.Sleep(1);
        } else if (handled >= 4) {
            first.Enter(128, 4_000_000);
        } else {
            long timeoutNs = otherActive ? 4_000_000 : _needTimer ? 5_000_000 : 1_000_000_000;
            first.Enter(1, timeoutNs);
        }

        _firstRingShort = first.HasUnsubmitted;
    }

    /// <summary>Processes the completions waiting on every ring and returns how many there were.</summary>
    private int ProcessAll() {
        int total = 0;
        foreach (Ring ring in _rings) {
            ring.FlushOverflow();
            uint head = *ring.CqHead;
            uint tail = Volatile.Read(ref *ring.CqTail);
            total += (int)(tail - head);
            while (head != tail) {
                Cqe cqe = ring.Cqes[head & ring.CqMask];
                head++;
                Dispatch(ring, cqe.UserData, cqe.Res, cqe.Flags);
            }
            Volatile.Write(ref *ring.CqHead, head);
        }
        return total;
    }

    /// <summary>Builds the user_data of an operation: kind, ring, slot and the connection's generation.</summary>
    private static ulong Tag(int op, int ring, int slot, int generation) =>
        (ulong)(uint)op | ((ulong)(uint)ring << 8) | ((ulong)(uint)slot << 12) | ((ulong)(uint)generation << 32);

    /// <summary>Routes one completion by the kind in its user_data.</summary>
    private void Dispatch(Ring ring, ulong userData, int res, uint flags) {
        int slot = (int)((userData >> 12) & 0xFFFFF);
        int generation = (int)(userData >> 32);
        switch ((int)(userData & 0xFF)) {
            case OpAccept:
                OnAccept(ring, res, flags);
                break;
            case OpRecv:
                OnRecv(ring, slot, generation, res, flags);
                break;
            case OpSend:
                OnSend(ring, slot, generation, res);
                break;
        }
    }

    /// <summary>The connection in a slot if it is the instance the completion belongs to; otherwise null.</summary>
    private static Conn? Find(Ring ring, int slot, int generation) {
        Conn? conn = slot < ring.Conns.Length ? ring.Conns[slot] : null;
        return conn is not null && conn.InUse && conn.Generation == generation ? conn : null;
    }

    /// <summary>
    /// A multishot accept completed. A result of 0 or more is the file table slot of a new connection.
    /// Without the MORE flag the accept has ended; after an error it waits 100 ms before being armed again.
    /// </summary>
    private void OnAccept(Ring ring, int res, uint flags) {
        if ((flags & IORING_CQE_F_MORE) == 0) {
            ring.AcceptArmed = false;
            ring.AcceptCancelling = false;
            if (res < 0 && res != -ECANCELED) {
                ring.AcceptErrors++;
                ring.AcceptRetryAt = Environment.TickCount64 + 100;
            }
        }

        if (res < 0 || res >= ring.Slots) {
            return;
        }

        if (res >= ring.Conns.Length) {
            Array.Resize(ref ring.Conns, Math.Min(Math.Max(ring.Conns.Length * 2, res + 1), ring.Slots));
        }

        Conn conn = ring.Conns[res] ??= new Conn { Ring = ring, Slot = res, Id = ring.Index << 20 | res };
        conn.InUse = true;
        conn.Open = true;
        conn.Generation++;
        ring.OpenCount++;
        ArmRecv(conn);
    }

    /// <summary>Starts a multishot receive on the connection's slot.</summary>
    private static void ArmRecv(Conn conn) {
        conn.Receiving = true;
        conn.Ring.PrepRecv(conn.Slot, Tag(OpRecv, conn.Ring.Index, conn.Slot, conn.Generation));
    }

    /// <summary>
    /// A receive completed. Data is handled, then its buffer goes back. An error closes the connection.
    /// EOF closes it too, unless responses are still to go out: then it closes once they are sent, as a
    /// client that shuts down its sending side still expects its answers. When the receive ends (no MORE
    /// flag) on an open connection it is armed again, except for ENOBUFS, which waits in the retry queue
    /// until buffers are back.
    /// </summary>
    private void OnRecv(Ring ring, int slot, int generation, int res, uint flags) {
        Conn? conn = Find(ring, slot, generation);
        bool more = (flags & IORING_CQE_F_MORE) != 0;
        int bid = (flags & IORING_CQE_F_BUFFER) != 0 ? (int)(flags >> 16) : -1;
        if (conn is not null && !more) {
            conn.Receiving = false;
        }

        if (conn is not null && conn.Open) {
            if (res > 0 && bid >= 0) {
                Receive(conn, ring.BufBase + (long)bid * Ring.BufSize, res);
            } else if (res == 0 && (conn.Answering || conn.Sending || conn.QLen > 0)) {
                conn.CloseWhenFlushed = true;
            } else if (res != -ENOBUFS) {
                Close(conn);
            }

            if (!more && conn.Open && !conn.CloseWhenFlushed) {
                if (res == -ENOBUFS) {
                    _retry.Add(new Retry { Id = conn.Id, Generation = conn.Generation });
                } else {
                    ArmRecv(conn);
                }
            }
        }

        if (bid >= 0) {
            ring.ReturnBuffer(bid);
        }

        if (conn is not null && !more) {
            TryRelease(conn);
        }
    }

    /// <summary>
    /// Arms again the receives that ended for lack of buffers. Every buffer has been handed back by now
    /// (with classic provided buffers, the kernel takes them in order before these receives), but at
    /// most 2,048 are armed per pass so they cannot use up the buffers in one burst again.
    /// </summary>
    private void DrainRetries() {
        if (_retry.Count == 0) {
            return;
        }

        int n = Math.Min(2048, _retry.Count);
        for (int i = 0; i < n; i++) {
            Retry r = _retry[i];
            Ring ring = _rings[r.Id >> 20];
            Conn? conn = Find(ring, r.Id & 0xFFFFF, r.Generation);
            if (conn is not null && conn.Open && !conn.Receiving) {
                ArmRecv(conn);
            }
        }

        _retry.RemoveRange(0, n);
    }

    /// <summary>
    /// Keeps one ring accepting at a time: the lowest ring that is armed with enough free slots, or
    /// unarmed with at least the resume level free. That ring's accept is cancelled below the reserve,
    /// then the next ring takes over. Any other armed accept is cancelled, so idle rings stay idle.
    /// A back-off after an accept error delays arming.
    /// </summary>
    private void RearmAccepts() {
        bool haveOwner = false;
        foreach (Ring ring in _rings) {
            int free = ring.Slots - ring.OpenCount;
            if (ring.AcceptArmed) {
                if (ring.AcceptCancelling) {
                    continue;
                }

                if (!haveOwner && free >= _reserve) {
                    haveOwner = true;
                } else {
                    ring.AcceptCancelling = true;
                    ring.PrepCancel(Tag(OpAccept, ring.Index, 0, 0), Tag(OpIgnore, ring.Index, 0, 0));
                }
            } else if (!haveOwner && free >= _resume) {
                haveOwner = true;
                if (Environment.TickCount64 >= ring.AcceptRetryAt) {
                    ring.AcceptArmed = true;
                    ring.PrepAccept(_listen, Tag(OpAccept, ring.Index, 0, 0));
                }
            }
        }
    }

    /// <summary>True while some ring is waiting out an accept back-off.</summary>
    private bool NeedsAcceptTimer() {
        foreach (Ring ring in _rings) {
            if (!ring.AcceptArmed && ring.Slots - ring.OpenCount >= _resume && Environment.TickCount64 < ring.AcceptRetryAt) {
                return true;
            }
        }
        return false;
    }

    /// <summary>
    /// Handles the bytes of one receive: every complete request in them, with leftovers of a partial
    /// request kept in conn.Input. Closes when a partial request exceeds MaxRequestBytes.
    /// </summary>
    private void Receive(Conn conn, byte* data, int got) {
        if (conn.Input is null) {
            int used = Parse(conn.Id, conn, new ReadOnlySpan<byte>(data, got));
            if (!conn.Open || conn.CloseWhenFlushed) {
                return;
            }

            if (used < got) {
                Carry(conn, new ReadOnlySpan<byte>(data + used, got - used));
            }
        } else {
            if (conn.Input.Length - conn.InputLength < got) {
                byte[] bigger = ArrayPool<byte>.Shared.Rent(conn.InputLength + got);
                conn.Input.AsSpan(0, conn.InputLength).CopyTo(bigger);
                ArrayPool<byte>.Shared.Return(conn.Input);
                conn.Input = bigger;
            }

            new ReadOnlySpan<byte>(data, got).CopyTo(conn.Input.AsSpan(conn.InputLength));
            conn.InputLength += got;
            int used = Parse(conn.Id, conn, conn.Input.AsSpan(0, conn.InputLength));
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
                    Close(conn);
                }
            }
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
    private int Parse(int id, Conn conn, ReadOnlySpan<byte> data) {
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
                Close(conn);
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
                    Close(conn); // whitespace before the colon is invalid and could hide a Content-Length
                    return data.Length;
                }
                if (Ascii.EqualsIgnoreCase(name, "content-length"u8)) {
                    if (!Utf8Parser.TryParse(value, out int length, out int used) || used != value.Length
                        || length < 0 || length > MaxRequestBytes || (sawLength && length != contentLength)) {
                        Close(conn);
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
                    Close(conn); // chunked bodies are out of scope
                    return data.Length;
                }
            }

            int total = headEnd + 4 + contentLength;
            if (rest.Length < total) {
                break;
            }

            Handle(id, conn, method, target, auth, hasAuth, rest.Slice(headEnd + 4, contentLength), close);
            consumed += total;
        }
        return consumed;
    }

    /// <summary>
    /// Runs one request through the Api inside the batch transaction (started on the first request of
    /// the batch) and records its response in _out. An exception from the Api becomes a 500. Once the
    /// batch transaction has failed to start or been rolled back, later requests are not run and get a 500.
    /// </summary>
    private void Handle(int id, Conn conn, ReadOnlySpan<byte> method, ReadOnlySpan<byte> target,
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
            Id = id,
            Generation = conn.Generation,
            Start = start,
            End = _out.Len,
            Close = close,
        };
        conn.Answering = true;
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
    /// Queues the batch's responses and starts a send for every connection that has output and no send
    /// in flight. Skips connections that closed since, or whose slot was reused (generation differs).
    /// A connection that builds up more than MaxPendingBytes unsent is closed.
    /// </summary>
    private void SendBatch() {
        _sendCount = 0;
        for (int i = 0; i < _segmentCount; i++) {
            ref Segment s = ref _segments[i];
            Ring ring = _rings[s.Id >> 20];
            Conn? conn = Find(ring, s.Id & 0xFFFFF, s.Generation);
            if (conn is null) {
                continue;
            }

            conn.Answering = false;
            if (!conn.Open) {
                continue;
            }

            Enqueue(conn, _out.Buf.AsSpan(s.Start, s.End - s.Start));
            if (conn.QLen + (conn.Sending ? conn.FLen - conn.FSent : 0) > MaxPendingBytes) {
                Close(conn); // the client is not reading
            } else if (!conn.InSendList) {
                conn.InSendList = true;
                if (_sendCount == _sendList.Length) {
                    Array.Resize(ref _sendList, _sendList.Length * 2);
                }

                _sendList[_sendCount++] = s.Id;
            }
        }

        for (int i = 0; i < _sendCount; i++) {
            int id = _sendList[i];
            Conn conn = _rings[id >> 20].Conns[id & 0xFFFFF]!;
            conn.InSendList = false;
            StartSend(conn);
        }
    }

    /// <summary>Appends bytes to the connection's queued output, growing the buffer if needed.</summary>
    private void Enqueue(Conn conn, ReadOnlySpan<byte> bytes) {
        if (conn.QPtr is null) {
            conn.QPtr = GetBuf(bytes.Length, out conn.QCap);
            conn.QLen = 0;
        } else if (conn.QCap - conn.QLen < bytes.Length) {
            byte* bigger = GetBuf(Math.Max(conn.QCap * 2, conn.QLen + bytes.Length), out int cap);
            Buffer.MemoryCopy(conn.QPtr, bigger, cap, conn.QLen);
            PutBuf(conn.QPtr, conn.QCap);
            conn.QPtr = bigger;
            conn.QCap = cap;
        }

        bytes.CopyTo(new Span<byte>(conn.QPtr + conn.QLen, bytes.Length));
        conn.QLen += bytes.Length;
    }

    /// <summary>
    /// Turns the queued output into the in-flight buffer and submits a send for it, if the connection is
    /// open, idle and has output. The buffer changes owner without a copy.
    /// </summary>
    private static void StartSend(Conn conn) {
        if (!conn.Open || conn.Sending || conn.QLen == 0) {
            return;
        }

        conn.FPtr = conn.QPtr;
        conn.FCap = conn.QCap;
        conn.FLen = conn.QLen;
        conn.FSent = 0;
        conn.QPtr = null;
        conn.QCap = conn.QLen = 0;
        conn.Sending = true;
        conn.Ring.PrepSend(conn.Slot, conn.FPtr, conn.FLen, Tag(OpSend, conn.Ring.Index, conn.Slot, conn.Generation));
    }

    /// <summary>
    /// A send completed. A short send goes out again from where it stopped. A finished one frees its buffer,
    /// then starts the next queued send or, if the connection was told to close, closes it. A send error closes it.
    /// </summary>
    private void OnSend(Ring ring, int slot, int generation, int res) {
        Conn? conn = Find(ring, slot, generation);
        if (conn is null || !conn.Sending) {
            return;
        }

        if (res > 0) {
            conn.FSent += res;
        }

        if (conn.Open && (res > 0 || res is -EAGAIN or -EINTR)) {
            if (conn.FSent < conn.FLen) {
                ring.PrepSend(slot, conn.FPtr + conn.FSent, conn.FLen - conn.FSent, Tag(OpSend, ring.Index, slot, generation));
                return;
            }

            conn.Sending = false;
            FreeInFlight(conn);
            if (conn.QLen > 0) {
                StartSend(conn);
            } else if (conn.CloseWhenFlushed) {
                Close(conn);
            }

            return;
        }

        conn.Sending = false;
        FreeInFlight(conn);
        Close(conn);   // also releases the slot when the connection was already closed
        TryRelease(conn);
    }

    /// <summary>Returns the in-flight buffer to the pool. Only call it when the kernel no longer owns it.</summary>
    private void FreeInFlight(Conn conn) {
        if (conn.FPtr is not null) {
            PutBuf(conn.FPtr, conn.FCap);
            conn.FPtr = null;
        }

        conn.FLen = conn.FSent = conn.FCap = 0;
    }

    /// <summary>
    /// Marks the connection closed and drops its input and queued output. A pending receive or send is ended
    /// with a shutdown, so a client that stopped reading cannot hold the slot. The slot itself is closed by
    /// <see cref="TryRelease"/> once no operation uses it. Safe to call twice.
    /// </summary>
    private void Close(Conn conn) {
        if (!conn.Open) {
            return;
        }

        conn.Open = false;
        conn.CloseWhenFlushed = false;
        conn.Answering = false;
        if (conn.Input is not null) {
            ArrayPool<byte>.Shared.Return(conn.Input);
        }

        conn.Input = null;
        conn.InputLength = 0;
        if (conn.QPtr is not null) {
            PutBuf(conn.QPtr, conn.QCap);
        }

        conn.QPtr = null;
        conn.QLen = conn.QCap = 0;
        if ((conn.Receiving || conn.Sending) && !conn.ShutdownSent) {
            conn.ShutdownSent = true;
            conn.Ring.PrepShutdown(conn.Slot, Tag(OpIgnore, conn.Ring.Index, conn.Slot, conn.Generation));
        }

        TryRelease(conn);
    }

    /// <summary>
    /// Closes the slot in the ring's file table and resets the connection for reuse, but only when the
    /// connection is closed and neither a receive nor a send is still running on it.
    /// </summary>
    private void TryRelease(Conn conn) {
        if (!conn.InUse || conn.Open || conn.Receiving || conn.Sending) {
            return;
        }

        conn.InUse = false;
        conn.ShutdownSent = false;
        conn.Ring.PrepClose(conn.Slot, Tag(OpIgnore, conn.Ring.Index, conn.Slot, conn.Generation));
        conn.Ring.OpenCount--;
    }

    /// <summary>
    /// A native send buffer of at least <paramref name="need"/> bytes: a small or large pooled one when it
    /// fits, so a short response does not hold a feed-sized buffer, otherwise an exact allocation.
    /// </summary>
    private byte* GetBuf(int need, out int cap) {
        if (need <= SmallBufSize) {
            cap = SmallBufSize;
            return _smallCount > 0 ? (byte*)_smallPool[--_smallCount] : (byte*)NativeMemory.Alloc(SmallBufSize);
        }

        if (need <= LargeBufSize) {
            cap = LargeBufSize;
            return _largeCount > 0 ? (byte*)_largePool[--_largeCount] : (byte*)NativeMemory.Alloc(LargeBufSize);
        }

        cap = need;
        return (byte*)NativeMemory.Alloc((nuint)need);
    }

    /// <summary>Keeps a pooled-size send buffer for reuse while its pool has room; frees any other.</summary>
    private void PutBuf(byte* buf, int cap) {
        if (cap == SmallBufSize && _smallCount < SmallPoolMax) {
            _smallPool[_smallCount++] = (nint)buf;
        } else if (cap == LargeBufSize && _largeCount < LargePoolMax) {
            _largePool[_largeCount++] = (nint)buf;
        } else {
            NativeMemory.Free(buf);
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

    /// <summary>Throws with the errno of the failed system call.</summary>
    private static void Fail(string what) =>
        throw new InvalidOperationException($"{what} failed: errno {Marshal.GetLastPInvokeError()}");
}

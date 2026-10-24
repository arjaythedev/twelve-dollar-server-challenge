using System.Buffers.Text;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using static Feed.Native;

namespace Feed;

/// <summary>
/// One io_uring ring and the thread that drives it. Accepted sockets live in the ring's own sparse
/// table of direct descriptors (multishot accept with IORING_FILE_INDEX_ALLOC), so they never use
/// the process's file descriptors: several rings of 65,535 slots each hold far more keep-alive
/// connections than LimitNOFILE=65535 would allow. Receives are multishot into a provided-buffer
/// ring, so an idle connection owns only its 88-byte <see cref="Conn"/> record.
///
/// Each loop: wait for completions (32, or 250 µs under load), reap them, then under App.Gate
/// parse and run every complete request in one transaction, commit once, and queue the responses.
/// </summary>
internal sealed unsafe class Worker {
    private const uint RingEntries = 4096;
    private const uint CqEntries = 16384;
    private const int BufCount = 2048;               // provided receive buffers
    private const int BufSize = 2048;
    private const int MaxHeaderBytes = 64 * 1024;    // largest request head kept while incomplete
    private const int MaxRequestBytes = 1 << 20;     // largest body
    private const int OutputHigh = 64 * 1024;        // unsent output that pauses a connection's parsing
    private const int BlockSize = 16 * 1024;         // pooled buffer size
    private const int MaxPooled = 1024;
    private const int ReapBatch = 1024;
    private const long StallMs = 120_000;            // partial input or blocked output this old is dropped
    private const long SweepMs = 10_000;

    // Under load each loop waits for this many completions or this long, whichever comes first.
    // Every wake-up has a fixed cost (syscall, lock, read transaction, submitting the sends), so
    // larger batches cut CPU per request: 55.5 µs at 250 µs/32 against 35.1 µs at 4 ms/128 on an
    // i7-3770 at 100,000 users, with p99 still about 10 ms.
    private const long WaitNs = 4_000_000;
    private const int MinComplete = 128;

    // user_data = op | slot << 8 | generation << 32
    private const uint OpAccept = 1, OpRecv = 2, OpSend = 3, OpShutdown = 4, OpClose = 5, OpCancel = 6, OpWake = 7;

    private const int FOpen = 1 << 0;
    private const int FRecvArmed = 1 << 1;
    private const int FSending = 1 << 2;
    private const int FCloseAfterSend = 1 << 3;   // last response queued: close once it is out
    private const int FClosing = 1 << 4;
    private const int FShutdown = 1 << 5;
    private const int FPaused = 1 << 6;           // too much unsent output: stopped parsing
    private const int FReady = 1 << 7;            // queued to resume parsing
    private const int FContinueSent = 1 << 8;     // sent "100 Continue" for the current request
    private const int FDeferred = 1 << 9;         // a write is with the Writer: stop parsing until it answers

    /// <summary>Per-connection state, indexed by direct descriptor slot.</summary>
    private struct Conn {
        public uint Gen;
        public int Flags;
        public byte* In;     // bytes of an incomplete request
        public int InLen, InCap;
        public byte* Fl;     // the buffer the kernel is sending
        public int FlLen, FlSent, FlCap;
        public byte* Pe;     // responses waiting for the current send
        public int PeLen, PeCap;
        public long Active;  // last receive or send, Environment.TickCount64
        public int BatchSeq, BatchBytes;
    }

    private struct RecvItem {
        public int Slot, Bid, Len;
        public uint Gen;
    }

    private struct Segment {
        public int Slot, Start, End;
        public uint Gen;
        public bool Close;
    }

    private struct SlotRef {
        public int Slot;
        public uint Gen;
    }

    private readonly int _id;
    private readonly App _app;
    private readonly int _listen;
    private readonly int _slots;
    private readonly int _reserve;
    private Ring _ring = null!;
    private BufRing _bufs = null!;
    private Conn* _conns;
    private Cqe* _cqes;
    private int _open;
    private bool _acceptArmed, _acceptCancelling;
    private uint _acceptGen;
    private long _now, _nextSweep, _nextMem;
    private int _batchSeq;

    private RecvItem[] _recv = new RecvItem[4096];
    private int _recvCount;
    private SlotRef[] _ready = new SlotRef[1024];
    private int _readyCount;
    private SlotRef[] _rearm = new SlotRef[1024];
    private int _rearmCount;
    private SlotRef[] _eof = new SlotRef[1024];
    private int _eofCount;
    private Segment[] _segments = new Segment[4096];
    private int _segmentCount;
    private int[] _touched = new int[4096];
    private int _touchedCount;
    private readonly Out _out = new(1 << 20);    // all responses of the current batch
    private readonly Out _body = new(64 * 1024); // body of the response being built
    private readonly nint[] _pool = new nint[MaxPooled];
    private int _pooled;

    // Write responses handed back by the Writer thread, and the eventfd it rings.
    private readonly Lock _doneGate = new();
    private List<WriteJob> _done = new(256);
    private List<WriteJob> _doneWork = new(256);
    private volatile bool _hasDone;
    private int _wakeFd = -1;
    private ulong* _wakeBuf;
    private readonly Out _resp = new(16 * 1024);

    public Worker(int id, App app, int listen, int slots) {
        _id = id;
        _app = app;
        _listen = listen;
        _slots = slots;
        _reserve = Math.Min(1024, slots / 8);
    }

    /// <summary>Sets the ring up on the calling thread (it must be the ring's only user) and serves forever.</summary>
    public void Run() {
        _ring = new Ring(RingEntries, CqEntries);
        _ring.RegisterSparseFiles((uint)_slots);
        _bufs = new BufRing(_ring, BufCount, BufSize, 0);
        _conns = (Conn*)NativeMemory.AllocZeroed((nuint)_slots, (nuint)sizeof(Conn));
        _cqes = (Cqe*)NativeMemory.Alloc(ReapBatch, (nuint)sizeof(Cqe));
        _wakeBuf = (ulong*)NativeMemory.Alloc(8);
        _wakeFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (_wakeFd < 0) {
            throw new InvalidOperationException($"eventfd failed: errno {Marshal.GetLastPInvokeError()}");
        }
        ArmWake();
        _now = Environment.TickCount64;
        _nextSweep = _now + SweepMs;
        UpdateAccept();

        int last = 0;
        Timespec ts;
        while (true) {
            if (_readyCount > 0) {
                _ring.Enter(0, null);
            } else if (last >= 4 || _rearmCount > 0) {
                ts = new Timespec { Sec = 0, Nsec = WaitNs };
                _ring.Enter((uint)Math.Clamp(_open * 2, 1, MinComplete), &ts);
            } else {
                ts = new Timespec { Sec = 1, Nsec = 0 };
                _ring.Enter(1, &ts);
            }

            _now = Environment.TickCount64;
            last = Reap();
            if (_hasDone) {
                DrainDone();
            }
            if (_recvCount > 0 || _readyCount > 0) {
                ProcessBatch();
            }

            for (int i = 0; i < _eofCount; i++) {
                ref Conn c = ref _conns[_eof[i].Slot];
                if (c.Gen == _eof[i].Gen && (c.Flags & FOpen) != 0) {
                    BeginClose(_eof[i].Slot);
                }
            }
            _eofCount = 0;

            int returned = _bufs.Added;
            _bufs.Publish();
            Rearm(Math.Clamp(returned, 32, BufCount / 2));
            Housekeeping();
        }
    }

    // ---- completions ----------------------------------------------------------------------

    /// <summary>Reaps and dispatches every ready completion. Returns how many there were.</summary>
    private int Reap() {
        int total = 0;
        while (true) {
            int n = _ring.Reap(_cqes, ReapBatch);
            for (int i = 0; i < n; i++) {
                Cqe* e = &_cqes[i];
                uint op = (uint)(e->UserData & 0xFF);
                int slot = (int)((e->UserData >> 8) & 0xFFFFFF);
                uint gen = (uint)(e->UserData >> 32);
                switch (op) {
                    case OpRecv: OnRecv(slot, gen, e->Res, e->Flags); break;
                    case OpSend: OnSend(slot, gen, e->Res); break;
                    case OpAccept: OnAccept(gen, e->Res, e->Flags); break;
                    case OpWake: ArmWake(); break; // the responses themselves are picked up by DrainDone
                    default: break; // shutdown/close/cancel only report failures, which change nothing
                }
            }
            total += n;
            if (n < ReapBatch) {
                break;
            }
        }
        if (total > 0) {
            UpdateAccept();
        }

        return total;
    }

    private void OnAccept(uint tag, int res, uint flags) {
        if (tag == _acceptGen && (flags & Ring.IORING_CQE_F_MORE) == 0) {
            _acceptArmed = false;
            _acceptCancelling = false;
        }
        if (res < 0) {
            if (res != -ECANCELED && res != -EAGAIN && res != -EINTR && res != -ENFILE && res != -EMFILE) {
                Log($"worker {_id}: accept failed: errno {-res}");
            }
            return;
        }
        if (res >= _slots) {
            return; // cannot happen: the kernel allocates within the registered table
        }

        ref Conn c = ref _conns[res];
        if (c.Flags != 0) {
            ReleaseBuffers(ref c); // stale record of a slot the kernel already reused
        }

        uint gen = c.Gen + 1;
        c = default;
        c.Gen = gen;
        c.Flags = FOpen;
        c.Active = _now;
        _open++;
        ArmRecv(res);
    }

    private void OnRecv(int slot, uint gen, int res, uint flags) {
        bool hasBuffer = (flags & Ring.IORING_CQE_F_BUFFER) != 0;
        int bid = (int)(flags >> Ring.IORING_CQE_BUFFER_SHIFT);
        ref Conn c = ref _conns[slot];
        if (c.Gen != gen || (c.Flags & FOpen) == 0) {
            if (hasBuffer) {
                _bufs.Return(bid);
            }
            return;
        }

        bool more = (flags & Ring.IORING_CQE_F_MORE) != 0;
        if (!more) {
            c.Flags &= ~FRecvArmed;
        }

        bool closing = (c.Flags & (FClosing | FCloseAfterSend)) != 0;
        if (res > 0 && hasBuffer) {
            c.Active = _now;
            if (closing) {
                _bufs.Return(bid);
            } else {
                Push(ref _recv, ref _recvCount, new RecvItem { Slot = slot, Gen = gen, Bid = bid, Len = res });
                if (!more) {
                    Push(ref _rearm, ref _rearmCount, new SlotRef { Slot = slot, Gen = gen });
                }
            }
        } else {
            if (hasBuffer) {
                _bufs.Return(bid);
            }
            if (res == -ENOBUFS && !closing) {
                Push(ref _rearm, ref _rearmCount, new SlotRef { Slot = slot, Gen = gen }); // out of receive buffers: retry later, bounded
            } else if (!more && (c.Flags & FClosing) == 0) {
                Push(ref _eof, ref _eofCount, new SlotRef { Slot = slot, Gen = gen }); // EOF or error: close after this batch
            }
        }

        if (!more && (c.Flags & FClosing) != 0) {
            ContinueClose(slot);
        }
    }

    private void OnSend(int slot, uint gen, int res) {
        ref Conn c = ref _conns[slot];
        if (c.Gen != gen || (c.Flags & FSending) == 0) {
            return;
        }

        c.Flags &= ~FSending;
        if (res < 0) {
            if (res is -EAGAIN or -EINTR) {
                SubmitSend(slot);
                return;
            }
            FreeBuffer(ref c.Fl, ref c.FlCap);
            FreeBuffer(ref c.Pe, ref c.PeCap);
            c.FlLen = c.PeLen = 0;
            ForceClose(slot);
            return;
        }

        c.Active = _now;
        c.FlSent += res;
        if (c.FlSent < c.FlLen) {
            SubmitSend(slot);
            return;
        }

        FreeBuffer(ref c.Fl, ref c.FlCap);
        c.FlLen = c.FlSent = 0;
        if (c.PeLen > 0) {
            StartSend(slot);
            return;
        }

        if ((c.Flags & FClosing) != 0) {
            ContinueClose(slot);
        } else if ((c.Flags & FCloseAfterSend) != 0) {
            BeginClose(slot);
        } else if ((c.Flags & FPaused) != 0) {
            c.Flags &= ~FPaused;
            AddReady(slot, ref c);
        }
    }

    // ---- request batch --------------------------------------------------------------------

    /// <summary>
    /// Parses and runs every complete request received in this loop (and the resumed ones) in one
    /// transaction, commits it, then hands each connection its responses. Nothing is sent before
    /// the commit, so a 201 always means the row is durable (rule 6).
    /// </summary>
    private void ProcessBatch() {
        _segmentCount = 0;
        _touchedCount = 0;
        _out.Reset();
        _batchSeq++;
        lock (_app.Gate) {
            _app.BeginBatch();
            for (int i = 0; i < _recvCount; i++) {
                ref RecvItem r = ref _recv[i];
                ref Conn c = ref _conns[r.Slot];
                byte* data = _bufs.Buffer(r.Bid);
                if (c.Gen == r.Gen && (c.Flags & FOpen) != 0 && (c.Flags & (FClosing | FCloseAfterSend)) == 0) {
                    if (c.InLen > 0 || (c.Flags & (FPaused | FDeferred)) != 0) {
                        if (Append(ref c, data, r.Len)) {
                            if ((c.Flags & (FPaused | FDeferred)) == 0) {
                                Consume(r.Slot, ref c);
                            }
                        } else {
                            ForceClose(r.Slot);
                        }
                    } else {
                        int used = Parse(r.Slot, ref c, data, r.Len);
                        if (used < r.Len && (c.Flags & (FClosing | FCloseAfterSend)) == 0 && !Append(ref c, data + used, r.Len - used)) {
                            ForceClose(r.Slot);
                        }
                    }
                }
                _bufs.Return(r.Bid);
            }
            _recvCount = 0;

            int ready = _readyCount;
            for (int i = 0; i < ready; i++) {
                ref Conn c = ref _conns[_ready[i].Slot];
                if (c.Gen != _ready[i].Gen) {
                    continue;
                }

                c.Flags &= ~FReady;
                if ((c.Flags & FOpen) != 0 && (c.Flags & (FClosing | FCloseAfterSend | FPaused | FDeferred)) == 0 && c.InLen > 0) {
                    Consume(_ready[i].Slot, ref c);
                }
            }
            // Entries added while resuming (re-paused connections) stay for the next loop.
            Array.Copy(_ready, ready, _ready, 0, _readyCount - ready);
            _readyCount -= ready;

            if (!_app.EndBatch()) {
                FailBatch();
            }

            for (int i = 0; i < _segmentCount; i++) {
                ref Segment s = ref _segments[i];
                ref Conn c = ref _conns[s.Slot];
                if (c.Gen == s.Gen && (c.Flags & FOpen) != 0 && (c.Flags & FClosing) == 0) {
                    QueueOutput(ref c, _out.Buf.AsSpan(s.Start, s.End - s.Start));
                }
            }
        }

        for (int i = 0; i < _touchedCount; i++) {
            int slot = _touched[i];
            ref Conn c = ref _conns[slot];
            if ((c.Flags & FOpen) == 0 || (c.Flags & FSending) != 0) {
                continue;
            }

            if (c.PeLen > 0) {
                StartSend(slot);
            } else if ((c.Flags & FCloseAfterSend) != 0) {
                BeginClose(slot);
            }
        }

    }

    /// <summary>Parses the connection's buffered input and drops what was consumed.</summary>
    private void Consume(int slot, ref Conn c) {
        int used = Parse(slot, ref c, c.In, c.InLen);
        if (used >= c.InLen) {
            c.InLen = 0;
            FreeBuffer(ref c.In, ref c.InCap); // idle connections keep no input buffer
        } else if (used > 0) {
            Buffer.MemoryCopy(c.In + used, c.In, c.InCap, c.InLen - used);
            c.InLen -= used;
        }
    }

    /// <summary>
    /// Runs every complete request in <paramref name="data"/>. Returns the bytes consumed; the rest is
    /// an incomplete request. Adapted from the C# entry by gichamba (PR #74).
    /// </summary>
    private int Parse(int slot, ref Conn c, byte* data, int length) {
        ReadOnlySpan<byte> all = new(data, length);
        int consumed = 0;
        while (consumed < all.Length && (c.Flags & (FCloseAfterSend | FClosing | FDeferred)) == 0) {
            if (c.PeLen + (c.FlLen - c.FlSent) + (c.BatchSeq == _batchSeq ? c.BatchBytes : 0) > OutputHigh) {
                c.Flags |= FPaused; // the client isn't reading: resume when the current send completes
                break;
            }

            ReadOnlySpan<byte> rest = all[consumed..];
            int headEnd = rest.IndexOf("\r\n\r\n"u8);
            if (headEnd < 0) {
                if (rest.Length > MaxHeaderBytes) {
                    BadRequest(slot, ref c);
                    return length;
                }
                break;
            }

            ReadOnlySpan<byte> head = rest[..headEnd];
            int lineEnd = head.IndexOf("\r\n"u8);
            ReadOnlySpan<byte> requestLine = lineEnd < 0 ? head : head[..lineEnd];
            int sp1 = requestLine.IndexOf((byte)' ');
            int sp2 = sp1 < 0 ? -1 : requestLine.LastIndexOf((byte)' ');
            if (sp1 <= 0 || sp2 <= sp1) {
                BadRequest(slot, ref c);
                return length;
            }
            ReadOnlySpan<byte> method = requestLine[..sp1];
            ReadOnlySpan<byte> target = requestLine[(sp1 + 1)..sp2];
            ReadOnlySpan<byte> version = requestLine[(sp2 + 1)..];
            bool http11 = version.SequenceEqual("HTTP/1.1"u8);
            if (!http11 && !version.SequenceEqual("HTTP/1.0"u8)) {
                BadRequest(slot, ref c);
                return length;
            }

            int contentLength = 0;
            bool sawLength = false, hasAuth = false, close = !http11, expectContinue = false;
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
                    BadRequest(slot, ref c); // whitespace before the colon is invalid and could hide a Content-Length
                    return length;
                }
                switch (name.Length) {
                    case 14 when Ascii.EqualsIgnoreCase(name, "content-length"u8):
                        if (!Utf8Parser.TryParse(value, out int len, out int used) || used != value.Length
                            || len < 0 || len > MaxRequestBytes || (sawLength && len != contentLength)) {
                            BadRequest(slot, ref c);
                            return length;
                        }
                        contentLength = len;
                        sawLength = true;
                        break;
                    case 13 when Ascii.EqualsIgnoreCase(name, "authorization"u8):
                        auth = value;
                        hasAuth = true;
                        break;
                    case 10 when Ascii.EqualsIgnoreCase(name, "connection"u8):
                        if (Ascii.EqualsIgnoreCase(value, "close"u8)) {
                            close = true;
                        } else if (Ascii.EqualsIgnoreCase(value, "keep-alive"u8)) {
                            close = false;
                        }
                        break;
                    case 17 when Ascii.EqualsIgnoreCase(name, "transfer-encoding"u8):
                        BadRequest(slot, ref c); // chunked bodies are out of scope
                        return length;
                    case 6 when Ascii.EqualsIgnoreCase(name, "expect"u8):
                        expectContinue = Ascii.EqualsIgnoreCase(value, "100-continue"u8);
                        break;
                }
            }

            int total = headEnd + 4 + contentLength;
            if (rest.Length < total) {
                if (expectContinue && (c.Flags & FContinueSent) == 0 && c.PeLen == 0 && c.FlLen == 0
                    && c.BatchSeq != _batchSeq) {
                    c.Flags |= FContinueSent;
                    QueueOutput(ref c, "HTTP/1.1 100 Continue\r\n\r\n"u8);
                    Touch(slot, ref c);
                }
                break;
            }

            c.Flags &= ~FContinueSent;
            close |= MemGuard.Level >= 2;
            Api api = _app.Api;
            api.Owner = this;
            api.Slot = slot;
            api.Gen = c.Gen;
            api.Close = close;
            int status = _app.Handle(method, target, auth, hasAuth, rest.Slice(headEnd + 4, contentLength), _body);
            consumed += total;
            if (status == Api.Deferred) {
                c.Flags |= FDeferred; // the Writer answers; later requests wait so responses stay in order
                break;
            }
            AddResponse(slot, ref c, status, _body.Written, close);
        }
        return consumed;
    }

    /// <summary>Answers a malformed request with 400 and closes the connection after it.</summary>
    private void BadRequest(int slot, ref Conn c) {
        AddResponse(slot, ref c, 400, "{\"error\":\"bad request\"}"u8, true);
    }

    /// <summary>Records one response of this batch for the connection.</summary>
    private void AddResponse(int slot, ref Conn c, int status, ReadOnlySpan<byte> body, bool close) {
        int start = _out.Len;
        _app.WriteResponse(_out, status, body, close);
        Push(ref _segments, ref _segmentCount, new Segment { Slot = slot, Gen = c.Gen, Start = start, End = _out.Len, Close = close });
        Touch(slot, ref c);
        c.BatchBytes += _out.Len - start;
        if (close) {
            c.Flags |= FCloseAfterSend;
        }
    }

    /// <summary>Notes that the connection has output from this batch.</summary>
    private void Touch(int slot, ref Conn c) {
        if (c.BatchSeq != _batchSeq) {
            c.BatchSeq = _batchSeq;
            c.BatchBytes = 0;
            Push(ref _touched, ref _touchedCount, slot);
        }
    }

    /// <summary>The batch did not commit or was rolled back: every response in it is rewritten as a 500.</summary>
    private void FailBatch() {
        for (int i = 0; i < _segmentCount; i++) {
            ref Segment s = ref _segments[i];
            s.Start = _out.Len;
            _app.WriteResponse(_out, 500, App.InternalError, s.Close);
            s.End = _out.Len;
        }
    }

    // ---- write responses from the Writer -----------------------------------------------------

    /// <summary>Called on the Writer thread: hands a committed write's response back to this worker.</summary>
    public void Complete(WriteJob job) {
        bool ring;
        lock (_doneGate) {
            ring = !_hasDone;
            _done.Add(job);
            _hasDone = true;
        }
        if (ring) {
            ulong one = 1;
            write(_wakeFd, (byte*)&one, 8);
        }
    }

    /// <summary>Keeps one read armed on the eventfd so the Writer's signal wakes this ring.</summary>
    private void ArmWake() {
        Sqe* s = Sqe();
        s->Opcode = Ring.OP_READ;
        s->Fd = _wakeFd;
        s->Addr = (ulong)_wakeBuf;
        s->Len = 8;
        s->Off = ulong.MaxValue;
        s->UserData = Ud(OpWake, 0, 0);
    }

    /// <summary>Queues the Writer's responses on their connections and resumes their parsing.</summary>
    private void DrainDone() {
        lock (_doneGate) {
            (_done, _doneWork) = (_doneWork, _done);
            _hasDone = false;
        }

        _touchedCount = 0;
        _batchSeq++;
        lock (_app.Gate) { // WriteResponse shares the Date cache
            foreach (WriteJob job in _doneWork) {
                ref Conn c = ref _conns[job.Slot];
                if (c.Gen == job.Gen && (c.Flags & FOpen) != 0 && (c.Flags & FClosing) == 0) {
                    _resp.Reset();
                    _app.WriteResponse(_resp, job.Status, job.Response.Written, job.Close);
                    QueueOutput(ref c, _resp.Written);
                    c.Flags &= ~FDeferred;
                    if (job.Close) {
                        c.Flags |= FCloseAfterSend;
                    }
                    Touch(job.Slot, ref c);
                    if (c.InLen > 0 && !job.Close) {
                        AddReady(job.Slot, ref c);
                    }
                }
                _app.Api.Writer.Return(job);
            }
        }
        _doneWork.Clear();

        for (int i = 0; i < _touchedCount; i++) {
            int slot = _touched[i];
            ref Conn c = ref _conns[slot];
            if ((c.Flags & FSending) == 0 && c.PeLen > 0) {
                StartSend(slot);
            }
        }
        _touchedCount = 0;
    }

    // ---- sending --------------------------------------------------------------------------

    /// <summary>Appends response bytes behind whatever the connection is already sending.</summary>
    private void QueueOutput(ref Conn c, ReadOnlySpan<byte> bytes) {
        if (c.PeLen + bytes.Length > c.PeCap) {
            Grow(ref c.Pe, ref c.PeCap, c.PeLen, c.PeLen + bytes.Length);
        }

        bytes.CopyTo(new Span<byte>(c.Pe + c.PeLen, bytes.Length));
        c.PeLen += bytes.Length;
    }

    /// <summary>Moves the queued output into flight and sends it.</summary>
    private void StartSend(int slot) {
        ref Conn c = ref _conns[slot];
        c.Fl = c.Pe;
        c.FlCap = c.PeCap;
        c.FlLen = c.PeLen;
        c.FlSent = 0;
        c.Pe = null;
        c.PeCap = c.PeLen = 0;
        SubmitSend(slot);
    }

    private void SubmitSend(int slot) {
        ref Conn c = ref _conns[slot];
        Sqe* s = Sqe();
        s->Opcode = Ring.OP_SEND;
        s->Flags = Ring.IOSQE_FIXED_FILE;
        s->Fd = slot;
        s->Addr = (ulong)(c.Fl + c.FlSent);
        s->Len = (uint)(c.FlLen - c.FlSent);
        s->OpFlags = MSG_NOSIGNAL;
        s->UserData = Ud(OpSend, slot, c.Gen);
        c.Flags |= FSending;
    }

    // ---- connection lifecycle -------------------------------------------------------------

    private void ArmRecv(int slot) {
        ref Conn c = ref _conns[slot];
        Sqe* s = Sqe();
        s->Opcode = Ring.OP_RECV;
        s->Flags = Ring.IOSQE_FIXED_FILE | Ring.IOSQE_BUFFER_SELECT;
        s->Ioprio = Ring.IORING_RECV_MULTISHOT;
        s->Fd = slot;
        s->BufGroup = 0;
        s->UserData = Ud(OpRecv, slot, c.Gen);
        c.Flags |= FRecvArmed;
    }

    /// <summary>Re-arms receives that ended (buffer exhaustion), at most <paramref name="budget"/> per loop.</summary>
    private void Rearm(int budget) {
        int n = Math.Min(budget, _rearmCount);
        for (int i = 0; i < n; i++) {
            ref Conn c = ref _conns[_rearm[i].Slot];
            if (c.Gen == _rearm[i].Gen && (c.Flags & FOpen) != 0
                && (c.Flags & (FRecvArmed | FClosing | FCloseAfterSend)) == 0) {
                ArmRecv(_rearm[i].Slot);
            }
        }
        Array.Copy(_rearm, n, _rearm, 0, _rearmCount - n);
        _rearmCount -= n;
    }

    /// <summary>Starts closing: waits for the send in flight, stops the receive, then frees the slot.</summary>
    private void BeginClose(int slot) {
        ref Conn c = ref _conns[slot];
        if ((c.Flags & FClosing) != 0) {
            return;
        }

        c.Flags |= FClosing;
        ContinueClose(slot);
    }

    /// <summary>Closes now, also breaking a send that is stuck because the client stopped reading.</summary>
    private void ForceClose(int slot) {
        ref Conn c = ref _conns[slot];
        c.Flags |= FClosing;
        if ((c.Flags & (FRecvArmed | FSending)) != 0 && (c.Flags & FShutdown) == 0) {
            Shutdown(slot, ref c);
        }
        ContinueClose(slot);
    }

    private void ContinueClose(int slot) {
        ref Conn c = ref _conns[slot];
        if ((c.Flags & FOpen) == 0 || (c.Flags & FSending) != 0) {
            return;
        }

        if ((c.Flags & FRecvArmed) != 0) {
            if ((c.Flags & FShutdown) == 0) {
                Shutdown(slot, ref c); // ends the multishot receive; its last completion continues the close
            }
            return;
        }

        ReleaseBuffers(ref c);
        Sqe* s = Sqe();
        s->Opcode = Ring.OP_CLOSE;
        s->Flags = Ring.IOSQE_CQE_SKIP_SUCCESS;
        s->FileIndex = (uint)slot + 1;
        s->UserData = Ud(OpClose, slot, c.Gen);
        c.Flags = 0;
        c.Gen++;
        _open--;
    }

    private void Shutdown(int slot, ref Conn c) {
        Sqe* s = Sqe();
        s->Opcode = Ring.OP_SHUTDOWN;
        s->Flags = Ring.IOSQE_FIXED_FILE | Ring.IOSQE_CQE_SKIP_SUCCESS;
        s->Fd = slot;
        s->Len = SHUT_RDWR;
        s->UserData = Ud(OpShutdown, slot, c.Gen);
        c.Flags |= FShutdown;
    }

    /// <summary>Arms or cancels the multishot accept from free slots and memory pressure.</summary>
    private void UpdateAccept() {
        int free = _slots - _open;
        if (_acceptArmed) {
            if ((free < _reserve || MemGuard.Level > 0) && !_acceptCancelling) {
                Sqe* s = Sqe();
                s->Opcode = Ring.OP_ASYNC_CANCEL;
                s->Flags = Ring.IOSQE_CQE_SKIP_SUCCESS;
                s->Addr = Ud(OpAccept, 0, _acceptGen);
                s->UserData = Ud(OpCancel, 0, 0);
                _acceptCancelling = true;
            }
        } else if (free >= 2 * _reserve && MemGuard.Level == 0) {
            _acceptGen++;
            Sqe* s = Sqe();
            s->Opcode = Ring.OP_ACCEPT;
            s->Fd = _listen;
            s->Ioprio = Ring.IORING_ACCEPT_MULTISHOT;
            s->FileIndex = Ring.IORING_FILE_INDEX_ALLOC;
            s->UserData = Ud(OpAccept, 0, _acceptGen);
            _acceptArmed = true;
        }
    }

    /// <summary>Memory checks (worker 0), the accept state, and the sweep for stalled connections.</summary>
    private void Housekeeping() {
        if (_id == 0 && _now >= _nextMem) {
            _nextMem = _now + 250;
            MemGuard.Update();
        }
        UpdateAccept();
        if (_now < _nextSweep) {
            return;
        }

        _nextSweep = _now + SweepMs;
        for (int slot = 0; slot < _slots; slot++) {
            ref Conn c = ref _conns[slot];
            if ((c.Flags & FOpen) != 0 && (c.InLen > 0 || c.FlLen > 0 || c.PeLen > 0) && _now - c.Active > StallMs) {
                ForceClose(slot);
            }
        }
    }

    // ---- helpers --------------------------------------------------------------------------

    private void AddReady(int slot, ref Conn c) {
        if ((c.Flags & FReady) == 0) {
            c.Flags |= FReady;
            Push(ref _ready, ref _readyCount, new SlotRef { Slot = slot, Gen = c.Gen });
        }
    }

    /// <summary>Appends bytes to the connection's incomplete request. False if it grew past the request limits.</summary>
    private bool Append(ref Conn c, byte* data, int length) {
        int need = c.InLen + length;
        if (need > MaxRequestBytes + MaxHeaderBytes) {
            return false;
        }

        if (need > c.InCap) {
            Grow(ref c.In, ref c.InCap, c.InLen, need);
        }

        Buffer.MemoryCopy(data, c.In + c.InLen, c.InCap - c.InLen, length);
        c.InLen = need;
        return true;
    }

    private void ReleaseBuffers(ref Conn c) {
        FreeBuffer(ref c.In, ref c.InCap);
        FreeBuffer(ref c.Fl, ref c.FlCap);
        FreeBuffer(ref c.Pe, ref c.PeCap);
        c.InLen = c.FlLen = c.FlSent = c.PeLen = 0;
    }

    /// <summary>Replaces a buffer with one of at least <paramref name="need"/> bytes, keeping its first <paramref name="keep"/> bytes.</summary>
    private void Grow(ref byte* buf, ref int cap, int keep, int need) {
        int newCap = need <= BlockSize ? BlockSize : (int)System.Numerics.BitOperations.RoundUpToPowerOf2((uint)need);
        byte* fresh;
        if (newCap == BlockSize && _pooled > 0) {
            fresh = (byte*)_pool[--_pooled];
        } else {
            fresh = (byte*)NativeMemory.Alloc((nuint)newCap);
        }
        if (keep > 0) {
            Buffer.MemoryCopy(buf, fresh, newCap, keep);
        }

        FreeBuffer(ref buf, ref cap);
        buf = fresh;
        cap = newCap;
    }

    private void FreeBuffer(ref byte* buf, ref int cap) {
        if (buf == null) {
            return;
        }

        if (cap == BlockSize && _pooled < MaxPooled) {
            _pool[_pooled++] = (nint)buf;
        } else {
            NativeMemory.Free(buf);
        }
        buf = null;
        cap = 0;
    }

    /// <summary>The next SQE. The ring submits when the SQ is full, so this only fails if the kernel stops taking entries.</summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private Sqe* Sqe() {
        Sqe* s = _ring.Get();
        while (s == null) {
            _ring.Submit();
            s = _ring.Get();
        }
        return s;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private static ulong Ud(uint op, int slot, uint gen) => op | ((ulong)(uint)slot << 8) | ((ulong)gen << 32);

    private static void Push<T>(ref T[] array, ref int count, T item) {
        if (count == array.Length) {
            Array.Resize(ref array, array.Length * 2);
        }

        array[count++] = item;
    }
}

using System.Buffers;
using System.Buffers.Text;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text.Json;

namespace Feed;

// One thread owns the io_uring rings and the SQLite connection. It parses requests, runs
// reads to completion inline, group-commits writes, and queues responses. There are no
// worker threads, so nothing here is locked or handed between threads.
sealed unsafe partial class UringServer(Db db, Jwt jwt, long started, int rings, int slots,
    int writeBatchSize, int writeBatchDelayMs) : IDisposable
{
    [StructLayout(LayoutKind.Sequential)]
    struct Event { public ulong Connection; public byte* Data; public int Length, Kind; }
    [StructLayout(LayoutKind.Sequential)]
    struct Response { public ulong Connection; public int Offset, Length, CloseAfter, Result; }
    const int DataEvent = 1, ClosedEvent = 2, WritableEvent = 3, EofEvent = 4;

    static partial class Native
    {
        const string Library = "feeduring";
        [LibraryImport(Library)] internal static partial int feed_uring_available(int slots);
        [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf8)]
        internal static partial nint feed_uring_open(string host, int port, int rings, int slots, out int error);
        [LibraryImport(Library)] internal static partial int feed_uring_poll(nint server, Event* events, int max, int timeoutMs);
        [LibraryImport(Library)] internal static partial int feed_uring_send(nint server, ulong connection, byte* bytes, int length, int closeAfter);
        [LibraryImport(Library)] internal static partial int feed_uring_send_batch(nint server, byte* bytes, int length, Response* responses, int count);
        [LibraryImport(Library)] internal static partial long feed_uring_pending_tx(nint server, ulong connection);
        [LibraryImport(Library)] internal static partial int feed_uring_close(nint server, ulong connection);
        [LibraryImport(Library)] internal static partial int feed_uring_finish(nint server, ulong connection);
        [LibraryImport(Library)] internal static partial void feed_uring_stop(nint server);
        [LibraryImport(Library)] internal static partial void feed_uring_free(nint server);
    }
    // Unfinished wire bytes of one connection (a partial or pipelined request).
    sealed class Pending
    {
        public byte[] Bytes = ArrayPool<byte>.Shared.Rent(512);
        public int Count;
        public bool Continued;
    }
    sealed class Job
    {
        public ulong Connection;
        public ApiRoute Route;
        public long Post, User, EnqueuedAt;
        public byte[]? Workspace;
        public int BodyCapacity, BodyOffset, BodyLength, AuthorLength, Offset, Length;
        public bool Close;
    }
    sealed class ResponseArena : IDisposable
    {
        public byte[] Bytes = ArrayPool<byte>.Shared.Rent(256 * 1024);
        public int Used;
        public void Ensure(int extra)
        {
            if (extra <= Bytes.Length - Used) return;
            byte[] larger = ArrayPool<byte>.Shared.Rent(Math.Max(checked(Used + extra), checked(Bytes.Length * 2)));
            Bytes.AsSpan(0, Used).CopyTo(larger); ArrayPool<byte>.Shared.Return(Bytes); Bytes = larger;
        }
        public void Dispose() => ArrayPool<byte>.Shared.Return(Bytes);
    }

    readonly object lifetime = new();
    readonly Dictionary<ulong, Pending> pending = new();
    readonly HashSet<ulong> closing = new(), scheduled = new(), readEof = new(), inFlight = new();
    readonly Queue<ulong> ready = new(), finished = new();
    readonly Queue<Job> writeJobs = new();
    readonly Stack<Job> recycledJobs = new();
    readonly Output scratch = new(256);
    readonly Job[] jobs = new Job[writeBatchSize];
    readonly Response[] responses = new Response[writeBatchSize];
    readonly ResponseArena arena = new();
    readonly long writeDelayTicks = (long)(writeBatchDelayMs * (double)Stopwatch.Frequency / 1000);
    nint server;
    volatile bool stopped;

    public static bool Available(int slots)
    {
        try { return Native.feed_uring_available(slots) == 1; }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException) { return false; }
    }

    public void Run(string host, int port)
    {
        // The rings are bound to this thread; it also disposes the server.
        server = Native.feed_uring_open(host, port, rings, slots, out int error);
        if (server == 0) throw new InvalidOperationException($"io_uring initialization failed errno={error}");
        Console.Error.WriteLine($"listening on {host}:{port}: io_uring, {rings} rings x {slots} slots, write window {writeBatchDelayMs} ms");
        Event* events = stackalloc Event[256];
        while (!stopped)
        {
            while (finished.TryDequeue(out ulong done))
            {
                inFlight.Remove(done);
                if (pending.ContainsKey(done)) Schedule(done);
                else if (readEof.Contains(done)) Native.feed_uring_finish(server, done);
            }
            int timeout = ready.Count > 0 ? 0 : 100;
            if (writeJobs.TryPeek(out Job? oldest))
            {
                long remaining = oldest.EnqueuedAt + writeDelayTicks - Stopwatch.GetTimestamp();
                timeout = Math.Min(timeout, Math.Max(0, (int)Math.Ceiling(remaining * 1000.0 / Stopwatch.Frequency)));
            }
            int count = Native.feed_uring_poll(server, events, 256, timeout);
            if (count < 0) { if (stopped) break; throw new IOException($"io_uring poll failed errno={-count}"); }
            for (int index = 0; index < count; index++)
            {
                Event item = events[index];
                if (item.Kind == ClosedEvent) { readEof.Remove(item.Connection); Forget(item.Connection); closing.Remove(item.Connection); inFlight.Remove(item.Connection); continue; }
                if (closing.Contains(item.Connection)) continue;
                if (item.Kind == EofEvent)
                {
                    readEof.Add(item.Connection);
                    if (pending.ContainsKey(item.Connection)) Schedule(item.Connection);
                    else if (!inFlight.Contains(item.Connection)) Native.feed_uring_finish(server, item.Connection);
                    continue;
                }
                if (item.Kind == WritableEvent) { if (pending.ContainsKey(item.Connection)) Schedule(item.Connection); continue; }
                if (item.Kind != DataEvent || item.Length <= 0) continue;
                if (!pending.TryGetValue(item.Connection, out Pending? state))
                    Process(item.Connection, new ReadOnlySpan<byte>(item.Data, item.Length), false);
                else if (Append(item.Connection, state, new ReadOnlySpan<byte>(item.Data, item.Length)))
                    Process(item.Connection, state.Bytes.AsSpan(0, state.Count), state.Continued);
            }
            // Bound pipeline processing per turn; slow readers retain only their
            // bounded unfinished wire bytes, not one job per pipelined request.
            int budget = Math.Min(ready.Count, 16);
            while (budget-- > 0)
            {
                ulong id = ready.Dequeue(); scheduled.Remove(id);
                if (!closing.Contains(id) && pending.TryGetValue(id, out Pending? state))
                    Process(id, state.Bytes.AsSpan(0, state.Count), state.Continued);
            }
            FlushWrites(false);
        }
        FlushWrites(true);
    }

    void Schedule(ulong id) { if (scheduled.Add(id)) ready.Enqueue(id); }
    void Forget(ulong id)
    {
        if (pending.Remove(id, out Pending? p)) ArrayPool<byte>.Shared.Return(p.Bytes, clearArray: true);
        scheduled.Remove(id);
        if (readEof.Contains(id) && !inFlight.Contains(id)) Native.feed_uring_finish(server, id);
    }
    bool Append(ulong id, Pending state, ReadOnlySpan<byte> bytes)
    {
        if (bytes.Length > HttpFraming.WireLimit - state.Count) { Native.feed_uring_close(server, id); Forget(id); closing.Add(id); return false; }
        if (state.Count + bytes.Length > state.Bytes.Length)
        {
            byte[] larger = ArrayPool<byte>.Shared.Rent(Math.Min(HttpFraming.WireLimit, Math.Max(state.Count + bytes.Length, state.Bytes.Length * 2)));
            state.Bytes.AsSpan(0, state.Count).CopyTo(larger);
            ArrayPool<byte>.Shared.Return(state.Bytes, clearArray: true); state.Bytes = larger;
        }
        bytes.CopyTo(state.Bytes.AsSpan(state.Count)); state.Count += bytes.Length; return true;
    }
    void Retain(ulong id, ReadOnlySpan<byte> bytes, bool continued, bool again)
    {
        if (!pending.TryGetValue(id, out Pending? state)) { state = new Pending(); pending.Add(id, state); }
        // The input may alias this state's buffer; copy before changing Count.
        if (bytes.Length > state.Bytes.Length)
        {
            byte[] larger = ArrayPool<byte>.Shared.Rent(bytes.Length); bytes.CopyTo(larger);
            ArrayPool<byte>.Shared.Return(state.Bytes, clearArray: true); state.Bytes = larger;
        }
        else bytes.CopyTo(state.Bytes);
        state.Count = bytes.Length; state.Continued = continued;
        if (again) Schedule(id);
    }
    void Process(ulong id, ReadOnlySpan<byte> wire, bool continued)
    {
        int budget = 32;
        while (!wire.IsEmpty && budget-- > 0)
        {
            // One request per connection at a time keeps pipelined responses in order.
            if (inFlight.Contains(id)) { Retain(id, wire, continued, false); return; }
            long queued = Native.feed_uring_pending_tx(server, id);
            if (queued < 0) { Forget(id); return; }
            if (queued >= 128 * 1024) { Retain(id, wire, continued, false); return; }
            int consumed = Handle(id, wire, ref continued);
            if (consumed < 0) { Forget(id); closing.Add(id); return; }
            if (consumed == 0) { if (readEof.Contains(id)) Forget(id); else Retain(id, wire, continued, false); return; }
            wire = wire[consumed..]; continued = false;
        }
        if (wire.IsEmpty) Forget(id);
        else Retain(id, wire, continued, true);
    }

    // Returns bytes consumed, 0 for an incomplete request, or -1 to close the connection.
    int Handle(ulong id, ReadOnlySpan<byte> wire, ref bool continued)
    {
        FlushWrites(false);
        int separator = wire.IndexOf("\r\n\r\n"u8);
        if (separator < 0)
        {
            if (wire.Length >= HttpFraming.HeaderLimit) return Error(id, 431, "headers too large");
            return 0;
        }
        int headerLength = separator + 4;
        if (headerLength > HttpFraming.HeaderLimit) return Error(id, 431, "headers too large");
        if (HttpFraming.Parse(wire[..headerLength], headerLength, out RequestHead head) != 0) return Error(id, 400, "bad request");
        ReadOnlySpan<byte> path = wire.Slice(head.PathOffset, head.PathLength), key = default;
        ApiRoute route = ApiRoute.Missing;
        if (head.Get && path.SequenceEqual("/health"u8)) route = ApiRoute.Health;
        else if (head.Get && path.SequenceEqual("/feed"u8)) route = ApiRoute.Feed;
        else if (head.Post && path.SequenceEqual("/posts"u8)) route = ApiRoute.Create;
        else if (head.Get && path.StartsWith("/posts/"u8) && !path[7..].Contains((byte)'/')) { route = ApiRoute.Post; key = path[7..]; }
        else if (head.Post && path.Length >= 12 && path.StartsWith("/posts/"u8) && path.EndsWith("/like"u8) && !path[7..^5].Contains((byte)'/')) { route = ApiRoute.Like; key = path[7..^5]; }
        byte[]? workspace = null, decoded = null;
        int authorLength = 0, bodyOffset = 0, bodyLength = 0;
        int bodyCapacity = head.Chunked ? HttpFraming.BodyLimit : Math.Clamp(head.ContentLength, 1, HttpFraming.BodyLimit);
        long user = 0, post = 0;
        try
        {
            if (route is ApiRoute.Create or ApiRoute.Like)
            {
                ReadOnlySpan<byte> authorization = wire.Slice(head.AuthOffset, head.AuthLength);
                if (route == ApiRoute.Create) workspace = ArrayPool<byte>.Shared.Rent(bodyCapacity + authorization.Length);
                string? auth = Authenticate(authorization, route == ApiRoute.Create ? workspace.AsSpan(bodyCapacity, authorization.Length) : Span<byte>.Empty, route == ApiRoute.Create, out user, out authorLength);
                if (auth != null) return Error(id, 401, auth);
            }
            if (route is ApiRoute.Post or ApiRoute.Like && !Jwt.PositiveUtf8(key, out post)) return Error(id, 400, "invalid post id");
            if (head.ContentLength > HttpFraming.BodyLimit) return Error(id, 413, "body too large");
            if (head.Continue && !continued)
            {
                continued = true;
                fixed (byte* interim = "HTTP/1.1 100 Continue\r\n\r\n"u8)
                    if (Native.feed_uring_send(server, id, interim, 25, 0) < 0) return -1;
            }
            int consumed;
            ReadOnlySpan<byte> body;
            if (head.Chunked)
            {
                decoded = ArrayPool<byte>.Shared.Rent(HttpFraming.BodyLimit);
                ChunkDecoder decoder = default; int cursor = headerLength;
                int result = decoder.Consume(wire, decoded.AsSpan(0, HttpFraming.BodyLimit), ref cursor);
                if (result == 0) return wire.Length >= HttpFraming.WireLimit ? Error(id, 413, "body too large") : 0;
                if (result != 1) return Error(id, result, result == 413 ? "body too large" : "bad request");
                consumed = cursor; body = decoded.AsSpan(0, decoder.Written);
            }
            else
            {
                consumed = headerLength + head.ContentLength;
                if (wire.Length < consumed) return 0;
                body = wire.Slice(headerLength, head.ContentLength);
            }
            if (route == ApiRoute.Create)
            {
                string? error;
                try { error = PostUtf8.Parse(body, workspace.AsSpan(0, bodyCapacity), out bodyOffset, out bodyLength); }
                catch (Exception ex) when (ex is JsonException or InvalidOperationException or System.Text.DecoderFallbackException) { error = "malformed JSON body"; }
                if (error != null) return Error(id, 400, error);
            }
            if (!recycledJobs.TryPop(out Job? job)) job = new Job();
            job.Connection = id; job.Route = route; job.Post = post; job.User = user;
            job.Workspace = workspace; job.BodyCapacity = bodyCapacity; job.BodyOffset = bodyOffset;
            job.BodyLength = bodyLength; job.AuthorLength = authorLength; job.Close = head.Close;
            workspace = null; inFlight.Add(id);
            if (route is ApiRoute.Create or ApiRoute.Like)
            {
                job.EnqueuedAt = Stopwatch.GetTimestamp();
                writeJobs.Enqueue(job);
                if (writeJobs.Count >= writeBatchSize) FlushWrites(true);
            }
            else
            {
                jobs[0] = job;
                ProcessJobs(1, write: false);
            }
            return head.Close ? -1 : consumed;
        }
        finally
        {
            if (workspace != null) ArrayPool<byte>.Shared.Return(workspace, clearArray: true);
            if (decoded != null) ArrayPool<byte>.Shared.Return(decoded);
        }
    }

    // Writes wait up to writeBatchDelayMs (or a full batch) so that one COMMIT covers many.
    void FlushWrites(bool force)
    {
        if (!writeJobs.TryPeek(out Job? oldest)) return;
        if (!force && writeJobs.Count < writeBatchSize && Stopwatch.GetTimestamp() < oldest.EnqueuedAt + writeDelayTicks) return;
        int count = 0;
        while (count < writeBatchSize && writeJobs.TryDequeue(out Job? job)) jobs[count++] = job;
        db.EndRead(); // a write group cannot start inside the open read transaction
        ProcessJobs(count, write: true);
    }

    // Runs jobs[0..count) and publishes their responses. Writes share one transaction with a
    // savepoint per request, and no response is published before COMMIT succeeds.
    void ProcessJobs(int count, bool write)
    {
        arena.Used = 0;
        try
        {
            bool rollback = false;
            try
            {
                if (write) db.BeginWrites(); else db.EnsureRead();
                for (int i = 0; i < count; i++)
                {
                    Job current = jobs[i];
                    if (write && count > 1) db.Savepoint();
                    int status = ExecuteJob(current, out bool failed);
                    if (write && count > 1)
                    {
                        if (failed) db.RollbackToSavepoint();
                        db.ReleaseSavepoint();
                    }
                    else if (write && failed) { db.RollbackWrites(); rollback = true; }
                    AppendResponse(current, status, scratch.Buffer.WrittenSpan);
                }
                if (write && !rollback) db.CommitWrites();
            }
            catch (Exception ex)
            {
                // A failed outer transaction invalidates every provisional success, not just its last job.
                Console.Error.WriteLine(ex);
                if (write) db.RollbackWrites();
                arena.Used = 0;
                for (int i = 0; i < count; i++) AppendResponse(jobs[i], 500, ApiCore.ErrorBytes("internal server error"));
            }
            for (int i = 0; i < count; i++)
                responses[i] = new Response { Connection = jobs[i].Connection, Offset = jobs[i].Offset, Length = jobs[i].Length, CloseAfter = jobs[i].Close ? 1 : 0 };
            fixed (byte* bytes = arena.Bytes)
            fixed (Response* entries = responses)
            {
                int result = Native.feed_uring_send_batch(server, bytes, arena.Used, entries, count);
                for (int i = 0; i < count; i++)
                    if (result < 0 || entries[i].Result < 0) Native.feed_uring_close(server, jobs[i].Connection);
            }
        }
        finally
        {
            for (int i = 0; i < count; i++)
            {
                Job current = jobs[i]; jobs[i] = null!;
                if (current.Workspace != null) { ArrayPool<byte>.Shared.Return(current.Workspace, clearArray: true); current.Workspace = null; }
                finished.Enqueue(current.Connection); recycledJobs.Push(current);
            }
        }
    }
    int ExecuteJob(Job job, out bool failed)
    {
        failed = false;
        try
        {
            return ApiCore.Execute(db, scratch, job.Route, job.Post, job.User,
                job.Route == ApiRoute.Create ? job.Workspace.AsSpan(job.BodyOffset, job.BodyLength) : ReadOnlySpan<byte>.Empty,
                job.Route == ApiRoute.Create ? job.Workspace.AsSpan(job.BodyCapacity, job.AuthorLength) : ReadOnlySpan<byte>.Empty, started);
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine(ex); failed = true;
            scratch.Reset(); scratch.Raw(ApiCore.ErrorBytes("internal server error"));
            return 500;
        }
    }
    void AppendResponse(Job job, int status, ReadOnlySpan<byte> body)
    {
        arena.Ensure(body.Length + 256); job.Offset = arena.Used;
        job.Length = FrameResponse(arena.Bytes.AsSpan(arena.Used), status, body, job.Close);
        arena.Used += job.Length;
    }
    string? Authenticate(ReadOnlySpan<byte> authorization, Span<byte> author, bool copy, out long user, out int length)
    {
        user = 0; length = 0;
        if (!authorization.StartsWith("Bearer "u8)) return "missing bearer token";
        char[]? rented = null;
        Span<char> chars = authorization.Length <= 1024 ? stackalloc char[1024] : (rented = ArrayPool<char>.Shared.Rent(authorization.Length));
        try
        {
            for (int i = 0; i < authorization.Length; i++) { if (authorization[i] > 127) return "invalid or expired token"; chars[i] = (char)authorization[i]; }
            return jwt.VerifyUtf8(chars[..authorization.Length], author, copy, out user, out length);
        }
        finally { if (rented != null) ArrayPool<char>.Shared.Return(rented, true); }
    }
    // Rejected requests are answered immediately and close the connection.
    int Error(ulong id, int status, string text)
    {
        ReadOnlySpan<byte> body = ApiCore.ErrorBytes(text);
        byte[] output = ArrayPool<byte>.Shared.Rent(body.Length + 256);
        try
        {
            int used = FrameResponse(output, status, body, true);
            fixed (byte* data = output)
                if (Native.feed_uring_send(server, id, data, used, 1) < 0) Native.feed_uring_close(server, id);
        }
        finally { ArrayPool<byte>.Shared.Return(output); }
        return -1;
    }
    static int FrameResponse(Span<byte> bytes, int status, ReadOnlySpan<byte> body, bool close)
    {
        ReadOnlySpan<byte> prefix = status switch
        {
            200 => "HTTP/1.1 200 OK\r\n"u8, 201 => "HTTP/1.1 201 Created\r\n"u8, 400 => "HTTP/1.1 400 Bad Request\r\n"u8,
            401 => "HTTP/1.1 401 Unauthorized\r\n"u8, 404 => "HTTP/1.1 404 Not Found\r\n"u8, 413 => "HTTP/1.1 413 Payload Too Large\r\n"u8,
            431 => "HTTP/1.1 431 Request Header Fields Too Large\r\n"u8, 503 => "HTTP/1.1 503 Service Unavailable\r\n"u8,
            _ => "HTTP/1.1 500 Internal Server Error\r\n"u8
        };
        prefix.CopyTo(bytes); int used = prefix.Length;
        "Content-Type: application/json\r\nContent-Length: "u8.CopyTo(bytes[used..]); used += 48;
        Utf8Formatter.TryFormat(body.Length, bytes[used..], out int digits); used += digits;
        "\r\n"u8.CopyTo(bytes[used..]); used += 2;
        if (close) { "Connection: close\r\n"u8.CopyTo(bytes[used..]); used += 19; }
        "\r\n"u8.CopyTo(bytes[used..]); used += 2;
        body.CopyTo(bytes[used..]); used += body.Length;
        return used;
    }

    // Safe from a signal handler thread: the poll loop wakes and exits.
    public void Stop() { lock (lifetime) { stopped = true; if (server != 0) Native.feed_uring_stop(server); } }
    public void Dispose()
    {
        Stop();
        lock (lifetime) { if (server != 0) { Native.feed_uring_free(server); server = 0; } }
        foreach (Pending p in pending.Values) ArrayPool<byte>.Shared.Return(p.Bytes, clearArray: true);
        pending.Clear();
        arena.Dispose();
    }
}

/// A Kestrel transport (IConnectionListenerFactory) on Linux epoll. One thread waits on a level-triggered epoll set:
/// a readable socket gets exactly one recv into the loop's buffer, the HTTP handler (`Conn.direct`) answers every
/// complete request in it, and the responses go out with one send. No pipes and no task per request.
/// Compared with Kestrel's socket transport, this saves the speculative read that ends in EAGAIN after every
/// request. Idea: #74 (own epoll loop), here kept inside Kestrel as its transport. x86-64 Linux only.
module Transport

#nowarn "9"

open System
open System.Buffers
open System.Collections.Concurrent
open System.Collections.Generic
open System.IO.Pipelines
open System.Net
open System.Net.Sockets
open System.Runtime.InteropServices
open System.Threading
open System.Threading.Channels
open System.Threading.Tasks
open Microsoft.AspNetCore.Connections
open Microsoft.AspNetCore.Http.Features
open Microsoft.FSharp.NativeInterop

/// struct epoll_event, packed on x86-64: 12 bytes.
[<Struct; StructLayout(LayoutKind.Sequential, Pack = 4)>]
type EpollEvent =
    val mutable Events: uint32
    val mutable Data: uint64

module private Libc =
    [<Literal>]
    let L = "libc.so.6"
    [<DllImport(L, SetLastError = true)>]
    extern int socket(int domain, int typ, int protocol)
    [<DllImport(L, SetLastError = true)>]
    extern int setsockopt(int fd, int level, int name, int& value, int len)
    [<DllImport(L, SetLastError = true)>]
    extern int bind(int fd, nativeint addr, int len)
    [<DllImport(L, SetLastError = true)>]
    extern int listen(int fd, int backlog)
    [<DllImport(L, SetLastError = true)>]
    extern int accept4(int fd, nativeint addr, nativeint len, int flags)
    [<DllImport(L, SetLastError = true)>]
    extern int epoll_create1(int flags)
    // epoll_ctl, recv and send never block here (non-blocking sockets), so they skip the GC transition.
    [<DllImport(L, SetLastError = true); SuppressGCTransition>]
    extern int epoll_ctl(int epfd, int op, int fd, EpollEvent& ev)
    [<DllImport(L, SetLastError = true)>]
    extern int epoll_wait(int epfd, nativeint events, int maxEvents, int timeout)
    [<DllImport(L, SetLastError = true); SuppressGCTransition>]
    extern nativeint recv(int fd, nativeint buf, nativeint len, int flags)
    [<DllImport(L, SetLastError = true); SuppressGCTransition>]
    extern nativeint send(int fd, nativeint buf, nativeint len, int flags)
    [<DllImport(L, SetLastError = true)>]
    extern int close(int fd)
    [<DllImport(L, SetLastError = true)>]
    extern int shutdown(int fd, int how)
    [<DllImport(L, SetLastError = true)>]
    extern int eventfd(uint32 initval, int flags)
    [<DllImport(L, SetLastError = true)>]
    extern nativeint write(int fd, nativeint buf, nativeint len)
    [<DllImport(L, SetLastError = true)>]
    extern nativeint read(int fd, nativeint buf, nativeint len)
    [<DllImport(L, SetLastError = true)>]
    extern int getrlimit(int resource, uint64[] rlim)
    /// A plain nanosleep: Thread.Sleep on Linux waits on a condition variable through the runtime.
    [<DllImport(L)>]
    extern int usleep(uint32 microseconds)

let private AF_INET = 2
let private SOCK_STREAM = 1
let private SOCK_NONBLOCK = 0x800
let private SOCK_CLOEXEC = 0x80000
let private SOL_SOCKET = 1
let private SO_REUSEADDR = 2
let private IPPROTO_TCP = 6
let private TCP_NODELAY = 1
let private EPOLL_CTL_ADD = 1
let private EPOLL_CTL_DEL = 2
let private EPOLL_CTL_MOD = 3
let private EPOLLIN = 0x001u
let private EPOLLOUT = 0x004u
let private EPOLLERR = 0x008u
let private EPOLLHUP = 0x010u
let private EAGAIN = 11
let private EINTR = 4
let private ENFILE = 23
let private EMFILE = 24
let private ENOBUFS = 105
let private ENOMEM = 12
let private MSG_NOSIGNAL = 0x4000
let private MSG_MORE = 0x8000

/// True where this transport works: x86-64 Linux (the epoll_event layout above).
let supported =
    OperatingSystem.IsLinux() && RuntimeInformation.ProcessArchitecture = Architecture.X64
    && Marshal.SizeOf<EpollEvent>() = 12

let private errno () = Marshal.GetLastPInvokeError()

type private Duplex(input: PipeReader, output: PipeWriter) =
    interface IDuplexPipe with
        member _.Input = input
        member _.Output = output

/// What a connection needs from its event loop.
[<AllowNullLiteral>]
type ILoop =
    abstract Epfd: int
    abstract Close: EConn -> unit
    /// The loop's receive buffer (pinned) and its address.
    abstract RecvBuffer: byte[]
    abstract RecvAddress: nativeint
    /// The loop's response buffer: empty between events.
    abstract Out: ArrayBufferWriter<byte>
    /// Holds the connection's response until the loop commits the open write transaction.
    abstract Defer: EConn -> unit

/// One accepted socket. Only the loop thread reads and writes it. The loop thread calls the HTTP handler
/// (`Conn.direct`) on the received bytes, and sends all its responses with one `send`. An idle connection holds
/// no buffer: a partial request or a response that the socket did not take goes into a pooled array.
and [<AllowNullLiteral>] EConn(id: int64, fd: int, loop: ILoop) =
    inherit ConnectionContext()
    let mutable features: IFeatureCollection = FeatureCollection(4)
    let finished = TaskCompletionSource()
    /// 0: Kestrel owns the connection. 1: detached, the loop owns it. 2: finished while Kestrel owned it.
    let mutable owner = 0
    let mutable transport: IDuplexPipe = null
    let mutable items: IDictionary<obj, obj> = null
    let mutable connectionId: string = null
    let mutable readOpen = true // registered for EPOLLIN, more requests can come
    let mutable closed = 0
    let mutable continueSent = false
    let mutable inBuf: byte[] = null // the start of a request that is not complete yet
    let mutable inLen = 0
    let mutable outBuf: byte[] = null // response bytes that wait for EPOLLOUT
    let mutable outOff = 0
    let mutable outLen = 0
    let mutable closeAfterSend = false
    /// Over the keep-alive budget at accept: the connection gets one request, "Connection: close", and the FIN.
    let mutable shed = false

    let release (a: byref<byte[]>) =
        if not (isNull a) then
            ArrayPool<byte>.Shared.Return a
            a <- null

    /// Completes when the connection stops reading. Kestrel then disposes it, and the loop closes the socket.
    member _.Closed = finished.Task

    /// The connection handler calls this. Kestrel then ends its part of the connection at once (its heartbeat
    /// stops visiting it, and its per-connection objects can go), and the loop closes the socket when it is done.
    /// False when the connection already finished: then Kestrel disposes it as usual.
    member _.Detach() = Interlocked.CompareExchange(&owner, 1, 0) = 0

    member _.Id = id
    member _.Shed with get () = shed and set v = shed <- v
    member _.Fd = fd
    /// The epoll data of this socket: the fd in the low 32 bits, the low 32 bits of the id above them.
    member _.Key = (uint64 (uint32 id) <<< 32) ||| uint64 (uint32 fd)

    member private this.Ctl(op: int, events: uint32) =
        let mutable ev = EpollEvent()
        ev.Events <- events
        ev.Data <- this.Key
        Libc.epoll_ctl(loop.Epfd, op, fd, &ev) |> ignore

    /// The loop calls this for every event on the socket.
    member this.OnEvent(events: uint32) =
        if not (isNull outBuf) then
            if events &&& (EPOLLOUT ||| EPOLLERR ||| EPOLLHUP) <> 0u then
                this.SendRest()
        elif readOpen && events &&& (EPOLLIN ||| EPOLLERR ||| EPOLLHUP) <> 0u then
            this.Receive()

    member private this.Receive() =
        let buf = loop.RecvBuffer
        let n = Libc.recv(fd, loop.RecvAddress, nativeint buf.Length, 0)
        if n > 0n then
            let n = int n
            let mutable data = ReadOnlySpan<byte>(buf, 0, n)
            if inLen > 0 then
                if inBuf.Length - inLen < n then
                    let bigger = ArrayPool<byte>.Shared.Rent(inLen + n)
                    Buffer.BlockCopy(inBuf, 0, bigger, 0, inLen)
                    ArrayPool<byte>.Shared.Return inBuf
                    inBuf <- bigger
                Buffer.BlockCopy(buf, 0, inBuf, inLen, n)
                inLen <- inLen + n
                data <- ReadOnlySpan<byte>(inBuf, 0, inLen)
            let out = loop.Out
            let mutable close = shed
            let mutable ok = true
            let mutable consumed = 0
            try
                consumed <- Conn.direct data out &close &continueSent
            with _ ->
                ok <- false
            if not ok then
                out.ResetWrittenCount()
                this.Finish()
            else
                let rest = data.Length - consumed
                if close || rest = 0 then
                    release &inBuf
                    inLen <- 0
                elif inLen > 0 then
                    if consumed > 0 then Buffer.BlockCopy(inBuf, consumed, inBuf, 0, rest)
                    inLen <- rest
                else
                    inBuf <- ArrayPool<byte>.Shared.Rent(max 4096 rest)
                    data.Slice(consumed).CopyTo(Span inBuf)
                    inLen <- rest
                this.Send close
        elif n = 0n then
            this.Finish()
        else
            let e = errno ()
            if e <> EAGAIN && e <> EINTR then
                this.Finish()

    /// Sends the loop's response buffer. What the socket does not take waits for EPOLLOUT; reading stops until then.
    member private this.Send(close: bool) =
        let out = loop.Out
        let len = out.WrittenCount
        if Db.inTxn && len > 0 then
            // A write transaction is open: this response waits for its COMMIT (Flush).
            outLen <- len
            outOff <- 0
            outBuf <- ArrayPool<byte>.Shared.Rent len
            out.WrittenSpan.CopyTo(Span outBuf)
            out.ResetWrittenCount()
            closeAfterSend <- close
            loop.Defer this
        else
            let sent = if len > 0 then this.TrySend(out.WrittenSpan, close) else 0
            if sent < 0 then
                out.ResetWrittenCount()
                this.Finish()
            elif sent < len then
                outLen <- len - sent
                outOff <- 0
                outBuf <- ArrayPool<byte>.Shared.Rent outLen
                out.WrittenSpan.Slice(sent).CopyTo(Span outBuf)
                out.ResetWrittenCount()
                closeAfterSend <- close
                this.Ctl(EPOLL_CTL_MOD, EPOLLOUT)
            else
                out.ResetWrittenCount()
                if close then this.Finish()

    member private this.SendRest() =
        let sent = this.TrySend(ReadOnlySpan<byte>(outBuf, outOff, outLen), closeAfterSend)
        if sent < 0 then
            release &outBuf
            this.Finish()
        else
            outOff <- outOff + sent
            outLen <- outLen - sent
            if outLen = 0 then
                release &outBuf
                if closeAfterSend then this.Finish() else this.Ctl(EPOLL_CTL_MOD, EPOLLIN)

    /// After the loop's COMMIT: sends the held response, or drops it and closes when the commit failed.
    member this.Flush(committed: bool) =
        if isNull outBuf then
            ()
        elif not committed then
            release &outBuf
            this.Finish()
        else
            let sent = this.TrySend(ReadOnlySpan<byte>(outBuf, outOff, outLen), closeAfterSend)
            if sent < 0 then
                release &outBuf
                this.Finish()
            else
                outOff <- outOff + sent
                outLen <- outLen - sent
                if outLen = 0 then
                    release &outBuf
                    if closeAfterSend then this.Finish()
                else
                    this.Ctl(EPOLL_CTL_MOD, EPOLLOUT)

    /// Bytes sent, 0 when the socket buffer is full, -1 on a socket error. `last`: the connection closes right after
    /// this response. If the loop owns the connection, the close follows at once on this thread, so MSG_MORE holds
    /// the data back and the close puts the FIN on the same segment (from #117): the client sees data and FIN
    /// together, closes second, and keeps no TIME_WAIT on its side. While Kestrel still owns the connection the
    /// close comes later from Kestrel, and data held by MSG_MORE would wait up to 200 ms, so then no MSG_MORE.
    member private _.TrySend(m: ReadOnlySpan<byte>, last: bool) : int =
        let flags = if last && Volatile.Read(&owner) = 1 then MSG_NOSIGNAL ||| MSG_MORE else MSG_NOSIGNAL
        let n =
            use p = fixed &MemoryMarshal.GetReference(m)
            Libc.send(fd, NativePtr.toNativeInt p, nativeint m.Length, flags)
        if n >= 0n then int n
        else
            let e = errno ()
            if e = EAGAIN || e = EINTR then 0 else -1

    /// No more requests (peer closed, error, or "Connection: close"): stop polling and end the connection.
    member private this.Finish() =
        if readOpen then
            readOpen <- false
            this.Ctl(EPOLL_CTL_MOD, 0u)
            release &inBuf
            inLen <- 0
            release &outBuf
            if Interlocked.CompareExchange(&owner, 2, 0) = 0 then finished.TrySetResult() |> ignore
            else loop.Close this

    /// Loop thread only: forget the socket and close it.
    member _.CloseNow() : bool =
        if Interlocked.Exchange(&closed, 1) <> 0 then false
        else
            let mutable ev = EpollEvent()
            Libc.epoll_ctl(loop.Epfd, EPOLL_CTL_DEL, fd, &ev) |> ignore
            Libc.close fd |> ignore
            readOpen <- false
            release &inBuf
            release &outBuf
            finished.TrySetResult() |> ignore
            true

    override _.ConnectionId
        with get () =
            if isNull connectionId then connectionId <- "e" + string id
            connectionId
        and set v = connectionId <- v

    override _.Features = features

    override _.Items
        with get () =
            if isNull items then items <- Dictionary<obj, obj>()
            items
        and set v = items <- v

    /// Not used: the handler gets the bytes from the loop. Kestrel's API needs a value, so make one on demand.
    override _.Transport
        with get () =
            if isNull transport then
                transport <- Duplex(Pipe().Reader, Pipe().Writer)
            transport
        and set v = transport <- v

    override _.Abort(_: ConnectionAbortedException) =
        if Volatile.Read(&closed) = 0 then Libc.shutdown(fd, 2) |> ignore

    /// Kestrel calls this after the handler ends. A detached connection only drops Kestrel's features, which
    /// point to Kestrel's connection objects. Otherwise close the socket on the loop thread.
    override this.DisposeAsync() =
        if Volatile.Read(&owner) = 1 then features <- FeatureCollection(0) else loop.Close this
        ValueTask.CompletedTask

/// The process's fd limit (soft RLIMIT_NOFILE; .NET raises it to the hard limit at start).
let private FdLimit =
    let lim = Array.zeroCreate<uint64> 2
    let soft = if Libc.getrlimit(7 (* RLIMIT_NOFILE *), lim) = 0 then lim.[0] else 65535UL
    int (min soft 1048576UL)

/// Accept stops while this many client sockets are open, so the process never runs out of fds. The runtime
/// needs a few free fds at any time: on a full fd table, CoreCLR's memory probe (a pipe) fails and the process
/// aborts with PAL_SEHException, the crash of #108 at the judge's fd limit.
let private AcceptLimit = FdLimit - 512

/// Keep-alive budget (idea: #117, #72). Each user holds one keep-alive connection, so past ~65,000 users the
/// fd limit (LimitNOFILE=65535) would stop accept. A connection accepted while this many sockets are open is
/// shed: it gets one request, the response says "Connection: close", and the server closes it right after the
/// send (so the TIME_WAIT is on the server, not on the client's ports). The client connects again for its next
/// request. The decision is per connection: kept connections stay kept, so the churn is only the users past the
/// budget. The margin leaves fds for the runtime and for the shed connections in flight.
let private KeepAliveBudget = FdLimit - 1024

/// Below this many events from one epoll_wait, the loop sleeps 2 ms before it waits again.
[<Literal>]
let private CoalesceBelow = 32

/// The epoll loop for one listening socket.
type private Loop(listenFd: int) as this =
    let epfd = Libc.epoll_create1 SOCK_CLOEXEC
    let efd = Libc.eventfd(0u, SOCK_NONBLOCK ||| SOCK_CLOEXEC)
    /// Connections by fd. An event carries the fd and the connection's id, so an event for a closed socket
    /// never reaches a new connection that got the same fd.
    let mutable conns: EConn[] = Array.zeroCreate 1024
    let recvBuffer = GC.AllocateUninitializedArray<byte>(65536, pinned = true)
    let recvAddress = Marshal.UnsafeAddrOfPinnedArrayElement(recvBuffer, 0)
    let out = ArrayBufferWriter<byte>(65536)
    let toClose = ConcurrentQueue<EConn>()
    let deferred = List<EConn>(64)
    let accepted =
        Channel.CreateUnbounded<ConnectionContext>(
            UnboundedChannelOptions(SingleReader = true, SingleWriter = true, AllowSynchronousContinuations = true)
        )
    let mutable nextId = 1L // a socket's key is >= 2^32; epoll data 0 = listening socket, 1 = eventfd
    let mutable thread: Thread = null
    /// False while accept is paused: AcceptLimit client sockets are open, or accept failed for lack of fds or memory
    /// (EMFILE, ENFILE, ENOBUFS, ENOMEM). The listening socket is then out of the epoll set, so the loop does not
    /// wake again and again for connections it cannot take. A socket closing below AcceptLimit resumes it.
    let mutable listening = true
    /// Open client sockets.
    let mutable live = 0

    do
        let add (fd: int) (data: uint64) =
            let mutable ev = EpollEvent()
            ev.Events <- EPOLLIN
            ev.Data <- data
            if Libc.epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) <> 0 then failwithf "epoll_ctl: errno %d" (errno ())
        add listenFd 0UL
        add efd 1UL
        thread <- Thread((fun () -> this.Run()), 256 * 1024, IsBackground = true, Name = "epoll")
        thread.Start()

    member _.Accepted = accepted.Reader

    /// Takes the listening socket out of the epoll set until a client socket closes. No allocation and no
    /// logging on this path: at the fd limit, either could fail too (idea: #117, #83).
    member private _.PauseAccept() =
        let mutable ev = EpollEvent()
        Libc.epoll_ctl(epfd, EPOLL_CTL_MOD, listenFd, &ev) |> ignore
        listening <- false

    member private this.AcceptAll() =
        let mutable go = true
        while go do
            let fd = if live >= AcceptLimit then -2 else Libc.accept4(listenFd, 0n, 0n, SOCK_NONBLOCK ||| SOCK_CLOEXEC)
            if fd = -2 then
                this.PauseAccept()
                go <- false
            elif fd < 0 then
                let e = errno ()
                if e = EMFILE || e = ENFILE || e = ENOBUFS || e = ENOMEM then
                    this.PauseAccept()
                    go <- false
                elif e <> EINTR && e <> 103 (* ECONNABORTED *) then
                    go <- false
            else
                let mutable one = 1
                Libc.setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, 4) |> ignore
                let id = nextId
                nextId <- nextId + 1L
                let c = new EConn(id, fd, this)
                live <- live + 1
                if live > KeepAliveBudget then c.Shed <- true
                if fd >= conns.Length then
                    let bigger = Array.zeroCreate (max (fd + 1) (conns.Length * 2))
                    Array.blit conns 0 bigger 0 conns.Length
                    conns <- bigger
                conns.[fd] <- c
                let mutable ev = EpollEvent()
                ev.Events <- EPOLLIN
                ev.Data <- c.Key
                Libc.epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) |> ignore
                // A shed connection serves one request and closes: the loop owns it from accept, and Kestrel never
                // sees it (Kestrel's dispatch of a connection costs a thread-pool wake-up, ~15 us per connection).
                if c.Shed then c.Detach() |> ignore else accepted.Writer.TryWrite c |> ignore

    member private this.Drain() =
        let mutable c: EConn = null
        while toClose.TryDequeue(&c) do
            this.Forget c

    member private this.Run() =
        let events = Array.zeroCreate<EpollEvent> 256
        let mutable buf = 0UL
        while true do
            let n =
                use p = fixed events
                Libc.epoll_wait(epfd, NativePtr.toNativeInt p, events.Length, -1)
            for i in 0 .. n - 1 do
                let ev = events.[i]
                try
                    match ev.Data with
                    | 0UL -> this.AcceptAll()
                    | 1UL ->
                        use p = fixed &buf
                        Libc.read(efd, NativePtr.toNativeInt p, 8n) |> ignore
                    | key ->
                        let fd = int (uint32 key)
                        if fd < conns.Length then
                            let c = conns.[fd]
                            if not (isNull c) && c.Key = key then c.OnEvent ev.Events
                with _ ->
                    ()
            if Db.inTxn then this.CommitAndFlush()
            if not toClose.IsEmpty then this.Drain()
            // Few events: wait 2 ms before the next epoll_wait, so that one wake-up serves more sockets. On a
            // single core each wake-up of this thread costs a context switch and more; a busy loop does not sleep.
            if n < CoalesceBelow then Libc.usleep 2000u |> ignore

    /// Commits the writes of this wake-up, then sends the responses that waited for it.
    member private _.CommitAndFlush() =
        let committed = Db.commit ()
        for c in deferred do
            try c.Flush committed with _ -> ()
        deferred.Clear()

    /// Loop thread only.
    member private _.Forget(c: EConn) =
        if c.Fd < conns.Length && obj.ReferenceEquals(conns.[c.Fd], c) then conns.[c.Fd] <- null
        if c.CloseNow() then live <- live - 1
        if not listening && live < AcceptLimit then
            // A socket closed, so an fd is free: take new connections again.
            let mutable ev = EpollEvent()
            ev.Events <- EPOLLIN
            Libc.epoll_ctl(epfd, EPOLL_CTL_MOD, listenFd, &ev) |> ignore
            listening <- true

    interface ILoop with
        member _.Epfd = epfd
        member _.RecvBuffer = recvBuffer
        member _.RecvAddress = recvAddress
        member _.Out = out
        member _.Defer c = deferred.Add c
        member _.Close c =
            if Thread.CurrentThread = thread then
                this.Forget c
            else
                toClose.Enqueue c
                let mutable one = 1UL
                use p = fixed &one
                Libc.write(efd, NativePtr.toNativeInt p, 8n) |> ignore

type private Listener(endpoint: EndPoint, loop: Loop) =
    interface IConnectionListener with
        member _.EndPoint = endpoint
        member _.AcceptAsync(ct) =
            let r = loop.Accepted
            let mutable c: ConnectionContext = null
            if r.TryRead(&c) then
                ValueTask<ConnectionContext>(c)
            else
                ValueTask<ConnectionContext>(
                    task {
                        try
                            return! r.ReadAsync(ct)
                        with _ ->
                            return null
                    }
                )
        member _.UnbindAsync(_) = ValueTask.CompletedTask
        member _.DisposeAsync() = ValueTask.CompletedTask

/// Register as Kestrel's IConnectionListenerFactory. IPv4 endpoints only, and one endpoint: its loop thread runs
/// every request, so the database needs no lock (`Db.singleThread`).
type EpollTransportFactory() =
    let mutable bound = 0
    interface IConnectionListenerFactory with
        member _.BindAsync(endpoint, _) =
            if Interlocked.Exchange(&bound, 1) <> 0 then
                raise (NotSupportedException "epoll transport: one endpoint only")
            Db.singleThread <- true
            Db.batching <- true
            Db.start false
            let ip =
                match endpoint with
                | :? IPEndPoint as ip when ip.AddressFamily = AddressFamily.InterNetwork -> ip
                | _ -> raise (NotSupportedException "epoll transport: IPv4 endpoints only")
            let fd = Libc.socket(AF_INET, SOCK_STREAM ||| SOCK_NONBLOCK ||| SOCK_CLOEXEC, 0)
            if fd < 0 then failwithf "socket: errno %d" (errno ())
            let mutable one = 1
            Libc.setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, 4) |> ignore
            let addr = Array.zeroCreate<byte> 16 // sockaddr_in
            addr.[0] <- byte AF_INET
            addr.[2] <- byte (ip.Port >>> 8)
            addr.[3] <- byte ip.Port
            ip.Address.GetAddressBytes().CopyTo(addr, 4)
            let rc =
                use p = fixed addr
                Libc.bind(fd, NativePtr.toNativeInt p, 16)
            if rc <> 0 then
                let e = errno ()
                raise (IO.IOException(sprintf "Failed to bind to address %O: errno %d" ip e))
            if Libc.listen(fd, 65535) <> 0 then failwithf "listen: errno %d" (errno ())
            ValueTask<IConnectionListener>(Listener(ip, Loop(fd)))

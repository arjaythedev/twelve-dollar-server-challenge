/// A Kestrel transport (IConnectionListenerFactory) on Linux epoll. One thread waits on a level-triggered epoll set:
/// a readable socket gets exactly one recv, and the bytes go into the connection's input pipe. Both pipes use the
/// inline scheduler, so the connection handler runs in that same call, and its response goes out with one send.
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
    [<DllImport(L, SetLastError = true)>]
    extern int epoll_ctl(int epfd, int op, int fd, EpollEvent& ev)
    [<DllImport(L, SetLastError = true)>]
    extern int epoll_wait(int epfd, nativeint events, int maxEvents, int timeout)
    [<DllImport(L, SetLastError = true)>]
    extern nativeint recv(int fd, nativeint buf, nativeint len, int flags)
    [<DllImport(L, SetLastError = true)>]
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
let private MSG_NOSIGNAL = 0x4000

/// True where this transport works: x86-64 Linux (the epoll_event layout above).
let supported =
    OperatingSystem.IsLinux() && RuntimeInformation.ProcessArchitecture = Architecture.X64
    && Marshal.SizeOf<EpollEvent>() = 12

let private errno () = Marshal.GetLastPInvokeError()

let private pipeOptions =
    PipeOptions(
        readerScheduler = PipeScheduler.Inline,
        writerScheduler = PipeScheduler.Inline,
        pauseWriterThreshold = 0L, // the handler limits request size itself; FlushAsync never waits
        resumeWriterThreshold = 0L,
        minimumSegmentSize = 4096,
        useSynchronizationContext = false
    )

type private Duplex(input: PipeReader, output: PipeWriter) =
    interface IDuplexPipe with
        member _.Input = input
        member _.Output = output

/// What a connection needs from its event loop.
[<AllowNullLiteral>]
type ILoop =
    abstract Epfd: int
    abstract Close: EConn -> unit

/// One accepted socket. Only the loop thread reads it; the send loop writes it.
and [<AllowNullLiteral>] EConn(id: int64, fd: int, loop: ILoop) =
    inherit ConnectionContext()
    let input = Pipe(pipeOptions)
    let output = Pipe(pipeOptions)
    let features = FeatureCollection(4)
    let mutable transport: IDuplexPipe = Duplex(input.Reader, output.Writer)
    let mutable items: IDictionary<obj, obj> = null
    let mutable connectionId: string = null
    let mutable readOpen = true // registered for EPOLLIN, input pipe open
    let mutable writeWait: TaskCompletionSource = null
    let mutable closed = 0
    let mutable sendTask: Task = null

    /// Starts the send loop (it waits on the output pipe).
    member this.Start() = sendTask <- this.SendLoop()

    member _.Id = id
    member _.Fd = fd
    member _.IsClosed = Volatile.Read(&closed) <> 0

    member private _.Ctl(op: int, events: uint32) =
        let mutable ev = EpollEvent()
        ev.Events <- events
        ev.Data <- uint64 id
        Libc.epoll_ctl(loop.Epfd, op, fd, &ev) |> ignore

    /// The loop calls this for every event on the socket.
    member this.OnEvent(events: uint32) =
        if events &&& (EPOLLOUT ||| EPOLLERR ||| EPOLLHUP) <> 0u && not (isNull writeWait) then
            let w = writeWait
            writeWait <- null
            this.Ctl(EPOLL_CTL_MOD, (if readOpen then EPOLLIN else 0u))
            w.TrySetResult() |> ignore
        if readOpen && events &&& (EPOLLIN ||| EPOLLERR ||| EPOLLHUP) <> 0u then
            this.Receive()

    member private this.Receive() =
        let w = input.Writer
        let mem = w.GetMemory 4096
        let n =
            use p = fixed &MemoryMarshal.GetReference(mem.Span)
            Libc.recv(fd, NativePtr.toNativeInt p, nativeint mem.Length, 0)
        if n > 0n then
            w.Advance(int n)
            // Inline scheduler: the connection handler runs inside this call, and its flush sends the response.
            let r = w.FlushAsync().GetAwaiter().GetResult()
            if r.IsCompleted then this.StopReading null
        elif n = 0n then
            this.StopReading null
        else
            let e = errno ()
            if e <> EAGAIN && e <> EINTR then
                this.StopReading(SocketException(e))

    /// End of input (peer closed, error, or the handler is done): stop polling, complete the input pipe.
    member private this.StopReading(error: exn) =
        if readOpen then
            readOpen <- false
            this.Ctl(EPOLL_CTL_MOD, (if isNull writeWait then 0u else EPOLLOUT))
            input.Writer.Complete error

    /// Sends what the handler writes, in order. Waits for EPOLLOUT only when the socket buffer is full.
    member private this.SendLoop() : Task =
        task {
            let reader = output.Reader
            try
                try
                    let mutable go = true
                    while go do
                        let! r = reader.ReadAsync()
                        let mutable rest = r.Buffer
                        while not rest.IsEmpty do
                            let sent = this.TrySend rest.First
                            if sent > 0 then
                                rest <- rest.Slice(int64 sent)
                            else
                                let tcs = TaskCompletionSource()
                                writeWait <- tcs
                                this.Ctl(EPOLL_CTL_MOD, (if readOpen then EPOLLIN ||| EPOLLOUT else EPOLLOUT))
                                do! tcs.Task
                        reader.AdvanceTo(r.Buffer.End)
                        if r.IsCompleted || r.IsCanceled then go <- false
                with _ ->
                    ()
            finally
                reader.Complete()
        }

    /// Bytes sent, 0 when the socket buffer is full. Throws on a socket error.
    member private _.TrySend(m: ReadOnlyMemory<byte>) : int =
        let n =
            use p = fixed &MemoryMarshal.GetReference(m.Span)
            Libc.send(fd, NativePtr.toNativeInt p, nativeint m.Length, MSG_NOSIGNAL)
        if n >= 0n then int n
        else
            let e = errno ()
            if e = EAGAIN || e = EINTR then 0 else raise (SocketException(e))

    /// Loop thread only: forget the socket and close it.
    member _.CloseNow() =
        if Interlocked.Exchange(&closed, 1) = 0 then
            let mutable ev = EpollEvent()
            Libc.epoll_ctl(loop.Epfd, EPOLL_CTL_DEL, fd, &ev) |> ignore
            Libc.close fd |> ignore
            readOpen <- false
            input.Writer.Complete()
            match writeWait with
            | null -> ()
            | w ->
                writeWait <- null
                w.TrySetException(ConnectionAbortedException()) |> ignore

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

    override _.Transport
        with get () = transport
        and set v = transport <- v

    override _.Abort(_: ConnectionAbortedException) =
        if Volatile.Read(&closed) = 0 then Libc.shutdown(fd, 2) |> ignore

    /// Kestrel calls this after the handler ends: let the send loop finish, then close the socket.
    override this.DisposeAsync() =
        if sendTask.IsCompleted then
            loop.Close this
            ValueTask.CompletedTask
        else
            ValueTask(
                task {
                    try
                        do! sendTask
                    with _ ->
                        ()
                    loop.Close this
                }
            )

/// The epoll loop for one listening socket.
type private Loop(listenFd: int) as this =
    let epfd = Libc.epoll_create1 SOCK_CLOEXEC
    let efd = Libc.eventfd(0u, SOCK_NONBLOCK ||| SOCK_CLOEXEC)
    let conns = Dictionary<int64, EConn>()
    let toClose = ConcurrentQueue<EConn>()
    let accepted =
        Channel.CreateUnbounded<ConnectionContext>(
            UnboundedChannelOptions(SingleReader = true, SingleWriter = true, AllowSynchronousContinuations = true)
        )
    let mutable nextId = 2L // 0 = listening socket, 1 = eventfd
    let mutable thread: Thread = null

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

    member private this.AcceptAll() =
        let mutable go = true
        while go do
            let fd = Libc.accept4(listenFd, 0n, 0n, SOCK_NONBLOCK ||| SOCK_CLOEXEC)
            if fd < 0 then
                go <- false
            else
                let mutable one = 1
                Libc.setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, 4) |> ignore
                let id = nextId
                nextId <- nextId + 1L
                let c = new EConn(id, fd, this)
                c.Start()
                conns.[id] <- c
                let mutable ev = EpollEvent()
                ev.Events <- EPOLLIN
                ev.Data <- uint64 id
                Libc.epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) |> ignore
                accepted.Writer.TryWrite c |> ignore

    member private _.Drain() =
        let mutable c: EConn = null
        while toClose.TryDequeue(&c) do
            conns.Remove c.Id |> ignore
            c.CloseNow()

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
                    | id ->
                        match conns.TryGetValue(int64 id) with
                        | true, c -> c.OnEvent ev.Events
                        | _ -> ()
                with _ ->
                    ()
            if not toClose.IsEmpty then this.Drain()

    interface ILoop with
        member _.Epfd = epfd
        member _.Close c =
            if Thread.CurrentThread = thread then
                conns.Remove c.Id |> ignore
                c.CloseNow()
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

/// Register as Kestrel's IConnectionListenerFactory. IPv4 endpoints only.
type EpollTransportFactory() =
    interface IConnectionListenerFactory with
        member _.BindAsync(endpoint, _) =
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

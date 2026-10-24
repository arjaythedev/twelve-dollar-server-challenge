/// Kestrel connection handler: Kestrel's socket transport gives us the connection's two pipes, and this module
/// parses HTTP/1.1 itself. Kestrel's HTTP layer does not run, so a connection has no request/response objects:
/// an idle keep-alive connection holds only the transport's state.
module Conn

#nowarn "9"

open System
open System.Buffers
open System.IO.Pipelines
open System.Text
open System.Threading.Tasks
open Microsoft.AspNetCore.Connections

let private MaxHeaderBytes = 16384
let private MaxBodyBytes = 1 <<< 20

let private Wrote = 1
let private Close = 2
let private NeedMore = 4
let private ContinueSent = 8
/// The connection answers one request and closes: the transport sheds it (see Transport.KeepAliveBudget).
let private ForceClose = 16
/// A write request was left unanswered and unconsumed because `Db.holdWrites` is set.
let private Held = 32

/// Set by `direct` when it stopped at a write request held by `Db.holdWrites` (io_uring loop: answer it later).
let mutable held = false

let private crlf2 = "\r\n\r\n"B
let private crlf = "\r\n"B
let private ows = " \t"B
let private continue100 = "HTTP/1.1 100 Continue\r\n\r\n"B

/// A post id: 1 to 18 ASCII digits, above zero. 0 means invalid.
let private parseId (s: ReadOnlySpan<byte>) =
    if s.Length = 0 || s.Length > 18 then 0L
    else
        let mutable v = 0L
        let mutable i = 0
        while i < s.Length do
            let c = s.[i]
            if c >= '0'B && c <= '9'B then
                v <- v * 10L + int64 (c - '0'B)
                i <- i + 1
            else
                v <- 0L
                i <- s.Length
        v

// Byte strings made once: an F# "..."B literal inside a function allocates a new array each time it runs.
let private bGet = "GET"B
let private bPost = "POST"B
let private bFeed = "/feed"B
let private bPostsSlash = "/posts/"B
let private bHealth = "/health"B
let private bPosts = "/posts"B
let private bLike = "/like"B
let private bHttp1 = "HTTP/1."B
let private bHttp10 = "HTTP/1.0"B
let private hAuthorization = "authorization"B
let private hContentLength = "content-length"B
let private hConnection = "connection"B
let private vClose = "close"B
let private vKeepAlive = "keep-alive"B
let private hTransferEncoding = "transfer-encoding"B
let private hExpect = "expect"B

let private dispatch (w: IBufferWriter<byte>) (close: bool) (meth: ReadOnlySpan<byte>) (path: ReadOnlySpan<byte>)
                     (auth: ReadOnlySpan<byte>) (body: ReadOnlySpan<byte>) : Task =
    if meth.SequenceEqual(bGet) then
        if path.SequenceEqual(bFeed) then
            Api.feed w close
            null
        elif path.StartsWith(bPostsSlash) && path.Slice(7).IndexOf('/'B) < 0 then
            let id = parseId (path.Slice 7)
            if id = 0L then Api.send w 400 close Api.InvalidId else Api.getPost w close id
            null
        elif path.SequenceEqual(bHealth) then
            Api.health w close
            null
        else
            Api.send w 404 close Api.NotFound
            null
    elif meth.SequenceEqual(bPost) then
        if path.SequenceEqual(bPosts) then
            let mutable userId = 0L
            let mutable username: string = null
            match Auth.verify auth &userId &username with
            | null -> Api.createPost w close userId username body
            | authError ->
                Api.send w 401 close authError
                null
        elif path.Length > 12 && path.StartsWith(bPostsSlash) && path.EndsWith(bLike)
             && path.Slice(7, path.Length - 12).IndexOf('/'B) < 0 then
            // Auth first, then the id (SPEC).
            let mutable userId = 0L
            let mutable username: string = null
            match Auth.verify auth &userId &username with
            | null ->
                let id = parseId (path.Slice(7, path.Length - 12))
                if id = 0L then
                    Api.send w 400 close Api.InvalidId
                    null
                else
                    Api.like w close userId id
            | authError ->
                Api.send w 401 close authError
                null
        else
            Api.send w 404 close Api.NotFound
            null
    else
        Api.send w 404 close Api.NotFound
        null

/// Parses and answers every complete request in `s`. Stops early at a request whose response is still pending
/// (a Task), or at a request that closes the connection.
/// `flags` comes in with ContinueSent set when the request at the start of `s` already got its 100 Continue.
let private processSpan (s: ReadOnlySpan<byte>) (w: IBufferWriter<byte>)
                        (consumed: byref<int>) (pending: byref<Task>) (flags: byref<int>) =
    let mutable off = 0
    let mutable stop = false
    while not stop do
        let rest = s.Slice off
        let headEnd = rest.IndexOf(ReadOnlySpan crlf2)
        if headEnd < 0 then
            if rest.Length > MaxHeaderBytes then
                Api.send w 400 true Api.NotFound
                flags <- flags ||| Wrote ||| Close
            elif rest.Length > 0 then
                flags <- flags ||| NeedMore
            stop <- true
        else
            let lineEnd = rest.IndexOf(ReadOnlySpan crlf)
            let line = rest.Slice(0, lineEnd)
            let sp1 = line.IndexOf ' 'B
            let afterMethod = if sp1 < 0 then ReadOnlySpan.Empty else line.Slice(sp1 + 1)
            let sp2 = afterMethod.IndexOf ' 'B
            if sp1 <= 0 || sp2 <= 0 || not (afterMethod.Slice(sp2 + 1).StartsWith(bHttp1)) then
                Api.send w 400 true Api.NotFound
                flags <- flags ||| Wrote ||| Close
                stop <- true
            else
                let meth = line.Slice(0, sp1)
                let mutable path = afterMethod.Slice(0, sp2)
                let q = path.IndexOf '?'B
                if q >= 0 then path <- path.Slice(0, q)
                let force = flags &&& ForceClose <> 0
                let mutable close = force || afterMethod.Slice(sp2 + 1).SequenceEqual(bHttp10)
                let mutable auth = ReadOnlySpan<byte>.Empty
                let mutable contentLength = 0
                let mutable expect = false
                let mutable bad = false
                let mutable hs = if headEnd > lineEnd then rest.Slice(lineEnd + 2, headEnd - lineEnd) else ReadOnlySpan.Empty
                while hs.Length > 0 do
                    let e = hs.IndexOf(ReadOnlySpan crlf)
                    let h = if e < 0 then hs else hs.Slice(0, e)
                    hs <- if e < 0 then ReadOnlySpan.Empty else hs.Slice(e + 2)
                    let colon = h.IndexOf ':'B
                    if colon > 0 then
                        let name = h.Slice(0, colon)
                        let value = h.Slice(colon + 1).Trim(ReadOnlySpan ows)
                        match name.Length with
                        | 13 when Ascii.EqualsIgnoreCase(name, hAuthorization) -> auth <- value
                        | 14 when Ascii.EqualsIgnoreCase(name, hContentLength) ->
                            let mutable n = 0L
                            let mutable i = 0
                            if value.Length = 0 || value.Length > 9 then bad <- true
                            while i < value.Length do
                                let c = value.[i]
                                if c >= '0'B && c <= '9'B then n <- n * 10L + int64 (c - '0'B) else bad <- true
                                i <- i + 1
                            contentLength <- int n
                        | 10 when Ascii.EqualsIgnoreCase(name, hConnection) ->
                            if Ascii.EqualsIgnoreCase(value, vClose) then close <- true
                            elif Ascii.EqualsIgnoreCase(value, vKeepAlive) then close <- force
                        | 17 when Ascii.EqualsIgnoreCase(name, hTransferEncoding) -> bad <- true // chunked bodies: not supported
                        | 6 when Ascii.EqualsIgnoreCase(name, hExpect) -> expect <- true
                        | _ -> ()
                if bad || contentLength > MaxBodyBytes then
                    Api.send w 400 true Api.Malformed
                    flags <- flags ||| Wrote ||| Close
                    stop <- true
                else
                    let total = headEnd + 4 + contentLength
                    if rest.Length < total then
                        // ContinueSent is cleared after each complete request, so a pipelined request gets its own 100.
                        if expect && flags &&& ContinueSent = 0 then
                            w.Write(ReadOnlySpan continue100)
                            flags <- flags ||| Wrote ||| ContinueSent
                        flags <- flags ||| NeedMore
                        stop <- true
                    elif Db.holdWrites && meth.SequenceEqual(bPost) then
                        // While the WAL catches up, writes wait in the input; the loop runs them again later.
                        flags <- flags ||| Held
                        stop <- true
                    else
                        let body = rest.Slice(headEnd + 4, contentLength)
                        let t =
                            try dispatch w close meth path auth body
                            with _ ->
                                Api.discardJson ()
                                Api.send w 500 close Api.Internal
                                null
                        off <- off + total
                        flags <- (flags ||| Wrote) &&& ~~~ContinueSent
                        if close then
                            flags <- flags ||| Close
                            stop <- true
                        if not (isNull t) then
                            pending <- t
                            stop <- true
    consumed <- off

/// One pass over what the transport has read. Returns (bytes consumed, pending response or null, flags).
let private step (buf: ReadOnlySequence<byte>) (w: IBufferWriter<byte>) (continueSent: bool) : struct (int * Task * int) =
    let mutable consumed = 0
    let mutable pending: Task = null
    let mutable flags = if continueSent then ContinueSent else 0
    if buf.IsSingleSegment then
        processSpan buf.FirstSpan w &consumed &pending &flags
    elif buf.Length > int64 (MaxHeaderBytes + MaxBodyBytes) then
        Api.send w 400 true Api.Malformed
        flags <- Wrote ||| Close
    else
        // A request split over two buffer segments (rare): copy it into one array.
        let len = int buf.Length
        let arr = ArrayPool<byte>.Shared.Rent len
        buf.CopyTo(Span arr)
        processSpan (ReadOnlySpan(arr, 0, len)) w &consumed &pending &flags
        ArrayPool<byte>.Shared.Return arr
    struct (consumed, pending, flags)

/// For the epoll transport: answers every complete request in `s` into `w`, in order. A pending response (only
/// possible if a write did not finish at once) is waited for in place. Returns the bytes consumed; sets `close` when
/// the connection must close after the responses are sent, and `continueSent` as for the next read.
/// `close` set on entry: answer the first request with "Connection: close" and stop there.
let direct (s: ReadOnlySpan<byte>) (w: IBufferWriter<byte>) (close: byref<bool>) (continueSent: byref<bool>) : int =
    let mutable off = 0
    let mutable go = true
    let force = if close then ForceClose else 0
    close <- false // from here on, the output: set only when a request closes the connection
    held <- false
    while go do
        let mutable consumed = 0
        let mutable pending: Task = null
        let mutable flags = (if continueSent then ContinueSent else 0) ||| force
        processSpan (s.Slice off) w &consumed &pending &flags
        off <- off + consumed
        continueSent <- flags &&& ContinueSent <> 0
        if not (isNull pending) then
            try
                pending.GetAwaiter().GetResult()
            with _ ->
                Api.send w 500 (flags &&& Close <> 0) Api.Internal
        if flags &&& Close <> 0 then
            close <- true
            go <- false
        elif flags &&& Held <> 0 then
            held <- true
            go <- false
        elif isNull pending then
            go <- false
    off

/// The connection loop on Kestrel's pipes (socket transport): read, answer every complete request, flush once per read.
let run (conn: ConnectionContext) : Task =
    let input = conn.Transport.Input
    let output = conn.Transport.Output
    task {
        try
            try
                let mutable go = true
                let mutable continueSent = false
                while go do
                    let! r = input.ReadAsync()
                    let buf = r.Buffer
                    let struct (consumed, pending, flags) = step buf (output :> IBufferWriter<byte>) continueSent
                    continueSent <- flags &&& ContinueSent <> 0
                    if flags &&& NeedMore <> 0 then
                        input.AdvanceTo(buf.GetPosition(int64 consumed), buf.End)
                    else
                        input.AdvanceTo(buf.GetPosition(int64 consumed))
                    if not (isNull pending) then
                        try
                            do! pending
                        with _ ->
                            Api.send output 500 (flags &&& Close <> 0) Api.Internal
                    if flags &&& Wrote <> 0 then
                        let! _ = output.FlushAsync()
                        ()
                    if flags &&& Close <> 0 || r.IsCompleted then
                        go <- false
            with _ ->
                ()
        finally
            input.Complete()
            output.Complete()
    }
    :> Task

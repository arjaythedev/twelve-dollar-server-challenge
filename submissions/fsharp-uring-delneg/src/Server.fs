/// The server: one thread drives every io_uring ring. Multishot accept into fixed-file tables (several rings lift the
/// connection ceiling past the 65,535-descriptor limit), multishot receive into provided-buffer rings, requests parsed
/// in place, one SQLite transaction per loop pass, and no response handed to the kernel before its pass's COMMIT.
module Server

#nowarn "9"

open System
open System.Collections.Generic
open System.Diagnostics
open System.Runtime.InteropServices
open System.Text
open System.Text.Json
open Microsoft.FSharp.NativeInterop
open Ring

let private env name dflt = envInt name dflt

// ---------- settings (env vars, for measuring; the defaults are what start.sh runs) ----------

let BufSize = 1024
let MaxReq = 16384
let private nBufs = env "BUFS" 1024 // receive buffers per ring, a power of two
let private answers = env "ANSWERS" 64 // requests answered per pass (0: all)
let private wwin = uint32 (env "WWIN" 10) // ms: writes commit at most once per window (group commit)
let private lifoOn = env "LIFO" 1 = 1
let private lifoCap = env "LIFOCAP" 4
let private lifoMin = env "LIFOMIN" 0
let private lifoAge = uint32 (env "LIFOAGE" 8000)
let private oneWriteOn = env "ONEWRITE" 1 = 1
let private wqMax = env "WQMAX" 4096 // held requests answered per pass once a hold ends
let private rearmMax = env "REARM" 2048
let private ckPass = env "CKPASS" 4000
let private ckHoldAt = env "HOLD" 16384
let private batchN = env "BATCH" 128
let private batchWaitUs = env "BWAIT" 1000
let private batchThresh = env "BTHRESH" 8
let private poolCap = env "POOLCAP" 6144
let private poolMax = env "POOLMAX" 4096
let private cqSize = env "CQ" 65536
let private statsOn = env "STATS" 0 = 1
let private taskrunFlag = env "TASKRUN_FLAG" 1 = 1
let private holdRem = env "HOLDREM" 0 // a hold starts only after a pass leaves at most this many frames (0: at HOLD)
let private holdMax = env "HOLDMAX" 262144 // with HOLDREM: past this many frames (1 GB of WAL), hold after any pass
let private holdMs = int64 (env "HOLDMS" 250) // a hold longer than this ends without its checkpoint pass

// ---------- user_data: type << 56 | gen << 32 | slot ----------

let UdAccept = 1UL
let UdRecv = 2UL
let UdSend = 3UL
let UdClose = 4UL
let UdCkpt = 6UL
let UdIgnore = 7UL
let NoGen = 0xFFFFFFu

let inline private ud (t: uint64) (slot: int) (gen: uint32) =
    (t <<< 56) ||| (uint64 (gen &&& 0xFFFFFFu) <<< 32) ||| uint64 (uint32 slot)

let inline private udT (u: uint64) = u >>> 56
let inline private udGen (u: uint64) = uint32 (u >>> 32) &&& 0xFFFFFFu
let inline private udSlot (u: uint64) = int (uint32 u)

// connection flags
[<Literal>]
let private LIVE = 1uy
[<Literal>]
let private RECV = 2uy
[<Literal>]
let private SENDING = 4uy
[<Literal>]
let private CLOSEAFTER = 8uy
[<Literal>]
let private DIRTY = 16uy
[<Literal>]
let private REARM = 32uy

// routes
[<Literal>]
let private RErr = 0uy
[<Literal>]
let private RHealth = 1uy
[<Literal>]
let private RFeed = 2uy
[<Literal>]
let private RPost = 3uy
[<Literal>]
let private RCreate = 4uy
[<Literal>]
let private RLike = 5uy

// errors: SPEC.md's messages
let private errMsgs =
    [| 404, "not found"; 400, "invalid post id"; 404, "post not found"; 401, "missing bearer token"
       401, "invalid or expired token"; 401, "invalid token payload"; 400, "malformed JSON body"; 400, "body is required"
       400, "body must be at most 500 characters"; 500, "internal server error" |]
let private errBody = errMsgs |> Array.map (fun (_, m) -> Encoding.ASCII.GetBytes("{\"error\":\"" + m + "\"}"))
let private errStatus = errMsgs |> Array.map fst
[<Literal>]
let private ENotFound = 0uy
[<Literal>]
let private EBadId = 1uy
[<Literal>]
let private ENoPost = 2uy
[<Literal>]
let private ENoToken = 3uy
[<Literal>]
let private EBadToken = 4uy
[<Literal>]
let private EBadPayload = 5uy
[<Literal>]
let private EBadJson = 6uy
[<Literal>]
let private ENoBody = 7uy
[<Literal>]
let private ELongBody = 8uy
[<Literal>]
let private EInternal = 9uy

// (an F# "…"B literal inside a function builds a new array on every call, so they are module values)
let private s200 = "HTTP/1.1 200 OK\r\n"B
let private s201 = "HTTP/1.1 201 Created\r\n"B
let private s400 = "HTTP/1.1 400 Bad Request\r\n"B
let private s401 = "HTTP/1.1 401 Unauthorized\r\n"B
let private s404 = "HTTP/1.1 404 Not Found\r\n"B
let private s503 = "HTTP/1.1 503 Service Unavailable\r\n"B
let private s500 = "HTTP/1.1 500 Internal Server Error\r\n"B

let private statusLine (st: int) =
    match st with
    | 200 -> s200
    | 201 -> s201
    | 400 -> s400
    | 401 -> s401
    | 404 -> s404
    | 503 -> s503
    | _ -> s500

let private hCtype = "Content-Type: application/json\r\nContent-Length: "B
let private hEnd = "\r\n\r\n"B
let private hEndClose = "\r\nConnection: close\r\n\r\n"B
let private crlf2 = "\r\n\r\n"B
let private kGet = "GET"B
let private kPost = "POST"B
let private kFeed = "/feed"B
let private kPosts = "/posts"B
let private kPostsSlash = "/posts/"B
let private kLike = "/like"B
let private kHealth = "/health"B
let private kBearer = "Bearer "B
let private kContentLength = "content-length"B
let private kAuthorization = "authorization"B
let private kConnection = "connection"B
let private kTransferEncoding = "transfer-encoding"B
let private kClose = "close"B
let private kKeepAlive = "keep-alive"B
let private kBodyKey = "body"B
let private jHealth = "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":"B
let private jDegraded = "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":"B
let private jPostOpen = "{\"post\":"B
let private jLiked = "{\"liked\":true,\"already_liked\":"B
let private jFalse = "false"B
let private jTrue = "true"B
let private jPostId = ",\"post_id\":"B

/// a positive decimal id of at most 18 digits, or -1
let private parseId (s: ReadOnlySpan<byte>) =
    if s.Length = 0 || s.Length > 18 then -1L
    else
        let mutable v = 0L
        let mutable ok = true
        for c in s do
            let d = int c - 48
            if d < 0 || d > 9 then ok <- false else v <- v * 10L + int64 d
        if ok && v > 0L then v else -1L

[<Struct; NoEquality; NoComparison>]
type Conn =
    val mutable Out: Json.Out // responses built and not sent yet (head at OutStart)
    val mutable Wire: Json.Out // the response the kernel is sending (from WireOff)
    val mutable In: byte[] // bytes of a request split across receives
    val mutable Gen: uint32
    val mutable Queued: int // requests read and not answered yet
    val mutable Held: int // requests held back by a write hold
    val mutable InLen: int
    val mutable OutStart: int
    val mutable WireOff: int
    val mutable Fl: byte

[<Struct; NoEquality; NoComparison>]
type Req =
    val mutable Cid: int
    val mutable Gen: uint32
    val mutable Ms: uint32
    val mutable Route: byte
    val mutable Err: byte
    val mutable Close: bool
    val mutable Held: bool
    val mutable Id: int64
    val mutable Uid: int64
    val mutable NameOff: int
    val mutable NameLen: int
    val mutable BodyOff: int
    val mutable BodyLen: int

/// One io_uring with its fixed-file table and its provided-buffer ring.
[<Sealed; AllowNullLiteral>]
type RingState(k: int, slots: int) =
    let ring =
        // newer kernels charge ring memory to RLIMIT_MEMLOCK: the completion queue halves until it fits
        let mutable r: Ring = null
        let mutable cq = cqSize
        while isNull r do
            try
                r <- Ring(4096u, uint32 cq)
            with :? InvalidOperationException when cq > 4096 -> cq <- cq / 2
        r
    do
        let reg = Marshal.AllocHGlobal 32
        NativeMemory.Clear(NativePtr.toVoidPtr (NativePtr.ofNativeInt<byte> reg), unativeint 32)
        wr32 reg (uint32 slots)
        wr32 (reg + 4n) 1u // IORING_RSRC_REGISTER_SPARSE
        try ring.Register(REGISTER_FILES2, reg, 32L) finally Marshal.FreeHGlobal reg
    let bufMem =
        let m = Native.mmap (0n, unativeint (nBufs * BufSize), 3, 0x2 ||| 0x20, -1, 0L)
        if m = -1n then raise (OutOfMemoryException "receive buffers")
        m
    let bufRing =
        let m = Native.mmap (0n, unativeint (nBufs * 16), 3, 0x2 ||| 0x20, -1, 0L)
        if m = -1n then raise (OutOfMemoryException "buffer ring")
        m
    let bufMask = uint16 (nBufs - 1)
    let mutable bufTail = 0us
    let how = ring.RegisterBufRing(bufRing, nBufs, 0us)
    let legacy = how = 0
    do
        if k = 0 then
            eprintfn "receive buffers: %s"
                (match how with
                 | 1 -> "buffer ring"
                 | 2 -> "buffer ring (registered on the retry with resv[0] = 1: Ubuntu 6.8 kernel)"
                 | _ -> "IORING_OP_PROVIDE_BUFFERS")
        if legacy then ring.Provide(0us, 0, nBufs, bufMem, BufSize, ud UdIgnore 0 0u)
    member val Rq = Array.zeroCreate<int> slots
    member val RqHead = 0 with get, set
    member val RqLen = 0 with get, set
    member val Live = 0 with get, set
    member val AcceptArmed = 0 with get, set
    member val Full = false with get, set
    member val Active = false with get, set
    member val High = false with get, set
    member _.Ring = ring
    member _.Buffer(bid: int) = bufMem + nativeint (bid * BufSize)
    member _.Recycle(bid: int) =
        if legacy then ring.Provide(0us, bid, 1, bufMem + nativeint (bid * BufSize), BufSize, ud UdIgnore 0 0u)
        else
            let e = bufRing + nativeint (int (bufTail &&& bufMask) * 16)
            wr64 e (uint64 (bufMem + nativeint (bid * BufSize)))
            wr32 (e + 8n) (uint32 BufSize)
            wr16 (e + 12n) (uint16 bid)
            bufTail <- bufTail + 1us
    /// makes the recycled buffers visible to the kernel (one store of the tail)
    member _.Publish() =
        if not legacy then storeRelease (NativePtr.ofNativeInt<uint16> (bufRing + 14n)) bufTail
    member this.Init() =
        if not legacy then
            for bid in 0 .. nBufs - 1 do
                this.Recycle bid
            this.Publish()

[<Sealed>]
type Worker(listenFds: int[], slots: int, nringsWanted: int, ck: Db.Checkpointer, started: int64) =
    let nl = listenFds.Length
    let mutable nrings = 0
    let R = Array.zeroCreate<RingState> nringsWanted
    let margin = min (slots / 4) 4096
    let highWater = slots - margin
    let lowGap = margin / 2
    let total = slots * nringsWanted
    // connections, by id = ring * slots + fixed-file slot
    // one record per connection (an array of structs: a connection's state shares a cache line or two)
    let conns = Array.zeroCreate<Conn> total
    let pool = Stack<Json.Out>()
    let cqes = List<Cqe>(8192)
    // the batch: requests read and not answered yet, reqs[reqHead .. nreq), oldest first
    let mutable reqs = Array.zeroCreate<Req> 1024
    let mutable reqHead = 0
    let mutable nreq = 0
    // the held queue: requests waiting out a write hold, wq[wqHead .. wqHead + wqLen), oldest first
    let mutable wq = Array.zeroCreate<Req> 1024
    let mutable wqHead = 0
    let mutable wqLen = 0
    let mutable warena = GC.AllocateUninitializedArray<byte>(1 <<< 16, pinned = true)
    let mutable warenaLen = 0
    let dirty = List<int>(1024)
    // request strings (new posts' text, token usernames) for the requests in the batch
    let mutable arena = GC.AllocateUninitializedArray<byte>(1 <<< 20, pinned = true)
    let mutable arenaLen = 0
    let mutable harena = GC.AllocateUninitializedArray<byte>(1 <<< 20, pinned = true)
    let hdr = Array.zeroCreate<byte> 160
    let mutable nrearm = 0
    let mutable passMs = 0u
    let mutable lastWriteMs = 0u
    let mutable nHeldNow = 0
    let mutable now = 0L
    let mutable uptime = 0L
    // checkpointer state
    let ckOn = not (isNull (box ck))
    let mutable ckBusy = false
    let mutable ckHold = false
    let mutable ckRewind = false
    let mutable ckFrom = 0
    let mutable ckBackfilled = 0
    let ckClock = Stopwatch()
    let ckSink = Marshal.AllocHGlobal 8
    let ckLog = Environment.GetEnvironmentVariable "CKLOG" = "1"
    // stats
    let mutable nobufs = 0L
    let mutable accErr = 0L
    let mutable lastAccErr = 0
    let mutable waitErrs = 0L
    let mutable maxBatch = 0
    let mutable stPasses = 0L
    let mutable stAns = 0L
    let mutable stWalk = 0L
    let mutable stCommitUs = 0L
    let mutable stCommitMax = 0L
    let mutable stCommits = 0L
    let mutable lastStats = 0L
    let mutable lastUtc = 0L

    let ringOf (cid: int) = R.[cid / slots].Ring
    let fslot (cid: int) = cid % slots

    let takeOut () = if pool.Count > 0 then pool.Pop() else Json.Out(poolCap)
    let giveOut (o: Json.Out) =
        if not (isNull o) then
            o.Shrink poolCap
            if pool.Count < poolMax then pool.Push o

    let reserveArena (n: int) =
        if arena.Length - arenaLen < n then
            let bigger = GC.AllocateUninitializedArray<byte>(max (arena.Length * 2) (arenaLen + n + 4096), pinned = true)
            Buffer.BlockCopy(arena, 0, bigger, 0, arenaLen)
            arena <- bigger

    let armRecv (cid: int) =
        (ringOf cid).RecvMulti(fslot cid, 0us, ud UdRecv cid conns.[cid].Gen)
        conns.[cid].Fl <- conns.[cid].Fl ||| RECV

    let submitClose (cid: int) =
        giveOut conns.[cid].Out
        conns.[cid].Out <- null
        giveOut conns.[cid].Wire
        conns.[cid].Wire <- null
        conns.[cid].In <- null
        conns.[cid].InLen <- 0
        let r = ringOf cid
        let slot = fslot cid
        if conns.[cid].Fl &&& RECV <> 0uy then
            // SHUTDOWN ends the multishot receive now (and sends the FIN), then the slot is closed
            let s = r.Sqe()
            wr8 s OP_SHUTDOWN
            wr8 (s + 1n) (SQE_FIXED_FILE ||| SQE_IO_HARDLINK ||| SQE_CQE_SKIP_SUCCESS)
            wri32 (s + 4n) slot
            wr32 (s + 24n) 2u // SHUT_RDWR
            wr64 (s + 32n) (ud UdClose cid NoGen)
        let c = r.Sqe()
        wr8 c OP_CLOSE
        wr32 (c + 44n) (uint32 (slot + 1))
        wr64 (c + 32n) (ud UdClose cid conns.[cid].Gen)

    /// stop using the connection: closes now, or after the send in flight
    let drop (cid: int) =
        let f = conns.[cid].Fl
        if f &&& LIVE <> 0uy then
            conns.[cid].Fl <- f &&& ~~~LIVE
            if f &&& SENDING <> 0uy then conns.[cid].Fl <- conns.[cid].Fl ||| CLOSEAFTER else submitClose cid

    let sendWire (cid: int) =
        let w = conns.[cid].Wire
        let off = conns.[cid].WireOff
        (ringOf cid).Send(fslot cid, true, Marshal.UnsafeAddrOfPinnedArrayElement(w.Wire, off), w.N - off, MSG_NOSIGNAL, ud UdSend cid conns.[cid].Gen)

    let startSend (cid: int) =
        conns.[cid].Wire <- conns.[cid].Out
        conns.[cid].WireOff <- conns.[cid].OutStart
        conns.[cid].Out <- null
        conns.[cid].Fl <- conns.[cid].Fl ||| SENDING
        sendWire cid

    // ---------- accept ----------

    let armAccept (k: int) =
        let rs = R.[k]
        for l in 0 .. nl - 1 do
            rs.Ring.AcceptMulti(listenFds.[l], ud UdAccept k (uint32 l))
        rs.AcceptArmed <- nl
        rs.Active <- true

    /// A full fixed-file table makes the kernel drop the connection it just accepted (ENFILE). So accept leaves a ring
    /// at a high-water mark while other rings have room below theirs; the slots above the mark absorb the burst still
    /// in flight and are used last.
    let acceptTarget () =
        let mutable q = -1
        let mutable i = 0
        while q < 0 && i < nrings do
            if not R.[i].Full && not R.[i].High then q <- i
            i <- i + 1
        i <- 0
        while q < 0 && i < nrings do
            if not R.[i].Full then q <- i
            i <- i + 1
        q

    let acceptArmedAnywhere () =
        let mutable a = false
        for i in 0 .. nrings - 1 do
            if R.[i].AcceptArmed > 0 then a <- true
        a

    let cancelAccept (k: int) =
        for l in 0 .. nl - 1 do
            R.[k].Ring.Cancel(ud UdAccept k (uint32 l), ud UdClose 0 NoGen)

    // ---------- checkpoints ----------
    // The checkpoint thread runs PASSIVE checkpoints on its own connection so the loop never waits on their fsyncs.
    // The loop asks for one every ckPass new WAL frames. Under constant writes a pass never ends with the WAL fully
    // copied, so it can't rewind. Past ckHoldAt frames, passes go on with writes flowing until one leaves at most
    // holdRem frames uncopied; then writes wait (reads are still answered) for one short pass, after which the next
    // write rewinds the WAL. A pass's time is mostly its random page writes and fsyncs, so a hold that starts only
    // when little is left stays short, and a long pass copies a page written many times once.

    let ckArmRead () = R.[0].Ring.Read(ck.Fd, ckSink, 8, ud UdCkpt 0 0u)

    let ckRequest () =
        ck.Request()
        ckBusy <- true
        ckFrom <- Db.walFrames

    let ckAfterFlush () =
        if ckOn then
            let f = Db.walFrames
            if f < ckBackfilled then ckBackfilled <- 0 // the WAL rewound
            if holdRem > 0 then
                // passes run without holding writes until one leaves at most holdRem frames uncopied; ckDone then holds
                // writes for one short pass that copies the rest, and the WAL rewinds
                if not ckHold && not ckBusy && (f - ckBackfilled >= ckPass || f >= ckHoldAt) then ckRequest ()
            elif not ckHold && f >= ckHoldAt then
                ckHold <- true
                ckClock.Restart()
                if not ckBusy then ckRequest ()
            elif not ckBusy && f - ckBackfilled >= ckPass then ckRequest ()

    let ckDone (res: int) =
        ckBusy <- false
        if res = 8 then
            if ck.Rc = 0 && ck.Backfilled >= 0 then ckBackfilled <- ck.Backfilled
            if ckLog then eprintfn "ckpt wall %.1f ms cpu %.1f ms rc %d backfilled %d frames %d hold %b" (float ck.WallUs / 1000.0) (float ck.CpuUs / 1000.0) ck.Rc ck.Backfilled Db.walFrames ckHold
            if holdRem > 0 && not ckHold && ck.Rc = 0 && Db.walFrames >= ckHoldAt then
                // past holdMax, hold anyway: passes aren't catching up with the writes
                if Db.walFrames - ckBackfilled <= holdRem || Db.walFrames >= holdMax then
                    ckHold <- true
                    ckClock.Restart()
                else ckRequest () // too much left for a short hold: one more pass with writes going on
            if ckHold then
                if Db.walFrames <> ckFrom then ckRequest () // frames came in during the pass: one more
                else
                    ckHold <- false
                    ckRewind <- true
                    if ckLog then eprintfn "hold %.1f ms frames %d rc %d backfilled %d nobufs %d" ckClock.Elapsed.TotalMilliseconds Db.walFrames ck.Rc ck.Backfilled nobufs
        ckArmRead ()

    // ---------- HTTP ----------

    let newReq (cid: int) (close: bool) : int =
        if nreq = reqs.Length then
            if reqHead > reqs.Length / 2 then
                Array.Copy(reqs, reqHead, reqs, 0, nreq - reqHead)
                nreq <- nreq - reqHead
                reqHead <- 0
            else
                let bigger = Array.zeroCreate<Req> (reqs.Length * 2)
                Array.Copy(reqs, bigger, nreq)
                reqs <- bigger
        let idx = nreq
        let r = &reqs.[idx]
        nreq <- nreq + 1
        r.Cid <- cid
        r.Gen <- conns.[cid].Gen
        r.Close <- close
        r.Route <- RErr
        r.Err <- ENotFound
        r.Held <- false
        r.Ms <- passMs
        conns.[cid].Queued <- conns.[cid].Queued + 1
        idx

    let fail (r: byref<Req>) (e: byte) =
        r.Route <- RErr
        r.Err <- e

    /// checks the bearer token; on success fills Uid (and the username in the arena when wantName). False: r is an error.
    let authed (r: byref<Req>) (p: ReadOnlySpan<byte>) (authOff: int) (authLen: int) (wantName: bool) =
        if authOff < 0 || authLen < 7 || not (p.Slice(authOff, 7).SequenceEqual(ReadOnlySpan kBearer)) then
            fail &r ENoToken
            false
        else
            let mutable sub = 0L
            let mutable name: string = null
            match Jwt.verify (p.Slice(authOff + 7, authLen - 7)) wantName &sub &name with
            | Jwt.Valid ->
                r.Uid <- sub
                if wantName then
                    let n = Encoding.UTF8.GetMaxByteCount name.Length
                    reserveArena n
                    r.NameOff <- arenaLen
                    r.NameLen <- Encoding.UTF8.GetBytes(name.AsSpan(), Span<byte>(arena, arenaLen, n))
                    arenaLen <- arenaLen + r.NameLen
                true
            | Jwt.Invalid ->
                fail &r EBadToken
                false
            | _ ->
                fail &r EBadPayload
                false

    /// POST /posts body: {"body": "..."} checked whole (it must be valid JSON), the text unescaped into the arena
    let createBody (r: byref<Req>) (body: ReadOnlySpan<byte>) =
        reserveArena (body.Length + 8)
        let at = arenaLen
        let mutable len = -1 // -1: no string "body" member
        let mutable reader = Utf8JsonReader(body)
        let mutable bad = false
        let mutable isObj = false
        try
            if not (reader.Read()) then bad <- true
            elif reader.TokenType <> JsonTokenType.StartObject then
                reader.Skip()
                if reader.Read() then bad <- true
            else
                isObj <- true
                let mutable go = true
                while go && reader.Read() do
                    if reader.TokenType = JsonTokenType.EndObject then go <- false
                    else
                        let isBody = reader.ValueTextEquals(ReadOnlySpan kBodyKey)
                        reader.Read() |> ignore
                        if isBody then
                            // the last duplicate wins, as in JSON.parse
                            if reader.TokenType = JsonTokenType.String then
                                len <-
                                    if reader.ValueIsEscaped then reader.CopyString(Span<byte>(arena, at, arena.Length - at))
                                    else
                                        reader.ValueSpan.CopyTo(Span<byte>(arena, at, arena.Length - at))
                                        reader.ValueSpan.Length
                            else
                                len <- -1
                                reader.Skip()
                        else reader.Skip()
                if reader.Read() then bad <- true
        with
        | :? JsonException
        | :? InvalidOperationException -> bad <- true
        if bad then fail &r EBadJson
        elif not isObj || len < 0 then fail &r ENoBody
        else
            // trimmed of whitespace, then counted in characters (code points)
            let mutable s = at
            let mutable e = at + len
            let inline ws (b: byte) = b = 32uy || (b >= 9uy && b <= 13uy)
            while s < e && ws arena.[s] do
                s <- s + 1
            while e > s && ws arena.[e - 1] do
                e <- e - 1
            if s = e then fail &r ENoBody
            else
                let mutable chars = 0
                for i in s .. e - 1 do
                    if arena.[i] &&& 0xC0uy <> 0x80uy then chars <- chars + 1
                if chars > 500 then fail &r ELongBody
                else
                    r.Route <- RCreate
                    r.BodyOff <- s
                    r.BodyLen <- e - s
                    arenaLen <- at + len

    /// Parses the complete requests in d. Returns the bytes used, or -1 to drop the connection.
    let parse (cid: int) (d: ReadOnlySpan<byte>) =
        let mutable off = 0
        let mutable result = -2
        while result = -2 do
            if off >= d.Length then result <- off
            else
                let p = d.Slice off
                let he = p.IndexOf(ReadOnlySpan crlf2)
                if he < 0 then result <- (if p.Length > MaxReq then -1 else off)
                else
                    let hlen = he + 4
                    let le = p.Slice(0, hlen).IndexOf '\r'B
                    let line = p.Slice(0, le)
                    let sp1 = line.IndexOf ' 'B
                    let sp2 = if sp1 < 0 then -1 else line.Slice(sp1 + 1).IndexOf ' 'B
                    if sp2 < 0 then result <- -1
                    else
                        let meth = line.Slice(0, sp1)
                        let mutable path = line.Slice(sp1 + 1, sp2)
                        let ver = line.Slice(sp1 + 2 + sp2)
                        let mutable close = ver.Length = 8 && ver.[7] = '0'B // HTTP/1.0
                        let mutable authOff = -1
                        let mutable authLen = 0
                        let mutable clen = 0
                        let mutable bad = false
                        let mutable h = le + 2
                        let hend = he + 2
                        while not bad && h < hend do
                            let eol = p.Slice(h, hend - h).IndexOf '\r'B
                            if eol < 0 then bad <- true
                            else
                                let hl = p.Slice(h, eol)
                                let colon = hl.IndexOf ':'B
                                if colon > 0 then
                                    let name = hl.Slice(0, colon)
                                    let mutable vs = colon + 1
                                    let mutable ve = eol
                                    while vs < ve && (hl.[vs] = 32uy || hl.[vs] = 9uy) do
                                        vs <- vs + 1
                                    while ve > vs && (hl.[ve - 1] = 32uy || hl.[ve - 1] = 9uy) do
                                        ve <- ve - 1
                                    let v = hl.Slice(vs, ve - vs)
                                    match name.Length with
                                    | 14 when Ascii.EqualsIgnoreCase(name, ReadOnlySpan kContentLength) ->
                                        clen <- 0
                                        for c in v do
                                            let dg = int c - 48
                                            if dg < 0 || dg > 9 then bad <- true
                                            elif not bad then
                                                clen <- clen * 10 + dg
                                                if clen > MaxReq then bad <- true
                                    | 13 when Ascii.EqualsIgnoreCase(name, ReadOnlySpan kAuthorization) ->
                                        authOff <- h + vs
                                        authLen <- v.Length
                                    | 10 when Ascii.EqualsIgnoreCase(name, ReadOnlySpan kConnection) ->
                                        if Ascii.EqualsIgnoreCase(v, ReadOnlySpan kClose) then close <- true
                                        elif Ascii.EqualsIgnoreCase(v, ReadOnlySpan kKeepAlive) then close <- false
                                    | 17 when Ascii.EqualsIgnoreCase(name, ReadOnlySpan kTransferEncoding) -> bad <- true // chunked: out of scope
                                    | _ -> ()
                                h <- h + eol + 2
                        let total = hlen + clen
                        if bad then result <- -1
                        elif p.Length < total then result <- (if total > MaxReq then -1 else off)
                        else
                            let body = p.Slice(hlen, clen)
                            let q = path.IndexOf '?'B
                            if q >= 0 then path <- path.Slice(0, q)
                            let ri = newReq cid close
                            let r = &reqs.[ri]
                            let get = meth.SequenceEqual(ReadOnlySpan kGet)
                            let post = meth.SequenceEqual(ReadOnlySpan kPost)
                            if get && path.SequenceEqual(ReadOnlySpan kFeed) then r.Route <- RFeed
                            elif path.Length > 7 && path.StartsWith(ReadOnlySpan kPostsSlash) then
                                let rest = path.Slice 7
                                let slash = rest.IndexOf '/'B
                                if get && slash < 0 then
                                    let id = parseId rest
                                    if id < 0L then fail &r EBadId
                                    else
                                        r.Route <- RPost
                                        r.Id <- id
                                elif post && slash >= 0 && rest.Slice(slash).SequenceEqual(ReadOnlySpan kLike) then
                                    // auth first, then the id
                                    if authed &r p authOff authLen false then
                                        let id = parseId (rest.Slice(0, slash))
                                        if id < 0L then fail &r EBadId
                                        else
                                            r.Route <- RLike
                                            r.Id <- id
                                else fail &r ENotFound
                            elif post && path.SequenceEqual(ReadOnlySpan kPosts) then
                                if authed &r p authOff authLen true then createBody &r body
                            elif get && path.SequenceEqual(ReadOnlySpan kHealth) then r.Route <- RHealth
                            else fail &r ENotFound
                            off <- off + total
                            if close then
                                conns.[cid].Fl <- (conns.[cid].Fl &&& ~~~LIVE) ||| CLOSEAFTER
                                result <- off
        result

    let onData (cid: int) (d: ReadOnlySpan<byte>) =
        let n = conns.[cid].InLen
        if n > 0 then
            let mutable b = conns.[cid].In
            if b.Length - n < d.Length then
                let bigger = Array.zeroCreate<byte> (max (b.Length * 2) (n + d.Length))
                Buffer.BlockCopy(b, 0, bigger, 0, n)
                b <- bigger
                conns.[cid].In <- b
            d.CopyTo(Span<byte>(b, n, d.Length))
            let all = n + d.Length
            let used = parse cid (ReadOnlySpan<byte>(b, 0, all))
            if used < 0 then drop cid
            else
                let left = all - used
                if left = 0 then
                    conns.[cid].In <- null
                    conns.[cid].InLen <- 0
                else
                    if used > 0 then Buffer.BlockCopy(b, used, b, 0, left)
                    conns.[cid].InLen <- left
        else
            let used = parse cid d
            if used < 0 then drop cid
            elif used < d.Length && conns.[cid].Fl &&& LIVE <> 0uy then
                let left = d.Length - used
                let b = Array.zeroCreate<byte> (max 2048 left)
                d.Slice(used).CopyTo(Span<byte> b)
                conns.[cid].In <- b
                conns.[cid].InLen <- left

    // ---------- answers ----------

    let body (r: byref<Req>) (o: Json.Out) : int =
        match r.Route with
        | RHealth ->
            if Db.health () then
                Json.put o (ReadOnlySpan jHealth)
                Json.putInt o uptime
                Json.putByte o '}'B
                200
            else
                Json.put o (ReadOnlySpan jDegraded)
                Json.putString o (ReadOnlySpan(Encoding.UTF8.GetBytes(Db.lastError ())))
                Json.putByte o '}'B
                503
        | RFeed -> if Db.feed o then 200 else (o.Clear(); Json.put o (ReadOnlySpan errBody.[int EInternal]); 500)
        | RPost ->
            match Db.post r.Id o with
            | 1 -> 200
            | 0 ->
                o.Clear()
                Json.put o (ReadOnlySpan errBody.[int ENoPost])
                404
            | _ ->
                o.Clear()
                Json.put o (ReadOnlySpan errBody.[int EInternal])
                500
        | RCreate ->
            let mutable created: byte[] = null
            let arena = if r.Held then warena else arena // a held post's strings are in the held queue's arena
            let id = Db.create r.Uid (Marshal.UnsafeAddrOfPinnedArrayElement(arena, r.BodyOff)) r.BodyLen &created
            if id < 0L then
                Json.put o (ReadOnlySpan errBody.[int EInternal])
                500
            else
                Json.put o (ReadOnlySpan jPostOpen)
                Json.putPost o id (ReadOnlySpan(arena, r.BodyOff, r.BodyLen)) (ReadOnlySpan created)
                    (ReadOnlySpan(arena, r.NameOff, r.NameLen)) 0L
                Json.putByte o '}'B
                201
        | RLike ->
            match Db.like r.Uid r.Id with
            | 1
            | 0 as v ->
                Json.put o (ReadOnlySpan jLiked)
                Json.put o (ReadOnlySpan(if v = 1 then jFalse else jTrue))
                Json.put o (ReadOnlySpan jPostId)
                Json.putInt o r.Id
                Json.putByte o '}'B
                if v = 1 then 201 else 200
            | 2 ->
                Json.put o (ReadOnlySpan errBody.[int ENoPost])
                404
            | _ ->
                Json.put o (ReadOnlySpan errBody.[int EInternal])
                500
        | _ ->
            Json.put o (ReadOnlySpan errBody.[int r.Err])
            errStatus.[int r.Err]

    /// The body goes into a pooled buffer after room for the head, and the head is written in front of it once the
    /// length is known: one contiguous send, no copy (unless an earlier answer on the connection waits to be sent).
    let answer (r: byref<Req>) (cid: int) =
        if r.Held then conns.[cid].Held <- conns.[cid].Held - 1
        conns.[cid].Queued <- conns.[cid].Queued - 1
        let o = takeOut ()
        let st = body &r o
        let line = statusLine st
        let mutable n = 0
        let h = Span<byte>(hdr)
        ReadOnlySpan(line).CopyTo h
        n <- line.Length
        ReadOnlySpan(hCtype).CopyTo(h.Slice n)
        n <- Json.intAt h (n + hCtype.Length) (int64 o.BodyLength)
        let tail = if r.Close then hEndClose else hEnd
        ReadOnlySpan(tail).CopyTo(h.Slice n)
        n <- n + tail.Length
        let start = Json.Head - n
        h.Slice(0, n).CopyTo(Span<byte>(o.Wire, start, n))
        let cur = conns.[cid].Out
        if isNull cur then
            conns.[cid].Out <- o
            conns.[cid].OutStart <- start
        else
            Json.put cur (ReadOnlySpan(o.Wire, start, o.N - start))
            giveOut o
        if conns.[cid].Fl &&& DIRTY = 0uy then
            conns.[cid].Fl <- conns.[cid].Fl ||| DIRTY
            dirty.Add cid

    /// Moves a request to the held queue: a write waiting out a hold, or a later request on a connection that has one
    /// there (a connection's requests keep their order). A new post's strings move to the queue's own arena, so the
    /// batch's arena can be reset while they wait. Each request is moved once; passes never walk the held queue.
    let toHeld (r: byref<Req>) (cid: int) =
        if not r.Held then
            r.Held <- true
            conns.[cid].Held <- conns.[cid].Held + 1
            if r.Route = RCreate then
                let n = r.NameLen + r.BodyLen
                if warena.Length - warenaLen < n then
                    let bigger = GC.AllocateUninitializedArray<byte>(max (warena.Length * 2) (warenaLen + n + 4096), pinned = true)
                    Buffer.BlockCopy(warena, 0, bigger, 0, warenaLen)
                    warena <- bigger
                Buffer.BlockCopy(arena, r.NameOff, warena, warenaLen, r.NameLen)
                r.NameOff <- warenaLen
                warenaLen <- warenaLen + r.NameLen
                Buffer.BlockCopy(arena, r.BodyOff, warena, warenaLen, r.BodyLen)
                r.BodyOff <- warenaLen
                warenaLen <- warenaLen + r.BodyLen
        if wqHead + wqLen = wq.Length then
            if wqHead > 0 then
                Array.Copy(wq, wqHead, wq, 0, wqLen)
                wqHead <- 0
            if wqLen = wq.Length then
                let bigger = Array.zeroCreate<Req> (wq.Length * 2)
                Array.Copy(wq, bigger, wqLen)
                wq <- bigger
        wq.[wqHead + wqLen] <- r
        wqLen <- wqLen + 1

    /// Runs one pass in one transaction. While a WAL hold or the write window is on, writes wait in the held queue
    /// (with every later request on their connections) while reads are answered. When the hold ends, the held queue is
    /// answered first, oldest first, up to wqMax requests in the pass's one commit.
    let flush () =
        uptime <- now - started
        let isWin = wwin > 0u && passMs - lastWriteMs < wwin
        let mutable hold = ckHold || isWin
        let mutable oneWrite = ckRewind && oneWriteOn && not hold
        let first = reqHead
        let drain = not hold && wqLen > 0
        if drain || nreq > first then
            let budget = if answers > 0 then answers else Int32.MaxValue
            let last = if nreq - first > budget then first + budget else nreq
            // A request answered more than 40 ms after it arrived costs the kernel a delayed-ACK timer and a bare ACK.
            // In a backlog, oldest-first makes every newcomer wait that long too. So past one pass of backlog the
            // newest go first; the old end gets a share of the pass from lifoAge on (all of it at twice that age), so
            // waiting has a bound. A connection's own requests keep their order.
            let lifo = lifoOn && nreq - first > budget
            let mutable oldShare = budget
            if lifo then
                let age = passMs - reqs.[first].Ms
                oldShare <- budget * (if age >= lifoAge * 2u then 8 elif age >= lifoAge then lifoCap else lifoMin) / 8
            let mutable db = drain || lifo
            let mutable j = first
            while not db && j < last do
                if reqs.[j].Route <> RErr then db <- true
                j <- j + 1
            let mutable wrote = false
            let mutable answered = 0
            let txn = db && Db.begin' ()
            // the held queue first
            if drain then
                let mutable n = 0
                let mutable stopW = false
                while not stopW && wqLen > 0 && n < wqMax do
                    let r = &wq.[wqHead]
                    let cid = r.Cid
                    let mutable take = true
                    if conns.[cid].Gen = r.Gen then
                        if conns.[cid].Fl &&& (LIVE ||| CLOSEAFTER) = 0uy then
                            conns.[cid].Queued <- conns.[cid].Queued - 1
                            conns.[cid].Held <- conns.[cid].Held - 1
                        else
                            let isWrite = r.Route = RCreate || r.Route = RLike
                            if isWrite && hold then
                                // the first commit after a rewind carried its one write: the rest wait
                                take <- false
                                stopW <- true
                            else
                                answer &r cid
                                n <- n + 1
                                if isWrite then
                                    wrote <- true
                                    ckRewind <- false
                                    // the first commit after a rewind still overwrites old WAL pages, each read back
                                    // from disk first when reclaim evicted it: it carries one write, not a batch
                                    if oneWrite then
                                        oneWrite <- false
                                        hold <- true
                    if take then
                        wqHead <- wqHead + 1
                        wqLen <- wqLen - 1
                        stWalk <- stWalk + 1L
                if wqLen = 0 then
                    wqHead <- 0
                    warenaLen <- 0
            let mutable i = first
            let mutable stop = false
            while not stop && i < nreq && answered < budget do
                if lifo && answered >= oldShare then stop <- true
                else
                    stWalk <- stWalk + 1L
                    let r = &reqs.[i]
                    let cid = r.Cid
                    if conns.[cid].Gen = r.Gen then
                        if conns.[cid].Fl &&& (LIVE ||| CLOSEAFTER) = 0uy then conns.[cid].Queued <- conns.[cid].Queued - 1
                        else
                            let isWrite = r.Route = RCreate || r.Route = RLike
                            if conns.[cid].Held > 0 || (hold && isWrite) then toHeld &r cid
                            else
                                answer &r cid
                                answered <- answered + 1
                                if isWrite then
                                    wrote <- true
                                    ckRewind <- false
                                    if oneWrite then
                                        oneWrite <- false
                                        hold <- true
                    i <- i + 1
            if lifo then
                let mutable k = nreq
                let mutable stop2 = false
                while not stop2 && k > i && answered < budget do
                    stWalk <- stWalk + 1L
                    let r = &reqs.[k - 1]
                    let cid = r.Cid
                    if conns.[cid].Gen = r.Gen then
                        if conns.[cid].Fl &&& (LIVE ||| CLOSEAFTER) = 0uy then
                            conns.[cid].Queued <- conns.[cid].Queued - 1
                            k <- k - 1
                        elif conns.[cid].Queued - conns.[cid].Held > 1 then stop2 <- true // pipelined: its older request goes first
                        else
                            let isWrite = r.Route = RCreate || r.Route = RLike
                            if conns.[cid].Held > 0 || (hold && isWrite) then toHeld &r cid
                            else
                                answer &r cid
                                answered <- answered + 1
                                if isWrite then
                                    wrote <- true
                                    ckRewind <- false
                                    if oneWrite then
                                        oneWrite <- false
                                        hold <- true
                            k <- k - 1
                    else k <- k - 1
                nreq <- k
            if wrote then lastWriteMs <- passMs
            nHeldNow <- wqLen
            let mutable ok = true
            let c0 = if statsOn then Diagnostics.Stopwatch.GetTimestamp() else 0L
            if txn && not (Db.commit ()) then
                ok <- false
                Db.rollback ()
            if statsOn && txn then
                let us = int64 (Diagnostics.Stopwatch.GetElapsedTime(c0).TotalMicroseconds)
                stCommitUs <- stCommitUs + us
                stCommits <- stCommits + 1L
                if us > stCommitMax then stCommitMax <- us
            elif not txn then Db.rollback () // closes cursors (and an implicit read) when no BEGIN ran
            if not ok && wrote then
                // never acknowledge an uncommitted write: every connection of this pass closes without an answer
                for d in 0 .. dirty.Count - 1 do
                    let cid = dirty.[d]
                    giveOut conns.[cid].Out
                    conns.[cid].Out <- null
                    drop cid
            for d in 0 .. dirty.Count - 1 do
                let cid = dirty.[d]
                let f = conns.[cid].Fl &&& ~~~DIRTY
                conns.[cid].Fl <- f
                if not (isNull conns.[cid].Out) && f &&& SENDING = 0uy then startSend cid
                elif conns.[cid].Queued = 0 && f &&& (LIVE ||| SENDING) = 0uy && f &&& CLOSEAFTER <> 0uy then
                    conns.[cid].Fl <- f &&& ~~~CLOSEAFTER
                    submitClose cid
            dirty.Clear()
            reqHead <- i
            if reqHead = nreq then
                reqHead <- 0
                nreq <- 0
                arenaLen <- 0
            elif arenaLen > (16 <<< 20) then
                // a backlog that never empties: keep only the strings still waiting
                if harena.Length < arenaLen then harena <- GC.AllocateUninitializedArray<byte>(arena.Length, pinned = true)
                let mutable hl = 0
                for k in reqHead .. nreq - 1 do
                    let r = &reqs.[k]
                    if r.Route = RCreate then
                        Buffer.BlockCopy(arena, r.NameOff, harena, hl, r.NameLen)
                        r.NameOff <- hl
                        hl <- hl + r.NameLen
                        Buffer.BlockCopy(arena, r.BodyOff, harena, hl, r.BodyLen)
                        r.BodyOff <- hl
                        hl <- hl + r.BodyLen
                let t = arena
                arena <- harena
                harena <- t
                arenaLen <- hl

    // ---------- completions ----------

    let onCqe (k: int) (e: Cqe) =
        let u = e.U
        let res = e.Res
        let flags = e.Flags
        let cid = udSlot u
        match udT u with
        | 2UL -> // receive
            let rs = R.[k]
            let hasBuf = flags &&& CQE_F_BUFFER <> 0u
            let bid = int (flags >>> 16)
            if conns.[cid].Gen <> udGen u then (if hasBuf then rs.Recycle bid)
            else
                if flags &&& CQE_F_MORE = 0u then conns.[cid].Fl <- conns.[cid].Fl &&& ~~~RECV
                if res > 0 then
                    if conns.[cid].Fl &&& LIVE <> 0uy then
                        onData cid (ReadOnlySpan<byte>(NativePtr.toVoidPtr (NativePtr.ofNativeInt<byte> (rs.Buffer bid)), res))
                    rs.Recycle bid
                    if conns.[cid].Fl &&& (RECV ||| LIVE) = LIVE then armRecv cid
                else
                    if hasBuf then rs.Recycle bid
                    if res = -ENOBUFS then
                        nobufs <- nobufs + 1L
                        if conns.[cid].Fl &&& (LIVE ||| REARM) = LIVE then
                            conns.[cid].Fl <- conns.[cid].Fl ||| REARM
                            rs.Rq.[(rs.RqHead + rs.RqLen) % slots] <- cid
                            rs.RqLen <- rs.RqLen + 1
                            nrearm <- nrearm + 1
                    elif conns.[cid].Fl &&& LIVE <> 0uy then drop cid
        | 3UL -> // send
            if conns.[cid].Gen = udGen u then
                let w = conns.[cid].Wire
                if res > 0 && conns.[cid].WireOff + res < w.N then
                    conns.[cid].WireOff <- conns.[cid].WireOff + res
                    sendWire cid
                else
                    conns.[cid].Fl <- conns.[cid].Fl &&& ~~~SENDING
                    giveOut w
                    conns.[cid].Wire <- null
                    if res <= 0 then conns.[cid].Fl <- (conns.[cid].Fl &&& ~~~LIVE) ||| CLOSEAFTER
                    if conns.[cid].Fl &&& CLOSEAFTER <> 0uy then
                        if conns.[cid].Queued = 0 then // else the pass closes it after its last answer
                            conns.[cid].Fl <- conns.[cid].Fl &&& ~~~(CLOSEAFTER ||| LIVE)
                            submitClose cid
                    elif not (isNull conns.[cid].Out) then startSend cid // what waits in out is committed
        | 1UL -> // accept
            let rs = R.[k]
            if flags &&& CQE_F_MORE = 0u then
                rs.AcceptArmed <- rs.AcceptArmed - 1
                if res = -23 || res = -24 then // ENFILE / EMFILE: this ring's table is full
                    rs.Full <- true
                    if rs.AcceptArmed = 0 then
                        let q = acceptTarget ()
                        if q >= 0 && R.[q].AcceptArmed = 0 then armAccept q
                elif res <> -ECANCELED && not rs.Full && not rs.High then
                    // one listener's multishot ended for another reason: armed again on this ring
                    rs.Ring.AcceptMulti(listenFds.[int (udGen u)], u)
                    rs.AcceptArmed <- rs.AcceptArmed + 1
            if res < 0 then
                accErr <- accErr + 1L
                lastAccErr <- res
            else
                let c = k * slots + res
                conns.[c].Gen <- (conns.[c].Gen + 1u) &&& 0xFFFFFFu
                conns.[c].Fl <- LIVE
                conns.[c].Queued <- 0
                conns.[c].Held <- 0
                conns.[c].In <- null
                conns.[c].InLen <- 0
                rs.Live <- rs.Live + 1
                armRecv c
                if rs.Live >= highWater && not rs.High then
                    rs.High <- true
                    let q = acceptTarget ()
                    if q >= 0 && not R.[q].High && rs.AcceptArmed > 0 then
                        cancelAccept k
                        if R.[q].AcceptArmed = 0 then armAccept q
        | 4UL -> // close
            if udGen u <> NoGen then
                let rs = R.[k]
                rs.Live <- rs.Live - 1
                rs.Full <- false
                if rs.High && rs.Live + lowGap < highWater then rs.High <- false
        | 6UL -> ckDone res
        | _ -> ()

    let reap (k: int) =
        let rs = R.[k]
        cqes.Clear()
        rs.Ring.Reap(cqes, Int32.MaxValue) |> ignore
        for i in 0 .. cqes.Count - 1 do
            onCqe k cqes.[i]
        rs.Publish()

    let stats () =
        let mutable live = 0
        let mutable ovf = 0u
        for k in 0 .. nrings - 1 do
            live <- live + R.[k].Live
            ovf <- ovf + R.[k].Ring.CqOverflow
        eprintfn "stats t=%d live=%d accErr=%d last=%d cqDropped=%d nobufs=%d rq=%d maxBatch=%d pool=%d cacheKB=%d passes=%d ans=%d wal=%d gc0=%d gc2=%d heapMB=%d walk=%d held=%d commits=%d commitMs=%d commitMaxUs=%d gathered=%d through=%d late=%d parked=%d"
            (now - started) live accErr lastAccErr ovf nobufs nrearm maxBatch pool.Count (Db.cacheKB ()) stPasses stAns
            Db.walFrames (GC.CollectionCount 0) (GC.CollectionCount 2) (GC.GetTotalMemory false >>> 20) stWalk wqLen
            stCommits (stCommitUs / 1000L) stCommitMax Vfs.gathered Vfs.through Vfs.late Pcache.parked
        stCommits <- 0L
        stCommitUs <- 0L
        stCommitMax <- 0L
        stWalk <- 0L
        maxBatch <- 0
        stPasses <- 0L
        stAns <- 0L

    member _.Run() =
        for k in 0 .. nringsWanted - 1 do
            if nrings = k then
                try
                    let rs = RingState(k, slots)
                    rs.Init()
                    R.[k] <- rs
                    nrings <- k + 1
                with e ->
                    if k = 0 then raise e
                    eprintfn "serving on %d rings (%s)" k e.Message
        armAccept 0
        if ckOn then ckArmRead ()
        let mutable lastReq = 0
        let mutable idlePasses = 0
        while true do
            let mutable others = false
            let mutable ready = false
            for k in 1 .. nrings - 1 do
                let rs = R.[k]
                if rs.Active then
                    others <- true
                    // sends go out and this ring's completions run now; it isn't entered again until the next pass.
                    // A ring with nothing queued and no completion work flagged by the kernel isn't entered at all.
                    if not taskrunFlag || rs.Ring.HasWork then rs.Ring.Enter(0u, 0L) |> ignore
                    if rs.Ring.CqReady > 0u then ready <- true
            let work = (nreq > reqHead || (wqLen > 0 && uint32 Environment.TickCount64 - lastWriteMs >= wwin)) && not ckHold
            // under load, wait a little for more completions: bigger passes share one COMMIT and one enter
            let ret =
                if work || ready then R.[0].Ring.Enter(0u, 0L)
                elif batchN > 1 && (lastReq >= batchThresh || nrearm > 0 || others || nHeldNow > 0) then
                    let us = if others && idlePasses > 64 then 5000 else batchWaitUs
                    R.[0].Ring.Enter(uint32 batchN, int64 us * 1000L)
                else R.[0].Ring.Enter(1u, 0L)
            if ret < 0L then
                let e = Native.errno ()
                // EINTR, ETIME, EBUSY, EAGAIN are expected; never exit (a dead process leaves no listener)
                if e <> 4 && e <> 62 && e <> 16 && e <> 11 then
                    waitErrs <- waitErrs + 1L
                    if waitErrs < 20L || waitErrs &&& 1023L = 0L then eprintfn "io_uring_enter: errno %d (%d)" e waitErrs
                    if e <> 53 then Threading.Thread.Sleep 1 // EBADR: completions were dropped; go on
            let t = Environment.TickCount64
            passMs <- uint32 t
            if t - lastUtc >= 200L then
                lastUtc <- t
                now <- DateTimeOffset.UtcNow.ToUnixTimeSeconds()
            for k in 0 .. nrings - 1 do
                if k = 0 || R.[k].Active then reap k
            lastReq <- nreq - reqHead
            if lastReq > maxBatch then maxBatch <- lastReq
            idlePasses <- if lastReq > 0 then 0 else idlePasses + 1
            if ckHold && ckClock.ElapsedMilliseconds > holdMs then // the checkpoint pass is stuck: don't wait longer
                ckHold <- false
                if ckLog then eprintfn "hold timeout frames %d" Db.walFrames
            if nreq > reqHead || wqLen > 0 then
                let before = nreq - reqHead
                flush ()
                stAns <- stAns + int64 (before - (nreq - reqHead))
                stPasses <- stPasses + 1L
                ckAfterFlush ()
            if nrearm > 0 then
                // A burst empties the buffer ring and ends every other armed receive with ENOBUFS. Re-arming them all
                // at once only ends them again, so a pass re-arms up to rearmMax per ring, oldest first.
                for k in 0 .. nrings - 1 do
                    let rs = R.[k]
                    let n = if rearmMax > 0 then min rs.RqLen rearmMax else rs.RqLen
                    for _ in 1 .. n do
                        let cid = rs.Rq.[rs.RqHead]
                        rs.RqHead <- (rs.RqHead + 1) % slots
                        conns.[cid].Fl <- conns.[cid].Fl &&& ~~~REARM
                        if conns.[cid].Fl &&& (LIVE ||| RECV) = LIVE then armRecv cid
                    rs.RqLen <- rs.RqLen - n
                    nrearm <- nrearm - n
            if not (acceptArmedAnywhere ()) then
                let q = acceptTarget ()
                if q >= 0 then armAccept q
            if statsOn && now <> lastStats then
                lastStats <- now
                stats ()

/// Listeners: `count` SO_REUSEPORT sockets on host:port, each with its own accept queue (somaxconn deep).
let listen (ip: Net.IPAddress) (port: int) (count: int) (deferAccept: int) =
    [| for _ in 1 .. count do
           let fd = Native.socket (2, 1 ||| 0x80000, 0) // AF_INET, SOCK_STREAM | SOCK_CLOEXEC
           if fd < 0 then failwithf "socket: errno %d" (Native.errno ())
           let mutable one = 1
           Native.setsockopt (fd, 1, 2, &one, 4) |> ignore // SO_REUSEADDR
           Native.setsockopt (fd, 1, 15, &one, 4) |> ignore // SO_REUSEPORT
           Native.setsockopt (fd, 6, 1, &one, 4) |> ignore // TCP_NODELAY
           if deferAccept > 0 then
               let mutable d = deferAccept
               Native.setsockopt (fd, 6, 9, &d, 4) |> ignore // TCP_DEFER_ACCEPT
           let sa = Marshal.AllocHGlobal 16
           NativeMemory.Clear(NativePtr.toVoidPtr (NativePtr.ofNativeInt<byte> sa), unativeint 16)
           wr16 sa 2us
           wr8 (sa + 2n) (byte (port >>> 8))
           wr8 (sa + 3n) (byte port)
           let a = ip.GetAddressBytes()
           for i in 0 .. 3 do
               wr8 (sa + 4n + nativeint i) a.[i]
           if Native.bind (fd, sa, 16) < 0 then failwithf "bind %O:%d: errno %d" ip port (Native.errno ())
           Marshal.FreeHGlobal sa
           if Native.listen (fd, 65535) < 0 then failwithf "listen: errno %d" (Native.errno ())
           yield fd |]

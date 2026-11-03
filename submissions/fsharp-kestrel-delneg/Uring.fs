/// A Kestrel transport (IConnectionListenerFactory) on Linux io_uring (ideas: #152, #82, #145, #137).
/// One thread owns every ring and runs every request, so the database needs no lock.
/// - Accepted sockets go straight into the ring's fixed-file table (multishot accept with IORING_FILE_INDEX_ALLOC):
///   they never get a process fd, so LimitNOFILE=65535 does not cap keep-alive connections. A ring holds one table
///   of up to 65,535 sockets; when it fills up, the next ring takes the new connections (up to 8 rings).
/// - Each socket has one multishot recv into the ring's provided buffer ring. The loop handles a received buffer
///   in place and gives it back to the kernel at once.
/// - The responses of one pass go into one buffer, after the COMMIT of the pass's writes (group commit). Each one
///   goes out with a send that has MSG_DONTWAIT: it finishes inside the submitting io_uring_enter. A response the
///   socket does not take at once is copied into the connection's own buffer and sent again with a normal send.
/// - The rings use SINGLE_ISSUER | DEFER_TASKRUN: completion work runs only when the loop enters the ring. The loop
///   waits in ring 0 for 32 completions or 2 ms, so one pass serves many sockets (as the epoll loop's 2 ms sleep).
/// - Kernel 6.8: a closed fixed file is released only after all older requests on its ring finish (the multishot
///   recvs of the other sockets never do), so a close is a SHUTDOWN (sends the FIN now) hard-linked to the CLOSE (#152).
module Uring

#nowarn "9"

open System
open System.Buffers
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

module private Sys =
    [<Literal>]
    let L = "libc.so.6"
    [<DllImport(L, SetLastError = true)>]
    extern int64 syscall(int64 n, int64 a1, int64 a2, int64 a3, int64 a4, int64 a5, int64 a6)
    [<DllImport(L, SetLastError = true)>]
    extern nativeint mmap(nativeint addr, unativeint len, int prot, int flags, int fd, int64 off)
    [<DllImport(L, SetLastError = true)>]
    extern int munmap(nativeint addr, unativeint len)
    [<DllImport(L, SetLastError = true)>]
    extern int close(int fd)
    [<DllImport(L, SetLastError = true)>]
    extern int socket(int domain, int typ, int protocol)
    [<DllImport(L, SetLastError = true)>]
    extern int setsockopt(int fd, int level, int name, int& value, int len)
    [<DllImport(L, SetLastError = true)>]
    extern int bind(int fd, nativeint addr, int len)
    [<DllImport(L, SetLastError = true)>]
    extern int listen(int fd, int backlog)
    [<DllImport(L, SetLastError = true)>]
    extern int getrlimit(int resource, uint64[] rlim)

let private SYS_setup = 425L
let private SYS_enter = 426L
let private SYS_register = 427L

// io_uring_setup flags and features
let private SETUP_CQSIZE = 1u <<< 3
let private SETUP_SUBMIT_ALL = 1u <<< 7
let private SETUP_COOP_TASKRUN = 1u <<< 8
let private SETUP_SINGLE_ISSUER = 1u <<< 12
let private SETUP_DEFER_TASKRUN = 1u <<< 13
let private FEAT_SINGLE_MMAP = 1u
let private FEAT_EXT_ARG = 1u <<< 8
// io_uring_enter flags
let private ENTER_GETEVENTS = 1L
let private ENTER_EXT_ARG = 8L
// register opcodes
let private REGISTER_FILES2 = 13L
let private REGISTER_PBUF_RING = 22L
// opcodes
let private OP_ACCEPT = 13uy
let private OP_ASYNC_CANCEL = 14uy
let private OP_CLOSE = 19uy
let private OP_SEND = 26uy
let private OP_RECV = 27uy
let private OP_SHUTDOWN = 34uy
let private OP_PROVIDE_BUFFERS = 31uy
// sqe flags
let private SQE_FIXED_FILE = 1uy
let private SQE_IO_HARDLINK = 1uy <<< 3
let private SQE_BUFFER_SELECT = 1uy <<< 5
let private SQE_CQE_SKIP_SUCCESS = 1uy <<< 6
// cqe flags
let private CQE_F_BUFFER = 1u
let private CQE_F_MORE = 2u
let private ACCEPT_MULTISHOT = 1us
let private RECV_MULTISHOT = 2us
let private FILE_INDEX_ALLOC = 0xFFFFFFFFu

let private EAGAIN = 11
let private EINTR = 4
let private ENOBUFS = 105
let private MSG_DONTWAIT = 0x40u
let private MSG_NOSIGNAL = 0x4000u

let private AF_INET = 2
let private SOCK_STREAM = 1
let private SOCK_CLOEXEC = 0x80000
let private SOL_SOCKET = 1
let private SO_REUSEADDR = 2
let private IPPROTO_TCP = 6
let private TCP_NODELAY = 1

/// Ring sizes. CQ 4x the SQ: a pass can take many completions.
let private SqEntries = 4096u
let private CqEntries = 16384u
/// Provided receive buffers per ring: 8,192 x 1 KB (8 MB; the ring hands them out in turn, so all of it gets
/// touched). A request is a few hundred bytes (the token alone is ~190); a bigger one arrives in parts. 2 KB buffers
/// cost 8 MB more per ring (peak RSS at 320,000 users 259 vs 210 MB) for no CPU difference. With 2,048, a saturated loop ran out of
/// buffers (ENOBUFS, receives armed again and again) and fell behind: 400,000 users passed half as often.
/// Tuning knobs for experiments (the defaults are the measured best): FS3_NBUFS (buffers per ring, a power of 2),
/// FS3_WAIT_N (completions the loop waits for) and FS3_WAIT_US (the longest wait).
let private envInt name dflt =
    match Int32.TryParse(Environment.GetEnvironmentVariable(name: string)) with
    | true, v when v > 0 -> v
    | _ -> dflt
let private NBufs =
    let n = envInt "FS3_NBUFS" 8192
    if n &&& (n - 1) = 0 && n <= 32768 then n else 8192
let private WaitN = uint32 (envInt "FS3_WAIT_N" 32)
let private WaitNs = int64 (envInt "FS3_WAIT_US" 2000) * 1000L
let private BufSize = 1024
let private BufGroup = 0us
let private MaxRings = 8
/// Completions handled in one pass at most (the rest wait for the next pass), so that one pass stays short.
let private PassCap = 4096
/// Accept moves to the next ring when a ring has fewer free slots than this: multishot accept goes on in the kernel
/// between passes, and a full table makes the kernel drop the socket it just accepted (#154 uses 4,096 too).
let private reserveOf (slots: int) = min 4096 (slots / 4)

// user_data: op (3 bits) | ring (3) | slot (16) | gen (24) | extra (18)
let private OpAccept = 1UL
let private OpRecv = 2UL
let private OpSendArena = 3UL
let private OpSendOwn = 4UL
let private OpIgnore = 5UL

/// user_data without the op and extra bits: the key of a (ring, slot, generation).
let inline private u64key (ring: int) (slot: int) (gen: uint32) =
    (uint64 ring <<< 3) ||| (uint64 slot <<< 6) ||| (uint64 (gen &&& 0xFFFFFFu) <<< 22)

let inline private ud (op: uint64) (ring: int) (slot: int) (gen: uint32) (extra: int) =
    op ||| (uint64 ring <<< 3) ||| (uint64 slot <<< 6) ||| (uint64 (gen &&& 0xFFFFFFu) <<< 22) ||| (uint64 extra <<< 46)

// slot state bits
let private Live = 1uy
let private Recving = 2uy
let private Sending = 4uy // a send from the slot's own buffer is in flight
let private CloseAfter = 8uy // close when the in-flight send is done
let private ContinueSent = 16uy
let private Closing = 32uy
let private Done = 64uy // answered a request with "Connection: close": later bytes are ignored
/// A write request waits in the input while Db.holdWrites is set; the slot is in `held`.
let private HeldW = 128uy

let private errno () = Marshal.GetLastPInvokeError()

let inline private rd32 (p: nativeint) = NativePtr.read (NativePtr.ofNativeInt<uint32> p)
/// Shared ring indexes: a full fence keeps the SQE/CQE accesses on the right side (a few per pass, so cheap).
let inline private loadAcquire (p: nativeptr<'T>) =
    let v = NativePtr.read p
    Interlocked.MemoryBarrier()
    v
let inline private storeRelease (p: nativeptr<'T>) (v: 'T) =
    Interlocked.MemoryBarrier()
    NativePtr.write p v

/// The process's fd limit (soft RLIMIT_NOFILE; .NET raises it to the hard limit at start). A ring's fixed-file table
/// can be at most this big.
let private FdLimit =
    let lim = Array.zeroCreate<uint64> 2
    let soft = if Sys.getrlimit(7, lim) = 0 then lim.[0] else 65535UL
    int (min soft 65535UL)

/// One io_uring instance: a fixed-file table of up to FdLimit sockets, its receive buffers and its slots. (The kernel
/// keeps about 3.75 KB per connection: socket, inode, file.)
type private Ring(index: int) =
    let p = Marshal.AllocHGlobal 120
    do
        NativeMemory.Clear(NativePtr.toVoidPtr (NativePtr.ofNativeInt<byte> p), unativeint 120)
        NativePtr.write (NativePtr.ofNativeInt<uint32> (p + 4n)) CqEntries
        NativePtr.write (NativePtr.ofNativeInt<uint32> (p + 8n))
            (SETUP_CQSIZE ||| SETUP_SUBMIT_ALL ||| SETUP_COOP_TASKRUN ||| SETUP_SINGLE_ISSUER ||| SETUP_DEFER_TASKRUN)
    let fd =
        let r = Sys.syscall(SYS_setup, int64 SqEntries, int64 p, 0L, 0L, 0L, 0L)
        if r < 0L then
            let e = errno ()
            Marshal.FreeHGlobal p
            raise (InvalidOperationException(sprintf "io_uring_setup: errno %d" e))
        int r
    let features = rd32 (p + 20n)
    do
        if features &&& FEAT_SINGLE_MMAP = 0u || features &&& FEAT_EXT_ARG = 0u then
            raise (InvalidOperationException "io_uring: kernel too old (needs SINGLE_MMAP and EXT_ARG)")
    let sqEntries = rd32 (p + 0n)
    let cqEntries = rd32 (p + 4n)
    // sq_off at 40, cq_off at 80
    let sqOff i = rd32 (p + 40n + nativeint (4 * i))
    let cqOff i = rd32 (p + 80n + nativeint (4 * i))
    let sqSize = int (sqOff 6) + int sqEntries * 4
    let cqSize = int (cqOff 5) + int cqEntries * 16
    let ringSize = max sqSize cqSize
    let ringMem =
        let m = Sys.mmap(0n, unativeint ringSize, 3, 0x1 ||| 0x8000, fd, 0L) // PROT_READ|WRITE, MAP_SHARED|POPULATE
        if m = -1n then raise (InvalidOperationException(sprintf "io_uring mmap: errno %d" (errno ())))
        m
    let sqesMem =
        let m = Sys.mmap(0n, unativeint (int sqEntries * 64), 3, 0x1 ||| 0x8000, fd, 0x10000000L)
        if m = -1n then raise (InvalidOperationException(sprintf "io_uring mmap sqes: errno %d" (errno ())))
        m
    let sqHead = NativePtr.ofNativeInt<uint32> (ringMem + nativeint (sqOff 0))
    let sqTail = NativePtr.ofNativeInt<uint32> (ringMem + nativeint (sqOff 1))
    let sqMask = rd32 (ringMem + nativeint (sqOff 2))
    let sqArray = ringMem + nativeint (sqOff 6)
    let cqHead = NativePtr.ofNativeInt<uint32> (ringMem + nativeint (cqOff 0))
    let cqTail = NativePtr.ofNativeInt<uint32> (ringMem + nativeint (cqOff 1))
    let cqMask = rd32 (ringMem + nativeint (cqOff 2))
    let cqes = ringMem + nativeint (cqOff 5)
    do
        for i in 0 .. int sqEntries - 1 do
            NativePtr.write (NativePtr.ofNativeInt<uint32> (sqArray + nativeint (4 * i))) (uint32 i)
        Marshal.FreeHGlobal p
    let mutable tail = NativePtr.read sqTail // our SQ tail, published before each enter
    let mutable published = tail

    let register (op: int64) (arg: nativeint) (n: int64) =
        let r = Sys.syscall(SYS_register, int64 fd, op, int64 arg, n, 0L, 0L)
        if r < 0L then raise (InvalidOperationException(sprintf "io_uring_register %d: errno %d" op (errno ())))

    /// Fixed-file table: as many slots as the fd limit allows (the kernel caps a table at RLIMIT_NOFILE).
    let slots = FdLimit
    do
        let reg = Marshal.AllocHGlobal 32
        NativeMemory.Clear(NativePtr.toVoidPtr (NativePtr.ofNativeInt<byte> reg), unativeint 32)
        NativePtr.write (NativePtr.ofNativeInt<uint32> reg) (uint32 slots)
        NativePtr.write (NativePtr.ofNativeInt<uint32> (reg + 4n)) 1u // IORING_RSRC_REGISTER_SPARSE
        try register REGISTER_FILES2 reg 32L finally Marshal.FreeHGlobal reg

    // Provided buffer ring: NBufs entries of 16 bytes (addr u64, len u32, bid u16, the ring tail in entry 0's last u16).
    let bufRing =
        let m = Sys.mmap(0n, unativeint (NBufs * 16), 3, 0x2 ||| 0x20, -1, 0L) // MAP_PRIVATE|MAP_ANONYMOUS
        if m = -1n then raise (OutOfMemoryException "buffer ring")
        m
    let bufMem =
        let m = Sys.mmap(0n, unativeint (NBufs * BufSize), 3, 0x2 ||| 0x20, -1, 0L)
        if m = -1n then raise (OutOfMemoryException "receive buffers")
        m
    let bufMask = uint16 (NBufs - 1)
    let mutable bufTail = 0us
    let mutable publishedTail = 0us
    /// Some 6.8 builds refuse buffer rings (#152: Ubuntu's 6.8.0-147 answers EINVAL). Then the buffers go to the
    /// kernel with IORING_OP_PROVIDE_BUFFERS instead: all at start, then each one again after use.
    /// FS3_LEGACY_BUFS=1 forces this path (tests).
    let legacy =
        if Environment.GetEnvironmentVariable "FS3_LEGACY_BUFS" = "1" then true
        else
            let reg = Marshal.AllocHGlobal 40
            NativeMemory.Clear(NativePtr.toVoidPtr (NativePtr.ofNativeInt<byte> reg), unativeint 40)
            NativePtr.write (NativePtr.ofNativeInt<uint64> reg) (uint64 bufRing)
            NativePtr.write (NativePtr.ofNativeInt<uint32> (reg + 8n)) (uint32 NBufs)
            NativePtr.write (NativePtr.ofNativeInt<uint16> (reg + 12n)) BufGroup
            try
                try
                    register REGISTER_PBUF_RING reg 1L
                    false
                with _ ->
                    // Ubuntu 6.8.0-147 (and other builds of that series) has the check on the struct's reserved words
                    // inverted: it refuses zeroes and accepts anything else (Launchpad #2162843). A correct kernel refuses
                    // a non-zero word, so this second try only succeeds where the first one failed for that reason.
                    NativePtr.write (NativePtr.ofNativeInt<uint64> (reg + 16n)) 1UL
                    try
                        register REGISTER_PBUF_RING reg 1L
                        if index = 0 then eprintfn "io_uring buffer ring registered with resv[0]=1 (inverted check)"
                        false
                    with e ->
                        if index = 0 then eprintfn "io_uring buffer ring refused (%s): using IORING_OP_PROVIDE_BUFFERS" e.Message
                        true
            finally
                Marshal.FreeHGlobal reg
    /// Legacy mode: buffers used since the last PublishBuffers.
    let returnedBids = List<int>()

    // Per-slot state.
    let gen = Array.zeroCreate<uint32> slots
    let state = Array.zeroCreate<byte> slots
    let inBuf = Array.zeroCreate<byte[]> slots
    let inLen = Array.zeroCreate<int> slots
    let own = Array.zeroCreate<byte[]> slots // pinned: the bytes of the in-flight send
    let ownOff = Array.zeroCreate<int> slots
    let ownLen = Array.zeroCreate<int> slots
    let pend = Array.zeroCreate<byte[]> slots // responses that wait for the in-flight send
    let pendLen = Array.zeroCreate<int> slots
    let recPass = Array.zeroCreate<int> slots // pass number + 1 of the slot's record in the arena
    let recIdx = Array.zeroCreate<int> slots
    let mutable openCount = 0
    let mutable accepting = false
    let mutable acceptGen = 0u

    do
        // Every buffer starts in the kernel's hands.
        for bid in 0 .. NBufs - 1 do
            let e = bufRing + nativeint (bid * 16)
            NativePtr.write (NativePtr.ofNativeInt<uint64> e) (uint64 (bufMem + nativeint (bid * BufSize)))
            NativePtr.write (NativePtr.ofNativeInt<uint32> (e + 8n)) (uint32 BufSize)
            NativePtr.write (NativePtr.ofNativeInt<uint16> (e + 12n)) (uint16 bid)
        bufTail <- uint16 NBufs
        publishedTail <- bufTail
        storeRelease (NativePtr.ofNativeInt<uint16> (bufRing + 14n)) bufTail

    member _.Index = index
    member _.Fd = fd
    member _.Slots = slots
    member _.Gen = gen
    member _.State = state
    member _.InBuf = inBuf
    member _.InLen = inLen
    member _.Own = own
    member _.OwnOff = ownOff
    member _.OwnLen = ownLen
    member _.Pend = pend
    member _.PendLen = pendLen
    member _.RecPass = recPass
    member _.RecIdx = recIdx
    member _.Open with get () = openCount and set v = openCount <- v
    member _.Accepting with get () = accepting and set v = accepting <- v
    member _.AcceptGen with get () = acceptGen and set v = acceptGen <- v
    member _.Buffer(bid: int) = bufMem + nativeint (bid * BufSize)

    /// Gives a receive buffer back to the kernel (published by PublishBuffers).
    member _.Recycle(bid: int) =
      if legacy then returnedBids.Add bid
      else
        let e = bufRing + nativeint (int (bufTail &&& bufMask) * 16)
        NativePtr.write (NativePtr.ofNativeInt<uint64> e) (uint64 (bufMem + nativeint (bid * BufSize)))
        NativePtr.write (NativePtr.ofNativeInt<uint32> (e + 8n)) (uint32 BufSize)
        NativePtr.write (NativePtr.ofNativeInt<uint16> (e + 12n)) (uint16 bid)
        bufTail <- bufTail + 1us

    /// Buffers given back since the last PublishBuffers.
    member _.Returned = if legacy then returnedBids.Count else int (bufTail - publishedTail)

    member this.PublishBuffers() =
        if legacy then
            // One PROVIDE_BUFFERS for each run of consecutive buffer ids.
            let mutable i = 0
            while i < returnedBids.Count do
                let first = returnedBids.[i]
                let mutable n = 1
                while i + n < returnedBids.Count && returnedBids.[i + n] = first + n do
                    n <- n + 1
                this.Provide(first, n)
                i <- i + n
            returnedBids.Clear()
        else
            publishedTail <- bufTail
            storeRelease (NativePtr.ofNativeInt<uint16> (bufRing + 14n)) bufTail

    /// Legacy mode: hands `count` buffers from id `first` to the kernel (no completion unless it fails).
    member this.Provide(first: int, count: int) =
        let s = this.Sqe()
        NativePtr.write (NativePtr.ofNativeInt<byte> s) OP_PROVIDE_BUFFERS
        NativePtr.write (NativePtr.ofNativeInt<byte> (s + 1n)) SQE_CQE_SKIP_SUCCESS
        NativePtr.write (NativePtr.ofNativeInt<int> (s + 4n)) count
        NativePtr.write (NativePtr.ofNativeInt<uint64> (s + 8n)) (uint64 first)
        NativePtr.write (NativePtr.ofNativeInt<uint64> (s + 16n)) (uint64 (bufMem + nativeint (first * BufSize)))
        NativePtr.write (NativePtr.ofNativeInt<uint32> (s + 24n)) (uint32 BufSize)
        NativePtr.write (NativePtr.ofNativeInt<uint64> (s + 32n)) OpIgnore
        NativePtr.write (NativePtr.ofNativeInt<uint16> (s + 40n)) BufGroup

    /// Called once after construction: in legacy mode, queues all buffers for the kernel.
    member this.StartBuffers() = if legacy then this.Provide(0, NBufs)

    /// The next free SQE, zeroed. Submits first when the SQ is full.
    member this.Sqe() : nativeint =
        let head = loadAcquire sqHead
        if tail - head >= sqEntries then
            this.Submit() |> ignore
        let s = sqesMem + nativeint (int (tail &&& sqMask) * 64)
        NativeMemory.Clear(NativePtr.toVoidPtr (NativePtr.ofNativeInt<byte> s), unativeint 64)
        tail <- tail + 1u
        s

    member _.Unsubmitted = tail - published

    /// io_uring_enter: submits the queued SQEs; with `wait` > 0 waits for that many completions or the timeout.
    member _.Enter(wait: uint32, flags: int64, arg: nativeint) : int64 =
        storeRelease sqTail tail
        let n = tail - published
        published <- tail
        let argsz = if flags &&& ENTER_EXT_ARG <> 0L then 24L else 0L
        Sys.syscall(SYS_enter, int64 fd, int64 n, int64 wait, flags, int64 arg, argsz)

    member this.Submit() = this.Enter(0u, 0L, 0n)

    /// Copies the ready completions into `into` (user_data, res, flags) and frees them in the CQ.
    /// The CQ tail: how many completions the kernel has posted so far (wraps).
    member _.CqTail = loadAcquire cqTail
    /// Completions posted and not reaped yet.
    member _.CqReady = loadAcquire cqTail - NativePtr.read cqHead

    member _.Reap(into: List<struct (uint64 * int * uint32)>, cap: int) =
        let mutable head = NativePtr.read cqHead
        let t = loadAcquire cqTail
        let mutable n = 0
        while head <> t && n < cap do
            let c = cqes + nativeint (int (head &&& cqMask) * 16)
            let u = NativePtr.read (NativePtr.ofNativeInt<uint64> c)
            let res = NativePtr.read (NativePtr.ofNativeInt<int> (c + 8n))
            let fl = NativePtr.read (NativePtr.ofNativeInt<uint32> (c + 12n))
            into.Add(struct (u, res, fl))
            head <- head + 1u
            n <- n + 1
        storeRelease cqHead head
        n

    member this.Accept(listenFd: int) =
        acceptGen <- acceptGen + 1u
        let s = this.Sqe()
        NativePtr.write (NativePtr.ofNativeInt<byte> s) OP_ACCEPT
        NativePtr.write (NativePtr.ofNativeInt<uint16> (s + 2n)) ACCEPT_MULTISHOT
        NativePtr.write (NativePtr.ofNativeInt<int> (s + 4n)) listenFd
        NativePtr.write (NativePtr.ofNativeInt<uint64> (s + 32n)) (ud OpAccept index 0 acceptGen 0)
        NativePtr.write (NativePtr.ofNativeInt<uint32> (s + 44n)) FILE_INDEX_ALLOC
        accepting <- true

    member this.StopAccept() =
        let s = this.Sqe()
        NativePtr.write (NativePtr.ofNativeInt<byte> s) OP_ASYNC_CANCEL
        NativePtr.write (NativePtr.ofNativeInt<byte> (s + 1n)) SQE_CQE_SKIP_SUCCESS
        NativePtr.write (NativePtr.ofNativeInt<int> (s + 4n)) -1
        NativePtr.write (NativePtr.ofNativeInt<uint64> (s + 16n)) (ud OpAccept index 0 acceptGen 0)
        NativePtr.write (NativePtr.ofNativeInt<uint64> (s + 32n)) OpIgnore
        accepting <- false

    member this.Recv(slot: int) =
        let s = this.Sqe()
        NativePtr.write (NativePtr.ofNativeInt<byte> s) OP_RECV
        NativePtr.write (NativePtr.ofNativeInt<byte> (s + 1n)) (SQE_FIXED_FILE ||| SQE_BUFFER_SELECT)
        NativePtr.write (NativePtr.ofNativeInt<uint16> (s + 2n)) RECV_MULTISHOT
        NativePtr.write (NativePtr.ofNativeInt<int> (s + 4n)) slot
        NativePtr.write (NativePtr.ofNativeInt<uint64> (s + 32n)) (ud OpRecv index slot gen.[slot] 0)
        NativePtr.write (NativePtr.ofNativeInt<uint16> (s + 40n)) BufGroup
        state.[slot] <- state.[slot] ||| Recving

    member this.Send(op: uint64, slot: int, addr: nativeint, len: int, flags: uint32, extra: int) =
        let s = this.Sqe()
        NativePtr.write (NativePtr.ofNativeInt<byte> s) OP_SEND
        NativePtr.write (NativePtr.ofNativeInt<byte> (s + 1n)) SQE_FIXED_FILE
        NativePtr.write (NativePtr.ofNativeInt<int> (s + 4n)) slot
        NativePtr.write (NativePtr.ofNativeInt<uint64> (s + 16n)) (uint64 addr)
        NativePtr.write (NativePtr.ofNativeInt<uint32> (s + 24n)) (uint32 len)
        NativePtr.write (NativePtr.ofNativeInt<uint32> (s + 28n)) flags
        NativePtr.write (NativePtr.ofNativeInt<uint64> (s + 32n)) (ud op index slot gen.[slot] extra)

    /// SHUTDOWN (sends the FIN now and ends the multishot recv) hard-linked to CLOSE of the fixed file.
    member this.ShutdownClose(slot: int) =
        let s = this.Sqe()
        NativePtr.write (NativePtr.ofNativeInt<byte> s) OP_SHUTDOWN
        NativePtr.write (NativePtr.ofNativeInt<byte> (s + 1n)) (SQE_FIXED_FILE ||| SQE_IO_HARDLINK ||| SQE_CQE_SKIP_SUCCESS)
        NativePtr.write (NativePtr.ofNativeInt<int> (s + 4n)) slot
        NativePtr.write (NativePtr.ofNativeInt<uint32> (s + 24n)) 2u // SHUT_RDWR
        NativePtr.write (NativePtr.ofNativeInt<uint64> (s + 32n)) OpIgnore
        let c = this.Sqe()
        NativePtr.write (NativePtr.ofNativeInt<byte> c) OP_CLOSE
        // The CLOSE posts its completion (ignored): then every deferred completion the loop runs posts one, and the
        // loop's "enter again until a call adds none" (Run) does not stop while closes wait.
        NativePtr.write (NativePtr.ofNativeInt<uint32> (c + 44n)) (uint32 (slot + 1))
        NativePtr.write (NativePtr.ofNativeInt<uint64> (c + 32n)) OpIgnore

/// The responses of one pass: one pinned buffer, the HTTP handler writes into it.
type private Arena(initial: int) =
    let mutable buf = GC.AllocateUninitializedArray<byte>(initial, pinned = true)
    let mutable len = 0
    member _.Array = buf
    member _.Length with get () = len and set v = len <- v
    member _.Address(off: int) = Marshal.UnsafeAddrOfPinnedArrayElement(buf, off)
    member _.Reset() = len <- 0
    member _.Ensure(hint: int) =
        let need = max hint 256
        if buf.Length - len < need then
            let bigger = GC.AllocateUninitializedArray<byte>(max (buf.Length * 2) (len + need), pinned = true)
            Buffer.BlockCopy(buf, 0, bigger, 0, len)
            buf <- bigger
    interface IBufferWriter<byte> with
        member _.Advance(n) = len <- len + n
        member this.GetSpan(hint) =
            this.Ensure hint
            Span<byte>(buf, len, buf.Length - len)
        member this.GetMemory(hint) =
            this.Ensure hint
            Memory<byte>(buf, len, buf.Length - len)

[<Struct>]
type private Record =
    val mutable Ring: int
    val mutable Slot: int
    val mutable Gen: uint32
    val mutable Start: int
    val mutable Len: int
    val mutable Close: bool

type private Loop(listenFd: int) as this =
    let rings = List<Ring>(MaxRings)
    let accepted =
        Channel.CreateUnbounded<ConnectionContext>(
            UnboundedChannelOptions(SingleReader = true, SingleWriter = true, AllowSynchronousContinuations = true)
        )
    let arena = Arena(1 <<< 20)
    let mutable records = Array.zeroCreate<Record> 1024
    let mutable nrec = 0
    let cqes = List<struct (uint64 * int * uint32)>(PassCap)
    let carry = List<struct (uint64 * int * uint32)>(256)
    let rearm = Queue<struct (int * int * uint32)>()
    let waitArg = Marshal.AllocHGlobal 40 // io_uring_getevents_arg (24 bytes) + __kernel_timespec (16)
    let mutable pass = 0
    let mutable debug = if isNull (Environment.GetEnvironmentVariable "FS3_DEBUG") then 0 else 60
    // FS3_STATS=1: every 5 s, one line of loop counters on stderr (passes, completions, longest pass, ENOBUFS...).
    let stats = not (isNull (Environment.GetEnvironmentVariable "FS3_STATS"))
    let mutable stPasses = 0L
    let mutable stCqes = 0L
    let mutable stMaxCqes = 0
    let mutable stMaxPassTicks = 0L
    let mutable stNoBufs = 0L
    let mutable stCarryMax = 0
    let mutable stRearmMax = 0
    let mutable stSpills = 0L
    let mutable stLast = Diagnostics.Stopwatch.GetTimestamp()
    let mutable acceptRing = 0
    let mutable acceptBroken = false
    /// Buffers of sends still in flight for connections that closed meanwhile, by (ring, slot, generation).
    let zombies = Dictionary<uint64, byte[]>()
    /// Slots whose next request is a write held by Db.holdWrites, by (ring, slot, generation).
    let held = List<struct (int * int * uint32)>()
    let mutable holdAt = Db.holdFrames
    let mutable holdSince = 0L
    let ready = new ManualResetEventSlim(false)
    let mutable initError: exn = null
    let mutable thread: Thread = null

    do
        NativeMemory.Clear(NativePtr.toVoidPtr (NativePtr.ofNativeInt<byte> waitArg), unativeint 40)
        NativePtr.write (NativePtr.ofNativeInt<uint64> (waitArg + 16n)) (uint64 (waitArg + 24n)) // ts pointer
        NativePtr.write (NativePtr.ofNativeInt<int64> (waitArg + 32n)) WaitNs // 2 ms by default
        thread <- Thread((fun () -> this.Run()), 512 * 1024, IsBackground = true, Name = "io_uring")
        thread.Start()
        ready.Wait()
        if not (isNull initError) then raise initError

    member _.Accepted = accepted.Reader

    member private _.Release(r: Ring, slot: int) =
        let inb = r.InBuf.[slot]
        if not (isNull inb) then
            ArrayPool<byte>.Shared.Return inb
            r.InBuf.[slot] <- null
        r.InLen.[slot] <- 0
        let p = r.Pend.[slot]
        if not (isNull p) then
            ArrayPool<byte>.Shared.Return p
            r.Pend.[slot] <- null
        r.PendLen.[slot] <- 0

    /// Ends a connection: frees its buffers and queues SHUTDOWN + CLOSE. The slot's generation changes, so the
    /// completions still in flight for it are recognised as stale. An in-flight send keeps its buffer alive in `own`
    /// until its completion comes back (the slot is not reused before the CLOSE, which follows the SHUTDOWN).
    member private this.CloseSlot(r: Ring, slot: int) =
        let st = r.State.[slot]
        if st &&& Live <> 0uy then
            this.Release(r, slot)
            if st &&& Sending <> 0uy then
                zombies.[(u64key r.Index slot r.Gen.[slot])] <- r.Own.[slot]
            r.Own.[slot] <- null
            r.State.[slot] <- Closing
            r.Gen.[slot] <- r.Gen.[slot] + 1u
            r.Open <- r.Open - 1
            r.ShutdownClose slot

    member private this.OnAccept(r: Ring, u: uint64, res: int, flags: uint32) =
        let g = uint32 (u >>> 22) &&& 0xFFFFFFu
        if flags &&& CQE_F_MORE = 0u && g = (r.AcceptGen &&& 0xFFFFFFu) then
            r.Accepting <- false
            if res < 0 && res <> -125 (* ECANCELED *) then acceptBroken <- true
        if res >= 0 then
            let slot = res
            r.State.[slot] <- Live
            r.Open <- r.Open + 1
            r.Recv slot
            // Kestrel is not told (see Listener).

    member private this.Process(r: Ring, slot: int, src: nativeint, n: int) =
        let mutable data = ReadOnlySpan<byte>(NativePtr.toVoidPtr (NativePtr.ofNativeInt<byte> src), n)
        let il = r.InLen.[slot]
        if il > 0 then
            let mutable ib = r.InBuf.[slot]
            if ib.Length - il < n then
                let bigger = ArrayPool<byte>.Shared.Rent(il + n)
                Buffer.BlockCopy(ib, 0, bigger, 0, il)
                ArrayPool<byte>.Shared.Return ib
                ib <- bigger
                r.InBuf.[slot] <- ib
            data.CopyTo(Span<byte>(ib, il, n))
            r.InLen.[slot] <- il + n
            data <- ReadOnlySpan<byte>(ib, 0, il + n)
        // The slot's responses of this pass stay together, in order, at the end of the arena.
        let mutable ri = -1
        if r.RecPass.[slot] = pass + 1 then
            ri <- r.RecIdx.[slot]
            let rc = records.[ri]
            if rc.Start + rc.Len <> arena.Length then
                let w = arena :> IBufferWriter<byte>
                let dst = w.GetSpan rc.Len
                ReadOnlySpan<byte>(arena.Array, rc.Start, rc.Len).CopyTo dst
                w.Advance rc.Len
                records.[ri].Start <- arena.Length - rc.Len
        let before = arena.Length
        let mutable close = false
        let mutable cont = r.State.[slot] &&& ContinueSent <> 0uy
        let mutable ok = true
        let mutable consumed = 0
        try
            consumed <- Conn.direct data arena &close &cont
            if Conn.held && r.State.[slot] &&& HeldW = 0uy then
                r.State.[slot] <- r.State.[slot] ||| HeldW
                held.Add(struct (r.Index, slot, r.Gen.[slot]))
        with _ ->
            ok <- false
        if not ok then
            arena.Length <- before
            this.CloseSlot(r, slot)
        else
            r.State.[slot] <- if cont then r.State.[slot] ||| ContinueSent else r.State.[slot] &&& ~~~ContinueSent
            let rest = data.Length - consumed
            if close || rest = 0 then
                let ib = r.InBuf.[slot]
                if not (isNull ib) then
                    ArrayPool<byte>.Shared.Return ib
                    r.InBuf.[slot] <- null
                r.InLen.[slot] <- 0
            elif r.InLen.[slot] > 0 then
                let ib = r.InBuf.[slot]
                if consumed > 0 then Buffer.BlockCopy(ib, consumed, ib, 0, rest)
                r.InLen.[slot] <- rest
            else
                let ib = ArrayPool<byte>.Shared.Rent(max 4096 rest)
                data.Slice(consumed).CopyTo(Span ib)
                r.InBuf.[slot] <- ib
                r.InLen.[slot] <- rest
            let wrote = arena.Length - before
            if wrote > 0 || close then
                if ri < 0 then
                    if nrec = records.Length then
                        let bigger = Array.zeroCreate (records.Length * 2)
                        Array.blit records 0 bigger 0 nrec
                        records <- bigger
                    ri <- nrec
                    nrec <- nrec + 1
                    records.[ri].Ring <- r.Index
                    records.[ri].Slot <- slot
                    records.[ri].Gen <- r.Gen.[slot]
                    records.[ri].Start <- before
                    records.[ri].Len <- 0
                    records.[ri].Close <- false
                    r.RecPass.[slot] <- pass + 1
                    r.RecIdx.[slot] <- ri
                records.[ri].Len <- records.[ri].Len + wrote
                if close then
                    records.[ri].Close <- true
                    r.State.[slot] <- r.State.[slot] ||| Done

    member private this.OnRecv(r: Ring, u: uint64, res: int, flags: uint32) =
        let slot = int ((u >>> 6) &&& 0xFFFFUL)
        let g = uint32 (u >>> 22) &&& 0xFFFFFFu
        let valid = r.State.[slot] &&& Live <> 0uy && (r.Gen.[slot] &&& 0xFFFFFFu) = g
        if valid then
            if flags &&& CQE_F_MORE = 0u then r.State.[slot] <- r.State.[slot] &&& ~~~Recving
            if res > 0 then
                if r.State.[slot] &&& Done = 0uy then this.Process(r, slot, r.Buffer(int (flags >>> 16)), res)
                if r.State.[slot] &&& (Live ||| Recving) = Live then r.Recv slot
            elif res = -ENOBUFS then
                stNoBufs <- stNoBufs + 1L
                if r.State.[slot] &&& Recving = 0uy then rearm.Enqueue(struct (r.Index, slot, r.Gen.[slot]))
            else
                this.CloseSlot(r, slot)
        if flags &&& CQE_F_BUFFER <> 0u then r.Recycle(int (flags >>> 16))

    /// Sends the slot's own buffer from `OwnOff` (a normal send: it waits for the socket if it has to).
    member private _.SendOwn(r: Ring, slot: int) =
        let b = r.Own.[slot]
        r.State.[slot] <- r.State.[slot] ||| Sending
        r.Send(OpSendOwn, slot, Marshal.UnsafeAddrOfPinnedArrayElement(b, r.OwnOff.[slot]), r.OwnLen.[slot], MSG_NOSIGNAL, 0)

    /// The rest of an arena response that the socket did not take at once: copy it into the slot's own buffer.
    member private this.Spill(r: Ring, slot: int, src: ReadOnlySpan<byte>, close: bool) =
        stSpills <- stSpills + 1L
        let b = GC.AllocateUninitializedArray<byte>(src.Length, pinned = true)
        src.CopyTo(Span b)
        r.Own.[slot] <- b
        r.OwnOff.[slot] <- 0
        r.OwnLen.[slot] <- b.Length
        if close then r.State.[slot] <- r.State.[slot] ||| CloseAfter
        this.SendOwn(r, slot)

    member private this.OnSendArena(r: Ring, u: uint64, res: int) =
        let slot = int ((u >>> 6) &&& 0xFFFFUL)
        let g = uint32 (u >>> 22) &&& 0xFFFFFFu
        let ri = int (u >>> 46)
        if r.State.[slot] &&& Live <> 0uy && (r.Gen.[slot] &&& 0xFFFFFFu) = g then
            let rc = records.[ri]
            if res = rc.Len then
                if rc.Close then this.CloseSlot(r, slot)
            elif res >= 0 || res = -EAGAIN || res = -EINTR then
                let sent = max res 0
                this.Spill(r, slot, ReadOnlySpan<byte>(arena.Array, rc.Start + sent, rc.Len - sent), rc.Close)
            else
                this.CloseSlot(r, slot)

    member private this.OnSendOwn(r: Ring, u: uint64, res: int) =
        let slot = int ((u >>> 6) &&& 0xFFFFUL)
        let g = uint32 (u >>> 22) &&& 0xFFFFFFu
        if r.State.[slot] &&& Live = 0uy || (r.Gen.[slot] &&& 0xFFFFFFu) <> g then
            // The connection closed meanwhile: the kernel is done with the send's buffer now.
            zombies.Remove(u &&& 0x3FFFFFFFFFF8UL) |> ignore
        else
            r.State.[slot] <- r.State.[slot] &&& ~~~Sending
            if res = -EAGAIN || res = -EINTR then
                this.SendOwn(r, slot)
            elif res <= 0 then
                r.Own.[slot] <- null
                this.CloseSlot(r, slot)
            else
                r.OwnOff.[slot] <- r.OwnOff.[slot] + res
                r.OwnLen.[slot] <- r.OwnLen.[slot] - res
                if r.OwnLen.[slot] > 0 then
                    this.SendOwn(r, slot)
                else
                    r.Own.[slot] <- null
                    let pl = r.PendLen.[slot]
                    if pl > 0 then
                        let p = r.Pend.[slot]
                        let b = GC.AllocateUninitializedArray<byte>(pl, pinned = true)
                        Buffer.BlockCopy(p, 0, b, 0, pl)
                        ArrayPool<byte>.Shared.Return p
                        r.Pend.[slot] <- null
                        r.PendLen.[slot] <- 0
                        r.Own.[slot] <- b
                        r.OwnOff.[slot] <- 0
                        r.OwnLen.[slot] <- pl
                        this.SendOwn(r, slot)
                    elif r.State.[slot] &&& CloseAfter <> 0uy then
                        this.CloseSlot(r, slot)

    member private this.Handle(struct (u: uint64, res: int, flags: uint32)) =
        if debug > 0 then
            debug <- debug - 1
            eprintfn "cqe op=%d ring=%d slot=%d res=%d flags=%x" (u &&& 7UL) ((u >>> 3) &&& 7UL) ((u >>> 6) &&& 0xFFFFUL) res flags
        let r = rings.[int ((u >>> 3) &&& 7UL)]
        match u &&& 7UL with
        | 1UL -> this.OnAccept(r, u, res, flags)
        | 2UL -> this.OnRecv(r, u, res, flags)
        | 3UL -> this.OnSendArena(r, u, res)
        | 4UL -> this.OnSendOwn(r, u, res)
        | _ -> ()

    /// After the COMMIT of the pass: queue a send for each response. A slot whose own send is still in flight keeps
    /// its new responses in `Pend` until then.
    member private this.Flush(committed: bool) =
        for i in 0 .. nrec - 1 do
            let rc = records.[i]
            let r = rings.[rc.Ring]
            let slot = rc.Slot
            if r.State.[slot] &&& Live <> 0uy && r.Gen.[slot] = rc.Gen then
                if not committed then
                    this.CloseSlot(r, slot)
                elif r.State.[slot] &&& Sending <> 0uy then
                    if rc.Len > 0 then
                        let pl = r.PendLen.[slot]
                        let mutable p = r.Pend.[slot]
                        if isNull p || p.Length - pl < rc.Len then
                            let bigger = ArrayPool<byte>.Shared.Rent(pl + rc.Len)
                            if not (isNull p) then
                                Buffer.BlockCopy(p, 0, bigger, 0, pl)
                                ArrayPool<byte>.Shared.Return p
                            p <- bigger
                            r.Pend.[slot] <- p
                        Buffer.BlockCopy(arena.Array, rc.Start, p, pl, rc.Len)
                        r.PendLen.[slot] <- pl + rc.Len
                    if rc.Close then r.State.[slot] <- r.State.[slot] ||| CloseAfter
                elif rc.Len > 0 then
                    r.Send(OpSendArena, slot, arena.Address rc.Start, rc.Len, MSG_NOSIGNAL ||| MSG_DONTWAIT, i)
                elif rc.Close then
                    this.CloseSlot(r, slot)

    /// Ends a write hold once the checkpoint thread has copied every frame and started the WAL again (or after 500 ms,
    /// unless that thread is starting the WAL right now: then the WAL goes on growing and the next hold comes 4,096
    /// frames later), and runs the held requests in this pass.
    member private this.MaybeResume() =
        let caught = Volatile.Read &Db.caughtUp
        let timedOut =
            not caught && Environment.TickCount64 - holdSince > 500L
            && Interlocked.CompareExchange(&Db.restartGate, 2, 0) = 0
        if caught || timedOut then
            holdAt <- if caught then Db.holdFrames else holdAt + Db.holdFrames
            Volatile.Write(&Db.walFrames, 0)
            Volatile.Write(&Db.caughtUp, false)
            Volatile.Write(&Db.holdWrites, false)
            if stats then eprintfn "hold end caught=%b ms=%d slots=%d" caught (Environment.TickCount64 - holdSince) held.Count
            for i in 0 .. held.Count - 1 do
                let struct (ri, slot, g) = held.[i]
                let r = rings.[ri]
                if r.Gen.[slot] = g && r.State.[slot] &&& HeldW <> 0uy then
                    r.State.[slot] <- r.State.[slot] &&& ~~~HeldW
                    if r.State.[slot] &&& (Live ||| Done) = Live then
                        try this.Process(r, slot, 0n, 0) with _ -> ()
            held.Clear()
            if timedOut then Volatile.Write(&Db.restartGate, 0)

    member private this.AddRing() =
        let r = Ring(rings.Count)
        r.StartBuffers()
        rings.Add r
        r

    /// Accept on the first ring with free slots (a new ring when all are nearly full).
    member private this.ManageAccept() =
        let cur = rings.[acceptRing]
        if cur.Accepting && cur.Slots - cur.Open < reserveOf cur.Slots then
            cur.StopAccept()
        if not cur.Accepting || acceptBroken then
            acceptBroken <- false
            let mutable target = -1
            for i in 0 .. rings.Count - 1 do
                if target < 0 && rings.[i].Slots - rings.[i].Open >= 2 * reserveOf rings.[i].Slots then target <- i
            if target < 0 && rings.Count < MaxRings then
                try target <- this.AddRing().Index with _ -> ()
            if target >= 0 then
                let t = rings.[target]
                if t.Index <> acceptRing && cur.Accepting then cur.StopAccept()
                acceptRing <- target
                if not t.Accepting then t.Accept listenFd

    member private this.Run() =
        try
            try
                this.AddRing() |> ignore
                rings.[0].Accept listenFd
                rings.[0].Submit() |> ignore
                // Last step that can fail is done: from here on there is no fallback to epoll. This thread handles
                // its first request only after this line.
                Db.start true
            with e ->
                initError <- e
        finally
            ready.Set()
        if isNull initError then
            let mutable openTotal = 0
            while true do
                // 1. Wait in ring 0: for 32 completions or 2 ms while connections are open, else for one completion.
                for i in 0 .. rings.Count - 1 do
                    let r = rings.[i]
                    if i > 0 || carry.Count > 0 then r.Enter(0u, ENTER_GETEVENTS, 0n) |> ignore
                    elif openTotal = 0 && rings.Count = 1 then r.Enter(1u, ENTER_GETEVENTS, 0n) |> ignore
                    else r.Enter(WaitN, ENTER_GETEVENTS ||| ENTER_EXT_ARG, waitArg) |> ignore
                    // From 6.13 one io_uring_enter runs at most max(20, wait) of the ring's deferred completions
                    // ("limit local tw done") and keeps the rest for the next call. With one call per ring per pass, 5
                    // rings gave a pass at most 32 + 4 x 20 of them: 320,000 users got 25,700 requests per second at 66%
                    // CPU and waited 4.5 s. So enter again, waiting for as many completions as the ring already holds:
                    // the call runs up to that many and returns at once (they are there). Until a call adds none (6.8
                    // runs them all in the first call; then the second finds none). Not "until a call adds fewer than
                    // 16": closes without a completion (CQE_SKIP_SUCCESS) made that stop early, with 4 s waits.
                    let mutable k = 0
                    let mutable more = true
                    while more && k < 64 do
                        let before = r.CqTail
                        r.Enter(r.CqReady, ENTER_GETEVENTS, 0n) |> ignore
                        more <- r.CqTail <> before
                        k <- k + 1
                // 2. Handle the completions: new connections, received requests (responses into the arena).
                pass <- pass + 1
                let t0 = if stats then Diagnostics.Stopwatch.GetTimestamp() else 0L
                if Db.holdWrites then this.MaybeResume()
                if stats && carry.Count > stCarryMax then stCarryMax <- carry.Count
                cqes.Clear()
                cqes.AddRange carry
                carry.Clear()
                let mutable room = PassCap - cqes.Count
                for r in rings do
                    if room > 0 then room <- room - r.Reap(cqes, room)
                if stats then
                    stCqes <- stCqes + int64 cqes.Count
                    if cqes.Count > stMaxCqes then stMaxCqes <- cqes.Count
                    if rearm.Count > stRearmMax then stRearmMax <- rearm.Count
                for i in 0 .. cqes.Count - 1 do
                    try this.Handle cqes.[i] with _ -> ()
                let mutable returned = 0
                for r in rings do
                    returned <- returned + r.Returned
                    r.PublishBuffers()
                // Receives that found no buffer: re-arm as many as buffers came back (bounded), so that a pass with
                // no free buffer does not re-arm receives that fail again at once.
                let mutable budget = min rearm.Count (min (max returned 32) (NBufs / 2))
                while budget > 0 do
                    let struct (ri, slot, g) = rearm.Dequeue()
                    let r = rings.[ri]
                    if r.Gen.[slot] = g && r.State.[slot] &&& (Live ||| Recving) = Live then r.Recv slot
                    budget <- budget - 1
                // 3. Commit the pass's writes, then send the responses.
                let committed = if Db.inTxn then Db.commit () else true
                if Db.bgCheckpoint && not Db.holdWrites && Volatile.Read &Db.walFrames >= holdAt then
                    Volatile.Write(&Db.caughtUp, false)
                    Volatile.Write(&Db.holdWrites, true)
                    Db.wakeCheckpointer ()
                    holdSince <- Environment.TickCount64
                (try this.Flush committed with _ -> ())
                (try this.ManageAccept() with _ -> ())
                // 4. Submit (the sends finish inside io_uring_enter), and handle their completions while the arena
                // still holds the bytes. Other completions wait for the next pass.
                for r in rings do
                    if r.Unsubmitted > 0u then r.Submit() |> ignore
                cqes.Clear()
                for r in rings do r.Reap(cqes, Int32.MaxValue) |> ignore
                for i in 0 .. cqes.Count - 1 do
                    let struct (u, _, _) = cqes.[i]
                    let op = u &&& 7UL
                    if op = OpSendArena || op = OpSendOwn then
                        (try this.Handle cqes.[i] with _ -> ())
                    elif op <> OpIgnore then
                        carry.Add cqes.[i]
                arena.Reset()
                nrec <- 0
                openTotal <- 0
                for r in rings do openTotal <- openTotal + r.Open
                if stats then
                    let t1 = Diagnostics.Stopwatch.GetTimestamp()
                    stPasses <- stPasses + 1L
                    if t1 - t0 > stMaxPassTicks then stMaxPassTicks <- t1 - t0
                    if t1 - stLast > 5L * Diagnostics.Stopwatch.Frequency then
                        let ms (t: int64) = float t * 1000.0 / float Diagnostics.Stopwatch.Frequency
                        let opens = String.Join(",", rings |> Seq.map (fun r -> string r.Open))
                        eprintfn "stats passes=%d cqes=%d maxcqes=%d maxpass=%.1fms nobufs=%d carrymax=%d rearmmax=%d rearmq=%d spills=%d zombies=%d open=%s gc=%d/%d/%d heap=%dMB committed=%dMB allocated=%dMB"
                            stPasses stCqes stMaxCqes (ms stMaxPassTicks) stNoBufs stCarryMax stRearmMax rearm.Count stSpills zombies.Count opens
                            (GC.CollectionCount 0) (GC.CollectionCount 1) (GC.CollectionCount 2)
                            (GC.GetGCMemoryInfo().HeapSizeBytes >>> 20) (GC.GetGCMemoryInfo().TotalCommittedBytes >>> 20)
                            (GC.GetTotalAllocatedBytes() >>> 20)
                        stPasses <- 0L; stCqes <- 0L; stMaxCqes <- 0; stMaxPassTicks <- 0L; stNoBufs <- 0L
                        stCarryMax <- 0; stRearmMax <- 0; stSpills <- 0L; stLast <- t1

/// Kestrel's accept never completes: the loop keeps every connection from accept to close. Handing each new
/// connection to Kestrel (whose handler then returned at once) woke a thread-pool thread that spun with sched_yield
/// against the loop on the one core: 44 to 32 us of CPU per new connection in a 320,000-user ramp, ~200 us when idle.
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

/// True where this transport can run: x86-64 Linux (the epoll fallback has the same limit).
let supported = OperatingSystem.IsLinux() && RuntimeInformation.ProcessArchitecture = Architecture.X64

/// Kestrel's IConnectionListenerFactory: io_uring if the kernel allows it, else the epoll transport.
type UringTransportFactory() =
    let epoll = Transport.EpollTransportFactory() :> IConnectionListenerFactory
    let mutable bound = 0
    interface IConnectionListenerFactory with
        member _.BindAsync(endpoint, ct) =
            if Interlocked.Exchange(&bound, 1) <> 0 then
                raise (NotSupportedException "io_uring transport: one endpoint only")
            let ip =
                match endpoint with
                | :? IPEndPoint as ip when ip.AddressFamily = AddressFamily.InterNetwork -> ip
                | _ -> raise (NotSupportedException "io_uring transport: IPv4 endpoints only")
            let fd = Sys.socket(AF_INET, SOCK_STREAM ||| SOCK_CLOEXEC, 0)
            if fd < 0 then failwithf "socket: errno %d" (errno ())
            let mutable one = 1
            Sys.setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, 4) |> ignore
            // Accepted sockets inherit TCP_NODELAY from the listening socket (a fixed file has no fd for setsockopt).
            Sys.setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, 4) |> ignore
            let addr = Array.zeroCreate<byte> 16
            addr.[0] <- byte AF_INET
            addr.[2] <- byte (ip.Port >>> 8)
            addr.[3] <- byte ip.Port
            ip.Address.GetAddressBytes().CopyTo(addr, 4)
            let rc =
                use p = fixed addr
                Sys.bind(fd, NativePtr.toNativeInt p, 16)
            if rc <> 0 then
                let e = errno ()
                raise (IO.IOException(sprintf "Failed to bind to address %O: errno %d" ip e))
            if Sys.listen(fd, 65535) <> 0 then failwithf "listen: errno %d" (errno ())
            let loop =
                try
                    Db.singleThread <- true
                    Db.batching <- true
                    Some(Loop(fd))
                with e ->
                    eprintfn "io_uring unavailable (%s): using epoll" e.Message
                    None
            match loop with
            | Some l -> ValueTask<IConnectionListener>(Listener(ip, l))
            | None ->
                Sys.close fd |> ignore
                epoll.BindAsync(endpoint, ct)

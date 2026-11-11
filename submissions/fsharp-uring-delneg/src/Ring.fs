/// io_uring without liburing (from our twelve-dollar and multicore entries): raw io_uring_setup / io_uring_enter /
/// io_uring_register through syscall(2), the rings mmapped, SQEs written in place. One thread owns a ring
/// (SINGLE_ISSUER | DEFER_TASKRUN: completion work runs only when that thread enters the ring).
module Ring

#nowarn "9"

open System
open System.Collections.Generic
open System.Runtime.InteropServices
open System.Threading
open Microsoft.FSharp.NativeInterop

let SYS_setup = 425L
let SYS_enter = 426L
let SYS_register = 427L

let SETUP_CQSIZE = 1u <<< 3
let SETUP_SUBMIT_ALL = 1u <<< 7
let SETUP_COOP_TASKRUN = 1u <<< 8
let SETUP_SINGLE_ISSUER = 1u <<< 12
let SETUP_DEFER_TASKRUN = 1u <<< 13
let FEAT_SINGLE_MMAP = 1u
let FEAT_EXT_ARG = 1u <<< 8
let ENTER_GETEVENTS = 1L
let ENTER_EXT_ARG = 8L
let REGISTER_FILES2 = 13L
let REGISTER_PBUF_RING = 22L

let OP_READ = 22uy
let OP_ACCEPT = 13uy
let OP_ASYNC_CANCEL = 14uy
let OP_CLOSE = 19uy
let OP_SEND = 26uy
let OP_RECV = 27uy
let OP_PROVIDE_BUFFERS = 31uy
let OP_SHUTDOWN = 34uy

let SQE_FIXED_FILE = 1uy
let SQE_IO_HARDLINK = 1uy <<< 3
let SQE_BUFFER_SELECT = 1uy <<< 5
let SQE_CQE_SKIP_SUCCESS = 1uy <<< 6
let CQE_F_BUFFER = 1u
let CQE_F_MORE = 2u
let ACCEPT_MULTISHOT = 1us
let RECV_MULTISHOT = 2us
let FILE_INDEX_ALLOC = 0xFFFFFFFFu

let EAGAIN = 11
let EINTR = 4
let ENOBUFS = 105
let ECANCELED = 125
let MSG_DONTWAIT = 0x40u
let MSG_NOSIGNAL = 0x4000u

let inline rd32 (p: nativeint) = NativePtr.read (NativePtr.ofNativeInt<uint32> p)
let inline wr8 (p: nativeint) (v: byte) = NativePtr.write (NativePtr.ofNativeInt<byte> p) v
let inline wr16 (p: nativeint) (v: uint16) = NativePtr.write (NativePtr.ofNativeInt<uint16> p) v
let inline wr32 (p: nativeint) (v: uint32) = NativePtr.write (NativePtr.ofNativeInt<uint32> p) v
let inline wri32 (p: nativeint) (v: int) = NativePtr.write (NativePtr.ofNativeInt<int> p) v
let inline wr64 (p: nativeint) (v: uint64) = NativePtr.write (NativePtr.ofNativeInt<uint64> p) v

/// Shared ring indexes: a full fence keeps the SQE/CQE accesses on the right side (a few per pass, so cheap).
let inline loadAcquire (p: nativeptr<'T>) =
    let v = NativePtr.read p
    Interlocked.MemoryBarrier()
    v

let inline storeRelease (p: nativeptr<'T>) (v: 'T) =
    Interlocked.MemoryBarrier()
    NativePtr.write p v

let envInt name dflt =
    match Int32.TryParse(Environment.GetEnvironmentVariable(name: string)) with
    | true, v when v >= 0 -> v
    | _ -> dflt

[<Struct>]
type Cqe =
    val mutable U: uint64
    val mutable Res: int
    val mutable Flags: uint32

[<AllowNullLiteral>]
type Ring(sqEntries0: uint32, cqEntries0: uint32) =
    let p = Marshal.AllocHGlobal 120
    do
        NativeMemory.Clear(NativePtr.toVoidPtr (NativePtr.ofNativeInt<byte> p), unativeint 120)
        wr32 (p + 4n) cqEntries0
        wr32 (p + 8n) (SETUP_CQSIZE ||| SETUP_SUBMIT_ALL ||| SETUP_COOP_TASKRUN ||| SETUP_SINGLE_ISSUER ||| SETUP_DEFER_TASKRUN)
    let fd =
        let r = Native.syscall (SYS_setup, int64 sqEntries0, int64 p, 0L, 0L, 0L, 0L)
        if r < 0L then
            let e = Native.errno ()
            Marshal.FreeHGlobal p
            raise (InvalidOperationException(sprintf "io_uring_setup: errno %d" e))
        int r
    let features = rd32 (p + 20n)
    do
        if features &&& FEAT_SINGLE_MMAP = 0u || features &&& FEAT_EXT_ARG = 0u then
            raise (InvalidOperationException "io_uring: kernel too old (needs SINGLE_MMAP and EXT_ARG)")
    let sqEntries = rd32 (p + 0n)
    let cqEntries = rd32 (p + 4n)
    let sqOff i = rd32 (p + 40n + nativeint (4 * i))
    let cqOff i = rd32 (p + 80n + nativeint (4 * i))
    let sqSize = int (sqOff 6) + int sqEntries * 4
    let cqSize = int (cqOff 5) + int cqEntries * 16
    let ringSize = max sqSize cqSize
    let ringMem =
        let m = Native.mmap (0n, unativeint ringSize, 3, 0x1 ||| 0x8000, fd, 0L) // PROT_READ|WRITE, MAP_SHARED|POPULATE
        if m = -1n then raise (InvalidOperationException(sprintf "io_uring mmap: errno %d" (Native.errno ())))
        m
    let sqesMem =
        let m = Native.mmap (0n, unativeint (int sqEntries * 64), 3, 0x1 ||| 0x8000, fd, 0x10000000L)
        if m = -1n then raise (InvalidOperationException(sprintf "io_uring mmap sqes: errno %d" (Native.errno ())))
        m
    let sqHead = NativePtr.ofNativeInt<uint32> (ringMem + nativeint (sqOff 0))
    let sqTail = NativePtr.ofNativeInt<uint32> (ringMem + nativeint (sqOff 1))
    let sqMask = rd32 (ringMem + nativeint (sqOff 2))
    let sqArray = ringMem + nativeint (sqOff 6)
    let cqHead = NativePtr.ofNativeInt<uint32> (ringMem + nativeint (cqOff 0))
    let cqTail = NativePtr.ofNativeInt<uint32> (ringMem + nativeint (cqOff 1))
    let cqMask = rd32 (ringMem + nativeint (cqOff 2))
    let cqes = ringMem + nativeint (cqOff 5)
    let cqOverflow = NativePtr.ofNativeInt<uint32> (ringMem + nativeint (cqOff 4))
    do
        for i in 0 .. int sqEntries - 1 do
            wr32 (sqArray + nativeint (4 * i)) (uint32 i)
        Marshal.FreeHGlobal p
    let mutable tail = NativePtr.read sqTail
    let mutable published = tail
    /// io_uring_getevents_arg (24 bytes) + __kernel_timespec (16)
    let waitArg = Marshal.AllocHGlobal 40
    do
        NativeMemory.Clear(NativePtr.toVoidPtr (NativePtr.ofNativeInt<byte> waitArg), unativeint 40)
        wr64 (waitArg + 16n) (uint64 (waitArg + 24n))

    member _.Fd = fd
    /// completions the kernel dropped because the CQ ring and its overflow list were full (should stay 0)
    member _.CqOverflow = NativePtr.read cqOverflow

    member _.Register(op: int64, arg: nativeint, n: int64) =
        let r = Native.syscall (SYS_register, int64 fd, op, int64 arg, n, 0L, 0L)
        if r < 0L then raise (InvalidOperationException(sprintf "io_uring_register %d: errno %d" op (Native.errno ())))

    /// io_uring_register that returns the errno instead of throwing (0 = ok)
    member _.TryRegister(op: int64, arg: nativeint, n: int64) =
        let r = Native.syscall (SYS_register, int64 fd, op, int64 arg, n, 0L, 0L)
        if r < 0L then Native.errno () else 0

    /// Registers a provided-buffer ring (IORING_REGISTER_PBUF_RING) for group `group`. Returns how: 1 = registered,
    /// 2 = registered on the retry with resv[0] = 1, 0 = refused (use IORING_OP_PROVIDE_BUFFERS).
    /// Ubuntu's 6.8.0-1xx kernels (-132, -135..-142, -145..-149) carry an inverted check in io_register_pbuf_ring
    /// (`if (!memchr_inv(reg.resv, 0, ...)) return -EINVAL`): they refuse ZEROED reserved fields. Upstream 6.8,
    /// 7.0 and master check the opposite way, so a correct kernel takes the first attempt (#23 does the same retry).
    member this.RegisterBufRing(ringAddr: nativeint, entries: int, group: uint16) =
        let reg = Marshal.AllocHGlobal 40
        try
            NativeMemory.Clear(NativePtr.toVoidPtr (NativePtr.ofNativeInt<byte> reg), unativeint 40)
            wr64 reg (uint64 ringAddr)
            wr32 (reg + 8n) (uint32 entries)
            wr16 (reg + 12n) group
            if this.TryRegister(REGISTER_PBUF_RING, reg, 1L) = 0 then 1
            else
                wr64 (reg + 16n) 1UL
                if this.TryRegister(REGISTER_PBUF_RING, reg, 1L) = 0 then 2 else 0
        finally
            Marshal.FreeHGlobal reg

    /// The next free SQE, zeroed. Submits first when the SQ is full.
    member this.Sqe() : nativeint =
        let head = loadAcquire sqHead
        if tail - head >= sqEntries then this.Submit() |> ignore
        let s = sqesMem + nativeint (int (tail &&& sqMask) * 64)
        NativeMemory.Clear(NativePtr.toVoidPtr (NativePtr.ofNativeInt<byte> s), unativeint 64)
        tail <- tail + 1u
        s

    member _.Unsubmitted = tail - published

    /// completions waiting in the CQ
    member _.CqReady = loadAcquire cqTail - NativePtr.read cqHead

    /// io_uring_enter: submits the queued SQEs; waits for `wait` completions (with a timeout in ns when > 0).
    member _.Enter(wait: uint32, timeoutNs: int64) : int64 =
        storeRelease sqTail tail
        let n = tail - published
        published <- tail
        if wait = 0u then Native.syscall (SYS_enter, int64 fd, int64 n, 0L, ENTER_GETEVENTS, 0L, 0L)
        elif timeoutNs <= 0L then Native.syscall (SYS_enter, int64 fd, int64 n, int64 wait, ENTER_GETEVENTS, 0L, 0L)
        else
            NativePtr.write (NativePtr.ofNativeInt<int64> (waitArg + 24n)) (timeoutNs / 1_000_000_000L)
            NativePtr.write (NativePtr.ofNativeInt<int64> (waitArg + 32n)) (timeoutNs % 1_000_000_000L)
            Native.syscall (SYS_enter, int64 fd, int64 n, int64 wait, ENTER_GETEVENTS ||| ENTER_EXT_ARG, int64 waitArg, 24L)

    member this.Submit() = this.Enter(0u, 0L)

    /// Copies up to `cap` ready completions into `into` and frees them in the CQ.
    member _.Reap(into: List<Cqe>, cap: int) =
        let mutable head = NativePtr.read cqHead
        let t = loadAcquire cqTail
        let mutable n = 0
        let mutable c = Cqe()
        while head <> t && n < cap do
            let a = cqes + nativeint (int (head &&& cqMask) * 16)
            c.U <- NativePtr.read (NativePtr.ofNativeInt<uint64> a)
            c.Res <- NativePtr.read (NativePtr.ofNativeInt<int> (a + 8n))
            c.Flags <- NativePtr.read (NativePtr.ofNativeInt<uint32> (a + 12n))
            into.Add c
            head <- head + 1u
            n <- n + 1
        storeRelease cqHead head
        n

    // ---------- requests ----------

    /// recv into a fixed buffer address (a plain fd: Postgres sockets)
    member this.RecvInto(sock: int, addr: nativeint, len: int, ud: uint64) =
        let s = this.Sqe()
        wr8 s OP_RECV
        wri32 (s + 4n) sock
        wr64 (s + 16n) (uint64 addr)
        wr32 (s + 24n) (uint32 len)
        wr64 (s + 32n) ud

    /// send from an address; fixed = the fd is an index in the fixed-file table
    member this.Send(sock: int, fixedFile: bool, addr: nativeint, len: int, flags: uint32, ud: uint64) =
        let s = this.Sqe()
        wr8 s OP_SEND
        if fixedFile then wr8 (s + 1n) SQE_FIXED_FILE
        wri32 (s + 4n) sock
        wr64 (s + 16n) (uint64 addr)
        wr32 (s + 24n) (uint32 len)
        wr32 (s + 28n) flags
        wr64 (s + 32n) ud

    /// read(2) of an eventfd's 8-byte counter
    member this.Read(fd: int, addr: nativeint, len: int, ud: uint64) =
        let s = this.Sqe()
        wr8 s OP_READ
        wri32 (s + 4n) fd
        wr64 (s + 8n) UInt64.MaxValue // offset -1: the file's position (none for an eventfd)
        wr64 (s + 16n) (uint64 addr)
        wr32 (s + 24n) (uint32 len)
        wr64 (s + 32n) ud

    /// multishot accept straight into the fixed-file table
    member this.AcceptMulti(listenFd: int, ud: uint64) =
        let s = this.Sqe()
        wr8 s OP_ACCEPT
        wr16 (s + 2n) ACCEPT_MULTISHOT
        wri32 (s + 4n) listenFd
        wr64 (s + 32n) ud
        wr32 (s + 44n) FILE_INDEX_ALLOC

    member this.Cancel(target: uint64, ud: uint64) =
        let s = this.Sqe()
        wr8 s OP_ASYNC_CANCEL
        wr8 (s + 1n) SQE_CQE_SKIP_SUCCESS
        wri32 (s + 4n) -1
        wr64 (s + 16n) target
        wr64 (s + 32n) ud

    /// multishot recv on a fixed file with a buffer from group `group`
    member this.RecvMulti(slot: int, group: uint16, ud: uint64) =
        let s = this.Sqe()
        wr8 s OP_RECV
        wr8 (s + 1n) (SQE_FIXED_FILE ||| SQE_BUFFER_SELECT)
        wr16 (s + 2n) RECV_MULTISHOT
        wri32 (s + 4n) slot
        wr64 (s + 32n) ud
        wr16 (s + 40n) group

    /// SHUTDOWN (sends the FIN now and ends the multishot recv) hard-linked to CLOSE of the fixed file. Kernel 6.8:
    /// a closed fixed file is released only after all older requests on its ring finish, so the SHUTDOWN goes first.
    member this.ShutdownClose(slot: int, ud: uint64) =
        let s = this.Sqe()
        wr8 s OP_SHUTDOWN
        wr8 (s + 1n) (SQE_FIXED_FILE ||| SQE_IO_HARDLINK ||| SQE_CQE_SKIP_SUCCESS)
        wri32 (s + 4n) slot
        wr32 (s + 24n) 2u // SHUT_RDWR
        wr64 (s + 32n) ud
        let c = this.Sqe()
        wr8 c OP_CLOSE
        wr8 (c + 1n) SQE_CQE_SKIP_SUCCESS
        wr32 (c + 44n) (uint32 (slot + 1))
        wr64 (c + 32n) ud

    /// legacy provided buffers: hands `count` buffers of `size` bytes from id `first` (at `addr`) to group `group`
    member this.Provide(group: uint16, first: int, count: int, addr: nativeint, size: int, ud: uint64) =
        let s = this.Sqe()
        wr8 s OP_PROVIDE_BUFFERS
        wr8 (s + 1n) SQE_CQE_SKIP_SUCCESS
        wri32 (s + 4n) count
        wr64 (s + 8n) (uint64 first)
        wr64 (s + 16n) (uint64 addr)
        wr32 (s + 24n) (uint32 size)
        wr64 (s + 32n) ud
        wr16 (s + 40n) group

/// A cross-thread mailbox for a ring's owner: other threads add items and wake the owner through an eventfd that
/// the owner's ring reads. The eventfd is written only when the owner may be asleep (one write per wake-up).
[<Sealed>]
type Mailbox<'T>() =
    let q = System.Collections.Concurrent.ConcurrentQueue<'T>()
    let efd =
        let fd = Native.eventfd (0u, 0x80000) // EFD_CLOEXEC (blocking: io_uring would answer EAGAIN on a nonblocking one instead of waiting)
        if fd < 0 then raise (InvalidOperationException(sprintf "eventfd: errno %d" (Native.errno ())))
        fd
    let one = Marshal.AllocHGlobal 8
    let sink = Marshal.AllocHGlobal 8
    let mutable signaled = 0
    do NativePtr.write (NativePtr.ofNativeInt<uint64> one) 1UL
    member _.Fd = efd
    /// where the owner's eventfd read goes
    member _.Sink = sink
    /// any thread
    member _.Post(item: 'T) =
        q.Enqueue item
        if Volatile.Read &signaled = 0 && Interlocked.Exchange(&signaled, 1) = 0 then
            Native.write (efd, one, unativeint 8) |> ignore
    /// owner: after its eventfd read completed (before draining)
    member _.Rearmed() = Volatile.Write(&signaled, 0)
    member _.TryTake(item: byref<'T>) = q.TryDequeue(&item)
    member _.IsEmpty = q.IsEmpty

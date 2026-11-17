/// A VFS over unix-excl that writes each commit's WAL frames with one pwrite. SQLite writes a transaction's frames one
/// after another, each as two writes (its 24-byte header, then the page): several thousand pwrite calls a second under
/// load. Here, while the loop thread runs COMMIT, writes that continue exactly where the previous one ended go into a
/// buffer, and the buffer goes to the file when the commit frame's page arrives (a frame header whose "database size"
/// field is non-zero marks the commit frame), inside SQLite's own call. So the commit is in the kernel before SQLite
/// publishes it in the WAL index and before COMMIT returns, and the buffer is empty whenever SQLite isn't inside that
/// call: every read, by either connection, finds the file whole. Any other write (outside COMMIT, out of sequence, by
/// another handle) writes the buffer first and then goes straight through, as do syncs and truncates, so the bytes
/// that reach the file and their order are SQLite's own. Reads and locks are unix-excl's, unchanged.
/// (The pattern of SQLite's own vfstrace/multiplex shims, over the public VFS API.)
///
/// The database file's writes go straight through too, but the checkpoint thread's are paced (see `pace`): a pass
/// writes thousands of pages in a fraction of a second, and when that lifts the kernel's dirty memory past the point
/// where it pauses writers, the next one it pauses is the loop, in its WAL write.
module Vfs

#nowarn "9"

open System
open System.Runtime.InteropServices
open Microsoft.FSharp.NativeInterop
open Sqlite

[<UnmanagedFunctionPointer(CallingConvention.Cdecl)>]
type XOpen = delegate of nativeint * nativeint * nativeint * int * nativeint -> int
[<UnmanagedFunctionPointer(CallingConvention.Cdecl)>]
type XClose = delegate of nativeint -> int
[<UnmanagedFunctionPointer(CallingConvention.Cdecl)>]
type XRw = delegate of nativeint * nativeint * int * int64 -> int
[<UnmanagedFunctionPointer(CallingConvention.Cdecl)>]
type XTruncate = delegate of nativeint * int64 -> int
[<UnmanagedFunctionPointer(CallingConvention.Cdecl)>]
type XSync = delegate of nativeint * int -> int

[<DllImport(L, ExactSpelling = true)>]
extern nativeint sqlite3_vfs_find(byte[] name)

[<DllImport(L, ExactSpelling = true)>]
extern int sqlite3_vfs_register(nativeint vfs, int makeDflt)

let private SQLITE_OPEN_WAL = 0x00080000
let private SQLITE_OPEN_MAIN_DB = 0x00000100
// sqlite3_vfs: xOpen at 40, 168 bytes (version 3); sqlite3_io_methods: xClose 8, xRead 16, xWrite 24, xTruncate 32,
// xSync 40, 152 bytes (version 3)
let private VfsSize = 168
let private MethodsSize = 152

let private cap = 4 <<< 20
let private buf = NativeMemory.Alloc(unativeint cap) |> NativePtr.ofVoidPtr<byte> |> NativePtr.toNativeInt
let mutable private bufFile = 0n // the handle whose frames are in the buffer
let mutable private bufOff = 0L
let mutable private bufLen = 0
let mutable private commitPending = false // the last buffered write was the commit frame's header
let mutable private commitTid = 0 // the managed thread running COMMIT (0: none)

/// counters: commits written in one go, writes that went straight through during COMMIT, buffers written late
let mutable gathered = 0L
let mutable through = 0L
let mutable late = 0L

let mutable private baseVfs = 0n
let mutable private baseMethods = 0n
let mutable private bOpen: XOpen = null
let mutable private bClose: XClose = null
let mutable private bWrite: XRw = null
let mutable private bTruncate: XTruncate = null
let mutable private bSync: XSync = null
let mutable private walMethods = 0n
let mutable private dbMethods = 0n
let private gate = obj ()

let inline private rdp (p: nativeint) (off: int) = NativePtr.read (NativePtr.ofNativeInt<nativeint> (p + nativeint off))
let inline private wrp (p: nativeint) (off: int) (v: nativeint) = NativePtr.write (NativePtr.ofNativeInt<nativeint> (p + nativeint off)) v

/// The unix VFS passes at most 131,071 bytes to one pwrite (seekAndWriteFd masks the length to 17 bits), and reports a
/// longer write as SQLITE_FULL, so the buffer goes out in pieces that size.
let private maxWrite = 0x1ffff

/// writes the buffer out; SQLite's result code
let private flushBuf () =
    if bufLen = 0 then OK
    else
        let mutable rc = OK
        let mutable done' = 0
        while rc = OK && done' < bufLen do
            let n = min maxWrite (bufLen - done')
            rc <- bWrite.Invoke(bufFile, buf + nativeint done', n, bufOff + int64 done')
            done' <- done' + n
        bufLen <- 0
        commitPending <- false
        rc

let private writeW (f: nativeint) (p: nativeint) (amt: int) (off: int64) =
    if commitTid <> 0 && Environment.CurrentManagedThreadId = commitTid && amt <= cap then
        // inside COMMIT on the loop thread
        let mutable rc = OK
        if bufLen > 0 && (f <> bufFile || off <> bufOff + int64 bufLen || bufLen + amt > cap) then
            if f = bufFile then rc <- flushBuf ()
        if rc <> OK then rc
        elif bufLen = 0 || f = bufFile then
            if bufLen = 0 then
                bufFile <- f
                bufOff <- off
            Buffer.MemoryCopy(NativePtr.toVoidPtr (NativePtr.ofNativeInt<byte> p),
                              NativePtr.toVoidPtr (NativePtr.ofNativeInt<byte> (buf + nativeint bufLen)), int64 amt, int64 amt)
            bufLen <- bufLen + amt
            if commitPending then
                // the commit frame's page: the commit goes to the file now, before SQLite publishes it
                gathered <- gathered + 1L
                flushBuf ()
            else
                // a frame header (24 bytes) with a non-zero database size (bytes 4-7) starts the commit frame
                if amt = 24 then
                    let q = NativePtr.ofNativeInt<byte> p
                    commitPending <- (NativePtr.get q 4 ||| NativePtr.get q 5 ||| NativePtr.get q 6 ||| NativePtr.get q 7) <> 0uy
                OK
        else
            // another handle: its write doesn't depend on frames SQLite hasn't published yet
            through <- through + 1L
            bWrite.Invoke(f, p, amt, off)
    else
        let rc = if bufLen > 0 && f = bufFile then flushBuf () else OK
        if rc <> OK then rc else bWrite.Invoke(f, p, amt, off)

let private truncateW (f: nativeint) (size: int64) =
    let rc = if bufLen > 0 && f = bufFile then flushBuf () else OK
    if rc <> OK then rc else bTruncate.Invoke(f, size)

let private syncW (f: nativeint) (flags: int) =
    let rc = if bufLen > 0 && f = bufFile then flushBuf () else OK
    if rc <> OK then rc else bSync.Invoke(f, flags)

let private closeW (f: nativeint) =
    let rc = if bufLen > 0 && f = bufFile then flushBuf () else OK
    if bufFile = f then bufFile <- 0n
    let rc2 = bClose.Invoke f
    if rc <> OK then rc else rc2

// ---------- the checkpoint thread's database writes, paced by the kernel's dirty memory ----------

let private vmCap = 16384
let private vmBuf = NativeMemory.Alloc(unativeint vmCap) |> NativePtr.ofVoidPtr<byte> |> NativePtr.toNativeInt
let mutable private vmFd = -2
let mutable private paceFd = -1 // a descriptor on the database file
let mutable private paceTid = 0 // the checkpoint thread
let mutable private paced = 0 // bytes it has written since it last looked
/// counters: looks at the dirty state, looks that found it high, ms waited
let mutable paceLooks = 0L
let mutable paceHigh = 0L
let mutable paceMs = 0L

/// the value of `name` (with its trailing space) in the n bytes of /proc/vmstat read into vmBuf; -1 when absent
let private vmValue (name: byte[]) (n: int) =
    let p = NativePtr.ofNativeInt<byte> vmBuf
    let mutable v = -1L
    let mutable i = 0
    while v < 0L && i + name.Length < n do
        let mutable k = 0
        while k < name.Length && NativePtr.get p (i + k) = name.[k] do k <- k + 1
        if k = name.Length then
            v <- 0L
            let mutable j = i + k
            while j < n && NativePtr.get p j >= '0'B && NativePtr.get p j <= '9'B do
                v <- v * 10L + int64 (NativePtr.get p j - '0'B)
                j <- j + 1
        else
            while i < n && NativePtr.get p i <> '\n'B do i <- i + 1
            i <- i + 1
    v

let private kDirty = "nr_dirty "B
let private kWriteback = "nr_writeback "B
let private kThresh = "nr_dirty_threshold "B
let private kBg = "nr_dirty_background_threshold "B

/// True when the system's dirty and under-writeback pages are within a quarter of the level up to which the kernel
/// lets every writer run free, halfway between its background and its hard threshold (balance_dirty_pages).
let private dirtyHigh () =
    if vmFd = -2 then vmFd <- Native.``open`` ("/proc/vmstat\000"B, 0x80000)
    let n = if vmFd >= 0 then int (Native.pread (vmFd, vmBuf, unativeint vmCap, 0L)) else 0
    if n <= 0 then false
    else
        let d = vmValue kDirty n
        let w = vmValue kWriteback n
        let t = vmValue kThresh n
        let b = vmValue kBg n
        d >= 0L && w >= 0L && t > 0L && b >= 0L && (d + w) * 8L > (t + b) * 3L

/// After each MiB the checkpoint thread writes to the database: when dirty memory is high, its own pages are sent to
/// the disk and waited for (sync_file_range: data only, no journal commit), then it waits a little for the rest (the
/// WAL's) to drain, 250 ms at most. With room to spare this is one read of /proc/vmstat and the pages stay dirty, so
/// the kernel still writes a page once however many passes wrote it.
let private paceNow () =
    paceLooks <- paceLooks + 1L
    if dirtyHigh () then
        paceHigh <- paceHigh + 1L
        let t0 = Diagnostics.Stopwatch.GetTimestamp()
        Native.sync_file_range (paceFd, 0L, 0L, 6u) |> ignore // SYNC_FILE_RANGE_WRITE | SYNC_FILE_RANGE_WAIT_AFTER
        let mutable tries = 0
        while tries < 25 && dirtyHigh () do
            Native.usleep 10000u |> ignore
            tries <- tries + 1
        paceMs <- paceMs + int64 (Diagnostics.Stopwatch.GetElapsedTime(t0).TotalMilliseconds)

let private writeDb (f: nativeint) (p: nativeint) (amt: int) (off: int64) =
    let rc = bWrite.Invoke(f, p, amt, off)
    if paceFd >= 0 && Environment.CurrentManagedThreadId = paceTid then
        paced <- paced + amt
        if paced >= (1 <<< 20) then
            paced <- 0
            paceNow ()
    rc

/// Called by the checkpoint thread: from now on its writes to the database file are paced. fd: a descriptor on it.
let pace (fd: int) =
    paceTid <- Environment.CurrentManagedThreadId
    paceFd <- fd

let private wDbWrite = XRw(fun f p amt off -> writeDb f p amt off)
let private wWrite = XRw(fun f p amt off -> writeW f p amt off)
let private wTruncate = XTruncate(fun f size -> truncateW f size)
let private wSync = XSync(fun f flags -> syncW f flags)
let private wClose = XClose(fun f -> closeW f)

let private vOpen =
    XOpen(fun _ name f flags outFlags ->
        let rc = bOpen.Invoke(baseVfs, name, f, flags, outFlags)
        let m = if rc = OK then rdp f 0 else 0n
        if rc = OK && flags &&& (SQLITE_OPEN_WAL ||| SQLITE_OPEN_MAIN_DB) <> 0 && m <> 0n then
            lock gate (fun () ->
                if baseMethods = 0n then
                    if NativePtr.read (NativePtr.ofNativeInt<int> m) < 3 then failwith "unix-excl io methods older than version 3"
                    bClose <- Marshal.GetDelegateForFunctionPointer<XClose>(rdp m 8)
                    bWrite <- Marshal.GetDelegateForFunctionPointer<XRw>(rdp m 24)
                    bTruncate <- Marshal.GetDelegateForFunctionPointer<XTruncate>(rdp m 32)
                    bSync <- Marshal.GetDelegateForFunctionPointer<XSync>(rdp m 40)
                    let w = Marshal.AllocHGlobal MethodsSize
                    Buffer.MemoryCopy(NativePtr.toVoidPtr (NativePtr.ofNativeInt<byte> m),
                                      NativePtr.toVoidPtr (NativePtr.ofNativeInt<byte> w), int64 MethodsSize, int64 MethodsSize)
                    wrp w 8 (Marshal.GetFunctionPointerForDelegate wClose)
                    wrp w 24 (Marshal.GetFunctionPointerForDelegate wWrite)
                    wrp w 32 (Marshal.GetFunctionPointerForDelegate wTruncate)
                    wrp w 40 (Marshal.GetFunctionPointerForDelegate wSync)
                    walMethods <- w
                    let d = Marshal.AllocHGlobal MethodsSize
                    Buffer.MemoryCopy(NativePtr.toVoidPtr (NativePtr.ofNativeInt<byte> m),
                                      NativePtr.toVoidPtr (NativePtr.ofNativeInt<byte> d), int64 MethodsSize, int64 MethodsSize)
                    wrp d 24 (Marshal.GetFunctionPointerForDelegate wDbWrite)
                    dbMethods <- d
                    baseMethods <- m
                if m = baseMethods then wrp f 0 (if flags &&& SQLITE_OPEN_WAL <> 0 then walMethods else dbMethods))
        rc)

/// COMMIT starts on this thread: its contiguous WAL writes are gathered until the commit frame
let enter () = commitTid <- Environment.CurrentManagedThreadId

/// COMMIT returned: anything still buffered (a commit frame that was never recognized) goes out now. SQLite's rc.
let leave () =
    commitTid <- 0
    if bufLen > 0 then
        late <- late + 1L
        flushBuf ()
    else OK

/// Registers the VFS `name` over `over` (unix-excl); false when `over` isn't there.
let register (name: string) (over: string) =
    baseVfs <- sqlite3_vfs_find (Text.Encoding.ASCII.GetBytes(over + "\000"))
    if baseVfs = 0n || NativePtr.read (NativePtr.ofNativeInt<int> baseVfs) < 1 then false
    else
        bOpen <- Marshal.GetDelegateForFunctionPointer<XOpen>(rdp baseVfs 40)
        let v = Marshal.AllocHGlobal VfsSize
        NativeMemory.Clear(NativePtr.toVoidPtr (NativePtr.ofNativeInt<byte> v), unativeint VfsSize)
        let ver = NativePtr.read (NativePtr.ofNativeInt<int> baseVfs)
        let size = match ver with | 1 -> 136 | 2 -> 144 | _ -> VfsSize
        Buffer.MemoryCopy(NativePtr.toVoidPtr (NativePtr.ofNativeInt<byte> baseVfs),
                          NativePtr.toVoidPtr (NativePtr.ofNativeInt<byte> v), int64 size, int64 size)
        wrp v 16 0n // pNext
        wrp v 24 (Marshal.StringToHGlobalAnsi name)
        wrp v 40 (Marshal.GetFunctionPointerForDelegate vOpen)
        sqlite3_vfs_register (v, 0) = OK

/// A VFS over unix-excl that writes each commit's WAL frames with one pwrite. SQLite writes a transaction's frames one
/// after another, each as two writes (its 24-byte header, then the page): several thousand pwrite calls a second under
/// load. Here, while the loop thread runs COMMIT, writes that continue exactly where the previous one ended go into a
/// buffer, and the buffer goes to the file when the commit frame's page arrives (a frame header whose "database size"
/// field is non-zero marks the commit frame), inside SQLite's own call. So the commit is in the kernel before SQLite
/// publishes it in the WAL index and before COMMIT returns, and the buffer is empty whenever SQLite isn't inside that
/// call: every read, by either connection, finds the file whole. Any other write (outside COMMIT, out of sequence, by
/// another handle) writes the buffer first and then goes straight through, as do syncs and truncates, so the bytes
/// that reach the file and their order are SQLite's own. Reads, locks and the database file are unix-excl's, unchanged.
/// (The pattern of SQLite's own vfstrace/multiplex shims, over the public VFS API.)
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

let private wWrite = XRw(fun f p amt off -> writeW f p amt off)
let private wTruncate = XTruncate(fun f size -> truncateW f size)
let private wSync = XSync(fun f flags -> syncW f flags)
let private wClose = XClose(fun f -> closeW f)

let private vOpen =
    XOpen(fun _ name f flags outFlags ->
        let rc = bOpen.Invoke(baseVfs, name, f, flags, outFlags)
        let m = if rc = OK then rdp f 0 else 0n
        if rc = OK && flags &&& SQLITE_OPEN_WAL <> 0 && m <> 0n then
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
                    baseMethods <- m
                if m = baseMethods then wrp f 0 walMethods)
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

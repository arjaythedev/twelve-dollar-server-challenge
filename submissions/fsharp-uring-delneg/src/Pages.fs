/// Keeps the pages of the process's own files (runtime, JIT, base library, SQLite: code and data) that it uses in
/// anonymous memory.
///
/// The box has no swap, so at the memory wall reclaim can only take file-backed pages, and the pages of a mapped
/// library are file-backed like any other: the loop thread then stalls on major faults that read its own code back
/// from disk. `mlock` is capped at RLIMIT_MEMLOCK (8 MB) and the runtime maps 66 MB. An anonymous page can't be
/// reclaimed without swap, so every page of those files that is in use is turned into one, with its content
/// unchanged, and the loop does the same once a second for pages first used later:
///  - private mappings: the page is copied on write. A writable one by MADV_POPULATE_WRITE, a read-only or executable
///    one by writing its own bytes back through /proc/self/mem, which the kernel allows on a private mapping without
///    a change of protection (how a debugger plants a breakpoint);
///  - read-only shared mappings (the runtime maps assemblies whole that way): a copy in anonymous memory is moved over
///    the run of pages with mremap, which replaces it in one step.
/// No call here can fault: a mapping that went away between the listing and the call is an error return.
module Pages

#nowarn "9"

open System.Runtime.InteropServices
open System.Text
open Microsoft.FSharp.NativeInterop

let private mapsCap = 1 <<< 20
let private pmCap = 8192 // page-table entries read per call
let private gap = 8L * 4096L // runs of a shared mapping this close are moved as one, to keep the mapping count low
let private maxShared = 1024
let mutable private mapsBuf = 0n
let mutable private pm = 0n
let mutable private pmFd = -1
let mutable private memFd = -1
// shared read-only mappings of the previous pass (start, end, inode): only a mapping that has been there for a pass
// is replaced, so one the runtime maps for a moment while it loads an assembly is left alone
let private prev: int64[] = Array.zeroCreate (3 * maxShared)
let private cur: int64[] = Array.zeroCreate (3 * maxShared)
let mutable private prevN = 0
let mutable private curN = 0
let mutable private moved = 0L
let mutable private failed = 0L
// the mapping being scanned: its file's name (in the maps text), its start and the file offset of its start, and the
// file once opened (-1: not yet, -2: can't be)
let mutable private curPath = 0n
let mutable private curBase = 0L
let mutable private curOff = 0L
let mutable private curFd = -1
// The folder the server runs from, as a mapped file's name starts in /proc/self/maps. The kernel's cached copy of a
// moved page is dropped only for the entry's own files, the ones in there: the cache of the system's files (the
// runtime, libc) is not the entry's to drop, and the kernel takes those copies back itself when memory gets tight.
let private own = Encoding.UTF8.GetBytes System.AppContext.BaseDirectory
let mutable private dropped = 0L

let private z (s: string) = Encoding.UTF8.GetBytes(s + "\000")

let inline private hexv (c: byte) =
    if c <= '9'B then int64 (c - '0'B)
    elif c >= 'a'B then int64 (c - 'a'B) + 10L
    else int64 (c - 'A'B) + 10L

/// kind 1: private, not writable; 2: private, writable; 3: shared, read-only
let private flush (kind: int) (s: int64) (e: int64) (exec: bool) (known: bool) =
    let len = e - s
    let a = nativeint s
    let n = unativeint len
    let prot = if exec then 5 else 1
    let r =
        if kind = 2 then (if Native.madvise (a, n, 23) = 0 then 1 else 0) // MADV_POPULATE_WRITE
        elif kind = 1 then
            if memFd >= 0 then (if int64 (Native.pwrite (memFd, a, n, s)) = len then 1 else 0)
            elif Native.mprotect (a, n, prot ||| 2) = 0 then
                let ok = Native.madvise (a, n, 23) = 0
                Native.mprotect (a, n, prot) |> ignore
                if ok then 1 else 0
            else 0
        elif not known || memFd < 0 then 2
        else
            let t = Native.mmap (0n, n, 3, 0x22, -1, 0L) // PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS
            if t = -1n then 0
            elif int64 (Native.pread (memFd, t, n, s)) = len
                 && Native.mprotect (t, n, prot) = 0
                 && Native.mremap (t, n, n, 3, a) = a then 1 // MREMAP_MAYMOVE|MREMAP_FIXED
            else
                Native.munmap (t, n) |> ignore
                0
    if r = 1 then
        moved <- moved + len
        // the kernel's cached copy of these pages is now a second copy that nothing maps: for a file of the entry's
        // own it is dropped at once, not left for reclaim to find when memory runs short
        if curFd = -1 then
            curFd <- Native.openAt (curPath, 0x80000)
            if curFd < 0 then curFd <- -2
        if curFd >= 0 && Native.posix_fadvise (curFd, curOff + (s - curBase), len, 4) = 0 then // POSIX_FADV_DONTNEED
            dropped <- dropped + len
    elif r = 0 then failed <- failed + len

/// the pages of [a, b) that are present and still the file's own: bits 63 (present) and 61 (file page) of
/// /proc/self/pagemap, without bit 62 (swapped)
let private scan (kind: int) (a: int64) (b: int64) (exec: bool) (known: bool) (path: nativeint) (off: int64) =
    curPath <- path
    curBase <- a
    curOff <- off
    // -2 (never opened, nothing dropped) unless the name starts with the server's own folder; the name ends in a NUL,
    // so the comparison stops inside it
    let name = NativePtr.ofNativeInt<byte> path
    let mutable k = 0
    while k < own.Length && NativePtr.get name k = own.[k] do k <- k + 1
    curFd <- if own.Length > 1 && k = own.Length then -1 else -2
    let g = if kind = 3 then gap else 0L
    let q = NativePtr.ofNativeInt<uint64> pm
    let mutable rs = -1L
    let mutable re = 0L
    let mutable at = a
    while at < b do
        let want = int (min (int64 pmCap) ((b - at) >>> 12))
        let got = int (Native.pread (pmFd, pm, unativeint (want * 8), (at >>> 12) * 8L)) / 8
        if got <= 0 then at <- b
        else
            for k in 0 .. got - 1 do
                if (NativePtr.get q k >>> 61) = 5UL then
                    let p = at + (int64 k <<< 12)
                    if rs >= 0L && p - re <= g then re <- p + 4096L
                    else
                        if rs >= 0L then flush kind rs re exec known
                        rs <- p
                        re <- p + 4096L
            at <- at + (int64 got <<< 12)
    if rs >= 0L then flush kind rs re exec known
    if curFd >= 0 then Native.close curFd |> ignore

let private mapsPath = z "/proc/self/maps"

let private pass () =
    let fd = Native.``open`` (mapsPath, 0x80000)
    if fd >= 0 then
        let mutable n = 0
        let mutable more = true
        while more && n < mapsCap do
            let r = int (Native.read (fd, mapsBuf + nativeint n, unativeint (mapsCap - n)))
            if r <= 0 then more <- false else n <- n + r
        Native.close fd |> ignore
        let p = NativePtr.ofNativeInt<byte> mapsBuf
        curN <- 0
        let mutable i = 0
        while i < n do
            // "start-end perms offset dev inode   path\n"
            let mutable e = i
            while e < n && NativePtr.get p e <> '\n'B do e <- e + 1
            if e < n then
                let mutable a = 0L
                while i < e && NativePtr.get p i <> '-'B do
                    a <- (a <<< 4) ||| hexv (NativePtr.get p i)
                    i <- i + 1
                i <- i + 1
                let mutable b = 0L
                while i < e && NativePtr.get p i <> ' 'B do
                    b <- (b <<< 4) ||| hexv (NativePtr.get p i)
                    i <- i + 1
                let pr = i + 1
                i <- pr + 5
                let mutable off = 0L
                while i < e && NativePtr.get p i <> ' 'B do
                    off <- (off <<< 4) ||| hexv (NativePtr.get p i)
                    i <- i + 1
                i <- i + 1
                while i < e && NativePtr.get p i <> ' 'B do i <- i + 1 // device
                i <- i + 1
                let mutable inode = 0L
                while i < e && NativePtr.get p i <> ' 'B do
                    inode <- inode * 10L + int64 (NativePtr.get p i - '0'B)
                    i <- i + 1
                while i < e && NativePtr.get p i = ' 'B do i <- i + 1
                // a file on disk: not a memfd, not a deleted file, not a device
                if pr + 3 < e && inode <> 0L && i + 5 < e && NativePtr.get p i = '/'B && NativePtr.get p (e - 1) <> ')'B
                   && not (NativePtr.get p (i + 1) = 'd'B && NativePtr.get p (i + 2) = 'e'B && NativePtr.get p (i + 3) = 'v'B
                           && NativePtr.get p (i + 4) = '/'B)
                   && NativePtr.get p pr = 'r'B && a < b then
                    let w = NativePtr.get p (pr + 1) = 'w'B
                    let x = NativePtr.get p (pr + 2) = 'x'B
                    NativePtr.set p e 0uy // the name, ended for open()
                    let path = mapsBuf + nativeint i
                    if NativePtr.get p (pr + 3) = 'p'B then scan (if w then 2 else 1) a b x true path off
                    elif not w then
                        let mutable known = false
                        for k in 0 .. prevN - 1 do
                            if prev.[3 * k] <= a && b <= prev.[3 * k + 1] && prev.[3 * k + 2] = inode then known <- true
                        if curN < maxShared then
                            cur.[3 * curN] <- a
                            cur.[3 * curN + 1] <- b
                            cur.[3 * curN + 2] <- inode
                            curN <- curN + 1
                        scan 3 a b x known path off
            i <- e + 1
        System.Array.Copy(cur, prev, 3 * curN)
        prevN <- curN

let mutable private armed = false
let mutable private log = false
let mutable private nextMs = 0L
let mutable private everyMs = 1000L

/// Two passes now (the second one for the shared mappings); `tick` does the later ones.
let start (logPasses: bool) =
    mapsBuf <- Marshal.AllocHGlobal mapsCap
    pm <- Marshal.AllocHGlobal(pmCap * 8)
    pmFd <- Native.``open`` (z "/proc/self/pagemap", 0x80000)
    memFd <- Native.``open`` (z "/proc/self/mem", 0x80002) // O_RDWR | O_CLOEXEC
    if pmFd < 0 then eprintfn "pages: no /proc/self/pagemap (errno %d)" (Native.errno ())
    else
        pass ()
        pass ()
        eprintfn "moved %d KB of code and data in use from file-backed to anonymous memory (%d KB of it the entry's own files: their cached copy dropped)%s%s"
            (moved >>> 10) (dropped >>> 10)
            (if failed > 0L then sprintf ", %d KB failed" (failed >>> 10) else "")
            (if memFd < 0 then " (no /proc/self/mem: by mprotect, shared mappings left)" else "")
        log <- logPasses
        armed <- true

/// Called by the loop on every pass of its own with the tick count in ms: a pass a second, on the loop thread (so no
/// other thread of the process waits for the address-space lock a pass takes), every 8 s once nothing new turns up.
let tick (ms: int64) =
    if armed && ms >= nextMs then
        let before = moved
        let t0 = if log then System.Diagnostics.Stopwatch.GetTimestamp() else 0L
        pass ()
        everyMs <- if moved <> before then 1000L else min 8000L (everyMs * 2L)
        nextMs <- ms + everyMs
        if log then
            eprintfn "pages: %d KB moved (%d in all, %d failed) in %d us" ((moved - before) >>> 10) (moved >>> 10)
                (failed >>> 10) (int64 (System.Diagnostics.Stopwatch.GetElapsedTime(t0).TotalMicroseconds))

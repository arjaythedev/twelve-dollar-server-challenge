/// SQLite's own page cache (pcache1), installed through SQLITE_CONFIG_PCACHE2 with one method of its own: xRekey.
/// A B-tree split that puts its new pages in order swaps two of them through a temporary page number, the lock-byte
/// page's (1 GiB / page size + 1: 262,145 for 4,096-byte pages). pcache1 remembers the largest page number it has held
/// and lowers it only when a truncate walks the cache, so after every such swap the next commit's truncate (drop the
/// pages past the end of the file) scans pcache1's whole hash table, tens of thousands of slots, to find nothing. Here
/// the page waits out the swap under number 0, which SQLite never gives a page, so pcache1's largest number stays near
/// the end of the file and the truncate looks at a slot or two. Fetches, unpins, truncates and sizes are pcache1's.
/// xRekey only runs inside a split, during an INSERT, which the loop steps with a GC transition.
module Pcache

#nowarn "9"

open System
open System.Runtime.InteropServices
open Microsoft.FSharp.NativeInterop
open Sqlite

[<UnmanagedFunctionPointer(CallingConvention.Cdecl)>]
type XRekey = delegate of nativeint * nativeint * uint32 * uint32 -> unit

/// int sqlite3_config(int, ...): the two options used here take one pointer
[<DllImport(L, ExactSpelling = true)>]
extern int sqlite3_config(int op, nativeint arg)

let private CONFIG_PCACHE2 = 18
let private CONFIG_GETPCACHE2 = 19
// sqlite3_pcache_methods2: iVersion, pArg, xInit, xShutdown, xCreate, xCachesize, xPagecount, xFetch, xUnpin,
// xRekey (72), xTruncate, xDestroy, xShrink: 104 bytes
let private MethodsSize = 104
let private RekeyOff = 72

let mutable private tempKey = 0u // the swap's temporary page number; 0 until the page size is known to be 4,096
let mutable private baseRekey: XRekey = null

/// pages that waited out a swap under number 0
let mutable parked = 0L

let private rekey =
    XRekey(fun p pg oldKey newKey ->
        let t = tempKey
        if t <> 0u && (newKey = t || oldKey = t) then
            if newKey = t then parked <- parked + 1L
            baseRekey.Invoke(p, pg, (if oldKey = t then 0u else oldKey), (if newKey = t then 0u else newKey))
        else baseRekey.Invoke(p, pg, oldKey, newKey))

/// Installs the methods: before sqlite3_initialize. false if SQLite refused (then pcache1 is used as it is).
let install () =
    let m = Marshal.AllocHGlobal MethodsSize
    NativeMemory.Clear(NativePtr.toVoidPtr (NativePtr.ofNativeInt<byte> m), unativeint MethodsSize)
    if sqlite3_config (CONFIG_GETPCACHE2, m) <> OK || Marshal.ReadIntPtr(m, RekeyOff) = 0n then false
    else
        baseRekey <- Marshal.GetDelegateForFunctionPointer<XRekey>(Marshal.ReadIntPtr(m, RekeyOff))
        Marshal.WriteIntPtr(m, RekeyOff, Marshal.GetFunctionPointerForDelegate rekey)
        sqlite3_config (CONFIG_PCACHE2, m) = OK

/// The main database's page size. Only for 4,096-byte pages (the size SQLite also gives its temporary databases) is
/// the temporary number taken over: 262,145 is the lock-byte page, which SQLite never uses for a page of data.
let enable (pageSize: int) = if pageSize = 4096 && not (isNull baseRekey) then tempKey <- 0x40000000u / 4096u + 1u

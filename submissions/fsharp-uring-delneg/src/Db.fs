/// The database: one SQLite connection driven by the event loop thread, one transaction per loop pass (its writes are
/// group-committed, rule 6), and a checkpoint thread with a connection of its own.
/// /feed and /posts/:id read SQLite's B-tree layer through its cursor calls (Sqlite.fs): no bytecode engine, no
/// sqlite3_column_* calls. Pages still come through SQLite's pager, WAL and page cache, inside the pass's read
/// transaction, so every request reads the rows while it is served (rule 5). Nothing is kept between requests but
/// schema facts (root pages, key descriptions, checked against the schema cookie on each read) and prepared
/// statements. A row the B-tree path doesn't cover (overflow page, unexpected type) is read with SQL instead.
module Db

#nowarn "9"
#nowarn "51"

open System
open System.Runtime.InteropServices
open System.Threading
open Microsoft.FSharp.NativeInterop
open Sqlite

let private sqlFeed =
    "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) "
    + "FROM posts p JOIN users u ON u.id = p.user_id ORDER BY p.created_at DESC, p.id DESC LIMIT 20"
let private sqlPost =
    "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) "
    + "FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = ?1"

/// SQLite's unix-excl VFS: the process holds the file's lock for its life, and the WAL index lives in its own memory,
/// shared by its two connections (no -shm file, no file locks per transaction). VFS=unix for the default one.
let private vfsName =
    match Environment.GetEnvironmentVariable "VFS" with
    | null | "" -> "unix-excl"
    | v -> v
/// WALBUF=0: the loop's WAL writes go straight to unix-excl, one pwrite per frame header and one per page (Vfs.fs)
let walBuf = vfsName = "unix-excl" && Environment.GetEnvironmentVariable "WALBUF" <> "0"
let mutable private vfs = z vfsName

let private chunkMB = match Int32.TryParse(Environment.GetEnvironmentVariable "CHUNK_MB") with | true, v when v > 0 -> v | _ -> 0

let mutable private db = 0n
let mutable private sHealth = 0n
let mutable private sFeed = 0n
let mutable private sPost = 0n
let mutable private sCreate = 0n
let mutable private sLike = 0n
let mutable private sExists = 0n
let mutable private sBegin = 0n
let mutable private sCommit = 0n
let mutable private sRollback = 0n

/// WAL frames after the last commit (sqlite3_wal_hook; the hook also turns SQLite's automatic checkpoint off)
let mutable walFrames = 0

[<UnmanagedFunctionPointer(CallingConvention.Cdecl)>]
type private WalHook = delegate of nativeint * nativeint * nativeint * int -> int
let private walHook = WalHook(fun _ _ _ frames -> walFrames <- frames; 0)

/// failed writes, commits and BEGINs since start (each logged with SQLite's extended code: the first 20, then every 1000th)
let mutable dbErrors = 0L
let private logErr (what: string) (rc: int) =
    dbErrors <- dbErrors + 1L
    if dbErrors <= 20L || dbErrors % 1000L = 0L then
        eprintfn "sqlite %s failed: rc %d extended %d %s (%d so far)" what rc (sqlite3_extended_errcode db) (errmsg db) dbErrors

let private exec (sql: string) =
    let rc = sqlite3_exec (db, z sql, 0n, 0n, 0n)
    if rc <> OK then failwithf "%s: %s" sql (errmsg db)

let private prep (sql: string) =
    let mutable s = 0n
    if sqlite3_prepare_v3 (db, z sql, -1, PREPARE_PERSISTENT, &s, 0n) <> OK then failwithf "prepare %s: %s" sql (errmsg db)
    s

let private scalar (sql: string) =
    let s = prep sql
    let v = if sqlite3_step s = ROW then sqlite3_column_int64 (s, 0) else Int64.MinValue
    sqlite3_reset s |> ignore
    v

let private text (sql: string) =
    let s = prep sql
    let v = if sqlite3_step s = ROW then Marshal.PtrToStringUTF8(sqlite3_column_text (s, 0)) else null
    sqlite3_reset s |> ignore
    v

// ---------- records (https://sqlite.org/fileformat2.html#record_format) ----------

[<Struct; NoEquality; NoComparison>]
type Col =
    val mutable P: nativeint
    val mutable Len: int
    val mutable T: uint32
    val mutable V: int64

let private cols = Array.zeroCreate<Col> 8
let private intSize = [| 0; 1; 2; 3; 4; 6; 8; 8; 0; 0 |]

let inline private rd (p: nativeint) (i: int) = NativePtr.read (NativePtr.ofNativeInt<byte> (p + nativeint i))
let inline private isInt (t: uint32) = (t >= 1u && t <= 6u) || t = 8u || t = 9u
let inline private isText (t: uint32) = t >= 13u && t &&& 1u = 1u

/// SQLite varint at p[i..] into v (clamped to 32 bits); returns its length
let private varint (p: nativeint) (i: int) (v: byref<uint32>) =
    let b0 = rd p i
    if b0 < 0x80uy then
        v <- uint32 b0
        1
    else
        let mutable x = 0UL
        let mutable k = 0
        let mutable fin = false
        while not fin do
            let b = rd p (i + k)
            if k = 8 then
                x <- (x <<< 8) ||| uint64 b
                fin <- true
            else
                x <- (x <<< 7) ||| uint64 (b &&& 0x7Fuy)
                if b < 0x80uy then fin <- true
            k <- k + 1
        v <- if x > 0xFFFFFFFFUL then 0xFFFFFFFFu else uint32 x
        k

/// Decodes the first n columns of the cursor's record into cols. False if the record is not wholly on its page
/// (overflow) or is malformed.
let private record (cur: nativeint) (n: int) =
    let size = sqlite3BtreePayloadSize cur
    let mutable amt = 0u
    let p = sqlite3BtreePayloadFetch (cur, &amt)
    if amt < size || size < 1u then false
    else
        let mutable hdr = 0u
        let mutable h = varint p 0 &hdr
        if hdr > size || hdr < uint32 h then false
        else
            let e = int size
            let mutable d = int hdr
            let mutable ok = true
            let mutable i = 0
            while ok && i < n do
                if uint32 h >= hdr then ok <- false
                else
                    let mutable t = 0u
                    h <- h + varint p h &t
                    let mutable len = 0
                    let mutable v = 0L
                    if t >= 12u then len <- int ((t - 12u) / 2u)
                    elif t = 10u || t = 11u then ok <- false
                    else
                        len <- intSize.[int t]
                        if t = 9u then v <- 1L
                        elif t >= 1u && t <= 6u && d + len <= e then
                            v <- int64 (sbyte (rd p d))
                            for k in 1 .. len - 1 do
                                v <- (v <<< 8) ||| int64 (rd p (d + k))
                    if ok then
                        if d + len > e then ok <- false
                        else
                            cols.[i].P <- p + nativeint d
                            cols.[i].Len <- len
                            cols.[i].T <- t
                            cols.[i].V <- v
                            d <- d + len
                i <- i + 1
            ok

// ---------- B-tree reads ----------

let mutable private bt = 0n
let mutable useBt = false
let mutable private cookie = 0u
let mutable private rootPosts = 0u
let mutable private rootUsers = 0u
let mutable private rootTop = 0u
let mutable private rootLikes = 0u
let mutable private topKey = 0n
let mutable private likesKey = 0n

[<Literal>]
let private CTop = 0
[<Literal>]
let private CPosts = 1
[<Literal>]
let private CUsers = 2
[<Literal>]
let private CLikes = 3
[<Literal>]
let private CFeedPost = 4
[<Literal>]
let private CAuthor = 24
[<Literal>]
let private CCount = 44

/// Cursors stay open for the rest of the transaction (closed before COMMIT/ROLLBACK), which read-only passes share; a write in between makes
/// SQLite save and reseek them, as for any open statement cursor. The feed reads its n-th post and that post's author
/// through cursors of their own: requests in one pass mostly ask for the same rows, and sqlite3BtreeTableMoveto returns
/// at once from a cursor already on the key. The row is still decoded from SQLite's page for each request.
let private curs = Array.zeroCreate<nativeint> CCount
let private isOpen = Array.zeroCreate<bool> CCount
let mutable private searchMem = 0n
let mutable private searchKey = 0n

let private cur (i: int) =
    let c = curs.[i]
    if isOpen.[i] then c
    else
        let root, key =
            if i = CTop then rootTop, topKey
            elif i = CLikes then rootLikes, likesKey
            elif i = CUsers || i >= CAuthor then rootUsers, 0n
            else rootPosts, 0n
        NativeMemory.Clear(NativePtr.toVoidPtr (NativePtr.ofNativeInt<byte> c), unativeint (sqlite3BtreeCursorSize ()))
        sqlite3BtreeCursorZero c
        if sqlite3BtreeCursor (bt, root, 0, key, c) <> OK then 0n
        else
            isOpen.[i] <- true
            c

let private endCursors () =
    for i in 0 .. CCount - 1 do
        if isOpen.[i] then
            sqlite3BtreeCloseCursor curs.[i] |> ignore
            isOpen.[i] <- false

let private beginRead () =
    if sqlite3_get_autocommit db <> 0 then false // only inside the pass's BEGIN
    elif sqlite3BtreeBeginTrans (bt, 0, 0n) <> OK then false
    else
        let mutable c = 0u
        sqlite3BtreeGetMeta (bt, BTREE_SCHEMA_VERSION, &c)
        c = cookie

/// Moves the likes_post_id_idx cursor to its first entry with post_id >= id. 0: on an entry, 1: none, -1: error.
let private likesSeek (c: nativeint) (id: int64) =
    let m = searchMem
    let k = searchKey
    NativeMemory.Clear(NativePtr.toVoidPtr (NativePtr.ofNativeInt<byte> m), unativeint MemSize)
    NativeMemory.Clear(NativePtr.toVoidPtr (NativePtr.ofNativeInt<byte> k), unativeint UnpackedSize)
    NativePtr.write (NativePtr.ofNativeInt<int64> m) id
    NativePtr.write (NativePtr.ofNativeInt<uint16> (m + 20n)) MEM_Int
    NativePtr.write (NativePtr.ofNativeInt<nativeint> k) likesKey
    NativePtr.write (NativePtr.ofNativeInt<nativeint> (k + 8n)) m
    NativePtr.write (NativePtr.ofNativeInt<uint16> (k + 28n)) 1us
    NativePtr.write (NativePtr.ofNativeInt<sbyte> (k + 30n)) 1y // entries equal on post_id sort after the key
    let mutable res = 0
    if sqlite3BtreeIndexMoveto (c, k, &res) <> OK then -1
    elif sqlite3BtreeEof c <> 0 then 1
    elif res < 0 then
        let rc = sqlite3BtreeNext (c, 0)
        if rc = DONE then 1 elif rc <> OK then -1 else 0
    else 0

/// The first column of the cursor's index entry (likes_post_id_idx: post_id) when it is an integer stored on the page:
/// a header-size byte, a type byte, then the big-endian value. Index entries here are a few bytes, so this is the
/// only shape it meets; anything else goes through the general decoder. False: not an integer, or malformed.
let private firstInt (c: nativeint) (v: byref<int64>) =
    // PayloadSize first: it parses the cell the cursor is on (getCellInfo), which PayloadFetch only reads back
    sqlite3BtreePayloadSize c |> ignore
    let mutable amt = 0u
    let p = sqlite3BtreePayloadFetch (c, &amt)
    let hdr = if amt >= 2u then rd p 0 else 0x80uy
    let t = if amt >= 2u then rd p 1 else 0x80uy
    if hdr < 0x80uy && t >= 1uy && t <= 6uy && hdr >= 2uy && uint32 hdr + uint32 intSize.[int t] <= amt then
        let h = int hdr
        let mutable x = int64 (sbyte (rd p h))
        for k in 1 .. intSize.[int t] - 1 do
            x <- (x <<< 8) ||| int64 (rd p (h + k))
        v <- x
        true
    elif hdr < 0x80uy && (t = 8uy || t = 9uy) && hdr >= 2uy then
        v <- int64 (t - 8uy)
        true
    elif record c 1 && isInt cols.[0].T then
        v <- cols.[0].V
        true
    else false

/// Adds the likes of post ids lo..hi to cnt[id - lo]. 0 ok, -1 error.
let private likesCount (c: nativeint) (lo: int64) (hi: int64) (cnt: Span<int64>) =
    let r = likesSeek c lo
    if r <> 0 then (if r < 0 then -1 else 0)
    else
        let mutable result = 1
        let mutable v = 0L
        while result = 1 do
            if not (firstInt c &v) then result <- -1
            else
                if v > hi then result <- 0
                else
                    if v >= lo then cnt.[int (v - lo)] <- cnt.[int (v - lo)] + 1L
                    let rc = sqlite3BtreeNext (c, 0)
                    if rc = DONE then result <- 0
                    elif rc <> OK then result <- -1
        result

[<Struct; NoEquality; NoComparison>]
type View =
    val mutable Body: nativeint
    val mutable BodyLen: int
    val mutable Ca: nativeint
    val mutable CaLen: int
    val mutable Au: nativeint
    val mutable AuLen: int

/// The posts row under pc and its author (through uc). 0 ok, -1 not covered, 1 no such author (the SQL join drops it).
// ---------- specialized decoders for the three record shapes the reads meet ----------
// Each reads only the columns it needs, with the record's type codes taken as 1- or 2-byte varints (every column
// here fits: a 500-character body's type code is 2 bytes). Any other shape goes through `record` above.

/// a type code of 1 or 2 varint bytes at p[h], h moved past it; 0xFFFFFFFF otherwise
let inline private code (p: nativeint) (h: byref<int>) =
    let b0 = rd p h
    if b0 < 0x80uy then
        h <- h + 1
        uint32 b0
    else
        let b1 = rd p (h + 1)
        if b1 < 0x80uy then
            h <- h + 2
            (uint32 (b0 &&& 0x7Fuy) <<< 7) ||| uint32 b1
        else 0xFFFFFFFFu

/// bytes of a column of type code t (t valid and not 10/11)
let inline private width (t: uint32) =
    if t >= 12u then int ((t - 12u) >>> 1)
    else
        match t with
        | 5u -> 6
        | 6u | 7u -> 8
        | 8u | 9u | 0u -> 0
        | _ -> int t

/// an integer column of type code t at p[d]
let inline private intAt (p: nativeint) (d: int) (t: uint32) =
    match t with
    | 8u -> 0L
    | 9u -> 1L
    | _ ->
        let n = width t
        let mutable x = int64 (sbyte (rd p d))
        for k in 1 .. n - 1 do
            x <- (x <<< 8) ||| int64 (rd p (d + k))
        x

/// the cursor's record when it is wholly on its page with a one-byte header length: its address, size and header
/// length (0 when not)
let inline private local (c: nativeint) (p: byref<nativeint>) (size: byref<int>) =
    let sz = sqlite3BtreePayloadSize c
    let mutable amt = 0u
    p <- sqlite3BtreePayloadFetch (c, &amt)
    size <- int sz
    if amt < sz || sz < 2u then 0
    else
        let hdr = rd p 0
        if hdr < 0x80uy && uint32 hdr <= sz then int hdr else 0

/// posts_created_at_id_idx entry (created_at, id): the id
let private topId (c: nativeint) (id: byref<int64>) =
    let mutable p = 0n
    let mutable size = 0
    let hdr = local c &p &size
    let mutable h = 1
    let t0 = if hdr > 0 then code p &h else 0xFFFFFFFFu
    let t1 = if hdr > 0 && t0 <> 0xFFFFFFFFu then code p &h else 0xFFFFFFFFu
    if t1 <> 0xFFFFFFFFu && h <= hdr && isText t0 && isInt t1 && hdr + width t0 + width t1 <= size then
        id <- intAt p (hdr + width t0) t1
        true
    elif record c 2 && isInt cols.[1].T then
        id <- cols.[1].V
        true
    else false

/// posts row (id, user_id, body, created_at): the user id, body and created_at into v
let private postRow (c: nativeint) (uid: byref<int64>) (v: byref<View>) =
    let mutable p = 0n
    let mutable size = 0
    let hdr = local c &p &size
    let mutable h = 1
    let bad = 0xFFFFFFFFu
    let t0 = if hdr > 0 then code p &h else bad
    let t1 = if t0 <> bad then code p &h else bad
    let t2 = if t1 <> bad then code p &h else bad
    let t3 = if t2 <> bad then code p &h else bad
    if t3 <> bad && h <= hdr && (t0 = 0u || isInt t0) && isInt t1 && isText t2 && isText t3 then
        let d1 = hdr + width t0
        let d2 = d1 + width t1
        let d3 = d2 + width t2
        if d3 + width t3 > size then false
        else
            uid <- intAt p d1 t1
            v.Body <- p + nativeint d2
            v.BodyLen <- width t2
            v.Ca <- p + nativeint d3
            v.CaLen <- width t3
            true
    elif record c 4 && isInt cols.[1].T && isText cols.[2].T && isText cols.[3].T then
        uid <- cols.[1].V
        v.Body <- cols.[2].P
        v.BodyLen <- cols.[2].Len
        v.Ca <- cols.[3].P
        v.CaLen <- cols.[3].Len
        true
    else false

/// users row (id, username, ...): the username into v
let private userRow (c: nativeint) (v: byref<View>) =
    let mutable p = 0n
    let mutable size = 0
    let hdr = local c &p &size
    let mutable h = 1
    let bad = 0xFFFFFFFFu
    let t0 = if hdr > 0 then code p &h else bad
    let t1 = if t0 <> bad then code p &h else bad
    if t1 <> bad && h <= hdr && (t0 = 0u || isInt t0) && isText t1 && hdr + width t0 + width t1 <= size then
        v.Au <- p + nativeint (hdr + width t0)
        v.AuLen <- width t1
        true
    elif record c 2 && isText cols.[1].T then
        v.Au <- cols.[1].P
        v.AuLen <- cols.[1].Len
        true
    else false

/// The posts row under pc and its author (through uc). 0 ok, -1 not covered, 1 no such author (the SQL join drops it).
let private postView (pc: nativeint) (uc: nativeint) (v: byref<View>) =
    let mutable uid = 0L
    if not (postRow pc &uid &v) then -1
    else
        let mutable res = 0
        if sqlite3BtreeTableMoveto (uc, uid, 0, &res) <> OK then -1
        elif res <> 0 then 1
        elif not (userRow uc &v) then -1
        else 0

[<Literal>]
let private Span_ = 512

let private feedIds = Array.zeroCreate<int64> 20
let private feedLikes = Array.zeroCreate<int64> 20
let private tally = Array.zeroCreate<int64> Span_

let private jFeedOpen = "{\"posts\":["B
let private jFeedClose = "]}"B
let private jPostOpen = "{\"post\":"B

/// /feed body on the B-tree: true if written, false if the caller must use SQL (the body is then reset)
let private btFeed (o: Json.Out) =
    let start = o.N
    if not (beginRead ()) then false
    else
        let top = cur CTop
        let lc = cur CLikes
        if top = 0n || lc = 0n then false
        else
            let mutable n = 0
            let mutable res = 0
            let mutable rc = sqlite3BtreeFirst (top, &res) // the index is (created_at DESC, id DESC): newest first
            let mutable ok = true
            while ok && rc = OK && res = 0 && n < 20 do
                let mutable id = 0L
                if not (topId top &id) then ok <- false
                else
                    feedIds.[n] <- id
                    n <- n + 1
                    rc <- sqlite3BtreeNext (top, 0)
                    if rc = DONE then
                        rc <- OK
                        res <- 1
            if not ok || rc <> OK then false
            else
                let mutable lo = Int64.MaxValue
                let mutable hi = Int64.MinValue
                for i in 0 .. n - 1 do
                    lo <- min lo feedIds.[i]
                    hi <- max hi feedIds.[i]
                let mutable lrc = 0
                if n > 0 && hi - lo < int64 Span_ then
                    let span = Span<int64>(tally, 0, int (hi - lo + 1L))
                    span.Clear()
                    lrc <- likesCount lc lo hi span
                    for i in 0 .. n - 1 do
                        feedLikes.[i] <- tally.[int (feedIds.[i] - lo)]
                else
                    let mutable i = 0
                    while lrc = 0 && i < n do
                        feedLikes.[i] <- 0L
                        lrc <- likesCount lc feedIds.[i] feedIds.[i] (Span<int64>(feedLikes, i, 1))
                        i <- i + 1
                if lrc <> 0 then false
                else
                    Json.put o (ReadOnlySpan jFeedOpen)
                    let mutable first = true
                    let mutable i = 0
                    let mutable v = View()
                    while ok && i < n do
                        let pc = cur (CFeedPost + i)
                        let uc = cur (CAuthor + i)
                        if pc = 0n || uc = 0n then ok <- false
                        elif sqlite3BtreeTableMoveto (pc, feedIds.[i], 0, &res) <> OK then ok <- false
                        elif res = 0 then // not in the table: same as the SQL join
                            match postView pc uc &v with
                            | 0 ->
                                if not first then Json.putByte o ','B
                                first <- false
                                Json.putPost o feedIds.[i] (Json.native v.Body v.BodyLen) (Json.native v.Ca v.CaLen)
                                    (Json.native v.Au v.AuLen) feedLikes.[i]
                            | 1 -> ()
                            | _ -> ok <- false
                        i <- i + 1
                    if ok then
                        Json.put o (ReadOnlySpan jFeedClose)
                        true
                    else
                        o.N <- start
                        false

/// /posts/:id on the B-tree: 1 written, 0 no such post, -1 use SQL
let private btPost (id: int64) (o: Json.Out) =
    if not (beginRead ()) then -1
    else
        let pc = cur CPosts
        let uc = cur CUsers
        let lc = cur CLikes
        if pc = 0n || uc = 0n || lc = 0n then -1
        else
            let mutable res = 0
            if sqlite3BtreeTableMoveto (pc, id, 0, &res) <> OK then -1
            elif res <> 0 then 0
            else
                let mutable v = View()
                match postView pc uc &v with
                | 1 -> 0
                | 0 ->
                    let mutable c = 0L
                    if likesCount lc id id (Span<int64>(&&c |> NativePtr.toVoidPtr, 1)) <> 0 then -1
                    else
                        Json.put o (ReadOnlySpan jPostOpen)
                        Json.putPost o id (Json.native v.Body v.BodyLen) (Json.native v.Ca v.CaLen) (Json.native v.Au v.AuLen) c
                        Json.putByte o '}'B
                        1
                | _ -> -1

// ---------- SQL reads (the general path) ----------

let inline private colText (s: nativeint) (i: int) = Json.native (sqlite3_column_text (s, i)) (sqlite3_column_bytes (s, i))

let private sqlPostRow (s: nativeint) (o: Json.Out) =
    let id = sqlite3_column_int64 (s, 0)
    let body = colText s 1
    let ca = colText s 2
    let au = colText s 3
    Json.putPost o id body ca au (sqlite3_column_int64 (s, 4))

let private sqlFeedBody (o: Json.Out) =
    let start = o.N
    Json.put o (ReadOnlySpan jFeedOpen)
    let mutable rc = sqlite3_step sFeed
    let mutable n = 0
    while rc = ROW do
        if n > 0 then Json.putByte o ','B
        sqlPostRow sFeed o
        n <- n + 1
        rc <- sqlite3_step sFeed
    sqlite3_reset sFeed |> ignore
    if rc = DONE then
        Json.put o (ReadOnlySpan jFeedClose)
        true
    else
        o.N <- start
        false

let private sqlPostBody (id: int64) (o: Json.Out) =
    sqlite3_bind_int64 (sPost, 1, id) |> ignore
    let rc = sqlite3_step sPost
    let r =
        if rc = ROW then
            Json.put o (ReadOnlySpan jPostOpen)
            sqlPostRow sPost o
            Json.putByte o '}'B
            1
        elif rc = DONE then 0
        else -1
    sqlite3_reset sPost |> ignore
    r

// ---------- the request API (loop thread, inside Begin .. Commit) ----------

/// GET /feed body into o: true, or false on a database error
let feed (o: Json.Out) = (useBt && btFeed o) || sqlFeedBody o

/// GET /posts/:id body into o: 1 found, 0 no such post, -1 error
let post (id: int64) (o: Json.Out) =
    let r = if useBt then btPost id o else -1
    if r >= 0 then r else sqlPostBody id o

/// UTC "YYYY-MM-DDTHH:MM:SS.mmmZ" (24 bytes), the column default's strftime('%Y-%m-%dT%H:%M:%fZ','now') format
let private ts = GC.AllocateArray<byte>(24, pinned = true)
let mutable private tsSec = Int64.MinValue

let now () =
    let t = DateTime.UtcNow
    let sec = t.Ticks / TimeSpan.TicksPerSecond
    if sec <> tsSec then
        tsSec <- sec
        Text.Encoding.ASCII.GetBytes(t.ToString("yyyy-MM-dd'T'HH:mm:ss", Globalization.CultureInfo.InvariantCulture), 0, 19, ts, 0)
        |> ignore
        ts.[19] <- '.'B
        ts.[23] <- 'Z'B
    let ms = int (t.Ticks / TimeSpan.TicksPerMillisecond % 1000L)
    ts.[20] <- byte (48 + ms / 100)
    ts.[21] <- byte (48 + ms / 10 % 10)
    ts.[22] <- byte (48 + ms % 10)
    ts

/// POST /posts: the new id (created_at in `created`), or -1
let create (uid: int64) (body: nativeint) (bodyLen: int) (created: byref<byte[]>) =
    let t = now ()
    created <- t
    sqlite3_bind_int64 (sCreate, 1, uid) |> ignore
    sqlite3_bind_text (sCreate, 2, body, bodyLen, 0n) |> ignore // SQLITE_STATIC: the arena is pinned and kept until the reset
    sqlite3_bind_text (sCreate, 3, Marshal.UnsafeAddrOfPinnedArrayElement(t, 0), 24, 0n) |> ignore
    let rc = sqlite3_step_commit sCreate
    if rc <> DONE then logErr "insert post" rc
    sqlite3_reset sCreate |> ignore
    if rc = DONE then sqlite3_last_insert_rowid db else -1L

/// POST /posts/:id/like: 1 new like, 0 repeat, 2 no such post, -1 error
let like (uid: int64) (pid: int64) =
    let t = now ()
    sqlite3_bind_int64 (sLike, 1, uid) |> ignore
    sqlite3_bind_int64 (sLike, 2, pid) |> ignore
    sqlite3_bind_text (sLike, 3, Marshal.UnsafeAddrOfPinnedArrayElement(t, 0), 24, 0n) |> ignore
    let rc = sqlite3_step_commit sLike
    if rc <> DONE then logErr "insert like" rc
    sqlite3_reset sLike |> ignore
    if rc <> DONE then -1
    elif sqlite3_changes db = 1 then 1
    else
        sqlite3_bind_int64 (sExists, 1, pid) |> ignore
        let e = sqlite3_step sExists
        sqlite3_reset sExists |> ignore
        if e = ROW then 0 elif e = DONE then 2 else -1

let health () =
    let rc = sqlite3_step sHealth
    sqlite3_reset sHealth |> ignore
    rc = ROW

let lastError () = errmsg db

/// A pass that wrote nothing leaves its transaction open, cursors and all, for the next pass (keep). Only this
/// connection writes, and a pass that writes commits, so that transaction's snapshot is always the latest one.
let mutable private carried = false

let carrying () = carried

let begin' () =
    if carried && sqlite3_get_autocommit db = 0 then true
    else
        carried <- false
        let rc = sqlite3_step sBegin
        sqlite3_reset sBegin |> ignore
        if rc <> DONE then logErr "BEGIN" rc
        rc = DONE

/// Ends a pass that wrote nothing without ending its transaction (the next pass's begin' goes on with it)
let keep () = carried <- true

/// true when committed
let commit () =
    carried <- false
    endCursors ()
    if walBuf then Vfs.enter ()
    let rc = sqlite3_step_commit sCommit
    let f = if walBuf then Vfs.leave () else OK
    if rc <> DONE then logErr "COMMIT" rc
    elif f <> OK then logErr "WAL write" f
    sqlite3_reset sCommit |> ignore
    rc = DONE && f = OK

let rollback () =
    carried <- false
    endCursors ()
    if sqlite3_get_autocommit db = 0 then
        sqlite3_step_commit sRollback |> ignore
        sqlite3_reset sRollback |> ignore

let inTransaction () = sqlite3_get_autocommit db = 0

let cacheKB () =
    let mutable c = 0
    let mutable h = 0
    sqlite3_db_status (db, DBSTATUS_CACHE_USED, &c, &h, 0) |> ignore
    c / 1024

// ---------- opening ----------

/// The decoders read columns by position: they run only on the schema they assume.
let private schemaChecks =
    [| "SELECT count(*) FROM pragma_table_info('posts') WHERE (cid=0 AND name='id') OR (cid=1 AND name='user_id') OR (cid=2 AND name='body') OR (cid=3 AND name='created_at')", 4L
       "SELECT count(*) FROM pragma_table_info('users') WHERE (cid=0 AND name='id') OR (cid=1 AND name='username')", 2L
       "SELECT count(*) FROM pragma_index_xinfo('posts_created_at_id_idx') WHERE (seqno=0 AND name='created_at' AND desc=1) OR (seqno=1 AND name='id' AND desc=1)", 2L
       "SELECT count(*) FROM pragma_index_xinfo('likes_post_id_idx') WHERE seqno=0 AND name='post_id' AND desc=0", 1L |]

let private btInit () =
    let root name = uint32 (scalar ("SELECT rootpage FROM sqlite_schema WHERE name = '" + name + "'"))
    rootPosts <- root "posts"
    rootUsers <- root "users"
    rootTop <- root "posts_created_at_id_idx"
    rootLikes <- root "likes_post_id_idx"
    let schemaOk = schemaChecks |> Array.forall (fun (q, want) -> scalar q = want)
    // (text is UTF-8 by construction: built with SQLITE_OMIT_UTF16, SQLite opens no other encoding)
    if not schemaOk then eprintfn "B-tree reads: unexpected schema (roots %d %d %d %d)" rootPosts rootUsers rootTop rootLikes
    if rootPosts = 0u || rootUsers = 0u || rootTop = 0u || rootLikes = 0u || not schemaOk then false
    else
        cookie <- uint32 (scalar "PRAGMA schema_version")
        bt <- sqlite3DbNameToBtree (db, z "main")
        topKey <- sqlite3KeyInfoAlloc (db, 2, 1)
        likesKey <- sqlite3KeyInfoAlloc (db, 1, 1)
        let size = sqlite3BtreeCursorSize ()
        for i in 0 .. CCount - 1 do
            curs.[i] <- NativePtr.toNativeInt (NativePtr.ofVoidPtr<byte> (NativeMemory.AlignedAlloc(unativeint size, 64un)))
        searchMem <- NativePtr.toNativeInt (NativePtr.ofVoidPtr<byte> (NativeMemory.AllocZeroed(unativeint MemSize)))
        searchKey <- NativePtr.toNativeInt (NativePtr.ofVoidPtr<byte> (NativeMemory.AllocZeroed(unativeint UnpackedSize)))
        bt <> 0n && topKey <> 0n && likesKey <> 0n

/// The B-tree path gives byte for byte what SQL gives: checked at startup on the feed and on the posts it lists (and on
/// ids that don't exist). On any difference it stays off and every read uses SQL.
let private selfTest () =
    let a = Json.Out(64 * 1024)
    let b = Json.Out(64 * 1024)
    let same () = a.BodyLength = b.BodyLength && ReadOnlySpan(a.Wire, Json.Head, a.BodyLength).SequenceEqual(ReadOnlySpan(b.Wire, Json.Head, b.BodyLength))
    let mutable ok = begin' ()
    if ok then
        ok <- btFeed a && sqlFeedBody b && same ()
        let ids = [ yield! (let s = prep "SELECT id FROM posts ORDER BY created_at DESC, id DESC LIMIT 25" in [ while sqlite3_step s = ROW do yield sqlite3_column_int64 (s, 0) ]); yield 1L; yield 0L; yield Int64.MaxValue / 2L ]
        for id in ids do
            if ok then
                a.Clear()
                b.Clear()
                let r1 = btPost id a
                let r2 = sqlPostBody id b
                ok <- r1 = r2 && same ()
        rollback ()
    ok

/// Opens the database (SQLITE_PATH). cacheKB: SQLite's page cache limit.
let openDb (path: string) (cacheKB: int) (warm: bool) =
    let mutable h = 0n
    // PCACHE=0: pcache1 as it is
    let pc = Environment.GetEnvironmentVariable "PCACHE" <> "0" && Pcache.install ()
    if sqlite3_initialize () <> OK then failwith "sqlite3_initialize"
    if walBuf then
        if Vfs.register "fsu-excl" "unix-excl" then vfs <- z "fsu-excl"
        else eprintfn "WAL write gathering off: no unix-excl VFS"
    if sqlite3_open_v2 (z path, &h, OPEN_READWRITE ||| OPEN_NOMUTEX, vfs) <> OK then failwithf "open %s: %s" path (errmsg h)
    db <- h
    exec "PRAGMA journal_mode=WAL"
    exec "PRAGMA synchronous=NORMAL"
    exec "PRAGMA mmap_size=0" // reads through SQLite's page cache: a cached page keeps its parsed header
    exec "PRAGMA temp_store=MEMORY"
    // the WAL is truncated at the first commit after each rewind: later frames go to fresh file pages, not over old
    // ones the kernel may have to read back from disk first
    exec "PRAGMA journal_size_limit=0"
    exec ("PRAGMA cache_size=-" + string cacheKB)
    sqlite3_wal_hook (db, Marshal.GetFunctionPointerForDelegate walHook, 0n) |> ignore
    // CHUNK_MB: the database file grows this many MiB at a time (SQLITE_FCNTL_CHUNK_SIZE), so a checkpoint's writes past
    // its end don't each extend the file
    if chunkMB > 0 then
        let mutable bytes = chunkMB <<< 20
        sqlite3_file_control (db, z "main", 6, &bytes) |> ignore
    exec "SELECT count(*) FROM sqlite_schema"
    if pc then Pcache.enable (int (scalar "PRAGMA page_size"))
    sHealth <- prep "SELECT 1"
    sFeed <- prep sqlFeed
    sPost <- prep sqlPost
    // No RETURNING (it buffers its rows in an ephemeral table set up per insert): created_at is bound in the column
    // default's format, and the id comes from sqlite3_last_insert_rowid.
    sCreate <- prep "INSERT INTO posts (user_id, body, created_at) VALUES (?1, ?2, ?3)"
    sLike <- prep "INSERT INTO likes (user_id, post_id, created_at) SELECT ?1, ?2, ?3 WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2) ON CONFLICT DO NOTHING"
    sExists <- prep "SELECT 1 FROM posts WHERE id = ?1"
    sBegin <- prep "BEGIN"
    sCommit <- prep "COMMIT"
    sRollback <- prep "ROLLBACK"
    if warm then
        // Writes land on random leaves of these indexes and authors on random rows of users: read now, while memory is
        // plentiful, they sit in SQLite's page cache (anonymous memory the kernel keeps) instead of being read from
        // disk one by one later, when the kernel has dropped its file cache.
        sqlite3_exec (db, z "SELECT sum(user_id) FROM likes INDEXED BY sqlite_autoindex_likes_1", 0n, 0n, 0n) |> ignore
        sqlite3_exec (db, z "SELECT sum(user_id) FROM posts INDEXED BY posts_user_id_idx", 0n, 0n, 0n) |> ignore
        sqlite3_exec (db, z "SELECT sum(length(username)) FROM users", 0n, 0n, 0n) |> ignore
    useBt <- Environment.GetEnvironmentVariable "BTREE" <> "0" && btInit ()
    if useBt then
        useBt <- selfTest ()
        if not useBt then eprintfn "B-tree reads differ from SQL: SQL for every read"
    else eprintfn "B-tree reads off: SQL for every read"
    useBt

/// bytes per WAL frame: a 24-byte header and a page
let walFrameBytes () = 24 + int (scalar "PRAGMA page_size")

// ---------- the checkpoint thread ----------

/// A thread with its own connection runs PASSIVE checkpoints when the loop asks, so the copy and its two fsyncs never
/// stop the loop. It signals each finished pass on an eventfd that the loop reads through its ring.
[<Sealed>]
type Checkpointer(path: string, nice: int) =
    let efd = Native.eventfd (0u, 0x80000)
    let one = Marshal.AllocHGlobal 8
    let wake = new AutoResetEvent(false)
    let mutable rc = 0
    let mutable backfilled = -1
    let mutable wallUs = 0L
    let mutable cpuUs = 0L
    let frameBytes = walFrameBytes ()
    let run () =
        if nice <> 0 then Native.setpriority (0, Native.gettid (), nice) |> ignore
        let mutable cdb = 0n
        let mutable dropFd = -1
        let mutable walFd = -1
        let main = z "main"
        while true do
            wake.WaitOne() |> ignore
            if cdb = 0n then
                let mutable h = 0n
                if sqlite3_open_v2 (z path, &h, OPEN_READWRITE ||| OPEN_NOMUTEX, vfs) = OK then
                    cdb <- h
                    sqlite3_busy_timeout (cdb, 0) |> ignore
                    sqlite3_exec (cdb, z "PRAGMA synchronous=NORMAL", 0n, 0n, 0n) |> ignore
                    sqlite3_wal_autocheckpoint (cdb, 0) |> ignore
                    if chunkMB > 0 then
                        let mutable bytes = chunkMB <<< 20
                        sqlite3_file_control (cdb, main, 6, &bytes) |> ignore
                    // kept open: closing any descriptor of the file would drop the process's POSIX locks on it
                    dropFd <- Native.``open`` (z path, 0x80000) // O_RDONLY | O_CLOEXEC
            let mutable log = -1
            let mutable done' = -1
            let t0 = Diagnostics.Stopwatch.GetTimestamp()
            let c0 = Native.threadCpuUs ()
            let r = if cdb <> 0n then sqlite3_wal_checkpoint_v2 (cdb, main, CHECKPOINT_PASSIVE, &log, &done') else 14
            // SQLite syncs the database only at the end of a pass that copied every frame in the WAL, i.e. when no
            // commit added frames while it ran (writes held): then the WAL still ends where the pass stopped. Then every
            // page is clean and the server reads from its own page cache: the kernel's copy goes, so it never has to
            // reclaim it on the one CPU when memory gets short. After a pass that didn't catch up, the pages it wrote
            // stay dirty until that sync, which writes each page once however many passes wrote it (a drop here would
            // start writing them all back after every pass, and fail to drop the ones still being written).
            if dropFd >= 0 && r = 0 && done' > 0 && done' = log && walFrames = done' then
                Native.posix_fadvise (dropFd, 0L, 0L, 4) |> ignore // POSIX_FADV_DONTNEED
            // the WAL frames the pass copied are synced and never read again (readers start past them): out of the cache
            // too, so a WAL that grows while the disk is slow doesn't fill memory
            if walFd < 0 && cdb <> 0n then walFd <- Native.``open`` (z (path + "-wal"), 0x80000) // kept open, as above
            if walFd >= 0 && r = 0 && done' > 0 then
                Native.posix_fadvise (walFd, 0L, 32L + int64 done' * int64 frameBytes, 4) |> ignore
            Volatile.Write(&cpuUs, Native.threadCpuUs () - c0)
            Volatile.Write(&wallUs, int64 (Diagnostics.Stopwatch.GetElapsedTime(t0).TotalMicroseconds))
            Volatile.Write(&backfilled, done')
            Volatile.Write(&rc, r)
            Native.write (efd, one, 8un) |> ignore
    do
        NativePtr.write (NativePtr.ofNativeInt<uint64> one) 1UL
        let t = Thread(run, 256 * 1024, IsBackground = true, Name = "checkpoint")
        t.Start()
    member _.Fd = efd
    member _.Request() = wake.Set() |> ignore
    /// the last pass: SQLite's rc and the frames it backfilled (-1 unknown)
    member _.Rc = Volatile.Read &rc
    member _.Backfilled = Volatile.Read &backfilled
    /// the last pass's wall time and its thread's CPU time, in microseconds
    member _.WallUs = Volatile.Read &wallUs
    member _.CpuUs = Volatile.Read &cpuUs

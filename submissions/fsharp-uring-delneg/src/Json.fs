/// Response bodies, written by hand as bytes (compact JSON, SPEC.md's key order). Strings are escaped (`"`, `\` and
/// control characters; the scan for them is vectorized by SearchValues), non-ASCII is copied as is.
module Json

#nowarn "9"

open System
open System.Buffers
open System.Buffers.Text
open Microsoft.FSharp.NativeInterop

let private escapeSet = SearchValues.Create(ReadOnlySpan(Array.init 32 byte |> Array.append [| '"'B; '\\'B |]))
let private hex = "0123456789abcdef"B

/// Room for the status line and headers in front of the body: they are written backwards from here when the body
/// is done, so the response goes out as one contiguous send without a copy.
[<Literal>]
let Head = 128

/// A response buffer: pinned (io_uring sends straight from it), grown on demand. The body starts at Head.
[<Sealed; AllowNullLiteral>]
type Out(size: int) =
    let mutable wire = GC.AllocateUninitializedArray<byte>(size, pinned = true)
    let mutable n = Head
    member _.Wire = wire
    member _.N
        with get () = n
        and set v = n <- v
    /// a span of at least `size` free bytes at the end
    member _.Reserve(size: int) : Span<byte> =
        if wire.Length - n < size then
            let bigger = GC.AllocateUninitializedArray<byte>(max (wire.Length * 2) (n + size + 1024), pinned = true)
            Buffer.BlockCopy(wire, 0, bigger, 0, n)
            wire <- bigger
        Span<byte>(wire, n, wire.Length - n)
    member _.Advance(count: int) = n <- n + count
    member _.Clear() = n <- Head
    member _.BodyLength = n - Head
    /// Big buffers are not kept: back to the usual size when a rare large response is done.
    member _.Shrink(usual: int) =
        if wire.Length > 4 * usual then wire <- GC.AllocateUninitializedArray<byte>(usual, pinned = true)
        n <- Head

let inline put (o: Out) (s: ReadOnlySpan<byte>) =
    s.CopyTo(o.Reserve s.Length)
    o.Advance s.Length

let inline putByte (o: Out) (b: byte) =
    (o.Reserve 1).[0] <- b
    o.Advance 1

/// v at s[p..]; returns the new position
let inline intAt (s: Span<byte>) (p: int) (v: int64) =
    let mutable k = 0
    Utf8Formatter.TryFormat(v, s.Slice p, &k) |> ignore
    p + k

let putInt (o: Out) (v: int64) = o.Advance(intAt (o.Reserve 24) 0 v)

/// a JSON string with its quotes at s[p..] (room for 6 bytes per input byte + 2); returns the new position
let stringAt (s: Span<byte>) (p0: int) (v: ReadOnlySpan<byte>) =
    let mutable p = p0
    s.[p] <- '"'B
    p <- p + 1
    let mutable rest = v
    let mutable go = true
    while go do
        let i = rest.IndexOfAny escapeSet
        if i < 0 then
            rest.CopyTo(s.Slice p)
            p <- p + rest.Length
            go <- false
        else
            rest.Slice(0, i).CopyTo(s.Slice p)
            p <- p + i
            let b = rest.[i]
            s.[p] <- '\\'B
            match b with
            | 34uy -> s.[p + 1] <- '"'B; p <- p + 2
            | 92uy -> s.[p + 1] <- '\\'B; p <- p + 2
            | 10uy -> s.[p + 1] <- 'n'B; p <- p + 2
            | 13uy -> s.[p + 1] <- 'r'B; p <- p + 2
            | 9uy -> s.[p + 1] <- 't'B; p <- p + 2
            | 8uy -> s.[p + 1] <- 'b'B; p <- p + 2
            | 12uy -> s.[p + 1] <- 'f'B; p <- p + 2
            | _ ->
                s.[p + 1] <- 'u'B
                s.[p + 2] <- '0'B
                s.[p + 3] <- '0'B
                s.[p + 4] <- hex.[int (b >>> 4)]
                s.[p + 5] <- hex.[int (b &&& 15uy)]
                p <- p + 6
            rest <- rest.Slice(i + 1)
    s.[p] <- '"'B
    p + 1

/// a JSON string with its quotes
let putString (o: Out) (v: ReadOnlySpan<byte>) =
    o.Advance(stringAt (o.Reserve(v.Length * 6 + 2)) 0 v)

let private kId = "{\"id\":"B
let private kBody = ",\"body\":"B
let private kCreated = ",\"created_at\":\""B
let private kAuthor = "\",\"author\":"B
let private kLikes = ",\"like_count\":"B

let inline private lit (s: Span<byte>) (p: int) (k: byte[]) =
    ReadOnlySpan(k).CopyTo(s.Slice p)
    p + k.Length

/// {"id":…,"body":…,"created_at":…,"author":…,"like_count":…} in one reserved span. created_at is sent as stored
/// (SPEC.md: it is stored in exactly the response format), the strings escaped.
let putPost (o: Out) (id: int64) (body: ReadOnlySpan<byte>) (createdAt: ReadOnlySpan<byte>) (author: ReadOnlySpan<byte>) (likes: int64) =
    let s = o.Reserve((body.Length + author.Length) * 6 + createdAt.Length + 128)
    let mutable p = lit s 0 kId
    p <- intAt s p id
    p <- lit s p kBody
    p <- stringAt s p body
    p <- lit s p kCreated
    createdAt.CopyTo(s.Slice p)
    p <- p + createdAt.Length
    p <- lit s p kAuthor
    p <- stringAt s p author
    p <- lit s p kLikes
    p <- intAt s p likes
    s.[p] <- '}'B
    o.Advance(p + 1)

/// span over native memory (SQLite's page bytes)
let inline native (p: nativeint) (n: int) = ReadOnlySpan<byte>(NativePtr.toVoidPtr (NativePtr.ofNativeInt<byte> p), n)

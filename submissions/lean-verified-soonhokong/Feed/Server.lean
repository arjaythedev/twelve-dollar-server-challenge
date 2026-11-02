import Feed.Handler

/-!
# Batches

The transport (`runtime/uring.c`) calls `batch` once per event-loop iteration with the unread
input of every connection that has some. A batch is one SQLite transaction:

1. every complete request is parsed; reads are answered from the transaction's snapshot;
2. each connection stops at its first write (a later request on that connection waits for the
   next batch, so it sees the write), and the writes are then performed in order;
3. the transaction commits; a write's answer is its result if the commit succeeded, else 500.

No answer leaves before the batch returns, so a 201 is always committed.
-/
namespace Feed

/-- What the transport does with one connection's input. -/
structure Reply where
  /-- bytes to send: `resp[start, size)` -/
  resp : ByteArray
  start : Nat
  /-- input bytes used; the rest is kept for the next batch -/
  consumed : Nat
  /-- bit 0: close after sending; bit 1: requests are pending, present the rest again -/
  flags : Nat
  deriving Inhabited

/-- The read phase of one connection's input: its answers, the bytes used, the flags, and the
write that ended it, if any. -/
structure ChunkOut where
  out : ByteArray
  consumed : Nat
  flags : Nat
  write : Option (Write × Bool)
  deriving Inhabited

def maxPipeline : Nat := 64

/-- The requests of one connection's input from `pos`: answer reads until the input ends, a write
comes, or the connection closes. `n` answers so far, appended to `out`. -/
def readLoop [Monad m] [Store m] (env : Env) (a : ByteArray) (pos n : Nat) (out scratch : ByteArray) :
    Nat → m (ChunkOut × ByteArray)
  | 0 => return (⟨out, pos, if pos < a.size then 2 else 0, none⟩, scratch)
  | fuel + 1 => do
    match Http.parse a pos with
    | .incomplete => return (⟨out, pos, 0, none⟩, scratch)
    | .bad st =>
      let e := if st == 413 then Err.tooLarge else if st == 431 then Err.headTooLarge else Err.badRequest
      return (⟨respondErr out e true, a.size, 1, none⟩, scratch)
    | .ok r =>
      let st ← decide env a r out scratch
      match st.write with
      | some w =>
        -- a write is performed after this batch's reads; later requests wait for the next batch
        if n == 0 && env.writesOk then
          return (⟨st.out, r.next, if r.next < a.size then 2 else 0, some (w, r.close)⟩, st.scratch)
        else return (⟨st.out, pos, 2, none⟩, st.scratch)
      | none =>
        if r.close then return (⟨st.out, r.next, 1, none⟩, st.scratch)
        else readLoop env a r.next (n + 1) st.out st.scratch fuel

def readPhase [Monad m] [Store m] (env : Env) (a : ByteArray) (scratch : ByteArray) : m (ChunkOut × ByteArray) :=
  readLoop env a 0 0 ByteArray.empty scratch maxPipeline

/-- The read phase over all chunks, in order. -/
def readAll [Monad m] [Store m] (env : Env) (chunks : Array ByteArray) (i : Nat) (outs : Array ChunkOut)
    (scratch : ByteArray) : Nat → m (Array ChunkOut)
  | 0 => return outs
  | fuel + 1 => do
    if h : i < chunks.size then
      let (o, scratch) ← readPhase env chunks[i] scratch
      readAll env chunks (i + 1) (outs.push o) scratch fuel
    else return outs

/-- The write phase: perform the writes in chunk order; their results in that order. -/
def writeAll [Monad m] [Store m] (outs : Array ChunkOut) (i : Nat) (res : Array (Nat × ByteArray)) :
    Nat → m (Array (Nat × ByteArray))
  | 0 => return res
  | fuel + 1 => do
    if h : i < outs.size then
      match outs[i].write with
      | some (w, _) => writeAll outs (i + 1) (res.push (← perform w)) fuel
      | none => writeAll outs (i + 1) res fuel
    else return res

/-- Replies, with each write's answer appended to its connection's output. -/
def replies (committed : Bool) (outs : Array ChunkOut) (results : Array (Nat × ByteArray)) (i wi : Nat)
    (acc : Array Reply) : Nat → Array Reply
  | 0 => acc
  | fuel + 1 =>
    if h : i < outs.size then
      let o := outs[i]
      match o.write with
      | some (_, close) =>
        let (st, body) := results[wi]!
        let (st, body) := if committed || st == 404 then (st, body) else (500, Err.internal.body)
        replies committed outs results (i + 1) (wi + 1)
          (acc.push ⟨respond o.out st body close, 0, o.consumed, o.flags ||| (if close then 1 else 0)⟩) fuel
      | none => replies committed outs results (i + 1) wi (acc.push ⟨o.out, 0, o.consumed, o.flags⟩) fuel
    else acc

/-- One batch: reads from one snapshot, then the writes in order, then the commit. -/
def runBatch [Monad m] [Store m] (env : Env) (chunks : Array ByteArray) : m (Array Reply) := do
  let ok ← Store.begin
  if !ok then
    return chunks.map fun _ => ⟨respondErr ByteArray.empty .internal true, 0, 0, 1⟩
  let outs ← readAll env chunks 0 (Array.mkEmpty chunks.size) (ByteArray.emptyWithCapacity 8192) chunks.size
  Store.endReads
  let results ← writeAll outs 0 (Array.mkEmpty 0) outs.size
  let committed ← Store.commit
  return replies committed outs results 0 0 (Array.mkEmpty outs.size) outs.size

/-- The HMAC key schedule for `JWT_SECRET`, computed once at startup. -/
@[export feed_make_key]
def makeKey (secret : ByteArray) : Hmac.Key := Hmac.Key.of secret

@[export feed_batch]
def batch (chunks : Array ByteArray) (now uptime : UInt64) (writesOk : UInt8) (key : Hmac.Key) : IO (Array Reply) :=
  runBatch ⟨key, now.toNat, uptime.toNat, writesOk != 0⟩ chunks

end Feed

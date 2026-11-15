/-!
# The receive side of the transport

Each connection has at most one receive in the kernel at a time: normally a multishot receive,
which stays armed while the ring's provided-buffer ring has buffers. When the buffer ring runs dry
(a burst, or the process was not scheduled for a while) multishot receives end with `ENOBUFS`.
Re-arming every ended receive at once only repeats that: the first `nbuf` of them take every
buffer again and the rest end again, pass after pass. Re-arming a bounded number per pass is not
enough either: a re-armed multishot receive competes for the buffers with every receive still
armed, so under load it can end again and go back to the end of the queue while its request
waits in the socket, where it costs a 3-4 KB socket buffer charged to the process (on a 2 GB box,
TCP's memory limit is reached long before the CPU is full).

So an ended receive puts its connection on its ring's queue, and the queue is *drained*: each
pass reads queued connections with single-shot receives (`reading`, `MSG_DONTWAIT`), at most
`nbuf` per ring per round (`issueRound`), in rounds within the pass (the reads complete inside
the non-blocking enter and their buffers come back before the next round) up to `readsPerPass`
in all. A read that fills its buffer may have left more in the socket: the connection is queued
again. A shorter read, or `EAGAIN`, means the socket is empty: the multishot receive is armed
again (`arm`). A request then waits in user memory (its own size) instead of in the socket.

This file is the specification of that mechanism and the source of part of its C:

* `Recv`, `Ev`, `Act` and `step` — the per-connection state machine. The emitter writes `step`
  as the lookup tables the runtime executes (`gen/transport_gen.h`), so the C transitions are
  this table, not a hand copy of it. `issue` is the one other transition: a queued connection is
  re-armed.
* `issueRound` — the per-ring, per-pass re-arm policy (FIFO, at most `nbuf`). It is a model: the
  C loop `rearm_queued` is written by hand to follow it and is tested, not generated.
* `Sizing` — the transport's constants as a dependent record: the fields that make invalid
  sizes unrepresentable (`nbuf` is a power of two by construction) or carry proofs that the
  packed identifiers fit, checked by the kernel when `sizing` is built and erased from C.

Trust boundary. The theorems are about the table and `issue` (each property holds for every
single transition) and about `issueRound` as a function on lists. They do not prove anything
about the C program as a whole: that the runtime classifies kernel completions into `Ev`
correctly, drops completions of older generations, keeps exactly one queue entry per `queued`
connection and re-arms at most `nbuf` per ring per pass, are obligations of `runtime/uring.c`,
tested by tests/metal/transport_cases.c and the live suites (check153.sh). The budget bounds
re-arms, not buffers: receives still armed on other connections draw from the same buffer
ring, so a re-armed receive can end with `ENOBUFS` again (it is queued again).
-/
namespace Feed.Transport

/-- Receive state of an open connection. -/
inductive Recv where
  | idle     -- no receive in the kernel and not queued (only while closing)
  | multi    -- one multishot receive armed
  | queued   -- on its ring's queue, waiting to be read
  | reading  -- one single-shot read in the kernel (draining the socket)
  deriving DecidableEq, Repr, Inhabited

/-- A receive completion, classified by the runtime from the CQE and from which operation it
belongs to (the `user_data` op): a multishot receive's events and a read's are distinct, so a
completion can only drive the state of the operation that produced it. -/
inductive Ev where
  | more        -- multishot: bytes, and the receive stays armed (`IORING_CQE_F_MORE`)
  | ended       -- multishot: bytes, and the receive ended (no `F_MORE`)
  | noBufs      -- multishot: ended with `ENOBUFS` (the buffer ring was empty)
  | eof         -- multishot: 0 bytes or an error: the peer is done
  | full        -- read: it filled its buffer, the socket may hold more
  | drained     -- read: less than a buffer, or `EAGAIN`: the socket is empty
  | readNoBufs  -- read: `ENOBUFS` (receives armed on other connections took the buffers)
  | readEof     -- read: 0 bytes or an error: the peer is done
  deriving DecidableEq, Repr, Inhabited

/-- The events of a single-shot read. -/
def Ev.isRead : Ev → Bool
  | .full | .drained | .readNoBufs | .readEof => true
  | _ => false

/-- What the runtime does after a transition. -/
inductive Act where
  | keep       -- nothing
  | enqueue    -- the connection goes on its ring's queue
  | close      -- shutdown, then close
  | arm        -- arm the multishot receive (the read that completed has left the kernel)
  deriving DecidableEq, Repr, Inhabited

/-- The transition on a completion; `none` when the completion cannot belong to this state
(the runtime drops it, as it drops completions of an older generation). -/
def step : Recv → Ev → Option (Recv × Act)
  | .multi, .more   => some (.multi, .keep)
  | .multi, .ended  => some (.queued, .enqueue)
  | .multi, .noBufs => some (.queued, .enqueue)
  | .multi, .eof    => some (.idle, .close)
  | .reading, .full    => some (.queued, .enqueue)
  | .reading, .drained => some (.multi, .arm)
  | .reading, .readNoBufs => some (.queued, .enqueue)
  | .reading, .readEof    => some (.idle, .close)
  | _, _ => none

/-- Issuing: only a queued connection, which has no receive in the kernel, gets one: a read. -/
def issue : Recv → Option Recv
  | .queued => some .reading
  | _ => none

/-- Receives of this connection in the kernel. -/
def Recv.inKernel : Recv → Nat
  | .multi => 1
  | .reading => 1
  | _ => 0

/-! ## Properties of the transitions -/

/-- After a completion at most one receive is in the kernel. If one is, either it is the
multishot receive that just completed and stays armed, or the completion was a single-shot read
(which has left the kernel) that drained the socket and the multishot receive is armed in its
place: a completion never adds a second receive. -/
theorem one_receive (s s' : Recv) (e : Ev) (a : Act) (h : step s e = some (s', a)) :
    s'.inKernel ≤ 1 ∧ (s'.inKernel = 1 →
      (e = .more ∧ s' = s ∧ a = .keep) ∨ (s = .reading ∧ e = .drained ∧ a = .arm)) := by
  cases s <;> cases e <;> simp [step] at h <;> obtain ⟨rfl, rfl⟩ := h <;> simp [Recv.inKernel]

/-- The multishot receive is armed exactly when a read reports the socket empty. -/
theorem arm_iff (s s' : Recv) (e : Ev) (a : Act) (h : step s e = some (s', a)) :
    a = .arm ↔ (s = .reading ∧ e = .drained) := by
  cases s <;> cases e <;> simp [step] at h <;> obtain ⟨rfl, rfl⟩ := h <;> simp

/-- A read's completion only drives a reading connection, and a multishot receive's only an armed
one: a completion of the other operation (which cannot be in the kernel) is dropped. -/
theorem events_belong (s : Recv) (e : Ev) (hs : (step s e).isSome) :
    (e.isRead = true → s = .reading) ∧ (e.isRead = false → s = .multi) := by
  cases s <;> cases e <;> simp_all [step, Ev.isRead]

/-- Issuing adds a receive only to a connection that has none, so it never makes two. -/
theorem issue_one (s s' : Recv) (h : issue s = some s') : s.inKernel = 0 ∧ s'.inKernel = 1 := by
  cases s <;> simp [issue] at h <;> subst h <;> simp [Recv.inKernel]

/-- An open connection is never left without a way to receive: unless the peer is done, it
ends up armed or queued. (Losing that would leave the connection silent forever.) -/
theorem live (s s' : Recv) (e : Ev) (a : Act) (h : step s e = some (s', a)) (he : e ≠ .eof)
    (he' : e ≠ .readEof) : s' ≠ .idle := by
  cases s <;> cases e <;> simp [step] at h <;> obtain ⟨rfl, rfl⟩ := h <;> simp_all

/-- The queue holds a connection exactly when its state is `queued`: it is enqueued only on a
transition into `queued`, which is never taken from `queued` (no duplicates), and it leaves the
queue only by `issue`, out of `queued`. -/
theorem enqueue_iff (s s' : Recv) (e : Ev) (a : Act) (h : step s e = some (s', a)) :
    (a = .enqueue ↔ s' = .queued) ∧ s ≠ .queued := by
  cases s <;> cases e <;> simp [step] at h <;> obtain ⟨rfl, rfl⟩ := h <;> simp

/-- End of input always closes, whichever operation saw it. -/
theorem eof_closes (s s' : Recv) (e : Ev) (a : Act) (he : e = .eof ∨ e = .readEof)
    (h : step s e = some (s', a)) : a = .close ∧ s' = .idle := by
  rcases he with rfl | rfl <;> cases s <;> simp_all [step]

/-! ## The issue policy -/

/-- One round over a ring's queue: issue reads from the front while fewer than `nbuf` have been
issued in this round (the runtime starts each round at `r = 0`). Returns the count afterwards, the
re-armed connections and the rest of the queue. -/
def issueRound {α} (nbuf : Nat) : Nat → List α → Nat × List α × List α
  | r, [] => (r, [], [])
  | r, x :: xs =>
    if r < nbuf then
      let t := issueRound nbuf (r + 1) xs
      (t.1, x :: t.2.1, t.2.2)
    else (r, [], x :: xs)

theorem issueRound_bound {α} (nbuf : Nat) :
    ∀ (r : Nat) (q : List α), r ≤ nbuf → (issueRound nbuf r q).1 ≤ nbuf
  | r, [], h => by simp [issueRound, h]
  | r, x :: xs, h => by
    unfold issueRound
    split
    · simp only
      exact issueRound_bound nbuf (r + 1) xs (by omega)
    · simpa using h

/-- FIFO: the issued reads are a prefix of the queue and the rest keeps its order. -/
theorem issueRound_fifo {α} (nbuf : Nat) :
    ∀ (r : Nat) (q : List α), (issueRound nbuf r q).2.1 ++ (issueRound nbuf r q).2.2 = q
  | r, [] => by simp [issueRound]
  | r, x :: xs => by
    unfold issueRound
    split
    · simp [issueRound_fifo nbuf (r + 1) xs]
    · simp

/-- It issues as many as the reservation allows: all of the queue, or up to `nbuf`. -/
theorem issueRound_count {α} (nbuf : Nat) :
    ∀ (r : Nat) (q : List α), r ≤ nbuf →
      (issueRound nbuf r q).2.1.length = min q.length (nbuf - r) ∧
      (issueRound nbuf r q).1 = r + (issueRound nbuf r q).2.1.length
  | r, [], _ => by simp [issueRound]
  | r, x :: xs, h => by
    unfold issueRound
    split
    · obtain ⟨h1, h2⟩ := issueRound_count nbuf (r + 1) xs (by omega)
      simp only [List.length_cons] at h1 h2 ⊢
      refine ⟨?_, ?_⟩ <;> omega
    · simp; omega

/-! ## Sizing -/

/-- The transport's constants. `nbuf` is `2 ^ nbufLog`, so the buffer-ring mask `nbuf - 1` is
valid by construction; the remaining fields are proofs about the packed identifiers the
runtime uses, checked when `sizing` is elaborated and erased from the generated header:

* `user_data` = op (8 bits) | ring (8) | generation (16) | slot (32);
* a connection id = ring << 16 | slot, so a slot must fit 16 bits;
* buffer ids are 16 bits and a provided-buffer ring has at most 32,768 entries;
* `jobBudget`, the requests answered in one pass (the rest wait on their connection, oldest first,
  for the next pass), is positive — every pass makes progress — and below `maxJobs`, the size of
  the per-pass job array.
`cqeBudget` (completions handled per ring per pass) is a plain constant. `readsPerPass` bounds the
reads of a pass across its rounds; `reads_round` makes it at least one full round, so every pass
drains at least `nbuf` queued connections of a ring that has them. -/
structure Sizing where
  maxRings : Nat
  slotsMax : Nat
  nbufLog : Nat
  bufSize : Nat
  cqeBudget : Nat
  maxJobs : Nat
  readsPerPass : Nat
  jobBudget : Nat
  rings_fit : maxRings ≤ 256
  slots_fit : slotsMax < 65536
  nbuf_fit : 2 ^ nbufLog ≤ 32768
  reads_round : 2 ^ nbufLog ≤ readsPerPass
  jobs_pos : 0 < jobBudget
  jobs_fit : jobBudget < maxJobs

def Sizing.nbuf (z : Sizing) : Nat := 2 ^ z.nbufLog

def sizing : Sizing where
  maxRings := 16
  slotsMax := 65535
  nbufLog := 10
  bufSize := 2048
  cqeBudget := 2048
  maxJobs := 65536
  readsPerPass := 16384
  jobBudget := 2048
  rings_fit := by decide
  slots_fit := by decide
  nbuf_fit := by decide
  reads_round := by decide
  jobs_pos := by decide
  jobs_fit := by decide

/-- A connection id packs (ring, slot) into 32 bits without overlap. -/
def connId (ri slot : Nat) : Nat := ri * 65536 + slot

theorem connId_inj (z : Sizing) (a b c d : Nat) (hb : b ≤ z.slotsMax) (hd : d ≤ z.slotsMax)
    (h : connId a b = connId c d) : a = c ∧ b = d := by
  have := z.slots_fit
  unfold connId at h
  omega

/-! ## Enumerations for the emitter -/

def Recv.all : List Recv := [.idle, .multi, .queued, .reading]
def Ev.all : List Ev := [.more, .ended, .noBufs, .eof, .full, .drained, .readNoBufs, .readEof]
def Act.all : List Act := [.keep, .enqueue, .close, .arm]

theorem Recv.mem_all (s : Recv) : s ∈ Recv.all := by cases s <;> decide
theorem Ev.mem_all (e : Ev) : e ∈ Ev.all := by cases e <;> decide
theorem Act.mem_all (a : Act) : a ∈ Act.all := by cases a <;> decide

/-- C names, in the order of `all` (that order is the C enum's numbering). -/
def Recv.cname : Recv → String
  | .idle => "R_IDLE" | .multi => "R_MULTI" | .queued => "R_QUEUED" | .reading => "R_READING"
def Ev.cname : Ev → String
  | .more => "EV_MORE" | .ended => "EV_ENDED" | .noBufs => "EV_NOBUFS" | .eof => "EV_EOF"
  | .full => "EV_FULL" | .drained => "EV_DRAINED" | .readNoBufs => "EV_READ_NOBUFS" | .readEof => "EV_READ_EOF"
def Act.cname : Act → String
  | .keep => "A_KEEP" | .enqueue => "A_ENQUEUE" | .close => "A_CLOSE" | .arm => "A_ARM"

end Feed.Transport

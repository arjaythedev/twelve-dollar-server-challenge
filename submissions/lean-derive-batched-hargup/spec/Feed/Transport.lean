/-!
# The receive side of the transport

Each connection has at most one receive in the kernel at a time: a multishot receive, which
stays armed while the ring's provided-buffer ring has buffers. When the buffer ring runs dry (a
burst, or the process was not scheduled for a while) multishot receives end with `ENOBUFS`.
Re-arming every ended receive at once only repeats that: the first `nbuf` of them take every
buffer again and the rest end again, pass after pass, which costs work quadratic in the
backlog exactly when the server is already behind. Instead an ended receive puts its
connection on its ring's queue, and each pass re-arms at most `nbuf` queued connections per
ring, oldest first (`issueRound`): the total work of a recovery is linear in the backlog, and a
re-armed receive reads the waiting request and stays armed (one operation per request).

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
  | queued   -- on its ring's queue, waiting to be re-armed
  deriving DecidableEq, Repr, Inhabited

/-- A receive completion, classified by the runtime from the CQE. -/
inductive Ev where
  | more        -- bytes, and the receive stays armed (`IORING_CQE_F_MORE`)
  | ended       -- bytes, and the receive ended (no `F_MORE`)
  | noBufs      -- ended with `ENOBUFS` (the buffer ring was empty)
  | eof         -- 0 bytes or an error: the peer is done
  deriving DecidableEq, Repr, Inhabited

/-- What the runtime does after a transition. -/
inductive Act where
  | keep       -- nothing
  | enqueue    -- the connection goes on its ring's queue
  | close      -- shutdown, then close
  deriving DecidableEq, Repr, Inhabited

/-- The transition on a completion; `none` when the completion cannot belong to this state
(the runtime drops it, as it drops completions of an older generation). -/
def step : Recv → Ev → Option (Recv × Act)
  | .multi, .more   => some (.multi, .keep)
  | .multi, .ended  => some (.queued, .enqueue)
  | .multi, .noBufs => some (.queued, .enqueue)
  | .multi, .eof    => some (.idle, .close)
  | _, _ => none

/-- Re-arming: only a queued connection, which has no receive in the kernel, gets one. -/
def issue : Recv → Option Recv
  | .queued => some .multi
  | _ => none

/-- Receives of this connection in the kernel. -/
def Recv.inKernel : Recv → Nat
  | .multi => 1
  | _ => 0

/-! ## Properties of the transitions -/

/-- After a completion at most one receive is in the kernel, and if one is, it is the one that
just completed and stays armed: completions never add a receive. -/
theorem one_receive (s s' : Recv) (e : Ev) (a : Act) (h : step s e = some (s', a)) :
    s'.inKernel ≤ 1 ∧ (s'.inKernel = 1 → e = .more ∧ s' = s ∧ a = .keep) := by
  cases s <;> cases e <;> simp [step] at h <;> obtain ⟨rfl, rfl⟩ := h <;> simp [Recv.inKernel]

/-- Re-arming adds a receive only to a connection that has none, so it never makes two. -/
theorem issue_one (s s' : Recv) (h : issue s = some s') : s.inKernel = 0 ∧ s'.inKernel = 1 := by
  cases s <;> simp [issue] at h <;> subst h <;> simp [Recv.inKernel]

/-- An open connection is never left without a way to receive: unless the peer is done, it
ends up armed or queued. (Losing that would leave the connection silent forever.) -/
theorem live (s s' : Recv) (e : Ev) (a : Act) (h : step s e = some (s', a)) (he : e ≠ .eof) :
    s' ≠ .idle := by
  cases s <;> cases e <;> simp [step] at h <;> obtain ⟨rfl, rfl⟩ := h <;> simp_all

/-- The queue holds a connection exactly when its state is `queued`: it is enqueued only on a
transition into `queued`, which is never taken from `queued` (no duplicates), and it leaves the
queue only by `issue`, out of `queued`. -/
theorem enqueue_iff (s s' : Recv) (e : Ev) (a : Act) (h : step s e = some (s', a)) :
    (a = .enqueue ↔ s' = .queued) ∧ s ≠ .queued := by
  cases s <;> cases e <;> simp [step] at h <;> obtain ⟨rfl, rfl⟩ := h <;> simp

/-- End of input always closes. -/
theorem eof_closes (s s' : Recv) (a : Act) (h : step s .eof = some (s', a)) : a = .close ∧ s' = .idle := by
  cases s <;> simp_all [step]

/-! ## The issue policy -/

/-- One pass over a ring's queue: re-arm from the front while fewer than `nbuf` have been re-armed
in this pass (the runtime starts each pass at `r = 0`). Returns the count afterwards, the
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
* buffer ids are 16 bits and a provided-buffer ring has at most 32,768 entries.
`maxJobs` (requests decoded in one pass) and `cqeBudget` (completions handled per ring per pass)
are plain constants: the runtime enforces them by checks, not by these proofs. -/
structure Sizing where
  maxRings : Nat
  slotsMax : Nat
  nbufLog : Nat
  bufSize : Nat
  cqeBudget : Nat
  maxJobs : Nat
  rings_fit : maxRings ≤ 256
  slots_fit : slotsMax < 65536
  nbuf_fit : 2 ^ nbufLog ≤ 32768

def Sizing.nbuf (z : Sizing) : Nat := 2 ^ z.nbufLog

def sizing : Sizing where
  maxRings := 16
  slotsMax := 65535
  nbufLog := 10
  bufSize := 2048
  cqeBudget := 2048
  maxJobs := 65536
  rings_fit := by decide
  slots_fit := by decide
  nbuf_fit := by decide

/-- A connection id packs (ring, slot) into 32 bits without overlap. -/
def connId (ri slot : Nat) : Nat := ri * 65536 + slot

theorem connId_inj (z : Sizing) (a b c d : Nat) (hb : b ≤ z.slotsMax) (hd : d ≤ z.slotsMax)
    (h : connId a b = connId c d) : a = c ∧ b = d := by
  have := z.slots_fit
  unfold connId at h
  omega

/-! ## Enumerations for the emitter -/

def Recv.all : List Recv := [.idle, .multi, .queued]
def Ev.all : List Ev := [.more, .ended, .noBufs, .eof]
def Act.all : List Act := [.keep, .enqueue, .close]

theorem Recv.mem_all (s : Recv) : s ∈ Recv.all := by cases s <;> decide
theorem Ev.mem_all (e : Ev) : e ∈ Ev.all := by cases e <;> decide
theorem Act.mem_all (a : Act) : a ∈ Act.all := by cases a <;> decide

/-- C names, in the order of `all` (that order is the C enum's numbering). -/
def Recv.cname : Recv → String
  | .idle => "R_IDLE" | .multi => "R_MULTI" | .queued => "R_QUEUED"
def Ev.cname : Ev → String
  | .more => "EV_MORE" | .ended => "EV_ENDED" | .noBufs => "EV_NOBUFS" | .eof => "EV_EOF"
def Act.cname : Act → String
  | .keep => "A_KEEP" | .enqueue => "A_ENQUEUE" | .close => "A_CLOSE"

end Feed.Transport

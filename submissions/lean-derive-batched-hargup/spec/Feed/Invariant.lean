import Feed.Model

/-!
# The global invariant

Three facts about the stored data, none of which the schema enforces, all of which hold for
the seed and are preserved by every write this API can perform:

* `dense`   — post ids are exactly `1, 2, …, n` in rowid order. Posts are only ever appended
              (no endpoint deletes), and SQLite gives a new row `max(rowid) + 1`.
* `mono`    — `created_at` never decreases along rowid order. New posts get `now`.
* `authors` — every post's author row exists. Users are never written.

`dense` and `mono` together make the feed order (`created_at DESC, id DESC`) the reverse of
rowid order (`Feed.Plan.feed_eq_tail`), which is what lets the generated feed plan read the
last 20 rows of the posts B-tree instead of walking an index and seeking each row.

The server establishes the invariant at startup with `checkInv` (one scan, `checkInv_sound`)
and re-establishes it after each insert with `insertOk` (`inv_addPost`); it is the only writer
(exclusive locking). If a check ever fails, the generated code stops using the plans that
depend on it.
-/
namespace Feed.Db

structure Inv (db : Db) : Prop where
  dense : ∀ (i : Nat) (h : i < db.posts.length), db.posts[i].id = i + 1
  mono : db.posts.Pairwise (fun a b => a.createdAt ≤ b.createdAt)
  authors : ∀ p ∈ db.posts, (db.user? p.author).isSome

/-! ## Establishing it: the startup scan -/

/-- One pass over the posts in rowid order, as the generated `app_check_inv` does it. -/
def checkFrom (db : Db) (expect : Nat) (prev : Option String) : List Post → Bool
  | [] => true
  | p :: ps =>
    p.id == expect &&
    (match prev with | none => true | some c => decide (c ≤ p.createdAt)) &&
    (db.user? p.author).isSome &&
    checkFrom db (expect + 1) (some p.createdAt) ps

def checkInv (db : Db) : Bool := db.checkFrom 1 none db.posts

theorem checkFrom_sound (db : Db) :
    ∀ (l : List Post) (expect : Nat) (prev : Option String), db.checkFrom expect prev l = true →
      (∀ (i : Nat) (h : i < l.length), l[i].id = expect + i) ∧
      l.Pairwise (fun a b => a.createdAt ≤ b.createdAt) ∧
      (∀ c, prev = some c → ∀ p ∈ l, c ≤ p.createdAt) ∧
      (∀ p ∈ l, (db.user? p.author).isSome)
  | [], _, _, _ => by simp
  | p :: ps, expect, prev, h => by
    simp only [checkFrom, Bool.and_eq_true, beq_iff_eq] at h
    obtain ⟨⟨⟨hid, hprev⟩, hauth⟩, hrest⟩ := h
    obtain ⟨ids, pw, lower, auth⟩ := checkFrom_sound db ps (expect + 1) (some p.createdAt) hrest
    have below : ∀ q ∈ ps, p.createdAt ≤ q.createdAt := lower p.createdAt rfl
    refine ⟨?_, ?_, ?_, ?_⟩
    · intro i hi
      cases i with
      | zero => simpa using hid
      | succ i =>
        have := ids i (by simp at hi; omega)
        simp [this]; omega
    · exact List.Pairwise.cons below pw
    · intro c hc q hq
      subst hc
      simp only [decide_eq_true_eq] at hprev
      simp only [List.mem_cons] at hq
      rcases hq with rfl | hq
      · exact hprev
      · exact String.le_trans hprev (below q hq)
    · intro q hq
      simp only [List.mem_cons] at hq
      rcases hq with rfl | hq
      · exact hauth
      · exact auth q hq

theorem checkInv_sound (db : Db) (h : db.checkInv = true) : db.Inv := by
  obtain ⟨ids, pw, _, auth⟩ := checkFrom_sound db db.posts 1 none h
  exact ⟨fun i hi => by rw [ids i hi]; omega, pw, auth⟩

/-! ## Preserving it -/

/-- The check after an insert: the new row got the next id, is not older than the previous
newest row, and its author exists. -/
def insertOk (db : Db) (p : Post) : Bool :=
  p.id == db.posts.length + 1 &&
  (match db.posts.getLast? with | none => true | some q => decide (q.createdAt ≤ p.createdAt)) &&
  (db.user? p.author).isSome

theorem user?_addPost (db : Db) (p : Post) (uid : Nat) : (db.addPost p).user? uid = db.user? uid := rfl

theorem inv_addPost (db : Db) (p : Post) (hi : db.Inv) (hok : db.insertOk p = true) :
    (db.addPost p).Inv := by
  simp only [insertOk, Bool.and_eq_true, beq_iff_eq] at hok
  obtain ⟨⟨hid, hlast⟩, hauth⟩ := hok
  -- every existing post is no newer than p
  have older : ∀ q ∈ db.posts, q.createdAt ≤ p.createdAt := by
    intro q hq
    cases hl : db.posts.getLast? with
    | none => simp_all
    | some r =>
      rw [hl] at hlast
      simp only [decide_eq_true_eq] at hlast
      have hr : r ∈ db.posts := List.mem_of_getLast? hl
      have hrq : q = r ∨ q.createdAt ≤ r.createdAt := by
        obtain ⟨i, hi', rfl⟩ := List.getElem_of_mem hq
        have hlen : db.posts.length - 1 < db.posts.length := by omega
        have hlast' : db.posts[db.posts.length - 1] = r := by
          rw [List.getLast?_eq_getElem?] at hl
          exact (List.getElem_of_getElem? hl).2
        by_cases hij : i = db.posts.length - 1
        · left; subst hij; exact hlast'
        · right
          rw [← hlast']
          exact List.pairwise_iff_getElem.mp hi.mono i (db.posts.length - 1) hi' hlen (by omega)
      rcases hrq with rfl | hrq
      · exact hlast
      · exact String.le_trans hrq hlast
  refine ⟨?_, ?_, ?_⟩
  · intro i h
    simp only [addPost, List.length_append, List.length_singleton] at h
    simp only [addPost]
    by_cases hlt : i < db.posts.length
    · rw [List.getElem_append_left hlt]; exact hi.dense i hlt
    · have : i = db.posts.length := by omega
      subst this
      simp [hid]
  · simp only [addPost]
    exact List.pairwise_append.mpr ⟨hi.mono, List.pairwise_singleton _ _, fun a ha b hb => by
      simp at hb; subst hb; exact older a ha⟩
  · intro q hq
    simp only [addPost, List.mem_append, List.mem_singleton] at hq
    rw [user?_addPost]
    rcases hq with hq | rfl
    · exact hi.authors q hq
    · exact hauth

/-- Likes never touch posts or users, so they preserve the invariant. -/
theorem inv_like (db db' : Db) (uid pid : Nat) (b : Bool) (hi : db.Inv)
    (h : db.like uid pid = some (db', b)) : db'.Inv := by
  unfold like at h
  split at h
  · simp at h
  · split at h
    · simp at h; obtain ⟨rfl, _⟩ := h; exact hi
    · simp at h; obtain ⟨rfl, _⟩ := h
      exact ⟨hi.dense, hi.mono, fun p hp => hi.authors p hp⟩

end Feed.Db

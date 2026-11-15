import Feed.Invariant

/-!
# Read plans

What the generated C does for the two read endpoints, written over the model with the
B-tree primitives it uses, and the theorems that make those plans correct:

| primitive (model)      | generated C                                  |
|------------------------|----------------------------------------------|
| `tailRows db n`        | `cur_last` on `posts`, then `cur_prev`       |
| `Db.post? db id`       | `cur_seek` on `posts` (table B-tree by rowid)|
| `Db.user? db uid`      | `cur_seek` on `users`                        |
| `rangeCounts db lo hi` | one `idx_seek_ge` + `cur_next` scan of `likes_post_id_idx` |

`feedPlan_eq` says the feed plan is the feed, given the invariant. Without the invariant the
generated code uses SQL (`ORDER BY created_at DESC, id DESC`), which is the definition.
-/
namespace Feed

/-! ## The feed order on an invariant database is reverse rowid order -/

theorem str_lt_of_le_of_ne {a b : String} (h : a ≤ b) (e : a ≠ b) : a < b := by
  apply Classical.byContradiction
  intro hn
  exact e (String.le_antisymm h (String.not_lt.mp hn))

theorem filterMap_congr' {α β} {f g : α → Option β} :
    ∀ (l : List α), (∀ x ∈ l, f x = g x) → l.filterMap f = l.filterMap g
  | [], _ => rfl
  | x :: xs, h => by
    simp only [List.filterMap_cons, h x List.mem_cons_self]
    rw [filterMap_congr' xs (fun y hy => h y (List.mem_cons_of_mem _ hy))]

theorem newerEq_iff (a b : Post) :
    newerEq a b = true ↔ b.createdAt < a.createdAt ∨ (b.createdAt = a.createdAt ∧ b.id ≤ a.id) := by
  simp [newerEq]

theorem newerEq_total (a b : Post) : (newerEq a b || newerEq b a) = true := by
  simp only [Bool.or_eq_true, newerEq_iff]
  rcases String.le_total a.createdAt b.createdAt with h | h
  · by_cases e : a.createdAt = b.createdAt
    · rcases Nat.le_total a.id b.id with h' | h'
      · exact Or.inr (Or.inr ⟨e, h'⟩)
      · exact Or.inl (Or.inr ⟨e.symm, h'⟩)
    · right; left
      exact str_lt_of_le_of_ne h e
  · by_cases e : a.createdAt = b.createdAt
    · rcases Nat.le_total a.id b.id with h' | h'
      · exact Or.inr (Or.inr ⟨e, h'⟩)
      · exact Or.inl (Or.inr ⟨e.symm, h'⟩)
    · left; left
      exact str_lt_of_le_of_ne h (Ne.symm e)

theorem newerEq_trans (a b c : Post) :
    newerEq a b = true → newerEq b c = true → newerEq a c = true := by
  simp only [newerEq_iff]
  rintro (h1 | ⟨h1, i1⟩) (h2 | ⟨h2, i2⟩)
  · exact Or.inl (String.lt_trans h2 h1)
  · rw [h2]; exact Or.inl h1
  · rw [← h1]; exact Or.inl h2
  · exact Or.inr ⟨h2.trans h1, Nat.le_trans i2 i1⟩

namespace Db

/-- Under the invariant, rowid order is strictly increasing in (created_at, id). -/
theorem pairwise_rowid (db : Db) (hi : db.Inv) :
    db.posts.Pairwise (fun a b => newerEq b a = true) := by
  rw [List.pairwise_iff_getElem]
  intro i j hi' hj hij
  have hm := List.pairwise_iff_getElem.mp hi.mono i j hi' hj hij
  have di := hi.dense i hi'
  have dj := hi.dense j hj
  rw [newerEq_iff]
  by_cases e : db.posts[i].createdAt = db.posts[j].createdAt
  · exact Or.inr ⟨e, by omega⟩
  · exact Or.inl (str_lt_of_le_of_ne hm e)

theorem mem_eq_of_id (db : Db) (hi : db.Inv) (a b : Post) (ha : a ∈ db.posts) (hb : b ∈ db.posts)
    (h : a.id = b.id) : a = b := by
  obtain ⟨i, hi', rfl⟩ := List.getElem_of_mem ha
  obtain ⟨j, hj, rfl⟩ := List.getElem_of_mem hb
  have := hi.dense i hi'
  have := hi.dense j hj
  have : i = j := by omega
  subst this
  rfl

/-- The sort in `feed` returns the posts in reverse rowid order. -/
theorem sort_eq_reverse (db : Db) (hi : db.Inv) :
    db.posts.mergeSort newerEq = db.posts.reverse := by
  apply List.Perm.eq_of_pairwise (le := fun a b => newerEq a b = true)
  · intro a b ha hb hab hba
    simp only [List.mem_mergeSort] at ha
    simp only [List.mem_reverse] at hb
    rw [newerEq_iff] at hab hba
    apply db.mem_eq_of_id hi a b ha hb
    rcases hab with h | ⟨h, i⟩ <;> rcases hba with h' | ⟨h', i'⟩
    · exact absurd h' (String.lt_asymm h)
    · rw [h'] at h; exact absurd h (String.lt_irrefl _)
    · rw [h] at h'; exact absurd h' (String.lt_irrefl _)
    · omega
  · exact List.pairwise_mergeSort newerEq_trans newerEq_total _
  · exact List.pairwise_reverse.mpr (db.pairwise_rowid hi)
  · exact (List.mergeSort_perm _ _).trans (List.reverse_perm _).symm

theorem take_filterMap {α β} (f : α → Option β) :
    ∀ (l : List α) (n : Nat), (∀ x ∈ l, (f x).isSome) → (l.filterMap f).take n = (l.take n).filterMap f
  | [], _, _ => by simp
  | x :: xs, 0, _ => by simp
  | x :: xs, n + 1, h => by
    obtain ⟨y, hy⟩ := Option.isSome_iff_exists.mp (h x (List.mem_cons_self))
    simp only [List.filterMap_cons, hy, List.take_succ_cons]
    rw [take_filterMap f xs n (fun z hz => h z (List.mem_cons_of_mem _ hz))]

/-- **The feed is the last 20 rows by rowid, newest first** (given the invariant). -/
theorem feed_eq_tail (db : Db) (hi : db.Inv) :
    db.feed = (db.posts.reverse.take 20).filterMap db.view := by
  unfold feed
  rw [db.sort_eq_reverse hi]
  apply take_filterMap
  intro p hp
  simp only [view, Option.isSome_map]
  exact hi.authors p (List.mem_reverse.mp hp)

end Db

/-! ## Plans -/

namespace Db

/-- The last `n` rows of the posts B-tree, from the last one backwards. -/
def tailRows (db : Db) (n : Nat) : List Post := db.posts.reverse.take n

/-- Like counts from one ordered scan of `likes_post_id_idx` over keys `lo..hi`: the scan
visits exactly the likes whose post id is in range, and each is counted for its post. -/
def rangeCounts (db : Db) (lo hi pid : Nat) : Nat :=
  (db.likes.filter fun l => decide (lo ≤ l.post) && decide (l.post ≤ hi)).countP (·.post == pid)

theorem rangeCounts_eq (db : Db) (lo hi pid : Nat) (h1 : lo ≤ pid) (h2 : pid ≤ hi) :
    db.rangeCounts lo hi pid = db.likeCount pid := by
  unfold rangeCounts likeCount
  rw [List.countP_filter]
  congr 1
  funext l
  by_cases e : l.post = pid
  · subst e; simp [h1, h2]
  · simp [e]

/-- The window the generated code scans: the newest rowid and the 19 below it. -/
def window (hi : Nat) : Nat := if hi > 20 then hi - 19 else 1

/-- `GET /feed` as generated: read the last rowid `hi`, count likes for `window hi .. hi` in
one index scan, then walk 20 rows back emitting each post with its author. -/
def feedPlan (db : Db) : List PostView :=
  match db.posts.getLast? with
  | none => []
  | some newest =>
    let hi := newest.id
    let lo := window hi
    (db.tailRows 20).filterMap fun p =>
      (db.user? p.author).map fun u => ⟨p.id, p.body, p.createdAt, u.username, db.rangeCounts lo hi p.id⟩

/-- Under the invariant every row of the tail lies in the scanned window. -/
theorem tail_in_window (db : Db) (hi : db.Inv) (newest : Post) (hn : db.posts.getLast? = some newest) :
    ∀ p ∈ db.tailRows 20, window newest.id ≤ p.id ∧ p.id ≤ newest.id := by
  intro p hp
  have hlen : 0 < db.posts.length := by
    cases h : db.posts with
    | nil => simp [h] at hn
    | cons _ _ => simp
  have hnewest : newest.id = db.posts.length := by
    rw [List.getLast?_eq_getElem?] at hn
    obtain ⟨hb, he⟩ := List.getElem_of_getElem? hn
    rw [← he, hi.dense _ hb]
    omega
  obtain ⟨j, hj, rfl⟩ := List.getElem_of_mem hp
  simp only [tailRows, List.getElem_take, List.getElem_reverse]
  simp only [tailRows, List.length_take, List.length_reverse] at hj
  have hk : db.posts.length - 1 - j < db.posts.length := by omega
  rw [hi.dense _ hk, hnewest]
  unfold window
  split <;> omega

/-- **The generated feed plan computes the feed.** -/
theorem feedPlan_eq (db : Db) (hi : db.Inv) : db.feedPlan = db.feed := by
  rw [db.feed_eq_tail hi]
  unfold feedPlan
  cases hn : db.posts.getLast? with
  | none =>
    have : db.posts = [] := List.getLast?_eq_none_iff.mp hn
    simp [this]
  | some newest =>
    simp only
    show List.filterMap _ (db.tailRows 20) = List.filterMap _ (db.tailRows 20)
    apply filterMap_congr'
    intro p hp
    obtain ⟨h1, h2⟩ := db.tail_in_window hi newest hn p hp
    simp only [view, db.rangeCounts_eq _ _ _ h1 h2]

/-- `GET /posts/:id` as generated: seek the post, seek its author, count its likes with the
same index scan over the single key `id`. -/
def postPlan (db : Db) (pid : Nat) : Option PostView :=
  (db.post? pid).bind fun p =>
    (db.user? p.author).map fun u => ⟨p.id, p.body, p.createdAt, u.username, db.rangeCounts pid pid p.id⟩

/-- **The generated post plan computes `GET /posts/:id`** (no invariant needed). -/
theorem postPlan_eq (db : Db) (pid : Nat) : db.postPlan pid = db.getPost pid := by
  unfold postPlan getPost
  cases h : db.post? pid with
  | none => rfl
  | some p =>
    have hid : p.id = pid := by
      unfold post? at h
      have := List.find?_some h
      simpa using this
    simp only [Option.bind_some, view]
    rw [db.rangeCounts_eq pid pid p.id (by omega) (by omega)]

end Db
end Feed

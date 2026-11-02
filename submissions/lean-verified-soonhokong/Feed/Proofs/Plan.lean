import Feed.Proofs.Page

/-!
# The read plans

The feed and post plans of `Feed/Handler.lean`, run on the model, compute the specification.
-/
namespace Feed
open Spec

@[simp] theorem id_bind {α β : Type} (x : _root_.Id α) (f : α → _root_.Id β) : x >>= f = f x := rfl
@[simp] theorem id_pure {α : Type} (a : α) : (pure a : _root_.Id α) = a := rfl
@[simp] theorem id_map {α β : Type} (f : α → β) (x : _root_.Id α) : (f <$> x : _root_.Id β) = f x := rfl

/-! ## What each storage read returns in the model -/

@[simp] theorem run_feedPage (n : Nat) (db : DB) :
    (Store.feedPage (m := Model) n).run db = (mkPage ((db.sorted.take n).map db.entry), db) := rfl
@[simp] theorem run_postPage (id : Nat) (db : DB) :
    (Store.postPage (m := Model) id).run db = (mkPage ((db.post? id).toList.map db.entry), db) := rfl

/-! ## Rows by id -/

theorem DB.post?_mem {db : DB} {pid : Nat} {p : Post} (h : db.post? pid = some p) : p ∈ db.posts ∧ p.id = pid := by
  unfold DB.post? at h
  exact ⟨List.mem_of_find?_eq_some h, by simpa using List.find?_some h⟩

theorem DB.mem_sorted {db : DB} {p : Post} : p ∈ db.sorted ↔ p ∈ db.posts := by
  simp [DB.sorted, List.mem_mergeSort]

/-! ## Rendering rows -/

/-- Items joined by commas, the first one preceded by a comma unless `n = 0`. -/
def joinFrom : Nat → List (List UInt8) → List UInt8
  | _, [] => []
  | n, x :: xs => (if n = 0 then [] else [0x2c]) ++ x ++ joinFrom (n + 1) xs

theorem joinFrom_succ (n : Nat) (xs : List (List UInt8)) :
    joinFrom (n + 1) xs = joinFrom 1 xs := by
  induction xs generalizing n with
  | nil => rfl
  | cons x xs ih => simp [joinFrom, ih]

theorem joinFrom_zero (xs : List (List UInt8)) : joinFrom 0 xs = commaSep xs := by
  induction xs with
  | nil => rfl
  | cons x xs ih =>
    cases xs with
    | nil => simp [joinFrom, commaSep]
    | cons y ys =>
      simp only [joinFrom, ↓reduceIte, List.nil_append, commaSep] at ih ⊢
      rw [joinFrom_succ] at ih
      simp only [Nat.add_one_ne_zero, ↓reduceIte, List.append_assoc] at ih ⊢
      rw [joinFrom_succ 1, ← ih]

theorem view_none {pg : Page} {k : Nat} : (pg.cols[8 * k + 4]! != 1) = true → pg.view k = none := by
  intro h
  simp only [bne_iff_ne, ne_eq] at h
  simp [Page.view, h]

theorem view_some {pg : Page} {k : Nat} : (pg.cols[8 * k + 4]! != 1) = false →
    pg.view k = some ⟨pg.cols[8 * k]!, slice pg.blob pg.cols[8 * k + 1]! pg.cols[8 * k + 2]!,
      slice pg.blob pg.cols[8 * k + 2]! pg.cols[8 * k + 3]!, slice pg.blob pg.cols[8 * k + 5]! pg.cols[8 * k + 6]!,
      pg.cols[8 * k + 7]!⟩ := by
  intro h
  simp only [bne_eq_false_iff_eq] at h
  simp [Page.view, h]

/-- **`renderPage` writes the post objects of the rows that have an author, at most `limit`,
separated by commas.** -/
theorem renderPage_go (pg : Page) (limit : Nat) :
    ∀ f k out n, n ≤ limit →
      let vs := ((List.range' k f).filterMap pg.view).take (limit - n)
      bs (renderPage.go pg limit k f out n).1 = bs out ++ joinFrom n (vs.map viewJson) ∧
      (renderPage.go pg limit k f out n).2 = n + vs.length := by
  intro f
  induction f with
  | zero => intro k out n hn; simp [renderPage.go, joinFrom]
  | succ f ih =>
    intro k out n hn
    simp only [renderPage.go]
    split
    · have : limit - n = 0 := by omega
      simp [this, joinFrom]
    · rename_i hlt
      split
      · rename_i hskip
        have := ih (k + 1) out n hn
        simp only [List.range'_succ, List.filterMap_cons, view_none hskip]
        exact this
      · rename_i hrender
        have hlim : 0 < limit - n := by omega
        obtain ⟨h1, h2⟩ := ih (k + 1) _ (n + 1) (by omega)
        simp only [List.range'_succ, List.filterMap_cons, view_some (Bool.eq_false_iff.mpr hrender)]
        rw [show limit - n = (limit - (n + 1)) + 1 by omega, List.take_succ_cons]
        refine ⟨?_, ?_⟩
        · rw [h1]
          simp only [List.map_cons, joinFrom]
          by_cases hn0 : n = 0
          · simp only [hn0, beq_self_eq_true, ↓reduceIte, bs_postJsonAfter_id] at h1 ⊢
            simp
          · have : (n == 0) = false := by simpa using hn0
            simp only [this, Bool.false_eq_true, ↓reduceIte, bs_postJsonAfter_comma, hn0]
            simp
        · rw [h2]; simp; omega

theorem renderPage_spec (out : ByteArray) (pg : Page) (limit : Nat) :
    let vs := ((List.range pg.count).filterMap pg.view).take limit
    bs (renderPage out pg limit).1 = bs out ++ commaSep (vs.map viewJson) ∧
    (renderPage out pg limit).2 = vs.length := by
  have := renderPage_go pg limit (pg.cols.size / 8) 0 out 0 (Nat.zero_le _)
  simp only [Nat.sub_zero, Nat.zero_add, joinFrom_zero] at this
  simp only [renderPage, Page.count, List.range_eq_range']
  exact this

/-! ## The feed -/

theorem run_eq_of_snd {α : Type} {x : Model α} {db : DB} (h : (x.run db).2 = db) : x.run db = ((x.run db).1, db) :=
  Prod.ext rfl h

/-- A `filterMap` over indices is a `filterMap` over the list they index. -/
theorem filterMap_range_getElem {α β : Type} [Inhabited α] (l : List α) (F : Nat → Option β) (g : α → Option β)
    (h : ∀ k < l.length, F k = g l[k]!) : (List.range l.length).filterMap F = l.filterMap g := by
  induction l generalizing F with
  | nil => simp
  | cons x xs ih =>
    rw [List.length_cons, List.range_succ_eq_map, List.filterMap_cons, List.filterMap_map]
    have h0 := h 0 (by simp)
    simp only [List.getElem!_cons_zero] at h0
    rw [h0, ih (F ∘ Nat.succ) (fun k hk => by simpa using h (k + 1) (by simp; omega))]
    simp [List.filterMap_cons]

theorem take_take_filterMap_prefix {α β : Type} (l : List α) (g : α → Option β) (w n : Nat)
    (h : n ≤ ((l.take w).filterMap g).length) :
    ((l.take w).filterMap g).take n = (l.filterMap g).take n := by
  conv => rhs; rw [← List.take_append_drop w l, List.filterMap_append]
  rw [List.take_append_of_le_length h]

/-- What one attempt computes on the model, unfolded. -/
theorem feedTry_run (db : DB) (body : ByteArray) (want : Nat) :
    (feedTry (m := Model) body want).run db =
      let P := mkPage ((db.sorted.take want).map db.entry)
      ((Bytes.appendAll (renderPage (Bytes.appendAll (Bytes.clear body) kPosts) P feedSize).1 kPostsEnd,
        (renderPage (Bytes.appendAll (Bytes.clear body) kPosts) P feedSize).2, P.cols.size / 8), db) := by
  rfl

/-! ## What the handler sees of a page -/

/-- Pages the handler cannot tell apart: as many columns, and every row reads back as the same post
object. The self-test checks this of each page the C storage operations return against the
model's (`Selftest.samePage`); the text may lie in the blob in another order. -/
def Page.Same (p q : Page) : Prop := p.cols.size = q.cols.size ∧ ∀ k < p.count, p.view k = q.view k

theorem filterMap_congr_mem {α β : Type} (l : List α) (f g : α → Option β) (h : ∀ x ∈ l, f x = g x) :
    l.filterMap f = l.filterMap g := by
  induction l with
  | nil => rfl
  | cons x xs ih =>
    simp only [List.filterMap_cons, h x List.mem_cons_self]
    rw [ih (fun y hy => h y (List.mem_cons_of_mem _ hy))]

theorem filterMap_range_congr {β : Type} (n : Nat) (f g : Nat → Option β) (h : ∀ k < n, f k = g k) :
    (List.range n).filterMap f = (List.range n).filterMap g :=
  filterMap_congr_mem _ f g fun k hk => h k (List.mem_range.mp hk)

/-- **The feed's bytes depend on a page only through what it reads back as.** -/
theorem feedFromPage_congr (body : ByteArray) (p q : Page) (h : p.Same q) :
    bs (feedFromPage body p).1 = bs (feedFromPage body q).1 ∧ (feedFromPage body p).2 = (feedFromPage body q).2 := by
  obtain ⟨hs, hv⟩ := h
  have hc : p.count = q.count := by simp [Page.count, hs]
  have hviews : (List.range p.count).filterMap p.view = (List.range q.count).filterMap q.view := by
    rw [← hc]; exact filterMap_range_congr _ _ _ hv
  obtain ⟨hb1, hn1⟩ := renderPage_spec (Bytes.appendAll (Bytes.clear body) kPosts) p feedSize
  obtain ⟨hb2, hn2⟩ := renderPage_spec (Bytes.appendAll (Bytes.clear body) kPosts) q feedSize
  rw [hviews] at hb1 hn1
  simp only [feedFromPage, bs_appendAll]
  refine ⟨by rw [hb1, hb2], ?_⟩
  simp only [hn1, hn2, hs]

/-- **A post's response depends on its page only through what the page reads back as.** -/
theorem postFromPage_congr (scratch : ByteArray) (id : Nat) (p q : Page) (h : p.Same q) :
    (postFromPage scratch id p).1 = (postFromPage scratch id q).1 ∧
    bs (postFromPage scratch id p).2 = bs (postFromPage scratch id q).2 := by
  obtain ⟨hs, hv⟩ := h
  by_cases h8 : p.cols.size < 8
  · have h8' : q.cols.size < 8 := hs ▸ h8
    simp [postFromPage, h8, h8']
  · have h8' : ¬ q.cols.size < 8 := hs ▸ h8
    have h0 := hv 0 (by simp [Page.count]; omega)
    simp only [Page.view, Nat.mul_zero, Nat.zero_add] at h0
    by_cases hf : p.cols[4]! = 1
    · by_cases hf' : q.cols[4]! = 1
      · simp only [hf, hf', ↓reduceIte, Option.some.injEq, View.mk.injEq] at h0
        obtain ⟨-, hb, hcr, hau, hl⟩ := h0
        simp only [postFromPage, h8, h8', hf, hf', decide_false, bne_self_eq_false, Bool.or_self,
          Bool.false_eq_true, ↓reduceIte, bs_appendAll, bs_postJson, true_and]
        rw [hb, hcr, hau, hl]
      · simp [hf, hf'] at h0
    · by_cases hf' : q.cols[4]! = 1
      · simp [hf, hf'] at h0
      · simp [postFromPage, h8, h8', hf, hf']

/-- One attempt with the first `want` posts of the order. -/
theorem feedTry_spec (db : DB) (body : ByteArray) (want : Nat) :
    let V := ((db.sorted.take want).filterMap db.view).take feedSize
    let r := (feedTry (m := Model) body want).run db
    r.2 = db ∧ bs r.1.1 = feedJson V ∧ r.1.2.1 = V.length ∧ r.1.2.2 = (db.sorted.take want).length := by
  generalize hL : db.sorted.take want = L
  intro V r
  generalize hP : mkPage (L.map db.entry) = P
  have hPn : P.count = L.length := by rw [← hP, count_mkPage]; simp
  -- the rendered views are the specification's
  have hviews : (List.range P.count).filterMap P.view = L.filterMap db.view := by
    rw [hPn]
    apply filterMap_range_getElem
    intro k hk
    rw [← hP, view_mkPage _ k (by simpa using hk)]
    simp only [List.getElem_map, getElem!_pos L k hk, entryView_entry]
  have hrun : r = ((Bytes.appendAll (renderPage (Bytes.appendAll (Bytes.clear body) kPosts) P feedSize).1 kPostsEnd,
      (renderPage (Bytes.appendAll (Bytes.clear body) kPosts) P feedSize).2, P.cols.size / 8), db) := by
    show (feedTry (m := Model) body want).run db = _
    rw [feedTry_run]
    simp only [hL, hP]
  obtain ⟨hb, hcnt⟩ := renderPage_spec (Bytes.appendAll (Bytes.clear body) kPosts) P feedSize
  rw [hviews] at hb hcnt
  rw [hrun]
  refine ⟨rfl, ?_, ?_, ?_⟩
  · rw [bs_appendAll, hb]
    simp [feedJson, kPosts, kPostsEnd, V]
  · exact hcnt
  · simpa [Page.count] using hPn

theorem DB.sorted_length (db : DB) : db.sorted.length = db.posts.length := by
  simp [DB.sorted, List.length_mergeSort]

/-- The retry loop: once an attempt reads past the end of the posts or finds 20 posts with an
author, its answer is the feed. Doubling `want` gets there within `f + 1` attempts when there
are fewer than `want * 2^f` posts. -/
theorem feedLoop_spec (db : DB) :
    ∀ (f want : Nat) (body : ByteArray), db.posts.length < want * 2 ^ f →
      ((feedLoop (m := Model) body want (f + 1)).run db).2 = db ∧
      bs ((feedLoop (m := Model) body want (f + 1)).run db).1 = feedJson db.feed := by
  intro f
  induction f with
  | zero =>
    intro want body hlt
    obtain ⟨hst, hb, -, hgot⟩ := feedTry_spec db body want
    have hfull : db.sorted.take want = db.sorted := List.take_of_length_le (by rw [DB.sorted_length]; omega)
    simp only [feedLoop, StateT.run_bind]
    rw [run_eq_of_snd hst]
    simp only [id_bind]
    have hless : (db.sorted.take want).length < want := by rw [hfull, DB.sorted_length]; omega
    rw [hgot]
    simp only [hless, decide_true, Bool.or_true, ↓reduceIte, StateT.run_pure, id_pure]
    refine ⟨trivial, ?_⟩
    rw [hb, hfull]; rfl
  | succ f ih =>
    intro want body hlt
    obtain ⟨hst, hb, hn, hgot⟩ := feedTry_spec db body want
    simp only [feedLoop, StateT.run_bind]
    rw [run_eq_of_snd hst]
    simp only [id_bind]
    split
    · rename_i hstop
      refine ⟨rfl, ?_⟩
      simp only [StateT.run_pure, id_pure, hb, DB.feed]
      simp only [Bool.or_eq_true, decide_eq_true_eq, hn, hgot] at hstop
      rcases hstop with h20 | hless
      · rw [take_take_filterMap_prefix _ _ _ _ (by simp at h20; omega)]; rfl
      · have : db.sorted.take want = db.sorted :=
          List.take_of_length_le (by have := List.length_take_le want db.sorted; simp at hless ⊢; omega)
        rw [this]; rfl
    · exact ih (want * 2) _ (by rw [Nat.pow_succ] at hlt; rw [Nat.mul_assoc, Nat.mul_comm 2]; exact hlt)

/-- **`GET /feed` answers the specification's feed**: the handler's plan (the rows of the newest
posts, their authors, their like counts, retrying further down the order when posts lack an
author) computes
`ORDER BY created_at DESC, id DESC LIMIT 20` over the join, byte for byte. -/
theorem feedBody_correct (db : DB) (hsize : db.posts.length < 20 * 2 ^ 63) (scratch : ByteArray) :
    ((feedBody (m := Model) scratch).run db).2 = db ∧
    bs ((feedBody (m := Model) scratch).run db).1 = feedJson db.feed :=
  feedLoop_spec db 63 feedSize scratch hsize

/-! ## One post -/

/-- **`GET /posts/:id` answers the specification**: the post with its author and like count,
byte for byte, or not found exactly when the specification has no such post (no row, or no
author row). -/
theorem postBody_correct (db : DB) (scratch : ByteArray) (pid : Nat) :
    let r := (postBody (m := Model) scratch pid).run db
    r.2 = db ∧ r.1.1 = (db.getPost pid).isSome ∧ ∀ v, db.getPost pid = some v → bs r.1.2 = Spec.postJson v := by
  intro r
  have hrun1 : (Store.postPage (m := Model) pid).run db = (mkPage ((db.post? pid).toList.map db.entry), db) := rfl
  cases hp : db.post? pid with
  | none =>
    have hget : db.getPost pid = none := by simp [DB.getPost, hp]
    have hsize : (mkPage ((db.post? pid).toList.map db.entry)).cols.size = 0 := by simp [hp, mkPage, encPage]
    simp only [r, postBody, StateT.run_bind, hrun1, id_bind, postFromPage, hsize]
    simp [hget]
  | some p =>
    have hpid : p.id = pid := (DB.post?_mem hp).2
    cases hu : db.user? p.userId with
    | none =>
      have hget : db.getPost pid = none := by simp [DB.getPost, hp, DB.view, hu]
      have hcols : (mkPage ((db.post? pid).toList.map db.entry)).cols =
          #[p.id, 0, p.body.length, p.body.length + p.createdAt.length, 0, p.body.length + p.createdAt.length,
            p.body.length + p.createdAt.length, db.likeCount p.id] := by
        simp [hp, mkPage, encPage, DB.entry, hu]
      simp only [r, postBody, StateT.run_bind, hrun1, id_bind, postFromPage, hcols]
      simp [hget]
    | some u =>
      have hget : db.getPost pid = some ⟨p.id, p.body, p.createdAt, u.name, db.likeCount p.id⟩ := by
        simp [DB.getPost, hp, DB.view, hu]
      have hpage : mkPage ((db.post? pid).toList.map db.entry) =
          ⟨⟨(p.body ++ p.createdAt ++ u.name).toArray⟩,
           #[p.id, 0, p.body.length, p.body.length + p.createdAt.length, 1, p.body.length + p.createdAt.length,
             p.body.length + p.createdAt.length + u.name.length, db.likeCount p.id]⟩ := by
        simp [hp, mkPage, encPage, DB.entry, hu]
      simp only [r, postBody, StateT.run_bind, hrun1, id_bind, postFromPage, hpage]
      refine ⟨by simp, by simp [hget], fun v hv => ?_⟩
      rw [hget] at hv
      cases hv
      simp [bs_postJson, kPostWrap, kWrapEnd, Spec.postJson, hpid, slice]

end Feed

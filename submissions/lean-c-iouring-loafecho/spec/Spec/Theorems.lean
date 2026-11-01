import Spec.Basic

/-!
# Spec.Theorems — the B-tree plans equal the SQL references under `Inv`,
`checkInv` decides `Inv` soundly, and inserts that pass `insertOk` preserve `Inv`.
-/

namespace Db

/-! ## `newer` is a total preorder, antisymmetric up to (createdAt, id) -/

theorem newer_iff (a b : Post) :
    newer a b = true ↔ (b.createdAt < a.createdAt ∨ (b.createdAt = a.createdAt ∧ b.id ≤ a.id)) := by
  simp [newer]

theorem newer_trans (a b c : Post) : newer a b = true → newer b c = true → newer a c = true := by
  simp only [newer_iff]
  rintro (h1 | ⟨h1, h1'⟩) (h2 | ⟨h2, h2'⟩)
  · exact Or.inl (String.lt_trans h2 h1)
  · exact Or.inl (h2 ▸ h1)
  · exact Or.inl (h1 ▸ h2)
  · exact Or.inr ⟨h2.trans h1, Nat.le_trans h2' h1'⟩

theorem newer_total (a b : Post) : (newer a b || newer b a) = true := by
  simp only [Bool.or_eq_true, newer_iff]
  by_cases h1 : b.createdAt < a.createdAt
  · exact Or.inl (Or.inl h1)
  by_cases h2 : a.createdAt < b.createdAt
  · exact Or.inr (Or.inl h2)
  have heq : b.createdAt = a.createdAt :=
    String.le_antisymm (String.not_lt.mp h2) (String.not_lt.mp h1)
  rcases Nat.le_total b.id a.id with h | h
  · exact Or.inl (Or.inr ⟨heq, h⟩)
  · exact Or.inr (Or.inr ⟨heq.symm, h⟩)

theorem newer_antisymm_key (a b : Post) (h1 : newer a b = true) (h2 : newer b a = true) :
    a.createdAt = b.createdAt ∧ a.id = b.id := by
  rw [newer_iff] at h1 h2
  rcases h1 with h1 | ⟨h1, h1'⟩ <;> rcases h2 with h2 | ⟨h2, h2'⟩
  · exact absurd h2 (String.lt_asymm h1)
  · exact absurd (h2 ▸ h1) (String.lt_irrefl _)
  · exact absurd (h1 ▸ h2) (String.lt_irrefl _)
  · exact ⟨h2, Nat.le_antisymm h2' h1'⟩

/-! ## Consequences of `Inv` -/

/-- Dense ids identify rows: two members with the same id are the same row. -/
theorem eq_of_id_eq {db : Db} (hd : ∀ i (h : i < db.posts.length), db.posts[i].id = i + 1)
    {a b : Post} (ha : a ∈ db.posts) (hb : b ∈ db.posts) (hab : a.id = b.id) : a = b := by
  obtain ⟨i, hi, rfl⟩ := List.mem_iff_getElem.mp ha
  obtain ⟨j, hj, rfl⟩ := List.mem_iff_getElem.mp hb
  have : i = j := by have := hd i hi; have := hd j hj; omega
  subst this; rfl

/-- Under `Inv`, rowid order is (createdAt, id)-ascending with strictly increasing ids. -/
theorem posts_pairwise_strict (db : Db) (h : db.Inv) :
    db.posts.Pairwise (fun a b => a.createdAt ≤ b.createdAt ∧ a.id < b.id) := by
  obtain ⟨hd, hm, -⟩ := h
  rw [List.pairwise_iff_getElem] at hm ⊢
  intro i j hi hj hij
  exact ⟨hm i j hi hj hij, by rw [hd i hi, hd j hj]; omega⟩

theorem reverse_pairwise_newer (db : Db) (h : db.Inv) : db.posts.reverse.Pairwise (fun a b => newer a b = true) := by
  rw [List.pairwise_reverse]
  refine (posts_pairwise_strict db h).imp ?_
  rintro a b ⟨hc, hid⟩
  rw [newer_iff]
  by_cases hlt : a.createdAt < b.createdAt
  · exact Or.inl hlt
  · exact Or.inr ⟨String.le_antisymm hc (String.not_lt.mp hlt), Nat.le_of_lt hid⟩

/-- Key lemma: under `Inv`, sorting newest-first is just reading the table backwards. -/
theorem mergeSort_newer_eq_reverse (db : Db) (h : db.Inv) :
    db.posts.mergeSort newer = db.posts.reverse := by
  apply List.Perm.eq_of_pairwise (le := fun a b => newer a b = true)
  · intro a b ha hb h1 h2
    have ha' : a ∈ db.posts := (List.mergeSort_perm db.posts newer).subset ha
    have hb' : b ∈ db.posts := List.mem_reverse.mp hb
    exact eq_of_id_eq h.1 ha' hb' (newer_antisymm_key a b h1 h2).2
  · exact List.pairwise_mergeSort (fun a b c => newer_trans a b c) newer_total db.posts
  · exact reverse_pairwise_newer db h
  · exact (List.mergeSort_perm db.posts newer).trans (List.reverse_perm db.posts).symm

/-! ## 1. Feed reference = last 20 rows, newest first -/

theorem feed_eq_tail (db : Db) (h : db.Inv) :
    db.feed = ((db.posts.reverse.take 20).map db.render) := by
  unfold feed
  rw [mergeSort_newer_eq_reverse db h]

/-! ## 2. Feed plan (backward cursor walk) = feed reference -/

theorem walkBack_eq (posts : List Post) :
    ∀ (k n : Nat), n ≤ posts.length → walkBack posts k n = ((posts.take n).reverse).take k
  | 0, _, _ => by simp [walkBack]
  | _ + 1, 0, _ => by simp [walkBack]
  | k + 1, i + 1, hn => by
    have hi : i < posts.length := by omega
    have ih := walkBack_eq posts k i (by omega)
    show (match posts[i]? with | some p => p :: walkBack posts k i | none => []) = _
    rw [List.getElem?_eq_getElem hi, ih, List.take_add_one (l := posts) (i := i),
      List.getElem?_eq_getElem hi]
    simp only [Option.toList, List.reverse_append, List.reverse_cons, List.reverse_nil,
      List.nil_append, List.singleton_append, List.take_succ_cons]

theorem feedPlan_eq (db : Db) (h : db.Inv) : db.feedPlan = db.feed := by
  rw [feed_eq_tail db h, feedPlan, walkBack_eq db.posts 20 db.posts.length (Nat.le_refl _),
    List.take_length]

/-! ## 3. Post plan (rowid seek) = getPost reference -/

theorem postPlan_eq (db : Db) (h : db.Inv) (id : Nat) : db.postPlan id = db.getPost id := by
  have hd := h.1
  unfold postPlan getPost
  by_cases hid : id ≥ 1 ∧ id - 1 < db.posts.length
  · obtain ⟨h1, h2⟩ := hid
    have hf : db.posts.find? (·.id = id) = some db.posts[id - 1] := by
      rw [List.find?_eq_some_iff_getElem]
      refine ⟨?_, id - 1, h2, rfl, ?_⟩
      · simp only [decide_eq_true_eq]; rw [hd _ h2]; omega
      · intro j hj
        simp only [Bool.not_eq_true', decide_eq_false_iff_not]
        rw [hd j (by omega)]; omega
    simp only [h1, ↓reduceIte, List.getElem?_eq_getElem h2, hf]
  · have hnone : db.posts.find? (·.id = id) = none := by
      rw [List.find?_eq_none]
      intro x hx
      obtain ⟨j, hj, rfl⟩ := List.mem_iff_getElem.mp hx
      simp only [decide_eq_true_eq]
      rw [hd j hj]; omega
    rw [hnone]
    split
    · rw [List.getElem?_eq_none (by omega)]
    · rfl

/-! ## 4. checkInv is sound for Inv -/

theorem denseFrom_sound : ∀ (n : Nat) (l : List Post), denseFrom n l = true →
    ∀ i (h : i < l.length), l[i].id = i + n
  | _, [], _, i, h => absurd h (Nat.not_lt_zero _)
  | n, p :: ps, hc, i, h => by
    simp only [denseFrom, Bool.and_eq_true, beq_iff_eq] at hc
    cases i with
    | zero => simpa using hc.1
    | succ i =>
      have := denseFrom_sound (n + 1) ps hc.2 i (by simp at h; omega)
      simp only [List.getElem_cons_succ, this]; omega

theorem monoB_sound : ∀ (l : List Post), monoB l = true →
    l.Pairwise (fun a b => a.createdAt ≤ b.createdAt)
  | [], _ => List.Pairwise.nil
  | [_], _ => List.pairwise_singleton _ _
  | a :: b :: rest, hc => by
    simp only [monoB, Bool.and_eq_true, decide_eq_true_eq] at hc
    have ih := monoB_sound (b :: rest) hc.2
    refine List.Pairwise.cons ?_ ih
    intro c hc'
    rcases List.mem_cons.mp hc' with rfl | hc'
    · exact hc.1
    · exact String.le_trans hc.1 (List.rel_of_pairwise_cons ih hc')

theorem checkInv_sound (db : Db) (h : db.checkInv = true) : db.Inv := by
  simp only [checkInv, Bool.and_eq_true, List.all_eq_true, List.any_eq_true, beq_iff_eq] at h
  obtain ⟨⟨hd, hm⟩, ha⟩ := h
  exact ⟨denseFrom_sound 1 db.posts hd, monoB_sound db.posts hm, ha⟩

/-! ## 5. Inserts that pass insertOk preserve Inv -/

theorem inv_addPost (db : Db) (p : Post) (h : db.Inv) (hp : db.insertOk p = true) :
    (db.addPost p).Inv := by
  obtain ⟨hd, hm, ha⟩ := h
  simp only [insertOk, Bool.and_eq_true, beq_iff_eq, List.any_eq_true] at hp
  obtain ⟨⟨hpid, hlast⟩, hpu⟩ := hp
  refine ⟨?_, ?_, ?_⟩
  · intro i hi
    simp only [addPost, List.length_append, List.length_singleton] at hi
    simp only [addPost]
    by_cases hlt : i < db.posts.length
    · rw [List.getElem_append_left hlt]; exact hd i hlt
    · have : i = db.posts.length := by omega
      subst this
      simp [hpid]
  · simp only [addPost, List.pairwise_append, List.pairwise_singleton, List.mem_singleton,
      forall_eq, true_and]
    refine ⟨hm, ?_⟩
    intro a ha'
    obtain ⟨j, hj, rfl⟩ := List.mem_iff_getElem.mp ha'
    have hlen : db.posts.length - 1 < db.posts.length := by omega
    rw [List.getLast?_eq_getElem?, List.getElem?_eq_getElem hlen] at hlast
    simp only [decide_eq_true_eq] at hlast
    refine String.le_trans ?_ hlast
    by_cases hjl : j = db.posts.length - 1
    · subst hjl; exact String.le_refl _
    · exact List.pairwise_iff_getElem.mp hm j _ hj hlen (by omega)
  · intro q hq
    simp only [addPost, List.mem_append, List.mem_singleton] at hq
    rcases hq with hq | rfl
    · exact ha q hq
    · exact hpu

/-! ## 6. Likes never affect Inv -/

theorem inv_addLike (db : Db) (l : Like) (h : db.Inv) : (db.addLike l).Inv := h

end Db

import Feed.Proofs.Bytes

/-!
# Token checks

What an accepted token satisfies: its signature is the base64url HMAC-SHA256 of its first two
parts under the server's key, its header says `"alg":"HS256"`, its `exp` is after `now`.
-/
namespace Feed.Jwt
open Json

theorem ctEq_go_zero (x a : ByteArray) (lo : Nat) :
    ∀ f k acc, ctEq.go x a lo k f acc = 0 → acc = 0 ∧ ∀ j, k ≤ j → j < k + f → x.get! j = a.get! (lo + j) := by
  intro f
  induction f with
  | zero => intro k acc h; exact ⟨h, fun j h1 h2 => by omega⟩
  | succ f ih =>
    intro k acc h
    obtain ⟨h0, hrest⟩ := ih (k + 1) _ h
    have hor := h0
    rw [UInt8.or_eq_zero_iff] at hor
    refine ⟨hor.1, fun j h1 h2 => ?_⟩
    rcases Nat.eq_or_lt_of_le h1 with rfl | hlt
    · have := hor.2
      rw [UInt8.xor_eq_zero_iff] at this
      exact this
    · exact hrest j (by omega) (by omega)

/-- `ctEq` holds exactly when every byte matches (and the lengths agree). -/
theorem ctEq_spec {x a : ByteArray} {lo hi : Nat} (h : ctEq x a lo hi = true) :
    hi - lo = x.size ∧ ∀ j < x.size, x.get! j = a.get! (lo + j) := by
  unfold ctEq at h
  simp only [Bool.and_eq_true, beq_iff_eq] at h
  exact ⟨h.1, fun j hj => (ctEq_go_zero x a lo x.size 0 0 h.2).2 j (Nat.zero_le _) (by omega)⟩

/-- **An accepted token was signed with the key**: the bytes after its second dot are the base64url
HMAC-SHA256 of the bytes before it, under `key`; its header names HS256; its `exp` is after `now`. -/
theorem verify_ok_sound {key : Hmac.Key} {now : Nat} {a : ByteArray} {lo hi uid : Nat} {name : ByteArray}
    (h : verify key now a lo hi = .ok uid name) :
    let hi' := min hi a.size
    let d1 := Bytes.findByte a 0x2e lo hi'
    let d2 := Bytes.findByte a 0x2e (d1 + 1) hi'
    let sig := b64Encode (Hmac.mac key a lo d2)
    (hi' - (d2 + 1) = sig.size ∧ ∀ j < sig.size, sig.get! j = a.get! (d2 + 1 + j)) ∧
    (∃ hd pl, b64Decode a lo d1 = some hd ∧ b64Decode a (d1 + 1) d2 = some pl ∧
      (∃ hf, parseTop hd 0 hd.size #[keyAlg] = .object hf ∧ hf[0]!.kind = .str ∧
        decodeString hd hf[0]!.lo hf[0]!.hi = hs256) ∧
      (∃ pf, parseTop pl 0 pl.size #[keyExp, keyNbf, keySub, keyUsername] = .object pf ∧ pf[0]!.kind = .num ∧
        numGt pl pf[0]!.lo pf[0]!.hi now = true)) := by
  intro hi' d1 d2 sig
  unfold verify at h
  simp only at h
  split at h
  · cases h
  split at h
  · cases h
  next hsig =>
  refine ⟨ctEq_spec (by simpa using hsig), ?_⟩
  split at h
  next hd pl hdh hdp =>
    refine ⟨hd, pl, hdh, hdp, ?_⟩
    split at h
    next hf pf hph hpp =>
      split at h
      · cases h
      next halg =>
      split at h
      · cases h
      next hexp =>
      simp only [Bool.or_eq_true, bne_iff_ne, ne_eq, not_or, Decidable.not_not] at halg hexp
      exact ⟨⟨hf, hph, halg⟩, pf, hpp, by simpa using hexp⟩
    next => cases h
  next => cases h

end Feed.Jwt

import Feed.Contract

namespace Feed.Contract

/-- A conservative body bound from the C primitive contracts (i64: 20 bytes, bool: 5).
Text/fragments are unbounded here. Fuel only limits generator recursion; exhaustion chooses
unbounded storage, never an unsound small bound. This is not a proof of the C primitives. -/
def Shape.fixedBound : Nat → Shape ctx → Option Nat
  | 0, _ => none
  | n + 1, .obj fs =>
    fs.foldlM (fun acc (k, v) => do
      let size ← v.fixedBound n
      pure (acc + (jsonString k).utf8ByteSize + 1 + size)) (2 + (fs.length - 1))
  | _ + 1, .int _ => some 20
  | _ + 1, .bool _ => some 5
  | _ + 1, .raw s => some s.utf8ByteSize
  | _ + 1, .str _ | _ + 1, .plainStr _ | _ + 1, .embed _ => none

/-- Capacity selection requires evidence from the actual wire shape, not an endpoint label. -/
structure Bounded (s : Shape ctx) (capacity : Nat) where
  body : Nat
  calculated : s.fixedBound 32 = some body
  fits : 96 + body ≤ capacity

def inlineCapacity : Nat := 192

def healthBound : Bounded healthShape inlineCapacity := ⟨57, by decide, by decide⟩
def likeBound : Bounded likeShape inlineCapacity := ⟨67, by decide, by decide⟩

example : postShape.fixedBound 32 = none := by decide
example : degradedShape.fixedBound 32 = none := by decide

end Feed.Contract

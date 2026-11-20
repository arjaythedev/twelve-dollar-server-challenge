/-! Typed wire contexts. References carry membership and type; no runtime tags are emitted. -/
namespace Feed.Wire
inductive Ty where
  | int | text | plainText | bool | json | fd | u32 | u64
  deriving Repr, DecidableEq
structure Binding where
  name : String
  ty : Ty
  deriving Repr, DecidableEq
inductive Ref : List Binding → Ty → Type where
  | here : Ref (b :: bs) b.ty
  | there : Ref bs t → Ref (b :: bs) t
def Ref.name : Ref bs t → String
  | @Ref.here b _ => b.name
  | .there r => r.name
/-- Evidence needed to copy string contents without JSON escaping. The parser's validated
unescaped-string branch establishes this boundary; trimming preserves it. -/
structure PlainText where
  value : String
  safe : ∀ c ∈ value.toList, c.toNat ≥ 32 ∧ c ≠ '"' ∧ c ≠ '\\'

/-- Machine integers match the generated C serializer's domain. -/
abbrev Value : Ty → Type
  | .int => Int64
  | .text | .json => String
  | .plainText => PlainText
  | .bool => Bool
  | .fd => Int32
  | .u32 => UInt32
  | .u64 => UInt64
def Env : List Binding → Type
  | [] => Unit
  | b :: bs => Value b.ty × Env bs
def Ref.get : Ref bs t → Env bs → Value t
  | .here, (v, _) => v
  | .there r, (_, rest) => r.get rest
end Feed.Wire

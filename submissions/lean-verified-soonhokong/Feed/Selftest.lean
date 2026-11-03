import Feed.Server
import Feed.Model

/-!
# Self-test

`bin/server --selftest <file>` (run by build.sh after linking) checks the C code that no proof
covers:

* each leaf (`runtime/leaves.c`, and `idle_pick` in `runtime/uring.c`) against its Lean
  definition, on random inputs;
* each storage operation of `runtime/db.c` against its meaning in `Feed/Model.lean`, on a random
  database written to `<file>` (ties in `created_at`, gaps in ids, posts whose author row is
  missing, a `users` table of three B-tree levels), before and after random writes; a page is
  compared row by row as the post objects it reads back as;
* the grouped rowid search of `runtime/db.c` against SQLite's own on every key of `users` and
  `posts`, and its in-order passes over the indexes (the feed's order, the like counts) against
  SQLite's cursors, also on trees of three levels;
* a transaction that grows the file and rolls back, and SQLite's integrity check after the writes;
* SHA-256 and HMAC-SHA256 against the published test vectors.
-/
namespace Feed.Selftest
open Bytes

/-! The references: the leaves' Lean bodies, restated without `@[extern]` so that they run as
Lean. Each is tied to its definition by `rfl` or a proof. -/

def refAppendEsc (out src : ByteArray) (lo hi : Nat) : ByteArray := out ++ ⟨(escape (slice src lo hi)).toArray⟩
def refAppendLitEsc (out lit src : ByteArray) (lo hi : Nat) : ByteArray :=
  out ++ lit ++ ⟨(escape (slice src lo hi)).toArray⟩
def refAppendNat (out : ByteArray) (n : Nat) : ByteArray := out ++ (toString n).toUTF8
def refAppendLitNat (out lit : ByteArray) (n : Nat) : ByteArray := out ++ lit ++ (toString n).toUTF8
def refAppendSlice (out src : ByteArray) (lo hi : Nat) : ByteArray := out ++ ⟨(slice src lo hi).toArray⟩
def refAppendAll (out src : ByteArray) : ByteArray := out ++ src
def refFindByte (a : ByteArray) (b : UInt8) (lo hi : Nat) : Nat := findByte.go a b hi lo (min hi a.size - lo)
def refFindHeadEnd (a : ByteArray) (lo hi : Nat) : Nat := findHeadEnd.go a hi lo (min hi a.size + 1 - lo)

example : @appendEsc = @refAppendEsc := rfl
example : @appendLitEsc = @refAppendLitEsc := rfl
example : @appendNat = @refAppendNat := rfl
example : @appendLitNat = @refAppendLitNat := rfl
example : @appendSlice = @refAppendSlice := rfl
example : @appendAll = @refAppendAll := rfl
example : @findByte = @refFindByte := rfl
example : @findHeadEnd = @refFindHeadEnd := rfl

def refMac (key : Hmac.Key) (m : ByteArray) (lo hi : Nat) : ByteArray :=
  let i := Sha256.finish key.inner 64 m lo hi
  Sha256.digest (Sha256.compressW key.outer i.a i.b i.c i.d i.e i.f i.g i.h 0x80000000 0 0 0 0 0 0 768)
def refB64Encode (d : ByteArray) : ByteArray :=
  Jwt.b64Encode.go d 0 (d.size / 3 + 1) (ByteArray.emptyWithCapacity (d.size * 4 / 3 + 4))
def refB64Decode (a : ByteArray) (lo hi : Nat) : Option ByteArray :=
  let hi := min hi a.size
  let hi := if hi > lo && a.get! (hi - 1) == 0x3d then hi - 1 else hi
  let hi := if hi > lo && a.get! (hi - 1) == 0x3d then hi - 1 else hi
  if (hi - lo) % 4 == 1 then none
  else Jwt.b64Decode.go a lo (hi - lo) 0 0 (ByteArray.emptyWithCapacity ((hi - lo) * 3 / 4 + 3))

def refPostJsonAfter (out start : ByteArray) (id : Nat) (src : ByteArray) (b c e : Nat)
    (names : ByteArray) (ns ne : Nat) (likes : Nat) : ByteArray :=
  let out := refAppendLitNat out start id
  let out := refAppendLitEsc out kBody src b c
  let out := refAppendLitEsc out kCreated src c e
  let out := refAppendLitEsc out kAuthor names ns ne
  let out := refAppendLitNat out kLikes likes
  refAppendAll out kClose

example : @postJsonAfter = @refPostJsonAfter := rfl

/-- `renderPage.go` with the reference post object. -/
def refRenderGo (pg : Page) (limit : Nat) (k : Nat) : Nat → ByteArray → Nat → ByteArray × Nat
  | 0, out, n => (out, n)
  | f + 1, out, n =>
    if n ≥ limit then (out, n) else
    let m := pg.cols
    if m[8 * k + 4]! != 1 then refRenderGo pg limit (k + 1) f out n else
    let out := refPostJsonAfter out (if n == 0 then kId else kCommaId) m[8 * k]! pg.blob m[8 * k + 1]! m[8 * k + 2]!
      m[8 * k + 3]! pg.blob m[8 * k + 5]! m[8 * k + 6]! m[8 * k + 7]!
    refRenderGo pg limit (k + 1) f out (n + 1)

theorem refRenderGo_eq (pg : Page) (limit : Nat) : ∀ f k out n,
    renderPage.go pg limit k f out n = refRenderGo pg limit k f out n := by
  intro f
  induction f with
  | zero => intros; rfl
  | succ f ih => intro k out n; simp only [renderPage.go, refRenderGo, ih]; rfl

def refFeedFromPage (_body : ByteArray) (pg : Page) : ByteArray × Nat × Nat :=
  let (b, n) := refRenderGo pg feedSize 0 (pg.cols.size / 8) (refAppendAll ByteArray.empty kPosts) 0
  (refAppendAll b kPostsEnd, n, pg.cols.size / 8)

example : @feedFromPage = @refFeedFromPage := by
  funext body pg
  simp only [feedFromPage, refFeedFromPage, renderPage, refRenderGo_eq]
  rfl

/-! HTTP framing: `Http.parse` with the reference byte searches. -/

def refHasToken (a : ByteArray) (hi : Nat) (tok : ByteArray) (i : Nat) : Nat → Bool
  | 0 => false
  | f + 1 =>
    if i ≥ hi then false else
    let e := refFindByte a 0x2c i hi
    let s := Http.skipL a i e
    Http.eqCI a s (Http.skipR a s e) tok || refHasToken a hi tok (e + 1) f

theorem refHasToken_eq (a : ByteArray) (hi : Nat) (tok : ByteArray) :
    ∀ f i, Http.hasToken.go a hi tok i f = refHasToken a hi tok i f := by
  intro f
  induction f with
  | zero => intros; rfl
  | succ f ih => intro i; simp only [Http.hasToken.go, refHasToken, ih]; rfl

def refScanHeads (a : ByteArray) (he : Nat) (i : Nat) (h : Http.Heads) : Nat → Http.Heads
  | 0 => h
  | f + 1 =>
    if i ≥ he then h else
    let le := refFindByte a 0x0d i he
    let colon := refFindByte a 0x3a i le
    let h :=
      if colon ≥ le || colon == i then { h with bad := true } else
      let vs := Http.skipL a (colon + 1) le
      let ve := Http.skipR a vs le
      if Http.eqCI a i colon Http.hContentLength then
        match Http.parseLen a vs ve with
        | none => { h with lenBad := true }
        | some n => match h.len with
          | none => { h with len := some n }
          | some m => if m == n then h else { h with lenBad := true }
      else if Http.eqCI a i colon Http.hAuthorization then { h with hasAuth := true, authLo := vs, authHi := ve }
      else if Http.eqCI a i colon Http.hConnection then
        { h with close := h.close || refHasToken a ve Http.tClose vs (ve - vs + 1),
                 keepAlive := h.keepAlive || refHasToken a ve Http.tKeepAlive vs (ve - vs + 1) }
      else if Http.eqCI a i colon Http.hTransferEncoding then { h with te := true }
      else h
    refScanHeads a he (le + 2) h f

theorem refScanHeads_eq (a : ByteArray) (he : Nat) :
    ∀ f i h, Http.scanHeads a he i h f = refScanHeads a he i h f := by
  intro f
  induction f with
  | zero => intros; rfl
  | succ f ih =>
    intro i h
    simp only [Http.scanHeads, refScanHeads, ih, Http.hasToken, refHasToken_eq]
    rfl

def refParse (a : ByteArray) (pos : Nat) : Http.Parse :=
  let n := a.size
  let he := refFindHeadEnd a pos n
  if he ≥ n then (if n - pos > Http.maxHead then .bad 431 else .incomplete) else
  if he - pos > Http.maxHead then .bad 431 else
  let le := refFindByte a 0x0d pos he
  let sp1 := refFindByte a 0x20 pos le
  let sp2 := refFindByte a 0x20 (sp1 + 1) le
  if sp1 ≥ le || sp2 ≥ le || sp1 == pos then .bad 400 else
  let v11 := Http.eqAt a (sp2 + 1) le Http.vHttp11
  let v10 := Http.eqAt a (sp2 + 1) le Http.vHttp10
  if !(v11 || v10) then .bad 400 else
  let method := if Http.eqAt a pos sp1 Http.mGet then Http.Method.get
    else if Http.eqAt a pos sp1 Http.mPost then Http.Method.post else Http.Method.other
  let q := refFindByte a 0x3f (sp1 + 1) sp2
  let h := if le == he then {} else refScanHeads a he (le + 2) {} (he - le)
  if h.bad || h.lenBad || h.te then .bad 400 else
  let len := h.len.getD 0
  if len > Http.maxBody then .bad 413 else
  let bodyLo := he + 4
  if bodyLo + len > n then .incomplete else
  let close := if v11 then h.close else !h.keepAlive
  .ok { method, pathLo := sp1 + 1, pathHi := q, hasAuth := h.hasAuth, authLo := h.authLo, authHi := h.authHi,
        bodyLo, bodyHi := bodyLo + len, close, next := bodyLo + len }

example : @Http.parse = @refParse := by
  funext a pos
  simp only [Http.parse, refParse, refScanHeads_eq]
  rfl

/-- `route` with the reference byte search. -/
def refRoute (a : ByteArray) (r : Http.Req) : Route :=
  let hi := min r.pathHi a.size
  let lo := min r.pathLo hi
  match r.method with
  | .get =>
    if Http.eqAt a lo hi pFeed then .feed
    else if Http.eqAt a lo hi pHealth then .health
    else if hi - lo > 7 && Http.eqAt a lo (lo + 7) pPostsSlash && refFindByte a 0x2f (lo + 7) hi == hi then
      .post (parseId a (lo + 7) hi)
    else .notFound
  | .post =>
    if Http.eqAt a lo hi pPosts then .create
    else if hi - lo > 12 && Http.eqAt a lo (lo + 7) pPostsSlash && Http.eqAt a (hi - 5) hi pLike
      && refFindByte a 0x2f (lo + 7) (hi - 5) == hi - 5 then .like (parseId a (lo + 7) (hi - 5))
    else .notFound
  | .other => .notFound

example : @route = @refRoute := rfl

/-! JSON input: `Json.decodeString` and `Json.parseTop` with the reference byte operations. -/

def refDecodeGo (a : ByteArray) (hi : Nat) (i : Nat) : Nat → ByteArray → ByteArray
  | 0, out => out
  | f + 1, out =>
    if i + 1 ≥ hi then out else
    let b := Json.byte a i
    if b == 0x5c then
      let e := Json.byte a (i + 1)
      if e == 0x75 then
        let c := Json.hex4 a (i + 2)
        if 0xd800 ≤ c && c < 0xdc00 && i + 12 ≤ hi - 1 && Json.byte a (i + 6) == 0x5c && Json.byte a (i + 7) == 0x75 then
          let d := Json.hex4 a (i + 8)
          if 0xdc00 ≤ d && d < 0xe000 then refDecodeGo a hi (i + 12) f (Json.pushUtf8 out (0x10000 + (c - 0xd800) * 1024 + (d - 0xdc00)))
          else refDecodeGo a hi (i + 6) f (Json.pushUtf8 out 0xfffd)
        else if 0xd800 ≤ c && c < 0xe000 then refDecodeGo a hi (i + 6) f (Json.pushUtf8 out 0xfffd)
        else refDecodeGo a hi (i + 6) f (Json.pushUtf8 out c)
      else
        let c : UInt8 := if e == 0x62 then 0x08 else if e == 0x66 then 0x0c else if e == 0x6e then 0x0a
          else if e == 0x72 then 0x0d else if e == 0x74 then 0x09 else e
        refDecodeGo a hi (i + 2) f (out.push c)
    else if b < 0x80 then refDecodeGo a hi (i + 1) f (out.push b)
    else
      let n := if b &&& 0xe0 == 0xc0 then 2 else if b &&& 0xf0 == 0xe0 then 3 else if b &&& 0xf8 == 0xf0 then 4 else 0
      let cont (k : Nat) := k < n → (i + k < hi - 1 && Json.byte a (i + k) &&& 0xc0 == 0x80)
      let ok := n ≥ 2 && cont 1 && (n < 3 || cont 2) && (n < 4 || cont 3)
      if ok then
        let cp := if n == 2 then (b &&& 0x1f).toNat * 64 + (Json.byte a (i + 1) &&& 0x3f).toNat
          else if n == 3 then ((b &&& 0x0f).toNat * 64 + (Json.byte a (i + 1) &&& 0x3f).toNat) * 64 + (Json.byte a (i + 2) &&& 0x3f).toNat
          else (((b &&& 0x07).toNat * 64 + (Json.byte a (i + 1) &&& 0x3f).toNat) * 64 + (Json.byte a (i + 2) &&& 0x3f).toNat) * 64 + (Json.byte a (i + 3) &&& 0x3f).toNat
        let minCp := if n == 2 then 0x80 else if n == 3 then 0x800 else 0x10000
        if cp < minCp || (0xd800 ≤ cp && cp < 0xe000) || cp > 0x10ffff then refDecodeGo a hi (i + 1) f (Json.pushUtf8 out 0xfffd)
        else refDecodeGo a hi (i + n) f (refAppendSlice out a i (i + n))
      else refDecodeGo a hi (i + 1) f (Json.pushUtf8 out 0xfffd)

theorem refDecodeGo_eq (a : ByteArray) (hi : Nat) :
    ∀ f i out, Json.decodeString.go a hi i f out = refDecodeGo a hi i f out := by
  intro f
  induction f with
  | zero => intros; rfl
  | succ f ih => intro i out; simp only [Json.decodeString.go, refDecodeGo, ih]; rfl

def refDecodeString (a : ByteArray) (lo hi : Nat) : ByteArray :=
  refDecodeGo a hi (lo + 1) (hi - lo) (ByteArray.emptyWithCapacity (hi - lo))

theorem decodeString_eq_ref : @Json.decodeString = @refDecodeString := by
  funext a lo hi
  simp only [Json.decodeString, refDecodeString, refDecodeGo_eq]

def refKeyEq (a : ByteArray) (klo khi : Nat) (key : ByteArray) : Bool :=
  if refFindByte a 0x5c klo khi == min khi a.size then Json.spanEq a klo khi key
  else
    let d := refDecodeString a (klo - 1) (khi + 1)
    d.size == key.size && Json.spanEq d 0 d.size key

def refRecordGo (a : ByteArray) (klo khi vlo vhi : Nat) (keys : Array ByteArray) (k : Nat) :
    Nat → Array Json.Field → Array Json.Field
  | 0, fs => fs
  | f + 1, fs =>
    if refKeyEq a klo khi (keys[k]!) then fs.set! k ⟨Json.kindAt a vlo, vlo, vhi⟩ else refRecordGo a klo khi vlo vhi keys (k + 1) f fs

theorem refRecordGo_eq (a : ByteArray) (klo khi vlo vhi : Nat) (keys : Array ByteArray) :
    ∀ f k fs, Json.recordField.go a klo khi vlo vhi keys k f fs = refRecordGo a klo khi vlo vhi keys k f fs := by
  have hk : ∀ key, Json.keyEq a klo khi key = refKeyEq a klo khi key := by
    intro key
    simp only [Json.keyEq, refKeyEq, decodeString_eq_ref]
    rfl
  intro f
  induction f with
  | zero => intros; rfl
  | succ f ih => intro k fs; simp only [Json.recordField.go, refRecordGo, ih, hk]

def refTopMembers (a : ByteArray) (hi : Nat) (keys : Array ByteArray) (i : Nat) (first : Bool) (fs : Array Json.Field) :
    Nat → Option (Nat × Array Json.Field)
  | 0 => none
  | f + 1 =>
    if i ≥ hi then none
    else if first && Json.byte a i == 0x7d then some (i + 1, fs)
    else if Json.byte a i != 0x22 then none
    else do
      let j ← Json.scanString a hi (i + 1)
      let klo := i + 1
      let khi := j - 1
      let j := Json.skipWs a hi j
      if j ≥ hi || Json.byte a j != 0x3a then none
      let vlo := Json.skipWs a hi (j + 1)
      let k ← Json.scanValue a hi vlo f
      let fs := refRecordGo a klo khi vlo k keys 0 keys.size fs
      let k := Json.skipWs a hi k
      if k ≥ hi then none
      else if Json.byte a k == 0x7d then some (k + 1, fs)
      else if Json.byte a k == 0x2c then refTopMembers a hi keys (Json.skipWs a hi (k + 1)) false fs f
      else none

theorem refTopMembers_eq (a : ByteArray) (hi : Nat) (keys : Array ByteArray) :
    ∀ f i first fs, Json.topMembers a hi keys i first fs f = refTopMembers a hi keys i first fs f := by
  intro f
  induction f with
  | zero => intros; rfl
  | succ f ih =>
    intro i first fs
    simp only [Json.topMembers, refTopMembers, ih, Json.recordField, refRecordGo_eq]

def refParseTop (a : ByteArray) (lo hi : Nat) (keys : Array ByteArray) : Json.Top :=
  let hi := min hi a.size
  let i := Json.skipWs a hi lo
  let fuel := hi - lo + 1
  if i < hi && Json.byte a i == 0x7b then
    match refTopMembers a hi keys (Json.skipWs a hi (i + 1)) true (Array.replicate keys.size ⟨.absent, 0, 0⟩) fuel with
    | none => .malformed
    | some (j, fs) => if Json.skipWs a hi j == hi then .object fs else .malformed
  else
    match Json.scanValue a hi i fuel with
    | none => .malformed
    | some j => if Json.skipWs a hi j == hi then .notObject else .malformed

example : @Json.parseTop = @refParseTop := by
  funext a lo hi keys
  simp only [Json.parseTop, refParseTop, refTopMembers_eq]
  rfl

example : @Hmac.mac = @refMac := rfl
example : @Jwt.b64Encode = @refB64Encode := rfl
example : @Jwt.b64Decode = @refB64Decode := rfl

/-! ## Random inputs -/

structure Rng where
  s : UInt64

def Rng.next (g : Rng) : UInt64 × Rng :=
  let x := g.s ^^^ (g.s <<< 13)
  let x := x ^^^ (x >>> 7)
  let x := x ^^^ (x <<< 17)
  (x, ⟨x⟩)

def Rng.below (g : Rng) (n : Nat) : Nat × Rng :=
  let (x, g) := g.next
  (x.toNat % (max n 1), g)

/-- A byte, often one that JSON escapes or HTTP framing looks for. -/
def Rng.byte (g : Rng) : UInt8 × Rng :=
  let (k, g) := g.below 8
  let (x, g) := g.next
  let special : Array UInt8 := #[0x22, 0x5c, 0x00, 0x08, 0x0c, 0x0a, 0x0d, 0x09, 0x1f, 0x7f, 0x80, 0xff, 0x2e, 0x3a, 0x20]
  if k < 3 then (special[x.toNat % special.size]!, g) else (x.toUInt8, g)

def Rng.bytes (g : Rng) (maxLen : Nat) : ByteArray × Rng := Id.run do
  let (n, g0) := g.below (maxLen + 1)
  let mut g := g0
  -- spare capacity varies, so that the C side's reads past the size are exercised
  let (spare, g1) := g.below 64
  g := g1
  let mut a := ByteArray.emptyWithCapacity (n + spare)
  for _ in [0:n] do
    let (b, g') := g.byte
    a := a.push b
    g := g'
  return (a, g)

/-- Bytes of which few need escaping (about one in a hundred): long plain runs, which the leaves
copy 32 bytes at a time. -/
def Rng.plainBytes (g : Rng) (maxLen : Nat) : ByteArray × Rng := Id.run do
  let (n, g0) := g.below (maxLen + 1)
  let mut g := g0
  let (spare, g1) := g.below 64
  g := g1
  let mut a := ByteArray.emptyWithCapacity (n + spare)
  for _ in [0:n] do
    let (k, g2) := g.below 64
    let (x, g3) := g2.next
    let (b, g4) := g3.byte
    a := a.push (if k == 0 then b else x.toUInt8 ||| 0x20)
    g := g4
  return (a, g)

/-- A range bound: usually inside the array, sometimes past it. -/
def Rng.bound (g : Rng) (size : Nat) : Nat × Rng :=
  let (k, g) := g.below 10
  if k == 0 then g.below (size + 100) else g.below (size + 1)

def Rng.nat (g : Rng) : Nat × Rng :=
  let (k, g) := g.below 6
  let (x, g) := g.next
  let (y, g) := g.next
  if k == 0 then (x.toNat * 2 ^ 64 + y.toNat, g)       -- beyond 64 bits
  else if k == 1 then (x.toNat, g)                    -- up to 64 bits
  else if k == 2 then ((x % 10).toNat, g)
  else ((x % 1000000).toNat, g)

/-! ## The checks -/

/-- A copy of `a` that the leaf may update in place, with `spare` bytes of room. -/
def fresh (a : ByteArray) (spare : Nat) : ByteArray :=
  a.foldl (fun b x => b.push x) (ByteArray.emptyWithCapacity (a.size + spare))

def checkLeaves (rounds : Nat) : Except String Unit := Id.run do
  let mut g : Rng := ⟨0x9e3779b97f4a7c15⟩
  for i in [0:rounds] do
    let (out, g1) := g.bytes 40
    let (src, g2) := if i % 2 == 0 then g1.bytes 300 else g1.plainBytes 300
    let (lit, g3) := g2.bytes 20
    let (lo, g4) := g3.bound src.size
    let (hi, g5) := g4.bound src.size
    let (n, g6) := g5.nat
    let (b, g7) := g6.byte
    -- room in `out`: usually little, sometimes enough for a leaf to write in place
    let (big, g7) := g7.below 4
    let (spare, g8) := g7.below (if big == 0 then 2400 else 80)
    g := g8
    if appendEsc (fresh out spare) src lo hi != refAppendEsc out src lo hi then
      return .error s!"appendEsc differs (round {i})"
    if appendLitEsc (fresh out spare) lit src lo hi != refAppendLitEsc out lit src lo hi then
      return .error s!"appendLitEsc differs (round {i})"
    if appendNat (fresh out spare) n != refAppendNat out n then
      return .error s!"appendNat differs (round {i}, n = {n})"
    if appendLitNat (fresh out spare) lit n != refAppendLitNat out lit n then
      return .error s!"appendLitNat differs (round {i})"
    if appendSlice (fresh out spare) src lo hi != refAppendSlice out src lo hi then
      return .error s!"appendSlice differs (round {i})"
    if appendAll (fresh out spare) src != refAppendAll out src then
      return .error s!"appendAll differs (round {i})"
    if findByte src b lo hi != refFindByte src b lo hi then
      return .error s!"findByte differs (round {i})"
    if findHeadEnd src lo hi != refFindHeadEnd src lo hi then
      return .error s!"findHeadEnd differs (round {i})"
    if (clear (fresh out spare)).size != 0 then
      return .error s!"clear differs (round {i})"
    let (c, g9) := g.bound src.size
    let (names, g10) := if i % 4 < 2 then g9.bytes 40 else g9.plainBytes 40
    let (ns, g11) := g10.bound names.size
    let (ne, g12) := g11.bound names.size
    let (likes, g13) := g12.nat
    g := g13
    if postJsonAfter (fresh out spare) lit n src lo c hi names ns ne likes != refPostJsonAfter out lit n src lo c hi names ns ne likes then
      return .error s!"postJsonAfter differs (round {i})"
  return .ok ()

/-- A range of an array of `size` bytes: usually in order inside it, sometimes anywhere (empty,
reversed, past the end, beyond 64 bits). -/
def Rng.range (g : Rng) (size : Nat) : (Nat × Nat) × Rng :=
  let (k, g) := g.below 8
  let (lo, g) := g.bound size
  let (len, g) := g.below (if k == 0 then 600 else 120)
  let (hi, g) := g.bound size
  let (x, g) := g.next
  if k == 1 then ((lo, hi), g)
  else if k == 2 then ((2 ^ 64 + x.toNat, lo), g)
  else if k == 3 then ((lo, 2 ^ 64 + x.toNat), g)
  else ((lo, lo + len), g)

/-- A random page: rows of ranges into the blob (some of them adjacent, as the storage layer lays
them out), found flags mostly 1, numbers of every size, sometimes columns past the last row. The
blob is mostly text without bytes to escape, so both of the leaf's paths run. -/
def Rng.page (g : Rng) : Page × Rng := Id.run do
  let (kind, g) := g.below 4
  let (blob, g) := if kind == 0 then g.bytes 900 else g.plainBytes 900
  -- kinds 2 and 3: no byte to escape, or exactly one, anywhere
  let blob := if kind ≥ 2 then blob.data.foldl (fun a x =>
    a.push (if x == 0x22 || x == 0x5c || x < 0x20 then 0x61 else x)) (ByteArray.emptyWithCapacity (blob.size + blob.size % 37))
    else blob
  let (pos, g) := g.below blob.size
  let (esc, g) := g.below 32
  let blob := if kind == 3 && blob.size > 0 then blob.set! pos (if esc == 0 then 0x22 else if esc == 1 then 0x5c else esc.toUInt8) else blob
  let (rows, g) := g.below 30
  let mut g := g
  let mut cols : Array Nat := #[]
  for _ in [0:rows] do
    let (id, g1) := g.nat
    let ((b, c), g2) := g1.range blob.size
    let (elen, g3) := g2.below 30
    let (f, g4) := g3.below 12
    let (big, g5) := g4.nat
    let ((s, t), g6) := g5.range blob.size
    let (likes, g7) := g6.nat
    g := g7
    let found := if f < 9 then 1 else if f == 9 then 0 else if f == 10 then 2 else 2 ^ 64 + big
    cols := cols ++ #[id, b, c, c + elen, found, s, t, likes]
  let (extra, g8) := g.below 16
  g := g8
  if extra < 8 then
    for j in [0:extra] do cols := cols.push (j + 1)
  return (⟨blob, cols⟩, g)

/-- The page leaf against its Lean definition. -/
def checkPages (rounds : Nat) : Except String Unit := Id.run do
  let mut g : Rng := ⟨0x5851f42d4c957f2d⟩
  for i in [0:rounds] do
    let (pg, g1) := g.page
    let (body, g2) := g1.bytes 40
    let (big, g3) := g2.below 4
    let (spare, g4) := g3.below (if big == 0 then 9000 else 100)
    g := g4
    let want := refFeedFromPage body pg
    if feedFromPage (fresh body spare) pg != want then
      return .error s!"feedFromPage differs (round {i})"
    -- a shared body: the leaf must not write into it
    if feedFromPage body pg != want then
      return .error s!"feedFromPage differs on a shared body (round {i})"
  return .ok ()

/-! ### Responses written in place -/

def refRespond (out : ByteArray) (status : Nat) (body : ByteArray) (close : Bool) : ByteArray :=
  let out := if out.size == 0 then ByteArray.emptyWithCapacity (body.size + 160) else out
  let out := refAppendAll out (statusLine status)
  let out := refAppendNat out body.size
  let out := refAppendAll out (if close then endHeadClose else endHead)
  refAppendAll out body

example : @respond = @refRespond := rfl

theorem feedFromPage_eq_ref : @feedFromPage = @refFeedFromPage := by
  funext body pg
  simp only [feedFromPage, refFeedFromPage, renderPage, refRenderGo_eq]
  rfl

def refRespondFeed (out : ByteArray) (pg : Page) (close : Bool) : ByteArray :=
  refRespond out 200 (refFeedFromPage ByteArray.empty pg).1 close

example : @respondFeed = @refRespondFeed := by
  funext out pg close
  simp only [respondFeed, refRespondFeed, feedFromPage_eq_ref]
  rfl

def refAuthored (pg : Page) (limit : Nat) : Nat := Page.authored.go pg limit 0 (pg.cols.size / 8) 0

example : @Page.authored = @refAuthored := rfl

/-- A page as the storage layer returns them: no byte of the text to escape, numbers below
`2^63`, found flags 0 or 1 (ranges may still lie anywhere). -/
def tamePage (pg : Page) : Page :=
  ⟨pg.blob.data.foldl (fun a x => a.push (if x == 0x22 || x == 0x5c || x < 0x20 then 0x61 else x))
      (ByteArray.emptyWithCapacity (pg.blob.size + pg.blob.size % 37)),
    (pg.cols.zipIdx).map fun (x, j) => if j % 8 == 4 then (if x == 1 then 1 else 0) else x % 2 ^ 63⟩

/-- The feed's response and the count of rows with an author against their Lean definitions, on
random pages (half of them tame), appended to outputs that are empty, shared or not, with room or
without. -/
def checkFeedResponses (rounds : Nat) : Except String Unit := Id.run do
  let mut g : Rng := ⟨0x2b992ddfa23249d6⟩
  for i in [0:rounds] do
    let (pg, g1) := g.page
    let pg := if i % 2 == 0 then tamePage pg else pg
    let (out, g2) := g1.bytes 40
    let (kind, g3) := g2.below 4
    let (spare, g4) := g3.below (if kind == 0 then 9000 else 100)
    let (cl, g5) := g4.below 2
    let (lim, g6) := g5.below 26
    g := g6
    let out := if kind == 1 then ByteArray.empty else out
    let close := cl == 1
    let want := refRespondFeed out pg close
    if respondFeed (fresh out spare) pg close != want then
      return .error s!"respondFeed differs (round {i})"
    -- a shared output: the leaf must not write into it
    let keep := fresh out 0
    if respondFeed out pg close != want then
      return .error s!"respondFeed differs on a shared output (round {i})"
    if out != keep then
      return .error s!"respondFeed wrote into a shared output (round {i})"
    let lim := if lim == 25 then 2 ^ 64 + i else lim
    if pg.authored lim != refAuthored pg lim then
      return .error s!"Page.authored differs (round {i})"
  return .ok ()

def refPostFromPage (scratch : ByteArray) (id : Nat) (pg : Page) : Bool × ByteArray :=
  if pg.cols.size < 8 || pg.cols[4]! != 1 then (false, scratch) else
  let out := refAppendAll ByteArray.empty kPostWrap
  let out := refPostJsonAfter out kId id pg.blob pg.cols[1]! pg.cols[2]! pg.cols[3]! pg.blob pg.cols[5]! pg.cols[6]!
    pg.cols[7]!
  (true, refAppendAll out kWrapEnd)

example : @postFromPage = @refPostFromPage := rfl

def refRespondPost (out : ByteArray) (id : Nat) (pg : Page) (close : Bool) : ByteArray :=
  refRespond out 200 (refPostFromPage ByteArray.empty id pg).2 close

example : @respondPost = @refRespondPost := rfl

/-- A post's response against its Lean definition, on random pages (half of them tame) and ids,
appended to outputs that are empty, shared or not, with room or without. -/
def checkPostResponses (rounds : Nat) : Except String Unit := Id.run do
  let mut g : Rng := ⟨0x7c3f1a9e5b2d4c81⟩
  for i in [0:rounds] do
    let (pg, g1) := g.page
    let pg := if i % 2 == 0 then tamePage pg else pg
    let (id, g2) := g1.nat
    let (out, g3) := g2.bytes 40
    let (kind, g4) := g3.below 4
    let (spare, g5) := g4.below (if kind == 0 then 2000 else 100)
    let (cl, g6) := g5.below 2
    g := g6
    let out := if kind == 1 then ByteArray.empty else out
    let close := cl == 1
    let want := refRespondPost out id pg close
    if respondPost (fresh out spare) id pg close != want then
      return .error s!"respondPost differs (round {i})"
    -- a shared output: the leaf must not write into it
    let keep := fresh out 0
    if respondPost out id pg close != want then
      return .error s!"respondPost differs on a shared output (round {i})"
    if out != keep then
      return .error s!"respondPost wrote into a shared output (round {i})"
  return .ok ()

/-! ### HTTP requests -/

def parseEq : Http.Parse → Http.Parse → Bool
  | .incomplete, .incomplete => true
  | .bad s, .bad t => s == t
  | .ok r, .ok q =>
    r.method == q.method && r.pathLo == q.pathLo && r.pathHi == q.pathHi && r.hasAuth == q.hasAuth &&
    r.authLo == q.authLo && r.authHi == q.authHi && r.bodyLo == q.bodyLo && r.bodyHi == q.bodyHi &&
    r.close == q.close && r.next == q.next
  | _, _ => false

def Rng.pick (g : Rng) (xs : Array String) : String × Rng :=
  let (k, g) := g.below xs.size
  (xs[k]!, g)

def reqMethods : Array String := #["GET", "GET", "POST", "POST", "PUT", "get", "", "GET ", "POSTS", "G"]
def reqTargets : Array String :=
  #["/feed", "/posts/12", "/posts", "/posts/7/like", "/health", "/x?y=1", "/a?", "?", "/a b", "", "/posts/1?x?y"]
def reqVersions : Array String :=
  #["HTTP/1.1", "HTTP/1.1", "HTTP/1.1", "HTTP/1.0", "HTTP/1.0", "HTTP/2", "http/1.1", "HTTP/1.1 ", " HTTP/1.1", "HTTP/1."]
def reqHeads : Array String := #[
  "Host: 10.0.0.1:3000", "User-Agent: k6/1.3.0 (https://k6.io/)", "Content-Length: 0", "Content-Length: 5",
  "content-length:12", "CONTENT-LENGTH:  3 \t", "Content-Length: 999999999999999", "Content-Length: 1234567890123456",
  "Content-Length: 1048577", "Content-Length: 1048576", "Content-Length:", "Content-Length: 5a", "Content-Length: -1",
  "Content-Length : 5", "Content-Length: 05", "Authorization: Bearer abc.def.ghi", "authorization:   tok  ",
  "AUTHORIZATION:", "Authorization:\t\t", "Connection: close", "Connection: keep-alive", "connection: Keep-Alive, Upgrade",
  "Connection: ,close ,", "Connection: closed", "Connection:  CLOSE\t,keep-alive", "Connection: ,,", "Connection:",
  "Transfer-Encoding: chunked", "transfer-encoding:", "NoColonLine", ": empty name", "X-Thing:\tvalue\t",
  "Content-Type: application/json", "X: a:b:c", "", " Content-Length: 5", "Connection : close"]
def reqSeps : Array String := #["\r\n", "\r\n", "\r\n", "\r\n", "\r\n", "\r\n", "\r\n", "\n", "\r", "\r\r\n"]
/-- Header lines that leave a request well formed. -/
def reqGoodHeads : Array String := #["Host: x", "Authorization: Bearer abc.def.ghi", "Connection: close", "Connection: keep-alive",
  "connection: Keep-Alive, Upgrade", "Connection: ,close ,", "authorization:   tok  ", "Content-Length: 3", "X-Thing:\tvalue\t"]
def reqSpecial : Array UInt8 := "\r\n :,?\t/HhCc0123456789".toUTF8.data

/-- A request-like text: mostly well formed, with the header lines and values the parser looks
at, sometimes broken by a few changed bytes or cut short; sometimes two in a row; sometimes a head
too large. -/
def Rng.request (g : Rng) : ByteArray × Rng := Id.run do
  let mut g := g
  let mut s := ""
  let (reqs, g0) := g.below 3
  -- half of the texts are made of the well-formed parts only
  let (wf, g0) := g0.below 2
  g := g0
  let pick (g : Rng) (all : Array String) (good : Nat) : String × Rng :=
    if wf == 0 then g.pick (all.extract 0 good) else g.pick all
  for _ in [0:reqs + 1] do
    let (m, g1) := pick g reqMethods 5
    let (t, g2) := pick g1 reqTargets 9
    let (v, g3) := pick g2 reqVersions 5
    let (sep, g4) := pick g3 reqSeps 7
    let (nh, g5) := g4.below 7
    g := g5
    s := s ++ m ++ " " ++ t ++ " " ++ v ++ sep
    for _ in [0:nh] do
      let (h, g1) := if wf == 0 then g.pick reqGoodHeads else g.pick reqHeads
      let (sep, g2) := pick g1 reqSeps 7
      s := s ++ h ++ sep
      g := g2
    let (endKind, g6) := g.below 10
    let (blen, g7) := g6.below 16
    g := g7
    if endKind != 0 then s := s ++ "\r\n"
    s := s ++ String.ofList (List.replicate blen 'b')
  let (big, g8) := g.below 60
  g := g8
  if big == 0 then s := "GET /feed HTTP/1.1\r\nX: " ++ String.ofList (List.replicate 16400 'a') ++ "\r\n\r\n"
  if big == 1 then s := "GET /feed HTTP/1.1\r\nX: " ++ String.ofList (List.replicate 16370 'a') ++ "\r\n\r\n"
  if big == 2 then s := String.ofList (List.replicate 16390 'a')
  let mut a := s.toUTF8
  -- a few changed bytes
  let (muts, g9) := g.below 4
  g := g9
  for _ in [0:if muts == 0 then 1 else if muts == 1 then 3 else 0] do
    let (p, g1) := g.below a.size
    let (k, g2) := g1.below 4
    let (x, g3) := g2.next
    g := g3
    if a.size > 0 then
      a := a.set! p (if k == 0 then x.toUInt8 else reqSpecial[x.toNat % reqSpecial.size]!)
  -- sometimes cut short
  let (cut, g10) := g.below 6
  let (at_, g11) := g10.below (a.size + 1)
  g := g11
  if cut == 0 then a := a.extract 0 at_
  return (a, g)

/-- `Http.parse` against its Lean definition, at the start, at the next request, inside, at and
past the end. -/
def checkHttp (rounds : Nat) : Except String Unit := Id.run do
  let mut g : Rng := ⟨0x14057b7ef767814f⟩
  for i in [0:rounds] do
    let (a, g1) := g.request
    let (p, g2) := g1.below (a.size + 1)
    g := g2
    let next := match refParse a 0 with
      | .ok r => r.next
      | _ => 0
    for pos in [0, next, p, a.size, a.size + 3, 2 ^ 64] do
      if !parseEq (Http.parse a pos) (refParse a pos) then
        return .error s!"Http.parse differs (round {i}, position {pos})"
  return .ok ()

/-- `Http.parse` on inputs with room after their bytes (the C search reads 32 bytes at a time
inside the allocation, past the end of the input, and must not see what lies there). -/
def checkHttpRoom (rounds : Nat) : Except String Unit := Id.run do
  let mut g : Rng := ⟨0x3c6ef372fe94f82b⟩
  for i in [0:rounds] do
    let (a, g1) := g.request
    let (spare, g2) := g1.below 80
    let (p, g3) := g2.below (a.size + 1)
    g := g3
    let b := fresh a spare
    let next := match refParse a 0 with
      | .ok r => r.next
      | _ => 0
    for pos in [0, next, p, a.size] do
      if !parseEq (Http.parse b pos) (refParse a pos) then
        return .error s!"Http.parse differs with room after the input (round {i}, position {pos})"
  return .ok ()

/-! ### JSON texts -/

def fieldEq (a b : Json.Field) : Bool := a.kind == b.kind && a.lo == b.lo && a.hi == b.hi

def topEq : Json.Top → Json.Top → Bool
  | .malformed, .malformed | .notObject, .notObject => true
  | .object fs, .object gs => fs.size == gs.size && (List.range fs.size).all fun k => fieldEq fs[k]! gs[k]!
  | _, _ => false

def bytesOf (l : List Nat) : ByteArray := ⟨(l.map Nat.toUInt8).toArray⟩

/-- Strings (with their quotes): escapes of every kind, surrogates paired and lone, UTF-8 valid
and not (overlong, encoded surrogates, past U+10FFFF, cut short, stray continuation bytes), and
texts that are not strings at all (a control byte, an unknown escape, bad hex). -/
def jsonStrs : Array ByteArray := #[
  "\"\"".toUTF8, "\"abc\"".toUTF8, "\"HS256\"".toUTF8, "\"a\\\"b\"".toUTF8, "\"\\\\\"".toUTF8,
  "\"\\/\\b\\f\\n\\r\\t\"".toUTF8, "\"\\u0041\\u00e9\\u4e2d\\u0000\"".toUTF8, "\"\\ud83d\\ude00\"".toUTF8,
  "\"\\ud83d\"".toUTF8, "\"\\ude00x\"".toUTF8, "\"\\ud83d\\u0041\"".toUTF8, "\"\\uD83D\\uDE00\"".toUTF8,
  "\"é中😀\"".toUTF8,
  "\"a long string with no escapes at all, longer than thirty-two bytes\"".toUTF8,
  "\"a long string with one escape \\n after more than thirty-two bytes of text\"".toUTF8,
  bytesOf [0x22, 0xc3, 0x28, 0x22], bytesOf [0x22, 0xe0, 0x80, 0x80, 0x22],
  bytesOf [0x22, 0xc0, 0xaf, 0x22], bytesOf [0x22, 0xed, 0xa0, 0x80, 0x22], bytesOf [0x22, 0xf4, 0x90, 0x80, 0x80, 0x22],
  bytesOf [0x22, 0xff, 0x22], bytesOf [0x22, 0xf0, 0x9f, 0x98, 0x22], bytesOf [0x22, 0x80, 0x41, 0x22],
  bytesOf [0x22, 0xe2, 0x82, 0xac, 0xf0, 0x9f, 0x98, 0x80, 0x22], bytesOf [0x22, 0x61, 0xe2, 0x82, 0x22],
  -- not strings: the last four
  "\"\\uZZZZ\"".toUTF8, "\"\\x\"".toUTF8, "\"\\u12\"".toUTF8, bytesOf [0x22, 0x09, 0x22]]

def jsonKeys : Array String := #["\"alg\"", "\"exp\"", "\"nbf\"", "\"sub\"", "\"username\"", "\"body\"", "\"\\u0061lg\"",
  "\"ex\\u0070\"", "\"bo\\u0064y\"", "\"user\\u006eame\"", "\"x\"", "\"\"", "\"alg \"", "\"ALG\"", "\"b\\\"ody\""]
/-- Numbers: the first thirteen valid. -/
def jsonNums : Array String := #["0", "-0", "12", "1.5", "1e10", "1E+5", "-1.2e-3", "1700000000", "17000000000000000000000",
  "1e400", "2.0E-2", "9", "-12.50e01", "01", "1.", ".5", "-", "1e", "1e+", "--1"]
def jsonWs : Array String := #["", "", "", " ", "\n", "\t ", "\r\n", "  "]

def Rng.wsBytes (g : Rng) : ByteArray × Rng := let (w, g) := g.pick jsonWs; (w.toUTF8, g)

/-- Scalars, empty containers and broken bits of JSON. -/
def Rng.jsonScalar (g : Rng) (clean : Bool) : ByteArray × Rng :=
  let (k, g) := g.below 8
  if k < 3 then let (n, g) := g.below (if clean then 13 else jsonNums.size); (jsonNums[n]!.toUTF8, g)
  else if k < 6 then let (x, g) := g.below (if clean then jsonStrs.size - 4 else jsonStrs.size); (jsonStrs[x]!, g)
  else if k == 6 then let (l, g) := g.below (if clean then 3 else 6); (#["true", "false", "null", "nul", "tru", "fals"][l]!.toUTF8, g)
  else let (l, g) := g.below (if clean then 4 else 11)
    (#["{}", "[]", "{ }", "[ ]", "{,}", "[,]", "[1,]", "{\"a\":}", "{\"a\" 1}", "}", "]"][l]!.toUTF8, g)

/-- A JSON-like value of nesting depth below `d`. -/
def Rng.json (g : Rng) (clean : Bool) : Nat → ByteArray × Rng
  | 0 => g.jsonScalar clean
  | d + 1 =>
    let (k, g) := g.below 10
    if k < 5 then g.jsonScalar clean
    else if k < 7 then Id.run do
      let (n, g0) := g.below 5
      let mut g := g0
      let mut out := "[".toUTF8
      for j in [0:n] do
        let (v, g1) := g.json clean d
        let (w1, g2) := g1.wsBytes
        let (w2, g3) := g2.wsBytes
        out := out ++ (if j == 0 then ByteArray.empty else ",".toUTF8) ++ w1 ++ v ++ w2
        g := g3
      return (out ++ "]".toUTF8, g)
    else Id.run do
      let (n, g0) := g.below 5
      let mut g := g0
      let mut out := "{".toUTF8
      for j in [0:n] do
        let (key, g1) := g.pick jsonKeys
        let (v, g2) := g1.json clean d
        let (w1, g3) := g2.wsBytes
        let (w2, g4) := g3.wsBytes
        out := out ++ (if j == 0 then ByteArray.empty else ",".toUTF8) ++ w1 ++ key.toUTF8 ++ w2 ++ ":".toUTF8 ++ w1 ++ v
        g := g4
      return (out ++ "}".toUTF8, g)

/-- A JSON text: mostly an object with wanted keys, sometimes another value, deep nesting, a few
changed bytes, or a cut. -/
def Rng.jsonText (g : Rng) : ByteArray × Rng := Id.run do
  let (k, g0) := g.below 20
  -- half of the texts have valid scalars only
  let (c, g0) := g0.below 2
  let clean := c == 0
  let mut g := g0
  let (w1, g1) := g.wsBytes
  let (w2, g2) := g1.wsBytes
  g := g2
  let (v, g3) :=
    if k == 0 then
      let (d, g') := g.below 3000
      (String.ofList (List.replicate d '[') ++ String.ofList (List.replicate d ']')).toUTF8 |> fun x => (x, g')
    else if k == 1 then
      let (d, g') := g.below 3000
      (String.ofList (List.replicate d '[') ++ "0" ++ String.ofList (List.replicate (d - d % 2) ']')).toUTF8 |> fun x => (x, g')
    else if k < 6 then g.json clean 3
    else Id.run do
      -- a top-level object, as in tokens and post bodies
      let (n, g0) := g.below 6
      let mut g := g0
      let mut out := "{".toUTF8
      for j in [0:n] do
        let (key, g1) := g.pick jsonKeys
        let (v, g2) := g1.json clean 2
        let (w, g3) := g2.wsBytes
        out := out ++ (if j == 0 then ByteArray.empty else ",".toUTF8) ++ w ++ key.toUTF8 ++ ":".toUTF8 ++ w ++ v
        g := g3
      return (out ++ "}".toUTF8, g)
  g := g3
  let mut a := w1 ++ v ++ w2
  let (m, g4) := g.below (if clean then 12 else 6)
  g := g4
  if m == 0 && a.size > 0 then
    let (p, g5) := g.below a.size
    let (x, g6) := g5.byte
    a := a.set! p x
    g := g6
  if m == 1 then
    let (p, g5) := g.below (a.size + 1)
    a := a.extract 0 p
    g := g5
  return (a, g)

def jsonKeySets : Array (Array ByteArray) := #[#[Jwt.keyAlg], #[Jwt.keyExp, Jwt.keyNbf, Jwt.keySub, Jwt.keyUsername],
  #[kBodyKey], #[], #["a".toUTF8, Jwt.keyAlg, Jwt.keyAlg, "x".toUTF8, Jwt.keyExp, Jwt.keyNbf, Jwt.keySub, Jwt.keyUsername,
    kBodyKey, "".toUTF8]]

/-- `Json.parseTop` and `Json.decodeString` against their Lean definitions: whole texts and
ranges inside and past them; every string the parser recorded and random ranges decoded. -/
def checkJson (rounds : Nat) : Except String Unit := Id.run do
  let mut g : Rng := ⟨0x1b873593cc9e2d51⟩
  for i in [0:rounds] do
    let (a, g1) := g.jsonText
    let (ks, g2) := g1.below jsonKeySets.size
    let (lo, g3) := g2.below (a.size + 3)
    let (hi, g4) := g3.below (a.size + 3)
    let (dlo, g5) := g4.below (a.size + 2)
    let (dhi, g6) := g5.below (a.size + 4)
    g := g6
    let keys := jsonKeySets[ks]!
    for (l, h) in [(0, a.size), (0, a.size + 5), (lo, hi), (lo, a.size), (2 ^ 64, a.size), (0, 2 ^ 64)] do
      let t := Json.parseTop a l h keys
      if !topEq t (refParseTop a l h keys) then
        return .error s!"Json.parseTop differs (round {i}, range {l} {h})"
      if let .object fs := t then
        for f in fs do
          if f.kind == .str && Json.decodeString a f.lo f.hi != refDecodeString a f.lo f.hi then
            return .error s!"Json.decodeString differs (round {i}, range {f.lo} {f.hi})"
    if Json.decodeString a dlo dhi != refDecodeString a dlo dhi then
      return .error s!"Json.decodeString differs (round {i}, range {dlo} {dhi})"
  return .ok ()

/-! ### Routes -/

def postIdEq : PostId → PostId → Bool
  | .ok m, .ok n => m == n
  | .invalid, .invalid => true
  | _, _ => false

def routeEq : Route → Route → Bool
  | .health, .health | .feed, .feed | .create, .create | .notFound, .notFound => true
  | .post i, .post j | .like i, .like j => postIdEq i j
  | _, _ => false

def routePaths : Array String := #["/feed", "/health", "/posts", "/posts/", "/posts/123", "/posts/1", "/posts/0", "/posts/007",
  "/posts/000", "/posts/12a", "/posts/-1", "/posts/1/like", "/posts/0/like", "/posts//like", "/posts/1/likes",
  "/posts/like", "/posts/12/like/", "/posts/1/2/like", "/posts/9223372036854775807", "/posts/9223372036854775808",
  "/posts/18446744073709551616/like", "/posts/12345678901234567890123", "/posts/00000000000000000000001",
  "/posts/00000000000000000000000/like", "/feed/", "/FEED", "/Posts/1", "", "/", "/posts/1/2", "/healthz", "/post/1/like"]

/-- `route` against its Lean definition: paths inside other bytes, with the range sometimes
off by one, reversed or past the end, for every method. -/
def checkRoutes (rounds : Nat) : Except String Unit := Id.run do
  let mut g : Rng := ⟨0x3c6ef372fe94f82b⟩
  for i in [0:rounds] do
    let (path, g1) := g.pick routePaths
    let (pre, g2) := g1.bytes 6
    let (post, g3) := g2.bytes 6
    let (m, g4) := g3.below 3
    let (k, g5) := g4.below 12
    let (x, g6) := g5.below 4
    let (y, g7) := g6.next
    g := g7
    let p := path.toUTF8
    let a := pre ++ p ++ post
    let (lo, hi) :=
      if k == 0 then (pre.size + x, pre.size + p.size)
      else if k == 1 then (pre.size, pre.size + p.size + x)
      else if k == 2 then (pre.size + p.size, pre.size)
      else if k == 3 then (pre.size, 2 ^ 64 + y.toNat)
      else if k == 4 then (2 ^ 64 + y.toNat, pre.size + p.size)
      else if k == 5 then (y.toNat % (a.size + 2), y.toNat / 7 % (a.size + 2))
      else (pre.size, pre.size + p.size)
    let method : Http.Method := if m == 0 then .get else if m == 1 then .post else .other
    -- method, pathLo, pathHi, hasAuth, authLo, authHi, bodyLo, bodyHi, close, next
    let r : Http.Req := ⟨method, lo, hi, false, 0, 0, 0, 0, false, 0⟩
    if !routeEq (route a r) (refRoute a r) then
      return .error s!"route differs (round {i}, path {path})"
  return .ok ()

/-- Text that is mostly base64url, sometimes with other bytes or `=` padding. -/
def Rng.b64Text (g : Rng) (maxLen : Nat) : ByteArray × Rng := Id.run do
  let alphabet := "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_".toUTF8
  let (n, g0) := g.below (maxLen + 1)
  let mut g := g0
  let mut a := ByteArray.emptyWithCapacity (n + 2)
  for _ in [0:n] do
    let (k, g1) := g.below 50
    let (x, g2) := g1.next
    a := a.push (if k == 0 then x.toUInt8 else alphabet.get! (x.toNat % 64))
    g := g2
  let (pad, g3) := g.below 4
  if pad == 1 then a := a.push 0x3d
  if pad == 2 then a := (a.push 0x3d).push 0x3d
  return (a, g3)

/-- The JWT leaves (HMAC-SHA256, base64url) against their Lean definitions. -/
def checkCrypto (rounds : Nat) : Except String Unit := Id.run do
  let mut g : Rng := ⟨0x2545f4914f6cdd1d⟩
  for i in [0:rounds] do
    let (secret, g1) := g.bytes 140
    let (m, g2) := g1.bytes 300
    let (lo, g3) := g2.bound m.size
    let (hi, g4) := g3.bound m.size
    let (t, g5) := g4.b64Text 120
    let (tlo, g6) := g5.bound t.size
    let (thi, g7) := g6.bound t.size
    g := g7
    let key := Hmac.Key.of secret
    if Hmac.mac key m lo hi != refMac key m lo hi then
      return .error s!"Hmac.mac differs (round {i})"
    if Hmac.mac key m 0 m.size != refMac key m 0 m.size then
      return .error s!"Hmac.mac differs on a whole message (round {i})"
    if Jwt.b64Encode m != refB64Encode m then
      return .error s!"b64Encode differs (round {i})"
    if Jwt.b64Decode t tlo thi != refB64Decode t tlo thi then
      return .error s!"b64Decode differs (round {i})"
    if Jwt.b64Decode t 0 t.size != refB64Decode t 0 t.size then
      return .error s!"b64Decode differs on a whole text (round {i})"
  return .ok ()

def hex (b : ByteArray) : String :=
  b.foldl (fun s x => s ++ (if x < 16 then "0" else "") ++ String.ofList (Nat.toDigits 16 x.toNat)) ""

/-! ## The idle policy -/

def refPick (budget minIdle timeout n : Nat) (idle : Array Nat) (busy : Array Bool) : Array Bool :=
  (Idle.pickList ⟨budget, minIdle, timeout⟩ n (idle.toList.zip busy.toList)).toArray

example : @Idle.pick = @refPick := rfl

/-- A small number, or now and then one of up to 62 bits. -/
def Rng.idleNat (g : Rng) (small : Nat) : Nat × Rng :=
  let (k, g) := g.below 16
  let (x, g) := g.next
  if k == 0 then ((x >>> 2).toNat, g) else ((x.toNat % (max small 1)), g)

/-- `Idle.pick` against its Lean definition: candidates in the transport's order (idle times
falling, with ties) and in any order, some busy, around the budget and the idle thresholds, with
and without a timeout, and arrays of different lengths. -/
def checkIdle (rounds : Nat) : Except String Unit := Id.run do
  let mut g : Rng := ⟨0x2545f4914f6cdd1d⟩
  for i in [0:rounds] do
    let (len, g1) := g.below 48
    let (budget, g2) := g1.idleNat 64
    let (n, g3) := g2.idleNat 96
    let (minIdle, g4) := g3.idleNat 3000
    let (tk, g5) := g4.below 3
    let (t, g6) := g5.idleNat 6000
    let timeout := if tk == 0 then 0 else t
    let (sorted, g7) := g6.below 2
    let (top, g8) := g7.idleNat 8000
    g := g8
    let mut idle : Array Nat := #[]
    let mut busy : Array Bool := #[]
    let mut cur := top
    for _ in [0:len] do
      let (d, h1) := g.below 400
      let (x, h2) := h1.idleNat 8000
      let (b, h3) := h2.below 5
      g := h3
      if sorted == 0 then
        -- falling, with ties
        cur := cur - (if d < 100 then 0 else d)
        idle := idle.push cur
      else idle := idle.push x
      busy := busy.push (b == 0)
    -- now and then the arrays differ in length (the shorter one decides)
    let (cut, g9) := g.below 8
    g := g9
    if cut == 0 then busy := busy.pop
    if cut == 1 then idle := idle.pop
    if Idle.pick budget minIdle timeout n idle busy != refPick budget minIdle timeout n idle busy then
      return .error s!"Idle.pick differs (round {i})"
  return .ok ()

/-- FIPS 180-4 and RFC 4231 test vectors. -/
def checkSha : Except String Unit := do
  let rep (c : Char) (n : Nat) : ByteArray := (String.ofList (List.replicate n c)).toUTF8
  let cases : List (ByteArray × String) := [
    ("abc".toUTF8, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
    ("".toUTF8, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
    ("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq".toUTF8,
      "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"),
    ("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu".toUTF8,
      "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"),
    (rep 'a' 1000, "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3")]
  for (m, want) in cases do
    if hex (Sha256.hash m) != want then throw s!"SHA-256 differs on a {m.size}-byte message"
  let macs : List (ByteArray × ByteArray × String) := [
    ("Jefe".toUTF8, "what do ya want for nothing?".toUTF8,
      "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"),
    (⟨Array.replicate 131 0xaa⟩, "Test Using Larger Than Block-Size Key - Hash Key First".toUTF8,
      "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54")]
  for (k, m, want) in macs do
    if hex (Hmac.mac (Hmac.Key.of k) m 0 m.size) != want then throw "HMAC-SHA256 differs"

/-! ## Storage operations against the model -/

open Spec

@[extern "feed_db_create"] opaque dbCreate (path script : @& String) : IO Bool
@[extern "feed_db_open"] opaque dbOpen (path : @& String) : IO Unit
/-- The grouped rowid search of `runtime/db.c` against SQLite's own, on every key of `users`. -/
@[extern "feed_db_check_lookups"] opaque checkLookups : IO Bool
/-- The reads of `runtime/db.c` over the batch's pages against SQLite's cursors: the in-order pass
over both indexes, the like counts of every post id and of random windows of ids, the grouped rowid
search on `posts`; on the test database and, rolled back after, on one whose trees have three levels. -/
@[extern "feed_db_check_index"] opaque checkIndexes : IO Bool
/-- A rolled-back transaction that grew the file, through the page cache wrapper of `runtime/db.c`. -/
@[extern "feed_db_check_rollback"] opaque checkRollback : IO Bool
/-- `PRAGMA integrity_check` says ok. -/
@[extern "feed_db_integrity"] opaque integrityOk : IO Bool

def hexOf (l : List UInt8) : String :=
  String.join (l.map fun x => (if x < 16 then "0" else "") ++ String.ofList (Nat.toDigits 16 x.toNat))

def textLit (l : List UInt8) : String := s!"CAST(X'{hexOf l}' AS TEXT)"

def schema : String :=
  "PRAGMA journal_mode = WAL;
CREATE TABLE users (id INTEGER PRIMARY KEY, username TEXT NOT NULL UNIQUE,
  created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')));
CREATE TABLE posts (id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id),
  body TEXT NOT NULL CHECK (length(body) BETWEEN 1 AND 500),
  created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')));
CREATE TABLE likes (user_id INTEGER NOT NULL REFERENCES users(id), post_id INTEGER NOT NULL REFERENCES posts(id),
  created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')), PRIMARY KEY (user_id, post_id));
CREATE INDEX posts_created_at_id_idx ON posts (created_at DESC, id DESC);
CREATE INDEX posts_user_id_idx ON posts (user_id);
CREATE INDEX likes_post_id_idx ON likes (post_id);
"

def Rng.text (g : Rng) (lo hi : Nat) (alphabet : Array UInt8) : List UInt8 × Rng := Id.run do
  let (n, g0) := g.below (hi - lo + 1)
  let mut g := g0
  let mut out : List UInt8 := []
  for _ in [0:lo + n] do
    let (k, g') := g.below alphabet.size
    out := alphabet[k]! :: out
    g := g'
  return (out, g)

def bodyChars : Array UInt8 := "abc xyz\"\\\t/0189".toUTF8.data
/-- Few distinct timestamps, so that many posts tie on `created_at`. -/
def stamps : Array String := #["2025-01-01T00:00:00.000Z", "2025-06-30T12:00:00.000Z", "2025-12-31T23:59:59.999Z",
  "2026-01-01T00:00:00.000Z", "2025-06-30T12:00:00.001Z"]

/-- The number of user ids: enough users that the `users` table is a B-tree of three levels. -/
def nUserIds : Nat := 60000

/-- A random database: user ids with gaps, posts with ids out of `created_at` order and with ties,
posts whose author row is missing, likes. -/
def randomDb (g : Rng) : DB × Rng := Id.run do
  let mut g := g
  let mut users : List User := []
  for i in [1:nUserIds] do
    let (k, g') := g.below 5
    g := g'
    if k != 0 then users := ⟨i, Spec.lit s!"user_{i}"⟩ :: users
  users := users.reverse
  let mut posts : List Post := []
  let mut id := 1
  for _ in [0:400] do
    let (gap, g1) := g.below 4
    let (k, g2) := g1.below 3
    let (uid, g2) := if k == 0 then g2.below 130 else g2.below (nUserIds + 10)
    let (st, g3) := g2.below stamps.size
    let (body, g4) := g3.text 1 40 bodyChars
    g := g4
    id := id + (if gap == 0 then 3 else 1)
    posts := ⟨id, uid + 1, body, Spec.lit stamps[st]!⟩ :: posts
  posts := posts.reverse
  let mut likes : List Like := []
  for _ in [0:1500] do
    let (uid, g1) := g.below 130
    let (pk, g2) := g1.below posts.length
    g := g2
    let pid := posts[pk]!.id
    unless likes.any (fun l => l.userId == uid + 1 && l.postId == pid) do
      likes := likes ++ [⟨uid + 1, pid⟩]
  return (⟨users, posts, likes, Spec.lit "2026-01-01T00:00:00.000Z"⟩, g)

def script (db : DB) : String := Id.run do
  let mut s := schema ++ "BEGIN;\n"
  for u in db.users do
    s := s ++ s!"INSERT INTO users (id, username) VALUES ({u.id}, {textLit u.name});\n"
  for p in db.posts do
    s := s ++ s!"INSERT INTO posts (id, user_id, body, created_at) VALUES ({p.id}, {p.userId}, {textLit p.body}, {textLit p.createdAt});\n"
  for l in db.likes do
    s := s ++ s!"INSERT INTO likes (user_id, post_id) VALUES ({l.userId}, {l.postId});\n"
  return s ++ "COMMIT;\n"

/-- `Page.Same`: as many columns, and every row reads back as the same post object (the text may
lie in the blob in another order). By `feedFromPage_congr` and `postFromPage_congr` the handler
answers the same from either page. -/
def samePage (a b : Page) : Bool :=
  a.cols.size == b.cols.size && (List.range (a.cols.size / 8)).all fun k => a.view k == b.view k

def Rng.ids (g : Rng) (n bound : Nat) : Array Nat × Rng := Id.run do
  let mut g := g
  let mut out := #[]
  for _ in [0:n] do
    let (k, g1) := g.below 20
    let (x, g2) := g1.below bound
    g := g2
    out := out.push (if k == 0 then 2 ^ 70 + x else if k == 1 then 0 else x)
  return (out, g)

/-- Every read operation, on random arguments, in one read phase. -/
def checkReads (db : DB) (g : Rng) : IO Rng := do
  let mut g := g
  unless ← Store.begin (m := IO) do throw (IO.userError "begin failed")
  for n in [0, 1, 2, 5, 20, 21, 40, 64, 65, 100, 100000] do
    unless samePage (← Store.feedPage (m := IO) n) ((Store.feedPage (m := Model) n).run db).1 do
      throw (IO.userError s!"feedPage {n} differs from the model")
  let maxId := (db.posts.map (·.id)).foldl max 0
  let (ids, g1) := g.ids 600 (maxId + 10)
  g := g1
  for id in ids ++ #[0, 1, maxId, maxId + 1, 2 ^ 63 - 1, 2 ^ 63, 2 ^ 64] do
    unless samePage (← Store.postPage (m := IO) id) ((Store.postPage (m := Model) id).run db).1 do
      throw (IO.userError s!"postPage {id} differs from the model")
  Store.endReads (m := IO)
  unless ← Store.commit (m := IO) do throw (IO.userError "commit failed")
  return g

/-- Random writes in one write phase, each compared with the model; the model after them. -/
def checkWrites (db : DB) (g : Rng) : IO (DB × Rng) := do
  let mut g := g
  let mut db := db
  unless ← Store.begin (m := IO) do throw (IO.userError "begin failed")
  Store.endReads (m := IO)
  let maxId := (db.posts.map (·.id)).foldl max 0
  for _ in [0:60] do
    let (k, g1) := g.below 3
    let (uid, g2) := g1.below (nUserIds + 10)
    let (pid, g3) := g2.below (maxId + 20)
    let (body, g4) := g3.text 1 30 bodyChars
    g := g4
    if k == 0 then
      let got ← Store.postExists (m := IO) pid
      let (want, _) := (Store.postExists (m := Model) pid).run db
      unless got == want do throw (IO.userError s!"postExists {pid} differs from the model")
    else if k == 1 then
      let got ← Store.insertLike (m := IO) (uid + 1) pid
      let (want, db') := (Store.insertLike (m := Model) (uid + 1) pid).run db
      unless got == want do throw (IO.userError s!"insertLike {uid + 1} {pid} differs from the model")
      db := db'
    else
      let (created, id) ← Store.insertPost (m := IO) (uid + 1) ⟨body.toArray⟩
      -- the model's clock is SQLite's `now` of this insert
      let ((created', id'), db') := (Store.insertPost (m := Model) (uid + 1) ⟨body.toArray⟩).run { db with clock := bs created }
      unless id == id' && created.data == created'.data do
        throw (IO.userError s!"insertPost differs from the model (id {id} vs {id'})")
      db := db'
  unless ← Store.commit (m := IO) do throw (IO.userError "commit failed")
  return (db, g)

def checkStorage (path : String) : IO Unit := do
  let (db, g) := randomDb ⟨0x2545f4914f6cdd1d⟩
  unless ← dbCreate path (script db) do throw (IO.userError "could not create the test database")
  dbOpen path
  unless ← checkLookups do throw (IO.userError "the grouped rowid search differs from SQLite's")
  unless ← checkIndexes do throw (IO.userError "the index passes, like counts or rowid search differ from SQLite's cursors")
  unless ← checkRollback do throw (IO.userError "a rolled-back transaction left the database wrong")
  let mut g ← checkReads db g
  let mut db := db
  for _ in [0:5] do
    let (db', g') ← checkWrites db g
    db := db'
    g ← checkReads db g'
  unless ← integrityOk do throw (IO.userError "the database fails SQLite's integrity check after the writes")

def run (path : String) : IO UInt32 := do
  match checkLeaves 20000 with
  | .error e => IO.eprintln s!"selftest: {e}"; return 1
  | .ok () => pure ()
  match checkPages 20000 with
  | .error e => IO.eprintln s!"selftest: {e}"; return 1
  | .ok () => pure ()
  match checkFeedResponses 20000 with
  | .error e => IO.eprintln s!"selftest: {e}"; return 1
  | .ok () => pure ()
  match checkPostResponses 20000 with
  | .error e => IO.eprintln s!"selftest: {e}"; return 1
  | .ok () => pure ()
  match checkHttp 20000 with
  | .error e => IO.eprintln s!"selftest: {e}"; return 1
  | .ok () => pure ()
  match checkHttpRoom 20000 with
  | .error e => IO.eprintln s!"selftest: {e}"; return 1
  | .ok () => pure ()
  match checkRoutes 20000 with
  | .error e => IO.eprintln s!"selftest: {e}"; return 1
  | .ok () => pure ()
  match checkJson 20000 with
  | .error e => IO.eprintln s!"selftest: {e}"; return 1
  | .ok () => pure ()
  match checkSha with
  | .error e => IO.eprintln s!"selftest: {e}"; return 1
  | .ok () => pure ()
  match checkIdle 20000 with
  | .error e => IO.eprintln s!"selftest: {e}"; return 1
  | .ok () => pure ()
  match checkCrypto 20000 with
  | .error e => IO.eprintln s!"selftest: {e}"; return 1
  | .ok () => pure ()
  try checkStorage path
  catch e => IO.eprintln s!"selftest: {e}"; return 1
  IO.println "selftest: the C leaves (bytes, HMAC-SHA256, base64url, post objects, feed pages, HTTP framing, routes, JSON input, the idle policy) agree with their Lean definitions on 20000 random inputs; the storage operations with the model on a random database; the grouped rowid search and the index pass with SQLite's; a rollback and the integrity check; SHA-256 and HMAC with the test vectors"
  return 0

end Feed.Selftest

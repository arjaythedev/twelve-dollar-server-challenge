import Feed.Server
import Feed.Model

/-!
# Self-test

`bin/server --selftest <file>` (run by build.sh after linking) checks the C code the proofs take
as given:

* each leaf of `runtime/leaves.c` (byte operations, HMAC-SHA256, base64url, post objects) against
  its Lean definition, on random inputs;
* each storage operation of `runtime/db.c` against its meaning in `Feed/Model.lean`, on a random
  database written to `<file>` (ties in `created_at`, gaps in ids, posts whose author row is
  missing, a `users` table of three B-tree levels), before and after random writes; a page is
  compared row by row as the post objects it reads back as;
* the grouped rowid search of `runtime/db.c` against SQLite's own on every key of `users`;
* a transaction that grows the file and rolls back, and SQLite's integrity check after the writes;
* SHA-256 and HMAC-SHA256 against the published test vectors.
-/
namespace Feed.Selftest
open Bytes

/-! The references: the leaves' Lean bodies, restated without `@[extern]` so that they run as
Lean. Each is tied to its definition by `rfl`. -/

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
    let (src, g2) := g1.bytes 300
    let (lit, g3) := g2.bytes 20
    let (lo, g4) := g3.bound src.size
    let (hi, g5) := g4.bound src.size
    let (n, g6) := g5.nat
    let (b, g7) := g6.byte
    let (spare, g8) := g7.below 80
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
    let (names, g10) := g9.bytes 40
    let (ns, g11) := g10.bound names.size
    let (ne, g12) := g11.bound names.size
    let (likes, g13) := g12.nat
    g := g13
    if postJsonAfter (fresh out spare) lit n src lo c hi names ns ne likes != refPostJsonAfter out lit n src lo c hi names ns ne likes then
      return .error s!"postJsonAfter differs (round {i})"
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
  match checkSha with
  | .error e => IO.eprintln s!"selftest: {e}"; return 1
  | .ok () => pure ()
  match checkCrypto 20000 with
  | .error e => IO.eprintln s!"selftest: {e}"; return 1
  | .ok () => pure ()
  try checkStorage path
  catch e => IO.eprintln s!"selftest: {e}"; return 1
  IO.println "selftest: the C leaves (bytes, HMAC-SHA256, base64url, post objects) agree with their Lean definitions on 20000 random inputs; the storage operations with the model on a random database; the grouped rowid search with SQLite's; a rollback and the integrity check; SHA-256 and HMAC with the test vectors"
  return 0

end Feed.Selftest

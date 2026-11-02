import Feed.Bytes

/-!
# HTTP/1.1 framing

Requests are parsed in place: a request is a set of byte ranges of the connection's input.
Only what the API needs is extracted: the method, the path (the query string is ignored), the
`Authorization` value, the body (by `Content-Length`; chunked bodies are refused) and whether the
connection closes after the response.
-/
namespace Feed.Http

def maxHead : Nat := 16384
def maxBody : Nat := 1048576

inductive Method where
  | get | post | other
  deriving BEq, Inhabited

structure Req where
  method : Method
  pathLo : Nat
  pathHi : Nat
  hasAuth : Bool
  authLo : Nat
  authHi : Nat
  bodyLo : Nat
  bodyHi : Nat
  /-- close the connection after answering -/
  close : Bool
  /-- where the next request starts -/
  next : Nat
  deriving Inhabited

inductive Parse where
  /-- more bytes are needed -/
  | incomplete
  /-- the bytes are not a request this server can answer; reply with this status and close -/
  | bad (status : Nat)
  | ok (r : Req)
  deriving Inhabited

@[inline] def byte (a : ByteArray) (i : Nat) : UInt8 := a.get! i

/-- ASCII lowercase. -/
@[inline] def lower (b : UInt8) : UInt8 := if 0x41 ≤ b && b ≤ 0x5a then b + 32 else b

/-- Whether `a[lo, hi)` equals `lit`, ignoring ASCII case (`lit` is lowercase). -/
def eqCI (a : ByteArray) (lo hi : Nat) (lit : ByteArray) : Bool :=
  hi - lo == lit.size && go 0 lit.size
where
  go (k : Nat) : Nat → Bool
    | 0 => true
    | f + 1 => lower (byte a (lo + k)) == lit.get! k && go (k + 1) f

/-- Whether `a[lo, hi)` equals `lit` exactly. -/
def eqAt (a : ByteArray) (lo hi : Nat) (lit : ByteArray) : Bool :=
  hi - lo == lit.size && go 0 lit.size
where
  go (k : Nat) : Nat → Bool
    | 0 => true
    | f + 1 => byte a (lo + k) == lit.get! k && go (k + 1) f

/-- A decimal `Content-Length` value. -/
def parseLen (a : ByteArray) (lo hi : Nat) : Option Nat :=
  if lo ≥ hi || hi - lo > 15 then none else go lo (hi - lo) 0
where
  go (i : Nat) : Nat → Nat → Option Nat
    | 0, acc => some acc
    | f + 1, acc =>
      let c := byte a i
      if 0x30 ≤ c && c ≤ 0x39 then go (i + 1) f (acc * 10 + (c - 0x30).toNat) else none

def hContentLength : ByteArray := "content-length".toUTF8
def hAuthorization : ByteArray := "authorization".toUTF8
def hConnection : ByteArray := "connection".toUTF8
def hTransferEncoding : ByteArray := "transfer-encoding".toUTF8
def tClose : ByteArray := "close".toUTF8
def tKeepAlive : ByteArray := "keep-alive".toUTF8
def mGet : ByteArray := "GET".toUTF8
def mPost : ByteArray := "POST".toUTF8
def vHttp11 : ByteArray := "HTTP/1.1".toUTF8
def vHttp10 : ByteArray := "HTTP/1.0".toUTF8

/-- What the header lines `a[i, he)` say: content length (if any; a conflict is an error),
authorization span, `Connection` tokens, and whether a transfer encoding was given. -/
structure Heads where
  len : Option Nat := none
  lenBad : Bool := false
  hasAuth : Bool := false
  authLo : Nat := 0
  authHi : Nat := 0
  close : Bool := false
  keepAlive : Bool := false
  te : Bool := false
  bad : Bool := false

/-- Skip spaces and tabs forward from `s` (not past `e`). -/
def skipL (a : ByteArray) (s e : Nat) : Nat := go s (e - s)
where
  go (s : Nat) : Nat → Nat
    | 0 => s
    | f + 1 => if byte a s == 0x20 || byte a s == 0x09 then go (s + 1) f else s

/-- Skip spaces and tabs backward from `e` (not before `s`). -/
def skipR (a : ByteArray) (s e : Nat) : Nat := go e (e - s)
where
  go (e : Nat) : Nat → Nat
    | 0 => e
    | f + 1 => if byte a (e - 1) == 0x20 || byte a (e - 1) == 0x09 then go (e - 1) f else e

/-- Whether the comma-separated header value `a[lo, hi)` has the token `tok` (lowercase). -/
def hasToken (a : ByteArray) (lo hi : Nat) (tok : ByteArray) : Bool :=
  go lo (hi - lo + 1)
where
  go (i : Nat) : Nat → Bool
    | 0 => false
    | f + 1 =>
      if i ≥ hi then false else
      let e := Bytes.findByte a 0x2c i hi
      let s := skipL a i e
      eqCI a s (skipR a s e) tok || go (e + 1) f

def scanHeads (a : ByteArray) (he : Nat) (i : Nat) (h : Heads) : Nat → Heads
  | 0 => h
  | f + 1 =>
    if i ≥ he then h else
    let le := Bytes.findByte a 0x0d i he
    let colon := Bytes.findByte a 0x3a i le
    let h :=
      if colon ≥ le || colon == i then { h with bad := true } else
      -- value: after ':' and optional whitespace, trailing whitespace dropped
      let vs := skipL a (colon + 1) le
      let ve := skipR a vs le
      let nameLo := i
      let nameHi := colon
      if eqCI a nameLo nameHi hContentLength then
        match parseLen a vs ve with
        | none => { h with lenBad := true }
        | some n => match h.len with
          | none => { h with len := some n }
          | some m => if m == n then h else { h with lenBad := true }
      else if eqCI a nameLo nameHi hAuthorization then { h with hasAuth := true, authLo := vs, authHi := ve }
      else if eqCI a nameLo nameHi hConnection then
        { h with close := h.close || hasToken a vs ve tClose, keepAlive := h.keepAlive || hasToken a vs ve tKeepAlive }
      else if eqCI a nameLo nameHi hTransferEncoding then { h with te := true }
      else h
    scanHeads a he (le + 2) h f

/-- Parse the request starting at `pos`. -/
def parse (a : ByteArray) (pos : Nat) : Parse :=
  let n := a.size
  let he := Bytes.findHeadEnd a pos n
  if he ≥ n then (if n - pos > maxHead then .bad 431 else .incomplete) else
  if he - pos > maxHead then .bad 431 else
  -- request line: METHOD SP TARGET SP VERSION
  let le := Bytes.findByte a 0x0d pos he
  let sp1 := Bytes.findByte a 0x20 pos le
  let sp2 := Bytes.findByte a 0x20 (sp1 + 1) le
  if sp1 ≥ le || sp2 ≥ le || sp1 == pos then .bad 400 else
  let v11 := eqAt a (sp2 + 1) le vHttp11
  let v10 := eqAt a (sp2 + 1) le vHttp10
  if !(v11 || v10) then .bad 400 else
  let method := if eqAt a pos sp1 mGet then Method.get else if eqAt a pos sp1 mPost then Method.post else Method.other
  let q := Bytes.findByte a 0x3f (sp1 + 1) sp2
  let h := if le == he then {} else scanHeads a he (le + 2) {} (he - le)
  if h.bad || h.lenBad || h.te then .bad 400 else
  let len := h.len.getD 0
  if len > maxBody then .bad 413 else
  let bodyLo := he + 4
  if bodyLo + len > n then .incomplete else
  let close := if v11 then h.close else !h.keepAlive
  .ok { method, pathLo := sp1 + 1, pathHi := q, hasAuth := h.hasAuth, authLo := h.authLo, authHi := h.authHi,
        bodyLo, bodyHi := bodyLo + len, close, next := bodyLo + len }

end Feed.Http

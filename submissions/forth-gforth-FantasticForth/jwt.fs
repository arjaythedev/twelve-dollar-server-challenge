\ ============================================================================
\ jwt.fs - JWT HS256 verification for the $12 Server Challenge feed API.
\ Depends on ffi.fs being loaded first (uses HMAC and EVP_sha256).
\ All words are prefixed jwt- to avoid collisions. Uses gforth locals.
\ ============================================================================

\ --- status constants ----------------------------------------------------
0 constant JWT-OK
1 constant JWT-BAD-TOKEN
2 constant JWT-BAD-PAYLOAD

\ --- owned buffers --------------------------------------------------------
create jwt-secret-buf   512 allot
variable jwt-secret-len  0 jwt-secret-len !

create jwt-hdr-buf      512 allot   variable jwt-hdr-len
create jwt-pay-buf     2048 allot   variable jwt-pay-len
create jwt-sig-buf       64 allot   variable jwt-sig-len
create jwt-mac-buf       64 allot
create jwt-maclen-buf     8 allot
create jwt-name-buf     300 allot

\ --- secret loading -------------------------------------------------------
: jwt-load-secret ( -- )
  s" JWT_SECRET" getenv            ( addr u )
  dup 0= if
    2drop s" twelve-dollar-challenge"
  then
  dup 511 > if 511 min then
  dup jwt-secret-len !
  jwt-secret-buf swap move ;

\ ==========================================================================
\ base64url decode
\ ==========================================================================
: jwt-b64val ( c -- n )
  dup [char] A >= over [char] Z <= and if [char] A - exit then
  dup [char] a >= over [char] z <= and if [char] a - 26 + exit then
  dup [char] 0 >= over [char] 9 <= and if [char] 0 - 52 + exit then
  dup [char] - = if drop 62 exit then
  dup [char] _ = if drop 63 exit then
  dup [char] + = if drop 62 exit then
  dup [char] / = if drop 63 exit then
  drop -1 ;

\ ( src u dst -- outlen ok )
: jwt-b64url-decode { src u dst -- outlen ok }
  0 { acc }  0 { bits }  0 { n }
  u 0 ?do
    src i + c@ jwt-b64val { v }
    v 0< if 0 false unloop exit then
    acc 6 lshift v or to acc
    bits 6 + to bits
    bits 8 >= if
      bits 8 - to bits
      dst n + acc bits rshift 255 and swap c!
      n 1+ to n
      acc 1 bits lshift 1- and to acc
    then
  loop
  n true ;

\ ==========================================================================
\ helpers
\ ==========================================================================
\ constant-time-ish 32-byte compare ( a b -- flag )
: jwt-ct-eq32 { a b -- flag }
  0 { d }
  32 0 do  a i + c@  b i + c@  xor  d or to d  loop
  d 0= ;

\ substring presence ( hay hu needle nu -- flag )
: jwt-has? search nip nip ;

\ HMAC-SHA256 over (data,u) with the loaded secret -> jwt-mac-buf (32 bytes)
\ ( data u -- )
: jwt-hmac { data u -- }
  EVP_sha256
  jwt-secret-buf jwt-secret-len @
  data u
  jwt-mac-buf jwt-maclen-buf
  HMAC drop ;

\ ==========================================================================
\ JSON string-value extraction (compact ASCII payloads only).
\ Find key as  "key":"  and return the value up to the next '"'.
\ ( json ju keyname keyu -- val-a val-u found? )
\ ==========================================================================
create jwt-needle-buf 128 allot
variable jwt-needle-len
\ build needle  "<key>":"  into jwt-needle-buf
: jwt-build-needle { ka ku -- }
  0 { o }
  jwt-needle-buf o + [char] " swap c!  o 1+ to o
  ku 0 ?do  jwt-needle-buf o + ka i + c@ swap c!  o 1+ to o  loop
  jwt-needle-buf o + [char] " swap c!  o 1+ to o
  jwt-needle-buf o + [char] : swap c!  o 1+ to o
  jwt-needle-buf o + [char] " swap c!  o 1+ to o
  o jwt-needle-len ! ;

: jwt-json-str { json ju ka ku -- val-a val-u found? }
  ka ku jwt-build-needle
  json ju jwt-needle-buf jwt-needle-len @ search  ( rest-a rest-u found )
  0= if 2drop 0 0 false exit then                 ( rest-a rest-u )
  jwt-needle-len @ /string                        ( va vmax )
  over { va }                                     ( va vmax )
  s\" \"" search                                  ( q-a q-u found2 )
  0= if 2drop 0 0 false exit then                 ( q-a q-u )
  drop                                            ( q-a )
  va -                                            ( val-u )
  va swap true ;                                  ( va val-u true )

\ positive-integer string test + parse ( a u -- n valid? )
: jwt-parse-uint { a u -- n valid? }
  u 0= if 0 false exit then
  0 { acc }
  u 0 ?do
    a i + c@ { c }
    c [char] 0 < c [char] 9 > or if 0 false unloop exit then
    acc 10 * c [char] 0 - + to acc
  loop
  acc dup 1 >= ;

\ ==========================================================================
\ token splitting using string locals
\ ==========================================================================
variable jwt-h-a variable jwt-h-u
variable jwt-p-a variable jwt-p-u
variable jwt-s-a variable jwt-s-u
variable jwt-hp-a variable jwt-hp-u

\ ( addr u -- ok )
: jwt-split3 { addr u -- ok }
  \ find first dot
  addr u s" ." search { f1 } { m1u } { m1a }
  f1 0= if false exit then
  m1a addr - { hlen }               \ header length
  addr jwt-h-a !  hlen jwt-h-u !
  \ remainder after first dot
  m1a 1+ { r1a }  u hlen - 1- { r1u }
  r1a r1u s" ." search { f2 } { m2u } { m2a }
  f2 0= if false exit then
  m2a r1a - { plen }
  r1a jwt-p-a !  plen jwt-p-u !
  m2a 1+ { sa }  r1u plen - 1- { su }
  \ signature must have no further dots
  sa su s" ." search nip nip if false exit then
  sa jwt-s-a !  su jwt-s-u !
  \ header.payload span (for HMAC), contiguous in original token
  addr jwt-hp-a !
  hlen 1+ plen + jwt-hp-u !
  true ;

\ extract integer value for a numeric key like "exp": ( json ju key ku -- n found? )
: jwt-json-int { json ju ka ku -- n found? }
  \ build needle  "key":
  0 { o }
  jwt-needle-buf o + [char] " swap c!  o 1+ to o
  ku 0 ?do jwt-needle-buf o + ka i + c@ swap c! o 1+ to o loop
  jwt-needle-buf o + [char] " swap c!  o 1+ to o
  jwt-needle-buf o + [char] : swap c!  o 1+ to o
  o jwt-needle-len !
  json ju jwt-needle-buf jwt-needle-len @ search  ( rest-a rest-u found )
  0= if 2drop 0 false exit then
  jwt-needle-len @ /string                        ( va vmax )
  { vmax } { va }
  0 { acc } 0 { cnt }
  vmax 0 ?do
    va i + c@ { c }
    c [char] 0 >= c [char] 9 <= and if
      acc 10 * c [char] 0 - + to acc
      cnt 1+ to cnt
    else
      leave
    then
  loop
  cnt 0= if 0 false exit then
  acc true ;

\ current unix time in seconds (from utime microseconds)
: jwt-now ( -- u ) utime 1000000 um/mod nip ;

\ ==========================================================================
\ main verify entry
\ jwt-verify ( tok-addr tok-u -- uid name-addr name-u status )
\ ==========================================================================
: jwt-verify { tok-a tok-u -- uid name-a name-u status }
  jwt-load-secret
  tok-a tok-u jwt-split3 0= if 0 0 0 JWT-BAD-TOKEN exit then
  \ decode header
  jwt-h-a @ jwt-h-u @ jwt-hdr-buf jwt-b64url-decode
  0= if 0 0 0 JWT-BAD-TOKEN exit then jwt-hdr-len !
  \ decode payload
  jwt-p-a @ jwt-p-u @ jwt-pay-buf jwt-b64url-decode
  0= if 0 0 0 JWT-BAD-TOKEN exit then jwt-pay-len !
  \ decode signature
  jwt-s-a @ jwt-s-u @ jwt-sig-buf jwt-b64url-decode
  0= if 0 0 0 JWT-BAD-TOKEN exit then jwt-sig-len !
  \ alg must be HS256
  jwt-hdr-buf jwt-hdr-len @ s\" \"alg\":\"HS256\"" jwt-has?
  0= if 0 0 0 JWT-BAD-TOKEN exit then
  \ signature length exactly 32
  jwt-sig-len @ 32 <> if 0 0 0 JWT-BAD-TOKEN exit then
  \ recompute HMAC over header.payload and compare
  jwt-hp-a @ jwt-hp-u @ jwt-hmac
  jwt-mac-buf jwt-sig-buf jwt-ct-eq32 0= if 0 0 0 JWT-BAD-TOKEN exit then
  \ exp check
  jwt-pay-buf jwt-pay-len @ s" exp" jwt-json-int  ( expval found )
  0= if 0 0 0 JWT-BAD-TOKEN exit then             ( expval )
  jwt-now <= if 0 0 0 JWT-BAD-TOKEN exit then      \ exp <= now => expired
  \ ---- payload validation ----
  \ sub string
  jwt-pay-buf jwt-pay-len @ s" sub" jwt-json-str  ( va vu found )
  0= if 0 0 0 JWT-BAD-PAYLOAD exit then            ( va vu )
  jwt-parse-uint                                   ( uid valid )
  0= if 0 0 0 JWT-BAD-PAYLOAD exit then            ( uid )
  { uid }
  \ username string
  jwt-pay-buf jwt-pay-len @ s" username" jwt-json-str  ( na nu found )
  0= if 0 0 0 JWT-BAD-PAYLOAD exit then                 ( na nu )
  { nu } { na }
  \ copy username into owned buffer
  na jwt-name-buf nu move
  uid jwt-name-buf nu JWT-OK ;

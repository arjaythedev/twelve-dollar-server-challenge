\ json.fs - Compact JSON emitter + minimal request-body parser
\ "$12 Server Challenge" feed API - pure gforth 0.7.3, 64-bit, no FFI.
\ All words GLOBAL, prefixed json-.

\ ===== OUTPUT BUFFER =====
create json-buf 65536 allot
variable json-len

: json-reset ( -- ) 0 json-len ! ;

: json-emit ( addr u -- )
    dup 0= if 2drop exit then
    json-buf json-len @ +   ( addr u dest )
    swap dup >r move        ( ) ( r: u )
    json-len @ r> + json-len ! ;

: json-result ( -- addr u ) json-buf json-len @ ;

: json-c ( ch -- )
    json-buf json-len @ + c!
    json-len @ 1+ json-len ! ;

\ append a literal string constant. Immediate word: at compile time of the
\ enclosing definition, parse the following  ...\"  text (with C-style
\ escapes handled below) and compile it as an in-line string literal,
\ followed by a call to json-emit.
\ We parse up to the next double-quote, un-escaping the few sequences we use.
create json-slit-buf 4096 allot
variable json-slit-len
: json-slit-c ( ch -- )
    json-slit-buf json-slit-len @ + c!  json-slit-len @ 1+ json-slit-len ! ;
: json-slit-decode ( addr u -- addr2 u2 )
    0 json-slit-len !
    over + swap ( end cur )
    begin 2dup u> while
        dup c@ dup [char] \ = if
            drop 1+                      ( end cur+1 )
            dup c@
            dup [char] n = if drop 10 json-slit-c else
            dup [char] t = if drop 9  json-slit-c else
            dup [char] r = if drop 13 json-slit-c else
            dup [char] q = if drop [char] " json-slit-c else
            json-slit-c then then then then
        else json-slit-c then
        1+
    repeat
    2drop json-slit-buf json-slit-len @ ;
: json-s," ( "ccc<quote>" -- )
    [char] " parse          ( addr u )  \ parse up to next double-quote
    json-slit-decode        ( addr2 u2 )
    postpone sliteral
    postpone json-emit ; immediate

\ ===== HEX HELPER for \u00XX =====
create json-hexdigits  char 0 c, char 1 c, char 2 c, char 3 c,
                       char 4 c, char 5 c, char 6 c, char 7 c,
                       char 8 c, char 9 c, char a c, char b c,
                       char c c, char d c, char e c, char f c,

: json-hexnib ( n -- ch ) 15 and json-hexdigits + c@ ;

\ emit control char 0x00..0x1F as \u00XX (lowercase)
: json-ctrl-u ( ch -- )
    [char] \ json-c [char] u json-c
    [char] 0 json-c [char] 0 json-c
    dup 4 rshift json-hexnib json-c
    json-hexnib json-c ;

\ ===== JSON STRING ESCAPING =====
: json-str-char ( ch -- )
    dup [char] " = if drop [char] \ json-c [char] " json-c exit then
    dup [char] \ = if drop [char] \ json-c [char] \ json-c exit then
    dup 8  = if drop [char] \ json-c [char] b json-c exit then
    dup 9  = if drop [char] \ json-c [char] t json-c exit then
    dup 10 = if drop [char] \ json-c [char] n json-c exit then
    dup 12 = if drop [char] \ json-c [char] f json-c exit then
    dup 13 = if drop [char] \ json-c [char] r json-c exit then
    dup 0x20 u< if json-ctrl-u exit then
    json-c ;

\ does this byte need escaping?  " (34), \ (92), or control (<0x20)
: json-needs-esc? ( ch -- f )
    dup [char] " = if drop true exit then
    dup [char] \ = if drop true exit then
    0x20 u< ;

\ 256-entry lookup table: json-safe[c] = 1 if byte c can be copied verbatim,
\ 0 if it needs escaping. Built once at load time from json-needs-esc? so the
\ scan loop below is a single indexed load + branch per byte instead of three
\ comparisons through a word call. Pure speed; the escaping contract is
\ unchanged (the table IS json-needs-esc?, inverted).
create json-safe 256 allot
:noname 256 0 ?do i json-needs-esc? 0= 1 and json-safe i + c! loop ; execute

\ Fast path: copy a run of "safe" bytes [a,a+n) straight into json-buf with one
\ move instead of n per-byte primitive calls.
: json-emit-run ( a n -- )
    dup 0= if 2drop exit then
    json-buf json-len @ +  swap  dup >r  move  ( ) ( r: n )
    r> json-len +! ;

\ json-str: walk the string, bulk-copying maximal safe runs and only dropping
\ to per-char escaping when a byte actually needs it. For plain-ASCII payloads
\ (the load test) the whole body is a single move. Byte-for-byte identical
\ output to the per-char version.
\ length of the maximal run of safe (non-escaped) bytes in [a, a+max);
\ returns max if every byte in the range is safe. Uses the json-safe table
\ (single indexed load per byte) instead of a per-byte word call.
: json-run-len { a max -- runlen }
    max 0 ?do
        a i + c@ json-safe + c@ 0= if i unloop exit then
    loop
    max ;

: json-str ( addr u -- )
    [char] " json-c
    begin dup 0> while              ( a u )
        2dup json-run-len           ( a u runlen )
        dup >r                      ( a u runlen ) ( R: runlen )
        dup if
            >r over r> json-emit-run    ( a u ) ( R: runlen )  \ emit (a, runlen)
        else drop then              ( a u ) ( R: runlen )
        r>                          ( a u runlen )
        /string                     ( a' u' )   \ advance past the safe run
        dup 0> if                   ( a' u' )
            over c@ json-str-char   \ escape the one offending byte
            1 /string
        then
    repeat
    2drop
    [char] " json-c ;

\ ===== INTEGER =====
\ Direct unsigned->decimal writer. Avoids the pictured-numeric-output machinery
\ (<# #s #>) and the intermediate json-emit move: profiling showed <# #s #>
\ costs ~48 ns/digit of interpreted overhead. We generate digits least-
\ significant first into a 24-byte scratch (enough for 2^64), then copy the run
\ into json-buf with one move. Byte-for-byte identical output.
create json-num-buf 24 allot

\ Generate digits least-significant first into json-num-buf (filled backward
\ from the end), then copy the digit run into json-buf with one move.
\ p is a local pointer that walks backward from one-past-the-end of the buffer.
: json-uint ( u -- )
    dup 0= if drop [char] 0 json-c exit then
    json-num-buf 24 + { p }        \ p = one past the last byte
    { u }
    begin u while
        u 10 /mod to u             \ ( rem ) ; u := quotient
        [char] 0 +                 \ ( ch )
        p 1- to p                  \ p := p-1
        p c!                       \ store digit at p
    repeat
    p  json-num-buf 24 + over -    ( p len )   \ len = end - p
    json-buf json-len @ +  swap  dup >r  move  r> json-len +! ;

: json-int ( n -- )
    dup 0< if [char] - json-c negate then
    json-uint ;

\ ===== POST OBJECT =====
\ ( id-n body-addr body-u created-addr created-u author-addr author-u like-n -- )
: json-post ( id ba bu ca cu aa au like -- )
    >r >r >r >r >r >r >r         ( id ) ( r: like au aa cu ca bu ba )
    json-s," {\qid\q:"
    json-int
    json-s," ,\qbody\q:"
    r> r> json-str                ( r: like au aa cu ca -- ba bu )
    json-s," ,\qcreated_at\q:"
    r> r> json-str                ( r: like au aa -- ca cu )
    json-s," ,\qauthor\q:"
    r> r> json-str                ( r: like -- aa au )
    json-s," ,\qlike_count\q:"
    r> json-int
    [char] } json-c ;

: json-post-wrapped ( id ba bu ca cu aa au like -- )
    json-s," {\qpost\q:"
    json-post
    [char] } json-c ;

: json-feed-open ( -- )  json-s," {\qposts\q:[" ;
: json-feed-sep  ( -- )  [char] , json-c ;
: json-feed-close ( -- ) json-s," ]}" ;

: json-error ( addr u -- )
    json-s," {\qerror\q:"
    json-str
    [char] } json-c ;

: json-health-ok ( uptime-n -- )
    json-s," {\qstatus\q:\qok\q,\qdb\q:\qok\q,\quptime_s\q:"
    json-int
    [char] } json-c ;

: json-like ( already-bool post-id-n -- )
    swap >r                       ( id ) ( r: already )
    json-s," {\qliked\q:true,\qalready_liked\q:"
    r> if json-s," true" else json-s," false" then
    json-s," ,\qpost_id\q:"
    json-int
    [char] } json-c ;

\ ===== REQUEST BODY PARSER =====
0 constant JSON-OK
1 constant JSON-MALFORMED
2 constant JSON-NO-BODY

create json-body-buf 2048 allot
variable json-body-len

\ -- parse cursor state --
variable json-p      \ current pointer
variable json-end    \ one past last byte

: json-more? ( -- flag ) json-p @ json-end @ u< ;
: json-peek ( -- ch ) json-p @ c@ ;
: json-advance ( -- ) json-p @ 1+ json-p ! ;

: json-skip-ws ( -- )
    begin
        json-more? if
            json-peek dup bl = swap
            dup 9 = swap dup 10 = swap 13 = or or or
        else 0 then
    while json-advance repeat ;

: json-body-c ( ch -- )
    json-body-buf json-body-len @ + c!
    json-body-len @ 1+ json-body-len ! ;

\ append up to 4 UTF-8 bytes for a BMP codepoint
: json-utf8 ( cp -- )
    dup 0x80 u< if json-body-c exit then
    dup 0x800 u< if
        dup 6 rshift 0xC0 or json-body-c
        0x3F and 0x80 or json-body-c exit then
    \ 0x800 .. 0xFFFF
    dup 12 rshift 0xE0 or json-body-c
    dup 6 rshift 0x3F and 0x80 or json-body-c
    0x3F and 0x80 or json-body-c ;

\ read one hex digit at cursor, advance; returns ( val ok )
: json-hexval ( ch -- n ok )
    dup [char] 0 >= over [char] 9 <= and if [char] 0 - true exit then
    dup [char] a >= over [char] f <= and if [char] a - 10 + true exit then
    dup [char] A >= over [char] F <= and if [char] A - 10 + true exit then
    drop 0 false ;

\ parse \uXXXX (cursor just past the 'u'); returns codepoint + ok
: json-parse-u ( -- cp ok )
    0 ( acc )
    4 0 do
        json-more? 0= if unloop false exit then
        json-peek json-hexval 0= if drop unloop false exit then
        swap 4 lshift + json-advance
    loop
    true ;

\ Parse a JSON string starting AT the opening quote.
\ Decodes into json-body-buf. Returns ok flag. Leaves cursor past closing quote.
: json-parse-string ( -- ok )
    0 json-body-len !
    json-more? 0= if false exit then
    json-peek [char] " <> if false exit then
    json-advance
    begin
        json-more? 0= if false exit then
        json-peek
        dup [char] " = if drop json-advance true exit then
        dup [char] \ = if
            drop json-advance
            json-more? 0= if false exit then
            json-peek
            dup [char] " = if drop [char] " json-body-c json-advance 0 else
            dup [char] \ = if drop [char] \ json-body-c json-advance 0 else
            dup [char] / = if drop [char] / json-body-c json-advance 0 else
            dup [char] n = if drop 10 json-body-c json-advance 0 else
            dup [char] t = if drop 9  json-body-c json-advance 0 else
            dup [char] r = if drop 13 json-body-c json-advance 0 else
            dup [char] b = if drop 8  json-body-c json-advance 0 else
            dup [char] f = if drop 12 json-body-c json-advance 0 else
            dup [char] u = if drop json-advance
                            json-parse-u 0= if false exit then
                            json-utf8 0 else
            drop false exit
            then then then then then then then then then
            drop
        else
            json-body-c json-advance
        then
    again ;

\ Scan a non-string value (number/true/false/null) to its end; return
\ whether it was a string (always false here). We only need: value is NOT
\ a string -> JSON-NO-BODY. So we just verify input is object-ish.

\ Find top-level key "body" in an object. Pragmatic scanner.
\ Returns status. On OK, fills json-body-buf / json-body-len.
: json-parse-body ( addr u -- body-addr body-u status )
    over + json-end !            ( addr )  ( set end )
    json-p !                     ( )       ( set p )
    json-skip-ws
    json-more? 0= if 0 0 JSON-MALFORMED exit then
    json-peek [char] { <> if 0 0 JSON-MALFORMED exit then
    json-advance
    json-skip-ws
    \ empty object?
    json-more? if json-peek [char] } = if 0 0 JSON-NO-BODY exit then then
    \ iterate key:value pairs
    begin
        json-skip-ws
        json-more? 0= if 0 0 JSON-MALFORMED exit then
        json-peek [char] " <> if 0 0 JSON-MALFORMED exit then
        \ read key as raw string (no decode needed; compare simple)
        json-advance
        json-p @                  ( keystart )
        begin
            json-more? 0= if drop 0 0 JSON-MALFORMED exit then
            json-peek [char] " = if
                json-p @ over -   ( keystart keylen )
                true
            else
                json-advance false
            then
        until
        json-advance              \ past closing quote of key
        \ now expect : 
        json-skip-ws
        json-more? 0= if 2drop 0 0 JSON-MALFORMED exit then
        json-peek [char] : <> if 2drop 0 0 JSON-MALFORMED exit then
        json-advance
        json-skip-ws
        \ is this the "body" key?
        2dup s" body" compare 0= if   ( keystart keylen )
            2drop
            \ value must be a string
            json-more? 0= if 0 0 JSON-NO-BODY exit then
            json-peek [char] " <> if 0 0 JSON-NO-BODY exit then
            json-parse-string 0= if 0 0 JSON-MALFORMED exit then
            json-body-buf json-body-len @ JSON-OK exit
        then
        2drop
        \ not the body key: skip its value
        json-more? 0= if 0 0 JSON-MALFORMED exit then
        json-peek [char] " = if
            \ string value: skip it (reuse decoder into scratch)
            json-parse-string 0= if 0 0 JSON-MALFORMED exit then
        else
            \ scalar value: advance until , or } or whitespace
            begin
                json-more? if
                    json-peek dup [char] , <> swap
                    dup [char] } <> swap
                    dup bl <> swap dup 9 <> swap
                    dup 10 <> swap 13 <> and and and and
                else 0 then
            while json-advance repeat
        then
        json-skip-ws
        json-more? 0= if 0 0 JSON-MALFORMED exit then
        json-peek [char] , = if json-advance else
        json-peek [char] } = if 0 0 JSON-NO-BODY exit else
            0 0 JSON-MALFORMED exit
        then then
    again ;

\ ============================================================================
\ http.fs - HTTP/1.1 request handling: parsing, routing for the five endpoints,
\ the response builder, and the exact error contract. Integrates db.fs, jwt.fs,
\ json.fs. The connection/accept loop that drives these lives in epoll.fs.
\ Load order: ffi.fs, db.fs, jwt.fs, json.fs, then this file.
\ ============================================================================

\ ---------------------------------------------------------------------------
\ Config from environment
\ ---------------------------------------------------------------------------
create host-buf 64 allot   variable host-len
variable the-port
variable start-time

: getenv-or ( name-a name-u dflt-a dflt-u -- a u )
  2>r getenv dup 0= if 2drop 2r> else 2rdrop then ;

: now-s ( -- n ) utime 1000000 um/mod nip ;   \ unix seconds

: config-load ( -- )
  \ Default to DIRECT serving on 0.0.0.0:80 (SPEC's direct mode). The env still
  \ overrides: the test harness and a behind-Nginx deployment set HOST=127.0.0.1
  \ PORT=3000. Serving direct reclaims the CPU Nginx spends proxying and lets us
  \ manage keep-alive connections (and shed idle ones) ourselves.
  s" HOST" s" 0.0.0.0" getenv-or  dup host-len ! host-buf swap move
  s" PORT" s" 80" getenv-or  0. 2swap >number 2drop drop the-port !
  now-s start-time ! ;

create hostz 80 allot
: host>z ( -- zaddr )
  host-buf host-len @ hostz swap move  0 hostz host-len @ + c!  hostz ;

\ ---------------------------------------------------------------------------
\ Prepared statements (created once, reset per use) - rule 5 compliant
\ ---------------------------------------------------------------------------
variable stmt-feed    variable stmt-post    variable stmt-health
variable stmt-insert  variable stmt-like-ins variable stmt-exists
variable stmt-created

: prepare-all ( -- )
  s" SELECT 1" db-prepare stmt-health !
  s" SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id=p.id) FROM posts p JOIN users u ON u.id=p.user_id ORDER BY p.created_at DESC, p.id DESC LIMIT 20"
    db-prepare stmt-feed !
  s" SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id=p.id) FROM posts p JOIN users u ON u.id=p.user_id WHERE p.id=?"
    db-prepare stmt-post !
  s" INSERT INTO posts (user_id, body) VALUES (?, ?)"
    db-prepare stmt-insert !
  s" SELECT created_at FROM posts WHERE id=?"
    db-prepare stmt-created !
  s" INSERT INTO likes (user_id, post_id) VALUES (?, ?) ON CONFLICT (user_id, post_id) DO NOTHING"
    db-prepare stmt-like-ins !
  s" SELECT 1 FROM posts WHERE id=?" db-prepare stmt-exists !
  db-prepare-txn ;

\ Release any read transaction held by our prepared SELECT statements. On a
\ single EXCLUSIVE-locked connection, an un-reset stepped SELECT keeps a read
\ transaction open, which makes WAL checkpoints return SQLITE_LOCKED and never
\ reclaim the WAL. Called before checkpointing (off the request path).
: reset-readers ( -- )
  stmt-feed @ sqlite3_reset drop
  stmt-post @ sqlite3_reset drop
  stmt-exists @ sqlite3_reset drop
  stmt-created @ sqlite3_reset drop
  stmt-health @ sqlite3_reset drop ;

\ WAL maintenance, run off the request path after a write batch commits.
\ Reset readers so the checkpoint isn't blocked by our own connection, then
\ TRUNCATE-checkpoint to copy committed frames back and shrink the WAL file.
\ The caller (finalize-batch) gates this by a committed-write counter so it runs
\ only every WAL-CKPT-WRITES writes, not every batch: a checkpoint fsyncs the
\ main db, so doing it per batch would serialize the write path. Periodic
\ checkpointing keeps the WAL well under the ~16 MiB growth cliff.
: db-checkpoint-wal ( -- )
  reset-readers
  SQLITE_CHECKPOINT_TRUNCATE db-checkpoint drop ;

\ ---------------------------------------------------------------------------
\ Buffers
\ ---------------------------------------------------------------------------
8192 constant REQ-MAX
create req-buf REQ-MAX allot   variable req-len

16384 65536 + constant RESP-MAX
create resp-buf RESP-MAX allot variable resp-len
: resp-reset ( -- ) 0 resp-len ! ;
: resp+ ( a u -- )
  dup resp-len @ + RESP-MAX > abort" resp overflow"
  resp-buf resp-len @ + swap dup >r move r> resp-len +! ;
: resp-c ( ch -- ) resp-buf resp-len @ + c! 1 resp-len +! ;

\ ---------------------------------------------------------------------------
\ Parsed request fields
\ ---------------------------------------------------------------------------
2variable r-method     \ addr/u into req-buf
2variable r-path       \ addr/u into req-buf
2variable r-auth       \ addr/u Authorization value (0 0 if none)
2variable r-body       \ addr/u request body
variable  r-clen
variable  r-close      \ 1 if Connection: close

\ ---------------------------------------------------------------------------
\ Small string helpers
\ ---------------------------------------------------------------------------
: s= ( a1 u1 a2 u2 -- f ) compare 0= ;

: lower ( c -- c' ) dup [char] A [char] Z 1+ within if 32 + then ;

\ case-insensitive equality
: ci= ( a1 u1 a2 u2 -- f )
  { a1 u1 a2 u2 }
  u1 u2 <> if false exit then
  u1 0 ?do
    a1 i + c@ lower  a2 i + c@ lower  <> if false unloop exit then
  loop true ;

\ does (a u) start with literal (p pu) ?
: starts-with ( a u p pu -- f )
  { a u p pu }
  u pu < if false exit then
  a pu p pu compare 0= ;

\ find byte c in (a u); ( idx true | false )
: findc ( a u c -- idx true | false )
  { a u c }
  u 0 ?do a i + c@ c = if i true unloop exit then loop false ;

\ find substring (n nu) in (h hu): ( idx true | false )
: findsub ( h hu n nu -- idx true | false )
  { h hu n nu }
  nu 0= if 0 true exit then
  hu nu < if false exit then
  hu nu - 1+ 0 ?do
    h i + nu  n nu  compare 0= if i true unloop exit then
  loop false ;

\ parse unsigned decimal from (a u): ( value ok? ). ok? false if empty or non-digit.
: parse-uint ( a u -- val ok? )
  { a u }
  u 0= if 0 false exit then
  0   ( acc )
  u 0 ?do
    a i + c@ [char] 0 [char] 9 1+ within 0= if drop 0 false unloop exit then
    10 * a i + c@ [char] 0 - +
  loop true ;

\ ---------------------------------------------------------------------------
\ Request parsing. req-buf holds the full request (headers + body).
\ ---------------------------------------------------------------------------
: trim-lead-sp ( a u -- a' u' )
  begin dup 0> if over c@ bl = else false then while 1 /string repeat ;

\ scan one header line within [a,u); handle a single "name: value" line.
: handle-header ( line-a line-u -- )
  2dup [char] : findc 0= if 2drop exit then   ( line-a line-u cidx )
  { la lu ci }
  la ci                                       ( name-a name-u )
  la ci 1+ +  lu ci 1+ -  trim-lead-sp         ( name-a name-u val-a val-u )
  { va vu }
  2dup s" authorization" ci= if va vu r-auth 2! then
  2dup s" content-length" ci= if va vu parse-uint if r-clen ! else drop then then
  2dup s" connection"     ci= if va vu s" close" ci= if 1 r-close ! then then
  2drop ;

: parse-request ( -- ok? )
  req-buf req-len @   { ra ru }
  0 0 r-auth 2!  0 r-clen !  0 r-close !  0 0 r-body 2!
  0 0 r-method 2!  0 0 r-path 2!
  ra ru s\" \r\n\r\n" findsub 0= if false exit then  { hend }
  ra ru s\" \r\n" findsub 0= if false exit then       { rlend }
  \ method = [0, first space) in request line
  ra rlend 32 findc 0= if false exit then             { sp1 }
  ra sp1 r-method 2!
  \ path = [sp1+1, next space or end-of-request-line)
  ra sp1 1+ +  rlend sp1 1+ -                          ( path-a maxlen )
  2dup 32 findc if nip else then                       ( path-a path-u )
  r-path 2!
  \ headers region = [rlend+2, hend)
  rlend 2 +   { pos }                 \ absolute index of first header line
  begin pos hend < while
    \ find CRLF at or after pos (must exist because hend is CRLFCRLF)
    ra pos +  hend pos -  s\" \r\n" findsub 0= if hend pos - then  ( linelen )
    { llen }
    ra pos +  llen  handle-header
    pos llen + 2 +  to pos
  repeat
  \ body = [hend+4, hend+4+clen) clamped to what we have
  r-clen @ ?dup if                                     ( clen )
    ra hend 4 + +  swap                                ( body-a clen )
    req-len @ hend 4 + - min                           ( body-a avail )
    r-body 2!
  then
  true ;


\ ---------------------------------------------------------------------------
\ Response writing into resp-buf
\ ---------------------------------------------------------------------------
: crlf+ ( -- ) 13 resp-c 10 resp-c ;
: int>resp ( n -- )
  dup 0= if [char] 0 resp-c exit then
  here 64 + { tmp } 0 { cnt }
  begin dup 0> while
    10 /mod swap [char] 0 + tmp cnt + c! cnt 1+ to cnt
  repeat drop
  cnt 0 ?do tmp cnt 1- i - + c@ resp-c loop ;

variable keep-alive?

: send-response ( ct? body-a body-u status-a status-u -- )
  { ct ba bu sa su }
  resp-reset
  s" HTTP/1.1 " resp+ sa su resp+ crlf+
  ct if s" Content-Type: application/json" resp+ crlf+ then
  s" Content-Length: " resp+ bu int>resp crlf+
  keep-alive? @ if
    s" Connection: keep-alive" resp+ crlf+
    s" Keep-Alive: timeout=70" resp+ crlf+
  else
    s" Connection: close" resp+ crlf+
  then
  crlf+ ba bu resp+ ;

: reply-json ( status-a status-u -- )
  { sa su } true json-result sa su send-response ;

: reply-error ( msg-a msg-u status-a status-u -- )
  { ma mu sa su } json-reset ma mu json-error sa su reply-json ;

\ ---------------------------------------------------------------------------
\ Status line constants
\ ---------------------------------------------------------------------------
: s200 s" 200 OK" ;
: s201 s" 201 Created" ;
: s400 s" 400 Bad Request" ;
: s401 s" 401 Unauthorized" ;
: s404 s" 404 Not Found" ;
: s500 s" 500 Internal Server Error" ;
: s503 s" 503 Service Unavailable" ;

\ emit a post object (compact) from the current row of a stmt (cols 0..4)
: emit-post-cols ( stmt -- )
  >r
  r@ 0 st-col-int
  r@ 1 st-col-text
  r@ 2 st-col-text
  r@ 3 st-col-text
  r> 4 st-col-int
  json-post ;

\ emit {"post":<obj>} from stmt cols
: emit-post-wrapped ( stmt -- )
  >r
  r@ 0 st-col-int
  r@ 1 st-col-text
  r@ 2 st-col-text
  r@ 3 st-col-text
  r> 4 st-col-int
  json-post-wrapped ;

\ ---- GET /health ----
: do-health ( -- )
  stmt-health @ st-reset
  stmt-health @ st-step SQLITE_ROW = if
    json-reset  now-s start-time @ -  json-health-ok
    s200 reply-json
  else
    json-reset
    json-s," {\qstatus\q:\qdegraded\q,\qdb\q:\qunreachable\q,\qerror\q:\qselect failed\q}"
    s503 reply-json
  then ;

\ ---- GET /feed ----
: do-feed ( -- )
  stmt-feed @ st-reset
  json-reset json-feed-open
  0 { n }
  begin stmt-feed @ st-step SQLITE_ROW = while
    n 0> if json-feed-sep then
    stmt-feed @ emit-post-cols
    n 1+ to n
  repeat
  json-feed-close
  s200 reply-json ;

\ ---- GET /posts/:id ----  ( id -- )
: do-get-post ( id -- )
  stmt-post @ st-reset
  stmt-post @ 1 rot st-bind-int
  stmt-post @ st-step SQLITE_ROW = if
    json-reset stmt-post @ emit-post-wrapped
    s200 reply-json
  else
    s" post not found" s404 reply-error
  then ;

\ ---- POST /posts ----  ( uid name-a name-u -- )
: ws? ( c -- f ) dup bl = swap dup 9 = swap dup 10 = swap dup 12 = swap 13 = or or or or ;
: trim-ws ( a u -- a' u' )
  begin dup 0> if over c@ ws? else false then while 1 /string repeat
  begin dup 0> if 2dup + 1- c@ ws? else false then while 1- repeat ;

create newpost-ca 32 allot  variable newpost-ca-len
2variable saved-name

: do-create-post ( uid name-a name-u -- )
  saved-name 2!  { uid }
  r-body 2@ dup 0= if 2drop s" malformed JSON body" s400 reply-error exit then
  json-parse-body                        ( body-a body-u status )
  dup JSON-MALFORMED = if drop 2drop s" malformed JSON body" s400 reply-error exit then
  dup JSON-NO-BODY   = if drop 2drop s" body is required" s400 reply-error exit then
  drop trim-ws                           ( tbody-a tbody-u )
  dup 0= if 2drop s" body is required" s400 reply-error exit then
  dup 500 > if 2drop s" body must be at most 500 characters" s400 reply-error exit then
  { tb-a tb-u }
  db-begin-write                         \ join/open the batch transaction
  stmt-insert @ st-reset
  stmt-insert @ 1 uid st-bind-int
  stmt-insert @ 2 tb-a tb-u st-bind-text
  stmt-insert @ st-step SQLITE_DONE = 0= if
    s" internal server error" s500 reply-error exit then
  db-note-write                          \ record the write in this batch
  dbh @ sqlite3_last_insert_rowid { new-id }
  \ fetch the server-assigned created_at for the new row
  stmt-created @ st-reset
  stmt-created @ 1 new-id st-bind-int
  stmt-created @ st-step SQLITE_ROW = 0= if
    s" internal server error" s500 reply-error exit then
  stmt-created @ 0 st-col-text           ( ca-a ca-u )
  dup newpost-ca-len !  newpost-ca swap move   \ copy created_at into owned buf
  json-reset
  new-id                                 ( id )
  tb-a tb-u                              ( id ba bu )
  newpost-ca newpost-ca-len @            ( id ba bu ca cu )
  saved-name 2@                          ( id ba bu ca cu aa au )
  0 json-post-wrapped
  s201 reply-json ;



\ ---- POST /posts/:id/like ----  ( uid id -- )
: do-like ( uid id -- )
  { uid id }
  \ does the post exist?
  stmt-exists @ st-reset
  stmt-exists @ 1 id st-bind-int
  stmt-exists @ st-step SQLITE_ROW = 0= if
    s" post not found" s404 reply-error exit then
  \ insert like (ON CONFLICT DO NOTHING)
  db-begin-write                    \ join/open the batch transaction
  stmt-like-ins @ st-reset
  stmt-like-ins @ 1 uid st-bind-int
  stmt-like-ins @ 2 id st-bind-int
  stmt-like-ins @ st-step SQLITE_DONE = 0= if
    s" internal server error" s500 reply-error exit then
  db-note-write
  dbh @ sqlite3_changes             ( changes )
  dup 1 = { newly }                 ( changes )  \ newly==true if inserted
  0= { already }                    \ already==true if no insert happened
  json-reset  already id json-like
  newly if s201 else s200 then
  reply-json ;

\ ---------------------------------------------------------------------------
\ Auth helper: require a valid Bearer token. On success pushes ( uid name-a name-u true ).
\ On failure sends the proper 401 and pushes ( false ).
\ ---------------------------------------------------------------------------
: require-auth ( -- uid name-a name-u true | false )
  r-auth 2@ dup 0= if 2drop
    s" missing bearer token" s401 reply-error false exit then
  ( auth-a auth-u )
  s" Bearer " starts-with 0= if
    s" missing bearer token" s401 reply-error false exit then
  r-auth 2@ 7 /string                  ( token-a token-u )
  jwt-verify                           ( uid name-a name-u status )
  dup JWT-OK = if drop true exit then
  dup JWT-BAD-PAYLOAD = if drop 2drop drop
    s" invalid token payload" s401 reply-error false exit then
  drop 2drop drop
  s" invalid or expired token" s401 reply-error false ;

\ parse a positive-integer path segment ( a u -- id true | false )
: seg>id ( a u -- id true | false )
  parse-uint dup 0= if exit then       ( id true )
  drop dup 1 < if drop false else true then ;

\ ---------------------------------------------------------------------------
\ Routing. Uses r-method / r-path.
\ Path forms: /health /feed /posts /posts/:id /posts/:id/like
\ ---------------------------------------------------------------------------
: method-is ( a u -- f ) r-method 2@ s= ;

\ handle POST /posts/:id/like  given the id-segment slice ( a u )
: route-like ( idseg-a idseg-u -- )
  { ida idu }
  s" POST" method-is 0= if s" not found" s404 reply-error exit then
  require-auth 0= if exit then          ( uid name-a name-u )
  2drop                                 ( uid )  \ drop name, keep uid
  ida idu seg>id 0= if
    drop s" invalid post id" s400 reply-error exit then  ( uid id )
  do-like ;

\ handle GET /posts/:id  given the id-segment slice ( a u )
: route-get-post ( idseg-a idseg-u -- )
  s" GET" method-is 0= if 2drop s" not found" s404 reply-error exit then
  seg>id 0= if s" invalid post id" s400 reply-error exit then  ( id )
  do-get-post ;

: route ( -- )
  r-path 2@ { pa pu }
  s" GET"  method-is  pa pu s" /health" s= and if do-health exit then
  s" GET"  method-is  pa pu s" /feed"   s= and if do-feed   exit then
  pa pu s" /posts" s= if
    s" POST" method-is if
      require-auth 0= if exit then  do-create-post exit
    then
    s" not found" s404 reply-error exit
  then
  pa pu s" /posts/" starts-with if
    pa 7 + pu 7 - { ra ru }             \ rest after "/posts/"
    ra ru s" /like" findsub dup if      ( idx true )
      drop                              ( idx )
      dup 5 + ru = if                   \ "/like" is a proper suffix
        ra swap route-like exit         ( id-seg: ra idx )
      else
        drop s" invalid post id" s400 reply-error exit
      then
    else drop then
    \ plain GET /posts/:id
    ra ru route-get-post exit
  then
  s" not found" s404 reply-error ;


\ ---------------------------------------------------------------------------
\ The connection layer (accept loop, per-connection read/write, keep-alive) is
\ the single-process epoll event loop in epoll.fs. It reuses the shared words
\ above — parse-request, route, and the resp-buf response builder — so all the
\ request handling lives here and only the I/O driver lives in epoll.fs.
\ server.fs loads this file then epoll.fs and calls epoll-main.
\ ---------------------------------------------------------------------------

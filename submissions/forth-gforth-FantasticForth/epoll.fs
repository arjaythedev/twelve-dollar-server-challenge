\ ============================================================================
\ epoll.fs - single-process epoll event loop: the connection/I/O driver.
\ Reuses http.fs's parse-request / route / resp-buf response builder unchanged:
\ those operate on req-buf and build into resp-buf, so the event loop copies a
\ connection's bytes into req-buf before routing and copies resp-buf out into
\ the connection's write buffer. Adds non-blocking sockets, a fixed connection
\ slot pool, accept backpressure, connection-pressure shedding, group commit and
\ off-path WAL checkpointing.
\ Load order: ffi.fs db.fs jwt.fs json.fs http.fs epoll.fs
\ ============================================================================

\ --- epoll op + event constants (match ffi.fs C helper) ---
1 constant EP-ADD
2 constant EP-DEL
3 constant EP-MOD
1 constant EPOLLIN
4 constant EPOLLOUT
$2000 constant EPOLLRDHUP   \ not used directly but documented
$008 constant EPOLLERR
$010 constant EPOLLHUP

11 constant EAGAIN-ERR       \ EAGAIN/EWOULDBLOCK on Linux x86-64

\ --- connection slot pool ---
\ MAX-SLOTS bounds concurrent connections. In DIRECT mode each keep-alive client
\ is one connection, so this is the real concurrency ceiling (the box grants
\ LimitNOFILE=65535). Measured cost is ~8 KB RSS per live connection (buffers
\ are lazily faulted); at ~17k live connections the whole process is ~220 MB
\ RSS (that includes the 64 MiB SQLite page cache) — well within the 2 GB box.
\ The buffer *address space* reserved is MAX-SLOTS * (RBUF-SZ+WBUF-SZ) =
\ 20000*(8K+16K) = 480 MB, lazily faulted (not committed). Connections past the
\ pool pressure are shed via Connection: close (see connection-pressure shedding).
20000 constant MAX-SLOTS
8192  constant RBUF-SZ        \ read buffer per connection (requests capped at REQ-MAX=8192)
16384 constant WBUF-SZ        \ write buffer per connection (feed ~3 KB; 16K is safe)

\ slot fields (per slot), stored in parallel arrays indexed by slot#
create slot-fd      MAX-SLOTS cells allot    \ fd owned by this slot (-1 if free)
create slot-state   MAX-SLOTS cells allot    \ 0=free 1=reading 2=writing
create slot-rlen    MAX-SLOTS cells allot    \ bytes accumulated in read buffer
create slot-wlen    MAX-SLOTS cells allot    \ total bytes to write
create slot-woff    MAX-SLOTS cells allot    \ bytes already written
create slot-keep    MAX-SLOTS cells allot    \ keep-alive after this response?
\ large buffers live on the heap (too big for the dictionary)
variable rbuf-base   variable wbuf-base
: alloc-bufs ( -- )
  MAX-SLOTS RBUF-SZ * allocate throw rbuf-base !
  MAX-SLOTS WBUF-SZ * allocate throw wbuf-base ! ;

0 constant ST-FREE
1 constant ST-READING
2 constant ST-WRITING

: slot-rbuf@ ( slot -- addr ) RBUF-SZ * rbuf-base @ + ;
: slot-wbuf@ ( slot -- addr ) WBUF-SZ * wbuf-base @ + ;
: scell ( base slot -- addr ) cells + ;

\ --- free-list stack: O(1) alloc/free instead of an O(MAX-SLOTS) scan per accept
create free-stack  MAX-SLOTS cells allot
variable free-top                  \ number of free slots currently on the stack
: free-push ( slot -- ) free-top @ free-stack swap cells + !  1 free-top +! ;
: free-pop  ( -- slot | -1 )
  free-top @ 0= if -1 exit then
  -1 free-top +!  free-top @ free-stack swap cells + @ ;

\ fd -> slot index map. fds can be as high as RLIMIT_NOFILE (65535 on the box)
\ plus the handful already open, so size well above that and GUARD every access:
\ an out-of-range fd must never index this array (that was a latent OOB write).
262144 constant MAX-FDMAP
create fd->slot MAX-FDMAP cells allot
: fd-ok? ( fd -- f ) dup 0>= swap MAX-FDMAP < and ;
: fdslot! ( slot fd -- ) dup fd-ok? if cells fd->slot + ! else 2drop then ;
: fdslot@ ( fd -- slot ) dup fd-ok? if cells fd->slot + @ else drop -1 then ;

variable epfd
variable lfd
variable listen-armed?             \ 1 if the listen fd is registered for EPOLLIN

: init-slots ( -- )
  0 free-top !
  \ push slots in reverse so slot 0 is handed out first (nicer for debugging)
  MAX-SLOTS 0 ?do -1 slot-fd i scell ! ST-FREE slot-state i scell ! loop
  MAX-SLOTS 0 ?do MAX-SLOTS 1- i - free-push loop
  MAX-FDMAP 0 ?do -1 fd->slot i cells + ! loop ;

\ pop a free slot; -1 if none
: alloc-slot ( -- slot | -1 ) free-pop ;

\ --- connection-pressure shedding ---------------------------------------
\ Open connections = MAX-SLOTS - free-top (O(1), no syscalls). In direct mode
\ each keep-alive client holds a slot (and an fd). To pass the pool/fd ceiling
\ honestly we shed connections under pressure by answering with Connection:close
\ instead of keep-alive, so the client reconnects on its next request (one
\ accept, ~100 us) rather than pinning a slot forever. Decided PER CONNECTION
\ (when its request completes), not per request-in-flight, and with hysteresis
\ so the open count doesn't oscillate at the threshold. This stores only
\ transport state; every request still reads live SQLite (rule 5).
\ Thresholds as fractions of the pool:
MAX-SLOTS 85 100 */ constant SHED-SOFT    \ past this: shed (close) finishing conns
MAX-SLOTS 97 100 */ constant SHED-HARD    \ past this: close every finishing conn
MAX-SLOTS 70 100 */ constant SHED-CLEAR   \ below this: back to normal keep-alive
variable shedding?   0 shedding? !        \ hysteresis latch

: open-conns ( -- n ) MAX-SLOTS free-top @ - ;

\ update the hysteresis latch from the current open-conn count.
: shed-update ( -- )
  open-conns { oc }
  shedding? @ if
    oc SHED-CLEAR <= if 0 shedding? ! then     \ dropped low enough: stop shedding
  else
    oc SHED-SOFT >= if 1 shedding? ! then       \ climbed past soft: start shedding
  then ;

\ should THIS finishing connection be kept alive? false => send Connection:close.
\ Under the hard ceiling we always close; while shedding (soft..hard) we close so
\ active clients cycle out and free slots for new ones.
: keep-this-conn? ( -- f )
  open-conns SHED-HARD >= if false exit then
  shedding? @ 0= ;

: slot-reset-read ( slot -- )
  0 over slot-rlen swap scell !
  ST-READING over slot-state swap scell !
  drop ;

\ --- accept backpressure -------------------------------------------------
\ When the slot pool is exhausted we must NOT keep accepting and immediately
\ closing: with level-triggered EPOLLIN on the listen fd and eager clients that
\ reconnect instantly, that becomes a 100% CPU accept/close storm. Instead we
\ disarm the listen fd until a slot frees, then re-arm. In practice connection
\ shedding (Connection: close under pressure) keeps us off this path, but it is
\ the backstop if the pool fills anyway.
: accept-pause ( -- )
  listen-armed? @ if
    epfd @ EP-MOD lfd @ 0 tdsc_epoll_ctl drop   \ interest=0: stop reporting readability
    0 listen-armed? !
  then ;
: accept-resume ( -- )
  listen-armed? @ 0= if
    epfd @ EP-MOD lfd @ EPOLLIN tdsc_epoll_ctl drop
    1 listen-armed? !
  then ;

: open-slot ( fd -- )
  { fd }
  \ Guard: an fd outside the map range cannot be tracked safely -> close it.
  fd fd-ok? 0= if fd tdsc_close drop exit then
  alloc-slot dup 0< if
    \ pool full: close THIS fd and pause accepting until a slot frees.
    drop fd tdsc_close drop accept-pause exit
  then                                                   ( slot )
  { slot }
  fd slot-fd slot scell !          \ slot-fd[slot]=fd
  slot fd fdslot!                  \ fd->slot[fd]=slot
  slot slot-reset-read
  epfd @ EP-ADD fd EPOLLIN tdsc_epoll_ctl drop ;  \ register for readability

: close-slot ( slot -- )
  { slot }
  \ ignore double-close of an already-free slot
  slot-state slot scell @ ST-FREE = if exit then
  slot-fd slot scell @  { fd }
  fd 0>= if
    epfd @ EP-DEL fd 0 tdsc_epoll_ctl drop
    fd tdsc_close drop
    -1 fd fdslot!
  then
  -1 slot-fd slot scell !
  ST-FREE slot-state slot scell !
  slot free-push                   \ return slot to the free list
  accept-resume ;                  \ a slot is available again -> resume accepting

\ ---------------------------------------------------------------------------
\ Request completeness: copy the slot's bytes into req-buf, run parse-request,
\ and check that headers + Content-Length body have all arrived. parse-request
\ sets r-clen; we locate the header end to know the body offset.
\ ---------------------------------------------------------------------------
: request-complete? ( slot -- f )
  { slot }
  slot slot-rbuf@  req-buf  slot-rlen slot scell @  dup req-len !  move
  req-buf req-len @ s\" \r\n\r\n" findsub  ( idx true | false )
  if                                       ( hend )
    parse-request if                       ( hend )
      4 + r-clen @ +  req-len @ <=
    else drop false then
  else false then ;

\ ---------------------------------------------------------------------------
\ Group commit bookkeeping (event-loop level)
\ ---------------------------------------------------------------------------
\ Slots whose request produced a write this pass. If the end-of-pass COMMIT
\ fails, each is rewritten to a 500 before its response is sent (rule 6: a 201
\ is only emitted for a durably committed row). Bounded by events per pass.
create pending-wslots MAX-SLOTS cells allot
variable pending-wn   0 pending-wn !
: note-write-slot ( slot -- )
  pending-wn @ MAX-SLOTS < if
    pending-wn @ cells pending-wslots + !  1 pending-wn +!
  else drop then ;

\ WAL checkpoint trigger: run an off-request-path TRUNCATE checkpoint once this
\ many writes have committed since the last checkpoint. A post/like commit adds
\ a few WAL frames, so ~1000 writes keeps the WAL well under the 4096-frame
\ (~16 MiB) "WAL-growth cliff" while amortizing the checkpoint fsync across many
\ writes (checkpointing every batch would serialize the write path).
1000 constant WAL-CKPT-WRITES
variable writes-since-ckpt  0 writes-since-ckpt !

\ rewrite a slot's buffered response to a 500 error (used on commit failure).
\ reply-error builds into resp-buf; we then copy it into the slot write buffer.
: slot->500 ( slot -- )
  { slot }
  s" internal server error" s500 reply-error
  resp-buf  slot slot-wbuf@  resp-len @  dup slot-wlen slot scell !  move
  0 slot-woff slot scell ! ;

\ commit the batch (if any) and handle checkpointing. Called at end of each
\ event-loop pass, BEFORE on-writable gets a chance to send write responses
\ (which only happens on the next epoll_wait). On commit failure, convert all
\ this pass's write-slots to 500.
: finalize-batch ( -- )
  batch-writes @ 0= if 0 pending-wn ! exit then
  db-commit-batch                   ( rc )
  SQLITE_DONE <> if
    \ commit failed: 500 every pending write-slot
    pending-wn @ 0 ?do
      pending-wslots i cells + @ slot->500
    loop
  then
  \ accumulate this batch's writes toward the checkpoint threshold, then reset.
  batch-writes @ writes-since-ckpt +!
  0 batch-writes !  0 pending-wn !
  \ off-path WAL maintenance: only when we've committed a threshold's worth of
  \ writes do we reset readers and TRUNCATE-checkpoint. This bounds the WAL (no
  \ growth cliff) without an fsync on every batch.
  writes-since-ckpt @ WAL-CKPT-WRITES >= if
    db-checkpoint-wal
    0 writes-since-ckpt !
  then ;

\ copy slot read buffer into the global req-buf and run the pipeline,
\ leaving the response bytes in resp-buf (resp-len). Then copy into slot wbuf.
: process-slot ( slot -- )
  { slot }
  \ copy request bytes -> req-buf
  slot slot-rbuf@  req-buf  slot-rlen slot scell @  dup req-len !  move
  batch-writes @ { w0 }                 \ write count before routing
  parse-request 0= if
    \ unparseable: emit 400 and close
    0 keep-alive? !
    s" malformed JSON body" s400 reply-error
  else
    \ keep-alive unless the client said close OR we're shedding under connection
    \ pressure (then answer Connection: close so this client cycles out). Decided
    \ per connection, as its request completes, with hysteresis (see shed-update).
    1 keep-alive? !
    r-close @ if 0 keep-alive? ! then
    keep-this-conn? 0= if 0 keep-alive? ! then
    route
  then
  \ if this request added a write to the batch, remember the slot so that a
  \ failed end-of-pass COMMIT can convert its response to a 500 before sending.
  batch-writes @ w0 <> if slot note-write-slot then
  \ copy resp-buf -> slot wbuf
  resp-buf  slot slot-wbuf@  resp-len @  dup slot-wlen slot scell !  move
  0 slot-woff slot scell !
  keep-alive? @  slot-keep slot scell !
  ST-WRITING slot-state slot scell ! ;

\ arm epoll interest for a slot according to its state
: arm ( slot -- )
  { slot }
  slot-fd slot scell @ { fd }
  slot-state slot scell @ ST-WRITING = if
    epfd @ EP-MOD fd EPOLLOUT tdsc_epoll_ctl drop
  else
    epfd @ EP-MOD fd EPOLLIN tdsc_epoll_ctl drop
  then ;

\ ---------------------------------------------------------------------------
\ Event handlers
\ ---------------------------------------------------------------------------
: on-readable ( slot -- )
  { slot }
  begin
    slot slot-rbuf@  slot-rlen slot scell @ +      ( dst )
    RBUF-SZ  slot-rlen slot scell @ -            ( dst space )
    dup 0<= if drop slot close-slot exit then           \ buffer full, drop
    slot-fd slot scell @ -rot tdsc_recv_nb              ( n )
    dup 0 = if drop slot close-slot exit then           \ EOF
    dup 0< if drop tdsc_errno EAGAIN-ERR = if exit then slot close-slot exit then
    slot-rlen slot scell +!                        \ rlen += n
    slot request-complete? if
      slot process-slot
      slot arm
      exit
    then
  again ;

: on-writable ( slot -- )
  { slot }
  begin
    slot-wlen slot scell @  slot-woff slot scell @ -  ( remaining )
    dup 0<= if drop                                     \ fully written
      slot-keep slot scell @ if
        slot slot-reset-read  slot arm
      else slot close-slot then
      exit
    then
    ( remaining )
    slot slot-wbuf@  slot-woff slot scell @ +      ( remaining src )
    swap                                                ( src remaining )
    slot-fd slot scell @ -rot tdsc_send_nb              ( n )
    dup 0< if drop tdsc_errno EAGAIN-ERR = if exit then slot close-slot exit then
    dup 0 = if drop slot close-slot exit then
    slot-woff slot scell +!
  again ;

\ accept all pending connections on the listen socket (edge-safe: loop until
\ EAGAIN, OR until the pool fills and open-slot pauses accepting).
: on-accept ( -- )
  begin
    lfd @ tdsc_accept_nb                      ( fd )
    dup 0< if                                 \ accept failed
      drop
      tdsc_errno EAGAIN-ERR = if exit then    \ EAGAIN: drained, just stop
      \ A real error (EMFILE/ENFILE: out of fds). The listen fd is
      \ level-triggered, so retrying immediately would busy-spin against a
      \ still-readable backlog. Pause accepting; close-slot re-arms when an
      \ fd frees up. This turns an fd-exhaustion spin into a quiet backpressure.
      accept-pause exit
    then
    dup tdsc_set_nodelay drop
    open-slot                                 \ registers (or closes + pauses if full)
    listen-armed? @ 0= if exit then           \ pool full: stop; backlog holds the rest
  again ;

\ ---------------------------------------------------------------------------
\ Event loop
\ ---------------------------------------------------------------------------
: dispatch-event ( i -- )
  { i }
  i tdsc_ev_fd   { fd }
  i tdsc_ev_ev   { ev }
  fd fdslot@     { slot }
  fd lfd @ = if on-accept exit then
  slot 0< if exit then
  ev EPOLLERR EPOLLHUP or and if slot close-slot exit then
  ev EPOLLIN  and if slot-state slot scell @ ST-READING = if slot on-readable then then
  fd fdslot@ 0< if exit then          \ slot may have closed during on-readable
  ev EPOLLOUT and if slot-state slot scell @ ST-WRITING = if slot on-writable then then ;

: event-loop ( -- )
  begin
    epfd @ -1 tdsc_epoll_wait        ( n )
    dup 0> if
      0 ?do i dispatch-event loop
      \ end of pass: commit this batch's writes (durably, before their
      \ responses are sent next pass) and checkpoint off the request path.
      finalize-batch
      \ refresh the connection-pressure shedding latch (O(1)).
      shed-update
    else drop then
  again ;

: epoll-server-start ( -- )
  tdsc_ignore_sigpipe
  config-load
  s" SQLITE_PATH" s" feed.db" getenv-or db-open
  db-configure
  prepare-all
  alloc-bufs
  init-slots
  tdsc_epoll_create dup 0< abort" epoll_create failed" epfd !
  host>z the-port @ 4096 tdsc_listen
  dup 0< abort" listen failed"  dup lfd !
  tdsc_set_nonblock drop
  \ register listen fd for EPOLLIN
  epfd @ EP-ADD lfd @ EPOLLIN tdsc_epoll_ctl drop
  1 listen-armed? !
  cr ." forth-gforth (epoll) listening on " host-buf host-len @ type
  [char] : emit the-port @ . cr ;

: epoll-main ( -- ) epoll-server-start event-loop ;

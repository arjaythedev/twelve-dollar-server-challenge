;; The $12 server, hand-written in WebAssembly text.
;;
;; Everything a request touches is here: the epoll loop, HTTP/1.1 parsing, routing, JSON reading and
;; writing, base64url, SHA-256/HMAC for the HS256 JWT check, and the SQL calls. Stock SQLite (compiled
;; to wasm with wasi-sdk) is merged in as module "sq"; this module imports its memory and a few exported
;; SQLite calls (src/sqlite_glue.c). "sys" is the host's raw socket/epoll calls (src/host.c).
;;
;; Memory map (this module owns [1 MiB, 16 MiB); SQLite's C stack is below 1 MiB, its data and heap above 16 MiB):
;;   0x100000  string constants, NUL-terminated, in 64-byte slots; SQL at 0x100800
;;   0x102000  epoll events: 512 x (fd i32, events i32)
;;   0x104000  RBUF  read buffer, 128 KiB (a connection's unfinished request is copied in front first)
;;   0x124000  BODYB response body being built, 128 KiB
;;   0x144000  GEN   per-fd generation, 65536 x i32: bumped on close, so a response held for COMMIT never
;;                   goes to a later connection that reuses the fd
;;   0x184000  CONN  per-fd state, 65536 x 16 bytes: in_ptr, in_len, out_ptr, out_len (malloc'd leftovers)
;;   0x284000  JWT scratch: header, payload, SHA-256 state/schedule/pad, HMAC states, tables
;;   0x285000  TXT   decoded POST body, 64 KiB
;;   0x295000  MISC  out-params, clock, iovec, digit buffer
;;   0x2a0000  LAST  per-fd ms of the last event (idle eviction), 65536 x i32
;;   0x2e0000  BT    B-tree reads: decoded columns (8 x 32 B), out-params at 0x2e0100, feed ids at 0x2e0200,
;;                   feed like counts at 0x2e0300, like counts by id span at 0x2e0400 (256 x i64)
;;   0x300000  RECS  the batch's responses: (fd, start, len, close after) x up to 60,000
;;   0x400000  OUTB  the batch's response bytes, sent after COMMIT (flushed between connections at 12 MiB)
(module
  (import "sq" "memory" (memory 1))
  (import "sq" "_initialize" (func $sq_init))
  (import "sq" "db_open" (func $db_open (param i32 i32) (result i32)))
  (import "sq" "db_exec" (func $db_exec (param i32 i32) (result i32)))
  (import "sq" "db_prepare" (func $db_prepare (param i32 i32 i32) (result i32)))
  (import "sq" "st_bind_int64" (func $bind_i64 (param i32 i32 i64) (result i32)))
  (import "sq" "st_bind_text" (func $bind_text (param i32 i32 i32 i32) (result i32)))
  (import "sq" "st_step" (func $step (param i32) (result i32)))
  (import "sq" "st_reset" (func $reset (param i32) (result i32)))
  (import "sq" "st_int64" (func $col_i64 (param i32 i32) (result i64)))
  (import "sq" "st_text" (func $col_text (param i32 i32) (result i32)))
  (import "sq" "st_bytes" (func $col_bytes (param i32 i32) (result i32)))
  (import "sq" "db_changes" (func $changes (param i32) (result i32)))
  (import "sq" "db_errmsg" (func $errmsg (param i32) (result i32)))
  (import "sq" "env_get" (func $getenv (param i32) (result i32)))
  (import "sq" "db_autocommit" (func $autocommit (param i32) (result i32)))
  (import "sq" "db_utf8" (func $db_utf8 (param i32) (result i32)))
  (import "sq" "bt_of" (func $bt_of (param i32) (result i32)))
  (import "sq" "bt_read" (func $bt_read (param i32 i32) (result i32)))
  (import "sq" "key_info" (func $key_info (param i32 i32 i32) (result i32)))
  (import "sq" "cur_size" (func $cur_size (result i32)))
  (import "sq" "cur_open" (func $cur_open (param i32 i32 i32 i32) (result i32)))
  (import "sq" "cur_close" (func $cur_close (param i32) (result i32)))
  (import "sq" "cur_first" (func $cur_first (param i32 i32) (result i32)))
  (import "sq" "cur_next" (func $cur_next (param i32) (result i32)))
  (import "sq" "cur_prev" (func $cur_prev (param i32) (result i32)))
  (import "sq" "cur_eof" (func $cur_eof (param i32) (result i32)))
  (import "sq" "cur_rowid" (func $cur_rowid (param i32) (result i64)))
  (import "sq" "cur_seek_rowid" (func $cur_seek_rowid (param i32 i64 i32) (result i32)))
  (import "sq" "cur_seek_int" (func $cur_seek_int (param i32 i32 i64 i32 i32) (result i32)))
  (import "sq" "cur_payload_size" (func $cur_psize (param i32) (result i32)))
  (import "sq" "cur_payload" (func $cur_payload (param i32 i32) (result i32)))
  (import "sq" "mem_alloc" (func $malloc (param i32) (result i32)))
  (import "sq" "mem_free" (func $free (param i32)))
  (import "wasi_snapshot_preview1" "clock_time_get" (func $clock (param i32 i64 i32) (result i32)))
  (import "wasi_snapshot_preview1" "fd_write" (func $fd_write (param i32 i32 i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "proc_exit" (func $exit (param i32)))
  (import "sys" "listen" (func $listen (param i32 i32 i32) (result i32)))
  (import "sys" "epoll_create" (func $epoll_create (result i32)))
  (import "sys" "epoll_ctl" (func $epoll_ctl (param i32 i32 i32 i32) (result i32)))
  (import "sys" "epoll_wait" (func $epoll_wait (param i32 i32 i32 i32) (result i32)))
  (import "sys" "accept" (func $accept (param i32) (result i32)))
  (import "sys" "recv" (func $recv (param i32 i32 i32) (result i32)))
  (import "sys" "send" (func $send (param i32 i32 i32) (result i32)))
  (import "sys" "close" (func $close (param i32) (result i32)))
  (import "sys" "sendmany" (func $sendmany (param i32 i32 i32) (result i32)))

  ;; ---------- string constants ----------
  (data (i32.const 0x100000) "HTTP/1.1 200 OK\0d\0a")
  (data (i32.const 0x100040) "HTTP/1.1 201 Created\0d\0a")
  (data (i32.const 0x100080) "HTTP/1.1 400 Bad Request\0d\0a")
  (data (i32.const 0x1000c0) "HTTP/1.1 401 Unauthorized\0d\0a")
  (data (i32.const 0x100100) "HTTP/1.1 404 Not Found\0d\0a")
  (data (i32.const 0x100140) "HTTP/1.1 500 Internal Server Error\0d\0a")
  (data (i32.const 0x100180) "HTTP/1.1 503 Service Unavailable\0d\0a")
  (data (i32.const 0x1001c0) "Content-Type: application/json\0d\0aContent-Length: ")
  (data (i32.const 0x100200) "\0d\0a\0d\0a")
  (data (i32.const 0x100240) "{\"error\":\"not found\"}")
  (data (i32.const 0x100280) "{\"error\":\"invalid post id\"}")
  (data (i32.const 0x1002c0) "{\"error\":\"post not found\"}")
  (data (i32.const 0x100300) "{\"error\":\"missing bearer token\"}")
  (data (i32.const 0x100340) "{\"error\":\"invalid or expired token\"}")
  (data (i32.const 0x100380) "{\"error\":\"invalid token payload\"}")
  (data (i32.const 0x1003c0) "{\"error\":\"malformed JSON body\"}")
  (data (i32.const 0x100400) "{\"error\":\"body is required\"}")
  (data (i32.const 0x100440) "{\"error\":\"body must be at most 500 characters\"}")
  (data (i32.const 0x100480) "{\"error\":\"internal server error\"}")
  (data (i32.const 0x1004c0) "{\"posts\":[")
  (data (i32.const 0x100500) "{\"id\":")
  (data (i32.const 0x100540) ",\"body\":")
  (data (i32.const 0x100580) ",\"created_at\":\"")
  (data (i32.const 0x1005c0) "\",\"author\":")
  (data (i32.const 0x100600) ",\"like_count\":")
  (data (i32.const 0x100640) "{\"post\":")
  (data (i32.const 0x100680) "{\"liked\":true,\"already_liked\":")
  (data (i32.const 0x1006c0) ",\"post_id\":")
  (data (i32.const 0x100700) "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":")
  (data (i32.const 0x100740) "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":")
  (data (i32.const 0x100780) ",\"like_count\":0}}")
  (data (i32.const 0x1007c0) "true")
  (data (i32.const 0x1007e0) "false")
  ;; SQL, NUL-terminated
  (data (i32.const 0x100800) "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = ?")
  (data (i32.const 0x100900) "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) FROM posts p JOIN users u ON u.id = p.user_id ORDER BY p.created_at DESC, p.id DESC LIMIT 20")
  (data (i32.const 0x100a00) "INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at")
  (data (i32.const 0x100b00) "INSERT INTO likes (user_id, post_id) VALUES (?, ?) ON CONFLICT (user_id, post_id) DO NOTHING")
  (data (i32.const 0x100c00) "SELECT 1 FROM posts WHERE id = ?")
  (data (i32.const 0x100c80) "SELECT 1")
  (data (i32.const 0x100ca0) "BEGIN")
  (data (i32.const 0x100cb0) "COMMIT")
  (data (i32.const 0x100cc0) "ROLLBACK")
  ;; WASI has no shared memory, so WAL runs with exclusive locking (wal-index on the heap)
  (data (i32.const 0x100d00) "PRAGMA locking_mode=EXCLUSIVE; PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA cache_size=-65536; PRAGMA temp_store=MEMORY;")
  (data (i32.const 0x100e00) "SQLITE_PATH")
  (data (i32.const 0x100e20) "JWT_SECRET")
  (data (i32.const 0x100e40) "HOST")
  (data (i32.const 0x100e60) "PORT")
  (data (i32.const 0x100f00) "alg")
  (data (i32.const 0x100f10) "exp")
  (data (i32.const 0x100f20) "sub")
  (data (i32.const 0x100f30) "username")
  (data (i32.const 0x100f40) "body")
  (data (i32.const 0x100f50) "HS256")
  (data (i32.const 0x100f60) "Bearer ")
  (data (i32.const 0x100f70) "content-length")
  (data (i32.const 0x100f80) "authorization")
  (data (i32.const 0x100f90) "connection")
  (data (i32.const 0x100fa0) "close")
  (data (i32.const 0x100fb0) "/feed")
  (data (i32.const 0x100fc0) "/health")
  (data (i32.const 0x100fd0) "/posts/")
  (data (i32.const 0x100fe0) "/like")
  (data (i32.const 0x101000) "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_")
  (data (i32.const 0x101080) "wat server listening\0a")
  (data (i32.const 0x1010c0) "fatal: cannot open the database\0a")
  (data (i32.const 0x101100) "fatal: cannot prepare a statement\0a")
  (data (i32.const 0x101140) "fatal: cannot listen\0a")
  (data (i32.const 0x101180) "fatal: JWT_SECRET, SQLITE_PATH, HOST and PORT must be set\0a")
  (data (i32.const 0x1011c0) "COMMIT failed: the batch's connections are closed unanswered\0a")
  (data (i32.const 0x101200) "SELECT name, rootpage FROM sqlite_schema WHERE type IN ('table','index')")
  (data (i32.const 0x101280) "posts")
  (data (i32.const 0x101290) "users")
  (data (i32.const 0x1012a0) "posts_created_at_id_idx")
  (data (i32.const 0x1012c0) "likes_post_id_idx")
  (data (i32.const 0x1012e0) "B-tree reads off: SQL statements serve every read\0a")
  ;; SHA-256 round constants and initial state, as little-endian i32s
  (data (i32.const 0x284b00) "\98\2f\8a\42\91\44\37\71\cf\fb\c0\b5\a5\db\b5\e9\5b\c2\56\39\f1\11\f1\59\a4\82\3f\92\d5\5e\1c\ab\98\aa\07\d8\01\5b\83\12\be\85\31\24\c3\7d\0c\55\74\5d\be\72\fe\b1\de\80\a7\06\dc\9b\74\f1\9b\c1\c1\69\9b\e4\86\47\be\ef\c6\9d\c1\0f\cc\a1\0c\24\6f\2c\e9\2d\aa\84\74\4a\dc\a9\b0\5c\da\88\f9\76\52\51\3e\98\6d\c6\31\a8\c8\27\03\b0\c7\7f\59\bf\f3\0b\e0\c6\47\91\a7\d5\51\63\ca\06\67\29\29\14\85\0a\b7\27\38\21\1b\2e\fc\6d\2c\4d\13\0d\38\53\54\73\0a\65\bb\0a\6a\76\2e\c9\c2\81\85\2c\72\92\a1\e8\bf\a2\4b\66\1a\a8\70\8b\4b\c2\a3\51\6c\c7\19\e8\92\d1\24\06\99\d6\85\35\0e\f4\70\a0\6a\10\16\c1\a4\19\08\6c\37\1e\4c\77\48\27\b5\bc\b0\34\b3\0c\1c\39\4a\aa\d8\4e\4f\ca\9c\5b\f3\6f\2e\68\ee\82\8f\74\6f\63\a5\78\14\78\c8\84\08\02\c7\8c\fa\ff\be\90\eb\6c\50\a4\f7\a3\f9\be\f2\78\71\c6")
  (data (i32.const 0x284c00) "\67\e6\09\6a\85\ae\67\bb\72\f3\6e\3c\3a\f5\4f\a5\7f\52\0e\51\8c\68\05\9b\ab\d9\83\1f\19\cd\e0\5b")

  (global $db (mut i32) (i32.const 0))
  (global $st_one (mut i32) (i32.const 0))
  (global $st_feed (mut i32) (i32.const 0))
  (global $st_ins (mut i32) (i32.const 0))
  (global $st_like (mut i32) (i32.const 0))
  (global $st_exists (mut i32) (i32.const 0))
  (global $st_ping (mut i32) (i32.const 0))
  (global $t0 (mut i64) (i64.const 0))
  (global $ep (mut i32) (i32.const 0))
  (global $lfd (mut i32) (i32.const 0))
  (global $w (mut i32) (i32.const 0))       ;; write cursor (BODYB while building a body, OUTB otherwise)
  (global $o (mut i32) (i32.const 0x400000)) ;; end of the batch's output in OUTB
  (global $ostart (mut i32) (i32.const 0x400000)) ;; start of the current connection's output
  (global $nrec (mut i32) (i32.const 0))
  (global $st_begin (mut i32) (i32.const 0))
  (global $st_commit (mut i32) (i32.const 0))
  (global $jp (mut i32) (i32.const 0))      ;; JSON reader cursor
  (global $je (mut i32) (i32.const 0))      ;; JSON reader end
  (global $jv (mut i32) (i32.const 0))      ;; value found by $jfind
  (global $uid (mut i64) (i64.const 0))     ;; from the token
  (global $un_p (mut i32) (i32.const 0))    ;; username, raw JSON string bytes from the token
  (global $un_n (mut i32) (i32.const 0))
  (global $closeme (mut i32) (i32.const 0))
  ;; idle-connection eviction (LAST[fd] at 0x2a0000: ms of the fd's last event, 0 = not open)
  (global $tick (mut i32) (i32.const 0))     ;; monotonic ms, read once per epoll_wait
  (global $nconn (mut i32) (i32.const 0))
  (global $maxfd (mut i32) (i32.const 0))
  (global $lastsweep (mut i32) (i32.const 0))
  (global $lis_off (mut i32) (i32.const 0))
  ;; group commit: the open transaction, the ms it began, and whether it has written anything
  (global $txopen (mut i32) (i32.const 0))
  (global $txt0 (mut i32) (i32.const 0))
  (global $wr (mut i32) (i32.const 0))
  ;; B-tree reads ($bt = 0: off, every read goes through SQL)
  (global $bt (mut i32) (i32.const 0))
  (global $r_posts (mut i32) (i32.const 0))
  (global $r_users (mut i32) (i32.const 0))
  (global $r_top (mut i32) (i32.const 0))
  (global $r_likes (mut i32) (i32.const 0))
  (global $cookie (mut i32) (i32.const 0))
  (global $k_top (mut i32) (i32.const 0))
  (global $k_likes (mut i32) (i32.const 0))
  (global $csz (mut i32) (i32.const 0))
  (global $cu_t (mut i32) (i32.const 0))
  (global $cu_p (mut i32) (i32.const 0))
  (global $cu_u (mut i32) (i32.const 0))
  (global $cu_l (mut i32) (i32.const 0))
  (global $vn (mut i32) (i32.const 0))

  ;; ---------- memory helpers ----------
  (func $strlen (param $p i32) (result i32)
    (local $q i32)
    (local.set $q (local.get $p))
    (block $d (loop $l
      (br_if $d (i32.eqz (i32.load8_u (local.get $q))))
      (local.set $q (i32.add (local.get $q) (i32.const 1)))
      (br $l)))
    (i32.sub (local.get $q) (local.get $p)))

  (func $memeq (param $a i32) (param $b i32) (param $n i32) (result i32)
    (block $no (loop $l
      (if (i32.eqz (local.get $n)) (then (return (i32.const 1))))
      (br_if $no (i32.ne (i32.load8_u (local.get $a)) (i32.load8_u (local.get $b))))
      (local.set $a (i32.add (local.get $a) (i32.const 1)))
      (local.set $b (i32.add (local.get $b) (i32.const 1)))
      (local.set $n (i32.sub (local.get $n) (i32.const 1)))
      (br $l)))
    (i32.const 0))

  ;; case-insensitive compare of $a with the lowercase constant $b
  (func $ieq (param $a i32) (param $b i32) (param $n i32) (result i32)
    (local $c i32)
    (block $no (loop $l
      (if (i32.eqz (local.get $n)) (then (return (i32.const 1))))
      (local.set $c (i32.load8_u (local.get $a)))
      (if (i32.lt_u (i32.sub (local.get $c) (i32.const 65)) (i32.const 26))
        (then (local.set $c (i32.or (local.get $c) (i32.const 32)))))
      (br_if $no (i32.ne (local.get $c) (i32.load8_u (local.get $b))))
      (local.set $a (i32.add (local.get $a) (i32.const 1)))
      (local.set $b (i32.add (local.get $b) (i32.const 1)))
      (local.set $n (i32.sub (local.get $n) (i32.const 1)))
      (br $l)))
    (i32.const 0))

  ;; first $c in [$p, $e), or 0
  (func $memchr (param $p i32) (param $c i32) (param $e i32) (result i32)
    (block $d (loop $l
      (br_if $d (i32.ge_u (local.get $p) (local.get $e)))
      (if (i32.eq (i32.load8_u (local.get $p)) (local.get $c)) (then (return (local.get $p))))
      (local.set $p (i32.add (local.get $p) (i32.const 1)))
      (br $l)))
    (i32.const 0))

  (func $log (param $s i32)
    (i32.store (i32.const 0x295020) (local.get $s))
    (i32.store (i32.const 0x295024) (call $strlen (local.get $s)))
    (drop (call $fd_write (i32.const 2) (i32.const 0x295020) (i32.const 1) (i32.const 0x295028))))

  (func $fatal (param $s i32) (call $log (local.get $s)) (call $exit (i32.const 1)))

  (func $clock_ms (result i32)
    (drop (call $clock (i32.const 1) (i64.const 1000000) (i32.const 0x295018)))
    (i32.wrap_i64 (i64.div_u (i64.load (i32.const 0x295018)) (i64.const 1000000))))

  (func $now (result i64)
    (drop (call $clock (i32.const 0) (i64.const 1000000) (i32.const 0x295010)))
    (i64.div_u (i64.load (i32.const 0x295010)) (i64.const 1000000000)))

  ;; ---------- writing ----------
  (func $put (param $p i32) (param $n i32)
    (memory.copy (global.get $w) (local.get $p) (local.get $n))
    (global.set $w (i32.add (global.get $w) (local.get $n))))

  (func $putc (param $c i32)
    (i32.store8 (global.get $w) (local.get $c))
    (global.set $w (i32.add (global.get $w) (i32.const 1))))

  (func $puts (param $s i32) (call $put (local.get $s) (call $strlen (local.get $s))))

  (func $putu (param $v i64)
    (local $t i32)
    (local.set $t (i32.const 0x295080))
    (loop $l
      (local.set $t (i32.sub (local.get $t) (i32.const 1)))
      (i64.store8 (local.get $t) (i64.add (i64.const 48) (i64.rem_u (local.get $v) (i64.const 10))))
      (local.set $v (i64.div_u (local.get $v) (i64.const 10)))
      (br_if $l (i64.ne (local.get $v) (i64.const 0))))
    (call $put (local.get $t) (i32.sub (i32.const 0x295080) (local.get $t))))

  (func $hexdig (param $d i32) (result i32)
    (select (i32.add (local.get $d) (i32.const 48)) (i32.add (local.get $d) (i32.const 87))
            (i32.lt_u (local.get $d) (i32.const 10))))

  ;; a JSON string, quoted and escaped
  (func $putjson (param $p i32) (param $n i32)
    (local $e i32) (local $st i32) (local $c i32) (local $v v128)
    (local.set $e (i32.add (local.get $p) (local.get $n)))
    (call $putc (i32.const 34))
    ;; 16 bytes at a time while none of them is '"', '\' or a control character
    (block $slow (loop $fast
      (br_if $slow (i32.gt_u (i32.add (local.get $p) (i32.const 16)) (local.get $e)))
      (local.set $v (v128.load (local.get $p)))
      (br_if $slow (v128.any_true (v128.or (v128.or
        (i8x16.eq (local.get $v) (v128.const i8x16 34 34 34 34 34 34 34 34 34 34 34 34 34 34 34 34))
        (i8x16.eq (local.get $v) (v128.const i8x16 92 92 92 92 92 92 92 92 92 92 92 92 92 92 92 92)))
        (i8x16.lt_u (local.get $v) (v128.const i8x16 32 32 32 32 32 32 32 32 32 32 32 32 32 32 32 32)))))
      (v128.store (global.get $w) (local.get $v))
      (global.set $w (i32.add (global.get $w) (i32.const 16)))
      (local.set $p (i32.add (local.get $p) (i32.const 16)))
      (br $fast)))
    (local.set $st (local.get $p))
    (block $d (loop $l
      (br_if $d (i32.ge_u (local.get $p) (local.get $e)))
      (local.set $c (i32.load8_u (local.get $p)))
      (if (i32.or (i32.or (i32.eq (local.get $c) (i32.const 34)) (i32.eq (local.get $c) (i32.const 92)))
                  (i32.lt_u (local.get $c) (i32.const 32)))
        (then
          (call $put (local.get $st) (i32.sub (local.get $p) (local.get $st)))
          (local.set $st (i32.add (local.get $p) (i32.const 1)))
          (call $putc (i32.const 92))
          (if (i32.eq (local.get $c) (i32.const 34)) (then (call $putc (i32.const 34)))
          (else (if (i32.eq (local.get $c) (i32.const 92)) (then (call $putc (i32.const 92)))
          (else (if (i32.eq (local.get $c) (i32.const 10)) (then (call $putc (i32.const 110)))
          (else (if (i32.eq (local.get $c) (i32.const 13)) (then (call $putc (i32.const 114)))
          (else (if (i32.eq (local.get $c) (i32.const 9)) (then (call $putc (i32.const 116)))
          (else
            (call $putc (i32.const 117)) (call $putc (i32.const 48)) (call $putc (i32.const 48))
            (call $putc (call $hexdig (i32.shr_u (local.get $c) (i32.const 4))))
            (call $putc (call $hexdig (i32.and (local.get $c) (i32.const 15))))))))))))))))
      (local.set $p (i32.add (local.get $p) (i32.const 1)))
      (br $l)))
    (call $put (local.get $st) (i32.sub (local.get $e) (local.get $st)))
    (call $putc (i32.const 34)))

  ;; ---------- responses ----------
  ;; status line + headers + body at $w (which must be the OUTB cursor)
  (func $respond (param $status i32) (param $bp i32) (param $bn i32)
    (call $puts (local.get $status))
    (call $put (i32.const 0x1001c0) (i32.const 48))
    (call $putu (i64.extend_i32_u (local.get $bn)))
    (call $put (i32.const 0x100200) (i32.const 4))
    (call $put (local.get $bp) (local.get $bn)))

  (func $begin (global.set $w (i32.const 0x124000)))

  (func $finish (param $status i32)
    (local $bn i32)
    (local.set $bn (i32.sub (global.get $w) (i32.const 0x124000)))
    (global.set $w (global.get $o))
    (call $respond (local.get $status) (i32.const 0x124000) (local.get $bn))
    (global.set $o (global.get $w)))

  (func $err (param $status i32) (param $msg i32)
    (global.set $w (global.get $o))
    (call $respond (local.get $status) (local.get $msg) (call $strlen (local.get $msg)))
    (global.set $o (global.get $w)))

  (func $post_json (param $s i32)
    (call $puts (i32.const 0x100500))
    (call $putu (call $col_i64 (local.get $s) (i32.const 0)))
    (call $puts (i32.const 0x100540))
    (call $putjson (call $col_text (local.get $s) (i32.const 1)) (call $col_bytes (local.get $s) (i32.const 1)))
    (call $puts (i32.const 0x100580))
    (call $put (call $col_text (local.get $s) (i32.const 2)) (call $col_bytes (local.get $s) (i32.const 2)))
    (call $puts (i32.const 0x1005c0))
    (call $putjson (call $col_text (local.get $s) (i32.const 3)) (call $col_bytes (local.get $s) (i32.const 3)))
    (call $puts (i32.const 0x100600))
    (call $putu (call $col_i64 (local.get $s) (i32.const 4)))
    (call $putc (i32.const 125)))

  ;; positive decimal integer of 1..18 digits, else -1
  (func $parse_id (param $p i32) (param $n i32) (result i64)
    (local $v i64) (local $c i32) (local $e i32)
    (if (i32.or (i32.eqz (local.get $n)) (i32.gt_u (local.get $n) (i32.const 18))) (then (return (i64.const -1))))
    (local.set $e (i32.add (local.get $p) (local.get $n)))
    (loop $l
      (local.set $c (i32.sub (i32.load8_u (local.get $p)) (i32.const 48)))
      (if (i32.gt_u (local.get $c) (i32.const 9)) (then (return (i64.const -1))))
      (local.set $v (i64.add (i64.mul (local.get $v) (i64.const 10)) (i64.extend_i32_u (local.get $c))))
      (local.set $p (i32.add (local.get $p) (i32.const 1)))
      (br_if $l (i32.lt_u (local.get $p) (local.get $e))))
    (select (local.get $v) (i64.const -1) (i64.gt_s (local.get $v) (i64.const 0))))

  ;; ---------- JSON reader (cursor $jp .. $je) ----------
  (func $jws
    (local $c i32)
    (block $d (loop $l
      (br_if $d (i32.ge_u (global.get $jp) (global.get $je)))
      (local.set $c (i32.load8_u (global.get $jp)))
      (br_if $d (i32.eqz (i32.or (i32.eq (local.get $c) (i32.const 32))
        (i32.or (i32.eq (local.get $c) (i32.const 9)) (i32.or (i32.eq (local.get $c) (i32.const 10)) (i32.eq (local.get $c) (i32.const 13)))))))
      (global.set $jp (i32.add (global.get $jp) (i32.const 1)))
      (br $l))))

  (func $hexval (param $c i32) (result i32)
    (if (i32.lt_u (i32.sub (local.get $c) (i32.const 48)) (i32.const 10)) (then (return (i32.sub (local.get $c) (i32.const 48)))))
    (local.set $c (i32.or (local.get $c) (i32.const 32)))
    (if (i32.lt_u (i32.sub (local.get $c) (i32.const 97)) (i32.const 6)) (then (return (i32.sub (local.get $c) (i32.const 87)))))
    (i32.const -1))

  ;; 4 hex digits at $p, or -1
  (func $hex4 (param $p i32) (result i32)
    (local $v i32) (local $i i32) (local $d i32)
    (loop $l
      (local.set $d (call $hexval (i32.load8_u (i32.add (local.get $p) (local.get $i)))))
      (if (i32.lt_s (local.get $d) (i32.const 0)) (then (return (i32.const -1))))
      (local.set $v (i32.or (i32.shl (local.get $v) (i32.const 4)) (local.get $d)))
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (br_if $l (i32.lt_u (local.get $i) (i32.const 4))))
    (local.get $v))

  ;; a string at $jp; unescaped UTF-8 goes to $out unless $out is 0. Returns its byte length, or -1.
  (func $jstr (param $out i32) (result i32)
    (local $o i32) (local $c i32) (local $cp i32) (local $lo i32) (local $k i32)
    (if (i32.ge_u (global.get $jp) (global.get $je)) (then (return (i32.const -1))))
    (if (i32.ne (i32.load8_u (global.get $jp)) (i32.const 34)) (then (return (i32.const -1))))
    (global.set $jp (i32.add (global.get $jp) (i32.const 1)))
    (local.set $o (local.get $out))
    (loop $l
      (if (i32.ge_u (global.get $jp) (global.get $je)) (then (return (i32.const -1))))
      (local.set $c (i32.load8_u (global.get $jp)))
      (global.set $jp (i32.add (global.get $jp) (i32.const 1)))
      (if (i32.eq (local.get $c) (i32.const 34)) (then (return (i32.sub (local.get $o) (local.get $out)))))
      (if (i32.lt_u (local.get $c) (i32.const 32)) (then (return (i32.const -1))))
      (if (i32.ne (local.get $c) (i32.const 92))
        (then
          (if (local.get $out) (then (i32.store8 (local.get $o) (local.get $c))))
          (local.set $o (i32.add (local.get $o) (i32.const 1)))
          (br $l)))
      ;; escape
      (if (i32.ge_u (global.get $jp) (global.get $je)) (then (return (i32.const -1))))
      (local.set $c (i32.load8_u (global.get $jp)))
      (global.set $jp (i32.add (global.get $jp) (i32.const 1)))
      (block $got
        (local.set $cp (local.get $c))
        (br_if $got (i32.or (i32.eq (local.get $c) (i32.const 34)) (i32.or (i32.eq (local.get $c) (i32.const 92)) (i32.eq (local.get $c) (i32.const 47)))))
        (local.set $cp (i32.const 8))  (br_if $got (i32.eq (local.get $c) (i32.const 98)))
        (local.set $cp (i32.const 12)) (br_if $got (i32.eq (local.get $c) (i32.const 102)))
        (local.set $cp (i32.const 10)) (br_if $got (i32.eq (local.get $c) (i32.const 110)))
        (local.set $cp (i32.const 13)) (br_if $got (i32.eq (local.get $c) (i32.const 114)))
        (local.set $cp (i32.const 9))  (br_if $got (i32.eq (local.get $c) (i32.const 116)))
        (if (i32.ne (local.get $c) (i32.const 117)) (then (return (i32.const -1))))
        (if (i32.lt_s (i32.sub (global.get $je) (global.get $jp)) (i32.const 4)) (then (return (i32.const -1))))
        (local.set $cp (call $hex4 (global.get $jp)))
        (if (i32.lt_s (local.get $cp) (i32.const 0)) (then (return (i32.const -1))))
        (global.set $jp (i32.add (global.get $jp) (i32.const 4)))
        ;; a high surrogate followed by \uDC00-\uDFFF makes one code point
        (if (i32.and (i32.eq (i32.and (local.get $cp) (i32.const 0xfc00)) (i32.const 0xd800))
                     (i32.ge_s (i32.sub (global.get $je) (global.get $jp)) (i32.const 6)))
          (then
            (if (i32.and (i32.eq (i32.load8_u (global.get $jp)) (i32.const 92))
                         (i32.eq (i32.load8_u (i32.add (global.get $jp) (i32.const 1))) (i32.const 117)))
              (then
                (local.set $lo (call $hex4 (i32.add (global.get $jp) (i32.const 2))))
                (if (i32.and (i32.ge_s (local.get $lo) (i32.const 0xdc00)) (i32.lt_s (local.get $lo) (i32.const 0xe000)))
                  (then
                    (local.set $cp (i32.add (i32.const 0x10000)
                      (i32.add (i32.shl (i32.sub (local.get $cp) (i32.const 0xd800)) (i32.const 10))
                               (i32.sub (local.get $lo) (i32.const 0xdc00)))))
                    (global.set $jp (i32.add (global.get $jp) (i32.const 6))))))))))
      ;; UTF-8 encode $cp
      (local.set $k
        (if (result i32) (i32.lt_u (local.get $cp) (i32.const 0x80)) (then (i32.const 1))
        (else (if (result i32) (i32.lt_u (local.get $cp) (i32.const 0x800)) (then (i32.const 2))
        (else (if (result i32) (i32.lt_u (local.get $cp) (i32.const 0x10000)) (then (i32.const 3)) (else (i32.const 4))))))))
      (if (local.get $out)
        (then
          (if (i32.eq (local.get $k) (i32.const 1)) (then (i32.store8 (local.get $o) (local.get $cp))))
          (if (i32.eq (local.get $k) (i32.const 2)) (then
            (i32.store8 (local.get $o) (i32.or (i32.const 0xc0) (i32.shr_u (local.get $cp) (i32.const 6))))
            (i32.store8 offset=1 (local.get $o) (i32.or (i32.const 0x80) (i32.and (local.get $cp) (i32.const 63))))))
          (if (i32.eq (local.get $k) (i32.const 3)) (then
            (i32.store8 (local.get $o) (i32.or (i32.const 0xe0) (i32.shr_u (local.get $cp) (i32.const 12))))
            (i32.store8 offset=1 (local.get $o) (i32.or (i32.const 0x80) (i32.and (i32.shr_u (local.get $cp) (i32.const 6)) (i32.const 63))))
            (i32.store8 offset=2 (local.get $o) (i32.or (i32.const 0x80) (i32.and (local.get $cp) (i32.const 63))))))
          (if (i32.eq (local.get $k) (i32.const 4)) (then
            (i32.store8 (local.get $o) (i32.or (i32.const 0xf0) (i32.shr_u (local.get $cp) (i32.const 18))))
            (i32.store8 offset=1 (local.get $o) (i32.or (i32.const 0x80) (i32.and (i32.shr_u (local.get $cp) (i32.const 12)) (i32.const 63))))
            (i32.store8 offset=2 (local.get $o) (i32.or (i32.const 0x80) (i32.and (i32.shr_u (local.get $cp) (i32.const 6)) (i32.const 63))))
            (i32.store8 offset=3 (local.get $o) (i32.or (i32.const 0x80) (i32.and (local.get $cp) (i32.const 63))))))))
      (local.set $o (i32.add (local.get $o) (local.get $k)))
      (br $l))
    (i32.const -1))

  ;; skip any JSON value at $jp: 0 ok, -1 malformed
  (func $jskip (param $depth i32) (result i32)
    (local $c i32) (local $close i32) (local $d i32) (local $left i32)
    (call $jws)
    (if (i32.or (i32.ge_u (global.get $jp) (global.get $je)) (i32.gt_u (local.get $depth) (i32.const 32)))
      (then (return (i32.const -1))))
    (local.set $c (i32.load8_u (global.get $jp)))
    (if (i32.eq (local.get $c) (i32.const 34))
      (then (return (select (i32.const -1) (i32.const 0) (i32.lt_s (call $jstr (i32.const 0)) (i32.const 0))))))
    (if (i32.or (i32.eq (local.get $c) (i32.const 123)) (i32.eq (local.get $c) (i32.const 91)))
      (then
        (local.set $close (i32.add (local.get $c) (i32.const 2)))
        (global.set $jp (i32.add (global.get $jp) (i32.const 1)))
        (call $jws)
        (if (i32.lt_u (global.get $jp) (global.get $je))
          (then (if (i32.eq (i32.load8_u (global.get $jp)) (local.get $close))
            (then (global.set $jp (i32.add (global.get $jp) (i32.const 1))) (return (i32.const 0))))))
        (loop $l
          (if (i32.eq (local.get $c) (i32.const 123))
            (then
              (call $jws)
              (if (i32.lt_s (call $jstr (i32.const 0)) (i32.const 0)) (then (return (i32.const -1))))
              (call $jws)
              (if (i32.ge_u (global.get $jp) (global.get $je)) (then (return (i32.const -1))))
              (if (i32.ne (i32.load8_u (global.get $jp)) (i32.const 58)) (then (return (i32.const -1))))
              (global.set $jp (i32.add (global.get $jp) (i32.const 1)))))
          (if (i32.lt_s (call $jskip (i32.add (local.get $depth) (i32.const 1))) (i32.const 0)) (then (return (i32.const -1))))
          (call $jws)
          (if (i32.ge_u (global.get $jp) (global.get $je)) (then (return (i32.const -1))))
          (local.set $d (i32.load8_u (global.get $jp)))
          (global.set $jp (i32.add (global.get $jp) (i32.const 1)))
          (br_if $l (i32.eq (local.get $d) (i32.const 44)))
          (if (i32.eq (local.get $d) (local.get $close)) (then (return (i32.const 0))))
          (return (i32.const -1)))
        (unreachable)))
    (local.set $left (i32.sub (global.get $je) (global.get $jp)))
    (if (i32.ge_s (local.get $left) (i32.const 4))
      (then
        (if (i32.or (i32.eq (i32.load (global.get $jp)) (i32.const 0x65757274))   ;; "true"
                    (i32.eq (i32.load (global.get $jp)) (i32.const 0x6c6c756e)))  ;; "null"
          (then (global.set $jp (i32.add (global.get $jp) (i32.const 4))) (return (i32.const 0))))))
    (if (i32.ge_s (local.get $left) (i32.const 5))
      (then
        (if (i32.and (i32.eq (i32.load (global.get $jp)) (i32.const 0x736c6166))  ;; "fals"
                     (i32.eq (i32.load8_u offset=4 (global.get $jp)) (i32.const 101)))
          (then (global.set $jp (i32.add (global.get $jp) (i32.const 5))) (return (i32.const 0))))))
    (if (i32.or (i32.eq (local.get $c) (i32.const 45)) (i32.lt_u (i32.sub (local.get $c) (i32.const 48)) (i32.const 10)))
      (then
        (global.set $jp (i32.add (global.get $jp) (i32.const 1)))
        (block $d2 (loop $l2
          (br_if $d2 (i32.ge_u (global.get $jp) (global.get $je)))
          (local.set $c (i32.load8_u (global.get $jp)))
          (br_if $d2 (i32.eqz (i32.or (i32.lt_u (i32.sub (local.get $c) (i32.const 48)) (i32.const 10))
            (i32.or (i32.or (i32.eq (local.get $c) (i32.const 46)) (i32.eq (i32.or (local.get $c) (i32.const 32)) (i32.const 101)))
                    (i32.or (i32.eq (local.get $c) (i32.const 43)) (i32.eq (local.get $c) (i32.const 45)))))))
          (global.set $jp (i32.add (global.get $jp) (i32.const 1)))
          (br $l2)))
        (return (i32.const 0))))
    (i32.const -1))

  ;; Walk the whole top-level object in [$s, $s+$n). If key $key/$kn is there, $jv = its value.
  ;; Returns 1 found, 0 not found, -1 malformed (checked even when the key is found early).
  (func $jfind (param $s i32) (param $n i32) (param $key i32) (param $kn i32) (result i32)
    (local $found i32) (local $ks i32) (local $c i32)
    (global.set $jp (local.get $s))
    (global.set $je (i32.add (local.get $s) (local.get $n)))
    (call $jws)
    (if (i32.ge_u (global.get $jp) (global.get $je)) (then (return (i32.const -1))))
    (if (i32.ne (i32.load8_u (global.get $jp)) (i32.const 123)) (then (return (i32.const -1))))
    (global.set $jp (i32.add (global.get $jp) (i32.const 1)))
    (call $jws)
    (block $end
      (if (i32.lt_u (global.get $jp) (global.get $je))
        (then (if (i32.eq (i32.load8_u (global.get $jp)) (i32.const 125))
          (then (global.set $jp (i32.add (global.get $jp) (i32.const 1))) (br $end)))))
      (loop $l
        (call $jws)
        (local.set $ks (i32.add (global.get $jp) (i32.const 1)))
        (if (i32.lt_s (call $jstr (i32.const 0)) (i32.const 0)) (then (return (i32.const -1))))
        (call $jws)
        (if (i32.ge_u (global.get $jp) (global.get $je)) (then (return (i32.const -1))))
        (if (i32.ne (i32.load8_u (global.get $jp)) (i32.const 58)) (then (return (i32.const -1))))
        (global.set $jp (i32.add (global.get $jp) (i32.const 1)))
        (call $jws)
        (if (i32.eqz (local.get $found))
          (then
            (if (i32.eq (i32.sub (call $keyend (local.get $ks)) (local.get $ks)) (local.get $kn))
              (then (if (call $memeq (local.get $ks) (local.get $key) (local.get $kn))
                (then (global.set $jv (global.get $jp)) (local.set $found (i32.const 1))))))))
        (if (i32.lt_s (call $jskip (i32.const 0)) (i32.const 0)) (then (return (i32.const -1))))
        (call $jws)
        (if (i32.ge_u (global.get $jp) (global.get $je)) (then (return (i32.const -1))))
        (local.set $c (i32.load8_u (global.get $jp)))
        (global.set $jp (i32.add (global.get $jp) (i32.const 1)))
        (br_if $l (i32.eq (local.get $c) (i32.const 44)))
        (br_if $end (i32.eq (local.get $c) (i32.const 125)))
        (return (i32.const -1))))
    (call $jws)
    (if (i32.ne (global.get $jp) (global.get $je)) (then (return (i32.const -1))))
    (local.get $found))

  ;; end of the raw key that starts at $ks (its closing quote; the key was already validated)
  (func $keyend (param $ks i32) (result i32)
    (local $c i32)
    (loop $l
      (local.set $c (i32.load8_u (local.get $ks)))
      (if (i32.eq (local.get $c) (i32.const 34)) (then (return (local.get $ks))))
      (local.set $ks (i32.add (local.get $ks)
        (select (i32.const 2) (i32.const 1) (i32.eq (local.get $c) (i32.const 92)))))
      (br $l))
    (unreachable))

  ;; ---------- base64url ----------
  (func $b64init
    (local $i i32)
    (memory.fill (i32.const 0x284d00) (i32.const 255) (i32.const 256))
    (loop $l
      (i32.store8 (i32.add (i32.const 0x284d00) (i32.load8_u (i32.add (i32.const 0x101000) (local.get $i)))) (local.get $i))
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (br_if $l (i32.lt_u (local.get $i) (i32.const 64)))))

  (func $b64v (param $p i32) (result i32) (i32.load8_u (i32.add (i32.const 0x284d00) (i32.load8_u (local.get $p)))))

  ;; base64url without padding, [$s, $s+$n) -> $out (at most $cap bytes). Returns length or -1.
  (func $b64dec (param $s i32) (param $n i32) (param $out i32) (param $cap i32) (result i32)
    (local $e i32) (local $o i32) (local $v i32) (local $r i32)
    (local.set $e (i32.add (local.get $s) (local.get $n)))
    (local.set $o (local.get $out))
    (if (i32.gt_u (local.get $n) (i32.div_u (i32.mul (local.get $cap) (i32.const 4)) (i32.const 3))) (then (return (i32.const -1))))
    (block $d (loop $l
      (br_if $d (i32.lt_s (i32.sub (local.get $e) (local.get $s)) (i32.const 4)))
      (local.set $v (i32.or (i32.or (i32.shl (call $b64v (local.get $s)) (i32.const 18))
                                    (i32.shl (call $b64v (i32.add (local.get $s) (i32.const 1))) (i32.const 12)))
                            (i32.or (i32.shl (call $b64v (i32.add (local.get $s) (i32.const 2))) (i32.const 6))
                                    (call $b64v (i32.add (local.get $s) (i32.const 3))))))
      (if (i32.ge_u (local.get $v) (i32.const 0x1000000)) (then (return (i32.const -1))))
      (if (i32.and (i32.or (i32.or (call $b64v (local.get $s)) (call $b64v (i32.add (local.get $s) (i32.const 1))))
                           (i32.or (call $b64v (i32.add (local.get $s) (i32.const 2))) (call $b64v (i32.add (local.get $s) (i32.const 3)))))
                   (i32.const 0xc0))
        (then (return (i32.const -1))))
      (i32.store8 (local.get $o) (i32.shr_u (local.get $v) (i32.const 16)))
      (i32.store8 offset=1 (local.get $o) (i32.shr_u (local.get $v) (i32.const 8)))
      (i32.store8 offset=2 (local.get $o) (local.get $v))
      (local.set $o (i32.add (local.get $o) (i32.const 3)))
      (local.set $s (i32.add (local.get $s) (i32.const 4)))
      (br $l)))
    (local.set $r (i32.sub (local.get $e) (local.get $s)))
    (if (i32.eq (local.get $r) (i32.const 1)) (then (return (i32.const -1))))
    (if (i32.ge_u (local.get $r) (i32.const 2))
      (then
        (local.set $v (i32.or (call $b64v (local.get $s)) (call $b64v (i32.add (local.get $s) (i32.const 1)))))
        (if (i32.eq (local.get $r) (i32.const 3)) (then (local.set $v (i32.or (local.get $v) (call $b64v (i32.add (local.get $s) (i32.const 2)))))))
        (if (i32.and (local.get $v) (i32.const 0xc0)) (then (return (i32.const -1))))
        (local.set $v (i32.or (i32.shl (call $b64v (local.get $s)) (i32.const 18)) (i32.shl (call $b64v (i32.add (local.get $s) (i32.const 1))) (i32.const 12))))
        (if (i32.eq (local.get $r) (i32.const 3)) (then (local.set $v (i32.or (local.get $v) (i32.shl (call $b64v (i32.add (local.get $s) (i32.const 2))) (i32.const 6))))))
        (i32.store8 (local.get $o) (i32.shr_u (local.get $v) (i32.const 16)))
        (local.set $o (i32.add (local.get $o) (i32.const 1)))
        (if (i32.eq (local.get $r) (i32.const 3)) (then
          (i32.store8 (local.get $o) (i32.shr_u (local.get $v) (i32.const 8)))
          (local.set $o (i32.add (local.get $o) (i32.const 1)))))))
    (i32.sub (local.get $o) (local.get $out)))

  (func $b64c (param $v i32) (result i32) (i32.load8_u (i32.add (i32.const 0x101000) (i32.and (local.get $v) (i32.const 63)))))

  ;; 32 bytes at $in -> 43 base64url chars at $out
  (func $b64enc32 (param $in i32) (param $out i32)
    (local $i i32) (local $v i32)
    (loop $l
      (local.set $v (i32.or (i32.or (i32.shl (i32.load8_u (local.get $in)) (i32.const 16))
                                    (i32.shl (i32.load8_u offset=1 (local.get $in)) (i32.const 8)))
                            (i32.load8_u offset=2 (local.get $in))))
      (i32.store8 (local.get $out) (call $b64c (i32.shr_u (local.get $v) (i32.const 18))))
      (i32.store8 offset=1 (local.get $out) (call $b64c (i32.shr_u (local.get $v) (i32.const 12))))
      (i32.store8 offset=2 (local.get $out) (call $b64c (i32.shr_u (local.get $v) (i32.const 6))))
      (i32.store8 offset=3 (local.get $out) (call $b64c (local.get $v)))
      (local.set $in (i32.add (local.get $in) (i32.const 3)))
      (local.set $out (i32.add (local.get $out) (i32.const 4)))
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (br_if $l (i32.lt_u (local.get $i) (i32.const 10))))
    (local.set $v (i32.or (i32.shl (i32.load8_u (local.get $in)) (i32.const 16)) (i32.shl (i32.load8_u offset=1 (local.get $in)) (i32.const 8))))
    (i32.store8 (local.get $out) (call $b64c (i32.shr_u (local.get $v) (i32.const 18))))
    (i32.store8 offset=1 (local.get $out) (call $b64c (i32.shr_u (local.get $v) (i32.const 12))))
    (i32.store8 offset=2 (local.get $out) (call $b64c (i32.shr_u (local.get $v) (i32.const 6)))))

  ;; ---------- SHA-256 / HMAC ----------
  ;; state at $h (8 i32), one 64-byte block at $p. Schedule in 0x284700.
  (func $sha_block (param $h i32) (param $p i32)
    (local $a i32) (local $b i32) (local $c i32) (local $d i32) (local $e i32) (local $f i32) (local $g i32) (local $hh i32)
    (local $t i32) (local $x i32) (local $y i32) (local $t1 i32) (local $t2 i32) (local $v v128)
    (v128.store (i32.const 0x284700) (i8x16.shuffle 3 2 1 0 7 6 5 4 11 10 9 8 15 14 13 12
      (local.tee $v (v128.load (local.get $p))) (local.get $v)))
    (v128.store (i32.const 0x284710) (i8x16.shuffle 3 2 1 0 7 6 5 4 11 10 9 8 15 14 13 12
      (local.tee $v (v128.load offset=16 (local.get $p))) (local.get $v)))
    (v128.store (i32.const 0x284720) (i8x16.shuffle 3 2 1 0 7 6 5 4 11 10 9 8 15 14 13 12
      (local.tee $v (v128.load offset=32 (local.get $p))) (local.get $v)))
    (v128.store (i32.const 0x284730) (i8x16.shuffle 3 2 1 0 7 6 5 4 11 10 9 8 15 14 13 12
      (local.tee $v (v128.load offset=48 (local.get $p))) (local.get $v)))
    ;; W[t] = s1(W[t-2]) + W[t-7] + s0(W[t-15]) + W[t-16], t = 16..63 ($t is the byte offset of W[t])
    (local.set $t (i32.const 0x284740))
    (loop $s
      (local.set $x (i32.load offset=4 (i32.sub (local.get $t) (i32.const 64))))   ;; W[t-15]
      (local.set $y (i32.load offset=56 (i32.sub (local.get $t) (i32.const 64))))  ;; W[t-2]
      (i32.store (local.get $t)
        (i32.add
          (i32.add (i32.load (i32.sub (local.get $t) (i32.const 64)))                 ;; W[t-16]
                   (i32.load offset=36 (i32.sub (local.get $t) (i32.const 64))))      ;; W[t-7]
          (i32.add
            (i32.xor (i32.xor (i32.rotr (local.get $x) (i32.const 7)) (i32.rotr (local.get $x) (i32.const 18))) (i32.shr_u (local.get $x) (i32.const 3)))
            (i32.xor (i32.xor (i32.rotr (local.get $y) (i32.const 17)) (i32.rotr (local.get $y) (i32.const 19))) (i32.shr_u (local.get $y) (i32.const 10))))))
      (local.set $t (i32.add (local.get $t) (i32.const 4)))
      (br_if $s (i32.lt_u (local.get $t) (i32.const 0x284800))))
    (local.set $a (i32.load (local.get $h)))
    (local.set $b (i32.load offset=4 (local.get $h)))
    (local.set $c (i32.load offset=8 (local.get $h)))
    (local.set $d (i32.load offset=12 (local.get $h)))
    (local.set $e (i32.load offset=16 (local.get $h)))
    (local.set $f (i32.load offset=20 (local.get $h)))
    (local.set $g (i32.load offset=24 (local.get $h)))
    (local.set $hh (i32.load offset=28 (local.get $h)))
    (local.set $t (i32.const 0))
    (loop $r
      (local.set $t1
        (i32.add
          (i32.add (local.get $hh)
            (i32.xor (i32.xor (i32.rotr (local.get $e) (i32.const 6)) (i32.rotr (local.get $e) (i32.const 11))) (i32.rotr (local.get $e) (i32.const 25))))
          (i32.add
            (i32.xor (i32.and (local.get $e) (local.get $f)) (i32.and (i32.xor (local.get $e) (i32.const -1)) (local.get $g)))
            (i32.add (i32.load offset=0x284b00 (local.get $t)) (i32.load offset=0x284700 (local.get $t))))))
      (local.set $t2
        (i32.add
          (i32.xor (i32.xor (i32.rotr (local.get $a) (i32.const 2)) (i32.rotr (local.get $a) (i32.const 13))) (i32.rotr (local.get $a) (i32.const 22)))
          (i32.xor (i32.xor (i32.and (local.get $a) (local.get $b)) (i32.and (local.get $a) (local.get $c))) (i32.and (local.get $b) (local.get $c)))))
      (local.set $hh (local.get $g))
      (local.set $g (local.get $f))
      (local.set $f (local.get $e))
      (local.set $e (i32.add (local.get $d) (local.get $t1)))
      (local.set $d (local.get $c))
      (local.set $c (local.get $b))
      (local.set $b (local.get $a))
      (local.set $a (i32.add (local.get $t1) (local.get $t2)))
      (local.set $t (i32.add (local.get $t) (i32.const 4)))
      (br_if $r (i32.lt_u (local.get $t) (i32.const 256))))
    (i32.store (local.get $h) (i32.add (i32.load (local.get $h)) (local.get $a)))
    (i32.store offset=4 (local.get $h) (i32.add (i32.load offset=4 (local.get $h)) (local.get $b)))
    (i32.store offset=8 (local.get $h) (i32.add (i32.load offset=8 (local.get $h)) (local.get $c)))
    (i32.store offset=12 (local.get $h) (i32.add (i32.load offset=12 (local.get $h)) (local.get $d)))
    (i32.store offset=16 (local.get $h) (i32.add (i32.load offset=16 (local.get $h)) (local.get $e)))
    (i32.store offset=20 (local.get $h) (i32.add (i32.load offset=20 (local.get $h)) (local.get $f)))
    (i32.store offset=24 (local.get $h) (i32.add (i32.load offset=24 (local.get $h)) (local.get $g)))
    (i32.store offset=28 (local.get $h) (i32.add (i32.load offset=28 (local.get $h)) (local.get $hh))))

  (func $bswap (param $x i32) (result i32)
    (i32.or (i32.or (i32.shl (local.get $x) (i32.const 24)) (i32.and (i32.shl (local.get $x) (i32.const 8)) (i32.const 0xff0000)))
            (i32.or (i32.and (i32.shr_u (local.get $x) (i32.const 8)) (i32.const 0xff00)) (i32.shr_u (local.get $x) (i32.const 24)))))

  ;; finish the hash in 0x284600 over [$p, $p+$n); $total = all bytes hashed including earlier blocks
  (func $sha_final (param $p i32) (param $n i32) (param $total i32)
    (local $blocks i32)
    (block $d (loop $l
      (br_if $d (i32.lt_u (local.get $n) (i32.const 64)))
      (call $sha_block (i32.const 0x284600) (local.get $p))
      (local.set $p (i32.add (local.get $p) (i32.const 64)))
      (local.set $n (i32.sub (local.get $n) (i32.const 64)))
      (br $l)))
    (memory.fill (i32.const 0x284800) (i32.const 0) (i32.const 128))
    (memory.copy (i32.const 0x284800) (local.get $p) (local.get $n))
    (i32.store8 (i32.add (i32.const 0x284800) (local.get $n)) (i32.const 0x80))
    (local.set $blocks (select (i32.const 2) (i32.const 1) (i32.ge_u (local.get $n) (i32.const 56))))
    (i32.store (i32.add (i32.const 0x2847fc) (i32.shl (local.get $blocks) (i32.const 6)))
               (call $bswap (i32.shl (local.get $total) (i32.const 3))))
    (call $sha_block (i32.const 0x284600) (i32.const 0x284800))
    (if (i32.eq (local.get $blocks) (i32.const 2)) (then (call $sha_block (i32.const 0x284600) (i32.const 0x284840)))))

  (func $digest_out (param $out i32)
    (local $v v128)
    (v128.store (local.get $out) (i8x16.shuffle 3 2 1 0 7 6 5 4 11 10 9 8 15 14 13 12
      (local.tee $v (v128.load (i32.const 0x284600))) (local.get $v)))
    (v128.store offset=16 (local.get $out) (i8x16.shuffle 3 2 1 0 7 6 5 4 11 10 9 8 15 14 13 12
      (local.tee $v (v128.load (i32.const 0x284610))) (local.get $v))))

  ;; HMAC key -> inner state 0x284a00 and outer state 0x284a20 (after the padded key block)
  (func $hmac_init (param $key i32) (param $kn i32)
    (local $i i32)
    (if (i32.gt_u (local.get $kn) (i32.const 64))
      (then
        (memory.copy (i32.const 0x284600) (i32.const 0x284c00) (i32.const 32))
        (call $sha_final (local.get $key) (local.get $kn) (local.get $kn))
        (call $digest_out (i32.const 0x284a40))
        (local.set $key (i32.const 0x284a40))
        (local.set $kn (i32.const 32))))
    (memory.fill (i32.const 0x284880) (i32.const 0) (i32.const 64))
    (memory.copy (i32.const 0x284880) (local.get $key) (local.get $kn))
    ;; inner
    (local.set $i (i32.const 0))
    (loop $l
      (i32.store8 offset=0x284800 (local.get $i) (i32.xor (i32.load8_u offset=0x284880 (local.get $i)) (i32.const 0x36)))
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (br_if $l (i32.lt_u (local.get $i) (i32.const 64))))
    (memory.copy (i32.const 0x284a00) (i32.const 0x284c00) (i32.const 32))
    (call $sha_block (i32.const 0x284a00) (i32.const 0x284800))
    ;; outer
    (local.set $i (i32.const 0))
    (loop $l2
      (i32.store8 offset=0x284800 (local.get $i) (i32.xor (i32.load8_u offset=0x284880 (local.get $i)) (i32.const 0x5c)))
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (br_if $l2 (i32.lt_u (local.get $i) (i32.const 64))))
    (memory.copy (i32.const 0x284a20) (i32.const 0x284c00) (i32.const 32))
    (call $sha_block (i32.const 0x284a20) (i32.const 0x284800)))

  ;; HMAC-SHA256 of [$p, $p+$n) -> 32 bytes at 0x284900
  (func $hmac (param $p i32) (param $n i32)
    (memory.copy (i32.const 0x284600) (i32.const 0x284a00) (i32.const 32))
    (call $sha_final (local.get $p) (local.get $n) (i32.add (local.get $n) (i32.const 64)))
    (call $digest_out (i32.const 0x284900))
    (memory.copy (i32.const 0x284600) (i32.const 0x284a20) (i32.const 32))
    (call $sha_final (i32.const 0x284900) (i32.const 32) (i32.const 96))
    (call $digest_out (i32.const 0x284900)))

  ;; ---------- auth ----------
  ;; 1 if the Authorization header holds a valid token ($uid, $un_p/$un_n set); else writes the 401 and returns 0
  (func $auth (param $h i32) (param $hn i32) (result i32)
    (local $t i32) (local $te i32) (local $d1 i32) (local $d2 i32) (local $hl i32) (local $pl i32)
    (local $v i32) (local $c i32) (local $exp i64) (local $s0 i32)
    (if (i32.or (i32.or (i32.eqz (local.get $h)) (i32.lt_u (local.get $hn) (i32.const 7)))
                (i32.eqz (call $memeq (local.get $h) (i32.const 0x100f60) (i32.const 7))))
      (then (call $err (i32.const 0x1000c0) (i32.const 0x100300)) (return (i32.const 0))))
    (local.set $t (i32.add (local.get $h) (i32.const 7)))
    (local.set $te (i32.add (local.get $h) (local.get $hn)))
    (block $bad
      (block $payload
        (local.set $d1 (call $memchr (local.get $t) (i32.const 46) (local.get $te)))
        (br_if $bad (i32.eqz (local.get $d1)))
        (local.set $d2 (call $memchr (i32.add (local.get $d1) (i32.const 1)) (i32.const 46) (local.get $te)))
        (br_if $bad (i32.eqz (local.get $d2)))
        (br_if $bad (call $memchr (i32.add (local.get $d2) (i32.const 1)) (i32.const 46) (local.get $te)))
        (local.set $hl (call $b64dec (local.get $t) (i32.sub (local.get $d1) (local.get $t)) (i32.const 0x284000) (i32.const 256)))
        (br_if $bad (i32.lt_s (local.get $hl) (i32.const 0)))
        (local.set $pl (call $b64dec (i32.add (local.get $d1) (i32.const 1)) (i32.sub (i32.sub (local.get $d2) (local.get $d1)) (i32.const 1))
                                     (i32.const 0x284100) (i32.const 1024)))
        (br_if $bad (i32.lt_s (local.get $pl) (i32.const 0)))
        ;; header: {"alg":"HS256",...}
        (br_if $bad (i32.ne (call $jfind (i32.const 0x284000) (local.get $hl) (i32.const 0x100f00) (i32.const 3)) (i32.const 1)))
        (local.set $v (global.get $jv))
        (br_if $bad (i32.ne (i32.load8_u (local.get $v)) (i32.const 34)))
        (br_if $bad (i32.eqz (call $memeq (i32.add (local.get $v) (i32.const 1)) (i32.const 0x100f50) (i32.const 5))))
        (br_if $bad (i32.ne (i32.load8_u offset=6 (local.get $v)) (i32.const 34)))
        ;; signature
        (br_if $bad (i32.ne (i32.sub (i32.sub (local.get $te) (local.get $d2)) (i32.const 1)) (i32.const 43)))
        (call $hmac (local.get $t) (i32.sub (local.get $d2) (local.get $t)))
        (call $b64enc32 (i32.const 0x284900) (i32.const 0x284940))
        (br_if $bad (i32.eqz (call $memeq (i32.const 0x284940) (i32.add (local.get $d2) (i32.const 1)) (i32.const 43))))
        ;; exp, a number later than now
        (br_if $bad (i32.ne (call $jfind (i32.const 0x284100) (local.get $pl) (i32.const 0x100f10) (i32.const 3)) (i32.const 1)))
        (local.set $v (global.get $jv))
        (br_if $bad (i32.gt_u (i32.sub (i32.load8_u (local.get $v)) (i32.const 48)) (i32.const 9)))
        (block $dd (loop $l
          (local.set $c (i32.sub (i32.load8_u (local.get $v)) (i32.const 48)))
          (br_if $dd (i32.gt_u (local.get $c) (i32.const 9)))
          (br_if $dd (i64.gt_u (local.get $exp) (i64.const 100000000000000)))
          (local.set $exp (i64.add (i64.mul (local.get $exp) (i64.const 10)) (i64.extend_i32_u (local.get $c))))
          (local.set $v (i32.add (local.get $v) (i32.const 1)))
          (br $l)))
        (br_if $bad (i64.le_s (local.get $exp) (call $now)))
        ;; sub: a string holding a positive integer
        (br_if $payload (i32.ne (call $jfind (i32.const 0x284100) (local.get $pl) (i32.const 0x100f20) (i32.const 3)) (i32.const 1)))
        (global.set $jp (global.get $jv))
        (local.set $s0 (i32.add (global.get $jv) (i32.const 1)))
        (br_if $payload (i32.lt_s (call $jstr (i32.const 0)) (i32.const 0)))
        (global.set $uid (call $parse_id (local.get $s0) (i32.sub (i32.sub (global.get $jp) (i32.const 1)) (local.get $s0))))
        (br_if $payload (i64.lt_s (global.get $uid) (i64.const 0)))
        ;; username: a string, kept as its raw JSON bytes
        (br_if $payload (i32.ne (call $jfind (i32.const 0x284100) (local.get $pl) (i32.const 0x100f30) (i32.const 8)) (i32.const 1)))
        (global.set $jp (global.get $jv))
        (local.set $s0 (i32.add (global.get $jv) (i32.const 1)))
        (br_if $payload (i32.lt_s (call $jstr (i32.const 0)) (i32.const 0)))
        (global.set $un_p (local.get $s0))
        (global.set $un_n (i32.sub (i32.sub (global.get $jp) (i32.const 1)) (local.get $s0)))
        (return (i32.const 1)))
      (call $err (i32.const 0x1000c0) (i32.const 0x100380))
      (return (i32.const 0)))
    (call $err (i32.const 0x1000c0) (i32.const 0x100340))
    (i32.const 0))

  ;; ---------- routes ----------
;; ---------- reads on SQLite's B-tree layer ----------
  ;; /feed and GET /posts/:id read the tables and indexes through SQLite's own cursors (pager, page cache,
  ;; WAL and locks are SQLite's; only the bytecode engine is skipped), inside the batch's transaction.
  ;; Nothing is kept between requests but root pages, checked against the schema cookie on every call.
  ;; A record this code doesn't cover (overflow pages, an unexpected type) sends the request to SQL.

  ;; SQLite varint at $p (https://sqlite.org/fileformat2.html#varint); its length goes to $vn
  (func $gv (param $p i32) (result i64)
    (local $v i64) (local $i i32) (local $b i32)
    (loop $l
      (local.set $b (i32.load8_u (i32.add (local.get $p) (local.get $i))))
      (if (i32.eq (local.get $i) (i32.const 8))
        (then
          (global.set $vn (i32.const 9))
          (return (i64.or (i64.shl (local.get $v) (i64.const 8)) (i64.extend_i32_u (local.get $b))))))
      (local.set $v (i64.or (i64.shl (local.get $v) (i64.const 7)) (i64.extend_i32_u (i32.and (local.get $b) (i32.const 0x7f)))))
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (br_if $l (i32.ge_u (local.get $b) (i32.const 0x80))))
    (global.set $vn (local.get $i))
    (local.get $v))

  ;; decode the first $n columns of the record under cursor $c into COLS (0x2e0000 + 32*i: serial type,
  ;; pointer, length, integer value). 0: not covered (overflow page, malformed, float key types 10/11)
  (func $record (param $c i32) (param $n i32) (result i32)
    (local $size i32) (local $p i32) (local $hdr i32) (local $h i32) (local $d i32) (local $end i32)
    (local $i i32) (local $t i32) (local $len i32) (local $v i64) (local $k i32) (local $col i32)
    (local.set $size (call $cur_psize (local.get $c)))
    (local.set $p (call $cur_payload (local.get $c) (i32.const 0x2e0104)))
    (if (i32.or (i32.lt_u (i32.load (i32.const 0x2e0104)) (local.get $size)) (i32.lt_u (local.get $size) (i32.const 1)))
      (then (return (i32.const 0))))
    (local.set $hdr (i32.wrap_i64 (call $gv (local.get $p))))
    (local.set $h (global.get $vn))
    (if (i32.or (i32.gt_u (local.get $hdr) (local.get $size)) (i32.lt_u (local.get $hdr) (local.get $h)))
      (then (return (i32.const 0))))
    (local.set $d (i32.add (local.get $p) (local.get $hdr)))
    (local.set $end (i32.add (local.get $p) (local.get $size)))
    (block $done (loop $l
      (br_if $done (i32.ge_u (local.get $i) (local.get $n)))
      (if (i32.ge_u (local.get $h) (local.get $hdr)) (then (return (i32.const 0))))
      (local.set $t (i32.wrap_i64 (call $gv (i32.add (local.get $p) (local.get $h)))))
      (local.set $h (i32.add (local.get $h) (global.get $vn)))
      (local.set $v (i64.const 0))
      (if (i32.ge_u (local.get $t) (i32.const 12))
        (then (local.set $len (i32.shr_u (i32.sub (local.get $t) (i32.const 12)) (i32.const 1))))
        (else
          (if (i32.ge_u (local.get $t) (i32.const 10)) (then (return (i32.const 0))))
          ;; sizes of serial types 0..7: 0 1 2 3 4 6 8 8 (one byte each, in an i64); 8 and 9 have none
          (local.set $len (select (i32.const 0)
            (i32.and (i32.wrap_i64 (i64.shr_u (i64.const 0x0808060403020100)
                                              (i64.extend_i32_u (i32.shl (local.get $t) (i32.const 3)))))
                     (i32.const 0xff))
            (i32.ge_u (local.get $t) (i32.const 8))))
          (if (i32.eq (local.get $t) (i32.const 9)) (then (local.set $v (i64.const 1))))
          (if (i32.and (i32.ge_u (local.get $t) (i32.const 1)) (i32.le_u (local.get $t) (i32.const 6)))
            (then
              (if (i32.gt_u (i32.add (local.get $d) (local.get $len)) (local.get $end)) (then (return (i32.const 0))))
              (local.set $v (i64.extend8_s (i64.load8_u (local.get $d))))
              (local.set $k (i32.const 1))
              (block $bd (loop $bl
                (br_if $bd (i32.ge_u (local.get $k) (local.get $len)))
                (local.set $v (i64.or (i64.shl (local.get $v) (i64.const 8))
                                      (i64.load8_u (i32.add (local.get $d) (local.get $k)))))
                (local.set $k (i32.add (local.get $k) (i32.const 1)))
                (br $bl)))))))
      (if (i32.gt_u (i32.add (local.get $d) (local.get $len)) (local.get $end)) (then (return (i32.const 0))))
      (local.set $col (i32.add (i32.const 0x2e0000) (i32.shl (local.get $i) (i32.const 5))))
      (i32.store (local.get $col) (local.get $t))
      (i32.store offset=4 (local.get $col) (local.get $d))
      (i32.store offset=8 (local.get $col) (local.get $len))
      (i64.store offset=16 (local.get $col) (local.get $v))
      (local.set $d (i32.add (local.get $d) (local.get $len)))
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (br $l)))
    (i32.const 1))

  (func $col_t (param $i i32) (result i32) (i32.load (i32.add (i32.const 0x2e0000) (i32.shl (local.get $i) (i32.const 5)))))
  (func $col_p (param $i i32) (result i32) (i32.load offset=4 (i32.add (i32.const 0x2e0000) (i32.shl (local.get $i) (i32.const 5)))))
  (func $col_n (param $i i32) (result i32) (i32.load offset=8 (i32.add (i32.const 0x2e0000) (i32.shl (local.get $i) (i32.const 5)))))
  (func $col_v (param $i i32) (result i64) (i64.load offset=16 (i32.add (i32.const 0x2e0000) (i32.shl (local.get $i) (i32.const 5)))))
  ;; integer serial types 1..6, 8, 9; text: odd types from 13
  (func $c_int (param $i i32) (result i32)
    (local $t i32) (local.set $t (call $col_t (local.get $i)))
    (i32.or (i32.and (i32.ge_u (local.get $t) (i32.const 1)) (i32.le_u (local.get $t) (i32.const 6)))
            (i32.or (i32.eq (local.get $t) (i32.const 8)) (i32.eq (local.get $t) (i32.const 9)))))
  (func $c_text (param $i i32) (result i32)
    (local $t i32) (local.set $t (call $col_t (local.get $i)))
    (i32.and (i32.ge_u (local.get $t) (i32.const 13)) (i32.and (local.get $t) (i32.const 1))))

  ;; join the batch's read transaction; 0 if B-tree reads are off or the schema changed
  (func $bt_begin (result i32)
    (if (i32.eqz (global.get $bt)) (then (return (i32.const 0))))
    (if (call $autocommit (global.get $db)) (then (return (i32.const 0))))
    (if (call $bt_read (global.get $bt) (i32.const 0x2e0108)) (then (return (i32.const 0))))
    (i32.eq (i32.load (i32.const 0x2e0108)) (global.get $cookie)))

  ;; open the four cursors (zeroed first, so closing one never opened is a no-op); 0 on failure
  (func $bt_open (result i32)
    (memory.fill (global.get $cu_t) (i32.const 0) (i32.shl (global.get $csz) (i32.const 2)))
    (i32.eqz (i32.or (i32.or (call $cur_open (global.get $bt) (global.get $r_top) (global.get $k_top) (global.get $cu_t))
                             (call $cur_open (global.get $bt) (global.get $r_likes) (global.get $k_likes) (global.get $cu_l)))
                     (i32.or (call $cur_open (global.get $bt) (global.get $r_posts) (i32.const 0) (global.get $cu_p))
                             (call $cur_open (global.get $bt) (global.get $r_users) (i32.const 0) (global.get $cu_u))))))

  (func $bt_close
    (drop (call $cur_close (global.get $cu_t))) (drop (call $cur_close (global.get $cu_l)))
    (drop (call $cur_close (global.get $cu_p))) (drop (call $cur_close (global.get $cu_u))))

  ;; add the likes of post ids $lo..$hi to the i64 counters at $cnt + 8*(id - $lo); 0 ok, -1 error
  (func $likes_count (param $lo i64) (param $hi i64) (param $cnt i32) (result i32)
    (local $rc i32) (local $v i64) (local $a i32)
    ;; default_rc 1: entries equal to the key sort after it, so the seek lands at or before the first one
    (if (call $cur_seek_int (global.get $cu_l) (global.get $k_likes) (local.get $lo) (i32.const 1) (i32.const 0x2e0100))
      (then (return (i32.const -1))))
    (if (call $cur_eof (global.get $cu_l)) (then (return (i32.const 0))))
    (if (i32.lt_s (i32.load (i32.const 0x2e0100)) (i32.const 0))
      (then
        (local.set $rc (call $cur_next (global.get $cu_l)))
        (if (i32.eq (local.get $rc) (i32.const 101)) (then (return (i32.const 0))))
        (if (local.get $rc) (then (return (i32.const -1))))))
    (loop $l
      (if (i32.eqz (call $record (global.get $cu_l) (i32.const 1))) (then (return (i32.const -1))))
      (if (i32.eqz (call $c_int (i32.const 0))) (then (return (i32.const -1))))
      (local.set $v (call $col_v (i32.const 0)))
      (if (i64.gt_s (local.get $v) (local.get $hi)) (then (return (i32.const 0))))
      (if (i64.ge_s (local.get $v) (local.get $lo))
        (then
          (local.set $a (i32.add (local.get $cnt) (i32.shl (i32.wrap_i64 (i64.sub (local.get $v) (local.get $lo))) (i32.const 3))))
          (i64.store (local.get $a) (i64.add (i64.load (local.get $a)) (i64.const 1)))))
      (local.set $rc (call $cur_next (global.get $cu_l)))
      (if (i32.eq (local.get $rc) (i32.const 101)) (then (return (i32.const 0))))
      (br_if $l (i32.eqz (local.get $rc))))
    (i32.const -1))

  ;; write one post as JSON from the posts row under $cu_p, its author and like count; 0: not covered
  (func $bt_post_out (param $id i64) (param $likes i64) (result i32)
    (local $bp i32) (local $bn i32) (local $cp i32) (local $cn i32)
    (if (i32.eqz (call $record (global.get $cu_p) (i32.const 4))) (then (return (i32.const 0))))
    (if (i32.eqz (i32.and (call $c_int (i32.const 1))
                          (i32.and (call $c_text (i32.const 2)) (call $c_text (i32.const 3)))))
      (then (return (i32.const 0))))
    (local.set $bp (call $col_p (i32.const 2))) (local.set $bn (call $col_n (i32.const 2)))
    (local.set $cp (call $col_p (i32.const 3))) (local.set $cn (call $col_n (i32.const 3)))
    (if (call $cur_seek_rowid (global.get $cu_u) (call $col_v (i32.const 1)) (i32.const 0x2e0100)) (then (return (i32.const 0))))
    (if (i32.load (i32.const 0x2e0100)) (then (return (i32.const 0))))   ;; no such user: let SQL's join decide
    (if (i32.eqz (call $record (global.get $cu_u) (i32.const 2))) (then (return (i32.const 0))))
    (if (i32.eqz (call $c_text (i32.const 1))) (then (return (i32.const 0))))
    (call $put (i32.const 0x100500) (i32.const 6))
    (call $putu (local.get $id))
    (call $put (i32.const 0x100540) (i32.const 8))
    (call $putjson (local.get $bp) (local.get $bn))
    (call $put (i32.const 0x100580) (i32.const 15))
    (call $put (local.get $cp) (local.get $cn))
    (call $put (i32.const 0x1005c0) (i32.const 11))
    (call $putjson (call $col_p (i32.const 1)) (call $col_n (i32.const 1)))
    (call $put (i32.const 0x100600) (i32.const 14))
    (call $putu (local.get $likes))
    (call $putc (i32.const 125))
    (i32.const 1))

  ;; /feed body into BODYB; 0: not covered (the caller starts over with SQL)
  (func $bt_feed (result i32)
    (local $n i32) (local $rc i32) (local $i i32) (local $lo i64) (local $hi i64) (local $id i64) (local $ok i32)
    (if (i32.eqz (call $bt_begin)) (then (return (i32.const 0))))
    (block $out
      (br_if $out (i32.eqz (call $bt_open)))
      ;; the index is (created_at DESC, id DESC): its first 20 entries are the newest posts
      (local.set $rc (call $cur_first (global.get $cu_t) (i32.const 0x2e0100)))
      (br_if $out (local.get $rc))
      (if (i32.eqz (i32.load (i32.const 0x2e0100)))
        (then (block $d (loop $l
          (br_if $d (i32.ge_u (local.get $n) (i32.const 20)))
          (br_if $out (i32.eqz (call $record (global.get $cu_t) (i32.const 2))))
          (br_if $out (i32.eqz (call $c_int (i32.const 1))))
          (i64.store (i32.add (i32.const 0x2e0200) (i32.shl (local.get $n) (i32.const 3))) (call $col_v (i32.const 1)))
          (local.set $n (i32.add (local.get $n) (i32.const 1)))
          (local.set $rc (call $cur_next (global.get $cu_t)))
          (br_if $d (i32.eq (local.get $rc) (i32.const 101)))
          (br_if $out (local.get $rc))
          (br $l)))))
      ;; like counts: one range scan of likes_post_id_idx when the ids span < 256, else one seek per post
      (if (local.get $n)
        (then
          (local.set $lo (i64.load (i32.const 0x2e0200)))
          (local.set $hi (local.get $lo))
          (local.set $i (i32.const 1))
          (block $d (loop $l
            (br_if $d (i32.ge_u (local.get $i) (local.get $n)))
            (local.set $id (i64.load (i32.add (i32.const 0x2e0200) (i32.shl (local.get $i) (i32.const 3)))))
            (if (i64.lt_s (local.get $id) (local.get $lo)) (then (local.set $lo (local.get $id))))
            (if (i64.gt_s (local.get $id) (local.get $hi)) (then (local.set $hi (local.get $id))))
            (local.set $i (i32.add (local.get $i) (i32.const 1)))
            (br $l)))
          (if (i64.lt_u (i64.sub (local.get $hi) (local.get $lo)) (i64.const 256))
            (then
              (memory.fill (i32.const 0x2e0400) (i32.const 0) (i32.shl (i32.add (i32.wrap_i64 (i64.sub (local.get $hi) (local.get $lo))) (i32.const 1)) (i32.const 3)))
              (br_if $out (call $likes_count (local.get $lo) (local.get $hi) (i32.const 0x2e0400)))
              (local.set $i (i32.const 0))
              (block $d (loop $l
                (br_if $d (i32.ge_u (local.get $i) (local.get $n)))
                (i64.store (i32.add (i32.const 0x2e0300) (i32.shl (local.get $i) (i32.const 3)))
                  (i64.load (i32.add (i32.const 0x2e0400) (i32.shl (i32.wrap_i64 (i64.sub
                    (i64.load (i32.add (i32.const 0x2e0200) (i32.shl (local.get $i) (i32.const 3)))) (local.get $lo))) (i32.const 3)))))
                (local.set $i (i32.add (local.get $i) (i32.const 1)))
                (br $l))))
            (else
              (local.set $i (i32.const 0))
              (block $d (loop $l
                (br_if $d (i32.ge_u (local.get $i) (local.get $n)))
                (local.set $id (i64.load (i32.add (i32.const 0x2e0200) (i32.shl (local.get $i) (i32.const 3)))))
                (i64.store (i32.add (i32.const 0x2e0300) (i32.shl (local.get $i) (i32.const 3))) (i64.const 0))
                (br_if $out (call $likes_count (local.get $id) (local.get $id) (i32.add (i32.const 0x2e0300) (i32.shl (local.get $i) (i32.const 3)))))
                (local.set $i (i32.add (local.get $i) (i32.const 1)))
                (br $l)))))))
      ;; the posts and their authors, newest first
      (call $puts (i32.const 0x1004c0))
      (local.set $i (i32.const 0))
      (local.set $ok (i32.const 1))   ;; "no comma before the first post"
      (block $d (loop $l
        (br_if $d (i32.ge_u (local.get $i) (local.get $n)))
        (local.set $id (i64.load (i32.add (i32.const 0x2e0200) (i32.shl (local.get $i) (i32.const 3)))))
        (block $found
          ;; consecutive ids: step back one row instead of seeking from the root
          (if (local.get $i)
            (then (if (i64.eq (local.get $id) (i64.sub (i64.load (i32.add (i32.const 0x2e01f8) (i32.shl (local.get $i) (i32.const 3)))) (i64.const 1)))
              (then (if (i32.eqz (call $cur_prev (global.get $cu_p)))
                (then (br_if $found (i64.eq (call $cur_rowid (global.get $cu_p)) (local.get $id)))))))))
          (br_if $out (call $cur_seek_rowid (global.get $cu_p) (local.get $id) (i32.const 0x2e0100)))
          (if (i32.load (i32.const 0x2e0100))
            (then (local.set $i (i32.add (local.get $i) (i32.const 1))) (br $l))))   ;; not in posts: the join drops it
        (if (i32.eqz (local.get $ok)) (then (call $putc (i32.const 44))))
        (local.set $ok (i32.const 0))
        (br_if $out (i32.eqz (call $bt_post_out (local.get $id)
          (i64.load (i32.add (i32.const 0x2e0300) (i32.shl (local.get $i) (i32.const 3)))))))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $l)))
      (call $putc (i32.const 93))
      (call $putc (i32.const 125))
      (call $bt_close)
      (return (i32.const 1)))
    (call $bt_close)
    (i32.const 0))

  ;; GET /posts/:id body into BODYB: 1 written, 2 no such post, 0 not covered
  (func $bt_get (param $id i64) (result i32)
    (if (i32.eqz (call $bt_begin)) (then (return (i32.const 0))))
    (block $out
      (br_if $out (i32.eqz (call $bt_open)))
      (br_if $out (call $cur_seek_rowid (global.get $cu_p) (local.get $id) (i32.const 0x2e0100)))
      (if (i32.load (i32.const 0x2e0100)) (then (call $bt_close) (return (i32.const 2))))
      (i64.store (i32.const 0x2e0300) (i64.const 0))
      (br_if $out (call $likes_count (local.get $id) (local.get $id) (i32.const 0x2e0300)))
      ;; the like count was read first: $bt_post_out reuses COLS
      (call $puts (i32.const 0x100640))
      (br_if $out (i32.eqz (call $bt_post_out (local.get $id) (i64.load (i32.const 0x2e0300)))))
      (call $putc (i32.const 125))
      (call $bt_close)
      (return (i32.const 1)))
    (call $bt_close)
    (i32.const 0))

  ;; root pages, key descriptions, cursors and the schema cookie; leaves $bt = 0 if anything is missing
  (func $bt_init
    (local $st i32) (local $nm i32) (local $nn i32) (local $root i32) (local $b i32)
    (if (i32.eqz (call $db_utf8 (global.get $db))) (then (call $log (i32.const 0x1012e0)) (return)))
    (local.set $st (call $prep (i32.const 0x101200)))
    (block $d (loop $l
      (br_if $d (i32.ne (call $step (local.get $st)) (i32.const 100)))
      (local.set $nm (call $col_text (local.get $st) (i32.const 0)))
      (local.set $nn (call $col_bytes (local.get $st) (i32.const 0)))
      (local.set $root (i32.wrap_i64 (call $col_i64 (local.get $st) (i32.const 1))))
      (if (i32.eq (local.get $nn) (i32.const 5))
        (then
          (if (call $memeq (local.get $nm) (i32.const 0x101280) (i32.const 5)) (then (global.set $r_posts (local.get $root))))
          (if (call $memeq (local.get $nm) (i32.const 0x101290) (i32.const 5)) (then (global.set $r_users (local.get $root))))))
      (if (i32.eq (local.get $nn) (i32.const 23))
        (then (if (call $memeq (local.get $nm) (i32.const 0x1012a0) (i32.const 23)) (then (global.set $r_top (local.get $root))))))
      (if (i32.eq (local.get $nn) (i32.const 17))
        (then (if (call $memeq (local.get $nm) (i32.const 0x1012c0) (i32.const 17)) (then (global.set $r_likes (local.get $root))))))
      (br $l)))
    (drop (call $reset (local.get $st)))
    (if (i32.eqz (i32.and (i32.and (i32.ne (global.get $r_posts) (i32.const 0)) (i32.ne (global.get $r_users) (i32.const 0)))
                          (i32.and (i32.ne (global.get $r_top) (i32.const 0)) (i32.ne (global.get $r_likes) (i32.const 0)))))
      (then (call $log (i32.const 0x1012e0)) (return)))
    (global.set $k_top (call $key_info (global.get $db) (i32.const 2) (i32.const 1)))
    (global.set $k_likes (call $key_info (global.get $db) (i32.const 1) (i32.const 1)))
    (global.set $csz (i32.and (i32.add (call $cur_size) (i32.const 15)) (i32.const -16)))
    (global.set $cu_t (call $malloc (i32.shl (global.get $csz) (i32.const 2))))
    (if (i32.eqz (i32.and (i32.and (i32.ne (global.get $k_top) (i32.const 0)) (i32.ne (global.get $k_likes) (i32.const 0)))
                          (i32.ne (global.get $cu_t) (i32.const 0))))
      (then (call $log (i32.const 0x1012e0)) (return)))
    (global.set $cu_l (i32.add (global.get $cu_t) (global.get $csz)))
    (global.set $cu_p (i32.add (global.get $cu_l) (global.get $csz)))
    (global.set $cu_u (i32.add (global.get $cu_p) (global.get $csz)))
    (local.set $b (call $bt_of (global.get $db)))
    (call $tx_begin)
    (if (call $bt_read (local.get $b) (i32.const 0x2e0108)) (then (call $flush) (call $log (i32.const 0x1012e0)) (return)))
    (global.set $cookie (i32.load (i32.const 0x2e0108)))
    (call $flush)
    (global.set $bt (local.get $b)))

  (func $feed
    (local $rc i32) (local $first i32)
    (call $begin)
    (if (call $bt_feed) (then (call $finish (i32.const 0x100000)) (return)))
    (call $begin)
    (call $puts (i32.const 0x1004c0))
    (local.set $first (i32.const 1))
    (block $d (loop $l
      (local.set $rc (call $step (global.get $st_feed)))
      (br_if $d (i32.ne (local.get $rc) (i32.const 100)))
      (if (i32.eqz (local.get $first)) (then (call $putc (i32.const 44))))
      (local.set $first (i32.const 0))
      (call $post_json (global.get $st_feed))
      (br $l)))
    (drop (call $reset (global.get $st_feed)))
    (if (i32.ne (local.get $rc) (i32.const 101))
      (then (call $err (i32.const 0x100140) (i32.const 0x100480)) (return)))
    (call $putc (i32.const 93))
    (call $putc (i32.const 125))
    (call $finish (i32.const 0x100000)))

  (func $health
    (local $e i32)
    (call $begin)
    (if (i32.eq (call $step (global.get $st_ping)) (i32.const 100))
      (then
        (drop (call $reset (global.get $st_ping)))
        (call $puts (i32.const 0x100700))
        (call $putu (i64.sub (call $now) (global.get $t0)))
        (call $putc (i32.const 125))
        (call $finish (i32.const 0x100000)))
      (else
        (local.set $e (call $errmsg (global.get $db)))
        (call $puts (i32.const 0x100740))
        (call $putjson (local.get $e) (call $strlen (local.get $e)))
        (call $putc (i32.const 125))
        (drop (call $reset (global.get $st_ping)))
        (call $finish (i32.const 0x100180)))))

  (func $get_post (param $id i64)
    (local $r i32)
    (if (i64.lt_s (local.get $id) (i64.const 0)) (then (call $err (i32.const 0x100080) (i32.const 0x100280)) (return)))
    (call $begin)
    (local.set $r (call $bt_get (local.get $id)))
    (if (i32.eq (local.get $r) (i32.const 1)) (then (call $finish (i32.const 0x100000)) (return)))
    (if (i32.eq (local.get $r) (i32.const 2)) (then (call $err (i32.const 0x100100) (i32.const 0x1002c0)) (return)))
    (drop (call $bind_i64 (global.get $st_one) (i32.const 1) (local.get $id)))
    (if (i32.eq (call $step (global.get $st_one)) (i32.const 100))
      (then
        (call $begin)
        (call $puts (i32.const 0x100640))
        (call $post_json (global.get $st_one))
        (call $putc (i32.const 125))
        (drop (call $reset (global.get $st_one)))
        (call $finish (i32.const 0x100000)))
      (else
        (drop (call $reset (global.get $st_one)))
        (call $err (i32.const 0x100100) (i32.const 0x1002c0)))))

  (func $like (param $p i32) (param $n i32) (param $ah i32) (param $an i32)
    (local $id i64) (local $rc i32) (local $fresh i32)
    (if (i32.eqz (call $auth (local.get $ah) (local.get $an))) (then (return)))
    (local.set $id (call $parse_id (local.get $p) (local.get $n)))
    (if (i64.lt_s (local.get $id) (i64.const 0)) (then (call $err (i32.const 0x100080) (i32.const 0x100280)) (return)))
    (drop (call $bind_i64 (global.get $st_exists) (i32.const 1) (local.get $id)))
    (local.set $rc (call $step (global.get $st_exists)))
    (drop (call $reset (global.get $st_exists)))
    (if (i32.ne (local.get $rc) (i32.const 100)) (then (call $err (i32.const 0x100100) (i32.const 0x1002c0)) (return)))
    (global.set $wr (i32.const 1))
    (drop (call $bind_i64 (global.get $st_like) (i32.const 1) (global.get $uid)))
    (drop (call $bind_i64 (global.get $st_like) (i32.const 2) (local.get $id)))
    (local.set $rc (call $step (global.get $st_like)))
    (drop (call $reset (global.get $st_like)))
    (if (i32.ne (local.get $rc) (i32.const 101)) (then (call $err (i32.const 0x100140) (i32.const 0x100480)) (return)))
    (local.set $fresh (i32.gt_s (call $changes (global.get $db)) (i32.const 0)))
    (call $begin)
    (call $puts (i32.const 0x100680))
    (call $puts (select (i32.const 0x1007e0) (i32.const 0x1007c0) (local.get $fresh)))
    (call $puts (i32.const 0x1006c0))
    (call $putu (local.get $id))
    (call $putc (i32.const 125))
    (call $finish (select (i32.const 0x100040) (i32.const 0x100000) (local.get $fresh))))

  (func $isws (param $c i32) (result i32)
    (i32.or (i32.eq (local.get $c) (i32.const 32)) (i32.lt_u (i32.sub (local.get $c) (i32.const 9)) (i32.const 5))))

  (func $create (param $ah i32) (param $an i32) (param $body i32) (param $bn i32)
    (local $f i32) (local $len i32) (local $s i32) (local $e i32) (local $q i32) (local $chars i32) (local $rc i32)
    (if (i32.eqz (call $auth (local.get $ah) (local.get $an))) (then (return)))
    (local.set $f (call $jfind (local.get $body) (local.get $bn) (i32.const 0x100f40) (i32.const 4)))
    (if (i32.lt_s (local.get $f) (i32.const 0)) (then (call $err (i32.const 0x100080) (i32.const 0x1003c0)) (return)))
    (if (i32.eqz (local.get $f)) (then (call $err (i32.const 0x100080) (i32.const 0x100400)) (return)))
    (if (i32.ne (i32.load8_u (global.get $jv)) (i32.const 34)) (then (call $err (i32.const 0x100080) (i32.const 0x100400)) (return)))
    (global.set $jp (global.get $jv))
    (global.set $je (i32.add (local.get $body) (local.get $bn)))
    (local.set $len (call $jstr (i32.const 0x285000)))
    (local.set $s (i32.const 0x285000))
    (local.set $e (i32.add (i32.const 0x285000) (local.get $len)))
    (block $d (loop $l
      (br_if $d (i32.ge_u (local.get $s) (local.get $e)))
      (br_if $d (i32.eqz (call $isws (i32.load8_u (local.get $s)))))
      (local.set $s (i32.add (local.get $s) (i32.const 1)))
      (br $l)))
    (block $d2 (loop $l2
      (br_if $d2 (i32.le_u (local.get $e) (local.get $s)))
      (br_if $d2 (i32.eqz (call $isws (i32.load8_u (i32.sub (local.get $e) (i32.const 1))))))
      (local.set $e (i32.sub (local.get $e) (i32.const 1)))
      (br $l2)))
    (if (i32.eq (local.get $s) (local.get $e)) (then (call $err (i32.const 0x100080) (i32.const 0x100400)) (return)))
    ;; count code points: every byte that isn't a UTF-8 continuation byte
    (local.set $q (local.get $s))
    (loop $l3
      (local.set $chars (i32.add (local.get $chars)
        (i32.ne (i32.and (i32.load8_u (local.get $q)) (i32.const 0xc0)) (i32.const 0x80))))
      (local.set $q (i32.add (local.get $q) (i32.const 1)))
      (br_if $l3 (i32.lt_u (local.get $q) (local.get $e))))
    (if (i32.gt_u (local.get $chars) (i32.const 500)) (then (call $err (i32.const 0x100080) (i32.const 0x100440)) (return)))
    (global.set $wr (i32.const 1))
    (drop (call $bind_i64 (global.get $st_ins) (i32.const 1) (global.get $uid)))
    (drop (call $bind_text (global.get $st_ins) (i32.const 2) (local.get $s) (i32.sub (local.get $e) (local.get $s))))
    (if (i32.ne (call $step (global.get $st_ins)) (i32.const 100))
      (then (drop (call $reset (global.get $st_ins))) (call $err (i32.const 0x100140) (i32.const 0x100480)) (return)))
    (call $begin)
    (call $puts (i32.const 0x100640))
    (call $puts (i32.const 0x100500))
    (call $putu (call $col_i64 (global.get $st_ins) (i32.const 0)))
    (call $puts (i32.const 0x100540))
    (call $putjson (local.get $s) (i32.sub (local.get $e) (local.get $s)))
    (call $puts (i32.const 0x100580))
    (call $put (call $col_text (global.get $st_ins) (i32.const 1)) (call $col_bytes (global.get $st_ins) (i32.const 1)))
    (call $puts (i32.const 0x1005c0))
    (call $putc (i32.const 34))
    (call $put (global.get $un_p) (global.get $un_n))
    (call $putc (i32.const 34))
    (call $puts (i32.const 0x100780))
    ;; the second step finishes the statement; the batch's COMMIT runs before any response is sent
    (local.set $rc (call $step (global.get $st_ins)))
    (drop (call $reset (global.get $st_ins)))
    (if (i32.ne (local.get $rc) (i32.const 101)) (then (call $err (i32.const 0x100140) (i32.const 0x100480)) (return)))
    (call $finish (i32.const 0x100040)))

  (func $handle (param $m i32) (param $mn i32) (param $path i32) (param $pn i32)
                (param $ah i32) (param $an i32) (param $body i32) (param $bn i32)
    (local $get i32) (local $post i32) (local $q i32) (local $rest i32) (local $re i32) (local $sl i32)
    (local.set $get (i32.and (i32.eq (local.get $mn) (i32.const 3))
      (i32.eq (i32.and (i32.load (local.get $m)) (i32.const 0xffffff)) (i32.const 0x544547))))     ;; "GET"
    (local.set $post (i32.and (i32.eq (local.get $mn) (i32.const 4))
      (i32.eq (i32.load (local.get $m)) (i32.const 0x54534f50))))                                   ;; "POST"
    (local.set $q (call $memchr (local.get $path) (i32.const 63) (i32.add (local.get $path) (local.get $pn))))
    (if (local.get $q) (then (local.set $pn (i32.sub (local.get $q) (local.get $path)))))
    (if (i32.and (local.get $get) (i32.eq (local.get $pn) (i32.const 5)))
      (then (if (call $memeq (local.get $path) (i32.const 0x100fb0) (i32.const 5)) (then (call $feed) (return)))))
    (if (i32.and (local.get $get) (i32.eq (local.get $pn) (i32.const 7)))
      (then (if (call $memeq (local.get $path) (i32.const 0x100fc0) (i32.const 7)) (then (call $health) (return)))))
    (if (i32.and (i32.gt_u (local.get $pn) (i32.const 7)) (i32.or (local.get $get) (local.get $post)))
      (then (if (call $memeq (local.get $path) (i32.const 0x100fd0) (i32.const 7))
        (then
          (local.set $rest (i32.add (local.get $path) (i32.const 7)))
          (local.set $re (i32.add (local.get $path) (local.get $pn)))
          (local.set $sl (call $memchr (local.get $rest) (i32.const 47) (local.get $re)))
          (if (i32.and (local.get $get) (i32.eqz (local.get $sl)))
            (then (call $get_post (call $parse_id (local.get $rest) (i32.sub (local.get $re) (local.get $rest)))) (return)))
          (if (i32.and (local.get $post) (i32.ne (local.get $sl) (i32.const 0)))
            (then (if (i32.eq (i32.sub (local.get $re) (local.get $sl)) (i32.const 5))
              (then (if (call $memeq (local.get $sl) (i32.const 0x100fe0) (i32.const 5))
                (then (call $like (local.get $rest) (i32.sub (local.get $sl) (local.get $rest)) (local.get $ah) (local.get $an)) (return)))))))
          (call $err (i32.const 0x100100) (i32.const 0x100240))
          (return)))))
    (if (i32.and (local.get $post) (i32.eq (local.get $pn) (i32.const 6)))
      (then (if (call $memeq (local.get $path) (i32.const 0x100fd0) (i32.const 6))
        (then (call $create (local.get $ah) (local.get $an) (local.get $body) (local.get $bn)) (return)))))
    (call $err (i32.const 0x100100) (i32.const 0x100240)))

  ;; ---------- connections ----------
  (func $conn (param $fd i32) (result i32) (i32.add (i32.const 0x184000) (i32.shl (local.get $fd) (i32.const 4))))

  (func $close_conn (param $fd i32)
    (local $c i32)
    (local.set $c (call $conn (local.get $fd)))
    (if (i32.load (local.get $c)) (then (call $free (i32.load (local.get $c)))))
    (if (i32.load offset=8 (local.get $c)) (then (call $free (i32.load offset=8 (local.get $c)))))
    (v128.store (local.get $c) (v128.const i64x2 0 0))
    (i32.store offset=0x2a0000 (i32.shl (local.get $fd) (i32.const 2)) (i32.const 0))
    (i32.store offset=0x144000 (i32.shl (local.get $fd) (i32.const 2))
      (i32.add (i32.load offset=0x144000 (i32.shl (local.get $fd) (i32.const 2))) (i32.const 1)))
    (global.set $nconn (i32.sub (global.get $nconn) (i32.const 1)))
    (drop (call $close (local.get $fd))))

  ;; Close keep-alive connections idle for 1.5 s or more, holding no partial request or unsent output.
  ;; Runs only near the fd limit (LimitNOFILE=65535 caps epoll at ~65k sockets). bench/load.js never
  ;; waits less than 3 s between a user's requests, so a closed idle socket is reopened by the client,
  ;; never hit mid-request.
  (func $sweep
    (local $fd i32) (local $t i32) (local $c i32) (local $r i32)
    (global.set $lastsweep (global.get $tick))
    (block $d (loop $l
      (br_if $d (i32.gt_s (local.get $fd) (global.get $maxfd)))
      (local.set $t (i32.load offset=0x2a0000 (i32.shl (local.get $fd) (i32.const 2))))
      (if (local.get $t)
        (then (if (i32.ge_s (i32.sub (global.get $tick) (local.get $t)) (i32.const 1500))
          (then
            (local.set $c (call $conn (local.get $fd)))
            (if (i32.eqz (i32.or (i32.load offset=4 (local.get $c)) (i32.load offset=12 (local.get $c))))
              (then
                ;; a request may already sit unread in the socket (closing it then would send a RST): read once,
                ;; close only if nothing came, else serve it
                (local.set $r (call $recv (local.get $fd) (i32.const 0x104000) (i32.const 0x20000)))
                (if (i32.eq (local.get $r) (i32.const -11))
                  (then (call $close_conn (local.get $fd)))
                  (else
                    (i32.store offset=0x2a0000 (i32.shl (local.get $fd) (i32.const 2)) (global.get $tick))
                    (call $after_recv (local.get $fd) (local.get $c) (i32.const 0) (local.get $r))
                    (call $maybe_flush)))))))))
      (local.set $fd (i32.add (local.get $fd) (i32.const 1)))
      (br $l))))

;; ---------- group commit ----------
  ;; Each epoll batch runs in one transaction. Responses wait in OUTB, and are sent only after COMMIT,
  ;; so no 201 goes out for a row that is not committed.
  (func $tx_begin
    (drop (call $step (global.get $st_begin))) (drop (call $reset (global.get $st_begin)))
    (global.set $txopen (i32.const 1))
    (global.set $txt0 (global.get $tick)))

  ;; note the connection's output since the last call; $cl: close it once that is sent
  (func $rec (param $fd i32) (param $cl i32)
    (local $r i32)
    (if (i32.and (i32.eq (global.get $o) (global.get $ostart)) (i32.eqz (local.get $cl))) (then (return)))
    (local.set $r (i32.add (i32.const 0x300000) (i32.shl (global.get $nrec) (i32.const 4))))
    (i32.store (local.get $r) (local.get $fd))
    (i32.store offset=4 (local.get $r) (global.get $ostart))
    (i32.store offset=8 (local.get $r) (i32.sub (global.get $o) (global.get $ostart)))
    (i32.store offset=12 (local.get $r) (i32.or (local.get $cl)
      (i32.shl (i32.load offset=0x144000 (i32.shl (local.get $fd) (i32.const 2))) (i32.const 1))))
    (global.set $ostart (global.get $o))
    (global.set $nrec (i32.add (global.get $nrec) (i32.const 1))))

  ;; COMMIT, then send every noted response (or, if COMMIT failed, close those connections unanswered).
  ;; One host call sends a chunk of up to 32768 responses; their results land in RBUF (free between connections).
  (func $flush
    (local $ok i32) (local $i i32) (local $r i32) (local $j i32) (local $m i32)
    (local.set $ok (i32.eq (call $step (global.get $st_commit)) (i32.const 101)))
    (drop (call $reset (global.get $st_commit)))
    (if (i32.eqz (local.get $ok))
      (then (call $log (call $errmsg (global.get $db))) (call $log (i32.const 0x1011c0))
            (drop (call $db_exec (global.get $db) (i32.const 0x100cc0)))))
    ;; a record whose connection is gone (closed, or its fd reused since) gets fd -1: nothing to send
    (block $d (loop $l
      (br_if $d (i32.ge_u (local.get $i) (global.get $nrec)))
      (local.set $r (i32.add (i32.const 0x300000) (i32.shl (local.get $i) (i32.const 4))))
      (if (i32.ne (i32.shr_u (i32.load offset=12 (local.get $r)) (i32.const 1))
                  (i32.load offset=0x144000 (i32.shl (i32.load (local.get $r)) (i32.const 2))))
        (then (i32.store (local.get $r) (i32.const -1)))
        (else (if (i32.eqz (local.get $ok))
          (then (call $close_conn (i32.load (local.get $r))) (i32.store (local.get $r) (i32.const -1))))))
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (br $l)))
    (if (local.get $ok)
      (then
        (local.set $j (i32.const 0))
        (block $d2 (loop $chunk
          (br_if $d2 (i32.ge_u (local.get $j) (global.get $nrec)))
          (local.set $m (i32.sub (global.get $nrec) (local.get $j)))
          (if (i32.gt_u (local.get $m) (i32.const 32768)) (then (local.set $m (i32.const 32768))))
          (drop (call $sendmany (i32.add (i32.const 0x300000) (i32.shl (local.get $j) (i32.const 4)))
                                (local.get $m) (i32.const 0x104000)))
          (local.set $i (i32.const 0))
          (block $d3 (loop $l3
            (br_if $d3 (i32.ge_u (local.get $i) (local.get $m)))
            (local.set $r (i32.add (i32.const 0x300000) (i32.shl (i32.add (local.get $j) (local.get $i)) (i32.const 4))))
            ;; still the same connection: an earlier record's send error may have closed it
            (if (i32.and (i32.ge_s (i32.load (local.get $r)) (i32.const 0))
                         (i32.eq (i32.shr_u (i32.load offset=12 (local.get $r)) (i32.const 1))
                                 (i32.load offset=0x144000 (i32.shl (i32.load (local.get $r)) (i32.const 2)))))
              (then (call $sent (i32.load (local.get $r)) (i32.load offset=4 (local.get $r)) (i32.load offset=8 (local.get $r))
                                (i32.and (i32.load offset=12 (local.get $r)) (i32.const 1))
                                (i32.load (i32.add (i32.const 0x104000) (i32.shl (local.get $i) (i32.const 2)))))))
            (local.set $i (i32.add (local.get $i) (i32.const 1)))
            (br $l3)))
          (local.set $j (i32.add (local.get $j) (local.get $m)))
          (br $chunk)))))
    (global.set $nrec (i32.const 0))
    (global.set $txopen (i32.const 0))
    (global.set $wr (i32.const 0))
    (global.set $o (i32.const 0x400000))
    (global.set $ostart (i32.const 0x400000)))

  ;; between connections: commit and send early if OUTB or RECS is filling up
  (func $maybe_flush
    (if (i32.or (i32.gt_u (global.get $o) (i32.const 0xc00000)) (i32.ge_u (global.get $nrec) (i32.const 60000)))
      (then (call $flush) (call $tx_begin))))

  ;; [$p, $p+$len) was offered to the socket, which took $r bytes (or -errno); keep the rest (EPOLLOUT
  ;; finishes it)
  (func $sent (param $fd i32) (param $p i32) (param $len i32) (param $cl i32) (param $r i32)
    (local $c i32) (local $q i32)
    (local.set $c (call $conn (local.get $fd)))
    (if (local.get $len)
      (then
        (if (i32.lt_s (local.get $r) (i32.const 0))
          (then
            (if (i32.ne (local.get $r) (i32.const -11)) (then (call $close_conn (local.get $fd)) (return)))
            (local.set $r (i32.const 0))))
        (if (i32.lt_u (local.get $r) (local.get $len))
          (then
            (local.set $q (call $malloc (i32.sub (local.get $len) (local.get $r))))
            (memory.copy (local.get $q) (i32.add (local.get $p) (local.get $r)) (i32.sub (local.get $len) (local.get $r)))
            (i32.store offset=8 (local.get $c) (local.get $q))
            (i32.store offset=12 (local.get $c) (i32.sub (local.get $len) (local.get $r)))
            (if (local.get $cl) (then (i32.store offset=4 (local.get $c) (i32.const -1))))   ;; close once the output is gone
            (drop (call $epoll_ctl (global.get $ep) (i32.const 3) (local.get $fd) (i32.const 4)))   ;; MOD, EPOLLOUT
            (return)))))
    (if (local.get $cl) (then (call $close_conn (local.get $fd)))))

  ;; handle every complete request in [$buf, $buf+$n); returns the bytes consumed
  (func $process (param $fd i32) (param $buf i32) (param $n i32) (result i32)
    (local $pos i32) (local $s i32) (local $rem i32) (local $e i32) (local $lim i32)
    (local $sp1 i32) (local $sp2 i32) (local $h i32) (local $nl i32) (local $hl i32) (local $colon i32)
    (local $kl i32) (local $v i32) (local $vl i32) (local $ah i32) (local $an i32) (local $clen i32) (local $hdr i32)
    (block $out (loop $req
      (local.set $s (i32.add (local.get $buf) (local.get $pos)))
      (local.set $rem (i32.sub (local.get $n) (local.get $pos)))
      (br_if $out (i32.lt_s (local.get $rem) (i32.const 4)))
      ;; end of headers
      (local.set $e (local.get $s))
      (local.set $lim (i32.add (local.get $s) (i32.sub (local.get $rem) (i32.const 3))))
      (block $found (loop $f
        (if (i32.ge_u (local.get $e) (local.get $lim))
          (then
            (if (i32.gt_u (local.get $rem) (i32.const 16384)) (then (global.set $closeme (i32.const 1))))
            (br $out)))
        (br_if $found (i32.eq (i32.load (local.get $e)) (i32.const 0x0a0d0a0d)))
        (local.set $e (i32.add (local.get $e) (i32.const 1)))
        (br $f)))
      (local.set $sp1 (call $memchr (local.get $s) (i32.const 32) (local.get $e)))
      (if (i32.eqz (local.get $sp1)) (then (global.set $closeme (i32.const 1)) (br $out)))
      (local.set $sp2 (call $memchr (i32.add (local.get $sp1) (i32.const 1)) (i32.const 32) (local.get $e)))
      (if (i32.eqz (local.get $sp2)) (then (global.set $closeme (i32.const 1)) (br $out)))
      (local.set $ah (i32.const 0)) (local.set $an (i32.const 0)) (local.set $clen (i32.const 0))
      (local.set $h (i32.add (call $memchr (local.get $s) (i32.const 10) (i32.add (local.get $e) (i32.const 2))) (i32.const 1)))
      (block $hd (loop $hl_
        (br_if $hd (i32.ge_u (local.get $h) (i32.add (local.get $e) (i32.const 2))))
        (local.set $nl (call $memchr (local.get $h) (i32.const 10) (i32.add (local.get $e) (i32.const 2))))
        (br_if $hd (i32.eqz (local.get $nl)))
        (local.set $hl (i32.sub (local.get $nl) (local.get $h)))
        (if (local.get $hl) (then (if (i32.eq (i32.load8_u (i32.sub (local.get $nl) (i32.const 1))) (i32.const 13))
          (then (local.set $hl (i32.sub (local.get $hl) (i32.const 1)))))))
        (local.set $colon (call $memchr (local.get $h) (i32.const 58) (i32.add (local.get $h) (local.get $hl))))
        (if (local.get $colon)
          (then
            (local.set $kl (i32.sub (local.get $colon) (local.get $h)))
            (local.set $v (i32.add (local.get $colon) (i32.const 1)))
            (block $sd (loop $sl
              (br_if $sd (i32.ge_u (local.get $v) (i32.add (local.get $h) (local.get $hl))))
              (br_if $sd (i32.ne (i32.load8_u (local.get $v)) (i32.const 32)))
              (local.set $v (i32.add (local.get $v) (i32.const 1)))
              (br $sl)))
            (local.set $vl (i32.sub (i32.add (local.get $h) (local.get $hl)) (local.get $v)))
            (if (i32.eq (local.get $kl) (i32.const 14))
              (then (if (call $ieq (local.get $h) (i32.const 0x100f70) (i32.const 14))
                (then (local.set $clen (i32.wrap_i64 (call $parse_len (local.get $v) (local.get $vl))))))))
            (if (i32.eq (local.get $kl) (i32.const 13))
              (then (if (call $ieq (local.get $h) (i32.const 0x100f80) (i32.const 13))
                (then (local.set $ah (local.get $v)) (local.set $an (local.get $vl))))))
            (if (i32.and (i32.eq (local.get $kl) (i32.const 10)) (i32.eq (local.get $vl) (i32.const 5)))
              (then (if (call $ieq (local.get $h) (i32.const 0x100f90) (i32.const 10))
                (then (if (call $ieq (local.get $v) (i32.const 0x100fa0) (i32.const 5))
                  (then (global.set $closeme (i32.const 1))))))))))
        (local.set $h (i32.add (local.get $nl) (i32.const 1)))
        (br $hl_)))
      (if (i32.gt_u (local.get $clen) (i32.const 65536)) (then (global.set $closeme (i32.const 1)) (br $out)))
      (local.set $hdr (i32.sub (i32.add (local.get $e) (i32.const 4)) (local.get $s)))
      (if (i32.lt_u (local.get $rem) (i32.add (local.get $hdr) (local.get $clen)))
        (then (global.set $closeme (i32.const 0)) (br $out)))
      (call $handle (local.get $s) (i32.sub (local.get $sp1) (local.get $s))
                    (i32.add (local.get $sp1) (i32.const 1)) (i32.sub (i32.sub (local.get $sp2) (local.get $sp1)) (i32.const 1))
                    (local.get $ah) (local.get $an) (i32.add (local.get $s) (local.get $hdr)) (local.get $clen))
      (local.set $pos (i32.add (local.get $pos) (i32.add (local.get $hdr) (local.get $clen))))
      (br_if $out (global.get $closeme))
      ;; a flood of pipelined requests: stop before OUTB overflows, answer what was handled, close
      (if (i32.gt_u (global.get $o) (i32.const 0xf00000)) (then (global.set $closeme (i32.const 1)) (br $out)))
      (br $req)))
    (local.get $pos))

  ;; Content-Length digits (anything else stops the number)
  (func $parse_len (param $p i32) (param $n i32) (result i64)
    (local $v i64) (local $c i32) (local $e i32)
    (local.set $e (i32.add (local.get $p) (local.get $n)))
    (block $d (loop $l
      (br_if $d (i32.ge_u (local.get $p) (local.get $e)))
      (local.set $c (i32.sub (i32.load8_u (local.get $p)) (i32.const 48)))
      (br_if $d (i32.gt_u (local.get $c) (i32.const 9)))
      (br_if $d (i64.gt_u (local.get $v) (i64.const 1000000)))
      (local.set $v (i64.add (i64.mul (local.get $v) (i64.const 10)) (i64.extend_i32_u (local.get $c))))
      (local.set $p (i32.add (local.get $p) (i32.const 1)))
      (br $l)))
    (local.get $v))

  (func $on_event (param $fd i32) (param $ev i32)
    (local $c i32) (local $pin i32) (local $r i32) (local $p i32) (local $ol i32)
    (local.set $c (call $conn (local.get $fd)))
    (i32.store offset=0x2a0000 (i32.shl (local.get $fd) (i32.const 2)) (global.get $tick))
    (local.set $ol (i32.load offset=12 (local.get $c)))
    (if (local.get $ol)
      (then
        ;; waiting to write
        (if (i32.and (local.get $ev) (i32.const 24)) (then (call $close_conn (local.get $fd)) (return)))  ;; ERR|HUP
        (local.set $r (call $send (local.get $fd) (i32.load offset=8 (local.get $c)) (local.get $ol)))
        (if (i32.lt_s (local.get $r) (i32.const 0))
          (then (if (i32.ne (local.get $r) (i32.const -11)) (then (call $close_conn (local.get $fd)))) (return)))
        (if (i32.lt_u (local.get $r) (local.get $ol))
          (then
            (local.set $p (call $malloc (i32.sub (local.get $ol) (local.get $r))))
            (memory.copy (local.get $p) (i32.add (i32.load offset=8 (local.get $c)) (local.get $r)) (i32.sub (local.get $ol) (local.get $r)))
            (call $free (i32.load offset=8 (local.get $c)))
            (i32.store offset=8 (local.get $c) (local.get $p))
            (i32.store offset=12 (local.get $c) (i32.sub (local.get $ol) (local.get $r)))
            (return)))
        (call $free (i32.load offset=8 (local.get $c)))
        (i32.store offset=8 (local.get $c) (i32.const 0))
        (i32.store offset=12 (local.get $c) (i32.const 0))
        (if (i32.eq (i32.load offset=4 (local.get $c)) (i32.const -1)) (then (call $close_conn (local.get $fd)) (return)))
        (drop (call $epoll_ctl (global.get $ep) (i32.const 3) (local.get $fd) (i32.const 0x2001)))   ;; MOD, IN|RDHUP
        (return)))
    ;; readable
    (local.set $pin (i32.load offset=4 (local.get $c)))
    (if (local.get $pin)
      (then
        (memory.copy (i32.const 0x104000) (i32.load (local.get $c)) (local.get $pin))
        (call $free (i32.load (local.get $c)))
        (i32.store (local.get $c) (i32.const 0))
        (i32.store offset=4 (local.get $c) (i32.const 0))))
    (local.set $r (call $recv (local.get $fd) (i32.add (i32.const 0x104000) (local.get $pin)) (i32.sub (i32.const 0x20000) (local.get $pin))))
    (call $after_recv (local.get $fd) (local.get $c) (local.get $pin) (local.get $r)))

  ;; RBUF holds $pin earlier bytes plus what recv returned ($r: count, 0 = EOF, or -errno)
  (func $after_recv (param $fd i32) (param $c i32) (param $pin i32) (param $r i32)
    (local $n i32) (local $used i32) (local $left i32) (local $p i32)
    (if (i32.le_s (local.get $r) (i32.const 0))
      (then
        (if (i32.ne (local.get $r) (i32.const -11)) (then (call $close_conn (local.get $fd)) (return)))
        (local.set $r (i32.const 0))))
    (local.set $n (i32.add (local.get $pin) (local.get $r)))
    (global.set $closeme (i32.const 0))
    (local.set $used (call $process (local.get $fd) (i32.const 0x104000) (local.get $n)))
    (call $rec (local.get $fd) (global.get $closeme))
    (if (global.get $closeme) (then (return)))   ;; closed after the batch's COMMIT and send
    (local.set $left (i32.sub (local.get $n) (local.get $used)))
    (if (local.get $left)
      (then
        (local.set $p (call $malloc (local.get $left)))
        (memory.copy (local.get $p) (i32.add (i32.const 0x104000) (local.get $used)) (local.get $left))
        (i32.store (local.get $c) (local.get $p))
        (i32.store offset=4 (local.get $c) (local.get $left)))))

  (func $accept_all
    (local $fd i32) (local $swept i32)
    (loop $l
      (local.set $fd (call $accept (global.get $lfd)))
      (if (i32.lt_s (local.get $fd) (i32.const 0))
        (then
          ;; EMFILE/ENFILE: free idle sockets once; if none can go, stop polling the listener until the next sweep
          (if (i32.or (i32.eq (local.get $fd) (i32.const -24)) (i32.eq (local.get $fd) (i32.const -23)))
            (then
              (if (i32.eqz (local.get $swept))
                (then (local.set $swept (i32.const 1)) (call $sweep) (br $l)))
              (global.set $lis_off (i32.const 1))
              (drop (call $epoll_ctl (global.get $ep) (i32.const 3) (global.get $lfd) (i32.const 0)))))
          (return)))
      (if (i32.ge_u (local.get $fd) (i32.const 65536))
        (then (drop (call $close (local.get $fd))) (br $l)))
      (v128.store (call $conn (local.get $fd)) (v128.const i64x2 0 0))
      (i32.store offset=0x2a0000 (i32.shl (local.get $fd) (i32.const 2)) (global.get $tick))
      (global.set $nconn (i32.add (global.get $nconn) (i32.const 1)))
      (if (i32.gt_s (local.get $fd) (global.get $maxfd)) (then (global.set $maxfd (local.get $fd))))
      (drop (call $epoll_ctl (global.get $ep) (i32.const 1) (local.get $fd) (i32.const 0x2001)))   ;; ADD, IN|RDHUP
      (br $l)))

  ;; dotted quad -> s_addr (bytes in network order)
  (func $parse_ip (param $p i32) (result i32)
    (local $v i32) (local $part i32) (local $sh i32) (local $c i32)
    (loop $l
      (local.set $c (i32.load8_u (local.get $p)))
      (if (i32.lt_u (i32.sub (local.get $c) (i32.const 48)) (i32.const 10))
        (then (local.set $part (i32.add (i32.mul (local.get $part) (i32.const 10)) (i32.sub (local.get $c) (i32.const 48)))))
        (else
          (local.set $v (i32.or (local.get $v) (i32.shl (i32.and (local.get $part) (i32.const 255)) (local.get $sh))))
          (local.set $sh (i32.add (local.get $sh) (i32.const 8)))
          (local.set $part (i32.const 0))))
      (local.set $p (i32.add (local.get $p) (i32.const 1)))
      (br_if $l (local.get $c)))
    (local.get $v))

  (func $prep (param $sql i32) (result i32)
    (if (call $db_prepare (global.get $db) (local.get $sql) (i32.const 0x295004))
      (then (call $fatal (i32.const 0x101100))))
    (i32.load (i32.const 0x295004)))

  (func (export "_start")
    (local $key i32) (local $path i32) (local $host i32) (local $port i32) (local $n i32) (local $i i32) (local $fd i32) (local $w i32)
    (call $sq_init)
    (call $b64init)
    (global.set $t0 (call $now))
    (local.set $key (call $getenv (i32.const 0x100e20)))
    (local.set $path (call $getenv (i32.const 0x100e00)))
    (local.set $host (call $getenv (i32.const 0x100e40)))
    (local.set $port (call $getenv (i32.const 0x100e60)))
    (if (i32.eqz (i32.and (i32.and (i32.ne (local.get $key) (i32.const 0)) (i32.ne (local.get $path) (i32.const 0)))
                          (i32.and (i32.ne (local.get $host) (i32.const 0)) (i32.ne (local.get $port) (i32.const 0)))))
      (then (call $fatal (i32.const 0x101180))))
    (call $hmac_init (local.get $key) (call $strlen (local.get $key)))
    (if (call $db_open (local.get $path) (i32.const 0x295000)) (then (call $fatal (i32.const 0x1010c0))))
    (global.set $db (i32.load (i32.const 0x295000)))
    (if (call $db_exec (global.get $db) (i32.const 0x100d00)) (then (call $fatal (i32.const 0x1010c0))))
    (global.set $st_one (call $prep (i32.const 0x100800)))
    (global.set $st_feed (call $prep (i32.const 0x100900)))
    (global.set $st_ins (call $prep (i32.const 0x100a00)))
    (global.set $st_like (call $prep (i32.const 0x100b00)))
    (global.set $st_exists (call $prep (i32.const 0x100c00)))
    (global.set $st_ping (call $prep (i32.const 0x100c80)))
    (global.set $st_begin (call $prep (i32.const 0x100ca0)))
    (global.set $st_commit (call $prep (i32.const 0x100cb0)))
    (call $bt_init)
    (global.set $lfd (call $listen (call $parse_ip (local.get $host))
      (i32.wrap_i64 (call $parse_len (local.get $port) (call $strlen (local.get $port)))) (i32.const 65535)))
    (if (i32.lt_s (global.get $lfd) (i32.const 0)) (then (call $fatal (i32.const 0x101140))))
    (global.set $ep (call $epoll_create))
    (drop (call $epoll_ctl (global.get $ep) (i32.const 1) (global.get $lfd) (i32.const 1)))
    (call $log (i32.const 0x101080))
    (loop $forever
      ;; a transaction that has written waits up to WINDOW ms for more writes: fewer commits, fewer WAL
      ;; frames (a hot page is written once per commit, not once per request) and fewer checkpoints
      (local.set $n (call $epoll_wait (global.get $ep) (i32.const 0x102000) (i32.const 512)
        (if (result i32) (global.get $txopen)
          (then (select (local.tee $w (i32.sub (i32.const 2) (i32.sub (call $clock_ms) (global.get $txt0))))
                        (i32.const 0) (i32.gt_s (local.get $w) (i32.const 0))))
          (else (i32.const 500)))))
      (global.set $tick (call $clock_ms))
      (if (i32.eqz (global.get $tick)) (then (global.set $tick (i32.const 1))))   ;; 0 marks a closed fd
      (if (i32.eqz (global.get $txopen)) (then (call $tx_begin)))
      (if (i32.and (i32.or (i32.gt_s (global.get $nconn) (i32.const 60000)) (global.get $lis_off))
                   (i32.ge_s (i32.sub (global.get $tick) (global.get $lastsweep)) (i32.const 500)))
        (then
          (call $sweep)
          (if (global.get $lis_off)
            (then
              (global.set $lis_off (i32.const 0))
              (drop (call $epoll_ctl (global.get $ep) (i32.const 3) (global.get $lfd) (i32.const 1)))))))
      (local.set $i (i32.const 0))
      (block $d (loop $l
        (br_if $d (i32.ge_s (local.get $i) (local.get $n)))
        (local.set $fd (i32.load (i32.add (i32.const 0x102000) (i32.shl (local.get $i) (i32.const 3)))))
        (if (i32.eq (local.get $fd) (global.get $lfd))
          (then (call $accept_all))
          (else
            (call $on_event (local.get $fd) (i32.load offset=4 (i32.add (i32.const 0x102000) (i32.shl (local.get $i) (i32.const 3)))))
            (call $maybe_flush)))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $l)))
      (if (i32.or (i32.eqz (global.get $wr)) (i32.ge_s (i32.sub (call $clock_ms) (global.get $txt0)) (i32.const 2)))
        (then (call $flush)))
      (br $forever)))
)

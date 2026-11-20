;; The $12 server, hand-written in WebAssembly text, running inside the stock sqlite3 shell.
;;
;; start.sql loads build/serve.so (src/serve.s) into the shell and calls serve('build/app.so'), which runs
;; this module's "run" export with three imports: host.sys (a Linux system call), host.call (a native
;; function by address) and host.membase (where this module's memory is). Everything else is here: the
;; io_uring event loop (its rings live in this memory), HTTP/1.1, routing, JSON reading, base64url,
;; SHA-256/HMAC for the HS256 JWT check, and the SQL. SQL goes to the shell's own connection through SQLite's
;; public API: the functions come from the sqlite3_api_routines table SQLite gives every extension, and
;; SQLite builds the JSON of each post itself (json_object), so the module copies one string per request.
;;
;; Memory map (one fixed size; addresses below 16 MiB keep the code's constants short):
;;   0x001000  API   copy of sqlite3_api_routines (2,232 bytes; offsets from SQLite 3.54.1's sqlite3ext.h)
;;   0x002000  MISC  syscall arguments: iovecs, out-params, sockaddr, timespec, rlimit, io_uring_params and
;;                   register structs; RT (per-ring state) at 0x2200; OUT segment counters at 0x2400
;;   0x005000  ENV   /proc/self/environ (64 KiB)
;;   0x020000  SQES/RINGS  8 rings: 64 submission entries (one page) and head/tail words and 128 completions
;;                   (one page) each
;;   0x030000  BUFR  8 provided-buffer rings of 1,024 entries (16 KiB each)
;;   0x100000  string constants and SQL, NUL-terminated
;;   0x284000  JWT scratch: header, payload, SHA-256 state/schedule/pad, HMAC states, tables
;;   0x285000  TXT   decoded POST body, 64 KiB; 0x295000 digit buffer; 0x296000 decoded username
;;   0x2a0000  BODYB response body being built (small bodies; post JSON is copied straight to OUT)
;;   0x300000  free lists: PEND blocks (4,096 x 4 bytes), SPILL blocks at 0x304000
;;   0x400000  CONN  32 bytes per connection (8 rings x 65,536 slots)
;;   0x1400000 RECV  receive buffers: 8 rings x 1,024 x 2 KiB
;;   0x2400000 OUT   32 MiB, 32 segments of 1 MiB: responses, sent straight from here
;;   0x4400000 SPILL 128 blocks of 256 KiB for output the socket did not take at once
;;   0x6400000 PEND  4,096 blocks of 16 KiB for partial requests and bytes that came in during a send
;;   0xa400000 READY connections to serve on the next turn (2 MiB)
(module
  (import "host" "sys" (func $sys (param i64 i64 i64 i64 i64 i64 i64) (result i64)))
  (import "host" "call" (func $call (param i64 i64 i64 i64 i64 i64 i64) (result i64)))
  (import "host" "membase" (func $membase (param i64 i64 i64 i64 i64 i64 i64) (result i64)))
  (memory 2656)   ;; 0xa600000 bytes; pages are only backed once touched

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
  (data (i32.const 0x100680) "{\"liked\":true,\"already_liked\":")
  (data (i32.const 0x1006c0) ",\"post_id\":")
  (data (i32.const 0x100700) "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":")
  (data (i32.const 0x100740) "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"")
  (data (i32.const 0x1007c0) "true")
  (data (i32.const 0x1007e0) "false")
  ;; SQL. SQLite writes each post object; the order of its keys is the order of json_object's arguments.
  ;; The feed: the subquery runs as a co-routine that scans posts_created_at_id_idx in feed order, and
  ;; group_concat joins its rows in the order the co-routine yields them (EXPLAIN QUERY PLAN shows no other
  ;; step; an ORDER BY inside group_concat would sort the 20 rows again in a temporary b-tree).
  (data (i32.const 0x100800) "SELECT json_object('post',json_object('id',p.id,'body',p.body,'created_at',p.created_at,'author',u.username,'like_count',(SELECT count(*) FROM likes l WHERE l.post_id=p.id))) FROM posts p JOIN users u ON u.id=p.user_id WHERE p.id=?1\00")
  (data (i32.const 0x100a00) "SELECT '{\"posts\":['||coalesce(group_concat(j,','),'')||']}' FROM (SELECT json_object('id',p.id,'body',p.body,'created_at',p.created_at,'author',u.username,'like_count',(SELECT count(*) FROM likes l WHERE l.post_id=p.id)) j FROM posts p JOIN users u ON u.id=p.user_id ORDER BY p.created_at DESC,p.id DESC LIMIT 20)\00")
  (data (i32.const 0x100c00) "INSERT INTO posts(user_id,body) VALUES(?1,?2) RETURNING json_object('post',json_object('id',id,'body',body,'created_at',created_at,'author',?3,'like_count',0))\00")
  (data (i32.const 0x100d00) "SELECT 1 FROM posts WHERE id=?1\00")
  (data (i32.const 0x100d40) "INSERT INTO likes(user_id,post_id) VALUES(?1,?2) ON CONFLICT(user_id,post_id) DO NOTHING\00")
  (data (i32.const 0x100dc0) "SELECT 1\00")
  (data (i32.const 0x100de0) "%s\00")
  (data (i32.const 0x100e00) "/proc/self/environ\00")
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
  (data (i32.const 0x101080) "wat server listening (inside sqlite3)\0a")
  (data (i32.const 0x1010c0) "fatal: cannot read sqlite3_api_routines\0a")
  (data (i32.const 0x101100) "fatal: cannot prepare a statement\0a")
  (data (i32.const 0x101140) "fatal: cannot listen\0a")
  (data (i32.const 0x101180) "fatal: JWT_SECRET, HOST and PORT must be set\0a")
  (data (i32.const 0x1011c0) "fatal: io_uring setup failed, errno \00")
  (data (i32.const 0x101200) "\00")
  ;; SHA-256 round constants and initial state, as little-endian i32s
  (data (i32.const 0x284b00) "\98\2f\8a\42\91\44\37\71\cf\fb\c0\b5\a5\db\b5\e9\5b\c2\56\39\f1\11\f1\59\a4\82\3f\92\d5\5e\1c\ab\98\aa\07\d8\01\5b\83\12\be\85\31\24\c3\7d\0c\55\74\5d\be\72\fe\b1\de\80\a7\06\dc\9b\74\f1\9b\c1\c1\69\9b\e4\86\47\be\ef\c6\9d\c1\0f\cc\a1\0c\24\6f\2c\e9\2d\aa\84\74\4a\dc\a9\b0\5c\da\88\f9\76\52\51\3e\98\6d\c6\31\a8\c8\27\03\b0\c7\7f\59\bf\f3\0b\e0\c6\47\91\a7\d5\51\63\ca\06\67\29\29\14\85\0a\b7\27\38\21\1b\2e\fc\6d\2c\4d\13\0d\38\53\54\73\0a\65\bb\0a\6a\76\2e\c9\c2\81\85\2c\72\92\a1\e8\bf\a2\4b\66\1a\a8\70\8b\4b\c2\a3\51\6c\c7\19\e8\92\d1\24\06\99\d6\85\35\0e\f4\70\a0\6a\10\16\c1\a4\19\08\6c\37\1e\4c\77\48\27\b5\bc\b0\34\b3\0c\1c\39\4a\aa\d8\4e\4f\ca\9c\5b\f3\6f\2e\68\ee\82\8f\74\6f\63\a5\78\14\78\c8\84\08\02\c7\8c\fa\ff\be\90\eb\6c\50\a4\f7\a3\f9\be\f2\78\71\c6")
  (data (i32.const 0x284c00) "\67\e6\09\6a\85\ae\67\bb\72\f3\6e\3c\3a\f5\4f\a5\7f\52\0e\51\8c\68\05\9b\ab\d9\83\1f\19\cd\e0\5b")

  (global $mb (mut i64) (i64.const 0))      ;; native address of memory offset 0
  (global $db (mut i64) (i64.const 0))      ;; the shell's connection
  (global $st_one (mut i64) (i64.const 0))
  (global $st_feed (mut i64) (i64.const 0))
  (global $st_ins (mut i64) (i64.const 0))
  (global $st_exists (mut i64) (i64.const 0))
  (global $st_like (mut i64) (i64.const 0))
  (global $st_ping (mut i64) (i64.const 0))
  (global $now (mut i64) (i64.const 0))     ;; wall clock seconds, read once per loop turn
  (global $t0 (mut i64) (i64.const 0))
  (global $w (mut i32) (i32.const 0))       ;; write cursor (BODYB while building a body, OUT otherwise)
  (global $o (mut i32) (i32.const 0x2400000)) ;; end of the output in OUT
  (global $jp (mut i32) (i32.const 0))      ;; JSON reader cursor
  (global $je (mut i32) (i32.const 0))      ;; JSON reader end
  (global $jv (mut i32) (i32.const 0))      ;; value found by $jfind
  (global $uid (mut i64) (i64.const 0))     ;; from the token
  (global $un_p (mut i32) (i32.const 0))    ;; username, raw JSON string bytes from the token
  (global $un_n (mut i32) (i32.const 0))
  (global $closeme (mut i32) (i32.const 0))
  ;; ---------- the three imports ----------
  ;; native address of offset $o in this memory
  (func $p (param $o i32) (result i64) (i64.add (global.get $mb) (i64.extend_i32_u (local.get $o))))
  (func $sys3 (param $nr i32) (param $a i64) (param $b i64) (param $c i64) (result i64)
    (call $sys (i64.extend_i32_u (local.get $nr)) (local.get $a) (local.get $b) (local.get $c) (i64.const 0) (i64.const 0) (i64.const 0)))
  ;; SQLite's function at byte offset $f of sqlite3_api_routines, called with up to five arguments
  (func $c5 (param $f i32) (param $a i64) (param $b i64) (param $c i64) (param $d i64) (param $e i64) (result i64)
    (call $call (i64.load offset=0x1000 (local.get $f)) (local.get $a) (local.get $b) (local.get $c) (local.get $d) (local.get $e) (i64.const 0)))
  (func $c1 (param $f i32) (param $a i64) (result i64)
    (call $c5 (local.get $f) (local.get $a) (i64.const 0) (i64.const 0) (i64.const 0) (i64.const 0)))
  (func $c2 (param $f i32) (param $a i64) (param $b i64) (result i64)
    (call $c5 (local.get $f) (local.get $a) (local.get $b) (i64.const 0) (i64.const 0) (i64.const 0)))

  ;; ---------- SQLite (offsets into sqlite3_api_routines) ----------
  (func $step (param $s i64) (result i32) (i32.wrap_i64 (call $c1 (i32.const 752) (local.get $s))))
  (func $reset (param $s i64) (drop (call $c1 (i32.const 616) (local.get $s))))
  (func $bind_i64 (param $s i64) (param $i i32) (param $v i64)
    (drop (call $c5 (i32.const 40) (local.get $s) (i64.extend_i32_u (local.get $i)) (local.get $v) (i64.const 0) (i64.const 0))))
  ;; text at [$off, $off + $n) of this memory, SQLITE_STATIC: it stays put until the statement is reset
  (func $bind_text (param $s i64) (param $i i32) (param $off i32) (param $n i32)
    (drop (call $c5 (i32.const 80) (local.get $s) (i64.extend_i32_u (local.get $i)) (call $p (local.get $off))
                    (i64.extend_i32_u (local.get $n)) (i64.const 0))))
  (func $changes (result i32) (i32.wrap_i64 (call $c1 (i32.const 120) (global.get $db))))
  (func $prep (param $sql i32) (result i64)
    (if (i32.wrap_i64 (call $c5 (i32.const 928) (global.get $db) (call $p (local.get $sql)) (i64.const -1) (call $p (i32.const 0x2020)) (i64.const 0)))
      (then (call $fatal (i32.const 0x101100))))
    (i64.load (i32.const 0x2020)))
  ;; a native NUL-terminated string -> $w (at most $n bytes), with sqlite3_snprintf(n + 1, w, "%s", s)
  (func $copy (param $s i64) (param $n i32)
    (drop (call $c5 (i32.const 744) (i64.extend_i32_u (i32.add (local.get $n) (i32.const 1))) (call $p (global.get $w))
                    (call $p (i32.const 0x100de0)) (local.get $s) (i64.const 0)))
    (global.set $w (i32.add (global.get $w) (local.get $n))))

  ;; ---------- Linux ----------
  (func $write2 (param $s i32) (param $n i32)
    (drop (call $sys3 (i32.const 1) (i64.const 2) (call $p (local.get $s)) (i64.extend_i32_u (local.get $n)))))
  (func $log (param $s i32) (call $write2 (local.get $s) (call $strlen (local.get $s))))
  (func $fatal (param $s i32) (call $log (local.get $s)) (drop (call $sys3 (i32.const 231) (i64.const 1) (i64.const 0) (i64.const 0))))
  ;; $s, then the negative errno $e, and exit
  (func $fatal_e (param $s i32) (param $e i64)
    (call $log (local.get $s))
    (global.set $w (i32.const 0x2a0000))
    (call $putu (i64.sub (i64.const 0) (local.get $e)))
    (call $putc (i32.const 10))
    (call $write2 (i32.const 0x2a0000) (i32.sub (global.get $w) (i32.const 0x2a0000)))
    (call $fatal (i32.const 0x101200)))
  (func $close (param $fd i32) (drop (call $sys3 (i32.const 3) (i64.extend_i32_u (local.get $fd)) (i64.const 0) (i64.const 0))))
  (func $clock (param $id i32) (result i64)
    (drop (call $sys3 (i32.const 228) (i64.extend_i32_u (local.get $id)) (call $p (i32.const 0x2060)) (i64.const 0)))
    (i64.load (i32.const 0x2060)))
  ;; the value of environment variable $name ($n bytes), NUL-terminated in ENV, or 0
  (func $getenv (param $name i32) (param $n i32) (result i32)
    (local $e i32)
    (local.set $e (i32.const 0x5000))
    (block $d (loop $l
      (br_if $d (i32.eqz (i32.load8_u (local.get $e))))
      (if (i32.and (call $memeq (local.get $e) (local.get $name) (local.get $n))
                   (i32.eq (i32.load8_u (i32.add (local.get $e) (local.get $n))) (i32.const 61)))
        (then (return (i32.add (local.get $e) (i32.add (local.get $n) (i32.const 1))))))
      (local.set $e (i32.add (local.get $e) (i32.add (call $strlen (local.get $e)) (i32.const 1))))
      (br $l)))
    (i32.const 0))

  ;; ---------- reused helpers: memory, writing, JSON reader, base64url, SHA-256/HMAC, auth, HTTP ----------
  (func $strlen (param $p i32) (result i32)
    (local $q i32)
    (local.set $q (local.get $p))
    (block $d (loop $l
      (br_if $d (i32.eqz (i32.load8_u (local.get $q))))
      (local.set $q (i32.add (local.get $q) (i32.const 1)))
      (br $l)))
    (i32.sub (local.get $q) (local.get $p)))

  ;; constant-time compare (for the token signature): ORs the differences of all $n bytes
  (func $cteq (param $a i32) (param $b i32) (param $n i32) (result i32)
    (local $d i32) (local $i i32)
    (block $d_ (loop $l
      (br_if $d_ (i32.ge_u (local.get $i) (local.get $n)))
      (local.set $d (i32.or (local.get $d) (i32.xor (i32.load8_u (i32.add (local.get $a) (local.get $i))) (i32.load8_u (i32.add (local.get $b) (local.get $i))))))
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (br $l)))
    (i32.eqz (local.get $d)))
  (func $memeq (param $a i32) (param $b i32) (param $n i32) (result i32)
    (block $no (loop $l
      (if (i32.eqz (local.get $n)) (then (return (i32.const 1))))
      (br_if $no (i32.ne (i32.load8_u (local.get $a)) (i32.load8_u (local.get $b))))
      (local.set $a (i32.add (local.get $a) (i32.const 1)))
      (local.set $b (i32.add (local.get $b) (i32.const 1)))
      (local.set $n (i32.sub (local.get $n) (i32.const 1)))
      (br $l)))
    (i32.const 0))

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

  (func $memchr (param $p i32) (param $c i32) (param $e i32) (result i32)
    (block $d (loop $l
      (br_if $d (i32.ge_u (local.get $p) (local.get $e)))
      (if (i32.eq (i32.load8_u (local.get $p)) (local.get $c)) (then (return (local.get $p))))
      (local.set $p (i32.add (local.get $p) (i32.const 1)))
      (br $l)))
    (i32.const 0))

  (func $put (param $p i32) (param $n i32)
    (local $d i32)
    (local.set $d (global.get $w))
    (global.set $w (i32.add (local.get $d) (local.get $n)))
    (if (i32.gt_u (local.get $n) (i32.const 256))
      (then (memory.copy (local.get $d) (local.get $p) (local.get $n)) (return)))
    (block $b (loop $l
      (br_if $b (i32.lt_u (local.get $n) (i32.const 16)))
      (v128.store (local.get $d) (v128.load (local.get $p)))
      (local.set $d (i32.add (local.get $d) (i32.const 16)))
      (local.set $p (i32.add (local.get $p) (i32.const 16)))
      (local.set $n (i32.sub (local.get $n) (i32.const 16)))
      (br $l)))
    (if (i32.ge_u (local.get $n) (i32.const 8))
      (then
        (i64.store (local.get $d) (i64.load (local.get $p)))
        (local.set $d (i32.add (local.get $d) (i32.const 8)))
        (local.set $p (i32.add (local.get $p) (i32.const 8)))
        (local.set $n (i32.sub (local.get $n) (i32.const 8)))))
    (if (i32.ge_u (local.get $n) (i32.const 4))
      (then
        (i32.store (local.get $d) (i32.load (local.get $p)))
        (local.set $d (i32.add (local.get $d) (i32.const 4)))
        (local.set $p (i32.add (local.get $p) (i32.const 4)))
        (local.set $n (i32.sub (local.get $n) (i32.const 4)))))
    (block $b (loop $l
      (br_if $b (i32.eqz (local.get $n)))
      (i32.store8 (local.get $d) (i32.load8_u (local.get $p)))
      (local.set $d (i32.add (local.get $d) (i32.const 1)))
      (local.set $p (i32.add (local.get $p) (i32.const 1)))
      (local.set $n (i32.sub (local.get $n) (i32.const 1)))
      (br $l))))

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

  (func $hex4 (param $p i32) (result i32)
    (local $v i32) (local $i i32) (local $d i32)
    (loop $l
      (local.set $d (call $hexval (i32.load8_u (i32.add (local.get $p) (local.get $i)))))
      (if (i32.lt_s (local.get $d) (i32.const 0)) (then (return (i32.const -1))))
      (local.set $v (i32.or (i32.shl (local.get $v) (i32.const 4)) (local.get $d)))
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (br_if $l (i32.lt_u (local.get $i) (i32.const 4))))
    (local.get $v))

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

  (func $keyend (param $ks i32) (result i32)
    (local $c i32)
    (loop $l
      (local.set $c (i32.load8_u (local.get $ks)))
      (if (i32.eq (local.get $c) (i32.const 34)) (then (return (local.get $ks))))
      (local.set $ks (i32.add (local.get $ks)
        (select (i32.const 2) (i32.const 1) (i32.eq (local.get $c) (i32.const 92)))))
      (br $l))
    (unreachable))

  (func $b64init
    (local $i i32)
    (memory.fill (i32.const 0x284d00) (i32.const 255) (i32.const 256))
    (loop $l
      (i32.store8 (i32.add (i32.const 0x284d00) (i32.load8_u (i32.add (i32.const 0x101000) (local.get $i)))) (local.get $i))
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (br_if $l (i32.lt_u (local.get $i) (i32.const 64)))))

  (func $b64v (param $p i32) (result i32) (i32.load8_u (i32.add (i32.const 0x284d00) (i32.load8_u (local.get $p)))))

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

  (func $hmac (param $p i32) (param $n i32)
    (memory.copy (i32.const 0x284600) (i32.const 0x284a00) (i32.const 32))
    (call $sha_final (local.get $p) (local.get $n) (i32.add (local.get $n) (i32.const 64)))
    (call $digest_out (i32.const 0x284900))
    (memory.copy (i32.const 0x284600) (i32.const 0x284a20) (i32.const 32))
    (call $sha_final (i32.const 0x284900) (i32.const 32) (i32.const 96))
    (call $digest_out (i32.const 0x284900)))

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
        (br_if $bad (i32.eqz (call $cteq (i32.const 0x284940) (i32.add (local.get $d2) (i32.const 1)) (i32.const 43))))
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
        (br_if $bad (i64.le_s (local.get $exp) (global.get $now)))
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

  (func $isws (param $c i32) (result i32)
    (i32.or (i32.eq (local.get $c) (i32.const 32)) (i32.lt_u (i32.sub (local.get $c) (i32.const 9)) (i32.const 5))))

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
      ;; many pipelined requests: stop after 64 KiB of output; the rest is served once this output is sent
      (br_if $out (i32.gt_u (i32.sub (global.get $o) (global.get $ostart)) (i32.const 0x10000)))
      (br $req)))
    (local.get $pos))

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

  ;; ---------- responses ----------
  ;; status line and headers for a body of $n bytes, at $w
  (func $head (param $status i32) (param $n i32)
    (call $puts (local.get $status))
    (call $put (i32.const 0x1001c0) (i32.const 48))
    (call $putu (i64.extend_i32_u (local.get $n)))
    (call $put (i32.const 0x100200) (i32.const 4)))
  (func $respond (param $status i32) (param $bp i32) (param $bn i32)
    (global.set $w (global.get $o))
    (call $head (local.get $status) (local.get $bn))
    (call $put (local.get $bp) (local.get $bn))
    (global.set $o (global.get $w)))
  (func $begin (global.set $w (i32.const 0x2a0000)))
  (func $finish (param $status i32) (call $respond (local.get $status) (i32.const 0x2a0000) (i32.sub (global.get $w) (i32.const 0x2a0000))))
  (func $err (param $status i32) (param $msg i32) (call $respond (local.get $status) (local.get $msg) (call $strlen (local.get $msg))))
  ;; the text in column 0 of $s's current row (JSON SQLite built) as the body
  (func $respond_col (param $status i32) (param $s i64)
    (local $t i64) (local $n i32)
    (local.set $t (call $c2 (i32.const 288) (local.get $s) (i64.const 0)))                  ;; sqlite3_column_text
    (local.set $n (i32.wrap_i64 (call $c2 (i32.const 160) (local.get $s) (i64.const 0))))   ;; sqlite3_column_bytes
    (global.set $w (global.get $o))
    (call $head (local.get $status) (local.get $n))
    (call $copy (local.get $t) (local.get $n))
    (global.set $o (global.get $w)))

  ;; ---------- routes ----------
  (func $feed
    (if (i32.eq (call $step (global.get $st_feed)) (i32.const 100))   ;; SQLITE_ROW
      (then (call $respond_col (i32.const 0x100000) (global.get $st_feed)))
      (else (call $err (i32.const 0x100140) (i32.const 0x100480))))
    (call $reset (global.get $st_feed)))

  (func $health
    (local $s i32) (local $c i32)
    (call $begin)
    (if (i32.eq (call $step (global.get $st_ping)) (i32.const 100))
      (then
        (call $puts (i32.const 0x100700))
        (call $putu (i64.sub (global.get $now) (global.get $t0)))
        (call $putc (i32.const 125))
        (call $reset (global.get $st_ping))
        (call $finish (i32.const 0x100000)))
      (else
        (call $reset (global.get $st_ping))
        (call $puts (i32.const 0x100740))
        ;; the error message, with anything that would need escaping in JSON left out
        (local.set $s (global.get $w))
        (call $copy (call $c1 (i32.const 424) (global.get $db)) (i32.const 200))   ;; sqlite3_errmsg
        (global.set $w (local.get $s))
        (block $d (loop $l
          (br_if $d (i32.eqz (local.tee $c (i32.load8_u (local.get $s)))))
          (if (i32.and (i32.ge_u (local.get $c) (i32.const 32)) (i32.and (i32.ne (local.get $c) (i32.const 34)) (i32.ne (local.get $c) (i32.const 92))))
            (then (call $putc (local.get $c))))
          (local.set $s (i32.add (local.get $s) (i32.const 1)))
          (br $l)))
        (call $putc (i32.const 34))
        (call $putc (i32.const 125))
        (call $finish (i32.const 0x100180)))))

  (func $get_post (param $id i64)
    (if (i64.lt_s (local.get $id) (i64.const 0)) (then (call $err (i32.const 0x100080) (i32.const 0x100280)) (return)))
    (call $bind_i64 (global.get $st_one) (i32.const 1) (local.get $id))
    (if (i32.eq (call $step (global.get $st_one)) (i32.const 100))
      (then (call $respond_col (i32.const 0x100000) (global.get $st_one)))
      (else (call $err (i32.const 0x100100) (i32.const 0x1002c0))))
    (call $reset (global.get $st_one)))

  (func $like (param $p i32) (param $n i32) (param $ah i32) (param $an i32)
    (local $id i64) (local $rc i32) (local $fresh i32)
    (if (i32.eqz (call $auth (local.get $ah) (local.get $an))) (then (return)))
    (local.set $id (call $parse_id (local.get $p) (local.get $n)))
    (if (i64.lt_s (local.get $id) (i64.const 0)) (then (call $err (i32.const 0x100080) (i32.const 0x100280)) (return)))
    (call $bind_i64 (global.get $st_exists) (i32.const 1) (local.get $id))
    (local.set $rc (call $step (global.get $st_exists)))
    (call $reset (global.get $st_exists))
    (if (i32.ne (local.get $rc) (i32.const 100)) (then (call $err (i32.const 0x100100) (i32.const 0x1002c0)) (return)))
    (call $bind_i64 (global.get $st_like) (i32.const 1) (global.get $uid))
    (call $bind_i64 (global.get $st_like) (i32.const 2) (local.get $id))
    (local.set $rc (call $step (global.get $st_like)))
    (call $reset (global.get $st_like))
    (if (i32.ne (local.get $rc) (i32.const 101)) (then (call $err (i32.const 0x100140) (i32.const 0x100480)) (return)))   ;; SQLITE_DONE
    (local.set $fresh (i32.gt_s (call $changes) (i32.const 0)))
    (call $begin)
    (call $puts (i32.const 0x100680))
    (call $puts (select (i32.const 0x1007e0) (i32.const 0x1007c0) (local.get $fresh)))
    (call $puts (i32.const 0x1006c0))
    (call $putu (local.get $id))
    (call $putc (i32.const 125))
    (call $finish (select (i32.const 0x100040) (i32.const 0x100000) (local.get $fresh))))

  (func $create (param $ah i32) (param $an i32) (param $body i32) (param $bn i32)
    (local $f i32) (local $len i32) (local $s i32) (local $e i32) (local $q i32) (local $chars i32) (local $un i32) (local $o0 i32)
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
    ;; the token's username, unescaped
    (global.set $jp (i32.sub (global.get $un_p) (i32.const 1)))
    (global.set $je (i32.add (i32.add (global.get $un_p) (global.get $un_n)) (i32.const 1)))
    (local.set $un (call $jstr (i32.const 0x296000)))
    (call $bind_i64 (global.get $st_ins) (i32.const 1) (global.get $uid))
    (call $bind_text (global.get $st_ins) (i32.const 2) (local.get $s) (i32.sub (local.get $e) (local.get $s)))
    (call $bind_text (global.get $st_ins) (i32.const 3) (i32.const 0x296000) (local.get $un))
    (if (i32.eq (call $step (global.get $st_ins)) (i32.const 100))
      (then
        (local.set $o0 (global.get $o))
        (call $respond_col (i32.const 0x100040) (global.get $st_ins))
        ;; the second step finishes the statement, which commits the row; the response goes out after that
        (if (i32.ne (call $step (global.get $st_ins)) (i32.const 101))
          (then (global.set $o (local.get $o0)) (call $err (i32.const 0x100140) (i32.const 0x100480)))))
      (else (call $err (i32.const 0x100140) (i32.const 0x100480))))
    (call $reset (global.get $st_ins)))

  ;; ---------- io_uring ----------
  ;; Up to 8 rings, all in this memory (IORING_SETUP_NO_MMAP, one page of 64 entries and one of 128
  ;; completions each), so the module writes every submission entry and reads every completion itself.
  ;; Connections never take a process file descriptor: a multishot accept puts each one straight into a slot
  ;; of its ring's fixed-file table (65,535 slots under LimitNOFILE=65535), and accepting moves to the
  ;; emptiest ring when one gets near full. Each connection has one multishot receive that takes 2 KiB
  ;; buffers from its ring's provided-buffer ring; the buffer goes back as soon as its requests are served.
  ;; Ring 0 is the one waited on; the others signal one eventfd when work arrives, and ring 0 reads it.
  ;;
  ;; A connection is ci = ring << 16 | slot. user_data = kind << 24 | ci; kinds: 1 accept, 2 recv,
  ;; 3 send from OUT, 4 send from SPILL, 5 close, 6 eventfd read, 8 shutdown.
  ;; CONN, 32 bytes per connection: 0 flags, 4 PEND block + 1, 8 bytes in PEND, 12 SPILL block + 1,
  ;; 16 SPILL cursor, 20 SPILL bytes left, 24 address of the send from OUT, 28 its length.
  ;; Flags: 1 receive armed, 2 send in flight, 8 closing, 16 shut down, 32 in the ready list.
  ;; RT, 32 bytes per ring at 0x2200: 0 ring fd, 4 SQ tail (ours), 8 connections, 12 buffer ring tail.
  (global $nr (mut i32) (i32.const 0))      ;; rings set up
  (global $ns (mut i32) (i32.const 0))      ;; fixed-file slots per ring
  (global $nhi (mut i32) (i32.const 0))     ;; 7/8 of them: the accept moves on from there
  (global $acc (mut i32) (i32.const 0))     ;; the ring that accepts
  (global $lfd (mut i32) (i32.const 0))     ;; listening socket
  (global $efd (mut i32) (i32.const 0))     ;; eventfd of rings 1..7
  (global $o_sqh (mut i32) (i32.const 0))   ;; offsets in a ring page: SQ head, SQ tail, SQ flags,
  (global $o_sqt (mut i32) (i32.const 0))   ;; CQ head, CQ tail, completion entries
  (global $o_sqf (mut i32) (i32.const 0))
  (global $o_cqh (mut i32) (i32.const 0))
  (global $o_cqt (mut i32) (i32.const 0))
  (global $o_cqes (mut i32) (i32.const 0))
  (global $ostart (mut i32) (i32.const 0))  ;; where the current connection's output starts in OUT
  (global $npend (mut i32) (i32.const 0))   ;; free PEND blocks
  (global $nfree (mut i32) (i32.const 0))   ;; free SPILL blocks
  (global $nready (mut i32) (i32.const 0))

  (func $rt (param $r i32) (result i32) (i32.add (i32.const 0x2200) (i32.shl (local.get $r) (i32.const 5))))
  (func $sqes (param $r i32) (result i32) (i32.add (i32.const 0x20000) (i32.shl (local.get $r) (i32.const 13))))
  (func $rp (param $r i32) (result i32) (i32.add (i32.const 0x21000) (i32.shl (local.get $r) (i32.const 13))))
  (func $conn (param $ci i32) (result i32) (i32.add (i32.const 0x400000) (i32.shl (local.get $ci) (i32.const 5))))
  (func $pendb (param $b i32) (result i32) (i32.add (i32.const 0x6400000) (i32.shl (i32.sub (local.get $b) (i32.const 1)) (i32.const 14))))
  (func $queued (param $r i32) (result i32)
    (i32.sub (i32.load offset=4 (call $rt (local.get $r))) (i32.load (i32.add (call $rp (local.get $r)) (global.get $o_sqh)))))
  (func $cq_ready (param $r i32) (result i32)
    (local $p i32) (local.set $p (call $rp (local.get $r)))
    (i32.ne (i32.load (i32.add (local.get $p) (global.get $o_cqh))) (i32.load (i32.add (local.get $p) (global.get $o_cqt)))))

  ;; io_uring_enter on ring $r: submits what is queued; with $fl = 1 (GETEVENTS) also runs the ring's
  ;; pending work and waits for $wait completions
  (func $enter (param $r i32) (param $fl i32) (param $wait i32)
    (drop (call $sys (i64.const 426) (i64.extend_i32_u (i32.load (call $rt (local.get $r))))
      (i64.extend_i32_u (call $queued (local.get $r))) (i64.extend_i32_u (local.get $wait))
      (i64.extend_i32_u (local.get $fl)) (i64.const 0) (i64.const 0))))

  ;; one submission entry on ring $r: 0 opcode, 1 flags, 2 ioprio, 4 fd, 16 addr, 24 len, 28 op flags,
  ;; 32 user_data (40 buf_group and 44 file_index stay 0 unless the caller sets them)
  (func $sqe (param $r i32) (param $op i32) (param $sf i32) (param $fd i32) (param $addr i64) (param $len i32)
             (param $fl i32) (param $prio i32) (param $ud i32) (result i32)
    (local $t i32) (local $q i32) (local $e i32)
    (local.set $t (call $rt (local.get $r)))
    (if (i32.ge_u (call $queued (local.get $r)) (i32.const 64)) (then (call $enter (local.get $r) (i32.const 0) (i32.const 0))))
    (local.set $q (i32.load offset=4 (local.get $t)))
    (local.set $e (i32.add (call $sqes (local.get $r)) (i32.shl (i32.and (local.get $q) (i32.const 63)) (i32.const 6))))
    (v128.store (local.get $e) (v128.const i64x2 0 0))
    (v128.store offset=16 (local.get $e) (v128.const i64x2 0 0))
    (v128.store offset=32 (local.get $e) (v128.const i64x2 0 0))
    (v128.store offset=48 (local.get $e) (v128.const i64x2 0 0))
    (i32.store8 (local.get $e) (local.get $op))
    (i32.store8 offset=1 (local.get $e) (local.get $sf))
    (i32.store16 offset=2 (local.get $e) (local.get $prio))
    (i32.store offset=4 (local.get $e) (local.get $fd))
    (i64.store offset=16 (local.get $e) (local.get $addr))
    (i32.store offset=24 (local.get $e) (local.get $len))
    (i32.store offset=28 (local.get $e) (local.get $fl))
    (i32.store offset=32 (local.get $e) (local.get $ud))
    (local.set $q (i32.add (local.get $q) (i32.const 1)))
    (i32.store offset=4 (local.get $t) (local.get $q))
    (i32.store (i32.add (call $rp (local.get $r)) (global.get $o_sqt)) (local.get $q))
    (local.get $e))

  ;; accept into a free fixed-file slot (file_index = IORING_FILE_INDEX_ALLOC). 16 are kept in flight, each
  ;; re-armed on completion. (A multishot accept would need a cancel to move to another ring, and on Linux 6.8
  ;; the cancelled one kept taking the listening socket's wake-ups.)
  (func $arm_acc (param $r i32)
    (i32.store offset=44 (call $sqe (local.get $r) (i32.const 13) (i32.const 0) (global.get $lfd) (i64.const 0) (i32.const 0)
      (i32.const 0) (i32.const 0) (i32.or (i32.const 0x1000000) (i32.shl (local.get $r) (i32.const 16)))) (i32.const -1)))
  ;; multishot receive with buffer selection (IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT, IORING_RECV_MULTISHOT)
  (func $arm_recv (param $ci i32)
    (local $c i32) (local.set $c (call $conn (local.get $ci)))
    (i32.store (local.get $c) (i32.or (i32.load (local.get $c)) (i32.const 1)))
    (drop (call $sqe (i32.shr_u (local.get $ci) (i32.const 16)) (i32.const 27) (i32.const 33) (i32.and (local.get $ci) (i32.const 0xffff))
      (i64.const 0) (i32.const 0) (i32.const 0) (i32.const 2) (i32.or (i32.const 0x2000000) (local.get $ci)))))
  ;; send $n bytes at $a (MSG_NOSIGNAL; from OUT also MSG_DONTWAIT, so it completes inside the next enter)
  (func $send (param $ci i32) (param $a i32) (param $n i32) (param $kind i32)
    (local $c i32) (local.set $c (call $conn (local.get $ci)))
    (i32.store (local.get $c) (i32.or (i32.load (local.get $c)) (i32.const 2)))
    (drop (call $sqe (i32.shr_u (local.get $ci) (i32.const 16)) (i32.const 26) (i32.const 1) (i32.and (local.get $ci) (i32.const 0xffff))
      (call $p (local.get $a)) (local.get $n) (select (i32.const 0x4040) (i32.const 0x4000) (i32.eq (local.get $kind) (i32.const 3)))
      (i32.const 0) (i32.or (i32.shl (local.get $kind) (i32.const 24)) (local.get $ci)))))
  ;; give receive buffer $bid back to ring $r
  (func $give (param $r i32) (param $bid i32)
    (local $t i32) (local $n i32) (local $e i32) (local $br i32)
    (local.set $t (call $rt (local.get $r)))
    (local.set $br (i32.add (i32.const 0x30000) (i32.shl (local.get $r) (i32.const 14))))
    (local.set $n (i32.load offset=12 (local.get $t)))
    (local.set $e (i32.add (local.get $br) (i32.shl (i32.and (local.get $n) (i32.const 1023)) (i32.const 4))))
    (i64.store (local.get $e) (call $p (i32.add (i32.const 0x1400000) (i32.shl (i32.or (i32.shl (local.get $r) (i32.const 10)) (local.get $bid)) (i32.const 11)))))
    (i32.store offset=8 (local.get $e) (i32.const 2048))
    (i32.store16 offset=12 (local.get $e) (local.get $bid))
    (local.set $n (i32.add (local.get $n) (i32.const 1)))
    (i32.store offset=12 (local.get $t) (local.get $n))
    (i32.store16 offset=14 (local.get $br) (local.get $n)))

  (func $ready (param $ci i32)
    (local $c i32) (local.set $c (call $conn (local.get $ci)))
    (if (i32.and (i32.load (local.get $c)) (i32.const 32)) (then (return)))
    (i32.store (local.get $c) (i32.or (i32.load (local.get $c)) (i32.const 32)))
    (i32.store (i32.add (i32.const 0xa400000) (i32.shl (global.get $nready) (i32.const 2))) (local.get $ci))
    (global.set $nready (i32.add (global.get $nready) (i32.const 1))))
  ;; append $n bytes to the connection's PEND block (16 KiB, the largest request); 0 if it does not fit
  (func $append (param $c i32) (param $s i32) (param $n i32) (result i32)
    (local $b i32) (local $l i32)
    (if (i32.eqz (local.tee $b (i32.load offset=4 (local.get $c))))
      (then
        (if (i32.eqz (global.get $npend)) (then (return (i32.const 0))))
        (global.set $npend (i32.sub (global.get $npend) (i32.const 1)))
        (local.set $b (i32.add (i32.load (i32.add (i32.const 0x300000) (i32.shl (global.get $npend) (i32.const 2)))) (i32.const 1)))
        (i32.store offset=4 (local.get $c) (local.get $b))
        (i32.store offset=8 (local.get $c) (i32.const 0))))
    (local.set $l (i32.load offset=8 (local.get $c)))
    (if (i32.gt_u (i32.add (local.get $l) (local.get $n)) (i32.const 16384)) (then (return (i32.const 0))))
    (memory.copy (i32.add (call $pendb (local.get $b)) (local.get $l)) (local.get $s) (local.get $n))
    (i32.store offset=8 (local.get $c) (i32.add (local.get $l) (local.get $n)))
    (i32.const 1))
  (func $free_pend (param $c i32)
    (local $b i32)
    (if (local.tee $b (i32.load offset=4 (local.get $c)))
      (then
        (i32.store (i32.add (i32.const 0x300000) (i32.shl (global.get $npend) (i32.const 2))) (i32.sub (local.get $b) (i32.const 1)))
        (global.set $npend (i32.add (global.get $npend) (i32.const 1)))
        (i32.store offset=4 (local.get $c) (i32.const 0))
        (i32.store offset=8 (local.get $c) (i32.const 0)))))
  (func $free_spill (param $b i32)
    (i32.store (i32.add (i32.const 0x304000) (i32.shl (global.get $nfree) (i32.const 2))) (local.get $b))
    (global.set $nfree (i32.add (global.get $nfree) (i32.const 1))))

  ;; close a connection: shut it down first if a receive or send is still in flight (their completions
  ;; come back here); once none is, free its blocks and close its slot
  (func $drop (param $ci i32)
    (local $c i32) (local $f i32) (local $r i32) (local $b i32)
    (local.set $c (call $conn (local.get $ci)))
    (local.set $f (i32.or (i32.load (local.get $c)) (i32.const 8)))
    (local.set $r (i32.shr_u (local.get $ci) (i32.const 16)))
    (if (i32.and (local.get $f) (i32.const 3))
      (then
        (if (i32.eqz (i32.and (local.get $f) (i32.const 16)))
          (then
            (local.set $f (i32.or (local.get $f) (i32.const 16)))
            (drop (call $sqe (local.get $r) (i32.const 34) (i32.const 1) (i32.and (local.get $ci) (i32.const 0xffff))   ;; SHUT_RDWR
              (i64.const 0) (i32.const 2) (i32.const 0) (i32.const 0) (i32.or (i32.const 0x8000000) (local.get $ci))))))
        (i32.store (local.get $c) (local.get $f))
        (return)))
    (call $free_pend (local.get $c))
    (if (local.tee $b (i32.load offset=12 (local.get $c))) (then (call $free_spill (i32.sub (local.get $b) (i32.const 1)))))
    (v128.store (local.get $c) (v128.const i64x2 0 0))
    (v128.store offset=16 (local.get $c) (v128.const i64x2 0 0))
    (i32.store offset=44 (call $sqe (local.get $r) (i32.const 19) (i32.const 0) (i32.const 0) (i64.const 0) (i32.const 0)
      (i32.const 0) (i32.const 0) (i32.or (i32.const 0x5000000) (local.get $ci))) (i32.add (i32.and (local.get $ci) (i32.const 0xffff)) (i32.const 1)))
    (local.set $b (call $rt (local.get $r)))
    (i32.store offset=8 (local.get $b) (i32.sub (i32.load offset=8 (local.get $b)) (i32.const 1))))

  ;; room in OUT for one connection's output (64 KiB plus one response): OUT is 32 segments of 1 MiB used
  ;; in turn, and a segment is reused only when no send from it is still in flight (counters at 0x2400)
  (func $out_room (result i32)
    (local $off i32) (local $s i32)
    (local.set $off (i32.sub (global.get $o) (i32.const 0x2400000)))
    (if (i32.le_u (i32.and (local.get $off) (i32.const 0xfffff)) (i32.const 0xcffff)) (then (return (i32.const 1))))
    (local.set $s (i32.and (i32.add (i32.shr_u (local.get $off) (i32.const 20)) (i32.const 1)) (i32.const 31)))
    (if (i32.load (i32.add (i32.const 0x2400) (i32.shl (local.get $s) (i32.const 2)))) (then (return (i32.const 0))))
    (global.set $o (i32.add (i32.const 0x2400000) (i32.shl (local.get $s) (i32.const 20))))
    (i32.const 1))
  (func $segc (param $a i32) (param $d i32)
    (local $p i32)
    (local.set $p (i32.add (i32.const 0x2400) (i32.shl (i32.shr_u (i32.sub (local.get $a) (i32.const 0x2400000)) (i32.const 20)) (i32.const 2))))
    (i32.store (local.get $p) (i32.add (i32.load (local.get $p)) (local.get $d))))

  ;; serve the complete requests in $n bytes at $buf (a receive buffer; $n = 0: what is in PEND). While a
  ;; send is in flight new bytes wait in PEND. The responses go out with one send straight from OUT; a
  ;; partial request stays in PEND.
  (func $serve (param $ci i32) (param $buf i32) (param $n i32)
    (local $c i32) (local $f i32) (local $used i32) (local $left i32) (local $len i32) (local $inpend i32)
    (local.set $c (call $conn (local.get $ci)))
    (local.set $f (i32.load (local.get $c)))
    (if (i32.and (local.get $f) (i32.const 8)) (then (return)))
    (if (i32.and (i32.ne (local.get $n) (i32.const 0)) (i32.or (i32.ne (i32.load offset=8 (local.get $c)) (i32.const 0)) (i32.ne (i32.and (local.get $f) (i32.const 2)) (i32.const 0))))
      (then
        (if (i32.eqz (call $append (local.get $c) (local.get $buf) (local.get $n))) (then (call $drop (local.get $ci)) (return)))
        (local.set $n (i32.const 0))))
    (if (i32.and (local.get $f) (i32.const 2)) (then (return)))
    (if (i32.eqz (local.get $n))
      (then
        (local.set $n (i32.load offset=8 (local.get $c)))
        (if (i32.eqz (local.get $n)) (then (return)))
        (local.set $buf (call $pendb (i32.load offset=4 (local.get $c))))
        (local.set $inpend (i32.const 1))))
    (if (i32.eqz (call $out_room))
      (then
        (if (i32.eqz (local.get $inpend))
          (then (if (i32.eqz (call $append (local.get $c) (local.get $buf) (local.get $n))) (then (call $drop (local.get $ci)) (return)))))
        (call $ready (local.get $ci))
        (return)))
    (global.set $ostart (global.get $o))
    (global.set $closeme (i32.const 0))
    (local.set $used (call $process (i32.const 0) (local.get $buf) (local.get $n)))
    (local.set $left (i32.sub (local.get $n) (local.get $used)))
    (if (local.get $inpend)
      (then
        (if (local.get $left)
          (then
            (if (local.get $used) (then (memory.copy (local.get $buf) (i32.add (local.get $buf) (local.get $used)) (local.get $left))))
            (i32.store offset=8 (local.get $c) (local.get $left)))
          (else (call $free_pend (local.get $c)))))
      (else
        (if (local.get $left)
          (then (if (i32.eqz (call $append (local.get $c) (i32.add (local.get $buf) (local.get $used)) (local.get $left)))
            (then (call $drop (local.get $ci)) (return)))))))
    (if (local.tee $len (i32.sub (global.get $o) (global.get $ostart)))
      (then
        (i32.store offset=24 (local.get $c) (global.get $ostart))
        (i32.store offset=28 (local.get $c) (local.get $len))
        (call $segc (global.get $ostart) (i32.const 1))
        (call $send (local.get $ci) (global.get $ostart) (local.get $len) (i32.const 3))
        (if (global.get $closeme) (then (i32.store (local.get $c) (i32.or (i32.load (local.get $c)) (i32.const 8)))))
        ;; stopped at the output limit with requests left: serve them next turn
        (if (i32.and (i32.gt_u (local.get $len) (i32.const 0x10000)) (i32.ne (local.get $left) (i32.const 0)))
          (then (call $ready (local.get $ci)))))
      (else (if (global.get $closeme) (then (call $drop (local.get $ci)))))))

  ;; a send finished: close if asked to, else serve what came in meanwhile
  (func $sent (param $ci i32)
    (local $c i32) (local.set $c (call $conn (local.get $ci)))
    (if (i32.and (i32.load (local.get $c)) (i32.const 8)) (then (call $drop (local.get $ci)) (return)))
    (if (i32.load offset=8 (local.get $c)) (then (call $serve (local.get $ci) (i32.const 0) (i32.const 0)))))

  ;; one completion: 0 user_data, 8 res, 12 flags (1 IORING_CQE_F_BUFFER with the buffer id in the top
  ;; 16 bits, 2 IORING_CQE_F_MORE)
  (func $on_cqe (param $e i32)
    (local $ud i32) (local $kind i32) (local $ci i32) (local $r i32) (local $res i32) (local $fl i32)
    (local $c i32) (local $t i32) (local $i i32) (local $m i32) (local $b i32) (local $n i32)
    (local.set $ud (i32.load (local.get $e)))
    (local.set $res (i32.load offset=8 (local.get $e)))
    (local.set $fl (i32.load offset=12 (local.get $e)))
    (local.set $kind (i32.shr_u (local.get $ud) (i32.const 24)))
    (local.set $ci (i32.and (local.get $ud) (i32.const 0x7ffff)))
    (local.set $r (i32.shr_u (local.get $ci) (i32.const 16)))
    (local.set $c (call $conn (local.get $ci)))
    (if (i32.eq (local.get $kind) (i32.const 2))
      (then
        (if (i32.gt_s (local.get $res) (i32.const 0))
          (then
            (local.set $b (i32.shr_u (local.get $fl) (i32.const 16)))
            (call $serve (local.get $ci)
              (i32.add (i32.const 0x1400000) (i32.shl (i32.or (i32.shl (local.get $r) (i32.const 10)) (local.get $b)) (i32.const 11)))
              (local.get $res))
            (call $give (local.get $r) (local.get $b))))
        (if (i32.eqz (i32.and (local.get $fl) (i32.const 2)))
          (then
            (i32.store (local.get $c) (i32.and (i32.load (local.get $c)) (i32.const -2)))
            ;; out of buffers (ENOBUFS) or a receive that ended with data: receive again
            (if (i32.and (i32.eqz (i32.and (i32.load (local.get $c)) (i32.const 8)))
                         (i32.or (i32.gt_s (local.get $res) (i32.const 0)) (i32.eq (local.get $res) (i32.const -105))))
              (then (call $arm_recv (local.get $ci)))
              (else (call $drop (local.get $ci))))))
        (return)))
    (if (i32.eq (local.get $kind) (i32.const 3))
      (then
        (i32.store (local.get $c) (i32.and (i32.load (local.get $c)) (i32.const -3)))
        (call $segc (i32.load offset=24 (local.get $c)) (i32.const -1))
        (local.set $n (i32.load offset=28 (local.get $c)))
        (if (i32.eq (local.get $res) (local.get $n)) (then (call $sent (local.get $ci)) (return)))
        (if (i32.and (i32.lt_s (local.get $res) (i32.const 0)) (i32.ne (local.get $res) (i32.const -11)))
          (then (call $drop (local.get $ci)) (return)))
        ;; the socket took only part of it (or nothing: EAGAIN): the rest goes to a SPILL block
        (if (i32.lt_s (local.get $res) (i32.const 0)) (then (local.set $res (i32.const 0))))
        (local.set $n (i32.sub (local.get $n) (local.get $res)))
        (if (i32.or (i32.eqz (global.get $nfree)) (i32.gt_u (local.get $n) (i32.const 0x40000)))
          (then (call $drop (local.get $ci)) (return)))
        (global.set $nfree (i32.sub (global.get $nfree) (i32.const 1)))
        (local.set $b (i32.load (i32.add (i32.const 0x304000) (i32.shl (global.get $nfree) (i32.const 2)))))
        (i32.store offset=12 (local.get $c) (i32.add (local.get $b) (i32.const 1)))
        (local.set $b (i32.add (i32.const 0x4400000) (i32.shl (local.get $b) (i32.const 18))))
        (memory.copy (local.get $b) (i32.add (i32.load offset=24 (local.get $c)) (local.get $res)) (local.get $n))
        (i32.store offset=16 (local.get $c) (local.get $b))
        (i32.store offset=20 (local.get $c) (local.get $n))
        (call $send (local.get $ci) (local.get $b) (local.get $n) (i32.const 4))
        (return)))
    (if (i32.eq (local.get $kind) (i32.const 4))
      (then
        (i32.store (local.get $c) (i32.and (i32.load (local.get $c)) (i32.const -3)))
        (if (i32.le_s (local.get $res) (i32.const 0)) (then (call $drop (local.get $ci)) (return)))
        (i32.store offset=16 (local.get $c) (i32.add (i32.load offset=16 (local.get $c)) (local.get $res)))
        (if (local.tee $n (i32.sub (i32.load offset=20 (local.get $c)) (local.get $res)))
          (then
            (i32.store offset=20 (local.get $c) (local.get $n))
            (call $send (local.get $ci) (i32.load offset=16 (local.get $c)) (local.get $n) (i32.const 4))
            (return)))
        (call $free_spill (i32.sub (i32.load offset=12 (local.get $c)) (i32.const 1)))
        (i32.store offset=12 (local.get $c) (i32.const 0))
        (call $sent (local.get $ci))
        (return)))
    (if (i32.eq (local.get $kind) (i32.const 1))
      (then
        (if (i32.ge_s (local.get $res) (i32.const 0))
          (then
            (local.set $ci (i32.or (i32.shl (local.get $r) (i32.const 16)) (local.get $res)))
            (local.set $t (call $rt (local.get $r)))
            (i32.store offset=8 (local.get $t) (i32.add (i32.load offset=8 (local.get $t)) (i32.const 1)))
            (call $arm_recv (local.get $ci))
            ;; near full (a burst can take a whole backlog at once): the accept moves to the emptiest ring
            (if (i32.ge_u (i32.load offset=8 (call $rt (global.get $acc))) (global.get $nhi))
              (then
                (local.set $m (global.get $acc))
                (loop $l
                  (if (i32.lt_u (i32.load offset=8 (call $rt (local.get $i))) (i32.load offset=8 (call $rt (local.get $m))))
                    (then (local.set $m (local.get $i))))
                  (br_if $l (i32.lt_u (local.tee $i (i32.add (local.get $i) (i32.const 1))) (global.get $nr))))
                (if (i32.and (i32.ne (local.get $m) (global.get $acc))
                             (i32.lt_u (i32.load offset=8 (call $rt (local.get $m))) (global.get $nhi)))
                  (then (global.set $acc (local.get $m))))))))
        ;; accept again, on the ring that accepts now
        (call $arm_acc (global.get $acc))
        (return)))
    (if (i32.eq (local.get $kind) (i32.const 6))
      (then (call $arm_poll))))

  ;; ring 0 reads the eventfd the other rings signal (8 bytes, which also resets it); read again on each
  ;; completion. (A multishot poll on it stopped firing after its first completion on Linux 6.8.)
  (func $arm_poll
    (drop (call $sqe (i32.const 0) (i32.const 22) (i32.const 0) (global.get $efd) (call $p (i32.const 0x21f8)) (i32.const 8) (i32.const 0)
      (i32.const 0) (i32.const 0x6000000))))

  ;; one ring: 64 entries, 128 completions, in SQES/RINGS slot $r; a sparse fixed-file table of $ns slots; a
  ;; provided-buffer ring (group 0) of 1,024 2 KiB buffers. Flags: CQSIZE, SUBMIT_ALL, COOP_TASKRUN,
  ;; TASKRUN_FLAG, SINGLE_ISSUER, DEFER_TASKRUN, NO_MMAP, NO_SQARRAY. Returns the ring fd or -errno.
  (func $ring (param $r i32) (result i64)
    (local $fd i64) (local $i i32) (local $br i32) (local $t i32)
    (memory.fill (i32.const 0x2100) (i32.const 0) (i32.const 120))
    (i32.store offset=4 (i32.const 0x2100) (i32.const 128))
    (i32.store offset=8 (i32.const 0x2100) (i32.const 95112))
    (i64.store offset=72 (i32.const 0x2100) (call $p (call $sqes (local.get $r))))   ;; sq_off.user_addr
    (i64.store offset=112 (i32.const 0x2100) (call $p (call $rp (local.get $r))))    ;; cq_off.user_addr
    (local.set $fd (call $sys3 (i32.const 425) (i64.const 64) (call $p (i32.const 0x2100)) (i64.const 0)))
    (if (i64.lt_s (local.get $fd) (i64.const 0)) (then (return (local.get $fd))))
    (global.set $o_sqh (i32.load offset=40 (i32.const 0x2100)))
    (global.set $o_sqt (i32.load offset=44 (i32.const 0x2100)))
    (global.set $o_sqf (i32.load offset=56 (i32.const 0x2100)))
    (global.set $o_cqh (i32.load offset=80 (i32.const 0x2100)))
    (global.set $o_cqt (i32.load offset=84 (i32.const 0x2100)))
    (global.set $o_cqes (i32.load offset=100 (i32.const 0x2100)))
    (local.set $t (call $rt (local.get $r)))
    (i32.store (local.get $t) (i32.wrap_i64 (local.get $fd)))
    ;; IORING_REGISTER_FILES2, IORING_RSRC_REGISTER_SPARSE
    (memory.fill (i32.const 0x2180) (i32.const 0) (i32.const 32))
    (i32.store (i32.const 0x2180) (global.get $ns))
    (i32.store offset=4 (i32.const 0x2180) (i32.const 1))
    (local.set $i (i32.wrap_i64 (call $sys (i64.const 427) (local.get $fd) (i64.const 13) (call $p (i32.const 0x2180)) (i64.const 32) (i64.const 0) (i64.const 0))))
    (if (local.get $i) (then (return (i64.extend_i32_s (local.get $i)))))
    ;; the buffers, then IORING_REGISTER_PBUF_RING
    (local.set $br (i32.add (i32.const 0x30000) (i32.shl (local.get $r) (i32.const 14))))
    (local.set $i (i32.const 0))
    (loop $l (call $give (local.get $r) (local.get $i)) (br_if $l (i32.lt_u (local.tee $i (i32.add (local.get $i) (i32.const 1))) (i32.const 1024))))
    (memory.fill (i32.const 0x21c0) (i32.const 0) (i32.const 40))
    (i64.store (i32.const 0x21c0) (call $p (local.get $br)))
    (i32.store offset=8 (i32.const 0x21c0) (i32.const 1024))
    (local.set $i (i32.wrap_i64 (call $sys (i64.const 427) (local.get $fd) (i64.const 22) (call $p (i32.const 0x21c0)) (i64.const 1) (i64.const 0) (i64.const 0))))
    (if (local.get $i) (then (return (i64.extend_i32_s (local.get $i)))))
    ;; rings 1..7: IORING_REGISTER_EVENTFD
    (if (local.get $r)
      (then
        (i32.store (i32.const 0x21f0) (global.get $efd))
        (local.set $i (i32.wrap_i64 (call $sys (i64.const 427) (local.get $fd) (i64.const 4) (call $p (i32.const 0x21f0)) (i64.const 1) (i64.const 0) (i64.const 0))))
        (if (local.get $i) (then (return (i64.extend_i32_s (local.get $i)))))))
    (local.get $fd))

  ;; ---------- start ----------
  ;; run(api, ctx), called by serve(): the shell's sqlite3_api_routines and serve()'s context
  (func (export "run") (param $api i64) (param $ctx i64)
    (local $key i32) (local $host i32) (local $port i32) (local $fd i64) (local $r i64) (local $n i32) (local $i i32) (local $h i32)
    (global.set $mb (call $membase (i64.const 0) (i64.const 0) (i64.const 0) (i64.const 0) (i64.const 0) (i64.const 0) (i64.const 0)))
    ;; copy sqlite3_api_routines into API: process_vm_readv(getpid(), local iovec, 1, remote iovec, 1, 0)
    (i64.store (i32.const 0x2000) (call $p (i32.const 0x1000)))
    (i64.store (i32.const 0x2008) (i64.const 2232))
    (i64.store (i32.const 0x2010) (local.get $api))
    (i64.store (i32.const 0x2018) (i64.const 2232))
    (if (i64.ne (call $sys (i64.const 310) (call $sys3 (i32.const 39) (i64.const 0) (i64.const 0) (i64.const 0))
                  (call $p (i32.const 0x2000)) (i64.const 1) (call $p (i32.const 0x2010)) (i64.const 1) (i64.const 0)) (i64.const 2232))
      (then (call $fatal (i32.const 0x1010c0))))
    ;; the environment, from /proc/self/environ
    (local.set $fd (call $sys3 (i32.const 2) (call $p (i32.const 0x100e00)) (i64.const 0) (i64.const 0)))
    (block $d (loop $l
      (local.set $r (call $sys3 (i32.const 0) (local.get $fd) (call $p (i32.add (i32.const 0x5000) (local.get $n)))
                      (i64.extend_i32_u (i32.sub (i32.const 0xfff0) (local.get $n)))))
      (br_if $d (i64.le_s (local.get $r) (i64.const 0)))
      (local.set $n (i32.add (local.get $n) (i32.wrap_i64 (local.get $r))))
      (br $l)))
    (call $close (i32.wrap_i64 (local.get $fd)))
    (local.set $key (call $getenv (i32.const 0x100e20) (i32.const 10)))
    (local.set $host (call $getenv (i32.const 0x100e40) (i32.const 4)))
    (local.set $port (call $getenv (i32.const 0x100e60) (i32.const 4)))
    (if (i32.eqz (i32.and (i32.ne (local.get $key) (i32.const 0)) (i32.and (i32.ne (local.get $host) (i32.const 0)) (i32.ne (local.get $port) (i32.const 0)))))
      (then (call $fatal (i32.const 0x101180))))
    (call $b64init)
    (call $hmac_init (local.get $key) (call $strlen (local.get $key)))
    (global.set $t0 (call $clock (i32.const 0)))
    (global.set $now (global.get $t0))
    ;; the shell's connection (start.sql opened it on SQLITE_PATH and set its pragmas) and the statements
    (global.set $db (call $c1 (i32.const 1192) (local.get $ctx)))   ;; sqlite3_context_db_handle
    (global.set $st_one (call $prep (i32.const 0x100800)))
    (global.set $st_feed (call $prep (i32.const 0x100a00)))
    (global.set $st_ins (call $prep (i32.const 0x100c00)))
    (global.set $st_exists (call $prep (i32.const 0x100d00)))
    (global.set $st_like (call $prep (i32.const 0x100d40)))
    (global.set $st_ping (call $prep (i32.const 0x100dc0)))
    ;; PEND and SPILL free lists
    (loop $l
      (i32.store (i32.add (i32.const 0x300000) (i32.shl (local.get $i) (i32.const 2))) (local.get $i))
      (br_if $l (i32.lt_u (local.tee $i (i32.add (local.get $i) (i32.const 1))) (i32.const 4096))))
    (global.set $npend (i32.const 4096))
    (local.set $i (i32.const 0))
    (loop $l (call $free_spill (local.get $i)) (br_if $l (i32.lt_u (local.tee $i (i32.add (local.get $i) (i32.const 1))) (i32.const 128))))
    ;; RLIMIT_NOFILE: the soft limit up to the hard one; a ring's fixed-file table may not be larger
    (drop (call $sys3 (i32.const 97) (i64.const 7) (call $p (i32.const 0x2090)) (i64.const 0)))
    (i64.store (i32.const 0x2090) (select (i64.const 65536) (i64.load (i32.const 0x2098)) (i64.gt_u (i64.load (i32.const 0x2098)) (i64.const 65536))))
    (drop (call $sys3 (i32.const 160) (i64.const 7) (call $p (i32.const 0x2090)) (i64.const 0)))
    (drop (call $sys3 (i32.const 97) (i64.const 7) (call $p (i32.const 0x2090)) (i64.const 0)))
    (global.set $ns (i32.wrap_i64 (select (i64.const 65536) (i64.load (i32.const 0x2090)) (i64.gt_u (i64.load (i32.const 0x2090)) (i64.const 65536)))))
    (global.set $nhi (i32.sub (global.get $ns) (i32.shr_u (global.get $ns) (i32.const 3))))
    ;; the listening socket: SO_REUSEADDR, TCP_NODELAY (accepted sockets inherit it)
    (global.set $lfd (i32.wrap_i64 (call $sys3 (i32.const 41) (i64.const 2) (i64.const 0x80001) (i64.const 0))))   ;; AF_INET, SOCK_STREAM | SOCK_CLOEXEC
    (i32.store (i32.const 0x2080) (i32.const 1))
    (drop (call $sys (i64.const 54) (i64.extend_i32_u (global.get $lfd)) (i64.const 1) (i64.const 2) (call $p (i32.const 0x2080)) (i64.const 4) (i64.const 0)))
    (drop (call $sys (i64.const 54) (i64.extend_i32_u (global.get $lfd)) (i64.const 6) (i64.const 1) (call $p (i32.const 0x2080)) (i64.const 4) (i64.const 0)))
    (local.set $n (i32.wrap_i64 (call $parse_len (local.get $port) (call $strlen (local.get $port)))))
    (i32.store16 (i32.const 0x2040) (i32.const 2))
    (i32.store16 (i32.const 0x2042) (i32.or (i32.shl (i32.and (local.get $n) (i32.const 255)) (i32.const 8)) (i32.shr_u (local.get $n) (i32.const 8))))
    (i32.store (i32.const 0x2044) (call $parse_ip (local.get $host)))
    (if (i32.or (i32.wrap_i64 (call $sys3 (i32.const 49) (i64.extend_i32_u (global.get $lfd)) (call $p (i32.const 0x2040)) (i64.const 16)))
                (i32.wrap_i64 (call $sys3 (i32.const 50) (i64.extend_i32_u (global.get $lfd)) (i64.const 65535) (i64.const 0))))
      (then (call $fatal (i32.const 0x101140))))
    ;; the rings (their memory must be page aligned) and the eventfd (EFD_CLOEXEC; blocking, so a read of it waits) of rings 1..7
    (if (i32.and (i32.wrap_i64 (global.get $mb)) (i32.const 4095)) (then (call $fatal (i32.const 0x1011c0))))
    (global.set $efd (i32.wrap_i64 (call $sys3 (i32.const 290) (i64.const 0) (i64.const 0x80000) (i64.const 0))))
    (local.set $i (i32.const 0))
    (block $d (loop $l
      (local.set $r (call $ring (local.get $i)))
      (if (i64.lt_s (local.get $r) (i64.const 0))
        (then (if (i32.eqz (local.get $i)) (then (call $fatal_e (i32.const 0x1011c0) (local.get $r)))) (br $d)))
      (br_if $l (i32.lt_u (local.tee $i (i32.add (local.get $i) (i32.const 1))) (i32.const 8)))))
    (global.set $nr (local.get $i))
    (local.set $i (i32.const 0))
    (loop $l (call $arm_acc (i32.const 0)) (br_if $l (i32.lt_u (local.tee $i (i32.add (local.get $i) (i32.const 1))) (i32.const 16))))
    (call $arm_poll)
    (call $log (i32.const 0x101080))
    (loop $forever
      ;; rings 1..7: submit, and run the work that came in (SQ flags: 2 IORING_SQ_CQ_OVERFLOW, 4 IORING_SQ_TASKRUN)
      (local.set $i (i32.const 1))
      (block $d (loop $l
        (br_if $d (i32.ge_u (local.get $i) (global.get $nr)))
        (if (i32.or (call $queued (local.get $i)) (i32.and (i32.load (i32.add (call $rp (local.get $i)) (global.get $o_sqf))) (i32.const 6)))
          (then (call $enter (local.get $i) (i32.const 1) (i32.const 0))))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $l)))
      ;; ring 0: submit, and wait unless there is something to do already
      (local.set $h (i32.eqz (global.get $nready)))
      (local.set $i (i32.const 0))
      (loop $l
        (if (call $cq_ready (local.get $i)) (then (local.set $h (i32.const 0))))
        (br_if $l (i32.lt_u (local.tee $i (i32.add (local.get $i) (i32.const 1))) (global.get $nr))))
      (call $enter (i32.const 0) (i32.const 1) (local.get $h))
      (local.set $i (i32.const 1))
      (block $d (loop $l
        (br_if $d (i32.ge_u (local.get $i) (global.get $nr)))
        (if (i32.and (i32.load (i32.add (call $rp (local.get $i)) (global.get $o_sqf))) (i32.const 6))
          (then (call $enter (local.get $i) (i32.const 1) (i32.const 0))))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $l)))
      (global.set $now (call $clock (i32.const 0)))
      (local.set $i (i32.const 0))
      (loop $l
        (call $drain (local.get $i))
        (br_if $l (i32.lt_u (local.tee $i (i32.add (local.get $i) (i32.const 1))) (global.get $nr))))
      (call $run_ready)
      (br $forever)))

  (func $drain (param $r i32)
    (local $p i32) (local $h i32)
    (local.set $p (call $rp (local.get $r)))
    (block $d (loop $l
      (local.set $h (i32.load (i32.add (local.get $p) (global.get $o_cqh))))
      (br_if $d (i32.eq (local.get $h) (i32.load (i32.add (local.get $p) (global.get $o_cqt)))))
      (call $on_cqe (i32.add (i32.add (local.get $p) (global.get $o_cqes)) (i32.shl (i32.and (local.get $h) (i32.const 127)) (i32.const 4))))
      (i32.store (i32.add (local.get $p) (global.get $o_cqh)) (i32.add (local.get $h) (i32.const 1)))
      (br $l))))

  ;; serve the connections that had to wait (OUT busy, or more requests than one turn's output)
  (func $run_ready
    (local $i i32) (local $n i32) (local $ci i32) (local $c i32)
    (local.set $n (global.get $nready))
    (block $d (loop $l
      (br_if $d (i32.ge_u (local.get $i) (local.get $n)))
      (local.set $ci (i32.load (i32.add (i32.const 0xa400000) (i32.shl (local.get $i) (i32.const 2)))))
      (local.set $c (call $conn (local.get $ci)))
      (i32.store (local.get $c) (i32.and (i32.load (local.get $c)) (i32.const -33)))
      (call $serve (local.get $ci) (i32.const 0) (i32.const 0))
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (br $l)))
    (global.set $nready (i32.sub (global.get $nready) (local.get $n)))
    (if (global.get $nready)
      (then (memory.copy (i32.const 0xa400000) (i32.add (i32.const 0xa400000) (i32.shl (local.get $n) (i32.const 2)))
              (i32.shl (global.get $nready) (i32.const 2))))))
)

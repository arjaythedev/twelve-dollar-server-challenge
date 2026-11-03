*> Feed API in COBOL: one epoll loop, SQLite in-process, group commit per loop iteration.
*> Config comes only from SQLITE_PATH, JWT_SECRET, HOST, PORT. See README.md.
IDENTIFICATION DIVISION.
PROGRAM-ID. server.
DATA DIVISION.
WORKING-STORAGE SECTION.
*> ---------- C interop ----------
01 NUL-P                USAGE POINTER.
01 IGN-P                USAGE POINTER.
01 IGN-I REDEFINES IGN-P PIC S9(18) COMP-5.
01 TMP-P                USAGE POINTER.
01 TMP-P2               USAGE POINTER.
01 RC                   PIC S9(9) COMP-5.
01 W-R                  PIC S9(9) COMP-5.
01 W-N                  PIC S9(9) COMP-5.
01 W-I                  PIC S9(9) COMP-5.
01 W-K                  PIC S9(9) COMP-5.
01 W-L                  PIC S9(9) COMP-5.
01 RLIM.
   05 RL-CUR            PIC S9(18) COMP-5.
   05 RL-MAX            PIC S9(18) COMP-5.
*> one input byte as an int; only the low byte is ever written, so the rest stays zero
01 BYTE-W               PIC S9(9) COMP-5 VALUE 0.
01 BYTE-X REDEFINES BYTE-W PIC X(4).
01 BYTE2-W              PIC S9(9) COMP-5 VALUE 0.
01 BYTE2-X REDEFINES BYTE2-W PIC X(4).
01 OUT-W                PIC S9(9) COMP-5 VALUE 0.
01 OUT-X REDEFINES OUT-W PIC X(4).
01 NUM-D18              PIC 9(18).
01 NUM-X18 REDEFINES NUM-D18 PIC X(18).
01 CL-ED                PIC Z(6)9.

*> ---------- config ----------
01 ENV-P                USAGE POINTER.
01 DB-PATH-P            USAGE POINTER.
01 HOST-Z               PIC X(64) VALUE LOW-VALUES.
01 PORT-N               PIC S9(9) COMP-5 VALUE 3000.
01 STARTED              PIC S9(18) COMP-5.
01 NOW-S                PIC S9(18) COMP-5.   *> time() writes it through its pointer argument
01 UPTIME               PIC S9(18) COMP-5.
01 ERRNO-P              USAGE POINTER.

*> ---------- HMAC-SHA256 (SHA-256 from libcrypto) ----------
01 KEY-BUF              PIC X(64).
*> SHA256_CTX states (112 bytes in OpenSSL 3) after absorbing key^ipad / key^opad, computed once
01 CTX-IN               PIC X(128).
01 CTX-OUT              PIC X(128).
01 CTX-W                PIC X(128).
01 INNER                PIC X(32).
01 MAC                  PIC X(32).
01 SIG                  PIC X(32).
01 PAD-BYTE             PIC X(64).

*> ---------- tables ----------
01 B64-TABLE.
   05 B64V              PIC S9(9) COMP-5 OCCURS 256.
01 HEX-TABLE.
   05 HEXV              PIC S9(9) COMP-5 OCCURS 256.
*> base64 group decode: byte1 = A4(v1)+D16(v2), byte2 = M16(v2)+D4(v3), byte3 = M64(v3)+v4
01 B64-PARTS.
   05 B64-PART OCCURS 64.
      10 T-A4           PIC S9(9) COMP-5.
      10 T-D16          PIC S9(9) COMP-5.
      10 T-M16          PIC S9(9) COMP-5.
      10 T-D4           PIC S9(9) COMP-5.
      10 T-M64          PIC S9(9) COMP-5.
01 B64-ALPHA            PIC X(64) VALUE
   'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_'.
01 HEX-ALPHA            PIC X(16) VALUE '0123456789abcdef'.

*> ---------- sockets / epoll ----------
01 LFD                  PIC S9(9) COMP-5 VALUE -1.
01 NCONNS               PIC S9(9) COMP-5 VALUE 0.
01 LIM                  PIC S9(9) COMP-5.
01 CONN-HI              PIC S9(9) COMP-5.
01 CONN-LO              PIC S9(9) COMP-5.
01 CONN-MAX             PIC S9(9) COMP-5.
01 ACC-LIMIT            PIC S9(9) COMP-5.
01 SPAWN-PENDING        PIC S9(9) COMP-5 VALUE 0.
01 SPAWNED              PIC S9(9) COMP-5 VALUE 0.
01 READY-FD             PIC S9(9) COMP-5 VALUE -1.
01 NOTIFY-FD            PIC S9(9) COMP-5 VALUE -1.
01 CHILD-PID            PIC S9(9) COMP-5.
01 PIPE-FDS.
   05 PIPE-RD           PIC S9(9) COMP-5.
   05 PIPE-WR           PIC S9(9) COMP-5.
01 ARG-COUNT            PIC S9(9) COMP-5.
01 ARG-TEXT             PIC X(64).
01 ARG-FD-ED            PIC Z(8)9.
01 EXE-Z                PIC X(15) VALUE Z"/proc/self/exe".
01 ARG0-Z               PIC X(7) VALUE Z"server".
01 ARG1-Z               PIC X(9) VALUE Z"--shared".
01 CKPT-Z               PIC X(7) VALUE Z"--ckpt".
01 CK-LOG               PIC S9(9) COMP-5.
01 CK-DONE              PIC S9(9) COMP-5.
01 ARG2-Z               PIC X(16).
01 EXEC-ARGV.
   05 EXEC-ARG          USAGE POINTER OCCURS 4.
01 ONE-BYTE             PIC X.
01 EPFD                 PIC S9(9) COMP-5.
01 CFD                  PIC S9(9) COMP-5.
01 CX                   PIC S9(9) COMP-5.
01 ONE-I                PIC S9(9) COMP-5 VALUE 1.
01 SADDR.
   05 SA-FAMILY         PIC 9(4) COMP-5.
   05 SA-PORT-HI        USAGE BINARY-CHAR UNSIGNED.
   05 SA-PORT-LO        USAGE BINARY-CHAR UNSIGNED.
   05 SA-ADDR           PIC X(4).
   05 SA-ZERO           PIC X(8).
01 CE.
   05 CE-EVENTS         PIC 9(9) COMP-5.
   05 CE-FD             PIC S9(9) COMP-5.
   05 CE-HI             PIC S9(9) COMP-5.
01 NEV                  PIC S9(9) COMP-5.
01 EI                   PIC S9(9) COMP-5.
01 EVS.
   05 EV OCCURS 512.
      10 EV-EVENTS      PIC 9(9) COMP-5.
      10 EV-FD          PIC S9(9) COMP-5.
      10 EV-HI          PIC S9(9) COMP-5.

*> ---------- connections (index = fd + 1) ----------
01 SOCK-FD              PIC S9(9) COMP-5.
01 FX                   PIC S9(9) COMP-5.
01 CONNS.
   05 CN OCCURS 65536.
      10 C-IN-P         USAGE POINTER.
      10 C-OUT-P        USAGE POINTER.
      10 C-IN-LEN       PIC S9(9) COMP-5.
      10 C-OUT-LEN      PIC S9(9) COMP-5.
      10 C-OUT-OFF      PIC S9(9) COMP-5.
      10 C-OPEN         PIC S9(9) COMP-5.
      10 C-CLOSING      PIC S9(9) COMP-5.
01 RBUF                 PIC X(65536).
01 INCAP                PIC S9(9) COMP-5 VALUE 131072.
01 W-SPACE              PIC S9(9) COMP-5.
01 FROM-CONN            PIC S9(9) COMP-5.

*> ---------- batch: responses wait in the arena until COMMIT ----------
01 ARENA                PIC X(4194304).
01 ALEN                 PIC S9(9) COMP-5 VALUE 0.
01 ALIMIT               PIC S9(9) COMP-5 VALUE 4063232.  *> arena size minus room for one worst-case response
01 NSEGS                PIC S9(9) COMP-5 VALUE 0.
01 SEGS.
   05 SG OCCURS 65536.
      10 SG-FD          PIC S9(9) COMP-5.
      10 SG-OFF         PIC S9(9) COMP-5.
      10 SG-LEN         PIC S9(9) COMP-5.
01 SEG-START            PIC S9(9) COMP-5.
01 NDEF                 PIC S9(9) COMP-5 VALUE 0.
01 NTODO                PIC S9(9) COMP-5.
01 DQ-I                 PIC S9(9) COMP-5.
01 DEFQ.
   05 DEF-FD            PIC S9(9) COMP-5 OCCURS 65536.
01 TODOQ.
   05 TODO-FD           PIC S9(9) COMP-5 OCCURS 65536.
01 ARENA-FULL           PIC S9(9) COMP-5.
01 IN-TXN               PIC S9(9) COMP-5 VALUE 0.
01 TXN-WRITES           PIC S9(9) COMP-5 VALUE 0.
01 BATCH-FAILED         PIC S9(9) COMP-5 VALUE 0.
01 BODY-START           PIC S9(9) COMP-5.
01 CL-POS               PIC S9(9) COMP-5.
01 STATUS-CODE          PIC S9(9) COMP-5.
01 ERR-BODY             PIC X(64).
01 ERR-LEN              PIC S9(9) COMP-5.
01 MARK                 PIC S9(9) COMP-5.

*> ---------- request parsing ----------
01 PN                   PIC S9(9) COMP-5.
01 USED                 PIC S9(9) COMP-5.
01 W-LEFT               PIC S9(9) COMP-5.
01 CLOSING-F            PIC S9(9) COMP-5.
01 RQ-P                 PIC S9(9) COMP-5.
01 RQ-HE                PIC S9(9) COMP-5.
01 RQ-LE                PIC S9(9) COMP-5.
01 RQ-SP1               PIC S9(9) COMP-5.
01 RQ-SP2               PIC S9(9) COMP-5.
01 M-L                  PIC S9(9) COMP-5.
01 PATH-S               PIC S9(9) COMP-5.
01 PATH-L               PIC S9(9) COMP-5.
01 AUTH-S               PIC S9(9) COMP-5.
01 AUTH-L               PIC S9(9) COMP-5.
01 CLEN                 PIC S9(9) COMP-5.
01 CLOSE-F              PIC S9(9) COMP-5.
01 EXPECT-F             PIC S9(9) COMP-5.
01 BODY-S               PIC S9(9) COMP-5.
01 H-P                  PIC S9(9) COMP-5.
01 H-L                  PIC S9(9) COMP-5.
01 H-C                  PIC S9(9) COMP-5.
01 H-V                  PIC S9(9) COMP-5.
01 H-VE                 PIC S9(9) COMP-5.
01 H-KN                 PIC S9(9) COMP-5.
01 HN-BUF               PIC X(32).
01 W-LIMIT              PIC S9(9) COMP-5.
01 IS-GET               PIC S9(9) COMP-5.
01 IS-POST              PIC S9(9) COMP-5.
01 SEG-S                PIC S9(9) COMP-5.
01 SEG-L                PIC S9(9) COMP-5.
01 SEG-SL               PIC S9(9) COMP-5.
01 POST-ID              PIC S9(18) COMP-5.
01 ID-OK                PIC S9(9) COMP-5.

*> ---------- auth ----------
01 AUTH-ERR             PIC S9(9) COMP-5.
01 USER-ID              PIC S9(18) COMP-5.
01 UNAME                PIC X(1024).
01 UNAME-L              PIC S9(9) COMP-5.
01 T-S                  PIC S9(9) COMP-5.
01 T-E                  PIC S9(9) COMP-5.
01 T-D1                 PIC S9(9) COMP-5.
01 T-D2                 PIC S9(9) COMP-5.
01 T-D3                 PIC S9(9) COMP-5.
01 DIFF                 PIC S9(9) COMP-5.
01 EXP-D                USAGE COMP-2.
01 NOW-D                USAGE COMP-2.

*> ---------- base64url ----------
01 B64-SRC              PIC S9(9) COMP-5.
01 B64-SL               PIC S9(9) COMP-5.
01 B64-N                PIC S9(9) COMP-5.
01 B64-RC               PIC S9(9) COMP-5.
01 B64-OUT              PIC X(4096).
01 B64-Q                PIC S9(9) COMP-5.
01 B64-REM              PIC S9(9) COMP-5.
01 V1                   PIC S9(9) COMP-5.
01 V2                   PIC S9(9) COMP-5.
01 V3                   PIC S9(9) COMP-5.
01 V4                   PIC S9(9) COMP-5.

*> ---------- JSON ----------
01 J-P                  PIC S9(9) COMP-5.
01 J-E                  PIC S9(9) COMP-5.
01 J-OK                 PIC S9(9) COMP-5.
01 J-STATE              PIC S9(9) COMP-5.
01 J-DEPTH              PIC S9(9) COMP-5.
01 J-STACK.
   05 J-STK             PIC S9(9) COMP-5 OCCURS 70.   *> 1 = object, 2 = array
01 JS-MODE              PIC S9(9) COMP-5.
01 JS-CAP               PIC S9(9) COMP-5.
01 JS-LEN               PIC S9(9) COMP-5.
01 JS-RC                PIC S9(9) COMP-5.
01 JS-BUF               PIC X(65536).
01 JS-CP                PIC S9(9) COMP-5.
01 JS-LO                PIC S9(9) COMP-5.
01 JS-UL                PIC S9(9) COMP-5.
01 JS-U.
   05 JS-UB             PIC X OCCURS 4.
01 H1                   PIC S9(9) COMP-5.
01 H2                   PIC S9(9) COMP-5.
01 H3                   PIC S9(9) COMP-5.
01 H4                   PIC S9(9) COMP-5.
01 KEY-BUF2             PIC X(32).
01 KEY-L                PIC S9(9) COMP-5.
01 VAL-S                PIC S9(9) COMP-5.
01 WN                   PIC S9(9) COMP-5.
01 WANTS.
   05 WANT OCCURS 4.
      10 WK             PIC X(8).
      10 WKL            PIC S9(9) COMP-5.
      10 WVS            PIC S9(9) COMP-5.
      10 WVE            PIC S9(9) COMP-5.

*> ---------- create ----------
01 TR-S                 PIC S9(9) COMP-5.
01 TR-E                 PIC S9(9) COMP-5.
01 NCHARS               PIC S9(9) COMP-5.

*> ---------- output helpers ----------
01 ESC-L                PIC S9(9) COMP-5.
01 ESC-I                PIC S9(9) COMP-5.
01 ESC-RUN              PIC S9(9) COMP-5.
01 PUT-N                PIC S9(18) COMP-5.
01 ROWS                 PIC S9(9) COMP-5.

*> ---------- SQLite ----------
01 DB                   USAGE POINTER.
01 ST-FEED              USAGE POINTER.
01 ST-POST              USAGE POINTER.
01 ST-INS               USAGE POINTER.
01 ST-LIKE              USAGE POINTER.
01 ST-EXISTS            USAGE POINTER.
01 ST-ONE               USAGE POINTER.
01 ST-BEGIN             USAGE POINTER.
01 ST-COMMIT            USAGE POINTER.
01 ST-ROLLBACK          USAGE POINTER.
01 COL-P                USAGE POINTER.
01 COL-I                PIC S9(9) COMP-5.
01 WS-STMT              USAGE POINTER.
01 ST-BEGINIMM          USAGE POINTER.
01 CREATED              PIC S9(9) COMP-5.

*> ---------- response text ----------
01 K-200  PIC X(17) VALUE 'HTTP/1.1 200 OK' & X"0D0A".
01 K-201  PIC X(22) VALUE 'HTTP/1.1 201 Created' & X"0D0A".
01 K-400  PIC X(26) VALUE 'HTTP/1.1 400 Bad Request' & X"0D0A".
01 K-401  PIC X(27) VALUE 'HTTP/1.1 401 Unauthorized' & X"0D0A".
01 K-404  PIC X(24) VALUE 'HTTP/1.1 404 Not Found' & X"0D0A".
01 K-500  PIC X(36) VALUE 'HTTP/1.1 500 Internal Server Error' & X"0D0A".
01 K-503  PIC X(34) VALUE 'HTTP/1.1 503 Service Unavailable' & X"0D0A".
01 K-CLOSE PIC X(19) VALUE 'Connection: close' & X"0D0A".
01 K-HEAD PIC X(58) VALUE 'Content-Type: application/json' & X"0D0A" & 'Content-Length:       ' & X"0D0A0D0A".
01 K-CONT PIC X(25) VALUE 'HTTP/1.1 100 Continue' & X"0D0A0D0A".
01 K-FEED1 PIC X(10) VALUE '{"posts":['.
01 K-FEED2 PIC X(2) VALUE ']}'.
01 K-POST1 PIC X(8) VALUE '{"post":'.
01 K-ID   PIC X(6) VALUE '{"id":'.
01 K-BODY PIC X(9) VALUE ',"body":"'.
01 K-CREATED PIC X(16) VALUE '","created_at":"'.
01 K-AUTHOR PIC X(12) VALUE '","author":"'.
01 K-LIKES PIC X(15) VALUE '","like_count":'.
01 K-ZERO-END PIC X(18) VALUE '","like_count":0}}'.
01 K-LIKED PIC X(30) VALUE '{"liked":true,"already_liked":'.
01 K-PID  PIC X(11) VALUE ',"post_id":'.
01 K-HEALTH PIC X(36) VALUE '{"status":"ok","db":"ok","uptime_s":'.
01 K-DEGRADED PIC X(49) VALUE '{"status":"degraded","db":"unreachable","error":"'.
01 E-NOTFOUND PIC X(21) VALUE '{"error":"not found"}'.
01 E-BADID PIC X(27) VALUE '{"error":"invalid post id"}'.
01 E-NOPOST PIC X(26) VALUE '{"error":"post not found"}'.
01 E-MISSING PIC X(32) VALUE '{"error":"missing bearer token"}'.
01 E-INVALID PIC X(36) VALUE '{"error":"invalid or expired token"}'.
01 E-PAYLOAD PIC X(33) VALUE '{"error":"invalid token payload"}'.
01 E-MALFORMED PIC X(31) VALUE '{"error":"malformed JSON body"}'.
01 E-REQUIRED PIC X(28) VALUE '{"error":"body is required"}'.
01 E-TOOLONG PIC X(47) VALUE '{"error":"body must be at most 500 characters"}'.
01 E-INTERNAL PIC X(33) VALUE '{"error":"internal server error"}'.

LINKAGE SECTION.
01 LK-IN                PIC X(131072).
01 LK-J                 PIC X(131072).
01 LK-COL               PIC X(1048576).
01 LK-ESC               PIC X(1048576).
01 LK-OUT               PIC X(4194304).
01 LK-CSTR              PIC X(4096).
01 LK-ERRNO             PIC S9(9) COMP-5.

PROCEDURE DIVISION.
MAIN-PARA.
    CALL STATIC "sqlite3_initialize"
    MOVE 1 TO IGN-I
    CALL STATIC "getenv" USING BY REFERENCE Z"SQLITE_PATH" RETURNING DB-PATH-P
    CALL STATIC "getenv" USING BY REFERENCE Z"JWT_SECRET" RETURNING ENV-P
    IF DB-PATH-P = NULL OR ENV-P = NULL
        DISPLAY "SQLITE_PATH and JWT_SECRET are required" UPON SYSERR
        STOP RUN RETURNING 1
    END-IF
    ACCEPT ARG-COUNT FROM ARGUMENT-NUMBER
    IF ARG-COUNT >= 1
        ACCEPT ARG-TEXT FROM ARGUMENT-VALUE
        IF ARG-TEXT = "--ckpt" PERFORM CHECKPOINTER END-IF
    END-IF
    *> an elastic worker is started as "--shared <fd>"
    IF ARG-COUNT >= 2
        ACCEPT ARG-TEXT FROM ARGUMENT-VALUE
        MOVE FUNCTION NUMVAL(ARG-TEXT) TO NOTIFY-FD
    END-IF
    PERFORM INIT-HMAC
    CALL STATIC "getenv" USING BY REFERENCE Z"HOST" RETURNING ENV-P
    IF ENV-P = NULL
        MOVE Z"0.0.0.0" TO HOST-Z
    ELSE
        CALL STATIC "strlen" USING BY VALUE ENV-P RETURNING W-L
        SET ADDRESS OF LK-CSTR TO ENV-P
        IF W-L > 60 MOVE 60 TO W-L END-IF
        MOVE LK-CSTR(1:W-L) TO HOST-Z(1:W-L)
    END-IF
    CALL STATIC "getenv" USING BY REFERENCE Z"PORT" RETURNING ENV-P
    IF ENV-P NOT = NULL
        CALL STATIC "atoi" USING BY VALUE ENV-P RETURNING PORT-N
    END-IF
    CALL STATIC "signal" USING BY VALUE 13 BY VALUE IGN-P
    *> SIGCHLD ignored: workers are reaped automatically
    CALL STATIC "signal" USING BY VALUE 17 BY VALUE IGN-P
    CALL STATIC "getrlimit" USING BY VALUE 7 BY REFERENCE RLIM
    IF RL-CUR < RL-MAX
        MOVE RL-MAX TO RL-CUR
        CALL STATIC "setrlimit" USING BY VALUE 7 BY REFERENCE RLIM
    END-IF
    *> leave room for SQLite's files and the listener
    IF RL-CUR > 65536 MOVE 65536 TO LIM ELSE MOVE RL-CUR TO LIM END-IF
    DIVIDE LIM BY 16 GIVING W-N
    IF W-N < 64 MOVE 64 TO W-N END-IF
    SUBTRACT W-N FROM LIM GIVING CONN-HI
    DIVIDE CONN-HI BY 8 GIVING W-N
    SUBTRACT W-N FROM CONN-HI GIVING CONN-LO
    SUBTRACT 16 FROM LIM GIVING CONN-MAX
    CALL STATIC "__errno_location" RETURNING ERRNO-P
    SET ADDRESS OF LK-ERRNO TO ERRNO-P
    PERFORM INIT-TABLES
    CALL STATIC "time" USING BY REFERENCE STARTED
    MOVE STARTED TO NOW-S
    PERFORM DB-OPEN
    IF ARG-COUNT = 0 PERFORM SPAWN-CKPT END-IF
    PERFORM LISTEN-INIT
    PERFORM LISTEN-START
    *> a worker started by SPAWN-WORKER tells its parent that it is listening
    IF NOTIFY-FD >= 0
        MOVE "1" TO ONE-BYTE
        CALL STATIC "write" USING BY VALUE NOTIFY-FD BY REFERENCE ONE-BYTE BY VALUE 1
        CALL STATIC "close" USING BY VALUE NOTIFY-FD
    END-IF
    PERFORM EVENT-LOOP
    STOP RUN.

INIT-HMAC.
    MOVE LOW-VALUES TO KEY-BUF
    CALL STATIC "strlen" USING BY VALUE ENV-P RETURNING W-L
    IF W-L > 64
        CALL STATIC "SHA256" USING BY VALUE ENV-P BY VALUE W-L BY REFERENCE KEY-BUF
    ELSE
        IF W-L > 0
            SET ADDRESS OF LK-CSTR TO ENV-P
            MOVE LK-CSTR(1:W-L) TO KEY-BUF(1:W-L)
        END-IF
    END-IF
    MOVE ALL X"36" TO PAD-BYTE
    CALL "CBL_XOR" USING KEY-BUF PAD-BYTE BY VALUE 64
    CALL STATIC "SHA256_Init" USING BY REFERENCE CTX-IN
    CALL STATIC "SHA256_Update" USING BY REFERENCE CTX-IN BY REFERENCE PAD-BYTE BY VALUE 64
    MOVE ALL X"5C" TO PAD-BYTE
    CALL "CBL_XOR" USING KEY-BUF PAD-BYTE BY VALUE 64
    CALL STATIC "SHA256_Init" USING BY REFERENCE CTX-OUT
    CALL STATIC "SHA256_Update" USING BY REFERENCE CTX-OUT BY REFERENCE PAD-BYTE BY VALUE 64.

INIT-TABLES.
    PERFORM VARYING W-I FROM 1 BY 1 UNTIL W-I > 256
        MOVE -1 TO B64V(W-I)
        MOVE -1 TO HEXV(W-I)
    END-PERFORM
    PERFORM VARYING W-I FROM 1 BY 1 UNTIL W-I > 64
        MOVE B64-ALPHA(W-I:1) TO BYTE-X(1:1)
        ADD 1 TO BYTE-W GIVING W-K
        SUBTRACT 1 FROM W-I GIVING B64V(W-K)
        SUBTRACT 1 FROM W-I GIVING W-N
        COMPUTE T-A4(W-I) = FUNCTION MOD(W-N * 4, 256)
        COMPUTE T-D16(W-I) = W-N / 16
        COMPUTE T-M16(W-I) = FUNCTION MOD(W-N, 16) * 16
        COMPUTE T-D4(W-I) = W-N / 4
        COMPUTE T-M64(W-I) = FUNCTION MOD(W-N, 4) * 64
    END-PERFORM
    PERFORM VARYING W-I FROM 1 BY 1 UNTIL W-I > 16
        MOVE HEX-ALPHA(W-I:1) TO BYTE-X(1:1)
        ADD 1 TO BYTE-W GIVING W-K
        SUBTRACT 1 FROM W-I GIVING HEXV(W-K)
        IF W-I > 10
            MOVE FUNCTION UPPER-CASE(HEX-ALPHA(W-I:1)) TO BYTE-X(1:1)
            ADD 1 TO BYTE-W GIVING W-K
            SUBTRACT 1 FROM W-I GIVING HEXV(W-K)
        END-IF
    END-PERFORM
    MOVE 0 TO BYTE-W.

*> ================= database =================
DB-OPEN.
    CALL STATIC "sqlite3_open_v2" USING BY VALUE DB-PATH-P BY REFERENCE DB BY VALUE 2 BY VALUE NUL-P
        RETURNING RC
    IF RC NOT = 0
        DISPLAY "cannot open database" UPON SYSERR
        STOP RUN RETURNING 1
    END-IF
    CALL STATIC "sqlite3_busy_timeout" USING BY VALUE DB BY VALUE 5000
    CALL STATIC "sqlite3_exec" USING BY VALUE DB
        BY REFERENCE Z"PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA mmap_size=1073741824; PRAGMA cache_size=-16000; SELECT count(*) FROM users WHERE id = 1;"
        BY VALUE NUL-P BY VALUE NUL-P BY VALUE NUL-P RETURNING RC
    IF RC NOT = 0
        DISPLAY "pragma failed" UPON SYSERR
        STOP RUN RETURNING 1
    END-IF
    *> The checkpointer process keeps the WAL short. This only fires if it can't keep up (CPU saturated
    *> for a long time), so the WAL stays bounded at about 256 MB.
    CALL STATIC "sqlite3_exec" USING BY VALUE DB BY REFERENCE Z"PRAGMA wal_autocheckpoint=65536;"
        BY VALUE NUL-P BY VALUE NUL-P BY VALUE NUL-P
    CALL STATIC "sqlite3_prepare_v2" USING BY VALUE DB
        BY REFERENCE Z"SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) FROM posts p JOIN users u ON u.id = p.user_id ORDER BY p.created_at DESC, p.id DESC LIMIT 20"
        BY VALUE -1 BY REFERENCE ST-FEED BY VALUE NUL-P RETURNING RC
    PERFORM CHECK-PREP
    CALL STATIC "sqlite3_prepare_v2" USING BY VALUE DB
        BY REFERENCE Z"SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = ?"
        BY VALUE -1 BY REFERENCE ST-POST BY VALUE NUL-P RETURNING RC
    PERFORM CHECK-PREP
    CALL STATIC "sqlite3_prepare_v2" USING BY VALUE DB
        BY REFERENCE Z"INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at"
        BY VALUE -1 BY REFERENCE ST-INS BY VALUE NUL-P RETURNING RC
    PERFORM CHECK-PREP
    CALL STATIC "sqlite3_prepare_v2" USING BY VALUE DB
        BY REFERENCE Z"INSERT INTO likes (user_id, post_id) SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2) ON CONFLICT (user_id, post_id) DO NOTHING"
        BY VALUE -1 BY REFERENCE ST-LIKE BY VALUE NUL-P RETURNING RC
    PERFORM CHECK-PREP
    CALL STATIC "sqlite3_prepare_v2" USING BY VALUE DB
        BY REFERENCE Z"SELECT 1 FROM posts WHERE id = ?"
        BY VALUE -1 BY REFERENCE ST-EXISTS BY VALUE NUL-P RETURNING RC
    PERFORM CHECK-PREP
    CALL STATIC "sqlite3_prepare_v2" USING BY VALUE DB BY REFERENCE Z"SELECT 1"
        BY VALUE -1 BY REFERENCE ST-ONE BY VALUE NUL-P RETURNING RC
    PERFORM CHECK-PREP
    CALL STATIC "sqlite3_prepare_v2" USING BY VALUE DB BY REFERENCE Z"BEGIN"
        BY VALUE -1 BY REFERENCE ST-BEGIN BY VALUE NUL-P RETURNING RC
    PERFORM CHECK-PREP
    CALL STATIC "sqlite3_prepare_v2" USING BY VALUE DB BY REFERENCE Z"BEGIN IMMEDIATE"
        BY VALUE -1 BY REFERENCE ST-BEGINIMM BY VALUE NUL-P RETURNING RC
    PERFORM CHECK-PREP
    CALL STATIC "sqlite3_prepare_v2" USING BY VALUE DB BY REFERENCE Z"COMMIT"
        BY VALUE -1 BY REFERENCE ST-COMMIT BY VALUE NUL-P RETURNING RC
    PERFORM CHECK-PREP
    CALL STATIC "sqlite3_prepare_v2" USING BY VALUE DB BY REFERENCE Z"ROLLBACK"
        BY VALUE -1 BY REFERENCE ST-ROLLBACK BY VALUE NUL-P RETURNING RC
    PERFORM CHECK-PREP
    *> touch the hot pages before the first request
    CALL STATIC "sqlite3_step" USING BY VALUE ST-FEED RETURNING RC
    CALL STATIC "sqlite3_reset" USING BY VALUE ST-FEED.

CHECK-PREP.
    IF RC NOT = 0
        DISPLAY "prepare failed" UPON SYSERR
        STOP RUN RETURNING 1
    END-IF.

TXN-BEGIN.
    IF IN-TXN = 0
        CALL STATIC "sqlite3_step" USING BY VALUE ST-BEGIN RETURNING RC
        CALL STATIC "sqlite3_reset" USING BY VALUE ST-BEGIN
        IF RC = 101 MOVE 1 TO IN-TXN END-IF
    END-IF.

*> Runs write statement WS-STMT inside the batch transaction, result in RC. With several processes a
*> read snapshot can't be upgraded once another process has committed (SQLITE_BUSY_SNAPSHOT): end the
*> read-only part of the batch, take the write lock with BEGIN IMMEDIATE (waits up to busy_timeout)
*> and retry once.
WSTEP.
    CALL STATIC "sqlite3_step" USING BY VALUE WS-STMT RETURNING RC
    IF (RC = 5 OR RC = 517) AND TXN-WRITES = 0
        CALL STATIC "sqlite3_reset" USING BY VALUE WS-STMT
        IF IN-TXN = 1
            CALL STATIC "sqlite3_step" USING BY VALUE ST-COMMIT RETURNING RC
            CALL STATIC "sqlite3_reset" USING BY VALUE ST-COMMIT
            MOVE 0 TO IN-TXN
        END-IF
        CALL STATIC "sqlite3_step" USING BY VALUE ST-BEGINIMM RETURNING RC
        CALL STATIC "sqlite3_reset" USING BY VALUE ST-BEGINIMM
        IF RC NOT = 101 EXIT PARAGRAPH END-IF
        MOVE 1 TO IN-TXN
        CALL STATIC "sqlite3_step" USING BY VALUE WS-STMT RETURNING RC
    END-IF
    IF RC = 100 OR RC = 101 MOVE 1 TO TXN-WRITES END-IF.

*> a failed statement may have rolled the whole batch back
DB-FAIL.
    IF IN-TXN = 1
        CALL STATIC "sqlite3_get_autocommit" USING BY VALUE DB RETURNING RC
        IF RC NOT = 0
            MOVE 0 TO IN-TXN
            IF TXN-WRITES = 1 MOVE 1 TO BATCH-FAILED END-IF
            MOVE 0 TO TXN-WRITES
        END-IF
    END-IF.

*> ================= network =================
LISTEN-INIT.
    MOVE 2 TO SA-FAMILY
    DIVIDE PORT-N BY 256 GIVING W-N REMAINDER W-K
    MOVE W-N TO SA-PORT-HI
    MOVE W-K TO SA-PORT-LO
    MOVE LOW-VALUES TO SA-ZERO
    CALL STATIC "inet_pton" USING BY VALUE 2 BY REFERENCE HOST-Z BY REFERENCE SA-ADDR RETURNING RC
    IF RC NOT = 1
        DISPLAY "bad HOST" UPON SYSERR
        STOP RUN RETURNING 1
    END-IF
    CALL STATIC "epoll_create1" USING BY VALUE 524288 RETURNING EPFD.

*> SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC; SO_REUSEPORT lets a second worker share the port
LISTEN-START.
    CALL STATIC "socket" USING BY VALUE 2 BY VALUE 526337 BY VALUE 0 RETURNING LFD
    CALL STATIC "setsockopt" USING BY VALUE LFD BY VALUE 1 BY VALUE 2 BY REFERENCE ONE-I BY VALUE 4
    CALL STATIC "setsockopt" USING BY VALUE LFD BY VALUE 1 BY VALUE 15 BY REFERENCE ONE-I BY VALUE 4
    CALL STATIC "setsockopt" USING BY VALUE LFD BY VALUE 6 BY VALUE 1 BY REFERENCE ONE-I BY VALUE 4
    CALL STATIC "bind" USING BY VALUE LFD BY REFERENCE SADDR BY VALUE 16 RETURNING RC
    IF RC NOT = 0
        DISPLAY "bind failed" UPON SYSERR
        STOP RUN RETURNING 1
    END-IF
    CALL STATIC "listen" USING BY VALUE LFD BY VALUE 65535 RETURNING RC
    MOVE 1 TO CE-EVENTS
    MOVE LFD TO CE-FD
    MOVE 0 TO CE-HI
    CALL STATIC "epoll_ctl" USING BY VALUE EPFD BY VALUE 1 BY VALUE LFD BY REFERENCE CE.

*> takes what is already queued, then leaves new clients to the other worker
LISTEN-STOP.
    MOVE CONN-MAX TO ACC-LIMIT
    PERFORM ACCEPT-CONNS
    CALL STATIC "close" USING BY VALUE LFD
    MOVE -1 TO LFD.

*> Every keep-alive client holds a descriptor, and LimitNOFILE caps descriptors per process. Close to
*> that cap this process starts one more copy of itself on the same port (SO_REUSEPORT) and stops
*> accepting until it has room again, so new clients go to the other copy. Below the cap there is one process.
SPAWN-WORKER.
    MOVE 0 TO SPAWN-PENDING
    MOVE 1 TO SPAWNED
    CALL STATIC "pipe2" USING BY REFERENCE PIPE-FDS BY VALUE 524288 RETURNING RC
    IF RC NOT = 0
        *> no pipe, no worker: keep accepting here
        MOVE 0 TO SPAWNED
        MOVE 1 TO CE-EVENTS
        MOVE LFD TO CE-FD
        CALL STATIC "epoll_ctl" USING BY VALUE EPFD BY VALUE 1 BY VALUE LFD BY REFERENCE CE
        EXIT PARAGRAPH
    END-IF
    CALL STATIC "fork" RETURNING CHILD-PID
    IF CHILD-PID = 0
        CALL STATIC "prctl" USING BY VALUE 1 BY VALUE 15
        CALL STATIC "fcntl" USING BY VALUE PIPE-WR BY VALUE 2 BY VALUE 0
        MOVE PIPE-WR TO ARG-FD-ED
        MOVE LOW-VALUES TO ARG2-Z
        MOVE FUNCTION TRIM(ARG-FD-ED) TO ARG2-Z
        INSPECT ARG2-Z REPLACING ALL SPACE BY LOW-VALUE
        SET EXEC-ARG(1) TO ADDRESS OF ARG0-Z
        SET EXEC-ARG(2) TO ADDRESS OF ARG1-Z
        SET EXEC-ARG(3) TO ADDRESS OF ARG2-Z
        SET EXEC-ARG(4) TO NULL
        CALL STATIC "execv" USING BY REFERENCE EXE-Z BY REFERENCE EXEC-ARGV
        CALL STATIC "_exit" USING BY VALUE 1 RETURNING NOTHING
    END-IF
    CALL STATIC "close" USING BY VALUE PIPE-WR
    MOVE PIPE-RD TO READY-FD
    MOVE 1 TO CE-EVENTS
    MOVE READY-FD TO CE-FD
    CALL STATIC "epoll_ctl" USING BY VALUE EPFD BY VALUE 1 BY VALUE READY-FD BY REFERENCE CE
    DISPLAY NCONNS " connections: started worker " CHILD-PID UPON SYSERR.

*> Checkpoints copy the WAL back into the database and fsync it. Run inline they stall every client for
*> the length of the fsync, so a helper process does them and the event loop never waits on the disk.
CHECKPOINTER.
    CALL STATIC "prctl" USING BY VALUE 1 BY VALUE 15
    CALL STATIC "sqlite3_open_v2" USING BY VALUE DB-PATH-P BY REFERENCE DB BY VALUE 2 BY VALUE NUL-P
        RETURNING RC
    IF RC NOT = 0 STOP RUN RETURNING 1 END-IF
    CALL STATIC "sqlite3_busy_timeout" USING BY VALUE DB BY VALUE 100
    CALL STATIC "sqlite3_exec" USING BY VALUE DB
        BY REFERENCE Z"PRAGMA synchronous=NORMAL; PRAGMA wal_autocheckpoint=0;"
        BY VALUE NUL-P BY VALUE NUL-P BY VALUE NUL-P
    PERFORM FOREVER
        MOVE 0 TO CK-LOG CK-DONE
        CALL STATIC "sqlite3_wal_checkpoint_v2" USING BY VALUE DB BY VALUE NUL-P BY VALUE 0
            BY REFERENCE CK-LOG BY REFERENCE CK-DONE
        IF CK-LOG > 0 AND CK-DONE < CK-LOG
            CALL STATIC "usleep" USING BY VALUE 1000
        ELSE
            CALL STATIC "usleep" USING BY VALUE 10000
        END-IF
    END-PERFORM.

SPAWN-CKPT.
    CALL STATIC "fork" RETURNING CHILD-PID
    IF CHILD-PID = 0
        SET EXEC-ARG(1) TO ADDRESS OF ARG0-Z
        SET EXEC-ARG(2) TO ADDRESS OF CKPT-Z
        SET EXEC-ARG(3) TO NULL
        CALL STATIC "execv" USING BY REFERENCE EXE-Z BY REFERENCE EXEC-ARGV
        CALL STATIC "_exit" USING BY VALUE 1 RETURNING NOTHING
    END-IF.

EVENT-LOOP.
    PERFORM FOREVER
        CALL STATIC "epoll_wait" USING BY VALUE EPFD BY REFERENCE EVS BY VALUE 512 BY VALUE -1
            RETURNING NEV
        CALL STATIC "time" USING BY REFERENCE NOW-S
        PERFORM VARYING EI FROM 1 BY 1 UNTIL EI > NEV
            MOVE EV-FD(EI) TO SOCK-FD
            EVALUATE TRUE
            WHEN SOCK-FD = LFD
                MOVE CONN-HI TO ACC-LIMIT
                PERFORM ACCEPT-CONNS
                *> nearly out of descriptors: new clients go to another worker
                IF NCONNS >= CONN-HI
                    IF SPAWNED = 1
                        PERFORM LISTEN-STOP
                    ELSE
                        CALL STATIC "epoll_ctl" USING BY VALUE EPFD BY VALUE 2 BY VALUE LFD BY VALUE NUL-P
                        MOVE 1 TO SPAWN-PENDING
                    END-IF
                END-IF
            WHEN SOCK-FD = READY-FD
                *> the new worker is listening (one byte), or it failed to start (EOF)
                CALL STATIC "read" USING BY VALUE READY-FD BY REFERENCE ONE-BYTE BY VALUE 1 RETURNING W-R
                CALL STATIC "close" USING BY VALUE READY-FD
                MOVE -1 TO READY-FD
                IF W-R = 1
                    PERFORM LISTEN-STOP
                ELSE
                    MOVE 0 TO SPAWNED
                    MOVE 1 TO CE-EVENTS
                    MOVE LFD TO CE-FD
                    CALL STATIC "epoll_ctl" USING BY VALUE EPFD BY VALUE 1 BY VALUE LFD BY REFERENCE CE
                END-IF
            WHEN OTHER
                ADD 1 TO SOCK-FD GIVING FX
                IF C-OPEN(FX) = 1
                    *> each socket waits for exactly one of read or write readiness
                    IF C-OUT-P(FX) NOT = NULL
                        PERFORM ON-WRITABLE
                    ELSE
                        PERFORM ON-READABLE
                    END-IF
                END-IF
            END-EVALUATE
        END-PERFORM
        PERFORM FLUSH-BATCH
        *> requests held back by a full arena
        PERFORM UNTIL NDEF = 0
            MOVE NDEF TO NTODO
            MOVE 0 TO NDEF
            PERFORM VARYING DQ-I FROM 1 BY 1 UNTIL DQ-I > NTODO
                MOVE DEF-FD(DQ-I) TO TODO-FD(DQ-I)
            END-PERFORM
            PERFORM VARYING DQ-I FROM 1 BY 1 UNTIL DQ-I > NTODO
                MOVE TODO-FD(DQ-I) TO SOCK-FD
                ADD 1 TO SOCK-FD GIVING FX
                IF C-OPEN(FX) = 1 AND C-IN-LEN(FX) > 0 AND C-OUT-P(FX) = NULL
                    SET ADDRESS OF LK-IN TO C-IN-P(FX)
                    MOVE C-IN-LEN(FX) TO PN
                    MOVE 1 TO FROM-CONN
                    PERFORM PROCESS-CONN
                END-IF
            END-PERFORM
            PERFORM FLUSH-BATCH
        END-PERFORM
        IF SPAWN-PENDING = 1 PERFORM SPAWN-WORKER END-IF
        IF LFD < 0 AND NCONNS < CONN-LO PERFORM LISTEN-START END-IF
    END-PERFORM.

ACCEPT-CONNS.
    PERFORM UNTIL NCONNS >= ACC-LIMIT
        CALL STATIC "accept4" USING BY VALUE LFD BY VALUE NUL-P BY VALUE NUL-P BY VALUE 526336
            RETURNING CFD
        IF CFD < 0 EXIT PERFORM END-IF
        IF CFD >= 65536
            CALL STATIC "close" USING BY VALUE CFD
        ELSE
            ADD 1 TO CFD GIVING CX
            MOVE 1 TO C-OPEN(CX)
            ADD 1 TO NCONNS
            MOVE 1 TO CE-EVENTS
            MOVE CFD TO CE-FD
            CALL STATIC "epoll_ctl" USING BY VALUE EPFD BY VALUE 1 BY VALUE CFD BY REFERENCE CE
        END-IF
    END-PERFORM.

CONN-CLOSE.
    IF C-IN-P(FX) NOT = NULL CALL STATIC "free" USING BY VALUE C-IN-P(FX) RETURNING NOTHING END-IF
    IF C-OUT-P(FX) NOT = NULL CALL STATIC "free" USING BY VALUE C-OUT-P(FX) RETURNING NOTHING END-IF
    SET C-IN-P(FX) TO NULL
    SET C-OUT-P(FX) TO NULL
    MOVE 0 TO C-IN-LEN(FX) C-OUT-LEN(FX) C-OUT-OFF(FX) C-OPEN(FX) C-CLOSING(FX)
    SUBTRACT 1 FROM NCONNS
    CALL STATIC "close" USING BY VALUE SOCK-FD.

ON-READABLE.
    IF C-IN-LEN(FX) > 0
        SUBTRACT C-IN-LEN(FX) FROM INCAP GIVING W-SPACE
        IF W-SPACE > 0
            SET TMP-P TO C-IN-P(FX)
            SET TMP-P UP BY C-IN-LEN(FX)
            CALL STATIC "read" USING BY VALUE SOCK-FD BY VALUE TMP-P BY VALUE W-SPACE RETURNING W-R
            IF W-R <= 0
                IF W-R < 0 AND LK-ERRNO = 11 EXIT PARAGRAPH END-IF
                PERFORM CONN-CLOSE
                EXIT PARAGRAPH
            END-IF
            ADD W-R TO C-IN-LEN(FX)
        END-IF
        SET ADDRESS OF LK-IN TO C-IN-P(FX)
        MOVE C-IN-LEN(FX) TO PN
        MOVE 1 TO FROM-CONN
    ELSE
        CALL STATIC "read" USING BY VALUE SOCK-FD BY REFERENCE RBUF BY VALUE 65536 RETURNING W-R
        IF W-R <= 0
            IF W-R < 0 AND LK-ERRNO = 11 EXIT PARAGRAPH END-IF
            PERFORM CONN-CLOSE
            EXIT PARAGRAPH
        END-IF
        SET ADDRESS OF LK-IN TO ADDRESS OF RBUF
        MOVE W-R TO PN
        MOVE 0 TO FROM-CONN
    END-IF
    PERFORM PROCESS-CONN.

PROCESS-CONN.
    MOVE ALEN TO SEG-START
    MOVE 0 TO CLOSING-F ARENA-FULL
    PERFORM SERVE
    IF USED < 0
        MOVE SEG-START TO ALEN
        PERFORM CONN-CLOSE
        EXIT PARAGRAPH
    END-IF
    IF ALEN > SEG-START
        ADD 1 TO NSEGS
        MOVE SOCK-FD TO SG-FD(NSEGS)
        MOVE SEG-START TO SG-OFF(NSEGS)
        SUBTRACT SEG-START FROM ALEN GIVING SG-LEN(NSEGS)
    END-IF
    MOVE CLOSING-F TO C-CLOSING(FX)
    IF CLOSING-F = 1
        MOVE 0 TO W-LEFT
    ELSE
        SUBTRACT USED FROM PN GIVING W-LEFT
    END-IF
    IF W-LEFT > 0
        IF FROM-CONN = 0
            CALL STATIC "malloc" USING BY VALUE INCAP RETURNING C-IN-P(FX)
            SET TMP-P TO ADDRESS OF RBUF
            SET TMP-P UP BY USED
            CALL STATIC "memcpy" USING BY VALUE C-IN-P(FX) BY VALUE TMP-P BY VALUE W-LEFT
        ELSE
            IF USED > 0
                SET TMP-P TO C-IN-P(FX)
                SET TMP-P UP BY USED
                CALL STATIC "memmove" USING BY VALUE C-IN-P(FX) BY VALUE TMP-P BY VALUE W-LEFT
            END-IF
        END-IF
        MOVE W-LEFT TO C-IN-LEN(FX)
        IF ARENA-FULL = 1
            ADD 1 TO NDEF
            MOVE SOCK-FD TO DEF-FD(NDEF)
        END-IF
    ELSE
        IF C-IN-P(FX) NOT = NULL
            CALL STATIC "free" USING BY VALUE C-IN-P(FX) RETURNING NOTHING
            SET C-IN-P(FX) TO NULL
            MOVE 0 TO C-IN-LEN(FX)
        END-IF
    END-IF
    IF CLOSING-F = 1 AND ALEN = SEG-START
        PERFORM CONN-CLOSE
    END-IF.

ON-WRITABLE.
    SET ADDRESS OF LK-OUT TO C-OUT-P(FX)
    SUBTRACT C-OUT-OFF(FX) FROM C-OUT-LEN(FX) GIVING W-N
    ADD 1 TO C-OUT-OFF(FX) GIVING W-K
    CALL STATIC "write" USING BY VALUE SOCK-FD BY REFERENCE LK-OUT(W-K:W-N) BY VALUE W-N RETURNING W-R
    IF W-R < 0
        IF LK-ERRNO NOT = 11 PERFORM CONN-CLOSE END-IF
        EXIT PARAGRAPH
    END-IF
    ADD W-R TO C-OUT-OFF(FX)
    IF C-OUT-OFF(FX) < C-OUT-LEN(FX) EXIT PARAGRAPH END-IF
    CALL STATIC "free" USING BY VALUE C-OUT-P(FX) RETURNING NOTHING
    SET C-OUT-P(FX) TO NULL
    MOVE 0 TO C-OUT-LEN(FX) C-OUT-OFF(FX)
    IF C-CLOSING(FX) = 1
        PERFORM CONN-CLOSE
        EXIT PARAGRAPH
    END-IF
    MOVE 1 TO CE-EVENTS
    MOVE SOCK-FD TO CE-FD
    CALL STATIC "epoll_ctl" USING BY VALUE EPFD BY VALUE 3 BY VALUE SOCK-FD BY REFERENCE CE
    IF C-IN-LEN(FX) > 0
        SET ADDRESS OF LK-IN TO C-IN-P(FX)
        MOVE C-IN-LEN(FX) TO PN
        MOVE 1 TO FROM-CONN
        PERFORM PROCESS-CONN
    END-IF.

*> commits the batch, then sends every response produced in it
FLUSH-BATCH.
    IF IN-TXN = 1
        CALL STATIC "sqlite3_step" USING BY VALUE ST-COMMIT RETURNING RC
        CALL STATIC "sqlite3_reset" USING BY VALUE ST-COMMIT
        IF RC NOT = 101
            CALL STATIC "sqlite3_step" USING BY VALUE ST-ROLLBACK RETURNING RC
            CALL STATIC "sqlite3_reset" USING BY VALUE ST-ROLLBACK
            IF TXN-WRITES = 1 MOVE 1 TO BATCH-FAILED END-IF
        END-IF
        MOVE 0 TO IN-TXN TXN-WRITES
    END-IF
    PERFORM VARYING W-I FROM 1 BY 1 UNTIL W-I > NSEGS
        MOVE SG-FD(W-I) TO SOCK-FD
        ADD 1 TO SOCK-FD GIVING FX
        IF C-OPEN(FX) = 1
            IF BATCH-FAILED = 1
                *> nothing in this batch was committed: never claim it was
                PERFORM CONN-CLOSE
            ELSE
                ADD 1 TO SG-OFF(W-I) GIVING W-K
                MOVE SG-LEN(W-I) TO W-N
                CALL STATIC "write" USING BY VALUE SOCK-FD BY REFERENCE ARENA(W-K:W-N) BY VALUE W-N
                    RETURNING W-R
                IF W-R < 0
                    IF LK-ERRNO = 11
                        MOVE 0 TO W-R
                    ELSE
                        PERFORM CONN-CLOSE
                        MOVE W-N TO W-R
                        MOVE 0 TO C-CLOSING(FX)
                    END-IF
                END-IF
                IF W-R < W-N
                    SUBTRACT W-R FROM W-N GIVING C-OUT-LEN(FX)
                    MOVE 0 TO C-OUT-OFF(FX)
                    CALL STATIC "malloc" USING BY VALUE C-OUT-LEN(FX) RETURNING C-OUT-P(FX)
                    SET ADDRESS OF LK-OUT TO C-OUT-P(FX)
                    ADD W-R TO W-K
                    MOVE ARENA(W-K:C-OUT-LEN(FX)) TO LK-OUT(1:C-OUT-LEN(FX))
                    MOVE 4 TO CE-EVENTS
                    MOVE SOCK-FD TO CE-FD
                    CALL STATIC "epoll_ctl" USING BY VALUE EPFD BY VALUE 3 BY VALUE SOCK-FD BY REFERENCE CE
                ELSE
                    IF C-CLOSING(FX) = 1 PERFORM CONN-CLOSE END-IF
                END-IF
            END-IF
        END-IF
    END-PERFORM
    MOVE 0 TO NSEGS ALEN BATCH-FAILED.

*> ================= HTTP =================
*> Serves every complete request in LK-IN(1:PN). USED = bytes consumed, or -1 to drop the connection.
SERVE.
    MOVE 0 TO USED
    PERFORM UNTIL USED >= PN OR CLOSING-F = 1
        IF ALEN > ALIMIT
            MOVE 1 TO ARENA-FULL
            EXIT PERFORM
        END-IF
        ADD 1 TO USED GIVING RQ-P
        *> find the blank line ending the headers
        MOVE 0 TO RQ-HE
        SUBTRACT 3 FROM PN GIVING W-LIMIT
        PERFORM VARYING W-I FROM RQ-P BY 1 UNTIL W-I > W-LIMIT
            IF LK-IN(W-I:1) = X"0D"
                IF LK-IN(W-I:4) = X"0D0A0D0A"
                    MOVE W-I TO RQ-HE
                    EXIT PERFORM
                END-IF
            END-IF
        END-PERFORM
        IF RQ-HE = 0
            SUBTRACT USED FROM PN GIVING W-N
            IF W-N > 65536 MOVE -1 TO USED END-IF
            EXIT PERFORM
        END-IF
        *> request line
        MOVE RQ-P TO RQ-LE
        PERFORM UNTIL LK-IN(RQ-LE:1) = X"0D"
            ADD 1 TO RQ-LE
        END-PERFORM
        MOVE 0 TO RQ-SP1 RQ-SP2
        PERFORM VARYING W-I FROM RQ-P BY 1 UNTIL W-I >= RQ-LE
            IF LK-IN(W-I:1) = " "
                IF RQ-SP1 = 0
                    MOVE W-I TO RQ-SP1
                ELSE
                    MOVE W-I TO RQ-SP2
                    EXIT PERFORM
                END-IF
            END-IF
        END-PERFORM
        IF RQ-SP2 = 0
            MOVE -1 TO USED
            EXIT PERFORM
        END-IF
        SUBTRACT RQ-P FROM RQ-SP1 GIVING M-L
        ADD 1 TO RQ-SP1 GIVING PATH-S
        SUBTRACT PATH-S FROM RQ-SP2 GIVING PATH-L
        MOVE 0 TO CLOSE-F EXPECT-F CLEN AUTH-S AUTH-L
        SUBTRACT RQ-SP2 FROM RQ-LE GIVING W-N
        IF W-N = 9
            ADD 1 TO RQ-SP2 GIVING W-K
            IF LK-IN(W-K:8) = "HTTP/1.0" MOVE 1 TO CLOSE-F END-IF
        END-IF
        *> headers
        ADD 2 TO RQ-LE GIVING H-P
        ADD 2 TO RQ-HE GIVING W-LIMIT
        PERFORM UNTIL H-P >= W-LIMIT
            MOVE H-P TO H-L
            PERFORM UNTIL LK-IN(H-L:1) = X"0D"
                ADD 1 TO H-L
            END-PERFORM
            MOVE 0 TO H-C
            PERFORM VARYING W-I FROM H-P BY 1 UNTIL W-I >= H-L
                IF LK-IN(W-I:1) = ":"
                    MOVE W-I TO H-C
                    EXIT PERFORM
                END-IF
            END-PERFORM
            IF H-C > 0
                PERFORM ONE-HEADER
                IF USED < 0 EXIT PERFORM END-IF
            END-IF
            ADD 2 TO H-L GIVING H-P
        END-PERFORM
        IF USED < 0 EXIT PERFORM END-IF
        ADD 4 TO RQ-HE GIVING BODY-S
        SUBTRACT BODY-S FROM PN GIVING W-N
        ADD 1 TO W-N
        IF W-N < CLEN
            IF EXPECT-F = 1 AND W-N = 0
                CALL STATIC "write" USING BY VALUE SOCK-FD BY REFERENCE K-CONT BY VALUE 25
            END-IF
            EXIT PERFORM
        END-IF
        PERFORM ROUTE-REQUEST
        IF CLOSE-F = 1 MOVE 1 TO CLOSING-F END-IF
        ADD BODY-S CLEN GIVING USED
        SUBTRACT 1 FROM USED
    END-PERFORM.

*> header name LK-IN(H-P : H-C - H-P), value trimmed of spaces and tabs
ONE-HEADER.
    SUBTRACT H-P FROM H-C GIVING H-KN
    IF H-KN NOT = 14 AND H-KN NOT = 13 AND H-KN NOT = 10 AND H-KN NOT = 6 AND H-KN NOT = 17
        EXIT PARAGRAPH
    END-IF
    ADD 1 TO H-C GIVING H-V
    PERFORM UNTIL H-V >= H-L OR (LK-IN(H-V:1) NOT = " " AND LK-IN(H-V:1) NOT = X"09")
        ADD 1 TO H-V
    END-PERFORM
    MOVE H-L TO H-VE
    PERFORM UNTIL H-VE <= H-V
        SUBTRACT 1 FROM H-VE GIVING W-K
        IF LK-IN(W-K:1) NOT = " " AND LK-IN(W-K:1) NOT = X"09" EXIT PERFORM END-IF
        MOVE W-K TO H-VE
    END-PERFORM
    MOVE LK-IN(H-P:H-KN) TO HN-BUF
    INSPECT HN-BUF CONVERTING "ABCDEFGHIJKLMNOPQRSTUVWXYZ" TO "abcdefghijklmnopqrstuvwxyz"
    SUBTRACT H-V FROM H-VE GIVING W-N
    EVALUATE TRUE
        WHEN H-KN = 14 AND HN-BUF(1:14) = "content-length"
            MOVE 0 TO CLEN
            PERFORM VARYING W-I FROM H-V BY 1 UNTIL W-I >= H-VE
                MOVE LK-IN(W-I:1) TO BYTE-X(1:1)
                IF BYTE-W < 48 OR BYTE-W > 57
                    MOVE -1 TO USED
                    EXIT PARAGRAPH
                END-IF
                COMPUTE CLEN = CLEN * 10 + BYTE-W - 48
                IF CLEN > 65536
                    MOVE -1 TO USED
                    EXIT PARAGRAPH
                END-IF
            END-PERFORM
        WHEN H-KN = 13 AND HN-BUF(1:13) = "authorization"
            MOVE H-V TO AUTH-S
            MOVE W-N TO AUTH-L
        WHEN H-KN = 10 AND HN-BUF(1:10) = "connection"
            IF W-N = 5
                IF FUNCTION LOWER-CASE(LK-IN(H-V:5)) = "close" MOVE 1 TO CLOSE-F END-IF
            END-IF
            IF W-N = 10
                IF FUNCTION LOWER-CASE(LK-IN(H-V:10)) = "keep-alive" MOVE 0 TO CLOSE-F END-IF
            END-IF
        WHEN H-KN = 6 AND HN-BUF(1:6) = "expect"
            IF W-N = 12
                IF FUNCTION LOWER-CASE(LK-IN(H-V:12)) = "100-continue" MOVE 1 TO EXPECT-F END-IF
            END-IF
        WHEN H-KN = 17 AND HN-BUF(1:17) = "transfer-encoding"
            *> chunked uploads aren't supported
            MOVE -1 TO USED
    END-EVALUATE.

*> ================= routing =================
ROUTE-REQUEST.
    *> drop a query string
    ADD PATH-S PATH-L GIVING W-LIMIT
    PERFORM VARYING W-I FROM PATH-S BY 1 UNTIL W-I >= W-LIMIT
        IF LK-IN(W-I:1) = "?"
            SUBTRACT PATH-S FROM W-I GIVING PATH-L
            EXIT PERFORM
        END-IF
    END-PERFORM
    MOVE 0 TO IS-GET IS-POST
    IF M-L = 3
        IF LK-IN(RQ-P:3) = "GET" MOVE 1 TO IS-GET END-IF
    END-IF
    IF M-L = 4
        IF LK-IN(RQ-P:4) = "POST" MOVE 1 TO IS-POST END-IF
    END-IF
    IF IS-GET = 1 AND PATH-L = 5
        IF LK-IN(PATH-S:5) = "/feed"
            PERFORM ROUTE-FEED
            EXIT PARAGRAPH
        END-IF
    END-IF
    IF PATH-L > 7
        IF LK-IN(PATH-S:7) = "/posts/"
            ADD 7 TO PATH-S GIVING SEG-S
            SUBTRACT 7 FROM PATH-L GIVING SEG-L
            MOVE 0 TO SEG-SL
            ADD PATH-S PATH-L GIVING W-LIMIT
            PERFORM VARYING W-I FROM SEG-S BY 1 UNTIL W-I >= W-LIMIT
                IF LK-IN(W-I:1) = "/"
                    MOVE W-I TO SEG-SL
                    EXIT PERFORM
                END-IF
            END-PERFORM
            IF IS-GET = 1 AND SEG-SL = 0
                PERFORM PARSE-ID
                PERFORM ROUTE-POST
                EXIT PARAGRAPH
            END-IF
            IF IS-POST = 1 AND SEG-SL > 0
                SUBTRACT SEG-SL FROM W-LIMIT GIVING W-N
                IF W-N = 5
                    IF LK-IN(SEG-SL:5) = "/like"
                        SUBTRACT SEG-S FROM SEG-SL GIVING SEG-L
                        PERFORM ROUTE-LIKE
                        EXIT PARAGRAPH
                    END-IF
                END-IF
            END-IF
        END-IF
    END-IF
    IF IS-POST = 1 AND PATH-L = 6
        IF LK-IN(PATH-S:6) = "/posts"
            PERFORM ROUTE-CREATE
            EXIT PARAGRAPH
        END-IF
    END-IF
    IF IS-GET = 1 AND PATH-L = 7
        IF LK-IN(PATH-S:7) = "/health"
            PERFORM ROUTE-HEALTH
            EXIT PARAGRAPH
        END-IF
    END-IF
    MOVE 404 TO STATUS-CODE
    MOVE E-NOTFOUND TO ERR-BODY
    MOVE LENGTH OF E-NOTFOUND TO ERR-LEN
    PERFORM ERR-RESP.

*> LK-IN(SEG-S:SEG-L) as a positive decimal id; ID-OK = 0 if it isn't one (huge ids just aren't found)
PARSE-ID.
    MOVE 0 TO ID-OK
    IF SEG-L = 0 EXIT PARAGRAPH END-IF
    ADD SEG-S SEG-L GIVING W-LIMIT
    PERFORM VARYING W-I FROM SEG-S BY 1 UNTIL W-I >= W-LIMIT
        MOVE LK-IN(W-I:1) TO BYTE-X(1:1)
        IF BYTE-W < 48 OR BYTE-W > 57 EXIT PARAGRAPH END-IF
    END-PERFORM
    IF SEG-L > 18
        MOVE 999999999999999999 TO POST-ID
    ELSE
        MOVE ALL "0" TO NUM-X18
        SUBTRACT SEG-L FROM 19 GIVING W-K
        MOVE LK-IN(SEG-S:SEG-L) TO NUM-X18(W-K:SEG-L)
        MOVE NUM-D18 TO POST-ID
    END-IF
    IF POST-ID > 0 MOVE 1 TO ID-OK END-IF.

ROUTE-FEED.
    PERFORM TXN-BEGIN
    IF IN-TXN = 0 PERFORM ERR-500 EXIT PARAGRAPH END-IF
    MOVE ALEN TO MARK
    MOVE 200 TO STATUS-CODE
    PERFORM BEGIN-RESP
    MOVE K-FEED1 TO ARENA(ALEN + 1:LENGTH OF K-FEED1)
    ADD LENGTH OF K-FEED1 TO ALEN
    MOVE 0 TO ROWS
    PERFORM FOREVER
        CALL STATIC "sqlite3_step" USING BY VALUE ST-FEED RETURNING RC
        IF RC NOT = 100 EXIT PERFORM END-IF
        IF ROWS > 0
            MOVE "," TO ARENA(ALEN + 1:1)
            ADD 1 TO ALEN
        END-IF
        ADD 1 TO ROWS
        SET TMP-P2 TO ST-FEED
        PERFORM PUT-POST-ROW
    END-PERFORM
    CALL STATIC "sqlite3_reset" USING BY VALUE ST-FEED
    IF RC NOT = 101
        PERFORM DB-FAIL
        MOVE MARK TO ALEN
        PERFORM ERR-500
        EXIT PARAGRAPH
    END-IF
    MOVE K-FEED2 TO ARENA(ALEN + 1:2)
    ADD 2 TO ALEN
    PERFORM FINISH-RESP.

ROUTE-POST.
    IF ID-OK = 0
        PERFORM BAD-ID
        EXIT PARAGRAPH
    END-IF
    PERFORM TXN-BEGIN
    IF IN-TXN = 0 PERFORM ERR-500 EXIT PARAGRAPH END-IF
    CALL STATIC "sqlite3_bind_int64" USING BY VALUE ST-POST BY VALUE 1 BY VALUE POST-ID
    CALL STATIC "sqlite3_step" USING BY VALUE ST-POST RETURNING RC
    EVALUATE RC
        WHEN 100
            MOVE 200 TO STATUS-CODE
            PERFORM BEGIN-RESP
            MOVE K-POST1 TO ARENA(ALEN + 1:LENGTH OF K-POST1)
            ADD LENGTH OF K-POST1 TO ALEN
            SET TMP-P2 TO ST-POST
            PERFORM PUT-POST-ROW
            MOVE "}" TO ARENA(ALEN + 1:1)
            ADD 1 TO ALEN
            PERFORM FINISH-RESP
        WHEN 101
            PERFORM NO-POST
        WHEN OTHER
            PERFORM DB-FAIL
            PERFORM ERR-500
    END-EVALUATE
    CALL STATIC "sqlite3_reset" USING BY VALUE ST-POST.

ROUTE-LIKE.
    PERFORM AUTH
    IF AUTH-ERR NOT = 0 PERFORM AUTH-RESP EXIT PARAGRAPH END-IF
    PERFORM PARSE-ID
    IF ID-OK = 0
        PERFORM BAD-ID
        EXIT PARAGRAPH
    END-IF
    PERFORM TXN-BEGIN
    IF IN-TXN = 0 PERFORM ERR-500 EXIT PARAGRAPH END-IF
    CALL STATIC "sqlite3_bind_int64" USING BY VALUE ST-LIKE BY VALUE 1 BY VALUE USER-ID
    CALL STATIC "sqlite3_bind_int64" USING BY VALUE ST-LIKE BY VALUE 2 BY VALUE POST-ID
    SET WS-STMT TO ST-LIKE
    PERFORM WSTEP
    CALL STATIC "sqlite3_reset" USING BY VALUE ST-LIKE
    IF RC NOT = 101
        PERFORM DB-FAIL
        PERFORM ERR-500
        EXIT PARAGRAPH
    END-IF
    CALL STATIC "sqlite3_changes" USING BY VALUE DB RETURNING CREATED
    IF CREATED = 0
        CALL STATIC "sqlite3_bind_int64" USING BY VALUE ST-EXISTS BY VALUE 1 BY VALUE POST-ID
        CALL STATIC "sqlite3_step" USING BY VALUE ST-EXISTS RETURNING RC
        CALL STATIC "sqlite3_reset" USING BY VALUE ST-EXISTS
        IF RC = 101
            PERFORM NO-POST
            EXIT PARAGRAPH
        END-IF
        IF RC NOT = 100
            PERFORM DB-FAIL
            PERFORM ERR-500
            EXIT PARAGRAPH
        END-IF
        MOVE 200 TO STATUS-CODE
    ELSE
        MOVE 201 TO STATUS-CODE
    END-IF
    PERFORM BEGIN-RESP
    MOVE K-LIKED TO ARENA(ALEN + 1:LENGTH OF K-LIKED)
    ADD LENGTH OF K-LIKED TO ALEN
    IF CREATED = 0
        MOVE "true" TO ARENA(ALEN + 1:4)
        ADD 4 TO ALEN
    ELSE
        MOVE "false" TO ARENA(ALEN + 1:5)
        ADD 5 TO ALEN
    END-IF
    MOVE K-PID TO ARENA(ALEN + 1:LENGTH OF K-PID)
    ADD LENGTH OF K-PID TO ALEN
    MOVE POST-ID TO PUT-N
    PERFORM PUT-NUM
    MOVE "}" TO ARENA(ALEN + 1:1)
    ADD 1 TO ALEN
    PERFORM FINISH-RESP.

ROUTE-CREATE.
    PERFORM AUTH
    IF AUTH-ERR NOT = 0 PERFORM AUTH-RESP EXIT PARAGRAPH END-IF
    SET ADDRESS OF LK-J TO ADDRESS OF LK-IN
    MOVE BODY-S TO J-P
    ADD BODY-S CLEN GIVING J-E
    MOVE 1 TO WN
    MOVE "body" TO WK(1)
    MOVE 4 TO WKL(1)
    PERFORM JSON-DOC
    IF J-OK = 0
        PERFORM MALFORMED
        EXIT PARAGRAPH
    END-IF
    IF WVS(1) = 0
        PERFORM BODY-REQUIRED
        EXIT PARAGRAPH
    END-IF
    IF LK-J(WVS(1):1) NOT = '"'
        PERFORM BODY-REQUIRED
        EXIT PARAGRAPH
    END-IF
    MOVE WVS(1) TO J-P
    MOVE 1 TO JS-MODE
    MOVE 65536 TO JS-CAP
    PERFORM JSON-STR
    IF JS-RC NOT = 0
        PERFORM MALFORMED
        EXIT PARAGRAPH
    END-IF
    *> trim ASCII whitespace
    MOVE 1 TO TR-S
    ADD 1 TO JS-LEN GIVING TR-E
    PERFORM UNTIL TR-S >= TR-E
        MOVE JS-BUF(TR-S:1) TO BYTE-X(1:1)
        IF BYTE-W NOT = 32 AND (BYTE-W < 9 OR BYTE-W > 13) EXIT PERFORM END-IF
        ADD 1 TO TR-S
    END-PERFORM
    PERFORM UNTIL TR-E <= TR-S
        SUBTRACT 1 FROM TR-E GIVING W-K
        MOVE JS-BUF(W-K:1) TO BYTE-X(1:1)
        IF BYTE-W NOT = 32 AND (BYTE-W < 9 OR BYTE-W > 13) EXIT PERFORM END-IF
        MOVE W-K TO TR-E
    END-PERFORM
    IF TR-S >= TR-E
        PERFORM BODY-REQUIRED
        EXIT PARAGRAPH
    END-IF
    *> length in characters (UTF-8 code points)
    MOVE 0 TO NCHARS
    PERFORM VARYING W-I FROM TR-S BY 1 UNTIL W-I >= TR-E
        MOVE JS-BUF(W-I:1) TO BYTE-X(1:1)
        IF BYTE-W < 128 OR BYTE-W > 191 ADD 1 TO NCHARS END-IF
    END-PERFORM
    IF NCHARS > 500
        MOVE 400 TO STATUS-CODE
        MOVE E-TOOLONG TO ERR-BODY
        MOVE LENGTH OF E-TOOLONG TO ERR-LEN
        PERFORM ERR-RESP
        EXIT PARAGRAPH
    END-IF
    PERFORM TXN-BEGIN
    IF IN-TXN = 0 PERFORM ERR-500 EXIT PARAGRAPH END-IF
    SUBTRACT TR-S FROM TR-E GIVING W-L
    CALL STATIC "sqlite3_bind_int64" USING BY VALUE ST-INS BY VALUE 1 BY VALUE USER-ID
    CALL STATIC "sqlite3_bind_text" USING BY VALUE ST-INS BY VALUE 2 BY REFERENCE JS-BUF(TR-S:W-L)
        BY VALUE W-L BY VALUE NUL-P
    SET WS-STMT TO ST-INS
    PERFORM WSTEP
    IF RC NOT = 100
        CALL STATIC "sqlite3_reset" USING BY VALUE ST-INS
        PERFORM DB-FAIL
        PERFORM ERR-500
        EXIT PARAGRAPH
    END-IF
    MOVE ALEN TO MARK
    MOVE 201 TO STATUS-CODE
    PERFORM BEGIN-RESP
    MOVE K-POST1 TO ARENA(ALEN + 1:LENGTH OF K-POST1)
    ADD LENGTH OF K-POST1 TO ALEN
    MOVE K-ID TO ARENA(ALEN + 1:LENGTH OF K-ID)
    ADD LENGTH OF K-ID TO ALEN
    SET TMP-P2 TO ST-INS
    MOVE 0 TO COL-I
    PERFORM PUT-COL
    MOVE K-BODY TO ARENA(ALEN + 1:LENGTH OF K-BODY)
    ADD LENGTH OF K-BODY TO ALEN
    SET TMP-P TO ADDRESS OF JS-BUF
    SUBTRACT 1 FROM TR-S GIVING W-K
    SET TMP-P UP BY W-K
    SET ADDRESS OF LK-ESC TO TMP-P
    MOVE W-L TO ESC-L
    PERFORM PUT-ESC
    MOVE K-CREATED TO ARENA(ALEN + 1:LENGTH OF K-CREATED)
    ADD LENGTH OF K-CREATED TO ALEN
    MOVE 1 TO COL-I
    PERFORM PUT-COL
    MOVE K-AUTHOR TO ARENA(ALEN + 1:LENGTH OF K-AUTHOR)
    ADD LENGTH OF K-AUTHOR TO ALEN
    SET ADDRESS OF LK-ESC TO ADDRESS OF UNAME
    MOVE UNAME-L TO ESC-L
    PERFORM PUT-ESC
    MOVE K-ZERO-END TO ARENA(ALEN + 1:LENGTH OF K-ZERO-END)
    ADD LENGTH OF K-ZERO-END TO ALEN
    PERFORM FINISH-RESP
    *> run RETURNING to completion so the insert finishes
    CALL STATIC "sqlite3_step" USING BY VALUE ST-INS RETURNING RC
    CALL STATIC "sqlite3_reset" USING BY VALUE ST-INS
    IF RC NOT = 101
        PERFORM DB-FAIL
        MOVE MARK TO ALEN
        PERFORM ERR-500
    END-IF.

BAD-ID.
    MOVE 400 TO STATUS-CODE
    MOVE E-BADID TO ERR-BODY
    MOVE LENGTH OF E-BADID TO ERR-LEN
    PERFORM ERR-RESP.

NO-POST.
    MOVE 404 TO STATUS-CODE
    MOVE E-NOPOST TO ERR-BODY
    MOVE LENGTH OF E-NOPOST TO ERR-LEN
    PERFORM ERR-RESP.

MALFORMED.
    MOVE 400 TO STATUS-CODE
    MOVE E-MALFORMED TO ERR-BODY
    MOVE LENGTH OF E-MALFORMED TO ERR-LEN
    PERFORM ERR-RESP.

BODY-REQUIRED.
    MOVE 400 TO STATUS-CODE
    MOVE E-REQUIRED TO ERR-BODY
    MOVE LENGTH OF E-REQUIRED TO ERR-LEN
    PERFORM ERR-RESP.

ROUTE-HEALTH.
    CALL STATIC "sqlite3_step" USING BY VALUE ST-ONE RETURNING RC
    CALL STATIC "sqlite3_reset" USING BY VALUE ST-ONE
    IF RC = 100
        MOVE 200 TO STATUS-CODE
        PERFORM BEGIN-RESP
        MOVE K-HEALTH TO ARENA(ALEN + 1:LENGTH OF K-HEALTH)
        ADD LENGTH OF K-HEALTH TO ALEN
        SUBTRACT STARTED FROM NOW-S GIVING UPTIME
        MOVE UPTIME TO PUT-N
        PERFORM PUT-NUM
        MOVE "}" TO ARENA(ALEN + 1:1)
        ADD 1 TO ALEN
    ELSE
        MOVE 503 TO STATUS-CODE
        PERFORM BEGIN-RESP
        MOVE K-DEGRADED TO ARENA(ALEN + 1:LENGTH OF K-DEGRADED)
        ADD LENGTH OF K-DEGRADED TO ALEN
        CALL STATIC "sqlite3_errmsg" USING BY VALUE DB RETURNING COL-P
        CALL STATIC "strlen" USING BY VALUE COL-P RETURNING ESC-L
        SET ADDRESS OF LK-ESC TO COL-P
        PERFORM PUT-ESC
        MOVE '"}' TO ARENA(ALEN + 1:2)
        ADD 2 TO ALEN
    END-IF
    PERFORM FINISH-RESP.

*> ================= responses =================
BEGIN-RESP.
    EVALUATE STATUS-CODE
        WHEN 200
            MOVE K-200 TO ARENA(ALEN + 1:LENGTH OF K-200)
            ADD LENGTH OF K-200 TO ALEN
        WHEN 201
            MOVE K-201 TO ARENA(ALEN + 1:LENGTH OF K-201)
            ADD LENGTH OF K-201 TO ALEN
        WHEN 400
            MOVE K-400 TO ARENA(ALEN + 1:LENGTH OF K-400)
            ADD LENGTH OF K-400 TO ALEN
        WHEN 401
            MOVE K-401 TO ARENA(ALEN + 1:LENGTH OF K-401)
            ADD LENGTH OF K-401 TO ALEN
        WHEN 404
            MOVE K-404 TO ARENA(ALEN + 1:LENGTH OF K-404)
            ADD LENGTH OF K-404 TO ALEN
        WHEN 503
            MOVE K-503 TO ARENA(ALEN + 1:LENGTH OF K-503)
            ADD LENGTH OF K-503 TO ALEN
        WHEN OTHER
            MOVE K-500 TO ARENA(ALEN + 1:LENGTH OF K-500)
            ADD LENGTH OF K-500 TO ALEN
    END-EVALUATE
    IF CLOSE-F = 1
        MOVE K-CLOSE TO ARENA(ALEN + 1:LENGTH OF K-CLOSE)
        ADD LENGTH OF K-CLOSE TO ALEN
    END-IF
    MOVE K-HEAD TO ARENA(ALEN + 1:LENGTH OF K-HEAD)
    ADD LENGTH OF K-HEAD TO ALEN
    SUBTRACT 10 FROM ALEN GIVING CL-POS
    MOVE ALEN TO BODY-START.

*> fills the blank Content-Length left by BEGIN-RESP
FINISH-RESP.
    SUBTRACT BODY-START FROM ALEN GIVING W-N
    MOVE W-N TO CL-ED
    MOVE CL-ED TO ARENA(CL-POS:7).

ERR-RESP.
    PERFORM BEGIN-RESP
    MOVE ERR-BODY(1:ERR-LEN) TO ARENA(ALEN + 1:ERR-LEN)
    ADD ERR-LEN TO ALEN
    PERFORM FINISH-RESP.

ERR-500.
    MOVE 500 TO STATUS-CODE
    MOVE E-INTERNAL TO ERR-BODY
    MOVE LENGTH OF E-INTERNAL TO ERR-LEN
    PERFORM ERR-RESP.

AUTH-RESP.
    MOVE 401 TO STATUS-CODE
    EVALUATE AUTH-ERR
        WHEN 1
            MOVE E-MISSING TO ERR-BODY
            MOVE LENGTH OF E-MISSING TO ERR-LEN
        WHEN 2
            MOVE E-INVALID TO ERR-BODY
            MOVE LENGTH OF E-INVALID TO ERR-LEN
        WHEN OTHER
            MOVE E-PAYLOAD TO ERR-BODY
            MOVE LENGTH OF E-PAYLOAD TO ERR-LEN
    END-EVALUATE
    PERFORM ERR-RESP.

*> one post object from the current row of statement TMP-P2
PUT-POST-ROW.
    MOVE K-ID TO ARENA(ALEN + 1:LENGTH OF K-ID)
    ADD LENGTH OF K-ID TO ALEN
    MOVE 0 TO COL-I
    PERFORM PUT-COL
    MOVE K-BODY TO ARENA(ALEN + 1:LENGTH OF K-BODY)
    ADD LENGTH OF K-BODY TO ALEN
    CALL STATIC "sqlite3_column_text" USING BY VALUE TMP-P2 BY VALUE 1 RETURNING COL-P
    CALL STATIC "sqlite3_column_bytes" USING BY VALUE TMP-P2 BY VALUE 1 RETURNING ESC-L
    SET ADDRESS OF LK-ESC TO COL-P
    PERFORM PUT-ESC
    MOVE K-CREATED TO ARENA(ALEN + 1:LENGTH OF K-CREATED)
    ADD LENGTH OF K-CREATED TO ALEN
    MOVE 2 TO COL-I
    PERFORM PUT-COL
    MOVE K-AUTHOR TO ARENA(ALEN + 1:LENGTH OF K-AUTHOR)
    ADD LENGTH OF K-AUTHOR TO ALEN
    CALL STATIC "sqlite3_column_text" USING BY VALUE TMP-P2 BY VALUE 3 RETURNING COL-P
    CALL STATIC "sqlite3_column_bytes" USING BY VALUE TMP-P2 BY VALUE 3 RETURNING ESC-L
    SET ADDRESS OF LK-ESC TO COL-P
    PERFORM PUT-ESC
    MOVE K-LIKES TO ARENA(ALEN + 1:LENGTH OF K-LIKES)
    ADD LENGTH OF K-LIKES TO ALEN
    MOVE 4 TO COL-I
    PERFORM PUT-COL
    MOVE "}" TO ARENA(ALEN + 1:1)
    ADD 1 TO ALEN.

*> appends column COL-I of statement TMP-P2 as SQLite's text form (an integer column becomes its
*> decimal digits, so no C function has to return a 64-bit value to COBOL)
PUT-COL.
    CALL STATIC "sqlite3_column_text" USING BY VALUE TMP-P2 BY VALUE COL-I RETURNING COL-P
    CALL STATIC "sqlite3_column_bytes" USING BY VALUE TMP-P2 BY VALUE COL-I RETURNING W-N
    SET ADDRESS OF LK-COL TO COL-P
    IF W-N > 0
        MOVE LK-COL(1:W-N) TO ARENA(ALEN + 1:W-N)
        ADD W-N TO ALEN
    END-IF.

*> appends PUT-N in decimal
PUT-NUM.
    MOVE PUT-N TO NUM-D18
    MOVE 1 TO W-K
    PERFORM UNTIL W-K >= 18 OR NUM-X18(W-K:1) NOT = "0"
        ADD 1 TO W-K
    END-PERFORM
    SUBTRACT W-K FROM 19 GIVING W-N
    MOVE NUM-X18(W-K:W-N) TO ARENA(ALEN + 1:W-N)
    ADD W-N TO ALEN.

*> appends LK-ESC(1:ESC-L) as JSON string contents
PUT-ESC.
    MOVE 1 TO ESC-RUN
    PERFORM VARYING ESC-I FROM 1 BY 1 UNTIL ESC-I > ESC-L
        MOVE LK-ESC(ESC-I:1) TO BYTE-X(1:1)
        IF BYTE-W < 32 OR BYTE-W = 34 OR BYTE-W = 92
            SUBTRACT ESC-RUN FROM ESC-I GIVING W-N
            IF W-N > 0
                MOVE LK-ESC(ESC-RUN:W-N) TO ARENA(ALEN + 1:W-N)
                ADD W-N TO ALEN
            END-IF
            ADD 1 TO ESC-I GIVING ESC-RUN
            EVALUATE BYTE-W
                WHEN 34 MOVE '\"' TO ARENA(ALEN + 1:2)
                WHEN 92 MOVE '\\' TO ARENA(ALEN + 1:2)
                WHEN 10 MOVE '\n' TO ARENA(ALEN + 1:2)
                WHEN 13 MOVE '\r' TO ARENA(ALEN + 1:2)
                WHEN 9  MOVE '\t' TO ARENA(ALEN + 1:2)
                WHEN OTHER
                    MOVE '\u00' TO ARENA(ALEN + 1:4)
                    DIVIDE BYTE-W BY 16 GIVING W-N REMAINDER W-K
                    ADD 1 TO W-N
                    ADD 1 TO W-K
                    MOVE HEX-ALPHA(W-N:1) TO ARENA(ALEN + 5:1)
                    MOVE HEX-ALPHA(W-K:1) TO ARENA(ALEN + 6:1)
                    ADD 4 TO ALEN
            END-EVALUATE
            ADD 2 TO ALEN
        END-IF
    END-PERFORM
    SUBTRACT ESC-RUN FROM ESC-I GIVING W-N
    IF W-N > 0
        MOVE LK-ESC(ESC-RUN:W-N) TO ARENA(ALEN + 1:W-N)
        ADD W-N TO ALEN
    END-IF.

*> ================= auth =================
*> Verifies the Authorization header LK-IN(AUTH-S:AUTH-L). AUTH-ERR: 0 ok, 1 missing bearer token,
*> 2 invalid or expired token, 3 invalid token payload. On success USER-ID and UNAME are set.
AUTH.
    MOVE 2 TO AUTH-ERR
    IF AUTH-L < 7
        MOVE 1 TO AUTH-ERR
        EXIT PARAGRAPH
    END-IF
    IF LK-IN(AUTH-S:7) NOT = "Bearer "
        MOVE 1 TO AUTH-ERR
        EXIT PARAGRAPH
    END-IF
    ADD 7 TO AUTH-S GIVING T-S
    ADD AUTH-S AUTH-L GIVING T-E
    MOVE 0 TO T-D1 T-D2 T-D3
    PERFORM VARYING W-I FROM T-S BY 1 UNTIL W-I >= T-E
        IF LK-IN(W-I:1) = "."
            EVALUATE TRUE
                WHEN T-D1 = 0 MOVE W-I TO T-D1
                WHEN T-D2 = 0 MOVE W-I TO T-D2
                WHEN OTHER MOVE W-I TO T-D3
            END-EVALUATE
        END-IF
    END-PERFORM
    IF T-D2 = 0 OR T-D3 NOT = 0 EXIT PARAGRAPH END-IF
    *> signature: 43 base64url chars = 32 bytes
    SUBTRACT T-D2 FROM T-E GIVING W-N
    IF W-N NOT = 44 EXIT PARAGRAPH END-IF
    ADD 1 TO T-D2 GIVING B64-SRC
    MOVE 43 TO B64-SL
    PERFORM B64-DEC
    IF B64-RC NOT = 0 OR B64-N NOT = 32 EXIT PARAGRAPH END-IF
    MOVE B64-OUT(1:32) TO SIG
    *> HMAC-SHA256 over header.payload
    SUBTRACT T-S FROM T-D2 GIVING W-L
    MOVE CTX-IN TO CTX-W
    CALL STATIC "SHA256_Update" USING BY REFERENCE CTX-W BY REFERENCE LK-IN(T-S:W-L) BY VALUE W-L
    CALL STATIC "SHA256_Final" USING BY REFERENCE INNER BY REFERENCE CTX-W
    MOVE CTX-OUT TO CTX-W
    CALL STATIC "SHA256_Update" USING BY REFERENCE CTX-W BY REFERENCE INNER BY VALUE 32
    CALL STATIC "SHA256_Final" USING BY REFERENCE MAC BY REFERENCE CTX-W
    MOVE 0 TO DIFF
    PERFORM VARYING W-I FROM 1 BY 1 UNTIL W-I > 32
        IF MAC(W-I:1) NOT = SIG(W-I:1) ADD 1 TO DIFF END-IF
    END-PERFORM
    IF DIFF NOT = 0 EXIT PARAGRAPH END-IF
    *> header: {"alg":"HS256",...}
    MOVE T-S TO B64-SRC
    SUBTRACT T-S FROM T-D1 GIVING B64-SL
    IF B64-SL > 680 EXIT PARAGRAPH END-IF
    PERFORM B64-DEC
    IF B64-RC NOT = 0 EXIT PARAGRAPH END-IF
    SET ADDRESS OF LK-J TO ADDRESS OF B64-OUT
    MOVE 1 TO J-P
    ADD 1 TO B64-N GIVING J-E
    MOVE 1 TO WN
    MOVE "alg" TO WK(1)
    MOVE 3 TO WKL(1)
    PERFORM JSON-DOC
    IF J-OK = 0 OR WVS(1) = 0 EXIT PARAGRAPH END-IF
    IF LK-J(WVS(1):1) NOT = '"' EXIT PARAGRAPH END-IF
    MOVE WVS(1) TO J-P
    MOVE 1 TO JS-MODE
    MOVE 8 TO JS-CAP
    PERFORM JSON-STR
    IF JS-RC NOT = 0 OR JS-LEN NOT = 5 EXIT PARAGRAPH END-IF
    IF JS-BUF(1:5) NOT = "HS256" EXIT PARAGRAPH END-IF
    *> payload
    ADD 1 TO T-D1 GIVING B64-SRC
    SUBTRACT B64-SRC FROM T-D2 GIVING B64-SL
    IF B64-SL > 2700 EXIT PARAGRAPH END-IF
    PERFORM B64-DEC
    IF B64-RC NOT = 0 EXIT PARAGRAPH END-IF
    MOVE 1 TO J-P
    ADD 1 TO B64-N GIVING J-E
    MOVE 4 TO WN
    MOVE "exp" TO WK(1)
    MOVE 3 TO WKL(1)
    MOVE "nbf" TO WK(2)
    MOVE 3 TO WKL(2)
    MOVE "sub" TO WK(3)
    MOVE 3 TO WKL(3)
    MOVE "username" TO WK(4)
    MOVE 8 TO WKL(4)
    PERFORM JSON-DOC
    *> exp is required
    IF J-OK = 0 OR WVS(1) = 0 EXIT PARAGRAPH END-IF
    MOVE NOW-S TO NOW-D
    *> exp / nbf must be numbers, checked against now
    IF WVS(1) > 0
        MOVE LK-J(WVS(1):1) TO BYTE-X(1:1)
        IF BYTE-W NOT = 45 AND (BYTE-W < 48 OR BYTE-W > 57) EXIT PARAGRAPH END-IF
        SUBTRACT WVS(1) FROM WVE(1) GIVING W-N
        COMPUTE EXP-D = FUNCTION NUMVAL-F(LK-J(WVS(1):W-N))
        IF NOW-D >= EXP-D EXIT PARAGRAPH END-IF
    END-IF
    IF WVS(2) > 0
        MOVE LK-J(WVS(2):1) TO BYTE-X(1:1)
        IF BYTE-W NOT = 45 AND (BYTE-W < 48 OR BYTE-W > 57) EXIT PARAGRAPH END-IF
        SUBTRACT WVS(2) FROM WVE(2) GIVING W-N
        COMPUTE EXP-D = FUNCTION NUMVAL-F(LK-J(WVS(2):W-N))
        IF NOW-D < EXP-D EXIT PARAGRAPH END-IF
    END-IF
    MOVE 3 TO AUTH-ERR
    IF WVS(3) = 0 OR WVS(4) = 0 EXIT PARAGRAPH END-IF
    IF LK-J(WVS(3):1) NOT = '"' OR LK-J(WVS(4):1) NOT = '"' EXIT PARAGRAPH END-IF
    *> sub: a positive integer string
    MOVE WVS(3) TO J-P
    MOVE 1 TO JS-MODE
    MOVE 24 TO JS-CAP
    PERFORM JSON-STR
    IF JS-RC NOT = 0 OR JS-LEN < 1 OR JS-LEN > 15 EXIT PARAGRAPH END-IF
    PERFORM VARYING W-I FROM 1 BY 1 UNTIL W-I > JS-LEN
        MOVE JS-BUF(W-I:1) TO BYTE-X(1:1)
        IF BYTE-W < 48 OR BYTE-W > 57 EXIT PARAGRAPH END-IF
    END-PERFORM
    MOVE ALL "0" TO NUM-X18
    SUBTRACT JS-LEN FROM 19 GIVING W-K
    MOVE JS-BUF(1:JS-LEN) TO NUM-X18(W-K:JS-LEN)
    MOVE NUM-D18 TO USER-ID
    IF USER-ID <= 0 EXIT PARAGRAPH END-IF
    *> username: any string
    MOVE WVS(4) TO J-P
    MOVE 1 TO JS-MODE
    MOVE 1024 TO JS-CAP
    PERFORM JSON-STR
    IF JS-RC NOT = 0 EXIT PARAGRAPH END-IF
    MOVE JS-LEN TO UNAME-L
    IF JS-LEN > 0 MOVE JS-BUF(1:JS-LEN) TO UNAME(1:JS-LEN) END-IF
    MOVE 0 TO AUTH-ERR.

*> base64url LK-IN(B64-SRC:B64-SL) -> B64-OUT(1:B64-N); B64-RC -1 on bad input
B64-DEC.
    MOVE 0 TO B64-N B64-RC
    DIVIDE B64-SL BY 4 GIVING B64-Q REMAINDER B64-REM
    IF B64-REM = 1
        MOVE -1 TO B64-RC
        EXIT PARAGRAPH
    END-IF
    MOVE B64-SRC TO W-I
    PERFORM B64-Q TIMES
        PERFORM B64-LOOKUP4
        IF B64-RC NOT = 0 EXIT PARAGRAPH END-IF
        ADD T-A4(V1) T-D16(V2) GIVING OUT-W
        ADD 1 TO B64-N
        MOVE OUT-X(1:1) TO B64-OUT(B64-N:1)
        ADD T-M16(V2) T-D4(V3) GIVING OUT-W
        ADD 1 TO B64-N
        MOVE OUT-X(1:1) TO B64-OUT(B64-N:1)
        SUBTRACT 1 FROM V4
        ADD T-M64(V3) V4 GIVING OUT-W
        ADD 1 TO B64-N
        MOVE OUT-X(1:1) TO B64-OUT(B64-N:1)
    END-PERFORM
    IF B64-REM >= 2
        MOVE LK-IN(W-I:1) TO BYTE-X(1:1)
        ADD 1 TO BYTE-W GIVING W-K
        ADD 1 TO B64V(W-K) GIVING V1
        ADD 1 TO W-I
        MOVE LK-IN(W-I:1) TO BYTE-X(1:1)
        ADD 1 TO BYTE-W GIVING W-K
        ADD 1 TO B64V(W-K) GIVING V2
        ADD 1 TO W-I
        IF V1 = 0 OR V2 = 0
            MOVE -1 TO B64-RC
            EXIT PARAGRAPH
        END-IF
        ADD T-A4(V1) T-D16(V2) GIVING OUT-W
        ADD 1 TO B64-N
        MOVE OUT-X(1:1) TO B64-OUT(B64-N:1)
        IF B64-REM = 3
            MOVE LK-IN(W-I:1) TO BYTE-X(1:1)
            ADD 1 TO BYTE-W GIVING W-K
            ADD 1 TO B64V(W-K) GIVING V3
            IF V3 = 0
                MOVE -1 TO B64-RC
                EXIT PARAGRAPH
            END-IF
            ADD T-M16(V2) T-D4(V3) GIVING OUT-W
            ADD 1 TO B64-N
            MOVE OUT-X(1:1) TO B64-OUT(B64-N:1)
        END-IF
    END-IF.

*> V1..V4 = 1-based sextet table indexes of the next four chars (V4 kept 1-based too)
B64-LOOKUP4.
    MOVE LK-IN(W-I:1) TO BYTE-X(1:1)
    ADD 1 TO BYTE-W GIVING W-K
    ADD 1 TO B64V(W-K) GIVING V1
    ADD 1 TO W-I
    MOVE LK-IN(W-I:1) TO BYTE-X(1:1)
    ADD 1 TO BYTE-W GIVING W-K
    ADD 1 TO B64V(W-K) GIVING V2
    ADD 1 TO W-I
    MOVE LK-IN(W-I:1) TO BYTE-X(1:1)
    ADD 1 TO BYTE-W GIVING W-K
    ADD 1 TO B64V(W-K) GIVING V3
    ADD 1 TO W-I
    MOVE LK-IN(W-I:1) TO BYTE-X(1:1)
    ADD 1 TO BYTE-W GIVING W-K
    ADD 1 TO B64V(W-K) GIVING V4
    ADD 1 TO W-I
    IF V1 = 0 OR V2 = 0 OR V3 = 0 OR V4 = 0 MOVE -1 TO B64-RC END-IF.

*> ================= JSON (strict, RFC 8259) =================
*> Validates the document LK-J(J-P : J-E - J-P). For a top-level object, WVS/WVE(i) get the span of
*> the last value of key WK(i) (0 if absent; the last duplicate wins, as in JSON.parse). J-OK 0/1.
JSON-DOC.
    MOVE 0 TO J-OK
    PERFORM VARYING W-K FROM 1 BY 1 UNTIL W-K > WN
        MOVE 0 TO WVS(W-K) WVE(W-K)
    END-PERFORM
    PERFORM JSON-WS
    IF J-P < J-E AND LK-J(J-P:1) = "{"
        ADD 1 TO J-P
        PERFORM JSON-WS
        IF J-P < J-E AND LK-J(J-P:1) = "}"
            ADD 1 TO J-P
        ELSE
            PERFORM FOREVER
                PERFORM JSON-WS
                IF J-P >= J-E EXIT PARAGRAPH END-IF
                IF LK-J(J-P:1) NOT = '"' EXIT PARAGRAPH END-IF
                MOVE 2 TO JS-MODE
                MOVE 32 TO JS-CAP
                PERFORM JSON-STR
                IF JS-RC = -1 EXIT PARAGRAPH END-IF
                IF JS-RC = 0
                    MOVE JS-LEN TO KEY-L
                ELSE
                    MOVE -1 TO KEY-L
                END-IF
                PERFORM JSON-WS
                IF J-P >= J-E EXIT PARAGRAPH END-IF
                IF LK-J(J-P:1) NOT = ":" EXIT PARAGRAPH END-IF
                ADD 1 TO J-P
                PERFORM JSON-WS
                MOVE J-P TO VAL-S
                PERFORM JSON-VALUE
                IF J-OK = 0 EXIT PARAGRAPH END-IF
                MOVE 0 TO J-OK
                PERFORM VARYING W-K FROM 1 BY 1 UNTIL W-K > WN
                    IF KEY-L = WKL(W-K)
                        IF KEY-BUF2(1:KEY-L) = WK(W-K)(1:KEY-L)
                            MOVE VAL-S TO WVS(W-K)
                            MOVE J-P TO WVE(W-K)
                        END-IF
                    END-IF
                END-PERFORM
                PERFORM JSON-WS
                IF J-P >= J-E EXIT PARAGRAPH END-IF
                IF LK-J(J-P:1) = ","
                    ADD 1 TO J-P
                ELSE
                    IF LK-J(J-P:1) = "}"
                        ADD 1 TO J-P
                        EXIT PERFORM
                    END-IF
                    EXIT PARAGRAPH
                END-IF
            END-PERFORM
        END-IF
    ELSE
        PERFORM JSON-VALUE
        IF J-OK = 0 EXIT PARAGRAPH END-IF
        MOVE 0 TO J-OK
    END-IF
    PERFORM JSON-WS
    IF J-P = J-E MOVE 1 TO J-OK END-IF.

JSON-WS.
    PERFORM UNTIL J-P >= J-E
        IF LK-J(J-P:1) NOT = " " AND LK-J(J-P:1) NOT = X"09" AND LK-J(J-P:1) NOT = X"0A"
                AND LK-J(J-P:1) NOT = X"0D"
            EXIT PERFORM
        END-IF
        ADD 1 TO J-P
    END-PERFORM.

*> Validates and skips one value at J-P (iterative, explicit stack). J-OK 0/1.
*> States: 1 = expect value, 2 = expect key, 3 = after value.
JSON-VALUE.
    MOVE 0 TO J-OK J-DEPTH
    MOVE 1 TO J-STATE
    PERFORM FOREVER
        PERFORM JSON-WS
        IF J-P >= J-E EXIT PARAGRAPH END-IF
        EVALUATE J-STATE
            WHEN 1
                EVALUATE LK-J(J-P:1)
                    WHEN '"'
                        MOVE 0 TO JS-MODE
                        PERFORM JSON-STR
                        IF JS-RC NOT = 0 EXIT PARAGRAPH END-IF
                        MOVE 3 TO J-STATE
                    WHEN "{"
                        IF J-DEPTH >= 64 EXIT PARAGRAPH END-IF
                        ADD 1 TO J-DEPTH
                        MOVE 1 TO J-STK(J-DEPTH)
                        ADD 1 TO J-P
                        PERFORM JSON-WS
                        IF J-P < J-E AND LK-J(J-P:1) = "}"
                            ADD 1 TO J-P
                            SUBTRACT 1 FROM J-DEPTH
                            MOVE 3 TO J-STATE
                        ELSE
                            MOVE 2 TO J-STATE
                        END-IF
                    WHEN "["
                        IF J-DEPTH >= 64 EXIT PARAGRAPH END-IF
                        ADD 1 TO J-DEPTH
                        MOVE 2 TO J-STK(J-DEPTH)
                        ADD 1 TO J-P
                        PERFORM JSON-WS
                        IF J-P < J-E AND LK-J(J-P:1) = "]"
                            ADD 1 TO J-P
                            SUBTRACT 1 FROM J-DEPTH
                            MOVE 3 TO J-STATE
                        END-IF
                    WHEN "t"
                        SUBTRACT J-P FROM J-E GIVING W-N
                        IF W-N < 4 EXIT PARAGRAPH END-IF
                        IF LK-J(J-P:4) NOT = "true" EXIT PARAGRAPH END-IF
                        ADD 4 TO J-P
                        MOVE 3 TO J-STATE
                    WHEN "f"
                        SUBTRACT J-P FROM J-E GIVING W-N
                        IF W-N < 5 EXIT PARAGRAPH END-IF
                        IF LK-J(J-P:5) NOT = "false" EXIT PARAGRAPH END-IF
                        ADD 5 TO J-P
                        MOVE 3 TO J-STATE
                    WHEN "n"
                        SUBTRACT J-P FROM J-E GIVING W-N
                        IF W-N < 4 EXIT PARAGRAPH END-IF
                        IF LK-J(J-P:4) NOT = "null" EXIT PARAGRAPH END-IF
                        ADD 4 TO J-P
                        MOVE 3 TO J-STATE
                    WHEN OTHER
                        PERFORM JSON-NUMBER
                        IF J-OK = 0 EXIT PARAGRAPH END-IF
                        MOVE 0 TO J-OK
                        MOVE 3 TO J-STATE
                END-EVALUATE
            WHEN 2
                IF LK-J(J-P:1) NOT = '"' EXIT PARAGRAPH END-IF
                MOVE 0 TO JS-MODE
                PERFORM JSON-STR
                IF JS-RC NOT = 0 EXIT PARAGRAPH END-IF
                PERFORM JSON-WS
                IF J-P >= J-E EXIT PARAGRAPH END-IF
                IF LK-J(J-P:1) NOT = ":" EXIT PARAGRAPH END-IF
                ADD 1 TO J-P
                MOVE 1 TO J-STATE
            WHEN OTHER
                IF J-DEPTH = 0
                    MOVE 1 TO J-OK
                    EXIT PARAGRAPH
                END-IF
                EVALUATE TRUE
                    WHEN LK-J(J-P:1) = ","
                        ADD 1 TO J-P
                        IF J-STK(J-DEPTH) = 1
                            MOVE 2 TO J-STATE
                        ELSE
                            MOVE 1 TO J-STATE
                        END-IF
                    WHEN LK-J(J-P:1) = "}" AND J-STK(J-DEPTH) = 1
                        ADD 1 TO J-P
                        SUBTRACT 1 FROM J-DEPTH
                    WHEN LK-J(J-P:1) = "]" AND J-STK(J-DEPTH) = 2
                        ADD 1 TO J-P
                        SUBTRACT 1 FROM J-DEPTH
                    WHEN OTHER
                        EXIT PARAGRAPH
                END-EVALUATE
        END-EVALUATE
        IF J-STATE = 3 AND J-DEPTH = 0
            MOVE 1 TO J-OK
            EXIT PARAGRAPH
        END-IF
    END-PERFORM.

*> -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
JSON-NUMBER.
    MOVE 0 TO J-OK
    IF LK-J(J-P:1) = "-" ADD 1 TO J-P END-IF
    IF J-P >= J-E EXIT PARAGRAPH END-IF
    MOVE LK-J(J-P:1) TO BYTE-X(1:1)
    IF BYTE-W = 48
        ADD 1 TO J-P
    ELSE
        IF BYTE-W < 49 OR BYTE-W > 57 EXIT PARAGRAPH END-IF
        PERFORM JSON-DIGITS
    END-IF
    IF J-P < J-E AND LK-J(J-P:1) = "."
        ADD 1 TO J-P
        IF J-P >= J-E EXIT PARAGRAPH END-IF
        MOVE LK-J(J-P:1) TO BYTE-X(1:1)
        IF BYTE-W < 48 OR BYTE-W > 57 EXIT PARAGRAPH END-IF
        PERFORM JSON-DIGITS
    END-IF
    IF J-P < J-E AND (LK-J(J-P:1) = "e" OR LK-J(J-P:1) = "E")
        ADD 1 TO J-P
        IF J-P < J-E AND (LK-J(J-P:1) = "+" OR LK-J(J-P:1) = "-") ADD 1 TO J-P END-IF
        IF J-P >= J-E EXIT PARAGRAPH END-IF
        MOVE LK-J(J-P:1) TO BYTE-X(1:1)
        IF BYTE-W < 48 OR BYTE-W > 57 EXIT PARAGRAPH END-IF
        PERFORM JSON-DIGITS
    END-IF
    MOVE 1 TO J-OK.

JSON-DIGITS.
    PERFORM UNTIL J-P >= J-E
        MOVE LK-J(J-P:1) TO BYTE-X(1:1)
        IF BYTE-W < 48 OR BYTE-W > 57 EXIT PERFORM END-IF
        ADD 1 TO J-P
    END-PERFORM.

*> String at J-P (on its opening quote). JS-MODE 0 = validate only, 1 = decode into JS-BUF,
*> 2 = decode into KEY-BUF2. JS-RC 0 ok, -1 syntax error, -2 valid but longer than JS-CAP.
JSON-STR.
    MOVE 0 TO JS-LEN JS-RC
    ADD 1 TO J-P
    PERFORM FOREVER
        IF J-P >= J-E
            MOVE -1 TO JS-RC
            EXIT PARAGRAPH
        END-IF
        MOVE LK-J(J-P:1) TO BYTE-X(1:1)
        EVALUATE TRUE
            WHEN BYTE-W = 34
                ADD 1 TO J-P
                IF JS-LEN > JS-CAP AND JS-MODE NOT = 0 MOVE -2 TO JS-RC END-IF
                EXIT PARAGRAPH
            WHEN BYTE-W < 32
                MOVE -1 TO JS-RC
                EXIT PARAGRAPH
            WHEN BYTE-W = 92
                PERFORM JSON-ESCAPE
                IF JS-RC NOT = 0 EXIT PARAGRAPH END-IF
            WHEN BYTE-W < 128
                ADD 1 TO JS-LEN
                IF JS-MODE NOT = 0 AND JS-LEN <= JS-CAP
                    IF JS-MODE = 1
                        MOVE LK-J(J-P:1) TO JS-BUF(JS-LEN:1)
                    ELSE
                        MOVE LK-J(J-P:1) TO KEY-BUF2(JS-LEN:1)
                    END-IF
                END-IF
                ADD 1 TO J-P
            WHEN OTHER
                PERFORM JSON-UTF8
                IF JS-RC NOT = 0 EXIT PARAGRAPH END-IF
        END-EVALUATE
    END-PERFORM.

*> one multi-byte UTF-8 sequence at J-P, lead byte in BYTE-W
JSON-UTF8.
    MOVE 0 TO JS-UL
    ADD 1 TO J-P GIVING W-K
    IF W-K < J-E
        MOVE LK-J(W-K:1) TO BYTE2-X(1:1)
    ELSE
        MOVE 0 TO BYTE2-W
    END-IF
    EVALUATE TRUE
        WHEN BYTE-W >= 194 AND BYTE-W <= 223
            MOVE 2 TO JS-UL
        WHEN BYTE-W >= 224 AND BYTE-W <= 239
            MOVE 3 TO JS-UL
            IF BYTE-W = 224 AND BYTE2-W < 160 MOVE 0 TO JS-UL END-IF
            IF BYTE-W = 237 AND BYTE2-W > 159 MOVE 0 TO JS-UL END-IF
        WHEN BYTE-W >= 240 AND BYTE-W <= 244
            MOVE 4 TO JS-UL
            IF BYTE-W = 240 AND BYTE2-W < 144 MOVE 0 TO JS-UL END-IF
            IF BYTE-W = 244 AND BYTE2-W > 143 MOVE 0 TO JS-UL END-IF
    END-EVALUATE
    SUBTRACT J-P FROM J-E GIVING W-N
    IF JS-UL = 0 OR W-N < JS-UL
        MOVE -1 TO JS-RC
        EXIT PARAGRAPH
    END-IF
    PERFORM VARYING W-I FROM 1 BY 1 UNTIL W-I >= JS-UL
        ADD J-P W-I GIVING W-K
        MOVE LK-J(W-K:1) TO BYTE2-X(1:1)
        IF BYTE2-W < 128 OR BYTE2-W > 191
            MOVE -1 TO JS-RC
            EXIT PARAGRAPH
        END-IF
    END-PERFORM
    IF JS-MODE NOT = 0
        ADD JS-LEN JS-UL GIVING W-K
        IF W-K <= JS-CAP
            ADD 1 TO JS-LEN GIVING W-K
            IF JS-MODE = 1
                MOVE LK-J(J-P:JS-UL) TO JS-BUF(W-K:JS-UL)
            ELSE
                MOVE LK-J(J-P:JS-UL) TO KEY-BUF2(W-K:JS-UL)
            END-IF
        END-IF
    END-IF
    ADD JS-UL TO JS-LEN
    ADD JS-UL TO J-P.

*> backslash escape at J-P
JSON-ESCAPE.
    ADD 1 TO J-P
    IF J-P >= J-E
        MOVE -1 TO JS-RC
        EXIT PARAGRAPH
    END-IF
    MOVE 1 TO JS-UL
    EVALUATE LK-J(J-P:1)
        WHEN '"'  MOVE '"' TO JS-UB(1)
        WHEN "\"  MOVE "\" TO JS-UB(1)
        WHEN "/"  MOVE "/" TO JS-UB(1)
        WHEN "b"  MOVE X"08" TO JS-UB(1)
        WHEN "f"  MOVE X"0C" TO JS-UB(1)
        WHEN "n"  MOVE X"0A" TO JS-UB(1)
        WHEN "r"  MOVE X"0D" TO JS-UB(1)
        WHEN "t"  MOVE X"09" TO JS-UB(1)
        WHEN "u"
            PERFORM JSON-HEX4
            IF JS-RC NOT = 0 EXIT PARAGRAPH END-IF
            MOVE JS-CP TO JS-LO
            *> a high surrogate followed by \uDC00-\uDFFF combines into one code point
            IF JS-CP >= 55296 AND JS-CP <= 56319
                SUBTRACT J-P FROM J-E GIVING W-N
                ADD 1 TO J-P GIVING W-K
                IF W-N >= 7
                    IF LK-J(W-K:2) = "\u"
                        MOVE J-P TO W-L
                        ADD 2 TO J-P
                        PERFORM JSON-HEX4
                        IF JS-RC NOT = 0 EXIT PARAGRAPH END-IF
                        IF JS-CP >= 56320 AND JS-CP <= 57343
                            COMPUTE JS-CP = 65536 + (JS-LO - 55296) * 1024 + (JS-CP - 56320)
                        ELSE
                            MOVE W-L TO J-P
                            MOVE JS-LO TO JS-CP
                        END-IF
                    END-IF
                END-IF
            END-IF
            *> lone surrogate: U+FFFD, as JS TextEncoder does
            IF JS-CP >= 55296 AND JS-CP <= 57343 MOVE 65533 TO JS-CP END-IF
            PERFORM UTF8-ENCODE
        WHEN OTHER
            MOVE -1 TO JS-RC
            EXIT PARAGRAPH
    END-EVALUATE
    IF JS-MODE NOT = 0
        ADD JS-LEN JS-UL GIVING W-K
        IF W-K <= JS-CAP
            ADD 1 TO JS-LEN GIVING W-K
            IF JS-MODE = 1
                MOVE JS-U(1:JS-UL) TO JS-BUF(W-K:JS-UL)
            ELSE
                MOVE JS-U(1:JS-UL) TO KEY-BUF2(W-K:JS-UL)
            END-IF
        END-IF
    END-IF
    ADD JS-UL TO JS-LEN
    ADD 1 TO J-P.

*> four hex digits after the 'u' at J-P -> JS-CP; leaves J-P on the last digit
JSON-HEX4.
    SUBTRACT J-P FROM J-E GIVING W-N
    IF W-N < 5
        MOVE -1 TO JS-RC
        EXIT PARAGRAPH
    END-IF
    ADD 1 TO J-P
    MOVE LK-J(J-P:1) TO BYTE-X(1:1)
    ADD 1 TO BYTE-W GIVING W-K
    MOVE HEXV(W-K) TO H1
    ADD 1 TO J-P
    MOVE LK-J(J-P:1) TO BYTE-X(1:1)
    ADD 1 TO BYTE-W GIVING W-K
    MOVE HEXV(W-K) TO H2
    ADD 1 TO J-P
    MOVE LK-J(J-P:1) TO BYTE-X(1:1)
    ADD 1 TO BYTE-W GIVING W-K
    MOVE HEXV(W-K) TO H3
    ADD 1 TO J-P
    MOVE LK-J(J-P:1) TO BYTE-X(1:1)
    ADD 1 TO BYTE-W GIVING W-K
    MOVE HEXV(W-K) TO H4
    IF H1 < 0 OR H2 < 0 OR H3 < 0 OR H4 < 0
        MOVE -1 TO JS-RC
        EXIT PARAGRAPH
    END-IF
    COMPUTE JS-CP = H1 * 4096 + H2 * 256 + H3 * 16 + H4.

*> JS-CP -> JS-U(1:JS-UL)
UTF8-ENCODE.
    EVALUATE TRUE
        WHEN JS-CP < 128
            MOVE JS-CP TO OUT-W
            MOVE OUT-X(1:1) TO JS-UB(1)
            MOVE 1 TO JS-UL
        WHEN JS-CP < 2048
            COMPUTE OUT-W = 192 + JS-CP / 64
            MOVE OUT-X(1:1) TO JS-UB(1)
            COMPUTE OUT-W = 128 + FUNCTION MOD(JS-CP, 64)
            MOVE OUT-X(1:1) TO JS-UB(2)
            MOVE 2 TO JS-UL
        WHEN JS-CP < 65536
            COMPUTE OUT-W = 224 + JS-CP / 4096
            MOVE OUT-X(1:1) TO JS-UB(1)
            COMPUTE OUT-W = 128 + FUNCTION MOD(JS-CP / 64, 64)
            MOVE OUT-X(1:1) TO JS-UB(2)
            COMPUTE OUT-W = 128 + FUNCTION MOD(JS-CP, 64)
            MOVE OUT-X(1:1) TO JS-UB(3)
            MOVE 3 TO JS-UL
        WHEN OTHER
            COMPUTE OUT-W = 240 + JS-CP / 262144
            MOVE OUT-X(1:1) TO JS-UB(1)
            COMPUTE OUT-W = 128 + FUNCTION MOD(JS-CP / 4096, 64)
            MOVE OUT-X(1:1) TO JS-UB(2)
            COMPUTE OUT-W = 128 + FUNCTION MOD(JS-CP / 64, 64)
            MOVE OUT-X(1:1) TO JS-UB(3)
            COMPUTE OUT-W = 128 + FUNCTION MOD(JS-CP, 64)
            MOVE OUT-X(1:1) TO JS-UB(4)
            MOVE 4 TO JS-UL
    END-EVALUATE.

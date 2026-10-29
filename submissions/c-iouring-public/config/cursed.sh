# cursed.sh — how much unusual stuff the build does. Sourced by build.sh and tools/dev.sh, after sqlite.sh.
# Each knob can be set on its own. Presets: CURSED=max (default, what we submit); CURSED=lawful: stock SQLite as its
# own unit on its public API (prepared SQL, SQLite's VFS, inline checkpoints) with every non-SQLite curse still on
# (batch parser, fbparse, SIMD JWT, exclusive locking); CURSED=boring: every knob at its plain, by-the-book value.
#
#   knob            cursed default (what we submit)                         boring
#   SQLITE_PATCHES  pcache1-maxkey                                          none: stock SQLite 3.54.1
#   SQLITE_LINK     unity: components/engine.c #includes sqlite3.c (one    separate: sqlite3.c is its own unit and
#                   unit, so it can use SQLite's internals)                 src/engine_public.c uses only sqlite3.h
#                   (independent of SQLITE_PATCHES; separate needs DB_READS=sql, DB_VFS=stock, DB_CKPT=sqlite|inline,
#                   DB_WAL_DROP=keep, and build.sh checks the objects use only public sqlite3_* symbols)
#   DB_READS        direct: reads through SQLite's B-tree cursors           sql: the spec's prepared SELECTs
#   DB_VFS          cursed: WAL write-coalescing + file-size cache shim     stock: SQLite's unix VFS
#   DB_CKPT         thread (max's default; below) or uring: the engine's    sqlite: SQLite's autocheckpoint
#                   own checkpoint as
#                   a state machine on the event loop's io_uring, same write gate, SQLITE_THREADSAFE=0, exclusive;
#                   thread: checkpoint thread + write gate; inline: PASSIVE after the replies, every 16,384 frames;
#                   bounded: RESTART on the loop thread every 16,384 frames once the batch's replies are submitted,
#                   WAL rewritten in place (DB_WAL_DROP defaults to keep), no write gate, SQLITE_THREADSAFE=0
#                   (the checkpoint style of Rust #82). CURSED=boring refuses uring.
#   DB_WAL_DROP     drop: checkpointed WAL pages leave the page cache       keep
#   DB_LOCKING      exclusive (thread mode always uses unix-excl + NORMAL)  normal
#   JWT             simd: multi-buffer batch HMAC (AVX-512VL / AVX2)        scalar
#   PARSER_FAST     batch (config/parsers.sh)                               slow
#   PARSER_SLOW     fb                                                      pico
# In every mode, writes are prepared SQL INSERTs, one read transaction is shared per batch, and every response waits
# for its batch's COMMIT.

case ${CURSED:-max} in
  # thread, not bounded: bounded's RESTART on the loop grows to 300-640 ms over a 5-min hold and each one fires a
  # 256-request 503 burst (#158's scored 0.60 %); thread: 0 503s and a lower p99 in 5/5 Xeon runs (exp1000-1017)
  max) : "${DB_CKPT:=thread}" ;;
  boring)
    : "${SQLITE_PATCHES:=none}" "${SQLITE_LINK:=separate}" "${DB_READS:=sql}" "${DB_VFS:=stock}" "${DB_CKPT:=sqlite}" "${DB_WAL_DROP:=keep}"
    : "${DB_LOCKING:=normal}" "${JWT:=scalar}" "${PARSER_FAST:=slow}" "${PARSER_SLOW:=pico}"
    ;;
  lawful) # "lawful evil": stock SQLite through its public API only, every other curse on
    : "${SQLITE_PATCHES:=none}" "${SQLITE_LINK:=separate}" "${DB_READS:=sql}" "${DB_VFS:=stock}" "${DB_CKPT:=inline}"
    : "${DB_WAL_DROP:=keep}"
    ;;
  *) echo "CURSED must be max|lawful|boring" >&2; exit 2 ;;
esac

: "${SQLITE_PATCHES:=pcache1-maxkey}"
if [ "$SQLITE_PATCHES" = none ]; then
  SQLITE_PATCHES=""
fi
for p in $SQLITE_PATCHES; do
  [ -f "patches/sqlite/$p.patch" ] || { echo "unknown SQLite patch '$p'" >&2; exit 2; }
done

# choose <name> <value> <word=number>...: the number for value (exits on an unknown value)
choose() {
  local name=$1 value=$2 pair
  shift 2
  for pair in "$@"; do
    if [ "${pair%%=*}" = "$value" ]; then
      echo "${pair#*=}"
      return
    fi
  done
  echo "$name must be one of: ${*%%=*}" >&2
  exit 2
}
D=$(choose DB_READS "${DB_READS:-direct}" direct=1 sql=0) || exit 2
V=$(choose DB_VFS "${DB_VFS:-cursed}" cursed=1 stock=0) || exit 2
K=$(choose DB_CKPT "${DB_CKPT:-thread}" thread=2 inline=1 sqlite=0 uring=3 bounded=4) || exit 2
[ "$K" = 4 ] && : "${DB_WAL_DROP:=keep}" # bounded: a 16,384-frame WAL (~64 MB) reused in place stays hot
WD=$(choose DB_WAL_DROP "${DB_WAL_DROP:-drop}" drop=1 keep=0) || exit 2
X=$(choose DB_LOCKING "${DB_LOCKING:-exclusive}" exclusive=1 normal=0) || exit 2
J=$(choose JWT "${JWT:-simd}" simd=0 scalar=1) || exit 2
L=$(choose SQLITE_LINK "${SQLITE_LINK:-unity}" unity=0 separate=1) || exit 2
NP=$(echo $SQLITE_PATCHES | wc -w | tr -d ' ')
if [ "$K" = 3 ] && [ "${CURSED:-max}" = boring ]; then
  echo "DB_CKPT=uring re-implements SQLite's checkpoint; CURSED=boring uses stock SQLite only" >&2
  exit 2
fi
if [ "$L" = 1 ]; then # separate: the public API has no B-tree cursors, VFS shim, checkpoint thread or fadvise
  bad=""
  [ "$D" = 0 ] || bad="$bad DB_READS=sql"
  [ "$V" = 0 ] || bad="$bad DB_VFS=stock"
  case $K in 0 | 1 | 4) ;; *) bad="$bad DB_CKPT=sqlite|inline|bounded" ;; esac
  [ "$WD" = 0 ] || bad="$bad DB_WAL_DROP=keep"
  if [ -n "$bad" ]; then
    echo "SQLITE_LINK=separate uses only SQLite's public API, so it also needs:$bad" >&2
    exit 2
  fi
fi

if [ "$K" = 3 ]; then # uring: no checkpoint thread, so no SQLite mutexes; the engine needs exclusive locking
  [ "$X" = 1 ] || { echo "DB_CKPT=uring needs DB_LOCKING=exclusive" >&2; exit 2; }
  SQLOPTS="${SQLOPTS/-DSQLITE_THREADSAFE=2/-DSQLITE_THREADSAFE=0}"
fi
if [ "$K" = 4 ]; then # bounded: one thread, one connection
  SQLOPTS="${SQLOPTS/-DSQLITE_THREADSAFE=2/-DSQLITE_THREADSAFE=0}"
fi

APPOPTS="$APPOPTS -DAPP_SQLITE_NPATCH=$NP -DAPP_DB_DIRECT=$D -DAPP_DB_VFS=$V -DAPP_DB_CKPT=$K -DAPP_DB_EXCL=$X"
APPOPTS="$APPOPTS -DAPP_DB_WAL_DROP=$WD -DAPP_JWT_SCALAR=$J -DAPP_SQLITE_LINK=$L"
CURSED_SUMMARY="sqlite_patches=[${SQLITE_PATCHES:-none}] sqlite_link=${SQLITE_LINK:-unity} reads=${DB_READS:-direct} vfs=${DB_VFS:-cursed}"
CURSED_SUMMARY="$CURSED_SUMMARY ckpt=${DB_CKPT:-thread} wal_drop=${DB_WAL_DROP:-drop} locking=${DB_LOCKING:-exclusive}"
CURSED_SUMMARY="$CURSED_SUMMARY jwt=${JWT:-simd}"

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
#   DB_CKPT         uring (max's default): the engine's own checkpoint as   sqlite: SQLite's autocheckpoint
#                   a state machine on the event loop's io_uring, same write gate, SQLITE_THREADSAFE=0, exclusive;
#                   thread: checkpoint thread + write gate; inline: PASSIVE after the replies, every 16,384 frames.
#                   CURSED=boring refuses uring.
#   DB_WAL_DROP     drop: checkpointed WAL pages leave the page cache       keep
#   DB_LOCKING      exclusive (thread mode always uses unix-excl + NORMAL)  normal
#   JWT             simd: multi-buffer batch HMAC (AVX-512VL / AVX2)        scalar
#   PARSER_FAST     batch (config/parsers.sh)                               slow
#   PARSER_SLOW     fb                                                      pico
# In every mode, writes are prepared SQL INSERTs, one read transaction is shared per batch, and every response waits
# for its batch's COMMIT.

case ${CURSED:-max} in
  max) : "${DB_CKPT:=uring}" ;;
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
K=$(choose DB_CKPT "${DB_CKPT:-thread}" thread=2 inline=1 sqlite=0 uring=3) || exit 2
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
  [ "$K" -lt 2 ] || bad="$bad DB_CKPT=sqlite|inline"
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

APPOPTS="$APPOPTS -DAPP_SQLITE_NPATCH=$NP -DAPP_DB_DIRECT=$D -DAPP_DB_VFS=$V -DAPP_DB_CKPT=$K -DAPP_DB_EXCL=$X"
APPOPTS="$APPOPTS -DAPP_DB_WAL_DROP=$WD -DAPP_JWT_SCALAR=$J -DAPP_SQLITE_LINK=$L"
CURSED_SUMMARY="sqlite_patches=[${SQLITE_PATCHES:-none}] sqlite_link=${SQLITE_LINK:-unity} reads=${DB_READS:-direct} vfs=${DB_VFS:-cursed}"
CURSED_SUMMARY="$CURSED_SUMMARY ckpt=${DB_CKPT:-thread} wal_drop=${DB_WAL_DROP:-drop} locking=${DB_LOCKING:-exclusive}"
CURSED_SUMMARY="$CURSED_SUMMARY jwt=${JWT:-simd}"

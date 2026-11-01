# compile.sh — how the server is compiled and linked. Sourced by build.sh and tools/dev.sh, after sqlite.sh,
# cursed.sh and parsers.sh have set SQLOPTS, APPOPTS and the parser choice.
#
#   generate_parser <outdir>                       <outdir>/http_gen.h from the API spec (gen/)
#   compile_units   <outdir> <sqlite-dir> <flags>  the server's objects in <outdir> plus the slow parser's objects
#   link_server     <outdir> <binary> <flags>
#
# Translation units:
#   SQLITE_LINK=unity (default)
#     src/unit_sqlite.c    the SQLite amalgamation + components/engine.c (which reads SQLite's B-trees directly)
#   SQLITE_LINK=separate
#     sqlite3.c            SQLite on its own
#     src/engine_public.c  engine.h over sqlite3.h only; check_public_api fails the build on any other SQLite symbol
#   both
#     src/unit_body.c      components/body.c (own unit: its static helpers share names with jwt.c's)
#     src/unit_server.c    everything else: loop, JWT (third_party/cursed-jwt), parsers (third_party/cursed-http-parser + the generated one), app, main
# WARN adds warning flags (tools/dev.sh: -Wall); format errors are always fatal. REUSE_SQLITE=1 keeps an existing
# sqlite.o built from the same SQLite directory (dev only: that unit is ~30-100 s to compile).

# third_party/cursed-http-parser and third_party/cursed-jwt: our libraries, vendored at pinned commits (PINS)
HP=third_party/cursed-http-parser
INCLUDES="-Isrc -Icomponents -I$HP/include -I$HP/vendor/pico -Ithird_party/cursed-jwt/include"

generate_parser() {
  python3 $HP/gen/httpgen.py gen gen/spec.json --base $HP/include/fastparse.h -o "$1/http_gen.h" \
    --profile gen/profile_mload.json > /dev/null && return 0
  echo "parser generator failed: using the library's fastparse.h" >&2
  cp $HP/include/fastparse.h "$1/http_gen.h"
}

compile_units() {
  local out=$1 sq=$2
  shift 2
  # -Werror=format in every build: a printf format that doesn't match its arguments must not compile. (No -w by
  # default: gcc's -w also silences warnings promoted to errors.)
  local cc=${CC:-cc} warn="${WARN:-} -Wformat -Werror=format"
  if [ "${REUSE_SQLITE:-}" = 1 ] && [ -f "$out/sqlite.o" ] && [ "$(cat "$out/sqlite.o.src" 2> /dev/null)" = "$sq ${SQLITE_LINK:-unity} $SQLOPTS" ] &&
    [ "$out/sqlite.o" -nt components/engine.c ]; then
    :
  else
    if [ "${SQLITE_LINK:-unity}" = separate ]; then
      $cc "$@" -I"$sq" $SQLOPTS -c "$sq/sqlite3.c" -o "$out/sqlite.o" || return 1
    else
      local tq=""
      if [ "${TURBO:-0}" = 1 ]; then # DB_READS=turbo: qc.c and its generated record decoders join this unit
        python3 third_party/cursed-sqlite-turbo/qc/gen_decoders.py "$out/qc_dec.h" || return 1
        tq="-Ithird_party/cursed-sqlite-turbo/qc -I$out"
      fi
      # shellcheck disable=SC2086 # $tq is a flag list
      $cc "$@" -I"$sq" $INCLUDES $tq $SQLOPTS -c src/unit_sqlite.c -o "$out/sqlite.o" || return 1
    fi
    echo "$sq ${SQLITE_LINK:-unity} $SQLOPTS" > "$out/sqlite.o.src"
  fi
  EXTRA_OBJS=""
  if [ "${SQLITE_LINK:-unity}" = separate ]; then
    $cc "$@" $warn -I"$sq" $INCLUDES -c src/engine_public.c -o "$out/engine.o" || return 1
    EXTRA_OBJS="$out/engine.o"
  fi
  $cc "$@" $warn $INCLUDES -c src/unit_body.c -o "$out/body.o" || return 1
  $cc "$@" $warn $APPOPTS $INCLUDES -I"$out" -c src/unit_server.c -o "$out/server.o" || return 1
  if [ -n "${JWT_ASM:-}" ]; then # OpenSSL's x86-64 SHA-256 block function, cursed-jwt's batch-1 path (sqlite.sh)
    $cc "$@" -c "$JWT_ASM" -o "$out/jwt_sha256.o" || return 1
    EXTRA_OBJS="$EXTRA_OBJS $out/jwt_sha256.o"
  fi
  parser_objs "$out" "$@" || return 1
  if [ "${SQLITE_LINK:-unity}" = separate ]; then check_public_api "$sq" "$out/engine.o" "$out/server.o"; fi
}

# check_public_api <sqlite-dir> <objects...>: every SQLite symbol the objects reference must be declared in
# sqlite3.h (sqlite3_* public API). Internals (sqlite3Btree*, sqlite3Pager*, Vdbe...) would fail here.
check_public_api() {
  local sq=$1 bad
  shift
  bad=$(nm -u "$@" | awk '{print $NF}' | sed 's/^_//' | grep -i '^sqlite' | sort -u |
    grep -vxF -f <(grep -oE 'sqlite3_[A-Za-z0-9_]+' "$sq/sqlite3.h" | sort -u) || true)
  if [ -n "$bad" ]; then
    echo "SQLITE_LINK=separate: non-public SQLite symbols referenced: $bad" >&2
    return 1
  fi
  echo "public-API check: $(nm -u "$@" | awk '{print $NF}' | sed 's/^_//' | grep -c '^sqlite3_') SQLite references, all declared in sqlite3.h" >&2
}

link_server() {
  local out=$1 bin=$2
  shift 2
  ${LD:-${CC:-cc}} "$@" "$out/sqlite.o" $EXTRA_OBJS "$out/body.o" "$out/server.o" $PARSER_OBJS $PARSER_LIBS \
    -o "$bin.new" -lm -lpthread && mv "$bin.new" "$bin"
}

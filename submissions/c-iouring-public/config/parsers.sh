# parsers.sh — which HTTP parsers are compiled in. Sourced by build.sh and tools/dev.sh, after cursed.sh.
#   PARSER_FAST  batch (default: bp_parse lanes + fast_parse) | single (fast_parse) | learn (fast_parse_learn)
#                | slow (no fast path)
#   PARSER_SLOW  fb (default: components/fbparse.h) | pico | llhttp | uws (C++) | hparse (needs zig 0.15)
#                | httparse (needs rustc)
# The default (batch + fb) is plain C and needs nothing beyond build-essential. The others live in
# third_party/parsers/, each behind picohttpparser's phr_parse_request signature (third_party/parsers/adapters/).

PF=$(choose PARSER_FAST "${PARSER_FAST:-batch}" batch=2 single=1 learn=3 slow=0) || exit 2
PS=$(choose PARSER_SLOW "${PARSER_SLOW:-fb}" fb=0 pico=1 llhttp=2 uws=3 hparse=4 httparse=5) || exit 2
APPOPTS="$APPOPTS -DAPP_FAST=$PF -DAPP_SLOW=$PS"

# parser_objs <outdir> <compile flags...>: builds the chosen slow parser's extra objects.
# Sets PARSER_OBJS, PARSER_LIBS and LD (the link driver: g++ when uWS's C++ is linked in).
parser_objs() {
  local out=$1
  shift
  local dir=third_party/parsers cc=${CC:-cc} cxx=${CXX:-g++}
  PARSER_OBJS=""
  PARSER_LIBS=""
  LD=$cc
  case $PS in
    2) # llhttp (C)
      for f in llhttp api http; do
        $cc "$@" -w -I$dir/llhttp/include -c $dir/llhttp/src/$f.c -o "$out/llhttp_$f.o" || return 1
        PARSER_OBJS="$PARSER_OBJS $out/llhttp_$f.o"
      done
      $cc "$@" -w -c $dir/adapters/lx_llhttp.c -o "$out/lx_llhttp.o" || return 1
      PARSER_OBJS="$PARSER_OBJS $out/lx_llhttp.o"
      ;;
    3) # uWebSockets (C++)
      $cxx -std=c++20 "$@" -w -c $dir/adapters/uw_uws.cpp -o "$out/uw_uws.o" || return 1
      PARSER_OBJS="$out/uw_uws.o"
      LD=$cxx
      ;;
    4) # hparse (Zig)
      command -v zig > /dev/null || { echo "PARSER_SLOW=hparse needs zig 0.15 on PATH" >&2; return 1; }
      zig build-obj -O ReleaseFast -mcpu=native -fPIC --dep hparse -Mroot=$dir/adapters/hx_hparse.zig \
        -Mhparse=$dir/hparse/src/root.zig -femit-bin="$out/hx_hparse.o" || return 1
      PARSER_OBJS="$out/hx_hparse.o"
      ;;
    5) # httparse (Rust), built the way its build.rs would: compile-time SIMD where the target has it
      command -v rustc > /dev/null || { echo "PARSER_SLOW=httparse needs rustc on PATH" >&2; return 1; }
      local cfg='--cfg feature=\"std\" --cfg httparse_simd --cfg httparse_simd_neon_intrinsics' feats # eval'd below
      feats=$(rustc -C target-cpu=native --print cfg | grep target_feature || true)
      if echo "$feats" | grep -q '"avx2"'; then cfg="$cfg --cfg httparse_simd_target_feature_avx2"; fi
      if echo "$feats" | grep -q '"sse4.2"'; then cfg="$cfg --cfg httparse_simd_target_feature_sse42"; fi
      eval rustc --edition 2018 --crate-type rlib --crate-name httparse -C opt-level=3 -C target-cpu=native $cfg \
        $dir/httparse/src/lib.rs -o "$out/libhttparse.rlib" || return 1
      rustc --edition 2018 --crate-type staticlib -C opt-level=3 -C target-cpu=native -C panic=abort \
        --extern httparse="$out/libhttparse.rlib" -L "$out" $dir/adapters/hx_httparse.rs -o "$out/libhx.a" || return 1
      PARSER_LIBS="$out/libhx.a -ldl"
      ;;
  esac
}

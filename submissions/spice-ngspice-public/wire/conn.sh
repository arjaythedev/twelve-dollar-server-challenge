#!/usr/bin/env bash
# conn.sh: one keep-alive connection from the benchmark's nginx (socat forks one per connection).
# Per request it frames the message off the socket (the header lines up to the blank line, then
# Content-Length bytes of body), hands the bytes to the chip as received, runs the SQL the chip asks
# for through the sqlite3 CLI, hands SQLite's output back to the chip and writes the chip's HTTP
# response to the socket. A '!' from the first job means the chip answered without the database.
export LC_ALL=C
here=$(cd "$(dirname "$0")" && pwd)
board=$here/../build/board.cir
boot=$(<"$BOOT_FILE")
d=$(mktemp -d "${TMPDIR:-/tmp}/conn.XXXXXX"); trap 'rm -rf "$d"' EXIT
cd "$d"

# chip <files...>: one job through the board; the job is the concatenated files, the chip's output goes
# to stdout. This only turns bytes into logic levels (d_source rows) and logic levels back into bytes.
#   d_source rows: time run iv ie i7..i0. run starts the clock at 6 ns; byte k is on the pins from
#   8 + 10k ns (2 ns after a falling edge) and the chip takes it at the rising edge 3 ns later; then ie.
#   eprint lists every change of ot/ob; a byte is ob at each new value of ot (last row per timestep).
chip() {
  od -An -v -tu1 "$@" | awk 'BEGIN { t = 8e-9; z = " 0s 0s 0s 0s 0s 0s 0s 0s"; printf "0 0s 0s 0s%s\n6e-9 1s 0s 0s%s\n", z, z }
    { for (i = 1; i <= NF; i++) { b = $i; s = ""
        for (j = 128; j >= 1; j /= 2) { s = s (b >= j ? " 1s" : " 0s"); if (b >= j) b -= j }
        printf "%.4e 1s 1s 0s%s\n", t, s; t += 1e-8 } }
    END { printf "%.4e 1s 0s 1s%s\n%.4e 1s 0s 0s%s\n", t, z, t + 1e-8, z }' > in.txt
  ngspice -b "$board" 2>/dev/null | awk '
    function flush() { if (have && ot != pot) { printf "%c", byte; pot = ot } }
    /^[0-9]\.[0-9]+e[-+][0-9]+[ \t]/ {
      if ($1 != pt) flush(); pt = $1; have = 1
      ot = substr($2, 1, 1); byte = 0
      for (i = 3; i <= 10; i++) byte = byte * 2 + (substr($i, 1, 1) == "1")
    }
    END { flush() }' pot=0
}

printf '\035' > sep
while IFS= read -r line; do
  req=$line$'\n'; cl=0
  while IFS= read -r line; do
    req+=$line$'\n'
    [ "$line" = $'\r' ] || [ -z "$line" ] && break
    case "${line,,}" in content-length:*) cl=${line#*:}; cl=${cl//[!0-9]/};; esac
  done
  body=; [ "${cl:-0}" -gt 0 ] && IFS= read -r -N "$cl" body
  printf 'A\n%s\n%s\n%s\n%s%s' "$EPOCHSECONDS" "$boot" "$JWT_SECRET" "$req" "$body" > job
  chip job > a
  IFS= read -r -n 1 c < a || exit 1   # no answer from the board: drop the connection
  if [ "$c" = '?' ]; then
    { IFS= read -r kind; sqlite3 -batch -ascii -cmd '.timeout 10000' "$SQLITE_PATH" > out 2> err; } < a
    printf 'B\n%s\n%s\n' "${kind#?}" "$?" > job
    chip job out sep err > b
    [ -s b ] || exit 1
    r=$(<b); printf '%s' "$r"
  else
    r=$(<a); printf '%s' "${r#?}"
  fi
done

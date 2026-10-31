#!/usr/bin/env bash
# server.sh: the network card driver. The web application is SQL (sql/); this file only moves bytes.
#
# sqlite3 can't open a socket or read one as it arrives, so Bash does those two things: it listens
# with its `accept` loadable builtin, reads a request's head line by line and its body by
# Content-Length, and quotes both into SQL. SQLite parses the request, picks the route (it writes
# a `.read route/<name>.sql` command to a file and then reads that file), checks the JWT
# (HMAC-SHA256 in a recursive CTE), runs the queries and renders the whole HTTP response.
#
# Served directly, every user holds a connection, and Bash has no select/epoll, so every
# connection gets its own small Bash process. They share a few sqlite3 processes: a connection
# writes its request to its own file and sends one line, `.read <file>`, to a sqlite3's FIFO
# (one line is one write, so lines from different connections never interleave: Bash
# line-buffers stdout); sqlite3 writes the response into the connection's FIFO with `.once`,
# length first, and the connection copies it to the socket. Nothing on the request path forks.
#
# `accept` binds, listens, takes one connection and closes the listener, so there is one listener
# at a time. Idle workers pass a one-byte token through a FIFO; the holder listens and hands the
# token on as soon as it has a connection, and the main shell forks a new idle worker.
set -u
export LC_ALL=C
shopt -u patsub_replacement  # "&" in a ${x//a/b} replacement would otherwise mean the match
DIR=$(cd "$(dirname "$0")" && pwd)
HOST=${HOST:-0.0.0.0} PORT=${PORT:-80} POOL=${POOL:-2} IDLE=${IDLE:-4}
: "${SQLITE_PATH:?}" "${JWT_SECRET:?}"
enable -f accept accept && enable -f mkfifo mkfifo && enable -f rm rm && enable -f sleep sleep && enable -f tee tee || exit 1

RUN=$(mktemp -d -p /dev/shm 2>/dev/null || mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$RUN"' EXIT  # our children only: `kill 0` would also hit whoever started us
trap 'exit 0' TERM INT
printf ".output /dev/null\nPRAGMA busy_timeout=5000;\nPRAGMA temp_store=MEMORY;\nCREATE TEMP TABLE cfg AS SELECT '%s' AS secret, %d AS boot;\n.read app.sql\n" \
	"${JWT_SECRET//\'/\'\'}" "$EPOCHSECONDS" > "$RUN/boot.sql"
sqlite3 "$SQLITE_PATH" "PRAGMA journal_mode=WAL;" > /dev/null || exit 1

# the sqlite3 pool: each reads `.read` lines from its FIFO, held open read-write so it never sees EOF
Q=()
for ((k = 0; k < POOL; k++)); do
	mkfifo "$RUN/q$k" && exec {fd}<>"$RUN/q$k" || exit 1
	Q[k]=$fd
	(cd "$DIR/sql" && exec sqlite3 -batch -init "$RUN/boot.sql" "$SQLITE_PATH" <&"$fd" >/dev/null 2>>"$RUN/sqlite.err") &
done

# serve FD: one keep-alive connection, until the client closes it
serve() {
	local fd=$1 me=$BASHPID head line cl body len resp res k
	k=$((me % POOL))
	trap '' PIPE
	mkfifo "$RUN/r$me" && exec {res}<>"$RUN/r$me" || return
	while IFS= read -r -u "$fd" head; do
		cl=0 body=""
		while IFS= read -r -u "$fd" line && [[ -n $line && $line != $'\r' ]]; do
			head+=$'\n'$line
			[[ $line == [Cc]ontent-[Ll]ength:* ]] && cl=${line//[!0-9]/}
		done
		((cl > 0)) && IFS= read -r -N "$cl" -u "$fd" body
		printf "REPLACE INTO temp.req VALUES(1,'%s','%s');\n.once %s\nSELECT '.read route/'||route||'.sql' FROM temp.q;\n.read %s\n.once %s\nSELECT octet_length(r)||char(10)||r FROM temp.http;\n" \
			"${head//\'/\'\'}" "${body//\'/\'\'}" "$RUN/p$k" "$RUN/p$k" "$RUN/r$me" > "$RUN/c$me.sql"
		printf '.read %s\n' "$RUN/c$me.sql" >&"${Q[k]}"
		IFS= read -r -u "$res" len && IFS= read -r -N "$((len + 1))" -u "$res" resp || break
		tee <<< "${resp%$'\n'}" >&"$fd" || break  # one write (tee copies 8 KB at a time); printf writes each line
		# separately, and on a reused connection Nagle + delayed ACK then hold the rest for 40 ms
	done
	rm -f "$RUN/r$me" "$RUN/c$me.sql"
}

# worker: an idle worker. Once it has a connection it asks the main shell (through the spawn FIFO)
# for a replacement: workers that forked their own replacements made a process chain as deep as the
# number of connections, and the kernel's per-process memory (anon_vma chains) grows with that depth.
worker() {
	local fd
	IFS= read -r -N 1 -u "$TOK" _ || exit 1
	until accept -b "$HOST" -v fd "$PORT" 2>/dev/null; do sleep 0.01; done
	printf . >&"$TOK"
	printf . >&"$SPAWN"
	serve "$fd"
	exit 0
}

mkfifo "$RUN/token" "$RUN/spawn" && exec {TOK}<>"$RUN/token" {SPAWN}<>"$RUN/spawn" || exit 1
for ((i = 0; i < IDLE; i++)); do worker & done
printf . >&"$TOK"
while IFS= read -r -N 1 -u "$SPAWN" _; do worker & done

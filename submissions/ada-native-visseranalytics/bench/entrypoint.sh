#!/usr/bin/env bash
# Own only this container's application and Nginx processes.
set -euo pipefail
: "${SQLITE_PATH:?Set SQLITE_PATH}"
: "${JWT_SECRET:?Set JWT_SECRET}"
if [[ ${ADA_SERVE_MODE:-nginx} == direct ]]; then
  # This container's unprivileged-port range permits port 80. start.sh itself
  # continues to use only the host/port/database/secret variables from SPEC.md.
  exec runuser -u ada -- bash -c 'printf "%s\n" "$$" > /run/ada/app.pid; exec env HOST=0.0.0.0 PORT=80 /app/start.sh'
fi
[[ ${ADA_SERVE_MODE:-nginx} == nginx ]] || { echo 'Expected direct or nginx mode' >&2; exit 1; }
[[ ${HOST:-127.0.0.1} == 127.0.0.1 && ${PORT:-3000} == 3000 ]] || {
  echo "The unchanged challenge Nginx configuration requires HOST=127.0.0.1 PORT=3000" >&2
  exit 1
}
test -r /challenge-bench.conf
runuser -u ada -- test -w "$SQLITE_PATH"
runuser -u ada -- test -w "$(dirname "$SQLITE_PATH")"
nginx -t -c /etc/nginx/ada-benchmark.conf
app_launcher=
nginx_pid=
cleanup() {
  trap - EXIT TERM INT
  if [[ -n $app_launcher ]]; then kill -TERM "$app_launcher" 2>/dev/null || true; fi
  if [[ -n $nginx_pid ]]; then kill -TERM "$nginx_pid" 2>/dev/null || true; fi
  wait || true
}
trap cleanup EXIT
trap 'exit 143' TERM
trap 'exit 130' INT
runuser -u ada -- bash -c 'printf "%s\n" "$$" > /run/ada/app.pid; exec /app/start.sh' &
app_launcher=$!
nginx -c /etc/nginx/ada-benchmark.conf -g 'daemon off;' &
nginx_pid=$!
status=0
wait -n "$app_launcher" "$nginx_pid" || status=$?
# An unexpected clean exit still means this service is no longer complete.
if [[ $status == 0 ]]; then status=1; fi
exit "$status"

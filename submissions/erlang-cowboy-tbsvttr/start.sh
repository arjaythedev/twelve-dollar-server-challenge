#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
[[ ! -x /opt/erlang/29.1.1/bin/erl ]] || export PATH="/opt/erlang/29.1.1/bin:$PATH"
ulimit -Sn "$(ulimit -Hn)"
exec erl +S 1:1 +SDcpu 1:1 +SDio 1 +A 1 -noshell \
  -pa bin .deps/cowlib/ebin .deps/ranch/ebin .deps/cowboy/ebin .deps/esqlite/ebin \
  -s challenge start

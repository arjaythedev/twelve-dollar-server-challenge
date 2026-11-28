#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
[[ ! -x /opt/erlang/29.1.1/bin/erl ]] || export PATH="/opt/erlang/29.1.1/bin:$PATH"
ulimit -Sn "$(ulimit -Hn)"
EX=/opt/elixir/1.20.5; [[ -d $EX ]] || EX=$(elixir -e "IO.write(:code.lib_dir(:elixir))" | sed "s|/lib/elixir$||")
exec erl +S 1:1 +SDcpu 1:1 +SDio 1 +A 1 +hmbs 1024 -noshell \
  -pa bin .deps/esqlite/ebin "$EX/lib/elixir/ebin" \
  -eval '{ok, _} = application:ensure_all_started(elixir), '"'"'Elixir.Server'"'"':start()'

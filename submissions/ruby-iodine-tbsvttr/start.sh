#!/usr/bin/env bash
set -euo pipefail
ulimit -Sn "$(ulimit -Hn)"
cd "$(dirname "$0")"
export PATH="/opt/ruby/bin:$PATH" RUBY_YJIT_ENABLE=1
exec bundle exec ruby server.rb

#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
export PATH="/opt/ruby/bin:$PATH" RUBY_YJIT_ENABLE=1
exec bundle exec ruby server.rb

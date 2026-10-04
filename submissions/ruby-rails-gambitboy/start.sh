#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
export PATH="/opt/ruby/bin:$PATH"
export RAILS_ENV=production
export SECRET_KEY_BASE="${SECRET_KEY_BASE:-$(openssl rand -hex 64)}"
export RUBY_YJIT_ENABLE=1
exec bundle exec puma -C config/puma.rb

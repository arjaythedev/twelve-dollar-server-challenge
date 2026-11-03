#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
export PATH="/opt/ruby/bin:$PATH"
bundle config set --local deployment true
bundle config set --local without "development test"
bundle install --jobs 4
bundle exec bootsnap precompile --gemfile app/ lib/

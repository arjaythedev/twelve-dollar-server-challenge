#!/usr/bin/env bash
# Runs as a normal user: installs the pinned gems (Gemfile.lock) into vendor/bundle.
set -euo pipefail
cd "$(dirname "$0")"
export PATH="/opt/ruby/bin:$PATH"
bundle config set --local deployment true
bundle config set --local path vendor/bundle
bundle config set --local force_ruby_platform true
bundle config set --local build.extralite --with-sqlite3-dir=/opt/sqlite
bundle install --jobs 4

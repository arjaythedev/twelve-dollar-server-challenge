#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
export PATH="/opt/ruby/bin:$PATH"
bundle config set --local deployment true
bundle install --jobs 4

#!/usr/bin/env bash
# build.sh: SQLite writes the HMAC-SHA256 SQL (sql/hmac.sql) from templates
set -euo pipefail
cd "$(dirname "$0")/sql"
sqlite3 :memory: < gen-hmac.sql > hmac.sql

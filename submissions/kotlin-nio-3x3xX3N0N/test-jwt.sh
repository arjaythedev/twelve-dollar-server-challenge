#!/usr/bin/env bash
# Extra JWT checks beyond test/test.sh (which has no "missing exp" case). Run against a server on a scratch DB:
#   bash test-jwt.sh http://127.0.0.1:3000
# Writes one post (the valid-token control). Needs curl and openssl.
set -uo pipefail
BASE="${1:-http://127.0.0.1:3000}"
JWT_SECRET="${JWT_SECRET:-twelve-dollar-challenge}"
b64url() { openssl base64 -A | tr '+/' '-_' | tr -d '='; }
sign() {
  local h p s
  h="$(printf '%s' '{"alg":"HS256","typ":"JWT"}' | b64url)"; p="$(printf '%s' "$1" | b64url)"
  s="$(printf '%s' "$h.$p" | openssl dgst -sha256 -hmac "$JWT_SECRET" -binary | b64url)"
  echo "$h.$p.$s"
}
now=$(date +%s); pass=0; fail=0
check() { # check <label> <payload> <want status> <want body>
  local out
  out="$(curl -s -w ' %{http_code}' -X POST "$BASE/posts" -H "Authorization: Bearer $(sign "$2")" \
         -H 'Content-Type: application/json' --data-binary '{"body":"jwt check"}')"
  if [ "${out##* }" = "$3" ] && { [ -z "$4" ] || [ "${out% *}" = "$4" ]; }; then echo "  ok   $1 -> $3"; pass=$((pass+1))
  else echo "  FAIL $1: want $3 $4, got $out"; fail=$((fail+1)); fi
}
INV='{"error":"invalid or expired token"}'
check "no exp"            '{"sub":"1","username":"golden_ember_1","iat":'$now'}' 401 "$INV"
check "exp null"          '{"sub":"1","username":"golden_ember_1","exp":null}' 401 "$INV"
check "exp as a string"   '{"sub":"1","username":"golden_ember_1","exp":"'$((now+3600))'"}' 401 "$INV"
check "exp in the past"   '{"sub":"1","username":"golden_ember_1","exp":'$((now-1))'}' 401 "$INV"
check "exp in the future" '{"sub":"1","username":"golden_ember_1","exp":'$((now+3600))'}' 201 ""
echo "passed=$pass failed=$fail"
[ "$fail" = 0 ]

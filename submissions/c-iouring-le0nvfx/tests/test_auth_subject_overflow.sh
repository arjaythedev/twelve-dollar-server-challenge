#!/usr/bin/env bash
# Run against a server with a disposable copy of the official seed database.
set -euo pipefail
base=${1:-http://127.0.0.1:3000}
export JWT_SECRET=${JWT_SECRET:-twelve-dollar-challenge}
token_for() {
  python3 - "$1" <<'PY'
import base64, hashlib, hmac, json, os, sys, time
enc = lambda b: base64.urlsafe_b64encode(b).rstrip(b'=')
header = enc(b'{"alg":"HS256","typ":"JWT"}')
payload = enc(json.dumps({'sub':sys.argv[1], 'username':'golden_ember_1',
                          'iat':int(time.time()), 'exp':int(time.time())+3600},
                         separators=(',', ':')).encode())
message = header + b'.' + payload
print((message + b'.' + enc(hmac.new(os.environ['JWT_SECRET'].encode(),
                                    message, hashlib.sha256).digest())).decode())
PY
}
for sub in 9223372036854775808 18446744073709551616 18446744073709551617; do
  token=$(token_for "$sub")
  code=$(curl -sS -o /dev/null -w '%{http_code}' -X POST \
    -H "Authorization: Bearer $token" "$base/posts/1/like")
  if [[ $code != 401 ]]; then
    printf 'FAIL: subject %s returned %s instead of 401\n' "$sub" "$code" >&2
    exit 1
  fi
done
token=$(token_for 1)
code=$(curl -sS -o /dev/null -w '%{http_code}' -X POST \
  -H "Authorization: Bearer $token" "$base/posts/1/like")
if [[ $code != 200 && $code != 201 ]]; then
  printf 'FAIL: valid subject returned %s\n' "$code" >&2
  exit 1
fi
printf '%s\n' 'Signed oversized JWT subjects: PASS'

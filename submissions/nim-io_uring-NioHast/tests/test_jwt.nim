## HS256 JWT tests.
##
## Tokens are generated exactly the way the challenge harness does, i.e. via
##   b64url() { openssl base64 -A | tr '+/' '-_' | tr -d '='; }
##   h = printf '%s' '{"alg":"HS256","typ":"JWT"}' | b64url
##   p = printf '%s' "$payload" | b64url
##   s = printf '%s' "$h.$p" | openssl dgst -sha256 -hmac "$JWT_SECRET" -binary | b64url
## so the test exercises the same wire format as production tokens.
import std/[osproc, streams, strutils, times]
import ../src/jwt

const JwtSecret = "twelve-dollar-challenge"
const Hs256Header = """{"alg":"HS256","typ":"JWT"}"""

var failures = 0

proc check(name: string; cond: bool) =
  if cond:
    echo "PASS ", name
  else:
    echo "FAIL ", name
    inc failures

proc runStdin(cmd: string; args: openArray[string]; data: string): string =
  let p = startProcess(cmd, args = args, options = {poUsePath, poStdErrToStdOut})
  p.inputStream.write(data)
  p.inputStream.close()
  result = p.outputStream.readAll()
  discard p.waitForExit()
  p.close()

proc b64url(data: string): string =
  ## openssl base64 -A | tr '+/' '-_' | tr -d '='
  let raw = runStdin("openssl", ["base64", "-A"], data)
  raw.replace('+', '-').replace('/', '_').replace("=", "").strip()

proc hmacB64(data, secret: string): string =
  ## openssl dgst -sha256 -hmac "$secret" -binary | b64url
  let raw = runStdin("openssl",
    ["dgst", "-sha256", "-hmac", secret, "-binary"], data)
  b64url(raw)

proc makeToken(payloadJson: string;
               headerJson = Hs256Header;
               secret = JwtSecret): string =
  let h = b64url(headerJson)
  let p = b64url(payloadJson)
  let s = hmacB64(h & "." & p, secret)
  h & "." & p & "." & s

proc escapeJson(s: string): string =
  result = "\""
  for c in s:
    case c
    of '"': result.add "\\\""
    of '\\': result.add "\\\\"
    else: result.add c
  result.add "\""

proc payloadJson(sub, username: string; exp: int): string =
  ## Mirrors the harness printf payload ordering.
  "{\"sub\":" & escapeJson(sub) &
    ",\"username\":" & escapeJson(username) &
    ",\"iat\":" & $exp &
    ",\"exp\":" & $exp & "}"

let now = epochTime().int

# 1. Valid token -> jsOk with parsed userId/username.
block:
  let token = makeToken(payloadJson("1", "golden_ember_1", now + 3600))
  let (status, info) = verifyJwt(token, JwtSecret)
  check("valid", status == jsOk and info.userId == 1 and
                 info.username == "golden_ember_1")

# 2. Bad signature (signed with the wrong secret) -> jsInvalid.
block:
  let token = makeToken(payloadJson("1", "golden_ember_1", now + 3600),
                        secret = "not-the-secret")
  let (status, _) = verifyJwt(token, JwtSecret)
  check("bad-signature", status == jsInvalid)

# 3. alg=none with empty signature -> jsInvalid.
block:
  let h = b64url("""{"alg":"none","typ":"JWT"}""")
  let p = b64url(payloadJson("1", "golden_ember_1", now + 3600))
  let token = h & "." & p & "."
  let (status, _) = verifyJwt(token, JwtSecret)
  check("alg-none", status == jsInvalid)

# 4. Expired token (exp in the past) -> jsInvalid.
block:
  let token = makeToken(payloadJson("1", "golden_ember_1", now - 3600))
  let (status, _) = verifyJwt(token, JwtSecret)
  check("expired", status == jsInvalid)

# 5. sub "abc" with a valid signature -> jsBadPayload.
block:
  let h = b64url(Hs256Header)
  let p = b64url("""{"sub":"abc","username":"golden_ember_1","exp":""" &
                  $(now + 3600) & """}""")
  let token = h & "." & p & "." & hmacB64(h & "." & p, JwtSecret)
  let (status, _) = verifyJwt(token, JwtSecret)
  check("bad-sub", status == jsBadPayload)

# 6. Missing username with a valid signature -> jsBadPayload.
block:
  let h = b64url(Hs256Header)
  let p = b64url("""{"sub":"2","exp":""" & $(now + 3600) & """}""")
  let token = h & "." & p & "." & hmacB64(h & "." & p, JwtSecret)
  let (status, _) = verifyJwt(token, JwtSecret)
  check("no-username", status == jsBadPayload)

if failures > 0:
  echo "FAILURES: ", failures
  quit(1)

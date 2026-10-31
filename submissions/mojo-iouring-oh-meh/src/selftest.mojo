# Unit checks for the leaf modules. Build: mojo build src/selftest.mojo -I src
# Run: selftest <jwt-signed-with-twelve-dollar-challenge> <unix-now>
from std.sys import argv
from bytes import Bytes, balloc, bcopy, lit, Out, parse_pos_int
from sha256 import Sha256
from jwt import Jwt, JWT_OK, JWT_INVALID, JWT_PAYLOAD
from jsonscan import validate_body, BODY_OK, BODY_MALFORMED, BODY_REQUIRED, BODY_TOO_LONG
from http import Req, parse_request, P_COMPLETE, P_INCOMPLETE, M_GET, M_POST
from record import varint

def hex(p: Bytes, n: Int) -> String:
    var buf = balloc(n * 2 + 1)
    var digits = lit("0123456789abcdef")
    for i in range(n):
        var b = Int(p[unsafe_offset=i])
        buf[unsafe_offset=i * 2] = digits[unsafe_offset=b >> 4]
        buf[unsafe_offset=i * 2 + 1] = digits[unsafe_offset=b & 15]
    buf[unsafe_offset=n * 2] = 0
    return String(unsafe_from_utf8_ptr=buf)


def check(name: String, ok: Bool) -> Int:
    if ok:
        print("  ok  ", name)
        return 0
    print("  FAIL", name)
    return 1


def sbytes(s: String) -> Bytes:
    # copy a String's bytes into leaked memory
    var n = s.byte_length()
    var p = balloc(n + 1)
    bcopy(p, Bytes(unsafe_from_address=Int(s.unsafe_ptr())), n)
    p[unsafe_offset=n] = 0
    return p


def main() raises:
    var fails = 0
    var out = balloc(32)
    var sha = Sha256()
    var abc = String("abc")
    sha.hash(sbytes(abc), 3, out)
    print("   sha256(abc) =", hex(out, 32))
    fails += check("sha256(abc)", hex(out, 32) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
    var empty = String("")
    sha.hash(sbytes(empty), 0, out)
    fails += check("sha256('')", hex(out, 32) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")
    var long = String("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")
    sha.hash(sbytes(long), long.byte_length(), out)
    fails += check("sha256(56 bytes)", hex(out, 32) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1")
    var key = String("Jefe")
    sha.set_key(sbytes(key), 4)
    var msg = String("what do ya want for nothing?")
    sha.hmac(sbytes(msg), msg.byte_length(), out)
    fails += check("hmac rfc4231 #2", hex(out, 32) == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843")

    # JWT
    var secret = String("twelve-dollar-challenge")
    var jwt = Jwt(sbytes(secret), secret.byte_length())
    var tok = String(argv()[1])
    var now = Int64(Int(String(argv()[2])))
    var rc = jwt.verify(sbytes(tok), tok.byte_length(), now)
    fails += check("jwt verify ok", rc == JWT_OK)
    fails += check("jwt user_id == 1", jwt.user_id == 1)
    var nb = balloc(jwt.name_len + 1)
    bcopy(nb, jwt.name_ptr, jwt.name_len)
    nb[unsafe_offset=jwt.name_len] = 0
    var name = String(unsafe_from_utf8_ptr=nb)
    fails += check("jwt username == golden_ember_1", name == "golden_ember_1")
    fails += check("jwt expired", jwt.verify(sbytes(tok), tok.byte_length(), now + 100000) == JWT_INVALID)
    var bad = tok + "x"
    fails += check("jwt bad sig", jwt.verify(sbytes(bad), bad.byte_length(), now) == JWT_INVALID)
    var garbage = String("abc")
    fails += check("jwt garbage", jwt.verify(sbytes(garbage), 3, now) == JWT_INVALID)

    # HTTP
    var req = String("GET /posts/42?x=1 HTTP/1.1\r\nHost: a\r\nAuthorization:  Bearer abc.def.ghi \r\nConnection: close\r\n\r\n")
    var r = Req(sbytes(req))
    parse_request(sbytes(req), req.byte_length(), r)
    fails += check("http complete", r.status == P_COMPLETE)
    fails += check("http method GET", r.method == M_GET)
    fails += check("http path", r.path_len == 9 and r.path[unsafe_offset=0] == 0x2F and r.path[unsafe_offset=8] == 0x32)
    fails += check("http bearer", r.bearer and r.tok_len == 11)
    fails += check("http close", r.close)
    fails += check("http consumed", r.consumed == req.byte_length())
    var post = String("POST /posts HTTP/1.1\r\nContent-Length: 12\r\n\r\n{\"body\":\"x\"}")
    var r2 = Req(sbytes(post))
    parse_request(sbytes(post), post.byte_length(), r2)
    fails += check("http post body", r2.status == P_COMPLETE and r2.method == M_POST and r2.body_len == 12 and not r2.close)
    var r3 = Req(sbytes(post))
    parse_request(sbytes(post), post.byte_length() - 3, r3)
    fails += check("http incomplete body", r3.status == P_INCOMPLETE)

    # JSON body
    var ob = balloc(8192)
    var olen = 0
    var j1 = String('{"body":"  hi \\"there\\" \\u00e9 \\ud83d\\ude00  ","x":[1,2.5e3,true,null,{}]}')
    rc = validate_body(sbytes(j1), j1.byte_length(), ob, 8192, olen)
    fails += check("json ok", rc == BODY_OK)
    fails += check("json unescaped+trimmed", olen == 18 and ob[unsafe_offset=0] == 0x68 and ob[unsafe_offset=17] == 0x80)
    var j2 = String("{bad json")
    fails += check("json malformed", validate_body(sbytes(j2), j2.byte_length(), ob, 8192, olen) == BODY_MALFORMED)
    var j3 = String('{"body":"   "}')
    fails += check("json whitespace body", validate_body(sbytes(j3), j3.byte_length(), ob, 8192, olen) == BODY_REQUIRED)
    var j4 = String('{"body":123}')
    fails += check("json numeric body", validate_body(sbytes(j4), j4.byte_length(), ob, 8192, olen) == BODY_REQUIRED)
    var j5 = String("{}")
    fails += check("json missing body", validate_body(sbytes(j5), j5.byte_length(), ob, 8192, olen) == BODY_REQUIRED)
    var big = String('{"body":"')
    for _i in range(501):
        big += "a"
    big += '"}'
    fails += check("json 501 chars", validate_body(sbytes(big), big.byte_length(), ob, 8192, olen) == BODY_TOO_LONG)
    var j6 = String('{"body":"ok"} trailing')
    fails += check("json trailing garbage", validate_body(sbytes(j6), j6.byte_length(), ob, 8192, olen) == BODY_MALFORMED)
    var j7 = String('{"body":"a\\x"}')
    fails += check("json bad escape", validate_body(sbytes(j7), j7.byte_length(), ob, 8192, olen) == BODY_MALFORMED)

    # writer + ints
    var w = Out(balloc(256), 256)
    w.put_int(0)
    w.put(0x20)
    w.put_int(1234567)
    w.put(0x20)
    w.put_json(sbytes(String('a"b\\c\n')), 6)
    var wb = balloc(w.n + 1)
    bcopy(wb, w.p, w.n)
    wb[unsafe_offset=w.n] = 0
    fails += check("writer", String(unsafe_from_utf8_ptr=wb) == '0 1234567 a\\"b\\\\c\\n')
    fails += check("parse_pos_int", parse_pos_int(sbytes(String("42")), 2) == 42 and parse_pos_int(sbytes(String("042")), 3) == -1 and parse_pos_int(sbytes(String("1.5")), 3) == -1)
    var vb = balloc(4)
    vb[unsafe_offset=0] = 0x81
    vb[unsafe_offset=1] = 0x02
    var vi = 0
    fails += check("varint", varint(vb, vi) == 130 and vi == 2)

    if fails == 0:
        print("ALL OK")
    else:
        print("FAILURES:", fails)

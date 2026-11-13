#!/usr/bin/env python3
"""Independent known-answer, randomized, canary and guard-page NASM primitive tests."""
import base64
import ctypes as C
import hashlib
import hmac
import json
import mmap
import pathlib
import random
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]
ROOT.joinpath("build").mkdir(exist_ok=True)
for name in ["crypto", "json"]:
    subprocess.run(["nasm", "-f", "elf64", "-g", "-F", "dwarf", str(ROOT / "src" / (name + ".asm")),
                    "-o", str(ROOT / "build" / (name + "-test.o"))], check=True)
subprocess.run(["nasm", "-f", "elf64", str(ROOT / "tests/abi.asm"),
                "-o", str(ROOT / "build/abi-test.o")], check=True)
subprocess.run(["gcc", "-shared", "-Wl,-Bsymbolic,-z,notext",
                str(ROOT / "build/crypto-test.o"), str(ROOT / "build/json-test.o"), str(ROOT / "build/abi-test.o"),
                "-o", str(ROOT / "build/primitives.so")], check=True)
lib = C.CDLL(str(ROOT / "build/primitives.so"))
lib.sha256.argtypes = [C.c_void_p, C.c_size_t, C.c_void_p]
lib.hmac_init.argtypes = [C.c_void_p, C.c_size_t]
lib.hmac_sha256.argtypes = lib.sha256.argtypes
lib.b64decode.argtypes = [C.c_void_p, C.c_size_t, C.c_void_p, C.c_size_t]
lib.b64decode.restype = C.c_ssize_t
lib.json_parse.argtypes = [C.c_void_p, C.c_size_t]
lib.json_quote.argtypes = [C.c_void_p, C.c_void_p, C.c_size_t]
lib.json_quote.restype = C.c_void_p
C.c_int.in_dll(lib,"quote_vector_enabled").value = 1
lib.abi_call.argtypes = [C.c_void_p] * 5
lib.numeric_date.argtypes = [C.c_void_p, C.c_size_t]
lib.numeric_date.restype = C.c_double
for data in [b"1791932885", b"1791932885.125", b"1.791932885e9", b"-1.25", b"0", b"1e-100"]:
    assert abs(lib.numeric_date(data, len(data)) - float(data)) <= max(abs(float(data)) * 1e-14, 1e-200), data
rng = random.Random(20261014)
out = C.create_string_buffer(64)
for n in list(range(260)) + [511, 512, 513, 1024, 4096, 8192]:
    data = rng.randbytes(n)
    out.raw = b"\xa5" * 64
    lib.sha256(data, n, C.addressof(out) + 16)
    assert out.raw[16:48] == hashlib.sha256(data).digest(), n
    assert out.raw[:16] == out.raw[48:] == b"\xa5" * 16
    encoded = base64.urlsafe_b64encode(data).rstrip(b"=")
    buf = C.create_string_buffer(n + 32)
    assert lib.b64decode(encoded, len(encoded), buf, n) == n
    assert buf.raw[:n] == data
    if n:
        assert lib.b64decode(encoded, len(encoded), buf, n - 1) == -1
for keylen in [0, 1, 32, 63, 64, 65, 100, 200]:
    key = rng.randbytes(keylen)
    lib.hmac_init(key, keylen)
    for n in [0, 1, 55, 56, 63, 64, 65, 1000, 8192]:
        data = rng.randbytes(n)
        lib.hmac_sha256(data, n, out)
        assert out.raw[:32] == hmac.digest(key, data, "sha256")
for bad in [b"A", b"AA=", b"+A", b"/A", b"AB", b"AAB", b"_", b"!!!!"]:
    assert lib.b64decode(bad, len(bad), out, 64) == -1, bad

def value(depth=0):
    options = [None, True, False, rng.randrange(-100000, 100000),
               rng.random(), "".join(chr(rng.choice([rng.randrange(32, 127), 0, 10, 233, 0x2713, 0x1f600]))
                                    for _ in range(rng.randrange(40)))]
    if depth < 4:
        options += [[value(depth + 1) for _ in range(rng.randrange(4))],
                    {"key" + str(i): value(depth + 1) for i in range(rng.randrange(4))}]
    return rng.choice(options)

for _ in range(2000):
    obj = value()
    data = json.dumps(obj, ensure_ascii=rng.choice([True, False]), separators=(",", ":")).encode()
    assert lib.json_parse(data, len(data)) > 0, data
    if isinstance(obj, str):
        raw = obj.encode()
        buf = C.create_string_buffer(6 * len(raw) + 34)
        C.memset(buf, 0xa5, len(buf))
        start = C.addressof(buf) + 16
        end = lib.json_quote(start, raw, len(raw))
        assert json.loads(C.string_at(start, end - start)) == obj
        assert buf.raw[:16] == b"\xa5" * 16
        assert buf.raw[end - C.addressof(buf):] == b"\xa5" * (len(buf) - end + C.addressof(buf))
for bad in [b"", b"{", b"{bad", b'{"a":01}', b"[1,]", b'{"a":true,}', b"NaN",
            b'"\xc0\x80"', b'"\\ud800"', b'"\\udc00"', b'"\xf4\x90\x80\x80"',
            b'"\xed\xa0\x80"', b'"\x01"', b'{"x":1} trailing']:
    assert lib.json_parse(bad, len(bad)) == -1, bad

# Inputs end immediately before a PROT_NONE page. Tail reads must stay in bounds.
page = mmap.PAGESIZE
mem = mmap.mmap(-1, page * 2)
address = C.addressof(C.c_char.from_buffer(mem))
libc = C.CDLL(None)
libc.mprotect.argtypes = [C.c_void_p, C.c_size_t, C.c_int]
assert libc.mprotect(address + page, page, 0) == 0
for n in range(130):
    data = rng.randbytes(n)
    C.memmove(address + page - n, data, n)
    lib.sha256(address + page - n, n, out)
    assert out.raw[:32] == hashlib.sha256(data).digest()
for data in [b"{}", b'"abc"', b'{"body":"caf\xc3\xa9"}', b'"\xf0\x9f\x98\x80"']:
    C.memmove(address + page - len(data), data, len(data))
    assert lib.json_parse(address + page - len(data), len(data)) > 0
# SIMD quote inputs end on a guard page, covering every block/tail boundary.
# Outputs also end at their documented maximum, before a protected page.
quote_mem = mmap.mmap(-1, page * 2)
quote_address = C.addressof(C.c_char.from_buffer(quote_mem))
assert libc.mprotect(quote_address + page, page, 0) == 0
for n in list(range(130)) + [255, 256, 257, 500]:
    for special in ['', '"', chr(92), chr(0), chr(31), 'é', '😀']:
        text = ('abc' * (n // 3 + 1))[:n]
        if n and special:
            pos = n // 2
            text = text[:pos] + special + text[pos+1:]
        raw = text.encode()
        C.memmove(address + page - len(raw), raw, len(raw))
        maximum = 6 * len(raw) + 2
        destination = quote_address + page - maximum
        C.memset(destination, 0xa5, maximum)
        end = lib.json_quote(destination, address + page - len(raw), len(raw))
        assert destination <= end <= quote_address + page
        assert json.loads(C.string_at(destination, end-destination)) == text
        assert C.string_at(end, quote_address+page-end) == b"\xa5" * (quote_address+page-end)
assert libc.mprotect(quote_address + page, page, 3) == 0
quote_mem.close()
assert libc.mprotect(address + page, page, 3) == 0
mem.close()
quote_abi_output=C.create_string_buffer(512)
for name, args in [
    ("json_quote", (C.addressof(quote_abi_output), C.cast(C.c_char_p(b"abcdefghijklmnop0123456789"), C.c_void_p), 26, 0)),
    ("sha256", (C.cast(C.c_char_p(b"abc"), C.c_void_p), 3, C.addressof(out), 0)),
    ("hmac_init", (C.cast(C.c_char_p(b"test"), C.c_void_p), 4, 0, 0)),
    ("hmac_sha256", (C.cast(C.c_char_p(b"abc"), C.c_void_p), 3, C.addressof(out), 0)),
    ("json_parse", (C.cast(C.c_char_p(b'{"body":"x"}'), C.c_void_p), 12, 0, 0)),
    ("b64decode", (C.cast(C.c_char_p(b"YWJj"), C.c_void_p), 4, C.addressof(out), 64)),
    ("numeric_date", (C.cast(C.c_char_p(b"123.5"), C.c_void_p), 5, 0, 0)),
]:
    assert lib.abi_call(C.cast(getattr(lib, name), C.c_void_p), *args) == 0, name
print("PASS: SHA/HMAC/base64url, NumericDate, 2000 JSON cases, ABI checks, canaries and guard-page tails")

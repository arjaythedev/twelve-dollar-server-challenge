#!/usr/bin/env python3
"""Compare Ada string parsing/escaping with Python's JSON oracle (no server)."""
import json
from pathlib import Path
import random
import subprocess
import tempfile

HERE = Path(__file__).resolve().parents[1]


def main():
    inputs, expected = [], []

    def valid(value, *, escaped=False, source=None):
        document = source or json.dumps({"body": value}, ensure_ascii=escaped)
        # The reference decoder also checks the hand-written documents.
        reference = json.loads(document)["body"]
        inputs.append(document.encode("utf-8"))
        expected.append(json.dumps(reference, ensure_ascii=False).encode("utf-8"))

    valid("")
    # Every ASCII byte and pair exercises both sides of escape/run boundaries.
    for a in range(128):
        valid(chr(a))
        for b in range(128):
            valid(chr(a) + chr(b))
    for text in ["λ", "😀", "café ✓", "\u2028\u2029", "\ufeff", "<>&/", "a" * 16000]:
        for escaped in (False, True):
            valid("prefix" + text + "suffix", escaped=escaped)
    rng = random.Random(20261026)
    alphabet = ''.join(map(chr, range(128))) + "λ😀é✓\u2003\uffff\U0010ffff"
    for i in range(1000):
        value = ''.join(rng.choice(alphabet) for _ in range(rng.randrange(600)))
        valid(value, escaped=bool(i % 2))
    for document in [
        '{"body":"first","body":"last"}',
        '{"ignored":{"body":"nested"},"body":"wanted","tail":[1,true,null]}',
        '{"b\\u006fdy":"escaped key","tail":{"body":false}}',
        '{"body":"a\\/b\\u0000c\\ud83d\\ude00d"}',
        '{"body":"text","tail":' + '[' * 100 + '0' + ']' * 100 + '}',
    ]:
        valid(None, source=document)

    invalid = [
        b'{"body":"unterminated}', b'{"body":"bad\\x"}',
        b'{"body":"\\uD800"}', b'{"body":"\\uDC00"}',
        b'{"body":"\\uD800\\u0041"}', b'{"body":"\\u12xz"}',
        b'{"body":"ok"} trailing', b'{"body":"ok","bad":[1,]}',
    ]
    invalid += [b'{"body":"prefix' + bytes([c]) + b'suffix"}'
                for c in range(32) if c not in (10, 13)]
    invalid += [b'{"body":"prefix' + sequence + b'suffix"}' for sequence in (
        b'\x80', b'\xc0\xaf', b'\xc2', b'\xe0\x80\xaf', b'\xed\xa0\x80',
        b'\xf0\x80\x80\xaf', b'\xf4\x90\x80\x80', b'\xff')]
    inputs.extend(invalid)
    expected.extend([b"INVALID"] * len(invalid))

    with tempfile.TemporaryDirectory(prefix="ada-json-") as tmp:
        binary = Path(tmp) / "json_probe"
        subprocess.run([
            "gnatmake-13", "-j1", "-O2", "-gnat2022", "-gnatwa", "-gnatVa",
            "-fstack-protector-strong", "-D", tmp, f"-I{HERE / 'src'}",
            str(HERE / "tests/json_probe.adb"), "-o", str(binary),
        ], check=True, cwd=tmp)
        result = subprocess.run([str(binary)], input=b"\n".join(inputs) + b"\n",
                                capture_output=True, check=True)
    # Split on LF only: valid JSON may contain literal Unicode line separators.
    actual = result.stdout.split(b"\n")[:-1]
    assert len(actual) == len(expected), (len(actual), len(expected), result.stderr)
    for i, (got, want) in enumerate(zip(actual, expected)):
        assert got == want, (i, inputs[i], got, want)
    print(f"Passed {len(expected)} JSON oracle cases")


if __name__ == "__main__":
    main()

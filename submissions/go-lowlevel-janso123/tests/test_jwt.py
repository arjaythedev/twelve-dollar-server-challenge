"""Run JWT regression cases against verbatim app source with only the Go standard library."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GO = shutil.which("go")


class JWTSource(unittest.TestCase):
    @unittest.skipUnless(GO, "JWT source checks require an installed Go toolchain")
    def test_authentication_source_contract(self):
        source = (ROOT / "app/api.go").read_text()
        attribution = source[:source.index("package main")]
        positive = source[source.index("func positive("):source.index("func space(")]
        authenticate = source[source.index("func (a *app) authenticate("):
                              source.index("func (a *app) handle(")]
        # Supply only imports and the app's secret field. The tested functions
        # stay byte-for-byte identical to production, including attribution.
        shell = '''package main
import ("crypto/hmac"; "crypto/sha256"; "encoding/base64"; "encoding/json";
        "strconv"; "strings"; "time"; "unicode/utf8")
type app struct { secret []byte }
'''
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            (directory / "go.mod").write_text("module jwt-source-tests\n\ngo 1.23\n")
            (directory / "auth.go").write_text(attribution + shell + positive + authenticate)
            for path in (ROOT / "tests/jwt").glob("*_test.go"):
                shutil.copyfile(path, directory / path.name)
            result = subprocess.run(
                [GO, "test", "-pgo=off", "-count=1", "-v", "./..."], cwd=directory,
                env=dict(os.environ, CGO_ENABLED="0", GOTOOLCHAIN="local", GOPROXY="off",
                         GOSUMDB="off", GOENV="off", GOWORK="off", GOFLAGS="", GOEXPERIMENT="",
                         GOTELEMETRY="off"), capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("--- PASS: TestStructuredClaimMutations", result.stdout)

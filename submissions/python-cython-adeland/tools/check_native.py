import json
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix='twelve-native-') as directory:
    binary = Path(directory)/'check-native'
    subprocess.run([
        'gcc', '-std=c11', '-O1', '-g', '-fsanitize=address,undefined',
        '-fno-omit-frame-pointer', '-Wno-unused-function', '-I'+str(root),
        '-I'+str(root/'_deps/llhttp/include'), '-I'+str(root/'_deps/yyjson/src'),
        str(root/'tools/check_native.c'), '-o', str(binary), '-lsqlite3', '-lcrypto',
    ], check=True)
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',
               UBSAN_OPTIONS='halt_on_error=1')
    subprocess.run([str(binary)], env=env, check=True, timeout=120)
    for mode, message in [('--overflow', 'use-after-poison'),
                          ('--uaf', 'use-after-poison'), ('--double-free', None)]:
        result = subprocess.run([str(binary), mode], env=env, capture_output=True,
                                text=True, timeout=30)
        assert result.returncode != 0, (mode, result.stdout, result.stderr)
        if message:
            assert message in result.stderr, (mode, result.stderr)
        print(json.dumps({'negative_control': mode, 'exit_code': result.returncode,
                          'detected': message or 'ownership invariant'}), flush=True)

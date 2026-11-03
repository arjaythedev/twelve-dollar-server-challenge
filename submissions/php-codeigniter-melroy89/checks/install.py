#!/usr/bin/env python3
"""Exercise installer failure paths in a temporary sandbox, without root or network."""
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'install.sh').read_text()
payloads = {'composer': b'verified composer\n', 'frankenphp': b'verified runtime\n'}

with tempfile.TemporaryDirectory(prefix='ci4-install-check-') as directory:
    root = Path(directory)
    target = root / 'installed'
    target.mkdir()
    mocks = root / 'mocks'
    mocks.mkdir()
    (mocks / 'apt-get').write_text('#!/bin/sh\nexit 0\n')
    (mocks / 'curl').write_text('''#!/usr/bin/env python3
import os
from pathlib import Path
import sys
args = sys.argv[1:]
out = Path(args[args.index('-o') + 1])
name = out.name
mode = os.environ['INSTALL_CHECK_MODE']
if mode == 'download-failure' and name == 'frankenphp':
    out.write_bytes(b'partial download')
    sys.exit(18)
if mode == 'checksum-failure' and name == 'frankenphp':
    out.write_bytes(b'corrupted runtime')
else:
    out.write_bytes(b'verified composer\\n' if name == 'composer' else b'verified runtime\\n')
''')
    for mock in mocks.iterdir():
        mock.chmod(0o755)

    # Redirect installation paths and pins to local fixtures; execute the actual script.
    script = source.replace('/usr/local/bin', str(target))
    for name, data in payloads.items():
        variable = name.upper() + '_SHA256='
        lines = script.splitlines()
        script = '\n'.join(variable + hashlib.sha256(data).hexdigest()
                           if line.startswith(variable) else line for line in lines) + '\n'
    installer = root / 'install.sh'
    installer.write_text(script)

    for mode in ('download-failure', 'checksum-failure', 'success'):
        for name in payloads:
            (target / name).write_bytes(b'existing executable\n')
        environment = {**os.environ, 'PATH': str(mocks) + os.pathsep + os.environ['PATH'],
                       'INSTALL_CHECK_MODE': mode}
        result = subprocess.run(['bash', str(installer)], env=environment,
                                capture_output=True, text=True, timeout=10)
        assert (result.returncode == 0) == (mode == 'success'), result.stderr
        for name, data in payloads.items():
            expected = data if mode == 'success' else b'existing executable\n'
            assert (target / name).read_bytes() == expected, (mode, name)
            if mode == 'success':
                assert (target / name).stat().st_mode & 0o777 == 0o755
        assert not list(target.glob('.ci4-install.*')), (mode, 'staging directory leaked')
        if mode == 'checksum-failure':
            assert 'Expected:' in result.stderr and 'Actual:' in result.stderr, result.stderr
        print(f'PASS: {mode}; executables and staging cleanup verified')

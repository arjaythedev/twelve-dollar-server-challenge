from pathlib import Path
import hashlib
import io
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parent.parent
DEPS = [
    ('llhttp', 'https://codeload.github.com/nodejs/llhttp/tar.gz/refs/tags/release/v9.3.0', '1a2b45cb8dda7082b307d336607023aa65549d6f060da1d246b1313da22b685a'),
    ('yyjson', 'https://codeload.github.com/ibireme/yyjson/tar.gz/refs/tags/0.12.0', 'b16246f617b2a136c78d73e5e2647c6f1de1313e46678062985bdcf1f40bb75d'),
]
for name, url, expected in DEPS:
    dest = ROOT / '_deps' / name
    if (dest / '.source-sha256').exists() and (dest / '.source-sha256').read_text().strip() == expected:
        continue
    data = urllib.request.urlopen(url, timeout=60).read()
    if hashlib.sha256(data).hexdigest() != expected:
        raise RuntimeError(f'{name} source checksum mismatch')
    dest.mkdir(parents=True, exist_ok=True)
    with tarfile.open(fileobj=io.BytesIO(data), mode='r:gz') as archive:
        for member in archive.getmembers():
            pieces = Path(member.name).parts[1:]
            if not pieces:
                continue
            member.name = str(Path(*pieces))
            archive.extract(member, dest, filter='data')
    (dest / '.source-sha256').write_text(hashlib.sha256(data).hexdigest() + '\n')

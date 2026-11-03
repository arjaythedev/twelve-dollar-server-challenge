#!/usr/bin/env python3
"""Checksum-pinned SQLite build with verified content-addressed cache and provenance."""
import ctypes, hashlib, json, os, pathlib, shutil, subprocess, urllib.request, zipfile
root = pathlib.Path(__file__).resolve().parent
cache = root / '.tools/sqlite-native'
cache.mkdir(parents=True, exist_ok=True)
archive = cache / 'sqlite-amalgamation-3530400.zip'
expected = '1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d'
def digest(path): return hashlib.sha256(path.read_bytes()).hexdigest()
if not archive.exists() or digest(archive) != expected:
    temporary = archive.with_suffix('.download')
    urllib.request.urlretrieve('https://www.sqlite.org/2026/' + archive.name, temporary)
    if digest(temporary) != expected: raise RuntimeError('SQLite archive checksum mismatch')
    temporary.replace(archive)
with zipfile.ZipFile(archive) as z:
    source = z.read('sqlite-amalgamation-3530400/sqlite3.c')
    header = z.read('sqlite-amalgamation-3530400/sqlite3.h')
compiler = shutil.which('gcc')
flags = ['-O2', '-fPIC', '-shared']
# This campaign measures compile flags one at a time against the retained build.
macros = ['-DSQLITE_THREADSAFE=2']
inputs = dict(version='3.53.4', archive_sha256=expected,
              source_sha256=hashlib.sha256(source).hexdigest(), header_sha256=hashlib.sha256(header).hexdigest(),
              compiler=compiler, compiler_sha256=digest(pathlib.Path(compiler)),
              compiler_version=subprocess.check_output([compiler, '--version'], text=True),
              flags=flags, macros=macros, libraries=['-lpthread', '-lm'],
              builder_sha256=digest(pathlib.Path(__file__)))
key = hashlib.sha256(json.dumps(inputs, sort_keys=True).encode()).hexdigest()
work = cache / key
work.mkdir(exist_ok=True)
library = work / 'libsqlite3.so'
manifest = work / 'manifest.json'
valid = False
if manifest.exists() and library.exists():
    prior = json.loads(manifest.read_text())
    valid = prior.get('inputs') == inputs and prior.get('library_sha256') == digest(library)
if not valid:
    (work / 'sqlite3.c').write_bytes(source)
    command = [compiler, *flags, '-o', str(library), str(work / 'sqlite3.c'), *macros, '-lpthread', '-lm']
    subprocess.run(command, check=True)
    lib = ctypes.CDLL(str(library))
    lib.sqlite3_libversion.restype = ctypes.c_char_p
    lib.sqlite3_sourceid.restype = ctypes.c_char_p
    lib.sqlite3_compileoption_get.restype = ctypes.c_char_p
    options = []
    for i in range(1000):
        option = lib.sqlite3_compileoption_get(i)
        if option is None: break
        options.append(option.decode())
    if lib.sqlite3_libversion().decode() != inputs['version']: raise RuntimeError('Native version mismatch')
    manifest.write_text(json.dumps(dict(inputs=inputs, build_key=key, command=command,
        library_sha256=digest(library), native_version=lib.sqlite3_libversion().decode(),
        native_source_id=lib.sqlite3_sourceid().decode(), compile_options=options), indent=2) + '\n')
output = root / 'target/native'
output.mkdir(parents=True, exist_ok=True)
shutil.copyfile(library, output / library.name)
shutil.copyfile(manifest, output / manifest.name)
print('SQLite native build:', key, digest(output / library.name))

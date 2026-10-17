#!/usr/bin/env python3
"""Read-only pinned-source collection. No contender code is executed here."""
import argparse
import concurrent.futures
import hashlib
import json
from pathlib import Path
import subprocess
import urllib.parse
import urllib.request

HERE = Path(__file__).resolve().parents[1]
REPO = 'arjaythedev/twelve-dollar-server-challenge'
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--fetch', default='9,19,26,32,42,51,55,60', help='comma-separated PRs, or all')
p.add_argument('--inventory', type=Path, default=HERE / 'results/competitors.json', help='captured immutable PR inventory')
a = p.parse_args()
record = json.loads(a.inventory.read_text())
fetch = {e['number'] for e in record['entries']} if a.fetch == 'all' else set(map(int, a.fetch.split(',')))
out = HERE / 'results/field'
out.mkdir(parents=True, exist_ok=True)
def api(endpoint):
    return json.loads(subprocess.check_output(['gh', 'api', endpoint], text=True))
def collect(entry):
    number, sha = entry['number'], entry['head_sha']
    metadata = out / (str(number) + '-' + sha[:12])
    metadata.mkdir(exist_ok=True)
    manifest_file = metadata / 'manifest.json'
    if manifest_file.exists():
        manifest = json.loads(manifest_file.read_text())
    else:
        current = api(f'repos/{REPO}/pulls/{number}')
        if current['head']['sha'] != sha:
            raise RuntimeError(f'PR {number} moved: refresh inventory before collection')
        files = []
        for page in range(1, 100):
            batch = api(f'repos/{REPO}/pulls/{number}/files?per_page=100&page={page}')
            files.extend(batch)
            if len(batch) < 100: break
        if api(f'repos/{REPO}/pulls/{number}')['head']['sha'] != sha:
            raise RuntimeError(f'PR {number} moved during collection')
        manifest = dict(number=number, sha=sha, repository=entry['head_repository'],
                        changed_files=files, source_files={},
                        eligibility='not independently screened', build='not run', official_tests='not run', capacity='not run')
    if number in fetch:
        owner = manifest['repository'] or REPO
        if 'submission_tree' not in manifest:
            folders = {str(Path(f['filename']).parent) for f in manifest['changed_files'] if f['filename'].startswith('submissions/')}
            folders = {'/'.join(path.split('/')[:2]) + '/' for path in folders}
            tree = api(f'repos/{owner}/git/trees/{sha}?recursive=1')
            if tree.get('truncated'): raise RuntimeError('truncated Git tree')
            manifest['submission_tree'] = [f for f in tree['tree'] if f['type'] == 'blob' and any(f['path'].startswith(folder) for folder in folders)]
        for f in manifest['submission_tree']:
            path = f['path']
            # Fetch every current file in affected submission directories.
            if f['mode'] == '120000': raise RuntimeError('symlink requires explicit source review')
            parts = Path(path).parts
            if '..' in parts or Path(path).is_absolute(): raise RuntimeError('unsafe source path')
            target = metadata / 'source' / path
            if not target.exists():
                url = 'https://raw.githubusercontent.com/' + owner + '/' + sha + '/' + urllib.parse.quote(path)
                with urllib.request.urlopen(url, timeout=30) as response: data = response.read(8 * 1024 * 1024 + 1)
                if len(data) > 8 * 1024 * 1024: raise RuntimeError('unexpected large source artifact')
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data)
            if f['mode'] == '100755': target.chmod(0o755)
            manifest['source_files'][path] = hashlib.sha256(target.read_bytes()).hexdigest()
    manifest_file.write_text(json.dumps(manifest, indent=2) + '\n')
    return dict(number=number, sha=sha, changes=len(manifest['changed_files']), fetched=len(manifest['source_files']))
with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
    for result in pool.map(collect, record['entries']): print(json.dumps(result), flush=True)

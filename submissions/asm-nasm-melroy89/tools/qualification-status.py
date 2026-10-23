#!/usr/bin/env python3
"""Reconcile local pinned source collection with separate qualification evidence."""
import argparse,hashlib,json
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--inventory',type=Path,default=ROOT/'results/competitors.json')
args=parser.parse_args()
inventory=json.loads(args.inventory.read_text())
rows=[]
for entry in sorted(inventory['entries'],key=lambda e:e['number']):
    p=ROOT/'results/field'/(str(entry['number'])+'-'+entry['head_sha'][:12])/'manifest.json'
    missing=not p.exists()
    m=json.loads(p.read_text()) if not missing else dict(number=entry['number'],sha=entry['head_sha'],source_files={})
    assert m['sha']==entry['head_sha'], 'manifest revision mismatch'
    folders=sorted({'/'.join(n.split('/')[:2]) for n in m['source_files'] if n.startswith('submissions/')})
    outside=[f['filename'] for f in m.get('changed_files',[]) if not f['filename'].startswith('submissions/')]
    missing_entrypoints=[folder+'/'+name for folder in folders for name in ['install.sh','build.sh','start.sh','README.md'] if folder+'/'+name not in m['source_files']]
    drift=[]
    for name,wanted in m['source_files'].items():
        f=p.parent/'source'/name
        if not f.is_file() or f.is_symlink() or hashlib.sha256(f.read_bytes()).hexdigest()!=wanted:
            drift.append(name)
    qfile=p.parent/'qualification.json'
    q=json.loads(qfile.read_text()) if qfile.exists() else {}
    matches=bool(q) and q.get('head_sha')==m['sha'] and not drift
    rows.append(dict(pr=m['number'],head=m['sha'],source_files=len(m['source_files']),
                     source_drift=drift,collection_missing=missing,submission_folders=folders,changed_outside_submissions=outside,missing_entrypoints=missing_entrypoints,qualification_revision_matches=matches,
                     build=q.get('build','not run') if matches else 'not verified',
                     official_tests=q.get('official_tests','not run') if matches else 'not verified',
                     eligibility='not independently cleared',
                     maximum_capacity='not established',
                     image=q.get('image') if matches else None))
report=dict(inventory=str(args.inventory),retrieved_at_utc=inventory.get('retrieved_at_utc'),boundary='Captured inventory pins only; remote heads may change after capture. Official tests are not eligibility clearance or maximum-capacity evidence.',rows=rows)
(ROOT/'results/qualification-status.json').write_text(json.dumps(report,indent=2)+'\n')
lines=['# Local qualification status','','This report reconciles captured source hashes and separate qualification.json',
       'records by exact pinned revision. Collection manifests describe their initial',
       'state and may predate qualification. Only heads in the selected inventory are',
       'included; older captured revisions do not duplicate current PR rows.',
       'Passing the official suite does not independently clear eligibility or prove',
       'maximum capacity. No additional contender code was executed.','',
       '| PR | Captured revision | Source files | Source hashes | Build | Official tests |',
       '| --- | --- | ---: | --- | --- | --- |']
for x in rows:
    lines.append(f"| #{x['pr']} | {x['head'][:12]} | {x['source_files']} | {'DRIFT' if x['source_drift'] else 'verified' if x['source_files'] else 'none'} | {x['build']} | {x['official_tests']} |")
lines+=['','All whole-field eligibility and maximum-capacity decisions remain open.',
        'See field.md and PERFORMANCE.md for individual source observations and',
        'matched short-screen results. Regenerate with python3 tools/qualification-status.py.','']
lines+=['## Static packaging review queue','','These are source observations, not automatic disqualifications. Root install,', 'normal-user build, four-variable runtime configuration and source-only deployment', 'still require manual/runtime review. No contender code was executed.','']
for x in rows:
    flags=[]
    if len(x['submission_folders'])!=1:flags.append(str(len(x['submission_folders']))+' captured submission folders')
    if x['changed_outside_submissions']:flags.append('changes outside submissions: '+', '.join(x['changed_outside_submissions']))
    if x['missing_entrypoints']:flags.append('missing captured entrypoints: '+', '.join(x['missing_entrypoints']))
    if flags:lines.append('- #'+str(x['pr'])+': '+'; '.join(flags))
(ROOT/'research/qualification-status.md').write_text('\n'.join(lines))
print(json.dumps(dict(inventory_entries=len(rows),missing=sum(x['collection_missing'] for x in rows),drifted=sum(bool(x['source_drift']) for x in rows),qualified=sum(x['official_tests']=='passed' for x in rows))))

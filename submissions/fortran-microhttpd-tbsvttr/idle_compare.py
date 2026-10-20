import argparse,json,re,subprocess,time
from pathlib import Path
root=Path(__file__).resolve().parents[2]
results=[]
def cleanup(process,command):
    if process is not None and process.poll() is None:
        try:process.communicate(input=command,timeout=20)
        except subprocess.TimeoutExpired:process.kill();process.wait()
folders={'cpp':'cpp-uwebsockets-v2-tbsvttr','fortran':'fortran-microhttpd-tbsvttr','rust':'rust-axum-tbsvttr','go':'go-nethttp-tbsvttr'}
parser=argparse.ArgumentParser(description='Four-server test with 15,000 idle keep-alive sockets and 64 active mixed-load connections')
parser.add_argument('--output',required=True)
output_path=Path(parser.parse_args().output)
output_path.parent.mkdir(parents=True,exist_ok=True)
if output_path.exists(): raise RuntimeError('output exists')
for index,(name,folder) in enumerate(folders.items()):
    port=21200+index
    server=idle=None
    try:
        server=subprocess.Popen(['docker','exec','-i','twelve-language-server','python3','/work/submissions/fortran-microhttpd-tbsvttr/compare.py','serve','--binary','/work/submissions/'+folder+'/bin/server','--port',str(port)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        ready=server.stdout.readline()
        if not ready or not json.loads(ready).get('ready'):raise RuntimeError(ready+server.stderr.read())
        idle=subprocess.Popen(['docker','exec','-i','twelve-language-loadgen','python3','/work/submissions/fortran-microhttpd-tbsvttr/tests/idle_client.py',str(port)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        idle_ready=idle.stdout.readline()
        if not idle_ready or not json.loads(idle_ready).get('ready'):raise RuntimeError(idle_ready+idle.stderr.read())
        print(name,'15000 idle keep-alives ready',flush=True)
        args=['docker','exec','-w','/work','twelve-language-loadgen','wrk','-t2','-c64','--latency','-s','/work/submissions/fortran-microhttpd-tbsvttr/bench.lua']
        url=f'http://127.0.0.1:{port}/feed'
        subprocess.run(args+['-d2s',url],stdout=subprocess.DEVNULL,check=True)
        start=time.time()
        output=subprocess.check_output(args+['-d8s',url],text=True,timeout=25)
        elapsed=time.time()-start
        assert 7.8<=elapsed<=12,elapsed
        server.stdin.write('metrics\n');server.stdin.flush()
        metrics=json.loads(server.stdout.readline())
        idle.stdin.write('verify\n');idle.stdin.flush()
        verified=idle.stdout.readline()
        if not verified:raise RuntimeError(idle.stderr.read())
        verified=json.loads(verified)
        assert verified=={'verified':15000,'same_sockets':True},verified
        idle.stdin.write('stop\n');idle.stdin.flush()
        tail,error=idle.communicate(timeout=20)
        if idle.returncode:raise RuntimeError(error)
        server.stdin.write('stop\n');server.stdin.flush()
        tail,error=server.communicate(timeout=20)
        if server.returncode:raise RuntimeError(error)
        duration=re.search(r' requests in ([0-9.]+)([a-z]+),',output)
        assert duration and duration[2]=='s' and 7.5 <= float(duration[1]) <= 12, ('Invalid wrk elapsed time',output)
        record=dict(name=name,workload='mixed',trial=1,idle_connections=15000,active_connections=64,metrics=metrics,verification=verified,host_elapsed=elapsed,output=output)
        results.append(record)
        output_path.write_text(json.dumps(results,indent=2)+'\n')
        print(name,next(line for line in output.splitlines() if line.startswith('Requests/sec:')),metrics,verified,flush=True)
        if 'Non-2xx' in output or 'Socket errors' in output:raise RuntimeError(output)
    finally:
        cleanup(idle,'stop\n')
        cleanup(server,'stop\n')
print('Results:',output_path,flush=True)

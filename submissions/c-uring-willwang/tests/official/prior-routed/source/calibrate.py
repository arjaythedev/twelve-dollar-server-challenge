"""Match the published network RTT before measuring application load."""
import argparse
import json
from pathlib import Path
import statistics
import sys
import time
sys.path.insert(0,str(Path(__file__).parent))
from private_net import Link,TC,checked

if __name__=="__main__":
    ap=argparse.ArgumentParser()
    ap.add_argument("--after",type=Path)
    ap.add_argument("--out",type=Path,required=True)
    a=ap.parse_args()
    if a.after:
        stop=time.monotonic()+600
        while not a.after.is_file() and time.monotonic()<stop:
            time.sleep(1)
        if not a.after.is_file():
            raise RuntimeError("timeout waiting for preceding run")
        time.sleep(4)
    link=Link(10,[{0,1,2},{3,4,5},{6,7,8},{11,12,13}],1,45)
    rounds={}
    delays=[0,15,30,45,60,80,100]
    current=45
    try:
        for delay in delays+list(reversed(delays)):
            for iface in ("rserver","rclient"):
                if delay:
                    link.router.setup(TC,"qdisc","replace","dev",iface,"root","netem",
                        "delay",f"{delay}us","limit","32768")
                elif current:
                    link.router.setup(TC,"qdisc","del","dev",iface,"root")
            current=delay
            data=json.loads(checked(link.client.cmd(["/usr/bin/taskset","-c","0",sys.executable,
                Path(__file__).parent/"icmp.py",link.targets[0][0]])))[1:]
            rounds.setdefault(delay,[]).append(data)
        center={d:statistics.median([p for r in values for p in r]) for d, values in rounds.items()}
        best=min(delays,key=lambda d:abs(center[d]-.25))
        a.out.write_text(json.dumps({"target_ms":.25,"recommended_each_hop_us":best,
            "median_by_delay_ms":center,"samples_by_delay_round_ms":rounds,
            "router_cpus":[9,14],"server_cpu":10,"generator_cpu":0},indent=2))
        print("selected delay, medians",best,center,flush=True)
    finally:
        link.close()

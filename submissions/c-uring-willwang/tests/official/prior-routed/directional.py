"""Assign one previously reserved middle-router RPS CPU to each direction."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time

HERE=Path(__file__).resolve().parent
CONFIG={
    "tserver":{"role":"server","mask":"400"},
    "tclient":{"role":"client","mask":"39ff"},
    "rserver":{"role":"router","mask":"200"},
    "rclient":{"role":"router","mask":"4000"},
}
NAMES={"server":["tserver"],"client":["tclient"],"router":["rserver","rclient"]}
PROFILE="historical-single-middle-router-directional-rps"


def expected_mask(name):
    return int(CONFIG[name]["mask"],16)


def parse_mask(value):
    return int(value.strip().replace(",",""),16)


def packet_counts(text):
    return {int(k):int(v) for k,v in re.findall(r"(\d+):(\d+),",text.split("incoming_cpu=",1)[1].split()[0])}


def verify(out):
    proof=json.loads((out/"directional-rps.json").read_text())
    digest=hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    if proof["adapter_sha256"]!=digest:
        raise ValueError("directional RPS helper differs from the measured source")
    if (out/"shape.json").exists():
        shape=json.loads((out/"shape.json").read_text())
        if shape["topology"]!=PROFILE or shape["adapter_sha256"]["directional.py"]!=digest:
            raise ValueError("shape does not fingerprint this directional RPS helper")
    if proof["requested"]!=CONFIG or proof["profile"]!=PROFILE or proof["scored_cpu_budget"]!=list(range(16)):
        raise ValueError("directional profile identity or CPU budget changed")
    if set(proof["actual_queues"])!=set(CONFIG):
        raise ValueError("expected four directional RX interface readings")
    namespaces=proof["private_namespaces"]
    if set(namespaces)!=set(NAMES) or len(set(namespaces.values()))!=3 or proof["initial_net_ns"] in namespaces.values():
        raise ValueError("initial and private namespace identity disagreement")
    for name,target in CONFIG.items():
        record=proof["actual_queues"][name]
        if (record["role"]!=target["role"] or record["ns_inode"]!=namespaces[target["role"]] or
            record["mtu"]!=1500 or not record["queues"] or
            any(parse_mask(mask)!=expected_mask(name) for mask in record["queues"].values())):
            raise ValueError(f"wrong MTU, CPU mask or namespace for {name}")
    expected=[("rserver","200"),("rclient","4000")]
    if [tuple(x) for x in proof["during_creation"]]!=expected:
        raise ValueError("router steering changed outside the intended interface creations")
    poses=json.loads((out/"placements.json").read_text())
    if [p["lane"] for p in poses["lanes"]]!=[0,1,2,3]:
        raise ValueError("four service paths were not proved")
    allowed=set(i for i in range(16) if expected_mask("tclient") & 1<<i)
    for i,p in enumerate(poses["lanes"]):
        server=packet_counts(p["server"]);client=packet_counts(p["client"])
        client_cpu=[0,3,6,11][i]
        if (server!={10:24} or sum(client.values())!=24 or set(client)-allowed
            or p["client_process_cpu"]!=client_cpu
            or not re.search(rf"\bside=client process_cpu={client_cpu} mtu=1500\b",p["client"])
            or not re.search(r"\bside=server process_cpu=10 mtu=1500\b",p["server"])
            or p["target"]!=f"10.240.{i+1}.1" or p["source"]!=f"10.243.{i+1}.1"):
            raise ValueError(f"endpoint socket CPU or target placement changed on lane {i}")
    return proof


def ns_sysfs_dump(holder,interfaces):
    # Host sysfs can remain tagged with its original network namespace even after
    # setns(). This subprocess also enters a private, nonpropagating mount namespace
    # and mounts a standard read-only sysfs scoped to the already-checked netns.
    # Both namespace/mount changes affect only this short-lived subprocess.
    reader="""import ctypes,json,os,pathlib,sys
lib=ctypes.CDLL(None,use_errno=True)
def mount(src,dst,typ,flags):
    if lib.mount(src,dst,typ,ctypes.c_ulong(flags),None):
        e=ctypes.get_errno();raise OSError(e,os.strerror(e),dst)
inode=int(sys.argv[1]); root=pathlib.Path(sys.argv[2])
if os.stat('/proc/self/ns/net').st_ino!=inode: raise ValueError('network namespace changed')
mount(b'sysfs',os.fsencode(root),b'sysfs',1|2|4|8)
out={}
for name in sys.argv[3:]:
    device=root/'class/net'/name
    queues={p.parent.name:p.read_text().strip() for p in sorted((device/'queues').glob('rx-*/rps_cpus'))}
    out[name]=dict(ns_inode=inode,queues=queues,mtu=int((device/'mtu').read_text()))
print(json.dumps(out))"""
    with tempfile.TemporaryDirectory(prefix="priorroute-private-sysfs-") as tmp:
        cmd=[*holder.ns,"/usr/bin/unshare","--mount","--propagation","private","--",sys.executable,"-c",reader,
             str(holder.inode),tmp,*interfaces]
        return json.loads(subprocess.check_output(cmd,text=True,timeout=6))


def install(module,output):
    normal_setup=module.Holder.setup
    normal_place=module.Link.placement
    created=[]
    def assigned(holder,binary,*args):
        if (holder.name=="router" and binary==module.IP and tuple(args[:2])==("link","add")
            and len(args)>=5 and args[2] in ("rserver","rclient") and tuple(args[3:5])==("type","veth")):
            name=args[2]
            mask=CONFIG[name]["mask"]
            normal_setup(holder,"/usr/sbin/sysctl","-qw","net.core.rps_default_mask="+mask)
            created.append((name,mask))
        return normal_setup(holder,binary,*args)
    def measured(link,cpu,groups):
        original=normal_place(link,cpu,groups)
        actual={}
        nss={}
        for holder,names in [(link.server,NAMES["server"]),(link.client,NAMES["client"]),(link.router,NAMES["router"])]:
            nss[holder.name]=holder.inode
            for name,value in ns_sysfs_dump(holder,names).items():
                actual[name]={**value,"role":holder.name}
        from observe import dump
        proof={"profile":PROFILE,"initial_net_ns":os.stat("/proc/self/ns/net").st_ino,"private_namespaces":nss,
               "requested":CONFIG,"actual_queues":actual,"during_creation":created,
               "scored_cpu_budget":sorted(set.union(*groups)|{9,10,14,15}),"unix":time.time(),
               "adapter_sha256":hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
        dump(output/"directional-rps.json",proof)
        verify(output)
        return original
    module.Holder.setup=assigned
    module.Link.placement=measured

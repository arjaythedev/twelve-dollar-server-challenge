"""A private client/server link. Only fixed namespace setup commands run as root.

Both namespace holders, all probes, every generator and the benchmark server run as the
calling user with no capabilities. The host keeps its own IP, qdisc, and steering setup.
"""
import contextlib
import json
import os
from pathlib import Path
import re
import subprocess as sp
import statistics
import sys
import tempfile
import time

HERE = Path(__file__).resolve().parent
SUDO = ["/usr/bin/sudo", "-n"]
IP = "/usr/sbin/ip"
TC = "/usr/sbin/tc"
SOCKET_RTT = 30
ETH="/usr/sbin/ethtool"
ROUTER_CORES={9,14}
DROP = ["/usr/bin/setpriv", "--reuid", str(os.getuid()), "--regid", str(os.getgid()),
        "--clear-groups", "--inh-caps=-all", "--ambient-caps=-all", "--bounding-set=-all", "--no-new-privs", "--"]

def checked(cmd, **kwargs):
    return sp.check_output(list(map(str, cmd)), text=True, timeout=10, **kwargs).strip()

class Holder:
    def __init__(self, directory, name):
        self.name = name
        marker = Path(directory) / name
        # EOF closes the namespace even if the manager exits unexpectedly.
        code = "import os,sys; open(sys.argv[1],'x').write(str(os.getpid())); os.read(0,1)"
        self.proc = sp.Popen([*SUDO, "/usr/bin/unshare", "--net", "--", *DROP, sys.executable,
                              "-c", code, str(marker)], stdin=sp.PIPE)
        for _ in range(300):
            if marker.exists() and marker.stat().st_size:
                break
            if self.proc.poll() is not None:
                raise RuntimeError(f"{name} private namespace failed to start")
            time.sleep(.025)
        self.pid = int(marker.read_text())
        self.path = f"/proc/{self.pid}/ns/net"
        self.inode = os.stat(self.path).st_ino
        if self.inode == os.stat("/proc/self/ns/net").st_ino:
            raise RuntimeError("cannot operate in host network namespace")
        self.ns = [*SUDO, "/usr/bin/nsenter", "--net=" + self.path, "--"]
        if [r["ifname"] for r in json.loads(checked([*self.ns, IP, "-j", "link"]))] != ["lo"]:
            raise RuntimeError("private namespace was not empty")

    def cmd(self, argv):
        return [*self.ns, *DROP, *map(str, argv)]

    def setup(self, binary, *argv):
        if self.proc.poll() is not None or os.stat(self.path).st_ino != self.inode:
            raise RuntimeError("namespace holder changed")
        if binary not in {IP, TC, ETH, "/usr/sbin/sysctl"}:
            raise ValueError("only fixed network setup tools permitted")
        return checked([*self.ns, binary, *argv])

    def close(self):
        self.proc.stdin.close()
        self.proc.wait(timeout=10)

class Link:
    """All clients use their own target subnet via one veth, all listeners bind in one namespace."""
    def __init__(self, server_cpu, groups, aliases, delay_us=SOCKET_RTT):
        if os.getuid() == 0:
            raise RuntimeError("run the benchmark harness as a non-root user")
        self.hold = contextlib.ExitStack()
        try:
            directory = self.hold.enter_context(tempfile.TemporaryDirectory(prefix="twelve-nets-"))
            self.server = Holder(directory, "server")
            self.hold.callback(self.server.close)
            self.client = Holder(directory, "client")
            self.hold.callback(self.client.close)
            self.router = Holder(directory, "router")
            self.hold.callback(self.router.close)
            endpoints=(self.server,self.client,self.router)
            if len({h.inode for h in endpoints})!=3 or ROUTER_CORES & ({server_cpu}|set.union(*groups)):
                raise RuntimeError("need distinct network namespaces and disjoint router cores")
            if not 0 <= delay_us <= 500 or not 1 <= aliases <= 32 or not 1 <= len(groups) <= 8:
                raise ValueError("invalid private network size/delay")
            self.meta = {"namespaces": {h.name:h.inode for h in endpoints},
                         "delay_per_router_egress_us": delay_us, "router_cpus":sorted(ROUTER_CORES),
                         "endpoint_netem": False, "ip_forwarding": "private router only"}
            mask_groups=[{server_cpu},set.union(*groups),ROUTER_CORES]
            for h,cpus in zip(endpoints,mask_groups):
                mask=sum(1<<c for c in cpus)
                h.setup("/usr/sbin/sysctl","-qw",f"net.core.rps_default_mask={mask:x}")
                h.setup(IP,"link","set","lo","up")
            self.router.setup("/usr/sbin/sysctl","-qw","net.ipv4.ip_forward=1")
            # Allow this unprivileged benchmark user to issue ICMP datagram pings in
            # its private generator namespace, without CAP_NET_RAW or changing host policy.
            self.client.setup("/usr/sbin/sysctl","-qw",f"net.ipv4.ping_group_range={os.getgid()} {os.getgid()}")
            self.router.setup(IP,"link","add","rserver","type","veth","peer","name","tserver","netns",str(self.server.pid))
            self.router.setup(IP,"link","add","rclient","type","veth","peer","name","tclient","netns",str(self.client.pid))
            self.interfaces = [(self.server,"tserver"),(self.client,"tclient"),
                               (self.router,"rserver"),(self.router,"rclient")]
            features=("rx-checksumming","tx-checksumming","scatter-gather","tcp-segmentation-offload",
                      "generic-segmentation-offload","generic-receive-offload","large-receive-offload",
                      "rx-gro-list","rx-gro-hw","rx-udp-gro-forwarding","receive-hashing")
            def dump(h,iface):
                out=checked(h.cmd([ETH,"-k",iface]))
                return dict((k,v.strip()) for line in out.splitlines()
                           for k,sep,v in [line.strip().partition(":")] if k in features)
            self.meta["nic_features_before"]={}
            self.meta["nic_features_after"]={}
            for h,iface in self.interfaces:
                self.meta["nic_features_before"][iface]=dump(h,iface)
                h.setup(ETH,"-K",iface,"gro","on")
                self.meta["nic_features_after"][iface]=dump(h,iface)
                if not self.meta["nic_features_after"][iface]["generic-receive-offload"].startswith("on"):
                    raise RuntimeError("GRO unavailable on private device "+iface)
                h.setup(IP,"link","set",iface,"mtu","1500","up")
            # Two point-to-point subnets; no default route and no NAT in any namespace.
            for h,iface,addr in [(self.server,"tserver","10.241.0.1/30"),(self.router,"rserver","10.241.0.2/30"),
                                 (self.client,"tclient","10.242.0.1/30"),(self.router,"rclient","10.242.0.2/30")]:
                h.setup(IP,"addr","add",addr,"dev",iface)
            self.server.setup(IP,"route","add","10.243.0.0/16","via","10.241.0.2","dev","tserver")
            self.router.setup(IP,"route","add","10.240.0.0/16","via","10.241.0.1","dev","rserver")
            self.router.setup(IP,"route","add","10.243.0.0/16","via","10.242.0.1","dev","rclient")
            self.targets = []
            for index, group in enumerate(groups, 1):
                source=f"10.243.{index}.1"
                self.client.setup(IP,"addr","add",source+"/32","dev","tclient")
                self.client.setup(IP,"route","add",f"10.240.{index}.0/24","via","10.242.0.2","dev","tclient","src",source)
                arr=[f"10.240.{index}.{i}" for i in range(1,aliases+1)]
                for ip in arr:
                    self.server.setup(IP,"addr","add",ip+"/32","dev","tserver")
                self.targets.append(arr)
            for h, iface in self.interfaces:
                if h is self.router and delay_us:
                    h.setup(TC,"qdisc","add","dev",iface,"root","netem","delay",f"{delay_us}us","limit","32768")
                self.meta[iface+"_qdisc"] = h.setup(TC,"-s","qdisc","show","dev",iface)
                if h is not self.router and "netem" in self.meta[iface+"_qdisc"]:
                    raise RuntimeError("emulated queue leaked into endpoint")
                st=Path(f"/proc/{h.pid}/net/dev").read_text()
                if iface not in st or re.search(r"\beth0:",st):
                    raise RuntimeError("unexpected interface in namespace")
            for h in endpoints:
                self.meta[h.name+"_routes"]=json.loads(h.setup(IP,"-json","route","show"))
                if any(r.get("dst")=="default" for r in self.meta[h.name+"_routes"]):
                    raise RuntimeError("private namespace has external default route")
            self.meta["placement"] = self.placement(server_cpu, groups)
            ping=checked(self.client.cmd(["/usr/bin/taskset","-c",str(min(groups[0])),sys.executable,
                HERE/"icmp.py",self.targets[0][0]]))
            # First exchange can include ARP. The timestamp is taken by the receiver's kernel.
            rtts=json.loads(ping)[1:]
            if len(rtts)!=24 or min(rtts)<=0:
                raise RuntimeError("could not establish idle network RTT")
            self.meta["idle_icmp_rtt_ms"]={"min":min(rtts),"median":statistics.median(rtts),
                "max":max(rtts),"mean":statistics.mean(rtts),"samples":rtts}
            self.meta["private_ethtool_stats_before"]=self.driver_stats()

        except BaseException:
            self.close()
            raise

    def placement(self, server_cpu, groups):
        path = HERE / "placement"
        srv=sp.Popen(self.server.cmd(["/usr/bin/taskset","-c",str(server_cpu),path,"server",self.targets[0][0],19447]),
                     stdout=sp.PIPE, stderr=sp.STDOUT, text=True)
        try:
            first = srv.stdout.readline()
            if not first.startswith("listening "):
                raise RuntimeError("placement listener failed: " + first)
            cli = checked(self.client.cmd(["/usr/bin/taskset","-c",str(min(groups[0])),path,"client",self.targets[0][0],19447]))
            native = srv.communicate(timeout=6)[0]
            if srv.returncode or f"incoming_cpu={server_cpu}:24," not in native or "mtu=1500" not in cli:
                raise RuntimeError("invalid RX placement or MTU: " + native + cli)
            observed = {int(n) for n in re.findall(r"(\d+):\d+,",cli.split("incoming_cpu=")[1].split()[0])}
            if not observed or observed - set.union(*groups):
                raise RuntimeError("client receive work escaped assigned cores: " + cli)
            for h in (self.server,self.client,self.router):
                count=checked(h.cmd(["/bin/cat","/proc/sys/net/netfilter/nf_conntrack_count"]))
                if int(count):
                    raise RuntimeError("private namespace tracks connections")
            return {"server":native.strip(),"client":cli}
        finally:
            if srv.poll() is None:
                srv.terminate()
                srv.wait(timeout=3)

    def driver_stats(self):
        return {iface:checked(h.cmd([ETH,"-S",iface])) for h,iface in self.interfaces}

    def close(self):
        self.hold.close()

if __name__=="__main__":
    link=Link(10,[{0,1,2},{3,4,5},{6,7,8},{11,12,13}],1,int(sys.argv[1]) if len(sys.argv)>1 else SOCKET_RTT)
    try: print(json.dumps(link.meta,indent=2))
    finally: link.close()

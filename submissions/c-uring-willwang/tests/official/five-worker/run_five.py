"""Run the ordinary-TCP five-external-worker diagnostic under the shared comparison lock."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import sys
import time

HERE=Path(__file__).resolve().parent
sys.path.insert(0,str(HERE))
sys.path.insert(0,str(HERE.parent/"tap-ring4096"))
from run_buffered import BufferedSplitTap,base,route_proof
from placement import PROFILE, ROOT, WORKER_CPUS, check, cli_geometry, files, placement, sha, shape_network
from assess_five import assess

DEFAULT_LOCK=ROOT.parents[1]/"cpu9-bench.lock"


class FiveWorker(BufferedSplitTap):
    def __init__(self,binary):
        super().__init__(binary)
        self.manifest,self.manifest_sha=files()
        check(sha(self.binary)==self.manifest["relay_sha256"], "historical split relay ELF differs")

    def sample(self,namespace):
        row=super().sample(namespace)
        wire=row["polling_relay"]
        actual={}
        for worker in wire.get("workers",[]):
            tid=worker["tid"]
            task=Path(f"/proc/{self.pid}/task/{tid}")
            try:
                actual[str(tid)]={"cpus":sorted(os.sched_getaffinity(tid)),"uid":task.stat().st_uid}
            except (FileNotFoundError, ProcessLookupError):
                actual[str(tid)]=None
        actual_napi={}
        for thread in self.napi:
            try:
                actual_napi[str(thread["pid"])]=sorted(os.sched_getaffinity(thread["pid"]))
            except ProcessLookupError:
                actual_napi[str(thread["pid"])]=None
        row["five_worker_placement"]={"threads":actual,"napi":actual_napi}
        return row

    def set_up(self,args,names,created):
        cli_geometry(args)
        data=super().set_up(args,names,created)
        cfg=data[names[1]]["polling_relay"]
        check(cfg["fixture_source_sha256_at_launch"]=={name:self.manifest["files"][name] for name in cfg["fixture_source_sha256_at_launch"]},
              "underlying launch source fingerprints differ")
        cfg["underlying_fixture_source_sha256_at_launch"]=cfg["fixture_source_sha256_at_launch"]
        cfg["fixture_source_sha256_at_launch"]=self.manifest["files"]
        cfg["source_manifest_sha256_at_launch"]=self.manifest_sha
        observed=self.sample(names[1])
        cfg["five_worker_placement"]=observed["five_worker_placement"]
        wire=observed["polling_relay"]
        cfg["five_worker_wire"]={k:wire[k] for k in ("uid","cpu","worker_count","ports","workers","handoffs")}
        shape_network(data,names,self.manifest,self.manifest_sha)
        return data


def main():
    if "-h" in sys.argv[1:] or "--help" in sys.argv[1:]:
        if "--wire-proof-only" in sys.argv[1:]:
            route_proof(None,relay_cpus=WORKER_CPUS)
        else:
            base.main(delay_us=75,relay_cpus=WORKER_CPUS)
    # The underlying router retains its own global comparison mutex inside either entry point.
    parser=argparse.ArgumentParser(add_help=False)
    parser.add_argument("--output",required=True,type=Path)
    parser.add_argument("--tag")
    parser.add_argument("--wire-proof-only",action="store_true")
    partial,_=parser.parse_known_args()
    if not partial.wire_proof_only and not partial.tag:
        parser.error("stock runs require --tag")
    if partial.tag and (not all(c.isalnum() or c in '_.-' for c in partial.tag) or partial.tag in ('.','..')):
        parser.error("invalid tag")
    out=partial.output.resolve() if partial.wire_proof_only else partial.output.resolve()/partial.tag
    fixture=FiveWorker(Path(os.getenv("TWELVE_SPLIT_TAP_RELAY",ROOT/"tap-split/relay")))
    quiet=Path(os.getenv("TWELVE_FIVE_WORKER_LOCK",DEFAULT_LOCK))
    with quiet.open("r") as lock:
        fcntl.flock(lock,fcntl.LOCK_EX)
        if out.exists():
            raise ValueError(f"retain the existing diagnostic, use a fresh output: {out}")
        stat=os.fstat(lock.fileno())
        expected=os.stat(DEFAULT_LOCK)
        check((stat.st_dev,stat.st_ino)==(expected.st_dev,expected.st_ino),"five-worker run requires the actual shared cpu9 component lock")
        begun=time.time()
        rc=1
        try:
            if partial.wire_proof_only:
                route_proof(fixture,relay_cpus=WORKER_CPUS)
            else:
                base.assess=assess
                base.main(network_setup=fixture.set_up,network_profile=PROFILE,network_sample=fixture.sample,
                          delay_us=75,relay_cpus=WORKER_CPUS)
            rc=0
        finally:
            try:
                fixture.close()
            except BaseException:
                rc=1
                raise
            finally:
                if out.is_dir():
                    record={"lock_file":DEFAULT_LOCK.name,"device":stat.st_dev,"inode":stat.st_ino,
                            "acquired_unix":begun,"finished_unix":time.time(),"runner_exit":rc}
                    (out/"component-test-lock.json").write_text(json.dumps(record,indent=2)+"\n")


if __name__=="__main__":
    main()

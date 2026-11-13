"""Run the primary stock/health assessor and check the additional external placement."""
import argparse
import json
from pathlib import Path
import sys

sys.path.insert(0,str(Path(__file__).resolve().parent))
from placement import PROFILE, ROOT, GROUPS, WORKER_CPUS, check, files, placement, read, shape_network
sys.path.insert(0, str(ROOT / "router"))
from scored_common import assess as assess_common, rows


def evidence(out):
    model, digest = files()
    shape, timing = read(out/"shape.json"), read(out/"timing.json")
    check((shape["topology"], shape["app_cpu"], shape["manager_cpu"], shape["router_cpus"], shape["client_cpus"],
           shape["router_delay_us_each_direction"], shape["target_aliases"], shape["server_ehash_requested"],
           shape["app_memory_bytes"], shape["mload_sha256"]) ==
          (PROFILE,10,15,list(WORKER_CPUS),GROUPS,75,8,16384,1750*2**20,model["stock_mload_sha256"]),
          "profile, four-generator CPU, service limit, topology or mload differs")
    check(len(list(out.glob("shard-*.json")))==4 and read(out/"app-start.json")["affinity"]==[10],
          "expected four stock shards and one service CPU")
    cfg, tids = shape_network(shape["network"], shape["namespaces"], model, digest)
    topology_napi=cfg["five_worker_placement"]["napi"]
    def observe(row):
        check(row["five_worker_placement"]["napi"]==topology_napi,"service NAPI identity changed")
        placement(row["polling_relay"],row["five_worker_placement"],cfg["uid"],tids)
        check(set(row["relay_worker_scheduling"])=={str(t) for t in tids.values()},
              "actual scheduler telemetry for every worker is required")
    network=rows(out/"network-samples.jsonl")
    pre=timing["stock_start_unix"]
    mid=timing["hold_starts_unix"]
    end=timing["cutoff_unix"]
    n={}
    for name,left,right in (("ramp",pre,mid),("hold",mid,end)):
        samples=[row for row in network if left<=row["unix"]<right]
        check(len(samples)>=2,f"insufficient real worker affinity and handoff samples in {name}")
        for row in samples:
            observe(row)
        n[name]=len(samples)
    observe(read(out/"external-at-client-exit.json"))
    return {"profile":PROFILE,"scored_placement_samples":n,"source_and_return_cpu":[2,5,8,9],
            "service_egress_reader_cpu":14,"service_protocol_and_napi_cpu":10,"ordinary_uid":cfg["uid"],
            "expected_lanes":4,"verified_bytes_all_lanes":4*86296,"stable_tids_by_cpu":tids,
            "source_manifest_sha256":digest,"infrastructure_flags":[]}


def assess(out):
    combined=assess_common(out)
    try:
        result=evidence(out)
    except (OSError,KeyError,TypeError,ValueError,IndexError,AttributeError) as error:
        result={"profile":PROFILE,"infrastructure_flags":[f"incomplete five-worker placement: {error}"]}
    combined["five_worker_assessment"]=result
    if result["infrastructure_flags"]:
        combined["infrastructure_flags"] += result["infrastructure_flags"]
        for key in ("infrastructure_ok","scored_healthy_environment","eligible","qualified"):
            combined[key]=False
    (out/"summary.json").write_text(json.dumps(combined,indent=2,sort_keys=True)+"\n")
    return combined


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trial",type=Path)
    out=parser.parse_args().trial
    result=assess(out)
    print(json.dumps({k:result[k] for k in ("qualified","eligible","stock_pass","infrastructure_flags","five_worker_assessment")}))

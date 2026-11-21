"""Five external workers, four client namespaces and a single service TAP."""
import hashlib
import json
from pathlib import Path

PROFILE = "single-queue-split-polling-tap-external-txring4096-five-workers"
WORKER_CPUS = (2, 5, 8, 9, 14)
GROUPS = ["0,1", "3,4", "6,7", "11,12,13"]
EXPECTED_DIGEST = "d53e8b1caeda00a88770b99733b77a0f669ef0722d4fdbd168bdec428c939fad"
ROOT = Path(__file__).resolve().parents[1]
EXPECTED_GRAPH = {14: ("server-egress", [0], [])}
EXPECTED_GRAPH.update({cpu: ("client-ingress", [lane], [lane]) for lane, cpu in enumerate(WORKER_CPUS[:-1], 1)})


def check(value, message):
    if not value:
        raise ValueError(message)


def read(path):
    return json.loads(path.read_bytes())


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def files():
    manifest = ROOT / "manifest.json"
    model = read(manifest)
    for name, expected in model["files"].items():
        check(sha(ROOT / name) == expected, f"frozen profile file differs: {name}")
    return model, sha(manifest)


def cli_geometry(args):
    check((args.app_cpu, args.manager_cpu, tuple(args.router_cpus), args.groups, args.aliases, args.server_ehash, args.delay_us)
          == (10, 15, WORKER_CPUS, GROUPS, 8, 16384, 75),
          "five-worker profile requires service/NAPI 10, manager 15, relay 2,5,8,9,14, the original four client groups, eight aliases, ehash 16384 and 75 us")


def placement(wire, observed, expected_uid, stable=None):
    workers = wire["workers"]
    check(wire["worker_count"] == len(workers) == 5 and len(wire["ports"]) == 5,
          "expected five workers on one service TAP and four generator TAPs")
    check(wire["uid"] == expected_uid and wire["cpu"] == 14, "relay user or service reader CPU changed")
    tids = {w["cpu"]: w["tid"] for w in workers}
    check(set(tids) == set(WORKER_CPUS) and len(set(tids.values())) == 5,
          "the five distinct worker CPUs or TIDs are absent")
    if stable is not None:
        check(stable == tids, "relay worker identities changed after preflight")
    check(set(observed["threads"]) == {str(tid) for tid in tids.values()}, "worker affinity evidence incomplete")
    for w in workers:
        cpu, tid = w["cpu"], str(w["tid"])
        check((w["role"], w["sources"], w["destinations"]) == EXPECTED_GRAPH[cpu],
              f"source/return ownership on CPU {cpu} differs")
        check(observed["threads"][tid] == {"cpus":[cpu], "uid":expected_uid},
              f"actual affinity or owner of worker {tid} differs from CPU {cpu}")
    handoffs=wire["handoffs"]
    check(len(handoffs)==4 and sorted(h["destination"] for h in handoffs)==[1,2,3,4],
          "requires exactly four reply queues")
    for h in handoffs:
        lane=h["destination"]
        check((h["source"], h["producer_cpu"], h["consumer_cpu"])==(0, 14, WORKER_CPUS[lane-1]),
              f"handoff for client lane {lane} has the wrong owner")
    check(len(observed["napi"])==1 and next(iter(observed["napi"].values()))==[10],
          "service threaded NAPI CPU differs from CPU 10")
    return tids


def shape_network(topology, names, manifest, manifest_sha):
    server, controller, *clients=names
    check(len(clients)==4 and len(set(names))==6, "expected four distinct client namespaces")
    cfg=topology[controller]["polling_relay"]
    dev=cfg["server_tap"]
    check((cfg["ports"], cfg["cpus"], cfg["delay_us"], cfg["mtu"])==([server,*clients],list(WORKER_CPUS),75,1500),
          "endpoint, CPU, delay or MTU topology differs")
    check(cfg["source_manifest_sha256_at_launch"]==manifest_sha and cfg["fixture_source_sha256_at_launch"]==manifest["files"]
          and cfg["binary_sha256"]==manifest["relay_sha256"],"profile launch provenance differs")
    check(int(topology[server]["ehash_entries"])==16384,"private service TCP ehash differs")
    expected_napi=topology[server]["threaded_napi"][dev]
    check(expected_napi["threaded"]=="1" and len(expected_napi["napi"])==1 and expected_napi["napi"][0]["cpus"]==[10]
          and int(topology[server]["rx_masks"][dev].replace(",",""),16)==0,"service threaded NAPI/RPS placement differs")
    check(set(cfg["private_tap_transmit_ring_geometry"])=={server,*clients},"missing ring geometry")
    for ns in [server,*clients]:
        ring=cfg["private_tap_transmit_ring_geometry"][ns]
        check(ring["original"]==1000 and ring["new"]==4096 and ring["ifindex"]>0,
              "private ring identity or size differs")
        port=topology[ns]["interface"]
        check((port["mtu"],port["num_tx_queues"],port["num_rx_queues"],port["txqlen"])==(1500,1,1,4096),
              "private TAP link geometry differs")
        offloads=next(iter(topology[ns]["offloads"].values()))
        check(offloads=={k:"on" for k in ("generic-receive-offload","generic-segmentation-offload","tcp-segmentation-offload","tx-checksumming")},"private offloads differ")
    wire=cfg["preflight_forwarding"]
    check(wire["gso"]>0 and wire["partial_checksum"]>0 and wire["bad_frame"]==wire["queue_overflow"]==0,"GSO/checksum preflight failed")
    check(len(cfg["tcp_payload_proofs"])==4,"missing per-lane byte proof")
    for group,ns,proof in zip(GROUPS,clients,cfg["tcp_payload_proofs"]):
        cpu_mask=sum(1<<int(c) for c in group.split(","))
        check(int(next(iter(topology[ns]["rx_masks"].values())).replace(",",""),16)==cpu_mask,"client RX mask differs")
        check(topology[ns]["socket_probe_cpu_and_napi_id"]==[[10,0]]*4,"short socket CPU/NAPI probe failed")
        c,s=proof["client"],proof["service"]
        check((c["count"],c["bytes"],c["sha256"],s["count"],s["cpus"],s["napi_ids"])==(32,86296,EXPECTED_DIGEST,32,[10],[0])
              and 0<c["mss"]<=1460 and 0<s["mss"]<=1460,"service TCP placement, byte checksum, count or MSS differs")
    tidmap=placement(cfg["five_worker_wire"], cfg["five_worker_placement"], cfg["uid"])
    check(cfg["five_worker_placement"]["napi"]=={str(expected_napi["napi"][0]["pid"]):[10]},
          "NAPI identity did not match setup")
    check({w["cpu"]:w["tid"] for w in cfg["verified_workers"]}==tidmap,"pre-existing verified worker list differs")
    check({w["cpu"]:(w["role"],w["sources"],w["destinations"]) for w in cfg["workers_with_delivery_ownership"]}==EXPECTED_GRAPH,"original verified assignment differs")
    check(len(cfg["verified_handoffs"])==4 and all(h["source"]==0 and h["producer_cpu"]==14 and h["consumer_cpu"]==WORKER_CPUS[h["destination"]-1] for h in cfg["verified_handoffs"])
          and sorted(h["destination"] for h in cfg["verified_handoffs"])==[1,2,3,4],"original verified handoffs differ")
    return cfg, tidmap

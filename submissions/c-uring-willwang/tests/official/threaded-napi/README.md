# Place receive processing on the server CPU with threaded NAPI

This optional fixture changes the ingress path in private benchmark namespaces while preserving the application binary, stock Go scorer, CPU and memory budget, network delay and workload. Run its entrypoint with the same arguments as the adjacent four-lane runner.

```sh
python3 /absolute/path/to/threaded-napi/run_threaded.py \
    --server /absolute/path/to/entry/bin/server --cwd /absolute/path/to/entry \
    --mload /absolute/path/to/mload --output /absolute/path/to/results \
    --users 280000 --hold 300 --tag original-threaded-280k-h1
```

The standard Linux 6.8 veth transmit path bypasses the peer NAPI queue for ordinary TCP when TCP segmentation offload is enabled on the transmitting device. Such packets enter `__netif_rx`, where receive packet steering can move work onto the service CPU's backlog.

1. The private router disables TCP segmentation offload on the four veth devices that transmit towards the server. Its client-facing devices retain their original offload configuration.
2. The service namespace keeps GRO enabled on each of its four veth devices, disables RPS and enables threaded NAPI. The Linux driver queues TCP traffic onto those interfaces’ NAPI queues. One CPU runs their poll threads and the service application.
3. The activation helper checks the network namespace inode and veth type. A generic-netlink `NETDEV_CMD_NAPI_GET` dump filtered by the private interface index returns its NAPI IDs and the exact owning kernel PIDs. Only newly created NAPI threads are accepted. The helper verifies the kernel-task flag and interface/ID-specific name before setting each affinity.
4. Before launch, every real client-to-service route exchanges four tiny TCP messages with a throwaway unprivileged test socket. Every accepted read must report the service CPU through `SO_INCOMING_CPU` and a NAPI ID owned by that interface through `SO_INCOMING_NAPI_ID`. The scored process then starts normally. Completion records that deletion of the private devices reaped their kernel threads.

The isolated 32-message functionality proof used a different CPU for the sender and receiver. With peer TSO enabled, all 16 reads reported sender CPU 15, NAPI ID zero and no NAPI-thread CPU time. With TSO disabled, all 16 reads reported service CPU 9 and the kernel-issued NAPI ID 8288. That thread consumed 1.30 ms and disappeared on cleanup. A separate routed probe verified all four lanes and observed their four distinct NAPI IDs on the requested service CPU. The local benchmark fixture uses sudo for its private-device setup. The submitted application runs as an ordinary user. [Direct comparison](results.json) · [Four routed lanes](four-lane-placement.json)

The Linux guest NIC in the official VPS uses its own receive queues and driver. This profile models the common property that a single guest CPU performs both receive polling and application processing. Its scheduling and virtual queue implementation can differ from the VPS. Compare each entry within the same named profile.

[Linux v6.8 veth eligibility and forwarding](https://github.com/torvalds/linux/blob/v6.8/drivers/net/veth.c#L323) · [Linux v6.8 threaded NAPI](https://github.com/torvalds/linux/blob/v6.8/net/core/dev.c#L6368) · [NAPI ID and owning thread API](https://github.com/torvalds/linux/blob/v6.8/net/core/netdev-genl.c#L175) · [Isolated placement results](results.json)

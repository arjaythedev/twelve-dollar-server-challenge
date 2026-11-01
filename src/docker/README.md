# Native container

The seccomp profile is Moby's default at `4b8214fc4971e06172938a1f1554ccff6647ee80`, with `io_uring_setup`, `io_uring_enter` and `io_uring_register` added to its allowlist. Other rules are unchanged. Source: <https://github.com/moby/profiles/blob/4b8214fc4971e06172938a1f1554ccff6647ee80/seccomp/default.json>

The optional `profiler` image stage adds perf. Its container uses Docker's default seccomp profile with CAP_PERFMON and UID 10001. It samples only the server process. No host sysctls or server permissions are changed.

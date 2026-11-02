# Native container

The seccomp profile is Moby's default at `4b8214fc4971e06172938a1f1554ccff6647ee80`, with `io_uring_setup`, `io_uring_enter` and `io_uring_register` added to its allowlist. Other rules are unchanged. Source: <https://github.com/moby/profiles/blob/4b8214fc4971e06172938a1f1554ccff6647ee80/seccomp/default.json>

The optional `profiler` image adds perf and samples only the server process. User profiling uses UID 10001 and CAP_PERFMON. `profile --scope kernel` or `--scope all` uses root with CAP_PERFMON, CAP_SYSLOG and CAP_SYS_PTRACE for kernel symbols and target process metadata. Host sysctls and server permissions stay unchanged.

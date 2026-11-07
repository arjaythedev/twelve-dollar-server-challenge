// Can this process create BPF maps and load the program types the eBPF design needs?
// Prints one line per attempt: what, then OK or the errno.
#include <linux/bpf.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static long bpf(int cmd, union bpf_attr *a) { return syscall(__NR_bpf, cmd, a, sizeof *a); }

static void map(const char *name, int type, int vsz) {
  union bpf_attr a; memset(&a, 0, sizeof a);
  a.map_type = type; a.key_size = 4; a.value_size = vsz; a.max_entries = 4;
  long fd = bpf(BPF_MAP_CREATE, &a);
  printf("map  %-12s %s\n", name, fd >= 0 ? "OK" : strerror(errno));
  if (fd >= 0) close(fd);
}

static void prog(const char *name, int type, int eat) {
  struct bpf_insn insn[2] = {
    { .code = BPF_ALU64 | BPF_MOV | BPF_K, .dst_reg = 0, .imm = 0 },  // r0 = 0
    { .code = BPF_JMP | BPF_EXIT },
  };
  union bpf_attr a; memset(&a, 0, sizeof a);
  a.prog_type = type; a.insns = (unsigned long)insn; a.insn_cnt = 2;
  a.license = (unsigned long)"GPL"; a.expected_attach_type = eat;
  long fd = bpf(BPF_PROG_LOAD, &a);
  printf("prog %-12s %s\n", name, fd >= 0 ? "OK" : strerror(errno));
  if (fd >= 0) close(fd);
}

int main(void) {
  char line[256]; FILE *f = fopen("/proc/self/status", "r");
  while (fgets(line, sizeof line, f))
    if (!strncmp(line, "Cap", 3) || !strncmp(line, "NoNewPrivs", 10)) fputs(line, stdout);
  printf("uid=%d\n", getuid());
  map("array", BPF_MAP_TYPE_ARRAY, 4);
  map("sockmap", BPF_MAP_TYPE_SOCKMAP, 4);
  map("sockhash", BPF_MAP_TYPE_SOCKHASH, 4);
  prog("socket_filter", BPF_PROG_TYPE_SOCKET_FILTER, 0);
  prog("sk_skb_parser", BPF_PROG_TYPE_SK_SKB, BPF_SK_SKB_STREAM_PARSER);
  prog("sk_skb_verdict", BPF_PROG_TYPE_SK_SKB, BPF_SK_SKB_STREAM_VERDICT);
  prog("sockops", BPF_PROG_TYPE_SOCK_OPS, 0);
  return 0;
}

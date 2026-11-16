#include <stdint.h>
void bf_execute(int64_t *tape);
void bf_mmio_sys(int64_t *t){}
void bf_mmio_crypto(int64_t *t){}
void bf_mmio_db(int64_t *t){}
void bf_mmio_net(int64_t *t){}
static int64_t tape[65536];
int main(int argc, char **argv) { bf_execute(tape + 32768); return 0; }

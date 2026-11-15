#ifndef BF_RUNTIME_H
#define BF_RUNTIME_H

#include <stdint.h>
#include <stddef.h>

void bf_runtime_init(int argc, char **argv);
void bf_mmio_sys(int64_t *tape);
void bf_mmio_crypto(int64_t *tape);
void bf_mmio_db(int64_t *tape);
void bf_mmio_net(int64_t *tape);
void bf_out_char(uint8_t c);
int64_t bf_in_char(void);

#endif

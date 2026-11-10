#include "../checkpoint_policy.h"
#include <assert.h>
#include <stdio.h>

static void tick(hkn_checkpoint_policy *p, int second, uint64_t written, int health) {
  unsigned previous = p->target;
  int64_t changed = p->changed;
  hkn_checkpoint_policy_sample(p, (int64_t)second * 1000000000, written, health);
  assert(p->target >= HKN_CHECKPOINT_MIN && p->target <= HKN_CHECKPOINT_MAX);
  assert(p->target % HKN_CHECKPOINT_STEP == 0);
  if (p->target == previous) return;
  int delta = (int)p->target - (int)previous;
  assert(delta == HKN_CHECKPOINT_STEP || delta == -HKN_CHECKPOINT_STEP);
  assert(p->changed - changed >= 5000000000);
}

int main(void) {
  hkn_checkpoint_policy p;
  hkn_checkpoint_policy_init(&p, 0);
  assert(p.target == 8192);
  hkn_checkpoint_policy_sample(&p, 500000000, 32768, 1);
  assert(p.written == 0 && p.target == 8192); // No subsecond samples.
  uint64_t written = 0;
  for (int second = 1; second <= 60; ++second) {
    written += 32768;
    tick(&p, second, written, 1);
  }
  assert(p.target == 16384);
  for (int second = 61; second <= 120; ++second) tick(&p, second, written, 1);
  assert(p.target == 8192); // Quiet traffic returns to the floor.

  hkn_checkpoint_policy_init(&p, 0);
  written = 0;
  for (int second = 1; second <= 60; ++second) {
    written += 65536;
    tick(&p, second, written, 0);
  }
  assert(p.target == 8192); // No growth without checkpoint progress.

  hkn_checkpoint_policy_init(&p, 0);
  written = 0;
  for (int second = 1; second <= 30; ++second) {
    written += 65536;
    tick(&p, second, written, 1);
  }
  assert(p.target == 16384);
  for (int second = 31; second <= 60; ++second) {
    written += 65536;
    tick(&p, second, written, -1);
  }
  assert(p.target == 8192); // Lag wins over high arrival rate.

  hkn_checkpoint_policy_init(&p, 0);
  p.target = p.rate = 12288;
  written = 0;
  for (int second = 1; second <= 60; ++second) {
    written += second % 2 ? 10240 : 14336;
    tick(&p, second, written, 1);
    assert(p.target == 12288); // Smoothed noise must not cause toggling.
  }
  puts("checkpoint policy passed: bounds, steps, cooldown, traffic, lag, noise");
  return 0;
}

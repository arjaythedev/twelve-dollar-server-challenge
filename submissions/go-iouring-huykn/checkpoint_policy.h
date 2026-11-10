#ifndef HUYKN_CHECKPOINT_POLICY_H
#define HUYKN_CHECKPOINT_POLICY_H
#include <stdint.h>

enum { HKN_CHECKPOINT_MIN = 8192, HKN_CHECKPOINT_MAX = 16384,
       HKN_CHECKPOINT_STEP = 2048, HKN_CHECKPOINT_RETRY = 1 };

typedef struct {
  uint64_t written, rate;
  int64_t sampled, changed;
  unsigned target;
  int trend;
} hkn_checkpoint_policy;

static inline void hkn_checkpoint_policy_init(hkn_checkpoint_policy *p, int64_t now) {
  *p = (hkn_checkpoint_policy){.sampled = now, .changed = now, .target = HKN_CHECKPOINT_MIN};
}

// Aim for one second of writes. Require three agreeing samples and at least
// five seconds between steps. Only observed checkpoint progress permits growth.
static inline void hkn_checkpoint_policy_sample(hkn_checkpoint_policy *p, int64_t now,
                                                 uint64_t written, int health) {
  int64_t elapsed = now - p->sampled;
  if (elapsed < 1000000000) return;
  uint64_t rate = (written - p->written) * 1000000000 / (uint64_t)elapsed;
  p->rate = (p->rate * 3 + rate) / 4;
  p->written = written;
  p->sampled = now;
  uint64_t desired = (p->rate + HKN_CHECKPOINT_STEP / 2) / HKN_CHECKPOINT_STEP * HKN_CHECKPOINT_STEP;
  if (desired < HKN_CHECKPOINT_MIN) desired = HKN_CHECKPOINT_MIN;
  if (desired > HKN_CHECKPOINT_MAX) desired = HKN_CHECKPOINT_MAX;
  int direction = (desired > p->target) - (desired < p->target);
  if (health < 0) direction = p->target > HKN_CHECKPOINT_MIN ? -1 : 0;
  if (!health && direction > 0) direction = 0;
  p->trend = direction * p->trend > 0 ? p->trend + direction : direction;
  if ((p->trend > -3 && p->trend < 3) || now - p->changed < 5000000000) return;
  p->target = (unsigned)((int)p->target + direction * HKN_CHECKPOINT_STEP);
  p->changed = now;
  p->trend = 0;
}
#endif

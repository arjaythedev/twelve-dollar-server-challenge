// shim.cpp: the glue between ngspice's d_cosim and the Verilated chip, in place of ngspice's own
// scripts/src/verilator_shim.cpp. Same job (pins in, eval, changed pins out), plus two things:
//  - the chip's clock multiplier: after each rising edge of the board clock it runs the core clock
//    (cclk, not a pin) for MULT cycles;
//  - a rising edge on the chip's rst input power-cycles it (a new model, every register at its
//    initial value).
// The ports come from inputs.h / outputs.h, which build-chip.sh generates from Vlng.h.

#include <string.h>
#include "verilated.h"
#include "Vlng.h"

#include "ngspice/cmtypes.h"
#include "ngspice/cosim.h"

#define VL_DATA(size, name, msb, lsb) + msb - lsb + 1
static const unsigned int outs = 0
#include "outputs.h"
  ;
#undef VL_DATA

static unsigned char previous_output[outs];
static VerilatedContext *ctx;
static unsigned char rst_prev, clk_prev;
static const int MULT = 8;   // core cycles per board clock (top.v counts them in ph)
#define PIN(name) (strcmp(#name, "cclk") != 0)   // cclk is driven here, not by the board

static void accept_input(struct co_info *pinfo, unsigned int index, Digital_t *vp)
{
  Vlng *topp = (Vlng *)pinfo->handle;
  if (vp->state == UNKNOWN) return;   // Verilator simulations are two-state
  unsigned int val = vp->state == ONE;
#define VL_DATA(size, name, msb, lsb)                                   \
  if (!PIN(name)) {                                                     \
  } else if (index >= msb - lsb + 1) {                                  \
    index -= msb - lsb + 1;                                             \
  } else {                                                              \
    if (val) topp->name |= 1ull << (msb - index);                       \
    else     topp->name &= ~(1ull << (msb - index));                    \
    return;                                                             \
  }
#include "inputs.h"
#undef VL_DATA
}

static void step(struct co_info *pinfo)
{
  static Digital_t oval = {ZERO, STRONG};
  Vlng *topp = (Vlng *)pinfo->handle;

  if (topp->rst && !rst_prev) {     // power-cycle: a fresh model with the same inputs
    Vlng *n = new Vlng{ctx};
#define VL_DATA(size, name, msb, lsb) n->name = topp->name;
#include "inputs.h"
#undef VL_DATA
    delete topp;
    pinfo->handle = topp = n;
  }
  rst_prev = topp->rst;
  topp->eval();
  if (topp->clk && !clk_prev)       // the clock multiplier
    for (int c = 0; c < MULT; c++) { topp->cclk = 1; topp->eval(); topp->cclk = 0; topp->eval(); }
  clk_prev = topp->clk;

  int index = 0;
#define VL_DATA(size, name, msb, lsb)                                   \
  for (int i = msb; i >= lsb; --i) {                                    \
    unsigned char bit = (topp->name >> i) & 1;                          \
    if (bit ^ previous_output[index]) {                                 \
      previous_output[index] = bit;                                     \
      oval.state = (Digital_State_t)bit;                                \
      (*pinfo->out_fn)(pinfo, index, &oval);                            \
    }                                                                   \
    ++index;                                                            \
  }
#include "outputs.h"
#undef VL_DATA
}

static void cleanup(struct co_info *pinfo)
{
  delete (Vlng *)pinfo->handle;
  pinfo->handle = nullptr;
}

extern "C" void Cosim_setup(struct co_info *pinfo)
{
  Verilated::debug(0);
  if (!ctx) ctx = new VerilatedContext;
  // After ngspice's reset, d_cosim can still hold a pin's last value from the previous run while the node
  // starts at 0, and then a 1 on that pin is never sent. Claiming every pin is 1 makes the first step
  // send an explicit 0 for each pin that is 0, which puts d_cosim back in step.
  for (unsigned int i = 0; i < outs; i++) previous_output[i] = 1;
  rst_prev = clk_prev = 0;
  pinfo->handle = new Vlng{ctx};
  pinfo->step = step;
  pinfo->cleanup = cleanup;
  unsigned int i = 0;
#define VL_DATA(size, name, msb, lsb) if (PIN(name)) i += msb - lsb + 1;
#include "inputs.h"
#undef VL_DATA
  pinfo->in_count = i;
  pinfo->out_count = outs;
  pinfo->inout_count = 0;
  pinfo->in_fn = accept_input;
  pinfo->method = After_input;  // Verilator requires input to advance
}

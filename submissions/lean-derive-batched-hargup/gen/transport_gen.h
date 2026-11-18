/* GENERATED from the Lean specification in spec/ (Feed.Emit.transportGen, from
 * Feed.Transport). Do not edit: change the specification and run `lake exe derive`. */
#ifndef TRANSPORT_GEN_H
#define TRANSPORT_GEN_H

/* Feed.Transport.sizing (its proof fields are checked by Lean and erased here) */
#define MAX_RINGS 16
#define SLOTS_MAX 65535
#define NBUF 1024
#define BUFSZ 2048
#define CQE_BUDGET 2048
#define MAX_JOBS 65536

/* Feed.Transport.Recv, Ev, Act */
enum { R_IDLE = 0, R_MULTI = 1, R_QUEUED = 2 };
enum { EV_MORE = 0, EV_ENDED = 1, EV_NOBUFS = 2, EV_EOF = 3, EV_COUNT = 4 };
enum { A_KEEP = 0, A_ENQUEUE = 1, A_CLOSE = 2, A_DROP = 3 };

/* Feed.Transport.step: the state after a receive completion and what to submit. A_DROP:
 * the completion cannot belong to this state (step is none). */
static const u8 RECV_NEXT[3][EV_COUNT] = {
  {R_IDLE, R_IDLE, R_IDLE, R_IDLE}  /* R_IDLE */,
  {R_MULTI, R_QUEUED, R_QUEUED, R_IDLE}  /* R_MULTI */,
  {R_IDLE, R_IDLE, R_IDLE, R_IDLE}  /* R_QUEUED */
};
static const u8 RECV_ACT[3][EV_COUNT] = {
  {A_DROP, A_DROP, A_DROP, A_DROP}  /* R_IDLE */,
  {A_KEEP, A_ENQUEUE, A_ENQUEUE, A_CLOSE}  /* R_MULTI */,
  {A_DROP, A_DROP, A_DROP, A_DROP}  /* R_QUEUED */
};

#endif

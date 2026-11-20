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
#define READS_PER_PASS 16384
#define JOB_BUDGET 2048

/* Feed.Transport.Recv, Ev, Act */
enum { R_IDLE = 0, R_MULTI = 1, R_QUEUED = 2, R_READING = 3 };
enum { EV_MORE = 0, EV_ENDED = 1, EV_NOBUFS = 2, EV_EOF = 3, EV_FULL = 4, EV_DRAINED = 5, EV_READ_NOBUFS = 6, EV_READ_EOF = 7, EV_COUNT = 8 };
enum { A_KEEP = 0, A_ENQUEUE = 1, A_CLOSE = 2, A_ARM = 3, A_DROP = 4 };

/* Feed.Transport.step: the state after a receive completion and what to submit. A_DROP:
 * the completion cannot belong to this state (step is none). */
static const u8 RECV_NEXT[4][EV_COUNT] = {
  {R_IDLE, R_IDLE, R_IDLE, R_IDLE, R_IDLE, R_IDLE, R_IDLE, R_IDLE}  /* R_IDLE */,
  {R_MULTI, R_QUEUED, R_QUEUED, R_IDLE, R_IDLE, R_IDLE, R_IDLE, R_IDLE}  /* R_MULTI */,
  {R_IDLE, R_IDLE, R_IDLE, R_IDLE, R_IDLE, R_IDLE, R_IDLE, R_IDLE}  /* R_QUEUED */,
  {R_IDLE, R_IDLE, R_IDLE, R_IDLE, R_QUEUED, R_MULTI, R_QUEUED, R_IDLE}  /* R_READING */
};
static const u8 RECV_ACT[4][EV_COUNT] = {
  {A_DROP, A_DROP, A_DROP, A_DROP, A_DROP, A_DROP, A_DROP, A_DROP}  /* R_IDLE */,
  {A_KEEP, A_ENQUEUE, A_ENQUEUE, A_CLOSE, A_DROP, A_DROP, A_DROP, A_DROP}  /* R_MULTI */,
  {A_DROP, A_DROP, A_DROP, A_DROP, A_DROP, A_DROP, A_DROP, A_DROP}  /* R_QUEUED */,
  {A_DROP, A_DROP, A_DROP, A_DROP, A_ENQUEUE, A_ARM, A_ENQUEUE, A_CLOSE}  /* R_READING */
};

#endif

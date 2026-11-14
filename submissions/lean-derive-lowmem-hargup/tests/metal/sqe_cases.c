/* Mock only the system-call boundary; run the actual reserve/construct/publish paths. */
#include <assert.h>
#include <stdarg.h>
#include <unistd.h>
static long mock_syscall(long number,...);
#define syscall mock_syscall
#include "out.c"
#include "http.c"
#include "uring.c"
#undef syscall
static void app_decode(const req_t *rq,job_t *j,u32 slot){(void)rq;(void)j;(void)slot;}
static void app_batch(job_t *js,int n){(void)js;(void)n;}
static ring *active;
static u64 expected[2000];
static unsigned produced, consumed, calls, limit=1;
static long mock_syscall(long number,...) {
  assert(number==__NR_io_uring_enter);
  va_list ap;va_start(ap,number);int fd=va_arg(ap,int);unsigned sub=va_arg(ap,unsigned);va_end(ap);
  assert(fd==active->fd && sub==active->to_submit);
  assert(*active->sq_ktail==active->sq_tail);
  assert(sub==(u32)(*active->sq_ktail-*active->sq_khead));
  if(++calls%7==0){errno=EINTR;return -1;}
  unsigned take=sub<limit?sub:limit;
  for(unsigned i=0;i<take;i++) {
    struct io_uring_sqe *s=&active->sqes[*active->sq_khead&active->sq_mask];
    assert(consumed<produced && s->user_data==expected[consumed]);
    assert(s->opcode==IORING_OP_SEND || s->opcode==IORING_OP_SHUTDOWN || s->opcode==IORING_OP_CLOSE);
    consumed++;(*active->sq_khead)++;
  }
  return take;
}
int main(void) {
  for(unsigned entries=2;entries<=8;entries*=2) {
    u32 head=UINT32_MAX-3,tail=head;
    struct io_uring_sqe slots[8];memset(slots,0xa5,sizeof slots);
    ring r={.fd=99,.sq_khead=&head,.sq_ktail=&tail,.sq_entries=entries,.sq_mask=entries-1,.sq_tail=head,.sqes=slots};
    active=&r;produced=consumed=calls=0;limit=1;
    for(unsigned i=0;i<1000;i++) {
      sqe_ensure(&r,1);u32 prior=tail;
      expected[produced++]=0x12340000ull+i;
      sqe_send(&r,17,0xabcdef00ull+i,23,expected[produced-1]);
      assert(tail==prior); /* Construction remains private until entry. */
      struct io_uring_sqe *s=&slots[(r.sq_tail-1)&r.sq_mask];
      assert(s->fd==17 && s->len==23 && s->addr==0xabcdef00ull+i);
      assert(s->off==0 && s->buf_index==0 && s->personality==0 && s->__pad2[0]==0);
      if(i%3==0)ring_flush(&r);
    }
    while(r.to_submit)ring_flush(&r);
    assert(consumed==produced && head==tail && r.sq_tail==tail);
    /* Leave one free slot, then close: the pair must fit before the first is constructed. */
    for(unsigned i=0;i<entries-1;i++) {
      expected[produced++]=1000+i;sqe_send(&r,17,0,0,1000+i);
    }
    conn c={.state=C_OPEN,.gen=3};r.conns=&c;r.idx=0;
    expected[produced++]=ud(OP_SHUTDOWN,0,3,0);
    expected[produced++]=ud(OP_CLOSE,0,3,0);
    conn_close(&r,0);
    assert(r.to_submit>=2);
    struct io_uring_sqe *shutdown=&slots[(r.sq_tail-2)&r.sq_mask], *close=&slots[(r.sq_tail-1)&r.sq_mask];
    assert(shutdown->opcode==IORING_OP_SHUTDOWN && (shutdown->flags&IOSQE_IO_HARDLINK));
    assert(close->opcode==IORING_OP_CLOSE && close->file_index==1);
    limit=entries;while(r.to_submit)ring_flush(&r);
    assert(consumed==produced && head==tail);
  }
  puts("SQE: private construction, zeroing, tiny queues, tail wrap, EINTR, partial submits and close-pair capacity passed");
  return 0;
}

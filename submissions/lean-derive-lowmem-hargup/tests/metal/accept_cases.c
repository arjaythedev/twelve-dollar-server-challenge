/* Actual transport accounting, with only the syscall boundary replaced. */
#include <assert.h>
#include <stdarg.h>
#include <unistd.h>
static long mock_syscall(long number, ...);
#define syscall mock_syscall
#include "out.c"
#include "http.c"
#include "uring.c"
#undef syscall
static void app_decode(const req_t *rq, job_t *j, u32 s) {(void)rq;(void)j;(void)s;}
static void app_batch(job_t *j, int n) {(void)j;(void)n;}
static ring *active;
static int reg_mode, reg_calls;
static long mock_syscall(long number, ...) {
  va_list ap; va_start(ap,number);
  int fd=va_arg(ap,int); (void)fd;
  if(number==__NR_io_uring_register) {
    int op=va_arg(ap,int);struct io_uring_buf_reg *r=va_arg(ap,void*);
    assert(op==IORING_REGISTER_PBUF_RING);reg_calls++;
    assert(r->resv[0]==(u64)(reg_calls==2));
    va_end(ap);
    if(reg_mode==0 || (reg_mode==1 && reg_calls==2))return 0;
    errno=reg_mode==2 ? EPERM : EINVAL;return -1;
  }
  assert(number==__NR_io_uring_enter);
  unsigned n=va_arg(ap,unsigned);va_end(ap);
  for(unsigned i=0;i<n;i++) {
    struct io_uring_sqe *s=&active->sqes[*active->sq_khead & active->sq_mask];
    if(s->opcode==IORING_OP_ACCEPT) {
      assert(s->ioprio==0 && s->file_index==IORING_FILE_INDEX_ALLOC);
    }
    (*active->sq_khead)++;
  }
  return n;
}
int main(void) {
  for(reg_mode=0;reg_mode<4;reg_mode++) {
    struct io_uring_buf_reg reg={0};reg_calls=0;
    int rc=register_buffers(99,&reg);
    assert((rc==0)==(reg_mode<2));
    assert(reg_calls==((reg_mode==1 || reg_mode==3)?2:1));
    assert(reg.resv[0]==0);
  }
  for(u32 nslots=1;nslots<=1024;nslots=nslots*2) {
    for(u32 entries=2;entries<=32;entries*=4) {
      u32 head=0,tail=0;struct io_uring_sqe sq[32]={0};
      ring r={.fd=99,.sq_khead=&head,.sq_ktail=&tail,.sq_entries=entries,
        .sq_mask=entries-1,.sqes=sq,.nslots=nslots};
      r.conns=calloc(nslots,sizeof(conn));active=&r;
      for(int cycle=0;cycle<3;cycle++) {
        while(r.nopen<nslots) {
          fill_accepts(&r);
          assert(r.accepts>0 && r.accepts<=accept_depth(&r));
          assert(r.nopen+r.accepts<=nslots);
          u32 count=r.accepts;
          /* Complete a failed accept first: the reservation must be released. */
          struct io_uring_cqe bad={.user_data=ud(OP_ACCEPT,0,0,0),.res=-EINTR};
          handle_cqe(&r,&bad);assert(r.accepts==count-1);fill_accepts(&r);
          count=r.accepts;
          for(u32 i=0;i<count;i++) {
            struct io_uring_cqe e={.user_data=ud(OP_ACCEPT,0,0,0),.res=(int)r.nopen};
            handle_cqe(&r,&e);assert(r.nopen+r.accepts<=nslots);
          }
        }
        fill_accepts(&r);assert(r.accepts==0); /* Full means no accepted-and-dropped socket. */
        while(r.to_submit)ring_flush(&r);
        /* All slots close, then a second burst can reuse them. */
        for(u32 i=0;i<nslots;i++) {
          r.conns[i].state=C_CLOSING;
          struct io_uring_cqe e={.user_data=ud(OP_CLOSE,0,r.conns[i].gen,i),.res=0};
          handle_cqe(&r,&e);
        }
        assert(!r.nopen);
      }
      free(r.conns);
    }
  }
  puts("accept: bounded reservations, full tables, errors, tiny SQs, slot reuse; buffer registration fallback passed");
}

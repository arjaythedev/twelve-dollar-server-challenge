/* Isolate the actual rec_cols lowering: the same username with a 3- versus 2-field prefix. */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "record.c"
static __attribute__((noinline)) u64 full(const rec_t *r) {
  col_t c[3];if(rec_cols(r,3,c)||!col_is_text(&c[1]))abort();
  return col_len(&c[1])+c[1].p[0];
}
static __attribute__((noinline)) u64 projected(const rec_t *r) {
  col_t c[2];if(rec_cols(r,2,c)||!col_is_text(&c[1]))abort();
  return col_len(&c[1])+c[1].p[0];
}
static u64 ns(void){struct timespec t;clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t);return (u64)t.tv_sec*1000000000ull+t.tv_nsec;}
int main(int argc,char **argv) {
  int rounds=argc>1?atoi(argv[1]):20000000;
  u8 bytes[64][700];rec_t rows[64];
  for(int fixture=0;fixture<2;fixture++) {
    for(unsigned i=0;i<64;i++) {
      u32 n=fixture?400+i:8+i%16;u32 serial=13+2*n;
      u8 *p=bytes[i];u32 h=serial<128?4:5;p[0]=h;p[1]=0;
      if(serial<128)p[2]=serial;else{p[2]=0x80|(serial>>7);p[3]=serial&127;}
      p[h-1]=51;memset(p+h,'a'+i%26,n+19);rows[i]=(rec_t){p,h+n+19};
      if(full(&rows[i])!=projected(&rows[i]))abort();
    }
    for(int pass=0;pass<6;pass++) {
      int proj=pass==1||pass==2||pass==5;
      u64 (*fn)(const rec_t*)=proj?projected:full,sum=0,start=ns();
      for(int i=0;i<rounds;i++)sum+=fn(&rows[i&63]);
      printf("%s %s ns/record=%.3f checksum=%llu\n",fixture?"long":"short",proj?"prefix2":"prefix3",(double)(ns()-start)/rounds,(unsigned long long)sum);
    }
  }
  return 0;
}

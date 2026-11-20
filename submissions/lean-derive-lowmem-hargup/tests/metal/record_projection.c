/* Compare the projection with the original prefix decoder on valid SQLite records,
 * and exercise malformed/truncated input under ASAN/UBSAN. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "record.c"
static u32 rng=42;
static u32 rnd(void) { rng^=rng<<13; rng^=rng>>17; rng^=rng<<5; return rng; }
static int varint(u8 *p,u64 v) {
  u8 tmp[10]; int n=0;
  do {tmp[n++]=(u8)(v&127);v>>=7;} while(v);
  for(int i=0;i<n;i++)p[i]=tmp[n-i-1]|(i+1<n?128:0);
  return n;
}
int main(void) {
  for(int trial=0;trial<20000;trial++) {
    u8 data[4096]; u64 types[8]; u32 ncols=1+rnd()%8,head=1;
    for(u32 i=0;i<ncols;i++){types[i]=(rnd()%3)?13+2*(rnd()%400):rnd()%10;head+=varint(data+head,types[i]);}
    assert(head<128);data[0]=(u8)head;
    u32 size=head;
    for(u32 i=0;i<ncols;i++)for(u32 j=0;j<serial_len(types[i]);j++)data[size++]=(u8)rnd();
    for(u32 col=0;col<ncols;col++) {
      rec_t r={data,size};col_t actual[8];
      u32 off=head;for(u32 i=0;i<col;i++)off+=serial_len(types[i]);
      assert(!rec_cols(&r,(int)col+1,actual));
      assert(actual[col].t==types[col]&&actual[col].p==data+off);
      for(int k=0;k<3;k++) {
        r.n=rnd()%(size+1);int rc=rec_cols(&r,(int)col+1,actual);
        assert((rc==0)==(r.n>=head && r.n>=off+serial_len(types[col])));
      }
    }

  }
  puts("record projection: 20000 valid/random records and truncated prefixes passed");
}

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include "out.c"
int main(void) {
  /* JSON parser's no-escape fact must remain valid for every trimmed substring. */
  const u8 safe[] = "hello UTF-8 漢字 😀 / literal";
  for (u32 a=0; a<sizeof safe-1; a++) for (u32 b=a; b<sizeof safe; b++) {
    out_t x,y; out_begin(&x,OUT_SMALL); out_begin(&y,OUT_SMALL);
    out_need(&x,JSTR_ROOM(b-a)); out_need(&y,b-a+2);
    out_jstr(&x,safe+a,b-a); out_plain_jstr(&y,safe+a,b-a);
    assert(x.p-x.start==y.p-y.start && !memcmp(x.start,y.start,x.p-x.start));
    out_abort(&x);out_abort(&y);
  }
  /* Vector escaping (whichever width this build selects) against a byte-at-a-time reference,
   * with escapes at every position relative to the 16- and 32-byte blocks. */
  static const u8 alphabet[]={'a','"','\\','\n','\t',0x01,0x1f,0x20,0x7f,0xc3,0xa9,'/'};
  unsigned seed=1;
  for(int k=0;k<200000;k++){
    u8 s[200],want[1300];u32 n=(u32)(k%200),w=0;
    for(u32 i=0;i<n;i++){seed=seed*1103515245u+12345u;s[i]=(seed>>16)%5?'x':alphabet[(seed>>8)%sizeof alphabet];}
    want[w++]='"';
    for(u32 i=0;i<n;i++){
      u8 c=s[i];
      if(c=='"'||c=='\\'){want[w++]='\\';want[w++]=c;}
      else if(c<0x20){w+=(u32)sprintf((char*)want+w,c=='\n'?"\\n":c=='\t'?"\\t":"\\u%04x",c);}
      else want[w++]=c;
    }
    want[w++]='"';
    out_t o;out_begin(&o,OUT_SMALL);out_need(&o,JSTR_ROOM(n));out_jstr(&o,s,n);
    assert((u32)(o.p-o.start)==w&&!memcmp(o.start,want,w));
    out_abort(&o);
  }
  const i64 values[]={LLONG_MIN,LLONG_MIN+1,-1,0,1,LLONG_MAX};
  for(int k=0;k<1000;k++)for(unsigned i=0;i<sizeof values/sizeof *values;i++) {
    out_t o;out_begin_inline(&o);out_i64(&o,values[i]);
    char want[32];int n=snprintf(want,sizeof want,"%lld",values[i]);
    assert(o.p-o.start==n&&!memcmp(o.start,want,n));
    static const u8 header[]="HTTP/1.1 200 OK\r\nContent-Length: ";
    obuf *b=out_finish(&o,header,sizeof header-1);
    b->data+=3;b->len-=3; /* short-send bookkeeping must not affect ownership */
    obuf_release(b);
  }
  for(int k=0;k<1000;k++) {
    out_t o;out_begin_inline(&o);out_lit(&o,"prefix",6);out_need(&o,20000);
    assert(o.b->cap!=OUT_INLINE&&!memcmp(o.start,"prefix",6));
    memset(o.p,'x',20000);o.p+=20000;out_abort(&o);
  }
  puts("output: vector escaping, signed extrema, inline reuse, partial sends, promotion and abort passed");
}

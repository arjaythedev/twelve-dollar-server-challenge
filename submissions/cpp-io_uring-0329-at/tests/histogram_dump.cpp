#include "shim.h"
#include <cstdio>
#include <vector>
int main(int argc,char**argv){
 if(argc!=3)return 2;
 char err[256]{}; dbx* h=dbx_open(argv[1],err,sizeof err);
 if(!h){std::fprintf(stderr,"%s\n",err);return 3;}
 if(dbx_begin(h))return 4;
 long long lo=0,hi=0; int r=dbx_likes_range(h,&lo,&hi);
 if(r!=1)return 5;
 std::vector<unsigned> counts(static_cast<size_t>(hi-lo+1));
 if(dbx_likes_fill(h,lo,counts.data(),counts.size()))return 6;
 FILE*f=std::fopen(argv[2],"wb"); if(!f)return 7;
 std::fwrite(&lo,sizeof lo,1,f); std::fwrite(&hi,sizeof hi,1,f); std::fwrite(counts.data(),sizeof(unsigned),counts.size(),f); std::fclose(f);
 dbx_commit(h); dbx_close(h);
}

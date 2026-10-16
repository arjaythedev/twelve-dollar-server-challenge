#include "native.h"
#include <assert.h>

static void check_pool(Pool *pool) {
    void **blocks=calloc(POOL_PAGES,sizeof(*blocks));
    assert(blocks);
    assert(!pool_get(pool,0) && !pool_get(pool,OUTPUT_CAP+1));
    for (size_t i=0; i<POOL_PAGES; i++) {
        blocks[i]=pool_get(pool,4096);
        assert(blocks[i] && (uintptr_t)blocks[i]%64==0);
        memset(blocks[i],i%251,4096);
    }
    assert(pool->inuse==POOL_CAP && !pool_get(pool,1));
    /* A coprime stride visits every block and breaks sequential free order. */
    for (size_t i=0; i<POOL_PAGES; i++) {
        size_t index=(i*8191)%POOL_PAGES;
        unsigned char *data=blocks[index];
        for (size_t j=0; j<4096; j++) assert(data[j]==index%251);
        pool_put(pool,data,4096);
    }
    assert(pool->inuse==0);
    for (int i=0; i<128; i++) { blocks[i]=pool_get(pool,OUTPUT_CAP); assert(blocks[i]); }
    assert(!pool_get(pool,4096));
    for (int i=127; i>=0; i--) pool_put(pool,blocks[i],OUTPUT_CAP);
    assert(pool->inuse==0);
    free(blocks);

    /* Independently track page ownership under a reproducible mixed-size trace. */
    unsigned char occupied[POOL_PAGES]={0};
    void *live[512]={0}; size_t sizes[512]={0};
    uint32_t seed=42; size_t expected=0;
    for (int turn=0; turn<20000; turn++) {
        seed=seed*1664525U+1013904223U;
        int slot=(seed>>16)%512;
        if (live[slot]) {
            size_t page=((char*)live[slot]-pool->arena)/4096;
            for (size_t j=0; j<sizes[slot]/4096; j++) {
                assert(occupied[page+j]); occupied[page+j]=0;
            }
            unsigned char *bytes=live[slot];
            assert(bytes[0]==slot%251 && bytes[sizes[slot]-1]==slot%251);
            pool_put(pool,live[slot],sizes[slot]);
            expected-=sizes[slot]; live[slot]=NULL;
        } else {
            size_t size=(size_t)4096<<((seed>>8)%9);
            void *data=pool_get(pool,size);
            if (data) {
                size_t page=((char*)data-pool->arena)/4096;
                assert(page%(size/4096)==0);
                for (size_t j=0; j<size/4096; j++) {
                    assert(!occupied[page+j]); occupied[page+j]=1;
                }
                memset(data,slot%251,size);
                live[slot]=data; sizes[slot]=size; expected+=size;
            }
        }
        assert(pool->inuse==expected && expected<=POOL_CAP);
    }
    for (int i=0; i<512; i++) pool_put(pool,live[i],sizes[i]);
    assert(pool->inuse==0);
    puts("ok exhaustion, mixed sizes, nonoverlap, coalescing, recovery");
}

static void expect_writer(Writer *writer, const char *expected, size_t n) {
    assert(writer->len==n);
    size_t pos=0;
    for (Chunk *chunk=writer->head; chunk; chunk=chunk->next) {
        assert(chunk->len<=sizeof(chunk->data));
        assert(memcmp(chunk->data,expected+pos,chunk->len)==0);
        pos+=chunk->len;
    }
    assert(pos==n);
}

static void check_encoding(Pool *pool) {
    Writer w={0};
    assert(!writer_int64(pool,&w,INT64_MIN));
    assert(!writer_literal(pool,&w,","));
    assert(!writer_int64(pool,&w,INT64_MAX));
    assert(!writer_literal(pool,&w,",0,"));
    assert(!writer_string(pool,&w,"a\0\1\n\"\\z",7));
    const char expected[]="-9223372036854775808,9223372036854775807,0,\"a\\u0000\\u0001\\u000a\\\"\\\\z\"";
    expect_writer(&w,expected,sizeof(expected)-1);
    assert(!writer_reset(pool,&w,0));
    assert(!writer_string(pool,&w,"",0)); expect_writer(&w,"\"\"",2);
    writer_release(pool,&w);
    char *large=malloc(OUTPUT_CAP); assert(large); memset(large,'x',OUTPUT_CAP);
    assert(!writer_append(pool,&w,large,OUTPUT_CAP));
    assert(writer_append(pool,&w,"x",1)==-1);
    expect_writer(&w,large,OUTPUT_CAP);
    writer_release(pool,&w); free(large);

    Request *requests[128];
    for (int i=0; i<128; i++) {
        requests[i]=request_acquire(pool); assert(requests[i]);
        assert((uintptr_t)requests[i]%64==0); request_reset(requests[i]);
        assert(!buf_append(pool,&requests[i]->auth,"token",5));
        assert(!buf_append(pool,&requests[i]->body,"body",4));
    }
    for (int i=127; i>=0; i--) request_release(pool,requests[i]);
    assert(pool->inuse==0);
    Buffer b={0}; assert(!buf_append(pool,&b,NULL,0));
    assert(buf_append(pool,&b,"x",OUTPUT_CAP+1)==-1); buf_release(pool,&b);
    puts("ok integer boundaries, JSON escaping, output cap, request ownership");
}

static void check_sends(Pool *pool) {
    size_t size=250000, skip=37;
    char *source=malloc(size); assert(source);
    for (size_t i=0; i<size; i++) source[i]=i%251;
    for (int cancel=0; cancel<2; cancel++) {
        Writer writer={0}; Output output={0};
        assert(!writer_append(pool,&writer,source,size));
        Chunk *original=writer.head;
        output_take(&output,&writer,skip);
        assert(!writer.head && output.head==original);
        int fd[2]; assert(!socketpair(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK,0,fd));
        int limit=4096; assert(!setsockopt(fd[0],SOL_SOCKET,SO_SNDBUF,&limit,sizeof(limit)));
        int blocked=0;
        for (int i=0; i<100; i++) {
            ssize_t n=output_send(pool,fd[0],&output);
            if (n<0) { assert(is_again()); blocked=1; break; }
        }
        assert(blocked && output.len>0);
        if (!cancel) {
            size_t received=0; char data[509];
            while (received<size-skip) {
                if (output.len) {
                    ssize_t n=output_send(pool,fd[0],&output);
                    assert(n>=0 || is_again());
                }
                ssize_t n=recv(fd[1],data,sizeof(data),0);
                if (n<0) { assert(is_again()); continue; }
                assert(n>0 && received+n<=size-skip);
                assert(!memcmp(data,source+skip+received,n)); received+=n;
            }
            assert(!output.len && !output.head);
        }
        output_release(pool,&output); close(fd[0]); close(fd[1]);
        assert(pool->inuse==0);
    }
    free(source);
    puts("ok scatter/gather over 32 chunks, partial sends, EAGAIN, cancellation");
}

int main(int argc, char **argv) {
    Pool pool={0}; assert(!pool_init(&pool));
    if (argc>1) {
        volatile char *data=pool_get(&pool,4096); assert(data);
        if (!strcmp(argv[1],"--overflow")) data[4096]=1;
        else if (!strcmp(argv[1],"--uaf")) { pool_put(&pool,(void*)data,4096); data[0]=1; }
        else if (!strcmp(argv[1],"--double-free")) { pool_put(&pool,(void*)data,4096); pool_put(&pool,(void*)data,4096); }
        else return 2;
        return 0; /* check_native.py requires these invalid accesses to fail. */
    }
    check_pool(&pool); check_encoding(&pool); check_sends(&pool);
    assert(pool.inuse==0); pool_destroy(&pool);
    puts("passed native ownership checks");
    return 0;
}

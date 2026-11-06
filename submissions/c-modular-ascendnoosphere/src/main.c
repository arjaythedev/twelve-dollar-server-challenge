#define _GNU_SOURCE
#include "abi.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct { Context ctx; void *handles[6]; int descriptors[6]; } Bundle;
static volatile sig_atomic_t stopping, reloading;
static volatile sig_atomic_t signal_wake=-1, wake_pending;
static void signal_handler(int signo) {
    int saved_errno=errno;
    if (signo==SIGHUP) reloading=1; else stopping=1;
    int fd=signal_wake;
    if(fd>=0) {
        uint64_t one=1;ssize_t n;
        do{n=write(fd,&one,sizeof(one));}while(n<0 && errno==EINTR);
    }
    wake_pending=1;
    errno=saved_errno;
}

static void destroy(Bundle *b) {
    if (!b) return;
    if (b->ctx.database) b->ctx.store->close(b->ctx.database);
    if (b->ctx.authentication) b->ctx.auth->close(b->ctx.authentication);
#ifndef ASC_BUILTIN
    for (int i=5;i>=0;--i) {
        if (b->handles[i]) dlclose(b->handles[i]);
        if (b->descriptors[i]>=0) close(b->descriptors[i]);
    }
#endif
    free(b);
}
#ifndef ASC_BUILTIN
static int snapshot_library(const char *path, const char *name) {
    int source=open(path,O_RDONLY|O_CLOEXEC);
    if (source<0) return -1;
    struct stat st;
    if (fstat(source,&st) || !S_ISREG(st.st_mode) || st.st_size<=0 || st.st_size>64*1024*1024) { close(source); return -1; }
    int target=memfd_create(name,MFD_CLOEXEC|MFD_ALLOW_SEALING);
    if (target<0) { close(source); return -1; }
    char block[65536]; ssize_t got;
    while ((got=read(source,block,sizeof(block)))!=0) {
        if (got<0) { if (errno==EINTR) continue; goto fail; }
        size_t offset=0;
        while (offset<(size_t)got) {
            ssize_t n=write(target,block+offset,(size_t)got-offset);
            if (n<0 && errno==EINTR) continue;
            if (n<=0) goto fail;
            offset+=(size_t)n;
        }
    }
    close(source);
    if (fcntl(target,F_ADD_SEALS,F_SEAL_WRITE|F_SEAL_GROW|F_SEAL_SHRINK|F_SEAL_SEAL)) { close(target); return -1; }
    return target;
fail:
    close(source); close(target); return -1;
}
#else
const void *asc_math_module(void),*asc_codec_module(void),*asc_auth_module(void);
const void *asc_store_module(void),*asc_app_module(void),*asc_transport_module(void);
#endif
static Bundle *load_bundle(const char *directory, const char *db, const char *secret, struct timespec started) {
    static const char *const names[]={"math","codec","auth","store","app","transport"};
    static const size_t sizes[]={sizeof(MathAPI),sizeof(CodecAPI),sizeof(AuthAPI),sizeof(StoreAPI),sizeof(AppAPI),sizeof(TransportAPI)};
#ifndef ASC_BUILTIN
    char frozen[PATH_MAX];
    if (!realpath(directory,frozen)) { perror("module generation"); return NULL; }
#else
    (void)directory;
#endif
    Bundle *b=calloc(1,sizeof(*b));
    if (!b) return NULL;
    for (unsigned i=0;i<6;++i) b->descriptors[i]=-1;
    const void *tables[6]={0};
#ifdef ASC_BUILTIN
    tables[0]=asc_math_module();tables[1]=asc_codec_module();tables[2]=asc_auth_module();
    tables[3]=asc_store_module();tables[4]=asc_app_module();tables[5]=asc_transport_module();
#else
    for (unsigned i=0;i<6;++i) {
        char path[PATH_MAX], fdpath[64];
        int n=snprintf(path,sizeof(path),"%s/%s.so",frozen,names[i]);
        if (n<0 || (size_t)n>=sizeof(path)) goto fail;
        b->descriptors[i]=snapshot_library(path,names[i]);
        if (b->descriptors[i]<0) { perror(path); goto fail; }
        snprintf(fdpath,sizeof(fdpath),"/proc/self/fd/%d",b->descriptors[i]);
        b->handles[i]=dlopen(fdpath,RTLD_NOW|RTLD_LOCAL);
        if (!b->handles[i]) { fprintf(stderr,"dlopen %s: %s\n",names[i],dlerror()); goto fail; }
        const void *(*entry)(void)=(const void *(*)(void))dlsym(b->handles[i],"asc_module");
        if (!entry) goto fail;
        tables[i]=entry();
        const ModuleHeader *header=tables[i];
        if (!header || header->abi!=ASC_ABI || header->size!=sizes[i] || !header->name) {
            fprintf(stderr,"module ABI mismatch: %s\n",names[i]); goto fail;
        }
    }
#endif
    for(unsigned i=0;i<6;++i) {
        const ModuleHeader *header=tables[i];
        if(!header || header->abi!=ASC_ABI || header->size!=sizes[i]) {
            fprintf(stderr,"invalid application component: %s\n",names[i]);goto fail;
        }
    }
    b->ctx.math=tables[0]; b->ctx.codec=tables[1]; b->ctx.auth=tables[2];
    b->ctx.store=tables[3]; b->ctx.app=tables[4]; b->ctx.transport=tables[5];
    const MathAPI *m=b->ctx.math; const CodecAPI *c=b->ctx.codec;
    const AuthAPI *a=b->ctx.auth; const StoreAPI *s=b->ctx.store; const TransportAPI *t=b->ctx.transport;
    if (!m->special || !m->header_end || !m->decimal || !m->route || !m->base64url ||
        !c->body || !c->jwt_header || !c->claims || !c->string || !c->integer || !c->post ||
        !a->open || !a->close || !a->verify || !s->open || !s->close || !s->health || !s->feed ||
        !s->post || !s->create || !s->like || !b->ctx.app->handle || !t->open || !t->step || !t->close ||
        t->state_size!=sizeof(TransportState) || t->connection_size!=sizeof(Connection)) goto fail;
    b->ctx.started=started;
    b->ctx.authentication=a->open(secret);
    if (!b->ctx.authentication) goto fail;
    b->ctx.database=s->open(db);
    if (!b->ctx.database) goto fail;
    return b;
fail:
    destroy(b); return NULL;
}
int main(void) {
    const char *db=getenv("SQLITE_PATH"), *secret=getenv("JWT_SECRET"), *host=getenv("HOST"), *port=getenv("PORT");
    if (!db || !secret) { fprintf(stderr,"SQLITE_PATH and JWT_SECRET are required\n"); return 1; }
    if (!host) host="127.0.0.1";
    if (!port) port="3000";
    struct sigaction action={0}; action.sa_handler=signal_handler; sigemptyset(&action.sa_mask);
    sigaction(SIGTERM,&action,NULL); sigaction(SIGINT,&action,NULL); sigaction(SIGHUP,&action,NULL);
    signal(SIGPIPE,SIG_IGN);
    char directory[PATH_MAX]={0};
#ifndef ASC_BUILTIN
    char executable[PATH_MAX];
    ssize_t n=readlink("/proc/self/exe",executable,sizeof(executable)-1);
    if (n<=0 || (size_t)n>=sizeof(executable)-1) return 1;
    executable[n]=0; char *slash=strrchr(executable,'/');
    if (!slash) return 1;
    *slash=0;
    int size=snprintf(directory,sizeof(directory),"%s/modules",executable);
    if (size<0 || (size_t)size>=sizeof(directory)) return 1;
#endif
    struct timespec started; clock_gettime(CLOCK_MONOTONIC,&started);
    Bundle *bundle=load_bundle(directory,db,secret,started);
    if (!bundle) return 1;
    TransportState *network=bundle->ctx.transport->open(host,port);
    if (!network) { perror("listen"); destroy(bundle); return 1; }
    int wake=eventfd(0,EFD_CLOEXEC|EFD_NONBLOCK);
    struct epoll_event event={.events=EPOLLIN,.data.fd=-1};
    if(wake<0 || epoll_ctl(network->epoll,EPOLL_CTL_ADD,wake,&event)) {
        perror("signal wake");if(wake>=0)close(wake);
        bundle->ctx.transport->close(network);destroy(bundle);return 1;
    }
    signal_wake=wake;
    fprintf(stderr,"listening %s:%s; math=%s; SQLite=3.54.1; ABI=%08x\n",host,port,bundle->ctx.math->h.name,ASC_ABI);
    int result=0;
    for (;;) {
        if(wake_pending) {
            wake_pending=0;
            uint64_t signals;ssize_t got;
            do{got=read(wake,&signals,sizeof(signals));}while(got<0 && errno==EINTR);
        }
        if(stopping)break;
        if (reloading) {
            reloading=0;
#ifdef ASC_BUILTIN
            fprintf(stderr,"SIGHUP: linked build remains active; restart to load a new executable\n");
#else
            Bundle *next=load_bundle(directory,db,secret,started);
            if (next) {
                Bundle *old=bundle; bundle=next; destroy(old);
                fprintf(stderr,"reload committed; math=%s; connections preserved\n",bundle->ctx.math->h.name);
            } else fprintf(stderr,"reload rejected; previous generation remains active\n");
#endif
            continue;
        }
        if (bundle->ctx.transport->step(network,&bundle->ctx,-1)<0) { perror("event loop"); result=1; break; }
    }
    fprintf(stderr,"stopped; requests=%llu accepted=%llu\n",(unsigned long long)network->requests,(unsigned long long)network->accepted);
    bundle->ctx.transport->close(network);destroy(bundle);
    signal_wake=-1;close(wake);return result;
}

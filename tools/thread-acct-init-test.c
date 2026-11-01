/* Standalone POSIX race regression: cc -std=c11 -pthread this-file.c -o test */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>
#include <stdint.h>
#include <time.h>
#include <stdlib.h>
#include <stdio.h>
static atomic_int reporters,start,failed;
static int expected;
static int counted_create(pthread_t *t,const pthread_attr_t *a,void *(*f)(void *),void *p) {
    atomic_fetch_add(&reporters,1);return pthread_create(t,a,f,p);
}
#define pthread_create counted_create
#include "../external/xboxrecomp/src/platform/thread_acct.c"
#undef pthread_create
uint64_t xbox_HostNowNs(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000u+t.tv_nsec;
}
static void *worker(void *unused) {
    (void)unused;while(!atomic_load(&start)) sched_yield();
    for(unsigned i=0;i<1000;i++)
        if(xbox_ThreadAcctInit()!=expected || xbox_ThreadAcctEnabled()!=expected) atomic_store(&failed,1);
    return NULL;
}
int main(int argc,char **argv) {
    pthread_t threads[32];expected=argc>1 && argv[1][0]=='1';
    setenv("RECOMP_THREAD_LOG",expected?"1":"0",1);
    for(unsigned i=0;i<32;i++) if(pthread_create(&threads[i],NULL,worker,NULL)) return 2;
    atomic_store(&start,1);
    for(unsigned i=0;i<32;i++) pthread_join(threads[i],NULL);
    if(atomic_load(&failed) || atomic_load(&reporters)!=expected) return 1;
    printf("PASS:32 simultaneous callers,32000 init calls,reporters=%d\n",atomic_load(&reporters));return 0;
}

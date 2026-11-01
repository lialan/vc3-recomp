#define _GNU_SOURCE
#include "guest_write_watch.h"
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sched.h>
#include <limits.h>
#define CHECK(c) do {if(!(c)) {fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#c);exit(1);}}while(0)
static ChGuestWriteWatch *watch;
static atomic_uint faults;
static void handler(int sig,siginfo_t *info,void *context) {
    (void)sig;
    if(ch_guest_write_watch_fault(watch,(uintptr_t)info->si_addr,ch_guest_write_fault_is_write(context))) {
        atomic_fetch_add_explicit(&faults,1,memory_order_relaxed);return;
    }
    _exit(99);
}
struct Writer {unsigned char *p;size_t page;atomic_int phase;};
static void *writer(void *opaque) {
    struct Writer *w=opaque;
    for(unsigned i=0;i<1000;i++) {
        while(atomic_load_explicit(&w->phase,memory_order_acquire)!=1) sched_yield();
        w->p[(i&1)*w->page+137]=(unsigned char)i;
        atomic_store_explicit(&w->phase,2,memory_order_release);
    }
    return NULL;
}
struct Reader {unsigned char *p;int fd,result;atomic_int phase;};
static void *reader(void *opaque) {
    struct Reader *r=opaque;
    ch_guest_write_watch_begin(watch,r->p,1);
    atomic_store_explicit(&r->phase,1,memory_order_release);
    r->result=(int)read(r->fd,r->p,1);
    ch_guest_write_watch_end(watch,r->p,1);
    return NULL;
}
int main(void) {
    size_t page=(size_t)sysconf(_SC_PAGESIZE),bytes=page*4;
    char path[PATH_MAX];const char *temp=getenv("TMPDIR");
    if(!temp) temp="/tmp";
    int path_bytes=snprintf(path,sizeof path,"%s/vc3-write-watch-XXXXXX",temp);
    CHECK(path_bytes>0 && (size_t)path_bytes<sizeof path);
    int fd=mkstemp(path);CHECK(fd>=0);CHECK(!unlink(path));CHECK(!ftruncate(fd,(off_t)bytes));
    unsigned char *a=mmap(NULL,bytes,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    unsigned char *b=mmap(NULL,bytes,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    CHECK(a!=MAP_FAILED && b!=MAP_FAILED);close(fd);
    memset(a,0,bytes);
    /* Initially read-only memory must remain ineligible and retain its
     * original protection throughout tracker creation/destruction. */
    CHECK(!mprotect(a+3*page,page,PROT_READ));
    ChGuestWriteView views[]={{a,bytes},{b,bytes}};
    watch=ch_guest_write_watch_create(views,2);CHECK(watch);
    struct sigaction sa={0},old_segv,old_bus;
    sa.sa_sigaction=handler;sa.sa_flags=SA_SIGINFO;sigemptyset(&sa.sa_mask);
    CHECK(!sigaction(SIGSEGV,&sa,&old_segv) && !sigaction(SIGBUS,&sa,&old_bus));
    CHECK(!ch_guest_write_range_create(watch,a+3*page,1));
    CHECK(!ch_guest_write_range_create(watch,a+bytes-1,2));
    CHECK(!ch_guest_write_range_create(watch,a,0));
    ChGuestWriteRange *x=ch_guest_write_range_create(watch,a+31,page+64);
    ChGuestWriteRange *y=ch_guest_write_range_create(watch,b+31,page+64);
    CHECK(x && y);
    CHECK(ch_guest_write_range_changed(x) && !ch_guest_write_range_changed(x));
    CHECK(ch_guest_write_range_changed(y) && !ch_guest_write_range_changed(y));
    b[137]=17;CHECK(a[137]==17);
    CHECK(ch_guest_write_range_changed(x) && ch_guest_write_range_changed(y));
    CHECK(!ch_guest_write_range_changed(x) && !ch_guest_write_range_changed(y));
    a[page+89]=23;
    CHECK(ch_guest_write_range_changed(y) && ch_guest_write_range_changed(x));
    CHECK(!ch_guest_write_range_changed(x));
    CHECK(!ch_guest_write_watch_fault(watch,(uintptr_t)a,0));
    CHECK(!ch_guest_write_watch_fault(watch,(uintptr_t)a+2*page,1));
    /* Actual execute fault must not be mistaken for a write and retried
     * forever by opening an NX data page. Child handler exits on unhandled. */
    pid_t child=fork();CHECK(child>=0);
    if(!child) {alarm(3);((void (*)(void))(uintptr_t)a)();_exit(1);}
    int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==99);
    /* Repeated writes through either mapping while another thread observes
     * force correct generation changes after writable intervals. */
    struct Writer w={.p=b,.page=page};atomic_init(&w.phase,0);
    pthread_t thread;CHECK(!pthread_create(&thread,NULL,writer,&w));
    for(unsigned i=0;i<1000;i++) {
        (void)ch_guest_write_range_changed(x);
        CHECK(!ch_guest_write_range_changed(x));
        atomic_store_explicit(&w.phase,1,memory_order_release);
        while(atomic_load_explicit(&w.phase,memory_order_acquire)!=2) (void)ch_guest_write_range_changed(y);
        CHECK(ch_guest_write_range_changed(x));
        CHECK(a[(i&1)*page+137]==(unsigned char)i);
        atomic_store_explicit(&w.phase,0,memory_order_release);
    }
    CHECK(!pthread_join(thread,NULL));
    /* A blocked kernel read holds a write lease. Repeated observer calls
     * must not reprotect its destination and turn read() into EFAULT. */
    int pipefd[2];CHECK(!pipe(pipefd));
    struct Reader r={.p=b+139,.fd=pipefd[0]};atomic_init(&r.phase,0);
    CHECK(!pthread_create(&thread,NULL,reader,&r));
    while(!atomic_load_explicit(&r.phase,memory_order_acquire)) sched_yield();
    for(unsigned i=0;i<100;i++) CHECK(ch_guest_write_range_changed(x));
    CHECK(!ch_guest_write_watch_destroy(watch)); /* lease prevents destruction */
    CHECK(write(pipefd[1],"z",1)==1);CHECK(!pthread_join(thread,NULL));
    CHECK(r.result==1 && a[139]=='z');close(pipefd[0]);close(pipefd[1]);
    CHECK(ch_guest_write_range_changed(x) && !ch_guest_write_range_changed(x));
    /* Protection changes invalidate tracking in all aliases and must not be
     * undone on destruction. Other pages still retain their fast path. */
    ch_guest_write_watch_invalidate(watch,b+page,1);
    CHECK(!mprotect(b+page,page,PROT_READ));
    CHECK(ch_guest_write_range_changed(x) && ch_guest_write_range_changed(x));
    CHECK(!ch_guest_write_watch_fault(watch,(uintptr_t)b+page,1));
    ch_guest_write_range_destroy(x);ch_guest_write_range_destroy(y);
    CHECK(ch_guest_write_watch_destroy(watch));watch=NULL;
    /* Destruction restores pages it armed; these stores must not fault. */
    a[137]=42;b[137]=43;CHECK(a[137]==43);
    child=fork();CHECK(child>=0);
    if(!child) {alarm(3);b[page]=1;_exit(1);}
    CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==99);
    CHECK(!sigaction(SIGSEGV,&old_segv,NULL) && !sigaction(SIGBUS,&old_bus,NULL));
    CHECK(!munmap(a,bytes) && !munmap(b,bytes));
    CHECK(atomic_load(&faults)>=1002);
    puts("PASS: exact alias write tracking, independent observers, actual write/NX fault decoding, 1000 concurrent writable/rearm cycles, kernel read lease, protection invalidation and teardown");
    return 0;
}

#define _GNU_SOURCE
#include "guest_write_watch.h"
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#if !defined(_WIN32)
#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>
#if defined(__APPLE__)
#include <sys/ucontext.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#else
#include <ucontext.h>
#endif
_Static_assert(ATOMIC_INT_LOCK_FREE==2 && ATOMIC_LLONG_LOCK_FREE==2,
    "write fault handler requires lock-free atomics");
struct Page {
    atomic_uint lock,state,writable; /* state: 0 unused, 1 watched, 2 disabled */
    atomic_ullong generation;
    unsigned leases,permissions,aliases;
};
struct ChGuestWriteWatch {
    ChGuestWriteView *views;size_t count,page_size,pages;
    struct Page *page;
    uint64_t ranges,checks,unchanged,single_pass_checks,single_pass_pages;
    atomic_ullong faults,protect_calls;
};
struct ChGuestWriteRange {
    ChGuestWriteWatch *watch;size_t first,count;uint64_t stamp[];
};
static void lock_page(struct Page *p) {
    while(atomic_exchange_explicit(&p->lock,1,memory_order_acquire)) {}
}
static void unlock_page(struct Page *p) {atomic_store_explicit(&p->lock,0,memory_order_release);}
static int protect_pages(ChGuestWriteWatch *w,size_t first,size_t count,int prot) {
    size_t at=first*w->page_size,bytes=count*w->page_size;int ok=1;
    for(size_t v=0;v<w->count;v++) if(at<w->views[v].bytes)
    {
        atomic_fetch_add_explicit(&w->protect_calls,1,memory_order_relaxed);
        if(mprotect((unsigned char *)w->views[v].base+at,
            bytes<w->views[v].bytes-at?bytes:w->views[v].bytes-at,prot)) ok=0;
    }
    return ok;
}
static int protect_page(ChGuestWriteWatch *w,size_t i,int prot) {return protect_pages(w,i,1,prot);}
static void permissions(ChGuestWriteWatch *w,size_t view,uintptr_t lo,uintptr_t hi) {
    uintptr_t base=(uintptr_t)w->views[view].base,end=base+w->views[view].bytes;
    if(lo<base) lo=base;
    if(hi>end) hi=end;
    if(hi<=lo) return;
    size_t first=(lo-base+w->page_size-1)/w->page_size,last=(hi-base)/w->page_size;
    for(size_t i=first;i<last;i++) w->page[i].permissions++;
}
static int read_permissions(ChGuestWriteWatch *w) {
#if defined(__linux__)
    FILE *f=fopen("/proc/self/maps","r");
    if(!f) return 0;
    char *line=NULL;size_t capacity=0;
    while(getline(&line,&capacity,f)>=0) {
        unsigned long long lo,hi;char flags[5];
        if(sscanf(line,"%llx-%llx %4s",&lo,&hi,flags)!=3) continue;
        if(flags[0]!='r' || flags[1]!='w' || flags[2]!='-') continue;
        for(size_t v=0;v<w->count;v++) permissions(w,v,(uintptr_t)lo,(uintptr_t)hi);
    }
    int ok=!ferror(f);free(line);fclose(f);return ok;
#elif defined(__APPLE__)
    for(size_t v=0;v<w->count;v++) {
        mach_vm_address_t at=(uintptr_t)w->views[v].base;
        uintptr_t end=(uintptr_t)w->views[v].base+w->views[v].bytes;
        while(at<end) {
            mach_vm_size_t size=0;mach_vm_address_t begin=at;
            vm_region_basic_info_data_64_t info;
            mach_msg_type_number_t n=VM_REGION_BASIC_INFO_COUNT_64;
            mach_port_t object=MACH_PORT_NULL;
            kern_return_t r=mach_vm_region(mach_task_self(),&begin,&size,VM_REGION_BASIC_INFO_64,
                (vm_region_info_t)&info,&n,&object);
            if(object!=MACH_PORT_NULL) mach_port_deallocate(mach_task_self(),object);
            if(r!=KERN_SUCCESS || !size || begin>UINTPTR_MAX-size || begin+size<=at) return 0;
            if(info.protection==(VM_PROT_READ|VM_PROT_WRITE)) permissions(w,v,begin,begin+size);
            at=begin+size;
        }
    }
    return 1;
#else
    (void)w;return 0;
#endif
}
ChGuestWriteWatch *ch_guest_write_watch_create(const ChGuestWriteView *views,size_t count) {
    long page_size=sysconf(_SC_PAGESIZE);
    if(!views || !count || count>32 || page_size<=0) return NULL;
    size_t size=(size_t)page_size,bytes=0;
    for(size_t v=0;v<count;v++) {
        uintptr_t base=(uintptr_t)views[v].base;
        if(!base || base%size || !views[v].bytes || views[v].bytes%size ||
            views[v].bytes>UINTPTR_MAX-base) return NULL;
        for(size_t j=0;j<v;j++) if(base<(uintptr_t)views[j].base+views[j].bytes &&
            (uintptr_t)views[j].base<base+views[v].bytes) return NULL;
        if(views[v].bytes>bytes) bytes=views[v].bytes;
    }
    ChGuestWriteWatch *w=calloc(1,sizeof *w);
    if(!w) return NULL;
    w->views=malloc(count*sizeof *views);w->pages=bytes/size;
    w->page=calloc(w->pages,sizeof *w->page);
    if(!w->views || !w->page) {free(w->views);free(w->page);free(w);return NULL;}
    memcpy(w->views,views,count*sizeof *views);w->count=count;w->page_size=size;
    atomic_init(&w->faults,0);atomic_init(&w->protect_calls,0);
    for(size_t i=0;i<w->pages;i++) {
        atomic_init(&w->page[i].lock,0);atomic_init(&w->page[i].state,0);
        atomic_init(&w->page[i].writable,1);atomic_init(&w->page[i].generation,1);
        for(size_t v=0;v<count;v++) w->page[i].aliases+=i*size<views[v].bytes;
    }
    if(!read_permissions(w)) {free(w->views);free(w->page);free(w);return NULL;}
    for(size_t i=0;i<w->pages;i++) if(w->page[i].permissions!=w->page[i].aliases)
        atomic_store(&w->page[i].state,2);
    return w;
}
static int page_at(ChGuestWriteWatch *w,uintptr_t address,size_t *index) {
    if(w) for(size_t v=0;v<w->count;v++) {
        uintptr_t base=(uintptr_t)w->views[v].base;
        if(address>=base && address-base<w->views[v].bytes) {
            *index=(address-base)/w->page_size;return 1;
        }
    }
    return 0;
}
ChGuestWriteRange *ch_guest_write_range_create(ChGuestWriteWatch *w,const void *memory,size_t bytes) {
    size_t first;
    if(!bytes || !page_at(w,(uintptr_t)memory,&first) || bytes>UINTPTR_MAX-(uintptr_t)memory) return NULL;
    int contained=0;
    for(size_t v=0;v<w->count;v++) {
        uintptr_t base=(uintptr_t)w->views[v].base,address=(uintptr_t)memory;
        if(address>=base && address-base<w->views[v].bytes && bytes<=w->views[v].bytes-(address-base)) contained=1;
    }
    if(!contained) return NULL;
    size_t last;
    if(!page_at(w,(uintptr_t)memory+bytes-1,&last) || last<first) return NULL;
    size_t count=last-first+1;
    if(count>(SIZE_MAX-sizeof(ChGuestWriteRange))/sizeof(uint64_t)) return NULL;
    ChGuestWriteRange *r=calloc(1,sizeof *r+count*sizeof(uint64_t));
    if(!r) return NULL;
    r->watch=w;r->first=first;r->count=count;
    for(size_t i=first;i<=last;i++) {
        struct Page *p=&w->page[i];lock_page(p);
        if(atomic_load_explicit(&p->state,memory_order_relaxed)==2) {unlock_page(p);free(r);return NULL;}
        atomic_store_explicit(&p->state,1,memory_order_release);unlock_page(p);
    }
    w->ranges++;return r;
}
void ch_guest_write_range_destroy(ChGuestWriteRange *r) {free(r);}
int ch_guest_write_range_changed(ChGuestWriteRange *r) {
    if(!r) return 1;
    ChGuestWriteWatch *w=r->watch;int changed=0,needs_arm=0;
    w->checks++;
    /* Stable protected ranges need one scan, not a separate arming probe and
     * generation scan. If any page needs arming, retain the locked full rescan
     * below so writable intervals and active leases stay conservative. */
    for(size_t j=0;j<r->count;j++) {
        struct Page *p=&w->page[r->first+j];
        if(atomic_load_explicit(&p->state,memory_order_acquire)!=1) {
            changed=1;r->stamp[j]=0;continue;
        }
        if(atomic_load_explicit(&p->writable,memory_order_acquire)) {needs_arm=1;break;}
        uint64_t generation=atomic_load_explicit(&p->generation,memory_order_acquire);
        changed|=r->stamp[j]!=generation;r->stamp[j]=generation;
    }
    if(!needs_arm) {
        w->single_pass_checks++;w->single_pass_pages+=r->count;
        if(!changed) w->unchanged++;
        return changed;
    }
    if(needs_arm) {
        /* Lock in backing-page order. Fault handlers own only one page and
         * never access guest bytes while holding its lock. Adjacent pages can
         * now be protected with one syscall per alias instead of per page. */
        for(size_t j=0;j<r->count;j++) lock_page(&w->page[r->first+j]);
        for(size_t j=0;j<r->count;) {
            struct Page *p=&w->page[r->first+j];
            if(atomic_load_explicit(&p->state,memory_order_relaxed)!=1 || p->leases ||
                !atomic_load_explicit(&p->writable,memory_order_relaxed)) {j++;continue;}
            size_t end=j+1;
            while(end<r->count) {
                struct Page *next=&w->page[r->first+end];
                if(atomic_load_explicit(&next->state,memory_order_relaxed)!=1 || next->leases ||
                    !atomic_load_explicit(&next->writable,memory_order_relaxed)) break;
                end++;
            }
            /* Force a new generation after every writable interval, even
             * when another observer saw its fault before the store finished. */
            for(size_t k=j;k<end;k++) atomic_fetch_add_explicit(&w->page[r->first+k].generation,1,memory_order_release);
            int ok=protect_pages(w,r->first+j,end-j,PROT_READ);
            if(!ok && !protect_pages(w,r->first+j,end-j,PROT_READ|PROT_WRITE)) abort();
            for(size_t k=j;k<end;k++) {
                struct Page *next=&w->page[r->first+k];
                if(ok) atomic_store_explicit(&next->writable,0,memory_order_release);
                else atomic_store_explicit(&next->state,2,memory_order_release);
            }
            changed=1;j=end;
        }
    }
    for(size_t j=0;j<r->count;j++) {
        struct Page *p=&w->page[r->first+j];
        if(atomic_load_explicit(&p->state,memory_order_acquire)!=1 ||
            atomic_load_explicit(&p->writable,memory_order_acquire)) {
            changed=1;r->stamp[j]=0;
        } else {
            uint64_t generation=atomic_load_explicit(&p->generation,memory_order_acquire);
            changed|=r->stamp[j]!=generation;r->stamp[j]=generation;
        }
    }
    if(needs_arm) for(size_t j=0;j<r->count;j++) unlock_page(&w->page[r->first+j]);
    if(!changed) w->unchanged++;
    return changed;
}
void ch_guest_write_range_rebase(ChGuestWriteRange *r) {if(r) (void)ch_guest_write_range_changed(r);}
int ch_guest_write_fault_is_write(void *context) {
    if(!context) return 0;
    ucontext_t *uc=context;
#if defined(__APPLE__) && defined(__aarch64__)
    uint64_t esr=uc->uc_mcontext->__es.__esr;
    return ((esr>>26)==0x24 || (esr>>26)==0x25) && (esr&(1u<<6));
#elif defined(__APPLE__) && defined(__x86_64__)
    return !!(uc->uc_mcontext->__es.__err&2);
#elif defined(__linux__) && defined(__x86_64__)
    return !!(uc->uc_mcontext.gregs[REG_ERR]&2);
#elif defined(__linux__) && defined(__aarch64__)
    const unsigned char *p=uc->uc_mcontext.__reserved;
    size_t bytes=sizeof uc->uc_mcontext.__reserved;
    for(size_t at=0;at+8<=bytes;) {
        uint32_t magic,size;memcpy(&magic,p+at,4);memcpy(&size,p+at+4,4);
        if(size<8 || size>bytes-at) break;
        if(magic==0x45535201 && size>=16) {
            uint64_t esr;memcpy(&esr,p+at+8,8);
            return ((esr>>26)==0x24 || (esr>>26)==0x25) && (esr&(1u<<6));
        }
        at+=size;
    }
    return 0;
#else
    (void)uc;return 0;
#endif
}
int ch_guest_write_watch_fault(ChGuestWriteWatch *w,uintptr_t address,int write_access) {
    if(!write_access) return 0;
    size_t i;if(!page_at(w,address,&i)) return 0;
    struct Page *p=&w->page[i];lock_page(p);
    if(atomic_load_explicit(&p->state,memory_order_relaxed)!=1) {unlock_page(p);return 0;}
    atomic_fetch_add_explicit(&p->generation,1,memory_order_release);
    atomic_fetch_add_explicit(&w->faults,1,memory_order_relaxed);
    int ok=protect_page(w,i,PROT_READ|PROT_WRITE);
    atomic_store_explicit(&p->writable,1,memory_order_release);
    if(!ok) atomic_store_explicit(&p->state,2,memory_order_release);
    unlock_page(p);return ok;
}
static void visit_write(ChGuestWriteWatch *w,void *memory,size_t bytes,int action) {
    if(!w || !bytes || bytes>UINTPTR_MAX-(uintptr_t)memory) return;
    uintptr_t lo=(uintptr_t)memory,hi=lo+bytes;
    for(size_t v=0;v<w->count;v++) {
        uintptr_t base=(uintptr_t)w->views[v].base,end=base+w->views[v].bytes;
        uintptr_t begin=lo>base?lo:base,limit=hi<end?hi:end;
        if(begin>=limit) continue;
        size_t first=(begin-base)/w->page_size,last=(limit-1-base)/w->page_size;
        for(size_t i=first;i<=last;i++) lock_page(&w->page[i]);
        for(size_t i=first;i<=last;i++) {
            struct Page *p=&w->page[i];
            if(action==0) p->leases++;
            else if(action==1) {if(!p->leases) abort();p->leases--;}
            atomic_fetch_add_explicit(&p->generation,1,memory_order_release);
        }
        if(action!=1) for(size_t i=first;i<=last;) {
            if(atomic_load_explicit(&w->page[i].state,memory_order_relaxed)!=1) {i++;continue;}
            size_t end=i+1;
            while(end<=last && atomic_load_explicit(&w->page[end].state,memory_order_relaxed)==1) end++;
            if(!protect_pages(w,i,end-i,PROT_READ|PROT_WRITE)) abort();
            for(size_t k=i;k<end;k++) atomic_store_explicit(&w->page[k].writable,1,memory_order_release);
            i=end;
        }
        for(size_t i=first;i<=last;i++) {
            if(action==2) atomic_store_explicit(&w->page[i].state,2,memory_order_release);
            unlock_page(&w->page[i]);
        }
    }
}
void ch_guest_write_watch_begin(ChGuestWriteWatch *w,void *p,size_t n) {visit_write(w,p,n,0);}
void ch_guest_write_watch_end(ChGuestWriteWatch *w,void *p,size_t n) {visit_write(w,p,n,1);}
void ch_guest_write_watch_invalidate(ChGuestWriteWatch *w,void *p,size_t n) {visit_write(w,p,n,2);}
int ch_guest_write_watch_destroy(ChGuestWriteWatch *w) {
    if(!w) return 1;
    for(size_t i=0;i<w->pages;i++) if(w->page[i].leases) return 0;
    for(size_t i=0;i<w->pages;i++) if(atomic_load(&w->page[i].state)==1)
        if(!protect_page(w,i,PROT_READ|PROT_WRITE)) return 0;
    free(w->views);free(w->page);free(w);return 1;
}
void ch_guest_write_watch_stats(const ChGuestWriteWatch *w,ChGuestWriteStats *out) {
    if(!out) return;
    if(!w) {memset(out,0,sizeof *out);return;}
    *out=(ChGuestWriteStats){w->ranges,w->checks,w->unchanged,
        atomic_load_explicit(&w->faults,memory_order_relaxed),atomic_load_explicit(&w->protect_calls,memory_order_relaxed),w->single_pass_checks,w->single_pass_pages};
}
#else
ChGuestWriteWatch *ch_guest_write_watch_create(const ChGuestWriteView *v,size_t n) {(void)v;(void)n;return NULL;}
ChGuestWriteRange *ch_guest_write_range_create(ChGuestWriteWatch *w,const void *p,size_t n) {(void)w;(void)p;(void)n;return NULL;}
void ch_guest_write_range_destroy(ChGuestWriteRange *r) {(void)r;}
int ch_guest_write_range_changed(ChGuestWriteRange *r) {(void)r;return 1;}
void ch_guest_write_range_rebase(ChGuestWriteRange *r) {(void)r;}
int ch_guest_write_watch_fault(ChGuestWriteWatch *w,uintptr_t p,int write_access) {(void)w;(void)p;(void)write_access;return 0;}
int ch_guest_write_fault_is_write(void *context) {(void)context;return 0;}
void ch_guest_write_watch_begin(ChGuestWriteWatch *w,void *p,size_t n) {(void)w;(void)p;(void)n;}
void ch_guest_write_watch_end(ChGuestWriteWatch *w,void *p,size_t n) {(void)w;(void)p;(void)n;}
void ch_guest_write_watch_invalidate(ChGuestWriteWatch *w,void *p,size_t n) {(void)w;(void)p;(void)n;}
int ch_guest_write_watch_destroy(ChGuestWriteWatch *w) {(void)w;return 1;}
void ch_guest_write_watch_stats(const ChGuestWriteWatch *w,ChGuestWriteStats *out) {(void)w;if(out) memset(out,0,sizeof *out);}
#endif

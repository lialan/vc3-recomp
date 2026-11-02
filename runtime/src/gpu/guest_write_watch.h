#ifndef CH_GUEST_WRITE_WATCH_H
#define CH_GUEST_WRITE_WATCH_H
#include <stddef.h>
#include <stdint.h>
typedef struct ChGuestWriteWatch ChGuestWriteWatch;
typedef struct ChGuestWriteRange ChGuestWriteRange;
/* Views must alias the SAME backing bytes at offset zero. Mapping ownership
 * stays with the caller. Only initially read/write (non-executable) host pages
 * are eligible. Install fault and syscall/protection hooks before guest writes.
 * Direct protection changes outside those hooks are unsupported. */
typedef struct {void *base;size_t bytes;} ChGuestWriteView;
typedef struct {uint64_t ranges,checks,unchanged,faults,protect_calls,single_pass_checks,single_pass_pages;} ChGuestWriteStats;
/* Observer-thread snapshot; fault/protection counters also include writers. */
void ch_guest_write_watch_stats(const ChGuestWriteWatch *,ChGuestWriteStats *);
ChGuestWriteWatch *ch_guest_write_watch_create(const ChGuestWriteView *,size_t count);
/* Serialized observer. NULL means use the original exact comparison path. */
ChGuestWriteRange *ch_guest_write_range_create(ChGuestWriteWatch *,const void *,size_t);
void ch_guest_write_range_destroy(ChGuestWriteRange *);
/* Arms before observing. A false result proves no writes since the previous
 * call; true requires exact comparison. Each range has independent stamps.
 * While a kernel write lease is active this always returns true. */
int ch_guest_write_range_changed(ChGuestWriteRange *);
/* For the observer that just wrote this whole range itself (inside a
 * begin/end lease) and whose snapshot equals the bytes written: re-arm and
 * take the current generations as its baseline, so its next check does not
 * see its own write as a change and fall back to an exact comparison. Other
 * ranges over the same pages still see the write. */
void ch_guest_write_range_rebase(ChGuestWriteRange *);
/* SA_SIGINFO handler calls this for a memory protection fault. No allocation,
 * stdio, guest-memory access or non-lock-free atomic operations are performed.
 * Unhandled faults must continue to the existing handler. */
int ch_guest_write_watch_fault(ChGuestWriteWatch *,uintptr_t address,int write_access);
/* Recognize a write data-abort in the host's SA_SIGINFO ucontext. Unknown
 * architectures/contexts return false; execute/read faults remain unhandled. */
int ch_guest_write_fault_is_write(void *context);
/* Pair around the ENTIRE syscall writing guest memory, including error exits.
 * An observer cannot reprotect a leased page. Ordinary CPU stores use faults. */
void ch_guest_write_watch_begin(ChGuestWriteWatch *,void *,size_t);
void ch_guest_write_watch_end(ChGuestWriteWatch *,void *,size_t);
/* Call BEFORE external protection/unmap operations. Affected pages permanently
 * fall back to exact comparison; the external operation then sets permissions. */
void ch_guest_write_watch_invalidate(ChGuestWriteWatch *,void *,size_t);
/* Caller must stop writers/fault callbacks, destroy ranges and join threads
 * first. Returns zero if a kernel write lease is still active; object remains
 * live in that case. Restores only protections installed by this tracker. */
int ch_guest_write_watch_destroy(ChGuestWriteWatch *);
#endif

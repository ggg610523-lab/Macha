/*
 * reef_host.h - the host ABI that the shipped Mach-O libSystem.B.dylib and the
 * rest of the macOS image graph call through the `_reef_host` global.
 *
 * Layout recovered at runtime from the shipped loader (see README): the loader
 * builds a 0x680-byte table, stores its address in libSystem.B's `_reef_host`
 * data slot and stores table[0x258] into libSystem.B's `_reef_tlv_offset`.
 */
#ifndef PLSR_REEF_HOST_H
#define PLSR_REEF_HOST_H

#include <stddef.h>
#include <stdint.h>

#define REEF_HOST_SIZE 0x680

/* Offsets used by the shipped images (runtime-observed). */
enum reef_slot {
    REEF_EXIT            = 0x028,
    REEF_PUTS            = 0x030,
    REEF_GETPID          = 0x038,
    /* 0x40: arc4random_buf / getentropy (buf, len) — not writev. */
    REEF_RANDOM_BUF      = 0x040,
    REEF_MALLOC          = 0x048,
    REEF_CALLOC          = 0x050,
    REEF_REALLOC         = 0x058,
    REEF_FREE            = 0x060,
    REEF_MALLOC_SIZE     = 0x068,
    REEF_MEMALIGN        = 0x070,
    REEF_MEMCPY          = 0x078,
    REEF_MEMMOVE         = 0x080,
    REEF_MEMSET          = 0x088,
    REEF_MEMCMP          = 0x090,
    REEF_MEMCHR          = 0x098,
    REEF_STRLEN          = 0x0a0,
    REEF_STRCMP          = 0x0a8,
    REEF_STRNCMP         = 0x0b0,
    REEF_STRCHR          = 0x0b8,
    REEF_STRRCHR         = 0x0c0,
    REEF_STRSTR          = 0x0c8,
    REEF_STRCASECMP      = 0x0d0,
    REEF_STRNCASECMP     = 0x0d8,
    REEF_STRTOD          = 0x0e0,
    REEF_SNPRINTF        = 0x0e8,
    REEF_READ            = 0x0f0,
    REEF_WRITE           = 0x0f8,
    REEF_THREAD_CREATE   = 0x100,
    REEF_CLOSE           = 0x108,
    REEF_LSEEK           = 0x110,
    REEF_ISATTY          = 0x118,
    REEF_STAT            = 0x120,
    REEF_FSTAT           = 0x128,
    REEF_GETTIMEOFDAY    = 0x130,
    REEF_CLOCK           = 0x138,
    REEF_PTHREAD_JOIN    = 0x140,
    REEF_PTHREAD_DETACH  = 0x148,
    REEF_PTHREAD_SELF    = 0x150,
    REEF_GETTID          = 0x158,
    REEF_THREAD_EXIT     = 0x160,
    REEF_PTHREAD_NAME    = 0x178,
    REEF_SCHED_YIELD     = 0x188,
    REEF_GETSPECIFIC     = 0x1a0,
    REEF_SETSPECIFIC     = 0x1a8,
    REEF_MUTEX_INIT      = 0x1b0,
    REEF_MUTEX_DESTROY   = 0x1b8,
    REEF_MUTEX_LOCK      = 0x1c0,
    REEF_MUTEX_TRYLOCK   = 0x1c8,
    REEF_MUTEX_UNLOCK    = 0x1d0,
    REEF_COND_INIT       = 0x1d8,
    REEF_COND_DESTROY    = 0x1e0,
    REEF_COND_WAIT       = 0x1e8,
    REEF_COND_TIMEDWAIT  = 0x1f0,
    REEF_COND_SIGNAL     = 0x1f8,
    REEF_COND_BROADCAST  = 0x200,
    REEF_ATOMIC_CAS      = 0x208,
    REEF_ATOMIC_XCHG     = 0x210,
    REEF_ATOMIC_ADD      = 0x218,
    REEF_ATOMIC_OR       = 0x220,
    REEF_DLOPEN          = 0x228,
    REEF_SYSCALL         = 0x250,
    REEF_TLV_OFFSET      = 0x258,
    REEF_TLS_GET         = 0x260,
    REEF_TLS_SET         = 0x268,
    REEF_MMAP            = 0x270,
    REEF_MUNMAP          = 0x278,
    REEF_MPROTECT        = 0x280,
    REEF_MSYNC           = 0x288,
    REEF_MINCORE         = 0x5b8,
    REEF_OPEN            = 0x2c8,
    REEF_OPENAT          = 0x2d0,
    REEF_GETCWD          = 0x2f8,
    REEF_CHDIR           = 0x300,
    REEF_DUP             = 0x320,
    REEF_DUP2            = 0x328,
    REEF_PIPE            = 0x330,
    REEF_FTRUNCATE       = 0x338,
    REEF_FSYNC           = 0x340,
    REEF_PREAD           = 0x348,
    REEF_PWRITE          = 0x350,
    REEF_OPENDIR         = 0x360,
    REEF_CLOSEDIR        = 0x368,
    REEF_SEEKDIR         = 0x370,
    REEF_TELLDIR         = 0x378,
    REEF_DIRFD           = 0x380,
    REEF_READV           = 0x3e0,
    REEF_WRITEV          = 0x3e8,
    REEF_EPOLL_CREATE    = 0x3f0,
    REEF_EPOLL_CTL       = 0x3f8,
    REEF_EPOLL_WAIT      = 0x400,
    REEF_EVENTFD         = 0x408,
    REEF_TIMERFD_SET     = 0x418,
    REEF_TIMERFD_GET     = 0x420,
    REEF_LISTEN          = 0x460,
    REEF_SHUTDOWN        = 0x490,
    REEF_GETHOSTNAME     = 0x4e0,
    REEF_FORK            = 0x500,
    REEF_SETSID          = 0x510,
    REEF_SETPGID         = 0x518,
    REEF_INOTIFY_INIT    = 0x558,
    REEF_INOTIFY_RM      = 0x568,
};

/* Build the host table (loads the shipped ELF host libraries when present). */
void *reef_host_init(void);

/* Value written into libSystem.B's `_reef_tlv_offset` slot. */
long reef_tlv_offset(void);

/* Describe an address as <image>+<offset>(<segment>) for diagnostics. */
const char *plsr_describe_addr(const void *addr);

/* dyld image enumeration used by libSystem.B's
 * __dyld_register_func_for_add_image: count, and a per-image record whose
 * first word is the Mach-O header and whose word at +0x10 is the slide. */
int plsr_dyld_image_count(void);
void *plsr_dyld_image_info(int index);
/* dyld image lookup for _dladdr (slot 0x230) and the exception call-stack
 * builder (slot 0x238): the Mach-O header of the image containing addr. */
void *plsr_image_header_for(const void *addr);
int plsr_image_dl_info(const void *addr, void *out[4]);

void *plsr_shim_lookup(const char *name);
void *plsr_image_symbol(const char *name);
/* free() with a chunk-header sanity check: shipped code sometimes frees a
 * pointer the host allocator never produced (libobjc's pending-category map),
 * and glibc's chunk walk would fault on it. */
void plsr_free_checked(void *p);
void *plsr_malloc_tracked(size_t n);
void *plsr_calloc_tracked(size_t n, size_t s);
void *plsr_realloc_tracked(void *p, size_t n);

#endif

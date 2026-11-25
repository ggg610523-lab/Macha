/*
 * reef_host.c - implementation of the pulsar `_reef_host` ABI.
 *
 * The shipped macOS images do not call Linux libc directly.  libSystem.B.dylib
 * is an adapter: every libc/POSIX entry point it exports loads a function
 * pointer out of the host table and tail-calls it.  This file builds that
 * table from scratch over the host libc.
 *
 * Slot semantics below were recovered from the decompiled Mach-O call sites
 * (`(*_reef_host + off)(args)`) and from the decompiled shipped loader that
 * fills the same table.  Slots without confirmed semantics get a logging stub
 * so a missing host service is observable instead of a null call.
 */
#define _GNU_SOURCE
#include "reef_host.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <malloc.h>
#include <pthread.h>
#include <sched.h>
#include <pwd.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/random.h>
#include <unistd.h>

static unsigned char g_table[REEF_HOST_SIZE];

/* Per-thread TLV array pointer published by libSystem.B's lookup helper
 * (slot 0x260 stores it, 0x268 reads it back). */
static _Thread_local long g_tls_slot;
static long reef_tls_set(long v) { g_tls_slot = v; return 0; }
static long reef_tls_get(void) { return g_tls_slot; }

/* Returned by the runtime's missing-bridge fallbacks (table slot 0x580). */
static long reef_noop(void) { return 0; }
/* __dyld_objc_notify_register(map_images, init_image, unmap_image).  dyld
 * stores the callbacks and replays them for loaded images; we already map
 * the whole graph in one _mapImages call, so only record them. */
static void *g_objc_map, *g_objc_init, *g_objc_unmap;
static long reef_objc_notify_register(void *map, void *init, void *unmap) {
    g_objc_map = map; g_objc_init = init; g_objc_unmap = unmap;
    return 0;
}

/* objc-selectors section data: the shared cache normally supplies a
 * pre-registered selector table.  A manual mapping has none, so report
 * "absent" (NULL), which _mapImages handles. */
static void *reef_section_data(const char *name) { (void)name; return NULL; }
static long reef_section_data_set(const char *name, void *data, void *cls) {
    (void)name; (void)data; (void)cls;
    return 0;
}


static void slot(int off, void *fn) {
    memcpy(g_table + off, &fn, sizeof fn);
}

static unsigned char *g_stub_page;

static long reef_unimpl_common(unsigned off) {
    if (getenv("PLSR_HOST_TRACE"))
        fprintf(stderr, "reef-host: unimplemented slot %#x from %s\n", off,
                plsr_describe_addr(__builtin_return_address(0)));
    return 0;
}

/* Each unfilled slot gets a trampoline that reports its own offset, so a
 * missing host function names the table entry instead of a bare address. */
static void *reef_unimpl_stub(unsigned off) {
    if (!g_stub_page) {
        g_stub_page = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (g_stub_page == MAP_FAILED) {
            g_stub_page = NULL;
            return (void *)reef_unimpl_common;
        }
    }
    unsigned char *p = g_stub_page + (off / 8) * 17;
    void *fn = (void *)reef_unimpl_common;
    p[0] = 0xbf;                       /* mov $off, %edi */
    memcpy(p + 1, &off, 4);
    p[5] = 0x48; p[6] = 0xb8;          /* movabs $reef_unimpl_common, %rax */
    memcpy(p + 7, &fn, 8);
    p[15] = 0xff; p[16] = 0xe0;        /* jmp *%rax */
    __builtin___clear_cache((char *)p, (char *)p + 17);
    return p;
}

/* ---- process ---- */
static void reef_exit(int code) {
    fflush(NULL);
    _exit(code & 0xff);
}
static int reef_puts(const char *s) {
    if (s) { fputs(s, stderr); fputc('\n', stderr); }
    return 0;
}

/* slot 0x40: fill buf with cryptographically strong random bytes.
 * libSystem's _arc4random / _arc4random_buf / _getentropy all call this.
 * Leaving it as the unimpl stub returned 0 without writing, so UUIDs and
 * Swift runtime seeds stayed zero and the process later SIGSEGV'd in
 * libswiftCore on tagged-pointer garbage. */
static int reef_random_buf(void *buf, size_t n) {
    if (!buf || n == 0) return 0;
#if defined(__GLIBC__) && __GLIBC__ >= 2
    if (getentropy(buf, n <= 256 ? n : 256) == 0) {
        if (n <= 256) return 0;
        /* getentropy caps at 256; fill the rest from /dev/urandom. */
        buf = (unsigned char *)buf + 256;
        n -= 256;
    }
#endif
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t off = 0;
    while (off < n) {
        ssize_t r = read(fd, (unsigned char *)buf + off, n - off);
        if (r <= 0) { close(fd); return -1; }
        off += (size_t)r;
    }
    close(fd);
    return 0;
}

/* ---- allocation ---- */
/* slot 0x70 is posix_memalign(void **memptr, size_t align, size_t size). */
static int reef_posix_memalign(void **out, size_t align, size_t size) {
    if (align < sizeof(void *)) align = sizeof(void *);
    return posix_memalign(out, align, size);
}

/* ---- formatting ---- */
/* slot 0xe8 is vsnprintf(buf, size, fmt, va_list). */
static int reef_vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) {
    return vsnprintf(buf, n, fmt, ap);
}

/* ---- dynamic symbol lookup ---- */
/* slot 0x250 is (handle or library install name, symbol) -> address. */
/* True when the argument looks like a library install name rather than a
 * dlopen handle.  CoreGraphics' _loadHost passes "@root/host/...so". */
static int lib_name_arg(const void *p) {
    const unsigned char *s = p;
    if (s[0] < 0x20 || s[0] > 0x7e) return 0;
    if (s[1] && (s[1] < 0x20 || s[1] > 0x7e)) return 0;
    size_t n = strnlen((const char *)p, 512);
    return n > 0 && n < 512;
}

/* The host library needs the vendored toolkit, whose sonames are not on the
 * system search path.  Preload them by absolute path so DT_NEEDED resolution
 * finds the already-loaded objects. */
static void preload_compat(void) {
    static int done;
    if (done) return;
    done = 1;
    const char *root = getenv("PLSR_ROOT");
    if (!root || !*root) return;
    static const char *libs[] = {
        "libicudata.so.76.1", "libicuuc.so.76.1", "libicui18n.so.76.1",
        "libjpeg.so.62.3.0", "libpulsar-toolkit-base.so.1.0",
        "libpulsar-toolkit.so.1.0", NULL
    };
    for (int i = 0; libs[i]; i++) {
        char path[4096];
        snprintf(path, sizeof path, "%s/compat/%s", root, libs[i]);
        dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    }
}

/* slot 0x250 is (handle or library install name, symbol) -> address. */
static void *reef_try_lib(const char *path, const char *sym) {
    void *d = dlopen(path, RTLD_LAZY | RTLD_GLOBAL);
    if (!d) return NULL;
    void *p = dlsym(d, sym);
    char b[320];
    if (!p && sym[0] != '_') {
        snprintf(b, sizeof b, "_%s", sym);
        p = dlsym(d, b);
    }
    if (!p) {                                   /* Linux ICU suffixes its ABI */
        snprintf(b, sizeof b, "%s_76", sym[0] == '_' ? sym + 1 : sym);
        p = dlsym(d, b);
    }
    return p;
}

/* slot 0x250: (handle or library install name, symbol) -> address.
 * libicucore and friends pass a colon-separated list of sonames and expect
 * the host to supply the implementation when the library itself has none. */
static void *reef_host_symbol(void *handle, const char *sym) {
    long h = (long)handle;
    if (sym) {
        void *p = plsr_shim_lookup(sym);
        if (p) return p;
    }
    if (h == 0 || h == -1 || h == -2 || (h > 0 && h < 0x10000))
        return dlsym(handle, sym);                 /* RTLD_DEFAULT/NEXT */
    if (!lib_name_arg(handle))
        return dlsym(handle, sym);                 /* dlopen handle */

    const char *root = getenv("PLSR_ROOT");
    char list[1024];
    snprintf(list, sizeof list, "%s", (const char *)handle);
    preload_compat();
    for (char *save = NULL, *lib = strtok_r(list, ":", &save); lib;
         lib = strtok_r(NULL, ":", &save)) {
        char path[4096];
        void *p = NULL;
        if (lib[0] == '/') p = reef_try_lib(lib, sym);
        if (!p && strncmp(lib, "@root/", 6) == 0 && root && *root) {
            snprintf(path, sizeof path, "%s/%s", root, lib + 6);
            p = reef_try_lib(path, sym);
        }
        if (!p) p = reef_try_lib(lib, sym);
        if (!p && root && *root) {
            static const char *dirs[] = { "compat/", "usr/lib/", "usr/lib/pulsar-appexec/", "" };
            for (int i = 0; i < 4 && !p; i++) {
                snprintf(path, sizeof path, "%s/%s%s", root, dirs[i], lib);
                p = reef_try_lib(path, sym);
            }
        }
        if (p) return p;
    }
    if (getenv("PLSR_HOST_TRACE"))
        fprintf(stderr, "reef-host: 0x250 %s: %s\n", (const char *)handle, sym);
    return NULL;
}

static void *reef_dlopen(const char *path, int flags) {
    return dlopen(path, flags ? flags : (RTLD_NOW | RTLD_GLOBAL));
}

/* ---- condition objects (opaque handle, like the mutex slots) ---- */
static void *reef_cond_create(void) {
    pthread_cond_t *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    if (pthread_cond_init(c, NULL) != 0) { free(c); return NULL; }
    return c;
}
static int reef_cond_destroy(void *c) {
    if (!c) return 0;
    pthread_cond_destroy((pthread_cond_t *)c);
    free(c);
    return 0;
}
static int reef_cond_wait(void *c, void *m) {
    return (c && m) ? pthread_cond_wait((pthread_cond_t *)c, (pthread_mutex_t *)m) : 0;
}
static int reef_cond_timedwait(void *c, void *m, const struct timespec *ts) {
    return (c && m) ? pthread_cond_timedwait((pthread_cond_t *)c,
                                            (pthread_mutex_t *)m, ts) : 0;
}
static int reef_cond_signal(void *c) {
    return c ? pthread_cond_signal((pthread_cond_t *)c) : 0;
}
static int reef_cond_broadcast(void *c) {
    return c ? pthread_cond_broadcast((pthread_cond_t *)c) : 0;
}

/*
 * Thread state.  The shipped adapters treat slots 0x1b0..0x1d0 as an opaque
 * mutex *object*: `m = create(NULL)` then destroy/lock/unlock(m).  They also
 * use 0x190/0x1a0/0x1a8 as pthread key create / get / set.
 */
static void *reef_mutex_create(long recursive) {
    pthread_mutex_t *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    if (recursive) {
        pthread_mutexattr_t a;
        pthread_mutexattr_init(&a);
        pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(m, &a);
        pthread_mutexattr_destroy(&a);
    } else {
        pthread_mutex_init(m, NULL);
    }
    return m;
}
static int reef_mutex_destroy(void *m) {
    if (!m) return 0;
    pthread_mutex_destroy((pthread_mutex_t *)m);
    free(m);
    return 0;
}
static int reef_mutex_lock(void *m) {
    return m ? pthread_mutex_lock((pthread_mutex_t *)m) : 0;
}
static int reef_mutex_unlock(void *m) {
    return m ? pthread_mutex_unlock((pthread_mutex_t *)m) : 0;
}

/* ---- libobjc / libc host slots ---- */
static long reef_thread_self(void) { return (long)pthread_self(); }

/* slot 0x218: host symbol lookup used by _initializeReexports/os_log. */
static void *reef_host_dlsym(void *handle, const char *name) {
    void *p = name ? dlsym(handle ? handle : RTLD_DEFAULT, name) : NULL;
    if (!p && name) p = plsr_image_symbol(name);
    if (!p && name) p = plsr_shim_lookup(name);
    if (getenv("PLSR_HOST_TRACE"))
        fprintf(stderr, "reef-host: dlsym(%s) = %p\n", name ? name : "(null)", p);
    return p;
}

/* Some CoreFoundation paths pass a CF object where a C path is expected.
 * Faulting inside libc would kill the launch, so validate the pointer at the
 * host boundary and report ENOENT instead. */
static int plsr_ptr_readable(const void *p) {
    if (!p) return 0;
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 1;
    char line[512], perms[8];
    unsigned long lo, hi;
    uintptr_t a = (uintptr_t)p;
    int ok = 0;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perms) == 3 &&
            a >= lo && a < hi) {
            if (perms[0] == 'r') {
                size_t max = (size_t)(hi - a);
                if (max > PATH_MAX) max = PATH_MAX;
                ok = memchr(p, 0, max) != NULL;   /* must be NUL-terminated */
            }
            break;
        }
    }
    fclose(f);
    return ok;
}

/* ---- allocator provenance ----
 * The host table exposes glibc malloc/free to the shipped images.  Some of
 * that code frees a pointer this allocator never produced: libobjc removes an
 * entry from _pendingCategories and frees the returned value, which is not a
 * heap block here, so glibc's chunk walk reads a garbage size (0xff...781)
 * and faults.  Track every block we hand out and leak anything else;
 * PLSR_FREE_TRACE reports the skipped pointers. */
#define ALLOC_SLOTS (1u << 17)
static void *g_allocs[ALLOC_SLOTS];

static void alloc_track(void *p) {
    if (!p) return;
    size_t i = ((uintptr_t)p >> 4) & (ALLOC_SLOTS - 1);
    for (size_t n = 0; n < ALLOC_SLOTS; n++) {
        if (!g_allocs[i] || g_allocs[i] == p) { g_allocs[i] = p; return; }
        i = (i + 1) & (ALLOC_SLOTS - 1);
    }
}
static int alloc_known(const void *p) {
    size_t i = ((uintptr_t)p >> 4) & (ALLOC_SLOTS - 1);
    for (size_t n = 0; n < ALLOC_SLOTS; n++) {
        if (g_allocs[i] == p) return 1;
        if (!g_allocs[i]) return 0;
        i = (i + 1) & (ALLOC_SLOTS - 1);
    }
    return 0;
}
/* ---- guard-page allocator (PLSR_EFENCE=1) ----
 * Diagnostic mode for heap-corruption hunts: every small allocation ends
 * exactly at a PROT_NONE page, so the first byte written past the block faults
 * at the store that does it.  Large blocks stay on the normal allocator.
 * A guard page address is recorded in a hash set, which is how free() tells an
 * efence block from a glibc one. */
#define EFENCE_SLOTS (1u << 21)
static uintptr_t g_efence_guards[EFENCE_SLOTS];
static size_t g_efence_pg;
static pthread_mutex_t g_efence_lock = PTHREAD_MUTEX_INITIALIZER;

static int efence_on(void) { return getenv("PLSR_EFENCE") != NULL; }
static size_t efence_page(void) {
    if (!g_efence_pg) {
        long pg = sysconf(_SC_PAGESIZE);
        g_efence_pg = pg > 0 ? (size_t)pg : 4096;
    }
    return g_efence_pg;
}
static void efence_record(uintptr_t guard, int add) {
    size_t i = (guard >> 12) & (EFENCE_SLOTS - 1);
    pthread_mutex_lock(&g_efence_lock);
    for (size_t n = 0; n < EFENCE_SLOTS; n++) {
        if (g_efence_guards[i] == guard) {
            if (!add) g_efence_guards[i] = 1;   /* tombstone */
            break;
        }
        if (g_efence_guards[i] == 0) {
            if (add) g_efence_guards[i] = guard;
            break;
        }
        if (g_efence_guards[i] == 1 && add) {
            g_efence_guards[i] = guard;
            break;
        }
        i = (i + 1) & (EFENCE_SLOTS - 1);
    }
    pthread_mutex_unlock(&g_efence_lock);
}
/* Returns 1 and sets *base when p is an efence block. */
static int efence_owner(void *p, uintptr_t *base) {
    size_t pg = efence_page();
    uintptr_t guard = ((uintptr_t)p + pg) & ~(uintptr_t)(pg - 1);
    size_t i = (guard >> 12) & (EFENCE_SLOTS - 1);
    for (size_t n = 0; n < EFENCE_SLOTS; n++) {
        if (g_efence_guards[i] == guard) { *base = guard - pg; return 1; }
        if (g_efence_guards[i] == 0) return 0;
        i = (i + 1) & (EFENCE_SLOTS - 1);
    }
    return 0;
}
static void *efence_alloc(size_t n) {
    size_t pg = efence_page();
    if (n == 0) n = 1;
    n = (n + 15) & ~(size_t)15;
    if (n > pg - 16) return NULL;
    unsigned char *base = mmap(NULL, pg * 2, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) return NULL;
    if (mprotect(base + pg, pg, PROT_NONE) != 0) {
        munmap(base, pg * 2);
        return NULL;
    }
    efence_record((uintptr_t)(base + pg), 1);
    return base + pg - n;
}
static void efence_free(void *p) {
    uintptr_t base;
    if (!efence_owner(p, &base)) return;
    size_t pg = efence_page();
    efence_record(base + pg, 0);
    munmap((void *)base, pg * 2);
}
/* Used by the signal handler when glibc's calloc faults on the corrupted
 * shipped heap.  Allocate through the guarded allocator so the returned block
 * is valid and plsr_free_checked can recognize it. */
void *plsr_calloc_recover(size_t n, size_t s) {
    if (!n || !s || n > ((size_t)1 << 20) || s > ((size_t)1 << 20) ||
        n > (size_t)-1 / s)
        return NULL;
    void *p = efence_alloc(n * s);
    if (p) memset(p, 0, n * s);
    return p;
}

void *plsr_malloc_tracked(size_t n) {
    if (efence_on()) {
        void *p = efence_alloc(n);
        if (p) return p;
    }
    void *p = malloc(n);
    alloc_track(p);
    return p;
}
void *plsr_calloc_tracked(size_t n, size_t s) {
    /* Shipped code occasionally reaches the allocator through a callback whose
     * ABI does not match calloc and passes an object/tagged pointer as the
     * element count.  Refuse such sizes instead of faulting inside glibc. */
    if (n > ((size_t)1 << 32) || s > ((size_t)1 << 32)) {
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef-host: rejected calloc(%#zx, %#zx)\n", n, s);
        return NULL;
    }
    if (efence_on() && s && n <= (size_t)-1 / s) {
        void *p = efence_alloc(n * s);
        if (p) { memset(p, 0, n * s); return p; }
    }
    void *p = calloc(n, s);
    alloc_track(p);
    return p;
}
void *plsr_realloc_tracked(void *p, size_t n) {
    if (p && efence_on()) {
        uintptr_t base;
        if (efence_owner(p, &base)) {
            void *q = efence_alloc(n);
            if (q) {
                size_t pg = efence_page();
                size_t old = (size_t)(base + pg - (uintptr_t)p);
                memcpy(q, p, old < n ? old : n);
                efence_free(p);
            }
            return q;
        }
    }
    if (p && !alloc_known(p)) {
        /* glibc's realloc frees the old chunk, which faults on a pointer this
         * allocator never produced.  Give the caller a fresh block instead. */
        if (getenv("PLSR_FREE_TRACE"))
            fprintf(stderr, "reef-host: untracked realloc(%p, %zu)\n", p, n);
        void *q = malloc(n);
        if (q) memcpy(q, p, n);
        alloc_track(q);
        return q;
    }
    void *q = realloc(p, n);
    if (q) alloc_track(q);
    return q;
}
void plsr_free_checked(void *p) {
    if (!p) return;
    if (efence_on()) {
        uintptr_t base;
        if (efence_owner(p, &base)) { efence_free(p); return; }
    }
    if (!alloc_known(p)) {
        if (getenv("PLSR_FREE_TRACE"))
            fprintf(stderr, "reef-host: skipped free(%p) (not a host allocation)\n", p);
        return;
    }
    free(p);
}

/* slot 0x100: Darwin open(path, flags, mode) -> Linux open(). */
static int reef_open_darwin(const char *path, int flags, int mode) {
    if (!plsr_ptr_readable(path)) return -1;
    int f = flags & 3;
    if (flags & 0x00000008) f |= O_APPEND;
    if (flags & 0x00000200) f |= O_CREAT;
    if (flags & 0x00000400) f |= O_TRUNC;
    if (flags & 0x00000800) f |= O_EXCL;
    if (flags & 0x00000004) f |= O_NONBLOCK;
    if (flags & 0x01000000) f |= O_CLOEXEC;
    int fd = open(path, f, mode);
    if (getenv("PLSR_HOST_TRACE"))
        fprintf(stderr, "reef-host: open('%s', 0x%x, 0%o) = %d\n",
                path ? path : "(null)", flags, mode, fd);
    return fd;
}

/* slot 0x298: Darwin struct stat (144 bytes) from Linux struct stat.
 *
 * The Mach-O adapters call this with the statAt argument convention
 * (dirfd, path, flags, buf): _stat$INODE64 forwards AT_FDCWD (-2), which a
 * two-argument shim would read as the path and fail with EPERM.  CoreFoundation
 * then sees mode 0 for the .app directory, refuses to create the main bundle,
 * and AppKit never loads the storyboard or the app delegate. */
static int reef_stat_darwin(long dirfd, const char *path, int flags, void *buf) {
    struct stat st;
    int legacy = 0;
    if (!buf) return -1;
    /* Legacy stat(path, buf) callers put the path in the dirfd slot.  Do not
     * test the sign of dirfd: the statAt convention passes AT_FDCWD, which
     * arrives as 0xfffffffe or -2 and is therefore *not* a mapped pointer.
     * A real path pointer is readable, so that is the discriminator. */
    if (dirfd > 0x10000 &&
        plsr_ptr_readable((const void *)(uintptr_t)dirfd)) {
        void *b = (void *)path;
        path = (const char *)dirfd;
        dirfd = -2;
        flags = 0;
        buf = b;
        legacy = 1;
    }
    if (!plsr_ptr_readable(path)) return -1;
    int lflags = (flags & 0x20) ? AT_SYMLINK_NOFOLLOW : 0;
    if (fstatat(dirfd == -2 ? AT_FDCWD : (int)dirfd, path, &st, lflags) != 0) {
        if (getenv("PLSR_HOST_TRACE"))
            fprintf(stderr, "reef-host: stat('%s') = -1\n", path ? path : "(null)");
        return -1;
    }
    if (getenv("PLSR_HOST_TRACE"))
        fprintf(stderr, "reef-host: stat('%s') mode=0%o size=%lld\n",
                path ? path : "(null)", (unsigned)st.st_mode, (long long)st.st_size);
    /* _statAt uses Darwin's 144-byte struct stat, but the legacy adapter
     * (_reef_legacy_stat) has only the 120-byte 32-bit-inode layout.  Writing
     * 144 bytes there runs past the end of its local struct and corrupts the
     * stack, which later surfaces as a bogus free() on the CFBundle path. */
    memset(buf, 0, legacy ? 120 : 144);
    *(uint32_t *)((char *)buf + 0) = (uint32_t)st.st_dev;
    *(uint16_t *)((char *)buf + 4) = (uint16_t)st.st_mode;
    *(uint16_t *)((char *)buf + 6) = (uint16_t)st.st_nlink;
    *(uint64_t *)((char *)buf + 8) = (uint64_t)st.st_ino;
    *(uint32_t *)((char *)buf + 16) = (uint32_t)st.st_uid;
    *(uint32_t *)((char *)buf + 20) = (uint32_t)st.st_gid;
    *(uint32_t *)((char *)buf + 24) = (uint32_t)st.st_rdev;
    *(int64_t *)((char *)buf + 32) = st.st_atim.tv_sec;
    *(int64_t *)((char *)buf + 40) = st.st_atim.tv_nsec;
    *(int64_t *)((char *)buf + 48) = st.st_mtim.tv_sec;
    *(int64_t *)((char *)buf + 56) = st.st_mtim.tv_nsec;
    *(int64_t *)((char *)buf + 64) = st.st_ctim.tv_sec;
    *(int64_t *)((char *)buf + 72) = st.st_ctim.tv_nsec;
    *(int64_t *)((char *)buf + 96) = st.st_size;
    *(int64_t *)((char *)buf + 104) = st.st_blocks;
    *(uint32_t *)((char *)buf + 112) = (uint32_t)st.st_blksize;
    return 0;
}

/* slots 0x230/0x238: dyld image lookup.  0x230(addr, Dl_info*) returns 1 and
 * fills {fname, fbase, sname, saddr}; 0x238(addr) returns the Mach-O header of
 * the containing image.  libSystem's _dladdr and its exception call-stack
 * builder both use them, so leaving them as failing stubs broke every
 * source-mapped backtrace. */
static int reef_dl_info_slot(const void *addr, void *out) {
    return plsr_image_dl_info(addr, (void **)out);
}
static void *reef_image_header_slot(const void *addr) {
    return plsr_image_header_for(addr);
}
/* slot 0x5b0: pthread stack bounds for _pthread_get_stackaddr_np. */
static int reef_thread_stack(void *th, void **addrp, size_t *sizep) {
    pthread_attr_t attr;
    if (pthread_getattr_np((pthread_t)th, &attr) != 0) return -1;
    void *sa = NULL;
    size_t sz = 0;
    int r = pthread_attr_getstack(&attr, &sa, &sz);
    pthread_attr_destroy(&attr);
    if (r != 0) return -1;
    if (addrp) *addrp = sa;
    if (sizep) *sizep = sz;
    return 0;
}

/* slot 0x2a8: faccessat(dirfd, path, mode, flags).  libSystem's _access
 * forwards AT_FDCWD (-2) and the caller's mode.  CFBundle asks whether a
 * resource path is accessible while resolving Main.storyboardc; with the slot
 * unimplemented the generic stub failed every check, so NSBundle reported
 * "Storyboard (Main) doesn't exist in bundle" for a storyboard its own
 * readdir had just listed. */
static int reef_access_darwin(long dirfd, const char *path, int mode, int flags) {
    if (!plsr_ptr_readable(path)) return -1;
    int fd = (dirfd == -2 || dirfd == (long)0xfffffffe) ? AT_FDCWD : (int)dirfd;
    if (faccessat(fd, path, mode, flags ? (int)flags : 0) != 0) return -errno;
    if (getenv("PLSR_HOST_TRACE"))
        fprintf(stderr, "reef-host: access('%s', %d) = 0\n", path, mode);
    return 0;
}

/* slot 0x120: clock_getres(clockid, struct timespec *). */
static int reef_darwin_clock(int id);
/* slot 0x120: clock_gettime(clockid, &sec, &nsec) with Darwin clock ids.
 * Returns 0 or a negative errno, matching the shim's convention. */
static int reef_clock_gettime_darwin(int id, int64_t *sec, int64_t *nsec) {
    struct timespec ts;
    if (clock_gettime(reef_darwin_clock(id), &ts) != 0) return -errno;
    if (sec) *sec = ts.tv_sec;
    if (nsec) *nsec = ts.tv_nsec;
    return 0;
}

/* ---- threads ---- */
/* slot 0x138: pthread_create(&t, start, arg, stacksize).  Darwin's default
 * stack is 512 KiB; glibc accepts it.  Slot 0x5a8 is the QoS variant. */
static int reef_pthread_create(void **out, void *(*start)(void *), void *arg,
                               size_t stack) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (stack >= 0x4000) pthread_attr_setstacksize(&attr, stack);
    pthread_t t;
    int r = pthread_create(&t, &attr, start, arg);
    pthread_attr_destroy(&attr);
    if (r == 0 && out) *out = (void *)t;
    return r;
}
static int reef_pthread_detach(void *t) { return pthread_detach((pthread_t)t); }
static long reef_sched_yield(void) { return sched_yield(); }


/* slot 0x4d8: the shim's getpwuid_r/getpwnam_r adapter.
 *   (uid, name, &ids, &ptrs, buf, buflen) -> 0
 * ids is {uid, gid}; ptrs is {name, passwd, gecos, dir, shell}.  The adapter
 * copies them into the Darwin struct passwd layout itself. */
static char *pw_store(char **cur, char *end, const char *s) {
    size_t n = strlen(s ? s : "");
    if (*cur + n + 1 > end) return (char *)"";
    char *out = *cur;
    memcpy(out, s ? s : "", n + 1);
    *cur += n + 1;
    return out;
}
static int reef_getpwuid_r(unsigned uid, const char *name, void *idout,
                           void *ptrs, char *buf, size_t buflen) {
    struct passwd lpw, *pw = NULL;
    char tmp[1024];
    /* Never hand the caller's buffer to glibc.  NSS writes a 0xff sentinel at
     * buf[buflen - 1]; callers here declare more room than they own (a 4096
     * buflen on a 4080-byte block was observed), so that byte lands in the
     * next chunk's size field and corrupts the heap.  Resolve into a private
     * buffer and return pointers into static storage instead. */
    int r = name ? getpwnam_r(name, &lpw, tmp, sizeof tmp, &pw)
                 : getpwuid_r((uid_t)uid, &lpw, tmp, sizeof tmp, &pw);
    if (getenv("PLSR_HOST_TRACE"))
        fprintf(stderr,
                "reef-host: passwd lookup(uid=%u name=%s buflen=%zu buf=%p) = %d\n",
                uid, name ? name : "-", buflen, (void *)buf, r);
    (void)buf;
    (void)buflen;
    if (r != 0) return r;
    uint32_t *ids = idout;
    char **p = ptrs;
    if (!pw) {
        ids[0] = 0; ids[1] = 0;
        p[0] = (char *)""; p[1] = (char *)""; p[2] = (char *)"";
        p[3] = (char *)"/"; p[4] = (char *)"/bin/sh";
        return 0;
    }
    ids[0] = (uint32_t)pw->pw_uid;
    ids[1] = (uint32_t)pw->pw_gid;
    static char store[4096];
    static pthread_mutex_t store_lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&store_lock);
    char *cur = store, *end = store + sizeof store;
    p[0] = pw_store(&cur, end, pw->pw_name ? pw->pw_name : "");
    p[1] = pw_store(&cur, end, pw->pw_passwd ? pw->pw_passwd : "");
    p[2] = pw_store(&cur, end, pw->pw_gecos ? pw->pw_gecos : "");
    p[3] = pw_store(&cur, end, pw->pw_dir ? pw->pw_dir : "/");
    p[4] = pw_store(&cur, end, pw->pw_shell ? pw->pw_shell : "/bin/sh");
    pthread_mutex_unlock(&store_lock);
    return 0;
}


/* slot 0x3d8: poll(fds, nfds, timeout_ms); the shim converts ppoll's
 * timespec to milliseconds before calling.  Darwin's struct pollfd layout
 * matches Linux's. */
static int reef_poll(void *fds, unsigned long nfds, int timeout) {
    return poll((struct pollfd *)fds, (nfds_t)nfds, timeout);
}
/* slot 0x158: __thread_selfid(), the magic syscall(0x7e000002) path.
 *
 * libdispatch records this value in its per-thread TSD (`dt_tid`) and in the
 * main queue's gate, then __dispatch_gate_broadcast_slow traps with ud2 when
 * the two differ.  In this runtime the TSD block and the gate can be touched
 * by different threads during AppKit startup, so report one stable id for the
 * whole process; PLSR_TID_GETTID=1 restores the real per-thread id. */
static uint64_t reef_thread_selfid(void) {
    if (getenv("PLSR_TID_GETTID")) return (uint64_t)syscall(SYS_gettid);
    return 0x1234;
}


/* slots 0x3f0..0x418: the shim exposes the Linux event loop primitives
 * directly (epoll/eventfd/timerfd); the ABIs are identical. */
static int reef_epoll_create1(int flags) { return epoll_create1(flags); }
static int reef_epoll_ctl(int epfd, int op, int fd, void *ev) {
    int r = epoll_ctl(epfd, op, fd, (struct epoll_event *)ev);
    if (getenv("PLSR_EPOLL_TRACE"))
        fprintf(stderr, "epoll_ctl(epfd=%d op=%d fd=%d ev=%p) = %d errno=%d\n",
                epfd, op, fd, ev, r, r < 0 ? errno : 0);
    return r;
}
static int reef_epoll_wait(int epfd, void *ev, int max, int timeout) {
    int r = epoll_wait(epfd, (struct epoll_event *)ev, max, timeout);
    if (getenv("PLSR_EPOLL_TRACE"))
        fprintf(stderr, "epoll_wait(epfd=%d max=%d timeout=%d) = %d errno=%d\n",
                epfd, max, timeout, r, r < 0 ? errno : 0);
    return r;
}
static int reef_eventfd(unsigned val, int flags) { return eventfd(val, flags); }
static int reef_darwin_clock(int id) {
    switch (id) {
    case 0: return CLOCK_REALTIME;        /* CLOCK_REALTIME */
    case 4: return CLOCK_MONOTONIC_RAW;   /* CLOCK_MONOTONIC_RAW */
    case 6: return CLOCK_MONOTONIC;       /* CLOCK_MONOTONIC */
    case 8: return CLOCK_MONOTONIC;       /* CLOCK_UPTIME_RAW */
    default: return CLOCK_MONOTONIC;
    }
}
static int reef_timerfd_create(int clockid, int flags) {
    int fd = timerfd_create(reef_darwin_clock(clockid), flags);
    if (getenv("PLSR_EPOLL_TRACE"))
        fprintf(stderr, "timerfd_create(darwin=%d flags=0x%x) = %d errno=%d\n",
                clockid, flags, fd, fd < 0 ? errno : 0);
    return fd;
}
static int reef_timerfd_settime(int fd, int flags, const void *nw, void *old) {
    return timerfd_settime(fd, flags, (const struct itimerspec *)nw,
                           (struct itimerspec *)old);
}
static int reef_timerfd_gettime(int fd, void *cur) {
    return timerfd_gettime(fd, (struct itimerspec *)cur);
}


/* slot 0x358: opendir(path, fd, &err) -> DIR*, with the error code returned
 * through the third argument.  slot 0x380: dirfd(DIR*). */
static void *reef_opendir_err(const char *path, int fd, int *err) {
    (void)fd;
    if (!plsr_ptr_readable(path)) {
        if (err) *err = ENOENT;
        return NULL;
    }
    DIR *d = opendir(path);
    if (getenv("PLSR_HOST_TRACE"))
        fprintf(stderr, "reef-host: opendir('%s') = %p errno=%d\n",
                path ? path : "(null)", (void *)d, d ? 0 : errno);
    if (!d && err) *err = errno;
    return d;
}

/* slot 0x360: readdir(dirp, entry) -> 1 entry / 0 end / -errno.  The libSystem
 * adapter fills its own Darwin struct dirent from this buffer: inode at 0,
 * d_type at 0x10, NUL-terminated name at 0x11.  Binding this slot to opendir
 * made every directory read pass a DIR* as a path, so CFBundle could not list
 * Contents/Resources, never found Main.storyboardc, and AppKit never loaded
 * the storyboard that installs the application delegate. */
static int reef_readdir_darwin(DIR *d, unsigned char *ent) {
    if (getenv("PLSR_NO_READDIR")) return 0;
    struct dirent *e;
    errno = 0;
    e = readdir(d);
    if (!e) return errno ? -errno : 0;
    if (getenv("PLSR_HOST_TRACE"))
        fprintf(stderr, "reef-readdir: '%s' type=%u\n", e->d_name,
                (unsigned)e->d_type);
    memset(ent, 0, 0x11);
    *(uint32_t *)(ent + 0) = (uint32_t)e->d_ino;
    ent[0x10] = (unsigned char)e->d_type;
    size_t n = strlen(e->d_name);
    if (n > 0xfe) n = 0xfe;
    memcpy(ent + 0x11, e->d_name, n);
    ent[0x11 + n] = 0;
    return 1;
}
static int reef_dirfd(void *d) { return dirfd((DIR *)d); }

/* slot 0x438: __ulock_wait/__ulock_wake, reached through
 * _syscall(0x7e000001, ...).  libdispatch's gate broadcast calls this with
 * op 0x81 (ULF_WAKE_ALL|UL_COMPARE_AND_WAIT); libSystem traps with ud2 when
 * the call fails with anything other than ENOENT, and the unimplemented stub
 * did exactly that.  Map Darwin's ulock operations onto Linux futexes and
 * return the Darwin convention (0, or a negative errno). */
#ifndef FUTEX_WAIT
#define FUTEX_WAIT 0
#endif
#ifndef FUTEX_WAKE
#define FUTEX_WAKE 1
#endif
static int reef_ulock(void *addr, uint32_t op, uint32_t val, uint64_t a,
                      uint64_t b, uint64_t c) {
    (void)a; (void)b; (void)c;
    if (!addr) return -14;                    /* EFAULT */
    if (op & 0x80) {                          /* ULF_WAKE_ALL */
        long n = syscall(SYS_futex, addr, FUTEX_WAKE, 0x7fffffff, NULL, NULL, 0);
        if (n < 0) return -errno;
        return n > 0 ? 0 : -2;                /* ENOENT: no waiters */
    }
    long r = syscall(SYS_futex, addr, FUTEX_WAIT, (int)val, NULL, NULL, 0);
    if (r == 0) return 0;
    return errno == EAGAIN ? -11 : -errno;    /* EAGAIN */
}


/* slot 0x3c8: sysconf with Darwin's _SC_* numbering. */
static long reef_sysconf(int name) {
    switch (name) {
    case 1: return sysconf(_SC_ARG_MAX);
    case 2: return sysconf(_SC_CHILD_MAX);
    case 3: return sysconf(_SC_CLK_TCK);
    case 4: return sysconf(_SC_NGROUPS_MAX);
    case 5: return sysconf(_SC_OPEN_MAX);
    case 29: return sysconf(_SC_PAGESIZE);
    case 57: return sysconf(_SC_NPROCESSORS_CONF);
    case 58: return sysconf(_SC_NPROCESSORS_ONLN);
    case 200: return sysconf(_SC_PHYS_PAGES);
    default: return -1;
    }
}

/* ---- host libraries ---- */
static void load_host_lib(const char *rel) {
    if (getenv("PLSR_NO_HOST")) return;
    const char *root = getenv("PLSR_ROOT");
    if (!root || !*root) return;
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", root, rel);
    preload_compat();
    void *h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (!h && getenv("PLSR_HOST_TRACE"))
        fprintf(stderr, "reef-host: %s: %s\n", path, dlerror());
}

void *reef_host_init(void) {
    memset(g_table, 0, sizeof g_table);

    /* ---- confirmed from the shipped adapters ---- */
    slot(REEF_EXIT, reef_exit);                    /* 0x028 exit(code) */
    slot(REEF_PUTS, reef_puts);                    /* 0x030 puts(str) */
    slot(REEF_GETPID, (void *)getpid);             /* 0x038 getpid() */
    slot(REEF_RANDOM_BUF, (void *)reef_random_buf); /* 0x040 arc4random_buf */
    slot(REEF_READV, (void *)readv);               /* 0x3e0 readv */
    slot(REEF_WRITEV, (void *)writev);             /* 0x3e8 writev */

    slot(REEF_MALLOC, (void *)plsr_malloc_tracked);   /* 0x048 */
    slot(REEF_CALLOC, (void *)plsr_calloc_tracked);   /* 0x050 */
    slot(REEF_REALLOC, (void *)plsr_realloc_tracked); /* 0x058 */
    slot(REEF_FREE, (void *)plsr_free_checked);    /* 0x060 */
    slot(REEF_MALLOC_SIZE, (void *)malloc_usable_size); /* 0x068 */
    slot(REEF_MEMALIGN, reef_posix_memalign);      /* 0x070 */

    slot(REEF_MEMCPY, (void *)memcpy);             /* 0x078 */
    slot(REEF_MEMMOVE, (void *)memmove);           /* 0x080 */
    slot(REEF_MEMSET, (void *)memset);             /* 0x088 */
    slot(REEF_MEMCMP, (void *)memcmp);             /* 0x090 */
    slot(REEF_MEMCHR, (void *)memchr);             /* 0x098 */
    slot(REEF_STRLEN, (void *)strlen);             /* 0x0a0 */
    slot(REEF_STRCMP, (void *)strcmp);             /* 0x0a8 */
    slot(REEF_STRNCMP, (void *)strncmp);           /* 0x0b0 */
    slot(REEF_STRCHR, (void *)strchr);             /* 0x0b8 */
    slot(REEF_STRRCHR, (void *)strrchr);           /* 0x0c0 */
    slot(REEF_STRSTR, (void *)strstr);             /* 0x0c8 */
    slot(REEF_STRCASECMP, (void *)strcasecmp);     /* 0x0d0 */
    slot(REEF_STRNCASECMP, (void *)strncasecmp);   /* 0x0d8 */
    slot(REEF_STRTOD, (void *)strtod);             /* 0x0e0 */
    slot(REEF_SNPRINTF, reef_vsnprintf);           /* 0x0e8 */

    slot(REEF_READ, (void *)read);                 /* 0x0f0 */
    slot(REEF_WRITE, (void *)write);               /* 0x0f8 */
    slot(REEF_CLOSE, (void *)close);               /* 0x108 */
    slot(REEF_LSEEK, (void *)lseek);               /* 0x110 */
    slot(REEF_ISATTY, (void *)isatty);             /* 0x118 */

    slot(REEF_DLOPEN, reef_dlopen);                /* 0x228 dlopen */
    slot(0x250, reef_host_symbol);                 /* dlsym(name, sym) */

    slot(REEF_GETCWD, (void *)getcwd);             /* 0x2f8 */

    /* thread state (see _threadState/_reef_tlv_lookup in libSystem.B) */
    slot(0x190, (void *)pthread_key_create);      /* key_create(&key, dtor) */
    slot(REEF_GETSPECIFIC, (void *)pthread_getspecific);
    slot(REEF_SETSPECIFIC, (void *)pthread_setspecific);
    slot(REEF_MUTEX_INIT, reef_mutex_create);      /* mutex object create */
    slot(REEF_MUTEX_DESTROY, reef_mutex_destroy);
    slot(REEF_MUTEX_LOCK, reef_mutex_lock);
    slot(REEF_MUTEX_TRYLOCK, (void *)pthread_mutex_trylock);
    slot(REEF_MUTEX_UNLOCK, reef_mutex_unlock);
    slot(REEF_COND_INIT, reef_cond_create);        /* cond object create */
    slot(REEF_COND_DESTROY, reef_cond_destroy);
    slot(REEF_COND_WAIT, reef_cond_wait);
    slot(REEF_COND_TIMEDWAIT, reef_cond_timedwait);
    slot(REEF_COND_SIGNAL, reef_cond_signal);
    slot(REEF_COND_BROADCAST, reef_cond_broadcast);

    /* dyld image enumeration (__dyld_register_func_for_add_image) */
    slot(REEF_ATOMIC_CAS, (void *)plsr_dyld_image_count);   /* 0x208 */
    slot(REEF_ATOMIC_XCHG, (void *)plsr_dyld_image_info);   /* 0x210 */

    /* ---- per-thread TLV array pointer ---- */
    slot(REEF_TLS_GET, (void *)reef_tls_set);       /* 0x260 */
    slot(REEF_TLS_SET, (void *)reef_tls_get);       /* 0x268 */

    slot(0x580, (void *)reef_noop);                /* missing-bridge return */
    /* libobjc runtime hooks */
    slot(0x220, (void *)reef_objc_notify_register); /* objc notify register */
    slot(0x240, (void *)reef_section_data);         /* get section data */
    slot(0x248, (void *)reef_section_data_set);     /* set section data */
    slot(0x150, (void *)reef_thread_self);          /* pthread_self */
    slot(0x218, (void *)reef_host_dlsym);           /* host dlsym(name) */
    slot(0x100, (void *)reef_open_darwin);          /* open */
    slot(0x298, (void *)reef_stat_darwin);          /* stat */
    slot(0x2a8, (void *)reef_access_darwin);        /* access/faccessat */
    slot(0x230, (void *)reef_dl_info_slot);         /* dladdr image info */
    slot(0x238, (void *)reef_image_header_slot);    /* image header for addr */
    slot(0x5b0, (void *)reef_thread_stack);         /* pthread stack bounds */
    slot(0x120, (void *)reef_clock_gettime_darwin); /* clock_gettime */
    slot(0x138, (void *)reef_pthread_create);       /* pthread_create */
    slot(0x5a8, (void *)reef_pthread_create);       /* pthread_create_qos */
    slot(0x148, (void *)reef_pthread_detach);       /* pthread_detach */
    slot(0x188, (void *)reef_sched_yield);          /* sched_yield */
    slot(0x4d8, (void *)reef_getpwuid_r);        /* getpwuid_r/getpwnam_r */
    slot(0x158, (void *)reef_thread_selfid);      /* __thread_selfid */
    slot(0x3d8, (void *)reef_poll);               /* poll */
    slot(0x438, (void *)reef_ulock);              /* __ulock_wait/wake */
    slot(0x3f0, (void *)reef_epoll_create1);      /* epoll_create1 */
    slot(0x3f8, (void *)reef_epoll_ctl);          /* epoll_ctl */
    slot(0x400, (void *)reef_epoll_wait);         /* epoll_wait */
    slot(0x408, (void *)reef_eventfd);            /* eventfd */
    slot(0x410, (void *)reef_timerfd_create);     /* timerfd_create */
    slot(0x418, (void *)reef_timerfd_settime);    /* timerfd_settime */
    slot(0x420, (void *)reef_timerfd_gettime);    /* timerfd_gettime */
    slot(0x358, (void *)reef_opendir_err);        /* opendir(path, fd, &err) */
    slot(0x380, (void *)reef_dirfd);              /* dirfd */
    slot(0x3c8, (void *)reef_sysconf);             /* sysconf */

    /* ---- directory stream, as used by the fts/opendir adapters ---- */
    slot(REEF_OPENDIR, (void *)reef_readdir_darwin); /* 0x360 readdir */
    slot(REEF_CLOSEDIR, (void *)closedir);         /* 0x368 */
    slot(REEF_SEEKDIR, (void *)seekdir);           /* 0x370 */
    slot(REEF_TELLDIR, (void *)telldir);           /* 0x378 */
    slot(REEF_DIRFD, (void *)dirfd);               /* 0x380 */

    /* TLS: the shipped runtime addresses its thread block at %fs:-0x1818. */
    *(long *)(g_table + REEF_TLV_OFFSET) = reef_tlv_offset();

    /* Vendor rendering bridge: AppKit/SwiftUI reach the Wayland toolkit
     * through these ELF host libraries. */
    load_host_lib("host/libpulsar-app-host.so");
    load_host_lib("host/libpulsar-web-host.so");
    load_host_lib("lib/libpulsar-metal-transl.so");
    load_host_lib("lib/libpulsar-metal-msl.so");

    for (int off = 0; off + 8 <= REEF_HOST_SIZE; off += 8) {
        void *cur;
        memcpy(&cur, g_table + off, sizeof cur);
        if (!cur) slot(off, reef_unimpl_stub((unsigned)off));
    }

    return g_table;
}

long reef_tlv_offset(void) {
    /* Observed in the shipped loader: table[0x258] = -0x1818. */
    return -0x1818L;
}

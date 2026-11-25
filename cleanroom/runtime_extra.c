/*
 * runtime_extra.c: second slice of the clean-room libSystem surface.
 *
 * Blocks runtime, BSD/malloc/machine shims, dyld and os_log stubs, and the
 * libc entry points whose Darwin spelling differs. Linked into
 * libplsr-libsystem.so; data symbols keep the double-underscore Mach-O names.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <malloc.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

/* ---- Blocks runtime ---- */

#define BLOCK_NEEDS_FREE (1 << 24)
#define BLOCK_HAS_COPY_DISPOSE (1 << 25)
#define BLOCK_IS_GLOBAL (1 << 28)

struct plsr_block_descriptor {
    unsigned long reserved;
    unsigned long size;
    void (*copy)(void *dst, const void *src);
    void (*dispose)(const void *src);
};

struct plsr_block {
    void *isa;
    int flags;
    int reserved;
    void (*invoke)(void *, ...);
    struct plsr_block_descriptor *descriptor;
};

/* The three block classes exist so metadata references resolve; the runtime
 * here does not message them. */
void *plsr_block_stack_class __asm__("__NSConcreteStackBlock") = NULL;
void *plsr_block_malloc_class __asm__("__NSConcreteMallocBlock") = NULL;
void *plsr_block_global_class __asm__("__NSConcreteGlobalBlock") = NULL;

void *plsr_Block_copy(const void *arg) __asm__("__Block_copy");
void *plsr_Block_copy(const void *arg)
{
    const struct plsr_block *src = arg;
    struct plsr_block *dst;
    size_t size;
    if (!src)
        return NULL;
    if (src->flags & BLOCK_IS_GLOBAL)
        return (void *)arg;
    size = src->descriptor && src->descriptor->size ? src->descriptor->size
                                                    : sizeof(*src);
    dst = calloc(1, size);
    if (!dst)
        return NULL;
    memcpy(dst, src, size);
    dst->flags |= BLOCK_NEEDS_FREE;
    if ((src->flags & BLOCK_HAS_COPY_DISPOSE) && src->descriptor &&
        src->descriptor->copy)
        src->descriptor->copy(dst, src);
    return dst;
}

void plsr_Block_release(const void *arg) __asm__("__Block_release");
void plsr_Block_release(const void *arg)
{
    struct plsr_block *b = (struct plsr_block *)arg;
    if (!b || !(b->flags & BLOCK_NEEDS_FREE))
        return;
    if ((b->flags & BLOCK_HAS_COPY_DISPOSE) && b->descriptor &&
        b->descriptor->dispose)
        b->descriptor->dispose(b);
    free(b);
}

void plsr_Block_object_assign(void *dst, const void *src, int flags)
    __asm__("__Block_object_assign");
void plsr_Block_object_assign(void *dst, const void *src, int flags)
{
    (void)flags;
    if (dst)
        *(const void **)dst = src;
}

void plsr_Block_object_dispose(const void *src, int flags)
    __asm__("__Block_object_dispose");
void plsr_Block_object_dispose(const void *src, int flags)
{
    (void)src;
    (void)flags;
}

/* ---- process arguments and executable path ---- */

extern char **environ;
static int g_argc;
static char **g_argv;
static int g_args_ready;

static void parse_cmdline(void)
{
    int fd;
    char *buf;
    size_t cap = 65536, len = 0;
    if (g_args_ready)
        return;
    g_args_ready = 1;
    fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd < 0)
        return;
    buf = malloc(cap);
    if (!buf) {
        close(fd);
        return;
    }
    for (;;) {
        ssize_t n = read(fd, buf + len, cap - len);
        if (n <= 0)
            break;
        len += (size_t)n;
        if (len == cap) {
            char *p = realloc(buf, cap * 2);
            if (!p)
                break;
            buf = p;
            cap *= 2;
        }
    }
    close(fd);
    g_argv = calloc(len + 1, sizeof(char *));
    if (!g_argv) {
        free(buf);
        return;
    }
    for (size_t i = 0; i < len;) {
        g_argv[g_argc++] = buf + i;
        i += strlen(buf + i) + 1;
    }
}

int *plsr_NSGetArgc(void) __asm__("__NSGetArgc");
int *plsr_NSGetArgc(void)
{
    parse_cmdline();
    return &g_argc;
}

char ***plsr_NSGetArgv(void) __asm__("__NSGetArgv");
char ***plsr_NSGetArgv(void)
{
    parse_cmdline();
    return &g_argv;
}

char ***plsr_NSGetEnviron(void) __asm__("__NSGetEnviron");
char ***plsr_NSGetEnviron(void) { return &environ; }

int plsr_NSGetExecutablePath(char *buf, uint32_t *size)
    __asm__("__NSGetExecutablePath");
int plsr_NSGetExecutablePath(char *buf, uint32_t *size)
{
    char path[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n < 0)
        return -1;
    path[n] = 0;
    if (!buf || !size || *size < (uint32_t)n + 1) {
        if (size)
            *size = (uint32_t)n + 1;
        return -1;
    }
    memcpy(buf, path, (size_t)n + 1);
    *size = (uint32_t)n + 1;
    return 0;
}

/* ---- printf/scanf locale variants ---- */

int plsr_asprintf_l(char **out, locale_t loc, const char *fmt, ...)
    __asm__("_asprintf_l");
int plsr_asprintf_l(char **out, locale_t loc, const char *fmt, ...)
{
    va_list ap;
    int r;
    (void)loc;
    va_start(ap, fmt);
    r = vasprintf(out, fmt, ap);
    va_end(ap);
    return r;
}

int plsr_snprintf_l(char *buf, size_t n, locale_t loc, const char *fmt, ...)
    __asm__("_snprintf_l");
int plsr_snprintf_l(char *buf, size_t n, locale_t loc, const char *fmt, ...)
{
    va_list ap;
    int r;
    (void)loc;
    va_start(ap, fmt);
    r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

int plsr_sscanf_l(const char *s, locale_t loc, const char *fmt, ...)
    __asm__("_sscanf_l");
int plsr_sscanf_l(const char *s, locale_t loc, const char *fmt, ...)
{
    va_list ap;
    int r;
    (void)loc;
    va_start(ap, fmt);
    r = vsscanf(s, fmt, ap);
    va_end(ap);
    return r;
}

/* ---- asserts, availability, stack guard, dyld ---- */

void plsr_assert_rtn(const char *func, const char *file, int line,
                     const char *expr) __asm__("___assert_rtn");
void plsr_assert_rtn(const char *func, const char *file, int line,
                     const char *expr)
{
    fprintf(stderr, "Assertion failed: %s (%s:%d): %s\n", expr,
            file ? file : "?", line, func ? func : "?");
    abort();
}

int plsr_availability_version_check(int count, ...)
    __asm__("__availability_version_check");
int plsr_availability_version_check(int count, ...)
{
    (void)count;
    return 1;
}

uintptr_t plsr_stack_chk_guard __asm__("___stack_chk_guard") =
    0x9e3779b97f4a7c15ULL;

void plsr_dyld_stub_binder(void) __asm__("dyld_stub_binder");
void plsr_dyld_stub_binder(void) { abort(); }

const char *plsr_dyld_get_image_name(uint32_t i) __asm__("__dyld_get_image_name");
const char *plsr_dyld_get_image_name(uint32_t i) { (void)i; return NULL; }
uint32_t plsr_dyld_image_count(void) __asm__("__dyld_image_count");
uint32_t plsr_dyld_image_count(void) { return 0; }
uint32_t plsr_dyld_get_program_sdk_version(void)
    __asm__("_dyld_get_program_sdk_version");
uint32_t plsr_dyld_get_program_sdk_version(void) { return 0; }
void plsr_dyld_register_func_for_add_image(void (*fn)(const void *, intptr_t))
    __asm__("__dyld_register_func_for_add_image");
void plsr_dyld_register_func_for_add_image(void (*fn)(const void *, intptr_t))
{
    (void)fn;
}

void *plsr_tlv_bootstrap(void) __asm__("__tlv_bootstrap");
void *plsr_tlv_bootstrap(void) { return NULL; }

/* ---- malloc zone surface ---- */

static int g_malloc_zone;
void *plsr_malloc_default_zone(void) __asm__("_malloc_default_zone");
void *plsr_malloc_default_zone(void) { return &g_malloc_zone; }
void *plsr_malloc_zone_from_ptr(const void *p) __asm__("_malloc_zone_from_ptr");
void *plsr_malloc_zone_from_ptr(const void *p) { (void)p; return NULL; }
size_t plsr_malloc_size(const void *p) __asm__("_malloc_size");
size_t plsr_malloc_size(const void *p) { return p ? malloc_usable_size((void *)p) : 0; }
size_t plsr_malloc_good_size(size_t n) __asm__("_malloc_good_size");
size_t plsr_malloc_good_size(size_t n) { return n; }
size_t plsr_malloc_zone_pressure_relief(void *zone, size_t goal)
    __asm__("_malloc_zone_pressure_relief");
size_t plsr_malloc_zone_pressure_relief(void *zone, size_t goal)
{
    (void)zone;
    (void)goal;
    return 0;
}
void plsr_malloc_zone_statistics(void *zone, void *stats)
    __asm__("_malloc_zone_statistics");
void plsr_malloc_zone_statistics(void *zone, void *stats)
{
    (void)zone;
    (void)stats;
}

void plsr_memset_pattern16(void *dst, const void *pattern, size_t len)
    __asm__("_memset_pattern16");
void plsr_memset_pattern16(void *dst, const void *pattern, size_t len)
{
    const unsigned char *p = pattern;
    unsigned char *d = dst;
    for (size_t i = 0; i < len; i++)
        d[i] = p[i % 16];
}

int plsr_memset_s(void *dst, size_t smax, int c, size_t n) __asm__("_memset_s");
int plsr_memset_s(void *dst, size_t smax, int c, size_t n)
{
    if (n > smax)
        return EINVAL;
    memset(dst, c, n);
    return 0;
}

/* ---- copyfile ---- */

struct plsr_copyfile_state { int flags; };

void *plsr_copyfile_state_alloc(void) __asm__("_copyfile_state_alloc");
void *plsr_copyfile_state_alloc(void)
{
    return calloc(1, sizeof(struct plsr_copyfile_state));
}
int plsr_copyfile_state_free(void *state) __asm__("_copyfile_state_free");
int plsr_copyfile_state_free(void *state)
{
    free(state);
    return 0;
}

static int copy_fd(int in, int out)
{
    char buf[65536];
    for (;;) {
        ssize_t n = read(in, buf, sizeof(buf));
        if (n == 0)
            return 0;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(out, buf + off, (size_t)(n - off));
            if (w < 0) {
                if (errno == EINTR)
                    continue;
                return -1;
            }
            off += w;
        }
    }
}

int plsr_fcopyfile(int in, int out, void *state, int flags)
    __asm__("_fcopyfile");
int plsr_fcopyfile(int in, int out, void *state, int flags)
{
    (void)state;
    (void)flags;
    return copy_fd(in, out);
}

int plsr_copyfile(const char *from, const char *to, void *state, int flags)
    __asm__("_copyfile");
int plsr_copyfile(const char *from, const char *to, void *state, int flags)
{
    int in, out, r;
    (void)state;
    (void)flags;
    in = open(from, O_RDONLY);
    if (in < 0)
        return -1;
    out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        close(in);
        return -1;
    }
    r = copy_fd(in, out);
    close(in);
    close(out);
    return r;
}

int plsr_exchangedata(const char *a, const char *b, unsigned int opts)
    __asm__("_exchangedata");
int plsr_exchangedata(const char *a, const char *b, unsigned int opts)
{
    (void)a;
    (void)b;
    (void)opts;
    errno = ENOTSUP;
    return -1;
}

int plsr_renamex_np(const char *a, const char *b, unsigned int flags)
    __asm__("_renamex_np");
int plsr_renamex_np(const char *a, const char *b, unsigned int flags)
{
    (void)flags;
    return rename(a, b);
}

/* ---- directory and stdio variants ---- */

FILE *plsr_fopen_darwin(const char *p, const char *m)
    __asm__("_fopen$DARWIN_EXTSN");
FILE *plsr_fopen_darwin(const char *p, const char *m) { return fopen(p, m); }

char *plsr_realpath_darwin(const char *p, char *r)
    __asm__("_realpath$DARWIN_EXTSN");
char *plsr_realpath_darwin(const char *p, char *r) { return realpath(p, r); }

DIR *plsr_opendir_inode64(const char *p) __asm__("_opendir$INODE64");
DIR *plsr_opendir_inode64(const char *p) { return opendir(p); }
struct dirent *plsr_readdir_inode64(DIR *d) __asm__("_readdir$INODE64");
struct dirent *plsr_readdir_inode64(DIR *d) { return readdir(d); }
DIR *plsr_fdopendir_inode64(int fd) __asm__("_fdopendir$INODE64");
DIR *plsr_fdopendir_inode64(int fd) { return fdopendir(fd); }

int plsr_statfs_inode64(const char *p, void *buf) __asm__("_statfs$INODE64");
int plsr_statfs_inode64(const char *p, void *buf)
{
    (void)p;
    (void)buf;
    errno = ENOTSUP;
    return -1;
}
int plsr_getmntinfo_inode64(void **mnt, int flags) __asm__("_getmntinfo$INODE64");
int plsr_getmntinfo_inode64(void **mnt, int flags)
{
    (void)mnt;
    (void)flags;
    errno = ENOTSUP;
    return -1;
}

/* ---- locale/wide-character variants ---- */

struct lconv *plsr_localeconv_l(locale_t loc) __asm__("_localeconv_l");
struct lconv *plsr_localeconv_l(locale_t loc) { (void)loc; return localeconv(); }
wint_t plsr_btowc_l(int c, locale_t loc) __asm__("_btowc_l");
wint_t plsr_btowc_l(int c, locale_t loc) { (void)loc; return btowc(c); }
int plsr_mbtowc_l(wchar_t *pwc, const char *s, size_t n, locale_t loc)
    __asm__("_mbtowc_l");
int plsr_mbtowc_l(wchar_t *pwc, const char *s, size_t n, locale_t loc)
{
    (void)loc;
    return mbtowc(pwc, s, n);
}
size_t plsr_mbrtowc_l(wchar_t *pwc, const char *s, size_t n, mbstate_t *ps,
                      locale_t loc) __asm__("_mbrtowc_l");
size_t plsr_mbrtowc_l(wchar_t *pwc, const char *s, size_t n, mbstate_t *ps,
                      locale_t loc)
{
    (void)loc;
    return mbrtowc(pwc, s, n, ps);
}
size_t plsr_mbrlen_l(const char *s, size_t n, mbstate_t *ps, locale_t loc)
    __asm__("_mbrlen_l");
size_t plsr_mbrlen_l(const char *s, size_t n, mbstate_t *ps, locale_t loc)
{
    (void)loc;
    return mbrlen(s, n, ps);
}
size_t plsr_mbsrtowcs_l(wchar_t *dst, const char **src, size_t n, mbstate_t *ps,
                        locale_t loc) __asm__("_mbsrtowcs_l");
size_t plsr_mbsrtowcs_l(wchar_t *dst, const char **src, size_t n, mbstate_t *ps,
                        locale_t loc)
{
    (void)loc;
    return mbsrtowcs(dst, src, n, ps);
}
size_t plsr_mbsnrtowcs_l(wchar_t *dst, const char **src, size_t n, size_t len,
                         mbstate_t *ps, locale_t loc)
    __asm__("_mbsnrtowcs_l");
size_t plsr_mbsnrtowcs_l(wchar_t *dst, const char **src, size_t n, size_t len,
                         mbstate_t *ps, locale_t loc)
{
    (void)loc;
    return mbsnrtowcs(dst, src, n, len, ps);
}
size_t plsr_wcrtomb_l(char *s, wchar_t wc, mbstate_t *ps, locale_t loc)
    __asm__("_wcrtomb_l");
size_t plsr_wcrtomb_l(char *s, wchar_t wc, mbstate_t *ps, locale_t loc)
{
    (void)loc;
    return wcrtomb(s, wc, ps);
}
size_t plsr_wcsnrtombs_l(char *dst, const wchar_t **src, size_t n, size_t len,
                         mbstate_t *ps, locale_t loc)
    __asm__("_wcsnrtombs_l");
size_t plsr_wcsnrtombs_l(char *dst, const wchar_t **src, size_t n, size_t len,
                         mbstate_t *ps, locale_t loc)
{
    (void)loc;
    return wcsnrtombs(dst, src, n, len, ps);
}
int plsr_wctob_l(wint_t c, locale_t loc) __asm__("_wctob_l");
int plsr_wctob_l(wint_t c, locale_t loc) { (void)loc; return wctob(c); }
size_t plsr_mb_cur_max_l(locale_t loc) __asm__("____mb_cur_max_l");
size_t plsr_mb_cur_max_l(locale_t loc) { (void)loc; return MB_CUR_MAX; }
int plsr_maskrune(int c, unsigned long f) __asm__("___maskrune");
int plsr_maskrune(int c, unsigned long f)
{
    (void)f;
    return isalnum((unsigned char)c) ? 1 : 0;
}
int plsr_tolower_int(int c) __asm__("___tolower");
int plsr_tolower_int(int c) { return tolower(c); }
int plsr_toupper_int(int c) __asm__("___toupper");
int plsr_toupper_int(int c) { return toupper(c); }

/* ---- math and 128-bit helpers ---- */

double plsr_exp10(double x) __asm__("___exp10");
double plsr_exp10(double x) { return pow(10.0, x); }
int plsr_fpclassifyd(double x) __asm__("___fpclassifyd");
int plsr_fpclassifyd(double x) { return fpclassify(x); }

struct plsr_sincos { double s, c; };
struct plsr_sincos plsr_sincos_stret(double x) __asm__("___sincos_stret");
struct plsr_sincos plsr_sincos_stret(double x)
{
    struct plsr_sincos r = { sin(x), cos(x) };
    return r;
}

__int128 plsr_divti3(__int128 a, __int128 b) __asm__("___divti3");
__int128 plsr_divti3(__int128 a, __int128 b) { return a / b; }
__int128 plsr_modti3(__int128 a, __int128 b) __asm__("___modti3");
__int128 plsr_modti3(__int128 a, __int128 b) { return a % b; }
unsigned __int128 plsr_udivti3(unsigned __int128 a, unsigned __int128 b)
    __asm__("___udivti3");
unsigned __int128 plsr_udivti3(unsigned __int128 a, unsigned __int128 b)
{
    return a / b;
}
unsigned __int128 plsr_umodti3(unsigned __int128 a, unsigned __int128 b)
    __asm__("___umodti3");
unsigned __int128 plsr_umodti3(unsigned __int128 a, unsigned __int128 b)
{
    return a % b;
}

int plsr_flsl(long v) __asm__("_flsl");
int plsr_flsl(long v)
{
    unsigned int x = (unsigned int)v;
    return x ? 32 - __builtin_clz(x) : 0;
}

/* ---- process, sysctl, machine, pthread, os_log ---- */

const char *plsr_getprogname(void) __asm__("_getprogname");
const char *plsr_getprogname(void)
{
    extern char *program_invocation_short_name;
    return program_invocation_short_name;
}

int plsr_sysctl(const int *name, unsigned int namelen, void *oldp,
                size_t *oldlenp, void *newp, size_t newlen) __asm__("_sysctl");
int plsr_sysctl(const int *name, unsigned int namelen, void *oldp,
                size_t *oldlenp, void *newp, size_t newlen)
{
    (void)name;
    (void)namelen;
    (void)oldp;
    (void)oldlenp;
    (void)newp;
    (void)newlen;
    errno = ENOTSUP;
    return -1;
}

int plsr_sysctlbyname(const char *name, void *oldp, size_t *oldlenp, void *newp,
                      size_t newlen) __asm__("_sysctlbyname");
int plsr_sysctlbyname(const char *name, void *oldp, size_t *oldlenp, void *newp,
                      size_t newlen)
{
    (void)newp;
    (void)newlen;
    if (!name || !oldp || !oldlenp) {
        errno = EINVAL;
        return -1;
    }
    if (!strcmp(name, "hw.ncpu")) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        if (*oldlenp < sizeof(int))
            return -1;
        *(int *)oldp = (int)n;
        *oldlenp = sizeof(int);
        return 0;
    }
    if (!strcmp(name, "hw.pagesize")) {
        if (*oldlenp < sizeof(int))
            return -1;
        *(int *)oldp = (int)sysconf(_SC_PAGESIZE);
        *oldlenp = sizeof(int);
        return 0;
    }
    if (!strcmp(name, "hw.memsize")) {
        long pages = sysconf(_SC_PHYS_PAGES);
        long page = sysconf(_SC_PAGESIZE);
        if (*oldlenp < sizeof(int64_t))
            return -1;
        *(int64_t *)oldp = (int64_t)pages * page;
        *oldlenp = sizeof(int64_t);
        return 0;
    }
    errno = ENOTSUP;
    return -1;
}

int plsr_proc_listallpids(void *buf, int size) __asm__("_proc_listallpids");
int plsr_proc_listallpids(void *buf, int size)
{
    (void)buf;
    (void)size;
    return 0;
}
int plsr_proc_name(int pid, char *buf, unsigned int size) __asm__("_proc_name");
int plsr_proc_name(int pid, char *buf, unsigned int size)
{
    (void)pid;
    (void)buf;
    (void)size;
    errno = ESRCH;
    return -1;
}
int plsr_proc_pidpath(int pid, char *buf, unsigned int size) __asm__("_proc_pidpath");
int plsr_proc_pidpath(int pid, char *buf, unsigned int size)
{
    (void)pid;
    (void)buf;
    (void)size;
    errno = ESRCH;
    return -1;
}

uint64_t plsr_mach_absolute_time(void) __asm__("_mach_absolute_time");
uint64_t plsr_mach_absolute_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

struct plsr_timebase { uint32_t numer, denom; };
int plsr_mach_timebase_info(struct plsr_timebase *tb) __asm__("_mach_timebase_info");
int plsr_mach_timebase_info(struct plsr_timebase *tb)
{
    if (tb) {
        tb->numer = 1;
        tb->denom = 1;
    }
    return 0;
}

int plsr_mach_msg(void *msg, unsigned int opt, unsigned int send_size,
                  unsigned int rcv_size, unsigned int port, unsigned int timeout,
                  unsigned int notify) __asm__("_mach_msg");
int plsr_mach_msg(void *msg, unsigned int opt, unsigned int send_size,
                  unsigned int rcv_size, unsigned int port, unsigned int timeout,
                  unsigned int notify)
{
    (void)msg;
    (void)opt;
    (void)send_size;
    (void)rcv_size;
    (void)port;
    (void)timeout;
    (void)notify;
    return 0x10000003;   /* MACH_SEND_INVALID_DEST */
}

uint32_t plsr_mach_task_self __asm__("_mach_task_self_") = 1;
int plsr_mach_port_allocate(uint32_t task, uint32_t right, uint32_t *name)
    __asm__("_mach_port_allocate");
int plsr_mach_port_allocate(uint32_t task, uint32_t right, uint32_t *name)
{
    (void)task;
    (void)right;
    if (name)
        *name = 0;
    return 0;
}
int plsr_mach_port_deallocate(uint32_t task, uint32_t name)
    __asm__("_mach_port_deallocate");
int plsr_mach_port_deallocate(uint32_t task, uint32_t name)
{
    (void)task;
    (void)name;
    return 0;
}
int plsr_mach_port_insert_right(uint32_t task, uint32_t name, uint32_t right,
                                uint32_t right_type)
    __asm__("_mach_port_insert_right");
int plsr_mach_port_insert_right(uint32_t task, uint32_t name, uint32_t right,
                                uint32_t right_type)
{
    (void)task;
    (void)name;
    (void)right;
    (void)right_type;
    return 0;
}
int plsr_mach_port_mod_refs(uint32_t task, uint32_t name, uint32_t right,
                            int delta) __asm__("_mach_port_mod_refs");
int plsr_mach_port_mod_refs(uint32_t task, uint32_t name, uint32_t right,
                            int delta)
{
    (void)task;
    (void)name;
    (void)right;
    (void)delta;
    return 0;
}

int plsr_pthread_main_np(void) __asm__("_pthread_main_np");
int plsr_pthread_main_np(void) { return 1; }
unsigned long plsr_pthread_mach_thread_np(void *t) __asm__("_pthread_mach_thread_np");
unsigned long plsr_pthread_mach_thread_np(void *t)
{
    (void)t;
    return (unsigned long)pthread_self();
}
void *plsr_pthread_get_stackaddr_np(void *t) __asm__("_pthread_get_stackaddr_np");
void *plsr_pthread_get_stackaddr_np(void *t)
{
    (void)t;
    return NULL;
}
unsigned int plsr_qos_class_self(void) __asm__("_qos_class_self");
unsigned int plsr_qos_class_self(void) { return 0; }

void plsr_voucher_adopt(void *v) __asm__("_voucher_adopt");
void plsr_voucher_adopt(void *v) { (void)v; }
void *plsr_voucher_copy(void *v) __asm__("_voucher_copy");
void *plsr_voucher_copy(void *v) { return v; }

/* ---- os_log / signpost / unfair lock ---- */

void *plsr_os_log_default __asm__("__os_log_default") = NULL;
void *plsr_os_log_disabled __asm__("__os_log_disabled") = NULL;

void *plsr_os_log_create(const char *subsystem, const char *category)
    __asm__("_os_log_create");
void *plsr_os_log_create(const char *subsystem, const char *category)
{
    (void)subsystem;
    (void)category;
    return NULL;
}
int plsr_os_log_type_enabled(void *log, int type) __asm__("_os_log_type_enabled");
int plsr_os_log_type_enabled(void *log, int type)
{
    (void)log;
    (void)type;
    return 0;
}
void plsr_os_log_impl(void *log, void *dso, const char *fmt, unsigned char *buf,
                      unsigned int size) __asm__("__os_log_impl");
void plsr_os_log_impl(void *log, void *dso, const char *fmt, unsigned char *buf,
                      unsigned int size)
{
    (void)log;
    (void)dso;
    (void)fmt;
    (void)buf;
    (void)size;
}
void plsr_os_log_error_impl(void *log, void *dso, const char *fmt,
                            unsigned char *buf, unsigned int size)
    __asm__("__os_log_error_impl");
void plsr_os_log_error_impl(void *log, void *dso, const char *fmt,
                            unsigned char *buf, unsigned int size)
{
    (void)log;
    (void)dso;
    (void)fmt;
    (void)buf;
    (void)size;
}
void plsr_os_signpost_emit_with_name_impl(void *log, void *dso, unsigned int type,
                                          unsigned long long sid,
                                          const char *name, const char *fmt,
                                          unsigned char *buf, unsigned int size)
    __asm__("__os_signpost_emit_with_name_impl");
void plsr_os_signpost_emit_with_name_impl(void *log, void *dso, unsigned int type,
                                          unsigned long long sid,
                                          const char *name, const char *fmt,
                                          unsigned char *buf, unsigned int size)
{
    (void)log;
    (void)dso;
    (void)type;
    (void)sid;
    (void)name;
    (void)fmt;
    (void)buf;
    (void)size;
}
int plsr_os_signpost_enabled(void *log) __asm__("_os_signpost_enabled");
int plsr_os_signpost_enabled(void *log) { (void)log; return 0; }
unsigned long long plsr_os_signpost_id_generate(void *log)
    __asm__("_os_signpost_id_generate");
unsigned long long plsr_os_signpost_id_generate(void *log)
{
    (void)log;
    return 0;
}
unsigned long long plsr_os_signpost_id_make_with_pointer(void *log, const void *p)
    __asm__("_os_signpost_id_make_with_pointer");
unsigned long long plsr_os_signpost_id_make_with_pointer(void *log, const void *p)
{
    (void)log;
    return (unsigned long long)(uintptr_t)p;
}
void plsr_os_release(void *obj) __asm__("_os_release");
void plsr_os_release(void *obj) { (void)obj; }

void plsr_os_unfair_lock_lock(void *lock) __asm__("_os_unfair_lock_lock");
void plsr_os_unfair_lock_lock(void *lock)
{
    int *p = lock;
    while (__atomic_exchange_n(p, 1, __ATOMIC_ACQUIRE))
        while (__atomic_load_n(p, __ATOMIC_RELAXED))
            ;
}
int plsr_os_unfair_lock_trylock(void *lock) __asm__("_os_unfair_lock_trylock");
int plsr_os_unfair_lock_trylock(void *lock)
{
    int *p = lock;
    int expected = 0;
    return __atomic_compare_exchange_n(p, &expected, 1, 0, __ATOMIC_ACQUIRE,
                                       __ATOMIC_RELAXED);
}
void plsr_os_unfair_lock_unlock(void *lock) __asm__("_os_unfair_lock_unlock");
void plsr_os_unfair_lock_unlock(void *lock)
{
    __atomic_store_n((int *)lock, 0, __ATOMIC_RELEASE);
}

char plsr_rune_locale[4096];
void *plsr_default_rune_locale __asm__("__DefaultRuneLocale") = plsr_rune_locale;

void *plsr_getsectiondata(const char *seg, const char *sect, size_t *size)
    __asm__("_getsectiondata");
void *plsr_getsectiondata(const char *seg, const char *sect, size_t *size)
{
    (void)seg;
    (void)sect;
    if (size)
        *size = 0;
    return NULL;
}

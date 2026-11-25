/*
 * libplsr-libsystem.so: clean-room host libSystem surface.
 *
 * The loader resolves Mach-O imports by name. This library exports the
 * leading-underscore spellings those images bind to (for example _strtoul,
 * _snprintf, _gettimeofday) and forwards them to the host C library.
 *
 * Deliberately not wrapped here, because Darwin and Linux disagree on the
 * data layout or flag values and a blind forward would corrupt the caller:
 *   - open/fcntl/creat      (O_* flag values differ)
 *   - stat/fstat/lstat      (struct stat layout differs)
 *   - mmap/mprotect         (PROT_/MAP_ flag values differ)
 *   - pthread_*             (pthread_attr_t and pthread_t differ)
 *   - sysconf/_SC_*         (selector numbering differs)
 *   - dispatch_*            (separate library, not implemented yet)
 * Those need explicit translation layers rather than aliases.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

/* CoreFoundation passes Darwin's clock id 8 and Darwin TFD flags. Linux
 * timerfd only accepts CLOCK_* ids, so translate to a monotonic timerfd and
 * keep it nonblocking/cloexec. */
int plsr_timerfd_create(int clockid, int flags) __asm__("_timerfd_create");
int plsr_timerfd_create(int clockid, int flags)
{
    (void)clockid;
    (void)flags;
    return timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
}

#define W(ret, name, params, args)                    \
    ret plsr_##name params __asm__("_" #name);        \
    ret plsr_##name params { return (ret)name args; }
#define WV(name, params, args)                        \
    void plsr_##name params __asm__("_" #name);       \
    void plsr_##name params { name args; }

/* ---- stdio ---- */
W(FILE *, fopen, (const char *p, const char *m), (p, m))
W(FILE *, fdopen, (int fd, const char *m), (fd, m))
WV(fclose, (FILE *f), (f))
W(size_t, fread, (void *b, size_t s, size_t n, FILE *f), (b, s, n, f))
W(size_t, fwrite, (const void *b, size_t s, size_t n, FILE *f), (b, s, n, f))
W(int, fflush, (FILE *f), (f))
W(int, fseek, (FILE *f, long o, int w), (f, o, w))
W(long, ftell, (FILE *f), (f))
WV(rewind, (FILE *f), (f))
W(char *, fgets, (char *b, int n, FILE *f), (b, n, f))
W(int, fgetc, (FILE *f), (f))
W(int, ungetc, (int c, FILE *f), (c, f))
W(int, fputc, (int c, FILE *f), (c, f))
W(int, putchar, (int c), (c))
W(int, puts, (const char *s), (s))
W(int, fputs, (const char *s, FILE *f), (s, f))
W(int, fileno, (FILE *f), (f))
W(int, setvbuf, (FILE *f, char *b, int m, size_t n), (f, b, m, n))
W(FILE *, tmpfile, (void), ())
W(int, remove, (const char *p), (p))
W(int, rename, (const char *a, const char *b), (a, b))
W(FILE *, popen, (const char *c, const char *m), (c, m))
W(int, pclose, (FILE *f), (f))
WV(perror, (const char *s), (s))

int plsr_printf(const char *fmt, ...) __asm__("_printf");
int plsr_printf(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vfprintf(stdout, fmt, ap);
    va_end(ap); return r;
}
int plsr_fprintf(FILE *f, const char *fmt, ...) __asm__("_fprintf");
int plsr_fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vfprintf(f, fmt, ap);
    va_end(ap); return r;
}
int plsr_sprintf(char *b, const char *fmt, ...) __asm__("_sprintf");
int plsr_sprintf(char *b, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vsprintf(b, fmt, ap);
    va_end(ap); return r;
}
int plsr_snprintf(char *b, size_t n, const char *fmt, ...) __asm__("_snprintf");
int plsr_snprintf(char *b, size_t n, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(b, n, fmt, ap);
    va_end(ap); return r;
}
int plsr_vprintf(const char *fmt, va_list ap) __asm__("_vprintf");
int plsr_vprintf(const char *fmt, va_list ap) { return vprintf(fmt, ap); }
W(int, vfprintf, (FILE *f, const char *fmt, va_list ap), (f, fmt, ap))
W(int, vsprintf, (char *b, const char *fmt, va_list ap), (b, fmt, ap))
W(int, vsnprintf, (char *b, size_t n, const char *fmt, va_list ap), (b, n, fmt, ap))

/* ---- stdlib ---- */
W(void *, malloc, (size_t n), (n))
W(void *, calloc, (size_t n, size_t s), (n, s))
W(void *, realloc, (void *p, size_t n), (p, n))
WV(free, (void *p), (p))
WV(abort, (void), ())
WV(exit, (int c), (c))
W(int, atexit, (void (*fn)(void)), (fn))
W(int, atoi, (const char *s), (s))
W(long, atol, (const char *s), (s))
W(long long, atoll, (const char *s), (s))
W(long, strtol, (const char *s, char **e, int b), (s, e, b))
W(unsigned long, strtoul, (const char *s, char **e, int b), (s, e, b))
W(long long, strtoll, (const char *s, char **e, int b), (s, e, b))
W(unsigned long long, strtoull, (const char *s, char **e, int b), (s, e, b))
W(double, strtod, (const char *s, char **e), (s, e))
W(float, strtof, (const char *s, char **e), (s, e))
W(int, abs, (int v), (v))
W(long, labs, (long v), (v))
W(long long, llabs, (long long v), (v))
WV(srand, (unsigned int s), (s))
W(int, rand, (void), ())
W(char *, getenv, (const char *n), (n))
W(int, setenv, (const char *n, const char *v, int o), (n, v, o))
W(int, unsetenv, (const char *n), (n))
W(int, putenv, (char *s), (s))
W(int, system, (const char *c), (c))
W(char *, realpath, (const char *p, char *r), (p, r))
W(int, mkstemp, (char *t), (t))
WV(qsort, (void *b, size_t n, size_t s, int (*c)(const void *, const void *)), (b, n, s, c))
W(void *, bsearch, (const void *k, const void *b, size_t n, size_t s,
                    int (*c)(const void *, const void *)), (k, b, n, s, c))

/* ---- string ---- */
W(void *, memcpy, (void *d, const void *s, size_t n), (d, s, n))
W(void *, memmove, (void *d, const void *s, size_t n), (d, s, n))
W(void *, memset, (void *d, int c, size_t n), (d, c, n))
W(int, memcmp, (const void *a, const void *b, size_t n), (a, b, n))
W(void *, memchr, (const void *s, int c, size_t n), (s, c, n))
W(size_t, strlen, (const char *s), (s))
W(int, strcmp, (const char *a, const char *b), (a, b))
W(int, strncmp, (const char *a, const char *b, size_t n), (a, b, n))
W(char *, strcpy, (char *d, const char *s), (d, s))
W(char *, strncpy, (char *d, const char *s, size_t n), (d, s, n))
W(char *, strcat, (char *d, const char *s), (d, s))
W(char *, strncat, (char *d, const char *s, size_t n), (d, s, n))
W(char *, strchr, (const char *s, int c), (s, c))
W(char *, strrchr, (const char *s, int c), (s, c))
W(char *, index, (const char *s, int c), (s, c))
W(char *, rindex, (const char *s, int c), (s, c))
W(char *, strstr, (const char *h, const char *n), (h, n))
W(char *, strdup, (const char *s), (s))
W(char *, strndup, (const char *s, size_t n), (s, n))
W(char *, stpcpy, (char *d, const char *s), (d, s))
W(size_t, strspn, (const char *s, const char *a), (s, a))
W(size_t, strcspn, (const char *s, const char *r), (s, r))
W(char *, strpbrk, (const char *s, const char *a), (s, a))
W(char *, strtok, (char *s, const char *d), (s, d))
W(char *, strtok_r, (char *s, const char *d, char **p), (s, d, p))
W(int, strcasecmp, (const char *a, const char *b), (a, b))
W(int, strncasecmp, (const char *a, const char *b, size_t n), (a, b, n))
W(char *, strerror, (int e), (e))
WV(bzero, (void *p, size_t n), (p, n))
WV(bcopy, (const void *s, void *d, size_t n), (s, d, n))

/* ---- ctype ---- */
W(int, isalpha, (int c), (c))
W(int, isdigit, (int c), (c))
W(int, isalnum, (int c), (c))
W(int, isspace, (int c), (c))
W(int, isupper, (int c), (c))
W(int, islower, (int c), (c))
W(int, isprint, (int c), (c))
W(int, ispunct, (int c), (c))
W(int, isxdigit, (int c), (c))
W(int, toupper, (int c), (c))
W(int, tolower, (int c), (c))

/* ---- unistd ---- */
W(ssize_t, write, (int fd, const void *b, size_t n), (fd, b, n))
W(ssize_t, read, (int fd, void *b, size_t n), (fd, b, n))
W(int, close, (int fd), (fd))
W(off_t, lseek, (int fd, off_t o, int w), (fd, o, w))
W(int, dup, (int fd), (fd))
W(int, dup2, (int a, int b), (a, b))
W(int, pipe, (int fds[2]), (fds))
/* Mach-O `exit` is symbol _exit; Mach-O `_exit` is __exit. Calling glibc
 * _exit here would resolve to our own interposing definition, so use _Exit. */
void plsr__exit(int c) __asm__("__exit");
void plsr__exit(int c) { _Exit(c); }
W(pid_t, fork, (void), ())
W(int, execv, (const char *p, char *const argv[]), (p, argv))
W(int, execve, (const char *p, char *const argv[], char *const envp[]), (p, argv, envp))
W(unsigned int, sleep, (unsigned int s), (s))
W(int, usleep, (useconds_t u), (u))
W(pid_t, getpid, (void), ())
W(pid_t, getppid, (void), ())
W(uid_t, getuid, (void), ())
W(uid_t, geteuid, (void), ())
W(gid_t, getgid, (void), ())
W(gid_t, getegid, (void), ())
W(int, isatty, (int fd), (fd))
W(int, fsync, (int fd), (fd))
W(int, truncate, (const char *p, off_t n), (p, n))
W(int, ftruncate, (int fd, off_t n), (fd, n))
W(char *, getcwd, (char *b, size_t n), (b, n))
W(int, chdir, (const char *p), (p))
W(int, access, (const char *p, int m), (p, m))
W(int, unlink, (const char *p), (p))
W(int, rmdir, (const char *p), (p))
W(int, mkdir, (const char *p, mode_t m), (p, m))
W(int, chmod, (const char *p, mode_t m), (p, m))
W(int, fchmod, (int fd, mode_t m), (fd, m))
W(mode_t, umask, (mode_t m), (m))
W(int, link, (const char *a, const char *b), (a, b))
W(int, symlink, (const char *a, const char *b), (a, b))
W(ssize_t, readlink, (const char *p, char *b, size_t n), (p, b, n))
W(int, getpagesize, (void), ())

/* ---- time ---- */
W(time_t, time, (time_t *t), (t))
W(int, gettimeofday, (struct timeval *tv, void *tz), (tv, tz))
W(int, nanosleep, (const struct timespec *r, struct timespec *rem), (r, rem))
W(struct tm *, localtime, (const time_t *t), (t))
W(struct tm *, gmtime, (const time_t *t), (t))
W(time_t, mktime, (struct tm *tm), (tm))
W(size_t, strftime, (char *b, size_t n, const char *f, const struct tm *tm), (b, n, f, tm))
W(double, difftime, (time_t a, time_t b), (a, b))

/* ---- math ---- */
W(double, sin, (double x), (x))
W(double, cos, (double x), (x))
W(double, tan, (double x), (x))
W(double, asin, (double x), (x))
W(double, acos, (double x), (x))
W(double, atan2, (double y, double x), (y, x))
W(double, sqrt, (double x), (x))
W(double, pow, (double x, double y), (x, y))
W(double, exp, (double x), (x))
W(double, log, (double x), (x))
W(double, log2, (double x), (x))
W(double, log10, (double x), (x))
W(double, fabs, (double x), (x))
W(double, fmod, (double x, double y), (x, y))
W(double, floor, (double x), (x))
W(double, ceil, (double x), (x))
W(double, round, (double x), (x))
W(double, trunc, (double x), (x))
W(double, fmin, (double x, double y), (x, y))
W(double, fmax, (double x, double y), (x, y))

/* ---- errno, randomness ---- */
int *plsr_error(void) __asm__("___error");
int *plsr_error(void) { return __errno_location(); }

uint32_t plsr_arc4random(void) __asm__("_arc4random");
uint32_t plsr_arc4random(void)
{
    uint32_t v;
    if (getrandom(&v, sizeof v, 0) != (ssize_t)sizeof v)
        v = (uint32_t)rand();
    return v;
}
uint32_t plsr_arc4random_uniform(uint32_t u) __asm__("_arc4random_uniform");
uint32_t plsr_arc4random_uniform(uint32_t u)
{
    return u ? plsr_arc4random() % u : 0;
}
int plsr_getentropy(void *b, size_t n) __asm__("_getentropy");
int plsr_getentropy(void *b, size_t n)
{
    return getrandom(b, n, 0) == (ssize_t)n ? 0 : -1;
}

/* ---- environ ---- */
char **plsr_environ __asm__("_environ");
static void plsr_environ_init(void) __attribute__((constructor));
static void plsr_environ_init(void) { plsr_environ = environ; }

/* ---- Darwin ABI translations ------------------------------------------- *
 * These cannot be plain aliases: Darwin and Linux disagree on flag values and
 * on the layout of stat and the pthread opaque types, so each entry point
 * converts in both directions. Sizes are checked at compile time.
 */

struct plsr_darwin_timespec { int64_t tv_sec; int64_t tv_nsec; };
struct plsr_darwin_stat {
    int32_t  st_dev;
    uint16_t st_mode;
    uint16_t st_nlink;
    uint64_t st_ino;
    uint32_t st_uid;
    uint32_t st_gid;
    int32_t  st_rdev;
    int32_t  __pad0;
    struct plsr_darwin_timespec st_atimespec, st_mtimespec;
    struct plsr_darwin_timespec st_ctimespec, st_birthtimespec;
    int64_t  st_size;
    int64_t  st_blocks;
    int32_t  st_blksize;
    uint32_t st_flags;
    uint32_t st_gen;
    int32_t  st_lspare;
    int64_t  st_qspare[2];
};
_Static_assert(sizeof(struct plsr_darwin_stat) == 144, "Darwin stat is 144 bytes");
_Static_assert(offsetof(struct plsr_darwin_stat, st_size) == 96, "Darwin st_size is at 96");

static void fill_darwin_stat(const struct stat *s, struct plsr_darwin_stat *d)
{
    memset(d, 0, sizeof(*d));
    d->st_dev = (int32_t)s->st_dev;
    d->st_mode = (uint16_t)s->st_mode;
    d->st_nlink = (uint16_t)s->st_nlink;
    d->st_ino = (uint64_t)s->st_ino;
    d->st_uid = (uint32_t)s->st_uid;
    d->st_gid = (uint32_t)s->st_gid;
    d->st_rdev = (int32_t)s->st_rdev;
    d->st_atimespec.tv_sec = (int64_t)s->st_atim.tv_sec;
    d->st_atimespec.tv_nsec = (int64_t)s->st_atim.tv_nsec;
    d->st_mtimespec.tv_sec = (int64_t)s->st_mtim.tv_sec;
    d->st_mtimespec.tv_nsec = (int64_t)s->st_mtim.tv_nsec;
    d->st_ctimespec.tv_sec = (int64_t)s->st_ctim.tv_sec;
    d->st_ctimespec.tv_nsec = (int64_t)s->st_ctim.tv_nsec;
    d->st_size = (int64_t)s->st_size;
    d->st_blocks = (int64_t)s->st_blocks;
    d->st_blksize = (int32_t)s->st_blksize;
}

static int stat_path(const char *p, struct plsr_darwin_stat *d)
{
    struct stat s;
    int r = stat(p, &s);
    if (r == 0)
        fill_darwin_stat(&s, d);
    return r;
}
static int lstat_path(const char *p, struct plsr_darwin_stat *d)
{
    struct stat s;
    int r = lstat(p, &s);
    if (r == 0)
        fill_darwin_stat(&s, d);
    return r;
}
static int fstat_fd(int fd, struct plsr_darwin_stat *d)
{
    struct stat s;
    int r = fstat(fd, &s);
    if (r == 0)
        fill_darwin_stat(&s, d);
    return r;
}

int plsr_stat(const char *p, struct plsr_darwin_stat *d) __asm__("_stat");
int plsr_stat(const char *p, struct plsr_darwin_stat *d) { return stat_path(p, d); }
int plsr_stat_inode64(const char *p, struct plsr_darwin_stat *d) __asm__("_stat$INODE64");
int plsr_stat_inode64(const char *p, struct plsr_darwin_stat *d) { return stat_path(p, d); }
int plsr_lstat(const char *p, struct plsr_darwin_stat *d) __asm__("_lstat");
int plsr_lstat(const char *p, struct plsr_darwin_stat *d) { return lstat_path(p, d); }
int plsr_lstat_inode64(const char *p, struct plsr_darwin_stat *d) __asm__("_lstat$INODE64");
int plsr_lstat_inode64(const char *p, struct plsr_darwin_stat *d) { return lstat_path(p, d); }
int plsr_fstat(int fd, struct plsr_darwin_stat *d) __asm__("_fstat");
int plsr_fstat(int fd, struct plsr_darwin_stat *d) { return fstat_fd(fd, d); }
int plsr_fstat_inode64(int fd, struct plsr_darwin_stat *d) __asm__("_fstat$INODE64");
int plsr_fstat_inode64(int fd, struct plsr_darwin_stat *d) { return fstat_fd(fd, d); }

int plsr_fstatat(int fd, const char *p, struct plsr_darwin_stat *d, int flag)
    __asm__("_fstatat");
int plsr_fstatat(int fd, const char *p, struct plsr_darwin_stat *d, int flag)
{
    struct stat s;
    int host_flag = (flag & 0x0020) ? AT_SYMLINK_NOFOLLOW : 0;   /* Darwin AT_SYMLINK_NOFOLLOW */
    int r = fstatat(fd, p, &s, host_flag);
    if (r == 0)
        fill_darwin_stat(&s, d);
    return r;
}
int plsr_fstatat_inode64(int fd, const char *p, struct plsr_darwin_stat *d, int flag)
    __asm__("_fstatat$INODE64");
int plsr_fstatat_inode64(int fd, const char *p, struct plsr_darwin_stat *d, int flag)
{
    return plsr_fstatat(fd, p, d, flag);
}

/* Darwin O_* values; Linux uses different bits for every flag but access mode. */
static int darwin_open_flags(int f)
{
    int out = f & 3;
    if (f & 0x0004) out |= O_NONBLOCK;
    if (f & 0x0008) out |= O_APPEND;
    if (f & 0x0040) out |= O_ASYNC;
    if (f & 0x0080) out |= O_SYNC;
    if (f & 0x0100) out |= O_NOFOLLOW;
    if (f & 0x0200) out |= O_CREAT;
    if (f & 0x0400) out |= O_TRUNC;
    if (f & 0x0800) out |= O_EXCL;
    if (f & 0x20000) out |= O_NOCTTY;
    if (f & 0x100000) out |= O_DIRECTORY;
    if (f & 0x1000000) out |= O_CLOEXEC;
    return out;
}

int plsr_open(const char *p, int flags, ...) __asm__("_open");
int plsr_open(const char *p, int flags, ...)
{
    mode_t mode = 0;
    if (flags & 0x0200) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    return open(p, darwin_open_flags(flags), mode);
}
int plsr_openat(int fd, const char *p, int flags, ...) __asm__("_openat");
int plsr_openat(int fd, const char *p, int flags, ...)
{
    mode_t mode = 0;
    if (flags & 0x0200) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    return openat(fd, p, darwin_open_flags(flags), mode);
}
int plsr_creat(const char *p, mode_t mode) __asm__("_creat");
int plsr_creat(const char *p, mode_t mode)
{
    return open(p, O_WRONLY | O_CREAT | O_TRUNC, mode);
}

int plsr_fcntl(int fd, int cmd, ...) __asm__("_fcntl");
int plsr_fcntl(int fd, int cmd, ...)
{
    va_list ap;
    void *arg;
    va_start(ap, cmd);
    arg = va_arg(ap, void *);
    va_end(ap);
    switch (cmd) {
    case 0:  /* F_DUPFD */
    case 1:  /* F_GETFD */
    case 2:  /* F_SETFD */
    case 3:  /* F_GETFL */
    case 4:  /* F_SETFL */
        return fcntl(fd, cmd, arg);
    default:
        /* Lock and preallocate commands carry Darwin-only structures. */
        errno = EINVAL;
        return -1;
    }
}

/* Darwin MAP_ANON is 0x1000, Linux MAP_ANONYMOUS is 0x20. */
static int darwin_map_flags(int f)
{
    int out = f & 3;
    if (f & 0x0010) out |= MAP_FIXED;
    if (f & 0x0040) out |= MAP_NORESERVE;
    if (f & 0x1000) out |= MAP_ANONYMOUS;
    return out;
}

void *plsr_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
    __asm__("_mmap");
void *plsr_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    return mmap(addr, len, prot, darwin_map_flags(flags), fd, off);
}
int plsr_munmap(void *addr, size_t len) __asm__("_munmap");
int plsr_munmap(void *addr, size_t len) { return munmap(addr, len); }
int plsr_mprotect(void *addr, size_t len, int prot) __asm__("_mprotect");
int plsr_mprotect(void *addr, size_t len, int prot) { return mprotect(addr, len, prot); }
int plsr_msync(void *addr, size_t len, int flags) __asm__("_msync");
int plsr_msync(void *addr, size_t len, int flags) { return msync(addr, len, flags); }

/* ---- pthread: Darwin handles are pointers, Linux pthread_t is an integer,
 * and the opaque mutex is 64 bytes with no room for the host type alone.
 * A 56-byte offset marker distinguishes an initialised block. */

struct plsr_thread {
    pthread_t host;
    void *(*fn)(void *);
    void *arg;
    void *ret;
};

static pthread_key_t g_self_key;
static pthread_once_t g_self_once = PTHREAD_ONCE_INIT;
static void self_key_init(void) { pthread_key_create(&g_self_key, NULL); }

static void *thread_trampoline(void *p)
{
    struct plsr_thread *t = p;
    pthread_setspecific(g_self_key, t);
    t->ret = t->fn(t->arg);
    return t->ret;
}

int plsr_pthread_create(void **out, const void *attr, void *(*fn)(void *), void *arg)
    __asm__("_pthread_create");
int plsr_pthread_create(void **out, const void *attr, void *(*fn)(void *), void *arg)
{
    struct plsr_thread *t;
    int r;
    (void)attr;   /* Darwin attributes carry no portable equivalent. */
    if (!out || !fn)
        return EINVAL;
    t = calloc(1, sizeof(*t));
    if (!t)
        return ENOMEM;
    t->fn = fn;
    t->arg = arg;
    r = pthread_create(&t->host, NULL, thread_trampoline, t);
    if (r != 0) {
        free(t);
        return r;
    }
    *out = t;
    return 0;
}

int plsr_pthread_join(void *thread, void **ret) __asm__("_pthread_join");
int plsr_pthread_join(void *thread, void **ret)
{
    struct plsr_thread *t = thread;
    int r;
    if (!t)
        return EINVAL;
    r = pthread_join(t->host, NULL);
    if (ret)
        *ret = t->ret;
    free(t);
    return r;
}

int plsr_pthread_detach(void *thread) __asm__("_pthread_detach");
int plsr_pthread_detach(void *thread)
{
    struct plsr_thread *t = thread;
    if (!t)
        return EINVAL;
    return pthread_detach(t->host);   /* handle is released by the trampoline side */
}

void *plsr_pthread_self(void) __asm__("_pthread_self");
void *plsr_pthread_self(void)
{
    struct plsr_thread *t;
    pthread_once(&g_self_once, self_key_init);
    t = pthread_getspecific(g_self_key);
    if (!t) {
        t = calloc(1, sizeof(*t));
        if (!t)
            return NULL;
        t->host = pthread_self();
        pthread_setspecific(g_self_key, t);
    }
    return t;
}

int plsr_pthread_equal(void *a, void *b) __asm__("_pthread_equal");
int plsr_pthread_equal(void *a, void *b) { return a == b; }

void plsr_pthread_exit(void *ret) __asm__("_pthread_exit");
void plsr_pthread_exit(void *ret) { pthread_exit(ret); }

/* Darwin pthread_once_t is a long; the host one is an int. Darwin's
 * PTHREAD_ONCE_INIT carries a signature instead of zero. */
#define DARWIN_ONCE_SIG 0x30b1bcbaL
static pthread_mutex_t g_once_lock = PTHREAD_MUTEX_INITIALIZER;
int plsr_pthread_once(long *once, void (*fn)(void)) __asm__("_pthread_once");
int plsr_pthread_once(long *once, void (*fn)(void))
{
    long state;
    if (!once || !fn)
        return EINVAL;
    state = __atomic_load_n(once, __ATOMIC_ACQUIRE);
    if (state == 0 || state == DARWIN_ONCE_SIG) {
        pthread_mutex_lock(&g_once_lock);
        state = *once;
        if (state == 0 || state == DARWIN_ONCE_SIG) {
            fn();
            __atomic_store_n(once, 1, __ATOMIC_RELEASE);
        }
        pthread_mutex_unlock(&g_once_lock);
    }
    return 0;
}

#define PLSR_MUTEX_MARKER 0x504c534d55544558ULL   /* "PLSMUTEX" */
static pthread_mutex_t g_mutex_init = PTHREAD_MUTEX_INITIALIZER;

/* Darwin's runtime uses recursive mutexes for its initialization locks. The
 * shim cannot recover the type from a static Darwin initializer, so every
 * translated mutex is recursive; that preserves correct nesting and does not
 * change normal lock/unlock behaviour. */
static void host_mutex_init(pthread_mutex_t *m)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(m, &attr);
    pthread_mutexattr_destroy(&attr);
}

static pthread_mutex_t *mutex_of(void *m)
{
    uint64_t *mark = (uint64_t *)((char *)m + 56);
    if (__atomic_load_n(mark, __ATOMIC_ACQUIRE) != PLSR_MUTEX_MARKER) {
        pthread_mutex_lock(&g_mutex_init);
        if (*mark != PLSR_MUTEX_MARKER) {
            host_mutex_init((pthread_mutex_t *)m);
            __atomic_store_n(mark, PLSR_MUTEX_MARKER, __ATOMIC_RELEASE);
        }
        pthread_mutex_unlock(&g_mutex_init);
    }
    return (pthread_mutex_t *)m;
}

int plsr_pthread_mutex_init(void *m, const void *attr) __asm__("_pthread_mutex_init");
int plsr_pthread_mutex_init(void *m, const void *attr)
{
    (void)attr;
    if (!m)
        return EINVAL;
    host_mutex_init((pthread_mutex_t *)m);
    __atomic_store_n((uint64_t *)((char *)m + 56), PLSR_MUTEX_MARKER, __ATOMIC_RELEASE);
    return 0;
}
int plsr_pthread_mutex_destroy(void *m) __asm__("_pthread_mutex_destroy");
int plsr_pthread_mutex_destroy(void *m)
{
    if (!m)
        return EINVAL;
    pthread_mutex_destroy(mutex_of(m));
    __atomic_store_n((uint64_t *)((char *)m + 56), 0, __ATOMIC_RELEASE);
    return 0;
}
int plsr_pthread_mutex_lock(void *m) __asm__("_pthread_mutex_lock");
int plsr_pthread_mutex_lock(void *m)
{
    pthread_mutex_t *hm = mutex_of(m);
    if (getenv("PLSR_PTHREAD_TRACE")) {
        uint64_t sig = 0, mark = 0;
        int owner = 0, tr;
        if (m) {
            memcpy(&sig, m, sizeof(sig));
            memcpy(&mark, (char *)m + 56, sizeof(mark));
            memcpy(&owner, (char *)m + 8, sizeof(owner));
        }
        tr = pthread_mutex_trylock(hm);
        if (tr == 0)
            pthread_mutex_unlock(hm);
        dprintf(2, "pthread_mutex_lock m=%p sig=%llx mark=%llx owner=%d try=%d self=%ld\n",
                m, (unsigned long long)sig, (unsigned long long)mark, owner, tr,
                (long)syscall(SYS_gettid));
    }
    return pthread_mutex_lock(hm);
}
int plsr_pthread_mutex_trylock(void *m) __asm__("_pthread_mutex_trylock");
int plsr_pthread_mutex_trylock(void *m) { return pthread_mutex_trylock(mutex_of(m)); }
int plsr_pthread_mutex_unlock(void *m) __asm__("_pthread_mutex_unlock");
int plsr_pthread_mutex_unlock(void *m) { return pthread_mutex_unlock(mutex_of(m)); }

int plsr_pthread_key_create(unsigned long *key, void (*dtor)(void *))
    __asm__("_pthread_key_create");
int plsr_pthread_key_create(unsigned long *key, void (*dtor)(void *))
{
    if (!key)
        return EINVAL;
    *key = 0;
    return pthread_key_create((pthread_key_t *)key, dtor);
}
void *plsr_pthread_getspecific(unsigned long key) __asm__("_pthread_getspecific");
void *plsr_pthread_getspecific(unsigned long key)
{
    return pthread_getspecific((pthread_key_t)key);
}
int plsr_pthread_setspecific(unsigned long key, const void *value)
    __asm__("_pthread_setspecific");
int plsr_pthread_setspecific(unsigned long key, const void *value)
{
    return pthread_setspecific((pthread_key_t)key, value);
}

/* Install names this library satisfies, read by an external loader. Only
 * libraries whose whole used surface is above are listed; over-claiming would
 * turn a missing symbol into a silent zero bind. */
__attribute__((visibility("default"), used))
const char *const plsr_host_imports[] = {
    "/usr/lib/libSystem.B.dylib",
    "/usr/lib/system/libsystem_c.dylib",
    "/usr/lib/system/libsystem_m.dylib",
    NULL
};

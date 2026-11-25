/*
 * Remaining Darwin host symbols used by the clean-room libSystem shim:
 * stdio globals, reef host bridge, futex-style ulock, half-float conversion,
 * and libunwind forwarding.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <linux/futex.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <unwind.h>

/* ---- stdio pointers ---- */
FILE *plsr_stderrp __asm__("___stderrp");
FILE *plsr_stdinp __asm__("___stdinp");
FILE *plsr_stdoutp __asm__("___stdoutp");

/* ---- reef host bridge ---- */
static void *reef_stub(void)
{
    return NULL;
}

static void *reef_table[0x200];
static void *reef_host_api[0x800 / sizeof(void *)];
static void *reef_launch_items_none[1] = { NULL };
void *plsr_reef_host __asm__("_reef_host") = reef_host_api;

static void reef_preload_toolkit(const char *root)
{
    static const char *const names[] = {
        "libicudata.so.76.1", "libicuuc.so.76.1", "libicui18n.so.76.1",
        "libjpeg.so.62.3.0", "libpulsar-toolkit-base.so.1.0",
        "libpulsar-toolkit.so.1.0"
    };
    char path[4096];
    if (!root)
        return;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        void *h;
        snprintf(path, sizeof path, "%s/compat/%s", root, names[i]);
        h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
        if (!h && getenv("PLSR_REEF_TRACE"))
            dprintf(2, "reef: preload %s failed: %s\n", path, dlerror());
    }
}

static void *reef_host_load(const char *path, const char *symbol)
{
    const char *root = getenv("PLSR_ROOT");
    char resolved[4096];
    void *handle, *fn;
    if (getenv("PLSR_REEF_TRACE"))
        dprintf(2, "reef: host_load path=%s symbol=%s root=%s\n",
                path ? path : "(null)", symbol ? symbol : "(null)",
                root ? root : "(null)");
    if (!path || !symbol)
        return NULL;
    reef_preload_toolkit(root);
    if (!strncmp(path, "@root/", 6)) {
        snprintf(resolved, sizeof resolved, "%s/%s", root ? root : ".", path + 6);
        path = resolved;
    }
    handle = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (!handle) {
        dprintf(2, "reef: dlopen %s failed: %s\n", path, dlerror());
        return NULL;
    }
    fn = dlsym(handle, symbol);
    if (!fn)
        dprintf(2, "reef: dlsym %s in %s failed: %s\n", symbol, path, dlerror());
    return fn;
}
const char *plsr_reef_loader_path(void) __asm__("___reef_loader_path");
const char *plsr_reef_loader_path(void)
{
    const char *p = reef_host_api[0x580 / sizeof(void *)];
    if (p)
        return p;
    p = getenv("PLSR_APP_BUNDLE");
    if (!p)
        p = getenv("PLSR_ROOT");
    return p ? p : "";
}

void **plsr_reef_launch_items(void) __asm__("___reef_launch_items");
void **plsr_reef_launch_items(void)
{
    void **p = reef_host_api[0x588 / sizeof(void *)];
    return p ? p : reef_launch_items_none;
}

void *plsr_reef_host_symbol(const char *name) __asm__("_reef_host_symbol");
void *plsr_reef_host_symbol(const char *name)
{
    return name ? dlsym(RTLD_DEFAULT, name) : NULL;
}

void *plsr_reef_anonymous_shared_memory(size_t size)
    __asm__("_reef_anonymous_shared_memory");
void *plsr_reef_anonymous_shared_memory(size_t size)
{
    void *p = mmap(NULL, size ? size : 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

void *plsr_reef_low_alloc(size_t size) __asm__("___reef_low_alloc");
void *plsr_reef_low_alloc(size_t size) { return malloc(size); }

void *plsr_reef_bridge_missing(const char *what) __asm__("_reef_bridge_missing");
void *plsr_reef_bridge_missing(const char *what)
{
    if (what)
        dprintf(2, "reef: missing bridge %s\n", what);
    return NULL;
}

void plsr_reef_report_unsupported(const char *what)
    __asm__("_reef_report_unsupported");
void plsr_reef_report_unsupported(const char *what)
{
    if (what)
        dprintf(2, "reef: unsupported %s\n", what);
}

int plsr_reef_sandbox_allows(const char *path, int op)
    __asm__("_reef_sandbox_allows");
int plsr_reef_sandbox_allows(const char *path, int op)
{
    (void)path;
    (void)op;
    return 1;
}

void *plsr_reef_mach_object_port_copy(void *port) __asm__("_reef_mach_object_port_copy");
void *plsr_reef_mach_object_port_copy(void *port)
{
    (void)port;
    return NULL;
}

void *plsr_reef_mach_object_port_make(void *port) __asm__("_reef_mach_object_port_make");
void *plsr_reef_mach_object_port_make(void *port)
{
    (void)port;
    return NULL;
}

void plsr_reef_set_block_hook(void *hook) __asm__("___reef_set_block_hook");
void plsr_reef_set_block_hook(void *hook) { (void)hook; }

void *plsr_reef_trace_retains(void *obj) __asm__("__reefTraceRetainsOf");
void *plsr_reef_trace_retains(void *obj) { return obj; }
void *plsr_reef_trace_retains_prefix(void *obj) __asm__("__reefTraceRetainsOfPrefix");
void *plsr_reef_trace_retains_prefix(void *obj) { return obj; }

void plsr_reef_dbus_emit(const void *a, const void *b)
    __asm__("___reefDBusEmitSignal");
void plsr_reef_dbus_emit(const void *a, const void *b) { (void)a; (void)b; }
void plsr_reef_dbus_value(const void *a, const void *b)
    __asm__("___reefDBusTypedValue");
void plsr_reef_dbus_value(const void *a, const void *b) { (void)a; (void)b; }

/* ---- Darwin ulock (futex) ---- */
int plsr_ulock_wait(uint32_t op, void *addr, uint64_t value, uint32_t timeout_us)
    __asm__("___ulock_wait");
int plsr_ulock_wait(uint32_t op, void *addr, uint64_t value, uint32_t timeout_us)
{
    struct timespec ts, *tsp = NULL;
    int r;
    (void)op;
    if (timeout_us) {
        ts.tv_sec = timeout_us / 1000000u;
        ts.tv_nsec = (long)(timeout_us % 1000000u) * 1000L;
        tsp = &ts;
    }
    r = (int)syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, (int)value, tsp, NULL, 0);
    return r < 0 ? -errno : 0;
}

int plsr_ulock_wake(uint32_t op, void *addr, uint64_t value)
    __asm__("___ulock_wake");
int plsr_ulock_wake(uint32_t op, void *addr, uint64_t value)
{
    int r;
    (void)op;
    (void)value;
    r = (int)syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
    return r < 0 ? -errno : r;
}

/* ---- half-float truncation ---- */
uint16_t plsr_truncsfhf2(float f) __asm__("___truncsfhf2");
uint16_t plsr_truncsfhf2(float f)
{
    uint32_t x, sign, exp, mant;
    uint16_t h;
    memcpy(&x, &f, sizeof(x));
    sign = (x >> 16) & 0x8000u;
    exp = (x >> 23) & 0xffu;
    mant = x & 0x7fffffu;
    if (exp == 0xffu)
        return (uint16_t)(sign | 0x7c00u | (mant ? 0x0200u : 0));
    if (exp == 0)
        return (uint16_t)sign;
    exp = exp - 127u + 15u;
    if (exp >= 31u)
        return (uint16_t)(sign | 0x7c00u);
    if (exp == 0)
        return (uint16_t)sign;
    h = (uint16_t)(sign | (exp << 10) | (mant >> 13));
    return h;
}

/* ---- libunwind forwarding ---- */
_Unwind_Reason_Code plsr_unwind_raise(struct _Unwind_Exception *e)
    __asm__("__Unwind_RaiseException");
_Unwind_Reason_Code plsr_unwind_raise(struct _Unwind_Exception *e)
{
    return _Unwind_RaiseException(e);
}

void plsr_unwind_resume(struct _Unwind_Exception *e) __asm__("__Unwind_Resume");
void plsr_unwind_resume(struct _Unwind_Exception *e)
{
    _Unwind_Resume(e);
}

void plsr_unwind_delete(struct _Unwind_Exception *e) __asm__("__Unwind_DeleteException");
void plsr_unwind_delete(struct _Unwind_Exception *e)
{
    _Unwind_DeleteException(e);
}

_Unwind_Ptr plsr_unwind_get_ip(struct _Unwind_Context *c) __asm__("__Unwind_GetIP");
_Unwind_Ptr plsr_unwind_get_ip(struct _Unwind_Context *c)
{
    return _Unwind_GetIP(c);
}

_Unwind_Ptr plsr_unwind_get_language(struct _Unwind_Context *c)
    __asm__("__Unwind_GetLanguageSpecificData");
_Unwind_Ptr plsr_unwind_get_language(struct _Unwind_Context *c)
{
    return (_Unwind_Ptr)_Unwind_GetLanguageSpecificData(c);
}

_Unwind_Ptr plsr_unwind_get_region(struct _Unwind_Context *c)
    __asm__("__Unwind_GetRegionStart");
_Unwind_Ptr plsr_unwind_get_region(struct _Unwind_Context *c)
{
    return _Unwind_GetRegionStart(c);
}

void plsr_unwind_set_gr(struct _Unwind_Context *c, int reg, _Unwind_Word value)
    __asm__("__Unwind_SetGR");
void plsr_unwind_set_gr(struct _Unwind_Context *c, int reg, _Unwind_Word value)
{
    _Unwind_SetGR(c, reg, value);
}

void plsr_unwind_set_ip(struct _Unwind_Context *c, _Unwind_Ptr ip)
    __asm__("__Unwind_SetIP");
void plsr_unwind_set_ip(struct _Unwind_Context *c, _Unwind_Ptr ip)
{
    _Unwind_SetIP(c, ip);
}

__attribute__((constructor)) static void platform_symbols_init(void)
{
    plsr_stderrp = stderr;
    plsr_stdinp = stdin;
    plsr_stdoutp = stdout;
    for (size_t i = 0; i < sizeof(reef_table) / sizeof(reef_table[0]); i++)
        reef_table[i] = (void *)reef_stub;
    for (size_t i = 0; i < sizeof(reef_host_api) / sizeof(reef_host_api[0]); i++)
        reef_host_api[i] = (void *)reef_stub;
    reef_host_api[0x250 / sizeof(void *)] = (void *)reef_host_load;
    reef_host_api[0x580 / sizeof(void *)] = NULL;
    reef_host_api[0x588 / sizeof(void *)] = NULL;
    reef_table[0] = reef_host_api;
    if (getenv("PLSR_REEF_TRACE"))
        dprintf(2, "reef: table=%p api=%p slot250=%p\n",
                (void *)reef_table, (void *)reef_host_api,
                reef_host_api[0x250 / sizeof(void *)]);
}

__attribute__((visibility("default"), used))
const char *const plsr_platform_imports[] = { "/usr/lib/libSystem.B.dylib", NULL };

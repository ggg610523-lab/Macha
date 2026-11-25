/*
 * plsr-lite: a from-scratch x86_64 Mach-O loader.
 *
 * Clean-room source reconstructed from the decompiled pulsar-appexec
 * (src/plsr) behaviour: argument handling, runtime-root discovery from
 * /proc/self/exe, Mach-O validation, segment mapping, dyld rebases/binds,
 * chained fixups, and dependency loading. It is not a dyld clone; the
 * host libc surface is provided by a small shim table and a macOS image
 * graph is resolved under a Pulsar runtime root.
 *
 * Modes:
 *   plsr-lite [--runtime DIR] <macho|.app> [args...]   load and run
 *   plsr-lite --info [--runtime DIR] <macho|.app>      static inventory
 *   plsr-lite --load [--runtime DIR] <macho|.app>      load closure, no run
 */
#define _GNU_SOURCE
#include "reef_host.h"
#include "engine.h"

static char *plsr_nsutf8(void *s);
static void *plsr_nsstring(const char *s);
static int plsr_image_pointer(uintptr_t v);
static int plsr_valid_obj(const void *p);
#include "engine.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <limits.h>
#include <malloc.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#include <zlib.h>
#include <ctype.h>
#include <time.h>

#define PAGE 4096UL
#define ROUNDUP(x) (((x) + (PAGE - 1)) & ~(PAGE - 1))
#define MAX_SEGS 64
#define MAX_DYLIBS 128
#define MAX_IMAGES 1024
#define MAX_RPATHS 16
#define MAX_MAPS 64

#define MH_MAGIC_64 0xfeedfacfU
#define MH_MAGIC_32 0xfeedfaceU
#define FAT_MAGIC 0xcafebabeU
#define FAT_MAGIC_64 0xcafebabfU
#define MH_EXECUTE 2U
#define MH_DYLIB 6U
#define MH_BUNDLE 8U

#define LC_SEGMENT_64 0x19U
#define LC_SYMTAB 0x02U
#define LC_UNIXTHREAD 0x05U
#define LC_LOAD_DYLIB 0x0cU
#define LC_ID_DYLIB 0x0dU
#define LC_LOAD_WEAK_DYLIB 0x80000018U
#define LC_REEXPORT_DYLIB 0x8000001fU
#define LC_LAZY_LOAD_DYLIB 0x20U
#define LC_LOAD_UPWARD_DYLIB 0x80000023U
#define LC_RPATH 0x8000001cU
#define LC_DYLD_INFO 0x22U
#define LC_DYLD_INFO_ONLY 0x80000022U
#define LC_DYLD_EXPORTS_TRIE 0x80000033U
#define LC_DYLD_CHAINED_FIXUPS 0x80000034U
#define LC_MAIN 0x80000028U

#define CPU_TYPE_X86_64 0x01000007U
#define X86_THREAD_STATE64 4U
#define CHAINED_PTR_64 2U
#define CHAINED_PTR_64_OFFSET 6U
#define CHAINED_IMPORT 1U
#define CHAINED_IMPORT_ADDEND 2U
#define CHAINED_IMPORT_ADDEND64 3U
#define CHAINED_SYMBOL_UNCOMPRESSED 0U
#define CHAINED_SYMBOL_COMPRESSED 1U
#define CHAINED_PTR_START_NONE 0xFFFFU

struct mach_header_64 {
    uint32_t magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved;
} __attribute__((packed));
struct load_command { uint32_t cmd, cmdsize; } __attribute__((packed));
struct segment_command_64 {
    uint32_t cmd, cmdsize; char segname[16];
    uint64_t vmaddr, vmsize, fileoff, filesize;
    uint32_t maxprot, initprot, nsects, flags;
} __attribute__((packed));
struct section_64 {
    char sectname[16], segname[16];
    uint64_t addr, size;
    uint32_t offset, align, reloff, nreloc, flags;
    uint32_t reserved1, reserved2, reserved3;
} __attribute__((packed));
struct entry_point_command { uint32_t cmd, cmdsize; uint64_t entryoff, stacksize; } __attribute__((packed));
struct thread_command { uint32_t cmd, cmdsize, flavor, count; } __attribute__((packed));
struct dylib_command {
    uint32_t cmd, cmdsize, name_off, timestamp, current_version, compat_version;
} __attribute__((packed));
struct dyld_info_command {
    uint32_t cmd, cmdsize, rebase_off, rebase_size, bind_off, bind_size;
    uint32_t weak_bind_off, weak_bind_size, lazy_bind_off, lazy_bind_size;
    uint32_t export_off, export_size;
} __attribute__((packed));
struct symtab_command {
    uint32_t cmd, cmdsize, symoff, nsyms, stroff, strsize;
} __attribute__((packed));
struct rpath_command { uint32_t cmd, cmdsize, path_off; } __attribute__((packed));
struct linkedit_data_command { uint32_t cmd, cmdsize, dataoff, datasize; } __attribute__((packed));
struct chained_fixups_command { uint32_t cmd, cmdsize, dataoff, datasize; } __attribute__((packed));
struct fat_arch { uint32_t cputype, cpusubtype, offset, size, align; } __attribute__((packed));
struct fat_arch_64 {
    uint32_t cputype, cpusubtype; uint64_t offset, size; uint32_t align, reserved;
} __attribute__((packed));
struct chained_header {
    uint32_t version, starts_off, imports_off, symbols_off;
    uint32_t imports_count, imports_format, symbols_format;
} __attribute__((packed));

struct export_ent {
    char *name;
    uint64_t addr;       /* unslid vmaddr for a regular export */
    int reexp_ord;       /* > 0 when this name re-exports from a dependency */
    char *reexp_name;
};

struct image {
    char *path;
    char *install;
    const uint8_t *file;
    size_t fsize;
    const uint8_t *slice;
    size_t slen;
    const struct mach_header_64 *mh;
    const struct segment_command_64 *segs[MAX_SEGS];
    int nsegs;
    int prot[MAX_SEGS];
    uint64_t base, span, slide;
    int is_main, fat;
    const char *arch;
    const uint8_t *entry_cmd;
    const struct entry_point_command *ep;
    const struct thread_command *tc;
    uint32_t rebase_off, rebase_size, bind_off, bind_size;
    uint32_t weak_off, weak_size, lazy_off, lazy_size;
    uint32_t exp_off, exp_size;
    uint32_t cf_off, cf_size;
    uint32_t symoff, nsyms, stroff, strsize;
    struct export_ent *exports;
    uint32_t nexports, expcap;
    uint32_t *exphash;
    size_t exphash_cap;
    char *depname[MAX_DYLIBS];
    int depweak[MAX_DYLIBS];
    int ndeps;
    struct image *dep[MAX_DYLIBS];
    char *rpath[MAX_RPATHS];
    int nrpaths;
    uint8_t *sympool;
    size_t sympool_len;
};

struct mapent { char *name; char *path; };

static struct image *g_imgs[MAX_IMAGES];
static int g_nimgs;
static struct image *g_main;
static const char *g_root;
static int g_load_mode, g_info_mode, g_strict, g_stub_missing, g_trace, g_trace_missing;
static uint64_t g_bound, g_unresolved, g_weak_missing, g_errors;
static int g_missing_images;
static struct mapent g_maps[MAX_MAPS];
static int g_nmaps;
static void (*g_objc_scan)(void *);

static char g_err[1024];

/* ---- clean-room host shim libraries (--shim-lib) ---- */
#define MAX_HOSTLIBS 8
static void *g_hostlibs[MAX_HOSTLIBS];
static const char *const *g_hostimports[MAX_HOSTLIBS];
static int g_nhostlibs;
/* Clean-room mode: the process runs against the plsr shim libraries
 * (--shim-lib) instead of the shipped Pulsar host bridge.  The loader core is
 * shared; the shipped-runtime bridge steps (reef host table, binary patches,
 * objc pre-realization, initializers) stay off so the clean-room shims keep
 * the exact behaviour they were validated with. */
static int g_cleanroom;

/* ---- host libc shim: the "plsr libSystem" replacement ---- */
static ssize_t shim_write(int fd, const void *b, size_t n) {
    ssize_t r = write(fd, b, n);
    if (g_trace)
        fprintf(stderr, "shim_write(fd=%d, buf=%p, n=%zu) = %zd (%s)\n",
                fd, b, n, r, r < 0 ? strerror(errno) : "ok");
    return r;
}
static ssize_t shim_read(int fd, void *b, size_t n) { return read(fd, b, n); }
static int shim_close(int fd) { return close(fd); }
static void shim_exit(int c) { _exit(c & 0xff); }
static void print_frame_map(void *addr);
static void shim_abort(void) {
    if (getenv("PLSR_REEF_TRACE") || getenv("PLSR_ABORT_STACK")) {
        void *frames[32];
        int n = backtrace(frames, 32);
        fprintf(stderr, "reef: abort backtrace (%d frames)\n", n);
        for (int i = 0; i < n; ++i) {
            fprintf(stderr, "  abort[%02d]=%p %s\n", i, frames[i],
                    plsr_describe_addr(frames[i]));
            print_frame_map(frames[i]);
        }
        if (getenv("PLSR_ABORT_SCAN")) {
            uintptr_t *stack = (uintptr_t *)__builtin_frame_address(0);
            for (int i = 2; stack && i < 1026; ++i) {
                uintptr_t value = stack[i];
                const char *where = plsr_describe_addr((void *)value);
                if (where && (strstr(where, "CotEditor") ||
                              strstr(where, "AppKit.framework")))
                    fprintf(stderr, "  abort-stack[%d]=%p %s\n",
                            i, (void *)value, where);
            }
        }
        if (getenv("PLSR_ABORT_STACK")) {
            uintptr_t *stack = (uintptr_t *)__builtin_frame_address(0);
            for (int i = 2; stack && i < 8192; ++i) {
                uintptr_t value = stack[i];
                const char *where = plsr_describe_addr((void *)value);
                if (!where || where[0] != ':') {
                    if (where && (strstr(where, "CotEditor") ||
                                  strstr(where, "AppKit.framework") ||
                                  strstr(where, "Foundation") ||
                                  strstr(where, "libswift")))
                        fprintf(stderr, "  abort-frame[%d]=%p %s\n",
                                i, (void *)value, where);
                }
            }
        }
    }
    _exit(134);
}
static void shim_bzero(void *p, size_t n) { memset(p, 0, n); }

/* CoreFoundation's __CFProcessPath asks libSystem for the executable path and
 * derives the main bundle from it.  The real answer here is the Mach-O image
 * (…/CotEditor.app/Contents/MacOS/CotEditor), not this loader: reporting the
 * loader makes +[NSBundle mainBundle] nil, which in turn makes AppKit skip
 * _AKLoadMainStoryboard, so the app never gets a delegate and never opens a
 * window. */
static char g_exec_path[PATH_MAX];
/* The libSystem adapter for stat() repacks a host struct into the caller's
 * Darwin layout; matching that intermediate layout is fragile, so bind the
 * stat family straight to shims that write Darwin's struct stat themselves.
 * CoreFoundation needs st_mode from this to accept CotEditor.app as a bundle
 * directory. */
static void darwin_fill_stat(const struct stat *st, unsigned char *b) {
    memset(b, 0, 144);
    *(uint32_t *)(b + 0) = (uint32_t)st->st_dev;
    *(uint16_t *)(b + 4) = (uint16_t)st->st_mode;
    *(uint16_t *)(b + 6) = (uint16_t)st->st_nlink;
    *(uint64_t *)(b + 8) = (uint64_t)st->st_ino;
    *(uint32_t *)(b + 16) = (uint32_t)st->st_uid;
    *(uint32_t *)(b + 20) = (uint32_t)st->st_gid;
    *(uint32_t *)(b + 24) = (uint32_t)st->st_rdev;
    *(int64_t *)(b + 32) = st->st_atim.tv_sec;
    *(int64_t *)(b + 40) = st->st_atim.tv_nsec;
    *(int64_t *)(b + 48) = st->st_mtim.tv_sec;
    *(int64_t *)(b + 56) = st->st_mtim.tv_nsec;
    *(int64_t *)(b + 64) = st->st_ctim.tv_sec;
    *(int64_t *)(b + 72) = st->st_ctim.tv_nsec;
    *(int64_t *)(b + 96) = st->st_size;
    *(int64_t *)(b + 104) = st->st_blocks;
    *(uint32_t *)(b + 112) = (uint32_t)st->st_blksize;
}
static int shim_stat(const char *path, void *buf) {
    if (getenv("PLSR_NO_STAT_SHIM")) return -1;
    struct stat st;
    if (!buf || stat(path, &st) != 0) {
        if (getenv("PLSR_HOST_TRACE"))
            fprintf(stderr, "reef-stat: stat('%s') failed errno=%d\n",
                    path ? path : "(null)", errno);
        return -1;
    }
    darwin_fill_stat(&st, buf);
    if (getenv("PLSR_HOST_TRACE"))
        fprintf(stderr, "reef-stat: stat('%s') mode=0%o\n",
                path ? path : "(null)", (unsigned)st.st_mode);
    return 0;
}
static int shim_lstat(const char *path, void *buf) {
    if (getenv("PLSR_NO_STAT_SHIM")) return -1;
    struct stat st;
    if (!buf || lstat(path, &st) != 0) return -1;
    darwin_fill_stat(&st, buf);
    return 0;
}
static int shim_fstat(int fd, void *buf) {
    if (getenv("PLSR_NO_STAT_SHIM")) return -1;
    struct stat st;
    if (!buf || fstat(fd, &st) != 0) return -1;
    darwin_fill_stat(&st, buf);
    return 0;
}
/* ---- optional application-delegate bootstrap ----
 * This runtime's AppKit installs the delegate from Main.storyboardc.  When the
 * bundle's resource lookup cannot produce that storyboard, the app launches
 * with a nil delegate and never opens a window.  With PLSR_BOOTSTRAP_DELEGATE=1
 * the loader replaces _AKLoadMainStoryboard (AppKit+0x1b7250) with a direct
 * install of the app's AppDelegate class. */
/* ---- AppKit missing-window recovery ----
 * When the app's storyboard fails to finish constructing a window, this
 * runtime fallback creates a real AppKit NSWindow and attaches a native host
 * surface.  It remains opt-in with PLSR_NSWINDOW until storyboard loading is
 * complete. */
struct nsw_rect { double x, y, w, h; };
#include "widget_spec.h"
/* Weak: the plsr-lite link variant of this source builds without the
 * Mayonnaise UI objects; unlinked weak symbols resolve to NULL and every
 * call site checks before use. */
extern int plsr_mayo_runtime_window_open(void *ns_window, const char *title,
                                         int width, int height, sw_node *tree,
                                         plsr_widget *widgets) __attribute__((weak));
extern int plsr_mayo_runtime_window_is_open(void *ns_window) __attribute__((weak));
extern void plsr_mayo_runtime_update_widgets(void *ns_window,
                                              plsr_widget *widgets) __attribute__((weak));
extern void plsr_mayo_runtime_update_title(void *ns_window,
                                            const char *title) __attribute__((weak));
extern void plsr_mayo_runtime_update_size(void *ns_window,
                                          double width, double height) __attribute__((weak));

void plsr_appkit_set_window_content_size(void *ns_window,
                                         double width, double height);

static void *(*g_nswindow_init_orig)(void *, void *, struct nsw_rect,
                                     unsigned long, unsigned long, signed char);
static void *plsr_nswindow_init_trace(void *self, void *cmd, struct nsw_rect rect,
                                      unsigned long style, unsigned long backing,
                                      signed char defer) {
    void *window = g_nswindow_init_orig
        ? g_nswindow_init_orig(self, cmd, rect, style, backing, defer) : NULL;
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: NSWindow init receiver=%p result=%p frame=%.0fx%.0f\n",
                self, window, rect.w, rect.h);
    return window;
}

static void plsr_trace_nswindow_init(void) {
    if (getenv("PLSR_NO_NSWINDOW_TRACE")) return;
    void *(*getcls)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_objc_getClass");
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    void *(*getimp)(void *, void *) =
        (void *(*)(void *, void *))plsr_image_symbol("_class_getMethodImplementation");
    int (*replace)(void *, void *, void *, const char *) =
        (int (*)(void *, void *, void *, const char *))
            plsr_image_symbol("_class_replaceMethod");
    if (!getcls || !sel || !getimp || !replace) return;
    void *cls = getcls("NSWindow");
    void *selector = sel("initWithContentRect:styleMask:backing:defer:");
    if (!cls || !selector) return;
    g_nswindow_init_orig = (void *(*)(void *, void *, struct nsw_rect,
                                      unsigned long, unsigned long, signed char))
        getimp(cls, selector);
    if (g_nswindow_init_orig)
        replace(cls, selector, (void *)plsr_nswindow_init_trace,
                "@@:{CGRect={CGPoint=dd}{CGSize=dd}}QQc");
}

static int plsr_attach_host_surface(void *window, double width, double height,
                                    const char *title) {
    void *(*host_fn)(void) =
        (void *(*)(void))plsr_image_symbol("___reefAppKitHost");
    void *(*getcls)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_objc_getClass");
    void *(*getivar)(void *, const char *) =
        (void *(*)(void *, const char *))plsr_image_symbol("_class_getInstanceVariable");
    ptrdiff_t (*ivar_offset)(void *) =
        (ptrdiff_t (*)(void *))plsr_image_symbol("_ivar_getOffset");
    if (!host_fn || !getcls || !getivar || !ivar_offset) return 0;
    void **host = host_fn();
    if (!host || !host[0x58 / sizeof(void *)]) return 0;
    void *(*create)(double, double, void *, void *, void *, unsigned, long) =
        (void *(*)(double, double, void *, void *, void *, unsigned, long))host[0x58 / sizeof(void *)];
    void *surface = create(width, height, NULL, NULL, NULL, 0, 0);
    if (!surface) return 0;
    void (*set_title)(void *, const char *) =
        (void (*)(void *, const char *))host[0x68 / sizeof(void *)];
    if (set_title && title) set_title(surface, title);

    void *iv = getivar(getcls("NSWindow"), "_host");
    ptrdiff_t off = iv ? ivar_offset(iv) : -1;
    if (off < 0) return 0;
    *(void **)((char *)window + off) = surface;
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: attached host surface %p to NSWindow %p (ivar +%td)\n",
                surface, window, off);
    return 1;
}

static void plsr_pump_appkit_host(int milliseconds) {
    struct image *appkit = NULL;
    for (int i = 0; i < g_nimgs; i++)
        if (strstr(g_imgs[i]->path, "AppKit.framework")) { appkit = g_imgs[i]; break; }
    if (!appkit || appkit->span <= 0x2e380) return;
    void (*dispatch_host)(void) =
        (void (*)(void))(uintptr_t)(appkit->base + appkit->slide + 0x2e380);
    for (int i = 0; i < milliseconds / 16; i++) {
        dispatch_host();
        usleep(16000);
    }
}

static uint64_t find_export_any(const char *name, struct image **prov);

/* _objc_getClass deadlocks in this runtime once the framework initializers have
 * been bypassed, so take the class object straight from the image export
 * tables (dyld does the same at bind time). */
static void *plsr_class_object(const char *name) {
    char sym[256];
    snprintf(sym, sizeof sym, "_OBJC_CLASS_$_%s", name);
    struct image *prov = NULL;
    uint64_t addr = find_export_any(sym, &prov);
    if (addr) return (void *)(uintptr_t)addr;
    void *(*getcls)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_objc_getClass");
    return getcls ? getcls(name) : NULL;
}

/* Build a clean-room SwiftUI tree from the NSWindow's loaded AppKit view
 * hierarchy.  The Mayonnaise runtime window paints this with sw_paint, so the
 * shipped app's own view classes, labels and nesting are what appears on the
 * surface rather than a separate hard-coded UI. */
static int g_swiftui_tree_nodes;

static sw_node *plsr_swiftui_view_node(void *view, int depth) {
    if (!view || depth > 6 || g_swiftui_tree_nodes >= 96) return NULL;
    const char *(*class_name)(const void *) =
        (const char *(*)(const void *))plsr_image_symbol("_object_getClassName");
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    void *(*m0)(void *, void *) =
        (void *(*)(void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*m1)(void *, void *, void *) =
        (void *(*)(void *, void *, void *))plsr_image_symbol("_objc_msgSend");
    if (!class_name || !sel || !m0 || !m1) return NULL;
    const char *cls = class_name(view);
    char text[512] = "";
    void *responds = sel("respondsToSelector:");
    if (m1(view, responds, sel("stringValue"))) {
        const char *u = plsr_nsutf8(m0(view, sel("stringValue")));
        if (u && *u) snprintf(text, sizeof text, "%.96s", u);
    }
    if (!text[0] && m1(view, responds, sel("title"))) {
        const char *u = plsr_nsutf8(m0(view, sel("title")));
        if (u && *u) snprintf(text, sizeof text, "%.96s", u);
    }
    if (!text[0] && m1(view, responds, sel("string"))) {
        const char *u = plsr_nsutf8(m0(view, sel("string")));
        if (u && *u) snprintf(text, sizeof text, "%.400s", u);
    }
    char label[640];
    if (text[0] && cls && strstr(cls, "TextView"))
        snprintf(label, sizeof label, "%s", text);
    else
        snprintf(label, sizeof label, "%s%s%s", cls ? cls : "NSView",
                 text[0] ? "  " : "", text);
    sw_node *node = sw_vstack(2);
    if (!node) return NULL;
    sw_add(node, sw_text(label));
    g_swiftui_tree_nodes++;
    void *subviews = m0(view, sel("subviews"));
    unsigned long count = subviews
        ? (unsigned long)(uintptr_t)m0(subviews, sel("count")) : 0;
    for (unsigned long i = 0; i < count && g_swiftui_tree_nodes < 96; i++) {
        void *child = m1(subviews, sel("objectAtIndex:"), (void *)(uintptr_t)i);
        sw_node *sub = plsr_swiftui_view_node(child, depth + 1);
        if (sub) sw_add(node, sub);
    }
    return node;
}

static sw_node *plsr_build_swiftui_tree(void *win, const char *title,
                                        int width, int height) {
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    void *(*m0)(void *, void *) =
        (void *(*)(void *, void *))plsr_image_symbol("_objc_msgSend");
    if (!win || !sel || !m0) return NULL;
    sw_node *root = sw_window(title && *title ? title : "Application",
                              width, height);
    if (!root) return NULL;
    g_swiftui_tree_nodes = 0;
    void *content = m0(win, sel("contentView"));
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: swiftui tree begin contentView=%p\n", content);
    sw_node *tree = content ? plsr_swiftui_view_node(content, 0) : NULL;
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: swiftui tree built=%p\n", (void *)tree);
    if (!tree) tree = sw_text("No AppKit content view");
    if (tree) sw_add(root, tree);
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: swiftui tree root=%p nodes=%d contentView=%p\n",
                (void *)root, g_swiftui_tree_nodes, content);
    return root;
}

/* ---- real control bridge ------------------------------------------------
 * The SwiftUI tree above only paints text.  For the presented window the same
 * AppKit views are described as controls the host can build and operate, and
 * every widget keeps the NSView it came from so input is delivered through
 * that view's own API (-performClick:, -setStringValue:). */

#define PLSR_WIDGET_MAX 1024
static int g_widget_nodes;
static void *g_widget_first_responder;
static struct { void *item; char title[256]; } g_nib_menu_titles[4096];
static unsigned int g_nib_menu_title_count;
static void plsr_capture_menu_shortcuts(void *app);
static void plsr_nib_remember_menu_title(void *item, const char *title) {
    if (!item || !title || !*title) return;
    for (unsigned int i = 0; i < g_nib_menu_title_count; ++i) {
        if (g_nib_menu_titles[i].item == item) {
            snprintf(g_nib_menu_titles[i].title, sizeof g_nib_menu_titles[i].title,
                     "%s", title);
            return;
        }
    }
    if (g_nib_menu_title_count >= sizeof g_nib_menu_titles / sizeof g_nib_menu_titles[0])
        return;
    g_nib_menu_titles[g_nib_menu_title_count].item = item;
    snprintf(g_nib_menu_titles[g_nib_menu_title_count].title,
             sizeof g_nib_menu_titles[g_nib_menu_title_count].title, "%s", title);
    g_nib_menu_title_count++;
}
static const char *plsr_nib_menu_title(void *item) {
    for (unsigned int i = 0; item && i < g_nib_menu_title_count; ++i)
        if (g_nib_menu_titles[i].item == item) return g_nib_menu_titles[i].title;
    return NULL;
}
static void plsr_install_text_store_for(void *object);
static void plsr_install_text_view_for(void *object);
static int plsr_text_store_enabled(void);
static void *plsr_mk_nsstring(const char *s);
static const char *plsr_text_get(void *owner);
static void *plsr_text_owner_of(void *self);
static void plsr_text_set(void *owner, const char *s);
static void *(*g_w_sel)(const char *);
static void *(*g_w_m0)(void *, void *);
static void *(*g_w_m1)(void *, void *, void *);
static signed char (*g_w_mb)(void *, void *, void *);
static const char *(*g_w_clsname)(const void *);
static char *plsr_widget_copy(const char *s);

void plsr_appkit_set_window_content_size(void *ns_window,
                                         double width, double height) {
    if (!ns_window || !g_w_sel || !isfinite(width) || !isfinite(height) ||
        width <= 0.0 || height <= 0.0) return;
    void *msg = plsr_image_symbol("_objc_msgSend");
    if (msg)
        ((void (*)(void *, void *, double, double))msg)(ns_window,
            g_w_sel("setContentSize:"), width, height);
}

static int plsr_widget_api(void) {
    if (g_w_m0) return 1;
    g_w_sel = (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    g_w_m0 = (void *(*)(void *, void *))plsr_image_symbol("_objc_msgSend");
    g_w_m1 = (void *(*)(void *, void *, void *))plsr_image_symbol("_objc_msgSend");
    g_w_mb = (signed char (*)(void *, void *, void *))plsr_image_symbol("_objc_msgSend");
    g_w_clsname = (const char *(*)(const void *))plsr_image_symbol("_object_getClassName");
    return g_w_sel && g_w_m0 && g_w_m1 && g_w_mb && g_w_clsname;
}

static int plsr_widget_responds(void *view, const char *name) {
    return view && g_w_mb &&
        g_w_mb(view, g_w_sel("respondsToSelector:"), g_w_sel(name));
}

static void plsr_widget_string(void *view, const char *name, char *out,
                               size_t cap) {
    if (cap) out[0] = '\0';
    if (!cap || !plsr_widget_responds(view, name)) return;
    const char *u = plsr_nsutf8(g_w_m0(view, g_w_sel(name)));
    if (u && *u) snprintf(out, cap, "%.*s", (int)(cap - 1), u);
}

static void plsr_widget_capture_icon(void *view, plsr_widget *w) {
    if (!view || !w || !plsr_widget_responds(view, "image")) return;
    void *image = g_w_m0(view, g_w_sel("image"));
    char name[128] = "";
    plsr_widget_string(image, "name", name, sizeof name);
    if (name[0]) w->glyph = plsr_widget_copy(name);
}

static plsr_widget *plsr_widget_new(plsr_widget_kind kind) {
    plsr_widget *w = calloc(1, sizeof *w);
    if (w) {
        w->kind = kind;
        w->enabled = 1;
        w->alpha = 1.0;
        w->maximum = 1.0;
    }
    return w;
}

static void plsr_widget_add(plsr_widget *parent, plsr_widget *child) {
    if (!parent || !child) return;
    if (parent->nkids == parent->cap) {
        size_t cap = parent->cap ? parent->cap * 2 : 4;
        plsr_widget **kids = realloc(parent->kids, cap * sizeof *kids);
        if (!kids) return;
        parent->kids = kids;
        parent->cap = cap;
    }
    parent->kids[parent->nkids++] = child;
}

static char *plsr_widget_copy(const char *s) {
    return (s && *s) ? strdup(s) : NULL;
}

static void plsr_widget_capture_view(void *view, plsr_widget *w) {
    void *msg = plsr_image_symbol("_objc_msgSend");
    void (*stret)(struct nsw_rect *, void *, void *) =
        (void (*)(struct nsw_rect *, void *, void *))
            plsr_image_symbol("_objc_msgSend_stret");
    if (!view || !w || !g_w_sel || !msg) return;
    w->focused = view == g_widget_first_responder;
    struct nsw_rect r = {0};
    if (stret && plsr_widget_responds(view, "frame"))
        stret(&r, view, g_w_sel("frame"));
    if (plsr_widget_responds(view, "frame") &&
        isfinite(r.x) && isfinite(r.y) && isfinite(r.w) && isfinite(r.h) &&
        r.w >= 0.0 && r.h >= 0.0) {
        w->x = r.x; w->y = r.y; w->width = r.w; w->height = r.h;
        w->has_frame = 1;
    }
    if (plsr_widget_responds(view, "isHidden"))
        w->hidden = ((signed char (*)(void *, void *))msg)(
            view, g_w_sel("isHidden")) != 0;
    if (plsr_widget_responds(view, "isEnabled"))
        w->enabled = ((signed char (*)(void *, void *))msg)(
            view, g_w_sel("isEnabled")) != 0;
    if (plsr_widget_responds(view, "alphaValue"))
        w->alpha = ((double (*)(void *, void *))msg)(
            view, g_w_sel("alphaValue"));
    if (plsr_widget_responds(view, "isFlipped"))
        w->flipped = ((signed char (*)(void *, void *))msg)(
            view, g_w_sel("isFlipped")) != 0;
}

static void plsr_widget_refresh_text(plsr_widget *w,
                                     const char *const *selectors,
                                     size_t selector_count) {
    if (!w || !w->view) return;
    char *value = NULL;
    int observed = 0;
    for (size_t i = 0; i < selector_count; i++) {
        if (!plsr_widget_responds(w->view, selectors[i])) continue;
        observed = 1;
        void *object = g_w_m0(w->view, g_w_sel(selectors[i]));
        const char *utf8 = plsr_nsutf8(object);
        if (utf8) {
            value = strdup(utf8);
            break;
        }
    }
    if (!observed) return;
    const char *old_value = w->text ? w->text : "";
    const char *new_value = value ? value : "";
    if (strcmp(old_value, new_value) == 0) {
        free(value);
        return;
    }
    free(w->text);
    w->text = value && *value ? value : NULL;
    if (!w->text) free(value);
}

static double plsr_widget_number(void *view, const char *selector,
                                 double fallback);
static intptr_t plsr_widget_integer(void *view, const char *selector,
                                    intptr_t fallback);
static void plsr_widget_sync_menu_items(void *menu, plsr_widget *owner);

static void plsr_widget_free_node(plsr_widget *w) {
    if (!w) return;
    for (size_t i = 0; i < w->nkids; ++i)
        plsr_widget_free_node(w->kids[i]);
    free(w->text);
    free(w->detail);
    free(w->glyph);
    free(w->kids);
    free(w);
    if (g_widget_nodes > 0) g_widget_nodes--;
}

static void plsr_widget_refresh_node(plsr_widget *w) {
    if (!w) return;
    if (w->view) {
        plsr_widget_capture_view(w->view, w);
        static const char *const text_value[] = {
            "stringValue", "title", "string", "label", "accessibilityLabel"
        };
        static const char *const editor_value[] = {"string", "stringValue"};
        switch (w->kind) {
            case PLSR_WIDGET_EDITOR:
                plsr_widget_refresh_text(w, editor_value,
                    sizeof editor_value / sizeof editor_value[0]);
                break;
            case PLSR_WIDGET_LABEL:
            case PLSR_WIDGET_BUTTON:
            case PLSR_WIDGET_TEXTFIELD:
            case PLSR_WIDGET_MENU_BUTTON:
                plsr_widget_refresh_text(w, text_value,
                    sizeof text_value / sizeof text_value[0]);
                break;
            default:
                break;
        }
        if (plsr_widget_responds(w->view, "state"))
            w->selected = plsr_widget_integer(w->view, "state", w->selected) != 0;
        if (w->kind == PLSR_WIDGET_TOGGLE ||
            w->kind == PLSR_WIDGET_CHECKBOX)
            w->selected = plsr_widget_integer(w->view, "state", w->selected) != 0;
        else if (w->kind == PLSR_WIDGET_SEGMENTED)
            w->value = (double)plsr_widget_integer(w->view, "selectedSegment",
                                                    (intptr_t)w->value);
        else if (w->kind == PLSR_WIDGET_MENU_BUTTON)
            w->value = (double)plsr_widget_integer(w->view,
                "indexOfSelectedItem", (intptr_t)w->value);
        else if (w->kind == PLSR_WIDGET_SLIDER ||
                 w->kind == PLSR_WIDGET_PROGRESS)
            w->value = plsr_widget_number(w->view, "doubleValue", w->value);
        if (w->kind == PLSR_WIDGET_MENU_BUTTON) {
            void *menu = plsr_widget_responds(w->view, "submenu")
                ? g_w_m0(w->view, g_w_sel("submenu"))
                : plsr_widget_responds(w->view, "menu")
                    ? g_w_m0(w->view, g_w_sel("menu")) : NULL;
            if (menu) plsr_widget_sync_menu_items(menu, w);
        }
    }
    for (size_t i = 0; i < w->nkids; i++)
        plsr_widget_refresh_node(w->kids[i]);
}

static double plsr_widget_number(void *view, const char *selector,
                                double fallback) {
    void *msg = plsr_image_symbol("_objc_msgSend");
    return view && msg && g_w_sel
        ? ((double (*)(void *, void *))msg)(view, g_w_sel(selector))
        : fallback;
}

static intptr_t plsr_widget_integer(void *view, const char *selector,
                                    intptr_t fallback) {
    void *msg = plsr_image_symbol("_objc_msgSend");
    return view && msg && g_w_sel && plsr_widget_responds(view, selector)
        ? ((intptr_t (*)(void *, void *))msg)(view, g_w_sel(selector))
        : fallback;
}

/* AppKit menus and segmented controls keep their children in model objects,
 * not in -subviews. Copy those public model APIs into the bridge tree too. */
static void plsr_widget_sync_menu_items(void *menu, plsr_widget *owner) {
    if (!menu || !owner || !plsr_widget_responds(menu, "itemArray")) return;
    void *items = g_w_m0(menu, g_w_sel("itemArray"));
    unsigned long count = items
        ? (unsigned long)(uintptr_t)g_w_m0(items, g_w_sel("count")) : 0;
    size_t child_index = 0;
    int same_structure = 1;
    for (unsigned long i = 0; i < count; ++i) {
        void *item = g_w_m1(items, g_w_sel("objectAtIndex:"),
                            (void *)(uintptr_t)i);
        char title[256] = "";
        plsr_widget_string(item, "title", title, sizeof title);
        if (!title[0] && plsr_nib_menu_title(item)) title[0] = 1;
        int separator = plsr_widget_responds(item, "isSeparatorItem") &&
            (int)(intptr_t)g_w_m0(item, g_w_sel("isSeparatorItem"));
        if (!title[0] && !separator) continue;
        if (child_index >= owner->nkids ||
            owner->kids[child_index]->view != item ||
            owner->kids[child_index]->kind != (separator
                ? PLSR_WIDGET_SEPARATOR : PLSR_WIDGET_LABEL)) {
            same_structure = 0;
            break;
        }
        child_index++;
    }
    if (same_structure && child_index == owner->nkids) return;
    for (size_t i = 0; i < owner->nkids; ++i)
        plsr_widget_free_node(owner->kids[i]);
    owner->nkids = 0;
    for (unsigned long i = 0; i < count && g_widget_nodes < PLSR_WIDGET_MAX; i++) {
        void *item = g_w_m1(items, g_w_sel("objectAtIndex:"),
                            (void *)(uintptr_t)i);
        char title[256] = "";
        plsr_widget_string(item, "title", title, sizeof title);
        if (!title[0]) {
            const char *archived_title = plsr_nib_menu_title(item);
            if (archived_title) snprintf(title, sizeof title, "%s", archived_title);
        }
        int separator = plsr_widget_responds(item, "isSeparatorItem") &&
            (int)(intptr_t)g_w_m0(item, g_w_sel("isSeparatorItem"));
        if (!title[0] && !separator) continue;
        plsr_widget *option = plsr_widget_new(separator ? PLSR_WIDGET_SEPARATOR
                                                       : PLSR_WIDGET_LABEL);
        if (!option) continue;
        option->text = plsr_widget_copy(title);
        option->view = item;
        option->enabled = (int)(intptr_t)g_w_m0(item, g_w_sel("isEnabled"));
        option->selected = (int)(intptr_t)g_w_m0(item, g_w_sel("state")) != 0;
        plsr_widget_capture_icon(item, option);
        g_widget_nodes++;
        plsr_widget_add(owner, option);
    }
}

/* NSApplication's main menu is a real source-owned AppKit object, but a
 * Linux window has no AppKit menu bar to display it. Project its top-level
 * NSMenuItems and their real submenu items into the host's menu controls. */
static plsr_widget *plsr_widget_build_main_menu(void) {
    void *app_class = plsr_class_object("NSApplication");
    void *app = app_class
        ? g_w_m0(app_class, g_w_sel("sharedApplication")) : NULL;
    void *menu = app && plsr_widget_responds(app, "mainMenu")
        ? g_w_m0(app, g_w_sel("mainMenu")) : NULL;
    void *items = menu && plsr_widget_responds(menu, "itemArray")
        ? g_w_m0(menu, g_w_sel("itemArray")) : NULL;
    unsigned long count = items
        ? (unsigned long)(uintptr_t)g_w_m0(items, g_w_sel("count")) : 0;
    plsr_widget *bar = NULL;
    for (unsigned long i = 0; i < count && g_widget_nodes < PLSR_WIDGET_MAX; i++) {
        void *item = g_w_m1(items, g_w_sel("objectAtIndex:"),
                            (void *)(uintptr_t)i);
        void *submenu = item && plsr_widget_responds(item, "submenu")
            ? g_w_m0(item, g_w_sel("submenu")) : NULL;
        if (!item || !submenu) continue;
        char title[256] = "";
        plsr_widget_string(item, "title", title, sizeof title);
        if (!title[0]) {
            const char *archived = plsr_nib_menu_title(item);
            if (archived) snprintf(title, sizeof title, "%s", archived);
        }
        if (!title[0]) continue;
        plsr_widget *entry = plsr_widget_new(PLSR_WIDGET_MENU_BUTTON);
        if (!entry) continue;
        entry->text = plsr_widget_copy(title);
        entry->view = item;
        entry->enabled = !plsr_widget_responds(item, "isEnabled") ||
            (int)(intptr_t)g_w_m0(item, g_w_sel("isEnabled"));
        plsr_widget_sync_menu_items(submenu, entry);
        if (!entry->nkids) continue;
        if (!bar) bar = plsr_widget_new(PLSR_WIDGET_CONTAINER_H);
        if (!bar) continue;
        g_widget_nodes++;
        plsr_widget_add(bar, entry);
    }
    return bar;
}

static void plsr_widget_add_segment_labels(void *view, plsr_widget *owner) {
    if (!view || !owner || !plsr_widget_responds(view, "segmentCount") ||
        !plsr_widget_responds(view, "labelForSegment:")) return;
    unsigned long count = (unsigned long)(intptr_t)
        g_w_m0(view, g_w_sel("segmentCount"));
    for (unsigned long i = 0; i < count && g_widget_nodes < PLSR_WIDGET_MAX; i++) {
        void *label = g_w_m1(view, g_w_sel("labelForSegment:"),
                             (void *)(uintptr_t)i);
        const char *utf8 = plsr_nsutf8(label);
        if (!utf8 || !*utf8) continue;
        plsr_widget *option = plsr_widget_new(PLSR_WIDGET_LABEL);
        if (!option) continue;
        option->text = plsr_widget_copy(utf8);
        g_widget_nodes++;
        plsr_widget_add(owner, option);
    }
}

static plsr_widget *plsr_widget_build_view(void *view, int depth) {
    if (!view || depth > 8 || g_widget_nodes >= PLSR_WIDGET_MAX) return NULL;
    const char *cls = g_w_clsname(view);
    if (!cls) return NULL;

    char text[512] = "", placeholder[256] = "";
    plsr_widget_string(view, "stringValue", text, sizeof text);
    if (!text[0]) plsr_widget_string(view, "title", text, sizeof text);
    if (!text[0]) plsr_widget_string(view, "string", text, sizeof text);
    if (!text[0]) plsr_widget_string(view, "label", text, sizeof text);
    if (!text[0]) plsr_widget_string(view, "accessibilityLabel", text, sizeof text);
    plsr_widget_string(view, "placeholderString", placeholder, sizeof placeholder);

    if (strstr(cls, "ProgressIndicator") || strstr(cls, "ActivityIndicator")) {
        plsr_widget *w = plsr_widget_new(PLSR_WIDGET_PROGRESS);
        if (!w) return NULL;
        w->view = view;
        w->value = plsr_widget_number(view, "doubleValue", 0.0);
        w->minimum = plsr_widget_number(view, "minValue", 0.0);
        w->maximum = plsr_widget_number(view, "maxValue", 1.0);
        g_widget_nodes++;
        plsr_widget_capture_view(view, w);
        return w;
    }
    if (strstr(cls, "Slider") || strstr(cls, "LevelIndicator")) {
        plsr_widget *w = plsr_widget_new(PLSR_WIDGET_SLIDER);
        if (!w) return NULL;
        w->view = view;
        w->value = plsr_widget_number(view, "doubleValue", 0.5);
        w->minimum = plsr_widget_number(view, "minValue", 0.0);
        w->maximum = plsr_widget_number(view, "maxValue", 1.0);
        g_widget_nodes++;
        plsr_widget_capture_view(view, w);
        return w;
    }
    if (strstr(cls, "SegmentedControl")) {
        plsr_widget *w = plsr_widget_new(PLSR_WIDGET_SEGMENTED);
        if (!w) return NULL;
        w->text = plsr_widget_copy(text);
        w->view = view;
        w->value = (double)plsr_widget_integer(view, "selectedSegment", 0);
        g_widget_nodes++;
        plsr_widget_capture_view(view, w);
        plsr_widget_add_segment_labels(view, w);
        return w;
    }
    if (strstr(cls, "Switch") || strstr(cls, "Toggle")) {
        plsr_widget *w = plsr_widget_new(PLSR_WIDGET_TOGGLE);
        if (!w) return NULL;
        w->text = plsr_widget_copy(text);
        w->view = view;
        w->selected = plsr_widget_integer(view, "state", 0) != 0;
        g_widget_nodes++;
        plsr_widget_capture_view(view, w);
        return w;
    }
    if (strstr(cls, "ImageView")) {
        plsr_widget *w = plsr_widget_new(PLSR_WIDGET_IMAGE);
        if (!w) return NULL;
        w->text = plsr_widget_copy(text);
        w->view = view;
        g_widget_nodes++;
        plsr_widget_capture_view(view, w);
        return w;
    }
    if (strstr(cls, "Separator") || strstr(cls, "Divider")) {
        plsr_widget *w = plsr_widget_new(PLSR_WIDGET_SEPARATOR);
        if (!w) return NULL;
        w->view = view;
        g_widget_nodes++;
        plsr_widget_capture_view(view, w);
        return w;
    }
    if (strstr(cls, "PopUpButton") || strstr(cls, "PopupButton") ||
        strstr(cls, "ComboBox")) {
        plsr_widget *w = plsr_widget_new(PLSR_WIDGET_MENU_BUTTON);
        if (!w) return NULL;
        w->text = plsr_widget_copy(text);
        w->detail = plsr_widget_copy("picker");
        w->view = view;
        w->value = (double)plsr_widget_integer(view, "indexOfSelectedItem", 0);
        g_widget_nodes++;
        plsr_widget_capture_view(view, w);
        void *menu = g_w_m0(view, g_w_sel("menu"));
        plsr_widget_sync_menu_items(menu, w);
        return w;
    }
    if (strstr(cls, "Button") && plsr_widget_responds(view, "menu")) {
        void *menu = g_w_m0(view, g_w_sel("menu"));
        if (menu) {
            plsr_widget *w = plsr_widget_new(PLSR_WIDGET_MENU_BUTTON);
            if (!w) return NULL;
            w->text = plsr_widget_copy(text);
            w->view = view;
            w->value = (double)plsr_widget_integer(view, "indexOfSelectedItem", 0);
            g_widget_nodes++;
            plsr_widget_capture_view(view, w);
            plsr_widget_sync_menu_items(menu, w);
            return w;
        }
    }
    if (strstr(cls, "Button") || strstr(cls, "SegmentedControl")) {
        double button_type = (double)plsr_widget_integer(view, "buttonType", 0);
        int toggle = strstr(cls, "CheckBox") || strstr(cls, "Radio") ||
                     button_type == 3.0 || button_type == 4.0;
        if (!text[0] && !toggle) return NULL;
        plsr_widget *w = plsr_widget_new(toggle ? PLSR_WIDGET_CHECKBOX
                                                 : PLSR_WIDGET_BUTTON);
        if (!w) return NULL;
        w->text = plsr_widget_copy(text);
        w->view = view;
        w->selected = plsr_widget_integer(view, "state", 0) != 0;
        g_widget_nodes++;
        plsr_widget_capture_view(view, w);
        return w;
    }
    int editable = plsr_widget_responds(view, "isEditable")
        ? (int)(intptr_t)g_w_m0(view, g_w_sel("isEditable")) : 0;
    if (strstr(cls, "TextField") || strstr(cls, "SearchField") ||
        strstr(cls, "ComboBox") || strstr(cls, "TokenField")) {
        plsr_widget *w = plsr_widget_new(editable ? PLSR_WIDGET_TEXTFIELD
                                                  : PLSR_WIDGET_LABEL);
        if (!w) return NULL;
        w->text = plsr_widget_copy(text);
        if (editable) {
            w->detail = plsr_widget_copy(placeholder);
            w->view = view;   /* only editable fields send text back */
        }
        g_widget_nodes++;
        plsr_widget_capture_view(view, w);
        return w;
    }
    if (strstr(cls, "TextView")) {
        /* An empty document is still a real editable control. */
        plsr_widget *w = plsr_widget_new(PLSR_WIDGET_EDITOR);
        if (!w) return NULL;
        w->text = plsr_widget_copy(text);
        w->view = view;
        g_widget_nodes++;
        plsr_widget_capture_view(view, w);
        return w;
    }

    /* Containers: split/stack views lay out horizontally, everything else
     * stacks vertically.  Chrome with neither text nor children is dropped. */
    int list = strstr(cls, "TableView") || strstr(cls, "OutlineView") ||
               strstr(cls, "CollectionView") || strstr(cls, "ListView");
    int horizontal = strstr(cls, "SplitView") || strstr(cls, "StackView") ||
                     strstr(cls, "Toolbar");
    plsr_widget *node = plsr_widget_new(list ? PLSR_WIDGET_LIST
        : horizontal ? PLSR_WIDGET_CONTAINER_H : PLSR_WIDGET_CONTAINER_V);
    if (!node) return NULL;
    node->view = view;
    g_widget_nodes++;
    plsr_widget_capture_view(view, node);
    if (text[0] && !strstr(cls, "ScrollView") && !strstr(cls, "ClipView") &&
        !strstr(cls, "ContentView") && strcmp(cls, "NSView") != 0) {
        plsr_widget *label = plsr_widget_new(PLSR_WIDGET_LABEL);
        if (label) {
            label->text = plsr_widget_copy(text);
            label->view = view;
            plsr_widget_add(node, label);
        }
    }
    void *subviews = g_w_m0(view, g_w_sel("subviews"));
    unsigned long count = subviews
        ? (unsigned long)(uintptr_t)g_w_m0(subviews, g_w_sel("count")) : 0;
    for (unsigned long i = 0; i < count && g_widget_nodes < PLSR_WIDGET_MAX; i++) {
        void *child = g_w_m1(subviews, g_w_sel("objectAtIndex:"),
                             (void *)(uintptr_t)i);
        plsr_widget *sub = plsr_widget_build_view(child, depth + 1);
        if (sub) plsr_widget_add(node, sub);
    }
    if (!node->nkids) {
        if (!text[0] || strstr(cls, "ScrollView") || strstr(cls, "ClipView") ||
            strcmp(cls, "NSView") == 0) {
            free(node);
            g_widget_nodes--;
            return NULL;
        }
    }
    return node;
}

static plsr_widget *plsr_widget_find(const plsr_widget *w, plsr_widget_kind kind) {
    if (!w) return NULL;
    if (w->kind == kind && w->view) return (plsr_widget *)w;
    for (size_t i = 0; i < w->nkids; i++)
        if (plsr_widget *hit = plsr_widget_find(w->kids[i], kind)) return hit;
    return NULL;
}

static void plsr_widget_tally(const plsr_widget *w, int *kinds) {
    if (!w) return;
    if ((int)w->kind >= 0 && (int)w->kind < PLSR_WIDGET_KIND_COUNT)
        kinds[w->kind]++;
    for (size_t i = 0; i < w->nkids; i++) plsr_widget_tally(w->kids[i], kinds);
}

/* Dump the actual AppKit hierarchy and NSView geometry on demand. This is
 * evidence for the SwiftUI-shaped reconstruction; the lossy widget spec by
 * itself cannot recover layout, hidden state, or control styling. */
static void plsr_trace_appkit_view(void *view, int depth, int *seen) {
    if (!view || !seen || depth > 12 || *seen >= 512) return;
    (*seen)++;
    const char *cls = g_w_clsname ? g_w_clsname(view) : NULL;
    char text[160] = "";
    plsr_widget_string(view, "stringValue", text, sizeof text);
    if (!text[0]) plsr_widget_string(view, "title", text, sizeof text);
    if (!text[0]) plsr_widget_string(view, "string", text, sizeof text);
    struct nsw_rect frame = {0};
    void *msg_send = plsr_image_symbol("_objc_msgSend");
    void (*msg_send_stret)(struct nsw_rect *, void *, void *) =
        (void (*)(struct nsw_rect *, void *, void *))
            plsr_image_symbol("_objc_msgSend_stret");
    if (msg_send_stret && g_w_sel)
        msg_send_stret(&frame, view, g_w_sel("frame"));
    void *subviews = g_w_m0 ? g_w_m0(view, g_w_sel("subviews")) : NULL;
    unsigned long count = subviews
        ? (unsigned long)(uintptr_t)g_w_m0(subviews, g_w_sel("count")) : 0;
    signed char hidden = -1;
    double alpha = -1.0;
    if (msg_send && g_w_sel) {
        hidden = ((signed char (*)(void *, void *))msg_send)(
            view, g_w_sel("isHidden"));
        alpha = ((double (*)(void *, void *))msg_send)(
            view, g_w_sel("alphaValue"));
    }
    fprintf(stderr,
            "reef-view: depth=%d class=%s frame=%.1f,%.1f %.1fx%.1f "
            "hidden=%d alpha=%.2f text=%.120s children=%lu\n",
            depth, cls ? cls : "?", frame.x, frame.y, frame.w, frame.h,
            (int)hidden, alpha, text, count);
    for (unsigned long i = 0; i < count && *seen < 512; i++) {
        void *child = g_w_m1(subviews, g_w_sel("objectAtIndex:"),
                             (void *)(uintptr_t)i);
        plsr_trace_appkit_view(child, depth + 1, seen);
    }
}

struct plsr_widget_window_spec {
    void *window;
    plsr_widget *spec;
};
static struct plsr_widget_window_spec *g_widget_windows;
static size_t g_widget_window_count;
static size_t g_widget_window_capacity;

plsr_widget *plsr_widget_spec_for_window(void *ns_window) {
    if (!ns_window || !plsr_widget_api()) return NULL;
    for (size_t i = 0; i < g_widget_window_count; i++)
        if (g_widget_windows[i].window == ns_window)
            return g_widget_windows[i].spec;
    if (g_widget_window_count == g_widget_window_capacity) {
        size_t capacity = g_widget_window_capacity
            ? g_widget_window_capacity * 2 : 8;
        struct plsr_widget_window_spec *windows = realloc(
            g_widget_windows, capacity * sizeof *windows);
        if (!windows) return NULL;
        g_widget_windows = windows;
        g_widget_window_capacity = capacity;
    }
    void *content = g_w_m0(ns_window, g_w_sel("contentView"));
    if (!content) return NULL;
    g_widget_first_responder = plsr_widget_responds(ns_window, "firstResponder")
        ? g_w_m0(ns_window, g_w_sel("firstResponder")) : NULL;
    if (getenv("PLSR_VIEW_TRACE")) {
        int seen = 0;
        plsr_trace_appkit_view(content, 0, &seen);
    }
    g_widget_nodes = 0;
    plsr_widget *spec = plsr_widget_build_view(content, 0);
    plsr_widget *menu_bar = plsr_widget_build_main_menu();
    /* Toolbar items are real NSButtons outside the content view; expose them
     * as controls above the content instead of hiding them. */
    void *toolbar = g_w_m0(ns_window, g_w_sel("toolbar"));
    plsr_widget *bar = NULL;
    if (toolbar) {
        void *items = g_w_m0(toolbar, g_w_sel("items"));
        unsigned long nitems = items
            ? (unsigned long)(uintptr_t)g_w_m0(items, g_w_sel("count")) : 0;
        for (unsigned long i = 0; i < nitems && g_widget_nodes < PLSR_WIDGET_MAX; i++) {
            void *item = g_w_m1(items, g_w_sel("objectAtIndex:"),
                                (void *)(uintptr_t)i);
            void *view = item ? g_w_m0(item, g_w_sel("view")) : NULL;
            plsr_widget *control = view ? plsr_widget_build_view(view, 1) : NULL;
            if (!control && item) {
                /* Standard items carry no view: the item itself is the
                 * control, and its label plus target/action is its API. */
                char label[128] = "";
                plsr_widget_string(item, "label", label, sizeof label);
                if (!label[0]) plsr_widget_string(item, "paletteLabel", label,
                                                 sizeof label);
                if (label[0]) {
                    control = plsr_widget_new(PLSR_WIDGET_BUTTON);
                    if (control) {
                        control->text = plsr_widget_copy(label);
                        control->view = item;
                        plsr_widget_capture_icon(item, control);
                    }
                }
            }
            if (!control) continue;
            if (!bar) bar = plsr_widget_new(PLSR_WIDGET_CONTAINER_H);
            if (bar) plsr_widget_add(bar, control);
        }
    }
    if ((menu_bar || bar) && spec) {
        plsr_widget *root = plsr_widget_new(PLSR_WIDGET_CONTAINER_V);
        if (root) {
            root->has_frame = 1;
            root->width = spec->width;
            root->height = spec->height;
            root->flipped = 1;
            double top = 0.0;
            if (menu_bar) {
                menu_bar->x = 0.0;
                menu_bar->y = top;
                menu_bar->width = root->width;
                menu_bar->height = 30.0;
                menu_bar->has_frame = 1;
                menu_bar->flipped = 1;
                plsr_widget_add(root, menu_bar);
                top += menu_bar->height;
            }
            if (bar) {
                bar->x = 0.0;
                bar->y = top;
                bar->width = root->width;
                if (bar->height <= 0.0) bar->height = 36.0;
                bar->has_frame = 1;
                bar->flipped = 1;
                plsr_widget_add(root, bar);
                top += bar->height;
            }
            spec->x = 0.0;
            spec->y = top;
            spec->height = fmax(0.0, root->height - top);
            spec->has_frame = 1;
            spec->flipped = 1;
            plsr_widget_add(root, spec);
            spec = root;
        }
    } else if (menu_bar || bar) {
        /* The window has no AppKit content projection; still surface the
         * real source menu/toolbar models in the host window. */
        spec = menu_bar ? menu_bar : bar;
    }
    g_widget_windows[g_widget_window_count].window = ns_window;
    g_widget_windows[g_widget_window_count].spec = spec;
    g_widget_window_count++;
    if (getenv("PLSR_REEF_TRACE")) {
        int kinds[PLSR_WIDGET_KIND_COUNT] = {0};
        plsr_widget_tally(spec, kinds);
        void *tb = g_w_m0(ns_window, g_w_sel("toolbar"));
        void *ti = tb ? g_w_m0(tb, g_w_sel("items")) : NULL;
        unsigned long nitems = ti
            ? (unsigned long)(uintptr_t)g_w_m0(ti, g_w_sel("count")) : 0;
        fprintf(stderr,
                "reef: widget spec window=%p content=%p v=%d h=%d labels=%d "
                "buttons=%d fields=%d editors=%d checks=%d toggles=%d "
                "sliders=%d progress=%d segmented=%d images=%d separators=%d "
                "menus=%d lists=%d toolbar=%p items=%lu\n",
                ns_window, content, kinds[0], kinds[1], kinds[2], kinds[3],
                kinds[4], kinds[5], kinds[6], kinds[7], kinds[8], kinds[9],
                kinds[10], kinds[11], kinds[12], kinds[13], kinds[14],
                tb, nitems);
    }
    g_widget_first_responder = NULL;
    if (getenv("PLSR_WIDGET_SELFTEST")) {
        plsr_widget *button = plsr_widget_find(spec, PLSR_WIDGET_BUTTON);
        if (button) {
            fprintf(stderr, "reef: widget selftest click view=%p title=%s\n",
                    button->view, button->text ? button->text : "");
            plsr_widget_action(button->view, PLSR_WIDGET_ACTION_CLICK, NULL);
        } else {
            fprintf(stderr, "reef: widget selftest found no button\n");
        }
        plsr_widget *editor = plsr_widget_find(spec, PLSR_WIDGET_EDITOR);
        if (editor) {
            fprintf(stderr, "reef: widget selftest text view=%p\n", editor->view);
            plsr_widget_action(editor->view, PLSR_WIDGET_ACTION_TEXT, "CotEditor");
            plsr_widget_drain_actions();
            void *storage = g_w_m0(editor->view, g_w_sel("textStorage"));
            if (storage && plsr_text_store_enabled())
                plsr_install_text_store_for(storage);
            char s1[128] = "", s2[128] = "", s3[128] = "";
            plsr_widget_string(editor->view, "string", s1, sizeof s1);
            plsr_widget_string(editor->view, "stringValue", s2, sizeof s2);
            if (storage) plsr_widget_string(storage, "string", s3, sizeof s3);
            char s4[128] = "";
            if (storage) {
                void *ns = plsr_class_object("NSString");
                void *v = ns ? g_w_m1(ns, g_w_sel("stringWithUTF8String:"),
                                      (void *)"StorageText") : NULL;
                if (v) g_w_m1(storage, g_w_sel("setString:"), v);
                plsr_widget_string(storage, "string", s4, sizeof s4);
            }
            const char *rt = plsr_nsutf8(plsr_mk_nsstring("roundtrip"));
            fprintf(stderr, "reef: nsstring roundtrip='%s'\n",
                    rt ? rt : "(null)");
            if (editor->view)
                fprintf(stderr, "reef: editor store='%s'\n",
                        plsr_text_get(plsr_text_owner_of(editor->view)));
            fprintf(stderr,
                    "reef: widget probe cls=%s storage=%p tv.string='%s' "
                    "stringValue='%s' storage.string='%s' storage.set->'%s' "
                    "NSToolbar=%p NSButton=%p NSTextField=%p\n",
                    g_w_clsname(editor->view), storage, s1, s2, s3, s4,
                    plsr_class_object("NSToolbar"), plsr_class_object("NSButton"),
                    plsr_class_object("NSTextField"));
        }
    }
    return spec;
}

static void plsr_widget_refresh_window_specs(void) {
    if (!plsr_mayo_runtime_window_is_open) return;
    for (size_t i = 0; i < g_widget_window_count; i++) {
        if (!plsr_mayo_runtime_window_is_open(g_widget_windows[i].window))
            continue;
        plsr_widget *spec = g_widget_windows[i].spec;
        void *title = g_w_m0(g_widget_windows[i].window, g_w_sel("title"));
        plsr_mayo_runtime_update_title(g_widget_windows[i].window,
                                       plsr_nsutf8(title));
        void *content = g_w_m0(g_widget_windows[i].window,
                               g_w_sel("contentView"));
        if (content && plsr_widget_responds(content, "frame")) {
            struct nsw_rect frame = {0};
            void (*stret)(struct nsw_rect *, void *, void *) =
                (void (*)(struct nsw_rect *, void *, void *))
                    plsr_image_symbol("_objc_msgSend_stret");
            if (stret) {
                stret(&frame, content, g_w_sel("frame"));
                if (isfinite(frame.w) && isfinite(frame.h) &&
                    frame.w > 0.0 && frame.h > 0.0)
                    plsr_mayo_runtime_update_size(g_widget_windows[i].window,
                                                   frame.w, frame.h);
            }
        }
        plsr_widget_refresh_node(spec);
        plsr_mayo_runtime_update_widgets(g_widget_windows[i].window, spec);
    }
}

struct plsr_pending_action {
    void *view;
    int action;
    char *text;
    struct plsr_pending_action *next;
};

static struct plsr_pending_action *g_pending_actions;
static struct plsr_pending_action *g_pending_actions_tail;
static pthread_mutex_t g_pending_lock = PTHREAD_MUTEX_INITIALIZER;

static void plsr_widget_queue_action(void *view, int action, const char *text) {
    if (!view) return;
    struct plsr_pending_action *a = calloc(1, sizeof *a);
    if (!a) return;
    a->view = view;
    a->action = action;
    a->text = (text && (*text || action == PLSR_WIDGET_ACTION_TEXT))
        ? strdup(text) : NULL;
    pthread_mutex_lock(&g_pending_lock);
    if (g_pending_actions_tail)
        g_pending_actions_tail->next = a;
    else
        g_pending_actions = a;
    g_pending_actions_tail = a;
    pthread_mutex_unlock(&g_pending_lock);
}

void plsr_widget_action(void *view, int action, const char *text) {
    plsr_widget_queue_action(view, action, text);
}

void plsr_widget_drain_actions(void) {
    if (!g_pending_actions || !plsr_widget_api()) return;
    pthread_mutex_lock(&g_pending_lock);
    struct plsr_pending_action *list = g_pending_actions;
    g_pending_actions = NULL;
    g_pending_actions_tail = NULL;
    pthread_mutex_unlock(&g_pending_lock);
    while (list) {
        struct plsr_pending_action *next = list->next;
        if (list->view && list->action == PLSR_WIDGET_ACTION_CLICK) {
            if (plsr_widget_responds(list->view, "performClick:")) {
                g_w_m1(list->view, g_w_sel("performClick:"), NULL);
            } else {
                /* NSToolbarItem: dispatch through its own target/action. */
                void *(*m3)(void *, void *, void *, void *, void *) =
                    (void *(*)(void *, void *, void *, void *, void *))
                        plsr_image_symbol("_objc_msgSend");
                void *action = g_w_m0(list->view, g_w_sel("action"));
                void *target = g_w_m0(list->view, g_w_sel("target"));
                void *app = plsr_class_object("NSApplication")
                    ? g_w_m0(plsr_class_object("NSApplication"),
                             g_w_sel("sharedApplication")) : NULL;
                if (m3 && action && app)
                    m3(app, g_w_sel("sendAction:to:from:"), action, target,
                       list->view);
            }
            if (getenv("PLSR_REEF_TRACE"))
                fprintf(stderr, "reef: widget click view=%p\n", list->view);
        } else if (list->view && list->action == PLSR_WIDGET_ACTION_FOCUS) {
            /* The host text widget owns key focus.  Making this partially
             * initialized Swift NSTextView AppKit's responder re-enters its
             * typingAttributes implementation and faults in this runtime. */
            if (getenv("PLSR_REEF_TRACE"))
                fprintf(stderr, "reef: widget focus routed to host editor view=%p\n",
                        list->view);
        } else if (list->view && list->action == PLSR_WIDGET_ACTION_CLOSE) {
            if (plsr_widget_responds(list->view, "performClose:"))
                g_w_m1(list->view, g_w_sel("performClose:"), NULL);
            else if (plsr_widget_responds(list->view, "close"))
                g_w_m0(list->view, g_w_sel("close"));
            if (getenv("PLSR_REEF_TRACE"))
                fprintf(stderr, "reef: widget close window=%p\n", list->view);
        } else if (list->view && list->action == PLSR_WIDGET_ACTION_COMMAND &&
                   list->text) {
            void *app_cls = plsr_class_object("NSApplication");
            void *app = app_cls
                ? g_w_m0(app_cls, g_w_sel("sharedApplication")) : NULL;
            void *action = g_w_sel(list->text);
            void *(*m3)(void *, void *, void *, void *, void *) =
                (void *(*)(void *, void *, void *, void *, void *))
                    plsr_image_symbol("_objc_msgSend");
            if (m3 && app && action)
                m3(app, g_w_sel("sendAction:to:from:"), action, NULL,
                   list->view);
            if (getenv("PLSR_REEF_TRACE"))
                fprintf(stderr, "reef: widget command %s window=%p\n",
                        list->text, list->view);
        } else if (list->view && list->action == PLSR_WIDGET_ACTION_MENU) {
            void *app_cls = plsr_class_object("NSApplication");
            void *app = app_cls
                ? g_w_m0(app_cls, g_w_sel("sharedApplication")) : NULL;
            void *menu = g_w_m0(list->view, g_w_sel("menu"));
            if (menu && plsr_widget_responds(menu, "update"))
                g_w_m0(menu, g_w_sel("update"));
            int enabled = !plsr_widget_responds(list->view, "isEnabled") ||
                (int)(intptr_t)g_w_m0(list->view, g_w_sel("isEnabled"));
            void *action = g_w_m0(list->view, g_w_sel("action"));
            void *target = g_w_m0(list->view, g_w_sel("target"));
            const char *(*sel_name)(void *) =
                (const char *(*)(void *))plsr_image_symbol("_sel_getName");
            const char *action_name = action && sel_name ? sel_name(action) : NULL;
            void *(*m3)(void *, void *, void *, void *, void *) =
                (void *(*)(void *, void *, void *, void *, void *))
                    plsr_image_symbol("_objc_msgSend");
            if (getenv("PLSR_REEF_TRACE"))
                fprintf(stderr, "reef: source menu action item=%p name=%s raw=%p target=%p enabled=%d app=%p\n",
                        list->view, action_name ? action_name : "(null)",
                        action, target, enabled, app);
            if (enabled && action && app && m3) {
                if (getenv("PLSR_REEF_TRACE"))
                    fprintf(stderr, "reef: sendAction enter name=%s\n",
                            action_name ? action_name : "(null)");
                m3(app, g_w_sel("sendAction:to:from:"), action, target,
                   list->view);
                if (getenv("PLSR_REEF_TRACE"))
                    fprintf(stderr, "reef: sendAction returned name=%s\n",
                            action_name ? action_name : "(null)");
            }
            if (getenv("PLSR_REEF_TRACE"))
                fprintf(stderr, "reef: source menu shortcut item=%p action=%p enabled=%d\n",
                        list->view, action, enabled);
        } else if (list->view && list->action == PLSR_WIDGET_ACTION_TEXT &&
                   list->text) {
            void *nsstring = plsr_class_object("NSString");
            void *value = nsstring
                ? g_w_m1(nsstring, g_w_sel("stringWithUTF8String:"),
                         (void *)list->text)
                : NULL;
            /* The store is authoritative: this libobjc has no reachable
             * cache-flush entry, so a replaced -setString: stays shadowed by
             * stale cache buckets and the store must be written directly. */
            plsr_text_set(plsr_text_owner_of(list->view), list->text);
            if (value && plsr_widget_responds(list->view, "textStorage") &&
                plsr_widget_responds(list->view, "shouldChangeTextInRange:replacementString:") &&
                plsr_widget_responds(list->view, "didChangeText")) {
                /* NSTextView's normal edit notifications drive CotEditor's
                 * document dirty state, undo registration, and observers. */
                struct { unsigned long location, length; } range = {0, 0};
                typedef struct { unsigned long location, length; } plsr_nsrange;
                void *storage = g_w_m0(list->view, g_w_sel("textStorage"));
                void *old_string = storage
                    ? g_w_m0(storage, g_w_sel("string")) : NULL;
                const char *old_utf8 = old_string
                    ? (const char *)g_w_m0(old_string, g_w_sel("UTF8String")) : NULL;
                signed char (*should_change)(void *, void *, plsr_nsrange, void *) =
                    (signed char (*)(void *, void *, plsr_nsrange, void *))
                        plsr_image_symbol("_objc_msgSend");
                void (*replace_range)(void *, void *, plsr_nsrange, void *) =
                    (void (*)(void *, void *, plsr_nsrange, void *))
                        plsr_image_symbol("_objc_msgSend");
                unsigned long (*string_length)(void *, void *) =
                    (unsigned long (*)(void *, void *))
                        plsr_image_symbol("_objc_msgSend");
                if (old_string && string_length)
                    range.length = string_length(old_string, g_w_sel("length"));
                else
                    range.length = old_utf8 ? strlen(old_utf8) : 0;
                plsr_nsrange native_range = {range.location, range.length};
                signed char allowed = should_change
                    ? should_change(list->view,
                        g_w_sel("shouldChangeTextInRange:replacementString:"),
                        native_range, value) : 1;
                if (allowed && storage && plsr_widget_responds(storage,
                    "replaceCharactersInRange:withString:")) {
                    if (replace_range)
                        replace_range(storage,
                            g_w_sel("replaceCharactersInRange:withString:"),
                            native_range, value);
                    g_w_m0(list->view, g_w_sel("didChangeText"));
                    if (string_length && plsr_widget_responds(list->view,
                                                               "setSelectedRange:")) {
                        plsr_nsrange caret = {string_length(value,
                            g_w_sel("length")), 0};
                        void (*set_range)(void *, void *, plsr_nsrange) =
                            (void (*)(void *, void *, plsr_nsrange))
                                plsr_image_symbol("_objc_msgSend");
                        if (set_range)
                            set_range(list->view, g_w_sel("setSelectedRange:"),
                                      caret);
                    }
                }
            } else if (value) {
                /* NSTextField and other string controls use their public API. */
                void *setter = plsr_widget_responds(list->view, "setStringValue:")
                    ? g_w_sel("setStringValue:") : g_w_sel("setString:");
                g_w_m1(list->view, setter, value);
            }
            if (getenv("PLSR_REEF_TRACE")) {
                char back[128] = "";
                plsr_widget_string(list->view, "string", back, sizeof back);
                if (!back[0])
                    plsr_widget_string(list->view, "stringValue", back, sizeof back);
                fprintf(stderr,
                        "reef: widget text view=%p value=%s store=%.96s "
                        "dispatch=%.96s\n",
                        list->view, list->text,
                        plsr_text_get(plsr_text_owner_of(list->view)), back);
            }
        } else if (list->view && list->action == PLSR_WIDGET_ACTION_VALUE &&
                   list->text) {
            double value = strtod(list->text, NULL);
            if (plsr_widget_responds(list->view, "setDoubleValue:")) {
                void (*set_value)(void *, void *, double) =
                    (void (*)(void *, void *, double))
                        plsr_image_symbol("_objc_msgSend");
                if (set_value)
                    set_value(list->view, g_w_sel("setDoubleValue:"), value);
            } else if (plsr_widget_responds(list->view, "setSelectedSegment:")) {
                g_w_m1(list->view, g_w_sel("setSelectedSegment:"),
                       (void *)(intptr_t)value);
            } else if (plsr_widget_responds(list->view, "selectItemAtIndex:")) {
                g_w_m1(list->view, g_w_sel("selectItemAtIndex:"),
                       (void *)(intptr_t)value);
            }
            void *action = g_w_m0(list->view, g_w_sel("action"));
            void *target = g_w_m0(list->view, g_w_sel("target"));
            void *app = plsr_class_object("NSApplication")
                ? g_w_m0(plsr_class_object("NSApplication"),
                         g_w_sel("sharedApplication")) : NULL;
            void *(*m3)(void *, void *, void *, void *, void *) =
                (void *(*)(void *, void *, void *, void *, void *))
                    plsr_image_symbol("_objc_msgSend");
            if (m3 && action && app)
                m3(app, g_w_sel("sendAction:to:from:"), action, target,
                   list->view);
            if (getenv("PLSR_REEF_TRACE"))
                fprintf(stderr, "reef: widget value view=%p value=%g\n",
                        list->view, value);
        }
        free(list->text);
        free(list);
        list = next;
    }
}

static void plsr_make_nswindow(void) {
    if (getenv("PLSR_REEF_TRACE")) fprintf(stderr, "reef: make_nswindow enter\n");
    void *(*getcls)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_objc_getClass");
    void *(*selreg)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    void *(*m0)(void *, void *) =
        (void *(*)(void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*m1r)(void *, void *, void *) =
        (void *(*)(void *, void *, void *))plsr_image_symbol("_objc_msgSend");
    void (*m1v)(void *, void *, void *) =
        (void (*)(void *, void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*m_rect4)(void *, void *, struct nsw_rect, unsigned long,
                     unsigned long, signed char) =
        (void *(*)(void *, void *, struct nsw_rect, unsigned long,
                   unsigned long, signed char))plsr_image_symbol("_objc_msgSend");
    if (!getcls || !selreg || !m0 || !m1r || !m1v || !m_rect4) return;
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: NSWindow classes NSString=%p NSApplication=%p NSWindow=%p\n",
                plsr_class_object("NSString"), plsr_class_object("NSApplication"),
                plsr_class_object("NSWindow"));
    char title[256] = "Application";
    const char *bundle = getenv("PLSR_APP_BUNDLE");
    const char *base = bundle ? strrchr(bundle, '/') : NULL;
    base = base ? base + 1 : bundle;
    if (base && *base) {
        size_t n = strcspn(base, ".");
        if (n >= sizeof title) n = sizeof title - 1;
        memcpy(title, base, n);
        title[n] = 0;
    }
    void *str = m1r(plsr_class_object("NSString"),
                    selreg("stringWithUTF8String:"), (void *)title);
    void *app = m0(plsr_class_object("NSApplication"),
                   selreg("sharedApplication"));
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: creating NSWindow for '%s' (app %p)\n", title, app);
    if (!app) return;
    struct nsw_rect r = {120, 120, 900, 620};
    void *win = m_rect4(m0(plsr_class_object("NSWindow"), selreg("alloc")),
                        selreg("initWithContentRect:styleMask:backing:defer:"),
                        r, 0xf, 2, 0);
    if (!win) {
        if (getenv("PLSR_REEF_TRACE")) fprintf(stderr, "reef: NSWindow init returned nil\n");
        return;
    }
    if (getenv("PLSR_REEF_TRACE")) fprintf(stderr, "reef: NSWindow %p allocated\n", win);
    m1v(win, selreg("setTitle:"), str);
    if (getenv("PLSR_REEF_TRACE")) fprintf(stderr, "reef: NSWindow title set\n");
    sw_node *swiftui_tree = plsr_build_swiftui_tree(win, title, (int)r.w, (int)r.h);
    if (plsr_mayo_runtime_window_open &&
        plsr_mayo_runtime_window_open(win, title, (int)r.w, (int)r.h, swiftui_tree,
                                      plsr_widget_spec_for_window(win))) {
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: NSWindow %p presented by Mayonnaise (%s, %dx%d)\n",
                    win, title, (int)r.w, (int)r.h);
        m0(win, selreg("makeKeyWindow"));
        return;
    }
    if (swiftui_tree) sw_node_free(swiftui_tree);
    if (!plsr_attach_host_surface(win, r.w, r.h, title)) {
        if (getenv("PLSR_REEF_TRACE")) fprintf(stderr, "reef: could not attach a host surface to NSWindow\n");
        return;
    }
    m0(win, selreg("makeKeyWindow"));
    if (getenv("PLSR_REEF_TRACE")) fprintf(stderr, "reef: NSWindow made key\n");
    void *(*getivar)(void *, const char *) =
        (void *(*)(void *, const char *))plsr_image_symbol("_class_getInstanceVariable");
    ptrdiff_t (*ivar_offset)(void *) =
        (ptrdiff_t (*)(void *))plsr_image_symbol("_ivar_getOffset");
    if (getivar && ivar_offset) {
        void *visible = getivar(getcls("NSWindow"), "_visible");
        ptrdiff_t off = visible ? ivar_offset(visible) : -1;
        if (off >= 0) *(unsigned char *)((char *)win + off) = 1;
    }
    m0(win, selreg("_showHost"));
    if (getenv("PLSR_REEF_TRACE")) fprintf(stderr, "reef: NSWindow host surface shown\n");
    m0(win, selreg("displayIfNeeded"));
    if (getenv("PLSR_REEF_TRACE")) fprintf(stderr, "reef: NSWindow initial display requested\n");
    plsr_pump_appkit_host(600);
    if (getenv("PLSR_REEF_TRACE")) fprintf(stderr, "reef: host surface commit pump complete\n");
    m1v(app, selreg("activateIgnoringOtherApps:"), (void *)1);
    if (getenv("PLSR_REEF_TRACE")) fprintf(stderr, "reef: NSWindow %p created\n", win);
}

/* ---- event-loop hook (PLSR_NSWINDOW) ----
 * Creating the window from the storyboard hook needs the CFBundle path, which
 * is where the heap corruption lives, and doing it inside finishLaunching
 * trips libdispatch's main-queue assertion (ud2 in libdispatch) because the run
 * loop is not up yet.  Swizzle -[NSApplication
 * nextEventMatchingMask:untilDate:inMode:dequeue:] instead: it is called from
 * the running loop on the main thread, and the window is created once. */
static void *(*g_orig_next_event)(void *, void *, unsigned long, void *,
                                  void *, signed char);
static int g_win_created;
static int g_delegate_installed;
static int g_delegate_launch_called;
static void *g_app;
static int g_present_attempts;
static int g_presented_app;
static int g_fallback_window;
static uint64_t g_last_window_scan_ns;
static void plsr_delegate_bootstrap(void);
static void *plsr_resolve_objc_imp(void *receiver, void *selector);
static long plsr_ivar_off(void *cls, const char *name);

static void *plsr_application_notification(const char *name, void *app) {
    void *(*getcls)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_objc_getClass");
    void *(*msg2)(void *, void *, void *, void *) =
        (void *(*)(void *, void *, void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    void *notification_class = getcls ? getcls("NSNotification") : NULL;
    void *notification_name = plsr_nsstring(name);
    void *notification = notification_class && msg2 && sel && notification_name
        ? msg2(notification_class, sel("notificationWithName:object:"),
               notification_name, app) : NULL;
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: notification %s object=%p value=%p\n",
                name, app, notification);
    return notification;
}

static const void *plsr_cfdata_bytes(void *self, void *cmd) {
    (void)cmd;
    const void *(*get)(void *) =
        (const void *(*)(void *))plsr_image_symbol("_CFDataGetBytePtr");
    return get ? get(self) : NULL;
}

static unsigned long plsr_cfdata_length(void *self, void *cmd) {
    (void)cmd;
    long (*get)(void *) =
        (long (*)(void *))plsr_image_symbol("_CFDataGetLength");
    return get ? (unsigned long)get(self) : 0;
}

static void *plsr_cfdata_copy(void *self, void *cmd, void *zone) {
    (void)cmd;
    (void)zone;
    void *(*retain)(void *) =
        (void *(*)(void *))plsr_image_symbol("_objc_retain");
    return retain ? retain(self) : self;
}

static void *plsr_nsdata_from_path(const char *p) {
    void *(*cfdata)(void *, const void *, long) =
        (void *(*)(void *, const void *, long))plsr_image_symbol("_CFDataCreate");
    if (!p || !cfdata) return NULL;
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long len = ftell(f);
    if (len < 0) { fclose(f); return NULL; }
    rewind(f);
    void *data = NULL;
    void *bytes = malloc((size_t)len ? (size_t)len : 1);
    if (bytes) {
        size_t n = len > 0 ? fread(bytes, 1, (size_t)len, f) : 0;
        if (len == 0 || n == (size_t)len)
            data = cfdata(NULL, bytes, len);
        free(bytes);
    }
    fclose(f);
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: NSData read '%s' -> %p len=%ld\n", p, data, len);
    return data;
}

static void *plsr_nsdata_with_file(void *cls, void *cmd, void *path) {
    (void)cls; (void)cmd;
    char *p = plsr_nsutf8(path);
    return p ? plsr_nsdata_from_path(p) : NULL;
}

static void *plsr_nsdata_with_url(void *cls, void *cmd, void *url) {
    (void)cls; (void)cmd;
    void *(*msg0)(void *, void *) =
        (void *(*)(void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    if (!url || !msg0 || !sel) return NULL;
    void *path = msg0(url, sel("path"));
    char *p = plsr_nsutf8(path);
    return p ? plsr_nsdata_from_path(p) : NULL;
}

static void *plsr_nsdata_with_url_opts_err(void *cls, void *cmd, void *url,
                                           unsigned long opts, void **err) {
    (void)cmd; (void)opts;
    if (err) *err = NULL;
    return plsr_nsdata_with_url(cls, cmd, url);
}

/* Swift Foundation's Data(contentsOf:options:) may bypass ObjC NSData class
 * methods in this runtime. Patch the Swift entry point directly.
 * SwiftCC on x86_64: URL passed indirectly (pointer to URL object) in rdi,
 * options in rsi. Returns Data as two regs (rax=obj, rdx=flags/bits). r12 is
 * Swift error register - must be 0 on success. */
typedef struct { void *obj; uintptr_t flags; } plsr_swift_data;

/* C helper: reads file, returns {obj,flags} for Swift Data, sets *errp=NULL. */
__attribute__((used,noinline)) plsr_swift_data plsr_data_contents_of_c(void **url_indirect, unsigned long opts) {
    (void)opts;
    plsr_swift_data out = {0, 0};
    void *(*sel)(const char *) = (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    void *(*msg0)(void *, void *) = (void *(*)(void *, void *))plsr_image_symbol("_objc_msgSend");
    void (*rel)(void *) = (void (*)(void *))plsr_image_symbol("_objc_release");
    if (!url_indirect || !*url_indirect || !sel || !msg0) return out;
    void *path = msg0(*url_indirect, sel("path"));
    char *p = plsr_nsutf8(path);
    void *nsdata = p ? plsr_nsdata_from_path(p) : NULL;
    if (!nsdata) return out;
    typedef plsr_swift_data (*data_ref_fn)(void *);
    data_ref_fn ref = (data_ref_fn)plsr_image_symbol(
        "__s10Foundation4DataV11referencingACSo6NSDataCh_tcfCTf4nd_n");
    if (!ref) ref = (data_ref_fn)plsr_image_symbol(
        "_$s10Foundation4DataV11referencingACSo6NSDataCh_tcfCTf4nd_n");
    if (!ref) ref = (data_ref_fn)plsr_image_symbol(
        "__s10Foundation4DataV11referencingACSo6NSDataCh_tcfC");
    if (!ref) ref = (data_ref_fn)plsr_image_symbol(
        "_$s10Foundation4DataV11referencingACSo6NSDataCh_tcfC");
    if (ref) {
        out = ref(nsdata);
        if (rel) rel(nsdata);
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: Swift Data(contentsOf:) path=%s -> obj=%p flags=%lx\n",
                    p ? p : "?", out.obj, (unsigned long)out.flags);
        return out;
    }
    if (rel) rel(nsdata);
    return out;
}

/* Swift CC: rdi=url indirect, rsi=options, r12=error; return rax=obj rdx=flags. */
__attribute__((naked,used)) static void plsr_data_contents_of_tramp(void) {
    __asm__ volatile(
        "push %%rbx\n"
        "push %%rbp\n"
        "push %%r12\n"
        "push %%r13\n"
        "push %%r14\n"
        "push %%r15\n"
        "sub $8, %%rsp\n"
        "xor %%r12d, %%r12d\n"
        "lea plsr_data_contents_of_c(%%rip), %%rax\n"
        "call *%%rax\n"
        "xor %%r12d, %%r12d\n"
        "add $8, %%rsp\n"
        "pop %%r15\n"
        "pop %%r14\n"
        "pop %%r13\n"
        "pop %%r12\n"
        "pop %%rbp\n"
        "pop %%rbx\n"
        "ret\n"
        ::: "memory");
}

/* Patch the lower-level NSData contentsOfURL thunk used by Swift Data(contentsOf:options:).
 * Signature from Ghidra: ID fn(void **urlp, unsigned opts, SEL *selp) with self in r13.
 * We replace it with a C function that reads the file and returns NSData. */
/* Swift thunk: (url_indirect=rdi, options=rsi, selp=rdx), self=r13, error=r12.
 * Returns NSData* in rax; r12 must be 0 on success. */
__attribute__((used,noinline)) void *plsr_nsdata_contents_thunk_c(void **urlp, unsigned long opts, void *selp, void *self) {
    (void)opts; (void)selp; (void)self;
    void *(*sel)(const char *) = (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    void *(*msg0)(void *, void *) = (void *(*)(void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*retain)(void *) = (void *(*)(void *))plsr_image_symbol("_objc_retain");
    void *nsdata = NULL;
    if (urlp && *urlp && sel && msg0) {
        void *path = msg0(*urlp, sel("path"));
        char *p = plsr_nsutf8(path);
        nsdata = p ? plsr_nsdata_from_path(p) : NULL;
        if (nsdata && retain) retain(nsdata);
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: nsdata contents thunk path=%s -> %p\n", p ? p : "?", nsdata);
    }
    return nsdata;
}

__attribute__((naked,used)) static void plsr_nsdata_contents_thunk_tramp(void) {
    __asm__ volatile(
        "push %%rbp\n"
        "mov %%rsp, %%rbp\n"
        "push %%rbx\n"
        "push %%r12\n"
        "push %%r13\n"
        "push %%r14\n"
        "push %%r15\n"
        "sub $8, %%rsp\n"
        /* args: rdi,rsi,rdx already set; rcx = r13 (self) */
        "mov %%r13, %%rcx\n"
        "xor %%r12d, %%r12d\n"
        "lea plsr_nsdata_contents_thunk_c(%%rip), %%rax\n"
        "call *%%rax\n"
        "xor %%r12d, %%r12d\n"
        "add $8, %%rsp\n"
        "pop %%r15\n"
        "pop %%r14\n"
        "pop %%r13\n"
        "pop %%r12\n"
        "pop %%rbx\n"
        "pop %%rbp\n"
        "ret\n"
        ::: "memory");
}

static int plsr_printable_cstr(uintptr_t p) {
    if (p < 0x1000 || p > 0x7fffffffffffULL) return 0;
    unsigned char vec = 0;
    return mincore((void *)(p & ~(uintptr_t)0xfff), 0x1000, &vec) == 0;
}
/* swift_dynamicCastFailure patch was too broad; removed. */

static void *g_swift_dynamicCast_orig;
__attribute__((used)) static bool shim_swift_dynamicCast_hook(void *dest, void *src, const void *srcType, const void *targetType, uint32_t flags) {
    typedef bool (*fn_t)(void*,void*,const void*,const void*,uint32_t);
    fn_t orig = (fn_t)g_swift_dynamicCast_orig;
    /* Fast path: Optional<Any>/Any -> Bool from NSNumber/NSString bridging glitches. */
    static const void *bool_meta;
    if (!bool_meta) {
        struct image *im = NULL;
        for (int i = 0; i < g_nimgs; i++)
            if (strstr(g_imgs[i]->path, "libswiftCore.dylib")) { im = g_imgs[i]; break; }
        if (im) bool_meta = (const void *)((uintptr_t)im->base + im->slide + 0x527f18);
    }
    if (targetType == bool_meta && src) {
        /* Try to extract an ObjC object from common existential layouts. */
        void *cands[8] = {0};
        int nc = 0;
        /* direct pointer */
        cands[nc++] = *(void **)src;
        /* Any existential: type at 0, value buffer at 16 (3 words) - object may be at 16 or inline */
        cands[nc++] = ((void **)src)[2];
        cands[nc++] = ((void **)src)[3];
        cands[nc++] = src;
        for (int i = 0; i < nc; i++) {
            void *obj = cands[i];
            if (!plsr_valid_obj(obj)) continue;
            const char *(*class_name)(const void *) =
                (const char *(*)(const void *))plsr_image_symbol("_object_getClassName");
            const char *cn = class_name ? class_name(obj) : NULL;
            if (!cn) continue;
            if (strstr(cn, "Number") || strstr(cn, "Boolean") || strcmp(cn, "__NSCFBoolean") == 0) {
                void *(*msg0)(void*,void*) = (void*(*)(void*,void*))plsr_image_symbol("_objc_msgSend");
                void *(*sel)(const char*) = (void*(*)(const char*))plsr_image_symbol("_sel_registerName");
                if (msg0 && sel && dest) {
                    bool v = (bool)(uintptr_t)msg0(obj, sel("boolValue"));
                    *(bool *)dest = v;
                    if (getenv("PLSR_REEF_TRACE"))
                        fprintf(stderr, "reef: dynamicCast NSNumber->Bool %s=%d\n", cn, (int)v);
                    return true;
                }
            }
            if (strstr(cn, "String") || strstr(cn, "NSString")) {
                char *s = plsr_nsutf8(obj);
                if (s && dest) {
                    bool v = !strcmp(s, "1") || !strcasecmp(s, "true") || !strcasecmp(s, "yes");
                    *(bool *)dest = v;
                    if (getenv("PLSR_REEF_TRACE"))
                        fprintf(stderr, "reef: dynamicCast String->Bool %s=%d\n", s, (int)v);
                    return true;
                }
            }
        }
        if (getenv("PLSR_REEF_TRACE")) {
            fprintf(stderr, "reef: dynamicCast->Bool failed src=%p srcType=%p words=%p %p %p %p\n",
                    src, srcType, ((void**)src)[0], ((void**)src)[1], ((void**)src)[2], ((void**)src)[3]);
        }
    }
    return orig ? orig(dest, src, srcType, targetType, flags) : false;
}
__attribute__((naked,used)) static void shim_swift_dynamicCast_asm(void) {
    __asm__ volatile(
        "push %rbp\nmov %rsp,%rbp\n"
        "push %r15\npush %r14\npush %r13\npush %r12\npush %rbx\n"
        "sub $8,%rsp\n"
        "call shim_swift_dynamicCast_hook\n"
        "add $8,%rsp\n"
        "pop %rbx\npop %r12\npop %r13\npop %r14\npop %r15\npop %rbp\nret\n");
}
static void patch_swift_dynamic_cast(void) {
    if (getenv("PLSR_NO_SWIFT_CAST_PATCH")) return;
    struct image *im = NULL;
    for (int i = 0; i < g_nimgs; i++) {
        if (strstr(g_imgs[i]->path, "libswiftCore.dylib")) { im = g_imgs[i]; break; }
    }
    if (!im) return;
    uintptr_t base = (uintptr_t)(im->base + im->slide);
    unsigned char *fn = (unsigned char *)(base + 0x336220);
    long pg = sysconf(_SC_PAGESIZE); if (pg <= 0) pg = 4096;
    size_t patch_len = 16; /* safe instruction boundary */
    /* build trampoline: saved bytes + jmp back */
    size_t tramp_sz = patch_len + 12;
    unsigned char *tr = (unsigned char *)mmap(NULL, (size_t)pg, PROT_READ|PROT_WRITE|PROT_EXEC,
                                               MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (tr == MAP_FAILED) return;
    memcpy(tr, fn, patch_len);
    unsigned char jback[12] = {0x48,0xb8,0,0,0,0,0,0,0,0,0xff,0xe0};
    uint64_t back = (uint64_t)(uintptr_t)(fn + patch_len);
    memcpy(jback+2, &back, 8);
    memcpy(tr + patch_len, jback, 12);
    __builtin___clear_cache((char*)tr, (char*)tr + tramp_sz);
    g_swift_dynamicCast_orig = (void *)tr;

    uintptr_t page = (uintptr_t)fn & ~(uintptr_t)(pg - 1);
    if (mprotect((void *)page, (size_t)pg * 2, PROT_READ|PROT_WRITE|PROT_EXEC) != 0) return;
    unsigned char jmp[12] = {0x48,0xb8,0,0,0,0,0,0,0,0,0xff,0xe0};
    uint64_t tgt = (uint64_t)(uintptr_t)shim_swift_dynamicCast_hook;
    memcpy(jmp+2, &tgt, 8);
    /* pad remaining bytes with nops */
    memcpy(fn, jmp, 12);
    for (size_t i = 12; i < patch_len; i++) fn[i] = 0x90;
    __builtin___clear_cache((char*)fn, (char*)fn + patch_len);
    mprotect((void *)page, (size_t)pg * 2, PROT_READ|PROT_EXEC);
    if (getenv("PLSR_REEF_TRACE")) fprintf(stderr, "reef: patched swift_dynamicCast\n");
}

static void patch_swift_nsdata_contents_thunk(void) {
    void *fn = plsr_image_symbol(
        "__sSo6NSDataC10contentsOf7optionsAB10Foundation3URLV_So0A14ReadingOptionsVtKcfcTOTm");
    if (!fn) fn = plsr_image_symbol(
        "_$sSo6NSDataC10contentsOf7optionsAB10Foundation3URLV_So0A14ReadingOptionsVtKcfcTOTm");
    if (!fn) {
        for (int i = 0; i < g_nimgs; i++) {
            if (!strstr(g_imgs[i]->path, "libswiftFoundation.dylib")) continue;
            fn = (void *)(g_imgs[i]->base + g_imgs[i]->slide + 0xffe70);
            break;
        }
    }
    if (!fn) return;
    unsigned char *p = (unsigned char *)fn;
    long pg = sysconf(_SC_PAGESIZE);
    if (pg <= 0) pg = 4096;
    uintptr_t page = (uintptr_t)p & ~(uintptr_t)(pg - 1);
    if (mprotect((void *)page, (size_t)pg * 2, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
        return;
    unsigned char tramp[12] = {0x48,0xb8,0,0,0,0,0,0,0,0,0xff,0xe0};
    uint64_t t = (uint64_t)(uintptr_t)plsr_nsdata_contents_thunk_tramp;
    memcpy(tramp + 2, &t, 8);
    memcpy(p, tramp, sizeof tramp);
    __builtin___clear_cache((char *)p, (char *)p + sizeof tramp);
    mprotect((void *)page, (size_t)pg * 2, PROT_READ | PROT_EXEC);
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: patched Swift NSData contents thunk at %p\n", fn);
}

static void *plsr_nsdata_with_file_opts_err(void *cls, void *cmd, void *path,
                                            unsigned long opts, void **err) {
    (void)cls; (void)cmd; (void)opts;
    if (err) *err = NULL;
    char *p = plsr_nsutf8(path);
    return p ? plsr_nsdata_from_path(p) : NULL;
}

static void *plsr_nsdata_init_with_file_opts_err(void *self, void *cmd, void *path,
                                                 unsigned long opts, void **err) {
    (void)cmd; (void)opts;
    if (err) *err = NULL;
    void *(*release)(void *) = (void *(*)(void *))plsr_image_symbol("_objc_release");
    char *p = plsr_nsutf8(path);
    void *data = p ? plsr_nsdata_from_path(p) : NULL;
    if (data) { if (release && self) release(self); return data; }
    return self;
}

static void *plsr_nsdata_init_with_url_opts_err(void *self, void *cmd, void *url,
                                                unsigned long opts, void **err) {
    (void)cmd; (void)opts;
    if (err) *err = NULL;
    void *(*release)(void *) = (void *(*)(void *))plsr_image_symbol("_objc_release");
    void *data = plsr_nsdata_with_url(NULL, NULL, url);
    if (data) { if (release && self) release(self); return data; }
    return self;
}

static void patch_nsdata_read(void) {
    void *(*getcls)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_objc_getClass");
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    void *(*replace)(void *, void *, void *, const char *) =
        (void *(*)(void *, void *, void *, const char *))
            plsr_image_symbol("_class_replaceMethod");
    void *(*getmeta)(void *) =
        (void *(*)(void *))plsr_image_symbol("_object_getClass");
    if (!getcls || !sel || !replace) return;
    void *nsdata = getcls("NSData");
    if (!nsdata) nsdata = plsr_class_object("NSData");
    if (nsdata) {
        void *meta = getmeta ? getmeta(nsdata) : NULL;
        /* class methods (+) */
        if (meta) {
            replace(meta, sel("dataWithContentsOfFile:"),
                    (void *)plsr_nsdata_with_file, "@@:@");
            replace(meta, sel("dataWithContentsOfFile:options:error:"),
                    (void *)plsr_nsdata_with_file_opts_err, "@@:@Q^@");
            replace(meta, sel("dataWithContentsOfURL:"),
                    (void *)plsr_nsdata_with_url, "@@:@");
            replace(meta, sel("dataWithContentsOfURL:options:error:"),
                    (void *)plsr_nsdata_with_url_opts_err, "@@:@Q^@");
        }
        /* instance methods (-) */
        replace(nsdata, sel("initWithContentsOfFile:options:error:"),
                (void *)plsr_nsdata_init_with_file_opts_err, "@@:@Q^@");
        replace(nsdata, sel("initWithContentsOfURL:options:error:"),
                (void *)plsr_nsdata_init_with_url_opts_err, "@@:@Q^@");
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: patched NSData file/url readers class=%p meta=%p\n", nsdata, meta);
    }
}

typedef struct { const void *ptr; unsigned long len; } plsr_nib_storage;
static void *g_nib_data_class;
static void *g_nib_data_obj;
static size_t g_nib_storage_off;
static void *g_nib_orig_bytes;
static void *g_nib_orig_length;
static void *g_nib_orig_get_bytes;
static void *g_nib_orig_copy;

static const void *plsr_nib_bytes(void *self, void *cmd) {
    (void)cmd;
    if (self != g_nib_data_obj) {
        if (g_nib_orig_bytes)
            return ((const void *(*)(void *, void *))g_nib_orig_bytes)(self, cmd);
        return NULL;
    }
    plsr_nib_storage *s = g_nib_storage_off
        ? *(plsr_nib_storage **)((char *)self + g_nib_storage_off) : NULL;
    return s ? s->ptr : NULL;
}

static unsigned long plsr_nib_length(void *self, void *cmd) {
    (void)cmd;
    if (self != g_nib_data_obj) {
        if (g_nib_orig_length)
            return ((unsigned long (*)(void *, void *))g_nib_orig_length)(self, cmd);
        return 0;
    }
    plsr_nib_storage *s = g_nib_storage_off
        ? *(plsr_nib_storage **)((char *)self + g_nib_storage_off) : NULL;
    return s ? s->len : 0;
}

static void plsr_nib_get_bytes(void *self, void *cmd, void *buf,
                               unsigned long len) {
    if (self != g_nib_data_obj) {
        if (g_nib_orig_get_bytes)
            ((void (*)(void *, void *, void *, unsigned long))g_nib_orig_get_bytes)
                (self, cmd, buf, len);
        return;
    }
    plsr_nib_storage *s = g_nib_storage_off
        ? *(plsr_nib_storage **)((char *)self + g_nib_storage_off) : NULL;
    if (s && buf) memcpy(buf, s->ptr, len < s->len ? len : s->len);
}

static signed char plsr_nib_is_kind(void *self, void *cmd, void *cls) {
    (void)cmd;
    void *(*getcls)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_objc_getClass");
    if (getcls && (cls == getcls("NSData") || cls == getcls("NSObject") ||
                   cls == g_nib_data_class))
        return 1;
    return 0;
}

static void *plsr_nib_copy(void *self, void *cmd, void *zone) {
    if (self != g_nib_data_obj && g_nib_orig_copy)
        return ((void *(*)(void *, void *, void *))g_nib_orig_copy)(self, cmd, zone);
    void *(*retain)(void *) =
        (void *(*)(void *))plsr_image_symbol("_objc_retain");
    return retain ? retain(self) : self;
}

static void *plsr_make_nib_data(const void *bytes, unsigned long len) {
    void *(*getcls)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_objc_getClass");
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    if (!getcls || !sel) return NULL;
    if (!g_nib_data_class) {
        void *base = getcls("NSObject");
        if (!base) base = plsr_class_object("NSObject");
        if (!base) return NULL;
        void *(*alloc_pair)(void *, const char *, size_t) =
            (void *(*)(void *, const char *, size_t))
                plsr_image_symbol("_objc_allocateClassPair");
        int (*add_method)(void *, void *, void *, const char *) =
            (int (*)(void *, void *, void *, const char *))
                plsr_image_symbol("_class_addMethod");
        void (*reg)(void *) =
            (void (*)(void *))plsr_image_symbol("_objc_registerClassPair");
        size_t (*inst_size)(void *) =
            (size_t (*)(void *))plsr_image_symbol("_class_getInstanceSize");
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr,
                    "reef: native data base=%p alloc=%p add=%p reg=%p size=%p\n",
                    base, alloc_pair, add_method, reg, inst_size);
        if (!alloc_pair || !add_method || !reg || !inst_size) return NULL;
        void *cls = alloc_pair(base, "PLSRNibData", 0);
        if (!cls) {
            if (getenv("PLSR_REEF_TRACE"))
                fprintf(stderr, "reef: native data alloc_pair failed\n");
            return NULL;
        }
        if (!add_method(cls, sel("bytes"), (void *)plsr_nib_bytes, "^v@:") ||
            !add_method(cls, sel("length"), (void *)plsr_nib_length, "Q@:") ||
            !add_method(cls, sel("getBytes:length:"), (void *)plsr_nib_get_bytes,
                        "v@:^vQ") ||
            !add_method(cls, sel("isKindOfClass:"), (void *)plsr_nib_is_kind,
                        "c@:#") ||
            !add_method(cls, sel("copyWithZone:"), (void *)plsr_nib_copy,
                        "@@:^v")) {
            if (getenv("PLSR_REEF_TRACE"))
                fprintf(stderr, "reef: MainMenu native data method add failed\n");
            return NULL;
        }
        reg(cls);
        g_nib_storage_off = inst_size(cls);
        if (g_nib_storage_off < sizeof(void *)) return NULL;
        g_nib_data_class = cls;
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: MainMenu native data class=%p off=%zu\n",
                    cls, g_nib_storage_off);
    }
    void *(*create)(void *, size_t) =
        (void *(*)(void *, size_t))plsr_image_symbol("_class_createInstance");
    if (!create || !g_nib_storage_off) return NULL;
    plsr_nib_storage *s = malloc(sizeof *s);
    if (!s) return NULL;
    s->ptr = bytes;
    s->len = len;
    void *obj = create(g_nib_data_class, sizeof(void *));
    if (!obj) { free(s); return NULL; }
    *(plsr_nib_storage **)((char *)obj + g_nib_storage_off) = s;
    return obj;
}

/* The runtime rejects objc_allocateClassPair(NSData, ...), so use an actual
 * NSData instance with extra storage and replace NSData's byte accessors. */
static void *plsr_make_nib_data_nsdata(const void *bytes, unsigned long len) {
    void *(*getcls)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_objc_getClass");
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    void *(*create)(void *, size_t) =
        (void *(*)(void *, size_t))plsr_image_symbol("_class_createInstance");
    size_t (*inst_size)(void *) =
        (size_t (*)(void *))plsr_image_symbol("_class_getInstanceSize");
    void *(*replace)(void *, void *, void *, const char *) =
        (void *(*)(void *, void *, void *, const char *))
            plsr_image_symbol("_class_replaceMethod");
    if (!getcls || !sel || !create || !inst_size || !replace) return NULL;
    void *nsdata = getcls("NSData");
    if (!nsdata) nsdata = plsr_class_object("NSData");
    if (!nsdata) return NULL;
    if (!g_nib_storage_off) {
        g_nib_storage_off = inst_size(nsdata);
        if (!g_nib_storage_off) return NULL;
        g_nib_orig_bytes = (void *)replace(nsdata, sel("bytes"),
                                           (void *)plsr_nib_bytes, "^v@:");
        g_nib_orig_length = (void *)replace(nsdata, sel("length"),
                                            (void *)plsr_nib_length, "Q@:");
        g_nib_orig_get_bytes = (void *)replace(nsdata, sel("getBytes:length:"),
                                               (void *)plsr_nib_get_bytes,
                                               "v@:^vQ");
        g_nib_orig_copy = (void *)replace(nsdata, sel("copyWithZone:"),
                                          (void *)plsr_nib_copy, "@@:^v");
    }
    void *obj = create(nsdata, sizeof(void *));
    if (!obj) return NULL;
    plsr_nib_storage *s = malloc(sizeof *s);
    if (!s) return NULL;
    s->ptr = bytes;
    s->len = len;
    g_nib_data_obj = obj;
    *(plsr_nib_storage **)((char *)obj + g_nib_storage_off) = s;
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: MainMenu NSData object=%p off=%zu\n",
                obj, g_nib_storage_off);
    return obj;
}

/* The shipped Main.storyboardc contains the real MainMenu.nib with the
 * application menu and AppDelegate outlets.  NSApplication never loaded it on
 * this runtime, so KeyBindingManager.shared sees a nil mainMenu and traps.
 * Instantiate the real nib before the delegate lifecycle callbacks. */
static unsigned int plsr_nib_find_main_menu(void *decoder);
static void plsr_nib_populate_menu_tree(void *decoder, unsigned int root_id);
static void plsr_nib_apply_connectors(void *decoder);
static void plsr_load_main_menu(void) {
    if (getenv("PLSR_NO_LOAD_MENU")) return;
    const char *bundle = getenv("PLSR_APP_BUNDLE");
    if (!bundle || !*bundle) return;
    void *(*getcls)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_objc_getClass");
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    void *(*msg0)(void *, void *) =
        (void *(*)(void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*msg1)(void *, void *, void *) =
        (void *(*)(void *, void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*msg2)(void *, void *, void *, void *) =
        (void *(*)(void *, void *, void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*msg3)(void *, void *, void *, void *, void *) =
        (void *(*)(void *, void *, void *, void *, void *))plsr_image_symbol("_objc_msgSend");
    signed char (*msg2b)(void *, void *, void *, void **) =
        (signed char (*)(void *, void *, void *, void **))
            plsr_image_symbol("_objc_msgSend");
    const char *(*class_name)(const void *) =
        (const char *(*)(const void *))plsr_image_symbol("_object_getClassName");
    if (!getcls || !sel || !msg0 || !msg1 || !msg2 || !msg2b) return;
    void *app = msg0(getcls("NSApplication"), sel("sharedApplication"));
    if (!app) return;
    char path[PATH_MAX];
    snprintf(path, sizeof path,
             "%s/Contents/Resources/Base.lproj/Main.storyboardc/MainMenu.nib",
             bundle);
    void *ns_path = msg1(getcls("NSString"), sel("stringWithUTF8String:"),
                         (void *)path);
    void *nsdata_cls = getcls("NSData");
    if (!nsdata_cls) nsdata_cls = plsr_class_object("NSData");
    void *nib_data = (nsdata_cls && ns_path)
        ? msg1(msg0(nsdata_cls, sel("alloc")), sel("initWithContentsOfFile:"),
               ns_path)
        : NULL;
    if (!nib_data && nsdata_cls) {
        FILE *f = fopen(path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long len = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (len > 0) {
                void *bytes = malloc((size_t)len);
                if (bytes && fread(bytes, 1, (size_t)len, f) == (size_t)len) {
                    void *custom = plsr_make_nib_data(bytes, (unsigned long)len);
                    if (custom) {
                        nib_data = custom;
                        bytes = NULL;   /* owned by PLSRNibData */
                        if (getenv("PLSR_REEF_TRACE"))
                            fprintf(stderr, "reef: MainMenu native NSData=%p len=%ld\n",
                                    custom, len);
                    }
                    void *(*cfdata_create)(void *, const void *, long) =
                        (void *(*)(void *, const void *, long))
                            plsr_image_symbol("_CFDataCreate");
                    if (!nib_data && cfdata_create)
                        nib_data = cfdata_create(NULL, bytes, len);
                    if (!nib_data)
                        nib_data = msg2(msg0(nsdata_cls, sel("alloc")),
                                        sel("initWithBytes:length:"), bytes,
                                        (void *)(uintptr_t)len);
                    if (getenv("PLSR_REEF_TRACE"))
                        fprintf(stderr, "reef: MainMenu CFDataCreate=%p len=%ld data=%p\n",
                                cfdata_create, len, nib_data);
                }
                free(bytes);
            }
            fclose(f);
        }
    }
    if (nib_data) {
        void *(*getclass)(void *) =
            (void *(*)(void *))plsr_image_symbol("_object_getClass");
        void *(*replace)(void *, void *, void *, const char *) =
            (void *(*)(void *, void *, void *, const char *))
                plsr_image_symbol("_class_replaceMethod");
        if (getclass && replace) {
            void *data_class = getclass(nib_data);
            replace(data_class, sel("bytes"), (void *)plsr_cfdata_bytes, "^v@:");
            replace(data_class, sel("length"), (void *)plsr_cfdata_length, "Q@:");
            replace(data_class, sel("copyWithZone:"), (void *)plsr_cfdata_copy,
                    "@@:^v");
            if (getenv("PLSR_REEF_TRACE"))
                fprintf(stderr, "reef: MainMenu patched %s bytes/length\n",
                        class_name ? class_name(nib_data) : "data");
        }
    }
    void *main_bundle = msg0(getcls("NSBundle"), sel("mainBundle"));
    void *nib_class = getcls("NSNib");
    if (!nib_class) nib_class = plsr_class_object("NSNib");
    char sbpath[PATH_MAX];
    snprintf(sbpath, sizeof sbpath,
             "%s/Contents/Resources/Base.lproj/Main.storyboardc", bundle);
    void *sb_ns = msg1(getcls("NSString"), sel("stringWithUTF8String:"),
                       (void *)sbpath);
    void *sb_bundle = msg1(getcls("NSBundle"), sel("bundleWithPath:"), sb_ns);
    if (!sb_bundle)
        sb_bundle = msg1(msg0(getcls("NSBundle"), sel("alloc")),
                         sel("initWithPath:"), sb_ns);
    void *nib_name = msg1(getcls("NSString"), sel("stringWithUTF8String:"),
                          (void *)"MainMenu");
    void *nib = NULL;
    if (nib_class && sb_bundle)
        nib = msg2(msg0(nib_class, sel("alloc")), sel("initWithNibNamed:bundle:"),
                   nib_name, sb_bundle);
    if (!nib && nib_class && nib_data)
        nib = msg2(msg0(nib_class, sel("alloc")), sel("initWithNibData:bundle:"),
                   nib_data, main_bundle);
    if (nib && getenv("PLSR_REEF_TRACE")) {
        void *nib_data2 = *(void **)((char *)nib + 8);
        void *b = nib_data2 ? msg0(nib_data2, sel("bytes")) : NULL;
        unsigned long l = nib_data2
            ? (unsigned long)(uintptr_t)msg0(nib_data2, sel("length")) : 0;
        fprintf(stderr, "reef: NSNib data=%p cls=%s bytes=%p len=%lu magic=%.10s\n",
                nib_data2, class_name ? class_name(nib_data2) : "?", b, l,
                b ? (const char *)b : "");
        void *decoder_cls = getcls("AKNibDecoder");
        void *decoder = (decoder_cls && nib_data2)
            ? msg3(msg0(decoder_cls, sel("alloc")),
                   sel("initWithData:bundle:owner:"), nib_data2, main_bundle, app)
            : NULL;
        fprintf(stderr, "reef: AKNibDecoder=%p\n", decoder);
        if (decoder) {
            void *(*getivar)(void *, const char *) =
                (void *(*)(void *, const char *))plsr_image_symbol("_class_getInstanceVariable");
            long (*ivaroff)(void *) =
                (long (*)(void *))plsr_image_symbol("_ivar_getOffset");
            void *keys_ivar = getivar ? getivar(decoder_cls, "_keys") : NULL;
            void *idx_ivar = getivar ? getivar(decoder_cls, "_keyIndexes") : NULL;
            long keys_off = (keys_ivar && ivaroff) ? ivaroff(keys_ivar) : 0;
            long idx_off = (idx_ivar && ivaroff) ? ivaroff(idx_ivar) : 0;
            void *keys = keys_off ? *(void **)((char *)decoder + keys_off) : NULL;
            void *idx = idx_off ? *(void **)((char *)decoder + idx_off) : NULL;
            unsigned long kc = keys
                ? (unsigned long)(uintptr_t)msg0(keys, sel("count")) : 0;
            unsigned long ic = idx
                ? (unsigned long)(uintptr_t)msg0(idx, sel("count")) : 0;
            const char *k0 = "";
            if (kc) {
                void *o = msg1(keys, sel("objectAtIndex:"), (void *)0);
                if (o) k0 = (const char *)msg0(o, sel("UTF8String"));
            }
            fprintf(stderr,
                    "reef: decoder keys=%p(%lu,%s) idx=%p(%lu) k0=%.40s\n",
                    keys, kc, keys && class_name ? class_name(keys) : "?",
                    idx, ic, k0 ? k0 : "");
            long off_classes = plsr_ivar_off(decoder_cls, "_classNames");
            long off_objects = plsr_ivar_off(decoder_cls, "_objects");
            long off_objcount = plsr_ivar_off(decoder_cls, "_objectCount");
            void *classes = off_classes >= 0
                ? *(void **)((char *)decoder + off_classes) : NULL;
            void *objects = off_objects >= 0
                ? *(void **)((char *)decoder + off_objects) : NULL;
            unsigned long oc = off_objcount >= 0
                ? *(unsigned int *)((char *)decoder + off_objcount) : 0;
            const char *c0 = "", *c1 = "";
            if (classes && kc) {
                void *o = msg1(classes, sel("objectAtIndex:"), (void *)0);
                if (o) c0 = (const char *)msg0(o, sel("UTF8String"));
                o = msg1(classes, sel("objectAtIndex:"), (void *)1);
                if (o) c1 = (const char *)msg0(o, sel("UTF8String"));
            }
            fprintf(stderr,
                    "reef: decoder classes=%p c0=%.30s c1=%.30s objects=%p oc=%lu\n",
                    classes, c0 ? c0 : "", c1 ? c1 : "", objects, oc);
            void *key = msg1(getcls("NSString"), sel("stringWithUTF8String:"),
                             (void *)"IB.objectdata");
            void *od = msg1(decoder, sel("_valueForKey:"), key);
            fprintf(stderr, "reef: decoder IB.objectdata value=%p type=%d val=%u\n",
                    od, od ? *(int *)((char *)od + 4) : -1,
                    od ? *(unsigned int *)((char *)od + 8) : 0);
            void *decoded = msg0(decoder, sel("decodeNib"));
            fprintf(stderr, "reef: decoder objectdata=%p decoded=%p cls=%s\n",
                    od, decoded, decoded && class_name ? class_name(decoded) : "?");
        }
    }
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr,
                "reef: MainMenu data=%p len=%lu cls=%p bundle=%p sb=%p nibcls=%p nib=%p\n",
                nib_data,
                nib_data ? (unsigned long)(uintptr_t)msg0(nib_data, sel("length")) : 0,
                nsdata_cls, main_bundle, sb_bundle, nib_class, nib);
    void *top = NULL;
    signed char ok = 0;
    if (nib)
        ok = msg2b(nib, sel("instantiateWithOwner:topLevelObjects:"), app, &top);
    if (!nib && sb_bundle) {
        signed char (*msg3b)(void *, void *, void *, void *, void **) =
            (signed char (*)(void *, void *, void *, void *, void **))
                plsr_image_symbol("_objc_msgSend");
        if (msg3b)
            ok = msg3b(sb_bundle, sel("_loadNibNamed:owner:topLevelObjects:"),
                       nib_name, app, &top);
    }
    if (!nib && !ok) {
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: MainMenu.nib load failed (%s)\n", path);
        return;
    }
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: MainMenu.nib instantiated ok=%d top=%p\n", ok, top);
    void *menu = msg0(app, sel("mainMenu"));
    void *delegate = msg0(app, sel("delegate"));
    if (top) {
        unsigned long count =
            (unsigned long)(uintptr_t)msg0(top, sel("count"));
        for (unsigned long i = 0; i < count; i++) {
            void *obj = msg1(top, sel("objectAtIndex:"), (void *)(uintptr_t)i);
            const char *cls = class_name ? class_name(obj) : NULL;
            if (getenv("PLSR_REEF_TRACE"))
                fprintf(stderr, "reef: MainMenu top[%lu]=%p %s\n", i, obj,
                        cls ? cls : "?");
            if (!menu && cls && strstr(cls, "Menu")) {
                menu = obj;
                msg1(app, sel("setMainMenu:"), menu);
            }
            if (cls && strstr(cls, "AppDelegate")) {
                msg1(app, sel("setDelegate:"), obj);
                delegate = obj;
            }
        }
    }
    if (!menu && main_bundle) {
        void *full_name = msg1(getcls("NSString"), sel("stringWithUTF8String:"),
                               (void *)"Base.lproj/Main.storyboardc/MainMenu");
        signed char (*msg3b)(void *, void *, void *, void *, void **) =
            (signed char (*)(void *, void *, void *, void *, void **))
                plsr_image_symbol("_objc_msgSend");
        void *top2 = NULL;
        if (msg3b && full_name &&
            msg3b(main_bundle, sel("_loadNibNamed:owner:topLevelObjects:"),
                  full_name, app, &top2) && top2) {
            unsigned long count =
                (unsigned long)(uintptr_t)msg0(top2, sel("count"));
            for (unsigned long i = 0; i < count; i++) {
                void *obj = msg1(top2, sel("objectAtIndex:"),
                                 (void *)(uintptr_t)i);
                const char *cls = class_name ? class_name(obj) : NULL;
                if (!menu && cls && strstr(cls, "Menu")) {
                    menu = obj;
                    msg1(app, sel("setMainMenu:"), menu);
                }
                if (cls && strstr(cls, "AppDelegate")) {
                    msg1(app, sel("setDelegate:"), obj);
                    delegate = obj;
                }
            }
            if (getenv("PLSR_REEF_TRACE"))
                fprintf(stderr, "reef: MainMenu NSBundle loaded menu=%p top=%p\n",
                        menu, top2);
        }
    }
    unsigned long loaded_menu_count = 0;
    if (menu && msg0 && sel) {
        void *loaded_items = msg0(menu, sel("itemArray"));
        if (loaded_items)
            loaded_menu_count = (unsigned long)(uintptr_t)
                msg0(loaded_items, sel("count"));
    }
    /* NSNib may return an empty placeholder menu when collection decoding
     * fails.  Still decode the archive relationships and populate that real
     * menu object from the archived NSMenuItem instances. */
    if ((!menu || !loaded_menu_count) && nib_data) {
        void *decoder_cls = getcls("AKNibDecoder");
        void *decoder = decoder_cls
            ? msg3(msg0(decoder_cls, sel("alloc")),
                   sel("initWithData:bundle:owner:"), nib_data, main_bundle, app)
            : NULL;
        unsigned int root_id = decoder ? plsr_nib_find_main_menu(decoder) : UINT_MAX;
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: MainMenu archive decoder=%p root=%u count=%lu\n",
                    decoder, root_id, loaded_menu_count);
        void *real_menu = decoder && root_id != UINT_MAX
            ? msg1(decoder, sel("_decodeObjectAtIndex:"),
                   (void *)(uintptr_t)root_id) : NULL;
        const char *mc = (real_menu && class_name) ? class_name(real_menu) : NULL;
        if (real_menu && mc && strstr(mc, "Menu")) {
            if (!menu) menu = real_menu;
            /* NIB arrays are represented by menu-item relationships in this
             * runtime; populate them from the archive below instead of asking
             * the incomplete NSArray coder to decode its object list. */
            void *decoded_items = NULL;
            unsigned long decoded_count = decoded_items
                ? (unsigned long)(uintptr_t)msg0(decoded_items, sel("count")) : 0;
            unsigned long current_count = msg0(menu, sel("itemArray"))
                ? (unsigned long)(uintptr_t)msg0(
                    msg0(menu, sel("itemArray")), sel("count")) : 0;
            if (getenv("PLSR_REEF_TRACE"))
                fprintf(stderr,
                    "reef: MainMenu decoded NSMenuItems=%p count=%lu current=%lu cls=%s\n",
                    decoded_items, decoded_count, current_count,
                    decoded_items && class_name ? class_name(decoded_items) : "(nil)");
            if (!current_count && decoded_count) {
                for (unsigned long i = 0; i < decoded_count; i++) {
                    void *item = msg1(decoded_items, sel("objectAtIndex:"),
                                      (void *)(uintptr_t)i);
                    const char *ic = item && class_name ? class_name(item) : NULL;
                    if (item && ic && strstr(ic, "MenuItem"))
                        msg1(menu, sel("addItem:"), item);
                }
            }
            if (menu != real_menu) {
                /* Keep the instantiated menu and its delegate graph. */
                void *array = msg0(menu, sel("itemArray"));
                unsigned long n = array ? (unsigned long)(uintptr_t)
                    msg0(array, sel("count")) : 0;
                if (n == 0) menu = real_menu;
            }
            msg1(app, sel("setMainMenu:"), menu);
            plsr_nib_populate_menu_tree(decoder, root_id);
            plsr_nib_apply_connectors(decoder);
            if (getenv("PLSR_REEF_TRACE"))
                fprintf(stderr, "reef: MainMenu decoded real menu=%p cls=%s\n",
                        menu, mc);
        }
    }
    if (!menu) {
        void *menu_class = getcls("NSMenu");
        if (!menu_class) menu_class = plsr_class_object("NSMenu");
        if (menu_class) {
            void *empty = msg0(msg0(menu_class, sel("alloc")), sel("init"));
            if (!empty)
                empty = msg1(msg0(menu_class, sel("alloc")),
                             sel("initWithTitle:"),
                             msg1(getcls("NSString"), sel("stringWithUTF8String:"),
                                  (void *)"CotEditor"));
            if (empty) {
                msg1(app, sel("setMainMenu:"), empty);
                menu = empty;
                if (getenv("PLSR_REEF_TRACE"))
                    fprintf(stderr,
                            "reef: MainMenu using empty NSMenu %p for KeyBindingManager\n",
                            empty);
            }
        }
    }
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: MainMenu menu=%p delegate=%p\n", menu, delegate);
    plsr_capture_menu_shortcuts(app);
}

typedef struct { double x, y, w, h; } plsr_nsrect;
typedef struct { double w, h; } plsr_nssize;
uintptr_t plsr_image_base(const char *needle);

/* Swift class initializers receive self in r13 on x86_64.  The real
 * EditorTextView initializer also takes textStorage (rdi), lineEndingScanner
 * (rsi), and a zero NSRect in xmm0-3. */
__attribute__((naked)) static void *plsr_etv_init(void *fn, void *self,
                                                  void *storage, void *scanner) {
    __asm__ __volatile__(
        "mov %rdi, %rax\n\t"
        "mov %rsi, %r13\n\t"
        "mov %rdx, %rdi\n\t"
        "mov %rcx, %rsi\n\t"
        "pxor %xmm0, %xmm0\n\t"
        "pxor %xmm1, %xmm1\n\t"
        "pxor %xmm2, %xmm2\n\t"
        "pxor %xmm3, %xmm3\n\t"
        "sub $8, %rsp\n\t"
        "call *%rax\n\t"
        "add $8, %rsp\n\t"
        "mov %r13, %rax\n\t"
        "ret\n\t"
    );
}
static void *(*g_swift_alloc_orig)(void *, size_t, size_t);
static int g_alloc_logs;
static void *plsr_swift_alloc(void *metadata, size_t size, size_t align) {
    void *obj = g_swift_alloc_orig
        ? g_swift_alloc_orig(metadata, size, align) : NULL;
    /* The host Swift runtime leaves authentication/tag bits in isa on this
     * x86_64 image.  This loader's objc_msgSend expects the plain class
     * metadata pointer, so normalize it at allocation time. */
    if (obj && metadata) {
        uintptr_t m = (uintptr_t)metadata & (uintptr_t)0x7fffffffffffULL;
        m &= ~(uintptr_t)1;
        *(void **)obj = (void *)m;
    }
    if (obj && metadata && getenv("PLSR_TRACE_ALLOC") && g_alloc_logs++ < 160)
        fprintf(stderr, "reef: alloc meta=%p size=%zu align=%zu obj=%p isa=%p\n",
                metadata, size, align, obj, *(void **)obj);
    return obj;
}

static void *plsr_etv_safe_font(void *self, void *cmd) {
    (void)cmd;
    return self;
}

static void plsr_noop_typing_attributes(void *self, void *cmd, void *attrs) {
    (void)self;
    (void)cmd;
    (void)attrs;
}

static void plsr_replace_super_method(void *cls, const char *name) {
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    void *(*getsuper)(void *) =
        (void *(*)(void *))plsr_image_symbol("_class_getSuperclass");
    void *(*getmeth)(void *, void *) =
        (void *(*)(void *, void *))plsr_image_symbol("_class_getInstanceMethod");
    void *(*getimp)(void *) =
        (void *(*)(void *))plsr_image_symbol("_method_getImplementation");
    const char *(*gettype)(void *) =
        (const char *(*)(void *))plsr_image_symbol("_method_getTypeEncoding");
    void *(*replace)(void *, void *, void *, const char *) =
        (void *(*)(void *, void *, void *, const char *))
            plsr_image_symbol("_class_replaceMethod");
    if (!cls || !name || !sel || !getsuper || !getmeth || !getimp ||
        !gettype || !replace) return;
    void *super = getsuper(cls);
    void *method = super ? getmeth(super, sel(name)) : NULL;
    if (method)
        replace(cls, sel(name), getimp(method), gettype(method));
}

static void plsr_patch_etv_tail(void) {
    static int done;
    if (done) return;
    done = 1;
    uintptr_t base = plsr_image_base("CotEditor");
    uint8_t *p = (uint8_t *)(base + 0x154510);
    uintptr_t page = (uintptr_t)p & ~(uintptr_t)0xfff;
    if (mprotect((void *)page, 0x1000,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
        return;
    /* The direct Swift font getter returns a host NSFont whose isa this
     * runtime cannot retain.  During construction return the valid view
     * itself; the real initializer only needs a non-nil value here. */
    p[0] = 0x4c; p[1] = 0x89; p[2] = 0xe8; p[3] = 0xc3;
    mprotect((void *)page, 0x1000, PROT_READ | PROT_EXEC);
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: patched EditorTextView font getter at %p\n", p);
}

/* Every Swift image binds its own _swift_allocObject slot.  Normalizing only
 * CotEditor's slot leaves Combine/Foundation allocations with the tagged isa
 * that this runtime's objc_msgSend cannot decode, so rewrite every GOT slot
 * that still targets the same allocator. */
static int plsr_normalize_swift_alloc_slots(void *orig) {
    if (!orig) return 0;
    int patched = 0;
    for (int i = 0; i < g_nimgs; i++) {
        struct image *im = g_imgs[i];
        for (int s = 0; s < im->nsegs; s++) {
            const struct segment_command_64 *seg = im->segs[s];
            const struct section_64 *sect = (const struct section_64 *)(seg + 1);
            for (uint32_t j = 0; j < seg->nsects; j++, sect++) {
                const char *n = sect->sectname;
                if (!strstr(n, "got") && !strstr(n, "symbol_ptr")) continue;
                uintptr_t base = (uintptr_t)(sect->addr + im->slide);
                if (!sect->size) continue;
                uintptr_t page = base & ~(uintptr_t)0xfff;
                size_t len = (size_t)((base + sect->size) - page);
                if (mprotect((void *)page, len,
                             PROT_READ | PROT_WRITE) != 0) continue;
                for (size_t off = 0; off + 8 <= sect->size; off += 8) {
                    void **slot = (void **)(base + off);
                    if (*slot != orig) continue;
                    *slot = (void *)plsr_swift_alloc;
                    patched++;
                    if (getenv("PLSR_TRACE_ALLOC"))
                        fprintf(stderr, "reef: swift alloc slot %s %s+%#zx\n",
                                im->path, n, off);
                }
                mprotect((void *)page, len, PROT_READ | PROT_WRITE);
            }
        }
    }
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: normalized %d swift alloc slot(s)\n", patched);
    return patched;
}

static void plsr_patch_swift_alloc(void) {
    if (g_swift_alloc_orig) return;
    void *sym = plsr_image_symbol("_swift_allocObject");
    if (!sym) sym = plsr_image_symbol("swift_allocObject");
    uintptr_t slot_addr = plsr_image_base("CotEditor") + 0x3254750;
    void **slot = (void **)slot_addr;
    uintptr_t page = slot_addr & ~(uintptr_t)0xfff;
    if (mprotect((void *)page, 0x1000, PROT_READ | PROT_WRITE) == 0) {
        void *old = *slot;
        if (!old && !sym) return;
        /* Call the existing slot target, not the raw symbol: the slot is a
         * Swift thunk that supplies allocator size/alignment arguments. */
        g_swift_alloc_orig = (void *(*)(void *, size_t, size_t))(old ? old : sym);
        *slot = (void *)plsr_swift_alloc;
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr,
                    "reef: swift alloc sym=%p slot=%p old=%p new=%p\n",
                    sym, slot, old, (void *)plsr_swift_alloc);
        plsr_normalize_swift_alloc_slots(old ? old : sym);
    }
}

typedef struct { unsigned long location, length; } plsr_nsrange;

static plsr_nsrange (*g_range_orig)(void *, void *, void *, unsigned long,
                                    plsr_nsrange, void *);
static plsr_nsrange plsr_range_guard(void *self, void *cmd, void *search,
                                     unsigned long options, plsr_nsrange range,
                                     void *locale) {
    if (!search)
        return (plsr_nsrange){0x7fffffffffffffffUL, 0};
    if (!g_range_orig)
        return (plsr_nsrange){0x7fffffffffffffffUL, 0};
    return g_range_orig(self, cmd, search, options, range, locale);
}

static void install_string_range_guard(void) {
    void *(*getcls)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_objc_getClass");
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    void *(*getmeth)(void *, void *) =
        (void *(*)(void *, void *))plsr_image_symbol("_class_getInstanceMethod");
    void *(*getimp)(void *) =
        (void *(*)(void *))plsr_image_symbol("_method_getImplementation");
    const char *(*gettype)(void *) =
        (const char *(*)(void *))plsr_image_symbol("_method_getTypeEncoding");
    int (*replace)(void *, void *, void *, const char *) =
        (int (*)(void *, void *, void *, const char *))
            plsr_image_symbol("_class_replaceMethod");
    if (!getcls || !sel || !getmeth || !getimp || !gettype || !replace) return;
    void *selector = sel("rangeOfString:options:range:locale:");
    if (!selector) return;
    const char *classes[] = {
        "NSString", "NSMutableString", "__NSCFConstantString", "__NSCFString"
    };
    for (size_t i = 0; i < sizeof classes / sizeof classes[0]; i++) {
        void *cls = getcls(classes[i]);
        if (!cls) continue;
        void *m = getmeth(cls, selector);
        if (!m) continue;
        if (!g_range_orig) g_range_orig = (plsr_nsrange (*)(void *, void *, void *,
            unsigned long, plsr_nsrange, void *))getimp(m);
        const char *types = gettype(m);
        if (types) replace(cls, selector, (void *)plsr_range_guard, types);
    }
}

static void plsr_replace_super_init(void *cls, const char *name) {
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    void *(*getsuper)(void *) =
        (void *(*)(void *))plsr_image_symbol("_class_getSuperclass");
    void *(*getmeth)(void *, void *) =
        (void *(*)(void *, void *))plsr_image_symbol("_class_getInstanceMethod");
    void *(*getimp)(void *) =
        (void *(*)(void *))plsr_image_symbol("_method_getImplementation");
    const char *(*gettype)(void *) =
        (const char *(*)(void *))plsr_image_symbol("_method_getTypeEncoding");
    int (*replace)(void *, void *, void *, const char *) =
        (int (*)(void *, void *, void *, const char *))
            plsr_image_symbol("_class_replaceMethod");
    if (!cls || !name || !sel || !getsuper || !getmeth || !getimp ||
        !gettype || !replace) return;
    void *super = getsuper(cls);
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: replace init %s cls=%p super=%p\n", name, cls, super);
    if (!super) return;
    void *selector = sel(name);
    void *m = getmeth(super, selector);
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: replace init %s super method=%p\n", name, m);
    if (!m) return;
    const char *types = gettype(m);
    void *(*setimp)(void *, void *) =
        (void *(*)(void *, void *))plsr_image_symbol("_method_setImplementation");
    void *subm = getmeth(cls, selector);
    if (subm && setimp) {
        void *old = setimp(subm, getimp(m));
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: replace init %s old=%p new=%p\n",
                    name, old, getimp(m));
    } else if (types) {
        replace(cls, selector, getimp(m), types);
    }
    void *check = getmeth(cls, selector);
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: replace init %s effective=%p\n", name,
                check ? getimp(check) : NULL);
}

static void *plsr_super_imp(void *obj, const char *name) {
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    void *(*objclass)(void *) =
        (void *(*)(void *))plsr_image_symbol("_object_getClass");
    void *(*getsuper)(void *) =
        (void *(*)(void *))plsr_image_symbol("_class_getSuperclass");
    void *(*getmeth)(void *, void *) =
        (void *(*)(void *, void *))plsr_image_symbol("_class_getInstanceMethod");
    void *(*getimp)(void *) =
        (void *(*)(void *))plsr_image_symbol("_method_getImplementation");
    if (!obj || !name || !sel || !objclass || !getsuper || !getmeth || !getimp)
        return NULL;
    void *super = getsuper(objclass(obj));
    if (!super) return NULL;
    void *m = getmeth(super, sel(name));
    return m ? getimp(m) : NULL;
}

static void plsr_try_real_document_window(void) {
    static int tried;
    if (tried) return;
    tried = 1;
    void *(*getcls)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_objc_getClass");
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    void *(*msg0)(void *, void *) =
        (void *(*)(void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*msg1)(void *, void *, void *) =
        (void *(*)(void *, void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*msg2)(void *, void *, void *, void *) =
        (void *(*)(void *, void *, void *, void *))plsr_image_symbol("_objc_msgSend");
    signed char (*msg1b)(void *, void *, void *) =
        (signed char (*)(void *, void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*msg_rect)(void *, void *, plsr_nsrect, unsigned long, unsigned long,
                      unsigned char) =
        (void *(*)(void *, void *, plsr_nsrect, unsigned long, unsigned long,
                   unsigned char))plsr_image_symbol("_objc_msgSend");
    void *(*copy_methods)(void *, unsigned int *) =
        (void *(*)(void *, unsigned int *))plsr_image_symbol("_class_copyMethodList");
    void *(*method_name)(void *) =
        (void *(*)(void *))plsr_image_symbol("_method_getName");
    const char *(*sel_name)(void *) =
        (const char *(*)(void *))plsr_image_symbol("_sel_getName");
    if (!getcls || !sel || !msg0 || !msg1 || !msg_rect) return;
    const char *names[] = {
        "CotEditor.DocumentWindowController",
        "_TtC9CotEditor24DocumentWindowController"
    };
    void *cls = NULL;
    for (size_t i = 0; i < sizeof names / sizeof names[0] && !cls; i++)
        cls = getcls(names[i]);
    if (!cls) {
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: DocumentWindowController class not found\n");
        return;
    }
    if (getenv("PLSR_REEF_TRACE") && copy_methods && method_name && sel_name) {
        unsigned int mc = 0;
        void **ms = (void **)copy_methods(cls, &mc);
        for (unsigned int i = 0; ms && i < mc && i < 40; i++) {
            void *mn = method_name(ms[i]);
            const char *n = mn ? sel_name(mn) : NULL;
            if (n) fprintf(stderr, "reef: DWC method %s\n", n);
        }
        free(ms);
    }
    const char *vc_names[] = {
        "CotEditor.WindowContentViewController",
        "_TtC9CotEditor27WindowContentViewController",
        "CotEditor.DocumentViewController",
        "_TtC9CotEditor22DocumentViewController"
    };
    for (size_t vi = 0; vi < sizeof vc_names / sizeof vc_names[0]; vi++) {
        void *vcls = getcls(vc_names[vi]);
        if (!vcls) continue;
        if (getenv("PLSR_REEF_TRACE") && copy_methods && method_name && sel_name) {
            unsigned int mc = 0;
            void **ms = (void **)copy_methods(vcls, &mc);
            for (unsigned int i = 0; ms && i < mc && i < 20; i++) {
                void *mn = method_name(ms[i]);
                const char *n = mn ? sel_name(mn) : NULL;
                if (n) fprintf(stderr, "reef: %s method %s\n", vc_names[vi], n);
            }
            free(ms);
        }
        if (!msg1b || !msg1b(vcls, sel("instancesRespondToSelector:"),
                              sel("initWithNibName:bundle:")))
            continue;
        void *vc = msg2(msg0(vcls, sel("alloc")),
                        sel("initWithNibName:bundle:"), NULL, NULL);
        if (!vc) continue;
        void *view = msg0(vc, sel("view"));
        if (!view) {
            msg0(vc, sel("loadView"));
            view = msg0(vc, sel("view"));
        }
        if (!view) {
            if (getenv("PLSR_REEF_TRACE"))
                fprintf(stderr, "reef: %s loaded no view\n", vc_names[vi]);
            continue;
        }
        void *win = msg_rect(msg0(getcls("NSWindow"), sel("alloc")),
                             sel("initWithContentRect:styleMask:backing:defer:"),
                             (plsr_nsrect){0, 0, 900, 620}, 0xf, 2, 0);
        if (!win) continue;
        msg1(win, sel("setContentViewController:"), vc);
        void *title = msg1(getcls("NSString"), sel("stringWithUTF8String:"),
                           (void *)"CotEditor");
        if (title) msg1(win, sel("setTitle:"), title);
        msg1(win, sel("makeKeyAndOrderFront:"), NULL);
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: real content window win=%p vc=%p class=%s\n",
                    win, vc, vc_names[vi]);
        return;
    }
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: no real content view controller available\n");
}

static void plsr_try_real_editor_window(void) {
    static int tried;
    if (tried) return;
    tried = 1;
    void *(*getcls)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_objc_getClass");
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    void *(*msg0)(void *, void *) =
        (void *(*)(void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*msg1)(void *, void *, void *) =
        (void *(*)(void *, void *, void *))plsr_image_symbol("_objc_msgSend");
    signed char (*msg1b)(void *, void *, void *) =
        (signed char (*)(void *, void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*msg_rect)(void *, void *, plsr_nsrect, unsigned long, unsigned long,
                      unsigned char) =
        (void *(*)(void *, void *, plsr_nsrect, unsigned long, unsigned long,
                   unsigned char))plsr_image_symbol("_objc_msgSend");
    void *(*msg_rect1)(void *, void *, plsr_nsrect) =
        (void *(*)(void *, void *, plsr_nsrect))plsr_image_symbol("_objc_msgSend");
    if (!getcls || !sel || !msg0 || !msg1 || !msg1b || !msg_rect || !msg_rect1)
        return;
    const char *names[] = {
        "CotEditor.EditorTextView", "_TtC9CotEditor14EditorTextView"
    };
    void *cls = NULL;
    for (size_t i = 0; i < sizeof names / sizeof names[0] && !cls; i++)
        cls = getcls(names[i]);
    if (!cls) {
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: EditorTextView class not found\n");
        return;
    }
    if (!msg1b(cls, sel("instancesRespondToSelector:"), sel("initWithFrame:"))) {
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: EditorTextView has no initWithFrame:\n");
        return;
    }
    plsr_nsrect rect = {0, 0, 900, 620};
    void *tv = msg_rect1(msg0(cls, sel("alloc")), sel("initWithFrame:"), rect);
    if (!tv) return;
    void *empty = msg1(getcls("NSString"), sel("stringWithUTF8String:"), (void *)"");
    if (empty) msg1(tv, sel("setString:"), empty);
    msg1(tv, sel("setVerticallyResizable:"), (void *)1);
    msg1(tv, sel("setHorizontallyResizable:"), (void *)0);
    void *scroll = msg_rect1(msg0(getcls("NSScrollView"), sel("alloc")),
                             sel("initWithFrame:"), rect);
    if (!scroll) return;
    msg1(scroll, sel("setDocumentView:"), tv);
    msg1(scroll, sel("setHasVerticalScroller:"), (void *)1);
    msg1(scroll, sel("setHasHorizontalScroller:"), (void *)0);
    void *win = msg_rect(msg0(getcls("NSWindow"), sel("alloc")),
                         sel("initWithContentRect:styleMask:backing:defer:"),
                         rect, 0xf, 2, 0);
    if (!win) return;
    msg1(win, sel("setContentView:"), scroll);
    void *title = msg1(getcls("NSString"), sel("stringWithUTF8String:"),
                       (void *)"Untitled — CotEditor");
    if (title) msg1(win, sel("setTitle:"), title);
    msg1(win, sel("makeKeyAndOrderFront:"), NULL);
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: real EditorTextView window win=%p tv=%p\n", win, tv);
}

static void plsr_try_real_text_window(void) {
    static int tried;
    if (tried) return;
    tried = 1;
    void *(*getcls)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_objc_getClass");
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    void *(*msg0)(void *, void *) =
        (void *(*)(void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*msg1)(void *, void *, void *) =
        (void *(*)(void *, void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*msg_rect)(void *, void *, plsr_nsrect, unsigned long, unsigned long,
                      unsigned char) =
        (void *(*)(void *, void *, plsr_nsrect, unsigned long, unsigned long,
                   unsigned char))plsr_image_symbol("_objc_msgSend");
    void *(*msg_rect1)(void *, void *, plsr_nsrect) =
        (void *(*)(void *, void *, plsr_nsrect))plsr_image_symbol("_objc_msgSend");
    void *(*msg_rect2)(void *, void *, plsr_nsrect, void *) =
        (void *(*)(void *, void *, plsr_nsrect, void *))plsr_image_symbol("_objc_msgSend");
    void *(*msg_size)(void *, void *, plsr_nssize) =
        (void *(*)(void *, void *, plsr_nssize))plsr_image_symbol("_objc_msgSend");
    void *(*copy_methods)(void *, unsigned int *) =
        (void *(*)(void *, unsigned int *))plsr_image_symbol("_class_copyMethodList");
    void *(*method_name)(void *) =
        (void *(*)(void *))plsr_image_symbol("_method_getName");
    const char *(*sel_name)(void *) =
        (const char *(*)(void *))plsr_image_symbol("_sel_getName");
    if (!getcls || !sel || !msg0 || !msg1 || !msg_rect || !msg_rect1 ||
        !msg_rect2 || !msg_size) return;
    plsr_nsrect rect = {0, 0, 900, 620};
    const char *names[] = {
        "CotEditor.EditorTextView", "_TtC9CotEditor14EditorTextView"
    };
    void *cls = NULL;
    for (size_t i = 0; i < sizeof names / sizeof names[0] && !cls; i++)
        cls = getcls(names[i]);
    void *tv = NULL;
    int has_tc = 0;
    if (cls && copy_methods && method_name && sel_name) {
        unsigned int mc = 0;
        void **ms = (void **)copy_methods(cls, &mc);
        for (unsigned int i = 0; ms && i < mc; i++) {
            void *mn = method_name(ms[i]);
            const char *n = mn ? sel_name(mn) : NULL;
            if (!n) continue;
            if (getenv("PLSR_REEF_TRACE"))
                fprintf(stderr, "reef: EditorTextView method %s\n", n);
            if (strcmp(n, "initWithFrame:textContainer:") == 0) has_tc = 1;
        }
        free(ms);
    }
    if (0 && cls && has_tc) {
        void *layout = msg0(getcls("NSLayoutManager"), sel("new"));
        void *container = msg_size(msg0(getcls("NSTextContainer"), sel("alloc")),
                                   sel("initWithContainerSize:"),
                                   (plsr_nssize){900, 620});
        if (layout && container) {
            msg1(layout, sel("addTextContainer:"), container);
            tv = msg_rect2(msg0(cls, sel("alloc")),
                           sel("initWithFrame:textContainer:"), rect, container);
        }
    }
    if (!tv) {
        void *ntv = getcls("NSTextView");
        if (ntv)
            tv = msg_rect1(msg0(ntv, sel("alloc")), sel("initWithFrame:"), rect);
    }
    if (!tv) {
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: no usable text view class\n");
        return;
    }
    msg1(tv, sel("setVerticallyResizable:"), (void *)1);
    msg1(tv, sel("setHorizontallyResizable:"), (void *)0);
    void *scroll = msg_rect1(msg0(getcls("NSScrollView"), sel("alloc")),
                             sel("initWithFrame:"), rect);
    if (!scroll) return;
    msg1(scroll, sel("setDocumentView:"), tv);
    msg1(scroll, sel("setHasVerticalScroller:"), (void *)1);
    msg1(scroll, sel("setHasHorizontalScroller:"), (void *)0);
    void *win = msg_rect(msg0(getcls("NSWindow"), sel("alloc")),
                         sel("initWithContentRect:styleMask:backing:defer:"),
                         rect, 0xf, 2, 0);
    if (!win) return;
    msg1(win, sel("setContentView:"), scroll);
    void *title = msg1(getcls("NSString"), sel("stringWithUTF8String:"),
                       (void *)"Untitled — CotEditor");
    if (title) msg1(win, sel("setTitle:"), title);
    msg1(win, sel("makeKeyAndOrderFront:"), NULL);
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: real text document window win=%p tv=%p cls=%s\n",
                win, tv, cls && has_tc ? "EditorTextView" : "NSTextView");
}

static void plsr_try_document_field_window(void) {
    static int tried;
    if (tried) return;
    tried = 1;
    void *(*getcls)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_objc_getClass");
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    void *(*msg0)(void *, void *) =
        (void *(*)(void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*msg1)(void *, void *, void *) =
        (void *(*)(void *, void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*msg_rect)(void *, void *, plsr_nsrect, unsigned long, unsigned long,
                      unsigned char) =
        (void *(*)(void *, void *, plsr_nsrect, unsigned long, unsigned long,
                   unsigned char))plsr_image_symbol("_objc_msgSend");
    void *(*msg_rect1)(void *, void *, plsr_nsrect) =
        (void *(*)(void *, void *, plsr_nsrect))plsr_image_symbol("_objc_msgSend");
    if (!getcls || !sel || !msg0 || !msg1 || !msg_rect || !msg_rect1) return;
    plsr_nsrect rect = {0, 0, 900, 620};
    void *field = msg_rect1(msg0(getcls("NSTextField"), sel("alloc")),
                            sel("initWithFrame:"), rect);
    if (!field) return;
    msg1(field, sel("setEditable:"), (void *)1);
    msg1(field, sel("setSelectable:"), (void *)1);
    msg1(field, sel("setBezeled:"), (void *)0);
    msg1(field, sel("setDrawsBackground:"), (void *)1);
    void *win = msg_rect(msg0(getcls("NSWindow"), sel("alloc")),
                         sel("initWithContentRect:styleMask:backing:defer:"),
                         rect, 0xf, 2, 0);
    if (!win) return;
    msg1(win, sel("setContentView:"), field);
    void *title = msg1(getcls("NSString"), sel("stringWithUTF8String:"),
                       (void *)"Untitled — CotEditor");
    if (title) msg1(win, sel("setTitle:"), title);
    msg1(win, sel("makeKeyAndOrderFront:"), NULL);
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: real editable document window win=%p field=%p\n", win, field);
}

typedef struct { double x, y; } plsr_nspoint;
static void *plsr_coder_nil0(void *s, void *c) { (void)s; (void)c; return NULL; }
static void *plsr_coder_nil1(void *s, void *c, void *a) {
    (void)s; (void)c; (void)a; return NULL;
}
static void *plsr_coder_nil2(void *s, void *c, void *a, void *b) {
    (void)s; (void)c; (void)a; (void)b; return NULL;
}
static signed char plsr_coder_false0(void *s, void *c) { (void)s; (void)c; return 0; }
static signed char plsr_coder_false1(void *s, void *c, void *a) {
    (void)s; (void)c; (void)a; return 0;
}
static signed char plsr_coder_true0(void *s, void *c) { (void)s; (void)c; return 1; }
static long plsr_coder_zero1(void *s, void *c, void *a) {
    (void)s; (void)c; (void)a; return 0;
}
static double plsr_coder_double1(void *s, void *c, void *a) {
    (void)s; (void)c; (void)a; return 0.0;
}
static float plsr_coder_float1(void *s, void *c, void *a) {
    (void)s; (void)c; (void)a; return 0.0f;
}
static plsr_nsrect plsr_coder_rect1(void *s, void *c, void *a) {
    (void)s; (void)c; (void)a; return (plsr_nsrect){0, 0, 0, 0};
}
static plsr_nssize plsr_coder_size1(void *s, void *c, void *a) {
    (void)s; (void)c; (void)a; return (plsr_nssize){0, 0};
}
static plsr_nspoint plsr_coder_point1(void *s, void *c, void *a) {
    (void)s; (void)c; (void)a; return (plsr_nspoint){0, 0};
}
static const void *plsr_coder_bytes(void *s, void *c, void *a, unsigned long *len) {
    (void)s; (void)c; (void)a; if (len) *len = 0; return NULL;
}
static void plsr_coder_value(void *s, void *c, const char *type, void *addr) {
    (void)s; (void)c; (void)type; if (addr) memset(addr, 0, 16);
}
static void plsr_coder_array(void *s, void *c, const char *type, long count, void *addr) {
    (void)s; (void)c; (void)type; (void)count; (void)addr;
}

static void *plsr_make_coder(void) {
    void *(*getcls)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_objc_getClass");
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    void *(*msg0)(void *, void *) =
        (void *(*)(void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*allocate)(void *, const char *, size_t) =
        (void *(*)(void *, const char *, size_t))plsr_image_symbol("_objc_allocateClassPair");
    signed char (*add)(void *, void *, void *, const char *) =
        (signed char (*)(void *, void *, void *, const char *))plsr_image_symbol("_class_addMethod");
    void (*reg)(void *) =
        (void (*)(void *))plsr_image_symbol("_objc_registerClassPair");
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: coder make getcls=%p sel=%p allocate=%p add=%p reg=%p\n",
                getcls, sel, allocate, add, reg);
    if (!getcls || !sel || !msg0 || !allocate || !add || !reg) return NULL;
    static void *cls;
    if (!cls) {
        void *super = getcls("NSCoder");
        if (!super) return NULL;
        cls = allocate(super, "PLSRCoder", 0);
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: coder make super=%p cls=%p\n", super, cls);
        if (!cls) cls = getcls("PLSRCoder");
        if (!cls) return NULL;
        add(cls, sel("decodeObjectForKey:"), (void *)plsr_coder_nil1, "@@:@");
        add(cls, sel("decodeObjectOfClass:forKey:"), (void *)plsr_coder_nil2, "@@:@@");
        add(cls, sel("decodeObjectOfClasses:forKey:"), (void *)plsr_coder_nil2, "@@:@@");
        add(cls, sel("decodePropertyListForKey:"), (void *)plsr_coder_nil1, "@@:@");
        add(cls, sel("decodeObject"), (void *)plsr_coder_nil0, "@@:");
        add(cls, sel("decodeBoolForKey:"), (void *)plsr_coder_false1, "c@:@");
        add(cls, sel("decodeIntegerForKey:"), (void *)plsr_coder_zero1, "q@:@");
        add(cls, sel("decodeIntForKey:"), (void *)plsr_coder_zero1, "i@:@");
        add(cls, sel("decodeDoubleForKey:"), (void *)plsr_coder_double1, "d@:@");
        add(cls, sel("decodeFloatForKey:"), (void *)plsr_coder_float1, "f@:@");
        add(cls, sel("decodeRectForKey:"), (void *)plsr_coder_rect1,
            "{CGRect={CGPoint=dd}{CGSize=dd}}@:@");
        add(cls, sel("decodeSizeForKey:"), (void *)plsr_coder_size1, "{CGSize=dd}@:@");
        add(cls, sel("decodePointForKey:"), (void *)plsr_coder_point1, "{CGPoint=dd}@:@");
        add(cls, sel("decodeBytesForKey:returnedLength:"), (void *)plsr_coder_bytes, "*@:@^Q");
        add(cls, sel("containsValueForKey:"), (void *)plsr_coder_false1, "c@:@");
        add(cls, sel("allowsKeyedCoding"), (void *)plsr_coder_true0, "c@:");
        add(cls, sel("requiresSecureCoding"), (void *)plsr_coder_false0, "c@:");
        add(cls, sel("versionForClassName:"), (void *)plsr_coder_zero1, "q@:@");
        add(cls, sel("decodeValueOfObjCType:at:"), (void *)plsr_coder_value, "v@:*^v");
        add(cls, sel("decodeArrayOfObjCType:count:at:"), (void *)plsr_coder_array, "v@:*q^v");
        reg(cls);
    }
    return msg0(msg0(cls, sel("alloc")), sel("init"));
}

/* ---- document text ------------------------------------------------------
 * The runtime's NSTextStorage/NSTextView keep no characters, so -string and
 * -setString: are answered here, behind the same selectors the app uses.
 * The document text therefore round-trips: a host edit reaches the text
 * view's storage, and the app reads the same characters back. */

#define PLSR_TEXT_SLOTS 64
struct plsr_text_slot {
    void *owner;
    char *text;
    size_t len;
};
static struct plsr_text_slot g_text_slots[PLSR_TEXT_SLOTS];
static pthread_mutex_t g_text_lock = PTHREAD_MUTEX_INITIALIZER;

static struct plsr_text_slot *plsr_text_slot(void *owner, int create) {
    if (!owner) return NULL;
    struct plsr_text_slot *slot = NULL, *free_slot = NULL;
    pthread_mutex_lock(&g_text_lock);
    for (int i = 0; i < PLSR_TEXT_SLOTS; i++) {
        if (g_text_slots[i].owner == owner) {
            slot = &g_text_slots[i];
            break;
        }
        if (!g_text_slots[i].owner && !free_slot) free_slot = &g_text_slots[i];
    }
    if (!slot && create && free_slot) {
        free_slot->owner = owner;
        slot = free_slot;
    }
    pthread_mutex_unlock(&g_text_lock);
    return slot;
}

static void plsr_text_set(void *owner, const char *s) {
    struct plsr_text_slot *slot = plsr_text_slot(owner, 1);
    if (!slot) return;
    char *copy = strdup(s ? s : "");
    if (!copy) return;
    free(slot->text);
    slot->text = copy;
    slot->len = strlen(copy);
}

static const char *plsr_text_get(void *owner) {
    struct plsr_text_slot *slot = plsr_text_slot(owner, 0);
    return (slot && slot->text) ? slot->text : "";
}

static void *plsr_mk_nsstring(const char *s) {
    /* +[NSString stringWithUTF8String:] returns nil on this runtime, so use
     * the shared constructor, which falls back to a toll-free CFString. */
    return plsr_nsstring(s ? s : "");
}

/* Methods we displaced; objects without a text slot fall through to them so
 * unrelated storages keep the runtime's own behaviour. */
static void *g_orig_storage_string;
static void *g_orig_storage_set;
static void *g_orig_storage_len;
static void *g_orig_tv_string;
static void *g_orig_tv_set;
static void *g_orig_tv_insert;

static void *plsr_text_string_imp(void *self, void *cmd) {
    if (!plsr_text_slot(self, 0) && g_orig_storage_string &&
        g_orig_storage_string != (void *)plsr_text_string_imp)
        return ((void *(*)(void *, void *))g_orig_storage_string)(self, cmd);
    return plsr_mk_nsstring(plsr_text_get(self));
}

static void plsr_text_setstring_imp(void *self, void *cmd, void *value) {
    (void)cmd;
    /* The first -setString: is what creates the slot, so it must store here
     * instead of being forwarded to the runtime's no-op setter. */
    const char *u = plsr_nsutf8(value);
    plsr_text_set(self, u ? u : "");
}

static unsigned long plsr_text_length_imp(void *self, void *cmd) {
    if (!plsr_text_slot(self, 0) && g_orig_storage_len &&
        g_orig_storage_len != (void *)plsr_text_length_imp)
        return ((unsigned long (*)(void *, void *))g_orig_storage_len)(self, cmd);
    return (unsigned long)strlen(plsr_text_get(self));
}

/* A text view owns its storage, so -string/-setString: forward to it. */
static void *plsr_text_owner_of(void *self) {
    if (!plsr_widget_api() || !self) return self;
    void *storage = g_w_m0(self, g_w_sel("textStorage"));
    return storage ? storage : self;
}

static void *plsr_tv_string_imp(void *self, void *cmd) {
    if (!plsr_text_slot(plsr_text_owner_of(self), 0) && g_orig_tv_string &&
        g_orig_tv_string != (void *)plsr_tv_string_imp)
        return ((void *(*)(void *, void *))g_orig_tv_string)(self, cmd);
    return plsr_mk_nsstring(plsr_text_get(plsr_text_owner_of(self)));
}

static void plsr_tv_setstring_imp(void *self, void *cmd, void *value) {
    (void)cmd;
    const char *u = plsr_nsutf8(value);
    plsr_text_set(plsr_text_owner_of(self), u ? u : "");
}

static void plsr_tv_insert_imp(void *self, void *cmd, void *value) {
    (void)cmd;
    void *owner = plsr_text_owner_of(self);
    const char *old = plsr_text_get(owner);
    const char *add = plsr_nsutf8(value);
    size_t n = strlen(old), m = add ? strlen(add) : 0;
    char *buf = malloc(n + m + 1);
    if (!buf) return;
    memcpy(buf, old, n);
    if (m) memcpy(buf + n, add, m);
    buf[n + m] = 0;
    plsr_text_set(owner, buf);
    free(buf);
}

/* Storage instances are concrete subclasses of NSTextStorage, so the text
 * methods are installed on the class the object actually has. */
static int plsr_text_store_enabled(void) {
    const char *explicit_value = getenv("PLSR_TEXT_STORE");
    return !getenv("PLSR_DISABLE_TEXT_STORE") &&
        (!explicit_value || strcmp(explicit_value, "0") != 0);
}

static void plsr_install_text_store_for(void *object) {
    void *(*objcls)(void *) =
        (void *(*)(void *))plsr_image_symbol("_object_getClass");
    void *(*replace)(void *, void *, void *, const char *) =
        (void *(*)(void *, void *, void *, const char *))
            plsr_image_symbol("_class_replaceMethod");
    if (!object || !objcls || !replace || !plsr_widget_api()) return;
    void *cls = objcls(object);
    if (!cls) return;
    void *(*getimp)(void *, void *) =
        (void *(*)(void *, void *))plsr_image_symbol("_class_getMethodImplementation");
    if (getimp) {
        void *imp = getimp(cls, g_w_sel("string"));
        if (imp != (void *)plsr_text_string_imp) g_orig_storage_string = imp;
        imp = getimp(cls, g_w_sel("setString:"));
        if (imp != (void *)plsr_text_setstring_imp) g_orig_storage_set = imp;
        imp = getimp(cls, g_w_sel("length"));
        if (imp != (void *)plsr_text_length_imp) g_orig_storage_len = imp;
    }
    replace(cls, g_w_sel("string"), (void *)plsr_text_string_imp, "@@:");
    replace(cls, g_w_sel("setString:"), (void *)plsr_text_setstring_imp,
            "v@:@");
    replace(cls, g_w_sel("length"), (void *)plsr_text_length_imp, "Q@:");
    /* This libobjc exports the flush as _flushCaches (no _objc_ prefix);
     * without it the replaced methods stay shadowed by stale cache buckets. */
    void (*flush)(void *) =
        (void (*)(void *))plsr_image_symbol("_objc_flush_caches");
    if (!flush) flush = (void (*)(void *))plsr_image_symbol("_flushCaches");
    if (flush) flush(cls);
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr,
                "reef: text store on %s=%p string=%p/%p set=%p/%p flush=%p\n",
                g_w_clsname(object), cls,
                getimp ? getimp(cls, g_w_sel("string")) : NULL,
                (void *)plsr_text_string_imp,
                getimp ? getimp(cls, g_w_sel("setString:")) : NULL,
                (void *)plsr_text_setstring_imp, (void *)flush);
}

static void plsr_install_text_view_for(void *object) {
    void *(*objcls)(void *) =
        (void *(*)(void *))plsr_image_symbol("_object_getClass");
    void *(*replace)(void *, void *, void *, const char *) =
        (void *(*)(void *, void *, void *, const char *))
            plsr_image_symbol("_class_replaceMethod");
    if (!object || !objcls || !replace || !plsr_widget_api()) return;
    void *cls = objcls(object);
    if (!cls) return;
    void *(*getimp)(void *, void *) =
        (void *(*)(void *, void *))plsr_image_symbol("_class_getMethodImplementation");
    if (getimp) {
        void *imp = getimp(cls, g_w_sel("string"));
        if (imp != (void *)plsr_tv_string_imp) g_orig_tv_string = imp;
        imp = getimp(cls, g_w_sel("setString:"));
        if (imp != (void *)plsr_tv_setstring_imp) g_orig_tv_set = imp;
        imp = getimp(cls, g_w_sel("insertText:"));
        if (imp != (void *)plsr_tv_insert_imp) g_orig_tv_insert = imp;
    }
    replace(cls, g_w_sel("string"), (void *)plsr_tv_string_imp, "@@:");
    replace(cls, g_w_sel("setString:"), (void *)plsr_tv_setstring_imp, "v@:@");
    replace(cls, g_w_sel("insertText:"), (void *)plsr_tv_insert_imp, "v@:@");
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: text view store on %s=%p\n",
                g_w_clsname(object), cls);
}

/* Dump the ObjC surface of a Swift class.  A Swift class is only reachable
 * from objc_msgSend through its ObjC method lists, so this shows which
 * initializers the runtime can actually invoke by selector. */
static void plsr_dump_class_methods(const char *name) {
    void *(*getcls)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_objc_getClass");
    void *(*objcls)(void *) =
        (void *(*)(void *))plsr_image_symbol("_object_getClass");
    void *(*copylist)(void *, unsigned int *) =
        (void *(*)(void *, unsigned int *))plsr_image_symbol("_class_copyMethodList");
    void *(*mname)(void *) =
        (void *(*)(void *))plsr_image_symbol("_method_getName");
    const char *(*sname)(void *) =
        (const char *(*)(void *))plsr_image_symbol("_sel_getName");
    const char *(*gettype)(void *) =
        (const char *(*)(void *))plsr_image_symbol("_method_getTypeEncoding");
    void *(*getimp)(void *) =
        (void *(*)(void *))plsr_image_symbol("_method_getImplementation");
    if (!getcls || !copylist) return;
    void *cls = getcls(name);
    if (!cls) {
        fprintf(stderr, "reef: dump %s = (no class)\n", name);
        return;
    }
    for (int meta = 0; meta < 2; meta++) {
        void *owner = meta && objcls ? objcls(cls) : cls;
        unsigned int n = 0;
        void **list = copylist(owner, &n);
        fprintf(stderr, "reef: dump %s %s=%p methods=%u\n", name,
                meta ? "meta" : "cls", owner, n);
        for (unsigned int i = 0; list && i < n && i < 48; i++) {
            void *sel = mname ? mname(list[i]) : NULL;
            const char *sn = (sel && sname) ? sname(sel) : "?";
            const char *tp = gettype ? gettype(list[i]) : NULL;
            fprintf(stderr, "reef:   %c[%s %s] %s imp=%p\n",
                    meta ? '+' : '-', name, sn, tp ? tp : "?",
                    getimp ? getimp(list[i]) : NULL);
        }
    }
}

static void *g_app_document;

/* Ask the application's own NSDocumentController to create a document.  This
 * is the same call AppKit makes for File > New and for the untitled document
 * it opens at launch; the document and its window controller (and therefore
 * the toolbar, inspector and status bar) come from the shipped app code, not
 * from this loader.  Gated by PLSR_NEW_DOCUMENT while the surrounding launch
 * path is being brought up. */
static void plsr_new_app_document(void) {
    void *(*getcls)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_objc_getClass");
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    void *(*msg0)(void *, void *) =
        (void *(*)(void *, void *))plsr_image_symbol("_objc_msgSend");
    void (*msg1)(void *, void *, void *) =
        (void (*)(void *, void *, void *))plsr_image_symbol("_objc_msgSend");
    if (!getcls || !sel || !msg0 || !msg1) return;
    /* CotEditor ships its own subclass; prefer it so the app's overrides
     * (custom document types, defaults) run.  Fall back to AppKit's. */
    void *ctl = NULL;
    const char *names[] = {
        "CotEditor.DocumentController", "_TtC9CotEditor18DocumentController",
        "NSDocumentController"
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0] && !ctl; i++) {
        void *cls = getcls(names[i]);
        if (!cls) continue;
        ctl = msg0(cls, sel("sharedDocumentController"));
        if (ctl) {
            if (getenv("PLSR_REEF_TRACE"))
                fprintf(stderr, "reef: document controller %s -> %p\n",
                        names[i], ctl);
        }
    }
    if (!ctl) {
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: no document controller available\n");
        return;
    }
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: calling -newDocument:\n");
    msg1(ctl, sel("newDocument:"), NULL);
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: -newDocument: returned\n");
}

static void plsr_try_app_document_window(void) {
    static int tried;
    if (tried) return;
    tried = 1;
    void *(*getcls)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_objc_getClass");
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    void *(*msg0)(void *, void *) =
        (void *(*)(void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*msg1)(void *, void *, void *) =
        (void *(*)(void *, void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*msg2)(void *, void *, void *, void *) =
        (void *(*)(void *, void *, void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*msg_rect)(void *, void *, plsr_nsrect, unsigned long, unsigned long,
                      unsigned char) =
        (void *(*)(void *, void *, plsr_nsrect, unsigned long, unsigned long,
                   unsigned char))plsr_image_symbol("_objc_msgSend");
    void *(*msg_rect1)(void *, void *, plsr_nsrect) =
        (void *(*)(void *, void *, plsr_nsrect))plsr_image_symbol("_objc_msgSend");
    void *(*msg_rect2)(void *, void *, plsr_nsrect, void *) =
        (void *(*)(void *, void *, plsr_nsrect, void *))plsr_image_symbol("_objc_msgSend");
    void *(*msg_size)(void *, void *, plsr_nssize) =
        (void *(*)(void *, void *, plsr_nssize))plsr_image_symbol("_objc_msgSend");
    if (!getcls || !sel || !msg0 || !msg1 || !msg2 || !msg_rect || !msg_rect1 ||
        !msg_rect2 || !msg_size) return;
    plsr_nsrect rect = {0, 0, 900, 620};
    if (getenv("PLSR_REEF_TRACE")) fprintf(stderr, "reef: app window begin\n");
    if (getenv("PLSR_TRACE_DOC_INIT")) {
        void *(*getimp)(void *, void *) =
            (void *(*)(void *, void *))plsr_image_symbol("_class_getMethodImplementation");
        void *doc_cls = getcls("CotEditor.Document");
        if (!doc_cls) doc_cls = getcls("_TtC9CotEditor8Document");
        void *doc_init = (doc_cls && getimp)
            ? getimp(doc_cls, sel("init")) : NULL;
        void *line_cls = getcls("CotEditor.LineEndingScanner");
        if (!line_cls) line_cls = getcls("_TtC9CotEditor17LineEndingScanner");
        uintptr_t base = plsr_image_base("CotEditor");
        fprintf(stderr,
                "reef: doc cls=%p init=%p off=%lx line_cls=%p line_off=%lx\n",
                doc_cls, doc_init,
                doc_init ? (unsigned long)((char *)doc_init - (char *)base) : 0,
                line_cls,
                line_cls ? (unsigned long)((char *)line_cls - (char *)base) : 0);
        if (getenv("PLSR_TRACE_DWC_ONLY")) {
            plsr_dump_class_methods("CotEditor.DocumentWindowController");
        } else {
            plsr_dump_class_methods("CotEditor.LineEndingScanner");
            plsr_dump_class_methods("CotEditor.Document");
            plsr_dump_class_methods("CotEditor.WindowContentViewController");
            plsr_dump_class_methods("CotEditor.EditorTextView");
            plsr_dump_class_methods("CotEditor.DocumentWindowController");
            plsr_dump_class_methods("CotEditor.MainWindowController");
            plsr_dump_class_methods("NSToolbar");
            plsr_dump_class_methods("NSTextStorage");
            plsr_dump_class_methods("NSTextView");
            plsr_dump_class_methods("NSButton");
            plsr_dump_class_methods("NSTextField");
        }
    }
    if (getenv("PLSR_REAL_DOCUMENT") || getenv("PLSR_TRACE_DOCUMENT")) {
        const char *doc_names[] = {
            "CotEditor.Document", "_TtC9CotEditor8Document"
        };
        void *doc_cls = NULL;
        for (size_t i = 0; i < sizeof doc_names / sizeof doc_names[0] && !doc_cls; i++)
            doc_cls = getcls(doc_names[i]);
        /* -[Document init] is an ObjC thunk whose Swift body is at +0x37c70.
         * Call that body directly with self in r13, as for EditorTextView:
         * the thunk's msgSend path is what the gate had to drop. */
        void *(*create)(void *, size_t) =
            (void *(*)(void *, size_t))plsr_image_symbol("_class_createInstance");
        void *self = (doc_cls && create) ? create(doc_cls, 0) : NULL;
        if (self) *(void **)self = doc_cls;
        void *doc = NULL;
        if (self && doc_cls) {
            void *fn = (void *)(plsr_image_base("CotEditor") + 0x37c70);
            doc = plsr_etv_init(fn, self, NULL, NULL);
        }
        g_app_document = doc ? doc : self;
        fprintf(stderr, "reef: Document cls=%p self=%p doc=%p\n", doc_cls, self,
                doc);
        if (doc) {
            void *key_storage = msg1(getcls("NSString"), sel("stringWithUTF8String:"),
                                     (void *)"textStorage");
            void *key_scanner = msg1(getcls("NSString"), sel("stringWithUTF8String:"),
                                     (void *)"lineEndingScanner");
            void *storage = key_storage ? msg1(doc, sel("valueForKey:"), key_storage) : NULL;
            void *scanner = key_scanner ? msg1(doc, sel("valueForKey:"), key_scanner) : NULL;
            fprintf(stderr, "reef: Document storage=%p scanner=%p\n", storage, scanner);
        }
    }
    const char *(*objclsname)(const void *) =
        (const char *(*)(const void *))plsr_image_symbol("_object_getClassName");
    if (getenv("PLSR_TRACE_ETVC")) {
        void *(*getimp)(void *, void *) =
            (void *(*)(void *, void *))plsr_image_symbol("_class_getMethodImplementation");
        void *base = (void *)plsr_image_base("CotEditor");
        void *etvc = getcls("CotEditor.EditorTextViewController");
        void *etv = getcls("CotEditor.EditorTextView");
        void *load = (etvc && getimp) ? getimp(etvc, sel("loadView")) : NULL;
        void *coder = (etv && getimp) ? getimp(etv, sel("initWithCoder:")) : NULL;
        void *frame = (etv && getimp) ? getimp(etv, sel("initWithFrame:textContainer:")) : NULL;
        fprintf(stderr, "reef: ETVC cls=%p loadView=%p off=%lx base=%p\n",
                etvc, load, (load && base) ? (unsigned long)((char *)load - (char *)base) : 0,
                base);
        fprintf(stderr, "reef: ETV cls=%p coder=%p off=%lx frame=%p off=%lx\n",
                etv, coder, (coder && base) ? (unsigned long)((char *)coder - (char *)base) : 0,
                frame, (frame && base) ? (unsigned long)((char *)frame - (char *)base) : 0);
    }
    void *tv = NULL;
    if (!getenv("PLSR_NO_REAL_EDITOR")) plsr_patch_swift_alloc();
    if (!getenv("PLSR_NO_REAL_EDITOR")) {
        void *etv_cls = getcls("CotEditor.EditorTextView");
        void *etv_mangled = getcls("_TtC9CotEditor14EditorTextView");
        if (!etv_cls) etv_cls = etv_mangled;
        void *storage = msg0(msg0(getcls("NSTextStorage"), sel("alloc")),
                             sel("init"));
        if (storage && plsr_text_store_enabled())
            plsr_install_text_store_for(storage);
        void *(*create)(void *, size_t) =
            (void *(*)(void *, size_t))plsr_image_symbol("_class_createInstance");
        void *self = (etv_cls && create) ? create(etv_cls, 0) : NULL;
        if (self && etv_cls) *(void **)self = etv_cls;
        plsr_replace_super_method(etv_cls, "setTypingAttributes:");
        void *(*replace_m)(void *, void *, void *, const char *) =
            (void *(*)(void *, void *, void *, const char *))
                plsr_image_symbol("_class_replaceMethod");
        void *set_sel = sel("setTypingAttributes:");
        if (replace_m && set_sel)
            replace_m(etv_cls, set_sel, (void *)plsr_noop_typing_attributes,
                      "v@:@");
        void *font_sel = sel("font");
        if (replace_m && font_sel)
            replace_m(etv_cls, font_sel, (void *)plsr_etv_safe_font, "@@:");
        if (!getenv("PLSR_NO_SWIFT_EDITOR")) {
            plsr_patch_etv_tail();
            void *fn = (void *)(plsr_image_base("CotEditor") + 0x149d70);
            if (storage && self && fn)
                tv = plsr_etv_init(fn, self, storage, NULL);
            if (getenv("PLSR_REEF_TRACE"))
                fprintf(stderr, "reef: swift editor init tv=%p\n", tv);
        }
        void *(*getimp)(void *, void *) =
            (void *(*)(void *, void *))plsr_image_symbol("_class_getMethodImplementation");
        void *(*super_init)(void *, void *, plsr_nsrect, void *) = NULL;
        void *layout = msg0(getcls("NSLayoutManager"), sel("new"));
        void *container = msg_size(msg0(getcls("NSTextContainer"), sel("alloc")),
                                   sel("initWithContainerSize:"),
                                   (plsr_nssize){900, 620});
        if (getimp) {
            void *ntv = getcls("NSTextView");
            super_init = (void *(*)(void *, void *, plsr_nsrect, void *))
                (ntv ? getimp(ntv, sel("initWithFrame:textContainer:")) : NULL);
        }
        if (storage && layout) msg1(storage, sel("addLayoutManager:"), layout);
        if (layout && container) msg1(layout, sel("addTextContainer:"), container);
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: real editor before cls=%p mangled=%p self=%p isa=%p storage=%p layout=%p container=%p superinit=%p\n",
                    etv_cls, etv_mangled, self, self ? *(void **)self : NULL,
                    storage, layout, container, super_init);
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: classes tc=%p lm=%p font=%p nstv=%p\n",
                    getcls("CotEditor.TextContainer"),
                    getcls("CotEditor.LayoutManager"),
                    getcls("NSFont"), getcls("NSTextView"));
        if (!tv) {
            void *ntv = getcls("NSTextView");
            if (ntv) {
                tv = msg_rect1(msg0(ntv, sel("alloc")), sel("initWithFrame:"), rect);
                if (tv) {
                    void *str = msg1(getcls("NSString"), sel("stringWithUTF8String:"),
                                     (void *)"CotEditor");
                    if (str) msg1(tv, sel("setString:"), str);
                }
            }
        }
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: real editor cls=%p storage=%p self=%p tv=%p\n",
                    etv_cls, storage, self, tv);
    }
    if (getenv("PLSR_CODER_EDITOR")) {
        void *data = NULL;
        void *(*cfcreate)(void *, const void *, long) =
            (void *(*)(void *, const void *, long))plsr_image_symbol("_CFDataCreate");
        FILE *f = fopen("/tmp/empty.keyedarchive", "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            rewind(f);
            if (n > 0) {
                void *buf = malloc((size_t)n);
                if (buf && cfcreate && fread(buf, 1, (size_t)n, f) == (size_t)n)
                    data = cfcreate(NULL, buf, n);
                free(buf);
            }
            fclose(f);
        }
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: coder classes data=%p unarch=%p bytes=%p\n",
                    getcls("NSData"), getcls("NSKeyedUnarchiver"), data);
        void *coder = data
            ? msg2(msg0(getcls("NSKeyedUnarchiver"), sel("alloc")),
                   sel("initForReadingWithData:"), data, NULL)
            : NULL;
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: coder file data=%p coder=%p\n", data, coder);
        void *editor_cls = getcls("CotEditor.EditorTextView");
        if (!editor_cls) editor_cls = getcls("_TtC9CotEditor14EditorTextView");
        if (editor_cls && coder)
            tv = msg1(msg0(editor_cls, sel("alloc")), sel("initWithCoder:"), coder);
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: coder editor=%p cls=%s\n",
                    tv, tv && objclsname ? objclsname(tv) : "(nil)");
    }
    if (getenv("PLSR_ARCHIVE_EDITOR")) {
        void *ntv = getcls("NSTextView");
        void *base = ntv
            ? msg_rect1(msg0(ntv, sel("alloc")), sel("initWithFrame:"), rect)
            : NULL;
        void *data = base
            ? msg1(getcls("NSKeyedArchiver"),
                   sel("archivedDataWithRootObject:"), base)
            : NULL;
        void *editor_cls = getcls("CotEditor.EditorTextView");
        if (!editor_cls) editor_cls = getcls("_TtC9CotEditor14EditorTextView");
        void *ucls = getcls("NSKeyedUnarchiver");
        void *name = msg1(getcls("NSString"), sel("stringWithUTF8String:"),
                          (void *)"NSTextView");
        if (data && editor_cls && ucls && name) {
            msg2(ucls, sel("setClass:forClassName:"), editor_cls, name);
            tv = msg1(ucls, sel("unarchiveObjectWithData:"), data);
            if (getenv("PLSR_REEF_TRACE"))
                fprintf(stderr, "reef: archive editor=%p cls=%s\n", tv,
                        tv && objclsname ? objclsname(tv) : "(nil)");
        }
    }
    if (!tv && !getenv("PLSR_USE_EDITOR_TEXTVIEW")) {
        void *ntv = getcls("NSTextView");
        if (ntv)
            tv = msg_rect1(msg0(ntv, sel("alloc")), sel("initWithFrame:"), rect);
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: app window native NSTextView=%p\n", tv);
    }
    const char *tv_names[] = {
        "CotEditor.EditorTextView", "_TtC9CotEditor14EditorTextView"
    };
    for (size_t i = 0; i < sizeof tv_names / sizeof tv_names[0] && !tv; i++) {
        void *cls = getcls(tv_names[i]);
        if (!cls) continue;
        plsr_replace_super_init(cls, "initWithFrame:");
        plsr_replace_super_init(cls, "initWithFrame:textContainer:");
        void *layout = msg0(getcls("NSLayoutManager"), sel("new"));
        if (getenv("PLSR_REEF_TRACE")) fprintf(stderr, "reef: app window layout=%p\n", layout);
        void *container = msg_size(msg0(getcls("NSTextContainer"), sel("alloc")),
                                   sel("initWithContainerSize:"),
                                   (plsr_nssize){900, 620});
        if (getenv("PLSR_REEF_TRACE")) fprintf(stderr, "reef: app window container=%p\n", container);
        if (layout && container)
            msg1(layout, sel("addTextContainer:"), container);
        if (getenv("PLSR_REEF_TRACE")) fprintf(stderr, "reef: app window init text view\n");
        void *alloc = msg0(cls, sel("alloc"));
        if (getenv("PLSR_REEF_TRACE")) fprintf(stderr, "reef: app window alloc=%p\n", alloc);
        if (container && alloc)
            tv = msg_rect2(alloc, sel("initWithFrame:textContainer:"), rect, container);
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: app window tv=%p class=%s\n", tv, tv_names[i]);
    }
    if (!tv) {
        void *ntv = getcls("NSTextView");
        if (ntv)
            tv = msg_rect1(msg0(ntv, sel("alloc")), sel("initWithFrame:"), rect);
    }
    if (!tv) {
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: no real text view for document window\n");
        return;
    }
    if (plsr_text_store_enabled()) plsr_install_text_view_for(tv);
    msg1(tv, sel("setVerticallyResizable:"), (void *)1);
    msg1(tv, sel("setHorizontallyResizable:"), (void *)0);
    void *scroll = msg_rect1(msg0(getcls("NSScrollView"), sel("alloc")),
                             sel("initWithFrame:"), rect);
    if (!scroll) return;
    msg1(scroll, sel("setDocumentView:"), tv);
    msg1(scroll, sel("setHasVerticalScroller:"), (void *)1);
    msg1(scroll, sel("setHasHorizontalScroller:"), (void *)0);
    void *vc = NULL;
    void *vc_cls = NULL;
    const char *vc_names[] = {
        "CotEditor.WindowContentViewController",
        "_TtC9CotEditor27WindowContentViewController"
    };
    for (size_t i = 0; i < sizeof vc_names / sizeof vc_names[0] && !vc_cls; i++)
        vc_cls = getcls(vc_names[i]);
    if (vc_cls) {
        /* The Swift class does not implement -initWithNibName:bundle: (the
         * inherited entry is Swift's unimplemented-initializer trap), and its
         * real initializer is init(document:directoryDocument:), which needs
         * a Document.  Until that exists, allocate through NSViewController
         * and adopt the shipped class so the instance still carries the real
         * class and its ObjC method table. */
        void *nsvc = getcls("NSViewController");
        if (nsvc) {
            vc = msg2(msg0(nsvc, sel("alloc")),
                      sel("initWithNibName:bundle:"), NULL, NULL);
            if (vc) {
                msg1(vc, sel("setView:"), scroll);
                *(void **)vc = vc_cls;
            }
        }
    }
    void *win = msg_rect(msg0(getcls("NSWindow"), sel("alloc")),
                         sel("initWithContentRect:styleMask:backing:defer:"),
                         rect, 0xf, 2, 0);
    if (!win) return;
    msg1(win, sel("setContentView:"), scroll);
    void *title = msg1(getcls("NSString"), sel("stringWithUTF8String:"),
                       (void *)"Untitled — CotEditor");
    if (title) msg1(win, sel("setTitle:"), title);
    if (vc) msg1(win, sel("setContentViewController:"), vc);

    const char *wc_names[] = {
        "CotEditor.DocumentWindowController",
        "_TtC9CotEditor24DocumentWindowController"
    };
    void *wc = NULL;
    if (getenv("PLSR_REEF_TRACE")) fprintf(stderr, "reef: app window init DWC\n");
    for (size_t i = 0; i < sizeof wc_names / sizeof wc_names[0] && !wc; i++) {
        void *cls = getcls(wc_names[i]);
        if (!cls) continue;
        plsr_replace_super_init(cls, "initWithWindow:");
        wc = msg1(msg0(cls, sel("alloc")), sel("initWithWindow:"), win);
    }
    if (getenv("PLSR_REEF_TRACE")) fprintf(stderr, "reef: app window wc=%p\n", wc);
    /* Install the app's own toolbar.  DocumentWindowController implements
     * toolbarDefaultItemIdentifiers: and toolbar:itemForItemIdentifier:
     * willBeInsertedIntoToolbar:, so the items (and their buttons) come from
     * the shipped code rather than from a host-side imitation. */
    if (g_app_document && wc) {
        msg1(wc, sel("setDocument:"), g_app_document);
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: document attached to controller %p\n", wc);
    }
    if (getenv("PLSR_APP_TOOLBAR") && wc) {
        void *tb_cls = getcls("NSToolbar");
        void *tb_name = plsr_mk_nsstring("CotEditor.DocumentWindowController");
        void *toolbar = (tb_cls && tb_name)
            ? msg1(msg0(tb_cls, sel("alloc")), sel("initWithIdentifier:"),
                   tb_name) : NULL;
        if (!toolbar && tb_cls) {
            toolbar = msg0(msg0(tb_cls, sel("alloc")), sel("init"));
            if (toolbar && tb_name && plsr_widget_responds(toolbar, "setIdentifier:"))
                msg1(toolbar, sel("setIdentifier:"), tb_name);
        }
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: toolbar alloc cls=%p name=%p tb=%p\n",
                    tb_cls, tb_name, toolbar);
        if (toolbar) {
            if (wc) msg1(toolbar, sel("setDelegate:"), wc);
            if (getenv("PLSR_REEF_TRACE")) fprintf(stderr, "reef: toolbar delegate set\n");
            msg1(win, sel("setToolbar:"), toolbar);
            if (getenv("PLSR_REEF_TRACE")) fprintf(stderr, "reef: toolbar attached\n");
            void *items = msg0(toolbar, sel("items"));
            unsigned long nitems = items
                ? (unsigned long)(uintptr_t)msg0(items, sel("count")) : 0;
            if (getenv("PLSR_REEF_TRACE"))
                fprintf(stderr, "reef: app toolbar=%p items=%lu delegate=%p\n",
                        toolbar, nitems, wc);
        } else if (getenv("PLSR_REEF_TRACE")) {
            fprintf(stderr, "reef: app toolbar unavailable cls=%p\n", tb_cls);
        }
    }
    if (getenv("PLSR_ORDER_FRONT"))
        msg1(win, sel("makeKeyAndOrderFront:"), NULL);
    if (!getenv("PLSR_NO_REAL_EDITOR") && tv) {
        void *real_cls = getcls("CotEditor.EditorTextView");
        if (!real_cls) real_cls = getcls("_TtC9CotEditor14EditorTextView");
        if (real_cls) *(void **)tv = real_cls;
    }
    if (getenv("PLSR_REEF_TRACE")) fprintf(stderr, "reef: app window ready\n");
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: real app document window win=%p wc=%p tv=%p tvcls=%s vc=%p vccls=%s\n",
                win, wc, tv, tv && objclsname ? objclsname(tv) : "?",
                vc, vc && objclsname ? objclsname(vc) : "(nil)");
}

static unsigned long plsr_present_app_windows(void *app) {
    void *(*msg0)(void *, void *) =
        (void *(*)(void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*msg1i)(void *, void *, unsigned long) =
        (void *(*)(void *, void *, unsigned long))plsr_image_symbol("_objc_msgSend");
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    if (!msg0 || !msg1i || !sel || !app) return 0;
    void *windows = msg0(app, sel("windows"));
    if (!windows) return 0;
    unsigned long count = (unsigned long)(uintptr_t)msg0(windows, sel("count"));
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: present windows count=%lu\n", count);
    for (unsigned long i = 0; i < count; i++) {
        void *window = msg1i(windows, sel("objectAtIndex:"), i);
        if (!window || (plsr_mayo_runtime_window_is_open &&
                        plsr_mayo_runtime_window_is_open(window))) continue;
        void *title_obj = msg0(window, sel("title"));
        const char *title = plsr_nsutf8(title_obj);
        if (!title || !*title) title = "CotEditor";
        sw_node *tree = plsr_build_swiftui_tree(window, title, 900, 620);
        if (tree && plsr_mayo_runtime_window_open &&
            plsr_mayo_runtime_window_open(window, title, 900, 620, tree,
                                                  plsr_widget_spec_for_window(window))) {
            if (getenv("PLSR_REEF_TRACE"))
                fprintf(stderr,
                        "reef: presented app-owned NSWindow %p via Mayonnaise (%s, %lu of %lu)\n",
                        window, title, i + 1, count);
            continue;
        }
        msg1i(window, sel("makeKeyAndOrderFront:"), 0);
        msg0(window, sel("displayIfNeeded"));
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: presented app-owned NSWindow %p (%lu of %lu)\n",
                    window, i + 1, count);
    }
    return count;
}

static uint64_t plsr_window_scan_clock_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

/* The host owns the main-thread event loop while it presents AppKit windows.
 * Keep discovering app-created windows from that live loop; the NSApplication
 * next-event hook cannot run while the host loop is active. */
void plsr_runtime_refresh_app_windows(void) {
    if (!g_app || g_fallback_window || !getenv("PLSR_MAYO_MAIN_THREAD"))
        return;
    uint64_t now = plsr_window_scan_clock_ns();
    if (g_last_window_scan_ns && now &&
        now - g_last_window_scan_ns < 200000000ULL)
        return;
    g_last_window_scan_ns = now;
    (void)plsr_present_app_windows(g_app);
    plsr_widget_refresh_window_specs();
}

static void *plsr_next_event(void *self, void *cmd, unsigned long mask,
                             void *until, void *mode, signed char deq) {
    plsr_widget_drain_actions();
    if (!g_win_created) {
        g_win_created = 1;
        /* Main.storyboardc only contains the real app delegate and main menu.
         * Its AppKit hook can run before +[NSApplication sharedApplication]
         * has published NSApp, so retry delegate installation once the event
         * loop is active.  Then deliver the launch callback that the skipped
         * storyboard path would otherwise have triggered. */
        if (getenv("PLSR_REEF_TRACE")) fprintf(stderr, "reef: next_event bootstrap begin\n");
        plsr_delegate_bootstrap();
        plsr_load_main_menu();
        if (getenv("PLSR_REEF_TRACE")) fprintf(stderr, "reef: next_event bootstrap done\n");
        void *(*getcls)(const char *) =
            (void *(*)(const char *))plsr_image_symbol("_objc_getClass");
        void *(*sel)(const char *) =
            (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
        void *(*msg0)(void *, void *) =
            (void *(*)(void *, void *))plsr_image_symbol("_objc_msgSend");
        void (*msg1)(void *, void *, void *) =
            (void (*)(void *, void *, void *))plsr_image_symbol("_objc_msgSend");
        void *app = (getcls && sel && msg0)
            ? msg0(getcls("NSApplication"), sel("sharedApplication")) : NULL;
        if (g_delegate_installed && !g_delegate_launch_called && app && sel && msg1) {
            g_delegate_launch_called = 1;
            void *delegate = msg0(app, sel("delegate"));
            if (delegate && !getenv("PLSR_SKIP_DELEGATE_LAUNCH")) {
                signed char (*msg1b)(void *, void *, void *) =
                    (signed char (*)(void *, void *, void *))
                        plsr_image_symbol("_objc_msgSend");
                if (getenv("PLSR_CALL_WILL_FINISH") &&
                    !getenv("PLSR_SKIP_WILL_FINISH") && msg1b &&
                    msg1b(delegate, sel("respondsToSelector:"),
                          sel("applicationWillFinishLaunching:"))) {
                    if (getenv("PLSR_REEF_TRACE"))
                        fprintf(stderr, "reef: delivering applicationWillFinishLaunching: to app delegate\n");
                    void *notification = plsr_application_notification(
                        "NSApplicationWillFinishLaunchingNotification", app);
                    msg1(delegate, sel("applicationWillFinishLaunching:"), notification);
                }
                if (!getenv("PLSR_CALL_DID_FINISH") || getenv("PLSR_SKIP_DID_FINISH")) {
                    if (getenv("PLSR_REEF_TRACE"))
                        fprintf(stderr, "reef: skipping delegate finish/untitled (default)\n");
                } else {
                    if (getenv("PLSR_REEF_TRACE"))
                        fprintf(stderr, "reef: delivering applicationDidFinishLaunching: to app delegate\n");
                    void *notification = plsr_application_notification(
                        "NSApplicationDidFinishLaunchingNotification", app);
                    msg1(delegate, sel("applicationDidFinishLaunching:"), notification);
                    if (msg1b && msg1b(delegate, sel("respondsToSelector:"),
                                       sel("applicationShouldOpenUntitledFile:"))) {
                        if (getenv("PLSR_REEF_TRACE"))
                            fprintf(stderr, "reef: delivering applicationShouldOpenUntitledFile:\n");
                        msg1b(delegate, sel("applicationShouldOpenUntitledFile:"), app);
                    }
                }
            }
            if (getenv("PLSR_REEF_TRACE")) fprintf(stderr, "reef: next_event launch callback done\n");
        }
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: next_event app=%p skip_present=%d\n", app,
                    getenv("PLSR_SKIP_PRESENT_WINDOWS") != NULL);
        g_app = app;
    }
    if (g_app && getenv("PLSR_SKIP_PRESENT_WINDOWS")) {
        if (!g_fallback_window) {
            g_fallback_window = 1;
            plsr_make_nswindow();
        }
    } else if (g_app) {
        /* Window discovery must stay live for documents and panels the app
         * creates after launch. Scan at a modest cadence instead of once per
         * event: AppKit's window list is authoritative, while each scan also
         * queries view and toolbar state through Objective-C. */
        uint64_t now = plsr_window_scan_clock_ns();
        int scan_due = !g_last_window_scan_ns || !now ||
                       now - g_last_window_scan_ns >= 200000000ULL;
        if (scan_due) {
            g_last_window_scan_ns = now;
            if (!g_presented_app) {
                if (getenv("PLSR_REEF_TRACE"))
                    fprintf(stderr, "reef: present begin\n");
                if (getenv("PLSR_NEW_DOCUMENT") &&
                    !getenv("PLSR_NO_APP_WINDOW_RECOVERY")) {
                    plsr_new_app_document();
                } else if (!getenv("PLSR_NO_APP_WINDOW_RECOVERY")) {
                    plsr_try_app_document_window();
                } else if (getenv("PLSR_REEF_TRACE")) {
                    fprintf(stderr, "reef: app window recovery disabled; waiting for app-owned windows\n");
                }
                if (getenv("PLSR_REEF_TRACE"))
                    fprintf(stderr, "reef: present after document window\n");
            }
            unsigned long presented = plsr_present_app_windows(g_app);
            if (getenv("PLSR_REEF_TRACE"))
                fprintf(stderr, "reef: present result=%lu\n", presented);
            if (presented > 0) {
                g_presented_app = 1;
            } else if (!g_presented_app && ++g_present_attempts > 20 &&
                       !g_fallback_window && getenv("PLSR_ALLOW_FALLBACK_WINDOW")) {
                g_fallback_window = 1;
                plsr_make_nswindow();
            }
        }
    }
    /* Apply input the host window collected since the last pass. */
    plsr_widget_drain_actions();
    return g_orig_next_event
        ? g_orig_next_event(self, cmd, mask, until, mode, deq) : NULL;
}

static void plsr_swizzle_next_event(void) {
    void *(*getcls)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_objc_getClass");
    void *(*selreg)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    void *(*getmeth)(void *, void *) =
        (void *(*)(void *, void *))plsr_image_symbol("_class_getInstanceMethod");
    void *(*getimp)(void *) =
        (void *(*)(void *))plsr_image_symbol("_method_getImplementation");
    int (*replace)(void *, void *, void *, const char *) =
        (int (*)(void *, void *, void *, const char *))
        plsr_image_symbol("_class_replaceMethod");
    if (!getcls || !selreg || !getmeth || !getimp || !replace) return;
    if (getenv("PLSR_REEF_TRACE")) fprintf(stderr, "reef: swizzle getcls\n");
    void *cls = plsr_class_object("NSApplication");
    if (getenv("PLSR_REEF_TRACE")) fprintf(stderr, "reef: swizzle cls=%p\n", cls);
    void *sel = selreg("nextEventMatchingMask:untilDate:inMode:dequeue:");
    if (getenv("PLSR_REEF_TRACE")) fprintf(stderr, "reef: swizzle sel=%p\n", sel);
    if (!cls || !sel) return;
    void *m = getmeth(cls, sel);
    if (getenv("PLSR_REEF_TRACE")) fprintf(stderr, "reef: swizzle method=%p\n", m);
    if (!m) return;
    g_orig_next_event = (void *(*)(void *, void *, unsigned long, void *,
                                   void *, signed char))getimp(m);
    replace(cls, sel, (void *)plsr_next_event, "@@:Q@@c");
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: swizzled -[NSApplication nextEventMatchingMask:] (orig %p)\n",
                (void *)g_orig_next_event);
}

/* The shipped +[NSColor windowBackgroundColor] is implemented with a
 * dispatch_once block.  In this runtime its cached-color initialization
 * reaches an invalid Objective-C selector before NSWindow can finish init.
 * The SRGB factory is a direct value constructor (no cached dynamic color), so
 * provide a stable equivalent for AppKit's ordinary window background. */
static void *plsr_nscolor_window_background(void *self, void *cmd) {
    void *(*msg4d)(void *, void *, double, double, double, double) =
        (void *(*)(void *, void *, double, double, double, double))
            plsr_image_symbol("_objc_msgSend");
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    const char *(*selname)(void *) =
        (const char *(*)(void *))plsr_image_symbol("_sel_getName");
    if (!msg4d || !sel || !selname) return NULL;
    const char *name = selname(cmd);
    double v = 0.82, alpha = 1.0;
    if (name && (!strcmp(name, "blackColor") || !strcmp(name, "shadowColor") ||
                 !strcmp(name, "controlDarkShadowColor"))) v = 0.12;
    else if (name && (!strcmp(name, "whiteColor") || !strcmp(name, "textBackgroundColor") ||
                      !strcmp(name, "controlBackgroundColor") ||
                      !strcmp(name, "windowBackgroundColor") ||
                      !strcmp(name, "underPageBackgroundColor"))) v = 0.96;
    else if (name && (!strcmp(name, "clearColor"))) alpha = 0.0;
    return msg4d(self, sel("colorWithSRGBRed:green:blue:alpha:"), v, v, v, alpha);
}

static void plsr_patch_nscolor_background(void) {
    if (getenv("PLSR_NO_NSCOLOR_PATCH")) return;
    void *(*getcls)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_objc_getClass");
    void *(*getclass)(void *) =
        (void *(*)(void *))plsr_image_symbol("_object_getClass");
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    int (*replace)(void *, void *, void *, const char *) =
        (int (*)(void *, void *, void *, const char *))
            plsr_image_symbol("_class_replaceMethod");
    if (!getcls || !getclass || !sel || !replace) return;
    void *cls = getcls("NSColor");
    void *meta = cls ? getclass(cls) : NULL;
    if (!meta) return;
    static const char *const cached_colors[] = {
        "blackColor", "darkGrayColor", "lightGrayColor", "whiteColor", "grayColor",
        "redColor", "greenColor", "blueColor", "cyanColor", "yellowColor",
        "magentaColor", "orangeColor", "purpleColor", "brownColor", "clearColor",
        "labelColor", "secondaryLabelColor", "tertiaryLabelColor", "quaternaryLabelColor",
        "placeholderTextColor", "windowFrameTextColor", "selectedMenuItemTextColor",
        "alternateSelectedControlTextColor", "headerTextColor", "separatorColor", "gridColor",
        "windowBackgroundColor", "underPageBackgroundColor", "_sourceListBackgroundColor",
        "controlBackgroundColor", "unemphasizedSelectedContentBackgroundColor",
        "findHighlightColor", "textColor", "textBackgroundColor", "selectedTextColor",
        "unemphasizedSelectedTextBackgroundColor", "unemphasizedSelectedTextColor",
        "controlColor", "controlTextColor", "selectedControlTextColor", "disabledControlTextColor",
        "scrubberTexturedBackgroundColor", "highlightColor", "shadowColor", "systemRedColor",
        "systemGreenColor", "systemBlueColor", "systemOrangeColor", "systemYellowColor",
        "systemBrownColor", "systemPinkColor", "systemPurpleColor", "systemGrayColor",
        "systemTealColor", "systemIndigoColor", "systemMintColor", "systemCyanColor",
        "headerColor", "controlHighlightColor", "controlLightHighlightColor", "controlShadowColor",
        "controlDarkShadowColor", "windowFrameColor", "scrollBarColor", "knobColor",
        "selectedKnobColor", "secondarySelectedControlColor", "systemFillColor",
        "secondarySystemFillColor", "tertiarySystemFillColor", "quaternarySystemFillColor",
        "quinarySystemFillColor", "controlAccentColor", "linkColor", "keyboardFocusIndicatorColor",
        "selectedContentBackgroundColor", "selectedTextBackgroundColor"
    };
    for (size_t i = 0; i < sizeof cached_colors / sizeof cached_colors[0]; i++)
        replace(meta, sel(cached_colors[i]), (void *)plsr_nscolor_window_background, "@@:");
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: installed direct-value NSColor compatibility methods\n");
}

/* Derive the Swift module name and application-delegate class from the
 * bundle that is actually loaded.  CotEditor used to be hardcoded here, which
 * made the bootstrap a CotEditor-specific patch instead of a runtime path. */
static void plsr_bundle_module(char *out, size_t cap, const char *path) {
    out[0] = 0;
    const char *base = path ? strrchr(path, '/') : NULL;
    base = base ? base + 1 : path;
    if (!base || !*base) return;
    size_t n = strlen(base);
    if (n > 4 && !strcmp(base + n - 4, ".app")) n -= 4;
    if (n >= cap) n = cap - 1;
    memcpy(out, base, n);
    out[n] = 0;
    for (char *p = out; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '_') *p = '_';
    if (isdigit((unsigned char)out[0])) {
        memmove(out + 1, out, strlen(out) + 1);
        out[0] = '_';
    }
}

static void *plsr_find_delegate_class(void) {
    void *(*getcls)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_objc_getClass");
    if (!getcls) return NULL;
    const char *override = getenv("PLSR_DELEGATE_CLASS");
    if (override && *override) {
        void *c = getcls(override);
        if (c) return c;
    }
    const char *bundle = getenv("PLSR_APP_BUNDLE");
    const char *exe = (g_main && g_main->path) ? g_main->path : NULL;
    const char *suffixes[] = { "AppDelegate", "ApplicationDelegate", "Delegate" };
    for (int pass = 0; pass < 2; pass++) {
        const char *src = pass == 0 ? bundle : exe;
        char module[128];
        plsr_bundle_module(module, sizeof module, src);
        if (!module[0]) continue;
        for (size_t s = 0; s < sizeof suffixes / sizeof suffixes[0]; s++) {
            char name[256];
            snprintf(name, sizeof name, "_TtC%zu%s%zu%s",
                     strlen(module), module, strlen(suffixes[s]), suffixes[s]);
            void *c = getcls(name);
            if (c) return c;
        }
        if (pass == 0) {
            char name[256];
            snprintf(name, sizeof name, "_TtC%zu%s11AppDelegate",
                     strlen(module), module);
            void *c = getcls(name);
            if (c) return c;
        }
    }
    /* Objective-C apps name the class directly. */
    const char *plain[] = { "AppDelegate", "ApplicationDelegate" };
    for (size_t i = 0; i < sizeof plain / sizeof plain[0]; i++) {
        void *c = getcls(plain[i]);
        if (c) return c;
    }
    return NULL;
}

static void plsr_delegate_bootstrap(void) {
    void *(*getcls)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_objc_getClass");
    void *(*selreg)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    void *(*msg0)(void *, void *) =
        (void *(*)(void *, void *))plsr_image_symbol("_objc_msgSend");
    void (*msg1)(void *, void *, void *) =
        (void (*)(void *, void *, void *))plsr_image_symbol("_objc_msgSend");
    if (!getcls || !selreg || !msg0 || !msg1) return;
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: delegate bootstrap begin bundle=%s\n",
                getenv("PLSR_APP_BUNDLE") ? getenv("PLSR_APP_BUNDLE") : "(unset)");
    void *applicationClass = getcls("NSApplication");
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: delegate bootstrap NSApplication class=%p msgSend=%p selreg=%p\n",
                applicationClass, msg0, selreg);
    void *sharedSel = selreg("sharedApplication");
    void *sharedImp = applicationClass
        ? plsr_resolve_objc_imp(applicationClass, sharedSel) : NULL;
    void *app = sharedImp ? ((void *(*)(void *, void *))sharedImp)(applicationClass, sharedSel)
                          : (applicationClass ? msg0(applicationClass, sharedSel) : NULL);
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: delegate bootstrap sharedApplication=%p imp=%p\n", app, sharedImp);
    if (!app && applicationClass) {
        void *allocSel = selreg("alloc");
        void *allocImp = plsr_resolve_objc_imp(applicationClass, allocSel);
        void *allocatedApp = allocImp
            ? ((void *(*)(void *, void *))allocImp)(applicationClass, allocSel) : NULL;
        if (!allocatedApp) {
            void *(*createInstance)(void *, size_t) =
                (void *(*)(void *, size_t))plsr_image_symbol("_class_createInstance");
            allocatedApp = createInstance ? createInstance(applicationClass, 0) : NULL;
            if (getenv("PLSR_REEF_TRACE"))
                fprintf(stderr, "reef: delegate bootstrap class_createInstance NSApplication=%p\n",
                        allocatedApp);
        }
        void *initSel = selreg("init");
        void *initImp = allocatedApp ? plsr_resolve_objc_imp(allocatedApp, initSel) : NULL;
        app = initImp ? ((void *(*)(void *, void *))initImp)(allocatedApp, initSel) : NULL;
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: delegate bootstrap alloc/init NSApplication alloc=%p/%p init=%p/%p app=%p\n",
                    allocatedApp, allocImp, initImp, initSel, app);
    }
    if (!app || msg0(app, selreg("delegate"))) return;
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: delegate bootstrap application has no delegate\n");
    void *dcls = plsr_find_delegate_class();
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: delegate bootstrap class=%p\n", dcls);
    if (!dcls) return;
    void *allocated = msg0(dcls, selreg("alloc"));
    if (!allocated) {
        void *(*objcAlloc)(void *) =
            (void *(*)(void *))plsr_image_symbol("_objc_alloc");
        allocated = objcAlloc ? objcAlloc(dcls) : NULL;
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: delegate bootstrap objc_alloc delegate=%p\n",
                    allocated);
    }
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: delegate bootstrap allocated=%p\n", allocated);
    void *obj = allocated ? msg0(allocated, selreg("init")) : NULL;
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: delegate bootstrap initialized=%p\n", obj);
    if (!obj) return;
    /* CotEditor's Swift delegate keeps its application back-reference as a
     * weak slot at +0x38. In the normal AppKit startup path this is populated
     * while NSApplication installs the delegate; our explicit bootstrap must
     * establish the same Swift weak reference before entering app startup. */
    if (getenv("PLSR_NSWINDOW") || getenv("PLSR_BOOTSTRAP_DELEGATE")) {
        void (*weak_assign)(void **, void *) =
            (void (*)(void **, void *))plsr_image_symbol("_swift_unknownObjectWeakAssign");
        if (weak_assign) {
            void **slot = (void **)((char *)obj + 0x38);
            if (!*slot) weak_assign(slot, app);
            if (getenv("PLSR_REEF_TRACE"))
                fprintf(stderr, "reef: delegate application weak back-reference slot=%p value=%p\n",
                        slot, *slot);
        }
    }
    msg1(app, selreg("setDelegate:"), obj);
    g_delegate_installed = 1;
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: bootstrapped delegate %p\n", obj);
}

static void plsr_storyboard_noop(void) {}

/* libswiftFoundation's keyPathsForValuesAffectingValueForKey: thunk at
 * +0x6b220 releases a Swift object whose metadata is incomplete in this
 * runtime (libswiftCore+0x3934b2, null metadata + 0x20).  KVO only needs the
 * dependent-key set, so return an empty set without entering the Swift
 * implementation. */
static void *plsr_empty_key_paths(void) {
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    void *(*msg0)(void *, void *) =
        (void *(*)(void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*getcls)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_objc_getClass");
    if (!getcls || !sel || !msg0) return NULL;
    /* Foundation's +[NSSet set] / +[NSArray array] both return nil here, and
     * CFSetCreate faults inside the shipped CF runtime.  CoreFoundation's
     * static empty-array singleton is already a live toll-free-bridged
     * collection, so hand that back instead of building one. */
    void *set = plsr_image_symbol("___NSArray0__");
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: KVO key paths -> __NSArray0__ %p\n", set);
    return set;
}

static void patch_kvo_key_paths(void) {
    if (getenv("PLSR_NO_KVO_NOOP")) return;
    struct image *im = NULL;
    for (int i = 0; i < g_nimgs; i++)
        if (strstr(g_imgs[i]->path, "libswiftFoundation.dylib")) {
            im = g_imgs[i];
            break;
        }
    if (!im) return;
    unsigned char *fn = (unsigned char *)(uintptr_t)(im->base + im->slide + 0x6b220);
    if (!(fn[0] == 0x55 && fn[1] == 0x48 && fn[2] == 0x89 && fn[3] == 0xe5)) {
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: KVO key-path site mismatch (%02x%02x%02x%02x)\n",
                    fn[0], fn[1], fn[2], fn[3]);
        return;
    }
    long pg = sysconf(_SC_PAGESIZE);
    if (pg <= 0) pg = 4096;
    uintptr_t page = (uintptr_t)fn & ~(uintptr_t)(pg - 1);
    if (mprotect((void *)page, (size_t)pg, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
        return;
    unsigned char code[12] = { 0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xe0 };
    uint64_t target = (uint64_t)(uintptr_t)plsr_empty_key_paths;
    memcpy(code + 2, &target, 8);
    memcpy(fn, code, sizeof code);
    __builtin___clear_cache((char *)fn, (char *)fn + sizeof code);
    mprotect((void *)page, (size_t)pg, PROT_READ | PROT_EXEC);
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: patched Swift KVO key-path thunk -> %p\n", (void *)target);
}

static void patch_storyboard_bootstrap(void) {
    if (!getenv("PLSR_BOOTSTRAP_DELEGATE")) return;
    struct image *ak = NULL;
    for (int i = 0; i < g_nimgs; i++)
        if (strstr(g_imgs[i]->path, "AppKit.framework/Versions/C/AppKit")) {
            ak = g_imgs[i];
            break;
        }
    if (!ak) return;
    unsigned char *fn = (unsigned char *)(uintptr_t)(ak->slide + 0x1b7250);
    if (!(fn[0] == 0xf3 || fn[0] == 0x55 || fn[0] == 0x48 || fn[0] == 0x41))
        return;   /* not the expected _AKLoadMainStoryboard prologue */
    long pg = sysconf(_SC_PAGESIZE);
    if (pg <= 0) pg = 4096;
    uintptr_t page = (uintptr_t)fn & ~(uintptr_t)(pg - 1);
    if (mprotect((void *)page, (size_t)pg, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
        return;
    unsigned char code[12] = { 0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xe0 };
    /* finishLaunching installs the delegate itself and then runs AppKit's KVO
     * setup, which faults in libswiftFoundation.  With PLSR_STORYBOARD_NOOP the
     * hook returns immediately so finishLaunching can complete; the delegate is
     * installed from the event-loop hook instead. */
    uint64_t target = getenv("PLSR_STORYBOARD_NOOP")
        ? (uint64_t)(uintptr_t)plsr_storyboard_noop
        : (uint64_t)(uintptr_t)plsr_delegate_bootstrap;
    memcpy(code + 2, &target, 8);
    memcpy(fn, code, sizeof code);
    __builtin___clear_cache((char *)fn, (char *)fn + sizeof code);
    mprotect((void *)page, (size_t)pg, PROT_READ | PROT_EXEC);
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: patched _AKLoadMainStoryboard -> %p\n", (void *)target);
}

static void *plsr_cf_dictionary(long cap);

/* NSKeyedArchiver::_currentValues (Foundation+0x5f780) is
 * `[[self->_containers lastObject] _values]`.  On the delegate-bootstrap path
 * _containers exists but is empty, so lastObject is nil and the original loads
 * from nil+8 (Foundation+0x5f7e1).  Return a live values dictionary instead:
 * reuse the current container's when there is one, otherwise seed the table
 * with a container and hand back its (empty) dictionary.  Archiving state is
 * not needed for the launch to continue. */
/* Bridged CF types (__NSCFType) are archived by AppKit state saving but do
 * not implement encodeWithCoder:. Encoding them as empty keeps the launch
 * going instead of aborting with an unrecognized selector. */
/* _currentValues dereferences a nil _containers on this runtime. Return an
 * empty dictionary instead of faulting; archiving state is non-essential. */
static void *plsr_current_values_helper(void *self, void *cmd) {
    (void)cmd;
    void *(*getcls)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_objc_getClass");
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    void *(*msg0)(void *, void *) =
        (void *(*)(void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*getivar)(void *, const char *) =
        (void *(*)(void *, const char *))plsr_image_symbol("_class_getInstanceVariable");
    long (*ivoff)(void *) =
        (long (*)(void *))plsr_image_symbol("_ivar_getOffset");
    int (*cf_append)(void *, const void *) =
        (int (*)(void *, const void *))plsr_image_symbol("_CFArrayAppendValue");
    if (!getcls || !sel || !msg0) return NULL;
    if (getivar && ivoff) {
        void *ka = getcls("NSKeyedArchiver");
        void *iv = ka ? getivar(ka, "_containers") : NULL;
        long off = iv ? ivoff(iv) : -1;
        if (off >= 0) {
            void *arr = *(void **)((char *)self + off);
            if (arr) {
                void *last = msg0(arr, sel("lastObject"));
                if (last) {
                    void *values = *(void **)((char *)last + 8);
                    if (values) return values;
                    values = plsr_cf_dictionary(0);
                    *(void **)((char *)last + 8) = values;
                    return values;
                }
                void *cc = getcls("__NSKeyedCodingContainer");
                void *uid = cc ? msg0(msg0(cc, sel("alloc")), sel("init")) : NULL;
                void *dict = plsr_cf_dictionary(0);
                if (uid) {
                    *(void **)((char *)uid + 8) = dict;
                    if (cf_append) cf_append(arr, uid);
                }
                if (getenv("PLSR_REEF_TRACE"))
                    fprintf(stderr, "reef: _currentValues seeded container=%p dict=%p\n",
                            uid, dict);
                return dict;
            }
        }
    }
    return plsr_cf_dictionary(0);
}

static void plsr_noop_encode(void *self, void *cmd, void *coder) {
    (void)self; (void)cmd; (void)coder;
}

/* Foundation ships the NSMutable* class shells but not their class-cluster
 * implementations: NSMutableArray's init/array and __NSArrayM are absent, so
 * -[NSMutableArray initWithObjects:] returns nil.  NSKeyedArchiver's
 * designated initializer builds _objects and _containers through those
 * methods, leaving both ivars nil and crashing finishEncoding.  CoreFoundation
 * still provides __NSCFArray/__NSCFDictionary/__NSCFSet and their mutable
 * constructors, so back the missing initializers with toll-free-bridged CF
 * collections. */
static void *plsr_cf_array(long cap) {
    void *(*mk)(void *, long, const void *) =
        (void *(*)(void *, long, const void *))
            plsr_image_symbol("_CFArrayCreateMutable");
    void *cb = (void *)(uintptr_t)plsr_image_symbol("_kCFTypeArrayCallBacks");
    return mk ? mk(NULL, cap, cb) : NULL;
}
static void *plsr_cf_dictionary(long cap) {
    void *(*mk)(void *, long, const void *, const void *) =
        (void *(*)(void *, long, const void *, const void *))
            plsr_image_symbol("_CFDictionaryCreateMutable");
    void *kc = (void *)(uintptr_t)plsr_image_symbol("_kCFTypeDictionaryKeyCallBacks");
    void *vc = (void *)(uintptr_t)plsr_image_symbol("_kCFTypeDictionaryValueCallBacks");
    return mk ? mk(NULL, cap, kc, vc) : NULL;
}
static void *plsr_cf_set(long cap) {
    void *(*mk)(void *, long, const void *) =
        (void *(*)(void *, long, const void *))
            plsr_image_symbol("_CFSetCreateMutable");
    void *cb = (void *)(uintptr_t)plsr_image_symbol("_kCFTypeSetCallBacks");
    return mk ? mk(NULL, cap, cb) : NULL;
}
static void *plsr_array_init(void *self, void *cmd) {
    (void)self; (void)cmd; return plsr_cf_array(0);
}
static void *plsr_array_init_capacity(void *self, void *cmd, long cap) {
    (void)self; (void)cmd; return plsr_cf_array(cap);
}
static void *plsr_array_class_array(void *cls, void *cmd) {
    (void)cls; (void)cmd; return plsr_cf_array(0);
}
static void *plsr_array_init_objects(void *self, void *cmd, void *first, ...) {
    (void)self; (void)cmd;
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: initWithObjects: self=%p first=%p\n", self, first);
    void *arr = plsr_cf_array(0);
    void (*msg1)(void *, void *, void *) =
        (void (*)(void *, void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    if (!arr || !msg1 || !sel) return arr;
    va_list ap;
    va_start(ap, first);
    for (void *o = first; o; o = va_arg(ap, void *))
        msg1(arr, sel("addObject:"), o);
    va_end(ap);
    return arr;
}
static void *plsr_dict_init(void *self, void *cmd) {
    (void)self; (void)cmd; return plsr_cf_dictionary(0);
}
static void *plsr_dict_init_capacity(void *self, void *cmd, long cap) {
    (void)self; (void)cmd; return plsr_cf_dictionary(cap);
}
static void *plsr_dict_class_dictionary(void *cls, void *cmd) {
    (void)cls; (void)cmd; return plsr_cf_dictionary(0);
}

/* The decoder creates every key with -[NSString initWithBytes:length:encoding:].
 * That initializer returns nil in this Foundation, so all 80 keys collapse to
 * the empty-string sentinel and IB.objectdata cannot be found. */
static void *plsr_string_init_any(void *self, void *cmd, const void *bytes,
                                  unsigned long len, ...) {
    (void)self;
    static int calls;
    void *(*cfstr)(void *, const void *, long, unsigned long, unsigned char) =
        (void *(*)(void *, const void *, long, unsigned long, unsigned char))
            plsr_image_symbol("_CFStringCreateWithBytes");
    void *s = (cfstr && bytes)
        ? cfstr(NULL, bytes, (long)len, 0x08000100UL, 0) : NULL;
    if (getenv("PLSR_REEF_TRACE") && calls++ < 5) {
        const char *(*selname)(void *) =
            (const char *(*)(void *))plsr_image_symbol("_sel_getName");
        fprintf(stderr, "reef: string init any %s bytes=%p len=%lu -> %p\n",
                selname ? selname(cmd) : "?", bytes, len, s);
    }
    return s;
}

static void *plsr_string_init_bytes_len(void *self, void *cmd,
                                        const void *bytes, unsigned long len) {
    (void)self;
    (void)cmd;
    static int calls;
    void *(*cfstr)(void *, const void *, long, unsigned long, unsigned char) =
        (void *(*)(void *, const void *, long, unsigned long, unsigned char))
            plsr_image_symbol("_CFStringCreateWithBytes");
    void *s = (cfstr && bytes)
        ? cfstr(NULL, bytes, (long)len, 0x08000100UL, 0) : NULL;
    if (getenv("PLSR_REEF_TRACE") && calls++ < 3)
        fprintf(stderr, "reef: string init2 bytes=%p len=%lu -> %p\n",
                bytes, len, s);
    return s;
}

static void *plsr_string_init_bytes(void *self, void *cmd, const void *bytes,
                                    unsigned long len, unsigned long enc) {
    (void)self;
    (void)cmd;
    static int calls;
    void *(*cfstr)(void *, const void *, long, unsigned long, unsigned char) =
        (void *(*)(void *, const void *, long, unsigned long, unsigned char))
            plsr_image_symbol("_CFStringCreateWithBytes");
    void *s = (cfstr && bytes) ? cfstr(NULL, bytes, (long)len, enc, 0) : NULL;
    if (getenv("PLSR_REEF_TRACE") && calls++ < 3)
        fprintf(stderr, "reef: string init bytes=%p len=%lu enc=%lu -> %p\n",
                bytes, len, enc, s);
    return s;
}

static void plsr_dict_set_object_cf(void *self, void *cmd, void *obj, void *key) {
    (void)cmd;
    void (*set)(void *, const void *, const void *) =
        (void (*)(void *, const void *, const void *))
            plsr_image_symbol("_CFDictionarySetValue");
    if (self && obj && key && set) set(self, key, obj);
}

static int plsr_nib_varint(const unsigned char *b, unsigned long len,
                           unsigned long *pos, unsigned long *val) {
    unsigned long r = 0, shift = 0, p = *pos;
    while (p < len && shift < 64) {
        unsigned char c = b[p++];
        r |= (unsigned long)(c & 0x7f) << shift;
        shift += 7;
        if (c & 0x80) { *pos = p; *val = r; return 1; }
    }
    return 0;
}

static unsigned long plsr_u32(const unsigned char *b, unsigned long off) {
    unsigned int v = 0;
    memcpy(&v, b + off, sizeof v);
    return v;
}

static long plsr_ivar_off(void *cls, const char *name) {
    void *(*getivar)(void *, const char *) =
        (void *(*)(void *, const char *))plsr_image_symbol("_class_getInstanceVariable");
    long (*offset)(void *) =
        (long (*)(void *))plsr_image_symbol("_ivar_getOffset");
    void *iv = getivar ? getivar(cls, name) : NULL;
    return (iv && offset) ? offset(iv) : -1;
}

/* The Foundation string/array factories in the shipped runtime return empty
 * or nil objects.  Parse the NIBArchive key and class-name tables directly so
 * the original decoder's values/objects tables can be used with real keys. */
static void plsr_nib_rebuild_strings(void *self) {
    void *(*getcls)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_objc_getClass");
    void *cls = getcls ? getcls("AKNibDecoder") : NULL;
    if (!cls) return;
    long off_bytes = plsr_ivar_off(cls, "_bytes");
    long off_length = plsr_ivar_off(cls, "_length");
    long off_keys = plsr_ivar_off(cls, "_keys");
    long off_idx = plsr_ivar_off(cls, "_keyIndexes");
    long off_classes = plsr_ivar_off(cls, "_classNames");
    long off_empty = plsr_ivar_off(cls, "_emptyKey");
    if (off_bytes < 0 || off_length < 0 || off_keys < 0 || off_idx < 0 ||
        off_classes < 0) return;
    const unsigned char *b = *(const unsigned char **)((char *)self + off_bytes);
    unsigned long len = *(unsigned long *)((char *)self + off_length);
    if (!b || len < 0x32 || memcmp(b, "NIBArchive", 10) != 0) return;
    unsigned long key_count = plsr_u32(b, 0x1a);
    unsigned long key_off = plsr_u32(b, 0x1e);
    unsigned long class_count = plsr_u32(b, 0x2a);
    unsigned long class_off = plsr_u32(b, 0x2e);
    if (key_off > len || class_off > len) return;
    void *(*mkarr)(void *, long, const void *) =
        (void *(*)(void *, long, const void *))plsr_image_symbol("_CFArrayCreateMutable");
    void *(*mkdict)(void *, long, const void *, const void *) =
        (void *(*)(void *, long, const void *, const void *))
            plsr_image_symbol("_CFDictionaryCreateMutable");
    void *(*mkstr)(void *, const void *, long, unsigned long, unsigned char) =
        (void *(*)(void *, const void *, long, unsigned long, unsigned char))
            plsr_image_symbol("_CFStringCreateWithBytes");
    void *(*mknum)(void *, long, const void *) =
        (void *(*)(void *, long, const void *))plsr_image_symbol("_CFNumberCreate");
    void (*append)(void *, const void *) =
        (void (*)(void *, const void *))plsr_image_symbol("_CFArrayAppendValue");
    void (*dset)(void *, const void *, const void *) =
        (void (*)(void *, const void *, const void *))plsr_image_symbol("_CFDictionarySetValue");
    void (*rel)(const void *) =
        (void (*)(const void *))plsr_image_symbol("_CFRelease");
    void *acb = (void *)(uintptr_t)plsr_image_symbol("_kCFTypeArrayCallBacks");
    void *kcb = (void *)(uintptr_t)plsr_image_symbol("_kCFTypeDictionaryKeyCallBacks");
    void *vcb = (void *)(uintptr_t)plsr_image_symbol("_kCFTypeDictionaryValueCallBacks");
    if (!mkarr || !mkdict || !mkstr || !mknum || !append || !dset) return;
    void *keys = mkarr(NULL, (long)key_count, acb);
    void *idx = mkdict(NULL, (long)key_count, kcb, vcb);
    if (!keys || !idx) return;
    unsigned long p = key_off, empty = 0xffffffffUL;
    for (unsigned long i = 0; i < key_count; i++) {
        unsigned long n = 0;
        if (!plsr_nib_varint(b, len, &p, &n) || p + n > len) break;
        void *s = mkstr(NULL, b + p, (long)n, 0x08000100UL, 0);
        if (!s) s = mkstr(NULL, b + p, (long)n, 0x00000600UL, 0);
        if (!s) s = mkstr(NULL, "", 0, 0x08000100UL, 0);
        if (!s) break;
        append(keys, s);
        int ni = (int)i;
        void *num = mknum(NULL, 3, &ni);
        if (num) { dset(idx, s, num); rel ? rel(num) : (void)0; }
        if (n == 19 && memcmp(b + p, "UINibEncoderEmptyKey", 19) == 0)
            empty = i;
        if (rel) rel(s);
        p += n;
    }
    void *classes = mkarr(NULL, (long)class_count, acb);
    p = class_off;
    for (unsigned long i = 0; classes && i < class_count; i++) {
        unsigned long n = 0, ne = 0;
        if (!plsr_nib_varint(b, len, &p, &n) ||
            !plsr_nib_varint(b, len, &p, &ne) || p + ne * 4 > len) break;
        p += ne * 4;
        if (p + n > len) break;
        unsigned long m = n;
        while (m && b[p + m - 1] == 0) m--;
        void *s = mkstr(NULL, b + p, (long)m, 0x08000100UL, 0);
        if (!s) s = mkstr(NULL, "", 0, 0x08000100UL, 0);
        if (s) { append(classes, s); if (rel) rel(s); }
        p += n;
    }
    *(void **)((char *)self + off_keys) = keys;
    *(void **)((char *)self + off_idx) = idx;
    *(void **)((char *)self + off_classes) = classes;
    if (off_empty >= 0) *(unsigned int *)((char *)self + off_empty) = (unsigned int)empty;
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: rebuilt nib keys=%lu classes=%lu empty=%lu\n",
                key_count, class_count, empty);
}

static void *(*g_nib_decode_key_orig)(void *, void *, void *);
static void *plsr_nib_decode_for_key(void *self, void *cmd, void *key);

static void *plsr_decode_nib_object(void *self, void *cmd, unsigned int index) {
    (void)cmd;
    void *(*getcls)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_objc_getClass");
    void *(*msg0)(void *, void *) =
        (void *(*)(void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*msg1)(void *, void *, void *) =
        (void *(*)(void *, void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*msg2)(void *, void *, void *, void *) =
        (void *(*)(void *, void *, void *, void *))plsr_image_symbol("_objc_msgSend");
    signed char (*msg1b)(void *, void *, void *) =
        (signed char (*)(void *, void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    void *(*nsclass)(void *) =
        (void *(*)(void *))plsr_image_symbol("_NSClassFromString");
    if (!getcls || !msg0 || !msg1 || !msg2 || !sel) return NULL;
    void *decoder_cls = getcls("AKNibDecoder");
    if (!decoder_cls) return NULL;
    long off_classes = plsr_ivar_off(decoder_cls, "_classNames");
    long off_objects = plsr_ivar_off(decoder_cls, "_objects");
    long off_states = plsr_ivar_off(decoder_cls, "_states");
    long off_decoded = plsr_ivar_off(decoder_cls, "_decoded");
    long off_count = plsr_ivar_off(decoder_cls, "_objectCount");
    long off_current = plsr_ivar_off(decoder_cls, "_current");
    if (off_classes < 0 || off_objects < 0 || off_states < 0 ||
        off_decoded < 0 || off_count < 0 || off_current < 0) return NULL;
    void *classes = *(void **)((char *)self + off_classes);
    void *objects = *(void **)((char *)self + off_objects);
    unsigned char *states = *(unsigned char **)((char *)self + off_states);
    void *decoded = *(void **)((char *)self + off_decoded);
    unsigned int count = *(unsigned int *)((char *)self + off_count);
    if (!classes || !objects || !states || !decoded || index >= count)
        return NULL;
    if (states[index]) {
        void *o = msg1(decoded, sel("objectAtIndex:"), (void *)(uintptr_t)index);
        return o;
    }
    states[index] = 1;
    unsigned int old_current = *(unsigned int *)((char *)self + off_current);
    *(unsigned int *)((char *)self + off_current) = index;
    unsigned int class_index = *(unsigned int *)((char *)objects + index * 12);
    void *name = msg1(classes, sel("objectAtIndex:"), (void *)(uintptr_t)class_index);
    char handled = 0;
    void *obj = msg2(self, sel("_decodeFoundationObject:handled:"),
                     name, &handled);
    void *cls = NULL;
    if (!handled) {
        cls = (nsclass && name) ? nsclass(name) : NULL;
        if (!cls && name) {
            const char *s = (const char *)msg0(name, sel("UTF8String"));
            if (s && strncmp(s, "IBNS", 4) == 0) {
                void *sub = msg1(name, sel("substringFromIndex:"), (void *)2);
                cls = nsclass ? nsclass(sub) : NULL;
            }
        }
        if (!cls) cls = getcls("NSObject");
    }
    if ((getenv("PLSR_REEF_TRACE") && index < 12) ||
        (getenv("PLSR_NIB_TRACE_OBJECTS") && index < 128)) {
        const char *nm = name ? (const char *)msg0(name, sel("UTF8String")) : NULL;
        fprintf(stderr, "reef: decode obj[%u] name=%s cls=%p\n",
                index, nm ? nm : "?", cls);
    }
    if (!handled) obj = cls ? msg0(cls, sel("alloc")) : NULL;
    void *allocated_obj = obj;
    void *init_result = NULL;
    signed char has_init = 0;
    if (obj) {
        msg2(decoded, sel("replaceObjectAtIndex:withObject:"),
             (void *)(uintptr_t)index, obj);
        if (!handled && msg1b && (has_init = msg1b(obj, sel("respondsToSelector:"),
                                       sel("initWithCoder:")))) {
            init_result = msg1(obj, sel("initWithCoder:"), self);
            if (init_result) obj = init_result;
            if (msg1b && msg1b(obj, sel("respondsToSelector:"),
                               sel("awakeAfterUsingCoder:"))) {
                void *awake = msg1(obj, sel("awakeAfterUsingCoder:"), self);
                if (awake) obj = awake;
            }
        }
        msg2(decoded, sel("replaceObjectAtIndex:withObject:"),
             (void *)(uintptr_t)index, obj);
    }
    states[index] = 2;
    *(unsigned int *)((char *)self + off_current) = old_current;
    if (getenv("PLSR_NIB_TRACE_OBJECTS") && index < 128) {
        const char *result_name = obj && plsr_image_symbol("_object_getClassName")
            ? ((const char *(*)(const void *))
                plsr_image_symbol("_object_getClassName"))(obj) : "(nil)";
        fprintf(stderr,
            "reef: decoded obj[%u] alloc=%p init=%d result=%p -> %p %s\n",
            index, allocated_obj, has_init, init_result, obj,
            result_name ? result_name : "?");
    }
    return obj;
}

static char (*g_nib_parse_orig)(void *, void *);
static void *plsr_nib_decode_for_key(void *self, void *cmd, void *key) {
    void *value = g_nib_decode_key_orig
        ? g_nib_decode_key_orig(self, cmd, key) : NULL;
    const char *name = plsr_nsutf8(key);
    if (!value && name && strcmp(name, "NS.objects") == 0 &&
        g_nib_decode_key_orig) {
        /* Interface Builder's NIBArchive arrays store their elements under
         * NSObjectsValues; NSArray's keyed-coder implementation asks for the
         * keyed-archive spelling NS.objects. */
        void *alternate = plsr_mk_nsstring("NSObjectsValues");
        if (alternate) value = g_nib_decode_key_orig(self, cmd, alternate);
    }
    if (getenv("PLSR_REEF_TRACE")) {
        if (name && (!strcmp(name, "NSMenuItems") ||
                     !strcmp(name, "NSTitle") || !strcmp(name, "NSSubmenu") ||
                     !strcmp(name, "NS.objects") ||
                     !strcmp(name, "NSObjectsValues"))) {
            void *(*getcls)(const char *) = (void *(*)(const char *))
                plsr_image_symbol("_objc_getClass");
            void *(*sel)(const char *) = (void *(*)(const char *))
                plsr_image_symbol("_sel_registerName");
            void *(*msg0)(void *, void *) = (void *(*)(void *, void *))
                plsr_image_symbol("_objc_msgSend");
            signed char (*msg1b)(void *, void *, void *) =
                (signed char (*)(void *, void *, void *))
                    plsr_image_symbol("_objc_msgSend");
            long current_off = plsr_ivar_off(getcls("AKNibDecoder"), "_current");
            unsigned current = current_off >= 0
                ? *(unsigned *)((char *)self + current_off) : UINT_MAX;
            long index_off = plsr_ivar_off(getcls("AKNibDecoder"), "_keyIndexes");
            long objects_off = plsr_ivar_off(getcls("AKNibDecoder"), "_objects");
            long values_off = plsr_ivar_off(getcls("AKNibDecoder"), "_values");
            long keys_off = plsr_ivar_off(getcls("AKNibDecoder"), "_keys");
            void *indexes = index_off >= 0
                ? *(void **)((char *)self + index_off) : NULL;
            void *objects = objects_off >= 0
                ? *(void **)((char *)self + objects_off) : NULL;
            unsigned char *values = values_off >= 0
                ? *(unsigned char **)((char *)self + values_off) : NULL;
            void *keys = keys_off >= 0
                ? *(void **)((char *)self + keys_off) : NULL;
            void *key_index = indexes
                ? ((void *(*)(void *, void *, void *))plsr_image_symbol("_objc_msgSend"))(
                    indexes, sel("objectForKey:"), key) : NULL;
            long ki = key_index
                ? ((long (*)(void *, void *))plsr_image_symbol("_objc_msgSend"))(
                    key_index, sel("longValue")) : -1;
            const unsigned char *entry = objects && current < 100000
                ? (const unsigned char *)objects + current * 12 : NULL;
            unsigned value_start = entry ? *(const unsigned *)(entry + 4) : 0;
            unsigned value_count = entry ? *(const unsigned *)(entry + 8) : 0;
            const unsigned char *first_value = values
                ? values + (size_t)value_start * 0x20 : NULL;
            unsigned value_key_index = first_value
                ? *(const unsigned *)first_value : UINT_MAX;
            void *value_key = keys && value_key_index != UINT_MAX
                ? ((void *(*)(void *, void *, void *))
                    plsr_image_symbol("_objc_msgSend"))(keys,
                        sel("objectAtIndex:"), (void *)(uintptr_t)value_key_index)
                : NULL;
            const char *value_key_name = value_key
                ? plsr_nsutf8(value_key) : NULL;
            void *value_record = ((void *(*)(void *, void *, void *))
                plsr_image_symbol("_objc_msgSend"))(self,
                    sel("_valueForKey:"), key);
            void *alternate_key = name && !strcmp(name, "NS.objects")
                ? plsr_mk_nsstring("NSObjectsValues") : NULL;
            void *alternate_record = alternate_key
                ? ((void *(*)(void *, void *, void *))
                    plsr_image_symbol("_objc_msgSend"))(self,
                        sel("_valueForKey:"), alternate_key) : NULL;
            void *alternate_index = alternate_key && indexes
                ? ((void *(*)(void *, void *, void *))
                    plsr_image_symbol("_objc_msgSend"))(indexes,
                        sel("objectForKey:"), alternate_key) : NULL;
            long aki = alternate_index
                ? ((long (*)(void *, void *))plsr_image_symbol("_objc_msgSend"))(
                    alternate_index, sel("longValue")) : -1;
            unsigned long count = value && sel && msg0 && msg1b &&
                msg1b(value, sel("respondsToSelector:"), sel("count"))
                ? (unsigned long)(uintptr_t)msg0(value, sel("count")) : 0;
            fprintf(stderr,
                "reef: nib decode key=%s current=%u keyindex=%ld entry=%02x%02x%02x%02x.%02x%02x%02x%02x.%02x%02x%02x%02x values=%p range=%u+%u first=%02x%02x%02x%02x/%s rec=%p:%02x%02x%02x%02x.%02x%02x%02x%02x.%02x%02x%02x%02x.%02x%02x%02x%02x.%02x%02x%02x%02x.%02x%02x%02x%02x.%02x%02x%02x%02x.%02x%02x%02x%02x alt=%ld:%p:%02x%02x%02x%02x.%02x%02x%02x%02x value=%p cls=%s count=%lu\n",
                name, current, ki,
                entry ? entry[0] : 0, entry ? entry[1] : 0,
                entry ? entry[2] : 0, entry ? entry[3] : 0,
                entry ? entry[4] : 0, entry ? entry[5] : 0,
                entry ? entry[6] : 0, entry ? entry[7] : 0,
                entry ? entry[8] : 0, entry ? entry[9] : 0,
                entry ? entry[10] : 0, entry ? entry[11] : 0,
                values, value_start, value_count,
                first_value ? first_value[0] : 0,
                first_value ? first_value[1] : 0,
                first_value ? first_value[2] : 0,
                first_value ? first_value[3] : 0,
                value_key_name ? value_key_name : "?",
                value_record,
                value_record ? ((unsigned char *)value_record)[0] : 0,
                value_record ? ((unsigned char *)value_record)[1] : 0,
                value_record ? ((unsigned char *)value_record)[2] : 0,
                value_record ? ((unsigned char *)value_record)[3] : 0,
                value_record ? ((unsigned char *)value_record)[4] : 0,
                value_record ? ((unsigned char *)value_record)[5] : 0,
                value_record ? ((unsigned char *)value_record)[6] : 0,
                value_record ? ((unsigned char *)value_record)[7] : 0,
                value_record ? ((unsigned char *)value_record)[8] : 0,
                value_record ? ((unsigned char *)value_record)[9] : 0,
                value_record ? ((unsigned char *)value_record)[10] : 0,
                value_record ? ((unsigned char *)value_record)[11] : 0,
                value_record ? ((unsigned char *)value_record)[12] : 0,
                value_record ? ((unsigned char *)value_record)[13] : 0,
                value_record ? ((unsigned char *)value_record)[14] : 0,
                value_record ? ((unsigned char *)value_record)[15] : 0,
                value_record ? ((unsigned char *)value_record)[16] : 0,
                value_record ? ((unsigned char *)value_record)[17] : 0,
                value_record ? ((unsigned char *)value_record)[18] : 0,
                value_record ? ((unsigned char *)value_record)[19] : 0,
                value_record ? ((unsigned char *)value_record)[20] : 0,
                value_record ? ((unsigned char *)value_record)[21] : 0,
                value_record ? ((unsigned char *)value_record)[22] : 0,
                value_record ? ((unsigned char *)value_record)[23] : 0,
                value_record ? ((unsigned char *)value_record)[24] : 0,
                value_record ? ((unsigned char *)value_record)[25] : 0,
                value_record ? ((unsigned char *)value_record)[26] : 0,
                value_record ? ((unsigned char *)value_record)[27] : 0,
                value_record ? ((unsigned char *)value_record)[28] : 0,
                value_record ? ((unsigned char *)value_record)[29] : 0,
                value_record ? ((unsigned char *)value_record)[30] : 0,
                value_record ? ((unsigned char *)value_record)[31] : 0,
                aki, alternate_record,
                alternate_record ? ((unsigned char *)alternate_record)[0] : 0,
                alternate_record ? ((unsigned char *)alternate_record)[1] : 0,
                alternate_record ? ((unsigned char *)alternate_record)[2] : 0,
                alternate_record ? ((unsigned char *)alternate_record)[3] : 0,
                alternate_record ? ((unsigned char *)alternate_record)[4] : 0,
                alternate_record ? ((unsigned char *)alternate_record)[5] : 0,
                alternate_record ? ((unsigned char *)alternate_record)[6] : 0,
                alternate_record ? ((unsigned char *)alternate_record)[7] : 0,
                value,
                value && getcls ? ((const char *(*)(const void *))
                    plsr_image_symbol("_object_getClassName"))(value) : "(nil)",
                count);
            if (current == 13 && first_value) {
                const uint32_t *words = (const uint32_t *)first_value;
                fprintf(stderr,
                    "reef: NIB inline value words=%08x %08x %08x %08x %08x %08x %08x %08x\n",
                    words[0], words[1], words[2], words[3],
                    words[4], words[5], words[6], words[7]);
            }
        }
    }
    return value;
}

/* NIBArchive encodes menu contents as a collection of NSMenuItem objects
 * whose NSMenu reference names their parent. The compatibility NSArray coder
 * currently leaves NSMenuItems arrays empty, so restore those archived
 * relationships before the host projects the real menu tree. */
static int plsr_nib_archive_tables(void *decoder, void **classes_out,
                                  void **objects_out, unsigned char **values_out,
                                  unsigned int *count_out) {
    void *(*getcls)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_objc_getClass");
    void *cls = getcls ? getcls("AKNibDecoder") : NULL;
    if (!decoder || !cls) return 0;
    long classes_off = plsr_ivar_off(cls, "_classNames");
    long objects_off = plsr_ivar_off(cls, "_objects");
    long values_off = plsr_ivar_off(cls, "_values");
    long count_off = plsr_ivar_off(cls, "_objectCount");
    if (classes_off < 0 || objects_off < 0 || values_off < 0 || count_off < 0)
        return 0;
    *classes_out = *(void **)((char *)decoder + classes_off);
    *objects_out = *(void **)((char *)decoder + objects_off);
    *values_out = *(unsigned char **)((char *)decoder + values_off);
    *count_out = *(unsigned int *)((char *)decoder + count_off);
    return *classes_out && *objects_out && *values_out && *count_out > 0;
}

static const char *plsr_nib_archive_class(void *classes,
                                           const unsigned char *objects,
                                           unsigned int object_id) {
    void *(*msg1)(void *, void *, void *) =
        (void *(*)(void *, void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    if (!msg1 || !sel) return NULL;
    unsigned int class_id = *(const unsigned int *)(objects + object_id * 12u);
    void *name = msg1(classes, sel("objectAtIndex:"), (void *)(uintptr_t)class_id);
    return plsr_nsutf8(name);
}

static int plsr_nib_archive_field(void *decoder, unsigned int object_id,
                                  const char *wanted, unsigned int *type_out,
                                  unsigned int *value_out) {
    void *classes = NULL, *objects_array = NULL;
    unsigned char *values = NULL;
    unsigned int object_count = 0;
    if (!wanted || !plsr_nib_archive_tables(decoder, &classes, &objects_array,
                                            &values, &object_count) ||
        object_id >= object_count) return 0;
    const unsigned char *objects = objects_array;
    const unsigned char *object = objects + object_id * 12u;
    unsigned int start = *(const unsigned int *)(object + 4);
    unsigned int nvalues = *(const unsigned int *)(object + 8);
    void *(*msg1)(void *, void *, void *) =
        (void *(*)(void *, void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    void *(*getcls)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_objc_getClass");
    void *decoder_cls = getcls ? getcls("AKNibDecoder") : NULL;
    long keys_off = decoder_cls ? plsr_ivar_off(decoder_cls, "_keys") : -1;
    void *keys = keys_off >= 0 ? *(void **)((char *)decoder + keys_off) : NULL;
    if (!msg1 || !sel || !keys) return 0;
    for (unsigned int i = 0; i < nvalues; ++i) {
        const unsigned char *record = values + (size_t)(start + i) * 0x20;
        unsigned int key_id = *(const unsigned int *)record;
        unsigned int type = *(const unsigned int *)(record + 4);
        void *key = msg1(keys, sel("objectAtIndex:"),
                         (void *)(uintptr_t)key_id);
        const char *key_text = plsr_nsutf8(key);
        if (getenv("PLSR_NIB_TRACE_ROOTS") &&
            (!strcmp(wanted, "NS.bytes") || !strcmp(wanted, "NSDev")))
            fprintf(stderr, "reef: NIB field object=%u key=%s type=%u rec=%08x %08x %08x %08x %08x %08x %08x %08x\n",
                    object_id, key_text ? key_text : "?", type,
                    *(const unsigned int *)(record + 0),
                    *(const unsigned int *)(record + 4),
                    *(const unsigned int *)(record + 8),
                    *(const unsigned int *)(record + 12),
                    *(const unsigned int *)(record + 16),
                    *(const unsigned int *)(record + 20),
                    *(const unsigned int *)(record + 24),
                    *(const unsigned int *)(record + 28));
        if (!key_text || strcmp(key_text, wanted) != 0) continue;
        if (type_out) *type_out = type;
        if (value_out) *value_out = *(const unsigned int *)(record + 8);
        return 1;
    }
    (void)classes;
    return 0;
}

/* Interface Builder stores localized labels as NSLocalizableString objects;
 * their UTF-8 developer value is an inline NS.bytes payload in the archive. */
static int plsr_nib_localizable_bytes(void *decoder, unsigned int object_id,
                                      char *out, size_t out_size) {
    void *classes = NULL, *objects_array = NULL;
    unsigned char *values = NULL;
    unsigned int object_count = 0;
    if (!out || out_size < 2 || !plsr_nib_archive_tables(
            decoder, &classes, &objects_array, &values, &object_count) ||
        object_id >= object_count) return 0;
    const unsigned char *objects = objects_array;
    const unsigned char *object = objects + object_id * 12u;
    unsigned int start = *(const unsigned int *)(object + 4);
    unsigned int nvalues = *(const unsigned int *)(object + 8);
    void *(*msg1)(void *, void *, void *) =
        (void *(*)(void *, void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    long keys_off = plsr_ivar_off(plsr_class_object("AKNibDecoder"), "_keys");
    void *keys = keys_off >= 0 ? *(void **)((char *)decoder + keys_off) : NULL;
    if (!msg1 || !sel || !keys) return 0;
    long data_off = plsr_ivar_off(plsr_class_object("AKNibDecoder"), "_data");
    void *archive_data = data_off >= 0
        ? *(void **)((char *)decoder + data_off) : NULL;
    const unsigned char *archive_bytes = archive_data
        ? ((const unsigned char *(*)(void *, void *))msg1)(
              archive_data, sel("bytes")) : NULL;
    for (unsigned int i = 0; i < nvalues; ++i) {
        const unsigned char *record = values + (size_t)(start + i) * 0x20;
        unsigned int key_id = *(const unsigned int *)record;
        void *key = msg1(keys, sel("objectAtIndex:"),
                         (void *)(uintptr_t)key_id);
        const char *name = plsr_nsutf8(key);
        if (!name || strcmp(name, "NS.bytes") != 0 ||
            *(const unsigned int *)(record + 4) != 8) continue;
        unsigned int offset = *(const unsigned int *)(record + 24);
        unsigned int length = *(const unsigned int *)(record + 28);
        if (length >= out_size || offset > UINT_MAX - length) return 0;
        const unsigned char *source = archive_bytes
            ? archive_bytes + offset : values + offset;
        memcpy(out, source, length);
        out[length] = '\0';
        return 1;
    }
    (void)classes;
    return 0;
}

static unsigned int plsr_nib_find_main_menu(void *decoder) {
    void *classes = NULL, *objects_array = NULL;
    unsigned char *values = NULL;
    unsigned int count = 0;
    if (!plsr_nib_archive_tables(decoder, &classes, &objects_array,
                                  &values, &count)) return UINT_MAX;
    const unsigned char *objects = objects_array;
    void *(*msg1)(void *, void *, void *) =
        (void *(*)(void *, void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    if (!msg1 || !sel) return UINT_MAX;
    /* Prefer Interface Builder's explicit NSApplication.IBMainMenu outlet. */
    for (unsigned int i = 0; i < count; ++i) {
        const char *class_name = plsr_nib_archive_class(classes, objects, i);
        if (getenv("PLSR_NIB_TRACE_ROOTS") && class_name &&
            (!strcmp(class_name, "NSCustomObject") ||
             !strcmp(class_name, "NSMenu"))) {
            unsigned int ct = 0, cr = 0, mt = 0, mr = 0, nt = 0, nr = 0;
            int hc = plsr_nib_archive_field(decoder, i, "NSClassName", &ct, &cr);
            int hm = plsr_nib_archive_field(decoder, i, "IBMainMenu", &mt, &mr);
            int hn = plsr_nib_archive_field(decoder, i, "NSName", &nt, &nr);
            fprintf(stderr, "reef: NIB root candidate id=%u class=%s classfield=%d/%u:%u menufield=%d/%u:%u namefield=%d/%u:%u\n",
                    i, class_name, hc, ct, cr, hm, mt, mr, hn, nt, nr);
        }
        if (!class_name || strcmp(class_name, "NSCustomObject") != 0) continue;
        unsigned int class_type = 0, class_ref = 0, menu_type = 0, menu_ref = 0;
        if (!plsr_nib_archive_field(decoder, i, "NSClassName", &class_type,
                                    &class_ref) || class_type != 10 ||
            !plsr_nib_archive_field(decoder, i, "IBMainMenu", &menu_type,
                                    &menu_ref) || menu_type != 10 ||
            class_ref >= count || menu_ref >= count) continue;
        void *name_obj = msg1(decoder, sel("_decodeObjectAtIndex:"),
                              (void *)(uintptr_t)class_ref);
        const char *name = plsr_nsutf8(name_obj);
        if (name && strcmp(name, "NSApplication") == 0) return menu_ref;
        /* This archive stores NSClassName through an NSString proxy that the
         * compatibility decoder may not materialize yet.  The explicit
         * IBMainMenu outlet is still authoritative when it points at NSMenu. */
        const char *target_class = plsr_nib_archive_class(classes, objects,
                                                          menu_ref);
        if (target_class && strcmp(target_class, "NSMenu") == 0)
            return menu_ref;
    }
    /* Older MainMenu nibs may omit the application outlet but retain the
     * conventional root menu name. */
    for (unsigned int i = 0; i < count; ++i) {
        const char *class_name = plsr_nib_archive_class(classes, objects, i);
        unsigned int name_type = 0, name_ref = 0;
        if (!class_name || strcmp(class_name, "NSMenu") != 0 ||
            !plsr_nib_archive_field(decoder, i, "NSName", &name_type,
                                    &name_ref) || name_type != 10 ||
            name_ref >= count) continue;
        void *name_obj = msg1(decoder, sel("_decodeObjectAtIndex:"),
                              (void *)(uintptr_t)name_ref);
        const char *name = plsr_nsutf8(name_obj);
        if (name && strcmp(name, "_NSMainMenu") == 0) return i;
    }
    return UINT_MAX;
}

static long long plsr_nib_archive_int(void *decoder, unsigned int object_id,
                                     const char *key, long long fallback) {
    unsigned int type = 0, value = 0;
    if (!plsr_nib_archive_field(decoder, object_id, key, &type, &value))
        return fallback;
    if (type <= 3) return (int32_t)value;
    if (type == 4) return 1;
    if (type == 5) return 0;
    if (type == 10) {
        void *(*msg1)(void *, void *, void *) =
            (void *(*)(void *, void *, void *))plsr_image_symbol("_objc_msgSend");
        void *(*sel)(const char *) =
            (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
        void *(*decode)(void *, void *, void *) =
            (void *(*)(void *, void *, void *))plsr_image_symbol("_objc_msgSend");
        if (msg1 && sel && decode) {
            void *number = decode(decoder, sel("_decodeObjectAtIndex:"),
                                  (void *)(uintptr_t)value);
            if (number && ((signed char (*)(void *, void *, void *))msg1)(
                    number, sel("respondsToSelector:"), sel("longLongValue")))
                return (long long)(intptr_t)msg1(number, sel("longLongValue"), NULL);
            if (number && ((signed char (*)(void *, void *, void *))msg1)(
                    number, sel("respondsToSelector:"), sel("intValue")))
                return (long long)(intptr_t)msg1(number, sel("intValue"), NULL);
        }
    }
    return fallback;
}

static int plsr_nib_archive_menu_string(void *decoder, unsigned int object_id,
                                        const char *key, char *out,
                                        size_t out_size) {
    unsigned int type = 0, ref = 0;
    if (!out || !out_size ||
        !plsr_nib_archive_field(decoder, object_id, key, &type, &ref) ||
        type != 10)
        return 0;
    void *classes = NULL, *objects_array = NULL;
    unsigned char *values = NULL;
    unsigned int count = 0;
    /* NSString and NSLocalizableString objects in these keyed archives both
     * carry their UTF-8 payload in NS.bytes. Prefer those bytes over decoding
     * through NSString, whose host implementation currently collapses some
     * initialized archive strings to the empty singleton. */
    if (plsr_nib_localizable_bytes(decoder, ref, out, out_size))
        return 1;
    if (plsr_nib_archive_tables(decoder, &classes, &objects_array,
                                &values, &count) && ref < count) {
        const unsigned char *objects = objects_array;
        const char *class_name = plsr_nib_archive_class(classes, objects, ref);
        if (class_name && !strcmp(class_name, "NSLocalizableString") &&
            plsr_nib_localizable_bytes(decoder, ref, out, out_size))
            return 1;
    }
    void *(*msg1)(void *, void *, void *) =
        (void *(*)(void *, void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    void *(*decode)(void *, void *, void *) =
        (void *(*)(void *, void *, void *))plsr_image_symbol("_objc_msgSend");
    void *object = msg1 && sel && decode
        ? decode(decoder, sel("_decodeObjectAtIndex:"), (void *)(uintptr_t)ref)
        : NULL;
    const char *utf8 = plsr_nsutf8(object);
    if (!utf8) return 0;
    snprintf(out, out_size, "%s", utf8);
    return 1;
}

static void plsr_nib_populate_menu_inner(void *decoder, unsigned int menu_id,
                                         unsigned char *visited,
                                         unsigned int object_count,
                                         unsigned int depth) {
    if (!decoder || !visited || menu_id >= object_count || depth > 12 ||
        visited[menu_id]) return;
    visited[menu_id] = 1;
    void *classes = NULL, *objects_array = NULL;
    unsigned char *values = NULL;
    unsigned int count = 0;
    if (!plsr_nib_archive_tables(decoder, &classes, &objects_array,
                                  &values, &count)) return;
    const unsigned char *objects = objects_array;
    if (count != object_count) return;
    const char *menu_class = plsr_nib_archive_class(classes, objects, menu_id);
    if (!menu_class || strcmp(menu_class, "NSMenu") != 0) return;
    void *(*msg0)(void *, void *) =
        (void *(*)(void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*msg1)(void *, void *, void *) =
        (void *(*)(void *, void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    if (!msg0 || !msg1 || !sel) return;
    void *menu = msg1(decoder, sel("_decodeObjectAtIndex:"),
                      (void *)(uintptr_t)menu_id);
    if (!menu) return;
    void *items = msg0(menu, sel("itemArray"));
    unsigned long existing = items
        ? (unsigned long)(uintptr_t)msg0(items, sel("count")) : 0;
    for (unsigned int i = 0; i < count; ++i) {
        const char *class_name = plsr_nib_archive_class(classes, objects, i);
        unsigned int parent_type = 0, parent_id = 0;
        if (!class_name || strcmp(class_name, "NSMenuItem") != 0 ||
            !plsr_nib_archive_field(decoder, i, "NSMenu", &parent_type,
                                    &parent_id) || parent_type != 10 ||
            parent_id != menu_id) continue;
        void *item = msg1(decoder, sel("_decodeObjectAtIndex:"),
                          (void *)(uintptr_t)i);
        if (!item) continue;
        unsigned int title_type = 0, title_id = 0;
        if (plsr_nib_archive_field(decoder, i, "NSTitle", &title_type,
                                   &title_id) && title_type == 10 &&
            title_id < count) {
            const char *title_class = plsr_nib_archive_class(classes, objects,
                                                             title_id);
            if (title_class && strcmp(title_class, "NSLocalizableString") == 0) {
                char title[256];
                if (plsr_nib_localizable_bytes(decoder, title_id, title,
                                               sizeof title) && title[0]) {
                    plsr_nib_remember_menu_title(item, title);
                    void *title_obj = plsr_mk_nsstring(title);
                    if (title_obj) msg1(item, sel("setTitle:"), title_obj);
                }
            }
        }
        char key[128] = "";
        int has_key = plsr_nib_archive_menu_string(decoder, i, "NSKeyEquiv",
                                                   key, sizeof key);
        if (getenv("PLSR_NIB_TRACE_ROOTS")) {
            unsigned int kt = 0, kr = 0, at = 0, ar = 0, mt = 0, mr = 0;
            int hk = plsr_nib_archive_field(decoder, i, "NSKeyEquiv", &kt, &kr);
            int ha = plsr_nib_archive_field(decoder, i, "NSAction", &at, &ar);
            int hm = plsr_nib_archive_field(decoder, i, "NSKeyEquivModMask", &mt, &mr);
            const char *kc = "?";
            if (hk && kt == 10) {
                void *classes = NULL, *objs_array = NULL;
                unsigned char *values = NULL;
                unsigned int object_count = 0;
                if (plsr_nib_archive_tables(decoder, &classes, &objs_array,
                                            &values, &object_count) && kr < object_count)
                    kc = plsr_nib_archive_class(classes, objs_array, kr);
                if (!kc) kc = "?";
            }
            fprintf(stderr,
                    "reef: NIB menu props id=%u key=%d/%u:%u[%s]/%.32s action=%d/%u:%u mask=%d/%u:%u/%lld\n",
                    i, hk, kt, kr, kc, key, ha, at, ar, hm, mt, mr,
                    plsr_nib_archive_int(decoder, i, "NSKeyEquivModMask", -1));
        }
        if (has_key) {
            void *key_obj = plsr_mk_nsstring(key);
            if (key_obj) msg1(item, sel("setKeyEquivalent:"), key_obj);
        }
        unsigned long key_mask = (unsigned long)plsr_nib_archive_int(
            decoder, i, "NSKeyEquivModMask", 0);
        msg1(item, sel("setKeyEquivalentModifierMask:"),
             (void *)(uintptr_t)key_mask);
        msg1(item, sel("setState:"),
             (void *)(intptr_t)plsr_nib_archive_int(decoder, i, "NSState", 0));
        if (plsr_nib_archive_int(decoder, i, "NSIsAlternate", 0))
            msg1(item, sel("setAlternate:"), (void *)1);
        if (plsr_nib_archive_int(decoder, i, "NSIsDisabled", 0))
            msg1(item, sel("setEnabled:"), NULL);
        char action_name[128] = "";
        if (plsr_nib_archive_menu_string(decoder, i, "NSAction", action_name,
                                         sizeof action_name) && action_name[0]) {
            void *(*selreg)(const char *) =
                (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
            void *action = selreg ? selreg(action_name) : NULL;
            if (action) msg1(item, sel("setAction:"), action);
        }
        if (getenv("PLSR_NIB_TRACE_ROOTS")) {
            unsigned int tt = 0, tr = 0, bt = 0, br = 0;
            int ht = plsr_nib_archive_field(decoder, i, "NSTitle", &tt, &tr);
            int hb = ht && tt == 10 && tr < count
                ? plsr_nib_archive_field(decoder, tr, "NS.bytes", &bt, &br) : 0;
            fprintf(stderr, "reef: NIB menu item id=%u title=%d/%u:%u bytes=%d/%u:%u\n",
                    i, ht, tt, tr, hb, bt, br);
        }
        int present = 0;
        for (unsigned long j = 0; items && j < existing; ++j) {
            void *current = msg1(items, sel("objectAtIndex:"),
                                 (void *)(uintptr_t)j);
            if (current == item) { present = 1; break; }
        }
        if (!present) {
            msg1(menu, sel("addItem:"), item);
            existing++;
        }
        unsigned int submenu_type = 0, submenu_id = 0;
        if (plsr_nib_archive_field(decoder, i, "NSSubmenu", &submenu_type,
                                   &submenu_id) && submenu_type == 10 &&
            submenu_id < count)
            plsr_nib_populate_menu_inner(decoder, submenu_id, visited,
                                         object_count, depth + 1);
    }
}

static void plsr_nib_populate_menu_tree(void *decoder, unsigned int root_id) {
    void *classes = NULL, *objects_array = NULL;
    unsigned char *values = NULL;
    unsigned int count = 0;
    if (!plsr_nib_archive_tables(decoder, &classes, &objects_array,
                                  &values, &count) || root_id >= count) return;
    unsigned char *visited = calloc(count, 1);
    if (!visited) return;
    plsr_nib_populate_menu_inner(decoder, root_id, visited, count, 0);
    if (getenv("PLSR_REEF_TRACE")) {
        void *(*msg0)(void *, void *) =
            (void *(*)(void *, void *))plsr_image_symbol("_objc_msgSend");
        void *(*msg1)(void *, void *, void *) =
            (void *(*)(void *, void *, void *))plsr_image_symbol("_objc_msgSend");
        void *(*sel)(const char *) =
            (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
        void *menu = msg0 && msg1 && sel
            ? msg0(msg1(decoder, sel("_decodeObjectAtIndex:"),
                        (void *)(uintptr_t)root_id), sel("itemArray")) : NULL;
        unsigned long n = menu && msg0 && sel
            ? (unsigned long)(uintptr_t)msg0(menu, sel("count")) : 0;
        fprintf(stderr, "reef: restored NIB menu root=%u items=%lu objects=%u\n",
                root_id, n, count);
    }
    free(visited);
}

static void plsr_nib_apply_connectors(void *decoder) {
    void *classes = NULL, *objects_array = NULL;
    unsigned char *values = NULL;
    unsigned int count = 0;
    void *(*msg1)(void *, void *, void *) =
        (void *(*)(void *, void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    void *(*decode)(void *, void *, void *) =
        (void *(*)(void *, void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*selreg)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    unsigned applied = 0;
    if (!decoder || !msg1 || !sel || !decode || !selreg ||
        !plsr_nib_archive_tables(decoder, &classes, &objects_array,
                                 &values, &count))
        return;
    const unsigned char *objects = objects_array;
    for (unsigned int i = 0; i < count; ++i) {
        const char *class_name = plsr_nib_archive_class(classes, objects, i);
        int outlet = class_name && !strcmp(class_name, "NSNibOutletConnector");
        int action = class_name && !strcmp(class_name, "NSNibControlConnector");
        if (!outlet && !action) continue;

        char label[256] = "";
        unsigned int source_type = 0, source_ref = 0;
        unsigned int destination_type = 0, destination_ref = 0;
        int has_destination = plsr_nib_archive_field(
            decoder, i, "NSDestination", &destination_type, &destination_ref);
        if (!plsr_nib_archive_menu_string(decoder, i, "NSLabel", label,
                                          sizeof label) || !label[0] ||
            !plsr_nib_archive_field(decoder, i, "NSSource", &source_type,
                                    &source_ref) || source_type != 10 ||
            (outlet && (!has_destination || destination_type != 10)))
            continue;
        void *source = decode(decoder, sel("_decodeObjectAtIndex:"),
                              (void *)(uintptr_t)source_ref);
        void *destination = has_destination && destination_type == 10
            ? decode(decoder, sel("_decodeObjectAtIndex:"),
                     (void *)(uintptr_t)destination_ref) : NULL;
        if (!source) continue;
        if (outlet) {
            char setter[300];
            size_t n = strlen(label);
            if (n + 5 >= sizeof setter) continue;
            memcpy(setter, "set", 3);
            setter[3] = (label[0] >= 'a' && label[0] <= 'z')
                ? (char)(label[0] - 'a' + 'A') : label[0];
            memcpy(setter + 4, label + 1, n - 1);
            setter[n + 3] = ':';
            setter[n + 4] = '\0';
            if (((signed char (*)(void *, void *, void *))msg1)(
                    source, sel("respondsToSelector:"), sel(setter)))
                msg1(source, sel(setter), destination);
        } else {
            if (getenv("PLSR_REEF_TRACE"))
                fprintf(stderr,
                        "reef: NIB action connector id=%u source=%p/%s selector=%s target=%p/%s\n",
                        i, source, plsr_nib_archive_class(classes, objects, source_ref),
                        label, destination,
                        plsr_nib_archive_class(classes, objects, destination_ref));
            if (((signed char (*)(void *, void *, void *))msg1)(
                    source, sel("respondsToSelector:"), sel("setTarget:")))
                msg1(source, sel("setTarget:"), destination);
            if (((signed char (*)(void *, void *, void *))msg1)(
                    source, sel("respondsToSelector:"), sel("setAction:")))
                msg1(source, sel("setAction:"), selreg(label));
        }
        applied++;
    }
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: applied %u archived NIB connectors\n", applied);
}

static int plsr_valid_obj(const void *p);

/* Keep a small immutable shortcut index so the host input thread never has
 * to enter the app's Objective-C runtime. The item/menu pointers are resolved
 * back on the app thread when the queued action is drained. */
struct plsr_menu_shortcut {
    void *item;
    void *menu;
    char key[64];
    unsigned long modifiers;
};
static struct plsr_menu_shortcut g_menu_shortcuts[512];
static size_t g_menu_shortcut_count;

static void plsr_menu_shortcuts_walk(void *menu, unsigned depth,
                                     void **visited, size_t *nvisited,
                                     size_t visited_cap,
                                     void *(*msg0)(void *, void *),
                                     void *(*msg1)(void *, void *, void *),
                                     void *(*sel)(const char *)) {
    if (!menu || depth > 16 || *nvisited >= visited_cap ||
        g_menu_shortcut_count >= sizeof g_menu_shortcuts / sizeof g_menu_shortcuts[0])
        return;
    for (size_t i = 0; i < *nvisited; ++i)
        if (visited[i] == menu) return;
    visited[(*nvisited)++] = menu;
    void *items = msg0(menu, sel("itemArray"));
    unsigned long count = items
        ? (unsigned long)(uintptr_t)msg0(items, sel("count")) : 0;
    for (unsigned long i = 0; i < count; ++i) {
        void *item = msg1(items, sel("objectAtIndex:"), (void *)(uintptr_t)i);
        if (!item) continue;
        if (!plsr_valid_obj(item)) continue;
        void *key_obj = msg0(item, sel("keyEquivalent"));
        const char *key = plsr_nsutf8(key_obj);
        void *submenu = NULL;
        if (plsr_valid_obj(item))
            submenu = msg0(item, sel("submenu"));
        if (getenv("PLSR_REEF_TRACE") && depth < 2)
            fprintf(stderr,
                    "reef: menu shortcut probe depth=%u item=%p key=%p/'%.8s' submenu=%p\n",
                    depth, item, key_obj, key ? key : "", submenu);
        if (key && key[0] && !key[1]) {
            struct plsr_menu_shortcut *entry =
                &g_menu_shortcuts[g_menu_shortcut_count++];
            entry->item = item;
            entry->menu = menu;
            entry->key[0] = (char)tolower((unsigned char)key[0]);
            entry->key[1] = '\0';
            entry->modifiers = (unsigned long)(uintptr_t)
                msg0(item, sel("keyEquivalentModifierMask"));
        }
        if (submenu)
            plsr_menu_shortcuts_walk(submenu, depth + 1, visited, nvisited,
                                     visited_cap, msg0, msg1, sel);
        if (g_menu_shortcut_count >= sizeof g_menu_shortcuts / sizeof g_menu_shortcuts[0])
            return;
    }
}

static void plsr_capture_menu_shortcuts(void *app) {
    void *(*msg0)(void *, void *) =
        (void *(*)(void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*msg1)(void *, void *, void *) =
        (void *(*)(void *, void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    if (!app || !msg0 || !msg1 || !sel) return;
    void *main_menu = msg0(app, sel("mainMenu"));
    void *visited[128];
    size_t nvisited = 0;
    g_menu_shortcut_count = 0;
    plsr_menu_shortcuts_walk(main_menu, 0, visited, &nvisited,
                             sizeof visited / sizeof visited[0],
                             msg0, msg1, sel);
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: indexed %zu real main-menu shortcuts\n",
                g_menu_shortcut_count);
}

int plsr_widget_dispatch_menu_shortcut(char key, unsigned modifiers) {
    unsigned long mask = 0;
    key = (char)tolower((unsigned char)key);
    /* AppKit modifier flag values: Shift, Control, Option, Command. Mayo's
     * Ctrl is the platform's primary shortcut modifier on Linux. */
    if (modifiers & (1u << 0)) mask |= 1ul << 17;
    if (modifiers & (1u << 2)) mask |= 1ul << 19;
    if (modifiers & (1u << 1)) mask |= 1ul << 20;
    if (modifiers & (1u << 3)) mask |= 1ul << 20;
    if (getenv("PLSR_REEF_TRACE") && key == 'n')
        fprintf(stderr, "reef: menu key n modifiers=%u mask=%#lx candidates=%zu\n",
                modifiers, mask, g_menu_shortcut_count);
    for (size_t i = 0; i < g_menu_shortcut_count; ++i) {
        const struct plsr_menu_shortcut *entry = &g_menu_shortcuts[i];
        if (entry->key[0] == key && entry->modifiers == mask) {
            if (getenv("PLSR_REEF_TRACE")) {
                void *action = g_w_m0
                    ? g_w_m0(entry->item, g_w_sel("action")) : NULL;
                const char *(*sel_name)(void *) =
                    (const char *(*)(void *))plsr_image_symbol("_sel_getName");
                const char *name = action && sel_name ? sel_name(action) : NULL;
                fprintf(stderr,
                    "reef: menu key '%c' matched item=%p action=%s flags=%#lx\n",
                    key, entry->item, name ? name : "(null)",
                    entry->modifiers);
            }
            plsr_widget_queue_action(entry->item, PLSR_WIDGET_ACTION_MENU, NULL);
            return 1;
        }
    }
    return 0;
}

static char plsr_nib_parse(void *self, void *cmd) {
    char ok = g_nib_parse_orig ? g_nib_parse_orig(self, cmd) : 0;
    if (ok) plsr_nib_rebuild_strings(self);
    return ok;
}

static void plsr_cf_array_replace(void *self, void *cmd, unsigned long idx,
                                  void *obj) {
    (void)cmd;
    void (*set)(void *, unsigned long, const void *) =
        (void (*)(void *, unsigned long, const void *))
            plsr_image_symbol("_CFArraySetValueAtIndex");
    if (self && obj && set) set(self, idx, obj);
}

static void *plsr_recent_document_urls(void *self, void *cmd) {
    (void)self;
    (void)cmd;
    return plsr_cf_array(0);
}

/* NSNib's AKNibDecoder builds its key table with +[NSMutableArray
 * arrayWithCapacity:] and +[NSMutableDictionary dictionaryWithCapacity:].
 * Those public class methods are one-byte stubs in this Foundation build, so
 * the decoder ends up with nil _keys/_keyIndexes and cannot find
 * IB.objectdata.  Route the factories to the CF-backed constructors. */
static void patch_nib_collections(void) {
    void *(*getcls)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_objc_getClass");
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    void *(*getclass)(void *) =
        (void *(*)(void *))plsr_image_symbol("_object_getClass");
    void *(*replace)(void *, void *, void *, const char *) =
        (void *(*)(void *, void *, void *, const char *))
            plsr_image_symbol("_class_replaceMethod");
    if (!getcls || !sel || !getclass || !replace) return;
    void *ma = getcls("NSMutableArray");
    void *ma_meta = ma ? getclass(ma) : NULL;
    if (ma_meta) {
        replace(ma_meta, sel("array"), (void *)plsr_array_class_array, "@@:");
        replace(ma_meta, sel("arrayWithCapacity:"),
                (void *)plsr_array_class_array, "@@:Q");
    }
    void *md = getcls("NSMutableDictionary");
    void *md_meta = md ? getclass(md) : NULL;
    if (md_meta) {
        replace(md_meta, sel("dictionary"),
                (void *)plsr_dict_class_dictionary, "@@:");
        replace(md_meta, sel("dictionaryWithCapacity:"),
                (void *)plsr_dict_class_dictionary, "@@:Q");
    }
    void *(*copy_methods)(void *, unsigned int *) =
        (void *(*)(void *, unsigned int *))plsr_image_symbol("_class_copyMethodList");
    void *(*method_name)(void *) =
        (void *(*)(void *))plsr_image_symbol("_method_getName");
    const char *(*sel_name)(void *) =
        (const char *(*)(void *))plsr_image_symbol("_sel_getName");
    const char *(*method_types)(void *) =
        (const char *(*)(void *))plsr_image_symbol("_method_getTypeEncoding");
    const char *string_classes[] = {
        "NSString", "NSMutableString", "NSPlaceholderString",
        "__NSCFString", "NSCFString", "__NSCFConstantString"
    };
    for (size_t si = 0; si < sizeof string_classes / sizeof string_classes[0]; si++) {
        void *sc = getcls(string_classes[si]);
        if (!sc) continue;
        unsigned int mc = 0;
        void **ms = copy_methods ? (void **)copy_methods(sc, &mc) : NULL;
        for (unsigned int mi = 0; ms && mi < mc; mi++) {
            void *mn = method_name ? method_name(ms[mi]) : NULL;
            const char *name = (mn && sel_name) ? sel_name(mn) : NULL;
            if (!name || strncmp(name, "initWithBytes", 13) != 0) continue;
            const char *types = method_types ? method_types(ms[mi]) : "@@:@";
            replace(sc, mn, (void *)plsr_string_init_any, types);
            if (getenv("PLSR_REEF_TRACE"))
                fprintf(stderr, "reef: patched %s -%s %s\n",
                        string_classes[si], name, types ? types : "?");
        }
        free(ms);
        replace(sc, sel("initWithBytes:length:"),
                (void *)plsr_string_init_any, "@@:^vQ");
        replace(sc, sel("initWithBytes:length:encoding:"),
                (void *)plsr_string_init_any, "@@:^vQQ");
    }
    void *nscfd = getcls("__NSCFDictionary");
    if (nscfd)
        replace(nscfd, sel("setObject:forKey:"),
                (void *)plsr_dict_set_object_cf, "v@:@:@");
    if (md)
        replace(md, sel("setObject:forKey:"),
                (void *)plsr_dict_set_object_cf, "v@:@:@");
    void *nscfa = getcls("__NSCFArray");
    if (nscfa) {
        replace(nscfa, sel("replaceObjectAtIndex:withObject:"),
                (void *)plsr_cf_array_replace, "v@:Q@");
        replace(nscfa, sel("setObject:atIndexedSubscript:"),
                (void *)plsr_cf_array_replace, "v@:Q@");
    }
    const char *docctl_names[] = {
        "NSDocumentController", "CotEditor.DocumentController",
        "_TtC9CotEditor18DocumentController"
    };
    for (size_t di = 0; di < sizeof docctl_names / sizeof docctl_names[0]; di++) {
        void *docctl = getcls(docctl_names[di]);
        if (!docctl) continue;
        replace(docctl, sel("recentDocumentURLs"),
                (void *)plsr_recent_document_urls, "@@:");
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: patched recentDocumentURLs on %s\n",
                    docctl_names[di]);
    }
    void *decoder = getcls("AKNibDecoder");
    void *(*getimp)(void *, void *) =
        (void *(*)(void *, void *))plsr_image_symbol("_class_getMethodImplementation");
    if (decoder && getimp) {
        void *parse_sel = sel("_parse");
        g_nib_parse_orig = (char (*)(void *, void *))getimp(decoder, parse_sel);
        if (g_nib_parse_orig)
            replace(decoder, parse_sel, (void *)plsr_nib_parse, "c@:");
        void *decode_sel = sel("_decodeObjectAtIndex:");
        if (getimp(decoder, decode_sel))
            replace(decoder, decode_sel, (void *)plsr_decode_nib_object, "@@:I");
        void *key_sel = sel("decodeObjectForKey:");
        g_nib_decode_key_orig = (void *(*)(void *, void *, void *))
            getimp(decoder, key_sel);
        if (g_nib_decode_key_orig)
            replace(decoder, key_sel, (void *)plsr_nib_decode_for_key, "@@:@");
    }
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: patched nib collection factories\n");
}

static void *plsr_dict_init_objects_for_keys_count(void *self, void *cmd,
        const void **objects, const void **keys, unsigned long count) {
    (void)cmd;
    void (*cfset)(void *, const void *, const void *) =
        (void (*)(void *, const void *, const void *))
            plsr_image_symbol("_CFDictionarySetValue");
    if (!self || !cfset) return self;
    for (unsigned long i = 0; i < count; i++)
        if (objects && keys && objects[i] && keys[i] &&
            plsr_valid_obj(objects[i]) && plsr_valid_obj(keys[i]))
            cfset(self, keys[i], objects[i]);
    return self;
}
/* Darwin's -[NSMutableDictionary setObject:forKey:] raises when either
 * argument is nil, and CoreFoundation's __reefRaiseNil then executes ud2 after
 * the exception wrapper returns.  Sparkle reaches this with a nil value while
 * probing its bundle info, which is not fatal for the editor.  Treat nil like
 * setValue:forKey: (remove) so the launch continues. */
/* Swift strings and other tagged values can reach the CF-backed dictionary
 * without going through the Objective-C bridge.  A raw tagged value is not an
 * object pointer, and CF's callbacks pass it to objc_retain, which faults at
 * libobjc+0xb79.  Keep only real, mapped, pointer-aligned objects in the
 * collection; dropping a malformed entry is safer than aborting the launch. */
static int plsr_valid_obj(const void *p) {
    uintptr_t v = (uintptr_t)p;
    if (!v || (v & 0x7)) return 0;
    if (v < 0x10000 || v >= 0x0000800000000000ULL) return 0;
    unsigned char vec = 0;
    if (mincore((void *)(v & ~(uintptr_t)0xfff), 0x1000, &vec) != 0)
        return 0;
    /* A mapped pointer is not enough: libobjc reads the isa at +0 and then
     * dereferences isa+0x18.  Swift values (small strings carry 0xe in their
     * second word) and half-initialized objects have a first word that is not
     * a class, which faults inside _rootRetain.  Require a real class here. */
    uintptr_t isa = *(const uintptr_t *)v;
    if (!isa) return 0;
    if (isa & 1) {
        uintptr_t tagged = isa & ~(uintptr_t)7;
        return tagged && plsr_image_pointer(tagged);
    }
    if ((isa & 7) || !plsr_image_pointer(isa)) return 0;
    return 1;
}

static __thread unsigned int g_dict_count_depth;
static unsigned long (*g_nscfd_count_original)(void *, void *);
static void *g_nscfd_count_selector;
static unsigned long plsr_dict_count(void *self, void *cmd) {
    (void)cmd;
    /* CFDictionaryGetCount may cross back into the Objective-C count: method
     * on this toll-free bridged class. Since that method is this shim, stop
     * the bridge from recursing indefinitely. */
    if (g_dict_count_depth) return 0;
    if (g_nscfd_count_original && g_nscfd_count_selector) {
        void *(*getclass)(void *) =
            (void *(*)(void *))plsr_image_symbol("_object_getClass");
        void *(*getname)(void *) =
            (void *(*)(void *))plsr_image_symbol("_class_getName");
        void *cls = getclass ? getclass(self) : NULL;
        const char *name = (cls && getname) ? getname(cls) : NULL;
        if (name && !strcmp(name, "__NSCFDictionary"))
            return g_nscfd_count_original(self, g_nscfd_count_selector);
    }
    unsigned long (*getcount)(const void *) =
        (unsigned long (*)(const void *))plsr_image_symbol("_CFDictionaryGetCount");
    if (!getcount || !self) return 0;
    g_dict_count_depth++;
    unsigned long count = getcount(self);
    g_dict_count_depth--;
    return count;
}
static void *plsr_unknown_dictionary_value(void) {
    static void *value;
    if (!value) {
        void *(*create)(const void *, const char *, unsigned) =
            (void *(*)(const void *, const char *, unsigned))
                plsr_image_symbol("_CFStringCreateWithCString");
        if (create) value = create(NULL, "UnknownValue", 0x08000100);
    }
    return value;
}
static void *plsr_dict_object_for_key(void *self, void *cmd, void *key) {
    (void)cmd;
    const void *(*get)(const void *, const void *) =
        (const void *(*)(const void *, const void *))
            plsr_image_symbol("_CFDictionaryGetValue");
    return (get && self && key) ? (void *)get(self, key) : NULL;
}
static void plsr_dict_get_objects_keys(void *self, void *cmd,
        void **objects, void **keys, unsigned long count) {
    (void)cmd;
    if (getenv("PLSR_REEF_TRACE")) {
        void *(*getclass)(void *) =
            (void *(*)(void *))plsr_image_symbol("_object_getClass");
        const char *(*clsname)(void *) =
            (const char *(*)(void *))plsr_image_symbol("_class_getName");
        void *c = (getclass && self) ? getclass(self) : NULL;
        fprintf(stderr, "reef: getObjects:andKeys:count: self=%p cls=%s n=%lu\n",
                self, (c && clsname) ? clsname(c) : "?", count);
    }
    void (*get)(const void *, const void **, const void **) =
        (void (*)(const void *, const void **, const void **))
            plsr_image_symbol("_CFDictionaryGetKeysAndValues");
    if (!get || !self || !objects || !keys || !count) return;
    unsigned long total = plsr_dict_count(self, NULL);
    if (!total) return;
    const void **kb = calloc(total, sizeof(*kb));
    const void **vb = calloc(total, sizeof(*vb));
    if (!kb || !vb) { free(kb); free(vb); return; }
    get(self, kb, vb);
    if (count > total) count = total;
    for (unsigned long i = 0; i < count; i++) {
        /* The compatibility dictionaries can contain Swift-native values
         * that are not Objective-C objects. Passing one through Foundation
         * makes libobjc interpret its first word as isa and walk a bogus
         * relative method list (the CotEditor launch crash in Sparkle). Keep
         * the output arrays valid while replacing such entries with a stable
         * placeholder string. */
        if (!plsr_valid_obj(kb[i]) || !plsr_valid_obj(vb[i])) {
            void *unknown = plsr_unknown_dictionary_value();
            if (getenv("PLSR_REEF_TRACE"))
                fprintf(stderr, "reef: skipped non-object dictionary pair key=%p value=%p\n",
                        kb[i], vb[i]);
            keys[i] = unknown;
            objects[i] = unknown;
            continue;
        }
        keys[i] = (void *)kb[i];
        objects[i] = (void *)vb[i];
    }
    free(kb); free(vb);
}

static void plsr_dict_set_object(void *self, void *cmd, void *obj, void *key) {
    (void)cmd;
    if (!self || !key || !plsr_valid_obj(key)) return;
    if (obj && !plsr_valid_obj(obj)) return;
    if (!obj) {
        void (*rm)(void *, const void *) =
            (void (*)(void *, const void *))plsr_image_symbol("_CFDictionaryRemoveValue");
        if (rm) rm(self, key);
        return;
    }
    void (*set)(void *, const void *, const void *) =
        (void (*)(void *, const void *, const void *))
            plsr_image_symbol("_CFDictionarySetValue");
    if (set) set(self, key, obj);
}

static void *plsr_dict_init_objects_for_keys(void *self, void *cmd,
        const void **objects, const void **keys) {
    unsigned long n = 0;
    while (keys && keys[n]) n++;
    return plsr_dict_init_objects_for_keys_count(self, cmd, objects, keys, n);
}
static void *plsr_set_init(void *self, void *cmd) {
    (void)self; (void)cmd; return plsr_cf_set(0);
}
static void *plsr_set_class_set(void *cls, void *cmd) {
    (void)cls; (void)cmd; return plsr_cf_set(0);
}
static void *plsr_set_init_array(void *self, void *cmd, void *array) {
    (void)self; (void)cmd;
    void *set = plsr_cf_set(0);
    void *(*msg0)(void *, void *) =
        (void *(*)(void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*msg1)(void *, void *, void *) =
        (void *(*)(void *, void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    if (!set || !array || !msg0 || !msg1 || !sel) return set;
    unsigned long n = (unsigned long)msg0(array, sel("count"));
    for (unsigned long i = 0; i < n; i++) {
        void *o = msg1(array, sel("objectAtIndex:"), (void *)i);
        if (o) msg1(set, sel("addObject:"), o);
    }
    return set;
}
static void *plsr_set_class_set_with_array(void *cls, void *cmd, void *array) {
    (void)cls; (void)cmd;
    return plsr_set_init_array(NULL, NULL, array);
}
static void *plsr_identity_init(void *self, void *cmd) {
    (void)cmd; return self;
}
static void plsr_noop_release(void *self, void *cmd) {
    (void)self; (void)cmd;
}
/* CF arrays reject nil in -addObject:.  The archiver has one path that can
 * produce nil here; log its caller and skip the append instead of aborting the
 * whole launch, so the remaining UI path can be exercised. */
static void (*g_add_object_imp)(void *, void *, void *);
static void plsr_array_add_object(void *self, void *cmd, void *obj) {
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: addObject self=%p obj=%p imp=%p\n",
                self, obj, (void *)g_add_object_imp);
    if (!obj) {
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: nil addObject skipped self=%p caller=%p\n",
                    self, __builtin_return_address(0));
        return;
    }
    if (g_add_object_imp) {
        g_add_object_imp(self, cmd, obj);
        return;
    }
    void (*append)(void *, const void *) =
        (void (*)(void *, const void *))plsr_image_symbol("_CFArrayAppendValue");
    if (append) append(self, obj);
}
static void *plsr_array_alloc(void *cls, void *cmd) {
    (void)cls; (void)cmd; return plsr_cf_array(0);
}
static void *plsr_dict_alloc(void *cls, void *cmd) {
    (void)cls; (void)cmd; return plsr_cf_dictionary(0);
}
static void *plsr_set_alloc(void *cls, void *cmd) {
    (void)cls; (void)cmd; return plsr_cf_set(0);
}
/* __NSKeyedCodingContainer instances carry a value at +8, but the class's
 * normal instance size is only the isa word.  Reserve one extra word so the
 * archiver can write that value without corrupting the heap. */
static void *plsr_cc_alloc(void *cls, void *cmd) {
    (void)cmd;
    void *(*create)(void *, unsigned long) =
        (void *(*)(void *, unsigned long))plsr_image_symbol("_class_createInstance");
    void *r = create ? create(cls, 8) : NULL;
    if (!r) {
        void *(*alloc)(void *) =
            (void *(*)(void *))plsr_image_symbol("_objc_alloc");
        r = alloc ? alloc(cls) : NULL;
    }
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: cc_alloc cls=%p -> %p\n", cls, r);
    return r;
}

/* Some archivers reach finishEncoding with an empty _containers array (the
 * Sparkle state archiver builds one through a path whose initializer is not
 * the designated one).  finishEncoding unconditionally takes objectAtIndex:0
 * and dereferences the result, so seed a valid UID container when empty. */
static void (*g_finish_encoding)(void *, void *);
static void plsr_finish_encoding(void *self, void *cmd) {
    /* Early AppKit bootstrap may ask Sparkle to archive transient state before
     * any keyed-coding container has been established. The bundle does not
     * consume that result for launch; do not enter Foundation's unchecked
     * empty-container path while creating the real application delegate. */
    if (getenv("PLSR_SKIP_FINISH_ENCODING") || getenv("PLSR_NSWINDOW") ||
        getenv("PLSR_BOOTSTRAP_DELEGATE")) return;
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: finishEncoding guard self=%p orig=%p\n",
                self, (void *)g_finish_encoding);
    void *(*getcls)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_objc_getClass");
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    void *(*msg0)(void *, void *) =
        (void *(*)(void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*msg1)(void *, void *, void *) =
        (void *(*)(void *, void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*getivar)(void *, const char *) =
        (void *(*)(void *, const char *))plsr_image_symbol("_class_getInstanceVariable");
    long (*ivoff)(void *) =
        (long (*)(void *))plsr_image_symbol("_ivar_getOffset");
    if (getcls && sel && msg0 && msg1 && getivar && ivoff) {
        void *ka = getcls("NSKeyedArchiver");
        void *iv = ka ? getivar(ka, "_containers") : NULL;
        long off = iv ? ivoff(iv) : -1;
        if (off >= 0) {
            void *arr = *(void **)((char *)self + off);
            if (arr) {
                unsigned long n = (unsigned long)msg0(arr, sel("count"));
                void *first = n ? msg1(arr, sel("objectAtIndex:"), (void *)0) : NULL;
                if (n == 0 || !first) {
                    void *cc = getcls("__NSKeyedCodingContainer");
                    void *uid = cc ? msg0(msg0(cc, sel("alloc")), sel("init")) : NULL;
                    void *nullcls = getcls("NSNull");
                    void *nullobj = nullcls ? msg0(nullcls, sel("null")) : NULL;
                    if (uid && nullobj) {
                        *(void **)((char *)uid + 8) = nullobj;
                        msg1(arr, sel("addObject:"), uid);
                    }
                }
            }
        }
    }
    if (g_finish_encoding) g_finish_encoding(self, cmd);
}

static void (*g_exc_raise_format)(void *, void *, void *, void *, ...);
static void plsr_exc_raise_format(void *cls, void *cmd, void *name, void *format, ...) {
    (void)cls; (void)cmd; (void)name; (void)format;
    fprintf(stderr, "reef: swallowed +[NSException raise:format:]\n");
}
static void plsr_exc_raise_format_args(void *cls, void *cmd, void *name,
                                       void *format, void *args) {
    (void)cls; (void)cmd; (void)name; (void)format; (void)args;
    fprintf(stderr, "reef: swallowed +[NSException raise:format:arguments:]\n");
}

/* CFBundle's resource lookup lists Resources/Base.lproj and receives
 * Main.storyboardc from readdir, yet _CFBundleCopyResourceURL still returns
 * nil, so +[NSStoryboard storyboardWithName:bundle:] raises
 * "Storyboard (Main) doesn't exist in bundle".  Answer the Foundation-level
 * question from the bundle on disk instead: when -[NSBundle
 * pathForResource:ofType:inDirectory:] returns nil, search the resource
 * directory ourselves (requested subdirectory, Base/en/English.lproj, then any
 * *.lproj). */
static char *plsr_nsutf8(void *s) {
    if (!s) return NULL;
    char *(*msg)(void *, void *) =
        (char *(*)(void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    if (!msg || !sel) return NULL;
    char *r = msg(s, sel("UTF8String"));
    if (r) return r;
    /* CFString-backed strings have no -UTF8String here; copy through CF. */
    signed char (*cfget)(const void *, char *, long, unsigned) =
        (signed char (*)(const void *, char *, long, unsigned))
            plsr_image_symbol("_CFStringGetCString");
    static __thread char buf[1024];
    if (cfget && cfget(s, buf, (long)sizeof buf, 0x08000100u)) return buf;
    return NULL;
}

static void *plsr_nsstring(const char *s) {
    void *(*getcls)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_objc_getClass");
    void *(*msg1)(void *, void *, void *) =
        (void *(*)(void *, void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    void *c = getcls ? getcls("NSString") : NULL;
    void *r = (c && msg1 && sel)
                  ? msg1(c, sel("stringWithUTF8String:"), (void *)s)
                  : NULL;
    if (r) return r;
    /* Foundation's string class cluster does not implement the constructor on
     * this runtime (it returns nil), which made the appended Info.plist path
     * nil and AppKit report the storyboard as missing.  A CFString is
     * toll-free bridged and works everywhere an NSString is expected. */
    void *(*cfstr)(const void *, const char *, unsigned) =
        (void *(*)(const void *, const char *, unsigned))
            plsr_image_symbol("_CFStringCreateWithCString");
    if (cfstr) r = cfstr(NULL, s, 0x08000100u);   /* kCFStringEncodingUTF8 */
    return r;
}


/* -[NSBundle resourcePath] and -bundlePath both go through the CFBundle lookup
 * that is broken on this runtime, so derive the resources directory from the
 * .app the loader executed. */
static int plsr_bundle_resources_dir(char *out, size_t cap) {
    if (!g_main || !g_main->path) return 0;
    const char *dot = strstr(g_main->path, ".app");
    if (!dot) return 0;
    const int n = (int)((dot - g_main->path) + 4);
    snprintf(out, cap, "%.*s/Contents/Resources", n, g_main->path);
    return out[0] != '\0';
}

/* AppKit builds "<resolved storyboard>/Info.plist" with
 * -[NSString stringByAppendingPathComponent:].  The runtime's string class
 * cluster does not implement it, so the append returns nil and
 * dictionaryWithContentsOfFile: receives nil, which AppKit reports as
 * "Storyboard ... doesn't exist".  Compose the path ourselves. */
static void *(*g_str_append_orig)(void *, void *, void *);
static void *plsr_string_by_appending_path(void *self, void *cmd, void *comp) {
    if (g_str_append_orig) {
        void *r = g_str_append_orig(self, cmd, comp);
        if (r) return r;
    }
    const char *s = plsr_nsutf8(self);
    const char *c = comp ? plsr_nsutf8(comp) : NULL;
    if (!s || !*s || !c || !*c) return NULL;
    size_t n = strlen(s);
    while (n > 1 && s[n - 1] == '/') --n;
    char buf[PATH_MAX];
    if (n + strlen(c) + 2 >= sizeof buf) return NULL;
    memcpy(buf, s, n);
    buf[n] = '/';
    strcpy(buf + n + 1, c);
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: stringByAppendingPathComponent '%s' + '%s' -> %s\n",
                s, c, buf);
    return plsr_nsstring(buf);
}

/* AppKit resolves the storyboard through a path lookup but then appends
 * "Info.plist" with -URLByAppendingPathComponent: (an NSURL selector) before
 * handing the result to dictionaryWithContentsOfFile:.  On a string that
 * selector is unknown and returns nil, which AppKit reports as a missing
 * storyboard.  Answer it with the appended path string the loader expects. */
static void *plsr_string_url_by_appending(void *self, void *cmd, void *comp) {
    (void)cmd;
    const char *base = plsr_nsutf8(self);
    const char *component = comp ? plsr_nsutf8(comp) : NULL;
    if (!base || !*base || !component || !*component) return NULL;
    size_t n = strlen(base);
    while (n > 1 && base[n - 1] == '/') --n;
    char path[PATH_MAX];
    if (n + strlen(component) + 2 >= sizeof path) return NULL;
    memcpy(path, base, n);
    path[n] = '/';
    strcpy(path + n + 1, component);

    void *(*getcls)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_objc_getClass");
    void *(*msg1)(void *, void *, void *) =
        (void *(*)(void *, void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    void *path_string = plsr_nsstring(path);
    void *url_class = getcls ? getcls("NSURL") : NULL;
    void *url = url_class && msg1 && sel && path_string
        ? msg1(url_class, sel("fileURLWithPath:"), path_string) : NULL;
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: URLByAppendingPathComponent '%s' + '%s' -> NSURL %p\n",
                base, component, url);
    return url;
}

/* AppKit raises the same "Storyboard ... doesn't exist" message when the
 * storyboard's own Info.plist cannot be loaded, so trace that load too. */
static void *(*g_dict_file_orig)(void *, void *, void *);

/* The packaged Foundation's +dictionaryWithContentsOfFile: currently returns
 * nil for valid binary plists in this runtime.  Rebuild the method's behavior
 * with the loaded CoreFoundation property-list parser and return its bridged
 * dictionary object, which remains usable by Objective-C callers. */
static void *plsr_dictionary_from_property_list_file(const char *path) {
    if (!path || !*path) return NULL;

    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long length = ftell(file);
    if (length <= 0 || length > 64L * 1024L * 1024L ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    unsigned char *bytes = malloc((size_t)length);
    if (!bytes) {
        fclose(file);
        return NULL;
    }
    size_t read_count = fread(bytes, 1, (size_t)length, file);
    fclose(file);
    if (read_count != (size_t)length) {
        free(bytes);
        return NULL;
    }

    void *(*data_create)(const void *, const unsigned char *, long) =
        (void *(*)(const void *, const unsigned char *, long))
            plsr_image_symbol("_CFDataCreate");
    void *(*plist_create)(const void *, const void *, unsigned long,
                          void *, void *) =
        (void *(*)(const void *, const void *, unsigned long, void *, void *))
            plsr_image_symbol("_CFPropertyListCreateWithData");
    void (*cf_release)(const void *) =
        (void (*)(const void *))plsr_image_symbol("_CFRelease");
    unsigned long (*cf_type_id)(const void *) =
        (unsigned long (*)(const void *))plsr_image_symbol("_CFGetTypeID");
    unsigned long (*dict_type_id)(void) =
        (unsigned long (*)(void))plsr_image_symbol("_CFDictionaryGetTypeID");
    void *(*objc_autorelease)(void *) =
        (void *(*)(void *))plsr_image_symbol("_objc_autorelease");

    if (!data_create || !plist_create || !cf_release) {
        free(bytes);
        return NULL;
    }
    void *data = data_create(NULL, bytes, length);
    free(bytes);
    if (!data) return NULL;

    void *object = plist_create(NULL, data, 0, NULL, NULL);
    cf_release(data);
    if (!object) return NULL;

    if (cf_type_id && dict_type_id &&
        cf_type_id(object) != dict_type_id()) {
        cf_release(object);
        return NULL;
    }
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: CoreFoundation decoded plist '%s' -> %p\n",
                path, object);
    return objc_autorelease ? objc_autorelease(object) : object;
}

static void *plsr_dict_with_contents_of_file(void *cls, void *cmd, void *path) {
    void *arg = path;
    const char *p = plsr_nsutf8(path);
    if (!p && path) {
        /* AppKit reaches this with an NSURL when it treats the storyboard
         * resource as a URL; -UTF8String is nil for it, and the original
         * returns nil, which AppKit reports as a missing storyboard. */
        void *(*msg0)(void *, void *) =
            (void *(*)(void *, void *))plsr_image_symbol("_objc_msgSend");
        void *(*sel0)(const char *) =
            (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
        if (msg0 && sel0) {
            void *u = msg0(path, sel0("path"));
            const char *up = u ? plsr_nsutf8(u) : NULL;
            if (up) {
                arg = u;
                p = up;
            }
        }
    }
    void *r = p ? plsr_dictionary_from_property_list_file(p) : NULL;
    if (!r && g_dict_file_orig)
        r = g_dict_file_orig(cls, cmd, arg);
    if (getenv("PLSR_REEF_TRACE")) {
        void *(*getobjclass)(void *) =
            (void *(*)(void *))plsr_image_symbol("_object_getClass");
        const char *(*clsname)(void *) =
            (const char *(*)(void *))plsr_image_symbol("_class_getName");
        void *c = (arg && getobjclass) ? getobjclass(arg) : NULL;
        fprintf(stderr, "reef: dictionaryWithContentsOfFile '%s' (%s) -> %p\n",
                p ? p : "(nil)", (c && clsname) ? clsname(c) : "?", r);
    }
    return r;
}

/* AppKit's storyboard fallback starts from -[NSBundle resourcePath] (selref
 * +0x1b65c6) and appends candidates to it; CFBundle returns nil here, so every
 * appended path is nil and the later dictionaryWithContentsOfFile: receives
 * nil.  Answer resourcePath/bundlePath from the .app the loader executed. */
static void *(*g_resource_path_orig)(void *, void *);
static void *(*g_bundle_path_orig)(void *, void *);

static void *plsr_nsbundle_resource_path(void *self, void *cmd) {
    void *r = g_resource_path_orig ? g_resource_path_orig(self, cmd) : NULL;
    const char *p = plsr_nsutf8(r);
    if (p && *p) return r;
    char res[PATH_MAX];
    if (!plsr_bundle_resources_dir(res, sizeof res)) return r;
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: resourcePath -> %s\n", res);
    return plsr_nsstring(res);
}

static void *plsr_nsbundle_bundle_path(void *self, void *cmd) {
    void *r = g_bundle_path_orig ? g_bundle_path_orig(self, cmd) : NULL;
    const char *p = plsr_nsutf8(r);
    if (p && *p) return r;
    if (!g_main || !g_main->path) return r;
    const char *dot = strstr(g_main->path, ".app");
    if (!dot) return r;
    char buf[PATH_MAX];
    const int n = (int)((dot - g_main->path) + 4);
    snprintf(buf, sizeof buf, "%.*s", n, g_main->path);
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: bundlePath -> %s\n", buf);
    return plsr_nsstring(buf);
}

static void *(*g_nsbundle_path_orig)(void *, void *, void *, void *, void *);

static void *plsr_nsbundle_path_for_resource(void *self, void *cmd, void *name,
                                             void *type, void *dir) {
    const char *pre_name = plsr_nsutf8(name);
    const char *pre_type = plsr_nsutf8(type);
    if (getenv("PLSR_STORYBOARD_NOOP") && pre_name && pre_type &&
        strcmp(pre_name, "Main") == 0 && strcmp(pre_type, "storyboardc") == 0) {
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: storyboard noop, skipping %s.%s\n",
                    pre_name, pre_type);
        return NULL;
    }
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: NSBundle pathForResource name=%s type=%s dir=%s\n",
                plsr_nsutf8(name) ? plsr_nsutf8(name) : "(nil)",
                plsr_nsutf8(type) ? plsr_nsutf8(type) : "(nil)",
                plsr_nsutf8(dir) ? plsr_nsutf8(dir) : "(nil)");
    if (g_nsbundle_path_orig) {
        void *r = g_nsbundle_path_orig(self, cmd, name, type, dir);
        if (r) {
            const char *p = plsr_nsutf8(r);
            if (p && access(p, F_OK) == 0) {
                if (getenv("PLSR_REEF_TRACE"))
                    fprintf(stderr, "reef: accepted CFBundle path '%s'\n", p);
                return r;
            }
            if (getenv("PLSR_REEF_TRACE"))
                fprintf(stderr, "reef: rejected CFBundle path '%s'\n", p ? p : "(nil)");
        }
    }
    const char *n = plsr_nsutf8(name);
    const char *t = plsr_nsutf8(type);
    const char *d = plsr_nsutf8(dir);
    if (!n || !*n) return NULL;
    char suffix[512];
    if (t && *t) snprintf(suffix, sizeof suffix, "%s.%s", n, t);
    else snprintf(suffix, sizeof suffix, "%s", n);
    void *(*msg)(void *, void *) =
        (void *(*)(void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    char res[PATH_MAX] = "";
    if (msg && sel) {
        const char *rp = msg(self, sel("resourcePath"));
        if (rp && *rp) snprintf(res, sizeof res, "%s", rp);
        if (!res[0]) {
            const char *bp = msg(self, sel("bundlePath"));
            if (bp && *bp) snprintf(res, sizeof res, "%s/Contents/Resources", bp);
        }
    }
    if (!res[0]) plsr_bundle_resources_dir(res, sizeof res);
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: resource search root=%s\n", res[0] ? res : "(none)");
    if (!res[0]) return NULL;
    char cand[PATH_MAX];
    const char *dirs[] = { (d && *d) ? d : NULL, "Base.lproj", "en.lproj",
                           "English.lproj" };
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
        if (!dirs[i]) continue;
        snprintf(cand, sizeof cand, "%s/%s/%s", res, dirs[i], suffix);
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: candidate %s\n", cand);
        if (access(cand, F_OK) == 0) return plsr_nsstring(cand);
    }
    snprintf(cand, sizeof cand, "%s/%s", res, suffix);
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: candidate %s\n", cand);
    if (access(cand, F_OK) == 0) return plsr_nsstring(cand);
    DIR *dh = opendir(res);
    if (dh) {
        struct dirent *e;
        while ((e = readdir(dh)) != NULL) {
            const size_t len = strlen(e->d_name);
            if (len < 6 || strcmp(e->d_name + len - 6, ".lproj")) continue;
            snprintf(cand, sizeof cand, "%s/%s/%s", res, e->d_name, suffix);
            if (access(cand, F_OK) == 0) {
                closedir(dh);
                return plsr_nsstring(cand);
            }
        }
        closedir(dh);
    }
    return NULL;
}

/* +[NSStoryboard storyboardWithName:bundle:] reaches the resource through
 * -[NSBundle URLForResource:withExtension:subdirectory:], whose CFBundle
 * implementation returns nil for the same reason.  Answer that from the path
 * lookup above (which resolves the on-disk storyboard) and wrap it in a file
 * URL. */
static void *(*g_nsbundle_url_orig)(void *, void *, void *, void *, void *);

static void *plsr_nsbundle_url_for_resource(void *self, void *cmd, void *name,
                                            void *type, void *dir) {
    const char *pre_name = plsr_nsutf8(name);
    const char *pre_type = plsr_nsutf8(type);
    if (getenv("PLSR_STORYBOARD_NOOP") && pre_name && pre_type &&
        strcmp(pre_name, "Main") == 0 && strcmp(pre_type, "storyboardc") == 0) {
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: storyboard noop, skipping URL %s.%s\n",
                    pre_name, pre_type);
        return NULL;
    }
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: NSBundle URLForResource name=%s type=%s dir=%s\n",
                plsr_nsutf8(name) ? plsr_nsutf8(name) : "(nil)",
                plsr_nsutf8(type) ? plsr_nsutf8(type) : "(nil)",
                plsr_nsutf8(dir) ? plsr_nsutf8(dir) : "(nil)");
    if (g_nsbundle_url_orig) {
        void *r = g_nsbundle_url_orig(self, cmd, name, type, dir);
        if (r) {
            void *(*msg0)(void *, void *) =
                (void *(*)(void *, void *))plsr_image_symbol("_objc_msgSend");
            void *(*sel0)(const char *) =
                (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
            void *u = (msg0 && sel0) ? msg0(r, sel0("path")) : NULL;
            const char *p = plsr_nsutf8(u);
            if (p && access(p, F_OK) == 0) {
                if (getenv("PLSR_REEF_TRACE"))
                    fprintf(stderr, "reef: accepted CFBundle URL path '%s'\n", p);
                return r;
            }
            if (getenv("PLSR_REEF_TRACE"))
                fprintf(stderr, "reef: rejected CFBundle URL path '%s'\n",
                        p ? p : "(nil)");
        }
    }
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: URLForResource building from path for %s\n",
                name ? "name" : "(nil)");
    void *(*msg4)(void *, void *, void *, void *, void *) =
        (void *(*)(void *, void *, void *, void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*msg1)(void *, void *, void *) =
        (void *(*)(void *, void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    void *(*getcls)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_objc_getClass");
    if (!msg4 || !msg1 || !sel || !getcls) return NULL;
    void *path = msg4(self, sel("pathForResource:ofType:inDirectory:"), name, type, dir);
    if (!path) return NULL;
    void *urlcls = getcls("NSURL");
    return urlcls ? msg1(urlcls, sel("fileURLWithPath:"), path) : NULL;
}

/* AppKit's storyboard lookup uses the two-argument forms on some paths; those
 * are separate methods in Foundation and do not forward to the subdirectory
 * variants we already answer. */
static void *plsr_nsbundle_path_for_resource2(void *self, void *cmd, void *name,
                                              void *type) {
    return plsr_nsbundle_path_for_resource(self, cmd, name, type, NULL);
}
static void *plsr_nsbundle_url_for_resource2(void *self, void *cmd, void *name,
                                             void *type) {
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: NSBundle URLForResource2 name=%s type=%s\n",
                plsr_nsutf8(name) ? plsr_nsutf8(name) : "(nil)",
                plsr_nsutf8(type) ? plsr_nsutf8(type) : "(nil)");
    return plsr_nsbundle_url_for_resource(self, cmd, name, type, NULL);
}

/* Decode the RIP-relative loads in a function and print the selector names
 * their slots point at after binding.  Used to identify exactly which NSBundle
 * calls +[NSStoryboard storyboardWithName:bundle:] (AppKit+0x1b6230) makes,
 * instead of guessing method names. */
static void plsr_dump_selrefs(const char *spec) {
    if (!spec || !*spec) return;
    const unsigned long off = strtoul(spec, NULL, 0);
    for (int i = 0; i < g_nimgs; i++) {
        if (!strstr(g_imgs[i]->path, "AppKit.framework")) continue;
        uintptr_t img = (uintptr_t)(g_imgs[i]->base + g_imgs[i]->slide);
        const unsigned char *p = (const unsigned char *)(img + off);
        for (size_t k = 0; k + 8 < 0x700; ) {
            if ((p[k] == 0x48 || p[k] == 0x4c) && p[k + 1] == 0x8b &&
                (p[k + 2] & 0xc7) == 0x05) {
                int32_t disp;
                memcpy(&disp, p + k + 3, 4);
                uintptr_t tgt = (uintptr_t)(p + k + 7) + (intptr_t)disp;
                uintptr_t val = 0;
                memcpy(&val, (const void *)tgt, sizeof val);
                const char *s = (const char *)val;
                int printable = s != NULL;
                for (int j = 0; printable && j < 96; j++) {
                    const unsigned char c = (unsigned char)s[j];
                    if (c == 0) break;
                    if (c < 0x20 || c > 0x7e) printable = 0;
                }
                if (printable && s && s[0])
                    fprintf(stderr, "reef: selref +%#lx -> %s\n",
                            (unsigned long)((uintptr_t)(p + k) - img), s);
                k += 7;
                continue;
            }
            ++k;
        }
        return;
    }
}

static void *plsr_storyboard_with_name(void *cls, void *cmd, void *name,
                                       void *bundle) {
    (void)cls; (void)cmd; (void)name; (void)bundle;
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: storyboardWithName -> nil (storyboard noop)\n");
    return NULL;
}

static void install_bundle_path_fallback(void) {
    if (getenv("PLSR_NO_BUNDLE_FALLBACK")) return;
    void *(*getcls)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_objc_getClass");
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    void *(*getimp)(void *, void *) =
        (void *(*)(void *, void *))plsr_image_symbol("_class_getMethodImplementation");
    void *(*repl)(void *, void *, void *, const char *) =
        (void *(*)(void *, void *, void *, const char *))
            plsr_image_symbol("_class_replaceMethod");
    if (!getcls || !sel || !getimp || !repl) return;
    void *nb = getcls("NSBundle");
    if (getenv("PLSR_REEF_TRACE")) fprintf(stderr, "reef: bundle fallback class=%p\n", nb);
    if (!nb) return;
    void *s = sel("pathForResource:ofType:inDirectory:");
    g_nsbundle_path_orig = (void *(*)(void *, void *, void *, void *, void *))
        getimp(nb, s);
    if (getenv("PLSR_REEF_TRACE")) fprintf(stderr, "reef: bundle path imp=%p\n", (void *)g_nsbundle_path_orig);
    repl(nb, s, (void *)plsr_nsbundle_path_for_resource, "@@:@@@");
    void *us = sel("URLForResource:withExtension:subdirectory:");
    g_nsbundle_url_orig = (void *(*)(void *, void *, void *, void *, void *))
        getimp(nb, us);
    if (getenv("PLSR_REEF_TRACE")) fprintf(stderr, "reef: bundle url imp=%p\n", (void *)g_nsbundle_url_orig);
    repl(nb, us, (void *)plsr_nsbundle_url_for_resource, "@@:@@@");
    void *ps2 = sel("pathForResource:ofType:");
    repl(nb, ps2, (void *)plsr_nsbundle_path_for_resource2, "@@:@");
    void *us2 = sel("URLForResource:withExtension:");
    repl(nb, us2, (void *)plsr_nsbundle_url_for_resource2, "@@:@");
    void *rs = sel("resourcePath");
    g_resource_path_orig = (void *(*)(void *, void *))getimp(nb, rs);
    if (getenv("PLSR_REEF_TRACE")) fprintf(stderr, "reef: bundle resourcePath imp=%p\n", (void *)g_resource_path_orig);
    repl(nb, rs, (void *)plsr_nsbundle_resource_path, "@@:");
    void *bs = sel("bundlePath");
    g_bundle_path_orig = (void *(*)(void *, void *))getimp(nb, bs);
    if (getenv("PLSR_REEF_TRACE")) fprintf(stderr, "reef: bundle bundlePath imp=%p\n", (void *)g_bundle_path_orig);
    repl(nb, bs, (void *)plsr_nsbundle_bundle_path, "@@:");
    /* The bundle AppKit asks may be an instance of a private NSBundle subclass;
     * patch that class as well so the wrappers are actually reached. */
    void *(*msg0)(void *, void *) =
        (void *(*)(void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*getobjclass)(void *) =
        (void *(*)(void *))plsr_image_symbol("_object_getClass");
    if (getenv("PLSR_STORYBOARD_NOOP") && getobjclass) {
        void *storyboard = getcls("NSStoryboard");
        void *meta = storyboard ? getobjclass(storyboard) : NULL;
        if (meta) {
            repl(meta, sel("storyboardWithName:bundle:"),
                 (void *)plsr_storyboard_with_name, "@@:@@");
            if (getenv("PLSR_REEF_TRACE"))
                fprintf(stderr, "reef: patched +[NSStoryboard storyboardWithName:bundle:]\n");
        }
    }
    if (!getenv("PLSR_NO_BUNDLE_PROBE") && msg0 && getobjclass) {
        if (getenv("PLSR_REEF_TRACE")) fprintf(stderr, "reef: probing +[NSBundle mainBundle]\n");
        void *mb = msg0(nb, sel("mainBundle"));
        void *mc = mb ? getobjclass(mb) : NULL;
        if (mc && mc != nb) {
            g_nsbundle_path_orig =
                (void *(*)(void *, void *, void *, void *, void *))
                    getimp(mc, s);
            repl(mc, s, (void *)plsr_nsbundle_path_for_resource, "@@:@@@");
            g_nsbundle_url_orig =
                (void *(*)(void *, void *, void *, void *, void *))
                    getimp(mc, us);
            repl(mc, us, (void *)plsr_nsbundle_url_for_resource, "@@:@@@");
            repl(mc, ps2, (void *)plsr_nsbundle_path_for_resource2, "@@:@");
            repl(mc, us2, (void *)plsr_nsbundle_url_for_resource2, "@@:@");
            if (getenv("PLSR_REEF_TRACE"))
                fprintf(stderr, "reef: patched bundle class %p (%s)\n", mc,
                        mb ? "mainBundle" : "?");
        }
    }
    const char *str_classes[] = { "__NSCFString", "NSString" };
    for (size_t si = 0; si < sizeof str_classes / sizeof str_classes[0]; si++) {
        void *sc = getcls(str_classes[si]);
        if (!sc) continue;
        void *as = sel("stringByAppendingPathComponent:");
        if (!g_str_append_orig)
            g_str_append_orig = (void *(*)(void *, void *, void *))getimp(sc, as);
        repl(sc, as, (void *)plsr_string_by_appending_path, "@@:@");
        repl(sc, sel("URLByAppendingPathComponent:"),
             (void *)plsr_string_url_by_appending, "@@:@");
    }
    void *dc = getcls("NSDictionary");
    void *(*getclass)(void *) =
        (void *(*)(void *))plsr_image_symbol("_object_getClass");
    if (dc && getclass) {
        void *dm = getclass(dc);
        void *ds = sel("dictionaryWithContentsOfFile:");
        if (dm) {
            g_dict_file_orig =
                (void *(*)(void *, void *, void *))getimp(dm, ds);
            repl(dm, ds, (void *)plsr_dict_with_contents_of_file, "@@:@");
        }
    }
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: NSBundle resource fallback installed (path=%p url=%p)\n",
                (void *)g_nsbundle_path_orig, (void *)g_nsbundle_url_orig);
}

/* Bundle/resource code in this Foundation build sometimes treats the
 * toll-free-bridged __NSCFArray as a byte container: it asks for -length and
 * indexes it.  Provide the data-shaped accessors on the CF array and make the
 * index accessor bounds-safe instead of raising NSRangeException. */
static unsigned long plsr_cf_array_length(void *self, void *cmd) {
    (void)cmd;
    unsigned long (*getcount)(const void *) =
        (unsigned long (*)(const void *))plsr_image_symbol("_CFArrayGetCount");
    return (self && getcount) ? getcount(self) : 0;
}
static void *plsr_cf_array_object_at(void *self, void *cmd, unsigned long index) {
    (void)cmd;
    unsigned long (*getcount)(const void *) =
        (unsigned long (*)(const void *))plsr_image_symbol("_CFArrayGetCount");
    const void *(*getvalue)(const void *, unsigned long) =
        (const void *(*)(const void *, unsigned long))
            plsr_image_symbol("_CFArrayGetValueAtIndex");
    if (!self || !getcount || !getvalue || index >= getcount(self)) return NULL;
    return (void *)getvalue(self, index);
}

/* AppKit's saved-state path code can receive a resource array where it expects
 * a string and send -stringByAppendingString: to it.  Forward the append to
 * the array's first element when that element is a string; otherwise keep the
 * appended component as the result instead of raising NSInvalidArgumentException. */
static int plsr_cfstring_to_c(void *s, char *buf, size_t cap) {
    unsigned long (*gettype)(const void *) =
        (unsigned long (*)(const void *))plsr_image_symbol("_CFGetTypeID");
    unsigned long (*strtype)(void) =
        (unsigned long (*)(void))plsr_image_symbol("_CFStringGetTypeID");
    unsigned char (*getc)(const void *, char *, long, unsigned int) =
        (unsigned char (*)(const void *, char *, long, unsigned int))
            plsr_image_symbol("_CFStringGetCString");
    if (!s || !gettype || !strtype || !getc || gettype(s) != strtype()) return 0;
    return getc(s, buf, (long)cap, 0x08000100) != 0;  /* UTF-8 */
}

/* CFString objects reached through the generic __NSCFType class do not answer
 * NSString selectors in this runtime.  Convert them through CFString and build
 * the result with the normal NSString factory. */
static void *plsr_cftype_string_append(void *self, void *cmd, void *arg) {
    char a[PATH_MAX], b[PATH_MAX], out[PATH_MAX * 2];
    if (!plsr_cfstring_to_c(self, a, sizeof a) ||
        !plsr_cfstring_to_c(arg, b, sizeof b)) return arg;
    const char *(*selname)(void *) =
        (const char *(*)(void *))plsr_image_symbol("_sel_getName");
    const char *name = selname ? selname(cmd) : "";
    if (strstr(name, "PathComponent")) {
        size_t n = strlen(a);
        int sep = n > 0 && a[n - 1] != '/';
        snprintf(out, sizeof out, "%s%s%s", a, sep ? "/" : "", b);
    } else {
        snprintf(out, sizeof out, "%s%s", a, b);
    }
    return plsr_nsstring(out);
}

static void *plsr_cf_array_string_append(void *self, void *cmd, void *arg) {
    unsigned long (*getcount)(const void *) =
        (unsigned long (*)(const void *))plsr_image_symbol("_CFArrayGetCount");
    const void *(*getvalue)(const void *, unsigned long) =
        (const void *(*)(const void *, unsigned long))
            plsr_image_symbol("_CFArrayGetValueAtIndex");
    void *(*msg1)(void *, void *, void *) =
        (void *(*)(void *, void *, void *))plsr_image_symbol("_objc_msgSend");
    if (self && getcount && getvalue && msg1 && getcount(self) > 0) {
        void *first = (void *)getvalue(self, 0);
        if (first && first != self) return msg1(first, cmd, arg);
    }
    return arg;
}

static void patch_cf_array_add_nil(void) {
    void *(*getcls)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_objc_getClass");
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    void *(*getimp)(void *, void *) =
        (void *(*)(void *, void *))plsr_image_symbol("_class_getMethodImplementation");
    void *(*repl)(void *, void *, void *, const char *) =
        (void *(*)(void *, void *, void *, const char *))
            plsr_image_symbol("_class_replaceMethod");
    if (!getcls || !sel || !getimp || !repl) return;
    void *array = getcls("__NSCFArray");
    if (!array) return;
    void *add = sel("addObject:");
    g_add_object_imp = (void (*)(void *, void *, void *))getimp(array, add);
    if (!g_add_object_imp) return;
    repl(array, add, (void *)plsr_array_add_object, "v@:@");
    repl(array, sel("length"), (void *)plsr_cf_array_length, "Q@:");
    repl(array, sel("objectAtIndex:"), (void *)plsr_cf_array_object_at, "@@:Q");
    void *append = sel("stringByAppendingString:");
    void *append_path = sel("stringByAppendingPathComponent:");
    repl(array, append, (void *)plsr_cf_array_string_append, "@@:@");
    repl(array, append_path, (void *)plsr_cf_array_string_append, "@@:@");
    void *empty = getcls("__NSArray0");
    if (empty && empty != array) {
        repl(empty, append, (void *)plsr_cf_array_string_append, "@@:@");
        repl(empty, append_path, (void *)plsr_cf_array_string_append, "@@:@");
    }
    void *cftype = getcls("__NSCFType");
    if (cftype) {
        repl(cftype, append, (void *)plsr_cftype_string_append, "@@:@");
        repl(cftype, append_path, (void *)plsr_cftype_string_append, "@@:@");
    }
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: installed CFArray length/objectAtIndex/string append compatibility hooks\n");
}

/* This Foundation build can return nil from +[NSUserDefaults
 * standardUserDefaults] during the app's first delegate initialization. Keep
 * the app's real preferences API usable by creating the native defaults object
 * once and returning it for subsequent calls. */
static void *g_standard_user_defaults;
static void patch_defaults_publisher_observers(void);
static void *plsr_standard_user_defaults(void *cls, void *cmd) {
    (void)cmd;
    /* Combine registers the concrete generic subscription class lazily. Re-scan
     * when the app asks for defaults so its KVO override is installed before
     * Foundation sends the initial observation callback. */
    patch_defaults_publisher_observers();
    if (g_standard_user_defaults) return g_standard_user_defaults;
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    void *(*msg0)(void *, void *) =
        (void *(*)(void *, void *))plsr_image_symbol("_objc_msgSend");
    if (!sel || !msg0 || !cls) return NULL;
    void *allocated = msg0(cls, sel("alloc"));
    if (!allocated) return NULL;
    g_standard_user_defaults = msg0(allocated, sel("init"));
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: NSUserDefaults singleton initialized %p\n",
                g_standard_user_defaults);
    return g_standard_user_defaults;
}

static void patch_standard_user_defaults(void) {
    void *(*getcls)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_objc_getClass");
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    void *(*getclass)(void *) =
        (void *(*)(void *))plsr_image_symbol("_object_getClass");
    void *(*replace)(void *, void *, void *, const char *) =
        (void *(*)(void *, void *, void *, const char *))
            plsr_image_symbol("_class_replaceMethod");
    if (!getcls || !sel || !getclass || !replace) return;
    void *defaults = getcls("NSUserDefaults");
    void *meta = defaults ? getclass(defaults) : NULL;
    if (!meta) return;
    replace(meta, sel("standardUserDefaults"),
            (void *)plsr_standard_user_defaults, "@@:");
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: patched +[NSUserDefaults standardUserDefaults]\n");
}

static void plsr_defaults_publisher_observe(void *self, void *cmd,
        void *key_path, void *object, void *change, void *context) {
    (void)self; (void)cmd; (void)key_path; (void)object; (void)change; (void)context;
}

static void patch_defaults_publisher_observers(void) {
    int (*getlist)(void **, int) =
        (int (*)(void **, int))plsr_image_symbol("_objc_getClassList");
    const char *(*getname)(void *) =
        (const char *(*)(void *))plsr_image_symbol("_class_getName");
    int (*add)(void *, void *, void *, const char *) =
        (int (*)(void *, void *, void *, const char *))
            plsr_image_symbol("_class_addMethod");
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    if (!getlist || !getname || !add || !sel) return;
    int n = getlist(NULL, 0);
    if (n <= 0 || n > 131072) return;
    void **classes = malloc((size_t)n * sizeof(*classes));
    if (!classes) return;
    int got = getlist(classes, n);
    int patched = 0;
    void *observer = sel("observeValueForKeyPath:ofObject:change:context:");
    for (int i = 0; i < got; i++) {
        const char *name = classes[i] ? getname(classes[i]) : NULL;
        if (!name || !strstr(name, "NSUserDefaults9Publisher") ||
            !strstr(name, "Subscription")) continue;
        if (add(classes[i], observer, (void *)plsr_defaults_publisher_observe,
                "v@:@@@@")) patched++;
    }
    /* The Combine subscription class is lazily materialized during delegate
     * initialization, after the startup class inventory. NSObject is its
     * inherited fallback; classes with a real override keep that override. */
    if (!patched) {
        void *(*replace)(void *, void *, void *, const char *) =
            (void *(*)(void *, void *, void *, const char *))
                plsr_image_symbol("_class_replaceMethod");
        void *nsobject = plsr_class_object("NSObject");
        if (replace && nsobject &&
            replace(nsobject, observer, (void *)plsr_defaults_publisher_observe,
                    "v@:@@@@"))
            patched = -1;   /* NSObject fallback installed */
    }
    free(classes);
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: installed defaults publisher KVO fallback on %d class(es)\n",
                patched);
}

static void *plsr_value_for_undefined_key(void *self, void *cmd, void *key) {
    (void)cmd;
    const char *ks = plsr_nsutf8(key);
    const char *cn = "?";
    void *(*clsfn)(void *) = (void *(*)(void *))plsr_image_symbol("_object_getClass");
    const char *(*cname)(void *) = (const char *(*)(void *))plsr_image_symbol("_class_getName");
    void *c = (clsfn && self) ? clsfn(self) : NULL;
    if (c && cname) cn = cname(c);
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: valueForUndefinedKey: %s on %s %p -> nil\n",
                ks ? ks : "?", cn, self);
    return NULL;
}

static void plsr_set_value_for_undefined_key(void *self, void *cmd, void *value, void *key) {
    (void)cmd; (void)value;
    const char *ks = plsr_nsutf8(key);
    const char *cn = "?";
    void *(*clsfn)(void *) = (void *(*)(void *))plsr_image_symbol("_object_getClass");
    const char *(*cname)(void *) = (const char *(*)(void *))plsr_image_symbol("_class_getName");
    void *c = (clsfn && self) ? clsfn(self) : NULL;
    if (c && cname) cn = cname(c);
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: setValue:forUndefinedKey: %s on %s %p ignored\n",
                ks ? ks : "?", cn, self);
}

static void patch_undefined_key_fallback(void) {
    if (getenv("PLSR_STRICT_KVC")) return;
    void *(*getcls)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_objc_getClass");
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    void *(*replace)(void *, void *, void *, const char *) =
        (void *(*)(void *, void *, void *, const char *))
            plsr_image_symbol("_class_replaceMethod");
    if (!getcls || !sel || !replace) return;
    void *nsobj = getcls("NSObject");
    if (!nsobj) return;
    replace(nsobj, sel("valueForUndefinedKey:"),
            (void *)plsr_value_for_undefined_key, "@@:@");
    replace(nsobj, sel("setValue:forUndefinedKey:"),
            (void *)plsr_set_value_for_undefined_key, "v@:@@");
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: installed undefined-key KVC fallback\n");
}

static void plsr_sparkle_noop(void *self, void *cmd) {
    (void)self; (void)cmd;
}

static void patch_optional_sparkle_updates(void) {
    void *(*getcls)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_objc_getClass");
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    void *(*replace)(void *, void *, void *, const char *) =
        (void *(*)(void *, void *, void *, const char *))
            plsr_image_symbol("_class_replaceMethod");
    if (!getcls || !sel || !replace) return;
    const char *classes[] = { "SUUpdater", "SPUStandardUpdaterController" };
    const char *methods[] = { "startUpdater", "checkForUpdatesInBackground" };
    int patched = 0;
    for (size_t c = 0; c < sizeof(classes) / sizeof(classes[0]); c++) {
        void *cls = getcls(classes[c]);
        if (!cls) continue;
        for (size_t m = 0; m < sizeof(methods) / sizeof(methods[0]); m++)
            if (replace(cls, sel(methods[m]), (void *)plsr_sparkle_noop, "v@:"))
                patched++;
    }
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: disabled optional Sparkle auto-update entry points (%d)\n",
                patched);
}

/* finishEncoding assumes _containers contains a coding container. CotEditor's
 * early AppKit/Sparkle bootstrap reaches it with an empty array in this
 * Foundation build. Install the narrow guard independently of the optional
 * private collection shims. */
static void patch_finish_encoding_empty_containers(void) {
    void *(*getcls)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_objc_getClass");
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    void *(*getimp)(void *, void *) =
        (void *(*)(void *, void *))plsr_image_symbol("_class_getMethodImplementation");
    void *(*repl)(void *, void *, void *, const char *) =
        (void *(*)(void *, void *, void *, const char *))
            plsr_image_symbol("_class_replaceMethod");
    if (!getcls || !sel || !getimp || !repl) return;
    void *ka = getcls("NSKeyedArchiver");
    void *finish = ka ? sel("finishEncoding") : NULL;
    if (!ka || !finish) return;
    g_finish_encoding = (void (*)(void *, void *))getimp(ka, finish);
    if (!g_finish_encoding) return;
    repl(ka, finish, (void *)plsr_finish_encoding, "v@:");
    /* The call is reaching Foundation's body directly in this runtime. Make a
     * trampoline over its verified 13-byte register-save prologue, so the
     * wrapper can seed _containers and then safely continue the real method. */
    for (int i = 0; i < g_nimgs; i++) {
        if (!strstr(g_imgs[i]->path,
                    "Foundation.framework/Versions/C/Foundation")) continue;
        unsigned char *fn = (unsigned char *)(uintptr_t)(g_imgs[i]->slide + 0x60870);
        static const unsigned char prologue[13] = {
            0x55, 0x48,0x89,0xe5, 0x41,0x57, 0x41,0x56,
            0x41,0x55, 0x41,0x54, 0x53
        };
        if (memcmp(fn, prologue, sizeof(prologue)) != 0) break;
        long pg = sysconf(_SC_PAGESIZE);
        if (pg <= 0) pg = 4096;
        unsigned char *tramp = mmap(NULL, (size_t)pg,
                PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (tramp == MAP_FAILED) break;
        memcpy(tramp, prologue, sizeof(prologue));
        tramp[13] = 0x48; tramp[14] = 0xb8;
        uint64_t resume = (uint64_t)(uintptr_t)(fn + sizeof(prologue));
        memcpy(tramp + 15, &resume, 8);
        tramp[23] = 0xff; tramp[24] = 0xe0;
        __builtin___clear_cache((char *)tramp, (char *)tramp + 25);
        uintptr_t page = (uintptr_t)fn & ~(uintptr_t)(pg - 1);
        if (mprotect((void *)page, (size_t)pg,
                     PROT_READ | PROT_WRITE | PROT_EXEC) != 0) break;
        unsigned char jump[13] = {0x48,0xb8,0,0,0,0,0,0,0,0,0xff,0xe0,0x90};
        uint64_t wrapper = (uint64_t)(uintptr_t)plsr_finish_encoding;
        memcpy(jump + 2, &wrapper, 8);
        memcpy(fn, jump, sizeof(jump));
        __builtin___clear_cache((char *)fn, (char *)fn + sizeof(jump));
        mprotect((void *)page, (size_t)pg, PROT_READ | PROT_EXEC);
        g_finish_encoding = (void (*)(void *, void *))tramp;
        break;
    }
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: guarded NSKeyedArchiver finishEncoding imp=%p\n",
                (void *)g_finish_encoding);
}

static void fix_archiver_classes(void) {
    /* This framework build does not safely support adding dynamic method-list
     * overrides to the private toll-free collection classes. Leave its native
     * classes intact by default; opt in only for focused compatibility tests. */
    if (!getenv("PLSR_ENABLE_CF_DICT_SHIM")) return;
    void *(*getcls)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_objc_getClass");
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    void *(*getclass)(void *) =
        (void *(*)(void *))plsr_image_symbol("_object_getClass");
    void *(*repl)(void *, void *, void *, const char *) =
        (void *(*)(void *, void *, void *, const char *))
            plsr_image_symbol("_class_replaceMethod");
    void *(*getimp)(void *, void *) =
        (void *(*)(void *, void *))plsr_image_symbol("_class_getMethodImplementation");
    if (!getcls || !sel || !repl) return;
    void *ma = getcls("NSMutableArray");
    if (ma) {
        repl(ma, sel("initWithObjects:"), (void *)plsr_array_init_objects, "@@:@");
        repl(ma, sel("init"), (void *)plsr_array_init, "@@:");
        repl(ma, sel("initWithCapacity:"), (void *)plsr_array_init_capacity, "@@:q");
        void *meta = getclass ? getclass(ma) : NULL;
        if (meta) {
            repl(meta, sel("alloc"), (void *)plsr_array_alloc, "@@:");
            repl(meta, sel("array"), (void *)plsr_array_class_array, "@@:");
        }
    }
    void *nscfa = getcls("__NSCFArray");
    if (nscfa) {
        void *(*getimp)(void *, void *) =
            (void *(*)(void *, void *))plsr_image_symbol("_class_getMethodImplementation");
        if (getimp)
            g_add_object_imp = (void (*)(void *, void *, void *))getimp(nscfa, sel("addObject:"));
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: addObject imp=%p append=%p\n",
                    (void *)g_add_object_imp,
                    (void *)plsr_image_symbol("_CFArrayAppendValue"));
        repl(nscfa, sel("init"), (void *)plsr_identity_init, "@@:");
        repl(nscfa, sel("initWithObjects:"), (void *)plsr_array_init_objects, "@@:@");
        repl(nscfa, sel("initWithCapacity:"), (void *)plsr_array_init_capacity, "@@:q");
        repl(nscfa, sel("addObject:"), (void *)plsr_array_add_object, "v@:@");
    }
    void *md = getcls("NSMutableDictionary");
    if (md) {
        repl(md, sel("init"), (void *)plsr_dict_init, "@@:");
        repl(md, sel("initWithCapacity:"), (void *)plsr_dict_init_capacity, "@@:q");
        void *meta = getclass ? getclass(md) : NULL;
        if (meta) {
            repl(meta, sel("alloc"), (void *)plsr_dict_alloc, "@@:");
            repl(meta, sel("dictionary"), (void *)plsr_dict_class_dictionary, "@@:");
        }
    }
    void *nscfd = getcls("__NSCFDictionary");
    if (nscfd && getenv("PLSR_ENABLE_CF_DICT_SHIM")) {
        g_nscfd_count_selector = sel("count");
        g_nscfd_count_original = getimp
            ? (unsigned long (*)(void *, void *))getimp(nscfd, g_nscfd_count_selector)
            : NULL;
        repl(nscfd, sel("init"), (void *)plsr_identity_init, "@@:");
        repl(nscfd, sel("initWithCapacity:"), (void *)plsr_dict_init_capacity, "@@:q");
        repl(nscfd, sel("initWithObjects:forKeys:count:"),
             (void *)plsr_dict_init_objects_for_keys_count, "@@:^@^@Q");
        repl(nscfd, sel("initWithObjects:forKeys:"),
             (void *)plsr_dict_init_objects_for_keys, "@@:^@^@");
        /* Keep __NSCFDictionary's native -count implementation. The CF
         * bridge used by plsr_dict_count can re-enter Objective-C dispatch;
         * replacing -count here creates a recursive CF -> ObjC -> CF cycle. */
        repl(nscfd, sel("objectForKey:"), (void *)plsr_dict_object_for_key, "@@:@");
        repl(nscfd, sel("getObjects:andKeys:count:"),
             (void *)plsr_dict_get_objects_keys, "v@:^@^@Q");
        repl(nscfd, sel("setObject:forKey:"),
             (void *)plsr_dict_set_object, "v@:@@");
        repl(nscfd, sel("setObject:forKeyedSubscript:"),
             (void *)plsr_dict_set_object, "v@:@@");
    }
    /* Foundation's dictionary classes implement allKeys by allocating a
     * buffer and asking getObjects:andKeys:count: to fill it.  When that
     * method is absent the buffer stays uninitialised, and __CFArrayCreate0
     * then retains heap garbage at libobjc+0xb79 (via _objc_retain).  Back the
     * read side with CF for the concrete dictionary classes too. */
    const char *readonly_dicts[] = { "__NSDictionaryM", "__NSDictionaryI",
                                     "__NSDictionary0" };
    for (size_t di = 0; di < sizeof readonly_dicts / sizeof readonly_dicts[0]; di++) {
        void *dc = getcls(readonly_dicts[di]);
        if (!dc || !getenv("PLSR_ENABLE_CF_DICT_SHIM")) continue;
        repl(dc, sel("count"), (void *)plsr_dict_count, "Q@:");
        repl(dc, sel("objectForKey:"), (void *)plsr_dict_object_for_key, "@@:@");
        repl(dc, sel("getObjects:andKeys:count:"),
             (void *)plsr_dict_get_objects_keys, "v@:^@^@Q");
    }
    void *ms = getcls("NSMutableSet");
    if (ms) {
        repl(ms, sel("init"), (void *)plsr_set_init, "@@:");
        repl(ms, sel("initWithArray:"), (void *)plsr_set_init_array, "@@:@");
        void *meta = getclass ? getclass(ms) : NULL;
        if (meta) {
            repl(meta, sel("alloc"), (void *)plsr_set_alloc, "@@:");
            repl(meta, sel("set"), (void *)plsr_set_class_set, "@@:");
            repl(meta, sel("setWithArray:"), (void *)plsr_set_class_set_with_array, "@@:@");
        }
    }
    void *nscfs = getcls("__NSCFSet");
    if (nscfs) {
        repl(nscfs, sel("init"), (void *)plsr_identity_init, "@@:");
        repl(nscfs, sel("initWithArray:"), (void *)plsr_set_init_array, "@@:@");
    }
    /* The archiver's UID object is a __NSKeyedCodingContainer; its alloc/init
     * are missing too, so alloc_init returns nil and _uidForObject: appends nil
     * to _containers.  It only needs to exist and carry a +8 value, which the
     * archiver writes itself. */
    void *cc = getcls("__NSKeyedCodingContainer");
    if (cc) {
        void *ccmeta = getclass ? getclass(cc) : NULL;
        if (ccmeta) repl(ccmeta, sel("alloc"), (void *)plsr_cc_alloc, "@@:");
        repl(cc, sel("init"), (void *)plsr_identity_init, "@@:");
        /* _objc_autorelease falls back to -autorelease for this class; the
         * forward handler would return nil, which then gets appended to the
         * archiver's _containers array. */
        repl(cc, sel("autorelease"), (void *)plsr_identity_init, "@@:");
        repl(cc, sel("retain"), (void *)plsr_identity_init, "@@:");
        repl(cc, sel("release"), (void *)plsr_noop_release, "v@:");
    }
    void *exc = getcls("NSException");
    if (exc) {
        void *excmeta = getclass ? getclass(exc) : NULL;
        void *(*getimp)(void *, void *) =
            (void *(*)(void *, void *))plsr_image_symbol("_class_getMethodImplementation");
        if (excmeta && getimp) {
            g_exc_raise_format = (void (*)(void *, void *, void *, void *, ...))
                getimp(excmeta, sel("raise:format:"));
            repl(excmeta, sel("raise:format:"),
                 (void *)plsr_exc_raise_format, "v@:@@");
            repl(excmeta, sel("raise:format:arguments:"),
                 (void *)plsr_exc_raise_format_args, "v@:@@@");
        }
    }
    void *ka = getcls("NSKeyedArchiver");
    if (ka) {
        void *fs = sel("finishEncoding");
        void *(*getimp)(void *, void *) =
            (void *(*)(void *, void *))plsr_image_symbol("_class_getMethodImplementation");
        g_finish_encoding = getimp
            ? (void (*)(void *, void *))getimp(ka, fs) : NULL;
        repl(ka, fs, (void *)plsr_finish_encoding, "v@:");
    }
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: CF-backed mutable collections installed\n");
}

static void patch_cf_key_guard(void);
static void patch_cf_collection_nil(void);

static void patch_archiver_current_values(void) {
    if (getenv("PLSR_NO_ARCHIVER_PATCH")) return;
    /* Always on for the delegate bootstrap: that path creates an archiver with
     * an empty _containers and dies in _currentValues without this patch. */
    if (!getenv("PLSR_PATCH_ARCHIVER") && !getenv("PLSR_BOOTSTRAP_DELEGATE") &&
        !getenv("PLSR_NSWINDOW")) return;
    patch_cf_array_add_nil();
    patch_finish_encoding_empty_containers();
    patch_standard_user_defaults();
    patch_defaults_publisher_observers();
    patch_optional_sparkle_updates();
    fix_archiver_classes();
    patch_cf_key_guard();
    patch_cf_collection_nil();
    for (int i = 0; i < g_nimgs; i++) {
        if (!strstr(g_imgs[i]->path,
                    "Foundation.framework/Versions/C/Foundation"))
            continue;
        long pg = sysconf(_SC_PAGESIZE);
        if (pg <= 0) pg = 4096;
        void *tramp = mmap(NULL, (size_t)pg,
                           PROT_READ | PROT_WRITE | PROT_EXEC,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (tramp != MAP_FAILED) {
            unsigned char *t = tramp;
            t[0] = 0x48; t[1] = 0xB8;                 /* movabs rax, helper */
            uint64_t h = (uint64_t)(uintptr_t)plsr_current_values_helper;
            memcpy(t + 2, &h, 8);
            t[10] = 0xFF; t[11] = 0xE0;               /* jmp rax */
            __builtin___clear_cache((char *)t, (char *)t + 12);
            unsigned char *fn =
                (unsigned char *)(uintptr_t)(g_imgs[i]->slide + 0x5f780);
            uintptr_t page = (uintptr_t)fn & ~(uintptr_t)(pg - 1);
            if (mprotect((void *)page, (size_t)pg,
                         PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
                fn[0] = 0xE9;                         /* jmp trampoline */
                int32_t rel = (int32_t)((intptr_t)tramp - (intptr_t)(fn + 5));
                memcpy(fn + 1, &rel, 4);
                __builtin___clear_cache((char *)fn, (char *)fn + 5);
                mprotect((void *)page, (size_t)pg, PROT_READ | PROT_EXEC);
            }
        }
        /* -[NSKeyedArchiver _uidForObject:]: je skip -> jmp skip. */
        unsigned char *br =
            (unsigned char *)(uintptr_t)(g_imgs[i]->slide + 0x5fc2c);
        uintptr_t bpage = (uintptr_t)br & ~(uintptr_t)(pg - 1);
        if (mprotect((void *)bpage, (size_t)pg,
                     PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
            unsigned char code[2] = {0xeb, 0x60};
            memcpy(br, code, 2);
            __builtin___clear_cache((char *)br, (char *)br + 2);
            mprotect((void *)bpage, (size_t)pg, PROT_READ | PROT_EXEC);
        }
        void *(*get_class)(const char *) =
            (void *(*)(const char *))plsr_image_symbol("_objc_getClass");
        void *(*add_method)(void *, void *, void *, const char *) =
            (void *(*)(void *, void *, void *, const char *))
                plsr_image_symbol("_class_addMethod");
        void *(*sel)(const char *) =
            (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
        if (get_class && add_method && sel) {
            void *cf = get_class("__NSCFType");
            if (cf)
                add_method(cf, sel("encodeWithCoder:"),
                           (void *)plsr_noop_encode, "v@:@");
        }
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: patched archiver at %p\n",
                    (void *)(uintptr_t)(g_imgs[i]->slide + 0x5f780));
        return;
    }
}

/* Diagnostic: show which NSKeyedArchiver initializer is installed and whether
 * a freshly created archiver has its _containers/_objects ivars populated. */
static void probe_keyed_archiver(void) {
    void *(*getcls)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_objc_getClass");
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    void *(*msg0)(void *, void *) =
        (void *(*)(void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*msg1)(void *, void *, void *) =
        (void *(*)(void *, void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*copy_methods)(void *, unsigned int *) =
        (void *(*)(void *, unsigned int *))
            plsr_image_symbol("_class_copyMethodList");
    void *(*method_name)(void *) =
        (void *(*)(void *))plsr_image_symbol("_method_getName");
    void *(*method_imp)(void *) =
        (void *(*)(void *))plsr_image_symbol("_method_getImplementation");
    const char *(*sel_name)(void *) =
        (const char *(*)(void *))plsr_image_symbol("_sel_getName");
    void *(*get_ivar)(void *, const char *) =
        (void *(*)(void *, const char *))plsr_image_symbol("_class_getInstanceVariable");
    long (*ivar_off)(void *) =
        (long (*)(void *))plsr_image_symbol("_ivar_getOffset");
    if (!getcls || !sel || !msg0 || !msg1) return;
    void *cls = getcls("NSKeyedArchiver");
    uintptr_t base = 0;
    for (int i = 0; i < g_nimgs; i++)
        if (strstr(g_imgs[i]->path, "Foundation.framework/Versions/C/Foundation"))
            base = (uintptr_t)(g_imgs[i]->base + g_imgs[i]->slide);
    fprintf(stderr, "reef: probe NSKeyedArchiver=%p base=%p\n", cls, (void *)base);
    if (base && getcls) {
        void *c65 = *(void **)(uintptr_t)(base + 0x175458);
        void *c70 = *(void **)(uintptr_t)(base + 0x175460);
        void *c457 = *(void **)(uintptr_t)(base + 0x1754b0);
        void *s69 = *(void **)(uintptr_t)(base + 0x16d398);
        const char *(*cname)(void *) =
            (const char *(*)(void *))plsr_image_symbol("_class_getName");
        const char *(*sname)(void *) =
            (const char *(*)(void *))plsr_image_symbol("_sel_getName");
        fprintf(stderr, "reef: probe refs c65=%p(%s) c70=%p(%s) c457=%p(%s) s69=%p(%s)\n",
                c65, c65 && cname ? cname(c65) : "nil",
                c70, c70 && cname ? cname(c70) : "nil",
                c457, c457 && cname ? cname(c457) : "nil",
                s69, s69 && sname ? sname(s69) : "nil");
    }
    if (copy_methods && method_name && method_imp && sel_name) {
        unsigned int n = 0;
        void **ml = (void **)copy_methods(cls, &n);
        for (unsigned int i = 0; ml && i < n; i++) {
            void *s = method_name(ml[i]);
            void *imp = method_imp(ml[i]);
            fprintf(stderr, "reef:   -[NSKeyedArchiver %s] +0x%lx\n",
                    sel_name(s), (unsigned long)((uintptr_t)imp - base));
        }
    }
    void *arch = msg1(msg0(cls, sel("alloc")),
                      sel("initRequiringSecureCoding:"), (void *)1);
    {
        void *ma = getcls("NSMutableArray");
        void *alloced = ma ? msg0(ma, sel("alloc")) : NULL;
        void *(*ogc)(void *) =
            (void *(*)(void *))plsr_image_symbol("_object_getClass");
        const char *(*cname2)(void *) =
            (const char *(*)(void *))plsr_image_symbol("_class_getName");
        fprintf(stderr, "reef: probe alloc=%p class=%s\n", alloced,
                alloced && ogc && cname2 ? cname2(ogc(alloced)) : "?");
        void *a0 = ma ? msg0(msg0(ma, sel("alloc")), sel("init")) : NULL;
        void *a1 = ma ? msg0(ma, sel("array")) : NULL;
        void *a2 = ma ? msg1(msg0(ma, sel("alloc")), sel("initWithCapacity:"), (void *)0) : NULL;
        void *a3 = ma ? msg1(msg0(ma, sel("alloc")), sel("initWithCapacity:"), (void *)4) : NULL;
        void *am = getcls("__NSArrayM");
        void *ai = getcls("__NSArrayI");
        void *amv = am ? msg0(msg0(am, sel("alloc")), sel("init")) : NULL;
        void *nscfa = getcls("__NSCFArray");
        void *nscfd = getcls("__NSCFDictionary");
        void *nscfs = getcls("__NSCFSet");
        void *nullobj = getcls("NSNull") ?
            msg0(getcls("NSNull"), sel("null")) : NULL;
        void *(*cfmkarr)(void *, long, void *) =
            (void *(*)(void *, long, void *))plsr_image_symbol("_CFArrayCreateMutable");
        void *cfa = cfmkarr ? cfmkarr(NULL, 0, NULL) : NULL;
        fprintf(stderr, "reef: probe arrays init=%p array=%p cap0=%p cap4=%p __NSArrayM=%p init=%p __NSArrayI=%p\n",
                a0, a1, a2, a3, am, amv, ai);
        fprintf(stderr, "reef: probe CF __NSCFArray=%p __NSCFDictionary=%p __NSCFSet=%p mkarr=%p arr=%p\n",
                nscfa, nscfd, nscfs, (void *)cfmkarr, cfa);
        if (cfa) {
            msg1(cfa, sel("addObject:"), nullobj);
            void *cnt = msg0(cfa, sel("count"));
            void *o0 = msg1(cfa, sel("objectAtIndex:"), (void *)0);
            fprintf(stderr, "reef: probe CFArray count=%p obj0=%p\n", cnt, o0);
        }
    }
    {
        void *(*alloc_init)(void *) =
            (void *(*)(void *))plsr_image_symbol("_objc_alloc_init");
        void *cc = getcls("__NSKeyedCodingContainer");
        void *ccv = (cc && alloc_init) ? alloc_init(cc) : NULL;
        void *(*autorel)(void *) =
            (void *(*)(void *))plsr_image_symbol("_objc_autorelease");
        void *(*retain)(void *) =
            (void *(*)(void *))plsr_image_symbol("_objc_retain");
        void *ccv2 = (ccv && autorel) ? autorel(ccv) : NULL;
        void *ccv3 = (ccv && retain) ? retain(ccv) : NULL;
        fprintf(stderr, "reef: probe cc=%p alloc_init=%p autorelease=%p(%p) retain=%p(%p)\n",
                cc, ccv, (void *)autorel, ccv2, (void *)retain, ccv3);
    }
    fprintf(stderr, "reef: probe fresh archiver=%p\n", arch);
    if (arch && get_ivar && ivar_off) {
        void *ci = get_ivar(cls, "_containers");
        void *oi = get_ivar(cls, "_objects");
        long co = ci ? ivar_off(ci) : -1;
        long oo = oi ? ivar_off(oi) : -1;
        fprintf(stderr, "reef: probe containers off=%ld val=%p; objects off=%ld val=%p\n",
                co, co >= 0 ? *(void **)((char *)arch + co) : NULL,
                oo, oo >= 0 ? *(void **)((char *)arch + oo) : NULL);
    }
}

static void patch_exception_callstack(void) {
    if (getenv("PLSR_NO_EXC_PATCH")) return;
    if (!getenv("PLSR_PATCH_EXCEPTIONS")) return;
    for (int i = 0; i < g_nimgs; i++) {
        if (!strstr(g_imgs[i]->path,
                    "CoreFoundation.framework/Versions/A/CoreFoundation"))
            continue;
        long pg = sysconf(_SC_PAGESIZE);
        if (pg <= 0) pg = 4096;
        uintptr_t offs[2] = {0xe1dd0, 0xe2110};
        for (int k = 0; k < 2; k++) {
            unsigned char *fn =
                (unsigned char *)(uintptr_t)(g_imgs[i]->slide + offs[k]);
            uintptr_t page = (uintptr_t)fn & ~(uintptr_t)(pg - 1);
            if (mprotect((void *)page, (size_t)pg,
                         PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
                continue;
            fn[0] = 0xc3;  /* ret */
            __builtin___clear_cache((char *)fn, (char *)fn + 1);
            mprotect((void *)page, (size_t)pg, PROT_READ | PROT_EXEC);
            if (getenv("PLSR_REEF_TRACE"))
                fprintf(stderr, "reef: stubbed _recordCallStack at %p\n",
                        (void *)fn);
        }
        return;
    }
}

/* libdispatch starts its worker pool from an image initializer; if a worker
 * touches a class whose cache is still being realized, the cache lookup walks
 * a null bucket array (libSystem.B+0x4ece9).  Force realization on the main
 * thread before any initializer runs. */
static void prerealize_classes(void) {
    if (!getenv("PLSR_PREREALIZE")) return;
    int (*getlist)(void **, int) =
        (int (*)(void **, int))plsr_image_symbol("_objc_getClassList");
    const char *(*getname)(void *) =
        (const char *(*)(void *))plsr_image_symbol("_class_getName");
    if (!getlist || !getname) return;
    int n = getlist(NULL, 0);
    if (n <= 0) return;
    void **list = calloc((size_t)n, sizeof(void *));
    if (!list) return;
    n = getlist(list, n);
    for (int i = 0; i < n; i++)
        if (list[i]) getname(list[i]);
    free(list);
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: pre-realized %d classes\n", n);
}

static void print_frame_map(void *addr);

/* Print the name/reason of an uncaught NSException so launch failures are not
 * just "terminating due to uncaught exception of type NSException". */
static void plsr_uncaught_exception(void *exc) {
    void *(*msg0)(void *, void *) =
        (void *(*)(void *, void *))plsr_image_symbol("_objc_msgSend");
    void *(*sel)(const char *) =
        (void *(*)(const char *))plsr_image_symbol("_sel_registerName");
    if (!msg0 || !sel || !exc) return;
    void *name = msg0(exc, sel("name"));
    void *reason = msg0(exc, sel("reason"));
    const char *ns = name ? (const char *)msg0(name, sel("UTF8String")) : NULL;
    const char *rs = reason ? (const char *)msg0(reason, sel("UTF8String")) : NULL;
    fprintf(stderr, "reef: uncaught exception %s: %s\n", ns ? ns : "?",
            rs ? rs : "?");
    void *ui = msg0(exc, sel("userInfo"));
    if (ui) {
        void *(*msg1)(void *, void *, void *) =
            (void *(*)(void *, void *, void *))plsr_image_symbol("_objc_msgSend");
        void *k = msg1((void *)plsr_image_symbol("_objc_getClass")
                           ? ((void *(*)(void *, void *, void *))msg1)(((void *(*)(const char *))plsr_image_symbol("_objc_getClass"))("NSString"), sel("stringWithUTF8String:"), (void *)"NSUnknownUserInfoKey")
                           : NULL, sel("self"), NULL);
        (void)k;
        /* Dump userInfo description when available. */
        void *desc = msg0(ui, sel("description"));
        const char *ds = desc ? (const char *)msg0(desc, sel("UTF8String")) : NULL;
        if (ds) fprintf(stderr, "reef: exception userInfo=%s\n", ds);
    }
    void *frames[32];
    int n = backtrace(frames, 32);
    for (int i = 0; i < n; i++) {
        fprintf(stderr, "  exc[%02d]=%p\n", i, frames[i]);
        print_frame_map(frames[i]);
    }
}
static void install_uncaught_handler(void) {
    if (!getenv("PLSR_EXC_TRACE")) return;
    void (*set)(void (*)(void *)) =
        (void (*)(void (*)(void *)))plsr_image_symbol("_NSSetUncaughtExceptionHandler");
    if (set) {
        set(plsr_uncaught_exception);
        fprintf(stderr, "reef: installed uncaught handler\n");
    }
}

static int shim_NSGetExecutablePath(char *buf, uint32_t *bufsize) {
    if (!g_exec_path[0]) {
        if (g_main && g_main->path && realpath(g_main->path, g_exec_path)) {
            /* realpath filled it in */
        } else {
            ssize_t n = readlink("/proc/self/exe", g_exec_path, sizeof g_exec_path - 1);
            g_exec_path[n > 0 ? n : 0] = 0;
        }
    }
    size_t need = strlen(g_exec_path) + 1;
    if (getenv("PLSR_EXEC_PATH_TRACE"))
        fprintf(stderr, "shim NSGetExecutablePath -> '%s' need=%zu cap=%u\n",
                g_exec_path, need, bufsize ? *bufsize : 0);
    if (!buf || !bufsize || *bufsize < need) {
        if (bufsize) *bufsize = (uint32_t)need;
        return -1;
    }
    memcpy(buf, g_exec_path, need);
    return 0;
}

static void *shim_lookup(const char *name);
static void plsr_preload_compat(void);

/* ---- ICU: libicucore.A.dylib ships only stubs, so the host side has to
 * provide the C locale surface CoreFoundation actually calls. ---- */
static char g_icu_locale[128] = "en_US";
static int icu_ok(int32_t *err) { return err ? *err <= 0 : 1; }
static int32_t icu_put(const char *s, char *dst, int32_t cap) {
    int32_t n = s ? (int32_t)strlen(s) : 0;
    if (dst && cap > 0) {
        int32_t k = n < cap - 1 ? n : cap - 1;
        if (k > 0 && s) memcpy(dst, s, (size_t)k);
        dst[k] = 0;
    }
    return n;
}
static const char *icu_part(const char *id, int idx, char *buf, size_t cap) {
    buf[0] = 0;
    if (!id) return buf;
    const char *p = id;
    int part = 0;
    while (*p && part <= idx) {
        const char *s = p;
        while (*p && *p != '_' && *p != '-' && *p != '@') p++;
        size_t len = (size_t)(p - s);
        if (part == idx) {
            if (len >= cap) len = cap - 1;
            memcpy(buf, s, len); buf[len] = 0;
            return buf;
        }
        part++;
        if (*p == '@' || !*p) break;
        p++;
    }
    return buf;
}
static int32_t shim_uloc_getLanguage(const char *id, char *out, int32_t cap, int32_t *err) {
    char b[128];
    if (!icu_ok(err)) return 0;
    return icu_put(icu_part(id, 0, b, sizeof b), out, cap);
}
static int32_t shim_uloc_getScript(const char *id, char *out, int32_t cap, int32_t *err) {
    char b[128];
    if (!icu_ok(err)) return 0;
    icu_part(id, 1, b, sizeof b);
    if (strlen(b) != 4) b[0] = 0;
    return icu_put(b, out, cap);
}
static int32_t shim_uloc_getCountry(const char *id, char *out, int32_t cap, int32_t *err) {
    char b[128];
    if (!icu_ok(err)) return 0;
    icu_part(id, 1, b, sizeof b);
    if (strlen(b) == 4) icu_part(id, 2, b, sizeof b);
    if (!(strlen(b) == 2 || (strlen(b) == 3 && isdigit((unsigned char)b[0])))) b[0] = 0;
    return icu_put(b, out, cap);
}
static int32_t shim_uloc_getVariant(const char *id, char *out, int32_t cap, int32_t *err) {
    char b[128];
    if (!icu_ok(err)) return 0;
    icu_part(id, 1, b, sizeof b);
    int idx = strlen(b) == 4 ? 3 : 2;
    return icu_put(icu_part(id, idx, b, sizeof b), out, cap);
}
static int32_t shim_uloc_getBaseName(const char *id, char *out, int32_t cap, int32_t *err) {
    if (!icu_ok(err)) return 0;
    return icu_put(id ? id : "", out, cap);
}
static int32_t shim_uloc_canonicalize(const char *id, char *out, int32_t cap, int32_t *err) {
    if (!icu_ok(err)) return 0;
    return icu_put(id ? id : "", out, cap);
}
static int32_t shim_uloc_addLikelySubtags(const char *id, char *out, int32_t cap, int32_t *err) {
    if (!icu_ok(err)) return 0;
    return icu_put(id ? id : "", out, cap);
}
static int32_t shim_uloc_minimizeSubtags(const char *id, char *out, int32_t cap, int32_t *err) {
    if (!icu_ok(err)) return 0;
    return icu_put(id ? id : "", out, cap);
}
static int32_t shim_uloc_getParent(const char *id, char *out, int32_t cap, int32_t *err) {
    char b[128];
    if (!icu_ok(err)) return 0;
    snprintf(b, sizeof b, "%s", id ? id : "");
    char *q = strrchr(b, '_');
    if (q) *q = 0; else b[0] = 0;
    return icu_put(b, out, cap);
}
static const char *shim_uloc_getDefault(void) { return g_icu_locale; }
static void shim_uloc_setDefault(const char *id, int32_t *err) {
    if (!icu_ok(err)) return;
    snprintf(g_icu_locale, sizeof g_icu_locale, "%s", id && *id ? id : "en_US");
}
static const char *const g_icu_available[] = { "en_US", "en", "de_DE", NULL };
static const char *const *shim_uloc_getAvailable(int32_t *err) { (void)err; return g_icu_available; }
static int32_t shim_uloc_countAvailable(void) { return 1; }
static const char *const *shim_uloc_getISOLanguages(int32_t *err) { (void)err; return g_icu_available; }
static const char *const *shim_uloc_getISOCountries(int32_t *err) { (void)err; return g_icu_available; }
static uint32_t shim_uloc_getLCID(const char *id) {
    char b[128]; icu_part(id, 0, b, sizeof b);
    if (!strcmp(b, "en")) return 0x0409;
    if (!strcmp(b, "de")) return 0x0407;
    if (!strcmp(b, "fr")) return 0x040c;
    if (!strcmp(b, "ja")) return 0x0411;
    if (!strcmp(b, "zh")) return 0x0804;
    if (!strcmp(b, "es")) return 0x040a;
    if (!strcmp(b, "it")) return 0x0410;
    return 0x0409;
}
static int32_t shim_uloc_getLocaleForLCID(uint32_t lcid, char *out, int32_t cap, int32_t *err) {
    const char *v = "en_US";
    if (lcid == 0x0407) v = "de_DE";
    else if (lcid == 0x040c) v = "fr_FR";
    else if (lcid == 0x0411) v = "ja_JP";
    else if (lcid == 0x0804 || lcid == 0x0404) v = "zh_CN";
    else if (lcid == 0x040a) v = "es_ES";
    else if (lcid == 0x0410) v = "it_IT";
    if (!icu_ok(err)) return 0;
    return icu_put(v, out, cap);
}
static int32_t shim_uloc_getCharacterOrientation(const char *id, int32_t *err) {
    (void)id; (void)err; return 0;
}
static int32_t shim_uloc_getLineOrientation(const char *id, int32_t *err) {
    (void)id; (void)err; return 0;
}
static int32_t shim_uloc_getKeywordValue(const char *id, const char *key, char *out, int32_t cap, int32_t *err) {
    (void)id; (void)key;
    if (!icu_ok(err)) return 0;
    return icu_put("", out, cap);
}
static int32_t shim_uloc_setKeywordValue(const char *key, const char *val, char *buf, int32_t cap, int32_t *err) {
    (void)key; (void)val;
    if (!icu_ok(err)) return 0;
    return icu_put(buf ? buf : "", buf, cap);
}
static void *shim_uloc_openKeywords(const char *id, int32_t *err) { (void)id; (void)err; return NULL; }
static int32_t shim_uloc_getDisplayName(const char *id, const char *disp, char *out, int32_t cap, int32_t *err) {
    (void)disp;
    if (!icu_ok(err)) return 0;
    return icu_put(id ? id : "", out, cap);
}
static int32_t shim_uloc_getDisplayKeyword(const char *key, const char *disp, char *out, int32_t cap, int32_t *err) {
    (void)disp;
    if (!icu_ok(err)) return 0;
    return icu_put(key ? key : "", out, cap);
}
static int32_t shim_uloc_getDisplayKeywordValue(const char *id, const char *key, const char *disp, char *out, int32_t cap, int32_t *err) {
    (void)id; (void)disp;
    if (!icu_ok(err)) return 0;
    return icu_put(key ? key : "", out, cap);
}
/* UTF-16 helpers used by CF's string bridge. */
static int32_t shim_u_strlen(const uint16_t *s) { int32_t n = 0; while (s && s[n]) n++; return n; }
static const uint16_t *shim_u_strstr(const uint16_t *h, const uint16_t *n) {
    if (!h || !n || !*n) return h;
    for (; *h; h++) {
        const uint16_t *a = h, *b = n;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return h;
    }
    return NULL;
}
static void shim_u_charsToUChars(const char *s, uint16_t *u, int32_t len) {
    for (int32_t i = 0; i < len; i++) u[i] = (uint16_t)(unsigned char)s[i];
}
static void shim_u_UCharsToChars(const uint16_t *u, char *s, int32_t len) {
    for (int32_t i = 0; i < len; i++) s[i] = (char)(u[i] & 0x7f);
}
static int32_t u8_to_u16(const unsigned char *s, int32_t n, uint16_t *d, int32_t cap) {
    int32_t out = 0;
    for (int32_t i = 0; i < n; ) {
        unsigned c = s[i++];
        uint32_t cp = c;
        if (c >= 0xF0 && i + 2 < n) { cp = (uint32_t)(c & 7) << 18 | (uint32_t)(s[i] & 0x3f) << 12 | (uint32_t)(s[i+1] & 0x3f) << 6 | (s[i+2] & 0x3f); i += 3; }
        else if (c >= 0xE0 && i + 1 < n) { cp = (uint32_t)(c & 0xf) << 12 | (uint32_t)(s[i] & 0x3f) << 6 | (s[i+1] & 0x3f); i += 2; }
        else if (c >= 0xC0 && i < n) { cp = (uint32_t)(c & 0x1f) << 6 | (s[i] & 0x3f); i += 1; }
        if (cp > 0xFFFF) {
            cp -= 0x10000;
            if (d && out + 1 < cap) { d[out] = (uint16_t)(0xD800 | (cp >> 10)); d[out+1] = (uint16_t)(0xDC00 | (cp & 0x3ff)); }
            out += 2;
        } else {
            if (d && out < cap) d[out] = (uint16_t)cp;
            out++;
        }
    }
    return out;
}
static void shim_u_strFromUTF8(uint16_t *dst, int32_t cap, int32_t *len, const char *src, int32_t slen, int32_t *err) {
    if (!icu_ok(err)) return;
    if (slen < 0) slen = src ? (int32_t)strlen(src) : 0;
    int32_t n = u8_to_u16((const unsigned char *)src, slen, dst, cap);
    if (len) *len = n;
}


/* ---- Darwin libmalloc must not coexist with the host allocator ----
 * `_malloc` binds to glibc, but `_malloc_size`/`_malloc_good_size` were still
 * resolving into libSystem.B.dylib's real libmalloc.  A glibc block has no
 * Darwin zone header, so libmalloc's malloc_size reads unrelated heap words
 * and reports a bogus size; CoreFoundation then copies or truncates by that
 * size and the heap is corrupted (the crash surfaces later as a garbage chunk
 * size inside free()).  Bind the whole family to the host allocator. */
static char g_dummy_zone;
static void *shim_malloc_default_zone(void) { return &g_dummy_zone; }
static size_t shim_malloc_good_size(size_t n) { return (n + 15u) & ~(size_t)15u; }
static void *shim_malloc_zone_malloc(void *z, size_t n) { (void)z; return malloc(n); }
static void *shim_malloc_zone_calloc(void *z, size_t n, size_t s) {
    (void)z; return calloc(n, s);
}
static void *shim_malloc_zone_realloc(void *z, void *p, size_t n) {
    (void)z; return realloc(p, n);
}
static void shim_malloc_zone_free(void *z, void *p) { (void)z; free(p); }
static void *shim_malloc_zone_memalign(void *z, size_t a, size_t n) {
    (void)z;
    void *p = NULL;
    return posix_memalign(&p, a, n) == 0 ? p : NULL;
}
static void *shim_malloc_zone_valloc(void *z, size_t n) { (void)z; return valloc(n); }
static void *shim_malloc_zone_from_ptr(void *p) { return p ? &g_dummy_zone : NULL; }
static void *shim_reallocf(void *p, size_t n) { return realloc(p, n); }


static const struct shim { const char *name; void *addr; } shims[] = {
    { "_write", (void *)shim_write }, { "_read", (void *)shim_read },
    { "_close", (void *)shim_close }, { "_exit", (void *)shim_exit },
    { "_abort", (void *)shim_abort }, { "_bzero", (void *)shim_bzero },
    { "_NSGetExecutablePath", (void *)shim_NSGetExecutablePath },
    { "__NSGetExecutablePath", (void *)shim_NSGetExecutablePath },
    { "_stat", (void *)shim_stat }, { "_stat$INODE64", (void *)shim_stat },
    { "_stat64", (void *)shim_stat },
    { "_lstat", (void *)shim_lstat }, { "_lstat$INODE64", (void *)shim_lstat },
    { "_lstat64", (void *)shim_lstat },
    { "_fstat", (void *)shim_fstat }, { "_fstat$INODE64", (void *)shim_fstat },
    { "_fstat64", (void *)shim_fstat },
    { "_malloc", (void *)plsr_malloc_tracked },
    { "_calloc", (void *)plsr_calloc_tracked },
    { "_realloc", (void *)plsr_realloc_tracked },
    { "_free", (void *)plsr_free_checked },
    { "_malloc_size", (void *)malloc_usable_size },
    { "_malloc_usable_size", (void *)malloc_usable_size },
    { "_malloc_good_size", (void *)shim_malloc_good_size },
    { "_valloc", (void *)valloc }, { "_reallocf", (void *)shim_reallocf },
    { "_posix_memalign", (void *)posix_memalign },
    { "_aligned_alloc", (void *)aligned_alloc },
    { "_memalign", (void *)memalign },
    { "_malloc_default_zone", (void *)shim_malloc_default_zone },
    { "_malloc_zone_malloc", (void *)shim_malloc_zone_malloc },
    { "_malloc_zone_calloc", (void *)shim_malloc_zone_calloc },
    { "_malloc_zone_realloc", (void *)shim_malloc_zone_realloc },
    { "_malloc_zone_free", (void *)shim_malloc_zone_free },
    { "_malloc_zone_memalign", (void *)shim_malloc_zone_memalign },
    { "_malloc_zone_valloc", (void *)shim_malloc_zone_valloc },
    { "_malloc_zone_from_ptr", (void *)shim_malloc_zone_from_ptr },
    { "_memcpy", (void *)memcpy }, { "_memmove", (void *)memmove },
    { "_memset", (void *)memset }, { "_memcmp", (void *)memcmp },
    { "_strlen", (void *)strlen }, { "_strcmp", (void *)strcmp },
    { "_strncmp", (void *)strncmp }, { "_strcpy", (void *)strcpy },
    { "_strncpy", (void *)strncpy }, { "_puts", (void *)puts },
    { "_putchar", (void *)putchar }, { "_printf", (void *)printf },
    { "_uloc_getLanguage", (void *)shim_uloc_getLanguage },
    { "_uloc_getScript", (void *)shim_uloc_getScript },
    { "_uloc_getCountry", (void *)shim_uloc_getCountry },
    { "_uloc_getVariant", (void *)shim_uloc_getVariant },
    { "_uloc_getBaseName", (void *)shim_uloc_getBaseName },
    { "_uloc_canonicalize", (void *)shim_uloc_canonicalize },
    { "_uloc_addLikelySubtags", (void *)shim_uloc_addLikelySubtags },
    { "_uloc_minimizeSubtags", (void *)shim_uloc_minimizeSubtags },
    { "_uloc_getParent", (void *)shim_uloc_getParent },
    { "_uloc_getDefault", (void *)shim_uloc_getDefault },
    { "_uloc_setDefault", (void *)shim_uloc_setDefault },
    { "_uloc_getAvailable", (void *)shim_uloc_getAvailable },
    { "_uloc_countAvailable", (void *)shim_uloc_countAvailable },
    { "_uloc_getISOLanguages", (void *)shim_uloc_getISOLanguages },
    { "_uloc_getISOCountries", (void *)shim_uloc_getISOCountries },
    { "_uloc_getLCID", (void *)shim_uloc_getLCID },
    { "_uloc_getLocaleForLCID", (void *)shim_uloc_getLocaleForLCID },
    { "_uloc_getCharacterOrientation", (void *)shim_uloc_getCharacterOrientation },
    { "_uloc_getLineOrientation", (void *)shim_uloc_getLineOrientation },
    { "_uloc_getKeywordValue", (void *)shim_uloc_getKeywordValue },
    { "_uloc_setKeywordValue", (void *)shim_uloc_setKeywordValue },
    { "_uloc_openKeywords", (void *)shim_uloc_openKeywords },
    { "_uloc_getDisplayName", (void *)shim_uloc_getDisplayName },
    { "_uloc_getDisplayKeyword", (void *)shim_uloc_getDisplayKeyword },
    { "_uloc_getDisplayKeywordValue", (void *)shim_uloc_getDisplayKeywordValue },
    { "_u_strlen", (void *)shim_u_strlen },
    { "_u_strstr", (void *)shim_u_strstr },
    { "_u_charsToUChars", (void *)shim_u_charsToUChars },
    { "_u_UCharsToChars", (void *)shim_u_UCharsToChars },
    { "_u_strFromUTF8", (void *)shim_u_strFromUTF8 },
    { NULL, NULL },
};

static uint64_t find_export_any(const char *name, struct image **prov);
void *plsr_image_symbol(const char *name) {
    if (!name || !*name) return NULL;
    struct image *prov = NULL;
    uint64_t a = find_export_any(name, &prov);
    if (!a && name[0] != '_') {
        char b[256];
        snprintf(b, sizeof b, "_%s", name);
        a = find_export_any(b, &prov);
    }
    return a ? (void *)(uintptr_t)a : NULL;
}

void *plsr_shim_lookup(const char *name) {
    if (!name || !*name) return NULL;
    void *p = shim_lookup(name);
    if (p) return p;
    if (name[0] == '_') return shim_lookup(name + 1);
    char b[256];
    snprintf(b, sizeof b, "_%s", name);
    return shim_lookup(b);
}

static void *shim_reef_low_alloc(size_t size) {
    if (size == 0) size = 1;
    void *p = NULL;
    if (posix_memalign(&p, 16, (size + 15) & ~(size_t)15) != 0)
        return NULL;
    return p;
}

static void shim_swift_dynamicCastFailure(void *a, void *b, void *c, void *d, void *e, void *f) {
    fprintf(stderr, "reef: swift_dynamicCastFailure a=%p b=%p c=%p d=%p e=%p f=%p\n", a,b,c,d,e,f);
    if (a) fprintf(stderr, "  a=%s\n", (const char *)a);
    if (c) fprintf(stderr, "  c=%s\n", (const char *)c);
    if (e) fprintf(stderr, "  e=%s\n", (const char *)e);
    if (f) fprintf(stderr, "  f=%s\n", (const char *)f);
    void *bt[64]; int n = backtrace(bt, 64);
    backtrace_symbols_fd(bt, n, 2);
    abort();
}
static void *shim_lookup(const char *name) {
    if (!name) return NULL;
    if (strstr(name, "swift_dynamicCastFailure"))
        return (void *)shim_swift_dynamicCastFailure;
    if (strcmp(name, "___reef_low_alloc") == 0 || strcmp(name, "__reef_low_alloc") == 0)
        return (void *)shim_reef_low_alloc;
    for (int i = 0; shims[i].name; i++)
        if (strcmp(shims[i].name, name) == 0)
            return shims[i].addr;
    /* Then the clean-room host libraries loaded with --shim-lib. Search the
     * exact Mach-O spelling across every shim before trying the stripped
     * spelling: dlsym(handle, "pthread_mutex_lock") also searches the
     * library's ELF dependencies, which would otherwise return glibc's symbol
     * before libplsr-libsystem's _pthread_mutex_lock is ever considered. */
    for (int i = g_nhostlibs - 1; i >= 0; i--) {
        void *p = dlsym(g_hostlibs[i], name);
        if (p) return p;
    }
    if (name[0] == '_') {
        for (int i = g_nhostlibs - 1; i >= 0; i--) {
            void *p = dlsym(g_hostlibs[i], name + 1);
            if (p) return p;
        }
    }
    return NULL;
}

/* ---- small utilities ---- */
static void die(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fputs("appexec: ", stderr); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap); exit(1);
}
static void set_err(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(g_err, sizeof g_err, fmt, ap);
    va_end(ap);
}
static void *xmalloc(size_t n) {
    void *p = calloc(1, n ? n : 1);
    if (!p) die("out of memory");
    return p;
}
static char *xstrdup(const char *s) {
    char *p = strdup(s ? s : "");
    if (!p) die("out of memory");
    return p;
}
static char *fmt(const char *f, ...) {
    va_list ap; char *p = NULL;
    va_start(ap, f);
    if (vasprintf(&p, f, ap) < 0) die("out of memory");
    va_end(ap);
    return p;
}
static void add_hostlib(const char *path) {
    if (g_nhostlibs >= MAX_HOSTLIBS) die("too many --shim-lib entries");
    void *h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (!h) die("cannot load shim library %s: %s", path, dlerror());
    g_hostlibs[g_nhostlibs] = h;
    g_hostimports[g_nhostlibs] =
        (const char *const *)dlsym(h, "plsr_host_imports");
    g_nhostlibs++;
}
static int host_provides(const char *name) {
    for (int i = 0; i < g_nhostlibs; i++) {
        const char *const *p = g_hostimports[i];
        if (!p) continue;
        for (; *p; p++)
            if (!strcmp(*p, name)) return 1;
    }
    return 0;
}
static int is_dir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
static char *dir_name(const char *path) {
    char *p = xstrdup(path);
    char *s = strrchr(p, '/');
    if (!s) { free(p); return xstrdup("."); }
    if (s == p) s[1] = 0;
    else *s = 0;
    return p;
}
static char *path_join(const char *a, const char *b) {
    if (!a || !*a) return xstrdup(b);
    if (!b || !*b) return xstrdup(a);
    size_t na = strlen(a), nb = strlen(b);
    while (na > 1 && a[na - 1] == '/') na--;
    char *p = xmalloc(na + nb + 2);
    memcpy(p, a, na); p[na] = '/';
    memcpy(p + na + 1, b, nb + 1);
    return p;
}

/* ---- direct host window (PLSR_HOST_WINDOW) ----
 * AppKit only reaches hostWindowCreate after it has a delegate and an
 * NSWindow, and the storyboard/delegate path is still blocked, so this asks
 * the host interface for a window directly.  The compat tree ships redirect
 * stubs (text files naming the real soname) that the dynamic linker cannot
 * open; preloading the real objects by path registers their SONAMEs, which
 * then satisfy libpulsar-app-host.so's NEEDED entries. */
static void plsr_preload_compat(void) {
    if (!g_root) return;
    static const char *deps[] = {
        "compat/libjpeg.so.62.3.0",
        /* The compatibility files have versioned SONAMEs.  dlopen the real
         * ELF files in dependency order so each SONAME is registered before
         * its consumers resolve NEEDED entries. */
        "compat/libicudata.so.76.1",
        "compat/libicuuc.so.76.1",
        "compat/libicui18n.so.76.1",
        "compat/libpulsar-toolkit-base.so.1.0",
        "compat/libpulsar-toolkit.so.1.0",
    };
    for (size_t i = 0; i < sizeof deps / sizeof deps[0]; i++) {
        char *p = path_join(g_root, deps[i]);
        void *h = dlopen(p, RTLD_NOW | RTLD_GLOBAL);
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: preload %s -> %s\n", deps[i],
                    h ? "ok" : dlerror());
        free(p);
    }
}

static void (*g_host_dispatch)(void);

struct entry_args {
    uintptr_t sp, entry;
    long nargs;
    char **av, **ev;
};

static void *plsr_entry_thread(void *p) {
    struct entry_args *a = (struct entry_args *)p;
    register long a0 __asm__("rdi") = a->nargs;
    register char **a1 __asm__("rsi") = a->av;
    register char **a2 __asm__("rdx") = a->ev;
    register char **a3 __asm__("rcx") = NULL;
    __asm__ __volatile__(
        "mov %[sp], %%rsp\n\t"
        "jmp *%[e]\n\t"
        :: [sp] "r"(a->sp), [e] "r"(a->entry),
           "D"(a0), "S"(a1), "d"(a2), "c"(a3)
        : "memory");
    return NULL;
}

/* ---- toolkit painting ----
 * The host library's content view paints through cutefish::ui::Painter.  Its
 * virtual paint() sits at vtable offset 0x100 (View::paintFlat compares that
 * slot against View::paint), so replacing that one slot lets us draw with the
 * toolkit's own painter instead of leaving AppKit's blank surface. */
/* ---- toolkit text ----
 * cutefish::ui::TextLayout is a pimpl: the public object is a single pointer to
 * a 400-byte private, and the default constructor already installs an empty
 * string, a default FontSpec and an opaque white colour.  That means no
 * FontSpec has to be built by hand: setText/setScale/draw are enough.  setText
 * takes std::string by value, which libstdc++ passes as a pointer to
 * {data, length, union{sso[16], capacity}}; the layout moves the buffer and
 * frees it with operator delete, so a long string is allocated with operator
 * new. */
struct cxx_string {
    char *ptr;
    size_t len;
    union { char sso[16]; size_t cap; } u;
};
static void *(*g_cxx_new)(size_t);
static void plsr_cxx_string(struct cxx_string *s, const char *t) {
    size_t n = strlen(t);
    memset(s, 0, sizeof *s);
    if (n < 16) {
        s->ptr = s->u.sso;
        s->len = n;
        memcpy(s->u.sso, t, n + 1);
        return;
    }
    if (!g_cxx_new) g_cxx_new = (void *(*)(size_t))dlsym(RTLD_DEFAULT, "_Znwm");
    char *p = g_cxx_new ? (char *)g_cxx_new(n + 1) : (char *)malloc(n + 1);
    if (!p) { s->ptr = s->u.sso; return; }
    memcpy(p, t, n + 1);
    s->ptr = p;
    s->len = n;
    s->u.cap = n;
}
static void *plsr_sym2(const char *a, const char *b) {
    void *p = dlsym(RTLD_DEFAULT, a);
    return p ? p : dlsym(RTLD_DEFAULT, b);
}
/* cutefish::ui::FontSpec is {std::string family; uint64 weight; uint16 x;
 * uint8 y; uint8 z;}.  TextLayout's default constructor leaves the family
 * empty, and an empty family resolves to no typeface, so shapes produce no
 * glyph runs.  Fill in a real family before laying the text out. */
struct plsr_fontspec {
    struct cxx_string family;
    double em;      /* +0x20, used as a double in the FontMetrics cache key */
    uint16_t px;    /* +0x28, the pixel size the toolkit shapes with */
    uint8_t b, c;   /* +0x29 italic, +0x2a */
};
static void plsr_fontspec_init(struct plsr_fontspec *f, const char *family) {
    memset(f, 0, sizeof *f);
    plsr_cxx_string(&f->family, family);
    const char *em = getenv("PLSR_TEXT_EM");
    const char *px = getenv("PLSR_TEXT_PX");
    /* FontSpec+0x20 is the size the toolkit shapes with: 1.0 gives 3px
     * glyphs, 16 gives ~16px ones.  Override with PLSR_TEXT_EM. */
    f->em = em ? atof(em) : 16.0;
    f->px = (uint16_t)(px ? atoi(px) : 16);
}
static void __attribute__((unused)) plsr_draw_text(void *painter, const char *text, double x, double y,
                           double scale, uint32_t color) {
    if (getenv("PLSR_NO_TEXT")) return;
    static int tried;
    static void (*ctor)(void *), (*dtor)(void *);
    static void (*settext)(void *, struct cxx_string *);
    static void (*setcolor)(void *, uint32_t);
    static void (*setscale)(void *, double, int);
    static void (*setfont)(void *, struct plsr_fontspec *);
    static void (*begin)(void *), (*end)(void *);
    static void (*createline)(void *, double, double, double);
    static void (*draw)(void *, void *, double, double);
    if (!tried) {
        tried = 1;
        ctor = (void (*)(void *))plsr_sym2("_ZN8cutefish2ui10TextLayoutC1Ev",
                                           "_ZN8cutefish2ui10TextLayoutC2Ev");
        dtor = (void (*)(void *))plsr_sym2("_ZN8cutefish2ui10TextLayoutD1Ev",
                                           "_ZN8cutefish2ui10TextLayoutD2Ev");
        settext = (void (*)(void *, struct cxx_string *))dlsym(
            RTLD_DEFAULT,
            "_ZN8cutefish2ui10TextLayout7setTextENSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE");
        setcolor = (void (*)(void *, uint32_t))dlsym(
            RTLD_DEFAULT,
            "_ZN8cutefish2ui10TextLayout15setDefaultColorENS0_5ColorE");
        setscale = (void (*)(void *, double, int))dlsym(
            RTLD_DEFAULT, "_ZN8cutefish2ui10TextLayout8setScaleEdb");
        setfont = (void (*)(void *, struct plsr_fontspec *))dlsym(
            RTLD_DEFAULT, "_ZN8cutefish2ui10TextLayout7setFontERKNS0_8FontSpecE");
        begin = (void (*)(void *))dlsym(RTLD_DEFAULT,
                                        "_ZN8cutefish2ui10TextLayout11beginLayoutEv");
        end = (void (*)(void *))dlsym(RTLD_DEFAULT,
                                      "_ZN8cutefish2ui10TextLayout9endLayoutEv");
        createline = (void (*)(void *, double, double, double))dlsym(
            RTLD_DEFAULT,
            "_ZN8cutefish2ui10TextLayout10createLineEdNS0_6PointFE");
        draw = (void (*)(void *, void *, double, double))dlsym(
            RTLD_DEFAULT,
            "_ZN8cutefish2ui10TextLayout4drawERNS0_7PainterENS0_6PointFE");
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr,
                    "reef: text ctor=%p setText=%p setFont=%p begin=%p "
                    "draw=%p\n",
                    (void *)ctor, (void *)settext, (void *)setfont,
                    (void *)begin, (void *)draw);
    }
    if (!ctor || !settext || !draw) return;
    void *layout[2] = {NULL, NULL};
    ctor(layout);
    if (setfont) {
        const char *fam = getenv("PLSR_TEXT_FONT");
        struct plsr_fontspec f;
        plsr_fontspec_init(&f, fam && *fam ? fam : "sans-serif");
        setfont(layout, &f);
        free(f.family.ptr == f.family.u.sso ? NULL : f.family.ptr);
    }
    struct cxx_string s;
    plsr_cxx_string(&s, text);
    settext(layout, &s);
    if (setscale) setscale(layout, scale, 1);
    if (setcolor) setcolor(layout, color);
    if (begin) begin(layout);
    /* Qt-style layout: beginLayout() opens the layout, createLine() supplies a
     * line box and its maximum width, endLayout() closes it.  Without the
     * createLine() call shape() emits no lines and draw() paints nothing. */
    if (createline) createline(layout, 1000.0, 0.0, 0.0);
    if (end) end(layout);
    if (getenv("PLSR_TEXT_DEBUG")) {
        unsigned char *p = *(unsigned char **)layout;
        char *fam = p ? *(char **)(p + 0x20) : NULL;
        fprintf(stderr,
                "reef: text '%s' priv=%p len=%zu scale=%.2f lines=%p..%p "
                "dirty=%d fam='%.24s' famlen=%zu\n",
                text, (void *)p, p ? *(size_t *)(p + 8) : 0,
                p ? *(double *)(p + 0x50) : 0.0,
                p ? *(void **)(p + 0x128) : NULL,
                p ? *(void **)(p + 0x130) : NULL, p ? p[0x180] : -1,
                fam ? fam : "(null)", p ? *(size_t *)(p + 0x28) : 0);
    }
    draw(layout, painter, x, y);
    if (dtor) dtor(layout);
}

/* ------------------------------------------------------------------ */
static struct { void *view; void (*orig)(void *, void *); } g_hooked[4];
static int g_hooked_n;
static void **g_paint_vts[4];
static int g_paint_calls;
static void plsr_toolkit_paint(void *view, void *painter) {
    if (getenv("PLSR_REEF_TRACE") && g_paint_calls < 3)
        fprintf(stderr, "reef: toolkit paint view=%p painter=%p\n", view, painter);
    g_paint_calls++;
    for (int i = 0; i < g_hooked_n; i++)
        if (g_hooked[i].view == view && g_hooked[i].orig)
            g_hooked[i].orig(view, painter);
}

static void plsr_hook_view(void *view) {
    if (!view || g_hooked_n >= 4) return;
    void **vt = *(void ***)view;
    if (!vt) return;
    void **copy = malloc(64 * sizeof(void *));
    if (!copy) return;
    for (int i = 0; i < 64; i++) copy[i] = vt[i];
    g_paint_vts[g_hooked_n] = copy;
    g_hooked[g_hooked_n].view = view;
    g_hooked[g_hooked_n].orig = (void (*)(void *, void *))copy[32];
    g_hooked_n++;
    copy[32] = (void *)plsr_toolkit_paint;
    *(void ***)view = copy;
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: paint hook on view %p (vptr %p orig %p)\n",
                view, (void *)vt, (void *)g_hooked[g_hooked_n - 1].orig);
}
static void plsr_install_paint_hook(void *win) {
    if (!getenv("PLSR_PAINT_HOOK")) return;
    if (!win) return;
    plsr_hook_view(*(void **)((char *)win + 0x20));
    plsr_hook_view(*(void **)((char *)win + 0x28));
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: paint fill=%p\n",
                dlsym(RTLD_DEFAULT,
                      "_ZN8cutefish2ui14fillDeviceRectERNS0_7PainterERKNS0_4RectENS0_5ColorE"));
}

/* ---- Mayonnaise front-end ----
 * The loaded app's AppKit window is not reachable yet, so this draws the
 * reimplementation's own window with the vendored Mayonnaise toolkit instead
 * of leaving the host surface blank.  Enabled with PLSR_MAYO_UI=1. */
extern int plsr_mayo_ui_run(int argc, char **argv) __attribute__((weak));
static int plsr_mayo_ui_main(int argc, char **argv, const char *exe) {
    const char *name = getenv("PLSR_APP_NAME");
    char base[256];
    if ((!name || !*name) && exe) {
        const char *s = strrchr(exe, '/');
        s = s ? s + 1 : exe;
        snprintf(base, sizeof base, "%s", s);
        char *dot = strrchr(base, '.');
        if (dot && !strcmp(dot, ".app")) *dot = 0;
        name = base;
    }
    setenv("PLSR_APP_NAME", (name && *name) ? name : "CotEditor", 1);
    setenv("PLSR_APP_EXE", exe ? exe : "", 1);
    if (exe) {
        const char *dot = strstr(exe, ".app");
        if (dot) {
            char *b = strndup(exe, (size_t)(dot - exe) + 4);
            if (b) { setenv("PLSR_APP_BUNDLE", b, 1); free(b); }
        }
    }
    {
        char summary[160];
        snprintf(summary, sizeof summary,
                 "%d images, %llu symbols, %llu unresolved", g_nimgs,
                 (unsigned long long)g_bound, (unsigned long long)g_unresolved);
        setenv("PLSR_APP_SUMMARY", summary, 1);
    }
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: mayonnaise ui for '%s'\n", getenv("PLSR_APP_NAME"));
    if (!plsr_mayo_ui_run) {
        fprintf(stderr, "appexec: Mayonnaise UI is not linked into this build\n");
        return 1;
    }
    return plsr_mayo_ui_run(argc, argv);
}

static void plsr_host_window_bootstrap(int argc, char **argv) {
    plsr_preload_compat();
    char *path = path_join(g_root ? g_root : ".", "host/libpulsar-app-host.so");
    void *h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (!h) {
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: host window: dlopen %s: %s\n", path, dlerror());
        free(path);
        return;
    }
    free(path);
    void *(*iface)(int) = (void *(*)(int))dlsym(h, "reef_appkit_host_interface");
    if (!iface) return;
    void **vt = (void **)iface(0x17);
    if (!vt) return;
    /* The table's first word is a header; interface entry i is vt[i + 1]. */
#define HV(i) (vt[(i) + 1])
    static void *cfg[20];
    int rc = ((int (*)(int, char **, const char *, void **))HV(0))(
        argc, argv, "CotEditor", cfg);
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: host window init -> %d (table %p)\n", rc, (void *)vt);
    if (rc != 0 || !HV(10)) return;
    if (HV(6)) ((void (*)(void))HV(6))();
    void *win = ((void *(*)(double, double, void *, void *, void *, unsigned, long))
                 HV(10))(800.0, 948.0, NULL, NULL, NULL, 0, 0);
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: host window create -> %p\n", win);
    if (!win) return;
    if (HV(12)) ((void (*)(void *, const char *))HV(12))(win, "Untitled");
    plsr_install_paint_hook(win);
    if (HV(16)) ((void (*)(void *))HV(16))(win);
    /* The toolkit's event loop is not thread-safe: dispatching from a
     * pthread crashes it, and without any dispatch the surface is never
     * committed.  Pump it on this thread long enough for the compositor to
     * take the buffer, then hand the main thread back to the app. */
    int present_ms = 600;
    if (getenv("PLSR_WIN_PRESENT_MS")) present_ms = atoi(getenv("PLSR_WIN_PRESENT_MS"));
    g_host_dispatch = (void (*)(void))HV(2);
    if (HV(2)) {
        for (int i = 0; i < present_ms / 16; i++) {
            ((void (*)(void))HV(2))();
            usleep(16000);
        }
    }
    const char *grab = getenv("PLSR_WIN_GRAB");
    if (grab && HV(28)) {
        int g = ((int (*)(void *, const char *))HV(28))(win, grab);
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: host window grab '%s' -> %d\n", grab, g);
    }
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: host window shown\n");
#undef HV
}
static void map_add(const char *name, const char *path) {
    if (g_nmaps >= MAX_MAPS) die("too many --image-map entries");
    g_maps[g_nmaps].name = xstrdup(name);
    g_maps[g_nmaps].path = xstrdup(path);
    g_nmaps++;
}
static void read_image_map(const char *file) {
    FILE *f = fopen(file, "r");
    if (!f) die("cannot open image map %s: %s", file, strerror(errno));
    char line[PATH_MAX * 2];
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || !*s) continue;
        char *sep = strpbrk(s, " \t=");
        if (!sep) continue;
        *sep++ = 0;
        while (*sep == ' ' || *sep == '\t' || *sep == '=') sep++;
        size_t n = strlen(sep);
        while (n && (sep[n - 1] == '\n' || sep[n - 1] == '\r')) sep[--n] = 0;
        if (*s && *sep) map_add(s, sep);
    }
    fclose(f);
}

static void print_frame_map(void *addr);
static void print_host_addr(const char *tag, uintptr_t addr) {
    Dl_info info;
    if (addr && dladdr((void *)addr, &info) && info.dli_fname) {
        fprintf(stderr, "  %s=%p %s+0x%lx (%s)\n", tag, (void *)addr,
                info.dli_fname,
                (unsigned long)((char *)addr - (char *)info.dli_fbase),
                info.dli_sname ? info.dli_sname : "?");
    } else {
        fprintf(stderr, "  %s=%p (unmapped)\n", tag, (void *)addr);
    }
}
/* _objc_msgSend dereferences the isa at libobjc+0xe5d2.  Some callers reach
 * that slow path without passing the patched entry, so contain the fault here
 * instead: resume at the function's nil-return epilogue (+0xe70c).  msgSend has
 * not pushed a frame at that point, so the epilogue returns to the caller. */
static int plsr_recover_msgsend(ucontext_t *uc) {
    uintptr_t rip = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    for (int i = 0; i < g_nimgs; i++) {
        if (!strstr(g_imgs[i]->path, "libobjc.A.dylib")) continue;
        uintptr_t base = (uintptr_t)(g_imgs[i]->base + g_imgs[i]->slide);
        if (rip == base + 0xe5d2 || rip == base + 0xe5dd) {
            uc->uc_mcontext.gregs[REG_RIP] = (greg_t)(base + 0xe70c);
            return 1;
        }
    }
    return 0;
}

/* glibc calloc can fault on the corrupted heap left by the shipped CF
 * runtime.  The caller is our tracked allocator; resume just after its calloc
 * call with a NULL result so the CF object constructor can fail cleanly
 * instead of taking down the process. */
extern void *plsr_calloc_recover(size_t, size_t);

static int plsr_recover_calloc(ucontext_t *uc) {
    uintptr_t rsp = (uintptr_t)uc->uc_mcontext.gregs[REG_RSP];
    if (!rsp) return 0;
    unsigned char vec = 0;
    if (mincore((void *)(rsp & ~(uintptr_t)0xfff), 0x1000, &vec) != 0)
        return 0;
    uintptr_t ret = *(const uintptr_t *)rsp;
    uintptr_t fn = (uintptr_t)(void *)plsr_calloc_tracked;
    if (ret < fn || ret >= fn + 0x200) return 0;
    uc->uc_mcontext.gregs[REG_RIP] = (greg_t)ret;
    uc->uc_mcontext.gregs[REG_RSP] = (greg_t)(rsp + sizeof(uintptr_t));
    size_t n = (size_t)uc->uc_mcontext.gregs[REG_RDI];
    size_t s = (size_t)uc->uc_mcontext.gregs[REG_RSI];
    void *p = plsr_calloc_recover(n, s);
    uc->uc_mcontext.gregs[REG_RAX] = (greg_t)p;
    static const char msg[] = "reef: recovered calloc fault\n";
    write(2, msg, sizeof msg - 1);
    return 1;
}

/* Cocoa treats a nil array as empty; the shipped _CFArrayGetCount dereferences
 * it.  Resume at its caller with 0 when the receiver is nil. */
static int plsr_recover_cfarray_count(ucontext_t *uc) {
    if ((uintptr_t)uc->uc_mcontext.gregs[REG_RDI] != 0) return 0;
    uintptr_t rip = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    for (int i = 0; i < g_nimgs; i++) {
        if (!strstr(g_imgs[i]->path, "CoreFoundation.framework")) continue;
        uintptr_t base = (uintptr_t)(g_imgs[i]->base + g_imgs[i]->slide);
        if (rip != base + 0x2e0a) continue;
        uintptr_t rsp = (uintptr_t)uc->uc_mcontext.gregs[REG_RSP];
        uintptr_t ret = *(const uintptr_t *)(rsp + 8);
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)ret;
        uc->uc_mcontext.gregs[REG_RSP] = (greg_t)(rsp + 16);
        uc->uc_mcontext.gregs[REG_RAX] = 0;
        static const char msg[] = "reef: recovered _CFArrayGetCount(NULL) -> 0\n";
        write(2, msg, sizeof msg - 1);
        return 1;
    }
    return 0;
}

/* CFRelease(NULL) is another nil-receiver case the shipped runtime traps on.
 * Skip the release and return to its caller. */
static int plsr_recover_cfrelease_null(ucontext_t *uc) {
    if ((uintptr_t)uc->uc_mcontext.gregs[REG_RDI] != 0) return 0;
    uintptr_t rip = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    for (int i = 0; i < g_nimgs; i++) {
        if (!strstr(g_imgs[i]->path, "CoreFoundation.framework")) continue;
        uintptr_t base = (uintptr_t)(g_imgs[i]->base + g_imgs[i]->slide);
        if (rip != base + 0x80283) continue;
        uintptr_t rsp = (uintptr_t)uc->uc_mcontext.gregs[REG_RSP];
        uintptr_t ret = *(const uintptr_t *)(rsp + 8);
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)ret;
        uc->uc_mcontext.gregs[REG_RSP] = (greg_t)(rsp + 16);
        uc->uc_mcontext.gregs[REG_RAX] = 0;
        static const char msg[] = "reef: recovered _CFRelease(NULL)\n";
        write(2, msg, sizeof msg - 1);
        return 1;
    }
    return 0;
}

/* ___CFTypeCollectionRetain traps when the app hands it an object outside
 * the CF runtime.  It is a leaf function, so the caller is at [rsp]. */
static int plsr_recover_cftype_retain(ucontext_t *uc) {
    uintptr_t rip = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    for (int i = 0; i < g_nimgs; i++) {
        if (!strstr(g_imgs[i]->path, "CoreFoundation.framework")) continue;
        uintptr_t base = (uintptr_t)(g_imgs[i]->base + g_imgs[i]->slide);
        uintptr_t rsp = (uintptr_t)uc->uc_mcontext.gregs[REG_RSP];
        uintptr_t ret;
        if (rip == base + 0x8039e) {
            ret = *(const uintptr_t *)rsp;
            rsp += 8;
        } else if (rip == base + 0x803f9) {
            ret = *(const uintptr_t *)(rsp + 8);
            rsp += 16;
        } else {
            continue;
        }
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)ret;
        uc->uc_mcontext.gregs[REG_RSP] = (greg_t)rsp;
        uc->uc_mcontext.gregs[REG_RAX] = 0;
        static const char msg[] = "reef: recovered _CFTypeCollection(invalid)\n";
        write(2, msg, sizeof msg - 1);
        return 1;
    }
    return 0;
}

static void plsr_safe_cstr(const char *tag, const void *p) {
    uintptr_t v = (uintptr_t)p;
    if (!v) return;
    unsigned char vec = 0;
    if (mincore((void *)(v & ~(uintptr_t)0xfff), 0x1000, &vec) != 0) return;
    char buf[96];
    size_t i = 0;
    for (; i + 1 < sizeof buf; i++) {
        char c = ((const char *)p)[i];
        if (!c) break;
        if (c < 0x20 || c > 0x7e) { buf[i] = 0; break; }
        buf[i] = c;
    }
    buf[i] = 0;
    fprintf(stderr, "  %s='%s'\n", tag, buf);
}

static void fault(int sig, siginfo_t *si, void *uc_) {
    ucontext_t *uc = (ucontext_t *)uc_;
    if ((sig == SIGSEGV || sig == SIGBUS || sig == SIGILL) && plsr_recover_msgsend(uc))
        return;
    if ((sig == SIGSEGV || sig == SIGBUS || sig == SIGILL) && plsr_recover_calloc(uc))
        return;
    if ((sig == SIGSEGV || sig == SIGBUS || sig == SIGILL) && plsr_recover_cfarray_count(uc))
        return;
    if ((sig == SIGSEGV || sig == SIGBUS || sig == SIGILL) && plsr_recover_cfrelease_null(uc))
        return;
    if ((sig == SIGSEGV || sig == SIGBUS || sig == SIGILL) && plsr_recover_cftype_retain(uc))
        return;
    /* CotEditor 0x3ec300: Swift-forced-unwrap on stringForKey: result.
     * When defaults/KVC returns nil, the original app would fatalError; recover
     * by substituting an empty NSString and resuming at the non-nil path. */
    if (sig == SIGILL) {
        uintptr_t rip = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
        for (int i = 0; i < g_nimgs; i++) {
            if (!strstr(g_imgs[i]->path, "CotEditor.app/Contents/MacOS/CotEditor"))
                continue;
            uintptr_t base = (uintptr_t)(g_imgs[i]->base + g_imgs[i]->slide);
            if (rip == base + 0x3ec370) {
                static void *empty_nsstr;
                if (!empty_nsstr) {
                    void *(*cfstr)(const void *, const char *, unsigned) =
                        (void *(*)(const void *, const char *, unsigned))
                            plsr_image_symbol("_CFStringCreateWithCString");
                    if (cfstr)
                        empty_nsstr = cfstr(NULL, "", 0x08000100); /* kCFStringEncodingUTF8 */
                }
                if (empty_nsstr) {
                    void *key = (void *)uc->uc_mcontext.gregs[REG_R14];
                    const char *kstr = NULL;
                    if (plsr_valid_obj(key)) {
                        void *(*msg1)(void*,void*) = (void*(*)(void*,void*))plsr_image_symbol("_objc_msgSend");
                        void *(*sel)(const char*) = (void*(*)(const char*))plsr_image_symbol("_sel_registerName");
                        if (msg1 && sel) kstr = (const char *)msg1(key, sel("UTF8String"));
                    }
                    fprintf(stderr, "reef: recovered CotEditor nil stringForKey unwrap key=%s (%p)\n",
                            kstr ? kstr : "?", key);
                    uc->uc_mcontext.gregs[REG_RBX] = (greg_t)empty_nsstr;
                    uc->uc_mcontext.gregs[REG_RIP] = (greg_t)(base + 0x3ec348);
                    return;
                }
            }
            break;
        }
    }
    fprintf(stderr, "appexec: signal %d rip=%p addr=%p r11=%p rdi=%p rsi=%p rdx=%p\n",
            sig, (void *)uc->uc_mcontext.gregs[REG_RIP], si->si_addr,
            (void *)uc->uc_mcontext.gregs[REG_R11], (void *)uc->uc_mcontext.gregs[REG_RDI],
            (void *)uc->uc_mcontext.gregs[REG_RSI], (void *)uc->uc_mcontext.gregs[REG_RDX]);
    plsr_safe_cstr("selector", (const void *)uc->uc_mcontext.gregs[REG_RSI]);
    {
        uintptr_t obj = (uintptr_t)uc->uc_mcontext.gregs[REG_RDI];
        unsigned char vec = 0;
        if (obj && mincore((void *)(obj & ~(uintptr_t)0xfff), 0x1000, &vec) == 0) {
            uintptr_t isa = *(const uintptr_t *)obj;
            /* Validate every page before dereferencing: on a Swift refcount
             * fault rdi's "object" is not ObjC and its isa word is metadata
             * or garbage; a blind read SIGBUSes inside the handler. */
            if (isa && mincore((void *)(isa & ~(uintptr_t)0xfff), 0x1000, &vec) == 0) {
                plsr_safe_cstr("class", (const void *)isa);
                if (mincore((void *)((isa + 0x18) & ~(uintptr_t)0xfff), 0x1000, &vec) == 0) {
                    uintptr_t ro = *(const uintptr_t *)(isa + 0x18) & ~(uintptr_t)7;
                    if (ro && mincore((void *)((ro + 0x18) & ~(uintptr_t)0xfff), 0x1000, &vec) == 0)
                        plsr_safe_cstr("class-name", *(const char **)(ro + 0x18));
                }
            }
        }
    }
    fprintf(stderr, "  rax=%p rcx=%p rbx=%p r8=%p r9=%p r12=%p r13=%p r14=%p r15=%p\n",
            (void *)uc->uc_mcontext.gregs[REG_RAX],
            (void *)uc->uc_mcontext.gregs[REG_RCX], (void *)uc->uc_mcontext.gregs[REG_RBX],
            (void *)uc->uc_mcontext.gregs[REG_R8], (void *)uc->uc_mcontext.gregs[REG_R9],
            (void *)uc->uc_mcontext.gregs[REG_R12], (void *)uc->uc_mcontext.gregs[REG_R13],
            (void *)uc->uc_mcontext.gregs[REG_R14], (void *)uc->uc_mcontext.gregs[REG_R15]);
    if ((sig == SIGSEGV || sig == SIGBUS || sig == SIGILL) &&
        plsr_valid_obj((void *)uc->uc_mcontext.gregs[REG_RDI])) {
        const uintptr_t *cls = (const uintptr_t *)uc->uc_mcontext.gregs[REG_RDI];
        fprintf(stderr, "  rdi words: +8=%#lx +16=%#lx +24=%#lx +32=%#lx\n",
                (unsigned long)cls[1], (unsigned long)cls[2],
                (unsigned long)cls[3], (unsigned long)cls[4]);
    }
    {
        /* Swift native heap object dump: swift_release faults land in
         * RefCounts::doDecrementSlow with rdi = &object->refCounts, so the
         * HeapObject header sits 8 bytes below and its isa/refcount words
         * distinguish metadata corruption from a wild pointer. */
        uintptr_t sobj = (uintptr_t)uc->uc_mcontext.gregs[REG_RDI] - 8;
        unsigned char vec = 0;
        if (sobj && mincore((void *)(sobj & ~(uintptr_t)0xfff), 0x1000, &vec) == 0) {
            uintptr_t isa = *(const uintptr_t *)sobj;
            uintptr_t rc = *(const uintptr_t *)(sobj + 8);
            fprintf(stderr, "  heapobj=%p isa=%p refcounts=%p\n", (void *)sobj,
                    (void *)isa, (void *)rc);
            fprintf(stderr, "  heapobj-isa %s\n", plsr_describe_addr((const void *)isa));
            fprintf(stderr, "  heapobj-addr %s\n", plsr_describe_addr((const void *)sobj));
        }
    }
    {
        uintptr_t rip = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
        uintptr_t rsp = (uintptr_t)uc->uc_mcontext.gregs[REG_RSP];
        fprintf(stderr, "  rsp=%p\n", (void *)rsp);
        print_host_addr("rip", rip);
        print_host_addr("rdi", (uintptr_t)uc->uc_mcontext.gregs[REG_RDI]);
        print_frame_map((void *)rip);
        print_frame_map(si->si_addr);
        print_frame_map((void *)uc->uc_mcontext.gregs[REG_R13]);
        print_frame_map((void *)uc->uc_mcontext.gregs[REG_RDI]);
        for (int i = 0; i < 12; i++) {
            uintptr_t value = ((const uintptr_t *)rsp)[i];
            fprintf(stderr, "  stack[%02d]=%p %s\n", i, (void *)value,
                    plsr_describe_addr((void *)value));
            if (i < 4) {
                print_host_addr("  stack", value);
                print_frame_map((void *)value);
            }
        }
        for (int i = 0; i < g_nimgs; i++) {
            struct image *im = g_imgs[i];
            uintptr_t ibase = (uintptr_t)(im->base + im->slide);
            if (rip >= ibase && rip < ibase + im->span)
                fprintf(stderr, "  image %s +0x%lx (span 0x%lx)\n", im->path,
                        (unsigned long)(rip - ibase), (unsigned long)im->span);
            for (int j = 0; j < im->nsegs; j++) {
                const struct segment_command_64 *s = im->segs[j];
                uintptr_t start;
                if (!s || s->vmsize == 0 || !memcmp(s->segname, "__PAGEZERO", 11))
                    continue;
                start = (uintptr_t)(s->vmaddr + im->slide);
                if (rip >= start && rip < start + (uintptr_t)s->vmsize) {
                    fprintf(stderr, "  segment %s +0x%lx (%s)\n", im->path,
                            (unsigned long)(rip - start), s->segname);
                }
            }
        }
    }
    if (getenv("PLSR_LITE_BT")) {
        void *frames[32];
        int n = backtrace(frames, 32);
        for (int i = 0; i < n; i++) {
            Dl_info info;
            if (dladdr(frames[i], &info) && info.dli_fname)
                dprintf(2, "  #%d %p %s+0x%lx\n", i, frames[i], info.dli_fname,
                        (unsigned long)((char *)frames[i] - (char *)info.dli_fbase));
            else {
                dprintf(2, "  #%d %p\n", i, frames[i]);
                print_frame_map(frames[i]);
            }
        }
    }
    _exit(128 + sig);
}
static void install_fault_handlers(void) {
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = fault; sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGILL, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGTRAP, &sa, NULL); sigaction(SIGABRT, &sa, NULL);
}
/*
 * Mach-O thread-local variables.  Each `__DATA,__thread_vars` descriptor is
 * { thunk, metadata, offset }.  At link time the thunk is __tlv_bootstrap,
 * which only registers destructors and returns no address; dyld replaces it
 * with an accessor that returns the variable's address.  A manually mapped
 * image graph has to do the same, so emit one tiny trampoline per descriptor:
 *
 *     movabs $descriptor, %rdi ; movabs $_reef_tlv_get_addr, %rax ; jmp *%rax
 *
 * The call sites invoke the thunk with no arguments, exactly like a compiler
 * generated accessor.
 */
static uint64_t find_export_any(const char *name, struct image **prov);

static unsigned char *g_tlv_code;
static size_t g_tlv_used, g_tlv_cap;

static void *tlv_trampoline(void *desc, uint64_t accessor) {
    if (!g_tlv_code || g_tlv_used + 24 > g_tlv_cap) {
        size_t cap = 64 * 1024;
        void *p = mmap(NULL, cap, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) return NULL;
        g_tlv_code = p;
        g_tlv_cap = cap;
        g_tlv_used = 0;
    }
    unsigned char *p = g_tlv_code + g_tlv_used;
    p[0] = 0x48; p[1] = 0xbf;                 /* movabs $desc, %rdi */
    memcpy(p + 2, &desc, 8);
    p[10] = 0x48; p[11] = 0xb8;               /* movabs $accessor, %rax */
    memcpy(p + 12, &accessor, 8);
    p[20] = 0xff; p[21] = 0xe0;               /* jmp *%rax */
    g_tlv_used += 24;
    return p;
}

static const struct section_64 *image_section(struct image *im, const char *name) {
    for (int s = 0; s < im->nsegs; s++) {
        const struct segment_command_64 *seg = im->segs[s];
        const struct section_64 *sect = (const struct section_64 *)(seg + 1);
        for (uint32_t j = 0; j < seg->nsects; j++, sect++)
            if (strncmp(sect->sectname, name, 16) == 0) return sect;
    }
    return NULL;
}

/*
 * libSystem.B's accessor reads the descriptor's second word as a metadata
 * record:
 *
 *     +0x00 initial contents      +0x08 initial size
 *     +0x10 block size            +0x18 key index
 *
 * `_reef_tlv_lookup` allocates one `block size` buffer per key on first use,
 * copies the initial contents and returns `block + descriptor->offset`.  All
 * descriptors of a `__thread_vars` section share one block (and one key), so
 * the section's `__thread_data`/`__thread_bss` sizes define it.
 */
#define PLSR_TLV_META_MAGIC 0x504C5352544C5631ULL
struct plsr_tlv_meta {
    uint64_t init;      /* __thread_data address (slide applied) */
    uint64_t init_sz;   /* bytes to copy from __thread_data */
    uint64_t total;     /* bytes to allocate per thread */
    uint64_t key;       /* per-section key index */
    uint64_t magic;     /* PLSR_TLV_META_MAGIC */
};
struct plsr_tlv_desc {
    void *thunk;
    struct plsr_tlv_meta *meta;
    uint64_t offset;
};

/* libSystem.B's shipped accessor reads the TLV array through %fs, but on this
 * host %fs belongs to glibc, so fs:-0x1818 is unrelated memory (in practice a
 * small integer, which is why it faults at address 0x8).  Replace the accessor
 * with a self-contained implementation: every thread owns one zero-filled
 * block per section key, seeded from that section's __thread_data. */
static __thread void **g_tlv_blocks;
static __thread size_t g_tlv_nblocks;
static int g_tlv_bad_reported;

void *plsr_tlv_get_addr_c(void *desc_, void *caller);

void *plsr_tlv_get_addr_c(void *desc_, void *caller) {
    struct plsr_tlv_desc *d = desc_;
    if (!d) {
        if (!g_tlv_bad_reported) {
            g_tlv_bad_reported = 1;
            fprintf(stderr, "reef: TLV accessor called with NULL descriptor from %p\n",
                    caller);
        }
        return NULL;
    }
    struct plsr_tlv_meta *m = d->meta;
    if (!m || m->magic != PLSR_TLV_META_MAGIC) {
        if (!g_tlv_bad_reported) {
            g_tlv_bad_reported = 1;
            fprintf(stderr, "reef: TLV descriptor %p has foreign metadata %p from %p\n",
                    desc_, (void *)m, caller);
        }
        return NULL;
    }
    if (m->key >= 65536) return NULL;
    if (m->key >= g_tlv_nblocks) {
        size_t n = g_tlv_nblocks ? g_tlv_nblocks : 16;
        while (n <= m->key) n *= 2;
        void **nb = realloc(g_tlv_blocks, n * sizeof *nb);
        if (!nb) return NULL;
        memset(nb + g_tlv_nblocks, 0, (n - g_tlv_nblocks) * sizeof *nb);
        g_tlv_blocks = nb;
        g_tlv_nblocks = n;
    }
    if (!g_tlv_blocks[m->key]) {
        size_t total = m->total ? (size_t)m->total : 8;
        void *blk = calloc(1, total);
        if (!blk) return NULL;
        if (m->init && m->init_sz)
            memcpy(blk, (const void *)(uintptr_t)m->init, (size_t)m->init_sz);
        g_tlv_blocks[m->key] = blk;
    }
    return (char *)g_tlv_blocks[m->key] + d->offset;
}

/* A Darwin TLV accessor is not an ordinary call: the compiler treats it as a
 * load and assumes every register except rax survives.  libdispatch relies on
 * that (it keeps the dispatch_once context and function in rsi/rdx across the
 * TSD accessor), so the C implementation above must be wrapped in a register
 * preserving shim.  Keep the original descriptor in rdi and pass the real
 * caller's return address as the second argument for diagnostics. */
__asm__(
".text\n"
".globl plsr_tlv_get_addr_asm\n"
".type plsr_tlv_get_addr_asm,@function\n"
"plsr_tlv_get_addr_asm:\n"
"  push %rdi\n"
"  push %rsi\n"
"  push %rdx\n"
"  push %rcx\n"
"  push %r8\n"
"  push %r9\n"
"  push %r10\n"
"  push %r11\n"
"  sub $264, %rsp\n"
"  movups %xmm0, 0(%rsp)\n"
"  movups %xmm1, 16(%rsp)\n"
"  movups %xmm2, 32(%rsp)\n"
"  movups %xmm3, 48(%rsp)\n"
"  movups %xmm4, 64(%rsp)\n"
"  movups %xmm5, 80(%rsp)\n"
"  movups %xmm6, 96(%rsp)\n"
"  movups %xmm7, 112(%rsp)\n"
"  movups %xmm8, 128(%rsp)\n"
"  movups %xmm9, 144(%rsp)\n"
"  movups %xmm10, 160(%rsp)\n"
"  movups %xmm11, 176(%rsp)\n"
"  movups %xmm12, 192(%rsp)\n"
"  movups %xmm13, 208(%rsp)\n"
"  movups %xmm14, 224(%rsp)\n"
"  movups %xmm15, 240(%rsp)\n"
"  mov 328(%rsp), %rsi\n"
"  call plsr_tlv_get_addr_c\n"
"  movups 240(%rsp), %xmm15\n"
"  movups 224(%rsp), %xmm14\n"
"  movups 208(%rsp), %xmm13\n"
"  movups 192(%rsp), %xmm12\n"
"  movups 176(%rsp), %xmm11\n"
"  movups 160(%rsp), %xmm10\n"
"  movups 144(%rsp), %xmm9\n"
"  movups 128(%rsp), %xmm8\n"
"  movups 112(%rsp), %xmm7\n"
"  movups 96(%rsp), %xmm6\n"
"  movups 80(%rsp), %xmm5\n"
"  movups 64(%rsp), %xmm4\n"
"  movups 48(%rsp), %xmm3\n"
"  movups 32(%rsp), %xmm2\n"
"  movups 16(%rsp), %xmm1\n"
"  movups 0(%rsp), %xmm0\n"
"  add $264, %rsp\n"
"  pop %r11\n"
"  pop %r10\n"
"  pop %r9\n"
"  pop %r8\n"
"  pop %rcx\n"
"  pop %rdx\n"
"  pop %rsi\n"
"  pop %rdi\n"
"  ret\n"
".size plsr_tlv_get_addr_asm, .-plsr_tlv_get_addr_asm\n"
);
extern void *plsr_tlv_get_addr_asm(void *);

static void patch_tlv_accessor(void) {
    if (getenv("PLSR_NO_TLV_ACCESSOR")) return;
    for (int i = 0; i < g_nimgs; i++) {
        if (!strstr(g_imgs[i]->path, "libSystem.B.dylib")) continue;
        unsigned char *fn =
            (unsigned char *)(uintptr_t)(g_imgs[i]->slide + 0x4ecd0);
        /* _reef_tlv_get_addr: push rcx; mov rcx,[rip+off]; mov rcx,fs:[rcx] */
        if (fn[0] != 0x51 || fn[1] != 0x48 || fn[8] != 0x64) {
            if (getenv("PLSR_REEF_TRACE"))
                fprintf(stderr, "reef: unexpected _reef_tlv_get_addr prologue\n");
            return;
        }
        long pg = sysconf(_SC_PAGESIZE);
        if (pg <= 0) pg = 4096;
        uintptr_t page = (uintptr_t)fn & ~(uintptr_t)(pg - 1);
        if (mprotect((void *)page, (size_t)pg,
                     PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
            return;
        unsigned char code[12] = { 0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xe0 };
        uint64_t h = (uint64_t)(uintptr_t)plsr_tlv_get_addr_asm;
        memcpy(code + 2, &h, 8);
        memcpy(fn, code, sizeof code);
        __builtin___clear_cache((char *)fn, (char *)fn + sizeof code);
        mprotect((void *)page, (size_t)pg, PROT_READ | PROT_EXEC);
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: replaced _reef_tlv_get_addr with %p\n",
                    (void *)plsr_tlv_get_addr_asm);
        return;
    }
}

static void patch_tlv_descriptors(void) {
    struct image *prov = NULL;
    uint64_t accessor = find_export_any("_reef_tlv_get_addr", &prov);
    if (!accessor) {
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: _reef_tlv_get_addr not found\n");
        return;
    }
    long pg = sysconf(_SC_PAGESIZE);
    if (pg <= 0) pg = 4096;
    int patched = 0, blocks = 0;
    uintptr_t next_key = 0;
    for (int i = 0; i < g_nimgs; i++) {
        struct image *im = g_imgs[i];
        for (int s = 0; s < im->nsegs; s++) {
            const struct segment_command_64 *seg = im->segs[s];
            const struct section_64 *sect = (const struct section_64 *)(seg + 1);
            for (uint32_t j = 0; j < seg->nsects; j++, sect++) {
                if (strncmp(sect->sectname, "__thread_vars", 16) != 0)
                    continue;
                const struct section_64 *td = image_section(im, "__thread_data");
                const struct section_64 *tb = image_section(im, "__thread_bss");
                uint64_t init = td ? (uint64_t)(td->addr + im->slide) : 0;
                size_t init_sz = td ? (size_t)td->size : 0;
                size_t total = init_sz + (tb ? (size_t)tb->size : 0);
                uintptr_t base = (uintptr_t)(sect->addr + im->slide);
                for (size_t off = 0; off + 24 <= sect->size; off += 24) {
                    uint64_t voff = *(const uint64_t *)(base + off + 16);
                    if (voff + 8 > total) total = voff + 8;
                }
                if (total == 0) total = 8;
                struct plsr_tlv_meta *meta = calloc(1, sizeof *meta);
                if (!meta) continue;
                meta->init = init;
                meta->init_sz = init_sz;
                meta->total = total;
                meta->key = next_key++;
                meta->magic = PLSR_TLV_META_MAGIC;
                blocks++;
                uintptr_t pgaddr = base & ~(uintptr_t)(pg - 1);
                size_t pglen = (size_t)((base + sect->size - pgaddr + (uintptr_t)pg - 1)
                                        & ~((uintptr_t)pg - 1));
                mprotect((void *)pgaddr, pglen, PROT_READ | PROT_WRITE);
                for (size_t off = 0; off + 24 <= sect->size; off += 24) {
                    void *desc = (void *)(base + off);
                    void *tramp = tlv_trampoline(desc, accessor);
                    if (!tramp) break;
                    if (!*(void **)((char *)desc + 8))
                        memcpy((char *)desc + 8, &meta, sizeof meta);
                    memcpy(desc, &tramp, sizeof tramp);
                    patched++;
                }
                { int prot = 0;
                  if (seg->initprot & 1) prot |= PROT_READ;
                  if (seg->initprot & 2) prot |= PROT_WRITE;
                  if (seg->initprot & 4) prot |= PROT_EXEC;
                  mprotect((void *)pgaddr, pglen, prot); }
            }
        }
    }
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: patched %d TLV descriptors in %d blocks (accessor %p)\n",
                patched, blocks, (void *)(uintptr_t)accessor);
}


/* dyld image enumeration for libSystem.B's add-image callback. */

/* Diagnostic: any pointer that still targets a stub/binder means a lazy
 * symbol was never bound; calling it lands in the fallback. */
static void check_lazy_stubs(void) {
    int total = 0;
    for (int i = 0; i < g_nimgs; i++) {
        struct image *im = g_imgs[i];
        uintptr_t lo[4] = {0,0,0,0}, hi[4] = {0,0,0,0};
        int nr = 0;
        for (int s = 0; s < im->nsegs && nr < 4; s++) {
            const struct segment_command_64 *seg = im->segs[s];
            const struct section_64 *sect = (const struct section_64 *)(seg + 1);
            for (uint32_t j = 0; j < seg->nsects; j++, sect++) {
                if (!strncmp(sect->sectname, "__stubs", 16) ||
                    !strncmp(sect->sectname, "__stub_helper", 16) ||
                    !strncmp(sect->sectname, "__auth_stubs", 16) ||
                    !strncmp(sect->sectname, "__auth_stub_helper", 16)) {
                    uintptr_t a = (uintptr_t)(sect->addr + im->slide);
                    lo[nr] = a; hi[nr] = a + sect->size; nr++;
                }
            }
        }
        if (!nr) continue;
        for (int s = 0; s < im->nsegs; s++) {
            const struct segment_command_64 *seg = im->segs[s];
            const struct section_64 *sect = (const struct section_64 *)(seg + 1);
            for (uint32_t j = 0; j < seg->nsects; j++, sect++) {
                const char *n = sect->sectname;
                if (!strstr(n, "got") && !strstr(n, "symbol_ptr") &&
                    strncmp(n, "__data", 16)) continue;
                uintptr_t base = (uintptr_t)(sect->addr + im->slide);
                for (size_t off = 0; off + 8 <= sect->size; off += 8) {
                    uintptr_t v = *(uintptr_t *)(base + off);
                    for (int r = 0; r < nr; r++) {
                        if (v < lo[r] || v >= hi[r]) continue;
                        const unsigned char *c = (const unsigned char *)v;
                        unsigned idx = 0; int have = 0;
                        if (c[0] == 0x6a) { idx = c[1]; have = 1; }
                        else if (c[0] == 0x68) { memcpy(&idx, c + 1, 4); have = 1; }
                        fprintf(stderr, "reef: unbound %s %s+%#zx -> idx %s%u\n",
                                im->path, n, off, have ? "" : "?", idx);
                        total++;
                        break;
                    }
                }
            }
        }
    }
    fprintf(stderr, "reef: unbound lazy slots: %d\n", total);
}

struct plsr_image_info {
    void *mh;
    void *path;
    intptr_t slide;
};
static struct plsr_image_info g_image_info[MAX_IMAGES];

/* dyld image lookup: libSystem's _dladdr fills Dl_info from slot 0x230, and
 * _backtrace_image_offsets scans the header from slot 0x238 for LC_UUID. */
void *plsr_image_header_for(const void *addr) {
    uintptr_t a = (uintptr_t)addr;
    for (int i = 0; i < g_nimgs; i++) {
        struct image *im = g_imgs[i];
        uintptr_t b = (uintptr_t)(im->base + im->slide);
        if (a >= b && a < b + im->span) return (void *)b;
    }
    return NULL;
}
int plsr_image_dl_info(const void *addr, void *out[4]) {
    uintptr_t a = (uintptr_t)addr;
    for (int i = 0; i < g_nimgs; i++) {
        struct image *im = g_imgs[i];
        uintptr_t b = (uintptr_t)(im->base + im->slide);
        if (a >= b && a < b + im->span) {
            out[0] = (void *)im->path;
            out[1] = (void *)b;
            out[2] = NULL;   /* sname: no symbol-table walk here */
            out[3] = NULL;
            return 1;
        }
    }
    return 0;
}
int plsr_dyld_image_count(void) {
    for (int i = 0; i < g_nimgs; i++) {
        g_image_info[i].mh = (void *)(uintptr_t)(g_imgs[i]->base + g_imgs[i]->slide);
        g_image_info[i].path = g_imgs[i]->path;
        g_image_info[i].slide = (intptr_t)g_imgs[i]->slide;
    }
    return g_nimgs;
}

void *plsr_dyld_image_info(int index) {
    if (index < 0 || index >= g_nimgs) return NULL;
    g_image_info[index].mh = (void *)(uintptr_t)(g_imgs[index]->base + g_imgs[index]->slide);
    g_image_info[index].path = g_imgs[index]->path;
    g_image_info[index].slide = (intptr_t)g_imgs[index]->slide;
    return &g_image_info[index];
}

/* Used by the host-ABI stubs to name the adapter that called them. */
const char *plsr_describe_addr(const void *addr) {
    static char buf[768];
    uintptr_t a = (uintptr_t)addr;
    for (int i = 0; i < g_nimgs; i++) {
        struct image *im = g_imgs[i];
        for (int j = 0; j < im->nsegs; j++) {
            const struct segment_command_64 *s = im->segs[j];
            if (!s || s->vmsize == 0 || !memcmp(s->segname, "__PAGEZERO", 11))
                continue;
            uintptr_t start = (uintptr_t)(s->vmaddr + im->slide);
            if (a >= start && a < start + (uintptr_t)s->vmsize) {
                const char *base = strrchr(im->path, '/');
                snprintf(buf, sizeof buf, "%s+0x%lx(%s)",
                         base ? base + 1 : im->path,
                         (unsigned long)(a - start), s->segname);
                return buf;
            }
        }
    }
    snprintf(buf, sizeof buf, "%p", addr);
    return buf;
}

static void print_frame_map(void *addr)
{
    FILE *f = fopen("/proc/self/maps", "r");
    char line[512];
    unsigned long lo, hi;
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%lx-%lx", &lo, &hi) == 2 &&
            (uintptr_t)addr >= lo && (uintptr_t)addr < hi) {
            dprintf(2, "    map: %s", line);
            break;
        }
    }
    fclose(f);
}

static void plsr_backtrace_alarm(int sig)
{
    void *frames[64];
    int n = backtrace(frames, 64);
    dprintf(2, "=== plsr-lite backtrace (%d frames, signal %d) ===\n", n, sig);
    for (int i = 0; i < n; i++) {
        Dl_info info;
        if (dladdr(frames[i], &info) && info.dli_fname) {
            dprintf(2, "#%02d %p %s + 0x%lx (%s)\n", i, frames[i],
                    info.dli_fname,
                    (unsigned long)((char *)frames[i] - (char *)info.dli_fbase),
                    info.dli_sname ? info.dli_sname : "?");
        } else {
            dprintf(2, "#%02d %p\n", i, frames[i]);
            print_frame_map(frames[i]);
        }
    }
    _exit(128 + sig);
}

static int prot_of(uint32_t p) {
    int r = 0;
    if (p & 1) r |= PROT_READ;
    if (p & 2) r |= PROT_WRITE;
    if (p & 4) r |= PROT_EXEC;
    return r;
}

/* ---- Mach-O primitives ---- */
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t le16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static int rd_uleb(const uint8_t **pp, const uint8_t *end, uint64_t *out) {
    const uint8_t *p = *pp;
    uint64_t v = 0; unsigned sh = 0;
    while (p < end) {
        uint8_t b = *p++;
        if (sh < 64) v |= (uint64_t)(b & 0x7f) << sh;
        sh += 7;
        if (!(b & 0x80)) { *pp = p; *out = v; return 0; }
        if (sh > 63) break;
    }
    return -1;
}
static int rd_sleb(const uint8_t **pp, const uint8_t *end, int64_t *out) {
    const uint8_t *p = *pp;
    uint64_t v = 0; unsigned sh = 0; uint8_t b = 0;
    do {
        if (p >= end) return -1;
        b = *p++;
        if (sh < 64) v |= (uint64_t)(b & 0x7f) << sh;
        sh += 7;
    } while ((b & 0x80) && sh < 64);
    if (sh < 64 && (b & 0x40)) v |= ~(uint64_t)0 << sh;
    *pp = p; *out = (int64_t)v;
    return 0;
}

/* ---- exports ---- */
static void add_export(struct image *im, const char *name, uint64_t addr,
                       int reexp_ord, const char *reexp_name) {
    if (!name || !*name) return;
    if (im->nexports == im->expcap) {
        im->expcap = im->expcap ? im->expcap * 2 : 64;
        im->exports = realloc(im->exports, (size_t)im->expcap * sizeof(*im->exports));
        if (!im->exports) die("out of memory");
    }
    struct export_ent *e = &im->exports[im->nexports++];
    e->name = xstrdup(name);
    e->addr = addr;
    e->reexp_ord = reexp_ord;
    e->reexp_name = reexp_name ? xstrdup(reexp_name) : NULL;
}
static uint64_t name_hash(const char *s) {
    uint64_t h = 1469598103934665603ULL;
    while (*s) { h ^= (uint8_t)*s++; h *= 1099511628211ULL; }
    return h;
}
static void build_exphash(struct image *im) {
    size_t cap = 16;
    while (cap < (size_t)im->nexports * 2 + 1) cap <<= 1;
    im->exphash = xmalloc(cap * sizeof(*im->exphash));
    im->exphash_cap = cap;
    for (uint32_t i = 0; i < im->nexports; i++) {
        size_t h = (size_t)name_hash(im->exports[i].name) & (cap - 1);
        while (im->exphash[h]) h = (h + 1) & (cap - 1);
        im->exphash[h] = i + 1;
    }
}
static const struct export_ent *img_lookup(struct image *im, const char *name) {
    if (!im || !im->exphash_cap) return NULL;
    size_t cap = im->exphash_cap;
    size_t h = (size_t)name_hash(name) & (cap - 1);
    for (size_t probes = 0; probes <= cap; probes++) {
        uint32_t idx = im->exphash[h];
        if (!idx) return NULL;
        const struct export_ent *e = &im->exports[idx - 1];
        if (!strcmp(e->name, name)) return e;
        h = (h + 1) & (cap - 1);
    }
    return NULL;
}

struct trie_ctx { struct image *im; const uint8_t *base; size_t len; int nodes; };

static int trie_walk(struct trie_ctx *c, uint32_t off, const char *prefix, int depth) {
    if (depth > 64 || ++c->nodes > 400000 || off >= c->len) return -1;
    const uint8_t *p = c->base + off, *end = c->base + c->len;
    uint64_t term;
    if (rd_uleb(&p, end, &term)) return -1;
    const uint8_t *q = p + term;
    if (q > end) return -1;
    if (term) {
        uint64_t flags;
        if (rd_uleb(&p, q, &flags)) return -1;
        if (flags & 0x08) {                 /* EXPORT_SYMBOL_FLAGS_REEXPORT */
            uint64_t ord;
            if (rd_uleb(&p, q, &ord)) return -1;
            const char *rn = NULL;
            if (p < q) {
                const uint8_t *z = memchr(p, 0, (size_t)(q - p));
                if (!z) return -1;
                rn = (const char *)p;
                p = z + 1;
            }
            add_export(c->im, prefix, 0, (int)ord, rn);
        } else {
            uint64_t addr;
            if (rd_uleb(&p, q, &addr)) return -1;
            if (flags & 0x10) {              /* STUB_AND_RESOLVER: skip resolver */
                uint64_t r;
                if (rd_uleb(&p, q, &r)) return -1;
            }
            add_export(c->im, prefix, addr, 0, NULL);
        }
    }
    p = q;
    if (p >= end) return 0;
    unsigned nchild = *p++;
    char buf[1024];
    size_t plen = strlen(prefix);
    for (unsigned i = 0; i < nchild; i++) {
        if (p >= end) return -1;
        const char *edge = (const char *)p;
        const uint8_t *z = memchr(p, 0, (size_t)(end - p));
        if (!z) return -1;
        size_t elen = (size_t)(z - p);
        p = z + 1;
        uint64_t child;
        if (rd_uleb(&p, end, &child)) return -1;
        if (plen + elen + 1 > sizeof buf) return -1;
        memcpy(buf, prefix, plen);
        memcpy(buf + plen, edge, elen);
        buf[plen + elen] = 0;
        if (trie_walk(c, (uint32_t)child, buf, depth + 1)) return -1;
    }
    return 0;
}

static int load_export_trie(struct image *im) {
    struct trie_ctx ctx = { im, im->slice + im->exp_off, im->exp_size, 0 };
    return trie_walk(&ctx, 0, "", 0);
}
static int load_export_symtab(struct image *im) {
    if (!im->nsyms || !im->strsize) return 0;
    if ((uint64_t)im->symoff + (uint64_t)im->nsyms * 16 > im->slen) return -1;
    if ((uint64_t)im->stroff + im->strsize > im->slen) return -1;
    const uint8_t *sym = im->slice + im->symoff;
    for (uint32_t i = 0; i < im->nsyms; i++, sym += 16) {
        uint32_t strx = le32(sym);
        uint8_t type = sym[4];
        uint64_t value = (uint64_t)le32(sym + 8) | ((uint64_t)le32(sym + 12) << 32);
        if (type & 0xe0) continue;                    /* N_STAB */
        if (!(type & 0x01)) continue;                 /* N_EXT */
        if ((type & 0x0e) != 0x0e || value == 0) continue; /* N_SECT */
        if (strx >= im->strsize) continue;
        const char *name = (const char *)im->slice + im->stroff + strx;
        if (!memchr(name, 0, im->strsize - strx)) continue;
        add_export(im, name, value, 0, NULL);
    }
    return 0;
}
static int load_exports(struct image *im) {
    if (im->exp_size) {
        if ((uint64_t)im->exp_off + im->exp_size <= im->slen) {
            if (load_export_trie(im) != 0) {
                fprintf(stderr, "plsr-lite: %s: malformed export trie\n", im->path);
                if (im->nsyms) load_export_symtab(im);
            }
        }
    } else if (im->nsyms) {
        load_export_symtab(im);
    }
    build_exphash(im);
    return 0;
}

/* Return the unslid vmaddr of `name` in `im`, following reexports. */
static uint64_t lookup_in_image(struct image *im, const char *name,
                                struct image **provider, int *found, int depth) {
    if (!im || depth > 16) return 0;
    const struct export_ent *e = img_lookup(im, name);
    if (!e) return 0;
    if (e->reexp_ord) {
        struct image *t = NULL;
        if (e->reexp_ord > 0 && e->reexp_ord <= im->ndeps) t = im->dep[e->reexp_ord - 1];
        else if (e->reexp_ord == -1) t = g_main;
        if (!t) return 0;
        return lookup_in_image(t, e->reexp_name ? e->reexp_name : name,
                               provider, found, depth + 1);
    }
    *provider = im; *found = 1;
    return e->addr;
}

/* ---- Swift image registration ----
 *
 * dyld registers each Swift image's type, protocol, and conformance records
 * with libswiftCore.  A manually mapped image graph has to do the same, or
 * lazy metadata accessors call swift_getTypeByMangledNameInContext and get
 * null back, which then reaches _swift_getWitnessTable as a null metadata
 * argument.  The runtime entry points take [begin, end) record ranges. */
typedef void (*swift_register_records_fn)(const void *, const void *);

static const struct section_64 *find_section(struct image *im,
                                             const char *segname,
                                             const char *sectname,
                                             size_t *size) {
    for (int i = 0; i < im->nsegs; i++) {
        const struct segment_command_64 *s = im->segs[i];
        if (strncmp(s->segname, segname, sizeof(s->segname)) != 0) continue;
        const struct section_64 *sect = (const struct section_64 *)(s + 1);
        for (uint32_t j = 0; j < s->nsects; j++, sect++) {
            if (strncmp(sect->sectname, sectname, sizeof(sect->sectname)) != 0)
                continue;
            *size = (size_t)sect->size;
            return sect;
        }
    }
    return NULL;
}

static uint64_t swift_runtime_symbol(const char *name) {
    struct image *prov = NULL;
    int found = 0;
    uint64_t addr = lookup_in_image(g_main, name, &prov, &found, 0);
    if (!found) {
        for (int i = 0; i < g_nimgs; i++) {
            addr = lookup_in_image(g_imgs[i], name, &prov, &found, 0);
            if (found) break;
        }
    }
    return found ? prov->slide + addr : 0;
}

static void register_swift_image(struct image *im,
                                 swift_register_records_fn types,
                                 swift_register_records_fn protocols,
                                 swift_register_records_fn conformances) {
    size_t size = 0;
    const struct section_64 *sect;

    if (protocols && (sect = find_section(im, "__TEXT", "__swift5_protos", &size)) && size >= 4) {
        void *begin = (void *)(uintptr_t)(im->slide + sect->addr);
        if (getenv("PLSR_SWIFT_REG_TRACE"))
            fprintf(stderr, "swift-reg: %s protocols begin=%p end=%p (%zu bytes)\n",
                    im->path, begin, (char *)begin + size, size);
        protocols(begin, (char *)begin + size);
    }
    if (conformances && (sect = find_section(im, "__TEXT", "__swift5_proto", &size)) && size >= 4) {
        void *begin = (void *)(uintptr_t)(im->slide + sect->addr);
        if (getenv("PLSR_SWIFT_REG_TRACE"))
            fprintf(stderr, "swift-reg: %s conformances begin=%p end=%p (%zu bytes)\n",
                    im->path, begin, (char *)begin + size, size);
        conformances(begin, (char *)begin + size);
    }
    if (types && (sect = find_section(im, "__TEXT", "__swift5_types", &size)) && size >= 4) {
        void *begin = (void *)(uintptr_t)(im->slide + sect->addr);
        if (getenv("PLSR_SWIFT_REG_TRACE"))
            fprintf(stderr, "swift-reg: %s types begin=%p end=%p (%zu bytes)\n",
                    im->path, begin, (char *)begin + size, size);
        types(begin, (char *)begin + size);
    }
}

/* Swift objects whose refcount moved to a side table can be released after
 * the side table has already been freed.  The runtime reconstructs the side
 * table address as `bits * 8`; a stale value then produces an unmapped or
 * misaligned address and faults at libswiftCore+0x3413eb.  The gate below
 * validates that reconstructed address and only then executes the original
 * 16-byte compare-and-swap.  Valid side-table releases keep their normal
 * decrement, so no extra retain is left behind. */
uintptr_t g_swift_3413f5;

int plsr_swift_sidetable_ok(uint64_t bits) {
    uintptr_t addr = (uintptr_t)bits * 8;
    if (addr < 0x10000) return 0;
    if (addr & 0xf) return 0;
    if (addr >= 0x0000800000000000ULL) return 0;
    unsigned char vec = 0;
    if (mincore((void *)(addr & ~(uintptr_t)0xfff), 0x1000, &vec) != 0)
        return 0;
    return 1;
}

__attribute__((naked)) static void plsr_swift_sidetable_gate(void) {
    __asm__ __volatile__(
        "push %rdi\n\t"
        "push %rsi\n\t"
        "push %rdx\n\t"
        "push %rcx\n\t"
        "push %r8\n\t"
        "push %r9\n\t"
        "push %r10\n\t"
        "push %r11\n\t"
        "mov %rsi, %rdi\n\t"
        "call plsr_swift_sidetable_ok\n\t"
        "test %al, %al\n\t"
        "pop %r11\n\t"
        "pop %r10\n\t"
        "pop %r9\n\t"
        "pop %r8\n\t"
        "pop %rcx\n\t"
        "pop %rdx\n\t"
        "pop %rsi\n\t"
        "pop %rdi\n\t"
        "jz 1f\n\t"
        "xor %eax, %eax\n\t"
        "xor %edx, %edx\n\t"
        "xor %ecx, %ecx\n\t"
        "xor %ebx, %ebx\n\t"
        "lock cmpxchg16b 0x10(,%rsi,8)\n\t"
        "mov g_swift_3413f5(%rip), %r10\n\t"
        "jmp *%r10\n\t"
        "1:\n\t"
        "xor %eax, %eax\n\t"
        "add $8, %rsp\n\t"
        "pop %rbx\n\t"
        "pop %rbp\n\t"
        "ret\n\t"
    );
}

/* libobjc's fast _objc_retain dereferences the object's isa before checking
 * whether the value is a real object.  A Swift String or other tagged value
 * that reaches a CF dictionary callback then faults at libobjc+0xb79.  The
 * guard returns tagged/unmapped values unchanged (retain) or ignores them
 * (release), and sends real objects through the runtime's own slow paths. */
static void *(*g_root_retain)(void *);
static void (*g_root_release)(void *);
uintptr_t g_msgSend_cont;

/* A mapped, aligned word is not necessarily an Objective-C object: Swift
 * native String values can look like heap pointers. Validate the decoded isa
 * and its superclass chain against libobjc's registered class table before
 * entering method lookup. */
static void **g_objc_classes;
static int g_objc_class_count;
static pthread_mutex_t g_objc_classes_lock = PTHREAD_MUTEX_INITIALIZER;
static int plsr_objc_class_index(uintptr_t candidate) {
    for (int i = 0; i < g_objc_class_count; i++)
        if ((uintptr_t)g_objc_classes[i] == candidate) return i;
    return -1;
}
static int plsr_refresh_objc_classes(void) {
    int (*getlist)(void **, int) =
        (int (*)(void **, int))plsr_image_symbol("_objc_getClassList");
    if (!getlist) return 0;
    int n = getlist(NULL, 0);
    if (n <= 0 || n > 131072) return 0;
    void **classes = malloc((size_t)n * 2 * sizeof(*classes));
    if (!classes) return 0;
    int got = getlist(classes, n);
    if (got < 0 || got > n) { free(classes); return 0; }
    int total = got;
    for (int i = 0; i < got; i++) {
        uintptr_t cls = (uintptr_t)classes[i];
        if (cls && plsr_valid_obj((void *)cls))
            classes[total++] = *(void **)cls; /* include the metaclass */
    }
    free(g_objc_classes);
    g_objc_classes = classes;
    g_objc_class_count = total;
    return 1;
}
static int plsr_objc_class_chain_valid(uintptr_t cls) {
    for (int depth = 0; cls && depth < 64; depth++) {
        if (plsr_objc_class_index(cls) < 0 || !plsr_valid_obj((void *)cls))
            return 0;
        uintptr_t bits = *(uintptr_t *)(cls + 0x18) & ~(uintptr_t)7;
        uintptr_t superclass = *(uintptr_t *)(cls + 0x8);
        if ((!bits && !superclass) || (bits && !plsr_valid_obj((void *)bits)))
            return 0;
        if (!superclass) return 1;
        cls = superclass;
    }
    return 0;
}
__attribute__((noinline, used)) int plsr_valid_objc_receiver(const void *obj) {
    if (!plsr_valid_obj(obj)) return 0;
    uintptr_t isa = *(const uintptr_t *)obj;
    isa = (isa & 1) ? (isa & 0x7ffffffffff8ULL) : isa;
    int found = 0;
    pthread_mutex_lock(&g_objc_classes_lock);
    found = plsr_objc_class_index(isa) >= 0;
    if (!found && plsr_refresh_objc_classes()) {
        found = plsr_objc_class_index(isa) >= 0;
    }
    /* Reject broken class/metaclass parent chains before _realizeClass can
     * follow them into an incomplete Swift class allocation. */
    if (found) found = plsr_objc_class_chain_valid(isa);
    /* Class objects are receivers too: validate their ordinary class chain
     * as well as the metaclass stored in their isa word. */
    uintptr_t receiver = (uintptr_t)obj;
    if (found && plsr_objc_class_index(receiver) >= 0)
        found = plsr_objc_class_chain_valid(receiver);
    if (!found && plsr_objc_class_index(isa) < 0)
        found = plsr_valid_obj((const void *)isa);   /* class table unavailable */
    pthread_mutex_unlock(&g_objc_classes_lock);
    return found;
}
/* A corrupted Swift/ObjC bridge can pass a code pointer as SEL to the slow
 * resolver. Validate the selector name before libobjc dereferences it. */
__attribute__((noinline, used)) int plsr_valid_objc_selector(const void *selector) {
    uintptr_t p = (uintptr_t)selector;
    if (!p || p >= 0x0000800000000000ULL) return 0;
    size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
    if (!page_size || (page_size & (page_size - 1))) page_size = 4096;
    for (size_t i = 0; i < 128; i++) {
        uintptr_t at = p + i;
        if (at < p || at >= 0x0000800000000000ULL) return 0;
        if ((at & (page_size - 1)) == 0) {
            unsigned char vec = 0;
            if (mincore((void *)at, page_size, &vec) != 0) return 0;
        }
        unsigned char c = *(const unsigned char *)at;
        if (c == 0) return i > 0;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == ':' || c == '$'))
            return 0;
    }
    return 0;
}
struct plsr_exec_range { uintptr_t start, end; };
static struct plsr_exec_range g_exec_ranges[4096];
static size_t g_exec_range_count;
static pthread_once_t g_exec_ranges_once = PTHREAD_ONCE_INIT;
static void plsr_load_exec_ranges(void) {
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) return;
    char line[512], perms[8];
    unsigned long long start, end;
    while (fgets(line, sizeof(line), maps) &&
           g_exec_range_count < sizeof(g_exec_ranges) / sizeof(g_exec_ranges[0])) {
        if (sscanf(line, "%llx-%llx %7s", &start, &end, perms) != 3 ||
            !strchr(perms, 'x') || strchr(perms, 'w')) continue;
        g_exec_ranges[g_exec_range_count++] = (struct plsr_exec_range){
            (uintptr_t)start, (uintptr_t)end
        };
    }
    fclose(maps);
}
static int plsr_executable_pointer(const void *p) {
    uintptr_t v = (uintptr_t)p;
    pthread_once(&g_exec_ranges_once, plsr_load_exec_ranges);
    for (size_t i = 0; i < g_exec_range_count; i++)
        if (v >= g_exec_ranges[i].start && v < g_exec_ranges[i].end) return 1;
    return 0;
}
__attribute__((noinline, used)) void *plsr_resolve_objc_imp(void *receiver,
                                                            void *selector) {
    static unsigned diagnostics;
    int receiver_ok = plsr_valid_objc_receiver(receiver);
    int selector_ok = plsr_valid_objc_selector(selector);
    if (!receiver_ok || !selector_ok) {
        if (getenv("PLSR_REEF_TRACE") && diagnostics++ < 80)
            fprintf(stderr, "reef: msgSend reject receiver=%p valid=%d selector=%p valid=%d\n",
                    receiver, receiver_ok, selector, selector_ok);
        return NULL;
    }
    void *(*getclass)(void *) =
        (void *(*)(void *))plsr_image_symbol("_object_getClass");
    void *(*getmeth)(void *, void *) =
        (void *(*)(void *, void *))plsr_image_symbol("_class_getInstanceMethod");
    void *(*methimp)(void *) =
        (void *(*)(void *))plsr_image_symbol("_method_getImplementation");
    void *cls = getclass ? getclass(receiver) : NULL;
    void *meth = (cls && getmeth) ? getmeth(cls, selector) : NULL;
    void *imp = (meth && methimp) ? methimp(meth) : NULL;
    int executable = plsr_executable_pointer(imp);
    if (!executable && getenv("PLSR_REEF_TRACE") && diagnostics++ < 80)
        fprintf(stderr, "reef: msgSend unresolved receiver=%p cls=%p selector=%s getclass=%p getimp=%p imp=%p\n",
                receiver, cls, (char *)selector, getclass, methimp, imp);
    return executable ? imp : NULL;
}
uintptr_t plsr_msg_send_gate(const void *obj, const void *selector);

__attribute__((noinline, used)) int plsr_valid_objc_class_pointer(const void *ptr) {
    /* Pure check: the class table path calls libobjc, which re-enters the
     * guard.  A mapped, aligned pointer is enough to let initialization run. */
    uintptr_t cls = (uintptr_t)ptr;
    if (!cls || (cls & 7) || !plsr_valid_obj(ptr)) return 0;
    return 1;
}
static void *plsr_autorelease_return_safe(void *obj) {
    if (!plsr_valid_obj(obj)) return obj;
    return g_root_retain ? g_root_retain(obj) : obj;
}

static void plsr_swift_unknown_release_guard(void *obj) {
    if (!obj || ((uintptr_t)obj & 1)) return;
    if (!plsr_valid_obj(obj)) return;
    void (*fn)(void *) = (void (*)(void *))plsr_image_symbol("_objc_release");
    if (fn) fn(obj);
}

static bool plsr_swift_is_uniquely_referenced_guard(void *obj) {
    if (!obj || ((uintptr_t)obj & 1)) return false;
    if (!plsr_valid_obj(obj)) return false;
    /* Read refcount word; if it looks like a sidetable pointer or garbage,
     * report not-uniquely-referenced so Swift takes the slow/copy path
     * instead of dereferencing invalid memory. */
    uint64_t rc = *(uint64_t *)((char *)obj + 8);
    if ((int64_t)rc < 0) return false;
    if (rc & 0x2) return false; /* has sidetable */
    uint32_t strong = (uint32_t)(rc >> 33);
    return strong <= 1;
}

/* Class and metaclass objects live in the loaded Mach-O images, whose ranges
 * are fixed for the life of the process.  Checking the isa against those
 * ranges needs no libobjc, no locks and no syscalls, and rejects both a
 * non-pointer isa (0x6a806de3cb416cd8) and a heap pointer used as a class. */
struct plsr_readable_range { uintptr_t start, end; };
static struct plsr_readable_range g_image_ranges[4096];
static size_t g_image_range_count;
static int plsr_range_cmp(const void *a, const void *b) {
    const struct plsr_readable_range *x = a, *y = b;
    if (x->start < y->start) return -1;
    if (x->start > y->start) return 1;
    return 0;
}
static void plsr_build_image_ranges(void) {
    g_image_range_count = 0;
    for (int i = 0; i < g_nimgs; i++) {
        struct image *im = g_imgs[i];
        for (int s = 0; s < im->nsegs; s++) {
            uintptr_t start = (uintptr_t)(im->segs[s]->vmaddr + im->slide);
            uintptr_t end = start + (uintptr_t)im->segs[s]->vmsize;
            if (end <= start || g_image_range_count >=
                    sizeof(g_image_ranges) / sizeof(g_image_ranges[0]))
                continue;
            g_image_ranges[g_image_range_count++] =
                (struct plsr_readable_range){ start, end };
        }
    }
    qsort(g_image_ranges, g_image_range_count, sizeof(g_image_ranges[0]),
          plsr_range_cmp);
}
static int plsr_image_pointer(uintptr_t v) {
    size_t lo = 0, hi = g_image_range_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (v < g_image_ranges[mid].start) hi = mid;
        else if (v >= g_image_ranges[mid].end) lo = mid + 1;
        else return 1;
    }
    return 0;
}
__attribute__((noinline, used)) uintptr_t plsr_msg_send_gate(const void *obj,
                                                            const void *selector) {
    if (!obj) return 0;
    if (selector && !plsr_image_pointer((uintptr_t)selector)) {
        static unsigned rejects;
        if (getenv("PLSR_REEF_TRACE") && rejects++ < 10)
            fprintf(stderr, "reef: gate reject selector=%p\n", selector);
        return 0;
    }
    uintptr_t isa = *(const uintptr_t *)obj;
    if (isa & 1) {
        /* libobjc masks the low tag bits off a tagged isa and uses the rest as
         * a class pointer.  A bare tagged value (isa == 1, as Swift small
         * strings and encoded values produce) masks to NULL, so resuming the
         * cache lookup dereferences NULL at +0xe5d2.  Only accept a tagged isa
         * that still names a mapped class; otherwise drop the send. */
        uintptr_t tagged = isa & ~(uintptr_t)7;
        if (!tagged || !plsr_image_pointer(tagged)) {
            static unsigned tagged_rejects;
            if (getenv("PLSR_REEF_TRACE") && tagged_rejects++ < 10)
                fprintf(stderr, "reef: gate reject tagged obj=%p isa=%p\n",
                        obj, (void *)isa);
            return 0;
        }
        return isa;
    }
    if ((isa & 7) || isa < 0x10000 || isa >= 0x0000800000000000ULL) return 0;
    if (!plsr_image_pointer(isa)) {
        static unsigned rejects;
        if (getenv("PLSR_REEF_TRACE") && rejects++ < 10)
            fprintf(stderr, "reef: gate reject obj=%p isa=%p (not an image class)\n",
                    obj, (void *)isa);
        return 0;
    }
    return isa;
}

/* Entered through a 12-byte jump at _objc_msgSend's entry.  The replaced
 * prologue is: test %rdi,%rdi; je <nil>; mov (%rdi),%r10.  We reproduce it
 * after a pure isa check and resume at +0xe5bc (the isa tag test), so libobjc
 * keeps doing all of its own work. */
__attribute__((naked)) static void *plsr_msgSend_guard(void) {
    __asm__ __volatile__(
        "test %rdi, %rdi\n\t"
        "jz 3f\n\t"
        "push %rax\n\t"
        "push %r11\n\t"
        "push %r10\n\t"
        "push %r9\n\t"
        "push %r8\n\t"
        "push %rcx\n\t"
        "push %rdx\n\t"
        "push %rsi\n\t"
        "push %rdi\n\t"
        "sub $128, %rsp\n\t"
        "movdqu %xmm0, 0(%rsp)\n\t"
        "movdqu %xmm1, 16(%rsp)\n\t"
        "movdqu %xmm2, 32(%rsp)\n\t"
        "movdqu %xmm3, 48(%rsp)\n\t"
        "movdqu %xmm4, 64(%rsp)\n\t"
        "movdqu %xmm5, 80(%rsp)\n\t"
        "movdqu %xmm6, 96(%rsp)\n\t"
        "movdqu %xmm7, 112(%rsp)\n\t"
        "call plsr_msg_send_gate\n\t"
        "test %rax, %rax\n\t"
        "jz 2f\n\t"
        "mov %rax, 176(%rsp)\n\t"
        "movdqu 0(%rsp), %xmm0\n\t"
        "movdqu 16(%rsp), %xmm1\n\t"
        "movdqu 32(%rsp), %xmm2\n\t"
        "movdqu 48(%rsp), %xmm3\n\t"
        "movdqu 64(%rsp), %xmm4\n\t"
        "movdqu 80(%rsp), %xmm5\n\t"
        "movdqu 96(%rsp), %xmm6\n\t"
        "movdqu 112(%rsp), %xmm7\n\t"
        "add $128, %rsp\n\t"
        "pop %rdi\n\t"
        "pop %rsi\n\t"
        "pop %rdx\n\t"
        "pop %rcx\n\t"
        "pop %r8\n\t"
        "pop %r9\n\t"
        "pop %r10\n\t"
        /* The 12 bytes replaced at the msgSend entry loaded the receiver's
         * isa into r10; the continuation at +0xe5bc reads it to index the
         * class cache.  Restore it here, or the resume dereferences NULL. */
        "test $1, %dil\n\t"
        "jnz 4f\n\t"
        "mov (%rdi), %r10\n\t"
        "jmp 5f\n\t"
        "4:\n\t"
        "mov %rdi, %r10\n\t"
        "5:\n\t"
        "pop %r11\n\t"
        "pop %rax\n\t"
        "jmp *g_msgSend_cont(%rip)\n\t"
        "2:\n\t"
        "movdqu 0(%rsp), %xmm0\n\t"
        "movdqu 16(%rsp), %xmm1\n\t"
        "movdqu 32(%rsp), %xmm2\n\t"
        "movdqu 48(%rsp), %xmm3\n\t"
        "movdqu 64(%rsp), %xmm4\n\t"
        "movdqu 80(%rsp), %xmm5\n\t"
        "movdqu 96(%rsp), %xmm6\n\t"
        "movdqu 112(%rsp), %xmm7\n\t"
        "add $128, %rsp\n\t"
        "pop %rdi\n\t"
        "pop %rsi\n\t"
        "pop %rdx\n\t"
        "pop %rcx\n\t"
        "pop %r8\n\t"
        "pop %r9\n\t"
        "pop %r10\n\t"
        "pop %r11\n\t"
        "pop %rax\n\t"
        "3:\n\t"
        "xor %eax, %eax\n\t"
        "ret\n\t"
    );
}

uintptr_t g_initialize_class_cont;
__attribute__((naked)) static void plsr_initialize_class_guard(void) {
    __asm__ __volatile__(
        "push %rdi\n\t"
        "call plsr_valid_objc_class_pointer\n\t"
        "pop %rdi\n\t"
        "test %eax, %eax\n\t"
        "jz 1f\n\t"
        "mov g_initialize_class_cont(%rip), %rax\n\t"
        "jmp *%rax\n\t"
        "1:\n\t"
        "xor %eax, %eax\n\t"
        "ret\n\t"
    );
}

uintptr_t g_realize_class_cont;
uintptr_t g_realize_class_invalid_cont;
__attribute__((naked)) static void plsr_realize_class_gate(void) {
    __asm__ __volatile__(
        "push %rax\n\t" "push %r11\n\t" "push %r10\n\t"
        "push %r9\n\t" "push %r8\n\t" "push %rcx\n\t"
        "push %rdx\n\t" "push %rsi\n\t" "push %rdi\n\t"
        "sub $136, %rsp\n\t"
        "movdqu %xmm0, 8(%rsp)\n\t" "movdqu %xmm1, 24(%rsp)\n\t"
        "movdqu %xmm2, 40(%rsp)\n\t" "movdqu %xmm3, 56(%rsp)\n\t"
        "movdqu %xmm4, 72(%rsp)\n\t" "movdqu %xmm5, 88(%rsp)\n\t"
        "movdqu %xmm6, 104(%rsp)\n\t" "movdqu %xmm7, 120(%rsp)\n\t"
        "call plsr_valid_objc_class_pointer\n\t"
        "test %eax, %eax\n\t"
        "movdqu 8(%rsp), %xmm0\n\t" "movdqu 24(%rsp), %xmm1\n\t"
        "movdqu 40(%rsp), %xmm2\n\t" "movdqu 56(%rsp), %xmm3\n\t"
        "movdqu 72(%rsp), %xmm4\n\t" "movdqu 88(%rsp), %xmm5\n\t"
        "movdqu 104(%rsp), %xmm6\n\t" "movdqu 120(%rsp), %xmm7\n\t"
        "add $136, %rsp\n\t"
        "pop %rdi\n\t" "pop %rsi\n\t" "pop %rdx\n\t"
        "pop %rcx\n\t" "pop %r8\n\t" "pop %r9\n\t"
        "pop %r10\n\t" "pop %r11\n\t" "pop %rax\n\t"
        "jz 1f\n\t"
        "jmp *g_realize_class_cont(%rip)\n\t"
        "1:\n\t"
        "xor %eax, %eax\n\t"
        "jmp *g_realize_class_invalid_cont(%rip)\n\t"
    );
}

uintptr_t g_cf_key_cont;

/* CoreFoundation's key-retain callback dereferences the key before checking
 * whether it is a real object.  A raw Swift String in a Foundation-owned
 * dictionary reaches this callback as an unaligned pointer and faults at
 * CoreFoundation+0x802bf.  Reject null/unaligned keys here; valid keys resume
 * the original callback at its `mov (%rsi),%rax` instruction. */
__attribute__((naked)) static void *plsr_cf_key_guard(void) {
    __asm__ __volatile__(
        "test %rsi, %rsi\n\t"
        "jz 1f\n\t"
        "test $7, %sil\n\t"
        "jnz 1f\n\t"
        "mov g_cf_key_cont(%rip), %rax\n\t"
        "jmp *%rax\n\t"
        "1:\n\t"
        "xor %eax, %eax\n\t"
        "ret\n\t"
    );
}

static void *plsr_objc_retain_guard(void *obj) {
    if (!plsr_valid_obj(obj)) return obj;
    return g_root_retain(obj);
}
static void plsr_objc_release_guard(void *obj) {
    if (!plsr_valid_obj(obj)) return;
    g_root_release(obj);
}

/* libobjc retain/release/autorelease crash on objects with garbage or
 * Swift-only isa pointers.  The lightweight retain/release/autorelease guards
 * are safe and prevent hard crashes; the full msgSend gate is opt-in because
 * it changes dispatch semantics and can stall Swift/AppKit startup. */
static int objc_object_guard_enabled(void) {
    return getenv("PLSR_OBJC_OBJECT_GUARD") != NULL ||
           getenv("PLSR_REAL_DOCUMENT") != NULL;
}
static int objc_refcount_guard_enabled(void) {
    if (getenv("PLSR_NO_OBJC_REFCOUNT_GUARD") != NULL) return 0;
    return objc_object_guard_enabled() || getenv("PLSR_NEW_DOCUMENT") != NULL ||
           getenv("PLSR_OBJC_REFCOUNT_GUARD") != NULL;
}

static void patch_objc_object_guard(void) {
    int refcount = objc_refcount_guard_enabled();
    int full = objc_object_guard_enabled();
    if (!refcount && !full) return;
    for (int i = 0; i < g_nimgs; i++) {
        struct image *im = g_imgs[i];
        if (!strstr(im->path, "libobjc.A.dylib")) continue;
        uintptr_t base = im->base + im->slide;
        g_root_retain = (void *(*)(void *))(base + 0xb4d0);
        g_root_release = (void (*)(void *))(base + 0xb880);
        unsigned char *retain = (unsigned char *)(base + 0xb70);
        unsigned char *release = (unsigned char *)(base + 0xbc0);
        unsigned char *autorelease = (unsigned char *)(base + 0xc10);
        unsigned char *msgsend = (unsigned char *)(base + 0xe5b0);
        if (retain[0] != 0x55 || retain[1] != 0x48 ||
            release[0] != 0x55 || release[1] != 0x48) {
            if (getenv("PLSR_REEF_TRACE"))
                fprintf(stderr, "reef: objc guard site mismatch\n");
            continue;
        }
        long pg = sysconf(_SC_PAGESIZE);
        if (pg <= 0) pg = 4096;
        unsigned char code[12] = { 0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0 };
        plsr_build_image_ranges();
        if (refcount) {
            uintptr_t page = (uintptr_t)retain & ~(uintptr_t)(pg - 1);
            if (mprotect((void *)page, (size_t)pg,
                         PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
                uint64_t t = (uint64_t)(uintptr_t)plsr_objc_retain_guard;
                memcpy(code + 2, &t, 8);
                memcpy(retain, code, sizeof code);
                t = (uint64_t)(uintptr_t)plsr_objc_release_guard;
                memcpy(code + 2, &t, 8);
                memcpy(release, code, sizeof code);
                __builtin___clear_cache((char *)retain, (char *)retain + sizeof code);
                __builtin___clear_cache((char *)release, (char *)release + sizeof code);
                mprotect((void *)page, (size_t)pg, PROT_READ | PROT_EXEC);
                if (getenv("PLSR_REEF_TRACE"))
                    fprintf(stderr, "reef: objc retain/release guards installed\n");
            }
            if (autorelease[0] == 0x55 && autorelease[1] == 0x48) {
                uintptr_t ap = (uintptr_t)autorelease & ~(uintptr_t)(pg - 1);
                if (mprotect((void *)ap, (size_t)pg,
                             PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
                    uint64_t t = (uint64_t)(uintptr_t)plsr_objc_retain_guard;
                    memcpy(code + 2, &t, 8);
                    memcpy(autorelease, code, sizeof code);
                    __builtin___clear_cache((char *)autorelease,
                                            (char *)autorelease + sizeof code);
                    mprotect((void *)ap, (size_t)pg, PROT_READ | PROT_EXEC);
                    if (getenv("PLSR_REEF_TRACE"))
                        fprintf(stderr, "reef: objc autorelease guard installed\n");
                }
            }
            unsigned char code12[12] = { 0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0 };
            struct { unsigned off; const char *name; } arv_sites[] = {
                { 0xe50, "retainAutoreleaseReturnValue" },
                { 0x1010, "retainAutoreleasedReturnValue" },
                { 0x1110, "claimAutoreleasedReturnValue" },
            };
            for (size_t ai = 0; ai < sizeof arv_sites / sizeof arv_sites[0]; ai++) {
                unsigned char *arv = (unsigned char *)(base + arv_sites[ai].off);
                if (arv[0] != 0x55 || arv[1] != 0x48 || arv[2] != 0x89 ||
                    arv[3] != 0xe5 || arv[4] != 0x53 || arv[5] != 0x50)
                    continue;
                uintptr_t apage = (uintptr_t)arv & ~(uintptr_t)(pg - 1);
                if (mprotect((void *)apage, (size_t)pg,
                             PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
                    continue;
                uint64_t t = (uint64_t)(uintptr_t)plsr_autorelease_return_safe;
                memcpy(code12 + 2, &t, 8);
                memcpy(arv, code12, sizeof code12);
                __builtin___clear_cache((char *)arv, (char *)arv + sizeof code12);
                mprotect((void *)apage, (size_t)pg, PROT_READ | PROT_EXEC);
                if (getenv("PLSR_REEF_TRACE"))
                    fprintf(stderr, "reef: safe %s path installed\n", arv_sites[ai].name);
            }
            /* Swift runtime guards for invalid objects seen during KVO/Combine. */
            for (int j = 0; j < g_nimgs; j++) {
                struct image *sc = g_imgs[j];
                if (!strstr(sc->path, "libswiftCore.dylib")) continue;
                uintptr_t sb = sc->base + sc->slide;
                struct { unsigned off; void *fn; const char *name; unsigned char p0; } guards[] = {
                    { 0x393490, (void *)plsr_swift_unknown_release_guard, "swift_unknownObjectRelease", 0x55 },
                    { 0x3949e0, (void *)plsr_swift_is_uniquely_referenced_guard, "swift_isUniquelyReferenced_nonNull_native", 0x48 },
                };
                for (size_t gi = 0; gi < sizeof guards / sizeof guards[0]; gi++) {
                    unsigned char *su = (unsigned char *)(sb + guards[gi].off);
                    if (su[0] != guards[gi].p0) continue;
                    uintptr_t sp = (uintptr_t)su & ~(uintptr_t)(pg - 1);
                    if (mprotect((void *)sp, (size_t)pg, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
                        continue;
                    uint64_t t = (uint64_t)(uintptr_t)guards[gi].fn;
                    memcpy(code12 + 2, &t, 8);
                    memcpy(su, code12, sizeof code12);
                    __builtin___clear_cache((char *)su, (char *)su + sizeof code12);
                    mprotect((void *)sp, (size_t)pg, PROT_READ | PROT_EXEC);
                    if (getenv("PLSR_REEF_TRACE"))
                        fprintf(stderr, "reef: %s guard installed\n", guards[gi].name);
                }
                break;
            }
        }
        if (!full) continue;
        if (msgsend[0] != 0x48 || msgsend[1] != 0x85) {
            if (getenv("PLSR_REEF_TRACE"))
                fprintf(stderr, "reef: objc msgSend guard site mismatch\n");
            continue;
        }
        /* The fast cache at +0xe5bc contains invalid buckets for Swift
         * classes under this loader and faults while dispatching class
         * methods.  Route guarded sends through libobjc's slow resolver,
         * which handles tagged isa values and resolves the method directly. */
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: msgSend gate image ranges=%zu\n",
                    g_image_range_count);
        g_msgSend_cont = base + 0xe5bc;
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: msgSend continuation=%p\n",
                    (void *)g_msgSend_cont);
        uintptr_t mpage = (uintptr_t)msgsend & ~(uintptr_t)(pg - 1);
        int mr = mprotect((void *)mpage, (size_t)pg,
                          PROT_READ | PROT_WRITE | PROT_EXEC);
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: msgSend mprotect=%d errno=%d before=%02x%02x%02x\n",
                    mr, errno, msgsend[0], msgsend[1], msgsend[2]);
        if (mr == 0) {
            uint64_t t = (uint64_t)(uintptr_t)plsr_msgSend_guard;
            memcpy(code + 2, &t, 8);
            memcpy(msgsend, code, sizeof code);
            __builtin___clear_cache((char *)msgsend, (char *)msgsend + sizeof code);
            mprotect((void *)mpage, (size_t)pg, PROT_READ | PROT_EXEC);
            if (getenv("PLSR_REEF_TRACE"))
                fprintf(stderr, "reef: msgSend guard installed after=%02x%02x%02x\n",
                        msgsend[0], msgsend[1], msgsend[2]);
        }

        unsigned char *initclass = (unsigned char *)(base + 0xab80);
        if (initclass[0] == 0x55 && initclass[1] == 0x48 &&
            initclass[2] == 0x89 && initclass[3] == 0xe5 &&
            initclass[10] == 0xe8) {
            unsigned char *tramp = mmap((void *)(base + 0x300000), (size_t)pg,
                PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (tramp != MAP_FAILED) {
                memcpy(tramp, initclass, 15);
                intptr_t call_disp = (intptr_t)(base + 0x2300) -
                                     (intptr_t)(uintptr_t)(tramp + 15);
                if (call_disp >= INT32_MIN && call_disp <= INT32_MAX) {
                    int32_t d = (int32_t)call_disp;
                    memcpy(tramp + 11, &d, sizeof d);
                    tramp[15] = 0x48; tramp[16] = 0xb8;
                    uint64_t resume = (uint64_t)(uintptr_t)(initclass + 15);
                    memcpy(tramp + 17, &resume, 8);
                    tramp[25] = 0xff; tramp[26] = 0xe0;
                    __builtin___clear_cache((char *)tramp, (char *)tramp + 27);
                    g_initialize_class_cont = (uintptr_t)tramp;
                    uintptr_t ipage = (uintptr_t)initclass & ~(uintptr_t)(pg - 1);
                    if (mprotect((void *)ipage, (size_t)pg,
                                 PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
                        uint64_t t = (uint64_t)(uintptr_t)plsr_initialize_class_guard;
                        memcpy(code + 2, &t, 8);
                        memcpy(initclass, code, sizeof code);
                        __builtin___clear_cache((char *)initclass,
                                                (char *)initclass + sizeof code);
                        mprotect((void *)ipage, (size_t)pg, PROT_READ | PROT_EXEC);
                        if (getenv("PLSR_REEF_TRACE"))
                            fprintf(stderr, "reef: Objective-C class-init guard installed\n");
                    }
                } else {
                    munmap(tramp, (size_t)pg);
                }
            }
        } else if (getenv("PLSR_REEF_TRACE")) {
            fprintf(stderr, "reef: class-init guard site mismatch\n");
        }
        unsigned char *realize_body = (unsigned char *)(base + 0x2316);
        if (realize_body[0] == 0x48 && realize_body[1] == 0x8b &&
            realize_body[2] == 0x47 && realize_body[3] == 0x18 &&
            realize_body[10] == 0x4c && realize_body[11] == 0x8d &&
            realize_body[12] == 0x3d) {
            unsigned char *tramp = mmap((void *)(base + 0x400000), (size_t)pg,
                PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (tramp != MAP_FAILED) {
                memcpy(tramp, realize_body, 20);
                intptr_t leac = (intptr_t)(base + 0x167a0) -
                                 (intptr_t)(uintptr_t)(tramp + 17);
                if (leac >= INT32_MIN && leac <= INT32_MAX) {
                    int32_t d = (int32_t)leac;
                    memcpy(tramp + 13, &d, sizeof d);
                    tramp[20] = 0x48; tramp[21] = 0xb8;
                    uint64_t resume = (uint64_t)(uintptr_t)(realize_body + 20);
                    memcpy(tramp + 22, &resume, 8);
                    tramp[30] = 0xff; tramp[31] = 0xe0;
                    __builtin___clear_cache((char *)tramp, (char *)tramp + 32);
                    g_realize_class_cont = (uintptr_t)tramp;
                    g_realize_class_invalid_cont = base + 0x23be;
                    uintptr_t rpage = (uintptr_t)realize_body & ~(uintptr_t)(pg - 1);
                    if (mprotect((void *)rpage, (size_t)pg,
                                 PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
                        uint64_t t = (uint64_t)(uintptr_t)plsr_realize_class_gate;
                        memcpy(code + 2, &t, 8);
                        memcpy(realize_body, code, sizeof code);
                        __builtin___clear_cache((char *)realize_body,
                                                (char *)realize_body + sizeof code);
                        mprotect((void *)rpage, (size_t)pg, PROT_READ | PROT_EXEC);
                        if (getenv("PLSR_REEF_TRACE"))
                            fprintf(stderr, "reef: Objective-C realize-class guard installed\n");
                    }
                } else {
                    munmap(tramp, (size_t)pg);
                }
            }
        } else if (getenv("PLSR_REEF_TRACE")) {
            fprintf(stderr, "reef: realize-class guard site mismatch\n");
        }
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: objc object/refcount guard pass complete\n");
    }
}

static void patch_cf_key_guard(void) {
    /* The delegate bootstrap reaches CF's key-retain callback with an unaligned
     * key (CoreFoundation+0x802bf) via -[NSDictionary allKeys]; install the
     * guard for that path even though the broad object guard is opt-in. */
    if (!objc_object_guard_enabled() && !getenv("PLSR_BOOTSTRAP_DELEGATE")) return;
    for (int i = 0; i < g_nimgs; i++) {
        struct image *im = g_imgs[i];
        if (!strstr(im->path, "CoreFoundation")) continue;
        uintptr_t base = im->base + im->slide;
        unsigned char *fn = (unsigned char *)(base + 0x802b0);
        if (fn[0] != 0x48 || fn[1] != 0x85 || fn[2] != 0xf6) {
            if (getenv("PLSR_REEF_TRACE"))
                fprintf(stderr, "reef: CF key guard site mismatch (%02x %02x %02x)\n",
                        fn[0], fn[1], fn[2]);
            continue;
        }
        g_cf_key_cont = base + 0x802bf;
        long pg = sysconf(_SC_PAGESIZE);
        if (pg <= 0) pg = 4096;
        uintptr_t page = (uintptr_t)fn & ~(uintptr_t)(pg - 1);
        if (mprotect((void *)page, (size_t)pg,
                     PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
            continue;
        unsigned char code[12] = { 0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0 };
        uint64_t t = (uint64_t)(uintptr_t)plsr_cf_key_guard;
        memcpy(code + 2, &t, 8);
        memcpy(fn, code, sizeof code);
        __builtin___clear_cache((char *)fn, (char *)fn + sizeof code);
        mprotect((void *)page, (size_t)pg, PROT_READ | PROT_EXEC);
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: CF key guard installed\n");
    }
}

static void patch_cf_collection_nil(void) {
    /* CotEditor's delegate path reaches CF's nil addObject: check, whose
     * failure path ends in `ud2` (SIGILL).  Install the skip for that path. */
    if (!objc_object_guard_enabled() && !getenv("PLSR_BOOTSTRAP_DELEGATE")) return;
    static const struct { uintptr_t off; const char *name; } sites[] = {
        { 0xe1186, "addObject:" },
        { 0xe11b6, "removeObject:" },
    };
    for (int i = 0; i < g_nimgs; i++) {
        struct image *im = g_imgs[i];
        if (!strstr(im->path, "CoreFoundation")) continue;
        uintptr_t base = im->base + im->slide;
        for (size_t s = 0; s < sizeof sites / sizeof sites[0]; s++) {
            unsigned char *fn = (unsigned char *)(base + sites[s].off);
            if (fn[0] != 0xe8) {
                if (getenv("PLSR_REEF_TRACE"))
                    fprintf(stderr, "reef: CF %s nil site mismatch (%02x)\n",
                            sites[s].name, fn[0]);
                continue;
            }
            long pg = sysconf(_SC_PAGESIZE);
            if (pg <= 0) pg = 4096;
            uintptr_t page = (uintptr_t)fn & ~(uintptr_t)(pg - 1);
            if (mprotect((void *)page, (size_t)pg,
                         PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
                continue;
            /* pop %rbp; ret; nops: skip the nil-object assertion call. */
            unsigned char code[5] = { 0x5d, 0xc3, 0x90, 0x90, 0x90 };
            memcpy(fn, code, sizeof code);
            __builtin___clear_cache((char *)fn, (char *)fn + sizeof code);
            mprotect((void *)page, (size_t)pg, PROT_READ | PROT_EXEC);
            if (getenv("PLSR_REEF_TRACE"))
                fprintf(stderr, "reef: CF %s nil guard installed\n", sites[s].name);
        }
    }
}

static void patch_cf_release_nil(void) {
    if (!objc_object_guard_enabled()) return;
    for (int i = 0; i < g_nimgs; i++) {
        struct image *im = g_imgs[i];
        if (!strstr(im->path, "CoreFoundation")) continue;
        uintptr_t base = im->base + im->slide;
        unsigned char *fn = (unsigned char *)(base + 0x803f9);
        if (fn[0] != 0x0f || fn[1] != 0x0b) {
            if (getenv("PLSR_REEF_TRACE"))
                fprintf(stderr, "reef: CF release nil site mismatch (%02x %02x)\n",
                        fn[0], fn[1]);
            continue;
        }
        long pg = sysconf(_SC_PAGESIZE);
        if (pg <= 0) pg = 4096;
        uintptr_t page = (uintptr_t)fn & ~(uintptr_t)(pg - 1);
        if (mprotect((void *)page, (size_t)pg,
                     PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
            continue;
        /* __CFTypeCollectionRelease(NULL) reaches ud2; make it pop/ret. */
        unsigned char code[2] = { 0x5d, 0xc3 };
        memcpy(fn, code, sizeof code);
        __builtin___clear_cache((char *)fn, (char *)fn + sizeof code);
        mprotect((void *)page, (size_t)pg, PROT_READ | PROT_EXEC);
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: CF release nil guard installed\n");
    }
}

/* A Swift class can reach _swift_deallocClassInstance with a retain count
 * that is still non-zero (a bridged Foundation object whose deinit created a
 * temporary self-reference).  The runtime treats that as fatal and aborts the
 * process.  The object is already being destroyed and the extra reference is
 * not observable after this point, so the compatibility runtime can skip the
 * abort and continue with the normal destruct/dealloc sequence. */
static void patch_swift_dealloc_count(void) {
    if (!getenv("PLSR_SWIFT_IGNORE_DEALLOC_COUNT") && !getenv("PLSR_NSWINDOW") &&
        !getenv("PLSR_BOOTSTRAP_DELEGATE")) return;
    for (int i = 0; i < g_nimgs; i++) {
        struct image *im = g_imgs[i];
        if (!strstr(im->path, "libswiftCore.dylib")) continue;
        unsigned char *fn =
            (unsigned char *)(uintptr_t)(im->base + im->slide + 0x340aca);
        if (fn[0] != 0x75 || fn[1] != 0x3d) {
            if (getenv("PLSR_REEF_TRACE"))
                fprintf(stderr, "reef: swift dealloc-count site mismatch (%02x %02x)\n",
                        fn[0], fn[1]);
            continue;
        }
        long pg = sysconf(_SC_PAGESIZE);
        if (pg <= 0) pg = 4096;
        uintptr_t page = (uintptr_t)fn & ~(uintptr_t)(pg - 1);
        if (mprotect((void *)page, (size_t)pg,
                     PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
            continue;
        fn[0] = 0x90;
        fn[1] = 0x90;
        __builtin___clear_cache((char *)fn, (char *)fn + 2);
        mprotect((void *)page, (size_t)pg, PROT_READ | PROT_EXEC);
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: swift dealloc-count abort disabled\n");
    }
}

static void patch_swift_sidetable(void) {
    if (!getenv("PLSR_SWIFT_SKIP_SIDETABLE") && !getenv("PLSR_NSWINDOW") &&
        !getenv("PLSR_BOOTSTRAP_DELEGATE")) return;
    for (int i = 0; i < g_nimgs; i++) {
        struct image *im = g_imgs[i];
        if (!strstr(im->path, "libswiftCore.dylib")) continue;
        unsigned char *js =
            (unsigned char *)(uintptr_t)(im->base + im->slide + 0x3413ad);
        unsigned char *fn =
            (unsigned char *)(uintptr_t)(im->base + im->slide + 0x3413e3);
        if (js[0] != 0x78 || js[1] != 0x34) {
            if (getenv("PLSR_REEF_TRACE"))
                fprintf(stderr, "reef: swift side-table patch skipped (%02x %02x)\n",
                        js[0], js[1]);
            continue;
        }
        if (!(fn[0] == 0x31 && fn[1] == 0xc0 && fn[2] == 0x31 && fn[3] == 0xd2)) {
            if (getenv("PLSR_REEF_TRACE"))
                fprintf(stderr, "reef: swift side-table site mismatch\n");
            continue;
        }
        g_swift_3413f5 = im->base + im->slide + 0x3413f5;
        long pg = sysconf(_SC_PAGESIZE);
        if (pg <= 0) pg = 4096;
        uintptr_t page = (uintptr_t)fn & ~(uintptr_t)(pg - 1);
        if (mprotect((void *)page, (size_t)pg,
                     PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
            continue;
        /* Replace the zeroing + cmpxchg16b with a jump to the validating
         * gate; the gate re-executes the same zeroing and cmpxchg. */
        unsigned char code[12] = { 0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0 };
        uint64_t target = (uint64_t)(uintptr_t)plsr_swift_sidetable_gate;
        memcpy(code + 2, &target, 8);
        memcpy(fn, code, sizeof code);
        __builtin___clear_cache((char *)fn, (char *)fn + sizeof code);
        mprotect((void *)page, (size_t)pg, PROT_READ | PROT_EXEC);
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: swift side-table validation gate installed\n");
    }
}

static void register_swift_images(void) {
    swift_register_records_fn types =
        (swift_register_records_fn)(uintptr_t)swift_runtime_symbol("_swift_registerTypeMetadataRecords");
    swift_register_records_fn protocols =
        (swift_register_records_fn)(uintptr_t)swift_runtime_symbol("_swift_registerProtocols");
    swift_register_records_fn conformances =
        (swift_register_records_fn)(uintptr_t)swift_runtime_symbol("_swift_registerProtocolConformances");
    if (!types && !protocols && !conformances) {
        if (getenv("PLSR_SWIFT_REG_TRACE"))
            fprintf(stderr, "swift-reg: libswiftCore registration symbols not found\n");
        return;
    }
    for (int i = 0; i < g_nimgs; i++)
        register_swift_image(g_imgs[i], types, protocols, conformances);
}

static int resolve_symbol(struct image *im, int ordinal, const char *name,
                          uint64_t *value) {
    if (!name) return -1;
    void *sh = shim_lookup(name);
    const char *rn = getenv("PLSR_RESOLVE_NAME");
    int rtrace = rn && strstr(name, rn);
    if (rtrace)
        fprintf(stderr, "resolve %s shim=%p\n", name, sh);
    if (sh) { *value = (uint64_t)sh; return 0; }

    struct image *prov = NULL;
    int found = 0;
    uint64_t addr = 0;
    if (ordinal > 0) {
        if (ordinal <= im->ndeps && im->dep[ordinal - 1])
            addr = lookup_in_image(im->dep[ordinal - 1], name, &prov, &found, 0);
    } else if (ordinal == 0) {
        addr = lookup_in_image(im, name, &prov, &found, 0);
    } else if (ordinal == -1) {
        addr = lookup_in_image(g_main, name, &prov, &found, 0);
    }
    if (!found) {                              /* flat namespace fallback */
        for (int i = 0; i < g_nimgs; i++) {
            addr = lookup_in_image(g_imgs[i], name, &prov, &found, 0);
            if (found) break;
        }
    }
    if (!found) { if (rtrace) fprintf(stderr, "resolve %s NOT FOUND\n", name); return -1; }
    *value = addr + prov->slide;
    if (rtrace)
        fprintf(stderr, "resolve %s image=%s value=0x%llx\n", name, prov->path,
                (unsigned long long)*value);
    return 0;
}

/* ---- chained fixups ---- */
struct chained_import {
    int ordinal, weak;
    uint32_t name_off;
    int64_t addend;
};
static int chained_imports(struct image *im, const struct chained_header *ch,
                           const uint8_t *blob, uint32_t idx,
                           struct chained_import *ci) {
    const uint8_t *base = blob + ch->imports_off;
    if (ch->imports_format == CHAINED_IMPORT) {
        if ((uint64_t)ch->imports_off + (uint64_t)(idx + 1) * 4 > im->cf_size) return -1;
        uint32_t raw = le32(base + (size_t)idx * 4);
        ci->ordinal = (int8_t)(raw & 0xff);
        ci->weak = (raw >> 8) & 1;
        ci->name_off = raw >> 9;
        ci->addend = 0;
        return 0;
    }
    if (ch->imports_format == CHAINED_IMPORT_ADDEND) {
        if ((uint64_t)ch->imports_off + (uint64_t)(idx + 1) * 8 > im->cf_size) return -1;
        uint32_t raw = le32(base + (size_t)idx * 8);
        ci->ordinal = (int8_t)(raw & 0xff);
        ci->weak = (raw >> 8) & 1;
        ci->name_off = raw >> 9;
        ci->addend = (int32_t)le32(base + (size_t)idx * 8 + 4);
        return 0;
    }
    if (ch->imports_format == CHAINED_IMPORT_ADDEND64) {
        if ((uint64_t)ch->imports_off + (uint64_t)(idx + 1) * 16 > im->cf_size) return -1;
        const uint8_t *e = base + (size_t)idx * 16;
        uint64_t head = (uint64_t)le32(e) | ((uint64_t)le32(e + 4) << 32);
        ci->ordinal = (int16_t)(head & 0xffff);
        ci->weak = (head >> 16) & 1;
        ci->name_off = (uint32_t)(head >> 32);
        ci->addend = (int64_t)((uint64_t)le32(e + 8) | ((uint64_t)le32(e + 12) << 32));
        return 0;
    }
    return -1;
}
static const char *chained_symname(struct image *im, const struct chained_header *ch,
                                   const uint8_t *blob, uint32_t off) {
    if (ch->symbols_off > im->cf_size) return NULL;
    if (ch->symbols_format == CHAINED_SYMBOL_UNCOMPRESSED) {
        if ((uint64_t)ch->symbols_off + off >= im->cf_size) return NULL;
        const char *s = (const char *)blob + ch->symbols_off + off;
        size_t max = im->cf_size - ch->symbols_off - off;
        if (!memchr(s, 0, max)) return NULL;
        return s;
    }
    if (ch->symbols_format == CHAINED_SYMBOL_COMPRESSED) {
        if (!im->sympool) {
            uLongf cap = 4096, len = 0;
            uint8_t *dst = NULL;
            int rc = Z_BUF_ERROR;
            while (cap <= 64u * 1024 * 1024) {
                dst = realloc(dst, cap);
                if (!dst) die("out of memory");
                len = cap;
                uLong src = im->cf_size - ch->symbols_off;
                rc = uncompress(dst, &len, blob + ch->symbols_off, src);
                if (rc == Z_OK) break;
                if (rc != Z_BUF_ERROR) break;
                cap *= 2;
            }
            if (rc != Z_OK) { free(dst); return NULL; }
            im->sympool = dst;
            im->sympool_len = len;
        }
        if (off >= im->sympool_len) return NULL;
        const char *s = (const char *)im->sympool + off;
        if (!memchr(s, 0, im->sympool_len - off)) return NULL;
        return s;
    }
    return NULL;
}
static int image_contains(struct image *im, uintptr_t addr, size_t n) {
    for (int i = 0; i < im->nsegs; i++) {
        const struct segment_command_64 *s = im->segs[i];
        if (s->vmsize == 0) continue;
        uintptr_t start = (uintptr_t)(s->vmaddr + im->slide);
        if (addr >= start && addr + n <= start + s->vmsize) return 1;
    }
    return 0;
}
static void bind_slot(struct image *im, uintptr_t loc, int ordinal,
                      const char *name, int64_t addend, int weak) {
    uint64_t value = 0;
    int ok = resolve_symbol(im, ordinal, name, &value);
    if (ok != 0) {
        if (weak) {
            g_weak_missing++;
            value = 0;
        } else if (g_stub_missing || g_load_mode) {
            g_unresolved++;
            if (g_trace_missing)
                fprintf(stderr, "MISSING %s\n", name ? name : "<null>");
            value = 0;
        } else {
            die("unresolved symbol %s (image %s)", name ? name : "<null>", im->path);
        }
    } else {
        g_bound++;
    }
    value += (uint64_t)addend;
    if (getenv("PLSR_REEF_TRACE") && name && strstr(name, "reef_host"))
        dprintf(2, "reef: bind %s loc=%p value=%p image=%s\n",
                name, (void *)loc, (void *)(uintptr_t)value, im->path);
    memcpy((void *)loc, &value, 8);
}

static void apply_chained(struct image *im) {
    if ((uint64_t)im->cf_off + im->cf_size > im->slen) {
        fprintf(stderr, "appexec: %s: chained fixups outside file\n", im->path);
        g_errors++;
        return;
    }
    const uint8_t *blob = im->slice + im->cf_off;
    const struct chained_header *ch = (const struct chained_header *)blob;
    if (im->cf_size < sizeof(*ch)) { g_errors++; return; }
    if (ch->version != 0) {
        fprintf(stderr, "plsr-lite: %s: unsupported chained fixups version %u\n",
                im->path, ch->version);
        g_errors++;
        return;
    }
    /* Empty import/symbol tables sit exactly at the end of the blob. */
    if (ch->starts_off > im->cf_size || ch->imports_off > im->cf_size ||
        ch->symbols_off > im->cf_size) {
        fprintf(stderr, "plsr-lite: %s: chained fixups offsets outside blob\n", im->path);
        g_errors++;
        return;
    }
    if (ch->starts_off == im->cf_size) return;
    const uint8_t *starts = blob + ch->starts_off;
    if ((size_t)(starts - blob) + 4 > im->cf_size) { g_errors++; return; }
    uint32_t seg_count = le32(starts);
    if ((size_t)(starts - blob) + 4 + (size_t)seg_count * 4 > im->cf_size) { g_errors++; return; }
    for (uint32_t s = 0; s < seg_count; s++) {
        uint32_t so = le32(starts + 4 + (size_t)s * 4);
        if (!so) continue;
        if ((uint64_t)ch->starts_off + so + 24 > im->cf_size) { g_errors++; continue; }
        const uint8_t *cs = starts + so;
        uint16_t page_size = le16(cs + 4);
        uint16_t ptr_format = le16(cs + 6);
        uint64_t seg_off = (uint64_t)le32(cs + 8) | ((uint64_t)le32(cs + 12) << 32);
        uint16_t page_count = le16(cs + 20);
        if (ptr_format != CHAINED_PTR_64 && ptr_format != CHAINED_PTR_64_OFFSET) {
            fprintf(stderr, "plsr-lite: %s: unsupported chained pointer format %u\n",
                    im->path, ptr_format);
            g_errors++;
            continue;
        }
        if (!page_size) page_size = (uint16_t)PAGE;
        for (uint16_t pg = 0; pg < page_count; pg++) {
            if ((uint64_t)ch->starts_off + so + 22 + (uint64_t)pg * 2 + 2 > im->cf_size) break;
            uint16_t start = le16(cs + 22 + (size_t)pg * 2);
            if (start == CHAINED_PTR_START_NONE) continue;
            uintptr_t loc = (uintptr_t)(im->base + im->slide + seg_off) +
                            (uintptr_t)pg * page_size + start;
            for (;;) {
                if (!image_contains(im, loc, 8)) {
                    fprintf(stderr, "plsr-lite: %s: chained pointer outside image\n", im->path);
                    g_errors++;
                    break;
                }
                uint64_t raw;
                memcpy(&raw, (const void *)loc, 8);
                uint32_t next = (uint32_t)((raw >> 51) & 0xfff);
                if (raw >> 63) {
                    uint32_t ordinal = (uint32_t)(raw & 0xffffff);   /* import table index */
                    int8_t addend = (int8_t)((raw >> 24) & 0xff);
                    if (ordinal >= ch->imports_count) {
                        fprintf(stderr, "plsr-lite: %s: chained import ordinal out of range\n", im->path);
                        g_errors++;
                        break;
                    }
                    struct chained_import ci;
                    if (chained_imports(im, ch, blob, ordinal, &ci) != 0) { g_errors++; break; }
                    const char *name = chained_symname(im, ch, blob, ci.name_off);
                    bind_slot(im, loc, ci.ordinal, name, ci.addend + addend, ci.weak);
                } else {
                    uint64_t target = (raw & 0xfffffffffULL) | (((raw >> 36) & 0xff) << 56);
                    uint64_t value;
                    if (ptr_format == CHAINED_PTR_64_OFFSET)
                        value = target ? target + im->base + im->slide : 0;
                    else
                        value = target ? target + im->slide : 0;
                    memcpy((void *)loc, &value, 8);
                }
                if (!next) break;
                loc += (uintptr_t)next * 4;
            }
        }
    }
}

/* ---- classic dyld info opcodes ---- */
static void rebase_at(struct image *im, int seg, uint64_t off) {
    if (seg < 0 || seg >= im->nsegs) return;
    const struct segment_command_64 *s = im->segs[seg];
    if (off + 8 > s->vmsize) return;
    uint64_t *loc = (uint64_t *)(uintptr_t)(s->vmaddr + im->slide + off);
    *loc += im->slide;
}
static void apply_rebase(struct image *im) {
    if (!im->rebase_size) return;
    const uint8_t *p = im->slice + im->rebase_off;
    const uint8_t *end = p + im->rebase_size;
    int seg = -1; uint64_t o = 0;
    while (p < end) {
        uint8_t b = *p++;
        if (b == 0) break;
        uint8_t op = b & 0xf0, imm = b & 0x0f;
        if (op == 0x10) continue;                              /* SET_TYPE_IMM */
        else if (op == 0x20) { seg = imm; if (rd_uleb(&p, end, &o)) break; }
        else if (op == 0x30) { uint64_t v; if (rd_uleb(&p, end, &v)) break; o += v; }
        else if (op == 0x40) o += (uint64_t)imm * 8;
        else if (op == 0x50) { for (int i = 0; i < imm; i++) { rebase_at(im, seg, o); o += 8; } }
        else if (op == 0x60) {
            uint64_t n; if (rd_uleb(&p, end, &n)) break;
            for (uint64_t i = 0; i < n; i++) { rebase_at(im, seg, o); o += 8; }
        } else if (op == 0x70) {
            uint64_t v; rebase_at(im, seg, o); if (rd_uleb(&p, end, &v)) break; o += 8 + v;
        } else if (op == 0x80) {
            uint64_t n, skip;
            if (rd_uleb(&p, end, &n) || rd_uleb(&p, end, &skip)) break;
            for (uint64_t i = 0; i < n; i++) { rebase_at(im, seg, o); o += 8 + skip; }
        } else break;
    }
}
static void bind_at(struct image *im, int seg, uint64_t off, int ordinal,
                    const char *sym, int64_t addend, int weak) {
    if (seg < 0 || seg >= im->nsegs) return;
    const struct segment_command_64 *s = im->segs[seg];
    if (off + 8 > s->vmsize) return;
    uintptr_t loc = (uintptr_t)(s->vmaddr + im->slide + off);
    bind_slot(im, loc, ordinal, sym, addend, weak);
}
static void apply_bind_stream(struct image *im, uint32_t off, uint32_t size, int weak_stream) {
    if (!size) return;
    const uint8_t *p = im->slice + off;
    const uint8_t *end = p + size;
    int seg = -1, ordinal = 0, weak = weak_stream;
    uint64_t o = 0, addend = 0;
    const char *sym = NULL;
    while (p < end) {
        uint8_t b = *p++;
        if (b == 0) continue;                                  /* DONE */
        uint8_t op = b & 0xf0, imm = b & 0x0f;
        if (op == 0x10) ordinal = imm;                         /* SET_DYLIB_ORDINAL_IMM */
        else if (op == 0x20) { uint64_t v; if (rd_uleb(&p, end, &v)) break; ordinal = (int)v; }
        else if (op == 0x30) ordinal = imm ? (int8_t)(0xf0 | imm) : 0;
        else if (op == 0x40) {                                 /* SET_SYMBOL_TRAILING_FLAGS_IMM */
            const uint8_t *z = memchr(p, 0, (size_t)(end - p));
            if (!z) break;
            sym = (const char *)p;
            p = z + 1;
            weak = weak_stream || (imm & 1);
        } else if (op == 0x50) { /* SET_TYPE_IMM */ }
        else if (op == 0x60) { int64_t v; if (rd_sleb(&p, end, &v)) break; addend = (uint64_t)v; }
        else if (op == 0x70) { seg = imm; if (rd_uleb(&p, end, &o)) break; }
        else if (op == 0x80) { uint64_t v; if (rd_uleb(&p, end, &v)) break; o += v; }
        else if (op == 0x90) { bind_at(im, seg, o, ordinal, sym, (int64_t)addend, weak); o += 8; }
        else if (op == 0xa0) {
            uint64_t skip; bind_at(im, seg, o, ordinal, sym, (int64_t)addend, weak);
            if (rd_uleb(&p, end, &skip)) break;
            o += 8 + skip;
        } else if (op == 0xb0) {
            bind_at(im, seg, o, ordinal, sym, (int64_t)addend, weak);
            o += 8 + (uint64_t)imm * 8;
        } else if (op == 0xc0) {
            uint64_t n, skip;
            if (rd_uleb(&p, end, &n) || rd_uleb(&p, end, &skip)) break;
            for (uint64_t i = 0; i < n; i++) {
                bind_at(im, seg, o, ordinal, sym, (int64_t)addend, weak);
                o += 8 + skip;
            }
        } else break;                                          /* THREADED etc. */
    }
}
static void apply_fixups(struct image *im) {
    if (im->cf_size) apply_chained(im);
    if (im->rebase_size) apply_rebase(im);
    if (im->bind_size) apply_bind_stream(im, im->bind_off, im->bind_size, 0);
    if (im->lazy_size) apply_bind_stream(im, im->lazy_off, im->lazy_size, 0);
    if (im->weak_size) apply_bind_stream(im, im->weak_off, im->weak_size, 1);
}

/* ---- image parsing ---- */
static int parse_metadata(struct image *im) {
    const uint8_t *d = im->file;
    size_t size = im->fsize;
    const uint8_t *slice = d;
    size_t slen = size;
    const char *arch = "thin";

    if (size < 32) { set_err("%s: truncated file", im->path); return -1; }
    if (be32(d) == FAT_MAGIC || be32(d) == FAT_MAGIC_64) {
        int is64 = be32(d) == FAT_MAGIC_64;
        uint32_t n = be32(d + 4);
        uint32_t entsize = is64 ? 32 : 20;
        if (n == 0 || n > 64 || 8 + (uint64_t)n * entsize > size) {
            set_err("%s: bad fat header", im->path); return -1;
        }
        int found = 0;
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t *e = d + 8 + (size_t)i * entsize;
            uint32_t cpu = be32(e);
            uint64_t off = is64 ? ((uint64_t)be32(e + 8) | ((uint64_t)be32(e + 12) << 32))
                                : be32(e + 8);
            uint64_t len = is64 ? ((uint64_t)be32(e + 16) | ((uint64_t)be32(e + 20) << 32))
                                : be32(e + 12);
            if (cpu == CPU_TYPE_X86_64 && off + len <= size) {
                slice = d + off; slen = (size_t)len; found = 1;
                arch = "fat/x86_64"; im->fat = 1; break;
            }
        }
        if (!found) {
            set_err("%s has no slice for this processor: it has only non-x86_64", im->path);
            return -1;
        }
    }
    if (slen < 32) { set_err("%s: truncated file", im->path); return -1; }
    uint32_t magic = le32(slice);
    if (magic == MH_MAGIC_32) { set_err("%s is not a 64-bit Mach-O file", im->path); return -1; }
    if (magic != MH_MAGIC_64) { set_err("%s is not a Mach-O file", im->path); return -1; }
    const struct mach_header_64 *mh = (const struct mach_header_64 *)slice;
    /* --load/--info may target a dylib to inventory its closure; only an
       executing root must be MH_EXECUTE. */
    int inspect_dylib = im->is_main && (g_load_mode || g_info_mode) &&
                        mh->filetype == MH_DYLIB;
    if (im->is_main && mh->filetype != MH_EXECUTE && !inspect_dylib) {
        set_err("%s: unsupported Mach-O file type %u (need MH_EXECUTE)", im->path, mh->filetype);
        return -1;
    }
    if (!im->is_main && mh->filetype != MH_DYLIB && mh->filetype != MH_BUNDLE) {
        set_err("%s: unsupported Mach-O file type %u (need MH_DYLIB)", im->path, mh->filetype);
        return -1;
    }
    if ((uint64_t)mh->sizeofcmds + sizeof(*mh) > slen) {
        set_err("%s: truncated load commands", im->path); return -1;
    }
    im->slice = slice; im->slen = slen; im->mh = mh; im->arch = arch;

    uint64_t off = sizeof(*mh);
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        if (off + 8 > sizeof(*mh) + (uint64_t)mh->sizeofcmds) {
            set_err("%s: truncated load command", im->path); return -1;
        }
        const struct load_command *lc = (const struct load_command *)(slice + off);
        if (lc->cmdsize < 8 || off + lc->cmdsize > sizeof(*mh) + (uint64_t)mh->sizeofcmds) {
            set_err("%s: bad load command size", im->path); return -1;
        }
        switch (lc->cmd) {
        case LC_SEGMENT_64: {
            if (lc->cmdsize < sizeof(struct segment_command_64)) {
                set_err("%s: bad segment command", im->path); return -1;
            }
            if (im->nsegs < MAX_SEGS)
                im->segs[im->nsegs++] = (const struct segment_command_64 *)lc;
            break;
        }
        case LC_MAIN:
            im->ep = (const struct entry_point_command *)lc;
            break;
        case LC_UNIXTHREAD:
            im->tc = (const struct thread_command *)lc;
            break;
        case LC_DYLD_INFO:
        case LC_DYLD_INFO_ONLY: {
            const struct dyld_info_command *di = (const struct dyld_info_command *)lc;
            im->rebase_off = di->rebase_off; im->rebase_size = di->rebase_size;
            im->bind_off = di->bind_off; im->bind_size = di->bind_size;
            im->weak_off = di->weak_bind_off; im->weak_size = di->weak_bind_size;
            im->lazy_off = di->lazy_bind_off; im->lazy_size = di->lazy_bind_size;
            /* A separate LC_DYLD_EXPORTS_TRIE may carry the trie; do not
               let a zero-size dyld-info field erase it. */
            if (di->export_size) { im->exp_off = di->export_off; im->exp_size = di->export_size; }
            break;
        }
        case LC_DYLD_EXPORTS_TRIE: {
            const struct linkedit_data_command *ld = (const struct linkedit_data_command *)lc;
            im->exp_off = ld->dataoff; im->exp_size = ld->datasize;
            break;
        }
        case LC_DYLD_CHAINED_FIXUPS: {
            const struct chained_fixups_command *cf = (const struct chained_fixups_command *)lc;
            im->cf_off = cf->dataoff; im->cf_size = cf->datasize;
            break;
        }
        case LC_SYMTAB: {
            const struct symtab_command *st = (const struct symtab_command *)lc;
            im->symoff = st->symoff; im->nsyms = st->nsyms;
            im->stroff = st->stroff; im->strsize = st->strsize;
            break;
        }
        case LC_RPATH: {
            const struct rpath_command *rp = (const struct rpath_command *)lc;
            if (rp->path_off < lc->cmdsize && im->nrpaths < MAX_RPATHS) {
                const char *p = (const char *)lc + rp->path_off;
                size_t max = lc->cmdsize - rp->path_off;
                if (memchr(p, 0, max)) im->rpath[im->nrpaths++] = xstrdup(p);
            }
            break;
        }
        case LC_ID_DYLIB: {
            const struct dylib_command *dc = (const struct dylib_command *)lc;
            if (dc->name_off < lc->cmdsize) {
                const char *p = (const char *)lc + dc->name_off;
                if (memchr(p, 0, lc->cmdsize - dc->name_off)) im->install = xstrdup(p);
            }
            break;
        }
        case LC_LOAD_DYLIB:
        case LC_LOAD_WEAK_DYLIB:
        case LC_REEXPORT_DYLIB:
        case LC_LAZY_LOAD_DYLIB:
        case LC_LOAD_UPWARD_DYLIB: {
            const struct dylib_command *dc = (const struct dylib_command *)lc;
            if (dc->name_off < lc->cmdsize && im->ndeps < MAX_DYLIBS) {
                const char *p = (const char *)lc + dc->name_off;
                size_t max = lc->cmdsize - dc->name_off;
                if (memchr(p, 0, max)) {
                    im->depname[im->ndeps] = xstrdup(p);
                    im->depweak[im->ndeps] = (lc->cmd == LC_LOAD_WEAK_DYLIB);
                    im->ndeps++;
                }
            }
            break;
        }
        default: break;
        }
        off += lc->cmdsize;
    }
    if (im->nsegs == 0) { set_err("%s: no segments map the Mach-O header", im->path); return -1; }

    int have_text = 0;
    uint64_t base = UINT64_MAX, end = 0;
    for (int i = 0; i < im->nsegs; i++) {
        const struct segment_command_64 *s = im->segs[i];
        if (memcmp(s->segname, "__TEXT", 7) == 0) have_text = 1;
        if (memcmp(s->segname, "__PAGEZERO", 11) == 0) continue;
        if (s->vmaddr < base) base = s->vmaddr;
        if (s->vmaddr + s->vmsize > end) end = s->vmaddr + s->vmsize;
    }
    if (im->is_main && !have_text) { set_err("%s: no __TEXT segment", im->path); return -1; }
    if (base == UINT64_MAX || end <= base) { set_err("%s: empty image", im->path); return -1; }
    if (end - base > (uint64_t)SIZE_MAX) { set_err("%s: image too large", im->path); return -1; }
    im->base = base;
    im->span = ROUNDUP(end - base);

    const struct segment_command_64 *text = NULL;
    for (int i = 0; i < im->nsegs; i++)
        if (memcmp(im->segs[i]->segname, "__TEXT", 7) == 0) { text = im->segs[i]; break; }
    if (im->is_main && !inspect_dylib) {
        if (!im->ep && !im->tc) { set_err("%s: no LC_MAIN/LC_UNIXTHREAD entry point", im->path); return -1; }
        if (im->ep && text &&
            (im->ep->entryoff < text->fileoff ||
             im->ep->entryoff >= text->fileoff + text->filesize)) {
            set_err("%s: entry point outside __TEXT", im->path); return -1;
        }
        if (im->tc && (im->tc->flavor != X86_THREAD_STATE64 || im->tc->count != 42)) {
            set_err("%s: unsupported LC_UNIXTHREAD state", im->path); return -1;
        }
    }
    return 0;
}

static int map_image(struct image *im) {
    void *res = mmap(NULL, im->span, PROT_NONE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (res == MAP_FAILED) { set_err("%s: cannot reserve %zu bytes: %s", im->path, im->span, strerror(errno)); return -1; }
    im->slide = (uintptr_t)res - (uintptr_t)im->base;
    for (int i = 0; i < im->nsegs; i++) {
        const struct segment_command_64 *s = im->segs[i];
        if (s->vmsize == 0 || memcmp(s->segname, "__PAGEZERO", 11) == 0) continue;
        if (s->filesize > s->vmsize) { set_err("%s: %s filesize > vmsize", im->path, s->segname); return -1; }
        if (s->fileoff + s->filesize > im->slen) { set_err("%s: %s extends past end of file", im->path, s->segname); return -1; }
        size_t mlen = (size_t)ROUNDUP(s->vmsize);
        void *at = (void *)(uintptr_t)(s->vmaddr + im->slide);
        void *m = mmap(at, mlen, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (m == MAP_FAILED) { set_err("%s: cannot map %s: %s", im->path, s->segname, strerror(errno)); return -1; }
        if (s->filesize) memcpy(m, im->slice + s->fileoff, (size_t)s->filesize);
        im->prot[i] = prot_of(s->initprot);
    }
    return 0;
}
static void protect_image(struct image *im) {
    for (int i = 0; i < im->nsegs; i++) {
        const struct segment_command_64 *s = im->segs[i];
        if (s->vmsize == 0 || memcmp(s->segname, "__PAGEZERO", 11) == 0) continue;
        size_t mlen = (size_t)ROUNDUP(s->vmsize);
        void *at = (void *)(uintptr_t)(s->vmaddr + im->slide);
        mprotect(at, mlen, im->prot[i]);
    }
}

/* ---- dependency resolution ---- */
static int is_host_libsystem(const char *name) {
    return strcmp(name, "/usr/lib/libSystem.B.dylib") == 0 ||
           strncmp(name, "/usr/lib/system/", 16) == 0;
}
static char *expand_dep_token(struct image *im, const char *token) {
    if (!strncmp(token, "@loader_path", 12))
        return token[12] == 0 ? xstrdup(dir_name(im->path))
                              : path_join(dir_name(im->path), token + 13);
    if (!strncmp(token, "@executable_path", 16))
        return token[16] == 0 ? xstrdup(dir_name(g_main ? g_main->path : im->path))
                              : path_join(dir_name(g_main ? g_main->path : im->path), token + 17);
    return xstrdup(token);
}
static char *try_paths(const char *a, const char *b) {
    char *p = b && *b ? path_join(a, b) : xstrdup(a);
    char *r = realpath(p, NULL);
    free(p);
    return r;
}
static char *resolve_dep(struct image *im, const char *name) {
    for (int i = 0; i < g_nmaps; i++)
        if (!strcmp(g_maps[i].name, name))
            return realpath(g_maps[i].path, NULL);

    if (!strncmp(name, "@rpath/", 7)) {
        const char *rest = name + 7;
        struct image *owners[2] = { im, g_main };
        for (int k = 0; k < 2; k++) {
            struct image *o = owners[k];
            if (!o) continue;
            for (int i = 0; i < o->nrpaths; i++) {
                char *exp = expand_dep_token(o, o->rpath[i]);
                char *cand = path_join(exp, rest);
                char *r = realpath(cand, NULL);
                free(exp); free(cand);
                if (r) return r;
            }
        }
        return NULL;
    }
    if (!strncmp(name, "@loader_path", 12) || !strncmp(name, "@executable_path", 16)) {
        char *p = expand_dep_token(im, name);
        char *r = realpath(p, NULL);
        free(p);
        return r;
    }
    if (name[0] == '/') {
        if (g_root) {
            char *r = try_paths(g_root, name + 1);
            if (r) return r;
        }
        return realpath(name, NULL);
    }
    return realpath(name, NULL);
}
static struct image *load_image(const char *path, int is_main);

static int load_deps(struct image *im) {
    for (int i = 0; i < im->ndeps; i++) {
        const char *name = im->depname[i];
        if (host_provides(name)) {
            im->dep[i] = NULL;                        /* provided by --shim-lib */
            continue;
        }
        /* Only skip a host libSystem when a --shim-lib actually advertises
         * it; otherwise the shipped Mach-O libSystem.B.dylib is the runtime
         * the images bind to. */
        if (!g_load_mode && is_host_libsystem(name) && host_provides(name)) {
            im->dep[i] = NULL;                        /* host shim replaces it */
            continue;
        }
        char *path = resolve_dep(im, name);
        if (!path) {
            if (strstr(name, "libswiftQuickLookUI.dylib")) {
                fprintf(stderr, "appexec: note: stubbed optional dependency %s\n",
                        name);
                continue;
            }
            g_missing_images++;
            fprintf(stderr, "appexec: warning: %s: cannot resolve dependency %s\n",
                    im->path, name);
            continue;
        }
        im->dep[i] = load_image(path, 0);
        free(path);
        if (!im->dep[i]) {
            g_missing_images++;
            fprintf(stderr, "appexec: warning: %s: cannot load dependency %s: %s\n",
                    im->path, name, g_err);
        }
    }
    return 0;
}

static struct image *load_image(const char *path, int is_main) {
    char *rp = realpath(path, NULL);
    if (!rp) { set_err("cannot open %s: %s", path, strerror(errno)); return NULL; }
    for (int i = 0; i < g_nimgs; i++)
        if (!strcmp(g_imgs[i]->path, rp)) { free(rp); return g_imgs[i]; }
    if (g_nimgs >= MAX_IMAGES) { free(rp); set_err("too many images"); return NULL; }

    struct image *im = xmalloc(sizeof(*im));
    im->path = rp;
    im->is_main = is_main;
    g_imgs[g_nimgs++] = im;
    if (is_main) g_main = im;

    int fd = open(rp, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { set_err("cannot open %s: %s", rp, strerror(errno)); goto fail; }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 32) {
        set_err("%s: truncated file", rp); close(fd); goto fail;
    }
    im->fsize = (size_t)st.st_size;
    im->file = mmap(NULL, im->fsize, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (im->file == MAP_FAILED) { set_err("cannot map %s: %s", rp, strerror(errno)); goto fail; }

    if (parse_metadata(im) != 0) goto fail;
    if (!g_info_mode) {
        if (map_image(im) != 0) goto fail;
        load_exports(im);
        if (load_deps(im) != 0) goto fail;
    }
    return im;

fail:
    if (g_main == im) g_main = NULL;
    g_nimgs--;
    g_imgs[g_nimgs] = NULL;
    return NULL;
}

/* ---- bundle resolution ---- */
static int resolve_bundle(const char *path, char *out, size_t outsz) {
    char plist[PATH_MAX];
    snprintf(plist, sizeof plist, "%s/Contents/Info.plist", path);
    FILE *f = fopen(plist, "rb");
    if (f) {
        static char buf[1 << 20];
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f); buf[n] = 0;
        const char *k = strstr(buf, "<key>CFBundleExecutable</key>");
        if (k) {
            const char *s = strstr(k, "<string>");
            if (s) {
                s += 8;
                const char *e = strstr(s, "</string>");
                if (e && (size_t)(e - s) + 1 < outsz) {
                    char full[PATH_MAX * 4];
                    size_t len = (size_t)(e - s);
                    char name[PATH_MAX];
                    if (len >= sizeof name) return -1;
                    memcpy(name, s, len); name[len] = 0;
                    if (strchr(name, '/')) return -1;
                    snprintf(full, sizeof full, "%s/Contents/MacOS/%s", path, name);
                    if (access(full, R_OK) == 0) {
                        size_t fl = strlen(full);
                        if (fl + 1 > outsz) return -1;
                        memcpy(out, full, fl + 1);
                        return 0;
                    }
                }
            }
        }
    }
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s/Contents/MacOS", path);
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *de; int rc = -1;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char full[PATH_MAX * 2];
        snprintf(full, sizeof full, "%s/%s", dir, de->d_name);
        if (access(full, R_OK) != 0) continue;
        size_t fl = strlen(full);
        if (fl + 1 > outsz) continue;
        memcpy(out, full, fl + 1); rc = 0; break;
    }
    closedir(d);
    return rc;
}

static void print_image_info(struct image *im) {
    printf("file: %s (%s)\n", im->path, im->arch);
    printf("cpu: 0x%08x  filetype: %u  ncmds: %u\n",
           im->mh->cputype, im->mh->filetype, im->mh->ncmds);
    printf("segments: %d  span: %zu bytes\n", im->nsegs, im->span);
    for (int i = 0; i < im->nsegs; i++) {
        const struct segment_command_64 *s = im->segs[i];
        printf("  %-14s vmaddr 0x%llx vmsize 0x%llx fileoff 0x%llx filesize 0x%llx prot %u\n",
               s->segname, (unsigned long long)s->vmaddr, (unsigned long long)s->vmsize,
               (unsigned long long)s->fileoff, (unsigned long long)s->filesize, s->initprot);
    }
    if (im->ep) printf("entry: LC_MAIN entryoff 0x%llx\n", (unsigned long long)im->ep->entryoff);
    else if (im->tc) printf("entry: LC_UNIXTHREAD\n");
    printf("dylibs: %d\n", im->ndeps);
    for (int i = 0; i < im->ndeps && i < 20; i++)
        printf("  %s%s\n", im->depname[i], im->depweak[i] ? " (weak)" : "");
    if (im->ndeps > 20) printf("  ... %d more\n", im->ndeps - 20);
    if (im->cf_size) {
        const struct chained_header *ch = (const struct chained_header *)(im->slice + im->cf_off);
        printf("chained fixups: %u bytes, imports %u, format %u/%u\n",
               im->cf_size, ch->imports_count, ch->imports_format, ch->symbols_format);
        if (ch->imports_format <= CHAINED_IMPORT_ADDEND64) {
            for (uint32_t i = 0; i < ch->imports_count && i < 20; i++) {
                struct chained_import ci;
                if (chained_imports(im, ch, im->slice + im->cf_off, i, &ci) == 0) {
                    const char *n = chained_symname(im, ch, im->slice + im->cf_off, ci.name_off);
                    if (n) printf("  %s\n", n);
                }
            }
            if (ch->imports_count > 20) printf("  ... %u more\n", ch->imports_count - 20);
        }
    } else if (im->rebase_size || im->bind_size) {
        printf("dyld info: rebase %u bytes, bind %u bytes\n", im->rebase_size, im->bind_size);
    } else {
        printf("fixups: none\n");
    }
}

/*
 * Darwin x86_64 addresses thread-local variables through %gs, not %fs.  The
 * shipped runtime reads the per-thread TLV array at %gs:-0x1818, which is why
 * `_reef_tlv_offset` is -0x1818.  Give the process a GS block so those
 * accesses land in memory we own instead of faulting.
 */
#ifndef ARCH_SET_GS
#define ARCH_SET_GS 0x1001
#endif
#ifndef SYS_arch_prctl
#define SYS_arch_prctl 158
#endif
#define DARWIN_GS_BLOCK 0x4000UL
#define DARWIN_TLV_OFFSET 0x1818UL

static void setup_darwin_tls(void) {
    unsigned char *blk = mmap(NULL, DARWIN_GS_BLOCK, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (blk == MAP_FAILED) return;
    /* %gs:-0x1818 must hold a TLV array pointer whose capacity lives at
     * array[-8]; capacity 0 makes libSystem.B allocate per-key entries. */
    unsigned char *arr = blk + 0x100;
    *(void **)blk = arr;
    *(unsigned long *)(arr - 8) = 0;
    unsigned long base = (unsigned long)(blk + DARWIN_TLV_OFFSET);
    if (syscall(SYS_arch_prctl, ARCH_SET_GS, base) != 0 && getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: ARCH_SET_GS failed: %s\n", strerror(errno));
    else if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: gs base=%p tlv=%p\n", (void *)base, (void *)blk);
}

/* Resolve an export by name across the whole loaded image graph. */
static uint64_t find_export_any(const char *name, struct image **prov) {
    for (int i = 0; i < g_nimgs; i++) {
        struct image *p = NULL;
        int found = 0;
        uint64_t addr = lookup_in_image(g_imgs[i], name, &p, &found, 0);
        if (found) {
            *prov = p;
            return p->slide + addr;
        }
    }
    return 0;
}

/*
 * Install the pulsar host ABI.  The shipped loader builds a 0x680-byte table
 * and writes it into libSystem.B.dylib's `_reef_host` data slot; it also
 * publishes table[0x258] as `_reef_tlv_offset`, which libSystem.B adds to the
 * thread pointer to address its thread-local block.  Everything the macOS
 * image graph needs from the Linux side goes through this table.
 */
/* dyld's map_images: libobjc registers classes, categories and selectors
 * from each image's __objc_classlist/__objc_catlist.  Without this call the
 * runtime only sees classes it was told about explicitly, so category
 * methods such as +[NSBundle mainBundle] are never attached. */
/* Diagnostic: NULL selector references are the usual cause of libobjc
 * reporting "<null selector>".  A NULL here means a chained-fixup bind was
 * not applied to __objc_selrefs. */
static int ptr_in_image(uint64_t v) {
    for (int i = 0; i < g_nimgs; i++) {
        struct image *im = g_imgs[i];
        for (int s = 0; s < im->nsegs; s++) {
            uint64_t a = im->segs[s]->vmaddr + im->slide;
            uint64_t b = a + im->segs[s]->vmsize;
            if (v >= a && v < b) return 1;
        }
    }
    return 0;
}

static void check_objc_selrefs(const char *when) {
    int covered = 0;
    size_t total = 0;
    for (int i = 0; i < g_nimgs; i++) {
        struct image *im = g_imgs[i];
        size_t sz = 0;
        const struct section_64 *s =
            find_section(im, "__DATA_CONST", "__objc_selrefs", &sz);
        if (!s) s = find_section(im, "__DATA", "__objc_selrefs", &sz);
        if (!s || !sz) continue;
        uint64_t *p = (uint64_t *)(uintptr_t)(s->addr + im->slide);
        size_t n = sz / 8, zeros = 0, bad = 0, first = (size_t)-1;
        uint64_t firstbad = 0;
        for (size_t k = 0; k < n; k++) {
            if (!p[k]) {
                if (first == (size_t)-1) first = k;
                zeros++;
            } else if (!ptr_in_image(p[k])) {
                if (!bad) firstbad = p[k];
                bad++;
            }
        }
        covered++;
        total += n;
        if (zeros || bad)
            dprintf(2, "reef[%s]: %s selrefs %zu entries: %zu NULL (first %zu), "
                       "%zu outside images (first %#llx)\n",
                    when, im->path, n, zeros, first, bad,
                    (unsigned long long)firstbad);
    }
    dprintf(2, "reef[%s]: selrefs coverage %d images, %zu entries\n",
            when, covered, total);
}

/* Diagnostic: report method lists whose selector name slot is NULL or points
 * outside the image graph, which libobjc reports as "<null selector>". */
static void scan_method_list(const char *img, const char *owner,
                             const char *kind, uint64_t ml) {
    if (!ml) return;
    uint32_t fl = *(uint32_t *)(uintptr_t)ml;
    uint32_t cnt = *(uint32_t *)(uintptr_t)(ml + 4);
    if (cnt > 20000) return;
    int rel = (fl & 0x80000000u) != 0;
    int bad = 0;
    uint64_t firstbad = 0;
    for (uint32_t e = 0; e < cnt; e++) {
        uint64_t ent = ml + 8 + (uint64_t)e * (rel ? 12 : 24);
        uint64_t nm;
        if (rel) {
            int32_t no = *(int32_t *)(uintptr_t)ent;
            nm = (uint64_t)((int64_t)ent + no);
        } else {
            nm = *(uint64_t *)(uintptr_t)ent;
        }
        if (!nm || !ptr_in_image(nm)) {
            if (!bad) firstbad = nm;
            bad++;
        }
    }
    if (bad)
        dprintf(2, "reef: %s %s %s: %d/%u bad selector names (first %#llx) flags=%#x\n",
                img, owner, kind, bad, cnt, (unsigned long long)firstbad, fl);
}

static void scan_class(const char *img, uint64_t cls, const char *kind) {
    if (!cls) return;
    uint64_t data = *(uint64_t *)(uintptr_t)(cls + 32);
    uint64_t ro = data & ~7ull;
    if (!ro) return;
    const char *cname = *(const char **)(uintptr_t)(ro + 24);
    scan_method_list(img, cname ? cname : "?", kind,
                     *(uint64_t *)(uintptr_t)(ro + 32));
}

static void check_objc_classes(void) {
    for (int i = 0; i < g_nimgs; i++) {
        struct image *im = g_imgs[i];
        size_t sz = 0;
        const struct section_64 *s =
            find_section(im, "__DATA_CONST", "__objc_classlist", &sz);
        if (!s) s = find_section(im, "__DATA", "__objc_classlist", &sz);
        if (s && sz) {
            uint64_t *list = (uint64_t *)(uintptr_t)(s->addr + im->slide);
            for (size_t k = 0; k < sz / 8; k++) {
                uint64_t cls = list[k];
                if (!cls) continue;
                scan_class(im->path, cls, "instance");
                scan_class(im->path, *(uint64_t *)(uintptr_t)cls, "class");
            }
        }
        sz = 0;
        s = find_section(im, "__DATA_CONST", "__objc_catlist", &sz);
        if (!s) s = find_section(im, "__DATA", "__objc_catlist", &sz);
        if (!s || !sz) continue;
        uint64_t *cats = (uint64_t *)(uintptr_t)(s->addr + im->slide);
        for (size_t k = 0; k < sz / 8; k++) {
            uint64_t cat = cats[k];
            if (!cat) continue;
            const char *cname = *(const char **)(uintptr_t)cat;
            scan_method_list(im->path, cname ? cname : "?", "cat-inst",
                             *(uint64_t *)(uintptr_t)(cat + 16));
            scan_method_list(im->path, cname ? cname : "?", "cat-class",
                             *(uint64_t *)(uintptr_t)(cat + 24));
        }
    }
}

static void map_objc_images(void) {
    struct image *prov = NULL;
    uint64_t fn = find_export_any("_mapImages", &prov);
    if (!fn) fn = find_export_any("map_images", &prov);
    if (!fn) {
        /* Local symbol (not in the export trie): libobjc's _mapImages. */
        for (int i = 0; i < g_nimgs; i++) {
            if (strstr(g_imgs[i]->path, "libobjc.A.dylib")) {
                fn = (uint64_t)(g_imgs[i]->base + g_imgs[i]->slide + 0x2d70);
                break;
            }
        }
    }
    if (!fn) {
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: _mapImages not found\n");
        return;
    }
    int n = g_nimgs;
    const char **paths = calloc((size_t)n, sizeof *paths);
    void **hdrs = calloc((size_t)n, sizeof *hdrs);
    if (!paths || !hdrs) { free(paths); free(hdrs); return; }
    for (int i = 0; i < n; i++) {
        paths[i] = g_imgs[i]->path;
        hdrs[i] = (void *)(uintptr_t)(g_imgs[i]->base + g_imgs[i]->slide);
    }
    ((void (*)(unsigned, const char **, void **))(uintptr_t)fn)((unsigned)n, paths, hdrs);
    free(paths); free(hdrs);
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: mapped %d ObjC images via %p\n", n, (void *)(uintptr_t)fn);
}

/* dyld runs every image's __mod_init_func initializers after mapping and
 * before the entry point.  libobjc's _initializeRuntime lives there: it
 * registers the _SEL_* globals the forwarding path uses, so skipping it
 * makes objc_msgSend(..., NULL, ...) recurse into _reef_objc_forward. */
/* Some framework initializers block forever in this runtime (they wait on a
 * service the host ABI does not provide).  PLSR_SKIP_INIT takes a comma
 * separated list of image path substrings whose initializers are skipped so
 * the launch can continue past them. */
static int plsr_skip_initializer(const char *path) {
    const char *skip = getenv("PLSR_SKIP_INIT");
    if (!skip || !*skip || !path) return 0;
    const char *p = skip;
    while (*p) {
        const char *end = strchr(p, ',');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        while (n && (*p == ' ' || *p == '\t')) { p++; n--; }
        while (n && (p[n - 1] == ' ' || p[n - 1] == '\t')) n--;
        for (const char *q = path; n && *q; q++)
            if (!strncmp(q, p, n)) return 1;
        if (!end) break;
        p = end + 1;
    }
    return 0;
}

static void run_image_initializers(struct image *im) {
    if (plsr_skip_initializer(im->path)) {
        if (getenv("PLSR_REEF_TRACE"))
            dprintf(2, "reef: skipped initializers for %s\n", im->path);
        return;
    }
    size_t sz = 0;
    const struct section_64 *s =
        find_section(im, "__DATA_CONST", "__mod_init_func", &sz);
    if (!s) s = find_section(im, "__DATA", "__mod_init_func", &sz);
    if (!s || !sz) return;
    uint64_t *fns = (uint64_t *)(uintptr_t)(s->addr + im->slide);
    for (size_t k = 0; k < sz / 8; k++) {
        if (!fns[k]) continue;
        if (getenv("PLSR_REEF_TRACE"))
            dprintf(2, "reef: init %s[%zu] -> %p (+0x%lx)\n", im->path, k,
                    (void *)(uintptr_t)fns[k],
                    (unsigned long)(fns[k] - (im->base + im->slide)));
        ((void (*)(void))(uintptr_t)fns[k])();
    }
}

/* libobjc's _initializeRuntime registers the _SEL_* globals; dyld runs it
 * before mapping any image, so do the same. */
static void run_libobjc_init(void) {
    for (int i = 0; i < g_nimgs; i++) {
        if (!strstr(g_imgs[i]->path, "libobjc.A.dylib")) continue;
        run_image_initializers(g_imgs[i]);
        if (getenv("PLSR_REEF_TRACE")) {
            uintptr_t b = g_imgs[i]->slide;
            dprintf(2, "reef: SEL alloc=%p class=%p resolve=%p fwd=%p dnr=%p\n",
                    *(void **)(b + 0x167a8), *(void **)(b + 0x16800),
                    *(void **)(b + 0x16828), *(void **)(b + 0x16838),
                    *(void **)(b + 0x16840));
        }
    }
}

/* libobjc recurses through _reef_objc_forward when a selector is missing:
 * forwardingTargetForSelector: is sent to the same class, which forwards
 * again until the stack is gone.  Replacing the forward handler with one
 * that returns nil breaks the cycle and lets the app keep running. */
static void *reef_forward_handler(void *self, void *sel, ...) {
    if (getenv("PLSR_REEF_TRACE"))
        dprintf(2, "reef: unhandled selector %p on %p\n", sel, self);
    return NULL;
}

static void install_forward_handler(void) {
    struct image *prov = NULL;
    uint64_t fn = find_export_any("_objc_setForwardHandler", &prov);
    if (!fn) return;
    ((void (*)(void *, void *))(uintptr_t)fn)((void *)reef_forward_handler,
                                              (void *)reef_forward_handler);
}

static void run_initializers(void) {
    for (int i = g_nimgs - 1; i >= 0; i--) {
        if (strstr(g_imgs[i]->path, "libobjc.A.dylib")) continue;
        run_image_initializers(g_imgs[i]);
    }
}

/* libdispatch keeps its thread identity (`dt_tid`) and the main-queue gate in
 * thread-local storage.  Nothing in this image graph initialises them on the
 * thread that enters the program, so the first gate broadcast - AppKit's
 * window creation is one - compares a zero/stale tid and traps with ud2 in
 * __dispatch_gate_broadcast_slow.  Do what dyld does on a normal launch: run
 * _libdispatch_init (creates the TSD key) and _libdispatch_tsd_init (records
 * this thread's id) before the Mach-O entry point. */
/* libdispatch's gate broadcast asserts that the calling thread owns the gate
 * (ud2 at libdispatch+0x1d458) and that the ulock wake failed only with
 * ENOENT (ud2 at +0x1d45f).  The TSD block and the main-queue gate are not
 * always touched by the same thread in this runtime, so both assertions fire
 * during AppKit startup even though the wake itself is correct.  Turn the two
 * conditional jumps into nops so the broadcast proceeds. */
static void patch_dispatch_gate(void) {
    if (getenv("PLSR_NO_GATE_PATCH")) return;
    struct image *im = NULL;
    for (int i = 0; i < g_nimgs; i++)
        if (strstr(g_imgs[i]->path, "libdispatch.dylib")) { im = g_imgs[i]; break; }
    if (!im) return;
    unsigned char *base = (unsigned char *)(uintptr_t)(im->base + im->slide);
    unsigned char *tid_jne = base + 0x1d404;   /* jne 0x1d458 (tid mismatch) */
    unsigned char *err_jne = base + 0x1d438;   /* jne 0x1d45a (errno != ENOENT) */
    static const unsigned char want1[2] = {0x75, 0x52};   /* jne +0x52 */
    static const unsigned char want2[2] = {0x75, 0x20};   /* jne +0x20 */
    if (memcmp(tid_jne, want1, 2) != 0 || memcmp(err_jne, want2, 2) != 0) {
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: gate patch skipped (%02x%02x / %02x%02x)\n",
                    tid_jne[0], tid_jne[1], err_jne[0], err_jne[1]);
        return;
    }
    long pg = sysconf(_SC_PAGESIZE);
    if (pg <= 0) pg = 4096;
    uintptr_t p1 = (uintptr_t)tid_jne & ~(uintptr_t)(pg - 1);
    uintptr_t p2 = (uintptr_t)err_jne & ~(uintptr_t)(pg - 1);
    mprotect((void *)p1, (size_t)pg, PROT_READ | PROT_WRITE | PROT_EXEC);
    if (p2 != p1) mprotect((void *)p2, (size_t)pg, PROT_READ | PROT_WRITE | PROT_EXEC);
    memset(tid_jne, 0x90, 2);
    memset(err_jne, 0x90, 2);
    __builtin___clear_cache((char *)tid_jne, (char *)tid_jne + 2);
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: disabled libdispatch gate traps\n");
}

static void plsr_libdispatch_init(void) {
    if (getenv("PLSR_NO_DISPATCH_INIT")) return;
    struct image *prov = NULL;
    uint64_t init = find_export_any("_libdispatch_init", &prov);
    uint64_t tsd = find_export_any("_libdispatch_tsd_init", &prov);
    if (init) ((void (*)(void))(uintptr_t)init)();
    if (tsd) ((void (*)(void))(uintptr_t)tsd)();
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: libdispatch init=%p tsd=%p\n",
                (void *)(uintptr_t)init, (void *)(uintptr_t)tsd);
}

uintptr_t plsr_image_base(const char *needle) {
    for (int i = 0; i < g_nimgs; i++)
        if (strstr(g_imgs[i]->path, needle))
            return (uintptr_t)(g_imgs[i]->base + g_imgs[i]->slide);
    return 0;
}

static void install_reef_host(int argc, char **argv, char **envp,
                              const char *exe) {
    setup_darwin_tls();
    void *table = reef_host_init();
    struct image *prov = NULL;

    *(int *)((char *)table + 0x000) = argc;
    *(char ***)((char *)table + 0x008) = argv;
    *(char ***)((char *)table + 0x010) = envp;
    *(char **)((char *)table + 0x018) = xstrdup(exe ? exe : "");
    {
        const char *base = exe ? strrchr(exe, '/') : NULL;
        *(char **)((char *)table + 0x020) =
            xstrdup(base ? base + 1 : (exe ? exe : ""));
    }

    void **slot = (void **)(uintptr_t)find_export_any("_reef_host", &prov);
    if (slot) {
        *slot = table;
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: _reef_host=%p table=%p\n", (void *)slot, table);
    } else if (getenv("PLSR_REEF_TRACE")) {
        fprintf(stderr, "reef: _reef_host symbol not found (images=%d)\n", g_nimgs);
        for (int i = 0; i < g_nimgs; i++) {
            if (!strstr(g_imgs[i]->path, "libSystem.B")) continue;
            const struct export_ent *e = img_lookup(g_imgs[i], "_reef_host");
            fprintf(stderr, "reef: libSystem.B exports=%u reef_host=%p base=%p slide=%lx\n",
                    g_imgs[i]->nexports, (const void *)e,
                    (void *)(uintptr_t)g_imgs[i]->base,
                    (unsigned long)g_imgs[i]->slide);
        }
    }

    long *tlv = (long *)(uintptr_t)find_export_any("_reef_tlv_offset", &prov);
    if (tlv) *tlv = reef_tlv_offset();
}

static void usage(FILE *out) {
    fprintf(out,
        "usage: plsr-lite [--info|--load] [--runtime DIR] [--shim-lib FILE]\n"
        "                 [--stub-missing[=zero]] [--image-map FILE]\n"
        "                 <macho or .app> [arguments...]\n");
}

int main(int argc, char **argv) {
    int info = 0, load = 0, argi = 1;
    for (; argi < argc; argi++) {
        const char *a = argv[argi];
        if (!strcmp(a, "--")) { argi++; break; }
        if (!strcmp(a, "--info")) info = 1;
        else if (!strcmp(a, "--load")) load = 1;
        else if (!strcmp(a, "--strict")) g_strict = 1;
        else if (!strcmp(a, "--trace-missing")) g_trace_missing = 1;
        else if (!strcmp(a, "--stub-missing")) g_stub_missing = 1;
        else if (!strcmp(a, "--stub-missing=zero")) g_stub_missing = 2;
        else if (!strcmp(a, "--shim-lib") && argi + 1 < argc) add_hostlib(argv[++argi]);
        else if (!strncmp(a, "--shim-lib=", 11)) add_hostlib(a + 11);
        else if (!strcmp(a, "--cleanroom")) g_cleanroom = 1;
        else if (!strcmp(a, "--runtime") && argi + 1 < argc) g_root = argv[++argi];
        else if (!strncmp(a, "--runtime=", 10)) g_root = a + 10;
        else if (!strcmp(a, "--image-map") && argi + 1 < argc) read_image_map(argv[++argi]);
        else if (!strncmp(a, "--image-map=", 12)) read_image_map(a + 12);
        else if (a[0] == '-') { usage(stderr); return 2; }
        else break;
    }
    if (argi >= argc) { usage(stderr); return 2; }
    g_load_mode = load;
    g_info_mode = info;
    if (g_nhostlibs > 0 || getenv("PLSR_CLEANROOM")) g_cleanroom = 1;
    g_objc_scan = (void (*)(void *))shim_lookup("plsr_objc_scan_image");
    g_trace = getenv("PLSR_LITE_TRACE") != NULL;

    if (!g_root) {
        char exe[PATH_MAX];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (n > 0) {
            exe[n] = 0;
            char *d = dir_name(exe);
            char *cand = path_join(d, "../lib/pulsarappexec");
            if (is_dir(cand)) g_root = cand;
            free(cand); free(d);
        }
        if (!g_root) {
            const char *home = getenv("HOME");
            if (home && *home) {
                char *cand = fmt("%s/.local/share/pulsarappexec", home);
                if (is_dir(cand)) g_root = cand;
                else free(cand);
            }
        }
    }

    const char *name = argv[argi];
    char resolved[PATH_MAX];
    const char *exe = name;
    if (is_dir(name)) {
        if (resolve_bundle(name, resolved, sizeof resolved) != 0)
            die("%s: no main executable in the bundle", name);
        exe = resolved;
    }

    struct image *main_img = load_image(exe, 1);
    if (!main_img) die("%s", g_err);

    if (info) { print_image_info(main_img); return 0; }

    install_fault_handlers();
    if (g_root) setenv("PLSR_ROOT", g_root, 1);
    /* The compat tree ships redirect stubs, so AppKit's CoreGraphics host load
     * (@root/host/libpulsar-app-host.so) cannot resolve its dependencies unless
     * the real objects are already registered.  Do this before any image
     * initializer runs. */
    if (!g_load_mode && !g_cleanroom) plsr_preload_compat();
    for (int i = 0; i < g_nimgs; i++) apply_fixups(g_imgs[i]);
    if (getenv("PLSR_REEF_TRACE")) {
        check_lazy_stubs(); check_objc_selrefs("pre"); check_objc_classes();
    }
    if (!g_load_mode && !g_cleanroom)
        install_reef_host(argc - argi, argv + argi, environ, main_img->path);
    if (!g_load_mode && getenv("PLSR_MAYO_UI"))
        return plsr_mayo_ui_main(argc - argi, argv + argi, main_img->path);
    /* Before the app's initializers: they set up their own toolkit/display
     * state, after which a window created through the host interface is never
     * mapped. */
    if (!g_load_mode && getenv("PLSR_HOST_WINDOW"))
        plsr_host_window_bootstrap(argc - argi, argv + argi);
    if (g_objc_scan && !g_load_mode) {
        for (int i = 0; i < g_nimgs; i++)
            g_objc_scan((void *)(uintptr_t)(g_imgs[i]->base + g_imgs[i]->slide));
    }
    for (int i = 0; i < g_nimgs; i++) protect_image(g_imgs[i]);
    if (!g_load_mode && !g_cleanroom) patch_tlv_accessor();
    if (!g_load_mode && !g_cleanroom) patch_tlv_descriptors();
    /* dyld maps every image before running initializers: _setUpBlockClasses
     * messages __NSStackBlock__, which must already be registered. */
    if (!g_load_mode && !g_cleanroom) map_objc_images();
    if (!g_load_mode && !g_cleanroom) run_libobjc_init();
    if (!g_load_mode && !g_cleanroom) install_uncaught_handler();
    if (!g_load_mode && !g_cleanroom) patch_undefined_key_fallback();
    if (!g_load_mode && !g_cleanroom) prerealize_classes();
    if (!g_load_mode && !g_cleanroom) install_string_range_guard();
    if (!g_load_mode && !g_cleanroom) install_forward_handler();
    if (getenv("PLSR_REEF_TRACE")) check_objc_selrefs("post");
    /* Swift text must be executable before entering libswiftCore's
     * registration routines; data must already have its final protection. */
    if (!g_load_mode) register_swift_images();
    if (!g_load_mode && !g_cleanroom) patch_swift_sidetable();
    if (!g_load_mode && !g_cleanroom) patch_swift_dealloc_count();
    if (!g_load_mode && !g_cleanroom && !getenv("PLSR_GUARD_LATE")) patch_objc_object_guard();
    if (!g_load_mode && !g_cleanroom) patch_cf_key_guard();
    if (!g_load_mode && !g_cleanroom) patch_cf_collection_nil();
    if (!g_load_mode && !g_cleanroom) patch_cf_release_nil();
    if (!g_load_mode && !g_cleanroom) run_initializers();
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: run_initializers returned\n");
    if (!g_load_mode && !g_cleanroom) plsr_libdispatch_init();
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: libdispatch init returned\n");
    if (!g_load_mode && !g_cleanroom) patch_dispatch_gate();
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: dispatch gate patched\n");
    if (!g_load_mode && !g_cleanroom) patch_storyboard_bootstrap();
    if (!g_load_mode && !g_cleanroom) patch_kvo_key_paths();
    if (!g_load_mode && !g_cleanroom) {
        if (getenv("PLSR_DUMP_SELREF")) plsr_dump_selrefs(getenv("PLSR_DUMP_SELREF"));
        if (getenv("PLSR_REEF_TRACE")) fprintf(stderr, "reef: install bundle path fallback\n");
        install_bundle_path_fallback();
        if (getenv("PLSR_REEF_TRACE")) fprintf(stderr, "reef: patch archiver current values\n");
        patch_archiver_current_values();
        if (getenv("PLSR_REEF_TRACE")) fprintf(stderr, "reef: patch NSColor background\n");
        plsr_patch_nscolor_background();
        if (getenv("PLSR_REEF_TRACE")) fprintf(stderr, "reef: install NSWindow init trace\n");
        plsr_trace_nswindow_init();
        if (getenv("PLSR_REEF_TRACE")) fprintf(stderr, "reef: patch NSData file reader\n");
        patch_nsdata_read();
        patch_swift_nsdata_contents_thunk();
        if (getenv("PLSR_REEF_TRACE")) fprintf(stderr, "reef: patch nib collection factories\n");
        patch_nib_collections();
        install_fault_handlers();
        if (getenv("PLSR_REEF_TRACE")) fprintf(stderr, "reef: post-init patches done\n");
    }
    if (!g_load_mode && getenv("PLSR_ARCHIVER_PROBE")) probe_keyed_archiver();
    if (!g_load_mode && !g_cleanroom) patch_exception_callstack();
    if (!g_load_mode && getenv("PLSR_NSWINDOW")) {
        /* Keep normal directory iteration enabled: storyboard discovery and
         * AppKit's bundle resource enumeration depend on readdir.  The old
         * suppression made CFBundle see an empty Resources directory, so
         * CotEditor threw before the event loop (and before this hook could
         * create its window). */
        plsr_swizzle_next_event();
    }
    if (!g_load_mode && getenv("PLSR_GUARD_LATE")) {
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: installing ObjC guards late\n");
        patch_objc_object_guard();
    }
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: initializers done\n");
    if (getenv("PLSR_REEF_TRACE")) {
        struct image *host_provider = NULL;
        uint64_t host_slot = find_export_any("_reef_host", &host_provider);
        void *slot = host_slot ? (void *)(uintptr_t)host_slot : NULL;
        void *api = slot ? *(void **)slot : NULL;
        dprintf(2, "reef: MachO _reef_host=%p api=%p slot250=%p provider=%s\n",
                slot, api,
                api ? *(void **)((char *)api + 0x250) : NULL,
                host_provider ? host_provider->path : "(missing)");
        for (int i = 0; i < g_nimgs; i++) {
            if (!strstr(g_imgs[i]->path, "CoreGraphics"))
                continue;
            uintptr_t got = (uintptr_t)(g_imgs[i]->base + g_imgs[i]->slide + 0x4e040);
            dprintf(2, "reef: CG got=%p value=%p\n", (void *)got, *(void **)got);
        }
        if (g_cleanroom) {
            void *rh = shim_lookup("_reef_host");
            void *capi = rh ? *(void **)rh : NULL;
            dprintf(2, "reef: shim _reef_host=%p api=%p slot250=%p\n", rh, capi,
                    capi ? *(void **)((char *)capi + 0x250) : NULL);
        }
    }

    if (load) {
        uint64_t exports = 0;
        for (int i = 0; i < g_nimgs; i++) exports += g_imgs[i]->nexports;
        if (g_trace)
            for (int i = 0; i < g_nimgs; i++)
                fprintf(stderr, "image %d: %s (base 0x%llx, slide 0x%llx, exports %u)\n",
                        i, g_imgs[i]->path, (unsigned long long)g_imgs[i]->base,
                        (unsigned long long)g_imgs[i]->slide, g_imgs[i]->nexports);
        printf("file: %s\n", main_img->path);
        printf("images: %d loaded, %d missing\n", g_nimgs, g_missing_images);
        printf("segments: %d  span: %zu bytes\n", main_img->nsegs, main_img->span);
        printf("exports: %llu  bound: %llu  unresolved: %llu  weak: %llu  errors: %llu\n",
               (unsigned long long)exports, (unsigned long long)g_bound,
               (unsigned long long)g_unresolved, (unsigned long long)g_weak_missing,
               (unsigned long long)g_errors);
        return (g_strict && g_errors) ? 1 : 0;
    }

    const struct segment_command_64 *text = NULL;
    for (int i = 0; i < main_img->nsegs; i++)
        if (memcmp(main_img->segs[i]->segname, "__TEXT", 7) == 0) { text = main_img->segs[i]; break; }

    uintptr_t entry;
    if (main_img->ep) {
        entry = (uintptr_t)(main_img->slide + text->vmaddr +
                            (main_img->ep->entryoff - text->fileoff));
    } else {
        const uint8_t *state = (const uint8_t *)main_img->tc + 16;
        uint64_t rip;
        memcpy(&rip, state + 16 * 8, 8);
        entry = (uintptr_t)(rip + main_img->slide);
    }

    if (getenv("PLSR_GDB_WAIT")) raise(SIGSTOP);
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: jumping to entry %p (tid %ld)\n", (void *)entry,
                (long)syscall(SYS_gettid));
    if (g_root)
        setenv("PLSR_ROOT", g_root, 1);
    if (main_img->path) {
        const char *dot = strstr(main_img->path, ".app");
        if (dot) {
            char *bundle = strndup(main_img->path, (size_t)(dot - main_img->path) + 4);
            if (bundle) {
                setenv("PLSR_APP_BUNDLE", bundle, 1);
                free(bundle);
            }
        } else {
            char *dir = dir_name(main_img->path);
            if (dir) {
                setenv("PLSR_APP_BUNDLE", dir, 1);
                free(dir);
            }
        }
    }
    /* Run the app on the process's real (pthread-registered) stack.  A private
     * mmap'd stack makes _pthread_get_stackaddr_np() report the pthread stack,
     * so CoreFoundation's exception recorder walks off the real frames and
     * faults (CoreFoundation+0xe1e3e).  Staying inside the main stack keeps
     * the bounds correct. */
    size_t stacksz = 512 * 1024;
    uintptr_t cur_sp;
    __asm__ __volatile__("mov %%rsp, %0" : "=r"(cur_sp));
    uintptr_t stack_top = (cur_sp - 64 * 1024) & ~(uintptr_t)0xF;
    uint8_t *stack = (uint8_t *)(stack_top - stacksz);
    extern char **environ;
    int nargs = argc - argi, envc = 0;
    while (environ[envc]) envc++;
    char **av = calloc((size_t)nargs + 1, sizeof(char *)), **ev = calloc((size_t)envc + 1, sizeof(char *));
    uint8_t *strp = stack;
    for (int i = 0; i < nargs; i++) {
        size_t n = strlen(argv[argi + i]) + 1;
        memcpy(strp, argv[argi + i], n); av[i] = (char *)strp; strp += n;
    }
    for (int i = 0; i < envc; i++) {
        size_t n = strlen(environ[i]) + 1;
        memcpy(strp, environ[i], n); ev[i] = (char *)strp; strp += n;
    }
    size_t words = 1 + (size_t)(nargs + 1) + (size_t)(envc + 1) + 1;
    uintptr_t sp = ((uintptr_t)(stack + stacksz) - words * 8) & ~(uintptr_t)0xF;
    sp -= 8;   /* SysV ABI: rsp % 16 == 8 on function entry */
    if (sp < (uintptr_t)strp) die("arguments too large for stack");
    uintptr_t *p = (uintptr_t *)sp;
    *p++ = (uintptr_t)nargs;
    for (int i = 0; i < nargs; i++) *p++ = (uintptr_t)av[i];
    *p++ = 0;
    for (int i = 0; i < envc; i++) *p++ = (uintptr_t)ev[i];
    *p++ = 0;
    *p++ = 0;

    if (getenv("PLSR_LITE_BT")) {
        for (int i = 0; i < g_nimgs; i++)
            dprintf(2, "IMG %d base=%p slide=%p span=%zu %s\n", i,
                    (void *)(uintptr_t)(g_imgs[i]->base + g_imgs[i]->slide),
                    (void *)(uintptr_t)g_imgs[i]->slide, g_imgs[i]->span,
                    g_imgs[i]->path);
        signal(SIGALRM, plsr_backtrace_alarm);
        /* Do not arm an alarm here: the diagnostic backtrace must not
         * terminate a healthy launch before the app's own timeout. */
    }
    /* The host surface is only kept alive by pumping its event loop, and that
     * loop must stay on the thread that initialized it.  Two modes make the
     * window survive: "keep" pumps here instead of entering the app, and
     * PLSR_ENTRY_THREAD runs the app's entry on a second thread so this thread
     * can keep pumping. */
    const char *winmode = getenv("PLSR_HOST_WINDOW");
    if (g_host_dispatch && winmode && !strcmp(winmode, "keep")) {
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: host window keep-alive on main thread\n");
        for (;;) { g_host_dispatch(); usleep(16000); }
    }
    if (g_host_dispatch && winmode && getenv("PLSR_ENTRY_THREAD")) {
        struct entry_args ea = { sp, entry, nargs, av, ev };
        pthread_t t;
        if (pthread_create(&t, NULL, plsr_entry_thread, &ea) == 0) {
            if (getenv("PLSR_REEF_TRACE"))
                fprintf(stderr, "reef: entry on thread, host loop on main\n");
            for (;;) { g_host_dispatch(); usleep(16000); }
        }
    }
    if (getenv("PLSR_ENTRY_TRAP"))
        raise(SIGSTOP);   /* debugger stop right before the Mach-O entry */
    register long a0 __asm__("rdi") = nargs;
    register char **a1 __asm__("rsi") = av;
    register char **a2 __asm__("rdx") = ev;
    register char **a3 __asm__("rcx") = NULL;   /* Darwin "apple" vector */
    __asm__ __volatile__(
        "mov %[sp], %%rsp\n\t"
        "jmp *%[e]\n\t"
        :: [sp] "r"(sp), [e] "r"(entry), "D"(a0), "S"(a1), "d"(a2), "c"(a3)
        : "memory");
    __builtin_unreachable();
}

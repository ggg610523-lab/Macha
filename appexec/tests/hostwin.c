/* hostwin: drive libpulsar-app-host.so's AppKit host interface directly.
 *
 * The Mach-O app reaches hostWindowCreate only after AppKit has a delegate and
 * an NSWindow.  This harness exercises the same host entry points (init,
 * create, title, show, dispatch, grab) without CoreFoundation in the way, so a
 * failure here is a backend problem and a success proves the window path works.
 *
 * usage: hostwin [--no-show] [--grab FILE] [--seconds N] [--width W] [--height H]
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef void *(*iface_fn)(int);
typedef int (*init_fn)(int, char **, const char *, void **);
typedef void *(*create_fn)(double, double, void *, void *, void *, unsigned, long);
typedef void (*show_fn)(void *);
typedef void (*title_fn)(void *, const char *);
typedef void (*dispatch_fn)(void);
typedef int (*grab_fn)(void *, const char *);

static void **vt;
/* reef_appkit_host_interface returns a table whose first word is a header
 * (0x17); interface entry i lives at vt[i + 1]. */
#define ENTRY(i) (vt[(i) + 1])

static const char *entry_names[] = {
    "init", "?", "dispatch", "statusSupported", "screenSize", "mainScreen",
    "themeInit", "themeIsDark", "themeAccent?", "themeAccent2?",
    "windowCreate", "windowRelease?", "windowSetTitle", "windowResize",
    "setMinSize", "setMaxSize", "windowShow", "viewRequestPaint",
    "windowClient?", "windowOwner?", "windowIsActive", "windowActivate",
    "windowStartMove", "windowMinimize", "windowHideApp", "windowMaximize",
    "setCursor", "setTextInput", "windowGrab", "testInput", "typeKey",
};

int main(int argc, char **argv) {
    const char *lib = getenv("PLSR_HOST_LIB");
    if (!lib) lib = "pulsar-appexec/usr/lib/pulsar-appexec/host/libpulsar-app-host.so";
    int do_show = 1, seconds = 8;
    double w = 900, h = 620;
    const char *grab = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--no-show")) do_show = 0;
        else if (!strcmp(argv[i], "--grab") && i + 1 < argc) grab = argv[++i];
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--width") && i + 1 < argc) w = atof(argv[++i]);
        else if (!strcmp(argv[i], "--height") && i + 1 < argc) h = atof(argv[++i]);
    }

    void *libh = dlopen(lib, RTLD_NOW | RTLD_GLOBAL);
    if (!libh) { fprintf(stderr, "hostwin: dlopen: %s\n", dlerror()); return 2; }
    iface_fn iface = (iface_fn)dlsym(libh, "reef_appkit_host_interface");
    if (!iface) { fprintf(stderr, "hostwin: no reef_appkit_host_interface\n"); return 2; }
    vt = (void **)iface(0x17);
    if (!vt) { fprintf(stderr, "hostwin: interface(0x17) = NULL\n"); return 2; }
    fprintf(stderr, "hostwin: vtable=%p\n", (void *)vt);
    fprintf(stderr, "hostwin: header=%p\n", vt[0]);
    for (int i = 0; i < 31; i++)
        fprintf(stderr, "hostwin:   [%2d] %-18s %p\n", i, entry_names[i], ENTRY(i));
    if (!ENTRY(0) || !ENTRY(10)) { fprintf(stderr, "hostwin: missing init/create\n"); return 2; }

    static char *av[] = { "CotEditor", NULL };
    void *cfg[20] = {0};
    int rc = ((init_fn)ENTRY(0))(1, av, "CotEditor", cfg);
    fprintf(stderr, "hostwin: init -> %d\n", rc);

    if (ENTRY(6)) { ((void (*)(void))ENTRY(6))(); fprintf(stderr, "hostwin: theme init done\n"); }
    if (getenv("PLSR_SCREEN") && ENTRY(4)) {
        double sw = 0, sh = 0;
        ((void (*)(void *, double, double *, double *))ENTRY(4))(NULL, 0.0, &sw, &sh);
        fprintf(stderr, "hostwin: screen %.0fx%.0f\n", sw, sh);
        if (sw > 100 && sh > 100) {
            if (w > sw) w = sw;
            if (h > sh) h = sh;
        }
    }

    fprintf(stderr, "hostwin: creating %gx%g\n", w, h);
    void *win = ((create_fn)ENTRY(10))(w, h, NULL, NULL, NULL, 0, 0);
    fprintf(stderr, "hostwin: create(%gx%g) -> %p\n", w, h, win);
    if (!win) return 3;
    if (ENTRY(12)) ((title_fn)ENTRY(12))(win, "CotEditor");
    if (do_show && ENTRY(16)) { ((show_fn)ENTRY(16))(win); fprintf(stderr, "hostwin: shown\n"); }

    /* Give the compositor a few frames before grabbing the window surface. */
    for (int t = 0; t < 20; t++) {
        if (ENTRY(2)) ((dispatch_fn)ENTRY(2))();
        usleep(50000);
    }
    if (grab && ENTRY(28)) {
        int g = ((grab_fn)ENTRY(28))(win, grab);
        fprintf(stderr, "hostwin: grab('%s') -> %d\n", grab, g);
    }

    for (int t = 0; t < seconds * 10; t++) {
        if (ENTRY(2)) ((dispatch_fn)ENTRY(2))();
        usleep(100000);
    }
    fprintf(stderr, "hostwin: done\n");
    return 0;
}

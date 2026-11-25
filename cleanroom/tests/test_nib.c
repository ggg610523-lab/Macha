/*
 * Nib and storyboard decoding, driven the way an app drives it.
 *
 * The fixtures are the shipped CotEditor resources: Main.storyboardc's
 * MainMenu.nib (35 menus, 258 items, 170 control connectors), the
 * PrintPanelAccessory scene (NSViewController + view nib), and Sparkle's
 * SUUpdateAlert.nib (an NSWindowTemplate with a real content view).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *objc_lib, *foundation_lib, *appkit_lib;
static void *(*get_class)(const char *);
static void *(*lookup)(void *, const char *);
static const char *(*class_name)(void *);
static void *(*class_super)(void *);
static int failures;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (cond)                                                             \
            printf("ok   %s\n", msg);                                         \
        else {                                                                \
            printf("FAIL %s\n", msg);                                         \
            failures++;                                                       \
        }                                                                     \
    } while (0)

static void *sym(void *handle, const char *name)
{
    void *p = dlsym(handle, name);
    if (!p) {
        fprintf(stderr, "missing symbol %s\n", name);
        exit(2);
    }
    return p;
}

static void *call0(void *o, const char *s)
{
    void *(*fn)(void *, const char *);
    if (!o)
        return NULL;
    fn = (void *(*)(void *, const char *))lookup(o, s);
    return fn ? fn(o, s) : NULL;
}

static void *call1(void *o, const char *s, void *a)
{
    void *(*fn)(void *, const char *, void *);
    if (!o)
        return NULL;
    fn = (void *(*)(void *, const char *, void *))lookup(o, s);
    return fn ? fn(o, s, a) : NULL;
}

static void *call2(void *o, const char *s, void *a, void *b)
{
    void *(*fn)(void *, const char *, void *, void *);
    if (!o)
        return NULL;
    fn = (void *(*)(void *, const char *, void *, void *))lookup(o, s);
    return fn ? fn(o, s, a, b) : NULL;
}

static void *call4(void *o, const char *s, void *a, void *b, void *c, void *d)
{
    void *(*fn)(void *, const char *, void *, void *, void *, void *);
    if (!o)
        return NULL;
    fn = (void *(*)(void *, const char *, void *, void *, void *, void *))
        lookup(o, s);
    return fn ? fn(o, s, a, b, c, d) : NULL;
}

static long num(void *v)
{
    return (long)(intptr_t)v;
}

typedef struct {
    double x, y, w, h;
} R;

static R frame_of(void *o)
{
    R (*fn)(void *, const char *);
    R r = { 0, 0, 0, 0 };
    if (!o)
        return r;
    fn = (R (*)(void *, const char *))lookup(o, "frame");
    return fn ? fn(o, "frame") : r;
}

static void set_frame_size(void *o, double w, double h)
{
    void (*fn)(void *, const char *, double, double);
    if (!o)
        return;
    fn = (void (*)(void *, const char *, double, double))lookup(o, "setFrameSize:");
    if (fn)
        fn(o, "setFrameSize:", w, h);
}

static void present_popover(void *src, void *dst, void *view)
{
    void (*fn)(void *, const char *, void *, R, void *, unsigned long, long);
    R r = { 0, 0, 10, 10 };
    if (!src)
        return;
    fn = (void (*)(void *, const char *, void *, R, void *, unsigned long, long))
        lookup(src,
               "presentViewController:asPopoverRelativeToRect:ofView:preferredEdge:behavior:");
    if (fn)
        fn(src,
           "presentViewController:asPopoverRelativeToRect:ofView:preferredEdge:behavior:",
           dst, r, view, 1, 0);
}

static unsigned long autoresizing_of(void *o)
{
    unsigned long (*fn)(void *, const char *);
    if (!o)
        return 0;
    fn = (unsigned long (*)(void *, const char *))lookup(o, "autoresizingMask");
    return fn ? fn(o, "autoresizingMask") : 0;
}

static int count_autoresizing(void *view, int depth)
{
    long n;
    void *subs;
    int total = 0;
    if (!view || depth > 8)
        return 0;
    if ((uintptr_t)(*(void **)view) < 0x10000)
        return 0;
    if (autoresizing_of(view))
        total++;
    subs = call0(view, "subviews");
    n = num(call0(subs, "count"));
    for (long i = 0; i < n; i++)
        total += count_autoresizing(
            call1(subs, "objectAtIndex:", (void *)(intptr_t)i), depth + 1);
    return total;
}

/* First direct child that carries an autoresizing mask. */
static void *find_masked_child(void *view)
{
    long n;
    void *subs;
    if (!view)
        return NULL;
    subs = call0(view, "subviews");
    n = num(call0(subs, "count"));
    for (long i = 0; i < n; i++) {
        void *c = call1(subs, "objectAtIndex:", (void *)(intptr_t)i);
        if (!c || (uintptr_t)(*(void **)c) < 0x10000)
            continue;
        if (autoresizing_of(c))
            return c;
    }
    return NULL;
}

static const char *cstr(void *s)
{
    return s ? (const char *)call0(s, "UTF8String") : NULL;
}

static void *mkstr(const char *s)
{
    void *cls = get_class("NSString");
    void *(*fn)(void *, const char *, const char *);
    if (!cls)
        return NULL;
    fn = (void *(*)(void *, const char *, const char *))lookup(
        cls, "stringWithUTF8String:");
    return fn ? fn(cls, "stringWithUTF8String:", s ? s : "") : NULL;
}

static const char *cls_name(void *o)
{
    return o ? class_name(*(void **)o) : "(nil)";
}

static void *find_item(void *menu, const char *title)
{
    long n = num(call0(menu, "numberOfItems"));
    for (long i = 0; i < n; i++) {
        void *item = call1(menu, "itemAtIndex:", (void *)(intptr_t)i);
        const char *t = cstr(call0(item, "title"));
        if (t && !strcmp(t, title))
            return item;
    }
    return NULL;
}

static long count_views(void *view, int depth)
{
    long total = 0, n;
    void *subs;
    if (!view || depth > 8)
        return 0;
    if (getenv("PLSR_NIB_DEBUG"))
        printf("view %p isa %p depth %d\n", view, *(void **)view, depth);
    if ((uintptr_t)(*(void **)view) < 0x10000)
        return 0;
    total = 1;
    subs = call0(view, "subviews");
    n = num(call0(subs, "count"));
    for (long i = 0; i < n; i++)
        total += count_views(call1(subs, "objectAtIndex:", (void *)(intptr_t)i),
                             depth + 1);
    return total;
}

static int has_text_field(void *view, int depth)
{
    long n;
    void *subs;
    if (!view || depth > 8)
        return 0;
    if ((uintptr_t)(*(void **)view) < 0x10000)
        return 0;
    if (!strcmp(cls_name(view), "NSTextField")) {
        const char *s = cstr(call0(view, "stringValue"));
        if (s && *s)
            return 1;
    }
    subs = call0(view, "subviews");
    n = num(call0(subs, "count"));
    for (long i = 0; i < n; i++)
        if (has_text_field(call1(subs, "objectAtIndex:", (void *)(intptr_t)i),
                           depth + 1))
            return 1;
    return 0;
}

int main(int argc, char **argv)
{
    void *bundle_cls, *bundle, *sb_cls, *sb, *menu, *file, *file_menu;
    void *app_cls, *app;
    int (*app_main)(int, const char **);
    char path[PATH_MAX];

    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 5) {
        fprintf(stderr,
                "usage: %s libobjc libfoundation libappkit CotEditor.app\n",
                argv[0]);
        return 2;
    }
    objc_lib = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
    if (!objc_lib) {
        fprintf(stderr, "dlopen %s: %s\n", argv[1], dlerror());
        return 2;
    }
    foundation_lib = dlopen(argv[2], RTLD_NOW | RTLD_GLOBAL);
    if (!foundation_lib) {
        fprintf(stderr, "dlopen %s: %s\n", argv[2], dlerror());
        return 2;
    }
    appkit_lib = dlopen(argv[3], RTLD_NOW | RTLD_GLOBAL);
    if (!appkit_lib) {
        fprintf(stderr, "dlopen %s: %s\n", argv[3], dlerror());
        return 2;
    }
    get_class = (void *(*)(const char *))sym(objc_lib, "_objc_getClass");
    lookup = (void *(*)(void *, const char *))sym(objc_lib,
                                                  "plsr_objc_lookup_imp");
    class_name = (const char *(*)(void *))sym(objc_lib, "_class_getName");
    class_super = (void *(*)(void *))sym(objc_lib, "_class_getSuperclass");
    (void)class_super;
    setenv("PLSR_APP_BUNDLE", argv[4], 1);

    /* ---- main storyboard: menus + delegate ---- */
    app_main = (int (*)(int, const char **))sym(appkit_lib, "_NSApplicationMain");
    CHECK(app_main(0, NULL) == 0, "NSApplicationMain launches the bundle");

    app_cls = get_class("NSApplication");
    app = app_cls ? call0(app_cls, "sharedApplication") : NULL;
    CHECK(app != NULL, "shared application exists");
    menu = app ? call0(app, "mainMenu") : NULL;
    CHECK(menu != NULL, "NSMainStoryboardFile installed the main menu");
    CHECK(menu && num(call0(menu, "numberOfItems")) == 10,
          "main menu has 10 top-level menus");
    CHECK(menu && find_item(menu, "CotEditor") != NULL, "CotEditor menu");
    CHECK(menu && find_item(menu, "File") != NULL, "File menu");
    CHECK(menu && find_item(menu, "Help") != NULL, "Help menu");

    file = menu ? find_item(menu, "File") : NULL;
    file_menu = file ? call0(file, "submenu") : NULL;
    CHECK(file_menu != NULL, "File menu built its submenu");
    CHECK(file_menu && num(call0(file_menu, "numberOfItems")) > 10,
          "File menu carries its items");
    {
        void *item = file_menu ? find_item(file_menu, "New") : NULL;
        const char *key = item ? cstr(call0(item, "keyEquivalent")) : NULL;
        const char *action = item ? (const char *)call0(item, "action") : NULL;
        CHECK(item != NULL, "File > New exists");
        CHECK(key && !strcmp(key, "n"), "File > New keeps its n key equivalent");
        CHECK(action && !strcmp(action, "newDocument:"),
              "File > New keeps its action");
    }
    {
        void *edit = menu ? find_item(menu, "Edit") : NULL;
        void *edit_menu = edit ? call0(edit, "submenu") : NULL;
        void *undo = edit_menu ? find_item(edit_menu, "Undo") : NULL;
        const char *key = undo ? cstr(call0(undo, "keyEquivalent")) : NULL;
        CHECK(undo != NULL, "Edit > Undo exists");
        CHECK(key && !strcmp(key, "z"), "Edit > Undo keeps its z key equivalent");
    }

    /* ---- scene storyboard: view controller + view nib ---- */
    bundle_cls = get_class("NSBundle");
    bundle = bundle_cls ? call0(bundle_cls, "mainBundle") : NULL;
    CHECK(bundle != NULL, "main bundle resolves the app resources");
    sb_cls = get_class("NSStoryboard");
    CHECK(sb_cls != NULL, "NSStoryboard is registered");
    sb = call2(sb_cls, "storyboardWithName:bundle:", mkstr("Main"), bundle);
    CHECK(sb != NULL, "Main.storyboardc loads");
    sb = call2(sb_cls, "storyboardWithName:bundle:",
               mkstr("PrintPanelAccessory"), bundle);
    CHECK(sb != NULL, "PrintPanelAccessory.storyboardc loads");
    {
        void *vc = sb ? call0(sb, "instantiateInitialController") : NULL;
        void *view = vc ? call0(vc, "view") : NULL;
        CHECK(vc != NULL, "initial controller instantiated from its scene nib");
        CHECK(vc && !strcmp(cls_name(vc), "NSViewController"),
              "scene controller is an NSViewController");
        CHECK(view != NULL, "controller loads its view nib");
        CHECK(view && count_views(view, 0) > 3, "view tree built from the nib");
        CHECK(view && has_text_field(view, 0),
              "nib text fields carry their string values");
        CHECK(view && num(call0(call0(view, "subviews"), "count")) > 0,
              "root view has subviews");
    }

    /* ---- a window nib: Sparkle's SUUpdateAlert ---- */
    {
        signed char (*load)(const char *, void *, void **, void **) =
            (signed char (*)(const char *, void *, void **, void **))
                sym(appkit_lib, "plsr_nib_load");
        void *top = NULL, *controller = NULL, *window = NULL;
        snprintf(path, sizeof path,
                 "%s/Contents/Frameworks/Sparkle.framework/Versions/B/"
                 "Resources/SUUpdateAlert.nib",
                 argv[4]);
        CHECK(load(path, NULL, &top, &controller) != 0,
              "SUUpdateAlert.nib decodes");
        if (top)
            for (long i = 0; i < num(call0(top, "count")); i++) {
                void *o = call1(top, "objectAtIndex:", (void *)(intptr_t)i);
                if (!strcmp(cls_name(o), "NSWindow"))
                    window = o;
            }
        CHECK(window != NULL, "NSWindowTemplate became an NSWindow");
        CHECK(window && call0(window, "title") != NULL,
              "window title decoded");
        CHECK(window && call0(window, "contentView") != NULL,
              "window content view decoded");
        CHECK(window && count_views(call0(window, "contentView"), 0) > 3,
              "window content view built its subviews");
        {
            void *content = window ? call0(window, "contentView") : NULL;
            R wf = frame_of(window), cf = frame_of(content);
            CHECK(content && cf.w == wf.w && cf.h == wf.h,
                  "content view is sized to the window");
            CHECK(content && count_autoresizing(content, 0) > 0,
                  "nib views carry their autoresizing masks");
            {
                void *target = content ? find_masked_child(content) : NULL;
                R before = frame_of(target);
                unsigned long m = autoresizing_of(target);
                int nx = ((m & 1u) ? 1 : 0) + ((m & 2u) ? 1 : 0) +
                         ((m & 4u) ? 1 : 0);
                int ny = ((m & 8u) ? 1 : 0) + ((m & 16u) ? 1 : 0) +
                         ((m & 32u) ? 1 : 0);
                double ex_dx = 0, ex_dw = 0, ex_dy = 0, ex_dh = 0;
                R after;
                if (getenv("PLSR_NIB_TEST_TRACE")) {
                    void *subs = call0(content, "subviews");
                    long n = num(call0(subs, "count"));
                    for (long i = 0; i < n; i++) {
                        void *c = call1(subs, "objectAtIndex:",
                                        (void *)(intptr_t)i);
                        R f = frame_of(c);
                        fprintf(stderr, "child %ld %s mask=%lu %.0fx%.0f\n", i,
                                cls_name(c), autoresizing_of(c), f.w, f.h);
                    }
                }
                /* A flexible min margin moves the origin, a flexible size
                 * grows the view, and a flexible max margin just absorbs. */
                if (m & 2u)
                    ex_dw = nx ? 120.0 / nx : 0.0;
                if (m & 1u)
                    ex_dx = nx ? 120.0 / nx : 0.0;
                if (m & 16u)
                    ex_dh = ny ? 80.0 / ny : 0.0;
                if (m & 8u)
                    ex_dy = ny ? 80.0 / ny : 0.0;
                set_frame_size(content, cf.w + 120.0, cf.h + 80.0);
                after = frame_of(target);
                if (getenv("PLSR_NIB_TEST_TRACE"))
                    fprintf(stderr,
                            "target mask=%lu before=%.1f,%.1f %.1fx%.1f after=%.1f,%.1f %.1fx%.1f\n",
                            m, before.x, before.y, before.w, before.h, after.x,
                            after.y, after.w, after.h);
                CHECK(target && m && nx + ny > 0 &&
                          fabs((after.x - before.x) - ex_dx) < 0.01 &&
                          fabs((after.w - before.w) - ex_dw) < 0.01 &&
                          fabs((after.y - before.y) - ex_dy) < 0.01 &&
                          fabs((after.h - before.h) - ex_dh) < 0.01,
                      "nib frame follows the autoresizing mask");
            }
        }
    }

    /* ---- controller presentation and segues ---- */
    {
        void *src_sb = call2(sb_cls, "storyboardWithName:bundle:",
                             mkstr("ShortcutTreeView"), bundle);
        void *dst_sb = call2(sb_cls, "storyboardWithName:bundle:",
                             mkstr("MultipleReplaceView"), bundle);
        void *src = src_sb ? call0(src_sb, "instantiateInitialController") : NULL;
        void *dst = dst_sb ? call0(dst_sb, "instantiateInitialController") : NULL;
        CHECK(src && dst, "segue scenes instantiate from their storyboards");
        if (src && dst) {
            void *pv, *win;
            CHECK(call0(src, "presentingViewController") == NULL,
                  "controller starts with no presenter");

            call1(src, "presentViewControllerAsModalWindow:", dst);
            pv = call0(src, "presentedViewControllers");
            CHECK(pv && num(call0(pv, "count")) == 1 &&
                      call1(pv, "objectAtIndex:", NULL) == dst,
                  "modal presentation records the presented controller");
            CHECK(call0(dst, "presentingViewController") == src,
                  "presented controller points back at its presenter");
            win = call0(call0(dst, "view"), "window");
            CHECK(win && (signed char)(intptr_t)call0(win, "isVisible"),
                  "modal presentation shows the destination window");
            call1(src, "dismissViewController:", dst);
            pv = call0(src, "presentedViewControllers");
            CHECK(pv && num(call0(pv, "count")) == 0 &&
                      call0(dst, "presentingViewController") == NULL,
                  "dismissal clears the presentation");
            CHECK(!win || !(signed char)(intptr_t)call0(win, "isVisible"),
                  "dismissal orders the window out");

            call1(src, "presentViewControllerAsSheet:", dst);
            pv = call0(src, "presentedViewControllers");
            CHECK(pv && num(call0(pv, "count")) == 1,
                  "sheet presentation records the presented controller");
            call1(src, "dismissViewController:", dst);

            present_popover(src, dst, call0(src, "view"));
            pv = call0(src, "presentedViewControllers");
            CHECK(pv && num(call0(pv, "count")) == 1,
                  "popover presentation records the presented controller");
            call1(src, "dismissViewController:", dst);

            {
                void *segue_cls = get_class("NSStoryboardSegue");
                void *segue = segue_cls
                    ? call4(segue_cls,
                            "segueWithIdentifier:source:destination:performHandler:",
                            mkstr("showDetails"), src, dst, NULL)
                    : NULL;
                CHECK(segue != NULL,
                      "NSStoryboardSegue builds with source and destination");
                CHECK(segue && call0(segue, "identifier") &&
                          !strcmp(cstr(call0(segue, "identifier")),
                                  "showDetails") &&
                          call0(segue, "sourceController") == src &&
                          call0(segue, "destinationController") == dst,
                      "segue keeps its identifier, source and destination");
                if (segue) {
                    call0(segue, "perform");
                    pv = call0(src, "presentedViewControllers");
                    CHECK(pv && num(call0(pv, "count")) == 1,
                          "performing a segue presents its destination");
                    call1(src, "dismissViewController:", dst);
                }
            }

            call2(src, "performSegueWithIdentifier:sender:", mkstr("missing"),
                  NULL);
            pv = call0(src, "presentedViewControllers");
            CHECK(pv && num(call0(pv, "count")) == 0,
                  "unknown segue identifier performs nothing");
            CHECK((signed char)(intptr_t)call2(
                      src, "shouldPerformSegueWithIdentifier:sender:",
                      mkstr("missing"), NULL),
                  "shouldPerformSegueWithIdentifier defaults to YES");
        }
    }

    printf(failures ? "\n%d FAILURE(S)\n" : "\nPASS: nib/storyboard slice\n",
           failures);
    return failures ? 1 : 0;
}

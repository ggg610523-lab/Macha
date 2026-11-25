/*
 * Host test for the clean-room AppKit slice.
 *
 * Loads libplsr-objc, libplsr-foundation and libplsr-appkit, then drives the
 * classes through the same entry points a Mach-O app uses: objc_getClass plus
 * plsr_objc_lookup_imp.  Checks the application singleton, a window with a
 * content view, the view tree, a button, a label, a menu and a toolbar.
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { double x, y, w, h; } rect;

static void *(*get_class)(const char *);
static void *(*lookup_imp)(void *, const char *);

static int failures;

#define CHECK(cond, what)                                                     \
    do {                                                                      \
        if (cond) {                                                           \
            printf("ok   %s\n", what);                                        \
        } else {                                                              \
            printf("FAIL %s\n", what);                                        \
            failures++;                                                       \
        }                                                                     \
    } while (0)

static void *call0(void *obj, const char *sel)
{
    void *(*fn)(void *, const char *);
    if (!obj)
        return NULL;
    fn = (void *(*)(void *, const char *))lookup_imp(obj, sel);
    return fn ? fn(obj, sel) : NULL;
}

static void *call1(void *obj, const char *sel, void *a)
{
    void *(*fn)(void *, const char *, void *);
    if (!obj)
        return NULL;
    fn = (void *(*)(void *, const char *, void *))lookup_imp(obj, sel);
    return fn ? fn(obj, sel, a) : NULL;
}

static void *call3(void *obj, const char *sel, void *a, void *b, void *c)
{
    void *(*fn)(void *, const char *, void *, void *, void *);
    if (!obj)
        return NULL;
    fn = (void *(*)(void *, const char *, void *, void *, void *))lookup_imp(
        obj, sel);
    return fn ? fn(obj, sel, a, b, c) : NULL;
}

static const char *utf8(void *s)
{
    const char *(*fn)(void *, const char *);
    if (!s)
        return NULL;
    fn = (const char *(*)(void *, const char *))lookup_imp(s, "UTF8String");
    return fn ? fn(s, "UTF8String") : NULL;
}

static void *nsstr(const char *text)
{
    void *cls = get_class("NSString");
    void *(*fn)(void *, const char *, const char *);
    if (!cls)
        return NULL;
    fn = (void *(*)(void *, const char *, const char *))lookup_imp(
        cls, "stringWithUTF8String:");
    return fn ? fn(cls, "stringWithUTF8String:", text) : NULL;
}

static long count_of(void *array)
{
    void *v = call0(array, "count");
    return (long)(intptr_t)v;
}

int main(int argc, char **argv)
{
    void *objc, *found, *appkit;
    void *app_cls, *win_cls, *view_cls, *button_cls, *field_cls, *menu_cls;
    void *toolbar_cls;
    void *app, *window, *view, *child, *button, *label, *menu, *toolbar;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 4) {
        fprintf(stderr, "usage: %s libplsr-objc.so libplsr-foundation.so "
                        "libplsr-appkit.so\n", argv[0]);
        return 2;
    }
    objc = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
    if (!objc) {
        fprintf(stderr, "dlopen %s: %s\n", argv[1], dlerror());
        return 2;
    }
    found = dlopen(argv[2], RTLD_NOW | RTLD_GLOBAL);
    if (!found) {
        fprintf(stderr, "dlopen %s: %s\n", argv[2], dlerror());
        return 2;
    }
    appkit = dlopen(argv[3], RTLD_NOW | RTLD_GLOBAL);
    if (!appkit) {
        fprintf(stderr, "dlopen %s: %s\n", argv[3], dlerror());
        return 2;
    }
    /* The runtime exports the Mach-O spelling of objc_getClass. */
    get_class = (void *(*)(const char *))dlsym(objc, "_objc_getClass");
    if (!get_class)
        get_class = (void *(*)(const char *))dlsym(objc, "objc_getClass");
    lookup_imp = (void *(*)(void *, const char *))dlsym(objc,
                                                        "plsr_objc_lookup_imp");
    if (!get_class || !lookup_imp) {
        fprintf(stderr, "objc runtime symbols missing (getClass=%p lookup=%p)\n",
                (void *)get_class, (void *)lookup_imp);
        return 2;
    }

    app_cls = get_class("NSApplication");
    win_cls = get_class("NSWindow");
    view_cls = get_class("NSView");
    button_cls = get_class("NSButton");
    field_cls = get_class("NSTextField");
    menu_cls = get_class("NSMenu");
    toolbar_cls = get_class("NSToolbar");
    CHECK(app_cls && win_cls && view_cls && button_cls && field_cls && menu_cls &&
              toolbar_cls,
          "AppKit classes registered");

    app = call0(app_cls, "sharedApplication");
    CHECK(app != NULL, "+[NSApplication sharedApplication]");
    CHECK(call0(app_cls, "sharedApplication") == app,
          "sharedApplication is a singleton");

    window = call0(win_cls, "alloc");
    CHECK(window != NULL, "+[NSWindow alloc]");
    if (window) {
        void *(*init)(void *, const char *, rect, unsigned long, unsigned long,
                      signed char);
        rect r = { 0, 0, 900, 620 };
        init = (void *(*)(void *, const char *, rect, unsigned long,
                          unsigned long, signed char))lookup_imp(
            window, "initWithContentRect:styleMask:backing:defer:");
        window = init ? init(window, "initWithContentRect:styleMask:backing:defer:",
                             r, 0xf, 2, 0) : NULL;
    }
    CHECK(window != NULL, "-[NSWindow initWithContentRect:styleMask:backing:defer:]");
    CHECK(call0(window, "isVisible") == NULL, "window starts hidden");
    call1(window, "setTitle:", nsstr("Untitled"));
    CHECK(utf8(call0(window, "title")) && !strcmp(utf8(call0(window, "title")),
                                                  "Untitled"),
          "window title round-trips");

    view = call0(view_cls, "alloc");
    CHECK(view != NULL, "+[NSView alloc]");
    call1(window, "setContentView:", view);
    CHECK(call0(window, "contentView") == view, "contentView set/get");

    child = call0(view_cls, "alloc");
    call1(view, "addSubview:", child);
    CHECK(count_of(call0(view, "subviews")) == 1, "addSubview -> subviews count 1");
    CHECK(call0(child, "superview") == view, "subview knows its superview");

    button = call3(button_cls, "buttonWithTitle:target:action:", nsstr("Run"),
                   NULL, NULL);
    CHECK(button != NULL, "+[NSButton buttonWithTitle:target:action:]");
    CHECK(utf8(call0(button, "title")) &&
              !strcmp(utf8(call0(button, "title")), "Run"),
          "button title round-trips");

    label = call1(field_cls, "labelWithString:", nsstr("hello"));
    CHECK(label != NULL, "+[NSTextField labelWithString:]");
    CHECK(utf8(call0(label, "stringValue")) &&
              !strcmp(utf8(call0(label, "stringValue")), "hello"),
          "label stringValue round-trips");
    call1(label, "setStringValue:", nsstr("second"));
    CHECK(utf8(call0(label, "stringValue")) &&
              !strcmp(utf8(call0(label, "stringValue")), "second"),
          "setStringValue round-trips");

    menu = call0(menu_cls, "alloc");
    menu = call1(menu, "initWithTitle:", nsstr("File"));
    call3(menu, "addItemWithTitle:action:keyEquivalent:", nsstr("Open"), NULL,
          nsstr("o"));
    CHECK((long)(intptr_t)call0(menu, "numberOfItems") == 1, "menu item added");
    call1(app, "setMainMenu:", menu);
    CHECK(call0(app, "mainMenu") == menu, "mainMenu set/get");

    toolbar = call0(toolbar_cls, "alloc");
    toolbar = call1(toolbar, "initWithIdentifier:", nsstr("MainToolbar"));
    CHECK(toolbar != NULL, "-[NSToolbar initWithIdentifier:]");
    call1(window, "setToolbar:", toolbar);
    CHECK(call0(window, "toolbar") == toolbar, "window toolbar set/get");

    printf(failures ? "FAIL: %d AppKit check(s)\n" : "PASS: AppKit slice\n",
           failures);
    return failures ? 1 : 0;
}

/*
 * NSView layout, drawing and event delivery through the host surface.
 *
 * Builds a window with a box, a button and an editable text field, attaches a
 * software surface, lays the tree out, renders it, resizes it (autoresizing),
 * clicks the button, types into the field, and fires a Command-key equivalent
 * from the main menu.  All input goes through the surface ABI, exactly as a
 * host window would deliver it.
 */
#define _GNU_SOURCE
#include "appkit_surface.h"
#include "objc_abi.h"

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    double x, y, w, h;
} R;

static void *objc_lib, *foundation_lib, *appkit_lib;
static void *(*get_class)(const char *);
static void *(*lookup)(void *, const char *);
static const char *(*class_name)(void *);
static int failures;
static int clicked;
static int new_doc;

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

static void *call3(void *o, const char *s, void *a, void *b, void *c)
{
    void *(*fn)(void *, const char *, void *, void *, void *);
    if (!o)
        return NULL;
    fn = (void *(*)(void *, const char *, void *, void *, void *))lookup(o, s);
    return fn ? fn(o, s, a, b, c) : NULL;
}

static void *call_rect(void *o, const char *s, R r, unsigned long mask,
                       unsigned long backing, signed char defer)
{
    void *(*fn)(void *, const char *, R, unsigned long, unsigned long,
                signed char);
    if (!o)
        return NULL;
    fn = (void *(*)(void *, const char *, R, unsigned long, unsigned long,
                    signed char))lookup(o, s);
    return fn ? fn(o, s, r, mask, backing, defer) : NULL;
}

static void call_rect1(void *o, const char *s, R r)
{
    void (*fn)(void *, const char *, R);
    if (!o)
        return;
    fn = (void (*)(void *, const char *, R))lookup(o, s);
    if (fn)
        fn(o, s, r);
}

static R frame_of(void *o)
{
    R (*fn)(void *, const char *);
    R r = { 0, 0, 0, 0 };
    if (!o)
        return r;
    fn = (R(*)(void *, const char *))lookup(o, "frame");
    return fn ? fn(o, "frame") : r;
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

/* ---- a real ObjC target the control and menu actions can call ---------- */

static void target_clicked(void *self, const char *sel, void *sender)
{
    (void)self;
    (void)sel;
    (void)sender;
    clicked++;
}

static void target_new_doc(void *self, const char *sel, void *sender)
{
    (void)self;
    (void)sel;
    (void)sender;
    new_doc++;
}

typedef struct {
    uint32_t entsize_flags;
    uint32_t count;
    plsr_method methods[4];
} target_methods_t;

static target_methods_t target_methods = {
    24, 2, {
        { "clicked:", "", (void *)target_clicked },
        { "newDoc:", "", (void *)target_new_doc },
    }
};

static plsr_class_ro target_ro = {
    0, 0, 16, 0, NULL, "SurfaceTarget", (plsr_method_list *)&target_methods,
    NULL, NULL, NULL, NULL
};
static plsr_class_ro target_meta_ro = {
    PLSR_RO_META, 0, 16, 0, NULL, "SurfaceTarget",
    (plsr_method_list *)&target_methods, NULL, NULL, NULL, NULL
};

static plsr_class target_class;
static plsr_class target_meta;

static void register_target(void)
{
    void *nsobj = sym(foundation_lib, "_OBJC_CLASS_$_NSObject");
    void *nsobj_meta = sym(foundation_lib, "_OBJC_METACLASS_$_NSObject");
    void (*reg)(plsr_class *) =
        (void (*)(plsr_class *))sym(objc_lib, "plsr_objc_register_class");
    target_class.isa = &target_meta;
    target_class.superclass = (void *)nsobj;
    target_class.data = (uintptr_t)&target_ro;
    target_meta.isa = &target_meta;
    target_meta.superclass = (void *)nsobj_meta;
    target_meta.data = (uintptr_t)&target_meta_ro;
    reg(&target_class);
}

int main(int argc, char **argv)
{
    void *target, *win, *content, *box, *button, *field, *app, *menu, *item;
    plsr_surface *surface;
    R r;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 4) {
        fprintf(stderr, "usage: %s libobjc libfoundation libappkit\n", argv[0]);
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

    register_target();
    target = calloc(1, 16);
    *(void **)target = &target_class;

    /* ---- window and view tree ---- */
    win = call0(get_class("NSWindow"), "alloc");
    r = (R){ 0, 0, 480, 320 };
    win = call_rect(win, "initWithContentRect:styleMask:backing:defer:", r, 15,
                    2, 0);
    CHECK(win != NULL, "window created");
    content = call0(get_class("NSView"), "alloc");
    call_rect1(content, "initWithFrame:", r);
    call1(win, "setContentView:", content);
    CHECK(call0(win, "contentView") == content, "content view installed");

    box = call0(get_class("NSBox"), "alloc");
    call_rect1(box, "initWithFrame:", r);
    call1(content, "addSubview:", box);

    button = call3(get_class("NSButton"), "buttonWithTitle:target:action:",
                   mkstr("Run"), target, (void *)"clicked:");
    r = (R){ 20, 20, 120, 32 };
    call_rect1(button, "setFrame:", r);
    call1(button, "setAutoresizingMask:", (void *)2);
    call1(content, "addSubview:", button);

    field = call0(get_class("NSTextField"), "alloc");
    r = (R){ 20, 70, 220, 24 };
    call_rect1(field, "initWithFrame:", r);
    call1(field, "setEditable:", (void *)1);
    call1(field, "setStringValue:", mkstr(""));
    call1(content, "addSubview:", field);

    CHECK(!strcmp(cls_name(button), "NSButton"), "button is an NSButton");
    CHECK(!strcmp(cls_name(field), "NSTextField"), "field is an NSTextField");

    /* ---- surface: layout ---- */
    surface = plsr_surface_new(480, 320);
    CHECK(surface != NULL, "surface created");
    plsr_surface_attach_window(surface, win);
    plsr_surface_layout(surface);
    CHECK(frame_of(content).w == 480 && frame_of(content).h == 320,
          "content view follows the surface size");
    CHECK(frame_of(button).x == 20 && frame_of(button).w == 120,
          "button keeps its nib frame");

    /* ---- hit testing ---- */
    CHECK(plsr_surface_hit_test(surface, 80, 320 - 36) == button,
          "hit test finds the button");
    CHECK(plsr_surface_hit_test(surface, 400, 40) == box,
          "hit test falls through to the box");

    /* ---- drawing ---- */
    plsr_surface_render(surface);
    {
        unsigned ink = plsr_surface_ink_pixels(surface, 0xf2f2f2ffu);
        unsigned colors = plsr_surface_distinct_colors(surface);
        printf("surface: %ux%u, %u ink pixels, %u colours\n",
               plsr_surface_width(surface), plsr_surface_height(surface), ink,
               colors);
        CHECK(ink > 500, "surface contains drawn controls");
        CHECK(colors >= 3, "surface uses several colours");
        CHECK(plsr_surface_needs_display(surface) == 0,
              "render clears the dirty flag");
        CHECK(plsr_surface_write_ppm(surface, "/tmp/appkit_surface.ppm") == 1,
              "surface written as PPM");
    }

    /* ---- autoresizing ---- */
    plsr_surface_resize(surface, 600, 320);
    plsr_surface_layout(surface);
    CHECK(frame_of(content).w == 600, "content view grows with the surface");
    CHECK(frame_of(button).w == 240, "width-sizable button absorbs the delta");
    CHECK(frame_of(field).w == 220, "fixed-width field keeps its size");

    /* ---- mouse: click the button ---- */
    plsr_surface_mouse(surface, PLSR_MOUSE_DOWN, 140, 320 - 36, 0);
    plsr_surface_mouse(surface, PLSR_MOUSE_UP, 140, 320 - 36, 0);
    CHECK(clicked == 1, "mouse down/up performs the button action");

    /* ---- mouse + keys: edit the text field ---- */
    plsr_surface_mouse(surface, PLSR_MOUSE_DOWN, 100, 320 - 82, 0);
    plsr_surface_mouse(surface, PLSR_MOUSE_UP, 100, 320 - 82, 0);
    CHECK(call0(win, "firstResponder") == field,
          "clicking the field makes it first responder");
    plsr_surface_key(surface, 4, "h", 0);
    plsr_surface_key(surface, 5, "i", 0);
    CHECK(cstr(call0(field, "stringValue")) &&
              !strcmp(cstr(call0(field, "stringValue")), "hi"),
          "typed characters reach the field");
    plsr_surface_key(surface, 51, "", 0);
    CHECK(cstr(call0(field, "stringValue")) &&
              !strcmp(cstr(call0(field, "stringValue")), "h"),
          "delete key edits the field");

    /* ---- menu key equivalent ---- */
    app = call0(get_class("NSApplication"), "sharedApplication");
    menu = call0(get_class("NSMenu"), "alloc");
    menu = call1(menu, "initWithTitle:", mkstr("Main"));
    item = call3(menu, "addItemWithTitle:action:keyEquivalent:", mkstr("New"),
                 (void *)"newDoc:", mkstr("n"));
    CHECK(item != NULL, "menu item added");
    call1(item, "setTarget:", target);
    call1(app, "setMainMenu:", menu);
    plsr_surface_key(surface, 45, "n", PLSR_MOD_COMMAND);
    CHECK(new_doc == 1, "Command-N reaches the menu item's target");

    plsr_surface_free(surface);
    printf(failures ? "\n%d FAILURE(S)\n" : "\nPASS: view layout, drawing and input\n",
           failures);
    return failures ? 1 : 0;
}

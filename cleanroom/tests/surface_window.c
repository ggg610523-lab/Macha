/*
 * Host window for the clean-room AppKit surface.
 *
 * Opens an X11 window, attaches a real NSWindow from libplsr-appkit to a
 * plsr_surface, renders the view tree and blits the RGBA pixels into the
 * window.  Without an app bundle it builds a small demo scene; with one it
 * instantiates the PrintPanelAccessory scene from that bundle's storyboard.
 *
 *   surface_window libobjc.so libfoundation.so libappkit.so \
 *                  [app-bundle] [--capture out.ppm]
 *
 * --capture grabs the window's pixels after the first expose and exits, which
 * is how the visual check is recorded without a screenshot tool.
 */
#define _GNU_SOURCE
#include "appkit_surface.h"
#include "objc_abi.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    double x, y, w, h;
} R;

static void *objc_lib, *foundation_lib, *appkit_lib;
static void *(*get_class)(const char *);
static void *(*lookup)(void *, const char *);

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

static void *add_field(void *parent, R r, const char *text, int editable)
{
    void *field = call0(get_class("NSTextField"), "alloc");
    call_rect1(field, "initWithFrame:", r);
    call1(field, "setStringValue:", mkstr(text));
    call1(field, "setEditable:", editable ? (void *)1 : NULL);
    call1(parent, "addSubview:", field);
    return field;
}

static void *add_button(void *parent, R r, const char *title)
{
    void *button = call3(get_class("NSButton"),
                         "buttonWithTitle:target:action:", mkstr(title), NULL,
                         NULL);
    call_rect1(button, "setFrame:", r);
    call1(parent, "addSubview:", button);
    return button;
}

static void *build_demo_scene(int *w, int *h)
{
    R r = { 0, 0, 560, 360 };
    void *win = call0(get_class("NSWindow"), "alloc");
    void *content;

    win = call_rect(win, "initWithContentRect:styleMask:backing:defer:", r, 15,
                    2, 0);
    if (!win)
        return NULL;
    content = call0(get_class("NSView"), "alloc");
    call_rect1(content, "initWithFrame:", r);
    call1(win, "setContentView:", content);
    call1(win, "setTitle:", mkstr("Clean-room AppKit surface"));

    {
        void *box = call0(get_class("NSBox"), "alloc");
        call_rect1(box, "initWithFrame:", r);
        call1(content, "addSubview:", box);
    }
    add_field(content, (R){ 24, 300, 400, 24 },
              "Clean-room AppKit surface", 0);
    add_field(content, (R){ 24, 264, 460, 22 },
              "Rendered through plsr_surface_* into a real X11 window", 0);
    add_field(content, (R){ 24, 190, 300, 26 }, "editable text field", 1);
    add_button(content, (R){ 24, 30, 120, 32 }, "Run");
    add_button(content, (R){ 160, 30, 120, 32 }, "Cancel");
    *w = 560;
    *h = 360;
    return win;
}

static void *build_storyboard_scene(const char *app, int *w, int *h)
{
    void *bundle, *sb, *vc, *view, *win;
    R r;

    setenv("PLSR_APP_BUNDLE", app, 1);
    bundle = call0(get_class("NSBundle"), "mainBundle");
    sb = call2(get_class("NSStoryboard"), "storyboardWithName:bundle:",
               mkstr("PrintPanelAccessory"), bundle);
    vc = sb ? call0(sb, "instantiateInitialController") : NULL;
    view = vc ? call0(vc, "view") : NULL;
    if (!view)
        return NULL;
    r = frame_of(view);
    if (r.w < 100.0 || r.h < 60.0)
        return NULL;
    win = call0(get_class("NSWindow"), "alloc");
    win = call_rect(win, "initWithContentRect:styleMask:backing:defer:", r, 15,
                    2, 0);
    if (!win)
        return NULL;
    call1(win, "setContentView:", view);
    call1(win, "setContentViewController:", vc);
    call1(win, "setTitle:", mkstr("Clean-room AppKit surface - scene"));
    *w = (int)(r.w + 0.5);
    *h = (int)(r.h + 0.5);
    return win;
}

static void blit(Display *dpy, Window win, GC gc, plsr_surface *surface)
{
    int w = plsr_surface_width(surface);
    int h = plsr_surface_height(surface);
    const unsigned char *px = plsr_surface_pixels(surface);
    unsigned char *data = malloc((size_t)w * (size_t)h * 4);
    XImage *img;
    if (!data)
        return;
    for (int i = 0; i < w * h; i++) {
        data[i * 4 + 0] = px[i * 4 + 2];
        data[i * 4 + 1] = px[i * 4 + 1];
        data[i * 4 + 2] = px[i * 4 + 0];
        data[i * 4 + 3] = 0;
    }
    img = XCreateImage(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                       DefaultDepth(dpy, DefaultScreen(dpy)), ZPixmap, 0,
                       (char *)data, w, h, 32, 0);
    if (!img) {
        free(data);
        return;
    }
    XPutImage(dpy, win, gc, img, 0, 0, 0, 0, w, h);
    XFlush(dpy);
    img->data = NULL;
    XDestroyImage(img);
    free(data);
}

static int capture_window(Display *dpy, Window win, int w, int h,
                          const char *path)
{
    XImage *img = XGetImage(dpy, win, 0, 0, w, h, AllPlanes, ZPixmap);
    FILE *f;
    unsigned long rm, gm, bm;
    int rs, gs, bs;
    if (!img)
        return 0;
    f = fopen(path, "wb");
    if (!f) {
        XDestroyImage(img);
        return 0;
    }
    rm = img->red_mask ? img->red_mask : 0xff0000;
    gm = img->green_mask ? img->green_mask : 0x00ff00;
    bm = img->blue_mask ? img->blue_mask : 0x0000ff;
    rs = __builtin_ctzl(rm);
    gs = __builtin_ctzl(gm);
    bs = __builtin_ctzl(bm);
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            unsigned long p = XGetPixel(img, x, y);
            unsigned char rgb[3] = {
                (unsigned char)((p & rm) >> rs),
                (unsigned char)((p & gm) >> gs),
                (unsigned char)((p & bm) >> bs),
            };
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
    XDestroyImage(img);
    return 1;
}

int main(int argc, char **argv)
{
    const char *app = NULL, *capture = NULL;
    int w = 560, h = 360;
    void *win;
    plsr_surface *surface;
    Display *dpy;
    Window xwin;
    GC gc;
    Atom wm_delete;
    int drawn = 0;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 4) {
        fprintf(stderr,
                "usage: %s libobjc libfoundation libappkit [app-bundle] "
                "[--capture out.ppm]\n",
                argv[0]);
        return 2;
    }
    objc_lib = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
    foundation_lib = dlopen(argv[2], RTLD_NOW | RTLD_GLOBAL);
    appkit_lib = dlopen(argv[3], RTLD_NOW | RTLD_GLOBAL);
    if (!objc_lib || !foundation_lib || !appkit_lib) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return 2;
    }
    get_class = (void *(*)(const char *))sym(objc_lib, "_objc_getClass");
    lookup = (void *(*)(void *, const char *))sym(objc_lib,
                                                  "plsr_objc_lookup_imp");

    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--capture")) {
            if (i + 1 < argc)
                capture = argv[++i];
        } else {
            app = argv[i];
        }
    }

    win = app ? build_storyboard_scene(app, &w, &h) : NULL;
    if (!win) {
        if (app)
            fprintf(stderr, "scene load failed, using the demo scene\n");
        win = build_demo_scene(&w, &h);
    }
    if (!win) {
        fprintf(stderr, "no NSWindow\n");
        return 1;
    }

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "cannot open display %s\n",
                getenv("DISPLAY") ? getenv("DISPLAY") : "(null)");
        return 2;
    }
    xwin = XCreateSimpleWindow(dpy, RootWindow(dpy, DefaultScreen(dpy)), 0, 0,
                               (unsigned)w, (unsigned)h, 0,
                               BlackPixel(dpy, DefaultScreen(dpy)),
                               WhitePixel(dpy, DefaultScreen(dpy)));
    XStoreName(dpy, xwin, "plsr surface");
    XSelectInput(dpy, xwin, ExposureMask | StructureNotifyMask);
    wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, xwin, &wm_delete, 1);
    gc = XCreateGC(dpy, xwin, 0, NULL);
    XMapWindow(dpy, xwin);
    XFlush(dpy);

    surface = plsr_surface_new(w, h);
    if (!surface) {
        fprintf(stderr, "no surface\n");
        return 1;
    }
    plsr_surface_attach_window(surface, win);
    plsr_surface_layout(surface);
    plsr_surface_render(surface);
    printf("surface %dx%d, %u ink pixels, %u colours\n", w, h,
           plsr_surface_ink_pixels(surface, 0xf2f2f2ffu),
           plsr_surface_distinct_colors(surface));
    if (!plsr_surface_write_ppm(surface, "/tmp/plsr_surface.ppm"))
        fprintf(stderr, "could not write /tmp/plsr_surface.ppm\n");

    for (;;) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        if (ev.type == Expose) {
            if (ev.xexpose.count != 0)
                continue;
            plsr_surface_render(surface);
            blit(dpy, xwin, gc, surface);
            drawn = 1;
        } else if (ev.type == ConfigureNotify) {
            if (ev.xconfigure.width != w || ev.xconfigure.height != h) {
                w = ev.xconfigure.width;
                h = ev.xconfigure.height;
                plsr_surface_resize(surface, w, h);
                plsr_surface_layout(surface);
                plsr_surface_render(surface);
                blit(dpy, xwin, gc, surface);
            }
        } else if (ev.type == ClientMessage &&
                   (Atom)ev.xclient.data.l[0] == wm_delete) {
            break;
        }
        if (drawn && capture) {
            XSync(dpy, False);
            usleep(250000);
            if (capture_window(dpy, xwin, w, h, capture))
                printf("captured %dx%d window to %s\n", w, h, capture);
            else
                fprintf(stderr, "capture failed\n");
            break;
        }
    }

    plsr_surface_free(surface);
    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, xwin);
    XCloseDisplay(dpy);
    return 0;
}

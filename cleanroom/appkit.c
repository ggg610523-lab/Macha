/*
 * Clean-room AppKit core.
 *
 * The shipped AppKit (12,002 decompiled functions) is the reference; this is
 * the first behavioural slice of its object graph: application, windows, the
 * view tree, buttons, text fields/views, menus and toolbars.  Every class is
 * registered with libplsr-objc so a Mach-O app that imports
 * _OBJC_CLASS_$_NSWindow and friends binds to these definitions.
 *
 * Layout follows Foundation's objects: isa first, then the class's own fields.
 * Strings are read through -UTF8String and created through
 * +[NSString stringWithUTF8String:], so Foundation remains the string owner.
 */
#define _GNU_SOURCE
#include "objc_abi.h"
#include "appkit_surface.h"

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void plsr_objc_register_class(plsr_class *c);
void *plsr_objc_lookup_imp(void *obj, const char *sel);
void *objc_getClass(const char *name) __asm__("_objc_getClass");
void CFRunLoopRun(void) __asm__("_CFRunLoopRun");
long CFRunLoopRunInMode(void *mode, double seconds, unsigned char returnAfterSourceHandled)
    __asm__("_CFRunLoopRunInMode");
void *CFRunLoopGetCurrent(void) __asm__("_CFRunLoopGetCurrent");
void *CFRunLoopGetMain(void) __asm__("_CFRunLoopGetMain");
void CFRunLoopStop(void *loop) __asm__("_CFRunLoopStop");

/* ---- helpers ----------------------------------------------------------- */

typedef struct {
    uint32_t entsize_flags;
    uint32_t count;
    plsr_method methods[72];
} method_array;

#define RO(flags_, size_, name_, methods_)                                    \
    { (flags_), 0, (size_), 0, NULL, (name_), (plsr_method_list *)(methods_), \
      NULL, NULL, NULL, NULL }

typedef struct { double x, y, w, h; } rect;
typedef struct { double x, y; } point;

static void *send0(void *obj, const char *sel)
{
    void *(*fn)(void *, const char *);
    if (!obj)
        return NULL;
    fn = (void *(*)(void *, const char *))plsr_objc_lookup_imp(obj, sel);
    return fn ? fn(obj, sel) : NULL;
}

static void *send1(void *obj, const char *sel, void *a)
{
    void *(*fn)(void *, const char *, void *);
    if (!obj)
        return NULL;
    fn = (void *(*)(void *, const char *, void *))plsr_objc_lookup_imp(obj, sel);
    return fn ? fn(obj, sel, a) : NULL;
}

static void *send2(void *obj, const char *sel, void *a, void *b)
{
    void *(*fn)(void *, const char *, void *, void *);
    if (!obj)
        return NULL;
    fn = (void *(*)(void *, const char *, void *, void *))plsr_objc_lookup_imp(obj, sel);
    return fn ? fn(obj, sel, a, b) : NULL;
}

static int responds(void *obj, const char *sel)
{
    return obj && plsr_objc_lookup_imp(obj, sel) != NULL;
}

static void *send_rect4(void *obj, const char *sel, rect r, unsigned long mask,
                        unsigned long backing, signed char defer)
{
    void *(*fn)(void *, const char *, rect, unsigned long, unsigned long,
                signed char);
    if (!obj)
        return NULL;
    fn = (void *(*)(void *, const char *, rect, unsigned long, unsigned long,
                    signed char))plsr_objc_lookup_imp(obj, sel);
    return fn ? fn(obj, sel, r, mask, backing, defer) : NULL;
}

static const char *cstr(void *nsstring)
{
    const char *(*fn)(void *, const char *);
    if (!nsstring)
        return NULL;
    fn = (const char *(*)(void *, const char *))plsr_objc_lookup_imp(
        nsstring, "UTF8String");
    return fn ? fn(nsstring, "UTF8String") : NULL;
}

static void *mkstr(const char *s)
{
    void *cls = objc_getClass("NSString");
    void *(*fn)(void *, const char *, const char *);
    if (!cls)
        return NULL;
    fn = (void *(*)(void *, const char *, const char *))plsr_objc_lookup_imp(
        cls, "stringWithUTF8String:");
    return fn ? fn(cls, "stringWithUTF8String:", s ? s : "") : NULL;
}

/* A plain object whose class was declared with `size` bytes of storage. */
static void *obj_new(plsr_class *cls, size_t size)
{
    void *o = calloc(1, size);
    if (o)
        *(void **)o = cls;
    return o;
}

/* ---- software surface -------------------------------------------------- */

struct plsr_surface {
    int width;
    int height;
    unsigned char *pixels;   /* RGBA, top-left origin */
    void *window;
    int needs_display;
    plsr_rect clip[32];
    int clip_depth;
};

static plsr_surface *g_draw;
static double g_draw_x, g_draw_y, g_draw_h;
static unsigned g_fill_rgba = 0x000000ffu;

static void surface_blend(plsr_surface *s, int x, int y, unsigned rgba)
{
    unsigned char *p;
    unsigned sa, ia, r, g, b, a;
    if (!s || x < 0 || y < 0 || x >= s->width || y >= s->height)
        return;
    if (s->clip_depth > 0) {
        plsr_rect c = s->clip[s->clip_depth - 1];
        if (x < c.x || y < c.y || x >= c.x + c.width || y >= c.y + c.height)
            return;
    }
    p = s->pixels + ((size_t)y * (size_t)s->width + (size_t)x) * 4;
    sa = (rgba & 0xffu);
    if (sa == 0xffu) {
        p[0] = (unsigned char)((rgba >> 24) & 0xffu);
        p[1] = (unsigned char)((rgba >> 16) & 0xffu);
        p[2] = (unsigned char)((rgba >> 8) & 0xffu);
        p[3] = 0xff;
        return;
    }
    ia = 255u - sa;
    r = ((rgba >> 24) & 0xffu) * sa + p[0] * ia;
    g = ((rgba >> 16) & 0xffu) * sa + p[1] * ia;
    b = ((rgba >> 8) & 0xffu) * sa + p[2] * ia;
    a = sa * 255u + p[3] * ia;
    p[0] = (unsigned char)(r / 255u);
    p[1] = (unsigned char)(g / 255u);
    p[2] = (unsigned char)(b / 255u);
    p[3] = (unsigned char)(a / 255u);
}

static void surface_fill(plsr_surface *s, double x, double y, double w,
                         double h, unsigned rgba)
{
    int x0, y0, x1, y1;
    if (!s || w <= 0.0 || h <= 0.0 || (rgba & 0xffu) == 0)
        return;
    x0 = (int)floor(x);
    y0 = (int)floor(y);
    x1 = (int)ceil(x + w);
    y1 = (int)ceil(y + h);
    for (int py = y0; py < y1; py++)
        for (int px = x0; px < x1; px++)
            surface_blend(s, px, py, rgba);
}

static void surface_push_clip(plsr_surface *s, double x, double y, double w,
                              double h)
{
    if (!s)
        return;
    if (s->clip_depth >= (int)(sizeof s->clip / sizeof s->clip[0]))
        return;
    s->clip[s->clip_depth++] = (plsr_rect){ x, y, w, h };
}

/* Local (bottom-left origin) rect of the view currently being drawn. */
static void draw_rect_local(double x, double y, double w, double h,
                            unsigned rgba)
{
    if (!g_draw)
        return;
    surface_fill(g_draw, g_draw_x + x,
                 (double)g_draw->height - (g_draw_y + y + h), w, h, rgba);
}

static void draw_frame_local(double x, double y, double w, double h, double t,
                             unsigned rgba)
{
    draw_rect_local(x, y, w, t, rgba);
    draw_rect_local(x, y + h - t, w, t, rgba);
    draw_rect_local(x, y + t, t, h - 2.0 * t, rgba);
    draw_rect_local(x + w - t, y + t, t, h - 2.0 * t, rgba);
}

/* Classic 5x7 column font, bit 0 = top row, printable ASCII 0x20..0x7e. */
static const unsigned char appkit_font5x7[95][5] = {
    { 0x00, 0x00, 0x00, 0x00, 0x00 }, { 0x00, 0x00, 0x5f, 0x00, 0x00 },
    { 0x00, 0x07, 0x00, 0x07, 0x00 }, { 0x14, 0x7f, 0x14, 0x7f, 0x14 },
    { 0x24, 0x2a, 0x7f, 0x2a, 0x12 }, { 0x23, 0x13, 0x08, 0x64, 0x62 },
    { 0x36, 0x49, 0x55, 0x22, 0x50 }, { 0x00, 0x05, 0x03, 0x00, 0x00 },
    { 0x00, 0x1c, 0x22, 0x41, 0x00 }, { 0x00, 0x41, 0x22, 0x1c, 0x00 },
    { 0x08, 0x2a, 0x1c, 0x2a, 0x08 }, { 0x08, 0x08, 0x3e, 0x08, 0x08 },
    { 0x00, 0x50, 0x30, 0x00, 0x00 }, { 0x08, 0x08, 0x08, 0x08, 0x08 },
    { 0x00, 0x60, 0x60, 0x00, 0x00 }, { 0x20, 0x10, 0x08, 0x04, 0x02 },
    { 0x3e, 0x51, 0x49, 0x45, 0x3e }, { 0x00, 0x42, 0x7f, 0x40, 0x00 },
    { 0x42, 0x61, 0x51, 0x49, 0x46 }, { 0x21, 0x41, 0x45, 0x4b, 0x31 },
    { 0x18, 0x14, 0x12, 0x7f, 0x10 }, { 0x27, 0x45, 0x45, 0x45, 0x39 },
    { 0x3c, 0x4a, 0x49, 0x49, 0x30 }, { 0x01, 0x71, 0x09, 0x05, 0x03 },
    { 0x36, 0x49, 0x49, 0x49, 0x36 }, { 0x06, 0x49, 0x49, 0x29, 0x1e },
    { 0x00, 0x36, 0x36, 0x00, 0x00 }, { 0x00, 0x56, 0x36, 0x00, 0x00 },
    { 0x00, 0x08, 0x14, 0x22, 0x41 }, { 0x14, 0x14, 0x14, 0x14, 0x14 },
    { 0x41, 0x22, 0x14, 0x08, 0x00 }, { 0x02, 0x01, 0x51, 0x09, 0x06 },
    { 0x32, 0x49, 0x79, 0x41, 0x3e }, { 0x7e, 0x11, 0x11, 0x11, 0x7e },
    { 0x7f, 0x49, 0x49, 0x49, 0x36 }, { 0x3e, 0x41, 0x41, 0x41, 0x22 },
    { 0x7f, 0x41, 0x41, 0x22, 0x1c }, { 0x7f, 0x49, 0x49, 0x49, 0x41 },
    { 0x7f, 0x09, 0x09, 0x09, 0x01 }, { 0x3e, 0x41, 0x49, 0x49, 0x7a },
    { 0x7f, 0x08, 0x08, 0x08, 0x7f }, { 0x00, 0x41, 0x7f, 0x41, 0x00 },
    { 0x20, 0x40, 0x41, 0x3f, 0x01 }, { 0x7f, 0x08, 0x14, 0x22, 0x41 },
    { 0x7f, 0x40, 0x40, 0x40, 0x40 }, { 0x7f, 0x02, 0x0c, 0x02, 0x7f },
    { 0x7f, 0x04, 0x08, 0x10, 0x7f }, { 0x3e, 0x41, 0x41, 0x41, 0x3e },
    { 0x7f, 0x09, 0x09, 0x09, 0x06 }, { 0x3e, 0x41, 0x51, 0x21, 0x5e },
    { 0x7f, 0x09, 0x19, 0x29, 0x46 }, { 0x46, 0x49, 0x49, 0x49, 0x31 },
    { 0x01, 0x01, 0x7f, 0x01, 0x01 }, { 0x3f, 0x40, 0x40, 0x40, 0x3f },
    { 0x1f, 0x20, 0x40, 0x20, 0x1f }, { 0x7f, 0x20, 0x18, 0x20, 0x7f },
    { 0x63, 0x14, 0x08, 0x14, 0x63 }, { 0x07, 0x08, 0x70, 0x08, 0x07 },
    { 0x61, 0x51, 0x49, 0x45, 0x43 }, { 0x00, 0x7f, 0x41, 0x41, 0x00 },
    { 0x02, 0x04, 0x08, 0x10, 0x20 }, { 0x00, 0x41, 0x41, 0x7f, 0x00 },
    { 0x04, 0x02, 0x01, 0x02, 0x04 }, { 0x40, 0x40, 0x40, 0x40, 0x40 },
    { 0x00, 0x01, 0x02, 0x04, 0x00 }, { 0x20, 0x54, 0x54, 0x54, 0x78 },
    { 0x7f, 0x48, 0x44, 0x44, 0x38 }, { 0x38, 0x44, 0x44, 0x44, 0x20 },
    { 0x38, 0x44, 0x44, 0x48, 0x7f }, { 0x38, 0x54, 0x54, 0x54, 0x18 },
    { 0x08, 0x7e, 0x09, 0x01, 0x02 }, { 0x0c, 0x52, 0x52, 0x52, 0x3e },
    { 0x7f, 0x08, 0x04, 0x04, 0x78 }, { 0x00, 0x44, 0x7d, 0x40, 0x00 },
    { 0x20, 0x40, 0x44, 0x3d, 0x00 }, { 0x7f, 0x10, 0x28, 0x44, 0x00 },
    { 0x00, 0x41, 0x7f, 0x40, 0x00 }, { 0x7c, 0x04, 0x18, 0x04, 0x78 },
    { 0x7c, 0x08, 0x04, 0x04, 0x78 }, { 0x38, 0x44, 0x44, 0x44, 0x38 },
    { 0x7c, 0x14, 0x14, 0x14, 0x08 }, { 0x08, 0x14, 0x14, 0x18, 0x7c },
    { 0x7c, 0x08, 0x04, 0x04, 0x08 }, { 0x48, 0x54, 0x54, 0x54, 0x20 },
    { 0x04, 0x3f, 0x44, 0x40, 0x20 }, { 0x3c, 0x40, 0x40, 0x20, 0x7c },
    { 0x1c, 0x20, 0x40, 0x20, 0x1c }, { 0x3c, 0x40, 0x30, 0x40, 0x3c },
    { 0x44, 0x28, 0x10, 0x28, 0x44 }, { 0x0c, 0x50, 0x50, 0x50, 0x3c },
    { 0x44, 0x64, 0x54, 0x4c, 0x44 }, { 0x00, 0x08, 0x36, 0x41, 0x00 },
    { 0x00, 0x00, 0x7f, 0x00, 0x00 }, { 0x00, 0x41, 0x36, 0x08, 0x00 },
    { 0x08, 0x04, 0x08, 0x10, 0x08 },
};

#define APKIT_GLYPH_W 5
#define APKIT_GLYPH_H 7

static double text_width_local(const char *text, int scale)
{
    size_t n = text ? strlen(text) : 0;
    return (double)n * 6.0 * (double)scale;
}

/* Draws at the local top-left corner of the text block, downward. */
static void draw_text_local(const char *text, double x, double y_top, int scale,
                            unsigned rgba)
{
    double pen = x;
    if (!text || scale < 1)
        return;
    for (const char *p = text; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        const unsigned char *glyph;
        if (ch == '\n') {
            pen = x;
            y_top -= (double)(APKIT_GLYPH_H + 3) * (double)scale;
            continue;
        }
        if (ch < 0x20 || ch > 0x7e)
            ch = '?';
        glyph = appkit_font5x7[ch - 0x20];
        for (int col = 0; col < APKIT_GLYPH_W; col++) {
            unsigned bits = glyph[col];
            for (int row = 0; row < APKIT_GLYPH_H; row++) {
                if (!(bits & (1u << row)))
                    continue;
                draw_rect_local(pen + col * scale,
                                y_top - (row + 1) * scale, scale, scale, rgba);
            }
        }
        pen += 6.0 * (double)scale;
    }
}

static void draw_text_centered_local(const char *text, rect r, int scale,
                                     unsigned rgba)
{
    double tw = text_width_local(text, scale);
    double th = (double)APKIT_GLYPH_H * (double)scale;
    draw_text_local(text, r.x + (r.w - tw) / 2.0,
                    r.y + (r.h + th) / 2.0, scale, rgba);
}

void NSRectFill(rect r) __asm__("_NSRectFill");
void NSRectFill(rect r)
{
    draw_rect_local(r.x, r.y, r.w, r.h, g_fill_rgba);
}

void NSFrameRect(rect r) __asm__("_NSFrameRect");
void NSFrameRect(rect r)
{
    draw_frame_local(r.x, r.y, r.w, r.h, 1.0, g_fill_rgba);
}

#define DECLARE_CLASS(n)                                                      \
    plsr_class plsr_##n##_class __asm__("_OBJC_CLASS_$_" #n);                 \
    plsr_class plsr_##n##_meta __asm__("_OBJC_METACLASS_$_" #n);

extern plsr_class plsr_NSObject_class __asm__("_OBJC_CLASS_$_NSObject");
extern plsr_class plsr_NSObject_meta __asm__("_OBJC_METACLASS_$_NSObject");

DECLARE_CLASS(NSResponder)
DECLARE_CLASS(NSView)
DECLARE_CLASS(NSWindow)
DECLARE_CLASS(NSApplication)
DECLARE_CLASS(NSButton)
DECLARE_CLASS(NSTextField)
DECLARE_CLASS(NSTextView)
DECLARE_CLASS(NSMenu)
DECLARE_CLASS(NSMenuItem)
DECLARE_CLASS(NSToolbar)
DECLARE_CLASS(NSToolbarItem)
DECLARE_CLASS(NSScrollView)
DECLARE_CLASS(NSColor)
DECLARE_CLASS(NSEvent)

void *plsr_NSApp __asm__("_NSApp");

/* ---- NSColor ----------------------------------------------------------- */

typedef struct {
    void *isa;
    unsigned rgba;
} color_obj;

static unsigned pack_rgba(double r, double g, double b, double a)
{
    unsigned ir = (unsigned)((r < 0 ? 0 : r > 1 ? 1 : r) * 255.0 + 0.5);
    unsigned ig = (unsigned)((g < 0 ? 0 : g > 1 ? 1 : g) * 255.0 + 0.5);
    unsigned ib = (unsigned)((b < 0 ? 0 : b > 1 ? 1 : b) * 255.0 + 0.5);
    unsigned ia = (unsigned)((a < 0 ? 0 : a > 1 ? 1 : a) * 255.0 + 0.5);
    return (ir << 24) | (ig << 16) | (ib << 8) | ia;
}

static void *color_alloc(void *cls, const char *sel)
{
    (void)sel;
    return obj_new(cls, sizeof(color_obj));
}

static void *color_with_rgba(void *cls, const char *sel, double r, double g,
                             double b, double a)
{
    color_obj *c = obj_new(cls, sizeof(color_obj));
    (void)sel;
    if (c)
        c->rgba = pack_rgba(r, g, b, a);
    return c;
}

static void *color_with_white(void *cls, const char *sel, double w, double a)
{
    color_obj *c = obj_new(cls, sizeof(color_obj));
    (void)sel;
    if (c)
        c->rgba = pack_rgba(w, w, w, a);
    return c;
}

#define NAMED_COLOR(fn, value)                                                \
    static void *fn(void *cls, const char *sel)                               \
    {                                                                         \
        color_obj *c = obj_new(cls, sizeof(color_obj));                       \
        (void)sel;                                                            \
        if (c)                                                                \
            c->rgba = value;                                                  \
        return c;                                                             \
    }

NAMED_COLOR(color_black, 0x000000ffu)
NAMED_COLOR(color_white, 0xffffffffu)
NAMED_COLOR(color_red, 0xff3b30ffu)
NAMED_COLOR(color_gray, 0x8e8e93ffu)
NAMED_COLOR(color_clear, 0x00000000u)
NAMED_COLOR(color_control_bg, 0xf5f6f8ffu)
NAMED_COLOR(color_text, 0x1b1d20ffu)
NAMED_COLOR(color_label, 0x3b3f46ffu)

static void color_set_fill(void *self, const char *sel)
{
    (void)sel;
    g_fill_rgba = ((color_obj *)self)->rgba;
}

static void color_set_stroke(void *self, const char *sel)
{
    (void)sel;
    g_fill_rgba = ((color_obj *)self)->rgba;
}

static void color_set(void *self, const char *sel)
{
    (void)sel;
    g_fill_rgba = ((color_obj *)self)->rgba;
}

static method_array NSColor_methods = {
    24, 3, {
        { "setFill", "", (void *)color_set_fill },
        { "setStroke", "", (void *)color_set_stroke },
        { "set", "", (void *)color_set },
    }
};

static method_array NSColor_class_methods = {
    24, 12, {
        { "alloc", "", (void *)color_alloc },
        { "colorWithCalibratedRed:green:blue:alpha:", "",
          (void *)color_with_rgba },
        { "colorWithDeviceRed:green:blue:alpha:", "", (void *)color_with_rgba },
        { "colorWithWhite:alpha:", "", (void *)color_with_white },
        { "blackColor", "", (void *)color_black },
        { "whiteColor", "", (void *)color_white },
        { "redColor", "", (void *)color_red },
        { "grayColor", "", (void *)color_gray },
        { "clearColor", "", (void *)color_clear },
        { "controlBackgroundColor", "", (void *)color_control_bg },
        { "textColor", "", (void *)color_text },
        { "labelColor", "", (void *)color_label },
    }
};

/* ---- NSEvent ----------------------------------------------------------- */

typedef struct {
    void *isa;
    long type;
    double x, y;
    long button;
    unsigned long modifiers;
    void *chars;
    unsigned keycode;
} event_obj;

static void *event_alloc(void *cls, const char *sel)
{
    (void)sel;
    return obj_new(cls, sizeof(event_obj));
}

static point event_location(void *self, const char *sel)
{
    event_obj *e = self;
    point p = { e->x, e->y };
    (void)sel;
    return p;
}

static long event_button(void *self, const char *sel)
{
    (void)sel;
    return ((event_obj *)self)->button;
}

static unsigned long event_modifiers(void *self, const char *sel)
{
    (void)sel;
    return ((event_obj *)self)->modifiers;
}

static void *event_chars(void *self, const char *sel)
{
    (void)sel;
    return ((event_obj *)self)->chars;
}

static void *event_chars_ignoring_modifiers(void *self, const char *sel)
{
    (void)sel;
    return ((event_obj *)self)->chars;
}

static unsigned long event_keycode(void *self, const char *sel)
{
    (void)sel;
    return ((event_obj *)self)->keycode;
}

static long event_type(void *self, const char *sel)
{
    (void)sel;
    return ((event_obj *)self)->type;
}

static method_array NSEvent_methods = {
    24, 7, {
        { "locationInWindow", "", (void *)event_location },
        { "buttonNumber", "", (void *)event_button },
        { "modifierFlags", "", (void *)event_modifiers },
        { "characters", "", (void *)event_chars },
        { "charactersIgnoringModifiers", "", (void *)event_chars_ignoring_modifiers },
        { "keyCode", "", (void *)event_keycode },
        { "type", "", (void *)event_type },
    }
};

static method_array NSEvent_class_methods = {
    24, 1, { { "alloc", "", (void *)event_alloc } }
};

static void *event_new(long type, double x, double y, long button,
                       unsigned long modifiers, const char *chars,
                       unsigned keycode)
{
    event_obj *e = obj_new(objc_getClass("NSEvent"), sizeof(event_obj));
    if (!e)
        return NULL;
    e->type = type;
    e->x = x;
    e->y = y;
    e->button = button;
    e->modifiers = modifiers;
    e->chars = chars ? mkstr(chars) : NULL;
    e->keycode = keycode;
    return e;
}

/* ---- object layouts ---------------------------------------------------- */

/* Shared prefix so a control can be used anywhere a view is. */
#define VIEW_FIELDS                                                           \
    void *isa;                                                                \
    rect frame;                                                               \
    struct view_obj *superview;                                               \
    struct view_obj **subviews;                                               \
    int nsubviews;                                                            \
    void *window;                                                             \
    int hidden;                                                               \
    unsigned long autoresizing;                                               \
    void *next_responder;                                                     \
    unsigned flags;                                                           \
    unsigned bg_rgba;                                                         \
    int stack_orientation;                                                    \
    double spacing;                                                           \
    signed char pressed;

#define VIEW_NEEDS_DISPLAY 1u
#define VIEW_AUTORESIZES_SUBVIEWS 2u
#define VIEW_EDITING 4u

/* NSView autoresizingMask bits, as AppKit defines them. */
#define AUTORESIZE_MIN_X 1u
#define AUTORESIZE_WIDTH 2u
#define AUTORESIZE_MAX_X 4u
#define AUTORESIZE_MIN_Y 8u
#define AUTORESIZE_HEIGHT 16u
#define AUTORESIZE_MAX_Y 32u

typedef struct view_obj {
    VIEW_FIELDS
} view_obj;

typedef struct {
    VIEW_FIELDS
    void *title;
    void *target;
    void *action;          /* SEL: selector name */
    long state;
    signed char enabled;
    signed char editable;
    void *placeholder;
    void *delegate;
} control_obj;

typedef struct window_obj {
    void *isa;
    rect frame;
    void *content_view;
    void *content_vc;
    void *toolbar;
    void *title;
    void *window_controller;
    signed char visible;
    signed char released;
    void *first_responder;
    void *mouse_view;
    void *surface;         /* plsr_surface attached by the host */
} window_obj;

static void window_mark_dirty(void *win)
{
    window_obj *w = win;
    if (w && w->surface)
        ((plsr_surface *)w->surface)->needs_display = 1;
}

typedef struct menu_obj {
    void *isa;
    void *title;
    void **items;
    int nitems;
    void *delegate;
    signed char autoenables_items;
} menu_obj;

typedef struct menuitem_obj {
    void *isa;
    void *title;
    void *target;
    void *action;
    void *submenu;
    void *key;
    signed char enabled;
    signed char state;
    signed char alternate;
    signed char separator;
    long tag;
    signed char hidden;
    void *represented_object;
    void *image;
    unsigned long key_mask;
    void *menu;
} menuitem_obj;

static void menuitem_perform_click(void *self, const char *sel, void *sender);
static void menu_update(void *self, const char *sel);

typedef struct toolbar_item_obj {
    void *isa;
    void *identifier;
    void *label;
    void *view;
    void *target;
    void *action;
    void *palette_label;
    void *tool_tip;
    void *image;
    void *toolbar;
    signed char enabled;
    signed char autovalidates;
} toolbar_item_obj;

typedef struct toolbar_obj {
    void *isa;
    void *identifier;
    void *delegate;
    void **items;
    int nitems;
    signed char visible;
} toolbar_obj;

static void toolbar_load_default_items(void *toolbar);

typedef struct app_obj {
    void *isa;
    void *delegate;
    void *main_menu;
    void *windows_menu;
    void *services_menu;
    void *help_menu;
    void *apple_menu;
    void **windows;
    int nwindows;
    void *key_window;
    long activation_policy;
    signed char finished_launching;
    signed char running;
} app_obj;

/* ---- NSView ------------------------------------------------------------ */

static void *view_alloc(void *cls, const char *sel)
{
    view_obj *v = obj_new(cls, sizeof(view_obj));
    (void)sel;
    if (v)
        v->flags = VIEW_AUTORESIZES_SUBVIEWS;   /* AppKit's default */
    return v;
}

static void *view_init_with_frame(void *self, const char *sel, rect r)
{
    (void)sel;
    ((view_obj *)self)->frame = r;
    return self;
}

static rect view_frame(void *self, const char *sel)
{
    (void)sel;
    return ((view_obj *)self)->frame;
}

static void view_resize_subviews(view_obj *v, double oldw, double oldh)
{
    double dw = v->frame.w - oldw;
    double dh = v->frame.h - oldh;
    for (int i = 0; i < v->nsubviews; i++) {
        view_obj *c = v->subviews[i];
        unsigned m = (unsigned)c->autoresizing;
        double x = c->frame.x, y = c->frame.y, w = c->frame.w, h = c->frame.h;
        double ow = w, oh = h;
        int fl = (m & AUTORESIZE_MIN_X) != 0;
        int fw = (m & AUTORESIZE_WIDTH) != 0;
        int fr = (m & AUTORESIZE_MAX_X) != 0;
        int fb = (m & AUTORESIZE_MIN_Y) != 0;
        int fh = (m & AUTORESIZE_HEIGHT) != 0;
        int ft = (m & AUTORESIZE_MAX_Y) != 0;
        int nx = fl + fw + fr, ny = fb + fh + ft;
        (void)fr;
        (void)ft;
        if (nx > 0) {
            double share = dw / (double)nx;
            if (fl)
                x += share;
            if (fw)
                w += share;
        }
        if (ny > 0) {
            double share = dh / (double)ny;
            if (fb)
                y += share;
            if (fh)
                h += share;
        }
        if (w < 1.0)
            w = 1.0;
        if (h < 1.0)
            h = 1.0;
        c->frame = (rect){ x, y, w, h };
        view_resize_subviews(c, ow, oh);
    }
}

static void view_set_frame(void *self, const char *sel, rect r)
{
    view_obj *v = self;
    double oldw = v->frame.w, oldh = v->frame.h;
    (void)sel;
    v->frame = r;
    if ((v->flags & VIEW_AUTORESIZES_SUBVIEWS) &&
        (r.w != oldw || r.h != oldh))
        view_resize_subviews(v, oldw, oldh);
    v->flags |= VIEW_NEEDS_DISPLAY;
}

static void view_set_frame_origin(void *self, const char *sel, double x, double y)
{
    (void)sel;
    ((view_obj *)self)->frame.x = x;
    ((view_obj *)self)->frame.y = y;
}

static void view_set_frame_size(void *self, const char *sel, double w, double h)
{
    view_obj *v = self;
    double oldw = v->frame.w, oldh = v->frame.h;
    (void)sel;
    v->frame.w = w;
    v->frame.h = h;
    if ((v->flags & VIEW_AUTORESIZES_SUBVIEWS) && (w != oldw || h != oldh))
        view_resize_subviews(v, oldw, oldh);
    v->flags |= VIEW_NEEDS_DISPLAY;
}

static void *view_superview(void *self, const char *sel)
{
    (void)sel;
    return ((view_obj *)self)->superview;
}

static void *view_window(void *self, const char *sel)
{
    (void)sel;
    return ((view_obj *)self)->window;
}

static void *view_subviews(void *self, const char *sel)
{
    void *cls;
    void *arr;
    void *(*add)(void *, const char *, void *);
    (void)sel;
    cls = objc_getClass("NSMutableArray");
    if (!cls)
        return NULL;
    add = (void *(*)(void *, const char *, void *))plsr_objc_lookup_imp(
        cls, "arrayWithCapacity:");
    arr = add ? add(cls, "arrayWithCapacity:", NULL) : NULL;
    if (!arr)
        return NULL;
    add = (void *(*)(void *, const char *, void *))plsr_objc_lookup_imp(
        arr, "addObject:");
    for (int i = 0; add && i < ((view_obj *)self)->nsubviews; i++)
        add(arr, "addObject:", ((view_obj *)self)->subviews[i]);
    return arr;
}

static void view_set_window(view_obj *v, void *window)
{
    if (!v)
        return;
    v->window = window;
    for (int i = 0; i < v->nsubviews; i++)
        view_set_window(v->subviews[i], window);
}

static void view_add_subview(void *self, const char *sel, void *child)
{
    view_obj *v = self;
    view_obj *c = child;
    (void)sel;
    if (!v || !c)
        return;
    v->subviews = realloc(v->subviews,
                          (size_t)(v->nsubviews + 1) * sizeof(*v->subviews));
    if (!v->subviews)
        return;
    v->subviews[v->nsubviews++] = c;
    c->superview = v;
    view_set_window(c, v->window);
    v->flags |= VIEW_NEEDS_DISPLAY;
}

static void view_remove_from_superview(void *self, const char *sel)
{
    view_obj *v = self;
    view_obj *p;
    (void)sel;
    if (!v || !(p = v->superview))
        return;
    for (int i = 0; i < p->nsubviews; i++) {
        if (p->subviews[i] == v) {
            memmove(&p->subviews[i], &p->subviews[i + 1],
                    (size_t)(p->nsubviews - i - 1) * sizeof(*p->subviews));
            p->nsubviews--;
            break;
        }
    }
    v->superview = NULL;
}

static void view_set_hidden(void *self, const char *sel, signed char hidden)
{
    (void)sel;
    ((view_obj *)self)->hidden = hidden;
}

static signed char view_is_hidden(void *self, const char *sel)
{
    (void)sel;
    return (signed char)((view_obj *)self)->hidden;
}

static void view_noop(void *self, const char *sel)
{
    (void)self;
    (void)sel;
}

static rect view_bounds(void *self, const char *sel)
{
    view_obj *v = self;
    rect r = { 0, 0, v ? v->frame.w : 0, v ? v->frame.h : 0 };
    (void)sel;
    return r;
}

static unsigned long view_autoresizing_mask(void *self, const char *sel)
{
    (void)sel;
    return ((view_obj *)self)->autoresizing;
}

static void view_set_autoresizing_mask(void *self, const char *sel,
                                       unsigned long mask)
{
    (void)sel;
    ((view_obj *)self)->autoresizing = mask;
}

static signed char view_autoresizes_subviews(void *self, const char *sel)
{
    (void)sel;
    return (signed char)((((view_obj *)self)->flags & VIEW_AUTORESIZES_SUBVIEWS) != 0);
}

static void view_set_autoresizes_subviews(void *self, const char *sel,
                                          signed char on)
{
    view_obj *v = self;
    (void)sel;
    if (on)
        v->flags |= VIEW_AUTORESIZES_SUBVIEWS;
    else
        v->flags &= ~VIEW_AUTORESIZES_SUBVIEWS;
}

static void view_set_needs_display(void *self, const char *sel, signed char on)
{
    view_obj *v = self;
    (void)sel;
    if (on)
        v->flags |= VIEW_NEEDS_DISPLAY;
    else
        v->flags &= ~VIEW_NEEDS_DISPLAY;
    window_mark_dirty(v->window);
}

static void view_set_needs_display_rect(void *self, const char *sel, rect r)
{
    (void)r;
    view_set_needs_display(self, sel, 1);
}

static signed char view_needs_display(void *self, const char *sel)
{
    (void)sel;
    return (signed char)((((view_obj *)self)->flags & VIEW_NEEDS_DISPLAY) != 0);
}

static void view_display(void *self, const char *sel)
{
    (void)sel;
    window_mark_dirty(((view_obj *)self)->window);
}

static void view_display_if_needed(void *self, const char *sel)
{
    view_obj *v = self;
    (void)sel;
    if (v && (v->flags & VIEW_NEEDS_DISPLAY)) {
        window_mark_dirty(v->window);
        v->flags &= ~VIEW_NEEDS_DISPLAY;
    }
}

static void view_draw_rect(void *self, const char *sel, rect r)
{
    view_obj *v = self;
    (void)sel;
    if (v && v->bg_rgba)
        draw_rect_local(r.x, r.y, r.w, r.h, v->bg_rgba);
}

static void view_layout(void *self, const char *sel)
{
    (void)self;
    (void)sel;
}

static void view_layout_subtree(void *self, const char *sel)
{
    view_obj *v = self;
    (void)sel;
    if (!v)
        return;
    {
        void (*fn)(void *, const char *) =
            (void (*)(void *, const char *))plsr_objc_lookup_imp(v, "layout");
        if (fn)
            fn(v, "layout");
    }
    for (int i = 0; i < v->nsubviews; i++)
        view_layout_subtree(v->subviews[i], "layoutSubtreeIfNeeded");
}

static void view_set_needs_layout(void *self, const char *sel, signed char on)
{
    (void)self;
    (void)sel;
    (void)on;
}

static signed char view_needs_layout(void *self, const char *sel)
{
    (void)self;
    (void)sel;
    return 0;
}

static void view_resize_with_old_size(void *self, const char *sel, double w,
                                      double h)
{
    (void)sel;
    view_resize_subviews(self, w, h);
}

static void *view_hit_test(void *self, const char *sel, double x, double y)
{
    view_obj *v = self;
    (void)sel;
    if (!v || v->hidden)
        return NULL;
    for (int i = v->nsubviews - 1; i >= 0; i--) {
        view_obj *c = v->subviews[i];
        if (x >= c->frame.x && x < c->frame.x + c->frame.w &&
            y >= c->frame.y && y < c->frame.y + c->frame.h) {
            void *hit = view_hit_test(c, "hitTest:", x - c->frame.x,
                                      y - c->frame.y);
            if (hit)
                return hit;
        }
    }
    return (x >= 0 && x < v->frame.w && y >= 0 && y < v->frame.h) ? v : NULL;
}

static void *view_next_responder(void *self, const char *sel)
{
    view_obj *v = self;
    (void)sel;
    return v->next_responder ? v->next_responder : (void *)v->superview;
}

static void view_set_next_responder(void *self, const char *sel, void *r)
{
    (void)sel;
    ((view_obj *)self)->next_responder = r;
}

static signed char view_accepts_first_responder(void *self, const char *sel)
{
    (void)self;
    (void)sel;
    return 0;
}

static signed char view_become_first_responder(void *self, const char *sel)
{
    (void)self;
    (void)sel;
    return 1;
}

static void view_resign_first_responder(void *self, const char *sel)
{
    (void)self;
    (void)sel;
}

static signed char view_is_first_responder(void *self, const char *sel)
{
    view_obj *v = self;
    (void)sel;
    return (signed char)(v && v->window &&
                         ((window_obj *)v->window)->first_responder == v);
}

static signed char view_is_flipped(void *self, const char *sel)
{
    (void)self;
    (void)sel;
    return 0;
}

static void view_set_background_color(void *self, const char *sel, void *color)
{
    color_obj *c = color;
    (void)sel;
    ((view_obj *)self)->bg_rgba = c ? c->rgba : 0;
}

static method_array NSView_methods = {
    24, 40, {
        { "initWithFrame:", "", (void *)view_init_with_frame },
        { "frame", "", (void *)view_frame },
        { "setFrame:", "", (void *)view_set_frame },
        { "setFrameOrigin:", "", (void *)view_set_frame_origin },
        { "setFrameSize:", "", (void *)view_set_frame_size },
        { "bounds", "", (void *)view_bounds },
        { "superview", "", (void *)view_superview },
        { "window", "", (void *)view_window },
        { "subviews", "", (void *)view_subviews },
        { "addSubview:", "", (void *)view_add_subview },
        { "removeFromSuperview", "", (void *)view_remove_from_superview },
        { "setHidden:", "", (void *)view_set_hidden },
        { "isHidden", "", (void *)view_is_hidden },
        { "setAutoresizingMask:", "", (void *)view_set_autoresizing_mask },
        { "autoresizingMask", "", (void *)view_autoresizing_mask },
        { "setAutoresizesSubviews:", "", (void *)view_set_autoresizes_subviews },
        { "autoresizesSubviews", "", (void *)view_autoresizes_subviews },
        { "setNeedsDisplay:", "", (void *)view_set_needs_display },
        { "setNeedsDisplayInRect:", "", (void *)view_set_needs_display_rect },
        { "needsDisplay", "", (void *)view_needs_display },
        { "display", "", (void *)view_display },
        { "displayIfNeeded", "", (void *)view_display_if_needed },
        { "drawRect:", "", (void *)view_draw_rect },
        { "layout", "", (void *)view_layout },
        { "layoutSubtreeIfNeeded", "", (void *)view_layout_subtree },
        { "setNeedsLayout", "", (void *)view_set_needs_layout },
        { "needsLayout", "", (void *)view_needs_layout },
        { "resizeSubviewsWithOldSize:", "", (void *)view_resize_with_old_size },
        { "hitTest:", "", (void *)view_hit_test },
        { "nextResponder", "", (void *)view_next_responder },
        { "setNextResponder:", "", (void *)view_set_next_responder },
        { "acceptsFirstResponder", "", (void *)view_accepts_first_responder },
        { "becomeFirstResponder", "", (void *)view_become_first_responder },
        { "resignFirstResponder", "", (void *)view_resign_first_responder },
        { "isFirstResponder", "", (void *)view_is_first_responder },
        { "isFlipped", "", (void *)view_is_flipped },
        { "setBackgroundColor:", "", (void *)view_set_background_color },
        { "setWantsLayer:", "", (void *)view_noop },
        { "viewDidMoveToWindow", "", (void *)view_noop },
        { "viewWillDraw", "", (void *)view_noop },
    }
};

static method_array NSView_class_methods = {
    24, 1, { { "alloc", "", (void *)view_alloc } }
};

/* ---- NSWindow ---------------------------------------------------------- */

static void *window_alloc(void *cls, const char *sel)
{
    (void)sel;
    return obj_new(cls, sizeof(window_obj));
}

static void *window_init_content_rect(void *self, const char *sel, rect r,
                                      unsigned long mask, unsigned long backing,
                                      signed char defer)
{
    (void)sel;
    (void)mask;
    (void)backing;
    (void)defer;
    ((window_obj *)self)->frame = r;
    ((window_obj *)self)->released = 1;
    return self;
}

static void *window_content_view(void *self, const char *sel)
{
    (void)sel;
    return ((window_obj *)self)->content_view;
}

static void window_set_content_view(void *self, const char *sel, void *view)
{
    window_obj *w = self;
    (void)sel;
    if (!w)
        return;
    w->content_view = view;
    if (view) {
        ((view_obj *)view)->window = w;
        /* AppKit sizes the content view to the window's content rect; the nib's
         * own frame is only a fallback when the window has no size yet. */
        if (w->frame.w > 0.0 && w->frame.h > 0.0) {
            rect r = { 0.0, 0.0, w->frame.w, w->frame.h };
            view_set_frame(view, "setFrame:", r);
        }
    }
}

static void *window_title(void *self, const char *sel)
{
    (void)sel;
    return ((window_obj *)self)->title;
}

static void window_set_title(void *self, const char *sel, void *title)
{
    (void)sel;
    ((window_obj *)self)->title = title;
}

static rect window_frame(void *self, const char *sel)
{
    (void)sel;
    return ((window_obj *)self)->frame;
}

static void window_order_front(void *self, const char *sel, void *sender)
{
    (void)sel;
    (void)sender;
    ((window_obj *)self)->visible = 1;
}

static void window_order_out(void *self, const char *sel, void *sender)
{
    (void)sel;
    (void)sender;
    ((window_obj *)self)->visible = 0;
}

static void window_make_key(void *self, const char *sel)
{
    (void)sel;
    ((window_obj *)self)->visible = 1;
}

static signed char window_is_visible(void *self, const char *sel)
{
    (void)sel;
    return (signed char)((window_obj *)self)->visible;
}

static void *window_toolbar(void *self, const char *sel)
{
    (void)sel;
    return ((window_obj *)self)->toolbar;
}

static void window_set_toolbar(void *self, const char *sel, void *toolbar)
{
    (void)sel;
    ((window_obj *)self)->toolbar = toolbar;
    toolbar_load_default_items(toolbar);
}

static void *window_content_vc(void *self, const char *sel)
{
    (void)sel;
    return ((window_obj *)self)->content_vc;
}

static void window_set_content_vc(void *self, const char *sel, void *vc)
{
    (void)sel;
    ((window_obj *)self)->content_vc = vc;
}

static void window_set_window_controller(void *self, const char *sel, void *wc)
{
    (void)sel;
    ((window_obj *)self)->window_controller = wc;
}

static void *window_window_controller(void *self, const char *sel)
{
    (void)sel;
    return ((window_obj *)self)->window_controller;
}

static void window_set_content_size(void *self, const char *sel, double w,
                                    double h)
{
    window_obj *win = self;
    (void)sel;
    if (!win)
        return;
    win->frame.w = w;
    win->frame.h = h;
    if (win->content_view) {
        rect r = { 0, 0, w, h };
        view_set_frame(win->content_view, "setFrame:", r);
    }
    window_mark_dirty(win);
}

static void window_make_first_responder(void *self, const char *sel, void *view)
{
    window_obj *win = self;
    (void)sel;
    if (!win)
        return;
    if (view && responds(view, "acceptsFirstResponder") &&
        !(signed char)(intptr_t)send0(view, "acceptsFirstResponder"))
        return;
    if (win->first_responder && win->first_responder != view)
        send0(win->first_responder, "resignFirstResponder");
    win->first_responder = view;
    if (view)
        send0(view, "becomeFirstResponder");
}

static void *window_first_responder(void *self, const char *sel)
{
    (void)sel;
    return ((window_obj *)self)->first_responder;
}

static void window_display(void *self, const char *sel)
{
    window_obj *w = self;
    (void)sel;
    if (w && w->surface)
        plsr_surface_render(w->surface);
}

static void window_layout_if_needed(void *self, const char *sel)
{
    window_obj *w = self;
    (void)sel;
    if (w && w->content_view)
        send0(w->content_view, "layoutSubtreeIfNeeded");
}

static method_array NSWindow_methods = {
    24, 23, {
        { "setContentSize:", "", (void *)window_set_content_size },
        { "makeFirstResponder:", "", (void *)window_make_first_responder },
        { "firstResponder", "", (void *)window_first_responder },
        { "display", "", (void *)window_display },
        { "layoutIfNeeded", "", (void *)window_layout_if_needed },
        { "initWithContentRect:styleMask:backing:defer:", "",
          (void *)window_init_content_rect },
        { "contentView", "", (void *)window_content_view },
        { "setContentView:", "", (void *)window_set_content_view },
        { "title", "", (void *)window_title },
        { "setTitle:", "", (void *)window_set_title },
        { "frame", "", (void *)window_frame },
        { "makeKeyAndOrderFront:", "", (void *)window_order_front },
        { "orderFront:", "", (void *)window_order_front },
        { "orderFrontRegardless", "", (void *)window_make_key },
        { "orderOut:", "", (void *)window_order_out },
        { "makeKeyWindow", "", (void *)window_make_key },
        { "isVisible", "", (void *)window_is_visible },
        { "toolbar", "", (void *)window_toolbar },
        { "setToolbar:", "", (void *)window_set_toolbar },
        { "contentViewController", "", (void *)window_content_vc },
        { "setContentViewController:", "", (void *)window_set_content_vc },
        { "setWindowController:", "", (void *)window_set_window_controller },
        { "windowController", "", (void *)window_window_controller },
    }
};

static method_array NSWindow_class_methods = {
    24, 1, { { "alloc", "", (void *)window_alloc } }
};

/* ---- NSApplication ----------------------------------------------------- */

static app_obj *g_app;

static void *app_alloc(void *cls, const char *sel)
{
    (void)sel;
    return obj_new(cls, sizeof(app_obj));
}

static void *app_shared(void *cls, const char *sel)
{
    (void)sel;
    if (!g_app) {
        g_app = obj_new((plsr_class *)cls, sizeof(app_obj));
        plsr_NSApp = g_app;
    }
    return g_app;
}

static void app_set_delegate(void *self, const char *sel, void *delegate)
{
    (void)sel;
    ((app_obj *)self)->delegate = delegate;
}

static void *app_delegate(void *self, const char *sel)
{
    (void)sel;
    return ((app_obj *)self)->delegate;
}

static void app_set_main_menu(void *self, const char *sel, void *menu)
{
    (void)sel;
    ((app_obj *)self)->main_menu = menu;
}

static void *app_main_menu(void *self, const char *sel)
{
    (void)sel;
    return ((app_obj *)self)->main_menu;
}

static void app_set_windows_menu(void *self, const char *sel, void *menu)
{
    (void)sel;
    ((app_obj *)self)->windows_menu = menu;
}

static void *app_windows_menu(void *self, const char *sel)
{
    (void)sel;
    return ((app_obj *)self)->windows_menu;
}

static void app_set_services_menu(void *self, const char *sel, void *menu)
{
    (void)sel;
    ((app_obj *)self)->services_menu = menu;
}

static void *app_services_menu(void *self, const char *sel)
{
    (void)sel;
    return ((app_obj *)self)->services_menu;
}

static void app_set_help_menu(void *self, const char *sel, void *menu)
{
    (void)sel;
    ((app_obj *)self)->help_menu = menu;
}

static void *app_help_menu(void *self, const char *sel)
{
    (void)sel;
    return ((app_obj *)self)->help_menu;
}

static void app_set_apple_menu(void *self, const char *sel, void *menu)
{
    (void)sel;
    ((app_obj *)self)->apple_menu = menu;
}

static void *app_apple_menu(void *self, const char *sel)
{
    (void)sel;
    return ((app_obj *)self)->apple_menu;
}

static void app_add_window(void *self, void *win)
{
    app_obj *a = self;
    if (!a || !win)
        return;
    a->windows = realloc(a->windows, (size_t)(a->nwindows + 1) * sizeof(*a->windows));
    if (!a->windows)
        return;
    a->windows[a->nwindows++] = win;
    a->key_window = win;
}

static void app_finish_launching(void *self, const char *sel)
{
    app_obj *a = self;
    (void)sel;
    if (!a || a->finished_launching)
        return;
    a->finished_launching = 1;
    if (a->delegate)
        send1(a->delegate, "applicationDidFinishLaunching:", NULL);
}

static double plsr_monotonic_secs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void app_run(void *self, const char *sel)
{
    app_obj *a = self;
    /* Real NSApplication.run returns only after stop:/terminate:.  Test
     * harnesses that drive NSApplicationMain without a UI session can set
     * PLSR_APP_RUN_IDLE_EXIT=<seconds> to let an idle app (no event, no
     * window, idle run loop) wind the loop down instead of spinning
     * forever.  Unset, the loop is unbounded like AppKit's. */
    const char *idle_env = getenv("PLSR_APP_RUN_IDLE_EXIT");
    double idle_limit = idle_env ? atof(idle_env) : 0.0;
    double idle_deadline = 0.0;
    (void)sel;
    if (!a)
        return;
    app_finish_launching(a, "finishLaunching");
    a->running = 1;
    while (a->running) {
        static const char next_event[] =
            "nextEventMatchingMask:untilDate:inMode:dequeue:";
        void *(*next)(void *, const char *, unsigned long, void *, void *,
                      signed char) =
            (void *(*)(void *, const char *, unsigned long, void *, void *,
                       signed char))plsr_objc_lookup_imp(a, next_event);
        void *event = NULL;
        long rl;
        if (next)
            event = next(a, next_event, ~0ul, NULL, NULL, 1);
        if (!a->running)
            break;
        /* AppKit's next-event pass delivers the archived launch callback and
         * lets the runtime bridge observe real app windows. Run the default
         * CF mode between passes so timers and sources continue to fire. */
        rl = CFRunLoopRunInMode(NULL, 0.05, 0);
        if (idle_limit > 0.0 && !event && (rl == 1 || rl == 3) &&
            a->nwindows == 0) {
            double now = plsr_monotonic_secs();
            if (idle_deadline == 0.0)
                idle_deadline = now + idle_limit;
            else if (now >= idle_deadline)
                break;
        } else {
            idle_deadline = 0.0;
        }
    }
    if (a->delegate)
        send1(a->delegate, "applicationWillTerminate:", NULL);
}

static void app_stop(void *self, const char *sel, void *sender)
{
    (void)sel;
    (void)sender;
    app_obj *app = self ? self : g_app;
    if (app) app->running = 0;
    CFRunLoopStop(CFRunLoopGetMain());
}

static void app_terminate(void *self, const char *sel, void *sender)
{
    app_obj *app = self ? self : g_app;
    long (*should_terminate)(void *, const char *, void *) = NULL;
    long decision = 1; /* NSTerminateNow */
    (void)sel;
    if (!app)
        return;
    if (app->delegate && responds(app->delegate, "applicationShouldTerminate:"))
        should_terminate = (long (*)(void *, const char *, void *))
            plsr_objc_lookup_imp(app->delegate, "applicationShouldTerminate:");
    /* NSTerminateCancel is zero. NSTerminateLater is nonzero and the
     * delegate is responsible for eventually calling replyToApplicationShouldTerminate:. */
    if (should_terminate)
        decision = should_terminate(app->delegate,
                                    "applicationShouldTerminate:", app);
    /* NSTerminateCancel=0; NSTerminateNow=1; NSTerminateLater=2. */
    if (decision == 0 || decision == 2)
        return;
    (void)sender;
    app->running = 0;
    CFRunLoopStop(CFRunLoopGetMain());
}

static void app_reply_terminate(void *self, const char *sel, signed char should)
{
    (void)self;
    (void)sel;
    if (should)
        ((app_obj *)self)->running = 0;
    if (should)
        CFRunLoopStop(CFRunLoopGetMain());
}

static void *app_windows(void *self, const char *sel)
{
    void *cls;
    void *arr;
    void *(*add)(void *, const char *, void *);
    app_obj *a = self;
    (void)sel;
    cls = objc_getClass("NSMutableArray");
    add = cls ? (void *(*)(void *, const char *, void *))plsr_objc_lookup_imp(
                    cls, "arrayWithCapacity:") : NULL;
    arr = add ? add(cls, "arrayWithCapacity:", NULL) : NULL;
    if (!arr || !a)
        return arr;
    add = (void *(*)(void *, const char *, void *))plsr_objc_lookup_imp(
        arr, "addObject:");
    for (int i = 0; add && i < a->nwindows; i++)
        add(arr, "addObject:", a->windows[i]);
    return arr;
}

static void *app_key_window(void *self, const char *sel)
{
    (void)sel;
    return ((app_obj *)self)->key_window;
}

static void app_set_activation_policy(void *self, const char *sel, long policy)
{
    (void)sel;
    ((app_obj *)self)->activation_policy = policy;
}

static void app_activate(void *self, const char *sel, signed char flag)
{
    (void)self;
    (void)sel;
    (void)flag;
}

static void *app_next_event(void *self, const char *sel, unsigned long mask,
                            void *until, void *mode, signed char dequeue)
{
    (void)self;
    (void)sel;
    (void)mask;
    (void)until;
    (void)mode;
    (void)dequeue;
    return NULL;
}

static signed char app_send_action(void *self, const char *sel, void *action,
                                   void *target, void *sender)
{
    void *(*fn)(void *, const char *, void *);
    app_obj *app = self;
    (void)sel;
    if (!app) app = g_app;
    if (!action) return 0;
    if (getenv("PLSR_REEF_TRACE"))
        fprintf(stderr, "reef: sendAction action=%p target=%p sender=%p keyWindow=%p delegate=%p\n",
                action, target, sender,
                app ? app->key_window : NULL, app ? app->delegate : NULL);
    if (target) {
        fn = (void *(*)(void *, const char *, void *))
            plsr_objc_lookup_imp(target, action);
        if (!fn) return 0;
        fn(target, action, sender);
        return 1;
    }

    /* Cocoa resolves a nil target through the key window's responder chain,
     * then the window, application delegate, and application itself. */
    void *window = app ? app->key_window : NULL;
    void *responder = window ? send0(window, "firstResponder") : NULL;
    for (unsigned depth = 0; responder && depth < 64; ++depth) {
        fn = (void *(*)(void *, const char *, void *))
            plsr_objc_lookup_imp(responder, action);
        if (fn) {
            fn(responder, action, sender);
            return 1;
        }
        void *next = send0(responder, "nextResponder");
        if (!next || next == responder) break;
        responder = next;
    }
    void *candidates[] = { window, app ? app->delegate : NULL, app };
    for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; ++i) {
        if (!candidates[i]) continue;
        if (getenv("PLSR_REEF_TRACE"))
            fprintf(stderr, "reef: sendAction lookup candidate[%zu]=%p\n", i, candidates[i]);
        fn = (void *(*)(void *, const char *, void *))
            plsr_objc_lookup_imp(candidates[i], action);
        if (fn) {
            if (getenv("PLSR_REEF_TRACE"))
                fprintf(stderr, "reef: sendAction invoke candidate[%zu] imp=%p\n", i, (void *)fn);
            fn(candidates[i], action, sender);
            return 1;
        }
    }
    return 0;
}

static method_array NSApplication_methods = {
    24, 24, {
        { "setDelegate:", "", (void *)app_set_delegate },
        { "delegate", "", (void *)app_delegate },
        { "setMainMenu:", "", (void *)app_set_main_menu },
        { "mainMenu", "", (void *)app_main_menu },
        { "finishLaunching", "", (void *)app_finish_launching },
        { "run", "", (void *)app_run },
        { "stop:", "", (void *)app_stop },
        { "windows", "", (void *)app_windows },
        { "keyWindow", "", (void *)app_key_window },
        { "mainWindow", "", (void *)app_key_window },
        { "setActivationPolicy:", "", (void *)app_set_activation_policy },
        { "activateIgnoringOtherApps:", "", (void *)app_activate },
        { "nextEventMatchingMask:untilDate:inMode:dequeue:", "",
          (void *)app_next_event },
        { "sendAction:to:from:", "", (void *)app_send_action },
        { "setWindowsMenu:", "", (void *)app_set_windows_menu },
        { "windowsMenu", "", (void *)app_windows_menu },
        { "setServicesMenu:", "", (void *)app_set_services_menu },
        { "servicesMenu", "", (void *)app_services_menu },
        { "setHelpMenu:", "", (void *)app_set_help_menu },
        { "helpMenu", "", (void *)app_help_menu },
        { "setAppleMenu:", "", (void *)app_set_apple_menu },
        { "appleMenu", "", (void *)app_apple_menu },
        { "terminate:", "", (void *)app_terminate },
        { "replyToApplicationShouldTerminate:", "", (void *)app_reply_terminate },
    }
};

static method_array NSApplication_class_methods = {
    24, 2, {
        { "alloc", "", (void *)app_alloc },
        { "sharedApplication", "", (void *)app_shared },
    }
};

/* ---- shared control text ------------------------------------------------ */

static void control_append_text(control_obj *c, const char *add, int backspace)
{
    const char *cur;
    char buf[1024];
    size_t n;
    if (!c)
        return;
    cur = cstr(c->title);
    if (!cur)
        cur = "";
    n = strlen(cur);
    if (backspace) {
        if (n)
            n--;
        memcpy(buf, cur, n);
        buf[n] = 0;
    } else {
        if (!add)
            return;
        if (n + strlen(add) >= sizeof buf)
            return;
        memcpy(buf, cur, n);
        strcpy(buf + n, add);
    }
    c->title = mkstr(buf);
    c->flags |= VIEW_NEEDS_DISPLAY;
    if (c->delegate && responds(c->delegate, "controlTextDidChange:"))
        send1(c->delegate, "controlTextDidChange:", c);
    window_mark_dirty(c->window);
}

/* ---- NSButton ---------------------------------------------------------- */

static void *button_alloc(void *cls, const char *sel)
{
    control_obj *c = obj_new(cls, sizeof(control_obj));
    (void)sel;
    if (c)
        c->flags = VIEW_AUTORESIZES_SUBVIEWS;
    return c;
}

static void *button_init_with_frame(void *self, const char *sel, rect r)
{
    (void)sel;
    ((control_obj *)self)->frame = r;
    ((control_obj *)self)->enabled = 1;
    return self;
}

static void *button_with_title(void *cls, const char *sel, void *title,
                               void *target, void *action)
{
    control_obj *b = obj_new((plsr_class *)cls, sizeof(control_obj));
    (void)sel;
    if (!b)
        return NULL;
    b->title = title;
    b->target = target;
    b->action = action;
    b->enabled = 1;
    return b;
}

static void *button_title(void *self, const char *sel)
{
    (void)sel;
    return ((control_obj *)self)->title;
}

static void button_set_title(void *self, const char *sel, void *title)
{
    (void)sel;
    ((control_obj *)self)->title = title;
}

static void button_set_target(void *self, const char *sel, void *target)
{
    (void)sel;
    ((control_obj *)self)->target = target;
}

static void *button_target(void *self, const char *sel)
{
    (void)sel;
    return ((control_obj *)self)->target;
}

static void button_set_action(void *self, const char *sel, void *action)
{
    (void)sel;
    ((control_obj *)self)->action = action;
}

static void *button_action(void *self, const char *sel)
{
    (void)sel;
    return ((control_obj *)self)->action;
}

static void button_perform_click(void *self, const char *sel, void *sender)
{
    control_obj *b = self;
    void *(*fn)(void *, const char *, void *);
    (void)sel;
    if (!b || !b->enabled || !b->action || !b->target)
        return;
    fn = (void *(*)(void *, const char *, void *))plsr_objc_lookup_imp(
        b->target, b->action);
    if (fn)
        fn(b->target, b->action, sender ? sender : b);
}

static void button_set_enabled(void *self, const char *sel, signed char on)
{
    (void)sel;
    ((control_obj *)self)->enabled = on;
}

static signed char button_is_enabled(void *self, const char *sel)
{
    (void)sel;
    return (signed char)((control_obj *)self)->enabled;
}

static void button_set_state(void *self, const char *sel, long state)
{
    (void)sel;
    ((control_obj *)self)->state = state;
}

static long button_state(void *self, const char *sel)
{
    (void)sel;
    return ((control_obj *)self)->state;
}

static void button_draw(void *self, const char *sel, rect r)
{
    control_obj *b = self;
    unsigned bg = b->pressed ? 0xb9c2cdffu : 0xdde3eaffu;
    (void)sel;
    draw_rect_local(r.x, r.y, r.w, r.h, bg);
    draw_frame_local(r.x, r.y, r.w, r.h, 1.0, 0x8f98a3ffu);
    if (b->title)
        draw_text_centered_local(cstr(b->title), r, 2, 0x1b1d20ffu);
}

static int point_in_view(void *self, void *ev)
{
    view_obj *v = self;
    event_obj *e = ev;
    double ax = 0.0, ay = 0.0;
    for (view_obj *p = v; p; p = p->superview) {
        ax += p->frame.x;
        ay += p->frame.y;
    }
    return e && e->x >= ax && e->x < ax + v->frame.w && e->y >= ay &&
           e->y < ay + v->frame.h;
}

static void button_mouse_down(void *self, const char *sel, void *ev)
{
    control_obj *b = self;
    (void)sel;
    (void)ev;
    b->pressed = 1;
    b->flags |= VIEW_NEEDS_DISPLAY;
    window_mark_dirty(b->window);
}

static void button_mouse_up(void *self, const char *sel, void *ev)
{
    control_obj *b = self;
    int inside = point_in_view(self, ev);
    (void)sel;
    b->pressed = 0;
    b->flags |= VIEW_NEEDS_DISPLAY;
    window_mark_dirty(b->window);
    if (inside)
        button_perform_click(self, "performClick:", self);
}

static signed char button_accepts_first(void *self, const char *sel)
{
    (void)self;
    (void)sel;
    return 1;
}

static method_array NSButton_methods = {
    24, 17, {
        { "drawRect:", "", (void *)button_draw },
        { "mouseDown:", "", (void *)button_mouse_down },
        { "mouseUp:", "", (void *)button_mouse_up },
        { "acceptsFirstResponder", "", (void *)button_accepts_first },
        { "initWithFrame:", "", (void *)button_init_with_frame },
        { "title", "", (void *)button_title },
        { "setTitle:", "", (void *)button_set_title },
        { "target", "", (void *)button_target },
        { "setTarget:", "", (void *)button_set_target },
        { "action", "", (void *)button_action },
        { "setAction:", "", (void *)button_set_action },
        { "performClick:", "", (void *)button_perform_click },
        { "setEnabled:", "", (void *)button_set_enabled },
        { "isEnabled", "", (void *)button_is_enabled },
        { "setState:", "", (void *)button_set_state },
        { "state", "", (void *)button_state },
        { "setBezelStyle:", "", (void *)view_noop },
    }
};

static method_array NSButton_class_methods = {
    24, 2, {
        { "alloc", "", (void *)button_alloc },
        { "buttonWithTitle:target:action:", "", (void *)button_with_title },
    }
};

/* ---- NSTextField ------------------------------------------------------- */

static void *field_string_value(void *self, const char *sel)
{
    (void)sel;
    return ((control_obj *)self)->title;
}

static void field_set_string_value(void *self, const char *sel, void *value)
{
    (void)sel;
    ((control_obj *)self)->title = value;
}

static void field_set_placeholder(void *self, const char *sel, void *value)
{
    (void)sel;
    ((control_obj *)self)->placeholder = value;
}

static void *field_placeholder(void *self, const char *sel)
{
    (void)sel;
    return ((control_obj *)self)->placeholder;
}

static void field_set_editable(void *self, const char *sel, signed char on)
{
    (void)sel;
    ((control_obj *)self)->editable = on;
}

static signed char field_is_editable(void *self, const char *sel)
{
    (void)sel;
    return (signed char)((control_obj *)self)->editable;
}

static void *field_label_with_string(void *cls, const char *sel, void *value)
{
    control_obj *f = obj_new((plsr_class *)cls, sizeof(control_obj));
    (void)sel;
    if (f)
        f->title = value;
    return f;
}

static void *field_text_field_with_string(void *cls, const char *sel, void *value)
{
    control_obj *f = obj_new((plsr_class *)cls, sizeof(control_obj));
    (void)sel;
    if (f) {
        f->title = value;
        f->editable = 1;
    }
    return f;
}

static void field_draw(void *self, const char *sel, rect r)
{
    control_obj *f = self;
    const char *t;
    (void)sel;
    draw_rect_local(r.x, r.y, r.w, r.h, 0xffffffffu);
    draw_frame_local(r.x, r.y, r.w, r.h, 1.0, 0x9aa2acffu);
    t = f->title ? cstr(f->title) : NULL;
    if (!t || !*t)
        t = f->placeholder ? cstr(f->placeholder) : NULL;
    if (t && *t)
        draw_text_local(t, r.x + 6.0, r.y + r.h - 6.0, 2,
                        (f->title && *cstr(f->title)) ? 0x1b1d20ffu
                                                      : 0x8a9099ffu);
    if (f->flags & VIEW_EDITING)
        draw_rect_local(r.x + 7.0 + text_width_local(t ? t : "", 2), r.y + 5.0,
                        1.0, r.h - 10.0, 0x2f6fd0ffu);
}

static void field_mouse_down(void *self, const char *sel, void *ev)
{
    control_obj *f = self;
    (void)sel;
    (void)ev;
    if (!f->editable)
        return;
    f->flags |= VIEW_EDITING;
    window_mark_dirty(f->window);
    if (f->window)
        send1(f->window, "makeFirstResponder:", self);
}

static void field_key_down(void *self, const char *sel, void *ev)
{
    control_obj *f = self;
    event_obj *e = ev;
    (void)sel;
    if (!f->editable || !e)
        return;
    if (e->keycode == 51)
        control_append_text(f, NULL, 1);
    else {
        /* e->chars is an NSString (charactersReturningByValue); the field
         * storage is UTF-8, so convert before appending. */
        const char *chars = e->chars ? cstr(e->chars) : NULL;
        if (chars && chars[0])
            control_append_text(f, chars, 0);
    }
}

static signed char field_accepts_first(void *self, const char *sel)
{
    (void)sel;
    return (signed char)(((control_obj *)self)->editable != 0);
}

static void field_set_delegate(void *self, const char *sel, void *delegate)
{
    (void)sel;
    ((control_obj *)self)->delegate = delegate;
}

static void *field_delegate(void *self, const char *sel)
{
    (void)sel;
    return ((control_obj *)self)->delegate;
}

static method_array NSTextField_methods = {
    24, 17, {
        { "initWithFrame:", "", (void *)button_init_with_frame },
        { "drawRect:", "", (void *)field_draw },
        { "mouseDown:", "", (void *)field_mouse_down },
        { "keyDown:", "", (void *)field_key_down },
        { "acceptsFirstResponder", "", (void *)field_accepts_first },
        { "stringValue", "", (void *)field_string_value },
        { "setStringValue:", "", (void *)field_set_string_value },
        { "placeholderString", "", (void *)field_placeholder },
        { "setPlaceholderString:", "", (void *)field_set_placeholder },
        { "isEditable", "", (void *)field_is_editable },
        { "setEditable:", "", (void *)field_set_editable },
        { "setBezeled:", "", (void *)view_noop },
        { "setDrawsBackground:", "", (void *)view_noop },
        { "setFont:", "", (void *)view_noop },
        { "setDelegate:", "", (void *)field_set_delegate },
        { "delegate", "", (void *)field_delegate },
        { "controlTextDidChange:", "", (void *)view_noop },
    }
};

static method_array NSTextField_class_methods = {
    24, 3, {
        { "alloc", "", (void *)button_alloc },
        { "labelWithString:", "", (void *)field_label_with_string },
        { "textFieldWithString:", "", (void *)field_text_field_with_string },
    }
};

/* ---- NSTextView -------------------------------------------------------- */

static void *text_view_storage(void *self, const char *sel)
{
    (void)self;
    (void)sel;
    return NULL;
}

static void text_view_insert(void *self, const char *sel, void *text)
{
    (void)sel;
    ((control_obj *)self)->title = text;
}

static void textview_draw(void *self, const char *sel, rect r)
{
    control_obj *t = self;
    const char *s = t->title ? cstr(t->title) : NULL;
    (void)sel;
    draw_rect_local(r.x, r.y, r.w, r.h, 0xffffffffu);
    if (s && *s)
        draw_text_local(s, r.x + 6.0, r.y + r.h - 6.0, 2, 0x1b1d20ffu);
}

static void textview_key_down(void *self, const char *sel, void *ev)
{
    control_obj *t = self;
    event_obj *e = ev;
    (void)sel;
    if (!e)
        return;
    if (e->keycode == 51)
        control_append_text(t, NULL, 1);
    else if (e->keycode == 36)
        control_append_text(t, "\n", 0);
    else {
        const char *chars = e->chars ? cstr(e->chars) : NULL;
        if (chars && chars[0])
            control_append_text(t, chars, 0);
    }
}

static void textview_mouse_down(void *self, const char *sel, void *ev)
{
    control_obj *t = self;
    (void)sel;
    (void)ev;
    t->flags |= VIEW_EDITING;
    if (t->window)
        send1(t->window, "makeFirstResponder:", self);
}

static method_array NSTextView_methods = {
    24, 13, {
        { "drawRect:", "", (void *)textview_draw },
        { "mouseDown:", "", (void *)textview_mouse_down },
        { "keyDown:", "", (void *)textview_key_down },
        { "acceptsFirstResponder", "", (void *)button_accepts_first },
        { "initWithFrame:", "", (void *)button_init_with_frame },
        { "string", "", (void *)field_string_value },
        { "setString:", "", (void *)field_set_string_value },
        { "insertText:", "", (void *)text_view_insert },
        { "textStorage", "", (void *)text_view_storage },
        { "setEditable:", "", (void *)field_set_editable },
        { "isEditable", "", (void *)field_is_editable },
        { "setFont:", "", (void *)view_noop },
        { "setVerticallyResizable:", "", (void *)view_noop },
    }
};

static method_array NSTextView_class_methods = {
    24, 1, { { "alloc", "", (void *)button_alloc } }
};

/* ---- NSScrollView ------------------------------------------------------ */

static void scroll_set_document_view(void *self, const char *sel, void *doc)
{
    (void)sel;
    view_add_subview(self, "addSubview:", doc);
}

static void *scroll_document_view(void *self, const char *sel)
{
    view_obj *v = self;
    (void)sel;
    return (v && v->nsubviews) ? v->subviews[0] : NULL;
}

static method_array NSScrollView_methods = {
    24, 5, {
        { "initWithFrame:", "", (void *)view_init_with_frame },
        { "setDocumentView:", "", (void *)scroll_set_document_view },
        { "documentView", "", (void *)scroll_document_view },
        { "setHasVerticalScroller:", "", (void *)view_noop },
        { "setHasHorizontalScroller:", "", (void *)view_noop },
    }
};

static method_array NSScrollView_class_methods = {
    24, 1, { { "alloc", "", (void *)view_alloc } }
};

/* ---- NSMenu / NSMenuItem ----------------------------------------------- */

static void *menu_alloc(void *cls, const char *sel)
{
    (void)sel;
    return obj_new(cls, sizeof(menu_obj));
}

static void *menuitem_alloc(void *cls, const char *sel)
{
    (void)sel;
    return obj_new(cls, sizeof(menuitem_obj));
}

static void *menu_init_with_title(void *self, const char *sel, void *title)
{
    (void)sel;
    ((menu_obj *)self)->title = title;
    ((menu_obj *)self)->autoenables_items = 1;
    return self;
}

static void menu_add_item(void *self, const char *sel, void *item)
{
    menu_obj *m = self;
    (void)sel;
    if (!m || !item)
        return;
    void **items = realloc(m->items, (size_t)(m->nitems + 1) * sizeof(*m->items));
    if (!items)
        return;
    m->items = items;
    m->items[m->nitems++] = item;
    ((menuitem_obj *)item)->menu = m;
}

static void menu_insert_item(void *self, const char *sel, void *item, long index)
{
    menu_obj *m = self;
    void **items;
    (void)sel;
    if (!m || !item) return;
    if (index < 0) index = 0;
    if (index > m->nitems) index = m->nitems;
    items = realloc(m->items, (size_t)(m->nitems + 1) * sizeof(*m->items));
    if (!items) return;
    m->items = items;
    memmove(&m->items[index + 1], &m->items[index],
            (size_t)(m->nitems - index) * sizeof(*m->items));
    m->items[index] = item;
    ((menuitem_obj *)item)->menu = m;
    m->nitems++;
}

static void menu_remove_item_at_index(void *self, const char *sel, long index)
{
    menu_obj *m = self;
    (void)sel;
    if (!m || index < 0 || index >= m->nitems) return;
    ((menuitem_obj *)m->items[index])->menu = NULL;
    memmove(&m->items[index], &m->items[index + 1],
            (size_t)(m->nitems - index - 1) * sizeof(*m->items));
    m->nitems--;
}

static void menu_remove_item(void *self, const char *sel, void *item)
{
    menu_obj *m = self;
    (void)sel;
    for (int i = 0; m && i < m->nitems; ++i)
        if (m->items[i] == item) { menu_remove_item_at_index(m, "", i); return; }
}

static void menu_remove_all_items(void *self, const char *sel)
{
    menu_obj *m = self;
    (void)sel;
    if (m) {
        for (int i = 0; i < m->nitems; ++i)
            ((menuitem_obj *)m->items[i])->menu = NULL;
        m->nitems = 0;
    }
}

static long menu_index_of_item(void *self, const char *sel, void *item)
{
    menu_obj *m = self;
    (void)sel;
    for (int i = 0; m && i < m->nitems; ++i)
        if (m->items[i] == item) return i;
    return -1;
}

static long menu_index_of_title(void *self, const char *sel, void *title)
{
    menu_obj *m = self;
    const char *wanted = cstr(title);
    (void)sel;
    if (!wanted) return -1;
    for (int i = 0; m && i < m->nitems; ++i) {
        menuitem_obj *item = m->items[i];
        const char *candidate = item ? cstr(item->title) : NULL;
        if (candidate && strcmp(candidate, wanted) == 0) return i;
    }
    return -1;
}

static void *menu_item_with_title(void *self, const char *sel, void *title)
{
    menu_obj *m = self;
    long i = menu_index_of_title(m, sel, title);
    return m && i >= 0 ? m->items[i] : NULL;
}

static void *menu_item_with_tag(void *self, const char *sel, long tag)
{
    menu_obj *m = self;
    (void)sel;
    for (int i = 0; m && i < m->nitems; ++i)
        if (((menuitem_obj *)m->items[i])->tag == tag) return m->items[i];
    return NULL;
}

static long menu_index_of_submenu(void *self, const char *sel, void *submenu)
{
    menu_obj *m = self;
    (void)sel;
    for (int i = 0; m && i < m->nitems; ++i)
        if (((menuitem_obj *)m->items[i])->submenu == submenu) return i;
    return -1;
}

static void menu_set_submenu_for_item(void *self, const char *sel,
                                      void *submenu, void *item)
{
    (void)self;
    (void)sel;
    if (item) ((menuitem_obj *)item)->submenu = submenu;
}

static void menu_perform_action_for_item(void *self, const char *sel, long index)
{
    menu_obj *m = self;
    (void)sel;
    if (!m || index < 0 || index >= m->nitems) return;
    menu_update(m, "update");
    if (index >= m->nitems) return;
    menuitem_obj *item = m->items[index];
    if (item && item->enabled && !item->hidden && !item->separator)
        menuitem_perform_click(item, "performClick:", item);
}

static signed char menu_perform_key_equivalent_inner(menu_obj *m, void *event,
                                                      unsigned depth)
{
    const char *chars = cstr(send0(event, "charactersIgnoringModifiers"));
    unsigned long flags = (unsigned long)(uintptr_t)send0(event, "modifierFlags");
    const unsigned long modifier_mask = 0x00ff0000ul;
    if (!m || depth > 32 || !chars || !*chars) return 0;
    menu_update(m, "update");
    for (int i = 0; i < m->nitems; ++i) {
        menuitem_obj *item = m->items[i];
        if (!item || !item->enabled || item->hidden || item->separator) continue;
        if (item->submenu && menu_perform_key_equivalent_inner(
                item->submenu, event, depth + 1)) return 1;
        const char *key = cstr(item->key);
        if (!key || !*key || strlen(key) != strlen(chars)) continue;
        if ((flags & modifier_mask) != (item->key_mask & modifier_mask)) continue;
        size_t n = strlen(key), j = 0;
        while (j < n && tolower((unsigned char)key[j]) ==
                        tolower((unsigned char)chars[j])) ++j;
        if (j == n) {
            menuitem_perform_click(item, "performClick:", event);
            return 1;
        }
    }
    return 0;
}

static signed char menu_perform_key_equivalent(void *self, const char *sel,
                                                void *event)
{
    (void)sel;
    return menu_perform_key_equivalent_inner(self, event, 0);
}

static void *menu_item_array(void *self, const char *sel)
{
    menu_obj *m = self;
    void *cls, *array;
    void *(*make)(void *, const char *, unsigned long);
    void *(*add)(void *, const char *, void *);
    (void)sel;
    if (!m) return NULL;
    cls = objc_getClass("NSMutableArray");
    make = cls ? (void *(*)(void *, const char *, unsigned long))
        plsr_objc_lookup_imp(cls, "arrayWithCapacity:") : NULL;
    array = make ? make(cls, "arrayWithCapacity:", (unsigned long)m->nitems) : NULL;
    add = array ? (void *(*)(void *, const char *, void *))
        plsr_objc_lookup_imp(array, "addObject:") : NULL;
    for (int i = 0; add && i < m->nitems; ++i) add(array, "addObject:", m->items[i]);
    return array;
}

static void *menu_title(void *self, const char *sel)
{ (void)sel; return self ? ((menu_obj *)self)->title : NULL; }
static void menu_set_title(void *self, const char *sel, void *title)
{ (void)sel; if (self) ((menu_obj *)self)->title = title; }
static void menu_set_delegate(void *self, const char *sel, void *delegate)
{ (void)sel; if (self) ((menu_obj *)self)->delegate = delegate; }
static void *menu_delegate(void *self, const char *sel)
{ (void)sel; return self ? ((menu_obj *)self)->delegate : NULL; }
static void menu_set_autoenables(void *self, const char *sel, signed char on)
{ (void)sel; if (self) ((menu_obj *)self)->autoenables_items = on; }
static signed char menu_autoenables(void *self, const char *sel)
{ (void)sel; return self ? ((menu_obj *)self)->autoenables_items : 0; }

static void menu_update(void *self, const char *sel)
{
    menu_obj *menu = self;
    (void)sel;
    if (!menu) return;
    if (menu->delegate && responds(menu->delegate, "menuNeedsUpdate:"))
        send1(menu->delegate, "menuNeedsUpdate:", menu);
    if (!menu->autoenables_items) return;
    for (int i = 0; i < menu->nitems; ++i) {
        menuitem_obj *item = menu->items[i];
        if (!item || !item->target || !item->action || item->separator) continue;
        const char *selector = responds(item->target, "validateMenuItem:")
            ? "validateMenuItem:" :
              responds(item->target, "validateUserInterfaceItem:")
                ? "validateUserInterfaceItem:" : NULL;
        if (!selector) continue;
        signed char (*validate)(void *, const char *, void *) =
            (signed char (*)(void *, const char *, void *))
                plsr_objc_lookup_imp(item->target, selector);
        if (validate) item->enabled = validate(item->target, selector, item);
    }
}

static void *menu_add_item_with_title(void *self, const char *sel, void *title,
                                      void *action, void *key)
{
    void *item_cls = objc_getClass("NSMenuItem");
    void *(*init)(void *, const char *, void *, void *, void *);
    void *item;
    (void)sel;
    item = item_cls ? obj_new((plsr_class *)item_cls, sizeof(menuitem_obj)) : NULL;
    if (!item)
        return NULL;
    init = (void *(*)(void *, const char *, void *, void *, void *))
        plsr_objc_lookup_imp(item, "initWithTitle:action:keyEquivalent:");
    if (init)
        item = init(item, "initWithTitle:action:keyEquivalent:", title, action, key);
    menu_add_item(self, "addItem:", item);
    return item;
}

static long menu_number_of_items(void *self, const char *sel)
{
    (void)sel;
    return ((menu_obj *)self)->nitems;
}

static void *menu_item_at_index(void *self, const char *sel, long index)
{
    menu_obj *m = self;
    (void)sel;
    if (!m || index < 0 || index >= m->nitems)
        return NULL;
    return m->items[index];
}

static void *menuitem_init_with_title(void *self, const char *sel, void *title,
                                      void *action, void *key)
{
    menuitem_obj *i = self;
    const char *equivalent = cstr(key);
    (void)sel;
    i->title = title;
    i->action = action;
    i->key = key;
    i->enabled = 1;
    i->key_mask = (equivalent && *equivalent) ? (1ul << 20) : 0;
    return self;
}

static void menuitem_set_target(void *self, const char *sel, void *target)
{
    (void)sel;
    ((menuitem_obj *)self)->target = target;
}

static void menuitem_set_action(void *self, const char *sel, void *action)
{
    (void)sel;
    ((menuitem_obj *)self)->action = action;
}

static void *menuitem_target(void *self, const char *sel)
{
    (void)sel;
    return ((menuitem_obj *)self)->target;
}

static void *menuitem_action(void *self, const char *sel)
{
    (void)sel;
    return ((menuitem_obj *)self)->action;
}

static void menuitem_set_submenu(void *self, const char *sel, void *menu)
{
    (void)sel;
    ((menuitem_obj *)self)->submenu = menu;
}

static void *menuitem_submenu(void *self, const char *sel)
{
    (void)sel;
    return ((menuitem_obj *)self)->submenu;
}

static void *menuitem_title(void *self, const char *sel)
{
    (void)sel;
    return ((menuitem_obj *)self)->title;
}

static void menuitem_set_title(void *self, const char *sel, void *title)
{
    (void)sel;
    ((menuitem_obj *)self)->title = title;
}

static method_array NSMenu_methods = {
    24, 25, {
        { "initWithTitle:", "", (void *)menu_init_with_title },
        { "addItem:", "", (void *)menu_add_item },
        { "insertItem:atIndex:", "", (void *)menu_insert_item },
        { "removeItem:", "", (void *)menu_remove_item },
        { "removeItemAtIndex:", "", (void *)menu_remove_item_at_index },
        { "removeAllItems", "", (void *)menu_remove_all_items },
        { "itemArray", "", (void *)menu_item_array },
        { "addItemWithTitle:action:keyEquivalent:", "",
          (void *)menu_add_item_with_title },
        { "numberOfItems", "", (void *)menu_number_of_items },
        { "itemAtIndex:", "", (void *)menu_item_at_index },
        { "itemWithTitle:", "", (void *)menu_item_with_title },
        { "itemWithTag:", "", (void *)menu_item_with_tag },
        { "indexOfItem:", "", (void *)menu_index_of_item },
        { "indexOfItemWithTitle:", "", (void *)menu_index_of_title },
        { "indexOfItemWithSubmenu:", "", (void *)menu_index_of_submenu },
        { "setSubmenu:forItem:", "", (void *)menu_set_submenu_for_item },
        { "performActionForItemAtIndex:", "", (void *)menu_perform_action_for_item },
        { "performKeyEquivalent:", "", (void *)menu_perform_key_equivalent },
        { "title", "", (void *)menu_title },
        { "setTitle:", "", (void *)menu_set_title },
        { "setDelegate:", "", (void *)menu_set_delegate },
        { "delegate", "", (void *)menu_delegate },
        { "setAutoenablesItems:", "", (void *)menu_set_autoenables },
        { "autoenablesItems", "", (void *)menu_autoenables },
        { "update", "", (void *)menu_update },
    }
};

static method_array NSMenu_class_methods = {
    24, 1, { { "alloc", "", (void *)menu_alloc } }
};

static void menuitem_set_key(void *self, const char *sel, void *key)
{
    (void)sel;
    ((menuitem_obj *)self)->key = key;
}

static void menuitem_set_key_mask(void *self, const char *sel, unsigned long mask)
{ (void)sel; if (self) ((menuitem_obj *)self)->key_mask = mask; }
static unsigned long menuitem_key_mask(void *self, const char *sel)
{ (void)sel; return self ? ((menuitem_obj *)self)->key_mask : 0; }

static void *menuitem_key(void *self, const char *sel)
{
    (void)sel;
    return ((menuitem_obj *)self)->key;
}

static void menuitem_set_enabled(void *self, const char *sel, signed char on)
{
    (void)sel;
    ((menuitem_obj *)self)->enabled = on;
}

static signed char menuitem_is_enabled(void *self, const char *sel)
{
    (void)sel;
    return (signed char)((menuitem_obj *)self)->enabled;
}

static void menuitem_set_alternate(void *self, const char *sel, signed char on)
{
    (void)sel;
    ((menuitem_obj *)self)->alternate = on;
}

static signed char menuitem_is_alternate(void *self, const char *sel)
{
    (void)sel;
    return ((menuitem_obj *)self)->alternate;
}

static void menuitem_set_tag(void *self, const char *sel, long value)
{ (void)sel; if (self) ((menuitem_obj *)self)->tag = value; }
static long menuitem_tag(void *self, const char *sel)
{ (void)sel; return self ? ((menuitem_obj *)self)->tag : 0; }
static void menuitem_set_state(void *self, const char *sel, long value)
{ (void)sel; if (self) ((menuitem_obj *)self)->state = (signed char)value; }
static long menuitem_state(void *self, const char *sel)
{ (void)sel; return self ? ((menuitem_obj *)self)->state : 0; }
static signed char menuitem_is_separator(void *self, const char *sel)
{ (void)sel; return self ? ((menuitem_obj *)self)->separator : 0; }

static void menuitem_perform_click(void *self, const char *sel, void *sender)
{
    menuitem_obj *item = self;
    void *app_cls, *app, *imp;
    (void)sel;
    if (!item || item->separator || !item->enabled || !item->action) return;
    app_cls = objc_getClass("NSApplication");
    imp = app_cls ? plsr_objc_lookup_imp(app_cls, "sharedApplication") : NULL;
    app = imp ? ((void *(*)(void *, const char *))imp)(app_cls,
                                                       "sharedApplication") : NULL;
    imp = app ? plsr_objc_lookup_imp(app, "sendAction:to:from:") : NULL;
    if (imp)
        ((signed char (*)(void *, const char *, void *, void *, void *))imp)(
            app, "sendAction:to:from:", item->action, item->target, sender);
}

static void menuitem_set_hidden(void *self, const char *sel, signed char on)
{ (void)sel; if (self) ((menuitem_obj *)self)->hidden = on; }
static signed char menuitem_is_hidden(void *self, const char *sel)
{ (void)sel; return self ? ((menuitem_obj *)self)->hidden : 0; }

static void menuitem_set_represented_object(void *self, const char *sel, void *obj)
{ (void)sel; if (self) ((menuitem_obj *)self)->represented_object = obj; }
static void *menuitem_represented_object(void *self, const char *sel)
{ (void)sel; return self ? ((menuitem_obj *)self)->represented_object : NULL; }

static void menuitem_set_image(void *self, const char *sel, void *image)
{ (void)sel; if (self) ((menuitem_obj *)self)->image = image; }
static void *menuitem_image(void *self, const char *sel)
{ (void)sel; return self ? ((menuitem_obj *)self)->image : NULL; }

static void *menuitem_separator(void *cls, const char *sel)
{
    menuitem_obj *i = obj_new((plsr_class *)cls, sizeof(menuitem_obj));
    (void)sel;
    if (i) {
        i->enabled = 1;
        i->separator = 1;
    }
    return i;
}

static method_array NSMenuItem_methods = {
    24, 28, {
        { "initWithTitle:action:keyEquivalent:", "",
          (void *)menuitem_init_with_title },
        { "title", "", (void *)menuitem_title },
        { "setTitle:", "", (void *)menuitem_set_title },
        { "target", "", (void *)menuitem_target },
        { "setTarget:", "", (void *)menuitem_set_target },
        { "action", "", (void *)menuitem_action },
        { "setAction:", "", (void *)menuitem_set_action },
        { "setSubmenu:", "", (void *)menuitem_set_submenu },
        { "submenu", "", (void *)menuitem_submenu },
        { "setKeyEquivalent:", "", (void *)menuitem_set_key },
        { "keyEquivalent", "", (void *)menuitem_key },
        { "setKeyEquivalentModifierMask:", "", (void *)menuitem_set_key_mask },
        { "keyEquivalentModifierMask", "", (void *)menuitem_key_mask },
        { "setEnabled:", "", (void *)menuitem_set_enabled },
        { "isEnabled", "", (void *)menuitem_is_enabled },
        { "setAlternate:", "", (void *)menuitem_set_alternate },
        { "isAlternate", "", (void *)menuitem_is_alternate },
        { "tag", "", (void *)menuitem_tag },
        { "setTag:", "", (void *)menuitem_set_tag },
        { "state", "", (void *)menuitem_state },
        { "setState:", "", (void *)menuitem_set_state },
        { "isSeparatorItem", "", (void *)menuitem_is_separator },
        { "setHidden:", "", (void *)menuitem_set_hidden },
        { "isHidden", "", (void *)menuitem_is_hidden },
        { "setRepresentedObject:", "", (void *)menuitem_set_represented_object },
        { "representedObject", "", (void *)menuitem_represented_object },
        { "setImage:", "", (void *)menuitem_set_image },
        { "image", "", (void *)menuitem_image },
    }
};

static method_array NSMenuItem_class_methods = {
    24, 2, {
        { "alloc", "", (void *)menuitem_alloc },
        { "separatorItem", "", (void *)menuitem_separator },
    }
};

/* ---- NSToolbar / NSToolbarItem ----------------------------------------- */

static void *toolbar_alloc(void *cls, const char *sel)
{
    (void)sel;
    return obj_new(cls, sizeof(toolbar_obj));
}

static void *toolbar_init_with_identifier(void *self, const char *sel, void *ident)
{
    (void)sel;
    ((toolbar_obj *)self)->identifier = ident;
    ((toolbar_obj *)self)->visible = 1;
    return self;
}

static void toolbar_set_delegate(void *self, const char *sel, void *delegate)
{
    (void)sel;
    ((toolbar_obj *)self)->delegate = delegate;
    toolbar_load_default_items(self);
}

static void *toolbar_delegate(void *self, const char *sel)
{ (void)sel; return self ? ((toolbar_obj *)self)->delegate : NULL; }

static void *toolbar_identifier(void *self, const char *sel)
{ (void)sel; return self ? ((toolbar_obj *)self)->identifier : NULL; }

static void toolbar_set_visible(void *self, const char *sel, signed char visible)
{ (void)sel; if (self) ((toolbar_obj *)self)->visible = visible; }

static signed char toolbar_is_visible(void *self, const char *sel)
{ (void)sel; return self ? ((toolbar_obj *)self)->visible : 0; }

static void *toolbar_items(void *self, const char *sel)
{
    void *cls;
    void *arr;
    void *(*add)(void *, const char *, void *);
    toolbar_obj *t = self;
    (void)sel;
    cls = objc_getClass("NSMutableArray");
    add = cls ? (void *(*)(void *, const char *, void *))plsr_objc_lookup_imp(
                    cls, "arrayWithCapacity:") : NULL;
    arr = add ? add(cls, "arrayWithCapacity:", NULL) : NULL;
    if (!arr || !t)
        return arr;
    add = (void *(*)(void *, const char *, void *))plsr_objc_lookup_imp(
        arr, "addObject:");
    for (int i = 0; add && i < t->nitems; i++)
        add(arr, "addObject:", t->items[i]);
    return arr;
}

static void toolbar_insert_item(void *self, const char *sel, void *ident, long index)
{
    toolbar_obj *t = self;
    void *item_cls = objc_getClass("NSToolbarItem");
    void *item;
    void *(*delegate_item)(void *, const char *, void *, void *, signed char);
    void **items;
    int delegated;
    (void)sel;
    if (!t) return;
    if (index < 0) index = 0;
    if (index > t->nitems) index = t->nitems;
    delegate_item = t->delegate
        ? (void *(*)(void *, const char *, void *, void *, signed char))
            plsr_objc_lookup_imp(t->delegate,
                "toolbar:itemForItemIdentifier:willBeInsertedIntoToolbar:")
        : NULL;
    delegated = delegate_item != NULL;
    item = delegate_item
        ? delegate_item(t->delegate,
            "toolbar:itemForItemIdentifier:willBeInsertedIntoToolbar:",
            t, ident, 1)
        : NULL;
    if (!item && !t->delegate && item_cls) {
        item = obj_new((plsr_class *)item_cls, sizeof(toolbar_item_obj));
        if (item) {
            ((toolbar_item_obj *)item)->enabled = 1;
            ((toolbar_item_obj *)item)->autovalidates = 1;
        }
    }
    if (!item)
        return;
    toolbar_item_obj *toolbar_item = item;
    if (!toolbar_item->identifier) toolbar_item->identifier = ident;
    toolbar_item->toolbar = t;
    if (!delegated && !toolbar_item->enabled) toolbar_item->enabled = 1;
    items = realloc(t->items, (size_t)(t->nitems + 1) * sizeof(*t->items));
    if (!items) return;
    t->items = items;
    memmove(&t->items[index + 1], &t->items[index],
            (size_t)(t->nitems - index) * sizeof(*t->items));
    t->items[index] = item;
    t->nitems++;
}

static void toolbar_load_default_items(void *self)
{
    toolbar_obj *toolbar = self;
    void *delegate, *identifiers, *imp;
    void *(*call)(void *, const char *, void *);
    long (*count_fn)(void *, const char *);
    void *(*at_fn)(void *, const char *, long);
    if (!toolbar || toolbar->nitems || !toolbar->delegate) return;
    delegate = toolbar->delegate;
    imp = plsr_objc_lookup_imp(delegate, "toolbarDefaultItemIdentifiers:");
    if (!imp) return;
    call = (void *(*)(void *, const char *, void *))imp;
    identifiers = call(delegate, "toolbarDefaultItemIdentifiers:", toolbar);
    if (!identifiers) return;
    count_fn = (long (*)(void *, const char *))
        plsr_objc_lookup_imp(identifiers, "count");
    at_fn = (void *(*)(void *, const char *, long))
        plsr_objc_lookup_imp(identifiers, "objectAtIndex:");
    if (!count_fn || !at_fn) return;
    long count = count_fn(identifiers, "count");
    for (long i = 0; i < count && i < 512; ++i) {
        void *identifier = at_fn(identifiers, "objectAtIndex:", i);
        if (identifier)
            toolbar_insert_item(toolbar,
                "insertItemWithItemIdentifier:atIndex:", identifier, i);
    }
}

static void toolbar_remove_item_at_index(void *self, const char *sel, long index)
{
    toolbar_obj *t = self;
    (void)sel;
    if (!t || index < 0 || index >= t->nitems) return;
    ((toolbar_item_obj *)t->items[index])->toolbar = NULL;
    memmove(&t->items[index], &t->items[index + 1],
            (size_t)(t->nitems - index - 1) * sizeof(*t->items));
    t->nitems--;
}

static void *toolbaritem_alloc(void *cls, const char *sel)
{
    (void)sel;
    toolbar_item_obj *item = obj_new(cls, sizeof(toolbar_item_obj));
    if (item) { item->enabled = 1; item->autovalidates = 1; }
    return item;
}

static void *toolbaritem_init_with_identifier(void *self, const char *sel,
                                              void *identifier)
{
    (void)sel;
    if (self) ((toolbar_item_obj *)self)->identifier = identifier;
    return self;
}

static void toolbaritem_set_identifier(void *self, const char *sel,
                                       void *identifier)
{ (void)sel; if (self) ((toolbar_item_obj *)self)->identifier = identifier; }

static void toolbaritem_set_view(void *self, const char *sel, void *view)
{
    (void)sel;
    ((toolbar_item_obj *)self)->view = view;
}

static void *toolbaritem_view(void *self, const char *sel)
{
    (void)sel;
    return ((toolbar_item_obj *)self)->view;
}

static void toolbaritem_set_label(void *self, const char *sel, void *label)
{
    (void)sel;
    ((toolbar_item_obj *)self)->label = label;
}

static void *toolbaritem_label(void *self, const char *sel)
{
    (void)sel;
    return ((toolbar_item_obj *)self)->label;
}

static void toolbaritem_set_target(void *self, const char *sel, void *target)
{
    (void)sel;
    ((toolbar_item_obj *)self)->target = target;
}

static void *toolbaritem_target(void *self, const char *sel)
{
    (void)sel;
    return ((toolbar_item_obj *)self)->target;
}

static void toolbaritem_set_action(void *self, const char *sel, void *action)
{
    (void)sel;
    ((toolbar_item_obj *)self)->action = action;
}

static void *toolbaritem_action(void *self, const char *sel)
{
    (void)sel;
    return ((toolbar_item_obj *)self)->action;
}

static void toolbaritem_set_palette_label(void *self, const char *sel, void *v)
{ (void)sel; if (self) ((toolbar_item_obj *)self)->palette_label = v; }
static void *toolbaritem_palette_label(void *self, const char *sel)
{ (void)sel; return self ? ((toolbar_item_obj *)self)->palette_label : NULL; }
static void toolbaritem_set_tool_tip(void *self, const char *sel, void *v)
{ (void)sel; if (self) ((toolbar_item_obj *)self)->tool_tip = v; }
static void *toolbaritem_tool_tip(void *self, const char *sel)
{ (void)sel; return self ? ((toolbar_item_obj *)self)->tool_tip : NULL; }
static void toolbaritem_set_image(void *self, const char *sel, void *v)
{ (void)sel; if (self) ((toolbar_item_obj *)self)->image = v; }
static void *toolbaritem_image(void *self, const char *sel)
{ (void)sel; return self ? ((toolbar_item_obj *)self)->image : NULL; }
static void toolbaritem_set_enabled(void *self, const char *sel, signed char v)
{ (void)sel; if (self) ((toolbar_item_obj *)self)->enabled = v; }
static signed char toolbaritem_is_enabled(void *self, const char *sel)
{ (void)sel; return self ? ((toolbar_item_obj *)self)->enabled : 0; }
static void toolbaritem_set_autovalidates(void *self, const char *sel, signed char v)
{ (void)sel; if (self) ((toolbar_item_obj *)self)->autovalidates = v; }
static signed char toolbaritem_autovalidates(void *self, const char *sel)
{ (void)sel; return self ? ((toolbar_item_obj *)self)->autovalidates : 0; }
static void *toolbaritem_identifier(void *self, const char *sel)
{ (void)sel; return self ? ((toolbar_item_obj *)self)->identifier : NULL; }

static method_array NSToolbar_methods = {
    24, 10, {
        { "initWithIdentifier:", "", (void *)toolbar_init_with_identifier },
        { "identifier", "", (void *)toolbar_identifier },
        { "setDelegate:", "", (void *)toolbar_set_delegate },
        { "delegate", "", (void *)toolbar_delegate },
        { "items", "", (void *)toolbar_items },
        { "insertItemWithItemIdentifier:atIndex:", "", (void *)toolbar_insert_item },
        { "removeItemAtIndex:", "", (void *)toolbar_remove_item_at_index },
        { "setVisible:", "", (void *)toolbar_set_visible },
        { "isVisible", "", (void *)toolbar_is_visible },
        { "validateVisibleItems", "", (void *)view_noop },
    }
};

static method_array NSToolbar_class_methods = {
    24, 1, { { "alloc", "", (void *)toolbar_alloc } }
};

static method_array NSToolbarItem_methods = {
    24, 21, {
        { "initWithItemIdentifier:", "", (void *)toolbaritem_init_with_identifier },
        { "setItemIdentifier:", "", (void *)toolbaritem_set_identifier },
        { "setView:", "", (void *)toolbaritem_set_view },
        { "view", "", (void *)toolbaritem_view },
        { "itemIdentifier", "", (void *)toolbaritem_identifier },
        { "setLabel:", "", (void *)toolbaritem_set_label },
        { "label", "", (void *)toolbaritem_label },
        { "setPaletteLabel:", "", (void *)toolbaritem_set_palette_label },
        { "paletteLabel", "", (void *)toolbaritem_palette_label },
        { "setToolTip:", "", (void *)toolbaritem_set_tool_tip },
        { "toolTip", "", (void *)toolbaritem_tool_tip },
        { "setImage:", "", (void *)toolbaritem_set_image },
        { "image", "", (void *)toolbaritem_image },
        { "setTarget:", "", (void *)toolbaritem_set_target },
        { "target", "", (void *)toolbaritem_target },
        { "setAction:", "", (void *)toolbaritem_set_action },
        { "action", "", (void *)toolbaritem_action },
        { "setEnabled:", "", (void *)toolbaritem_set_enabled },
        { "isEnabled", "", (void *)toolbaritem_is_enabled },
        { "setAutovalidates:", "", (void *)toolbaritem_set_autovalidates },
        { "autovalidates", "", (void *)toolbaritem_autovalidates },
    }
};

static method_array NSToolbarItem_class_methods = {
    24, 1, { { "alloc", "", (void *)toolbaritem_alloc } }
};

/* ---- NSResponder ------------------------------------------------------- */

static void *responder_init(void *self, const char *sel)
{
    (void)sel;
    return self;
}

static method_array NSResponder_methods = {
    24, 1, { { "init", "", (void *)responder_init } }
};

static method_array NSResponder_class_methods = {
    24, 1, { { "alloc", "", (void *)view_alloc } }
};

/* ---- NSBox ------------------------------------------------------------- */

static void box_draw(void *self, const char *sel, rect r)
{
    view_obj *v = self;
    (void)sel;
    draw_rect_local(r.x, r.y, r.w, r.h,
                    v->bg_rgba ? v->bg_rgba : 0xf7f8faffu);
    draw_frame_local(r.x, r.y, r.w, r.h, 1.0, 0xc2c8d0ffu);
}

static void box_set_fill(void *self, const char *sel, void *color)
{
    color_obj *c = color;
    (void)sel;
    ((view_obj *)self)->bg_rgba = c ? c->rgba : 0;
}

static method_array NSBox_methods = {
    24, 2, {
        { "drawRect:", "", (void *)box_draw },
        { "setFillColor:", "", (void *)box_set_fill },
    }
};

/* ---- NSStackView ------------------------------------------------------- */

static void stack_set_orientation(void *self, const char *sel, long orientation)
{
    (void)sel;
    ((view_obj *)self)->stack_orientation = (int)orientation;
}

static long stack_orientation(void *self, const char *sel)
{
    (void)sel;
    return ((view_obj *)self)->stack_orientation;
}

static void stack_set_spacing(void *self, const char *sel, double spacing)
{
    (void)sel;
    ((view_obj *)self)->spacing = spacing;
}

static double stack_spacing(void *self, const char *sel)
{
    (void)sel;
    return ((view_obj *)self)->spacing;
}

static void stack_add_arranged(void *self, const char *sel, void *child)
{
    (void)sel;
    view_add_subview(self, "addSubview:", child);
}

static void *stack_arranged(void *self, const char *sel)
{
    return view_subviews(self, sel);
}

static void stack_layout(void *self, const char *sel)
{
    view_obj *v = self;
    (void)sel;
    if (!v)
        return;
    if (v->stack_orientation == 0) {
        double x = 0.0;
        for (int i = 0; i < v->nsubviews; i++) {
            view_obj *c = v->subviews[i];
            c->frame.x = x;
            c->frame.y = 0.0;
            c->frame.h = v->frame.h;
            x += c->frame.w + v->spacing;
        }
    } else {
        double y = v->frame.h;
        for (int i = 0; i < v->nsubviews; i++) {
            view_obj *c = v->subviews[i];
            c->frame.x = 0.0;
            c->frame.w = v->frame.w;
            y -= c->frame.h;
            c->frame.y = y;
            y -= v->spacing;
        }
    }
}

static method_array NSStackView_methods = {
    24, 7, {
        { "setOrientation:", "", (void *)stack_set_orientation },
        { "orientation", "", (void *)stack_orientation },
        { "setSpacing:", "", (void *)stack_set_spacing },
        { "spacing", "", (void *)stack_spacing },
        { "addArrangedSubview:", "", (void *)stack_add_arranged },
        { "arrangedSubviews", "", (void *)stack_arranged },
        { "layout", "", (void *)stack_layout },
    }
};

/* ---- plain view subclasses the nibs instantiate ------------------------ */

#define DECLARE_VIEW_SUBCLASS(n)                                              \
    DECLARE_CLASS(n)                                                          \
    static const plsr_class_ro n##_ro =                                       \
        RO(0, sizeof(view_obj), #n, &NSView_methods);                         \
    static const plsr_class_ro n##_meta_ro =                                  \
        RO(PLSR_RO_META, sizeof(view_obj), #n, &NSView_class_methods);

DECLARE_VIEW_SUBCLASS(NSClipView)
DECLARE_VIEW_SUBCLASS(NSSegmentedControl)
DECLARE_VIEW_SUBCLASS(NSImageView)
DECLARE_VIEW_SUBCLASS(NSSwitch)
DECLARE_VIEW_SUBCLASS(NSScroller)
DECLARE_VIEW_SUBCLASS(NSTableView)
DECLARE_VIEW_SUBCLASS(NSOutlineView)
DECLARE_VIEW_SUBCLASS(NSTableHeaderView)

DECLARE_CLASS(NSBox)
static const plsr_class_ro NSBox_ro = RO(0, sizeof(view_obj), "NSBox", &NSBox_methods);
static const plsr_class_ro NSBox_meta_ro = RO(PLSR_RO_META, sizeof(view_obj), "NSBox", &NSView_class_methods);

DECLARE_CLASS(NSStackView)
static const plsr_class_ro NSStackView_ro = RO(0, sizeof(view_obj), "NSStackView", &NSStackView_methods);
static const plsr_class_ro NSStackView_meta_ro = RO(PLSR_RO_META, sizeof(view_obj), "NSStackView", &NSView_class_methods);

/* ---- host surface ------------------------------------------------------ */

plsr_surface *plsr_surface_new(int width, int height)
{
    plsr_surface *s;
    if (width < 1)
        width = 1;
    if (height < 1)
        height = 1;
    s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->pixels = calloc((size_t)width * (size_t)height, 4);
    if (!s->pixels) {
        free(s);
        return NULL;
    }
    s->width = width;
    s->height = height;
    s->needs_display = 1;
    s->clip[0] = (plsr_rect){ 0, 0, width, height };
    return s;
}

void plsr_surface_free(plsr_surface *s)
{
    if (!s)
        return;
    if (s->window && ((window_obj *)s->window)->surface == s)
        ((window_obj *)s->window)->surface = NULL;
    free(s->pixels);
    free(s);
}

int plsr_surface_width(const plsr_surface *s)
{
    return s ? s->width : 0;
}

int plsr_surface_height(const plsr_surface *s)
{
    return s ? s->height : 0;
}

const unsigned char *plsr_surface_pixels(const plsr_surface *s)
{
    return s ? s->pixels : NULL;
}

void plsr_surface_resize(plsr_surface *s, int width, int height)
{
    unsigned char *pixels;
    if (!s)
        return;
    if (width < 1)
        width = 1;
    if (height < 1)
        height = 1;
    if (width == s->width && height == s->height)
        return;
    pixels = calloc((size_t)width * (size_t)height, 4);
    if (!pixels)
        return;
    free(s->pixels);
    s->pixels = pixels;
    s->width = width;
    s->height = height;
    s->needs_display = 1;
}

void plsr_surface_clear(plsr_surface *s, unsigned rgba)
{
    if (!s)
        return;
    for (int i = 0; i < s->width * s->height; i++) {
        s->pixels[i * 4] = (unsigned char)((rgba >> 24) & 0xffu);
        s->pixels[i * 4 + 1] = (unsigned char)((rgba >> 16) & 0xffu);
        s->pixels[i * 4 + 2] = (unsigned char)((rgba >> 8) & 0xffu);
        s->pixels[i * 4 + 3] = (unsigned char)(rgba & 0xffu);
    }
}

void plsr_surface_attach_window(plsr_surface *s, void *window)
{
    window_obj *w = window;
    if (!s)
        return;
    s->window = window;
    if (w) {
        w->surface = s;
        if (w->content_view) {
            rect r = { 0, 0, s->width, s->height };
            view_set_frame(w->content_view, "setFrame:", r);
        }
    }
    s->needs_display = 1;
}

void *plsr_surface_window(const plsr_surface *s)
{
    return s ? s->window : NULL;
}

void plsr_surface_set_needs_display(plsr_surface *s)
{
    if (s)
        s->needs_display = 1;
}

int plsr_surface_needs_display(const plsr_surface *s)
{
    return s ? s->needs_display : 0;
}

void plsr_surface_layout(plsr_surface *s)
{
    window_obj *w;
    if (!s || !(w = s->window))
        return;
    if (w->content_view) {
        rect r = { 0, 0, s->width, s->height };
        view_set_frame(w->content_view, "setFrame:", r);
        send0(w->content_view, "layoutSubtreeIfNeeded");
    }
    s->needs_display = 1;
}

static void draw_view_tree(view_obj *v, double ox, double oy)
{
    plsr_surface *s = g_draw;
    double w, h;
    int saved;
    if (!v || !s || v->hidden)
        return;
    w = v->frame.w;
    h = v->frame.h;
    saved = s->clip_depth;
    surface_push_clip(s, ox, (double)s->height - oy - h, w, h);
    {
        double px = g_draw_x, py = g_draw_y, ph = g_draw_h;
        rect bounds = { 0, 0, w, h };
        void (*fn)(void *, const char *, rect);
        g_draw_x = ox;
        g_draw_y = oy;
        g_draw_h = h;
        fn = (void (*)(void *, const char *, rect))plsr_objc_lookup_imp(
            v, "drawRect:");
        if (fn)
            fn(v, "drawRect:", bounds);
        for (int i = 0; i < v->nsubviews; i++) {
            view_obj *c = v->subviews[i];
            draw_view_tree(c, ox + c->frame.x, oy + c->frame.y);
        }
        g_draw_x = px;
        g_draw_y = py;
        g_draw_h = ph;
    }
    s->clip_depth = saved;
}

void plsr_surface_render(plsr_surface *s)
{
    window_obj *w;
    if (!s)
        return;
    s->clip_depth = 1;
    s->clip[0] = (plsr_rect){ 0, 0, s->width, s->height };
    plsr_surface_clear(s, 0xf2f2f2ffu);
    if ((w = s->window) && w->content_view) {
        g_draw = s;
        draw_view_tree(w->content_view, 0.0, 0.0);
        g_draw = NULL;
    }
    s->needs_display = 0;
}

void *plsr_surface_hit_test(plsr_surface *s, double x, double y)
{
    window_obj *w;
    if (!s || !(w = s->window) || !w->content_view)
        return NULL;
    return view_hit_test(w->content_view, "hitTest:", x,
                         (double)s->height - y);
}

static void event_dispatch(view_obj *v, const char *sel, void *event)
{
    while (v) {
        void (*fn)(void *, const char *, void *);
        fn = (void (*)(void *, const char *, void *))plsr_objc_lookup_imp(v,
                                                                        sel);
        if (fn) {
            fn(v, sel, event);
            return;
        }
        v = v->next_responder ? v->next_responder : v->superview;
    }
}

void plsr_surface_mouse(plsr_surface *s, int type, double x, double y,
                        int button)
{
    window_obj *w;
    view_obj *content, *hit;
    double wy;
    void *ev;
    if (!s || !(w = s->window) || !(content = w->content_view))
        return;
    wy = (double)s->height - y;
    ev = event_new(type == PLSR_MOUSE_DOWN || type == PLSR_MOUSE_UP ? 1 : 6, x,
                   wy, button, 0, NULL, 0);
    if (type == PLSR_MOUSE_DOWN) {
        hit = view_hit_test(content, "hitTest:", x, wy);
        w->mouse_view = hit;
        if (hit) {
            send1(w, "makeFirstResponder:", hit);
            event_dispatch(hit, "mouseDown:", ev);
        }
    } else if (type == PLSR_MOUSE_UP) {
        if (w->mouse_view)
            event_dispatch((view_obj *)w->mouse_view, "mouseUp:", ev);
        w->mouse_view = NULL;
    } else if (type == PLSR_MOUSE_MOVED) {
        hit = view_hit_test(content, "hitTest:", x, wy);
        if (hit)
            event_dispatch(hit, "mouseMoved:", ev);
    } else {
        if (w->mouse_view)
            event_dispatch((view_obj *)w->mouse_view, "mouseDragged:", ev);
    }
    s->needs_display = 1;
}

static int menu_key_equivalent(app_obj *app, menu_obj *menu, void *event)
{
    signed char (*perform)(void *, const char *, void *);
    (void)app;
    if (!menu || !event) return 0;
    perform = (signed char (*)(void *, const char *, void *))
        plsr_objc_lookup_imp(menu, "performKeyEquivalent:");
    return perform ? perform(menu, "performKeyEquivalent:", event) : 0;
}

void plsr_surface_key(plsr_surface *s, unsigned keycode, const char *chars,
                      unsigned modifiers)
{
    window_obj *w;
    void *target, *ev;
    if (!s || !(w = s->window))
        return;
    unsigned long cocoa_modifiers = 0;
    if (modifiers & PLSR_MOD_SHIFT) cocoa_modifiers |= 1ul << 17;
    if (modifiers & PLSR_MOD_CONTROL) cocoa_modifiers |= 1ul << 18;
    if (modifiers & PLSR_MOD_OPTION) cocoa_modifiers |= 1ul << 19;
    if (modifiers & PLSR_MOD_COMMAND) cocoa_modifiers |= 1ul << 20;
    ev = event_new(10, 0, 0, 0, cocoa_modifiers, chars, keycode);
    if (chars && *chars &&
        menu_key_equivalent(g_app, g_app ? g_app->main_menu : NULL, ev))
        return;
    target = w->first_responder ? w->first_responder : w->content_view;
    event_dispatch((view_obj *)target, "keyDown:", ev);
    s->needs_display = 1;
}

unsigned plsr_surface_ink_pixels(const plsr_surface *s, unsigned bg)
{
    unsigned n = 0;
    unsigned char br = (unsigned char)((bg >> 24) & 0xffu);
    unsigned char bgc = (unsigned char)((bg >> 16) & 0xffu);
    unsigned char bb = (unsigned char)((bg >> 8) & 0xffu);
    if (!s)
        return 0;
    for (int i = 0; i < s->width * s->height; i++) {
        const unsigned char *p = s->pixels + i * 4;
        if (p[0] != br || p[1] != bgc || p[2] != bb)
            n++;
    }
    return n;
}

unsigned plsr_surface_distinct_colors(const plsr_surface *s)
{
    unsigned table[4096];
    unsigned char used[4096];
    unsigned count = 0;
    if (!s)
        return 0;
    memset(used, 0, sizeof used);
    for (int i = 0; i < s->width * s->height; i++) {
        const unsigned char *p = s->pixels + i * 4;
        unsigned v = ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) |
                     ((unsigned)p[2] << 8) | p[3];
        unsigned h = (v * 2654435761u) & 4095u;
        while (used[h] && table[h] != v)
            h = (h + 1u) & 4095u;
        if (!used[h]) {
            used[h] = 1;
            table[h] = v;
            count++;
        }
    }
    return count;
}

int plsr_surface_write_ppm(const plsr_surface *s, const char *path)
{
    FILE *f;
    if (!s || !path || !(f = fopen(path, "wb")))
        return 0;
    fprintf(f, "P6\n%d %d\n255\n", s->width, s->height);
    for (int i = 0; i < s->width * s->height; i++)
        fwrite(s->pixels + i * 4, 1, 3, f);
    fclose(f);
    return 1;
}

/* ---- class metadata ---------------------------------------------------- */

static const plsr_class_ro NSResponder_ro = RO(0, 64, "NSResponder", &NSResponder_methods);
static const plsr_class_ro NSResponder_meta_ro = RO(PLSR_RO_META, 64, "NSResponder", &NSResponder_class_methods);
static const plsr_class_ro NSView_ro = RO(0, sizeof(view_obj), "NSView", &NSView_methods);
static const plsr_class_ro NSView_meta_ro = RO(PLSR_RO_META, sizeof(view_obj), "NSView", &NSView_class_methods);
static const plsr_class_ro NSWindow_ro = RO(0, sizeof(window_obj), "NSWindow", &NSWindow_methods);
static const plsr_class_ro NSWindow_meta_ro = RO(PLSR_RO_META, sizeof(window_obj), "NSWindow", &NSWindow_class_methods);
static const plsr_class_ro NSApplication_ro = RO(0, sizeof(app_obj), "NSApplication", &NSApplication_methods);
static const plsr_class_ro NSApplication_meta_ro = RO(PLSR_RO_META, sizeof(app_obj), "NSApplication", &NSApplication_class_methods);
static const plsr_class_ro NSButton_ro = RO(0, sizeof(control_obj), "NSButton", &NSButton_methods);
static const plsr_class_ro NSButton_meta_ro = RO(PLSR_RO_META, sizeof(control_obj), "NSButton", &NSButton_class_methods);
static const plsr_class_ro NSTextField_ro = RO(0, sizeof(control_obj), "NSTextField", &NSTextField_methods);
static const plsr_class_ro NSTextField_meta_ro = RO(PLSR_RO_META, sizeof(control_obj), "NSTextField", &NSTextField_class_methods);
static const plsr_class_ro NSTextView_ro = RO(0, sizeof(control_obj), "NSTextView", &NSTextView_methods);
static const plsr_class_ro NSTextView_meta_ro = RO(PLSR_RO_META, sizeof(control_obj), "NSTextView", &NSTextView_class_methods);
static const plsr_class_ro NSScrollView_ro = RO(0, sizeof(view_obj), "NSScrollView", &NSScrollView_methods);
static const plsr_class_ro NSScrollView_meta_ro = RO(PLSR_RO_META, sizeof(view_obj), "NSScrollView", &NSScrollView_class_methods);
static const plsr_class_ro NSMenu_ro = RO(0, sizeof(menu_obj), "NSMenu", &NSMenu_methods);
static const plsr_class_ro NSMenu_meta_ro = RO(PLSR_RO_META, sizeof(menu_obj), "NSMenu", &NSMenu_class_methods);
static const plsr_class_ro NSMenuItem_ro = RO(0, sizeof(menuitem_obj), "NSMenuItem", &NSMenuItem_methods);
static const plsr_class_ro NSMenuItem_meta_ro = RO(PLSR_RO_META, sizeof(menuitem_obj), "NSMenuItem", &NSMenuItem_class_methods);
static const plsr_class_ro NSToolbar_ro = RO(0, sizeof(toolbar_obj), "NSToolbar", &NSToolbar_methods);
static const plsr_class_ro NSToolbar_meta_ro = RO(PLSR_RO_META, sizeof(toolbar_obj), "NSToolbar", &NSToolbar_class_methods);
static const plsr_class_ro NSToolbarItem_ro = RO(0, sizeof(toolbar_item_obj), "NSToolbarItem", &NSToolbarItem_methods);
static const plsr_class_ro NSToolbarItem_meta_ro = RO(PLSR_RO_META, sizeof(toolbar_item_obj), "NSToolbarItem", &NSToolbarItem_class_methods);

static const plsr_class_ro NSColor_ro = RO(0, sizeof(color_obj), "NSColor", &NSColor_methods);
static const plsr_class_ro NSColor_meta_ro = RO(PLSR_RO_META, sizeof(color_obj), "NSColor", &NSColor_class_methods);
static const plsr_class_ro NSEvent_ro = RO(0, sizeof(event_obj), "NSEvent", &NSEvent_methods);
static const plsr_class_ro NSEvent_meta_ro = RO(PLSR_RO_META, sizeof(event_obj), "NSEvent", &NSEvent_class_methods);

static void wire(plsr_class *cls, plsr_class *meta, plsr_class *super,
                 plsr_class *meta_super, const plsr_class_ro *ro,
                 const plsr_class_ro *meta_ro)
{
    cls->isa = meta;
    cls->superclass = super;
    cls->data = (uintptr_t)ro;
    meta->isa = meta;
    meta->superclass = meta_super;
    meta->data = (uintptr_t)meta_ro;
    plsr_objc_register_class(cls);
}

/* ---- entry points ------------------------------------------------------ */

/* A shipped app's window and menus come from its main storyboard: the
 * storyboard's MainMenu nib installs the delegate and the main menu, and the
 * delegate's applicationDidFinishLaunching: creates the windows. */
static void app_load_main_storyboard(void *app)
{
    void *bcls = objc_getClass("NSBundle");
    void *bundle, *name, *sbcls, *sb;
    if (!bcls)
        return;
    bundle = send0(bcls, "mainBundle");
    if (!bundle)
        return;
    sbcls = objc_getClass("NSStoryboard");
    name = send1(bundle, "objectForInfoDictionaryKey:",
                 mkstr("NSMainStoryboardFile"));
    if (name && sbcls) {
        sb = send2(sbcls, "storyboardWithName:bundle:", name, bundle);
        if (sb)
            send1(sb, "loadMainMenuWithOwner:", app);
    }
    name = send1(bundle, "objectForInfoDictionaryKey:", mkstr("NSMainNibFile"));
    if (name)
        send2(bundle, "loadNibNamed:owner:", name, app);
}

int NSApplicationMain(int argc, const char **argv) __asm__("_NSApplicationMain");
int NSApplicationMain(int argc, const char **argv)
{
    void *app_cls = objc_getClass("NSApplication");
    void *(*shared)(void *, const char *);
    void *app;
    (void)argc;
    (void)argv;
    if (!app_cls)
        return 1;
    shared = (void *(*)(void *, const char *))plsr_objc_lookup_imp(
        app_cls, "sharedApplication");
    app = shared ? shared(app_cls, "sharedApplication") : NULL;
    if (!app)
        return 1;
    app_load_main_storyboard(app);
    send0(app, "finishLaunching");
    send0(app, "run");
    return 0;
}

static void appkit_init(void) __attribute__((constructor));
static void appkit_init(void)
{
    plsr_class *nsobj = &plsr_NSObject_class;
    plsr_class *nsobj_meta = &plsr_NSObject_meta;

    wire(&plsr_NSResponder_class, &plsr_NSResponder_meta, nsobj, nsobj_meta,
         &NSResponder_ro, &NSResponder_meta_ro);
    wire(&plsr_NSView_class, &plsr_NSView_meta, &plsr_NSResponder_class,
         &plsr_NSResponder_meta, &NSView_ro, &NSView_meta_ro);
    wire(&plsr_NSWindow_class, &plsr_NSWindow_meta, &plsr_NSResponder_class,
         &plsr_NSResponder_meta, &NSWindow_ro, &NSWindow_meta_ro);
    wire(&plsr_NSApplication_class, &plsr_NSApplication_meta, &plsr_NSResponder_class,
         &plsr_NSResponder_meta, &NSApplication_ro, &NSApplication_meta_ro);
    wire(&plsr_NSButton_class, &plsr_NSButton_meta, &plsr_NSView_class,
         &plsr_NSView_meta, &NSButton_ro, &NSButton_meta_ro);
    wire(&plsr_NSTextField_class, &plsr_NSTextField_meta, &plsr_NSView_class,
         &plsr_NSView_meta, &NSTextField_ro, &NSTextField_meta_ro);
    wire(&plsr_NSTextView_class, &plsr_NSTextView_meta, &plsr_NSView_class,
         &plsr_NSView_meta, &NSTextView_ro, &NSTextView_meta_ro);
    wire(&plsr_NSScrollView_class, &plsr_NSScrollView_meta, &plsr_NSView_class,
         &plsr_NSView_meta, &NSScrollView_ro, &NSScrollView_meta_ro);
    wire(&plsr_NSMenu_class, &plsr_NSMenu_meta, nsobj, nsobj_meta, &NSMenu_ro,
         &NSMenu_meta_ro);
    wire(&plsr_NSMenuItem_class, &plsr_NSMenuItem_meta, nsobj, nsobj_meta,
         &NSMenuItem_ro, &NSMenuItem_meta_ro);
    wire(&plsr_NSToolbar_class, &plsr_NSToolbar_meta, nsobj, nsobj_meta,
         &NSToolbar_ro, &NSToolbar_meta_ro);
    wire(&plsr_NSToolbarItem_class, &plsr_NSToolbarItem_meta, nsobj, nsobj_meta,
         &NSToolbarItem_ro, &NSToolbarItem_meta_ro);

    wire(&plsr_NSColor_class, &plsr_NSColor_meta, nsobj, nsobj_meta,
         &NSColor_ro, &NSColor_meta_ro);
    wire(&plsr_NSEvent_class, &plsr_NSEvent_meta, nsobj, nsobj_meta,
         &NSEvent_ro, &NSEvent_meta_ro);

    wire(&plsr_NSBox_class, &plsr_NSBox_meta, &plsr_NSView_class,
         &plsr_NSView_meta, &NSBox_ro, &NSBox_meta_ro);
    wire(&plsr_NSClipView_class, &plsr_NSClipView_meta, &plsr_NSView_class,
         &plsr_NSView_meta, &NSClipView_ro, &NSClipView_meta_ro);
    wire(&plsr_NSStackView_class, &plsr_NSStackView_meta, &plsr_NSView_class,
         &plsr_NSView_meta, &NSStackView_ro, &NSStackView_meta_ro);
    wire(&plsr_NSSegmentedControl_class, &plsr_NSSegmentedControl_meta,
         &plsr_NSView_class, &plsr_NSView_meta, &NSSegmentedControl_ro,
         &NSSegmentedControl_meta_ro);
    wire(&plsr_NSImageView_class, &plsr_NSImageView_meta, &plsr_NSView_class,
         &plsr_NSView_meta, &NSImageView_ro, &NSImageView_meta_ro);
    wire(&plsr_NSSwitch_class, &plsr_NSSwitch_meta, &plsr_NSView_class,
         &plsr_NSView_meta, &NSSwitch_ro, &NSSwitch_meta_ro);
    wire(&plsr_NSScroller_class, &plsr_NSScroller_meta, &plsr_NSView_class,
         &plsr_NSView_meta, &NSScroller_ro, &NSScroller_meta_ro);
    wire(&plsr_NSTableView_class, &plsr_NSTableView_meta, &plsr_NSView_class,
         &plsr_NSView_meta, &NSTableView_ro, &NSTableView_meta_ro);
    wire(&plsr_NSOutlineView_class, &plsr_NSOutlineView_meta, &plsr_NSView_class,
         &plsr_NSView_meta, &NSOutlineView_ro, &NSOutlineView_meta_ro);
    wire(&plsr_NSTableHeaderView_class, &plsr_NSTableHeaderView_meta,
         &plsr_NSView_class, &plsr_NSView_meta, &NSTableHeaderView_ro,
         &NSTableHeaderView_meta_ro);

    /* View subclasses answer the generic view selectors too. */
    plsr_objc_register_class(&plsr_NSView_class);
}

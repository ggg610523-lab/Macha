/*
 * Host surface ABI for the clean-room AppKit slice.
 *
 * A host (a toolkit window, a headless test, or the loader's Mayonnaise
 * bridge) owns a surface: an RGBA buffer sized in points.  It attaches the
 * NSWindow whose content view tree should fill it, asks AppKit to lay the tree
 * out and render it, and forwards mouse and key input back through the real
 * NSView responder chain.
 *
 * Coordinates in the surface are top-left origin, like a framebuffer.  AppKit
 * coordinates stay bottom-left origin inside the views; the surface converts.
 */
#ifndef PLSR_APPKIT_SURFACE_H
#define PLSR_APPKIT_SURFACE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct plsr_surface plsr_surface;

typedef struct {
    double x;
    double y;
    double width;
    double height;
} plsr_rect;

typedef struct {
    double x;
    double y;
} plsr_point;

/* ---- surface ---------------------------------------------------------- */

plsr_surface *plsr_surface_new(int width, int height);
void plsr_surface_free(plsr_surface *surface);
int plsr_surface_width(const plsr_surface *surface);
int plsr_surface_height(const plsr_surface *surface);
const unsigned char *plsr_surface_pixels(const plsr_surface *surface);

void plsr_surface_clear(plsr_surface *surface, unsigned rgba);
/* Reallocate at a new size; call plsr_surface_layout() afterwards. */
void plsr_surface_resize(plsr_surface *surface, int width, int height);
void plsr_surface_attach_window(plsr_surface *surface, void *ns_window);
void *plsr_surface_window(const plsr_surface *surface);
void plsr_surface_set_needs_display(plsr_surface *surface);
int plsr_surface_needs_display(const plsr_surface *surface);

/* Size the window's content view to the surface and run layoutSubtreeIfNeeded. */
void plsr_surface_layout(plsr_surface *surface);
/* Clear and draw the content view tree into the surface. */
void plsr_surface_render(plsr_surface *surface);

/* ---- input ------------------------------------------------------------ */

void *plsr_surface_hit_test(plsr_surface *surface, double x, double y);
void plsr_surface_mouse(plsr_surface *surface, int type, double x, double y,
                        int button);
void plsr_surface_key(plsr_surface *surface, unsigned keycode, const char *chars,
                      unsigned modifiers);

enum {
    PLSR_MOUSE_DOWN = 0,
    PLSR_MOUSE_UP = 1,
    PLSR_MOUSE_MOVED = 2,
    PLSR_MOUSE_DRAGGED = 3
};

enum {
    PLSR_MOD_SHIFT = 1,
    PLSR_MOD_CONTROL = 2,
    PLSR_MOD_OPTION = 4,
    PLSR_MOD_COMMAND = 8
};

/* ---- inspection ------------------------------------------------------- */

int plsr_surface_write_ppm(const plsr_surface *surface, const char *path);
unsigned plsr_surface_ink_pixels(const plsr_surface *surface,
                                 unsigned background_rgba);
unsigned plsr_surface_distinct_colors(const plsr_surface *surface);

#ifdef __cplusplus
}
#endif

#endif

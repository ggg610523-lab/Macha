#ifndef PLSR_SWIFTUI_ENGINE_H
#define PLSR_SWIFTUI_ENGINE_H

#include <math.h>
#include <stddef.h>

struct sw_node;

/* ------------------------------------------------------------------ */
/* Framework manifest (generated from the shipped export lists)        */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    const char *install_name;
    unsigned exports;
    unsigned functions;
    unsigned data;
} plsr_swiftui_framework;

const plsr_swiftui_framework *plsr_swiftui_frameworks(size_t *count);
const plsr_swiftui_framework *plsr_swiftui_framework_at(size_t index);
const plsr_swiftui_framework *plsr_swiftui_framework_named(const char *name);
const plsr_swiftui_framework *plsr_swiftui_framework_for_install_name(
    const char *install_name);

/* ------------------------------------------------------------------ */
/* Geometry                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    double width;
    double height;
} sw_size;

typedef struct {
    double x;
    double y;
} sw_point;

typedef struct {
    double x;
    double y;
    double width;
    double height;
} sw_rect;

/* NAN means "unspecified": the parent decides, as SwiftUI's
 * ProposedViewSize.nil does. */
typedef struct {
    double width;
    double height;
} sw_proposal;

sw_proposal sw_proposal_unspecified(void);
sw_proposal sw_proposal_size(double width, double height);
int sw_proposal_has_width(sw_proposal p);
int sw_proposal_has_height(sw_proposal p);

/* ------------------------------------------------------------------ */
/* Colour and fonts                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    double r;
    double g;
    double b;
    double a;
} sw_color;

sw_color sw_rgb(double r, double g, double b);
sw_color sw_rgba(double r, double g, double b, double a);
sw_color sw_color_hex(unsigned rgb);
sw_color sw_color_clear(void);

typedef struct {
    double size;
    int weight;      /* 400 regular, 600 semibold, 700 bold */
    int monospaced;
} sw_font;

sw_font sw_font_system(double size);
sw_font sw_font_bold(double size);
sw_font sw_font_semibold(double size);
sw_font sw_font_mono(double size);

double sw_text_line_height(const sw_font *font);
double sw_text_advance(const sw_font *font);
double sw_text_width(const sw_font *font, const char *text);
void sw_text_measure(const sw_font *font, const char *text, double max_width,
                     double *out_width, double *out_height, int *out_lines);

/* ------------------------------------------------------------------ */
/* Values (the type-erased Binding<T> payload)                         */
/* ------------------------------------------------------------------ */

typedef enum {
    SW_VALUE_NONE = 0,
    SW_VALUE_DOUBLE,
    SW_VALUE_INT,
    SW_VALUE_BOOL,
    SW_VALUE_STRING,
    SW_VALUE_COLOR,
    SW_VALUE_FONT
} sw_value_kind;

typedef struct {
    sw_value_kind kind;
    union {
        double number;
        int integer;
        int boolean;
        const char *string;
        sw_color color;
        sw_font font;
    } as;
} sw_value;

sw_value sw_value_none(void);
sw_value sw_value_double(double number);
sw_value sw_value_int(int integer);
sw_value sw_value_bool(int boolean);
sw_value sw_value_string(const char *string);
sw_value sw_value_color(sw_color color);
sw_value sw_value_font(sw_font font);

double sw_value_as_double(sw_value value, double fallback);
int sw_value_as_int(sw_value value, int fallback);
int sw_value_as_bool(sw_value value, int fallback);
const char *sw_value_as_string(sw_value value, const char *fallback);
sw_color sw_value_as_color(sw_value value, sw_color fallback);
sw_font sw_value_as_font(sw_value value, sw_font fallback);

/* ------------------------------------------------------------------ */
/* Environment                                                         */
/* ------------------------------------------------------------------ */

typedef struct sw_environment sw_environment;

#define SW_ENV_COLOR_SCHEME "colorScheme"
#define SW_ENV_FONT "font"
#define SW_ENV_FOREGROUND_COLOR "foregroundColor"
#define SW_ENV_ACCENT_COLOR "accentColor"
#define SW_ENV_LINE_LIMIT "lineLimit"

sw_environment *sw_environment_new(void);
void sw_environment_free(sw_environment *environment);
void sw_environment_set(sw_environment *environment, const char *key,
                        sw_value value);
int sw_environment_has(const sw_environment *environment, const char *key);
sw_value sw_environment_value(const sw_environment *environment,
                              const char *key);

/* Resolve a key the way layout does: the node's own overrides first, then
 * its ancestors, then the host environment. */
sw_value sw_environment_resolve(const sw_environment *root,
                                const struct sw_node *node, const char *key);

/* ------------------------------------------------------------------ */
/* Binding / State / Observable                                        */
/* ------------------------------------------------------------------ */

typedef sw_value (*sw_binding_getter)(void *context);
typedef void (*sw_binding_setter)(void *context, sw_value value);

typedef struct sw_binding sw_binding;

sw_binding *sw_binding_new(sw_binding_getter get, sw_binding_setter set,
                           void *context);
sw_binding *sw_binding_constant(sw_value value);
sw_value sw_binding_get(const sw_binding *binding);
void sw_binding_set(sw_binding *binding, sw_value value);
void sw_binding_free(sw_binding *binding);

typedef struct sw_state sw_state;

sw_state *sw_state_new(sw_value initial);
void sw_state_free(sw_state *state);
sw_value sw_state_value(const sw_state *state);
void sw_state_set(sw_state *state, sw_value value);
unsigned sw_state_revision(const sw_state *state);
sw_binding *sw_state_binding(sw_state *state);

/* Bumped by every state and observable write; the host uses it to decide
 * whether the last rendered frame is stale. */
unsigned long sw_state_generation(void);

typedef void (*sw_observer_fn)(void *context, const char *key, sw_value value);

typedef struct sw_observable sw_observable;

sw_observable *sw_observable_new(void);
void sw_observable_free(sw_observable *observable);
sw_value sw_observable_get(const sw_observable *observable, const char *key);
void sw_observable_set(sw_observable *observable, const char *key,
                       sw_value value);
sw_binding *sw_observable_binding(sw_observable *observable, const char *key);
void sw_observable_watch(sw_observable *observable, sw_observer_fn observer,
                         void *context);
unsigned long sw_observable_generation(const sw_observable *observable);

/* ------------------------------------------------------------------ */
/* View graph                                                          */
/* ------------------------------------------------------------------ */

typedef enum {
    SW_GROUP = 0,
    SW_TEXT,
    SW_IMAGE,
    SW_SPACER,
    SW_DIVIDER,
    SW_RECTANGLE,
    SW_ROUNDED_RECTANGLE,
    SW_CIRCLE,
    SW_CAPSULE,
    SW_PATH,
    SW_HSTACK,
    SW_VSTACK,
    SW_ZSTACK,
    SW_BUTTON,
    SW_LIST,
    SW_FOREACH,
    SW_TEXT_FIELD,
    SW_WINDOW
} sw_kind;

typedef enum { SW_LEADING = 0, SW_CENTER, SW_TRAILING } sw_align_h;
typedef enum { SW_TOP = 0, SW_MIDDLE, SW_BOTTOM } sw_align_v;

typedef struct sw_node sw_node;
struct sw_node_state;

struct sw_node {
    sw_kind kind;
    char *text;
    char *symbol;

    sw_color foreground;
    sw_color background;
    sw_color fill;
    sw_font font;

    struct {
        double top;
        double leading;
        double bottom;
        double trailing;
    } padding;

    double radius;
    double opacity;
    double offset_x;
    double offset_y;
    double spacing;
    double titlebar_height;

    double fixed_w;
    double fixed_h;
    double min_w;
    double min_h;
    double max_w;
    double max_h;
    int flex_w;
    int flex_h;

    int align_h;
    int align_v;
    int line_limit;
    int clip;
    int visible;
    int focused;

    /* explicit styling wins over the inherited environment */
    int font_explicit;
    int foreground_explicit;
    int background_explicit;

    sw_environment *environment;

    sw_node **children;
    size_t count;
    size_t capacity;
    sw_node *parent;

    struct sw_node_state *states;
    size_t state_count;

    /* layout results */
    sw_rect frame;
    int text_lines;
    double wrap_width;
};

sw_node *sw_group(void);
sw_node *sw_text(const char *text);
sw_node *sw_styled_text(const char *text, double size, int weight);
sw_node *sw_hstack(double spacing);
sw_node *sw_vstack(double spacing);
sw_node *sw_zstack(void);
sw_node *sw_spacer(void);
sw_node *sw_divider(void);
sw_node *sw_rectangle(void);
sw_node *sw_rounded_rectangle(double radius);
sw_node *sw_circle(void);
sw_node *sw_capsule(void);
sw_node *sw_image(const char *symbol, double size);
sw_node *sw_button(const char *title);
sw_node *sw_list(void);
sw_node *sw_foreach(void);
sw_node *sw_text_field(const char *placeholder, const char *value);
sw_node *sw_text_field_focus(sw_node *node, int focused);
sw_node *sw_window(const char *title, double width, double height);

sw_node *sw_add(sw_node *parent, sw_node *child);
sw_node *sw_foreground(sw_node *node, sw_color color);
sw_node *sw_background(sw_node *node, sw_color color);
sw_node *sw_fill(sw_node *node, sw_color color);
sw_node *sw_padding(sw_node *node, double all);
sw_node *sw_padding_edges(sw_node *node, double top, double leading,
                          double bottom, double trailing);
sw_node *sw_frame(sw_node *node, double width, double height);
sw_node *sw_frame_min(sw_node *node, double width, double height);
sw_node *sw_frame_max(sw_node *node, double width, double height);
sw_node *sw_flex(sw_node *node);
sw_node *sw_flex_width(sw_node *node);
sw_node *sw_flex_height(sw_node *node);
sw_node *sw_opacity(sw_node *node, double opacity);
sw_node *sw_offset(sw_node *node, double x, double y);
sw_node *sw_clip(sw_node *node);
sw_node *sw_corner_radius(sw_node *node, double radius);
sw_node *sw_line_limit(sw_node *node, int lines);
sw_node *sw_align(sw_node *node, sw_align_h horizontal, sw_align_v vertical);
sw_node *sw_font_apply(sw_node *node, sw_font font);
sw_node *sw_spacing(sw_node *node, double spacing);
void sw_text_set(sw_node *node, const char *text);
void sw_node_free(sw_node *node);

sw_node *sw_environment_apply(sw_node *node, const char *key, sw_value value);
sw_node *sw_environment_color(sw_node *node, const char *key, sw_color color);
sw_node *sw_environment_font(sw_node *node, sw_font font);
sw_node *sw_environment_int(sw_node *node, const char *key, int integer);
sw_node *sw_environment_bool(sw_node *node, const char *key, int boolean);
sw_node *sw_environment_string(sw_node *node, const char *key,
                               const char *string);
sw_node *sw_environment_dark(sw_node *node);

sw_state *sw_node_state(sw_node *node, const char *key, sw_value initial);
void sw_node_states_free(sw_node *node);

void sw_layout(sw_node *root, sw_proposal proposal);
void sw_layout_environment(sw_node *root, sw_proposal proposal,
                           const sw_environment *environment);
void sw_dump(const sw_node *root, int depth);
const char *sw_kind_name(sw_kind kind);

/* ------------------------------------------------------------------ */
/* Canvas                                                              */
/* ------------------------------------------------------------------ */

typedef struct sw_canvas sw_canvas;

sw_canvas *sw_canvas_new(int width, int height);
void sw_canvas_free(sw_canvas *canvas);
int sw_canvas_width(const sw_canvas *canvas);
int sw_canvas_height(const sw_canvas *canvas);
const unsigned char *sw_canvas_pixels(const sw_canvas *canvas);

void sw_canvas_clear(sw_canvas *canvas, sw_color color);
void sw_canvas_fill_rect(sw_canvas *canvas, double x, double y, double width,
                         double height, sw_color color);
void sw_canvas_fill_round_rect(sw_canvas *canvas, double x, double y,
                               double width, double height, double radius,
                               sw_color color);
void sw_canvas_fill_circle(sw_canvas *canvas, double cx, double cy,
                           double radius, sw_color color);
void sw_canvas_stroke_round_rect(sw_canvas *canvas, double x, double y,
                                 double width, double height, double radius,
                                 double line_width, sw_color color);
void sw_canvas_draw_text(sw_canvas *canvas, const sw_font *font,
                         const char *text, double x, double y, double max_width,
                         sw_color color);
void sw_canvas_push_clip(sw_canvas *canvas, double x, double y, double width,
                         double height);
void sw_canvas_pop_clip(sw_canvas *canvas);
unsigned sw_canvas_distinct_colors(const sw_canvas *canvas);
unsigned sw_canvas_ink_pixels(const sw_canvas *canvas, sw_color background);
int sw_canvas_write_png(const sw_canvas *canvas, const char *path);
void sw_canvas_print_ansi(const sw_canvas *canvas, int columns);
void sw_canvas_print_ascii(const sw_canvas *canvas, int columns);

void sw_draw(const sw_node *root, sw_canvas *canvas);
void sw_draw_environment(const sw_node *root, sw_canvas *canvas,
                         const sw_environment *environment);

/* Host primitive adapter: the engine resolves layout and style, the host
 * supplies rectangle/text primitives (cutefish toolkit, CGContext, ...). */
typedef struct sw_painter sw_painter;

struct sw_painter {
    void *context;
    void (*rect)(void *context, sw_rect rect, sw_color color, double radius);
    void (*text)(void *context, const char *text, sw_rect rect, sw_font font,
                 sw_color color);
};

void sw_paint(const sw_node *root, const sw_environment *environment,
              const sw_painter *painter);

/* ------------------------------------------------------------------ */
/* Hosting (the NSHostingView equivalent)                              */
/* ------------------------------------------------------------------ */

typedef struct sw_host sw_host;

sw_host *sw_host_new(double width, double height);
void sw_host_free(sw_host *host);
void sw_host_set_background(sw_host *host, sw_color color);
void sw_host_set_root(sw_host *host, sw_node *root);
void sw_host_resize(sw_host *host, double width, double height);
void sw_host_render(sw_host *host);
sw_canvas *sw_host_canvas(sw_host *host);
int sw_host_write_png(sw_host *host, const char *path);

sw_environment *sw_host_environment(sw_host *host);
void sw_host_set_environment(sw_host *host, sw_environment *environment);
int sw_host_needs_render(const sw_host *host);
unsigned long sw_host_revision(const sw_host *host);

/* NSHostingView-shaped wrapper around the hosting surface. */
typedef sw_host sw_hosting_view;

sw_hosting_view *sw_hosting_view_new(sw_node *root, double width,
                                     double height);
void sw_hosting_view_free(sw_hosting_view *view);
void sw_hosting_view_set_root(sw_hosting_view *view, sw_node *root);
void sw_hosting_view_set_frame(sw_hosting_view *view, double width,
                               double height);
void sw_hosting_view_layout(sw_hosting_view *view);
void sw_hosting_view_draw(sw_hosting_view *view);
sw_size sw_hosting_view_intrinsic_size(const sw_hosting_view *view);
int sw_hosting_view_needs_display(const sw_hosting_view *view);

#endif

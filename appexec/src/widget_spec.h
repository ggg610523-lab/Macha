/* Widget bridge between the loaded AppKit view tree and the host window.
 *
 * The loader walks the app's real NSView hierarchy and describes each control
 * as a widget the host can instantiate (button, text field, label, editor).
 * Every widget keeps the NSView it came from, so host input is delivered back
 * through that view's own API (-performClick:, -setStringValue:) instead of
 * being painted as static text.
 */
#ifndef PLSR_WIDGET_SPEC_H
#define PLSR_WIDGET_SPEC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PLSR_WIDGET_CONTAINER_V = 0,
    PLSR_WIDGET_CONTAINER_H,
    PLSR_WIDGET_LABEL,
    PLSR_WIDGET_BUTTON,
    PLSR_WIDGET_TEXTFIELD,
    PLSR_WIDGET_EDITOR,
    PLSR_WIDGET_CHECKBOX,
    PLSR_WIDGET_TOGGLE,
    PLSR_WIDGET_SLIDER,
    PLSR_WIDGET_PROGRESS,
    PLSR_WIDGET_SEGMENTED,
    PLSR_WIDGET_IMAGE,
    PLSR_WIDGET_SEPARATOR,
    PLSR_WIDGET_MENU_BUTTON,
    PLSR_WIDGET_LIST,
    PLSR_WIDGET_KIND_COUNT
} plsr_widget_kind;

typedef struct plsr_widget {
    plsr_widget_kind kind;
    char *text;      /* label/button title, field value or editor contents */
    char *detail;    /* placeholder or secondary text */
    char *glyph;     /* optional Mayonnaise vector icon name */
    void *view;      /* the NSView this widget represents (may be NULL) */
    double x, y, width, height; /* frame in the parent NSView coordinate space */
    double value, minimum, maximum, alpha;
    unsigned has_frame : 1;
    unsigned hidden : 1;
    unsigned enabled : 1;
    unsigned flipped : 1;
    unsigned selected : 1;
    unsigned focused : 1; /* AppKit window's live first responder */
    struct plsr_widget **kids;
    size_t nkids;
    size_t cap;
} plsr_widget;

/* Actions the host sends back to the app. */
enum {
    PLSR_WIDGET_ACTION_CLICK = 1,  /* -performClick: on the control */
    PLSR_WIDGET_ACTION_TEXT = 2,   /* set displayed string */
    PLSR_WIDGET_ACTION_VALUE = 3,  /* set numeric control value */
    PLSR_WIDGET_ACTION_COMMAND = 4, /* NSApplication responder-chain action */
    PLSR_WIDGET_ACTION_FOCUS = 5, /* make the real AppKit view first responder */
    PLSR_WIDGET_ACTION_CLOSE = 6, /* close the source AppKit window */
    PLSR_WIDGET_ACTION_MENU = 7  /* dispatch a source NSMenuItem action */
};

/* Queue an action for the app thread.  Safe to call from the host UI thread. */
void plsr_widget_action(void *view, int action, const char *text);

/* Drain queued actions on the app thread, invoking the real AppKit methods. */
void plsr_widget_drain_actions(void);

/* Match the supplied key and Mayo modifier bits against the archived main
 * menu, then enqueue that menu item's real target/action pair. */
int plsr_widget_dispatch_menu_shortcut(char key, unsigned modifiers);

/* Spec for a presented NSWindow, or NULL when it has no controls yet. */
plsr_widget *plsr_widget_spec_for_window(void *ns_window);

#ifdef __cplusplus
}
#endif

#endif

#define _GNU_SOURCE
#include "engine.h"

#include <stdlib.h>

struct sw_host {
    double width;
    double height;
    sw_color background;
    sw_node *root;
    sw_canvas *canvas;
    sw_environment *environment;
    unsigned long rendered_generation;
};

sw_host *sw_host_new(double width, double height)
{
    sw_host *host = calloc(1, sizeof(*host));
    if (!host)
        return NULL;
    host->width = width > 0.0 ? width : 1.0;
    host->height = height > 0.0 ? height : 1.0;
    host->background = sw_rgb(1.0, 1.0, 1.0);
    return host;
}

void sw_host_free(sw_host *host)
{
    if (!host)
        return;
    sw_canvas_free(host->canvas);
    free(host);
}

void sw_host_set_background(sw_host *host, sw_color color)
{
    if (host)
        host->background = color;
}

void sw_host_set_root(sw_host *host, sw_node *root)
{
    if (!host)
        return;
    host->root = root;
    if (root)
        sw_layout_environment(root, sw_proposal_size(host->width, host->height),
                              host->environment);
}

void sw_host_resize(sw_host *host, double width, double height)
{
    if (!host)
        return;
    host->width = width > 0.0 ? width : 1.0;
    host->height = height > 0.0 ? height : 1.0;
    sw_canvas_free(host->canvas);
    host->canvas = NULL;
    if (host->root)
        sw_layout_environment(host->root,
                              sw_proposal_size(host->width, host->height),
                              host->environment);
}

void sw_host_render(sw_host *host)
{
    if (!host || !host->root)
        return;

    int width = (int)(host->width + 0.5);
    int height = (int)(host->height + 0.5);
    if (width < 1)
        width = 1;
    if (height < 1)
        height = 1;

    if (!host->canvas || sw_canvas_width(host->canvas) != width ||
        sw_canvas_height(host->canvas) != height) {
        sw_canvas_free(host->canvas);
        host->canvas = sw_canvas_new(width, height);
        if (!host->canvas)
            return;
    }

    sw_canvas_clear(host->canvas, host->background);
    sw_layout_environment(host->root, sw_proposal_size(host->width,
                                                       host->height),
                          host->environment);
    sw_draw_environment(host->root, host->canvas, host->environment);
    host->rendered_generation = sw_state_generation();
}

sw_canvas *sw_host_canvas(sw_host *host)
{
    return host ? host->canvas : NULL;
}

int sw_host_write_png(sw_host *host, const char *path)
{
    if (!host || !host->canvas)
        return 0;
    return sw_canvas_write_png(host->canvas, path);
}

sw_environment *sw_host_environment(sw_host *host)
{
    return host ? host->environment : NULL;
}

void sw_host_set_environment(sw_host *host, sw_environment *environment)
{
    if (!host)
        return;
    host->environment = environment;
    if (host->root)
        sw_layout_environment(host->root,
                              sw_proposal_size(host->width, host->height),
                              host->environment);
}

int sw_host_needs_render(const sw_host *host)
{
    return host ? sw_state_generation() != host->rendered_generation : 0;
}

unsigned long sw_host_revision(const sw_host *host)
{
    return host ? host->rendered_generation : 0;
}

/* ------------------------------------------------------------------ */
/* NSHostingView-shaped wrapper                                        */
/* ------------------------------------------------------------------ */

sw_hosting_view *sw_hosting_view_new(sw_node *root, double width, double height)
{
    sw_host *view = sw_host_new(width, height);
    if (view)
        sw_host_set_root(view, root);
    return view;
}

void sw_hosting_view_free(sw_hosting_view *view)
{
    sw_host_free(view);
}

void sw_hosting_view_set_root(sw_hosting_view *view, sw_node *root)
{
    sw_host_set_root(view, root);
}

void sw_hosting_view_set_frame(sw_hosting_view *view, double width,
                               double height)
{
    sw_host_resize(view, width, height);
}

void sw_hosting_view_layout(sw_hosting_view *view)
{
    if (!view || !view->root)
        return;
    sw_layout_environment(view->root,
                          sw_proposal_size(view->width, view->height),
                          view->environment);
}

void sw_hosting_view_draw(sw_hosting_view *view)
{
    sw_host_render(view);
}

sw_size sw_hosting_view_intrinsic_size(const sw_hosting_view *view)
{
    sw_size size = { 0.0, 0.0 };
    if (view && view->root) {
        size.width = view->root->frame.width;
        size.height = view->root->frame.height;
    }
    return size;
}

int sw_hosting_view_needs_display(const sw_hosting_view *view)
{
    return sw_host_needs_render(view);
}

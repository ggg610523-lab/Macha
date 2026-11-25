#define _GNU_SOURCE
#include "engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Construction                                                        */
/* ------------------------------------------------------------------ */

static sw_node *node_new(sw_kind kind)
{
    sw_node *node = calloc(1, sizeof(*node));
    if (!node)
        return NULL;

    node->kind = kind;
    node->opacity = 1.0;
    node->visible = 1;
    node->titlebar_height = 38.0;
    node->fixed_w = NAN;
    node->fixed_h = NAN;
    node->min_w = NAN;
    node->min_h = NAN;
    node->max_w = NAN;
    node->max_h = NAN;
    node->wrap_width = NAN;
    node->font = sw_font_system(16.0);
    node->foreground = sw_rgb(0.11, 0.12, 0.14);
    node->fill = sw_rgb(0.55, 0.57, 0.60);
    node->background = sw_color_clear();
    node->align_h = SW_LEADING;
    node->align_v = SW_TOP;
    return node;
}

sw_node *sw_group(void)
{
    return node_new(SW_GROUP);
}

sw_node *sw_text(const char *text)
{
    sw_node *node = node_new(SW_TEXT);
    if (!node)
        return NULL;
    node->text = strdup(text ? text : "");
    return node;
}

sw_node *sw_styled_text(const char *text, double size, int weight)
{
    sw_node *node = sw_text(text);
    if (!node)
        return NULL;
    node->font.size = size > 0.0 ? size : 16.0;
    node->font.weight = weight;
    node->font_explicit = 1;
    return node;
}

sw_node *sw_hstack(double spacing)
{
    sw_node *node = node_new(SW_HSTACK);
    if (node)
        node->spacing = spacing;
    return node;
}

sw_node *sw_vstack(double spacing)
{
    sw_node *node = node_new(SW_VSTACK);
    if (node)
        node->spacing = spacing;
    return node;
}

sw_node *sw_zstack(void)
{
    return node_new(SW_ZSTACK);
}

sw_node *sw_spacer(void)
{
    sw_node *node = node_new(SW_SPACER);
    if (node) {
        node->flex_w = 1;
        node->flex_h = 1;
    }
    return node;
}

sw_node *sw_divider(void)
{
    sw_node *node = node_new(SW_DIVIDER);
    if (node)
        node->fill = sw_rgb(0.85, 0.86, 0.88);
    return node;
}

sw_node *sw_rectangle(void)
{
    return node_new(SW_RECTANGLE);
}

sw_node *sw_rounded_rectangle(double radius)
{
    sw_node *node = node_new(SW_ROUNDED_RECTANGLE);
    if (node)
        node->radius = radius;
    return node;
}

sw_node *sw_circle(void)
{
    return node_new(SW_CIRCLE);
}

sw_node *sw_capsule(void)
{
    return node_new(SW_CAPSULE);
}

sw_node *sw_image(const char *symbol, double size)
{
    sw_node *node = node_new(SW_IMAGE);
    if (!node)
        return NULL;
    node->text = strdup(symbol ? symbol : "");
    node->background = sw_rgb(0.89, 0.90, 0.92);
    node->background_explicit = 1;
    if (size > 0.0) {
        node->fixed_w = size;
        node->fixed_h = size;
    }
    return node;
}

sw_node *sw_button(const char *title)
{
    sw_node *node = node_new(SW_BUTTON);
    if (!node)
        return NULL;
    node->text = strdup(title ? title : "Button");
    node->font = sw_font_semibold(15.0);
    node->font_explicit = 1;
    node->padding.top = 7.0;
    node->padding.leading = 14.0;
    node->padding.bottom = 7.0;
    node->padding.trailing = 14.0;
    node->radius = 6.0;
    node->foreground = sw_rgb(1.0, 1.0, 1.0);
    node->foreground_explicit = 1;
    node->min_h = 30.0;
    return node;
}

sw_node *sw_window(const char *title, double width, double height)
{
    sw_node *node = node_new(SW_WINDOW);
    if (!node)
        return NULL;
    node->text = strdup(title ? title : "");
    node->radius = 10.0;
    node->flex_w = 1;
    node->flex_h = 1;
    if (width > 0.0)
        node->fixed_w = width;
    if (height > 0.0)
        node->fixed_h = height;
    return node;
}

sw_node *sw_list(void)
{
    sw_node *node = node_new(SW_LIST);
    if (node) {
        node->flex_w = 1;
        node->flex_h = 1;
    }
    return node;
}

sw_node *sw_foreach(void)
{
    return node_new(SW_FOREACH);
}

sw_node *sw_text_field(const char *placeholder, const char *value)
{
    sw_node *node = node_new(SW_TEXT_FIELD);
    if (!node)
        return NULL;
    node->symbol = strdup(placeholder ? placeholder : "");
    node->text = strdup(value ? value : "");
    node->font = sw_font_system(14.0);
    node->radius = 6.0;
    node->min_h = 30.0;
    return node;
}

sw_node *sw_text_field_focus(sw_node *node, int focused)
{
    if (node)
        node->focused = focused;
    return node;
}

sw_node *sw_add(sw_node *parent, sw_node *child)
{
    if (!parent || !child)
        return parent;

    if (parent->count == parent->capacity) {
        size_t capacity = parent->capacity ? parent->capacity * 2 : 4;
        sw_node **children = realloc(parent->children,
                                     capacity * sizeof(*children));
        if (!children)
            return parent;
        parent->children = children;
        parent->capacity = capacity;
    }

    parent->children[parent->count++] = child;
    child->parent = parent;
    return parent;
}

sw_node *sw_foreground(sw_node *node, sw_color color)
{
    if (node) {
        node->foreground = color;
        node->foreground_explicit = 1;
    }
    return node;
}

sw_node *sw_background(sw_node *node, sw_color color)
{
    if (node) {
        node->background = color;
        node->background_explicit = 1;
    }
    return node;
}

sw_node *sw_fill(sw_node *node, sw_color color)
{
    if (node)
        node->fill = color;
    return node;
}

sw_node *sw_padding(sw_node *node, double all)
{
    return sw_padding_edges(node, all, all, all, all);
}

sw_node *sw_padding_edges(sw_node *node, double top, double leading,
                          double bottom, double trailing)
{
    if (!node)
        return node;
    node->padding.top = top;
    node->padding.leading = leading;
    node->padding.bottom = bottom;
    node->padding.trailing = trailing;
    return node;
}

sw_node *sw_frame(sw_node *node, double width, double height)
{
    if (!node)
        return node;
    node->fixed_w = width > 0.0 ? width : NAN;
    node->fixed_h = height > 0.0 ? height : NAN;
    return node;
}

sw_node *sw_frame_min(sw_node *node, double width, double height)
{
    if (!node)
        return node;
    node->min_w = width > 0.0 ? width : NAN;
    node->min_h = height > 0.0 ? height : NAN;
    return node;
}

sw_node *sw_frame_max(sw_node *node, double width, double height)
{
    if (!node)
        return node;
    node->max_w = width > 0.0 ? width : NAN;
    node->max_h = height > 0.0 ? height : NAN;
    return node;
}

sw_node *sw_flex(sw_node *node)
{
    if (!node)
        return node;
    node->flex_w = 1;
    node->flex_h = 1;
    return node;
}

sw_node *sw_flex_width(sw_node *node)
{
    if (node)
        node->flex_w = 1;
    return node;
}

sw_node *sw_flex_height(sw_node *node)
{
    if (node)
        node->flex_h = 1;
    return node;
}

sw_node *sw_opacity(sw_node *node, double opacity)
{
    if (node)
        node->opacity = opacity < 0.0 ? 0.0 : (opacity > 1.0 ? 1.0 : opacity);
    return node;
}

sw_node *sw_offset(sw_node *node, double x, double y)
{
    if (node) {
        node->offset_x = x;
        node->offset_y = y;
    }
    return node;
}

sw_node *sw_clip(sw_node *node)
{
    if (node)
        node->clip = 1;
    return node;
}

sw_node *sw_corner_radius(sw_node *node, double radius)
{
    if (node)
        node->radius = radius;
    return node;
}

sw_node *sw_line_limit(sw_node *node, int lines)
{
    if (node)
        node->line_limit = lines;
    return node;
}

sw_node *sw_align(sw_node *node, sw_align_h horizontal, sw_align_v vertical)
{
    if (node) {
        node->align_h = horizontal;
        node->align_v = vertical;
    }
    return node;
}

sw_node *sw_font_apply(sw_node *node, sw_font font)
{
    if (node) {
        node->font = font;
        node->font_explicit = 1;
    }
    return node;
}

sw_node *sw_spacing(sw_node *node, double spacing)
{
    if (node)
        node->spacing = spacing;
    return node;
}

void sw_text_set(sw_node *node, const char *text)
{
    if (!node)
        return;
    free(node->text);
    node->text = strdup(text ? text : "");
}

void sw_node_free(sw_node *node)
{
    if (!node)
        return;
    for (size_t i = 0; i < node->count; i++)
        sw_node_free(node->children[i]);
    sw_node_states_free(node);
    sw_environment_free(node->environment);
    free(node->children);
    free(node->text);
    free(node->symbol);
    free(node);
}

/* ------------------------------------------------------------------ */
/* Environment resolution                                              */
/* ------------------------------------------------------------------ */

typedef struct sw_env_frame {
    const sw_environment *environment;
    const struct sw_env_frame *parent;
} sw_env_frame;

static sw_value env_lookup(const sw_env_frame *frame, const char *key,
                           sw_value fallback)
{
    for (; frame; frame = frame->parent) {
        if (frame->environment && sw_environment_has(frame->environment, key))
            return sw_environment_value(frame->environment, key);
    }
    return fallback;
}

static int env_is_dark(const sw_env_frame *frame)
{
    sw_value value = env_lookup(frame, SW_ENV_COLOR_SCHEME, sw_value_none());
    if (value.kind == SW_VALUE_INT)
        return value.as.integer != 0;
    if (value.kind == SW_VALUE_BOOL)
        return value.as.boolean != 0;
    if (value.kind == SW_VALUE_STRING && value.as.string)
        return strcmp(value.as.string, "dark") == 0;
    return 0;
}

static sw_font resolved_font(const sw_node *node, const sw_env_frame *frame)
{
    if (node->font_explicit && node->font.size > 0.0)
        return node->font;
    sw_value value = env_lookup(frame, SW_ENV_FONT, sw_value_none());
    if (value.kind == SW_VALUE_FONT && value.as.font.size > 0.0)
        return value.as.font;
    return sw_font_system(16.0);
}

static sw_color resolved_foreground(const sw_node *node,
                                    const sw_env_frame *frame)
{
    if (node->foreground_explicit)
        return node->foreground;
    sw_value value = env_lookup(frame, SW_ENV_FOREGROUND_COLOR, sw_value_none());
    if (value.kind == SW_VALUE_COLOR)
        return value.as.color;
    return env_is_dark(frame) ? sw_rgb(0.92, 0.93, 0.95)
                              : sw_rgb(0.11, 0.12, 0.14);
}

static sw_color resolved_background(const sw_node *node,
                                    const sw_env_frame *frame)
{
    if (node->background_explicit)
        return node->background;
    if (node->kind == SW_WINDOW)
        return env_is_dark(frame) ? sw_rgb(0.11, 0.12, 0.14)
                                  : sw_rgb(1.0, 1.0, 1.0);
    return node->background;
}

static sw_color resolved_accent(const sw_env_frame *frame)
{
    sw_value value = env_lookup(frame, SW_ENV_ACCENT_COLOR, sw_value_none());
    if (value.kind == SW_VALUE_COLOR)
        return value.as.color;
    return sw_rgb(0.16, 0.42, 0.92);
}

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

static double align_h(double child, double parent, int mode)
{
    if (mode == SW_CENTER)
        return (parent - child) / 2.0;
    if (mode == SW_TRAILING)
        return parent - child;
    return 0.0;
}

static double align_v(double child, double parent, int mode)
{
    if (mode == SW_MIDDLE)
        return (parent - child) / 2.0;
    if (mode == SW_BOTTOM)
        return parent - child;
    return 0.0;
}

static void layout_node(sw_node *node, sw_proposal proposal,
                        const sw_env_frame *frame);

static void layout_children(sw_node *node, sw_proposal proposal,
                            sw_proposal inner, const sw_env_frame *frame)
{
    double content_w = 0.0;
    double content_h = 0.0;

    switch (node->kind) {
    case SW_TEXT:
    case SW_BUTTON: {
        double width = 0.0;
        double height = 0.0;
        int lines = 0;
        sw_font font = resolved_font(node, frame);
        sw_text_measure(&font, node->text ? node->text : "", inner.width,
                        &width, &height, &lines);
        node->wrap_width = inner.width;
        node->text_lines = lines;
        content_w = width;
        content_h = height;
        if (node->kind == SW_BUTTON) {
            if (!isnan(inner.width) && inner.width > content_w)
                content_w = inner.width;
            if (!isnan(inner.height) && inner.height > content_h)
                content_h = inner.height;
        }
        break;
    }
    case SW_TEXT_FIELD: {
        sw_font font = resolved_font(node, frame);
        const char *shown = (node->text && node->text[0])
                                ? node->text
                                : (node->symbol ? node->symbol : "");
        double width = 0.0;
        double height = 0.0;
        int lines = 0;
        sw_text_measure(&font, shown, NAN, &width, &height, &lines);
        node->wrap_width = NAN;
        node->text_lines = 1;
        content_w = isnan(inner.width) ? fmax(width + 24.0, 120.0)
                                       : inner.width;
        content_h = fmax(height + 14.0, 30.0);
        break;
    }
    case SW_IMAGE: {
        double width = inner.width;
        double height = inner.height;
        if (isnan(width) && isnan(height)) {
            width = 20.0;
            height = 20.0;
        } else if (isnan(width)) {
            width = height;
        } else if (isnan(height)) {
            height = width;
        }
        content_w = width;
        content_h = height;
        break;
    }
    case SW_SPACER:
        content_w = isnan(inner.width)
                        ? (isnan(node->min_w) ? 0.0 : node->min_w)
                        : inner.width;
        content_h = isnan(inner.height)
                        ? (isnan(node->min_h) ? 0.0 : node->min_h)
                        : inner.height;
        break;
    case SW_DIVIDER: {
        int vertical = node->parent && node->parent->kind == SW_HSTACK;
        if (vertical) {
            content_w = 1.0;
            content_h = isnan(inner.height) ? 0.0 : inner.height;
        } else {
            content_w = isnan(inner.width) ? 0.0 : inner.width;
            content_h = 1.0;
        }
        break;
    }
    case SW_RECTANGLE:
    case SW_ROUNDED_RECTANGLE:
    case SW_CIRCLE:
    case SW_CAPSULE:
    case SW_PATH: {
        double width = inner.width;
        double height = inner.height;
        if (isnan(width) && isnan(height)) {
            width = 10.0;
            height = 10.0;
        } else if (isnan(width)) {
            width = height;
        } else if (isnan(height)) {
            height = node->kind == SW_CAPSULE ? width / 2.0 : width;
        }
        content_w = width;
        content_h = height;
        break;
    }
    case SW_HSTACK: {
        double total = 0.0;
        double max_h = 0.0;
        int flex = 0;

        for (size_t i = 0; i < node->count; i++) {
            sw_node *child = node->children[i];
            if (child->flex_w) {
                flex++;
                continue;
            }
            layout_node(child, sw_proposal_size(NAN, inner.height), frame);
            total += child->frame.width;
            if (child->frame.height > max_h)
                max_h = child->frame.height;
        }
        if (node->count > 1)
            total += node->spacing * (double)(node->count - 1);

        if (flex && !isnan(inner.width)) {
            double extra = inner.width - total;
            if (extra < 0.0)
                extra = 0.0;
            double each = extra / (double)flex;
            for (size_t i = 0; i < node->count; i++) {
                sw_node *child = node->children[i];
                if (!child->flex_w)
                    continue;
                layout_node(child, sw_proposal_size(each, inner.height),
                            frame);
                total += child->frame.width;
                if (child->frame.height > max_h)
                    max_h = child->frame.height;
            }
        }

        content_w = total;
        content_h = isnan(inner.height) ? max_h : inner.height;
        break;
    }
    case SW_VSTACK:
    case SW_LIST:
    case SW_FOREACH: {
        double total = 0.0;
        double max_w = 0.0;
        int flex = 0;

        for (size_t i = 0; i < node->count; i++) {
            sw_node *child = node->children[i];
            if (child->flex_h) {
                flex++;
                continue;
            }
            layout_node(child, sw_proposal_size(inner.width, NAN), frame);
            total += child->frame.height;
            if (child->frame.width > max_w)
                max_w = child->frame.width;
        }
        if (node->count > 1)
            total += node->spacing * (double)(node->count - 1);

        if (flex && !isnan(inner.height)) {
            double extra = inner.height - total;
            if (extra < 0.0)
                extra = 0.0;
            double each = extra / (double)flex;
            for (size_t i = 0; i < node->count; i++) {
                sw_node *child = node->children[i];
                if (!child->flex_h)
                    continue;
                layout_node(child, sw_proposal_size(inner.width, each),
                            frame);
                total += child->frame.height;
                if (child->frame.width > max_w)
                    max_w = child->frame.width;
            }
        }

        content_w = isnan(inner.width) ? max_w : inner.width;
        content_h = total;
        break;
    }
    case SW_ZSTACK: {
        double max_w = 0.0;
        double max_h = 0.0;
        for (size_t i = 0; i < node->count; i++) {
            layout_node(node->children[i], inner, frame);
            if (node->children[i]->frame.width > max_w)
                max_w = node->children[i]->frame.width;
            if (node->children[i]->frame.height > max_h)
                max_h = node->children[i]->frame.height;
        }
        content_w = isnan(inner.width) ? max_w : inner.width;
        content_h = isnan(inner.height) ? max_h : inner.height;
        break;
    }
    case SW_GROUP:
        if (node->count) {
            layout_node(node->children[0], inner, frame);
            content_w = node->children[0]->frame.width;
            content_h = node->children[0]->frame.height;
        }
        break;
    case SW_WINDOW: {
        double bar = (node->text && node->text[0]) ? node->titlebar_height : 0.0;
        sw_proposal child_proposal = inner;
        if (!isnan(child_proposal.height))
            child_proposal.height -= bar;
        for (size_t i = 0; i < node->count; i++)
            layout_node(node->children[i], child_proposal, frame);
        if (!isnan(inner.width)) {
            content_w = inner.width;
        } else {
            content_w = node->count ? node->children[0]->frame.width : 0.0;
        }
        if (!isnan(inner.height)) {
            content_h = inner.height;
        } else {
            content_h = (node->count ? node->children[0]->frame.height : 0.0) +
                        bar;
        }
        break;
    }
    default:
        break;
    }

    double width = content_w + node->padding.leading + node->padding.trailing;
    double height = content_h + node->padding.top + node->padding.bottom;

    if (!isnan(node->fixed_w))
        width = node->fixed_w;
    if (!isnan(node->fixed_h))
        height = node->fixed_h;
    if (isnan(node->fixed_w) && node->flex_w && !isnan(proposal.width))
        width = proposal.width;
    if (isnan(node->fixed_h) && node->flex_h && !isnan(proposal.height))
        height = proposal.height;

    if (!isnan(node->min_w) && width < node->min_w)
        width = node->min_w;
    if (!isnan(node->min_h) && height < node->min_h)
        height = node->min_h;
    if (!isnan(node->max_w) && width > node->max_w)
        width = node->max_w;
    if (!isnan(node->max_h) && height > node->max_h)
        height = node->max_h;

    if (width < 0.0)
        width = 0.0;
    if (height < 0.0)
        height = 0.0;

    node->frame.x = 0.0;
    node->frame.y = 0.0;
    node->frame.width = width;
    node->frame.height = height;

    double box_w = width - node->padding.leading - node->padding.trailing;
    double box_h = height - node->padding.top - node->padding.bottom;
    if (box_w < 0.0)
        box_w = 0.0;
    if (box_h < 0.0)
        box_h = 0.0;

    switch (node->kind) {
    case SW_HSTACK: {
        double x = 0.0;
        for (size_t i = 0; i < node->count; i++) {
            sw_node *child = node->children[i];
            child->frame.x = x;
            child->frame.y = align_v(child->frame.height, box_h,
                                     node->align_v);
            x += child->frame.width + node->spacing;
        }
        break;
    }
    case SW_VSTACK:
    case SW_LIST:
    case SW_FOREACH: {
        double y = 0.0;
        for (size_t i = 0; i < node->count; i++) {
            sw_node *child = node->children[i];
            child->frame.y = y;
            child->frame.x = align_h(child->frame.width, box_w,
                                     node->align_h);
            y += child->frame.height + node->spacing;
        }
        break;
    }
    case SW_ZSTACK:
        for (size_t i = 0; i < node->count; i++) {
            sw_node *child = node->children[i];
            child->frame.x = align_h(child->frame.width, box_w, node->align_h);
            child->frame.y = align_v(child->frame.height, box_h,
                                     node->align_v);
        }
        break;
    case SW_WINDOW: {
        double bar = (node->text && node->text[0]) ? node->titlebar_height : 0.0;
        for (size_t i = 0; i < node->count; i++) {
            node->children[i]->frame.x = 0.0;
            node->children[i]->frame.y = bar;
        }
        break;
    }
    default:
        for (size_t i = 0; i < node->count; i++) {
            node->children[i]->frame.x = 0.0;
            node->children[i]->frame.y = 0.0;
        }
        break;
    }
}

static void layout_node(sw_node *node, sw_proposal proposal,
                        const sw_env_frame *frame)
{
    if (!node || !node->visible)
        return;

    sw_env_frame local;
    if (node->environment) {
        local.environment = node->environment;
        local.parent = frame;
        frame = &local;
    }

    sw_proposal inner = proposal;
    if (!isnan(node->fixed_w))
        inner.width = node->fixed_w;
    else if (node->flex_w && !isnan(proposal.width))
        inner.width = proposal.width;
    if (!isnan(inner.width))
        inner.width -= node->padding.leading + node->padding.trailing;

    if (!isnan(node->fixed_h))
        inner.height = node->fixed_h;
    else if (node->flex_h && !isnan(proposal.height))
        inner.height = proposal.height;
    if (!isnan(inner.height))
        inner.height -= node->padding.top + node->padding.bottom;

    if (!isnan(inner.width) && inner.width < 0.0)
        inner.width = 0.0;
    if (!isnan(inner.height) && inner.height < 0.0)
        inner.height = 0.0;

    layout_children(node, proposal, inner, frame);
}

void sw_layout_environment(sw_node *root, sw_proposal proposal,
                           const sw_environment *environment)
{
    if (!root)
        return;
    sw_env_frame frame = { environment, NULL };
    layout_node(root, proposal, &frame);
}

void sw_layout(sw_node *root, sw_proposal proposal)
{
    sw_layout_environment(root, proposal, NULL);
}

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

static sw_color fade(sw_color color, double alpha)
{
    color.a *= alpha;
    return color;
}

static void draw_node(const sw_node *node, sw_canvas *canvas, double x,
                      double y, double alpha, const sw_env_frame *frame)
{
    if (!node || !node->visible)
        return;

    double a = alpha * node->opacity;
    if (a <= 0.0)
        return;

    sw_env_frame local;
    if (node->environment) {
        local.environment = node->environment;
        local.parent = frame;
        frame = &local;
    }

    x += node->offset_x;
    y += node->offset_y;

    double width = node->frame.width;
    double height = node->frame.height;
    double cx = x + node->padding.leading;
    double cy = y + node->padding.top;
    double cw = width - node->padding.leading - node->padding.trailing;
    double ch = height - node->padding.top - node->padding.bottom;
    if (cw < 0.0)
        cw = 0.0;
    if (ch < 0.0)
        ch = 0.0;

    int clipped = node->clip || node->kind == SW_WINDOW;
    if (clipped)
        sw_canvas_push_clip(canvas, x, y, width, height);

    sw_color background = resolved_background(node, frame);
    if (background.a > 0.0)
        sw_canvas_fill_round_rect(canvas, x, y, width, height, node->radius,
                                  fade(background, a));

    switch (node->kind) {
    case SW_TEXT: {
        sw_font font = resolved_font(node, frame);
        sw_color ink = node->foreground_explicit ? node->foreground
                                                 : resolved_foreground(node, frame);
        sw_canvas_draw_text(canvas, &font, node->text ? node->text : "", cx, cy,
                            node->wrap_width, fade(ink, a));
        break;
    }
    case SW_BUTTON: {
        sw_color plate = node->background_explicit ? node->background
                                                   : resolved_accent(frame);
        sw_font font = resolved_font(node, frame);
        sw_color ink = node->foreground_explicit ? node->foreground
                                                 : sw_rgb(1.0, 1.0, 1.0);
        sw_canvas_fill_round_rect(canvas, x, y, width, height, node->radius,
                                  fade(plate, a));
        double tw = 0.0;
        double th = 0.0;
        int lines = 0;
        sw_text_measure(&font, node->text ? node->text : "", NAN, &tw, &th,
                        &lines);
        double tx = cx + (cw - tw) / 2.0;
        double ty = cy + (ch - th) / 2.0;
        sw_canvas_draw_text(canvas, &font, node->text ? node->text : "", tx, ty,
                            NAN, fade(ink, a));
        break;
    }
    case SW_TEXT_FIELD: {
        int dark = env_is_dark(frame);
        sw_font font = resolved_font(node, frame);
        sw_color plate = node->background_explicit
                             ? node->background
                             : (dark ? sw_color_hex(0x1c1e22) : sw_rgb(1, 1, 1));
        sw_color border = dark ? sw_color_hex(0x3a3d44) : sw_color_hex(0xd0d3d9);
        sw_color ink = node->foreground_explicit
                           ? node->foreground
                           : (dark ? sw_color_hex(0xe6e8ec)
                                   : sw_color_hex(0x2a2d33));
        double radius = node->radius > 0.0 ? node->radius : 6.0;
        int empty = !node->text || !node->text[0];
        const char *shown = empty ? (node->symbol ? node->symbol : "")
                                  : node->text;
        sw_color shown_ink =
            empty ? (dark ? sw_color_hex(0x8b9099) : sw_color_hex(0x9aa0a8))
                  : ink;
        sw_canvas_fill_round_rect(canvas, x, y, width, height, radius,
                                  fade(plate, a));
        sw_canvas_stroke_round_rect(canvas, x, y, width, height, radius, 1.0,
                                    fade(border, a));
        double th = sw_text_line_height(&font);
        double tx = cx + 8.0;
        double ty = cy + (ch - th) / 2.0;
        sw_canvas_draw_text(canvas, &font, shown, tx, ty, cw - 16.0,
                            fade(shown_ink, a));
        if (node->focused && !empty)
            sw_canvas_fill_rect(canvas, tx + sw_text_width(&font, shown) + 1.0,
                                ty, 1.0, th, fade(ink, a));
        break;
    }
    case SW_IMAGE: {
        sw_color plate = node->background_explicit ? node->background
                                                   : sw_color_hex(0xe4e6ea);
        sw_canvas_fill_round_rect(canvas, cx, cy, cw, ch, 5.0,
                                  fade(plate, a));
        if (node->text && node->text[0]) {
            sw_font font = sw_font_bold(11.0);
            double tw = sw_text_width(&font, node->text);
            double ty = cy + (ch - sw_text_line_height(&font)) / 2.0;
            sw_canvas_draw_text(canvas, &font, node->text,
                                cx + (cw - tw) / 2.0, ty, NAN,
                                fade(node->foreground, a));
        }
        break;
    }
    case SW_DIVIDER:
        sw_canvas_fill_rect(canvas, cx, cy, cw, ch, fade(node->fill, a));
        break;
    case SW_RECTANGLE:
    case SW_ROUNDED_RECTANGLE:
    case SW_CIRCLE:
    case SW_CAPSULE:
    case SW_PATH: {
        double radius = node->radius;
        if (node->kind == SW_CIRCLE || node->kind == SW_CAPSULE)
            radius = fmin(cw, ch) / 2.0;
        sw_canvas_fill_round_rect(canvas, cx, cy, cw, ch, radius,
                                  fade(node->fill, a));
        break;
    }
    case SW_WINDOW: {
        if (node->text && node->text[0]) {
            double bar = node->titlebar_height;
            int dark = env_is_dark(frame);
            sw_color band = dark ? sw_color_hex(0x26282d)
                                 : sw_color_hex(0xeceef2);
            sw_canvas_fill_round_rect(canvas, x, y, width, bar, node->radius,
                                      fade(band, a));
            sw_canvas_fill_rect(canvas, x, y + bar / 2.0, width, bar / 2.0,
                                fade(band, a));
            sw_canvas_fill_rect(canvas, x, y + bar - 1.0, width, 1.0,
                                fade(dark ? sw_color_hex(0x3a3d44)
                                          : sw_color_hex(0xd6d9df), a));
            sw_canvas_fill_circle(canvas, x + 20.0, y + bar / 2.0, 6.0,
                                  fade(sw_color_hex(0xff5f57), a));
            sw_canvas_fill_circle(canvas, x + 40.0, y + bar / 2.0, 6.0,
                                  fade(sw_color_hex(0xfebc2e), a));
            sw_canvas_fill_circle(canvas, x + 60.0, y + bar / 2.0, 6.0,
                                  fade(sw_color_hex(0x28c840), a));
            sw_font font = sw_font_semibold(13.0);
            double tw = sw_text_width(&font, node->text);
            double tx = x + (width - tw) / 2.0;
            if (tx < x + 80.0)
                tx = x + 80.0;
            sw_canvas_draw_text(canvas, &font, node->text, tx,
                                y + (bar - sw_text_line_height(&font)) / 2.0,
                                NAN, fade(dark ? sw_color_hex(0xd8dbe1)
                                               : sw_color_hex(0x3c4048), a));
        }
        break;
    }
    default:
        break;
    }

    for (size_t i = 0; i < node->count; i++) {
        const sw_node *child = node->children[i];
        draw_node(child, canvas, cx + child->frame.x, cy + child->frame.y, a,
                  frame);
    }

    if (node->kind == SW_LIST) {
        for (size_t i = 0; i + 1 < node->count; i++) {
            const sw_node *child = node->children[i];
            sw_canvas_fill_rect(canvas, cx + 12.0,
                                cy + child->frame.y + child->frame.height,
                                cw - 24.0, 1.0,
                                fade(sw_color_hex(0xe2e4e8), a));
        }
    }

    if (node->kind == SW_WINDOW)
        sw_canvas_stroke_round_rect(canvas, x + 0.5, y + 0.5, width - 1.0,
                                    height - 1.0, node->radius, 1.0,
                                    fade(sw_color_hex(0xc9ccd3), a));

    if (clipped)
        sw_canvas_pop_clip(canvas);
}

void sw_draw_environment(const sw_node *root, sw_canvas *canvas,
                         const sw_environment *environment)
{
    if (!root || !canvas)
        return;
    sw_env_frame frame = { environment, NULL };
    draw_node(root, canvas, 0.0, 0.0, 1.0, &frame);
}

void sw_draw(const sw_node *root, sw_canvas *canvas)
{
    sw_draw_environment(root, canvas, NULL);
}

/* ------------------------------------------------------------------ */
/* Painter bridge (toolkit / CGContext / any host primitive set)       */
/* ------------------------------------------------------------------ */

static void paint_node(const sw_node *node, const sw_env_frame *frame,
                       const sw_painter *painter, double x, double y,
                       double alpha)
{
    if (!node || !node->visible || !painter)
        return;

    double a = alpha * node->opacity;
    if (a <= 0.0)
        return;

    sw_env_frame local;
    if (node->environment) {
        local.environment = node->environment;
        local.parent = frame;
        frame = &local;
    }

    x += node->offset_x;
    y += node->offset_y;

    double width = node->frame.width;
    double height = node->frame.height;
    double cx = x + node->padding.leading;
    double cy = y + node->padding.top;
    double cw = width - node->padding.leading - node->padding.trailing;
    double ch = height - node->padding.top - node->padding.bottom;
    if (cw < 0.0)
        cw = 0.0;
    if (ch < 0.0)
        ch = 0.0;

    sw_color background = resolved_background(node, frame);
    if (background.a > 0.0 && painter->rect)
        painter->rect(painter->context, (sw_rect){ x, y, width, height },
                      fade(background, a), node->radius);

    switch (node->kind) {
    case SW_TEXT:
    case SW_BUTTON:
    case SW_TEXT_FIELD: {
        sw_font font = resolved_font(node, frame);
        sw_color ink = node->foreground_explicit
                           ? node->foreground
                           : resolved_foreground(node, frame);
        if (node->kind == SW_BUTTON) {
            ink = node->foreground_explicit ? node->foreground
                                            : sw_rgb(1.0, 1.0, 1.0);
            if (!node->background_explicit && painter->rect)
                painter->rect(painter->context,
                              (sw_rect){ x, y, width, height },
                              fade(resolved_accent(frame), a), node->radius);
        }
        if (node->kind == SW_TEXT_FIELD) {
            int dark = env_is_dark(frame);
            double radius = node->radius > 0.0 ? node->radius : 6.0;
            if (painter->rect) {
                painter->rect(painter->context,
                              (sw_rect){ x, y, width, height },
                              fade(dark ? sw_color_hex(0x3a3d44)
                                        : sw_color_hex(0xd0d3d9), a),
                              radius);
                painter->rect(painter->context,
                              (sw_rect){ x + 1.0, y + 1.0, width - 2.0,
                                         height - 2.0 },
                              fade(dark ? sw_color_hex(0x1c1e22)
                                        : sw_rgb(1.0, 1.0, 1.0), a),
                              radius > 1.0 ? radius - 1.0 : 0.0);
            }
            const char *shown = (node->text && node->text[0])
                                    ? node->text
                                    : (node->symbol ? node->symbol : "");
            if (painter->text)
                painter->text(painter->context, shown,
                              (sw_rect){ cx + 8.0, cy, cw - 16.0, ch }, font,
                              fade(ink, a));
            break;
        }
        if (painter->text)
            painter->text(painter->context, node->text ? node->text : "",
                          (sw_rect){ cx, cy, cw, ch }, font, fade(ink, a));
        break;
    }
    case SW_IMAGE:
    case SW_DIVIDER:
    case SW_RECTANGLE:
    case SW_ROUNDED_RECTANGLE:
    case SW_CIRCLE:
    case SW_CAPSULE:
    case SW_PATH: {
        sw_color fill = node->fill;
        double radius = node->radius;
        if (node->kind == SW_IMAGE)
            fill = node->background_explicit ? node->background
                                             : sw_color_hex(0xe4e6ea);
        if (node->kind == SW_CIRCLE || node->kind == SW_CAPSULE)
            radius = fmin(cw, ch) / 2.0;
        if (painter->rect)
            painter->rect(painter->context, (sw_rect){ cx, cy, cw, ch },
                          fade(fill, a), radius);
        break;
    }
    case SW_WINDOW:
        if (node->text && node->text[0] && painter->rect) {
            int dark = env_is_dark(frame);
            painter->rect(painter->context,
                          (sw_rect){ x, y, width, node->titlebar_height },
                          fade(dark ? sw_color_hex(0x26282d)
                                    : sw_color_hex(0xeceef2), a),
                          node->radius);
            if (painter->text)
                painter->text(painter->context, node->text,
                              (sw_rect){ x, y, width, node->titlebar_height },
                              sw_font_semibold(13.0),
                              fade(dark ? sw_color_hex(0xd8dbe1)
                                        : sw_color_hex(0x3c4048), a));
        }
        break;
    default:
        break;
    }

    for (size_t i = 0; i < node->count; i++) {
        const sw_node *child = node->children[i];
        paint_node(child, frame, painter, cx + child->frame.x,
                   cy + child->frame.y, a);
    }

    if (node->kind == SW_LIST && painter->rect) {
        for (size_t i = 0; i + 1 < node->count; i++) {
            const sw_node *child = node->children[i];
            painter->rect(painter->context,
                          (sw_rect){ cx + 12.0,
                                     cy + child->frame.y + child->frame.height,
                                     cw - 24.0, 1.0 },
                          fade(sw_color_hex(0xe2e4e8), a), 0.0);
        }
    }
}

void sw_paint(const sw_node *root, const sw_environment *environment,
              const sw_painter *painter)
{
    if (!root || !painter)
        return;
    sw_env_frame frame = { environment, NULL };
    paint_node(root, &frame, painter, 0.0, 0.0, 1.0);
}

/* ------------------------------------------------------------------ */
/* Introspection                                                       */
/* ------------------------------------------------------------------ */

const char *sw_kind_name(sw_kind kind)
{
    static const char *names[] = {
        "Group", "Text", "Image", "Spacer", "Divider", "Rectangle",
        "RoundedRectangle", "Circle", "Capsule", "Path", "HStack",
        "VStack", "ZStack", "Button", "List", "ForEach", "TextField",
        "Window"
    };
    size_t count = sizeof(names) / sizeof(names[0]);
    size_t index = (size_t)kind;
    return index < count ? names[index] : "Unknown";
}

void sw_dump(const sw_node *node, int depth)
{
    if (!node)
        return;

    printf("%*s%-16s %7.1f x %7.1f  at (%7.1f, %7.1f)", depth * 2, "",
           sw_kind_name(node->kind), node->frame.width, node->frame.height,
           node->frame.x, node->frame.y);
    if (node->text && node->text[0] && node->kind != SW_WINDOW) {
        char preview[48];
        size_t i = 0;
        for (; node->text[i] && i + 1 < sizeof(preview); i++)
            preview[i] = node->text[i];
        preview[i] = 0;
        printf("  \"%s\"", preview);
    }
    if (node->kind == SW_TEXT && node->text_lines > 1)
        printf("  (%d lines)", node->text_lines);
    putchar('\n');

    for (size_t i = 0; i < node->count; i++)
        sw_dump(node->children[i], depth + 1);
}

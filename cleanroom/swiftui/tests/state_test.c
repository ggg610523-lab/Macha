#include "engine.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void check(int condition, const char *message)
{
    if (!condition) {
        printf("FAIL: %s\n", message);
        failures++;
    }
}

static int watch_count;
static char watch_key[32];
static int watch_value;

static void on_change(void *context, const char *key, sw_value value)
{
    (void)context;
    watch_count++;
    snprintf(watch_key, sizeof(watch_key), "%s", key ? key : "");
    watch_value = sw_value_as_int(value, -1);
}

static unsigned long checksum(const sw_canvas *canvas)
{
    const unsigned char *pixels = sw_canvas_pixels(canvas);
    size_t size = (size_t)sw_canvas_width(canvas) * sw_canvas_height(canvas) * 4;
    unsigned long hash = 1469598103934665603UL;
    for (size_t i = 0; i < size; i++) {
        hash ^= pixels[i];
        hash *= 1099511628211UL;
    }
    return hash;
}

static int has_red_text(const sw_canvas *canvas)
{
    const unsigned char *pixels = sw_canvas_pixels(canvas);
    size_t count = (size_t)sw_canvas_width(canvas) * sw_canvas_height(canvas);
    for (size_t i = 0; i < count; i++) {
        const unsigned char *p = pixels + i * 4;
        if (p[3] > 200 && p[0] > 120 && (int)p[0] - (int)p[1] > 40 &&
            (int)p[0] - (int)p[2] > 40)
            return 1;
    }
    return 0;
}

typedef struct {
    int rects;
    int texts;
} paint_stats;

static void painter_rect(void *context, sw_rect rect, sw_color color,
                         double radius)
{
    (void)rect;
    (void)color;
    (void)radius;
    ((paint_stats *)context)->rects++;
}

static void painter_text(void *context, const char *text, sw_rect rect,
                         sw_font font, sw_color color)
{
    (void)text;
    (void)rect;
    (void)font;
    (void)color;
    ((paint_stats *)context)->texts++;
}

int main(void)
{
    /* Values. */
    check(sw_value_as_int(sw_value_int(7), -1) == 7, "int value round trip");
    check(fabs(sw_value_as_double(sw_value_double(2.5), -1.0) - 2.5) < 0.001,
          "double value round trip");
    check(strcmp(sw_value_as_string(sw_value_string("hello"), ""), "hello") == 0,
          "string value round trip");
    check(sw_value_as_bool(sw_value_bool(1), 0) == 1, "bool value round trip");

    /* Environment store. */
    sw_environment *root_env = sw_environment_new();
    check(root_env != NULL, "environment allocated");
    sw_environment_set(root_env, SW_ENV_ACCENT_COLOR,
                       sw_value_color(sw_color_hex(0xcc3366)));
    check(sw_environment_has(root_env, SW_ENV_ACCENT_COLOR),
          "environment stores a key");
    check(sw_environment_value(root_env, SW_ENV_ACCENT_COLOR).kind ==
              SW_VALUE_COLOR,
          "environment returns the value");
    check(!sw_environment_has(root_env, "missing"), "missing key is absent");

    /* Inheritance through the node tree. */
    sw_node *root = sw_vstack(0.0);
    sw_environment_font(root, sw_font_system(20.0));
    sw_environment_color(root, SW_ENV_FOREGROUND_COLOR,
                         sw_color_hex(0xd02020));
    sw_node *plain = sw_text("Inherited text");
    sw_add(root, plain);

    sw_layout_environment(root, sw_proposal_size(400.0, NAN), root_env);
    check(fabs(plain->frame.height - 30.0) < 0.5,
          "environment font drives line height");
    check(fabs(plain->frame.width - 252.0) < 0.5,
          "environment font drives text width");

    sw_value inherited = sw_environment_resolve(root_env, plain, SW_ENV_FONT);
    check(inherited.kind == SW_VALUE_FONT &&
              fabs(inherited.as.font.size - 20.0) < 0.001,
          "font resolves through the node tree");

    sw_canvas *canvas = sw_canvas_new(400, 120);
    sw_canvas_clear(canvas, sw_rgb(1, 1, 1));
    sw_draw_environment(root, canvas, root_env);
    check(has_red_text(canvas), "inherited foreground colour reaches the text");
    sw_canvas_free(canvas);

    /* Dark colour scheme. */
    sw_node *dark_window = sw_window("Dark", 300.0, 200.0);
    sw_environment_dark(dark_window);
    sw_add(dark_window, sw_text("Night"));
    sw_layout_environment(dark_window, sw_proposal_size(300.0, 200.0),
                          root_env);
    sw_canvas *dark = sw_canvas_new(300, 200);
    sw_canvas_clear(dark, sw_rgb(1, 1, 1));
    sw_draw_environment(dark_window, dark, root_env);
    const unsigned char *sample =
        sw_canvas_pixels(dark) + ((size_t)150 * 300 + 150) * 4;
    check(sample[0] < 80 && sample[1] < 80 && sample[2] < 80,
          "dark colour scheme paints a dark window");
    sw_canvas_free(dark);

    /* Binding and state. */
    sw_state *count = sw_state_new(sw_value_int(1));
    sw_binding *binding = sw_state_binding(count);
    check(sw_value_as_int(sw_binding_get(binding), -1) == 1,
          "binding reads the state");
    unsigned long generation = sw_state_generation();
    sw_binding_set(binding, sw_value_int(2));
    check(sw_value_as_int(sw_state_value(count), -1) == 2,
          "binding writes the state");
    check(sw_state_revision(count) == 1, "state revision advances");
    check(sw_state_generation() > generation, "state generation advances");
    sw_binding_free(binding);

    sw_binding *constant = sw_binding_constant(sw_value_int(9));
    sw_binding_set(constant, sw_value_int(10));
    check(sw_value_as_int(sw_binding_get(constant), -1) == 9,
          "constant binding ignores writes");
    sw_binding_free(constant);

    sw_node *view = sw_vstack(0.0);
    sw_state *first = sw_node_state(view, "counter", sw_value_int(0));
    sw_state *second = sw_node_state(view, "counter", sw_value_int(99));
    check(first == second, "node state is stable for a key");
    sw_binding *node_binding = sw_state_binding(first);
    sw_binding_set(node_binding, sw_value_int(4));
    check(sw_value_as_int(
              sw_state_value(sw_node_state(view, "counter", sw_value_int(0))),
              -1) == 4,
          "node state binding updates the cell");
    sw_binding_free(node_binding);

    /* Observable. */
    sw_observable *document = sw_observable_new();
    sw_observable_watch(document, on_change, NULL);
    sw_observable_set(document, "wordCount", sw_value_int(12));
    check(watch_count == 1 && strcmp(watch_key, "wordCount") == 0 &&
              watch_value == 12,
          "observable notifies watchers");
    check(sw_value_as_int(sw_observable_get(document, "wordCount"), -1) == 12,
          "observable stores properties");
    sw_binding *observable_binding = sw_observable_binding(document, "wordCount");
    sw_binding_set(observable_binding, sw_value_int(13));
    check(sw_value_as_int(sw_observable_get(document, "wordCount"), -1) == 13,
          "observable binding writes");
    check(watch_count == 2, "observable binding notifies again");
    sw_binding_free(observable_binding);
    sw_observable_free(document);

    /* Host invalidation and re-render. */
    sw_node *window = sw_window("State", 320.0, 220.0);
    sw_node *stack = sw_vstack(6.0);
    sw_padding(stack, 12.0);
    sw_add(window, stack);
    sw_state *counter = sw_node_state(window, "count", sw_value_int(0));
    sw_node *label = sw_text("Count: 0");
    sw_add(stack, label);

    sw_host *host = sw_host_new(320.0, 220.0);
    sw_host_set_environment(host, root_env);
    sw_host_set_root(host, window);
    sw_host_render(host);
    check(!sw_host_needs_render(host), "fresh frame is up to date");
    unsigned long before = checksum(sw_host_canvas(host));

    sw_binding *counter_binding = sw_state_binding(counter);
    sw_binding_set(counter_binding, sw_value_int(1));
    check(sw_host_needs_render(host), "state write invalidates the host");

    char text[32];
    snprintf(text, sizeof(text), "Count: %d",
             sw_value_as_int(sw_binding_get(counter_binding), 0));
    sw_text_set(label, text);
    sw_host_render(host);
    check(!sw_host_needs_render(host), "re-render clears the invalidation");
    check(strcmp(label->text, "Count: 1") == 0, "label reflects the state");
    check(checksum(sw_host_canvas(host)) != before,
          "re-render changes the pixels");
    sw_binding_free(counter_binding);

    /* Hosting view wrapper. */
    sw_node *hosted_root = sw_window("Hosted", 200.0, 120.0);
    sw_hosting_view *hosted = sw_hosting_view_new(hosted_root, 200.0, 120.0);
    sw_hosting_view_draw(hosted);
    sw_size intrinsic = sw_hosting_view_intrinsic_size(hosted);
    check(intrinsic.width == 200.0 && intrinsic.height == 120.0,
          "hosting view reports intrinsic size");
    check(!sw_hosting_view_needs_display(hosted),
          "hosting view is clean after draw");
    sw_hosting_view_free(hosted);
    sw_node_free(hosted_root);

    /* ForEach lays its content out vertically. */
    sw_node *each = sw_foreach();
    sw_add(each, sw_styled_text("one", 14.0, 400));
    sw_add(each, sw_styled_text("two", 14.0, 400));
    sw_layout(each, sw_proposal_size(200.0, NAN));
    check(each->count == 2 && each->frame.height >= 40.0,
          "ForEach lays out its rows");
    sw_node_free(each);

    /* Painter bridge. */
    paint_stats stats = { 0, 0 };
    sw_painter painter = { &stats, painter_rect, painter_text };
    sw_node *painted = sw_window("Painted", 200.0, 120.0);
    sw_add(painted, sw_text("Hello"));
    sw_layout(painted, sw_proposal_size(200.0, 120.0));
    sw_paint(painted, NULL, &painter);
    check(stats.rects >= 2, "painter receives window rectangles");
    check(stats.texts >= 2, "painter receives title and body text");
    sw_node_free(painted);

    sw_host_free(host);
    sw_node_free(window);
    sw_node_free(view);
    sw_node_free(root);
    sw_node_free(dark_window);
    sw_state_free(count);
    sw_environment_free(root_env);

    if (failures) {
        printf("FAIL: %d check(s)\n", failures);
        return 1;
    }
    printf("PASS: environment inheritance, bindings, state, observables and painter bridge\n");
    return 0;
}

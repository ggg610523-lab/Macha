#include "engine.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int failures;

static void check(int condition, const char *message)
{
    if (!condition) {
        printf("FAIL: %s\n", message);
        failures++;
    }
}

static sw_node *code_line(const char *text)
{
    sw_node *node = sw_styled_text(text, 13.0, 400);
    sw_font_apply(node, sw_font_mono(13.0));
    sw_foreground(node, sw_color_hex(0x2c7d5a));
    return node;
}

static sw_node *list_row(const char *name, const char *detail)
{
    sw_node *row = sw_hstack(8.0);
    sw_align(row, SW_LEADING, SW_MIDDLE);
    sw_padding_edges(row, 8.0, 12.0, 8.0, 12.0);
    sw_add(row, sw_styled_text(name, 13.0, 400));
    sw_add(row, sw_spacer());
    sw_node *meta = sw_styled_text(detail, 11.0, 400);
    sw_foreground(meta, sw_color_hex(0x8a8f99));
    sw_add(row, meta);
    return row;
}

static sw_node *file_row(const char *name, const char *badge, int active,
                         sw_color dot)
{
    sw_node *row = sw_hstack(8.0);
    sw_align(row, SW_LEADING, SW_MIDDLE);
    sw_padding_edges(row, 6.0, 8.0, 6.0, 8.0);
    if (active)
        sw_background(row, sw_color_hex(0xdce8fb));

    sw_node *mark = sw_circle();
    sw_fill(mark, dot);
    sw_frame(mark, 8.0, 8.0);
    sw_add(row, mark);

    sw_add(row, sw_styled_text(name, 13.0, active ? 600 : 400));
    sw_add(row, sw_spacer());

    if (badge) {
        sw_node *tag = sw_styled_text(badge, 11.0, 400);
        sw_foreground(tag, sw_color_hex(0x8a8f99));
        sw_add(row, tag);
    }
    return row;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "render.png";

    sw_node *window = sw_window("CotEditor - Untitled.txt", 960.0, 640.0);
    sw_node *root = sw_vstack(0.0);
    sw_add(window, root);

    /* Toolbar. */
    sw_node *toolbar = sw_hstack(10.0);
    sw_align(toolbar, SW_LEADING, SW_MIDDLE);
    sw_padding_edges(toolbar, 12.0, 16.0, 12.0, 16.0);
    sw_background(toolbar, sw_color_hex(0xf5f6f8));

    sw_node *icon = sw_rounded_rectangle(6.0);
    sw_fill(icon, sw_color_hex(0x2f6fd0));
    sw_frame(icon, 26.0, 26.0);
    sw_add(toolbar, icon);
    sw_add(toolbar, sw_styled_text("Untitled.txt", 15.0, 600));
    sw_add(toolbar, sw_spacer());
    sw_add(toolbar, sw_button("Share"));
    sw_add(toolbar, sw_button("Run"));
    sw_add(root, toolbar);
    sw_add(root, sw_divider());

    /* Sidebar + editor. */
    sw_node *body = sw_hstack(0.0);
    sw_flex(body);
    sw_add(root, body);

    sw_node *sidebar = sw_vstack(4.0);
    sw_frame(sidebar, 230.0, NAN);
    sw_flex_height(sidebar);
    sw_padding(sidebar, 12.0);
    sw_background(sidebar, sw_color_hex(0xeff1f5));

    sw_node *section = sw_styled_text("FILES", 11.0, 600);
    sw_foreground(section, sw_color_hex(0x8a8f99));
    sw_add(sidebar, section);
    sw_add(sidebar, file_row("Untitled.txt", "edit", 1,
                             sw_color_hex(0x2f6fd0)));
    sw_add(sidebar, file_row("loader-notes.md", NULL, 0,
                             sw_color_hex(0x8a8f99)));
    sw_add(sidebar, file_row("window.c", NULL, 0,
                             sw_color_hex(0x8a8f99)));
    sw_add(sidebar, file_row("host_abi.h", NULL, 0,
                             sw_color_hex(0x8a8f99)));
    sw_add(sidebar, sw_spacer());

    sw_node *sidebar_footer = sw_styled_text("3 files - 1 open", 11.0, 400);
    sw_foreground(sidebar_footer, sw_color_hex(0x8a8f99));
    sw_add(sidebar, sidebar_footer);
    sw_add(body, sidebar);
    sw_add(body, sw_divider());

    sw_node *editor = sw_vstack(12.0);
    sw_flex(editor);
    sw_padding(editor, 24.0);
    sw_background(editor, sw_rgb(1.0, 1.0, 1.0));

    sw_node *heading = sw_styled_text("Notes on the loader", 26.0, 700);
    sw_add(editor, heading);

    sw_node *paragraph = sw_styled_text(
        "The Mach-O images bind against a host table that is filled at "
        "startup. Every slot is a thin translation layer over the Linux "
        "system calls, and the toolkit draws the window once the content "
        "view has been laid out.", 14.0, 400);
    sw_foreground(paragraph, sw_color_hex(0x3b3f46));
    sw_add(editor, paragraph);
    sw_add(editor, sw_divider());

    sw_node *code = sw_vstack(4.0);
    sw_flex_width(code);
    sw_padding(code, 12.0);
    sw_background(code, sw_color_hex(0xf6f7f9));
    sw_corner_radius(code, 8.0);
    sw_add(code, code_line("let host = HostWindow(width: 960, height: 640)"));
    sw_add(code, code_line("host.root = EditorView(document: doc)"));
    sw_add(code, code_line("host.present()"));
    sw_add(editor, code);

    sw_node *search = sw_text_field("Search notes", "");
    sw_text_field_focus(search, 1);
    sw_add(editor, search);

    sw_node *recent = sw_list();
    sw_add(recent, list_row("README.md", "2 KB"));
    sw_add(recent, list_row("window.c", "8 KB"));
    sw_add(editor, recent);

    sw_add(editor, sw_spacer());

    sw_node *footer = sw_styled_text("Line 1, Column 1 - UTF-8 - LF", 11.0,
                                     400);
    sw_foreground(footer, sw_color_hex(0x8a8f99));
    sw_add(editor, footer);
    sw_add(body, editor);

    sw_host *host = sw_host_new(960.0, 640.0);
    sw_host_set_root(host, window);
    sw_host_render(host);

    check(window->frame.width == 960.0 && window->frame.height == 640.0,
          "window fills the host");
    check(fabs(sidebar->frame.width - 230.0) < 0.5,
          "sidebar is 230 points wide");
    check(editor->frame.width > 600.0, "editor takes the remaining width");
    check(paragraph->text_lines >= 2, "paragraph wraps onto several lines");
    check(toolbar->frame.height >= 44.0, "toolbar keeps its padding");
    check(body->frame.height > 500.0, "body fills the window height");
    check(code->frame.width > 300.0, "code block spans the editor");
    check(search->frame.height >= 30.0,
          "text field keeps its control height");
    check(recent->frame.height > 40.0, "list receives leftover height");
    check(recent->count == 2, "list holds both rows");
    check(fabs(code->frame.width - editor->frame.width + 48.0) < 1.0,
          "code block matches the editor content width");

    sw_canvas *canvas = sw_host_canvas(host);
    check(canvas != NULL, "host produced a canvas");
    if (canvas) {
        unsigned ink = sw_canvas_ink_pixels(canvas, sw_rgb(1.0, 1.0, 1.0));
        unsigned colors = sw_canvas_distinct_colors(canvas);
        printf("render: %dx%d, %u ink pixels, %u distinct colours\n",
               sw_canvas_width(canvas), sw_canvas_height(canvas), ink, colors);
        check(ink > 20000, "window contains drawn content");
        check(colors > 12, "window uses more than a dozen colours");
        check(sw_canvas_write_png(canvas, path) == 1, "PNG written");
        printf("image: %s\n", path);
        if (isatty(1))
            sw_canvas_print_ansi(canvas, 96);
        else
            sw_canvas_print_ascii(canvas, 96);
    }

    printf("layout:\n");
    sw_dump(window, 0);

    sw_host_free(host);
    sw_node_free(window);

    if (failures) {
        printf("FAIL: %d check(s)\n", failures);
        return 1;
    }
    printf("PASS: clean-room SwiftUI engine laid out and rendered a window\n");
    return 0;
}

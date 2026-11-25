#define _GNU_SOURCE
#include "engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define SW_MAX_CLIP 64

struct sw_canvas {
    int width;
    int height;
    unsigned char *pixels;
    sw_rect clip[SW_MAX_CLIP];
    int depth;
};

sw_color sw_rgb(double r, double g, double b)
{
    sw_color color = { r, g, b, 1.0 };
    return color;
}

sw_color sw_rgba(double r, double g, double b, double a)
{
    sw_color color = { r, g, b, a };
    return color;
}

sw_color sw_color_hex(unsigned rgb)
{
    return sw_rgb(((rgb >> 16) & 0xff) / 255.0,
                  ((rgb >> 8) & 0xff) / 255.0,
                  (rgb & 0xff) / 255.0);
}

sw_color sw_color_clear(void)
{
    return sw_rgba(0.0, 0.0, 0.0, 0.0);
}

sw_proposal sw_proposal_unspecified(void)
{
    sw_proposal p = { NAN, NAN };
    return p;
}

sw_proposal sw_proposal_size(double width, double height)
{
    sw_proposal p = { width, height };
    return p;
}

int sw_proposal_has_width(sw_proposal p)
{
    return !isnan(p.width);
}

int sw_proposal_has_height(sw_proposal p)
{
    return !isnan(p.height);
}

sw_canvas *sw_canvas_new(int width, int height)
{
    if (width < 1)
        width = 1;
    if (height < 1)
        height = 1;

    sw_canvas *canvas = calloc(1, sizeof(*canvas));
    if (!canvas)
        return NULL;
    canvas->pixels = calloc((size_t)width * (size_t)height, 4);
    if (!canvas->pixels) {
        free(canvas);
        return NULL;
    }
    canvas->width = width;
    canvas->height = height;
    canvas->clip[0] = (sw_rect){ 0, 0, width, height };
    canvas->depth = 0;
    return canvas;
}

void sw_canvas_free(sw_canvas *canvas)
{
    if (!canvas)
        return;
    free(canvas->pixels);
    free(canvas);
}

int sw_canvas_width(const sw_canvas *canvas)
{
    return canvas ? canvas->width : 0;
}

int sw_canvas_height(const sw_canvas *canvas)
{
    return canvas ? canvas->height : 0;
}

const unsigned char *sw_canvas_pixels(const sw_canvas *canvas)
{
    return canvas ? canvas->pixels : NULL;
}

static double clamp01(double v)
{
    if (v < 0.0)
        return 0.0;
    if (v > 1.0)
        return 1.0;
    return v;
}

static unsigned char to_byte(double v)
{
    return (unsigned char)(clamp01(v) * 255.0 + 0.5);
}

static int in_clip(const sw_canvas *canvas, int x, int y)
{
    const sw_rect *r = &canvas->clip[canvas->depth];
    return x >= r->x && y >= r->y && x < r->x + r->width &&
           y < r->y + r->height;
}

static void blend_pixel(sw_canvas *canvas, int x, int y, sw_color color,
                        double coverage)
{
    if (x < 0 || y < 0 || x >= canvas->width || y >= canvas->height)
        return;
    if (!in_clip(canvas, x, y))
        return;

    double sa = clamp01(color.a * coverage);
    if (sa <= 0.0)
        return;

    unsigned char *p = canvas->pixels + ((size_t)y * canvas->width + x) * 4;
    double da = p[3] / 255.0;
    double dr = p[0] / 255.0;
    double dg = p[1] / 255.0;
    double db = p[2] / 255.0;

    double out_a = sa + da * (1.0 - sa);
    if (out_a <= 0.0) {
        p[0] = p[1] = p[2] = p[3] = 0;
        return;
    }

    double r = (color.r * sa + dr * da * (1.0 - sa)) / out_a;
    double g = (color.g * sa + dg * da * (1.0 - sa)) / out_a;
    double b = (color.b * sa + db * da * (1.0 - sa)) / out_a;

    p[0] = to_byte(r);
    p[1] = to_byte(g);
    p[2] = to_byte(b);
    p[3] = to_byte(out_a);
}

void sw_canvas_clear(sw_canvas *canvas, sw_color color)
{
    if (!canvas)
        return;
    for (size_t i = 0; i < (size_t)canvas->width * canvas->height; i++) {
        unsigned char *p = canvas->pixels + i * 4;
        p[0] = to_byte(color.r);
        p[1] = to_byte(color.g);
        p[2] = to_byte(color.b);
        p[3] = to_byte(color.a);
    }
}

static int round_rect_contains(double px, double py, double x, double y,
                               double width, double height, double radius)
{
    if (px < x || py < y || px > x + width || py > y + height)
        return 0;
    if (radius <= 0.0)
        return 1;

    if (radius > width / 2.0)
        radius = width / 2.0;
    if (radius > height / 2.0)
        radius = height / 2.0;

    double cx = px;
    double cy = py;
    int corner = 0;

    if (px < x + radius) {
        cx = x + radius;
        corner = 1;
    } else if (px > x + width - radius) {
        cx = x + width - radius;
        corner = 1;
    }
    if (py < y + radius) {
        cy = y + radius;
        corner = 1;
    } else if (py > y + height - radius) {
        cy = y + height - radius;
        corner = 1;
    }
    if (!corner)
        return 1;

    double dx = px - cx;
    double dy = py - cy;
    return dx * dx + dy * dy <= radius * radius;
}

void sw_canvas_fill_rect(sw_canvas *canvas, double x, double y, double width,
                         double height, sw_color color)
{
    if (!canvas || width <= 0.0 || height <= 0.0 || color.a <= 0.0)
        return;

    const sw_rect *clip = &canvas->clip[canvas->depth];
    double x0 = fmax(x, clip->x);
    double y0 = fmax(y, clip->y);
    double x1 = fmin(x + width, clip->x + clip->width);
    double y1 = fmin(y + height, clip->y + clip->height);
    if (x1 <= x0 || y1 <= y0)
        return;

    int ix0 = (int)floor(x0);
    int iy0 = (int)floor(y0);
    int ix1 = (int)ceil(x1);
    int iy1 = (int)ceil(y1);

    for (int py = iy0; py < iy1; py++) {
        for (int px = ix0; px < ix1; px++) {
            /* coverage of the pixel square inside the rectangle */
            double lx = fmax(x, px);
            double rx = fmin(x + width, px + 1);
            double ty = fmax(y, py);
            double by = fmin(y + height, py + 1);
            if (rx <= lx || by <= ty)
                continue;
            blend_pixel(canvas, px, py, color, (rx - lx) * (by - ty));
        }
    }
}

void sw_canvas_fill_round_rect(sw_canvas *canvas, double x, double y,
                               double width, double height, double radius,
                               sw_color color)
{
    if (!canvas || width <= 0.0 || height <= 0.0 || color.a <= 0.0)
        return;
    if (radius <= 0.0) {
        sw_canvas_fill_rect(canvas, x, y, width, height, color);
        return;
    }

    const sw_rect *clip = &canvas->clip[canvas->depth];
    int ix0 = (int)floor(fmax(x, clip->x));
    int iy0 = (int)floor(fmax(y, clip->y));
    int ix1 = (int)ceil(fmin(x + width, clip->x + clip->width));
    int iy1 = (int)ceil(fmin(y + height, clip->y + clip->height));

    for (int py = iy0; py < iy1; py++) {
        for (int px = ix0; px < ix1; px++) {
            int hits = 0;
            for (int sy = 0; sy < 2; sy++) {
                for (int sx = 0; sx < 2; sx++) {
                    double fx = px + (sx + 0.5) * 0.5;
                    double fy = py + (sy + 0.5) * 0.5;
                    if (round_rect_contains(fx, fy, x, y, width, height,
                                            radius))
                        hits++;
                }
            }
            if (hits)
                blend_pixel(canvas, px, py, color, hits / 4.0);
        }
    }
}

void sw_canvas_fill_circle(sw_canvas *canvas, double cx, double cy,
                           double radius, sw_color color)
{
    if (!canvas || radius <= 0.0 || color.a <= 0.0)
        return;

    const sw_rect *clip = &canvas->clip[canvas->depth];
    int ix0 = (int)floor(fmax(cx - radius, clip->x));
    int iy0 = (int)floor(fmax(cy - radius, clip->y));
    int ix1 = (int)ceil(fmin(cx + radius, clip->x + clip->width));
    int iy1 = (int)ceil(fmin(cy + radius, clip->y + clip->height));

    for (int py = iy0; py < iy1; py++) {
        for (int px = ix0; px < ix1; px++) {
            int hits = 0;
            for (int sy = 0; sy < 2; sy++) {
                for (int sx = 0; sx < 2; sx++) {
                    double dx = px + (sx + 0.5) * 0.5 - cx;
                    double dy = py + (sy + 0.5) * 0.5 - cy;
                    if (dx * dx + dy * dy <= radius * radius)
                        hits++;
                }
            }
            if (hits)
                blend_pixel(canvas, px, py, color, hits / 4.0);
        }
    }
}

void sw_canvas_stroke_round_rect(sw_canvas *canvas, double x, double y,
                                 double width, double height, double radius,
                                 double line_width, sw_color color)
{
    if (!canvas || width <= 0.0 || height <= 0.0 || line_width <= 0.0 ||
        color.a <= 0.0)
        return;

    const sw_rect *clip = &canvas->clip[canvas->depth];
    int ix0 = (int)floor(fmax(x - line_width, clip->x));
    int iy0 = (int)floor(fmax(y - line_width, clip->y));
    int ix1 = (int)ceil(fmin(x + width + line_width,
                             clip->x + clip->width));
    int iy1 = (int)ceil(fmin(y + height + line_width,
                             clip->y + clip->height));

    for (int py = iy0; py < iy1; py++) {
        for (int px = ix0; px < ix1; px++) {
            int hits = 0;
            for (int sy = 0; sy < 2; sy++) {
                for (int sx = 0; sx < 2; sx++) {
                    double fx = px + (sx + 0.5) * 0.5;
                    double fy = py + (sy + 0.5) * 0.5;
                    int outer = round_rect_contains(fx, fy, x, y, width,
                                                    height, radius);
                    int inner = round_rect_contains(
                        fx, fy, x + line_width, y + line_width,
                        width - 2 * line_width, height - 2 * line_width,
                        radius > line_width ? radius - line_width : 0.0);
                    if (outer && !inner)
                        hits++;
                }
            }
            if (hits)
                blend_pixel(canvas, px, py, color, hits / 4.0);
        }
    }
}

void sw_canvas_push_clip(sw_canvas *canvas, double x, double y, double width,
                         double height)
{
    if (!canvas || canvas->depth + 1 >= SW_MAX_CLIP)
        return;

    const sw_rect *parent = &canvas->clip[canvas->depth];
    double x0 = fmax(x, parent->x);
    double y0 = fmax(y, parent->y);
    double x1 = fmin(x + width, parent->x + parent->width);
    double y1 = fmin(y + height, parent->y + parent->height);
    sw_rect r = { x0, y0, fmax(0.0, x1 - x0), fmax(0.0, y1 - y0) };
    canvas->clip[++canvas->depth] = r;
}

void sw_canvas_pop_clip(sw_canvas *canvas)
{
    if (canvas && canvas->depth > 0)
        canvas->depth--;
}

unsigned sw_canvas_distinct_colors(const sw_canvas *canvas)
{
    if (!canvas)
        return 0;

    enum { TABLE = 1 << 16 };
    unsigned *table = calloc(TABLE, sizeof(*table));
    if (!table)
        return 0;

    unsigned count = 0;
    for (size_t i = 0; i < (size_t)canvas->width * canvas->height; i++) {
        const unsigned char *p = canvas->pixels + i * 4;
        unsigned key = ((unsigned)p[0] << 16) | ((unsigned)p[1] << 8) |
                       p[2] | ((unsigned)p[3] << 24);
        unsigned slot = (key * 2654435761u) & (TABLE - 1);
        for (unsigned probe = 0; probe < 8; probe++) {
            unsigned at = (slot + probe) & (TABLE - 1);
            if (table[at] == key)
                break;
            if (table[at] == 0) {
                table[at] = key;
                count++;
                break;
            }
        }
    }
    free(table);
    return count;
}

unsigned sw_canvas_ink_pixels(const sw_canvas *canvas, sw_color background)
{
    if (!canvas)
        return 0;

    unsigned char br = to_byte(background.r);
    unsigned char bg = to_byte(background.g);
    unsigned char bb = to_byte(background.b);
    unsigned count = 0;

    for (size_t i = 0; i < (size_t)canvas->width * canvas->height; i++) {
        const unsigned char *p = canvas->pixels + i * 4;
        int dr = (int)p[0] - br;
        int dg = (int)p[1] - bg;
        int db = (int)p[2] - bb;
        if (dr * dr + dg * dg + db * db > 64)
            count++;
    }
    return count;
}

static void put_u32(unsigned char *out, unsigned value)
{
    out[0] = (unsigned char)(value >> 24);
    out[1] = (unsigned char)(value >> 16);
    out[2] = (unsigned char)(value >> 8);
    out[3] = (unsigned char)value;
}

static void write_chunk(FILE *file, const char *type, const unsigned char *data,
                        unsigned length)
{
    unsigned char header[8];
    put_u32(header, length);
    memcpy(header + 4, type, 4);
    fwrite(header, 1, 8, file);
    if (length)
        fwrite(data, 1, length, file);

    unsigned crc = crc32(0L, (const Bytef *)type, 4);
    if (length)
        crc = crc32(crc, data, length);
    unsigned char crc_bytes[4];
    put_u32(crc_bytes, crc);
    fwrite(crc_bytes, 1, 4, file);
}

void sw_canvas_print_ansi(const sw_canvas *canvas, int columns)
{
    if (!canvas || columns < 8)
        return;

    int cell = canvas->width / columns;
    if (cell < 1)
        cell = 1;
    int rows = canvas->height / (cell * 2);
    if (rows < 1)
        rows = 1;

    for (int row = 0; row < rows; row++) {
        for (int column = 0; column < columns; column++) {
            int x0 = column * cell;
            int y0 = row * cell * 2;
            unsigned long r = 0, g = 0, b = 0;
            int samples = 0;
            for (int y = y0; y < y0 + cell * 2 && y < canvas->height; y++) {
                for (int x = x0; x < x0 + cell && x < canvas->width; x++) {
                    const unsigned char *p =
                        canvas->pixels + ((size_t)y * canvas->width + x) * 4;
                    r += p[0];
                    g += p[1];
                    b += p[2];
                    samples++;
                }
            }
            if (!samples)
                continue;
            printf("\x1b[48;2;%lu;%lu;%lum  ", r / samples, g / samples,
                   b / samples);
        }
        printf("\x1b[0m\n");
    }
    printf("\x1b[0m");
}

void sw_canvas_print_ascii(const sw_canvas *canvas, int columns)
{
    static const char ramp[] = " .:-=+*#%@";
    if (!canvas || columns < 8)
        return;

    int cell = canvas->width / columns;
    if (cell < 1)
        cell = 1;
    int rows = canvas->height / (cell * 2);
    if (rows < 1)
        rows = 1;

    for (int row = 0; row < rows; row++) {
        for (int column = 0; column < columns; column++) {
            int x0 = column * cell;
            int y0 = row * cell * 2;
            double luminance = 0.0;
            int samples = 0;
            for (int y = y0; y < y0 + cell * 2 && y < canvas->height; y++) {
                for (int x = x0; x < x0 + cell && x < canvas->width; x++) {
                    const unsigned char *p =
                        canvas->pixels + ((size_t)y * canvas->width + x) * 4;
                    luminance += 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
                    samples++;
                }
            }
            if (!samples)
                continue;
            double darkness = 1.0 - luminance / samples / 255.0;
            int index = (int)(darkness * (sizeof(ramp) - 2) + 0.5);
            if (index < 0)
                index = 0;
            if (index > (int)sizeof(ramp) - 2)
                index = (int)sizeof(ramp) - 2;
            putchar(ramp[index]);
        }
        putchar('\n');
    }
}

int sw_canvas_write_png(const sw_canvas *canvas, const char *path)
{
    if (!canvas || !path)
        return 0;

    size_t stride = (size_t)canvas->width * 4 + 1;
    size_t raw_size = stride * (size_t)canvas->height;
    unsigned char *raw = malloc(raw_size);
    if (!raw)
        return 0;

    for (int y = 0; y < canvas->height; y++) {
        unsigned char *row = raw + stride * (size_t)y;
        row[0] = 0;
        memcpy(row + 1, canvas->pixels + (size_t)y * canvas->width * 4,
               (size_t)canvas->width * 4);
    }

    uLongf compressed_size = compressBound(raw_size);
    unsigned char *compressed = malloc(compressed_size);
    if (!compressed) {
        free(raw);
        return 0;
    }
    if (compress2(compressed, &compressed_size, raw, raw_size,
                  Z_DEFAULT_COMPRESSION) != Z_OK) {
        free(raw);
        free(compressed);
        return 0;
    }
    free(raw);

    FILE *file = fopen(path, "wb");
    if (!file) {
        free(compressed);
        return 0;
    }

    static const unsigned char signature[8] = {
        0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a
    };
    fwrite(signature, 1, 8, file);

    unsigned char ihdr[13];
    put_u32(ihdr, (unsigned)canvas->width);
    put_u32(ihdr + 4, (unsigned)canvas->height);
    ihdr[8] = 8;    /* bit depth */
    ihdr[9] = 6;    /* RGBA */
    ihdr[10] = 0;   /* deflate */
    ihdr[11] = 0;   /* adaptive filtering */
    ihdr[12] = 0;   /* no interlace */
    write_chunk(file, "IHDR", ihdr, 13);
    write_chunk(file, "IDAT", compressed, (unsigned)compressed_size);
    write_chunk(file, "IEND", NULL, 0);

    int ok = ferror(file) == 0;
    fclose(file);
    free(compressed);
    return ok;
}

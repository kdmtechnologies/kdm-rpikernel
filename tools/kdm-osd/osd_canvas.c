/*
 * osd_canvas.c - Implementation of OsdCanvas & OsdFont OOP Classes
 */
#include "osd_canvas.h"

#include <math.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static inline uint32_t pack_argb(uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

osd_canvas_t *osd_canvas_create(int width, int height) {
    osd_canvas_t *self = calloc(1, sizeof(osd_canvas_t));
    if (!self)
        return NULL;
    self->width  = width;
    self->height = height;
    self->pixels = calloc((size_t)width * height, sizeof(uint32_t));
    if (!self->pixels) {
        free(self);
        return NULL;
    }
    return self;
}

void osd_canvas_destroy(osd_canvas_t *self) {
    if (!self)
        return;
    free(self->pixels);
    free(self);
}

void osd_canvas_clear(osd_canvas_t *self) {
    if (!self || !self->pixels)
        return;
    memset(self->pixels, 0, (size_t)self->width * self->height * sizeof(uint32_t));
}

void osd_canvas_fill_rect_raw(osd_canvas_t *self, int x, int y, int w, int h, uint32_t argb) {
    if (!self || !self->pixels || w <= 0 || h <= 0)
        return;
    int x0 = clampi(x, 0, self->width);
    int y0 = clampi(y, 0, self->height);
    int x1 = clampi(x + w, 0, self->width);
    int y1 = clampi(y + h, 0, self->height);
    for (int py = y0; py < y1; py++) {
        uint32_t *row = &self->pixels[py * self->width];
        for (int px = x0; px < x1; px++) {
            row[px] = argb;
        }
    }
}

void osd_canvas_blend_pixel(osd_canvas_t *self, int x, int y,
                            uint8_t r, uint8_t g, uint8_t b, float alpha) {
    if ((unsigned)x >= (unsigned)self->width || (unsigned)y >= (unsigned)self->height || alpha <= 0.0f)
        return;
    if (alpha >= 0.998f) {
        self->pixels[y * self->width + x] = pack_argb(255, r, g, b);
        return;
    }

    uint32_t dst = self->pixels[y * self->width + x];
    if (dst == 0) {
        uint8_t a8 = (uint8_t)(alpha * 255.0f + 0.5f);
        self->pixels[y * self->width + x] = a8 ? pack_argb(a8, r, g, b) : 0;
        return;
    }

    float da    = ((dst >> 24) & 0xFF) * (1.0f / 255.0f);
    float inv_a = 1.0f - alpha;
    float out_a = alpha + da * inv_a;
    if (out_a <= 1e-5f) {
        self->pixels[y * self->width + x] = 0;
        return;
    }

    float w_dst = (da * inv_a) / out_a;
    float w_src = alpha / out_a;
    float out_r = r * w_src + ((dst >> 16) & 0xFF) * w_dst;
    float out_g = g * w_src + ((dst >> 8)  & 0xFF) * w_dst;
    float out_b = b * w_src + ( dst        & 0xFF) * w_dst;

    self->pixels[y * self->width + x] = pack_argb(
        (uint8_t)clampi((int)(out_a * 255.0f + 0.5f), 0, 255),
        (uint8_t)clampi((int)(out_r + 0.5f), 0, 255),
        (uint8_t)clampi((int)(out_g + 0.5f), 0, 255),
        (uint8_t)clampi((int)(out_b + 0.5f), 0, 255)
    );
}

static inline float sd_rounded_box(float px, float py, float cx, float cy,
                                   float half_w, float half_h, float r) {
    float qx = fabsf(px - cx) - half_w + r;
    float qy = fabsf(py - cy) - half_h + r;
    if (qx <= 0.0f)
        return fmaxf(qx, qy) - r;
    if (qy <= 0.0f)
        return qx - r;
    return hypotf(qx, qy) - r;
}

void osd_canvas_fill_rrect(osd_canvas_t *self, float x, float y, float w, float h, float rad,
                           uint8_t r, uint8_t g, uint8_t b, float alpha) {
    if (w <= 0.0f || h <= 0.0f)
        return;
    float cx = x + w * 0.5f;
    float cy = y + h * 0.5f;
    float hw = w * 0.5f;
    float hh = h * 0.5f;
    rad = clampf(rad, 0.0f, fminf(hw, hh));

    int x0 = clampi((int)floorf(x - 2.0f), 0, self->width - 1);
    int y0 = clampi((int)floorf(y - 2.0f), 0, self->height - 1);
    int x1 = clampi((int)ceilf(x + w + 2.0f), 0, self->width - 1);
    int y1 = clampi((int)ceilf(y + h + 2.0f), 0, self->height - 1);

    int ix0 = (int)ceilf(x + rad + 1.0f);
    int ix1 = (int)floorf(x + w - rad - 1.0f);
    int iy0 = (int)ceilf(y + rad + 1.0f);
    int iy1 = (int)floorf(y + h - rad - 1.0f);

    for (int py = y0; py <= y1; py++) {
        bool in_y = (py >= iy0 && py <= iy1);
        for (int px = x0; px <= x1; px++) {
            if (in_y && px >= ix0 && px <= ix1) {
                osd_canvas_blend_pixel(self, px, py, r, g, b, alpha);
                continue;
            }
            float d = sd_rounded_box(px + 0.5f, py + 0.5f, cx, cy, hw, hh, rad);
            float cov = clampf(0.5f - d, 0.0f, 1.0f);
            if (cov > 0.0f)
                osd_canvas_blend_pixel(self, px, py, r, g, b, alpha * cov);
        }
    }
}

void osd_canvas_stroke_rrect(osd_canvas_t *self, float x, float y, float w, float h, float rad,
                             float stroke_w, uint8_t r, uint8_t g, uint8_t b, float alpha) {
    float cx = x + w * 0.5f;
    float cy = y + h * 0.5f;
    float hw = w * 0.5f;
    float hh = h * 0.5f;
    rad = clampf(rad, 0.0f, fminf(hw, hh));
    float half_s = stroke_w * 0.5f;

    int x0 = clampi((int)floorf(x - stroke_w - 2.0f), 0, self->width - 1);
    int y0 = clampi((int)floorf(y - stroke_w - 2.0f), 0, self->height - 1);
    int x1 = clampi((int)ceilf(x + w + stroke_w + 2.0f), 0, self->width - 1);
    int y1 = clampi((int)ceilf(y + h + stroke_w + 2.0f), 0, self->height - 1);

    int inner_x0 = (int)ceilf(x + rad + stroke_w + 2.0f);
    int inner_x1 = (int)floorf(x + w - rad - stroke_w - 2.0f);
    int inner_y0 = (int)ceilf(y + rad + stroke_w + 2.0f);
    int inner_y1 = (int)floorf(y + h - rad - stroke_w - 2.0f);

    for (int py = y0; py <= y1; py++) {
        bool mid_y = (py >= inner_y0 && py <= inner_y1);
        for (int px = x0; px <= x1; px++) {
            if (mid_y && px == inner_x0 && inner_x1 >= inner_x0) {
                px = inner_x1;
                continue;
            }
            float d = fabsf(sd_rounded_box(px + 0.5f, py + 0.5f, cx, cy, hw, hh, rad)) - half_s;
            float cov = clampf(0.5f - d, 0.0f, 1.0f);
            if (cov > 0.0f)
                osd_canvas_blend_pixel(self, px, py, r, g, b, alpha * cov);
        }
    }
}

void osd_canvas_fill_circle(osd_canvas_t *self, float cx, float cy, float radius,
                            uint8_t r, uint8_t g, uint8_t b, float alpha) {
    int x0 = (int)floorf(cx - radius - 2.0f);
    int y0 = (int)floorf(cy - radius - 2.0f);
    int x1 = (int)ceilf(cx + radius + 2.0f);
    int y1 = (int)ceilf(cy + radius + 2.0f);
    for (int py = y0; py <= y1; py++) {
        for (int px = x0; px <= x1; px++) {
            float d = hypotf(px + 0.5f - cx, py + 0.5f - cy) - radius;
            float cov = clampf(0.5f - d, 0.0f, 1.0f);
            if (cov > 0.0f)
                osd_canvas_blend_pixel(self, px, py, r, g, b, alpha * cov);
        }
    }
}

void osd_canvas_stroke_circle(osd_canvas_t *self, float cx, float cy, float radius, float stroke_w,
                              uint8_t r, uint8_t g, uint8_t b, float alpha) {
    float half_s = stroke_w * 0.5f;
    int x0 = (int)floorf(cx - radius - half_s - 2.0f);
    int y0 = (int)floorf(cy - radius - half_s - 2.0f);
    int x1 = (int)ceilf(cx + radius + half_s + 2.0f);
    int y1 = (int)ceilf(cy + radius + half_s + 2.0f);
    for (int py = y0; py <= y1; py++) {
        for (int px = x0; px <= x1; px++) {
            float d = fabsf(hypotf(px + 0.5f - cx, py + 0.5f - cy) - radius) - half_s;
            float cov = clampf(0.5f - d, 0.0f, 1.0f);
            if (cov > 0.0f)
                osd_canvas_blend_pixel(self, px, py, r, g, b, alpha * cov);
        }
    }
}

void osd_canvas_draw_segment(osd_canvas_t *self, float ax, float ay, float bx, float by,
                             float width, bool round_cap,
                             uint8_t r, uint8_t g, uint8_t b, float alpha) {
    float half_w = width * 0.5f;
    int x0 = (int)floorf(fminf(ax, bx) - half_w - 2.0f);
    int y0 = (int)floorf(fminf(ay, by) - half_w - 2.0f);
    int x1 = (int)ceilf(fmaxf(ax, bx) + half_w + 2.0f);
    int y1 = (int)ceilf(fmaxf(ay, by) + half_w + 2.0f);

    float vx = bx - ax;
    float vy = by - ay;
    float len2 = vx * vx + vy * vy;
    float len  = sqrtf(len2);

    for (int py = y0; py <= y1; py++) {
        for (int px = x0; px <= x1; px++) {
            float pxf = px + 0.5f;
            float pyf = py + 0.5f;
            float d;
            if (len < 1e-4f) {
                d = hypotf(pxf - ax, pyf - ay) - half_w;
            } else {
                float t = ((pxf - ax) * vx + (pyf - ay) * vy) / len2;
                if (round_cap) {
                    t = clampf(t, 0.0f, 1.0f);
                    d = hypotf(pxf - (ax + t * vx), pyf - (ay + t * vy)) - half_w;
                } else {
                    float tc = clampf(t, 0.0f, 1.0f);
                    float perp = hypotf(pxf - (ax + tc * vx), pyf - (ay + tc * vy)) - half_w;
                    float para = (fabsf(t - 0.5f) - 0.5f) * len;
                    d = fmaxf(perp, para);
                }
            }
            float cov = clampf(0.5f - d, 0.0f, 1.0f);
            if (cov > 0.0f)
                osd_canvas_blend_pixel(self, px, py, r, g, b, alpha * cov);
        }
    }
}

void osd_canvas_draw_arc(osd_canvas_t *self, float cx, float cy, float radius,
                         float a0_rad, float a1_rad, float width,
                         uint8_t r, uint8_t g, uint8_t b, float alpha) {
    const int steps = 20;
    for (int k = 0; k < steps; k++) {
        float t0 = a0_rad + (a1_rad - a0_rad) * ((float)k / (float)steps);
        float t1 = a0_rad + (a1_rad - a0_rad) * ((float)(k + 1) / (float)steps);
        osd_canvas_draw_segment(self,
                                cx + cosf(t0) * radius, cy + sinf(t0) * radius,
                                cx + cosf(t1) * radius, cy + sinf(t1) * radius,
                                width, true, r, g, b, alpha);
    }
}

bool osd_canvas_save_png(const osd_canvas_t *self, const char *path, int crop_h) {
    int out_h = (crop_h > 0 && crop_h <= self->height) ? crop_h : self->height;
    FILE *fp = fopen(path, "wb");
    if (!fp)
        return false;
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) {
        fclose(fp);
        return false;
    }
    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_write_struct(&png, NULL);
        fclose(fp);
        return false;
    }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        fclose(fp);
        return false;
    }
    png_init_io(png, fp);
    png_set_IHDR(png, info, self->width, out_h, 8, PNG_COLOR_TYPE_RGBA,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    uint8_t *row = malloc((size_t)self->width * 4);
    for (int y = 0; y < out_h; y++) {
        for (int x = 0; x < self->width; x++) {
            uint32_t p = self->pixels[y * self->width + x];
            row[x * 4 + 0] = (p >> 16) & 0xFF;
            row[x * 4 + 1] = (p >> 8)  & 0xFF;
            row[x * 4 + 2] =  p        & 0xFF;
            row[x * 4 + 3] = (p >> 24) & 0xFF;
        }
        png_write_row(png, row);
    }
    free(row);
    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &info);
    fclose(fp);
    return true;
}

/* --- OsdFont Implementation --- */

static const char *const k_font_paths[] = {
    "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/freefont/FreeSansBold.ttf",
    "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
    "/System/Library/Fonts/Helvetica.ttc",
    NULL
};

static bool load_face_at_size(FT_Library lib, FT_Face *out_face, int px) {
    for (int i = 0; k_font_paths[i] != NULL; i++) {
        if (access(k_font_paths[i], R_OK) == 0) {
            if (FT_New_Face(lib, k_font_paths[i], 0, out_face) == 0) {
                FT_Set_Pixel_Sizes(*out_face, 0, (FT_UInt)px);
                return true;
            }
        }
    }
    return false;
}

bool osd_font_init(osd_font_t *self, float scale) {
    memset(self, 0, sizeof(*self));
    if (FT_Init_FreeType(&self->lib) != 0)
        return false;
    int bar_px   = (int)roundf(24.0f * scale);
    int date_px  = (int)roundf(30.0f * scale);
    int label_px = (int)roundf(19.0f * scale);
    if (!load_face_at_size(self->lib, &self->face_bar, bar_px))
        return false;
    if (!load_face_at_size(self->lib, &self->face_date, date_px))
        return false;
    if (!load_face_at_size(self->lib, &self->face_label, label_px))
        return false;
    return true;
}

void osd_font_destroy(osd_font_t *self) {
    if (!self)
        return;
    if (self->face_label)
        FT_Done_Face(self->face_label);
    if (self->face_date)
        FT_Done_Face(self->face_date);
    if (self->face_bar)
        FT_Done_Face(self->face_bar);
    if (self->lib)
        FT_Done_FreeType(self->lib);
}

int osd_font_measure(FT_Face face, const char *text, int *out_ascender, int *out_height) {
    int pen_x = 0;
    int max_asc = 0;
    int max_desc = 0;
    for (const char *p = text; *p; p++) {
        if (FT_Load_Char(face, (FT_ULong)*p, FT_LOAD_RENDER) != 0)
            continue;
        FT_GlyphSlot g = face->glyph;
        if (g->bitmap_top > max_asc)
            max_asc = g->bitmap_top;
        int desc = (int)g->bitmap.rows - g->bitmap_top;
        if (desc > max_desc)
            max_desc = desc;
        pen_x += (int)(g->advance.x >> 6);
    }
    if (out_ascender)
        *out_ascender = max_asc;
    if (out_height)
        *out_height = max_asc + max_desc;
    return pen_x;
}

void osd_font_draw_text(osd_canvas_t *canvas, FT_Face face, const char *text,
                        int x, int baseline_y, uint8_t r, uint8_t g, uint8_t b, float alpha) {
    int pen_x = x;
    for (const char *p = text; *p; p++) {
        if (FT_Load_Char(face, (FT_ULong)*p, FT_LOAD_RENDER) != 0)
            continue;
        FT_GlyphSlot slot = face->glyph;
        FT_Bitmap *bmp = &slot->bitmap;
        int gx = pen_x + slot->bitmap_left;
        int gy = baseline_y - slot->bitmap_top;

        for (unsigned int row = 0; row < bmp->rows; row++) {
            for (unsigned int col = 0; col < bmp->width; col++) {
                uint8_t cov = bmp->buffer[row * bmp->pitch + col];
                if (cov > 0)
                    osd_canvas_blend_pixel(canvas, gx + (int)col, gy + (int)row,
                                           r, g, b, alpha * (cov / 255.0f));
            }
        }
        pen_x += (int)(slot->advance.x >> 6);
    }
}

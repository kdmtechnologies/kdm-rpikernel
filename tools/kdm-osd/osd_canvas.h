/*
 * osd_canvas.h - OOP 2D ARGB8888 Surface, Anti-Aliased SDF Primitives & Font Class
 */
#ifndef KDM_OSD_CANVAS_H
#define KDM_OSD_CANVAS_H

#include "osd_types.h"
#include <ft2build.h>
#include FT_FREETYPE_H

typedef struct {
    int width;
    int height;
    uint32_t *pixels; /* ARGB8888 (0xAARRGGBB) */
} osd_canvas_t;

typedef struct {
    FT_Library lib;
    FT_Face face_bar;   /* Bold 24px font for top status bar & pill labels */
    FT_Face face_date;  /* Bold 30px font for Quick Settings date header */
    FT_Face face_label; /* Bold 19px font for Quick Settings circular button labels */
} osd_font_t;

/* --- OsdCanvas Lifecycle & Export --- */
osd_canvas_t *osd_canvas_create(int width, int height);
void osd_canvas_destroy(osd_canvas_t *self);
void osd_canvas_clear(osd_canvas_t *self);
void osd_canvas_fill_rect_raw(osd_canvas_t *self, int x, int y, int w, int h, uint32_t argb);
bool osd_canvas_save_png(const osd_canvas_t *self, const char *path, int crop_h);

/* --- OsdCanvas Anti-Aliased SDF Drawing Methods --- */
void osd_canvas_blend_pixel(osd_canvas_t *self, int x, int y,
                            uint8_t r, uint8_t g, uint8_t b, float alpha);
void osd_canvas_fill_rrect(osd_canvas_t *self, float x, float y, float w, float h, float rad,
                           uint8_t r, uint8_t g, uint8_t b, float alpha);
void osd_canvas_stroke_rrect(osd_canvas_t *self, float x, float y, float w, float h, float rad,
                             float stroke_w, uint8_t r, uint8_t g, uint8_t b, float alpha);
void osd_canvas_fill_circle(osd_canvas_t *self, float cx, float cy, float radius,
                            uint8_t r, uint8_t g, uint8_t b, float alpha);
void osd_canvas_stroke_circle(osd_canvas_t *self, float cx, float cy, float radius, float stroke_w,
                              uint8_t r, uint8_t g, uint8_t b, float alpha);
void osd_canvas_draw_segment(osd_canvas_t *self, float ax, float ay, float bx, float by,
                             float width, bool round_cap,
                             uint8_t r, uint8_t g, uint8_t b, float alpha);
void osd_canvas_draw_arc(osd_canvas_t *self, float cx, float cy, float radius,
                         float a0_rad, float a1_rad, float width,
                         uint8_t r, uint8_t g, uint8_t b, float alpha);

/* --- OsdFont Class Methods --- */
bool osd_font_init(osd_font_t *self, float scale);
void osd_font_destroy(osd_font_t *self);
int  osd_font_measure(FT_Face face, const char *text, int *out_ascender, int *out_height);
void osd_font_draw_text(osd_canvas_t *canvas, FT_Face face, const char *text,
                        int x, int baseline_y, uint8_t r, uint8_t g, uint8_t b, float alpha);

#endif /* KDM_OSD_CANVAS_H */

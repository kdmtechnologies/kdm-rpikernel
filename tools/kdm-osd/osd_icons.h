/*
 * osd_icons.h - OOP Vector Icon Renderer Class (Status Bar + Quick Settings Shade)
 */
#ifndef KDM_OSD_ICONS_H
#define KDM_OSD_ICONS_H

#include "osd_canvas.h"

/* --- Top-Right Status Bar Icons --- */
void osd_icons_draw_wifi(osd_canvas_t *c, float x, float y, float size, int bars, bool connected);
void osd_icons_draw_battery(osd_canvas_t *c, float x, float y, float w, float h, int pct, bool charging);

/* --- Slider Endpoint Icons --- */
void osd_icons_draw_sun(osd_canvas_t *c, float cx, float cy, float size, bool is_max,
                        uint8_t r, uint8_t g, uint8_t b, float alpha);
void osd_icons_draw_speaker(osd_canvas_t *c, float cx, float cy, float size, bool with_waves,
                            uint8_t r, uint8_t g, uint8_t b, float alpha);
void osd_icons_draw_chevron_down(osd_canvas_t *c, float cx, float cy, float size,
                                 uint8_t r, uint8_t g, uint8_t b, float alpha);
void osd_icons_draw_gear(osd_canvas_t *c, float cx, float cy, float size,
                         uint8_t r, uint8_t g, uint8_t b, float alpha);
void osd_icons_draw_search(osd_canvas_t *c, float cx, float cy, float size,
                           uint8_t r, uint8_t g, uint8_t b, float alpha);
void osd_icons_draw_power(osd_canvas_t *c, float cx, float cy, float size,
                          uint8_t r, uint8_t g, uint8_t b, float alpha);
void osd_icons_draw_dots_v(osd_canvas_t *c, float cx, float cy, float size,
                           uint8_t r, uint8_t g, uint8_t b, float alpha);

/* --- Quick Settings Circular Toggle Button Icons --- */
void osd_icons_draw_qs_button(osd_canvas_t *c, float cx, float cy, float radius,
                              osd_qs_id_t id, bool active);
const char *osd_icons_get_qs_label(osd_qs_id_t id);

#endif /* KDM_OSD_ICONS_H */

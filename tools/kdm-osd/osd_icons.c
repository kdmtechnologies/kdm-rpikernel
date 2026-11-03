/*
 * osd_icons.c - Implementation of OsdIcons Vector Icon Renderer Class
 */
#include "osd_icons.h"
#include <math.h>

static void draw_wifi_quarter_layer(osd_canvas_t *s, float x, float y, float size, int bars,
                                    bool connected, uint8_t r, uint8_t g, uint8_t b, float base_alpha) {
    float ox = x + size * 0.12f;
    float oy = y + size * 0.88f;

    float dot_r = size * 0.105f;
    osd_canvas_fill_circle(s, ox, oy, dot_r, r, g, b, base_alpha * (connected ? 1.0f : 0.35f));

    float radii[3] = { size * 0.33f, size * 0.57f, size * 0.81f };
    float thickness = size * 0.128f;
    float half_t = thickness * 0.5f;

    int x0 = (int)floorf(x - 3.0f);
    int y0 = (int)floorf(y - 3.0f);
    int x1 = (int)ceilf(x + size + 3.0f);
    int y1 = (int)ceilf(y + size + 3.0f);

    for (int i = 0; i < 3; i++) {
        float rad = radii[i];
        float a_mult = base_alpha * ((connected && bars >= (i + 1)) ? 1.0f : 0.28f);

        for (int py = y0; py <= y1; py++) {
            for (int px = x0; px <= x1; px++) {
                float dx = (px + 0.5f) - ox;
                float dy = oy - (py + 0.5f);
                if (dx < -half_t || dy < -half_t)
                    continue;
                float d;
                if (dx >= 0.0f && dy >= 0.0f)
                    d = fabsf(hypotf(dx, dy) - rad) - half_t;
                else if (dx < 0.0f)
                    d = hypotf(dx, dy - rad) - half_t;
                else
                    d = hypotf(dx - rad, dy) - half_t;
                float cov = clampf(0.5f - d, 0.0f, 1.0f);
                if (cov > 0.0f)
                    osd_canvas_blend_pixel(s, px, py, r, g, b, a_mult * cov);
            }
        }
    }
}

void osd_icons_draw_wifi(osd_canvas_t *c, float x, float y, float size, int bars, bool connected) {
    draw_wifi_quarter_layer(c, x + 1.5f, y + 2.0f, size, bars, connected, 0, 0, 0, 0.55f);
    draw_wifi_quarter_layer(c, x, y, size, bars, connected, 248, 250, 252, 1.0f);
}

void osd_icons_draw_battery(osd_canvas_t *c, float x, float y, float w, float h, int pct, bool charging) {
    pct = clampi(pct, 0, 100);
    float nub_w = w * 0.09f;
    float nub_gap = fmaxf(1.5f, h * 0.045f);
    float body_w = w - nub_w - nub_gap;
    float stroke = fmaxf(2.4f, h * 0.12f);
    float radius = h * 0.22f;

    osd_canvas_stroke_rrect(c, x + 1.5f, y + 2.0f, body_w, h, radius, stroke + 0.8f, 0, 0, 0, 0.50f);
    osd_canvas_fill_rrect(c, x + stroke * 0.5f, y + stroke * 0.5f,
                          body_w - stroke, h - stroke, radius * 0.6f, 14, 18, 24, 0.55f);
    osd_canvas_stroke_rrect(c, x, y, body_w, h, radius, stroke, 248, 250, 252, 0.98f);

    float nub_h = h * 0.44f;
    osd_canvas_fill_rrect(c, x + body_w + nub_gap * 0.6f, y + (h - nub_h) * 0.5f,
                          nub_w, nub_h, fmaxf(1.5f, nub_w * 0.45f), 248, 250, 252, 0.98f);

    float pad = stroke + fmaxf(2.0f, h * 0.075f);
    float max_fill_w = body_w - pad * 2.0f;
    float fill_h = h - pad * 2.0f;
    float fill_w = (max_fill_w * pct) / 100.0f;
    float fill_r = fmaxf(1.8f, h * 0.08f);

    if (fill_w > 1.0f && fill_h > 1.0f) {
        uint8_t fr = 94, fg = 178, fb = 76;
        if (pct <= 15 && !charging) {
            fr = 228; fg = 68; fb = 68;
        } else if (pct <= 30 && !charging) {
            fr = 235; fg = 165; fb = 22;
        }
        osd_canvas_fill_rrect(c, x + pad, y + pad, fill_w, fill_h, fill_r, fr, fg, fb, 1.0f);
        if (fill_h >= 4.0f) {
            osd_canvas_fill_rrect(c, x + pad + 0.5f, y + pad + 0.5f,
                                  fmaxf(1.0f, fill_w - 1.0f), fill_h * 0.35f, fill_r * 0.7f,
                                  255, 255, 255, 0.20f);
        }
    }

    if (charging) {
        float cx = x + body_w * 0.5f;
        float cy = y + h * 0.5f;
        float bt = fmaxf(2.2f, h * 0.11f);
        osd_canvas_draw_segment(c, cx + h * 0.09f, cy - h * 0.28f, cx - h * 0.12f, cy + h * 0.03f, bt, true, 255, 255, 255, 1.0f);
        osd_canvas_draw_segment(c, cx - h * 0.12f, cy + h * 0.03f, cx + h * 0.12f, cy + h * 0.03f, bt, true, 255, 255, 255, 1.0f);
        osd_canvas_draw_segment(c, cx + h * 0.12f, cy + h * 0.03f, cx - h * 0.09f, cy + h * 0.28f, bt, true, 255, 255, 255, 1.0f);
    }
}

void osd_icons_draw_sun(osd_canvas_t *c, float cx, float cy, float size, bool is_max,
                        uint8_t r, uint8_t g, uint8_t b, float alpha) {
    float core_r = size * 0.23f;
    osd_canvas_fill_circle(c, cx, cy, core_r, r, g, b, alpha);

    if (!is_max) {
        float dot_ring_r = size * 0.39f;
        float dot_r = fmaxf(1.5f, size * 0.062f);
        for (int i = 0; i < 8; i++) {
            float ang = (float)i * (PI_F * 0.25f);
            osd_canvas_fill_circle(c, cx + cosf(ang) * dot_ring_r, cy + sinf(ang) * dot_ring_r,
                                   dot_r, r, g, b, alpha);
        }
    } else {
        float ray_in  = size * 0.33f;
        float ray_out = size * 0.49f;
        float ray_w   = fmaxf(2.0f, size * 0.085f);
        for (int i = 0; i < 8; i++) {
            float ang = (float)i * (PI_F * 0.25f);
            float cs = cosf(ang), sn = sinf(ang);
            osd_canvas_draw_segment(c, cx + cs * ray_in, cy + sn * ray_in,
                                    cx + cs * ray_out, cy + sn * ray_out,
                                    ray_w, true, r, g, b, alpha);
        }
    }
}

void osd_icons_draw_speaker(osd_canvas_t *c, float cx, float cy, float size, bool with_waves,
                            uint8_t r, uint8_t g, uint8_t b, float alpha) {
    float sx = with_waves ? (cx - size * 0.14f) : (cx + size * 0.04f);

    osd_canvas_fill_rrect(c, sx - size * 0.30f, cy - size * 0.16f,
                          size * 0.22f, size * 0.32f, fmaxf(1.8f, size * 0.05f), r, g, b, alpha);

    int cone_steps = (int)ceilf(size * 0.6f);
    if (cone_steps < 12)
        cone_steps = 12;
    float seg_w = fmaxf(2.0f, size * 0.075f);
    for (int i = 0; i <= cone_steps; i++) {
        float t = (float)i / (float)cone_steps;
        float x = (sx - size * 0.10f) + t * (size * 0.22f);
        float half_h = size * (0.14f + t * 0.23f);
        osd_canvas_draw_segment(c, x, cy - half_h, x, cy + half_h, seg_w, true, r, g, b, alpha);
    }

    if (!with_waves)
        return;

    float radii[3] = { size * 0.22f, size * 0.35f, size * 0.48f };
    float thick = fmaxf(2.0f, size * 0.082f);
    for (int w = 0; w < 3; w++) {
        osd_canvas_draw_arc(c, sx, cy, radii[w], -0.68f, 0.68f, thick, r, g, b, alpha);
    }
}

void osd_icons_draw_chevron_down(osd_canvas_t *c, float cx, float cy, float size,
                                 uint8_t r, uint8_t g, uint8_t b, float alpha) {
    float hw = size * 0.36f;
    float hh = size * 0.19f;
    float sw = fmaxf(2.0f, size * 0.105f);
    osd_canvas_draw_segment(c, cx - hw, cy - hh, cx, cy + hh, sw, true, r, g, b, alpha);
    osd_canvas_draw_segment(c, cx, cy + hh, cx + hw, cy - hh, sw, true, r, g, b, alpha);
}

void osd_icons_draw_gear(osd_canvas_t *c, float cx, float cy, float size,
                         uint8_t r, uint8_t g, uint8_t b, float alpha) {
    /* 8 gear teeth around outer ring */
    float tooth_in  = size * 0.28f;
    float tooth_out = size * 0.46f;
    float tooth_w   = fmaxf(2.8f, size * 0.15f);
    for (int i = 0; i < 8; i++) {
        float a = (float)i * (PI_F * 0.25f);
        osd_canvas_draw_segment(c,
                                cx + cosf(a) * tooth_in,  cy + sinf(a) * tooth_in,
                                cx + cosf(a) * tooth_out, cy + sinf(a) * tooth_out,
                                tooth_w, true, r, g, b, alpha);
    }
    /* Thick ring body */
    osd_canvas_stroke_circle(c, cx, cy, size * 0.28f, size * 0.18f, r, g, b, alpha);
}

void osd_icons_draw_search(osd_canvas_t *c, float cx, float cy, float size,
                           uint8_t r, uint8_t g, uint8_t b, float alpha) {
    float sw = fmaxf(2.2f, size * 0.11f);
    float ring_r = size * 0.30f;
    float rcx = cx - size * 0.06f;
    float rcy = cy - size * 0.06f;
    osd_canvas_stroke_circle(c, rcx, rcy, ring_r, sw, r, g, b, alpha);
    float h0 = ring_r * 0.72f;
    float h1 = size * 0.42f;
    osd_canvas_draw_segment(c, rcx + h0, rcy + h0, cx + h1 * 0.85f, cy + h1 * 0.85f,
                            sw * 1.05f, true, r, g, b, alpha);
}

void osd_icons_draw_power(osd_canvas_t *c, float cx, float cy, float size,
                          uint8_t r, uint8_t g, uint8_t b, float alpha) {
    float sw = fmaxf(2.2f, size * 0.11f);
    float rad = size * 0.36f;
    /* Arc with gap at top (-PI/2) */
    osd_canvas_draw_arc(c, cx, cy + size * 0.03f, rad,
                        -PI_F * 0.28f, PI_F * 1.28f, sw, r, g, b, alpha);
    /* Top vertical stem */
    osd_canvas_draw_segment(c, cx, cy - size * 0.42f, cx, cy - size * 0.02f,
                            sw, true, r, g, b, alpha);
}

void osd_icons_draw_dots_v(osd_canvas_t *c, float cx, float cy, float size,
                           uint8_t r, uint8_t g, uint8_t b, float alpha) {
    float dot_r = fmaxf(2.2f, size * 0.095f);
    float step  = size * 0.31f;
    osd_canvas_fill_circle(c, cx, cy - step, dot_r, r, g, b, alpha);
    osd_canvas_fill_circle(c, cx, cy,        dot_r, r, g, b, alpha);
    osd_canvas_fill_circle(c, cx, cy + step, dot_r, r, g, b, alpha);
}

/* --- 7 Circular Quick Settings Toggle Icons (Matching Reference Screenshot) --- */

static void draw_qs_wifi_glyph(osd_canvas_t *c, float cx, float cy, float sz,
                               uint8_t r, uint8_t g, uint8_t b, float a) {
    float by = cy + sz * 0.30f;
    osd_canvas_fill_circle(c, cx, by, sz * 0.085f, r, g, b, a);
    float sw = fmaxf(2.1f, sz * 0.095f);
    osd_canvas_draw_arc(c, cx, by, sz * 0.26f, -PI_F * 0.76f, -PI_F * 0.24f, sw, r, g, b, a);
    osd_canvas_draw_arc(c, cx, by, sz * 0.45f, -PI_F * 0.77f, -PI_F * 0.23f, sw, r, g, b, a);
    osd_canvas_draw_arc(c, cx, by, sz * 0.64f, -PI_F * 0.78f, -PI_F * 0.22f, sw, r, g, b, a);
}

static void draw_qs_data_glyph(osd_canvas_t *c, float cx, float cy, float sz,
                               uint8_t r, uint8_t g, uint8_t b, float a) {
    float sw = fmaxf(2.1f, sz * 0.095f);
    float lx = cx - sz * 0.18f;
    float rx = cx + sz * 0.18f;
    float top_y = cy - sz * 0.34f;
    float bot_y = cy + sz * 0.34f;
    float wing  = sz * 0.16f;

    /* Left Down Arrow (↓) */
    osd_canvas_draw_segment(c, lx, top_y, lx, bot_y, sw, true, r, g, b, a);
    osd_canvas_draw_segment(c, lx - wing, bot_y - wing, lx, bot_y, sw, true, r, g, b, a);
    osd_canvas_draw_segment(c, lx + wing, bot_y - wing, lx, bot_y, sw, true, r, g, b, a);

    /* Right Up Arrow (↑) */
    osd_canvas_draw_segment(c, rx, bot_y, rx, top_y, sw, true, r, g, b, a);
    osd_canvas_draw_segment(c, rx - wing, top_y + wing, rx, top_y, sw, true, r, g, b, a);
    osd_canvas_draw_segment(c, rx + wing, top_y + wing, rx, top_y, sw, true, r, g, b, a);
}

static void draw_qs_sound_glyph(osd_canvas_t *c, float cx, float cy, float sz, bool active,
                                uint8_t r, uint8_t g, uint8_t b, float a) {
    osd_icons_draw_speaker(c, cx, cy, sz * 0.88f, active, r, g, b, a);
    if (!active) {
        float sw = fmaxf(2.2f, sz * 0.085f);
        osd_canvas_draw_segment(c, cx - sz * 0.32f, cy - sz * 0.30f,
                                cx + sz * 0.28f, cy + sz * 0.30f, sw, true, r, g, b, a);
    }
}

static void draw_qs_calls_glyph(osd_canvas_t *c, float cx, float cy, float sz,
                                uint8_t r, uint8_t g, uint8_t b, float a) {
    float sw  = fmaxf(2.0f, sz * 0.085f);
    float acx = cx - sz * 0.02f;
    float acy = cy + sz * 0.02f;
    float rad = sz * 0.28f;
    float a0  = PI_F * 0.56f;
    float a1  = PI_F * 1.19f;
    /* Handset spine arc on bottom-left */
    osd_canvas_draw_arc(c, acx, acy, rad, a0, a1, sw * 1.35f, r, g, b, a);
    /* Connected earpiece & mouthpiece pads at exact arc endpoints */
    float x0 = acx + cosf(a0) * (rad - sw * 0.25f);
    float y0 = acy + sinf(a0) * (rad - sw * 0.25f);
    float x1 = acx + cosf(a1) * (rad - sw * 0.25f);
    float y1 = acy + sinf(a1) * (rad - sw * 0.25f);
    osd_canvas_fill_circle(c, x0, y0, sw * 1.15f, r, g, b, a);
    osd_canvas_fill_circle(c, x1, y1, sw * 1.15f, r, g, b, a);
    /* Small Wi-Fi waves on top-right */
    float wx = cx + sz * 0.05f;
    float wy = cy - sz * 0.03f;
    osd_canvas_fill_circle(c, wx, wy, sz * 0.045f, r, g, b, a);
    osd_canvas_draw_arc(c, wx, wy, sz * 0.14f, -PI_F * 0.72f, -PI_F * 0.20f, sw * 0.8f, r, g, b, a);
    osd_canvas_draw_arc(c, wx, wy, sz * 0.24f, -PI_F * 0.72f, -PI_F * 0.20f, sw * 0.8f, r, g, b, a);
}

static void draw_qs_torch_glyph(osd_canvas_t *c, float cx, float cy, float sz,
                                uint8_t r, uint8_t g, uint8_t b, float a,
                                uint8_t bg_r, uint8_t bg_g, uint8_t bg_b) {
    /* Flashlight head + handle body */
    float head_w = sz * 0.36f;
    float head_h = sz * 0.16f;
    float body_w = sz * 0.22f;
    float body_h = sz * 0.44f;
    osd_canvas_fill_rrect(c, cx - head_w * 0.5f, cy - sz * 0.18f, head_w, head_h, 3.0f, r, g, b, a);
    osd_canvas_fill_rrect(c, cx - body_w * 0.5f, cy - sz * 0.04f, body_w, body_h, 3.0f, r, g, b, a);
    /* Power button cutout on handle */
    osd_canvas_fill_circle(c, cx, cy + sz * 0.12f, sz * 0.045f, bg_r, bg_g, bg_b, 1.0f);
    /* 3 light rays above flashlight head */
    float sw = fmaxf(1.8f, sz * 0.065f);
    osd_canvas_draw_segment(c, cx, cy - sz * 0.26f, cx, cy - sz * 0.36f, sw, true, r, g, b, a);
    osd_canvas_draw_segment(c, cx - sz * 0.12f, cy - sz * 0.25f, cx - sz * 0.17f, cy - sz * 0.34f, sw, true, r, g, b, a);
    osd_canvas_draw_segment(c, cx + sz * 0.12f, cy - sz * 0.25f, cx + sz * 0.17f, cy - sz * 0.34f, sw, true, r, g, b, a);
}

static void draw_qs_bluetooth_glyph(osd_canvas_t *c, float cx, float cy, float sz,
                                    uint8_t r, uint8_t g, uint8_t b, float a) {
    float sw = fmaxf(2.0f, sz * 0.085f);
    float top_y = cy - sz * 0.38f;
    float bot_y = cy + sz * 0.38f;
    float rx    = cx + sz * 0.24f;
    float lx    = cx - sz * 0.22f;

    osd_canvas_draw_segment(c, cx, top_y, cx, bot_y, sw, true, r, g, b, a);
    osd_canvas_draw_segment(c, cx, top_y, rx, cy - sz * 0.19f, sw, true, r, g, b, a);
    osd_canvas_draw_segment(c, rx, cy - sz * 0.19f, lx, cy + sz * 0.19f, sw, true, r, g, b, a);
    osd_canvas_draw_segment(c, lx, cy - sz * 0.19f, rx, cy + sz * 0.19f, sw, true, r, g, b, a);
    osd_canvas_draw_segment(c, rx, cy + sz * 0.19f, cx, bot_y, sw, true, r, g, b, a);
}

static void draw_qs_location_glyph(osd_canvas_t *c, float cx, float cy, float sz,
                                   uint8_t r, uint8_t g, uint8_t b, float a,
                                   uint8_t bg_r, uint8_t bg_g, uint8_t bg_b) {
    float head_cy = cy - sz * 0.10f;
    float head_r  = sz * 0.26f;
    float tip_y   = cy + sz * 0.38f;

    /* Upper circular head of pin */
    osd_canvas_fill_circle(c, cx, head_cy, head_r, r, g, b, a);
    /* Tapered lower cone to bottom tip */
    for (int i = 0; i <= 14; i++) {
        float t = (float)i / 14.0f;
        float y = head_cy + head_r * 0.35f + t * (tip_y - (head_cy + head_r * 0.35f));
        float hw = head_r * 0.92f * (1.0f - t);
        osd_canvas_draw_segment(c, cx - hw, y, cx + hw, y, 2.5f, true, r, g, b, a);
    }
    /* Inner hole punched with button background color */
    osd_canvas_fill_circle(c, cx, head_cy, head_r * 0.42f, bg_r, bg_g, bg_b, 1.0f);
}

void osd_icons_draw_qs_button(osd_canvas_t *c, float cx, float cy, float radius,
                              osd_qs_id_t id, bool active) {
    /* Active = Soft Sage-Green (#8CB6A0), Inactive = Dark Translucent Grey (#48444E) */
    uint8_t bg_r = active ? 140 : 72;
    uint8_t bg_g = active ? 182 : 68;
    uint8_t bg_b = active ? 160 : 78;

    uint8_t fg_r = active ? 255 : 210;
    uint8_t fg_g = active ? 255 : 212;
    uint8_t fg_b = active ? 255 : 218;

    osd_canvas_fill_circle(c, cx, cy, radius, bg_r, bg_g, bg_b, active ? 0.98f : 0.90f);

    float glyph_sz = radius * 1.05f;
    switch (id) {
        case OSD_QS_WIFI:
            draw_qs_wifi_glyph(c, cx, cy, glyph_sz, fg_r, fg_g, fg_b, 1.0f);
            break;
        case OSD_QS_DATA:
            draw_qs_data_glyph(c, cx, cy, glyph_sz, fg_r, fg_g, fg_b, 1.0f);
            break;
        case OSD_QS_BLUETOOTH:
            draw_qs_bluetooth_glyph(c, cx, cy, glyph_sz, fg_r, fg_g, fg_b, 1.0f);
            break;
        case OSD_QS_CALLS:
            draw_qs_calls_glyph(c, cx, cy, glyph_sz, fg_r, fg_g, fg_b, 1.0f);
            break;
        case OSD_QS_LOCATION:
            draw_qs_location_glyph(c, cx, cy, glyph_sz, fg_r, fg_g, fg_b, 1.0f, bg_r, bg_g, bg_b);
            break;
        case OSD_QS_SOUND:
            draw_qs_sound_glyph(c, cx, cy, glyph_sz, active, fg_r, fg_g, fg_b, 1.0f);
            break;
        case OSD_QS_TORCH:
            draw_qs_torch_glyph(c, cx, cy, glyph_sz, fg_r, fg_g, fg_b, 1.0f, bg_r, bg_g, bg_b);
            break;
    }
}

const char *osd_icons_get_qs_label(osd_qs_id_t id) {
    switch (id) {
        case OSD_QS_WIFI:      return "Wi-Fi";
        case OSD_QS_DATA:      return "Mobile data";
        case OSD_QS_BLUETOOTH: return "Bluetooth";
        case OSD_QS_CALLS:     return "Wi-Fi Calling";
        case OSD_QS_LOCATION:  return "Location";
        case OSD_QS_SOUND:     return "Sound";
        case OSD_QS_TORCH:     return "Torch";
        default:               return "";
    }
}

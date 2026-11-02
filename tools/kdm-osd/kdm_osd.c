/*
 * kdm_osd.c - Direct DRM/KMS Transparent Status Overlay + iPhone-Style
 *             Brightness & Volume Control Center Sliders for KDM CM4 Handheld
 *
 * Features:
 * - 100% transparent background top-right status bar (Wi-Fi, Battery, 12h Clock)
 *   with subtle drop shadows for readability over any background.
 * - iPhone Control Center-style vertical capsule sliders for Brightness (Sun icon)
 *   and Volume (Speaker + sound waves icon).
 * - Interactive touchscreen control (/dev/input/event* goodix_ts):
 *   - Swipe down or tap top-right status bar to open Control Center sliders
 *   - Drag Brightness slider to control RM67199 OLED DSI brightness in real time
 *   - Drag Volume slider to control system/mockup volume (0-100%)
 *   - Tap outside or wait 4s to auto-hide
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/input.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include <png.h>

#ifndef __NR_pidfd_open
#define __NR_pidfd_open 434
#endif
#ifndef __NR_pidfd_getfd
#define __NR_pidfd_getfd 438
#endif

#define PI_F 3.14159265358979323846f

static volatile sig_atomic_t g_running = 1;

static void handle_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

typedef struct {
    int rotate;             /* 0, 90, 180, 270 (default 270 for RM67199 landscape) */
    float scale;            /* UI scale factor (default 1.15) */
    int margin_top;         /* Top margin in landscape pixels */
    int margin_right;       /* Right margin in landscape pixels */
    int mock_battery;       /* Mock battery % (0..100, default 75) */
    bool mock_charging;     /* Mock charging state */
    int mock_wifi;          /* -1 = live /proc/net/wireless, 0..3 = mock bars */
    int init_volume;        /* Initial volume % (0..100, default 70) */
    int init_brightness;    /* -1 = read /sys/class/backlight, or 0..100 */
    const char *mock_time;  /* NULL = live system clock, or e.g. "1:14PM" */
    bool format_24h;        /* 24-hour clock format */
    bool always_sliders;    /* Keep Control Center sliders visible */
    const char *preview_out;/* Render PNG preview to path and exit */
    const char *drm_path;   /* Optional explicit /dev/dri/cardX */
} osd_config_t;

typedef struct {
    int battery_pct;
    bool charging;
    int wifi_bars;          /* 0..3 */
    bool wifi_connected;
    int brightness_pct;     /* 0..100 */
    int volume_pct;         /* 0..100 */
    bool vol_open;
    bool brt_open;
    uint64_t sliders_activity_ms;
    char time_str[32];

    /* Layout hit-boxes inside the overlay surface (landscape coords relative to overlay) */
    int surf_w;
    int surf_h;
    int bar_h;
    float panel_x, panel_y, panel_w, panel_h;
    float brt_x, brt_y, brt_w, brt_h;
    float vol_x, vol_y, vol_w, vol_h;
} osd_state_t;

typedef struct {
    int width;
    int height;
    uint32_t *pixels;       /* ARGB8888 (0xAARRGGBB) */
} surface_t;

/* --- Math & Anti-Aliased SDF Drawing Helpers --- */

static inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline uint32_t pack_argb(uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static inline void blend_pixel(surface_t *s, int x, int y, uint8_t r, uint8_t g, uint8_t b, float alpha) {
    if (x < 0 || y < 0 || x >= s->width || y >= s->height || alpha <= 0.0f)
        return;
    uint8_t sa = (uint8_t)(clampf(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
    if (sa == 0)
        return;

    uint32_t dst = s->pixels[y * s->width + x];
    uint8_t da = (dst >> 24) & 0xFF;
    uint8_t dr = (dst >> 16) & 0xFF;
    uint8_t dg = (dst >> 8) & 0xFF;
    uint8_t db = dst & 0xFF;

    if (sa == 255 || da == 0) {
        s->pixels[y * s->width + x] = pack_argb(sa, r, g, b);
        return;
    }

    float fa = sa / 255.0f;
    float fda = da / 255.0f;
    float out_a = fa + fda * (1.0f - fa);
    if (out_a <= 0.0f) {
        s->pixels[y * s->width + x] = 0;
        return;
    }
    uint8_t or_ = (uint8_t)clampf((r * fa + dr * fda * (1.0f - fa)) / out_a + 0.5f, 0.0f, 255.0f);
    uint8_t og  = (uint8_t)clampf((g * fa + dg * fda * (1.0f - fa)) / out_a + 0.5f, 0.0f, 255.0f);
    uint8_t ob  = (uint8_t)clampf((b * fa + db * fda * (1.0f - fa)) / out_a + 0.5f, 0.0f, 255.0f);
    uint8_t oa  = (uint8_t)clampf(out_a * 255.0f + 0.5f, 0.0f, 255.0f);
    s->pixels[y * s->width + x] = pack_argb(oa, or_, og, ob);
}

static float sdf_rrect(float px, float py, float cx, float cy, float hx, float hy, float radius) {
    float qx = fabsf(px - cx) - (hx - radius);
    float qy = fabsf(py - cy) - (hy - radius);
    float out_d = hypotf(fmaxf(qx, 0.0f), fmaxf(qy, 0.0f));
    float in_d  = fminf(fmaxf(qx, qy), 0.0f);
    return out_d + in_d - radius;
}

static void draw_filled_rrect(surface_t *s, float x, float y, float w, float h, float radius,
                              uint8_t r, uint8_t g, uint8_t b, float alpha) {
    float cx = x + w * 0.5f;
    float cy = y + h * 0.5f;
    float hx = w * 0.5f;
    float hy = h * 0.5f;
    int x0 = (int)floorf(x - 2.0f);
    int y0 = (int)floorf(y - 2.0f);
    int x1 = (int)ceilf(x + w + 2.0f);
    int y1 = (int)ceilf(y + h + 2.0f);

    for (int py = y0; py <= y1; py++) {
        for (int px = x0; px <= x1; px++) {
            float d = sdf_rrect(px + 0.5f, py + 0.5f, cx, cy, hx, hy, radius);
            float cov = clampf(0.5f - d, 0.0f, 1.0f);
            if (cov > 0.0f)
                blend_pixel(s, px, py, r, g, b, alpha * cov);
        }
    }
}

static void draw_stroked_rrect(surface_t *s, float x, float y, float w, float h, float radius,
                               float stroke_w, uint8_t r, uint8_t g, uint8_t b, float alpha) {
    float cx = x + w * 0.5f;
    float cy = y + h * 0.5f;
    float hx = w * 0.5f;
    float hy = h * 0.5f;
    float half_t = stroke_w * 0.5f;
    int x0 = (int)floorf(x - stroke_w - 2.0f);
    int y0 = (int)floorf(y - stroke_w - 2.0f);
    int x1 = (int)ceilf(x + w + stroke_w + 2.0f);
    int y1 = (int)ceilf(y + h + stroke_w + 2.0f);

    for (int py = y0; py <= y1; py++) {
        for (int px = x0; px <= x1; px++) {
            float d = fabsf(sdf_rrect(px + 0.5f, py + 0.5f, cx, cy, hx, hy, radius)) - half_t;
            float cov = clampf(0.5f - d, 0.0f, 1.0f);
            if (cov > 0.0f)
                blend_pixel(s, px, py, r, g, b, alpha * cov);
        }
    }
}

static float dist_to_segment(float px, float py, float ax, float ay, float bx, float by) {
    float vx = bx - ax, vy = by - ay;
    float wx = px - ax, wy = py - ay;
    float c1 = vx * wx + vy * wy;
    if (c1 <= 0.0f)
        return hypotf(px - ax, py - ay);
    float c2 = vx * vx + vy * vy;
    if (c2 <= c1)
        return hypotf(px - bx, py - by);
    float t = c1 / c2;
    return hypotf(px - (ax + t * vx), py - (ay + t * vy));
}

static void draw_thick_segment(surface_t *s, float ax, float ay, float bx, float by,
                               float thickness, bool round_caps,
                               uint8_t r, uint8_t g, uint8_t b, float alpha) {
    float radius = thickness * 0.5f;
    int x0 = (int)floorf(fminf(ax, bx) - radius - 2.0f);
    int y0 = (int)floorf(fminf(ay, by) - radius - 2.0f);
    int x1 = (int)ceilf(fmaxf(ax, bx) + radius + 2.0f);
    int y1 = (int)ceilf(fmaxf(ay, by) + radius + 2.0f);

    float vx = bx - ax, vy = by - ay;
    float len = hypotf(vx, vy);
    if (len < 1e-4f)
        return;
    float ux = vx / len, uy = vy / len;

    for (int py = y0; py <= y1; py++) {
        for (int px = x0; px <= x1; px++) {
            float fx = px + 0.5f, fy = py + 0.5f;
            float d;
            if (round_caps) {
                d = dist_to_segment(fx, fy, ax, ay, bx, by) - radius;
            } else {
                float proj = (fx - ax) * ux + (fy - ay) * uy;
                float perp = fabsf(-(fx - ax) * uy + (fy - ay) * ux);
                float dx = fmaxf(-proj, proj - len);
                float dy = perp - radius;
                d = hypotf(fmaxf(dx, 0.0f), fmaxf(dy, 0.0f)) + fminf(fmaxf(dx, dy), 0.0f);
            }
            float cov = clampf(0.5f - d, 0.0f, 1.0f);
            if (cov > 0.0f)
                blend_pixel(s, px, py, r, g, b, alpha * cov);
        }
    }
}

static void draw_filled_circle(surface_t *s, float cx, float cy, float radius,
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
                blend_pixel(s, px, py, r, g, b, alpha * cov);
        }
    }
}

/* --- Status Bar Icons (Transparent Background + Drop Shadow) --- */

static void draw_wifi_icon_layer(surface_t *s, float x, float y, float size, int bars,
                                 bool connected, uint8_t r, uint8_t g, uint8_t b, float base_alpha) {
    float ox = x + size * 0.12f;
    float oy = y + size * 0.88f;

    float dot_r = size * 0.105f;
    draw_filled_circle(s, ox, oy, dot_r, r, g, b, base_alpha * (connected ? 1.0f : 0.35f));

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
                    blend_pixel(s, px, py, r, g, b, a_mult * cov);
            }
        }
    }
}

static void draw_wifi_icon(surface_t *s, float x, float y, float size, int bars, bool connected) {
    /* Subtle dark drop shadow so icon stays crisp on bright backgrounds */
    draw_wifi_icon_layer(s, x + 1.5f, y + 2.0f, size, bars, connected, 0, 0, 0, 0.55f);
    draw_wifi_icon_layer(s, x, y, size, bars, connected, 248, 250, 252, 1.0f);
}

static void draw_battery_icon(surface_t *s, float x, float y, float w, float h, int pct, bool charging) {
    pct = clampi(pct, 0, 100);
    float nub_w = w * 0.09f;
    float nub_gap = fmaxf(1.5f, h * 0.045f);
    float body_w = w - nub_w - nub_gap;
    float stroke = fmaxf(2.4f, h * 0.12f);
    float radius = h * 0.22f;

    /* Drop shadow for outer frame */
    draw_stroked_rrect(s, x + 1.5f, y + 2.0f, body_w, h, radius, stroke + 0.8f, 0, 0, 0, 0.50f);

    /* Subtle dark translucent interior so green bar pops on any background */
    draw_filled_rrect(s, x + stroke * 0.5f, y + stroke * 0.5f,
                      body_w - stroke, h - stroke, radius * 0.6f,
                      14, 18, 24, 0.55f);

    /* Crisp white outer frame + right terminal nub */
    draw_stroked_rrect(s, x, y, body_w, h, radius, stroke, 248, 250, 252, 0.98f);
    float nub_h = h * 0.44f;
    draw_filled_rrect(s, x + body_w + nub_gap * 0.6f, y + (h - nub_h) * 0.5f,
                      nub_w, nub_h, fmaxf(1.5f, nub_w * 0.45f), 248, 250, 252, 0.98f);

    /* Inner fill bar */
    float pad = stroke + fmaxf(2.0f, h * 0.075f);
    float max_fill_w = body_w - pad * 2.0f;
    float fill_h = h - pad * 2.0f;
    float fill_w = (max_fill_w * pct) / 100.0f;
    float fill_r = fmaxf(1.8f, h * 0.08f);

    if (fill_w > 1.0f && fill_h > 1.0f) {
        uint8_t fr = 94, fg = 178, fb = 76; /* Sample green */
        if (pct <= 15 && !charging) {
            fr = 228; fg = 68; fb = 68;
        } else if (pct <= 30 && !charging) {
            fr = 235; fg = 165; fb = 22;
        }
        draw_filled_rrect(s, x + pad, y + pad, fill_w, fill_h, fill_r, fr, fg, fb, 1.0f);
        if (fill_h >= 4.0f) {
            draw_filled_rrect(s, x + pad + 0.5f, y + pad + 0.5f,
                              fmaxf(1.0f, fill_w - 1.0f), fill_h * 0.35f, fill_r * 0.7f,
                              255, 255, 255, 0.20f);
        }
    }

    if (charging) {
        float cx = x + body_w * 0.5f;
        float cy = y + h * 0.5f;
        float bt = fmaxf(2.2f, h * 0.11f);
        draw_thick_segment(s, cx + h * 0.09f, cy - h * 0.28f, cx - h * 0.12f, cy + h * 0.03f, bt, true, 255, 255, 255, 1.0f);
        draw_thick_segment(s, cx - h * 0.12f, cy + h * 0.03f, cx + h * 0.12f, cy + h * 0.03f, bt, true, 255, 255, 255, 1.0f);
        draw_thick_segment(s, cx + h * 0.12f, cy + h * 0.03f, cx - h * 0.09f, cy + h * 0.28f, bt, true, 255, 255, 255, 1.0f);
    }
}

/* --- Horizontal Dark/Transparent Volume & Brightness Sliders --- */

/*
 * Draw the horizontal slider track + white fill pill + 16 step dots underneath:
 * - Track: slim rounded pill in dark translucent gray (with subtle drop shadow)
 * - Fill: bright white rounded pill from left to pct%
 * - Step dots: 16 evenly-spaced small dots below the track
 */
static void draw_horizontal_slider_track(surface_t *s, float track_x, float track_cy,
                                         float track_w, float track_h, int pct, float scale) {
    pct = clampi(pct, 0, 100);
    float radius = track_h * 0.5f;
    float track_y = track_cy - radius;

    /* 1. Subtle drop shadow for legibility on transparent background */
    draw_filled_rrect(s, track_x, track_y + 2.0f, track_w, track_h, radius, 0, 0, 0, 0.45f);

    /* 2. Unfilled dark gray pill track */
    draw_filled_rrect(s, track_x, track_y, track_w, track_h, radius, 66, 70, 78, 0.88f);

    /* 3. Bright white rounded pill fill from left to pct% */
    if (pct > 0) {
        float fill_w = (track_w * (float)pct) / 100.0f;
        if (fill_w < track_h)
            fill_w = track_h;
        draw_filled_rrect(s, track_x, track_y, fill_w, track_h, radius, 250, 252, 255, 0.98f);
    }

    /* 4. Row of 16 subtle step dots underneath the track */
    const int num_dots = 16;
    float dots_y = track_cy + radius + roundf(8.0f * scale);
    float dot_r  = fmaxf(1.5f, 1.6f * scale);
    float start_x = track_x + radius + 2.0f * scale;
    float end_x   = track_x + track_w - radius - 2.0f * scale;

    for (int i = 0; i < num_dots; i++) {
        float dx = start_x + (end_x - start_x) * ((float)i / (float)(num_dots - 1));
        draw_filled_circle(s, dx + 0.8f, dots_y + 1.2f, dot_r, 0, 0, 0, 0.38f);
        draw_filled_circle(s, dx, dots_y, dot_r, 128, 134, 144, 0.72f);
    }
}

/*
 * Left/Right Sun Icons for the Brightness horizontal slider:
 * - is_max == false (left icon): center sun disk + 8 tiny dot rays
 * - is_max == true  (right icon): center sun disk + 8 radiating line rays
 */
static void draw_sun_endpoint_icon_layer(surface_t *s, float cx, float cy, float size,
                                         bool is_max, uint8_t r, uint8_t g, uint8_t b, float alpha) {
    float core_r = size * 0.22f;
    draw_filled_circle(s, cx, cy, core_r, r, g, b, alpha);

    if (!is_max) {
        /* 8 small dot rays around the sun disk */
        float dot_ring_r = size * 0.38f;
        float dot_r = fmaxf(1.8f, size * 0.062f);
        for (int i = 0; i < 8; i++) {
            float ang = (float)i * (PI_F * 0.25f);
            draw_filled_circle(s, cx + cosf(ang) * dot_ring_r, cy + sinf(ang) * dot_ring_r,
                               dot_r, r, g, b, alpha);
        }
    } else {
        /* 8 longer radiating line rays */
        float ray_in  = size * 0.33f;
        float ray_out = size * 0.49f;
        float ray_w   = fmaxf(2.2f, size * 0.088f);
        for (int i = 0; i < 8; i++) {
            float ang = (float)i * (PI_F * 0.25f);
            float c = cosf(ang), sn = sinf(ang);
            draw_thick_segment(s,
                               cx + c * ray_in,  cy + sn * ray_in,
                               cx + c * ray_out, cy + sn * ray_out,
                               ray_w, true, r, g, b, alpha);
        }
    }
}

static void draw_sun_endpoint_icon(surface_t *s, float cx, float cy, float size, bool is_max) {
    draw_sun_endpoint_icon_layer(s, cx + 1.5f, cy + 2.0f, size, is_max, 0, 0, 0, 0.50f);
    draw_sun_endpoint_icon_layer(s, cx, cy, size, is_max, 246, 248, 252, 0.96f);
}

/*
 * Left/Right Speaker Icons for the Volume horizontal slider:
 * - with_waves == false (left icon): speaker body + cone only
 * - with_waves == true  (right icon): speaker body + cone + 3 curved sound wave arcs
 */
static void draw_speaker_endpoint_icon_layer(surface_t *s, float cx, float cy, float size,
                                             bool with_waves, uint8_t r, uint8_t g, uint8_t b, float alpha) {
    float sx = with_waves ? (cx - size * 0.14f) : (cx + size * 0.04f);

    /* Rectangular back of speaker */
    draw_filled_rrect(s, sx - size * 0.25f, cy - size * 0.15f,
                      size * 0.17f, size * 0.30f, fmaxf(2.0f, size * 0.06f), r, g, b, alpha);

    /* Speaker cone polygon */
    int cone_steps = (int)ceilf(size * 0.6f);
    if (cone_steps < 12)
        cone_steps = 12;
    float seg_w = fmaxf(2.4f, size * 0.08f);
    for (int i = 0; i <= cone_steps; i++) {
        float t = (float)i / (float)cone_steps;
        float x = (sx - size * 0.12f) + t * (size * 0.23f);
        float half_h = size * (0.14f + t * 0.23f);
        draw_thick_segment(s, x, cy - half_h, x, cy + half_h, seg_w, true, r, g, b, alpha);
    }

    if (!with_waves)
        return;

    /* 3 sound wave arcs on the right */
    float radii[3] = { size * 0.22f, size * 0.35f, size * 0.48f };
    float thick = fmaxf(2.2f, size * 0.085f);
    for (int w = 0; w < 3; w++) {
        float rad = radii[w];
        const int steps = 16;
        float a0 = -0.68f, a1 = 0.68f;
        for (int k = 0; k < steps; k++) {
            float t0 = a0 + (a1 - a0) * ((float)k / steps);
            float t1 = a0 + (a1 - a0) * ((float)(k + 1) / steps);
            draw_thick_segment(s,
                               sx + cosf(t0) * rad, cy + sinf(t0) * rad,
                               sx + cosf(t1) * rad, cy + sinf(t1) * rad,
                               thick, true, r, g, b, alpha);
        }
    }
}

static void draw_speaker_endpoint_icon(surface_t *s, float cx, float cy, float size, bool with_waves) {
    draw_speaker_endpoint_icon_layer(s, cx + 1.5f, cy + 2.0f, size, with_waves, 0, 0, 0, 0.50f);
    draw_speaker_endpoint_icon_layer(s, cx, cy, size, with_waves, 246, 248, 252, 0.96f);
}

/* --- FreeType Text Rendering --- */

static const char *const k_font_paths[] = {
    "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/freefont/FreeSansBold.ttf",
    "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
    "/System/Library/Fonts/Helvetica.ttc",
    NULL
};

static bool init_freetype_face(FT_Library *lib, FT_Face *face, int pixel_height) {
    if (FT_Init_FreeType(lib) != 0)
        return false;
    for (int i = 0; k_font_paths[i] != NULL; i++) {
        if (access(k_font_paths[i], R_OK) == 0) {
            if (FT_New_Face(*lib, k_font_paths[i], 0, face) == 0) {
                FT_Set_Pixel_Sizes(*face, 0, (FT_UInt)pixel_height);
                return true;
            }
        }
    }
    return false;
}

static int measure_text_ft(FT_Face face, const char *text, int *out_ascender, int *out_height) {
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

static void draw_text_ft(surface_t *s, FT_Face face, const char *text,
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
                    blend_pixel(s, gx + (int)col, gy + (int)row, r, g, b, alpha * (cov / 255.0f));
            }
        }
        pen_x += (int)(slot->advance.x >> 6);
    }
}

/* --- Hardware & Mockup State Readers/Writers --- */

static char g_backlight_dir[256] = {0};
static int  g_backlight_max = 255;
static struct timespec g_vol_mtime = {0, 0};
static struct timespec g_brt_mtime = {0, 0};
static int  g_last_hw_raw = -1;

static bool timespec_changed(const struct timespec *a, const struct timespec *b) {
    return (a->tv_sec != b->tv_sec) || (a->tv_nsec != b->tv_nsec);
}

static void record_file_mtime(const char *path, struct timespec *out_mtime) {
    struct stat sb;
    if (stat(path, &sb) == 0)
        *out_mtime = sb.st_mtim;
}

static void discover_backlight(void) {
    DIR *dir = opendir("/sys/class/backlight");
    if (!dir)
        return;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.')
            continue;
        snprintf(g_backlight_dir, sizeof(g_backlight_dir), "/sys/class/backlight/%.180s", ent->d_name);
        char max_path[300];
        snprintf(max_path, sizeof(max_path), "%s/max_brightness", g_backlight_dir);
        FILE *fp = fopen(max_path, "r");
        if (fp) {
            if (fscanf(fp, "%d", &g_backlight_max) != 1 || g_backlight_max <= 0)
                g_backlight_max = 255;
            fclose(fp);
        }
        break;
    }
    closedir(dir);
}

static int raw_to_brightness_pct(int raw) {
    int min_raw = 10; /* Safe minimum so OLED never turns completely off */
    if (raw <= min_raw)
        return 0;
    int pct = ((raw - min_raw) * 100 + (g_backlight_max - min_raw) / 2) /
              (g_backlight_max - min_raw);
    return clampi(pct, 0, 100);
}

static int read_raw_backlight(void) {
    if (!g_backlight_dir[0])
        return -1;
    char path[300];
    snprintf(path, sizeof(path), "%s/brightness", g_backlight_dir);
    FILE *fp = fopen(path, "r");
    if (!fp)
        return -1;
    int raw = -1;
    if (fscanf(fp, "%d", &raw) != 1)
        raw = -1;
    fclose(fp);
    return raw;
}

static void write_raw_backlight_from_pct(int pct) {
    if (!g_backlight_dir[0])
        return;
    pct = clampi(pct, 0, 100);
    int min_raw = 10;
    int raw = clampi(min_raw + (pct * (g_backlight_max - min_raw)) / 100, min_raw, g_backlight_max);
    char path[300];
    snprintf(path, sizeof(path), "%s/brightness", g_backlight_dir);
    FILE *fp = fopen(path, "w");
    if (fp) {
        fprintf(fp, "%d\n", raw);
        fclose(fp);
        g_last_hw_raw = raw;
    }
}

static int read_hardware_brightness_pct(int fallback_pct) {
    /* 1. Check /run/kdm-osd/brightness override file first */
    FILE *ovr = fopen("/run/kdm-osd/brightness", "r");
    if (ovr) {
        int val = -1;
        if (fscanf(ovr, "%d", &val) == 1 && val >= 0) {
            fclose(ovr);
            if (val > 100 && val <= g_backlight_max)
                return raw_to_brightness_pct(val);
            return clampi(val, 0, 100);
        }
        fclose(ovr);
    }
    /* 2. Read from /sys/class/backlight */
    int raw = read_raw_backlight();
    if (raw >= 0) {
        g_last_hw_raw = raw;
        return raw_to_brightness_pct(raw);
    }
    return fallback_pct;
}

static void write_hardware_brightness_pct(int pct) {
    pct = clampi(pct, 0, 100);
    FILE *ovr = fopen("/run/kdm-osd/brightness", "w");
    if (ovr) {
        fprintf(ovr, "%d\n", pct);
        fclose(ovr);
        record_file_mtime("/run/kdm-osd/brightness", &g_brt_mtime);
    }
    write_raw_backlight_from_pct(pct);
}

static int read_volume_pct(int current_pct) {
    FILE *fp = fopen("/run/kdm-osd/volume", "r");
    if (fp) {
        int v = -1;
        if (fscanf(fp, "%d", &v) == 1 && v >= 0) {
            fclose(fp);
            return clampi(v, 0, 100);
        }
        fclose(fp);
    }
    return current_pct;
}

static void write_volume_pct(int pct) {
    pct = clampi(pct, 0, 100);
    FILE *fp = fopen("/run/kdm-osd/volume", "w");
    if (fp) {
        fprintf(fp, "%d\n", pct);
        fclose(fp);
        record_file_mtime("/run/kdm-osd/volume", &g_vol_mtime);
    }
}

static bool poll_external_sliders(osd_state_t *st, uint64_t now) {
    bool vol_trig = false;
    bool brt_trig = false;
    struct stat sb;

    /* 1. Check if /run/kdm-osd/volume was written/touched */
    if (stat("/run/kdm-osd/volume", &sb) == 0 && timespec_changed(&sb.st_mtim, &g_vol_mtime)) {
        g_vol_mtime = sb.st_mtim;
        st->volume_pct = read_volume_pct(st->volume_pct);
        vol_trig = true;
    }

    /* 2. Check if /run/kdm-osd/brightness was written/touched */
    if (stat("/run/kdm-osd/brightness", &sb) == 0 && timespec_changed(&sb.st_mtim, &g_brt_mtime)) {
        g_brt_mtime = sb.st_mtim;
        st->brightness_pct = read_hardware_brightness_pct(st->brightness_pct);
        write_raw_backlight_from_pct(st->brightness_pct);
        brt_trig = true;
    }

    /* 3. Check if hardware /sys/class/backlight/.../brightness changed directly */
    int raw = read_raw_backlight();
    if (raw >= 0 && g_last_hw_raw >= 0 && raw != g_last_hw_raw) {
        g_last_hw_raw = raw;
        st->brightness_pct = raw_to_brightness_pct(raw);
        FILE *ovr = fopen("/run/kdm-osd/brightness", "w");
        if (ovr) {
            fprintf(ovr, "%d\n", st->brightness_pct);
            fclose(ovr);
            record_file_mtime("/run/kdm-osd/brightness", &g_brt_mtime);
        }
        brt_trig = true;
    } else if (raw >= 0 && g_last_hw_raw < 0) {
        g_last_hw_raw = raw;
    }

    if (vol_trig && !brt_trig) {
        st->vol_open = true;
        st->brt_open = false;
        st->sliders_activity_ms = now;
    } else if (brt_trig && !vol_trig) {
        st->brt_open = true;
        st->vol_open = false;
        st->sliders_activity_ms = now;
    } else if (vol_trig && brt_trig) {
        st->vol_open = true;
        st->brt_open = true;
        st->sliders_activity_ms = now;
    }
    return (vol_trig || brt_trig);
}

static void update_clock(const osd_config_t *cfg, osd_state_t *st) {
    if (cfg->mock_time && cfg->mock_time[0]) {
        snprintf(st->time_str, sizeof(st->time_str), "%s", cfg->mock_time);
        return;
    }
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);

    if (cfg->format_24h) {
        snprintf(st->time_str, sizeof(st->time_str), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    } else {
        int h12 = tmv.tm_hour % 12;
        if (h12 == 0)
            h12 = 12;
        const char *ampm = (tmv.tm_hour >= 12) ? "PM" : "AM";
        snprintf(st->time_str, sizeof(st->time_str), "%d:%02d%s", h12, tmv.tm_min, ampm);
    }
}

static void update_wifi(const osd_config_t *cfg, osd_state_t *st) {
    if (cfg->mock_wifi >= 0) {
        st->wifi_connected = (cfg->mock_wifi > 0);
        st->wifi_bars = cfg->mock_wifi > 3 ? 3 : cfg->mock_wifi;
        return;
    }
    FILE *fp = fopen("/proc/net/wireless", "r");
    if (!fp) {
        st->wifi_connected = true;
        st->wifi_bars = 3;
        return;
    }
    char line[256];
    int line_no = 0;
    bool found = false;
    while (fgets(line, sizeof(line), fp)) {
        line_no++;
        if (line_no <= 2)
            continue;
        char iface[32];
        int status = 0;
        float link = 0.0f, level = 0.0f;
        if (sscanf(line, " %31[^:]: %x %f %f", iface, &status, &link, &level) >= 4) {
            found = true;
            st->wifi_connected = true;
            if (link >= 45.0f || (level < 0.0f && level >= -62.0f))
                st->wifi_bars = 3;
            else if (link >= 25.0f || (level < 0.0f && level >= -74.0f))
                st->wifi_bars = 2;
            else
                st->wifi_bars = 1;
            break;
        }
    }
    fclose(fp);
    if (!found) {
        st->wifi_connected = false;
        st->wifi_bars = 0;
    }
}

static void update_battery(const osd_config_t *cfg, osd_state_t *st) {
    FILE *ovr = fopen("/run/kdm-osd/battery", "r");
    if (ovr) {
        int pct = -1;
        char status_buf[32] = {0};
        if (fscanf(ovr, "%d %31s", &pct, status_buf) >= 1 && pct >= 0 && pct <= 100) {
            st->battery_pct = pct;
            st->charging = (strcasecmp(status_buf, "Charging") == 0 || strcmp(status_buf, "1") == 0);
            fclose(ovr);
            return;
        }
        fclose(ovr);
    }
    st->battery_pct = cfg->mock_battery;
    st->charging    = cfg->mock_charging;
}

/* --- Render Transparent Status Bar + Optional iPhone Control Center Sliders --- */

static void compute_overlay_geometry(const osd_config_t *cfg, osd_state_t *st, FT_Face face) {
    float s = cfg->scale;
    int bar_h   = (int)roundf(48.0f * s);
    int pad_x   = (int)roundf(16.0f * s);
    int gap     = (int)roundf(16.0f * s);
    int wifi_sz = (int)roundf(24.0f * s);
    int bat_w   = (int)roundf(38.0f * s);
    int asc = 0, th = 0;
    int max_text_w = measure_text_ft(face, "12:59PM", &asc, &th);

    int bar_w = pad_x * 2 + wifi_sz + gap + bat_w + gap + max_text_w;

    /* Horizontal Volume & Brightness Slider Rows */
    float icon_col_w = roundf(38.0f * s);
    float track_w    = roundf(300.0f * s);
    float row_h      = roundf(46.0f * s);
    float row_gap    = roundf(10.0f * s);
    float panel_w    = icon_col_w * 2.0f + track_w + roundf(18.0f * s);
    float panel_h    = row_h * 2.0f + row_gap + roundf(14.0f * s);

    int surf_w = (int)ceilf(fmaxf((float)bar_w, panel_w + 8.0f));
    surf_w = (surf_w + 7) & ~7; /* 8-pixel alignment */
    int surf_h = bar_h + (int)ceilf(panel_h + 8.0f * s);

    st->surf_w  = surf_w;
    st->surf_h  = surf_h;
    st->bar_h   = bar_h;

    st->panel_w = panel_w;
    st->panel_h = panel_h;
    st->panel_x = (float)surf_w - panel_w - roundf(4.0f * s);
    st->panel_y = (float)bar_h + roundf(2.0f * s);

    /* Horizontal Volume Track Hitbox (Row 1) */
    st->vol_x = st->panel_x + icon_col_w + roundf(8.0f * s);
    st->vol_y = st->panel_y + roundf(4.0f * s);
    st->vol_w = track_w;
    st->vol_h = row_h;

    /* Horizontal Brightness Track Hitbox:
     * Row 1 if only Brightness is open; Row 2 if both Volume & Brightness are open */
    st->brt_x = st->vol_x;
    st->brt_y = st->vol_open ? (st->vol_y + row_h + row_gap) : st->vol_y;
    st->brt_w = track_w;
    st->brt_h = row_h;
}

static surface_t *render_osd_surface(const osd_config_t *cfg, osd_state_t *st, FT_Face face) {
    float s = cfg->scale;
    compute_overlay_geometry(cfg, st, face);

    surface_t *surf = calloc(1, sizeof(surface_t));
    surf->width  = st->surf_w;
    surf->height = st->surf_h;
    surf->pixels = calloc((size_t)surf->width * surf->height, sizeof(uint32_t));
    /* Note: surf->pixels is initialized to 0x00000000 (100% transparent background!) */

    int pad_right = (int)roundf(12.0f * s);
    int gap       = (int)roundf(16.0f * s);
    int wifi_sz   = (int)roundf(24.0f * s);
    int bat_w     = (int)roundf(38.0f * s);
    int bat_h     = (int)roundf(20.0f * s);

    int ascender = 0, text_h = 0;
    int text_w = measure_text_ft(face, st->time_str, &ascender, &text_h);

    /* Right-align [Wi-Fi] [Battery] [Time] inside the top bar */
    float time_x = (float)(surf->width - pad_right - text_w);
    float bat_x  = time_x - gap - bat_w;
    float wifi_x = bat_x - gap - wifi_sz;
    float center_y = st->bar_h * 0.5f;

    /* 1. Wi-Fi Icon (transparent background + subtle drop shadow) */
    draw_wifi_icon(surf, wifi_x, center_y - wifi_sz * 0.5f, (float)wifi_sz,
                   st->wifi_bars, st->wifi_connected);

    /* 2. Battery Icon (transparent background + green fill) */
    draw_battery_icon(surf, bat_x, center_y - bat_h * 0.5f, (float)bat_w, (float)bat_h,
                      st->battery_pct, st->charging);

    /* 3. Clock Text (with subtle dark shadow for readability on light backgrounds) */
    int baseline_y = (int)roundf(center_y + ascender * 0.5f - 1.0f * s);
    draw_text_ft(surf, face, st->time_str, (int)roundf(time_x) + 1, baseline_y + 2, 0, 0, 0, 0.55f);
    draw_text_ft(surf, face, st->time_str, (int)roundf(time_x), baseline_y, 248, 250, 252, 1.0f);

    /* 4. Horizontal Dark-Theme Volume & Brightness Sliders (100% transparent background) */
    if (st->vol_open || st->brt_open) {
        float track_h  = fmaxf(6.0f, roundf(10.0f * s));
        float icon_sz  = roundf(26.0f * s);
        float icon_pad = roundf(22.0f * s);

        if (st->vol_open) {
            /* Volume Horizontal Bar [quiet speaker] [===track + 16 dots===] [loud speaker] */
            float vol_cy = st->vol_y + st->vol_h * 0.42f;
            draw_speaker_endpoint_icon(surf, st->vol_x - icon_pad, vol_cy, icon_sz, false);
            draw_horizontal_slider_track(surf, st->vol_x, vol_cy, st->vol_w, track_h, st->volume_pct, s);
            draw_speaker_endpoint_icon(surf, st->vol_x + st->vol_w + icon_pad, vol_cy, icon_sz, true);
        }

        if (st->brt_open) {
            /* Brightness Horizontal Bar [dim sun] [===track + 16 dots===] [bright sun] */
            float brt_cy = st->brt_y + st->brt_h * 0.42f;
            draw_sun_endpoint_icon(surf, st->brt_x - icon_pad, brt_cy, icon_sz, false);
            draw_horizontal_slider_track(surf, st->brt_x, brt_cy, st->brt_w, track_h, st->brightness_pct, s);
            draw_sun_endpoint_icon(surf, st->brt_x + st->brt_w + icon_pad, brt_cy, icon_sz, true);
        }
    }

    return surf;
}

static void free_surface(surface_t *s) {
    if (!s)
        return;
    free(s->pixels);
    free(s);
}

static bool save_surface_png(const surface_t *s, const char *path, int crop_h) {
    int out_h = (crop_h > 0 && crop_h <= s->height) ? crop_h : s->height;
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
    png_set_IHDR(png, info, s->width, out_h, 8, PNG_COLOR_TYPE_RGBA,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    uint8_t *row = malloc((size_t)s->width * 4);
    for (int y = 0; y < out_h; y++) {
        for (int x = 0; x < s->width; x++) {
            uint32_t p = s->pixels[y * s->width + x];
            row[x * 4 + 0] = (p >> 16) & 0xFF;
            row[x * 4 + 1] = (p >> 8) & 0xFF;
            row[x * 4 + 2] = p & 0xFF;
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

/* --- Touchscreen Input Handler (/dev/input/event* goodix_ts) --- */

typedef struct {
    int fd;
    int max_x;
    int max_y;
    int cur_raw_x;
    int cur_raw_y;
    bool touching;
    bool grabbed;
    int active_drag; /* 0 = none, 1 = gesture from top bar, 2 = brightness slider, 3 = volume slider */
    int start_lx;
    int start_ly;
} touch_input_t;

static void init_touch_input(touch_input_t *ti) {
    memset(ti, 0, sizeof(*ti));
    ti->fd = -1;
    ti->max_x = 1080;
    ti->max_y = 1920;

    DIR *dir = opendir("/dev/input");
    if (!dir)
        return;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "event", 5) != 0)
            continue;
        char path[128];
        snprintf(path, sizeof(path), "/dev/input/%.64s", ent->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        char name[128] = {0};
        ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);
        if (strcasestr(name, "goodix") || strcasestr(name, "touch") || strcasestr(name, "gt9")) {
            struct input_absinfo abs_x = {0}, abs_y = {0};
            if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &abs_x) == 0 && abs_x.maximum > 0)
                ti->max_x = abs_x.maximum;
            else if (ioctl(fd, EVIOCGABS(ABS_X), &abs_x) == 0 && abs_x.maximum > 0)
                ti->max_x = abs_x.maximum;

            if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &abs_y) == 0 && abs_y.maximum > 0)
                ti->max_y = abs_y.maximum;
            else if (ioctl(fd, EVIOCGABS(ABS_Y), &abs_y) == 0 && abs_y.maximum > 0)
                ti->max_y = abs_y.maximum;

            ti->fd = fd;
            printf("kdm-osd: connected touchscreen '%s' (%s, %dx%d)\n",
                   name, path, ti->max_x, ti->max_y);
            break;
        }
        close(fd);
    }
    closedir(dir);
}

static void map_touch_to_landscape(const touch_input_t *ti, const osd_config_t *cfg,
                                   int land_w, int land_h, int *out_lx, int *out_ly) {
    int rx = clampi(ti->cur_raw_x, 0, ti->max_x);
    int ry = clampi(ti->cur_raw_y, 0, ti->max_y);
    if (cfg->rotate == 270) {
        *out_lx = (land_w - 1) - (ry * land_w) / ti->max_y;
        *out_ly = (rx * land_h) / ti->max_x;
    } else if (cfg->rotate == 90) {
        *out_lx = (ry * land_w) / ti->max_y;
        *out_ly = (land_h - 1) - (rx * land_h) / ti->max_x;
    } else if (cfg->rotate == 180) {
        *out_lx = (land_w - 1) - (rx * land_w) / ti->max_x;
        *out_ly = (land_h - 1) - (ry * land_h) / ti->max_y;
    } else {
        *out_lx = (rx * land_w) / ti->max_x;
        *out_ly = (ry * land_h) / ti->max_y;
    }
}

static void set_touch_grab(touch_input_t *ti, bool grab) {
    if (ti->fd < 0 || ti->grabbed == grab)
        return;
    int arg = grab ? 1 : 0;
    if (ioctl(ti->fd, EVIOCGRAB, &arg) == 0)
        ti->grabbed = grab;
}

static bool process_touch_events(touch_input_t *ti, const osd_config_t *cfg, osd_state_t *st,
                                 int land_w, int land_h) {
    if (ti->fd < 0)
        return false;

    bool dirty = false;
    struct input_event ev;
    while (read(ti->fd, &ev, sizeof(ev)) == sizeof(ev)) {
        if (ev.type == EV_ABS) {
            if (ev.code == ABS_MT_POSITION_X || ev.code == ABS_X) {
                ti->cur_raw_x = ev.value;
            } else if (ev.code == ABS_MT_POSITION_Y || ev.code == ABS_Y) {
                ti->cur_raw_y = ev.value;
            } else if (ev.code == ABS_MT_TRACKING_ID) {
                if (ev.value >= 0 && !ti->touching) {
                    ti->touching = true;
                } else if (ev.value == -1 && ti->touching) {
                    ti->touching = false;
                    ti->active_drag = 0;
                    set_touch_grab(ti, false);
                }
            }
        } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
            if (ev.value == 1 && !ti->touching) {
                ti->touching = true;
            } else if (ev.value == 0 && ti->touching) {
                ti->touching = false;
                ti->active_drag = 0;
                set_touch_grab(ti, false);
            }
        } else if (ev.type == EV_SYN && ev.code == SYN_REPORT && ti->touching) {
            int lx = 0, ly = 0;
            map_touch_to_landscape(ti, cfg, land_w, land_h, &lx, &ly);

            /* Local coordinates inside the top-right overlay surface */
            int ox_origin = land_w - st->surf_w - cfg->margin_right;
            int oy_origin = cfg->margin_top;
            float rx = (float)(lx - ox_origin);
            float ry = (float)(ly - oy_origin);

            if (ti->active_drag == 0) {
                ti->start_lx = lx;
                ti->start_ly = ly;

                if (st->vol_open || st->brt_open) {
                    st->sliders_activity_ms = now_ms();
                    /* Check if touch is on Horizontal Brightness Bar row */
                    if (st->brt_open &&
                        rx >= st->brt_x - 36.0f && rx <= st->brt_x + st->brt_w + 36.0f &&
                        ry >= st->brt_y - 6.0f  && ry <= st->brt_y + st->brt_h + 6.0f) {
                        ti->active_drag = 2;
                        set_touch_grab(ti, true);
                    }
                    /* Check if touch is on Horizontal Volume Bar row */
                    else if (st->vol_open &&
                             rx >= st->vol_x - 36.0f && rx <= st->vol_x + st->vol_w + 36.0f &&
                             ry >= st->vol_y - 6.0f  && ry <= st->vol_y + st->vol_h + 6.0f) {
                        ti->active_drag = 3;
                        set_touch_grab(ti, true);
                    }
                    /* Check if touch is in top status bar (toggle close) */
                    else if (rx >= -20.0f && rx <= (float)st->surf_w + 10.0f &&
                             ry >= -10.0f && ry <= (float)st->bar_h) {
                        st->vol_open = false;
                        st->brt_open = false;
                        ti->active_drag = -1;
                        set_touch_grab(ti, false);
                        dirty = true;
                    }
                    /* Touch outside overlay when not in always_sliders mode closes sliders */
                    else if (!cfg->always_sliders) {
                        st->vol_open = false;
                        st->brt_open = false;
                        ti->active_drag = -1;
                        set_touch_grab(ti, false);
                        dirty = true;
                    } else {
                        ti->active_drag = -1;
                    }
                } else {
                    /* Sliders closed: check if touch starts in top-right status bar zone */
                    if (rx >= -20.0f && rx <= (float)st->surf_w + 10.0f &&
                        ry >= -10.0f && ry <= (float)st->bar_h + 24.0f) {
                        ti->active_drag = 1;
                        st->vol_open = true;
                        st->brt_open = true;
                        st->sliders_activity_ms = now_ms();
                        dirty = true;
                    } else {
                        ti->active_drag = -1; /* Ignore touches outside top-right bar */
                    }
                }
            }

            if (ti->active_drag == 2) {
                /* Dragging Horizontal Brightness slider (left = 0%, right = 100%) */
                st->sliders_activity_ms = now_ms();
                int pct = (int)roundf(((rx - st->brt_x) * 100.0f) / st->brt_w);
                pct = clampi(pct, 0, 100);
                if (pct != st->brightness_pct) {
                    st->brightness_pct = pct;
                    write_hardware_brightness_pct(pct);
                    dirty = true;
                }
            } else if (ti->active_drag == 3) {
                /* Dragging Horizontal Volume slider (left = 0%, right = 100%) */
                st->sliders_activity_ms = now_ms();
                int pct = (int)roundf(((rx - st->vol_x) * 100.0f) / st->vol_w);
                pct = clampi(pct, 0, 100);
                if (pct != st->volume_pct) {
                    st->volume_pct = pct;
                    write_volume_pct(pct);
                    dirty = true;
                }
            }
        }
    }
    return dirty;
}

/* --- DRM/KMS Master Borrowing & Hardware Overlay Plane Management --- */

static bool is_drm_master_fd(int fd) {
    errno = 0;
    if (drmAuthMagic(fd, 0) == 0)
        return true;
    return (errno == EINVAL);
}

static int borrow_active_drm_master_fd(int card_minor, pid_t *out_pid) {
    char target_suffix[32];
    snprintf(target_suffix, sizeof(target_suffix), "/dev/dri/card%d", card_minor);

    DIR *proc = opendir("/proc");
    if (!proc)
        return -1;

    pid_t self_pid = getpid();
    struct dirent *pent;
    while ((pent = readdir(proc)) != NULL) {
        if (!isdigit((unsigned char)pent->d_name[0]))
            continue;
        pid_t pid = (pid_t)atoi(pent->d_name);
        if (pid <= 1 || pid == self_pid)
            continue;

        char fd_dir_path[128];
        snprintf(fd_dir_path, sizeof(fd_dir_path), "/proc/%d/fd", pid);
        DIR *fddir = opendir(fd_dir_path);
        if (!fddir)
            continue;

        struct dirent *fent;
        while ((fent = readdir(fddir)) != NULL) {
            if (!isdigit((unsigned char)fent->d_name[0]))
                continue;
            char link_path[300], resolved[256];
            snprintf(link_path, sizeof(link_path), "/proc/%d/fd/%.200s", pid, fent->d_name);
            ssize_t len = readlink(link_path, resolved, sizeof(resolved) - 1);
            if (len <= 0)
                continue;
            resolved[len] = '\0';
            if (strcmp(resolved, target_suffix) != 0)
                continue;

            int target_fd = atoi(fent->d_name);
            int pidfd = (int)syscall(__NR_pidfd_open, pid, 0);
            if (pidfd < 0)
                continue;
            int dup_fd = (int)syscall(__NR_pidfd_getfd, pidfd, target_fd, 0);
            close(pidfd);
            if (dup_fd < 0)
                continue;

            if (is_drm_master_fd(dup_fd)) {
                closedir(fddir);
                closedir(proc);
                if (out_pid)
                    *out_pid = pid;
                return dup_fd;
            }
            close(dup_fd);
        }
        closedir(fddir);
    }
    closedir(proc);
    return -1;
}

typedef struct {
    int fd;
    int card_minor;
    uint32_t crtc_id;
    int crtc_index;
    int crtc_w;
    int crtc_h;
    uint32_t plane_id;

    int buf_w;
    int buf_h;
    uint32_t pitch;
    uint64_t size;
    uint32_t handle;
    int dmabuf_fd;
    uint32_t *map;

    int cached_master_fd;
    pid_t cached_master_pid;
    uint32_t cached_fb_id;
    uint32_t stale_fb_id;
} drm_overlay_t;

static void set_plane_zpos_max(int drm_fd, uint32_t plane_id) {
    drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(drm_fd, plane_id, DRM_MODE_OBJECT_PLANE);
    if (!props)
        return;
    for (uint32_t i = 0; i < props->count_props; i++) {
        drmModePropertyPtr prop = drmModeGetProperty(drm_fd, props->props[i]);
        if (!prop)
            continue;
        if (strcmp(prop->name, "zpos") == 0) {
            uint64_t max_z = 255;
            if ((prop->flags & DRM_MODE_PROP_RANGE) && prop->count_values >= 2)
                max_z = prop->values[1];
            drmModeObjectSetProperty(drm_fd, plane_id, DRM_MODE_OBJECT_PLANE, prop->prop_id, max_z);
        }
        drmModeFreeProperty(prop);
    }
    drmModeFreeObjectProperties(props);
}

static bool init_drm_overlay(drm_overlay_t *ov, const char *explicit_card) {
    memset(ov, 0, sizeof(*ov));
    ov->fd = -1;
    ov->dmabuf_fd = -1;
    ov->cached_master_fd = -1;
    ov->cached_master_pid = -1;

    for (int i = 0; i < 4; i++) {
        char path[64];
        if (explicit_card)
            snprintf(path, sizeof(path), "%s", explicit_card);
        else
            snprintf(path, sizeof(path), "/dev/dri/card%d", i);

        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            if (explicit_card)
                break;
            continue;
        }
        drmDropMaster(fd);
        drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

        drmModeResPtr res = drmModeGetResources(fd);
        if (!res) {
            close(fd);
            if (explicit_card)
                break;
            continue;
        }

        uint32_t found_crtc = 0;
        int found_idx = -1, cw = 0, ch = 0;
        for (int c = 0; c < res->count_crtcs; c++) {
            drmModeCrtcPtr crtc = drmModeGetCrtc(fd, res->crtcs[c]);
            if (crtc && crtc->mode_valid && crtc->width > 0 && crtc->height > 0) {
                found_crtc = crtc->crtc_id;
                found_idx  = c;
                cw = (int)crtc->width;
                ch = (int)crtc->height;
                drmModeFreeCrtc(crtc);
                break;
            }
            if (crtc)
                drmModeFreeCrtc(crtc);
        }

        if (!found_crtc) {
            drmModeFreeResources(res);
            close(fd);
            if (explicit_card)
                break;
            continue;
        }

        drmModePlaneResPtr pres = drmModeGetPlaneResources(fd);
        uint32_t best_plane = 0;
        uint32_t best_stale_fb = 0;
        if (pres) {
            for (uint32_t p = 0; p < pres->count_planes; p++) {
                drmModePlanePtr plane = drmModeGetPlane(fd, pres->planes[p]);
                if (!plane)
                    continue;
                bool crtc_ok = (plane->possible_crtcs & (1u << found_idx)) != 0;
                bool fmt_ok  = false;
                for (uint32_t f = 0; f < plane->count_formats; f++) {
                    if (plane->formats[f] == DRM_FORMAT_ARGB8888) {
                        fmt_ok = true;
                        break;
                    }
                }
                if (crtc_ok && fmt_ok) {
                    drmModeObjectPropertiesPtr props =
                        drmModeObjectGetProperties(fd, plane->plane_id, DRM_MODE_OBJECT_PLANE);
                    if (props) {
                        for (uint32_t k = 0; k < props->count_props; k++) {
                            drmModePropertyPtr prop = drmModeGetProperty(fd, props->props[k]);
                            if (prop) {
                                if (strcmp(prop->name, "type") == 0 &&
                                    props->prop_values[k] == DRM_PLANE_TYPE_OVERLAY) {
                                    best_plane = plane->plane_id;
                                    best_stale_fb = plane->fb_id;
                                }
                                drmModeFreeProperty(prop);
                            }
                        }
                        drmModeFreeObjectProperties(props);
                    }
                }
                drmModeFreePlane(plane);
            }
            drmModeFreePlaneResources(pres);
        }
        drmModeFreeResources(res);

        if (best_plane) {
            ov->fd          = fd;
            ov->card_minor  = i;
            ov->crtc_id     = found_crtc;
            ov->crtc_index  = found_idx;
            ov->crtc_w      = cw;
            ov->crtc_h      = ch;
            ov->plane_id    = best_plane;
            ov->stale_fb_id = best_stale_fb;
            return true;
        }
        close(fd);
        if (explicit_card)
            break;
    }
    return false;
}

static bool allocate_overlay_buffer(drm_overlay_t *ov, int buf_w, int buf_h) {
    struct drm_mode_create_dumb creq = {
        .width  = (uint32_t)buf_w,
        .height = (uint32_t)buf_h,
        .bpp    = 32,
    };
    if (drmIoctl(ov->fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0)
        return false;

    struct drm_mode_map_dumb mreq = { .handle = creq.handle };
    if (drmIoctl(ov->fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0)
        return false;

    void *map = mmap(NULL, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED, ov->fd, mreq.offset);
    if (map == MAP_FAILED)
        return false;

    int dmabuf_fd = -1;
    if (drmPrimeHandleToFD(ov->fd, creq.handle, DRM_CLOEXEC | DRM_RDWR, &dmabuf_fd) < 0)
        return false;

    ov->buf_w     = buf_w;
    ov->buf_h     = buf_h;
    ov->pitch     = creq.pitch;
    ov->size      = creq.size;
    ov->handle    = creq.handle;
    ov->dmabuf_fd = dmabuf_fd;
    ov->map       = (uint32_t *)map;
    memset(ov->map, 0, ov->size);
    return true;
}

static void blit_and_present_overlay(drm_overlay_t *ov, const surface_t *surf, const osd_config_t *cfg) {
    int bw = surf->width;
    int bh = surf->height;
    int crtc_x = 0, crtc_y = 0, crtc_w = ov->buf_w, crtc_h = ov->buf_h;
    uint32_t stride_px = ov->pitch / 4;

    if (cfg->rotate == 270) {
        crtc_x = cfg->margin_top;
        crtc_y = cfg->margin_right;
        for (int y = 0; y < bh; y++) {
            for (int x = 0; x < bw; x++) {
                int dst_x = y;
                int dst_y = (bw - 1) - x;
                ov->map[dst_y * stride_px + dst_x] = surf->pixels[y * bw + x];
            }
        }
    } else if (cfg->rotate == 90) {
        crtc_x = ov->crtc_w - bh - cfg->margin_top;
        crtc_y = ov->crtc_h - ov->buf_h - cfg->margin_right;
        int y_off = ov->buf_h - bw;
        for (int y = 0; y < bh; y++) {
            for (int x = 0; x < bw; x++) {
                int dst_x = (bh - 1) - y;
                int dst_y = y_off + x;
                ov->map[dst_y * stride_px + dst_x] = surf->pixels[y * bw + x];
            }
        }
    } else if (cfg->rotate == 180) {
        crtc_x = cfg->margin_right;
        crtc_y = ov->crtc_h - bh - cfg->margin_top;
        for (int y = 0; y < bh; y++) {
            for (int x = 0; x < bw; x++) {
                int dst_x = (bw - 1) - x;
                int dst_y = (bh - 1) - y;
                ov->map[dst_y * stride_px + dst_x] = surf->pixels[y * bw + x];
            }
        }
    } else {
        crtc_x = ov->crtc_w - ov->buf_w - cfg->margin_right;
        crtc_y = cfg->margin_top;
        int x_off = ov->buf_w - bw;
        for (int y = 0; y < bh; y++) {
            memcpy(&ov->map[y * stride_px + x_off], &surf->pixels[y * bw], (size_t)bw * 4);
        }
    }

    if (ov->cached_master_fd >= 0) {
        if (ov->cached_master_pid <= 0 || kill(ov->cached_master_pid, 0) != 0 ||
            !is_drm_master_fd(ov->cached_master_fd)) {
            close(ov->cached_master_fd);
            ov->cached_master_fd  = -1;
            ov->cached_master_pid = -1;
            ov->cached_fb_id      = 0;
        }
    }

    if (ov->cached_master_fd < 0) {
        pid_t mpid = -1;
        int mfd = borrow_active_drm_master_fd(ov->card_minor, &mpid);
        if (mfd >= 0) {
            ov->cached_master_fd  = mfd;
            ov->cached_master_pid = mpid;
            ov->cached_fb_id      = 0;
        }
    }

    bool borrowed = (ov->cached_master_fd >= 0);
    int use_fd    = borrowed ? ov->cached_master_fd : ov->fd;

    if (!borrowed)
        drmSetMaster(ov->fd);

    if (ov->cached_fb_id == 0) {
        uint32_t imported_handle = ov->handle;
        if (borrowed) {
            if (drmPrimeFDToHandle(use_fd, ov->dmabuf_fd, &imported_handle) < 0) {
                if (!borrowed)
                    drmDropMaster(ov->fd);
                return;
            }
        }
        uint32_t handles[4] = { imported_handle, 0, 0, 0 };
        uint32_t pitches[4] = { ov->pitch, 0, 0, 0 };
        uint32_t offsets[4] = { 0, 0, 0, 0 };
        drmModeAddFB2(use_fd, (uint32_t)ov->buf_w, (uint32_t)ov->buf_h,
                      DRM_FORMAT_ARGB8888, handles, pitches, offsets, &ov->cached_fb_id, 0);
    }

    if (ov->cached_fb_id != 0) {
        set_plane_zpos_max(use_fd, ov->plane_id);
        drmModeSetPlane(use_fd, ov->plane_id, ov->crtc_id, ov->cached_fb_id, 0,
                        crtc_x, crtc_y, (uint32_t)crtc_w, (uint32_t)crtc_h,
                        0, 0, (uint32_t)ov->buf_w << 16, (uint32_t)ov->buf_h << 16);
        if (ov->stale_fb_id != 0 && ov->stale_fb_id != ov->cached_fb_id) {
            drmModeRmFB(use_fd, ov->stale_fb_id);
            ov->stale_fb_id = 0;
        }
    }

    if (!borrowed)
        drmDropMaster(ov->fd);
}

static void disable_overlay_plane(drm_overlay_t *ov) {
    bool borrowed = (ov->cached_master_fd >= 0);
    int use_fd    = borrowed ? ov->cached_master_fd : ov->fd;
    if (!borrowed)
        drmSetMaster(ov->fd);
    drmModeSetPlane(use_fd, ov->plane_id, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    if (ov->cached_fb_id != 0) {
        drmModeRmFB(use_fd, ov->cached_fb_id);
        ov->cached_fb_id = 0;
    }
    if (!borrowed)
        drmDropMaster(ov->fd);
    if (ov->cached_master_fd >= 0) {
        close(ov->cached_master_fd);
        ov->cached_master_fd = -1;
    }
}

static void print_usage(const char *prog) {
    printf("Usage: %s [options]\n"
           "  --rotate <0|90|180|270>  Display rotation (default: 270 for RM67199 landscape)\n"
           "  --scale <float>          UI scale factor (default: 1.15)\n"
           "  --margin-top <px>        Top margin in pixels (default: 4)\n"
           "  --margin-right <px>      Right margin in pixels (default: 8)\n"
           "  --battery <0-100>        Mock battery percentage (default: 75)\n"
           "  --charging               Mock battery charging state\n"
           "  --wifi <0-3>             Override Wi-Fi signal bars (default: live wlan0)\n"
           "  --volume <0-100>         Initial volume percentage (default: 70)\n"
           "  --brightness <0-100>     Initial brightness percentage (default: live DSI backlight)\n"
           "  --time <string>          Override time string (e.g. \"1:14PM\")\n"
           "  --24h                    Use 24-hour time format\n"
           "  --show-sliders           Keep iPhone Control Center sliders open continuously\n"
           "  --preview <out.png>      Render overlay to PNG file and exit\n",
           prog);
}

int main(int argc, char **argv) {
    osd_config_t cfg = {
        .rotate          = 270,
        .scale           = 1.0f,
        .margin_top      = 4,
        .margin_right    = 8,
        .mock_battery    = 75,
        .mock_charging   = false,
        .mock_wifi       = -1,
        .init_volume     = 70,
        .init_brightness = -1,
        .mock_time       = NULL,
        .format_24h      = false,
        .always_sliders  = false,
        .preview_out     = NULL,
        .drm_path        = NULL,
    };

    static const struct option long_opts[] = {
        { "rotate",       required_argument, NULL, 'r' },
        { "scale",        required_argument, NULL, 's' },
        { "margin-top",   required_argument, NULL, 't' },
        { "margin-right", required_argument, NULL, 'm' },
        { "battery",      required_argument, NULL, 'b' },
        { "charging",     no_argument,       NULL, 'c' },
        { "wifi",         required_argument, NULL, 'w' },
        { "volume",       required_argument, NULL, 'v' },
        { "brightness",   required_argument, NULL, 'B' },
        { "time",         required_argument, NULL, 'T' },
        { "24h",          no_argument,       NULL, '2' },
        { "show-sliders", no_argument,       NULL, 'S' },
        { "preview",      required_argument, NULL, 'p' },
        { "card",         required_argument, NULL, 'd' },
        { "help",         no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "r:s:t:m:b:cw:v:B:T:2Sp:d:h", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'r': cfg.rotate = atoi(optarg); break;
            case 's': cfg.scale = strtof(optarg, NULL); break;
            case 't': cfg.margin_top = atoi(optarg); break;
            case 'm': cfg.margin_right = atoi(optarg); break;
            case 'b': cfg.mock_battery = atoi(optarg); break;
            case 'c': cfg.mock_charging = true; break;
            case 'w': cfg.mock_wifi = atoi(optarg); break;
            case 'v': cfg.init_volume = clampi(atoi(optarg), 0, 100); break;
            case 'B': cfg.init_brightness = clampi(atoi(optarg), 0, 100); break;
            case 'T': cfg.mock_time = optarg; break;
            case '2': cfg.format_24h = true; break;
            case 'S': cfg.always_sliders = true; break;
            case 'p': cfg.preview_out = optarg; break;
            case 'd': cfg.drm_path = optarg; break;
            case 'h':
            default:
                print_usage(argv[0]);
                return (opt == 'h') ? 0 : 1;
        }
    }

    discover_backlight();

    FT_Library ft_lib;
    FT_Face ft_face;
    int font_px = (int)roundf(24.0f * cfg.scale);
    if (!init_freetype_face(&ft_lib, &ft_face, font_px)) {
        fprintf(stderr, "kdm-osd: failed to load TrueType font\n");
        return 1;
    }

    osd_state_t st = {0};
    st.volume_pct = read_volume_pct(cfg.init_volume);
    if (cfg.init_brightness >= 0) {
        st.brightness_pct = cfg.init_brightness;
        write_hardware_brightness_pct(st.brightness_pct);
    } else {
        st.brightness_pct = read_hardware_brightness_pct(75);
        write_raw_backlight_from_pct(st.brightness_pct);
    }
    record_file_mtime("/run/kdm-osd/volume", &g_vol_mtime);
    record_file_mtime("/run/kdm-osd/brightness", &g_brt_mtime);
    st.vol_open = cfg.always_sliders;
    st.brt_open = cfg.always_sliders;
    st.sliders_activity_ms = now_ms();

    update_clock(&cfg, &st);
    update_wifi(&cfg, &st);
    update_battery(&cfg, &st);

    if (cfg.preview_out) {
        st.vol_open = cfg.always_sliders;
        st.brt_open = cfg.always_sliders;
        surface_t *surf = render_osd_surface(&cfg, &st, ft_face);
        int crop_h = (st.vol_open || st.brt_open) ? surf->height : st.bar_h;
        bool ok = save_surface_png(surf, cfg.preview_out, crop_h);
        free_surface(surf);
        FT_Done_Face(ft_face);
        FT_Done_FreeType(ft_lib);
        if (!ok) {
            fprintf(stderr, "kdm-osd: failed to write preview PNG to %s\n", cfg.preview_out);
            return 1;
        }
        printf("kdm-osd: preview written to %s\n", cfg.preview_out);
        return 0;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    drm_overlay_t ov;
    if (!init_drm_overlay(&ov, cfg.drm_path)) {
        fprintf(stderr, "kdm-osd: failed to find active DRM CRTC and OVERLAY plane\n");
        return 1;
    }

    compute_overlay_geometry(&cfg, &st, ft_face);
    int drm_buf_w = (cfg.rotate == 90 || cfg.rotate == 270) ? st.surf_h : st.surf_w;
    int drm_buf_h = (cfg.rotate == 90 || cfg.rotate == 270) ? st.surf_w : st.surf_h;

    if (!allocate_overlay_buffer(&ov, drm_buf_w, drm_buf_h)) {
        fprintf(stderr, "kdm-osd: failed to allocate DRM dumb buffer (%dx%d)\n", drm_buf_w, drm_buf_h);
        close(ov.fd);
        return 1;
    }

    touch_input_t ti;
    init_touch_input(&ti);

    int land_w = (cfg.rotate == 90 || cfg.rotate == 270) ? ov.crtc_h : ov.crtc_w;
    int land_h = (cfg.rotate == 90 || cfg.rotate == 270) ? ov.crtc_w : ov.crtc_h;

    printf("kdm-osd: attached to /dev/dri/card%d CRTC %u (%dx%d) overlay plane %u (rotate=%d)\n",
           ov.card_minor, ov.crtc_id, ov.crtc_w, ov.crtc_h, ov.plane_id, cfg.rotate);

    uint64_t last_periodic_ms = 0;
    bool need_redraw = true;

    while (g_running) {
        uint64_t now = now_ms();

        /* Poll touchscreen input with 16ms timeout when sliders are open, or 200ms when closed */
        if (ti.fd >= 0) {
            struct pollfd pfd = { .fd = ti.fd, .events = POLLIN };
            int timeout_ms = (st.vol_open || st.brt_open) ? 16 : 200;
            if (poll(&pfd, 1, timeout_ms) > 0 && (pfd.revents & POLLIN)) {
                if (process_touch_events(&ti, &cfg, &st, land_w, land_h))
                    need_redraw = true;
            }
        } else {
            usleep(200000);
        }

        now = now_ms();

        /* Poll external Volume/Brightness changes at ~5Hz (or 60Hz while open) */
        if (!ti.touching && poll_external_sliders(&st, now)) {
            need_redraw = true;
        }

        /* Auto-hide Control Center sliders after 3 seconds of inactivity */
        if ((st.vol_open || st.brt_open) && !cfg.always_sliders && !ti.touching &&
            (now - st.sliders_activity_ms) >= 3000ULL) {
            st.vol_open = false;
            st.brt_open = false;
            set_touch_grab(&ti, false);
            need_redraw = true;
        }

        /* 1Hz periodic status check (Clock, Wi-Fi, Battery) */
        if (now - last_periodic_ms >= 1000ULL) {
            last_periodic_ms = now;
            char prev_time[32];
            snprintf(prev_time, sizeof(prev_time), "%s", st.time_str);
            int prev_wifi = st.wifi_bars;
            int prev_bat  = st.battery_pct;
            bool prev_chg = st.charging;

            update_clock(&cfg, &st);
            update_wifi(&cfg, &st);
            update_battery(&cfg, &st);

            if (strcmp(prev_time, st.time_str) != 0 ||
                prev_wifi != st.wifi_bars ||
                prev_bat != st.battery_pct ||
                prev_chg != st.charging) {
                need_redraw = true;
            }
        }

        if (need_redraw) {
            surface_t *surf = render_osd_surface(&cfg, &st, ft_face);
            memset(ov.map, 0, ov.size);
            blit_and_present_overlay(&ov, surf, &cfg);
            free_surface(surf);
            need_redraw = false;
        }
    }

    set_touch_grab(&ti, false);
    if (ti.fd >= 0)
        close(ti.fd);
    disable_overlay_plane(&ov);
    munmap(ov.map, ov.size);
    close(ov.dmabuf_fd);
    close(ov.fd);
    FT_Done_Face(ft_face);
    FT_Done_FreeType(ft_lib);
    return 0;
}

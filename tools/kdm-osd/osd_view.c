/*
 * osd_view.c - Implementation of OsdView UI Layout & Composition Class
 *
 * Renders:
 *   1. Compact Top-Right Status Bar ([Wi-Fi] [84%] [Battery] [Time]) when closed
 *   2. Full-Screen (1920x1080) Pull-Down Quick Settings Notification Center Shade
 *      with partial dirty-rect updates for 56Hz zero-lag slider dragging
 *   3. Standalone Volume / Brightness HUD popup when shade is closed
 */
#include "osd_view.h"
#include "osd_icons.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Full-screen shade dark translucent backdrop (ARGB8888: 90% alpha, #18121E) */
#define SHADE_BG_ARGB 0xE618121EU

/*
 * Draw wide pill-style slider matching the full-screen reference UI:
 *   - Dark translucent pill track with subtle outer rim
 *   - Soft slate-teal (#68868A) active fill from left to pct%
 *   - Embedded white icon inside left cap (Speaker for Volume, Sun for Brightness)
 *   - Embedded 3-vertical-dots menu icon inside right cap
 */
static void draw_full_pill_slider(osd_canvas_t *c, float x, float y, float w, float h,
                                  float scale, int pct, bool is_brightness) {
    pct = clampi(pct, 0, 100);
    float rad = h * 0.5f;
    float cy  = y + rad;

    /* 1. Clear slider bounding box to shade backdrop color */
    osd_canvas_fill_rect_raw(c, (int)floorf(x - 4.0f), (int)floorf(y - 4.0f),
                             (int)ceilf(w + 8.0f), (int)ceilf(h + 8.0f), SHADE_BG_ARGB);

    /* 2. Unfilled dark translucent pill track */
    osd_canvas_fill_rrect(c, x, y, w, h, rad, 42, 38, 48, 0.94f);

    /* 3. Filled active bar (#68868A slate-teal matching reference screenshot) */
    if (pct > 0) {
        float fill_w = (w * (float)pct) / 100.0f;
        if (fill_w < h)
            fill_w = h;
        osd_canvas_fill_rrect(c, x, y, fill_w, h, rad, 104, 134, 138, 0.98f);
    }

    /* 4. Subtle outer border highlight */
    osd_canvas_stroke_rrect(c, x, y, w, h, rad, 1.3f, 255, 255, 255, 0.16f);

    /* 5. Left icon inside the pill cap */
    float left_icon_cx = x + roundf(36.0f * scale);
    float icon_sz      = roundf(30.0f * scale);
    if (is_brightness) {
        osd_icons_draw_sun(c, left_icon_cx, cy, icon_sz, true, 255, 255, 255, 0.98f);
    } else {
        osd_icons_draw_speaker(c, left_icon_cx, cy, icon_sz, pct > 0, 255, 255, 255, 0.98f);
    }

    /* 6. Right 3-dots menu icon inside the pill cap */
    float right_dots_cx = x + w - roundf(32.0f * scale);
    osd_icons_draw_dots_v(c, right_dots_cx, cy, roundf(22.0f * scale), 255, 255, 255, 0.92f);
}

/*
 * Draw standalone transparent HUD slider (with 16 step dots underneath)
 * used when Volume or Brightness is changed externally while Quick Settings shade is closed.
 */
static void draw_hud_slider_track(osd_canvas_t *c, float track_x, float track_cy,
                                  float track_w, float track_h, int pct, float scale) {
    pct = clampi(pct, 0, 100);
    float radius  = track_h * 0.5f;
    float track_y = track_cy - radius;

    osd_canvas_fill_rrect(c, track_x, track_y + 2.0f, track_w, track_h, radius, 0, 0, 0, 0.45f);
    osd_canvas_fill_rrect(c, track_x, track_y, track_w, track_h, radius, 66, 70, 78, 0.88f);

    if (pct > 0) {
        float fill_w = (track_w * (float)pct) / 100.0f;
        if (fill_w < track_h)
            fill_w = track_h;
        osd_canvas_fill_rrect(c, track_x, track_y, fill_w, track_h, radius, 250, 252, 255, 0.98f);
    }

    const int num_dots = 16;
    float dots_y  = track_cy + radius + roundf(8.0f * scale);
    float dot_r   = fmaxf(1.5f, 1.6f * scale);
    float start_x = track_x + radius + 2.0f * scale;
    float end_x   = track_x + track_w - radius - 2.0f * scale;

    for (int i = 0; i < num_dots; i++) {
        float dx = start_x + (end_x - start_x) * ((float)i / (float)(num_dots - 1));
        osd_canvas_fill_circle(c, dx + 0.8f, dots_y + 1.2f, dot_r, 0, 0, 0, 0.38f);
        osd_canvas_fill_circle(c, dx, dots_y, dot_r, 128, 134, 144, 0.72f);
    }
}

/*
 * Draw the 7 Circular Quick-Toggle Buttons + Centered Labels underneath
 */
static void draw_qs_buttons_row(osd_canvas_t *c, const osd_config_t *cfg,
                                const osd_state_t *st, const osd_font_t *font) {
    float s = cfg->scale;
    int row_y0 = (int)floorf(st->qs_cy - st->qs_r - 6.0f);
    int row_h  = (int)ceilf(st->qs_r * 2.0f + 78.0f * s);
    osd_canvas_fill_rect_raw(c, (int)floorf(st->panel_x - 20.0f), row_y0,
                             (int)ceilf(st->panel_w + 40.0f), row_h, SHADE_BG_ARGB);

    for (int i = 0; i < OSD_QS_COUNT; i++) {
        osd_icons_draw_qs_button(c, st->qs_cx[i], st->qs_cy, st->qs_r,
                                 (osd_qs_id_t)i, st->qs_active[i]);

        int asc = 0, th = 0;
        if (i == OSD_QS_DATA) {
            /* Two-line label "Mobile" / "data" matching reference screenshot */
            int w1 = osd_font_measure(font->face_label, "Mobile", &asc, &th);
            int w2 = osd_font_measure(font->face_label, "data", &asc, &th);
            int by1 = (int)roundf(st->qs_cy + st->qs_r + 28.0f * s);
            int by2 = by1 + (int)roundf(23.0f * s);
            osd_font_draw_text(c, font->face_label, "Mobile",
                               (int)roundf(st->qs_cx[i] - w1 * 0.5f), by1,
                               230, 232, 238, 0.95f);
            osd_font_draw_text(c, font->face_label, "data",
                               (int)roundf(st->qs_cx[i] - w2 * 0.5f), by2,
                               230, 232, 238, 0.95f);
        } else {
            const char *lbl = osd_icons_get_qs_label((osd_qs_id_t)i);
            int lw = osd_font_measure(font->face_label, lbl, &asc, &th);
            int by = (int)roundf(st->qs_cy + st->qs_r + 28.0f * s);
            osd_font_draw_text(c, font->face_label, lbl,
                               (int)roundf(st->qs_cx[i] - lw * 0.5f), by,
                               230, 232, 238, 0.95f);
        }
    }
}

void osd_view_compute_layout(const osd_config_t *cfg, osd_state_t *st, const osd_font_t *font) {
    float s = cfg->scale;
    int bar_h   = (int)roundf(48.0f * s);
    int pad_x   = (int)roundf(16.0f * s);
    int gap     = (int)roundf(16.0f * s);
    int wifi_sz = (int)roundf(24.0f * s);
    int bat_w   = (int)roundf(38.0f * s);
    int asc = 0, th = 0;
    int max_text_w = osd_font_measure(font->face_bar, "12:59PM", &asc, &th);
    int bar_w = pad_x * 2 + wifi_sz + gap + bat_w + gap + max_text_w + 16;
    bar_w = (bar_w + 7) & ~7;

    st->surf_w = OSD_SCREEN_W;
    st->surf_h = OSD_SCREEN_H;
    st->bar_w  = bar_w;
    st->bar_h  = bar_h;

    /* Centered 1260px Content Container inside 1920x1080 Full-Screen Shade */
    st->panel_w = roundf(1260.0f * s);
    st->panel_h = (float)OSD_SCREEN_H;
    st->panel_x = ((float)OSD_SCREEN_W - st->panel_w) * 0.5f;
    st->panel_y = 0.0f;

    /* Header Settings Gear icon position */
    st->gear_cx = st->panel_x + st->panel_w - roundf(84.0f * s);
    st->gear_cy = roundf(112.0f * s);
    st->gear_r  = roundf(22.0f * s);

    /* Two Wide Pill Buttons ("Device control" & "Media output") */
    float pill_gap = roundf(32.0f * s);
    st->pill_w    = (st->panel_w - pill_gap) * 0.5f;
    st->pill_h    = roundf(86.0f * s);
    st->pill_y    = roundf(180.0f * s);
    st->pill_x[0] = st->panel_x;
    st->pill_x[1] = st->panel_x + st->pill_w + pill_gap;

    /* 7 Circular Quick-Toggle Buttons across Center Row */
    st->qs_r  = roundf(46.0f * s);
    st->qs_cy = roundf(476.0f * s);
    float left_cx  = st->panel_x + roundf(56.0f * s);
    float right_cx = st->panel_x + st->panel_w - roundf(56.0f * s);
    for (int i = 0; i < OSD_QS_COUNT; i++) {
        st->qs_cx[i] = left_cx + (right_cx - left_cx) * ((float)i / (float)(OSD_QS_COUNT - 1));
    }

    if (st->shade_open || st->shade_pull_px > 0.5f) {
        /* Full-Width Pill Sliders inside Quick Settings Shade */
        float slider_h = roundf(62.0f * s);
        st->vol_x = st->panel_x;
        st->vol_y = roundf(786.0f * s);
        st->vol_w = st->panel_w;
        st->vol_h = slider_h;

        st->brt_x = st->panel_x;
        st->brt_y = roundf(874.0f * s);
        st->brt_w = st->panel_w;
        st->brt_h = slider_h;
    } else {
        /* Standalone Top-Right HUD popup slider geometry (when shade is closed) */
        float icon_col_w = roundf(38.0f * s);
        float track_w    = roundf(300.0f * s);
        float row_h      = roundf(46.0f * s);
        float row_gap    = roundf(10.0f * s);
        float hud_w      = icon_col_w * 2.0f + track_w + roundf(24.0f * s);
        int hud_wi       = ((int)ceilf(hud_w) + 7) & ~7;
        st->hud_w        = hud_wi > st->bar_w ? hud_wi : st->bar_w;

        float hud_x = (float)OSD_SCREEN_W - (float)cfg->margin_right - hud_w;
        st->vol_x = hud_x + icon_col_w + roundf(8.0f * s);
        st->vol_y = (float)(cfg->margin_top + bar_h) + roundf(6.0f * s);
        st->vol_w = track_w;
        st->vol_h = row_h;

        st->brt_x = st->vol_x;
        st->brt_y = st->vol_open ? (st->vol_y + row_h + row_gap) : st->vol_y;
        st->brt_w = track_w;
        st->brt_h = row_h;

        int active_rows = (st->vol_open ? 1 : 0) + (st->brt_open ? 1 : 0);
        st->hud_h = cfg->margin_top + bar_h +
                    (active_rows > 0 ? (int)ceilf(active_rows * (row_h + row_gap) + 12.0f * s) : 0);
    }
}

void osd_view_render_into(osd_canvas_t *c, const osd_config_t *cfg,
                          osd_state_t *st, const osd_font_t *font,
                          osd_rect_t *out_dirty) {
    float s = cfg->scale;
    osd_view_compute_layout(cfg, st, font);

    /* --- Case A: Full-Screen Quick Settings Shade is Open or Pulling --- */
    if (st->shade_open || st->shade_pull_px > 0.5f) {
        /* Fast Partial Dirty-Rect Updates when dragging sliders or tapping toggles */
        if (st->shade_rendered && st->dirty_kind == OSD_DIRTY_BRT_SLIDER) {
            draw_full_pill_slider(c, st->brt_x, st->brt_y, st->brt_w, st->brt_h,
                                  s, st->brightness_pct, true);
            if (out_dirty) {
                out_dirty->x = (int)floorf(st->brt_x - 4.0f);
                out_dirty->y = (int)floorf(st->brt_y - 4.0f);
                out_dirty->w = (int)ceilf(st->brt_w + 8.0f);
                out_dirty->h = (int)ceilf(st->brt_h + 8.0f);
            }
            return;
        }
        if (st->shade_rendered && st->dirty_kind == OSD_DIRTY_VOL_SLIDER) {
            draw_full_pill_slider(c, st->vol_x, st->vol_y, st->vol_w, st->vol_h,
                                  s, st->volume_pct, false);
            if (out_dirty) {
                out_dirty->x = (int)floorf(st->vol_x - 4.0f);
                out_dirty->y = (int)floorf(st->vol_y - 4.0f);
                out_dirty->w = (int)ceilf(st->vol_w + 8.0f);
                out_dirty->h = (int)ceilf(st->vol_h + 8.0f);
            }
            return;
        }
        if (st->shade_rendered && st->dirty_kind == OSD_DIRTY_QS_ROW) {
            draw_qs_buttons_row(c, cfg, st, font);
            draw_full_pill_slider(c, st->vol_x, st->vol_y, st->vol_w, st->vol_h,
                                  s, st->volume_pct, false);
            if (out_dirty) {
                int y0 = (int)floorf(st->qs_cy - st->qs_r - 6.0f);
                int y1 = (int)ceilf(st->vol_y + st->vol_h + 6.0f);
                out_dirty->x = (int)floorf(st->panel_x - 20.0f);
                out_dirty->y = y0;
                out_dirty->w = (int)ceilf(st->panel_w + 40.0f);
                out_dirty->h = y1 - y0;
            }
            return;
        }

        /* Full 1920x1080 Shade Render */
        osd_canvas_fill_rect_raw(c, 0, 0, c->width, c->height, SHADE_BG_ARGB);

        /* 1. Top Status Row inside Shade: [Time] on left, [Wi-Fi] [84%] [Battery] on right */
        float top_cy  = roundf(36.0f * s);
        int asc = 0, th = 0;
        osd_font_measure(font->face_bar, st->time_str, &asc, &th);
        int time_by = (int)roundf(top_cy + asc * 0.45f);
        osd_font_draw_text(c, font->face_bar, st->time_str,
                           (int)roundf(st->panel_x), time_by, 248, 250, 252, 0.98f);

        float bat_w   = roundf(36.0f * s);
        float bat_h   = roundf(19.0f * s);
        float bat_x   = st->panel_x + st->panel_w - bat_w;
        osd_icons_draw_battery(c, bat_x, top_cy - bat_h * 0.5f, bat_w, bat_h,
                               st->battery_pct, st->charging);

        char pct_str[16];
        snprintf(pct_str, sizeof(pct_str), "%d%%", st->battery_pct);
        int pct_w = osd_font_measure(font->face_bar, pct_str, &asc, &th);
        float pct_x = bat_x - roundf(10.0f * s) - (float)pct_w;
        osd_font_draw_text(c, font->face_bar, pct_str,
                           (int)roundf(pct_x), time_by, 248, 250, 252, 0.98f);

        float wifi_sz = roundf(22.0f * s);
        float wifi_x  = pct_x - roundf(14.0f * s) - wifi_sz;
        osd_icons_draw_wifi(c, wifi_x, top_cy - wifi_sz * 0.5f, wifi_sz,
                            st->wifi_bars, st->wifi_connected);

        /* 2. Header Row: Date ("Fri, 24 Mar") on left + [Search] [Power] [Gear] [3-Dots] on right */
        int date_asc = 0, date_h = 0;
        osd_font_measure(font->face_date, st->date_str, &date_asc, &date_h);
        int date_by = (int)roundf(st->gear_cy + date_asc * 0.45f);
        osd_font_draw_text(c, font->face_date, st->date_str,
                           (int)roundf(st->panel_x), date_by, 248, 250, 252, 1.0f);

        float h_icon_sz = roundf(26.0f * s);
        float dots_cx   = st->panel_x + st->panel_w - roundf(12.0f * s);
        float gear_cx   = st->gear_cx;
        float power_cx  = gear_cx - roundf(76.0f * s);
        float search_cx = power_cx - roundf(76.0f * s);

        osd_icons_draw_search(c, search_cx, st->gear_cy, h_icon_sz, 248, 250, 252, 0.98f);
        osd_icons_draw_power(c,  power_cx,  st->gear_cy, h_icon_sz, 248, 250, 252, 0.98f);
        osd_icons_draw_gear(c,   gear_cx,   st->gear_cy, h_icon_sz, 248, 250, 252, 0.98f);
        osd_icons_draw_dots_v(c, dots_cx,   st->gear_cy, h_icon_sz, 248, 250, 252, 0.98f);

        /* 3. Two Wide Pill Buttons ("Device control" & "Media output") */
        static const char *const pill_titles[2] = { "Device control", "Media output" };
        float pill_rad = st->pill_h * 0.5f;
        for (int p = 0; p < 2; p++) {
            bool act = st->pill_active[p];
            uint8_t pr = act ? 104 : 62;
            uint8_t pg = act ? 134 : 54;
            uint8_t pb = act ? 138 : 72;
            osd_canvas_fill_rrect(c, st->pill_x[p], st->pill_y, st->pill_w, st->pill_h,
                                  pill_rad, pr, pg, pb, 0.88f);
            osd_canvas_stroke_rrect(c, st->pill_x[p], st->pill_y, st->pill_w, st->pill_h,
                                    pill_rad, 1.2f, 255, 255, 255, 0.10f);
            int tw = osd_font_measure(font->face_bar, pill_titles[p], &asc, &th);
            int tx = (int)roundf(st->pill_x[p] + (st->pill_w - tw) * 0.5f);
            int ty = (int)roundf(st->pill_y + st->pill_h * 0.5f + asc * 0.45f);
            osd_font_draw_text(c, font->face_bar, pill_titles[p], tx, ty, 248, 250, 252, 0.98f);
        }

        /* 4. 7 Circular Quick-Toggle Buttons + Labels */
        draw_qs_buttons_row(c, cfg, st, font);

        /* 5. Page Indicator Dots (6 dots centered at x = 960, y = 736) */
        float dots_cy  = roundf(736.0f * s);
        float dot_step = roundf(18.0f * s);
        float dots_x0  = (float)OSD_SCREEN_W * 0.5f - dot_step * 2.5f;
        for (int d = 0; d < 6; d++) {
            float dx = dots_x0 + d * dot_step;
            osd_canvas_fill_circle(c, dx, dots_cy, roundf(3.8f * s),
                                   255, 255, 255, (d == 0) ? 0.95f : 0.35f);
        }

        /* 6. Full-Width Volume Pill Slider & Brightness Pill Slider */
        draw_full_pill_slider(c, st->vol_x, st->vol_y, st->vol_w, st->vol_h,
                              s, st->volume_pct, false);
        draw_full_pill_slider(c, st->brt_x, st->brt_y, st->brt_w, st->brt_h,
                              s, st->brightness_pct, true);

        /* 7. Bottom White Swipe-Up / Home Handle Bar */
        float grab_w = roundf(340.0f * s);
        float grab_h = fmaxf(5.0f, roundf(6.5f * s));
        osd_canvas_fill_rrect(c,
                              ((float)OSD_SCREEN_W - grab_w) * 0.5f,
                              (float)OSD_SCREEN_H - roundf(32.0f * s),
                              grab_w, grab_h, grab_h * 0.5f,
                              255, 255, 255, 0.96f);

        if (out_dirty) {
            out_dirty->x = 0;
            out_dirty->y = 0;
            out_dirty->w = c->width;
            out_dirty->h = c->height;
        }
        return;
    }

    /* --- Case B: Shade is Closed -> Render Only Compact Top-Right Bar (+ Optional HUD) --- */
    int active_w = (st->vol_open || st->brt_open) ? st->hud_w : st->bar_w;
    int active_h = (st->vol_open || st->brt_open) ? st->hud_h : (cfg->margin_top + st->bar_h);
    int box_x    = OSD_SCREEN_W - active_w;

    osd_canvas_fill_rect_raw(c, box_x, 0, active_w, active_h, 0x00000000U);

    int pad_right = cfg->margin_right + (int)roundf(12.0f * s);
    int gap       = (int)roundf(16.0f * s);
    int wifi_sz   = (int)roundf(24.0f * s);
    int bat_w     = (int)roundf(38.0f * s);
    int bat_h     = (int)roundf(20.0f * s);

    int ascender = 0, text_h = 0;
    int text_w = osd_font_measure(font->face_bar, st->time_str, &ascender, &text_h);

    float time_x   = (float)(OSD_SCREEN_W - pad_right - text_w);
    float bat_x    = time_x - gap - bat_w;
    float wifi_x   = bat_x - gap - wifi_sz;
    float center_y = (float)cfg->margin_top + st->bar_h * 0.5f;

    osd_icons_draw_wifi(c, wifi_x, center_y - wifi_sz * 0.5f, (float)wifi_sz,
                        st->wifi_bars, st->wifi_connected);
    osd_icons_draw_battery(c, bat_x, center_y - bat_h * 0.5f, (float)bat_w, (float)bat_h,
                           st->battery_pct, st->charging);

    int baseline_y = (int)roundf(center_y + ascender * 0.5f - 1.0f * s);
    osd_font_draw_text(c, font->face_bar, st->time_str,
                       (int)roundf(time_x) + 1, baseline_y + 2, 0, 0, 0, 0.55f);
    osd_font_draw_text(c, font->face_bar, st->time_str,
                       (int)roundf(time_x), baseline_y, 248, 250, 252, 1.0f);

    if (st->vol_open || st->brt_open) {
        float track_h  = fmaxf(6.0f, roundf(10.0f * s));
        float icon_sz  = roundf(26.0f * s);
        float icon_pad = roundf(22.0f * s);

        if (st->vol_open) {
            float vol_cy = st->vol_y + st->vol_h * 0.42f;
            osd_icons_draw_speaker(c, st->vol_x - icon_pad + 1.5f, vol_cy + 2.0f, icon_sz, false, 0, 0, 0, 0.50f);
            osd_icons_draw_speaker(c, st->vol_x - icon_pad, vol_cy, icon_sz, false, 246, 248, 252, 0.96f);
            draw_hud_slider_track(c, st->vol_x, vol_cy, st->vol_w, track_h, st->volume_pct, s);
            osd_icons_draw_speaker(c, st->vol_x + st->vol_w + icon_pad + 1.5f, vol_cy + 2.0f, icon_sz, true, 0, 0, 0, 0.50f);
            osd_icons_draw_speaker(c, st->vol_x + st->vol_w + icon_pad, vol_cy, icon_sz, true, 246, 248, 252, 0.96f);
        }

        if (st->brt_open) {
            float brt_cy = st->brt_y + st->brt_h * 0.42f;
            osd_icons_draw_sun(c, st->brt_x - icon_pad + 1.5f, brt_cy + 2.0f, icon_sz, false, 0, 0, 0, 0.50f);
            osd_icons_draw_sun(c, st->brt_x - icon_pad, brt_cy, icon_sz, false, 246, 248, 252, 0.96f);
            draw_hud_slider_track(c, st->brt_x, brt_cy, st->brt_w, track_h, st->brightness_pct, s);
            osd_icons_draw_sun(c, st->brt_x + st->brt_w + icon_pad + 1.5f, brt_cy + 2.0f, icon_sz, true, 0, 0, 0, 0.50f);
            osd_icons_draw_sun(c, st->brt_x + st->brt_w + icon_pad, brt_cy, icon_sz, true, 246, 248, 252, 0.96f);
        }
    }

    if (out_dirty) {
        out_dirty->x = box_x;
        out_dirty->y = 0;
        out_dirty->w = active_w;
        out_dirty->h = active_h;
    }
}

osd_canvas_t *osd_view_render(const osd_config_t *cfg, osd_state_t *st, const osd_font_t *font) {
    osd_view_compute_layout(cfg, st, font);
    osd_canvas_t *c = osd_canvas_create(st->surf_w, st->surf_h);
    if (!c)
        return NULL;
    st->shade_rendered = false;
    st->dirty_kind     = OSD_DIRTY_FULL;
    osd_view_render_into(c, cfg, st, font, NULL);
    return c;
}

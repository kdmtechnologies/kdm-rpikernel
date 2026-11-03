/*
 * kdm_osd.c - Main Application Controller (OsdApp) for KDM Direct DRM/KMS Overlay
 *
 * Modular Object-Oriented (OOP) Architecture:
 *   - osd_types.h   : Domain models (osd_config_t, osd_state_t, osd_qs_id_t, osd_dirty_kind_t)
 *   - osd_canvas.c  : OsdCanvas (ARGB8888 2D SDF surface + PNG export) & OsdFont (FreeType)
 *   - osd_icons.c   : OsdIcons (Anti-aliased vector icon renderer + Quick Settings buttons)
 *   - osd_view.c    : OsdView (Top-right status bar + 1920x1080 Full-Screen Shade + HUD)
 *   - osd_hw.c      : OsdHardware (Backlight sysfs, /run/kdm-osd/ IPC, Wi-Fi, Battery, Clock)
 *   - osd_touch.c   : OsdTouchController (Finger-following swipe-down/up, buttons, sliders)
 *   - osd_drm.c     : OsdDrmPresenter (DRM overlay plane, zero-copy swipe crop, dirty-rect blit)
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <getopt.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "osd_types.h"
#include "osd_canvas.h"
#include "osd_icons.h"
#include "osd_view.h"
#include "osd_hw.h"
#include "osd_touch.h"
#include "osd_drm.h"

static volatile sig_atomic_t g_running = 1;

static void handle_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static void print_usage(const char *prog) {
    printf("Usage: %s [options]\n"
           "  --rotate <0|90|180|270>  Display rotation (default: 270 for RM67199 landscape)\n"
           "  --scale <float>          UI scale factor (default: 1.0)\n"
           "  --margin-top <px>        Top margin in pixels (default: 4)\n"
           "  --margin-right <px>      Right margin in pixels (default: 8)\n"
           "  --battery <0-100>        Mock battery percentage (default: 75)\n"
           "  --charging               Mock battery charging state\n"
           "  --wifi <0-3>             Override Wi-Fi signal bars (default: live wlan0)\n"
           "  --volume <0-100>         Initial volume percentage (default: 70)\n"
           "  --brightness <0-100>     Initial brightness percentage (default: live DSI backlight)\n"
           "  --time <string>          Override time string (e.g. \"1:14PM\")\n"
           "  --24h                    Use 24-hour time format\n"
           "  --show-sliders           Keep Quick Settings shade open continuously\n"
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

    /* 1. Initialize Font Engine (24px bar, 30px date header, 19px toggle labels) */
    osd_font_t font;
    if (!osd_font_init(&font, cfg.scale)) {
        fprintf(stderr, "kdm-osd: failed to load TrueType font\n");
        return 1;
    }

    /* 2. Initialize Hardware & Domain State */
    osd_state_t st = {0};
    osd_hw_t hw;
    osd_hw_init(&hw, &cfg, &st);

    /* 3. Optional Offline PNG Preview Mode */
    if (cfg.preview_out) {
        st.shade_open    = cfg.always_sliders;
        st.shade_pull_px = st.shade_open ? (float)OSD_SCREEN_H : 0.0f;
        osd_canvas_t *preview = osd_view_render(&cfg, &st, &font);
        int crop_h = (st.shade_open || st.vol_open || st.brt_open) ? preview->height : st.bar_h;
        bool ok = osd_canvas_save_png(preview, cfg.preview_out, crop_h);
        osd_canvas_destroy(preview);
        osd_font_destroy(&font);
        if (!ok) {
            fprintf(stderr, "kdm-osd: failed to write preview PNG to %s\n", cfg.preview_out);
            return 1;
        }
        printf("kdm-osd: preview written to %s\n", cfg.preview_out);
        return 0;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    /* 4. Compute Layout & Allocate Persistent 1920x1080 Canvas + DRM Overlay Plane */
    osd_view_compute_layout(&cfg, &st, &font);
    osd_canvas_t *canvas = osd_canvas_create(st.surf_w, st.surf_h);
    if (!canvas) {
        fprintf(stderr, "kdm-osd: failed to allocate %dx%d canvas\n", st.surf_w, st.surf_h);
        osd_font_destroy(&font);
        return 1;
    }

    int drm_buf_w = (cfg.rotate == 90 || cfg.rotate == 270) ? st.surf_h : st.surf_w;
    int drm_buf_h = (cfg.rotate == 90 || cfg.rotate == 270) ? st.surf_w : st.surf_h;

    osd_drm_t drm;
    if (!osd_drm_init(&drm, cfg.drm_path, drm_buf_w, drm_buf_h)) {
        fprintf(stderr, "kdm-osd: failed to initialize DRM overlay plane (%dx%d)\n",
                drm_buf_w, drm_buf_h);
        osd_canvas_destroy(canvas);
        osd_font_destroy(&font);
        return 1;
    }

    /* 5. Initialize Touchscreen Controller */
    osd_touch_t touch;
    osd_touch_init(&touch);

    int land_w = (cfg.rotate == 90 || cfg.rotate == 270) ? drm.crtc_h : drm.crtc_w;
    int land_h = (cfg.rotate == 90 || cfg.rotate == 270) ? drm.crtc_w : drm.crtc_h;

    printf("kdm-osd: attached to /dev/dri/card%d CRTC %u (%dx%d) overlay plane %u (rotate=%d)\n",
           drm.card_minor, drm.crtc_id, drm.crtc_w, drm.crtc_h, drm.plane_id, cfg.rotate);

    uint64_t last_periodic_ms = 0;
    bool need_redraw = true;

    /* 6. Main Event Loop */
    while (g_running) {
        if (touch.fd >= 0) {
            struct pollfd pfd = { .fd = touch.fd, .events = POLLIN };
            int timeout_ms = (st.shade_animating || touch.touching ||
                              st.shade_open || st.vol_open || st.brt_open) ? 16 : 150;
            if (poll(&pfd, 1, timeout_ms) > 0 && (pfd.revents & POLLIN)) {
                if (osd_touch_process(&touch, &cfg, &st, &hw, land_w, land_h))
                    need_redraw = true;
            }
        } else {
            usleep(16000);
        }

        uint64_t now = osd_now_ms();

        /* Step smooth 56Hz ease-out swipe animation when finger is released */
        if (st.shade_animating && !touch.touching) {
            float diff = st.shade_target_px - st.shade_pull_px;
            float step = diff * 0.28f;
            if (fabsf(step) < 22.0f)
                step = (diff > 0.0f) ? fminf(diff, 22.0f) : fmaxf(diff, -22.0f);
            st.shade_pull_px = clampf(st.shade_pull_px + step, 0.0f, (float)land_h);

            if (fabsf(st.shade_target_px - st.shade_pull_px) <= 1.5f) {
                st.shade_pull_px   = st.shade_target_px;
                st.shade_animating = false;
                if (st.shade_pull_px <= 0.5f) {
                    st.shade_open     = false;
                    st.shade_rendered = false;
                    st.dirty_kind     = OSD_DIRTY_FULL;
                    osd_touch_set_grab(&touch, false);
                } else {
                    st.shade_open = true;
                    if (st.dirty_kind == OSD_DIRTY_NONE)
                        st.dirty_kind = st.shade_rendered ? OSD_DIRTY_SHADE_PULL : OSD_DIRTY_FULL;
                }
            } else {
                if (st.dirty_kind == OSD_DIRTY_NONE)
                    st.dirty_kind = st.shade_rendered ? OSD_DIRTY_SHADE_PULL : OSD_DIRTY_FULL;
            }
            need_redraw = true;
        }

        /* Poll external /run/kdm-osd/{volume,brightness} or sysfs backlight changes */
        if (!touch.touching && osd_hw_poll_external_sliders(&hw, &st, now)) {
            need_redraw = true;
        }

        /* Auto-hide single-bar HUD popups after 3s, or animate Quick Settings shade closed after 15s */
        if (!cfg.always_sliders && !touch.touching && !st.shade_animating) {
            if ((st.vol_open || st.brt_open) && (now - st.sliders_activity_ms) >= 3000ULL) {
                st.vol_open   = false;
                st.brt_open   = false;
                st.dirty_kind = OSD_DIRTY_FULL;
                osd_touch_set_grab(&touch, false);
                need_redraw = true;
            } else if (st.shade_open && (now - st.sliders_activity_ms) >= 15000ULL) {
                st.shade_target_px = 0.0f;
                st.shade_animating = true;
                need_redraw = true;
            }
        }

        /* 1Hz periodic status check (Clock, Date, Wi-Fi, Battery) */
        if (!st.shade_animating && !touch.touching && (now - last_periodic_ms >= 1000ULL)) {
            last_periodic_ms = now;
            char prev_time[32];
            snprintf(prev_time, sizeof(prev_time), "%s", st.time_str);
            int prev_wifi = st.wifi_bars;
            int prev_bat  = st.battery_pct;
            bool prev_chg = st.charging;

            osd_hw_update_status(&hw, &cfg, &st);

            if (strcmp(prev_time, st.time_str) != 0 ||
                prev_wifi != st.wifi_bars ||
                prev_bat != st.battery_pct ||
                prev_chg != st.charging) {
                st.dirty_kind = OSD_DIRTY_FULL;
                need_redraw = true;
            }
        }

        if (need_redraw) {
            osd_rect_t dirty_rect = {0};
            if (st.dirty_kind == OSD_DIRTY_SHADE_PULL && st.shade_rendered) {
                /* Zero-copy hardware plane crop update only (0.00ms pixel copy!) */
                osd_drm_present(&drm, canvas, &cfg, &st, NULL);
            } else {
                osd_view_render_into(canvas, &cfg, &st, &font, &dirty_rect);
                osd_drm_present(&drm, canvas, &cfg, &st, &dirty_rect);
            }
            st.dirty_kind = OSD_DIRTY_NONE;
            need_redraw   = false;
        }

        /* Flush pending hardware DSI brightness after presenting the UI frame */
        osd_hw_flush_brightness(&hw, &st, !touch.touching);
    }

    osd_touch_destroy(&touch);
    osd_drm_destroy(&drm);
    osd_canvas_destroy(canvas);
    osd_font_destroy(&font);
    return 0;
}

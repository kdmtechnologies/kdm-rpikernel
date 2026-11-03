/*
 * osd_types.h - Core Domain Models & Configuration Types for kdm-osd
 */
#ifndef KDM_OSD_TYPES_H
#define KDM_OSD_TYPES_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define PI_F 3.14159265358979323846f
#define OSD_SCREEN_W 1920
#define OSD_SCREEN_H 1080
#define OSD_QS_COUNT 7

typedef enum {
    OSD_QS_WIFI      = 0,
    OSD_QS_DATA      = 1,
    OSD_QS_BLUETOOTH = 2,
    OSD_QS_CALLS     = 3,
    OSD_QS_LOCATION  = 4,
    OSD_QS_SOUND     = 5,
    OSD_QS_TORCH     = 6,
} osd_qs_id_t;

typedef enum {
    OSD_DIRTY_NONE       = 0,
    OSD_DIRTY_FULL       = 1, /* Re-render & blit active surface */
    OSD_DIRTY_SHADE_PULL = 2, /* Zero-copy DRM plane crop update only (swipe animation) */
    OSD_DIRTY_VOL_SLIDER = 3, /* Partial dirty-rect update for Volume slider */
    OSD_DIRTY_BRT_SLIDER = 4, /* Partial dirty-rect update for Brightness slider */
    OSD_DIRTY_QS_ROW     = 5, /* Partial dirty-rect update for Quick Settings toggle row */
} osd_dirty_kind_t;

typedef struct {
    int x, y, w, h;
} osd_rect_t;

typedef struct {
    int rotate;              /* 0, 90, 180, 270 (default 270 for RM67199 landscape) */
    float scale;             /* UI scale factor (default 1.0) */
    int margin_top;          /* Top margin in landscape pixels */
    int margin_right;        /* Right margin in landscape pixels */
    int mock_battery;        /* Mock battery % (0..100, default 75) */
    bool mock_charging;      /* Mock charging state */
    int mock_wifi;           /* -1 = live /proc/net/wireless, 0..3 = mock bars */
    int init_volume;         /* Initial volume % (0..100, default 70) */
    int init_brightness;     /* -1 = read /sys/class/backlight, or 0..100 */
    const char *mock_time;   /* NULL = live system clock, or e.g. "1:14PM" */
    bool format_24h;         /* 24-hour clock format */
    bool always_sliders;     /* Keep Quick Settings shade visible continuously */
    const char *preview_out; /* Render PNG preview to path and exit */
    const char *drm_path;    /* Optional explicit /dev/dri/cardX */
} osd_config_t;

typedef struct {
    /* Live / Mock System Status */
    int battery_pct;
    bool charging;
    int wifi_bars;           /* 0..3 */
    bool wifi_connected;
    int brightness_pct;      /* 0..100 */
    int volume_pct;          /* 0..100 */
    int saved_vol_pct;       /* Restored when unmuting Sound quick toggle */

    /* UI Visibility & Animation States */
    bool vol_open;           /* Single-bar Volume HUD popup (external change) */
    bool brt_open;           /* Single-bar Brightness HUD popup (external change) */
    bool shade_open;         /* Pull-down Quick Settings Notification Center shade */
    bool shade_animating;    /* Spring/ease-out animation active after touch release */
    bool shade_rendered;     /* True if full-screen shade is already rendered in DRM buffers */
    float shade_pull_px;     /* Live vertical pull-down height (0..1080 landscape px) */
    float shade_target_px;   /* Target pull-down height (0 or 1080) when animating */
    osd_dirty_kind_t dirty_kind;
    bool qs_active[OSD_QS_COUNT];
    bool pill_active[2];     /* "Device control" / "Media output" pill toggle state */

    uint64_t sliders_activity_ms;
    char time_str[32];
    char date_str[48];

    /* Computed Layout & Hitboxes (landscape coordinates 0..1920 x 0..1080) */
    int surf_w;              /* Full landscape width (1920) */
    int surf_h;              /* Full landscape height (1080) */
    int bar_w;               /* Compact top-right status bar width when shade is closed */
    int bar_h;               /* Compact top-right status bar height when shade is closed */
    int hud_w;               /* Standalone HUD popup width when shade is closed */
    int hud_h;               /* Standalone HUD popup height when shade is closed */
    float panel_x, panel_y, panel_w, panel_h;
    float pill_x[2], pill_y, pill_w, pill_h;
    float gear_cx, gear_cy, gear_r;
    float qs_cx[OSD_QS_COUNT], qs_cy, qs_r;
    float vol_x, vol_y, vol_w, vol_h;
    float brt_x, brt_y, brt_w, brt_h;
} osd_state_t;

static inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline uint64_t osd_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

#endif /* KDM_OSD_TYPES_H */

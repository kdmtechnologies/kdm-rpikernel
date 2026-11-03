/*
 * osd_touch.h - OOP Touchscreen Gesture & Hit-Testing Controller (OsdTouchController)
 *
 * Handles:
 *   - Goodix GT9886 /dev/input/eventX discovery & 270-degree landscape mapping
 *   - Real-time finger-following top-edge swipe-down & bottom-up swipe-up gestures
 *   - 7 Circular Quick-Toggle buttons & 2 Wide Pill buttons tap detection
 *   - Real-time horizontal Volume & Brightness pill slider dragging
 */
#ifndef KDM_OSD_TOUCH_H
#define KDM_OSD_TOUCH_H

#include "osd_hw.h"

typedef enum {
    OSD_DRAG_IGNORE     = -1,
    OSD_DRAG_NONE       = 0,
    OSD_DRAG_TOP_SWIPE  = 1,
    OSD_DRAG_BRIGHTNESS = 2,
    OSD_DRAG_VOLUME     = 3,
    OSD_DRAG_SHADE_CARD = 4,
} osd_drag_mode_t;

typedef struct {
    int fd;
    int max_x;
    int max_y;
    int cur_raw_x;
    int cur_raw_y;
    bool touching;
    bool grabbed;
    osd_drag_mode_t active_drag;
    int start_lx;
    int start_ly;
    int last_ly;
    uint64_t last_move_ms;
    float vel_y;
} osd_touch_t;

void osd_touch_init(osd_touch_t *tc);
void osd_touch_destroy(osd_touch_t *tc);
void osd_touch_set_grab(osd_touch_t *tc, bool grab);
bool osd_touch_process(osd_touch_t *tc, const osd_config_t *cfg,
                       osd_state_t *st, osd_hw_t *hw,
                       int land_w, int land_h);

#endif /* KDM_OSD_TOUCH_H */

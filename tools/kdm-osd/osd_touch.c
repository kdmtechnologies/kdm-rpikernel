/*
 * osd_touch.c - Implementation of OsdTouchController Class
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "osd_touch.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

void osd_touch_init(osd_touch_t *tc) {
    memset(tc, 0, sizeof(*tc));
    tc->fd    = -1;
    tc->max_x = 1080;
    tc->max_y = 1920;

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
                tc->max_x = abs_x.maximum;
            else if (ioctl(fd, EVIOCGABS(ABS_X), &abs_x) == 0 && abs_x.maximum > 0)
                tc->max_x = abs_x.maximum;

            if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &abs_y) == 0 && abs_y.maximum > 0)
                tc->max_y = abs_y.maximum;
            else if (ioctl(fd, EVIOCGABS(ABS_Y), &abs_y) == 0 && abs_y.maximum > 0)
                tc->max_y = abs_y.maximum;

            tc->fd = fd;
            printf("kdm-osd: connected touchscreen '%s' (%s, %dx%d)\n",
                   name, path, tc->max_x, tc->max_y);
            break;
        }
        close(fd);
    }
    closedir(dir);
}

void osd_touch_set_grab(osd_touch_t *tc, bool grab) {
    if (tc->fd < 0 || tc->grabbed == grab)
        return;
    int arg = grab ? 1 : 0;
    if (ioctl(tc->fd, EVIOCGRAB, &arg) == 0)
        tc->grabbed = grab;
}

void osd_touch_destroy(osd_touch_t *tc) {
    if (!tc)
        return;
    osd_touch_set_grab(tc, false);
    if (tc->fd >= 0) {
        close(tc->fd);
        tc->fd = -1;
    }
}

static void map_touch_to_landscape(const osd_touch_t *tc, const osd_config_t *cfg,
                                   int land_w, int land_h, int *out_lx, int *out_ly) {
    int rx = clampi(tc->cur_raw_x, 0, tc->max_x);
    int ry = clampi(tc->cur_raw_y, 0, tc->max_y);
    if (cfg->rotate == 270) {
        *out_lx = (land_w - 1) - (ry * land_w) / tc->max_y;
        *out_ly = (rx * land_h) / tc->max_x;
    } else if (cfg->rotate == 90) {
        *out_lx = (ry * land_w) / tc->max_y;
        *out_ly = (land_h - 1) - (rx * land_h) / tc->max_x;
    } else if (cfg->rotate == 180) {
        *out_lx = (land_w - 1) - (rx * land_w) / tc->max_x;
        *out_ly = (land_h - 1) - (ry * land_h) / tc->max_y;
    } else {
        *out_lx = (rx * land_w) / tc->max_x;
        *out_ly = (ry * land_h) / tc->max_y;
    }
}

static void upgrade_dirty(osd_state_t *st, osd_dirty_kind_t kind) {
    if (st->dirty_kind == OSD_DIRTY_FULL)
        return;
    if (kind == OSD_DIRTY_FULL) {
        st->dirty_kind = OSD_DIRTY_FULL;
        return;
    }
    if (st->dirty_kind == OSD_DIRTY_NONE || st->dirty_kind == OSD_DIRTY_SHADE_PULL) {
        st->dirty_kind = kind;
    } else if (st->dirty_kind != kind) {
        st->dirty_kind = OSD_DIRTY_FULL;
    }
}

static void handle_touch_release(osd_touch_t *tc, const osd_config_t *cfg,
                                 osd_state_t *st, int land_w, int land_h, bool *dirty) {
    tc->touching = false;

    if (tc->active_drag == OSD_DRAG_TOP_SWIPE) {
        /* If user tapped the top-right status bar or dragged/flicked down -> animate open */
        bool tapped_bar = (tc->start_lx >= land_w - st->bar_w - 24 &&
                           tc->start_ly <= cfg->margin_top + st->bar_h + 24 &&
                           abs(tc->last_ly - tc->start_ly) < 18);
        if (tapped_bar || st->shade_pull_px >= (float)land_h * 0.18f || tc->vel_y > 0.32f) {
            st->shade_open      = true;
            st->vol_open        = false;
            st->brt_open        = false;
            st->shade_target_px = (float)land_h;
            st->shade_animating = true;
            if (st->shade_pull_px < 24.0f)
                st->shade_pull_px = 24.0f;
            st->sliders_activity_ms = osd_now_ms();
            osd_touch_set_grab(tc, true);
            upgrade_dirty(st, st->shade_rendered ? OSD_DIRTY_SHADE_PULL : OSD_DIRTY_FULL);
            *dirty = true;
        } else if (st->shade_open || st->shade_pull_px > 0.0f) {
            st->shade_target_px = 0.0f;
            st->shade_animating = true;
            upgrade_dirty(st, st->shade_rendered ? OSD_DIRTY_SHADE_PULL : OSD_DIRTY_FULL);
            *dirty = true;
        } else {
            osd_touch_set_grab(tc, false);
        }
    } else if (tc->active_drag == OSD_DRAG_SHADE_CARD) {
        if (st->shade_pull_px < (float)land_h - 4.0f) {
            if (!cfg->always_sliders &&
                (st->shade_pull_px <= (float)land_h * 0.80f || tc->vel_y < -0.32f)) {
                st->shade_target_px = 0.0f;
                st->shade_animating = true;
            } else {
                st->shade_target_px = (float)land_h;
                st->shade_animating = true;
            }
            upgrade_dirty(st, st->shade_rendered ? OSD_DIRTY_SHADE_PULL : OSD_DIRTY_FULL);
            *dirty = true;
        }
    }

    tc->active_drag = OSD_DRAG_NONE;
    if (!st->shade_open && !st->shade_animating)
        osd_touch_set_grab(tc, false);
}

bool osd_touch_process(osd_touch_t *tc, const osd_config_t *cfg,
                       osd_state_t *st, osd_hw_t *hw,
                       int land_w, int land_h) {
    if (tc->fd < 0)
        return false;

    bool dirty = false;
    int new_brt_pct = -1;
    int new_vol_pct = -1;
    struct input_event ev;
    while (read(tc->fd, &ev, sizeof(ev)) == sizeof(ev)) {
        if (ev.type == EV_ABS) {
            if (ev.code == ABS_MT_POSITION_X || ev.code == ABS_X) {
                tc->cur_raw_x = ev.value;
            } else if (ev.code == ABS_MT_POSITION_Y || ev.code == ABS_Y) {
                tc->cur_raw_y = ev.value;
            } else if (ev.code == ABS_MT_TRACKING_ID) {
                if (ev.value >= 0 && !tc->touching) {
                    tc->touching = true;
                } else if (ev.value == -1 && tc->touching) {
                    handle_touch_release(tc, cfg, st, land_w, land_h, &dirty);
                }
            }
        } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
            if (ev.value == 1 && !tc->touching) {
                tc->touching = true;
            } else if (ev.value == 0 && tc->touching) {
                handle_touch_release(tc, cfg, st, land_w, land_h, &dirty);
            }
        } else if (ev.type == EV_SYN && ev.code == SYN_REPORT && tc->touching) {
            int lx = 0, ly = 0;
            map_touch_to_landscape(tc, cfg, land_w, land_h, &lx, &ly);
            uint64_t now = osd_now_ms();

            float rx = (float)lx;
            float ry = (float)ly;

            if (tc->active_drag == OSD_DRAG_NONE) {
                tc->start_lx     = lx;
                tc->start_ly     = ly;
                tc->last_ly      = ly;
                tc->last_move_ms = now;
                tc->vel_y        = 0.0f;

                if (st->shade_open && st->shade_pull_px >= (float)land_h * 0.5f) {
                    st->sliders_activity_ms = now;
                    st->shade_animating     = false;
                    st->shade_pull_px       = (float)land_h;

                    /* 1. Check 7 Circular Quick-Toggle Buttons */
                    int hit_qs = -1;
                    for (int i = 0; i < OSD_QS_COUNT; i++) {
                        if (hypotf(rx - st->qs_cx[i], ry - st->qs_cy) <= st->qs_r + 14.0f) {
                            hit_qs = i;
                            break;
                        }
                    }
                    if (hit_qs >= 0) {
                        osd_hw_toggle_qs(hw, st, (osd_qs_id_t)hit_qs);
                        osd_hw_update_status(hw, cfg, st);
                        tc->active_drag = OSD_DRAG_SHADE_CARD;
                        osd_touch_set_grab(tc, true);
                        upgrade_dirty(st, (hit_qs == OSD_QS_WIFI) ? OSD_DIRTY_FULL : OSD_DIRTY_QS_ROW);
                        dirty = true;
                    }
                    /* 2. Check 2 Wide Pill Buttons ("Device control" / "Media output") */
                    else if (ry >= st->pill_y - 6.0f && ry <= st->pill_y + st->pill_h + 6.0f &&
                             ((rx >= st->pill_x[0] && rx <= st->pill_x[0] + st->pill_w) ||
                              (rx >= st->pill_x[1] && rx <= st->pill_x[1] + st->pill_w))) {
                        int p = (rx >= st->pill_x[1]) ? 1 : 0;
                        st->pill_active[p] = !st->pill_active[p];
                        tc->active_drag = OSD_DRAG_SHADE_CARD;
                        osd_touch_set_grab(tc, true);
                        upgrade_dirty(st, OSD_DIRTY_FULL);
                        dirty = true;
                    }
                    /* 3. Check Brightness Pill Slider Row */
                    else if (rx >= st->brt_x - 24.0f && rx <= st->brt_x + st->brt_w + 24.0f &&
                             ry >= st->brt_y - 12.0f && ry <= st->brt_y + st->brt_h + 12.0f) {
                        tc->active_drag = OSD_DRAG_BRIGHTNESS;
                        osd_touch_set_grab(tc, true);
                    }
                    /* 4. Check Volume Pill Slider Row */
                    else if (rx >= st->vol_x - 24.0f && rx <= st->vol_x + st->vol_w + 24.0f &&
                             ry >= st->vol_y - 12.0f && ry <= st->vol_y + st->vol_h + 12.0f) {
                        tc->active_drag = OSD_DRAG_VOLUME;
                        osd_touch_set_grab(tc, true);
                    }
                    /* 5. Touch anywhere else on Full-Screen Shade -> allow swipe-up to close */
                    else {
                        tc->active_drag = OSD_DRAG_SHADE_CARD;
                        osd_touch_set_grab(tc, true);
                    }
                } else if (st->vol_open || st->brt_open) {
                    /* Standalone single-bar HUD popup is open */
                    st->sliders_activity_ms = now;
                    if (st->brt_open &&
                        rx >= st->brt_x - 36.0f && rx <= st->brt_x + st->brt_w + 36.0f &&
                        ry >= st->brt_y - 8.0f  && ry <= st->brt_y + st->brt_h + 8.0f) {
                        tc->active_drag = OSD_DRAG_BRIGHTNESS;
                        osd_touch_set_grab(tc, true);
                    } else if (st->vol_open &&
                               rx >= st->vol_x - 36.0f && rx <= st->vol_x + st->vol_w + 36.0f &&
                               ry >= st->vol_y - 8.0f  && ry <= st->vol_y + st->vol_h + 8.0f) {
                        tc->active_drag = OSD_DRAG_VOLUME;
                        osd_touch_set_grab(tc, true);
                    } else if (ly <= 80) {
                        tc->active_drag = OSD_DRAG_TOP_SWIPE;
                    } else {
                        st->vol_open = false;
                        st->brt_open = false;
                        tc->active_drag = OSD_DRAG_IGNORE;
                        osd_touch_set_grab(tc, false);
                        upgrade_dirty(st, OSD_DIRTY_FULL);
                        dirty = true;
                    }
                } else {
                    /* Shade & HUD closed: check if touch begins along top edge (ly <= 80) */
                    if (ly <= 80) {
                        tc->active_drag = OSD_DRAG_TOP_SWIPE;
                    } else {
                        tc->active_drag = OSD_DRAG_IGNORE;
                    }
                }
            } else {
                uint64_t dt = (now > tc->last_move_ms) ? (now - tc->last_move_ms) : 1;
                float inst_v = (float)(ly - tc->last_ly) / (float)dt;
                tc->vel_y = tc->vel_y * 0.55f + inst_v * 0.45f;
                tc->last_ly = ly;
                tc->last_move_ms = now;
            }

            /* Handle ongoing gestures / drags */
            if (tc->active_drag == OSD_DRAG_TOP_SWIPE) {
                int dy = ly - tc->start_ly;
                if (dy >= 6 || st->shade_open) {
                    st->shade_open          = true;
                    st->shade_animating     = false;
                    st->vol_open            = false;
                    st->brt_open            = false;
                    st->sliders_activity_ms = now;
                    float new_pull = clampf((float)ly, 16.0f, (float)land_h);
                    if (fabsf(new_pull - st->shade_pull_px) >= 1.0f) {
                        st->shade_pull_px = new_pull;
                        osd_touch_set_grab(tc, true);
                        upgrade_dirty(st, st->shade_rendered ? OSD_DIRTY_SHADE_PULL : OSD_DIRTY_FULL);
                        dirty = true;
                    }
                }
            } else if (tc->active_drag == OSD_DRAG_SHADE_CARD) {
                int dy = ly - tc->start_ly;
                if (dy < -6 && !cfg->always_sliders) {
                    st->sliders_activity_ms = now;
                    st->shade_animating     = false;
                    float new_pull = clampf((float)land_h + (float)dy, 8.0f, (float)land_h);
                    if (fabsf(new_pull - st->shade_pull_px) >= 1.0f) {
                        st->shade_pull_px = new_pull;
                        upgrade_dirty(st, st->shade_rendered ? OSD_DIRTY_SHADE_PULL : OSD_DIRTY_FULL);
                        dirty = true;
                    }
                }
            } else if (tc->active_drag == OSD_DRAG_BRIGHTNESS) {
                st->sliders_activity_ms = now;
                int pct = (int)roundf(((rx - st->brt_x) * 100.0f) / st->brt_w);
                pct = clampi(pct, 0, 100);
                if (pct != st->brightness_pct) {
                    st->brightness_pct = pct;
                    new_brt_pct = pct;
                    upgrade_dirty(st, st->shade_open ? OSD_DIRTY_BRT_SLIDER : OSD_DIRTY_FULL);
                    dirty = true;
                }
            } else if (tc->active_drag == OSD_DRAG_VOLUME) {
                st->sliders_activity_ms = now;
                int pct = (int)roundf(((rx - st->vol_x) * 100.0f) / st->vol_w);
                pct = clampi(pct, 0, 100);
                if (pct != st->volume_pct) {
                    st->volume_pct = pct;
                    new_vol_pct = pct;
                    bool was_snd = st->qs_active[OSD_QS_SOUND];
                    st->qs_active[OSD_QS_SOUND] = (pct > 0);
                    if (st->shade_open) {
                        upgrade_dirty(st, (was_snd != st->qs_active[OSD_QS_SOUND])
                                          ? OSD_DIRTY_QS_ROW : OSD_DIRTY_VOL_SLIDER);
                    } else {
                        upgrade_dirty(st, OSD_DIRTY_FULL);
                    }
                    dirty = true;
                }
            }
        }
    }

    if (new_brt_pct >= 0)
        osd_hw_set_brightness(hw, st, new_brt_pct);
    if (new_vol_pct >= 0)
        osd_hw_set_volume(hw, st, new_vol_pct);

    return dirty;
}

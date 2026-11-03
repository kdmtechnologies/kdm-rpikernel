/*
 * osd_hw.c - Implementation of OsdHardware Class
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "osd_hw.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

static bool timespec_changed(const struct timespec *a, const struct timespec *b) {
    return (a->tv_sec != b->tv_sec) || (a->tv_nsec != b->tv_nsec);
}

static void record_file_mtime(const char *path, struct timespec *out_mtime) {
    struct stat sb;
    if (stat(path, &sb) == 0)
        *out_mtime = sb.st_mtim;
}

static void discover_backlight(osd_hw_t *hw) {
    DIR *dir = opendir("/sys/class/backlight");
    if (!dir)
        return;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.')
            continue;
        snprintf(hw->backlight_dir, sizeof(hw->backlight_dir),
                 "/sys/class/backlight/%.180s", ent->d_name);
        char max_path[300];
        snprintf(max_path, sizeof(max_path), "%s/max_brightness", hw->backlight_dir);
        FILE *fp = fopen(max_path, "r");
        if (fp) {
            if (fscanf(fp, "%d", &hw->backlight_max) != 1 || hw->backlight_max <= 0)
                hw->backlight_max = 255;
            fclose(fp);
        }
        break;
    }
    closedir(dir);
}

static int raw_to_brightness_pct(const osd_hw_t *hw, int raw) {
    int min_raw = 10; /* Safe minimum so OLED never turns completely off */
    if (raw <= min_raw)
        return 0;
    int span = hw->backlight_max - min_raw;
    if (span <= 0)
        span = 245;
    int pct = ((raw - min_raw) * 100 + span / 2) / span;
    return clampi(pct, 0, 100);
}

static int read_raw_backlight(const osd_hw_t *hw) {
    if (!hw->backlight_dir[0])
        return -1;
    char path[300];
    snprintf(path, sizeof(path), "%s/brightness", hw->backlight_dir);
    FILE *fp = fopen(path, "r");
    if (!fp)
        return -1;
    int raw = -1;
    if (fscanf(fp, "%d", &raw) != 1)
        raw = -1;
    fclose(fp);
    return raw;
}

static void write_raw_backlight_from_pct(osd_hw_t *hw, int pct) {
    if (!hw->backlight_dir[0])
        return;
    pct = clampi(pct, 0, 100);
    int min_raw = 10;
    int raw = clampi(min_raw + (pct * (hw->backlight_max - min_raw)) / 100,
                     min_raw, hw->backlight_max);
    if (raw == hw->last_hw_raw)
        return;
    char path[300];
    snprintf(path, sizeof(path), "%s/brightness", hw->backlight_dir);
    FILE *fp = fopen(path, "w");
    if (fp) {
        fprintf(fp, "%d\n", raw);
        fclose(fp);
        hw->last_hw_raw = raw;
    }
}

static int read_brightness_pct(osd_hw_t *hw, int fallback_pct) {
    FILE *ovr = fopen("/run/kdm-osd/brightness", "r");
    if (ovr) {
        int val = -1;
        if (fscanf(ovr, "%d", &val) == 1 && val >= 0) {
            fclose(ovr);
            if (val > 100 && val <= hw->backlight_max)
                return raw_to_brightness_pct(hw, val);
            return clampi(val, 0, 100);
        }
        fclose(ovr);
    }
    int raw = read_raw_backlight(hw);
    if (raw >= 0) {
        hw->last_hw_raw = raw;
        return raw_to_brightness_pct(hw, raw);
    }
    return fallback_pct;
}

static int read_volume_pct(int fallback_pct) {
    FILE *fp = fopen("/run/kdm-osd/volume", "r");
    if (fp) {
        int v = -1;
        if (fscanf(fp, "%d", &v) == 1 && v >= 0) {
            fclose(fp);
            return clampi(v, 0, 100);
        }
        fclose(fp);
    }
    return fallback_pct;
}

void osd_hw_flush_brightness(osd_hw_t *hw, const osd_state_t *st, bool force) {
    (void)st;
    if (hw->pending_brt_pct < 0)
        return;
    uint64_t now = osd_now_ms();
    if (!force && (now - hw->last_brt_write_ms) < 18ULL)
        return;
    int pct = hw->pending_brt_pct;
    hw->pending_brt_pct = -1;
    hw->last_brt_write_ms = now;
    mkdir("/run/kdm-osd", 0755);
    FILE *ovr = fopen("/run/kdm-osd/brightness", "w");
    if (ovr) {
        fprintf(ovr, "%d\n", pct);
        fclose(ovr);
        record_file_mtime("/run/kdm-osd/brightness", &hw->brt_mtime);
    }
    write_raw_backlight_from_pct(hw, pct);
}

void osd_hw_set_brightness(osd_hw_t *hw, osd_state_t *st, int pct) {
    pct = clampi(pct, 0, 100);
    st->brightness_pct = pct;
    hw->pending_brt_pct = pct;
}

void osd_hw_set_volume(osd_hw_t *hw, osd_state_t *st, int pct) {
    pct = clampi(pct, 0, 100);
    st->volume_pct = pct;
    if (pct > 0)
        st->saved_vol_pct = pct;
    mkdir("/run/kdm-osd", 0755);
    FILE *fp = fopen("/run/kdm-osd/volume", "w");
    if (fp) {
        fprintf(fp, "%d\n", pct);
        fclose(fp);
        record_file_mtime("/run/kdm-osd/volume", &hw->vol_mtime);
    }
}

void osd_hw_toggle_qs(osd_hw_t *hw, osd_state_t *st, osd_qs_id_t id) {
    if (id < 0 || id >= OSD_QS_COUNT)
        return;
    st->qs_active[id] = !st->qs_active[id];
    if (id == OSD_QS_SOUND) {
        if (!st->qs_active[OSD_QS_SOUND]) {
            if (st->volume_pct > 0)
                st->saved_vol_pct = st->volume_pct;
            osd_hw_set_volume(hw, st, 0);
        } else if (st->volume_pct == 0) {
            osd_hw_set_volume(hw, st, st->saved_vol_pct > 0 ? st->saved_vol_pct : 70);
        }
    }
}

void osd_hw_init(osd_hw_t *hw, const osd_config_t *cfg, osd_state_t *st) {
    memset(hw, 0, sizeof(*hw));
    hw->backlight_max   = 255;
    hw->last_hw_raw     = -1;
    hw->pending_brt_pct = -1;

    mkdir("/run/kdm-osd", 0755);
    discover_backlight(hw);

    /* Default Quick Settings active toggles matching reference UI */
    st->qs_active[OSD_QS_WIFI]      = true;
    st->qs_active[OSD_QS_DATA]      = true;
    st->qs_active[OSD_QS_BLUETOOTH] = false;
    st->qs_active[OSD_QS_CALLS]     = false;
    st->qs_active[OSD_QS_LOCATION]  = false;
    st->qs_active[OSD_QS_SOUND]     = true;
    st->qs_active[OSD_QS_TORCH]     = false;

    st->volume_pct    = read_volume_pct(cfg->init_volume);
    st->saved_vol_pct = st->volume_pct > 0 ? st->volume_pct : 70;

    if (cfg->init_brightness >= 0) {
        osd_hw_set_brightness(hw, st, cfg->init_brightness);
        osd_hw_flush_brightness(hw, st, true);
    } else {
        st->brightness_pct = read_brightness_pct(hw, 75);
        write_raw_backlight_from_pct(hw, st->brightness_pct);
    }

    record_file_mtime("/run/kdm-osd/volume", &hw->vol_mtime);
    record_file_mtime("/run/kdm-osd/brightness", &hw->brt_mtime);

    st->shade_open      = cfg->always_sliders;
    st->shade_pull_px   = cfg->always_sliders ? (float)OSD_SCREEN_H : 0.0f;
    st->shade_target_px = st->shade_pull_px;
    st->shade_animating = false;
    st->shade_rendered  = false;
    st->dirty_kind      = OSD_DIRTY_FULL;
    st->vol_open        = false;
    st->brt_open        = false;
    st->sliders_activity_ms = osd_now_ms();

    osd_hw_update_status(hw, cfg, st);
}

void osd_hw_update_status(osd_hw_t *hw, const osd_config_t *cfg, osd_state_t *st) {
    (void)hw;
    /* 1. Clock & Date string */
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);

    static const char *const wdays[7] = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
    };
    static const char *const months[12] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    snprintf(st->date_str, sizeof(st->date_str), "%s, %d %s",
             wdays[clampi(tmv.tm_wday, 0, 6)],
             tmv.tm_mday,
             months[clampi(tmv.tm_mon, 0, 11)]);

    if (cfg->mock_time && cfg->mock_time[0]) {
        snprintf(st->time_str, sizeof(st->time_str), "%s", cfg->mock_time);
    } else if (cfg->format_24h) {
        snprintf(st->time_str, sizeof(st->time_str), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    } else {
        int h12 = tmv.tm_hour % 12;
        if (h12 == 0)
            h12 = 12;
        const char *ampm = (tmv.tm_hour >= 12) ? "PM" : "AM";
        snprintf(st->time_str, sizeof(st->time_str), "%d:%02d%s", h12, tmv.tm_min, ampm);
    }

    /* 2. Wi-Fi Signal Bars */
    if (!st->qs_active[OSD_QS_WIFI]) {
        st->wifi_connected = false;
        st->wifi_bars = 0;
    } else if (cfg->mock_wifi >= 0) {
        st->wifi_connected = (cfg->mock_wifi > 0);
        st->wifi_bars = clampi(cfg->mock_wifi, 0, 3);
    } else {
        FILE *fp = fopen("/proc/net/wireless", "r");
        if (!fp) {
            st->wifi_connected = true;
            st->wifi_bars = 3;
        } else {
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
    }

    /* 3. Battery Status */
    FILE *ovr = fopen("/run/kdm-osd/battery", "r");
    if (ovr) {
        int pct = -1;
        char status_buf[32] = {0};
        if (fscanf(ovr, "%d %31s", &pct, status_buf) >= 1 && pct >= 0 && pct <= 100) {
            st->battery_pct = pct;
            st->charging = (strcasecmp(status_buf, "Charging") == 0 ||
                            strcmp(status_buf, "1") == 0);
            fclose(ovr);
            return;
        }
        fclose(ovr);
    }
    st->battery_pct = cfg->mock_battery;
    st->charging    = cfg->mock_charging;
}

bool osd_hw_poll_external_sliders(osd_hw_t *hw, osd_state_t *st, uint64_t now_ms) {
    bool vol_trig = false;
    bool brt_trig = false;
    struct stat sb;

    /* 1. Check if /run/kdm-osd/volume was written/touched */
    if (stat("/run/kdm-osd/volume", &sb) == 0 &&
        timespec_changed(&sb.st_mtim, &hw->vol_mtime)) {
        hw->vol_mtime = sb.st_mtim;
        st->volume_pct = read_volume_pct(st->volume_pct);
        vol_trig = true;
    }

    /* 2. Check if /run/kdm-osd/brightness was written/touched */
    if (stat("/run/kdm-osd/brightness", &sb) == 0 &&
        timespec_changed(&sb.st_mtim, &hw->brt_mtime)) {
        hw->brt_mtime = sb.st_mtim;
        st->brightness_pct = read_brightness_pct(hw, st->brightness_pct);
        write_raw_backlight_from_pct(hw, st->brightness_pct);
        brt_trig = true;
    }

    /* 3. Check if hardware /sys/class/backlight/.../brightness changed directly */
    int raw = read_raw_backlight(hw);
    if (raw >= 0 && hw->last_hw_raw >= 0 && raw != hw->last_hw_raw) {
        hw->last_hw_raw = raw;
        st->brightness_pct = raw_to_brightness_pct(hw, raw);
        FILE *ovr = fopen("/run/kdm-osd/brightness", "w");
        if (ovr) {
            fprintf(ovr, "%d\n", st->brightness_pct);
            fclose(ovr);
            record_file_mtime("/run/kdm-osd/brightness", &hw->brt_mtime);
        }
        brt_trig = true;
    } else if (raw >= 0 && hw->last_hw_raw < 0) {
        hw->last_hw_raw = raw;
    }

    if (st->shade_open) {
        if (vol_trig || brt_trig) {
            st->sliders_activity_ms = now_ms;
            if (vol_trig && !brt_trig && st->dirty_kind == OSD_DIRTY_NONE)
                st->dirty_kind = OSD_DIRTY_VOL_SLIDER;
            else if (brt_trig && !vol_trig && st->dirty_kind == OSD_DIRTY_NONE)
                st->dirty_kind = OSD_DIRTY_BRT_SLIDER;
            else
                st->dirty_kind = OSD_DIRTY_FULL;
        }
        return (vol_trig || brt_trig);
    }

    if (vol_trig || brt_trig)
        st->dirty_kind = OSD_DIRTY_FULL;

    if (vol_trig && !brt_trig) {
        st->vol_open = true;
        st->brt_open = false;
        st->sliders_activity_ms = now_ms;
    } else if (brt_trig && !vol_trig) {
        st->brt_open = true;
        st->vol_open = false;
        st->sliders_activity_ms = now_ms;
    } else if (vol_trig && brt_trig) {
        st->vol_open = true;
        st->brt_open = true;
        st->sliders_activity_ms = now_ms;
    }
    return (vol_trig || brt_trig);
}

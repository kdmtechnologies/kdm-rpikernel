/*
 * osd_hw.c - Implementation of OsdHardware Class
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "osd_hw.h"

#include <alsa/asoundlib.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static bool timespec_changed(const struct timespec *a, const struct timespec *b) {
    return (a->tv_sec != b->tv_sec) || (a->tv_nsec != b->tv_nsec);
}

static void record_file_mtime(const char *path, struct timespec *out_mtime) {
    struct stat sb;
    if (stat(path, &sb) == 0)
        *out_mtime = sb.st_mtim;
}

/* ==========================================================================
 * 1. ALSA Hardware Mixer Control (Zero-fork real-time volume & mute)
 * ========================================================================== */

static void init_alsa_mixer(osd_hw_t *hw) {
    snd_mixer_t *mixer = NULL;
    if (snd_mixer_open(&mixer, 0) < 0)
        return;

    const char *cards[] = { "hw:0", "default", "hw:1", NULL };
    bool attached = false;
    for (int i = 0; cards[i]; i++) {
        if (snd_mixer_attach(mixer, cards[i]) == 0) {
            attached = true;
            break;
        }
    }
    if (!attached ||
        snd_mixer_selem_register(mixer, NULL, NULL) < 0 ||
        snd_mixer_load(mixer) < 0) {
        snd_mixer_close(mixer);
        return;
    }

    static const char *const preferred_names[] = {
        "PCM", "Master", "Speaker", "Headphone", "Digital", "SoftMaster", NULL
    };

    snd_mixer_elem_t *found_elem = NULL;
    for (int p = 0; preferred_names[p] && !found_elem; p++) {
        for (snd_mixer_elem_t *elem = snd_mixer_first_elem(mixer);
             elem; elem = snd_mixer_elem_next(elem)) {
            if (!snd_mixer_selem_is_active(elem) ||
                !snd_mixer_selem_has_playback_volume(elem))
                continue;
            const char *name = snd_mixer_selem_get_name(elem);
            if (name && strcasecmp(name, preferred_names[p]) == 0) {
                found_elem = elem;
                break;
            }
        }
    }

    if (!found_elem) {
        for (snd_mixer_elem_t *elem = snd_mixer_first_elem(mixer);
             elem; elem = snd_mixer_elem_next(elem)) {
            if (snd_mixer_selem_is_active(elem) &&
                snd_mixer_selem_has_playback_volume(elem)) {
                found_elem = elem;
                break;
            }
        }
    }

    if (!found_elem) {
        snd_mixer_close(mixer);
        return;
    }

    hw->alsa_mixer = mixer;
    hw->alsa_elem  = found_elem;
    snd_mixer_selem_get_playback_volume_range(found_elem, &hw->alsa_min_vol, &hw->alsa_max_vol);

    if (snd_mixer_selem_get_playback_dB_range(found_elem, &hw->alsa_min_db, &hw->alsa_max_db) == 0 &&
        hw->alsa_max_db > hw->alsa_min_db) {
        hw->alsa_has_db = true;
    } else {
        hw->alsa_has_db = false;
    }
}

static bool read_alsa_state(osd_hw_t *hw, int *out_pct, bool *out_unmuted) {
    if (!hw->alsa_mixer || !hw->alsa_elem)
        return false;

    snd_mixer_t *mixer     = (snd_mixer_t *)hw->alsa_mixer;
    snd_mixer_elem_t *elem = (snd_mixer_elem_t *)hw->alsa_elem;
    snd_mixer_handle_events(mixer);

    int sw = 1;
    if (snd_mixer_selem_has_playback_switch(elem)) {
        snd_mixer_selem_get_playback_switch(elem, SND_MIXER_SCHN_FRONT_LEFT, &sw);
    }
    bool unmuted = (sw != 0);

    int pct = 0;
    if (hw->alsa_has_db) {
        long db = 0;
        if (snd_mixer_selem_get_playback_dB(elem, SND_MIXER_SCHN_FRONT_LEFT, &db) == 0) {
            if (db <= hw->alsa_min_db) {
                pct = 0;
            } else {
                /* Inverse of amixer -M / PipeWire 60dB perceptual curve */
                float norm = powf(10.0f, (float)(db - hw->alsa_max_db) / 6000.0f);
                pct = clampi((int)lroundf(norm * 100.0f), 0, 100);
            }
        }
    } else {
        long raw = 0;
        if (snd_mixer_selem_get_playback_volume(elem, SND_MIXER_SCHN_FRONT_LEFT, &raw) == 0) {
            long span = hw->alsa_max_vol - hw->alsa_min_vol;
            if (span > 0)
                pct = clampi((int)(((raw - hw->alsa_min_vol) * 100L + span / 2) / span), 0, 100);
        }
    }

    if (!unmuted)
        pct = 0;

    *out_pct     = pct;
    *out_unmuted = unmuted;
    return true;
}

static void write_alsa_volume_pct(osd_hw_t *hw, int pct) {
    if (!hw->alsa_mixer || !hw->alsa_elem)
        return;

    snd_mixer_elem_t *elem = (snd_mixer_elem_t *)hw->alsa_elem;
    pct = clampi(pct, 0, 100);

    if (pct <= 0) {
        if (snd_mixer_selem_has_playback_switch(elem))
            snd_mixer_selem_set_playback_switch_all(elem, 0);
        snd_mixer_selem_set_playback_volume_all(elem, hw->alsa_min_vol);
        hw->last_alsa_pct     = 0;
        hw->last_alsa_unmuted = false;
        return;
    }

    if (snd_mixer_selem_has_playback_switch(elem))
        snd_mixer_selem_set_playback_switch_all(elem, 1);

    if (hw->alsa_has_db) {
        /* Exact 60dB perceptual curve matching `amixer -M` and PipeWire */
        long target_db = hw->alsa_max_db + (long)lroundf(6000.0f * log10f((float)pct / 100.0f));
        if (target_db < hw->alsa_min_db)
            target_db = hw->alsa_min_db;
        if (target_db > hw->alsa_max_db)
            target_db = hw->alsa_max_db;
        snd_mixer_selem_set_playback_dB_all(elem, target_db, 0);
    } else {
        long span = hw->alsa_max_vol - hw->alsa_min_vol;
        long raw  = hw->alsa_min_vol + (span * (long)pct) / 100L;
        snd_mixer_selem_set_playback_volume_all(elem, raw);
    }

    hw->last_alsa_pct     = pct;
    hw->last_alsa_unmuted = true;
}

/* ==========================================================================
 * 2. Real System Wi-Fi Radio Control (NetworkManager / rfkill / sysfs)
 * ========================================================================== */

static bool is_system_wifi_enabled(void) {
    /* 1. Check /sys/class/rfkill/rfkillN/type == "wlan" -> soft == 0 */
    DIR *dir = opendir("/sys/class/rfkill");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_name[0] == '.')
                continue;
            char type_path[300];
            snprintf(type_path, sizeof(type_path), "/sys/class/rfkill/%.180s/type", ent->d_name);
            FILE *tf = fopen(type_path, "r");
            if (!tf)
                continue;
            char type_buf[32] = {0};
            if (fscanf(tf, "%31s", type_buf) == 1 && strcmp(type_buf, "wlan") == 0) {
                fclose(tf);
                char soft_path[300];
                snprintf(soft_path, sizeof(soft_path), "/sys/class/rfkill/%.180s/soft", ent->d_name);
                FILE *sf = fopen(soft_path, "r");
                if (sf) {
                    int soft_blocked = 0;
                    if (fscanf(sf, "%d", &soft_blocked) == 1) {
                        fclose(sf);
                        closedir(dir);
                        return (soft_blocked == 0);
                    }
                    fclose(sf);
                }
            } else {
                fclose(tf);
            }
        }
        closedir(dir);
    }

    /* 2. Fallback: check /sys/class/net/wlan0/flags (IFF_UP = 0x1) */
    FILE *ff = fopen("/sys/class/net/wlan0/flags", "r");
    if (ff) {
        unsigned int flags = 0;
        if (fscanf(ff, "0x%x", &flags) == 1) {
            fclose(ff);
            return (flags & 0x1u) != 0;
        }
        fclose(ff);
    }
    return true;
}

static void set_system_wifi_enabled(bool enable) {
    /* Write directly to /sys/class/rfkill/rfkillN/soft for instant kernel radio toggle */
    DIR *dir = opendir("/sys/class/rfkill");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_name[0] == '.')
                continue;
            char type_path[300];
            snprintf(type_path, sizeof(type_path), "/sys/class/rfkill/%.180s/type", ent->d_name);
            FILE *tf = fopen(type_path, "r");
            if (!tf)
                continue;
            char type_buf[32] = {0};
            if (fscanf(tf, "%31s", type_buf) == 1 && strcmp(type_buf, "wlan") == 0) {
                fclose(tf);
                char soft_path[300];
                snprintf(soft_path, sizeof(soft_path), "/sys/class/rfkill/%.180s/soft", ent->d_name);
                FILE *sf = fopen(soft_path, "w");
                if (sf) {
                    fprintf(sf, "%d\n", enable ? 0 : 1);
                    fclose(sf);
                }
            } else {
                fclose(tf);
            }
        }
        closedir(dir);
    }

    /* Also notify NetworkManager / ip link asynchronously so connection state updates cleanly */
    if (enable) {
        int rc = system("(nmcli radio wifi on || ip link set wlan0 up) >/dev/null 2>&1 &");
        (void)rc;
    } else {
        int rc = system("(nmcli radio wifi off || ip link set wlan0 down) >/dev/null 2>&1 &");
        (void)rc;
    }
}

/* ==========================================================================
 * 3. MAX17055 Fuel Gauge (/dev/i2c-1 @ 0x36)
 *    Battery: Nintendo Switch Lite HDH-003 (3.8V nom / 4.35V max, 3570mAh, 13.6Wh)
 *    Sense Resistor: YAGEO PS0306DRF7T0R01L (0.01 Ohm / 10 mOhm)
 * ========================================================================== */

#define MAX17055_I2C_ADDR       0x36
#define MAX17055_DESIGN_CAP_LSB 7140U   /* 3570 mAh / 0.5 mAh (Rsense = 10 mOhm) */
#define MAX17055_DQACC_LSB      223U    /* 7140 / 32 */
#define MAX17055_DPACC_LSB      1599U   /* (223 * 51200) / 7140 for VChg > 4.275V */
#define MAX17055_ICHGTERM_LSB   730U    /* ~114 mA / 0.15625 mA */
#define MAX17055_VEMPTY_CFG     0xA561U /* 3.30V empty, 3.88V recovery */

static bool max17055_read_reg(int fd, uint8_t reg, uint16_t *out_val) {
    if (fd < 0)
        return false;
    uint8_t buf[2] = {0};
    struct i2c_msg msgs[2] = {
        { .addr = MAX17055_I2C_ADDR, .flags = 0,        .len = 1, .buf = &reg },
        { .addr = MAX17055_I2C_ADDR, .flags = I2C_M_RD, .len = 2, .buf = buf  },
    };
    struct i2c_rdwr_ioctl_data io = { .msgs = msgs, .nmsgs = 2 };
    if (ioctl(fd, I2C_RDWR, &io) < 0)
        return false;
    *out_val = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    return true;
}

static bool max17055_write_reg(int fd, uint8_t reg, uint16_t val) {
    if (fd < 0)
        return false;
    uint8_t buf[3] = { reg, (uint8_t)(val & 0xFFU), (uint8_t)((val >> 8) & 0xFFU) };
    struct i2c_msg msg = {
        .addr  = MAX17055_I2C_ADDR,
        .flags = 0,
        .len   = 3,
        .buf   = buf,
    };
    struct i2c_rdwr_ioctl_data io = { .msgs = &msg, .nmsgs = 1 };
    return (ioctl(fd, I2C_RDWR, &io) >= 0);
}

static void init_max17055(osd_hw_t *hw) {
    hw->i2c_bat_fd   = -1;
    hw->has_max17055 = false;

    int fd = open("/dev/i2c-1", O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return;

    uint16_t devname = 0;
    if (!max17055_read_reg(fd, 0x21, &devname) || (devname & 0xFFF0U) != 0x4010U) {
        close(fd);
        return;
    }

    hw->i2c_bat_fd   = fd;
    hw->has_max17055 = true;

    /* Check if POR (Status bit 1) is set or DesignCap != 3570 mAh (7140 LSB) */
    uint16_t status = 0, dcap = 0;
    if (max17055_read_reg(fd, 0x00, &status) &&
        max17055_read_reg(fd, 0x18, &dcap)) {
        if ((status & 0x0002U) || dcap != MAX17055_DESIGN_CAP_LSB) {
            uint16_t hibcfg = 0x890CU;
            max17055_read_reg(fd, 0xBA, &hibcfg);

            /* Exit Hibernate Mode per Maxim ModelGauge m5 EZ Guide */
            max17055_write_reg(fd, 0x60, 0x0090U);
            max17055_write_reg(fd, 0xBA, 0x0000U);
            max17055_write_reg(fd, 0x60, 0x0000U);

            /* Write HDH-003 (3.8V/4.35V, 3570mAh) + 10mOhm EZ Config */
            max17055_write_reg(fd, 0x18, MAX17055_DESIGN_CAP_LSB);
            max17055_write_reg(fd, 0x45, MAX17055_DQACC_LSB);
            max17055_write_reg(fd, 0x1E, MAX17055_ICHGTERM_LSB);
            max17055_write_reg(fd, 0x3A, MAX17055_VEMPTY_CFG);
            max17055_write_reg(fd, 0x46, MAX17055_DPACC_LSB);
            max17055_write_reg(fd, 0xDB, 0x8400U); /* Refresh=1, VChg=1 (4.35V Li-ion) */

            for (int i = 0; i < 50; i++) {
                uint16_t mcfg = 0;
                if (max17055_read_reg(fd, 0xDB, &mcfg) && !(mcfg & 0x8000U))
                    break;
                usleep(10000);
            }

            max17055_write_reg(fd, 0xBA, hibcfg);
            if (max17055_read_reg(fd, 0x00, &status))
                max17055_write_reg(fd, 0x00, status & ~0x0002U);
        }
    }
}

static bool read_max17055_battery(osd_hw_t *hw, int *out_pct, bool *out_charging) {
    if (!hw->has_max17055 || hw->i2c_bat_fd < 0)
        return false;

    uint16_t status = 0, dcap = 0;
    if (max17055_read_reg(hw->i2c_bat_fd, 0x00, &status) && (status & 0x0002U)) {
        /* Re-run EZ Config if battery was hot-plugged (POR bit set) */
        close(hw->i2c_bat_fd);
        init_max17055(hw);
        if (!hw->has_max17055)
            return false;
    } else if (max17055_read_reg(hw->i2c_bat_fd, 0x18, &dcap) &&
               dcap != MAX17055_DESIGN_CAP_LSB) {
        close(hw->i2c_bat_fd);
        init_max17055(hw);
        if (!hw->has_max17055)
            return false;
    }

    uint16_t repsoc_raw = 0, curr_raw = 0, avg_curr_raw = 0;
    if (!max17055_read_reg(hw->i2c_bat_fd, 0x06, &repsoc_raw))
        return false;

    int pct = clampi((int)lroundf((float)repsoc_raw / 256.0f), 0, 100);
    bool charging = false;
    if (max17055_read_reg(hw->i2c_bat_fd, 0x0A, &curr_raw) &&
        max17055_read_reg(hw->i2c_bat_fd, 0x0B, &avg_curr_raw)) {
        int16_t curr_s     = (int16_t)curr_raw;
        int16_t avg_curr_s = (int16_t)avg_curr_raw;
        /* 1 LSB = 0.15625 mA with 0.01 Ohm sense resistor; > +15 mA (96 LSB) = charging */
        charging = (curr_s > 96 || avg_curr_s > 96);
    }

    *out_pct      = pct;
    *out_charging = charging;
    return true;
}

/* ==========================================================================
 * 4. Backlight & Runtime IPC Files
 * ========================================================================== */

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
        hw->is_dsi_backlight = (strstr(ent->d_name, ".dsi") != NULL);
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
    int min_raw = 10;
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
    /*
     * For MIPI DSI video-mode panels (.dsi), LCD dimming is performed in
     * hardware by the BCM2711 HVS DRM dimmer plane (osd_drm.c) so that the
     * DSI video stream never drops to LPDT or shakes. Only write sysfs
     * brightness for dedicated PWM/GPIO backlights.
     */
    if (!hw->backlight_dir[0] || hw->is_dsi_backlight)
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
    if (!hw->is_dsi_backlight) {
        int raw = read_raw_backlight(hw);
        if (raw >= 0) {
            hw->last_hw_raw = raw;
            return raw_to_brightness_pct(hw, raw);
        }
    }
    return fallback_pct;
}

static int read_volume_pct_file(int fallback_pct) {
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
    st->brightness_pct  = pct;
    hw->pending_brt_pct = pct;
}

void osd_hw_set_volume(osd_hw_t *hw, osd_state_t *st, int pct) {
    pct = clampi(pct, 0, 100);
    st->volume_pct = pct;
    if (pct > 0)
        st->saved_vol_pct = pct;
    st->qs_active[OSD_QS_SOUND] = (pct > 0);

    /* Apply directly to ALSA hardware mixer */
    write_alsa_volume_pct(hw, pct);

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

    if (id == OSD_QS_WIFI) {
        hw->wifi_toggle_grace_until_ms = osd_now_ms() + 4000ULL;
        set_system_wifi_enabled(st->qs_active[OSD_QS_WIFI]);
        if (!st->qs_active[OSD_QS_WIFI]) {
            st->wifi_connected = false;
            st->wifi_bars      = 0;
        }
    } else if (id == OSD_QS_SOUND) {
        if (!st->qs_active[OSD_QS_SOUND]) {
            if (st->volume_pct > 0)
                st->saved_vol_pct = st->volume_pct;
            osd_hw_set_volume(hw, st, 0);
        } else {
            int restore = (st->saved_vol_pct > 0) ? st->saved_vol_pct : 70;
            osd_hw_set_volume(hw, st, restore);
        }
    }
}

void osd_hw_init(osd_hw_t *hw, const osd_config_t *cfg, osd_state_t *st) {
    memset(hw, 0, sizeof(*hw));
    hw->backlight_max   = 255;
    hw->last_hw_raw     = -1;
    hw->pending_brt_pct = -1;
    hw->last_alsa_pct   = -1;

    mkdir("/run/kdm-osd", 0755);
    discover_backlight(hw);
    init_alsa_mixer(hw);
    init_max17055(hw);
    if (hw->has_max17055) {
        /* Clear any stale manual override file when live MAX17055 is present */
        unlink("/run/kdm-osd/battery");
    }

    /* Initialize Quick Settings toggles from real system state */
    st->qs_active[OSD_QS_WIFI]      = is_system_wifi_enabled();
    st->qs_active[OSD_QS_DATA]      = true;
    st->qs_active[OSD_QS_BLUETOOTH] = false;
    st->qs_active[OSD_QS_CALLS]     = false;
    st->qs_active[OSD_QS_LOCATION]  = false;
    st->qs_active[OSD_QS_SOUND]     = true;
    st->qs_active[OSD_QS_TORCH]     = false;

    int alsa_pct = 0;
    bool alsa_unmuted = true;
    if (read_alsa_state(hw, &alsa_pct, &alsa_unmuted)) {
        st->volume_pct              = alsa_pct;
        st->qs_active[OSD_QS_SOUND] = (alsa_unmuted && alsa_pct > 0);
        hw->last_alsa_pct           = alsa_pct;
        hw->last_alsa_unmuted       = alsa_unmuted;
        FILE *fp = fopen("/run/kdm-osd/volume", "w");
        if (fp) {
            fprintf(fp, "%d\n", st->volume_pct);
            fclose(fp);
        }
    } else {
        st->volume_pct              = read_volume_pct_file(cfg->init_volume);
        st->qs_active[OSD_QS_SOUND] = (st->volume_pct > 0);
    }
    st->saved_vol_pct = (st->volume_pct > 0) ? st->volume_pct : 70;

    if (cfg->init_brightness >= 0) {
        osd_hw_set_brightness(hw, st, cfg->init_brightness);
        osd_hw_flush_brightness(hw, st, true);
    } else {
        st->brightness_pct = read_brightness_pct(hw, 85);
        osd_hw_set_brightness(hw, st, st->brightness_pct);
        osd_hw_flush_brightness(hw, st, true);
    }

    record_file_mtime("/run/kdm-osd/volume", &hw->vol_mtime);
    record_file_mtime("/run/kdm-osd/brightness", &hw->brt_mtime);

    st->shade_open          = cfg->always_sliders;
    st->shade_pull_px       = cfg->always_sliders ? (float)OSD_SCREEN_H : 0.0f;
    st->shade_target_px     = st->shade_pull_px;
    st->shade_animating     = false;
    st->shade_rendered      = false;
    st->dirty_kind          = OSD_DIRTY_FULL;
    st->vol_open            = false;
    st->brt_open            = false;
    st->sliders_activity_ms = osd_now_ms();

    osd_hw_update_status(hw, cfg, st);
}

void osd_hw_destroy(osd_hw_t *hw) {
    if (!hw)
        return;
    if (hw->i2c_bat_fd >= 0) {
        close(hw->i2c_bat_fd);
        hw->i2c_bat_fd   = -1;
        hw->has_max17055 = false;
    }
    if (hw->alsa_mixer) {
        snd_mixer_close((snd_mixer_t *)hw->alsa_mixer);
        hw->alsa_mixer = NULL;
        hw->alsa_elem  = NULL;
    }
}

void osd_hw_update_status(osd_hw_t *hw, const osd_config_t *cfg, osd_state_t *st) {
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

    /* 2. Sync Wi-Fi radio state & Signal Bars */
    uint64_t now_ms = osd_now_ms();
    if (cfg->mock_wifi < 0 && now_ms >= hw->wifi_toggle_grace_until_ms) {
        st->qs_active[OSD_QS_WIFI] = is_system_wifi_enabled();
    }

    if (!st->qs_active[OSD_QS_WIFI]) {
        st->wifi_connected = false;
        st->wifi_bars      = 0;
    } else if (cfg->mock_wifi >= 0) {
        st->wifi_connected = (cfg->mock_wifi > 0);
        st->wifi_bars      = clampi(cfg->mock_wifi, 0, 3);
    } else {
        FILE *fp = fopen("/proc/net/wireless", "r");
        if (!fp) {
            st->wifi_connected = true;
            st->wifi_bars      = 3;
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
                st->wifi_bars      = 0;
            }
        }
    }

    /* 3. Battery Status (Manual /run/kdm-osd/battery override -> Live MAX17055 -> Mock fallback) */
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

    int live_pct = 0;
    bool live_chg = false;
    if (read_max17055_battery(hw, &live_pct, &live_chg)) {
        st->battery_pct = live_pct;
        st->charging    = live_chg;
        return;
    }

    st->battery_pct = cfg->mock_battery;
    st->charging    = cfg->mock_charging;
}

bool osd_hw_poll_external_sliders(osd_hw_t *hw, osd_state_t *st, uint64_t now_ms) {
    bool vol_trig = false;
    bool brt_trig = false;
    struct stat sb;

    /* 1. Check if /run/kdm-osd/volume was written externally */
    if (stat("/run/kdm-osd/volume", &sb) == 0 &&
        timespec_changed(&sb.st_mtim, &hw->vol_mtime)) {
        hw->vol_mtime  = sb.st_mtim;
        int new_vol    = read_volume_pct_file(st->volume_pct);
        st->volume_pct = new_vol;
        st->qs_active[OSD_QS_SOUND] = (new_vol > 0);
        write_alsa_volume_pct(hw, new_vol);
        vol_trig = true;
    } else {
        /* Also check if ALSA mixer volume/mute was changed by external app (e.g. wpctl / alsamixer) */
        int alsa_pct = 0;
        bool alsa_unmuted = true;
        if (read_alsa_state(hw, &alsa_pct, &alsa_unmuted)) {
            if (hw->last_alsa_pct >= 0 &&
                (abs(alsa_pct - hw->last_alsa_pct) >= 2 || alsa_unmuted != hw->last_alsa_unmuted)) {
                hw->last_alsa_pct           = alsa_pct;
                hw->last_alsa_unmuted       = alsa_unmuted;
                st->volume_pct              = alsa_pct;
                st->qs_active[OSD_QS_SOUND] = (alsa_unmuted && alsa_pct > 0);
                if (alsa_pct > 0)
                    st->saved_vol_pct = alsa_pct;
                FILE *fp = fopen("/run/kdm-osd/volume", "w");
                if (fp) {
                    fprintf(fp, "%d\n", st->volume_pct);
                    fclose(fp);
                    record_file_mtime("/run/kdm-osd/volume", &hw->vol_mtime);
                }
                vol_trig = true;
            }
        }
    }

    /* 2. Check if /run/kdm-osd/brightness was written externally */
    if (stat("/run/kdm-osd/brightness", &sb) == 0 &&
        timespec_changed(&sb.st_mtim, &hw->brt_mtime)) {
        hw->brt_mtime      = sb.st_mtim;
        st->brightness_pct = read_brightness_pct(hw, st->brightness_pct);
        write_raw_backlight_from_pct(hw, st->brightness_pct);
        brt_trig = true;
    }

    /* 3. Check if non-DSI hardware /sys/class/backlight/.../brightness changed directly */
    if (!hw->is_dsi_backlight) {
        int raw = read_raw_backlight(hw);
        if (raw >= 0 && hw->last_hw_raw >= 0 && raw != hw->last_hw_raw) {
            hw->last_hw_raw    = raw;
            st->brightness_pct = raw_to_brightness_pct(hw, raw);
            FILE *ovr = fopen("/run/kdm-osd/brightness", "w");
            if (ovr) {
                fprintf(ovr, "%d\n", st->brightness_pct);
                fclose(ovr);
                record_file_mtime("/run/kdm-osd/brightness", &hw->brt_mtime);
            }
            brt_trig = true;
        }
    }

    if (vol_trig || brt_trig) {
        if (!st->shade_open) {
            if (vol_trig) {
                st->vol_open = true;
                st->brt_open = false;
            } else {
                st->brt_open = true;
                st->vol_open = false;
            }
        }
        st->dirty_kind          = OSD_DIRTY_FULL;
        st->sliders_activity_ms = now_ms;
        return true;
    }
    return false;
}

/*
 * osd_hw.h - OOP Hardware & System Status Service Class (OsdHardware)
 *
 * Encapsulates:
 *   - Real ALSA hardware mixer volume & mute control (snd_mixer_t / PCM dB scaling)
 *   - Real NetworkManager / rfkill Wi-Fi radio toggle & RSSI bar estimation
 *   - Backlight sysfs & runtime IPC files (/run/kdm-osd/{brightness,volume,battery})
 *   - System Clock & Date formatting ("1:14PM", "Wed, 4 Nov")
 *   - Quick Settings toggle button states
 */
#ifndef KDM_OSD_HW_H
#define KDM_OSD_HW_H

#include "osd_types.h"
#include <sys/stat.h>

typedef struct {
    char backlight_dir[256];
    int backlight_max;
    int last_hw_raw;
    int pending_brt_pct;
    uint64_t last_brt_write_ms;
    bool is_dsi_backlight;

    void *alsa_mixer;
    void *alsa_elem;
    long alsa_min_vol;
    long alsa_max_vol;
    long alsa_min_db;
    long alsa_max_db;
    bool alsa_has_db;
    int last_alsa_pct;
    bool last_alsa_unmuted;

    uint64_t wifi_toggle_grace_until_ms;

    int i2c_bat_fd;
    bool has_max17055;

    struct timespec vol_mtime;
    struct timespec brt_mtime;
    struct timespec bat_mtime;
} osd_hw_t;

void osd_hw_init(osd_hw_t *hw, const osd_config_t *cfg, osd_state_t *st);
void osd_hw_destroy(osd_hw_t *hw);
void osd_hw_update_status(osd_hw_t *hw, const osd_config_t *cfg, osd_state_t *st);
bool osd_hw_poll_external_sliders(osd_hw_t *hw, osd_state_t *st, uint64_t now_ms);
void osd_hw_set_brightness(osd_hw_t *hw, osd_state_t *st, int pct);
void osd_hw_flush_brightness(osd_hw_t *hw, const osd_state_t *st, bool force);
void osd_hw_set_volume(osd_hw_t *hw, osd_state_t *st, int pct);
void osd_hw_toggle_qs(osd_hw_t *hw, osd_state_t *st, osd_qs_id_t id);

#endif /* KDM_OSD_HW_H */

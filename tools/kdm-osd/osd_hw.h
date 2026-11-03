/*
 * osd_hw.h - OOP Hardware & System Status Service Class (OsdHardware)
 *
 * Encapsulates:
 *   - DSI Backlight sysfs discovery (/sys/class/backlight/) & raw-to-percent mapping
 *   - Runtime IPC files (/run/kdm-osd/{brightness,volume,battery})
 *   - Live Wi-Fi RSSI bar estimation (/proc/net/wireless)
 *   - System Clock & Date formatting ("1:14PM", "Tue, Dec 22")
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
    struct timespec vol_mtime;
    struct timespec brt_mtime;
} osd_hw_t;

void osd_hw_init(osd_hw_t *hw, const osd_config_t *cfg, osd_state_t *st);
void osd_hw_update_status(osd_hw_t *hw, const osd_config_t *cfg, osd_state_t *st);
bool osd_hw_poll_external_sliders(osd_hw_t *hw, osd_state_t *st, uint64_t now_ms);
void osd_hw_set_brightness(osd_hw_t *hw, osd_state_t *st, int pct);
void osd_hw_flush_brightness(osd_hw_t *hw, const osd_state_t *st, bool force);
void osd_hw_set_volume(osd_hw_t *hw, osd_state_t *st, int pct);
void osd_hw_toggle_qs(osd_hw_t *hw, osd_state_t *st, osd_qs_id_t id);

#endif /* KDM_OSD_HW_H */

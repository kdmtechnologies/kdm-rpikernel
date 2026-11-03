/*
 * osd_drm.h - OOP DRM/KMS Hardware Overlay Plane Presenter (OsdDrmPresenter)
 *
 * Encapsulates:
 *   - DRM CRTC & DRM_PLANE_TYPE_OVERLAY discovery
 *   - Full-screen (1080x1920 portrait / 1920x1080 landscape) DRM dumb buffer allocation
 *   - Active DRM master FD borrowing via pidfd_getfd
 *   - Partial dirty-rect rotated surface blitting
 *   - Zero-copy hardware plane cropping for 56Hz touch-following swipe animation
 */
#ifndef KDM_OSD_DRM_H
#define KDM_OSD_DRM_H

#include "osd_canvas.h"
#include <sys/types.h>

typedef struct {
    uint32_t handle;
    int dmabuf_fd;
    uint32_t *map;
    uint32_t cached_fb_id;
} osd_drm_buf_t;

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

    osd_drm_buf_t bufs[2];
    int front_idx;
    bool zpos_set;

    int cached_master_fd;
    pid_t cached_master_pid;
    uint32_t stale_fb_id;
} osd_drm_t;

bool osd_drm_init(osd_drm_t *drm, const char *explicit_card, int buf_w, int buf_h);
void osd_drm_present(osd_drm_t *drm, const osd_canvas_t *canvas,
                     const osd_config_t *cfg, osd_state_t *st,
                     const osd_rect_t *dirty);
void osd_drm_destroy(osd_drm_t *drm);

#endif /* KDM_OSD_DRM_H */

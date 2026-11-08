/*
 * osd_drm.c - Implementation of OsdDrmPresenter Class
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "osd_drm.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif
#ifndef SYS_pidfd_getfd
#define SYS_pidfd_getfd 438
#endif

static int sys_pidfd_open(pid_t pid, unsigned int flags) {
    return (int)syscall(SYS_pidfd_open, pid, flags);
}

static int sys_pidfd_getfd(int pidfd, int targetfd, unsigned int flags) {
    return (int)syscall(SYS_pidfd_getfd, pidfd, targetfd, flags);
}

static bool is_drm_master_fd(int fd) {
    struct drm_auth auth = { .magic = 0 };
    return (ioctl(fd, DRM_IOCTL_AUTH_MAGIC, &auth) == 0 || errno == EINVAL);
}

static int borrow_active_drm_master_fd(int card_minor, pid_t *out_pid) {
    char target_dev[64];
    snprintf(target_dev, sizeof(target_dev), "/dev/dri/card%d", card_minor);

    DIR *proc = opendir("/proc");
    if (!proc)
        return -1;

    pid_t self = getpid();
    struct dirent *pent;
    while ((pent = readdir(proc)) != NULL) {
        if (!isdigit((unsigned char)pent->d_name[0]))
            continue;
        pid_t pid = (pid_t)atoi(pent->d_name);
        if (pid <= 1 || pid == self)
            continue;

        char fd_dir_path[128];
        snprintf(fd_dir_path, sizeof(fd_dir_path), "/proc/%d/fd", (int)pid);
        DIR *fddir = opendir(fd_dir_path);
        if (!fddir)
            continue;

        struct dirent *fent;
        while ((fent = readdir(fddir)) != NULL) {
            if (!isdigit((unsigned char)fent->d_name[0]))
                continue;
            char link_path[300], link_target[256];
            snprintf(link_path, sizeof(link_path), "/proc/%d/fd/%.200s", (int)pid, fent->d_name);
            ssize_t len = readlink(link_path, link_target, sizeof(link_target) - 1);
            if (len <= 0)
                continue;
            link_target[len] = '\0';
            if (strcmp(link_target, target_dev) != 0)
                continue;

            int target_fd_num = atoi(fent->d_name);
            int pfd = sys_pidfd_open(pid, 0);
            if (pfd < 0)
                continue;
            int dup_fd = sys_pidfd_getfd(pfd, target_fd_num, 0);
            close(pfd);
            if (dup_fd < 0)
                continue;

            if (is_drm_master_fd(dup_fd)) {
                closedir(fddir);
                closedir(proc);
                if (out_pid)
                    *out_pid = pid;
                return dup_fd;
            }
            close(dup_fd);
        }
        closedir(fddir);
    }
    closedir(proc);
    return -1;
}

static void set_plane_zpos_offset(int drm_fd, uint32_t plane_id, int offset_from_max) {
    if (!plane_id)
        return;
    drmModeObjectPropertiesPtr props =
        drmModeObjectGetProperties(drm_fd, plane_id, DRM_MODE_OBJECT_PLANE);
    if (!props)
        return;
    for (uint32_t i = 0; i < props->count_props; i++) {
        drmModePropertyPtr prop = drmModeGetProperty(drm_fd, props->props[i]);
        if (!prop)
            continue;
        if (strcmp(prop->name, "zpos") == 0) {
            uint64_t min_z = 1;
            uint64_t max_z = 255;
            if ((prop->flags & DRM_MODE_PROP_RANGE) && prop->count_values >= 2) {
                min_z = prop->values[0];
                max_z = prop->values[1];
            }
            uint64_t target_z = (max_z > (uint64_t)offset_from_max + min_z)
                                ? (max_z - (uint64_t)offset_from_max)
                                : min_z;
            drmModeObjectSetProperty(drm_fd, plane_id, DRM_MODE_OBJECT_PLANE,
                                     prop->prop_id, target_z);
        }
        drmModeFreeProperty(prop);
    }
    drmModeFreeObjectProperties(props);
}

static bool allocate_single_dumb_buffer_ex(int fd, osd_drm_buf_t *buf, int buf_w, int buf_h,
                                           uint32_t *out_pitch, uint64_t *out_size) {
    struct drm_mode_create_dumb creq = {
        .width  = (uint32_t)buf_w,
        .height = (uint32_t)buf_h,
        .bpp    = 32,
    };
    if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0)
        return false;

    struct drm_mode_map_dumb mreq = { .handle = creq.handle };
    if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0)
        return false;

    void *map = mmap(NULL, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mreq.offset);
    if (map == MAP_FAILED)
        return false;

    int dmabuf_fd = -1;
    if (drmPrimeHandleToFD(fd, creq.handle, DRM_CLOEXEC | DRM_RDWR, &dmabuf_fd) < 0) {
        munmap(map, creq.size);
        return false;
    }

    if (out_pitch)
        *out_pitch = creq.pitch;
    if (out_size)
        *out_size = creq.size;
    buf->handle       = creq.handle;
    buf->dmabuf_fd    = dmabuf_fd;
    buf->map          = (uint32_t *)map;
    buf->cached_fb_id = 0;
    memset(buf->map, 0, creq.size);
    return true;
}

static bool allocate_dumb_buffers(osd_drm_t *drm, int buf_w, int buf_h) {
    drm->buf_w = buf_w;
    drm->buf_h = buf_h;
    if (!allocate_single_dumb_buffer_ex(drm->fd, &drm->bufs[0], buf_w, buf_h,
                                        &drm->pitch, &drm->size))
        return false;
    if (!allocate_single_dumb_buffer_ex(drm->fd, &drm->bufs[1], buf_w, buf_h,
                                        &drm->pitch, &drm->size)) {
        munmap(drm->bufs[0].map, drm->size);
        drm->bufs[0].map = NULL;
        close(drm->bufs[0].dmabuf_fd);
        drm->bufs[0].dmabuf_fd = -1;
        return false;
    }
    drm->front_idx = 0;

    if (drm->dim_plane_id != 0) {
        if (!allocate_single_dumb_buffer_ex(drm->fd, &drm->dim_bufs[0],
                                            OSD_DIM_BUF_SZ, OSD_DIM_BUF_SZ,
                                            &drm->dim_pitch, &drm->dim_size) ||
            !allocate_single_dumb_buffer_ex(drm->fd, &drm->dim_bufs[1],
                                            OSD_DIM_BUF_SZ, OSD_DIM_BUF_SZ,
                                            &drm->dim_pitch, &drm->dim_size)) {
            drm->dim_plane_id = 0;
        }
    }
    drm->dim_front_idx = 0;
    drm->last_dim_pct  = -1;
    return true;
}

bool osd_drm_init(osd_drm_t *drm, const char *explicit_card, int buf_w, int buf_h) {
    memset(drm, 0, sizeof(*drm));
    drm->fd                    = -1;
    drm->bufs[0].dmabuf_fd     = -1;
    drm->bufs[1].dmabuf_fd     = -1;
    drm->dim_bufs[0].dmabuf_fd = -1;
    drm->dim_bufs[1].dmabuf_fd = -1;
    drm->cached_master_fd      = -1;
    drm->cached_master_pid     = -1;
    drm->last_dim_pct          = -1;

    for (int i = 0; i < 4; i++) {
        char path[64];
        if (explicit_card)
            snprintf(path, sizeof(path), "%s", explicit_card);
        else
            snprintf(path, sizeof(path), "/dev/dri/card%d", i);

        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            if (explicit_card)
                break;
            continue;
        }
        drmDropMaster(fd);
        drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

        drmModeResPtr res = drmModeGetResources(fd);
        if (!res) {
            close(fd);
            if (explicit_card)
                break;
            continue;
        }

        uint32_t found_crtc = 0;
        int found_idx = -1, cw = 0, ch = 0;
        for (int c = 0; c < res->count_crtcs; c++) {
            drmModeCrtcPtr crtc = drmModeGetCrtc(fd, res->crtcs[c]);
            if (crtc && crtc->mode_valid && crtc->width > 0 && crtc->height > 0) {
                found_crtc = crtc->crtc_id;
                found_idx  = c;
                cw = (int)crtc->width;
                ch = (int)crtc->height;
                drmModeFreeCrtc(crtc);
                break;
            }
            if (crtc)
                drmModeFreeCrtc(crtc);
        }

        if (!found_crtc) {
            drmModeFreeResources(res);
            close(fd);
            if (explicit_card)
                break;
            continue;
        }

        drmModePlaneResPtr pres = drmModeGetPlaneResources(fd);
        uint32_t prev_plane    = 0;
        uint32_t prev_stale_fb = 0;
        uint32_t best_plane    = 0;
        uint32_t best_stale_fb = 0;
        if (pres) {
            for (uint32_t p = 0; p < pres->count_planes; p++) {
                drmModePlanePtr plane = drmModeGetPlane(fd, pres->planes[p]);
                if (!plane)
                    continue;
                bool crtc_ok = (plane->possible_crtcs & (1u << found_idx)) != 0;
                bool fmt_ok  = false;
                for (uint32_t f = 0; f < plane->count_formats; f++) {
                    if (plane->formats[f] == DRM_FORMAT_ARGB8888) {
                        fmt_ok = true;
                        break;
                    }
                }
                if (crtc_ok && fmt_ok) {
                    drmModeObjectPropertiesPtr props =
                        drmModeObjectGetProperties(fd, plane->plane_id, DRM_MODE_OBJECT_PLANE);
                    if (props) {
                        for (uint32_t k = 0; k < props->count_props; k++) {
                            drmModePropertyPtr prop = drmModeGetProperty(fd, props->props[k]);
                            if (prop) {
                                if (strcmp(prop->name, "type") == 0 &&
                                    props->prop_values[k] == DRM_PLANE_TYPE_OVERLAY) {
                                    prev_plane    = best_plane;
                                    prev_stale_fb = best_stale_fb;
                                    best_plane    = plane->plane_id;
                                    best_stale_fb = plane->fb_id;
                                }
                                drmModeFreeProperty(prop);
                            }
                        }
                        drmModeFreeObjectProperties(props);
                    }
                }
                drmModeFreePlane(plane);
            }
            drmModeFreePlaneResources(pres);
        }
        drmModeFreeResources(res);

        if (best_plane) {
            drm->fd         = fd;
            drm->card_minor = i;
            drm->crtc_id    = found_crtc;
            drm->crtc_index = found_idx;
            drm->crtc_w     = cw;
            drm->crtc_h     = ch;

            if (prev_plane != 0) {
                /* Use second-highest overlay plane for UI, and topmost plane for full-screen LCD dimmer */
                drm->plane_id        = prev_plane;
                drm->stale_fb_id     = prev_stale_fb;
                drm->dim_plane_id    = best_plane;
                drm->dim_stale_fb_id = best_stale_fb;
            } else {
                drm->plane_id        = best_plane;
                drm->stale_fb_id     = best_stale_fb;
                drm->dim_plane_id    = 0;
                drm->dim_stale_fb_id = 0;
            }

            if (!allocate_dumb_buffers(drm, buf_w, buf_h)) {
                close(drm->fd);
                drm->fd = -1;
                return false;
            }
            return true;
        }
        close(fd);
        if (explicit_card)
            break;
    }
    return false;
}

static void rotate_blit_rect(const osd_drm_t *drm, uint32_t *dst_map,
                             const osd_canvas_t *canvas, const osd_config_t *cfg,
                             const osd_rect_t *rect) {
    if (!dst_map || !canvas || !rect)
        return;
    int bw = canvas->width;
    int bh = canvas->height;
    int x0 = clampi(rect->x, 0, bw);
    int y0 = clampi(rect->y, 0, bh);
    int x1 = clampi(rect->x + rect->w, 0, bw);
    int y1 = clampi(rect->y + rect->h, 0, bh);
    if (x0 >= x1 || y0 >= y1)
        return;

    uint32_t stride_px = drm->pitch / 4;
    const uint32_t *src = canvas->pixels;

    if (cfg->rotate == 270) {
        for (int y = y0; y < y1; y++) {
            const uint32_t *src_row = &src[y * bw];
            int dst_x = y;
            for (int x = x0; x < x1; x++) {
                int dst_y = (bw - 1) - x;
                dst_map[dst_y * stride_px + dst_x] = src_row[x];
            }
        }
    } else if (cfg->rotate == 90) {
        for (int y = y0; y < y1; y++) {
            const uint32_t *src_row = &src[y * bw];
            int dst_x = (bh - 1) - y;
            for (int x = x0; x < x1; x++) {
                dst_map[x * stride_px + dst_x] = src_row[x];
            }
        }
    } else if (cfg->rotate == 180) {
        for (int y = y0; y < y1; y++) {
            const uint32_t *src_row = &src[y * bw];
            int dst_y = (bh - 1) - y;
            uint32_t *dst_row = &dst_map[dst_y * stride_px];
            for (int x = x0; x < x1; x++) {
                dst_row[(bw - 1) - x] = src_row[x];
            }
        }
    } else {
        for (int y = y0; y < y1; y++) {
            memcpy(&dst_map[y * stride_px + x0], &src[y * bw + x0], (size_t)(x1 - x0) * 4);
        }
    }
}

static void compute_plane_crop(const osd_drm_t *drm, const osd_config_t *cfg,
                               const osd_state_t *st,
                               int *crtc_x, int *crtc_y, uint32_t *crtc_w, uint32_t *crtc_h,
                               uint32_t *src_x, uint32_t *src_y, uint32_t *src_w, uint32_t *src_h) {
    int land_w = OSD_SCREEN_W;
    int land_h = OSD_SCREEN_H;

    if (st->shade_open || st->shade_pull_px > 0.5f) {
        int pull_h = clampi((int)roundf(st->shade_pull_px), 2, land_h);
        int off_y  = land_h - pull_h;

        if (cfg->rotate == 270) {
            *crtc_x = 0;
            *crtc_y = 0;
            *crtc_w = (uint32_t)pull_h;
            *crtc_h = (uint32_t)land_w;
            *src_x  = (uint32_t)off_y << 16;
            *src_y  = 0;
            *src_w  = (uint32_t)pull_h << 16;
            *src_h  = (uint32_t)land_w << 16;
        } else if (cfg->rotate == 90) {
            *crtc_x = drm->crtc_w - pull_h;
            *crtc_y = 0;
            *crtc_w = (uint32_t)pull_h;
            *crtc_h = (uint32_t)land_w;
            *src_x  = 0;
            *src_y  = 0;
            *src_w  = (uint32_t)pull_h << 16;
            *src_h  = (uint32_t)land_w << 16;
        } else if (cfg->rotate == 180) {
            *crtc_x = 0;
            *crtc_y = drm->crtc_h - pull_h;
            *crtc_w = (uint32_t)land_w;
            *crtc_h = (uint32_t)pull_h;
            *src_x  = 0;
            *src_y  = 0;
            *src_w  = (uint32_t)land_w << 16;
            *src_h  = (uint32_t)pull_h << 16;
        } else {
            *crtc_x = 0;
            *crtc_y = 0;
            *crtc_w = (uint32_t)land_w;
            *crtc_h = (uint32_t)pull_h;
            *src_x  = 0;
            *src_y  = (uint32_t)off_y << 16;
            *src_w  = (uint32_t)land_w << 16;
            *src_h  = (uint32_t)pull_h << 16;
        }
        return;
    }

    int active_w = (st->vol_open || st->brt_open) ? st->hud_w : st->bar_w;
    int active_h = (st->vol_open || st->brt_open) ? st->hud_h : (cfg->margin_top + st->bar_h);
    active_w = clampi(active_w, 16, land_w);
    active_h = clampi(active_h, 16, land_h);

    if (cfg->rotate == 270) {
        *crtc_x = 0;
        *crtc_y = 0;
        *crtc_w = (uint32_t)active_h;
        *crtc_h = (uint32_t)active_w;
        *src_x  = 0;
        *src_y  = 0;
        *src_w  = (uint32_t)active_h << 16;
        *src_h  = (uint32_t)active_w << 16;
    } else if (cfg->rotate == 90) {
        *crtc_x = drm->crtc_w - active_h;
        *crtc_y = drm->crtc_h - active_w;
        *crtc_w = (uint32_t)active_h;
        *crtc_h = (uint32_t)active_w;
        *src_x  = (uint32_t)(drm->buf_w - active_h) << 16;
        *src_y  = (uint32_t)(drm->buf_h - active_w) << 16;
        *src_w  = (uint32_t)active_h << 16;
        *src_h  = (uint32_t)active_w << 16;
    } else if (cfg->rotate == 180) {
        *crtc_x = 0;
        *crtc_y = drm->crtc_h - active_h;
        *crtc_w = (uint32_t)active_w;
        *crtc_h = (uint32_t)active_h;
        *src_x  = 0;
        *src_y  = (uint32_t)(drm->buf_h - active_h) << 16;
        *src_w  = (uint32_t)active_w << 16;
        *src_h  = (uint32_t)active_h << 16;
    } else {
        *crtc_x = drm->crtc_w - active_w;
        *crtc_y = 0;
        *crtc_w = (uint32_t)active_w;
        *crtc_h = (uint32_t)active_h;
        *src_x  = (uint32_t)(drm->buf_w - active_w) << 16;
        *src_y  = 0;
        *src_w  = (uint32_t)active_w << 16;
        *src_h  = (uint32_t)active_h << 16;
    }
}

static bool ensure_fb_registered_ex(int use_fd, bool borrowed, osd_drm_buf_t *buf,
                                    int w, int h, uint32_t pitch) {
    if (buf->cached_fb_id != 0)
        return true;
    uint32_t imported_handle = buf->handle;
    if (borrowed) {
        if (drmPrimeFDToHandle(use_fd, buf->dmabuf_fd, &imported_handle) < 0)
            return false;
    }
    uint32_t handles[4] = { imported_handle, 0, 0, 0 };
    uint32_t pitches[4] = { pitch, 0, 0, 0 };
    uint32_t offsets[4] = { 0, 0, 0, 0 };
    return (drmModeAddFB2(use_fd, (uint32_t)w, (uint32_t)h,
                          DRM_FORMAT_ARGB8888, handles, pitches, offsets,
                          &buf->cached_fb_id, 0) == 0);
}

static void update_hardware_lcd_dimmer(osd_drm_t *drm, int use_fd, bool borrowed, int brightness_pct) {
    if (!drm->dim_plane_id)
        return;

    int pct = clampi(brightness_pct, 0, 100);
    if (pct == drm->last_dim_pct)
        return;

    if (pct >= 100) {
        drmModeSetPlane(use_fd, drm->dim_plane_id, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        drm->last_dim_pct = 100;
    } else {
        /*
         * Map 99%..0% brightness to 2..216 alpha (up to 85% dimming at 0% brightness
         * so the LCD dims deeply while remaining readable to slide brightness back up).
         */
        uint8_t dim_alpha = (uint8_t)clampi(((100 - pct) * 216) / 100, 2, 216);
        uint32_t argb     = ((uint32_t)dim_alpha << 24);

        int d_idx = drm->dim_front_idx ^ 1;
        osd_drm_buf_t *dbuf = &drm->dim_bufs[d_idx];
        if (!dbuf->map)
            return;

        size_t total_px = (size_t)(drm->dim_size / 4);
        for (size_t i = 0; i < total_px; i++)
            dbuf->map[i] = argb;

        if (ensure_fb_registered_ex(use_fd, borrowed, dbuf,
                                    OSD_DIM_BUF_SZ, OSD_DIM_BUF_SZ, drm->dim_pitch)) {
            if (drmModeSetPlane(use_fd, drm->dim_plane_id, drm->crtc_id, dbuf->cached_fb_id, 0,
                                0, 0, (uint32_t)drm->crtc_w, (uint32_t)drm->crtc_h,
                                0, 0,
                                (uint32_t)OSD_DIM_BUF_SZ << 16,
                                (uint32_t)OSD_DIM_BUF_SZ << 16) == 0) {
                drm->dim_front_idx = d_idx;
                drm->last_dim_pct  = pct;
            }
        }
    }

    if (drm->dim_stale_fb_id != 0 &&
        drm->dim_stale_fb_id != drm->dim_bufs[0].cached_fb_id &&
        drm->dim_stale_fb_id != drm->dim_bufs[1].cached_fb_id &&
        drm->dim_stale_fb_id != drm->bufs[0].cached_fb_id &&
        drm->dim_stale_fb_id != drm->bufs[1].cached_fb_id) {
        drmModeRmFB(use_fd, drm->dim_stale_fb_id);
        drm->dim_stale_fb_id = 0;
    }
}

void osd_drm_present(osd_drm_t *drm, const osd_canvas_t *canvas,
                     const osd_config_t *cfg, osd_state_t *st,
                     const osd_rect_t *dirty) {
    bool zero_copy_pull = (st->dirty_kind == OSD_DIRTY_SHADE_PULL && st->shade_rendered);
    int target_idx      = zero_copy_pull ? drm->front_idx : (drm->front_idx ^ 1);
    osd_drm_buf_t *target = &drm->bufs[target_idx];
    if (!target->map)
        return;

    if (!zero_copy_pull && dirty && dirty->w > 0 && dirty->h > 0) {
        rotate_blit_rect(drm, target->map, canvas, cfg, dirty);
    }

    pid_t mpid = -1;
    int mfd = borrow_active_drm_master_fd(drm->card_minor, &mpid);
    if (mfd >= 0) {
        if (mpid != drm->cached_master_pid) {
            drm->cached_master_pid        = mpid;
            drm->bufs[0].cached_fb_id     = 0;
            drm->bufs[1].cached_fb_id     = 0;
            drm->dim_bufs[0].cached_fb_id = 0;
            drm->dim_bufs[1].cached_fb_id = 0;
            drm->zpos_set                 = false;
            drm->last_dim_pct             = -1;
        }
    } else if (drm->cached_master_pid != -1) {
        drm->cached_master_pid        = -1;
        drm->bufs[0].cached_fb_id     = 0;
        drm->bufs[1].cached_fb_id     = 0;
        drm->dim_bufs[0].cached_fb_id = 0;
        drm->dim_bufs[1].cached_fb_id = 0;
        drm->zpos_set                 = false;
        drm->last_dim_pct             = -1;
    }

    bool borrowed = (mfd >= 0);
    int use_fd    = borrowed ? mfd : drm->fd;

    if (!borrowed)
        drmSetMaster(drm->fd);

    if (ensure_fb_registered_ex(use_fd, borrowed, target, drm->buf_w, drm->buf_h, drm->pitch)) {
        if (!drm->zpos_set) {
            if (drm->dim_plane_id != 0) {
                set_plane_zpos_offset(use_fd, drm->plane_id, 1);
                set_plane_zpos_offset(use_fd, drm->dim_plane_id, 0);
            } else {
                set_plane_zpos_offset(use_fd, drm->plane_id, 0);
            }
            drm->zpos_set = true;
        }

        int crtc_x = 0, crtc_y = 0;
        uint32_t crtc_w = 0, crtc_h = 0;
        uint32_t src_x = 0, src_y = 0, src_w = 0, src_h = 0;
        compute_plane_crop(drm, cfg, st, &crtc_x, &crtc_y, &crtc_w, &crtc_h,
                           &src_x, &src_y, &src_w, &src_h);

        if (drmModeSetPlane(use_fd, drm->plane_id, drm->crtc_id, target->cached_fb_id, 0,
                            crtc_x, crtc_y, crtc_w, crtc_h,
                            src_x, src_y, src_w, src_h) < 0 && borrowed) {
            /* Master process reopened /dev/dri/cardN with a new drm_file; re-import FBs */
            drm->bufs[0].cached_fb_id     = 0;
            drm->bufs[1].cached_fb_id     = 0;
            drm->dim_bufs[0].cached_fb_id = 0;
            drm->dim_bufs[1].cached_fb_id = 0;
            drm->last_dim_pct             = -1;
            if (ensure_fb_registered_ex(use_fd, borrowed, target, drm->buf_w, drm->buf_h, drm->pitch)) {
                if (drm->dim_plane_id != 0) {
                    set_plane_zpos_offset(use_fd, drm->plane_id, 1);
                    set_plane_zpos_offset(use_fd, drm->dim_plane_id, 0);
                } else {
                    set_plane_zpos_offset(use_fd, drm->plane_id, 0);
                }
                drmModeSetPlane(use_fd, drm->plane_id, drm->crtc_id, target->cached_fb_id, 0,
                                crtc_x, crtc_y, crtc_w, crtc_h,
                                src_x, src_y, src_w, src_h);
            }
        }
        drm->front_idx = target_idx;

        if (drm->stale_fb_id != 0 &&
            drm->stale_fb_id != drm->bufs[0].cached_fb_id &&
            drm->stale_fb_id != drm->bufs[1].cached_fb_id) {
            drmModeRmFB(use_fd, drm->stale_fb_id);
            drm->stale_fb_id = 0;
        }
    }

    /* Update Hardware HVS Full-Screen LCD Dimmer Plane */
    update_hardware_lcd_dimmer(drm, use_fd, borrowed, st->brightness_pct);

    if (borrowed)
        close(mfd);
    else
        drmDropMaster(drm->fd);

    /* Keep secondary buffer synchronized when shade is active so partial dirty rects stay tear-free */
    if (!zero_copy_pull && dirty && dirty->w > 0 && dirty->h > 0 &&
        (st->shade_open || st->shade_pull_px > 0.5f)) {
        osd_drm_buf_t *other = &drm->bufs[target_idx ^ 1];
        if (other->map)
            rotate_blit_rect(drm, other->map, canvas, cfg, dirty);
        st->shade_rendered = true;
    } else if (!st->shade_open && st->shade_pull_px <= 0.5f) {
        st->shade_rendered = false;
    }
}

void osd_drm_destroy(osd_drm_t *drm) {
    if (!drm || drm->fd < 0)
        return;

    pid_t mpid = -1;
    int mfd = borrow_active_drm_master_fd(drm->card_minor, &mpid);
    bool borrowed = (mfd >= 0);
    int use_fd    = borrowed ? mfd : drm->fd;
    if (!borrowed)
        drmSetMaster(drm->fd);

    drmModeSetPlane(use_fd, drm->plane_id, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    if (drm->dim_plane_id)
        drmModeSetPlane(use_fd, drm->dim_plane_id, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);

    for (int i = 0; i < 2; i++) {
        if (drm->bufs[i].cached_fb_id != 0) {
            drmModeRmFB(use_fd, drm->bufs[i].cached_fb_id);
            drm->bufs[i].cached_fb_id = 0;
        }
        if (drm->dim_bufs[i].cached_fb_id != 0) {
            drmModeRmFB(use_fd, drm->dim_bufs[i].cached_fb_id);
            drm->dim_bufs[i].cached_fb_id = 0;
        }
    }
    if (borrowed)
        close(mfd);
    else
        drmDropMaster(drm->fd);
    for (int i = 0; i < 2; i++) {
        if (drm->bufs[i].map) {
            munmap(drm->bufs[i].map, drm->size);
            drm->bufs[i].map = NULL;
        }
        if (drm->bufs[i].dmabuf_fd >= 0) {
            close(drm->bufs[i].dmabuf_fd);
            drm->bufs[i].dmabuf_fd = -1;
        }
        if (drm->dim_bufs[i].map) {
            munmap(drm->dim_bufs[i].map, drm->dim_size);
            drm->dim_bufs[i].map = NULL;
        }
        if (drm->dim_bufs[i].dmabuf_fd >= 0) {
            close(drm->dim_bufs[i].dmabuf_fd);
            drm->dim_bufs[i].dmabuf_fd = -1;
        }
    }
    close(drm->fd);
    drm->fd = -1;
}

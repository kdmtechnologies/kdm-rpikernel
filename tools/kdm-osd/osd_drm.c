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
#include <sys/syscall.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#ifndef __NR_pidfd_open
#define __NR_pidfd_open 434
#endif
#ifndef __NR_pidfd_getfd
#define __NR_pidfd_getfd 438
#endif

static bool is_drm_master_fd(int fd) {
    errno = 0;
    if (drmAuthMagic(fd, 0) == 0)
        return true;
    return (errno == EINVAL);
}

static int borrow_active_drm_master_fd(int card_minor, pid_t *out_pid) {
    char target_suffix[32];
    snprintf(target_suffix, sizeof(target_suffix), "/dev/dri/card%d", card_minor);

    DIR *proc = opendir("/proc");
    if (!proc)
        return -1;

    pid_t self_pid = getpid();
    struct dirent *pent;
    while ((pent = readdir(proc)) != NULL) {
        if (!isdigit((unsigned char)pent->d_name[0]))
            continue;
        pid_t pid = (pid_t)atoi(pent->d_name);
        if (pid <= 1 || pid == self_pid)
            continue;

        char fd_dir_path[128];
        snprintf(fd_dir_path, sizeof(fd_dir_path), "/proc/%d/fd", pid);
        DIR *fddir = opendir(fd_dir_path);
        if (!fddir)
            continue;

        struct dirent *fent;
        while ((fent = readdir(fddir)) != NULL) {
            if (!isdigit((unsigned char)fent->d_name[0]))
                continue;
            char link_path[300], resolved[256];
            snprintf(link_path, sizeof(link_path), "/proc/%d/fd/%.200s", pid, fent->d_name);
            ssize_t len = readlink(link_path, resolved, sizeof(resolved) - 1);
            if (len <= 0)
                continue;
            resolved[len] = '\0';
            if (strcmp(resolved, target_suffix) != 0)
                continue;

            int target_fd = atoi(fent->d_name);
            int pidfd = (int)syscall(__NR_pidfd_open, pid, 0);
            if (pidfd < 0)
                continue;
            int dup_fd = (int)syscall(__NR_pidfd_getfd, pidfd, target_fd, 0);
            close(pidfd);
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

static void set_plane_zpos_max(int drm_fd, uint32_t plane_id) {
    drmModeObjectPropertiesPtr props =
        drmModeObjectGetProperties(drm_fd, plane_id, DRM_MODE_OBJECT_PLANE);
    if (!props)
        return;
    for (uint32_t i = 0; i < props->count_props; i++) {
        drmModePropertyPtr prop = drmModeGetProperty(drm_fd, props->props[i]);
        if (!prop)
            continue;
        if (strcmp(prop->name, "zpos") == 0) {
            uint64_t max_z = 255;
            if ((prop->flags & DRM_MODE_PROP_RANGE) && prop->count_values >= 2)
                max_z = prop->values[1];
            drmModeObjectSetProperty(drm_fd, plane_id, DRM_MODE_OBJECT_PLANE,
                                     prop->prop_id, max_z);
        }
        drmModeFreeProperty(prop);
    }
    drmModeFreeObjectProperties(props);
}

static bool allocate_single_dumb_buffer(osd_drm_t *drm, osd_drm_buf_t *buf, int buf_w, int buf_h) {
    struct drm_mode_create_dumb creq = {
        .width  = (uint32_t)buf_w,
        .height = (uint32_t)buf_h,
        .bpp    = 32,
    };
    if (drmIoctl(drm->fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0)
        return false;

    struct drm_mode_map_dumb mreq = { .handle = creq.handle };
    if (drmIoctl(drm->fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0)
        return false;

    void *map = mmap(NULL, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED, drm->fd, mreq.offset);
    if (map == MAP_FAILED)
        return false;

    int dmabuf_fd = -1;
    if (drmPrimeHandleToFD(drm->fd, creq.handle, DRM_CLOEXEC | DRM_RDWR, &dmabuf_fd) < 0) {
        munmap(map, creq.size);
        return false;
    }

    drm->buf_w        = buf_w;
    drm->buf_h        = buf_h;
    drm->pitch        = creq.pitch;
    drm->size         = creq.size;
    buf->handle       = creq.handle;
    buf->dmabuf_fd    = dmabuf_fd;
    buf->map          = (uint32_t *)map;
    buf->cached_fb_id = 0;
    memset(buf->map, 0, drm->size);
    return true;
}

static bool allocate_dumb_buffers(osd_drm_t *drm, int buf_w, int buf_h) {
    if (!allocate_single_dumb_buffer(drm, &drm->bufs[0], buf_w, buf_h))
        return false;
    if (!allocate_single_dumb_buffer(drm, &drm->bufs[1], buf_w, buf_h)) {
        munmap(drm->bufs[0].map, drm->size);
        drm->bufs[0].map = NULL;
        close(drm->bufs[0].dmabuf_fd);
        drm->bufs[0].dmabuf_fd = -1;
        return false;
    }
    drm->front_idx = 0;
    return true;
}

bool osd_drm_init(osd_drm_t *drm, const char *explicit_card, int buf_w, int buf_h) {
    memset(drm, 0, sizeof(*drm));
    drm->fd                = -1;
    drm->bufs[0].dmabuf_fd = -1;
    drm->bufs[1].dmabuf_fd = -1;
    drm->cached_master_fd  = -1;
    drm->cached_master_pid = -1;

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
            drm->fd          = fd;
            drm->card_minor  = i;
            drm->crtc_id     = found_crtc;
            drm->crtc_index  = found_idx;
            drm->crtc_w      = cw;
            drm->crtc_h      = ch;
            drm->plane_id    = best_plane;
            drm->stale_fb_id = best_stale_fb;

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
        /*
         * Full-Screen Quick Settings Shade (with vertical slide-down offset):
         * Display the bottom `pull_h` rows of the shade at the top `pull_h` rows
         * of the landscape screen so the shade physically slides down with the finger.
         */
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

    /* Compact Top-Right Status Bar (+ optional standalone HUD popup) */
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

static bool ensure_fb_registered(osd_drm_t *drm, int use_fd, bool borrowed, osd_drm_buf_t *buf) {
    if (buf->cached_fb_id != 0)
        return true;
    uint32_t imported_handle = buf->handle;
    if (borrowed) {
        if (drmPrimeFDToHandle(use_fd, buf->dmabuf_fd, &imported_handle) < 0)
            return false;
    }
    uint32_t handles[4] = { imported_handle, 0, 0, 0 };
    uint32_t pitches[4] = { drm->pitch, 0, 0, 0 };
    uint32_t offsets[4] = { 0, 0, 0, 0 };
    return (drmModeAddFB2(use_fd, (uint32_t)drm->buf_w, (uint32_t)drm->buf_h,
                          DRM_FORMAT_ARGB8888, handles, pitches, offsets,
                          &buf->cached_fb_id, 0) == 0);
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

    if (drm->cached_master_fd >= 0) {
        if (drm->cached_master_pid <= 0 || kill(drm->cached_master_pid, 0) != 0 ||
            !is_drm_master_fd(drm->cached_master_fd)) {
            close(drm->cached_master_fd);
            drm->cached_master_fd     = -1;
            drm->cached_master_pid    = -1;
            drm->bufs[0].cached_fb_id = 0;
            drm->bufs[1].cached_fb_id = 0;
            drm->zpos_set             = false;
        }
    }

    if (drm->cached_master_fd < 0) {
        pid_t mpid = -1;
        int mfd = borrow_active_drm_master_fd(drm->card_minor, &mpid);
        if (mfd >= 0) {
            drm->cached_master_fd     = mfd;
            drm->cached_master_pid    = mpid;
            drm->bufs[0].cached_fb_id = 0;
            drm->bufs[1].cached_fb_id = 0;
            drm->zpos_set             = false;
        }
    }

    bool borrowed = (drm->cached_master_fd >= 0);
    int use_fd    = borrowed ? drm->cached_master_fd : drm->fd;

    if (!borrowed)
        drmSetMaster(drm->fd);

    if (ensure_fb_registered(drm, use_fd, borrowed, target)) {
        if (!drm->zpos_set) {
            set_plane_zpos_max(use_fd, drm->plane_id);
            drm->zpos_set = true;
        }

        int crtc_x = 0, crtc_y = 0;
        uint32_t crtc_w = 0, crtc_h = 0;
        uint32_t src_x = 0, src_y = 0, src_w = 0, src_h = 0;
        compute_plane_crop(drm, cfg, st, &crtc_x, &crtc_y, &crtc_w, &crtc_h,
                           &src_x, &src_y, &src_w, &src_h);

        drmModeSetPlane(use_fd, drm->plane_id, drm->crtc_id, target->cached_fb_id, 0,
                        crtc_x, crtc_y, crtc_w, crtc_h,
                        src_x, src_y, src_w, src_h);
        drm->front_idx = target_idx;

        if (drm->stale_fb_id != 0 &&
            drm->stale_fb_id != drm->bufs[0].cached_fb_id &&
            drm->stale_fb_id != drm->bufs[1].cached_fb_id) {
            drmModeRmFB(use_fd, drm->stale_fb_id);
            drm->stale_fb_id = 0;
        }
    }

    if (!borrowed)
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

    bool borrowed = (drm->cached_master_fd >= 0);
    int use_fd    = borrowed ? drm->cached_master_fd : drm->fd;
    if (!borrowed)
        drmSetMaster(drm->fd);

    drmModeSetPlane(use_fd, drm->plane_id, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    for (int i = 0; i < 2; i++) {
        if (drm->bufs[i].cached_fb_id != 0) {
            drmModeRmFB(use_fd, drm->bufs[i].cached_fb_id);
            drm->bufs[i].cached_fb_id = 0;
        }
    }
    if (!borrowed)
        drmDropMaster(drm->fd);

    if (drm->cached_master_fd >= 0) {
        close(drm->cached_master_fd);
        drm->cached_master_fd = -1;
    }
    for (int i = 0; i < 2; i++) {
        if (drm->bufs[i].map) {
            munmap(drm->bufs[i].map, drm->size);
            drm->bufs[i].map = NULL;
        }
        if (drm->bufs[i].dmabuf_fd >= 0) {
            close(drm->bufs[i].dmabuf_fd);
            drm->bufs[i].dmabuf_fd = -1;
        }
    }
    close(drm->fd);
    drm->fd = -1;
}

/*
 * osd_view.h - OOP UI Layout & View Renderer Class
 */
#ifndef KDM_OSD_VIEW_H
#define KDM_OSD_VIEW_H

#include "osd_canvas.h"

void osd_view_compute_layout(const osd_config_t *cfg, osd_state_t *st, const osd_font_t *font);
void osd_view_render_into(osd_canvas_t *c, const osd_config_t *cfg,
                          osd_state_t *st, const osd_font_t *font,
                          osd_rect_t *out_dirty);
osd_canvas_t *osd_view_render(const osd_config_t *cfg, osd_state_t *st, const osd_font_t *font);

#endif /* KDM_OSD_VIEW_H */

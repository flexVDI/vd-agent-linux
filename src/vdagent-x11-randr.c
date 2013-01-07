/*  vdagent-x11-randr.c vdagent Xrandr integration code

    Copyright 2012 Red Hat, Inc.

    Red Hat Authors:
    Alon Levy <alevy@redhat.com>
    Hans de Goede <hdegoede@redhat.com>
    Marc-André Lureau <mlureau@redhat.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or   
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of 
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the  
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <string.h>
#include <syslog.h>
#include <stdlib.h>

#include <X11/extensions/Xinerama.h>

#include "vdagentd-proto.h"
#include "vdagent-x11.h"
#include "vdagent-x11-priv.h"

static int caught_error;
static int (*old_error_handler)(Display *, XErrorEvent *);

static int error_handler(Display *display, XErrorEvent *error)
{
    caught_error = 1;
    return 0;
}

static void arm_error_handler(struct vdagent_x11 *x11)
{
    caught_error = 0;
    XSync(x11->display, False);
    old_error_handler = XSetErrorHandler(error_handler);
}

static int check_error_handler(struct vdagent_x11 *x11)
{
    int error;

    XSync(x11->display, False);
    XSetErrorHandler(old_error_handler);
    error = caught_error;
    caught_error = 0;

    return error;
}

static XRRModeInfo *mode_from_id(struct vdagent_x11 *x11, int id)
{
    int i;

    for (i = 0 ; i < x11->randr.res->nmode ; ++i) {
        if (id == x11->randr.res->modes[i].id) {
            return &x11->randr.res->modes[i];
        }
    }
    return NULL;
}

static XRRCrtcInfo *crtc_from_id(struct vdagent_x11 *x11, int id)
{
    int i;

    for (i = 0 ; i < x11->randr.res->ncrtc ; ++i) {
        if (id == x11->randr.res->crtcs[i]) {
            return x11->randr.crtcs[i];
        }
    }
    return NULL;
}

static void free_randr_resources(struct vdagent_x11 *x11)
{
    int i;

    if (!x11->randr.res) {
        return;
    }
    if (x11->randr.outputs != NULL) {
        for (i = 0 ; i < x11->randr.res->noutput; ++i) {
            XRRFreeOutputInfo(x11->randr.outputs[i]);
        }
        free(x11->randr.outputs);
    }
    if (x11->randr.crtcs != NULL) {
        for (i = 0 ; i < x11->randr.res->ncrtc; ++i) {
            XRRFreeCrtcInfo(x11->randr.crtcs[i]);
        }
        free(x11->randr.crtcs);
    }
    XRRFreeScreenResources(x11->randr.res);
    x11->randr.res = NULL;
    x11->randr.outputs = NULL;
    x11->randr.crtcs = NULL;
}

static void update_randr_res(struct vdagent_x11 *x11)
{
    int i;

    free_randr_resources(x11);
    /* Note: we don't need XRRGetScreenResourcesCurrent since we cache the changes */
    x11->randr.res = XRRGetScreenResources(x11->display, x11->root_window);
    x11->randr.outputs = malloc(x11->randr.res->noutput * sizeof(*x11->randr.outputs));
    x11->randr.crtcs = malloc(x11->randr.res->ncrtc * sizeof(*x11->randr.crtcs));
    for (i = 0 ; i < x11->randr.res->noutput; ++i) {
        x11->randr.outputs[i] = XRRGetOutputInfo(x11->display, x11->randr.res,
                                                 x11->randr.res->outputs[i]);
    }
    for (i = 0 ; i < x11->randr.res->ncrtc; ++i) {
        x11->randr.crtcs[i] = XRRGetCrtcInfo(x11->display, x11->randr.res,
                                             x11->randr.res->crtcs[i]);
    }
    /* XXX is this dynamic? should it be cached? */
    if (XRRGetScreenSizeRange(x11->display, x11->root_window,
                              &x11->randr.min_width,
                              &x11->randr.min_height,
                              &x11->randr.max_width,
                              &x11->randr.max_height) != 1) {
        syslog(LOG_ERR, "update_randr_res: RRGetScreenSizeRange failed");
    }
}

void vdagent_x11_randr_init(struct vdagent_x11 *x11)
{
    int i;

    if (XRRQueryExtension(x11->display, &i, &i)) {
        x11->has_xrandr = 1;
        update_randr_res(x11);
        XRRQueryVersion(x11->display, &x11->xrandr_major, &x11->xrandr_minor);
    } else {
        x11->randr.res = NULL;
    }

    if (XineramaQueryExtension(x11->display, &i, &i))
        x11->has_xinerama = 1;

    switch (x11->has_xrandr << 4 | x11->has_xinerama) {
    case 0x00:
        syslog(LOG_ERR, "Neither Xrandr nor Xinerama found, assuming single monitor setup");
        break;
    case 0x01:
        if (x11->debug)
            syslog(LOG_DEBUG, "Found Xinerama extension without Xrandr, assuming Xinerama multi monitor setup");
        break;
    case 0x10:
        syslog(LOG_ERR, "Found Xrandr but no Xinerama, weird!");
        break;
    case 0x11:
        /* Standard xrandr setup, nothing to see here */
        break;
    }
}

static XRRModeInfo *
find_mode_by_name (struct vdagent_x11 *x11, char *name)
{
    int        	m;
    XRRModeInfo        *ret = NULL;

    for (m = 0; m < x11->randr.res->nmode; m++) {
        XRRModeInfo *mode = &x11->randr.res->modes[m];
        if (!strcmp (name, mode->name)) {
            ret = mode;
            break;
        }
    }
    return ret;
}

static XRRModeInfo *
find_mode_by_size (struct vdagent_x11 *x11, int width, int height)
{
    int        	m;
    XRRModeInfo        *ret = NULL;

    for (m = 0; m < x11->randr.res->nmode; m++) {
        XRRModeInfo *mode = &x11->randr.res->modes[m];
        if (mode->width == width && mode->height == height) {
            ret = mode;
            break;
        }
    }
    return ret;
}

static void delete_mode(struct vdagent_x11 *x11, int output_index, const char* name)
{
    int m;
    XRRModeInfo *mode;
    XRROutputInfo *output_info;
    XRRCrtcInfo *crtc_info;
    RRCrtc crtc;
    int current_mode = -1;

    if (x11->debug)
        syslog(LOG_DEBUG, "Deleting mode %s", name);

    output_info = x11->randr.outputs[output_index];
    if (output_info->ncrtc != 1) {
        syslog(LOG_ERR, "output has %d crtcs, expected exactly 1, "
               "failed to delete mode", output_info->ncrtc);
        return;
    }
    crtc_info = crtc_from_id(x11, output_info->crtcs[0]);
    current_mode = crtc_info->mode;
    crtc = output_info->crtc;
    for (m = 0 ; m < x11->randr.res->nmode; ++m) {
        mode = &x11->randr.res->modes[m];
        if (strcmp(mode->name, name) == 0)
            break;
    }
    if (m < x11->randr.res->nmode) {
        arm_error_handler(x11);
        XRRDeleteOutputMode (x11->display, x11->randr.res->outputs[output_index],
                             mode->id);
        XRRDestroyMode (x11->display, mode->id);
	// ignore race error, if mode is created by others
	check_error_handler(x11);
    }

    /* silly to update everytime for more then one monitor */
    update_randr_res(x11);
}

static void set_reduced_cvt_mode(XRRModeInfo *mode, int width, int height)
{
    /* Code taken from hw/xfree86/modes/xf86cvt.c
     * See that file for lineage. Originated in public domain code
     * Would be nice if xorg exported this in a library */

    /* 1) top/bottom margin size (% of height) - default: 1.8 */
#define CVT_MARGIN_PERCENTAGE 1.8

    /* 2) character cell horizontal granularity (pixels) - default 8 */
#define CVT_H_GRANULARITY 8

    /* 4) Minimum vertical porch (lines) - default 3 */
#define CVT_MIN_V_PORCH 3

    /* 4) Minimum number of vertical back porch lines - default 6 */
#define CVT_MIN_V_BPORCH 6

    /* Pixel Clock step (kHz) */
#define CVT_CLOCK_STEP 250

    /* Minimum vertical blanking interval time (µs) - default 460 */
#define CVT_RB_MIN_VBLANK 460.0

    /* Fixed number of clocks for horizontal sync */
#define CVT_RB_H_SYNC 32.0

    /* Fixed number of clocks for horizontal blanking */
#define CVT_RB_H_BLANK 160.0

    /* Fixed number of lines for vertical front porch - default 3 */
#define CVT_RB_VFPORCH 3

    int VBILines;
    float VFieldRate = 60.0;
    int VSync;
    float HPeriod;

    /* 2. Horizontal pixels */
    width = width - (width % CVT_H_GRANULARITY);

    mode->width = width;
    mode->height = height;
    VSync = 10;

    /* 8. Estimate Horizontal period. */
    HPeriod = ((float) (1000000.0 / VFieldRate - CVT_RB_MIN_VBLANK)) / height;

    /* 9. Find number of lines in vertical blanking */
    VBILines = ((float) CVT_RB_MIN_VBLANK) / HPeriod + 1;

    /* 10. Check if vertical blanking is sufficient */
    if (VBILines < (CVT_RB_VFPORCH + VSync + CVT_MIN_V_BPORCH))
        VBILines = CVT_RB_VFPORCH + VSync + CVT_MIN_V_BPORCH;

    /* 11. Find total number of lines in vertical field */
    mode->vTotal = height + VBILines;

    /* 12. Find total number of pixels in a line */
    mode->hTotal = mode->width + CVT_RB_H_BLANK;

    /* Fill in HSync values */
    mode->hSyncEnd = mode->width + CVT_RB_H_BLANK / 2;
    mode->hSyncStart = mode->hSyncEnd - CVT_RB_H_SYNC;

    /* Fill in VSync values */
    mode->vSyncStart = mode->height + CVT_RB_VFPORCH;
    mode->vSyncEnd = mode->vSyncStart + VSync;

    /* 15/13. Find pixel clock frequency (kHz for xf86) */
    mode->dotClock = mode->hTotal * 1000.0 / HPeriod;
    mode->dotClock -= mode->dotClock % CVT_CLOCK_STEP;

}

static XRRModeInfo *create_new_mode(struct vdagent_x11 *x11, int output_index,
                                    int width, int height)
{
    char modename[20];
    XRRModeInfo mode;

    snprintf(modename, sizeof(modename), "%dx%d-%d", width, height, output_index);
    mode.hSkew = 0;
    mode.name = modename;
    mode.nameLength = strlen(mode.name);
    set_reduced_cvt_mode(&mode, width, height);
    mode.modeFlags = 0;
    mode.id = 0;
    arm_error_handler(x11);
    XRRCreateMode (x11->display, x11->root_window, &mode);
    // ignore race error, if mode is created by others
    check_error_handler(x11);

    /* silly to update everytime for more then one monitor */
    update_randr_res(x11);

    return find_mode_by_name(x11, modename);
}

static int xrandr_add_and_set(struct vdagent_x11 *x11, int output, int x, int y,
                              int width, int height)
{
    XRRModeInfo *mode;
    int xid;
    Status s;
    RROutput outputs[1];
    char modename[20];

    if (!x11->randr.res || output >= x11->randr.res->noutput || output < 0) {
        syslog(LOG_ERR, "%s: program error: missing RANDR or bad output",
               __FUNCTION__);
        return 0;
    }
    if (x11->set_crtc_config_not_functional) {
        /* fail, set_best_mode will find something close. */
        return 0;
    }
    xid = x11->randr.res->outputs[output];
    mode = find_mode_by_size(x11, width, height);
    if (!mode) {
        mode = create_new_mode(x11, output, width, height);
    }
    if (!mode) {
        syslog(LOG_ERR, "failed to add a new mode");
        return 0;
    }
    XRRAddOutputMode(x11->display, xid, mode->id);
    outputs[0] = xid;
    s = XRRSetCrtcConfig(x11->display, x11->randr.res, x11->randr.res->crtcs[output],
                         CurrentTime, x, y, mode->id, RR_Rotate_0, outputs,
                         1);

    if (s != RRSetConfigSuccess) {
        syslog(LOG_ERR, "failed to XRRSetCrtcConfig");
        x11->set_crtc_config_not_functional = 1;
        return 0;
    }

    /* clean the previous name, if any */
    snprintf(modename, sizeof(modename), "%dx%d-%d", x11->width, x11->height, output);
    delete_mode(x11, output, modename);

    return 1;
}

static void xrandr_disable_output(struct vdagent_x11 *x11, int output)
{
    Status s;

    if (!x11->randr.res || output >= x11->randr.res->noutput || output < 0) {
        syslog(LOG_ERR, "%s: program error: missing RANDR or bad output",
               __FUNCTION__);
        return;
    }

    s = XRRSetCrtcConfig(x11->display, x11->randr.res,
                         x11->randr.res->crtcs[output],
                         CurrentTime, 0, 0, None, RR_Rotate_0,
                         NULL, 0);

    if (s != RRSetConfigSuccess)
        syslog(LOG_ERR, "failed to disable monitor");
}

static int set_screen_to_best_size(struct vdagent_x11 *x11, int width, int height,
                                   int *out_width, int *out_height){
    int i, num_sizes = 0;
    int best = -1;
    unsigned int closest_diff = -1;
    XRRScreenSize *sizes;
    XRRScreenConfiguration *config;
    Rotation rotation;

    sizes = XRRSizes(x11->display, x11->screen, &num_sizes);
    if (!sizes || !num_sizes) {
        syslog(LOG_ERR, "XRRSizes failed");
        return 0;
    }
    if (x11->debug)
        syslog(LOG_DEBUG, "set_screen_to_best_size found %d modes\n", num_sizes);

    /* Find the closest size which will fit within the monitor */
    for (i = 0; i < num_sizes; i++) {
        if (sizes[i].width  > width ||
            sizes[i].height > height)
            continue; /* Too large for the monitor */

        unsigned int wdiff = width  - sizes[i].width;
        unsigned int hdiff = height - sizes[i].height;
        unsigned int diff = wdiff * wdiff + hdiff * hdiff;
        if (diff < closest_diff) {
            closest_diff = diff;
            best = i;
        }
    }

    if (best == -1) {
        syslog(LOG_ERR, "no suitable resolution found for monitor");
        return 0;
    }

    config = XRRGetScreenInfo(x11->display, x11->root_window);
    if(!config) {
        syslog(LOG_ERR, "get screen info failed");
        return 0;
    }
    XRRConfigCurrentConfiguration(config, &rotation);
    XRRSetScreenConfig(x11->display, config, x11->root_window, best,
                       rotation, CurrentTime);
    XRRFreeScreenConfigInfo(config);

    if (x11->debug)
        syslog(LOG_DEBUG, "set_screen_to_best_size set size to: %dx%d\n",
               sizes[best].width, sizes[best].height);
    *out_width = sizes[best].width;
    *out_height = sizes[best].height;
    return 1;
}

void vdagent_x11_randr_handle_root_size_change(struct vdagent_x11 *x11,
                                               int width, int height)
{
    if (width == x11->width && height == x11->height) {
        return;
    }

    if (x11->debug)
        syslog(LOG_DEBUG, "Root size changed to %dx%d send %d",
               width, height, !x11->dont_send_guest_xorg_res);

    x11->width  = width;
    x11->height = height;
    if (!x11->dont_send_guest_xorg_res) {
        vdagent_x11_send_daemon_guest_xorg_res(x11);
    }
}

static uint32_t min_int(uint32_t x, uint32_t y)
{
    return x > y ? y : x;
}

static uint32_t max_int(uint32_t x, uint32_t y)
{
    return x > y ? x : y;
}

static int constrain_to_range(uint32_t low, uint32_t *val, uint32_t high)
{
    if (low <= *val && *val <= high) {
        return 0;
    }
    if (low > *val) {
        *val = low;
    }
    if (*val > high) {
        *val = high;
    }
    return 1;
}

static void constrain_to_screen(struct vdagent_x11 *x11, uint32_t *w, uint32_t *h)
{
    uint32_t lx, ly, hx, hy;
    uint32_t orig_h = *h;
    uint32_t orig_w = *w;

    lx = x11->randr.min_width;
    hx = x11->randr.max_width;
    ly = x11->randr.min_height;
    hy = x11->randr.max_height;
    if (constrain_to_range(lx, w, hx)) {
        syslog(LOG_ERR, "width not in driver range: ! %d < %d < %d",
               lx, orig_w, hx);
    }
    if (constrain_to_range(ly, h, hy)) {
        syslog(LOG_ERR, "height not in driver range: ! %d < %d < %d",
               ly, orig_h, hy);
    }
}

/*
 * The agent config doesn't contain a primary size, just the monitors, but
 * we need to total size as well, to make sure we have enough memory and
 * because X needs it.
 *
 * At the same pass constrain any provided size to what the server accepts.
 *
 * Exit axioms:
 *  x >= 0, y >= 0 for all x, y in mon_config
 *  max_width >= width >= min_width,
 *  max_height >= height >= min_height for all monitors in mon_config
 */
static void zero_base_monitors(struct vdagent_x11 *x11,
                               VDAgentMonitorsConfig *mon_config,
                               uint32_t *width, uint32_t *height)
{
    int i = 0;
    uint32_t min_x, min_y, max_x, max_y;
    uint32_t *mon_height, *mon_width;

    mon_config->monitors[0].x &= ~7;
    mon_config->monitors[0].width &= ~7;
    mon_width = &mon_config->monitors[i].width;
    mon_height = &mon_config->monitors[i].height;
    constrain_to_screen(x11, mon_width, mon_height);
    min_x = mon_config->monitors[0].x;
    min_y = mon_config->monitors[0].y;
    max_x = mon_config->monitors[0].width + min_x;
    max_y = mon_config->monitors[0].height + min_y;
    for (++i ; i < mon_config->num_of_monitors; ++i) {
        mon_config->monitors[i].x &= ~7;
        mon_config->monitors[i].width &= ~7;
        mon_width = &mon_config->monitors[i].width;
        mon_height = &mon_config->monitors[i].height;
        constrain_to_screen(x11, mon_width, mon_height);
        min_x = min_int(mon_config->monitors[i].x, min_x);
        min_y = min_int(mon_config->monitors[i].y, min_y);
        max_x = max_int(mon_config->monitors[i].x + *mon_width, max_x);
        max_y = max_int(mon_config->monitors[i].y + *mon_height, max_y);
    }
    if (min_x != 0 || min_y != 0) {
        syslog(LOG_ERR, "%s: agent config %d,%d rooted, adjusting to 0,0.",
               __FUNCTION__, min_x, min_y);
        for (i = 0 ; i < mon_config->num_of_monitors; ++i) {
            mon_config->monitors[i].x -= min_x;
            mon_config->monitors[i].y -= min_y;
        }
    }
    max_x -= min_x;
    max_y -= min_y;
    *width = max_x;
    *height = max_y;
}

static int same_monitor_configs(struct vdagent_x11 *x11, VDAgentMonitorsConfig *mon)
{
    int i;
    XRRModeInfo *mode;
    XRRCrtcInfo *crtc;
    VDAgentMonConfig *client_mode;
    XRRScreenResources *res = x11->randr.res;

    if (res->noutput > res->ncrtc) {
        syslog(LOG_ERR, "error: unexpected noutput > ncrtc in driver");
        return 0;
    }

    if (mon->num_of_monitors > res->noutput) {
        for (i = res->noutput; i < mon->num_of_monitors; i++) {
            if (mon->monitors[i].width || mon->monitors[i].height) {
                syslog(LOG_WARNING,
                       "warning: unexpected client request: #mon %d > driver output %d",
                       mon->num_of_monitors, res->noutput);
                break;
            }
        }
        mon->num_of_monitors = res->noutput;
    }

    for (i = 0 ; i < mon->num_of_monitors; ++i) {
        if (x11->randr.outputs[i]->ncrtc == 0) {
            return 0;
        }
        if (x11->randr.outputs[i]->ncrtc != 1) {
            syslog(LOG_ERR, "error: unexpected driver config, ncrtc %d != 1",
                   x11->randr.outputs[i]->ncrtc);
            return 0;
        }
        crtc = crtc_from_id(x11, x11->randr.outputs[i]->crtcs[0]);
        if (!crtc) {
            syslog(LOG_ERR, "error: inconsistent or stale data from X");
            return 0;
        }
        client_mode = &mon->monitors[i];
        mode = mode_from_id(x11, crtc->mode);
        if (!mode) {
            return 0;
        }
        /* NOTE: we don't compare depth. */
        /* NOTE 2: width set by X is a multiple of 8, so ignore lower 3 bits */
        if ((mode->width & ~7) != (client_mode->width & ~7) ||
            mode->height != client_mode->height ||
            (crtc->x & ~7) != (client_mode->x & ~7) ||
            crtc->y != client_mode->y) {
            return 0;
        }
    }
    return 1;
}

static void dump_monitors_config(struct vdagent_x11 *x11,
                                 VDAgentMonitorsConfig *mon_config,
                                 const char *prefix)
{
    int i;
    VDAgentMonConfig *m;

    syslog(LOG_DEBUG, "%s: %d, %x", prefix, mon_config->num_of_monitors,
           mon_config->flags);
    for (i = 0 ; i < mon_config->num_of_monitors; ++i) {
        m = &mon_config->monitors[i];
        syslog(LOG_DEBUG, "received monitor %d config %dx%d+%d+%d", i,
               m->width, m->height, m->x, m->y);
    }
}

/*
 * Set monitor configuration according to client request.
 *
 * On exit send current configuration to client, regardless of error.
 *
 * Errors:
 *  screen size too large for driver to handle. (we set the largest/smallest possible)
 *  no randr support in X server.
 *  invalid configuration request from client.
 */
void vdagent_x11_set_monitor_config(struct vdagent_x11 *x11,
                                    VDAgentMonitorsConfig *mon_config)
{
    int i;
    int width, height;
    int x, y;
    uint32_t primary_w, primary_h;

    if (!x11->has_xrandr)
        goto exit;

    if (mon_config->num_of_monitors < 1) {
        syslog(LOG_ERR, "client sent invalid monitor config number %d",
               mon_config->num_of_monitors);
        goto exit;
    }

    if (same_monitor_configs(x11, mon_config)) {
        goto exit;
    }

    if (x11->debug) {
        dump_monitors_config(x11, mon_config, "from guest");
    }

    zero_base_monitors(x11, mon_config, &primary_w, &primary_h);

    constrain_to_screen(x11, &primary_w, &primary_h);

    if (x11->debug) {
        dump_monitors_config(x11, mon_config, "after zeroing");
    }

    for (i = mon_config->num_of_monitors; i < x11->randr.res->noutput; i++)
        xrandr_disable_output(x11, i);

    for (i = 0; i < mon_config->num_of_monitors; ++i) {
        /* Try to create the requested resolution */
        width = mon_config->monitors[i].width;
        height = mon_config->monitors[i].height;
        x = mon_config->monitors[i].x;
        y = mon_config->monitors[i].y;
        if (!xrandr_add_and_set(x11, i, x, y, width, height) &&
                mon_config->num_of_monitors == 1) {
            set_screen_to_best_size(x11, width, height,
                                    &primary_w, &primary_h);
            goto update;
        }
    }

    if (primary_w != x11->width || primary_h != x11->height) {
        if (x11->debug)
            syslog(LOG_DEBUG, "Changing screen size to %dx%d",
                   primary_w, primary_h);
        arm_error_handler(x11);
        XRRSetScreenSize(x11->display, x11->root_window, primary_w, primary_h,
                         DisplayWidthMM(x11->display, x11->screen),
                         DisplayHeightMM(x11->display, x11->screen));
        if (check_error_handler(x11)) {
            syslog(LOG_ERR, "failed to XRRSetScreenSize");
            x11->set_crtc_config_not_functional = 1;
        }
    }

update:
    update_randr_res(x11);
    x11->width = primary_w;
    x11->height = primary_h;

    /* Flush output buffers and consume any pending events (ConfigureNotify) */
    x11->dont_send_guest_xorg_res = 1;
    vdagent_x11_do_read(x11);
    x11->dont_send_guest_xorg_res = 0;

exit:
    vdagent_x11_send_daemon_guest_xorg_res(x11);

    /* Flush output buffers and consume any pending events */
    vdagent_x11_do_read(x11);
}

void vdagent_x11_send_daemon_guest_xorg_res(struct vdagent_x11 *x11)
{
    struct vdagentd_guest_xorg_resolution *res = NULL;
    XineramaScreenInfo *screen_info = NULL;
    int i, screen_count = 0;

    if (x11->has_xinerama) {
        /* Xinerama reports the same information RANDR reports, so stay
         * with Xinerama for support of Xinerama only setups */
        screen_info = XineramaQueryScreens(x11->display, &screen_count);
    }

    if (screen_count == 0)
        screen_count = 1;

    res = malloc(screen_count * sizeof(*res));
    if (!res) {
        syslog(LOG_ERR, "out of memory while trying to send resolutions, not sending resolutions.");
        if (screen_info)
            XFree(screen_info);
        return;
    }

    if (screen_info) {
        for (i = 0; i < screen_count; i++) {
            if (screen_info[i].screen_number >= screen_count) {
                syslog(LOG_ERR, "Invalid screen number in xinerama screen info (%d >= %d)",
                       screen_info[i].screen_number, screen_count);
                XFree(screen_info);
                free(res);
                return;
            }
            res[screen_info[i].screen_number].width = screen_info[i].width;
            res[screen_info[i].screen_number].height = screen_info[i].height;
            res[screen_info[i].screen_number].x = screen_info[i].x_org;
            res[screen_info[i].screen_number].y = screen_info[i].y_org;
        }
        XFree(screen_info);
    } else {
        res[0].width  = x11->width;
        res[0].height = x11->height;
        res[0].x = 0;
        res[0].y = 0;
    }

    udscs_write(x11->vdagentd, VDAGENTD_GUEST_XORG_RESOLUTION, x11->width,
                x11->height, (uint8_t *)res, screen_count * sizeof(*res));
    free(res);
}

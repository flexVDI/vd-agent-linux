#include <string.h>
#include <stdlib.h>

#include <X11/extensions/Xinerama.h>

#include "vdagentd-proto.h"
#include "vdagent-x11-priv.h"

static int caught_error;
static int (*old_error_handler)(Display *, XErrorEvent *);

static int error_handler(Display *display, XErrorEvent *error)
{
    caught_error = 1;
}

static void arm_error_handler(struct vdagent_x11 *x11)
{
    caught_error = 0;
    XSync(x11->display, True);
    old_error_handler = XSetErrorHandler(error_handler);
}

static void check_error_handler(struct vdagent_x11 *x11)
{
    XSync(x11->display, False);
    XSetErrorHandler(old_error_handler);
    if (caught_error) {
        x11->set_crtc_config_not_functional = 1;
    }
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

static int update_randr_res(struct vdagent_x11 *x11)
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
        fprintf(x11->errfile, "RRGetScreenSizeRange failed\n");
    }
}

void vdagent_x11_randr_init(struct vdagent_x11 *x11)
{
    int i;

    if (XRRQueryExtension(x11->display, &i, &i)) {
        x11->has_xrandr = 1;
        if (!update_randr_res(x11)) {
            fprintf(x11->errfile, "get screen info failed\n");
        }
        XRRQueryVersion(x11->display, &x11->xrandr_major, &x11->xrandr_minor);
    } else {
        x11->randr.res = NULL;
    }

    if (XineramaQueryExtension(x11->display, &i, &i))
        x11->has_xinerama = 1;

    switch (x11->has_xrandr << 4 | x11->has_xinerama) {
    case 0x00:
        fprintf(x11->errfile, "Neither Xrandr nor Xinerama found, assuming single monitor setup\n");
        break;
    case 0x01:
        if (x11->verbose)
            fprintf(x11->errfile, "Found Xinerama extension without Xrandr, assuming a multi monitor setup\n");
        break;
    case 0x10:
        fprintf(x11->errfile, "Found Xrandr but no Xinerama, weird! Assuming a single monitor setup\n");
        break;
    case 0x11:
        /* Standard single monitor setup, nothing to see here */
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

static int mode_in_use(struct vdagent_x11 *x11, int xid, const char *name)
{
    int m;
    XRRModeInfo *mode;
    XRROutputInfo *output_info;
    int crtc;

    output_info = XRRGetOutputInfo(x11->display, x11->randr.res, xid);
    crtc = output_info->crtc;
    XRRFreeOutputInfo(output_info);
    if (!crtc) {
        return 0;
    }
    for (m = 0 ; m < x11->randr.res->nmode; ++m) {
        mode = &x11->randr.res->modes[m];
        if (strcmp(mode->name, name) == 0) {
            return 1;
        }
    }
    return 0;
}

static void delete_mode(struct vdagent_x11 *x11, int output_index, const char *name)
{
    int m;
    XRRModeInfo *mode;
    XRRModeInfo *the_mode;
    XRROutputInfo *output_info;
    XRRCrtcInfo *crtc_info;
    RRCrtc crtc;
    int current_mode = -1;

    output_info = x11->randr.outputs[output_index];
    if (output_info->ncrtc != 1) {
        fprintf(x11->errfile,
                "output has %d crtcs, expected exactly 1, "
                "failed to delete mode\n",
                output_info->ncrtc);
        return;
    }
    crtc_info = crtc_from_id(x11, output_info->crtcs[0]);
    current_mode = crtc_info->mode;
    crtc = output_info->crtc;
    the_mode = NULL;
    for (m = 0 ; m < x11->randr.res->nmode; ++m) {
        mode = &x11->randr.res->modes[m];
        if (strcmp(mode->name, name) == 0) {
            the_mode = mode;
            break;
        }
    }
    if (the_mode) {
        if (crtc && the_mode->id == current_mode) {
            fprintf(x11->errfile,
                    "delete_mode of in use mode, setting crtc to NULL mode\n");
            XRRSetCrtcConfig(x11->display, x11->randr.res, crtc,
                             CurrentTime, 0, 0, None, RR_Rotate_0, NULL, 0);
        }
        XRRDeleteOutputMode (x11->display, x11->randr.res->outputs[output_index],
                             mode->id);
        XRRDestroyMode (x11->display, mode->id);
    }
}

void set_reduced_cvt_mode(XRRModeInfo *mode, int width, int height)
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

    /* we may be using this mode from an old invocation - check first */
    snprintf(modename, sizeof(modename), "vdagent-%d", output_index);
    arm_error_handler(x11);
    delete_mode(x11, output_index, modename);
    check_error_handler(x11);
    if (x11->set_crtc_config_not_functional) {
        return NULL;
    }
    mode.hSkew = 0;
    mode.name = modename;
    mode.nameLength = strlen(mode.name);
    set_reduced_cvt_mode(&mode, width, height);
    mode.modeFlags = 0;
    mode.id = 0;

    XRRCreateMode (x11->display, x11->root_window, &mode);
    /* silly to update everytime for more then one monitor */
    if (!update_randr_res(x11)) {
        fprintf(x11->errfile, "get screen info failed\n");
    }
    return find_mode_by_name(x11, modename);
}

static int xrandr_add_and_set(struct vdagent_x11 *x11, int output, int x, int y,
                              int width, int height)
{
    XRRModeInfo *mode;
    int xid;
    Status s;
    RROutput outputs[1];

    if (!x11->randr.res || output >= x11->randr.res->noutput || output < 0) {
        fprintf(x11->errfile, "%s: program error: missing RANDR or bad output\n",
                __FUNCTION__);
        return 0;
    }
    if (x11->set_crtc_config_not_functional) {
        /* succeed without doing anything. set_best_mode will
         * find something close. */
        return 1;
    }
    xid = x11->randr.res->outputs[output];
    mode = find_mode_by_size(x11, width, height);
    if (!mode) {
        mode = create_new_mode(x11, output, width, height);
    }
    if (!mode) {
        fprintf(x11->errfile, "failed to add a new mode\n");
        return 0;
    }
    XRRAddOutputMode(x11->display, xid, mode->id); // Call this anyway?
    outputs[0] = xid;
    s = XRRSetCrtcConfig(x11->display, x11->randr.res, x11->randr.res->crtcs[output],
                         CurrentTime, x, y, mode->id, RR_Rotate_0, outputs,
                         1);
    if (s != RRSetConfigSuccess) {
        fprintf(x11->errfile, "failed to XRRSetCrtcConfig\n");
        // TODO - return crtc config to default
        return 0;
    }
    return 1;
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
        fprintf(x11->errfile, "XRRSizes failed\n");
        return 0;
    }

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
        fprintf(x11->errfile, "no suitable resolution found for monitor\n");
        return 0;
    }

    config = XRRGetScreenInfo(x11->display, x11->root_window);
    if(!config) {
        fprintf(x11->errfile, "get screen info failed\n");
        return 0;
    }
    XRRConfigCurrentConfiguration(config, &rotation);
    XRRSetScreenConfig(x11->display, config, x11->root_window, best,
                       rotation, CurrentTime);
    XRRFreeScreenConfigInfo(config);

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

    x11->width  = width;
    x11->height = height;
    if (!x11->dont_send_guest_xorg_res) {
        vdagent_x11_send_daemon_guest_xorg_res(x11);
    }
}

static int min_int(int x, int y)
{
    return x > y ? y : x;
}

static int max_int(int x, int y)
{
    return x > y ? x : y;
}

int constrain_to_range(int low, int *val, int high)
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

void constrain_to_screen(struct vdagent_x11 *x11, int *w, int *h)
{
    int lx, ly, hx, hy;
    int orig_h = *h;
    int orig_w = *w;

    lx = x11->randr.min_width;
    hx = x11->randr.max_width;
    ly = x11->randr.min_height;
    hy = x11->randr.max_height;
    if (constrain_to_range(lx, w, hx)) {
        fprintf(x11->errfile,
                "width not in driver range: ! %d < %d < %d\n",
               lx, orig_w, hx);
    }
    if (constrain_to_range(ly, h, hy)) {
        fprintf(x11->errfile,
                "height not in driver range: ! %d < %d < %d\n",
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
                               int *width, int *height)
{
    int i = 0;
    int min_x;
    int min_y;
    int max_x;
    int max_y;
    int *mon_height;
    int *mon_width;

    mon_width = &mon_config->monitors[i].width;
    mon_height = &mon_config->monitors[i].height;
    constrain_to_screen(x11, mon_width, mon_height);
    min_x = mon_config->monitors[0].x;
    min_y = mon_config->monitors[0].y;
    max_x = mon_config->monitors[0].width + min_x;
    max_y = mon_config->monitors[0].height + min_y;
    for (++i ; i < mon_config->num_of_monitors; ++i) {
        mon_width = &mon_config->monitors[i].width;
        mon_height = &mon_config->monitors[i].height;
        constrain_to_screen(x11, mon_width, mon_height);
        min_x = min_int(mon_config->monitors[i].x, min_x);
        min_y = min_int(mon_config->monitors[i].y, min_y);
        max_x = max_int(mon_config->monitors[i].x + *mon_width, max_x);
        max_y = max_int(mon_config->monitors[i].y + *mon_height, max_y);
    }
    if (min_x != 0 || min_y != 0) {
        fprintf(x11->errfile,
                "%s: agent config %d,%d rooted, adjusting to 0,0.\n",
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

int same_monitor_configs(struct vdagent_x11 *x11, VDAgentMonitorsConfig *mon)
{
    int i;
    XRRModeInfo *mode;
    XRRCrtcInfo *crtc;
    VDAgentMonConfig *client_mode;
    XRRScreenResources *res = x11->randr.res;

    if (res->noutput > res->ncrtc) {
        fprintf(x11->errfile, "error: unexpected noutput > ncrtc in driver\n");
        return 0;
    }

    if (mon->num_of_monitors > res->noutput) {
        fprintf(x11->errfile, "error: unexpected client request: "
                              "#mon %d > driver output %d\n",
                mon->num_of_monitors, res->noutput);
        return 0;
    }

    for (i = 0 ; i < mon->num_of_monitors; ++i) {
        if (x11->randr.outputs[i]->ncrtc == 0) {
            return 0;
        }
        if (x11->randr.outputs[i]->ncrtc != 1) {
            fprintf(x11->errfile,
                    "error: unexpected driver config, ncrtc %d != 1",
                    x11->randr.outputs[i]->ncrtc);
            return 0;
        }
        crtc = crtc_from_id(x11, x11->randr.outputs[i]->crtcs[0]);
        if (!crtc) {
            fprintf(x11->errfile, "error: inconsistent or stale data from X\n");
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
            crtc->x != client_mode->x ||
            crtc->y != client_mode->y) {
            return 0;
        }
    }
    return 1;
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
    int primary_w, primary_h;
    Status s;

    if (!x11->has_xrandr)
        goto exit;

    if (mon_config->num_of_monitors < 1) {
        fprintf(x11->errfile, "client sent invalid monitor config number %d\n",
                mon_config->num_of_monitors);
        goto exit;
    }

    if (same_monitor_configs(x11, mon_config)) {
        goto exit;
    }

    zero_base_monitors(x11, mon_config, &primary_w, &primary_h);

    constrain_to_screen(x11, &primary_w, &primary_h);
    /*
     * Set screen size once now. If the screen size is reduced it may (will
     * probably) invalidate a currently set crtc mode, so disable crtcs first.
     */
    if (x11->width > primary_w || x11->height > primary_h) {
        for (i = 0 ; i < x11->randr.res->ncrtc; ++i) {
            /* This can fail if we are not in vt, xserver/RRCrtcSet checks
             * vtSema */
            s = XRRSetCrtcConfig(x11->display, x11->randr.res,
                                 x11->randr.res->crtcs[i],
                                 CurrentTime, 0, 0, None, RR_Rotate_0,
                                 NULL, 0);
            if (s != RRSetConfigSuccess) {
                goto exit;
            }
        }
    }
    if (primary_w != x11->width || primary_h != x11->height) {
        arm_error_handler(x11);
        XRRSetScreenSize(x11->display, x11->root_window, primary_w, primary_h,
                         DisplayWidthMM(x11->display, x11->screen),
                         DisplayHeightMM(x11->display, x11->screen));
        check_error_handler(x11);
    }
    for (i = 0; i < mon_config->num_of_monitors; ++i) {
        /* Try to create the requested resolution */
        width = mon_config->monitors[i].width;
        height = mon_config->monitors[i].height;
        x = mon_config->monitors[i].x;
        y = mon_config->monitors[i].y;
        if (!xrandr_add_and_set(x11, i, x, y, width, height) &&
            mon_config->num_of_monitors == 1) {
            set_screen_to_best_size(x11, width, height, &width, &height);
        }
    }
    if (!update_randr_res(x11)) {
        fprintf(x11->errfile, "get screen info failed\n");
    }
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
        fprintf(x11->errfile, "out of memory while trying to send resolutions, not sending resolutions.\n");
        if (screen_info)
            XFree(screen_info);
        return;
    }

    if (screen_info) {
        for (i = 0; i < screen_count; i++) {
            if (screen_info[i].screen_number >= screen_count) {
                fprintf(x11->errfile, "Invalid screen number in xinerama screen info (%d >= %d)\n",
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

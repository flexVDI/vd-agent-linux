#include <string.h>
#include <stdlib.h>

#include <X11/extensions/Xinerama.h>

#include "vdagentd-proto.h"
#include "vdagent-x11-priv.h"

void vdagent_x11_randr_init(struct vdagent_x11 *x11)
{
    int i;

    if (XRRQueryExtension(x11->display, &i, &i)) {
        x11->has_xrandr = 1;
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

void vdagent_x11_randr_handle_root_size_change(struct vdagent_x11 *x11,
                                               int width, int height)
{
    if (width == x11->width && height == x11->height) {
        return;
    }

    x11->width  = width;
    x11->height = height;

    vdagent_x11_send_daemon_guest_xorg_res(x11);
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
    int i, num_sizes = 0;
    int best = -1;
    unsigned int closest_diff = -1;
    XRRScreenSize *sizes;
    XRRScreenConfiguration *config;
    Rotation rotation;

    if (!x11->has_xrandr)
        return;

    if (mon_config->num_of_monitors != 1) {
        fprintf(x11->errfile,
                "Only 1 monitor supported, ignoring additional monitors\n");
    }

    sizes = XRRSizes(x11->display, x11->screen, &num_sizes);
    if (!sizes || !num_sizes) {
        fprintf(x11->errfile, "XRRSizes failed\n");
        return;
    }

    /* Find the closest size which will fit within the monitor */
    for (i = 0; i < num_sizes; i++) {
        if (sizes[i].width  > mon_config->monitors[0].width ||
            sizes[i].height > mon_config->monitors[0].height)
            continue; /* Too large for the monitor */

        unsigned int wdiff = mon_config->monitors[0].width  - sizes[i].width;
        unsigned int hdiff = mon_config->monitors[0].height - sizes[i].height;
        unsigned int diff = wdiff * wdiff + hdiff * hdiff;
        if (diff < closest_diff) {
            closest_diff = diff;
            best = i;
        }
    }

    if (best == -1) {
        fprintf(x11->errfile, "no suitable resolution found for monitor\n");
        return;
    }

    config = XRRGetScreenInfo(x11->display, x11->root_window);
    if(!config) {
        fprintf(x11->errfile, "get screen info failed\n");
        return;
    }
    XRRConfigCurrentConfiguration(config, &rotation);
    XRRSetScreenConfig(x11->display, config, x11->root_window, best,
                       rotation, CurrentTime);
    XRRFreeScreenConfigInfo(config);
    x11->width = sizes[best].width;
    x11->height = sizes[best].height;
    vdagent_x11_send_daemon_guest_xorg_res(x11);

    /* Flush output buffers and consume any pending events (ConfigureNotify) */
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

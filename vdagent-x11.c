/*  vdagent-x11.c vdagent x11 code

    Copyright 2010 Red Hat, Inc.

    Red Hat Authors:
    Hans de Goede <hdegoede@redhat.com>

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

/* Note *all* X11 calls in this file which do not wait for a result must be
   followed by an XFlush, given that the X11 code pumping the event loop
   (and thus flushing queued writes) is only called when there is data to be
   read from the X11 socket. */

#include <stdio.h>
#include <stdlib.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
#include "vdagentd-proto.h"
#include "vdagent-x11.h"

struct vdagent_x11 {
    Display *display;
    struct udscs_connection *vdagentd;
    int verbose;
    int fd;
    int screen;
    int root_window;
    int width;
    int height;
    int has_xrandr;
};

static void vdagent_x11_send_guest_xorg_res(struct vdagent_x11 *x11);

struct vdagent_x11 *vdagent_x11_create(struct udscs_connection *vdagentd,
    int verbose)
{
    struct vdagent_x11 *x11;
    XWindowAttributes attrib;
    int xrandr_event_base, xrandr_error_base;

    x11 = calloc(1, sizeof(*x11));
    if (!x11) {
        fprintf(stderr, "out of memory allocating vdagent_x11 struct\n");
        return NULL;
    }
    
    x11->vdagentd = vdagentd;
    x11->verbose = verbose;

    x11->display = XOpenDisplay(NULL);
    if (!x11->display) {
        fprintf(stderr, "could not connect to X-server\n");
        free(x11);
        return NULL;
    }

    x11->screen = DefaultScreen(x11->display);
    x11->root_window = RootWindow(x11->display, x11->screen);
    x11->fd = ConnectionNumber(x11->display);

    if (XRRQueryExtension(x11->display, &xrandr_event_base, &xrandr_error_base))
        x11->has_xrandr = 1;
    else
        fprintf(stderr, "no xrandr\n");

    XSelectInput(x11->display, x11->root_window, StructureNotifyMask);
    XGetWindowAttributes(x11->display, x11->root_window, &attrib);

    x11->width = attrib.width;
    x11->height = attrib.height;

    vdagent_x11_send_guest_xorg_res(x11);

    return x11;
}

void vdagent_x11_destroy(struct vdagent_x11 *x11)
{
    if (!x11)
        return;

    XCloseDisplay(x11->display);
    free(x11);
}

int vdagent_x11_get_fd(struct vdagent_x11 *x11)
{
    return x11->fd;
}

void vdagent_x11_do_read(struct vdagent_x11 *x11)
{
    XEvent event;
    int handled = 0;

    if (!XPending(x11->display))
        return;

    XNextEvent(x11->display, &event);
    switch (event.type) {
    case ConfigureNotify:
        if (event.xconfigure.window != x11->root_window)
            break;

        handled = 1;

        if (event.xconfigure.width  == x11->width &&
            event.xconfigure.height == x11->height)
            break;

        x11->width  = event.xconfigure.width;
        x11->height = event.xconfigure.height;

        vdagent_x11_send_guest_xorg_res(x11);
        break;
    }
    if (!handled && x11->verbose)
        fprintf(stderr, "unhandled x11 event, type %d, window %d\n",
                (int)event.type, (int)event.xany.window);
}

static void vdagent_x11_send_guest_xorg_res(struct vdagent_x11 *x11)
{
    struct vdagentd_guest_xorg_resolution res;
    struct udscs_message_header header;

    header.type = VDAGENTD_GUEST_XORG_RESOLUTION;
    header.opaque = 0;
    header.size = sizeof(res);

    res.width  = x11->width;
    res.height = x11->height;

    udscs_write(x11->vdagentd, &header, (uint8_t *)&res);
}

void vdagent_x11_set_monitor_config(struct vdagent_x11 *x11,
                                    VDAgentMonitorsConfig *mon_config)
{
    int i, num_sizes = 0;
    int best = -1;
    unsigned int closest_diff = -1;
    XRRScreenSize* sizes;
    XRRScreenConfiguration* config;
    Rotation rotation;

    if (!x11->has_xrandr)
        return;

    if (mon_config->num_of_monitors != 1) {
        fprintf(stderr, "Only 1 monitor supported, ignoring monitor config\n");
        return;
    }

    sizes = XRRSizes(x11->display, x11->screen, &num_sizes);
    if (!sizes || !num_sizes) {
        fprintf(stderr, "XRRSizes failed\n");
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
        fprintf(stderr, "no suitable resolution found for monitor\n");
        return;
    }

    config = XRRGetScreenInfo(x11->display, x11->root_window);
    if(!config) {
        fprintf(stderr, "get screen info failed\n");
        return;
    }
    XRRConfigCurrentConfiguration(config, &rotation);
    XRRSetScreenConfig(x11->display, config, x11->root_window, best,
                       rotation, CurrentTime);
    XRRFreeScreenConfigInfo(config);
    XFlush(x11->display);
}

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
#include <limits.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/Xfixes.h>
#include "vdagentd-proto.h"
#include "vdagent-x11.h"

const char *utf8_atom_names[] = {
    "UTF8_STRING",
    "text/plain;charset=UTF-8",
    "text/plain;charset=utf-8",
};

struct vdagent_x11 {
    Display *display;
    Atom clipboard_atom;
    Atom targets_atom;
    Atom incr_atom;
    Atom utf8_atoms[sizeof(utf8_atom_names)/sizeof(utf8_atom_names[0])];
    Window root_window;
    Window selection_window;
    struct udscs_connection *vdagentd;
    int verbose;
    int fd;
    int screen;
    int width;
    int height;
    int has_xrandr;
    int has_xfixes;
    int xfixes_event_base;
};

static void vdagent_x11_send_daemon_guest_xorg_res(struct vdagent_x11 *x11);
static void vdagent_x11_handle_selection_notify(struct vdagent_x11 *x11,
                                                XEvent *event);

struct vdagent_x11 *vdagent_x11_create(struct udscs_connection *vdagentd,
    int verbose)
{
    struct vdagent_x11 *x11;
    XWindowAttributes attrib;
    int i, major, minor;

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
    x11->clipboard_atom = XInternAtom(x11->display, "CLIPBOARD", False);
    x11->targets_atom = XInternAtom(x11->display, "TARGETS", False);
    x11->incr_atom = XInternAtom(x11->display, "INCR", False);
    for(i = 0; i < sizeof(utf8_atom_names)/sizeof(utf8_atom_names[0]); i++)
        x11->utf8_atoms[i] = XInternAtom(x11->display, utf8_atom_names[i],
                                         False);

    /* We should not store properties (for selections) on the root window */
    x11->selection_window = XCreateSimpleWindow(x11->display, x11->root_window,
                                                0, 0, 1, 1, 0, 0, 0);

    if (XRRQueryExtension(x11->display, &i, &i))
        x11->has_xrandr = 1;
    else
        fprintf(stderr, "no xrandr\n");

    if (XFixesQueryExtension(x11->display, &x11->xfixes_event_base, &i) &&
        XFixesQueryVersion(x11->display, &major, &minor) && major >= 1) {
        x11->has_xfixes = 1;
        XFixesSelectSelectionInput(x11->display, x11->root_window,
                                   x11->clipboard_atom,
                                   XFixesSetSelectionOwnerNotifyMask);
    } else
        fprintf(stderr, "no xfixes, no guest -> client copy paste support\n");

    /* Catch resolution changes */
    XSelectInput(x11->display, x11->root_window, StructureNotifyMask);

    /* Get the current resolution */
    XGetWindowAttributes(x11->display, x11->root_window, &attrib);
    x11->width = attrib.width;
    x11->height = attrib.height;
    vdagent_x11_send_daemon_guest_xorg_res(x11);

    /* No need for XFlush as XGetWindowAttributes does an implicit Xflush */

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
    if (event.type == x11->xfixes_event_base) {
        union {
            XEvent ev;
            XFixesSelectionNotifyEvent xfev;
        } ev;

        ev.ev = event;
        if (ev.xfev.subtype != XFixesSetSelectionOwnerNotify) {
            if (x11->verbose)
                fprintf(stderr, "unexpected xfix event subtype %d window %d\n",
                        (int)ev.xfev.subtype, (int)event.xany.window);
            return;
        }

        /* FIXME plenty, we need to request the selection with a type of
           TARGETS and then compare the TARGETS against our list of known
           UTF-8 targets. */
        udscs_write(x11->vdagentd, VDAGENTD_CLIPBOARD_GRAB,
                    VD_AGENT_CLIPBOARD_UTF8_TEXT, NULL, 0);
        handled = 1;
    } else switch (event.type) {
    case ConfigureNotify:
        if (event.xconfigure.window != x11->root_window)
            break;

        handled = 1;

        if (event.xconfigure.width  == x11->width &&
            event.xconfigure.height == x11->height)
            break;

        x11->width  = event.xconfigure.width;
        x11->height = event.xconfigure.height;

        vdagent_x11_send_daemon_guest_xorg_res(x11);
        break;
    case SelectionNotify:
        handled = 1;

        if (event.xselection.target == x11->targets_atom) {
            /* FIXME */
        } else {
            vdagent_x11_handle_selection_notify(x11, &event);
        }
        break;
    }
    if (!handled && x11->verbose)
        fprintf(stderr, "unhandled x11 event, type %d, window %d\n",
                (int)event.type, (int)event.xany.window);
}

static void vdagent_x11_send_daemon_guest_xorg_res(struct vdagent_x11 *x11)
{
    struct vdagentd_guest_xorg_resolution res;

    res.width  = x11->width;
    res.height = x11->height;

    udscs_write(x11->vdagentd, VDAGENTD_GUEST_XORG_RESOLUTION, 0,
                (uint8_t *)&res, sizeof(res));
}

static void vdagent_x11_handle_selection_notify(struct vdagent_x11 *x11,
                                                XEvent *event)
{
    Atom type;
    int format;
    unsigned long len, remain;
    unsigned char *data = NULL;

    if (XGetWindowProperty(x11->display, x11->selection_window,
                           x11->clipboard_atom, 0, LONG_MAX, True,
                           event->xselection.target, &type, &format, &len,
                           &remain, &data) != Success) {
        fprintf(stderr, "XGetWindowProperty(size) failed\n");
        goto error;
    }

    if (type == x11->incr_atom) {
        /* FIXME */
        fprintf(stderr, "incr properties are currently unsupported\n");
        goto error;
    }

    if (type != event->xselection.target) {
        fprintf(stderr, "expected property type: %s, got: %s\n",
                XGetAtomName(x11->display, event->xselection.target),
                XGetAtomName(x11->display, type));
        goto error;
    }

    if (format != 8) {
        fprintf(stderr, "only 8 bit formats are supported\n");
        goto error;
    }

    if (len == 0) {
        fprintf(stderr, "property contains no data (zero length)\n");
        goto error;
    }

    /* FIXME don't hardcode VD_AGENT_CLIPBOARD_UTF8_TEXT */
    udscs_write(x11->vdagentd, VDAGENTD_CLIPBOARD_DATA,
                VD_AGENT_CLIPBOARD_UTF8_TEXT, data, len);
    XFree(data);
    return;

error:
    if (data)
        XFree(data);
    /* Notify the spice client that no answer is forthcoming */
    udscs_write(x11->vdagentd, VDAGENTD_CLIPBOARD_DATA,
                VD_AGENT_CLIPBOARD_NONE, NULL, 0);
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

void vdagent_x11_clipboard_request(struct vdagent_x11 *x11, uint32_t type)
{
    /* FIXME use atom which we figured out matches one of our supported
       types when we first got notified of a selection owner change */
    Atom target = x11->utf8_atoms[0];

    XConvertSelection(x11->display, x11->clipboard_atom, target,
                      x11->clipboard_atom, x11->selection_window, CurrentTime);
    XFlush(x11->display);
}

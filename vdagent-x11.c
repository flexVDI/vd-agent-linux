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
#include <string.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/Xfixes.h>
#include "vdagentd-proto.h"
#include "vdagent-x11.h"

enum { owner_none, owner_guest, owner_client };

const char *utf8_atom_names[] = {
    "UTF8_STRING",
    "text/plain;charset=UTF-8",
    "text/plain;charset=utf-8",
};

#define utf8_atom_count (sizeof(utf8_atom_names)/sizeof(utf8_atom_names[0]))

struct vdagent_x11_selection_request {
    XEvent event;
    struct vdagent_x11_selection_request *next;
};

struct vdagent_x11 {
    Display *display;
    Atom clipboard_atom;
    Atom targets_atom;
    Atom incr_atom;
    Atom multiple_atom;
    Atom utf8_atoms[utf8_atom_count];
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
    int expected_targets_notifies;
    int expect_property_notify;
    int clipboard_owner;
    Atom clipboard_request_target;
    int clipboard_type_count;
    /* TODO Add support for more types here */
    /* Warning the size of these needs to be increased each time we add
       support for a new type!! */
    uint32_t clipboard_agent_types[1];
    Atom clipboard_x11_targets[1];
    uint8_t *clipboard_data;
    uint32_t clipboard_data_size;
    uint32_t clipboard_data_space;
    struct vdagent_x11_selection_request *selection_request;
};

static void vdagent_x11_send_daemon_guest_xorg_res(struct vdagent_x11 *x11);
static void vdagent_x11_handle_selection_notify(struct vdagent_x11 *x11,
                                                XEvent *event, int incr);
static void vdagent_x11_handle_selection_request(struct vdagent_x11 *x11);
static void vdagent_x11_handle_targets_notify(struct vdagent_x11 *x11,
                                              XEvent *event, int incr);
static void vdagent_x11_send_selection_notify(struct vdagent_x11 *x11,
                                              Atom prop,
                                              int process_next_req);

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
    x11->multiple_atom = XInternAtom(x11->display, "MULTIPLE", False);
    for(i = 0; i < utf8_atom_count; i++)
        x11->utf8_atoms[i] = XInternAtom(x11->display, utf8_atom_names[i],
                                         False);

    /* We should not store properties (for selections) on the root window */
    x11->selection_window = XCreateSimpleWindow(x11->display, x11->root_window,
                                                0, 0, 1, 1, 0, 0, 0);
    if (x11->verbose)
        fprintf(stderr, "Selection window: %u\n",
                (unsigned int)x11->selection_window);

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

static void vdagent_x11_set_clipboard_owner(struct vdagent_x11 *x11,
    int new_owner)
{
    /* Clear pending requests and clipboard data */
    if (x11->selection_request) {
         fprintf(stderr,
                 "selection requests pending on clipboard ownership change, "
                 "clearing");
         while (x11->selection_request)
             vdagent_x11_send_selection_notify(x11, None, 0);
    }
    x11->clipboard_data_size = 0;
    x11->clipboard_request_target = None;
    x11->expect_property_notify = 0;

    if (new_owner == owner_none) {
        /* When going from owner_guest to owner_none we need to send a
           clipboard release message to the client */
        if (x11->clipboard_owner == owner_guest)
           udscs_write(x11->vdagentd, VDAGENTD_CLIPBOARD_RELEASE, 0, NULL, 0);

        x11->clipboard_type_count = 0;
    }
    x11->clipboard_owner = new_owner;
}

static void vdagent_x11_handle_event(struct vdagent_x11 *x11, XEvent event)
{
    int handled = 0;


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

        if (x11->verbose)
            fprintf(stderr, "New selection owner: %u\n",
                    (unsigned int)ev.xfev.owner);

        /* Ignore becoming the owner ourselves */
        if (ev.xfev.owner == x11->selection_window)
            return;

        /* If the clipboard owner is changed we no longer own it */
        vdagent_x11_set_clipboard_owner(x11, owner_none);

        if (ev.xfev.owner == None)
            return;

        /* Request the supported targets from the new owner */
        XConvertSelection(x11->display, x11->clipboard_atom, x11->targets_atom,
                          x11->targets_atom, x11->selection_window,
                          CurrentTime);
        XFlush(x11->display);
        x11->expected_targets_notifies++;
        return;
    }

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

        vdagent_x11_send_daemon_guest_xorg_res(x11);
        break;
    case SelectionNotify:
        if (event.xselection.target == x11->targets_atom)
            vdagent_x11_handle_targets_notify(x11, &event, 0);
        else
            vdagent_x11_handle_selection_notify(x11, &event, 0);

        handled = 1;
        break;
    case PropertyNotify:
        handled = 1;
        if (!x11->expect_property_notify ||
                event.xproperty.state != PropertyNewValue)
            break;

        if (event.xproperty.atom == x11->targets_atom)
            vdagent_x11_handle_targets_notify(x11, &event, 1);
        else
            vdagent_x11_handle_selection_notify(x11, &event, 1);
        break;
    case SelectionClear:
        /* Do nothing the clipboard ownership will get updated through
           the XFixesSetSelectionOwnerNotify event */
        handled = 1;
        break;
    case SelectionRequest: {
        struct vdagent_x11_selection_request *req, *new_req;

        new_req = malloc(sizeof(*new_req));
        if (!new_req) {
            fprintf(stderr, "out of memory on SelectionRequest, ignoring.\n");
            break;
        }

        handled = 1;

        new_req->event = event;
        new_req->next = NULL;

        if (!x11->selection_request) {
            x11->selection_request = new_req;
            vdagent_x11_handle_selection_request(x11);
            break;
        }

        /* maybe we should limit the selection_request stack depth ? */
        req = x11->selection_request;
        while (req->next)
            req = req->next;

        req->next = new_req;
        break;
    }
    }
    if (!handled && x11->verbose)
        fprintf(stderr, "unhandled x11 event, type %d, window %d\n",
                (int)event.type, (int)event.xany.window);
}

void vdagent_x11_do_read(struct vdagent_x11 *x11)
{
    XEvent event;

    while (XPending(x11->display)) {
        XNextEvent(x11->display, &event);
        vdagent_x11_handle_event(x11, event);
    }
}

static void vdagent_x11_send_daemon_guest_xorg_res(struct vdagent_x11 *x11)
{
    struct vdagentd_guest_xorg_resolution res;

    res.width  = x11->width;
    res.height = x11->height;

    udscs_write(x11->vdagentd, VDAGENTD_GUEST_XORG_RESOLUTION, 0,
                (uint8_t *)&res, sizeof(res));
}

static const char *vdagent_x11_get_atom_name(struct vdagent_x11 *x11, Atom a)
{
    if (a == None)
        return "None";

    return XGetAtomName(x11->display, a);
}

static int vdagent_x11_get_selection(struct vdagent_x11 *x11, XEvent *event,
                                  Atom type, Atom prop, int format,
                                  unsigned char **data_ret, int incr)
{
    Bool del = incr ? True: False;
    Atom type_ret;
    int format_ret, ret_val = -1;
    unsigned long len, remain;
    unsigned char *data = NULL;

    if (incr) {
        if (event->xproperty.atom != prop) {
            fprintf(stderr, "PropertyNotify parameters mismatch\n");
            goto exit;
        }
    } else {
        if (event->xselection.property == None) {
            if (x11->verbose)
                fprintf(stderr, "XConvertSelection refused by clipboard owner\n");
            goto exit;
        }

        if (event->xselection.requestor != x11->selection_window ||
                event->xselection.selection != x11->clipboard_atom ||
                event->xselection.property  != prop) {
            fprintf(stderr, "SelectionNotify parameters mismatch\n");
            goto exit;
        }
    }

    /* FIXME when we've incr support we should not immediately
       delete the property (as we need to first register for
       property change events) */
    if (XGetWindowProperty(x11->display, x11->selection_window, prop, 0,
                           LONG_MAX, del, type, &type_ret, &format_ret, &len,
                           &remain, &data) != Success) {
        fprintf(stderr, "XGetWindowProperty failed\n");
        goto exit;
    }

    if (!incr) {
        if (type_ret == x11->incr_atom) {
            int prop_min_size = *(uint32_t*)data;

            if (x11->expect_property_notify) {
                fprintf(stderr,
                        "received an incr property notify while "
                        "still reading another incr property\n");
                goto exit;
            }

            if (x11->clipboard_data_space < prop_min_size) {
                free(x11->clipboard_data);
                x11->clipboard_data = malloc(prop_min_size);
                if (!x11->clipboard_data) {
                    fprintf(stderr,
                            "out of memory allocating clipboard buffer\n");
                    x11->clipboard_data_space = 0;
                    goto exit;
                }
                x11->clipboard_data_space = prop_min_size;
            }
            x11->expect_property_notify = 1;
            XSelectInput(x11->display, x11->selection_window,
                         PropertyChangeMask);
            XDeleteProperty(x11->display, x11->selection_window, prop);
            XFlush(x11->display);
            XFree(data);
            return 0; /* Wait for more data */
        }
        XDeleteProperty(x11->display, x11->selection_window, prop);
        XFlush(x11->display);
    }

    if (type_ret != type) {
        fprintf(stderr, "expected property type: %s, got: %s\n",
                vdagent_x11_get_atom_name(x11, type),
                vdagent_x11_get_atom_name(x11, type_ret));
        goto exit;
    }

    if (format_ret != format) {
        fprintf(stderr, "expected %d bit format, got %d bits\n", format,
                format_ret);
        goto exit;
    }

    /* Convert len to bytes */
    switch(format) {
    case 8:
        break;
    case 16:
        len *= sizeof(short);
        break;
    case 32:
        len *= sizeof(long);
        break;
    }

    if (incr) {
        if (len) {
            if (x11->clipboard_data_size + len > x11->clipboard_data_space) {
                void *old_clipboard_data = x11->clipboard_data;

                x11->clipboard_data_space = x11->clipboard_data_size + len;
                x11->clipboard_data = realloc(x11->clipboard_data,
                                              x11->clipboard_data_space);
                if (!x11->clipboard_data) {
                    fprintf(stderr,
                            "out of memory allocating clipboard buffer\n");
                    x11->clipboard_data_space = 0;
                    free(old_clipboard_data);
                    goto exit;
                }
            }
            memcpy(x11->clipboard_data + x11->clipboard_data_size, data, len);
            x11->clipboard_data_size += len;
            if (x11->verbose)
                fprintf(stderr, "Appended %ld bytes to buffer\n", len);
            XFree(data);
            return 0; /* Wait for more data */
        }
        len = x11->clipboard_data_size;
        *data_ret = x11->clipboard_data;
    } else
        *data_ret = data;

    if (len > 0)
        ret_val = len;
    else
        fprintf(stderr, "property contains no data (zero length)\n");

exit:
    if ((incr || ret_val == -1) && data)
        XFree(data);

    if (incr) {
        x11->clipboard_data_size = 0;
        x11->expect_property_notify = 0;
    }

    return ret_val;
}

static void vdagent_x11_get_selection_free(struct vdagent_x11 *x11,
    unsigned char *data, int incr)
{
    if (incr) {
        /* If the clipboard has grown large return the memory to the system */
        if (x11->clipboard_data_space > 512 * 1024) {
            free(x11->clipboard_data);
            x11->clipboard_data = NULL;
            x11->clipboard_data_space = 0;
        }
    } else if (data)
        XFree(data);
}

static uint32_t vdagent_x11_target_to_type(struct vdagent_x11 *x11,
                                           Atom target)
{
    int i;

    if (target == None)
        return VD_AGENT_CLIPBOARD_NONE;

    for (i = 0; i < utf8_atom_count; i++)
        if (x11->utf8_atoms[i] == target)
            return VD_AGENT_CLIPBOARD_UTF8_TEXT;

    /* TODO Add support for more types here */
    
    fprintf(stderr, "unexpected selection type %s\n",
            vdagent_x11_get_atom_name(x11, target));
    return VD_AGENT_CLIPBOARD_NONE;
}

static Atom vdagent_x11_type_to_target(struct vdagent_x11 *x11, uint32_t type)
{
    int i;

    for (i = 0; i < x11->clipboard_type_count; i++)
        if (x11->clipboard_agent_types[i] == type)
            return x11->clipboard_x11_targets[i];

    fprintf(stderr, "client requested unavailable type %u\n", type);
    return None;
}

static void vdagent_x11_handle_selection_notify(struct vdagent_x11 *x11,
                                                XEvent *event, int incr)
{
    int len = -1;
    unsigned char *data = NULL;
    uint32_t type;

    type  = vdagent_x11_target_to_type(x11, x11->clipboard_request_target);

    if (x11->clipboard_request_target == None)
        fprintf(stderr, "SelectionNotify received without a target\n");
    else if (!incr &&
             event->xselection.target != x11->clipboard_request_target)
        fprintf(stderr, "Requested %s target got %s\n",
                vdagent_x11_get_atom_name(x11, x11->clipboard_request_target),
                vdagent_x11_get_atom_name(x11, event->xselection.target));
    else
        len = vdagent_x11_get_selection(x11, event, event->xselection.target,
                                        x11->clipboard_atom, 8, &data, incr);
    if (len == 0) /* waiting for more data? */
        return;
    if (len == -1) {
        type = VD_AGENT_CLIPBOARD_NONE;
        len = 0;
    }

    udscs_write(x11->vdagentd, VDAGENTD_CLIPBOARD_DATA, type, data, len);
    x11->clipboard_request_target = None;
    vdagent_x11_get_selection_free(x11, data, incr);
}

static Atom atom_lists_overlap(Atom *atoms1, Atom *atoms2, int l1, int l2)
{
    int i, j;

    for (i = 0; i < l1; i++)
        for (j = 0; j < l2; j++)
            if (atoms1[i] == atoms2[j])
                return atoms1[i];

    return 0;
}

static void vdagent_x11_print_targets(struct vdagent_x11 *x11,
                                      const char *action, Atom *atoms, int c)
{
    int i;

    if (!x11->verbose)
        return;

    fprintf(stderr, "%s %d targets:\n", action, c);
    for (i = 0; i < c; i++)
        fprintf(stderr, "%s\n", vdagent_x11_get_atom_name(x11, atoms[i]));
}

static void vdagent_x11_handle_targets_notify(struct vdagent_x11 *x11,
                                              XEvent *event, int incr)
{
    int len;
    Atom atom, *atoms = NULL;

    if (!x11->expected_targets_notifies) {
        fprintf(stderr, "unexpected selection notify TARGETS\n");
        return;
    }

    x11->expected_targets_notifies--;

    /* If we have more targets_notifies pending, ignore this one, we
       are only interested in the targets list of the current owner
       (which is the last one we've requested a targets list from) */
    if (x11->expected_targets_notifies)
        return;

    len = vdagent_x11_get_selection(x11, event, XA_ATOM, x11->targets_atom, 32,
                                    (unsigned char **)&atoms, incr);
    if (len == 0 || len == -1) /* waiting for more data or error? */
        return;

    /* bytes -> atoms */
    len /= sizeof(Atom);
    vdagent_x11_print_targets(x11, "received", atoms, len);

    x11->clipboard_type_count = 0;
    atom = atom_lists_overlap(x11->utf8_atoms, atoms, utf8_atom_count, len);
    if (atom) {
        x11->clipboard_agent_types[x11->clipboard_type_count] =
            VD_AGENT_CLIPBOARD_UTF8_TEXT;
        x11->clipboard_x11_targets[x11->clipboard_type_count] = atom;
        x11->clipboard_type_count++;
    }

    /* TODO Add support for more types here */

    if (x11->clipboard_type_count) {
        udscs_write(x11->vdagentd, VDAGENTD_CLIPBOARD_GRAB, 0,
                    (uint8_t *)x11->clipboard_agent_types,
                    x11->clipboard_type_count * sizeof(uint32_t));
        vdagent_x11_set_clipboard_owner(x11, owner_guest);
    }

    vdagent_x11_get_selection_free(x11, (unsigned char *)atoms, incr);
}

static void vdagent_x11_send_selection_notify(struct vdagent_x11 *x11,
    Atom prop, int process_next_req)
{
    XEvent res, *event = &x11->selection_request->event;
    struct vdagent_x11_selection_request *selection_request;

    res.xselection.property = prop;
    res.xselection.type = SelectionNotify;
    res.xselection.display = event->xselectionrequest.display;
    res.xselection.requestor = event->xselectionrequest.requestor;
    res.xselection.selection = event->xselectionrequest.selection;
    res.xselection.target = event->xselectionrequest.target;
    res.xselection.time = event->xselectionrequest.time;
    XSendEvent(x11->display, event->xselectionrequest.requestor, 0, 0, &res);
    XFlush(x11->display);

    selection_request = x11->selection_request;
    x11->selection_request = selection_request->next;
    free(selection_request);
    if (process_next_req)
        vdagent_x11_handle_selection_request(x11);
}

static void vdagent_x11_send_targets(struct vdagent_x11 *x11, XEvent *event)
{
    /* TODO Add support for more types here */
    /* Warning the size of this needs to be increased each time we add support
       for a new type, or the atom count of an existing type changes */
    Atom prop, targets[4] = { x11->targets_atom, };
    int i, j, target_count = 1;

    for (i = 0; i < x11->clipboard_type_count; i++) {
        switch (x11->clipboard_agent_types[i]) {
            case VD_AGENT_CLIPBOARD_UTF8_TEXT:
                for (j = 0; j < utf8_atom_count; j++) {
                    targets[target_count] = x11->utf8_atoms[j];
                    target_count++;
                }
                break;
            /* TODO Add support for more types here */
        }
    }

    prop = event->xselectionrequest.property;
    if (prop == None)
        prop = event->xselectionrequest.target;

    XChangeProperty(x11->display, event->xselectionrequest.requestor, prop,
                    XA_ATOM, 32, PropModeReplace, (unsigned char *)&targets,
                    target_count);
    vdagent_x11_print_targets(x11, "sent", targets, target_count);
    vdagent_x11_send_selection_notify(x11, prop, 1);
}

static void vdagent_x11_handle_selection_request(struct vdagent_x11 *x11)
{
    XEvent *event;
    uint32_t type = VD_AGENT_CLIPBOARD_NONE;

    if (!x11->selection_request)
        return;

    event = &x11->selection_request->event;

    if (x11->clipboard_owner != owner_client) {
        fprintf(stderr,
            "received selection request event for target %s, "
            "while not owning client clipboard\n",
            vdagent_x11_get_atom_name(x11, event->xselectionrequest.target));
        vdagent_x11_send_selection_notify(x11, None, 1);
        return;
    }

    if (event->xselectionrequest.target == x11->multiple_atom) {
        fprintf(stderr, "multiple target not supported\n");
        vdagent_x11_send_selection_notify(x11, None, 1);
        return;
    }

    if (event->xselectionrequest.target == x11->targets_atom) {
        vdagent_x11_send_targets(x11, event);
        return;
    }

    type = vdagent_x11_target_to_type(x11, event->xselectionrequest.target);
    if (type == VD_AGENT_CLIPBOARD_NONE) {
        vdagent_x11_send_selection_notify(x11, None, 1);
        return;
    }

    udscs_write(x11->vdagentd, VDAGENTD_CLIPBOARD_REQUEST, type, NULL, 0);
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
    Atom target;

    if (x11->clipboard_owner != owner_guest) {
        fprintf(stderr,
                "received clipboard req while not owning guest clipboard\n");
        udscs_write(x11->vdagentd, VDAGENTD_CLIPBOARD_DATA,
                    VD_AGENT_CLIPBOARD_NONE, NULL, 0);
        return;
    }

    target = vdagent_x11_type_to_target(x11, type);
    if (target == None) {
        udscs_write(x11->vdagentd, VDAGENTD_CLIPBOARD_DATA,
                    VD_AGENT_CLIPBOARD_NONE, NULL, 0);
        return;
    }

    if (x11->clipboard_request_target) {
        fprintf(stderr, "XConvertSelection request is already pending\n");
        udscs_write(x11->vdagentd, VDAGENTD_CLIPBOARD_DATA,
                    VD_AGENT_CLIPBOARD_NONE, NULL, 0);
        return;
    }
    x11->clipboard_request_target = target;
    XConvertSelection(x11->display, x11->clipboard_atom, target,
                      x11->clipboard_atom, x11->selection_window, CurrentTime);
    XFlush(x11->display);
}

void vdagent_x11_clipboard_grab(struct vdagent_x11 *x11, uint32_t *types,
    uint32_t type_count)
{
    int i;

    x11->clipboard_type_count = 0;
    for (i = 0; i < type_count; i++) {
        /* TODO Add support for more types here */
        /* Check if we support the type */
        if (types[i] != VD_AGENT_CLIPBOARD_UTF8_TEXT)
            continue;

        x11->clipboard_agent_types[x11->clipboard_type_count] = types[i];
        x11->clipboard_type_count++;
    }

    if (!x11->clipboard_type_count)
        return;

    XSetSelectionOwner(x11->display, x11->clipboard_atom,
                       x11->selection_window, CurrentTime);
    XFlush(x11->display);
    vdagent_x11_set_clipboard_owner(x11, owner_client);
}

void vdagent_x11_clipboard_data(struct vdagent_x11 *x11, uint32_t type,
    const uint8_t *data, uint32_t size)
{
    Atom prop;
    XEvent *event;
    uint32_t type_from_event;

    if (!x11->selection_request) {
        fprintf(stderr, "received clipboard data without an outstanding"
                        "selection request, ignoring\n");
        return;
    }

    event = &x11->selection_request->event;
    type_from_event = vdagent_x11_target_to_type(x11,
                                             event->xselectionrequest.target);
    if (type_from_event != type) {
        fprintf(stderr, "expecting type %u clipboard data got %u\n",
                type_from_event, type);
        vdagent_x11_send_selection_notify(x11, None, 1);
        return;
    }

    prop = event->xselectionrequest.property;
    if (prop == None)
        prop = event->xselectionrequest.target;

    /* FIXME: use INCR for large data transfers */
    XChangeProperty(x11->display, event->xselectionrequest.requestor, prop,
                    event->xselectionrequest.target, 8, PropModeReplace,
                    data, size);
    vdagent_x11_send_selection_notify(x11, prop, 1);
}

void vdagent_x11_clipboard_release(struct vdagent_x11 *x11)
{
    XEvent event;

    if (x11->clipboard_owner != owner_client) {
        fprintf(stderr,
            "received clipboard release while not owning client clipboard\n");
        return;
    }

    XSetSelectionOwner(x11->display, x11->clipboard_atom, None, CurrentTime);
    /* Make sure we process the XFixesSetSelectionOwnerNotify event caused
       by this, so we don't end up changing the clipboard owner to none, after
       it has already been re-owned because this event is still pending. */
    XSync(x11->display, False);
    while (XCheckTypedEvent(x11->display, x11->xfixes_event_base,
                            &event))
        vdagent_x11_handle_event(x11, event);

    /* Note no need to do a set_clipboard_owner(owner_none) here, as that is
       already done by processing the XFixesSetSelectionOwnerNotify event. */
}

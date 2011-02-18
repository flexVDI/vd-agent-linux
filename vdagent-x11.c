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

/* Note: Our event loop is only called when there is data to be read from the
   X11 socket. If events have arrived and have already been read by libX11 from
   the socket triggered by other libX11 calls from this file, the select for
   read in the main loop, won't see these and our event loop won't get called!
   
   Thus we must make sure that all queued events have been consumed, whenever
   we return to the main loop. IOW all (externally callable) functions in this
   file must end with calling XPending and consuming all queued events.
   
   Calling XPending when-ever we return to the mainloop also ensures any
   pending writes are flushed. */

#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <assert.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/Xfixes.h>
#include "vdagentd-proto.h"
#include "vdagent-x11.h"

enum { owner_none, owner_guest, owner_client };

struct vdagent_x11_selection_request {
    XEvent event;
    struct vdagent_x11_selection_request *next;
};

struct clipboard_format_tmpl {
    uint32_t type;
    const char *atom_names[16];
};

struct clipboard_format_info {
    uint32_t type;
    Atom atoms[16];
    int atom_count;
};

static const struct clipboard_format_tmpl clipboard_format_templates[] = {
    { VD_AGENT_CLIPBOARD_UTF8_TEXT, { "UTF8_STRING",
      "text/plain;charset=UTF-8", "text/plain;charset=utf-8", NULL }, },
    { VD_AGENT_CLIPBOARD_IMAGE_PNG, { "image/png", NULL }, },
    { VD_AGENT_CLIPBOARD_IMAGE_BMP, { "image/bmp", "image/x-bmp",
      "image/x-MS-bmp", "image/x-win-bitmap", NULL }, },
    { VD_AGENT_CLIPBOARD_IMAGE_TIFF, { "image/tiff", NULL }, },
    { VD_AGENT_CLIPBOARD_IMAGE_JPG, { "image/jpeg", NULL }, },
};

#define clipboard_format_count (sizeof(clipboard_format_templates)/sizeof(clipboard_format_templates[0]))

struct vdagent_x11 {
    struct clipboard_format_info clipboard_formats[clipboard_format_count];
    Display *display;
    Atom clipboard_atom;
    Atom clipboard_primary_atom;
    Atom targets_atom;
    Atom incr_atom;
    Atom multiple_atom;
    Window root_window;
    Window selection_window;
    struct udscs_connection *vdagentd;
    FILE *errfile;
    int verbose;
    int fd;
    int screen;
    int width;
    int height;
    int has_xrandr;
    int has_xfixes;
    int xfixes_event_base;
    int max_prop_size;
    int expected_targets_notifies;
    int expect_property_notify;
    int clipboard_owner;
    Atom clipboard_request_target;
    int clipboard_type_count;
    uint32_t clipboard_agent_types[256];
    Atom clipboard_x11_targets[256];
    uint8_t *clipboard_data;
    uint32_t clipboard_data_size;
    uint32_t clipboard_data_space;
    struct vdagent_x11_selection_request *selection_request;
    uint8_t *selection_req_data;
    uint32_t selection_req_data_pos;
    uint32_t selection_req_data_size;
    Atom selection_req_atom;
};

static void vdagent_x11_send_daemon_guest_xorg_res(struct vdagent_x11 *x11);
static void vdagent_x11_handle_selection_notify(struct vdagent_x11 *x11,
                                                XEvent *event, int incr);
static void vdagent_x11_handle_selection_request(struct vdagent_x11 *x11);
static void vdagent_x11_handle_targets_notify(struct vdagent_x11 *x11,
                                              XEvent *event, int incr);
static void vdagent_x11_handle_property_delete_notify(struct vdagent_x11 *x11,
                                                      XEvent *del_event);
static void vdagent_x11_send_selection_notify(struct vdagent_x11 *x11,
                                              Atom prop,
                                              int process_next_req);
static void vdagent_x11_set_clipboard_owner(struct vdagent_x11 *x11,
                                            int new_owner);

struct vdagent_x11 *vdagent_x11_create(struct udscs_connection *vdagentd,
    FILE *errfile, int verbose)
{
    struct vdagent_x11 *x11;
    XWindowAttributes attrib;
    int i, j, major, minor;

    x11 = calloc(1, sizeof(*x11));
    if (!x11) {
        fprintf(errfile, "out of memory allocating vdagent_x11 struct\n");
        return NULL;
    }

    x11->vdagentd = vdagentd;
    x11->errfile = errfile;
    x11->verbose = verbose;

    x11->display = XOpenDisplay(NULL);
    if (!x11->display) {
        fprintf(x11->errfile, "could not connect to X-server\n");
        free(x11);
        return NULL;
    }

    x11->screen = DefaultScreen(x11->display);
    x11->root_window = RootWindow(x11->display, x11->screen);
    x11->fd = ConnectionNumber(x11->display);
    x11->clipboard_atom = XInternAtom(x11->display, "CLIPBOARD", False);
    x11->clipboard_primary_atom = XInternAtom(x11->display, "PRIMARY", False);
    x11->targets_atom = XInternAtom(x11->display, "TARGETS", False);
    x11->incr_atom = XInternAtom(x11->display, "INCR", False);
    x11->multiple_atom = XInternAtom(x11->display, "MULTIPLE", False);
    for(i = 0; i < clipboard_format_count; i++) {
        x11->clipboard_formats[i].type = clipboard_format_templates[i].type;
        for(j = 0; clipboard_format_templates[i].atom_names[j]; j++) {
            x11->clipboard_formats[i].atoms[j] =
                XInternAtom(x11->display,
                            clipboard_format_templates[i].atom_names[j],
                            False);
        }
        x11->clipboard_formats[i].atom_count = j;
    }

    /* We should not store properties (for selections) on the root window */
    x11->selection_window = XCreateSimpleWindow(x11->display, x11->root_window,
                                                0, 0, 1, 1, 0, 0, 0);
    if (x11->verbose)
        fprintf(x11->errfile, "Selection window: %u\n",
                (unsigned int)x11->selection_window);

    if (XRRQueryExtension(x11->display, &i, &i))
        x11->has_xrandr = 1;
    else
        fprintf(x11->errfile, "no xrandr\n");

    if (XFixesQueryExtension(x11->display, &x11->xfixes_event_base, &i) &&
        XFixesQueryVersion(x11->display, &major, &minor) && major >= 1) {
        x11->has_xfixes = 1;
        XFixesSelectSelectionInput(x11->display, x11->root_window,
                                   x11->clipboard_atom,
                                   XFixesSetSelectionOwnerNotifyMask|
                                   XFixesSelectionWindowDestroyNotifyMask|
                                   XFixesSelectionClientCloseNotifyMask);
        XFixesSelectSelectionInput(x11->display, x11->root_window,
                                   x11->clipboard_primary_atom,
                                   XFixesSetSelectionOwnerNotifyMask|
                                   XFixesSelectionWindowDestroyNotifyMask|
                                   XFixesSelectionClientCloseNotifyMask);
    } else
        fprintf(x11->errfile,
                "no xfixes, no guest -> client copy paste support\n");

    x11->max_prop_size = XExtendedMaxRequestSize(x11->display);
    if (x11->max_prop_size) {
        x11->max_prop_size -= 100;
    } else {
        x11->max_prop_size = XMaxRequestSize(x11->display) - 100;
    }
    /* Be a good X11 citizen and maximize the amount of data we send at once */
    if (x11->max_prop_size > 262144)
        x11->max_prop_size = 262144;

    /* Catch resolution changes */
    XSelectInput(x11->display, x11->root_window, StructureNotifyMask);

    /* Get the current resolution */
    XGetWindowAttributes(x11->display, x11->root_window, &attrib);
    x11->width = attrib.width;
    x11->height = attrib.height;
    vdagent_x11_send_daemon_guest_xorg_res(x11);

    /* Flush output buffers and consume any pending events */
    vdagent_x11_do_read(x11);

    return x11;
}

void vdagent_x11_destroy(struct vdagent_x11 *x11)
{
    if (!x11)
        return;

    vdagent_x11_set_clipboard_owner(x11, owner_none);
    XCloseDisplay(x11->display);
    free(x11);
}

int vdagent_x11_get_fd(struct vdagent_x11 *x11)
{
    return x11->fd;
}

static void vdagent_x11_next_selection_request(struct vdagent_x11 *x11)
{
    struct vdagent_x11_selection_request *selection_request;

    free(x11->selection_req_data);
    x11->selection_req_data = NULL;
    x11->selection_req_data_pos = 0;
    x11->selection_req_data_size = 0;
    x11->selection_req_atom = None;

    selection_request = x11->selection_request;
    x11->selection_request = selection_request->next;
    free(selection_request);
}

static void vdagent_x11_set_clipboard_owner(struct vdagent_x11 *x11,
    int new_owner)
{
    /* Clear pending requests and clipboard data */
    if (x11->selection_request) {
        fprintf(x11->errfile,
                "selection requests pending on clipboard ownership change, "
                "clearing\n");
        while (x11->selection_request) {
            vdagent_x11_send_selection_notify(x11, None, 0);
            vdagent_x11_next_selection_request(x11);
        }
    }
    if (x11->clipboard_request_target != None) {
        fprintf(x11->errfile,
                "client clipboard request pending on clipboard ownership "
                "change, clearing\n");
        udscs_write(x11->vdagentd, VDAGENTD_CLIPBOARD_DATA,
                    VD_AGENT_CLIPBOARD_NONE, NULL, 0);
        x11->clipboard_request_target = None;
    }
    x11->clipboard_data_size = 0;
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

static int vdagent_x11_get_clipboard_atom(struct vdagent_x11 *x11, uint8_t selection, Atom* clipboard)
{
    if (selection == VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD) {
        *clipboard = x11->clipboard_atom;
    } else if (selection == VD_AGENT_CLIPBOARD_SELECTION_PRIMARY) {
        *clipboard = x11->clipboard_primary_atom;
    } else {
        fprintf(x11->errfile, "selection_get_grab: unknown selection\n");
        return -1;
    }

    return 0;
}

static int vdagent_x11_get_clipboard_selection(struct vdagent_x11 *x11,
                                               XEvent *event, uint8_t *selection)
{
    Atom atom;

    if (event->type == x11->xfixes_event_base) {
        XFixesSelectionNotifyEvent *xfev = (XFixesSelectionNotifyEvent *)event;
        atom = xfev->selection;
    } else if (event->type == SelectionNotify) {
        atom = event->xselection.selection;
    } else if (event->type == SelectionRequest) {
        atom = event->xselectionrequest.selection;
    } else {
        fprintf(x11->errfile, "get_clipboard_selection: unknown event type\n");
        return -1;
    }

    if (atom == x11->clipboard_atom) {
        *selection = VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD;
    } else if (atom == x11->clipboard_primary_atom) {
        *selection = VD_AGENT_CLIPBOARD_SELECTION_PRIMARY;
    } else {
        fprintf(x11->errfile, "get_clipboard_selection: unknown selection\n");
        return -1;
    }

    return 0;
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
        switch (ev.xfev.subtype) {
        case XFixesSetSelectionOwnerNotify:
            break;
        /* Treat ... as a SelectionOwnerNotify None */
        case XFixesSelectionWindowDestroyNotify:
        case XFixesSelectionClientCloseNotify:
            ev.xfev.owner = None;
            break;
        default:
            if (x11->verbose)
                fprintf(x11->errfile,
                        "unexpected xfix event subtype %d window %d\n",
                        (int)ev.xfev.subtype, (int)event.xany.window);
            return;
        }

        if (x11->verbose)
            fprintf(x11->errfile, "New selection owner: %u\n",
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
        if (x11->expect_property_notify &&
                                event.xproperty.state == PropertyNewValue) {
            if (event.xproperty.atom == x11->targets_atom) {
                vdagent_x11_handle_targets_notify(x11, &event, 1);
            } else {
                vdagent_x11_handle_selection_notify(x11, &event, 1);
            }
        }
        if (x11->selection_req_data && 
                                 event.xproperty.state == PropertyDelete) {
            vdagent_x11_handle_property_delete_notify(x11, &event);
        }
        /* Always mark as handled, since we cannot unselect input for property
           notifications once we are done with handling the incr transfer. */
        handled = 1;
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
            fprintf(x11->errfile,
                    "out of memory on SelectionRequest, ignoring.\n");
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
        fprintf(x11->errfile, "unhandled x11 event, type %d, window %d\n",
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

    *data_ret = NULL;

    if (incr) {
        if (event->xproperty.atom != prop) {
            fprintf(x11->errfile, "PropertyNotify parameters mismatch\n");
            goto exit;
        }
    } else {
        if (event->xselection.property == None) {
            if (x11->verbose)
                fprintf(x11->errfile,
                        "XConvertSelection refused by clipboard owner\n");
            goto exit;
        }

        if (event->xselection.requestor != x11->selection_window ||
                event->xselection.selection != x11->clipboard_atom ||
                event->xselection.property  != prop) {
            fprintf(x11->errfile, "SelectionNotify parameters mismatch\n");
            goto exit;
        }
    }

    if (XGetWindowProperty(x11->display, x11->selection_window, prop, 0,
                           LONG_MAX, del, type, &type_ret, &format_ret, &len,
                           &remain, &data) != Success) {
        fprintf(x11->errfile, "XGetWindowProperty failed\n");
        goto exit;
    }

    if (!incr) {
        if (type_ret == x11->incr_atom) {
            int prop_min_size = *(uint32_t*)data;

            if (x11->expect_property_notify) {
                fprintf(x11->errfile,
                        "received an incr SelectionNotify while "
                        "still reading another incr property\n");
                goto exit;
            }

            if (x11->clipboard_data_space < prop_min_size) {
                free(x11->clipboard_data);
                x11->clipboard_data = malloc(prop_min_size);
                if (!x11->clipboard_data) {
                    fprintf(x11->errfile,
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
            XFree(data);
            return 0; /* Wait for more data */
        }
        XDeleteProperty(x11->display, x11->selection_window, prop);
    }

    if (type_ret != type) {
        fprintf(x11->errfile, "expected property type: %s, got: %s\n",
                vdagent_x11_get_atom_name(x11, type),
                vdagent_x11_get_atom_name(x11, type_ret));
        goto exit;
    }

    if (format_ret != format) {
        fprintf(x11->errfile, "expected %d bit format, got %d bits\n", format,
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
                    fprintf(x11->errfile,
                            "out of memory allocating clipboard buffer\n");
                    x11->clipboard_data_space = 0;
                    free(old_clipboard_data);
                    goto exit;
                }
            }
            memcpy(x11->clipboard_data + x11->clipboard_data_size, data, len);
            x11->clipboard_data_size += len;
            if (x11->verbose)
                fprintf(x11->errfile, "Appended %ld bytes to buffer\n", len);
            XFree(data);
            return 0; /* Wait for more data */
        }
        len = x11->clipboard_data_size;
        *data_ret = x11->clipboard_data;
    } else
        *data_ret = data;

    if (len > 0) {
        ret_val = len;
    } else {
        fprintf(x11->errfile, "property contains no data (zero length)\n");
        *data_ret = NULL;
    }

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
    int i, j;

    for (i = 0; i < clipboard_format_count; i++) {
        for (j = 0; j < x11->clipboard_formats[i].atom_count; i++) {
            if (x11->clipboard_formats[i].atoms[j] == target) {
                return x11->clipboard_formats[i].type;
            }
        }
    }

    fprintf(x11->errfile, "unexpected selection type %s\n",
            vdagent_x11_get_atom_name(x11, target));
    return VD_AGENT_CLIPBOARD_NONE;
}

static Atom vdagent_x11_type_to_target(struct vdagent_x11 *x11, uint32_t type)
{
    int i;

    for (i = 0; i < x11->clipboard_type_count; i++)
        if (x11->clipboard_agent_types[i] == type)
            return x11->clipboard_x11_targets[i];

    fprintf(x11->errfile, "client requested unavailable type %u\n", type);
    return None;
}

static void vdagent_x11_handle_selection_notify(struct vdagent_x11 *x11,
                                                XEvent *event, int incr)
{
    int len = -1;
    unsigned char *data = NULL;
    uint32_t type;

    if (x11->clipboard_request_target == None) {
        fprintf(x11->errfile, "SelectionNotify received without a target\n");
        return;
    }

    type  = vdagent_x11_target_to_type(x11, x11->clipboard_request_target);
    if (!incr &&
             event->xselection.target != x11->clipboard_request_target &&
             event->xselection.target != x11->incr_atom)
        fprintf(x11->errfile, "Requested %s target got %s\n",
                vdagent_x11_get_atom_name(x11, x11->clipboard_request_target),
                vdagent_x11_get_atom_name(x11, event->xselection.target));
    else
        len = vdagent_x11_get_selection(x11, event, x11->clipboard_request_target,
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

    fprintf(x11->errfile, "%s %d targets:\n", action, c);
    for (i = 0; i < c; i++)
        fprintf(x11->errfile, "%s\n",
                vdagent_x11_get_atom_name(x11, atoms[i]));
}

static void vdagent_x11_handle_targets_notify(struct vdagent_x11 *x11,
                                              XEvent *event, int incr)
{
    int i, len;
    Atom atom, *atoms = NULL;

    if (!x11->expected_targets_notifies) {
        fprintf(x11->errfile, "unexpected selection notify TARGETS\n");
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
    for (i = 0; i < clipboard_format_count; i++) {
        atom = atom_lists_overlap(x11->clipboard_formats[i].atoms, atoms,
                                  x11->clipboard_formats[i].atom_count, len);
        if (atom) {
            x11->clipboard_agent_types[x11->clipboard_type_count] =
                x11->clipboard_formats[i].type;
            x11->clipboard_x11_targets[x11->clipboard_type_count] = atom;
            x11->clipboard_type_count++;
            if (x11->clipboard_type_count ==
                    sizeof(x11->clipboard_agent_types)/sizeof(uint32_t)) {
                fprintf(x11->errfile,
                        "handle_targets_notify: too many types\n");
                break;
            }
        }
    }

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

    res.xselection.property = prop;
    res.xselection.type = SelectionNotify;
    res.xselection.display = event->xselectionrequest.display;
    res.xselection.requestor = event->xselectionrequest.requestor;
    res.xselection.selection = event->xselectionrequest.selection;
    res.xselection.target = event->xselectionrequest.target;
    res.xselection.time = event->xselectionrequest.time;
    XSendEvent(x11->display, event->xselectionrequest.requestor, 0, 0, &res);

    if (process_next_req) {
        vdagent_x11_next_selection_request(x11);
        vdagent_x11_handle_selection_request(x11);
    }
}

static void vdagent_x11_send_targets(struct vdagent_x11 *x11, XEvent *event)
{
    Atom prop, targets[256] = { x11->targets_atom, };
    int i, j, k, target_count = 1;

    for (i = 0; i < x11->clipboard_type_count; i++) {
        for (j = 0; j < clipboard_format_count; j++) {
            if (x11->clipboard_formats[j].type !=
                    x11->clipboard_agent_types[i])
                continue;

            for (k = 0; k < x11->clipboard_formats[j].atom_count; k++) {
                targets[target_count] = x11->clipboard_formats[j].atoms[k];
                target_count++;
                if (target_count == sizeof(targets)/sizeof(Atom)) {
                    fprintf(x11->errfile, "send_targets: too many targets\n");
                    goto exit_loop;
                }
            }
        }
    }
exit_loop:

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
        fprintf(x11->errfile,
            "received selection request event for target %s, "
            "while not owning client clipboard\n",
            vdagent_x11_get_atom_name(x11, event->xselectionrequest.target));
        vdagent_x11_send_selection_notify(x11, None, 1);
        return;
    }

    if (event->xselectionrequest.target == x11->multiple_atom) {
        fprintf(x11->errfile, "multiple target not supported\n");
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

static void vdagent_x11_handle_property_delete_notify(struct vdagent_x11 *x11,
                                                      XEvent *del_event)
{
    XEvent *sel_event;
    int len;

    assert(x11->selection_request);
    sel_event = &x11->selection_request->event;
    if (del_event->xproperty.window != sel_event->xselectionrequest.requestor
            || del_event->xproperty.atom != x11->selection_req_atom) {
        return;
    }

    len = x11->selection_req_data_size - x11->selection_req_data_pos;
    if (len > x11->max_prop_size) {
        len = x11->max_prop_size;
    }

    if (x11->verbose) {
        if (len) {
            fprintf(x11->errfile, "Sending %d-%d/%d bytes of clipboard data\n",
                    x11->selection_req_data_pos,
                    x11->selection_req_data_pos + len - 1,
                    x11->selection_req_data_size);
        } else {
            fprintf(x11->errfile, "Ending incr send of clipboard data\n");
        }
    }
    XChangeProperty(x11->display, sel_event->xselectionrequest.requestor,
                    x11->selection_req_atom,
                    sel_event->xselectionrequest.target, 8, PropModeReplace,
                    x11->selection_req_data + x11->selection_req_data_pos,
                    len);
    x11->selection_req_data_pos += len;

    /* Note we must explictly send a 0 sized XChangeProperty to signal the
       incr transfer is done. Hence we do not check if we've send all data
       but instead check we've send the final 0 sized XChangeProperty. */
    if (len == 0) {
        vdagent_x11_next_selection_request(x11);
        vdagent_x11_handle_selection_request(x11);
    }
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

    /* Flush output buffers and consume any pending events */
    vdagent_x11_do_read(x11);
}

void vdagent_x11_clipboard_request(struct vdagent_x11 *x11, uint32_t type)
{
    Atom target;

    if (x11->clipboard_owner != owner_guest) {
        fprintf(x11->errfile,
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
        fprintf(x11->errfile,
                "XConvertSelection request is already pending\n");
        udscs_write(x11->vdagentd, VDAGENTD_CLIPBOARD_DATA,
                    VD_AGENT_CLIPBOARD_NONE, NULL, 0);
        return;
    }
    x11->clipboard_request_target = target;
    XConvertSelection(x11->display, x11->clipboard_atom, target,
                      x11->clipboard_atom, x11->selection_window, CurrentTime);

    /* Flush output buffers and consume any pending events */
    vdagent_x11_do_read(x11);
}

void vdagent_x11_clipboard_grab(struct vdagent_x11 *x11, uint32_t *types,
    uint32_t type_count)
{
    if (type_count > sizeof(x11->clipboard_agent_types)/sizeof(uint32_t)) {
        fprintf(x11->errfile, "x11_clipboard_grab: too many types\n");
        type_count = sizeof(x11->clipboard_agent_types)/sizeof(uint32_t);
    }

    memcpy(x11->clipboard_agent_types, types, type_count * sizeof(uint32_t));
    x11->clipboard_type_count = type_count;

    XSetSelectionOwner(x11->display, x11->clipboard_atom,
                       x11->selection_window, CurrentTime);
    vdagent_x11_set_clipboard_owner(x11, owner_client);

    /* Flush output buffers and consume any pending events */
    vdagent_x11_do_read(x11);
}

void vdagent_x11_clipboard_data(struct vdagent_x11 *x11, uint32_t type,
    uint8_t *data, uint32_t size)
{
    Atom prop;
    XEvent *event;
    uint32_t type_from_event;

    if (x11->selection_req_data) {
        if (type || size) {
            fprintf(x11->errfile, "received clipboard data while still sending"
                                  " data from previous request, ignoring\n");
        }
        free(data);
        return;
    }

    if (!x11->selection_request) {
        if (type || size) {
            fprintf(x11->errfile, "received clipboard data without an "
                                  "outstanding selection request, ignoring\n");
        }
        free(data);
        return;
    }

    event = &x11->selection_request->event;
    type_from_event = vdagent_x11_target_to_type(x11,
                                             event->xselectionrequest.target);
    if (type_from_event != type) {
        fprintf(x11->errfile, "expecting type %u clipboard data got %u\n",
                type_from_event, type);
        vdagent_x11_send_selection_notify(x11, None, 1);
        free(data);

        /* Flush output buffers and consume any pending events */
        vdagent_x11_do_read(x11);
        return;
    }

    prop = event->xselectionrequest.property;
    if (prop == None)
        prop = event->xselectionrequest.target;

    if (size > x11->max_prop_size) {
        unsigned long len = size;
        if (x11->verbose)
            fprintf(x11->errfile, "Starting incr send of clipboard data\n");
        x11->selection_req_data = data;
        x11->selection_req_data_pos = 0;
        x11->selection_req_data_size = size;
        x11->selection_req_atom = prop;
        XSelectInput(x11->display, event->xselectionrequest.requestor,
                     PropertyChangeMask);
        XChangeProperty(x11->display, event->xselectionrequest.requestor, prop,
                        x11->incr_atom, 32, PropModeReplace,
                        (unsigned char*)&len, 1);
        vdagent_x11_send_selection_notify(x11, prop, 0);
    } else {
        XChangeProperty(x11->display, event->xselectionrequest.requestor, prop,
                        event->xselectionrequest.target, 8, PropModeReplace,
                        data, size);
        vdagent_x11_send_selection_notify(x11, prop, 1);
        free(data);
    }

    /* Flush output buffers and consume any pending events */
    vdagent_x11_do_read(x11);
}

void vdagent_x11_clipboard_release(struct vdagent_x11 *x11)
{
    XEvent event;

    if (x11->clipboard_owner != owner_client) {
        fprintf(x11->errfile,
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

    /* Flush output buffers and consume any pending events */
    vdagent_x11_do_read(x11);
}

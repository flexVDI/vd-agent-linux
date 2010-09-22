/*  vdagentd-uinput.c vdagentd uinput handling code

    Copyright 2010 Red Hat, Inc.

    Red Hat Authors:
    Hans de Goede <hdegoede@redhat.com>
    Gerd Hoffmann <kraxel@redhat.com>

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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <spice/vd_agent.h>

static int tablet = -1;

void uinput_setup(const char *uinput_devname, int width, int height)
{
    struct uinput_user_dev device = {
        .name = "spice vdagent tablet",
        .absmax  [ ABS_X ] = width,
        .absmax  [ ABS_Y ] = height,
    };
    int rc;

    if (tablet != -1)
        close(tablet);

    tablet = open(uinput_devname, O_RDWR);
    if (tablet == -1) {
        fprintf(stderr, "open %s: %s\n", uinput_devname, strerror(errno));
        exit(1);
    }

    rc = write(tablet, &device, sizeof(device));
    if (rc != sizeof(device)) {
        fprintf(stderr, "%s: write error\n", __FUNCTION__);
        exit(1);
    }

    /* buttons */
    ioctl(tablet, UI_SET_EVBIT, EV_KEY);
    ioctl(tablet, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(tablet, UI_SET_KEYBIT, BTN_MIDDLE);
    ioctl(tablet, UI_SET_KEYBIT, BTN_RIGHT);

    /* wheel */
    ioctl(tablet, UI_SET_EVBIT, EV_REL);
    ioctl(tablet, UI_SET_RELBIT, REL_WHEEL);

    /* abs ptr */
    ioctl(tablet, UI_SET_EVBIT, EV_ABS);
    ioctl(tablet, UI_SET_ABSBIT, ABS_X);
    ioctl(tablet, UI_SET_ABSBIT, ABS_Y);

    rc = ioctl(tablet, UI_DEV_CREATE);
    if (rc < 0) {
        fprintf(stderr, "%s: create error\n", __FUNCTION__);
        exit(1);
    }
}

void uinput_close(void)
{
    close(tablet);
    tablet = -1;
}

static void uinput_send_event(__u16 type, __u16 code, __s32 value)
{
    struct input_event event = {
        .type  = type,
        .code  = code,
        .value = value,
    };
    int rc;

    rc = write(tablet, &event, sizeof(event));
    if (rc != sizeof(event)) {
        fprintf(stderr, "%s: write error\n", __FUNCTION__);
        exit(1);
    }
}

void uinput_do_mouse(VDAgentMouseState *mouse, int verbose)
{
    struct button_s {
        const char *name;
        int mask;
        int btn;
    };
    static const struct button_s btns[] = {
        { .name = "left",   .mask =  VD_AGENT_LBUTTON_MASK, .btn = BTN_LEFT      },
        { .name = "middle", .mask =  VD_AGENT_MBUTTON_MASK, .btn = BTN_MIDDLE    },
        { .name = "right",  .mask =  VD_AGENT_RBUTTON_MASK, .btn = BTN_RIGHT     },
    };
    static const struct button_s wheel[] = {
        { .name = "up",     .mask =  VD_AGENT_UBUTTON_MASK, .btn = 1  },
        { .name = "down",   .mask =  VD_AGENT_DBUTTON_MASK, .btn = -1 },
    };
    static VDAgentMouseState last;
    int i, down;

    if (last.x != mouse->x) {
        if (verbose)
            fprintf(stderr, "mouse: abs-x %d\n", mouse->x);
        uinput_send_event(EV_ABS, ABS_X, mouse->x);
    }
    if (last.y != mouse->y) {
        if (verbose)
            fprintf(stderr, "mouse: abs-y %d\n", mouse->y);
        uinput_send_event(EV_ABS, ABS_Y, mouse->y);
    }
    for (i = 0; i < sizeof(btns)/sizeof(btns[0]); i++) {
        if ((last.buttons & btns[i].mask) == (mouse->buttons & btns[i].mask))
            continue;
        down = !!(mouse->buttons & btns[i].mask);
        if (verbose)
            fprintf(stderr, "mouse: btn-%s %s\n",
                    btns[i].name, down ? "down" : "up");
        uinput_send_event(EV_KEY, btns[i].btn, down);
    }
    for (i = 0; i < sizeof(wheel)/sizeof(wheel[0]); i++) {
        if ((last.buttons & wheel[i].mask) == (mouse->buttons & wheel[i].mask))
            continue;
        if (mouse->buttons & wheel[i].mask) {
            if (verbose)
                fprintf(stderr, "mouse: wheel-%s\n", wheel[i].name);
            uinput_send_event(EV_REL, REL_WHEEL, wheel[i].btn);
        }
    }
    if (verbose)
        fprintf(stderr, "mouse: syn\n");
    uinput_send_event(EV_SYN, SYN_REPORT, 0);

    last = *mouse;
}

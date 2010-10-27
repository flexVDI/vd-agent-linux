/*  vdagent.c xorg-client to vdagentd (daemon).

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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/select.h>
#include <spice/vd_agent.h>

#include "udscs.h"
#include "vdagentd-proto.h"
#include "vdagentd-proto-strings.h"
#include "vdagent-x11.h"

static int verbose = 0;
static struct vdagent_x11 *x11 = NULL;

void daemon_read_complete(struct udscs_connection **connp,
    struct udscs_message_header *header, const uint8_t *data)
{
    switch (header->type) {
    case VDAGENTD_MONITORS_CONFIG:
        vdagent_x11_set_monitor_config(x11, (VDAgentMonitorsConfig *)data);
        break;
    case VDAGENTD_CLIPBOARD_REQUEST:
        vdagent_x11_clipboard_request(x11, header->opaque);
        break;
    case VDAGENTD_CLIPBOARD_GRAB:
        vdagent_x11_clipboard_grab(x11, (uint32_t *)data,
                                   header->size / sizeof(uint32_t));
        break;
    case VDAGENTD_CLIPBOARD_DATA:
        vdagent_x11_clipboard_data(x11, header->opaque, data, header->size);
        break;
    case VDAGENTD_CLIPBOARD_RELEASE:
        vdagent_x11_clipboard_release(x11);
        break;
    default:
        if (verbose)
            fprintf(stderr, "Unknown message from vdagentd type: %d\n",
                    header->type);
    }
}

static void usage(FILE *fp)
{
    fprintf(fp,
            "vdagent -- spice agent xorg client\n"
            "options:\n"
            "  -h         print this text\n"
            "  -d         print debug messages\n");
}

int main(int argc, char *argv[])
{
    struct udscs_connection *client = NULL;
    fd_set readfds, writefds;
    int c, n, nfds, x11_fd;

    for (;;) {
        if (-1 == (c = getopt(argc, argv, "dh")))
            break;
        switch (c) {
        case 'd':
            verbose++;
            break;
        case 'h':
            usage(stdout);
            exit(0);
        default:
            usage(stderr);
            exit(1);
        }
    }

    client = udscs_connect(VDAGENTD_SOCKET, daemon_read_complete, NULL,
                           vdagentd_messages, VDAGENTD_NO_MESSAGES,
                           verbose? stderr:NULL, stderr);
    if (!client)
        exit(1);

    x11 = vdagent_x11_create(client, verbose);
    if (!x11) {
        udscs_destroy_connection(&client);
        exit(1);
    }

    while (client) {
        FD_ZERO(&readfds);
        FD_ZERO(&writefds);

        nfds = udscs_client_fill_fds(client, &readfds, &writefds);
        x11_fd = vdagent_x11_get_fd(x11);
        FD_SET(x11_fd, &readfds);
        if (x11_fd >= nfds)
            nfds = x11_fd + 1;

        n = select(nfds, &readfds, &writefds, NULL, NULL);
        if (n == -1) {
            if (errno == EINTR)
                continue;
            perror("select");
            exit(1);
        }

        if (FD_ISSET(x11_fd, &readfds))
            vdagent_x11_do_read(x11);
        udscs_client_handle_fds(&client, &readfds, &writefds);
    }

    vdagent_x11_destroy(x11);

    return 0;
}

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

#include "udscs.h"
#include "vdagentd-proto.h"

void daemon_read_complete(struct udscs_connection *conn,
    struct udscs_message_header *header, const uint8_t *data)
{
}

int main(int argc, char *argv[])
{
    struct udscs_connection *client;
    fd_set readfds, writefds;
    int n, nfds;
    
    client = udscs_connect(VDAGENTD_SOCKET, daemon_read_complete, NULL);
    if (!client)
        exit(1);

    /* test test */
    struct vdagentd_guest_xorg_resolution res = { 1680, 1050 };
    struct udscs_message_header header = {
        VDAGENTD_GUEST_XORG_RESOLUTION,
        0,
        sizeof(res),
    };
    udscs_write(client, &header, (uint8_t *)&res);

    for (;;) {
        FD_ZERO(&readfds);
        FD_ZERO(&writefds);

        nfds = udscs_client_fill_fds(client, &readfds, &writefds);

        n = select(nfds, &readfds, &writefds, NULL, NULL);
        if (n == -1) {
            if (errno == EINTR)
                continue;
            perror("select");
            exit(1);
        }

        udscs_client_handle_fds(client, &readfds, &writefds);
    }

    udscs_destroy_connection(client);

    return 0;    
}

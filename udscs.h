/*  udscs.h Unix Domain Socket Client Server framework header file

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

#ifndef __UDSCS_H
#define __UDSCS_H

#include <stdint.h>
#include <sys/select.h>

struct udscs_connection;
struct udscs_server;
struct udscs_message_header {
    uint32_t type;
    uint32_t opaque;
    uint32_t size;  
    uint8_t data[0];
};

typedef void (*udscs_read_callback)(struct udscs_connection *conn,
    struct udscs_message_header *header, const uint8_t *data);
/* Note udscs will destroy the connection in question itself after
   this callback has completed! */
typedef void (*udscs_disconnect_callback)(struct udscs_connection *conn);

/* Create a unix domain socket named name and start listening on it.
   read_callback will get called when a complete message has been
   received, and disconnect_callback when a client is disconnected. */
struct udscs_server *udscs_create_server(const char *socketname,
    udscs_read_callback read_callback,
    udscs_disconnect_callback disconnect_callback);

void udscs_destroy_server(struct udscs_server *server);

/* Connect to a unix domain socket named name. read_callback will get called
   when a complete message has been received, and disconnect_callback when a
   client is disconnected. */
struct udscs_connection *udscs_connect(const char *socketname,
    udscs_read_callback read_callback,
    udscs_disconnect_callback disconnect_callback);

void udscs_destroy_connection(struct udscs_connection *conn);


/* Given an usdcs server or client fill the fd_sets pointed to by readfds and
   writefds for select() usage.

   Return value: value of the highest fd + 1 */
int udscs_server_fill_fds(struct udscs_server *server, fd_set *readfds,
        fd_set *writefds);

int udscs_client_fill_fds(struct udscs_connection *conn, fd_set *readfds,
        fd_set *writefds);

/* Handle any events flagged by select for the given udscs server or client. */
void udscs_server_handle_fds(struct udscs_server *server, fd_set *readfds,
        fd_set *writefds);

void udscs_client_handle_fds(struct udscs_connection *conn, fd_set *readfds,
        fd_set *writefds);


/* Queue the message described by header and header->size bytes of additional
   data bytes for delivery to the vdagent connected through conn.

   Returns 0 on success -1 on error (only happens when malloc fails) */
int udscs_write(struct udscs_connection *conn,
        struct udscs_message_header *header, const uint8_t *data);

/* Like udscs_write, but then send the message to all clients connected to
   the server */
int udscs_server_write_all(struct udscs_server *server,
        struct udscs_message_header *header, const uint8_t *data);

#endif

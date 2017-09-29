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

#include <stdio.h>
#include <stdint.h>
#include <sys/select.h>
#include <sys/socket.h>


/* ---------- Generic bits and client-side API ---------- */

struct udscs_connection;
struct udscs_message_header {
    uint32_t type;
    uint32_t arg1;
    uint32_t arg2;
    uint32_t size;
};

/* Callbacks with this type will be called when a complete message has been
 * received. The callback may call udscs_destroy_connection, in which case
 * *connp must be made NULL (which udscs_destroy_connection takes care of).
 */
typedef void (*udscs_read_callback)(struct udscs_connection **connp,
    struct udscs_message_header *header, uint8_t *data);

/* Callbacks with this type will be called when the connection is disconnected.
 * Note:
 * 1) udscs will destroy the connection in question itself after
 *    this callback has completed!
 * 2) This callback is always called, even if the disconnect is initiated
 *    by the udscs user through returning -1 from a read callback, or
 *    by explictly calling udscs_destroy_connection.
 */
typedef void (*udscs_disconnect_callback)(struct udscs_connection *conn);

/* Connect to a unix domain socket named name. */
struct udscs_connection *udscs_connect(const char *socketname,
    udscs_read_callback read_callback,
    udscs_disconnect_callback disconnect_callback,
    const char * const type_to_string[], int no_types, int debug);

/* The contents of connp will be made NULL. */
void udscs_destroy_connection(struct udscs_connection **connp);

int udscs_client_fill_fds(struct udscs_connection *conn, fd_set *readfds,
    fd_set *writefds);

/* Note the connection may be destroyed (when disconnected) by this call
 * in this case the disconnect calllback will get called before the
 * destruction and the contents of connp will be made NULL.
 */
void udscs_client_handle_fds(struct udscs_connection **connp, fd_set *readfds,
    fd_set *writefds);

/* Queue a message for delivery to the client connected through conn.
 * Returns 0 on success -1 on error (only happens when malloc fails).
 */
int udscs_write(struct udscs_connection *conn, uint32_t type, uint32_t arg1,
        uint32_t arg2, const uint8_t *data, uint32_t size);

/* To associate per connection data with a connection. */
void udscs_set_user_data(struct udscs_connection *conn, void *data);
void *udscs_get_user_data(struct udscs_connection *conn);


#ifndef UDSCS_NO_SERVER

/* ---------- Server-side API ---------- */

struct udscs_server;

/* Callbacks with this type will be called when a new connection to a
 * server is accepted.
 */
typedef void (*udscs_connect_callback)(struct udscs_connection *conn);

/* Create a unix domain socket named name and start listening on it. */
struct udscs_server *udscs_create_server(const char *socketname,
    udscs_connect_callback connect_callback,
    udscs_read_callback read_callback,
    udscs_disconnect_callback disconnect_callback,
    const char * const type_to_string[], int no_types, int debug);

void udscs_destroy_server(struct udscs_server *server);

/* Like udscs_write, but then send the message to all clients connected to
 * the server.
 */
int udscs_server_write_all(struct udscs_server *server,
    uint32_t type, uint32_t arg1, uint32_t arg2,
    const uint8_t *data, uint32_t size);

/* Callback type for udscs_server_for_all_clients. Clients can be disconnected
 * from this callback just like with a read callback.
 */
typedef int (*udscs_for_all_clients_callback)(struct udscs_connection **connp,
    void *priv);

/* Call func for all clients connected to the server, passing through
 * priv to all func calls. Returns the total of the return values from all
 * calls to func.
 */
int udscs_server_for_all_clients(struct udscs_server *server,
    udscs_for_all_clients_callback func, void *priv);

/* Given an udscs server or client fill the fd_sets pointed to by readfds and
 * writefds for select() usage.
 * Return value: value of the highest fd + 1
 */
int udscs_server_fill_fds(struct udscs_server *server, fd_set *readfds,
    fd_set *writefds);

/* Handle any events flagged by select for the given udscs server or client. */
void udscs_server_handle_fds(struct udscs_server *server, fd_set *readfds,
    fd_set *writefds);

/* Returns the peer's user credentials. */
struct ucred udscs_get_peer_cred(struct udscs_connection *conn);

#endif

#endif

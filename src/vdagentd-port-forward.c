/**
 * Copyright Flexible Software Solutions S.L. 2014
 **/

#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <glib.h>
#include "vdagentd-port-forward.h"

typedef struct temp_data {
    fd_set *readfds, *writefds;
    int nfds;
} temp_data;


struct port_forwarder {
    GHashTable *acceptors;
    GHashTable *connections;
    vdagent_port_forwarder_send_command_callback send_command;
    temp_data tmp;
};


static void set_temp_data(port_forwarder *pf, fd_set *readfds, fd_set *writefds)
{
    pf->tmp.readfds = readfds;
    pf->tmp.writefds = writefds;
    pf->tmp.nfds = -1;
}


typedef struct write_buffer write_buffer;
struct write_buffer {
    uint8_t *buff;
    size_t size, pos;
    write_buffer *next;
};


static write_buffer * new_write_buffer(const uint8_t * data, size_t size)
{
    write_buffer * result = (write_buffer *)malloc(sizeof(write_buffer));
    result->buff = (uint8_t *)malloc(size);
    memcpy(result->buff, data, size);
    result->size = size;
    result->pos = 0;
    result->next = NULL;
}


static void delete_write_buffer(write_buffer * buffer)
{
    free(buffer->buff);
    free(buffer);
}


static void delete_write_buffer_recursive(write_buffer * buffer)
{
    if (buffer) {
        delete_write_buffer_recursive(buffer->next);
        delete_write_buffer(buffer);
    }
}


static void add_data_to_write_buffer(write_buffer **bufferp, const uint8_t * data, size_t size)
{
    while(*bufferp) {
        bufferp = &(*bufferp)->next;
    }
    *bufferp = new_write_buffer(data, size);
}


typedef struct connection {
    int socket;
    write_buffer * buffer;
} connection;


static connection *new_connection()
{
    connection *conn = (connection *)malloc(sizeof(connection));
    conn->buffer = NULL;
    return conn;
}


static void delete_connection(gpointer value)
{
    connection * conn = (connection *)value;
    shutdown(conn->socket, SHUT_RDWR);
    close(conn->socket);
    delete_write_buffer_recursive(conn->buffer);
    free(conn);
}


static int get_connection_port(const connection * conn)
{
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    bzero((char *) &addr, addr_len);
    if (!getsockname(conn->socket, (struct sockaddr *)&addr, &addr_len)) {
        return ntohs(addr.sin_port);
    } else {
        return -1;
    }
}


static int get_connection_id(const connection * conn)
{
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    bzero((char *) &addr, addr_len);
    if (!getpeername(conn->socket, (struct sockaddr *)&addr, &addr_len)) {
        return ntohs(addr.sin_port);
    } else {
        return -1;
    }
}


port_forwarder *vdagent_port_forwarder_create(vdagent_port_forwarder_send_command_callback cb)
{
    port_forwarder *pf;

    pf = calloc(1, sizeof(port_forwarder));
    if (pf) {
        pf->send_command = cb;
        pf->acceptors = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                              NULL, delete_connection);
        pf->connections = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                                NULL, delete_connection);
        if (!pf->acceptors || !pf->connections) {
            vdagent_port_forwarder_destroy(pf);
            free(pf);
            pf = NULL;
        }
    }
    return pf;
}


void vdagent_port_forwarder_destroy(port_forwarder *pf)
{
    if (pf) {
        if (pf->acceptors) {
            g_hash_table_destroy(pf->acceptors);
        }
        if (pf->connections) {
            g_hash_table_destroy(pf->connections);
        }
        free(pf);
    }
}


static void add_connection_to_fd_set(gpointer key, gpointer value, gpointer user_data)
{
    connection *conn = (connection *)value;
    port_forwarder *pf = (port_forwarder *)user_data;
    FD_SET(conn->socket, pf->tmp.readfds);
    if (conn->buffer) FD_SET(conn->socket, pf->tmp.writefds);
    if (conn->socket > pf->tmp.nfds) pf->tmp.nfds = conn->socket;
}


int vdagent_port_forwarder_fill_fds(port_forwarder *pf, fd_set *readfds, fd_set *writefds)
{
    set_temp_data(pf, readfds, writefds);
    /* Add acceptors to the read set */
    g_hash_table_foreach(pf->acceptors, add_connection_to_fd_set, pf);
    /* Add connections to the read/write set */
    g_hash_table_foreach(pf->connections, add_connection_to_fd_set, pf);
    return pf->tmp.nfds + 1;
}


static connection *accept_connection(int acceptor)
{
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    int socket;
    connection * conn = NULL;
    bzero((char *) &addr, addr_len);
    socket = accept(acceptor, (struct sockaddr *)&addr, &addr_len);
    if (socket >= 0) {
        fcntl(socket, F_SETFL, O_NONBLOCK);
        conn = new_connection();
        conn->socket = socket;
    }
    return conn;
}


static void check_new_connection(gpointer key, gpointer value, gpointer user_data)
{
    connection *acceptor = (connection *)value, *conn;
    port_forwarder *pf = (port_forwarder *)user_data;
    VDAgentPortForwardConnectMessage msg;

    if (FD_ISSET(acceptor->socket, pf->tmp.readfds)) {
        conn = accept_connection(acceptor->socket);
        if (!conn) {
            /* TODO Error */
            fprintf(stderr, "Failed to accept connection on port %d\n", GPOINTER_TO_UINT(key));
        } else {
            msg.id = get_connection_id(conn);
            msg.port = GPOINTER_TO_UINT(key);
            g_hash_table_insert(pf->connections, GUINT_TO_POINTER(msg.id), conn);
            pf->send_command(VD_AGENT_PORT_FORWARD_CONNECT,
                             (const uint8_t *)&msg, sizeof(msg));
        }
    }
}


static gboolean read_connection(port_forwarder *pf, connection *conn, int id)
{
    const size_t BUFFER_SIZE = 2048;
    uint8_t msg_buffer[BUFFER_SIZE + sizeof(VDAgentPortForwardDataMessage)];
    VDAgentPortForwardDataMessage * msg = (VDAgentPortForwardDataMessage *)msg_buffer;
    VDAgentPortForwardCloseMessage closeMsg;
    int bytes_read;

    bytes_read = read(conn->socket, msg->data, BUFFER_SIZE);
    if (bytes_read <= 0) {
        /* Error or connection closed by peer */
        closeMsg.id = id;
        pf->send_command(VD_AGENT_PORT_FORWARD_CLOSE,
                         (const uint8_t *)&closeMsg, sizeof(closeMsg));
        return TRUE;
    } else {
        msg->id = id;
        msg->size = bytes_read;
        pf->send_command(VD_AGENT_PORT_FORWARD_DATA, msg_buffer,
                         sizeof(VDAgentPortForwardDataMessage) + bytes_read);
        return FALSE;
    }
}


static gboolean write_connection(port_forwarder *pf, connection *conn, int id)
{
    VDAgentPortForwardCloseMessage closeMsg;
    int bytes_written;

    while (conn->buffer) {
        bytes_written = write(conn->socket, &conn->buffer->buff[conn->buffer->pos],
                              conn->buffer->size - conn->buffer->pos);
        if (bytes_written < 0) {
            /* Error*/
            closeMsg.id = id;
            pf->send_command(VD_AGENT_PORT_FORWARD_CLOSE,
                             (const uint8_t *)&closeMsg, sizeof(closeMsg));
            return TRUE;
        } else {
            conn->buffer->pos += bytes_written;
            if (conn->buffer->pos < conn->buffer->size) {
                break;
            } else {
                write_buffer * next = conn->buffer->next;
                delete_write_buffer(conn->buffer);
                conn->buffer = next;
            }
        }
    }

    return FALSE;
}


/* Returns TRUE if the connection must be closed */
static gboolean check_connection_data(gpointer key, gpointer value, gpointer user_data)
{
    connection *conn = (connection *)value;
    port_forwarder *pf = (port_forwarder *)user_data;
    gboolean remove = FALSE;

    if (FD_ISSET(conn->socket, pf->tmp.readfds)) {
        remove = read_connection(pf, conn, GPOINTER_TO_UINT(key));
    }

    if (!remove && FD_ISSET(conn->socket, pf->tmp.writefds)) {
        remove = write_connection(pf, conn, GPOINTER_TO_UINT(key));
    }

    return remove;
}


void vdagent_port_forwarder_handle_fds(port_forwarder *pf, fd_set *readfds, fd_set *writefds)
{
    set_temp_data(pf, readfds, writefds);
    /* Check acceptors */
    g_hash_table_foreach(pf->acceptors, check_new_connection, pf);
    /* Check connections */
    g_hash_table_foreach_remove(pf->connections, check_connection_data, pf);
}


static int listen_to(port_forwarder *pf, uint16_t port)
{
    int sock, reuse_addr = 1;
    struct sockaddr_in addr;
    socklen_t addrLen = sizeof(struct sockaddr_in);

    bzero((char *) &addr, addrLen);
    addr.sin_family = AF_INET;
    inet_aton("127.0.0.1", &addr.sin_addr);
    addr.sin_port = htons(port);
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0 ||
        fcntl(sock, F_SETFL, O_NONBLOCK) ||
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse_addr, sizeof(int)) ||
        bind(sock, (struct sockaddr *) &addr, addrLen) < 0) {
        return 1;
    }
    listen(sock, 5);
    connection *acceptor = new_connection();
    acceptor->socket = sock;

    g_hash_table_insert(pf->acceptors, GUINT_TO_POINTER(port), acceptor);
    return 0;
}


static void read_data(port_forwarder *pf, VDAgentPortForwardDataMessage * msg)
{
    if (msg->size) {
        connection *conn = g_hash_table_lookup(pf->connections, GUINT_TO_POINTER(msg->id));
        if (conn) {
            add_data_to_write_buffer(&conn->buffer, msg->data, msg->size);
        } else {
            /* TODO: Return error to client */
            fprintf(stderr, "Unknown connection %d on data command\n", msg->id);
        }
    }
}


void do_port_forward_command(port_forwarder *pf, VDAgentMessage *message_header, uint8_t *data)
{
    uint16_t port;
    int id;
    switch (message_header->opaque) {
        case VD_AGENT_PORT_FORWARD_LISTEN:
            port = ((VDAgentPortForwardListenMessage *)data)->port;
            if (listen_to(pf, port)) {
                /* TODO: Return error to client */
                fprintf(stderr, "Failed to listen to port %d\n", port);
            }
            break;
        case VD_AGENT_PORT_FORWARD_DATA:
            read_data(pf, (VDAgentPortForwardDataMessage *)data);
            break;
        case VD_AGENT_PORT_FORWARD_CLOSE:
            id = ((VDAgentPortForwardCloseMessage *)data)->id;
            if (!g_hash_table_remove(pf->connections, GUINT_TO_POINTER(id))) {
                /* TODO: Return error to client */
                fprintf(stderr, "Unknown connection %d on close command\n", id);
            }
            break;
        case VD_AGENT_PORT_FORWARD_SHUTDOWN:
            port = ((VDAgentPortForwardShutdownMessage *)data)->port;
            if (!g_hash_table_remove(pf->acceptors, GUINT_TO_POINTER(port))) {
                /* TODO: Return error to client */
                fprintf(stderr, "Not listening to port %d on shutdown command\n", port);
            }
            break;
        default:
            /* TODO: Return error to client */
            fprintf(stderr, "Unknown command\n", port);
            break;
    }
}

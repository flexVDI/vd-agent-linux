/**
 * Copyright Flexible Software Solutions S.L. 2014
 **/

#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <syslog.h>
#include <glib.h>
#include "vdagentd-port-forward.h"

typedef struct current_fds {
    fd_set *readfds, *writefds;
    int nfds;
} current_fds;

struct port_forwarder {
    GHashTable *acceptors;
    GHashTable *connections;
    gboolean client_disconnected;
    vdagent_port_forwarder_send_command_callback send_command;
    current_fds fds;
    int debug;
};

void vdagent_port_forwarder_client_disconnected(port_forwarder* pf)
{
    if (!pf->client_disconnected) {
        syslog(LOG_INFO, "Client disconnected, removing port redirections");
        pf->client_disconnected = TRUE;
        g_hash_table_remove_all(pf->connections);
        g_hash_table_remove_all(pf->acceptors);
    }
}

static void set_current_fds(port_forwarder *pf, fd_set *readfds, fd_set *writefds)
{
    pf->fds.readfds = readfds;
    pf->fds.writefds = writefds;
    pf->fds.nfds = -1;
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
    return result;
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
    if (conn)
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

port_forwarder *vdagent_port_forwarder_create(vdagent_port_forwarder_send_command_callback cb,
                                              int debug)
{
    port_forwarder *pf;

    pf = calloc(1, sizeof(port_forwarder));
    if (pf) {
        pf->client_disconnected = TRUE;
        pf->debug = debug;
        pf->send_command = cb;
        pf->acceptors = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                              NULL, delete_connection);
        pf->connections = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                                NULL, delete_connection);
        if (!pf->acceptors || !pf->connections) {
            vdagent_port_forwarder_destroy(pf);
            pf = NULL;
        }
        if (pf->debug) syslog(LOG_DEBUG, "Port forwarder created");
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
    FD_SET(conn->socket, pf->fds.readfds);
    if (conn->buffer) FD_SET(conn->socket, pf->fds.writefds);
    if (conn->socket > pf->fds.nfds) pf->fds.nfds = conn->socket;
}

int vdagent_port_forwarder_fill_fds(port_forwarder *pf, fd_set *readfds, fd_set *writefds)
{
    set_current_fds(pf, readfds, writefds);
    /* Add acceptors to the read set */
    g_hash_table_foreach(pf->acceptors, add_connection_to_fd_set, pf);
    /* Add connections to the read/write set */
    g_hash_table_foreach(pf->connections, add_connection_to_fd_set, pf);
    return pf->fds.nfds + 1;
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

static void try_send_command(port_forwarder *pf, uint32_t command,
                             const uint8_t *data, uint32_t data_size)
{
    if (pf->debug) syslog(LOG_DEBUG, "Sending command %d with %d bytes", (int)command, (int)data_size);
    if (!pf->client_disconnected && pf->send_command(command, data, data_size) == -1) {
        pf->client_disconnected = TRUE;
        syslog(LOG_INFO, "Client has disconnected");
    }
}

static void check_new_connection(gpointer key, gpointer value, gpointer user_data)
{
    connection *acceptor = (connection *)value, *conn;
    port_forwarder *pf = (port_forwarder *)user_data;
    VDAgentPortForwardConnectMessage msg;
    if (pf->client_disconnected) return;

    if (FD_ISSET(acceptor->socket, pf->fds.readfds)) {
        conn = accept_connection(acceptor->socket);
        if (!conn) {
            syslog(LOG_ERR, "Failed to accept connection on port %d: %m", GPOINTER_TO_UINT(key));
        } else {
            msg.id = get_connection_id(conn);
            if (msg.id == -1) {
                syslog(LOG_ERR, "Failed to accept connection on port %d", GPOINTER_TO_UINT(key));
                delete_connection(conn);
            } else {
                msg.port = GPOINTER_TO_UINT(key);
                g_hash_table_insert(pf->connections, GUINT_TO_POINTER(msg.id), conn);
                try_send_command(pf, VD_AGENT_PORT_FORWARD_CONNECT,
                                 (const uint8_t *)&msg, sizeof(msg));
            }
        }
    }
}

static gboolean read_connection(port_forwarder *pf, connection *conn, int id)
{
    const size_t HEAD_SIZE = sizeof(VDAgentPortForwardDataMessage);
    const size_t BUFFER_SIZE = VD_AGENT_MAX_DATA_SIZE - HEAD_SIZE;
    uint8_t msg_buffer[VD_AGENT_MAX_DATA_SIZE];
    VDAgentPortForwardDataMessage * msg = (VDAgentPortForwardDataMessage *)msg_buffer;
    VDAgentPortForwardCloseMessage closeMsg;
    int bytes_read;

    bytes_read = read(conn->socket, msg->data, BUFFER_SIZE);
    if (bytes_read <= 0) {
        /* Error or connection closed by peer */
        closeMsg.id = id;
        try_send_command(pf, VD_AGENT_PORT_FORWARD_CLOSE,
                         (const uint8_t *)&closeMsg, sizeof(closeMsg));
        return TRUE;
    } else {
        msg->id = id;
        msg->size = bytes_read;
        try_send_command(pf, VD_AGENT_PORT_FORWARD_DATA, msg_buffer,
                         HEAD_SIZE + bytes_read);
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
            /* Error */
            closeMsg.id = id;
            try_send_command(pf, VD_AGENT_PORT_FORWARD_CLOSE,
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
    gboolean remove = pf->client_disconnected;

    if (!remove && FD_ISSET(conn->socket, pf->fds.readfds)) {
        remove = read_connection(pf, conn, GPOINTER_TO_UINT(key));
    }

    if (!remove && FD_ISSET(conn->socket, pf->fds.writefds)) {
        remove = write_connection(pf, conn, GPOINTER_TO_UINT(key));
    }

    return remove;
}

void vdagent_port_forwarder_handle_fds(port_forwarder *pf, fd_set *readfds, fd_set *writefds)
{
    set_current_fds(pf, readfds, writefds);
    /* Check acceptors */
    if (!pf->client_disconnected)
        g_hash_table_foreach(pf->acceptors, check_new_connection, pf);
    /* Check connections */
    if (!pf->client_disconnected)
        g_hash_table_foreach_remove(pf->connections, check_connection_data, pf);
    if (pf->client_disconnected) {
        g_hash_table_remove_all(pf->connections);
        g_hash_table_remove_all(pf->acceptors);
    }
}

static int listen_to(port_forwarder *pf, uint16_t port)
{
    int sock, reuse_addr = 1;
    struct sockaddr_in addr;
    socklen_t addrLen = sizeof(struct sockaddr_in);

    if (g_hash_table_lookup(pf->acceptors, GUINT_TO_POINTER(port))) {
        syslog(LOG_INFO, "Already listening to port %d", (int)port);
        return 0;
    }

    bzero((char *) &addr, addrLen);
    addr.sin_family = AF_INET;
    inet_aton("127.0.0.1", &addr.sin_addr);
    addr.sin_port = htons(port);
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0 ||
        fcntl(sock, F_SETFL, O_NONBLOCK) ||
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse_addr, sizeof(int)) ||
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &reuse_addr, sizeof(int)) ||
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
        }
        /* Ignore unknown connections, they happen when data messages arrive before the close
         * command has reached the client.
         */
    }
}

void do_port_forward_command(port_forwarder *pf, uint32_t command, uint8_t *data)
{
    uint16_t port;
    int id;
    if (pf->debug) syslog(LOG_DEBUG, "Receiving command %d", (int)command);
    pf->client_disconnected = FALSE;
    switch (command) {
        case VD_AGENT_PORT_FORWARD_LISTEN:
            port = ((VDAgentPortForwardListenMessage *)data)->port;
            if (listen_to(pf, port)) {
                syslog(LOG_ERR, "Failed to listen to port %d: %m", port);
            }
            break;
        case VD_AGENT_PORT_FORWARD_DATA:
            read_data(pf, (VDAgentPortForwardDataMessage *)data);
            break;
        case VD_AGENT_PORT_FORWARD_CLOSE:
            id = ((VDAgentPortForwardCloseMessage *)data)->id;
            if (pf->debug) syslog(LOG_DEBUG, "Client closed connection %d", id);
            if (!g_hash_table_remove(pf->connections, GUINT_TO_POINTER(id))) {
                syslog(LOG_WARNING, "Unknown connection %d on close command", id);
            }
            break;
        case VD_AGENT_PORT_FORWARD_SHUTDOWN:
            port = ((VDAgentPortForwardShutdownMessage *)data)->port;
            if (port == 0) {
                if (pf->debug) syslog(LOG_DEBUG, "Resetting port forwarder by client");
                g_hash_table_remove_all(pf->connections);
                g_hash_table_remove_all(pf->acceptors);
            } else if (!g_hash_table_remove(pf->acceptors, GUINT_TO_POINTER(port))) {
                syslog(LOG_WARNING, "Not listening to port %d on shutdown command", port);
            }
            break;
        default:
            pf->client_disconnected = TRUE;
            syslog(LOG_WARNING, "Unknown command %d\n", (int)command);
            break;
    }
}

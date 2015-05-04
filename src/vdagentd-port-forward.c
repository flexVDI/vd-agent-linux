/**
 * Copyright Flexible Software Solutions S.L. 2014
 **/

#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
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
    int connected;
    int acked;
    int socket;
    write_buffer * buffer;
    uint32_t data_sent, data_received, ack_interval;
} connection;

static connection *new_connection()
{
    return (connection *)g_malloc0(sizeof(connection));
}

static void delete_connection(gpointer value)
{
    connection * conn = (connection *)value;
    shutdown(conn->socket, SHUT_RDWR);
    close(conn->socket);
    delete_write_buffer_recursive(conn->buffer);
    g_free(conn);
}

static guint32 generate_connection_id()
{
    static guint32 seq = 0;
    return ++seq;
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

#define WINDOW_SIZE 10*1024*1024

static void add_connection_to_fd_set(gpointer key, gpointer value, gpointer user_data)
{
    connection *conn = (connection *)value;
    port_forwarder *pf = (port_forwarder *)user_data;
    if (conn->acked && conn->data_sent < WINDOW_SIZE)
        FD_SET(conn->socket, pf->fds.readfds);
    if (!conn->connected || conn->buffer)
        FD_SET(conn->socket, pf->fds.writefds);
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
        conn->connected = TRUE;
    }
    return conn;
}

static void try_send_command(port_forwarder *pf, uint32_t command,
                             const uint8_t *data, uint32_t data_size)
{
    if (pf->debug)
        syslog(LOG_DEBUG, "Sending command %d with %d bytes", (int)command, (int)data_size);
    if (!pf->client_disconnected && pf->send_command(command, data, data_size) == -1) {
        pf->client_disconnected = TRUE;
        syslog(LOG_INFO, "Client has disconnected");
    }
}

static void check_new_connection(gpointer key, gpointer value, gpointer user_data)
{
    connection *acceptor = (connection *)value, *conn;
    port_forwarder *pf = (port_forwarder *)user_data;
    VDAgentPortForwardAcceptedMessage msg;
    if (pf->client_disconnected) return;

    if (FD_ISSET(acceptor->socket, pf->fds.readfds)) {
        conn = accept_connection(acceptor->socket);
        if (!conn) {
            syslog(LOG_ERR, "Failed to accept connection on port %d: %m",
                   GPOINTER_TO_UINT(key));
        } else {
            msg.id = generate_connection_id();
            msg.ack_interval = WINDOW_SIZE / 2;
            msg.port = GPOINTER_TO_UINT(key);
            g_hash_table_insert(pf->connections, GUINT_TO_POINTER(msg.id), conn);
            try_send_command(pf, VD_AGENT_PORT_FORWARD_ACCEPTED,
                             (const uint8_t *)&msg, sizeof(msg));
        }
    }
}

static gboolean read_connection(port_forwarder *pf, connection *conn, guint32 id)
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
        syslog(LOG_DEBUG, "Read error, returned %d: %m", bytes_read);
        closeMsg.id = id;
        try_send_command(pf, VD_AGENT_PORT_FORWARD_CLOSE,
                         (const uint8_t *)&closeMsg, sizeof(closeMsg));
        return TRUE;
    } else {
        msg->id = id;
        msg->size = bytes_read;
        try_send_command(pf, VD_AGENT_PORT_FORWARD_DATA, msg_buffer,
                         HEAD_SIZE + bytes_read);
        conn->data_sent += bytes_read;
        return FALSE;
    }
}

static gboolean write_connection(port_forwarder *pf, connection *conn, guint32 id)
{
    VDAgentPortForwardCloseMessage closeMsg;
    VDAgentPortForwardAckMessage ackMsg;
    int bytes_written;

    while (conn->buffer) {
        bytes_written = write(conn->socket, &conn->buffer->buff[conn->buffer->pos],
                              conn->buffer->size - conn->buffer->pos);
        if (bytes_written < 0) {
            /* Error */
            syslog(LOG_DEBUG, "Write error, returned %d: %m", bytes_written);
            closeMsg.id = id;
            try_send_command(pf, VD_AGENT_PORT_FORWARD_CLOSE,
                             (const uint8_t *)&closeMsg, sizeof(closeMsg));
            return TRUE;
        } else {
            conn->data_received += bytes_written;
            if (conn->data_received >= conn->ack_interval) {
                ackMsg.id = id;
                ackMsg.size = conn->data_received;
                conn->data_received = 0;
                try_send_command(pf, VD_AGENT_PORT_FORWARD_ACK,
                                (const uint8_t *)&ackMsg, sizeof(ackMsg));
            }
            conn->buffer->pos += bytes_written;
            if (conn->buffer->pos < conn->buffer->size) {
                break;
            } else {
                write_buffer * next = conn->buffer->next;
                delete_write_buffer(conn->buffer);
                conn->buffer = next;
                if (!next && !conn->acked) return TRUE;
            }
        }
    }

    return FALSE;
}

static gboolean finish_connect(port_forwarder *pf, connection *conn, guint32 id)
{
    VDAgentPortForwardCloseMessage closeMsg;
    VDAgentPortForwardAckMessage ackMsg;
    int result = 0;
    socklen_t result_len = sizeof(result);
    if (getsockopt(conn->socket, SOL_SOCKET, SO_ERROR, &result, &result_len) < 0 ||
            result != 0) {
        if (result != 0) errno = result;
        syslog(LOG_DEBUG, "Connection error: %m");
        closeMsg.id = id;
        try_send_command(pf, VD_AGENT_PORT_FORWARD_CLOSE,
                         (const uint8_t *)&closeMsg, sizeof(closeMsg));
        return TRUE;
    }
    conn->connected = conn->acked = TRUE;
    syslog(LOG_DEBUG, "Connection established with id %d", id);
    ackMsg.id = id;
    ackMsg.size = WINDOW_SIZE / 2;
    try_send_command(pf, VD_AGENT_PORT_FORWARD_ACK,
                     (const uint8_t *)&ackMsg, sizeof(ackMsg));
    return FALSE;
}

/* Returns TRUE if the connection must be closed */
static gboolean check_connection_data(gpointer key, gpointer value, gpointer user_data)
{
    connection *conn = (connection *)value;
    port_forwarder *pf = (port_forwarder *)user_data;
    gboolean remove = pf->client_disconnected;
    guint32 id = GPOINTER_TO_UINT(key);

    if (!remove && FD_ISSET(conn->socket, pf->fds.readfds)) {
        remove = read_connection(pf, conn, id);
    }

    if (!remove && FD_ISSET(conn->socket, pf->fds.writefds)) {
        if (conn->connected)
            remove = write_connection(pf, conn, id);
        else
            remove = finish_connect(pf, conn, id);
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

static void listen_to(port_forwarder *pf, VDAgentPortForwardListenMessage *msg)
{
    int sock, reuse_addr = 1;
    struct sockaddr_in addr;
    socklen_t addrLen = sizeof(struct sockaddr_in);
    connection *acceptor;

    if (g_hash_table_lookup(pf->acceptors, GUINT_TO_POINTER(msg->port))) {
        syslog(LOG_INFO, "Already listening to port %d", (int)msg->port);
    } else {
        bzero((char *) &addr, addrLen);
        addr.sin_family = AF_INET;
        // TODO: gethostbyname is potentially blocking...
        struct hostent *host = gethostbyname(msg->bind_address);
        if (!host) {
            syslog(LOG_WARNING, "Host %s not found", msg->bind_address);
            return;
        }
        bcopy((char *)host->h_addr, (char *)&addr.sin_addr.s_addr, host->h_length);
        addr.sin_port = htons(msg->port);
        sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0 ||
            fcntl(sock, F_SETFL, O_NONBLOCK) ||
            setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse_addr, sizeof(int)) ||
            setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &reuse_addr, sizeof(int)) ||
            bind(sock, (struct sockaddr *) &addr, addrLen) < 0) {
            syslog(LOG_ERR, "Failed to listen to address %s, port %d: %m",
                   msg->bind_address, msg->port);
        } else {
            listen(sock, 5);
            acceptor = new_connection();
            acceptor->socket = sock;
            acceptor->acked = acceptor->connected = TRUE;
            g_hash_table_insert(pf->acceptors, GUINT_TO_POINTER(msg->port), acceptor);
        }
    }
}

static void read_data(port_forwarder *pf, VDAgentPortForwardDataMessage *msg)
{
    if (msg->size) {
        connection *conn = g_hash_table_lookup(pf->connections, GUINT_TO_POINTER(msg->id));
        if (conn) {
            add_data_to_write_buffer(&conn->buffer, msg->data, msg->size);
        }
        /* Ignore unknown connections, they happen when data/ack messages arrive before
         * the close command has reached the other side.
         */
    }
}

static void ack_data(port_forwarder *pf, VDAgentPortForwardAckMessage *msg)
{
    connection *conn = g_hash_table_lookup(pf->connections, GUINT_TO_POINTER(msg->id));
    if (conn) {
        if (conn->acked) {
            conn->data_sent -= msg->size;
            if (pf->debug) syslog(LOG_DEBUG, "Connection %d ack %d bytes, %d remaining",
                                  (int)msg->id, (int)msg->size, conn->data_sent);
        } else {
            conn->acked = TRUE;
            conn->ack_interval = msg->size;
        }
    } else {
        syslog(LOG_WARNING, "Unknown connection %d on ACK command", msg->id);
    }
}

static void shutdown_port(port_forwarder *pf, uint16_t port) {
    if (port == 0) {
        if (pf->debug) syslog(LOG_DEBUG, "Resetting port forwarder by client");
        g_hash_table_remove_all(pf->connections);
        g_hash_table_remove_all(pf->acceptors);
    } else if (!g_hash_table_remove(pf->acceptors, GUINT_TO_POINTER(port))) {
        syslog(LOG_WARNING, "Not listening to port %d on shutdown command", port);
    }
}

static void start_closing(port_forwarder *pf, guint32 id)
{
    connection *conn = g_hash_table_lookup(pf->connections, GUINT_TO_POINTER(id));
    if (conn) {
        if (pf->debug) syslog(LOG_DEBUG, "Client closed connection %d", id);
        if (conn->buffer)
            conn->acked = FALSE;
        else
            g_hash_table_remove(pf->connections, GUINT_TO_POINTER(id));
    } else {
        syslog(LOG_WARNING, "Unknown connection %d on close command", id);
    }
}

static void connect_remote(port_forwarder *pf, VDAgentPortForwardConnectMessage *msg)
{
    struct sockaddr_in serv_addr;
    int addr_len = sizeof(serv_addr);
    bzero(&serv_addr, addr_len);

    // TODO: gethostbyname is potentially blocking...
    struct hostent *server = gethostbyname(msg->host);
    if (!server) {
        syslog(LOG_WARNING, "Host %s not found", msg->host);
        return;
    }
    serv_addr.sin_family = AF_INET;
    bcopy((char *)server->h_addr, (char *)&serv_addr.sin_addr.s_addr, server->h_length);
    serv_addr.sin_port = htons(msg->port);

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd >= 0) {
        fcntl(sockfd, F_SETFL, O_NONBLOCK);
        int ret = connect(sockfd, (const struct sockaddr *)&serv_addr, addr_len);
        if (ret < 0 && errno != EINPROGRESS) {
            syslog(LOG_WARNING, "Error connecting to %s:%d", msg->host, msg->port);
            close(sockfd);
            return;
        }
        connection * conn = new_connection();
        conn->socket = sockfd;
        g_hash_table_insert(pf->connections, GUINT_TO_POINTER(msg->id), conn);
        syslog(LOG_DEBUG, "Connecting to %s:%d...", msg->host, msg->port);
    } else {
        syslog(LOG_WARNING, "Error creating socket");
    }
}

void do_port_forward_command(port_forwarder *pf, uint32_t command, uint8_t *data)
{
    uint16_t port;
    guint32 id;
    if (pf->debug) syslog(LOG_DEBUG, "Receiving command %d", (int)command);
    pf->client_disconnected = FALSE;
    switch (command) {
    case VD_AGENT_PORT_FORWARD_LISTEN:
        listen_to(pf, (VDAgentPortForwardListenMessage *)data);
        break;
    case VD_AGENT_PORT_FORWARD_CONNECT:
        connect_remote(pf, (VDAgentPortForwardConnectMessage *)data);
        break;
    case VD_AGENT_PORT_FORWARD_DATA:
        read_data(pf, (VDAgentPortForwardDataMessage *)data);
        break;
    case VD_AGENT_PORT_FORWARD_ACK:
        ack_data(pf, (VDAgentPortForwardAckMessage *)data);
        break;
    case VD_AGENT_PORT_FORWARD_CLOSE:
        id = ((VDAgentPortForwardCloseMessage *)data)->id;
        start_closing(pf, id);
        break;
    case VD_AGENT_PORT_FORWARD_SHUTDOWN:
        port = ((VDAgentPortForwardShutdownMessage *)data)->port;
        shutdown_port(pf, port);
        break;
    default:
        pf->client_disconnected = TRUE;
        syslog(LOG_WARNING, "Unknown command %d\n", (int)command);
        break;
    }
}

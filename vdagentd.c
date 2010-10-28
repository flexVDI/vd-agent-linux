/*  vdagentd.c vdagentd (daemon) code

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
#include <sys/select.h>
#include <sys/stat.h>
#include <spice/vd_agent.h>

#include "udscs.h"
#include "vdagentd-proto.h"
#include "vdagentd-proto-strings.h"
#include "vdagentd-uinput.h"
#include "vdagent-virtio-port.h"
#include "console-kit.h"

struct agent_data {
    char *session;
    int width;
    int height;
};

/* variables */
static const char *portdev = "/dev/virtio-ports/com.redhat.spice.0";
static const char *uinput = "/dev/uinput";
static int debug = 0;
static struct udscs_server *server = NULL;
static struct vdagent_virtio_port *virtio_port = NULL;
static struct console_kit *console_kit = NULL;
static VDAgentMonitorsConfig *mon_config = NULL;
static uint32_t *capabilities = NULL;
static int capabilities_size = 0;
static int uinput_width = 0;
static int uinput_height = 0;
static const char *active_session = NULL;
static struct udscs_connection *active_session_conn = NULL;
static int agent_owns_clipboard = 0;

/* utility functions */
/* vdagentd <-> spice-client communication handling */
static void send_capabilities(struct vdagent_virtio_port *port,
    uint32_t request)
{
    VDAgentAnnounceCapabilities *caps;
    uint32_t size;

    size = sizeof(*caps) + VD_AGENT_CAPS_BYTES;
    caps = calloc(1, size);
    if (!caps) {
        fprintf(stderr,
                "out of memory allocating capabilities array (write)\n");
        return;
    }

    caps->request = request;
    VD_AGENT_SET_CAPABILITY(caps->caps, VD_AGENT_CAP_MOUSE_STATE);
    VD_AGENT_SET_CAPABILITY(caps->caps, VD_AGENT_CAP_MONITORS_CONFIG);
    VD_AGENT_SET_CAPABILITY(caps->caps, VD_AGENT_CAP_REPLY);
    if (console_kit)
        VD_AGENT_SET_CAPABILITY(caps->caps, VD_AGENT_CAP_CLIPBOARD_BY_DEMAND);

    vdagent_virtio_port_write(port, VDP_CLIENT_PORT,
                              VD_AGENT_ANNOUNCE_CAPABILITIES, 0,
                              (uint8_t *)caps, size);
    free(caps);
}

static void do_client_monitors(struct vdagent_virtio_port *port, int port_nr,
    VDAgentMessage *message_header, VDAgentMonitorsConfig *new_monitors)
{
    VDAgentReply reply;
    uint32_t size;

    /* Store monitor config to send to agents when they connect */
    size = sizeof(VDAgentMonitorsConfig) +
           new_monitors->num_of_monitors * sizeof(VDAgentMonConfig);
    if (message_header->size != size) {
        fprintf(stderr, "invalid message size for VDAgentMonitorsConfig\n");
        return;
    }

    if (!mon_config ||
            mon_config->num_of_monitors != new_monitors->num_of_monitors) {
        free(mon_config);
        mon_config = malloc(size);
        if (!mon_config) {
            fprintf(stderr, "out of memory allocating monitors config\n");
            return;
        }
    }
    memcpy(mon_config, new_monitors, size);

    /* Send monitor config to currently connected agents */
    udscs_server_write_all(server, VDAGENTD_MONITORS_CONFIG, 0,
                           (uint8_t *)mon_config, size);

    /* Acknowledge reception of monitors config to spice server / client */
    reply.type  = VD_AGENT_MONITORS_CONFIG;
    reply.error = VD_AGENT_SUCCESS;
    vdagent_virtio_port_write(port, port_nr, VD_AGENT_REPLY, 0,
                              (uint8_t *)&reply, sizeof(reply));
}

static void do_client_capabilities(struct vdagent_virtio_port *port,
    VDAgentMessage *message_header,
    VDAgentAnnounceCapabilities *caps)
{
    capabilities_size = VD_AGENT_CAPS_SIZE_FROM_MSG_SIZE(message_header->size);

    free(capabilities);
    capabilities = malloc(capabilities_size * sizeof(uint32_t));
    if (!capabilities) {
        fprintf(stderr,
                "out of memory allocating capabilities array (read)\n");
        capabilities_size = 0;
        return;
    }
    memcpy(capabilities, caps->caps, capabilities_size * sizeof(uint32_t));
    if (caps->request)
        send_capabilities(port, 0);
}

static void do_client_clipboard(struct vdagent_virtio_port *port,
    VDAgentMessage *message_header, uint8_t *message_data)
{
    uint32_t type = 0, opaque = 0, size = 0;
    uint8_t *data = NULL;

    if (!active_session_conn) {
        fprintf(stderr,
                "Could not find an agent connnection belonging to the "
                "active session, ignoring client clipboard request\n");
        return;
    }

    switch (message_header->type) {
    case VD_AGENT_CLIPBOARD_GRAB:
        type = VDAGENTD_CLIPBOARD_GRAB;
        data = message_data;
        size = message_header->size;
        agent_owns_clipboard = 0;
        break;
    case VD_AGENT_CLIPBOARD_REQUEST: {
        VDAgentClipboardRequest *req = (VDAgentClipboardRequest *)message_data;
        type = VDAGENTD_CLIPBOARD_REQUEST;
        opaque = req->type;
        break;
    }
    case VD_AGENT_CLIPBOARD: {
        VDAgentClipboard *clipboard = (VDAgentClipboard *)message_data;
        type = VDAGENTD_CLIPBOARD_DATA;
        opaque = clipboard->type;
        size = message_header->size - sizeof(VDAgentClipboard);
        data = clipboard->data;
        break;
    }
    case VD_AGENT_CLIPBOARD_RELEASE:
        type = VDAGENTD_CLIPBOARD_RELEASE;
        break;
    }

    udscs_write(active_session_conn, type, opaque, data, size);
}

int virtio_port_read_complete(
        struct vdagent_virtio_port *port,
        VDIChunkHeader *chunk_header,
        VDAgentMessage *message_header,
        uint8_t *data)
{
    uint32_t min_size = 0;

    if (message_header->protocol != VD_AGENT_PROTOCOL) {
        fprintf(stderr, "message with wrong protocol version ignoring\n");
        return 0;
    }

    switch (message_header->type) {
    case VD_AGENT_MOUSE_STATE:
        if (message_header->size != sizeof(VDAgentMouseState))
            goto size_error;
        uinput_do_mouse((VDAgentMouseState *)data, debug > 1);
        break;
    case VD_AGENT_MONITORS_CONFIG:
        if (message_header->size < sizeof(VDAgentMonitorsConfig))
            goto size_error;
        do_client_monitors(port, chunk_header->port, message_header,
                    (VDAgentMonitorsConfig *)data);
        break;
    case VD_AGENT_ANNOUNCE_CAPABILITIES:
        if (message_header->size < sizeof(VDAgentAnnounceCapabilities))
            goto size_error;
        do_client_capabilities(port, message_header,
                        (VDAgentAnnounceCapabilities *)data);
        break;
    case VD_AGENT_CLIPBOARD_GRAB:
    case VD_AGENT_CLIPBOARD_REQUEST:
    case VD_AGENT_CLIPBOARD:
    case VD_AGENT_CLIPBOARD_RELEASE:
        switch (message_header->type) {
        case VD_AGENT_CLIPBOARD_GRAB:
            min_size = sizeof(VDAgentClipboardGrab); break;
        case VD_AGENT_CLIPBOARD_REQUEST:
            min_size = sizeof(VDAgentClipboardRequest); break;
        case VD_AGENT_CLIPBOARD:
            min_size = sizeof(VDAgentClipboard); break;
        }
        if (message_header->size < min_size)
            goto size_error;
        do_client_clipboard(port, message_header, data);
        break;
    default:
        if (debug)
            fprintf(stderr, "unknown message type %d\n", message_header->type);
        break;
    }

    return 0;

size_error:
    fprintf(stderr, "read: invalid message size: %u for message type: %u\n",
                    message_header->size, message_header->type);
    return 0;
}

/* vdagentd <-> vdagent communication handling */
void do_agent_clipboard(struct udscs_connection *conn,
    struct udscs_message_header *header, const uint8_t *data)
{
    if (!VD_AGENT_HAS_CAPABILITY(capabilities, capabilities_size,
                                 VD_AGENT_CAP_CLIPBOARD_BY_DEMAND))
        goto error;

    /* Check that this agent is from the currently active session */
    if (conn != active_session_conn) {
        fprintf(stderr, "Clipboard request from agent which is not in the active session?\n");
        goto error;
    }

    switch (header->type) {
    case VDAGENTD_CLIPBOARD_GRAB:
        vdagent_virtio_port_write(virtio_port, VDP_CLIENT_PORT,
                                  VD_AGENT_CLIPBOARD_GRAB, 0,
                                  data, header->size);
        agent_owns_clipboard = 1;
        break;
    case VDAGENTD_CLIPBOARD_REQUEST: {
        VDAgentClipboardRequest req = { .type = header->opaque };
        vdagent_virtio_port_write(virtio_port, VDP_CLIENT_PORT,
                                  VD_AGENT_CLIPBOARD_REQUEST, 0,
                                  (uint8_t *)&req, sizeof(req));
        break;
    }
    case VDAGENTD_CLIPBOARD_DATA: {
        VDAgentClipboard *clipboard;
        uint32_t size = sizeof(*clipboard) + header->size;

        clipboard = calloc(1, size);
        if (!clipboard) {
            fprintf(stderr,
                    "out of memory allocating clipboard (write)\n");
            return;
        }
        clipboard->type = header->opaque;
        memcpy(clipboard->data, data, header->size);

        vdagent_virtio_port_write(virtio_port, VDP_CLIENT_PORT,
                                  VD_AGENT_CLIPBOARD, 0,
                                  (uint8_t *)clipboard, size);
        free(clipboard);
        break;
    }
    case VDAGENTD_CLIPBOARD_RELEASE:
        vdagent_virtio_port_write(virtio_port, VDP_CLIENT_PORT,
                                  VD_AGENT_CLIPBOARD_RELEASE, 0, NULL, 0);
        agent_owns_clipboard = 0;
        break;
    }

    return;

error:
    if (header->type == VDAGENTD_CLIPBOARD_REQUEST) {
        /* Let the agent know no answer is coming */
        udscs_write(conn, VDAGENTD_CLIPBOARD_DATA,
                    VD_AGENT_CLIPBOARD_NONE, NULL, 0);
    }
}

/* When we open the vdagent virtio channel, the server automatically goes into
   client mouse mode, so we can only have the channel open when we know the
   active session resolution. This function checks that we have an agent in the
   active session, and that it has told us its resolution. If these conditions
   are met it sets the uinput tablet device's resolution and opens the virtio
   channel (if it is not already open). If these conditions are not met, it
   closes both. */
static void check_xorg_resolution(void) {
    struct agent_data *agent_data = udscs_get_user_data(active_session_conn);

    if (agent_data && agent_data->width) {
        /* FIXME objectify uinput and let it handle all this */
        if (agent_data->width != uinput_width || 
            agent_data->height != uinput_height) {
            if (uinput_width)
                uinput_close();
            uinput_setup(uinput, agent_data->width, agent_data->height);
            uinput_width = agent_data->width;
            uinput_height = agent_data->height;
        }
        if (!virtio_port) {
            fprintf(stderr, "opening vdagent virtio channel\n");
            virtio_port = vdagent_virtio_port_create(portdev,
                                                     virtio_port_read_complete,
                                                     NULL);
            if (!virtio_port)
                exit(1);

            send_capabilities(virtio_port, 1);
        }
    } else {
        if (uinput_width) {
            uinput_close();
            uinput_width = uinput_height = 0;
        }
        vdagent_virtio_port_destroy(&virtio_port);
        fprintf(stderr, "closed vdagent virtio channel\n");
    }
}

static int connection_matches_active_session(struct udscs_connection **connp,
    void *priv)
{
    struct udscs_connection **conn_ret = (struct udscs_connection **)priv;
    struct agent_data *agent_data = udscs_get_user_data(*connp);

    /* Check if this connection matches the currently active session */
    if (!agent_data->session || !active_session)
        return 0;
    if (strcmp(agent_data->session, active_session))
        return 0;

    *conn_ret = *connp;
    return 1;
}

void update_active_session_connection(void)
{
    struct udscs_connection *new_conn = NULL;
    int n;

    n = udscs_server_for_all_clients(server, connection_matches_active_session,
                                     (void*)&new_conn);
    if (n != 1)
        new_conn = NULL;

    if (new_conn == active_session_conn)
        return;

    active_session_conn = new_conn;

    if (agent_owns_clipboard) {
        vdagent_virtio_port_write(virtio_port, VDP_CLIENT_PORT,
                                  VD_AGENT_CLIPBOARD_RELEASE, 0, NULL, 0);
        agent_owns_clipboard = 0;
    }

    check_xorg_resolution();    
}

void agent_connect(struct udscs_connection *conn)
{
    uint32_t pid;
    struct agent_data *agent_data;
    
    agent_data = calloc(1, sizeof(*agent_data));
    if (!agent_data) {
        fprintf(stderr, "Out of memory allocating agent data, disconnecting\n");
        udscs_destroy_connection(&conn);
        return;
    }

    pid = udscs_get_peer_cred(conn).pid;
    agent_data->session = console_kit_session_for_pid(console_kit, pid);
    udscs_set_user_data(conn, (void *)agent_data);
    update_active_session_connection();

    if (mon_config)
        udscs_write(conn, VDAGENTD_MONITORS_CONFIG, 0, (uint8_t *)mon_config,
                    sizeof(VDAgentMonitorsConfig) +
                    mon_config->num_of_monitors * sizeof(VDAgentMonConfig));
}

void agent_disconnect(struct udscs_connection *conn)
{
    struct agent_data *agent_data = udscs_get_user_data(conn);

    free(agent_data->session);
    agent_data->session = NULL;
    update_active_session_connection();

    free(agent_data);
}

void agent_read_complete(struct udscs_connection **connp,
    struct udscs_message_header *header, const uint8_t *data)
{
    struct agent_data *agent_data = udscs_get_user_data(*connp);

    switch (header->type) {
    case VDAGENTD_GUEST_XORG_RESOLUTION: {
        struct vdagentd_guest_xorg_resolution *res =
            (struct vdagentd_guest_xorg_resolution *)data;

        if (header->size != sizeof(*res)) {
            fprintf(stderr,
                    "guest xorg resolution message has wrong size, disconnecting agent\n");
            udscs_destroy_connection(connp);
            return;
        }

        agent_data->width  = res->width;
        agent_data->height = res->height;
        check_xorg_resolution();
        break;
    }
    case VDAGENTD_CLIPBOARD_GRAB:
    case VDAGENTD_CLIPBOARD_REQUEST:
    case VDAGENTD_CLIPBOARD_DATA:
    case VDAGENTD_CLIPBOARD_RELEASE:
        do_agent_clipboard(*connp, header, data);
        break;
    default:
        fprintf(stderr, "unknown message from vdagent: %u, ignoring\n",
                header->type);
    }
}

/* main */

static void usage(FILE *fp)
{
    fprintf(fp,
            "vdagentd\n"
            "options:\n"
            "  -h         print this text\n"
            "  -d         print debug messages (and don't daemonize)\n"
            "  -s <port>  set virtio serial port  [%s]\n"
            "  -u <dev>   set uinput device       [%s]\n",
            portdev, uinput);
}

void daemonize(void)
{
    /* detach from terminal */
    switch (fork()) {
    case -1:
        perror("fork");
        exit(1);
    case 0:
        close(0); close(1); close(2);
        setsid();
        open("/dev/null",O_RDWR); dup(0); dup(0);
        break;
    default:
        exit(0);
    }
}

void main_loop(void)
{
    fd_set readfds, writefds;
    int n, nfds, ck_fd = 0;

    /* FIXME catch sigquit and set a flag to quit */
    for (;;) {
        FD_ZERO(&readfds);
        FD_ZERO(&writefds);

        nfds = udscs_server_fill_fds(server, &readfds, &writefds);
        n = vdagent_virtio_port_fill_fds(virtio_port, &readfds, &writefds);
        if (n >= nfds)
            nfds = n + 1;
        if (console_kit) {
            ck_fd = console_kit_get_fd(console_kit);
            FD_SET(ck_fd, &readfds);
            if (ck_fd >= nfds)
                nfds = ck_fd + 1;
        }

        n = select(nfds, &readfds, &writefds, NULL, NULL);
        if (n == -1) {
            if (errno == EINTR)
                continue;
            perror("select");
            exit(1);
        }

        udscs_server_handle_fds(server, &readfds, &writefds);
        vdagent_virtio_port_handle_fds(&virtio_port, &readfds, &writefds);
        if (FD_ISSET(ck_fd, &readfds)) {
            active_session = console_kit_get_active_session(console_kit);
            update_active_session_connection();
            check_xorg_resolution();
        }
    }
}

int main(int argc, char *argv[])
{
    int c;

    for (;;) {
        if (-1 == (c = getopt(argc, argv, "dhx:y:s:u:")))
            break;
        switch (c) {
        case 'd':
            debug++;
            break;
        case 's':
            portdev = optarg;
            break;
        case 'u':
            uinput = optarg;
            break;
        case 'h':
            usage(stdout);
            exit(0);
        default:
            usage(stderr);
            exit(1);
        }
    }

    /* Setup communication with vdagent process(es) */
    server = udscs_create_server(VDAGENTD_SOCKET, agent_connect,
                                 agent_read_complete, agent_disconnect,
                                 vdagentd_messages, VDAGENTD_NO_MESSAGES,
                                 debug? stderr:NULL, stderr);
    if (!server)
        exit(1);
    if (chmod(VDAGENTD_SOCKET, 0666)) {
        fprintf(stderr, "could not change permissions on %s: %s\n",
                VDAGENTD_SOCKET, strerror(errno));
    }

    console_kit = console_kit_create(stderr);
    if (!console_kit)
        fprintf(stderr, "Could not connect to console kit, disabling copy and paste support\n");
    active_session = console_kit_get_active_session(console_kit);

    if (!debug)
        daemonize();

    main_loop();

    udscs_destroy_server(server);

    return 0;
}

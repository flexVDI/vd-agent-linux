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
#include <spice/vd_agent.h>

#include "udscs.h"
#include "vdagentd-proto.h"
#include "vdagentd-uinput.h"
#include "vdagent-virtio-port.h"

/* variables */
static const char *portdev = "/dev/virtio-ports/com.redhat.spice.0";
static const char *uinput = "/dev/uinput";
static int connection_count = 0;
static int debug = 0;
static struct udscs_server *server = NULL;
static struct vdagent_virtio_port *virtio_port = NULL;
static VDAgentMonitorsConfig *mon_config = NULL;
static uint32_t *capabilities = NULL;
static int capabilities_size = 0;

/* vdagent virtio port handling */
static void send_reply(struct vdagent_virtio_port *port, int port_nr,
    uint32_t type, uint32_t error)
{
    VDIChunkHeader chunk_header;
    VDAgentMessage message_header;
    VDAgentReply reply;

    chunk_header.port = port_nr;
    chunk_header.size = sizeof(VDAgentMessage) + sizeof(VDAgentReply);
    message_header.protocol = VD_AGENT_PROTOCOL;
    message_header.type = VD_AGENT_REPLY;
    message_header.opaque = 0;
    message_header.size = sizeof(VDAgentReply);
    reply.type = type;
    reply.error = error;
    vdagent_virtio_port_write(port, &chunk_header, &message_header,
                              (uint8_t *)&reply);
}

static void send_capabilities(struct vdagent_virtio_port *port,
    uint32_t request)
{
    VDIChunkHeader chunk_header;
    VDAgentMessage message_header;
    VDAgentAnnounceCapabilities *caps;
    int size;

    size = sizeof(*caps) + VD_AGENT_CAPS_BYTES;
    caps = calloc(1, size);
    if (!caps) {
        fprintf(stderr,
                "out of memory allocating capabilities array (write)\n");
        return;
    }

    chunk_header.port = VDP_CLIENT_PORT;
    chunk_header.size = sizeof(VDAgentMessage) + size;
    message_header.protocol = VD_AGENT_PROTOCOL;
    message_header.type = VD_AGENT_ANNOUNCE_CAPABILITIES;
    message_header.opaque = 0;
    message_header.size = size;
    caps->request = request;
    VD_AGENT_SET_CAPABILITY(caps->caps, VD_AGENT_CAP_MOUSE_STATE);
    VD_AGENT_SET_CAPABILITY(caps->caps, VD_AGENT_CAP_MONITORS_CONFIG);
    VD_AGENT_SET_CAPABILITY(caps->caps, VD_AGENT_CAP_REPLY);

    vdagent_virtio_port_write(port, &chunk_header, &message_header,
                              (uint8_t *)caps);
    free(caps);
}

static void do_monitors(struct vdagent_virtio_port *port, int port_nr,
    VDAgentMessage *message_header, VDAgentMonitorsConfig *new_monitors)
{
    struct udscs_message_header udscs_header;
    int size;

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
    udscs_header.type = VDAGENTD_MONITORS_CONFIG;
    udscs_header.opaque = 0;
    udscs_header.size = size;
    udscs_server_write_all(server, &udscs_header, (uint8_t *)mon_config);

    /* Acknowledge reception of monitors config to spice server / client */
    send_reply(port, port_nr, VD_AGENT_MONITORS_CONFIG, VD_AGENT_SUCCESS);
}

static void do_capabilities(struct vdagent_virtio_port *port,
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

int virtio_port_read_complete(
        struct vdagent_virtio_port *port,
        VDIChunkHeader *chunk_header,
        VDAgentMessage *message_header,
        uint8_t *data)
{
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
        do_monitors(port, chunk_header->port, message_header,
                    (VDAgentMonitorsConfig *)data);
        break;
    case VD_AGENT_ANNOUNCE_CAPABILITIES:
        if (message_header->size < sizeof(VDAgentAnnounceCapabilities))
            goto size_error;
        do_capabilities(port, message_header,
                        (VDAgentAnnounceCapabilities *)data);
        break;
    default:
        if (debug)
            fprintf(stderr, "unknown message type %d\n", message_header->type);
        break;
    }

    return 0;

size_error:
    fprintf(stderr, "read: invalid message size: %d for message type: %d\n",
                    message_header->size, message_header->type);
    return 0;
}

/* vdagent client handling */

void client_connect(struct udscs_connection *conn)
{
    struct udscs_message_header udscs_header;

    /* We don't create the tablet until we've gotten the xorg resolution
       from the vdagent client */
    connection_count++;

    if (mon_config) {
        udscs_header.type = VDAGENTD_MONITORS_CONFIG;
        udscs_header.opaque = 0;
        udscs_header.size = sizeof(VDAgentMonitorsConfig) +
                        mon_config->num_of_monitors * sizeof(VDAgentMonConfig);
        udscs_write(conn, &udscs_header, (uint8_t *)mon_config);
    }
}

void client_disconnect(struct udscs_connection *conn)
{
    connection_count--;
    if (connection_count == 0) {
        uinput_close();
        vdagent_virtio_port_destroy(&virtio_port);
    }
}

int client_read_complete(struct udscs_connection *conn,
    struct udscs_message_header *header, const uint8_t *data)
{
    switch (header->type) {
    case VDAGENTD_GUEST_XORG_RESOLUTION: {
        struct vdagentd_guest_xorg_resolution *res =
            (struct vdagentd_guest_xorg_resolution *)data;

        if (header->size != sizeof(*res)) {
            fprintf(stderr,
                    "guest xorg resolution message has wrong size, disconnecting client\n");
            return -1;
        }

        /* Now that we know the xorg resolution setup the uinput device */
        uinput_setup(uinput, res->width, res->height);
        /* Now that we have a tablet and thus can forward mouse events,
           we can open the vdagent virtio port. */
        if (!virtio_port) {
            virtio_port = vdagent_virtio_port_create(portdev,
                                                     virtio_port_read_complete,
                                                     NULL);
            if (!virtio_port)
                exit(1);

            send_capabilities(virtio_port, 1);
        }
        break;
    }
    default:
        fprintf(stderr, "unknown message from vdagent client: %u, ignoring\n",
                header->type);
    }

    return 0;
}

/* main */

static void usage(FILE *fp)
{
    fprintf(fp,
            "vdagent -- handle spice agent mouse via uinput\n"
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
    int n, nfds;

    /* FIXME catch sigquit and set a flag to quit */
    for (;;) {
        FD_ZERO(&readfds);
        FD_ZERO(&writefds);

        nfds = udscs_server_fill_fds(server, &readfds, &writefds);
        n = vdagent_virtio_port_fill_fds(virtio_port, &readfds, &writefds);
        if (n >= nfds)
            nfds = n + 1;

        n = select(nfds, &readfds, &writefds, NULL, NULL);
        if (n == -1) {
            if (errno == EINTR)
                continue;
            perror("select");
            exit(1);
        }

        udscs_server_handle_fds(server, &readfds, &writefds);
        vdagent_virtio_port_handle_fds(&virtio_port, &readfds, &writefds);
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
    server = udscs_create_server(VDAGENTD_SOCKET,
                                 client_connect,
                                 client_read_complete,
                                 client_disconnect);
    if (!server)
        exit(1);

    if (!debug)
        daemonize();

    main_loop();

    udscs_destroy_server(server);

    return 0;
}
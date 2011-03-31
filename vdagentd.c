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
#include <signal.h>
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
static const char *logfilename = "/var/log/spice-vdagentd/spice-vdagentd.log";
static const char *pidfilename = "/var/run/spice-vdagentd/spice-vdagentd.pid";
static const char *portdev = "/dev/virtio-ports/com.redhat.spice.0";
static const char *uinput_device = "/dev/uinput";
static int debug = 0;
static struct udscs_server *server = NULL;
static struct vdagent_virtio_port *virtio_port = NULL;
static struct console_kit *console_kit = NULL;
static struct vdagentd_uinput *uinput = NULL;
static VDAgentMonitorsConfig *mon_config = NULL;
static uint32_t *capabilities = NULL;
static int capabilities_size = 0;
static const char *active_session = NULL;
static struct udscs_connection *active_session_conn = NULL;
static int agent_owns_clipboard[256] = { 0, };
static FILE *logfile = NULL;
static int quit = 0;
static int retval = 0;

/* utility functions */
/* vdagentd <-> spice-client communication handling */
static void send_capabilities(struct vdagent_virtio_port *vport,
    uint32_t request)
{
    VDAgentAnnounceCapabilities *caps;
    uint32_t size;

    size = sizeof(*caps) + VD_AGENT_CAPS_BYTES;
    caps = calloc(1, size);
    if (!caps) {
        fprintf(logfile,
                "out of memory allocating capabilities array (write)\n");
        return;
    }

    caps->request = request;
    VD_AGENT_SET_CAPABILITY(caps->caps, VD_AGENT_CAP_MOUSE_STATE);
    VD_AGENT_SET_CAPABILITY(caps->caps, VD_AGENT_CAP_MONITORS_CONFIG);
    VD_AGENT_SET_CAPABILITY(caps->caps, VD_AGENT_CAP_REPLY);
    VD_AGENT_SET_CAPABILITY(caps->caps, VD_AGENT_CAP_CLIPBOARD_BY_DEMAND);
    VD_AGENT_SET_CAPABILITY(caps->caps, VD_AGENT_CAP_CLIPBOARD_SELECTION);

    vdagent_virtio_port_write(vport, VDP_CLIENT_PORT,
                              VD_AGENT_ANNOUNCE_CAPABILITIES, 0,
                              (uint8_t *)caps, size);
    free(caps);
}

static void do_client_monitors(struct vdagent_virtio_port *vport, int port_nr,
    VDAgentMessage *message_header, VDAgentMonitorsConfig *new_monitors)
{
    VDAgentReply reply;
    uint32_t size;

    /* Store monitor config to send to agents when they connect */
    size = sizeof(VDAgentMonitorsConfig) +
           new_monitors->num_of_monitors * sizeof(VDAgentMonConfig);
    if (message_header->size != size) {
        fprintf(logfile, "invalid message size for VDAgentMonitorsConfig\n");
        return;
    }

    if (!mon_config ||
            mon_config->num_of_monitors != new_monitors->num_of_monitors) {
        free(mon_config);
        mon_config = malloc(size);
        if (!mon_config) {
            fprintf(logfile, "out of memory allocating monitors config\n");
            return;
        }
    }
    memcpy(mon_config, new_monitors, size);

    /* Send monitor config to currently connected agents */
    udscs_server_write_all(server, VDAGENTD_MONITORS_CONFIG, 0, 0,
                           (uint8_t *)mon_config, size);

    /* Acknowledge reception of monitors config to spice server / client */
    reply.type  = VD_AGENT_MONITORS_CONFIG;
    reply.error = VD_AGENT_SUCCESS;
    vdagent_virtio_port_write(vport, port_nr, VD_AGENT_REPLY, 0,
                              (uint8_t *)&reply, sizeof(reply));
}

static void do_client_capabilities(struct vdagent_virtio_port *vport,
    VDAgentMessage *message_header,
    VDAgentAnnounceCapabilities *caps)
{
    int new_size = VD_AGENT_CAPS_SIZE_FROM_MSG_SIZE(message_header->size);

    if (capabilities_size != new_size) {
        capabilities_size = new_size;
        free(capabilities);
        capabilities = malloc(capabilities_size * sizeof(uint32_t));
        if (!capabilities) {
            fprintf(logfile,
                    "out of memory allocating capabilities array (read)\n");
            capabilities_size = 0;
            return;
        }
    }
    memcpy(capabilities, caps->caps, capabilities_size * sizeof(uint32_t));
    if (caps->request)
        send_capabilities(vport, 0);
}

static void do_client_clipboard(struct vdagent_virtio_port *vport,
    VDAgentMessage *message_header, uint8_t *data)
{
    uint32_t msg_type = 0, data_type = 0, size = message_header->size;
    uint8_t selection = VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD;

    if (!active_session_conn) {
        fprintf(logfile,
                "Could not find an agent connnection belonging to the "
                "active session, ignoring client clipboard request\n");
        return;
    }

    if (VD_AGENT_HAS_CAPABILITY(capabilities, capabilities_size,
                                VD_AGENT_CAP_CLIPBOARD_SELECTION)) {
      selection = data[0];
      data += 4;
      size -= 4;
    }

    switch (message_header->type) {
    case VD_AGENT_CLIPBOARD_GRAB:
        msg_type = VDAGENTD_CLIPBOARD_GRAB;
        agent_owns_clipboard[selection] = 0;
        break;
    case VD_AGENT_CLIPBOARD_REQUEST: {
        VDAgentClipboardRequest *req = (VDAgentClipboardRequest *)data;
        msg_type = VDAGENTD_CLIPBOARD_REQUEST;
        data_type = req->type;
        data = NULL;
        size = 0;
        break;
    }
    case VD_AGENT_CLIPBOARD: {
        VDAgentClipboard *clipboard = (VDAgentClipboard *)data;
        msg_type = VDAGENTD_CLIPBOARD_DATA;
        data_type = clipboard->type;
        size = size - sizeof(VDAgentClipboard);
        data = clipboard->data;
        break;
    }
    case VD_AGENT_CLIPBOARD_RELEASE:
        msg_type = VDAGENTD_CLIPBOARD_RELEASE;
        data = NULL;
        size = 0;
        break;
    }

    udscs_write(active_session_conn, msg_type, selection, data_type,
                data, size);
}

int virtio_port_read_complete(
        struct vdagent_virtio_port *vport,
        int port_nr,
        VDAgentMessage *message_header,
        uint8_t *data)
{
    uint32_t min_size = 0;

    if (message_header->protocol != VD_AGENT_PROTOCOL) {
        fprintf(logfile, "message with wrong protocol version ignoring\n");
        return 0;
    }

    switch (message_header->type) {
    case VD_AGENT_MOUSE_STATE:
        if (message_header->size != sizeof(VDAgentMouseState))
            goto size_error;
        vdagentd_uinput_do_mouse(&uinput, (VDAgentMouseState *)data);
        if (!uinput) {
            /* Try to re-open the tablet */
            struct agent_data *agent_data =
                udscs_get_user_data(active_session_conn);
            if (agent_data)
                uinput = vdagentd_uinput_create(uinput_device,
                                                agent_data->width,
                                                agent_data->height,
                                                logfile, debug > 1);
            if (!uinput) {
                fprintf(logfile, "Fatal uinput error\n");
                retval = 1;
                quit = 1;
            }
        }
        break;
    case VD_AGENT_MONITORS_CONFIG:
        if (message_header->size < sizeof(VDAgentMonitorsConfig))
            goto size_error;
        do_client_monitors(vport, port_nr, message_header,
                    (VDAgentMonitorsConfig *)data);
        break;
    case VD_AGENT_ANNOUNCE_CAPABILITIES:
        if (message_header->size < sizeof(VDAgentAnnounceCapabilities))
            goto size_error;
        do_client_capabilities(vport, message_header,
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
        if (VD_AGENT_HAS_CAPABILITY(capabilities, capabilities_size,
                                    VD_AGENT_CAP_CLIPBOARD_SELECTION)) {
            min_size += 4;
        }
        if (message_header->size < min_size) {
            goto size_error;
        }
        do_client_clipboard(vport, message_header, data);
        break;
    default:
        if (debug)
            fprintf(logfile, "unknown message type %d\n", message_header->type);
        break;
    }

    return 0;

size_error:
    fprintf(logfile, "read: invalid message size: %u for message type: %u\n",
                    message_header->size, message_header->type);
    return 0;
}

/* vdagentd <-> vdagent communication handling */
int do_agent_clipboard(struct udscs_connection *conn,
        struct udscs_message_header *header, const uint8_t *data)
{
    const uint8_t *msg;
    uint8_t *buf = NULL, selection = header->arg1;
    uint32_t msg_type = 0, data_type = -1, size = header->size;

    if (!VD_AGENT_HAS_CAPABILITY(capabilities, capabilities_size,
                                 VD_AGENT_CAP_CLIPBOARD_BY_DEMAND))
        goto error;

    /* Check that this agent is from the currently active session */
    if (conn != active_session_conn) {
        fprintf(logfile, "Clipboard request from agent "
                         "which is not in the active session?\n");
        goto error;
    }

    if (!virtio_port) {
        fprintf(logfile,
                "Clipboard request from agent but no client connection\n");
        goto error;
    }

    if (!VD_AGENT_HAS_CAPABILITY(capabilities, capabilities_size,
                                 VD_AGENT_CAP_CLIPBOARD_SELECTION) &&
            selection != VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD) {
        goto error;
    }

    switch (header->type) {
    case VDAGENTD_CLIPBOARD_GRAB:
        msg_type = VD_AGENT_CLIPBOARD_GRAB;
        agent_owns_clipboard[selection] = 1;
        break;
    case VDAGENTD_CLIPBOARD_REQUEST:
        msg_type = VD_AGENT_CLIPBOARD_REQUEST;
        data_type = header->arg2;
        size = 0;
        break;
    case VDAGENTD_CLIPBOARD_DATA:
        msg_type = VD_AGENT_CLIPBOARD;
        data_type = header->arg2;
        break;
    case VDAGENTD_CLIPBOARD_RELEASE:
        msg_type = VD_AGENT_CLIPBOARD_RELEASE;
        size = 0;
        agent_owns_clipboard[selection] = 0;
        break;
    }

    if (size != header->size) {
        fprintf(logfile,
            "unexpected extra data in clipboard msg, disconnecting agent\n");
        return -1;
    }

    if (VD_AGENT_HAS_CAPABILITY(capabilities, capabilities_size,
                                VD_AGENT_CAP_CLIPBOARD_SELECTION)) {
        size += 4;
    }
    if (data_type != -1) {
        size += 4;
    }
    if (size != header->size) {
        uint8_t *p;
        buf = p = malloc(size);
        if (!buf) {
            fprintf(logfile, "out of memory allocating clipboard (write)\n");
            return -1;
        }
        if (VD_AGENT_HAS_CAPABILITY(capabilities, capabilities_size,
                                    VD_AGENT_CAP_CLIPBOARD_SELECTION)) {
            p[0] = selection;
            p[1] = 0;
            p[2] = 0;
            p[3] = 0;
            p += 4;
        }
        if (data_type != -1) {
            uint32_t *u = (uint32_t *)p;
            u[0] = data_type;
            p += 4;
        }
        memcpy(p, data, header->size);
        msg = buf;
    } else {
        msg = data;
    }

    vdagent_virtio_port_write(virtio_port, VDP_CLIENT_PORT, msg_type,
                              0, msg, size);
    free(buf);
    return 0;

error:
    if (header->type == VDAGENTD_CLIPBOARD_REQUEST) {
        /* Let the agent know no answer is coming */
        udscs_write(conn, VDAGENTD_CLIPBOARD_DATA,
                    selection, VD_AGENT_CLIPBOARD_NONE, NULL, 0);
    }
    return 0;
}

/* When we open the vdagent virtio channel, the server automatically goes into
   client mouse mode, so we can only have the channel open when we know the
   active session resolution. This function checks that we have an agent in the
   active session, and that it has told us its resolution. If these conditions
   are met it sets the uinput tablet device's resolution and opens the virtio
   channel (if it is not already open). If these conditions are not met, it
   closes both. */
static void check_xorg_resolution(void)
{
    struct agent_data *agent_data = udscs_get_user_data(active_session_conn);

    if (agent_data && agent_data->width) {
        if (!uinput)
            uinput = vdagentd_uinput_create(uinput_device,
                                            agent_data->width,
                                            agent_data->height,
                                            logfile, debug > 1);
        else
            vdagentd_uinput_update_size(&uinput, agent_data->width,
                                        agent_data->height);
        if (!uinput) {
            fprintf(logfile, "Fatal uinput error\n");
            retval = 1;
            quit = 1;
            return;
        }

        if (!virtio_port) {
            fprintf(logfile, "opening vdagent virtio channel\n");
            virtio_port = vdagent_virtio_port_create(portdev,
                                                     virtio_port_read_complete,
                                                     NULL, logfile);
            if (!virtio_port) {
                fprintf(logfile,
                        "Fatal error opening vdagent virtio channel\n");
                retval = 1;
                quit = 1;
                return;
            }
            send_capabilities(virtio_port, 1);
        }
    } else {
        vdagentd_uinput_destroy(&uinput);
        if (virtio_port) {
            vdagent_virtio_port_flush(&virtio_port);
            vdagent_virtio_port_destroy(&virtio_port);
            fprintf(logfile, "closed vdagent virtio channel\n");
        }
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

void release_clipboards(void)
{
    uint8_t sel;

    for (sel = 0; sel < VD_AGENT_CLIPBOARD_SELECTION_SECONDARY; ++sel) {
        if (agent_owns_clipboard[sel] && virtio_port) {
            vdagent_virtio_port_write(virtio_port, VDP_CLIENT_PORT,
                                      VD_AGENT_CLIPBOARD_RELEASE, 0, &sel, 1);
        }
        agent_owns_clipboard[sel] = 0;
    }
}

void update_active_session_connection(void)
{
    struct udscs_connection *new_conn = NULL;
    int n;

    if (!active_session)
        active_session = console_kit_get_active_session(console_kit);

    n = udscs_server_for_all_clients(server, connection_matches_active_session,
                                     (void*)&new_conn);
    if (n != 1)
        new_conn = NULL;

    if (new_conn == active_session_conn)
        return;

    active_session_conn = new_conn;

    release_clipboards();

    check_xorg_resolution();    
}

void agent_connect(struct udscs_connection *conn)
{
    uint32_t pid;
    struct agent_data *agent_data;
    
    agent_data = calloc(1, sizeof(*agent_data));
    if (!agent_data) {
        fprintf(logfile, "Out of memory allocating agent data, disconnecting\n");
        udscs_destroy_connection(&conn);
        return;
    }

    pid = udscs_get_peer_cred(conn).pid;
    agent_data->session = console_kit_session_for_pid(console_kit, pid);
    udscs_set_user_data(conn, (void *)agent_data);
    update_active_session_connection();

    if (mon_config)
        udscs_write(conn, VDAGENTD_MONITORS_CONFIG, 0, 0,
                    (uint8_t *)mon_config, sizeof(VDAgentMonitorsConfig) +
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
    struct udscs_message_header *header, uint8_t *data)
{
    struct agent_data *agent_data = udscs_get_user_data(*connp);

    switch (header->type) {
    case VDAGENTD_GUEST_XORG_RESOLUTION: {
        struct vdagentd_guest_xorg_resolution *res =
            (struct vdagentd_guest_xorg_resolution *)data;

        if (header->size != sizeof(*res)) {
            fprintf(logfile,
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
        if (do_agent_clipboard(*connp, header, data)) {
            udscs_destroy_connection(connp);
            return;
        }
        break;
    default:
        fprintf(logfile, "unknown message from vdagent: %u, ignoring\n",
                header->type);
    }
    free(data);
}

/* main */

static void usage(FILE *fp)
{
    fprintf(fp,
            "vdagentd\n"
            "options:\n"
            "  -h         print this text\n"
            "  -d         log debug messages (use twice for extra info)\n"
            "  -s <port>  set virtio serial port  [%s]\n"
            "  -u <dev>   set uinput device       [%s]\n"
            "  -x         don't daemonize (and log to logfile)\n",
            portdev, uinput_device);
}

void daemonize(void)
{
    int x;
    FILE *pidfile;

    /* detach from terminal */
    switch (fork()) {
    case 0:
        close(0); close(1); close(2);
        setsid();
        x = open("/dev/null", O_RDWR); dup(x); dup(x);
        pidfile = fopen(pidfilename, "w");
        if (pidfile) {
            fprintf(pidfile, "%d\n", (int)getpid());
            fclose(pidfile);
        }
        break;
    case -1:
        fprintf(logfile, "fork: %s\n", strerror(errno));
        retval = 1;
    default:
        udscs_destroy_server(server);
        if (logfile != stderr)
            fclose(logfile);
        exit(retval);
    }
}

void main_loop(void)
{
    fd_set readfds, writefds;
    int n, nfds, ck_fd = 0;

    while (!quit) {
        FD_ZERO(&readfds);
        FD_ZERO(&writefds);

        nfds = udscs_server_fill_fds(server, &readfds, &writefds);
        n = vdagent_virtio_port_fill_fds(virtio_port, &readfds, &writefds);
        if (n >= nfds)
            nfds = n + 1;

        ck_fd = console_kit_get_fd(console_kit);
        FD_SET(ck_fd, &readfds);
        if (ck_fd >= nfds)
            nfds = ck_fd + 1;

        n = select(nfds, &readfds, &writefds, NULL, NULL);
        if (n == -1) {
            if (errno == EINTR)
                continue;
            fprintf(logfile, "Fatal error select: %s\n", strerror(errno));
            retval = 1;
            break;
        }

        udscs_server_handle_fds(server, &readfds, &writefds);

        if (virtio_port) {
            vdagent_virtio_port_handle_fds(&virtio_port, &readfds, &writefds);
            if (!virtio_port) {
                fprintf(logfile,
                        "AIIEEE lost spice client connection, reconnecting\n");
                virtio_port = vdagent_virtio_port_create(portdev,
                                                     virtio_port_read_complete,
                                                     NULL, logfile);
            }
            if (!virtio_port) {
                fprintf(logfile,
                        "Fatal error opening vdagent virtio channel\n");
                retval = 1;
                break;
            }
        }

        if (FD_ISSET(ck_fd, &readfds)) {
            active_session = console_kit_get_active_session(console_kit);
            update_active_session_connection();
        }
        fflush(logfile);
    }
}

static void quit_handler(int sig)
{
    quit = 1;
}

int main(int argc, char *argv[])
{
    int c;
    int do_daemonize = 1;
    struct sigaction act;

    for (;;) {
        if (-1 == (c = getopt(argc, argv, "-dhxs:u:")))
            break;
        switch (c) {
        case 'd':
            debug++;
            break;
        case 's':
            portdev = optarg;
            break;
        case 'u':
            uinput_device = optarg;
            break;
        case 'x':
            do_daemonize = 0;
            break;
        case 'h':
            usage(stdout);
            return 0;
        default:
            usage(stderr);
            return 1;
        }
    }

    memset(&act, 0, sizeof(act));
    act.sa_flags = SA_RESTART;
    act.sa_handler = quit_handler;
    sigaction(SIGINT, &act, NULL);
    sigaction(SIGHUP, &act, NULL);
    sigaction(SIGTERM, &act, NULL);
    sigaction(SIGQUIT, &act, NULL);

    if (do_daemonize) {
        logfile = fopen(logfilename, "a");
        if (!logfile) {
            fprintf(stderr, "Error opening %s: %s\n", logfilename,
                    strerror(errno));
            logfile = stderr;
        }
    } else
        logfile = stderr;

    /* Setup communication with vdagent process(es) */
    server = udscs_create_server(VDAGENTD_SOCKET, agent_connect,
                                 agent_read_complete, agent_disconnect,
                                 vdagentd_messages, VDAGENTD_NO_MESSAGES,
                                 debug? logfile:NULL, logfile);
    if (!server) {
        fprintf(logfile, "Fatal could not create server socket %s\n",
                VDAGENTD_SOCKET);
        if (logfile != stderr)
            fclose(logfile);
        return 1;
    }
    if (chmod(VDAGENTD_SOCKET, 0666)) {
        fprintf(logfile, "Fatal could not change permissions on %s: %s\n",
                VDAGENTD_SOCKET, strerror(errno));
        udscs_destroy_server(server);
        if (logfile != stderr)
            fclose(logfile);
        return 1;
    }

    if (do_daemonize)
        daemonize();

    console_kit = console_kit_create(logfile);
    if (!console_kit) {
        fprintf(logfile, "Fatal could not connect to console kit\n");
        udscs_destroy_server(server);
        if (logfile != stderr)
            fclose(logfile);
        return 1;
    }

    main_loop();

    release_clipboards();

    vdagentd_uinput_destroy(&uinput);
    vdagent_virtio_port_flush(&virtio_port);
    vdagent_virtio_port_destroy(&virtio_port);
    console_kit_destroy(console_kit);
    udscs_destroy_server(server);
    if (unlink(VDAGENTD_SOCKET) != 0)
        fprintf(logfile, "unlink %s: %s\n", VDAGENTD_SOCKET, strerror(errno));
    fprintf(logfile, "vdagentd quiting, returning status %d\n", retval);
    if (logfile != stderr)
        fclose(logfile);

    if (do_daemonize)
        unlink(pidfilename);

    return retval;
}

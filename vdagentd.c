#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/select.h>

#include <linux/input.h>
#include <linux/uinput.h>

/* spice structs */

#include <spice/vd_agent.h>

#include "udscs.h"
#include "vdagentd-proto.h"
#include "vdagent-virtio-port.h"

/* variables */
static const char *portdev = "/dev/virtio-ports/com.redhat.spice.0";
static const char *uinput = "/dev/uinput";

static int tablet = -1;
static int connection_count = 0;
static int debug = 0;
static int width, height;
static struct udscs_server *server = NULL;
static struct vdagent_virtio_port *virtio_port = NULL;
static VDAgentMonitorsConfig *mon_config = NULL;

int virtio_port_read_complete(
        struct vdagent_virtio_port *port,
        VDIChunkHeader *chunk_header,
        VDAgentMessage *message_header,
        uint8_t *data);

/* uinput */
static void uinput_setup(void)
{
    struct uinput_user_dev device = {
        .name = "spice vdagent tablet",
        .absmax  [ ABS_X ] = width,
        .absmax  [ ABS_Y ] = height,
    };
    int rc;

    if (tablet != -1)
        close(tablet);

    tablet = open(uinput, O_RDWR);
    if (tablet == -1) {
        fprintf(stderr, "open %s: %s\n", uinput, strerror(errno));
        exit(1);
    }

    rc = write(tablet, &device, sizeof(device));
    if (rc != sizeof(device)) {
        fprintf(stderr, "%s: write error\n", __FUNCTION__);
        exit(1);
    }

    /* buttons */
    ioctl(tablet, UI_SET_EVBIT, EV_KEY);
    ioctl(tablet, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(tablet, UI_SET_KEYBIT, BTN_MIDDLE);
    ioctl(tablet, UI_SET_KEYBIT, BTN_RIGHT);

    /* wheel */
    ioctl(tablet, UI_SET_EVBIT, EV_REL);
    ioctl(tablet, UI_SET_RELBIT, REL_WHEEL);

    /* abs ptr */
    ioctl(tablet, UI_SET_EVBIT, EV_ABS);
    ioctl(tablet, UI_SET_ABSBIT, ABS_X);
    ioctl(tablet, UI_SET_ABSBIT, ABS_Y);

    rc = ioctl(tablet, UI_DEV_CREATE);
    if (rc < 0) {
        fprintf(stderr, "%s: create error\n", __FUNCTION__);
        exit(1);
    }

    /* Now that we have a tablet and thus can forward mouse events,
       we can open the vdagent virtio port. */
    if (!virtio_port) {
        virtio_port = vdagent_virtio_port_create(portdev,
                                                 virtio_port_read_complete,
                                                 NULL);
        if (!virtio_port)
            exit(1);
    }
}

static void uinput_send_event(__u16 type, __u16 code, __s32 value)
{
    struct input_event event = {
        .type  = type,
        .code  = code,
        .value = value,
    };
    int rc;

    rc = write(tablet, &event, sizeof(event));
    if (rc != sizeof(event)) {
        fprintf(stderr, "%s: write error\n", __FUNCTION__);
        exit(1);
    }
}

/* spice port */

static void do_mouse(VDAgentMouseState *mouse)
{
    struct button_s {
        const char *name;
        int mask;
        int btn;
    };
    static const struct button_s btns[] = {
        { .name = "left",   .mask =  VD_AGENT_LBUTTON_MASK, .btn = BTN_LEFT      },
        { .name = "middle", .mask =  VD_AGENT_MBUTTON_MASK, .btn = BTN_MIDDLE    },
        { .name = "right",  .mask =  VD_AGENT_RBUTTON_MASK, .btn = BTN_RIGHT     },
    };
    static const struct button_s wheel[] = {
        { .name = "up",     .mask =  VD_AGENT_UBUTTON_MASK, .btn = 1  },
        { .name = "down",   .mask =  VD_AGENT_DBUTTON_MASK, .btn = -1 },
    };
    static VDAgentMouseState last;
    int i, down;

    if (last.x != mouse->x) {
        if (debug > 1)
            fprintf(stderr, "mouse: abs-x %d\n", mouse->x);
        uinput_send_event(EV_ABS, ABS_X, mouse->x);
    }
    if (last.y != mouse->y) {
        if (debug > 1)
            fprintf(stderr, "mouse: abs-y %d\n", mouse->y);
        uinput_send_event(EV_ABS, ABS_Y, mouse->y);
    }
    for (i = 0; i < sizeof(btns)/sizeof(btns[0]); i++) {
        if ((last.buttons & btns[i].mask) == (mouse->buttons & btns[i].mask))
            continue;
        down = !!(mouse->buttons & btns[i].mask);
        if (debug > 1)
            fprintf(stderr, "mouse: btn-%s %s\n",
                    btns[i].name, down ? "down" : "up");
        uinput_send_event(EV_KEY, btns[i].btn, down);
    }
    for (i = 0; i < sizeof(wheel)/sizeof(wheel[0]); i++) {
        if ((last.buttons & wheel[i].mask) == (mouse->buttons & wheel[i].mask))
            continue;
        if (mouse->buttons & wheel[i].mask) {
            if (debug > 1)
                fprintf(stderr, "mouse: wheel-%s\n", wheel[i].name);
            uinput_send_event(EV_REL, REL_WHEEL, wheel[i].btn);
        }
    }
    if (debug > 1)
        fprintf(stderr, "mouse: syn\n");
    uinput_send_event(EV_SYN, SYN_REPORT, 0);

    last = *mouse;
}

static void do_monitors(
    struct vdagent_virtio_port *port,
    VDAgentMonitorsConfig *new_monitors, int port_nr)
{
    VDIChunkHeader chunk_header;
    VDAgentMessage message_header;
    VDAgentReply reply;
    struct udscs_message_header udscs_header;
    int size;

    /* Store monitor config to send to agents when they connect */
    size = sizeof(VDAgentMonitorsConfig) +
           new_monitors->num_of_monitors * sizeof(VDAgentMonConfig);
    if (!mon_config ||
            mon_config->num_of_monitors != new_monitors->num_of_monitors) {
        free(mon_config);
        mon_config = malloc(size);
        if (!mon_config) {
            fprintf(stderr, "out of memory allocting monitors config\n");
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
    chunk_header.port = port_nr;
    chunk_header.size = sizeof(VDAgentMessage) + sizeof(VDAgentReply);
    message_header.protocol = VD_AGENT_PROTOCOL;
    message_header.type = VD_AGENT_REPLY;
    message_header.opaque = 0;
    message_header.size = sizeof(VDAgentReply);
    reply.type = VD_AGENT_MONITORS_CONFIG;
    reply.error = VD_AGENT_SUCCESS;
    vdagent_virtio_port_write(port, &chunk_header, &message_header,
                              (uint8_t *)&reply);
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
        do_mouse((VDAgentMouseState *)data);
        break;
    case VD_AGENT_MONITORS_CONFIG:
        do_monitors(port, (VDAgentMonitorsConfig *)data, chunk_header->port);
        break;
    default:
        if (debug)
            fprintf(stderr, "unknown message type %d\n", message_header->type);
        break;
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
        close(tablet);
        tablet = -1;
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

        width = res->width;
        height = res->height;
        /* Now that we know the xorg resolution setup the uinput device */
        uinput_setup();
        break;
    }
    default:
        fprintf(stderr, "unknown message from vdagent client: %u, ignoring\n",
                header->type);
    }

    return 0;
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

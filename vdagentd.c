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

typedef struct VDAgentHeader {
    uint32_t port;
    uint32_t size;
} VDAgentHeader;

/* variables */

static const char *portdev = "/dev/virtio-ports/com.redhat.spice.0";
static const char *uinput = "/dev/uinput";

static int vdagent, tablet;
static int debug = 0;
static int width = 1024, height = 768; /* FIXME: don't hardcode */
static struct udscs_server *server = NULL;

/* uinput */

static void uinput_setup(void)
{
    struct uinput_user_dev device = {
        .name = "spice vdagent tablet",
        .absmax  [ ABS_X ] = width,
        .absmax  [ ABS_Y ] = height,
    };
    int rc;

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
    ioctl(tablet, UI_SET_KEYBIT, BTN_GEAR_UP);
    ioctl(tablet, UI_SET_KEYBIT, BTN_GEAR_DOWN);

    /* abs ptr */
    ioctl(tablet, UI_SET_EVBIT, EV_ABS);
    ioctl(tablet, UI_SET_ABSBIT, ABS_X);
    ioctl(tablet, UI_SET_ABSBIT, ABS_Y);

    rc = ioctl(tablet, UI_DEV_CREATE);
    if (rc < 0) {
        fprintf(stderr, "%s: create error\n", __FUNCTION__);
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
    static const struct {
        const char *name;
        int mask;
        int btn;
    } btns[] = {
        { .name = "left",   .mask =  VD_AGENT_LBUTTON_MASK, .btn = BTN_LEFT      },
        { .name = "middle", .mask =  VD_AGENT_MBUTTON_MASK, .btn = BTN_MIDDLE    },
        { .name = "right",  .mask =  VD_AGENT_RBUTTON_MASK, .btn = BTN_RIGHT     },
        { .name = "up",     .mask =  VD_AGENT_UBUTTON_MASK, .btn = BTN_GEAR_UP   },
        { .name = "down",   .mask =  VD_AGENT_DBUTTON_MASK, .btn = BTN_GEAR_DOWN },
    };
    static VDAgentMouseState last;
    int i, down;

    if (last.x != mouse->x) {
        if (debug)
            fprintf(stderr, "mouse: abs-x %d\n", mouse->x);
        uinput_send_event(EV_ABS, ABS_X, mouse->x);
    }
    if (last.y != mouse->y) {
        if (debug)
            fprintf(stderr, "mouse: abs-y %d\n", mouse->y);
        uinput_send_event(EV_ABS, ABS_Y, mouse->y);
    }
    for (i = 0; i < sizeof(btns)/sizeof(btns[0]); i++) {
        if ((last.buttons & btns[i].mask) == (mouse->buttons & btns[i].mask))
            continue;
        down = !!(mouse->buttons & btns[i].mask);
        if (debug)
            fprintf(stderr, "mouse: btn-%s %s\n",
                    btns[i].name, down ? "down" : "up");
        uinput_send_event(EV_KEY, btns[i].btn, down);
    }
    if (debug)
        fprintf(stderr, "mouse: syn\n");
    uinput_send_event(EV_SYN, SYN_REPORT, 0);

    last = *mouse;
}

static void do_monitors(VDAgentMonitorsConfig *monitors)
{
    int i;

    if (!debug)
        return;
    fprintf(stderr, "monitors: %d\n", monitors->num_of_monitors);
    for (i = 0; i < monitors->num_of_monitors; i++) {
        fprintf(stderr, "  #%d: size %dx%d pos +%d+%d depth %d\n", i,
                monitors->monitors[i].width, monitors->monitors[i].height,
                monitors->monitors[i].x, monitors->monitors[i].y,
                monitors->monitors[i].depth);
    }
}

void vdagent_read(void)
{
    VDAgentHeader header;
    VDAgentMessage *message;
    void *data;
    int rc;

    rc = read(vdagent, &header, sizeof(header));
    if (rc != sizeof(header)) {
        fprintf(stderr, "vdagent header read error (%d/%zd)\n", rc, sizeof(header));
        exit(1);
    }

    message = malloc(header.size);
    rc = read(vdagent, message, header.size);
    if (rc != header.size) {
        fprintf(stderr, "vdagent message read error (%d/%d)\n", rc, header.size);
        exit(1);
    }
    data = message->data;

    switch (message->type) {
    case VD_AGENT_MOUSE_STATE:
        do_mouse(data);
        break;
    case VD_AGENT_MONITORS_CONFIG:
        do_monitors(data);
        break;
    default:
        if (debug)
            fprintf(stderr, "unknown message type %d\n", message->type);
        break;
    }

    free(message);
}

/* main */

static void usage(FILE *fp)
{
    fprintf(fp,
            "vdagent -- handle spice agent mouse via uinput\n"
            "options:\n"
            "  -h         print this text\n"
            "  -d         print debug messages (and don't daemonize)\n"
            "  -x width   set display width       [%d]\n"
            "  -y height  set display height      [%d]\n"
            "  -s <port>  set virtio serial port  [%s]\n"
            "  -u <dev>   set uinput device       [%s]\n",
            width, height, portdev, uinput);
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

int client_read_complete(struct udscs_connection *conn,
    struct udscs_message_header *header, const uint8_t *data)
{
    switch (header->type) {
    case VDAGENTD_GUEST_XORG_RESOLUTION: {
        struct vdagentd_guest_xorg_resolution *res =
            (struct vdagentd_guest_xorg_resolution *)data;

        if (header->size != sizeof(*res))
            return -1;

        width = res->width;
        height = res->height;
        close(tablet);
        tablet = open(uinput, O_RDWR);
        if (-1 == tablet) {
            fprintf(stderr, "open %s: %s\n", uinput, strerror(errno));
            exit(1);
        }
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

    /* FIXME catch sigterm and stop on it */
    for (;;) {
        FD_ZERO(&readfds);
        FD_ZERO(&writefds);

        nfds = udscs_server_fill_fds(server, &readfds, &writefds);

        FD_SET(vdagent, &readfds);
        if (vdagent >= nfds)
            nfds = vdagent + 1;

        n = select(nfds, &readfds, &writefds, NULL, NULL);
        if (n == -1) {
            if (errno == EINTR)
                continue;
            perror("select");
            exit(1);
        }

        udscs_server_handle_fds(server, &readfds, &writefds);
        if (FD_ISSET(vdagent, &readfds))
            vdagent_read();
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
        case 'x':
            width = atoi(optarg);
            break;
        case 'y':
            height = atoi(optarg);
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

    /* Open virtio port connection */
    vdagent = open(portdev, O_RDWR);
    if (-1 == vdagent) {
        fprintf(stderr, "open %s: %s\n", portdev, strerror(errno));
        exit(1);
    }

    /* Setup communication with vdagent process(es) */
    server = udscs_create_server(VDAGENTD_SOCKET, client_read_complete, NULL);
    if (!server)
        exit(1);

    tablet = open(uinput, O_RDWR);
    if (-1 == tablet) {
        fprintf(stderr, "open %s: %s\n", uinput, strerror(errno));
        exit(1);
    }
    uinput_setup();

    if (!debug)
        daemonize();

    main_loop();

    udscs_destroy_server(server);

    return 0;
}

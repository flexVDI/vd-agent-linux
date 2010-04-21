#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <linux/input.h>
#include <linux/uinput.h>

/* spice structs */

typedef struct __attribute__ ((__packed__)) vmc_header {
    uint32_t port;
    uint32_t size;
} vmc_header;

typedef struct __attribute__ ((__packed__)) vmc_mouse {
    uint32_t x;
    uint32_t y;
    uint32_t buttons;
    uint8_t  display;
} vmc_mouse;

typedef struct __attribute__ ((__packed__)) vmc_message {
    uint32_t protocol;
    uint32_t type;
    uint64_t opaque;
    uint32_t size;
    union {
        vmc_mouse mouse;
    };
} vmc_message;

/* variables */

static const char *portdev = "/dev/virtio-ports/com.redhat.spice.0";
static const char *uinput = "/dev/uinput";

static int vmc, tablet;
static int debug = 0;
static int width = 1024, height = 768; /* FIXME: don't hardcode */

/* uinput */

void uinput_setup(void)
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

void uinput_send_event(__u16 type, __u16 code, __s32 value)
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

void do_mouse(vmc_mouse *mouse)
{
    static const struct {
        const char *name;
        int mask;
        int btn;
    } btns[] = {
        { .name = "left",   .mask =  2, .btn = BTN_LEFT   },
        { .name = "middle", .mask =  4, .btn = BTN_MIDDLE },
        { .name = "right",  .mask =  8, .btn = BTN_RIGHT  },
    };
    static vmc_mouse last;
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

void vmc_read(void)
{
    vmc_header header;
    vmc_message *message;
    int rc;

    rc = read(vmc, &header, sizeof(header));
    if (rc != sizeof(header)) {
        fprintf(stderr, "vmc header read error (%d/%zd)\n", rc, sizeof(header));
        exit(1);
    }

    message = malloc(header.size);
    rc = read(vmc, message, header.size);
    if (rc != header.size) {
        fprintf(stderr, "vmc message read error (%d/%d)\n", rc, header.size);
        exit(1);
    }

    switch (message->type) {
    case 1:
        do_mouse(&message->mouse);
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
            "  -d         don't daemonize, print debug messages\n"
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

    vmc = open(portdev, O_RDWR);
    if (-1 == vmc) {
        fprintf(stderr, "open %s: %s\n", portdev, strerror(errno));
        exit(1);
    }

    tablet = open(uinput, O_RDWR);
    if (-1 == tablet) {
        fprintf(stderr, "open %s: %s\n", uinput, strerror(errno));
        exit(1);
    }
    uinput_setup();

    if (!debug)
        daemonize();
    for (;;) {
        vmc_read();
    }
    return 0;
}

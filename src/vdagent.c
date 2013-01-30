/*  vdagent.c xorg-client to vdagentd (daemon).

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

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <spice/vd_agent.h>
#include <glib.h>

#include "udscs.h"
#include "vdagentd-proto.h"
#include "vdagentd-proto-strings.h"
#include "vdagent-x11.h"

typedef struct AgentFileXferTask {
    uint32_t                       id;
    int                            file_fd;
    uint64_t                       read_bytes;
    char                           *file_name;
    uint64_t                       file_size;
} AgentFileXferTask;

GHashTable *agent_file_xfer_tasks = NULL;

static const char *portdev = "/dev/virtio-ports/com.redhat.spice.0";
static int debug = 0;
static struct vdagent_x11 *x11 = NULL;
static struct udscs_connection *client = NULL;
static int quit = 0;
static int version_mismatch = 0;

static void agent_file_xfer_task_free(gpointer data)
{
    AgentFileXferTask *task = data;

    g_return_if_fail(task != NULL);

    if (task->file_fd > 0) {
        close(task->file_fd);
    }
    g_free(task->file_name);
    g_free(task);
}

/* Parse start messag then create a new file xfer task */
static AgentFileXferTask *vdagent_parse_start_msg(
    VDAgentFileXferStartMessage *msg)
{
    GKeyFile *keyfile = NULL;
    AgentFileXferTask *task = NULL;
    GError *error = NULL;

    keyfile = g_key_file_new();
    if (g_key_file_load_from_data(keyfile,
                                  (const gchar *)msg->data,
                                  -1,
                                  G_KEY_FILE_NONE, &error) == FALSE) {
        syslog(LOG_ERR, "failed to load keyfile from data, error:%s\n",
               error->message);
        goto error;
    }
    task = g_new0(AgentFileXferTask, 1);
    task->id = msg->id;
    task->file_name = g_key_file_get_string(
        keyfile, "vdagent-file-xfer", "name", &error);
    if (error) {
        syslog(LOG_ERR, "failed to parse filename, error:%s\n", error->message);
        goto error;
    }
    task->file_size = g_key_file_get_uint64(
        keyfile, "vdagent-file-xfer", "size", &error);
    if (error) {
        syslog(LOG_ERR, "failed to parse filesize, error:%s\n", error->message);
        goto error;
    }

    g_key_file_free(keyfile);
    return task;

error:
    g_clear_error(&error);
    agent_file_xfer_task_free(task);
    if (keyfile) {
        g_key_file_free(keyfile);
    }
    return NULL;
}

static void vdagent_file_xfer_start(VDAgentFileXferStartMessage *msg)
{
    AgentFileXferTask *new;
    char *file_path;
    const gchar *desktop;

    new = vdagent_parse_start_msg(msg);
    if (new == NULL) {
        goto error;
    }

    desktop = g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP);
    if (desktop == NULL) {
        goto error;
    }
    file_path = g_build_filename(desktop, new->file_name, NULL);
    new->file_fd = open(file_path, O_CREAT | O_WRONLY, 0644);
    g_free(file_path);
    if (new->file_fd == -1) {
        syslog(LOG_ERR, "Create file error:%s\n", strerror(errno));
        goto error;
    }

    if (ftruncate(new->file_fd, new->file_size) < 0) {
        goto error;
    }

    g_hash_table_insert(agent_file_xfer_tasks, GINT_TO_POINTER(msg->id), new);

    udscs_write(client, VDAGENTD_FILE_XFER_STATUS,
                msg->id, VD_AGENT_FILE_XFER_STATUS_CAN_SEND_DATA, NULL, 0);
    return ;

error:
    udscs_write(client, VDAGENTD_FILE_XFER_STATUS,
                msg->id, VD_AGENT_FILE_XFER_STATUS_ERROR, NULL, 0);
    agent_file_xfer_task_free(new);
}

static void vdagent_file_xfer_status(VDAgentFileXferStatusMessage *msg)
{
    syslog(LOG_INFO, "task %d received response %d", msg->id, msg->result);

    if (msg->result == VD_AGENT_FILE_XFER_STATUS_CAN_SEND_DATA) {
        /* Do nothing */
    } else {
        /* Error, remove this task */
        gboolean found;
        found = g_hash_table_remove(agent_file_xfer_tasks, GINT_TO_POINTER(msg->id));
        if (found) {
            syslog(LOG_DEBUG, "remove task %d", msg->id);
        } else {
            syslog(LOG_ERR, "can not find task %d", msg->id);
        }
    }
}

static void vdagent_file_xfer_data(VDAgentFileXferDataMessage *msg)
{
    AgentFileXferTask *task;
    int len;

    task = g_hash_table_lookup(agent_file_xfer_tasks, GINT_TO_POINTER(msg->id));
    if (task == NULL) {
        syslog(LOG_INFO, "Can not find task:%d", msg->id);
        return ;
    }

    len = write(task->file_fd, msg->data, msg->size);
    if (len == -1) {
        syslog(LOG_ERR, "write file error:%s\n", strerror(errno));
        /* TODO: close, cancel dnd */
        return ;
    }

    task->read_bytes += msg->size;
    if (task->read_bytes >= task->file_size) {
        gboolean found;
        if (task->read_bytes > task->file_size) {
            syslog(LOG_ERR, "error: received too much data");
        }
        syslog(LOG_DEBUG, "task %d have been finished", task->id);
        found = g_hash_table_remove(agent_file_xfer_tasks, GINT_TO_POINTER(msg->id));
        if (found) {
            syslog(LOG_DEBUG, "remove task %d", msg->id);
        } else {
            syslog(LOG_ERR, "can not find task %d", msg->id);
        }
    }
}

void daemon_read_complete(struct udscs_connection **connp,
    struct udscs_message_header *header, uint8_t *data)
{
    switch (header->type) {
    case VDAGENTD_MONITORS_CONFIG:
        vdagent_x11_set_monitor_config(x11, (VDAgentMonitorsConfig *)data, 0);
        free(data);
        break;
    case VDAGENTD_CLIPBOARD_REQUEST:
        vdagent_x11_clipboard_request(x11, header->arg1, header->arg2);
        free(data);
        break;
    case VDAGENTD_CLIPBOARD_GRAB:
        vdagent_x11_clipboard_grab(x11, header->arg1, (uint32_t *)data,
                                   header->size / sizeof(uint32_t));
        free(data);
        break;
    case VDAGENTD_CLIPBOARD_DATA:
        vdagent_x11_clipboard_data(x11, header->arg1, header->arg2,
                                   data, header->size);
        /* vdagent_x11_clipboard_data takes ownership of the data (or frees
           it immediately) */
        break;
    case VDAGENTD_CLIPBOARD_RELEASE:
        vdagent_x11_clipboard_release(x11, header->arg1);
        free(data);
        break;
    case VDAGENTD_VERSION:
        if (strcmp((char *)data, VERSION) != 0) {
            syslog(LOG_INFO, "vdagentd version mismatch: got %s expected %s",
                   data, VERSION);
            udscs_destroy_connection(connp);
            version_mismatch = 1;
        }
        break;
    case VDAGENTD_FILE_XFER_START:
        vdagent_file_xfer_start((VDAgentFileXferStartMessage *)data);
        free(data);
        break;
    case VDAGENTD_FILE_XFER_STATUS:
        vdagent_file_xfer_status((VDAgentFileXferStatusMessage *)data);
        free(data);
        break;
    case VDAGENTD_FILE_XFER_DATA:
        vdagent_file_xfer_data((VDAgentFileXferDataMessage *)data);
        free(data);
        break;
    default:
        syslog(LOG_ERR, "Unknown message from vdagentd type: %d, ignoring",
               header->type);
        free(data);
    }
}

int client_setup(int reconnect)
{
    while (!quit) {
        client = udscs_connect(VDAGENTD_SOCKET, daemon_read_complete, NULL,
                               vdagentd_messages, VDAGENTD_NO_MESSAGES,
                               debug);
        if (client || !reconnect || quit) {
            break;
        }
        sleep(1);
    }
    return client == NULL;
}

static void usage(FILE *fp)
{
    fprintf(fp,
            "vdagent -- spice agent xorg client\n"
            "options:\n"
            "  -h         print this text\n"
            "  -d         log debug messages\n"
            "  -s <port>  set virtio serial port  [%s]\n"
            "  -x         don't daemonize\n",
            portdev);
}

static void quit_handler(int sig)
{
    quit = 1;
}

void daemonize(void)
{
    int x, retval = 0;

    /* detach from terminal */
    switch (fork()) {
    case 0:
        close(0); close(1); close(2);
        setsid();
        x = open("/dev/null", O_RDWR); x = dup(x); x = dup(x);
        break;
    case -1:
        syslog(LOG_ERR, "fork: %s", strerror(errno));
        retval = 1;
    default:
        exit(retval);
    }
}

static int file_test(const char *path)
{
    struct stat buffer;

    return stat(path, &buffer);
}

int main(int argc, char *argv[])
{
    fd_set readfds, writefds;
    int c, n, nfds, x11_fd;
    int do_daemonize = 1;
    int x11_sync = 0;
    struct sigaction act;

    for (;;) {
        if (-1 == (c = getopt(argc, argv, "-dxhys:")))
            break;
        switch (c) {
        case 'd':
            debug++;
            break;
        case 's':
            portdev = optarg;
            break;
        case 'x':
            do_daemonize = 0;
            break;
        case 'y':
            x11_sync = 1;
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

    openlog("spice-vdagent", do_daemonize ? LOG_PID : (LOG_PID | LOG_PERROR),
            LOG_USER);

    if (file_test(portdev) != 0) {
        syslog(LOG_ERR, "Missing virtio device '%s': %s",
                portdev, strerror(errno));
        return 1;
    }

    if (do_daemonize)
        daemonize();

    agent_file_xfer_tasks = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, agent_file_xfer_task_free);

reconnect:
    if (version_mismatch) {
        syslog(LOG_INFO, "Version mismatch, restarting");
        sleep(1);
        execvp(argv[0], argv);
    }

    if (client_setup(do_daemonize)) {
        return 1;
    }

    x11 = vdagent_x11_create(client, debug, x11_sync);
    if (!x11) {
        udscs_destroy_connection(&client);
        return 1;
    }

    while (client && !quit) {
        FD_ZERO(&readfds);
        FD_ZERO(&writefds);

        nfds = udscs_client_fill_fds(client, &readfds, &writefds);
        x11_fd = vdagent_x11_get_fd(x11);
        FD_SET(x11_fd, &readfds);
        if (x11_fd >= nfds)
            nfds = x11_fd + 1;

        n = select(nfds, &readfds, &writefds, NULL, NULL);
        if (n == -1) {
            if (errno == EINTR)
                continue;
            syslog(LOG_ERR, "Fatal error select: %s", strerror(errno));
            break;
        }

        if (FD_ISSET(x11_fd, &readfds))
            vdagent_x11_do_read(x11);
        udscs_client_handle_fds(&client, &readfds, &writefds);
    }

    vdagent_x11_destroy(x11, client == NULL);
    udscs_destroy_connection(&client);
    if (!quit)
        goto reconnect;

    return 0;
}

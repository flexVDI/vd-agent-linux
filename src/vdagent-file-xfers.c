/*  vdagent file xfers code

    Copyright 2013 Red Hat, Inc.

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
#include <inttypes.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <spice/vd_agent.h>
#include <glib.h>

#include "vdagentd-proto.h"
#include "vdagent-file-xfers.h"

struct vdagent_file_xfers {
    GHashTable *xfers;
    struct udscs_connection *vdagentd;
};

typedef struct AgentFileXferTask {
    uint32_t                       id;
    int                            file_fd;
    uint64_t                       read_bytes;
    char                           *file_name;
    uint64_t                       file_size;
} AgentFileXferTask;

static void vdagent_file_xfer_task_free(gpointer data)
{
    AgentFileXferTask *task = data;

    g_return_if_fail(task != NULL);

    if (task->file_fd > 0) {
        close(task->file_fd);
    }
    g_free(task->file_name);
    g_free(task);
}

struct vdagent_file_xfers *vdagent_file_xfers_create(
    struct udscs_connection *vdagentd)
{
    struct vdagent_file_xfers *xfers;

    xfers = g_malloc(sizeof(*xfers));
    xfers->xfers = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                         NULL, vdagent_file_xfer_task_free);
    xfers->vdagentd = vdagentd;

    return xfers;
}

void vdagent_file_xfers_destroy(struct vdagent_file_xfers *xfers)
{
    g_hash_table_destroy(xfers->xfers);
    g_free(xfers);
}

/* Parse start message then create a new file xfer task */
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
        syslog(LOG_ERR, "file-xfer: failed to load keyfile: %s",
               error->message);
        goto error;
    }
    task = g_new0(AgentFileXferTask, 1);
    task->id = msg->id;
    task->file_name = g_key_file_get_string(
        keyfile, "vdagent-file-xfer", "name", &error);
    if (error) {
        syslog(LOG_ERR, "file-xfer: failed to parse filename: %s",
               error->message);
        goto error;
    }
    task->file_size = g_key_file_get_uint64(
        keyfile, "vdagent-file-xfer", "size", &error);
    if (error) {
        syslog(LOG_ERR, "file-xfer: failed to parse filesize: %s",
               error->message);
        goto error;
    }

    g_key_file_free(keyfile);
    return task;

error:
    g_clear_error(&error);
    vdagent_file_xfer_task_free(task);
    if (keyfile) {
        g_key_file_free(keyfile);
    }
    return NULL;
}

void vdagent_file_xfers_start(struct vdagent_file_xfers *xfers,
    VDAgentFileXferStartMessage *msg)
{
    AgentFileXferTask *new;
    char *file_path = NULL;
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
    if (new->file_fd == -1) {
        syslog(LOG_ERR, "file-xfer: failed to create file %s: %s",
               file_path, strerror(errno));
        goto error;
    }

    if (ftruncate(new->file_fd, new->file_size) < 0) {
        syslog(LOG_ERR, "file-xfer: err reserving %"PRIu64" bytes for %s: %s",
               new->file_size, file_path, strerror(errno));
        goto error;
    }

    g_hash_table_insert(xfers->xfers, GINT_TO_POINTER(msg->id), new);

    udscs_write(xfers->vdagentd, VDAGENTD_FILE_XFER_STATUS,
                msg->id, VD_AGENT_FILE_XFER_STATUS_CAN_SEND_DATA, NULL, 0);
    g_free(file_path);
    return ;

error:
    udscs_write(xfers->vdagentd, VDAGENTD_FILE_XFER_STATUS,
                msg->id, VD_AGENT_FILE_XFER_STATUS_ERROR, NULL, 0);
    vdagent_file_xfer_task_free(new);
    g_free(file_path);
}

void vdagent_file_xfers_status(struct vdagent_file_xfers *xfers,
    VDAgentFileXferStatusMessage *msg)
{
    syslog(LOG_INFO, "file-xfer: task %d received response %d",
           msg->id, msg->result);

    if (msg->result == VD_AGENT_FILE_XFER_STATUS_CAN_SEND_DATA) {
        /* Do nothing */
    } else {
        /* Error, remove this task */
        gboolean found;
        found = g_hash_table_remove(xfers->xfers, GINT_TO_POINTER(msg->id));
        if (found) {
            syslog(LOG_DEBUG, "file-xfer: remove task %d", msg->id);
        } else {
            syslog(LOG_ERR, "file-xfer: can not find task %d", msg->id);
        }
    }
}

void vdagent_file_xfers_data(struct vdagent_file_xfers *xfers,
    VDAgentFileXferDataMessage *msg)
{
    AgentFileXferTask *task;
    int len;

    task = g_hash_table_lookup(xfers->xfers, GINT_TO_POINTER(msg->id));
    if (task == NULL) {
        syslog(LOG_INFO, "file-xfer: can not find task %d", msg->id);
        return ;
    }

    len = write(task->file_fd, msg->data, msg->size);
    if (len == -1) {
        syslog(LOG_ERR, "file-xfer: error writing %s: %s", task->file_name,
               strerror(errno));
        /* TODO: close, cancel dnd */
        return ;
    }

    task->read_bytes += msg->size;
    if (task->read_bytes >= task->file_size) {
        gboolean found;
        if (task->read_bytes > task->file_size) {
            syslog(LOG_ERR, "file-xfer: error received too much data");
        }
        syslog(LOG_DEBUG, "file-xfer: task %d has completed", task->id);
        found = g_hash_table_remove(xfers->xfers, GINT_TO_POINTER(msg->id));
        if (found) {
            syslog(LOG_DEBUG, "file-xfer: remove task %d", msg->id);
        } else {
            syslog(LOG_ERR, "file-xfer: can not find task %d", msg->id);
        }
    }
}

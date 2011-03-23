/*  vdagent-virtio-port.c virtio port communication code

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

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
#include "vdagent-virtio-port.h"

struct vdagent_virtio_port_buf {
    uint8_t *buf;
    size_t pos;
    size_t size;
    
    struct vdagent_virtio_port_buf *next;
};

struct vdagent_virtio_port {
    int fd;
    FILE *errfile;

    /* Read stuff, single buffer, separate header and data buffer */
    int chunk_header_read;
    int chunk_data_pos;
    int message_header_read;
    int message_data_pos;
    VDIChunkHeader chunk_header;
    VDAgentMessage message_header;
    uint8_t chunk_data[VD_AGENT_MAX_DATA_SIZE];
    uint8_t *message_data;

    /* Writes are stored in a linked list of buffers, with both the header
       + data for a single message in 1 buffer. */
    struct vdagent_virtio_port_buf *write_buf;

    /* Callbacks */
    vdagent_virtio_port_read_callback read_callback;
    vdagent_virtio_port_disconnect_callback disconnect_callback;
};

static void vdagent_virtio_port_do_write(struct vdagent_virtio_port **vportp);
static void vdagent_virtio_port_do_read(struct vdagent_virtio_port **vportp);

struct vdagent_virtio_port *vdagent_virtio_port_create(const char *portname,
    vdagent_virtio_port_read_callback read_callback,
    vdagent_virtio_port_disconnect_callback disconnect_callback,
    FILE *errfile)
{
    struct vdagent_virtio_port *vport;

    vport = calloc(1, sizeof(*vport));
    if (!vport)
        return 0;

    vport->errfile = errfile;
    vport->fd = open(portname, O_RDWR);
    if (vport->fd == -1) {
        fprintf(vport->errfile, "open %s: %s\n", portname, strerror(errno));
        free(vport);
        return NULL;
    }    

    vport->read_callback = read_callback;
    vport->disconnect_callback = disconnect_callback;

    return vport;
}

void vdagent_virtio_port_destroy(struct vdagent_virtio_port **vportp)
{
    struct vdagent_virtio_port_buf *wbuf, *next_wbuf;
    struct vdagent_virtio_port *vport = *vportp;

    if (!vport)
        return;

    if (vport->disconnect_callback)
        vport->disconnect_callback(vport);

    wbuf = vport->write_buf;
    while (wbuf) {
        next_wbuf = wbuf->next;
        free(wbuf->buf);
        free(wbuf);
        wbuf = next_wbuf;
    }

    free(vport->message_data);

    close(vport->fd);
    free(vport);
    *vportp = NULL;
}

int vdagent_virtio_port_fill_fds(struct vdagent_virtio_port *vport,
        fd_set *readfds, fd_set *writefds)
{
    if (!vport)
        return -1;

    FD_SET(vport->fd, readfds);
    if (vport->write_buf)
        FD_SET(vport->fd, writefds);

    return vport->fd + 1;
}

void vdagent_virtio_port_handle_fds(struct vdagent_virtio_port **vportp,
        fd_set *readfds, fd_set *writefds)
{
    if (!*vportp)
        return;

    if (FD_ISSET((*vportp)->fd, readfds))
        vdagent_virtio_port_do_read(vportp);

    if (*vportp && FD_ISSET((*vportp)->fd, writefds))
        vdagent_virtio_port_do_write(vportp);
}

int vdagent_virtio_port_write(
        struct vdagent_virtio_port *vport,
        uint32_t port_nr,
        uint32_t message_type,
        uint32_t message_opaque,
        const uint8_t *data,
        uint32_t data_size)
{
    struct vdagent_virtio_port_buf *wbuf, *new_wbuf;
    VDIChunkHeader chunk_header;
    VDAgentMessage message_header;

    new_wbuf = malloc(sizeof(*new_wbuf));
    if (!new_wbuf)
        return -1;

    new_wbuf->pos = 0;
    new_wbuf->size = sizeof(chunk_header) + sizeof(message_header) + data_size;
    new_wbuf->next = NULL;
    new_wbuf->buf = malloc(new_wbuf->size);
    if (!new_wbuf->buf) {
        free(new_wbuf);
        return -1;
    }

    chunk_header.port = port_nr;
    chunk_header.size = sizeof(message_header) + data_size;
    message_header.protocol = VD_AGENT_PROTOCOL;
    message_header.type = message_type;
    message_header.opaque = message_opaque;
    message_header.size = data_size;

    memcpy(new_wbuf->buf, &chunk_header, sizeof(chunk_header));
    memcpy(new_wbuf->buf + sizeof(chunk_header), &message_header,
           sizeof(message_header));
    memcpy(new_wbuf->buf + sizeof(chunk_header) + sizeof(message_header),
           data, data_size);

    if (!vport->write_buf) {
        vport->write_buf = new_wbuf;
        return 0;
    }

    /* maybe we should limit the write_buf stack depth ? */
    wbuf = vport->write_buf;
    while (wbuf->next)
        wbuf = wbuf->next;

    wbuf->next = new_wbuf;

    return 0;
}

void vdagent_virtio_port_flush(struct vdagent_virtio_port **vportp)
{
    while (*vportp && (*vportp)->write_buf)
        vdagent_virtio_port_do_write(vportp);
}

static void vdagent_virtio_port_do_chunk(struct vdagent_virtio_port **vportp)
{
    int avail, read, pos = 0;
    struct vdagent_virtio_port *vport = *vportp;

    if (vport->message_header_read < sizeof(vport->message_header)) {
        read = sizeof(vport->message_header) - vport->message_header_read;
        if (read > vport->chunk_header.size) {
            read = vport->chunk_header.size;
        }
        memcpy((uint8_t *)&vport->message_header + vport->message_header_read,
               vport->chunk_data, read);
        vport->message_header_read += read;
        if (vport->message_header_read == sizeof(vport->message_header) &&
                vport->message_header.size) {
            vport->message_data = malloc(vport->message_header.size);
            if (!vport->message_data) {
                fprintf(vport->errfile, "out of memory, disconnecting virtio\n");
                vdagent_virtio_port_destroy(vportp);
                return;
            }
        }
        pos = read;
    }

    if (vport->message_header_read == sizeof(vport->message_header)) {
        read  = vport->message_header.size - vport->message_data_pos;
        avail = vport->chunk_header.size - pos;

        if (avail > read) {
            fprintf(vport->errfile, "chunk larger then message, lost sync?\n");
            vdagent_virtio_port_destroy(vportp);
            return;
        }

        if (avail < read)
            read = avail;

        if (read) {
            memcpy(vport->message_data + vport->message_data_pos,
                   vport->chunk_data + pos, read);
            vport->message_data_pos += read;
        }

        if (vport->message_data_pos == vport->message_header.size) {
            if (vport->read_callback) {
                int r = vport->read_callback(vport, vport->chunk_header.port,
                                 &vport->message_header, vport->message_data);
                if (r == -1) {
                    vdagent_virtio_port_destroy(vportp);
                    return;
                }
            }
            vport->message_header_read = 0;
            vport->message_data_pos = 0;
            free(vport->message_data);
            vport->message_data = NULL;
        }
    }
}

static void vdagent_virtio_port_do_read(struct vdagent_virtio_port **vportp)
{
    ssize_t n;
    size_t to_read;
    uint8_t *dest;
    struct vdagent_virtio_port *vport = *vportp;

    if (vport->chunk_header_read < sizeof(vport->chunk_header)) {
        to_read = sizeof(vport->chunk_header) - vport->chunk_header_read;
        dest = (uint8_t *)&vport->chunk_header + vport->chunk_header_read;
    } else {
        to_read = vport->chunk_header.size - vport->chunk_data_pos;
        dest = vport->chunk_data + vport->chunk_data_pos;
    }

    n = read(vport->fd, dest, to_read);
    if (n < 0) {
        if (errno == EINTR)
            return;
        fprintf(vport->errfile, "reading from vdagent virtio port: %s\n",
                strerror(errno));
    }
    if (n <= 0) {
        vdagent_virtio_port_destroy(vportp);
        return;
    }

    if (vport->chunk_header_read < sizeof(vport->chunk_header)) {
        vport->chunk_header_read += n;
        if (vport->chunk_header_read == sizeof(vport->chunk_header)) {
            if (vport->chunk_header.size > VD_AGENT_MAX_DATA_SIZE) {
                fprintf(vport->errfile, "chunk size too large\n");
                vdagent_virtio_port_destroy(vportp);
                return;
            }
        }
    } else {
        vport->chunk_data_pos += n;
        if (vport->chunk_data_pos == vport->chunk_header.size) {
            vdagent_virtio_port_do_chunk(vportp);
            vport->chunk_header_read = 0;
            vport->chunk_data_pos = 0;
        }
    }
}

static void vdagent_virtio_port_do_write(struct vdagent_virtio_port **vportp)
{
    ssize_t n;
    size_t to_write;
    struct vdagent_virtio_port *vport = *vportp;

    struct vdagent_virtio_port_buf* wbuf = vport->write_buf;
    if (!wbuf) {
        fprintf(vport->errfile,
                "do_write called on a port without a write buf ?!\n");
        return;
    }

    to_write = wbuf->size - wbuf->pos;
    n = write(vport->fd, wbuf->buf + wbuf->pos, to_write);
    if (n < 0) {
        if (errno == EINTR)
            return;
        fprintf(vport->errfile, "writing to vdagent virtio port: %s\n",
                strerror(errno));
        vdagent_virtio_port_destroy(vportp);
        return;
    }

    wbuf->pos += n;
    if (wbuf->pos == wbuf->size) {
        vport->write_buf = wbuf->next;
        free(wbuf->buf);
        free(wbuf);
    }
}

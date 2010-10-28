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
#include <stdio.h>
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

static void vdagent_virtio_port_do_write(struct vdagent_virtio_port **portp);
static void vdagent_virtio_port_do_read(struct vdagent_virtio_port **portp);

struct vdagent_virtio_port *vdagent_virtio_port_create(const char *portname,
    vdagent_virtio_port_read_callback read_callback,
    vdagent_virtio_port_disconnect_callback disconnect_callback)
{
    struct vdagent_virtio_port *port;

    port = calloc(1, sizeof(*port));
    if (!port)
        return 0;

    port->fd = open(portname, O_RDWR);
    if (port->fd == -1) {
        fprintf(stderr, "open %s: %s\n", portname, strerror(errno));
        free(port);
        return NULL;
    }    

    port->read_callback = read_callback;
    port->disconnect_callback = disconnect_callback;

    return port;
}

void vdagent_virtio_port_destroy(struct vdagent_virtio_port **portp)
{
    struct vdagent_virtio_port_buf *wbuf, *next_wbuf;
    struct vdagent_virtio_port *port = *portp;

    if (!port)
        return;

    if (port->disconnect_callback)
        port->disconnect_callback(port);

    wbuf = port->write_buf;
    while (wbuf) {
        next_wbuf = wbuf->next;
        free(wbuf->buf);
        free(wbuf);
        wbuf = next_wbuf;
    }

    free(port->message_data);

    close(port->fd);
    free(port);
    *portp = NULL;
}

int vdagent_virtio_port_fill_fds(struct vdagent_virtio_port *port,
        fd_set *readfds, fd_set *writefds)
{
    if (!port)
        return -1;

    FD_SET(port->fd, readfds);
    if (port->write_buf)
        FD_SET(port->fd, writefds);

    return port->fd + 1;
}

void vdagent_virtio_port_handle_fds(struct vdagent_virtio_port **portp,
        fd_set *readfds, fd_set *writefds)
{
    if (!*portp)
        return;

    if (FD_ISSET((*portp)->fd, readfds))
        vdagent_virtio_port_do_read(portp);

    if (*portp && FD_ISSET((*portp)->fd, writefds))
        vdagent_virtio_port_do_write(portp);
}

int vdagent_virtio_port_write(
        struct vdagent_virtio_port *port,
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

    if (!port->write_buf) {
        port->write_buf = new_wbuf;
        return 0;
    }

    /* maybe we should limit the write_buf stack depth ? */
    wbuf = port->write_buf;
    while (wbuf->next)
        wbuf = wbuf->next;

    wbuf->next = new_wbuf;

    return 0;
}

void vdagent_virtio_port_flush(struct vdagent_virtio_port **portp)
{
    while (*portp && (*portp)->write_buf)
        vdagent_virtio_port_do_write(portp);
}

static void vdagent_virtio_port_do_chunk(struct vdagent_virtio_port **portp)
{
    int avail, read, pos = 0;
    struct vdagent_virtio_port *port = *portp;

    if (port->message_header_read < sizeof(port->message_header)) {
        read = sizeof(port->message_header) - port->message_header_read;
        memcpy((uint8_t *)&port->message_header + port->message_header_read,
               port->chunk_data, read);
        port->message_header_read += read;
        if (port->message_header_read == sizeof(port->message_header) &&
                port->message_header.size) {
            port->message_data = malloc(port->message_header.size);
            if (!port->message_data) {
                fprintf(stderr, "out of memory, disconnecting virtio\n");
                vdagent_virtio_port_destroy(portp);
                return;
            }
        }
        pos = read;
    }

    if (port->message_header_read == sizeof(port->message_header)) {
        read  = port->message_header.size - port->message_data_pos;
        avail = port->chunk_header.size - pos;

        if (avail > read) {
            fprintf(stderr, "chunk larger then message, lost sync?\n");
            vdagent_virtio_port_destroy(portp);
            return;
        }

        if (avail < read)
            read = avail;

        if (read) {
            memcpy(port->message_data + port->message_data_pos,
                   port->chunk_data + pos, read);
            port->message_data_pos += read;
        }

        if (port->message_data_pos == port->message_header.size) {
            if (port->read_callback) {
                int r = port->read_callback(port, &port->chunk_header,
                                    &port->message_header, port->message_data);
                if (r == -1) {
                    vdagent_virtio_port_destroy(portp);
                    return;
                }
            }
            port->message_header_read = 0;
            port->message_data_pos = 0;
            free(port->message_data);
            port->message_data = NULL;
        }
    }
}

static void vdagent_virtio_port_do_read(struct vdagent_virtio_port **portp)
{
    ssize_t n;
    size_t to_read;
    uint8_t *dest;
    struct vdagent_virtio_port *port = *portp;

    if (port->chunk_header_read < sizeof(port->chunk_header)) {
        to_read = sizeof(port->chunk_header) - port->chunk_header_read;
        dest = (uint8_t *)&port->chunk_header + port->chunk_header_read;
    } else {
        to_read = port->chunk_header.size - port->chunk_data_pos;
        dest = port->chunk_data + port->chunk_data_pos;
    }

    n = read(port->fd, dest, to_read);
    if (n < 0) {
        if (errno == EINTR)
            return;
        perror("reading from vdagent virtio port");
    }
    if (n <= 0) {
        vdagent_virtio_port_destroy(portp);
        return;
    }

    if (port->chunk_header_read < sizeof(port->chunk_header)) {
        port->chunk_header_read += n;
        if (port->chunk_header_read == sizeof(port->chunk_header)) {
            if (port->chunk_header.size > VD_AGENT_MAX_DATA_SIZE) {
                fprintf(stderr, "chunk size too large\n");
                vdagent_virtio_port_destroy(portp);
                return;
            }
        }
    } else {
        port->chunk_data_pos += n;
        if (port->chunk_data_pos == port->chunk_header.size) {
            vdagent_virtio_port_do_chunk(portp);
            port->chunk_header_read = 0;
            port->chunk_data_pos = 0;
        }
    }
}

static void vdagent_virtio_port_do_write(struct vdagent_virtio_port **portp)
{
    ssize_t n;
    size_t to_write;
    struct vdagent_virtio_port *port = *portp;

    struct vdagent_virtio_port_buf* wbuf = port->write_buf;
    if (!wbuf) {
        fprintf(stderr,
                "do_write called on a port without a write buf ?!\n");
        return;
    }

    to_write = wbuf->size - wbuf->pos;
    n = write(port->fd, wbuf->buf + wbuf->pos, to_write);
    if (n < 0) {
        if (errno == EINTR)
            return;
        perror("writing to vdagent virtio port");
        vdagent_virtio_port_destroy(portp);
        return;
    }

    wbuf->pos += n;
    if (wbuf->pos == wbuf->size) {
        port->write_buf = wbuf->next;
        free(wbuf->buf);
        free(wbuf);
    }
}

/**
 * Copyright Flexible Software Solutions S.L. 2014
 **/

#ifndef __PORT_FORWARD_H
#define __PORT_FORWARD_H

#include <sys/select.h>
#include <spice/vd_agent.h>

typedef struct port_forwarder port_forwarder;

/*
 * Callback to send commands to the SPICE client.
 * Returns 0 on success, -1 on error (client disconnected)
 */
typedef int (*vdagent_port_forwarder_send_command_callback)(
    uint32_t command, const uint8_t *data, uint32_t data_size);

/*
 * Create a port forwarder, with a callback and a debug flag
 */
port_forwarder *vdagent_port_forwarder_create(vdagent_port_forwarder_send_command_callback cb,
                                              int debug);

/*
 * Destroy the port forwarder.
 */
void vdagent_port_forwarder_destroy(port_forwarder *pf);

/*
 * Given a port_forwarder, fill the fd_sets pointed to by readfds and
 * writefds for select() usage.
 *
 * Return value: value of the highest fd + 1
 */
int vdagent_port_forwarder_fill_fds(port_forwarder *pf,
                                    fd_set *readfds, fd_set *writefds);

/* Handle any events flagged by select for the given port_forwarder. */
void vdagent_port_forwarder_handle_fds(port_forwarder *pf,
                                       fd_set *readfds, fd_set *writefds);

/*
 * Handle a message comming from the SPICE client through the virtio port.
 */
void do_port_forward_command(port_forwarder *pf, uint32_t command, uint8_t *data);

/*
 * Signal the port forwarder that the client has disconnected.
 * All connections and open ports are closed.
 */
void vdagent_port_forwarder_client_disconnected(port_forwarder *pf);

#endif /* __PORT_FORWARD_H */

/**
 * Copyright Flexible Software Solutions S.L. 2014
 **/

#ifndef __PORT_FORWARD_H
#define __PORT_FORWARD_H

#include <sys/select.h>
#include <spice/vd_agent.h>

typedef struct port_forwarder port_forwarder;
typedef void (*vdagent_port_forwarder_send_command_callback)(
    uint32_t command, const uint8_t *data, uint32_t data_size);

port_forwarder *vdagent_port_forwarder_create(vdagent_port_forwarder_send_command_callback cb);

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
 * PRE: message_header.type == VD_AGENT_PORT_FORWARD_COMMAND
 */
void do_port_forward_command(port_forwarder *pf,
                             VDAgentMessage *message_header, uint8_t *data);

#endif /* __PORT_FORWARD_H */

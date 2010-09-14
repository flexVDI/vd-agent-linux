/*  vdagentd-proto.h header file for the protocol over the unix domain socket
    between the vdagent process / xorg-client and the vdagentd (daemon).

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

#ifndef __VDAGENTD_PROTO_H
#define __VDAGENTD_PROTO_H

#define VDAGENTD_SOCKET "/tmp/vdagent"

enum {
    VDAGENTD_GUEST_XORG_RESOLUTION,
};

struct vdagentd_guest_xorg_resolution {
    int width;
    int height;
};

#endif

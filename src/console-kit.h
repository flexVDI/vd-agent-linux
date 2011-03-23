/*  console-kit.h vdagentd ConsoleKit integration code - header

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

#ifndef __CONSOLE_KIT_H
#define __CONSOLE_KIT_H

#include <stdio.h>
#include <stdint.h>

struct console_kit;

struct console_kit *console_kit_create(FILE *errfile);
void console_kit_destroy(struct console_kit *ck);

int console_kit_get_fd(struct console_kit *ck);

const char *console_kit_get_active_session(struct console_kit *ck);
/* Note result must be free()-ed by caller */
char *console_kit_session_for_pid(struct console_kit *ck, uint32_t pid);

#endif

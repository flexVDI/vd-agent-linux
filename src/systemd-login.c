/*  systemd-login.c vdagentd libsystemd-login integration code

    Copyright 2012 Red Hat, Inc.

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

#include "session-info.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-login.h>

struct session_info {
    FILE *logfile;
    int verbose;
    sd_login_monitor *mon;
    char *session;
};

struct session_info *session_info_create(FILE *logfile, int verbose)
{
    struct session_info *si;
    int r;

    si = calloc(1, sizeof(*si));
    if (!si)
        return NULL;

    si->logfile = logfile;
    si->verbose = verbose;

    r = sd_login_monitor_new("session", &si->mon);
    if (r < 0) {
        fprintf(logfile, "Error creating login monitor: %s\n", strerror(-r));
        free(si);
        return NULL;
    }

    return si;
}

void session_info_destroy(struct session_info *si)
{
    sd_login_monitor_unref(si->mon);
    free(si->session);
    free(si);
}

int session_info_get_fd(struct session_info *si)
{
    return sd_login_monitor_get_fd(si->mon);
}

const char *session_info_get_active_session(struct session_info *si)
{
    int r;
    char *old_session = si->session;

    si->session = NULL;
    r = sd_seat_get_active("seat0", &si->session, NULL);
    /* ENOENT happens when a seat is switching from one session to another */
    if (r < 0 && r != ENOENT)
        fprintf(si->logfile, "Error getting active session: %s\n",
                strerror(-r));

    if (si->verbose && si->session &&
            (!old_session || strcmp(old_session, si->session)))
        fprintf(si->logfile, "Active session: %s\n", si->session);

    sd_login_monitor_flush(si->mon);
    free(old_session);

    return si->session;
}

char *session_info_session_for_pid(struct session_info *si, uint32_t pid)
{
    int r;
    char *session = NULL;

    r = sd_pid_get_session(pid, &session);
    if (r < 0)
        fprintf(si->logfile, "Error getting session for pid %u: %s\n",
                pid, strerror(-r));
    else if (si->verbose)
        fprintf(si->logfile, "Session for pid %u: %s\n", pid, session);

    return session;
}

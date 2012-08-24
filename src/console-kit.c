/*  console-kit.c vdagentd ConsoleKit integration code

    Copyright 2010-2012 Red Hat, Inc.

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
#include <dbus/dbus.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

struct session_info {
    DBusConnection *connection;
    int fd;
    char *seat;
    char *active_session;
};

static char *console_kit_get_first_seat(struct session_info *ck);
static char *console_kit_check_active_session_change(struct session_info *ck);

struct session_info *session_info_create(int verbose)
{
    struct session_info *ck;
    DBusError error;
    char match[1024];

    ck = calloc(1, sizeof(*ck));
    if (!ck)
        return NULL;

    dbus_error_init(&error);
    ck->connection = dbus_bus_get_private(DBUS_BUS_SYSTEM, &error);
    if (ck->connection == NULL || dbus_error_is_set(&error)) {
        if (dbus_error_is_set(&error)) {
             syslog(LOG_ERR, "Unable to connect to system bus: %s",
                    error.message);
             dbus_error_free(&error);
        } else
             syslog(LOG_ERR, "Unable to connect to system bus");
        free(ck);
        return NULL;
    }
    
    if (!dbus_connection_get_unix_fd(ck->connection, &ck->fd)) {
        syslog(LOG_ERR, "Unable to get connection fd");
        session_info_destroy(ck);
        return NULL;
    }

    if (!console_kit_get_first_seat(ck)) {
        session_info_destroy(ck);
        return NULL;
    }

    /* Register for active session changes */
    snprintf(match, sizeof(match),
             "type='signal',interface='org.freedesktop.ConsoleKit.Seat',"
             "path='%s',member='ActiveSessionChanged'", ck->seat);
    dbus_error_init(&error);
    dbus_bus_add_match(ck->connection, match, &error);
    if (dbus_error_is_set(&error)) { 
        syslog(LOG_ERR, "Match Error (%s)", error.message);
        session_info_destroy(ck);
        return NULL;
    }

    return ck;
}

void session_info_destroy(struct session_info *ck)
{
    if (!ck)
        return;

    dbus_connection_close(ck->connection);
    free(ck->seat);
    free(ck->active_session);
    free(ck);
}

int session_info_get_fd(struct session_info *ck)
{
    return ck->fd;
}

static char *console_kit_get_first_seat(struct session_info *ck)
{
    DBusError error;
    DBusMessage *message = NULL;
    DBusMessage *reply = NULL;
    DBusMessageIter iter, subiter;
    int type;
    char *seat = NULL;

    message = dbus_message_new_method_call("org.freedesktop.ConsoleKit",
                                           "/org/freedesktop/ConsoleKit/Manager",
                                           "org.freedesktop.ConsoleKit.Manager",
                                           "GetSeats");
    if (message == NULL) {
        syslog(LOG_ERR, "Unable to create dbus message");
        goto exit;
    }

    dbus_error_init(&error);
    reply = dbus_connection_send_with_reply_and_block(ck->connection,
                                                      message,
                                                      -1,
                                                      &error);
    if (reply == NULL || dbus_error_is_set(&error)) {
        if (dbus_error_is_set(&error)) {
            syslog(LOG_ERR, "GetSeats failed: %s", error.message);
            dbus_error_free(&error);
        } else
            syslog(LOG_ERR, "GetSeats failed");
        goto exit;
    }

    dbus_message_iter_init(reply, &iter);
    type = dbus_message_iter_get_arg_type(&iter);
    if (type != DBUS_TYPE_ARRAY) {
        syslog(LOG_ERR,
               "expected an array return value, got a '%c' instead", type);
        goto exit;
    }

    dbus_message_iter_recurse(&iter, &subiter);
    type = dbus_message_iter_get_arg_type(&subiter);
    if (type != DBUS_TYPE_OBJECT_PATH) {
        syslog(LOG_ERR,
               "expected an object path element, got a '%c' instead", type);
        goto exit;
    }

    dbus_message_iter_get_basic(&subiter, &seat);
    ck->seat = strdup(seat);

exit:
    if (reply != NULL) {
            dbus_message_unref(reply);
    }

    if (message != NULL) {
            dbus_message_unref(message);
    }

    return ck->seat;
}

const char *session_info_get_active_session(struct session_info *ck)
{
    DBusError error;
    DBusMessage *message = NULL;
    DBusMessage *reply = NULL;
    char *session = NULL;

    if (!ck)
        return NULL;

    if (ck->active_session)
        return console_kit_check_active_session_change(ck);

    message = dbus_message_new_method_call("org.freedesktop.ConsoleKit",
                                           ck->seat,
                                           "org.freedesktop.ConsoleKit.Seat",
                                           "GetActiveSession");
    if (message == NULL) {
        syslog(LOG_ERR, "Unable to create dbus message");
        goto exit;
    }

    dbus_error_init(&error);
    reply = dbus_connection_send_with_reply_and_block(ck->connection,
                                                      message,
                                                      -1,
                                                      &error);
    if (reply == NULL || dbus_error_is_set(&error)) {
        if (dbus_error_is_set(&error)) {
            syslog(LOG_ERR, "GetActiveSession failed: %s", error.message);
            dbus_error_free(&error);
        } else
            syslog(LOG_ERR, "GetActiveSession failed");
        goto exit;
    }

    dbus_error_init(&error);
    if (!dbus_message_get_args(reply,
                               &error,
                               DBUS_TYPE_OBJECT_PATH, &session,
                               DBUS_TYPE_INVALID)) {
        if (dbus_error_is_set(&error)) {
            syslog(LOG_ERR, "error get ssid from reply: %s", error.message);
            dbus_error_free(&error);
        } else
            syslog(LOG_ERR, "error getting ssid from reply");
        session = NULL;
        goto exit;
    }

    ck->active_session = strdup(session);

exit:
    if (reply != NULL) {
            dbus_message_unref(reply);
    }

    if (message != NULL) {
            dbus_message_unref(message);
    }

    /* In case the session was changed while we were running */
    return console_kit_check_active_session_change(ck);
}

char *session_info_session_for_pid(struct session_info *ck, uint32_t pid)
{
    DBusError error;
    DBusMessage *message = NULL;
    DBusMessage *reply = NULL;
    DBusMessageIter args;
    char *ssid = NULL;

    if (!ck)
        return NULL;

    message = dbus_message_new_method_call("org.freedesktop.ConsoleKit",
                                           "/org/freedesktop/ConsoleKit/Manager",
                                           "org.freedesktop.ConsoleKit.Manager",
                                           "GetSessionForUnixProcess");
    if (message == NULL) {
        syslog(LOG_ERR, "Unable to create dbus message");
        goto exit;
    }

    dbus_message_iter_init_append(message, &args);
    if (!dbus_message_iter_append_basic(&args, DBUS_TYPE_UINT32, &pid)) {
        syslog(LOG_ERR, "Unable to append dbus message args");
        goto exit;
    }

    dbus_error_init(&error);
    reply = dbus_connection_send_with_reply_and_block(ck->connection,
                                                      message,
                                                      -1,
                                                      &error);
    if (reply == NULL || dbus_error_is_set(&error)) {
        if (dbus_error_is_set(&error)) {
            syslog(LOG_ERR, "GetSessionForUnixProcess failed: %s",
                   error.message);
            dbus_error_free(&error);
        } else
            syslog(LOG_ERR, "GetSessionForUnixProces failed");
        goto exit;
    }

    dbus_error_init(&error);
    if (!dbus_message_get_args(reply,
                               &error,
                               DBUS_TYPE_OBJECT_PATH, &ssid,
                               DBUS_TYPE_INVALID)) {
        if (dbus_error_is_set(&error)) {
            syslog(LOG_ERR, "error get ssid from reply: %s", error.message);
            dbus_error_free(&error);
        } else
            syslog(LOG_ERR, "error getting ssid from reply");
        ssid = NULL;
        goto exit;
    }

    ssid = strdup(ssid);

exit:
    if (reply != NULL) {
            dbus_message_unref(reply);
    }

    if (message != NULL) {
            dbus_message_unref(message);
    }

    return ssid;
}

static char *console_kit_check_active_session_change(struct session_info *ck)
{
    DBusMessage *message = NULL;
    DBusMessageIter iter;
    char *session;
    int type;

    /* non blocking read of the next available message */
    dbus_connection_read_write(ck->connection, 0);
    while ((message = dbus_connection_pop_message(ck->connection))) {
        if (dbus_message_get_type(message) == DBUS_MESSAGE_TYPE_SIGNAL) {
            const char *member = dbus_message_get_member (message);
            if (!strcmp(member, "NameAcquired")) {
                dbus_message_unref(message);
                continue;
            }
            if (strcmp(member, "ActiveSessionChanged")) {
                syslog(LOG_ERR, "unexpected signal member: %s", member);
                dbus_message_unref(message);
                continue;
            }
        } else {
            syslog(LOG_ERR, "received non signal message!");
            dbus_message_unref(message);
            continue;
        }

        free(ck->active_session);
        ck->active_session = NULL;

        dbus_message_iter_init(message, &iter);
        type = dbus_message_iter_get_arg_type(&iter);
        /* Session should be an object path, but there is a bug in
           ConsoleKit where it sends a string rather then an object_path
           accept object_path too in case the bug ever gets fixed */
        if (type != DBUS_TYPE_STRING && type != DBUS_TYPE_OBJECT_PATH) {
            syslog(LOG_ERR,
                   "ActiveSessionChanged message has unexpected type: '%c'",
                   type);
            dbus_message_unref(message);
            continue;
        }

        dbus_message_iter_get_basic(&iter, &session);
        ck->active_session = strdup(session);
        dbus_message_unref(message);

        /* non blocking read of the next available message */
        dbus_connection_read_write(ck->connection, 0);
    }

    return ck->active_session;
}

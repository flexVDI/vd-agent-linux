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

#define INTERFACE_CONSOLE_KIT "org.freedesktop.ConsoleKit"
#define OBJ_PATH_CONSOLE_KIT  "/org/freedesktop/ConsoleKit"

#define INTERFACE_CONSOLE_KIT_MANAGER    INTERFACE_CONSOLE_KIT ".Manager"
#define OBJ_PATH_CONSOLE_KIT_MANAGER     OBJ_PATH_CONSOLE_KIT "/Manager"

#define INTERFACE_CONSOLE_KIT_SEAT       INTERFACE_CONSOLE_KIT ".Seat"

static char *console_kit_get_first_seat(struct session_info *info);
static char *console_kit_check_active_session_change(struct session_info *info);

struct session_info *session_info_create(int verbose)
{
    struct session_info *info;
    DBusError error;
    char match[1024];

    info = calloc(1, sizeof(*info));
    if (!info)
        return NULL;

    dbus_error_init(&error);
    info->connection = dbus_bus_get_private(DBUS_BUS_SYSTEM, &error);
    if (info->connection == NULL || dbus_error_is_set(&error)) {
        if (dbus_error_is_set(&error)) {
             syslog(LOG_ERR, "Unable to connect to system bus: %s",
                    error.message);
             dbus_error_free(&error);
        } else
             syslog(LOG_ERR, "Unable to connect to system bus");
        free(info);
        return NULL;
    }

    if (!dbus_connection_get_unix_fd(info->connection, &info->fd)) {
        syslog(LOG_ERR, "Unable to get connection fd");
        session_info_destroy(info);
        return NULL;
    }

    if (!console_kit_get_first_seat(info)) {
        session_info_destroy(info);
        return NULL;
    }

    /* Register for active session changes */
    snprintf(match, sizeof(match),
             "type='signal',interface='%s',"
             "path='%s',member='ActiveSessionChanged'",
             INTERFACE_CONSOLE_KIT_SEAT, info->seat);
    dbus_error_init(&error);
    dbus_bus_add_match(info->connection, match, &error);
    if (dbus_error_is_set(&error)) {
        syslog(LOG_ERR, "Match Error (%s)", error.message);
        session_info_destroy(info);
        return NULL;
    }

    return info;
}

void session_info_destroy(struct session_info *info)
{
    if (!info)
        return;

    dbus_connection_close(info->connection);
    free(info->seat);
    free(info->active_session);
    free(info);
}

int session_info_get_fd(struct session_info *info)
{
    return info->fd;
}

static char *console_kit_get_first_seat(struct session_info *info)
{
    DBusError error;
    DBusMessage *message = NULL;
    DBusMessage *reply = NULL;
    DBusMessageIter iter, subiter;
    int type;
    char *seat = NULL;


    message = dbus_message_new_method_call(INTERFACE_CONSOLE_KIT,
                                           OBJ_PATH_CONSOLE_KIT_MANAGER,
                                           INTERFACE_CONSOLE_KIT_MANAGER,
                                           "GetSeats");
    if (message == NULL) {
        syslog(LOG_ERR, "Unable to create dbus message");
        goto exit;
    }

    dbus_error_init(&error);
    reply = dbus_connection_send_with_reply_and_block(info->connection,
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
    info->seat = strdup(seat);

exit:
    if (reply != NULL) {
            dbus_message_unref(reply);
    }

    if (message != NULL) {
            dbus_message_unref(message);
    }

    return info->seat;
}

const char *session_info_get_active_session(struct session_info *info)
{
    DBusError error;
    DBusMessage *message = NULL;
    DBusMessage *reply = NULL;
    char *session = NULL;

    if (!info)
        return NULL;

    if (info->active_session)
        return console_kit_check_active_session_change(info);

    message = dbus_message_new_method_call(INTERFACE_CONSOLE_KIT,
                                           info->seat,
                                           INTERFACE_CONSOLE_KIT_SEAT,
                                           "GetActiveSession");
    if (message == NULL) {
        syslog(LOG_ERR, "Unable to create dbus message");
        goto exit;
    }

    dbus_error_init(&error);
    reply = dbus_connection_send_with_reply_and_block(info->connection,
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

    info->active_session = strdup(session);

exit:
    if (reply != NULL) {
            dbus_message_unref(reply);
    }

    if (message != NULL) {
            dbus_message_unref(message);
    }

    /* In case the session was changed while we were running */
    return console_kit_check_active_session_change(info);
}

char *session_info_session_for_pid(struct session_info *info, uint32_t pid)
{
    DBusError error;
    DBusMessage *message = NULL;
    DBusMessage *reply = NULL;
    DBusMessageIter args;
    char *ssid = NULL;

    if (!info)
        return NULL;

    message = dbus_message_new_method_call(INTERFACE_CONSOLE_KIT,
                                           OBJ_PATH_CONSOLE_KIT_MANAGER,
                                           INTERFACE_CONSOLE_KIT_MANAGER,
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
    reply = dbus_connection_send_with_reply_and_block(info->connection,
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

static char *console_kit_check_active_session_change(struct session_info *info)
{
    DBusMessage *message = NULL;
    DBusMessageIter iter;
    char *session;
    int type;

    /* non blocking read of the next available message */
    dbus_connection_read_write(info->connection, 0);
    while ((message = dbus_connection_pop_message(info->connection))) {
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

        free(info->active_session);
        info->active_session = NULL;

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
        info->active_session = strdup(session);
        dbus_message_unref(message);

        /* non blocking read of the next available message */
        dbus_connection_read_write(info->connection, 0);
    }

    return info->active_session;
}

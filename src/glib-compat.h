/*  glib-compat.h

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
#ifndef __GLIB_COMPAT_H
#define __GLIB_COMPAT_H

#include <glib.h>

#if !GLIB_CHECK_VERSION(2,26,0)
static inline guint64 g_key_file_get_uint64(GKeyFile *file,
    const gchar *group, const gchar *key, GError **error)
{
    gchar *val_str;
    guint64 val = 0;

    val_str = g_key_file_get_value(file, group, key, error);
    if (val_str) {
        val = g_ascii_strtoull(val_str, NULL, 10);
        g_free(val_str);
    }

    return val;
}
#endif

#endif

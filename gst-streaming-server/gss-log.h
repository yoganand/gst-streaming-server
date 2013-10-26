/* GStreamer Streaming Server
 * Copyright (C) 2013 David Schleef <ds@schleef.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifndef _GSS_LOG_H_
#define _GSS_LOG_H_

#include <glib.h>
#include <gst-streaming-server/gss-transaction.h>

G_BEGIN_DECLS

typedef enum {
  GSS_ERROR_FILE_SEEK,
  GSS_ERROR_FILE_READ
} GssErrorEnum;

GQuark _gss_error_quark;

void gss_log_init (void);
void gss_log_set_verbosity (int level);
void gss_log_send_syslog (int level, const char *msg);
void gss_log_transaction (GssTransaction *t);


G_END_DECLS

#endif


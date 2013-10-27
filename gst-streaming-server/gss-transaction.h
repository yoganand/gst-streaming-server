/* GStreamer Streaming Server
 * Copyright (C) 2009-2012 Entropy Wave Inc <info@entropywave.com>
 * Copyright (C) 2009-2012 David Schleef <ds@schleef.org>
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


#ifndef _GSS_TRANSACTION_H
#define _GSS_TRANSACTION_H

#include <libsoup/soup.h>
#include "gss-config.h"
#include "gss-types.h"

G_BEGIN_DECLS

typedef void (GssTransactionCallback)(GssTransaction *transaction);

struct _GssTransaction {
  GssServer *server;
  SoupServer *soupserver;
  SoupMessage *msg;
  const char *path;
  GHashTable *query;
  SoupClientContext *client;
  GssResource *resource;
  GssSession *session;
  GString *s;
  GString *script;
  const char *debug_message;
  int id;
  guint64 start_time;
  guint64 completion_time;
  guint64 finish_time;
  gboolean async;
};

GssTransaction * gss_transaction_new (GssServer *server,
    SoupServer * soupserver, SoupMessage * msg, const char *path,
    GHashTable * query, SoupClientContext * client);
void gss_transaction_free (GssTransaction *transaction);
void gss_transaction_error_not_found (GssTransaction *t, const char *reason);
void gss_transaction_redirect (GssTransaction * t, const char *target);
void gss_transaction_error (GssTransaction * t, const char *message);
void gss_transaction_delay (GssTransaction *t, int msec);
void gss_transaction_dump (GssTransaction *t);

gchar *gss_json_gobject_to_data (GObject * gobject, gsize * length);


G_END_DECLS

#endif


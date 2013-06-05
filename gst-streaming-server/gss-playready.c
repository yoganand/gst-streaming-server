/* GStreamer Streaming Server
 * Copyright (C) 2013 Rdio Inc <ingestions@rd.io>
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

#include "config.h"

#include "gss-playready.h"
#include "gss-utils.h"


static SoupSession *session;

struct _Request
{
  SoupServer *server;
  SoupMessage *request_message;
};

static void
done (SoupSession * session, SoupMessage * msg, gpointer user_data)
{
  struct _Request *request = user_data;


  GST_ERROR ("status: %d", msg->status_code);
  GST_ERROR ("response: %s", msg->response_body->data);

  soup_message_body_append (request->request_message->response_body,
      SOUP_MEMORY_COPY, msg->response_body->data, msg->response_body->length);

  soup_message_set_status (request->request_message, msg->status_code);
  soup_server_unpause_message (request->server, request->request_message);

  g_free (request);
}

static const guint8 content_key[16] = {
  0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
  0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
};


static void
playready_post_resource (GssTransaction * t)
{
  //GssServer *server = GSS_SERVER (t->resource->priv);
  GHashTable *hash;
  //gboolean ret = FALSE;
  SoupMessage *message;
  struct _Request *request;
  char *url;
  char *base64_key;

  hash = gss_config_get_post_hash (t);
  if (hash) {
    GST_ERROR ("got hash");
    gss_utils_dump_hash (hash);
    g_hash_table_unref (hash);
  } else {
    GST_ERROR ("no hash: %s", t->msg->request_body->data);
  }

  base64_key = g_base64_encode (content_key, 16);
  GST_ERROR ("base64_key %s", base64_key);
#if 0
  url = g_strdup_printf ("http://playready.directtaps.net/pr/svc/"
      "rightsmanager.asmx?ContentKey=%s", base64_key);
#else
  url = g_strdup_printf ("http://playready.directtaps.net/pr/svc/"
      "rightsmanager.asmx");
#endif
  message = soup_message_new (SOUP_METHOD_POST, url);
  g_free (url);
  g_free (base64_key);
  soup_message_body_append (message->request_body, SOUP_MEMORY_COPY,
      t->msg->request_body->data, t->msg->request_body->length);
  soup_message_headers_replace (message->request_headers,
      "Content-Type", soup_message_headers_get_one (t->msg->request_headers,
          "Content-Type"));

  if (session == NULL) {
    session = soup_session_async_new_with_options ("timeout", 10, NULL);
  }
  request = g_malloc (sizeof (*request));
  request->server = t->soupserver;
  request->request_message = t->msg;
  soup_session_queue_message (session, message, done, request);

  soup_server_pause_message (t->soupserver, t->msg);
}

static void
playready_get_resource (GssTransaction * t)
{
}

void
gss_playready_setup (GssServer * server)
{
  gss_server_add_file_resource (server, "/crossdomain.xml", 0, "text/xml");
  gss_server_add_file_resource (server, "/clientaccesspolicy.xml", 0,
      "text/xml");
  gss_server_add_file_resource (server, "/request.cms", 0,
      "application/vnd.ms-sstr+xml");

  gss_server_add_resource (server, "/rightsmanager.asmx",
      0, "text/xml", playready_get_resource, NULL,
      playready_post_resource, server);
}

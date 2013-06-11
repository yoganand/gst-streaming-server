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

  //base64_key = g_base64_encode (content_key, 16);
  base64_key = g_strdup ("eNqVnXrElmo2NSsn7IXeEA==");
  //base64_key = g_strdup ("FRDBoj7X!LuEDVjIvK0abQ281xY=");
  GST_ERROR ("base64_key %s", base64_key);
#if 0
  url = g_strdup_printf ("http://playready.directtaps.net/pr/svc/"
      //"rightsmanager.asmx?ContentKey=%s", base64_key);
      "rightsmanager.asmx?KeySeed=XVBovsmzhP9gRIZxWfFta3VVRPzVEWmJsazEJ46I");
#else
  url = g_strdup_printf ("http://playready.directtaps.net/pr/svc/"
      "rightsmanager.asmx"
      "?UncompressedDigitalVideoOPL=300"
      "&CompressedDigitalVideoOPL=300"
      "&UncompressedDigitalAudioOPL=300"
      "&CompressedDigitalAudioOPL=300" "&AnalogVideoOPL=300");
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


guint8 *
gss_playready_get_default_key_seed (void)
{
  guint8 default_seed[30] = { 0x5D, 0x50, 0x68, 0xBE, 0xC9, 0xB3, 0x84,
    0xFF, 0x60, 0x44, 0x86, 0x71, 0x59, 0xF1, 0x6D, 0x6B, 0x75,
    0x55, 0x44, 0xFC, 0xD5, 0x11, 0x69, 0x89, 0xB1, 0xAC, 0xC4,
    0x27, 0x8E, 0x88
  };

  return g_memdup (default_seed, 30);
}

guint8 *
gss_playready_generate_key (guint8 * key_seed, int key_seed_len,
    guint8 * kid, int kid_len)
{
  GChecksum *checksum;
  guint8 *hash_a;
  guint8 *hash_b;
  guint8 *hash_c;
  guint8 *key;
  gsize size;
  int i;

  checksum = g_checksum_new (G_CHECKSUM_SHA256);
  size = g_checksum_type_get_length (G_CHECKSUM_SHA256);
  hash_a = g_malloc (size);
  hash_b = g_malloc (size);
  hash_c = g_malloc (size);

  //
  // Create sha_A_Output buffer. It is the SHA of the truncatedKeySeed and
  // the keyIdAsBytes
  //
  g_checksum_update (checksum, key_seed, 30);
  g_checksum_update (checksum, kid, kid_len);
  g_checksum_get_digest (checksum, hash_a, &size);

  //
  // Create sha_B_Output buffer. It is the SHA of the truncatedKeySeed, the
  // keyIdAsBytes, and the truncatedKeySeed again.
  //
  g_checksum_reset (checksum);
  g_checksum_update (checksum, key_seed, 30);
  g_checksum_update (checksum, kid, kid_len);
  g_checksum_update (checksum, key_seed, 30);
  g_checksum_get_digest (checksum, hash_b, &size);

  //
  // Create sha_C_Output buffer. It is the SHA of the truncatedKeySeed,
  // the keyIdAsBytes, the truncatedKeySeed again, and the keyIdAsBytes
  // again.
  //
  g_checksum_reset (checksum);
  g_checksum_update (checksum, key_seed, 30);
  g_checksum_update (checksum, kid, kid_len);
  g_checksum_update (checksum, key_seed, 30);
  g_checksum_update (checksum, kid, kid_len);
  g_checksum_get_digest (checksum, hash_c, &size);

  key = g_malloc (16);
  for (i = 0; i < 16; i++) {
    key[i] = hash_a[i] ^ hash_a[i + 16] ^ hash_b[i] ^ hash_b[i + 16] ^
        hash_c[i] ^ hash_c[i + 16];
  }

  g_checksum_free (checksum);
  g_free (hash_a);
  g_free (hash_b);
  g_free (hash_c);

  return key;
}

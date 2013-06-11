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

  soup_message_body_append (request->request_message->response_body,
      SOUP_MEMORY_COPY, msg->response_body->data, msg->response_body->length);

  soup_message_set_status (request->request_message, msg->status_code);
  soup_server_unpause_message (request->server, request->request_message);

  g_free (request);
}

/* This resource proxys requests to the Microsoft PlayReady demo
 * server at http://playready.directtaps.net/pr/svc/rightsmanager.asmx
 * Normally, you'd have clients send requests directly to your PlayReady
 * license server.  */
static void
playready_post_resource (GssTransaction * t)
{
  SoupMessage *message;
  struct _Request *request;
  char *url;

  url = g_strdup_printf
      ("http://playready.directtaps.net/pr/svc/rightsmanager.asmx"
      "?UncompressedDigitalVideoOPL=300" "&CompressedDigitalVideoOPL=300"
      "&UncompressedDigitalAudioOPL=300" "&CompressedDigitalAudioOPL=300"
      "&AnalogVideoOPL=300");
  message = soup_message_new (SOUP_METHOD_POST, url);
  g_free (url);
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


/* This is the key seed used by the demo Playready server at
 * http://playready.directtaps.net/pr/svc/rightsmanager.asmx
 * As it is public, it is completely useless as a *private*
 * key seed.  */
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

/*
 * Description of this algorithm is in "PlayReady Header Object",
 * available at:
 *   http://www.microsoft.com/playready/documents/
 * Direct link:
 *   http://download.microsoft.com/download/2/0/2/202E5BD8-36C6-4DB8-9178-12472F8B119E/PlayReady%20Header%20Object%204-15-2013.docx
 */
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

  g_checksum_update (checksum, key_seed, 30);
  g_checksum_update (checksum, kid, kid_len);
  g_checksum_get_digest (checksum, hash_a, &size);

  g_checksum_reset (checksum);
  g_checksum_update (checksum, key_seed, 30);
  g_checksum_update (checksum, kid, kid_len);
  g_checksum_update (checksum, key_seed, 30);
  g_checksum_get_digest (checksum, hash_b, &size);

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

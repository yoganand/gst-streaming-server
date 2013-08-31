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

#include <openssl/aes.h>


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

const char *jpg_files[] = {
  "/roku/xml/bbb-1.jpg",
  "/roku/xml/bbb-2.jpg",
  "/roku/xml/bbb-3.jpg",
  "/roku/xml/bbb-4.jpg",
};

const char *png_files[] = {
  "/roku/xml/smooth-streaming.png",
};

const char *xml_files[] = {
  "/roku/xml/categories.xml",
  "/roku/xml/creativity.xml",
  "/roku/xml/design.xml",
  "/roku/xml/generic.xml",
  "/roku/xml/globalissues.xml",
  "/roku/xml/inspiration.xml",
  "/roku/xml/music.xml",
  "/roku/xml/smooth-streaming.xml",
  "/roku/xml/themind.xml"
};

void
gss_playready_setup (GssServer * server)
{
  int i;

  if (0) {
    gss_server_add_file_resource (server, "/crossdomain.xml", 0, "text/xml");
    gss_server_add_file_resource (server, "/clientaccesspolicy.xml", 0,
        "text/xml");
    gss_server_add_file_resource (server, "/request.cms", 0,
        "application/vnd.ms-sstr+xml");

    gss_server_add_resource (server, "/rightsmanager.asmx",
        0, "text/xml", playready_get_resource, NULL,
        playready_post_resource, server);
  }

  for (i = 0; i < G_N_ELEMENTS (jpg_files); i++) {
    gss_server_add_file_resource (server, jpg_files[i], 0, "image/jpeg");
  }
  for (i = 0; i < G_N_ELEMENTS (png_files); i++) {
    gss_server_add_file_resource (server, png_files[i], 0, "image/png");
  }
  for (i = 0; i < G_N_ELEMENTS (xml_files); i++) {
    gss_server_add_file_resource (server, xml_files[i], 0, "text/xml");
  }
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


char *
gss_playready_get_protection_header_base64 (GssAdaptive * adaptive,
    const char *la_url)
{
  char *wrmheader;
  char *prot_header_base64;
  gunichar2 *utf16;
  glong items;
  int len;
  guchar *content;
  gchar *kid_base64;

  kid_base64 = g_base64_encode (adaptive->kid, adaptive->kid_len);
  /* this all needs to be on one line, to satisfy clients */
  /* Note: DS_ID is ignored by Roku */
  /* Roku checks CHECKSUM if it exists */
  wrmheader =
      g_strdup_printf
      ("<WRMHEADER xmlns=\"http://schemas.microsoft.com/DRM/2007/03/PlayReadyHeader\" "
      "version=\"4.0.0.0\">" "<DATA>" "<PROTECTINFO>" "<KEYLEN>16</KEYLEN>"
      "<ALGID>AESCTR</ALGID>" "</PROTECTINFO>" "<KID>%s</KID>"
      //"<CHECKSUM>BGw1aYZ1YXM=</CHECKSUM>"
      "<CUSTOMATTRIBUTES>"
      "<CONTENT_ID>%s</CONTENT_ID>"
      "<IIS_DRM_VERSION>7.1.1064.0</IIS_DRM_VERSION>" "</CUSTOMATTRIBUTES>"
      "<LA_URL>%s</LA_URL>" "<DS_ID>AH+03juKbUGbHl1V/QIwRA==</DS_ID>"
      "</DATA>" "</WRMHEADER>", kid_base64, adaptive->content_id, la_url);
  g_free (kid_base64);
  len = strlen (wrmheader);
  utf16 = g_utf8_to_utf16 (wrmheader, len, NULL, &items, NULL);

  content = g_malloc (items * sizeof (gunichar2) + 10);
  memcpy (content + 10, utf16, items * sizeof (gunichar2));
  GST_WRITE_UINT32_LE (content, items * sizeof (gunichar2) + 10);
  GST_WRITE_UINT16_LE (content + 4, 1);
  GST_WRITE_UINT16_LE (content + 6, 1);
  GST_WRITE_UINT16_LE (content + 8, items * sizeof (gunichar2));

  prot_header_base64 =
      g_base64_encode (content, items * sizeof (gunichar2) + 10);

  g_free (content);
  g_free (utf16);

  return prot_header_base64;
}

void
gss_adaptive_generate_content_key (GssAdaptive * adaptive)
{
  guint8 *seed;

  seed = gss_playready_get_default_key_seed ();

  adaptive->content_key = gss_playready_generate_key (seed, 30, adaptive->kid,
      adaptive->kid_len);

  g_free (seed);
}

void
gss_playready_setup_iv (GssAdaptive * adaptive,
    GssAdaptiveLevel * level, GssIsomFragment * fragment)
{
  guint64 *init_vectors;
  guint64 iv;
  int i;
  int n_samples;

  gss_utils_get_random_bytes ((guint8 *) & iv, 8);

  n_samples = gss_isom_fragment_get_n_samples (fragment);
  init_vectors = g_malloc (n_samples * sizeof (guint64));
  for (i = 0; i < n_samples; i++) {
    init_vectors[i] = iv + i;
  }
  gss_isom_fragment_set_sample_encryption (fragment, n_samples,
      init_vectors, level->is_h264);
  g_free (init_vectors);

  if (adaptive->content_key == NULL) {
    gss_adaptive_generate_content_key (adaptive);
  }
}

void
gss_playready_encrypt_samples (GssIsomFragment * fragment, guint8 * mdat_data,
    guint8 * content_key)
{
  GssBoxTrun *trun = &fragment->trun;
  GssBoxUUIDSampleEncryption *se = &fragment->sample_encryption;
  guint64 sample_offset;
  int i;

  sample_offset = 8;

  for (i = 0; i < trun->sample_count; i++) {
    unsigned char raw_iv[16];
    unsigned char ecount_buf[16] = { 0 };
    unsigned int num = 0;
    AES_KEY key;

    memset (raw_iv, 0, 16);
    GST_WRITE_UINT64_BE (raw_iv, se->samples[i].iv);

    AES_set_encrypt_key (content_key, 16 * 8, &key);

    if (se->samples[i].num_entries == 0) {
      AES_ctr128_encrypt (mdat_data + sample_offset,
          mdat_data + sample_offset, trun->samples[i].size,
          &key, raw_iv, ecount_buf, &num);
    } else {
      guint64 offset;
      int j;
      offset = sample_offset;
      for (j = 0; j < se->samples[i].num_entries; j++) {
        offset += se->samples[i].entries[j].bytes_of_clear_data;
        AES_ctr128_encrypt (mdat_data + offset,
            mdat_data + offset,
            se->samples[i].entries[j].bytes_of_encrypted_data,
            &key, raw_iv, ecount_buf, &num);
        offset += se->samples[i].entries[j].bytes_of_encrypted_data;
      }
    }
    sample_offset += trun->samples[i].size;
  }
}

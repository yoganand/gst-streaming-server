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

/*
 * Documentation on PlayReady is available here:
 * available at:
 *   http://www.microsoft.com/playready/documents/
 *
 * Some interesting documents, and direct links:
 *
 * PlayReady Header Object
 *   http://download.microsoft.com/download/2/0/2/202E5BD8-36C6-4DB8-9178-12472F8B119E/PlayReady%20Header%20Object%204-15-2013.docx
 *
 * DASH Content Protection using Microsoft PlayReady
 *   http://download.microsoft.com/download/4/8/3/483BE343-B8AF-4569-B7FF-5DCD14354CC5/MPEG%20DASH%20Content%20Protection%20using%20PlayReady%201.1%20-%202013-02-18.docx
 */

#include "config.h"

#include "gss-playready.h"
#include "gss-utils.h"
#include "gss-html.h"

#include <openssl/evp.h>

enum
{
  PROP_LICENSE_URL = 1,
  PROP_KEY_SEED,
  PROP_ALLOW_CLEAR
};

#define DEFAULT_LICENSE_URL "http://playready.directtaps.net/pr/svc/rightsmanager.asmx"
/* This is the key seed used by the demo Playready server at
 * http://playready.directtaps.net/pr/svc/rightsmanager.asmx
 * As it is public, it is completely useless as a *private*
 * key seed.  */
#define DEFAULT_KEY_SEED "5D5068BEC9B384FF6044867159F16D6B755544FCD5116989B1ACC4278E88"
#define DEFAULT_ALLOW_CLEAR FALSE


static void gss_playready_finalize (GObject * object);
static void gss_playready_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gss_playready_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static void gss_playready_get_resource (GssTransaction * t);
static void gss_playready_post_resource (GssTransaction * t);

static GssObject *parent_class;

G_DEFINE_TYPE (GssPlayready, gss_playready, GSS_TYPE_OBJECT);

static void
gss_playready_init (GssPlayready * playready)
{
  playready->license_url = g_strdup (DEFAULT_LICENSE_URL);
  gss_playready_set_key_seed_hex (playready, DEFAULT_KEY_SEED);
  playready->allow_clear = DEFAULT_ALLOW_CLEAR;
}

static void
gss_playready_class_init (GssPlayreadyClass * playready_class)
{
  G_OBJECT_CLASS (playready_class)->set_property = gss_playready_set_property;
  G_OBJECT_CLASS (playready_class)->get_property = gss_playready_get_property;
  G_OBJECT_CLASS (playready_class)->finalize = gss_playready_finalize;

  g_object_class_install_property (G_OBJECT_CLASS (playready_class),
      PROP_LICENSE_URL, g_param_spec_string ("license-url", "License URL",
          "URL for the license server.", DEFAULT_LICENSE_URL,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property (G_OBJECT_CLASS (playready_class),
      PROP_KEY_SEED, g_param_spec_string ("key-seed", "Key Seed",
          "Private key seed used to generate content keys from content IDs.",
          DEFAULT_KEY_SEED,
          (GParamFlags) (GSS_PARAM_SECURE | G_PARAM_READWRITE |
              G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property (G_OBJECT_CLASS (playready_class),
      PROP_ALLOW_CLEAR, g_param_spec_boolean ("allow-clear", "Allow Clear",
          "Allow clear streaming in addition to encrypted streaming.",
          DEFAULT_ALLOW_CLEAR,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  parent_class = g_type_class_peek_parent (playready_class);
}

static void
gss_playready_finalize (GObject * object)
{
  GssPlayready *playready = GSS_PLAYREADY (object);

  g_free (playready->license_url);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gss_playready_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GssPlayready *playready;

  playready = GSS_PLAYREADY (object);

  switch (prop_id) {
    case PROP_LICENSE_URL:
      g_free (playready->license_url);
      playready->license_url = g_value_dup_string (value);
      break;
    case PROP_KEY_SEED:
      gss_playready_set_key_seed_hex (playready, g_value_get_string (value));
      break;
    case PROP_ALLOW_CLEAR:
      playready->allow_clear = g_value_get_boolean (value);
      break;
    default:
      g_assert_not_reached ();
      break;
  }
}

static void
gss_playready_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GssPlayready *playready;

  playready = GSS_PLAYREADY (object);

  switch (prop_id) {
    case PROP_LICENSE_URL:
      g_value_set_string (value, playready->license_url);
      break;
    case PROP_KEY_SEED:
      g_value_take_string (value, gss_playready_get_key_seed_hex (playready));
      break;
    case PROP_ALLOW_CLEAR:
      g_value_set_boolean (value, playready->allow_clear);
      break;
    default:
      g_assert_not_reached ();
      break;
  }
}

GssPlayready *
gss_playready_new (void)
{
  return g_object_new (GSS_TYPE_PLAYREADY, NULL);
}

void
gss_playready_add_resources (GssPlayready * playready, GssServer * server)
{
  GssResource *r;

  GSS_OBJECT_SERVER (playready) = server;

  r = gss_server_add_resource (server, "/admin/playready", GSS_RESOURCE_ADMIN,
      GSS_TEXT_HTML, gss_playready_get_resource, NULL,
      gss_playready_post_resource, playready);
  gss_server_add_admin_resource (server, r, "Playready");

  server->playready = playready;
}

static void
gss_playready_get_resource (GssTransaction * t)
{
  GssPlayready *playready = GSS_PLAYREADY (t->resource->priv);
  GString *s = g_string_new ("");

  t->s = s;

  gss_html_header (t);

  GSS_A ("<h1>Playready</h1>\n");

  gss_config_append_config_block (G_OBJECT (playready), t, TRUE);

  gss_html_footer (t);
}

static void
gss_playready_post_resource (GssTransaction * t)
{
  GssPlayready *playready = GSS_PLAYREADY (t->resource->priv);
  gboolean ret = FALSE;
  GHashTable *hash;

  hash = gss_config_get_post_hash (t);
  if (hash) {
    const char *value;

    value = g_hash_table_lookup (hash, "action");
    if (value) {
    } else {
      ret = gss_config_handle_post_hash (G_OBJECT (playready), t, hash);
    }
  }

  if (ret) {
    gss_config_save_object (G_OBJECT (playready));
    gss_transaction_redirect (t, "");
  } else {
    gss_transaction_error (t, "Configuration Error");
  }

}

void
gss_playready_set_key_seed_hex (GssPlayready * playready, const char *key_seed)
{
  int i;

  if (strlen (key_seed) != 60) {
    GST_ERROR_OBJECT (playready,
        "PlayReady key seed wrong length (should be 60 kex characters)");
    return;
  }

  for (i = 0; i < 60; i++) {
    if (!g_ascii_isxdigit (key_seed[i])) {
      GST_ERROR_OBJECT (playready,
          "PlayReady key seed contains non-hexidecimal characters");
      return;
    }
  }
  for (i = 0; i < 30; i++) {
    playready->key_seed[i] =
        (g_ascii_xdigit_value (key_seed[i * 2]) << 4) |
        g_ascii_xdigit_value (key_seed[i * 2 + 1]);
  }
}

char *
gss_playready_get_key_seed_hex (GssPlayready * playready)
{
  int i;
  char *s;
  s = g_malloc (61);
  for (i = 0; i < 30; i++) {
#define HEXCHAR(x) (((x)<10) ? '0'+(x) : 'a'+((x)-10))
    s[2 * i] = HEXCHAR ((playready->key_seed[i] >> 4) & 0xf);
    s[2 * i + 1] = HEXCHAR (playready->key_seed[i] & 0xf);
  }
  s[60] = 0;
  return s;
}


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
}


/*
 * Description of this algorithm is in "PlayReady Header Object",
 * available at:
 *   http://www.microsoft.com/playready/documents/
 * Direct link:
 *   http://download.microsoft.com/download/2/0/2/202E5BD8-36C6-4DB8-9178-12472F8B119E/PlayReady%20Header%20Object%204-15-2013.docx
 */
void
gss_playready_generate_key (GssPlayready * playready, guint8 * key,
    const guint8 * kid, int kid_len)
{
  GChecksum *checksum;
  guint8 *hash_a;
  guint8 *hash_b;
  guint8 *hash_c;
  gsize size;
  int i;

  checksum = g_checksum_new (G_CHECKSUM_SHA256);
  size = g_checksum_type_get_length (G_CHECKSUM_SHA256);
  hash_a = g_malloc (size);
  hash_b = g_malloc (size);
  hash_c = g_malloc (size);

  g_checksum_update (checksum, playready->key_seed, 30);
  g_checksum_update (checksum, kid, kid_len);
  g_checksum_get_digest (checksum, hash_a, &size);

  g_checksum_reset (checksum);
  g_checksum_update (checksum, playready->key_seed, 30);
  g_checksum_update (checksum, kid, kid_len);
  g_checksum_update (checksum, playready->key_seed, 30);
  g_checksum_get_digest (checksum, hash_b, &size);

  g_checksum_reset (checksum);
  g_checksum_update (checksum, playready->key_seed, 30);
  g_checksum_update (checksum, kid, kid_len);
  g_checksum_update (checksum, playready->key_seed, 30);
  g_checksum_update (checksum, kid, kid_len);
  g_checksum_get_digest (checksum, hash_c, &size);

  for (i = 0; i < 16; i++) {
    key[i] = hash_a[i] ^ hash_a[i + 16] ^ hash_b[i] ^ hash_b[i + 16] ^
        hash_c[i] ^ hash_c[i + 16];
  }

  g_checksum_free (checksum);
  g_free (hash_a);
  g_free (hash_b);
  g_free (hash_c);
}


char *
gss_playready_get_protection_header_base64 (GssAdaptive * adaptive,
    const char *la_url, const char *auth_token)
{
  guchar *content;
  gsize content_len;
  char *prot_header_base64;

  content_len = gss_playready_get_protection_header (adaptive, la_url,
      auth_token, &content);

  prot_header_base64 = g_base64_encode (content, content_len);

  g_free (content);

  return prot_header_base64;
}


gsize
gss_playready_get_protection_header (GssAdaptive * adaptive,
    const char *la_url, const char *auth_token, guchar ** p_content)
{
  char *wrmheader;
  gunichar2 *utf16;
  glong items;
  int len;
  gsize size;
  guchar *content;
  gchar *kid_base64;

  if (auth_token == NULL)
    auth_token = "";

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
      //"<content_id>%s</content_id>"
      //"<auth_token>%s</auth_token>"
      "<IIS_DRM_VERSION>7.1.1064.0</IIS_DRM_VERSION>" "</CUSTOMATTRIBUTES>" "<LA_URL>%s</LA_URL>" "<DS_ID>AH+03juKbUGbHl1V/QIwRA==</DS_ID>" "</DATA>" "</WRMHEADER>", kid_base64,       // adaptive->content_id,
      //auth_token,
      la_url);
  g_free (kid_base64);
  len = strlen (wrmheader);
  utf16 = g_utf8_to_utf16 (wrmheader, len, NULL, &items, NULL);
  g_free (wrmheader);

  content = g_malloc (items * sizeof (gunichar2) + 10);
  memcpy (content + 10, utf16, items * sizeof (gunichar2));
  GST_WRITE_UINT32_LE (content, items * sizeof (gunichar2) + 10);
  GST_WRITE_UINT16_LE (content + 4, 1);
  GST_WRITE_UINT16_LE (content + 6, 1);
  GST_WRITE_UINT16_LE (content + 8, items * sizeof (gunichar2));

  size = items * sizeof (gunichar2) + 10;
  g_free (utf16);

  *p_content = content;

  return size;
}

void
gss_playready_setup_iv (GssPlayready * playready, GssAdaptive * adaptive,
    GssAdaptiveLevel * level, GssIsomFragment * fragment)
{
  guint64 *init_vectors;
  int i;
  int n_samples;
  guint64 iv;
  gboolean is_video;

  n_samples = gss_isom_fragment_get_n_samples (fragment);
  init_vectors = g_malloc (n_samples * sizeof (guint64));
  iv = level->iv + ((guint64) fragment->index << 32);
  for (i = 0; i < n_samples; i++) {
    init_vectors[i] = iv + i;
  }
  is_video = gss_isom_track_is_video (level->track);
  gss_isom_fragment_set_sample_encryption (fragment, n_samples,
      init_vectors, is_video);
  g_free (init_vectors);
}

void
gss_playready_encrypt_samples (GssIsomFragment * fragment, guint8 * mdat_data,
    guint8 * content_key)
{
  GssBoxTrun *trun = &fragment->trun;
  GssBoxUUIDSampleEncryption *se = &fragment->sample_encryption;
  guint64 sample_offset;
  int i;
  int bytes = 0;
  EVP_CIPHER_CTX *ctx;

  sample_offset = 8;

  ctx = EVP_CIPHER_CTX_new ();
  for (i = 0; i < trun->sample_count; i++) {
    unsigned char raw_iv[16];
    int len;

    memset (raw_iv, 0, 16);
    GST_WRITE_UINT64_BE (raw_iv, se->samples[i].iv);

    EVP_EncryptInit_ex (ctx, EVP_aes_128_ctr (), NULL, content_key, raw_iv);

    if (se->samples[i].num_entries == 0) {
      EVP_EncryptUpdate (ctx,
          mdat_data + sample_offset, &len,
          mdat_data + sample_offset, trun->samples[i].size);
      bytes += trun->samples[i].size;
    } else {
      guint64 offset;
      int j;
      offset = sample_offset;
      for (j = 0; j < se->samples[i].num_entries; j++) {
        offset += se->samples[i].entries[j].bytes_of_clear_data;
        EVP_EncryptUpdate (ctx,
            mdat_data + offset, &len,
            mdat_data + offset,
            se->samples[i].entries[j].bytes_of_encrypted_data);
        offset += se->samples[i].entries[j].bytes_of_encrypted_data;
        bytes += se->samples[i].entries[j].bytes_of_encrypted_data;
      }
    }
    sample_offset += trun->samples[i].size;
  }
  EVP_CIPHER_CTX_free (ctx);
}

const char *
gss_playready_get_uri (GssDrmType drm_type)
{
  switch (drm_type) {
    case GSS_DRM_CLEAR:
      return "";
    case GSS_DRM_PLAYREADY:
      return "urn:uuid:9a04f079-9840-4286-ab92-e65be0885f95";
    case GSS_DRM_CENC:
      return "urn:mpeg:dash:mp4protection:2011";
    default:
      return NULL;
  }
}

GssDrmType
gss_drm_get_drm_type (const char *s)
{
  if (strcmp (s, "pr") == 0)
    return GSS_DRM_PLAYREADY;
  if (strcmp (s, "clear") == 0)
    return GSS_DRM_CLEAR;
  return GSS_DRM_UNKNOWN;
}

const char *
gss_drm_get_drm_name (GssDrmType drm_type)
{
  switch (drm_type) {
    case GSS_DRM_CLEAR:
      return "clear";
    case GSS_DRM_PLAYREADY:
      return "pr";
    case GSS_DRM_CENC:
      return "cenc";
    default:
      return "";
  }
}

const guint8 *
gss_drm_get_drm_uuid (GssDrmType drm_type)
{
  static const guint8 playready_uuid[] =
      { 0x9a, 0x04, 0xf0, 0x79, 0x98, 0x40, 0x42, 0x86, 0xab, 0x92, 0xe6, 0x5b,
    0xe0, 0x88, 0x5f, 0x95
  };

  switch (drm_type) {
    default:
    case GSS_DRM_CLEAR:
      return NULL;
    case GSS_DRM_PLAYREADY:
      return playready_uuid;
  }
}

void
gss_playready_add_protection_header (GssIsomMovie * movie,
    GssAdaptive * adaptive, const char *la_url, const char *auth_token)
{
  static const guint8 playready_uuid[] =
      { 0x9a, 0x04, 0xf0, 0x79, 0x98, 0x40, 0x42, 0x86, 0xab, 0x92, 0xe6, 0x5b,
    0xe0, 0x88, 0x5f, 0x95
  };

  GST_ERROR ("got here");
  movie->pssh.present = TRUE;

  memcpy (movie->pssh.uuid, playready_uuid, 16);

  movie->pssh.len =
      gss_playready_get_protection_header (adaptive, la_url, auth_token,
      &movie->pssh.data);

}

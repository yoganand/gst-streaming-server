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

#include <gst/gst.h>
#include <gst/base/gstbytereader.h>

#include "gss-server.h"
#include "gss-html.h"
#include "gss-session.h"
#include "gss-soup.h"
#include "gss-content.h"
#include "gss-ism-parser.h"
#include "gss-playready.h"
#include "gss-utils.h"

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <openssl/aes.h>

#define AUDIO_TRACK_ID 1
#define VIDEO_TRACK_ID 2

typedef struct _GssISM GssISM;
typedef struct _GssISMLevel GssISMLevel;

struct _GssISM
{
  guint64 duration;

  int max_width;
  int max_height;

  int n_audio_levels;
  int n_video_levels;

  char *video_codec_data;
  char *audio_codec_data;
  gboolean playready;
  int audio_rate;

  GssISMLevel *audio_levels;
  GssISMLevel *video_levels;

  gboolean needs_encryption;

  guint8 *kid;
  gsize kid_len;
  guint8 *content_key;
};

struct _GssISMLevel
{
  const char *filename;

  int n_fragments;
  int bitrate;
  int video_width;
  int video_height;

  GssISMParser *parser;
  int track_id;
};


GssISM *gss_ism_new (void);
void gss_ism_free (GssISM * ism);
GssISMLevel *gss_ism_get_level (GssISM * ism, gboolean video, guint64 bitrate);


static void gss_smooth_streaming_resource_get_manifest (GssTransaction * t);
static void gss_smooth_streaming_resource_get_content (GssTransaction * t);

typedef struct _GssFileFragment GssFileFragment;


static void
gss_ism_generate_content_key (GssISM * ism)
{
  guint8 *seed;

  seed = gss_playready_get_default_key_seed ();

  ism->content_key = gss_playready_generate_key (seed, 30, ism->kid,
      ism->kid_len);

  g_free (seed);
}

static guint8 *
gss_ism_assemble_chunk (GssTransaction * t, GssISM * ism,
    GssISMLevel * level, GssISMFragment * fragment)
{
  guint8 *mdat_data;
  off_t ret;
  int fd;
  ssize_t n;
  int i;
  int offset;

  fd = open (level->filename, O_RDONLY);
  if (fd < 0) {
    GST_WARNING ("file not found %s", level->filename);
    soup_message_set_status (t->msg, SOUP_STATUS_NOT_FOUND);
    return NULL;
  }

  mdat_data = g_malloc (fragment->mdat_size);

  GST_WRITE_UINT32_BE (mdat_data, fragment->mdat_size);
  GST_WRITE_UINT32_LE (mdat_data + 4, GST_MAKE_FOURCC ('m', 'd', 'a', 't'));
  offset = 8;
  for (i = 0; i < fragment->n_mdat_chunks; i++) {
    GST_DEBUG ("chunk %d: %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT,
        i, fragment->chunks[i].offset, fragment->chunks[i].size);
    ret = lseek (fd, fragment->chunks[i].offset, SEEK_SET);
    if (ret < 0) {
      GST_WARNING ("failed to seek");
      soup_message_set_status (t->msg, SOUP_STATUS_NOT_FOUND);
      close (fd);
      return NULL;
    }

    n = read (fd, mdat_data + offset, fragment->chunks[i].size);
    if (n < fragment->chunks[i].size) {
      GST_WARNING ("read failed");
      g_free (mdat_data);
      soup_message_set_status (t->msg, SOUP_STATUS_NOT_FOUND);
      close (fd);
      return NULL;
    }
    offset += fragment->chunks[i].size;
  }
  close (fd);

  return mdat_data;
}


static void
gss_ism_send_chunk (GssTransaction * t, GssISM * ism,
    GssISMLevel * level, GssISMFragment * fragment, guint8 * mdat_data)
{
  guint8 *moof_data;
  int moof_size;

  gss_ism_fragment_serialize (fragment, &moof_data, &moof_size);

  soup_message_set_status (t->msg, SOUP_STATUS_OK);
  soup_message_body_append (t->msg->response_body, SOUP_MEMORY_TAKE, moof_data,
      moof_size);
  soup_message_body_append (t->msg->response_body, SOUP_MEMORY_TAKE, mdat_data,
      fragment->mdat_size);
}

static void
gss_ism_playready_setup_iv (GssISM * ism,
    GssISMLevel * level, GssISMFragment * fragment)
{
  guint64 *init_vectors;
  guint64 iv;
  int i;
  int n_samples;

  gss_utils_get_random_bytes ((guint8 *) & iv, 8);

  n_samples = gss_ism_fragment_get_n_samples (fragment);
  init_vectors = g_malloc (n_samples * sizeof (guint64));
  for (i = 0; i < n_samples; i++) {
    init_vectors[i] = iv + i;
  }
  gss_ism_fragment_set_sample_encryption (fragment, n_samples,
      init_vectors, (fragment->track_id == VIDEO_TRACK_ID));
  g_free (init_vectors);

  if (ism->content_key == NULL) {
    gss_ism_generate_content_key (ism);
  }
}

/* FIXME should be extracted from file */
typedef struct _ISMInfo ISMInfo;
struct _ISMInfo
{
  const char *filename;
  const char *mount;
  gboolean is_encrypted;
  int video_bitrate;
  const char *video_codec_data;
  int audio_bitrate;
  gboolean enable_drm;
};

ISMInfo ism_files[] = {
  {
        "SuperSpeedway_720_2962.ismv",
        "SuperSpeedwayPR",
        TRUE,
        2962000,
        "000000016764001FAC2CA5014016EFFC100010014808080A000007D200017700C100005A648000B4C9FE31C6080002D3240005A64FF18E1DA12251600000000168E9093525",
        128000,
        TRUE,
      },
  {
        "SuperSpeedway/SuperSpeedway_720_2962.ismv",
        "SuperSpeedway",
        FALSE,
        2962000,
        "000000016764001FAC2CA5014016EFFC100010014808080A000007D200017700C100005A648000B4C9FE31C6080002D3240005A64FF18E1DA12251600000000168E9093525",
        128000,
        FALSE,
      },
  {
        "drwho-406.ismv",
        "boondocks",
        FALSE,
        2500000,
        NULL,
        128000,
        FALSE,
      },
  {
        "boondocks-411.ismv",
        "boondocksHD",
        FALSE,
        5000000,
        NULL,
        128000,
        FALSE,
      },
  {
        "boondocks-411.ismv",
        "boondocksHD-DRM",
        FALSE,
        5000000,
        NULL,
        128000,
        TRUE,
      },
  {
        "drwho-406.mp4",
        "drwho",
        FALSE,
        2500000,
        NULL,
        128000,
      TRUE},
};

static char *
get_codec_string (guint8 * codec_data, int len)
{
  char *s;
  int i;

  if (codec_data == NULL)
    return g_strdup ("");

  s = g_malloc (len * 2 + 1);
  for (i = 0; i < len; i++) {
    sprintf (s + i * 2, "%02x", codec_data[i]);
  }
  return s;
}

void
gss_smooth_streaming_setup (GssServer * server)
{
  int i;

  for (i = 0; i < G_N_ELEMENTS (ism_files); i++) {
    ISMInfo *info = &ism_files[i];
    GssISM *ism;
    GssISMParser *parser;
    char *s;

    ism = gss_ism_new ();

    /* FIXME */
    ism->kid = g_base64_decode ("AmfjCTOPbEOl3WD/5mcecA==", &ism->kid_len);

    ism->n_video_levels = 1;
    ism->video_levels = g_malloc0 (ism->n_video_levels * sizeof (GssISMLevel));
    ism->n_audio_levels = 1;
    ism->audio_levels = g_malloc0 (ism->n_audio_levels * sizeof (GssISMLevel));

    parser = gss_ism_parser_new ();
    gss_ism_parser_parse_file (parser, info->filename);

    if (gss_ism_parser_get_n_fragments (parser, AUDIO_TRACK_ID) == 0) {
      gss_ism_parser_fragmentize (parser);
    }

    ism->max_width = parser->movie->tracks[1]->mp4v.width;
    ism->max_height = parser->movie->tracks[1]->mp4v.height;

    ism->audio_levels[0].n_fragments =
        gss_ism_parser_get_n_fragments (parser, AUDIO_TRACK_ID);
    ism->audio_levels[0].parser = parser;
    ism->audio_levels[0].track_id = AUDIO_TRACK_ID;
    ism->audio_levels[0].filename = g_strdup (info->filename);
    ism->audio_levels[0].bitrate = info->audio_bitrate;

    ism->video_levels[0].n_fragments =
        gss_ism_parser_get_n_fragments (parser, VIDEO_TRACK_ID);
    ism->video_levels[0].filename = g_strdup (info->filename);
    ism->video_levels[0].bitrate = info->video_bitrate;
    ism->video_levels[0].video_width = parser->movie->tracks[1]->mp4v.width;
    ism->video_levels[0].video_height = parser->movie->tracks[1]->mp4v.height;
    ism->video_levels[0].parser = parser;
    ism->video_levels[0].track_id = VIDEO_TRACK_ID;

    ism->duration = gss_ism_parser_get_duration (parser, VIDEO_TRACK_ID);

    if (info->video_codec_data) {
      ism->video_codec_data = g_strdup (info->video_codec_data);

#if 0
      g_print ("%s\n", ism->video_codec_data);
      g_print ("%s\n",
          get_codec_string (parser->movie->tracks[1]->esds.codec_data,
              parser->movie->tracks[1]->esds.codec_data_len));
#endif
    } else {
      ism->video_codec_data =
          get_codec_string (parser->movie->tracks[1]->esds.codec_data,
          parser->movie->tracks[1]->esds.codec_data_len);
    }
    ism->audio_codec_data =
        get_codec_string (parser->movie->tracks[0]->esds.codec_data,
        parser->movie->tracks[0]->esds.codec_data_len);
    GST_DEBUG ("video: %s", ism->video_codec_data);
    GST_DEBUG ("audio: %s", ism->audio_codec_data);
    ism->audio_rate = parser->movie->tracks[0]->mp4a.sample_rate >> 16;
    GST_DEBUG ("sample_rate: %d", ism->audio_rate);
    ism->playready = info->enable_drm;
    ism->needs_encryption = info->enable_drm && (!info->is_encrypted);

    s = g_strdup_printf ("/%s/Manifest", info->mount);
    gss_server_add_resource (server, s, 0, "text/xml;charset=utf-8",
        gss_smooth_streaming_resource_get_manifest, NULL, NULL, ism);
    g_free (s);
    s = g_strdup_printf ("/%s/content", info->mount);
    gss_server_add_resource (server, s, 0, "video/mp4",
        gss_smooth_streaming_resource_get_content, NULL, NULL, ism);
    g_free (s);
  }
}

static void
gss_smooth_streaming_resource_get_manifest (GssTransaction * t)
{
  GssISM *ism = (GssISM *) t->resource->priv;
  GString *s = g_string_new ("");
  int i;

  t->s = s;

  GSS_A ("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");

  GSS_P
      ("<SmoothStreamingMedia MajorVersion=\"2\" MinorVersion=\"1\" Duration=\"%"
      G_GUINT64_FORMAT "\">\n", ism->duration);
  GSS_P
      ("  <StreamIndex Type=\"video\" Name=\"video\" Chunks=\"%d\" QualityLevels=\"%d\" MaxWidth=\"%d\" MaxHeight=\"%d\" "
      "DisplayWidth=\"%d\" DisplayHeight=\"%d\" "
      "Url=\"content?stream=video&amp;bitrate={bitrate}&amp;start_time={start time}\">\n",
      ism->video_levels[0].n_fragments, ism->n_video_levels, ism->max_width,
      ism->max_height, ism->max_width, ism->max_height);
  /* also IsLive, LookaheadCount, DVRWindowLength */

  for (i = 0; i < ism->n_video_levels; i++) {
    GssISMLevel *level = &ism->video_levels[i];

    GSS_P ("    <QualityLevel Index=\"%d\" Bitrate=\"%d\" "
        "FourCC=\"H264\" MaxWidth=\"%d\" MaxHeight=\"%d\" "
        "CodecPrivateData=\"%s\" />\n", i, level->bitrate, level->video_width,
        level->video_height, ism->video_codec_data);
  }
  {
    GssISMLevel *level = &ism->video_levels[0];

    for (i = 0; i < level->n_fragments; i++) {
      GssISMFragment *fragment;
      fragment = gss_ism_parser_get_fragment (level->parser,
          level->track_id, i);
      GSS_P ("    <c d=\"%" G_GUINT64_FORMAT "\" />\n",
          (guint64) fragment->duration);
    }
  }
  GSS_A ("  </StreamIndex>\n");

  GSS_P ("  <StreamIndex Type=\"audio\" Index=\"0\" Name=\"audio\" "
      "Chunks=\"%d\" QualityLevels=\"%d\" "
      "Url=\"content?stream=audio&amp;bitrate={bitrate}&amp;start_time={start time}\">\n",
      ism->audio_levels[0].n_fragments, ism->n_audio_levels);
  for (i = 0; i < ism->n_video_levels; i++) {
    GssISMLevel *level = &ism->audio_levels[i];

    GSS_P ("    <QualityLevel FourCC=\"AACL\" Bitrate=\"%d\" "
        "SamplingRate=\"%d\" Channels=\"2\" BitsPerSample=\"16\" "
        "PacketSize=\"4\" AudioTag=\"255\" CodecPrivateData=\"%s\" />\n",
        level->bitrate, ism->audio_rate, ism->audio_codec_data);
  }
  {
    GssISMLevel *level = &ism->audio_levels[0];

    for (i = 0; i < level->n_fragments; i++) {
      GssISMFragment *fragment;
      fragment = gss_ism_parser_get_fragment (level->parser,
          level->track_id, i);
      GSS_P ("    <c d=\"%" G_GUINT64_FORMAT "\" />\n",
          (guint64) fragment->duration);
    }
  }

  GSS_A ("  </StreamIndex>\n");
  if (ism->playready) {
    GSS_A ("<Protection>\n");
    GSS_A
        ("  <ProtectionHeader SystemID=\"9a04f079-9840-4286-ab92-e65be0885f95\">");
    {
      char *wrmheader;
      char *prot_header_base64;
      gunichar2 *utf16;
      glong items;
      int len;
      guchar *content;
      const char *la_url;
      gchar *kid_base64;

      la_url = "http://playready.directtaps.net/pr/svc/rightsmanager.asmx";

      kid_base64 = g_base64_encode (ism->kid, ism->kid_len);
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
          "<IIS_DRM_VERSION>7.1.1064.0</IIS_DRM_VERSION>" "</CUSTOMATTRIBUTES>"
          "<LA_URL>%s</LA_URL>" "<DS_ID>AH+03juKbUGbHl1V/QIwRA==</DS_ID>"
          "</DATA>" "</WRMHEADER>", kid_base64, la_url);
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

      GSS_P ("%s", prot_header_base64);

      g_free (prot_header_base64);
      g_free (content);
      g_free (utf16);
    }
    GSS_A ("</ProtectionHeader>\n");
    GSS_A ("</Protection>\n");
  }
  GSS_A ("</SmoothStreamingMedia>\n");

}

static gboolean
g_hash_table_lookup_guint64 (GHashTable * hash, const char *key,
    guint64 * value)
{
  const char *s;
  char *end;

  s = g_hash_table_lookup (hash, key);
  if (s == NULL)
    return FALSE;

  if (s[0] == '\0')
    return FALSE;

  *value = g_ascii_strtoull (s, &end, 0);

  if (end[0] != '\0')
    return FALSE;
  return TRUE;
}

static void
gss_smooth_streaming_resource_get_content (GssTransaction * t)
{
  GssISM *ism = (GssISM *) t->resource->priv;
  guint64 start_time;
  const char *stream;
  guint64 bitrate;
  GssISMLevel *level;
  GssISMFragment *fragment;

  //GST_ERROR ("content request");

  if (t->query == NULL) {
    GST_ERROR ("no query");
    soup_message_set_status (t->msg, SOUP_STATUS_NOT_FOUND);
    return;
  }

  stream = g_hash_table_lookup (t->query, "stream");

  if (stream == NULL ||
      !g_hash_table_lookup_guint64 (t->query, "start_time", &start_time) ||
      !g_hash_table_lookup_guint64 (t->query, "bitrate", &bitrate)) {
    GST_ERROR ("bad params");
    soup_message_set_status (t->msg, SOUP_STATUS_NOT_FOUND);
    return;
  }

  if (strcmp (stream, "audio") != 0 && strcmp (stream, "video") != 0) {
    GST_ERROR ("bad stream %s", stream);
    soup_message_set_status (t->msg, SOUP_STATUS_NOT_FOUND);
    return;
  }

  level = gss_ism_get_level (ism, (stream[0] == 'v'), bitrate);
  if (level == NULL) {
    GST_ERROR ("no level for %s, %" G_GUINT64_FORMAT, stream, bitrate);
    soup_message_set_status (t->msg, SOUP_STATUS_NOT_FOUND);
    return;
  }

  fragment = gss_ism_parser_get_fragment_by_timestamp (level->parser,
      level->track_id, start_time);
  if (fragment == NULL) {
    GST_ERROR ("no fragment for %" G_GUINT64_FORMAT, start_time);
    soup_message_set_status (t->msg, SOUP_STATUS_NOT_FOUND);
    return;
  }
  //GST_ERROR ("frag %s %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT,
  //    level->filename, fragment->offset, fragment->size);

  {
    guint8 *mdat_data;

    if (ism->needs_encryption) {
      gss_ism_playready_setup_iv (ism, level, fragment);
    }
    mdat_data = gss_ism_assemble_chunk (t, ism, level, fragment);
    if (ism->needs_encryption) {
      gss_ism_encrypt_samples (fragment, mdat_data, ism->content_key);
    }
    gss_ism_send_chunk (t, ism, level, fragment, mdat_data);
  }
}

GssISM *
gss_ism_new (void)
{
  GssISM *ism;

  ism = g_malloc0 (sizeof (GssISM));

  return ism;

}

void
gss_ism_free (GssISM * ism)
{

  g_free (ism->audio_levels);
  g_free (ism->video_levels);
  g_free (ism);
}

GssISMLevel *
gss_ism_get_level (GssISM * ism, gboolean video, guint64 bitrate)
{
  int i;
  if (video) {
    for (i = 0; i < ism->n_video_levels; i++) {
      if (ism->video_levels[i].bitrate == bitrate) {
        return &ism->video_levels[i];
      }
    }
  } else {
    for (i = 0; i < ism->n_audio_levels; i++) {
      if (ism->audio_levels[i].bitrate == bitrate) {
        return &ism->audio_levels[i];
      }
    }
  }
  return NULL;
}

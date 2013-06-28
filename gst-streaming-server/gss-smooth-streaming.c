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
 *
 * http://msdn.microsoft.com/en-us/library/ff469518.aspx
 *
 */

#include "config.h"

#include <gst/gst.h>
#include <gst/base/gstbytereader.h>

#include "gss-smooth-streaming.h"
#include "gss-server.h"
#include "gss-html.h"
#include "gss-session.h"
#include "gss-soup.h"
#include "gss-content.h"
#include "gss-isom.h"
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


static void gss_smooth_streaming_resource_get_manifest (GssTransaction * t);
static void gss_smooth_streaming_resource_get_content (GssTransaction * t);


static guint8 *
gss_ism_assemble_chunk (GssTransaction * t, GssISM * ism,
    GssISMLevel * level, GssIsomFragment * fragment)
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
    GssISMLevel * level, GssIsomFragment * fragment, guint8 * mdat_data)
{
  guint8 *moof_data;
  int moof_size;

  gss_isom_fragment_serialize (fragment, &moof_data, &moof_size);

  soup_message_set_status (t->msg, SOUP_STATUS_OK);
  soup_message_body_append (t->msg->response_body, SOUP_MEMORY_TAKE, moof_data,
      moof_size);
  soup_message_body_append (t->msg->response_body, SOUP_MEMORY_TAKE, mdat_data,
      fragment->mdat_size);
}

/* FIXME should be extracted from file */
typedef struct _ISMInfo ISMInfo;
struct _ISMInfo
{
  const char *filename;
  const char *mount;
  char *kid_base64;
  int video_bitrate;
  const char *video_codec_data;
  int audio_bitrate;
  gboolean enable_drm;
};

ISMInfo ism_files[] = {
  {
        "SuperSpeedway_720_2962.ismv",
        "SuperSpeedwayPR",
        "AmfjCTOPbEOl3WD/5mcecA==",
        2962000,
        "000000016764001FAC2CA5014016EFFC100010014808080A000007D200017700C100005A648000B4C9FE31C6080002D3240005A64FF18E1DA12251600000000168E9093525",
        128000,
        TRUE,
      },
  {
        "SuperSpeedway/SuperSpeedway_720_2962.ismv",
        "SuperSpeedway",
        NULL,
        2962000,
        "000000016764001FAC2CA5014016EFFC100010014808080A000007D200017700C100005A648000B4C9FE31C6080002D3240005A64FF18E1DA12251600000000168E9093525",
        128000,
        FALSE,
      },
  {
        "drwho-406.ismv",
        "boondocks",
        NULL,
        2500000,
        NULL,
        128000,
        FALSE,
      },
  {
        "boondocks-411.ismv",
        "boondocksHD",
        NULL,
        5000000,
        NULL,
        128000,
        FALSE,
      },
  {
        "boondocks-411.ismv",
        "boondocksHD-DRM",
        NULL,
        5000000,
        NULL,
        128000,
        TRUE,
      },
  {
        "drwho-406.mp4",
        "drwho",
        NULL,
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
    GssIsomFile *file;
    GssIsomTrack *video_track;
    char *s;

    ism = gss_ism_new ();

    if (info->kid_base64) {
      ism->kid = g_base64_decode ("AmfjCTOPbEOl3WD/5mcecA==", &ism->kid_len);
    } else {
      ism->kid_len = 16;
      ism->kid = g_malloc (ism->kid_len);
      gss_utils_get_random_bytes ((guint8 *) ism->kid, ism->kid_len);
    }

    ism->n_video_levels = 1;
    ism->video_levels = g_malloc0 (ism->n_video_levels * sizeof (GssISMLevel));
    ism->n_audio_levels = 1;
    ism->audio_levels = g_malloc0 (ism->n_audio_levels * sizeof (GssISMLevel));

    file = gss_isom_file_new ();
    gss_isom_file_parse_file (file, info->filename);

    if (gss_isom_file_get_n_fragments (file, AUDIO_TRACK_ID) == 0) {
      gss_isom_file_fragmentize (file);
    }

    video_track = gss_isom_movie_get_video_track (file->movie);
    ism->max_width = MAX (ism->max_width, video_track->mp4v.width);
    ism->max_height = MAX (ism->max_height, video_track->mp4v.height);

    ism->audio_levels[0].track_id = AUDIO_TRACK_ID;
    ism->audio_levels[0].n_fragments = gss_isom_file_get_n_fragments (file,
        ism->audio_levels[0].track_id);
    ism->audio_levels[0].file = file;
    ism->audio_levels[0].track_id = AUDIO_TRACK_ID;
    ism->audio_levels[0].filename = g_strdup (info->filename);
    ism->audio_levels[0].bitrate = info->audio_bitrate;

    ism->video_levels[0].track_id = VIDEO_TRACK_ID;
    ism->video_levels[0].n_fragments = gss_isom_file_get_n_fragments (file,
        ism->video_levels[0].track_id);
    ism->video_levels[0].filename = g_strdup (info->filename);
    ism->video_levels[0].bitrate = info->video_bitrate;
    ism->video_levels[0].video_width = file->movie->tracks[1]->mp4v.width;
    ism->video_levels[0].video_height = file->movie->tracks[1]->mp4v.height;
    ism->video_levels[0].file = file;
    ism->video_levels[0].is_h264 = TRUE;

    ism->duration = gss_isom_file_get_duration (file, VIDEO_TRACK_ID);

    if (info->video_codec_data) {
      ism->video_codec_data = g_strdup (info->video_codec_data);
    } else {
      ism->video_codec_data =
          get_codec_string (file->movie->tracks[1]->esds.codec_data,
          file->movie->tracks[1]->esds.codec_data_len);
    }
    ism->audio_codec_data =
        get_codec_string (file->movie->tracks[0]->esds.codec_data,
        file->movie->tracks[0]->esds.codec_data_len);
    ism->audio_rate = file->movie->tracks[0]->mp4a.sample_rate >> 16;
    ism->playready = info->enable_drm;
    ism->needs_encryption = info->enable_drm && (info->kid_base64 == NULL);

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
      GssIsomFragment *fragment;
      fragment = gss_isom_file_get_fragment (level->file, level->track_id, i);
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
      GssIsomFragment *fragment;
      fragment = gss_isom_file_get_fragment (level->file, level->track_id, i);
      GSS_P ("    <c d=\"%" G_GUINT64_FORMAT "\" />\n",
          (guint64) fragment->duration);
    }
  }

  GSS_A ("  </StreamIndex>\n");
  if (ism->playready) {
    char *prot_header_base64;

    GSS_A ("<Protection>\n");
    GSS_A ("  <ProtectionHeader "
        "SystemID=\"9a04f079-9840-4286-ab92-e65be0885f95\">");

    prot_header_base64 = gss_playready_get_protection_header_base64 (ism,
        "http://playready.directtaps.net/pr/svc/rightsmanager.asmx");
    GSS_P ("%s", prot_header_base64);
    g_free (prot_header_base64);

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
  GssIsomFragment *fragment;

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

  fragment = gss_isom_file_get_fragment_by_timestamp (level->file,
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
      gss_playready_setup_iv (ism, level, fragment);
    }
    mdat_data = gss_ism_assemble_chunk (t, ism, level, fragment);
    if (ism->needs_encryption) {
      gss_playready_encrypt_samples (fragment, mdat_data, ism->content_key);
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

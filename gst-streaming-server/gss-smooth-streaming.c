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
#include <glib/gstdio.h>

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

#define GSS_ISM_SECOND 10000000

static void gss_smooth_streaming_resource_get_manifest2 (GssTransaction * t,
    GssISM * ism);
static void gss_smooth_streaming_resource_get_content2 (GssTransaction * t,
    GssISM * ism);
#if 0
static void gss_smooth_streaming_resource_get_manifest (GssTransaction * t);
static void gss_smooth_streaming_resource_get_content (GssTransaction * t);
#endif
static void gss_smooth_streaming_get_resource (GssTransaction * t);
static void load_file (GssISM * ism, char *filename, int video_bitrate,
    int audio_bitrate);

static GHashTable *ism_cache;


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
  ism_cache = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
      (GDestroyNotify) gss_ism_free);

  gss_server_add_resource (server, "/ism-vod/", GSS_RESOURCE_PREFIX,
      NULL, gss_smooth_streaming_get_resource, NULL, NULL, NULL);

}

#if 0
static void
gss_smooth_streaming_resource_get_manifest (GssTransaction * t)
{
  gss_smooth_streaming_resource_get_manifest2 (t, t->resource->priv);
}
#endif

static void
gss_smooth_streaming_resource_get_manifest2 (GssTransaction * t, GssISM * ism)
{
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
        level->video_height, level->codec_data);
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
  for (i = 0; i < ism->n_audio_levels; i++) {
    GssISMLevel *level = &ism->audio_levels[i];

    GSS_P ("    <QualityLevel FourCC=\"AACL\" Bitrate=\"%d\" "
        "SamplingRate=\"%d\" Channels=\"2\" BitsPerSample=\"16\" "
        "PacketSize=\"4\" AudioTag=\"255\" CodecPrivateData=\"%s\" />\n",
        level->bitrate, level->audio_rate, level->codec_data);
    break;
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

static void
gss_smooth_streaming_resource_get_dash_mpd (GssTransaction * t, GssISM * ism)
{
  GString *s = g_string_new ("");
  int i;

  t->s = s;

  GSS_P ("<MPD xmlns=\"urn:mpeg:DASH:schema:MPD:2011\" "
      "mediaPresentationDuration=\"PT%dS\" "
      "minBufferTime=\"PT5S\" "
      "profiles=\"urn:mpeg:dash:profile:isoff-on-demand:2011\" "
      "type=\"static\">\n", (int) (ism->duration / GSS_ISM_SECOND));
  GSS_A ("  <BaseURL>http://localhost:8080/ism-vod/southpark/</BaseURL>\n");
  GSS_P ("  <Period duration=\"PT%dS\" start=\"PT0S\">\n",
      (int) (ism->duration / GSS_ISM_SECOND));

  GSS_A ("    <AdaptationSet mimeType=\"video/mp4v\" "
      "codecs=\"avc1.4D401F\" frameRate=\"24/1\" "
      "segmentAlignment=\"true\" subsegmentAlignment=\"true\" "
      "bitstreamSwitching=\"true\">\n");
  GSS_A ("      <SegmentTemplate timescale=\"100000\" "
      "initialization=\"$Bandwidth$/init.mp4v\" "
      "media=\"content?stream=video&amp;bitrate=$Bandwidth$&amp;start_time=$Time$\">\n");
  GSS_A ("        <SegmentTimeline>\n");
  GSS_P ("          <S t=\"0\" d=\"500000\" r=\"%d\"/>\n",
      ism->video_levels[0].n_fragments);
  GSS_A ("        </SegmentTimeline>\n");
  GSS_A ("      </SegmentTemplate>\n");

  for (i = 0; i < ism->n_video_levels; i++) {
    GssISMLevel *level = &ism->video_levels[i];

    GSS_P ("      <Representation id=\"v%d\" width=\"%d\" height=\"%d\" "
        "bandwidth=\"%d\"/>\n",
        i, level->video_width, level->video_height, level->bitrate);
  }
  GSS_A ("    </AdaptationSet>\n");

  GSS_A ("    <AdaptationSet mimeType=\"video/mp4v\" "
      "codecs=\"mp4a\" frameRate=\"24/1\" "
      "segmentAlignment=\"true\" subsegmentAlignment=\"true\" "
      "bitstreamSwitching=\"true\">\n");
  GSS_A ("      <SegmentTemplate timescale=\"100000\" "
      "initialization=\"$Bandwidth$/init.mp4v\" "
      "media=\"content?stream=audio&amp;bitrate=$Bandwidth$&amp;start_time=$Time$\">\n");
  GSS_A ("        <SegmentTimeline>\n");
  GSS_P ("          <S t=\"0\" d=\"500000\" r=\"%d\"/>\n",
      ism->video_levels[0].n_fragments);
  GSS_A ("        </SegmentTimeline>\n");
  GSS_A ("      </SegmentTemplate>\n");
  for (i = 0; i < ism->n_audio_levels; i++) {
    GssISMLevel *level = &ism->audio_levels[i];

    GSS_P ("      <Representation id=\"a%d\" bandwidth=\"%d\"/>\n",
        i, level->bitrate);
  }
  GSS_A ("    </AdaptationSet>\n");
  GSS_A ("  </Period>\n");
  GSS_A ("</MPD>\n");

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

#if 0
static void
gss_smooth_streaming_resource_get_content (GssTransaction * t)
{
  gss_smooth_streaming_resource_get_content2 (t, t->resource->priv);
}
#endif

static void
gss_smooth_streaming_resource_get_content2 (GssTransaction * t, GssISM * ism)
{
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

static gboolean
split (const char *line, char **filename, int *video_bitrate,
    int *audio_bitrate)
{
  const char *s = line;
  char *end;
  int i;

  while (g_ascii_isspace (s[0]))
    s++;
  if (s[0] == '#' || s[0] == 0)
    return FALSE;

  for (i = 0; s[i]; i++) {
    if (g_ascii_isspace (s[i]))
      break;
  }

  *filename = g_strndup (s, i);
  *video_bitrate = 0;
  *audio_bitrate = 0;

  s += i;
  while (g_ascii_isspace (s[0]))
    s++;

  *video_bitrate = strtoul (s, &end, 10);
  s = end;

  *audio_bitrate = strtoul (s, &end, 10);
  s = end;

  return TRUE;
}

static guint8 *
create_key_id (const char *key_string)
{
  GChecksum *checksum;
  guint8 *bytes;
  gsize size;

  checksum = g_checksum_new (G_CHECKSUM_SHA1);
  bytes = g_malloc (20);
  g_checksum_update (checksum, (const guint8 *) key_string, -1);
  g_checksum_update (checksum,
      (const guint8 *) "KThMK9Tibb+X9qRuTvwOchPRwH+4hV05yZXnx7C", -1);
  g_checksum_get_digest (checksum, bytes, &size);
  g_checksum_free (checksum);

  return bytes;
}

static GssISM *
gss_ism_load (const char *key)
{
  GssISM *ism;
  int i;
  char *filename;
  char *contents;
  gsize length;
  char **lines;
  gboolean ret;
  GError *error = NULL;

  GST_DEBUG ("looking for %s", key);

  filename = g_strdup_printf ("ism-vod/%s/gss-manifest", key);
  ret = g_file_get_contents (filename, &contents, &length, &error);
  g_free (filename);
  if (!ret) {
    g_error_free (error);
    return NULL;
  }

  GST_DEBUG ("loading %s", key);

  ism = gss_ism_new ();

  ism->kid = create_key_id (key);
  ism->kid_len = 16;

  lines = g_strsplit (contents, "\n", 0);
  for (i = 0; lines[i]; i++) {
    char *fn = NULL;
    int video_bitrate = 0;
    int audio_bitrate = 0;
    char *full_fn;

    ret = split (lines[i], &fn, &video_bitrate, &audio_bitrate);
    if (!ret)
      continue;

    GST_ERROR ("fn %s video_bitrate %d audio_bitrate %d",
        fn, video_bitrate, audio_bitrate);

    full_fn = g_strdup_printf ("ism-vod/%s/%s", key, fn);

    load_file (ism, full_fn, video_bitrate, audio_bitrate);
    g_free (full_fn);
    g_free (fn);
  }

  ism->playready = TRUE;
  ism->needs_encryption = TRUE;

  g_strfreev (lines);
  g_free (contents);

  GST_DEBUG ("loading done");

  return ism;
}

static void
load_file (GssISM * ism, char *filename, int video_bitrate, int audio_bitrate)
{
  GssIsomFile *file;
  GssIsomTrack *video_track;
  GssIsomTrack *audio_track;

  file = gss_isom_file_new ();
  gss_isom_file_parse_file (file, filename);

  if (gss_isom_file_get_n_fragments (file, AUDIO_TRACK_ID) == 0) {
    gss_isom_file_fragmentize (file);
  }

  if (ism->duration == 0) {
    ism->duration = gss_isom_file_get_duration (file, VIDEO_TRACK_ID);
  }

  video_track = gss_isom_movie_get_video_track (file->movie);
  if (video_track) {
    GssISMLevel *level;

    ism->video_levels = g_realloc (ism->video_levels,
        (ism->n_video_levels + 1) * sizeof (GssISMLevel));
    level = ism->video_levels + ism->n_video_levels;
    ism->n_video_levels++;
    memset (level, 0, sizeof (GssISMLevel));

    ism->max_width = MAX (ism->max_width, video_track->mp4v.width);
    ism->max_height = MAX (ism->max_height, video_track->mp4v.height);

    level->track_id = VIDEO_TRACK_ID;
    level->n_fragments = gss_isom_file_get_n_fragments (file, level->track_id);
    level->filename = g_strdup (filename);
    level->bitrate = video_bitrate;
    level->video_width = file->movie->tracks[1]->mp4v.width;
    level->video_height = file->movie->tracks[1]->mp4v.height;
    level->file = file;
    level->is_h264 = TRUE;

    level->codec_data = get_codec_string (video_track->esds.codec_data,
        video_track->esds.codec_data_len);
  }

  audio_track = gss_isom_movie_get_audio_track (file->movie);
  if (audio_track) {
    GssISMLevel *level;

    ism->audio_levels = g_realloc (ism->audio_levels,
        (ism->n_audio_levels + 1) * sizeof (GssISMLevel));
    level = ism->audio_levels + ism->n_audio_levels;
    ism->n_audio_levels++;
    memset (level, 0, sizeof (GssISMLevel));

    level->track_id = AUDIO_TRACK_ID;
    level->n_fragments = gss_isom_file_get_n_fragments (file, level->track_id);
    level->file = file;
    level->track_id = AUDIO_TRACK_ID;
    level->filename = g_strdup (filename);
    level->bitrate = audio_bitrate;
    level->codec_data = get_codec_string (audio_track->esds.codec_data,
        audio_track->esds.codec_data_len);
    level->audio_rate = audio_track->mp4a.sample_rate >> 16;
  }

}

static void
gss_smooth_streaming_get_resource (GssTransaction * t)
{
  const char *e;
  const char *path;
  char *key;
  char *subpath;
  GssISM *ism;

  path = t->path + 9;
  e = strchr (path, '/');
  if (e == NULL) {
    soup_message_set_status (t->msg, SOUP_STATUS_NOT_FOUND);
    return;
  }

  key = g_strndup (path, e - path);
  subpath = g_strdup (e + 1);

  ism = g_hash_table_lookup (ism_cache, key);
  if (ism == NULL) {
    ism = gss_ism_load (key);
    if (ism == NULL) {
      g_free (key);
      g_free (subpath);
      soup_message_set_status (t->msg, SOUP_STATUS_NOT_FOUND);
      return;
    }
    g_hash_table_replace (ism_cache, key, ism);
  } else {
    g_free (key);
  }

  if (strcmp (subpath, "Manifest") == 0) {
    gss_smooth_streaming_resource_get_manifest2 (t, ism);
  } else if (strcmp (subpath, "content") == 0) {
    gss_smooth_streaming_resource_get_content2 (t, ism);
  } else if (strcmp (subpath, "manifest.mpd") == 0) {
    gss_smooth_streaming_resource_get_dash_mpd (t, ism);
  } else {
    soup_message_set_status (t->msg, SOUP_STATUS_NOT_FOUND);
  }

  g_free (subpath);
}

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
#include <json-glib/json-glib.h>

#include "gss-adaptive.h"
#include "gss-server.h"
#include "gss-html.h"
#include "gss-session.h"
#include "gss-soup.h"
#include "gss-content.h"
#include "gss-isom.h"
#include "gss-playready.h"
#include "gss-sglist.h"
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

static void gss_adaptive_resource_get_manifest (GssTransaction * t,
    GssAdaptive * adaptive);
static void gss_adaptive_resource_get_content (GssTransaction * t,
    GssAdaptive * adaptive);
static void load_file (GssAdaptive * adaptive, const char *filename);
static void gss_adaptive_async_assemble_chunk (GssAdaptiveAsync * async);
static void gss_adaptive_async_assemble_chunk_finish (GssAdaptiveAsync * async);


static guint8 *
gss_adaptive_assemble_chunk (GssTransaction * t, GssAdaptive * adaptive,
    GssAdaptiveLevel * level, GssIsomFragment * fragment)
{
  GError *error = NULL;
  guint8 *mdat_data;
  int fd;
  gboolean ret;

  g_return_val_if_fail (t != NULL, NULL);
  g_return_val_if_fail (adaptive != NULL, NULL);
  g_return_val_if_fail (level != NULL, NULL);
  g_return_val_if_fail (fragment != NULL, NULL);

  fd = open (level->filename, O_RDONLY);
  if (fd < 0) {
    GST_WARNING ("failed to open \"%s\", error=\"%s\", broken manifest?",
        level->filename, g_strerror (errno));
    gss_transaction_error_not_found (t,
        "failed to open file (broken manifest?)");
    return NULL;
  }

  mdat_data = g_malloc (fragment->mdat_size);

  GST_WRITE_UINT32_BE (mdat_data, fragment->mdat_size);
  GST_WRITE_UINT32_LE (mdat_data + 4, GST_MAKE_FOURCC ('m', 'd', 'a', 't'));

  ret = gss_sglist_load (fragment->sglist, fd, mdat_data + 8, &error);
  if (!ret) {
    gss_transaction_error_not_found (t, error->message);
    g_error_free (error);
    g_free (mdat_data);
    close (fd);
    return NULL;
  }

  close (fd);

  return mdat_data;
}


typedef struct _ManifestQuery ManifestQuery;
struct _ManifestQuery
{
  int max_pixels;
  int max_width;
  int max_height;
  int max_bitrate;
  int max_profile;
  int max_level;
  char *auth_token;
};

static void
parse_manifest_query (ManifestQuery * mq, GssTransaction * t)
{
  const char *str;

  mq->max_pixels = INT_MAX;
  mq->max_width = INT_MAX;
  mq->max_height = INT_MAX;
  mq->max_bitrate = INT_MAX;
  mq->max_profile = INT_MAX;
  mq->max_level = INT_MAX;
  mq->auth_token = NULL;

  if (t->query == NULL)
    return;

  str = g_hash_table_lookup (t->query, "max_pixels");
  if (str)
    mq->max_pixels = strtoul (str, NULL, 0);
  str = g_hash_table_lookup (t->query, "max_width");
  if (str)
    mq->max_width = strtoul (str, NULL, 0);
  str = g_hash_table_lookup (t->query, "max_height");
  if (str)
    mq->max_height = strtoul (str, NULL, 0);
  str = g_hash_table_lookup (t->query, "max_bitrate");
  if (str)
    mq->max_bitrate = strtoul (str, NULL, 0);
  str = g_hash_table_lookup (t->query, "max_profile");
  if (str)
    mq->max_profile = strtoul (str, NULL, 0);
  str = g_hash_table_lookup (t->query, "max_level");
  if (str)
    mq->max_level = strtoul (str, NULL, 0);

  mq->auth_token = g_hash_table_lookup (t->query, "auth_token");
}

static gboolean
manifest_query_check_video (ManifestQuery * mq, GssAdaptiveLevel * level)
{
  if (level->video_width > mq->max_width ||
      level->video_height > mq->max_height ||
      level->video_width * level->video_height > mq->max_pixels ||
      level->profile > mq->max_profile ||
      level->level > mq->max_level || level->bitrate > mq->max_bitrate)
    return FALSE;
  return TRUE;
}

static void
gss_adaptive_resource_get_manifest (GssTransaction * t, GssAdaptive * adaptive)
{
  GString *s = g_string_new ("");
  ManifestQuery mq;
  int i;
  int show_audio_levels;

  t->s = s;

  parse_manifest_query (&mq, t);

  GSS_A ("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");

  GSS_P
      ("<SmoothStreamingMedia MajorVersion=\"2\" MinorVersion=\"1\" Duration=\"%"
      G_GUINT64_FORMAT "\">\n", adaptive->duration);
  GSS_P
      ("  <StreamIndex Type=\"video\" Name=\"video\" Chunks=\"%d\" QualityLevels=\"%d\" MaxWidth=\"%d\" MaxHeight=\"%d\" "
      "DisplayWidth=\"%d\" DisplayHeight=\"%d\" "
      "Url=\"content?stream=video&amp;bitrate={bitrate}&amp;start_time={start time}\">\n",
      adaptive->video_levels[0].n_fragments, adaptive->n_video_levels,
      adaptive->max_width, adaptive->max_height, adaptive->max_width,
      adaptive->max_height);
  /* also IsLive, LookaheadCount, DVRWindowLength */

  for (i = 0; i < adaptive->n_video_levels; i++) {
    GssAdaptiveLevel *level = &adaptive->video_levels[i];

    if (manifest_query_check_video (&mq, level)) {
      GSS_P ("    <QualityLevel Index=\"%d\" Bitrate=\"%d\" "
          "FourCC=\"H264\" MaxWidth=\"%d\" MaxHeight=\"%d\" "
          "CodecPrivateData=\"%s\" />\n", i, level->bitrate, level->video_width,
          level->video_height, level->codec_data);
    }
  }
  {
    GssAdaptiveLevel *level = &adaptive->video_levels[0];

    for (i = 0; i < level->n_fragments; i++) {
      GssIsomFragment *fragment;
      fragment = gss_isom_track_get_fragment (level->track, i);
      GSS_P ("    <c d=\"%" G_GUINT64_FORMAT "\" />\n",
          (guint64) fragment->duration);
    }
  }
  GSS_A ("  </StreamIndex>\n");

  show_audio_levels = 1;
  GSS_P ("  <StreamIndex Type=\"audio\" Index=\"0\" Name=\"audio\" "
      "Chunks=\"%d\" QualityLevels=\"%d\" "
      "Url=\"content?stream=audio&amp;bitrate={bitrate}&amp;start_time={start time}\">\n",
      adaptive->audio_levels[0].n_fragments, show_audio_levels);
  for (i = 0; i < show_audio_levels; i++) {
    GssAdaptiveLevel *level = &adaptive->audio_levels[i];

    GSS_P ("    <QualityLevel FourCC=\"AACL\" Bitrate=\"%d\" "
        "SamplingRate=\"%d\" Channels=\"2\" BitsPerSample=\"16\" "
        "PacketSize=\"4\" AudioTag=\"255\" CodecPrivateData=\"%s\" />\n",
        level->bitrate, level->audio_rate, level->codec_data);
    break;
  }
  {
    GssAdaptiveLevel *level = &adaptive->audio_levels[0];

    for (i = 0; i < level->n_fragments; i++) {
      GssIsomFragment *fragment;
      fragment = gss_isom_track_get_fragment (level->track, i);
      GSS_P ("    <c d=\"%" G_GUINT64_FORMAT "\" />\n",
          (guint64) fragment->duration);
    }
  }

  GSS_A ("  </StreamIndex>\n");
  if (adaptive->drm_type == GSS_DRM_PLAYREADY) {
    char *prot_header_base64;

    GSS_A ("<Protection>\n");
    GSS_A ("  <ProtectionHeader "
        "SystemID=\"9a04f079-9840-4286-ab92-e65be0885f95\">");

    prot_header_base64 = gss_playready_get_protection_header_base64 (adaptive,
        t->server->playready->license_url, mq.auth_token);
    GSS_P ("%s", prot_header_base64);
    g_free (prot_header_base64);

    GSS_A ("</ProtectionHeader>\n");
    GSS_A ("</Protection>\n");
  }
  GSS_A ("</SmoothStreamingMedia>\n");

}

static void
append_content_protection (GssTransaction * t, GssAdaptive * adaptive,
    const char *auth_token)
{
  GString *s = t->s;

  if (adaptive->drm_type == GSS_DRM_PLAYREADY) {
    char *prot_header_base64;
    GSS_A ("      <ContentProtection "
        "schemeIdUri=\"urn:uuid:9a04f079-9840-4286-ab92-e65be0885f95\">\n");
    prot_header_base64 =
        gss_playready_get_protection_header_base64 (adaptive,
        t->server->playready->license_url, auth_token);
    GSS_P ("        <mspr:pro>%s</mspr:pro>\n", prot_header_base64);
    g_free (prot_header_base64);
    GSS_A ("      </ContentProtection>\n");
  }

}

static void
gss_adaptive_resource_get_dash_range_mpd (GssTransaction * t,
    GssAdaptive * adaptive)
{
  GString *s = g_string_new ("");
  int i;
  ManifestQuery mq;

  parse_manifest_query (&mq, t);
  t->s = s;

  soup_message_headers_replace (t->msg->response_headers, "Content-Type",
      "application/octet-stream");

  GSS_A ("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
  GSS_A ("<MPD xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"\n"
      "  xmlns=\"urn:mpeg:dash:schema:mpd:2011\"\n");
  if (adaptive->drm_type == GSS_DRM_PLAYREADY) {
    GSS_A ("  xmlns:mspr=\"urn:microsoft:playready\"\n");
  }
  GSS_P ("  xsi:schemaLocation=\"urn:mpeg:dash:schema:mpd:2011 DASH-MPD.xsd\"\n"
      "  type=\"static\"\n"
      "  mediaPresentationDuration=\"PT%dS\"\n"
      "  minBufferTime=\"PT2S\"\n"
      "  profiles=\"urn:mpeg:dash:profile:isoff-on-demand:2011\">\n",
      (int) (adaptive->duration / GSS_ISM_SECOND));
  GSS_P ("  <Period>\n");

  GSS_A ("    <AdaptationSet mimeType=\"audio/mp4\" "
      "lang=\"en\" "
      "subsegmentAlignment=\"true\" " "subsegmentStartsWithSAP=\"1\">\n");
  append_content_protection (t, adaptive, mq.auth_token);
  for (i = 0; i < adaptive->n_audio_levels; i++) {
    GssAdaptiveLevel *level = &adaptive->audio_levels[i];
    GssIsomTrack *track = level->track;

    GSS_P ("      <Representation id=\"a%d\" codecs=\"%s\" bandwidth=\"%d\">\n",
        i, level->codec, level->bitrate);
    GSS_P ("        <BaseURL>content/a%d</BaseURL>\n", i);
    GSS_P ("        <SegmentBase indexRange=\"%" G_GSIZE_FORMAT "-%"
        G_GSIZE_FORMAT "\">" "<Initialization range=\"%" G_GSIZE_FORMAT "-%"
        G_GSIZE_FORMAT "\" /></SegmentBase>\n", track->dash_header_size,
        track->dash_header_and_sidx_size - 1, (gsize) 0,
        track->dash_header_size - 1);
    GSS_A ("      </Representation>\n");
    break;
  }
  GSS_A ("    </AdaptationSet>\n");

  GSS_A ("    <AdaptationSet mimeType=\"video/mp4\" "
      "subsegmentAlignment=\"true\" " "subsegmentStartsWithSAP=\"1\">\n");
  append_content_protection (t, adaptive, mq.auth_token);
  for (i = 0; i < adaptive->n_video_levels; i++) {
    GssAdaptiveLevel *level = &adaptive->video_levels[i];
    GssIsomTrack *track = level->track;

    if (manifest_query_check_video (&mq, level)) {
      GSS_P ("      <Representation id=\"v%d\" bandwidth=\"%d\" "
          "codecs=\"%s\" width=\"%d\" height=\"%d\">\n",
          i, level->bitrate, level->codec,
          level->video_width, level->video_height);
      GSS_P ("        <BaseURL>content/v%d</BaseURL>\n", i);
      GSS_P ("        <SegmentBase indexRange=\"%" G_GSIZE_FORMAT "-%"
          G_GSIZE_FORMAT "\">" "<Initialization range=\"%" G_GSIZE_FORMAT "-%"
          G_GSIZE_FORMAT "\" /></SegmentBase>\n", track->dash_header_size,
          track->dash_header_and_sidx_size - 1, (gsize) 0,
          track->dash_header_size - 1);
      GSS_A ("      </Representation>\n");
    }
  }
  GSS_A ("    </AdaptationSet>\n");

  GSS_A ("  </Period>\n");
  GSS_A ("</MPD>\n");
  GSS_A ("\n");

}

static gboolean
ranges_overlap (guint64 start1, guint64 len1, guint64 start2, guint64 len2)
{
  if (start1 + len1 <= start2 || start2 + len2 <= start1)
    return FALSE;
  return TRUE;
}

#if 0
static guint64
overlap_size (guint64 start1, guint64 size1, guint64 start2, guint64 size2)
{
  guint64 start;
  guint64 end;

  start = MAX (start1, start2);
  end = MIN (start1 + size1, start2 + size2);
  if (start < end) {
    return end - start;
  }
  return 0;
}
#endif

static void
gss_soup_message_body_append_clipped (SoupMessageBody * body,
    SoupMemoryUse use, guint8 * data, guint64 start1, guint64 size1,
    guint64 start2, guint64 size2)
{
  guint64 start;
  guint64 end;
  guint64 offset;

  start = MAX (start1, start2);
  end = MIN (start1 + size1, start2 + size2);
  if (start >= end)
    return;

  offset = start - start2;
  soup_message_body_append (body, use, data + offset, end - start);
}

static void
gss_adaptive_resource_get_dash_range_fragment (GssTransaction * t,
    GssAdaptive * adaptive, const char *path)
{
  gboolean have_range;
  SoupRange *ranges;
  int n_ranges;
  int index;
  GssAdaptiveLevel *level;
  gsize start, end;
  int i;
  guint64 offset;
  guint64 n_bytes;
  guint64 header_size;

  /* skip over content/ */
  path += 8;

  if (path[0] != 'a' && path[0] != 'v') {
    GST_ERROR ("bad path: %s", path);
    return;
  }
  index = strtoul (path + 1, NULL, 10);

  level = NULL;
  if (path[0] == 'a') {
    if (index < adaptive->n_audio_levels) {
      level = &adaptive->audio_levels[index];
    }
  } else {
    if (index < adaptive->n_video_levels) {
      level = &adaptive->video_levels[index];
    }
  }

  if (level == NULL) {
    GST_ERROR ("bad level: %c%d from path %s", path[0], index, path);
    return;
  }

  if (t->msg->method == SOUP_METHOD_HEAD) {
    GST_DEBUG ("%s: HEAD", path);
    soup_message_headers_set_content_length (t->msg->response_headers,
        level->track->dash_size);
    return;
  }

  have_range = soup_message_headers_get_ranges (t->msg->request_headers,
      level->track->dash_size, &ranges, &n_ranges);

  if (have_range) {
    if (n_ranges != 1) {
      GST_ERROR ("too many ranges");
    }
    start = ranges[0].start;
    end = ranges[0].end + 1;
  } else {
    start = 0;
    end = level->track->dash_size;
  }
  GST_DEBUG ("%s: range: %ld-%ld", path, start, end);

  offset = start;
  n_bytes = end - start;

  if (ranges_overlap (offset, n_bytes, 0,
          level->track->dash_header_and_sidx_size)) {
    gss_soup_message_body_append_clipped (t->msg->response_body,
        SOUP_MEMORY_COPY, level->track->dash_header_data,
        offset, n_bytes, 0, level->track->dash_header_and_sidx_size);
  }
  header_size = level->track->dash_header_and_sidx_size;

  for (i = 0; i < level->track->n_fragments; i++) {
    GssIsomFragment *fragment = level->track->fragments[i];
    guint8 *mdat_data;

    if (offset + n_bytes <= fragment->offset)
      break;

    if (ranges_overlap (offset, n_bytes, header_size + fragment->offset,
            fragment->moof_size)) {
      gss_soup_message_body_append_clipped (t->msg->response_body,
          SOUP_MEMORY_COPY, fragment->moof_data,
          offset, n_bytes, header_size + fragment->offset, fragment->moof_size);
    }

    if (ranges_overlap (offset, n_bytes, header_size + fragment->offset +
            fragment->moof_size, fragment->mdat_size)) {
      mdat_data = gss_adaptive_assemble_chunk (t, adaptive, level, fragment);
      if (adaptive->drm_type != GSS_DRM_CLEAR) {
        gss_playready_encrypt_samples (fragment, mdat_data,
            adaptive->content_key);
      }

      gss_soup_message_body_append_clipped (t->msg->response_body,
          SOUP_MEMORY_COPY, mdat_data + 8,
          offset, n_bytes, header_size + fragment->offset + fragment->moof_size,
          fragment->mdat_size - 8);
      g_free (mdat_data);
    }
  }

  soup_message_body_complete (t->msg->response_body);

  if (have_range) {
    soup_message_headers_set_content_range (t->msg->response_headers,
        ranges[0].start, ranges[0].end, level->track->dash_size);

    soup_message_set_status (t->msg, SOUP_STATUS_PARTIAL_CONTENT);

    soup_message_headers_free_ranges (t->msg->response_headers, ranges);
  } else {
    soup_message_set_status (t->msg, SOUP_STATUS_OK);
  }

  soup_message_headers_replace (t->msg->response_headers, "Content-Type",
      (path[0] == 'v') ? "video/mp4" : "audio/mp4");
}

static void
gss_adaptive_resource_get_dash_live_mpd (GssTransaction * t,
    GssAdaptive * adaptive)
{
  GString *s = g_string_new ("");
  int i;
  ManifestQuery mq;

  parse_manifest_query (&mq, t);

  t->s = s;

  soup_message_headers_replace (t->msg->response_headers, "Content-Type",
      "application/octet-stream");

  GSS_P ("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");
  GSS_A ("<MPD xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"\n"
      "  xmlns=\"urn:mpeg:dash:schema:mpd:2011\"\n");
  if (adaptive->drm_type == GSS_DRM_PLAYREADY) {
    GSS_A ("  xmlns:mspr=\"urn:microsoft:playready\"\n");
  }
  GSS_P ("  xsi:schemaLocation=\"urn:mpeg:dash:schema:mpd:2011 DASH-MPD.xsd\"\n"
      "  type=\"static\"\n"
      "  mediaPresentationDuration=\"PT%dS\"\n"
      "  minBufferTime=\"PT4S\"\n"
      "  profiles=\"urn:mpeg:dash:profile:isoff-live:2011\">\n",
      (int) (adaptive->duration / GSS_ISM_SECOND));
  GSS_P ("  <Period>\n");

  GSS_A ("    <AdaptationSet " "id=\"1\" "
      "profiles=\"ccff\" "
      "bitstreamSwitching=\"true\" "
      "segmentAlignment=\"true\" "
      "contentType=\"audio\" " "mimeType=\"audio/mp4\" " "lang=\"en\">\n");
  append_content_protection (t, adaptive, mq.auth_token);
  GSS_A ("    <SegmentTemplate timescale=\"10000000\" "
      "media=\"content?stream=audio&amp;bitrate=$Bandwidth$&amp;start_time=$Time$\" "
      "initialization=\"content?stream=audio&amp;bitrate=$Bandwidth$&amp;start_time=init\">\n");
  GSS_A ("      <SegmentTimeline>\n");
  {
    GssAdaptiveLevel *level = &adaptive->audio_levels[0];

    for (i = 0; i < level->n_fragments; i++) {
      GssIsomFragment *fragment;
      fragment = gss_isom_track_get_fragment (level->track, i);
      GSS_P ("        <S d=\"%" G_GUINT64_FORMAT "\" />\n",
          (guint64) fragment->duration);
    }
  }
  GSS_A ("      </SegmentTimeline>\n");
  GSS_A ("    </SegmentTemplate>\n");
  for (i = 0; i < adaptive->n_audio_levels; i++) {
    GssAdaptiveLevel *level = &adaptive->audio_levels[i];

    GSS_P ("      <Representation id=\"a%d\" codecs=\"%s\" "
        "bandwidth=\"%d\" audioSamplingRate=\"%d\"/>\n",
        i, level->codec, level->bitrate, level->audio_rate);
  }
  GSS_A ("    </AdaptationSet>\n");

  GSS_P ("    <AdaptationSet " "id=\"2\" "
      "profiles=\"ccff\" "
      "bitstreamSwitching=\"true\" "
      "segmentAlignment=\"true\" "
      "contentType=\"video\" "
      "mimeType=\"video/mp4\" "
      "maxWidth=\"1920\" " "maxHeight=\"1080\" " "startWithSAP=\"1\">\n");
  append_content_protection (t, adaptive, mq.auth_token);

  GSS_A ("    <SegmentTemplate timescale=\"10000000\" "
      "media=\"content?stream=video&amp;bitrate=$Bandwidth$&amp;start_time=$Time$\" "
      "initialization=\"content?stream=video&amp;bitrate=$Bandwidth$&amp;start_time=init\">\n");
  GSS_A ("      <SegmentTimeline>\n");
  {
    GssAdaptiveLevel *level = &adaptive->video_levels[0];

    for (i = 0; i < level->n_fragments; i++) {
      GssIsomFragment *fragment;
      fragment = gss_isom_track_get_fragment (level->track, i);
      GSS_P ("        <S d=\"%" G_GUINT64_FORMAT "\" />\n",
          (guint64) fragment->duration);
    }
  }
  GSS_A ("      </SegmentTimeline>\n");
  GSS_A ("    </SegmentTemplate>\n");
  for (i = 0; i < adaptive->n_video_levels; i++) {
    GssAdaptiveLevel *level = &adaptive->video_levels[i];

    if (manifest_query_check_video (&mq, level)) {
      GSS_P ("      <Representation id=\"v%d\" bandwidth=\"%d\" "
          "codecs=\"%s\" width=\"%d\" height=\"%d\"/>\n",
          i, level->bitrate, level->codec,
          level->video_width, level->video_height);
    }
  }
  GSS_A ("    </AdaptationSet>\n");

  GSS_A ("  </Period>\n");
  GSS_A ("</MPD>\n");
  GSS_A ("\n");

}

static gboolean
parse_guint64 (const char *s, guint64 * value)
{
  char *end;

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
gss_adaptive_resource_get_content (GssTransaction * t, GssAdaptive * adaptive)
{
  const char *stream;
  const char *start_time_str;
  const char *bitrate_str;
  guint64 start_time;
  guint64 bitrate;
  gboolean is_init;
  GssAdaptiveLevel *level;
  GssIsomFragment *fragment;
  gboolean ret;

  //GST_ERROR ("content request");

  if (t->query == NULL) {
    gss_transaction_error_not_found (t, "no smooth streaming query");
    return;
  }

  stream = g_hash_table_lookup (t->query, "stream");
  if (stream == NULL) {
    gss_transaction_error_not_found (t, "missing stream parameterr");
    return;
  }
  start_time_str = g_hash_table_lookup (t->query, "start_time");
  if (start_time_str == NULL) {
    gss_transaction_error_not_found (t, "missing start_time parameter");
    return;
  }
  bitrate_str = g_hash_table_lookup (t->query, "bitrate");
  if (bitrate_str == NULL) {
    gss_transaction_error_not_found (t, "missing bitrate parameter");
    return;
  }

  ret = parse_guint64 (bitrate_str, &bitrate);
  if (!ret) {
    gss_transaction_error_not_found (t, "bitrate is not a number");
    return;
  }

  if (strcmp (start_time_str, "init") == 0) {
    is_init = TRUE;
    start_time = 0;
  } else {
    is_init = FALSE;
    ret = parse_guint64 (start_time_str, &start_time);
    if (!ret) {
      gss_transaction_error_not_found (t,
          "start_time is not a number or \"init\"");
      return;
    }
  }

  if (strcmp (stream, "audio") != 0 && strcmp (stream, "video") != 0) {
    gss_transaction_error_not_found (t, "stream is not \"audio\" or \"video\"");
    return;
  }

  level = gss_adaptive_get_level (adaptive, (stream[0] == 'v'), bitrate);
  if (level == NULL) {
    gss_transaction_error_not_found (t,
        "level not found for stream and bitrate");
    return;
  }

  soup_message_headers_replace (t->msg->response_headers, "Content-Type",
      (stream[0] == 'v') ? "video/mp4" : "audio/mp4");

  if (is_init) {
    soup_message_body_append (t->msg->response_body, SOUP_MEMORY_COPY,
        level->track->ccff_header_data, level->track->ccff_header_size);
  } else {
    GssAdaptiveAsync *async;

    fragment = gss_isom_track_get_fragment_by_timestamp (level->track,
        start_time);
    if (fragment == NULL) {
      gss_transaction_error_not_found (t, "fragment not found for start_time");
      return;
    }
    //GST_ERROR ("frag %s %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT,
    //    level->filename, fragment->offset, fragment->size);

    soup_server_pause_message (t->soupserver, t->msg);

    async = gss_adaptive_async_new ();
    async->adaptive = adaptive;
    async->level = level;
    async->fragment = fragment;
    async->transaction = t;
    async->process = gss_adaptive_async_assemble_chunk;
    async->finish = gss_adaptive_async_assemble_chunk_finish;

    gss_adaptive_async_push (async);
  }
}

static void
gss_adaptive_async_assemble_chunk (GssAdaptiveAsync * async)
{

  async->data = gss_adaptive_assemble_chunk (async->transaction,
      async->adaptive, async->level, async->fragment);
  if (async->adaptive->drm_type != GSS_DRM_CLEAR) {
    gss_playready_encrypt_samples (async->fragment, async->data,
        async->adaptive->content_key);
  }
}

static void
gss_adaptive_async_assemble_chunk_finish (GssAdaptiveAsync * async)
{
  GssTransaction *t = async->transaction;

  soup_message_set_status (t->msg, SOUP_STATUS_OK);
  /* strip off mdat header at end of moof_data */
  soup_message_body_append (t->msg->response_body, SOUP_MEMORY_COPY,
      async->fragment->moof_data, async->fragment->moof_size - 8);
  soup_message_body_append (t->msg->response_body, SOUP_MEMORY_TAKE,
      async->data, async->fragment->mdat_size);
  soup_server_unpause_message (t->soupserver, t->msg);
}

GssAdaptive *
gss_adaptive_new (void)
{
  GssAdaptive *adaptive;

  adaptive = g_malloc0 (sizeof (GssAdaptive));

  return adaptive;

}

void
gss_adaptive_free (GssAdaptive * adaptive)
{
  int i;

  g_return_if_fail (adaptive != NULL);

  for (i = 0; i < adaptive->n_parsers; i++) {
    gss_isom_parser_free (adaptive->parsers[i]);
  }

  for (i = 0; i < adaptive->n_audio_levels; i++) {
    adaptive->audio_levels[i].track = NULL;
    g_free (adaptive->audio_levels[i].codec_data);
    g_free (adaptive->audio_levels[i].filename);
    g_free (adaptive->audio_levels[i].codec);
  }
  for (i = 0; i < adaptive->n_video_levels; i++) {
    adaptive->video_levels[i].track = NULL;
    g_free (adaptive->video_levels[i].codec_data);
    g_free (adaptive->video_levels[i].filename);
    g_free (adaptive->video_levels[i].codec);
  }
  g_free (adaptive->drm_info.data);
  g_free (adaptive->audio_levels);
  g_free (adaptive->video_levels);
  g_free (adaptive->content_id);
  g_free (adaptive->kid);
  g_free (adaptive);
}

GssAdaptiveLevel *
gss_adaptive_get_level (GssAdaptive * adaptive, gboolean video, guint64 bitrate)
{
  int i;

  g_return_val_if_fail (adaptive != NULL, NULL);
  g_return_val_if_fail (bitrate != 0, NULL);

  if (video) {
    for (i = 0; i < adaptive->n_video_levels; i++) {
      if (adaptive->video_levels[i].bitrate == bitrate) {
        return &adaptive->video_levels[i];
      }
    }
  } else {
    for (i = 0; i < adaptive->n_audio_levels; i++) {
      if (adaptive->audio_levels[i].bitrate == bitrate) {
        return &adaptive->audio_levels[i];
      }
    }
  }
  return NULL;
}

static guint8 *
create_key_id (const char *key_string)
{
  GChecksum *checksum;
  guint8 *bytes;
  gsize size;

  g_return_val_if_fail (key_string != NULL, NULL);

  checksum = g_checksum_new (G_CHECKSUM_SHA1);
  bytes = g_malloc (20);
  g_checksum_update (checksum, (const guint8 *) key_string, -1);
  g_checksum_update (checksum,
      (const guint8 *) "KThMK9Tibb+X9qRuTvwOchPRwH+4hV05yZXnx7C", -1);
  size = 20;
  g_checksum_get_digest (checksum, bytes, &size);
  g_checksum_free (checksum);

  return bytes;
}

static gboolean
parse_json (GssAdaptive * adaptive, JsonParser * parser, const char *dir)
{
  JsonNode *node;
  JsonObject *obj;
  JsonNode *n;
  JsonArray *version_array;
  int version;
  int len;
  int i;

  g_return_val_if_fail (adaptive != NULL, FALSE);
  g_return_val_if_fail (parser != NULL, FALSE);
  g_return_val_if_fail (dir != NULL, FALSE);

  node = json_parser_get_root (parser);
  obj = json_node_get_object (node);

  n = json_object_get_member (obj, "manifest_version");
  version = json_node_get_int (n);
  if (version != 0) {
    GST_ERROR ("bad version %d", version);
    return FALSE;
  }

  n = json_object_get_member (obj, "versions");
  version_array = json_node_get_array (n);
  len = json_array_get_length (version_array);
  for (i = 0; i < len; i++) {
    JsonArray *files_array;
    int files_len;
    const char *version_string;
    int j;

    n = json_array_get_element (version_array, i);
    if (n == NULL)
      return FALSE;
    obj = json_node_get_object (n);
    if (obj == NULL)
      return FALSE;

    n = json_object_get_member (obj, "version");
    if (n == NULL)
      return FALSE;
    version_string = json_node_get_string (n);
    if (version_string == NULL)
      return FALSE;

    n = json_object_get_member (obj, "files");
    if (n == NULL)
      return FALSE;
    files_array = json_node_get_array (n);
    if (files_array == NULL)
      return FALSE;
    files_len = json_array_get_length (files_array);

    if (files_len == 0)
      return FALSE;

    for (j = 0; j < files_len; j++) {
      const char *filename;
      char *full_fn;

      n = json_array_get_element (files_array, j);
      if (n == NULL)
        return FALSE;
      if (json_node_get_node_type (n) == JSON_NODE_OBJECT) {
        obj = json_node_get_object (n);
        if (obj) {
          n = json_object_get_member (obj, "filename");
          if (n == NULL)
            return FALSE;
        }
      }
      filename = json_node_get_string (n);
      if (filename == NULL)
        return FALSE;

      full_fn = g_strdup_printf ("%s/%s", dir, filename);
      load_file (adaptive, full_fn);
      g_free (full_fn);
    }

    return TRUE;
  }
  return TRUE;
}

GssAdaptive *
gss_adaptive_load (GssServer * server, const char *key, const char *dir,
    const char *version, GssDrmType drm_type, GssAdaptiveStream stream_type)
{
  GssAdaptive *adaptive;
  char *filename;
  gboolean ret;
  GError *error = NULL;
  JsonParser *parser;

  g_return_val_if_fail (GSS_IS_SERVER (server), NULL);
  g_return_val_if_fail (key != NULL, NULL);
  g_return_val_if_fail (dir != NULL, NULL);

  GST_DEBUG ("looking for %s", key);

  parser = json_parser_new ();
  filename = g_strdup_printf ("%s/gss-manifest", dir);
  ret = json_parser_load_from_file (parser, filename, &error);
  if (!ret) {
    GST_DEBUG ("failed to open %s", filename);
    g_free (filename);
    g_object_unref (parser);
    g_error_free (error);
    return NULL;
  }
  g_free (filename);

  GST_DEBUG ("loading %s", key);

  adaptive = gss_adaptive_new ();

  adaptive->server = server;

  adaptive->content_id = g_strdup (key);
  adaptive->kid = create_key_id (key);
  adaptive->kid_len = 16;
  adaptive->drm_type = drm_type;
  adaptive->stream_type = stream_type;

  gss_playready_generate_key (server->playready, adaptive->content_key,
      adaptive->kid, adaptive->kid_len);

  ret = parse_json (adaptive, parser, dir);
  if (!ret) {
    gss_adaptive_free (adaptive);
    g_object_unref (parser);
    GST_WARNING ("json format error in %s/gss-manifest", dir);
    return NULL;
  }

  g_object_unref (parser);

  GST_DEBUG ("loading done");

  return adaptive;
}

static void
generate_iv (GssAdaptiveLevel * level, const char *filename, int track_id)
{
  GChecksum *csum;
  char *s;
  gsize size;
  guint8 bytes[20];

  g_return_if_fail (level != NULL);
  g_return_if_fail (filename != NULL);

  s = g_strdup_printf ("%s:%d", filename, track_id);

  csum = g_checksum_new (G_CHECKSUM_SHA1);
  g_checksum_update (csum, (guchar *) s, -1);
  g_free (s);

  size = 20;
  g_checksum_get_digest (csum, (guchar *) bytes, &size);
  memcpy (&level->iv, bytes, sizeof (level->iv));

  g_checksum_free (csum);
}

static int
estimate_bitrate (GssIsomTrack * track)
{
  guint64 size = 0;
  guint64 duration = 0;
  int i;

  g_return_val_if_fail (track != NULL, 0);
  g_return_val_if_fail (track->n_fragments > 0, 0);
  g_return_val_if_fail (track->mdhd.timescale != 0, 0);

  for (i = 0; i < track->n_fragments; i++) {
    GssIsomFragment *fragment = track->fragments[i];
    size += fragment->moof_size;
    size += fragment->mdat_header_size;
    size += fragment->mdat_size;
    duration += fragment->duration;
  }

  GST_DEBUG ("size %" G_GUINT64_FORMAT " duration %" G_GUINT64_FORMAT,
      size, duration);

  return gst_util_uint64_scale (8 * size, track->mdhd.timescale, duration);
}

void
gss_adaptive_convert_ism (GssAdaptive * adaptive, GssIsomMovie * movie,
    GssIsomTrack * track, GssDrmType drm_type)
{
  int i;
  guint64 offset = 0;
  gboolean is_video;

  is_video = (track->hdlr.handler_type == GST_MAKE_FOURCC ('v', 'i', 'd', 'e'));

  GST_DEBUG ("stsd entries %d", track->stsd.entry_count);

  if (drm_type == GSS_DRM_PLAYREADY) {
    track->is_encrypted = TRUE;
  }
  for (i = 0; i < track->n_fragments; i++) {
    GssIsomFragment *fragment = track->fragments[i];

    fragment->offset = offset;
    gss_isom_fragment_serialize (fragment, &fragment->moof_data,
        &fragment->moof_size, is_video);
    offset += fragment->moof_size;
    offset += fragment->mdat_header_size;
    offset += fragment->mdat_size;
  }
  track->dash_size = offset;

  gss_isom_movie_serialize_track_ccff (movie, track,
      &track->ccff_header_data, &track->ccff_header_size);

  track->dash_size += track->dash_header_and_sidx_size;
}

void
gss_adaptive_convert_isoff_ondemand (GssAdaptive * adaptive,
    GssIsomMovie * movie, GssIsomTrack * track, GssDrmType drm_type)
{
  int i;
  guint64 offset = 0;
  gboolean is_video;

  is_video = (track->hdlr.handler_type == GST_MAKE_FOURCC ('v', 'i', 'd', 'e'));

  GST_DEBUG ("stsd entries %d", track->stsd.entry_count);

  if (drm_type != GSS_DRM_CLEAR) {
    movie->pssh.data = adaptive->drm_info.data;
    movie->pssh.len = adaptive->drm_info.data_len;
    memcpy (movie->pssh.uuid,
        gss_drm_get_drm_uuid (adaptive->drm_info.drm_type), 16);
    movie->pssh.present = TRUE;
  }

  if (drm_type == GSS_DRM_PLAYREADY) {
    track->is_encrypted = TRUE;
  }
  for (i = 0; i < track->n_fragments; i++) {
    GssIsomFragment *fragment = track->fragments[i];

    fragment->offset = offset;
    gss_isom_fragment_serialize (fragment, &fragment->moof_data,
        &fragment->moof_size, is_video);
    offset += fragment->moof_size;
    offset += fragment->mdat_header_size;
    offset += fragment->mdat_size;
  }
  track->dash_size = offset;

  gss_isom_movie_serialize_track_dash (movie, track,
      &track->dash_header_data, &track->dash_header_size,
      &track->dash_header_and_sidx_size);

  track->dash_size += track->dash_header_and_sidx_size;
}


static void
gss_level_from_track (GssAdaptive * adaptive, GssIsomTrack * track,
    GssIsomMovie * movie, const char *filename, gboolean is_video)
{
  GssAdaptiveLevel *level;
  int i;

  g_return_if_fail (adaptive != NULL);
  g_return_if_fail (track != NULL);
  g_return_if_fail (movie != NULL);
  g_return_if_fail (filename != NULL);

  if (is_video) {
    adaptive->video_levels = g_realloc (adaptive->video_levels,
        (adaptive->n_video_levels + 1) * sizeof (GssAdaptiveLevel));
    level = adaptive->video_levels + adaptive->n_video_levels;
    adaptive->n_video_levels++;
  } else {
    adaptive->audio_levels = g_realloc (adaptive->audio_levels,
        (adaptive->n_audio_levels + 1) * sizeof (GssAdaptiveLevel));
    level = adaptive->audio_levels + adaptive->n_audio_levels;
    adaptive->n_audio_levels++;
  }
  memset (level, 0, sizeof (GssAdaptiveLevel));
  level->track = track;

  if (is_video) {
    adaptive->max_width = MAX (adaptive->max_width, track->mp4v.width);
    adaptive->max_height = MAX (adaptive->max_height, track->mp4v.height);
  }

  if (adaptive->drm_type != GSS_DRM_CLEAR) {
    generate_iv (level, filename, track->tkhd.track_id);

    for (i = 0; i < track->n_fragments; i++) {
      GssIsomFragment *fragment = track->fragments[i];
      gss_playready_setup_iv (adaptive->server->playready, adaptive, level,
          fragment);
      /* Hack to prevent serialization of sample encryption UUID and
       * enable saiz/saio serialization */
      if (adaptive->stream_type == GSS_ADAPTIVE_STREAM_ISOFF_ONDEMAND) {
        fragment->sample_encryption.present = FALSE;
        fragment->saiz.present = TRUE;
        fragment->saio.present = TRUE;
        fragment->tfdt.present = TRUE;
      }
    }
  }

  if (adaptive->drm_type == GSS_DRM_PLAYREADY) {
    /* FIXME move */
    if (adaptive->drm_info.data == NULL) {
      adaptive->drm_info.drm_type = GSS_DRM_PLAYREADY;
      adaptive->drm_info.data_len =
          gss_playready_get_protection_header (adaptive,
          adaptive->server->playready->license_url, NULL,
          &adaptive->drm_info.data);
    }
  }

  if (adaptive->stream_type == GSS_ADAPTIVE_STREAM_ISOFF_ONDEMAND) {
    gss_adaptive_convert_isoff_ondemand (adaptive, movie, track,
        adaptive->drm_type);
  } else if (adaptive->stream_type == GSS_ADAPTIVE_STREAM_ISM) {
    gss_adaptive_convert_ism (adaptive, movie, track, adaptive->drm_type);
  }

  level->track_id = track->tkhd.track_id;
  level->n_fragments = track->n_fragments;
  level->filename = g_strdup (filename);
  level->bitrate = estimate_bitrate (track);
  level->video_width = track->mp4v.width;
  level->video_height = track->mp4v.height;
  //level->file = file;

  level->codec_data = gss_hex_encode (track->esds.codec_data,
      track->esds.codec_data_len);
  if (is_video) {
    level->codec = g_strdup_printf ("avc1.%02x%02x%02x",
        track->esds.codec_data[1],
        track->esds.codec_data[2], track->esds.codec_data[3]);
    level->profile = track->esds.codec_data[1];
    level->level = track->esds.codec_data[2];
  } else {
    /* FIXME hard-coded AAC LC */
    level->codec = g_strdup ("mp4a.40.2");
    level->profile = 2;
    level->audio_rate = (track->mp4a.sample_rate >> 16);
  }
}

static void
load_file (GssAdaptive * adaptive, const char *filename)
{
  GssIsomParser *file;
  GssIsomTrack *video_track;
  GssIsomTrack *audio_track;

  g_return_if_fail (adaptive != 0);
  g_return_if_fail (filename != 0);

  file = gss_isom_parser_new ();
  adaptive->parsers[adaptive->n_parsers] = file;
  adaptive->n_parsers++;
  gss_isom_parser_parse_file (file, filename);

  if (file->movie->tracks[0]->n_fragments == 0) {
    gss_isom_parser_fragmentize (file);
  }
#if 0
  if (adaptive->drm_type == GSS_DRM_PLAYREADY &&
      adaptive->stream_type == GSS_ADAPTIVE_STREAM_ISOFF_ONDEMAND) {
    gss_playready_add_protection_header (file->movie, adaptive,
        adaptive->server->playready->license_url, NULL);
  }
#endif

  if (adaptive->duration == 0) {
    adaptive->duration = gss_isom_movie_get_duration (file->movie);
  }

  video_track = gss_isom_movie_get_video_track (file->movie);
  if (video_track) {
    gss_level_from_track (adaptive, video_track, file->movie, filename, TRUE);
  }

  audio_track = gss_isom_movie_get_audio_track (file->movie);
  if (audio_track) {
    gss_level_from_track (adaptive, audio_track, file->movie, filename, FALSE);
#if 0
    GssAdaptiveLevel *level;
    int i;

    adaptive->audio_levels = g_realloc (adaptive->audio_levels,
        (adaptive->n_audio_levels + 1) * sizeof (GssAdaptiveLevel));
    level = adaptive->audio_levels + adaptive->n_audio_levels;
    adaptive->n_audio_levels++;
    memset (level, 0, sizeof (GssAdaptiveLevel));

    generate_iv (level, filename, video_track->tkhd.track_id);

    for (i = 0; i < audio_track->n_fragments; i++) {
      GssIsomFragment *fragment = audio_track->fragments[i];
      gss_playready_setup_iv (adaptive->server->playready, adaptive, level,
          fragment);
    }
    gss_isom_track_prepare_streaming (file->movie, audio_track);

    level->track_id = audio_track->tkhd.track_id;
    level->track = audio_track;
    level->n_fragments =
        gss_isom_parser_get_n_fragments (file, level->track_id);
    level->file = file;
    level->filename = g_strdup (filename);
    level->bitrate = audio_bitrate;
    level->codec_data = gss_hex_encode (audio_track->esds.codec_data,
        audio_track->esds.codec_data_len);
    level->audio_rate = audio_track->mp4a.sample_rate >> 16;
    /* FIXME hard-coded AAC LC */
    level->codec = g_strdup ("mp4a.40.2");
#endif
  }

}

void
gss_adaptive_get_resource (GssTransaction * t, GssAdaptive * adaptive,
    const char *path)
{
  gboolean failed;

  g_return_if_fail (t != NULL);
  g_return_if_fail (adaptive != NULL);
  g_return_if_fail (path != NULL);

  soup_message_headers_replace (t->msg->response_headers,
      "Access-Control-Allow-Origin", "*");

  failed = FALSE;
  switch (adaptive->stream_type) {
    case GSS_ADAPTIVE_STREAM_ISM:
      if (strcmp (path, "Manifest") == 0) {
        gss_adaptive_resource_get_manifest (t, adaptive);
      } else if (strcmp (path, "content") == 0) {
        gss_adaptive_resource_get_content (t, adaptive);
      } else {
        failed = TRUE;
      }
      break;
    case GSS_ADAPTIVE_STREAM_ISOFF_LIVE:
      if (strcmp (path, "manifest.mpd") == 0) {
        gss_adaptive_resource_get_dash_live_mpd (t, adaptive);
      } else if (strcmp (path, "content") == 0) {
        gss_adaptive_resource_get_content (t, adaptive);
      } else {
        failed = TRUE;
      }
      break;
    case GSS_ADAPTIVE_STREAM_ISOFF_ONDEMAND:
      if (strcmp (path, "manifest.mpd") == 0) {
        gss_adaptive_resource_get_dash_range_mpd (t, adaptive);
      } else if (strncmp (path, "content/", 8) == 0) {
        gss_adaptive_resource_get_dash_range_fragment (t, adaptive, path);
      } else {
        failed = TRUE;
      }
      break;
    default:
      failed = TRUE;
  }

  if (failed) {
    gss_transaction_error_not_found (t, "invalid path for stream type");
  }
}

GssAdaptiveStream
gss_adaptive_get_stream_type (const char *s)
{
  g_return_val_if_fail (s != NULL, GSS_ADAPTIVE_STREAM_UNKNOWN);

  if (strcmp (s, "ism") == 0)
    return GSS_ADAPTIVE_STREAM_ISM;
  if (strcmp (s, "isoff-live") == 0)
    return GSS_ADAPTIVE_STREAM_ISOFF_LIVE;
  if (strcmp (s, "isoff-ondemand") == 0)
    return GSS_ADAPTIVE_STREAM_ISOFF_ONDEMAND;
  return GSS_ADAPTIVE_STREAM_UNKNOWN;
}

const char *
gss_adaptive_stream_get_name (GssAdaptiveStream stream_type)
{
  switch (stream_type) {
    case GSS_ADAPTIVE_STREAM_ISM:
      return "ism";
    case GSS_ADAPTIVE_STREAM_ISOFF_LIVE:
      return "isoff-live";
    case GSS_ADAPTIVE_STREAM_ISOFF_ONDEMAND:
      return "isoff-ondemand";
    default:
      return "unknown";
  }
}


static GAsyncQueue *async_queue;
static gboolean async_threads_stop = FALSE;

static gboolean
gss_adaptive_async_finish (gpointer priv)
{
  GssAdaptiveAsync *async = priv;

  async->finish (async);
  gss_adaptive_async_free (async);

  return FALSE;
}

static gpointer
gss_adaptive_async_thread (gpointer unused)
{
  GssAdaptiveAsync *async;

  while (!async_threads_stop) {
    async = g_async_queue_pop (async_queue);

    async->process (async);
    g_idle_add (gss_adaptive_async_finish, async);
  }
  return NULL;
}


void
gss_adaptive_async_init (void)
{
  int i;

  async_queue = g_async_queue_new ();

#define N_THREADS 1
  for (i = 0; i < N_THREADS; i++) {
    g_thread_new ("gss_worker", gss_adaptive_async_thread, NULL);
  }
}

GssAdaptiveAsync *
gss_adaptive_async_new (void)
{
  return g_malloc0 (sizeof (GssAdaptiveAsync));
}

void
gss_adaptive_async_free (GssAdaptiveAsync * async)
{
  g_free (async);
}

void
gss_adaptive_async_push (GssAdaptiveAsync * async)
{
  if (async_queue == NULL) {
    gss_adaptive_async_init ();
  }

  g_async_queue_push (async_queue, async);
}

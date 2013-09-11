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


#ifndef _GSS_SMOOTH_STREAMING_H
#define _GSS_SMOOTH_STREAMING_H

#include "gss-server.h"
#include "gss-isom.h"

G_BEGIN_DECLS

/* 128-bit AES key */
#define GSS_ADAPTIVE_KEY_LENGTH 16

typedef struct _GssAdaptive GssAdaptive;
typedef struct _GssAdaptiveLevel GssAdaptiveLevel;

typedef enum {
  GSS_ADAPTIVE_STREAM_UNKNOWN,
  GSS_ADAPTIVE_STREAM_ISM,
  GSS_ADAPTIVE_STREAM_ISOFF_LIVE,
  GSS_ADAPTIVE_STREAM_ISOFF_ONDEMAND
} GssAdaptiveStream;

typedef enum {
  GSS_DRM_UNKNOWN,
  GSS_DRM_CLEAR,
  GSS_DRM_PLAYREADY,
  GSS_DRM_CENC
} GssDrmType;

struct _GssAdaptive
{
  GssServer *server;
  char *content_id;
  guint64 duration;

  int max_width;
  int max_height;

  int n_audio_levels;
  int n_video_levels;

  GssAdaptiveLevel *audio_levels;
  GssAdaptiveLevel *video_levels;

  guint8 *kid;
  gsize kid_len;
  guint8 content_key[GSS_ADAPTIVE_KEY_LENGTH];

  int n_parsers;
  GssIsomParser *parsers[20];
};

struct _GssAdaptiveLevel
{
  char *filename;

  int n_fragments;
  int bitrate;
  int video_width;
  int video_height;
  int profile;
  int level;

  GssIsomTrack *track;
  int track_id;
  char *codec_data;
  int audio_rate;
  char *codec;

  guint64 iv;
};

GssAdaptive *gss_adaptive_new (void);
void gss_adaptive_free (GssAdaptive * adaptive);
GssAdaptiveLevel *gss_adaptive_get_level (GssAdaptive * adaptive, gboolean video, guint64 bitrate);

GssAdaptiveStream gss_adaptive_get_stream_type (const char *s);
GssAdaptive * gss_adaptive_load (GssServer * server, const char *key, const char *dir);
void gss_adaptive_get_resource (GssTransaction * t, GssAdaptive *adaptive,
    const char *subpath);


G_END_DECLS

#endif


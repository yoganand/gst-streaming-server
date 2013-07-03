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

typedef struct _GssISM GssISM;
typedef struct _GssISMLevel GssISMLevel;

struct _GssISM
{
  guint64 duration;

  int max_width;
  int max_height;

  int n_audio_levels;
  int n_video_levels;

  gboolean playready;

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

  GssIsomFile *file;
  int track_id;
  gboolean is_h264;
  char *codec_data;
  int audio_rate;
};

GssISM *gss_ism_new (void);
void gss_ism_free (GssISM * ism);
GssISMLevel *gss_ism_get_level (GssISM * ism, gboolean video, guint64 bitrate);


void gss_smooth_streaming_setup (GssServer * server);

G_END_DECLS

#endif


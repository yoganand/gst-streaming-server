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

#define HACK_CCFF 1

#include "config.h"

#include <gst/gst.h>
#include <gst/base/gstbytereader.h>
#include <gst/base/gstbytewriter.h>

#include "gss-server.h"
#include "gss-html.h"
#include "gss-session.h"
#include "gss-soup.h"
#include "gss-content.h"
#include "gss-vod.h"
#include "gss-isom.h"
#include "gss-isom-boxes.h"
#include "gss-playready.h"

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <openssl/aes.h>

typedef struct _Container Container;
struct _Container
{
  guint32 atom;
  void (*parse) (GssIsomParser * parser, GssIsomTrack * track,
      GstByteReader * br);
  Container *atoms;
};

/* XMP data */
/* be7acfcb-97a9-42e8-9c71-999491e3afac */
const guint8 uuid_xmp_data[] = { 0xbe, 0x7a, 0xcf, 0xcb, 0x97, 0xa9, 0x42,
  0xe8, 0x9c, 0x71, 0x99, 0x94, 0x91, 0xe3, 0xaf, 0xac
};

/* SampleEncryptionBox */
/* A2394F52-5A9B-4f14-A244-6C427C648DF4 */
const guint8 uuid_sample_encryption[16] = {
  0xa2, 0x39, 0x4f, 0x52, 0x5a, 0x9b, 0x4f, 0x14,
  0xa2, 0x44, 0x6c, 0x42, 0x7c, 0x64, 0x8d, 0xf4
};

/* ProtectionSystemSpecificHeaderBox */
/* 0xd08a4f18-10f3-4a82-b6c8-32d8aba183d3 */
const guint8 uuid_protection_header[16] = {
  0xd0, 0x8a, 0x4f, 0x18, 0x10, 0xf3, 0x4a, 0x82,
  0xb6, 0xc8, 0x32, 0xd8, 0xab, 0xa1, 0x83, 0xd3
};


static gboolean file_read (GssIsomParser * parser, guint8 * buffer,
    guint64 offset, guint64 n_bytes);
static void gss_isom_parse_ftyp (GssIsomParser * parser, guint64 offset,
    guint64 size);
static void gss_isom_parse_moof (GssIsomParser * parser,
    GssIsomFragment * fragment, GstByteReader * br);
static void gss_isom_parse_traf (GssIsomParser * parser,
    GssIsomFragment * fragment, GstByteReader * br);
static void gss_isom_parse_mfhd (GssIsomParser * parser,
    GssIsomFragment * fragment, GstByteReader * br);
static void gss_isom_parse_tfhd (GssIsomParser * parser, GssBoxTfhd * tfhd,
    GstByteReader * br);
static void gss_isom_parse_trun (GssIsomParser * parser, GssBoxTrun * trun,
    GstByteReader * br);
static void gss_isom_parse_sdtp (GssIsomParser * parser, GssBoxSdtp * sdtp,
    GstByteReader * br, int sample_count);
static void gss_isom_parse_mfra (GssIsomParser * parser, guint64 offset,
    guint64 size);
static void gss_isom_parse_sample_encryption (GssIsomParser * parser,
    GssBoxUUIDSampleEncryption * se, GstByteReader * br);
static void gss_isom_parse_avcn (GssIsomParser * parser, GssBoxAvcn * avcn,
    GstByteReader * br);
static void gss_isom_parse_tfdt (GssIsomParser * parser, GssBoxTfdt * tfdt,
    GstByteReader * br);
static void gss_isom_parse_trik (GssIsomParser * parser, GssBoxTrik * trik,
    GstByteReader * br);
static void gss_isom_parse_moov (GssIsomParser * parser, GssIsomMovie * movie,
    GstByteReader * br);
static void gss_isom_fixup_moof (GssIsomFragment * fragment);
static void gss_isom_parser_fixup (GssIsomParser * parser);
static void gss_isom_parse_container (GssIsomParser * parser,
    GssIsomTrack * track, GstByteReader * br, Container * atoms,
    guint32 parent_atom);
static void gss_isom_parse_ignore (GssIsomParser * parser, GssIsomTrack * track,
    GstByteReader * br);

static guint64 gss_isom_moof_get_duration (GssIsomFragment * fragment);


#define CHECK_END(br) do { \
  if ((br)->byte < (br)->size) \
    GST_ERROR("leftover bytes %d < %d", (br)->byte, (br)->size); \
} while(0)
#define CHECK_END_BOX(br, atom) do { \
  if ((br)->byte < (br)->size) \
    GST_ERROR("leftover bytes %d < %d in container %" GST_FOURCC_FORMAT, \
        (br)->byte, (br)->size, GST_FOURCC_ARGS((atom))); \
} while(0)


GssIsomParser *
gss_isom_parser_new (void)
{
  GssIsomParser *parser;

  parser = g_malloc0 (sizeof (GssIsomParser));

  return parser;
}

void
gss_isom_parser_dump (GssIsomParser * parser)
{
  gss_isom_movie_dump (parser->movie);
}

void
gss_isom_parser_free (GssIsomParser * parser)
{
  gss_isom_movie_free (parser->movie);

  g_free (parser->filename);
  g_free (parser->data);
  if (parser->fd > 0) {
    close (parser->fd);
  }
  g_free (parser);
}

GssIsomFragment *
gss_isom_track_get_fragment (GssIsomTrack * track, int index)
{
  return track->fragments[index];
}

GssIsomFragment *
gss_isom_track_get_fragment_by_timestamp (GssIsomTrack * track,
    guint64 timestamp)
{
  int i;

  for (i = 0; i < track->n_fragments; i++) {
    if (track->fragments[i]->timestamp == timestamp) {
      return track->fragments[i];
    }
  }

  return NULL;
}

gboolean
gss_isom_track_is_video (GssIsomTrack * track)
{
  return (track->hdlr.handler_type == GST_MAKE_FOURCC ('v', 'i', 'd', 'e'));
}

guint64
gss_isom_movie_get_duration (GssIsomMovie * movie)
{
  guint64 duration;

  duration = movie->mvhd.duration;

  return duration;
}

int
gss_isom_parser_get_n_fragments (GssIsomParser * parser, int track_id)
{
  GssIsomTrack *track;
  track = gss_isom_movie_get_track_by_id (parser->movie, track_id);
  return track->n_fragments;
}

void
gss_isom_parser_load_chunk (GssIsomParser * parser, guint64 offset,
    guint64 size)
{
  if (parser->data) {
    g_free (parser->data);
  }
  parser->data = g_malloc (size);
  parser->data_offset = offset;
  parser->data_size = size;

  file_read (parser, parser->data, parser->data_offset, parser->data_size);
}

gboolean
gss_isom_parser_parse_file (GssIsomParser * parser, const char *filename)
{
  parser->filename = g_strdup (filename);
  parser->fd = open (filename, O_RDONLY);
  if (parser->fd < 0) {
    GST_ERROR ("cannot open %s", filename);
    return FALSE;
  }

  {
    int ret;
    struct stat sb;

    ret = fstat (parser->fd, &sb);
    if (ret < 0) {
      GST_ERROR ("stat failed");
      close (parser->fd);
      parser->fd = -1;
      return FALSE;
    }
    parser->file_size = sb.st_size;
  }

  parser->offset = 0;
  while (!parser->error && parser->offset < parser->file_size) {
    guint8 buffer[16];
    guint64 size = 0;
    guint32 size32 = 0;
    guint32 atom = 0;
    GstByteReader br;

    file_read (parser, buffer, parser->offset, 16);
    gst_byte_reader_init (&br, buffer, 16);

    gst_byte_reader_get_uint32_be (&br, &size32);
    gst_byte_reader_get_uint32_le (&br, &atom);
    if (size32 == 1) {
      gst_byte_reader_get_uint64_be (&br, &size);
    } else if (size32 == 0) {
      size = parser->file_size - parser->offset;
    } else {
      size = size32;
    }

    if (atom == GST_MAKE_FOURCC ('f', 't', 'y', 'p')) {
      gss_isom_parser_load_chunk (parser, parser->offset, size);

      gss_isom_parse_ftyp (parser, parser->offset, size);
    } else if (atom == GST_MAKE_FOURCC ('m', 'o', 'o', 'f')) {
      GstByteReader br;
      GssIsomFragment *fragment;
      GssIsomTrack *track;

      gss_isom_parser_load_chunk (parser, parser->offset, size);
      gst_byte_reader_init (&br, parser->data + 8, size - 8);

      fragment = gss_isom_fragment_new ();
      gss_isom_parse_moof (parser, fragment, &br);
      gss_isom_fixup_moof (fragment);

      track = gss_isom_movie_get_track_by_id (parser->movie,
          fragment->tfhd.track_id);
      if (track == NULL) {
        GST_ERROR ("track not found for fragment");
      } else {
        if (track->n_fragments == track->n_fragments_alloc) {
          track->n_fragments_alloc += 100;
          track->fragments = g_realloc (track->fragments,
              track->n_fragments_alloc * sizeof (GssIsomFragment *));
        }
        track->fragments[track->n_fragments] = fragment;
        fragment->index = track->n_fragments;
        track->n_fragments++;
      }
      parser->current_fragment = fragment;

      fragment->duration = gss_isom_moof_get_duration (fragment);
      fragment->offset = parser->offset;
      fragment->moof_size = size;
    } else if (atom == GST_MAKE_FOURCC ('m', 'd', 'a', 't')) {
      if (parser->is_isml && parser->current_fragment == NULL) {
        GST_ERROR ("mdat with no moof, broken file");
        parser->error = TRUE;
        close (parser->fd);
        parser->fd = -1;
        return FALSE;
      }

      if (parser->is_isml) {
        parser->current_fragment->mdat_size = size;

        parser->current_fragment->n_mdat_chunks = 1;
        parser->current_fragment->chunks = g_malloc (sizeof (GssMdatChunk) * 1);
        parser->current_fragment->chunks[0].offset = parser->offset + 8;
        parser->current_fragment->chunks[0].size = size - 8;
      }
    } else if (atom == GST_MAKE_FOURCC ('m', 'f', 'r', 'a')) {
      gss_isom_parse_mfra (parser, parser->offset, size);
    } else if (atom == GST_MAKE_FOURCC ('m', 'o', 'o', 'v')) {
      GstByteReader br;
      guint8 *data;
      GssIsomMovie *movie;

      data = g_malloc (size);
      file_read (parser, data, parser->offset, size);
      gst_byte_reader_init (&br, data + 8, size - 8);

      movie = gss_isom_movie_new ();
      gss_isom_parse_moov (parser, movie, &br);

      parser->movie = movie;
      g_free (data);
    } else if (atom == GST_MAKE_FOURCC ('u', 'u', 'i', 'd')) {
      guint8 uuid[16];

      file_read (parser, uuid, parser->offset + 8, 16);

      if (memcmp (uuid, uuid_xmp_data, 16) == 0) {

      } else {
        GST_WARNING ("unknown UUID: %02x%02x%02x%02x-%02x%02x-%02x%02x-"
            "%02x%02x-%02x%02x%02x%02x%02x%02x\n",
            uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6],
            uuid[7], uuid[8], uuid[9], uuid[10], uuid[11], uuid[12], uuid[13],
            uuid[14], uuid[15]);
      }
    } else if (atom == GST_MAKE_FOURCC ('f', 'r', 'e', 'e')) {
      /* ignore */
    } else if (atom == GST_MAKE_FOURCC ('w', 'i', 'd', 'e')) {
      /* ignore */
    } else if (atom == GST_MAKE_FOURCC ('s', 't', 'y', 'p')) {
      GST_ERROR ("styp");
    } else if (atom == GST_MAKE_FOURCC ('s', 'i', 'd', 'x')) {
      GST_ERROR ("sidx");
    } else if (atom == GST_MAKE_FOURCC ('p', 'd', 'i', 'n')) {
      gss_isom_parser_load_chunk (parser, parser->offset, size);

      parser->pdin.present = TRUE;
      parser->pdin.atom = atom;
      parser->pdin.size = size - 8;
      gst_byte_reader_dup_data (&br, size - 8, &parser->pdin.data);
    } else if (atom == GST_MAKE_FOURCC ('b', 'l', 'o', 'c')) {
      gss_isom_parser_load_chunk (parser, parser->offset, size);

      parser->bloc.present = TRUE;
      parser->bloc.atom = atom;
      parser->bloc.size = size - 8;
      gst_byte_reader_dup_data (&br, size - 8, &parser->bloc.data);
    } else {
      GST_WARNING ("unknown atom %" GST_FOURCC_FORMAT
          " at offset %" G_GINT64_MODIFIER "x, size %" G_GUINT64_FORMAT,
          GST_FOURCC_ARGS (atom), parser->offset, size);
    }

    parser->offset += size;
  }

  if (parser->error) {
    GST_ERROR ("file error");
    close (parser->fd);
    parser->fd = -1;
    return FALSE;
  }

  gss_isom_parser_fixup (parser);

  close (parser->fd);
  parser->fd = -1;
  return TRUE;
}

static void
gss_isom_parser_fixup (GssIsomParser * parser)
{
  GssIsomTrack *track;
  guint64 ts;
  int i;
  int j;

  for (j = 0; j < parser->movie->n_tracks; j++) {
    ts = 0;
    track = parser->movie->tracks[j];
    for (i = 0; i < track->n_fragments; i++) {
      track->fragments[i]->timestamp = ts;
      ts += track->fragments[i]->duration;
    }
  }
}

GssIsomTrack *
gss_isom_track_new (void)
{
  GssIsomTrack *track;
  track = g_malloc0 (sizeof (GssIsomTrack));
  return track;
}

void
gss_isom_track_free (GssIsomTrack * track)
{
  int i;
  if (track->fragments) {
    for (i = 0; i < track->n_fragments; i++) {
      gss_isom_fragment_free (track->fragments[i]);
    }
    g_free (track->fragments);
  }
  g_free (track->hdlr.name);
  g_free (track->dref.entries);
  g_free (track->stts.entries);
  g_free (track->ctts.entries);
  g_free (track->esds.codec_data);
  g_free (track->stsd.entries);
  g_free (track->stsz.sample_sizes);
  g_free (track->stco.chunk_offsets);
  g_free (track->stss.sample_numbers);
  g_free (track->stsc.entries);
  g_free (track->stsh.entries);
  g_free (track->esds_store.data);
  g_free (track->ccff_header_data);
  g_free (track);
}

guint64
gss_isom_track_get_n_samples (GssIsomTrack * track)
{
  return track->stsz.sample_count;
}

GssIsomMovie *
gss_isom_movie_new (void)
{
  GssIsomMovie *movie;
  movie = g_malloc0 (sizeof (GssIsomMovie));
  movie->tracks = g_malloc (20 * sizeof (GssIsomTrack *));
  return movie;
}

void
gss_isom_movie_free (GssIsomMovie * movie)
{
  int i;
  for (i = 0; i < movie->n_tracks; i++) {
    gss_isom_track_free (movie->tracks[i]);
  }
  g_free (movie->hdlr.data);
  g_free (movie->ilst.data);
  g_free (movie->tracks);
  g_free (movie);
}

GssIsomTrack *
gss_isom_movie_get_track_by_id (GssIsomMovie * movie, int track_id)
{
  int i;
  for (i = 0; i < movie->n_tracks; i++) {
    if (movie->tracks[i]->tkhd.track_id == track_id) {
      return movie->tracks[i];
    }
  }
  return NULL;
}

GssIsomFragment *
gss_isom_fragment_new (void)
{
  GssIsomFragment *fragment;
  fragment = g_malloc0 (sizeof (GssIsomFragment));
  return fragment;
}

void
gss_isom_fragment_free (GssIsomFragment * fragment)
{
  int i;
  g_free (fragment->trun.samples);
  g_free (fragment->sdtp.sample_flags);
  for (i = 0; i < fragment->sample_encryption.sample_count; i++) {
    g_free (fragment->sample_encryption.samples[i].entries);
  }
  g_free (fragment->sample_encryption.samples);
  g_free (fragment->chunks);
  g_free (fragment);
}


static gboolean
file_read (GssIsomParser * file, guint8 * buffer, guint64 offset,
    guint64 n_bytes)
{
  off_t ret;
  ssize_t n;

  ret = lseek (file->fd, offset, SEEK_SET);
  if (ret < 0) {
    GST_ERROR ("seek to %" G_GUINT64_FORMAT " failed", offset);
    file->error = TRUE;
    return FALSE;
  }

  if (offset + n_bytes > file->file_size) {
    n_bytes = file->file_size - offset;
  }

  n = read (file->fd, buffer, n_bytes);
  if (n != n_bytes) {
    GST_ERROR ("read of %" G_GUINT64_FORMAT " bytes at offset %"
        G_GUINT64_FORMAT " failed: %s", n_bytes, offset, strerror (errno));
    file->error = TRUE;
    return FALSE;
  }

  return TRUE;
}


static void
gss_isom_parse_ftyp (GssIsomParser * file, guint64 offset, guint64 size)
{
  GstByteReader br;
  guint8 *data;
  guint32 atom = 0;
  guint32 tmp = 0;
  gboolean ret;

  data = g_malloc (size);
  ret = file_read (file, data, file->offset, size);
  if (!ret) {
    return;
  }

  gst_byte_reader_init (&br, data + 8, size - 8);

  gst_byte_reader_get_uint32_le (&br, &file->ftyp_atom);
  GST_DEBUG ("ftyp: %" GST_FOURCC_FORMAT, GST_FOURCC_ARGS (file->ftyp_atom));
  if (file->ftyp_atom == GST_MAKE_FOURCC ('i', 's', 'm', 'l')) {
    file->is_isml = TRUE;
  } else if (file->ftyp_atom == GST_MAKE_FOURCC ('m', 'p', '4', '2')) {
    file->is_mp42 = TRUE;
  } else if (file->ftyp_atom == GST_MAKE_FOURCC ('q', 't', ' ', ' ')) {
    //file->is_qt__ = TRUE;
  } else if (file->ftyp_atom == GST_MAKE_FOURCC ('d', 'a', 's', 'h')) {
    //file->is_dash = TRUE;
    file->is_isml = TRUE;
  } else if (file->ftyp_atom == GST_MAKE_FOURCC ('c', 'c', 'f', 'f')) {
  } else {
    GST_ERROR ("not isml, mp4, or qt file: %" GST_FOURCC_FORMAT,
        GST_FOURCC_ARGS (atom));
  }
  gst_byte_reader_get_uint32_le (&br, &tmp);
  while (gst_byte_reader_get_uint32_le (&br, &atom)) {
    GST_DEBUG ("compat: %" GST_FOURCC_FORMAT, GST_FOURCC_ARGS (atom));
    if (atom == GST_MAKE_FOURCC ('i', 's', 'm', 'l')) {
      file->ftyp |= GSS_ISOM_FTYP_ISML;
    } else if (atom == GST_MAKE_FOURCC ('m', 'p', '4', '2')) {
      file->ftyp |= GSS_ISOM_FTYP_MP42;
    } else if (atom == GST_MAKE_FOURCC ('m', 'p', '4', '1')) {
      file->ftyp |= GSS_ISOM_FTYP_MP41;
    } else if (atom == GST_MAKE_FOURCC ('p', 'i', 'f', 'f')) {
      file->ftyp |= GSS_ISOM_FTYP_PIFF;
    } else if (atom == GST_MAKE_FOURCC ('i', 's', 'o', '2')) {
      file->ftyp |= GSS_ISOM_FTYP_ISO2;
    } else if (atom == GST_MAKE_FOURCC ('i', 's', 'o', '6')) {
      file->ftyp |= GSS_ISOM_FTYP_ISO6;
    } else if (atom == GST_MAKE_FOURCC ('i', 's', 'o', 'm')) {
      file->ftyp |= GSS_ISOM_FTYP_ISOM;
    } else if (atom == GST_MAKE_FOURCC ('q', 't', ' ', ' ')) {
      file->ftyp |= GSS_ISOM_FTYP_QT__;
    } else if (atom == 0) {
      GST_DEBUG ("ignoring 0 ftyp");
    } else {
      GST_ERROR ("unknown ftyp compat %" GST_FOURCC_FORMAT,
          GST_FOURCC_ARGS (atom));
    }
  }
  g_free (data);
}

static void
gst_byte_reader_init_sub (GstByteReader * sbr, const GstByteReader * br,
    guint size)
{
  gst_byte_reader_init (sbr, br->data + br->byte, size);
}

static void
gss_isom_parse_moof (GssIsomParser * file, GssIsomFragment * fragment,
    GstByteReader * br)
{
  while (gst_byte_reader_get_remaining (br) >= 8) {
    guint64 size = 0;
    guint32 size32 = 0;
    guint32 atom = 0;
    GstByteReader sbr;

    gst_byte_reader_get_uint32_be (br, &size32);
    gst_byte_reader_get_uint32_le (br, &atom);
    if (size32 == 1) {
      gst_byte_reader_get_uint64_be (br, &size);
    } else {
      size = size32;
    }

    gst_byte_reader_init_sub (&sbr, br, size - 8);
    if (atom == GST_MAKE_FOURCC ('m', 'f', 'h', 'd')) {
      gss_isom_parse_mfhd (file, fragment, &sbr);
    } else if (atom == GST_MAKE_FOURCC ('t', 'r', 'a', 'f')) {
      gss_isom_parse_traf (file, fragment, &sbr);
    } else if (atom == GST_MAKE_FOURCC ('u', 'u', 'i', 'd')) {
      const guint8 *uuid = NULL;

      gst_byte_reader_peek_data (br, 16, &uuid);

      if (memcmp (uuid, uuid_xmp_data, 16) == 0) {

      } else {
        GST_WARNING ("unknown UUID: %02x%02x%02x%02x-%02x%02x-%02x%02x-"
            "%02x%02x-%02x%02x%02x%02x%02x%02x\n",
            uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6],
            uuid[7], uuid[8], uuid[9], uuid[10], uuid[11], uuid[12], uuid[13],
            uuid[14], uuid[15]);
      }
    } else {
      GST_WARNING ("unknown atom %" GST_FOURCC_FORMAT
          " inside moof at offset %" G_GINT64_MODIFIER "x, size %"
          G_GUINT64_FORMAT, GST_FOURCC_ARGS (atom), file->offset, size);
    }

    gst_byte_reader_skip (br, size - 8);
  }
}

static void
gss_isom_parse_traf (GssIsomParser * file, GssIsomFragment * fragment,
    GstByteReader * br)
{
  while (gst_byte_reader_get_remaining (br) >= 8) {
    guint64 size = 0;
    guint32 size32 = 0;
    guint32 atom = 0;
    GstByteReader sbr;

    gst_byte_reader_get_uint32_be (br, &size32);
    gst_byte_reader_get_uint32_le (br, &atom);
    if (size32 == 1) {
      gst_byte_reader_get_uint64_be (br, &size);
    } else {
      size = size32;
    }

    gst_byte_reader_init_sub (&sbr, br, size - 8);
    if (atom == GST_MAKE_FOURCC ('t', 'f', 'h', 'd')) {
      gss_isom_parse_tfhd (file, &fragment->tfhd, &sbr);
    } else if (atom == GST_MAKE_FOURCC ('t', 'r', 'u', 'n')) {
      gss_isom_parse_trun (file, &fragment->trun, &sbr);
    } else if (atom == GST_MAKE_FOURCC ('s', 'd', 't', 'p')) {
      gss_isom_parse_sdtp (file, &fragment->sdtp, &sbr,
          fragment->trun.sample_count);
    } else if (atom == GST_MAKE_FOURCC ('a', 'v', 'c', 'n')) {
      gss_isom_parse_avcn (file, &fragment->avcn, &sbr);
    } else if (atom == GST_MAKE_FOURCC ('t', 'f', 'd', 't')) {
      gss_isom_parse_tfdt (file, &fragment->tfdt, &sbr);
    } else if (atom == GST_MAKE_FOURCC ('t', 'r', 'i', 'k')) {
      gss_isom_parse_trik (file, &fragment->trik, &sbr);
    } else if (atom == GST_MAKE_FOURCC ('u', 'u', 'i', 'd')) {
      const guint8 *uuid = NULL;

      gst_byte_reader_get_data (&sbr, 16, &uuid);

      if (memcmp (uuid, uuid_sample_encryption, 16) == 0) {
        gss_isom_parse_sample_encryption (file, &fragment->sample_encryption,
            &sbr);
      } else {
        GST_WARNING ("unknown UUID: %02x%02x%02x%02x-%02x%02x-%02x%02x-"
            "%02x%02x-%02x%02x%02x%02x%02x%02x\n",
            uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6],
            uuid[7], uuid[8], uuid[9], uuid[10], uuid[11], uuid[12], uuid[13],
            uuid[14], uuid[15]);
      }
    } else {
      GST_WARNING ("unknown atom %" GST_FOURCC_FORMAT
          " inside traf at offset %" G_GINT64_MODIFIER "x, size %"
          G_GUINT64_FORMAT, GST_FOURCC_ARGS (atom), file->offset, size);
    }

    gst_byte_reader_skip (br, size - 8);
  }
}

static void
gss_isom_parse_mfhd (GssIsomParser * file, GssIsomFragment * fragment,
    GstByteReader * br)
{
  GssBoxMfhd *mfhd = &fragment->mfhd;

  gst_byte_reader_get_uint8 (br, &mfhd->version);
  gst_byte_reader_get_uint24_be (br, &mfhd->flags);
  gst_byte_reader_get_uint32_be (br, &mfhd->sequence_number);
}

static void
gss_isom_parse_tfhd (GssIsomParser * file, GssBoxTfhd * tfhd,
    GstByteReader * br)
{

  gst_byte_reader_get_uint8 (br, &tfhd->version);
  gst_byte_reader_get_uint24_be (br, &tfhd->flags);
  gst_byte_reader_get_uint32_be (br, &tfhd->track_id);
  if (tfhd->flags & TF_SAMPLE_DESCRIPTION_INDEX) {
    gst_byte_reader_skip (br, 4);
  }
  if (tfhd->flags & TF_DEFAULT_SAMPLE_DURATION) {
    gst_byte_reader_get_uint32_be (br, &tfhd->default_sample_duration);
  }
  if (tfhd->flags & TF_DEFAULT_SAMPLE_SIZE) {
    gst_byte_reader_get_uint32_be (br, &tfhd->default_sample_size);
  }
  if (tfhd->flags & TF_DEFAULT_SAMPLE_FLAGS) {
    gst_byte_reader_get_uint32_be (br, &tfhd->default_sample_flags);
  }

}

static void
gss_isom_parse_trun (GssIsomParser * file, GssBoxTrun * trun,
    GstByteReader * br)
{
  int i;

  gst_byte_reader_get_uint8 (br, &trun->version);
  gst_byte_reader_get_uint24_be (br, &trun->flags);
  gst_byte_reader_get_uint32_be (br, &trun->sample_count);
  if (trun->flags & TR_DATA_OFFSET) {
    gst_byte_reader_get_uint32_be (br, &trun->data_offset);
  }
  if (trun->flags & TR_FIRST_SAMPLE_FLAGS) {
    gst_byte_reader_get_uint32_be (br, &trun->first_sample_flags);
  }

  trun->samples = g_malloc0 (sizeof (GssBoxTrunSample) * trun->sample_count);
  for (i = 0; i < trun->sample_count; i++) {
    if (trun->flags & TR_SAMPLE_DURATION) {
      gst_byte_reader_get_uint32_be (br, &trun->samples[i].duration);
    }
    if (trun->flags & TR_SAMPLE_SIZE) {
      gst_byte_reader_get_uint32_be (br, &trun->samples[i].size);
    }
    if (trun->flags & TR_SAMPLE_FLAGS) {
      gst_byte_reader_get_uint32_be (br, &trun->samples[i].flags);
    }
    if (trun->flags & TR_SAMPLE_COMPOSITION_TIME_OFFSETS) {
      gst_byte_reader_get_uint32_be (br,
          &trun->samples[i].composition_time_offset);
    }
  }
}

static void
gss_isom_fixup_moof (GssIsomFragment * fragment)
{
  GssBoxTfhd *tfhd = &fragment->tfhd;
  GssBoxTrun *trun = &fragment->trun;
  int i;

  if (!(trun->flags & TR_SAMPLE_DURATION)) {
    for (i = 0; i < trun->sample_count; i++) {
      trun->samples[i].duration = tfhd->default_sample_duration;
    }
  }

  if (!(trun->flags & TR_SAMPLE_FLAGS)) {
    for (i = 0; i < trun->sample_count; i++) {
      trun->samples[i].flags = tfhd->default_sample_duration;
    }
  }
}

static guint64
gss_isom_moof_get_duration (GssIsomFragment * fragment)
{
  guint64 duration = 0;
  int i;

  for (i = 0; i < fragment->trun.sample_count; i++) {
    duration += fragment->trun.samples[i].duration;
  }
  return duration;
}

static void
gss_isom_parse_sdtp (GssIsomParser * file, GssBoxSdtp * sdtp,
    GstByteReader * br, int sample_count)
{
  int i;

  gst_byte_reader_get_uint8 (br, &sdtp->version);
  gst_byte_reader_get_uint24_be (br, &sdtp->flags);

  sdtp->sample_flags = g_malloc (sizeof (guint8) * sample_count);
  for (i = 0; i < sample_count; i++) {
    gst_byte_reader_get_uint8 (br, &sdtp->sample_flags[i]);
  }

}

static void
gss_isom_parse_sample_encryption (GssIsomParser * file,
    GssBoxUUIDSampleEncryption * se, GstByteReader * br)
{
  int i;
  int j;

  se->present = TRUE;
  gst_byte_reader_get_uint8 (br, &se->version);
  gst_byte_reader_get_uint24_be (br, &se->flags);
  GST_DEBUG ("flags %08x", se->flags);
  if (se->flags & 0x0001) {
    gst_byte_reader_get_uint24_be (br, &se->algorithm_id);
    gst_byte_reader_get_uint8 (br, &se->iv_size);
    GST_DEBUG ("iv size %d", se->iv_size);
    gst_byte_reader_skip (br, 16);
  }
  gst_byte_reader_get_uint32_be (br, &se->sample_count);
  se->samples = g_malloc0 (se->sample_count *
      sizeof (GssBoxUUIDSampleEncryptionSample));
  for (i = 0; i < se->sample_count; i++) {
    gst_byte_reader_get_uint64_be (br, &se->samples[i].iv);
    if (se->flags & 0x0002) {
      gst_byte_reader_get_uint16_be (br, &se->samples[i].num_entries);
      GST_DEBUG ("n_entries %d", se->samples[i].num_entries);
      se->samples[i].entries = g_malloc0 (se->samples[i].num_entries *
          sizeof (GssBoxUUIDSampleEncryptionSampleEntry));
      for (j = 0; j < se->samples[i].num_entries; j++) {
        gst_byte_reader_get_uint16_be (br,
            &se->samples[i].entries[j].bytes_of_clear_data);
        gst_byte_reader_get_uint32_be (br,
            &se->samples[i].entries[j].bytes_of_encrypted_data);
        GST_DEBUG ("clear %d enc %d",
            se->samples[i].entries[j].bytes_of_clear_data,
            se->samples[i].entries[j].bytes_of_encrypted_data);
      }
    }
  }


}

static void
gss_isom_parse_avcn (GssIsomParser * file, GssBoxAvcn * avcn,
    GstByteReader * br)
{

  gst_byte_reader_get_uint8 (br, &avcn->version);
  gst_byte_reader_get_uint24_be (br, &avcn->flags);

  /* FIXME */

  CHECK_END (br);
}

static void
gss_isom_parse_tfdt (GssIsomParser * file, GssBoxTfdt * tfdt,
    GstByteReader * br)
{
  guint32 tmp;

  gst_byte_reader_get_uint8 (br, &tfdt->version);
  gst_byte_reader_get_uint24_be (br, &tfdt->flags);

  /* FIXME */
  gst_byte_reader_get_uint32_be (br, &tmp);

  CHECK_END (br);
}

static void
gss_isom_parse_trik (GssIsomParser * file, GssBoxTrik * trik,
    GstByteReader * br)
{

  gst_byte_reader_get_uint8 (br, &trik->version);
  gst_byte_reader_get_uint24_be (br, &trik->flags);

  /* FIXME */

  CHECK_END (br);
}

static void
gss_isom_parse_mfra (GssIsomParser * file, guint64 offset, guint64 size)
{
  /* ignored for now */
  GST_DEBUG ("FIXME parse mfra atom");
}

void
gss_isom_fragment_set_sample_encryption (GssIsomFragment * fragment,
    int n_samples, guint64 * init_vectors, gboolean is_video)
{
  GssBoxUUIDSampleEncryption *se = &fragment->sample_encryption;
  GssBoxTrun *trun = &fragment->trun;
  int i;

  se->present = TRUE;
  se->flags = 0;
  se->sample_count = n_samples;
  se->samples =
      g_malloc0 (sizeof (GssBoxUUIDSampleEncryptionSample) * n_samples);
  for (i = 0; i < n_samples; i++) {
    se->samples[i].iv = init_vectors[i];
  }

  if (is_video) {
    /* This actually is for just H.264, not all video */
    se->flags |= 0x0002;
    for (i = 0; i < n_samples; i++) {
      se->samples[i].num_entries = 1;
      se->samples[i].entries = g_malloc0 (se->samples[i].num_entries *
          sizeof (GssBoxUUIDSampleEncryptionSampleEntry));
      se->samples[i].entries[0].bytes_of_clear_data = 5;
      se->samples[i].entries[0].bytes_of_encrypted_data =
          trun->samples[i].size - 5;
    }

  }
}

static void
gss_isom_parse_mvhd (GssIsomParser * file, GssBoxMvhd * mvhd,
    GstByteReader * br)
{
  int i;
  guint32 tmp = 0;
  guint16 tmp16 = 0;

  gst_byte_reader_get_uint8 (br, &mvhd->version);
  gst_byte_reader_get_uint24_be (br, &mvhd->flags);
  if (mvhd->version == 1) {
    gst_byte_reader_get_uint64_be (br, &mvhd->creation_time);
    gst_byte_reader_get_uint64_be (br, &mvhd->modification_time);
    gst_byte_reader_get_uint32_be (br, &mvhd->timescale);
    gst_byte_reader_get_uint64_be (br, &mvhd->duration);
  } else {
    gst_byte_reader_get_uint32_be (br, &tmp);
    mvhd->creation_time = tmp;
    gst_byte_reader_get_uint32_be (br, &tmp);
    mvhd->modification_time = tmp;
    gst_byte_reader_get_uint32_be (br, &mvhd->timescale);
    gst_byte_reader_get_uint32_be (br, &tmp);
    tmp = mvhd->duration;
  }

  gst_byte_reader_get_uint32_be (br, &tmp);
  //g_assert (tmp == 0x00010000);
  gst_byte_reader_get_uint16_be (br, &tmp16);
  //g_assert (tmp16 == 0x0100);
  gst_byte_reader_get_uint16_be (br, &tmp16);
  // tmp16 reserved should be 0
  for (i = 0; i < 9; i++) {
    gst_byte_reader_get_uint32_be (br, &tmp);
    // pre-defined
  }
  for (i = 0; i < 6; i++) {
    gst_byte_reader_get_uint32_be (br, &tmp);
    // reserved (should be 0)
  }

  gst_byte_reader_get_uint32_be (br, &mvhd->next_track_id);
}

static void
gss_isom_parse_tkhd (GssIsomParser * file, GssIsomTrack * track,
    GstByteReader * br)
{
  GssBoxTkhd *tkhd = &track->tkhd;
  guint32 tmp = 0;
  guint16 tmp16 = 0;
  int i;

  tkhd->present = TRUE;
  gst_byte_reader_get_uint8 (br, &tkhd->version);
  gst_byte_reader_get_uint24_be (br, &tkhd->flags);
  if (tkhd->version == 1) {
    gst_byte_reader_get_uint64_be (br, &tkhd->creation_time);
    gst_byte_reader_get_uint64_be (br, &tkhd->modification_time);
    gst_byte_reader_get_uint32_be (br, &tkhd->track_id);
    gst_byte_reader_get_uint32_be (br, &tmp);
    gst_byte_reader_get_uint64_be (br, &tkhd->duration);
  } else {
    gst_byte_reader_get_uint32_be (br, &tmp);
    tkhd->creation_time = tmp;
    gst_byte_reader_get_uint32_be (br, &tmp);
    tkhd->modification_time = tmp;
    gst_byte_reader_get_uint32_be (br, &tkhd->track_id);
    gst_byte_reader_get_uint32_be (br, &tmp);
    gst_byte_reader_get_uint32_be (br, &tmp);
    tkhd->duration = tmp;
  }
  gst_byte_reader_get_uint32_be (br, &tmp);
  gst_byte_reader_get_uint32_be (br, &tmp);
  gst_byte_reader_get_uint16_be (br, &tkhd->layer);
  gst_byte_reader_get_uint16_be (br, &tkhd->alternate_group);
  gst_byte_reader_get_uint16_be (br, &tkhd->volume);
  gst_byte_reader_get_uint16_be (br, &tmp16);
  for (i = 0; i < 9; i++) {
    gst_byte_reader_get_uint32_be (br, &tkhd->matrix[i]);
  }
  gst_byte_reader_get_uint32_be (br, &tkhd->width);
  gst_byte_reader_get_uint32_be (br, &tkhd->height);

  CHECK_END (br);
}

static void
gss_isom_parse_tref (GssIsomParser * file, GssIsomTrack * track,
    GstByteReader * br)
{
  GssBoxTref *tref = &track->tref;
  tref->present = TRUE;

  GST_FIXME ("parse tref");

  //CHECK_END (br);
}

static void
gss_isom_parse_elst (GssIsomParser * file, GssIsomTrack * track,
    GstByteReader * br)
{
  GssBoxElst *elst = &track->elst;
  int i;

  elst->present = TRUE;
  gst_byte_reader_get_uint8 (br, &elst->version);
  gst_byte_reader_get_uint24_be (br, &elst->flags);

  gst_byte_reader_get_uint32_be (br, &elst->entry_count);
  elst->entries = g_malloc0 (elst->entry_count * sizeof (GssBoxElstEntry));
  for (i = 0; i < elst->entry_count; i++) {
    if (elst->version == 0) {
      guint32 tmp = 0;
      gst_byte_reader_get_uint32_be (br, &tmp);
      elst->entries[i].segment_duration = tmp;
      gst_byte_reader_get_uint32_be (br, &tmp);
      elst->entries[i].media_time = tmp;
    } else {
      gst_byte_reader_get_uint64_be (br, &elst->entries[i].segment_duration);
      gst_byte_reader_get_uint64_be (br, &elst->entries[i].media_time);
    }
    gst_byte_reader_get_uint32_be (br, &elst->entries[i].media_rate);

  }

  GST_FIXME ("parse elst");

  //CHECK_END (br);
}

static guint32
pack_language_code (char *language)
{
  guint32 code;
  code = ((language[0] - 0x60) & 0x1f) << 10;
  code |= ((language[1] - 0x60) & 0x1f) << 5;
  code |= ((language[2] - 0x60) & 0x1f);
  return code;
}

static void
unpack_language_code (char *language, guint16 code)
{
  language[0] = 0x60 + ((code >> 10) & 0x1f);
  language[1] = 0x60 + ((code >> 5) & 0x1f);
  language[2] = 0x60 + (code & 0x1f);
  language[3] = 0;
}

static void
gss_isom_parse_mdhd (GssIsomParser * file, GssIsomTrack * track,
    GstByteReader * br)
{
  GssBoxMdhd *mdhd = &track->mdhd;
  guint32 tmp = 0;
  guint16 tmp16 = 0;

  mdhd->present = TRUE;
  gst_byte_reader_get_uint8 (br, &mdhd->version);
  gst_byte_reader_get_uint24_be (br, &mdhd->flags);
  if (mdhd->version == 1) {
    gst_byte_reader_get_uint64_be (br, &mdhd->creation_time);
    gst_byte_reader_get_uint64_be (br, &mdhd->modification_time);
    gst_byte_reader_get_uint32_be (br, &mdhd->timescale);
    gst_byte_reader_get_uint64_be (br, &mdhd->duration);
  } else {
    gst_byte_reader_get_uint32_be (br, &tmp);
    mdhd->creation_time = tmp;
    gst_byte_reader_get_uint32_be (br, &tmp);
    mdhd->modification_time = tmp;
    gst_byte_reader_get_uint32_be (br, &mdhd->timescale);
    gst_byte_reader_get_uint32_be (br, &tmp);
    mdhd->duration = tmp;
  }
  gst_byte_reader_get_uint16_be (br, &tmp16);
  unpack_language_code (mdhd->language_code, tmp16);
  gst_byte_reader_get_uint16_be (br, &tmp16);

  CHECK_END (br);
}

void
gst_byte_reader_dump (GstByteReader * br)
{
  guint8 data;
  while (gst_byte_reader_get_uint8 (br, &data)) {
    GST_ERROR ("data %02x %c", data, g_ascii_isprint (data) ? data : '.');
  }
}

gboolean
get_string (GssIsomParser * file, GstByteReader * br, gchar ** string)
{
  gboolean ret;
  guint8 len = 0;
  gboolean nul_terminated;

  if (file->ftyp & (GSS_ISOM_FTYP_MP41 | GSS_ISOM_FTYP_MP42 |
          GSS_ISOM_FTYP_PIFF | GSS_ISOM_FTYP_ISO6)) {
    nul_terminated = TRUE;
  } else {
    nul_terminated = FALSE;
  }

  gst_byte_reader_peek_uint8 (br, &len);
  if (gst_byte_reader_get_remaining (br) == len + 1) {
    const guint8 *s;

    if (nul_terminated) {
      GST_WARNING ("expecting nul-terminated string, got pascal string "
          "(ftyp %08x}", file->ftyp);
    }

    gst_byte_reader_get_uint8 (br, &len);
    ret = gst_byte_reader_get_data (br, len, &s);
    if (ret) {
      *string = g_malloc (len + 1);
      memcpy (*string, s, len);
      (*string)[len] = 0;
    }
  } else {
    if (!nul_terminated) {
      GST_WARNING ("expecting pascal string, got nul-terminated string "
          "(ftyp %08x)", file->ftyp);
    }

    ret = gst_byte_reader_dup_string (br, string);
  }

  return ret;
}

static void
put_string (GstByteWriter * bw, gchar * s, gboolean nul_terminated)
{
  if (nul_terminated) {
    gst_byte_writer_put_string (bw, s);
  } else {
    gst_byte_writer_put_uint8 (bw, strlen (s));
    gst_byte_writer_put_data (bw, (guchar *) s, strlen (s));
  }
}

static void
gss_isom_parse_hdlr (GssIsomParser * file, GssIsomTrack * track,
    GstByteReader * br)
{
  GssBoxHdlr *hdlr = &track->hdlr;
  guint32 tmp = 0;

  hdlr->present = TRUE;
  gst_byte_reader_get_uint8 (br, &hdlr->version);
  gst_byte_reader_get_uint24_be (br, &hdlr->flags);
  gst_byte_reader_get_uint32_be (br, &tmp);
  gst_byte_reader_get_uint32_le (br, &hdlr->handler_type);
  gst_byte_reader_skip (br, 12);
  get_string (file, br, &hdlr->name);

  gst_byte_reader_dump (br);

  CHECK_END (br);
}

static void
gss_isom_parse_vmhd (GssIsomParser * file, GssIsomTrack * track,
    GstByteReader * br)
{
  GssBoxVmhd *vmhd = &track->vmhd;
  vmhd->present = TRUE;
  gst_byte_reader_get_uint8 (br, &vmhd->version);
  gst_byte_reader_get_uint24_be (br, &vmhd->flags);
  gst_byte_reader_skip (br, 8);

  CHECK_END (br);
}

static void
gss_isom_parse_smhd (GssIsomParser * file, GssIsomTrack * track,
    GstByteReader * br)
{
  GssBoxSmhd *smhd = &track->smhd;
  smhd->present = TRUE;
  gst_byte_reader_get_uint8 (br, &smhd->version);
  gst_byte_reader_get_uint24_be (br, &smhd->flags);
  gst_byte_reader_skip (br, 4);

  CHECK_END (br);
}

static void
gss_isom_parse_hmhd (GssIsomParser * file, GssIsomTrack * track,
    GstByteReader * br)
{
  GssBoxHmhd *hmhd = &track->hmhd;
  guint32 tmp;

  hmhd->present = TRUE;
  gst_byte_reader_get_uint8 (br, &hmhd->version);
  gst_byte_reader_get_uint24_be (br, &hmhd->flags);
  gst_byte_reader_get_uint16_be (br, &hmhd->maxPDUsize);
  gst_byte_reader_get_uint16_be (br, &hmhd->avgPDUsize);
  gst_byte_reader_get_uint32_be (br, &hmhd->maxbitrate);
  gst_byte_reader_get_uint32_be (br, &hmhd->avgbitrate);
  gst_byte_reader_get_uint32_be (br, &tmp);

  CHECK_END (br);
}

static void
gss_isom_parse_dref (GssIsomParser * file, GssIsomTrack * track,
    GstByteReader * br)
{
  GssBoxDref *dref = &track->dref;
  int i;
  dref->present = TRUE;
  gst_byte_reader_get_uint8 (br, &dref->version);
  gst_byte_reader_get_uint24_be (br, &dref->flags);
  gst_byte_reader_get_uint32_be (br, &dref->entry_count);
  dref->entries = g_malloc0 (sizeof (GssBoxDrefEntry) * dref->entry_count);
  for (i = 0; i < dref->entry_count; i++) {
    guint64 size = 0;
    guint32 size32 = 0;
    guint32 atom = 0;
    GstByteReader sbr;

    gst_byte_reader_get_uint32_be (br, &size32);
    gst_byte_reader_get_uint32_le (br, &atom);
    dref->entries[0].atom = atom;
    if (size32 == 1) {
      gst_byte_reader_get_uint64_be (br, &size);
    } else {
      size = size32;
    }

    gst_byte_reader_init_sub (&sbr, br, size - 8);
    if (atom == GST_MAKE_FOURCC ('u', 'r', 'l', ' ')) {
      GST_FIXME ("url_");
    } else if (atom == GST_MAKE_FOURCC ('u', 'r', 'n', ' ')) {
      GST_FIXME ("urn_");
    } else if (atom == GST_MAKE_FOURCC ('a', 'l', 'i', 's')) {
      GST_FIXME ("alis");
    } else if (atom == GST_MAKE_FOURCC ('c', 'i', 'o', 's')) {
      GST_FIXME ("cios");
    } else {
      GST_WARNING ("unknown atom %" GST_FOURCC_FORMAT " inside dref, size %"
          G_GUINT64_FORMAT, GST_FOURCC_ARGS (atom), size);
    }

    gst_byte_reader_skip (br, size - 8);
  }

  gst_byte_reader_dump (br);

  CHECK_END (br);
}

static void
gss_isom_parse_stts (GssIsomParser * file, GssIsomTrack * track,
    GstByteReader * br)
{
  GssBoxStts *stts = &track->stts;
  int i;
  stts->present = TRUE;
  gst_byte_reader_get_uint8 (br, &stts->version);
  gst_byte_reader_get_uint24_be (br, &stts->flags);
  gst_byte_reader_get_uint32_be (br, &stts->entry_count);
  stts->entries = g_malloc0 (sizeof (GssBoxSttsEntry) * stts->entry_count);
  for (i = 0; i < stts->entry_count; i++) {
    gst_byte_reader_get_uint32_be (br, &stts->entries[i].sample_count);
    gst_byte_reader_get_int32_be (br, &stts->entries[i].sample_delta);
  }

  CHECK_END (br);
}

static void
gss_isom_parse_ctts (GssIsomParser * file, GssIsomTrack * track,
    GstByteReader * br)
{
  GssBoxCtts *ctts = &track->ctts;
  int i;
  ctts->present = TRUE;
  gst_byte_reader_get_uint8 (br, &ctts->version);
  gst_byte_reader_get_uint24_be (br, &ctts->flags);
  gst_byte_reader_get_uint32_be (br, &ctts->entry_count);
  ctts->entries = g_malloc0 (sizeof (GssBoxCttsEntry) * ctts->entry_count);
  for (i = 0; i < ctts->entry_count; i++) {
    gst_byte_reader_get_uint32_be (br, &ctts->entries[i].sample_count);
    gst_byte_reader_get_uint32_be (br, &ctts->entries[i].sample_offset);
  }

  CHECK_END (br);
}

static void
gss_isom_parse_stsz (GssIsomParser * file, GssIsomTrack * track,
    GstByteReader * br)
{
  GssBoxStsz *stsz = &track->stsz;
  int i;
  stsz->present = TRUE;
  gst_byte_reader_get_uint8 (br, &stsz->version);
  gst_byte_reader_get_uint24_be (br, &stsz->flags);
  gst_byte_reader_get_uint32_be (br, &stsz->sample_size);
  gst_byte_reader_get_uint32_be (br, &stsz->sample_count);
  if (stsz->sample_size == 0) {
    stsz->sample_sizes = g_malloc0 (sizeof (guint32) * stsz->sample_count);
    for (i = 0; i < stsz->sample_count; i++) {
      gst_byte_reader_get_uint32_be (br, &stsz->sample_sizes[i]);
    }
  }

  CHECK_END (br);
}

static void
gss_isom_parse_stsc (GssIsomParser * file, GssIsomTrack * track,
    GstByteReader * br)
{
  GssBoxStsc *stsc = &track->stsc;
  int i;
  stsc->present = TRUE;
  gst_byte_reader_get_uint8 (br, &stsc->version);
  gst_byte_reader_get_uint24_be (br, &stsc->flags);
  gst_byte_reader_get_uint32_be (br, &stsc->entry_count);
  stsc->entries = g_malloc0 (sizeof (GssBoxStscEntry) * stsc->entry_count);
  for (i = 0; i < stsc->entry_count; i++) {
    gst_byte_reader_get_uint32_be (br, &stsc->entries[i].first_chunk);
    gst_byte_reader_get_uint32_be (br, &stsc->entries[i].samples_per_chunk);
    gst_byte_reader_get_uint32_be (br,
        &stsc->entries[i].sample_description_index);
  }

  CHECK_END (br);
}

static void
gss_isom_parse_stco (GssIsomParser * file, GssIsomTrack * track,
    GstByteReader * br)
{
  GssBoxStco *stco = &track->stco;
  guint32 tmp = 0;
  int i;
  stco->present = TRUE;
  gst_byte_reader_get_uint8 (br, &stco->version);
  gst_byte_reader_get_uint24_be (br, &stco->flags);
  gst_byte_reader_get_uint32_be (br, &stco->entry_count);
  stco->chunk_offsets = g_malloc0 (sizeof (guint64) * stco->entry_count);
  for (i = 0; i < stco->entry_count; i++) {
    gst_byte_reader_get_uint32_be (br, &tmp);
    stco->chunk_offsets[i] = tmp;
  }

  CHECK_END (br);
}

static void
gss_isom_parse_co64 (GssIsomParser * file, GssIsomTrack * track,
    GstByteReader * br)
{
  GssBoxStco *stco = &track->stco;
  int i;
  stco->present = TRUE;
  gst_byte_reader_get_uint8 (br, &stco->version);
  gst_byte_reader_get_uint24_be (br, &stco->flags);
  gst_byte_reader_get_uint32_be (br, &stco->entry_count);
  stco->chunk_offsets = g_malloc0 (sizeof (guint64) * stco->entry_count);
  for (i = 0; i < stco->entry_count; i++) {
    gst_byte_reader_get_uint64_be (br, &stco->chunk_offsets[i]);
  }

  CHECK_END (br);
}

static void
gss_isom_parse_stss (GssIsomParser * file, GssIsomTrack * track,
    GstByteReader * br)
{
  GssBoxStss *stss = &track->stss;
  int i;
  stss->present = TRUE;
  gst_byte_reader_get_uint8 (br, &stss->version);
  gst_byte_reader_get_uint24_be (br, &stss->flags);
  gst_byte_reader_get_uint32_be (br, &stss->entry_count);
  stss->sample_numbers = g_malloc0 (sizeof (guint32) * stss->entry_count);
  for (i = 0; i < stss->entry_count; i++) {
    gst_byte_reader_get_uint32_be (br, &stss->sample_numbers[i]);
  }

  CHECK_END (br);
}

static void
gss_isom_parse_stsh (GssIsomParser * file, GssIsomTrack * track,
    GstByteReader * br)
{
  GssBoxStsh *stsh = &track->stsh;
  int i;
  stsh->present = TRUE;
  gst_byte_reader_get_uint8 (br, &stsh->version);
  gst_byte_reader_get_uint24_be (br, &stsh->flags);
  gst_byte_reader_get_uint32_be (br, &stsh->entry_count);
  stsh->entries = g_malloc0 (sizeof (GssBoxStshEntry) * stsh->entry_count);
  for (i = 0; i < stsh->entry_count; i++) {
    gst_byte_reader_get_uint32_be (br,
        &stsh->entries[i].shadowed_sample_number);
    gst_byte_reader_get_uint32_be (br, &stsh->entries[i].sync_sample_number);
  }

  CHECK_END (br);
}

static void
gss_isom_parse_stdp (GssIsomParser * file, GssIsomTrack * track,
    GstByteReader * br)
{
  GssBoxStdp *stdp = &track->stdp;
  int i;
  stdp->present = TRUE;
  gst_byte_reader_get_uint8 (br, &stdp->version);
  gst_byte_reader_get_uint24_be (br, &stdp->flags);
  stdp->priorities =
      g_malloc0 (sizeof (GssBoxStshEntry) * track->stsz.sample_count);
  for (i = 0; i < track->stsz.sample_count; i++) {
    gst_byte_reader_get_uint16_be (br, &stdp->priorities[i]);
  }

  CHECK_END (br);
}

static void
gss_isom_parse_esds (GssIsomParser * file, GssIsomTrack * track,
    GstByteReader * br)
{
  GssBoxEsds *esds = &track->esds;
  guint32 tmp = 0;

  {
    const guint8 *ptr = NULL;
    int size = gst_byte_reader_get_remaining (br);
    gst_byte_reader_peek_data (br, size, &ptr);
    track->esds_store.size = size;
    track->esds_store.data = g_memdup (ptr, size);
  }
  gst_byte_reader_get_uint32_be (br, &tmp);

  while (gst_byte_reader_get_remaining (br) > 0) {
    guint8 tag = 0;
    guint32 len = 0;
    guint8 tmp = 0;

    gst_byte_reader_get_uint8 (br, &tag);
    do {
      gst_byte_reader_get_uint8 (br, &tmp);
      len <<= 7;
      len |= (tmp & 0x7f);
    } while (tmp & 0x80);

    GST_DEBUG ("tag %d len %d", tag, len);
    switch (tag) {
      case 0x03:               /* ES_DescrTag */
        gst_byte_reader_get_uint16_be (br, &esds->es_id);
        gst_byte_reader_get_uint8 (br, &esds->es_flags);
        if (esds->es_flags & 0x80) {
          gst_byte_reader_skip (br, 2);
        }
        if (esds->es_flags & 0x40) {
          gst_byte_reader_skip (br, 2);
        }
        if (esds->es_flags & 0x20) {
          gst_byte_reader_skip (br, 2);
        }
        break;
      case 0x04:               /* DecoderConfigDescrTag */
        gst_byte_reader_get_uint8 (br, &esds->type_indication);
        GST_DEBUG ("type_indication %d", esds->type_indication);
        gst_byte_reader_get_uint8 (br, &esds->stream_type);
        GST_DEBUG ("stream_type %d", esds->stream_type);
        gst_byte_reader_get_uint24_be (br, &esds->buffer_size_db);
        GST_DEBUG ("buffer_size_db %d", esds->buffer_size_db);
        gst_byte_reader_get_uint32_be (br, &esds->max_bitrate);
        GST_DEBUG ("max_bitrate %d", esds->max_bitrate);
        gst_byte_reader_get_uint32_be (br, &esds->avg_bitrate);
        GST_DEBUG ("avg_bitrate %d", esds->avg_bitrate);
        break;
      case 0x05:               /* DecSpecificInfoTag */
        GST_DEBUG ("codec data len %d", len);
        esds->codec_data_len = len;
        gst_byte_reader_dup_data (br, esds->codec_data_len, &esds->codec_data);
        break;
      case 0x06:               /* SLConfigDescrTag */
      default:
        gst_byte_reader_skip (br, len);
        break;
    }

  }
}

static void
gss_isom_parse_avcC (GssIsomParser * file, GssIsomTrack * track,
    GstByteReader * br)
{
  GssBoxEsds *esds = &track->esds;

  esds->codec_data_len = gst_byte_reader_get_remaining (br);
  gst_byte_reader_dup_data (br, esds->codec_data_len, &esds->codec_data);
}

static Container stsd_atoms[] = {
  {GST_MAKE_FOURCC ('a', 'v', 'c', 'C'), gss_isom_parse_avcC},
  {GST_MAKE_FOURCC ('e', 's', 'd', 's'), gss_isom_parse_esds},
  {GST_MAKE_FOURCC ('s', 'i', 'n', 'f'), gss_isom_parse_ignore},
  {GST_MAKE_FOURCC ('b', 't', 'r', 't'), gss_isom_parse_ignore},
  {0}
};

static void
gss_isom_parse_stsd (GssIsomParser * file, GssIsomTrack * track,
    GstByteReader * br)
{
  GssBoxStsd *stsd = &track->stsd;
  int i;
  stsd->present = TRUE;
  gst_byte_reader_get_uint8 (br, &stsd->version);
  gst_byte_reader_get_uint24_be (br, &stsd->flags);
  gst_byte_reader_get_uint32_be (br, &stsd->entry_count);
  stsd->entries = g_malloc0 (sizeof (GssBoxStsdEntry) * stsd->entry_count);
  for (i = 0; i < stsd->entry_count; i++) {
    guint64 size = 0;
    guint32 size32 = 0;
    guint32 atom = 0;
    GstByteReader sbr;

    gst_byte_reader_get_uint32_be (br, &size32);
    gst_byte_reader_get_uint32_le (br, &atom);
    stsd->entries[i].atom = atom;
    if (size32 == 1) {
      gst_byte_reader_get_uint64_be (br, &size);
    } else {
      size = size32;
    }

    GST_DEBUG ("stsd atom %" GST_FOURCC_FORMAT, GST_FOURCC_ARGS (atom));
    gst_byte_reader_init_sub (&sbr, br, size - 8);
    if (atom == GST_MAKE_FOURCC ('m', 'p', '4', 'a') ||
        atom == GST_MAKE_FOURCC ('e', 'n', 'c', 'a')) {
      GssBoxMp4a *mp4a = &track->mp4a;

      mp4a->present = TRUE;
      gst_byte_reader_skip (&sbr, 6);
      gst_byte_reader_get_uint16_be (&sbr, &mp4a->data_reference_index);

      gst_byte_reader_skip (&sbr, 8);
      gst_byte_reader_get_uint16_be (&sbr, &mp4a->channel_count);
      gst_byte_reader_get_uint16_be (&sbr, &mp4a->sample_size);
      gst_byte_reader_skip (&sbr, 4);
      gst_byte_reader_get_uint32_be (&sbr, &mp4a->sample_rate);

      gss_isom_parse_container (file, track, &sbr, stsd_atoms, atom);
#if 0
      /* esds */
      gst_byte_reader_skip (&sbr, 8);   /* length, esds */
      gst_byte_reader_skip (&sbr, 4);   /* flags */
      gss_isom_parse_esds (file, track, &sbr);
#endif

      CHECK_END (&sbr);
    } else if (atom == GST_MAKE_FOURCC ('a', 'v', 'c', '1') ||
        atom == GST_MAKE_FOURCC ('e', 'n', 'c', 'v') ||
        atom == GST_MAKE_FOURCC ('m', 'p', '4', 'v')) {
      GssBoxMp4v *mp4v = &track->mp4v;
      //GssBoxEsds *esds = &track->esds;

      GST_DEBUG ("avc1");
      mp4v->present = TRUE;
      gst_byte_reader_skip (&sbr, 6);
      gst_byte_reader_get_uint16_be (&sbr, &mp4v->data_reference_index);

      gst_byte_reader_skip (&sbr, 16);
      gst_byte_reader_get_uint16_be (&sbr, &mp4v->width);
      gst_byte_reader_get_uint16_be (&sbr, &mp4v->height);
      GST_DEBUG ("%dx%d", mp4v->width, mp4v->height);
      gst_byte_reader_skip (&sbr, 50);

      gss_isom_parse_container (file, track, &sbr, stsd_atoms, atom);

      //gst_byte_reader_dump (&sbr);
      CHECK_END (&sbr);
    } else if (atom == GST_MAKE_FOURCC ('t', 'm', 'c', 'd')) {
      GST_FIXME ("tmcd");
    } else if (atom == GST_MAKE_FOURCC ('a', 'p', 'c', 'h')) {
      GST_FIXME ("apch");
    } else if (atom == GST_MAKE_FOURCC ('A', 'V', '1', 'x')) {
      GST_FIXME ("AV1x");
    } else {
#if 1
      GST_WARNING ("unknown atom %" GST_FOURCC_FORMAT " inside stsd, size %"
          G_GUINT64_FORMAT, GST_FOURCC_ARGS (atom), size);
#endif
    }

    gst_byte_reader_skip (br, size - 8);
  }

  gst_byte_reader_dump (br);

  CHECK_END (br);
}

static void
gss_isom_parse_ignore (GssIsomParser * file, GssIsomTrack * track,
    GstByteReader * br)
{
  GST_FIXME ("ingoring atom");
}

static Container dinf_atoms[] = {
  {GST_MAKE_FOURCC ('d', 'r', 'e', 'f'), gss_isom_parse_dref},
  {0}
};

static Container stbl_atoms[] = {
  {GST_MAKE_FOURCC ('s', 't', 't', 's'), gss_isom_parse_stts},
  {GST_MAKE_FOURCC ('c', 't', 't', 's'), gss_isom_parse_ctts},
  {GST_MAKE_FOURCC ('s', 't', 's', 's'), gss_isom_parse_stss},
  {GST_MAKE_FOURCC ('s', 't', 's', 'd'), gss_isom_parse_stsd},
  {GST_MAKE_FOURCC ('s', 't', 's', 'z'), gss_isom_parse_stsz},
  {GST_MAKE_FOURCC ('s', 't', 's', 'c'), gss_isom_parse_stsc},
  {GST_MAKE_FOURCC ('s', 't', 'c', 'o'), gss_isom_parse_stco},
  {GST_MAKE_FOURCC ('c', 'o', '6', '4'), gss_isom_parse_co64},
  {GST_MAKE_FOURCC ('s', 't', 's', 'h'), gss_isom_parse_stsh},
  {GST_MAKE_FOURCC ('s', 't', 'd', 'p'), gss_isom_parse_stdp},
  {GST_MAKE_FOURCC ('c', 's', 'l', 'g'), gss_isom_parse_ignore},
  {GST_MAKE_FOURCC ('s', 't', 'p', 's'), gss_isom_parse_ignore},
  {GST_MAKE_FOURCC ('s', 'd', 't', 'p'), gss_isom_parse_ignore},
  {0}
};

static Container minf_atoms[] = {
  {GST_MAKE_FOURCC ('v', 'm', 'h', 'd'), gss_isom_parse_vmhd},
  {GST_MAKE_FOURCC ('s', 'm', 'h', 'd'), gss_isom_parse_smhd},
  {GST_MAKE_FOURCC ('h', 'm', 'h', 'd'), gss_isom_parse_hmhd},
  {GST_MAKE_FOURCC ('d', 'i', 'n', 'f'), NULL, dinf_atoms},
  {GST_MAKE_FOURCC ('s', 't', 'b', 'l'), NULL, stbl_atoms},
  {GST_MAKE_FOURCC ('g', 'm', 'h', 'd'), gss_isom_parse_ignore},
  {GST_MAKE_FOURCC ('h', 'd', 'l', 'r'), gss_isom_parse_ignore},
  {GST_MAKE_FOURCC ('c', 'o', 'd', 'e'), gss_isom_parse_ignore},
  {0}
};

static Container mdia_atoms[] = {
  {GST_MAKE_FOURCC ('m', 'd', 'h', 'd'), gss_isom_parse_mdhd},
  {GST_MAKE_FOURCC ('h', 'd', 'l', 'r'), gss_isom_parse_hdlr},
  {GST_MAKE_FOURCC ('m', 'i', 'n', 'f'), NULL, minf_atoms},
  {GST_MAKE_FOURCC ('i', 'm', 'a', 'p'), gss_isom_parse_ignore},
  {GST_MAKE_FOURCC ('u', 'd', 't', 'a'), gss_isom_parse_ignore},
  {0}
};

static Container edts_atoms[] = {
  {GST_MAKE_FOURCC ('e', 'l', 's', 't'), gss_isom_parse_elst},
  {0}
};

static Container trak_atoms[] = {
  {GST_MAKE_FOURCC ('t', 'k', 'h', 'd'), gss_isom_parse_tkhd},
  {GST_MAKE_FOURCC ('t', 'r', 'e', 'f'), gss_isom_parse_tref},
  //{GST_MAKE_FOURCC ('u', 'd', 't', 'a'), NULL, udta_atoms},
  {GST_MAKE_FOURCC ('u', 'd', 't', 'a'), gss_isom_parse_ignore},
  {GST_MAKE_FOURCC ('e', 'd', 't', 's'), NULL, edts_atoms},
  {GST_MAKE_FOURCC ('m', 'd', 'i', 'a'), NULL, mdia_atoms},
  {GST_MAKE_FOURCC ('m', 'e', 't', 'a'), gss_isom_parse_ignore},
  {GST_MAKE_FOURCC ('l', 'o', 'a', 'd'), gss_isom_parse_ignore},
  {GST_MAKE_FOURCC ('t', 'a', 'p', 't'), gss_isom_parse_ignore},
  {0}
};


static void
gss_isom_parse_container (GssIsomParser * file, GssIsomTrack * track,
    GstByteReader * br, Container * atoms, guint32 parent_atom)
{
  while (gst_byte_reader_get_remaining (br) >= 8) {
    guint64 size = 0;
    guint32 size32 = 0;
    guint32 atom = 0;
    GstByteReader sbr;
    int i;

    gst_byte_reader_get_uint32_be (br, &size32);
    gst_byte_reader_get_uint32_le (br, &atom);
    if (size32 == 1) {
      gst_byte_reader_get_uint64_be (br, &size);
    } else {
      size = size32;
    }

    gst_byte_reader_init_sub (&sbr, br, size - 8);
    for (i = 0; atoms[i].atom != 0; i++) {
      if (atoms[i].atom == atom) {
        if (atoms[i].parse) {
          atoms[i].parse (file, track, &sbr);
        } else {
          gss_isom_parse_container (file, track, &sbr, atoms[i].atoms, atom);
        }
        break;
      }
    }
    if (atoms[i].atom == 0) {
      GST_WARNING ("unknown atom %" GST_FOURCC_FORMAT
          " inside %" GST_FOURCC_FORMAT ", size %" G_GUINT64_FORMAT,
          GST_FOURCC_ARGS (atom), GST_FOURCC_ARGS (parent_atom), size);
    }

    gst_byte_reader_skip (br, size - 8);
  }

  CHECK_END_BOX (br, parent_atom);
}

static void
gss_isom_parse_container_udta (GssIsomParser * file, GssIsomMovie * movie,
    GstByteReader * br, Container * atoms, guint32 parent_atom)
{
  while (gst_byte_reader_get_remaining (br) >= 8) {
    guint64 size = 0;
    guint32 size32 = 0;
    guint32 atom = 0;
    GstByteReader sbr;
    int i;

    gst_byte_reader_get_uint32_be (br, &size32);
    gst_byte_reader_get_uint32_le (br, &atom);
    if (size32 == 1) {
      gst_byte_reader_get_uint64_be (br, &size);
    } else {
      size = size32;
    }

    GST_DEBUG ("size %08x atom %" GST_FOURCC_FORMAT, size32,
        GST_FOURCC_ARGS (atom));

    gst_byte_reader_init_sub (&sbr, br, size - 8);
    for (i = 0; atoms[i].atom != 0; i++) {
      if (atoms[i].atom == atom) {
        if (atoms[i].parse) {
          atoms[i].parse (file, (void *) movie, &sbr);
        }
        if (atoms[i].atoms) {
          gss_isom_parse_container_udta (file, movie, &sbr, atoms[i].atoms,
              atom);
        }
        break;
      }
    }
    if (atoms[i].atom == 0) {
      GST_WARNING ("unknown atom %" GST_FOURCC_FORMAT
          " inside %" GST_FOURCC_FORMAT ", size %" G_GUINT64_FORMAT,
          GST_FOURCC_ARGS (atom), GST_FOURCC_ARGS (parent_atom), size);
    }

    gst_byte_reader_skip (br, size - 8);
  }

  CHECK_END_BOX (br, parent_atom);
}

static void
gss_isom_parse_udta_meta (GssIsomParser * file, GssIsomTrack * track,
    GstByteReader * br)
{
  //GssIsomMovie *movie = (void *)track;
  guint32 tmp;
  gst_byte_reader_get_uint32_be (br, &tmp);
}

static void
gss_isom_parse_store (GssBoxStore * store, GstByteReader * br)
{
  store->present = TRUE;
  store->size = gst_byte_reader_get_remaining (br);
  gst_byte_reader_dup_data (br, store->size, &store->data);
}

static void
gss_isom_parse_xtra (GssIsomParser * file, GssIsomTrack * track,
    GstByteReader * br)
{
  GssIsomMovie *movie = (void *) track;
  gss_isom_parse_store (&movie->xtra, br);
}

static void
gss_isom_parse_udta_hdlr (GssIsomParser * file, GssIsomTrack * track,
    GstByteReader * br)
{
  GssIsomMovie *movie = (void *) track;
  gss_isom_parse_store (&movie->hdlr, br);
}

static void
gss_isom_parse_ilst (GssIsomParser * file, GssIsomTrack * track,
    GstByteReader * br)
{
  GssIsomMovie *movie = (void *) track;
  gss_isom_parse_store (&movie->ilst, br);
}

static Container udta_meta_atoms[] = {
  {GST_MAKE_FOURCC ('h', 'd', 'l', 'r'), gss_isom_parse_udta_hdlr},
  {GST_MAKE_FOURCC ('i', 'l', 's', 't'), gss_isom_parse_ilst},
  {0}
};

static Container udta_atoms[] = {
  {GST_MAKE_FOURCC ('m', 'e', 't', 'a'), gss_isom_parse_udta_meta,
      udta_meta_atoms},
  {GST_MAKE_FOURCC ('X', 't', 'r', 'a'), gss_isom_parse_xtra},
  //{GST_MAKE_FOURCC ('h', 'd', 'l', 'r'), gss_isom_parse_hdlr},
  //{GST_MAKE_FOURCC ('m', 'd', 'i', 'r'), gss_isom_parse_mdir},
  //{GST_MAKE_FOURCC ('i', 'l', 's', 't'), gss_isom_parse_ilst},
  {0}
};

static void
gss_isom_parse_udta (GssIsomParser * file, GssIsomMovie * movie,
    GstByteReader * br)
{
  gss_isom_parse_container_udta (file, movie, br, udta_atoms,
      GST_MAKE_FOURCC ('u', 'd', 't', 'a'));
}

static void
gss_isom_parse_mvex (GssIsomParser * file, GssIsomMovie * movie,
    GstByteReader * br)
{
  gss_isom_parse_store (&movie->mvex, br);
}

static void
gss_isom_parse_skip (GssIsomParser * file, GssBoxSkip * skip,
    GstByteReader * br)
{
}

static void
gss_isom_parse_iods (GssIsomParser * file, GssIsomMovie * movie,
    GstByteReader * br)
{
  gss_isom_parse_store (&movie->iods, br);
}

static void
gss_isom_parse_meta (GssIsomParser * file, GssIsomMovie * movie,
    GstByteReader * br)
{
  gss_isom_parse_store (&movie->meta, br);
}

static void
gss_isom_parse_moov (GssIsomParser * file, GssIsomMovie * movie,
    GstByteReader * br)
{
  while (gst_byte_reader_get_remaining (br) >= 8) {
    guint64 size = 0;
    guint32 size32 = 0;
    guint32 atom = 0;
    GstByteReader sbr;

    gst_byte_reader_get_uint32_be (br, &size32);
    gst_byte_reader_get_uint32_le (br, &atom);
    if (size32 == 1) {
      gst_byte_reader_get_uint64_be (br, &size);
    } else {
      size = size32;
    }

    gst_byte_reader_init_sub (&sbr, br, size - 8);
    if (atom == GST_MAKE_FOURCC ('m', 'v', 'h', 'd')) {
      gss_isom_parse_mvhd (file, &movie->mvhd, &sbr);
    } else if (atom == GST_MAKE_FOURCC ('t', 'r', 'a', 'k')) {
      GssIsomTrack *track;
      track = gss_isom_track_new ();
      gss_isom_parse_container (file, track, &sbr, trak_atoms, atom);
      movie->tracks[movie->n_tracks] = track;
      movie->n_tracks++;
    } else if (atom == GST_MAKE_FOURCC ('u', 'd', 't', 'a')) {
      gss_isom_parse_udta (file, movie, &sbr);
    } else if (atom == GST_MAKE_FOURCC ('m', 'v', 'e', 'x')) {
      gss_isom_parse_mvex (file, movie, &sbr);
    } else if (atom == GST_MAKE_FOURCC ('m', 'e', 't', 'a')) {
      gss_isom_parse_meta (file, movie, &sbr);
    } else if (atom == GST_MAKE_FOURCC ('s', 'k', 'i', 'p')) {
      gss_isom_parse_skip (file, &movie->skip, &sbr);
    } else if (atom == GST_MAKE_FOURCC ('i', 'o', 'd', 's')) {
      gss_isom_parse_iods (file, movie, &sbr);
    } else if (atom == GST_MAKE_FOURCC ('a', 'i', 'n', 'f')) {
      movie->ainf.present = TRUE;
      movie->ainf.atom = atom;
      movie->ainf.size = gst_byte_reader_get_remaining (&sbr);
      gst_byte_reader_dup_data (&sbr, movie->ainf.size, &movie->ainf.data);
    } else if (atom == GST_MAKE_FOURCC ('u', 'u', 'i', 'd')) {
      const guint8 *uuid = NULL;

      gst_byte_reader_get_data (&sbr, 16, &uuid);

      if (0) {
      } else {
        GST_WARNING ("unknown UUID: %02x%02x%02x%02x-%02x%02x-%02x%02x-"
            "%02x%02x-%02x%02x%02x%02x%02x%02x\n",
            uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6],
            uuid[7], uuid[8], uuid[9], uuid[10], uuid[11], uuid[12], uuid[13],
            uuid[14], uuid[15]);
      }
    } else {
      GST_WARNING ("unknown atom %" GST_FOURCC_FORMAT
          " inside moov at offset %" G_GINT64_MODIFIER "x, size %"
          G_GUINT64_FORMAT, GST_FOURCC_ARGS (atom), file->offset + br->byte,
          size);
    }

    gst_byte_reader_skip (br, size - 8);
  }
}


#define BOX_INIT(bw, atom) (gst_byte_writer_put_uint32_be ((bw), 0), \
  gst_byte_writer_put_uint32_le ((bw), (atom)), (bw)->parent.byte - 8)
#define BOX_FINISH(bw, offset) \
  GST_WRITE_UINT32_BE((void *)(bw)->parent.data + (offset), \
      (bw)->parent.byte - (offset))

static void
gss_isom_tfhd_serialize (GssBoxTfhd * tfhd, GstByteWriter * bw)
{
  int offset;

  offset = BOX_INIT (bw, GST_MAKE_FOURCC ('t', 'f', 'h', 'd'));

  gst_byte_writer_put_uint8 (bw, tfhd->version);
  gst_byte_writer_put_uint24_be (bw, tfhd->flags);
  gst_byte_writer_put_uint32_be (bw, tfhd->track_id);
  if (tfhd->flags & TF_SAMPLE_DESCRIPTION_INDEX) {
    /* FIXME not handled */
    gst_byte_writer_put_uint32_be (bw, 0);
  }
  if (tfhd->flags & TF_DEFAULT_SAMPLE_DURATION) {
    gst_byte_writer_put_uint32_be (bw, tfhd->default_sample_duration);
  }
  if (tfhd->flags & TF_DEFAULT_SAMPLE_SIZE) {
    gst_byte_writer_put_uint32_be (bw, tfhd->default_sample_size);
  }
  if (tfhd->flags & TF_DEFAULT_SAMPLE_FLAGS) {
    gst_byte_writer_put_uint32_be (bw, tfhd->default_sample_flags);
  }

  BOX_FINISH (bw, offset);
}

static void
gss_isom_tfdt_serialize (GssBoxTfdt * tfdt, GstByteWriter * bw)
{
  int offset;

  offset = BOX_INIT (bw, GST_MAKE_FOURCC ('t', 'f', 'd', 't'));

  gst_byte_writer_put_uint8 (bw, tfdt->version);
  gst_byte_writer_put_uint24_be (bw, tfdt->flags);
  if (tfdt->version == 1) {
    gst_byte_writer_put_uint64_be (bw, tfdt->start_time);
  } else {
    gst_byte_writer_put_uint32_be (bw, tfdt->start_time);
  }

  BOX_FINISH (bw, offset);
}

static void
gss_isom_trun_serialize (GssBoxTrun * trun, GstByteWriter * bw)
{
  int offset;
  int i;

  offset = BOX_INIT (bw, GST_MAKE_FOURCC ('t', 'r', 'u', 'n'));

  gst_byte_writer_put_uint8 (bw, trun->version);
  gst_byte_writer_put_uint24_be (bw, trun->flags);
  gst_byte_writer_put_uint32_be (bw, trun->sample_count);
  if (trun->flags & TR_DATA_OFFSET) {
    /* This needs to be fixed up later, once we know the size of
     * the moof atom. */
    trun->data_offset_fixup = bw->parent.byte;
    gst_byte_writer_put_uint32_be (bw, 0);
  }
  if (trun->flags & TR_FIRST_SAMPLE_FLAGS) {
    gst_byte_writer_put_uint32_be (bw, trun->first_sample_flags);
  }

  for (i = 0; i < trun->sample_count; i++) {
    if (trun->flags & TR_SAMPLE_DURATION) {
      gst_byte_writer_put_uint32_be (bw, trun->samples[i].duration);
    }
    if (trun->flags & TR_SAMPLE_SIZE) {
      gst_byte_writer_put_uint32_be (bw, trun->samples[i].size);
    }
    if (trun->flags & TR_SAMPLE_FLAGS) {
      gst_byte_writer_put_uint32_be (bw, trun->samples[i].flags);
    }
    if (trun->flags & TR_SAMPLE_COMPOSITION_TIME_OFFSETS) {
      gst_byte_writer_put_uint32_be (bw,
          trun->samples[i].composition_time_offset);
    }
  }

  BOX_FINISH (bw, offset);
}

#ifdef unused
static void
gss_isom_trun_dump (GssBoxTrun * trun)
{
  int i;

  g_print ("trun:\n");
  g_print ("  version: %d\n", trun->version);
  g_print ("  flags: %06x\n", trun->flags);
  g_print ("  sample_count: %d\n", trun->sample_count);
  if (trun->flags & TR_DATA_OFFSET) {
    g_print ("  data_offset: unknown\n");
  }
  if (trun->flags & TR_FIRST_SAMPLE_FLAGS) {
    g_print ("  first_sample_flags: %08x\n", trun->first_sample_flags);
  }
  g_print ("  samples:\n");
  for (i = 0; i < trun->sample_count; i++) {
    if (trun->flags & TR_SAMPLE_DURATION) {
      g_print ("    duration: %d\n", trun->samples[i].duration);
    }
    if (trun->flags & TR_SAMPLE_SIZE) {
      g_print ("    size: %d\n", trun->samples[i].size);
    }
    if (trun->flags & TR_SAMPLE_FLAGS) {
      g_print ("    flags: %08x\n", trun->samples[i].flags);
    }
    if (trun->flags & TR_SAMPLE_COMPOSITION_TIME_OFFSETS) {
      g_print ("    cto: %d\n", trun->samples[i].composition_time_offset);
    }
  }

}
#endif


static void
gss_isom_avcn_serialize (GssBoxAvcn * avcn, GstByteWriter * bw)
{
  const guint8 bytes[] = {
    0x01, 0x64, 0x00, 0x28, 0xff, 0xe1, 0x00, 0x3b, 0x67, 0x64, 0x00, 0x28,
    0xac, 0x2c, 0xa5, 0x01,
    0xe0, 0x08, 0x9f, 0x97, 0xff, 0x04, 0x00, 0x04, 0x00, 0x52, 0x02, 0x02,
    0x02, 0x80, 0x00, 0x00,
    0x03, 0x00, 0x80, 0x00, 0x00, 0x18, 0x31, 0x30, 0x00, 0x16, 0xe3, 0x60,
    0x00, 0x0e, 0x4e, 0x1f,
    0xf8, 0xc7, 0x18, 0x98, 0x00, 0x0b, 0x71, 0xb0, 0x00, 0x07, 0x27, 0x0f,
    0xfc, 0x63, 0x87, 0x68,
    0x58, 0xb4, 0x58, 0x01, 0x00, 0x05, 0x68, 0xe9, 0x09, 0x35, 0x25
  };
  int offset;

  offset = BOX_INIT (bw, GST_MAKE_FOURCC ('a', 'v', 'c', 'n'));

  gst_byte_writer_put_data (bw, bytes, 0x4b);

  BOX_FINISH (bw, offset);
}

static void
gss_isom_trik_serialize (GssBoxTrik * trik, GstByteWriter * bw)
{
  const guint8 bytes[] = {
    0x00, 0x00, 0x00, 0x00, 0x41, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03,
    0x03,
    0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03,
    0x03, 0x03, 0x03, 0x03,
    0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03,
    0x03, 0x03, 0x03, 0x03,
    0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03,
  };
  int offset;

  offset = BOX_INIT (bw, GST_MAKE_FOURCC ('t', 'r', 'i', 'k'));

  gst_byte_writer_put_data (bw, bytes, 0x34);

  BOX_FINISH (bw, offset);
}

static void
gss_isom_sdtp_serialize (GssBoxSdtp * sdtp, GstByteWriter * bw,
    int sample_count)
{
  int offset;
  int i;

  offset = BOX_INIT (bw, GST_MAKE_FOURCC ('s', 'd', 't', 'p'));

  gst_byte_writer_put_uint8 (bw, sdtp->version);
  gst_byte_writer_put_uint24_be (bw, sdtp->flags);
  for (i = 0; i < sample_count; i++) {
    gst_byte_writer_put_uint8 (bw, sdtp->sample_flags[i]);
  }

  BOX_FINISH (bw, offset);
}

static void
gss_isom_sample_encryption_serialize (GssBoxUUIDSampleEncryption * se,
    GstByteWriter * bw)
{
  int offset;
  int i, j;

  if (!se->present)
    return;
  offset = BOX_INIT (bw, GST_MAKE_FOURCC ('u', 'u', 'i', 'd'));

  gst_byte_writer_put_data (bw, uuid_sample_encryption, 16);
  gst_byte_writer_put_uint8 (bw, se->version);
  gst_byte_writer_put_uint24_be (bw, se->flags);
  if (se->flags & 0x0001) {
    gst_byte_writer_put_uint24_be (bw, se->algorithm_id);
    gst_byte_writer_put_uint8 (bw, se->iv_size);
    gst_byte_writer_put_data (bw, se->kid, 16);
  }
  gst_byte_writer_put_uint32_be (bw, se->sample_count);
  for (i = 0; i < se->sample_count; i++) {
    gst_byte_writer_put_uint64_be (bw, se->samples[i].iv);
    if (se->flags & 0x0002) {
      gst_byte_writer_put_uint16_be (bw, se->samples[i].num_entries);
      for (j = 0; j < se->samples[i].num_entries; j++) {
        gst_byte_writer_put_uint16_be (bw,
            se->samples[i].entries[j].bytes_of_clear_data);
        gst_byte_writer_put_uint32_be (bw,
            se->samples[i].entries[j].bytes_of_encrypted_data);
      }
    }
  }

  BOX_FINISH (bw, offset);
}


static void
gss_isom_traf_serialize (GssIsomFragment * fragment, GstByteWriter * bw,
    gboolean is_video)
{
  int offset;
  offset = BOX_INIT (bw, GST_MAKE_FOURCC ('t', 'r', 'a', 'f'));

  gss_isom_tfhd_serialize (&fragment->tfhd, bw);
  gss_isom_tfdt_serialize (&fragment->tfdt, bw);
  gss_isom_trun_serialize (&fragment->trun, bw);
  if (0 && is_video) {
    gss_isom_avcn_serialize (&fragment->avcn, bw);
    gss_isom_trik_serialize (&fragment->trik, bw);
  }
  if (0)
    gss_isom_sdtp_serialize (&fragment->sdtp, bw, fragment->trun.sample_count);
  gss_isom_sample_encryption_serialize (&fragment->sample_encryption, bw);

  BOX_FINISH (bw, offset);
}

static void
gss_isom_mfhd_serialize (GssBoxMfhd * mfhd, GstByteWriter * bw)
{
  int offset;
  offset = BOX_INIT (bw, GST_MAKE_FOURCC ('m', 'f', 'h', 'd'));

  gst_byte_writer_put_uint8 (bw, mfhd->version);
  gst_byte_writer_put_uint24_be (bw, mfhd->flags);
  gst_byte_writer_put_uint32_be (bw, mfhd->sequence_number);


  BOX_FINISH (bw, offset);
}

static void
gss_isom_sidx_serialize (GssBoxSidx * sidx, GstByteWriter * bw)
{
  int offset;
  int i;

  offset = BOX_INIT (bw, GST_MAKE_FOURCC ('s', 'i', 'd', 'x'));
  gst_byte_writer_put_uint8 (bw, sidx->version);
  gst_byte_writer_put_uint24_be (bw, sidx->flags);
  gst_byte_writer_put_uint32_be (bw, sidx->reference_id);
  gst_byte_writer_put_uint32_be (bw, sidx->timescale);
  if (sidx->version == 0) {
    gst_byte_writer_put_uint32_be (bw, sidx->earliest_presentation_time);
    gst_byte_writer_put_uint32_be (bw, sidx->first_offset);
  } else {
    gst_byte_writer_put_uint64_be (bw, sidx->earliest_presentation_time);
    gst_byte_writer_put_uint64_be (bw, sidx->first_offset);
  }
  gst_byte_writer_put_uint16_be (bw, 0);        /* reserved */
  gst_byte_writer_put_uint16_be (bw, sidx->n_entries);
  for (i = 0; i < sidx->n_entries; i++) {
    guint32 tmp;
    tmp = ((sidx->entries[i].reference_type & 1) << 31);
    tmp |= (sidx->entries[i].reference_size) & 0x7fffffff;
    gst_byte_writer_put_uint32_be (bw, tmp);
    gst_byte_writer_put_uint32_be (bw, sidx->entries[i].subsegment_duration);
    tmp = ((sidx->entries[i].starts_with_sap & 1) << 31);
    tmp |= ((sidx->entries[i].sap_type & 0x7) << 28);
    tmp |= (sidx->entries[i].sap_delta_time) & 0x0fffffff;
    gst_byte_writer_put_uint32_be (bw, tmp);
  }

  BOX_FINISH (bw, offset);
}

void
gss_isom_fragment_serialize (GssIsomFragment * fragment, guint8 ** data,
    gsize * size, gboolean is_video)
{
  GstByteWriter *bw;
  int offset_moof;

  bw = gst_byte_writer_new ();

  offset_moof = BOX_INIT (bw, GST_MAKE_FOURCC ('m', 'o', 'o', 'f'));
  gss_isom_mfhd_serialize (&fragment->mfhd, bw);
  gss_isom_traf_serialize (fragment, bw, is_video);

  BOX_FINISH (bw, offset_moof);

  GST_WRITE_UINT32_BE ((void *) (bw->parent.data +
          fragment->trun.data_offset_fixup), bw->parent.byte + 8 - offset_moof);

  gst_byte_writer_put_uint32_be (bw, fragment->mdat_size);
  gst_byte_writer_put_uint32_le (bw, GST_MAKE_FOURCC ('m', 'd', 'a', 't'));

  *size = bw->parent.byte;
  *data = gst_byte_writer_free_and_get_data (bw);
}

static void
gss_isom_tkhd_serialize (GssBoxTkhd * tkhd, GstByteWriter * bw)
{
  int offset;
  int i;

  if (!tkhd->present)
    return;

  offset = BOX_INIT (bw, GST_MAKE_FOURCC ('t', 'k', 'h', 'd'));

  gst_byte_writer_put_uint8 (bw, tkhd->version);
  gst_byte_writer_put_uint24_be (bw, tkhd->flags);
  if (tkhd->version == 1) {
    gst_byte_writer_put_uint64_be (bw, tkhd->creation_time);
    gst_byte_writer_put_uint64_be (bw, tkhd->modification_time);
    gst_byte_writer_put_uint32_be (bw, tkhd->track_id);
    gst_byte_writer_put_uint32_be (bw, 0);
    gst_byte_writer_put_uint64_be (bw, tkhd->duration);
  } else {
    gst_byte_writer_put_uint32_be (bw, tkhd->creation_time);
    gst_byte_writer_put_uint32_be (bw, tkhd->modification_time);
    gst_byte_writer_put_uint32_be (bw, tkhd->track_id);
    gst_byte_writer_put_uint32_be (bw, 0);
    gst_byte_writer_put_uint32_be (bw, tkhd->duration);
  }
  gst_byte_writer_put_uint32_be (bw, 0);
  gst_byte_writer_put_uint32_be (bw, 0);
  gst_byte_writer_put_uint16_be (bw, tkhd->layer);
  gst_byte_writer_put_uint16_be (bw, tkhd->alternate_group);
  gst_byte_writer_put_uint16_be (bw, tkhd->volume);
  gst_byte_writer_put_uint16_be (bw, 0);
  for (i = 0; i < 9; i++) {
    gst_byte_writer_put_uint32_be (bw, tkhd->matrix[i]);
  }
  gst_byte_writer_put_uint32_be (bw, tkhd->width);
  gst_byte_writer_put_uint32_be (bw, tkhd->height);

  BOX_FINISH (bw, offset);
}

static void
gss_isom_tref_serialize (GssBoxTref * tref, GstByteWriter * bw)
{
  int offset;

  return;

  offset = BOX_INIT (bw, GST_MAKE_FOURCC ('t', 'r', 'e', 'f'));

  BOX_FINISH (bw, offset);
}

static void
gss_isom_mdhd_serialize (GssBoxMdhd * mdhd, GstByteWriter * bw)
{
  int offset;

  offset = BOX_INIT (bw, GST_MAKE_FOURCC ('m', 'd', 'h', 'd'));

  gst_byte_writer_put_uint8 (bw, mdhd->version);
  gst_byte_writer_put_uint24_be (bw, mdhd->flags);
  if (mdhd->version == 1) {
    gst_byte_writer_put_uint64_be (bw, mdhd->creation_time);
    gst_byte_writer_put_uint64_be (bw, mdhd->modification_time);
    gst_byte_writer_put_uint32_be (bw, mdhd->timescale);
    gst_byte_writer_put_uint64_be (bw, mdhd->duration);
  } else {
    gst_byte_writer_put_uint32_be (bw, mdhd->creation_time);
    gst_byte_writer_put_uint32_be (bw, mdhd->modification_time);
    gst_byte_writer_put_uint32_be (bw, mdhd->timescale);
    gst_byte_writer_put_uint32_be (bw, mdhd->duration);
  }
  gst_byte_writer_put_uint16_be (bw, pack_language_code (mdhd->language_code));
  gst_byte_writer_put_uint16_be (bw, 0);

  BOX_FINISH (bw, offset);
}

static void
gss_isom_hdlr_serialize (GssBoxHdlr * hdlr, GstByteWriter * bw)
{
  int offset;

  if (!hdlr->present)
    return;
  offset = BOX_INIT (bw, GST_MAKE_FOURCC ('h', 'd', 'l', 'r'));

  gst_byte_writer_put_uint8 (bw, hdlr->version);
  gst_byte_writer_put_uint24_be (bw, hdlr->flags);
  gst_byte_writer_put_uint32_be (bw, 0);
  gst_byte_writer_put_uint32_le (bw, hdlr->handler_type);
  gst_byte_writer_fill (bw, 0, 12);
  put_string (bw, hdlr->name, TRUE);

  BOX_FINISH (bw, offset);
}

static void
gss_isom_vmhd_serialize (GssBoxVmhd * vmhd, GstByteWriter * bw)
{
  int offset;

  if (!vmhd->present)
    return;

  offset = BOX_INIT (bw, GST_MAKE_FOURCC ('v', 'm', 'h', 'd'));

  gst_byte_writer_put_uint8 (bw, vmhd->version);
  gst_byte_writer_put_uint24_be (bw, vmhd->flags);
  gst_byte_writer_fill (bw, 0, 8);

  BOX_FINISH (bw, offset);
}

static void
gss_isom_smhd_serialize (GssBoxSmhd * smhd, GstByteWriter * bw)
{
  int offset;

  if (!smhd->present)
    return;

  offset = BOX_INIT (bw, GST_MAKE_FOURCC ('s', 'm', 'h', 'd'));

  gst_byte_writer_put_uint8 (bw, smhd->version);
  gst_byte_writer_put_uint24_be (bw, smhd->flags);
  gst_byte_writer_fill (bw, 0, 4);

  BOX_FINISH (bw, offset);
}

static void
gss_isom_hmhd_serialize (GssBoxHmhd * hmhd, GstByteWriter * bw)
{
  int offset;

  if (!hmhd->present)
    return;

  offset = BOX_INIT (bw, GST_MAKE_FOURCC ('h', 'm', 'h', 'd'));

  gst_byte_writer_put_uint8 (bw, hmhd->version);
  gst_byte_writer_put_uint24_be (bw, hmhd->flags);
  gst_byte_writer_put_uint16_be (bw, hmhd->maxPDUsize);
  gst_byte_writer_put_uint16_be (bw, hmhd->avgPDUsize);
  gst_byte_writer_put_uint32_be (bw, hmhd->maxbitrate);
  gst_byte_writer_put_uint32_be (bw, hmhd->avgbitrate);
  gst_byte_writer_put_uint32_be (bw, 0);

  BOX_FINISH (bw, offset);
}

#ifdef unused
static void
gss_isom_dref_serialize (GssBoxDref * dref, GstByteWriter * bw)
{
  int offset;
  int i;

  if (!dref->present)
    return;

  offset = BOX_INIT (bw, GST_MAKE_FOURCC ('d', 'r', 'e', 'f'));

  gst_byte_writer_put_uint8 (bw, dref->version);
  gst_byte_writer_put_uint24_be (bw, dref->flags);
  gst_byte_writer_put_uint32_be (bw, dref->entry_count);
  for (i = 0; i < dref->entry_count; i++) {
    int offset_entry;

    offset_entry = BOX_INIT (bw, dref->entries[i].atom);

    if (dref->entries[i].atom == GST_MAKE_FOURCC ('u', 'r', 'l', ' ')) {
      gst_byte_writer_put_uint32_be (bw, 0x00000001);
      GST_FIXME ("url_");
    } else if (dref->entries[i].atom == GST_MAKE_FOURCC ('u', 'r', 'n', ' ')) {
      GST_FIXME ("urn_");
    } else if (dref->entries[i].atom == GST_MAKE_FOURCC ('a', 'l', 'i', 's')) {
      GST_FIXME ("alis");
    } else if (dref->entries[i].atom == GST_MAKE_FOURCC ('c', 'i', 'o', 's')) {
      GST_FIXME ("cios");
    } else {
      GST_WARNING ("unknown atom %" GST_FOURCC_FORMAT " inside dref",
          GST_FOURCC_ARGS (dref->entries[i].atom));
    }

    BOX_FINISH (bw, offset_entry);
  }

  BOX_FINISH (bw, offset);
}
#endif

static void
gss_isom_stts_serialize (GssBoxStts * stts, GstByteWriter * bw)
{
  int offset;
  int i;

  offset = BOX_INIT (bw, GST_MAKE_FOURCC ('s', 't', 't', 's'));

  gst_byte_writer_put_uint8 (bw, stts->version);
  gst_byte_writer_put_uint24_be (bw, stts->flags);
  gst_byte_writer_put_uint32_be (bw, stts->entry_count);
  for (i = 0; i < stts->entry_count; i++) {
    gst_byte_writer_put_uint32_be (bw, stts->entries[i].sample_count);
    gst_byte_writer_put_int32_be (bw, stts->entries[i].sample_delta);
  }

  BOX_FINISH (bw, offset);
}

static void
gss_isom_ctts_serialize (GssBoxCtts * ctts, GstByteWriter * bw)
{
  int offset;
  int i;

  offset = BOX_INIT (bw, GST_MAKE_FOURCC ('c', 't', 't', 's'));

  gst_byte_writer_put_uint8 (bw, ctts->version);
  gst_byte_writer_put_uint24_be (bw, ctts->flags);
  gst_byte_writer_put_uint32_be (bw, ctts->entry_count);
  for (i = 0; i < ctts->entry_count; i++) {
    gst_byte_writer_put_uint32_be (bw, ctts->entries[i].sample_count);
    gst_byte_writer_put_uint32_be (bw, ctts->entries[i].sample_offset);
  }

  BOX_FINISH (bw, offset);
}

static void
gss_isom_stss_serialize (GssBoxStss * stss, GstByteWriter * bw)
{
  int offset;
  int i;

  offset = BOX_INIT (bw, GST_MAKE_FOURCC ('s', 't', 's', 's'));

  gst_byte_writer_put_uint8 (bw, stss->version);
  gst_byte_writer_put_uint24_be (bw, stss->flags);
  gst_byte_writer_put_uint32_be (bw, stss->entry_count);
  for (i = 0; i < stss->entry_count; i++) {
    gst_byte_writer_put_uint32_be (bw, stss->sample_numbers[i]);
  }


  BOX_FINISH (bw, offset);
}

static void
gss_isom_avcC_serialize (GssIsomTrack * track, GstByteWriter * bw)
{
  GssBoxEsds *esds = &track->esds;

  gst_byte_writer_put_data (bw, esds->codec_data, esds->codec_data_len);
}

static void
gss_isom_esds_serialize (GssBoxEsds * esds, GstByteWriter * bw)
{
  int n_tags;
  int i;

  gst_byte_writer_put_uint32_be (bw, 0);

  n_tags = 4;
  for (i = 0; i < n_tags; i++) {
    static int tags[4] = { 0x03, 0x04, 0x05, 0x06 };
    guint8 tag = 0;
    guint32 len = 0;

    tag = tags[i];

    gst_byte_writer_put_uint8 (bw, tag);
    /* placeholder for length */
    gst_byte_writer_put_uint32_be (bw, 0x80808000);
#if 0
    do {
      gst_byte_writer_put_uint8 (bw, &tmp);
      len <<= 7;
      len |= (tmp & 0x7f);
    } while (tmp & 0x80);
#endif

    GST_DEBUG ("tag %d len %d", tag, len);
    switch (tag) {
      case 0x03:               /* ES_DescrTag */
        gst_byte_writer_put_uint16_be (bw, esds->es_id);
        gst_byte_writer_put_uint8 (bw, esds->es_flags);
        if (esds->es_flags & 0x80) {
          gst_byte_writer_fill (bw, 0, 2);
        }
        if (esds->es_flags & 0x40) {
          gst_byte_writer_fill (bw, 0, 2);
        }
        if (esds->es_flags & 0x20) {
          gst_byte_writer_fill (bw, 0, 2);
        }
        break;
      case 0x04:               /* DecoderConfigDescrTag */
        gst_byte_writer_put_uint8 (bw, esds->type_indication);
        gst_byte_writer_put_uint8 (bw, esds->stream_type);
        gst_byte_writer_put_uint24_be (bw, esds->buffer_size_db);
        gst_byte_writer_put_uint32_be (bw, esds->max_bitrate);
        gst_byte_writer_put_uint32_be (bw, esds->avg_bitrate);
        break;
      case 0x05:               /* DecSpecificInfoTag */
        GST_DEBUG ("codec data len %d", len);
        esds->codec_data_len = len;
        gst_byte_writer_put_data (bw, esds->codec_data, esds->codec_data_len);
        break;
      case 0x06:               /* SLConfigDescrTag */
      default:
        gst_byte_writer_fill (bw, 0, len);
        break;
    }

  }
}

static void
gss_isom_stsd_serialize (GssIsomTrack * track, GstByteWriter * bw)
{
  int offset;
  int i;
  GssBoxStsd *stsd = &track->stsd;

  offset = BOX_INIT (bw, GST_MAKE_FOURCC ('s', 't', 's', 'd'));

  gst_byte_writer_put_uint8 (bw, stsd->version);
  gst_byte_writer_put_uint24_be (bw, stsd->flags);
  gst_byte_writer_put_uint32_be (bw, stsd->entry_count);
  for (i = 0; i < stsd->entry_count; i++) {
    int offset_entry;
    guint32 atom = stsd->entries[i].atom;

    offset_entry = BOX_INIT (bw, atom);

    GST_DEBUG ("stsd atom %" GST_FOURCC_FORMAT, GST_FOURCC_ARGS (atom));
    if (atom == GST_MAKE_FOURCC ('m', 'p', '4', 'a') ||
        atom == GST_MAKE_FOURCC ('e', 'n', 'c', 'a')) {
      GssBoxMp4a *mp4a = &track->mp4a;
      int offset_esds;

      GST_DEBUG ("serialize mp4a");
      mp4a->present = TRUE;
      gst_byte_writer_fill (bw, 0, 6);
      gst_byte_writer_put_uint16_be (bw, mp4a->data_reference_index);

      gst_byte_writer_fill (bw, 0, 8);
      gst_byte_writer_put_uint16_be (bw, mp4a->channel_count);
      gst_byte_writer_put_uint16_be (bw, mp4a->sample_size);
      gst_byte_writer_fill (bw, 0, 4);
      gst_byte_writer_put_uint32_be (bw, mp4a->sample_rate);

      /* FIXME serialize container */

      /* esds */
      offset_esds = BOX_INIT (bw, GST_MAKE_FOURCC ('e', 's', 'd', 's'));
      if (0)
        gss_isom_esds_serialize (&track->esds, bw);
      gst_byte_writer_put_data (bw, track->esds_store.data,
          track->esds_store.size);
      BOX_FINISH (bw, offset_esds);
    } else if (atom == GST_MAKE_FOURCC ('a', 'v', 'c', '1') ||
        atom == GST_MAKE_FOURCC ('e', 'n', 'c', 'v') ||
        atom == GST_MAKE_FOURCC ('m', 'p', '4', 'v')) {
      GssBoxMp4v *mp4v = &track->mp4v;
      int offset_avcC;
      //GssBoxEsds *esds = &track->esds;

      GST_DEBUG ("avc1");
      mp4v->present = TRUE;
      gst_byte_writer_fill (bw, 0, 6);
      gst_byte_writer_put_uint16_be (bw, mp4v->data_reference_index);

      gst_byte_writer_fill (bw, 0, 16);
      gst_byte_writer_put_uint16_be (bw, mp4v->width);
      gst_byte_writer_put_uint16_be (bw, mp4v->height);
      GST_DEBUG ("%dx%d", mp4v->width, mp4v->height);
      gst_byte_writer_put_uint32_be (bw, 0x00480000);
      gst_byte_writer_put_uint32_be (bw, 0x00480000);

      gst_byte_writer_put_uint32_be (bw, 0x00000000);
      gst_byte_writer_put_uint32_be (bw, 0x00010a41);
      gst_byte_writer_put_uint32_be (bw, 0x56432043);   //"AVC Coding"
      gst_byte_writer_put_uint32_be (bw, 0x6f64696e);
      gst_byte_writer_put_uint32_be (bw, 0x67000000);
      gst_byte_writer_put_uint32_be (bw, 0x00000000);
      gst_byte_writer_put_uint32_be (bw, 0x00000000);
      gst_byte_writer_put_uint32_be (bw, 0x00000000);
      gst_byte_writer_put_uint32_be (bw, 0x00000000);
      gst_byte_writer_put_uint32_be (bw, 0x00000018);
      gst_byte_writer_put_uint16_be (bw, 0xffff);

      offset_avcC = BOX_INIT (bw, GST_MAKE_FOURCC ('a', 'v', 'c', 'C'));
      gss_isom_avcC_serialize (track, bw);
      BOX_FINISH (bw, offset_avcC);

    } else if (atom == GST_MAKE_FOURCC ('t', 'm', 'c', 'd')) {
      GST_FIXME ("tmcd");
    } else if (atom == GST_MAKE_FOURCC ('a', 'p', 'c', 'h')) {
      GST_FIXME ("apch");
    } else if (atom == GST_MAKE_FOURCC ('A', 'V', '1', 'x')) {
      GST_FIXME ("AV1x");
    } else {
#if 1
      GST_WARNING ("unknown atom %" GST_FOURCC_FORMAT " inside stsd",
          GST_FOURCC_ARGS (atom));
#endif
    }

    BOX_FINISH (bw, offset_entry);
  }

  BOX_FINISH (bw, offset);
}

static void
gss_isom_stsz_serialize (GssBoxStsz * stsz, GstByteWriter * bw)
{
  int offset;
  int i;

  offset = BOX_INIT (bw, GST_MAKE_FOURCC ('s', 't', 's', 'z'));

  gst_byte_writer_put_uint8 (bw, stsz->version);
  gst_byte_writer_put_uint24_be (bw, stsz->flags);
  gst_byte_writer_put_uint32_be (bw, stsz->sample_size);
  gst_byte_writer_put_uint32_be (bw, stsz->sample_count);
  if (stsz->sample_size == 0) {
    for (i = 0; i < stsz->sample_count; i++) {
      gst_byte_writer_put_uint32_be (bw, stsz->sample_sizes[i]);
    }
  }

  BOX_FINISH (bw, offset);
}

#ifdef unused
static void
gss_isom_stsc_dump (GssBoxStsc * stsc)
{
  int i;

  g_print ("stsc:\n");
  g_print ("  entry_count: %d\n", stsc->entry_count);
  g_print ("    index: first_chunk samples_per_chunk desc_index\n");
  for (i = 0; i < stsc->entry_count; i++) {
    g_print ("    %d: %d %d %d\n", i,
        stsc->entries[i].first_chunk,
        stsc->entries[i].samples_per_chunk,
        stsc->entries[i].sample_description_index);
  }
}
#endif

static void
gss_isom_stsc_serialize (GssBoxStsc * stsc, GstByteWriter * bw)
{
  int offset;
  int i;

  offset = BOX_INIT (bw, GST_MAKE_FOURCC ('s', 't', 's', 'c'));

  gst_byte_writer_put_uint8 (bw, stsc->version);
  gst_byte_writer_put_uint24_be (bw, stsc->flags);
  gst_byte_writer_put_uint32_be (bw, stsc->entry_count);
  for (i = 0; i < stsc->entry_count; i++) {
    gst_byte_writer_put_uint32_be (bw, stsc->entries[i].first_chunk);
    gst_byte_writer_put_uint32_be (bw, stsc->entries[i].samples_per_chunk);
    gst_byte_writer_put_uint32_be (bw,
        stsc->entries[i].sample_description_index);
  }

  BOX_FINISH (bw, offset);
}

static void
gss_isom_stco_serialize (GssBoxStco * stco, GstByteWriter * bw)
{
  int offset;
  int i;

  //offset = BOX_INIT (bw, GST_MAKE_FOURCC ('c', 'o', '6', '4'));
  offset = BOX_INIT (bw, GST_MAKE_FOURCC ('s', 't', 'c', 'o'));

  gst_byte_writer_put_uint8 (bw, stco->version);
  gst_byte_writer_put_uint24_be (bw, stco->flags);
  gst_byte_writer_put_uint32_be (bw, stco->entry_count);
  for (i = 0; i < stco->entry_count; i++) {
    gst_byte_writer_put_uint32_be (bw, stco->chunk_offsets[i]);
  }

  BOX_FINISH (bw, offset);
}

static void
gss_isom_stdp_serialize (GssBoxStdp * stdp, GstByteWriter * bw,
    GssIsomTrack * track)
{
  int offset;
  int i;

  offset = BOX_INIT (bw, GST_MAKE_FOURCC ('s', 't', 'd', 'p'));

  gst_byte_writer_put_uint8 (bw, stdp->version);
  gst_byte_writer_put_uint24_be (bw, stdp->flags);
  for (i = 0; i < track->stsz.sample_count; i++) {
    gst_byte_writer_put_uint16_be (bw, stdp->priorities[i]);
  }

  BOX_FINISH (bw, offset);
}

static void
gss_isom_stsh_serialize (GssBoxStsh * stsh, GstByteWriter * bw)
{
  int offset;
  int i;

  offset = BOX_INIT (bw, GST_MAKE_FOURCC ('s', 't', 's', 'h'));

  gst_byte_writer_put_uint8 (bw, stsh->version);
  gst_byte_writer_put_uint24_be (bw, stsh->flags);
  gst_byte_writer_put_uint32_be (bw, stsh->entry_count);
  for (i = 0; i < stsh->entry_count; i++) {
    gst_byte_writer_put_uint32_be (bw, stsh->entries[i].shadowed_sample_number);
    gst_byte_writer_put_uint32_be (bw, stsh->entries[i].sync_sample_number);
  }

  BOX_FINISH (bw, offset);
}

#ifdef unused
static void
gss_isom_elst_serialize (GssBoxElst * elst, GstByteWriter * bw)
{
  int offset_elst;
  int i;

  offset_elst = BOX_INIT (bw, GST_MAKE_FOURCC ('e', 'l', 's', 't'));

  gst_byte_writer_put_uint8 (bw, elst->version);
  gst_byte_writer_put_uint24_be (bw, elst->flags);
  gst_byte_writer_put_uint32_be (bw, elst->entry_count);
  for (i = 0; i < elst->entry_count; i++) {
    if (elst->version == 0) {
      gst_byte_writer_put_uint32_be (bw, elst->entries[i].segment_duration);
      gst_byte_writer_put_uint32_be (bw, elst->entries[i].media_time);
    } else {
      gst_byte_writer_put_uint64_be (bw, elst->entries[i].segment_duration);
      gst_byte_writer_put_uint64_be (bw, elst->entries[i].media_time);
    }
    gst_byte_writer_put_uint32_be (bw, elst->entries[i].media_rate);
  }

  BOX_FINISH (bw, offset_elst);
}
#endif

void
gss_isom_track_dump (GssIsomTrack * track)
{
  /* trak */
  g_print ("  atom: %" GST_FOURCC_FORMAT "\n",
      GST_FOURCC_ARGS (GST_MAKE_FOURCC ('t', 'r', 'a', 'k')));

  g_print ("    ignore: tkhd\n");
  //gss_isom_tkhd_dump (&track->tkhd);
  g_print ("    ignore: tref\n");
  //gss_isom_tref_dump (&track->tref);

  /* trak/mdia */
  g_print ("    atom: %" GST_FOURCC_FORMAT "\n",
      GST_FOURCC_ARGS (GST_MAKE_FOURCC ('m', 'd', 'i', 'a')));
  g_print ("      ignore: mdhd\n");
  //gss_isom_mdhd_dump (&track->mdhd);
  g_print ("      ignore: hdlr\n");
  //gss_isom_hdlr_dump (&track->hdlr);

  /* trak/mdia/minf */
  g_print ("      atom: %" GST_FOURCC_FORMAT "\n",
      GST_FOURCC_ARGS (GST_MAKE_FOURCC ('m', 'i', 'n', 'f')));
  g_print ("        ignore: vmhd\n");
  //gss_isom_vmhd_dump (&track->vmhd);
  g_print ("        ignore: smhd\n");
  //gss_isom_smhd_dump (&track->smhd);
  g_print ("        ignore: hmhd\n");
  //gss_isom_hmhd_dump (&track->hmhd);

  /* trak/mdia/minf/dinf */
  g_print ("        atom: %" GST_FOURCC_FORMAT "\n",
      GST_FOURCC_ARGS (GST_MAKE_FOURCC ('d', 'i', 'n', 'f')));
  g_print ("          ignore: dref\n");
  //gss_isom_dref_dump (&track->dref);   /* */

  /* trak/mdia/minf/stbl */
  g_print ("        atom: %" GST_FOURCC_FORMAT "\n",
      GST_FOURCC_ARGS (GST_MAKE_FOURCC ('s', 't', 'b', 'l')));
  g_print ("    ignore: stsd\n");
  //gss_isom_stsd_dump (track);
  g_print ("    ignore: stts\n");
  //gss_isom_stts_dump (&track->stts);
  g_print ("    ignore: stsc\n");
  //gss_isom_stsc_dump (&track->stsc);
  g_print ("    ignore: stsz\n");
  //gss_isom_stsz_dump (&track->stsz);
  g_print ("    ignore: stco\n");
  //gss_isom_stco_dump (&track->stco);

#if 0
  if (0) {
    gss_isom_ctts_dump (&track->ctts);
    gss_isom_stss_dump (&track->stss);
    gss_isom_stsh_dump (&track->stsh);
    gss_isom_stdp_dump (&track->stdp, track);
  }
#endif

#if 0
  /* trak/mdia/minf/stsd */
  offset_stsd = BOX_INIT (bw, GST_MAKE_FOURCC ('s', 't', 's', 'd'));
  gss_isom_mp4v_dump (&track->mp4v);
  gss_isom_mp4a_dump (&track->mp4a);
  gss_isom_esds_dump (&track->esds);
#endif
}

static void
gss_isom_track_serialize (GssIsomTrack * track, GstByteWriter * bw)
{
  int offset;
  int offset_mdia;
  int offset_minf;
  //int offset_dinf;
  int offset_stbl;

  /* trak */
  offset = BOX_INIT (bw, GST_MAKE_FOURCC ('t', 'r', 'a', 'k'));

  gss_isom_tkhd_serialize (&track->tkhd, bw);

  gss_isom_tref_serialize (&track->tref, bw);

  /* trak/mdia */
  offset_mdia = BOX_INIT (bw, GST_MAKE_FOURCC ('m', 'd', 'i', 'a'));
  gss_isom_mdhd_serialize (&track->mdhd, bw);
  gss_isom_hdlr_serialize (&track->hdlr, bw);

  /* trak/mdia/minf */
  offset_minf = BOX_INIT (bw, GST_MAKE_FOURCC ('m', 'i', 'n', 'f'));

  /* trak/mdia/minf/stbl */
  offset_stbl = BOX_INIT (bw, GST_MAKE_FOURCC ('s', 't', 'b', 'l'));
  gss_isom_stsd_serialize (track, bw);
  gss_isom_stts_serialize (&track->stts, bw);
  gss_isom_stsc_serialize (&track->stsc, bw);
  gss_isom_stco_serialize (&track->stco, bw);
  gss_isom_stsz_serialize (&track->stsz, bw);

  if (0) {
    gss_isom_stss_serialize (&track->stss, bw);
    gss_isom_ctts_serialize (&track->ctts, bw);
    gss_isom_stsh_serialize (&track->stsh, bw);
    gss_isom_stdp_serialize (&track->stdp, bw, track);
  }
  BOX_FINISH (bw, offset_stbl);

  gss_isom_vmhd_serialize (&track->vmhd, bw);
  gss_isom_smhd_serialize (&track->smhd, bw);
  gss_isom_hmhd_serialize (&track->hmhd, bw);

  BOX_FINISH (bw, offset_minf);
  BOX_FINISH (bw, offset_mdia);
  BOX_FINISH (bw, offset);
}

static void
gss_isom_mvhd_dump (GssBoxMvhd * mvhd)
{
  g_print ("  atom: mvhd\n");
  g_print ("    version: 0x%08x\n", mvhd->version);
  g_print ("    flags: 0x%08x\n", mvhd->flags);
  g_print ("    creation_time: 0x%08lx\n", mvhd->creation_time);
  g_print ("    modification_time: 0x%08lx\n", mvhd->modification_time);
  g_print ("    timescale: %d\n", mvhd->timescale);
  g_print ("    duration: %ld\n", mvhd->duration);

  g_print ("    unknown: 0x%08x\n", 0x00010000);
  g_print ("    unknown: 0x%08x\n", 0x0100);
  g_print ("    unknown: 0x%08x\n", 0x0000);

  /* matrix */
  g_print ("    matrix[0]: 0x%08x\n", 0x00000000);
  g_print ("    matrix[0]: 0x%08x\n", 0x00000000);
  g_print ("    matrix[0]: 0x%08x\n", 0x00010000);
  g_print ("    matrix[0]: 0x%08x\n", 0x00000000);
  g_print ("    matrix[0]: 0x%08x\n", 0x00000000);
  g_print ("    matrix[0]: 0x%08x\n", 0x00000000);
  g_print ("    matrix[0]: 0x%08x\n", 0x00010000);
  g_print ("    matrix[0]: 0x%08x\n", 0x00000000);
  g_print ("    matrix[0]: 0x%08x\n", 0x00000000);

  g_print ("    unknown: 0x%08x\n", 0x00000000);
  g_print ("    unknown: 0x%08x\n", 0x40000000);
  g_print ("    unknown: 0x%08x\n", 0x00000000);
  g_print ("    unknown: 0x%08x\n", 0x00000000);
  g_print ("    unknown: 0x%08x\n", 0x00000000);
  g_print ("    unknown: 0x%08x\n", 0x00000000);

  g_print ("    track_id: %d\n", mvhd->next_track_id);

  /* FIXME for ccff */
  g_print ("    unknown: 0x%08x\n", 0x00000000);
  g_print ("    unknown: 0x%08x\n", 0xffffffff);

}

static void
gss_isom_mvhd_serialize (GssBoxMvhd * mvhd, GstByteWriter * bw)
{
  int offset;
  offset = BOX_INIT (bw, GST_MAKE_FOURCC ('m', 'v', 'h', 'd'));

  gst_byte_writer_put_uint8 (bw, mvhd->version);
  gst_byte_writer_put_uint24_be (bw, mvhd->flags);
  if (mvhd->version == 1) {
    gst_byte_writer_put_uint64_be (bw, mvhd->creation_time);
    gst_byte_writer_put_uint64_be (bw, mvhd->modification_time);
    gst_byte_writer_put_uint32_be (bw, mvhd->timescale);
    gst_byte_writer_put_uint64_be (bw, mvhd->duration);
  } else {
    gst_byte_writer_put_uint32_be (bw, mvhd->creation_time);
    gst_byte_writer_put_uint32_be (bw, mvhd->modification_time);
    gst_byte_writer_put_uint32_be (bw, mvhd->timescale);
    gst_byte_writer_put_uint32_be (bw, mvhd->duration);
  }

  gst_byte_writer_put_uint32_be (bw, 0x00010000);
  gst_byte_writer_put_uint16_be (bw, 0x0100);
  gst_byte_writer_put_uint16_be (bw, 0x0000);

  /* matrix */
  gst_byte_writer_put_uint32_be (bw, 0x00000000);
  gst_byte_writer_put_uint32_be (bw, 0x00000000);
  gst_byte_writer_put_uint32_be (bw, 0x00010000);
  gst_byte_writer_put_uint32_be (bw, 0x00000000);
  gst_byte_writer_put_uint32_be (bw, 0x00000000);
  gst_byte_writer_put_uint32_be (bw, 0x00000000);
  gst_byte_writer_put_uint32_be (bw, 0x00010000);
  gst_byte_writer_put_uint32_be (bw, 0x00000000);
  gst_byte_writer_put_uint32_be (bw, 0x00000000);

  gst_byte_writer_put_uint32_be (bw, 0x00000000);
  gst_byte_writer_put_uint32_be (bw, 0x40000000);
  gst_byte_writer_put_uint32_be (bw, 0x00000000);
  gst_byte_writer_put_uint32_be (bw, 0x00000000);
  gst_byte_writer_put_uint32_be (bw, 0x00000000);
  gst_byte_writer_put_uint32_be (bw, 0x00000000);

  gst_byte_writer_put_uint32_be (bw, mvhd->next_track_id);

#if 0
  /* FIXME for ccff */
  gst_byte_writer_put_uint32_be (bw, 0x00000000);
  gst_byte_writer_put_uint32_be (bw, 0xffffffff);
#endif
#if 1
  /* FIXME for dash */
  gst_byte_writer_put_uint32_be (bw, 0x00000000);
  gst_byte_writer_put_uint32_be (bw, 0x00000002);
#endif

  BOX_FINISH (bw, offset);
}

static void
gss_isom_ainf_serialize (GssBoxStore * ainf, GstByteWriter * bw)
{
  int offset;
  if (!ainf->present)
    return;
  offset = BOX_INIT (bw, GST_MAKE_FOURCC ('a', 'i', 'n', 'f'));

  gst_byte_writer_put_data (bw, ainf->data, ainf->size);

  BOX_FINISH (bw, offset);
}

static void
gss_isom_iods_serialize (GssBoxStore * iods, GstByteWriter * bw)
{
  int offset;

  if (!iods->present)
    return;
  offset = BOX_INIT (bw, GST_MAKE_FOURCC ('i', 'o', 'd', 's'));

  gst_byte_writer_put_data (bw, iods->data, iods->size);

  BOX_FINISH (bw, offset);
}

static void
gss_isom_mehd_serialize (GssBoxMehd * mehd, GstByteWriter * bw)
{
  int offset;
  offset = BOX_INIT (bw, GST_MAKE_FOURCC ('m', 'e', 'h', 'd'));
  gst_byte_writer_put_uint8 (bw, mehd->version);
  gst_byte_writer_put_uint24_be (bw, mehd->flags);
  if (mehd->version == 0) {
    gst_byte_writer_put_uint32_be (bw, mehd->fragment_duration);
  } else {
    gst_byte_writer_put_uint64_be (bw, mehd->fragment_duration);
  }
  BOX_FINISH (bw, offset);
}

static void
gss_isom_trex_serialize (GssBoxTrex * trex, GstByteWriter * bw)
{
  int offset;

  offset = BOX_INIT (bw, GST_MAKE_FOURCC ('t', 'r', 'e', 'x'));
  gst_byte_writer_put_uint8 (bw, trex->version);
  gst_byte_writer_put_uint24_be (bw, trex->flags);
  gst_byte_writer_put_uint32_be (bw, trex->track_id);
  gst_byte_writer_put_uint32_be (bw, trex->default_sample_description_index);
  gst_byte_writer_put_uint32_be (bw, trex->default_sample_duration);
  gst_byte_writer_put_uint32_be (bw, trex->default_sample_size);
  gst_byte_writer_put_uint32_be (bw, trex->default_sample_flags);
  BOX_FINISH (bw, offset);
}

static void
fixup_track (GssIsomTrack * track, gboolean is_video)
{
  track->tkhd.creation_time += 0;
  track->tkhd.modification_time += 0;

  track->tkhd.duration = 0;

  track->mdhd.version = 1;
  track->mdhd.timescale = 10000000;
  track->mdhd.duration = 0;
  //track->stsd.entry_count = 0;
  track->stsz.sample_count = 0;
  track->stsc.entry_count = 0;
  track->stco.entry_count = 0;
  track->stts.entry_count = 0;
}

static void
gss_isom_moov_serialize (GssIsomMovie * movie, GstByteWriter * bw)
{
  int offset;
  int offset_mvex;
  int offset_2;
  int i;

  offset = BOX_INIT (bw, GST_MAKE_FOURCC ('m', 'o', 'o', 'v'));

  gss_isom_mvhd_serialize (&movie->mvhd, bw);
#if 1
  /* mvex */
  offset_mvex = BOX_INIT (bw, GST_MAKE_FOURCC ('m', 'v', 'e', 'x'));
#if 0
  offset_2 = BOX_INIT (bw, GST_MAKE_FOURCC ('m', 'e', 'h', 'd'));
  gst_byte_writer_put_uint32_be (bw, 0x01000000);
  gst_byte_writer_put_uint32_be (bw, 0x00000000);
  gst_byte_writer_put_uint32_be (bw, 0x1f1e5c05);
  BOX_FINISH (bw, offset_2);
#endif
  for (i = 0; i < movie->n_tracks; i++) {
    offset_2 = BOX_INIT (bw, GST_MAKE_FOURCC ('t', 'r', 'e', 'x'));
    gst_byte_writer_put_uint32_be (bw, 0x00000000);
    gst_byte_writer_put_uint32_be (bw, movie->tracks[i]->tkhd.track_id);
    gst_byte_writer_put_uint32_be (bw, 0x00000001);
    gst_byte_writer_put_uint32_be (bw, 0x00000000);
    gst_byte_writer_put_uint32_be (bw, 0x00000000);
    gst_byte_writer_put_uint32_be (bw, 0x00000000);
    BOX_FINISH (bw, offset_2);
  }
  BOX_FINISH (bw, offset_mvex);
#endif

  if (0)
    gss_isom_ainf_serialize (&movie->ainf, bw);
  if (0)
    gss_isom_iods_serialize (&movie->iods, bw);
  for (i = 0; i < movie->n_tracks; i++) {
    gss_isom_track_serialize (movie->tracks[i], bw);
  }

  /* mvex */
  if (movie->mvex.present) {
    offset_mvex = BOX_INIT (bw, GST_MAKE_FOURCC ('m', 'v', 'e', 'x'));
    offset_2 = BOX_INIT (bw, GST_MAKE_FOURCC ('m', 'e', 'h', 'd'));
    gst_byte_writer_put_uint8 (bw, movie->mehd.version);
    gst_byte_writer_put_uint24_be (bw, movie->mehd.flags);
    if (movie->mehd.version == 0) {
      gst_byte_writer_put_uint32_be (bw, movie->mehd.fragment_duration);
    } else {
      gst_byte_writer_put_uint64_be (bw, movie->mehd.fragment_duration);
    }
    BOX_FINISH (bw, offset_2);
    for (i = 0; i < movie->n_tracks; i++) {
      GssIsomTrack *track = movie->tracks[i];

      offset_2 = BOX_INIT (bw, GST_MAKE_FOURCC ('t', 'r', 'e', 'x'));
      gst_byte_writer_put_uint8 (bw, track->trex.version);
      gst_byte_writer_put_uint24_be (bw, track->trex.flags);
      gst_byte_writer_put_uint32_be (bw, track->trex.track_id);
      gst_byte_writer_put_uint32_be (bw,
          track->trex.default_sample_description_index);
      gst_byte_writer_put_uint32_be (bw, track->trex.default_sample_duration);
      gst_byte_writer_put_uint32_be (bw, track->trex.default_sample_size);
      gst_byte_writer_put_uint32_be (bw, track->trex.default_sample_flags);
      BOX_FINISH (bw, offset_2);
    }
    BOX_FINISH (bw, offset_mvex);
  }

  BOX_FINISH (bw, offset);
}

static void
gss_isom_moov_dump (GssIsomMovie * movie)
{
  int i;

  g_print ("atom: moov\n");
  gss_isom_mvhd_dump (&movie->mvhd);
#if 0
  gss_isom_ainf_dump (&movie->ainf);
  gss_isom_iods_dump (&movie->iods);
#endif
  for (i = 0; i < movie->n_tracks; i++) {
    gss_isom_track_dump (movie->tracks[i]);
  }
}

void
gss_isom_movie_dump (GssIsomMovie * movie)
{
  g_print ("atom: ftyp\n");
  g_print ("  file type: %" GST_FOURCC_FORMAT "\n",
      GST_FOURCC_ARGS (GST_MAKE_FOURCC ('c', 'c', 'f', 'f')));
  g_print ("  unknown: %d\n", 0x00000001);
  g_print ("  compat: %" GST_FOURCC_FORMAT "\n",
      GST_FOURCC_ARGS (GST_MAKE_FOURCC ('i', 's', 'o', '6')));

  gss_isom_moov_dump (movie);

}

void
create_sidx (GssBoxSidx * sidx, GssIsomTrack * track)
{
  int i;

  memset (sidx, 0, sizeof (GssBoxSidx));

  sidx->version = 0;
  sidx->flags = 0;
  sidx->reference_id = 1;       /* FIXME wut? */
  sidx->timescale = track->mdhd.timescale;
  sidx->earliest_presentation_time = 0;
  sidx->first_offset = 0;

  sidx->n_entries = track->n_fragments;
  sidx->entries = g_malloc0 (sizeof (GssBoxSidxEntry) * sidx->n_entries);
  for (i = 0; i < sidx->n_entries; i++) {
    sidx->entries[i].reference_size = track->fragments[i]->moof_size +
        track->fragments[i]->mdat_size;
    sidx->entries[i].subsegment_duration = track->fragments[i]->duration;
    /* FIXME fixup */
    if (i < sidx->n_entries - 1) {
      sidx->entries[i].subsegment_duration -= 30;
    }
    sidx->entries[i].starts_with_sap = 1;
    sidx->entries[i].sap_type = 1;
    sidx->entries[i].sap_delta_time = 0;
  }
}

/* For isoff-on-demand profile */
void
gss_isom_track_serialize_dash (GssIsomTrack * track, guint8 ** data, int *size)
{
  GstByteWriter *bw;
  int offset;
  int offset_mvex;
  guint32 ftyp;
  gboolean is_video;
  GssBoxMvhd mvhd;

  is_video = (track->hdlr.handler_type == GST_MAKE_FOURCC ('v', 'i', 'd', 'e'));

  bw = gst_byte_writer_new ();

  ftyp = GST_MAKE_FOURCC ('d', 'a', 's', 'h');

  offset = BOX_INIT (bw, GST_MAKE_FOURCC ('f', 't', 'y', 'p'));
  gst_byte_writer_put_uint32_le (bw, ftyp);
  switch (ftyp) {
    case GST_MAKE_FOURCC ('c', 'c', 'f', 'f'):
      gst_byte_writer_put_uint32_be (bw, 0x00000001);
      gst_byte_writer_put_uint32_le (bw, GST_MAKE_FOURCC ('i', 's', 'o', '6'));
      break;
    case GST_MAKE_FOURCC ('d', 'a', 's', 'h'):
      gst_byte_writer_put_uint32_be (bw, 0x00000000);
      gst_byte_writer_put_uint32_le (bw, GST_MAKE_FOURCC ('i', 's', 'o', '6'));
      gst_byte_writer_put_uint32_le (bw, GST_MAKE_FOURCC ('a', 'v', 'c', '1'));
      gst_byte_writer_put_uint32_le (bw, GST_MAKE_FOURCC ('m', 'p', '4', '1'));
      break;
    default:
      g_assert (0);
      break;
  }
  BOX_FINISH (bw, offset);

  /* moov */
  offset = BOX_INIT (bw, GST_MAKE_FOURCC ('m', 'o', 'o', 'v'));
  memset (&mvhd, 0, sizeof (mvhd));
  mvhd.timescale = track->mdhd.timescale;
  gss_isom_mvhd_serialize (&mvhd, bw);

  /* mvex */
  offset_mvex = BOX_INIT (bw, GST_MAKE_FOURCC ('m', 'v', 'e', 'x'));
  //gss_isom_mehd_serialize (&movie->mvhd, bw);
  gss_isom_trex_serialize (&track->trex, bw);
  BOX_FINISH (bw, offset_mvex);

  gss_isom_track_serialize (track, bw);

  BOX_FINISH (bw, offset);

  {
    GssBoxSidx sidx;
    create_sidx (&sidx, track);
    gss_isom_sidx_serialize (&sidx, bw);
  }

  if (1) {
    GssIsomFragment *fragment = track->fragments[0];
    int offset_moof;

    offset_moof = BOX_INIT (bw, GST_MAKE_FOURCC ('m', 'o', 'o', 'f'));
    gss_isom_mfhd_serialize (&fragment->mfhd, bw);
    gss_isom_traf_serialize (fragment, bw, is_video);
    BOX_FINISH (bw, offset_moof);

    GST_WRITE_UINT32_BE ((void *) (bw->parent.data +
            fragment->trun.data_offset_fixup),
        bw->parent.byte + 8 - offset_moof);
  }

  *size = bw->parent.byte;
  *data = gst_byte_writer_free_and_get_data (bw);
}

/* This is only meant for isoff-live profile, from a movie that is still in
 * mp4 form. */
void
gss_isom_movie_serialize_track_ccff (GssIsomMovie * movie, GssIsomTrack * track,
    guint8 ** data, gsize * size)
{
  GstByteWriter *bw;
  int offset;
  guint32 ftyp;
  int offset_moov;

  bw = gst_byte_writer_new ();

  ftyp = GST_MAKE_FOURCC ('c', 'c', 'f', 'f');

  offset = BOX_INIT (bw, GST_MAKE_FOURCC ('f', 't', 'y', 'p'));
  gst_byte_writer_put_uint32_le (bw, ftyp);
  gst_byte_writer_put_uint32_be (bw, 0x00000001);
  gst_byte_writer_put_uint32_le (bw, GST_MAKE_FOURCC ('i', 's', 'o', '6'));
  BOX_FINISH (bw, offset);

  offset_moov = BOX_INIT (bw, GST_MAKE_FOURCC ('m', 'o', 'o', 'v'));

  gss_isom_mvhd_serialize (&movie->mvhd, bw);
  gss_isom_track_serialize (track, bw);

  {
    int offset_mvex;
    //int offset_2;
    /* mvex */
    offset_mvex = BOX_INIT (bw, GST_MAKE_FOURCC ('m', 'v', 'e', 'x'));
    gss_isom_mehd_serialize (&movie->mehd, bw);
    gss_isom_trex_serialize (&track->trex, bw);
    BOX_FINISH (bw, offset_mvex);
  }

  BOX_FINISH (bw, offset_moov);

  *size = bw->parent.byte;
  *data = gst_byte_writer_free_and_get_data (bw);
}

void
gss_isom_movie_serialize_track_dash (GssIsomMovie * movie, GssIsomTrack * track,
    guint8 ** data, gsize * header_size, gsize * size)
{
  GstByteWriter *bw;
  int offset;
  int offset_moov;

  bw = gst_byte_writer_new ();

  offset = BOX_INIT (bw, GST_MAKE_FOURCC ('f', 't', 'y', 'p'));
  gst_byte_writer_put_uint32_le (bw, GST_MAKE_FOURCC ('d', 'a', 's', 'h'));
  gst_byte_writer_put_uint32_be (bw, 0x00000000);
  gst_byte_writer_put_uint32_le (bw, GST_MAKE_FOURCC ('i', 's', 'o', '6'));
  gst_byte_writer_put_uint32_le (bw, GST_MAKE_FOURCC ('a', 'v', 'c', '1'));
  gst_byte_writer_put_uint32_le (bw, GST_MAKE_FOURCC ('m', 'p', '4', '1'));
  BOX_FINISH (bw, offset);

  offset_moov = BOX_INIT (bw, GST_MAKE_FOURCC ('m', 'o', 'o', 'v'));

  gss_isom_mvhd_serialize (&movie->mvhd, bw);
  gss_isom_track_serialize (track, bw);

  {
    int offset_mvex;
    //int offset_2;
    /* mvex */
    offset_mvex = BOX_INIT (bw, GST_MAKE_FOURCC ('m', 'v', 'e', 'x'));
    gss_isom_mehd_serialize (&movie->mehd, bw);
    gss_isom_trex_serialize (&track->trex, bw);
    BOX_FINISH (bw, offset_mvex);
  }

  BOX_FINISH (bw, offset_moov);

  *header_size = bw->parent.byte;

  {
    GssBoxSidx sidx;
    create_sidx (&sidx, track);
    gss_isom_sidx_serialize (&sidx, bw);
    g_free (sidx.entries);
  }

  *size = bw->parent.byte;
  *data = gst_byte_writer_free_and_get_data (bw);
}

void
gss_isom_movie_serialize (GssIsomMovie * movie, guint8 ** data, int *size)
{
  GstByteWriter *bw;
  int offset;

  bw = gst_byte_writer_new ();

  offset = BOX_INIT (bw, GST_MAKE_FOURCC ('f', 't', 'y', 'p'));
  gst_byte_writer_put_uint32_le (bw, GST_MAKE_FOURCC ('d', 'a', 's', 'h'));
  gst_byte_writer_put_uint32_be (bw, 0x00000000);
  gst_byte_writer_put_uint32_le (bw, GST_MAKE_FOURCC ('i', 's', 'o', '6'));
  gst_byte_writer_put_uint32_le (bw, GST_MAKE_FOURCC ('a', 'v', 'c', '1'));
  gst_byte_writer_put_uint32_le (bw, GST_MAKE_FOURCC ('m', 'p', '4', '1'));
  BOX_FINISH (bw, offset);

  gss_isom_moov_serialize (movie, bw);

  gss_isom_sidx_serialize (&movie->sidx, bw);

  *size = bw->parent.byte;
  *data = gst_byte_writer_free_and_get_data (bw);
}

int
gss_isom_fragment_get_n_samples (GssIsomFragment * fragment)
{
  return fragment->trun.sample_count;
}

int *
gss_isom_fragment_get_sample_sizes (GssIsomFragment * fragment)
{
  int *s;
  int i;
  GssBoxTrun *trun = &fragment->trun;

  s = g_malloc (sizeof (int) * trun->sample_count);
  for (i = 0; i < trun->sample_count; i++) {
    s[i] = trun->samples[i].size;
  }
  return s;
}

GssIsomTrack *
gss_isom_movie_get_video_track (GssIsomMovie * movie)
{
  int i;
  g_return_val_if_fail (movie != NULL, NULL);
  for (i = 0; i < movie->n_tracks; i++) {
    if (movie->tracks[i]->hdlr.handler_type ==
        GST_MAKE_FOURCC ('v', 'i', 'd', 'e')) {
      return movie->tracks[i];
    }
  }
  return NULL;
}

GssIsomTrack *
gss_isom_movie_get_audio_track (GssIsomMovie * movie)
{
  int i;
  g_return_val_if_fail (movie != NULL, NULL);
  for (i = 0; i < movie->n_tracks; i++) {
    if (movie->tracks[i]->hdlr.handler_type ==
        GST_MAKE_FOURCC ('s', 'o', 'u', 'n')) {
      return movie->tracks[i];
    }
  }
  return NULL;
}

void
gss_isom_parser_fragmentize (GssIsomParser * file)
{
  GssIsomTrack *video_track;
  GssIsomTrack *audio_track;
  int n_fragments;
  int i;
  guint64 video_timestamp = 0;
  guint64 audio_timestamp = 0;
  guint64 video_timescale_ts = 0;
  guint64 audio_timescale_ts = 0;
  int audio_index = 0;
  int audio_index_end;
  GssIsomSampleIterator video_iter;
  GssIsomSampleIterator audio_iter;

  video_track = gss_isom_movie_get_video_track (file->movie);
  if (video_track == NULL) {
    GST_ERROR ("no video track");
    return;
  }

  if (video_track->stsz.sample_count == 0) {
    GST_ERROR ("sample_count == 0, already fragmented?");
    return;
  }

  if (!video_track->stss.present) {
    GST_ERROR ("no stss atom in video track, something wrong");
    return;
  }

  audio_track = gss_isom_movie_get_audio_track (file->movie);
  if (audio_track == NULL) {
    GST_ERROR ("no audio track");
    return;
  }

  video_track->filename = file->filename;
  audio_track->filename = file->filename;

  video_track->tkhd.track_id = 1;
  video_track->trex.track_id = 1;
  video_track->trex.default_sample_description_index = 1;
  audio_track->tkhd.track_id = 2;
  audio_track->trex.track_id = 2;
  audio_track->trex.default_sample_description_index = 1;

  n_fragments = video_track->stss.entry_count;

  video_track->fragments = g_malloc0 (sizeof (GssIsomFragment *) * n_fragments);
  video_track->n_fragments = n_fragments;
  video_track->n_fragments_alloc = n_fragments;

  audio_track->fragments = g_malloc0 (sizeof (GssIsomFragment *) * n_fragments);
  audio_track->n_fragments = n_fragments;
  audio_track->n_fragments_alloc = n_fragments;

  gss_isom_sample_iter_init (&video_iter, video_track);
  gss_isom_sample_iter_init (&audio_iter, audio_track);

  for (i = 0; i < n_fragments; i++) {
    GssIsomFragment *audio_fragment;
    GssIsomFragment *video_fragment;
    guint64 n_samples;
    GssBoxTrunSample *samples;
    int sample_offset;
    int j;

    audio_fragment = gss_isom_fragment_new ();
    audio_track->fragments[i] = audio_fragment;
    audio_fragment->index = i;
    video_fragment = gss_isom_fragment_new ();
    video_track->fragments[i] = video_fragment;
    video_fragment->index = i;

    video_fragment->mfhd.sequence_number = i;

    video_fragment->tfhd.track_id = video_track->tkhd.track_id;
    video_fragment->tfhd.flags = 0;
    video_fragment->tfhd.default_sample_duration = 0x061a80;
    video_fragment->tfhd.default_sample_size = 0;
    video_fragment->tfhd.default_sample_flags = 0x000100c0;

    video_fragment->tfdt.version = 1;

    video_fragment->trun.version = 1;

    sample_offset = video_track->stss.sample_numbers[i] - 1;
    if (i == n_fragments - 1) {
      n_samples = gss_isom_track_get_n_samples (video_track) - sample_offset;
    } else {
      n_samples = (video_track->stss.sample_numbers[i + 1] - 1) - sample_offset;
    }
    video_fragment->trun.sample_count = n_samples;
    video_fragment->trun.data_offset = 12;

    video_fragment->mdat_size = 8;
    video_fragment->n_mdat_chunks = n_samples;
    video_fragment->chunks = g_malloc (sizeof (GssMdatChunk) * n_samples);

    video_fragment->sdtp.sample_flags = g_malloc0 (sizeof (guint8) * n_samples);
    video_fragment->sdtp.sample_flags[0] = 0x14;
    for (j = 1; j < n_samples; j++) {
      video_fragment->sdtp.sample_flags[j] = 0x1c;
    }

    samples = g_malloc0 (sizeof (GssBoxTrunSample) * n_samples);
    video_fragment->timestamp = video_timestamp;
    video_fragment->tfdt.start_time = video_timestamp;
    for (j = 0; j < n_samples; j++) {
      GssIsomSample sample;
      guint64 next_timestamp;

      gss_isom_sample_iter_get_sample (&video_iter, &sample);

      video_timescale_ts += sample.duration;
      next_timestamp = gst_util_uint64_scale_int (video_timescale_ts,
          10000000, video_track->mdhd.timescale);
      samples[j].duration = next_timestamp - video_timestamp;
      video_timestamp = next_timestamp;
      samples[j].size = sample.size;
      samples[j].flags = 0;
      samples[j].composition_time_offset =
          gst_util_uint64_scale_int (sample.composition_time_offset +
          video_track->mdhd.timescale, 10000000,
          video_track->mdhd.timescale) - 10000000;

      video_fragment->chunks[j].offset = sample.offset;
      video_fragment->chunks[j].size = sample.size;
      video_fragment->mdat_size += sample.size;

      gss_isom_sample_iter_iterate (&video_iter);
    }
    video_fragment->trun.samples = samples;
    /* FIXME not all strictly necessary, should be handled in serializer */
    video_fragment->trun.flags =
        TR_SAMPLE_SIZE | TR_SAMPLE_DURATION | TR_DATA_OFFSET |
        TR_SAMPLE_COMPOSITION_TIME_OFFSETS;
    video_fragment->trun.flags = 0x0b01;
    video_fragment->trun.first_sample_flags = 0x40;
    video_fragment->duration = video_timestamp - video_fragment->timestamp;

    audio_fragment->mfhd.sequence_number = i;

    audio_index_end = gss_isom_track_get_index_from_timestamp (audio_track,
        gst_util_uint64_scale_int (video_timestamp,
            audio_track->mdhd.timescale, 10000000));

    audio_fragment->tfhd.track_id = audio_track->tkhd.track_id;
    audio_fragment->tfhd.flags = 0;
    audio_fragment->tfhd.default_sample_duration = 0;
    audio_fragment->tfhd.default_sample_size = 0;
    audio_fragment->tfhd.default_sample_flags = 0xc0;

    n_samples = audio_index_end - audio_index;

    audio_fragment->mdat_size = 8;
    audio_fragment->n_mdat_chunks = n_samples;
    audio_fragment->chunks = g_malloc (sizeof (GssMdatChunk) * n_samples);

    audio_fragment->trun.sample_count = n_samples;
    audio_fragment->trun.data_offset = 12;
    samples = g_malloc0 (sizeof (GssBoxTrunSample) * n_samples);

    audio_fragment->sdtp.sample_flags =
        g_malloc0 (sizeof (guint32) * n_samples);

    audio_fragment->timestamp = audio_timestamp;
    audio_fragment->tfdt.start_time = audio_timestamp;
    for (j = 0; j < n_samples; j++) {
      GssIsomSample sample;
      guint64 next_timestamp;

      gss_isom_sample_iter_get_sample (&audio_iter, &sample);

      audio_timescale_ts += sample.duration;
      next_timestamp = gst_util_uint64_scale_int (audio_timescale_ts,
          10000000, audio_track->mdhd.timescale);
      samples[j].duration = next_timestamp - audio_timestamp;
      audio_timestamp = next_timestamp;

      samples[j].size = sample.size;
      samples[j].flags = 0;
      samples[j].composition_time_offset = sample.composition_time_offset;

      audio_fragment->chunks[j].offset = sample.offset;
      audio_fragment->chunks[j].size = sample.size;
      audio_fragment->mdat_size += sample.size;

      gss_isom_sample_iter_iterate (&audio_iter);
    }
    audio_fragment->trun.samples = samples;
    audio_fragment->trun.version = 1;
    /* FIXME not all strictly necessary, should be handled in serializer */
    audio_fragment->trun.flags =
        TR_SAMPLE_DURATION | TR_SAMPLE_SIZE | TR_DATA_OFFSET;
    audio_fragment->duration = audio_timestamp - audio_fragment->timestamp;

    audio_index += n_samples;
  }

  file->movie->mvhd.timescale = 10000000;
  fixup_track (video_track, TRUE);
  fixup_track (audio_track, FALSE);

  file->movie->mehd.version = 1;
  /* FIXME */
  file->movie->mehd.fragment_duration = video_timestamp;
  file->movie->mvhd.duration = video_timestamp;

}

void
gss_isom_track_prepare_streaming (GssIsomMovie * movie, GssIsomTrack * track)
{
  int i;
  guint64 offset = 0;
  gboolean is_video;

  is_video = (track->hdlr.handler_type == GST_MAKE_FOURCC ('v', 'i', 'd', 'e'));


  for (i = 0; i < track->n_fragments; i++) {
    GssIsomFragment *fragment = track->fragments[i];

    fragment->offset = offset;
    gss_isom_fragment_serialize (fragment, &fragment->moof_data,
        &fragment->moof_size, is_video);
    offset += fragment->moof_size;
    offset += fragment->mdat_size;
  }
  track->dash_size = offset;

  gss_isom_movie_serialize_track_ccff (movie, track,
      &track->ccff_header_data, &track->ccff_header_size);

  gss_isom_movie_serialize_track_dash (movie, track,
      &track->dash_header_data, &track->dash_header_size,
      &track->dash_header_and_sidx_size);

  track->dash_size += track->dash_header_and_sidx_size;
}

int
gss_isom_track_get_index_from_timestamp (GssIsomTrack * track,
    guint64 timestamp)
{
  int i;
  int offset;
  guint64 ts;
  GssBoxSttsEntry *entries;

  ts = 0;
  offset = 0;
  entries = track->stts.entries;
  for (i = 0; i < track->stts.entry_count; i++) {
    if (timestamp - ts >= entries[i].sample_count * entries[i].sample_delta) {
      ts += entries[i].sample_count * entries[i].sample_delta;
      offset += entries[i].sample_count;
    } else {
      offset += (timestamp - ts) / entries[i].sample_delta;
      return offset;
    }
  }

  return track->stsz.sample_count;
}

void
gss_isom_track_get_sample (GssIsomTrack * track, GssIsomSample * sample,
    int sample_index)
{
  int i;
  int offset;
  int chunk_index;
  int index_in_chunk;

  offset = 0;
  sample->duration = 0;
  for (i = 0; i < track->stts.entry_count; i++) {
    if (sample_index < offset + track->stts.entries[i].sample_count) {
      sample->duration = track->stts.entries[i].sample_delta;
      break;
    }
    offset += track->stts.entries[i].sample_count;
  }

  if (track->stsz.sample_size == 0) {
    sample->size = track->stsz.sample_sizes[sample_index];
  } else {
    sample->size = track->stsz.sample_size;
  }

  offset = 0;
  sample->composition_time_offset = 0;
  for (i = 0; i < track->ctts.entry_count; i++) {
    if (sample_index < offset + track->ctts.entries[i].sample_count) {
      sample->composition_time_offset = track->ctts.entries[i].sample_offset;
      break;
    }
    offset += track->ctts.entries[i].sample_count;
  }

  offset = 0;
  chunk_index = 0;
  index_in_chunk = 0;
  GST_ERROR ("looking up sample_index %d", sample_index);
  for (i = 0; i < track->stsc.entry_count; i++) {
    int n_chunks;

    if (i == track->stsc.entry_count - 1) {
      /* don't actually care */
      n_chunks = 0;
    } else {
      n_chunks = track->stsc.entries[i + 1].first_chunk -
          track->stsc.entries[i].first_chunk;
    }
    chunk_index = (track->stsc.entries[i].first_chunk - 1);
    index_in_chunk = sample_index - offset;
    chunk_index += index_in_chunk / track->stsc.entries[i].samples_per_chunk;
    index_in_chunk = index_in_chunk % track->stsc.entries[i].samples_per_chunk;

    offset += track->stsc.entries[i].samples_per_chunk * n_chunks;
  }

  /* FIXME */
  if (index_in_chunk != 0) {
    GST_ERROR ("fixme index_in_chunk != 0 (%d)", index_in_chunk);
  }

  sample->offset = track->stco.chunk_offsets[chunk_index];
}

void
gss_isom_sample_iter_init (GssIsomSampleIterator * iter, GssIsomTrack * track)
{
  memset (iter, 0, sizeof (*iter));
  iter->track = track;
}

gboolean
gss_isom_sample_iter_iterate (GssIsomSampleIterator * iter)
{
  GssIsomTrack *track = iter->track;

  iter->index_in_stts++;
  if (iter->index_in_stts >= track->stts.entries[iter->stts_index].sample_count) {
    iter->index_in_stts = 0;
    iter->stts_index++;
  }

  if (track->ctts.present) {
    iter->index_in_ctts++;
    if (iter->index_in_ctts >=
        track->ctts.entries[iter->ctts_index].sample_count) {
      iter->index_in_ctts = 0;
      iter->ctts_index++;
    }
  }

  iter->index_in_chunk++;
  if (track->stsz.sample_size > 0) {
    iter->offset_in_chunk += track->stsz.sample_size;
  } else {
    iter->offset_in_chunk += track->stsz.sample_sizes[iter->sample_index];
  }
  if (iter->index_in_chunk >=
      track->stsc.entries[iter->stsc_index].samples_per_chunk) {
    iter->chunk_index++;
    iter->index_in_chunk = 0;
    iter->offset_in_chunk = 0;
  }
  if (iter->stsc_index < track->stsc.entry_count - 1 &&
      iter->chunk_index >=
      track->stsc.entries[iter->stsc_index + 1].first_chunk - 1) {
    iter->stsc_index++;
  }

  iter->sample_index++;
  if (iter->sample_index >= track->stsz.sample_count) {
    return FALSE;
  }

  return TRUE;
}

void
gss_isom_sample_iter_get_sample (GssIsomSampleIterator * iter,
    GssIsomSample * sample)
{
  GssIsomTrack *track = iter->track;

  /* duration, size, flags, composition_time_offset, offset */

  sample->duration = track->stts.entries[iter->stts_index].sample_delta;

  if (track->stsz.sample_size > 0) {
    g_assert (0);
    sample->size = track->stsz.sample_size;
  } else {
    sample->size = track->stsz.sample_sizes[iter->sample_index];
  }

  if (track->ctts.present) {
    sample->composition_time_offset =
        track->ctts.entries[iter->ctts_index].sample_offset;
  } else {
    sample->composition_time_offset = 0;
  }

  sample->offset = track->stco.chunk_offsets[iter->chunk_index] +
      iter->offset_in_chunk;
  //GST_ERROR("sample: %d %d %d", iter->chunk_index, iter->offset_in_chunk, (int)sample->offset);
}

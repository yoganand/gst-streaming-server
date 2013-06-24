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
#include <gst/base/gstbytewriter.h>

#include "gss-server.h"
#include "gss-html.h"
#include "gss-session.h"
#include "gss-soup.h"
#include "gss-content.h"
#include "gss-vod.h"
#include "gss-ism-parser.h"
#include "gss-iso-atoms.h"
#include "gss-playready.h"

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <openssl/aes.h>

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


static gboolean parser_read (GssISMParser * parser, guint8 * buffer,
    guint64 offset, guint64 n_bytes);
static void gss_ism_parse_ftyp (GssISMParser * parser, guint64 offset,
    guint64 size);
static void gss_ism_parse_moof (GssISMParser * parser,
    GssISMFragment * fragment, GstByteReader * br);
static void gss_ism_parse_traf (GssISMParser * parser, AtomTraf * traf,
    GstByteReader * br);
static void gss_ism_parse_mfhd (GssISMParser * parser, AtomMfhd * mfhd,
    GstByteReader * br);
static void gss_ism_parse_tfhd (GssISMParser * parser, AtomTfhd * tfhd,
    GstByteReader * br);
static void gss_ism_parse_trun (GssISMParser * parser, AtomTrun * trun,
    GstByteReader * br);
static void gss_ism_parse_sdtp (GssISMParser * parser, AtomSdtp * sdtp,
    GstByteReader * br, int sample_count);
static void gss_ism_parse_mfra (GssISMParser * parser, guint64 offset,
    guint64 size);
static void gss_ism_parse_sample_encryption (GssISMParser * parser,
    AtomUUIDSampleEncryption * se, GstByteReader * br);
static void gss_ism_parse_moov (GssISMParser * parser, GssISMMovie * movie,
    GstByteReader * br);
static void gss_ism_fixup_moof (GssISMFragment * fragment);
static void gss_ism_parser_fixup (GssISMParser * parser);

static guint64 gss_ism_moof_get_duration (GssISMFragment * fragment);


GssISMParser *
gss_ism_parser_new (void)
{
  GssISMParser *parser;

  parser = g_malloc0 (sizeof (GssISMParser));

  return parser;
}

void
gss_ism_parser_free (GssISMParser * parser)
{
  if (parser->fd > 0) {
    close (parser->fd);
  }
  g_free (parser->fragments);
  g_free (parser);
}

GssISMFragment *
gss_ism_parser_get_fragment (GssISMParser * parser, int track_id, int index)
{
  int frag_i;
  int i;

  frag_i = 0;

  for (i = 0; i < parser->n_fragments; i++) {
    if (parser->fragments[i]->traf.tfhd.track_id == track_id) {
      if (frag_i == index) {
        return parser->fragments[i];
      }
      frag_i++;
    }
  }

  return NULL;
}

GssISMFragment *
gss_ism_parser_get_fragment_by_timestamp (GssISMParser * parser,
    int track_id, guint64 timestamp)
{
  int i;

  for (i = 0; i < parser->n_fragments; i++) {
    if (parser->fragments[i]->traf.tfhd.track_id == track_id) {
      if (parser->fragments[i]->timestamp == timestamp) {
        return parser->fragments[i];
      }
    }
  }

  return NULL;
}

guint64
gss_ism_parser_get_duration (GssISMParser * parser, int track_id)
{
  int i;
  guint64 duration;

  duration = 0;
  for (i = 0; i < parser->n_fragments; i++) {
    if (parser->fragments[i]->traf.tfhd.track_id == track_id) {
      duration += parser->fragments[i]->duration;
    }
  }

  return duration;
}

int
gss_ism_parser_get_n_fragments (GssISMParser * parser, int track_id)
{
  int i;
  int n;

  n = 0;
  for (i = 0; i < parser->n_fragments; i++) {
    if (parser->fragments[i]->traf.tfhd.track_id == track_id) {
      n++;
    }
  }

  return n;
}

void
gss_ism_parser_load_chunk (GssISMParser * parser, guint64 offset, guint64 size)
{
  if (parser->data) {
    g_free (parser->data);
  }
  parser->data = g_malloc (size);
  parser->data_offset = offset;
  parser->data_size = size;

  parser_read (parser, parser->data, parser->data_offset, parser->data_size);
}

gboolean
gss_ism_parser_parse_file (GssISMParser * parser, const char *filename)
{
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

    parser_read (parser, buffer, parser->offset, 16);
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
      gss_ism_parser_load_chunk (parser, parser->offset, size);

      gss_ism_parse_ftyp (parser, parser->offset, size);
    } else if (atom == GST_MAKE_FOURCC ('m', 'o', 'o', 'f')) {
      GstByteReader br;
      GssISMFragment *fragment;

      gss_ism_parser_load_chunk (parser, parser->offset, size);
      gst_byte_reader_init (&br, parser->data + 8, size - 8);

      fragment = gss_ism_fragment_new ();
      gss_ism_parse_moof (parser, fragment, &br);
      gss_ism_fixup_moof (fragment);

      if (parser->n_fragments == parser->n_fragments_alloc) {
        parser->n_fragments_alloc += 100;
        parser->fragments = g_realloc (parser->fragments,
            parser->n_fragments_alloc * sizeof (GssISMFragment *));
      }
      parser->fragments[parser->n_fragments] = fragment;
      parser->n_fragments++;
      parser->current_fragment = fragment;

      fragment->duration = gss_ism_moof_get_duration (fragment);
      fragment->moof_offset = parser->offset;
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
        parser->current_fragment->mdat_offset = parser->offset;
        parser->current_fragment->mdat_size = size;
      }
    } else if (atom == GST_MAKE_FOURCC ('m', 'f', 'r', 'a')) {
      gss_ism_parse_mfra (parser, parser->offset, size);
    } else if (atom == GST_MAKE_FOURCC ('m', 'o', 'o', 'v')) {
      GstByteReader br;
      guint8 *data;
      GssISMMovie *movie;

      data = g_malloc (size);
      parser_read (parser, data, parser->offset, size);
      gst_byte_reader_init (&br, data + 8, size - 8);

      movie = gss_ism_movie_new ();
      gss_ism_parse_moov (parser, movie, &br);

      parser->movie = movie;
    } else if (atom == GST_MAKE_FOURCC ('u', 'u', 'i', 'd')) {
      const guint8 *uuid = NULL;

      uuid = buffer + 8;

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
    } else {
      GST_WARNING ("unknown atom %" GST_FOURCC_FORMAT
          " at offset %" G_GINT64_MODIFIER "x, size %d",
          GST_FOURCC_ARGS (atom), parser->offset, size);
    }

    parser->offset += size;
  }

  if (parser->error) {
    GST_ERROR ("parser error");
    close (parser->fd);
    parser->fd = -1;
    return FALSE;
  }

  gss_ism_parser_fixup (parser);

  close (parser->fd);
  parser->fd = -1;
  return TRUE;
}

static void
gss_ism_parser_fixup (GssISMParser * parser)
{
  guint64 ts;
  int track_id;
  int i;

  /* FIXME this should use the real track id's in the file */
  for (track_id = 1; track_id <= 2; track_id++) {
    ts = 0;
    for (i = 0; i < parser->n_fragments; i++) {
      if (parser->fragments[i]->traf.tfhd.track_id == track_id) {
        parser->fragments[i]->timestamp = ts;
        ts += parser->fragments[i]->duration;
      }
    }
  }
}

GssISMTrack *
gss_ism_track_new (void)
{
  return g_malloc0 (sizeof (GssISMTrack));
}

void
gss_ism_track_free (GssISMTrack * track)
{
  g_free (track);
}

GssISMMovie *
gss_ism_movie_new (void)
{
  GssISMMovie *movie;
  movie = g_malloc0 (sizeof (GssISMMovie));
  movie->tracks = g_malloc (20 * sizeof (GssISMTrack *));
  return movie;
}

void
gss_ism_movie_free (GssISMMovie * movie)
{
  g_free (movie->tracks);
  g_free (movie);
}

GssISMFragment *
gss_ism_fragment_new (void)
{
  GssISMFragment *fragment;
  fragment = g_malloc0 (sizeof (GssISMFragment));
  return fragment;
}

void
gss_ism_fragment_free (GssISMFragment * fragment)
{
  g_free (fragment);
}


static gboolean
parser_read (GssISMParser * parser, guint8 * buffer, guint64 offset,
    guint64 n_bytes)
{
  off_t ret;
  ssize_t n;

  ret = lseek (parser->fd, offset, SEEK_SET);
  if (ret < 0) {
    GST_ERROR ("seek to %" G_GUINT64_FORMAT " failed", offset);
    parser->error = TRUE;
    return FALSE;
  }

  if (offset + n_bytes > parser->file_size) {
    n_bytes = parser->file_size - offset;
  }

  n = read (parser->fd, buffer, n_bytes);
  if (n != n_bytes) {
    GST_ERROR ("read of %" G_GUINT64_FORMAT " bytes at offset %"
        G_GUINT64_FORMAT " failed: %s", n_bytes, offset, strerror (errno));
    parser->error = TRUE;
    return FALSE;
  }

  return TRUE;
}


static void
gss_ism_parse_ftyp (GssISMParser * parser, guint64 offset, guint64 size)
{
  GstByteReader br;
  guint8 *data;
  guint32 atom = 0;
  guint32 tmp = 0;
  gboolean ret;

  data = g_malloc (size);
  ret = parser_read (parser, data, parser->offset, size);
  if (!ret) {
    return;
  }

  gst_byte_reader_init (&br, data + 8, size - 8);

  gst_byte_reader_get_uint32_le (&br, &parser->ftyp_atom);
  GST_DEBUG ("ftyp: %" GST_FOURCC_FORMAT, GST_FOURCC_ARGS (parser->ftyp_atom));
  if (parser->ftyp_atom == GST_MAKE_FOURCC ('i', 's', 'm', 'l')) {
    parser->is_isml = TRUE;
  } else if (parser->ftyp_atom == GST_MAKE_FOURCC ('m', 'p', '4', '2')) {
    parser->is_mp42 = TRUE;
  } else if (parser->ftyp_atom == GST_MAKE_FOURCC ('q', 't', ' ', ' ')) {
    //parser->is_qt__ = TRUE;
  } else {
    GST_ERROR ("not isml, mp4, or qt file: %" GST_FOURCC_FORMAT,
        GST_FOURCC_ARGS (atom));
  }
  gst_byte_reader_get_uint32_le (&br, &tmp);
  while (gst_byte_reader_get_uint32_le (&br, &atom)) {
    GST_DEBUG ("compat: %" GST_FOURCC_FORMAT, GST_FOURCC_ARGS (atom));
    if (atom == GST_MAKE_FOURCC ('i', 's', 'm', 'l')) {
      parser->ftyp |= GSS_ISOM_FTYP_ISML;
    } else if (atom == GST_MAKE_FOURCC ('m', 'p', '4', '2')) {
      parser->ftyp |= GSS_ISOM_FTYP_MP42;
    } else if (atom == GST_MAKE_FOURCC ('m', 'p', '4', '1')) {
      parser->ftyp |= GSS_ISOM_FTYP_MP41;
    } else if (atom == GST_MAKE_FOURCC ('p', 'i', 'f', 'f')) {
      parser->ftyp |= GSS_ISOM_FTYP_PIFF;
    } else if (atom == GST_MAKE_FOURCC ('i', 's', 'o', '2')) {
      parser->ftyp |= GSS_ISOM_FTYP_ISO2;
    } else if (atom == GST_MAKE_FOURCC ('i', 's', 'o', 'm')) {
      parser->ftyp |= GSS_ISOM_FTYP_ISOM;
    } else if (atom == GST_MAKE_FOURCC ('q', 't', ' ', ' ')) {
      parser->ftyp |= GSS_ISOM_FTYP_QT__;
    } else if (atom == 0) {
      GST_DEBUG ("ignoring 0 ftyp");
    } else {
      GST_ERROR ("unknown ftyp %" GST_FOURCC_FORMAT, GST_FOURCC_ARGS (atom));
    }
  }
}

static void
gst_byte_reader_init_sub (GstByteReader * sbr, const GstByteReader * br,
    guint size)
{
  gst_byte_reader_init (sbr, br->data + br->byte, size);
}

static void
gss_ism_parse_moof (GssISMParser * parser, GssISMFragment * fragment,
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
      gss_ism_parse_mfhd (parser, &fragment->mfhd, &sbr);
    } else if (atom == GST_MAKE_FOURCC ('t', 'r', 'a', 'f')) {
      gss_ism_parse_traf (parser, &fragment->traf, &sbr);
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
          " inside moof at offset %" G_GINT64_MODIFIER "x, size %d",
          GST_FOURCC_ARGS (atom), parser->offset, size);
    }

    gst_byte_reader_skip (br, size - 8);
  }
}

static void
gss_ism_parse_traf (GssISMParser * parser, AtomTraf * traf, GstByteReader * br)
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
      gss_ism_parse_tfhd (parser, &traf->tfhd, &sbr);
    } else if (atom == GST_MAKE_FOURCC ('t', 'r', 'u', 'n')) {
      gss_ism_parse_trun (parser, &traf->trun, &sbr);
    } else if (atom == GST_MAKE_FOURCC ('s', 'd', 't', 'p')) {
      gss_ism_parse_sdtp (parser, &traf->sdtp, &sbr, traf->trun.sample_count);
    } else if (atom == GST_MAKE_FOURCC ('u', 'u', 'i', 'd')) {
      const guint8 *uuid = NULL;

      gst_byte_reader_get_data (&sbr, 16, &uuid);

      if (memcmp (uuid, uuid_sample_encryption, 16) == 0) {
        gss_ism_parse_sample_encryption (parser, &traf->sample_encryption,
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
          " inside traf at offset %" G_GINT64_MODIFIER "x, size %d",
          GST_FOURCC_ARGS (atom), parser->offset, size);
    }

    gst_byte_reader_skip (br, size - 8);
  }
}

static void
gss_ism_parse_mfhd (GssISMParser * parser, AtomMfhd * mfhd, GstByteReader * br)
{

  gst_byte_reader_get_uint8 (br, &mfhd->version);
  gst_byte_reader_get_uint24_be (br, &mfhd->flags);
  gst_byte_reader_get_uint32_be (br, &mfhd->sequence_number);
}

static void
gss_ism_parse_tfhd (GssISMParser * parser, AtomTfhd * tfhd, GstByteReader * br)
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
gss_ism_parse_trun (GssISMParser * parser, AtomTrun * trun, GstByteReader * br)
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

  trun->samples = g_malloc0 (sizeof (AtomTrunSample) * trun->sample_count);
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
gss_ism_fixup_moof (GssISMFragment * fragment)
{
  AtomTfhd *tfhd = &fragment->traf.tfhd;
  AtomTrun *trun = &fragment->traf.trun;
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
gss_ism_moof_get_duration (GssISMFragment * fragment)
{
  guint64 duration = 0;
  int i;

  for (i = 0; i < fragment->traf.trun.sample_count; i++) {
    duration += fragment->traf.trun.samples[i].duration;
  }
  return duration;
}

static void
gss_ism_parse_sdtp (GssISMParser * parser, AtomSdtp * sdtp, GstByteReader * br,
    int sample_count)
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
gss_ism_parse_sample_encryption (GssISMParser * parser,
    AtomUUIDSampleEncryption * se, GstByteReader * br)
{
  int i;
  int j;

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
      sizeof (AtomUUIDSampleEncryptionSample));
  for (i = 0; i < se->sample_count; i++) {
    gst_byte_reader_get_uint64_be (br, &se->samples[i].iv);
    if (se->flags & 0x0002) {
      gst_byte_reader_get_uint16_be (br, &se->samples[i].num_entries);
      GST_DEBUG ("n_entries %d", se->samples[i].num_entries);
      se->samples[i].entries = g_malloc0 (se->samples[i].num_entries *
          sizeof (AtomUUIDSampleEncryptionSampleEntry));
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
gss_ism_parse_mfra (GssISMParser * parser, guint64 offset, guint64 size)
{
  /* ignored for now */
  GST_DEBUG ("FIXME parse mfra atom");
}

void
gss_ism_fragment_set_sample_encryption (GssISMFragment * fragment,
    int n_samples, guint64 * init_vectors, gboolean is_h264)
{
  AtomUUIDSampleEncryption *se = &fragment->traf.sample_encryption;
  AtomTrun *trun = &fragment->traf.trun;
  int i;

  se->present = TRUE;
  se->flags = 0;
  se->sample_count = n_samples;
  se->samples = g_malloc0 (sizeof (AtomUUIDSampleEncryptionSample) * n_samples);
  for (i = 0; i < n_samples; i++) {
    se->samples[i].iv = init_vectors[i];
  }

  if (is_h264) {
    se->flags |= 0x0002;
    for (i = 0; i < n_samples; i++) {
      se->samples[i].num_entries = 1;
      se->samples[i].entries = g_malloc0 (se->samples[i].num_entries *
          sizeof (AtomUUIDSampleEncryptionSampleEntry));
      se->samples[i].entries[0].bytes_of_clear_data = 5;
      se->samples[i].entries[0].bytes_of_encrypted_data =
          trun->samples[i].size - 5;
    }

  }
}

static void
gss_ism_parse_mvhd (GssISMParser * parser, AtomMvhd * mvhd, GstByteReader * br)
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

#define CHECK_END(br) do { \
  if ((br)->byte < (br)->size) \
    GST_ERROR("leftover bytes %d < %d", (br)->byte, (br)->size); \
} while(0)
#define CHECK_END_ATOM(br, atom) do { \
  if ((br)->byte < (br)->size) \
    GST_ERROR("leftover bytes %d < %d in container %" GST_FOURCC_FORMAT, \
        (br)->byte, (br)->size, GST_FOURCC_ARGS((atom))); \
} while(0)
static void
gss_ism_parse_tkhd (GssISMParser * parser, GssISMTrack * track,
    GstByteReader * br)
{
  AtomTkhd *tkhd = &track->tkhd;
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
  gst_byte_reader_get_uint32_be (br, &tmp);
  gst_byte_reader_get_uint16_be (br, &tmp16);
  if (tmp16 == 0x0100)
    tkhd->track_is_audio = TRUE;
  gst_byte_reader_get_uint16_be (br, &tmp16);
  for (i = 0; i < 9; i++) {
    gst_byte_reader_get_uint32_be (br, &tmp);
  }
  gst_byte_reader_get_uint32_be (br, &tmp);
  if (tmp == 0x01400000)
    tkhd->track_is_visual = TRUE;
  gst_byte_reader_get_uint32_be (br, &tmp);

  CHECK_END (br);
}

static void
gss_ism_parse_tref (GssISMParser * parser, GssISMTrack * track,
    GstByteReader * br)
{
  AtomTref *tref = &track->tref;
  tref->present = TRUE;

  GST_FIXME ("parse tref");

  //CHECK_END (br);
}

static void
gss_ism_parse_elst (GssISMParser * parser, GssISMTrack * track,
    GstByteReader * br)
{
  AtomElst *elst = &track->elst;
  elst->present = TRUE;
  gst_byte_reader_get_uint8 (br, &elst->version);
  gst_byte_reader_get_uint24_be (br, &elst->flags);

  GST_FIXME ("parse elst");

  //CHECK_END (br);
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
gss_ism_parse_mdhd (GssISMParser * parser, GssISMTrack * track,
    GstByteReader * br)
{
  AtomMdhd *mdhd = &track->mdhd;
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
get_string (GssISMParser * parser, GstByteReader * br, gchar ** string)
{
  gboolean ret;
  guint8 len = 0;
  gboolean nul_terminated;

  if (parser->ftyp & (GSS_ISOM_FTYP_MP41 | GSS_ISOM_FTYP_MP42 |
          GSS_ISOM_FTYP_PIFF)) {
    nul_terminated = TRUE;
  } else {
    nul_terminated = FALSE;
  }

  gst_byte_reader_peek_uint8 (br, &len);
  if (gst_byte_reader_get_remaining (br) == len + 1) {
    const guint8 *s;

    if (nul_terminated) {
      GST_WARNING ("expecting nul-terminated string, got pascal string "
          "(ftyp %08x}", parser->ftyp);
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
          "(ftyp %08x)", parser->ftyp);
    }

    ret = gst_byte_reader_dup_string (br, string);
  }

  return ret;
}

static void
gss_ism_parse_hdlr (GssISMParser * parser, GssISMTrack * track,
    GstByteReader * br)
{
  AtomHdlr *hdlr = &track->hdlr;
  guint32 tmp = 0;

  hdlr->present = TRUE;
  gst_byte_reader_get_uint8 (br, &hdlr->version);
  gst_byte_reader_get_uint24_be (br, &hdlr->flags);
  gst_byte_reader_get_uint32_be (br, &tmp);
  gst_byte_reader_get_uint32_be (br, &hdlr->handler_type);
  gst_byte_reader_skip (br, 12);
  get_string (parser, br, &hdlr->name);

  gst_byte_reader_dump (br);

  CHECK_END (br);
}

static void
gss_ism_parse_vmhd (GssISMParser * parser, GssISMTrack * track,
    GstByteReader * br)
{
  AtomVmhd *vmhd = &track->vmhd;
  vmhd->present = TRUE;
  gst_byte_reader_get_uint8 (br, &vmhd->version);
  gst_byte_reader_get_uint24_be (br, &vmhd->flags);
  gst_byte_reader_skip (br, 8);

  CHECK_END (br);
}

static void
gss_ism_parse_smhd (GssISMParser * parser, GssISMTrack * track,
    GstByteReader * br)
{
  AtomSmhd *smhd = &track->smhd;
  smhd->present = TRUE;
  gst_byte_reader_get_uint8 (br, &smhd->version);
  gst_byte_reader_get_uint24_be (br, &smhd->flags);
  gst_byte_reader_skip (br, 4);

  CHECK_END (br);
}

static void
gss_ism_parse_hmhd (GssISMParser * parser, GssISMTrack * track,
    GstByteReader * br)
{
  AtomHmhd *hmhd = &track->hmhd;
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
gss_ism_parse_dref (GssISMParser * parser, GssISMTrack * track,
    GstByteReader * br)
{
  AtomDref *dref = &track->dref;
  int i;
  dref->present = TRUE;
  gst_byte_reader_get_uint8 (br, &dref->version);
  gst_byte_reader_get_uint24_be (br, &dref->flags);
  gst_byte_reader_get_uint32_be (br, &dref->entry_count);
  dref->entries = g_malloc0 (sizeof (AtomDrefEntry) * dref->entry_count);
  for (i = 0; i < dref->entry_count; i++) {
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
    if (atom == GST_MAKE_FOURCC ('u', 'r', 'l', ' ')) {
      GST_FIXME ("url_");
    } else if (atom == GST_MAKE_FOURCC ('u', 'r', 'n', ' ')) {
      GST_FIXME ("urn_");
    } else if (atom == GST_MAKE_FOURCC ('a', 'l', 'i', 's')) {
      GST_FIXME ("alis");
    } else if (atom == GST_MAKE_FOURCC ('c', 'i', 'o', 's')) {
      GST_FIXME ("cios");
    } else {
      GST_WARNING ("unknown atom %" GST_FOURCC_FORMAT " inside dref, size %d",
          GST_FOURCC_ARGS (atom), size);
    }

    gst_byte_reader_skip (br, size - 8);
  }

  gst_byte_reader_dump (br);

  CHECK_END (br);
}

static void
gss_ism_parse_stts (GssISMParser * parser, GssISMTrack * track,
    GstByteReader * br)
{
  AtomStts *stts = &track->stts;
  int i;
  stts->present = TRUE;
  gst_byte_reader_get_uint8 (br, &stts->version);
  gst_byte_reader_get_uint24_be (br, &stts->flags);
  gst_byte_reader_get_uint32_be (br, &stts->entry_count);
  stts->entries = g_malloc0 (sizeof (AtomSttsEntry) * stts->entry_count);
  for (i = 0; i < stts->entry_count; i++) {
    gst_byte_reader_get_uint32_be (br, &stts->entries[i].sample_count);
    gst_byte_reader_get_int32_be (br, &stts->entries[i].sample_delta);
  }

  CHECK_END (br);
}

static void
gss_ism_parse_ctts (GssISMParser * parser, GssISMTrack * track,
    GstByteReader * br)
{
  AtomCtts *ctts = &track->ctts;
  int i;
  ctts->present = TRUE;
  gst_byte_reader_get_uint8 (br, &ctts->version);
  gst_byte_reader_get_uint24_be (br, &ctts->flags);
  gst_byte_reader_get_uint32_be (br, &ctts->entry_count);
  ctts->entries = g_malloc0 (sizeof (AtomCttsEntry) * ctts->entry_count);
  for (i = 0; i < ctts->entry_count; i++) {
    gst_byte_reader_get_uint32_be (br, &ctts->entries[i].sample_count);
    gst_byte_reader_get_uint32_be (br, &ctts->entries[i].sample_offset);
  }

  CHECK_END (br);
}

static void
gss_ism_parse_stsz (GssISMParser * parser, GssISMTrack * track,
    GstByteReader * br)
{
  AtomStsz *stsz = &track->stsz;
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
gss_ism_parse_stsc (GssISMParser * parser, GssISMTrack * track,
    GstByteReader * br)
{
  AtomStsc *stsc = &track->stsc;
  int i;
  stsc->present = TRUE;
  gst_byte_reader_get_uint8 (br, &stsc->version);
  gst_byte_reader_get_uint24_be (br, &stsc->flags);
  gst_byte_reader_get_uint32_be (br, &stsc->entry_count);
  stsc->entries = g_malloc0 (sizeof (AtomStscEntry) * stsc->entry_count);
  for (i = 0; i < stsc->entry_count; i++) {
    gst_byte_reader_get_uint32_be (br, &stsc->entries[i].first_chunk);
    gst_byte_reader_get_uint32_be (br, &stsc->entries[i].samples_per_chunk);
    gst_byte_reader_get_uint32_be (br,
        &stsc->entries[i].sample_description_index);
  }

  CHECK_END (br);
}

static void
gss_ism_parse_stco (GssISMParser * parser, GssISMTrack * track,
    GstByteReader * br)
{
  AtomStco *stco = &track->stco;
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
gss_ism_parse_co64 (GssISMParser * parser, GssISMTrack * track,
    GstByteReader * br)
{
  AtomStco *stco = &track->stco;
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
gss_ism_parse_stss (GssISMParser * parser, GssISMTrack * track,
    GstByteReader * br)
{
  AtomStss *stss = &track->stss;
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
gss_ism_parse_stsh (GssISMParser * parser, GssISMTrack * track,
    GstByteReader * br)
{
  AtomStsh *stsh = &track->stsh;
  int i;
  stsh->present = TRUE;
  gst_byte_reader_get_uint8 (br, &stsh->version);
  gst_byte_reader_get_uint24_be (br, &stsh->flags);
  gst_byte_reader_get_uint32_be (br, &stsh->entry_count);
  stsh->entries = g_malloc0 (sizeof (AtomStshEntry) * stsh->entry_count);
  for (i = 0; i < stsh->entry_count; i++) {
    gst_byte_reader_get_uint32_be (br,
        &stsh->entries[i].shadowed_sample_number);
    gst_byte_reader_get_uint32_be (br, &stsh->entries[i].sync_sample_number);
  }

  CHECK_END (br);
}

static void
gss_ism_parse_stdp (GssISMParser * parser, GssISMTrack * track,
    GstByteReader * br)
{
  AtomStdp *stdp = &track->stdp;
  int i;
  stdp->present = TRUE;
  gst_byte_reader_get_uint8 (br, &stdp->version);
  gst_byte_reader_get_uint24_be (br, &stdp->flags);
  stdp->priorities =
      g_malloc0 (sizeof (AtomStshEntry) * track->stsz.sample_count);
  for (i = 0; i < track->stsz.sample_count; i++) {
    gst_byte_reader_get_uint16_be (br, &stdp->priorities[i]);
  }

  CHECK_END (br);
}

static void
gss_ism_parse_stsd (GssISMParser * parser, GssISMTrack * track,
    GstByteReader * br)
{
  AtomStsd *stsd = &track->stsd;
  int i;
  stsd->present = TRUE;
  gst_byte_reader_get_uint8 (br, &stsd->version);
  gst_byte_reader_get_uint24_be (br, &stsd->flags);
  gst_byte_reader_get_uint32_be (br, &stsd->entry_count);
  for (i = 0; i < stsd->entry_count; i++) {
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
    if (atom == GST_MAKE_FOURCC ('m', 'p', '4', 'a')) {
      GST_FIXME ("mp4a");
    } else if (atom == GST_MAKE_FOURCC ('a', 'v', 'c', '1')) {
      GST_FIXME ("avc1");
    } else if (atom == GST_MAKE_FOURCC ('e', 'n', 'c', 'v')) {
      GST_FIXME ("encv");
    } else if (atom == GST_MAKE_FOURCC ('e', 'n', 'c', 'a')) {
      GST_FIXME ("enca");
    } else if (atom == GST_MAKE_FOURCC ('t', 'm', 'c', 'd')) {
      GST_FIXME ("tmcd");
    } else if (atom == GST_MAKE_FOURCC ('a', 'p', 'c', 'h')) {
      GST_FIXME ("apch");
    } else if (atom == GST_MAKE_FOURCC ('A', 'V', '1', 'x')) {
      GST_FIXME ("AV1x");
    } else {
#if 0
      GST_WARNING ("unknown atom %" GST_FOURCC_FORMAT " inside stsd, size %d",
          GST_FOURCC_ARGS (atom), size);
#endif
    }

    gst_byte_reader_skip (br, size - 8);
  }

  gst_byte_reader_dump (br);

  CHECK_END (br);
}

static void
gss_ism_parse_ignore (GssISMParser * parser, GssISMTrack * track,
    GstByteReader * br)
{
  GST_FIXME ("ingoring atom");
}

typedef struct _ContainerAtoms ContainerAtoms;
struct _ContainerAtoms
{
  guint32 atom;
  void (*parse) (GssISMParser * parser, GssISMTrack * track,
      GstByteReader * br);
  ContainerAtoms *atoms;
};

static ContainerAtoms dinf_atoms[] = {
  {GST_MAKE_FOURCC ('d', 'r', 'e', 'f'), gss_ism_parse_dref},
  {0}
};

static ContainerAtoms stbl_atoms[] = {
  {GST_MAKE_FOURCC ('s', 't', 't', 's'), gss_ism_parse_stts},
  {GST_MAKE_FOURCC ('c', 't', 't', 's'), gss_ism_parse_ctts},
  {GST_MAKE_FOURCC ('s', 't', 's', 's'), gss_ism_parse_stss},
  {GST_MAKE_FOURCC ('s', 't', 's', 'd'), gss_ism_parse_stsd},
  {GST_MAKE_FOURCC ('s', 't', 's', 'z'), gss_ism_parse_stsz},
  {GST_MAKE_FOURCC ('s', 't', 's', 'c'), gss_ism_parse_stsc},
  {GST_MAKE_FOURCC ('s', 't', 'c', 'o'), gss_ism_parse_stco},
  {GST_MAKE_FOURCC ('c', 'o', '6', '4'), gss_ism_parse_co64},
  {GST_MAKE_FOURCC ('s', 't', 's', 'h'), gss_ism_parse_stsh},
  {GST_MAKE_FOURCC ('s', 't', 'd', 'p'), gss_ism_parse_stdp},
  {GST_MAKE_FOURCC ('c', 's', 'l', 'g'), gss_ism_parse_ignore},
  {GST_MAKE_FOURCC ('s', 't', 'p', 's'), gss_ism_parse_ignore},
  {GST_MAKE_FOURCC ('s', 'd', 't', 'p'), gss_ism_parse_ignore},
  {0}
};

static ContainerAtoms minf_atoms[] = {
  {GST_MAKE_FOURCC ('v', 'm', 'h', 'd'), gss_ism_parse_vmhd},
  {GST_MAKE_FOURCC ('s', 'm', 'h', 'd'), gss_ism_parse_smhd},
  {GST_MAKE_FOURCC ('h', 'm', 'h', 'd'), gss_ism_parse_hmhd},
  {GST_MAKE_FOURCC ('d', 'i', 'n', 'f'), NULL, dinf_atoms},
  {GST_MAKE_FOURCC ('s', 't', 'b', 'l'), NULL, stbl_atoms},
  {GST_MAKE_FOURCC ('g', 'm', 'h', 'd'), gss_ism_parse_ignore},
  {GST_MAKE_FOURCC ('h', 'd', 'l', 'r'), gss_ism_parse_ignore},
  {GST_MAKE_FOURCC ('c', 'o', 'd', 'e'), gss_ism_parse_ignore},
  {0}
};

static ContainerAtoms mdia_atoms[] = {
  {GST_MAKE_FOURCC ('m', 'd', 'h', 'd'), gss_ism_parse_mdhd},
  {GST_MAKE_FOURCC ('h', 'd', 'l', 'r'), gss_ism_parse_hdlr},
  {GST_MAKE_FOURCC ('m', 'i', 'n', 'f'), NULL, minf_atoms},
  {GST_MAKE_FOURCC ('i', 'm', 'a', 'p'), gss_ism_parse_ignore},
  {GST_MAKE_FOURCC ('u', 'd', 't', 'a'), gss_ism_parse_ignore},
  {0}
};

static ContainerAtoms edts_atoms[] = {
  {GST_MAKE_FOURCC ('e', 'l', 's', 't'), gss_ism_parse_elst},
  {0}
};

#if 0
static ContainerAtoms udta_atoms[] = {
  {0}
};
#endif

static ContainerAtoms trak_atoms[] = {
  {GST_MAKE_FOURCC ('t', 'k', 'h', 'd'), gss_ism_parse_tkhd},
  {GST_MAKE_FOURCC ('t', 'r', 'e', 'f'), gss_ism_parse_tref},
  //{GST_MAKE_FOURCC ('u', 'd', 't', 'a'), NULL, udta_atoms},
  {GST_MAKE_FOURCC ('u', 'd', 't', 'a'), gss_ism_parse_ignore},
  {GST_MAKE_FOURCC ('e', 'd', 't', 's'), NULL, edts_atoms},
  {GST_MAKE_FOURCC ('m', 'd', 'i', 'a'), NULL, mdia_atoms},
  {GST_MAKE_FOURCC ('m', 'e', 't', 'a'), gss_ism_parse_ignore},
  {GST_MAKE_FOURCC ('l', 'o', 'a', 'd'), gss_ism_parse_ignore},
  {GST_MAKE_FOURCC ('t', 'a', 'p', 't'), gss_ism_parse_ignore},
  {0}
};


static void
gss_ism_parse_container (GssISMParser * parser, GssISMTrack * track,
    GstByteReader * br, ContainerAtoms * atoms, guint32 parent_atom)
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
          atoms[i].parse (parser, track, &sbr);
        } else {
          gss_ism_parse_container (parser, track, &sbr, atoms[i].atoms, atom);
        }
        break;
      }
    }
    if (atoms[i].atom == 0) {
      GST_WARNING ("unknown atom %" GST_FOURCC_FORMAT
          " inside %" GST_FOURCC_FORMAT ", size %d",
          GST_FOURCC_ARGS (atom), GST_FOURCC_ARGS (parent_atom), size);
    }

    gst_byte_reader_skip (br, size - 8);
  }

  CHECK_END_ATOM (br, parent_atom);
}

static void
gss_ism_parse_udta (GssISMParser * parser, AtomUdta * udta, GstByteReader * br)
{
}

static void
gss_ism_parse_mvex (GssISMParser * parser, AtomMvex * mvex, GstByteReader * br)
{
}

static void
gss_ism_parse_meta (GssISMParser * parser, AtomMeta * meta, GstByteReader * br)
{
}

static void
gss_ism_parse_skip (GssISMParser * parser, AtomSkip * skip, GstByteReader * br)
{
}

static void
gss_ism_parse_iods (GssISMParser * parser, AtomIods * iods, GstByteReader * br)
{
}

static void
gss_ism_parse_moov (GssISMParser * parser, GssISMMovie * movie,
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
      gss_ism_parse_mvhd (parser, &movie->mvhd, &sbr);
    } else if (atom == GST_MAKE_FOURCC ('t', 'r', 'a', 'k')) {
      GssISMTrack *track;
      track = gss_ism_track_new ();
      gss_ism_parse_container (parser, track, &sbr, trak_atoms, atom);
      movie->tracks[movie->n_tracks] = track;
      movie->n_tracks++;
    } else if (atom == GST_MAKE_FOURCC ('u', 'd', 't', 'a')) {
      gss_ism_parse_udta (parser, &movie->udta, &sbr);
    } else if (atom == GST_MAKE_FOURCC ('m', 'v', 'e', 'x')) {
      gss_ism_parse_mvex (parser, &movie->mvex, &sbr);
    } else if (atom == GST_MAKE_FOURCC ('m', 'e', 't', 'a')) {
      gss_ism_parse_meta (parser, &movie->meta, &sbr);
    } else if (atom == GST_MAKE_FOURCC ('s', 'k', 'i', 'p')) {
      gss_ism_parse_skip (parser, &movie->skip, &sbr);
    } else if (atom == GST_MAKE_FOURCC ('i', 'o', 'd', 's')) {
      gss_ism_parse_iods (parser, &movie->iods, &sbr);
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
          " inside moov at offset %" G_GINT64_MODIFIER "x, size %d",
          GST_FOURCC_ARGS (atom), parser->offset + br->byte, size);
    }

    gst_byte_reader_skip (br, size - 8);
  }
}


#define ATOM_INIT(bw, atom) (gst_byte_writer_put_uint32_be ((bw), 0), \
  gst_byte_writer_put_uint32_le ((bw), (atom)), (bw)->parent.byte - 8)
#define ATOM_FINISH(bw, offset) \
  GST_WRITE_UINT32_BE((void *)(bw)->parent.data + (offset), \
      (bw)->parent.byte - (offset))

static void
gss_ism_tfhd_serialize (AtomTfhd * tfhd, GstByteWriter * bw)
{
  int offset;

  offset = ATOM_INIT (bw, GST_MAKE_FOURCC ('t', 'f', 'h', 'd'));

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

  ATOM_FINISH (bw, offset);
}

static void
gss_ism_trun_serialize (AtomTrun * trun, GstByteWriter * bw)
{
  int offset;
  int i;

  offset = ATOM_INIT (bw, GST_MAKE_FOURCC ('t', 'r', 'u', 'n'));

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

  ATOM_FINISH (bw, offset);
}

static void
gss_ism_sdtp_serialize (AtomSdtp * sdtp, GstByteWriter * bw, int sample_count)
{
  int offset;
  int i;

  offset = ATOM_INIT (bw, GST_MAKE_FOURCC ('s', 'd', 't', 'p'));

  gst_byte_writer_put_uint8 (bw, sdtp->version);
  gst_byte_writer_put_uint24_be (bw, sdtp->flags);
  for (i = 0; i < sample_count; i++) {
    gst_byte_writer_put_uint8 (bw, sdtp->sample_flags[i]);
  }

  ATOM_FINISH (bw, offset);
}

static void
gss_ism_sample_encryption_serialize (AtomUUIDSampleEncryption * se,
    GstByteWriter * bw)
{
  int offset;
  int i, j;

  if (!se->present)
    return;
  offset = ATOM_INIT (bw, GST_MAKE_FOURCC ('u', 'u', 'i', 'd'));

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

  ATOM_FINISH (bw, offset);
}


static void
gss_ism_traf_serialize (AtomTraf * traf, GstByteWriter * bw)
{
  int offset;
  offset = ATOM_INIT (bw, GST_MAKE_FOURCC ('t', 'r', 'a', 'f'));

  gss_ism_tfhd_serialize (&traf->tfhd, bw);
  gss_ism_trun_serialize (&traf->trun, bw);
  gss_ism_sdtp_serialize (&traf->sdtp, bw, traf->trun.sample_count);
  gss_ism_sample_encryption_serialize (&traf->sample_encryption, bw);

  ATOM_FINISH (bw, offset);
}

static void
gss_ism_mfhd_serialize (AtomMfhd * mfhd, GstByteWriter * bw)
{
  int offset;
  offset = ATOM_INIT (bw, GST_MAKE_FOURCC ('m', 'f', 'h', 'd'));

  gst_byte_writer_put_uint8 (bw, mfhd->version);
  gst_byte_writer_put_uint24_be (bw, mfhd->flags);
  gst_byte_writer_put_uint32_be (bw, mfhd->sequence_number);


  ATOM_FINISH (bw, offset);
}

static void
gss_ism_moof_serialize (GssISMFragment * fragment, GstByteWriter * bw)
{
  int offset;
  offset = ATOM_INIT (bw, GST_MAKE_FOURCC ('m', 'o', 'o', 'f'));

  gss_ism_mfhd_serialize (&fragment->mfhd, bw);
  gss_ism_traf_serialize (&fragment->traf, bw);
  //gss_ism_xmp_data_serialize (&moof->xmp_data, bw);

  ATOM_FINISH (bw, offset);

  GST_WRITE_UINT32_BE (
      (void *) (bw->parent.data + fragment->traf.trun.data_offset_fixup),
      bw->parent.byte + 8);
}

void
gss_ism_fragment_serialize (GssISMFragment * fragment, guint8 ** data,
    int *size)
{
  GstByteWriter *bw;

  bw = gst_byte_writer_new ();

  gss_ism_moof_serialize (fragment, bw);

  *size = bw->parent.byte;
  *data = gst_byte_writer_free_and_get_data (bw);
}

int
gss_ism_fragment_get_n_samples (GssISMFragment * fragment)
{
  return fragment->traf.trun.sample_count;
}

int *
gss_ism_fragment_get_sample_sizes (GssISMFragment * fragment)
{
  int *s;
  int i;
  AtomTrun *trun = &fragment->traf.trun;

  s = g_malloc (sizeof (int) * trun->sample_count);
  for (i = 0; i < trun->sample_count; i++) {
    s[i] = trun->samples[i].size;
  }
  return s;
}

void
gss_ism_encrypt_samples (GssISMFragment * fragment, guint8 * mdat_data,
    guint8 * content_key)
{
  AtomTrun *trun = &fragment->traf.trun;
  AtomUUIDSampleEncryption *se = &fragment->traf.sample_encryption;
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

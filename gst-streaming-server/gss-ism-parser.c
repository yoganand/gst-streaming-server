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

typedef struct _Fragment Fragment;
typedef struct _Track Track;
typedef struct _Movie Movie;

typedef struct _GssISMParser GssISMParser;

typedef enum
{
  GSS_ISOM_FTYP_ISML = (1 << 0),
  GSS_ISOM_FTYP_MP42 = (1 << 1),
  GSS_ISOM_FTYP_MP41 = (1 << 2),
  GSS_ISOM_FTYP_PIFF = (1 << 3),
  GSS_ISOM_FTYP_ISO2 = (1 << 4),
  GSS_ISOM_FTYP_ISOM = (1 << 5),
} GssIsomFtyp;

struct _Movie
{
  int n_tracks;
  Track **tracks;
};

struct _Track
{
  AtomTkhd tkhd;
  AtomTref tref;

  /* inside edts */
  AtomElst elst;

  /* inside mdia */
  AtomMdhd mdhd;
  AtomHdlr hdlr;

  /* inside mdia/minf */
  AtomVmhd vmhd;
  AtomSmhd smhd;
  AtomHmhd hmhd;
  /* mpeg stream headers (?) */

  /* inside mdia/minf/dinf */
  AtomDref dref;

  /* inside mdia/minf/stbl */
  AtomStts stts;
  AtomCtts ctts;
  AtomStss stss;
  AtomStsd stsd;
  AtomStsz stsz;
  AtomStsc stsc;
  AtomStco stco;
  AtomStsh stsh;
  AtomStdp stdp;
};

struct _AtomParser
{
  guint64 offset;
  guint64 size;
};

struct _Fragment
{
  guint64 moof_offset;
  guint64 moof_size;
  guint64 mdat_offset;
  guint64 mdat_size;

  guint64 duration;
  guint64 start_timestamp;

  AtomMoof *moof;
};

struct _GssISMParser
{
  gboolean error;
  int fd;
  guint64 file_size;
  guint64 offset;
  GssIsomFtyp ftyp;
  guint32 ftyp_atom;
  gboolean is_isml;
  gboolean is_mp42;

  Fragment *fragments;
  int n_fragments;
  int n_fragments_alloc;

  Fragment *current_fragment;

  void *moov;
};

static gboolean parser_read (GssISMParser * parser, guint8 * buffer, int offset,
    int n_bytes);
static void gss_ism_parse_ftyp (GssISMParser * parser, guint64 offset,
    guint64 size);
static void gss_ism_parse_moof (GssISMParser * parser, AtomMoof * moof,
    GstByteReader * br);
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
static void gss_ism_parse_moov (GssISMParser * parser, AtomMoov * moov,
    GstByteReader * br);
static void gss_ism_fixup_moof (AtomMoof * moof);

static guint64 gss_ism_moof_get_duration (AtomMoof * moof);


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
gss_ism_parser_get_fragments (GssISMParser * parser, int track_id,
    int *n_fragments)
{
  int n_frags;
  GssISMFragment *fragments;
  guint64 ts;
  int frag_i;
  int i;

  n_frags = gss_ism_parser_get_n_fragments (parser, track_id);
  fragments = g_malloc (n_frags * sizeof (GssISMFragment));
  frag_i = 0;

  ts = 0;
  for (i = 0; i < parser->n_fragments; i++) {
    if (parser->fragments[i].moof->traf.tfhd.track_id == track_id) {
      fragments[frag_i].track_id = track_id;
      fragments[frag_i].moof_offset = parser->fragments[i].moof_offset;
      fragments[frag_i].moof_size = parser->fragments[i].moof_size;
      fragments[frag_i].mdat_offset = parser->fragments[i].mdat_offset;
      fragments[frag_i].mdat_size = parser->fragments[i].mdat_size;
      fragments[frag_i].timestamp = ts;
      fragments[frag_i].duration = parser->fragments[i].duration;
      fragments[frag_i].moof = parser->fragments[i].moof;
      fragments[frag_i].n_samples =
          parser->fragments[i].moof->traf.trun.sample_count;
      ts += fragments[frag_i].duration;
      frag_i++;
    }
  }

  if (n_fragments)
    *n_fragments = n_frags;
  return fragments;
}

int
gss_ism_parser_get_n_fragments (GssISMParser * parser, int track_id)
{
  int i;
  int n;

  n = 0;
  for (i = 0; i < parser->n_fragments; i++) {
    if (parser->fragments[i].moof->traf.tfhd.track_id == track_id) {
      n++;
    }
  }

  return n;
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
      exit (1);
    }
    parser->file_size = sb.st_size;
  }

  parser->offset = 0;
  while (!parser->error && parser->offset < parser->file_size) {
    guint8 buffer[32];
    guint32 size;
    guint32 atom;

    parser_read (parser, buffer, parser->offset, 32);

    size = GST_READ_UINT32_BE (buffer);
    atom = GST_READ_UINT32_LE (buffer + 4);
    if (atom == GST_MAKE_FOURCC ('f', 't', 'y', 'p')) {
      gss_ism_parse_ftyp (parser, parser->offset, size);
    } else if (atom == GST_MAKE_FOURCC ('m', 'o', 'o', 'f')) {
      GstByteReader br;
      guint8 *data;
      AtomMoof *moof;

      data = g_malloc (size);
      parser_read (parser, data, parser->offset, size);
      gst_byte_reader_init (&br, data + 8, size - 8);

      moof = g_malloc0 (sizeof (AtomMoof));
      gss_ism_parse_moof (parser, moof, &br);

      gss_ism_fixup_moof (moof);

      if (parser->n_fragments == parser->n_fragments_alloc) {
        parser->n_fragments_alloc += 100;
        parser->fragments = g_realloc (parser->fragments,
            parser->n_fragments_alloc * sizeof (Fragment));
      }
      parser->current_fragment = &parser->fragments[parser->n_fragments];
      parser->n_fragments++;

      parser->current_fragment->duration = gss_ism_moof_get_duration (moof);
      parser->current_fragment->moof_offset = parser->offset;
      parser->current_fragment->moof_size = size;
      parser->current_fragment->moof = moof;
    } else if (atom == GST_MAKE_FOURCC ('m', 'd', 'a', 't')) {
      if (parser->is_isml && parser->current_fragment == NULL) {
        GST_ERROR ("mdat with no moof, broken file");
        parser->error = TRUE;
        return FALSE;
      }

      parser->current_fragment->mdat_offset = parser->offset;
      parser->current_fragment->mdat_size = size;
    } else if (atom == GST_MAKE_FOURCC ('m', 'f', 'r', 'a')) {
      gss_ism_parse_mfra (parser, parser->offset, size);
    } else if (atom == GST_MAKE_FOURCC ('m', 'o', 'o', 'v')) {
      GstByteReader br;
      guint8 *data;
      AtomMoov *moov;

      data = g_malloc (size);
      parser_read (parser, data, parser->offset, size);
      gst_byte_reader_init (&br, data + 8, size - 8);

      moov = g_malloc0 (sizeof (AtomMoov));
      gss_ism_parse_moov (parser, moov, &br);

      parser->moov = moov;
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
    } else {
      GST_WARNING ("unknown atom %" GST_FOURCC_FORMAT
          " at offset %" G_GINT64_MODIFIER "x, size %d",
          GST_FOURCC_ARGS (atom), parser->offset, size);
    }

    parser->offset += size;
  }

  if (parser->error) {
    GST_ERROR ("parser error");
    return FALSE;
  }

  return TRUE;
}

Track *
gss_ism_track_new (void)
{
  return g_malloc0 (sizeof (Track));
}

void
gss_ism_track_free (Track * track)
{
  g_free (track);
}

Movie *
gss_ism_movie_new (void)
{
  Movie *movie;
  movie = g_malloc0 (sizeof (Movie));
  movie->tracks = g_malloc (10 * sizeof (Track *));
  return movie;
}

void
gss_ism_movie_free (Movie * movie)
{
  g_free (movie->tracks);
  g_free (movie);
}


static gboolean
parser_read (GssISMParser * parser, guint8 * buffer, int offset, int n_bytes)
{
  off_t ret;
  ssize_t n;

  ret = lseek (parser->fd, offset, SEEK_SET);
  if (ret < 0) {
    GST_ERROR ("seek to %d failed", offset);
    return FALSE;
  }

  n = read (parser->fd, buffer, n_bytes);
  if (n != n_bytes) {
    GST_ERROR ("read of %d bytes failed", n_bytes);
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

  data = g_malloc (size);
  parser_read (parser, data, parser->offset, size);

  gst_byte_reader_init (&br, data + 8, size - 8);

  gst_byte_reader_get_uint32_le (&br, &parser->ftyp_atom);
  GST_ERROR ("ftyp: %" GST_FOURCC_FORMAT, GST_FOURCC_ARGS (parser->ftyp_atom));
  if (parser->ftyp_atom == GST_MAKE_FOURCC ('i', 's', 'm', 'l')) {
    parser->is_isml = TRUE;
  } else if (parser->ftyp_atom == GST_MAKE_FOURCC ('m', 'p', '4', '2')) {
    parser->is_mp42 = FALSE;
  } else {
    GST_ERROR ("not isml or mp4 file");
  }
  gst_byte_reader_get_uint32_le (&br, &tmp);
  while (gst_byte_reader_get_uint32_le (&br, &atom)) {
    GST_ERROR ("compat: %" GST_FOURCC_FORMAT, GST_FOURCC_ARGS (atom));
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
gss_ism_parse_moof (GssISMParser * parser, AtomMoof * moof, GstByteReader * br)
{
  while (gst_byte_reader_get_remaining (br) >= 8) {
    guint32 size = 0;
    guint32 atom = 0;
    GstByteReader sbr;

    gst_byte_reader_get_uint32_be (br, &size);
    gst_byte_reader_get_uint32_le (br, &atom);

    gst_byte_reader_init_sub (&sbr, br, size - 8);
    if (atom == GST_MAKE_FOURCC ('m', 'f', 'h', 'd')) {
      gss_ism_parse_mfhd (parser, &moof->mfhd, &sbr);
    } else if (atom == GST_MAKE_FOURCC ('t', 'r', 'a', 'f')) {
      gss_ism_parse_traf (parser, &moof->traf, &sbr);
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
    guint32 size = 0;
    guint32 atom = 0;
    GstByteReader sbr;

    gst_byte_reader_get_uint32_be (br, &size);
    gst_byte_reader_get_uint32_le (br, &atom);

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
          " inside moof at offset %" G_GINT64_MODIFIER "x, size %d",
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
gss_ism_fixup_moof (AtomMoof * moof)
{
  AtomTfhd *tfhd = &moof->traf.tfhd;
  AtomTrun *trun = &moof->traf.trun;
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
gss_ism_moof_get_duration (AtomMoof * moof)
{
  guint64 duration = 0;
  int i;

  for (i = 0; i < moof->traf.trun.sample_count; i++) {
    duration += moof->traf.trun.samples[i].duration;
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
  AtomMoof *moof = fragment->moof;
  AtomUUIDSampleEncryption *se = &moof->traf.sample_encryption;
  AtomTrun *trun = &moof->traf.trun;
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
static void
gss_ism_parse_tkhd (GssISMParser * parser, Track * track, GstByteReader * br)
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
gss_ism_parse_tref (GssISMParser * parser, Track * track, GstByteReader * br)
{
  AtomTref *tref = &track->tref;
  tref->present = TRUE;

  CHECK_END (br);
}

static void
gss_ism_parse_elst (GssISMParser * parser, Track * track, GstByteReader * br)
{
  AtomElst *elst = &track->elst;
  elst->present = TRUE;
  gst_byte_reader_get_uint8 (br, &elst->version);
  gst_byte_reader_get_uint24_be (br, &elst->flags);

  CHECK_END (br);
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
gss_ism_parse_mdhd (GssISMParser * parser, Track * track, GstByteReader * br)
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
  if (parser->ftyp & GSS_ISOM_FTYP_ISML) {
    guint8 len = 0;
    const guint8 *s;
    gst_byte_reader_get_uint8 (br, &len);
    ret = gst_byte_reader_get_data (br, len, &s);
    if (ret) {
      *string = g_malloc (len);
      memcpy (*string, s, len);
      (*string)[len] = 0;
    }
  } else {
    ret = gst_byte_reader_dup_string (br, string);
  }
  return ret;
}

static void
gss_ism_parse_hdlr (GssISMParser * parser, Track * track, GstByteReader * br)
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
gss_ism_parse_vmhd (GssISMParser * parser, Track * track, GstByteReader * br)
{
  AtomVmhd *vmhd = &track->vmhd;
  vmhd->present = TRUE;
  gst_byte_reader_get_uint8 (br, &vmhd->version);
  gst_byte_reader_get_uint24_be (br, &vmhd->flags);
  gst_byte_reader_skip (br, 8);

  CHECK_END (br);
}

static void
gss_ism_parse_smhd (GssISMParser * parser, Track * track, GstByteReader * br)
{
  AtomSmhd *smhd = &track->smhd;
  smhd->present = TRUE;
  gst_byte_reader_get_uint8 (br, &smhd->version);
  gst_byte_reader_get_uint24_be (br, &smhd->flags);
  gst_byte_reader_skip (br, 4);

  CHECK_END (br);
}

static void
gss_ism_parse_hmhd (GssISMParser * parser, Track * track, GstByteReader * br)
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
gss_ism_parse_dref (GssISMParser * parser, Track * track, GstByteReader * br)
{
  AtomDref *dref = &track->dref;
  int i;
  dref->present = TRUE;
  gst_byte_reader_get_uint8 (br, &dref->version);
  gst_byte_reader_get_uint24_be (br, &dref->flags);
  gst_byte_reader_get_uint32_be (br, &dref->entry_count);
  dref->entries = g_malloc0 (sizeof (AtomDrefEntry) * dref->entry_count);
  for (i = 0; i < dref->entry_count; i++) {
    guint32 size = 0;
    guint32 atom = 0;
    GstByteReader sbr;

    gst_byte_reader_get_uint32_be (br, &size);
    gst_byte_reader_get_uint32_le (br, &atom);

    gst_byte_reader_init_sub (&sbr, br, size - 8);
    if (atom == GST_MAKE_FOURCC ('u', 'r', 'l', ' ')) {
      GST_ERROR ("url_");
    } else if (atom == GST_MAKE_FOURCC ('u', 'r', 'n', ' ')) {
      GST_ERROR ("urn_");
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
gss_ism_parse_stts (GssISMParser * parser, Track * track, GstByteReader * br)
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
gss_ism_parse_ctts (GssISMParser * parser, Track * track, GstByteReader * br)
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
gss_ism_parse_stsz (GssISMParser * parser, Track * track, GstByteReader * br)
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
gss_ism_parse_stsc (GssISMParser * parser, Track * track, GstByteReader * br)
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
gss_ism_parse_stco (GssISMParser * parser, Track * track, GstByteReader * br)
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
gss_ism_parse_co64 (GssISMParser * parser, Track * track, GstByteReader * br)
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
gss_ism_parse_stss (GssISMParser * parser, Track * track, GstByteReader * br)
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
gss_ism_parse_stsh (GssISMParser * parser, Track * track, GstByteReader * br)
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
gss_ism_parse_stdp (GssISMParser * parser, Track * track, GstByteReader * br)
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
gss_ism_parse_stsd (GssISMParser * parser, Track * track, GstByteReader * br)
{
  AtomStsd *stsd = &track->stsd;
  int i;
  stsd->present = TRUE;
  gst_byte_reader_get_uint8 (br, &stsd->version);
  gst_byte_reader_get_uint24_be (br, &stsd->flags);
  gst_byte_reader_get_uint32_be (br, &stsd->entry_count);
  for (i = 0; i < stsd->entry_count; i++) {
    guint32 size = 0;
    guint32 atom = 0;
    GstByteReader sbr;

    gst_byte_reader_get_uint32_be (br, &size);
    gst_byte_reader_get_uint32_le (br, &atom);

    gst_byte_reader_init_sub (&sbr, br, size - 8);
    if (atom == GST_MAKE_FOURCC ('m', 'p', '4', 'a')) {
      GST_ERROR ("mp4a");
    } else if (atom == GST_MAKE_FOURCC ('a', 'v', 'c', '1')) {
      GST_ERROR ("avc1");
    } else if (atom == GST_MAKE_FOURCC ('e', 'n', 'c', 'v')) {
      GST_ERROR ("encv");
    } else if (atom == GST_MAKE_FOURCC ('e', 'n', 'c', 'a')) {
      GST_ERROR ("enca");
    } else {
      GST_WARNING ("unknown atom %" GST_FOURCC_FORMAT " inside stsd, size %d",
          GST_FOURCC_ARGS (atom), size);
    }

    gst_byte_reader_skip (br, size - 8);
  }

  gst_byte_reader_dump (br);

  CHECK_END (br);
}

typedef struct _ContainerAtoms ContainerAtoms;
struct _ContainerAtoms
{
  guint32 atom;
  void (*parse) (GssISMParser * parser, Track * track, GstByteReader * br);
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
  {0}
};

static ContainerAtoms minf_atoms[] = {
  {GST_MAKE_FOURCC ('v', 'm', 'h', 'd'), gss_ism_parse_vmhd},
  {GST_MAKE_FOURCC ('s', 'm', 'h', 'd'), gss_ism_parse_smhd},
  {GST_MAKE_FOURCC ('h', 'm', 'h', 'd'), gss_ism_parse_hmhd},
  {GST_MAKE_FOURCC ('d', 'i', 'n', 'f'), NULL, dinf_atoms},
  {GST_MAKE_FOURCC ('s', 't', 'b', 'l'), NULL, stbl_atoms},
  {0}
};

static ContainerAtoms mdia_atoms[] = {
  {GST_MAKE_FOURCC ('m', 'd', 'h', 'd'), gss_ism_parse_mdhd},
  {GST_MAKE_FOURCC ('h', 'd', 'l', 'r'), gss_ism_parse_hdlr},
  {GST_MAKE_FOURCC ('m', 'i', 'n', 'f'), NULL, minf_atoms},
  {0}
};

static ContainerAtoms edts_atoms[] = {
  {GST_MAKE_FOURCC ('e', 'l', 's', 't'), gss_ism_parse_elst},
  {0}
};

static ContainerAtoms trak_atoms[] = {
  {GST_MAKE_FOURCC ('t', 'k', 'h', 'd'), gss_ism_parse_tkhd},
  {GST_MAKE_FOURCC ('t', 'r', 'e', 'f'), gss_ism_parse_tref},
  {GST_MAKE_FOURCC ('e', 'd', 't', 's'), NULL, edts_atoms},
  {GST_MAKE_FOURCC ('m', 'd', 'i', 'a'), NULL, mdia_atoms},
  {0}
};


static void
gss_ism_parse_container (GssISMParser * parser, Track * track,
    GstByteReader * br, ContainerAtoms * atoms, guint32 parent_atom)
{
  while (gst_byte_reader_get_remaining (br) >= 8) {
    guint32 size = 0;
    guint32 atom = 0;
    int i;
    GstByteReader sbr;

    gst_byte_reader_get_uint32_be (br, &size);
    gst_byte_reader_get_uint32_le (br, &atom);

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
gss_ism_parse_moov (GssISMParser * parser, AtomMoov * moov, GstByteReader * br)
{
  Movie *movie;

  movie = gss_ism_movie_new ();
  while (gst_byte_reader_get_remaining (br) >= 8) {
    guint32 size = 0;
    guint32 atom = 0;
    GstByteReader sbr;

    gst_byte_reader_get_uint32_be (br, &size);
    gst_byte_reader_get_uint32_le (br, &atom);

    gst_byte_reader_init_sub (&sbr, br, size - 8);
    if (atom == GST_MAKE_FOURCC ('m', 'v', 'h', 'd')) {
      gss_ism_parse_mvhd (parser, &moov->mvhd, &sbr);
    } else if (atom == GST_MAKE_FOURCC ('t', 'r', 'a', 'k')) {
      Track *track;
      track = gss_ism_track_new ();
      gss_ism_parse_container (parser, track, &sbr, trak_atoms, atom);
      movie->tracks[movie->n_tracks] = track;
      movie->n_tracks++;
    } else if (atom == GST_MAKE_FOURCC ('u', 'd', 't', 'a')) {
      gss_ism_parse_udta (parser, &moov->udta, &sbr);
    } else if (atom == GST_MAKE_FOURCC ('m', 'v', 'e', 'x')) {
      gss_ism_parse_mvex (parser, &moov->mvex, &sbr);
    } else if (atom == GST_MAKE_FOURCC ('u', 'u', 'i', 'd')) {
      const guint8 *uuid = NULL;

      gst_byte_reader_peek_data (br, 16, &uuid);
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
gss_ism_moof_serialize (AtomMoof * moof, GstByteWriter * bw)
{
  int offset;
  offset = ATOM_INIT (bw, GST_MAKE_FOURCC ('m', 'o', 'o', 'f'));

  gss_ism_mfhd_serialize (&moof->mfhd, bw);
  gss_ism_traf_serialize (&moof->traf, bw);
  //gss_ism_xmp_data_serialize (&moof->xmp_data, bw);

  ATOM_FINISH (bw, offset);

  GST_WRITE_UINT32_BE (
      (void *) (bw->parent.data + moof->traf.trun.data_offset_fixup),
      bw->parent.byte + 8);
}

void
gss_ism_fragment_serialize (GssISMFragment * fragment, guint8 ** data,
    int *size)
{
  GstByteWriter *bw;
  AtomMoof *moof = fragment->moof;

  bw = gst_byte_writer_new ();

  gss_ism_moof_serialize (moof, bw);

  *size = bw->parent.byte;
  *data = gst_byte_writer_free_and_get_data (bw);
}

int *
gss_ism_fragment_get_sample_sizes (GssISMFragment * fragment)
{
  int *s;
  int i;
  AtomTrun *trun = &((AtomMoof *) fragment->moof)->traf.trun;

  s = g_malloc (sizeof (int) * fragment->n_samples);
  for (i = 0; i < fragment->n_samples; i++) {
    s[i] = trun->samples[i].size;
  }
  return s;
}

void
gss_ism_encrypt_samples (GssISMFragment * fragment, guint8 * mdat_data,
    guint8 * content_key)
{
  AtomMoof *moof = fragment->moof;
  AtomTrun *trun = &((AtomMoof *) fragment->moof)->traf.trun;
  AtomUUIDSampleEncryption *se = &moof->traf.sample_encryption;
  guint64 sample_offset;
  int i;

  sample_offset = 8;

  for (i = 0; i < fragment->n_samples; i++) {
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

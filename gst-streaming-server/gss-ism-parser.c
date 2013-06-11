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
#include "gss-playready.h"

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <openssl/aes.h>

typedef struct _Fragment Fragment;

typedef struct _GssISMParser GssISMParser;

typedef struct _AtomMoof AtomMoof;
typedef struct _AtomMfhd AtomMfhd;
typedef struct _AtomTfhd AtomTfhd;
typedef struct _AtomTrun AtomTrun;
typedef struct _AtomSdtp AtomSdtp;
typedef struct _AtomTraf AtomTraf;
typedef struct _AtomTrunSample AtomTrunSample;
typedef struct _AtomUUIDProtectionHeader AtomUUIDProtectionHeader;
typedef struct _AtomUUIDSampleEncryption AtomUUIDSampleEncryption;
typedef struct _AtomUUIDSampleEncryptionSample AtomUUIDSampleEncryptionSample;
typedef struct _AtomUUIDSampleEncryptionSampleEntry
    AtomUUIDSampleEncryptionSampleEntry;

struct _AtomMfhd
{
  guint8 version;
  guint32 flags;

  guint32 sequence_number;
};

struct _AtomTfhd
{
  guint8 version;
  guint32 flags;

  guint32 track_id;
  guint32 default_sample_duration;
  guint32 default_sample_size;
  guint32 default_sample_flags;
};

struct _AtomTrun
{
  guint8 version;
  guint32 flags;

  guint32 sample_count;
  guint32 data_offset;
  guint32 first_sample_flags;

  AtomTrunSample *samples;

  /* This is a special field used while writing: it stores the offset
   * to the data_offset location, so it can be fixed up once we know
   * the size of the entire moof atom */
  int data_offset_fixup;
};

struct _AtomTrunSample
{
  guint32 duration;
  guint32 size;
  guint32 flags;
  guint32 composition_time_offset;
};

struct _AtomSdtp
{
  guint8 version;
  guint32 flags;

  guint8 *sample_flags;

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

struct _AtomUUIDSampleEncryption
{
  gboolean present;
  guint8 version;
  guint32 flags;
  guint32 algorithm_id;
  guint8 iv_size;
  guint8 kid[16];
  guint32 sample_count;
  AtomUUIDSampleEncryptionSample *samples;
};

#define MAX_IV_SIZE 16
struct _AtomUUIDSampleEncryptionSample
{
  guint64 iv;
  guint16 num_entries;
  AtomUUIDSampleEncryptionSampleEntry *entries;
};

struct _AtomUUIDSampleEncryptionSampleEntry
{
  guint16 bytes_of_clear_data;
  guint32 bytes_of_encrypted_data;
};

struct _AtomTraf
{
  guint8 version;
  guint32 flags;

  AtomTfhd tfhd;
  AtomTrun trun;
  AtomSdtp sdtp;
  AtomUUIDSampleEncryption sample_encryption;
};

struct _AtomMoof
{
  guint8 version;
  guint32 flags;

  AtomMfhd mfhd;
  AtomTraf traf;
};

/* ProtectionSystemSpecificHeaderBox */
/* 0xd08a4f18-10f3-4a82-b6c8-32d8aba183d3 */
const guint8 uuid_protection_header[16] = {
  0xd0, 0x8a, 0x4f, 0x18, 0x10, 0xf3, 0x4a, 0x82,
  0xb6, 0xc8, 0x32, 0xd8, 0xab, 0xa1, 0x83, 0xd3
};

struct _AtomUUIDProtectionHeader
{
  guint8 version;
  guint32 flags;
  guint8 system_id[16];
  guint32 data_size;
  guint8 *data;
};

enum TrFlags
{
  TR_DATA_OFFSET = 0x01,        /* data-offset-present */
  TR_FIRST_SAMPLE_FLAGS = 0x04, /* first-sample-flags-present */
  TR_SAMPLE_DURATION = 0x0100,  /* sample-duration-present */
  TR_SAMPLE_SIZE = 0x0200,      /* sample-size-present */
  TR_SAMPLE_FLAGS = 0x0400,     /* sample-flags-present */
  TR_SAMPLE_COMPOSITION_TIME_OFFSETS = 0x0800   /* sample-composition-time-offsets-pre
                                                   sents */
};

enum TfFlags
{
  TF_BASE_DATA_OFFSET = 0x01,   /* base-data-offset-present */
  TF_SAMPLE_DESCRIPTION_INDEX = 0x02,   /* sample-description-index-present */
  TF_DEFAULT_SAMPLE_DURATION = 0x08,    /* default-sample-duration-present */
  TF_DEFAULT_SAMPLE_SIZE = 0x010,       /* default-sample-size-present */
  TF_DEFAULT_SAMPLE_FLAGS = 0x020,      /* default-sample-flags-present */
  TF_DURATION_IS_EMPTY = 0x010000       /* sample-composition-time-offsets-presents */
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

  Fragment *fragments;
  int n_fragments;
  int n_fragments_alloc;

  Fragment *current_fragment;
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
      if (parser->current_fragment == NULL) {
        GST_ERROR ("mdat with no moof, broken file");
        parser->error = TRUE;
        return FALSE;
      }

      parser->current_fragment->mdat_offset = parser->offset;
      parser->current_fragment->mdat_size = size;
    } else if (atom == GST_MAKE_FOURCC ('m', 'f', 'r', 'a')) {
      gss_ism_parse_mfra (parser, parser->offset, size);
    } else if (atom == GST_MAKE_FOURCC ('m', 'o', 'o', 'v')) {
      /* ignore moov atom */
    } else if (atom == GST_MAKE_FOURCC ('u', 'u', 'i', 'd')) {
      const guint8 *uuid = NULL;

      uuid = buffer + 8;

      if (memcmp (uuid, uuid_xmp_data, 16) == 0) {

      } else {
        GST_ERROR
            ("UUID: %02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x\n",
            uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6],
            uuid[7], uuid[8], uuid[9], uuid[10], uuid[11], uuid[12], uuid[13],
            uuid[14], uuid[15]);
      }
    } else {
      GST_ERROR ("unknown atom %" GST_FOURCC_FORMAT
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
  GST_ERROR ("ftyp");
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
        GST_ERROR
            ("UUID: %02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x\n",
            uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6],
            uuid[7], uuid[8], uuid[9], uuid[10], uuid[11], uuid[12], uuid[13],
            uuid[14], uuid[15]);
      }
    } else {
      GST_ERROR ("unknown atom %" GST_FOURCC_FORMAT
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
        GST_ERROR
            ("UUID: %02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x\n",
            uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6],
            uuid[7], uuid[8], uuid[9], uuid[10], uuid[11], uuid[12], uuid[13],
            uuid[14], uuid[15]);
      }
    } else {
      GST_ERROR ("unknown atom %" GST_FOURCC_FORMAT
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
#if 0
    g_print ("trun %d %d %d %d\n",
        trun->samples[i].duration,
        trun->samples[i].size,
        trun->samples[i].flags, trun->samples[i].composition_time_offset);
#endif
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
  GST_ERROR ("flags %08x", se->flags);
  if (se->flags & 0x0001) {
    gst_byte_reader_get_uint24_be (br, &se->algorithm_id);
    gst_byte_reader_get_uint8 (br, &se->iv_size);
    GST_ERROR ("iv size %d", se->iv_size);
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

#if 0
static guint8 dumb_aes128_key[16] = {
  0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
  0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
};
#endif

void
gss_ism_encrypt_samples (GssISMFragment * fragment, guint8 * mdat_data)
{
  AtomMoof *moof = fragment->moof;
  AtomTrun *trun = &((AtomMoof *) fragment->moof)->traf.trun;
  AtomUUIDSampleEncryption *se = &moof->traf.sample_encryption;
  guint64 sample_offset;
  int i;
  guint8 *content_key;

  sample_offset = 8;

  {
    guint8 *kid;
    gsize kid_len;
    guint8 *seed;

    kid = g_base64_decode ("AmfjCTOPbEOl3WD/5mcecA==", &kid_len);
    seed = gss_playready_get_default_key_seed ();

    GST_ERROR ("kid len %" G_GSIZE_FORMAT, kid_len);

    content_key = gss_playready_generate_key (seed, 30, kid, kid_len);

    g_free (kid);
    g_free (seed);
  }


  for (i = 0; i < fragment->n_samples; i++) {
    unsigned char raw_iv[16];
    unsigned char ecount_buf[16] = { 0 };
    unsigned int num = 0;
    AES_KEY key;

    memset (raw_iv, 0, 16);
    GST_WRITE_UINT64_BE (raw_iv, se->samples[i].iv);

    AES_set_encrypt_key (content_key, 16 * 8, &key);

    //GST_ERROR ("enc offset %" G_GUINT64_FORMAT " size %" G_GUINT64_FORMAT,
    //    enc_offset, size);

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
    //GST_ERROR ("num %d", num);
  }

  g_free (content_key);
}

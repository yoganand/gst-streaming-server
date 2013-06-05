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
#include "gss-vod.h"
#include "gss-ism-parser.h"

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

typedef struct _Fragment Fragment;

typedef struct _GssISMParser GssISMParser;

typedef struct _AtomMfhd AtomMfhd;
typedef struct _AtomTfhd AtomTfhd;
typedef struct _AtomTrun AtomTrun;
typedef struct _AtomTrunSample AtomTrunSample;

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
};

struct _AtomTrunSample
{
  guint32 duration;
  guint32 size;
  guint32 flags;
  guint32 composition_time_offset;
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
  guint64 offset;
  guint64 size;
  guint64 start_timestamp;

  AtomTrun trun;
  AtomMfhd mfhd;
  AtomTfhd tfhd;
  guint64 duration;
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
static void gss_ism_parse_moof (GssISMParser * parser, guint64 offset,
    guint64 size);
static void gss_ism_parse_mdat (GssISMParser * parser, guint64 offset,
    guint64 size);
static void gss_ism_parse_mfra (GssISMParser * parser, guint64 offset,
    guint64 size);
static void atom_parse (GssISMParser * parser, guint64 base_offset,
    guint64 parent_size, int indent);


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
    if (parser->fragments[i].tfhd.track_id == track_id) {
      fragments[frag_i].offset = parser->fragments[i].offset;
      fragments[frag_i].size = parser->fragments[i].size;
      fragments[frag_i].timestamp = ts;
      fragments[frag_i].duration = parser->fragments[i].duration;
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
    if (parser->fragments[i].tfhd.track_id == track_id) {
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
    guint8 buffer[16];
    guint32 size;
    guint32 atom;

    parser_read (parser, buffer, parser->offset, 16);

    size = GST_READ_UINT32_BE (buffer);
    atom = GST_READ_UINT32_LE (buffer + 4);
    if (atom == GST_MAKE_FOURCC ('f', 't', 'y', 'p')) {
      gss_ism_parse_ftyp (parser, parser->offset, size);
    } else if (atom == GST_MAKE_FOURCC ('m', 'o', 'o', 'f')) {
      gss_ism_parse_moof (parser, parser->offset, size);
    } else if (atom == GST_MAKE_FOURCC ('m', 'd', 'a', 't')) {
      gss_ism_parse_mdat (parser, parser->offset, size);
    } else if (atom == GST_MAKE_FOURCC ('m', 'f', 'r', 'a')) {
      gss_ism_parse_mfra (parser, parser->offset, size);
    } else if (atom == GST_MAKE_FOURCC ('m', 'o', 'o', 'v')) {
      /* ignore moov atom */
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
gss_ism_parse_moof (GssISMParser * parser, guint64 offset, guint64 size)
{
  GstByteReader br;
  guint8 *data;

  data = g_malloc (size);
  parser_read (parser, data, offset, size);

  gst_byte_reader_init (&br, data + 8, size - 8);

  if (parser->n_fragments == parser->n_fragments_alloc) {
    parser->n_fragments_alloc += 100;
    parser->fragments = g_realloc (parser->fragments,
        parser->n_fragments_alloc * sizeof (Fragment));
  }
  parser->current_fragment = &parser->fragments[parser->n_fragments];
  parser->n_fragments++;

  parser->current_fragment->offset = offset;
  parser->current_fragment->size = size;

  atom_parse (parser, offset + 8, size - 8, 2);

  g_free (data);

#if 0
  g_print ("frag: track %d duration %" G_GUINT64_FORMAT "\n",
      parser->current_fragment->tfhd.track_id,
      parser->current_fragment->duration);
#endif
}

static void
gss_ism_parse_mdat (GssISMParser * parser, guint64 offset, guint64 size)
{
  if (parser->current_fragment == NULL) {
    GST_ERROR ("mdat with no moof, broken file");
    parser->error = TRUE;
    return;
  }

  parser->current_fragment->size += size;
}

static void
gss_ism_parse_mfra (GssISMParser * parser, guint64 offset, guint64 size)
{
#if 0
  int i;

  for (i = 0; i < parser->n_fragments; i++) {
    g_print ("%08llx\n", (unsigned long long) parser->fragments[i].offset);
  }
#endif
#if 0
  for (i = 0; i < size; i += 16) {
    guint8 buffer[16];
    gboolean ret;

    ret = parser_read (parser, buffer, offset + i, 16);
    g_print
        ("%08lx: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
        offset, buffer[0], buffer[1], buffer[2], buffer[3], buffer[4],
        buffer[5], buffer[6], buffer[7], buffer[8], buffer[9], buffer[10],
        buffer[11], buffer[12], buffer[13], buffer[14], buffer[15]);
  }
#endif
  atom_parse (parser, offset + 8, size - 8, 2);
}



static void
atom_parse (GssISMParser * parser, guint64 base_offset, guint64 parent_size,
    int indent)
{
  guint64 offset = 0;
  guint8 buffer[16];

  while (offset < parent_size) {
    gboolean ret;
    guint32 size;
    guint32 atom;

    ret = parser_read (parser, buffer, base_offset + offset, 16);
    if (!ret)
      break;

#if 0
    g_print
        ("%*.*s%08x: %02x %02x %02x %02x %c%c%c%c %02x %02x %02x %02x %02x %02x %02x %02x\n",
        indent, indent, "                ", (int) offset, buffer[0], buffer[1],
        buffer[2], buffer[3], buffer[4], buffer[5], buffer[6], buffer[7],
        buffer[8], buffer[9], buffer[10], buffer[11], buffer[12], buffer[13],
        buffer[14], buffer[15]);
#endif

    size = GST_READ_UINT32_BE (buffer);
    atom = GST_READ_UINT32_LE (buffer + 4);

    if (atom == GST_MAKE_FOURCC ('t', 'r', 'a', 'f')) {
      atom_parse (parser, base_offset + offset + 8, size - 8, indent + 2);
    } else if (atom == GST_MAKE_FOURCC ('m', 'f', 'h', 'd')) {
      GstByteReader br = { 0 };
      guint8 *data;
      Fragment *fragment = &parser->fragments[parser->n_fragments - 1];
      AtomMfhd *mfhd = &fragment->mfhd;

      data = g_malloc (size);
      parser_read (parser, data, base_offset + offset, size);

      gst_byte_reader_init (&br, data + 8, size - 8);

      gst_byte_reader_get_uint8 (&br, &mfhd->version);
      gst_byte_reader_get_uint24_be (&br, &mfhd->flags);
      gst_byte_reader_get_uint32_be (&br, &mfhd->sequence_number);

    } else if (atom == GST_MAKE_FOURCC ('t', 'r', 'u', 'n')) {
      GstByteReader br = { 0 };
      guint8 *data;
      Fragment *fragment = &parser->fragments[parser->n_fragments - 1];
      AtomTrun *trun = &fragment->trun;
      AtomTfhd *tfhd = &fragment->tfhd;
      int i;

      data = g_malloc (size);
      parser_read (parser, data, base_offset + offset, size);

      gst_byte_reader_init (&br, data + 8, size - 8);

      gst_byte_reader_get_uint8 (&br, &trun->version);
      gst_byte_reader_get_uint24_be (&br, &trun->flags);
      gst_byte_reader_get_uint32_be (&br, &trun->sample_count);
      if (trun->flags & TR_DATA_OFFSET) {
        gst_byte_reader_get_uint32_be (&br, &trun->data_offset);
      }
      if (trun->flags & TR_FIRST_SAMPLE_FLAGS) {
        gst_byte_reader_get_uint32_be (&br, &trun->first_sample_flags);
      }

      trun->samples = g_malloc0 (sizeof (AtomTrunSample) * trun->sample_count);
      fragment->duration = 0;
      for (i = 0; i < trun->sample_count; i++) {
        if (trun->flags & TR_SAMPLE_DURATION) {
          gst_byte_reader_get_uint32_be (&br, &trun->samples[i].duration);
        } else {
          trun->samples[i].duration = tfhd->default_sample_duration;
        }
        if (trun->flags & TR_SAMPLE_SIZE) {
          gst_byte_reader_get_uint32_be (&br, &trun->samples[i].size);
        }
        if (trun->flags & TR_SAMPLE_FLAGS) {
          gst_byte_reader_get_uint32_be (&br, &trun->samples[i].flags);
        } else {
          trun->samples[i].flags = tfhd->default_sample_duration;
        }
        if (trun->flags & TR_SAMPLE_COMPOSITION_TIME_OFFSETS) {
          gst_byte_reader_get_uint32_be (&br,
              &trun->samples[i].composition_time_offset);
        }
#if 0
        g_print ("trun %d %d %d %d\n",
            trun->samples[i].duration,
            trun->samples[i].size,
            trun->samples[i].flags, trun->samples[i].composition_time_offset);
#endif
        fragment->duration += trun->samples[i].duration;
      }

    } else if (atom == GST_MAKE_FOURCC ('t', 'f', 'h', 'd')) {
      GstByteReader br = { 0 };
      guint8 *data;
      Fragment *fragment = &parser->fragments[parser->n_fragments - 1];
      AtomTfhd *tfhd = &fragment->tfhd;

      data = g_malloc (size);
      parser_read (parser, data, base_offset + offset, size);

      gst_byte_reader_init (&br, data + 8, size - 8);

      gst_byte_reader_get_uint8 (&br, &tfhd->version);
      gst_byte_reader_get_uint24_be (&br, &tfhd->flags);
      gst_byte_reader_get_uint32_be (&br, &tfhd->track_id);
      if (tfhd->flags & TF_SAMPLE_DESCRIPTION_INDEX) {
        gst_byte_reader_skip (&br, 4);
      }
      if (tfhd->flags & TF_DEFAULT_SAMPLE_DURATION) {
        gst_byte_reader_get_uint32_be (&br, &tfhd->default_sample_duration);
      }
      if (tfhd->flags & TF_DEFAULT_SAMPLE_SIZE) {
        gst_byte_reader_get_uint32_be (&br, &tfhd->default_sample_size);
      }
      if (tfhd->flags & TF_DEFAULT_SAMPLE_FLAGS) {
        gst_byte_reader_get_uint32_be (&br, &tfhd->default_sample_flags);
      }
    } else {
#if 0
      int i;
      for (i = 8; i < size; i += 8) {
        ret = parser_read (parser, buffer, base_offset + offset + i, 8);
        if (!ret)
          break;

        g_print ("%*.*s  %08x: %02x %02x %02x %02x %02x %02x %02x %02x\n",
            indent, indent, "                ",
            (int) offset + i,
            buffer[0], buffer[1], buffer[2], buffer[3],
            buffer[4], buffer[5], buffer[6], buffer[7]);
      }
#endif
    }

    offset += size;
  }
}

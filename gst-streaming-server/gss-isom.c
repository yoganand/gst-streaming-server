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
#include "gss-isom.h"
#include "gss-iso-atoms.h"
#include "gss-playready.h"

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <openssl/aes.h>

typedef struct _ContainerAtoms ContainerAtoms;
struct _ContainerAtoms
{
  guint32 atom;
  void (*parse) (GssIsomFile * file, GssIsomTrack * track, GstByteReader * br);
  ContainerAtoms *atoms;
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


static gboolean file_read (GssIsomFile * file, guint8 * buffer,
    guint64 offset, guint64 n_bytes);
static void gss_isom_parse_ftyp (GssIsomFile * file, guint64 offset,
    guint64 size);
static void gss_isom_parse_moof (GssIsomFile * file,
    GssIsomFragment * fragment, GstByteReader * br);
static void gss_isom_parse_traf (GssIsomFile * file, AtomTraf * traf,
    GstByteReader * br);
static void gss_isom_parse_mfhd (GssIsomFile * file, AtomMfhd * mfhd,
    GstByteReader * br);
static void gss_isom_parse_tfhd (GssIsomFile * file, AtomTfhd * tfhd,
    GstByteReader * br);
static void gss_isom_parse_trun (GssIsomFile * file, AtomTrun * trun,
    GstByteReader * br);
static void gss_isom_parse_sdtp (GssIsomFile * file, AtomSdtp * sdtp,
    GstByteReader * br, int sample_count);
static void gss_isom_parse_mfra (GssIsomFile * file, guint64 offset,
    guint64 size);
static void gss_isom_parse_sample_encryption (GssIsomFile * file,
    AtomUUIDSampleEncryption * se, GstByteReader * br);
static void gss_isom_parse_moov (GssIsomFile * file, GssIsomMovie * movie,
    GstByteReader * br);
static void gss_isom_fixup_moof (GssIsomFragment * fragment);
static void gss_isom_file_fixup (GssIsomFile * file);
static void gss_isom_parse_container (GssIsomFile * file,
    GssIsomTrack * track, GstByteReader * br, ContainerAtoms * atoms,
    guint32 parent_atom);
static void gss_isom_parse_ignore (GssIsomFile * file, GssIsomTrack * track,
    GstByteReader * br);

static guint64 gss_isom_moof_get_duration (GssIsomFragment * fragment);


GssIsomFile *
gss_isom_file_new (void)
{
  GssIsomFile *file;

  file = g_malloc0 (sizeof (GssIsomFile));

  return file;
}

void
gss_isom_file_free (GssIsomFile * file)
{
  if (file->fd > 0) {
    close (file->fd);
  }
  g_free (file->fragments);
  g_free (file);
}

GssIsomFragment *
gss_isom_file_get_fragment (GssIsomFile * file, int track_id, int index)
{
  int frag_i;
  int i;

  frag_i = 0;

  for (i = 0; i < file->n_fragments; i++) {
    if (file->fragments[i]->traf.tfhd.track_id == track_id) {
      if (frag_i == index) {
        return file->fragments[i];
      }
      frag_i++;
    }
  }

  return NULL;
}

GssIsomFragment *
gss_isom_file_get_fragment_by_timestamp (GssIsomFile * file,
    int track_id, guint64 timestamp)
{
  int i;

  for (i = 0; i < file->n_fragments; i++) {
    if (file->fragments[i]->traf.tfhd.track_id == track_id) {
      if (file->fragments[i]->timestamp == timestamp) {
        return file->fragments[i];
      }
    }
  }

  return NULL;
}

guint64
gss_isom_file_get_duration (GssIsomFile * file, int track_id)
{
  int i;
  guint64 duration;

  duration = 0;
  for (i = 0; i < file->n_fragments; i++) {
    if (file->fragments[i]->traf.tfhd.track_id == track_id) {
      duration += file->fragments[i]->duration;
    }
  }

  return duration;
}

int
gss_isom_file_get_n_fragments (GssIsomFile * file, int track_id)
{
  int i;
  int n;

  n = 0;
  for (i = 0; i < file->n_fragments; i++) {
    if (file->fragments[i]->traf.tfhd.track_id == track_id) {
      n++;
    }
  }

  return n;
}

void
gss_isom_file_load_chunk (GssIsomFile * file, guint64 offset, guint64 size)
{
  if (file->data) {
    g_free (file->data);
  }
  file->data = g_malloc (size);
  file->data_offset = offset;
  file->data_size = size;

  file_read (file, file->data, file->data_offset, file->data_size);
}

gboolean
gss_isom_file_parse_file (GssIsomFile * file, const char *filename)
{
  file->fd = open (filename, O_RDONLY);
  if (file->fd < 0) {
    GST_ERROR ("cannot open %s", filename);
    return FALSE;
  }

  {
    int ret;
    struct stat sb;

    ret = fstat (file->fd, &sb);
    if (ret < 0) {
      GST_ERROR ("stat failed");
      close (file->fd);
      file->fd = -1;
      return FALSE;
    }
    file->file_size = sb.st_size;
  }

  file->offset = 0;
  while (!file->error && file->offset < file->file_size) {
    guint8 buffer[16];
    guint64 size = 0;
    guint32 size32 = 0;
    guint32 atom = 0;
    GstByteReader br;

    file_read (file, buffer, file->offset, 16);
    gst_byte_reader_init (&br, buffer, 16);

    gst_byte_reader_get_uint32_be (&br, &size32);
    gst_byte_reader_get_uint32_le (&br, &atom);
    if (size32 == 1) {
      gst_byte_reader_get_uint64_be (&br, &size);
    } else if (size32 == 0) {
      size = file->file_size - file->offset;
    } else {
      size = size32;
    }

    if (atom == GST_MAKE_FOURCC ('f', 't', 'y', 'p')) {
      gss_isom_file_load_chunk (file, file->offset, size);

      gss_isom_parse_ftyp (file, file->offset, size);
    } else if (atom == GST_MAKE_FOURCC ('m', 'o', 'o', 'f')) {
      GstByteReader br;
      GssIsomFragment *fragment;

      gss_isom_file_load_chunk (file, file->offset, size);
      gst_byte_reader_init (&br, file->data + 8, size - 8);

      fragment = gss_isom_fragment_new ();
      gss_isom_parse_moof (file, fragment, &br);
      gss_isom_fixup_moof (fragment);

      if (file->n_fragments == file->n_fragments_alloc) {
        file->n_fragments_alloc += 100;
        file->fragments = g_realloc (file->fragments,
            file->n_fragments_alloc * sizeof (GssIsomFragment *));
      }
      file->fragments[file->n_fragments] = fragment;
      file->n_fragments++;
      file->current_fragment = fragment;

      fragment->duration = gss_isom_moof_get_duration (fragment);
      fragment->moof_offset = file->offset;
      fragment->moof_size = size;
    } else if (atom == GST_MAKE_FOURCC ('m', 'd', 'a', 't')) {
      if (file->is_isml && file->current_fragment == NULL) {
        GST_ERROR ("mdat with no moof, broken file");
        file->error = TRUE;
        close (file->fd);
        file->fd = -1;
        return FALSE;
      }

      if (file->is_isml) {
        file->current_fragment->mdat_size = size;

        file->current_fragment->n_mdat_chunks = 1;
        file->current_fragment->chunks = g_malloc (sizeof (GssMdatChunk) * 1);
        file->current_fragment->chunks[0].offset = file->offset + 8;
        file->current_fragment->chunks[0].size = size - 8;
      }
    } else if (atom == GST_MAKE_FOURCC ('m', 'f', 'r', 'a')) {
      gss_isom_parse_mfra (file, file->offset, size);
    } else if (atom == GST_MAKE_FOURCC ('m', 'o', 'o', 'v')) {
      GstByteReader br;
      guint8 *data;
      GssIsomMovie *movie;

      data = g_malloc (size);
      file_read (file, data, file->offset, size);
      gst_byte_reader_init (&br, data + 8, size - 8);

      movie = gss_isom_movie_new ();
      gss_isom_parse_moov (file, movie, &br);

      file->movie = movie;
    } else if (atom == GST_MAKE_FOURCC ('u', 'u', 'i', 'd')) {
      guint8 uuid[16];

      file_read (file, uuid, file->offset + 8, 16);

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
          GST_FOURCC_ARGS (atom), file->offset, size);
    }

    file->offset += size;
  }

  if (file->error) {
    GST_ERROR ("file error");
    close (file->fd);
    file->fd = -1;
    return FALSE;
  }

  gss_isom_file_fixup (file);

  close (file->fd);
  file->fd = -1;
  return TRUE;
}

static void
gss_isom_file_fixup (GssIsomFile * file)
{
  guint64 ts;
  int track_id;
  int i;

  /* FIXME this should use the real track id's in the file */
  for (track_id = 1; track_id <= 2; track_id++) {
    ts = 0;
    for (i = 0; i < file->n_fragments; i++) {
      if (file->fragments[i]->traf.tfhd.track_id == track_id) {
        file->fragments[i]->timestamp = ts;
        ts += file->fragments[i]->duration;
      }
    }
  }
}

GssIsomTrack *
gss_isom_track_new (void)
{
  return g_malloc0 (sizeof (GssIsomTrack));
}

void
gss_isom_track_free (GssIsomTrack * track)
{
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
  g_free (movie->tracks);
  g_free (movie);
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
  g_free (fragment);
}


static gboolean
file_read (GssIsomFile * file, guint8 * buffer, guint64 offset, guint64 n_bytes)
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
gss_isom_parse_ftyp (GssIsomFile * file, guint64 offset, guint64 size)
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
    } else if (atom == GST_MAKE_FOURCC ('i', 's', 'o', 'm')) {
      file->ftyp |= GSS_ISOM_FTYP_ISOM;
    } else if (atom == GST_MAKE_FOURCC ('q', 't', ' ', ' ')) {
      file->ftyp |= GSS_ISOM_FTYP_QT__;
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
gss_isom_parse_moof (GssIsomFile * file, GssIsomFragment * fragment,
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
      gss_isom_parse_mfhd (file, &fragment->mfhd, &sbr);
    } else if (atom == GST_MAKE_FOURCC ('t', 'r', 'a', 'f')) {
      gss_isom_parse_traf (file, &fragment->traf, &sbr);
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
          GST_FOURCC_ARGS (atom), file->offset, size);
    }

    gst_byte_reader_skip (br, size - 8);
  }
}

static void
gss_isom_parse_traf (GssIsomFile * file, AtomTraf * traf, GstByteReader * br)
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
      gss_isom_parse_tfhd (file, &traf->tfhd, &sbr);
    } else if (atom == GST_MAKE_FOURCC ('t', 'r', 'u', 'n')) {
      gss_isom_parse_trun (file, &traf->trun, &sbr);
    } else if (atom == GST_MAKE_FOURCC ('s', 'd', 't', 'p')) {
      gss_isom_parse_sdtp (file, &traf->sdtp, &sbr, traf->trun.sample_count);
    } else if (atom == GST_MAKE_FOURCC ('u', 'u', 'i', 'd')) {
      const guint8 *uuid = NULL;

      gst_byte_reader_get_data (&sbr, 16, &uuid);

      if (memcmp (uuid, uuid_sample_encryption, 16) == 0) {
        gss_isom_parse_sample_encryption (file, &traf->sample_encryption, &sbr);
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
          GST_FOURCC_ARGS (atom), file->offset, size);
    }

    gst_byte_reader_skip (br, size - 8);
  }
}

static void
gss_isom_parse_mfhd (GssIsomFile * file, AtomMfhd * mfhd, GstByteReader * br)
{

  gst_byte_reader_get_uint8 (br, &mfhd->version);
  gst_byte_reader_get_uint24_be (br, &mfhd->flags);
  gst_byte_reader_get_uint32_be (br, &mfhd->sequence_number);
}

static void
gss_isom_parse_tfhd (GssIsomFile * file, AtomTfhd * tfhd, GstByteReader * br)
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
gss_isom_parse_trun (GssIsomFile * file, AtomTrun * trun, GstByteReader * br)
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
gss_isom_fixup_moof (GssIsomFragment * fragment)
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
gss_isom_moof_get_duration (GssIsomFragment * fragment)
{
  guint64 duration = 0;
  int i;

  for (i = 0; i < fragment->traf.trun.sample_count; i++) {
    duration += fragment->traf.trun.samples[i].duration;
  }
  return duration;
}

static void
gss_isom_parse_sdtp (GssIsomFile * file, AtomSdtp * sdtp, GstByteReader * br,
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
gss_isom_parse_sample_encryption (GssIsomFile * file,
    AtomUUIDSampleEncryption * se, GstByteReader * br)
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
gss_isom_parse_mfra (GssIsomFile * file, guint64 offset, guint64 size)
{
  /* ignored for now */
  GST_DEBUG ("FIXME parse mfra atom");
}

void
gss_isom_fragment_set_sample_encryption (GssIsomFragment * fragment,
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
gss_isom_parse_mvhd (GssIsomFile * file, AtomMvhd * mvhd, GstByteReader * br)
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
gss_isom_parse_tkhd (GssIsomFile * file, GssIsomTrack * track,
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
gss_isom_parse_tref (GssIsomFile * file, GssIsomTrack * track,
    GstByteReader * br)
{
  AtomTref *tref = &track->tref;
  tref->present = TRUE;

  GST_FIXME ("parse tref");

  //CHECK_END (br);
}

static void
gss_isom_parse_elst (GssIsomFile * file, GssIsomTrack * track,
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
gss_isom_parse_mdhd (GssIsomFile * file, GssIsomTrack * track,
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
get_string (GssIsomFile * file, GstByteReader * br, gchar ** string)
{
  gboolean ret;
  guint8 len = 0;
  gboolean nul_terminated;

  if (file->ftyp & (GSS_ISOM_FTYP_MP41 | GSS_ISOM_FTYP_MP42 |
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
gss_isom_parse_hdlr (GssIsomFile * file, GssIsomTrack * track,
    GstByteReader * br)
{
  AtomHdlr *hdlr = &track->hdlr;
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
gss_isom_parse_vmhd (GssIsomFile * file, GssIsomTrack * track,
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
gss_isom_parse_smhd (GssIsomFile * file, GssIsomTrack * track,
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
gss_isom_parse_hmhd (GssIsomFile * file, GssIsomTrack * track,
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
gss_isom_parse_dref (GssIsomFile * file, GssIsomTrack * track,
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
gss_isom_parse_stts (GssIsomFile * file, GssIsomTrack * track,
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
gss_isom_parse_ctts (GssIsomFile * file, GssIsomTrack * track,
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
gss_isom_parse_stsz (GssIsomFile * file, GssIsomTrack * track,
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
gss_isom_parse_stsc (GssIsomFile * file, GssIsomTrack * track,
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
gss_isom_parse_stco (GssIsomFile * file, GssIsomTrack * track,
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
gss_isom_parse_co64 (GssIsomFile * file, GssIsomTrack * track,
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
gss_isom_parse_stss (GssIsomFile * file, GssIsomTrack * track,
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
gss_isom_parse_stsh (GssIsomFile * file, GssIsomTrack * track,
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
gss_isom_parse_stdp (GssIsomFile * file, GssIsomTrack * track,
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
gss_isom_parse_esds (GssIsomFile * file, GssIsomTrack * track,
    GstByteReader * br)
{
  AtomEsds *esds = &track->esds;
  guint32 tmp = 0;

  //gst_byte_reader_dump (br);
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
gss_isom_parse_avcC (GssIsomFile * file, GssIsomTrack * track,
    GstByteReader * br)
{
  AtomEsds *esds = &track->esds;

  esds->codec_data_len = gst_byte_reader_get_remaining (br);
  gst_byte_reader_dup_data (br, esds->codec_data_len, &esds->codec_data);
}

static ContainerAtoms stsd_atoms[] = {
  {GST_MAKE_FOURCC ('a', 'v', 'c', 'C'), gss_isom_parse_avcC},
  {GST_MAKE_FOURCC ('e', 's', 'd', 's'), gss_isom_parse_esds},
  {GST_MAKE_FOURCC ('s', 'i', 'n', 'f'), gss_isom_parse_ignore},
  {GST_MAKE_FOURCC ('b', 't', 'r', 't'), gss_isom_parse_ignore},
  {0}
};

static void
gss_isom_parse_stsd (GssIsomFile * file, GssIsomTrack * track,
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

    GST_DEBUG ("stsd atom %" GST_FOURCC_FORMAT, GST_FOURCC_ARGS (atom));
    gst_byte_reader_init_sub (&sbr, br, size - 8);
    if (atom == GST_MAKE_FOURCC ('m', 'p', '4', 'a') ||
        atom == GST_MAKE_FOURCC ('e', 'n', 'c', 'a')) {
      AtomMp4a *mp4a = &track->mp4a;

      GST_DEBUG ("mp4a");
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
      AtomMp4v *mp4v = &track->mp4v;
      //AtomEsds *esds = &track->esds;

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
#if 0
      /* avcC */
      gst_byte_reader_skip (&sbr, 8);
      esds->codec_data_len = gst_byte_reader_get_remaining (&sbr);
      gst_byte_reader_dup_data (&sbr, esds->codec_data_len, &esds->codec_data);
#endif

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
gss_isom_parse_ignore (GssIsomFile * file, GssIsomTrack * track,
    GstByteReader * br)
{
  GST_FIXME ("ingoring atom");
}

static ContainerAtoms dinf_atoms[] = {
  {GST_MAKE_FOURCC ('d', 'r', 'e', 'f'), gss_isom_parse_dref},
  {0}
};

static ContainerAtoms stbl_atoms[] = {
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

static ContainerAtoms minf_atoms[] = {
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

static ContainerAtoms mdia_atoms[] = {
  {GST_MAKE_FOURCC ('m', 'd', 'h', 'd'), gss_isom_parse_mdhd},
  {GST_MAKE_FOURCC ('h', 'd', 'l', 'r'), gss_isom_parse_hdlr},
  {GST_MAKE_FOURCC ('m', 'i', 'n', 'f'), NULL, minf_atoms},
  {GST_MAKE_FOURCC ('i', 'm', 'a', 'p'), gss_isom_parse_ignore},
  {GST_MAKE_FOURCC ('u', 'd', 't', 'a'), gss_isom_parse_ignore},
  {0}
};

static ContainerAtoms edts_atoms[] = {
  {GST_MAKE_FOURCC ('e', 'l', 's', 't'), gss_isom_parse_elst},
  {0}
};

#if 0
static ContainerAtoms udta_atoms[] = {
  {0}
};
#endif

static ContainerAtoms trak_atoms[] = {
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
gss_isom_parse_container (GssIsomFile * file, GssIsomTrack * track,
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
          atoms[i].parse (file, track, &sbr);
        } else {
          gss_isom_parse_container (file, track, &sbr, atoms[i].atoms, atom);
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
gss_isom_parse_udta (GssIsomFile * file, AtomUdta * udta, GstByteReader * br)
{
}

static void
gss_isom_parse_mvex (GssIsomFile * file, AtomMvex * mvex, GstByteReader * br)
{
}

static void
gss_isom_parse_meta (GssIsomFile * file, AtomMeta * meta, GstByteReader * br)
{
}

static void
gss_isom_parse_skip (GssIsomFile * file, AtomSkip * skip, GstByteReader * br)
{
}

static void
gss_isom_parse_iods (GssIsomFile * file, AtomIods * iods, GstByteReader * br)
{
}

static void
gss_isom_parse_moov (GssIsomFile * file, GssIsomMovie * movie,
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
      gss_isom_parse_udta (file, &movie->udta, &sbr);
    } else if (atom == GST_MAKE_FOURCC ('m', 'v', 'e', 'x')) {
      gss_isom_parse_mvex (file, &movie->mvex, &sbr);
    } else if (atom == GST_MAKE_FOURCC ('m', 'e', 't', 'a')) {
      gss_isom_parse_meta (file, &movie->meta, &sbr);
    } else if (atom == GST_MAKE_FOURCC ('s', 'k', 'i', 'p')) {
      gss_isom_parse_skip (file, &movie->skip, &sbr);
    } else if (atom == GST_MAKE_FOURCC ('i', 'o', 'd', 's')) {
      gss_isom_parse_iods (file, &movie->iods, &sbr);
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
          GST_FOURCC_ARGS (atom), file->offset + br->byte, size);
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
gss_isom_tfhd_serialize (AtomTfhd * tfhd, GstByteWriter * bw)
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
gss_isom_trun_serialize (AtomTrun * trun, GstByteWriter * bw)
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
gss_isom_sdtp_serialize (AtomSdtp * sdtp, GstByteWriter * bw, int sample_count)
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
gss_isom_sample_encryption_serialize (AtomUUIDSampleEncryption * se,
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
gss_isom_traf_serialize (AtomTraf * traf, GstByteWriter * bw)
{
  int offset;
  offset = ATOM_INIT (bw, GST_MAKE_FOURCC ('t', 'r', 'a', 'f'));

  gss_isom_tfhd_serialize (&traf->tfhd, bw);
  gss_isom_trun_serialize (&traf->trun, bw);
  gss_isom_sdtp_serialize (&traf->sdtp, bw, traf->trun.sample_count);
  gss_isom_sample_encryption_serialize (&traf->sample_encryption, bw);

  ATOM_FINISH (bw, offset);
}

static void
gss_isom_mfhd_serialize (AtomMfhd * mfhd, GstByteWriter * bw)
{
  int offset;
  offset = ATOM_INIT (bw, GST_MAKE_FOURCC ('m', 'f', 'h', 'd'));

  gst_byte_writer_put_uint8 (bw, mfhd->version);
  gst_byte_writer_put_uint24_be (bw, mfhd->flags);
  gst_byte_writer_put_uint32_be (bw, mfhd->sequence_number);


  ATOM_FINISH (bw, offset);
}

static void
gss_isom_moof_serialize (GssIsomFragment * fragment, GstByteWriter * bw)
{
  int offset;
  offset = ATOM_INIT (bw, GST_MAKE_FOURCC ('m', 'o', 'o', 'f'));

  gss_isom_mfhd_serialize (&fragment->mfhd, bw);
  gss_isom_traf_serialize (&fragment->traf, bw);
  //gss_isom_xmp_data_serialize (&moof->xmp_data, bw);

  ATOM_FINISH (bw, offset);

  GST_WRITE_UINT32_BE (
      (void *) (bw->parent.data + fragment->traf.trun.data_offset_fixup),
      bw->parent.byte + 8);
}

void
gss_isom_fragment_serialize (GssIsomFragment * fragment, guint8 ** data,
    int *size)
{
  GstByteWriter *bw;

  bw = gst_byte_writer_new ();

  gss_isom_moof_serialize (fragment, bw);

  *size = bw->parent.byte;
  *data = gst_byte_writer_free_and_get_data (bw);
}

int
gss_isom_fragment_get_n_samples (GssIsomFragment * fragment)
{
  return fragment->traf.trun.sample_count;
}

int *
gss_isom_fragment_get_sample_sizes (GssIsomFragment * fragment)
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
gss_isom_file_fragmentize (GssIsomFile * file)
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

  n_fragments = video_track->stss.entry_count;

  file->fragments = g_malloc0 (sizeof (GssIsomFragment *) * n_fragments * 2);
  file->n_fragments = n_fragments * 2;
  file->n_fragments_alloc = n_fragments * 2;

  for (i = 0; i < n_fragments; i++) {
    GssIsomFragment *audio_fragment;
    GssIsomFragment *video_fragment;
    guint64 n_samples;
    AtomTrunSample *samples;
    int sample_offset;
    int j;

    audio_fragment = gss_isom_fragment_new ();
    video_fragment = gss_isom_fragment_new ();
    file->fragments[i * 2 + 0] = audio_fragment;
    file->fragments[i * 2 + 1] = video_fragment;

    video_fragment->mfhd.sequence_number = 2 * i + 2;

    video_fragment->traf.tfhd.track_id = video_track->tkhd.track_id;
    video_fragment->traf.tfhd.flags =
        TF_DEFAULT_SAMPLE_DURATION | TF_DEFAULT_SAMPLE_FLAGS;
    video_fragment->traf.tfhd.default_sample_duration = 0x061a80;
    video_fragment->traf.tfhd.default_sample_size = 0;
    video_fragment->traf.tfhd.default_sample_flags = 0x000100c0;

    sample_offset = video_track->stss.sample_numbers[i] - 1;
    if (i == n_fragments - 1) {
      n_samples = gss_isom_track_get_n_samples (video_track) - sample_offset;
    } else {
      n_samples = (video_track->stss.sample_numbers[i + 1] - 1) - sample_offset;
    }
    video_fragment->traf.trun.sample_count = n_samples;
    video_fragment->traf.trun.data_offset = 12;

    video_fragment->mdat_size = 8;
    video_fragment->n_mdat_chunks = n_samples;
    video_fragment->chunks = g_malloc (sizeof (GssMdatChunk) * n_samples);

    video_fragment->traf.sdtp.sample_flags =
        g_malloc0 (sizeof (guint8) * n_samples);
    video_fragment->traf.sdtp.sample_flags[0] = 0x14;
    for (j = 1; j < n_samples; j++) {
      video_fragment->traf.sdtp.sample_flags[j] = 0x1c;
    }

    samples = g_malloc0 (sizeof (AtomTrunSample) * n_samples);
    video_fragment->timestamp = video_timestamp;
    for (j = 0; j < n_samples; j++) {
      GssIsomSample sample;
      guint64 next_timestamp;

      gss_isom_track_get_sample (video_track, &sample, sample_offset + j);

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
    }
    video_fragment->traf.trun.samples = samples;
    /* FIXME not all strictly necessary, should be handled in serializer */
    video_fragment->traf.trun.flags =
        TR_SAMPLE_SIZE | TR_DATA_OFFSET | TR_FIRST_SAMPLE_FLAGS |
        TR_SAMPLE_COMPOSITION_TIME_OFFSETS;
    video_fragment->traf.trun.first_sample_flags = 0x40;
    video_fragment->duration = video_timestamp - video_fragment->timestamp;

    audio_fragment->mfhd.sequence_number = 2 * i + 1;

    audio_index_end = gss_isom_track_get_index_from_timestamp (audio_track,
        gst_util_uint64_scale_int (video_timestamp,
            audio_track->mdhd.timescale, 10000000));

    audio_fragment->traf.tfhd.track_id = audio_track->tkhd.track_id;
    audio_fragment->traf.tfhd.flags = TF_DEFAULT_SAMPLE_FLAGS;
    audio_fragment->traf.tfhd.default_sample_duration = 0;
    audio_fragment->traf.tfhd.default_sample_size = 0;
    audio_fragment->traf.tfhd.default_sample_flags = 0xc0;

    n_samples = audio_index_end - audio_index;

    audio_fragment->mdat_size = 8;
    audio_fragment->n_mdat_chunks = n_samples;
    audio_fragment->chunks = g_malloc (sizeof (GssMdatChunk) * n_samples);

    audio_fragment->traf.trun.sample_count = n_samples;
    audio_fragment->traf.trun.data_offset = 12;
    samples = g_malloc0 (sizeof (AtomTrunSample) * n_samples);

    audio_fragment->traf.sdtp.sample_flags =
        g_malloc0 (sizeof (guint32) * n_samples);

    audio_fragment->timestamp = audio_timestamp;
    for (j = 0; j < n_samples; j++) {
      GssIsomSample sample;
      guint64 next_timestamp;

      gss_isom_track_get_sample (audio_track, &sample, audio_index + j * 1);

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
    }
    audio_fragment->traf.trun.samples = samples;
    /* FIXME not all strictly necessary, should be handled in serializer */
    audio_fragment->traf.trun.flags =
        TR_SAMPLE_DURATION | TR_SAMPLE_SIZE | TR_DATA_OFFSET;
    audio_fragment->duration = audio_timestamp - audio_fragment->timestamp;

    audio_index += n_samples;
  }
}

int
gss_isom_track_get_index_from_timestamp (GssIsomTrack * track,
    guint64 timestamp)
{
  int i;
  int offset;
  guint64 ts;
  AtomSttsEntry *entries;

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
  for (i = 0; i < track->stsc.entry_count; i++) {
    int n_chunks;

#if 0
    g_print ("stsc %d: %d %d %d\n", i,
        track->stsc.entries[i].first_chunk,
        track->stsc.entries[i].samples_per_chunk,
        track->stsc.entries[i].sample_description_index);
#endif

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
    GST_ERROR ("fixme index_in_chunk != 0");
  }

  sample->offset = track->stco.chunk_offsets[chunk_index];
}

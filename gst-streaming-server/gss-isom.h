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


#ifndef _GSS_ISOM_H
#define _GSS_ISOM_H

#include "gss-isom-boxes.h"
#include "gss-server.h"

G_BEGIN_DECLS

typedef struct _GssIsomFragment GssIsomFragment;
typedef struct _GssIsomTrack GssIsomTrack;
typedef struct _GssIsomMovie GssIsomMovie;
typedef struct _GssIsomParser GssIsomParser;
typedef struct _GssIsomSample GssIsomSample;
typedef struct _GssIsomSampleIterator GssIsomSampleIterator;
typedef struct _GssMdatChunk GssMdatChunk;
typedef struct _GssChunk GssChunk;

typedef enum
{
  GSS_ISOM_FTYP_ISML = (1 << 0),
  GSS_ISOM_FTYP_MP42 = (1 << 1),
  GSS_ISOM_FTYP_MP41 = (1 << 2),
  GSS_ISOM_FTYP_PIFF = (1 << 3),
  GSS_ISOM_FTYP_ISO2 = (1 << 4),
  GSS_ISOM_FTYP_ISOM = (1 << 5),
  GSS_ISOM_FTYP_QT__ = (1 << 6),
  GSS_ISOM_FTYP_ISO6 = (1 << 7),
} GssIsomFtyp;

struct _GssMdatChunk {
  guint64 offset;
  guint64 size;
};

struct _GssChunk {
  guint64 offset;
  guint64 size;
  guint64 source_offset;
  guint8 *data;
};

struct _GssIsomFragment {
  int track_id;
  guint64 offset;
  guint8 *moof_data;
  guint64 moof_size;
  int mdat_size;
  guint8 *mdat_header;
  int mdat_header_size;
  int n_mdat_chunks;
  GssMdatChunk *chunks;
  guint64 timestamp;
  guint64 duration;
  int index;

  GssBoxMfhd mfhd;
  GssBoxTfhd tfhd;
  GssBoxTrun trun;
  GssBoxSdtp sdtp;
  GssBoxUUIDSampleEncryption sample_encryption;
  GssBoxAvcn avcn;
  GssBoxTfdt tfdt;
  GssBoxTrik trik;
  GssBoxSaiz saiz;
  GssBoxSaio saio;

};

struct _GssIsomMovie
{
  int n_tracks;
  GssIsomTrack **tracks;

  GssBoxMvhd mvhd;

  GssBoxUdta udta;
  /* udta */
  GssBoxStore meta;
  GssBoxStore hdlr;
  GssBoxStore ilst;
  GssBoxMdir mdir;
  GssBoxStore xtra;

  GssBoxSkip skip;
  GssBoxStore iods;
  GssBoxStore mvex;
  GssBoxStore ainf;

  GssBoxSidx sidx;
  GssBoxMehd mehd;

  GssBoxPssh pssh;
};

struct _GssIsomTrack
{
  GssBoxTkhd tkhd;
  GssBoxTref tref;

  /* inside edts */
  GssBoxElst elst;

  /* inside mdia */
  GssBoxMdhd mdhd;
  GssBoxHdlr hdlr;

  /* inside mdia/minf */
  GssBoxVmhd vmhd;
  GssBoxSmhd smhd;
  GssBoxHmhd hmhd;
  /* mpeg stream headers (?) */

  /* inside mdia/minf/dinf */
  GssBoxDref dref;

  /* inside mdia/minf/stbl */
  GssBoxStts stts;
  GssBoxCtts ctts;
  GssBoxStss stss;
  GssBoxStsd stsd;
  GssBoxStsz stsz;
  GssBoxStsc stsc;
  GssBoxStco stco;
  GssBoxStsh stsh;
  GssBoxStdp stdp;

  /* inside mdia/minf/stbl/stsd */
  GssBoxMp4v mp4v;
  GssBoxMp4a mp4a;
  GssBoxEsds esds;
  GssBoxStore esds_store;

  /* in mvex at top level */
  GssBoxTrex trex;

  //guint8 *header;
  //gsize header_size;

  //guint8 *index;
  //gsize index_size;

  gboolean is_encrypted;
  GssIsomFragment **fragments;
  int n_fragments;
  int n_fragments_alloc;

  guint8 *ccff_header_data;
  gsize ccff_header_size;

  guint8 *dash_header_data;
  gsize dash_header_size;
  gsize dash_header_and_sidx_size;

  gsize dash_size;

  char *filename;
};


struct _GssIsomParser
{
  gboolean error;
  int fd;
  char *filename;
  guint64 file_size;
  guint64 offset;
  GssIsomFtyp ftyp;
  guint32 ftyp_atom;
  gboolean is_isml;
  gboolean is_mp42;

  GssIsomFragment *current_fragment;

  void *moov;

  GssIsomMovie *movie;

  guint8 *data;
  guint64 data_offset;
  guint64 data_size;

  GssBoxStore pdin;
  GssBoxStore bloc;
};

struct _GssIsomSampleIterator
{
  GssIsomTrack *track;
  int sample_index;
  int stts_index;
  int index_in_stts;
  int ctts_index;
  int index_in_ctts;
  int stsc_index;
  int chunk_index;
  int index_in_chunk;
  int offset_in_chunk;

};

struct _GssIsomSample
{
  guint32 duration;
  guint32 size;
  //guint32 flags;
  guint32 composition_time_offset;
  guint64 offset;
};


GssIsomParser *gss_isom_parser_new (void);
void gss_isom_parser_free (GssIsomParser *file);
gboolean gss_isom_parser_parse_file (GssIsomParser *file,
    const char *filename);
guint64 gss_isom_movie_get_duration (GssIsomMovie *movie);
GssIsomFragment * gss_isom_track_get_fragment (GssIsomTrack * track, int index);
GssIsomFragment * gss_isom_track_get_fragment_by_timestamp (GssIsomTrack *track,
    guint64 timestamp);
gboolean gss_isom_track_is_video (GssIsomTrack *track);

void gss_isom_fragment_set_sample_encryption (GssIsomFragment *fragment,
    int n_samples, guint64 *init_vectors, gboolean is_video);
void gss_isom_fragment_serialize (GssIsomFragment *fragment, guint8 **data,
    gsize *size, gboolean is_video);
void gss_isom_movie_serialize_track_ccff (GssIsomMovie * movie, GssIsomTrack *track,
    guint8 ** data, gsize *size);
void gss_isom_movie_serialize_track_dash (GssIsomMovie * movie, GssIsomTrack *track,
    guint8 ** data, gsize *header_size, gsize *size);
void gss_isom_movie_serialize (GssIsomMovie * movie, guint8 ** data,
    int *size);
void gss_isom_track_serialize_dash (GssIsomTrack *track, guint8 ** data, int *size);
int * gss_isom_fragment_get_sample_sizes (GssIsomFragment *fragment);
void gss_isom_encrypt_samples (GssIsomFragment * fragment, guint8 * mdat_data,
    guint8 *content_key);
int gss_isom_fragment_get_n_samples (GssIsomFragment *fragment);

GssIsomMovie *gss_isom_movie_new (void);
void gss_isom_movie_free (GssIsomMovie * movie);
GssIsomTrack * gss_isom_movie_get_video_track (GssIsomMovie * movie);
GssIsomTrack * gss_isom_movie_get_audio_track (GssIsomMovie * movie);
GssIsomTrack * gss_isom_movie_get_track_by_id (GssIsomMovie * movie, int track_id);

GssIsomFragment *gss_isom_fragment_new (void);
void gss_isom_fragment_free (GssIsomFragment * fragment);

void gss_isom_parser_fragmentize (GssIsomParser *file);
#if 0
void gss_isom_track_prepare_streaming (GssIsomMovie *movie, GssIsomTrack *track,
    const GssBoxPssh *pssh);
#endif

guint64 gss_isom_track_get_n_samples (GssIsomTrack *track);

void gss_isom_track_get_sample (GssIsomTrack *track, GssIsomSample *sample,
    int sample_index);
int gss_isom_track_get_index_from_timestamp (GssIsomTrack *track, guint64
    timestamp);

void gss_isom_sample_iter_init (GssIsomSampleIterator *iter,
    GssIsomTrack *track);
gboolean gss_isom_sample_iter_iterate (GssIsomSampleIterator *iter);
void gss_isom_sample_iter_get_sample (GssIsomSampleIterator *iter,
    GssIsomSample *sample);
void gss_isom_track_convert_h264_codec_data (GssIsomTrack *track);

void gss_isom_parser_dump (GssIsomParser *file);
void gss_isom_movie_dump (GssIsomMovie *movie);
void gss_isom_track_dump (GssIsomTrack *track);


G_END_DECLS

#endif


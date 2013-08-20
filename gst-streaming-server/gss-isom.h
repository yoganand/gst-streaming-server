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

#include "gss-iso-atoms.h"
#include "gss-server.h"

G_BEGIN_DECLS

typedef struct _GssIsomFragment GssIsomFragment;
typedef struct _GssIsomTrack GssIsomTrack;
typedef struct _GssIsomMovie GssIsomMovie;
typedef struct _GssIsomFile GssIsomFile;
typedef struct _GssIsomFragment GssIsomFragment;
typedef struct _GssIsomSample GssIsomSample;
typedef struct _GssIsomSampleIterator GssIsomSampleIterator;
typedef struct _GssMdatChunk GssMdatChunk;

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

struct _GssIsomFragment {
  int track_id;
  guint64 moof_offset;
  guint64 moof_size;
  int mdat_size;
  int n_mdat_chunks;
  GssMdatChunk *chunks;
  guint64 timestamp;
  guint64 duration;

  AtomMfhd mfhd;
  AtomTraf traf;
};

struct _GssIsomMovie
{
  int n_tracks;
  GssIsomTrack **tracks;

  AtomMvhd mvhd;

  AtomUdta udta;
  /* udta */
  AtomStore meta;
  AtomStore hdlr;
  AtomStore ilst;
  AtomMdir mdir;
  AtomStore xtra;

  AtomSkip skip;
  AtomStore iods;
  AtomStore mvex;
  AtomStore ainf;

};

struct _GssIsomTrack
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

  /* inside mdia/minf/stbl/stsd */
  AtomMp4v mp4v;
  AtomMp4a mp4a;
  AtomEsds esds;
  AtomStore esds_store;

};


struct _GssIsomFile
{
  gboolean error;
  int fd;
  guint64 file_size;
  guint64 offset;
  GssIsomFtyp ftyp;
  guint32 ftyp_atom;
  gboolean is_isml;
  gboolean is_mp42;

  GssIsomFragment **fragments;
  int n_fragments;
  int n_fragments_alloc;

  GssIsomFragment *current_fragment;

  void *moov;

  GssIsomMovie *movie;

  guint8 *data;
  guint64 data_offset;
  guint64 data_size;

  AtomStore pdin;
  AtomStore bloc;
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


GssIsomFile *gss_isom_file_new (void);
void gss_isom_file_free (GssIsomFile *file);
gboolean gss_isom_file_parse_file (GssIsomFile *file,
    const char *filename);
GssIsomFragment * gss_isom_file_get_fragment (GssIsomFile *file,
    int track_id, int frag_index);
GssIsomFragment * gss_isom_file_get_fragment_by_timestamp (
    GssIsomFile *file, int track_id, guint64 timestamp);
int gss_isom_file_get_n_fragments (GssIsomFile *file, int track_id);
guint64 gss_isom_file_get_duration (GssIsomFile *file, int track_id);
void gss_isom_file_free (GssIsomFile *file);

void gss_isom_fragment_set_sample_encryption (GssIsomFragment *fragment,
    int n_samples, guint64 *init_vectors, gboolean is_h264);
void gss_isom_fragment_serialize (GssIsomFragment *fragment, guint8 **data,
    int *size, gboolean is_video);
void gss_isom_movie_serialize_track (GssIsomMovie * movie, int track, guint8 ** data,
    int *size);
int * gss_isom_fragment_get_sample_sizes (GssIsomFragment *fragment);
void gss_isom_encrypt_samples (GssIsomFragment * fragment, guint8 * mdat_data,
    guint8 *content_key);
int gss_isom_fragment_get_n_samples (GssIsomFragment *fragment);

GssIsomMovie *gss_isom_movie_new (void);
void gss_isom_movie_free (GssIsomMovie * movie);
GssIsomTrack * gss_isom_movie_get_video_track (GssIsomMovie * movie);
GssIsomTrack * gss_isom_movie_get_audio_track (GssIsomMovie * movie);

GssIsomFragment *gss_isom_fragment_new (void);
void gss_isom_fragment_free (GssIsomFragment * fragment);

void gss_isom_file_fragmentize (GssIsomFile *file);

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






G_END_DECLS

#endif


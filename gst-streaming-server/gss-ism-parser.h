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


#ifndef _GSS_ISM_PARSER_H
#define _GSS_ISM_PARSER_H

#include "gss-iso-atoms.h"
#include "gss-server.h"

G_BEGIN_DECLS

typedef struct _GssISMFragment GssISMFragment;
typedef struct _GssISMTrack GssISMTrack;
typedef struct _GssISMMovie GssISMMovie;
typedef struct _GssISMParser GssISMParser;
typedef struct _GssISMFragment GssISMFragment;
typedef struct _GssISMSample GssISMSample;
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
} GssIsomFtyp;

struct _GssMdatChunk {
  guint64 offset;
  guint64 size;
};

struct _GssISMFragment {
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

struct _GssISMMovie
{
  int n_tracks;
  GssISMTrack **tracks;

  AtomMvhd mvhd;
  AtomUdta udta;
  AtomMvex mvex;
  AtomMeta meta;
  AtomSkip skip;
  AtomIods iods;
};

struct _GssISMTrack
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

  GssISMFragment **fragments;
  int n_fragments;
  int n_fragments_alloc;

  GssISMFragment *current_fragment;

  void *moov;

  GssISMMovie *movie;

  guint8 *data;
  guint64 data_offset;
  guint64 data_size;
};

struct _GssISMSample
{
  guint32 duration;
  guint32 size;
  //guint32 flags;
  guint32 composition_time_offset;
  guint64 offset;
};


GssISMParser *gss_ism_parser_new (void);
void gss_ism_parser_free (GssISMParser *parser);
gboolean gss_ism_parser_parse_file (GssISMParser *parser,
    const char *filename);
GssISMFragment * gss_ism_parser_get_fragment (GssISMParser *parser,
    int track_id, int frag_index);
GssISMFragment * gss_ism_parser_get_fragment_by_timestamp (
    GssISMParser *parser, int track_id, guint64 timestamp);
int gss_ism_parser_get_n_fragments (GssISMParser *parser, int track_id);
guint64 gss_ism_parser_get_duration (GssISMParser *parser, int track_id);
void gss_ism_parser_free (GssISMParser *parser);

void gss_ism_fragment_set_sample_encryption (GssISMFragment *fragment,
    int n_samples, guint64 *init_vectors, gboolean is_h264);
void gss_ism_fragment_serialize (GssISMFragment *fragment, guint8 **data,
    int *size);
int * gss_ism_fragment_get_sample_sizes (GssISMFragment *fragment);
void gss_ism_encrypt_samples (GssISMFragment * fragment, guint8 * mdat_data,
    guint8 *content_key);
int gss_ism_fragment_get_n_samples (GssISMFragment *fragment);

GssISMMovie *gss_ism_movie_new (void);
void gss_ism_movie_free (GssISMMovie * movie);

GssISMFragment *gss_ism_fragment_new (void);
void gss_ism_fragment_free (GssISMFragment * fragment);

void gss_ism_parser_fragmentize (GssISMParser *parser);

guint64 gss_ism_track_get_n_samples (GssISMTrack *track);

void gss_ism_track_get_sample (GssISMTrack *track, GssISMSample *sample,
    int sample_index);
int gss_ism_track_get_index_from_timestamp (GssISMTrack *track, guint64
    timestamp);



G_END_DECLS

#endif


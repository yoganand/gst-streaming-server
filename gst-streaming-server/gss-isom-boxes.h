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

#ifndef _GSS_ISO_ATOMS_H
#define _GSS_ISO_ATOMS_H

#include <glib.h>

/* GssBox definitions */

typedef struct _GssBoxMfhd GssBoxMfhd;
typedef struct _GssBoxTfhd GssBoxTfhd;
typedef struct _GssBoxTrun GssBoxTrun;
typedef struct _GssBoxTrunSample GssBoxTrunSample;
typedef struct _GssBoxSdtp GssBoxSdtp;
typedef struct _GssBoxUUIDSampleEncryption GssBoxUUIDSampleEncryption;
typedef struct _GssBoxUUIDSampleEncryptionSample GssBoxUUIDSampleEncryptionSample;
typedef struct _GssBoxUUIDSampleEncryptionSampleEntry GssBoxUUIDSampleEncryptionSampleEntry;
typedef struct _GssBoxAvcn GssBoxAvcn;
typedef struct _GssBoxTfdt GssBoxTfdt;
typedef struct _GssBoxTrik GssBoxTrik;
//typedef struct _GssBoxTraf GssBoxTraf;
//typedef struct _GssBoxMoof GssBoxMoof;

typedef struct _GssBoxMvhd GssBoxMvhd;
typedef struct _GssBoxIods GssBoxIods;
typedef struct _GssBoxAinf GssBoxAinf;
typedef struct _GssBoxTrak GssBoxTrak;
typedef struct _GssBoxTkhd GssBoxTkhd;
typedef struct _GssBoxTref GssBoxTref;
typedef struct _GssBoxMdia GssBoxMdia;
typedef struct _GssBoxMdhd GssBoxMdhd;
typedef struct _GssBoxHdlr GssBoxHdlr;
typedef struct _GssBoxMinf GssBoxMinf;
typedef struct _GssBoxVmhd GssBoxVmhd;
typedef struct _GssBoxSmhd GssBoxSmhd;
typedef struct _GssBoxHmhd GssBoxHmhd;
typedef struct _GssBoxDinf GssBoxDinf;
typedef struct _GssBoxUrl_ GssBoxUrl_;
typedef struct _GssBoxUrn_ GssBoxUrn_;
typedef struct _GssBoxDrefEntry GssBoxDrefEntry;
typedef struct _GssBoxDref GssBoxDref;
typedef struct _GssBoxStbl GssBoxStbl;
typedef struct _GssBoxSttsEntry GssBoxSttsEntry;
typedef struct _GssBoxStts GssBoxStts;
typedef struct _GssBoxCttsEntry GssBoxCttsEntry;
typedef struct _GssBoxCtts GssBoxCtts;
typedef struct _GssBoxEsds GssBoxEsds;
typedef struct _GssBoxMp4v GssBoxMp4v;
typedef struct _GssBoxMp4a GssBoxMp4a;
typedef struct _GssBoxMp4s GssBoxMp4s;
typedef struct _GssBoxStsd GssBoxStsd;
typedef struct _GssBoxStsdEntry GssBoxStsdEntry;
typedef struct _GssBoxStsz GssBoxStsz;
typedef struct _GssBoxStscEntry GssBoxStscEntry;
typedef struct _GssBoxStsc GssBoxStsc;
typedef struct _GssBoxStco GssBoxStco;
typedef struct _GssBoxStss GssBoxStss;
typedef struct _GssBoxStshEntry GssBoxStshEntry;
typedef struct _GssBoxStsh GssBoxStsh;
typedef struct _GssBoxStdp GssBoxStdp;
typedef struct _GssBoxEdts GssBoxEdts;
typedef struct _GssBoxElstEntry GssBoxElstEntry;
typedef struct _GssBoxElst GssBoxElst;
typedef struct _GssBoxUdta GssBoxUdta;
typedef struct _GssBoxCprt GssBoxCprt;
typedef struct _GssBoxUUIDProtectionHeader GssBoxUUIDProtectionHeader;
typedef struct _GssBoxMoov GssBoxMoov;
typedef struct _GssBoxParser GssBoxParser;
typedef struct _GssBoxMvex GssBoxMvex;
typedef struct _GssBoxMeta GssBoxMeta;
typedef struct _GssBoxIlst GssBoxIlst;
typedef struct _GssBoxMdir GssBoxMdir;
typedef struct _GssBoxSkip GssBoxSkip;
typedef struct _GssBoxMehd GssBoxMehd;
typedef struct _GssBoxTrex GssBoxTrex;
typedef struct _GssBoxSidxEntry GssBoxSidxEntry;
typedef struct _GssBoxSidx GssBoxSidx;
typedef struct _GssBoxPssh GssBoxPssh;
typedef struct _GssBoxSinf GssBoxSinf;
typedef struct _GssBoxSaiz GssBoxSaiz;
typedef struct _GssBoxSaio GssBoxSaio;
typedef struct _GssBoxStore GssBoxStore;


struct _GssBoxMfhd
{
  guint8 version;
  guint32 flags;

  guint32 sequence_number;
};

struct _GssBoxTfhd
{
  guint8 version;
  guint32 flags;

  guint32 track_id;
  guint32 default_sample_duration;
  guint32 default_sample_size;
  guint32 default_sample_flags;
};

struct _GssBoxTrun
{
  guint8 version;
  guint32 flags;

  guint32 sample_count;
  guint32 data_offset;
  guint32 first_sample_flags;

  GssBoxTrunSample *samples;

  /* This is a special field used while writing: it stores the offset
   * to the data_offset location, so it can be fixed up once we know
   * the size of the entire moof atom */
  int data_offset_fixup;
};

struct _GssBoxTrunSample
{
  guint32 duration;
  guint32 size;
  guint32 flags;
  guint32 composition_time_offset;
};

struct _GssBoxSdtp
{
  gboolean present;

  guint8 version;
  guint32 flags;

  guint8 *sample_flags;

};

struct _GssBoxUUIDSampleEncryption
{
  gboolean present;
  guint8 version;
  guint32 flags;
  guint32 algorithm_id;
  guint8 iv_size;
  guint8 kid[16];
  guint32 sample_count;
  GssBoxUUIDSampleEncryptionSample *samples;
};

#define MAX_IV_SIZE 16
struct _GssBoxUUIDSampleEncryptionSample
{
  guint64 iv;
  guint16 num_entries;
  GssBoxUUIDSampleEncryptionSampleEntry *entries;
};

struct _GssBoxUUIDSampleEncryptionSampleEntry
{
  guint16 bytes_of_clear_data;
  guint32 bytes_of_encrypted_data;
};

struct _GssBoxAvcn
{
  guint8 version;
  guint32 flags;
};

struct _GssBoxTfdt
{
  gboolean present;
  guint8 version;
  guint32 flags;

  guint64 start_time;
};

struct _GssBoxTrik
{
  guint8 version;
  guint32 flags;
};

#if 0
struct _GssBoxTraf
{
  guint8 version;
  guint32 flags;

  GssBoxTfhd tfhd;
  GssBoxTrun trun;
  GssBoxSdtp sdtp;
  GssBoxUUIDSampleEncryption sample_encryption;
  GssBoxAvcn avcn;
  GssBoxTfdt tfdt;
  GssBoxTrik trik;
};
#endif

#if 0
struct _GssBoxMoof
{

};
#endif

/* From ISO/IEC 14496-1:2002 */

struct _GssBoxMvhd {
  guint8 version;
  guint32 flags;
  guint64 creation_time;
  guint64 modification_time;
  guint32 timescale;
  guint64 duration;
  guint32 next_track_id;
};

struct _GssBoxIods
{
  guint8 version;
  guint32 flags;
  /* object descriptor */
};

struct _GssBoxAinf
{
  gboolean present;
  guint8 version;
  guint32 flags;

  /* FIXME */
};

struct _GssBoxTkhd
{
  gboolean present;
  guint8 version;
  guint32 flags;
  guint64 creation_time;
  guint64 modification_time;
  guint32 track_id;
  guint64 duration;
  guint16 layer;
  guint16 alternate_group;
  guint16 volume;
  guint32 matrix[9];
  guint32 width;
  guint32 height;
};

struct _GssBoxTref
{
  gboolean present;
  /* contains other stuff */

  guint32 *track_ids;
};

struct _GssBoxMdia
{
  gboolean present;
  /* container */
};

struct _GssBoxMdhd
{
  gboolean present;
  guint8 version;
  guint32 flags;
  guint64 creation_time;
  guint64 modification_time;
  guint32 timescale;
  guint64 duration;
  char language_code[4];
};

struct _GssBoxHdlr
{
  gboolean present;
  guint8 version;
  guint32 flags;
  guint32 handler_type;
  char *name;
};

struct _GssBoxMinf
{
  gboolean present;
  /* container */
};

struct _GssBoxVmhd
{
  gboolean present;
  guint8 version;
  guint32 flags;
};

struct _GssBoxSmhd
{
  gboolean present;
  guint8 version;
  guint32 flags;
};

struct _GssBoxHmhd
{
  gboolean present;
  guint8 version;
  guint32 flags;
  guint16 maxPDUsize;
  guint16 avgPDUsize;
  guint32 maxbitrate;
  guint32 avgbitrate;
};

struct _GssBoxDinf
{
  gboolean present;
  /* container */
};

struct _GssBoxUrl_
{
  gboolean present;
  guint8 version;
  guint32 flags;
  char *location;
};

struct _GssBoxUrn_
{
  gboolean present;
  guint8 version;
  guint32 flags;
  char *name;
  char *location;
};

struct _GssBoxDrefEntry {
  guint32 atom;
  guint8 entry_version;
  guint32 entry_flags;
  struct _GssBoxUrn_ urn_;
  struct _GssBoxUrl_ url_;
};

struct _GssBoxDref
{
  gboolean present;
  guint8 version;
  guint32 flags;
  guint32 entry_count;
  struct _GssBoxDrefEntry *entries;
};

struct _GssBoxStbl
{
  /* container */
};

struct _GssBoxSttsEntry
{
  guint32 sample_count;
  gint32 sample_delta;
};

struct _GssBoxStts
{
  gboolean present;
  guint8 version;
  guint32 flags;
  guint32 entry_count;
  struct _GssBoxSttsEntry *entries;
};

struct _GssBoxCttsEntry
{
  guint32 sample_count;
  guint32 sample_offset;
};

struct _GssBoxCtts
{
  gboolean present;
  guint8 version;
  guint32 flags;
  guint32 entry_count;
  struct _GssBoxCttsEntry *entries;
};

struct _GssBoxEsds
{
  gboolean present;
  /* ES descriptor */
  guint8 version;
  guint32 flags;

  guint16 es_id;
  guint8 es_flags;
  guint8 type_indication;
  guint8 stream_type;
  guint32 buffer_size_db;
  guint32 max_bitrate;
  guint32 avg_bitrate;
  int codec_data_len;
  guint8 *codec_data;
};

struct _GssBoxMp4v
{
  gboolean present;
  guint16 data_reference_index;
  guint16 width;
  guint16 height;
  struct _GssBoxEsds es;

};

struct _GssBoxMp4a
{
  gboolean present;
  guint16 data_reference_index;
  guint16 channel_count;
  guint16 sample_size;
  guint32 sample_rate;
  struct _GssBoxEsds es;
};

struct _GssBoxMp4s
{
  gboolean present;
  guint16 data_reference_index;
  struct _GssBoxEsds es;
};

struct _GssBoxStsdEntry
{
  guint32 atom;

};

struct _GssBoxStsd
{
  gboolean present;
  guint8 version;
  guint32 flags;
  guint32 entry_count;
  GssBoxStsdEntry *entries;

};

struct _GssBoxStsz
{
  gboolean present;
  guint8 version;
  guint32 flags;
  guint32 sample_size;
  guint32 sample_count;
  guint32 *sample_sizes;
};

struct _GssBoxStscEntry
{
  gboolean present;
  guint32 first_chunk;
  guint32 samples_per_chunk;
  guint32 sample_description_index;
};

struct _GssBoxStsc
{
  gboolean present;
  guint8 version;
  guint32 flags;
  guint32 entry_count;
  struct _GssBoxStscEntry *entries;
};

/* This is for both stco and co64 */
struct _GssBoxStco
{
  gboolean present;
  guint8 version;
  guint32 flags;
  guint32 entry_count;
  guint64 *chunk_offsets;
};

struct _GssBoxStss
{
  gboolean present;
  guint8 version;
  guint32 flags;
  guint32 entry_count;
  guint32 *sample_numbers;
};

struct _GssBoxStshEntry
{
  gboolean present;
  guint32 shadowed_sample_number;
  guint32 sync_sample_number;
};

struct _GssBoxStsh
{
  gboolean present;
  guint8 version;
  guint32 flags;
  guint32 entry_count;
  struct _GssBoxStshEntry *entries;
};

struct _GssBoxStdp
{
  gboolean present;
  guint8 version;
  guint32 flags;
  guint16 *priorities;
};

struct _GssBoxEdts
{
  /* container */
};

struct _GssBoxElstEntry
{
  guint64 segment_duration;
  guint64 media_time;
  guint32 media_rate;
};

struct _GssBoxElst
{
  gboolean present;
  guint8 version;
  guint32 flags;
  guint32 entry_count;
  GssBoxElstEntry *entries;
};

struct _GssBoxUdta
{
  gboolean present;
  /* container */
};

struct _GssBoxCprt
{
  gboolean present;
  guint8 version;
  guint32 flags;
  char language[4];
  char *notice;
};

struct _GssBoxTrak
{
  /* container */

  GssBoxTkhd tkhd;
  GssBoxTref tref;
  GssBoxMdia mdia;
};

struct _GssBoxMvex
{
};

struct _GssBoxMeta
{
  gboolean present;
  guint8 version;
  guint32 flags;
};

struct _GssBoxIlst
{
  gboolean present;
  guint8 version;
  guint32 flags;
};

struct _GssBoxMdir
{
  gboolean present;
  guint8 version;
  guint32 flags;
};

struct _GssBoxMehd
{
  gboolean present;
  guint8 version;
  guint32 flags;
  guint64 fragment_duration;
};

struct _GssBoxTrex
{
  gboolean present;
  guint8 version;
  guint32 flags;

  guint32 track_id;
  guint32 default_sample_description_index;
  guint32 default_sample_duration;
  guint32 default_sample_size;
  guint32 default_sample_flags;
};

struct _GssBoxSidxEntry
{
  guint8 reference_type;
  guint32 reference_size;
  guint32 subsegment_duration;
  guint8 starts_with_sap;
  guint8 sap_type;
  guint32 sap_delta_time;
};

struct _GssBoxSidx
{
  guint8 version;
  guint32 flags;
  guint32 reference_id;
  guint32 timescale;
  guint64 earliest_presentation_time;
  guint64 first_offset;

  int n_entries;
  GssBoxSidxEntry *entries;
};

struct _GssBoxPssh
{
  gboolean present;
  guint8 version;
  guint32 flags;
  guint8 uuid[16];
  guint32 len;
  guint8 *data;
};

struct _GssBoxSinf
{
  guint32 original_atom;
};

struct _GssBoxSaiz
{
  gboolean present;
  guint8 version;
  guint32 flags;
  guint32 aux_info_type;
  guint32 aux_info_type_parameter;
  guint32 default_sample_info_size;
  guint32 sample_count;
  guint8 *sizes;
};

struct _GssBoxSaio
{
  gboolean present;
  guint8 version;
  guint32 flags;
  guint32 aux_info_type;
  guint32 aux_info_type_parameter;
  guint32 entry_count;
};

struct _GssBoxSkip
{
};

struct _GssBoxStore
{
  guint32 atom;
  gboolean present;
  guint8 *data;
  int size;
};

struct _GssBoxUUIDProtectionHeader
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

struct _GssBoxMoov
{
  guint8 version;
  guint32 flags;

  GssBoxMvhd mvhd;
  GssBoxTrak trak;
  GssBoxUdta udta;
  GssBoxMvex mvex;
};

#endif


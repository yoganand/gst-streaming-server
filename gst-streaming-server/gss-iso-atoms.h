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

/* Atom definitions */

typedef struct _AtomMfhd AtomMfhd;
typedef struct _AtomTfhd AtomTfhd;
typedef struct _AtomTrun AtomTrun;
typedef struct _AtomTrunSample AtomTrunSample;
typedef struct _AtomSdtp AtomSdtp;
typedef struct _AtomUUIDSampleEncryption AtomUUIDSampleEncryption;
typedef struct _AtomUUIDSampleEncryptionSample AtomUUIDSampleEncryptionSample;
typedef struct _AtomUUIDSampleEncryptionSampleEntry AtomUUIDSampleEncryptionSampleEntry;
typedef struct _AtomTraf AtomTraf;
typedef struct _AtomMoof AtomMoof;

typedef struct _AtomMvhd AtomMvhd;
typedef struct _AtomIods AtomIods;
typedef struct _AtomTrak AtomTrak;
typedef struct _AtomTkhd AtomTkhd;
typedef struct _AtomTref AtomTref;
typedef struct _AtomMdia AtomMdia;
typedef struct _AtomMdhd AtomMdhd;
typedef struct _AtomHdlr AtomHdlr;
typedef struct _AtomMinf AtomMinf;
typedef struct _AtomVmhd AtomVmhd;
typedef struct _AtomSmhd AtomSmhd;
typedef struct _AtomHmhd AtomHmhd;
typedef struct _AtomDinf AtomDinf;
typedef struct _AtomUrl_ AtomUrl_;
typedef struct _AtomUrn_ AtomUrn_;
typedef struct _AtomDrefEntry AtomDrefEntry;
typedef struct _AtomDref AtomDref;
typedef struct _AtomStbl AtomStbl;
typedef struct _AtomSttsEntry AtomSttsEntry;
typedef struct _AtomStts AtomStts;
typedef struct _AtomCttsEntry AtomCttsEntry;
typedef struct _AtomCtts AtomCtts;
typedef struct _AtomEsds AtomEsds;
typedef struct _AtomMp4v AtomMp4v;
typedef struct _AtomMp4a AtomMp4a;
typedef struct _AtomMp4s AtomMp4s;
typedef struct _AtomStsd AtomStsd;
typedef struct _AtomStsz AtomStsz;
typedef struct _AtomStscEntry AtomStscEntry;
typedef struct _AtomStsc AtomStsc;
typedef struct _AtomStco AtomStco;
typedef struct _AtomStss AtomStss;
typedef struct _AtomStshEntry AtomStshEntry;
typedef struct _AtomStsh AtomStsh;
typedef struct _AtomStdp AtomStdp;
typedef struct _AtomEdts AtomEdts;
typedef struct _AtomElstEntry AtomElstEntry;
typedef struct _AtomElst AtomElst;
typedef struct _AtomUdta AtomUdta;
typedef struct _AtomCprt AtomCprt;
typedef struct _AtomUUIDProtectionHeader AtomUUIDProtectionHeader;
typedef struct _AtomMoov AtomMoov;
typedef struct _AtomParser AtomParser;

typedef struct _AtomMvex AtomMvex;
typedef struct _AtomMeta AtomMeta;
typedef struct _AtomSkip AtomSkip;


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

};

/* From ISO/IEC 14496-1:2002 */

struct _AtomMvhd {
  guint8 version;
  guint32 flags;
  guint64 creation_time;
  guint64 modification_time;
  guint32 timescale;
  guint64 duration;
  guint32 next_track_id;
};

struct _AtomIods
{
  guint8 version;
  guint32 flags;
  /* object descriptor */
};

struct _AtomTkhd
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

struct _AtomTref
{
  gboolean present;
  /* contains other stuff */

  guint32 *track_ids;
};

struct _AtomMdia
{
  gboolean present;
  /* container */
};

struct _AtomMdhd
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

struct _AtomHdlr
{
  gboolean present;
  guint8 version;
  guint32 flags;
  guint32 handler_type;
  char *name;
};

struct _AtomMinf
{
  gboolean present;
  /* container */
};

struct _AtomVmhd
{
  gboolean present;
  guint8 version;
  guint32 flags;
};

struct _AtomSmhd
{
  gboolean present;
  guint8 version;
  guint32 flags;
};

struct _AtomHmhd
{
  gboolean present;
  guint8 version;
  guint32 flags;
  guint16 maxPDUsize;
  guint16 avgPDUsize;
  guint32 maxbitrate;
  guint32 avgbitrate;
};

struct _AtomDinf
{
  gboolean present;
  /* container */
};

struct _AtomUrl_
{
  gboolean present;
  guint8 version;
  guint32 flags;
  char *location;
};

struct _AtomUrn_
{
  gboolean present;
  guint8 version;
  guint32 flags;
  char *name;
  char *location;
};

struct _AtomDrefEntry {
  guint8 entry_version;
  guint32 entry_flags;
  struct _AtomUrn_ urn_;
  struct _AtomUrl_ url_;
};

struct _AtomDref
{
  gboolean present;
  guint8 version;
  guint32 flags;
  guint32 entry_count;
  struct _AtomDrefEntry *entries;
};

struct _AtomStbl
{
  /* container */
};

struct _AtomSttsEntry
{
  guint32 sample_count;
  gint32 sample_delta;
};

struct _AtomStts
{
  gboolean present;
  guint8 version;
  guint32 flags;
  guint32 entry_count;
  struct _AtomSttsEntry *entries;
};

struct _AtomCttsEntry
{
  guint32 sample_count;
  guint32 sample_offset;
};

struct _AtomCtts
{
  gboolean present;
  guint8 version;
  guint32 flags;
  guint32 entry_count;
  struct _AtomCttsEntry *entries;
};

struct _AtomEsds
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

struct _AtomMp4v
{
  gboolean present;
  guint16 data_reference_index;
  guint16 width;
  guint16 height;
  struct _AtomEsds es;

};

struct _AtomMp4a
{
  gboolean present;
  guint16 data_reference_index;
  guint16 channel_count;
  guint16 sample_size;
  guint32 sample_rate;
  struct _AtomEsds es;
};

struct _AtomMp4s
{
  gboolean present;
  guint16 data_reference_index;
  struct _AtomEsds es;
};

struct _AtomStsd
{
  gboolean present;
  guint8 version;
  guint32 flags;
  guint32 entry_count;
  //AtomStsdEntry *entries;

};

struct _AtomStsz
{
  gboolean present;
  guint8 version;
  guint32 flags;
  guint32 sample_size;
  guint32 sample_count;
  guint32 *sample_sizes;
};

struct _AtomStscEntry
{
  gboolean present;
  guint32 first_chunk;
  guint32 samples_per_chunk;
  guint32 sample_description_index;
};

struct _AtomStsc
{
  gboolean present;
  guint8 version;
  guint32 flags;
  guint32 entry_count;
  struct _AtomStscEntry *entries;
};

/* This is for both stco and co64 */
struct _AtomStco
{
  gboolean present;
  guint8 version;
  guint32 flags;
  guint32 entry_count;
  guint64 *chunk_offsets;
};

struct _AtomStss
{
  gboolean present;
  guint8 version;
  guint32 flags;
  guint32 entry_count;
  guint32 *sample_numbers;
};

struct _AtomStshEntry
{
  gboolean present;
  guint32 shadowed_sample_number;
  guint32 sync_sample_number;
};

struct _AtomStsh
{
  gboolean present;
  guint8 version;
  guint32 flags;
  guint32 entry_count;
  struct _AtomStshEntry *entries;
};

struct _AtomStdp
{
  gboolean present;
  guint8 version;
  guint32 flags;
  guint16 *priorities;
};

struct _AtomEdts
{
  /* container */
};

struct _AtomElstEntry
{
  guint64 segment_duration;
  gint64 media_time;
  gint16 media_rate;
};

struct _AtomElst
{
  gboolean present;
  guint8 version;
  guint32 flags;
  guint32 entry_count;
  struct _AtomElstEntry entries;
};

struct _AtomUdta
{
  gboolean present;
  /* container */
};

struct _AtomCprt
{
  gboolean present;
  guint8 version;
  guint32 flags;
  char language[4];
  char *notice;
};

struct _AtomTrak
{
  /* container */

  AtomTkhd tkhd;
  AtomTref tref;
  AtomMdia mdia;
};

struct _AtomMvex
{
};

struct _AtomMeta
{
};

struct _AtomSkip
{
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

struct _AtomMoov
{
  guint8 version;
  guint32 flags;

  AtomMvhd mvhd;
  AtomTrak trak;
  AtomUdta udta;
  AtomMvex mvex;
};

#endif


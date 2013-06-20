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

#include "gss-server.h"

G_BEGIN_DECLS

typedef struct _GssISMParser GssISMParser;

typedef struct _GssISMFragment GssISMFragment;
struct _GssISMFragment {
  int track_id;
  guint64 moof_offset;
  guint64 moof_size;
  guint64 mdat_offset;
  guint64 mdat_size;
  guint64 timestamp;
  guint64 duration;
  int n_samples;
  void *moof;
};


GssISMParser *gss_ism_parser_new (void);
void gss_ism_parser_free (GssISMParser *parser);
gboolean gss_ism_parser_parse_file (GssISMParser *parser,
    const char *filename);
GssISMFragment * gss_ism_parser_get_fragments (GssISMParser *parser, int track_id,
    int *n_fragments);
int gss_ism_parser_get_n_fragments (GssISMParser *parser, int track_id);
void gss_ism_parser_free (GssISMParser *parser);

void gss_ism_fragment_set_sample_encryption (GssISMFragment *fragment,
    int n_samples, guint64 *init_vectors, gboolean is_h264);
void gss_ism_fragment_serialize (GssISMFragment *fragment, guint8 **data,
    int *size);
int * gss_ism_fragment_get_sample_sizes (GssISMFragment *fragment);
void gss_ism_encrypt_samples (GssISMFragment * fragment, guint8 * mdat_data,
    guint8 *content_key);

G_END_DECLS

#endif


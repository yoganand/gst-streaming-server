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


#ifndef _GSS_SGLIST_H
#define _GSS_SGLIST_H

#include "gss-isom-boxes.h"
#include "gss-server.h"

G_BEGIN_DECLS

typedef struct _GssSGList GssSGList;
typedef struct _GssSGChunk GssSGChunk;

struct _GssSGChunk {
  gsize offset;
  gsize size;
};

struct _GssSGList {
  int n_chunks;
  GssSGChunk *chunks;
};


GssSGList *gss_sglist_new (int n_chunks);
void gss_sglist_free (GssSGList *sglist);
gsize gss_sglist_get_size (GssSGList *sglist);
gboolean gss_sglist_load (GssSGList *sglist, int fd, guint8 *dest,
    GError **error);


G_END_DECLS

#endif


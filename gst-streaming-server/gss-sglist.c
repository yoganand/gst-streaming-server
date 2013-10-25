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

#define HACK_CCFF 1

#include "config.h"

#include <gst/gst.h>

#include "gss-sglist.h"
#include "gss-log.h"

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>


GssSGList *
gss_sglist_new (int n_chunks)
{
  GssSGList *sglist;

  g_return_val_if_fail (n_chunks > 0, NULL);

  sglist = g_malloc0 (sizeof (GssSGList));
  sglist->n_chunks = n_chunks;
  sglist->chunks = g_malloc0 (sizeof (GssSGChunk) * n_chunks);

  return sglist;
}

void
gss_sglist_free (GssSGList * sglist)
{
  g_return_if_fail (sglist != NULL);

  g_free (sglist->chunks);
  g_free (sglist);
}

gsize
gss_sglist_get_size (GssSGList * sglist)
{
  int i;
  gsize size = 0;

  g_return_val_if_fail (sglist != NULL, 0);

  for (i = 0; i < sglist->n_chunks; i++) {
    size += sglist->chunks[i].size;
  }
  return size;
}

gboolean
gss_sglist_load (GssSGList * sglist, int fd, guint8 * dest, GError ** error)
{
  int i;
  off_t ret;
  ssize_t n;
  off_t offset = 0;

  for (i = 0; i < sglist->n_chunks; i++) {
    GST_DEBUG ("chunk %d: %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT,
        i, sglist->chunks[i].offset, sglist->chunks[i].size);
    ret = lseek (fd, sglist->chunks[i].offset, SEEK_SET);
    if (ret < 0) {
      GST_WARNING ("failed to seek to %" G_GUINT64_FORMAT
          " error=\"%s\"", sglist->chunks[i].offset, g_strerror (errno));
      if (error) {
        *error = g_error_new (_gss_error_quark, GSS_ERROR_FILE_SEEK,
            "failed to seek on file");
      }
      return FALSE;
    }

    n = read (fd, dest + offset, sglist->chunks[i].size);
    if (n < sglist->chunks[i].size) {
      GST_WARNING ("failed to read %" G_GUINT64_FORMAT " bytes at %"
          G_GUINT64_FORMAT " error=\"%s\"",
          sglist->chunks[i].size, sglist->chunks[i].offset, g_strerror (errno));
      if (error) {
        *error = g_error_new (_gss_error_quark, GSS_ERROR_FILE_READ,
            "failed to read from file");
      }
      return FALSE;
    }
    offset += sglist->chunks[i].size;
  }

  return TRUE;
}

void
gss_sglist_merge (GssSGList * sglist)
{
  int i;

  for (i = sglist->n_chunks - 1; i > 0; i--) {
    if (sglist->chunks[i].offset ==
        sglist->chunks[i - 1].offset + sglist->chunks[i - 1].size) {
      sglist->chunks[i - 1].size += sglist->chunks[i].size;
      sglist->chunks[i].size = 0;
      sglist->chunks[i].offset = 0;
    }
  }
}

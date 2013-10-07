/* GStreamer Streaming Server
 * Copyright (C) 2009-2012 Entropy Wave Inc <info@entropywave.com>
 * Copyright (C) 2009-2012 David Schleef <ds@schleef.org>
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


#ifndef _GSS_VOD_H_
#define _GSS_VOD_H_

#include <gst/gst.h>
#include <glib/gstdio.h>

#include "gss-server.h"

#define GSS_TYPE_VOD \
  (gss_vod_get_type())
#define GSS_VOD(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GSS_TYPE_VOD,GssVod))
#define GSS_VOD_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GSS_TYPE_VOD,GssVodClass))
#define GSS_VOD_GET_CLASS(obj) \
    (G_TYPE_INSTANCE_GET_CLASS ((obj), GSS_TYPE_VOD, GssVodClass))
#define GSS_IS_VOD(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GSS_TYPE_VOD))
#define GSS_IS_VOD_CLASS(obj) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GSS_TYPE_VOD))

typedef struct _GssVod GssVod;
typedef struct _GssVodClass GssVodClass;

struct _GssVod {
  GssModule module;
  GHashTable *cache;

  /* properties */
  char *endpoint;
  char *archive_dir;
  int dir_levels;
  int cache_size;
};

struct _GssVodClass {
  GssModuleClass module_class;

};

GType gss_vod_get_type (void);

GssVod *gss_vod_new (void);

#endif


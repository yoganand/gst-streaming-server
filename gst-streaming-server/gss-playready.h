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


#ifndef _GSS_PLAYREADY_H
#define _GSS_PLAYREADY_H

#include "gss-server.h"
#include "gss-adaptive.h"

G_BEGIN_DECLS

#define GSS_TYPE_PLAYREADY \
    (gss_playready_get_type())
#define GSS_PLAYREADY(obj) \
    (G_TYPE_CHECK_INSTANCE_CAST((obj),GSS_TYPE_PLAYREADY,GssPlayready))
#define GSS_PLAYREADY_CLASS(klass) \
    (G_TYPE_CHECK_CLASS_CAST((klass),GSS_TYPE_PLAYREADY,GssPlayreadyClass))
#define GSS_PLAYREADY_GET_CLASS(obj) \
    (G_TYPE_INSTANCE_GET_CLASS ((obj), GSS_TYPE_PLAYREADY, GssPlayreadyClass))
#define GSS_IS_PLAYREADY(obj) \
    (G_TYPE_CHECK_INSTANCE_TYPE((obj),GSS_TYPE_PLAYREADY))
#define GSS_IS_PLAYREADY_CLASS(obj) \
    (G_TYPE_CHECK_CLASS_TYPE((klass),GSS_TYPE_PLAYREADY))

struct _GssPlayready {
  GssObject object;

  /* properties */
  char *license_url;
  guint8 key_seed[30];
  gboolean allow_clear;

};

struct _GssPlayreadyClass
{
  GssObjectClass object_class;

};

GType gss_playready_get_type (void);

GssPlayready *gss_playready_new (void);
void gss_playready_set_key_seed_hex (GssPlayready *playready, const char *key_seed);
char * gss_playready_get_key_seed_hex (GssPlayready *playready);
void gss_playready_generate_key (GssPlayready *playready, guint8 *key,
        const guint8 * kid, int kid_len);
void gss_playready_add_resources (GssPlayready * playready, GssServer * server);

void gss_playready_setup (GssServer * server);
char * gss_playready_get_protection_header_base64 (GssAdaptive *adaptive,
    const char *la_url, const char *auth_token);
void gss_playready_encrypt_samples (GssIsomFragment * fragment,
    guint8 * mdat_data, guint8 * content_key);
void gss_playready_setup_iv (GssPlayready *playready, GssAdaptive * adaptive,
    GssAdaptiveLevel * level, GssIsomFragment * fragment);

GssDrmType gss_drm_get_drm_type (const char *s);
const char *gss_drm_get_drm_name (GssDrmType drm_type);

G_END_DECLS

#endif


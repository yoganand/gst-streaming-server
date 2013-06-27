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
#include "gss-smooth-streaming.h"

G_BEGIN_DECLS

void gss_playready_setup (GssServer * server);
guint8 * gss_playready_generate_key (guint8 *key_seed, int key_seed_len,
    guint8 *kid, int kid_len);
guint8 * gss_playready_get_default_key_seed (void);
char * gss_playready_get_protection_header_base64 (GssISM *ism,
    const char *la_url);

G_END_DECLS

#endif


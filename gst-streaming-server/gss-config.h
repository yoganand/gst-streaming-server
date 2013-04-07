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

#ifndef _GSS_CONFIG_H_
#define _GSS_CONFIG_H_

#include <glib-object.h>
#include <libxml/parser.h>

#include "gss-session.h"
#include "gss-transaction.h"


G_BEGIN_DECLS

#define GSS_TYPE_CONFIG \
  (gss_config_get_type())
#define GSS_CONFIG(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GSS_TYPE_CONFIG,GssConfig))
#define GSS_CONFIG_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GSS_TYPE_CONFIG,GssConfigClass))
#define GSS_CONFIG_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS ((obj), GSS_TYPE_CONFIG, GssConfigClass))
#define GSS_IS_CONFIG(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GSS_TYPE_CONFIG))
#define GSS_IS_CONFIG_CLASS(obj) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GSS_TYPE_CONFIG))


#define GSS_PARAM_SECURE (1<<27)
#define GSS_PARAM_MULTILINE (1<<28)
#define GSS_PARAM_HIDE (1<<29)
#define GSS_PARAM_FILE_UPLOAD (1<<30)

typedef struct _GssConfig GssConfig;
struct _GssConfig
{
  GObject object;

  /* properties */
  char *config_file;

  xmlDocPtr doc;

  GList *config_list;
};

typedef struct _GssConfigClass GssConfigClass;
struct _GssConfigClass
{
  GObjectClass object_class;

};

GType gss_config_get_type (void);

void gss_deinit (void);
void gss_init (void);

GObject *gss_config_create_object (GssConfig *config, GType type,
    const char *name);
void gss_config_load_object (GssConfig *config, GObject *object,
    const char *name);

void gss_config_attach (GssConfig *config, GObject *object);
void gss_config_save_config_file (GssConfig *config);
void gss_config_save_object (GObject *object);
void gss_config_load_config_file (GssConfig *config);

void gss_config_append_config_block (GObject *object, GssTransaction *t,
    gboolean show);
gboolean gss_config_handle_post (GObject *object, GssTransaction *t);
GHashTable * gss_config_get_post_hash (GssTransaction *t);
gboolean gss_config_handle_post_hash (GObject * object, GssTransaction * t,
    GHashTable *hash);
void gss_config_add_server_resources (GssServer *server);
void gss_config_post_resource (GssTransaction * t);


G_END_DECLS

#endif


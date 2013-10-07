/* GStreamer Streaming Server
 * Copyright (C) 2009-2013 Entropy Wave Inc <info@entropywave.com>
 * Copyright (C) 2009-2013 David Schleef <ds@schleef.org>
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

#include "config.h"

#include "gss-types.h"
#include "gss-module.h"
#include "gss-utils.h"
#include "gss-html.h"

enum
{
  MOO
};

#define DEFAULT_NAME ""
#define DEFAULT_TITLE ""


static void gss_module_finalize (GObject * module);
static void gss_module_set_property (GObject * module, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gss_module_get_property (GObject * module, guint prop_id,
    GValue * value, GParamSpec * pspec);


static GObjectClass *parent_class;

G_DEFINE_TYPE (GssModule, gss_module, GSS_TYPE_OBJECT);

static void
gss_module_init (GssModule * module)
{

}

static void
gss_module_class_init (GssModuleClass * module_class)
{
  G_OBJECT_CLASS (module_class)->set_property = gss_module_set_property;
  G_OBJECT_CLASS (module_class)->get_property = gss_module_get_property;
  G_OBJECT_CLASS (module_class)->finalize = gss_module_finalize;

#if 0
  g_object_class_install_property (G_OBJECT_CLASS (module_class),
      PROP_NAME, g_param_spec_string ("name", "Name",
          "Name", DEFAULT_NAME,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property (G_OBJECT_CLASS (module_class),
      PROP_TITLE, g_param_spec_string ("title", "Title",
          "Title", DEFAULT_TITLE,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
#endif

  parent_class = g_type_class_peek_parent (module_class);
}

static void
gss_module_finalize (GObject * gmodule)
{
  //GssModule *module = GSS_MODULE (gmodule);


  parent_class->finalize (gmodule);
}

static void
gss_module_set_property (GObject * module, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  //GssModule *gssmodule;

  //gssmodule = GSS_MODULE (module);

  switch (prop_id) {
#if 0
    case PROP_NAME:
      gss_module_set_name (gssmodule, g_value_get_string (value));
      break;
    case PROP_TITLE:
      gss_module_set_title (gssmodule, g_value_get_string (value));
      break;
#endif
    default:
      g_assert_not_reached ();
      break;
  }
}

static void
gss_module_get_property (GObject * module, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  //GssModule *gssmodule;

  //gssmodule = GSS_MODULE (module);

  switch (prop_id) {
#if 0
    case PROP_NAME:
      g_value_set_string (value, gssmodule->name);
      break;
    case PROP_TITLE:
      g_value_set_string (value, gssmodule->title);
      break;
#endif
    default:
      g_assert_not_reached ();
      break;
  }
}

void
gss_module_set_admin_resource (GssModule * module, GssResource * resource)
{
  g_return_if_fail (GSS_IS_MODULE (module));
  g_return_if_fail (resource != NULL);

  module->admin_resource = resource;
}

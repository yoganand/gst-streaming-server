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


#ifndef _GSS_MODULE_H
#define _GSS_MODULE_H

#include "gss-types.h"
#include "gss-object.h"

G_BEGIN_DECLS

#define GSS_TYPE_MODULE \
  (gss_module_get_type())
#define GSS_MODULE(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GSS_TYPE_MODULE,GssModule))
#define GSS_MODULE_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GSS_TYPE_MODULE,GssModuleClass))
#define GSS_MODULE_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS ((obj), GSS_TYPE_MODULE, GssModuleClass))
#define GSS_IS_MODULE(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GSS_TYPE_MODULE))
#define GSS_IS_MODULE_CLASS(obj) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GSS_TYPE_MODULE))

struct _GssModule {
  GssObject object;

  GssResource *admin_resource;
};

struct _GssModuleClass {
  GssObjectClass module_class;

};


GType gss_module_get_type (void);

void gss_module_set_admin_resource (GssModule *module, GssResource *resource);


G_END_DECLS

#endif


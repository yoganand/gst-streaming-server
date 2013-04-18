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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gstvalue.h>

#include "gss-config.h"
#include "gss-utils.h"
#include "gss-server.h"
#include "gss-html.h"
#include "gss-transaction.h"

#include <libxml/parser.h>

enum
{
  PROP_0,
  PROP_CONFIG_FILE
};

#define DEFAULT_CONFIG_FILE "config"

static void gss_config_finalize (GObject * object);
static void gss_config_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gss_config_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);


G_DEFINE_TYPE (GssConfig, gss_config, G_TYPE_OBJECT);


static void
gss_config_init (GssConfig * config)
{

  config->config_file = g_strdup (DEFAULT_CONFIG_FILE);
}

static void
gss_config_class_init (GssConfigClass * config_class)
{
  G_OBJECT_CLASS (config_class)->set_property = gss_config_set_property;
  G_OBJECT_CLASS (config_class)->get_property = gss_config_get_property;
  G_OBJECT_CLASS (config_class)->finalize = gss_config_finalize;

  g_object_class_install_property (G_OBJECT_CLASS (config_class),
      PROP_CONFIG_FILE, g_param_spec_string ("config-file",
          "Config File", "Config File", DEFAULT_CONFIG_FILE,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_CONSTRUCT |
              G_PARAM_STATIC_STRINGS)));

}

static void
gss_config_finalize (GObject * object)
{
  GssConfig *config = GSS_CONFIG (object);

  g_list_free (config->config_list);
  config->config_list = NULL;

  g_free (config->config_file);

  G_OBJECT_CLASS (gss_config_parent_class)->finalize (object);
}

static void
gss_config_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GssConfig *config;

  config = GSS_CONFIG (object);

  switch (prop_id) {
    case PROP_CONFIG_FILE:
      g_free (config->config_file);
      config->config_file = g_value_dup_string (value);
      break;
    default:
      g_assert_not_reached ();
      break;
  }
}

static void
gss_config_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GssConfig *config;

  config = GSS_CONFIG (object);

  switch (prop_id) {
    case PROP_CONFIG_FILE:
      g_value_set_string (value, config->config_file);
      break;
    default:
      g_assert_not_reached ();
      break;
  }
}


void
gss_init (void)
{

}

void
gss_deinit (void)
{
}

void
gss_config_attach (GssConfig * config, GObject * object)
{
  g_return_if_fail (GSS_IS_CONFIG (config));
  g_return_if_fail (G_IS_OBJECT (object));

  /* FIXME need a detach */

  g_object_set_data (object, "GssConfig", config);

  config->config_list = g_list_prepend (config->config_list, object);
}


void
gss_config_append_config_block (GObject * object, GssTransaction * t,
    gboolean show)
{
  GString *s = t->s;
  GParamSpec **pspecs;
  guint n_pspecs;
  int i;

  pspecs = g_object_class_list_properties (G_OBJECT_GET_CLASS (object),
      &n_pspecs);

  for (i = 0; i < (int) n_pspecs; i++) {
    if (!(pspecs[i]->flags & G_PARAM_READABLE)) {
      if (pspecs[i]->value_type == G_TYPE_BOOLEAN) {
        gss_html_append_button (s, g_param_spec_get_nick (pspecs[i]),
            pspecs[i]->name, "on");
      }
    }
  }

  if (!(t->resource->flags & GSS_RESOURCE_ADMIN)) {
    g_string_append (s, "<div class='accordion' id='accordion-config'>\n");
    g_string_append (s, "<div class='accordion-group'>\n");
    g_string_append (s, "<div class='accordion-heading'>\n");
    g_string_append (s, "<div class='accordion-toggle'>\n");
    g_string_append (s, "<button class='btn btn-mini' data-toggle='collapse' "
        "data-parent='#accordion-config' data-target='#collapse-config'>\n");
    g_string_append (s, "<b class='caret'></b> Edit\n");
    g_string_append (s, "</button>\n");
    g_string_append (s, "</div>\n");
    g_string_append_printf (s,
        "<div id='collapse-config' class='accordion-body collapse %s'>\n",
        show ? "in" : "out");
    g_string_append (s, "<div class='accordion-inner'>\n");
  }

  g_string_append_printf (s,
      "<form class='form-horizontal' method='post' enctype='multipart/form-data' >\n");

  if (t->session) {
    g_string_append_printf (s,
        "<input name='session_id' type='hidden' value='%s'>\n",
        t->session->session_id);
  }

  for (i = 0; i < (int) n_pspecs; i++) {
    char *value;
    const char *blurb;
    gboolean writable;

    if (!(pspecs[i]->flags & G_PARAM_READABLE))
      continue;
    if (pspecs[i]->flags & GSS_PARAM_HIDE)
      continue;
    if (strcmp (pspecs[i]->name, "name") == 0)
      continue;

    writable = (pspecs[i]->flags & G_PARAM_WRITABLE) &&
        !(pspecs[i]->flags & G_PARAM_CONSTRUCT_ONLY);

    if (strcmp (pspecs[i]->name, "width") == 0) {
      int width, height;

      g_object_get (object, "width", &width, "height", &height, NULL);

      g_string_append (s, "<div class='control-group'>\n");
      g_string_append_printf (s,
          "<label class='control-label' for='size'>Size</label>\n");

      g_string_append (s, "<div class='controls'>\n");
      g_string_append (s, "<div class='input-append'>\n");
      g_string_append_printf (s,
          "<input type='text' class='input-small' id='width' name='width' "
          "value='%d'>", width);
      g_string_append (s, "<span class='add-on'>&times;</span>");
      g_string_append_printf (s,
          "<input type='text' class='input-small' id='height' name='height' "
          "value='%d'>", height);

      g_string_append (s,
          "<div class='btn-toolbar'>"
          "<div class='btn-group'>"
          "<button class='btn dropdown-toggle' data-toggle='dropdown' href='#'><b class='caret'></b> Common Sizes</button>"
          "<ul class='dropdown-menu'>"
          "<li><a href='javascript:void(0)' onclick='setsize(320,240)'>320x240</a></li>"
          "<li><a href='javascript:void(0)' onclick='setsize(400,300)'>400x300</a></li>"
          "<li><a href='javascript:void(0)' onclick='setsize(480,360)'>480x360</a></li>"
          "<li><a href='javascript:void(0)' onclick='setsize(640,480)'>640x480</a></li>"
          "<li class='divider'></li>"
          "<li><a href='javascript:void(0)' onclick='setsize(320,180)'>320x180</a></li>"
          "<li><a href='javascript:void(0)' onclick='setsize(400,224)'>400x224</a></li>"
          "<li><a href='javascript:void(0)' onclick='setsize(480,270)'>480x270</a></li>"
          "<li><a href='javascript:void(0)' onclick='setsize(640,360)'>640x360</a></li>"
          "<li><a href='javascript:void(0)' onclick='setsize(800,450)'>800x450</a></li>"
          "<li><a href='javascript:void(0)' onclick='setsize(960,540)'>960x540</a></li>"
          "<li><a href='javascript:void(0)' onclick='setsize(1280,720)'>1280x720</a></li>"
          "<li><a href='javascript:void(0)' onclick='setsize(1600,900)'>1600x900</a></li>"
          "<li><a href='javascript:void(0)' onclick='setsize(1920,1080)'>1920x1080</a></li>"
          "</ul>" "</div>" "</div>\n");
      g_string_append (s, "<script>function setsize(width,height) {\n"
          "document.getElementById('width').value=width;"
          "document.getElementById('height').value=height;"
          "return false;" "}</script>\n");

      g_string_append_printf (s, "</div>");
      g_string_append_printf (s, "</div>");
      g_string_append_printf (s, "</div>");

      continue;
    }
    if (strcmp (pspecs[i]->name, "height") == 0)
      continue;

    value = g_object_get_as_string (object, pspecs[i]);

    g_string_append (s, "<div class='control-group'>\n");
    g_string_append (s, "<label class='control-label'");
    if (writable) {
      g_string_append_printf (s, " for='%s'", pspecs[i]->name);
    }
    if (g_object_property_is_default (object, pspecs[i])) {
      g_string_append_printf (s, ">%s</label>\n",
          g_param_spec_get_nick (pspecs[i]));
    } else {
      g_string_append_printf (s, "><b>%s</b></label>\n",
          g_param_spec_get_nick (pspecs[i]));
    }

    g_string_append (s, "<div class='controls'>\n");
    if (pspecs[i]->flags & GSS_PARAM_SECURE) {
      g_string_append_printf (s, "<div id='hide-%s'>\n", pspecs[i]->name);
      g_string_append_printf (s,
          "<button class='btn' type='button' onclick=\""
          "document.getElementById('show-%s').style.display='block';"
          "document.getElementById('hide-%s').style.display='none';"
          "\">Show</button>\n", pspecs[i]->name, pspecs[i]->name);
      g_string_append (s, "</div>\n");
      g_string_append_printf (s, "<div id='show-%s' style='display: none;'>\n",
          pspecs[i]->name);
    }
    blurb = g_param_spec_get_blurb (pspecs[i]);
    if (blurb[0] == '[') {
      g_string_append_printf (s, "<div class='input-append'>");
    }
    if (!writable) {
      char *safe;
      safe = gss_html_sanitize_entity (value);
      g_string_append_printf (s,
          "<span class='input uneditable-input'>%s</span>", safe);
      g_free (safe);
    } else if (G_TYPE_IS_ENUM (pspecs[i]->value_type)) {
      GEnumClass *eclass;
      int value;
      int j;

      eclass = G_ENUM_CLASS (g_type_class_peek (pspecs[i]->value_type));

      g_object_get (object, pspecs[i]->name, &value, NULL);

      g_string_append_printf (s, "<select id='%s' name='%s'>\n",
          pspecs[i]->name, pspecs[i]->name);

      for (j = 0; j < (int) eclass->n_values; j++) {
        GEnumValue *ev = eclass->values + j;
        g_string_append_printf (s, "<option value=\"%s\" %s>%s</option>\n",
            ev->value_name,
            (ev->value == value) ? "selected=\"selected\"" : "",
            ev->value_nick);
      }
      g_string_append_printf (s, "</select>");
    } else if (pspecs[i]->value_type == G_TYPE_BOOLEAN) {
      gboolean selected = TRUE;

      g_object_get (object, pspecs[i]->name, &selected, NULL);

      g_string_append_printf (s,
          "<input type='hidden' name='%s' value='0'>"
          "<input type='checkbox' class='input' "
          "id='%s' name='%s' value='1' %s>",
          pspecs[i]->name,
          pspecs[i]->name, pspecs[i]->name,
          selected ? "checked='checked'" : "");
    } else if ((pspecs[i]->value_type == G_TYPE_STRING)
        && pspecs[i]->flags & GSS_PARAM_FILE_UPLOAD) {
      g_string_append_printf (s,
          "<input type='file' class='input-xlarge' "
          "id='%s' name='%s' value=''>", pspecs[i]->name, pspecs[i]->name);
    } else if (pspecs[i]->value_type == G_TYPE_INT) {
      g_string_append_printf (s,
          "<input type='text' class='input-medium' id='%s' name='%s' "
          "value='%s'>", pspecs[i]->name, pspecs[i]->name, value);
    } else {
      char *u;
      u = gss_html_sanitize_attribute (value);
      if (pspecs[i]->flags & GSS_PARAM_MULTILINE) {
        g_string_append_printf (s,
            "<textarea rows='4' class='input-large' id='%s' name='%s'>"
            "%s</textarea>", pspecs[i]->name, pspecs[i]->name, u);
      } else {
        g_string_append_printf (s,
            "<input type='text' class='input-large' id='%s' name='%s' "
            "value='%s'>", pspecs[i]->name, pspecs[i]->name, u);
      }
      g_free (u);
    }
    if (blurb[0] == '[') {
      const char *end = strchr (blurb + 1, ']');
      int len = end - blurb - 1;
      g_string_append_printf (s, "<span class='add-on'>%.*s</span>",
          len, blurb + 1);
      g_string_append (s, "</div>\n");
      blurb = end + 1;
    }
    if (pspecs[i]->flags & GSS_PARAM_SECURE) {
      g_string_append (s, "</div>\n");
    }
    g_string_append_printf (s, "<span class='help-inline'>%s</span>", blurb);
    g_string_append (s, "</div>\n");
    g_string_append (s, "</div>\n");

    g_free (value);
  }

  g_string_append (s, "<div class='form-actions'>\n");
  g_string_append (s,
      "<button type='submit' class='btn btn-primary'>Save changes</button>\n");
  //g_string_append (s, "<button class='btn'>Cancel</button>");
  g_string_append (s, "</div>\n");

  g_free (pspecs);

  g_string_append (s, "</form>\n");

  if (!(t->resource->flags & GSS_RESOURCE_ADMIN)) {
    g_string_append (s, "</div>\n");
    g_string_append (s, "</div>\n");
    g_string_append (s, "</div>\n");
    g_string_append (s, "</div>\n");
    g_string_append (s, "</div>\n");
  }

}

GHashTable *
gss_config_get_post_hash (GssTransaction * t)
{
  GHashTable *hash;
  const char *content_type;

  content_type = soup_message_headers_get_one (t->msg->request_headers,
      "Content-Type");

  hash = NULL;
  if (g_ascii_strncasecmp (content_type, "application/x-www-form-urlencoded",
          33) == 0) {
    hash = soup_form_decode (t->msg->request_body->data);
  } else if (g_ascii_strncasecmp (content_type, "multipart/form-data", 19) == 0) {
    SoupBuffer *buffer = NULL;

    hash = soup_form_decode_multipart (t->msg, "none", NULL, NULL, &buffer);

    if (buffer) {
      soup_buffer_free (buffer);
    }
  }

  return hash;
}

gboolean
gss_config_handle_post (GObject * object, GssTransaction * t)
{
  GHashTable *hash;
  gboolean ret = FALSE;

  hash = gss_config_get_post_hash (t);
  if (hash) {
    ret = gss_config_handle_post_hash (object, t, hash);
    g_hash_table_unref (hash);
  }

  return ret;
}

gboolean
gss_config_handle_post_hash (GObject * object, GssTransaction * t,
    GHashTable * hash)
{
  char *key, *value;
  GHashTableIter iter;

  g_return_val_if_fail (hash != NULL, FALSE);

  g_hash_table_iter_init (&iter, hash);
  while (g_hash_table_iter_next (&iter, (gpointer) & key, (gpointer) & value)) {
    GParamSpec *ps;

    if (strcmp (key, "session_id") == 0)
      continue;

    ps = g_object_class_find_property (G_OBJECT_GET_CLASS (object), key);
    if (ps == NULL) {
      GST_WARNING ("request with bad property '%s'", key);
      return FALSE;
    }
  }

  g_hash_table_iter_init (&iter, hash);
  while (g_hash_table_iter_next (&iter, (gpointer) & key, (gpointer) & value)) {
    GParamSpec *ps;
    GValue val = G_VALUE_INIT;
    gboolean ret;

    if (strcmp (key, "session_id") == 0)
      continue;

    ps = g_object_class_find_property (G_OBJECT_GET_CLASS (object), key);

    g_value_init (&val, ps->value_type);
    if (ps->value_type == G_TYPE_STRING) {
      if (ps->flags & GSS_PARAM_MULTILINE) {
        char *to_lf;
        to_lf = gss_utils_crlf_to_lf (value);
        g_value_set_string (&val, to_lf);
        g_free (to_lf);
      } else {
        g_value_set_string (&val, value);
      }
      ret = TRUE;
    } else {
      ret = gst_value_deserialize (&val, value);
    }

    if (ret) {
      GST_DEBUG ("setting property %s to '%s'", key, value);
      g_object_set_property (object, key, &val);
    } else {
      GST_WARNING ("could not deserialize property %s, value %s", key, value);
    }

    g_value_unset (&val);
  }

  gss_config_save_object (object);

  return TRUE;
}


static void
gss_server_get_resource (GssTransaction * t)
{
  GssServer *server = GSS_SERVER (t->resource->priv);
  GString *s = g_string_new ("");

  t->s = s;

  gss_html_header (t);

  g_string_append (s, "<h1>Server Configuration</h1><br>\n");

  gss_config_append_config_block (G_OBJECT (server), t, FALSE);

  gss_html_footer (t);
}

static void
gss_server_post_resource (GssTransaction * t)
{
  GssServer *server = GSS_SERVER (t->resource->priv);
  gboolean ret;

  ret = gss_config_handle_post (G_OBJECT (server), t);

  if (ret) {
    gss_transaction_redirect (t, "");
  } else {
    gss_transaction_error (t, "Configuration Error");
  }
}

static void
gss_config_get_resource (GssTransaction * t)
{
  GssConfig *config = t->resource->priv;
  GString *s = g_string_new ("");
  GList *g;

  t->s = s;

  gss_html_header (t);

  g_string_append (s, "<h1>Configuration</h1>\n");
  g_string_append (s, "<br>\n");

  /* FIXME do not work */
  gss_html_append_button (s, "Download", "download", "download");
  gss_html_append_button (s, "Upload", "upload", "upload");
  gss_html_append_button (s, "Reset", "reset", "reset");

  g_string_append (s, "<br>\n");
  g_string_append (s, "<table class='table table-striped table-bordered "
      "table-condensed'>\n");
  g_string_append (s, "<thead>\n");
  g_string_append (s, "<tr>\n");
  g_string_append (s, "<th>Parameter</th>\n");
  g_string_append (s, "<th>Value</th>\n");
  g_string_append (s, "</tr>\n");
  g_string_append (s, "</thead>\n");
  g_string_append (s, "<tbody>\n");

  for (g = config->config_list; g; g = g_list_next (g)) {
    GObject *object = g->data;
    GParamSpec **pspecs;
    int n_properties;
    int i;

    pspecs = g_object_class_list_properties (G_OBJECT_GET_CLASS (object),
        (guint *) & n_properties);

    for (i = 0; i < n_properties; i++) {
      const GParamSpec *pspec = pspecs[i];
      char *value_string;
      char *safe;
      gboolean is_default;

      if (!(pspec->flags & G_PARAM_WRITABLE))
        continue;
      if (!(pspec->flags & G_PARAM_READABLE))
        continue;
      if (pspec->flags & G_PARAM_CONSTRUCT_ONLY)
        continue;
      if (strcmp (pspec->name, "name") == 0)
        continue;

      is_default = g_object_property_is_default (object, pspec);

      g_string_append (s, "<tr>\n");
      g_string_append (s, "<td>\n");
      if (!is_default)
        g_string_append (s, "<b>");
      g_string_append_printf (s, "%s.%s\n", GSS_OBJECT_NAME (object),
          pspec->name);
      g_string_append (s, "</td>\n");
      if (!is_default)
        g_string_append (s, "</b>");
      g_string_append (s, "<td>");
      if (!is_default)
        g_string_append (s, "<b>");
      g_string_append (s, "<pre class='pre-table'>");

      value_string = g_object_get_as_string (object, pspec);
      safe = gss_html_sanitize_entity (value_string);
      g_string_append (s, safe);
      g_free (safe);
      g_free (value_string);

      g_string_append (s, "</pre>");
      if (!is_default)
        g_string_append (s, "</b>");
      g_string_append (s, "</td>\n");
      g_string_append (s, "</tr>\n");
    }

    g_free (pspecs);
  }

  g_string_append (s, "</tbody>\n");
  g_string_append (s, "</table>\n");

  gss_html_footer (t);
}

void
gss_config_post_resource (GssTransaction * t)
{
  gboolean ret;
  GObject *object = G_OBJECT (t->resource->priv);

  ret = gss_config_handle_post (object, t);

  if (ret) {
    gss_transaction_redirect (t, "");
  } else {
    gss_transaction_error (t, "Configuration Error");
  }
}

static char *
get_xml_class_name (const char *class_name)
{

  if (strncmp (class_name, "Gst", 3) == 0) {
    class_name += 3;
  } else if (strncmp (class_name, "Gss", 3) == 0) {
    class_name += 3;
  } else if (strncmp (class_name, "Ew", 2) == 0) {
    class_name += 2;
  }
  return g_ascii_strdown (class_name, -1);
}

static void
gss_config_dump_object (GObject * object, xmlNsPtr ns, xmlNodePtr parent)
{
  xmlNodePtr node;
  GParamSpec **pspecs;
  int n_properties;
  int i;
  char *object_name;
  xmlChar *name;
  xmlChar *content;
  char *s;

  g_return_if_fail (G_IS_OBJECT (object));

  pspecs = g_object_class_list_properties (G_OBJECT_GET_CLASS (object),
      (guint *) & n_properties);

  s = get_xml_class_name (G_OBJECT_TYPE_NAME (object));
  name = xmlCharStrdup (s);
  g_free (s);
  node = xmlNewChild (parent, ns, name, NULL);
  xmlFree (name);

  g_object_get (object, "name", &object_name, NULL);
  name = xmlCharStrdup ("name");
  content = xmlCharStrdup (object_name);
  g_free (object_name);
  xmlNewTextChild (node, ns, name, content);
  xmlFree (name);
  xmlFree (content);

  for (i = 0; i < n_properties; i++) {
    const GParamSpec *pspec = pspecs[i];
    char *s;

    if (strcmp (pspec->name, "name") == 0)
      continue;
    if (!(pspec->flags & G_PARAM_READABLE))
      continue;
    if (!(pspec->flags & G_PARAM_WRITABLE))
      continue;
    if (pspec->flags & G_PARAM_CONSTRUCT_ONLY)
      continue;

    s = g_object_get_as_string (object, pspec);

    name = xmlCharStrdup (pspec->name);
    content = xmlCharStrdup (s);
    xmlNewTextChild (node, ns, name, content);
    xmlFree (name);
    xmlFree (content);
    g_free (s);
  }

  if (GSS_IS_PROGRAM (object)) {
    GssProgram *program = GSS_PROGRAM (object);
    GList *g;
    for (g = program->streams; g; g = g_list_next (g)) {
      GssStream *stream = g->data;
      gss_config_dump_object (G_OBJECT (stream), ns, node);
    }
  }

  g_free (pspecs);
}

static void
gss_config_append_config_file (GssConfig * config, GString * s)
{
  GList *g;
  xmlNsPtr ns;
  xmlDocPtr doc;

  doc = xmlNewDoc ((xmlChar *) "1.0");

  doc->xmlRootNode = xmlNewDocNode (doc, NULL, (xmlChar *) "oberon", NULL);
  ns = xmlNewNs (doc->xmlRootNode,
      (xmlChar *) "http://entropywave.com/oberon/1.0/", (xmlChar *) "ew");

  for (g = config->config_list; g; g = g_list_next (g)) {
    GObject *object = g->data;

    gss_config_dump_object (object, ns, doc->xmlRootNode);
  }

  {
    xmlChar *str;
    int len;
    xmlDocDumpFormatMemory (doc, &str, &len, 1);
    g_string_append (s, (char *) str);
    xmlFree (str);
  }

  xmlFreeDoc (doc);
}

void
gss_config_save_object (GObject * object)
{
  GssConfig *config;

  g_return_if_fail (G_IS_OBJECT (object));

  config = g_object_get_data (object, "GssConfig");
  if (!config) {
    GST_ERROR_OBJECT (object, "not attached to config object");
    return;
  }
  gss_config_save_config_file (config);
}

void
gss_config_save_config_file (GssConfig * config)
{
  GError *error = NULL;
  GString *s = g_string_new ("");
  gboolean ret;

  gss_config_append_config_file (config, s);

  ret = g_file_set_contents (config->config_file, s->str, s->len, &error);
  g_string_free (s, TRUE);
  if (!ret) {
    g_error_free (error);
  }
}

static void
gss_config_file_get_resource (GssTransaction * t)
{
  GssConfig *config = t->resource->priv;
  GString *s = g_string_new ("");

  t->s = s;

  gss_config_append_config_file (config, s);
}

static xmlNodePtr
get_child_node_by_name (xmlNodePtr root, const char *name)
{
  xmlNodePtr node;

  node = root->children;

  while (node) {
    if (node->type == XML_ELEMENT_NODE) {
      if (strcmp ((char *) node->name, name) == 0) {
        return node;
      }
    }
    node = node->next;
  }
  return NULL;
}

static xmlNodePtr
find_node (xmlNodePtr root, const char *type, const char *name)
{
  xmlNodePtr node;
  xmlNodePtr n;

  node = root->children;

  while (node) {
    if (node->type == XML_ELEMENT_NODE) {
      if (strcmp ((char *) node->name, type) == 0) {
        n = get_child_node_by_name (node, "name");
        if (n != NULL) {
          char *s;
          s = (char *) xmlNodeGetContent (n);
          if (strcmp (s, name) == 0) {
            xmlFree (s);
            return node;
          }
          xmlFree (s);
        }
      }
    }

    node = node->next;
  }

  return NULL;
}

void
gss_config_load_config_file (GssConfig * config)
{
  xmlNodePtr root;
  char *contents;
  gsize size;
  gboolean ret;

  ret = g_file_get_contents (config->config_file, &contents, &size, NULL);
  if (!ret) {
    return;
  }
  config->doc = xmlParseMemory (contents, size);
  g_free (contents);

  root = xmlDocGetRootElement (config->doc);
  if (root == NULL) {
    xmlFreeDoc (config->doc);
    config->doc = NULL;
    GST_WARNING ("corrupt config file");
    return;
  }
}

static int
get_num_children (xmlNodePtr node)
{
  xmlNodePtr n;
  int count = 0;

  n = node->children;
  while (n) {
    if (n->type == XML_ELEMENT_NODE) {
      count++;
    }
    n = n->next;
  }

  return count;
}

GObject *
gss_config_create_object (GssConfig * config, GType type, const char *name)
{
  xmlNodePtr root;
  xmlNodePtr node = NULL;
  xmlNodePtr n;
  char *type_name;
  int n_params;
  int i;
  GParameter *params = NULL;
  GObjectClass *object_class;
  GObject *obj = NULL;

  object_class = g_type_class_ref (type);
  type_name = get_xml_class_name (g_type_name (type));

  root = xmlDocGetRootElement (config->doc);
  if (root) {
    node = find_node (root, type_name, name);
  }
  if (node) {

    n_params = get_num_children (node);
    params = g_malloc0 (sizeof (GParameter) * n_params);

    n = node->children;
    n_params = 0;
    while (n) {
      if (n->type == XML_ELEMENT_NODE) {
        GParamSpec *pspec;
        xmlChar *s;

        s = xmlNodeGetContent (n);
        GST_DEBUG_OBJECT (config, "creating %s:%s to %s", name, n->name,
            (char *) s);
        pspec = g_object_class_find_property (object_class, (char *) n->name);
        if (pspec) {
          gboolean ret;

          params[n_params].name = pspec->name;
          g_value_init (&params[n_params].value, pspec->value_type);
          ret = gst_value_deserialize (&params[n_params].value, (char *) s);
          if (ret) {
            n_params++;
          } else {
            GST_WARNING_OBJECT (config,
                "could not deserialize property %s, value %s", n->name, s);
          }
        }
        xmlFree (s);
      }
      n = n->next;
    }
  } else {
    n_params = 1;
    params = g_malloc0 (sizeof (GParameter) * n_params);

    params[0].name = "name";
    g_value_init (&params[0].value, G_TYPE_STRING);
    g_value_set_string (&params[0].value, name);
  }

  obj = g_object_newv (type, n_params, params);
  gss_config_attach (config, obj);

  for (i = 0; i < n_params; i++) {
    g_value_unset (&params[i].value);
  }

  g_free (type_name);
  g_free (params);
  g_type_class_unref (object_class);

  return obj;
}

GObject *
gss_config_create_object_2 (GssConfig * config, GType parent_type,
    const char *parent_name, GType type, const char *name)
{
  xmlNodePtr root;
  xmlNodePtr node = NULL;
  xmlNodePtr n;
  char *type_name;
  int n_params;
  int i;
  GParameter *params = NULL;
  GObjectClass *object_class;
  GObject *obj = NULL;

  object_class = g_type_class_ref (type);

  root = xmlDocGetRootElement (config->doc);
  if (root) {
    type_name = get_xml_class_name (g_type_name (parent_type));
    node = find_node (root, type_name, parent_name);
    g_free (type_name);
  }
  if (node) {
    type_name = get_xml_class_name (g_type_name (type));
    node = find_node (node, type_name, name);
    g_free (type_name);
  }
  if (node) {

    n_params = get_num_children (node);
    params = g_malloc0 (sizeof (GParameter) * n_params);

    n = node->children;
    n_params = 0;
    while (n) {
      if (n->type == XML_ELEMENT_NODE) {
        GParamSpec *pspec;
        xmlChar *s;

        s = xmlNodeGetContent (n);
        GST_DEBUG_OBJECT (config, "creating %s:%s to %s", name, n->name,
            (char *) s);
        pspec = g_object_class_find_property (object_class, (char *) n->name);
        if (pspec) {
          gboolean ret;

          params[n_params].name = pspec->name;
          g_value_init (&params[n_params].value, pspec->value_type);
          ret = gst_value_deserialize (&params[n_params].value, (char *) s);
          if (ret) {
            n_params++;
          } else {
            GST_WARNING_OBJECT (config,
                "could not deserialize property %s, value %s", n->name, s);
          }
        }
        xmlFree (s);
      }
      n = n->next;
    }

    obj = g_object_newv (type, n_params, params);
    for (i = 0; i < n_params; i++) {
      g_value_unset (&params[i].value);
    }
  }


  g_free (params);
  g_type_class_unref (object_class);

  return obj;
}

void
gss_config_load_object (GssConfig * config, GObject * object, const char *name)
{
  xmlNodePtr root;
  xmlNodePtr node;
  xmlNodePtr n;
  char *type_name;

  if (config->doc == NULL)
    return;

  gss_config_attach (config, object);

  type_name = get_xml_class_name (G_OBJECT_TYPE_NAME (object));

  root = xmlDocGetRootElement (config->doc);

  node = find_node (root, type_name, name);
  if (node == NULL) {
    GST_WARNING ("config: failed to find %s:%s", type_name, name);
    goto done;
  }

  n = node->children;
  while (n) {
    if (n->type == XML_ELEMENT_NODE) {
      GParamSpec *pspec;
      xmlChar *s;

      s = xmlNodeGetContent (n);
      GST_DEBUG_OBJECT (config, "loading %s:%s to %s", name, n->name,
          (char *) s);
      pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (object),
          (char *) n->name);
      if (pspec && pspec->flags & G_PARAM_WRITABLE &&
          !(pspec->flags & G_PARAM_CONSTRUCT_ONLY)) {
        GValue value = G_VALUE_INIT;
        gboolean ret;

        g_value_init (&value, pspec->value_type);
        ret = gst_value_deserialize (&value, (char *) s);
        if (ret) {
          g_object_set_property (object, (char *) n->name, &value);
        } else {
          GST_WARNING_OBJECT (config,
              "could not deserialize property %s, value %s", n->name, s);
        }
        g_value_unset (&value);
      }
      xmlFree (s);
    }
    n = n->next;
  }

done:
  g_free (type_name);
}

void
gss_config_add_server_resources (GssServer * server)
{
  GssResource *r;

  r = gss_server_add_resource (server, "/admin/server", GSS_RESOURCE_ADMIN,
      GSS_TEXT_HTML, gss_server_get_resource, NULL, gss_server_post_resource,
      server);
  gss_server_add_admin_resource (server, r, "Server");

  r = gss_server_add_resource (server, "/admin/config", GSS_RESOURCE_ADMIN,
      GSS_TEXT_HTML, gss_config_get_resource, NULL, gss_config_post_resource,
      NULL);

  gss_server_add_resource (server, "/admin/config_file", GSS_RESOURCE_ADMIN,
      GSS_TEXT_PLAIN, gss_config_file_get_resource, NULL, NULL, NULL);
}

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

#include "config.h"

#include "gss-html.h"
#include "gss-transaction.h"

#include <string.h>
#include <json-glib/json-glib.h>


void
gss_transaction_redirect (GssTransaction * t, const char *target)
{
  char *s;

  s = g_strdup_printf
      ("<html><head><meta http-equiv='refresh' content='0'></head>\n"
      "<body>Oops, you were supposed to "
      "be redirected <a href='%s'>here</a>.</body></html>\n", target);
  soup_message_set_response (t->msg, GSS_TEXT_HTML, SOUP_MEMORY_TAKE, s,
      strlen (s));
  soup_message_headers_append (t->msg->response_headers, "Location", target);
  soup_message_set_status (t->msg, SOUP_STATUS_SEE_OTHER);
}

void
gss_transaction_error (GssTransaction * t, const char *message)
{
  GString *s = g_string_new ("");

  t->s = s;

  gss_html_header (t);

  g_string_append (s, "<h1>Configuration Failed</h1><hr>\n");
  g_string_append (s, "<p>Invalid configuration options were provided.\n"
      "Please return to previous page and retry.</p>\n");

  gss_html_footer (t);
  soup_message_set_status (t->msg, SOUP_STATUS_BAD_REQUEST);
}

static gboolean
unpause (gpointer priv)
{
  GssTransaction *t = (GssTransaction *) priv;

  soup_server_unpause_message (t->soupserver, t->msg);
  g_free (t);

  return FALSE;
}

void
gss_transaction_delay (GssTransaction * t, int msec)
{
  GssTransaction *new_t;

  /* FIXME this is pure evil */

  new_t = g_malloc0 (sizeof (GssTransaction));
  memcpy (new_t, t, sizeof (GssTransaction));

  soup_server_pause_message (t->soupserver, t->msg);
  g_timeout_add (msec, unpause, new_t);
}




/* some stuff copied from json-glib because it needs a one-line
 * modification to include all properties, not just non-default ones */

static JsonNode *gss_json_serialize_pspec (const GValue * real_value,
    GParamSpec * pspec);


static JsonObject *
gss_json_gobject_dump (GObject * gobject)
{
  JsonSerializableIface *iface = NULL;
  JsonSerializable *serializable = NULL;
  gboolean list_properties = FALSE;
  gboolean serialize_property = FALSE;
  gboolean get_property = FALSE;
  JsonObject *object;
  GParamSpec **pspecs;
  guint n_pspecs, i;

  if (JSON_IS_SERIALIZABLE (gobject)) {
    serializable = JSON_SERIALIZABLE (gobject);
    iface = JSON_SERIALIZABLE_GET_IFACE (gobject);
    list_properties = (iface->list_properties != NULL);
    serialize_property = (iface->serialize_property != NULL);
    get_property = (iface->get_property != NULL);
  }

  object = json_object_new ();

  if (list_properties)
    pspecs = json_serializable_list_properties (serializable, &n_pspecs);
  else
    pspecs =
        g_object_class_list_properties (G_OBJECT_GET_CLASS (gobject),
        &n_pspecs);

  for (i = 0; i < n_pspecs; i++) {
    GParamSpec *pspec = pspecs[i];
    GValue value = { 0, };
    JsonNode *node = NULL;

    /* read only what we can */
    if (!(pspec->flags & G_PARAM_READABLE))
      continue;

    g_value_init (&value, G_PARAM_SPEC_VALUE_TYPE (pspec));

    if (get_property)
      json_serializable_get_property (serializable, pspec, &value);
    else
      g_object_get_property (gobject, pspec->name, &value);

    /* if there is a serialization vfunc, then it is completely responsible
     * for serializing the property, possibly by calling the implementation
     * of the default JsonSerializable interface through chaining up
     */
    if (serialize_property) {
      node = json_serializable_serialize_property (serializable,
          pspec->name, &value, pspec);
    } else
      node = gss_json_serialize_pspec (&value, pspec);

    if (node)
      json_object_set_member (object, pspec->name, node);

    g_value_unset (&value);
  }

  g_free (pspecs);

  return object;
}

static JsonNode *
gss_json_gobject_serialize (GObject * gobject)
{
  JsonNode *retval;

  g_return_val_if_fail (G_IS_OBJECT (gobject), NULL);

  retval = json_node_new (JSON_NODE_OBJECT);
  json_node_take_object (retval, gss_json_gobject_dump (gobject));

  return retval;
}

gchar *
gss_json_gobject_to_data (GObject * gobject, gsize * length)
{
  JsonGenerator *gen;
  JsonNode *root;
  gchar *data;

  g_return_val_if_fail (G_OBJECT (gobject), NULL);

  root = gss_json_gobject_serialize (gobject);

  gen = g_object_new (JSON_TYPE_GENERATOR,
      "root", root, "pretty", TRUE, "indent", 2, NULL);

  data = json_generator_to_data (gen, length);
  g_object_unref (gen);

  json_node_free (root);

  return data;
}

static JsonNode *
gss_json_serialize_pspec (const GValue * real_value, GParamSpec * pspec)
{
  JsonNode *retval = NULL;
  GValue value = { 0, };
  JsonNodeType node_type;

  switch (G_TYPE_FUNDAMENTAL (G_VALUE_TYPE (real_value))) {
    case G_TYPE_INT64:
    case G_TYPE_BOOLEAN:
    case G_TYPE_DOUBLE:
      /* JSON native types */
      retval = json_node_new (JSON_NODE_VALUE);
      g_value_init (&value, G_VALUE_TYPE (real_value));
      g_value_copy (real_value, &value);
      json_node_set_value (retval, &value);
      g_value_unset (&value);
      break;

    case G_TYPE_STRING:
      /* strings might be NULL, so we handle it differently */
      if (!g_value_get_string (real_value))
        retval = json_node_new (JSON_NODE_NULL);
      else {
        retval = json_node_new (JSON_NODE_VALUE);
        json_node_set_string (retval, g_value_get_string (real_value));
        break;
      }
      break;

    case G_TYPE_INT:
      retval = json_node_new (JSON_NODE_VALUE);
      json_node_set_int (retval, g_value_get_int (real_value));
      break;

    case G_TYPE_FLOAT:
      retval = json_node_new (JSON_NODE_VALUE);
      json_node_set_double (retval, g_value_get_float (real_value));
      break;

    case G_TYPE_BOXED:
      if (G_VALUE_HOLDS (real_value, G_TYPE_STRV)) {
        gchar **strv = g_value_get_boxed (real_value);
        gint i, strv_len;
        JsonArray *array;

        strv_len = g_strv_length (strv);
        array = json_array_sized_new (strv_len);

        for (i = 0; i < strv_len; i++) {
          JsonNode *str = json_node_new (JSON_NODE_VALUE);

          json_node_set_string (str, strv[i]);
          json_array_add_element (array, str);
        }

        retval = json_node_new (JSON_NODE_ARRAY);
        json_node_take_array (retval, array);
      } else if (json_boxed_can_serialize (G_VALUE_TYPE (real_value),
              &node_type)) {
        gpointer boxed = g_value_get_boxed (real_value);

        retval = json_boxed_serialize (G_VALUE_TYPE (real_value), boxed);
      } else
        g_warning ("Boxed type '%s' is not handled by JSON-GLib",
            g_type_name (G_VALUE_TYPE (real_value)));
      break;

    case G_TYPE_UINT:
      retval = json_node_new (JSON_NODE_VALUE);
      json_node_set_int (retval, g_value_get_uint (real_value));
      break;

    case G_TYPE_LONG:
      retval = json_node_new (JSON_NODE_VALUE);
      json_node_set_int (retval, g_value_get_long (real_value));
      break;

    case G_TYPE_ULONG:
      retval = json_node_new (JSON_NODE_VALUE);
      json_node_set_int (retval, g_value_get_long (real_value));
      break;

    case G_TYPE_CHAR:
      retval = json_node_new (JSON_NODE_VALUE);
#if GLIB_CHECK_VERSION (2, 31, 0)
      json_node_set_int (retval, g_value_get_schar (real_value));
#else
      json_node_set_int (retval, g_value_get_char (real_value));
#endif
      break;

    case G_TYPE_UCHAR:
      retval = json_node_new (JSON_NODE_VALUE);
      json_node_set_int (retval, g_value_get_uchar (real_value));
      break;

    case G_TYPE_ENUM:
      retval = json_node_new (JSON_NODE_VALUE);
      json_node_set_int (retval, g_value_get_enum (real_value));
      break;

    case G_TYPE_FLAGS:
      retval = json_node_new (JSON_NODE_VALUE);
      json_node_set_int (retval, g_value_get_flags (real_value));
      break;

    case G_TYPE_OBJECT:
    {
      GObject *object = g_value_get_object (real_value);

      if (object != NULL) {
        retval = json_node_new (JSON_NODE_OBJECT);
        json_node_take_object (retval, gss_json_gobject_dump (object));
      } else
        retval = json_node_new (JSON_NODE_NULL);
    }
      break;

    case G_TYPE_NONE:
      retval = json_node_new (JSON_NODE_NULL);
      break;

    default:
      g_warning ("Unsupported type `%s'",
          g_type_name (G_VALUE_TYPE (real_value)));
      break;
  }

  return retval;
}

void
gss_transaction_dump (GssTransaction * t)
{
  SoupMessageHeadersIter iter;
  const char *name;
  const char *value;

  g_print ("Request: %s %s\n", t->msg->method, t->path);
  if (t->query) {
    GHashTableIter iter;
    char *key, *value;

    g_hash_table_iter_init (&iter, t->query);
    while (g_hash_table_iter_next (&iter, (gpointer) & key, (gpointer) & value)) {
      g_print ("  %s=%s\n", key, value);
    }
  }
  if (t->msg->method == SOUP_METHOD_POST) {
    g_print ("Content:\n%s\n", t->msg->request_body->data);
  }
  g_print ("From: %s\n",
      soup_address_get_physical (soup_client_context_get_address (t->client)));
  g_print ("Status: %d\n", t->msg->status_code);
  g_print ("Response Latency: %" G_GUINT64_FORMAT " us\n",
      t->completion_time - t->start_time);
  g_print ("Request Headers:\n");
  soup_message_headers_iter_init (&iter, t->msg->request_headers);
  while (soup_message_headers_iter_next (&iter, &name, &value)) {
    g_print ("  %s: %s\n", name, value);
  }

  g_print ("Response Headers:\n");
  soup_message_headers_iter_init (&iter, t->msg->response_headers);
  while (soup_message_headers_iter_next (&iter, &name, &value)) {
    g_print ("  %s: %s\n", name, value);
  }
  g_print ("\n");

}

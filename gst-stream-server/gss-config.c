
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gss-server.h"
#include "gss-config.h"

#include <glib/gstdio.h>



static EwConfigField *
ew_config_get_field (EwConfig *config, const char *key)
{
  EwConfigField *field;
  field = g_hash_table_lookup (config->hash, key);
  if (field == NULL) {
    field = g_malloc0 (sizeof (EwConfigField));
    field->value = g_strdup ("");
    g_hash_table_insert (config->hash, g_strdup(key), field);
  }
  return field;
}

static void
ew_config_field_free (gpointer data)
{
  EwConfigField *field = (EwConfigField *)data;
  g_free (field->value);
}

EwConfig *
ew_config_new (void)
{
  EwConfig *config;

  config = g_malloc0 (sizeof(EwConfig));

  config->hash = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
      ew_config_field_free);

  return config;
}

void
ew_config_free (EwConfig *config)
{
  g_hash_table_unref (config->hash);
  g_free (config);
}

void ew_config_set_config_filename (EwConfig *config, const char *filename)
{
  g_free (config->config_filename);
  config->config_filename = g_strdup (filename);
}

static int
get_timestamp (const char *filename)
{
  //GStatBuf statbuf;
  struct stat statbuf;
  int ret;

  ret = g_stat (filename, &statbuf);
  if (ret == 0) {
    return statbuf.st_mtime;
  }
  return 0;
}

void ew_config_check_config_file (EwConfig *config)
{
  int timestamp;

  timestamp = get_timestamp (config->config_filename);
  if (timestamp > config->config_timestamp) {
    ew_config_load_from_file (config);
  }
}

static int
compare (gconstpointer a, gconstpointer b)
{
  return strcmp ((const char *)a,(const char *)b);
}

void
ew_config_hash_to_string (GString *s, GHashTable *hash)
{
  GList *list;
  GList *g;

  list = g_hash_table_get_keys (hash);
  list = g_list_sort (list, compare);

  for(g=list;g;g=g_list_next(g)){
    const char *key = g->data;
    EwConfigField *field;

    field = g_hash_table_lookup (hash, key);
    if (!(field->flags & EW_CONFIG_FLAG_NOSAVE)) {
      char *esc = g_strescape (field->value, NULL);
      g_string_append_printf (s, "%s=%s\n", key, esc);
      g_free (esc);
    }
  }

  g_list_free (list);
}

void
ew_config_write_config_to_file (EwConfig *config)
{
  GString *s;

  s = g_string_new ("");
  ew_config_hash_to_string (s, config->hash);

  g_file_set_contents (config->config_filename, s->str, s->len, NULL);
  g_string_free (s, TRUE);

  config->config_timestamp = time(NULL);
}

void
ew_config_set (EwConfig *config, const char *key, const char *value)
{
  EwConfigField *field;
  field = ew_config_get_field (config, key);
  
  if (field->locked) return;

  if (strcmp (field->value, value) != 0) {
    g_free (field->value);
    field->value = g_strdup (value);

    if (field->notify) field->notify (key, field->notify_priv);
  }
}

void
ew_config_lock (EwConfig *config, const char *key)
{
  EwConfigField *field;
  field = ew_config_get_field (config, key);
  
  field->locked = TRUE;
}

const char *
ew_config_get (EwConfig *config, const char *key)
{
  EwConfigField *field;
  field = ew_config_get_field (config, key);
  return field->value;
}

gboolean
ew_config_value_is_equal (EwConfig *config, const char *key, const char *value)
{
  EwConfigField *field;
  field = ew_config_get_field (config, key);
  return (strcmp (field->value, value) == 0);
}

gboolean
ew_config_value_is_on (EwConfig *config, const char *key)
{
  EwConfigField *field;
  field = ew_config_get_field (config, key);
  return (strcmp (field->value, "on") == 0);
}

gboolean
ew_config_get_boolean (EwConfig *config, const char *key)
{
  EwConfigField *field;
  field = ew_config_get_field (config, key);
  if (strcmp (field->value, "on") == 0) return TRUE;
  if (strcmp (field->value, "true") == 0) return TRUE;
  if (strcmp (field->value, "yes") == 0) return TRUE;
  if (strcmp (field->value, "1") == 0) return TRUE;
  return FALSE;
}

int
ew_config_get_int (EwConfig *config, const char *key)
{
  EwConfigField *field;
  field = ew_config_get_field (config, key);
  return strtol (field->value, NULL, 0);
}

void
ew_config_load_defaults (EwConfig *config, EwConfigDefault *list)
{
  int i;
  for(i=0;list[i].name;i++){
    ew_config_set (config, list[i].name, list[i].default_value);
  }
}

void
_ew_config_load_from_file (EwConfig *config, gboolean lock)
{
  gboolean ret;
  gchar *contents;
  gsize length;
  gchar **lines;
  int i;

  ret = g_file_get_contents (config->config_filename, &contents, &length, NULL);
  if (!ret) return;

  lines = g_strsplit (contents, "\n", 0);

  for(i=0;lines[i];i++){
    char **kv;
    kv = g_strsplit (lines[i], "=", 2);
    if (kv[0] && kv[1]) {
      char *unesc = g_strcompress (kv[1]);
      ew_config_set (config, kv[0], unesc);
      g_free (unesc);
      if (lock) ew_config_lock (config, kv[0]);
    }
    g_strfreev (kv);
  }

  g_strfreev (lines);
  g_free (contents);

  config->config_timestamp = time(NULL);
}

void
ew_config_load_from_file (EwConfig *config)
{
  _ew_config_load_from_file (config, FALSE);
}

void
ew_config_load_from_file_locked (EwConfig *config, const char *filename)
{
  char *tmp = config->config_filename;
  config->config_filename = (char *)filename;
  _ew_config_load_from_file (config, TRUE);
  config->config_filename = tmp;
}

void ew_config_set_notify (EwConfig *config, const char *key,
    EwConfigNotifyFunc notify, void *notify_priv)
{
  EwConfigField *field;

  field = ew_config_get_field (config, key);
  field->notify = notify;
  field->notify_priv = notify_priv;
}


void
ew_config_handle_post (EwConfig *config, SoupMessage *msg)
{
  GHashTable *hash;
  char *filename, *media_type;
  SoupBuffer *buffer;
  const char *content_type;

  content_type = soup_message_headers_get_one (msg->request_headers,
      "Content-Type");

  hash = NULL;
  if (g_str_equal (content_type, "application/x-www-form-urlencoded")) {
    //g_print ("urlencoded: %s\n", msg->request_body->data);
    hash = soup_form_decode (msg->request_body->data);
  } else if (g_str_has_prefix (content_type, "multipart/form-data")) {
    //g_print("multipart/form-data\n");
    hash = soup_form_decode_multipart (msg, "logo_file", &filename,
        &media_type, &buffer);

    if (buffer && buffer->length > 0) {
      GError *error = NULL;
      gboolean ret;

      ret = g_file_set_contents ("logo.png", buffer->data, buffer->length,
          &error);
      if (!ret) {
        /* FIXME */
        //ew_server_log (server, "failed to write logo.png file");
      }
      soup_buffer_free (buffer);
    }
    if (g_hash_table_lookup (hash, "firmware_file")) {
      g_hash_table_unref (hash);
      g_free (filename);
      g_free (media_type);
      hash = soup_form_decode_multipart (msg, "firmware_file", &filename,
          &media_type, &buffer);

      if (buffer && buffer->length > 0) {
        g_file_set_contents ("/opt/entropywave/ew-oberon/new-firmware",
            buffer->data, buffer->length, NULL);
        g_file_set_contents ("/tmp/reboot", "", 0, NULL);
        soup_buffer_free (buffer);
      }
    }
    if (g_hash_table_lookup (hash, "cert_file")) {
      g_hash_table_unref (hash);
      g_free (filename);
      g_free (media_type);
      hash = soup_form_decode_multipart (msg, "cert_file", &filename,
          &media_type, &buffer);

      if (buffer && buffer->length > 0) {
        g_file_set_contents ("server.crt", buffer->data, buffer->length, NULL);
        soup_buffer_free (buffer);
      }
    }
    if (g_hash_table_lookup (hash, "key_file")) {
      g_hash_table_unref (hash);
      g_free (filename);
      g_free (media_type);
      hash = soup_form_decode_multipart (msg, "key_file", &filename,
          &media_type, &buffer);

      if (buffer && buffer->length > 0) {
        g_file_set_contents ("server.key", buffer->data, buffer->length, NULL);
        soup_buffer_free (buffer);
      }
    }
    g_free (filename);
    g_free (media_type);
  }

#if 0
  if (hash) {
    GHashTableIter iter;
    char *key, *value;
    g_hash_table_iter_init (&iter, hash);
    while (g_hash_table_iter_next (&iter, (gpointer)&key, (gpointer)&value)) {
      g_print("%s=%s\n", key, value);
    }
  }
#endif

  if (hash) {
    const char *s, *t;
    char *key, *value;
    GHashTableIter iter;

    g_hash_table_remove (hash, "session_id");
    g_hash_table_remove (hash, "firmware_file");
    g_hash_table_remove (hash, "key_file");
    g_hash_table_remove (hash, "cert_file");

    s = g_hash_table_lookup (hash, "poweroff");
    if (s) {
      g_file_set_contents ("/tmp/shutdown", "", 0, NULL);
    }

    s = g_hash_table_lookup (hash, "reboot");
    if (s) {
      g_file_set_contents ("/tmp/reboot", "", 0, NULL);
    }

    s = g_hash_table_lookup (hash, "admin_token0");
    t = g_hash_table_lookup (hash, "admin_token1");
    if (s || t) {
      const char *t = g_hash_table_lookup (hash, "admin_token1");

      if (s && t && strcmp (s, t) == 0) {
#define REALM "Entropy Wave E1000"
        ew_config_set (config, "admin_token",
            soup_auth_domain_digest_encode_password("admin", REALM, s));
#if 0
        ew_config_set (config, "admin_hash",
            password_hash ("admin", s));
#endif
      }
      g_hash_table_remove (hash, "admin_token0");
      g_hash_table_remove (hash, "admin_token1");
    }

    s = g_hash_table_lookup (hash, "editor_token0");
    t = g_hash_table_lookup (hash, "editor_token1");
    if (s || t) {
      const char *t = g_hash_table_lookup (hash, "editor_token1");

      if (s && t && strcmp (s, t) == 0) {
        ew_config_set (config, "editor_token",
            soup_auth_domain_digest_encode_password("editor", REALM, s));
#if 0
        ew_config_set (config, "editor_hash",
            password_hash ("admin", s));
#endif
      }
      g_hash_table_remove (hash, "editor_token0");
      g_hash_table_remove (hash, "editor_token1");
    }

    g_hash_table_iter_init (&iter, hash);
    while (g_hash_table_iter_next (&iter, (gpointer)&key, (gpointer)&value)) {
      ew_config_set (config, key, value);
    }
    ew_config_write_config_to_file (config);
  }
}
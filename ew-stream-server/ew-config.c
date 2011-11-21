
#include "config.h"

#include "ew-server.h"
#include "ew-config.h"
#include "ew-html.h"

#include <glib/gstdio.h>
#include <glib-object.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <net/if.h>
#include <string.h>
#include <glib/gstdio.h>


enum {
  PROP_PORT = 1
};


char * get_time_string (void);


/* EwConfig stuff */

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

static void
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



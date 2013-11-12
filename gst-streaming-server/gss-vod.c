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

#include "config.h"

#include "gss-vod.h"
#include "gss-utils.h"
#include "gss-config.h"
#include "gss-html.h"
#include "gss-adaptive.h"
#include "gss-playready.h"
#include "gss-soup.h"


enum
{
  PROP_0,
  PROP_ENDPOINT,
  PROP_ARCHIVE_DIR,
  PROP_DIR_LEVELS,
  PROP_CACHE_SIZE
};

#define DEFAULT_ENDPOINT "vod"
#define DEFAULT_ARCHIVE_DIR "vod"
#define DEFAULT_DIR_LEVELS 0
#define DEFAULT_CACHE_SIZE 100

static void gss_vod_finalize (GObject * object);
static void gss_vod_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gss_vod_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static void gss_vod_get_resource (GssTransaction * t);
static void gss_vod_post_resource (GssTransaction * t);
static GssAdaptive *gss_vod_get_adaptive (GssVod * vod, const char *key,
    const char *version, GssDrmType drm_type, GssAdaptiveStream stream_type);
static void gss_vod_get_adaptive_resource (GssTransaction * t);
static void gss_vod_attach (GssObject * object, GssServer * server);
static void gss_vod_player_get_resource (GssTransaction * t);

G_DEFINE_TYPE (GssVod, gss_vod, GSS_TYPE_MODULE);

static GObjectClass *parent_class;

static void
gss_vod_init (GssVod * vod)
{
  vod->cache = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
      (GDestroyNotify) gss_adaptive_free);
}

static void
gss_vod_class_init (GssVodClass * vod_class)
{
  G_OBJECT_CLASS (vod_class)->set_property = gss_vod_set_property;
  G_OBJECT_CLASS (vod_class)->get_property = gss_vod_get_property;
  G_OBJECT_CLASS (vod_class)->finalize = gss_vod_finalize;

  GSS_OBJECT_CLASS (vod_class)->attach = gss_vod_attach;

  g_object_class_install_property (G_OBJECT_CLASS (vod_class),
      PROP_ENDPOINT, g_param_spec_string ("endpoint", "Endpoint",
          "Endpoint", DEFAULT_ENDPOINT,
          (GParamFlags) (G_PARAM_CONSTRUCT | G_PARAM_READWRITE |
              G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property (G_OBJECT_CLASS (vod_class),
      PROP_ARCHIVE_DIR, g_param_spec_string ("archive-dir", "Archive Directory",
          "Archive directory", DEFAULT_ARCHIVE_DIR,
          (GParamFlags) (G_PARAM_CONSTRUCT | G_PARAM_READWRITE |
              G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property (G_OBJECT_CLASS (vod_class),
      PROP_DIR_LEVELS, g_param_spec_int ("dir-levels", "Directory Levels",
          "Directory levels", 0, 4, DEFAULT_DIR_LEVELS,
          (GParamFlags) (G_PARAM_CONSTRUCT | G_PARAM_READWRITE |
              G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property (G_OBJECT_CLASS (vod_class),
      PROP_CACHE_SIZE, g_param_spec_int ("cache-size", "Cache Size",
          "Number of streams to hold in memory.", 1, 10000, DEFAULT_CACHE_SIZE,
          (GParamFlags) (G_PARAM_CONSTRUCT | G_PARAM_READWRITE |
              G_PARAM_STATIC_STRINGS)));

  parent_class = g_type_class_peek_parent (vod_class);
}

static void
gss_vod_finalize (GObject * object)
{
  GssVod *vod;

  vod = GSS_VOD (object);

  g_free (vod->endpoint);
  g_free (vod->archive_dir);
  g_hash_table_unref (vod->cache);

  parent_class->finalize (object);
}

static void
string_replace (char **s_ptr, char *s)
{
  char *old = *s_ptr;
  *s_ptr = s;
  g_free (old);
}

static void
gss_vod_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GssVod *vod;

  vod = GSS_VOD (object);

  switch (prop_id) {
    case PROP_ENDPOINT:
      string_replace (&vod->endpoint, g_value_dup_string (value));
      break;
    case PROP_ARCHIVE_DIR:
      string_replace (&vod->archive_dir, g_value_dup_string (value));
      break;
    case PROP_DIR_LEVELS:
      vod->dir_levels = g_value_get_int (value);
      break;
    case PROP_CACHE_SIZE:
      vod->cache_size = g_value_get_int (value);
      break;
    default:
      g_assert_not_reached ();
      break;
  }
}

static void
gss_vod_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GssVod *vod;

  vod = GSS_VOD (object);

  switch (prop_id) {
    case PROP_ENDPOINT:
      g_value_set_string (value, vod->endpoint);
      break;
    case PROP_ARCHIVE_DIR:
      g_value_set_string (value, vod->archive_dir);
      break;
    case PROP_DIR_LEVELS:
      g_value_set_int (value, vod->dir_levels);
      break;
    case PROP_CACHE_SIZE:
      g_value_set_int (value, vod->cache_size);
      break;
    default:
      g_assert_not_reached ();
      break;
  }
}


GssVod *
gss_vod_new (void)
{
  GssVod *vod;

  vod = g_object_new (GSS_TYPE_VOD, "name", "admin.vod", NULL);

  return vod;
}

static void
gss_vod_attach (GssObject * object, GssServer * server)
{
  GssVod *vod = GSS_VOD (object);
  GssResource *r;

  r = gss_server_add_resource (GSS_OBJECT_SERVER (object), "/admin/vod",
      GSS_RESOURCE_ADMIN, GSS_TEXT_HTML, gss_vod_get_resource, NULL,
      gss_vod_post_resource, vod);
  r->name = g_strdup ("Video On Demand");
  gss_module_set_admin_resource (GSS_MODULE (vod), r);

  gss_server_add_resource (GSS_OBJECT_SERVER (object), "/vod/",
      GSS_RESOURCE_PREFIX, NULL, gss_vod_get_adaptive_resource, NULL, NULL,
      vod);

  gss_server_add_resource (GSS_OBJECT_SERVER (object), "/vod-player",
      GSS_RESOURCE_UI, GSS_TEXT_HTML, gss_vod_player_get_resource,
      NULL, NULL, vod);
}

static void
gss_vod_get_resource (GssTransaction * t)
{
  GssVod *vod = GSS_VOD (t->resource->priv);
  GString *s = g_string_new ("");

  t->s = s;

  gss_html_header (t);

  GSS_A ("<h1>Video On Demand</h1>\n");

  gss_config_append_config_block (G_OBJECT (vod), t, TRUE);

  gss_html_footer (t);
}


static void
gss_vod_post_resource (GssTransaction * t)
{
  GssVod *vod = GSS_VOD (t->resource->priv);
  gboolean ret = FALSE;
  GHashTable *hash;

  hash = gss_config_get_post_hash (t);
  if (hash) {
    const char *value;

    value = g_hash_table_lookup (hash, "action");
    if (value) {
    } else {
      ret = gss_config_handle_post_hash (G_OBJECT (vod), t, hash);
    }
  }

  if (ret) {
    gss_config_save_object (G_OBJECT (vod));
    gss_transaction_redirect (t, "");
  } else {
    gss_transaction_error (t, "Configuration Error");
  }

}

#if 0
static gboolean
chop_path (const char *path, char *parts[7])
{
  char *s;
  int j;

  s = strdup (path);

  for (j = 0; j < 6; j++) {
    parts[j] = s;
    while (s[0] && s[0] != '/')
      s++;
    if (s[0] == 0) {
      g_free (parts[0]);
      return FALSE;
    }
    s[0] = 0;
    s++;
  }
  parts[j] = s;

  return TRUE;
}
#endif

static char *
chomp (const char **s)
{
  const char *delim;
  char *key;

  delim = strchr (*s, '/');
  if (delim == NULL) {
    key = strdup (*s);
    *s += strlen (*s);
  } else {
    key = strndup (*s, delim - *s);
    *s = delim + 1;
  }

  return key;
}

static void
gss_vod_get_adaptive_resource (GssTransaction * t)
{
  GssVod *vod = t->resource->priv;
  char *key;
  char *content_version;
  GssAdaptive *adaptive;
  const char *path;
  char *drm;
  char *stream;
  GssDrmType drm_type;
  GssAdaptiveStream stream_type;

  GST_DEBUG ("path: %s", t->path);

  path = t->path + 2 + strlen (vod->endpoint);

  key = chomp (&path);
  content_version = chomp (&path);
  drm = chomp (&path);
  drm_type = gss_drm_get_drm_type (drm);
  stream = chomp (&path);
  stream_type = gss_adaptive_get_stream_type (stream);

  if (drm_type == GSS_DRM_UNKNOWN) {
    gss_transaction_error_not_found (t, "invalid drm type");
    goto error;
  }

  if (drm_type == GSS_DRM_CLEAR && !t->server->playready->allow_clear) {
    gss_transaction_error_not_found (t, "clear streaming disabled");
    goto error;
  }

  if (stream_type == GSS_ADAPTIVE_STREAM_UNKNOWN) {
    gss_transaction_error_not_found (t, "invalid stream type");
    goto error;
  }

  adaptive =
      gss_vod_get_adaptive (vod, key, content_version, drm_type, stream_type);
  if (adaptive == NULL) {
    GST_DEBUG ("failed to load %s", key);
    gss_transaction_error_not_found (t, "failed to load");
    goto error;
  }

  GST_DEBUG ("subpath: %s", path);

  gss_adaptive_get_resource (t, adaptive, path);

error:
  g_free (key);
  g_free (content_version);
  g_free (stream);
  g_free (drm);
}

static GssAdaptive *
gss_vod_get_adaptive (GssVod * vod, const char *key, const char *version,
    GssDrmType drm_type, GssAdaptiveStream stream_type)
{
  GssAdaptive *adaptive;
  char *hash_key;

  hash_key = g_strdup_printf ("%s/%s/%s/%s",
      key, version, gss_drm_get_drm_name (drm_type),
      gss_adaptive_stream_get_name (stream_type));

  adaptive = g_hash_table_lookup (vod->cache, hash_key);
  if (adaptive == NULL) {
    char *dir;

    switch (vod->dir_levels) {
      case 0:
        dir = g_strdup_printf ("%s/%s", vod->archive_dir, key);
        break;
      case 1:
        dir = g_strdup_printf ("%s/%c/%s", vod->archive_dir, key[0], key);
        break;
      case 2:
        dir =
            g_strdup_printf ("%s/%c/%c/%s", vod->archive_dir, key[0], key[1],
            key);
        break;
      case 3:
        dir =
            g_strdup_printf ("%s/%c/%c/%c/%s", vod->archive_dir, key[0], key[1],
            key[2], key);
        break;
      default:
        g_assert_not_reached ();
    }

    adaptive =
        gss_adaptive_load (GSS_OBJECT_SERVER (vod), key, dir, version, drm_type,
        stream_type);
    g_free (dir);
    if (adaptive == NULL) {
      g_free (hash_key);
      return NULL;
    }
    g_hash_table_replace (vod->cache, hash_key, adaptive);
  } else {
    g_free (hash_key);
  }
  return adaptive;
}

static void
gss_vod_player_get_resource (GssTransaction * t)
{
  //GssVod *vod = GSS_VOD (t->resource->priv);
  GString *s = g_string_new ("");
  char *url;
  char *base_url;
  const char *content_id;

  t->s = s;

  content_id = NULL;
  if (t->query) {
    content_id = g_hash_table_lookup (t->query, "content_id");
  }
  if (content_id == NULL) {
    content_id = "elephantsdream";
  }
  base_url = gss_soup_get_base_url_http (t->server, t->msg);
  url = g_strdup_printf ("%s/vod/%s/0/clear/isoff-ondemand/manifest.mpd",
      base_url, content_id);
  g_free (base_url);

  gss_html_header (t);

  GSS_A ("<h1>Video On Demand</h1>\n");
  GSS_A ("<br>\n");
  GSS_A ("<script src=\"dash.min.js\"></script>\n");
  GSS_A ("<script>\n");
  GSS_A ("function startVideo() {\n");
  GSS_A ("  video = document.querySelector(\".dash-video-player video\");\n");
  GSS_A ("  context = new Dash.di.DashContext();\n");
  GSS_A ("  player = new MediaPlayer(context);\n");
  GSS_A ("  player.startup();\n");
  GSS_A ("  player.attachView(video);\n");
  GSS_A ("  player.setAutoPlay(false);\n");
  GSS_P ("  url = \"%s\";\n", url);
  GSS_A ("  player.attachSource(url);\n");
  GSS_A ("}\n");
  GSS_A ("</script>\n");
  GSS_A ("<style>\n");
  GSS_A ("  video {\n");
  GSS_A ("    width: 640px;\n");
  GSS_A ("    height: 360px\n");
  GSS_A ("  }\n");
  GSS_A ("</style>\n");
  GSS_A ("<div class=\"dash-video-player\">\n");
  GSS_A ("  <video controls=\"true\" autoplay=\"true\"></video>\n");
  GSS_A ("</div>\n");
  GSS_A ("<script>\n");
  GSS_A ("document.body.onload=startVideo()\n");
  GSS_A ("</script>\n");
  GSS_A ("\n");
  GSS_A ("\n");

  gss_html_footer (t);

  g_free (url);
}

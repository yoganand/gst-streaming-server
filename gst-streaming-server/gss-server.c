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

#include "gss-server.h"
#include "gss-html.h"
#include "gss-session.h"
#include "gss-soup.h"
#ifdef ENABLE_RTSP
#include "gss-rtsp.h"
#endif
#include "gss-content.h"
#include "gss-utils.h"
#include "gss-vod.h"
#include "gss-adaptive.h"
#include "gss-playready.h"
#include "gss-log.h"

#define GST_CAT_DEFAULT gss_debug


#define BASE "/"

enum
{
  PROP_0,
  PROP_ENABLE_PUBLIC_INTERFACE,
  PROP_HTTP_PORT,
  PROP_HTTPS_PORT,
  PROP_SERVER_HOSTNAME,
  PROP_MAX_CONNECTIONS,
  PROP_MAX_RATE,
  PROP_ADMIN_HOSTS_ALLOW,
  PROP_KIOSK_HOSTS_ALLOW,
  PROP_REALM,
  PROP_ADMIN_TOKEN,
  PROP_ENABLE_HTML5_VIDEO,
  PROP_ENABLE_CORTADO,
  PROP_ENABLE_FLASH,
  PROP_ENABLE_RTSP,
  PROP_ENABLE_RTMP,
  PROP_ENABLE_VOD,
  PROP_ARCHIVE_DIR,
  PROP_CAS_SERVER
};

#define DEFAULT_ENABLE_PUBLIC_INTERFACE TRUE
#define DEFAULT_HTTP_PORT 80
#define DEFAULT_HTTPS_PORT 443
#define DEFAULT_SERVER_HOSTNAME ""
#define DEFAULT_MAX_CONNECTIONS 10000
#define DEFAULT_MAX_RATE 100000
#define DEFAULT_ADMIN_HOSTS_ALLOW "0.0.0.0/0"
#define DEFAULT_KIOSK_HOSTS_ALLOW ""
/* This is the result of soup_auth_domain_digest_encode_password ("admin",
 * "GStreamer Streaming Server", "admin"); */
#define DEFAULT_ADMIN_TOKEN "f09e5ebc80c348d2ccf8a59a8cd37827"
#define DEFAULT_REALM "GStreamer Streaming Server"
#define DEFAULT_ENABLE_HTML5_VIDEO TRUE
#define DEFAULT_ENABLE_CORTADO FALSE
#define DEFAULT_ENABLE_FLASH TRUE
#define DEFAULT_ENABLE_RTSP FALSE
#define DEFAULT_ENABLE_RTMP FALSE
#define DEFAULT_ENABLE_VOD FALSE
#ifdef USE_LOCAL
#define DEFAULT_ARCHIVE_DIR "."
#else
#define DEFAULT_ARCHIVE_DIR "/mnt/sdb1"
#endif
#define DEFAULT_CAS_SERVER "https://10.0.2.23:8444/cas"

/* Server Resources */
static void gss_server_resource_main_page (GssTransaction * transaction);
static void gss_server_resource_list (GssTransaction * transaction);
static void gss_server_resource_about (GssTransaction * t);
static void gss_asset_get_resource (GssTransaction * t);

/* GssServer internals */
static void gss_server_resource_callback (SoupServer * soupserver,
    SoupMessage * msg, const char *path, GHashTable * query,
    SoupClientContext * client, gpointer user_data);
static void gss_server_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gss_server_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static void gss_server_setup_resources (GssServer * server);
static void gss_server_attach (GssObject * object, GssServer * x_server);


static gboolean periodic_timer (gpointer data);


G_DEFINE_TYPE (GssServer, gss_server, GSS_TYPE_MODULE);


static const gchar *soup_method_source;
#define SOUP_METHOD_SOURCE (soup_method_source)

static GObjectClass *parent_class;

static SoupServer *
get_http_server (int port)
{
  SoupAddress *if6;
  SoupServer *server;

  if6 = soup_address_new_any (SOUP_ADDRESS_FAMILY_IPV6, port);
  server = soup_server_new (SOUP_SERVER_INTERFACE, if6, SOUP_SERVER_PORT,
      port, NULL);
  g_object_unref (if6);
  if (server)
    return server;

  /* try again with just IPv4 */
  server = soup_server_new (SOUP_SERVER_PORT, port, NULL);
  return server;
}

static void
gss_server_set_http_port (GssServer * server, int port)
{
  if (port == 0) {
    GST_DEBUG_OBJECT (server, "trying port 80");
    server->server = get_http_server (80);
    port = 80;
    if (server->server == NULL) {
      GST_DEBUG_OBJECT (server, "trying port 8080");
      server->server = get_http_server (8080);
      port = 8080;
    }
  } else {
    GST_DEBUG_OBJECT (server, "trying port %d", port);
    server->server = get_http_server (port);
  }

  if (server->server == NULL) {
    return;
  }

  server->http_port = port;

  g_free (server->base_url);
  if (server->http_port == 80) {
    server->base_url = g_strdup_printf ("http://%s", server->server_hostname);
  } else {
    server->base_url = g_strdup_printf ("http://%s:%d", server->server_hostname,
        server->http_port);
  }



  if (server->server) {
    soup_server_add_handler (server->server, "/", gss_server_resource_callback,
        server, NULL);
    soup_server_run_async (server->server);
  }
}

static SoupServer *
get_https_server (int port)
{
  SoupAddress *if6;
  SoupServer *server;

  if6 = soup_address_new_any (SOUP_ADDRESS_FAMILY_IPV6, port);
  server = soup_server_new (SOUP_SERVER_PORT, port,
      SOUP_SERVER_INTERFACE, if6,
      SOUP_SERVER_SSL_CERT_FILE, "server.crt",
      SOUP_SERVER_SSL_KEY_FILE, "server.key", NULL);
  g_object_unref (if6);
  if (server)
    return server;

  /* try again with just IPv4 */
  server = soup_server_new (SOUP_SERVER_PORT, port,
      SOUP_SERVER_SSL_CERT_FILE, "server.crt",
      SOUP_SERVER_SSL_KEY_FILE, "server.key", NULL);
  return server;
}

static void
gss_server_set_https_port (GssServer * server, int port)
{
  if (port == 0) {
    server->ssl_server = get_https_server (443);
    port = 443;
    if (server->ssl_server == NULL) {
      server->ssl_server = get_https_server (8443);
      port = 8443;
    }
  } else {
    server->ssl_server = get_https_server (port);
  }

  if (server->ssl_server == NULL) {
    return;
  }

  server->https_port = port;

  g_free (server->base_url_https);
  if (server->https_port == 443) {
    server->base_url_https = g_strdup_printf ("https://%s",
        server->server_hostname);
  } else {
    server->base_url_https = g_strdup_printf ("https://%s:%d",
        server->server_hostname, server->https_port);
  }

  if (server->ssl_server) {
    soup_server_add_handler (server->ssl_server, "/",
        gss_server_resource_callback, server, NULL);
    soup_server_run_async (server->ssl_server);
  }
}

static void
gss_server_init (GssServer * server)
{
  char *s;

  server->metrics = gss_metrics_new ();

  server->resources = g_hash_table_new_full (g_str_hash, g_str_equal,
      NULL, (GDestroyNotify) gss_resource_free);

  server->client_session = soup_session_async_new ();

  server->enable_public_interface = DEFAULT_ENABLE_PUBLIC_INTERFACE;
  s = gss_utils_gethostname ();
  gss_server_set_server_hostname (server, s);
  g_free (s);
  gss_object_set_title (GSS_OBJECT (server), "GStreamer Streaming Server");
  server->max_connections = DEFAULT_MAX_CONNECTIONS;
  server->max_rate = DEFAULT_MAX_RATE;
  server->admin_hosts_allow = g_strdup (DEFAULT_ADMIN_HOSTS_ALLOW);
  server->admin_arl =
      gss_addr_range_list_new_from_string (server->admin_hosts_allow, TRUE,
      TRUE);
  server->kiosk_hosts_allow = g_strdup (DEFAULT_KIOSK_HOSTS_ALLOW);
  server->kiosk_arl =
      gss_addr_range_list_new_from_string (server->kiosk_hosts_allow, FALSE,
      FALSE);
  server->admin_token = g_strdup (DEFAULT_ADMIN_TOKEN);
  server->realm = g_strdup (DEFAULT_REALM);
  server->enable_html5_video = DEFAULT_ENABLE_HTML5_VIDEO;
  server->enable_cortado = DEFAULT_ENABLE_CORTADO;
  server->enable_flash = DEFAULT_ENABLE_FLASH;
  server->enable_rtsp = DEFAULT_ENABLE_RTSP;
  server->enable_rtmp = DEFAULT_ENABLE_RTMP;
  server->enable_vod = DEFAULT_ENABLE_VOD;

  server->enable_flowplayer = FALSE;
  server->enable_persona = FALSE;
  server->enable_programs = TRUE;
  server->programs = NULL;
  server->archive_dir = g_strdup (DEFAULT_ARCHIVE_DIR);
  server->cas_server = g_strdup (DEFAULT_CAS_SERVER);

#ifdef ENABLE_RTSP
  if (server->enable_rtsp)
    gss_server_rtsp_init (server);
#endif

  gss_server_add_module (server, GSS_MODULE (server));
  gss_server_setup_resources (server);

  g_timeout_add (1000, (GSourceFunc) periodic_timer, server);
}

static void
gss_server_finalize (GObject * object)
{
  GssServer *server = GSS_SERVER (object);
  int i;

  g_list_free_full (server->programs, g_object_unref);

  if (server->server)
    g_object_unref (server->server);
  if (server->ssl_server)
    g_object_unref (server->ssl_server);

  g_list_free (server->featured_resources);
  server->modules = g_list_remove (server->modules, server);
  g_list_free_full (server->modules, g_object_unref);

  g_hash_table_unref (server->resources);
  for (i = 0; i < server->n_prefix_resources; i++) {
    gss_resource_free (server->prefix_resources[i]);
  }
  g_free (server->prefix_resources);
  gss_metrics_free (server->metrics);
  g_free (server->base_url);
  g_free (server->base_url_https);
  g_free (server->server_hostname);
  g_free (server->realm);
  g_free (server->admin_hosts_allow);
  gss_addr_range_list_free (server->admin_arl);
  g_free (server->kiosk_hosts_allow);
  gss_addr_range_list_free (server->kiosk_arl);
  g_free (server->admin_token);
  g_free (server->archive_dir);
  g_free (server->cas_server);
  g_object_unref (server->client_session);

  parent_class->finalize (object);
}

static void
gss_server_class_init (GssServerClass * server_class)
{
  soup_method_source = g_intern_static_string ("SOURCE");

  G_OBJECT_CLASS (server_class)->set_property = gss_server_set_property;
  G_OBJECT_CLASS (server_class)->get_property = gss_server_get_property;
  G_OBJECT_CLASS (server_class)->finalize = gss_server_finalize;

  GSS_OBJECT_CLASS (server_class)->attach = gss_server_attach;

  g_object_class_install_property (G_OBJECT_CLASS (server_class),
      PROP_ENABLE_PUBLIC_INTERFACE,
      g_param_spec_boolean ("enable-public-interface",
          "Enable Public Interface", "Enable Public Interface",
          DEFAULT_ENABLE_PUBLIC_INTERFACE,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property (G_OBJECT_CLASS (server_class),
      PROP_HTTP_PORT, g_param_spec_int ("http-port", "HTTP Port", "HTTP Port",
          0, 65535, DEFAULT_HTTP_PORT,
          (GParamFlags) (G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE |
              G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property (G_OBJECT_CLASS (server_class),
      PROP_HTTPS_PORT, g_param_spec_int ("https-port", "HTTPS Port",
          "HTTPS Port", 0, 65535, DEFAULT_HTTPS_PORT,
          (GParamFlags) (G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE |
              G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property (G_OBJECT_CLASS (server_class),
      PROP_SERVER_HOSTNAME, g_param_spec_string ("server-hostname",
          "Server Hostname", "Server Hostname", DEFAULT_SERVER_HOSTNAME,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
#if 0
  g_object_class_install_property (G_OBJECT_CLASS (server_class), PROP_TITLE,
      g_param_spec_string ("title", "Title", "Title", DEFAULT_TITLE,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
#endif
  g_object_class_install_property (G_OBJECT_CLASS (server_class),
      PROP_MAX_CONNECTIONS, g_param_spec_int ("max-connections",
          "Maximum number of connections", "Maximum number of connections", 0,
          G_MAXINT, DEFAULT_MAX_CONNECTIONS,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property (G_OBJECT_CLASS (server_class),
      PROP_MAX_RATE, g_param_spec_int ("max-rate",
          "Maximum bitrate (in kbytes/sec, 0 is unlimited)",
          "Maximum bitrate (in kbytes/sec)", 0, G_MAXINT, DEFAULT_MAX_RATE,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property (G_OBJECT_CLASS (server_class),
      PROP_ADMIN_HOSTS_ALLOW, g_param_spec_string ("admin-hosts-allow",
          "Allowed Hosts (admin)", "Allowed Hosts (admin)",
          DEFAULT_ADMIN_HOSTS_ALLOW,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property (G_OBJECT_CLASS (server_class),
      PROP_KIOSK_HOSTS_ALLOW, g_param_spec_string ("kiosk-hosts-allow",
          "Allowed Hosts (kiosk)", "IP addresses or address ranges listed "
          "here will automatically be given access to the kiosk group.",
          DEFAULT_KIOSK_HOSTS_ALLOW,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
#if 0
  /* Don't want to expose this yet */
  g_object_class_install_property (G_OBJECT_CLASS (server_class),
      PROP_REALM, g_param_spec_string ("realm",
          "Realm", "Realm",
          DEFAULT_REALM,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
              GSS_PARAM_HIDE)));
#endif
  g_object_class_install_property (G_OBJECT_CLASS (server_class),
      PROP_ADMIN_TOKEN, g_param_spec_string ("admin-token",
          "Admin Token", "Admin Token",
          DEFAULT_ADMIN_TOKEN,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
              GSS_PARAM_HIDE)));
  g_object_class_install_property (G_OBJECT_CLASS (server_class),
      PROP_ARCHIVE_DIR, g_param_spec_string ("archive-dir", "Archive Directory",
          "Archive Directory", DEFAULT_ARCHIVE_DIR,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property (G_OBJECT_CLASS (server_class),
      PROP_ENABLE_HTML5_VIDEO, g_param_spec_boolean ("enable-html5-video",
          "Enable HTML5 Video", "Enable HTML5 Video",
          DEFAULT_ENABLE_HTML5_VIDEO,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property (G_OBJECT_CLASS (server_class),
      PROP_ENABLE_CORTADO, g_param_spec_boolean ("enable-cortado",
          "Enable Cortado Java Applet", "Enable Cortado Java Applet",
          DEFAULT_ENABLE_CORTADO,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property (G_OBJECT_CLASS (server_class),
      PROP_ENABLE_FLASH, g_param_spec_boolean ("enable-flash", "Enable Flash",
          "Enable Flash", DEFAULT_ENABLE_FLASH,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
#ifdef ENABLE_RTSP
#define RTSP_FLAGS G_PARAM_READWRITE
#else
#define RTSP_FLAGS G_PARAM_READABLE
#endif
  g_object_class_install_property (G_OBJECT_CLASS (server_class),
      PROP_ENABLE_RTSP,
      g_param_spec_boolean ("enable-rtsp", "Enable RTSP",
          "Enable RTSP", DEFAULT_ENABLE_RTSP,
          (GParamFlags) (RTSP_FLAGS | G_PARAM_STATIC_STRINGS)));
#ifdef ENABLE_RTMP
#define RTMP_FLAGS G_PARAM_READWRITE
#else
#define RTMP_FLAGS G_PARAM_READABLE
#endif
  g_object_class_install_property (G_OBJECT_CLASS (server_class),
      PROP_ENABLE_RTMP,
      g_param_spec_boolean ("enable-rtmp", "Enable RTMP",
          "Enable RTMP", DEFAULT_ENABLE_RTMP,
          (GParamFlags) (RTMP_FLAGS | G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property (G_OBJECT_CLASS (server_class),
      PROP_ENABLE_VOD,
      g_param_spec_boolean ("enable-vod", "Enable VOD",
          "Enable VOD", DEFAULT_ENABLE_VOD,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
#ifdef ENABLE_CAS
  g_object_class_install_property (G_OBJECT_CLASS (server_class),
      PROP_CAS_SERVER, g_param_spec_string ("cas-server", "CAS Server",
          "CAS Server used for authentication (Example: https://cas.example.com/cas",
          DEFAULT_CAS_SERVER,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
#endif

  parent_class = g_type_class_peek_parent (server_class);
}


static void
gss_server_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GssServer *server;

  server = GSS_SERVER (object);

  switch (prop_id) {
    case PROP_ENABLE_PUBLIC_INTERFACE:
      server->enable_public_interface = g_value_get_boolean (value);
      break;
    case PROP_HTTP_PORT:
      gss_server_set_http_port (server, g_value_get_int (value));
      break;
    case PROP_HTTPS_PORT:
      gss_server_set_https_port (server, g_value_get_int (value));
      break;
    case PROP_SERVER_HOSTNAME:
      gss_server_set_server_hostname (server, g_value_get_string (value));
      break;
    case PROP_MAX_CONNECTIONS:
      server->max_connections = g_value_get_int (value);
      break;
    case PROP_MAX_RATE:
      server->max_rate = g_value_get_int (value);
      break;
    case PROP_ADMIN_HOSTS_ALLOW:
      if (strcmp (server->admin_hosts_allow, g_value_get_string (value))) {
        g_free (server->admin_hosts_allow);
        server->admin_hosts_allow = g_value_dup_string (value);
        gss_addr_range_list_free (server->admin_arl);
        server->admin_arl =
            gss_addr_range_list_new_from_string (server->admin_hosts_allow,
            TRUE, TRUE);
      }
      break;
    case PROP_KIOSK_HOSTS_ALLOW:
      if (strcmp (server->kiosk_hosts_allow, g_value_get_string (value))) {
        g_free (server->kiosk_hosts_allow);
        server->kiosk_hosts_allow = g_value_dup_string (value);
        gss_addr_range_list_free (server->kiosk_arl);
        server->kiosk_arl =
            gss_addr_range_list_new_from_string (server->kiosk_hosts_allow,
            FALSE, FALSE);
      }
      break;
    case PROP_ADMIN_TOKEN:
      g_free (server->admin_token);
      server->admin_token = g_value_dup_string (value);
      break;
    case PROP_REALM:
      g_free (server->realm);
      server->realm = g_value_dup_string (value);
      break;
    case PROP_ARCHIVE_DIR:
      g_free (server->archive_dir);
      server->archive_dir = g_value_dup_string (value);
      break;
    case PROP_ENABLE_HTML5_VIDEO:
      server->enable_html5_video = g_value_get_boolean (value);
      break;
    case PROP_ENABLE_CORTADO:
      server->enable_cortado = g_value_get_boolean (value);
      break;
    case PROP_ENABLE_FLASH:
      server->enable_flash = g_value_get_boolean (value);
      break;
    case PROP_ENABLE_RTSP:
      server->enable_rtsp = g_value_get_boolean (value);
      break;
    case PROP_ENABLE_RTMP:
      server->enable_rtmp = g_value_get_boolean (value);
      break;
    case PROP_ENABLE_VOD:
      server->enable_vod = g_value_get_boolean (value);
      break;
    case PROP_CAS_SERVER:
      g_free (server->cas_server);
      server->cas_server = g_value_dup_string (value);
      break;
    default:
      g_assert_not_reached ();
      break;
  }
}

static void
gss_server_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GssServer *server;

  server = GSS_SERVER (object);

  switch (prop_id) {
    case PROP_ENABLE_PUBLIC_INTERFACE:
      g_value_set_boolean (value, server->enable_public_interface);
      break;
    case PROP_HTTP_PORT:
      g_value_set_int (value, server->http_port);
      break;
    case PROP_HTTPS_PORT:
      g_value_set_int (value, server->https_port);
      break;
    case PROP_SERVER_HOSTNAME:
      g_value_set_string (value, server->server_hostname);
      break;
    case PROP_MAX_CONNECTIONS:
      g_value_set_int (value, server->max_connections);
      break;
    case PROP_MAX_RATE:
      g_value_set_int (value, server->max_rate);
      break;
    case PROP_ADMIN_HOSTS_ALLOW:
      g_value_set_string (value, server->admin_hosts_allow);
      break;
    case PROP_KIOSK_HOSTS_ALLOW:
      g_value_set_string (value, server->kiosk_hosts_allow);
      break;
    case PROP_ADMIN_TOKEN:
      g_value_set_string (value, server->admin_token);
      break;
    case PROP_REALM:
      g_value_set_string (value, server->realm);
      break;
    case PROP_ARCHIVE_DIR:
      g_value_set_string (value, server->archive_dir);
      break;
    case PROP_ENABLE_HTML5_VIDEO:
      g_value_set_boolean (value, server->enable_html5_video);
      break;
    case PROP_ENABLE_CORTADO:
      g_value_set_boolean (value, server->enable_cortado);
      break;
    case PROP_ENABLE_FLASH:
      g_value_set_boolean (value, server->enable_flash);
      break;
    case PROP_ENABLE_RTSP:
      g_value_set_boolean (value, server->enable_rtsp);
      break;
    case PROP_ENABLE_RTMP:
      g_value_set_boolean (value, server->enable_rtmp);
      break;
    case PROP_ENABLE_VOD:
      g_value_set_boolean (value, server->enable_vod);
      break;
    case PROP_CAS_SERVER:
      g_value_set_string (value, server->cas_server);
      break;
    default:
      g_assert_not_reached ();
      break;
  }
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
gss_server_attach (GssObject * object, GssServer * x_server)
{
  GssServer *server = GSS_SERVER (object);
  GssResource *r;

  r = gss_server_add_resource (GSS_OBJECT_SERVER (object), "/admin/server",
      GSS_RESOURCE_ADMIN, GSS_TEXT_HTML, gss_server_get_resource, NULL,
      gss_server_post_resource, server);
  r->name = g_strdup ("Server");
  gss_module_set_admin_resource (GSS_MODULE (server), r);

}

void
gss_server_set_footer_html (GssServer * server, GssFooterHtml footer_html,
    gpointer priv)
{
  server->footer_html = footer_html;
  server->footer_html_priv = priv;
}

void
gss_server_set_title (GssServer * server, const char *title)
{
  gss_object_set_title (GSS_OBJECT (server), title);
}

void
gss_server_set_realm (GssServer * server, const char *realm)
{
  g_free (server->realm);
  server->realm = g_strdup (realm);
}

GssServer *
gss_server_new (void)
{
  GssServer *server;

  server = g_object_new (GSS_TYPE_SERVER, NULL);

  return server;
}

void
gss_server_disable_programs (GssServer * server)
{
  server->enable_programs = FALSE;
}

void
gss_server_add_warnings_callback (GssServer * server,
    void (*add_warnings_func) (GssTransaction * t, void *priv), void *priv)
{
  server->add_warnings = add_warnings_func;
  server->add_warnings_priv = priv;
}

GssResource *
gss_server_add_resource (GssServer * server, const char *location,
    GssResourceFlags flags, const char *content_type,
    GssTransactionCallback get_callback,
    GssTransactionCallback put_callback, GssTransactionCallback post_callback,
    gpointer priv)
{
  GssResource *resource;

  g_return_val_if_fail (content_type == NULL ||
      strcmp (content_type, "text/html") != 0, NULL);
  g_return_val_if_fail (content_type == NULL ||
      strcmp (content_type, "text/plain") != 0, NULL);

  resource = g_new0 (GssResource, 1);
  resource->location = g_strdup (location);
  resource->flags = flags;
  resource->content_type = content_type;
  resource->get_callback = get_callback;
  resource->put_callback = put_callback;
  resource->post_callback = post_callback;
  resource->priv = priv;

  if (flags & GSS_RESOURCE_PREFIX) {
    server->n_prefix_resources++;
    server->prefix_resources = g_realloc (server->prefix_resources,
        sizeof (GssResource *) * server->n_prefix_resources);
    server->prefix_resources[server->n_prefix_resources - 1] = resource;
  } else {
    g_hash_table_replace (server->resources, resource->location, resource);
  }

  return resource;
}

void
gss_server_remove_resource (GssServer * server, const char *location)
{
  g_hash_table_remove (server->resources, location);
}

void
gss_server_remove_resources_by_priv (GssServer * server, void *priv)
{
  GHashTableIter iter;
  const char *key;
  GssResource *resource;

  g_hash_table_iter_init (&iter, server->resources);
  while (g_hash_table_iter_next (&iter, (gpointer *) & key,
          (gpointer *) & resource)) {
    if (resource->priv == priv) {
      g_hash_table_iter_remove (&iter);
    }
  }
}

static void
gss_server_setup_resources (GssServer * server)
{
  gss_session_add_session_callbacks (server);

  gss_server_add_resource (server, "/", GSS_RESOURCE_UI, GSS_TEXT_HTML,
      gss_server_resource_main_page, NULL, NULL, NULL);
  gss_server_add_resource (server, "/list", GSS_RESOURCE_UI, GSS_TEXT_PLAIN,
      gss_server_resource_list, NULL, NULL, NULL);
  gss_server_add_string_resource (server, "/check", 0, GSS_TEXT_HTML,
      "<!DOCTYPE html><html><title>Status</title>"
      "<body>status: ok</body></html>\n");

  gss_server_add_resource (server, "/about", GSS_RESOURCE_UI, GSS_TEXT_HTML,
      gss_server_resource_about, NULL, NULL, NULL);
  gss_server_add_resource (server, "/contact", GSS_RESOURCE_UI, GSS_TEXT_HTML,
      gss_resource_unimplemented, NULL, NULL, NULL);
  gss_server_add_resource (server, "/add_program", GSS_RESOURCE_UI,
      GSS_TEXT_HTML, gss_resource_unimplemented, NULL, NULL, NULL);
  gss_server_add_resource (server, "/dashboard", GSS_RESOURCE_UI, GSS_TEXT_HTML,
      gss_resource_unimplemented, NULL, NULL, NULL);
  gss_server_add_resource (server, "/monitor", GSS_RESOURCE_UI, GSS_TEXT_HTML,
      gss_resource_unimplemented, NULL, NULL, NULL);
  gss_server_add_resource (server, "/meep", GSS_RESOURCE_UI, GSS_TEXT_HTML,
      gss_resource_unimplemented, NULL, NULL, NULL);

  if (server->enable_cortado) {
    gss_server_add_file_resource (server, "/cortado.jar", 0,
        "application/java-archive");
  }

  if (server->enable_osplayer) {
    gss_server_add_file_resource (server, "/OSplayer.swf", 0,
        "application/x-shockwave-flash");
    gss_server_add_file_resource (server, "/AC_RunActiveContent.js", 0,
        "application/javascript");
  }

  if (server->enable_flowplayer) {
    gss_server_add_file_resource (server, "/flowplayer-3.2.15.swf", 0,
        "application/x-shockwave-flash");
    gss_server_add_file_resource (server, "/flowplayer.controls-3.2.14.swf", 0,
        "application/x-shockwave-flash");
    gss_server_add_file_resource (server, "/flowplayer-3.2.11.min.js", 0,
        "application/javascript");
  }

  gss_server_add_static_resource (server, "/images/footer-entropywave.png",
      0, "image/png",
      gss_data_footer_entropywave_png, gss_data_footer_entropywave_png_len);

  gss_server_add_string_resource (server, "/robots.txt", 0,
      GSS_TEXT_PLAIN, "User-agent: *\nDisallow: /\n");

  gss_server_add_static_resource (server, "/dash.min.js", 0,
      "text/javascript", gss_data_dash_min_js, gss_data_dash_min_js_len);
  gss_server_add_static_resource (server, "/include.js", 0,
      "text/javascript", gss_data_include_js, gss_data_include_js_len);
  gss_server_add_static_resource (server,
      "/bootstrap/css/bootstrap-responsive.css", 0, "text/css",
      gss_data_bootstrap_responsive_css, gss_data_bootstrap_responsive_css_len);
  gss_server_add_static_resource (server,
      "/bootstrap/css/bootstrap.css", 0, "text/css",
      gss_data_bootstrap_css, gss_data_bootstrap_css_len);
  gss_server_add_static_resource (server,
      "/bootstrap/js/bootstrap.js", 0, "text/javascript",
      gss_data_bootstrap_js, gss_data_bootstrap_js_len);
  gss_server_add_static_resource (server,
      "/bootstrap/js/jquery.js", 0, "text/javascript",
      gss_data_jquery_js, gss_data_jquery_js_len);
  gss_server_add_static_resource (server,
      "/bootstrap/img/glyphicons-halflings.png", 0, "image/png",
      gss_data_glyphicons_halflings_png, gss_data_glyphicons_halflings_png_len);
  gss_server_add_static_resource (server,
      "/bootstrap/img/glyphicons-halflings-white.png", 0, "image/png",
      gss_data_glyphicons_halflings_white_png,
      gss_data_glyphicons_halflings_white_png_len);
  gss_server_add_static_resource (server,
      "/no-snapshot.jpg", 0, "image/jpeg",
      gss_data_no_snapshot_jpg, gss_data_no_snapshot_jpg_len);
  gss_server_add_static_resource (server,
      "/offline.jpg", 0, "image/jpeg",
      gss_data_offline_jpg, gss_data_offline_jpg_len);
  gss_server_add_static_resource (server,
      "/sign_in_blue.png", 0, "image/png",
      gss_data_sign_in_blue_png, gss_data_sign_in_blue_png_len);

  gss_server_add_resource (server, "/assets/", GSS_RESOURCE_PREFIX,
      NULL, gss_asset_get_resource, NULL, NULL, NULL);

  gss_playready_setup (server);
}

void
gss_server_add_resource_simple (GssServer * server, GssResource * r)
{
  g_hash_table_replace (server->resources, r->location, r);
}

void
gss_server_add_file_resource (GssServer * server,
    const char *filename, GssResourceFlags flags, const char *content_type)
{
  GssResource *r;

  r = gss_resource_new_file (filename, flags, content_type);
  if (r == NULL)
    return;
  gss_server_add_resource_simple (server, r);
}

void
gss_server_add_static_resource (GssServer * server, const char *filename,
    GssResourceFlags flags, const char *content_type, const char *string,
    int len)
{
  GssResource *r;

  r = gss_resource_new_static (filename, flags, content_type, string, len);
  gss_server_add_resource_simple (server, r);
}

void
gss_server_add_string_resource (GssServer * server, const char *filename,
    GssResourceFlags flags, const char *content_type, const char *string)
{
  GssResource *r;

  r = gss_resource_new_string (filename, flags, content_type, string);
  gss_server_add_resource_simple (server, r);
}

void
gss_server_set_server_hostname (GssServer * server, const char *hostname)
{
  g_free (server->server_hostname);
  server->server_hostname = g_strdup (hostname);

  g_free (server->base_url);
  g_free (server->base_url_https);
  if (server->server_hostname[0]) {
    if (server->http_port == 80) {
      server->base_url = g_strdup_printf ("http://%s", server->server_hostname);
    } else {
      server->base_url =
          g_strdup_printf ("http://%s:%d", server->server_hostname,
          server->http_port);
    }
    if (server->https_port == 443) {
      server->base_url_https = g_strdup_printf ("https://%s",
          server->server_hostname);
    } else {
      server->base_url_https = g_strdup_printf ("https://%s:%d",
          server->server_hostname, server->https_port);
    }
  } else {
    server->base_url = g_strdup ("");
    server->base_url_https = g_strdup ("");
  }
}

void
gss_server_follow_all (GssProgram * program, const char *host)
{

}

void
gss_server_add_program_simple (GssServer * server, GssProgram * program)
{
  GssProgramClass *program_class;

  server->programs = g_list_append (server->programs, program);
  GSS_OBJECT_SERVER (program) = server;

  program_class = GSS_PROGRAM_GET_CLASS (program);
  if (program_class->add_resources) {
    program_class->add_resources (program);
  }
}

GssProgram *
gss_server_add_program (GssServer * server, const char *program_name)
{
  GssProgram *program;
  program = gss_program_new (program_name);
  gss_server_add_program_simple (server, program);
  return program;
}

void
gss_server_remove_program (GssServer * server, GssProgram * program)
{
  g_return_if_fail (GSS_IS_SERVER (server));
  g_return_if_fail (GSS_IS_PROGRAM (program));

  gss_server_remove_resources_by_priv (server, program);
  server->programs = g_list_remove (server->programs, program);
  GSS_OBJECT_SERVER (program) = NULL;
}

void
gss_server_add_module (GssServer * server, GssModule * module)
{
  GssObjectClass *object_class = GSS_OBJECT_GET_CLASS (module);

  g_return_if_fail (GSS_IS_SERVER (server));
  g_return_if_fail (GSS_IS_MODULE (module));

  server->modules = g_list_append (server->modules, module);
  GSS_OBJECT_SERVER (module) = server;

  if (object_class->attach) {
    object_class->attach (GSS_OBJECT (module), server);
  }
}

void
gss_server_add_featured_resource (GssServer * server, GssResource * resource,
    const char *name)
{
  g_return_if_fail (GSS_IS_SERVER (server));
  g_return_if_fail (resource != NULL);
  g_return_if_fail (name != NULL);
  g_return_if_fail (name[0] != 0);

  resource->name = g_strdup (name);
  server->featured_resources =
      g_list_append (server->featured_resources, resource);
}

GssProgram *
gss_server_get_program_by_name (GssServer * server, const char *name)
{
  GList *g;

  g_return_val_if_fail (GSS_IS_SERVER (server), NULL);
  g_return_val_if_fail (name != NULL, NULL);

  for (g = server->programs; g; g = g_list_next (g)) {
    GssProgram *program = g->data;
    if (strcmp (GSS_OBJECT_NAME (program), name) == 0) {
      return program;
    }
  }
  return NULL;
}

const char *
gss_server_get_multifdsink_string (void)
{
  return "multifdsink "
      "sync=false " "time-min=200000000 " "recover-policy=keyframe "
#if GST_CHECK_VERSION(1,0,0)
      "unit-format=time " "burst-format=time "
#else
      "unit-type=2 " "burst-unit=2 "
#endif
      "units-max=20000000000 "
      "units-soft-max=11000000000 "
      "sync-method=burst-keyframe " "burst-value=3000000000";
}

static GssResource *
gss_server_lookup_resource (GssServer * server, const char *path)
{
  int i;

  for (i = 0; i < server->n_prefix_resources; i++) {
    if (g_str_has_prefix (path, server->prefix_resources[i]->location)) {
      return server->prefix_resources[i];
    }
  }

  return g_hash_table_lookup (server->resources, path);
}

static void
gss_server_resource_callback (SoupServer * soupserver, SoupMessage * msg,
    const char *path, GHashTable * query, SoupClientContext * client,
    gpointer user_data)
{
  GssServer *server = (GssServer *) user_data;
  GssTransaction *t;
  GssSession *session;

  t = gss_transaction_new (server, soupserver, msg, path, query, client);

  t->resource = gss_server_lookup_resource (server, path);

  if (!t->resource) {
    gss_transaction_error_not_found (t, "resource not found");
    return;
  }

  if (t->resource->flags & GSS_RESOURCE_UI) {
    if (!server->enable_public_interface && soupserver == server->server) {
      gss_transaction_error_not_found (t, "public interface disabled");
      return;
    }
  }

  if (t->resource->flags & GSS_RESOURCE_HTTPS_ONLY) {
    if (soupserver != server->ssl_server) {
      gss_transaction_error_not_found (t, "resource https only");
      return;
    }
  }

  session = gss_session_get_session (query);
  if (session && soupserver != server->ssl_server) {
    gss_session_invalidate (session);
    session = NULL;
  }

  if (t->resource->flags & GSS_RESOURCE_USER) {
    if (session == NULL) {
      gss_transaction_error_not_found (t, "resource requires login");
      return;
    }
  }
  t->session = session;

  if (t->resource->flags & GSS_RESOURCE_ADMIN) {
    if (session == NULL || !session->is_admin ||
        !gss_addr_range_list_check_address (server->admin_arl,
            soup_client_context_get_address (client))) {
      gss_html_error_401 (server, msg);
      return;
    }
  }

  if (t->resource->content_type) {
    soup_message_headers_replace (msg->response_headers, "Content-Type",
        t->resource->content_type);
  }

  if (t->resource->etag) {
    const char *inm;

    soup_message_headers_append (msg->response_headers, "Cache-Control",
        "max-age=86400");
    inm = soup_message_headers_get_one (msg->request_headers, "If-None-Match");
    if (inm && !strcmp (inm, t->resource->etag)) {
      soup_message_set_status (msg, SOUP_STATUS_NOT_MODIFIED);
      return;
    }
  } else {
    soup_message_headers_append (msg->response_headers, "Cache-Control",
        "must-revalidate");
  }


  if (t->resource->flags & GSS_RESOURCE_HTTP_ONLY) {
    if (soupserver != server->server) {
      gss_resource_onetime_redirect (t);
      return;
    }
  }

  soup_message_set_status (msg, SOUP_STATUS_OK);

  if ((msg->method == SOUP_METHOD_GET || msg->method == SOUP_METHOD_HEAD)
      && t->resource->get_callback) {
    t->resource->get_callback (t);
  } else if (msg->method == SOUP_METHOD_PUT && t->resource->put_callback) {
    t->resource->put_callback (t);
  } else if (msg->method == SOUP_METHOD_POST && t->resource->post_callback) {
    t->resource->post_callback (t);
  } else if (msg->method == SOUP_METHOD_SOURCE && t->resource->put_callback) {
    t->resource->put_callback (t);
  } else if (msg->method == SOUP_METHOD_OPTIONS) {
    soup_message_headers_replace (msg->response_headers,
        "Access-Control-Allow-Origin", "*");
    soup_message_headers_replace (msg->response_headers,
        "Access-Control-Allow-Headers", "origin,range");
    soup_message_headers_replace (msg->response_headers,
        "Access-Control-Expose-Headers", "Server,range");
    soup_message_headers_replace (msg->response_headers,
        "Access-Control-Allow-Methods", "GET, HEAD, OPTIONS");
  } else {
    gss_html_error_405 (server, msg);
  }

  if (t->s) {
    int len;
    gchar *content;

    len = t->s->len;
    content = g_string_free (t->s, FALSE);
    soup_message_body_append (msg->response_body, SOUP_MEMORY_TAKE,
        content, len);
  }
}

static void
gss_server_resource_main_page (GssTransaction * t)
{
  GString *s;
  GList *g;

  s = t->s = g_string_new ("");

  gss_html_header (t);

  GSS_P ("<ul class='thumbnails'>\n");
  for (g = t->server->programs; g; g = g_list_next (g)) {
    GssProgram *program = g->data;

    if (program->is_archive)
      continue;

    GSS_P ("<li class='span4'>\n");
    GSS_P ("<div class='thumbnail'>\n");
    GSS_P ("<a href=\"/%s%s%s\">",
        GSS_OBJECT_NAME (program),
        t->session ? "?session_id=" : "",
        t->session ? t->session->session_id : "");
    gss_program_add_jpeg_block (program, t);
    GSS_P ("</a>\n");
    GSS_P ("<h5>%s</h5>\n", GSS_OBJECT_SAFE_TITLE (program));
    GSS_P ("</div>\n");
    GSS_P ("</li>\n");
  }
  GSS_P ("</ul>\n");

  if (t->server->enable_vod) {
    GSS_P ("<ul class='thumbnails'>\n");
    for (g = t->server->programs; g; g = g_list_next (g)) {
      GssProgram *program = g->data;

      if (!program->is_archive)
        continue;

      GSS_P ("<li class='span4'>\n");
      GSS_P ("<div class='thumbnail'>\n");
      GSS_P ("<a href=\"/%s%s%s\">",
          GSS_OBJECT_NAME (program),
          t->session ? "?session_id=" : "",
          t->session ? t->session->session_id : "");
      gss_program_add_jpeg_block (program, t);
      GSS_P ("</a>\n");
      GSS_P ("<h5>%s</h5>\n", GSS_OBJECT_SAFE_TITLE (program));
      GSS_P ("</div>\n");
      GSS_P ("</li>\n");
    }
    GSS_P ("</ul>\n");
  }

  gss_html_footer (t);
}

static void
gss_server_resource_list (GssTransaction * t)
{
  GString *s;
  GList *g;

  s = t->s = g_string_new ("");

  for (g = t->server->programs; g; g = g_list_next (g)) {
    GssProgram *program = g->data;
    GSS_P ("%s\n", GSS_OBJECT_NAME (program));
  }
}


static gboolean
periodic_timer (gpointer data)
{
  GssServer *server = (GssServer *) data;
  GList *g;

  for (g = server->programs; g; g = g_list_next (g)) {
    GssProgram *program = g->data;

    if (program->restart_delay) {
      program->restart_delay--;
      if (program->restart_delay == 0) {
        gss_program_start (program);
      }
    }

  }

  return TRUE;
}

static void
gss_server_resource_about (GssTransaction * t)
{
  GString *s;

  s = t->s = g_string_new ("");

  gss_html_header (t);

  GSS_P ("<h2>About</h2>\n");

  GSS_P ("<p>GStreamer Streaming Server is based on the work of many\n");
  GSS_P ("open source projects, some of which are listed below.\n");
  GSS_P ("</p>\n");

  GSS_P ("<h3>GStreamer Streaming Server</h3>\n");
  GSS_P ("<pre>\n");
  GSS_P ("Copyright (C) 2009-2012 Entropy Wave Inc <info@entropywave.com>\n");
  GSS_P ("Copyright (C) 2009-2012 David Schleef <ds@schleef.org>\n");
  GSS_P ("Copyright (C) 2012 Jan Schmidt <thaytan@noraisin.net>\n");
  GSS_P ("\n");
  GSS_P ("This library is free software; you can redistribute it and/or\n");
  GSS_P ("modify it under the terms of the GNU Library General Public\n");
  GSS_P ("License as published by the Free Software Foundation; either\n");
  GSS_P ("version 2 of the License, or (at your option) any later version.\n");
  GSS_P ("\n");
  GSS_P ("This library is distributed in the hope that it will be useful,\n");
  GSS_P ("but WITHOUT ANY WARRANTY; without even the implied warranty of\n");
  GSS_P ("MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU\n");
  GSS_P ("Library General Public License for more details.\n");
  GSS_P ("\n");
  GSS_P ("You should have received a copy of the GNU Library General Public\n");
  GSS_P ("License along with this library; if not, write to the\n");
  GSS_P ("Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,\n");
  GSS_P ("Boston, MA 02110-1301, USA.\n");
  GSS_P ("</pre>\n");

  GSS_P ("<h3>Twitter Bootstrap</h3>\n");
  GSS_P ("<pre>\n");
  GSS_A ("Copyright 2012 Twitter, Inc\n");
  GSS_A ("Licensed under the Apache License v2.0\n");
  GSS_A ("http://www.apache.org/licenses/LICENSE-2.0\n");
  GSS_A ("\n");
  GSS_A
      ("Designed and built with all the love in the world @twitter by @mdo and @fat.\n");
  GSS_P ("</pre>\n");

  GSS_P ("<h3>GStreamer</h3>\n");
  GSS_P ("<p>Includes gstreamer, gst-plugins-base, gst-plugins-good, \n");
  GSS_P ("gst-plugins-bad, and gst-rtsp-server.</p>\n");
  GSS_P ("<pre>\n");
  GSS_P ("Copyright (C) 1999-2012 GStreamer contributors\n");
  GSS_P ("\n");
  GSS_P ("This library is free software; you can redistribute it and/or\n");
  GSS_P ("modify it under the terms of the GNU Library General Public\n");
  GSS_P ("License as published by the Free Software Foundation; either\n");
  GSS_P ("version 2 of the License, or (at your option) any later version.\n");
  GSS_P ("\n");
  GSS_P ("This library is distributed in the hope that it will be useful,\n");
  GSS_P ("but WITHOUT ANY WARRANTY; without even the implied warranty of\n");
  GSS_P ("MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU\n");
  GSS_P ("Library General Public License for more details.\n");
  GSS_P ("\n");
  GSS_P ("You should have received a copy of the GNU Library General Public\n");
  GSS_P ("License along with this library; if not, write to the\n");
  GSS_P ("Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,\n");
  GSS_P ("Boston, MA 02110-1301, USA.\n");
  GSS_P ("</pre>\n");

  GSS_P ("<h3>Libsoup</h3>\n");
  GSS_P ("<pre>\n");
  GSS_P ("Copyright (C) 2009, 2010 Red Hat, Inc.\n");
  GSS_A ("Copyright 1999-2003 Ximian, Inc.\n");
  GSS_A ("Copyright (C) 2000-2003, Ximian, Inc.\n");
  GSS_A ("Copyright (C) 2001-2007 Novell, Inc.\n");
  GSS_A ("Copyright (C) 2007, 2008, 2009, 2010, 2011, 2012 Red Hat, Inc.\n");
  GSS_A ("Copyright (C) 2008 Diego Escalante Urrelo\n");
  GSS_A ("Copyright (C) 2009, 2011 Collabora Ltd.\n");
  GSS_A ("Copyright (C) 2009 Gustavo Noronha Silva.\n");
  GSS_A ("Copyright (C) 2009, 2010 Igalia S.L.\n");
  GSS_P ("\n");
  GSS_P ("This library is free software; you can redistribute it and/or\n");
  GSS_P ("modify it under the terms of the GNU Library General Public\n");
  GSS_P ("License as published by the Free Software Foundation; either\n");
  GSS_P ("version 2 of the License, or (at your option) any later version.\n");
  GSS_P ("\n");
  GSS_P ("This library is distributed in the hope that it will be useful,\n");
  GSS_P ("but WITHOUT ANY WARRANTY; without even the implied warranty of\n");
  GSS_P ("MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU\n");
  GSS_P ("Library General Public License for more details.\n");
  GSS_P ("\n");
  GSS_P ("You should have received a copy of the GNU Library General Public\n");
  GSS_P ("License along with this library; if not, write to the\n");
  GSS_P ("Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,\n");
  GSS_P ("Boston, MA 02110-1301, USA.\n");
  GSS_P ("</pre>\n");

  GSS_P ("<h3>JSON-Glib</h3>\n");
  GSS_P ("<pre>\n");
  GSS_P ("Copyright (C) 1997, 1998 Tim Janik\n");
  GSS_P ("Copyright (C) 2007, 2008, 2009  OpenedHand Ltd.\n");
  GSS_P ("Copyright (C) 2009, 2010, 2011  Intel Corp.\n");
  GSS_P ("Copyright (C) 2010  Luca Bruno <lethalman88@gmail.com>\n");
  GSS_P ("\n");
  GSS_P ("This library is free software; you can redistribute it and/or\n");
  GSS_P ("modify it under the terms of the GNU Lesser General Public\n");
  GSS_P ("License as published by the Free Software Foundation; either\n");
  GSS_P
      ("version 2.1 of the License, or (at your option) any later version.\n");
  GSS_P ("\n");
  GSS_P ("This library is distributed in the hope that it will be useful,\n");
  GSS_P ("but WITHOUT ANY WARRANTY; without even the implied warranty of\n");
  GSS_P ("MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU\n");
  GSS_P ("Library General Public License for more details.\n");
  GSS_P ("\n");
  GSS_P ("You should have received a copy of the GNU Library General Public\n");
  GSS_P ("License along with this library; if not, write to the\n");
  GSS_P ("Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,\n");
  GSS_P ("Boston, MA 02110-1301, USA.\n");
  GSS_P ("</pre>\n");

  GSS_P ("<h3>dashif.org DASH player</h3>\n");
  GSS_P ("<pre>\n");
  GSS_P ("The copyright in this software is being made available under\n");
  GSS_P ("the BSD License,\n");
  GSS_P ("included below. This software may be subject to other third\n");
  GSS_P ("party and contributor rights, including patent rights, and no\n");
  GSS_P ("such rights are granted under this license.\n");
  GSS_P ("\n");
  GSS_P ("Copyright (c) 2013, Digital Primates\n");
  GSS_P ("All rights reserved.\n");
  GSS_P ("\n");
  GSS_P ("Redistribution and use in source and binary forms, with or\n");
  GSS_P ("without modification, are permitted provided that the following\n");
  GSS_P ("conditions are met:\n");
  GSS_P ("Redistributions of source code must retain the above copyright\n");
  GSS_P ("notice, this list of conditions and the following disclaimer.\n");
  GSS_P ("Redistributions in binary form must reproduce the above copyright\n");
  GSS_P ("notice, this list of conditions and the following disclaimer in\n");
  GSS_P ("the documentation and/or other materials provided with the\n");
  GSS_P ("distribution.\n");
  GSS_P ("Neither the name of the Digital Primates nor the names of its\n");
  GSS_P ("contributors may be used to endorse or promote products derived\n");
  GSS_P ("from this software without specific prior written permission.\n");
  GSS_P ("\n");
  GSS_P ("THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND\n");
  GSS_P ("CONTRIBUTORS \"AS IS\" AND ANY EXPRESS OR IMPLIED WARRANTIES,\n");
  GSS_P ("INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF\n");
  GSS_P ("MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE\n");
  GSS_P ("DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR\n");
  GSS_P ("CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,\n");
  GSS_P ("SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT\n");
  GSS_P ("LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF\n");
  GSS_P ("USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED\n");
  GSS_P ("AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT\n");
  GSS_P ("LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING\n");
  GSS_P ("IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF\n");
  GSS_P ("THE POSSIBILITY OF SUCH DAMAGE.\n");
  GSS_P ("\n");
  GSS_P ("</pre>\n");

  gss_html_footer (t);
}

static void
gss_asset_get_resource (GssTransaction * t)
{
  const char *filename;
  gboolean ret;
  gchar *contents;
  gsize size;
  GError *error = NULL;
  const char *media_type;

  filename = t->path + 1;

  GST_DEBUG ("path: %s", filename);

  ret = g_file_get_contents (filename, &contents, &size, &error);
  if (!ret) {
    g_error_free (error);
    gss_transaction_error_not_found (t, "file not found for asset");
    return;
  }

  if (g_str_has_suffix (filename, ".html")) {
    media_type = GSS_TEXT_HTML;
  } else if (g_str_has_suffix (filename, ".js")) {
    media_type = "text/javascript";
  } else {
    media_type = "application/octet-stream";
  }

  soup_message_set_response (t->msg, media_type, SOUP_MEMORY_TAKE,
      contents, size);
  soup_message_set_status (t->msg, SOUP_STATUS_OK);
}

void
gss_server_create_module (GssServer * server, GssConfig * config, GType type,
    const char *name)
{
  GObject *object;

  object = gss_config_create_object (config, type, name);

  gss_server_add_module (server, GSS_MODULE (object));
}

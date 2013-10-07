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

#include "gss-server.h"
#include "gst-streaming-server/gss-user.h"
#include "gst-streaming-server/gss-manager.h"
#include "gst-streaming-server/gss-push.h"
#include "gst-streaming-server/gss-utils.h"
#include "gst-streaming-server/gss-vod.h"
#include "gst-streaming-server/gss-playready.h"

#include <gst/gst.h>

#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>


#define GETTEXT_PACKAGE "ew-stream-server"

#define CONFIG_FILENAME "config"

#define LOG g_print

gboolean verbose = TRUE;
gboolean cl_verbose;
gboolean enable_daemon = FALSE;
int http_port = 0;
int https_port = 0;
char *config_file = NULL;

static void signal_interrupt (int signum);
static void add_program (GssServer * server, int i);


static GOptionEntry entries[] = {
  {"verbose", 'v', 0, G_OPTION_ARG_NONE, &cl_verbose, "Be verbose", NULL},
  {"daemon", 'd', 0, G_OPTION_ARG_NONE, &enable_daemon, "Daemonize", NULL},
  {"http-port", 0, 0, G_OPTION_ARG_INT, &http_port, "HTTP port", NULL},
  {"https-port", 0, 0, G_OPTION_ARG_INT, &https_port, "HTTPS port", NULL},
  {"config-file", 0, 0, G_OPTION_ARG_STRING, &config_file, "Configuration file",
      NULL},

  {NULL}

};

GssConfig *config;
GssServer *server;
GMainLoop *main_loop;

static void G_GNUC_NORETURN
do_quit (int signal)
{
  LOG ("caught signal %d", signal);

  kill (0, SIGTERM);

  exit (0);
}

static void
daemonize (void)
{
  int ret;
  int fd;
  char s[20];

#if 0
  ret = chdir ("/var/log");
  if (ret < 0)
    exit (1);
#endif

  ret = fork ();
  if (ret < 0)
    exit (1);                   /* fork error */
  if (ret > 0)
    exit (0);                   /* parent */

  ret = setpgid (0, 0);
  if (ret < 0) {
    g_print ("could not set process group\n");
  }
  ret = setuid (65534);
  if (ret < 0) {
    g_print ("could not switch user to 'nobody'\n");
  }
  umask (0022);

  fd = open ("/dev/null", O_RDWR);
  dup2 (fd, 0);
  close (fd);

#if 0
  fd = open ("/tmp/ew-stream-server.log", O_RDWR | O_CREAT | O_TRUNC, 0644);
#else
  fd = open ("/dev/null", O_RDWR | O_CREAT | O_TRUNC, 0644);
#endif
  dup2 (fd, 1);
  dup2 (fd, 2);
  close (fd);

  fd = open ("/var/run/ew-stream-server.pid", O_RDWR | O_CREAT | O_TRUNC, 0644);
  sprintf (s, "%d\n", getpid ());
  ret = write (fd, s, strlen (s));
  close (fd);

  signal (SIGHUP, do_quit);
  signal (SIGTERM, do_quit);

}

int
main (int argc, char *argv[])
{
  GError *error = NULL;
  GOptionContext *context;
  int i;

  signal (SIGPIPE, SIG_IGN);
  signal (SIGINT, signal_interrupt);

  context = g_option_context_new ("- GStreamer Streaming Server");
  g_option_context_add_main_entries (context, entries, GETTEXT_PACKAGE);
  g_option_context_add_group (context, gst_init_get_option_group ());
  if (!g_option_context_parse (context, &argc, &argv, &error)) {
    g_print ("option parsing failed: %s", error->message);
    exit (1);
  }
  g_option_context_free (context);

  gss_init ();

  config = g_object_new (GSS_TYPE_CONFIG, "config-file", "config", NULL);
  gss_config_load_config_file (config);

  server = g_object_new (GSS_TYPE_SERVER, "name", "admin.server",
      "http-port", http_port, "https-port", https_port,
      "title", "GStreamer Streaming Server", NULL);
  gss_config_load_object (config, G_OBJECT (server), "admin.server");
  gss_config_attach (config, G_OBJECT (server));

  if (enable_daemon)
    daemonize ();

  if (server->server == NULL) {
    g_print ("failed to create HTTP server\n");
    exit (1);
  }
  if (server->ssl_server == NULL) {
    g_print ("failed to create HTTPS server\n");
    exit (1);
  }

  gss_config_add_server_resources (server);

  gss_server_create_module (server, config, GSS_TYPE_USER, "admin.user");
  gss_server_create_module (server, config, GSS_TYPE_MANAGER, "admin.manager");
  gss_server_create_module (server, config, GSS_TYPE_VOD, "admin.vod");
  gss_server_create_module (server, config, GSS_TYPE_PLAYREADY, "admin.pr");

  for (i = 0; i < 1; i++) {
    char *key;

    key = g_strdup_printf ("stream%d", i);
#if 0
    if (!gss_config_exists (server->config, key))
      break;
#endif

    add_program (server, i);

    g_free (key);
  }

  gss_config_save_config_file (config);

  main_loop = g_main_loop_new (NULL, TRUE);

  g_main_loop_run (main_loop);

  g_main_loop_unref (main_loop);
  main_loop = NULL;

  GSS_CLEANUP (server);

  gss_deinit ();
  gst_deinit ();

  exit (0);
}

static void
signal_interrupt (int signum)
{
  if (main_loop) {
    g_main_loop_quit (main_loop);
  }
}


static void
add_program (GssServer * server, int i)
{
  GssProgram *program;
  char *title;

  title = g_strdup_printf ("Stream #%d", i);
  program = gss_push_new ();
  g_object_set (program,
      "title", title, "description", "Automatically created push stream", NULL);
  gss_object_set_automatic_name (GSS_OBJECT (program));
  gss_server_add_program_simple (server, program);
  g_free (title);

  g_object_set (program, "enabled", TRUE, NULL);
}

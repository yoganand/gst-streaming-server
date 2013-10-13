
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gss-log.h"
#include "gss-utils.h"
#include "gss-object.h"
#include <gst/gst.h>
#include <string.h>
#include <syslog.h>

#define ENABLE_DEBUG

static void log_handler (GstDebugCategory * category, GstDebugLevel level,
    const gchar * file, const gchar * function, gint line, GObject * object,
    GstDebugMessage * message, gpointer data);
static void glog_handler (const gchar * log_domain, GLogLevelFlags log_level,
    const gchar * message, gpointer user_data);

GST_DEBUG_CATEGORY (gss_debug);

void
gss_log_init (void)
{
  GST_DEBUG_CATEGORY_INIT (gss_debug, "gss", 0, "Streaming Server");

#if GST_CHECK_VERSION(1,0,0)
  gst_debug_set_default_threshold (GST_LEVEL_FIXME);
#else
  gst_debug_set_default_threshold (GST_LEVEL_WARNING);
#endif
  gst_debug_set_threshold_for_name ("gss", GST_LEVEL_INFO);
  gst_debug_remove_log_function (gst_debug_log_default);
#if GST_CHECK_VERSION(1,0,0)
  gst_debug_add_log_function (log_handler, NULL, NULL);
#else
  gst_debug_add_log_function (log_handler, NULL);
#endif

  g_log_set_default_handler (glog_handler, NULL);

  openlog ("gst-streaming-server", LOG_NDELAY, LOG_DAEMON);
}

static void
log_handler (GstDebugCategory * category, GstDebugLevel level,
    const gchar * file, const gchar * function, gint line, GObject * object,
    GstDebugMessage * message, gpointer data)
{
#if GST_CHECK_VERSION(1,0,0)
  static const char level_char[] = " EWFIDLT8M";
#else
  static const char level_char[] = " EWIDLFTM";
#endif
  char *s2;

  if (level > gst_debug_category_get_threshold (category))
    return;

  if (strncmp (file, "gss-", 4) == 0)
    file += 4;
  if (strncmp (file, "gst", 3) == 0)
    file += 3;

  if (strncmp (file, "deck", 4) == 0 && line == 204) {
    /* FIXME someone shut this shit up */
    return;
  }
  if (strncmp (file, "vide", 4) == 0 && line == 1975) {
    /* FIXME someone shut this shit up */
    return;
  }
  if (strncmp (file, "inte", 4) == 0 && line == 472) {
    /* FIXME someone shut this shit up */
    return;
  }
  if (strncmp (file, "cbr.", 4) == 0) {
    /* FIXME someone shut this shit up */
    return;
  }
  if (strncmp (file, "h264", 4) == 0) {
    /* FIXME someone shut this shit up */
    return;
  }
  if (strncmp (file, "base", 4) == 0) {
    /* FIXME someone shut this shit up */
    return;
  }
  if (strncmp (file, "qtmu", 4) == 0) {
    /* FIXME someone shut this shit up */
    return;
  }
  if (strncmp (file, "ebml", 4) == 0) {
    /* FIXME someone shut this shit up */
    return;
  }

  s2 = g_strdup_printf ("%c %-10.10s %-4.4s:%-4d %s\n",
      level_char[level],
      GST_IS_OBJECT (object) ? GST_OBJECT_NAME (object) :
      GSS_IS_OBJECT (object) ? GSS_OBJECT_NAME (object) : "",
      file, line, gst_debug_message_get (message));

  gss_log_send_syslog (level, s2);

#ifdef ENABLE_DEBUG
  g_print ("%s", s2);
#endif
}

static void
glog_handler (const gchar * log_domain, GLogLevelFlags log_level,
    const gchar * message, gpointer user_data)
{
  char level_char;
  int level;
  char *s2;

  level_char = 'X';
  level = 0;
  if (log_level & (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL)) {
    level_char = 'E';
    level = 0;
  }
  if (log_level & (G_LOG_LEVEL_WARNING | G_LOG_LEVEL_MESSAGE)) {
    level_char = 'W';
    level = 1;
  }
  if (log_level & (G_LOG_LEVEL_INFO | G_LOG_LEVEL_DEBUG)) {
    level_char = 'I';
    level = 2;
  }

  s2 = g_strdup_printf ("%c %-10.10s glib:glib %s\n",
      level_char, log_domain, message);

  gss_log_send_syslog (level, s2);
#ifdef ENABLE_DEBUG
  g_print ("%s", s2);
#endif
}

void
gss_log_send_syslog (int level, const char *msg)
{
  int severity;

  severity = MIN (7, level + 3);

  syslog (LOG_DAEMON | severity, "%s", msg);
}

void
gss_log_transaction (GssTransaction * t)
{
  char *s;
  const char *user_agent;
  GDateTime *datetime;
  char *dt;

  user_agent = soup_message_headers_get_one (t->msg->request_headers,
      "User-Agent");
  if (user_agent == NULL) {
    user_agent = "-";
  }
  datetime = g_date_time_new_now_utc ();
  dt = g_date_time_format (datetime, "%Y-%m-%d %H:%M:%S");
  if (0) {
    s = g_strdup_printf ("%s - - [%s] \"%s %s %s\" %d %d \"%s\" \"%s\"",
        soup_address_get_physical (soup_client_context_get_address (t->client)),
        dt,
        t->msg->method,
        t->path,
        "HTTP/1.1",
        t->msg->status_code,
        (int) t->msg->response_body->length, "-", user_agent);
  } else {
    s = g_strdup_printf ("%s %s %s \"%s\" %d %" G_GSIZE_FORMAT " %"
        G_GUINT64_FORMAT " %s",
        soup_address_get_physical (soup_client_context_get_address (t->client)),
        dt, t->msg->method, t->path, t->msg->status_code,
        t->msg->response_body->length, t->finish_time - t->start_time,
        t->debug_message ? t->debug_message : "");
  }
  syslog (LOG_USER | LOG_INFO, "%s", s);
  g_print ("%s\n", s);
  g_free (s);
  g_free (dt);
  g_date_time_unref (datetime);
}

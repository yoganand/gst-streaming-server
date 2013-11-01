
#include "config.h"

#include <gst-streaming-server/gss-isom.h>

#include <stdio.h>



gboolean verbose = FALSE;
gboolean dump = FALSE;
gboolean fragment = FALSE;

static GOptionEntry entries[] = {
  {"verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose, "Be verbose", NULL},
  {"dump", 'd', 0, G_OPTION_ARG_NONE, &dump, "Dump file to readable output",
      NULL},
  {"fragment", 'd', 0, G_OPTION_ARG_NONE, &fragment, "Fragment file", NULL},
  {NULL}
};

int
main (int argc, char *argv[])
{
  GError *error = NULL;
  GOptionContext *context;
  int i;

  context = g_option_context_new ("- ISOM manipulation tool");
  g_option_context_add_main_entries (context, entries, GETTEXT_PACKAGE);
  g_option_context_add_group (context, gst_init_get_option_group ());
  if (!g_option_context_parse (context, &argc, &argv, &error)) {
    g_print ("option parsing failed: %s", error->message);
    exit (1);
  }
  g_option_context_free (context);

  if (argc < 2) {
    g_print ("need filename\n");
    exit (1);
  }

  for (i = 1; i < argc; i++) {
    GssIsomParser *file;
    gboolean ret;
    guint8 *data;
    int size;

    file = gss_isom_parser_new ();

    ret = gss_isom_parser_parse_file (file, argv[i]);
    if (!ret) {
      g_print ("parse failed");
      continue;
    }

    if (dump) {
      gss_isom_parser_dump (file);
    }
    if (fragment) {
      int j;
      GssIsomTrack *track;

      gss_isom_parser_fragmentize (file, TRUE);

      track = file->movie->tracks[0];

      g_print ("n_fragmenst: %d\n", track->n_fragments);
      for (j = 0; j < track->n_fragments; j++) {
        g_print ("fragment: %d\n", j);
        g_print ("  offset: %" G_GUINT64_FORMAT "\n",
            track->fragments[j]->offset);
        g_print ("  moof_size: %" G_GUINT64_FORMAT "\n",
            track->fragments[j]->moof_size);
        g_print ("  mdat_size: %d\n", track->fragments[j]->mdat_size);
      }

      gss_isom_track_serialize_dash (track, &data, &size);
      g_file_set_contents ("out.mov", (gchar *) data, size, NULL);
    }

    gss_isom_parser_free (file);
  }

  return 0;
}

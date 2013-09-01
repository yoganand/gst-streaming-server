
#include <gst-streaming-server/gss-isom.h>
#include <gst-streaming-server/gss-utils.h>
#include <json-glib/json-glib.h>

#include <stdio.h>

#include <openssl/aes.h>


#define GETTEXT_PACKAGE NULL

gboolean verbose;

static char *random_iv_string (void);

static GOptionEntry entries[] = {
  {"verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose, "Be verbose", NULL},
  {NULL}
};

int
main (int argc, char *argv[])
{
  GError *error = NULL;
  GOptionContext *context;
  JsonGenerator *gen;
  JsonNode *root;
  JsonObject *obj;
  JsonArray *array;
  int i;
  char *data;

  context = g_option_context_new ("- ISOM parsing test");
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

  array = json_array_new ();
  for (i = 1; i < argc; i++) {
    GssIsomParser *file;
    GssIsomTrack *video_track;
    GssIsomTrack *audio_track;
    JsonObject *object;
    JsonObject *o;
    gboolean ret;

    file = gss_isom_parser_new ();

    ret = gss_isom_parser_parse_file (file, argv[i]);
    if (!ret) {
      g_print ("parse failed");
      continue;
    }

    video_track = gss_isom_movie_get_video_track (file->movie);
    audio_track = gss_isom_movie_get_audio_track (file->movie);

    object = json_object_new ();
    json_object_set_string_member (object, "filename", argv[i]);
    if (video_track) {
      char *codec;

      o = json_object_new ();
      json_object_set_int_member (o, "track_id", video_track->tkhd.track_id);
      json_object_set_int_member (o, "bitrate", video_track->esds.avg_bitrate);
      codec = g_strdup_printf ("avc1.%02x%02x%02x",
          video_track->esds.codec_data[1],
          video_track->esds.codec_data[2], video_track->esds.codec_data[3]);
      json_object_set_string_member (o, "codec", codec);
      g_free (codec);
      json_object_set_string_member (o, "iv", random_iv_string ());
      json_object_set_object_member (object, "video", o);
    }
    if (audio_track) {
      o = json_object_new ();
      json_object_set_int_member (o, "track_id", audio_track->tkhd.track_id);
      json_object_set_int_member (o, "bitrate", audio_track->esds.avg_bitrate);
      json_object_set_string_member (o, "codec", "mp4a.40.2");  /* AAC LC */
      json_object_set_string_member (o, "iv", random_iv_string ());
      json_object_set_object_member (object, "audio", o);
    }

    json_array_add_object_element (array, object);

    gss_isom_parser_free (file);
  }

  obj = json_object_new ();
  json_object_set_int_member (obj, "version", 0);
  json_object_set_array_member (obj, "media", array);

  root = json_node_new (JSON_NODE_OBJECT);
  json_node_take_object (root, obj);
  gen = g_object_new (JSON_TYPE_GENERATOR,
      "root", root, "pretty", TRUE, "indent", 2, NULL);
  data = json_generator_to_data (gen, NULL);
  g_object_unref (gen);
  json_node_free (root);

  g_print ("%s\n", data);

  return 0;
}

static char *
random_iv_string (void)
{
  guint8 iv[8];

  gss_utils_get_random_bytes (iv, 8);
  return gss_base64url_encode (iv, 8);
}


bin_PROGRAMS = \
	gst-streaming-server \
	gss-transcoder

noinst_PROGRAMS = \
	vts-server gss-info gss-isom-tool


gst_streaming_server_CFLAGS = $(GSS_CFLAGS) $(GST_CFLAGS) $(SOUP_CFLAGS) $(GST_RTSP_SERVER_CFLAGS) $(JSON_GLIB_CFLAGS)
gst_streaming_server_LDADD = $(GSS_LIBS) $(GST_LIBS) $(SOUP_LIBS) $(GST_RTSP_SERVER_LIBS) $(JSON_GLIB_LIBS)
gst_streaming_server_SOURCES = \
	gst-streaming-server.c \
	gst-streaming-server.h

vts_server_CFLAGS = $(GSS_CFLAGS) $(GST_CFLAGS) $(SOUP_CFLAGS) $(GST_RTSP_SERVER_CFLAGS) $(JSON_GLIB_CFLAGS)
vts_server_LDADD = $(GSS_LIBS) $(GST_LIBS) $(SOUP_LIBS) $(GST_RTSP_SERVER_LIBS) $(JSON_GLIB_LIBS)
vts_server_SOURCES = \
	vts-server.c

gss_info_CFLAGS = $(GSS_CFLAGS) $(GST_CFLAGS) $(SOUP_CFLAGS) $(GST_RTSP_SERVER_CFLAGS) $(JSON_GLIB_CFLAGS) $(OPENSSL_CFLAGS)
gss_info_LDADD = $(GSS_LIBS) $(GST_LIBS) $(SOUP_LIBS) $(GST_RTSP_SERVER_LIBS) $(JSON_GLIB_LIBS) $(OPENSSL_LIBS)
gss_info_SOURCES = \
	gss-info.c

gss_transcoder_CFLAGS = $(GSS_CFLAGS) $(GST_CFLAGS) $(SOUP_CFLAGS) $(GST_RTSP_SERVER_CFLAGS) $(JSON_GLIB_CFLAGS)
gss_transcoder_LDADD = $(GSS_LIBS) $(GST_LIBS) $(SOUP_LIBS) $(GST_RTSP_SERVER_LIBS) $(JSON_GLIB_LIBS)
gss_transcoder_SOURCES = \
	gss-transcoder.c

gss_isom_tool_CFLAGS = $(GSS_CFLAGS) $(GST_CFLAGS) $(SOUP_CFLAGS) $(GST_RTSP_SERVER_CFLAGS) $(JSON_GLIB_CFLAGS) $(OPENSSL_CFLAGS)
gss_isom_tool_LDADD = $(GSS_LIBS) $(GST_LIBS) $(SOUP_LIBS) $(GST_RTSP_SERVER_LIBS) $(JSON_GLIB_LIBS) $(OPENSSL_LIBS)
gss_isom_tool_SOURCES = \
	gss-isom-tool.c




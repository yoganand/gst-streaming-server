
bin_PROGRAMS = \
	gss-server \
	gss-transcoder

noinst_PROGRAMS = \
	vts-server


gss_server_CFLAGS = $(GSS_CFLAGS) $(GST_CFLAGS) $(SOUP_CFLAGS) $(GST_RTSP_SERVER_CFLAGS) $(JSON_GLIB_CFLAGS)
gss_server_LDADD = $(GSS_LIBS) $(GST_LIBS) $(SOUP_LIBS) $(GST_RTSP_SERVER_LIBS) $(JSON_GLIB_LIBS)
gss_server_SOURCES = \
	gss-server.c \
	gss-server.h

vts_server_CFLAGS = $(GSS_CFLAGS) $(GST_CFLAGS) $(SOUP_CFLAGS) $(GST_RTSP_SERVER_CFLAGS) $(JSON_GLIB_CFLAGS)
vts_server_LDADD = $(GSS_LIBS) $(GST_LIBS) $(SOUP_LIBS) $(GST_RTSP_SERVER_LIBS) $(JSON_GLIB_LIBS)
vts_server_SOURCES = \
	vts-server.c

gss_transcoder_CFLAGS = $(GSS_CFLAGS) $(GST_CFLAGS) $(SOUP_CFLAGS) $(GST_RTSP_SERVER_CFLAGS) $(JSON_GLIB_CFLAGS)
gss_transcoder_LDADD = $(GSS_LIBS) $(GST_LIBS) $(SOUP_LIBS) $(GST_RTSP_SERVER_LIBS) $(JSON_GLIB_LIBS)
gss_transcoder_SOURCES = \
	gss-transcoder.c \
	gss-transcoder.h




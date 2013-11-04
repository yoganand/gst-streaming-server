include $(top_srcdir)/common/check.mak


AM_CFLAGS = $(GST_CFLAGS) $(SOUP_CFLAGS) $(GSS_CFLAGS) $(GST_CHECK_CFLAGS)
LDADD = $(GSS_LIBS) $(GST_LIBS) $(SOUP_LIBS) $(GST_CHECK_LIBS)

check_PROGRAMS = \
	sglist

TESTS = $(check_PROGRAMS)


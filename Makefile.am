
AUTOMAKE_OPTIONS = foreign

SUBDIRS = gst-streaming-server tools pkgconfig doc tests po

EXTRA_DIST = autogen.sh gtk-doc.make HACKING TODO BUGS README

DISTCHECK_CONFIGURE_FLAGS=--enable-gtk-doc

ACLOCAL_AMFLAGS = -I m4

include $(top_srcdir)/common/release.mak
include $(top_srcdir)/common/po.mak

check-valgrind:
	cd tests/check && make check-valgrind

if HAVE_GST_CHECK
check-torture:
	cd tests/check && make torture
else
check-torture:
	true
endif



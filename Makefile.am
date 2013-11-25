
AUTOMAKE_OPTIONS = foreign

SUBDIRS = gst-streaming-server tools pkgconfig doc tests po common

debian_files = \
	debian/changelog \
	debian/source \
	debian/source/format \
	debian/README.source \
	debian/rules \
	debian/copyright \
	debian/gst-streaming-server.upstart \
	debian/control \
	debian/compat \
	debian/postinst \
	debian/docs \
	debian/create-changelog-entry \
	debian/README.Debian \
	debian/gst-streaming-server.default

EXTRA_DIST = autogen.sh gtk-doc.make HACKING TODO BUGS README $(debian_files)

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



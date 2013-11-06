AC_PREREQ([2.68])

dnl please read gstreamer/docs/random/autotools before changing this file

dnl initialize autoconf
dnl releases only do -Wall, git and prerelease does -Werror too
dnl use a three digit version number for releases, and four for git/prerelease
AC_INIT([GStreamer Streaming Server],[0.10.0.1],[http://bugzilla.gnome.org/enter_bug.cgi?product=GStreamer],[gst-streaming-server])

AG_GST_INIT

dnl initialize automake
AM_INIT_AUTOMAKE([-Wno-portability 1.11 no-dist-gzip dist-xz tar-ustar])

dnl define PACKAGE_VERSION_* variables
AS_VERSION

dnl check if this is a release version
AS_NANO(GST_GIT="no", GST_GIT="yes")

dnl can autoconf find the source ?
AC_CONFIG_SRCDIR([gst/audiotestsrc/gstaudiotestsrc.c])

dnl define the output header for config
AC_CONFIG_HEADERS([config.h])

dnl AM_MAINTAINER_MODE only provides the option to configure to enable it
AM_MAINTAINER_MODE([enable])

dnl sets host_* variables
AC_CANONICAL_HOST

dnl use pretty build output with automake >= 1.11
m4_ifdef([AM_SILENT_RULES],[AM_SILENT_RULES([yes])],
  [AM_DEFAULT_VERBOSITY=1
   AC_SUBST(AM_DEFAULT_VERBOSITY)])

dnl our libraries and install dirs use GST_API_VERSION in the filename
dnl to allow side-by-side installation of different API versions
GST_API_VERSION=1.0
AC_SUBST(GST_API_VERSION)
AC_DEFINE_UNQUOTED(GST_API_VERSION, "$GST_API_VERSION",
  [GStreamer API Version])

dnl CURRENT, REVISION, AGE
dnl - library source changed -> increment REVISION
dnl - interfaces added/removed/changed -> increment CURRENT, REVISION = 0
dnl - interfaces added -> increment AGE
dnl - interfaces removed -> AGE = 0
dnl
dnl Keep CURRENT as MINOR * 100 + MICRO
dnl Ex : 1.0.0 => 0
dnl      1.0.3 => 3
dnl      1.1.0 => 100
dnl      1.2.5 => 205
dnl      1.10.9 (who knows) => 1009
dnl
dnl sets GST_LT_LDFLAGS
AS_LIBTOOL(GST, 0, 0, 0)

dnl *** required versions of GStreamer stuff ***
GST_REQ=1.0.0

dnl *** autotools stuff ****

dnl allow for different autotools
AS_AUTOTOOLS_ALTERNATE

dnl Add parameters for aclocal
AC_SUBST(ACLOCAL_AMFLAGS, "-I m4 -I common/m4")

dnl set up gettext
dnl the version check needs to stay here because autopoint greps for it
AM_GNU_GETTEXT_VERSION([0.17])
AM_GNU_GETTEXT([external])
AG_GST_GETTEXT([gst-streaming-server-$GST_API_VERSION])

dnl *** check for arguments to configure ***

AG_GST_ARG_DISABLE_FATAL_WARNINGS

AG_GST_ARG_DEBUG
AG_GST_ARG_PROFILING
AG_GST_ARG_VALGRIND
AG_GST_ARG_GCOV

AG_GST_ARG_EXAMPLES

AG_GST_ARG_WITH_PKG_CONFIG_PATH
AG_GST_ARG_WITH_PACKAGE_NAME
AG_GST_ARG_WITH_PACKAGE_ORIGIN

AG_GST_PKG_CONFIG_PATH

dnl allow for different autotools
AS_AUTOTOOLS_ALTERNATE

AC_CONFIG_SRCDIR([gst-streaming-server/gss-server.c])

AC_CONFIG_MACRO_DIR([m4])
ACLOCAL_AMFLAGS="-I m4 $ACLOCAL_AMFLAGS"
AC_SUBST(ACLOCAL_AMFLAGS)

dnl set up gettext
dnl the version check needs to stay here because autopoint greps for it
#AM_GNU_GETTEXT_VERSION([0.11.5])
#AM_GNU_GETTEXT([external])

AC_LIBTOOL_WIN32_DLL
AC_PROG_LIBTOOL
AM_PROG_AS
AC_PROG_CXX
AM_PROG_CC_C_O
AC_SYS_LARGEFILE
LT_PATH_NM

AC_PATH_PROG(VALGRIND_PATH, valgrind, no)
AM_CONDITIONAL(HAVE_VALGRIND, test ! "x$VALGRIND_PATH" = "xno")

AC_CHECK_LIBM
AC_SUBST(LIBM)

AS_COMPILER_FLAG(-Wall, GSS_CFLAGS="$GSS_CFLAGS -Wall")
if test "x$GSS_GIT" = "xyes"
then
  AS_COMPILER_FLAG(-Werror, GSS_CFLAGS="$GSS_CFLAGS -Werror")
fi

#AS_COMPILER_FLAG(-O3, CFLAGS=`echo $CFLAGS | sed s/O2/O3/`)
#AS_COMPILER_FLAG(-O3, CXXFLAGS=`echo $CXXFLAGS | sed s/O2/O3/`)

# Enable severe pain
#AS_COMPILER_FLAGS(COMPANY_CFLAGS,"-Wextra -Wno-missing-field-initializers -Wno-unused-parameter -Wold-style-definition -Wdeclaration-after-statement -Wmissing-declarations -Wmissing-prototypes -Wredundant-decls -Wmissing-noreturn -Wshadow -Wpointer-arith -Wcast-align -Wwrite-strings -Winline -Wformat-nonliteral -Wformat-security -Wswitch-enum -Wswitch-default")
#COMPANY_CFLAGS="-Wextra -Wno-missing-field-initializers -Wno-unused-parameter -Wold-style-definition -Wdeclaration-after-statement -Wmissing-declarations -Wmissing-prototypes -Wredundant-decls -Wmissing-noreturn -Wshadow -Wpointer-arith -Wcast-align -Wwrite-strings -Winline -Wformat-nonliteral -Wformat-security -Wswitch-enum -Wswitch-default"
#GSS_CFLAGS="$GSS_CFLAGS $COMPANY_CFLAGS"


##################################################
# Check for gtk-doc.
##################################################

if test "x$cross_compiling" = "xyes" ; then
  enable_gtk_doc=no
fi
GTK_DOC_CHECK([1.0])


########################
# GStreamer stuff
########################

PKG_PROG_PKG_CONFIG()
AC_ARG_WITH(gst-api-version,
  AC_HELP_STRING([--with-gst-api-version],[0.10,1.0,auto (default auto)]),
    [], [with_gst_api_version=auto])
case "${with_gst_api_version}" in
  0.10|1.0)
    GST_API_VERSION="${with_gst_api_version}"
    ;;
  auto)
    if ${PKG_CONFIG} --exists gstreamer-1.0 ; then
      GST_API_VERSION=1.0
    elif ${PKG_CONFIG} --exists gstreamer-0.10 ; then
      GST_API_VERSION=0.10
    else
      GST_API_VERSION=1.0
    fi
    ;;
  *)
    echo "Bad gst-api-version: $with_gst_api_version"
    ;;
esac

dnl *** check for dependencies ***

GLIB_REQ=2.26.0
AG_GST_PKG_CHECK_MODULES(GLIB, glib-2.0 >= $GLIB_REQ, yes)

AG_GST_PKG_CHECK_MODULES(GST,
    gstreamer-$GST_API_VERSION gstreamer-base-$GST_API_VERSION, yes)

AG_GST_PKG_CHECK_MODULES(GST_CHECK, gstreamer-check-$GST_API_VERSION, yes)
AM_CONDITIONAL(HAVE_GST_CHECK, test "x$HAVE_GST_CHECK" = "xyes")

GST_RTSP_SERVER_REQ=0.10.1
AG_GST_PKG_CHECK_MODULES(GST_RTSP_SERVER,
    gst-rtsp-server-$GST_API_VERSION >= $GST_RTSP_SERVER_REQ)
if test "$HAVE_GST_RTSP_SERVER" = yes ; then
  AC_DEFINE(ENABLE_RTSP, 1, [Enable RTSP])
fi
AM_CONDITIONAL(ENABLE_RTSP, [test "$HAVE_GST_RTSP_SERVER" = yes])

LIBSOUP_REQ=2.38.0
AG_GST_PKG_CHECK_MODULES(SOUP, libsoup-2.4 > LIBSOUP_REQ, yes)

JSON_GLIB_REQ=1.0
AG_GST_PKG_CHECK_MODULES(JSON_GLIB,json-glib-1.0 > JSON_GLIB_REQ, yes)

AG_GST_PKG_CHECK_MODULES(OPENSSL, openssl)

AG_GST_PKG_CHECK_MODULES(LIBXML2, libxml-2.0, yes)

AM_CONDITIONAL(ENABLE_STATIC_LIB, [false])

GST_ALL_LDFLAGS="-no-undefined"
AC_SUBST(GST_ALL_LDFLAGS)

AM_CONDITIONAL(HAVE_FALSE, false)

GSS_CFLAGS="$GSS_CFLAGS -I\$(top_srcdir)"
AC_SUBST(GSS_CFLAGS)

GSS_LIBS="\$(top_builddir)/gst-streaming-server/libgss-$GST_API_VERSION.la"
AC_SUBST(GSS_LIBS)


AS_HOST_DEFINES()

#tests/check/Makefile
AC_CONFIG_FILES([
Makefile 
doc/Makefile
gst-streaming-server/Makefile
pkgconfig/Makefile
pkgconfig/gst-streaming-server-uninstalled.pc
pkgconfig/gst-streaming-server.pc
tools/Makefile
tests/Makefile
doc/version.entities
po/Makefile.in
common/Makefile
common/m4/Makefile
])
AC_OUTPUT

#case "$host_os" in
#  mingw32*)
#    sed 's/^deplibs_check_method=.*/deplibs_check_method="pass_all"/' libtool >libtool.tmp && mv libtool.tmp libtool
#    ;;
#esac


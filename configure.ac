AC_PREREQ(2.63)
AC_INIT([spice-vdagent], [0.8.2])
AC_CONFIG_SRCDIR([configure.ac])

AM_CONFIG_HEADER([src/config.h])

AM_INIT_AUTOMAKE([foreign dist-bzip2 no-dist-gzip subdir-objects])
m4_ifdef([AM_SILENT_RULES],[AM_SILENT_RULES([yes])])

AC_PROG_CC
AC_HEADER_STDC
AC_PROG_INSTALL
AC_PROG_LN_S
AC_DEFINE(_GNU_SOURCE, [1], [Enable GNU extensions])

AC_ARG_ENABLE([console-kit],
              [AS_HELP_STRING([--enable-console-kit], [Enable ConsoleKit use (default: yes)])],
              [enable_console_kit="$enableval"],
              [enable_console_kit="yes"])

AC_ARG_ENABLE([pciaccess],
              [AS_HELP_STRING([--enable-pciaccess], [Enable libpciaccess use for auto generation of Xinerama xorg.conf (default: yes)])],
              [enable_pciaccess="$enableval"],
              [enable_pciaccess="yes"])

PKG_PROG_PKG_CONFIG
PKG_CHECK_MODULES(X, [xfixes xrandr xinerama x11])
PKG_CHECK_MODULES(SPICE, [spice-protocol >= 0.8.0])
if test x"$enable_console_kit" = "xyes" ; then
    PKG_CHECK_MODULES(DBUS, [dbus-1])
    AC_DEFINE([HAVE_CONSOLE_KIT], [1], [If defined, vdagentd will be compiled with ConsoleKit support] )
fi
AM_CONDITIONAL(HAVE_CONSOLE_KIT, test x"$enable_console_kit" = "xyes")
if test x"$enable_pciaccess" = "xyes" ; then
    PKG_CHECK_MODULES(PCIACCESS, [pciaccess >= 0.10])
    AC_DEFINE([HAVE_PCIACCESS], [1], [If defined, vdagentd will be compiled with pciaccess support] )
fi
AM_CONDITIONAL(HAVE_PCIACCESS, test x"$enable_pciaccess" = "xyes")

AC_CONFIG_FILES([
Makefile
])
AC_OUTPUT

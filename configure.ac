#!/bin/bash -norc
dnl	Process this file with autoconf to produce the configure script
dnl	for the tkcapture extension (TEA build).

AC_INIT([tkcapture],[1.0])

TEA_INIT()

AC_CONFIG_AUX_DIR(tclconfig)

#--------------------------------------------------------------------
# Load tclConfig.sh and tkConfig.sh.
#--------------------------------------------------------------------

TEA_PATH_TCLCONFIG
TEA_LOAD_TCLCONFIG
TEA_PATH_TKCONFIG
TEA_LOAD_TKCONFIG

#--------------------------------------------------------------------
# Tk built in its macosx directory defines MAC_OSX_TK in tkConfig.h
# rather than TK_DEFS, so TEA takes it for X11; --enable-aqua says so.
#--------------------------------------------------------------------

AC_ARG_ENABLE(aqua,
    AS_HELP_STRING([--enable-aqua],
	[build for Aqua Tk where TEA detects X11 (default: no)]),
    [enable_aqua=$enableval], [enable_aqua=no])
if test "$enable_aqua" = yes -a "${TEA_WINDOWINGSYSTEM}" = x11; then
    AC_DEFINE(MAC_OSX_TK, 1, [Are we building against Mac OS X TkAqua?])
    TEA_WINDOWINGSYSTEM=aqua
fi

TEA_PREFIX
TEA_SETUP_COMPILER

#--------------------------------------------------------------------
# Sources.  One backend per windowing system; the XImage converter is
# needed by the X11 backend only.
#--------------------------------------------------------------------

TEA_ADD_SOURCES([tkCapture.c])
TEA_ADD_HEADERS([])
TEA_ADD_INCLUDES([-I\"`${CYGPATH} ${srcdir}/generic`\"])
TEA_ADD_CFLAGS([])
TEA_ADD_STUB_SOURCES([])
TEA_ADD_TCL_SOURCES([library/tkcapture.tcl])

case "${TEA_WINDOWINGSYSTEM}" in
    win32)
	TEA_ADD_SOURCES([tkCaptureWin.c])
	TEA_ADD_LIBS([-luser32 -lgdi32 -ldwmapi])
	;;
    aqua)
	TEA_ADD_SOURCES([tkCaptureMac.m])
	TEA_ADD_LIBS([-framework Cocoa -framework QuartzCore])
	;;
    *)
	TEA_ADD_SOURCES([tkCaptureConvert.c tkCaptureX11.c])
	;;
esac

#--------------------------------------------------------------------
# Public Tcl and Tk headers only.  Those of the installed Tcl and Tk
# that the configuration files describe come first: TEA otherwise
# looks in the extension's prefix first, which MSYS2 sets to its own
# tree, where the headers of another Tcl version may be.
#--------------------------------------------------------------------

if test x"${with_tclinclude}" = x -a ! -f "${TCL_BIN_DIR}/Makefile" \
	-a -f "${TCL_PREFIX}/include/tcl.h" ; then
    with_tclinclude="${TCL_PREFIX}/include"
fi
if test x"${with_tkinclude}" = x -a ! -f "${TK_BIN_DIR}/Makefile" \
	-a -f "${TK_PREFIX}/include/tk.h" ; then
    with_tkinclude="${TK_PREFIX}/include"
fi

TEA_PUBLIC_TCL_HEADERS
TEA_PUBLIC_TK_HEADERS
TEA_PATH_X

#--------------------------------------------------------------------
# X11: under a compositing manager, the X Composite extension gives
# the contents of a toplevel wherever it is on the screen.
#--------------------------------------------------------------------

AC_ARG_ENABLE(xcomposite,
    AS_HELP_STRING([--enable-xcomposite],
	[read toplevels through the X Composite extension (default: yes,
	if available)]),
    [enable_xcomposite=$enableval], [enable_xcomposite=yes])
if test "${TEA_WINDOWINGSYSTEM}" = x11 -a "$enable_xcomposite" = yes; then
    tkcapture_save_CPPFLAGS="$CPPFLAGS"
    CPPFLAGS="$CPPFLAGS $XINCLUDES"
    AC_CHECK_HEADER([X11/extensions/Xcomposite.h],
	[AC_CHECK_LIB([Xcomposite], [XCompositeNameWindowPixmap],
	    [enable_xcomposite=yes], [enable_xcomposite=no], [$XLIBSW])],
	[enable_xcomposite=no], [#include <X11/Xlib.h>])
    CPPFLAGS="$tkcapture_save_CPPFLAGS"
    if test "$enable_xcomposite" = yes; then
	AC_DEFINE(HAVE_XCOMPOSITE, 1, [Use the X Composite extension])
	TEA_ADD_LIBS([-lXcomposite])
    fi
    AC_MSG_CHECKING([whether to use the X Composite extension])
    AC_MSG_RESULT([$enable_xcomposite])
fi

TEA_ENABLE_THREADS
TEA_ENABLE_SHARED
TEA_CONFIG_CFLAGS
TEA_ENABLE_SYMBOLS

AC_DEFINE(USE_TCL_STUBS, 1, [Use Tcl stubs])
AC_DEFINE(USE_TK_STUBS, 1, [Use Tk stubs])

TEA_MAKE_LIB

TEA_PROG_TCLSH
TEA_PROG_WISH

AC_CONFIG_FILES([Makefile pkgIndex.tcl])

AC_OUTPUT

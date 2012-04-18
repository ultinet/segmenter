#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ(2.59)
AC_INIT(segmenter, 0.1.0, mozo@mozo.jp)
AM_INIT_AUTOMAKE([-Wall -Werror])
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_SRCDIR([segmenter.c])
AC_CONFIG_HEADER([config.h])

LT_INIT

# Checks for programs.
AC_PROG_CC

PKG_PROG_PKG_CONFIG
PKG_CHECK_MODULES([FFMPEG], [libavcodec libavformat libavutil], [], [
  AC_MSG_ERROR([FFmpeg libraries weren't find among PKG_CONFIG_PATH])
])

# Checks for header files.
AC_HEADER_STDC
AC_CHECK_HEADERS([stdlib.h string.h getopt.h libgen.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_C_CONST
AC_TYPE_SIZE_T

# Checks for library functions.
AC_FUNC_MALLOC
AC_FUNC_STRTOD
AC_CHECK_FUNCS([memmove strchr strdup strrchr strtol])

ac_save_CFLAGS=$CFLAGS
ac_save_LDFLAGS=$LDFLAGS

CFLAGS="$CFLAGS $FFMPEG_CFLAGS"
LDFLAGS="$LDFLAGS $FFMPEG_LIBS"

AC_CHECK_TYPE([enum AVMediaType], [
  AC_DEFINE([HAVE_AV_MEDIA_TYPE], [1], [Define to 1 if AVMediaType is defined])
], [], [
#include <libavutil/avutil.h>
])
AC_CHECK_DECL([AV_PKT_FLAG_KEY], [
  AC_DEFINE([HAVE_AV_PKT_FLAG_KEY], [1], [Define to 1 if AV_KPT_FLAG_KEY is defined])
], [], [
#include <libavcodec/avcodec.h>
])
AC_CHECK_FUNCS([av_strerror avio_open avio_close avio_flush])
AC_CHECK_FUNCS([avformat_new_stream avformat_open_input avformat_find_stream_info avformat_write_header avformat_close_input])
AC_CHECK_FUNCS([avcodec_open2])

CFLAGS=$ac_save_CFLAGS
LDFLAGS=$ac_save_LDFLAGS

AC_CHECK_FUNCS([basename])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT

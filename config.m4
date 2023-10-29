dnl config.m4 for extension trapbox

dnl Comments in this file start with the string 'dnl'.
dnl Remove where necessary.

dnl If your extension references something external, use 'with':

dnl PHP_ARG_WITH([trapbox],
dnl   [for trapbox support],
dnl   [AS_HELP_STRING([--with-trapbox],
dnl     [Include trapbox support])])

dnl Otherwise use 'enable':

PHP_ARG_ENABLE([trapbox],
  [whether to enable trapbox support],
  [AS_HELP_STRING([--enable-trapbox],
    [Enable trapbox support])],
  [no])

if test "$PHP_TRAPBOX" != "no"; then
  dnl Write more examples of tests here...

  dnl Remove this code block if the library does not support pkg-config.
  dnl PKG_CHECK_MODULES([LIBFOO], [foo])
  dnl PHP_EVAL_INCLINE($LIBFOO_CFLAGS)
  dnl PHP_EVAL_LIBLINE($LIBFOO_LIBS, TRAPBOX_SHARED_LIBADD)

  dnl If you need to check for a particular library version using PKG_CHECK_MODULES,
  dnl you can use comparison operators. For example:
  dnl PKG_CHECK_MODULES([LIBFOO], [foo >= 1.2.3])
  dnl PKG_CHECK_MODULES([LIBFOO], [foo < 3.4])
  dnl PKG_CHECK_MODULES([LIBFOO], [foo = 1.2.3])

  dnl Remove this code block if the library supports pkg-config.
  dnl --with-trapbox -> check with-path
  dnl SEARCH_PATH="/usr/local /usr"     # you might want to change this
  dnl SEARCH_FOR="/include/trapbox.h"  # you most likely want to change this
  dnl if test -r $PHP_TRAPBOX/$SEARCH_FOR; then # path given as parameter
  dnl   TRAPBOX_DIR=$PHP_TRAPBOX
  dnl else # search default path list
  dnl   AC_MSG_CHECKING([for trapbox files in default path])
  dnl   for i in $SEARCH_PATH ; do
  dnl     if test -r $i/$SEARCH_FOR; then
  dnl       TRAPBOX_DIR=$i
  dnl       AC_MSG_RESULT(found in $i)
  dnl     fi
  dnl   done
  dnl fi
  dnl
  dnl if test -z "$TRAPBOX_DIR"; then
  dnl   AC_MSG_RESULT([not found])
  dnl   AC_MSG_ERROR([Please reinstall the trapbox distribution])
  dnl fi

  dnl Remove this code block if the library supports pkg-config.
  dnl --with-trapbox -> add include path
  dnl PHP_ADD_INCLUDE($TRAPBOX_DIR/include)

  dnl Remove this code block if the library supports pkg-config.
  dnl --with-trapbox -> check for lib and symbol presence
  dnl LIBNAME=TRAPBOX # you may want to change this
  dnl LIBSYMBOL=TRAPBOX # you most likely want to change this

  dnl If you need to check for a particular library function (e.g. a conditional
  dnl or version-dependent feature) and you are using pkg-config:
  dnl PHP_CHECK_LIBRARY($LIBNAME, $LIBSYMBOL,
  dnl [
  dnl   AC_DEFINE(HAVE_TRAPBOX_FEATURE, 1, [ ])
  dnl ],[
  dnl   AC_MSG_ERROR([FEATURE not supported by your trapbox library.])
  dnl ], [
  dnl   $LIBFOO_LIBS
  dnl ])

  dnl If you need to check for a particular library function (e.g. a conditional
  dnl or version-dependent feature) and you are not using pkg-config:
  dnl PHP_CHECK_LIBRARY($LIBNAME, $LIBSYMBOL,
  dnl [
  dnl   PHP_ADD_LIBRARY_WITH_PATH($LIBNAME, $TRAPBOX_DIR/$PHP_LIBDIR, TRAPBOX_SHARED_LIBADD)
  dnl   AC_DEFINE(HAVE_TRAPBOX_FEATURE, 1, [ ])
  dnl ],[
  dnl   AC_MSG_ERROR([FEATURE not supported by your trapbox library.])
  dnl ],[
  dnl   -L$TRAPBOX_DIR/$PHP_LIBDIR -lm
  dnl ])
  dnl
  dnl PHP_SUBST(TRAPBOX_SHARED_LIBADD)

  dnl In case of no dependencies
  AC_DEFINE(HAVE_TRAPBOX, 1, [ Have trapbox support ])

  PHP_NEW_EXTENSION(trapbox, trapbox.c, $ext_shared)
fi

# Require a minimum autoconf version
AC_PREREQ([2.65])

# Initialize autoconf
# Specify package's name, version number, and bug-report address.
AC_INIT([Parlib], [1.0], [klueska@cs.berkeley.edu])

# Directory containing any external m4 macros
AC_CONFIG_MACRO_DIR([m4])

# Auxiliary scripts such as install-sh and depcomp should be in DIRECTORY
AC_CONFIG_AUX_DIR([build-aux])

# Make the default prefix /usr instead of /usr/local
AC_PREFIX_DEFAULT([/usr])

# Initialize automake
# Turn on all Automake warnings and report them as errors.
# This is a foreign package
AM_INIT_AUTOMAKE([subdir-objects foreign -Wall -Werror -Wno-portability])

# Checks for a C compiler
AC_PROG_CC

# Checks for a C++ compiler
#AC_PROG_CXX

# Check for an assembler
AM_PROG_AS

# Checks for libtool
AC_PROG_LIBTOOL

# Check for HEADERS and #define HAVE_HEADER_H for each header found
#AC_CHECK_HEADERS([HEADERS ...])

# Output the following to config.h 
#AC_DEFINE(VARIABLE, VALUE, DESCRIPTION)

# Declare config.h as the output header
AC_CONFIG_HEADERS([config.h])
AX_PREFIX_CONFIG_H()

# Declare Makefiles as output files
AC_CONFIG_FILES([
  Makefile 
])

# A safety check. FILE is a distributed source file, making sure that
# 'configure' is not run from outerspace.
AC_CONFIG_SRCDIR([src/vcore.c])

# Set the common AM_CFLAGS for all Makefile.am files
COMMON_CFLAGS="            \
  -std=gnu99               \
  -g -O2 -Wall             \
  -Wno-unused-function     \
  -Wno-unused-value        \
  -Wno-missing-braces      \
  -fno-exceptions          \
  -fno-strict-aliasing     \
"

LIB_CFLAGS="               \
  -ftls-model=initial-exec \
  -fvisibility=hidden      \
"

TEST_CFLAGS="              \
"

# Set the common AM_CFLAGS for all Makefile.am files
AC_SUBST([LIB_CFLAGS],["$COMMON_CFLAGS $LIB_CFLAGS"])
AC_SUBST([TEST_CFLAGS],["$COMMON_CFLAGS $TEST_CFLAGS"])

# Set up some global variables for use in the makefile
SRCDIR=src
TESTSDIR=tests
SYSDEPDIR_BASE=$SRCDIR/sysdeps/unix/sysv/linux
SYSDEPDIR_i686=$SYSDEPDIR_BASE/i686
SYSDEPDIR_x86_64=$SYSDEPDIR_BASE/x86_64
AC_SUBST([SRCDIR])
AC_SUBST([TESTSDIR])
AC_SUBST([SYSDEPDIR_i686])
AC_SUBST([SYSDEPDIR_x86_64])
AM_SUBST_NOTMAKE([SRCDIR])
AM_SUBST_NOTMAKE([TESTSDIR])
AM_SUBST_NOTMAKE([SYSDEPDIR_i686])
AM_SUBST_NOTMAKE([SYSDEPDIR_x86_64])

case $host in
  *i686*)   
            ARCH_i686=true;;
  *x86_64*) 
            ARCH_x86_64=true;;
esac
AM_CONDITIONAL([ARCH_i686], [test x$ARCH_i686 = xtrue])
AM_CONDITIONAL([ARCH_x86_64], [test x$ARCH_x86_64 = xtrue])

# Check if we have gcc > 4.4
AC_PREPROC_IFELSE([AC_LANG_SOURCE(
[
#include <features.h>
#if !__GNUC_PREREQ(4,4)
  #error "Looks like I'm going to be building just static libs :("
#endif
])],
[STATIC_ONLY=false],
[STATIC_ONLY=true])
AM_CONDITIONAL([STATIC_ONLY], [test x$STATIC_ONLY = xtrue])

# Allow us to disable __thread TLS for Uthreads
AC_ARG_ENABLE([uthread-tls],
  [AS_HELP_STRING([--disable-uthread-tls],
    [disable __thread tls support for uthreads])],
  [
    if test "x$enable_uthread_tls" = "xno"; then
      AC_DEFINE([NO_UTHREAD_TLS], [1],
                  [Define to 1 if you want to disable support for uthread tls])
    fi
  ],
  []
)

# Check if we have the sphinx documentation tool installed
SPHINX_BUILD=`which sphinx-build`
AM_CONDITIONAL([SPHINX_BUILD], [test x$SPHINX_BUILD != x])

# Check whether LIBRARY exists and contains FUNCT
# Execute ACT-IF-FOUND if it does.  ACT-IF-NOT otherwise.
#AC_CHECK_LIB(LIBRARY, FUNCT, [ACT-IF-FOUND], [ACT-IF-NOT])

echo -n "checking whether RD/WR fsgsbase supported... "
AC_RUN_IFELSE(
  [AC_LANG_PROGRAM([[
      static inline unsigned long rdfsbase(void)
      {
        unsigned long fs;
        asm volatile("rdfsbase %%rax"
            : "=a" (fs)
            :: "memory");
        return fs;
      }
      
      static inline void wrfsbase(unsigned long fs)
      {
        asm volatile("wrfsbase %%rax"
            :: "a" (fs)
            : "memory");
      }
    ]],
    [[ 
      wrfsbase(rdfsbase());
    ]]
  )],
  [
    AC_DEFINE([HAVE_FSGSBASE], [1], [Define to 1 if the current architecture and OS support RD/WR on fsgsbase])
    echo "yes"
  ],
  [echo "no"]
)

# Actually output all declared files
AC_OUTPUT

AUTOMAKE_OPTIONS = subdir-objects

#
#  Makefile.am for the Virtuoso ADBC driver.
#
#  Builds libvirtadbc as a shared library (loadable module) that
#  exports the canonical "AdbcDriverInit" entrypoint. The driver
#  links Virtuoso's existing CLI/RPC convenience archives (libwic +
#  libdk1t + libthrs + libutil) so DatabaseInit can allocate a
#  Virtuoso CLI environment without needing an external ODBC driver
#  manager.
#
#  This file is part of the OpenLink Software Virtuoso Open-Source (VOS)
#  project.
#
#  Copyright (C) 1998-2026 OpenLink Software
#
#  Licensed under the GNU GPL v2; see COPYING in the project root.
#

#
#  The driver is installed as $(libdir)/libvirtadbc.{so,dylib}; that is
#  the location the arrow-adbc driver manager will dlopen given the
#  driver name "virtuoso".
#
lib_LTLIBRARIES = libvirtadbc.la

libvirtadbc_la_SOURCES = \
	adbc_entry.c \
	database.c \
	options.c \
	error.c \
	nanoarrow.c

libvirtadbc_la_CFLAGS  = @VIRT_AM_CFLAGS@
libvirtadbc_la_CFLAGS += -I$(srcdir)/include
libvirtadbc_la_CFLAGS += -I$(top_srcdir)/libsrc
libvirtadbc_la_CFLAGS += -I$(top_srcdir)/libsrc/Dk
libvirtadbc_la_CFLAGS += -I$(top_srcdir)/libsrc/Wi
libvirtadbc_la_CFLAGS += -I$(top_srcdir)/libsrc/odbcsdk/include
libvirtadbc_la_CFLAGS += -I$(top_srcdir)/libsrc/langfunc
libvirtadbc_la_CFLAGS += -I$(top_srcdir)/libsrc/plugin
libvirtadbc_la_CFLAGS += -fvisibility=hidden

#
#  -rpath $(libdir) is required: the project's custom libtool wrapper
#  downgrades to a static convenience archive when -rpath is absent
#  from the link line.
#
libvirtadbc_la_LDFLAGS = -module -avoid-version -rpath '$(libdir)'

libvirtadbc_la_LIBADD = \
	$(top_builddir)/libsrc/Wi/libwic.la \
	$(top_builddir)/libsrc/Dk/libdk1t.la \
	$(top_builddir)/libsrc/Thread/libthrs.la \
	$(top_builddir)/libsrc/util/libutil.la \
	@clntlibs@ @LIBTHR@ @LIBDL@ @ZLIB_LIB@

noinst_HEADERS = \
	virtuoso_adbc.h \
	include/adbc.h \
	include/nanoarrow.h

# ----------------------------------------------------------------------
#  Tests.
#
#  smoke_test       -- dispatch-table validation (asserts which slots
#                      are wired at the current phase).
#  unit_test        -- exercises the option store, the URI parser, and
#                      the AdbcDatabase lifecycle state machine without
#                      needing a live server. The unit test compiles
#                      the relevant translation units directly into
#                      itself because driver internals are hidden from
#                      the shared library's exported ABI.
# ----------------------------------------------------------------------
check_PROGRAMS  = tests/adbc_smoke_test tests/adbc_unit_test
TESTS           = tests/adbc_smoke_test tests/adbc_unit_test

tests_adbc_smoke_test_SOURCES = tests/smoke_test.c
tests_adbc_smoke_test_CFLAGS  = @VIRT_AM_CFLAGS@ -I$(srcdir)/include
tests_adbc_smoke_test_LDADD   = libvirtadbc.la @LIBDL@

tests_adbc_unit_test_SOURCES = \
	tests/unit_test.c \
	options.c \
	error.c \
	database.c \
	adbc_entry.c
tests_adbc_unit_test_CFLAGS  = @VIRT_AM_CFLAGS@
tests_adbc_unit_test_CFLAGS += -I$(srcdir) -I$(srcdir)/include
tests_adbc_unit_test_CFLAGS += -I$(top_srcdir)/libsrc
tests_adbc_unit_test_CFLAGS += -I$(top_srcdir)/libsrc/Dk
tests_adbc_unit_test_CFLAGS += -I$(top_srcdir)/libsrc/Wi
tests_adbc_unit_test_CFLAGS += -I$(top_srcdir)/libsrc/odbcsdk/include
tests_adbc_unit_test_CFLAGS += -I$(top_srcdir)/libsrc/langfunc
tests_adbc_unit_test_CFLAGS += -I$(top_srcdir)/libsrc/plugin
tests_adbc_unit_test_LDADD   = \
	$(top_builddir)/libsrc/Wi/libwic.la \
	$(top_builddir)/libsrc/Dk/libdk1t.la \
	$(top_builddir)/libsrc/Thread/libthrs.la \
	$(top_builddir)/libsrc/util/libutil.la \
	@clntlibs@ @LIBTHR@ @LIBDL@ @ZLIB_LIB@

EXTRA_DIST = \
	README.md \
	include/VENDOR.txt

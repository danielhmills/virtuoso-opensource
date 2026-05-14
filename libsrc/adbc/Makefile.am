AUTOMAKE_OPTIONS = subdir-objects

#
#  Makefile.am for the Virtuoso ADBC driver (phase 1 skeleton).
#
#  Builds libvirtadbc as a shared library that exports the canonical
#  ADBC entrypoint "AdbcDriverInit". At this stage the entrypoint is a
#  stub and no real CLI/RPC linkage happens yet; phases 2+ will link
#  the libwic objects in.
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
#  driver name "virtuoso". noinst_LTLIBRARIES is not usable here because
#  libtool refuses to build a shared form for convenience archives.
#
lib_LTLIBRARIES = libvirtadbc.la

libvirtadbc_la_SOURCES = \
	adbc_entry.c \
	nanoarrow.c

libvirtadbc_la_CFLAGS  = @VIRT_AM_CFLAGS@
libvirtadbc_la_CFLAGS += -I$(srcdir)/include
libvirtadbc_la_CFLAGS += -fvisibility=hidden

#
#  -rpath $(libdir) is required: the project's custom libtool wrapper
#  downgrades to a static convenience archive when -rpath is absent
#  from the link line, even for lib_LTLIBRARIES.
#
libvirtadbc_la_LDFLAGS = -module -avoid-version -rpath '$(libdir)'

noinst_HEADERS = \
	virtuoso_adbc.h \
	include/adbc.h \
	include/nanoarrow.h

# ----------------------------------------------------------------------
#  Phase 1 smoke test: link the driver directly and verify that
#  AdbcDriverInit accepts ADBC_VERSION_1_0_0 / 1.1.0 and rejects
#  NULL drivers and unknown versions.
# ----------------------------------------------------------------------
check_PROGRAMS  = tests/adbc_smoke_test
TESTS           = tests/adbc_smoke_test

tests_adbc_smoke_test_SOURCES = tests/smoke_test.c
tests_adbc_smoke_test_CFLAGS  = @VIRT_AM_CFLAGS@ -I$(srcdir)/include
tests_adbc_smoke_test_LDADD   = libvirtadbc.la @LIBDL@

EXTRA_DIST = \
	README.md \
	include/VENDOR.txt

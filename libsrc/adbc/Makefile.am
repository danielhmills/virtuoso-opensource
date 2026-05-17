AUTOMAKE_OPTIONS = subdir-objects

#
#  This file is part of the OpenLink Software Virtuoso Open-Source (VOS)
#  project.
#
#  Copyright (C) 1998-2026 OpenLink Software
#
#  This project is free software; you can redistribute it and/or modify it
#  under the terms of the GNU General Public License as published by the
#  Free Software Foundation; only version 2 of the License, dated June 1991.
#
#  This program is distributed in the hope that it will be useful, but
#  WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
#  General Public License for more details.
#
#  You should have received a copy of the GNU General Public License along
#  with this program; if not, write to the Free Software Foundation, Inc.,
#  51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
#
#
#
#  Makefile.am for the Virtuoso ADBC driver.
#
#  Builds libvirtadbc as a shared library (loadable module) that
#  exports the canonical "AdbcDriverInit" entrypoint. The driver
#  links Virtuoso's existing CLI/RPC convenience archives (libwic +
#  libdk1t + libthrs + libutil) so it can talk to a Virtuoso server
#  over the same wire protocol that isql and JDBC use.
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
	connection.c \
	statement.c \
	arrow_reader.c \
	arrow_writer.c \
	catalog.c \
	type_map.c \
	options.c \
	error.c \
	program_info.c

libvirtadbc_la_CFLAGS  = @VIRT_AM_CFLAGS@
libvirtadbc_la_CFLAGS += -I$(srcdir)
libvirtadbc_la_CFLAGS += $(ADBC_CFLAGS)
libvirtadbc_la_CFLAGS += $(NANOARROW_CFLAGS)
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
	$(NANOARROW_LIBS) \
	@clntlibs@ @LIBTHR@ @LIBDL@ @ZLIB_LIB@

noinst_HEADERS = \
	virtuoso_adbc.h

# ----------------------------------------------------------------------
#  Tests.
#
#  smoke_test       -- exists since phase 1 (dispatch table validation).
#  unit_test        -- phase 2/3 unit tests: options storage, URI parser,
#                      AdbcDatabase lifecycle, AdbcConnection lifecycle
#                      invariants. Integration paths that actually open
#                      a Virtuoso connection are gated on the env var
#                      VIRT_ADBC_TEST_URI; the test skips them when unset.
# ----------------------------------------------------------------------
check_PROGRAMS  = tests/adbc_smoke_test tests/adbc_unit_test
TESTS           = tests/adbc_smoke_test tests/adbc_unit_test

tests_adbc_smoke_test_SOURCES = tests/smoke_test.c
tests_adbc_smoke_test_CFLAGS  = @VIRT_AM_CFLAGS@ -I$(srcdir)
tests_adbc_smoke_test_CFLAGS += $(ADBC_CFLAGS)
tests_adbc_smoke_test_LDADD   = libvirtadbc.la @LIBDL@

#
#  The unit test exercises internal helpers (virt_opt_*, virt_build_connstr)
#  that are hidden from the shared library's exported ABI. Compile the
#  relevant translation units directly into the test binary so the test
#  can call them without going through dlsym.
#
tests_adbc_unit_test_SOURCES = \
	tests/unit_test.c \
	options.c \
	error.c \
	database.c \
	connection.c \
	statement.c \
	arrow_reader.c \
	arrow_writer.c \
	catalog.c \
	type_map.c \
	program_info.c \
	adbc_entry.c
tests_adbc_unit_test_CFLAGS  = @VIRT_AM_CFLAGS@
tests_adbc_unit_test_CFLAGS += -I$(srcdir)
tests_adbc_unit_test_CFLAGS += $(ADBC_CFLAGS)
tests_adbc_unit_test_CFLAGS += $(NANOARROW_CFLAGS)
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
	$(NANOARROW_LIBS) \
	@clntlibs@ @LIBTHR@ @LIBDL@ @ZLIB_LIB@

EXTRA_DIST = README.md

# ----------------------------------------------------------------------
#  Upstream ADBC validation harness (C++, Google Test).
#
#  Requires: Google Test (libgtest + libgtest_main) and the arrow-adbc
#            validation sources (symlinked in tests/validation/).
#
#  The runner is NOT built by default and not part of "make check"
#  unless GTEST_CFLAGS + GTEST_LIBS are set.  When available:
#
#    GTEST_CFLAGS="$(pkg-config --cflags gtest)"    \
#    GTEST_LIBS="$(pkg-config --libs gtest gtest_main)" \
#    make tests/validation/adbc_validation_runner
#
#  Then run against a Virtuoso server:
#
#    VIRT_ADBC_TEST_URI=virtuoso://dba:dba@localhost:1111 \
#    ./tests/validation/adbc_validation_runner
#
#  Set VIRT_VALIDATION_SERVER_CMD to have the runner start its own
#  server fixture (e.g. VIRT_VALIDATION_SERVER_CMD="virtuoso-t +configfile virt.ini").
# ----------------------------------------------------------------------

# Shared CPPFLAGS for validation sources.
virt_validation_cppflags  = $(GTEST_CFLAGS)
virt_validation_cppflags += -I$(srcdir)/tests/validation/include
virt_validation_cppflags += -I$(srcdir)/tests/validation
virt_validation_cppflags += -I$(srcdir)/include
virt_validation_cppflags += -I$(srcdir)
virt_validation_cppflags += -I$(top_srcdir)/libsrc
virt_validation_cppflags += -I$(top_srcdir)/libsrc/Dk
virt_validation_cppflags += -I$(top_srcdir)/libsrc/Wi
virt_validation_cppflags += -I$(top_srcdir)/libsrc/odbcsdk/include
virt_validation_cppflags += -I$(top_srcdir)/libsrc/langfunc
virt_validation_cppflags += -I$(top_srcdir)/libsrc/plugin
virt_validation_cppflags += -I$(top_builddir)/libsrc
virt_validation_cppflags += -I$(top_srcdir)
virt_validation_cppflags += @VIRT_AM_CFLAGS@

EXTRA_PROGRAMS = tests/validation/adbc_validation_runner

tests_validation_adbc_validation_runner_SOURCES  = \
	tests/validation/validation_runner.cc

nodist_tests_validation_adbc_validation_runner_SOURCES = \
	tests/validation/adbc_validation.cc \
	tests/validation/adbc_validation_connection.cc \
	tests/validation/adbc_validation_database.cc \
	tests/validation/adbc_validation_statement.cc \
	tests/validation/adbc_validation_util.cc \
	tests/validation/common/utils.c

tests_validation_adbc_validation_runner_CPPFLAGS  = $(virt_validation_cppflags)
tests_validation_adbc_validation_runner_LDADD     = \
	libvirtadbc.la \
	$(top_builddir)/libsrc/Wi/libwic.la \
	$(top_builddir)/libsrc/Dk/libdk1t.la \
	$(top_builddir)/libsrc/Thread/libthrs.la \
	$(top_builddir)/libsrc/util/libutil.la \
	$(NANOARROW_LIBS) \
	@clntlibs@ @LIBTHR@ @LIBDL@ @ZLIB_LIB@ \
	$(GTEST_LIBS) -lstdc++

# Hook: if the runner binary exists and GTEST is available, add it to
# the check target.
check-validation:
	@if [ -z "$(GTEST_CFLAGS)$(GTEST_LIBS)" ]; then \
	  echo "[SKIP] validation runner not built (set GTEST_CFLAGS and GTEST_LIBS)"; \
	elif $(MAKE) tests/validation/adbc_validation_runner; then \
	  echo "=== ADBC Validation Runner ==="; \
	  if [ -n "$$VIRT_ADBC_TEST_URI" ]; then \
	    ./tests/validation/adbc_validation_runner $(GTEST_ARGS); \
	  else \
	    echo "[SKIP] Set VIRT_ADBC_TEST_URI to run validation tests"; \
	  fi; \
	else \
	  exit $$?; \
	fi

.PHONY: check-validation

# ----------------------------------------------------------------------
#  pkg-config file
#
#  Generates adbc-driver-virtuoso.pc from the .pc.in template, then
#  installs it into $(libdir)/pkgconfig/ alongside the driver.
# ----------------------------------------------------------------------

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = adbc-driver-virtuoso.pc

# Rule to generate the .pc file by substituting @prefix@ / @libdir@ /
# @PACKAGE_VERSION@ at build time (we cannot use AC_CONFIG_FILES
# because configure.ac lives elsewhere in the parent project).
adbc-driver-virtuoso.pc: $(top_srcdir)/adbc-driver-virtuoso.pc.in
	@$(MKDIR_P) $(@D)
	sed \
	  -e 's|@prefix@|$(prefix)|g' \
	  -e 's|@exec_prefix@|$(exec_prefix)|g' \
	  -e 's|@libdir@|$(libdir)|g' \
	  -e 's|@includedir@|$(includedir)|g' \
	  -e 's|@PACKAGE_VERSION@|$(PACKAGE_VERSION)|g' \
	  $< > $@

CLEANFILES = adbc-driver-virtuoso.pc

# ----------------------------------------------------------------------
#  Python wheel
#
#  Builds a minimal wheel that vendors libvirtadbc.{so,dylib} and
#  registers the entry-point adbc_driver_virtuoso so the pyarrow
#  driver manager can find it.
#
#  Use:  make python-wheel
# ----------------------------------------------------------------------

python-wheel: libvirtadbc.la
	@if [ -d python ]; then \
	  rm -rf python/build python/dist python/adbc_driver_virtuoso.egg-info; \
	  cp -p .libs/libvirtadbc.* python/adbc_driver_virtuoso/ 2>/dev/null || true; \
	  cd python && $(PYTHON) -m build --wheel; \
	else \
	  echo "[SKIP] python/ directory not present"; \
	fi

.PHONY: python-wheel

#!/bin/sh
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
#  vendor_validation.sh
#
#  Creates symlinks from an arrow-adbc checkout into this directory so
#  that the validation harness can be compiled as part of the Virtuoso
#  ADBC driver test suite (Phase 10).
#
#  Usage:
#    cd libsrc/adbc/tests/validation
#    ./vendor_validation.sh /path/to/arrow-adbc
#
#  This script does NOT commit anything — symlinks to external trees
#  should never be checked in.  Run this in your local checkout before
#  building the validation runner.
#
#  This file is part of the OpenLink Software Virtuoso Open-Source (VOS)
#  project.
#
#  Copyright (C) 1998-2026 OpenLink Software
#
#  Licensed under the GNU GPL v2; see COPYING in the project root.
#

set -e

ARROW_ADBC="${1:?Usage: $0 /path/to/arrow-adbc}"

if [ ! -d "$ARROW_ADBC/c/validation" ]; then
    echo "ERROR: $ARROW_ADBC does not appear to be an arrow-adbc checkout" >&2
    exit 1
fi

# Validation harness sources
for f in adbc_validation.h adbc_validation.cc \
         adbc_validation_util.h adbc_validation_util.cc \
         adbc_validation_connection.cc adbc_validation_database.cc \
         adbc_validation_statement.cc; do
    ln -sfn "$ARROW_ADBC/c/validation/$f" "$f"
done

# Shared driver utilities. Keep common/ as a local directory so Automake
# can create common/.deps for dependency tracking.
rm -rf common
mkdir -p common
for f in utils.c utils.h options.h; do
    ln -sfn "$ARROW_ADBC/c/driver/common/$f" "common/$f"
done

# C++ nanoarrow wrapper (not vendored in our tree)
if [ -f "$ARROW_ADBC/c/vendor/nanoarrow/nanoarrow.hpp" ]; then
    ln -sfn "$ARROW_ADBC/c/vendor/nanoarrow/nanoarrow.hpp" \
        include/nanoarrow/nanoarrow.hpp
else
    echo "WARNING: nanoarrow.hpp not found — C++ validation may not compile" >&2
fi

echo "Done. Symlinks created from $ARROW_ADBC"
echo ""
echo "Next steps:"
echo "  GTEST_CFLAGS=\"\$(pkg-config --cflags gtest)\" \\"
echo "  GTEST_LIBS=\"\$(pkg-config --libs gtest gtest_main)\" \\"
echo "  make tests/validation/adbc_validation_runner"

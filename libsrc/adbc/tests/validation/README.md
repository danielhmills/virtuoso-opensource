## ADBC Validation Runner (Phase 10)

Integrates the upstream arrow-adbc validation harness with the Virtuoso ADBC driver.

### Setup (one-time)

```sh
cd libsrc/adbc/tests/validation
./vendor_validation.sh /path/to/arrow-adbc
```

This creates symlinks to the upstream validation sources and shared utilities.
**Do not commit these symlinks** — they are local-only and listed in `.gitignore`.

### Prerequisites

- Google Test + Google Mock (`libgtest-dev`, `libgmock-dev` on Debian; `googletest` on Homebrew)
- A checkout of `apache/arrow-adbc` at a known path
- A running Virtuoso server (or `VIRT_VALIDATION_SERVER_CMD` to start one)

### Building

```sh
GTEST_CFLAGS="$(pkg-config --cflags gtest)" \
GTEST_LIBS="$(pkg-config --libs gtest gtest_main)" \
make tests/validation/adbc_validation_runner
```

Or via the autotools hook (after `autogen.sh && ./configure`):

```sh
GTEST_CFLAGS="..." GTEST_LIBS="..." make check-validation
```

### Running

```sh
VIRT_ADBC_TEST_URI=virtuoso://user:pass@host:port \
./tests/validation/adbc_validation_runner

# Filter to specific suites:
VIRT_ADBC_TEST_URI=... ./tests/validation/adbc_validation_runner \
  --gtest_filter='VirtuosoConnectionTest.*'

# List all registered tests:
VIRT_ADBC_TEST_URI=... ./tests/validation/adbc_validation_runner \
  --gtest_list_tests
```

The test runner loads `libvirtadbc` directly via `AdbcDriverInit` (no driver manager / dlopen needed).

### Files

| File | Purpose |
|------|---------|
| `validation_runner.cc` | Virtuoso-specific `DriverQuirks` + GTest fixtures (committed) |
| `vendor_validation.sh` | Creates local symlinks to arrow-adbc sources (committed) |
| `include/` | Shims mapping `<arrow-adbc/adbc.h>` to our vendored header (committed symlinks) |
| `adbc_validation*.cc/h` | Upstream harness (symlinks, created by vendor script) |
| `common/` | Upstream shared utilities (symlink, created by vendor script) |

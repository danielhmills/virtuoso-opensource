# ADBC Driver for Virtuoso

An [Arrow Database Connectivity (ADBC)](https://arrow.apache.org/adbc/) driver
for OpenLink Virtuoso. Once complete it loads as a peer of the upstream
`adbc_driver_postgresql` / `adbc_driver_sqlite` drivers via the
`arrow-adbc` driver manager.

**Status: phase 1 (skeleton).** The library builds, exports
`AdbcDriverInit`, and the dispatch table validates correctly for ADBC
1.0.0 and 1.1.0 — but no database/connection/statement functions are
implemented yet. See [`../../plan.md`](../../plan.md) for the full
phased plan.

## Layout

```
libsrc/adbc/
├── adbc_entry.c            -- AdbcDriverInit (dispatch table)
├── virtuoso_adbc.h         -- private driver header
├── nanoarrow.c             -- vendored from apache/arrow-adbc
├── include/
│   ├── adbc.h              -- vendored ADBC public ABI
│   ├── nanoarrow.h         -- vendored
│   └── VENDOR.txt          -- vendoring provenance
└── tests/
    └── smoke_test.c        -- phase 1 sanity test
```

## Building

The driver is wired into the standard Virtuoso autotools build:

```sh
./autogen.sh && ./configure && make -C libsrc/adbc
make -C libsrc/adbc check        # runs the smoke test
```

The result is `libsrc/adbc/.libs/libvirtadbc.{so,dylib}` — a libtool
module exporting one public symbol, `AdbcDriverInit`.

## Future phases

Phases 2–12 are sketched in [`../../plan.md`](../../plan.md) and add,
in order: database lifecycle, connection lifecycle + transactions,
read-only statements (the MVP), parameter binding, catalog/metadata,
bulk ingest, RDF/SPARQL extras, ADBC 1.1.0 polish, validation
test harness, packaging, and upstream submission to `apache/arrow-adbc`.

## Vendored sources

`adbc.h`, `nanoarrow.h`, and `nanoarrow.c` are vendored verbatim from
`apache/arrow-adbc`; see [`include/VENDOR.txt`](include/VENDOR.txt) for
the upstream commit and refresh procedure. They are Apache 2.0
licensed; the rest of this directory is GPL-2 in line with the parent
project.

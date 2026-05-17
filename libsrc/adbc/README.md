# ADBC Driver for Virtuoso

An [Arrow Database Connectivity (ADBC)](https://arrow.apache.org/adbc/) driver
for OpenLink Virtuoso. Once complete it loads as a peer of the upstream
`adbc_driver_postgresql` / `adbc_driver_sqlite` drivers via the
`arrow-adbc` driver manager.

**Status.** The driver builds when enabled with `--with-adbc-prefix=...`,
exports `AdbcDriverInit`, and the dispatch table validates correctly for
ADBC 1.0.0 and 1.1.0.

## Layout

```
libsrc/adbc/
├── adbc_entry.c            -- AdbcDriverInit (dispatch table)
├── virtuoso_adbc.h         -- private driver header
└── tests/
    └── smoke_test.c        -- phase 1 sanity test
```

## Building

The driver is optional. Build Virtuoso without it by omitting the
`--with-adbc-prefix` flag:

```sh
./autogen.sh && ./configure && make
```

To build the ADBC driver, install Arrow ADBC and nanoarrow under a
prefix and point configure at it:

```sh
./autogen.sh && ./configure --with-adbc-prefix=/opt/arrow-adbc && make -C libsrc/adbc
make -C libsrc/adbc check        # runs the smoke test
```

The result is `libsrc/adbc/.libs/libvirtadbc.{so,dylib}` — a libtool
module exporting one public symbol, `AdbcDriverInit`.

## Virtuoso-specific options and conventions

| Option key                              | Type   | Effect |
| --------------------------------------- | ------ | ------ |
| `adbc.virtuoso.dialect`                 | string | `sql` (default) or `sparql`. SPARQL mode prepends `sparql ` to the statement text before sending it on the wire — the same convention isql / JDBC / the legacy ODBC driver use. |
| `adbc.virtuoso.fetch.batch_rows`        | int    | Rows pulled per `ArrowArrayStream.get_next` (default 4096). |
| `adbc.virtuoso.charset`                 | string | Server-side character set (passed through to `SQLDriverConnect`). |

### SPARQL result schemas

When a statement is executed with `adbc.virtuoso.dialect=sparql`, the
output `ArrowSchema` carries the following Arrow metadata so
downstream tools can recognise the RDF semantics:

- Top-level schema: `virtuoso:dialect=sparql`
- Each column: `virtuoso:rdf=true`

The columns themselves come back as ordinary `utf8` (or the Virtuoso
SQL-mapped type for projected aggregates) because the driver-manager
layer cannot distinguish DV_IRI_ID, DV_RDF, and DV_GEOMETRY from
DV_STRING through ODBC alone — they all surface as `SQL_VARCHAR`. The
string values therefore carry the standard Virtuoso textual
encodings:

- IRIs: bare IRI text (no angle brackets, no Virtuoso prefix scheme)
- Blank nodes: `_:bN` notation
- Plain literals: the literal value
- Typed literals: `"value"^^<datatype-iri>`
- Language-tagged literals: `"value"@lang`

Exposing each of these as a distinct Arrow logical type (extension
types for IRI_ID, struct projection for RDF literals, WKB binary for
geometry) requires a server-side helper that returns the underlying
DV_* code per column. That work is deferred to a later iteration.

### Example: SPARQL ingest from Python

```python
import adbc_driver_manager.dbapi

with adbc_driver_manager.dbapi.connect(
        driver="/usr/local/lib/libvirtadbc.so",
        db_kwargs={"uri": "virtuoso://dba:dba@localhost:1111"}) as conn:
    cur = conn.cursor()
    cur.adbc_statement.set_options(**{"adbc.virtuoso.dialect": "sparql"})
    cur.execute("select ?s ?p ?o where { ?s ?p ?o } limit 100")
    schema = cur.adbc_get_schema()
    assert schema.metadata.get(b"virtuoso:dialect") == b"sparql"
    table = cur.fetch_arrow_table()
```

## Future phases

Phases 9–12 are sketched in [`../../plan.md`](../../plan.md) and add,
in order: ADBC 1.1.0 polish (ExecuteSchema, statistics, partitions,
cancel), validation test harness, packaging, and upstream submission
to `apache/arrow-adbc`.

## External sources

The driver expects the Arrow ADBC public headers and nanoarrow to come
from the external prefix provided to `configure`.
